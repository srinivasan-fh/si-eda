// SiEDA Core — interactive router: walkaround, push-and-shove, differential pairs, segment drag and length tuning.
//
// Model. A route session takes a snapshot of the board (Base: pads, tracks, vias, rules, a spatial grid) and works on
// an overlay of it (World: base items marked gone, plus items added by the session). Every head position is computed
// from the overlay as it was after the last placed corner, so moving the cursor back springs shoved copper back, and
// cancel() simply drops the overlay. Nothing reaches the PcbLayout before commit().
//
// Shove. The head is laid as fixed copper; anything it is too close to is pushed: a track is taken with the rest of
// its "line" (the chain of segments between pads, vias and junctions) and the line is walked around the octagonal
// hull of the pusher (the pusher inflated by clearance + half widths), choosing the shorter way round that does not
// run into fixed copper; the moved part is then pulled tight with 45° shortcuts. A via is moved to the nearest side
// of the pusher's hull and its tracks follow with 45° joints. Every moved item pushes in turn (breadth first) until
// nothing collides or a fixed item (pad, locked track, the route itself, board edge, hole keep-out, plane layer,
// tamper mesh) is in the way — then the step fails and the head falls back to walking around, or stops short.
//
// Every check is the DRC's: net-pair clearance (design rule, IPC-2221 voltage spacing, isolation barrier), the
// package's own pad gap where a track is still inside its own pad, edge clearance, hole keep-outs and hole spacing.
#include "sieda/InteractiveRouter.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <thread>
#include <tuple>

#include "sieda/Isolation.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/LengthRules.hpp"
#include "sieda/Stackup.hpp"

namespace sieda {
namespace {

constexpr double kTol = 1e-6;       // clearance comparisons
constexpr double kMargin = 1e-7;    // shoved copper keeps this much beyond the exact clearance (rounding)
constexpr double kJoin = 1e-4;      // track ends closer than this are one node
constexpr double kMinEtchGap = 0.1; // DRC: gap inside one footprint where a track is still in its own pad
constexpr double kInvSqrt2 = 0.70710678118654752440;

/// Cancellation of a head computation: set for the duration of one computeHead() on its thread, polled by the long
/// loops (shove queue, grid search, binary searches).
thread_local const std::function<bool()>* t_abort = nullptr;
bool abortRequested() { return t_abort && (*t_abort)(); }
struct AbortScope {
    const std::function<bool()>* saved;
    explicit AbortScope(const std::function<bool()>* f) : saved(t_abort) { t_abort = f; }
    ~AbortScope() { t_abort = saved; }
    AbortScope(const AbortScope&) = delete;
    AbortScope& operator=(const AbortScope&) = delete;
};

/// Runs independent, read-only jobs of one head update side by side (the first on this thread). Each job sees the
/// caller's cancellation plus its own `stop` flag. The jobs' results are then used in the same order as a sequential
/// run would, so the outcome never depends on the timing or on the number of cores.
struct Parallel {
    static bool enabled() {
        // SIEDA_ROUTER_SERIAL=1 runs them one after the other (benchmarks; the results are the same).
        static const unsigned cores = std::thread::hardware_concurrency();
        static const bool allowed = cores > 1 && std::getenv("SIEDA_ROUTER_SERIAL") == nullptr;
        if (!allowed) return false;
        static const bool forced = std::getenv("SIEDA_ROUTER_PARALLEL") != nullptr;  // tests: always side by side
        if (forced) return true;
        // Only with spare cores: on a machine already busy, side-by-side trials just slow each other down.
        thread_local std::chrono::steady_clock::time_point checked{};
        thread_local bool spare = true;
        const auto now = std::chrono::steady_clock::now();
        if (now - checked > std::chrono::milliseconds(250)) {
            double load[1] = {0};
            spare = getloadavg(load, 1) != 1 || load[0] < static_cast<double>(cores) - 1.5;
            checked = now;
        }
        return spare;
    }
    static void run(const std::vector<std::function<void()>>& jobs, std::atomic<bool>* stop = nullptr) {
        if (jobs.empty()) return;
        if (!enabled() || jobs.size() == 1) {
            for (const auto& j : jobs) j();
            return;
        }
        const std::function<bool()>* parent = t_abort;
        const std::function<bool()> check = [parent, stop] {
            return (stop && stop->load(std::memory_order_relaxed)) || (parent && (*parent)());
        };
        std::vector<std::thread> pool;
        pool.reserve(jobs.size() - 1);
        for (size_t k = 1; k < jobs.size(); ++k)
            pool.emplace_back([&, k] {
                AbortScope scope(&check);
                jobs[k]();
            });
        {
            AbortScope scope(&check);
            jobs[0]();
        }
        for (auto& t : pool) t.join();
    }
};

// ------------------------------------------------------------------------------------------------ geometry helpers

double cross(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
Vec2 unit(Vec2 v) {
    const double l = v.length();
    return l > 0 ? v * (1.0 / l) : Vec2{};
}
Vec2 leftNormal(Vec2 d) { return unit(Vec2{-d.y, d.x}); }
bool samePoint(Vec2 a, Vec2 b, double tol = kJoin) { return (a - b).length() <= tol; }

Vec2 closestOnSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    const double t = len2 > 0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return a + ab * t;
}

/// Distance from a point to pad copper (0 inside); round pads are circles. Same as the DRC's.
double padDistance(const Pad& p, Vec2 pt) {
    if (p.round) return std::max(0.0, (pt - p.position).length() - std::min(p.size.x, p.size.y) / 2);
    return pointRectDistance(pt, p.bounds());
}

double padSegmentDistance(const Pad& p, Vec2 a, Vec2 b) {
    if (p.round) return std::max(0.0, pointSegmentDistance(p.position, a, b) - std::min(p.size.x, p.size.y) / 2);
    return segmentRectDistance(a, b, p.bounds());
}

Rect segmentBox(Vec2 a, Vec2 b, double r) { return Rect(a.x, a.y, b.x, b.y).inflated(r); }

double pathLength(const std::vector<Vec2>& p) {
    double l = 0;
    for (size_t i = 1; i < p.size(); ++i) l += (p[i] - p[i - 1]).length();
    return l;
}

/// Drops repeated points, points in the middle of a straight run and spikes (a run that doubles back on itself).
std::vector<Vec2> simplifyPath(const std::vector<Vec2>& p) {
    std::vector<Vec2> out;
    for (Vec2 q : p) {
        if (!out.empty() && samePoint(out.back(), q, 1e-6)) continue;  // sub-micron pieces are rounding
        while (out.size() >= 2) {
            const Vec2 d1 = out.back() - out[out.size() - 2], d2 = q - out.back();
            if (std::fabs(cross(unit(d1), unit(d2))) < 1e-9)
                out.pop_back();
            else
                break;
        }
        if (!out.empty() && samePoint(out.back(), q, 1e-6)) continue;
        out.push_back(q);
    }
    return out;
}

/// First `len` mm of a polyline.
std::vector<Vec2> pathPrefix(const std::vector<Vec2>& p, double len) {
    std::vector<Vec2> out;
    if (p.empty()) return out;
    out.push_back(p[0]);
    for (size_t i = 1; i < p.size() && len > 1e-12; ++i) {
        const double l = (p[i] - p[i - 1]).length();
        if (l <= len) {
            out.push_back(p[i]);
            len -= l;
        } else {
            out.push_back(p[i - 1] + (p[i] - p[i - 1]) * (len / l));
            len = 0;
        }
    }
    return simplifyPath(out);
}

/// A join from direction d1 into d2 makes an acute (< 89°) corner — an acid trap the DRC warns about.
bool acuteJoin(Vec2 d1, Vec2 d2) {
    const double l = d1.length() * d2.length();
    return l > 0 && d1.dot(d2) < -0.0175 * l;
}

/// Connections from a to b in the posture: straight when aligned, otherwise the two bends (preferred first).
std::vector<std::vector<Vec2>> postureLinks(Vec2 a, Vec2 b, RoutePosture posture, bool swap) {
    const Vec2 d = b - a;
    const double ax = std::fabs(d.x), ay = std::fabs(d.y);
    if (d.length() < 1e-9) return {{a}};
    const double tol = 1e-9 * std::max(1.0, ax + ay);
    if (posture == RoutePosture::Free || ax < tol || ay < tol ||
        (posture == RoutePosture::Diagonal45 && std::fabs(ax - ay) < tol))
        return {{a, b}};
    const double sx = d.x > 0 ? 1 : -1, sy = d.y > 0 ? 1 : -1;
    Vec2 straightFirst, otherFirst;
    if (posture == RoutePosture::Diagonal45) {
        const double m = std::min(ax, ay);
        otherFirst = a + Vec2{sx * m, sy * m};     // diagonal, then straight
        straightFirst = b - Vec2{sx * m, sy * m};  // straight, then diagonal
    } else {
        straightFirst = {b.x, a.y};  // horizontal first
        otherFirst = {a.x, b.y};     // vertical first
    }
    std::vector<Vec2> p1{a, straightFirst, b}, p2{a, otherFirst, b};
    if (swap) return {p2, p1};
    return {p1, p2};
}

/// The polyline offset by `d` to its left (miter joins).
std::vector<Vec2> offsetPath(const std::vector<Vec2>& c, double d) {
    std::vector<Vec2> out;
    const size_t n = c.size();
    if (n < 2) return c;
    for (size_t i = 0; i < n; ++i) {
        if (i == 0) {
            out.push_back(c[0] + leftNormal(c[1] - c[0]) * d);
        } else if (i + 1 == n) {
            out.push_back(c[i] + leftNormal(c[i] - c[i - 1]) * d);
        } else {
            const Vec2 n1 = leftNormal(c[i] - c[i - 1]), n2 = leftNormal(c[i + 1] - c[i]);
            const double denom = 1 + n1.dot(n2);
            out.push_back(denom < 0.1 ? c[i] + n1 * d : c[i] + (n1 + n2) * (d / denom));
        }
    }
    return out;
}

/// The polyline with corners replaced by arcs of `radius` drawn as chords of at most 15°. An arc may use up to half
/// of each neighbouring segment (all of an end segment); where that is too short the radius shrinks, and below
/// `minRadius` (or when `clear` refuses the arc) the corner stays sharp. `vertexOut[i]` is where vertex i's
/// replacement starts in the result.
std::vector<Vec2> filletPath(const std::vector<Vec2>& pts, double radius, double minRadius,
                             const std::function<bool(const std::vector<Vec2>&)>& clear, std::vector<size_t>* vertexOut) {
    const size_t n = pts.size();
    std::vector<Vec2> out;
    if (vertexOut) vertexOut->assign(n, 0);
    if (n == 0) return out;
    out.push_back(pts[0]);
    for (size_t i = 1; i + 1 < n; ++i) {
        if (vertexOut) (*vertexOut)[i] = out.size();
        const Vec2 d1 = pts[i] - pts[i - 1], d2 = pts[i + 1] - pts[i];
        const double l1 = d1.length(), l2 = d2.length();
        const Vec2 u1 = l1 > 0 ? d1 * (1 / l1) : Vec2{}, u2 = l2 > 0 ? d2 * (1 / l2) : Vec2{};
        const double turn = std::acos(std::clamp(u1.dot(u2), -1.0, 1.0));  // 0 = straight on
        bool done = false;
        if (l1 > 1e-9 && l2 > 1e-9 && turn > 0.02 && turn < 2.0) {  // up to ~115°: 45° and 90° corners
            const double avail = std::min(i == 1 ? l1 : l1 / 2, i + 2 == n ? l2 : l2 / 2);
            double r = std::min(radius, avail / std::tan(turn / 2));
            for (int attempt = 0; attempt < 2 && !done && r >= minRadius; ++attempt, r /= 2) {
                const double t = r * std::tan(turn / 2);
                const Vec2 a = pts[i] - u1 * t, b = pts[i] + u2 * t;
                const double side = cross(u1, u2) > 0 ? 1 : -1;
                const Vec2 nrm = Vec2{-u1.y, u1.x} * side;  // towards the inside of the turn
                const Vec2 c = a + nrm * r;
                const int steps = std::max(2, static_cast<int>(std::ceil(turn / (kPi / 12))));
                std::vector<Vec2> arc{a};
                const Vec2 ra = a - c;
                for (int k = 1; k < steps; ++k) {
                    const double ang = side * turn * k / steps;
                    const double cs = std::cos(ang), sn = std::sin(ang);
                    arc.push_back(c + Vec2{ra.x * cs - ra.y * sn, ra.x * sn + ra.y * cs});
                }
                arc.push_back(b);
                if (!clear(arc)) continue;
                out.insert(out.end(), arc.begin(), arc.end());
                done = true;
            }
        }
        if (!done) out.push_back(pts[i]);
    }
    if (vertexOut && n >= 1) (*vertexOut)[n - 1] = out.size();
    out.push_back(pts[n - 1]);
    // Only drop repeated points: the chords are nearly collinear and must stay.
    std::vector<Vec2> clean;
    for (Vec2 q : out)
        if (clean.empty() || !samePoint(clean.back(), q, 1e-7)) clean.push_back(q);
    return clean;
}


/// True-arc corner fillets (Track::arc) for one or more runs that belong together (a single track, the members of a
/// differential pair or a bus, or parallel lines being converted). Each run is a polyline drawn with `protos[k]`'s
/// net, layer and width. Corners that every run turns together (the same turn, the runs' corners side by side on one
/// bisector) become concentric arcs: `radius` is the innermost run's radius and the others grow by their offset, so
/// the spacing between the runs stays exact through the turn. Other corners get their own arc of `radius`. An arc
/// may use up to half of each neighbouring segment (all of an end segment); where that is too short the radius
/// shrinks, below `minRadius` the corner stays sharp. `clear(run, arc)` vets each arc (one retry at half the
/// radius). `sharp` lists corners (run, vertex) that must stay sharp. `vertexOut[k][i]` is the index of the first
/// output track of vertex i's replacement in run k.
std::vector<std::vector<Track>> filletArcRunsCore(const std::vector<std::vector<Vec2>>& runs,
                                                  const std::vector<Track>& protos, double radius, double minRadius,
                                                  const std::function<bool(size_t, const Track&)>& clear,
                                                  const std::set<std::pair<size_t, size_t>>& sharp,
                                                  std::vector<std::vector<size_t>>* vertexOut) {
    struct Corner {
        bool arc = false;
        Vec2 a, b, c;
        bool ccw = false;
    };
    struct TurnInfo {
        bool ok = false;
        Vec2 u1, u2;
        double turn = 0, avail = 0;
    };
    const size_t nr = runs.size();
    std::vector<std::vector<Corner>> dec(nr);
    std::vector<std::vector<TurnInfo>> turns(nr);
    for (size_t k = 0; k < nr; ++k) {
        const auto& pts = runs[k];
        const size_t n = pts.size();
        dec[k].assign(n, {});
        turns[k].assign(n, {});
        for (size_t i = 1; i + 1 < n; ++i) {
            const Vec2 d1 = pts[i] - pts[i - 1], d2 = pts[i + 1] - pts[i];
            const double l1 = d1.length(), l2 = d2.length();
            if (l1 <= 1e-9 || l2 <= 1e-9) continue;
            TurnInfo& t = turns[k][i];
            t.u1 = d1 * (1 / l1);
            t.u2 = d2 * (1 / l2);
            t.turn = std::acos(std::clamp(t.u1.dot(t.u2), -1.0, 1.0));
            t.avail = std::min(i == 1 ? l1 : l1 / 2, i + 2 == n ? l2 : l2 / 2);
            t.ok = t.turn > 0.02 && t.turn < 2.0 && !sharp.count({k, i});  // up to ~115°: 45° and 90° corners
        }
    }
    auto arcAt = [&](size_t k, size_t i, double r, Corner& out) {
        const TurnInfo& t = turns[k][i];
        const double tl = r * std::tan(t.turn / 2);
        if (tl > t.avail + 1e-9 || r < minRadius) return false;
        const double side = cross(t.u1, t.u2) > 0 ? 1 : -1;
        out.a = runs[k][i] - t.u1 * tl;
        out.b = runs[k][i] + t.u2 * tl;
        out.c = out.a + Vec2{-t.u1.y, t.u1.x} * (side * r);
        out.ccw = side > 0;
        out.arc = true;
        return true;
    };
    auto arcTrack = [&](size_t k, const Corner& c) {
        const Track& p = protos[k];
        return makeArcTrack(c.a, c.b, c.c, c.ccw, p.net, p.layer, p.width);
    };
    // Corners the runs turn together.
    std::vector<std::vector<char>> grouped(nr);
    for (size_t k = 0; k < nr; ++k) grouped[k].assign(runs[k].size(), 0);
    if (nr > 1) {
        double span = 1.0;
        for (const Track& p : protos) span += 2 * p.width;
        for (size_t k = 1; k < nr; ++k) span += (runs[k].front() - runs[0].front()).length();
        std::vector<size_t> from(nr, 1);
        for (size_t i = 1; i + 1 < runs[0].size(); ++i) {
            const TurnInfo& t0 = turns[0][i];
            if (!t0.ok) continue;
            std::vector<size_t> at(nr, 0);
            at[0] = i;
            bool all = true;
            for (size_t k = 1; k < nr && all; ++k) {
                bool found = false;
                for (size_t j = from[k]; j + 1 < runs[k].size() && !found; ++j) {
                    const TurnInfo& t = turns[k][j];
                    if (t.ok && t.u1.dot(t0.u1) > 1 - 1e-9 && t.u2.dot(t0.u2) > 1 - 1e-9 &&
                        (runs[k][j] - runs[0][i]).length() <= span) {
                        at[k] = j;
                        found = true;
                    }
                }
                all = found;
            }
            if (!all) continue;
            // Concentric: every corner on one bisector line.
            const Vec2 bis = unit(t0.u2 - t0.u1), perp{-bis.y, bis.x};
            bool aligned = bis.length() > 0.5;
            size_t inner = 0;
            for (size_t k = 0; k < nr && aligned; ++k) {
                aligned = std::fabs((runs[k][at[k]] - runs[0][i]).dot(perp)) <= 1e-6;
                if (runs[k][at[k]].dot(bis) > runs[inner][at[inner]].dot(bis)) inner = k;
            }
            if (!aligned) continue;
            const double cosh = std::cos(t0.turn / 2), tanh = std::tan(t0.turn / 2);
            std::vector<double> delta(nr);
            double r = radius;
            for (size_t k = 0; k < nr; ++k) {
                delta[k] = (runs[inner][at[inner]] - runs[k][at[k]]).dot(bis) * cosh;
                r = std::min(r, turns[k][at[k]].avail / tanh - delta[k]);
            }
            for (size_t k = 0; k < nr; ++k) {
                grouped[k][at[k]] = 1;
                from[k] = at[k] + 1;
            }
            for (int attempt = 0; attempt < 2 && r >= minRadius; ++attempt, r /= 2) {
                std::vector<Corner> cs(nr);
                bool ok = true;
                for (size_t k = 0; k < nr && ok; ++k) ok = arcAt(k, at[k], r + delta[k], cs[k]) && clear(k, arcTrack(k, cs[k]));
                if (!ok) continue;
                for (size_t k = 0; k < nr; ++k) dec[k][at[k]] = cs[k];
                break;
            }
        }
    }
    // Every other corner on its own.
    for (size_t k = 0; k < nr; ++k)
        for (size_t i = 1; i + 1 < runs[k].size(); ++i) {
            if (grouped[k][i] || !turns[k][i].ok) continue;
            double r = std::min(radius, turns[k][i].avail / std::tan(turns[k][i].turn / 2));
            for (int attempt = 0; attempt < 2 && r >= minRadius; ++attempt, r /= 2) {
                Corner c;
                if (!arcAt(k, i, r, c) || !clear(k, arcTrack(k, c))) continue;
                dec[k][i] = c;
                break;
            }
        }
    // Emit: straight pieces between the arcs.
    std::vector<std::vector<Track>> out(nr);
    if (vertexOut) vertexOut->assign(nr, {});
    for (size_t k = 0; k < nr; ++k) {
        const auto& pts = runs[k];
        if (vertexOut) (*vertexOut)[k].assign(pts.size(), 0);
        if (pts.empty()) continue;
        Vec2 cur = pts[0];
        auto straight = [&](Vec2 to) {
            if ((to - cur).length() <= 1e-9) return;
            Track t = protos[k];
            t.arc = false;
            t.a = cur;
            t.b = to;
            out[k].push_back(t);
        };
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const Corner& c = dec[k][i];
            straight(c.arc ? c.a : pts[i]);
            if (vertexOut) (*vertexOut)[k][i] = out[k].size();
            if (c.arc) {
                out[k].push_back(arcTrack(k, c));
                cur = c.b;
            } else {
                cur = pts[i];
            }
        }
        straight(pts.back());
        if (vertexOut) (*vertexOut)[k][pts.size() - 1] = out[k].size();
    }
    return out;
}

/// filletArcRunsCore on the runs with their straight-through points dropped (a corner placed in the middle of a
/// straight line, or the short collinear piece a pair's outer member gets at a placed corner, must not limit the
/// arc next to it). `sharp` and `vertexOut` use the original vertex indices.
std::vector<std::vector<Track>> filletArcRuns(const std::vector<std::vector<Vec2>>& runs, const std::vector<Track>& protos,
                                              double radius, double minRadius,
                                              const std::function<bool(size_t, const Track&)>& clear,
                                              const std::set<std::pair<size_t, size_t>>& sharp,
                                              std::vector<std::vector<size_t>>* vertexOut) {
    const size_t nr = runs.size();
    std::vector<std::vector<Vec2>> simple(nr);
    std::vector<std::vector<size_t>> map(nr);
    for (size_t k = 0; k < nr; ++k) {
        const auto& p = runs[k];
        const size_t n = p.size();
        map[k].assign(n, 0);
        std::vector<size_t> pending;
        for (size_t i = 0; i < n; ++i) {
            bool drop = false;
            if (i > 0 && !simple[k].empty()) {
                const Vec2 d1 = p[i] - simple[k].back();
                if (d1.length() <= 1e-9) {
                    drop = i + 1 < n;  // a repeated point (the last one is kept: the run must end where it ends)
                } else if (i + 1 < n) {
                    const Vec2 d2 = p[i + 1] - p[i];
                    drop = d2.length() <= 1e-9 || (std::fabs(cross(unit(d1), unit(d2))) <= 1e-9 && d1.dot(d2) > 0);
                }
            }
            if (drop) {
                pending.push_back(i);
                continue;
            }
            simple[k].push_back(p[i]);
            map[k][i] = simple[k].size() - 1;
            for (size_t q : pending) map[k][q] = simple[k].size() - 1;
            pending.clear();
        }
        for (size_t q : pending) map[k][q] = simple[k].empty() ? 0 : simple[k].size() - 1;
    }
    std::set<std::pair<size_t, size_t>> sharpSimple;
    for (const auto& [k, i] : sharp)
        if (k < nr && i < map[k].size()) sharpSimple.insert({k, map[k][i]});
    std::vector<std::vector<size_t>> vo;
    auto out = filletArcRunsCore(simple, protos, radius, minRadius, clear, sharpSimple, &vo);
    if (vertexOut) {
        vertexOut->assign(nr, {});
        for (size_t k = 0; k < nr; ++k) {
            (*vertexOut)[k].assign(runs[k].size(), 0);
            for (size_t i = 0; i < runs[k].size(); ++i)
                (*vertexOut)[k][i] = map[k][i] < vo[k].size() ? vo[k][map[k][i]] : out[k].size();
        }
    }
    return out;
}

/// filletArcRuns, then every arc checked against the other runs' final copper (`clearance(netA, netB)`): a corner
/// whose arc comes too close stays sharp and the runs are filleted again without it (all sharp after 7 rounds).
std::vector<std::vector<Track>> filletArcRunsChecked(const std::vector<std::vector<Vec2>>& pts,
                                                     const std::vector<Track>& protos, double radius, double minRadius,
                                                     const std::function<bool(size_t, const Track&)>& clear,
                                                     const std::function<double(int, int)>& clearance,
                                                     std::vector<std::vector<size_t>>* vertexOut) {
    std::vector<std::vector<Track>> res;
    std::vector<std::vector<size_t>> vos;
    std::set<std::pair<size_t, size_t>> sharp;
    for (int attempt = 0; attempt < 8; ++attempt) {
        res = filletArcRuns(pts, protos, radius, minRadius, clear, sharp, &vos);
        const size_t before = sharp.size();
        for (size_t gk = 0; gk < res.size(); ++gk)
            for (size_t ti = 0; ti < res[gk].size(); ++ti) {
                const Track& a = res[gk][ti];
                if (!a.arc) continue;
                bool bad = false;
                for (size_t gk2 = 0; gk2 < res.size() && !bad; ++gk2) {
                    if (gk2 == gk) continue;
                    for (const Track& o : res[gk2])
                        if (o.layer == a.layer && o.net != a.net &&
                            trackTrackDistance(a, o) - (a.width + o.width) / 2 < clearance(a.net, o.net) - kTol) {
                            bad = true;
                            break;
                        }
                }
                if (!bad) continue;
                for (size_t v = 0; v < vos[gk].size(); ++v)
                    if (vos[gk][v] == ti) sharp.insert({gk, v});
            }
        if (sharp.size() == before) break;
        if (attempt == 6)
            for (size_t gk = 0; gk < pts.size(); ++gk)
                for (size_t v = 0; v < pts[gk].size(); ++v) sharp.insert({gk, v});
    }
    if (vertexOut) *vertexOut = vos;
    return res;
}

// --------------------------------------------------------------------------------------------- octagonal hulls

const Vec2 kDirs[8] = {{1, 0},  {kInvSqrt2, kInvSqrt2},   {0, 1},  {-kInvSqrt2, kInvSqrt2},
                       {-1, 0}, {-kInvSqrt2, -kInvSqrt2}, {0, -1}, {kInvSqrt2, -kInvSqrt2}};

/// Convex octagon {p : kDirs[k]·p ≤ h[k]}; v[k] is the corner between edges k and k+1 (edge k runs from v[k-1]).
struct Octagon {
    double h[8] = {};
    Vec2 v[8];
    bool inside(Vec2 p, double tol = 1e-7) const {
        for (int k = 0; k < 8; ++k)
            if (kDirs[k].dot(p) >= h[k] - tol) return false;
        return true;
    }
};

/// The octagon around a convex set (points + radius) grown by `extra`: every boundary point is at least
/// radius + extra from the set.
Octagon makeOctagon(const std::vector<Vec2>& pts, double grow) {
    Octagon o;
    for (int k = 0; k < 8; ++k) {
        double m = -std::numeric_limits<double>::max();
        for (Vec2 p : pts) m = std::max(m, kDirs[k].dot(p));
        o.h[k] = m + grow;
    }
    for (int k = 0; k < 8; ++k) {
        const int k2 = (k + 1) % 8;
        const double a1 = kDirs[k].x, b1 = kDirs[k].y, a2 = kDirs[k2].x, b2 = kDirs[k2].y;
        const double det = a1 * b2 - a2 * b1;
        o.v[k] = {(o.h[k] * b2 - o.h[k2] * b1) / det, (a1 * o.h[k2] - a2 * o.h[k]) / det};
    }
    return o;
}

struct Clip {
    double t0 = 0, t1 = 1;
    int k0 = -1, k1 = -1;  // edge through which the segment enters / leaves (-1: that end is inside)
};

/// Part of segment A-B strictly inside the octagon (Cyrus–Beck). False when it does not enter it.
bool clipSegment(const Octagon& o, Vec2 A, Vec2 B, Clip& c) {
    const double tol = 1e-7;
    const Vec2 D = B - A;
    c = Clip{};
    for (int k = 0; k < 8; ++k) {
        const double num = o.h[k] - tol - kDirs[k].dot(A);
        const double den = kDirs[k].dot(D);
        if (std::fabs(den) < 1e-12) {
            if (num <= 0) return false;
            continue;
        }
        const double t = num / den;
        if (den < 0) {
            if (t > c.t0) {
                c.t0 = t;
                c.k0 = k;
            }
        } else if (t < c.t1) {
            c.t1 = t;
            c.k1 = k;
        }
    }
    return (c.t1 - c.t0) * D.length() > 1e-7;
}

/// The polyline with its part inside the octagon replaced by the octagon's boundary, going counter-clockwise (in
/// the order of kDirs) or clockwise. False when an end of the polyline is inside the octagon.
bool walkAround(const std::vector<Vec2>& pts, const Octagon& o, bool ccw, std::vector<Vec2>& out) {
    const int n = static_cast<int>(pts.size());
    int first = -1, last = -1;
    Clip cf, cl, c;
    for (int i = 0; i + 1 < n; ++i)
        if (clipSegment(o, pts[static_cast<size_t>(i)], pts[static_cast<size_t>(i) + 1], c)) {
            first = i;
            cf = c;
            break;
        }
    if (first < 0) {
        out = pts;
        return true;
    }
    for (int i = n - 2; i >= first; --i)
        if (clipSegment(o, pts[static_cast<size_t>(i)], pts[static_cast<size_t>(i) + 1], c)) {
            last = i;
            cl = c;
            break;
        }
    if (cf.k0 < 0 || cl.k1 < 0) return false;
    const Vec2 a0 = pts[static_cast<size_t>(first)], a1 = pts[static_cast<size_t>(first) + 1];
    const Vec2 b0 = pts[static_cast<size_t>(last)], b1 = pts[static_cast<size_t>(last) + 1];
    const Vec2 E = a0 + (a1 - a0) * cf.t0, X = b0 + (b1 - b0) * cl.t1;
    const int kin = cf.k0, kout = cl.k1;
    auto along = [&](int k, Vec2 p) { return Vec2{-kDirs[k].y, kDirs[k].x}.dot(p); };
    out.assign(pts.begin(), pts.begin() + first + 1);
    out.push_back(E);
    if (ccw) {
        if (!(kin == kout && along(kin, X) >= along(kin, E) - 1e-12)) {
            int k = kin;
            do {
                out.push_back(o.v[k]);
                k = (k + 1) % 8;
            } while (k != kout);
        }
    } else {
        if (!(kin == kout && along(kin, X) <= along(kin, E) + 1e-12)) {
            int k = kin;
            do {
                k = (k + 7) % 8;
                out.push_back(o.v[k]);
            } while (k != kout);
        }
    }
    out.push_back(X);
    out.insert(out.end(), pts.begin() + last + 1, pts.end());
    out = simplifyPath(out);
    return true;
}

/// A copper shape as the hulls see it: a point, a segment or a box, plus a radius.
struct Shape {
    enum Type { Point, Segment, Box, Arc } type = Point;
    Vec2 a, b;
    Rect box;
    double radius = 0;
    Track arc;  // Arc: the arc track; its hull points are chords, so `radius` includes their sagitta
    static constexpr double kArcSag = 0.005;
    std::vector<Vec2> points() const {
        if (type == Point) return {a};
        if (type == Segment) return {a, b};
        if (type == Arc) return trackPolyline(arc, kArcSag);
        return {{box.x0, box.y0}, {box.x1, box.y0}, {box.x1, box.y1}, {box.x0, box.y1}};
    }
    /// Edge distance from this shape to segment p-q.
    double distanceTo(Vec2 p, Vec2 q) const {
        if (type == Point) return pointSegmentDistance(a, p, q) - radius;
        if (type == Segment) return segmentSegmentDistance(a, b, p, q) - radius;
        if (type == Arc) return trackSegmentDistance(arc, p, q) - (radius - kArcSag);
        return segmentRectDistance(p, q, box) - radius;
    }
};

Shape trackShape(const Track& t) {
    Shape s;
    if (isArcTrack(t)) {
        s.type = Shape::Arc;
        s.arc = t;
        s.a = t.a;
        s.b = t.b;
        s.radius = t.width / 2 + Shape::kArcSag;
        return s;
    }
    s.type = Shape::Segment;
    s.a = t.a;
    s.b = t.b;
    s.radius = t.width / 2;
    return s;
}
Shape viaShape(const Via& v) {
    Shape s;
    s.a = v.position;
    s.radius = v.diameter / 2;
    return s;
}
Shape padShape(const Pad& p) {
    Shape s;
    if (p.round) {
        s.a = p.position;
        s.radius = std::min(p.size.x, p.size.y) / 2;
    } else {
        s.type = Shape::Box;
        s.box = p.bounds();
    }
    return s;
}

// ------------------------------------------------------------------------------------------- board snapshot (Base)

struct Grid {
    std::vector<std::vector<size_t>> cells;
};

struct Base {
    BoardSettings s;
    std::vector<Pad> pads;
    std::vector<Track> tracks;
    std::vector<Via> vias;
    std::vector<char> trackFixed;  // locked, or a tamper-mesh stripe
    std::vector<char> arcMovable;  // an arc fixed only by its shape: the shove engine may move it as a whole
    std::vector<char> viaFixed;    // inside a pad of its net (moving it would leave the pad), or a mesh via
    std::map<int, std::vector<size_t>> compPads;
    std::map<int, std::string> refs;
    std::vector<std::string> netNames;
    std::vector<size_t> pinCount;
    std::map<int, std::pair<double, double>> ranges;
    std::vector<const std::pair<double, double>*> rangeOf;  // ranges by net index (fast lookup), null = unknown
    std::vector<int> domain;  // isolation domain per net (empty: no barrier rule)
    std::set<int> barrierComps;
    std::vector<int> planeNet;  // per copper layer: the net whose plane it is, -1 = none
    struct Mesh {
        Rect region;
        int netA = -1, netB = -1, layerA = -1, layerB = -1;
    };
    std::vector<Mesh> meshes;
    double maxClearance = 0;

    Rect area;
    double cell = 2;
    int cols = 1, rows = 1;
    Grid padGrid, trackGrid, viaGrid;
    std::vector<Grid> layerTrackGrid;  // the tracks of each layer (the clearance checks look at one layer)

    Base(const Base&) = delete;  // rangeOf points into ranges
    Base& operator=(const Base&) = delete;
    Base(const PcbLayout& pcb, const Schematic& sch) {
        s = pcb.settings;
        pads = pcb.pads(sch);
        tracks = pcb.tracks;
        vias = pcb.vias;
        for (size_t i = 0; i < pads.size(); ++i) compPads[pads[i].componentId].push_back(i);
        for (const auto& c : sch.components()) refs[c.id] = c.ref;
        for (const auto& n : sch.nets()) {
            netNames.push_back(n.name);
            pinCount.push_back(n.pins.size());
        }
        ranges = netVoltageRanges(sch);
        for (const auto& r : ranges)
            if (r.first >= 0) {
                if (static_cast<size_t>(r.first) >= rangeOf.size()) rangeOf.resize(static_cast<size_t>(r.first) + 1, nullptr);
                rangeOf[static_cast<size_t>(r.first)] = &r.second;
            }
        if (s.isolationGap > 0) {
            domain = galvanicDomains(sch).netDomain;
            for (const auto& c : sch.components())
                if (isIsolationBarrier(c)) barrierComps.insert(c.id);
        }
        planeNet.assign(static_cast<size_t>(std::max(1, s.layerCount)), -1);
        for (const auto& z : pcb.zones)
            if (z.plane && z.layer >= 0 && z.layer < s.layerCount) planeNet[static_cast<size_t>(z.layer)] = netIndex(z.net);
        std::set<int> meshNets;
        if (!pcb.tamperMeshes.empty())
            for (const auto& g : pcb.tamperMeshGeometry(sch, pads)) {
                if (!g.error.empty() || g.mesh < 0) continue;
                const TamperMesh& tm = pcb.tamperMeshes[static_cast<size_t>(g.mesh)];
                meshes.push_back({g.region, g.netA, g.netB, tm.layerA, tm.layerB});
                meshNets.insert(g.netA);
                meshNets.insert(g.netB);
            }
        trackFixed.resize(tracks.size());
        arcMovable.resize(tracks.size());
        // Arcs are fixed for the line machinery (lines, drags, gloss move straight segments); the shove engine moves
        // an arc that only its shape holds as a whole (Shover::shoveArc). Locked arcs, teardrops and mesh stripes stay.
        for (size_t i = 0; i < tracks.size(); ++i) {
            trackFixed[i] = tracks[i].locked || meshNets.count(tracks[i].net) || tracks[i].arc || tracks[i].teardrop;
            arcMovable[i] = tracks[i].arc && !tracks[i].locked && !meshNets.count(tracks[i].net) && !tracks[i].teardrop &&
                            isArcTrack(tracks[i]);
        }
        viaFixed.resize(vias.size());
        for (size_t i = 0; i < vias.size(); ++i) {
            bool inPad = false;
            for (const auto& p : pads)
                if (p.net == vias[i].net && (p.throughHole || vias[i].spans(p.smdLayer)) &&
                    padDistance(p, vias[i].position) <= 0)
                    inPad = true;
            viaFixed[i] = inPad || meshNets.count(vias[i].net);
        }
        maxClearance = std::max(s.clearance, s.isolationGap);
        if (!ranges.empty()) {
            double lo = 0, hi = 0;
            for (const auto& r : ranges) {
                lo = std::min(lo, r.second.first);
                hi = std::max(hi, r.second.second);
            }
            maxClearance = std::max(maxClearance, ipc2221Clearance(hi - lo, s.highAltitude, s.coated()));
        }
        maxClearance = std::max(maxClearance, kMinEtchGap);
        for (const auto& nc : s.netClearances) maxClearance = std::max(maxClearance, nc.second);  // net classes

        area = Rect(0, 0, std::max(1.0, s.width), std::max(1.0, s.height)).inflated(20);
        cell = std::max(1.0, std::max(area.width(), area.height()) / 160);
        cols = std::max(1, static_cast<int>(std::ceil(area.width() / cell)));
        rows = std::max(1, static_cast<int>(std::ceil(area.height() / cell)));
        for (Grid* g : {&padGrid, &trackGrid, &viaGrid}) g->cells.assign(static_cast<size_t>(cols * rows), {});
        for (size_t i = 0; i < pads.size(); ++i) insert(padGrid, pads[i].bounds(), i);
        for (size_t i = 0; i < tracks.size(); ++i)
            insert(trackGrid, trackBox(tracks[i], tracks[i].width / 2), i);
        for (size_t i = 0; i < vias.size(); ++i)
            insert(viaGrid, Rect::centered(vias[i].position, vias[i].diameter, vias[i].diameter), i);
        int layers = std::max(1, s.layerCount);
        for (const auto& t : tracks) layers = std::max(layers, t.layer + 1);
        layerTrackGrid.assign(static_cast<size_t>(layers), Grid{});
        for (Grid& g : layerTrackGrid) {
            g.cells.assign(static_cast<size_t>(cols * rows), {});
        }
        for (size_t i = 0; i < tracks.size(); ++i)
            if (tracks[i].layer >= 0)
                insert(layerTrackGrid[static_cast<size_t>(tracks[i].layer)], trackBox(tracks[i], tracks[i].width / 2), i);
    }

    int netIndex(const std::string& name) const {
        for (size_t i = 0; i < netNames.size(); ++i)
            if (netNames[i] == name) return static_cast<int>(i);
        return -1;
    }
    std::string netName(int n) const {
        return n >= 0 && n < static_cast<int>(netNames.size()) ? netNames[static_cast<size_t>(n)] : std::string("(no net)");
    }

    void cellRange(const Rect& r, int& i0, int& i1, int& j0, int& j1) const {
        auto ci = [&](double x) { return std::clamp(static_cast<int>(std::floor((x - area.x0) / cell)), 0, cols - 1); };
        auto cj = [&](double y) { return std::clamp(static_cast<int>(std::floor((y - area.y0) / cell)), 0, rows - 1); };
        i0 = ci(r.x0);
        i1 = ci(r.x1);
        j0 = cj(r.y0);
        j1 = cj(r.y1);
    }
    void insert(Grid& g, const Rect& r, size_t idx) {
        int i0, i1, j0, j1;
        cellRange(r, i0, i1, j0, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) g.cells[static_cast<size_t>(j * cols + i)].push_back(idx);
    }
    /// The items whose cells `r` touches, ascending, each once. Read-only: the de-duplication marks are per
    /// thread, so several head searches may query one snapshot at once.
    void query(const Grid& g, const Rect& r, std::vector<size_t>& out) const {
        out.clear();
        int i0, i1, j0, j1;
        cellRange(r, i0, i1, j0, j1);
        if (i0 == i1 && j0 == j1) {  // one cell: ascending already, no repeats
            const auto& c = g.cells[static_cast<size_t>(j0 * cols + i0)];
            out.assign(c.begin(), c.end());
            return;
        }
        thread_local std::vector<unsigned> stamp;
        thread_local unsigned epoch = 0;
        if (++epoch == 0) {
            std::fill(stamp.begin(), stamp.end(), 0u);
            epoch = 1;
        }
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (size_t idx : g.cells[static_cast<size_t>(j * cols + i)]) {
                    if (idx >= stamp.size()) stamp.resize(std::max(idx + 1, 2 * stamp.size()), 0u);
                    if (stamp[idx] != epoch) {
                        stamp[idx] = epoch;
                        out.push_back(idx);
                    }
                }
        std::sort(out.begin(), out.end());
    }

    /// IPC-2221 spacing for the two nets' potential difference (0 when unknown) — the DRC's voltage rule.
    double voltageClearance(int a, int b) const {
        if (a < 0 || b < 0 || a == b) return 0;
        const size_t ua = static_cast<size_t>(a), ub = static_cast<size_t>(b);
        const std::pair<double, double>* ra = ua < rangeOf.size() ? rangeOf[ua] : nullptr;
        const std::pair<double, double>* rb = ub < rangeOf.size() ? rangeOf[ub] : nullptr;
        if (!ra || !rb) return 0;
        if (pinCount[ua] < 2 || pinCount[ub] < 2) return 0;
        const double dv = std::max(std::fabs(ra->second - rb->first), std::fabs(rb->second - ra->first));
        const double need = ipc2221Clearance(dv, s.highAltitude, s.coated());
        return need > s.clearance + 1e-3 ? need : 0;
    }
    /// Copper-to-copper clearance between two nets: the design rule, the voltage spacing and the isolation gap.
    double clearance(int a, int b, bool barrierPad = false) const {
        if (rangeOf.empty() && domain.empty() && s.netClearances.empty())
            return std::max(s.clearance, 0.0);  // no voltage, isolation or net-class rules
        double c = std::max(s.clearance, voltageClearance(a, b));
        if (!s.netClearances.empty()) c = std::max({c, s.clearanceFor(netName(a)), s.clearanceFor(netName(b))});  // net classes
        if (!barrierPad && !domain.empty() && a >= 0 && b >= 0 && a != b) {
            const int da = a < static_cast<int>(domain.size()) ? domain[static_cast<size_t>(a)] : -1;
            const int db = b < static_cast<int>(domain.size()) ? domain[static_cast<size_t>(b)] : -1;
            if (da >= 0 && db >= 0 && da != db) c = std::max(c, s.isolationGap);
        }
        return c;
    }
};

// ------------------------------------------------------------------------------------------- overlay (World)

/// Removed flags of a World's items (base, then added), plus the added items still alive in ascending order, so the
/// clearance queries skip the many tracks a shove step adds and drops again.
struct GoneFlags {
    std::vector<char> f;
    size_t base = 0;
    std::vector<uint32_t> live;  // alive added items: index - base, ascending

    struct Ref {
        GoneFlags* g;
        size_t i;
        operator char() const { return g->f[i]; }
        Ref& operator=(char c) {
            g->set(i, c);
            return *this;
        }
        Ref& operator=(const Ref& o) { return *this = static_cast<char>(o); }
    };
    GoneFlags() = default;
    GoneFlags(size_t n, char c) : f(n, c), base(n) {}
    char operator[](size_t i) const { return f[i]; }
    Ref operator[](size_t i) { return {this, i}; }
    size_t size() const { return f.size(); }
    void push_back(char c) {
        f.push_back(c);
        if (!c) live.push_back(static_cast<uint32_t>(f.size() - 1 - base));
    }
    void set(size_t i, char c) {
        if ((f[i] != 0) == (c != 0)) {
            f[i] = c;
            return;
        }
        f[i] = c;
        if (i < base) return;
        const auto k = static_cast<uint32_t>(i - base);
        const auto at = std::lower_bound(live.begin(), live.end(), k);
        if (c)
            live.erase(at);
        else
            live.insert(at, k);
    }
};

struct World {
    const Base* b = nullptr;
    GoneFlags goneT, goneV;  // base + added
    std::vector<Track> addT;
    std::vector<Via> addV;
    std::vector<Rect> addTBox, addVBox;  // their copper extents (the overlay only grows)
    std::vector<char> fixAddT, fixAddV;  // added copper of the route itself is fixed
    std::vector<int> fixedNets;          // nets being routed: their copper is never shoved

    World() = default;
    explicit World(const Base* base) : b(base), goneT(base->tracks.size(), 0), goneV(base->vias.size(), 0) {}

    size_t baseT() const { return b->tracks.size(); }
    size_t baseV() const { return b->vias.size(); }
    size_t nT() const { return baseT() + addT.size(); }
    size_t nV() const { return baseV() + addV.size(); }
    const Track& track(size_t i) const { return i < baseT() ? b->tracks[i] : addT[i - baseT()]; }
    const Via& via(size_t i) const { return i < baseV() ? b->vias[i] : addV[i - baseV()]; }
    bool aliveT(size_t i) const { return !goneT[i]; }
    bool aliveV(size_t i) const { return !goneV[i]; }
    bool netFixed(int n) const { return std::find(fixedNets.begin(), fixedNets.end(), n) != fixedNets.end(); }
    bool trackFixed(size_t i) const {
        if (netFixed(track(i).net)) return true;
        // An added arc (a shoved or re-filleted one) is held like a base arc: lines end at it.
        return i < baseT() ? b->trackFixed[i] != 0 : (fixAddT[i - baseT()] != 0 || addT[i - baseT()].arc);
    }
    /// An arc only its shape holds (not locked, a teardrop, a mesh stripe or the route itself): trackFixed for the
    /// line machinery, but the shove engine moves it as a whole (Shover::shoveArc).
    bool arcShovable(size_t i) const {
        const Track& t = track(i);
        if (!t.arc || t.teardrop || netFixed(t.net)) return false;
        return i < baseT() ? b->arcMovable[i] != 0 : (fixAddT[i - baseT()] == 0 && isArcTrack(t));
    }
    bool viaFixed(size_t i) const {
        if (netFixed(via(i).net)) return true;
        return i < baseV() ? b->viaFixed[i] != 0 : fixAddV[i - baseV()] != 0;
    }
    size_t addTrack(Track t, bool fixed) {
        t.id = -1;
        t.locked = false;
        addT.push_back(t);
        addTBox.push_back(trackBox(t, t.width / 2));
        fixAddT.push_back(fixed);
        goneT.push_back(0);
        return nT() - 1;
    }
    size_t addVia(Via v, bool fixed) {
        v.id = -1;
        addV.push_back(v);
        addVBox.push_back(Rect::centered(v.position, v.diameter, v.diameter));
        fixAddV.push_back(fixed);
        goneV.push_back(0);
        return nV() - 1;
    }
    void tracksIn(const Rect& r, std::vector<size_t>& out) const {
        b->query(b->trackGrid, r, out);
        out.erase(std::remove_if(out.begin(), out.end(), [&](size_t i) { return !aliveT(i); }), out.end());
        for (uint32_t k : goneT.live)
            if (addTBox[k].intersects(r)) out.push_back(baseT() + k);
    }
    /// tracksIn, only the tracks on `layer` (the same order).
    void tracksOnLayer(const Rect& r, int layer, std::vector<size_t>& out) const {
        if (layer < 0 || layer >= static_cast<int>(b->layerTrackGrid.size())) {
            tracksIn(r, out);
            out.erase(std::remove_if(out.begin(), out.end(), [&](size_t i) { return track(i).layer != layer; }), out.end());
            return;
        }
        b->query(b->layerTrackGrid[static_cast<size_t>(layer)], r, out);
        out.erase(std::remove_if(out.begin(), out.end(), [&](size_t i) { return !aliveT(i); }), out.end());
        for (uint32_t k : goneT.live)
            if (addT[k].layer == layer && addTBox[k].intersects(r)) out.push_back(baseT() + k);
    }
    void viasIn(const Rect& r, std::vector<size_t>& out) const {
        b->query(b->viaGrid, r, out);
        out.erase(std::remove_if(out.begin(), out.end(), [&](size_t i) { return !aliveV(i); }), out.end());
        for (uint32_t k : goneV.live)
            if (addVBox[k].intersects(r)) out.push_back(baseV() + k);
    }
    void padsIn(const Rect& r, std::vector<size_t>& out) const { b->query(b->padGrid, r, out); }
};

/// One trial of a bisection: the value tried, the overlay it produced and whether it fits.
struct Probe {
    double at = 0;
    World w;
    bool ok = false;
    std::string why;
};

/// Bisection for the furthest value that fits, exactly as `while (it < steps && wide(lo, hi)) { mid; eval; take }`
/// runs it, but two steps at a time: the middle and both quarter points are tried side by side, then taken in the
/// sequential order (the same points, the same result, whatever the timing).
template <class Wide, class Eval, class Take>
void bisect(double& lo, double& hi, int steps, const Wide& wide, const Eval& eval, const Take& take) {
    int it = 0;
    while (it < steps && wide(lo, hi) && !abortRequested()) {
        Probe mid, low, high;
        mid.at = (lo + hi) / 2;
        low.at = (lo + mid.at) / 2;
        high.at = (mid.at + hi) / 2;
        const bool second = Parallel::enabled() && it + 1 < steps && wide(lo, mid.at) && wide(mid.at, hi);
        if (second)
            Parallel::run({[&] { eval(mid); }, [&] { eval(low); }, [&] { eval(high); }});
        else
            eval(mid);
        const bool ok = mid.ok;
        (ok ? lo : hi) = mid.at;
        take(mid);
        ++it;
        if (!second || !(it < steps && wide(lo, hi) && !abortRequested())) continue;
        Probe& next = ok ? high : low;
        (next.ok ? lo : hi) = next.at;
        take(next);
        ++it;
    }
}

// ------------------------------------------------------------------------------------------------ clearance checks

enum class HitKind { Pad, Track, Via, Edge, Hole, Plane, Mesh };
struct Hit {
    HitKind kind = HitKind::Pad;
    size_t index = 0;
    bool fixed = true;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
    bool operator<(const Hit& o) const { return std::tie(kind, index) < std::tie(o.kind, o.index); }
};

inline bool inNets(const std::vector<int>& nets, int n) { return n >= 0 && std::find(nets.begin(), nets.end(), n) != nets.end(); }

double clearanceTo(const Base& B, const std::vector<int>& nets, int other, bool barrierPad = false) {
    double c = 0;
    for (int n : nets) c = std::max(c, B.clearance(n, other, barrierPad));
    return nets.empty() ? B.s.clearance : c;
}

/// Everything copper of `nets` along centre line `seg` (straight or arc, half width hw) on `layer` is too close to.
/// Stops at the first hit when `out` is null. Returns true if there is any.
bool trackHits(const World& w, const std::vector<int>& nets, int layer, const Track& seg, double hw,
               std::vector<Hit>* out) {
    const Vec2 a = seg.a, b = seg.b;
    const Base& B = *w.b;
    const BoardSettings& s = B.s;
    bool any = false;
    auto hit = [&](HitKind k, size_t i, bool fixed) {
        any = true;
        if (out) out->push_back({k, i, fixed});
        return out == nullptr;  // stop
    };
    const Rect box = trackBox(seg, hw + B.maxClearance + 0.05);
    thread_local std::vector<size_t> found;  // no allocation per check (trackHits does not recurse)
    // Quick rejection: the gap between the bounding boxes is a lower bound of the distance (with a margin far above
    // rounding, so only items the exact check would pass are skipped).
    const Rect sb = trackBox(seg, 0);
    auto gap = [&](const Rect& r) { return std::max({0.0, r.x0 - sb.x1, sb.x0 - r.x1, r.y0 - sb.y1, sb.y0 - r.y1}); };
    w.padsIn(box, found);
    for (size_t i : found) {
        const Pad& p = B.pads[i];
        if (!p.onLayer(layer) || inNets(nets, p.net)) continue;
        const bool barrier = B.barrierComps.count(p.componentId) > 0;
        if (gap(p.bounds()) - hw >= clearanceTo(B, nets, p.net, barrier) - kTol + 1e-9) continue;
        const double d = (seg.arc ? (p.round ? std::max(0.0, trackPointDistance(seg, p.position) - std::min(p.size.x, p.size.y) / 2)
                                             : trackRectDistance(seg, p.bounds()))
                                  : padSegmentDistance(p, a, b)) - hw;
        double need = clearanceTo(B, nets, p.net, barrier);
        if (d >= need - kTol) continue;
        // Where the track is still inside its own pad of the same footprint, the gap is the package's pad gap.
        const Vec2 c = seg.arc ? trackClosestPoint(seg, p.position) : closestOnSegment(p.position, a, b);
        bool own = false;
        auto it = B.compPads.find(p.componentId);
        if (it != B.compPads.end())
            for (size_t q : it->second) {
                const Pad& o = B.pads[q];
                if (inNets(nets, o.net) && o.onLayer(layer) && padDistance(o, c) <= 0) own = true;
            }
        if (own) {
            double hv = 0;
            for (int n : nets) hv = std::max(hv, B.voltageClearance(n, p.net));
            need = std::max(kMinEtchGap, hv);
            if (d >= need - kTol) continue;
        }
        if (hit(HitKind::Pad, i, true)) return true;
    }
    w.tracksOnLayer(box, layer, found);
    for (size_t i : found) {
        const Track& t = w.track(i);
        if (t.layer != layer || inNets(nets, t.net)) continue;
        const double need = clearanceTo(B, nets, t.net);
        if (gap(trackBox(t, t.width / 2)) - hw >= need - kTol + 1e-9) continue;
        const double d = trackTrackDistance(seg, t) - hw - t.width / 2;
        if (d < need - kTol && hit(HitKind::Track, i, w.trackFixed(i))) return true;
    }
    w.viasIn(box, found);
    for (size_t i : found) {
        const Via& v = w.via(i);
        if (!v.spans(layer) || inNets(nets, v.net)) continue;
        const double d = trackPointDistance(seg, v.position) - hw - v.diameter / 2;
        if (d < clearanceTo(B, nets, v.net) - kTol && hit(HitKind::Via, i, w.viaFixed(i))) return true;
    }
    // Board edge. On a rectangular board a straight track's distance to the edge is its nearer end's (to within
    // rounding): one well clear of it needs no exact check.
    bool nearEdge = true;
    if (!seg.arc && !s.hasCustomOutline()) {
        const double ends = std::min({a.x, a.y, s.width - a.x, s.height - a.y, b.x, b.y, s.width - b.x, s.height - b.y});
        nearEdge = ends - hw < s.edgeClearance - kTol + 1e-6;
    }
    if (nearEdge && s.trackEdgeDistance(seg) - hw < s.edgeClearance - kTol && hit(HitKind::Edge, 0, true)) return true;
    for (size_t k = 0; k < s.holes.size(); ++k)
        if (trackPointDistance(seg, s.holes[k].position) - hw < s.holes[k].keepout / 2 - kTol &&
            hit(HitKind::Hole, k, true))
            return true;
    if (layer >= 0 && layer < static_cast<int>(B.planeNet.size())) {
        const int pn = B.planeNet[static_cast<size_t>(layer)];
        if (pn >= 0 && !inNets(nets, pn) && hit(HitKind::Plane, static_cast<size_t>(layer), true)) return true;
    }
    for (size_t k = 0; k < B.meshes.size(); ++k) {
        const auto& m = B.meshes[k];
        if ((layer == m.layerA || layer == m.layerB) && !inNets(nets, m.netA) && !inNets(nets, m.netB) &&
            trackRectDistance(seg, m.region) - hw <= 0 && hit(HitKind::Mesh, k, true))
            return true;
    }
    return any;
}

/// The same for the straight segment a-b.
bool segmentHits(const World& w, const std::vector<int>& nets, int layer, Vec2 a, Vec2 b, double hw,
                 std::vector<Hit>* out) {
    Track seg;
    seg.a = a;
    seg.b = b;
    return trackHits(w, nets, layer, seg, hw, out);
}

/// Everything via `v` (index `self` in the world, or SIZE_MAX) is too close to.
bool viaHits(const World& w, const Via& v, size_t self, std::vector<Hit>* out) {
    const Base& B = *w.b;
    const BoardSettings& s = B.s;
    bool any = false;
    auto hit = [&](HitKind k, size_t i, bool fixed) {
        any = true;
        if (out) out->push_back({k, i, fixed});
        return out == nullptr;
    };
    const std::vector<int> nets{v.net};
    const double r = v.diameter / 2;
    const Rect box = Rect::centered(v.position, v.diameter, v.diameter).inflated(B.maxClearance + s.minHoleToHole + 3);
    std::vector<size_t> found;
    w.padsIn(box, found);
    for (size_t i : found) {
        const Pad& p = B.pads[i];
        bool bad = false;
        if (p.throughHole || v.spans(p.smdLayer)) {
            const double d = padDistance(p, v.position) - r;
            if (p.net != v.net || v.net < 0)
                bad = d < clearanceTo(B, nets, p.net, B.barrierComps.count(p.componentId) > 0) - kTol;
            else if (!p.throughHole && !s.viaInPad)
                bad = padDistance(p, v.position) <= 0;  // a via in an SMD pad wicks solder away (DRC_VIA_IN_PAD)
        }
        if (!bad && p.throughHole && p.drill > 0)
            bad = (v.position - p.position).length() - (v.drill + p.drill) / 2 < s.minHoleToHole - kTol;
        if (bad && hit(HitKind::Pad, i, true)) return true;
    }
    w.tracksIn(box, found);
    for (size_t i : found) {
        const Track& t = w.track(i);
        if (!v.spans(t.layer) || inNets(nets, t.net)) continue;
        const double d = trackPointDistance(t, v.position) - r - t.width / 2;
        if (d < clearanceTo(B, nets, t.net) - kTol && hit(HitKind::Track, i, w.trackFixed(i))) return true;
    }
    w.viasIn(box, found);
    for (size_t i : found) {
        if (i == self) continue;
        const Via& o = w.via(i);
        const double dist = (o.position - v.position).length();
        bool bad = dist - (v.drill + o.drill) / 2 < s.minHoleToHole - kTol;
        if (!bad && o.net != v.net && v.overlaps(o))
            bad = dist - r - o.diameter / 2 < clearanceTo(B, nets, o.net) - kTol;
        if (bad && hit(HitKind::Via, i, w.viaFixed(i) || o.net == v.net)) return true;
    }
    if (s.edgeDistance(v.position) - r < s.edgeClearance - kTol && hit(HitKind::Edge, 0, true)) return true;
    for (size_t k = 0; k < s.holes.size(); ++k) {
        const auto& h = s.holes[k];
        const double dist = (h.position - v.position).length();
        if ((dist - r < h.keepout / 2 - kTol || dist - (v.drill + h.drill) / 2 < s.minHoleToHole - kTol) &&
            hit(HitKind::Hole, k, true))
            return true;
    }
    for (size_t k = 0; k < B.meshes.size(); ++k) {
        const auto& m = B.meshes[k];
        if (v.net != m.netA && v.net != m.netB && (v.spans(m.layerA) || v.spans(m.layerB)) &&
            m.region.inflated(r).contains(v.position) && hit(HitKind::Mesh, k, true))
            return true;
    }
    return any;
}

std::string describeHit(const World& w, const Hit& h) {
    const Base& B = *w.b;
    switch (h.kind) {
        case HitKind::Pad: {
            const Pad& p = B.pads[h.index];
            auto it = B.refs.find(p.componentId);
            return "a pad of " + (it != B.refs.end() ? it->second : std::string("a part")) + " (" + B.netName(p.net) + ")";
        }
        case HitKind::Track: {
            const Track& t = w.track(h.index);
            if (w.netFixed(t.net)) return "the route itself";
            return std::string(t.locked ? "a locked track of " : "a track of ") + B.netName(t.net);
        }
        case HitKind::Via: return "a via of " + B.netName(w.via(h.index).net);
        case HitKind::Edge: return "the board edge";
        case HitKind::Hole: return "a mounting-hole keep-out";
        case HitKind::Plane: return "the " + B.netName(B.planeNet[h.index]) + " plane layer";
        case HitKind::Mesh: return "the tamper mesh";
    }
    return "an obstacle";
}

/// Same-net copper a polyline touches (pads, vias, other tracks): a shoved line must keep every one.
std::vector<Hit> contactsOf(const World& w, int net, int layer, const std::vector<Vec2>& pts, double hw) {
    const Base& B = *w.b;
    std::vector<Hit> out;
    std::vector<size_t> found;
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        const Vec2 a = pts[k], b = pts[k + 1];
        const Rect box = segmentBox(a, b, hw + 3);
        w.padsIn(box, found);
        for (size_t i : found) {
            const Pad& p = B.pads[i];
            if (p.net == net && p.onLayer(layer) && padSegmentDistance(p, a, b) <= hw - 1e-6)
                out.push_back({HitKind::Pad, i, true});
        }
        w.viasIn(box, found);
        for (size_t i : found) {
            const Via& v = w.via(i);
            if (v.net == net && v.spans(layer) && pointSegmentDistance(v.position, a, b) <= v.diameter / 2)
                out.push_back({HitKind::Via, i, true});
        }
        w.tracksOnLayer(box, layer, found);
        for (size_t i : found) {
            const Track& t = w.track(i);
            if (t.net == net && t.layer == layer &&
                trackSegmentDistance(t, a, b) <= std::min(hw, t.width / 2))
                out.push_back({HitKind::Track, i, true});
        }
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

bool pathClear(const World& w, const std::vector<int>& nets, int layer, const std::vector<Vec2>& pts, double hw) {
    for (size_t k = 0; k + 1 < pts.size(); ++k)
        if (segmentHits(w, nets, layer, pts[k], pts[k + 1], hw, nullptr)) return false;
    return true;
}

// ------------------------------------------------------------------------------------------------------ lines

/// A chain of same-net, same-width segments on one layer between anchors (pads, vias, junctions, fixed segments).
struct Line {
    int net = -1, layer = 0;
    double width = 0;
    std::vector<Vec2> pts;
    std::vector<size_t> segs;
    long viaAt[2] = {-1, -1};  // via at the start / end anchor
};

std::vector<size_t> tracksEndingAt(const World& w, int net, int layer, Vec2 p, size_t except) {
    std::vector<size_t> found, out;
    w.tracksOnLayer(Rect::centered(p, 2 * kJoin, 2 * kJoin), layer, found);
    for (size_t i : found) {
        const Track& t = w.track(i);
        if (i != except && t.net == net && t.layer == layer && (samePoint(t.a, p) || samePoint(t.b, p))) out.push_back(i);
    }
    return out;
}

/// True when node `p` is held by a pad or a via of the net (the via index goes to *via).
bool nodeAnchored(const World& w, int net, int layer, Vec2 p, long* via) {
    std::vector<size_t> found;
    w.viasIn(Rect::centered(p, 0.01, 0.01), found);
    for (size_t i : found) {
        const Via& v = w.via(i);
        if (v.net == net && v.spans(layer) && (v.position - p).length() <= v.diameter / 2) {
            *via = static_cast<long>(i);
            return true;
        }
    }
    w.padsIn(Rect::centered(p, 0.01, 0.01), found);
    for (size_t i : found) {
        const Pad& pd = w.b->pads[i];
        if (pd.net == net && pd.onLayer(layer) && padDistance(pd, p) <= 0) return true;
    }
    return false;
}

Line extractLine(const World& w, size_t ti) {
    const Track& t = w.track(ti);
    Line L;
    L.net = t.net;
    L.layer = t.layer;
    L.width = t.width;
    L.pts = {t.a, t.b};
    L.segs = {ti};
    for (int fwd = 1; fwd >= 0; --fwd) {
        size_t cur = ti;
        for (int guard = 0; guard < 4096; ++guard) {
            const Vec2 end = fwd ? L.pts.back() : L.pts.front();
            long via = -1;
            if (nodeAnchored(w, L.net, L.layer, end, &via)) {
                L.viaAt[fwd] = via;
                break;
            }
            const auto at = tracksEndingAt(w, L.net, L.layer, end, cur);
            if (at.size() != 1) break;
            const size_t nx = at[0];
            const Track& n = w.track(nx);
            if (w.trackFixed(nx) || std::fabs(n.width - L.width) > 1e-9 ||
                std::find(L.segs.begin(), L.segs.end(), nx) != L.segs.end())
                break;
            const Vec2 other = samePoint(n.a, end) ? n.b : n.a;
            if (fwd) {
                L.pts.push_back(other);
                L.segs.push_back(nx);
            } else {
                L.pts.insert(L.pts.begin(), other);
                L.segs.insert(L.segs.begin(), nx);
            }
            cur = nx;
        }
    }
    return L;
}

// ------------------------------------------------------------------------------------------- joint optimiser

/// Two track ends meeting at `p` with directions `da`, `db` pointing away from it make an acute (< 89°) join: the
/// DRC's DRC_ACUTE_ANGLE (an acid trap). 0° is two tracks running over each other.
bool acuteAway(Vec2 da, Vec2 db) {
    const double l = da.length() * db.length();
    return l > 0 && da.dot(db) > 0.01745 * l;
}

Vec2 awayFrom(const Track& t, Vec2 p) {
    if (t.arc && isArcTrack(t)) return trackEndDirection(t, !samePoint(t.a, p));  // an arc leaves along its tangent
    return samePoint(t.a, p) ? t.b - t.a : t.a - t.b;
}
Vec2 otherEnd(const Track& t, Vec2 p) { return samePoint(t.a, p) ? t.b : t.a; }

/// Alive tracks of `net` on `layer` with an end at `p`.
std::vector<size_t> endsAt(const World& w, int net, int layer, Vec2 p) {
    std::vector<size_t> found, out;
    w.tracksOnLayer(Rect::centered(p, 2 * kJoin, 2 * kJoin), layer, found);
    for (size_t i : found) {
        const Track& t = w.track(i);
        if (t.net == net && t.layer == layer && (samePoint(t.a, p) || samePoint(t.b, p))) out.push_back(i);
    }
    return out;
}

/// A join inside a pad or via of the net is covered by copper (the DRC does not report it).
bool joinCovered(const World& w, int net, int layer, Vec2 p) {
    long via = -1;
    return nodeAnchored(w, net, layer, p, &via);
}

/// The board as it was already had an acute join of the net at `p` (tolerated, like every old violation).
bool acuteBefore(const Base& B, int net, int layer, Vec2 p) {
    std::vector<size_t> found, at;
    B.query(B.trackGrid, Rect::centered(p, 2 * kJoin, 2 * kJoin), found);
    for (size_t i : found) {
        const Track& t = B.tracks[i];
        if (t.net == net && t.layer == layer && (samePoint(t.a, p) || samePoint(t.b, p))) at.push_back(i);
    }
    for (size_t x = 0; x < at.size(); ++x)
        for (size_t y = x + 1; y < at.size(); ++y)
            if (acuteAway(awayFrom(B.tracks[at[x]], p), awayFrom(B.tracks[at[y]], p))) return true;
    return false;
}

struct AcuteJoin {
    Vec2 at;
    size_t a = 0, b = 0;
};

/// Acute joins this session made: at an end of a track it added, between two tracks of which at least one is not
/// part of the route itself (the route's own corners are built non-acute by the posture). Old ones are tolerated.
std::vector<AcuteJoin> newAcuteJoins(const World& w) {
    std::vector<AcuteJoin> out;
    std::set<std::tuple<long long, long long, int>> seen;
    for (size_t k = w.baseT(); k < w.nT(); ++k) {
        if (!w.aliveT(k)) continue;
        const Track& t = w.track(k);
        for (Vec2 p : {t.a, t.b}) {
            const auto key = std::make_tuple(std::llround(p.x * 1e5), std::llround(p.y * 1e5), t.layer);
            if (!seen.insert(key).second) continue;
            const auto at = endsAt(w, t.net, t.layer, p);
            if (at.size() < 2 || joinCovered(w, t.net, t.layer, p)) continue;
            bool old = false, checkedOld = false;
            for (size_t x = 0; x < at.size(); ++x)
                for (size_t y = x + 1; y < at.size(); ++y) {
                    const size_t i = at[x], j = at[y];
                    if (i < w.baseT() && j < w.baseT()) continue;
                    const bool fi = i >= w.baseT() && w.fixAddT[i - w.baseT()], fj = j >= w.baseT() && w.fixAddT[j - w.baseT()];
                    if (fi && fj) continue;
                    if (!acuteAway(awayFrom(w.track(i), p), awayFrom(w.track(j), p))) continue;
                    if (!checkedOld) {
                        old = acuteBefore(*w.b, t.net, t.layer, p);
                        checkedOld = true;
                    }
                    if (!old) out.push_back({p, i, j});
                }
        }
    }
    return out;
}

// ------------------------------------------------------------------------------------------------------ shove

class Shover {
public:
    Shover(World& w, const RouterOptions& opt) : w_(w), B_(*w.b), opt_(opt) {}

    /// Makes room for the given (already added, fixed) head tracks and vias. False with `why` when it cannot.
    bool run(const std::vector<size_t>& tracks, const std::vector<size_t>& vias, std::string& why) {
        for (size_t t : tracks) push({false, t});
        for (size_t v : vias) push({true, v});
        while (!queue_.empty()) {
            const Item p = queue_.front();
            queue_.pop_front();
            if (!alive(p)) continue;
            if (abortRequested()) {
                why = "Cancelled";
                return false;
            }
            for (;;) {
                std::vector<Hit> hits = hitsOf(p);
                if (hits.empty()) break;
                auto fixedHit = std::find_if(hits.begin(), hits.end(), [&](const Hit& h) { return h.fixed && !movableArc(h); });
                if (fixedHit != hits.end()) {
                    why = "Blocked by " + describeHit(w_, *fixedHit);
                    return false;
                }
                if (++ops_ > opt_.shoveLimit) {
                    why = "Too much copper to shove here";
                    return false;
                }
                // An arc in the way goes first (its neighbours follow it); otherwise the first hit.
                auto arcHit = std::find_if(hits.begin(), hits.end(), [&](const Hit& h) { return movableArc(h); });
                const Hit h = arcHit != hits.end() ? *arcHit : hits.front();
                const bool ok = h.kind == HitKind::Track
                                    ? (arcHit != hits.end() ? shoveArc(h.index, p, why) : shoveTrack(h.index, p, why))
                                    : shoveVia(h.index, p, why);
                if (!ok) return false;
                if (!alive(p)) break;
            }
        }
        for (const Item& it : touched_)
            if (alive(it) && !hitsOf(it).empty()) {
                why = "Shoved copper would break clearance";
                return false;
            }
        refilletCorners();
        return cleanJoins(why);
    }

    /// Post-shove optimiser: removes overlaps (0° joins) and chamfers acute joins that shoving made, keeping every
    /// clearance and connection. Fails when an acute join would be left (the DRC would warn about it).
    bool cleanJoins(std::string& why) {
        for (int pass = 0; pass < 8; ++pass) {
            const auto joins = newAcuteJoins(w_);
            if (joins.empty()) return true;
            bool progress = false;
            for (const auto& j : joins)
                if (w_.aliveT(j.a) && w_.aliveT(j.b) && fixJoin(j)) progress = true;
            if (!progress) break;
        }
        if (newAcuteJoins(w_).empty()) return true;
        why = "Shoving here would leave an acute corner";
        return false;
    }

private:
    /// Replaces tracks `olds` by `news` when the new copper keeps clearance and every connection of the old.
    bool tryReplace(const std::vector<size_t>& olds, const std::vector<Track>& news) {
        if (olds.empty()) return false;
        const Track proto = w_.track(olds.front());
        std::vector<Hit> grand;
        for (size_t o : olds) {
            auto g = grandfathered_.find({false, o});
            if (g != grandfathered_.end()) grand.insert(grand.end(), g->second.begin(), g->second.end());
            w_.goneT[o] = 1;
        }
        std::vector<Hit> before;
        for (size_t o : olds) {
            const Track& t = w_.track(o);
            const auto c = contactsOf(w_, t.net, t.layer, {t.a, t.b}, t.width / 2);
            before.insert(before.end(), c.begin(), c.end());
        }
        std::vector<size_t> added;
        for (const Track& t : news) added.push_back(w_.addTrack(t, false));
        auto isNew = [&](const Hit& h) {
            return h.kind == HitKind::Track && std::find(added.begin(), added.end(), h.index) != added.end();
        };
        bool ok = true;
        std::vector<Hit> after;
        for (size_t i : added) {
            const Track& t = w_.track(i);
            for (const Hit& h : rawHits({false, i}))
                if (std::find(grand.begin(), grand.end(), h) == grand.end()) ok = false;
            const auto c = contactsOf(w_, t.net, t.layer, {t.a, t.b}, t.width / 2);
            for (const Hit& h : c)
                if (!isNew(h)) after.push_back(h);
        }
        auto norm = [](std::vector<Hit>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        norm(before);
        norm(after);
        before.erase(std::remove_if(before.begin(), before.end(),
                                    [&](const Hit& h) {
                                        return h.kind == HitKind::Track &&
                                               std::find(olds.begin(), olds.end(), h.index) != olds.end();
                                    }),
                     before.end());
        ok = ok && std::includes(after.begin(), after.end(), before.begin(), before.end());
        if (!ok) {
            for (size_t i : added) w_.goneT[i] = 1;
            for (size_t o : olds) w_.goneT[o] = 0;
            return false;
        }
        for (size_t i : added) {
            grandfathered_[{false, i}] = grand;
            touched_.push_back({false, i});
        }
        (void)proto;
        return true;
    }

    bool fixJoin(const AcuteJoin& j) {
        const Track A = w_.track(j.a), Bt = w_.track(j.b);
        const Vec2 p = j.at, da = awayFrom(A, p), db = awayFrom(Bt, p);
        const bool ma = !w_.trackFixed(j.a), mb = !w_.trackFixed(j.b);
        const double la = da.length(), lb = db.length();
        if (std::fabs(cross(unit(da), unit(db))) < 1e-6) {
            // Two tracks running over each other: the longer is trimmed to start where the shorter ends, or the
            // shorter (covered by the longer) goes.
            const bool aShort = la <= lb;
            const size_t s = aShort ? j.a : j.b, l = aShort ? j.b : j.a;
            const Track& S = aShort ? A : Bt;
            const Track& Lt = aShort ? Bt : A;
            const bool ms = aShort ? ma : mb, ml = aShort ? mb : ma;
            const Vec2 sEnd = otherEnd(S, p), lEnd = otherEnd(Lt, p);
            if (ml) {
                std::vector<Track> rest;
                if ((lEnd - sEnd).length() > 1e-6) {
                    Track t = Lt;
                    t.a = sEnd;
                    t.b = lEnd;
                    rest.push_back(t);
                }
                if (tryReplace({l}, rest)) return true;
            }
            return ms && S.width <= Lt.width + 1e-9 && tryReplace({s}, {});
        }
        if (!ma || !mb || A.layer != Bt.layer || endsAt(w_, A.net, A.layer, p).size() != 2) return false;
        const double wd = std::min(A.width, Bt.width);
        for (double cut : {wd, 2 * wd, 0.5 * wd, 4 * wd, 0.25 * wd}) {
            if (cut >= 0.5 * la || cut >= 0.5 * lb) continue;
            const Vec2 p1 = p + unit(da) * cut, p2 = p + unit(db) * cut;
            Track a2 = A, b2 = Bt, c = wd == A.width ? A : Bt;
            a2.a = p1;
            a2.b = otherEnd(A, p);
            b2.a = p2;
            b2.b = otherEnd(Bt, p);
            c.a = p1;
            c.b = p2;
            if (acuteAway(a2.b - p1, p2 - p1) || acuteAway(b2.b - p2, p1 - p2)) continue;
            if (tryReplace({j.a, j.b}, {a2, c, b2})) return true;
        }
        return false;
    }

public:

private:
    struct Item {
        bool via = false;
        size_t idx = 0;
        bool operator<(const Item& o) const { return std::tie(via, idx) < std::tie(o.via, o.idx); }
    };
    World& w_;
    const Base& B_;
    const RouterOptions& opt_;
    int ops_ = 0;
    std::deque<Item> queue_;
    std::vector<Item> touched_;
    std::map<Item, std::vector<Hit>> grandfathered_;  // violations the item's original copper already had

    void push(Item it) {
        queue_.push_back(it);
        touched_.push_back(it);
    }
    bool alive(const Item& it) const { return it.via ? w_.aliveV(it.idx) : w_.aliveT(it.idx); }
    int netOf(const Item& it) const { return it.via ? w_.via(it.idx).net : w_.track(it.idx).net; }

    std::vector<Hit> rawHits(const Item& it) const {
        std::vector<Hit> hits;
        if (it.via) {
            viaHits(w_, w_.via(it.idx), it.idx, &hits);
        } else {
            const Track& t = w_.track(it.idx);
            trackHits(w_, {t.net}, t.layer, t, t.width / 2, &hits);
        }
        return hits;
    }
    std::vector<Hit> hitsOf(const Item& it) const {
        std::vector<Hit> hits = rawHits(it);
        auto g = grandfathered_.find(it);
        if (g != grandfathered_.end())
            hits.erase(std::remove_if(hits.begin(), hits.end(),
                                      [&](const Hit& h) {
                                          return std::find(g->second.begin(), g->second.end(), h) != g->second.end();
                                      }),
                       hits.end());
        return hits;
    }

    /// Hull of the pusher for copper of `net` with half size `half` (track half width or via radius).
    Octagon pusherHull(const Item& p, int net, double half, double holeDrill) const {
        if (p.via) {
            const Via& v = w_.via(p.idx);
            double grow = B_.clearance(net, v.net) + half + v.diameter / 2;
            if (holeDrill > 0) grow = std::max(grow, B_.s.minHoleToHole + (holeDrill + v.drill) / 2);
            return makeOctagon({v.position}, grow + kMargin);
        }
        const Track& t = w_.track(p.idx);
        if (t.arc && isArcTrack(t)) {  // a shoved arc pushing on: the hull of its chords (radius covers the sagitta)
            const Shape s = trackShape(t);
            return makeOctagon(s.points(), B_.clearance(net, t.net) + half + s.radius + kMargin);
        }
        return makeOctagon({t.a, t.b}, B_.clearance(net, t.net) + half + t.width / 2 + kMargin);
    }
    bool polylineHitsPusher(const std::vector<Vec2>& pts, int net, double hw, const Item& p) const {
        Shape s = p.via ? viaShape(w_.via(p.idx)) : trackShape(w_.track(p.idx));
        const double need = B_.clearance(net, netOf(p));
        for (size_t k = 0; k + 1 < pts.size(); ++k)
            if (s.distanceTo(pts[k], pts[k + 1]) - hw < need - kTol) return true;
        return false;
    }
    /// First fixed hit of the polyline that is not grandfathered.
    bool firstFixedHit(const std::vector<Vec2>& pts, const Line& L, const std::vector<Hit>& grand, Hit& out) const {
        std::vector<Hit> hits;
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            hits.clear();
            segmentHits(w_, {L.net}, L.layer, pts[k], pts[k + 1], L.width / 2, &hits);
            for (const Hit& h : hits)
                if (h.fixed && std::find(grand.begin(), grand.end(), h) == grand.end()) {
                    out = h;
                    return true;
                }
        }
        return false;
    }
    Shape shapeOfHit(const Hit& h) const {
        if (h.kind == HitKind::Pad) return padShape(B_.pads[h.index]);
        if (h.kind == HitKind::Track) return trackShape(w_.track(h.index));
        return viaShape(w_.via(h.index));
    }
    int netOfHit(const Hit& h) const {
        if (h.kind == HitKind::Pad) return B_.pads[h.index].net;
        if (h.kind == HitKind::Track) return w_.track(h.index).net;
        return w_.via(h.index).net;
    }

    /// Pulls the moved stretch [lo, hi] of a shoved line tight with posture shortcuts that keep everything clear.
    std::vector<Vec2> tighten(const std::vector<Vec2>& pts, size_t lo, size_t hi, const Line& L,
                              const std::vector<Hit>& grand) const {
        if (pts.size() < 3 || hi <= lo + 1) return pts;
        const RoutePosture posture =
            opt_.posture == RoutePosture::Free ? RoutePosture::Diagonal45 : opt_.posture;
        auto clean = [&](const std::vector<Vec2>& q) {
            std::vector<Hit> hits;
            for (size_t k = 0; k + 1 < q.size(); ++k) {
                hits.clear();
                segmentHits(w_, {L.net}, L.layer, q[k], q[k + 1], L.width / 2, &hits);
                for (const Hit& h : hits)
                    if (std::find(grand.begin(), grand.end(), h) == grand.end()) return false;
            }
            return true;
        };
        std::vector<Vec2> out(pts.begin(), pts.begin() + static_cast<long>(lo) + 1);
        size_t i = lo;
        while (i < hi) {
            bool done = false;
            for (size_t j = hi; j >= i + 2 && !done; --j) {
                double orig = 0;
                for (size_t k = i; k < j; ++k) orig += (pts[k + 1] - pts[k]).length();
                for (const auto& link : postureLinks(pts[i], pts[j], posture, false)) {
                    if (pathLength(link) >= orig - 1e-6) continue;
                    const Vec2 inDir = out.size() >= 2 ? out.back() - out[out.size() - 2] : Vec2{};
                    if (acuteJoin(inDir, link[1] - link[0])) continue;
                    if (j + 1 < pts.size() && acuteJoin(link.back() - link[link.size() - 2], pts[j + 1] - pts[j])) continue;
                    if (!clean(link)) continue;
                    out.insert(out.end(), link.begin() + 1, link.end());
                    i = j;
                    done = true;
                    break;
                }
            }
            if (!done) {
                out.push_back(pts[i + 1]);
                ++i;
            }
        }
        out.insert(out.end(), pts.begin() + static_cast<long>(hi) + 1, pts.end());
        return simplifyPath(out);
    }

    /// Cuts acute corners (acid traps) of a shoved line where the cut keeps everything clear.
    std::vector<Vec2> relieveAcute(std::vector<Vec2> pts, const Line& L, const std::vector<Hit>& grand) const {
        auto clean = [&](Vec2 a, Vec2 b) {
            std::vector<Hit> hits;
            segmentHits(w_, {L.net}, L.layer, a, b, L.width / 2, &hits);
            for (const Hit& h : hits)
                if (std::find(grand.begin(), grand.end(), h) == grand.end()) return false;
            return true;
        };
        for (size_t i = 1; i + 1 < pts.size(); ++i) {
            const Vec2 a = pts[i - 1], b = pts[i], c = pts[i + 1];
            if (!acuteJoin(b - a, c - b)) continue;
            const double la = (b - a).length(), lc = (c - b).length();
            for (double cut : {L.width, 2 * L.width, 0.5 * L.width, 4 * L.width}) {
                if (cut >= la - 1e-6 || cut >= lc - 1e-6) continue;
                const Vec2 b1 = b + unit(a - b) * cut, b2 = b + unit(c - b) * cut;
                if (acuteJoin(b1 - a, b2 - b1) || acuteJoin(b2 - b1, c - b2) || !clean(b1, b2)) continue;
                pts[i] = b1;
                pts.insert(pts.begin() + static_cast<long>(i) + 1, b2);
                ++i;
                break;
            }
        }
        return pts;
    }

    /// Replaces line L by polyline `pts` (its old segments are gone already); unchanged segments come back.
    void writeBack(const Line& L, const std::vector<Vec2>& pts, const std::vector<Hit>& grand) {
        std::vector<char> reused(L.segs.size(), 0);
        Track proto = w_.track(L.segs.front());
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            const Vec2 a = pts[k], b = pts[k + 1];
            bool kept = false;
            for (size_t s = 0; s < L.segs.size() && !kept; ++s) {
                const Track& o = w_.track(L.segs[s]);
                if (!reused[s] && ((samePoint(o.a, a, 1e-9) && samePoint(o.b, b, 1e-9)) ||
                                   (samePoint(o.a, b, 1e-9) && samePoint(o.b, a, 1e-9)))) {
                    reused[s] = 1;
                    w_.goneT[L.segs[s]] = 0;
                    kept = true;
                }
            }
            if (kept) continue;
            // Another line of the net already has copper there (two lines walked round the same hull): no double.
            bool covered = false;
            std::vector<size_t> found;
            w_.tracksOnLayer(segmentBox(a, b, 0.01), proto.layer, found);
            for (size_t i : found) {
                const Track& o = w_.track(i);
                if (o.net == proto.net && o.layer == proto.layer && o.width >= proto.width - 1e-9 &&
                    trackPointDistance(o, a) <= 1e-7 && trackPointDistance(o, b) <= 1e-7)
                    covered = true;
            }
            if (covered) continue;
            Track t = proto;
            t.a = a;
            t.b = b;
            const Item it{false, w_.addTrack(t, false)};
            grandfathered_[it] = grand;
            push(it);
        }
    }

    /// A violation the board already had: with static copper or base items (never with copper this session added).
    bool preexisting(const Hit& h) const {
        if (h.kind == HitKind::Track) return h.index < w_.baseT();
        if (h.kind == HitKind::Via) return h.index < w_.baseV();
        return true;
    }

    std::vector<Hit> lineHits(const Line& L) const {
        std::vector<Hit> all;
        for (size_t s : L.segs) {
            const Item it{false, s};
            if (s < w_.baseT())  // untouched board copper: what it already violated stays tolerated
                for (const Hit& h : rawHits(it))
                    if (preexisting(h)) all.push_back(h);
            auto g = grandfathered_.find(it);
            if (g != grandfathered_.end()) all.insert(all.end(), g->second.begin(), g->second.end());
        }
        std::sort(all.begin(), all.end());
        all.erase(std::unique(all.begin(), all.end()), all.end());
        return all;
    }

    bool shoveTrack(size_t ti, const Item& pusher, std::string& why) {
        const Line L = extractLine(w_, ti);
        const double hw = L.width / 2;
        const std::vector<Hit> grand = lineHits(L);
        for (size_t s : L.segs) w_.goneT[s] = 1;
        const auto contactsBefore = contactsOf(w_, L.net, L.layer, L.pts, hw);
        auto restore = [&] {
            for (size_t s : L.segs) w_.goneT[s] = 0;
        };
        std::vector<Vec2> pts = L.pts;
        bool clear = false;
        for (int round = 0; round < 12; ++round) {
            Octagon hull;
            if (polylineHitsPusher(pts, L.net, hw, pusher)) {
                hull = pusherHull(pusher, L.net, hw, 0);
            } else {
                Hit h;
                if (!firstFixedHit(pts, L, grand, h)) {
                    clear = true;
                    break;
                }
                if (h.kind == HitKind::Hole) {
                    // A mounting-hole keep-out is a circle the line walks round (no clearance beyond the keep-out).
                    const auto& mh = B_.s.holes[h.index];
                    hull = makeOctagon({mh.position}, mh.keepout / 2 + hw + kMargin);
                } else if (h.kind == HitKind::Mesh) {
                    const Rect& r = B_.meshes[h.index].region;
                    hull = makeOctagon({{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}}, hw + kMargin);
                } else if (h.kind != HitKind::Pad && h.kind != HitKind::Track && h.kind != HitKind::Via) {
                    why = "A track of " + B_.netName(L.net) + " cannot be shoved past " + describeHit(w_, h);
                    restore();
                    return false;
                } else {
                    const Shape s = shapeOfHit(h);
                    const bool barrier = h.kind == HitKind::Pad && B_.barrierComps.count(B_.pads[h.index].componentId) > 0;
                    hull = makeOctagon(s.points(), s.radius + B_.clearance(L.net, netOfHit(h), barrier) + hw + kMargin);
                }
            }
            std::vector<Vec2> c1, c2;
            const bool ok1 = walkAround(pts, hull, true, c1), ok2 = walkAround(pts, hull, false, c2);
            if (!ok1 && !ok2) {
                restore();
                // An end of the line sits in the pusher's way: if it is a via that may move, move it instead.
                for (int e = 0; e < 2; ++e) {
                    const long v = L.viaAt[e];
                    if (v >= 0 && !w_.viaFixed(static_cast<size_t>(v)) && hull.inside(w_.via(static_cast<size_t>(v)).position, -1.0))
                        return shoveVia(static_cast<size_t>(v), pusher, why);
                }
                why = "A track of " + B_.netName(L.net) + " is held in the way (its pad or junction cannot move)";
                return false;
            }
            auto score = [&](const std::vector<Vec2>& c) {
                Hit h;
                return pathLength(c) + (firstFixedHit(c, L, grand, h) ? 1e6 : 0.0);
            };
            if (ok1 && ok2)
                pts = score(c1) <= score(c2) ? c1 : c2;
            else
                pts = ok1 ? c1 : c2;
        }
        if (!clear) {
            restore();
            why = "No room to shove the track of " + B_.netName(L.net);
            return false;
        }
        // Pull the moved stretch tight.
        size_t pre = 0, suf = 0;
        while (pre < pts.size() && pre < L.pts.size() && samePoint(pts[pre], L.pts[pre], 1e-9)) ++pre;
        while (suf < pts.size() - pre && suf < L.pts.size() &&
               samePoint(pts[pts.size() - 1 - suf], L.pts[L.pts.size() - 1 - suf], 1e-9))
            ++suf;
        const size_t lo = pre > 0 ? pre - 1 : 0, hi = std::min(pts.size() - 1, pts.size() - suf);
        std::vector<Vec2> tight = tighten(pts, lo, hi, L, grand);
        tight = relieveAcute(tight, L, grand);
        auto keepsContacts = [&](const std::vector<Vec2>& q) {
            const auto after = contactsOf(w_, L.net, L.layer, q, hw);
            return std::includes(after.begin(), after.end(), contactsBefore.begin(), contactsBefore.end());
        };
        if (keepsContacts(tight))
            pts = tight;
        else if (!keepsContacts(pts)) {
            restore();
            why = "Shoving the track of " + B_.netName(L.net) + " would disconnect it";
            return false;
        }
        writeBack(L, pts, grand);
        return true;
    }

    bool shoveVia(size_t vi, const Item& pusher, std::string& why) {
        const Via v = w_.via(vi);
        if (w_.viaFixed(vi)) {
            why = "Blocked by " + describeHit(w_, {HitKind::Via, vi, true});
            return false;
        }
        const double r = v.diameter / 2;
        // Tracks attached to the via (their ends inside its land) follow it; one passing through it pins it.
        struct Attached {
            Line line;
            bool atStart;
        };
        std::vector<Attached> attached;
        std::vector<size_t> found;
        w_.tracksIn(Rect::centered(v.position, v.diameter, v.diameter), found);
        for (size_t i : found) {
            const Track& t = w_.track(i);
            if (t.net != v.net || !v.spans(t.layer)) continue;
            const bool aIn = (t.a - v.position).length() <= r, bIn = (t.b - v.position).length() <= r;
            if (!aIn && !bIn) {
                if (trackPointDistance(t, v.position) <= r) {
                    why = "A via of " + B_.netName(v.net) + " is pinned by a track through it";
                    return false;
                }
                continue;
            }
            if (aIn && bIn) continue;  // a stub inside the land moves along below
            if (w_.trackFixed(i)) {
                why = "A via of " + B_.netName(v.net) + " is held by a locked track";
                return false;
            }
            Line L = extractLine(w_, i);
            bool atStart = (L.pts.front() - v.position).length() <= r;
            if (!atStart && (L.pts.back() - v.position).length() > r) continue;
            bool dup = false;
            for (const auto& a : attached) dup = dup || a.line.segs == L.segs;
            if (!dup) attached.push_back({L, atStart});
        }
        const Octagon hull = pusherHull(pusher, v.net, r, v.drill);
        std::vector<std::pair<double, int>> cand;
        for (int k = 0; k < 8; ++k) cand.push_back({std::max(0.0, hull.h[k] - kDirs[k].dot(v.position)), k});
        std::sort(cand.begin(), cand.end());
        // Further out on the same sides (one via pitch beyond), tried only when no nearest side is free.
        const double pitch = v.diameter + B_.s.clearance;
        for (int k = 0; k < 8; ++k) cand.push_back({std::max(0.0, hull.h[k] - kDirs[k].dot(v.position)) + pitch, k});
        std::sort(cand.begin() + 8, cand.end());
        w_.goneV[vi] = 1;
        for (const auto& a : attached)
            for (size_t s : a.line.segs) w_.goneT[s] = 1;
        Via nv = v;
        bool placed = false;
        for (const auto& c : cand) {
            nv.position = v.position + kDirs[c.second] * (c.first + 1e-6);
            std::vector<Hit> hits;
            viaHits(w_, nv, SIZE_MAX, &hits);
            bool bad = false;
            for (const Hit& h : hits) bad = bad || h.fixed;
            if (!bad && !(pusher.via ? false : polylineHitsPusher({nv.position, nv.position}, v.net, r, pusher))) {
                placed = true;
                break;
            }
        }
        if (!placed) {
            w_.goneV[vi] = 0;
            for (const auto& a : attached)
                for (size_t s : a.line.segs) w_.goneT[s] = 0;
            why = "No room to shove a via of " + B_.netName(v.net);
            return false;
        }
        const Item vit{true, w_.addVia(nv, false)};
        {
            std::vector<Hit> g;
            if (vi < w_.baseV()) {
                std::vector<Hit> all;
                viaHits(w_, v, SIZE_MAX, &all);
                for (const Hit& h : all)
                    if (preexisting(h)) g.push_back(h);
            } else if (auto old = grandfathered_.find({true, vi}); old != grandfathered_.end()) {
                g = old->second;
            }
            grandfathered_[vit] = g;
        }
        push(vit);
        const Vec2 shift = nv.position - v.position;
        for (const auto& a : attached) {
            std::vector<Vec2> pts = a.line.pts;
            if (!a.atStart) std::reverse(pts.begin(), pts.end());
            const Vec2 start = pts[0] + shift;
            std::vector<Vec2> rest(pts.begin() + 1, pts.end());
            std::vector<Vec2> best;
            for (const auto& link : postureLinks(start, rest.front(), RoutePosture::Diagonal45, false)) {
                std::vector<Vec2> q = link;
                q.insert(q.end(), rest.begin() + 1, rest.end());
                q = simplifyPath(q);
                if (best.empty()) best = q;
                if (pathClear(w_, {a.line.net}, a.line.layer, q, a.line.width / 2)) {
                    best = q;
                    break;
                }
            }
            Line L = a.line;
            writeBack(L, best, {});
        }
        return true;
    }

    // ------------------------------------------------------------------------------------------ shoving arcs

    /// A hit on an arc the shove engine may move as a whole.
    bool movableArc(const Hit& h) const { return h.kind == HitKind::Track && w_.arcShovable(h.index); }

    /// Where an arc track ends: the one movable straight track of its net that continues it there (or none), or held
    /// (a pad, a via, a junction, a locked track or another arc).
    struct ArcEnd {
        long nb = -1;
        bool held = false;
    };
    ArcEnd arcEnd(size_t ai, Vec2 p) const {
        const Track& A = w_.track(ai);
        ArcEnd e;
        long via = -1;
        if (nodeAnchored(w_, A.net, A.layer, p, &via)) {
            e.held = true;
            return e;
        }
        const auto at = tracksEndingAt(w_, A.net, A.layer, p, ai);
        if (at.empty()) return e;
        if (at.size() > 1 || w_.trackFixed(at[0]) || w_.track(at[0]).arc) {
            e.held = true;
            return e;
        }
        e.nb = static_cast<long>(at[0]);
        return e;
    }

    /// Track `t` keeps the clearance (plus kMargin) to the pusher.
    bool clearOfPusher(const Track& t, const Item& p) const {
        if (p.via) {
            const Via& v = w_.via(p.idx);
            if (!v.spans(t.layer)) return true;
            const double need = B_.clearance(v.net, t.net) + kMargin;
            return trackPointDistance(t, v.position) - t.width / 2 - v.diameter / 2 >= need;
        }
        const Track& u = w_.track(p.idx);
        if (u.layer != t.layer) return true;
        const double need = B_.clearance(u.net, t.net) + kMargin;
        return trackTrackDistance(t, u) - t.width / 2 - u.width / 2 >= need;
    }

    /// Shoves arc track `ai` clear of `pusher` as a whole, its neighbours following:
    ///  - an arc that fillets a corner (tangent straight neighbours on both ends) is re-filleted with another radius:
    ///    the arc and its tangent points scale about the corner, so the neighbours only get shorter or longer along
    ///    their own lines (a larger radius moves the arc away from the corner, a smaller one towards it);
    ///  - any other arc whose ends are free or continue in one movable straight track is offset concentrically (the
    ///    radius changes); the neighbours' near ends follow its ends;
    ///  - when no radius clears the pusher, a fillet goes back to its sharp corner (straight tracks, shoved as lines
    ///    from there) and is re-filleted at the end of the shove where it fits (refilletCorners).
    /// Otherwise it fails like a fixed track ("Blocked by ..."). The smallest move that clears the pusher wins.
    bool shoveArc(size_t ai, const Item& pusher, std::string& why) {
        const Hit asHit{HitKind::Track, ai, true};
        const Track A = w_.track(ai);
        const ArcGeom g = trackArc(A);
        auto blocked = [&] {
            why = "Blocked by " + describeHit(w_, asHit);
            return false;
        };
        if (!g.valid) return blocked();
        const ArcEnd e[2] = {arcEnd(ai, A.a), arcEnd(ai, A.b)};
        if (e[0].held || e[1].held || (e[0].nb >= 0 && e[0].nb == e[1].nb)) return blocked();
        const Vec2 ends[2] = {A.a, A.b};
        std::vector<size_t> olds{ai};
        for (const ArcEnd& x : e)
            if (x.nb >= 0) olds.push_back(static_cast<size_t>(x.nb));
        // What the old copper already violated stays tolerated for the new.
        std::vector<Hit> grand;
        for (size_t o : olds) {
            const Item it{false, o};
            if (o < w_.baseT())
                for (const Hit& h : rawHits(it))
                    if (preexisting(h)) grand.push_back(h);
            auto gi = grandfathered_.find(it);
            if (gi != grandfathered_.end()) grand.insert(grand.end(), gi->second.begin(), gi->second.end());
        }
        std::sort(grand.begin(), grand.end());
        grand.erase(std::unique(grand.begin(), grand.end()), grand.end());
        for (size_t o : olds) w_.goneT[o] = 1;
        auto restore = [&] {
            for (size_t o : olds) w_.goneT[o] = 0;
        };
        // Same-net copper on the arc itself (beside its neighbours): the moved arc must keep touching it.
        const auto contactsBefore = contactsOf(w_, A.net, A.layer, trackPolyline(A, 1e-3), A.width / 2);

        // The new arc with the neighbours' near ends moved onto its ends.
        auto build = [&](const Track& arc2, std::vector<Track>& out) {
            out.assign(1, arc2);
            const Vec2 to[2] = {arc2.a, arc2.b};
            for (int k = 0; k < 2; ++k) {
                if (e[k].nb < 0) continue;
                Track n = w_.track(static_cast<size_t>(e[k].nb));
                (samePoint(n.a, ends[k]) ? n.a : n.b) = to[k];
                const Vec2 far = otherEnd(n, to[k]);
                if ((far - to[k]).length() < 0.01) return false;
                if (acuteAway(far - to[k], trackEndDirection(arc2, k == 1))) return false;
                out.push_back(n);
            }
            return true;
        };
        auto clearsPusher = [&](const std::vector<Track>& c) {
            for (const Track& t : c)
                if (!clearOfPusher(t, pusher)) return false;
            return true;
        };
        // No new fixed obstacle (other movable arcs are shoved on in turn) and every contact kept.
        auto acceptable = [&](const std::vector<Track>& c) {
            std::vector<Hit> hits;
            for (const Track& t : c) {
                hits.clear();
                trackHits(w_, {t.net}, t.layer, t, t.width / 2, &hits);
                for (const Hit& h : hits)
                    if (h.fixed && !movableArc(h) && std::find(grand.begin(), grand.end(), h) == grand.end()) return false;
            }
            const auto after = contactsOf(w_, A.net, A.layer, trackPolyline(c.front(), 1e-3), A.width / 2);
            return std::includes(after.begin(), after.end(), contactsBefore.begin(), contactsBefore.end());
        };
        // The least change of the scale f (1 = as it is) within [lo, hi] that clears the pusher: scanned outwards in
        // steps both ways, the step where it starts to clear bisected.
        auto search = [&](const std::function<bool(double, std::vector<Track>&)>& make, double lo, double hi,
                          double step, std::vector<Track>& best) {
            double bestMove = std::numeric_limits<double>::max();
            for (int dir : {1, -1}) {
                double prev = 1;
                bool bisected = false;
                int tries = 0;
                for (int k = 1; k <= 400; ++k) {
                    const double f = 1 + dir * k * step;
                    if (f < lo || f > hi || std::fabs(f - 1) >= bestMove) break;
                    std::vector<Track> c;
                    if (!make(f, c) || !clearsPusher(c)) {
                        prev = f;
                        continue;
                    }
                    if (!bisected) {  // the first step that clears: bisected for the least move
                        bisected = true;
                        double a = prev, b = f;
                        std::vector<Track> cb = c;
                        for (int it = 0; it < 30 && std::fabs(b - a) > 1e-9; ++it) {
                            const double m = (a + b) / 2;
                            std::vector<Track> cm;
                            if (make(m, cm) && clearsPusher(cm)) {
                                b = m;
                                cb = std::move(cm);
                            } else {
                                a = m;
                            }
                        }
                        if (acceptable(cb)) {
                            best = std::move(cb);
                            bestMove = std::fabs(b - 1);
                            break;
                        }
                    }
                    if (acceptable(c)) {
                        best = std::move(c);
                        bestMove = std::fabs(f - 1);
                        break;
                    }
                    if (++tries >= 24) break;  // further out only runs into more copper
                    prev = f;
                }
            }
            return !best.empty();
        };
        auto scaled = [&](Vec2 o, double f) {
            Track t = A;
            t.a = o + (A.a - o) * f;
            t.b = o + (A.b - o) * f;
            t.mid = o + (A.mid - o) * f;
            return t;
        };
        const double stepLen = std::max(0.02, (B_.s.clearance + A.width) / 4);
        const double lo = std::max(A.width / 2, 0.01) / g.r;  // the centre line's radius at least the half width

        // A fillet: tangent straight neighbours on both ends; V is the corner it rounds.
        bool fillet = e[0].nb >= 0 && e[1].nb >= 0;
        Vec2 V;
        if (fillet) {
            const Vec2 d0 = trackEndDirection(A, false), d1 = trackEndDirection(A, true);
            for (int k = 0; k < 2 && fillet; ++k) {
                const Track& n = w_.track(static_cast<size_t>(e[k].nb));
                const Vec2 u = unit(otherEnd(n, ends[k]) - ends[k]), d = k == 0 ? d0 : d1;
                fillet = std::fabs(cross(u, d)) < 1e-3 && u.dot(d) < 0;
            }
            const double den = cross(d0, d1);
            if (fillet && std::fabs(den) > 1e-6) {
                const double s0 = cross(A.b - A.a, d1) / den, s1 = cross(A.b - A.a, d0) / den;
                V = A.a + d0 * s0;
                fillet = s0 > 1e-6 && s1 > 1e-6;
            } else {
                fillet = false;
            }
        }
        std::vector<Track> news;
        if (fillet) {
            const double reach = std::max((A.mid - V).length(), 1e-6);
            double hi = std::numeric_limits<double>::max();
            for (int k = 0; k < 2; ++k) {
                const Track& n = w_.track(static_cast<size_t>(e[k].nb));
                hi = std::min(hi, ((otherEnd(n, ends[k]) - V).length() - 0.01) / (ends[k] - V).length());
            }
            search([&](double f, std::vector<Track>& out) { return build(scaled(V, f), out); }, lo, hi,
                   std::max(1e-6, stepLen / reach), news);
        } else {
            search([&](double f, std::vector<Track>& out) { return build(scaled(g.c, f), out); }, lo,
                   std::numeric_limits<double>::max(), stepLen / g.r, news);
        }
        if (news.empty() && fillet) {
            // Back to the sharp corner: the neighbours meet at V and are shoved as lines; refilletCorners rounds it.
            const Track& n0 = w_.track(static_cast<size_t>(e[0].nb));
            const Track& n1 = w_.track(static_cast<size_t>(e[1].nb));
            if (!acuteAway(otherEnd(n0, A.a) - V, otherEnd(n1, A.b) - V)) {
                for (int k = 0; k < 2; ++k) {
                    Track n = w_.track(static_cast<size_t>(e[k].nb));
                    (samePoint(n.a, ends[k]) ? n.a : n.b) = V;
                    news.push_back(n);
                }
                refillets_.push_back({A.net, A.layer, A.width, g.r, V, std::max(1.0, 4 * (A.a - V).length())});
            }
        }
        if (news.empty()) {
            restore();
            return blocked();
        }
        for (const Track& t : news) {
            const Item it{false, w_.addTrack(t, false)};
            grandfathered_[it] = grand;
            push(it);
        }
        return true;
    }

    /// A fillet shoveArc turned back into its corner: rounded again after the shove.
    struct Refillet {
        int net = -1, layer = 0;
        double width = 0, radius = 0;
        Vec2 at;
        double reach = 0;
    };
    std::vector<Refillet> refillets_;

    /// Rounds each such corner again: the nearest corner of two movable straight tracks of its net within reach of
    /// where it was, with the old radius, a half or a quarter of it, where that keeps every clearance and
    /// connection; a corner with no room stays sharp.
    void refilletCorners() {
        for (const Refillet& R : refillets_) {
            std::vector<size_t> found;
            w_.tracksOnLayer(Rect::centered(R.at, 2 * R.reach, 2 * R.reach), R.layer, found);
            double bestD = R.reach;
            size_t ta = SIZE_MAX, tb = SIZE_MAX;
            Vec2 corner;
            for (size_t i : found) {
                const Track& t = w_.track(i);
                if (t.net != R.net || t.layer != R.layer || t.arc || w_.trackFixed(i) || std::fabs(t.width - R.width) > 1e-9)
                    continue;
                for (Vec2 p : {t.a, t.b}) {
                    const double d = (p - R.at).length();
                    if (d >= bestD || joinCovered(w_, R.net, R.layer, p)) continue;
                    const auto at = endsAt(w_, R.net, R.layer, p);
                    if (at.size() != 2) continue;
                    const size_t o = at[0] == i ? at[1] : at[0];
                    const Track& u = w_.track(o);
                    if (u.arc || w_.trackFixed(o) || std::fabs(u.width - R.width) > 1e-9) continue;
                    bestD = d;
                    corner = p;
                    ta = i;
                    tb = o;
                }
            }
            if (ta != SIZE_MAX) filletCorner(ta, tb, corner, R.radius);
        }
    }

    /// Rounds the corner at `p` between straight tracks `ia` and `ib` with a tangent arc of radius r0 (or r0 / 2,
    /// r0 / 4) when the result keeps every clearance and connection.
    bool filletCorner(size_t ia, size_t ib, Vec2 p, double r0) {
        const Track X = w_.track(ia), Y = w_.track(ib);
        const Vec2 x = otherEnd(X, p), y = otherEnd(Y, p);
        const double lx = (x - p).length(), ly = (y - p).length();
        const Vec2 dx = unit(x - p), dy = unit(y - p);
        const double phi = std::acos(std::clamp(dx.dot(dy), -1.0, 1.0));  // the corner's inner angle
        if (phi < 1e-3 || phi > kPi - 1e-3) return false;
        for (double r : {r0, r0 / 2, r0 / 4}) {
            const double T = r / std::tan(phi / 2);
            if (T >= lx - 0.01 || T >= ly - 0.01) continue;
            const Vec2 t0 = p + dx * T, t1 = p + dy * T;
            const Vec2 c = p + unit(dx + dy) * (r / std::sin(phi / 2));
            const Track arc = makeArcTrack(t0, t1, c, cross(t0 - c, t1 - c) > 0, X.net, X.layer, X.width);
            Track x2 = X, y2 = Y;
            (samePoint(x2.a, p) ? x2.a : x2.b) = t0;
            (samePoint(y2.a, p) ? y2.a : y2.b) = t1;
            if (tryReplace({ia, ib}, {x2, arc, y2})) return true;
        }
        return false;
    }
};

// ------------------------------------------------------------------------------------- walkaround (grid search)

struct GridPath {
    std::vector<Vec2> pts;
    bool reached = false;
};

/// Shortest octilinear (or orthogonal) way from `start` towards `target` for copper of `nets` with half width `hw`,
/// on a grid aligned to `start`, pulled tight with posture links. Ends as close to the target as it can get.
GridPath gridRoute(const World& w, const std::vector<int>& nets, int layer, double hw, Vec2 start, Vec2 target,
                   RoutePosture posture, Vec2 inDir) {
    GridPath res;
    const Base& B = *w.b;
    const BoardSettings& s = B.s;
    if (layer >= 0 && layer < static_cast<int>(B.planeNet.size())) {
        const int pn = B.planeNet[static_cast<size_t>(layer)];
        if (pn >= 0 && !inNets(nets, pn)) return res;
    }
    const double dist = (target - start).length();
    const double margin = std::max(4.0, 0.75 * dist);
    Rect win(std::min(start.x, target.x) - margin, std::min(start.y, target.y) - margin,
             std::max(start.x, target.x) + margin, std::max(start.y, target.y) + margin);
    win = Rect(std::max(win.x0, -1.0), std::max(win.y0, -1.0), std::min(win.x1, s.width + 1), std::min(win.y1, s.height + 1));
    const double cell = std::max(0.05, std::sqrt(std::max(1e-6, win.width() * win.height()) / 200000.0));
    const int i0 = static_cast<int>(std::floor((win.x0 - start.x) / cell));
    const int i1 = static_cast<int>(std::ceil((win.x1 - start.x) / cell));
    const int j0 = static_cast<int>(std::floor((win.y0 - start.y) / cell));
    const int j1 = static_cast<int>(std::ceil((win.y1 - start.y) / cell));
    const int cols = std::max(1, i1 - i0 + 1), rows = std::max(1, j1 - j0 + 1);
    auto centre = [&](int i, int j) { return Vec2{start.x + (i0 + i) * cell, start.y + (j0 + j) * cell}; };
    auto idx = [&](int i, int j) { return static_cast<size_t>(j) * static_cast<size_t>(cols) + static_cast<size_t>(i); };
    std::vector<char> blocked(static_cast<size_t>(cols) * static_cast<size_t>(rows), 0);
    const double slack = cell * 0.75;
    auto markRect = [&](const Rect& r, const auto& bad) {
        const int a0 = std::max(0, static_cast<int>(std::floor((r.x0 - start.x) / cell)) - i0);
        const int a1 = std::min(cols - 1, static_cast<int>(std::ceil((r.x1 - start.x) / cell)) - i0);
        const int b0 = std::max(0, static_cast<int>(std::floor((r.y0 - start.y) / cell)) - j0);
        const int b1 = std::min(rows - 1, static_cast<int>(std::ceil((r.y1 - start.y) / cell)) - j0);
        for (int j = b0; j <= b1; ++j)
            for (int i = a0; i <= a1; ++i)
                if (!blocked[idx(i, j)] && bad(centre(i, j))) blocked[idx(i, j)] = 1;
    };
    const Rect query = win.inflated(hw + B.maxClearance + 1);
    std::vector<size_t> found;
    w.padsIn(query, found);
    for (size_t pi : found) {
        const Pad& p = B.pads[pi];
        if (!p.onLayer(layer) || inNets(nets, p.net)) continue;
        const double need = hw + clearanceTo(B, nets, p.net, B.barrierComps.count(p.componentId) > 0) + slack;
        auto it = B.compPads.find(p.componentId);
        markRect(p.bounds().inflated(need), [&](Vec2 c) {
            const double d = padDistance(p, c);
            if (d >= need) return false;
            if (it != B.compPads.end())
                for (size_t q : it->second) {
                    const Pad& o = B.pads[q];
                    if (inNets(nets, o.net) && o.onLayer(layer) && padDistance(o, c) <= 0) return d < hw + kMinEtchGap + slack;
                }
            return true;
        });
    }
    w.tracksIn(query, found);
    for (size_t ti : found) {
        const Track& t = w.track(ti);
        if (t.layer != layer || inNets(nets, t.net)) continue;
        const double need = hw + t.width / 2 + clearanceTo(B, nets, t.net) + slack;
        markRect(trackBox(t, need), [&](Vec2 c) { return trackPointDistance(t, c) < need; });
    }
    w.viasIn(query, found);
    for (size_t vi : found) {
        const Via& v = w.via(vi);
        if (!v.spans(layer) || inNets(nets, v.net)) continue;
        const double need = hw + v.diameter / 2 + clearanceTo(B, nets, v.net) + slack;
        markRect(Rect::centered(v.position, 2 * need, 2 * need), [&](Vec2 c) { return (c - v.position).length() < need; });
    }
    for (const auto& m : B.meshes)
        if ((layer == m.layerA || layer == m.layerB) && !inNets(nets, m.netA) && !inNets(nets, m.netB))
            markRect(m.region.inflated(hw + slack), [](Vec2) { return true; });
    // Board edge and mounting holes (the rectangular board's edge distance inline: the same values, per cell).
    const bool rectBoard = !s.hasCustomOutline();
    const Rect boardRect(0, 0, s.width, s.height);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            if (blocked[idx(i, j)]) continue;
            const Vec2 c = centre(i, j);
            double ed;
            if (rectBoard) {
                ed = std::min({c.x, c.y, s.width - c.x, s.height - c.y});
                if (ed < 0) ed = -pointRectDistance(c, boardRect);
            } else {
                ed = s.edgeDistance(c);
            }
            if (ed < s.edgeClearance + hw + slack || (!s.holes.empty() && s.holeDistance(c) < hw + slack))
                blocked[idx(i, j)] = 1;
        }
    const int si = -i0, sj = -j0;
    if (si < 0 || sj < 0 || si >= cols || sj >= rows) return res;
    blocked[idx(si, sj)] = 0;
    const int ti = std::clamp(static_cast<int>(std::lround((target.x - start.x) / cell)) - i0, 0, cols - 1);
    const int tj = std::clamp(static_cast<int>(std::lround((target.y - start.y) / cell)) - j0, 0, rows - 1);
    const size_t goal = idx(ti, tj);

    const int nd = posture == RoutePosture::Orthogonal90 ? 4 : 8;
    const int di[8] = {1, 0, -1, 0, 1, -1, -1, 1}, dj[8] = {0, 1, 0, -1, 1, 1, -1, -1};
    auto h = [&](int i, int j) {
        const double dx = std::abs(i - ti), dy = std::abs(j - tj);
        return nd == 4 ? (dx + dy) : (std::max(dx, dy) + (std::sqrt(2.0) - 1) * std::min(dx, dy));
    };
    const size_t n = blocked.size();
    std::vector<double> g(n, std::numeric_limits<double>::max());
    std::vector<long> parent(n, -1);
    std::vector<char> closed(n, 0);
    // Open list: a binary min-heap on (f, h, cell), a strict order, so the cells come out in exactly the order of
    // any other priority queue on the same keys.
    struct QItem {
        double f, h;
        size_t cell;
    };
    auto later = [](const QItem& a, const QItem& b) {
        if (a.f != b.f) return a.f > b.f;
        if (a.h != b.h) return a.h > b.h;
        return a.cell > b.cell;
    };
    std::vector<QItem> open;
    open.reserve(4096);
    auto push = [&](QItem q) {
        open.push_back(q);
        std::push_heap(open.begin(), open.end(), later);
    };
    const size_t s0 = idx(si, sj);
    // A target the start cannot reach makes the search expand the start's whole region and keep the first cell
    // it expands with the smallest h (nearest the target). A flood fill finds the region and that smallest h
    // cheaply, so the search can stop as soon as it expands such a cell: the same cell, the same path. (Diagonal
    // steps need both side cells free, so the region is the 4-connected one.)
    double stopAtH = -1;
    {
        std::vector<char> seen(n, 0);
        std::vector<size_t> stack{s0};
        seen[s0] = 1;
        bool goalIn = false;
        double hmin = std::numeric_limits<double>::max();
        size_t count = 0;
        while (!stack.empty()) {
            const size_t c = stack.back();
            stack.pop_back();
            ++count;
            const int ci = static_cast<int>(c % static_cast<size_t>(cols)), cj = static_cast<int>(c / static_cast<size_t>(cols));
            goalIn = goalIn || c == goal;
            hmin = std::min(hmin, h(ci, cj));
            for (int d = 0; d < 4; ++d) {
                const int ni = ci + di[d], nj = cj + dj[d];
                if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
                const size_t nx = idx(ni, nj);
                if (blocked[nx] || seen[nx]) continue;
                seen[nx] = 1;
                stack.push_back(nx);
            }
        }
        if (!goalIn && count < 600000) stopAtH = hmin;
    }
    g[s0] = 0;
    push({h(si, sj), h(si, sj), s0});
    size_t best = s0;
    double bestH = h(si, sj);
    size_t expanded = 0;
    const double diag = std::sqrt(2.0);
    while (!open.empty() && expanded < 600000) {
        if ((expanded & 1023) == 1023 && abortRequested()) break;
        std::pop_heap(open.begin(), open.end(), later);
        const QItem top = open.back();
        open.pop_back();
        const size_t cur = top.cell;
        if (closed[cur]) continue;
        closed[cur] = 1;
        ++expanded;
        if (top.h < bestH) {
            bestH = top.h;
            best = cur;
        }
        if (top.h == stopAtH) break;  // the nearest cell of an unreachable target's region (see above)
        if (cur == goal) break;
        const int ci = static_cast<int>(cur % static_cast<size_t>(cols)), cj = static_cast<int>(cur / static_cast<size_t>(cols));
        const double gc = g[cur];
        for (int d = 0; d < nd; ++d) {
            const int ni = ci + di[d], nj = cj + dj[d];
            if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
            const size_t nx = idx(ni, nj);
            if (blocked[nx] || closed[nx]) continue;
            if (d >= 4 && (blocked[idx(ci + di[d], cj)] || blocked[idx(ci, cj + dj[d])])) continue;  // no corner cutting
            const double ng = gc + (d >= 4 ? diag : 1.0);
            if (ng < g[nx] - 1e-12) {
                g[nx] = ng;
                parent[nx] = static_cast<long>(cur);
                const double hn = h(ni, nj);
                push({ng + hn, hn, nx});
            }
        }
    }
    const size_t end = closed[goal] ? goal : best;
    std::vector<Vec2> raw;
    for (long c = static_cast<long>(end); c >= 0; c = parent[static_cast<size_t>(c)])
        raw.push_back(centre(static_cast<int>(static_cast<size_t>(c) % static_cast<size_t>(cols)),
                             static_cast<int>(static_cast<size_t>(c) / static_cast<size_t>(cols))));
    std::reverse(raw.begin(), raw.end());
    raw[0] = start;
    bool wantTarget = end == goal;
    if (wantTarget && !samePoint(raw.back(), target, 1e-9)) raw.push_back(target);
    raw = simplifyPath(raw);
    // String pulling: from each point, the farthest later point reachable with a clean posture link.
    std::vector<Vec2> out{raw[0]};
    size_t i = 0;
    while (i + 1 < raw.size()) {
        bool found2 = false;
        for (size_t j = raw.size() - 1; j > i && !found2; --j) {
            for (const auto& link : postureLinks(raw[i], raw[j], posture, false)) {
                const Vec2 prev = out.size() >= 2 ? out.back() - out[out.size() - 2] : inDir;
                if (link.size() < 2 || acuteJoin(prev, link[1] - link[0])) continue;
                if (!pathClear(w, nets, layer, link, hw)) continue;
                out.insert(out.end(), link.begin() + 1, link.end());
                i = j;
                found2 = true;
                break;
            }
        }
        if (!found2) break;
    }
    res.pts = simplifyPath(out);
    res.reached = wantTarget && i + 1 == raw.size() && samePoint(res.pts.back(), target, 1e-9);
    return res;
}

// ------------------------------------------------------------------------------------------ meanders (tuning)

/// Accordion replacing track `t`: `bumps` bumps of height `amplitude` on one side, starting `start` mm along it.
std::vector<Vec2> accordion(const Track& t, double amplitude, double side, double pitch, int bumps, double start) {
    const Vec2 u = unit(t.b - t.a);
    const Vec2 n = Vec2{-u.y, u.x} * side;
    std::vector<Vec2> pts = {t.a};
    Vec2 p = t.a + u * start;
    pts.push_back(p);
    for (int i = 0; i < bumps; ++i) {
        p = p + n * amplitude;
        pts.push_back(p);
        p = p + u * pitch;
        pts.push_back(p);
        p = p - n * amplitude;
        pts.push_back(p);
        p = p + u * pitch;
        pts.push_back(p);
    }
    pts.push_back(t.b);
    return simplifyPath(pts);
}

Json trackJson(const Track& t) {
    Json j = Json::object();
    j["id"] = t.id;
    j["net"] = t.net;
    j["layer"] = t.layer;
    j["width"] = t.width;
    j["ax"] = t.a.x;
    j["ay"] = t.a.y;
    j["bx"] = t.b.x;
    j["by"] = t.b.y;
    if (t.teardrop) j["teardrop"] = true;
    if (t.arc) {  // true arc (see the snapshot's tracks)
        j["arc"] = true;
        j["mx"] = t.mid.x;
        j["my"] = t.mid.y;
        const ArcGeom g = trackArc(t);
        if (g.valid) {
            j["cx"] = g.c.x;
            j["cy"] = g.c.y;
            j["radius"] = g.r;
            j["startAngle"] = g.start;
            j["sweep"] = g.sweep;
        }
    }
    return j;
}

/// A via as JSON; with the board's layer count also its span (`toLayer` resolved) and kind, as in the snapshot.
Json viaJson(const Via& v, int layerCount = 0) {
    Json j = Json::object();
    j["id"] = v.id;
    j["net"] = v.net;
    j["x"] = v.position.x;
    j["y"] = v.position.y;
    j["drill"] = v.drill;
    j["diameter"] = v.diameter;
    if (layerCount > 0) {
        j["fromLayer"] = v.fromLayer;
        j["toLayer"] = v.lastLayer(layerCount);
        j["kind"] = viaKind(v, layerCount);
    }
    return j;
}

/// Consecutive collinear pieces of a path (same layer and width) become one track.
std::vector<Track> mergeCollinear(const std::vector<Track>& in) {
    std::vector<Track> out;
    for (const Track& t : in) {
        if (trackLength(t) < 1e-9) continue;
        if (!out.empty() && !t.arc && !out.back().arc) {
            Track& p = out.back();
            if (p.layer == t.layer && std::fabs(p.width - t.width) < 1e-12 && p.net == t.net && samePoint(p.b, t.a, 1e-9) &&
                std::fabs(cross(unit(p.b - p.a), unit(t.b - t.a))) < 1e-9 && (p.b - p.a).dot(t.b - t.a) > 0) {
                p.b = t.b;
                continue;
            }
        }
        out.push_back(t);
    }
    return out;
}

bool sameTrack(const Track& a, const Track& b) {
    return a.id == b.id && a.net == b.net && a.layer == b.layer && a.width == b.width && a.a == b.a && a.b == b.b &&
           a.locked == b.locked && a.arc == b.arc && (!a.arc || a.mid == b.mid);
}
bool sameVia(const Via& a, const Via& b) {
    return a.id == b.id && a.net == b.net && a.position == b.position && a.drill == b.drill &&
           a.diameter == b.diameter && a.fromLayer == b.fromLayer && a.toLayer == b.toLayer;
}

/// Writes the world's changes into the layout: gone base items are removed, `routeTracks` and every other added
/// item are added. Returns what changed.
RouteChanges applyWorld(PcbLayout& pcb, const World& w, const std::vector<Track>& routeTracks,
                        const std::vector<Via>& routeVias) {
    RouteChanges ch;
    std::set<int> goneT, goneV;
    for (size_t i = 0; i < w.baseT(); ++i)
        if (w.goneT[i]) {
            ch.removedTracks.push_back(w.b->tracks[i]);
            goneT.insert(w.b->tracks[i].id);
        }
    for (size_t i = 0; i < w.baseV(); ++i)
        if (w.goneV[i]) {
            ch.removedVias.push_back(w.b->vias[i]);
            goneV.insert(w.b->vias[i].id);
        }
    pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return goneT.count(t.id) > 0; }),
                     pcb.tracks.end());
    pcb.vias.erase(std::remove_if(pcb.vias.begin(), pcb.vias.end(), [&](const Via& v) { return goneV.count(v.id) > 0; }),
                   pcb.vias.end());
    for (const Track& t : routeTracks) ch.addedTracks.push_back(pcb.addTrack(t));
    for (size_t k = 0; k < w.addT.size(); ++k)
        if (!w.goneT[w.baseT() + k] && !w.fixAddT[k]) ch.addedTracks.push_back(pcb.addTrack(w.addT[k]));
    for (const Via& v : routeVias) ch.addedVias.push_back(pcb.addVia(v));
    for (size_t k = 0; k < w.addV.size(); ++k)
        if (!w.goneV[w.baseV() + k] && !w.fixAddV[k]) ch.addedVias.push_back(pcb.addVia(w.addV[k]));
    ch.ok = true;
    return ch;
}

/// Pads of `net` partitioned by the copper (tracks and vias of the net, minus `without`): label[i] is the smallest pad
/// index (among `padsOfNet`) joined to pad i.
std::vector<size_t> padPartition(const std::vector<Pad>& pads, const std::vector<size_t>& padsOfNet,
                                 const std::vector<Track>& tracks, const std::vector<Via>& vias, int net,
                                 const std::set<int>& without) {
    std::vector<const Track*> ts;
    for (const auto& t : tracks)
        if (t.net == net && !without.count(t.id)) ts.push_back(&t);
    std::vector<const Via*> vs;
    for (const auto& v : vias)
        if (v.net == net) vs.push_back(&v);
    const size_t np = padsOfNet.size(), nt = ts.size(), nv = vs.size();
    std::vector<size_t> parent(np + nt + nv);
    for (size_t i = 0; i < parent.size(); ++i) parent[i] = i;
    std::function<size_t(size_t)> find = [&](size_t x) {
        while (parent[x] != x) x = parent[x] = parent[parent[x]];
        return x;
    };
    auto unite = [&](size_t a, size_t b) { parent[find(a)] = find(b); };
    for (size_t i = 0; i < nt; ++i) {
        const Track& t = *ts[i];
        for (size_t p = 0; p < np; ++p) {
            const Pad& pd = pads[padsOfNet[p]];
            if (!pd.onLayer(t.layer)) continue;
            const bool touch = pd.round ? trackPointDistance(t, pd.position) <= std::min(pd.size.x, pd.size.y) / 2 + t.width / 2 - 1e-6
                                        : trackRectDistance(t, pd.bounds()) <= t.width / 2 - 1e-6;
            if (touch) unite(i + np, p);
        }
        for (size_t j = i + 1; j < nt; ++j)
            if (ts[j]->layer == t.layer && trackTrackDistance(t, *ts[j]) <= std::min(t.width, ts[j]->width) / 2) unite(i + np, j + np);
        for (size_t v = 0; v < nv; ++v)
            if (vs[v]->spans(t.layer) && trackPointDistance(t, vs[v]->position) <= vs[v]->diameter / 2) unite(i + np, v + np + nt);
    }
    for (size_t v = 0; v < nv; ++v)
        for (size_t p = 0; p < np; ++p) {
            const Pad& pd = pads[padsOfNet[p]];
            if ((pd.throughHole || vs[v]->spans(pd.smdLayer)) && padDistance(pd, vs[v]->position) <= vs[v]->diameter / 2 - 1e-6)
                unite(v + np + nt, p);
        }
    std::vector<size_t> label(np);
    for (size_t p = 0; p < np; ++p) {
        label[p] = p;
        for (size_t q = 0; q < p; ++q)
            if (find(q) == find(p)) {
                label[p] = q;
                break;
            }
    }
    return label;
}

}  // namespace

// ================================================================================================ the router

struct InteractiveRouter::Impl {
    PcbLayout& pcb;
    const Schematic& sch;
    RouterOptions opt;
    std::unique_ptr<Base> base;
    World committed, current;
    enum class Kind { None, Route, Pair, Bus, Drag, DragVia, DragMulti } kind = Kind::None;

    struct Member {
        int net = -1;
        Vec2 end;                    // where the next head starts
        long startPad = -1;          // pad the route began on (reaching it is not "reaching a target")
        std::vector<Track> placed;   // in path order
        std::vector<Vec2> head;      // current head polyline (starts at `end`)
    };
    std::vector<Member> members;
    int layer = 0;
    double width = 0, gap = 0, spacing = 0;
    Vec2 centre;                 // pair: where the centre line continues
    std::vector<Vec2> centreHead;
    int side = 0;                // pair: members[0] lies on the left (+1) or right (-1) of the centre line
    std::vector<double> busOffset;  // bus: each member's offset left of the centre line (fixed at the first corner)
    Vec2 busAcross;                 // bus: across the start pads' row (the bundle leaves the row this way or back)
    double busPadHalf = 0;          // bus: largest pad half-size across the row
    Vec2 busFanOut;                 // bus, before the first corner: the side the bundle leaves the row (0 = later)
    Vec2 lastDir;                // direction of the last placed segment (0 = none)
    std::vector<Via> placedVias;
    bool reached = false, blocked = false;
    std::string status, err;
    Vec2 lastCursor;
    bool hasCursor = false;

    struct Drag {
        size_t track = 0;
        Vec2 a, b, grab, dir, normal;
        Vec2 anchorA, anchorB;
        bool anchoredA = true, anchoredB = true;
        /// Direction of the copper beyond a free anchor: arriving at anchorA / leaving anchorB (0 = none or several).
        Vec2 intoA, outOfB;
    } drag;

    /// Via drag: the via and the lines that end on it (each oriented to start at the via).
    struct ViaDrag {
        Via via;
        Vec2 grab;
        std::vector<Line> lines;
        size_t index = 0;           // the via in the board snapshot
        Vec2 at;                    // where the via is now
        std::vector<Track> tracks;  // the lines as they follow the via (per-line layer and width)
        bool valid = false;         // false: the via fits nowhere along the way (commit changes nothing)
    } vdrag;
    /// Corner drag and multi-track drag: the dragged tracks move by the cursor's offset; at each end of the
    /// selection that is not shared by two dragged tracks, the lines beyond rejoin their old paths, or (on a pad or
    /// via) a link joins the end to it.
    struct MultiDrag {
        struct End {
            Vec2 p;
            int net = -1, layer = 0;
            double width = 0;
            bool anchored = false;    // on a pad / via / fixed track: a link from p to the moved end
            std::vector<Line> lines;  // lines starting at p that rejoin from the moved end
        };
        std::vector<size_t> sel;      // dragged tracks (board indices)
        std::vector<End> ends;
        Vec2 grab, offset;
        bool corner = false;
        std::vector<Track> tracks;    // the result at `offset`
        bool valid = false;
    } mdrag;
    bool multiRoute = false;          // the bus was started with beginMultiRoute
    double groupTarget = 0;     // longest other member of the net's matched-length group (0 = none)

    RoutePreview prev;

    Impl(PcbLayout& p, const Schematic& s) : pcb(p), sch(s) {}

    void reset() {
        kind = Kind::None;
        base.reset();
        committed = World();
        current = World();
        members.clear();
        placedVias.clear();
        centreHead.clear();
        reached = blocked = false;
        side = 0;
        lastDir = {};
        status.clear();
        hasCursor = false;
        vdrag = ViaDrag{};
        mdrag = MultiDrag{};
        multiRoute = false;
        groupTarget = 0;
        busOffset.clear();
        prev = RoutePreview{};
    }

    bool fail(const std::string& why) {
        err = why;
        return false;
    }

    double netWidth(int net) const {
        if (opt.width > 0) return opt.width;
        return base->s.widthFor(base->netName(net));
    }

    bool layerUsable(int l, const std::vector<int>& nets) {
        if (l < 0 || l >= std::max(1, base->s.layerCount)) return fail("That copper layer is not in the stack-up");
        const int pn = base->planeNet[static_cast<size_t>(l)];
        if (pn >= 0 && !inNets(nets, pn))
            return fail(copperLayerName(l, base->s.layerCount) + " is the " + base->netName(pn) +
                        " plane — route other nets on a signal layer");
        return true;
    }

    /// Pad (preferring the active layer), via or track with a net under `at`.
    struct StartHit {
        int net = -1;
        Vec2 point;
        long pad = -1;
        int layer = -1;  // layer the start forces (SMD pad on another layer), -1 = any
    };
    bool findStart(Vec2 at, int l, StartHit& out) const {
        const Base& B = *base;
        long best = -1;
        double bestD = 1e9;
        for (size_t i = 0; i < B.pads.size(); ++i) {
            const Pad& p = B.pads[i];
            if (p.net < 0 || padDistance(p, at) > 0) continue;
            const double d = (p.position - at).length() + (p.onLayer(l) ? 0 : 1000);
            if (d < bestD) {
                bestD = d;
                best = static_cast<long>(i);
            }
        }
        if (best >= 0) {
            const Pad& p = B.pads[static_cast<size_t>(best)];
            out = {p.net, p.position, best, p.onLayer(l) ? -1 : p.smdLayer};
            return true;
        }
        for (size_t i = 0; i < B.vias.size(); ++i) {
            const Via& v = B.vias[i];
            if (v.net >= 0 && v.spans(l) && (v.position - at).length() <= v.diameter / 2) {
                out = {v.net, v.position, -1, -1};
                return true;
            }
        }
        for (size_t i = 0; i < B.tracks.size(); ++i) {
            const Track& t = B.tracks[i];
            if (t.net < 0 || t.layer != l || trackPointDistance(t, at) > t.width / 2) continue;
            Vec2 p = trackClosestPoint(t, at);
            if ((p - t.a).length() <= t.width) p = t.a;
            if ((p - t.b).length() <= t.width) p = t.b;
            out = {t.net, p, -1, -1};
            return true;
        }
        return false;
    }

    void startSession(const std::vector<int>& nets) {
        committed = World(base.get());
        committed.fixedNets = nets;
        current = committed;
        groupTarget = 0;
        if (!nets.empty() && nets[0] >= 0)
            for (const auto& g : lengthGroups(sch, base->s)) {
                if (std::find(g.nets.begin(), g.nets.end(), nets[0]) == g.nets.end()) continue;
                for (int n : g.nets)
                    if (n != nets[0]) groupTarget = std::max(groupTarget, routedNetLength(pcb, n));
            }
        // A length rule or match group sets the target instead (boards without any are unchanged).
        LengthTarget lt;
        if (!nets.empty() && nets[0] >= 0 && lengthTargetFor(pcb, sch, base->pads, nets[0], lt)) groupTarget = lt.target;
    }

    // ------------------------------------------------------------------------------------------------ routing

    bool beginRoute(Vec2 at, int l) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        if (l < 0 || l >= std::max(1, base->s.layerCount)) {
            base.reset();
            return fail("That copper layer is not in the stack-up");
        }
        StartHit st;
        if (!findStart(at, l, st)) {
            base.reset();
            return fail("Start the route on a pad, via or track that has a net");
        }
        if (st.layer >= 0) l = st.layer;
        if (!layerUsable(l, {st.net})) {
            base.reset();
            return false;
        }
        layer = l;
        width = netWidth(st.net);
        Member m;
        m.net = st.net;
        m.end = st.point;
        m.startPad = st.pad;
        members = {m};
        kind = Kind::Route;
        startSession({st.net});
        // Starting at the free end of a track: the route continues it, so its first corner must not fold back.
        if (st.pad < 0 && !joinCovered(committed, st.net, l, st.point)) {
            const auto at = endsAt(committed, st.net, l, st.point);
            if (at.size() == 1) lastDir = st.point - otherEnd(committed.track(at[0]), st.point);
        }
        status = "Routing " + base->netName(st.net);
        buildPreview();
        return true;
    }

    bool beginPair(Vec2 at, int l) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        if (l < 0 || l >= std::max(1, base->s.layerCount)) return bail("That copper layer is not in the stack-up");
        StartHit st;
        if (!findStart(at, l, st) || st.pad < 0) return bail("Start a differential pair on a pad of one of its nets");
        int pNet = -1, nNet = -1;
        for (const auto& pr : differentialPairs(sch))
            if (pr.first == st.net || pr.second == st.net) {
                pNet = pr.first;
                nNet = pr.second;
            }
        if (pNet < 0)
            return bail(base->netName(st.net) + " is not part of a differential pair (name the nets X_P / X_N or X+ / X-)");
        if (st.layer >= 0) l = st.layer;
        const int other = st.net == pNet ? nNet : pNet;
        const Pad& sp = base->pads[static_cast<size_t>(st.pad)];
        long partner = -1;
        double bestD = 1e9;
        for (size_t i = 0; i < base->pads.size(); ++i) {
            const Pad& p = base->pads[i];
            if (p.net != other || !p.onLayer(l)) continue;
            const double d = (p.position - sp.position).length() + (p.componentId == sp.componentId ? 0 : 1000);
            if (d < bestD) {
                bestD = d;
                partner = static_cast<long>(i);
            }
        }
        if (partner < 0) return bail("No pad of " + base->netName(other) + " to start the pair's other member from");
        if (!layerUsable(l, {pNet, nNet})) {
            base.reset();
            return false;
        }
        layer = l;
        width = std::max(netWidth(pNet), netWidth(nNet));
        if (opt.pairGap > 0) {
            gap = opt.pairGap;
        } else {
            const auto geom = differentialPairGeometry(base->s, l, base->s.differentialImpedance);
            gap = std::max(geom.second, base->s.clearance);
        }
        gap = std::max(gap, base->clearance(pNet, nNet));
        spacing = width + gap;
        Member mp, mn;
        mp.net = pNet;
        mn.net = nNet;
        const Pad& pp = base->pads[static_cast<size_t>(st.net == pNet ? st.pad : partner)];
        const Pad& pn = base->pads[static_cast<size_t>(st.net == pNet ? partner : st.pad)];
        mp.end = pp.position;
        mp.startPad = st.net == pNet ? st.pad : partner;
        mn.end = pn.position;
        mn.startPad = st.net == pNet ? partner : st.pad;
        members = {mp, mn};
        centre = (mp.end + mn.end) * 0.5;
        kind = Kind::Pair;
        startSession({pNet, nNet});
        status = "Routing pair " + base->netName(pNet) + " / " + base->netName(nNet);
        buildPreview();
        return true;
    }

    /// Where the head aims: the cursor, or the pad / via / track of `net` under it.
    Vec2 snapTarget(Vec2 cursor, const Member& m, bool& hit) const {
        hit = false;
        if (!opt.snapToPads) return cursor;
        const Base& B = *base;
        long best = -1;
        double bestD = 1e9;
        for (size_t i = 0; i < B.pads.size(); ++i) {
            const Pad& p = B.pads[i];
            if (p.net != m.net || !p.onLayer(layer) || padDistance(p, cursor) > 0.05) continue;
            const double d = (p.position - cursor).length();
            if (d < bestD) {
                bestD = d;
                best = static_cast<long>(i);
            }
        }
        if (best >= 0) {
            hit = best != m.startPad || !m.placed.empty();
            return B.pads[static_cast<size_t>(best)].position;
        }
        std::vector<size_t> found;
        committed.viasIn(Rect::centered(cursor, 4, 4), found);
        for (size_t i : found) {
            const Via& v = committed.via(i);
            if (v.net == m.net && v.spans(layer) && (v.position - cursor).length() <= v.diameter / 2 &&
                !samePoint(v.position, m.end)) {
                hit = true;
                return v.position;
            }
        }
        committed.tracksIn(Rect::centered(cursor, 4, 4), found);
        for (size_t i : found) {
            const Track& t = committed.track(i);
            if (t.net != m.net || t.layer != layer) continue;
            if (i >= committed.baseT() && committed.fixAddT[i - committed.baseT()]) continue;  // the route itself
            if (trackPointDistance(t, cursor) <= t.width / 2) {
                hit = true;
                Vec2 p = trackClosestPoint(t, cursor);
                if ((p - t.a).length() <= t.width) p = t.a;
                if ((p - t.b).length() <= t.width) p = t.b;
                return p;
            }
        }
        return cursor;
    }

    /// The posture heads from a to b; a head that would turn back on the last placed segment (an acute corner) is
    /// left out — the walkaround search then finds a way that turns properly.
    std::vector<std::vector<Vec2>> orderedLinks(Vec2 a, Vec2 b) const {
        auto links = postureLinks(a, b, opt.posture, opt.swapPosture);
        if (lastDir.length() > 0)
            links.erase(std::remove_if(links.begin(), links.end(),
                                       [&](const std::vector<Vec2>& x) { return x.size() >= 2 && acuteJoin(lastDir, x[1] - x[0]); }),
                        links.end());
        return links;
    }

    struct HeadLine {
        int net;
        std::vector<Vec2> pts;
        /// The line starts back on the member's last placed track, which ends at `trimFrom`: that track ends at the
        /// line's start instead (a pair's inner member turning at a placed corner).
        bool trim = false;
        Vec2 trimFrom;
        HeadLine(int n, std::vector<Vec2> p, bool tr = false, Vec2 from = {})
            : net(n), pts(std::move(p)), trim(tr), trimFrom(from) {}
    };

    /// Adds the head lines to `w` as fixed copper (no checks).
    std::vector<size_t> addHeadLines(World& w, const std::vector<HeadLine>& lines) const {
        std::vector<size_t> added;
        for (const auto& hl : lines) {
            if (hl.trim && !hl.pts.empty())
                for (size_t k = 0; k < w.addT.size(); ++k) {
                    Track& t = w.addT[k];
                    if (w.goneT[w.baseT() + k] || !w.fixAddT[k] || t.net != hl.net || t.arc || !samePoint(t.b, hl.trimFrom, 1e-9))
                        continue;
                    if (pointSegmentDistance(hl.pts.front(), t.a, t.b) <= 1e-9) {
                        t.b = hl.pts.front();
                        w.addTBox[k] = trackBox(t, t.width / 2);
                    }
                }
            for (size_t k = 0; k + 1 < hl.pts.size(); ++k) {
                if ((hl.pts[k + 1] - hl.pts[k]).length() < 1e-9) continue;
                Track t;
                t.net = hl.net;
                t.layer = layer;
                t.width = width;
                t.a = hl.pts[k];
                t.b = hl.pts[k + 1];
                added.push_back(w.addTrack(t, true));
            }
        }
        return added;
    }

    bool highlight() const { return opt.mode == RouterMode::Highlight; }

    double cornerRadius() const {
        if (opt.cornerRadius > 0) return opt.cornerRadius;
        return opt.cornerRadius < 0 ? std::max(0.5, 4 * width) : 0;
    }

    /// Rounded corners for a single-track route: `tracks` (in path order, `headFrom` = index of the first head
    /// track) as runs of joined segments of one layer, each run filleted where the arcs keep clearance in `w`.
    /// Returns the rounded tracks; `headOut` receives the index where the head's part starts.
    std::vector<Track> roundRoute(const std::vector<Track>& tracks, size_t headFrom, const World& w, size_t* headOut) const {
        std::vector<Track> out;
        if (headOut) *headOut = std::numeric_limits<size_t>::max();
        const double r = cornerRadius();
        size_t i = 0;
        while (i < tracks.size()) {
            size_t j = i + 1;
            while (j < tracks.size() && tracks[j].layer == tracks[i].layer && std::fabs(tracks[j].width - tracks[i].width) < 1e-12 &&
                   samePoint(tracks[j].a, tracks[j - 1].b, 1e-9))
                ++j;
            std::vector<Vec2> pts{tracks[i].a};
            for (size_t k = i; k < j; ++k) pts.push_back(tracks[k].b);
            const Track& t0 = tracks[i];
            if (opt.arcCorners && r > 0) {  // true arcs
                std::vector<std::vector<size_t>> vos;
                auto clearArc = [&](size_t, const Track& arc) {
                    return !trackHits(w, {t0.net}, t0.layer, arc, t0.width / 2, nullptr);
                };
                const auto res = filletArcRuns({pts}, {t0}, r, std::max(t0.width, 0.05), clearArc, {}, &vos);
                for (size_t k = 0; k < res[0].size(); ++k) {
                    if (headOut && *headOut == std::numeric_limits<size_t>::max() && headFrom >= i && headFrom < j &&
                        k >= vos[0][headFrom - i])
                        *headOut = out.size();
                    out.push_back(res[0][k]);
                }
                i = j;
                continue;
            }
            auto clear = [&](const std::vector<Vec2>& arc) { return pathClear(w, {t0.net}, t0.layer, arc, t0.width / 2); };
            std::vector<size_t> vo;
            const std::vector<Vec2> q = r > 0 ? filletPath(pts, r, std::max(t0.width, 0.05), clear, &vo) : pts;
            if (r <= 0) {
                vo.resize(pts.size());
                for (size_t k = 0; k < pts.size(); ++k) vo[k] = k;
            }
            for (size_t k = 0; k + 1 < q.size(); ++k) {
                if (headOut && *headOut == std::numeric_limits<size_t>::max() && headFrom >= i && headFrom < j &&
                    k >= vo[headFrom - i])
                    *headOut = out.size();
                Track t = t0;
                t.a = q[k];
                t.b = q[k + 1];
                out.push_back(t);
            }
            i = j;
        }
        if (headOut && *headOut == std::numeric_limits<size_t>::max()) *headOut = out.size();
        return out;
    }

    /// Arc corners for a differential pair or a bus: each member's tracks (`per[k]`, in path order, the head from
    /// `headFrom[k]`) are cut into runs (one layer and width); when every member has the same runs they are filleted
    /// together, so the corners they turn together become concentric arcs. Every arc is then checked against the
    /// other members' final copper; a corner that fails stays sharp (and the rounding is redone without it).
    std::vector<std::vector<Track>> roundGroup(const std::vector<std::vector<Track>>& per,
                                               const std::vector<size_t>& headFrom, const World& w,
                                               std::vector<size_t>* headOut) const {
        const size_t nm = per.size();
        const size_t none = std::numeric_limits<size_t>::max();
        std::vector<std::vector<Track>> out(nm);
        if (headOut) headOut->assign(nm, none);
        const double r = cornerRadius();
        std::vector<std::vector<std::pair<size_t, size_t>>> runs(nm);  // [from, to) track ranges
        for (size_t k = 0; k < nm; ++k)
            for (size_t i = 0; i < per[k].size();) {
                size_t j = i + 1;
                while (j < per[k].size() && per[k][j].layer == per[k][i].layer && !per[k][j].arc &&
                       std::fabs(per[k][j].width - per[k][i].width) < 1e-12 && samePoint(per[k][j].a, per[k][j - 1].b, 1e-9))
                    ++j;
                runs[k].push_back({i, j});
                i = j;
            }
        bool joint = nm > 1;
        for (size_t k = 1; k < nm && joint; ++k) {
            joint = runs[k].size() == runs[0].size();
            for (size_t ri = 0; ri < runs[0].size() && joint; ++ri)
                joint = per[k][runs[k][ri].first].layer == per[0][runs[0][ri].first].layer;
        }
        std::vector<int> nets;
        for (const auto& m : members) nets.push_back(m.net);
        // Groups of (member, run index) filleted together.
        std::vector<std::vector<std::pair<size_t, size_t>>> groups;
        if (joint) {
            for (size_t ri = 0; ri < runs[0].size(); ++ri) {
                groups.push_back({});
                for (size_t k = 0; k < nm; ++k) groups.back().push_back({k, ri});
            }
        } else {
            for (size_t k = 0; k < nm; ++k)
                for (size_t ri = 0; ri < runs[k].size(); ++ri) groups.push_back({{k, ri}});
        }
        for (const auto& g : groups) {
            std::vector<std::vector<Vec2>> pts;
            std::vector<Track> protos;
            for (const auto& [k, ri] : g) {
                const auto [from, to] = runs[k][ri];
                std::vector<Vec2> p{per[k][from].a};
                for (size_t t = from; t < to; ++t) p.push_back(per[k][t].b);
                pts.push_back(p);
                protos.push_back(per[k][from]);
            }
            const bool single = per[g[0].first][runs[g[0].first][g[0].second].first].arc;
            std::vector<std::vector<Track>> res;
            std::vector<std::vector<size_t>> vos;
            if (r > 0 && !single) {
                auto clearArc = [&](size_t gk, const Track& arc) {
                    return !trackHits(w, nets, protos[gk].layer, arc, protos[gk].width / 2, nullptr);
                };
                res = filletArcRunsChecked(pts, protos, r, std::max(protos[0].width, 0.05), clearArc,
                                           [&](int a, int b) { return base->clearance(a, b); }, &vos);
            } else {  // an arc already: kept as it is
                res.assign(pts.size(), {});
                vos.assign(pts.size(), {});
                for (size_t gk = 0; gk < pts.size(); ++gk) {
                    const auto [from, to] = runs[g[gk].first][g[gk].second];
                    res[gk].assign(per[g[gk].first].begin() + static_cast<long>(from),
                                   per[g[gk].first].begin() + static_cast<long>(to));
                    for (size_t v = 0; v < pts[gk].size(); ++v) vos[gk].push_back(v);
                }
            }
            for (size_t gk = 0; gk < g.size(); ++gk) {
                const auto [k, ri] = g[gk];
                const auto [from, to] = runs[k][ri];
                for (size_t t = 0; t < res[gk].size(); ++t) {
                    if (headOut && (*headOut)[k] == none && headFrom[k] >= from && headFrom[k] < to &&
                        t >= vos[gk][headFrom[k] - from])
                        (*headOut)[k] = out[k].size();
                    out[k].push_back(res[gk][t]);
                }
            }
        }
        if (headOut)
            for (size_t k = 0; k < nm; ++k)
                if ((*headOut)[k] == none) (*headOut)[k] = out[k].size();
        return out;
    }

    /// Lays the head lines into `w` as fixed copper and makes room for them (shove) or checks them (walkaround).
    bool placeHead(World& w, const std::vector<HeadLine>& lines, bool shove, std::string& why) const {
        const std::vector<size_t> added = addHeadLines(w, lines);
        if (shove) {
            Shover sh(w, opt);
            return sh.run(added, {}, why);
        }
        for (size_t i : added) {
            const Track& t = w.track(i);
            std::vector<Hit> hits;
            if (segmentHits(w, {t.net}, t.layer, t.a, t.b, t.width / 2, &hits)) {
                why = "Blocked by " + describeHit(w, hits.front());
                return false;
            }
        }
        if (!newAcuteJoins(w).empty()) {
            why = "The head would make an acute corner here";
            return false;
        }
        return true;
    }

    /// Tries the candidate centre / head paths; returns the first that fits (with its world), else the longest
    /// prefix that fits. `build` turns a path into head lines.
    void solveHead(const std::vector<std::vector<Vec2>>& candidates, const std::function<GridPath()>& walkSearch,
                   const std::function<std::vector<HeadLine>(const std::vector<Vec2>&, bool)>& build, Vec2 target,
                   std::vector<Vec2>& chosen) {
        const bool shove = opt.mode == RouterMode::Shove;
        std::string why, firstWhy;
        blocked = false;
        if (highlight()) {
            // Straight where pointed: nothing moves, nothing stops the head; collisions are listed in the preview.
            std::vector<Vec2> c = candidates.empty() ? std::vector<Vec2>{} : candidates.front();
            if (c.size() < 2) c = walkSearch().pts;
            World w = committed;
            if (c.size() >= 2) addHeadLines(w, build(c, true));
            current = w;
            chosen = c.size() >= 2 ? c : std::vector<Vec2>{};
            return;
        }
        // The candidates are tried side by side (with spare cores); the first in order that fits wins, exactly as
        // one after the other would.
        struct Try {
            World w;
            bool ok = false;
            bool ran = false;
            std::string why;
        };
        std::vector<Try> tries(candidates.size());
        auto attempt = [&](size_t k) {
            tries[k].w = committed;
            tries[k].ok = placeHead(tries[k].w, build(candidates[k], true), shove, tries[k].why);
            tries[k].ran = true;
        };
        if (Parallel::enabled() && candidates.size() > 1) {
            std::vector<std::function<void()>> jobs;
            for (size_t k = 0; k < candidates.size(); ++k) jobs.push_back([&, k] { attempt(k); });
            Parallel::run(jobs);
        } else {
            for (size_t k = 0; k < candidates.size(); ++k) {
                if (abortRequested()) return;
                attempt(k);
                if (tries[k].ok) break;
            }
        }
        if (abortRequested()) return;
        for (size_t k = 0; k < tries.size() && tries[k].ran; ++k) {
            if (tries[k].ok) {
                current = tries[k].w;
                chosen = candidates[k];
                return;
            }
            why = tries[k].why;
            if (firstWhy.empty()) firstWhy = why;
        }
        // Walk around what cannot be shoved (Stop: the head stops short instead).
        const GridPath walk = opt.mode == RouterMode::Stop ? GridPath{} : walkSearch();
        const std::vector<Vec2>& walkPath = walk.pts;
        if (walk.reached && walkPath.size() >= 2) {
            World w = committed;
            if (placeHead(w, build(walkPath, true), false, why)) {
                current = w;
                chosen = walkPath;
                status = shove ? "Walking around — " + firstWhy : "Walking around obstacles";
                return;
            }
        }
        // Stop short: the longest fitting prefix of the preferred path, or the partial walkaround, whichever gets
        // closer to the target.
        blocked = true;
        status = firstWhy.empty() ? "Blocked" : firstWhy;
        std::vector<Vec2> bestPath;
        World bestWorld = committed;
        if (!candidates.empty()) {
            // Bisection for the longest prefix that fits. Two steps at a time: the middle and both quarter points
            // are tried side by side, then used as the sequential steps would (the same points, the same result).
            const auto& p = candidates.front();
            double lo = 0, hi = pathLength(p);
            bisect(
                lo, hi, 12, [](double a, double b) { return b - a > 1e-3; },
                [&](Probe& pr) {
                    const auto q = pathPrefix(p, pr.at);
                    pr.w = committed;
                    pr.ok = q.size() >= 2 && placeHead(pr.w, build(q, false), shove, pr.why);
                },
                [&](Probe& pr) {
                    if (!pr.ok) return;
                    bestPath = pathPrefix(p, pr.at);
                    bestWorld = std::move(pr.w);
                });
        }
        if (walkPath.size() >= 2) {
            const double dw = (walkPath.back() - target).length();
            const double db = bestPath.size() >= 2 ? (bestPath.back() - target).length() : 1e18;
            if (dw < db - 1e-6) {
                World w = committed;
                if (placeHead(w, build(walkPath, false), false, why)) {
                    bestPath = walkPath;
                    bestWorld = w;
                }
            }
        }
        if (bestPath.size() >= 2) {
            current = bestWorld;
            chosen = bestPath;
        } else {
            current = committed;
            chosen.clear();
        }
    }

    void routeHead(Vec2 cursor) {
        Member& m = members[0];
        bool hit = false;
        const Vec2 target = snapTarget(cursor, m, hit);
        current = committed;
        m.head.clear();
        reached = false;
        blocked = false;
        status = "Routing " + base->netName(m.net);
        if ((target - m.end).length() < 1e-6) {
            reached = hit && !m.placed.empty();
            return;
        }
        const auto links = orderedLinks(m.end, target);
        auto build = [&](const std::vector<Vec2>& p, bool) { return std::vector<HeadLine>{{m.net, p}}; };
        const Vec2 from = m.end;
        auto walk = [&] { return gridRoute(committed, {m.net}, layer, width / 2, from, target, opt.posture, lastDir); };
        solveHead(links, walk, build, target, m.head);
        reached = hit && !m.head.empty() && samePoint(m.head.back(), target, 1e-6);
    }

    /// The pair's member lines for a centre path (with the links from the members' ends and, at a target, to the
    /// target pads).
    std::vector<HeadLine> pairLines(const std::vector<Vec2>& c, const Vec2* targets) {
        std::vector<HeadLine> out;
        if (c.size() < 2) return out;
        int sd = side;
        if (sd == 0) {
            const double cr = cross(c[1] - c[0], members[0].end - centre);
            sd = cr >= 0 ? 1 : -1;  // y-down: the left normal (-dy, dx) points to positive cross
        }
        for (size_t k = 0; k < 2; ++k) {
            const double d = (k == 0 ? sd : -sd) * spacing / 2;
            std::vector<Vec2> off = offsetPath(c, d);
            std::vector<Vec2> pts{members[k].end};
            const Vec2 u = unit(lastDir), v = unit(c[1] - c[0]);
            bool trim = false;
            if (!members[k].placed.empty() && lastDir.length() > 0 && std::fabs(cross(u, v)) > 1e-9 && u.dot(v) > 0) {
                // A turn at a placed corner: each member turns where its last line meets its new line (the miter),
                // so the gap stays exact through the corner. The inner member's last track ends at its miter instead
                // (trimmed in the overlay by addHeadLines, and in the placed route by trimTo).
                const double s = cross(off.front() - members[k].end, v) / cross(u, v);
                const Vec2 miter = members[k].end + u * s;
                if (s < -1e-9 && -s < trackLength(members[k].placed.back()) - 1e-9) {
                    pts = {miter};
                    trim = true;
                } else if (s > 1e-9) {
                    pts.push_back(miter);
                }
            } else {
                auto lead = postureLinks(members[k].end, off.front(), RoutePosture::Diagonal45, false);
                pts.insert(pts.end(), lead.front().begin() + 1, lead.front().end());
            }
            pts.insert(pts.end(), off.begin() + 1, off.end());
            if (targets) {
                auto tail = postureLinks(off.back(), targets[k], RoutePosture::Diagonal45, false);
                pts.insert(pts.end(), tail.front().begin() + 1, tail.front().end());
            }
            out.push_back({members[k].net, simplifyPath(pts), trim, members[k].end});
        }
        return out;
    }

    // ---------------------------------------------------------------------------------------------------- bus

    bool beginBus(Vec2 at, int l, int count) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        count = std::clamp(count, 2, 16);
        if (l < 0 || l >= std::max(1, base->s.layerCount)) return bail("That copper layer is not in the stack-up");
        StartHit st;
        if (!findStart(at, l, st) || st.pad < 0) return bail("Start a bus on a pad of a part");
        if (st.layer >= 0) l = st.layer;
        const Pad& sp = base->pads[static_cast<size_t>(st.pad)];
        auto it = base->compPads.find(sp.componentId);
        if (it == base->compPads.end()) return bail("Start a bus on a pad of a part");
        // The row: the part's pads on this layer in line with the clicked one (along the nearest neighbour).
        long nearest = -1;
        double nd = 1e18;
        for (size_t q : it->second) {
            const Pad& o = base->pads[q];
            if (static_cast<long>(q) == st.pad || !o.onLayer(l)) continue;
            const double d = (o.position - sp.position).length();
            if (d < nd) {
                nd = d;
                nearest = static_cast<long>(q);
            }
        }
        if (nearest < 0) return bail("The part has no other pad for a bus");
        const Vec2 axis = std::fabs(base->pads[static_cast<size_t>(nearest)].position.x - sp.position.x) >=
                                  std::fabs(base->pads[static_cast<size_t>(nearest)].position.y - sp.position.y)
                              ? Vec2{1, 0}
                              : Vec2{0, 1};
        const Vec2 across{-axis.y, axis.x};
        const double tol = std::max(0.05, 0.25 * std::min(sp.size.x, sp.size.y));
        std::vector<size_t> row;
        for (size_t q : it->second) {
            const Pad& o = base->pads[q];
            // Pads whose net goes somewhere (an unconnected pin's one-pin net has nothing to route to).
            const bool wired = o.net >= 0 && static_cast<size_t>(o.net) < base->pinCount.size() &&
                               base->pinCount[static_cast<size_t>(o.net)] >= 2;
            if (o.onLayer(l) && wired &&
                std::fabs((o.position - sp.position).dot(across)) <= tol)
                row.push_back(q);
        }
        std::sort(row.begin(), row.end(), [&](size_t a, size_t b) {
            return base->pads[a].position.dot(axis) < base->pads[b].position.dot(axis);
        });
        const long at0 = std::find(row.begin(), row.end(), static_cast<size_t>(st.pad)) - row.begin();
        if (at0 >= static_cast<long>(row.size())) return bail("That pad's net connects to nothing else");
        // From the clicked pad onwards along the row, then back the other way if the row ends first.
        std::vector<size_t> pick;
        std::vector<int> nets;
        auto take = [&](size_t q) {
            const int n = base->pads[q].net;
            if (static_cast<int>(pick.size()) >= count || std::find(nets.begin(), nets.end(), n) != nets.end()) return;
            pick.push_back(q);
            nets.push_back(n);
        };
        for (long k = at0; k < static_cast<long>(row.size()); ++k) take(row[static_cast<size_t>(k)]);
        for (long k = at0 - 1; k >= 0; --k) take(row[static_cast<size_t>(k)]);
        if (pick.size() < 2) return bail("No other pad with a net next to it in its row");
        if (!layerUsable(l, nets)) {
            base.reset();
            return false;
        }
        layer = l;
        width = 0;
        double clr = 0;
        for (int n : nets) width = std::max(width, netWidth(n));
        for (int a : nets)
            for (int b : nets)
                if (a != b) clr = std::max(clr, base->clearance(a, b));
        spacing = width + clr;  // centre to centre
        members.clear();
        centre = {};
        for (size_t q : pick) {
            Member m;
            m.net = base->pads[q].net;
            m.end = base->pads[q].position;
            m.startPad = static_cast<long>(q);
            members.push_back(m);
            centre = centre + m.end * (1.0 / static_cast<double>(pick.size()));
        }
        busAcross = across;
        busPadHalf = 0;
        for (size_t q : pick) busPadHalf = std::max(busPadHalf, std::fabs(base->pads[q].size.dot(across)) / 2);
        kind = Kind::Bus;
        startSession(nets);
        status = "Routing a bus of " + std::to_string(members.size()) + " nets";
        buildPreview();
        return true;
    }

    /// Each member's offset left of a centre line leaving in direction `dir`: members keep their order across the
    /// bundle (by where they start), `spacing` apart. Fixed once the first corner is placed.
    std::vector<double> busOffsets(Vec2 dir) const {
        if (!busOffset.empty()) return busOffset;
        const Vec2 nrm = leftNormal(dir);
        std::vector<size_t> order(members.size());
        for (size_t k = 0; k < order.size(); ++k) order[k] = k;
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return (members[a].end - centre).dot(nrm) < (members[b].end - centre).dot(nrm); });
        std::vector<double> off(members.size());
        const double mid = (static_cast<double>(members.size()) - 1) / 2;
        for (size_t r = 0; r < order.size(); ++r) off[order[r]] = (static_cast<double>(r) - mid) * spacing;
        return off;
    }

    std::vector<HeadLine> busLines(const std::vector<Vec2>& c) const {
        std::vector<HeadLine> out;
        if (c.size() < 2) return out;
        const std::vector<double> off = busOffsets(c[1] - c[0]);
        for (size_t k = 0; k < members.size(); ++k) {
            const std::vector<Vec2> o = offsetPath(c, off[k]);
            std::vector<Vec2> pts{members[k].end};
            if (busFanOut.length() > 0) {
                // Fan-in: straight out of the pad past the row, 45° to the member's lane, straight into the bundle.
                const Vec2 nrm = leftNormal(busFanOut);
                const Vec2 p = members[k].end;
                const Vec2 e = p + busFanOut * (busPadHalf + base->s.clearance + width / 2);
                const double across = (o.front() - e).dot(nrm);
                const Vec2 d = e + busFanOut * std::fabs(across) + nrm * across;
                pts.push_back(e);
                pts.push_back(d);
                pts.push_back(o.front());
            } else {
                auto lead = postureLinks(members[k].end, o.front(), RoutePosture::Diagonal45, false);
                pts.insert(pts.end(), lead.front().begin() + 1, lead.front().end());
            }
            pts.insert(pts.end(), o.begin() + 1, o.end());
            out.push_back({members[k].net, simplifyPath(pts)});
        }
        return out;
    }

    void busHead(Vec2 cursor) {
        current = committed;
        centreHead.clear();
        for (auto& m : members) m.head.clear();
        reached = false;
        blocked = false;
        status = "Routing a bus of " + std::to_string(members.size()) + " nets";
        if ((cursor - centre).length() < 1e-6) return;
        std::vector<int> nets;
        for (const auto& m : members) nets.push_back(m.net);
        auto build = [&](const std::vector<Vec2>& c, bool) { return busLines(c); };
        const double half = (static_cast<double>(members.size()) - 1) / 2 * spacing + width / 2;
        // Before the first corner the bundle starts clear of the pad row, on the cursor's side: each pad leaves
        // straight out and fans in at 45° to its place in the bundle without crossing its neighbours.
        Vec2 from = centre;
        Vec2 dir = lastDir;
        if (!busOffset.empty() && lastDir.length() <= 0) {
            // Just after the bundle's vias: each member leaves its via and joins its lane at 45°, the lanes start
            // beyond the vias on the cursor's side (along the nearest of the eight directions).
            const Vec2 want = unit(cursor - centre);
            Vec2 out = kDirs[0];
            for (const Vec2& d8 : kDirs)
                if (d8.dot(want) > out.dot(want)) out = d8;
            const Vec2 nrm = leftNormal(out);
            double ahead = 0, viaR = 0;
            for (const Via& v : placedVias) viaR = std::max(viaR, v.diameter / 2);
            for (size_t k = 0; k < members.size(); ++k) {
                const Vec2 q = members[k].end - centre;
                ahead = std::max(ahead, q.dot(out) + std::fabs(busOffset[k] - q.dot(nrm)));
            }
            from = centre + out * (ahead + viaR + base->s.clearance + width);
            dir = out;
            busFanOut = {};
        } else if (busOffset.empty() && multiRoute) {
            // Multi-route from anywhere: the bundle leaves towards the cursor along the nearest of the eight
            // directions, starting beyond every start point far enough for each member's 45° lead into its lane.
            const Vec2 want = unit(cursor - centre);
            Vec2 out = kDirs[0];
            for (const Vec2& d8 : kDirs)
                if (d8.dot(want) > out.dot(want)) out = d8;
            const std::vector<double> off = busOffsets(out);
            const Vec2 nrm = leftNormal(out);
            double ahead = 0;
            for (size_t k = 0; k < members.size(); ++k) {
                const Vec2 q = members[k].end - centre;
                ahead = std::max(ahead, q.dot(out) + std::fabs(off[k] - q.dot(nrm)));
            }
            from = centre + out * (ahead + busPadHalf + base->s.clearance + width);
            dir = out;
            busFanOut = {};
        } else if (busOffset.empty()) {
            const Vec2 out = (cursor - centre).dot(busAcross) >= 0 ? busAcross : busAcross * -1.0;
            const std::vector<double> off = busOffsets(out);
            double fan = 0;
            for (size_t k = 0; k < members.size(); ++k) {
                const Vec2 q = centre + leftNormal(out) * off[k];
                fan = std::max(fan, std::fabs((q - members[k].end).dot(leftNormal(out))));
            }
            from = centre + out * (fan + busPadHalf + base->s.clearance + width);
            dir = out;
            busFanOut = out;
        } else {
            busFanOut = {};
        }
        // After a placed corner the members end on the line across the centre: the bundle runs on straight by its
        // half width before it may turn, so every member turns at its own miter point without doubling back.
        bool prefix = false;
        if (!busOffset.empty() && lastDir.length() > 0) {
            double maxO = 0;
            for (double o : busOffset) maxO = std::max(maxO, std::fabs(o));
            from = centre + unit(lastDir) * maxO;
            prefix = maxO > 1e-9;
        }
        if ((cursor - from).length() < 1e-6) return;
        auto withPrefix = [&](std::vector<Vec2> c) {
            if (prefix && !c.empty()) c.insert(c.begin(), centre);
            return simplifyPath(c);
        };
        auto walk = [&] {
            GridPath g = gridRoute(committed, nets, layer, half, from, cursor, opt.posture, dir);
            if (g.pts.size() >= 2) g.pts = withPrefix(g.pts);
            return g;
        };
        std::vector<Vec2> chosen;
        std::vector<std::vector<Vec2>> links;
        for (auto& l : postureLinks(from, cursor, opt.posture, opt.swapPosture))
            if (!(dir.length() > 0 && l.size() >= 2 && acuteJoin(dir, l[1] - l[0]))) links.push_back(withPrefix(l));
        solveHead(links, walk, build, cursor, chosen);
        if (chosen.size() < 2) return;
        centreHead = chosen;
        const auto lines = busLines(chosen);
        for (size_t k = 0; k < members.size() && k < lines.size(); ++k) members[k].head = lines[k].pts;
    }

    void pairHead(Vec2 cursor) {
        current = committed;
        centreHead.clear();
        for (auto& m : members) m.head.clear();
        reached = false;
        blocked = false;
        status = "Routing pair " + base->netName(members[0].net) + " / " + base->netName(members[1].net);
        // Target: a pad of either member under the cursor, with the nearest pad of the other member.
        Vec2 target = cursor;
        Vec2 targets[2];
        bool atPads = false;
        if (opt.snapToPads) {
            for (size_t i = 0; i < base->pads.size() && !atPads; ++i) {
                const Pad& p = base->pads[i];
                if ((p.net != members[0].net && p.net != members[1].net) || !p.onLayer(layer) || padDistance(p, cursor) > 0.05)
                    continue;
                if (static_cast<long>(i) == members[0].startPad || static_cast<long>(i) == members[1].startPad) continue;
                const size_t k = p.net == members[0].net ? 0 : 1;
                long partner = -1;
                double bestD = 1e9;
                for (size_t j = 0; j < base->pads.size(); ++j) {
                    const Pad& q = base->pads[j];
                    if (q.net != members[1 - k].net || !q.onLayer(layer)) continue;
                    const double d = (q.position - p.position).length() + (q.componentId == p.componentId ? 0 : 1000);
                    if (d < bestD) {
                        bestD = d;
                        partner = static_cast<long>(j);
                    }
                }
                if (partner < 0) continue;
                targets[k] = p.position;
                targets[1 - k] = base->pads[static_cast<size_t>(partner)].position;
                target = (targets[0] + targets[1]) * 0.5;
                atPads = true;
            }
        }
        if ((target - centre).length() < 1e-6) return;
        // Approach the target pads square to the line between them.
        std::vector<std::vector<Vec2>> cands = orderedLinks(centre, target);
        auto build = [&](const std::vector<Vec2>& c, bool full) { return pairLines(c, full && atPads ? targets : nullptr); };
        auto walk = [&] {
            return gridRoute(committed, {members[0].net, members[1].net}, layer, spacing / 2 + width / 2, centre, target,
                             opt.posture, lastDir);
        };
        std::vector<Vec2> chosen;
        solveHead(cands, walk, build, target, chosen);
        if (chosen.size() < 2) return;
        centreHead = chosen;
        const bool full = !blocked && samePoint(chosen.back(), target, 1e-6);
        const auto lines = pairLines(chosen, full && atPads ? targets : nullptr);
        for (size_t k = 0; k < 2 && k < lines.size(); ++k) members[k].head = lines[k].pts;
        reached = atPads && full;
    }

    // --------------------------------------------------------------------------------------------------- drag

    bool beginDrag(int trackId, Vec2 grab) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        long ti = -1;
        for (size_t i = 0; i < base->tracks.size(); ++i)
            if (base->tracks[i].id == trackId) ti = static_cast<long>(i);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        if (ti < 0) return bail("No track with that id");
        const Track t = base->tracks[static_cast<size_t>(ti)];
        if (base->tracks[static_cast<size_t>(ti)].arc) return bail("Arcs are not dragged: drag a straight track next to it");
        if (base->trackFixed[static_cast<size_t>(ti)]) return bail("The track is locked");
        if (t.net < 0) return bail("The track has no net");
        if ((t.b - t.a).length() < 1e-6) return bail("The track has no length");
        startSession({t.net});
        layer = t.layer;
        width = t.width;
        drag.track = static_cast<size_t>(ti);
        drag.a = t.a;
        drag.b = t.b;
        drag.grab = grab;
        drag.dir = unit(t.b - t.a);
        drag.normal = Vec2{-drag.dir.y, drag.dir.x};
        committed.goneT[drag.track] = 1;
        for (int e = 0; e < 2; ++e) {
            const Vec2 p = e == 0 ? t.a : t.b;
            long via = -1;
            bool anchored = nodeAnchored(committed, t.net, t.layer, p, &via);
            Vec2 anchor = p;
            if (!anchored) {
                const auto at = tracksEndingAt(committed, t.net, t.layer, p, drag.track);
                if (at.size() == 1 && !base->trackFixed[at[0]] && std::fabs(committed.track(at[0]).width - t.width) < 1e-9) {
                    const Track& n = committed.track(at[0]);
                    anchor = samePoint(n.a, p) ? n.b : n.a;
                    committed.goneT[at[0]] = 1;
                    // The track beyond the anchor: the dragged legs must not fold back onto it (acute corner).
                    const auto beyond = tracksEndingAt(committed, t.net, t.layer, anchor, at[0]);
                    if (beyond.size() == 1) {
                        const Track& f = committed.track(beyond[0]);
                        const Vec2 far = samePoint(f.a, anchor) ? f.b : f.a;
                        (e == 0 ? drag.intoA : drag.outOfB) = e == 0 ? anchor - far : far - anchor;
                    }
                } else {
                    anchored = true;
                }
            }
            (e == 0 ? drag.anchorA : drag.anchorB) = anchor;
            (e == 0 ? drag.anchoredA : drag.anchoredB) = anchored;
        }
        Member m;
        m.net = t.net;
        m.end = drag.anchorA;
        members = {m};
        current = committed;
        kind = Kind::Drag;
        status = "Dragging a track of " + base->netName(t.net);
        computeHead(grab);
        return true;
    }

    std::vector<Vec2> dragPath(double delta) const {
        const Vec2 off = drag.normal * delta;
        const double len = (drag.b - drag.a).length();
        const double ad = std::fabs(delta);
        std::vector<Vec2> pts;
        Vec2 a1 = drag.a + off, b1 = drag.b + off;
        if (ad < 1e-9) return {drag.anchorA, drag.a, drag.b, drag.anchorB};
        // Anchored ends meet the moved segment with 45° legs where the segment is long enough.
        if (drag.anchoredA) {
            pts.push_back(drag.a);
            if (2 * ad < len && opt.posture == RoutePosture::Diagonal45) a1 = a1 + drag.dir * ad;
        } else {
            pts.push_back(drag.anchorA);
            auto links = postureLinks(drag.anchorA, a1, opt.posture, false);
            std::vector<Vec2> pick = links.front();
            for (const auto& l : links)
                if (l.size() < 2 || (!acuteJoin(l.back() - l[l.size() - 2], drag.dir) && !acuteJoin(drag.intoA, l[1] - l[0]))) {
                    pick = l;
                    break;
                }
            pts.insert(pts.end(), pick.begin() + 1, pick.end() - 1);
        }
        pts.push_back(a1);
        if (drag.anchoredB) {
            if (2 * ad < len && opt.posture == RoutePosture::Diagonal45) b1 = b1 - drag.dir * ad;
            pts.push_back(b1);
            pts.push_back(drag.b);
        } else {
            pts.push_back(b1);
            auto links = postureLinks(b1, drag.anchorB, opt.posture, false);
            std::vector<Vec2> pick = links.front();
            for (const auto& l : links)
                if (l.size() < 2 || (!acuteJoin(drag.dir, l[1] - l[0]) && !acuteJoin(l.back() - l[l.size() - 2], drag.outOfB))) {
                    pick = l;
                    break;
                }
            pts.insert(pts.end(), pick.begin() + 1, pick.end());
        }
        return simplifyPath(pts);
    }

    /// The path bent around the fixed copper in its way (each obstacle's octagonal clearance hull, the shorter way
    /// round), as the shove engine walks a pushed line around a pad. Empty when an obstacle cannot be passed.
    std::vector<Vec2> hugPath(std::vector<Vec2> pts, int net, double hw) const {
        std::vector<Hit> hits;
        for (int round = 0; round < 6; ++round) {
            bool found = false;
            Hit h;
            for (size_t k = 0; k + 1 < pts.size() && !found; ++k) {
                hits.clear();
                segmentHits(committed, {net}, layer, pts[k], pts[k + 1], hw, &hits);
                for (const Hit& x : hits)
                    if (x.fixed && (x.kind == HitKind::Pad || x.kind == HitKind::Track || x.kind == HitKind::Via)) {
                        h = x;
                        found = true;
                        break;
                    }
            }
            if (!found) return pts;
            Shape s;
            int on = -1;
            bool barrier = false;
            if (h.kind == HitKind::Pad) {
                const Pad& p = base->pads[h.index];
                s = padShape(p);
                on = p.net;
                barrier = base->barrierComps.count(p.componentId) > 0;
            } else if (h.kind == HitKind::Track) {
                s = trackShape(committed.track(h.index));
                on = committed.track(h.index).net;
            } else {
                s = viaShape(committed.via(h.index));
                on = committed.via(h.index).net;
            }
            const Octagon hull = makeOctagon(s.points(), s.radius + base->clearance(net, on, barrier) + hw + 1e-4);
            std::vector<Vec2> c1, c2;
            const bool ok1 = walkAround(pts, hull, true, c1), ok2 = walkAround(pts, hull, false, c2);
            if (!ok1 && !ok2) return {};
            pts = simplifyPath(ok1 && ok2 ? (pathLength(c1) <= pathLength(c2) ? c1 : c2) : (ok1 ? c1 : c2));
        }
        return {};
    }

    void dragHead(Vec2 cursor) {
        Member& m = members[0];
        const double delta = (cursor - drag.grab).dot(drag.normal);
        const bool shove = opt.mode == RouterMode::Shove;
        std::string why;
        blocked = false;
        status = "Dragging a track of " + base->netName(m.net);
        auto tryDelta = [&](double d, World& w, std::string& whyOut) {
            return placeHead(w, {{m.net, dragPath(d)}}, shove, whyOut);
        };
        World w = committed;
        if (highlight()) {
            addHeadLines(w, {{m.net, dragPath(delta)}});
            current = w;
            m.head = dragPath(delta);
            return;
        }
        // The full move and (in case it does not fit) the start run side by side.
        World w0 = committed;
        bool full = false, atStart = false;
        std::string why0;
        if (Parallel::enabled())
            Parallel::run({[&] { full = tryDelta(delta, w, why); }, [&] { atStart = tryDelta(0, w0, why0); }});
        else
            full = tryDelta(delta, w, why);
        if (full) {
            current = w;
            m.head = dragPath(delta);
            return;
        }
        if (opt.hugDrag) {
            // Hug: bend around the pad (or other fixed copper) in the way instead of stopping short.
            const std::vector<Vec2> hug = hugPath(dragPath(delta), m.net, width / 2);
            if (hug.size() >= 2) {
                World wh = committed;
                std::string whyHug;
                if (placeHead(wh, {{m.net, hug}}, shove, whyHug) && newAcuteJoins(wh).empty()) {
                    current = wh;
                    m.head = hug;
                    status = "Dragging a track of " + base->netName(m.net) + " — hugging an obstacle";
                    return;
                }
            }
        }
        blocked = true;
        status = why;
        double lo = 0, hi = delta;
        World best = committed;
        bool any = false;
        if (!Parallel::enabled()) atStart = tryDelta(0, w0, why0);
        if (atStart) {
            best = w0;
            any = true;
        }
        bisect(
            lo, hi, 14, [](double a, double b) { return std::fabs(b - a) > 1e-3; },
            [&](Probe& pr) {
                pr.w = committed;
                pr.ok = tryDelta(pr.at, pr.w, pr.why);
            },
            [&](Probe& pr) {
                if (!pr.ok) return;
                best = std::move(pr.w);
                any = true;
            });
        current = any ? best : committed;
        m.head = any ? dragPath(lo) : std::vector<Vec2>{};
    }

    // ------------------------------------------------------------------------------ corner and multi-track drag

    bool beginCornerDrag(int trackId, Vec2 grab) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        long ti = -1;
        for (size_t i = 0; i < base->tracks.size(); ++i)
            if (base->tracks[i].id == trackId) ti = static_cast<long>(i);
        if (ti < 0) return bail("No track with that id");
        const Track t = base->tracks[static_cast<size_t>(ti)];
        if (t.arc) return bail("Arcs are not dragged: drag a straight track next to it");
        if (base->trackFixed[static_cast<size_t>(ti)]) return bail("The track is locked");
        if (t.net < 0) return bail("The track has no net");
        const Vec2 v = (grab - t.a).length() <= (grab - t.b).length() ? t.a : t.b;
        startSession({t.net});
        layer = t.layer;
        width = t.width;
        long via = -1;
        if (nodeAnchored(committed, t.net, t.layer, v, &via)) return bail("That corner is on a pad or via: drag the track or the via");
        const auto at = tracksEndingAt(committed, t.net, t.layer, v, static_cast<size_t>(ti));
        if (at.size() != 1) return bail("Drag a corner where exactly two tracks meet");
        const Track& o = committed.track(at[0]);
        if (o.arc || base->trackFixed[at[0]] || std::fabs(o.width - t.width) > 1e-9)
            return bail("The other track at the corner is locked, an arc or of another width");
        // The line through the corner, split there: two lines starting at the corner.
        World plain(base.get());  // without the session's fixed nets, so the line runs on through the corner
        Line L = extractLine(plain, static_cast<size_t>(ti));
        size_t iv = L.pts.size();
        for (size_t k = 0; k < L.pts.size(); ++k)
            if (samePoint(L.pts[k], v, 1e-9)) iv = k;
        if (iv == L.pts.size() || iv == 0 || iv + 1 == L.pts.size()) return bail("Drag a corner where exactly two tracks meet");
        Line back = L, fwd = L;
        back.pts.assign(L.pts.begin(), L.pts.begin() + static_cast<long>(iv) + 1);
        std::reverse(back.pts.begin(), back.pts.end());
        back.segs.assign(L.segs.begin(), L.segs.begin() + static_cast<long>(iv));
        std::reverse(back.segs.begin(), back.segs.end());
        back.viaAt[0] = -1;
        back.viaAt[1] = L.viaAt[0];
        fwd.pts.assign(L.pts.begin() + static_cast<long>(iv), L.pts.end());
        fwd.segs.assign(L.segs.begin() + static_cast<long>(iv), L.segs.end());
        fwd.viaAt[0] = -1;
        for (size_t s : L.segs) committed.goneT[s] = 1;
        MultiDrag::End e;
        e.p = v;
        e.net = t.net;
        e.layer = t.layer;
        e.width = t.width;
        e.lines = {back, fwd};
        mdrag.ends = {e};
        mdrag.grab = grab;
        mdrag.corner = true;
        Member m;
        m.net = t.net;
        m.end = v;
        members = {m};
        current = committed;
        kind = Kind::DragMulti;
        status = "Dragging a corner of " + base->netName(t.net);
        computeHead(grab);
        return true;
    }

    bool beginMultiDrag(const std::vector<int>& trackIds, Vec2 grab) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        std::map<int, size_t> byId;
        for (size_t i = 0; i < base->tracks.size(); ++i) byId[base->tracks[i].id] = i;
        std::vector<size_t> sel;
        std::vector<int> nets;
        for (int id : trackIds) {
            auto it = byId.find(id);
            if (it == byId.end() || std::find(sel.begin(), sel.end(), it->second) != sel.end()) continue;
            const Track& t = base->tracks[it->second];
            if (t.arc) return bail("Arcs are not dragged: leave them out of the selection");
            if (base->trackFixed[it->second]) return bail("A selected track is locked");
            if (t.net < 0) return bail("A selected track has no net");
            sel.push_back(it->second);
            if (std::find(nets.begin(), nets.end(), t.net) == nets.end()) nets.push_back(t.net);
        }
        if (sel.empty()) return bail("Select the tracks to drag");
        startSession(nets);
        World plain(base.get());  // lines are traced without the session's fixed nets
        for (size_t s : sel) {
            committed.goneT[s] = 1;
            plain.goneT[s] = 1;
        }
        // Ends of the selection: shared by two dragged tracks (they just move), or a boundary end.
        auto sharedEnd = [&](size_t self, Vec2 p) {
            const Track& t = base->tracks[self];
            for (size_t o : sel)
                if (o != self && base->tracks[o].net == t.net && base->tracks[o].layer == t.layer &&
                    (samePoint(base->tracks[o].a, p) || samePoint(base->tracks[o].b, p)))
                    return true;
            return false;
        };
        std::vector<std::vector<size_t>> taken;
        for (size_t s : sel) {
            const Track& t = base->tracks[s];
            for (Vec2 p : {t.a, t.b}) {
                if (sharedEnd(s, p)) continue;
                bool dup = false;
                for (const auto& e : mdrag.ends) dup = dup || (e.net == t.net && e.layer == t.layer && samePoint(e.p, p));
                if (dup) continue;
                MultiDrag::End e;
                e.p = p;
                e.net = t.net;
                e.layer = t.layer;
                e.width = t.width;
                long via = -1;
                e.anchored = nodeAnchored(committed, t.net, t.layer, p, &via);
                if (!e.anchored)
                    for (size_t o : tracksEndingAt(committed, t.net, t.layer, p, SIZE_MAX)) {
                        if (committed.trackFixed(o) || committed.track(o).arc) {
                            e.anchored = true;  // a locked track or an arc holds the end: link to it
                            continue;
                        }
                        Line L = extractLine(plain, o);
                        if (!samePoint(L.pts.front(), p)) {
                            std::reverse(L.pts.begin(), L.pts.end());
                            std::reverse(L.segs.begin(), L.segs.end());
                            std::swap(L.viaAt[0], L.viaAt[1]);
                        }
                        if (!samePoint(L.pts.front(), p)) continue;
                        bool seen = false;
                        for (const auto& segs : taken) seen = seen || segs == L.segs;
                        if (seen) continue;
                        taken.push_back(L.segs);
                        for (size_t q : L.segs) committed.goneT[q] = 1;
                        e.lines.push_back(L);
                    }
                mdrag.ends.push_back(e);
            }
        }
        mdrag.sel = sel;
        mdrag.grab = grab;
        layer = base->tracks[sel.front()].layer;
        width = base->tracks[sel.front()].width;
        Member m;
        m.net = base->tracks[sel.front()].net;
        m.end = grab;
        members = {m};
        current = committed;
        kind = Kind::DragMulti;
        status = "Dragging " + std::to_string(sel.size()) + (sel.size() == 1 ? " track" : " tracks");
        computeHead(grab);
        return true;
    }

    /// The dragged copper at offset `d`: the selection moved, each boundary end joined back (links to anchors,
    /// lines rejoining their old paths with the posture, skipping a corner where that is shorter and not acute).
    std::vector<Track> multiTracks(Vec2 d) const {
        std::vector<Track> out;
        const RoutePosture posture = opt.posture;
        for (size_t s : mdrag.sel) {
            Track t = base->tracks[s];
            t.id = -1;
            t.a = t.a + d;
            t.b = t.b + d;
            out.push_back(t);
        }
        for (const auto& e : mdrag.ends) {
            const Vec2 q = e.p + d;
            if (e.anchored && (q - e.p).length() > 1e-9) {
                auto links = postureLinks(e.p, q, posture, false);
                for (const Track& t : toTracks(links.front(), e.net, e.layer, e.width)) out.push_back(t);
            }
            for (const Line& L : e.lines) {
                const std::vector<Vec2>& p = L.pts;
                std::vector<Vec2> best;
                double bestLen = std::numeric_limits<double>::max();
                for (size_t k = 1; k < p.size() && k <= 2; ++k)
                    for (const auto& link : postureLinks(q, p[k], posture, false)) {
                        std::vector<Vec2> path = link;
                        path.insert(path.end(), p.begin() + static_cast<long>(k) + 1, p.end());
                        path = simplifyPath(path);
                        bool acute = false;
                        for (size_t j = 1; j + 1 < path.size(); ++j)
                            acute = acute || acuteJoin(path[j] - path[j - 1], path[j + 1] - path[j]);
                        const double len = pathLength(path) + (acute ? 1e6 : 0.0);
                        if (len < bestLen - 1e-9) {
                            bestLen = len;
                            best = path;
                        }
                    }
                if (best.size() < 2) best = {q, p.back()};
                for (const Track& t : toTracks(best, L.net, L.layer, L.width)) out.push_back(t);
            }
        }
        return out;
    }

    bool placeMulti(World& w, Vec2 d, std::string& why) const {
        std::vector<size_t> added;
        for (const Track& t : multiTracks(d)) added.push_back(w.addTrack(t, true));
        if (highlight()) return true;
        if (opt.mode == RouterMode::Shove) {
            Shover sh(w, opt);
            return sh.run(added, {}, why);
        }
        for (size_t i : added) {
            const Track& t = w.track(i);
            std::vector<Hit> hits;
            if (trackHits(w, {t.net}, t.layer, t, t.width / 2, &hits)) {
                why = "Blocked by " + describeHit(w, hits.front());
                return false;
            }
        }
        if (!newAcuteJoins(w).empty()) {
            why = "The tracks would make an acute corner here";
            return false;
        }
        return true;
    }

    void multiDragHead(Vec2 cursor) {
        const Vec2 d = cursor - mdrag.grab;
        status = mdrag.corner ? "Dragging a corner" : "Dragging " + std::to_string(mdrag.sel.size()) + " tracks";
        blocked = false;
        std::string why;
        World w = committed;
        if (placeMulti(w, d, why)) {
            current = w;
            mdrag.offset = d;
            mdrag.tracks = multiTracks(d);
            mdrag.valid = true;
            return;
        }
        blocked = true;
        status = why;
        double lo = 0, hi = 1;
        World best = committed;
        bool any = false;
        {
            World w0 = committed;
            if (placeMulti(w0, {}, why)) {
                best = w0;
                any = true;
            }
        }
        const double dl = d.length();
        bisect(
            lo, hi, 14, [dl](double a, double b) { return (b - a) * dl > 1e-3; },
            [&](Probe& pr) {
                pr.w = committed;
                pr.ok = placeMulti(pr.w, d * pr.at, pr.why);
            },
            [&](Probe& pr) {
                if (!pr.ok) return;
                best = std::move(pr.w);
                any = true;
            });
        mdrag.valid = any;
        current = any ? best : committed;
        mdrag.offset = d * lo;
        mdrag.tracks = any ? multiTracks(mdrag.offset) : std::vector<Track>{};
    }

    // --------------------------------------------------------------------------------------------- multi-route

    bool beginMultiRoute(const std::vector<Vec2>& starts, int l) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        if (starts.size() < 2) return bail("Pick two or more pads, vias or tracks to route together");
        if (starts.size() > 16) return bail("At most 16 nets route together");
        if (l < 0 || l >= std::max(1, base->s.layerCount)) return bail("That copper layer is not in the stack-up");
        std::vector<StartHit> hits;
        std::vector<int> nets;
        int forced = -1;
        for (Vec2 at : starts) {
            StartHit st;
            if (!findStart(at, l, st)) return bail("Nothing with a net at one of the picked points");
            if (std::find(nets.begin(), nets.end(), st.net) != nets.end()) return bail("Pick each net once");
            if (st.layer >= 0) {
                if (forced >= 0 && forced != st.layer) return bail("The picked pads are on different sides of the board");
                forced = st.layer;
            }
            hits.push_back(st);
            nets.push_back(st.net);
        }
        if (forced >= 0) l = forced;
        if (!layerUsable(l, nets)) {
            base.reset();
            return false;
        }
        layer = l;
        width = 0;
        double clr = 0;
        for (int n : nets) width = std::max(width, netWidth(n));
        for (int a : nets)
            for (int b : nets)
                if (a != b) clr = std::max(clr, base->clearance(a, b));
        spacing = width + clr;
        members.clear();
        centre = {};
        for (const auto& st : hits) {
            Member m;
            m.net = st.net;
            m.end = st.point;
            m.startPad = st.pad;
            members.push_back(m);
            centre = centre + m.end * (1.0 / static_cast<double>(hits.size()));
        }
        // The bundle leaves across the starts' main direction (a principal axis of the points).
        double sxx = 0, syy = 0, sxy = 0;
        for (const auto& m : members) {
            const Vec2 q = m.end - centre;
            sxx += q.x * q.x;
            syy += q.y * q.y;
            sxy += q.x * q.y;
        }
        const double ang = 0.5 * std::atan2(2 * sxy, sxx - syy);
        const Vec2 axis{std::cos(ang), std::sin(ang)};
        busAcross = Vec2{-axis.y, axis.x};
        busPadHalf = 0;
        for (const auto& st : hits)
            if (st.pad >= 0)
                busPadHalf = std::max(busPadHalf, std::fabs(base->pads[static_cast<size_t>(st.pad)].size.dot(busAcross)) / 2);
        kind = Kind::Bus;
        multiRoute = true;
        startSession(nets);
        status = "Routing " + std::to_string(members.size()) + " nets together";
        buildPreview();
        return true;
    }

    // ----------------------------------------------------------------------------------------------- via drag

    bool beginViaDrag(int viaId, Vec2 grab) {
        reset();
        base = std::make_unique<Base>(pcb, sch);
        auto bail = [&](const std::string& why) {
            base.reset();
            return fail(why);
        };
        long vi = -1;
        for (size_t i = 0; i < base->vias.size(); ++i)
            if (base->vias[i].id == viaId) vi = static_cast<long>(i);
        if (vi < 0) return bail("No via with that id");
        const Via v = base->vias[static_cast<size_t>(vi)];
        if (v.net < 0) return bail("The via has no net");
        if (base->viaFixed[static_cast<size_t>(vi)]) return bail("The via sits in a pad of its net or on the tamper mesh");
        startSession({v.net});
        const size_t idx = static_cast<size_t>(vi);
        // Tracks ending on the via follow it; one running through it (or a locked one) holds it in place.
        const double r = v.diameter / 2;
        std::vector<size_t> found;
        committed.tracksIn(Rect::centered(v.position, v.diameter, v.diameter), found);
        std::vector<Line> lines;
        for (size_t i : found) {
            const Track& t = committed.track(i);
            if (t.net != v.net || !v.spans(t.layer)) continue;
            const bool aIn = (t.a - v.position).length() <= r, bIn = (t.b - v.position).length() <= r;
            if (!aIn && !bIn) {
                if (trackPointDistance(t, v.position) <= r) return bail("A track runs through the via");
                continue;
            }
            if (aIn && bIn) return bail("A short track inside the via's land holds it");
            if (base->trackFixed[i]) return bail(base->tracks[i].arc ? "An arc ends on the via" : "A locked track ends on the via");
            Line L = extractLine(committed, i);
            if ((L.pts.back() - v.position).length() <= r && (L.pts.front() - v.position).length() > r) {
                std::reverse(L.pts.begin(), L.pts.end());
                std::reverse(L.segs.begin(), L.segs.end());
                std::swap(L.viaAt[0], L.viaAt[1]);
            }
            if ((L.pts.front() - v.position).length() > r) continue;
            if ((L.pts.back() - v.position).length() <= r) return bail("A track loops back to the via");
            bool dup = false;
            for (const auto& o : lines) dup = dup || o.segs == L.segs;
            if (!dup) lines.push_back(L);
        }
        committed.goneV[idx] = 1;
        for (const auto& L : lines)
            for (size_t s : L.segs) committed.goneT[s] = 1;
        vdrag.via = v;
        vdrag.grab = grab;
        vdrag.lines = lines;
        vdrag.index = idx;
        vdrag.at = v.position;
        layer = v.fromLayer;
        width = lines.empty() ? base->s.trackWidth : lines.front().width;
        Member m;
        m.net = v.net;
        m.end = v.position;
        members = {m};
        current = committed;
        kind = Kind::DragVia;
        status = "Dragging a via of " + base->netName(v.net);
        computeHead(grab);
        return true;
    }

    /// The lines on a via at `pos`: each starts at the via and rejoins its old path with a 45° link, skipping a
    /// corner when that makes a shorter join without an acute corner.
    std::vector<Track> viaLines(Vec2 pos) const {
        std::vector<Track> out;
        for (const Line& L : vdrag.lines) {
            const std::vector<Vec2>& p = L.pts;
            std::vector<Vec2> best;
            double bestLen = std::numeric_limits<double>::max();
            for (size_t k = 1; k < p.size() && k <= 2; ++k) {
                for (const auto& link : postureLinks(pos, p[k], RoutePosture::Diagonal45, false)) {
                    std::vector<Vec2> q = link;
                    q.insert(q.end(), p.begin() + static_cast<long>(k) + 1, p.end());
                    q = simplifyPath(q);
                    bool acute = false;
                    for (size_t j = 1; j + 1 < q.size(); ++j) acute = acute || acuteJoin(q[j] - q[j - 1], q[j + 1] - q[j]);
                    const double len = pathLength(q) + (acute ? 1e6 : 0.0);
                    if (len < bestLen - 1e-9) {
                        bestLen = len;
                        best = q;
                    }
                }
            }
            if (best.size() < 2) best = {pos, p.back()};
            for (const Track& t : toTracks(best, L.net, L.layer, L.width)) out.push_back(t);
        }
        return out;
    }

    bool placeVia(World& w, Vec2 pos, std::string& why) const {
        Via nv = vdrag.via;
        nv.position = pos;
        std::vector<size_t> tracks;
        for (const Track& t : viaLines(pos)) tracks.push_back(w.addTrack(t, true));
        const size_t vi = w.addVia(nv, true);
        if (highlight()) return true;
        if (opt.mode == RouterMode::Shove) {
            Shover sh(w, opt);
            return sh.run(tracks, {vi}, why);
        }
        std::vector<Hit> hits;
        if (viaHits(w, w.via(vi), vi, &hits)) {
            why = "Blocked by " + describeHit(w, hits.front());
            return false;
        }
        for (size_t i : tracks) {
            const Track& t = w.track(i);
            if (segmentHits(w, {t.net}, t.layer, t.a, t.b, t.width / 2, &hits)) {
                why = "Blocked by " + describeHit(w, hits.front());
                return false;
            }
        }
        if (!newAcuteJoins(w).empty()) {
            why = "The via's tracks would make an acute corner here";
            return false;
        }
        return true;
    }

    void viaDragHead(Vec2 cursor) {
        const Vec2 from = vdrag.via.position, to = from + (cursor - vdrag.grab);
        status = "Dragging a via of " + base->netName(vdrag.via.net);
        blocked = false;
        std::string why;
        World w = committed;
        if (placeVia(w, to, why)) {
            current = w;
            vdrag.at = to;
            vdrag.tracks = viaLines(to);
            vdrag.valid = true;
            return;
        }
        // Stop at the furthest point along the way that fits.
        blocked = true;
        status = why;
        double lo = 0, hi = 1;
        World best = committed;
        bool any = false;
        {
            World w0 = committed;
            if (placeVia(w0, from, why)) {
                best = w0;
                any = true;
            }
        }
        const double span = (to - from).length();
        bisect(
            lo, hi, 14, [span](double a, double b) { return (b - a) * span > 1e-3; },
            [&](Probe& pr) {
                pr.w = committed;
                pr.ok = placeVia(pr.w, from + (to - from) * pr.at, pr.why);
            },
            [&](Probe& pr) {
                if (!pr.ok) return;
                best = std::move(pr.w);
                any = true;
            });
        vdrag.valid = any;
        if (any) {
            current = best;
            vdrag.at = from + (to - from) * lo;
            vdrag.tracks = viaLines(vdrag.at);
            return;
        }
        // Nowhere fits: show the via and its tracks where they are; commit leaves the board alone.
        current = committed;
        current.goneV[vdrag.index] = 0;
        for (const Line& L : vdrag.lines)
            for (size_t s : L.segs) current.goneT[s] = 0;
        vdrag.at = from;
        vdrag.tracks.clear();
    }

    // ------------------------------------------------------------------------------------------- session steps

    std::atomic<unsigned> ownAbort{0};
    const std::atomic<unsigned>* extAbort = nullptr;

    void computeHead(Vec2 cursor) {
        // Cancellable: a request made while this runs (requestAbort / the external counter) stops it, and the
        // state from before it comes back, so the preview never shows a half-computed head.
        const unsigned own0 = ownAbort.load(std::memory_order_relaxed);
        const unsigned ext0 = extAbort ? extAbort->load(std::memory_order_relaxed) : 0;
        const std::function<bool()> check = [&] {
            return ownAbort.load(std::memory_order_relaxed) != own0 ||
                   (extAbort && extAbort->load(std::memory_order_relaxed) != ext0);
        };
        AbortScope scope(&check);
        World savedCurrent = current;
        const std::vector<Member> savedMembers = members;
        const std::vector<Vec2> savedCentre = centreHead;
        const ViaDrag savedVia = vdrag;
        const MultiDrag savedMulti = mdrag;
        const bool savedReached = reached, savedBlocked = blocked, savedHas = hasCursor;
        const std::string savedStatus = status;
        const Vec2 savedCursor = lastCursor;
        lastCursor = cursor;
        hasCursor = true;
        if (kind == Kind::Route) routeHead(cursor);
        if (kind == Kind::Pair) pairHead(cursor);
        if (kind == Kind::Bus) busHead(cursor);
        if (kind == Kind::Drag) dragHead(cursor);
        if (kind == Kind::DragVia) viaDragHead(cursor);
        if (kind == Kind::DragMulti) multiDragHead(cursor);
        if (check()) {
            current = std::move(savedCurrent);
            members = savedMembers;
            centreHead = savedCentre;
            vdrag = savedVia;
            mdrag = savedMulti;
            reached = savedReached;
            blocked = savedBlocked;
            hasCursor = savedHas;
            status = savedStatus;
            lastCursor = savedCursor;
            prev.aborted = true;
            return;
        }
        buildPreview();
    }

    static std::vector<Track> toTracks(const std::vector<Vec2>& pts, int net, int l, double w) {
        std::vector<Track> out;
        for (size_t k = 0; k + 1 < pts.size(); ++k) {
            if ((pts[k + 1] - pts[k]).length() < 1e-9) continue;
            Track t;
            t.net = net;
            t.layer = l;
            t.width = w;
            t.a = pts[k];
            t.b = pts[k + 1];
            out.push_back(t);
        }
        return out;
    }

    /// A head that starts back on the last placed track (the inner member of a pair turning at a placed corner): that
    /// track ends where the head starts instead.
    static void trimBacktrack(std::vector<Track>& placed, std::vector<Track>& head) {
        if (placed.empty() || head.empty()) return;
        Track& last = placed.back();
        const Vec2 s = head.front().a;
        if (last.arc || samePoint(s, last.b, 1e-9) || samePoint(s, last.a, 1e-9)) return;
        if (pointSegmentDistance(s, last.a, last.b) <= 1e-9) last.b = s;
    }

    bool fixHead() {
        if (kind != Kind::Route && kind != Kind::Pair && kind != Kind::Bus) return fail("Nothing to place");
        bool any = false;
        for (const auto& m : members) any = any || m.head.size() >= 2;
        if (!any) return fail("The head is empty");
        committed = current;
        for (auto& m : members) {
            if (m.head.size() < 2) continue;
            std::vector<Track> news = toTracks(m.head, m.net, layer, width);
            if (kind == Kind::Pair) trimBacktrack(m.placed, news);
            for (const Track& t : news) m.placed.push_back(t);
            m.end = m.head.back();
            m.head.clear();
        }
        if (kind == Kind::Pair && centreHead.size() >= 2) {
            if (side == 0) {
                const double cr = cross(centreHead[1] - centreHead[0], members[0].placed.empty() ? members[0].end - centre
                                                                                                : members[0].placed.front().a - centre);
                side = cr >= 0 ? 1 : -1;
            }
            lastDir = centreHead.back() - centreHead[centreHead.size() - 2];
            centre = centreHead.back();
            centreHead.clear();
        } else if (kind == Kind::Bus && centreHead.size() >= 2) {
            if (busOffset.empty()) busOffset = busOffsets(centreHead[1] - centreHead[0]);
            busFanOut = {};
            lastDir = centreHead.back() - centreHead[centreHead.size() - 2];
            centre = centreHead.back();
            centreHead.clear();
        } else if (kind == Kind::Route && !members[0].placed.empty()) {
            lastDir = members[0].placed.back().b - members[0].placed.back().a;
        }
        buildPreview();
        return true;
    }

    /// Via of `net` at `pos` from the route's layer to `to` for the via type: its span and drill / pad size.
    bool makeVia(int net, Vec2 pos, int to, Via& v) {
        const BoardSettings& s = base->s;
        const int n = std::max(1, s.layerCount);
        const int lo = std::min(layer, to), hi = std::max(layer, to);
        v = Via{};
        v.net = net;
        v.position = pos;
        v.drill = s.viaDrill;
        v.diameter = s.viaDiameter;
        RouterViaType type = opt.viaType;
        const bool hdi = s.hdi && n >= 4;
        if (type == RouterViaType::Auto) {
            if (!hdi || (lo == 0 && hi == n - 1)) type = RouterViaType::Through;
            else type = hi - lo == 1 ? RouterViaType::Micro : RouterViaType::Blind;
        }
        if (type == RouterViaType::Through) return true;  // 0 … bottom
        if (!hdi) return fail("Blind, buried and micro vias need HDI on a board of 4 or more layers (Board Setup)");
        if (type == RouterViaType::Micro) {
            if (hi - lo != 1) return fail("A microvia joins neighbouring layers only — pick the next layer");
            const double ring = std::min(s.minAnnularRing, 0.075);
            v.drill = s.microviaDrill;
            v.diameter = std::min(s.viaDiameter, std::max(s.microviaDiameter, v.drill + 2 * ring));
        }
        v.fromLayer = lo;
        v.toLayer = hi >= n - 1 ? -1 : hi;
        return true;
    }

    /// The layer V goes to: -1 = the other outer layer (through vias) or the next layer towards the other side
    /// (blind / micro / auto on an HDI board); -2 = the next layer the other way.
    int viaTarget(int toLayer) const {
        const BoardSettings& s = base->s;
        if (toLayer >= 0) return toLayer;
        const int bottom = s.bottomLayer();
        const bool stepwise = opt.viaType == RouterViaType::Blind || opt.viaType == RouterViaType::Micro ||
                              (opt.viaType == RouterViaType::Auto && s.hdi && s.layerCount >= 4);
        if (!stepwise) return layer == 0 ? bottom : 0;
        // Towards the far side: down from the top half, up from the bottom half; -2 reverses.
        int dir = layer < (bottom + 1) / 2 ? 1 : -1;
        if (toLayer == -2) dir = -dir;
        const int t = layer + dir;
        return t < 0 || t > bottom ? layer - dir : t;
    }

    bool addVia(int toLayer) {
        if (kind != Kind::Route && kind != Kind::Pair && kind != Kind::Bus) return fail("Start a route first");
        const BoardSettings& s = base->s;
        if (s.layerCount < 2) return fail("A single-sided board has no vias");
        const int to = viaTarget(toLayer);
        if (to == layer) return fail("The route is already on that layer");
        {
            Via probe;
            if (!makeVia(-1, {}, to, probe)) return false;
        }
        std::vector<int> nets;
        for (const auto& m : members) nets.push_back(m.net);
        if (!layerUsable(to, nets)) return false;
        bool any = false;
        for (const auto& m : members) any = any || m.head.size() >= 2;
        if (any) fixHead();
        World w = committed;
        std::vector<size_t> newVias, newTracks;
        std::vector<Via> vias;
        std::vector<std::vector<Track>> leads(members.size());
        if (kind == Kind::Route) {
            const Member& m = members[0];
            // Already on a through-hole pad or a via of the net: just change layer.
            bool through = false;
            for (const auto& p : base->pads)
                if (p.net == m.net && p.throughHole && padDistance(p, m.end) <= 0) through = true;
            for (size_t i = 0; i < w.nV(); ++i)
                if (w.aliveV(i) && w.via(i).net == m.net && w.via(i).spans(layer) && w.via(i).spans(to) &&
                    (w.via(i).position - m.end).length() <= w.via(i).diameter / 2)
                    through = true;
            if (through) {
                layer = to;
                lastDir = {};
                computeIfCursor();
                return true;
            }
            Via v;
            makeVia(m.net, m.end, to, v);
            vias.push_back(v);
        } else if (kind == Kind::Bus) {
            // A via per member, in a row across the bundle at via pitch (the members fan out to it at 45°); the
            // bundle continues on the next layer from the vias.
            if (busOffset.empty() || lastDir.length() <= 0) return fail("Place a corner first: the bus needs a direction for its vias");
            const Vec2 dir = unit(lastDir), n = leftNormal(dir);
            Via proto;
            makeVia(members[0].net, {}, to, proto);
            double clr = 0;
            for (const auto& a : members)
                for (const auto& b : members)
                    if (a.net != b.net) clr = std::max(clr, base->clearance(a.net, b.net));
            const double viaPitch = std::max(proto.diameter + clr, proto.drill + s.minHoleToHole) + kMargin;
            const double spread = std::max(1.0, viaPitch / std::max(spacing, 1e-6));
            double shift = 0;
            for (double o : busOffset) shift = std::max(shift, std::fabs(o) * (spread - 1));
            const Vec2 row = centre + dir * (shift + proto.diameter / 2 + width);
            for (size_t k = 0; k < members.size(); ++k) {
                Via v;
                makeVia(members[k].net, row + n * (busOffset[k] * spread), to, v);
                vias.push_back(v);
                auto lead = postureLinks(members[k].end, v.position, RoutePosture::Diagonal45, true).front();
                leads[k] = toTracks(lead, members[k].net, layer, width);
                for (const Track& t : leads[k]) newTracks.push_back(w.addTrack(t, true));
            }
        } else {
            Vec2 dir = lastDir;
            if (dir.length() <= 0) {
                const Vec2 across = members[0].end - members[1].end;
                dir = across.length() > 0 ? Vec2{across.y, -across.x} : Vec2{1, 0};
            }
            dir = unit(dir);
            const Vec2 n = leftNormal(dir);
            Via proto;
            makeVia(members[0].net, {}, to, proto);
            const double off = std::max(spacing / 2, (proto.diameter + base->clearance(members[0].net, members[1].net)) / 2 + kMargin);
            int sd = side != 0 ? side : (cross(dir, members[0].end - centre) >= 0 ? 1 : -1);
            for (size_t k = 0; k < 2; ++k) {
                Via v;
                makeVia(members[k].net, centre + n * ((k == 0 ? sd : -sd) * off), to, v);
                vias.push_back(v);
                if (!samePoint(members[k].end, v.position, 1e-9)) {
                    auto lead = postureLinks(members[k].end, v.position, RoutePosture::Diagonal45, false).front();
                    leads[k] = toTracks(lead, members[k].net, layer, width);
                    for (const Track& t : leads[k]) newTracks.push_back(w.addTrack(t, true));
                }
            }
        }
        for (const Via& v : vias) newVias.push_back(w.addVia(v, true));
        std::string why;
        bool ok;
        if (opt.mode == RouterMode::Shove) {
            Shover sh(w, opt);
            ok = sh.run(newTracks, newVias, why);
        } else if (highlight()) {
            ok = true;  // placed as asked; the preview lists what it violates
        } else {
            ok = true;
            for (size_t i : newVias) {
                std::vector<Hit> hits;
                if (viaHits(w, w.via(i), i, &hits)) {
                    ok = false;
                    why = "Blocked by " + describeHit(w, hits.front());
                    break;
                }
            }
            for (size_t i : newTracks) {
                const Track& t = w.track(i);
                std::vector<Hit> hits;
                if (ok && segmentHits(w, {t.net}, t.layer, t.a, t.b, t.width / 2, &hits)) {
                    ok = false;
                    why = "Blocked by " + describeHit(w, hits.front());
                }
            }
        }
        if (!ok) return fail("No room for a via here: " + why);
        committed = w;
        current = w;
        for (size_t k = 0; k < members.size(); ++k) {
            for (const Track& t : leads[k]) members[k].placed.push_back(t);
            members[k].end = vias[k].position;
        }
        for (const Via& v : vias) placedVias.push_back(v);
        if (kind == Kind::Pair) {
            if (side == 0) side = cross(lastDir.length() > 0 ? lastDir : Vec2{1, 0}, members[0].end - centre) >= 0 ? 1 : -1;
            centre = (members[0].end + members[1].end) * 0.5;
        }
        if (kind == Kind::Bus) {
            centre = {};
            for (const auto& m : members) centre = centre + m.end * (1.0 / static_cast<double>(members.size()));
        }
        layer = to;
        lastDir = {};
        computeIfCursor();
        return true;
    }

    void computeIfCursor() {
        for (auto& m : members) m.head.clear();
        centreHead.clear();
        current = committed;
        buildPreview();
    }

    bool verifyUnchanged() const {
        if (pcb.tracks.size() != base->tracks.size() || pcb.vias.size() != base->vias.size()) return false;
        for (size_t i = 0; i < pcb.tracks.size(); ++i)
            if (!sameTrack(pcb.tracks[i], base->tracks[i])) return false;
        for (size_t i = 0; i < pcb.vias.size(); ++i)
            if (!sameVia(pcb.vias[i], base->vias[i])) return false;
        const auto pads = pcb.pads(sch);
        if (pads.size() != base->pads.size()) return false;
        for (size_t i = 0; i < pads.size(); ++i)
            if (!(pads[i].position == base->pads[i].position) || pads[i].net != base->pads[i].net) return false;
        return true;
    }

    RouteChanges commit() {
        RouteChanges ch;
        if (kind == Kind::None) {
            ch.error = "No route in progress";
            return ch;
        }
        if (kind == Kind::Route || kind == Kind::Pair || kind == Kind::Bus) {
            bool any = false;
            for (const auto& m : members) any = any || m.head.size() >= 2;
            if (any) fixHead();
        }
        if (!verifyUnchanged()) {
            ch.error = "The board changed while routing — the route was cancelled";
            reset();
            return ch;
        }
        std::vector<Track> routeTracks;
        std::vector<Via> routeVias = placedVias;
        if (kind == Kind::DragVia) {
            if (!vdrag.valid) {
                reset();
                ch.ok = true;  // nothing changed
                return ch;
            }
            committed = current;
            routeVias = {vdrag.via};
            routeVias[0].position = vdrag.at;
            routeTracks = mergeCollinear(vdrag.tracks);
        } else if (kind == Kind::DragMulti) {
            if (!mdrag.valid) {
                reset();
                ch.ok = true;  // nothing changed
                return ch;
            }
            committed = current;
            routeTracks = mergeCollinear(mdrag.tracks);
        } else if (kind == Kind::Drag) {
            committed = current;
            for (const Track& t : mergeCollinear(toTracks(members[0].head, members[0].net, layer, width)))
                routeTracks.push_back(t);
        } else if ((kind == Kind::Pair || kind == Kind::Bus) && opt.arcCorners && cornerRadius() > 0) {
            std::vector<std::vector<Track>> per;
            std::vector<size_t> from;
            for (const auto& m : members) {
                per.push_back(m.placed);
                from.push_back(m.placed.size());
            }
            for (const auto& ts : roundGroup(per, from, committed, nullptr))
                for (const Track& t : mergeCollinear(ts)) routeTracks.push_back(t);
        } else {
            for (const auto& m : members)
                for (const Track& t : mergeCollinear(kind == Kind::Route && cornerRadius() > 0
                                                         ? roundRoute(m.placed, m.placed.size(), committed, nullptr)
                                                         : m.placed))
                    routeTracks.push_back(t);
        }
        // Route pieces that run exactly over copper the net already has add nothing.
        std::vector<Track> kept;
        for (const Track& t : routeTracks) {
            bool covered = false;
            std::vector<size_t> found;
            committed.tracksIn(trackBox(t, 0.01), found);
            for (size_t i : found) {
                const Track& o = committed.track(i);
                if (i >= committed.baseT() && committed.fixAddT[i - committed.baseT()]) continue;  // the route itself
                if (o.net == t.net && o.layer == t.layer && o.width >= t.width - 1e-9 && !t.arc &&
                    trackPointDistance(o, t.a) <= 1e-7 && trackPointDistance(o, t.b) <= 1e-7)
                    covered = true;
            }
            if (!covered) kept.push_back(t);
        }
        std::unique_ptr<PcbLayout> before;
        if (opt.removeLoops) before = std::make_unique<PcbLayout>(pcb);
        ch = applyWorld(pcb, committed, kept, routeVias);
        if (before) removeLoops(*before, ch);
        if (opt.autoTeardrops && !ch.addedTracks.empty()) {
            TeardropOptions to;
            to.trackIds = ch.addedTracks;
            const BoardEditResult td = addTeardrops(pcb, sch, to);
            for (int id : td.changes.addedTracks) ch.addedTracks.push_back(id);
        }
        std::vector<Track> pruned;
        pruneTeardrops(pcb, sch, &pruned);
        for (const Track& t : pruned) {
            const auto it = std::find(ch.addedTracks.begin(), ch.addedTracks.end(), t.id);
            if (it != ch.addedTracks.end())
                ch.addedTracks.erase(it);
            else
                ch.removedTracks.push_back(t);
        }
        reset();
        return ch;
    }

    /// Loop removal after a commit: old tracks of the routed nets that were needed before (removing one split the
    /// net's pads) but are redundant now are removed one by one, then vias of those nets left touching nothing.
    void removeLoops(const PcbLayout& before, RouteChanges& ch) {
        const std::set<int> added(ch.addedTracks.begin(), ch.addedTracks.end());
        std::set<int> nets;
        for (const auto& t : pcb.tracks)
            if (added.count(t.id) && t.net >= 0) nets.insert(t.net);
        if (nets.empty()) return;
        std::set<std::string> poured;
        for (const auto& z : pcb.zones) poured.insert(z.net);
        const auto& pads = base->pads;
        std::set<int> goneIds;
        for (int net : nets) {
            if (net >= static_cast<int>(sch.nets().size()) || poured.count(sch.nets()[static_cast<size_t>(net)].name)) continue;
            std::vector<size_t> padsOfNet;
            for (size_t i = 0; i < pads.size(); ++i)
                if (pads[i].net == net) padsOfNet.push_back(i);
            if (padsOfNet.size() < 2) continue;
            std::vector<int> cand;
            size_t count = 0;
            for (const auto& t : pcb.tracks)
                if (t.net == net) {
                    ++count;
                    if (!added.count(t.id) && !t.locked && !t.arc && !t.teardrop) cand.push_back(t.id);
                }
            if (cand.empty() || count > 600) continue;
            const std::set<int> none;
            const auto ref = padPartition(pads, padsOfNet, pcb.tracks, pcb.vias, net, none);
            const auto refBefore = padPartition(pads, padsOfNet, before.tracks, before.vias, net, none);
            std::set<int> gone;
            for (int id : cand) {
                const std::set<int> one{id};
                if (padPartition(pads, padsOfNet, before.tracks, before.vias, net, one) == refBefore) continue;  // was spare
                std::set<int> trial = gone;
                trial.insert(id);
                if (padPartition(pads, padsOfNet, pcb.tracks, pcb.vias, net, trial) == ref) gone.insert(id);
            }
            // The rest of a removed path (stubs between removed tracks) goes too.
            bool more = !gone.empty();
            while (more) {
                more = false;
                for (int id : cand) {
                    if (gone.count(id)) continue;
                    const Track* t = nullptr;
                    for (const auto& o : pcb.tracks)
                        if (o.id == id) t = &o;
                    if (t == nullptr) continue;
                    // A stub: one end touches nothing kept of the net but removed tracks.
                    for (const Vec2 e : {t->a, t->b}) {
                        bool kept = false, touchedGone = false;
                        for (const auto& o : pcb.tracks) {
                            if (o.id == id || o.net != net || o.layer != t->layer || trackPointDistance(o, e) > o.width / 2) continue;
                            if (gone.count(o.id))
                                touchedGone = true;
                            else
                                kept = true;
                        }
                        for (const auto& v : pcb.vias) kept = kept || (v.net == net && (v.position - e).length() <= v.diameter / 2);
                        for (size_t p : padsOfNet) kept = kept || (pads[p].onLayer(t->layer) && padDistance(pads[p], e) <= 0);
                        if (touchedGone && !kept) {
                            std::set<int> trial = gone;
                            trial.insert(id);
                            if (padPartition(pads, padsOfNet, pcb.tracks, pcb.vias, net, trial) == ref) {
                                gone.insert(id);
                                more = true;
                            }
                            break;
                        }
                    }
                }
            }
            goneIds.insert(gone.begin(), gone.end());
        }
        if (goneIds.empty()) return;
        std::set<int> touchedVias;
        for (const auto& t : pcb.tracks)
            if (goneIds.count(t.id))
                for (const auto& v : pcb.vias)
                    if (v.net == t.net && v.spans(t.layer) && trackPointDistance(t, v.position) <= v.diameter / 2) touchedVias.insert(v.id);
        for (const auto& t : pcb.tracks)
            if (goneIds.count(t.id)) ch.removedTracks.push_back(t);
        pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return goneIds.count(t.id) > 0; }),
                         pcb.tracks.end());
        std::set<int> goneVias;
        for (const auto& v : pcb.vias) {
            if (!touchedVias.count(v.id)) continue;
            bool used = false;
            for (const auto& t : pcb.tracks) used = used || (t.net == v.net && v.spans(t.layer) && trackPointDistance(t, v.position) <= v.diameter / 2);
            for (const auto& p : pads) used = used || (p.net == v.net && padDistance(p, v.position) <= v.diameter / 2);
            if (!used) goneVias.insert(v.id);
        }
        for (const auto& v : pcb.vias)
            if (goneVias.count(v.id)) {
                const auto it = std::find(ch.addedVias.begin(), ch.addedVias.end(), v.id);
                if (it != ch.addedVias.end())
                    ch.addedVias.erase(it);
                else
                    ch.removedVias.push_back(v);
            }
        pcb.vias.erase(std::remove_if(pcb.vias.begin(), pcb.vias.end(), [&](const Via& v) { return goneVias.count(v.id) > 0; }),
                       pcb.vias.end());
    }

    /// What the session's own copper (route, dragged track or via) violates in the current overlay.
    std::vector<RouteCollision> collisions() const {
        std::vector<RouteCollision> out;
        std::set<Hit> seen;
        const World& w = current;
        auto record = [&](const Hit& h, Vec2 a, Vec2 b) {
            if (!seen.insert(h).second) return;
            RouteCollision c;
            c.at = (a + b) * 0.5;
            switch (h.kind) {
                case HitKind::Pad: {
                    const Pad& pd = w.b->pads[h.index];
                    c.kind = "pad";
                    c.a = pd.position;
                    c.size = pd.size;
                    c.at = closestOnSegment(pd.position, a, b);
                    break;
                }
                case HitKind::Track: {
                    const Track& t = w.track(h.index);
                    c.kind = "track";
                    c.id = h.index < w.baseT() ? t.id : -1;
                    c.a = t.a;
                    c.b = t.b;
                    c.width = t.width;
                    break;
                }
                case HitKind::Via: {
                    const Via& v = w.via(h.index);
                    c.kind = "via";
                    c.id = h.index < w.baseV() ? v.id : -1;
                    c.a = v.position;
                    c.width = v.diameter;
                    c.at = closestOnSegment(v.position, a, b);
                    break;
                }
                case HitKind::Edge: c.kind = "edge"; break;
                case HitKind::Hole:
                    c.kind = "hole";
                    c.a = w.b->s.holes[h.index].position;
                    c.width = w.b->s.holes[h.index].keepout;
                    c.at = closestOnSegment(c.a, a, b);
                    break;
                case HitKind::Plane: c.kind = "plane"; break;
                case HitKind::Mesh: c.kind = "mesh"; break;
            }
            out.push_back(c);
        };
        std::vector<Hit> hits;
        for (size_t k = 0; k < w.addT.size(); ++k) {
            const size_t i = w.baseT() + k;
            if (!w.aliveT(i) || !w.fixAddT[k]) continue;
            const Track& t = w.track(i);
            hits.clear();
            segmentHits(w, {t.net}, t.layer, t.a, t.b, t.width / 2, &hits);
            for (const Hit& h : hits) record(h, t.a, t.b);
        }
        for (size_t k = 0; k < w.addV.size(); ++k) {
            const size_t i = w.baseV() + k;
            if (!w.aliveV(i) || !w.fixAddV[k]) continue;
            hits.clear();
            viaHits(w, w.via(i), i, &hits);
            for (const Hit& h : hits) record(h, w.via(i).position, w.via(i).position);
        }
        return out;
    }

    void buildPreview() {
        RoutePreview p;
        p.active = kind != Kind::None;
        if (!p.active) {
            prev = p;
            return;
        }
        p.kind = kind == Kind::Route       ? "route"
                 : kind == Kind::Pair      ? "pair"
                 : kind == Kind::Bus       ? (multiRoute ? "multi" : "bus")
                 : kind == Kind::Drag      ? "drag"
                 : kind == Kind::DragMulti ? (mdrag.corner ? "corner" : "multidrag")
                                           : "via";
        p.status = status;
        p.blocked = blocked;
        p.reachedTarget = reached;
        for (const auto& m : members) p.nets.push_back(m.net);
        p.layer = layer;
        p.layerCount = std::max(1, base->s.layerCount);
        p.width = width;
        p.gap = kind == Kind::Pair ? gap : 0;
        for (const auto& m : members) {
            std::vector<Track> placed = m.placed, head = toTracks(m.head, m.net, layer, width);
            if (kind == Kind::Pair) trimBacktrack(placed, head);
            p.placed.insert(p.placed.end(), placed.begin(), placed.end());
            p.head.insert(p.head.end(), head.begin(), head.end());
        }
        if (kind == Kind::Route && cornerRadius() > 0 && !members.empty()) {
            // Show the corners as they will be written: the whole route rounded, split where the head begins.
            std::vector<Track> all = p.placed;
            const size_t headFrom = all.size();
            all.insert(all.end(), p.head.begin(), p.head.end());
            size_t h = 0;
            const std::vector<Track> round = roundRoute(all, headFrom, current, &h);
            h = std::min(h, round.size());
            p.placed.assign(round.begin(), round.begin() + static_cast<long>(h));
            p.head.assign(round.begin() + static_cast<long>(h), round.end());
        }
        if ((kind == Kind::Pair || kind == Kind::Bus) && opt.arcCorners && cornerRadius() > 0 && !members.empty()) {
            // Concentric arc corners, as they will be written.
            std::vector<std::vector<Track>> per;
            std::vector<size_t> from;
            for (const auto& m : members) {
                std::vector<Track> placed = m.placed, head = toTracks(m.head, m.net, layer, width);
                if (kind == Kind::Pair) trimBacktrack(placed, head);
                from.push_back(placed.size());
                placed.insert(placed.end(), head.begin(), head.end());
                per.push_back(placed);
            }
            std::vector<size_t> h;
            const auto round = roundGroup(per, from, current, &h);
            p.placed.clear();
            p.head.clear();
            for (size_t k = 0; k < round.size(); ++k) {
                const size_t hk = std::min(h[k], round[k].size());
                p.placed.insert(p.placed.end(), round[k].begin(), round[k].begin() + static_cast<long>(hk));
                p.head.insert(p.head.end(), round[k].begin() + static_cast<long>(hk), round[k].end());
            }
        }
        if (kind == Kind::Pair || kind == Kind::Bus)
            p.end = centreHead.size() >= 2 ? centreHead.back() : centre;
        else if (kind == Kind::DragVia)
            p.end = vdrag.at;
        else if (kind == Kind::DragMulti)
            p.end = (mdrag.corner && !mdrag.ends.empty() ? mdrag.ends[0].p : mdrag.grab) + mdrag.offset;
        else
            p.end = !members.empty() ? (members[0].head.size() >= 2 ? members[0].head.back() : members[0].end) : Vec2{};
        p.vias = placedVias;
        if (kind == Kind::DragMulti) p.head = mdrag.tracks;
        if (kind == Kind::DragVia) {
            p.head = vdrag.tracks;
            if (vdrag.valid) {
                Via v = vdrag.via;
                v.position = vdrag.at;
                p.vias = {v};
            }
        }
        for (size_t k = 0; k < current.addT.size(); ++k)
            if (!current.goneT[current.baseT() + k] && !current.fixAddT[k]) p.shovedTracks.push_back(current.addT[k]);
        for (size_t k = 0; k < current.addV.size(); ++k)
            if (!current.goneV[current.baseV() + k] && !current.fixAddV[k]) p.shovedVias.push_back(current.addV[k]);
        for (size_t i = 0; i < current.baseT(); ++i)
            if (current.goneT[i]) p.hiddenTracks.push_back(base->tracks[i].id);
        for (size_t i = 0; i < current.baseV(); ++i)
            if (current.goneV[i]) p.hiddenVias.push_back(base->vias[i].id);
        if (kind == Kind::DragVia) {
            for (const Track& t : vdrag.tracks) p.length += trackLength(t);
        } else if (kind == Kind::DragMulti) {
            for (const Track& t : mdrag.tracks) p.length += trackLength(t);
        } else if (!members.empty()) {
            for (const Track& t : members[0].placed) p.length += trackLength(t);
            p.length += pathLength(members[0].head);
        }
        if (highlight()) {
            p.collisions = collisions();
            if (!p.collisions.empty())
                p.status = std::to_string(p.collisions.size()) +
                           (p.collisions.size() == 1 ? " collision" : " collisions") + " — the DRC will report them";
        }
        // The net's whole length: its copper in the overlay (the session's own pieces aside) plus the route.
        if (!members.empty()) {
            const int net = members[0].net;
            double other = 0;
            for (size_t i = 0; i < current.nT(); ++i) {
                if (!current.aliveT(i) || (i >= current.baseT() && current.fixAddT[i - current.baseT()])) continue;
                const Track& t = current.track(i);
                if (t.net == net) other += trackLength(t);
            }
            p.netLength = other + p.length;
            p.targetLength = groupTarget;
        }
        prev = p;
    }
};

InteractiveRouter::InteractiveRouter(PcbLayout& pcb, const Schematic& sch) : impl_(std::make_unique<Impl>(pcb, sch)) {}
InteractiveRouter::~InteractiveRouter() = default;

void InteractiveRouter::setOptions(const RouterOptions& options) {
    impl_->opt = options;
    impl_->opt.shoveLimit = std::clamp(options.shoveLimit, 1, 10000);
    // A route in progress re-aims its head at the last cursor position with the new options.
    if (impl_->kind != Impl::Kind::None && impl_->hasCursor) impl_->computeHead(impl_->lastCursor);
}
const RouterOptions& InteractiveRouter::options() const { return impl_->opt; }

bool InteractiveRouter::beginRoute(Vec2 at, int layer) {
    impl_->err.clear();
    return impl_->beginRoute(at, layer);
}
bool InteractiveRouter::beginBus(Vec2 at, int layer, int count) {
    impl_->err.clear();
    return impl_->beginBus(at, layer, count);
}
bool InteractiveRouter::beginPair(Vec2 at, int layer) {
    impl_->err.clear();
    return impl_->beginPair(at, layer);
}
bool InteractiveRouter::beginDrag(int trackId, Vec2 grab) {
    impl_->err.clear();
    return impl_->beginDrag(trackId, grab);
}
bool InteractiveRouter::beginCornerDrag(int trackId, Vec2 grab) { return impl_->beginCornerDrag(trackId, grab); }
bool InteractiveRouter::beginMultiDrag(const std::vector<int>& trackIds, Vec2 grab) {
    return impl_->beginMultiDrag(trackIds, grab);
}
bool InteractiveRouter::beginMultiRoute(const std::vector<Vec2>& starts, int layer) {
    return impl_->beginMultiRoute(starts, layer);
}

bool InteractiveRouter::beginViaDrag(int viaId, Vec2 grab) {
    impl_->err.clear();
    return impl_->beginViaDrag(viaId, grab);
}
void InteractiveRouter::requestAbort() { impl_->ownAbort.fetch_add(1, std::memory_order_relaxed); }
void InteractiveRouter::setAbortSource(const std::atomic<unsigned>* counter) { impl_->extAbort = counter; }
const RoutePreview& InteractiveRouter::moveTo(Vec2 cursor) {
    if (impl_->kind != Impl::Kind::None) impl_->computeHead(cursor);
    return impl_->prev;
}
bool InteractiveRouter::fixHead() {
    impl_->err.clear();
    return impl_->fixHead();
}
bool InteractiveRouter::addVia(int toLayer) {
    impl_->err.clear();
    const bool ok = impl_->addVia(toLayer);
    if (ok && impl_->hasCursor) impl_->computeHead(impl_->lastCursor);
    return ok;
}
RouteChanges InteractiveRouter::commit() {
    impl_->err.clear();
    RouteChanges ch = impl_->commit();
    if (!ch.ok) impl_->err = ch.error;
    return ch;
}
void InteractiveRouter::cancel() { impl_->reset(); }
bool InteractiveRouter::active() const { return impl_->kind != Impl::Kind::None; }
const RoutePreview& InteractiveRouter::preview() const { return impl_->prev; }
const std::string& InteractiveRouter::error() const { return impl_->err; }

// ============================================================================================== length tuning

LengthTuneResult tuneTrackLength(PcbLayout& pcb, const Schematic& sch, int trackId, double target, double maxAmplitude) {
    LengthTuneOptions o;
    o.target = target;
    o.maxAmplitude = maxAmplitude;
    return tuneTrackLength(pcb, sch, trackId, o);
}


// --------------------------------------------------------------------------------- length tuning: patterns, spans

namespace {

/// One meander pattern along the line a → b between `s0` and `s1` (distances from a): `n` units of `style` with
/// height `amp` on side `side` (+1 = the left normal) and leg pitch `pitch`, centred on `centre` (clamped into the
/// span). Returns the whole polyline from a to b. Accordion: n rectangular bumps (up, across one pitch, down, one pitch
/// on); trombone: one bump as wide as the span (n ignored); sawtooth: n triangular teeth two pitches wide.
std::vector<Vec2> meanderPolyline(Vec2 a, Vec2 b, double s0, double s1, MeanderStyle style, int n, double amp, double side,
                                  double pitch, double centre) {
    const Vec2 u = unit(b - a), nrm = Vec2{-u.y, u.x} * side;
    double run = 0;
    switch (style) {
        case MeanderStyle::Accordion: run = 2 * pitch * n - pitch; break;  // the last unit ends on its down leg
        case MeanderStyle::Trombone: run = s1 - s0 - 2 * pitch; break;  // clear of the stretch ends (pads)
        case MeanderStyle::Sawtooth: run = 2 * pitch * n; break;
    }
    run = std::clamp(run, std::min(pitch, s1 - s0), s1 - s0);
    const double start = std::clamp(centre - run / 2, s0, std::max(s0, s1 - run));
    std::vector<Vec2> pts{a};
    Vec2 p = a + u * start;
    pts.push_back(p);
    switch (style) {
        case MeanderStyle::Accordion:
            for (int i = 0; i < n; ++i) {
                if (i > 0) {
                    p = p + u * pitch;
                    pts.push_back(p);
                }
                p = p + nrm * amp;
                pts.push_back(p);
                p = p + u * pitch;
                pts.push_back(p);
                p = p - nrm * amp;
                pts.push_back(p);
            }
            break;
        case MeanderStyle::Trombone:
            p = p + nrm * amp;
            pts.push_back(p);
            p = p + u * run;
            pts.push_back(p);
            p = p - nrm * amp;
            pts.push_back(p);
            break;
        case MeanderStyle::Sawtooth:
            for (int i = 0; i < n; ++i) {
                pts.push_back(p + u * pitch + nrm * amp);
                p = p + u * (2 * pitch);
                pts.push_back(p);
            }
            break;
    }
    pts.push_back(b);
    return simplifyPath(pts);
}

/// 45° chamfers at the polyline's corners that turn by 80° or more (`cut` along each leg, at most 45 % of it).
std::vector<Vec2> chamferCorners(const std::vector<Vec2>& pts, double cut) {
    if (pts.size() < 3) return pts;
    std::vector<Vec2> out{pts[0]};
    for (size_t i = 1; i + 1 < pts.size(); ++i) {
        const Vec2 d1 = pts[i] - pts[i - 1], d2 = pts[i + 1] - pts[i];
        const double l1 = d1.length(), l2 = d2.length();
        if (l1 < 1e-9 || l2 < 1e-9 || unit(d1).dot(unit(d2)) > std::cos(80 * kPi / 180)) {
            out.push_back(pts[i]);
            continue;
        }
        const double c = std::min({cut, 0.45 * l1, 0.45 * l2});
        out.push_back(pts[i] - unit(d1) * c);
        out.push_back(pts[i] + unit(d2) * c);
    }
    out.push_back(pts.back());
    return out;
}

double tracksLength(const std::vector<Track>& ts) {
    double l = 0;
    for (const Track& t : ts) l += trackLength(t);
    return l;
}

}  // namespace

namespace {
/// Advanced interactive length tuning (styles, corner shapes, drag-along spans, coupled pairs, phase tuning, length
/// rules / match groups / xSignals). The plain accordion path of tuneTrackLength is kept bit for bit.
LengthTuneResult tuneTrackLengthAdvanced(PcbLayout& pcb, const Schematic& sch, size_t ti, const LengthTuneOptions& opt) {
    LengthTuneResult r;
    const Track sel = pcb.tracks[ti];
    r.net = sel.net;
    if (sel.arc) {
        r.message = "Tune on a straight track (arcs are not meandered)";
        return r;
    }
    const auto pads = pcb.pads(sch);
    // What is measured: the net's xSignal (pad to pad through series parts) when a length rule or match group applies
    // or the signal passes series parts; otherwise the net's routed copper.
    LengthTarget lt;
    const bool ruled = lengthTargetFor(pcb, sch, pads, sel.net, lt);
    if (!ruled) lt.xsignal = xSignalOf(sch, pads, sel.net);
    const bool viaX = ruled || !lt.xsignal.seriesParts.empty();
    auto measure = [&](const PcbLayout& p) {
        if (!viaX) return routedNetLength(p, sel.net);
        const double l = xSignalLength(p, pads, lt.xsignal);
        return l > 0 ? l : routedNetLength(p, sel.net);
    };
    r.before = measure(pcb);
    for (int n : lt.xsignal.nets) r.xsignalNets.push_back(n);
    // Pair partner (differential pair group).
    int partner = -1;
    double groupLongest = 0, groupTol = 0;
    for (const auto& g : lengthGroups(sch, pcb.settings)) {
        if (std::find(g.nets.begin(), g.nets.end(), sel.net) == g.nets.end()) continue;
        if (r.group.empty()) {
            r.group = g.name;
            r.groupKind = g.kind;
        }
        if (g.kind == "pair" && g.nets.size() == 2) partner = g.nets[0] == sel.net ? g.nets[1] : g.nets[0];
        for (int n : g.nets) groupLongest = std::max(groupLongest, routedNetLength(pcb, n));
        groupTol = std::max(groupTol, g.tolerance / 2);
    }
    double target = opt.target, tolerance = 0.01;
    if (target > 0) {
        r.targetSource = "typed";
    } else if (opt.phase && partner >= 0) {
        target = routedNetLength(pcb, partner);
        tolerance = std::max(tolerance, groupTol);
        r.targetSource = "partner";
    } else if (ruled) {
        target = lt.target;
        tolerance = std::max(tolerance, lt.tolerance / 2);
        r.targetSource = lt.source;
    } else if (!r.group.empty() && !opt.coupled) {
        target = groupLongest;
        tolerance = std::max(tolerance, groupTol);
        r.targetSource = "group:" + r.group;
    }
    if (target <= 0) {
        r.message = opt.coupled ? "Coupled tuning needs a target: type one, or give the pair a length rule or match group"
                                : "The net has no length target — type one, or give it a length rule or match group";
        return r;
    }
    r.target = target;
    r.tolerance = tolerance;
    r.after = r.before;
    double want = target - r.before;
    if (want <= tolerance) {
        r.ok = true;
        r.changes.ok = true;
        r.message = want < -tolerance ? "The net is already longer than the target" : "The net is already at the target length";
        return r;
    }
    Base base(pcb, sch);
    World w(&base);
    const double clr = base.s.clearance;
    const double wd = sel.width;
    double maxAmp = opt.maxAmplitude > 0 ? opt.maxAmplitude : 2.0;
    if (opt.phase && opt.maxAmplitude <= 0) maxAmp = std::max(2 * wd, clr + wd);  // phase bumps stay small
    double pitch = opt.spacing > 0 ? std::max(wd + clr, opt.spacing + wd) : std::max(wd + clr, 3 * wd);
    if (opt.phase && opt.spacing <= 0) pitch = wd + clr + wd;
    const double margin = std::max(2 * wd, 0.5);

    // Partner track side by side with the selected one (coupled tuning; phase bumps point away from it).
    long ptrack = -1;
    double pairOffset = 0;  // signed distance of the partner line from the selected line along its left normal
    if (partner >= 0) {
        const Vec2 u = unit(sel.b - sel.a), nl{-u.y, u.x};
        double best = 1e18;
        for (size_t i = 0; i < base.tracks.size(); ++i) {
            const Track& o = base.tracks[i];
            if (o.net != partner || o.layer != sel.layer || o.arc || base.trackFixed[i]) continue;
            const Vec2 v = unit(o.b - o.a);
            if (std::fabs(cross(u, v)) > 1e-6) continue;
            const double off = (o.a - sel.a).dot(nl);
            if (std::fabs(off) > 4 * (wd + o.width + clr) || std::fabs(off) < 1e-6) continue;
            const double t0 = (o.a - sel.a).dot(u), t1 = (o.b - sel.a).dot(u);
            const double overlap = std::min(std::max(t0, t1), (sel.b - sel.a).length()) - std::max(std::min(t0, t1), 0.0);
            if (overlap < 2 * margin) continue;
            if (std::fabs(off) < best) {
                best = std::fabs(off);
                ptrack = static_cast<long>(i);
                pairOffset = off;
            }
        }
    }
    if (opt.coupled && ptrack < 0) {
        r.message = "Coupled tuning: no track of the pair partner runs alongside this one";
        return r;
    }
    r.coupled = opt.coupled;
    r.partnerNet = partner;

    // Candidate tracks: the selected one first; without a span or coupling, then the measured nets' other tracks.
    std::vector<size_t> cand{ti};
    if (!opt.hasSpan && !opt.coupled) {
        std::vector<size_t> others;
        for (size_t i = 0; i < base.tracks.size(); ++i)
            if (i != ti && !base.trackFixed[i] && !base.tracks[i].arc &&
                std::find(lt.xsignal.nets.begin(), lt.xsignal.nets.end(), base.tracks[i].net) != lt.xsignal.nets.end())
                others.push_back(i);
        std::stable_sort(others.begin(), others.end(),
                         [&](size_t a, size_t b) { return trackLength(base.tracks[a]) > trackLength(base.tracks[b]); });
        cand.insert(cand.end(), others.begin(), others.end());
    }
    auto clearOwnPads = [&](const Track& t, const Track& piece) {
        for (const auto& pd : base.pads)
            if (pd.net == t.net && pd.onLayer(t.layer) &&
                (pd.round ? std::max(0.0, trackPointDistance(piece, pd.position) - std::min(pd.size.x, pd.size.y) / 2)
                          : trackRectDistance(piece, pd.bounds())) - t.width / 2 < clr - kTol)
                return false;
        return true;
    };
    // Tracks of polyline `pts` with the corner shape asked for (`proto`'s net, layer and width).
    auto shape = [&](const std::vector<std::vector<Vec2>>& lines, const std::vector<Track>& protos, double amp) {
        std::vector<std::vector<Track>> out(lines.size());
        if (opt.corner == MeanderCorner::Round) {
            const double rad = std::max(0.05, std::min(amp, pitch) / 2 * 0.999);
            out = filletArcRuns(lines, protos, rad, std::max(0.01, std::min(rad, wd / 2)),
                                [](size_t, const Track&) { return true; }, {}, nullptr);
        } else {
            for (size_t k = 0; k < lines.size(); ++k) {
                const auto& pts = lines[k];
                for (size_t i = 0; i + 1 < pts.size(); ++i) {
                    if ((pts[i + 1] - pts[i]).length() < 1e-9) continue;
                    Track t = protos[k];
                    t.a = pts[i];
                    t.b = pts[i + 1];
                    out[k].push_back(t);
                }
            }
        }
        return out;
    };
    for (size_t ci : cand) {
        if (want <= tolerance) break;
        const Track t = base.tracks[ci];
        if (t.arc) continue;
        const double L = trackLength(t);
        const Vec2 u = unit(t.b - t.a);
        double s0 = margin, s1 = L - margin;
        // The pattern stays off the net's own pads at the track's ends: their clearance plus a track width beyond
        // their far edge (a meander pushed against an end by the press point or a span would otherwise start inside
        // the pad's clearance, and no smaller height can fix that).
        for (const auto& pd : base.pads) {
            if (pd.net != t.net || !pd.onLayer(t.layer)) continue;
            const bool atA = padDistance(pd, t.a) <= 0, atB = padDistance(pd, t.b) <= 0;
            if (!atA && !atB) continue;
            const Rect bb = pd.bounds();
            double lo = std::numeric_limits<double>::max(), hi = -lo;
            for (Vec2 c : {Vec2{bb.x0, bb.y0}, Vec2{bb.x1, bb.y0}, Vec2{bb.x1, bb.y1}, Vec2{bb.x0, bb.y1}}) {
                lo = std::min(lo, (c - t.a).dot(u));
                hi = std::max(hi, (c - t.a).dot(u));
            }
            if (atA) s0 = std::max(s0, hi + clr + wd);
            if (atB) s1 = std::min(s1, lo - clr - wd);
        }
        if (opt.hasSpan && ci == ti) {
            const double p0 = (opt.spanFrom - t.a).dot(u), p1 = (opt.spanTo - t.a).dot(u);
            s0 = std::max(s0, std::min(p0, p1));
            s1 = std::min(s1, std::max(p0, p1));
        }
        // Coupled: the pair's centre line over the stretch both tracks share.
        Track partnerTrack;
        Vec2 la = t.a, lb = t.b;  // the line the pattern runs along
        double half = 0;
        if (opt.coupled) {
            partnerTrack = base.tracks[static_cast<size_t>(ptrack)];
            const double q0 = (partnerTrack.a - t.a).dot(u), q1 = (partnerTrack.b - t.a).dot(u);
            s0 = std::max(s0, std::min(q0, q1) + margin);
            s1 = std::min(s1, std::max(q0, q1) - margin);
            half = pairOffset / 2;
        }
        if (s1 - s0 < pitch) continue;
        double centre = (s0 + s1) / 2;
        if (opt.hasNear && ci == ti && !opt.hasSpan) centre = (opt.near - t.a).dot(u);
        const double coupledPitch = opt.coupled ? pitch + std::fabs(pairOffset) : pitch;
        const double stylePitch = coupledPitch;
        const double capAmp = opt.style == MeanderStyle::Sawtooth ? std::min(maxAmp, stylePitch) : maxAmp;
        int maxN = 1;
        if (opt.style == MeanderStyle::Accordion) maxN = static_cast<int>(std::floor((s1 - s0 + stylePitch) / (2 * stylePitch)));
        if (opt.style == MeanderStyle::Sawtooth) maxN = static_cast<int>(std::floor((s1 - s0) / (2 * stylePitch)));
        if (maxN < 1) continue;
        std::vector<double> sides;
        if (opt.phase && ptrack >= 0)
            sides = {pairOffset > 0 ? -1.0 : 1.0};  // away from the partner
        else if (opt.coupled)
            sides = {1.0, -1.0};
        else
            sides = {1.0, -1.0};
        bool done = false;
        for (double sd : sides) {
            if (done) break;
            // The pattern on the line (the centre line when coupled), and the tracks it becomes for each member.
            auto build = [&](int n, double amp, std::vector<std::vector<Track>>& out) {
                const Vec2 off = Vec2{-u.y, u.x} * half;
                const Vec2 ca = la + off, cb = lb + off;
                std::vector<Vec2> centrePts = meanderPolyline(ca, cb, s0, s1, opt.style, n, amp, sd, stylePitch, centre);
                // Chamfers on the centre line, so a pair's members keep their gap through them too.
                if (opt.corner == MeanderCorner::Mitered) centrePts = chamferCorners(centrePts, std::min(amp, stylePitch) / 3);
                if (!opt.coupled) {
                    out = shape({centrePts}, {t}, amp);
                    return;
                }
                // Members: the centre pattern offset by half the pair offset each way (miter joins keep the gap), run
                // from each member's own track ends.
                std::vector<Vec2> mine = offsetPath(centrePts, -half), theirs = offsetPath(centrePts, half);
                mine.front() = t.a;
                mine.back() = t.b;
                const double pa = (partnerTrack.a - t.a).dot(u), pb = (partnerTrack.b - t.a).dot(u);
                theirs.front() = pa <= pb ? partnerTrack.a : partnerTrack.b;
                theirs.back() = pa <= pb ? partnerTrack.b : partnerTrack.a;
                out = shape({simplifyPath(mine), simplifyPath(theirs)}, {t, partnerTrack}, amp);
            };
            auto added = [&](const std::vector<std::vector<Track>>& out) {
                return tracksLength(out[0]) - L;
            };
            // Fewest units that reach the target at the largest height, then the height that hits it exactly.
            int n = maxN;
            std::vector<std::vector<Track>> trial;
            if (opt.style != MeanderStyle::Trombone)
                for (int k = 1; k <= maxN; ++k) {
                    build(k, capAmp, trial);
                    if (added(trial) >= want) {
                        n = k;
                        break;
                    }
                }
            double lo = 0.02, hi = capAmp;
            build(n, hi, trial);
            if (added(trial) > want) {
                for (int it = 0; it < 40 && hi - lo > 1e-7; ++it) {
                    const double mid = (lo + hi) / 2;
                    build(n, mid, trial);
                    (added(trial) > want ? hi : lo) = mid;
                }
            }
            for (int shrink = 0; shrink < 4 && !done; ++shrink) {
                const double amp = hi / (1 << shrink);
                if (amp < 0.02) break;
                std::vector<std::vector<Track>> out;
                build(n, amp, out);
                w.goneT[ci] = 1;
                if (opt.coupled) w.goneT[static_cast<size_t>(ptrack)] = 1;
                bool ok = true;
                std::vector<size_t> addedIdx;
                for (size_t k = 0; k < out.size() && ok; ++k) {
                    const Track& proto = k == 0 ? t : partnerTrack;
                    for (size_t i = 0; i < out[k].size() && ok; ++i) {
                        const Track& piece = out[k][i];
                        ok = !trackHits(w, {proto.net}, proto.layer, piece, proto.width / 2, nullptr);
                        if (ok && i > 0 && i + 1 < out[k].size()) ok = clearOwnPads(proto, piece);
                    }
                    if (!ok) break;
                    for (const Track& piece : out[k]) addedIdx.push_back(w.addTrack(piece, false));
                }
                if (ok) ok = newAcuteJoins(w).empty();
                if (!ok) {
                    for (size_t idx : addedIdx) w.goneT[idx] = 1;
                    w.goneT[ci] = 0;
                    if (opt.coupled) w.goneT[static_cast<size_t>(ptrack)] = 0;
                    continue;
                }
                want -= added(out);
                done = true;
            }
        }
    }
    // Copper this session added (some may have been withdrawn again).
    double added = 0;
    for (size_t k = 0; k < w.addT.size(); ++k) {
        if (w.goneT[w.baseT() + k]) continue;
        r.addedTracks.push_back(w.addT[k]);
        if (w.addT[k].net == sel.net) added += trackLength(w.addT[k]);
    }
    if (r.addedTracks.empty()) {
        r.message = "No room for a meander here";
        return r;
    }
    for (size_t i = 0; i < w.baseT(); ++i)
        if (w.goneT[i]) {
            r.removedTracks.push_back(base.tracks[i].id);
            if (base.tracks[i].net == sel.net) added -= trackLength(base.tracks[i]);
        }
    r.ok = true;
    r.message = want <= tolerance ? (opt.coupled ? "Pair tuned to the target length" : "Tuned to the target length")
                                  : "Lengthened as far as the free space allows";
    if (!opt.apply) {
        r.after = r.before + added;
        r.changes.ok = true;
        return r;
    }
    r.changes = applyWorld(pcb, w, {}, {});
    r.after = measure(pcb);
    r.applied = true;
    return r;
}
}  // namespace

LengthTuneResult tuneTrackLength(PcbLayout& pcb, const Schematic& sch, int trackId, const LengthTuneOptions& opt) {
    LengthTuneResult r;
    long ti = -1;
    for (size_t i = 0; i < pcb.tracks.size(); ++i)
        if (pcb.tracks[i].id == trackId) ti = static_cast<long>(i);
    if (ti < 0) {
        r.message = "No track with that id";
        return r;
    }
    // Patterns, corner shapes, drag-along spans, coupled pairs, phase tuning and length rules take the advanced path;
    // the plain accordion stays exactly as it was.
    if (opt.style != MeanderStyle::Accordion || opt.corner != MeanderCorner::Square || opt.hasSpan || opt.coupled ||
        opt.phase || !pcb.settings.lengthRules.empty() || !pcb.settings.matchGroups.empty())
        return tuneTrackLengthAdvanced(pcb, sch, static_cast<size_t>(ti), opt);
    const Track sel = pcb.tracks[static_cast<size_t>(ti)];
    r.net = sel.net;
    if (sel.net < 0) {
        r.message = "The track has no net";
        return r;
    }
    r.before = routedNetLength(pcb, sel.net);
    double target = opt.target;
    double tolerance = 0.01;
    double groupLongest = 0;
    for (const auto& g : lengthGroups(sch, pcb.settings)) {
        if (std::find(g.nets.begin(), g.nets.end(), sel.net) == g.nets.end()) continue;
        if (r.group.empty()) {
            r.group = g.name;
            r.groupKind = g.kind;
        }
        for (int n : g.nets) groupLongest = std::max(groupLongest, routedNetLength(pcb, n));
        if (target <= 0) tolerance = std::max(tolerance, g.tolerance / 2);
    }
    if (target <= 0) {
        if (r.group.empty()) {
            r.message = "The net is not in a matched-length group — give a target length";
            return r;
        }
        target = groupLongest;
    }
    r.target = target;
    r.tolerance = tolerance;
    r.after = r.before;
    double want = target - r.before;
    if (want <= tolerance) {
        r.ok = true;
        r.changes.ok = true;
        r.message = want < -tolerance ? "The net is already longer than the target" : "The net is already at the target length";
        return r;
    }
    Base base(pcb, sch);
    World w(&base);
    const double clr = base.s.clearance;
    const double maxAmp = opt.maxAmplitude > 0 ? opt.maxAmplitude : 2.0;
    // The selected track first, then the net's other unlocked tracks, longest first.
    std::vector<size_t> cand{static_cast<size_t>(ti)};
    std::vector<size_t> others;
    for (size_t i = 0; i < base.tracks.size(); ++i)
        if (static_cast<long>(i) != ti && base.tracks[i].net == sel.net && !base.trackFixed[i]) others.push_back(i);
    std::stable_sort(others.begin(), others.end(), [&](size_t a, size_t b) {
        return (base.tracks[a].b - base.tracks[a].a).length() > (base.tracks[b].b - base.tracks[b].a).length();
    });
    cand.insert(cand.end(), others.begin(), others.end());
    std::vector<double> amps;
    for (double a : {0.5, 1.0, 1.5, 2.0, 3.0, 4.0})
        if (a < maxAmp - 1e-9) amps.push_back(a);
    amps.push_back(maxAmp);
    // Same-net pads the meander must stay clear of (it may not run over another pad of its own net).
    auto clearOwnPads = [&](const Track& t, Vec2 a, Vec2 b) {
        for (const auto& pd : base.pads)
            if (pd.net == t.net && pd.onLayer(t.layer) && padSegmentDistance(pd, a, b) - t.width / 2 < clr - kTol) return false;
        return true;
    };
    for (size_t ci : cand) {
        if (want <= tolerance) break;
        if (base.trackFixed[ci]) continue;
        const Track t = base.tracks[ci];
        const double wd = t.width, L = (t.b - t.a).length();
        // Leg pitch (centre to centre): the requested edge-to-edge spacing, never closer than width + clearance.
        const double pitch = opt.spacing > 0 ? std::max(wd + clr, opt.spacing + wd) : std::max(wd + clr, 3 * wd);
        const double margin = std::max(2 * wd, 0.5);
        const int maxBumps = static_cast<int>(std::floor((L - 2 * margin) / (2 * pitch)));
        if (maxBumps < 1) continue;
        bool done = false;
        for (double amax : amps) {
            if (done) break;
            const int need = std::max(1, static_cast<int>(std::ceil(want / (2 * amax))));
            for (int bumps = std::min(need, maxBumps); bumps >= 1 && !done; bumps = bumps > 1 ? (bumps + 1) / 2 : 0) {
                const double amp = std::min(amax, want / (2.0 * bumps));
                if (amp < 0.05) break;
                const double run = 2.0 * pitch * bumps;
                std::vector<double> starts;
                if (opt.hasNear && ci == static_cast<size_t>(ti)) {
                    const double along = (opt.near - t.a).dot(unit(t.b - t.a));
                    starts.push_back(std::clamp(along - run / 2, margin, std::max(margin, L - margin - run)));
                }
                for (double where : {0.5, 0.0, 1.0}) starts.push_back(margin + (L - 2 * margin - run) * where);
                for (double start : starts) {
                    if (done) break;
                    for (double sd : {1.0, -1.0}) {
                        const auto pts = accordion(t, amp, sd, pitch, bumps, start);
                        w.goneT[ci] = 1;
                        bool ok = pathClear(w, {t.net}, t.layer, pts, wd / 2);
                        for (size_t k = 1; ok && k + 2 < pts.size(); ++k) ok = clearOwnPads(t, pts[k], pts[k + 1]);
                        if (!ok) {
                            w.goneT[ci] = 0;
                            continue;
                        }
                        for (size_t k = 0; k + 1 < pts.size(); ++k) {
                            Track nt = t;
                            nt.a = pts[k];
                            nt.b = pts[k + 1];
                            w.addTrack(nt, false);
                        }
                        want -= pathLength(pts) - L;
                        done = true;
                        break;
                    }
                }
            }
        }
    }
    if (w.addT.empty()) {
        r.message = "No room for a meander on the net's tracks";
        return r;
    }
    double added = 0;
    for (const Track& t : w.addT) {
        r.addedTracks.push_back(t);
        added += (t.b - t.a).length();
    }
    for (size_t i = 0; i < w.baseT(); ++i)
        if (w.goneT[i]) {
            r.removedTracks.push_back(base.tracks[i].id);
            added -= (base.tracks[i].b - base.tracks[i].a).length();
        }
    r.ok = true;
    r.message = want <= tolerance ? "Tuned to the target length" : "Lengthened as far as the free space allows";
    if (!opt.apply) {
        r.after = r.before + added;
        r.changes.ok = true;
        return r;
    }
    r.changes = applyWorld(pcb, w, {}, {});
    r.after = routedNetLength(pcb, sel.net);
    r.applied = true;
    return r;
}

LengthTuneOptions lengthTuneOptionsFromJson(const Json& j) {
    LengthTuneOptions o;
    if (!j.isObject()) return o;
    o.target = std::max(0.0, j.get("target").asNumber(0));
    o.maxAmplitude = std::max(0.0, j.get("maxAmplitude").asNumber(0));
    o.spacing = std::max(0.0, j.get("spacing").asNumber(0));
    if (j.has("x") && j.has("y") && j.get("x").isNumber() && j.get("y").isNumber()) {
        o.hasNear = true;
        o.near = {j.get("x").asNumber(0), j.get("y").asNumber(0)};
    }
    if (j.has("apply")) o.apply = j.get("apply").asBool(true);
    const std::string style = j.get("style").asString("accordion");
    if (style == "trombone") o.style = MeanderStyle::Trombone;
    if (style == "sawtooth") o.style = MeanderStyle::Sawtooth;
    const std::string corner = j.get("corner").asString("square");
    if (corner == "mitered") o.corner = MeanderCorner::Mitered;
    if (corner == "round") o.corner = MeanderCorner::Round;
    if (j.get("fromX").isNumber() && j.get("fromY").isNumber() && j.get("toX").isNumber() && j.get("toY").isNumber()) {
        o.hasSpan = true;
        o.spanFrom = {j.get("fromX").asNumber(0), j.get("fromY").asNumber(0)};
        o.spanTo = {j.get("toX").asNumber(0), j.get("toY").asNumber(0)};
    }
    o.coupled = j.get("coupled").asBool(false);
    o.phase = j.get("phase").asBool(false);
    return o;
}

MatchLengthsResult matchTrackLengths(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                                     const LengthTuneOptions& options, double tolerance) {
    MatchLengthsResult r;
    std::vector<int> nets;
    for (int id : trackIds)
        for (const auto& t : pcb.tracks)
            if (t.id == id && t.net >= 0 && !t.teardrop && std::find(nets.begin(), nets.end(), t.net) == nets.end())
                nets.push_back(t.net);
    std::sort(nets.begin(), nets.end());
    if (nets.size() < 2) {
        r.message = "Select tracks of two or more nets to match";
        return r;
    }
    // A match group of the selection for the duration of the command (the tuner's group target: the longest).
    MatchGroup g;
    g.name = "\x01selection";
    for (int n : nets)
        if (n < static_cast<int>(sch.nets().size())) g.nets.push_back(sch.nets()[static_cast<size_t>(n)].name);
    g.tolerance = std::max(0.01, tolerance);
    pcb.settings.matchGroups.push_back(g);
    {
        const auto pads = pcb.pads(sch);
        LengthTarget lt;
        if (lengthTargetFor(pcb, sch, pads, nets.front(), lt)) r.target = lt.target;
    }
    for (int n : nets) {
        // Its longest straight track that the tuner may meander.
        int best = -1;
        double bestLen = 0;
        for (const auto& t : pcb.tracks)
            if (t.net == n && !t.arc && !t.teardrop && !t.locked && trackLength(t) > bestLen) {
                bestLen = trackLength(t);
                best = t.id;
            }
        if (best < 0) continue;
        LengthTuneOptions o = options;
        o.target = 0;
        o.apply = true;
        o.hasNear = o.hasSpan = o.coupled = o.phase = false;
        LengthTuneResult tr = tuneTrackLength(pcb, sch, best, o);
        if (tr.applied && tr.ok)
            ++r.tuned;
        else if (tr.target > 0 && tr.before >= tr.target - tr.tolerance - 1e-9)
            ++r.matched;
        else
            ++r.short_;
        r.nets.push_back(std::move(tr));
    }
    auto& groups = pcb.settings.matchGroups;
    groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const MatchGroup& m) { return m.name == g.name; }),
                 groups.end());
    r.ok = r.tuned > 0;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d of %zu nets lengthened to %.2f mm, %d already matched, %d without room", r.tuned,
                  nets.size(), r.target, r.matched, r.short_);
    r.message = buf;
    return r;
}

Json matchLengthsJson(const MatchLengthsResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    j["message"] = r.message;
    j["target"] = r.target;
    j["tuned"] = r.tuned;
    j["matched"] = r.matched;
    j["short"] = r.short_;
    Json ns = Json::array();
    for (const auto& n : r.nets) ns.push(lengthTuneJson(n));
    j["nets"] = ns;
    return j;
}

Json lengthTuneJson(const LengthTuneResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    j["message"] = r.message;
    j["net"] = r.net;
    j["group"] = r.group;
    j["groupKind"] = r.groupKind;
    j["tolerance"] = r.tolerance;
    j["before"] = r.before;
    j["after"] = r.after;
    j["target"] = r.target;
    j["applied"] = r.applied;
    Json added = Json::array(), removed = Json::array();
    for (const Track& t : r.addedTracks) added.push(trackJson(t));
    for (int id : r.removedTracks) removed.push(id);
    j["addedTracks"] = added;
    j["removedTracks"] = removed;
    j["changes"] = routeChangesJson(r.changes);
    if (!r.targetSource.empty()) {
        j["targetSource"] = r.targetSource;
        Json xs = Json::array();
        for (int n : r.xsignalNets) xs.push(n);
        j["xsignalNets"] = xs;
        j["coupled"] = r.coupled;
        j["partnerNet"] = r.partnerNet;
    }
    return j;
}

// ================================================================================================== fanout

FanoutResult fanoutComponent(PcbLayout& pcb, const Schematic& sch, int componentId, const FanoutOptions& options) {
    FanoutResult res;
    if (pcb.settings.layerCount < 2) {
        res.message = "A single-sided board has no vias to fan out to";
        return res;
    }
    const Component* comp = sch.find(componentId);
    if (!comp || !comp->pcb.placed) {
        res.message = "The part is not on the board";
        return res;
    }
    std::vector<size_t> pinCount;
    for (const auto& n : sch.nets()) pinCount.push_back(n.pins.size());
    const std::vector<Pad> all = pcb.pads(sch);
    std::vector<size_t> mine;
    Vec2 centre;
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].componentId == componentId) {
            mine.push_back(i);
            centre = centre + all[i].position;
        }
    if (mine.empty()) {
        res.message = "The part has no pads";
        return res;
    }
    centre = centre * (1.0 / static_cast<double>(mine.size()));
    const BoardSettings& s = pcb.settings;
    RouterOptions ro;
    ro.mode = options.shove ? RouterMode::Shove : RouterMode::Walkaround;
    ro.viaType = options.viaType;
    ro.snapToPads = false;
    // Pads in pad order, so the result is the same every time.
    std::stable_sort(mine.begin(), mine.end(), [&](size_t a, size_t b) { return all[a].padNumber < all[b].padNumber; });
    for (size_t i : mine) {
        const Pad pd = all[i];
        if (pd.throughHole || pd.net < 0 || static_cast<size_t>(pd.net) >= pinCount.size() ||
            pinCount[static_cast<size_t>(pd.net)] < 2) {
            ++res.skipped;
            continue;
        }
        if (options.onlyUnrouted) {
            bool routed = false;
            for (const auto& t : pcb.tracks)
                routed = routed || (t.net == pd.net && t.layer == pd.smdLayer && padSegmentDistance(pd, t.a, t.b) <= t.width / 2);
            for (const auto& v : pcb.vias)
                routed = routed || (v.net == pd.net && v.spans(pd.smdLayer) && padDistance(pd, v.position) <= v.diameter / 2);
            if (routed) {
                ++res.skipped;
                continue;
            }
        }
        // Outward: along the pad's long side for gull-wing / QFN pads, diagonal for square pads (BGA dog-bone).
        const Vec2 d = pd.position - centre;
        Vec2 dir;
        if (pd.size.x > 1.2 * pd.size.y)
            dir = {d.x >= 0 ? 1.0 : -1.0, 0};
        else if (pd.size.y > 1.2 * pd.size.x)
            dir = {0, d.y >= 0 ? 1.0 : -1.0};
        else if (std::fabs(d.x) < 1e-6 || std::fabs(d.y) < 1e-6)
            dir = std::fabs(d.x) >= std::fabs(d.y) ? Vec2{d.x >= 0 ? 1.0 : -1.0, 0} : Vec2{0, d.y >= 0 ? 1.0 : -1.0};
        else
            dir = Vec2{d.x >= 0 ? 1.0 : -1.0, d.y >= 0 ? 1.0 : -1.0} * kInvSqrt2;
        const double half = std::fabs(pd.size.x * dir.x) / 2 + std::fabs(pd.size.y * dir.y) / 2;
        const double viaR = (options.viaType == RouterViaType::Micro ? s.microviaDiameter : s.viaDiameter) / 2;
        const double base = options.distance > 0 ? options.distance : half + s.clearance + viaR + 0.05;
        const Vec2 alt[3] = {dir, unit(dir + Vec2{-dir.y, dir.x}), unit(dir + Vec2{dir.y, -dir.x})};
        bool done = false;
        std::string why;
        for (double extra : {0.0, 0.25, 0.5, 1.0}) {
            for (const Vec2& a : alt) {
                if (done) break;
                const Vec2 to = pd.position + a * (base + extra);
                InteractiveRouter r(pcb, sch);
                r.setOptions(ro);
                if (!r.beginRoute(pd.position, pd.smdLayer)) {
                    why = r.error();
                    continue;
                }
                const RoutePreview& pv = r.moveTo(to);
                const double direct = (to - pd.position).length();
                if (pv.blocked || (pv.end - to).length() > 1e-6 || pv.length > direct * 1.1 + 1e-6) {
                    why = pv.status;
                    r.cancel();
                    continue;
                }
                if (!r.addVia()) {
                    why = r.error();
                    r.cancel();
                    continue;
                }
                const RouteChanges ch = r.commit();
                if (!ch.ok) {
                    why = ch.error;
                    continue;
                }
                done = true;
            }
            if (done) break;
        }
        if (done) {
            ++res.fanned;
        } else {
            res.failedPads.push_back(pd.padNumber);
            if (res.firstProblem.empty()) res.firstProblem = why;
        }
    }
    res.ok = res.fanned > 0;
    res.message = std::to_string(res.fanned) + " pad" + (res.fanned == 1 ? "" : "s") + " fanned out";
    if (!res.failedPads.empty())
        res.message += ", " + std::to_string(res.failedPads.size()) + " without room" +
                       (res.firstProblem.empty() ? std::string() : " (" + res.firstProblem + ")");
    return res;
}

FanoutOptions fanoutOptionsFromJson(const Json& j) {
    FanoutOptions o;
    if (!j.isObject()) return o;
    if (j.has("shove")) o.shove = j.get("shove").asBool(o.shove);
    if (j.has("onlyUnrouted")) o.onlyUnrouted = j.get("onlyUnrouted").asBool(o.onlyUnrouted);
    o.distance = std::max(0.0, j.get("distance").asNumber(0));
    const std::string vt = j.get("viaType").asString(std::string());
    if (vt == "blind") o.viaType = RouterViaType::Blind;
    if (vt == "micro") o.viaType = RouterViaType::Micro;
    if (vt == "auto") o.viaType = RouterViaType::Auto;
    return o;
}

Json fanoutJson(const FanoutResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    j["message"] = r.message;
    j["fanned"] = r.fanned;
    j["skipped"] = r.skipped;
    Json failed = Json::array();
    for (int n : r.failedPads) failed.push(n);
    j["failed"] = failed;
    return j;
}


// ============================================================================================ arc corners

ArcCornersResult convertCornersToArcs(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                                      const ArcCornersOptions& opt) {
    ArcCornersResult r;
    Base B(pcb, sch);
    World w(&B);
    std::map<int, size_t> byId;
    for (size_t i = 0; i < B.tracks.size(); ++i) byId[B.tracks[i].id] = i;
    std::set<size_t> sel;
    for (int id : trackIds) {
        auto it = byId.find(id);
        if (it == byId.end()) continue;
        const Track& t = B.tracks[it->second];
        if (!t.arc && !t.locked && t.net >= 0 && trackLength(t) > 1e-9) sel.insert(it->second);
    }
    if (sel.empty()) {
        r.message = "Select straight, unlocked tracks with corners between them";
        return r;
    }
    // Chains of selected tracks joined end to end in open board (no pad, via or third track at the joint).
    struct Chain {
        std::vector<size_t> segs;
        std::vector<Vec2> pts;
    };
    std::vector<Chain> chains;
    std::set<size_t> used;
    auto nextAt = [&](size_t cur, Vec2 p) -> long {
        const Track& t = B.tracks[cur];
        const auto at = endsAt(w, t.net, t.layer, p);
        if (at.size() != 2) return -1;
        const size_t o = at[0] == cur ? at[1] : at[0];
        long via = -1;
        if (!sel.count(o) || used.count(o) || std::fabs(B.tracks[o].width - t.width) > 1e-9 ||
            nodeAnchored(w, t.net, t.layer, p, &via))
            return -1;
        return static_cast<long>(o);
    };
    for (size_t s : sel) {
        if (used.count(s)) continue;
        Chain c;
        c.segs = {s};
        c.pts = {B.tracks[s].a, B.tracks[s].b};
        used.insert(s);
        for (int fwd = 1; fwd >= 0; --fwd)
            for (size_t cur = s;;) {
                const Vec2 end = fwd ? c.pts.back() : c.pts.front();
                const long nx = nextAt(cur, end);
                if (nx < 0) break;
                const Track& n = B.tracks[static_cast<size_t>(nx)];
                const Vec2 other = samePoint(n.a, end) ? n.b : n.a;
                if (fwd) {
                    c.pts.push_back(other);
                    c.segs.push_back(static_cast<size_t>(nx));
                } else {
                    c.pts.insert(c.pts.begin(), other);
                    c.segs.insert(c.segs.begin(), static_cast<size_t>(nx));
                }
                used.insert(static_cast<size_t>(nx));
                cur = static_cast<size_t>(nx);
            }
        if (c.pts.size() >= 3) chains.push_back(c);
    }
    // Differential pair partners are filleted together (concentric arcs).
    std::map<int, int> partner;
    for (const auto& g : lengthGroups(sch, pcb.settings))
        if (g.kind == "pair" && g.nets.size() == 2) {
            partner[g.nets[0]] = g.nets[1];
            partner[g.nets[1]] = g.nets[0];
        }
    std::vector<std::vector<size_t>> groups;
    std::set<size_t> grouped;
    for (size_t i = 0; i < chains.size(); ++i) {
        if (grouped.count(i)) continue;
        std::vector<size_t> g{i};
        grouped.insert(i);
        const Track& ti = B.tracks[chains[i].segs[0]];
        auto p = partner.find(ti.net);
        if (p != partner.end()) {
            size_t best = chains.size();
            double bestD = 1e18;
            for (size_t j = 0; j < chains.size(); ++j) {
                if (grouped.count(j)) continue;
                const Track& tj = B.tracks[chains[j].segs[0]];
                if (tj.net != p->second || tj.layer != ti.layer) continue;
                const double d = (chains[j].pts.front() - chains[i].pts.front()).length();
                if (d < bestD) {
                    bestD = d;
                    best = j;
                }
            }
            if (best < chains.size()) {
                g.push_back(best);
                grouped.insert(best);
            }
        }
        groups.push_back(g);
    }
    std::vector<Track> added;
    for (const auto& g : groups) {
        std::vector<std::vector<Vec2>> pts;
        std::vector<Track> protos;
        std::vector<int> nets;
        int corners = 0;
        for (size_t ci : g) {
            pts.push_back(chains[ci].pts);
            Track p = B.tracks[chains[ci].segs[0]];
            p.id = -1;
            p.locked = false;
            protos.push_back(p);
            nets.push_back(p.net);
            corners += static_cast<int>(chains[ci].pts.size()) - 2;
            for (size_t s : chains[ci].segs) w.goneT[s] = 1;
        }
        const double radius = opt.radius > 0 ? opt.radius : std::max(0.5, 4 * protos[0].width);
        auto clearArc = [&](size_t k, const Track& arc) {
            return !trackHits(w, nets, protos[k].layer, arc, protos[k].width / 2, nullptr);
        };
        const auto res = filletArcRunsChecked(pts, protos, radius, std::max(protos[0].width, 0.05), clearArc,
                                              [&](int a, int b) { return B.clearance(a, b); }, nullptr);
        int arcs = 0;
        for (const auto& run : res)
            for (const Track& t : run) arcs += t.arc ? 1 : 0;
        if (arcs == 0) {
            for (size_t ci : g)
                for (size_t s : chains[ci].segs) w.goneT[s] = 0;
            r.kept += corners;
            continue;
        }
        r.converted += arcs;
        r.kept += corners - arcs;
        for (size_t ci : g)
            for (size_t s : chains[ci].segs) r.removedTracks.push_back(B.tracks[s].id);
        for (const auto& run : res)
            for (const Track& t : mergeCollinear(run)) {
                w.addTrack(t, true);
                added.push_back(t);
            }
    }
    r.addedTracks = added;
    r.ok = r.converted > 0;
    char buf[160];
    std::snprintf(buf, sizeof buf, "%d corner%s converted to arcs%s", r.converted, r.converted == 1 ? "" : "s",
                  r.kept > 0 ? (", " + std::to_string(r.kept) + " kept sharp (no room for an arc)").c_str() : "");
    r.message = r.ok ? buf : (r.kept > 0 ? "No corner has room for an arc that keeps clearance"
                                         : "No corners between the selected tracks");
    if (r.ok && opt.apply) {
        r.changes = applyWorld(pcb, w, added, {});
        r.applied = true;
    }
    return r;
}

Json arcCornersJson(const ArcCornersResult& r) {
    Json j = Json::object();
    j["ok"] = r.ok;
    j["message"] = r.message;
    j["converted"] = r.converted;
    j["kept"] = r.kept;
    j["applied"] = r.applied;
    Json add = Json::array();
    for (const Track& t : r.addedTracks) add.push(trackJson(t));
    j["addedTracks"] = add;
    Json rem = Json::array();
    for (int id : r.removedTracks) rem.push(id);
    j["removedTracks"] = rem;
    j["changes"] = routeChangesJson(r.changes);
    return j;
}

// ========================================================================================== board commands

namespace {

std::vector<Track> straightTracks(const std::vector<Vec2>& pts, const Track& proto) {
    std::vector<Track> out;
    for (size_t k = 0; k + 1 < pts.size(); ++k) {
        if ((pts[k + 1] - pts[k]).length() < 1e-9) continue;
        Track t = proto;
        t.id = -1;
        t.locked = false;
        t.arc = false;
        t.teardrop = false;
        t.a = pts[k];
        t.b = pts[k + 1];
        out.push_back(t);
    }
    return out;
}

int netByName(const Schematic& sch, const std::string& name) {
    if (name.empty()) return sch.groundNet();
    for (const auto& n : sch.nets())
        if (n.name == name) return n.index;
    return -1;
}

/// A straight-line pull-tight of a polyline with 45° shortcuts that `clear` accepts and make no acute corner.
std::vector<Vec2> tightenPolyline(const std::vector<Vec2>& pts, const std::function<bool(const std::vector<Vec2>&)>& clear) {
    const size_t n = pts.size();
    if (n < 3) return pts;
    std::vector<Vec2> out{pts[0]};
    size_t i = 0;
    while (i + 1 < n) {
        bool done = false;
        for (size_t j = n - 1; j >= i + 2 && !done; --j) {
            double orig = 0;
            for (size_t k = i; k < j; ++k) orig += (pts[k + 1] - pts[k]).length();
            for (const auto& link : postureLinks(pts[i], pts[j], RoutePosture::Diagonal45, false)) {
                if (link.size() < 2 || pathLength(link) >= orig - 1e-6) continue;
                const Vec2 inDir = out.size() >= 2 ? out.back() - out[out.size() - 2] : Vec2{};
                if (acuteJoin(inDir, link[1] - link[0])) continue;
                if (j + 1 < n && acuteJoin(link.back() - link[link.size() - 2], pts[j + 1] - pts[j])) continue;
                if (!clear(link)) continue;
                out.insert(out.end(), link.begin() + 1, link.end());
                i = j;
                done = true;
                break;
            }
        }
        if (!done) {
            out.push_back(pts[i + 1]);
            ++i;
        }
    }
    return simplifyPath(out);
}

BoardEditResult finishEdit(PcbLayout& pcb, World& w, BoardEditResult& r, const std::vector<Track>& tracks,
                           const std::vector<Via>& vias, bool apply) {
    r.addedTracks = tracks;
    r.addedVias = vias;
    for (size_t i = 0; i < w.baseT(); ++i)
        if (w.goneT[i]) r.removedTracks.push_back(w.b->tracks[i].id);
    for (size_t i = 0; i < w.baseV(); ++i)
        if (w.goneV[i]) r.removedVias.push_back(w.b->vias[i].id);
    r.ok = !tracks.empty() || !vias.empty() || !r.removedTracks.empty();
    if (r.ok && apply) {
        r.changes = applyWorld(pcb, w, tracks, vias);
        r.applied = true;
    }
    return r;
}

}  // namespace

BoardEditResult addTeardrops(PcbLayout& pcb, const Schematic& sch, const TeardropOptions& opt) {
    BoardEditResult r;
    Base B(pcb, sch);
    World w(&B);
    const std::set<int> want(opt.trackIds.begin(), opt.trackIds.end());
    const double frac = std::clamp(opt.length, 0.3, 3.0);
    std::vector<Track> added;
    std::vector<size_t> found;
    for (size_t i = 0; i < B.tracks.size(); ++i) {
        const Track t = B.tracks[i];
        if (t.teardrop || t.arc || t.net < 0 || (!want.empty() && !want.count(t.id))) continue;
        const double L = trackLength(t);
        if (L < 1e-6) continue;
        for (int e = 0; e < 2; ++e) {
            const Vec2 E = e == 0 ? t.a : t.b;
            const Vec2 u = unit((e == 0 ? t.b : t.a) - E), nrm{-u.y, u.x};
            // The via or pad the end sits in: its size across the track and its depth along it.
            double across = 0, depth = 0;
            Vec2 C;
            bool isVia = false;
            long padIdx = -1;
            if (opt.vias)
                for (size_t v = 0; v < B.vias.size() && across <= 0; ++v) {
                    const Via& via = B.vias[v];
                    if (via.net == t.net && via.spans(t.layer) && (via.position - E).length() <= via.diameter / 2 - 1e-9) {
                        across = depth = via.diameter;
                        C = via.position;
                        isVia = true;
                    }
                }
            if (across <= 0 && opt.pads) {
                w.padsIn(Rect::centered(E, 1e-3, 1e-3), found);
                for (size_t pi : found) {
                    const Pad& p = B.pads[pi];
                    if (p.net != t.net || !p.onLayer(t.layer) || padDistance(p, E) > 0) continue;
                    across = p.round ? std::min(p.size.x, p.size.y) : std::fabs(p.size.x * u.y) + std::fabs(p.size.y * u.x);
                    depth = p.round ? across : std::fabs(p.size.x * u.x) + std::fabs(p.size.y * u.y);
                    C = p.position;
                    padIdx = static_cast<long>(pi);
                    break;
                }
            }
            if (across <= 0 || t.width >= 0.9 * across) continue;
            // Already has a teardrop at this end.
            bool has = false;
            auto mine = [&](const Track& o) {
                return o.teardrop && o.net == t.net && o.layer == t.layer && (o.a - E).length() <= across / 2 + 1e-6 &&
                       trackPointDistance(t, o.b) <= 1e-6;
            };
            for (const auto& o : B.tracks) has = has || mine(o);
            for (const auto& o : added) has = has || mine(o);
            if (has) continue;
            const double edge = std::max(0.0, (C - E).dot(u) + depth / 2);
            const double half = across / 2 - t.width / 2;
            const double step = 0.8 * t.width;
            const int K = std::max(1, static_cast<int>(std::ceil(half / step)));
            bool ok = false;
            for (double scale : {1.0, 0.5}) {
                double ltd = frac * across * scale;
                ltd = std::min(ltd, 0.8 * L - edge);
                if (ltd < t.width) break;
                std::vector<Track> lines;
                int idx = 0;
                for (int k = 1; k <= K; ++k)
                    for (double sgn : {1.0, -1.0}) {
                        const double o = sgn * std::min(k * step, half);
                        Track td = t;
                        td.id = -1;
                        td.locked = false;
                        td.teardrop = true;
                        td.a = E + nrm * o;
                        td.b = E + u * (edge + ltd - 0.02 * idx++);  // staggered ends: no two lines share one
                        lines.push_back(td);
                    }
                bool fits = true;
                for (const Track& td : lines) {
                    const bool inside = isVia ? (td.a - C).length() <= across / 2 - td.width / 2 + 1e-9
                                              : padDistance(B.pads[static_cast<size_t>(padIdx)], td.a) <= 0;
                    fits = fits && inside && !trackHits(w, {t.net}, t.layer, td, td.width / 2, nullptr);
                }
                if (!fits) continue;
                for (const Track& td : lines) {
                    w.addTrack(td, true);
                    added.push_back(td);
                }
                ok = true;
                break;
            }
            if (ok)
                ++r.added;
            else
                ++r.skipped;
        }
    }
    finishEdit(pcb, w, r, added, {}, opt.apply);
    r.message = std::to_string(r.added) + (r.added == 1 ? " teardrop" : " teardrops") +
                (r.skipped > 0 ? ", " + std::to_string(r.skipped) + " without room" : std::string());
    return r;
}

BoardEditResult removeTeardrops(PcbLayout& pcb, const std::vector<int>& trackIds) {
    BoardEditResult r;
    const std::set<int> want(trackIds.begin(), trackIds.end());
    std::vector<const Track*> owners;
    for (const auto& t : pcb.tracks)
        if (!t.teardrop && want.count(t.id)) owners.push_back(&t);
    std::set<int> gone;
    for (const auto& t : pcb.tracks) {
        if (!t.teardrop) continue;
        bool hit = want.empty();
        for (const Track* o : owners)
            hit = hit || (o->net == t.net && o->layer == t.layer && trackPointDistance(*o, t.b) <= 1e-6);
        if (hit) gone.insert(t.id);
    }
    for (const auto& t : pcb.tracks)
        if (gone.count(t.id)) r.changes.removedTracks.push_back(t);
    pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return gone.count(t.id) > 0; }),
                     pcb.tracks.end());
    r.removedTracks.assign(gone.begin(), gone.end());
    r.added = static_cast<int>(gone.size());
    r.ok = !gone.empty();
    r.applied = r.ok;
    r.changes.ok = true;
    r.message = std::to_string(gone.size()) + " teardrop track" + (gone.size() == 1 ? "" : "s") + " removed";
    return r;
}

int pruneTeardrops(PcbLayout& pcb, const Schematic& sch, std::vector<Track>* removed) {
    bool any = false;
    for (const auto& t : pcb.tracks) any = any || t.teardrop;
    if (!any) return 0;
    const auto pads = pcb.pads(sch);
    std::set<int> gone;
    for (const auto& t : pcb.tracks) {
        if (!t.teardrop) continue;
        bool onTrack = false, inCopper = false;
        for (const auto& o : pcb.tracks)
            onTrack = onTrack || (!o.teardrop && o.net == t.net && o.layer == t.layer && trackPointDistance(o, t.b) <= 1e-6);
        for (const auto& v : pcb.vias) inCopper = inCopper || (v.net == t.net && v.spans(t.layer) && (v.position - t.a).length() <= v.diameter / 2);
        for (const auto& p : pads) inCopper = inCopper || (p.net == t.net && p.onLayer(t.layer) && padDistance(p, t.a) <= 0);
        if (!onTrack || !inCopper) gone.insert(t.id);
    }
    if (removed)
        for (const auto& t : pcb.tracks)
            if (gone.count(t.id)) removed->push_back(t);
    pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return gone.count(t.id) > 0; }),
                     pcb.tracks.end());
    return static_cast<int>(gone.size());
}

BoardEditResult stitchVias(PcbLayout& pcb, const Schematic& sch, const ViaPatternOptions& opt) {
    BoardEditResult r;
    const int net = netByName(sch, opt.net);
    if (net < 0) {
        r.message = "No such net";
        return r;
    }
    const auto& fills = pcb.zoneFills(sch);
    std::vector<const ZoneFill*> own;
    for (const auto& f : fills)
        if (f.net == net && f.islands > 0) own.push_back(&f);
    std::set<int> layers;
    for (const ZoneFill* f : own) layers.insert(f->layer);
    if (layers.size() < 2) {
        r.message = "Stitching needs pours or planes of the net on two layers";
        return r;
    }
    Base B(pcb, sch);
    World w(&B);
    const BoardSettings& s = pcb.settings;
    const double pitch = opt.pitch > 0 ? std::max(opt.pitch, s.viaDiameter + s.clearance) : 2.0;
    const Rect area = opt.hasArea ? opt.area : Rect(0, 0, s.width, s.height);
    Via proto;
    proto.net = net;
    proto.drill = s.viaDrill;
    proto.diameter = s.viaDiameter;
    std::vector<Via> added;
    for (double y = area.y0 + pitch / 2; y <= area.y1 - pitch / 2 + 1e-9; y += pitch)
        for (double x = area.x0 + pitch / 2; x <= area.x1 - pitch / 2 + 1e-9; x += pitch) {
            Via v = proto;
            v.position = {x, y};
            if (!s.contains(v.position)) continue;
            std::set<int> on;
            for (const ZoneFill* f : own)
                if (f->islandNear(v.position, v.diameter / 2) >= 0 && f->islandAt(v.position) >= 0) on.insert(f->layer);
            if (on.size() < 2) continue;
            if (viaHits(w, v, SIZE_MAX, nullptr)) {
                ++r.skipped;
                continue;
            }
            w.addVia(v, true);
            added.push_back(v);
        }
    r.added = static_cast<int>(added.size());
    finishEdit(pcb, w, r, {}, added, opt.apply);
    r.message = std::to_string(r.added) + " stitching vias" +
                (r.skipped > 0 ? ", " + std::to_string(r.skipped) + " grid points without room" : std::string());
    return r;
}

BoardEditResult shieldTracks(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds, const ViaPatternOptions& opt) {
    BoardEditResult r;
    const int net = netByName(sch, opt.net);
    if (net < 0) {
        r.message = "No ground net to shield with";
        return r;
    }
    Base B(pcb, sch);
    World w(&B);
    const BoardSettings& s = pcb.settings;
    const double pitch = opt.pitch > 0 ? std::max(opt.pitch, s.viaDiameter + s.clearance) : 1.0;
    Via proto;
    proto.net = net;
    proto.drill = s.viaDrill;
    proto.diameter = s.viaDiameter;
    const std::set<int> want(trackIds.begin(), trackIds.end());
    std::vector<Via> added;
    int floating = 0;
    const auto& fills = pcb.zoneFills(sch);
    for (const auto& t : B.tracks) {
        if (!want.count(t.id) || t.net == net || t.teardrop) continue;
        const double L = trackLength(t);
        if (L < pitch / 2) continue;
        const double d = opt.offset > 0 ? opt.offset : t.width / 2 + B.clearance(t.net, net) + proto.diameter / 2 + 1e-3;
        for (double at = pitch / 2; at <= L - pitch / 4 + 1e-9; at += pitch) {
            const double f = at / L;
            const Vec2 p = trackPointAt(t, f);
            const Vec2 tan = unit(trackPointAt(t, std::min(1.0, f + 1e-4)) - trackPointAt(t, std::max(0.0, f - 1e-4)));
            const Vec2 nrm{-tan.y, tan.x};
            for (double side : {1.0, -1.0}) {
                Via v = proto;
                v.position = p + nrm * (side * d);
                if (viaHits(w, v, SIZE_MAX, nullptr)) {
                    ++r.skipped;
                    continue;
                }
                bool connected = false;
                for (const auto& z : fills) connected = connected || (z.net == net && z.islandAt(v.position) >= 0);
                floating += connected ? 0 : 1;
                w.addVia(v, true);
                added.push_back(v);
            }
        }
    }
    r.added = static_cast<int>(added.size());
    finishEdit(pcb, w, r, {}, added, opt.apply);
    r.message = std::to_string(r.added) + " shielding vias" +
                (r.skipped > 0 ? ", " + std::to_string(r.skipped) + " places without room" : std::string()) +
                (floating > 0 ? "; " + std::to_string(floating) + " reach no pour or plane of the net yet" : std::string());
    return r;
}

BoardEditResult glossTracks(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds, const GlossOptions& opt) {
    BoardEditResult r;
    Base B(pcb, sch);
    World w(&B);
    const std::set<int> want(trackIds.begin(), trackIds.end());
    std::set<size_t> seen;
    std::vector<Track> added;
    double saved = 0;
    for (size_t i = 0; i < B.tracks.size(); ++i) {
        const Track& t = B.tracks[i];
        if (!want.count(t.id) || seen.count(i) || B.trackFixed[i] || t.teardrop || t.net < 0) continue;
        const Line L = extractLine(w, i);
        for (size_t sgm : L.segs) seen.insert(sgm);
        bool skip = false;
        for (size_t sgm : L.segs) skip = skip || B.tracks[sgm].teardrop || B.tracks[sgm].arc;
        if (skip) continue;
        const double orig = pathLength(L.pts);
        for (size_t sgm : L.segs) w.goneT[sgm] = 1;
        auto clear = [&](const std::vector<Vec2>& q) { return pathClear(w, {L.net}, L.layer, q, L.width / 2); };
        std::vector<Vec2> better = tightenPolyline(L.pts, clear);
        if (opt.retrace) {
            const GridPath g = gridRoute(w, {L.net}, L.layer, L.width / 2, L.pts.front(), L.pts.back(),
                                         RoutePosture::Diagonal45, Vec2{});
            if (g.reached && g.pts.size() >= 2 && samePoint(g.pts.front(), L.pts.front(), 1e-9) &&
                samePoint(g.pts.back(), L.pts.back(), 1e-9) && pathLength(g.pts) < pathLength(better) - 1e-3 && clear(g.pts))
                better = g.pts;
        }
        const double now = pathLength(better);
        if (now < orig - 1e-4) {
            std::vector<size_t> idx;
            const std::vector<Track> news = straightTracks(better, B.tracks[L.segs.front()]);
            for (const Track& nt : news) idx.push_back(w.addTrack(nt, true));
            if (newAcuteJoins(w).empty()) {
                added.insert(added.end(), news.begin(), news.end());
                saved += orig - now;
                ++r.added;
                continue;
            }
            for (size_t k : idx) w.goneT[k] = 1;
        }
        for (size_t sgm : L.segs) w.goneT[sgm] = 0;
    }
    finishEdit(pcb, w, r, added, {}, opt.apply);
    char buf[120];
    std::snprintf(buf, sizeof buf, "%d line%s improved, %.2f mm shorter", r.added, r.added == 1 ? "" : "s", saved);
    r.message = r.added > 0 ? buf : "The selected lines are already as tight as the board allows";
    return r;
}

Json boardEditJson(const BoardEditResult& r, int layerCount) {
    Json j = Json::object();
    j["ok"] = r.ok;
    j["message"] = r.message;
    j["added"] = r.added;
    j["skipped"] = r.skipped;
    j["applied"] = r.applied;
    Json ts = Json::array(), vs = Json::array(), rt = Json::array(), rv = Json::array();
    for (const Track& t : r.addedTracks) ts.push(trackJson(t));
    for (const Via& v : r.addedVias) vs.push(viaJson(v, layerCount));
    for (int id : r.removedTracks) rt.push(id);
    for (int id : r.removedVias) rv.push(id);
    j["addedTracks"] = ts;
    j["addedVias"] = vs;
    j["removedTracks"] = rt;
    j["removedVias"] = rv;
    j["changes"] = routeChangesJson(r.changes);
    return j;
}

// ================================================================================================= JSON

RouterOptions routerOptionsFromJson(const Json& j, RouterOptions o) {
    if (!j.isObject()) return o;
    const std::string mode = j.get("mode").asString(std::string());
    if (mode == "shove") o.mode = RouterMode::Shove;
    if (mode == "walkaround") o.mode = RouterMode::Walkaround;
    if (mode == "highlight") o.mode = RouterMode::Highlight;
    if (mode == "stop") o.mode = RouterMode::Stop;
    const Json& posture = j.get("posture");
    const std::string ps = posture.isNumber() ? std::to_string(posture.asInt()) : posture.asString(std::string());
    if (ps == "45") o.posture = RoutePosture::Diagonal45;
    if (ps == "90") o.posture = RoutePosture::Orthogonal90;
    if (ps == "free") o.posture = RoutePosture::Free;
    if (j.has("swapPosture")) o.swapPosture = j.get("swapPosture").asBool(o.swapPosture);
    if (j.has("width")) o.width = std::max(0.0, j.get("width").asNumber(o.width));
    if (j.has("pairGap")) o.pairGap = std::max(0.0, j.get("pairGap").asNumber(o.pairGap));
    if (j.has("snap")) o.snapToPads = j.get("snap").asBool(o.snapToPads);
    const std::string vt = j.get("viaType").asString(std::string());
    if (vt == "through") o.viaType = RouterViaType::Through;
    if (vt == "blind") o.viaType = RouterViaType::Blind;
    if (vt == "micro") o.viaType = RouterViaType::Micro;
    if (vt == "auto") o.viaType = RouterViaType::Auto;
    if (j.has("cornerRadius")) o.cornerRadius = j.get("cornerRadius").asNumber(o.cornerRadius);
    if (j.has("arcCorners")) o.arcCorners = j.get("arcCorners").asBool(o.arcCorners);
    if (j.has("removeLoops")) o.removeLoops = j.get("removeLoops").asBool(o.removeLoops);
    if (j.has("teardrops")) o.autoTeardrops = j.get("teardrops").asBool(o.autoTeardrops);
    if (j.has("hug")) o.hugDrag = j.get("hug").asBool(o.hugDrag);
    if (j.has("shoveLimit")) o.shoveLimit = std::clamp(j.get("shoveLimit").asInt(o.shoveLimit), 1, 10000);
    return o;
}

Json routePreviewJson(const RoutePreview& p) {
    Json j = Json::object();
    j["active"] = p.active;
    j["kind"] = p.kind;
    j["status"] = p.status;
    j["blocked"] = p.blocked;
    j["reachedTarget"] = p.reachedTarget;
    Json nets = Json::array();
    for (int n : p.nets) nets.push(n);
    j["nets"] = nets;
    j["layer"] = p.layer;
    j["width"] = p.width;
    j["gap"] = p.gap;
    j["endX"] = p.end.x;
    j["endY"] = p.end.y;
    j["length"] = p.length;
    j["netLength"] = p.netLength;
    if (p.aborted) j["aborted"] = true;
    j["targetLength"] = p.targetLength;
    auto tracks = [](const std::vector<Track>& ts) {
        Json a = Json::array();
        for (const Track& t : ts) a.push(trackJson(t));
        return a;
    };
    auto vias = [&](const std::vector<Via>& vs) {
        Json a = Json::array();
        for (const Via& v : vs) a.push(viaJson(v, p.layerCount));
        return a;
    };
    auto ids = [](const std::vector<int>& v) {
        Json a = Json::array();
        for (int i : v) a.push(i);
        return a;
    };
    j["placed"] = tracks(p.placed);
    j["head"] = tracks(p.head);
    j["vias"] = vias(p.vias);
    j["shovedTracks"] = tracks(p.shovedTracks);
    j["shovedVias"] = vias(p.shovedVias);
    j["hiddenTracks"] = ids(p.hiddenTracks);
    j["hiddenVias"] = ids(p.hiddenVias);
    Json col = Json::array();
    for (const RouteCollision& c : p.collisions) {
        Json k = Json::object();
        k["kind"] = c.kind;
        k["id"] = c.id;
        k["x"] = c.at.x;
        k["y"] = c.at.y;
        k["ax"] = c.a.x;
        k["ay"] = c.a.y;
        k["bx"] = c.b.x;
        k["by"] = c.b.y;
        k["w"] = c.size.x;
        k["h"] = c.size.y;
        k["width"] = c.width;
        col.push(k);
    }
    j["collisions"] = col;
    return j;
}

Json routeChangesJson(const RouteChanges& c) {
    Json j = Json::object();
    j["ok"] = c.ok;
    if (!c.error.empty()) j["error"] = c.error;
    Json rt = Json::array(), rv = Json::array(), at = Json::array(), av = Json::array();
    for (const Track& t : c.removedTracks) rt.push(trackJson(t));
    for (const Via& v : c.removedVias) rv.push(viaJson(v));
    for (int i : c.addedTracks) at.push(i);
    for (int i : c.addedVias) av.push(i);
    j["removedTracks"] = rt;
    j["removedVias"] = rv;
    j["addedTracks"] = at;
    j["addedVias"] = av;
    return j;
}

}  // namespace sieda
