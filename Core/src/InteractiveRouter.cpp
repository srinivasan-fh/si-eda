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
#include <cmath>
#include <deque>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <tuple>

#include "sieda/Isolation.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Stackup.hpp"

namespace sieda {
namespace {

constexpr double kTol = 1e-6;       // clearance comparisons
constexpr double kMargin = 1e-7;    // shoved copper keeps this much beyond the exact clearance (rounding)
constexpr double kJoin = 1e-4;      // track ends closer than this are one node
constexpr double kMinEtchGap = 0.1; // DRC: gap inside one footprint where a track is still in its own pad
constexpr double kInvSqrt2 = 0.70710678118654752440;

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
    enum Type { Point, Segment, Box } type = Point;
    Vec2 a, b;
    Rect box;
    double radius = 0;
    std::vector<Vec2> points() const {
        if (type == Point) return {a};
        if (type == Segment) return {a, b};
        return {{box.x0, box.y0}, {box.x1, box.y0}, {box.x1, box.y1}, {box.x0, box.y1}};
    }
    /// Edge distance from this shape to segment p-q.
    double distanceTo(Vec2 p, Vec2 q) const {
        if (type == Point) return pointSegmentDistance(a, p, q) - radius;
        if (type == Segment) return segmentSegmentDistance(a, b, p, q) - radius;
        return segmentRectDistance(p, q, box) - radius;
    }
};

Shape trackShape(const Track& t) {
    Shape s;
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
    mutable std::vector<unsigned> stamp;
};

struct Base {
    BoardSettings s;
    std::vector<Pad> pads;
    std::vector<Track> tracks;
    std::vector<Via> vias;
    std::vector<char> trackFixed;  // locked, or a tamper-mesh stripe
    std::vector<char> viaFixed;    // inside a pad of its net (moving it would leave the pad), or a mesh via
    std::map<int, std::vector<size_t>> compPads;
    std::map<int, std::string> refs;
    std::vector<std::string> netNames;
    std::vector<size_t> pinCount;
    std::map<int, std::pair<double, double>> ranges;
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
    mutable unsigned epoch = 0;

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
        for (size_t i = 0; i < tracks.size(); ++i) trackFixed[i] = tracks[i].locked || meshNets.count(tracks[i].net);
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

        area = Rect(0, 0, std::max(1.0, s.width), std::max(1.0, s.height)).inflated(20);
        cell = std::max(1.0, std::max(area.width(), area.height()) / 160);
        cols = std::max(1, static_cast<int>(std::ceil(area.width() / cell)));
        rows = std::max(1, static_cast<int>(std::ceil(area.height() / cell)));
        for (Grid* g : {&padGrid, &trackGrid, &viaGrid}) g->cells.assign(static_cast<size_t>(cols * rows), {});
        for (size_t i = 0; i < pads.size(); ++i) insert(padGrid, pads[i].bounds(), i);
        for (size_t i = 0; i < tracks.size(); ++i)
            insert(trackGrid, segmentBox(tracks[i].a, tracks[i].b, tracks[i].width / 2), i);
        for (size_t i = 0; i < vias.size(); ++i)
            insert(viaGrid, Rect::centered(vias[i].position, vias[i].diameter, vias[i].diameter), i);
        padGrid.stamp.assign(pads.size(), 0);
        trackGrid.stamp.assign(tracks.size(), 0);
        viaGrid.stamp.assign(vias.size(), 0);
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
    void query(const Grid& g, const Rect& r, std::vector<size_t>& out) const {
        out.clear();
        if (++epoch == 0) {
            for (const Grid* q : {&padGrid, &trackGrid, &viaGrid}) std::fill(q->stamp.begin(), q->stamp.end(), 0u);
            epoch = 1;
        }
        int i0, i1, j0, j1;
        cellRange(r, i0, i1, j0, j1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                for (size_t idx : g.cells[static_cast<size_t>(j * cols + i)])
                    if (g.stamp[idx] != epoch) {
                        g.stamp[idx] = epoch;
                        out.push_back(idx);
                    }
        std::sort(out.begin(), out.end());
    }

    /// IPC-2221 spacing for the two nets' potential difference (0 when unknown) — the DRC's voltage rule.
    double voltageClearance(int a, int b) const {
        if (a < 0 || b < 0 || a == b) return 0;
        auto ia = ranges.find(a), ib = ranges.find(b);
        if (ia == ranges.end() || ib == ranges.end()) return 0;
        if (pinCount[static_cast<size_t>(a)] < 2 || pinCount[static_cast<size_t>(b)] < 2) return 0;
        const double dv = std::max(std::fabs(ia->second.second - ib->second.first),
                                   std::fabs(ib->second.second - ia->second.first));
        const double need = ipc2221Clearance(dv, s.highAltitude, s.coated());
        return need > s.clearance + 1e-3 ? need : 0;
    }
    /// Copper-to-copper clearance between two nets: the design rule, the voltage spacing and the isolation gap.
    double clearance(int a, int b, bool barrierPad = false) const {
        double c = std::max(s.clearance, voltageClearance(a, b));
        if (!barrierPad && !domain.empty() && a >= 0 && b >= 0 && a != b) {
            const int da = a < static_cast<int>(domain.size()) ? domain[static_cast<size_t>(a)] : -1;
            const int db = b < static_cast<int>(domain.size()) ? domain[static_cast<size_t>(b)] : -1;
            if (da >= 0 && db >= 0 && da != db) c = std::max(c, s.isolationGap);
        }
        return c;
    }
};

// ------------------------------------------------------------------------------------------- overlay (World)

struct World {
    const Base* b = nullptr;
    std::vector<char> goneT, goneV;  // base + added
    std::vector<Track> addT;
    std::vector<Via> addV;
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
        return i < baseT() ? b->trackFixed[i] != 0 : fixAddT[i - baseT()] != 0;
    }
    bool viaFixed(size_t i) const {
        if (netFixed(via(i).net)) return true;
        return i < baseV() ? b->viaFixed[i] != 0 : fixAddV[i - baseV()] != 0;
    }
    size_t addTrack(Track t, bool fixed) {
        t.id = -1;
        t.locked = false;
        addT.push_back(t);
        fixAddT.push_back(fixed);
        goneT.push_back(0);
        return nT() - 1;
    }
    size_t addVia(Via v, bool fixed) {
        v.id = -1;
        addV.push_back(v);
        fixAddV.push_back(fixed);
        goneV.push_back(0);
        return nV() - 1;
    }
    void tracksIn(const Rect& r, std::vector<size_t>& out) const {
        std::vector<size_t> base;
        b->query(b->trackGrid, r, base);
        out.clear();
        for (size_t i : base)
            if (aliveT(i)) out.push_back(i);
        for (size_t k = 0; k < addT.size(); ++k) {
            const size_t i = baseT() + k;
            if (aliveT(i) && segmentBox(addT[k].a, addT[k].b, addT[k].width / 2).intersects(r)) out.push_back(i);
        }
    }
    void viasIn(const Rect& r, std::vector<size_t>& out) const {
        std::vector<size_t> base;
        b->query(b->viaGrid, r, base);
        out.clear();
        for (size_t i : base)
            if (aliveV(i)) out.push_back(i);
        for (size_t k = 0; k < addV.size(); ++k) {
            const size_t i = baseV() + k;
            if (aliveV(i) && Rect::centered(addV[k].position, addV[k].diameter, addV[k].diameter).intersects(r))
                out.push_back(i);
        }
    }
    void padsIn(const Rect& r, std::vector<size_t>& out) const { b->query(b->padGrid, r, out); }
};

// ------------------------------------------------------------------------------------------------ clearance checks

enum class HitKind { Pad, Track, Via, Edge, Hole, Plane, Mesh };
struct Hit {
    HitKind kind = HitKind::Pad;
    size_t index = 0;
    bool fixed = true;
    bool operator==(const Hit& o) const { return kind == o.kind && index == o.index; }
    bool operator<(const Hit& o) const { return std::tie(kind, index) < std::tie(o.kind, o.index); }
};

bool inNets(const std::vector<int>& nets, int n) { return n >= 0 && std::find(nets.begin(), nets.end(), n) != nets.end(); }

double clearanceTo(const Base& B, const std::vector<int>& nets, int other, bool barrierPad = false) {
    double c = 0;
    for (int n : nets) c = std::max(c, B.clearance(n, other, barrierPad));
    return nets.empty() ? B.s.clearance : c;
}

/// Everything copper of `nets` along a-b (half width hw) on `layer` is too close to. Stops at the first hit when
/// `out` is null. Returns true if there is any.
bool segmentHits(const World& w, const std::vector<int>& nets, int layer, Vec2 a, Vec2 b, double hw,
                 std::vector<Hit>* out) {
    const Base& B = *w.b;
    const BoardSettings& s = B.s;
    bool any = false;
    auto hit = [&](HitKind k, size_t i, bool fixed) {
        any = true;
        if (out) out->push_back({k, i, fixed});
        return out == nullptr;  // stop
    };
    const Rect box = segmentBox(a, b, hw + B.maxClearance + 0.05);
    std::vector<size_t> found;
    w.padsIn(box, found);
    for (size_t i : found) {
        const Pad& p = B.pads[i];
        if (!p.onLayer(layer) || inNets(nets, p.net)) continue;
        const bool barrier = B.barrierComps.count(p.componentId) > 0;
        const double d = padSegmentDistance(p, a, b) - hw;
        double need = clearanceTo(B, nets, p.net, barrier);
        if (d >= need - kTol) continue;
        // Where the track is still inside its own pad of the same footprint, the gap is the package's pad gap.
        const Vec2 c = closestOnSegment(p.position, a, b);
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
    w.tracksIn(box, found);
    for (size_t i : found) {
        const Track& t = w.track(i);
        if (t.layer != layer || inNets(nets, t.net)) continue;
        const double d = segmentSegmentDistance(a, b, t.a, t.b) - hw - t.width / 2;
        if (d < clearanceTo(B, nets, t.net) - kTol && hit(HitKind::Track, i, w.trackFixed(i))) return true;
    }
    w.viasIn(box, found);
    for (size_t i : found) {
        const Via& v = w.via(i);
        if (!v.spans(layer) || inNets(nets, v.net)) continue;
        const double d = pointSegmentDistance(v.position, a, b) - hw - v.diameter / 2;
        if (d < clearanceTo(B, nets, v.net) - kTol && hit(HitKind::Via, i, w.viaFixed(i))) return true;
    }
    if (s.segmentEdgeDistance(a, b) - hw < s.edgeClearance - kTol && hit(HitKind::Edge, 0, true)) return true;
    for (size_t k = 0; k < s.holes.size(); ++k)
        if (pointSegmentDistance(s.holes[k].position, a, b) - hw < s.holes[k].keepout / 2 - kTol &&
            hit(HitKind::Hole, k, true))
            return true;
    if (layer >= 0 && layer < static_cast<int>(B.planeNet.size())) {
        const int pn = B.planeNet[static_cast<size_t>(layer)];
        if (pn >= 0 && !inNets(nets, pn) && hit(HitKind::Plane, static_cast<size_t>(layer), true)) return true;
    }
    for (size_t k = 0; k < B.meshes.size(); ++k) {
        const auto& m = B.meshes[k];
        if ((layer == m.layerA || layer == m.layerB) && !inNets(nets, m.netA) && !inNets(nets, m.netB) &&
            segmentRectDistance(a, b, m.region) - hw <= 0 && hit(HitKind::Mesh, k, true))
            return true;
    }
    return any;
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
        const double d = pointSegmentDistance(v.position, t.a, t.b) - r - t.width / 2;
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
        w.tracksIn(box, found);
        for (size_t i : found) {
            const Track& t = w.track(i);
            if (t.net == net && t.layer == layer &&
                segmentSegmentDistance(a, b, t.a, t.b) <= std::min(hw, t.width / 2))
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
    w.tracksIn(Rect::centered(p, 2 * kJoin, 2 * kJoin), found);
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
            for (;;) {
                std::vector<Hit> hits = hitsOf(p);
                if (hits.empty()) break;
                auto fixedHit = std::find_if(hits.begin(), hits.end(), [](const Hit& h) { return h.fixed; });
                if (fixedHit != hits.end()) {
                    why = "Blocked by " + describeHit(w_, *fixedHit);
                    return false;
                }
                if (++ops_ > opt_.shoveLimit) {
                    why = "Too much copper to shove here";
                    return false;
                }
                const Hit h = hits.front();
                const bool ok = h.kind == HitKind::Track ? shoveTrack(h.index, p, why) : shoveVia(h.index, p, why);
                if (!ok) return false;
                if (!alive(p)) break;
            }
        }
        for (const Item& it : touched_)
            if (alive(it) && !hitsOf(it).empty()) {
                why = "Shoved copper would break clearance";
                return false;
            }
        return true;
    }

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
            segmentHits(w_, {t.net}, t.layer, t.a, t.b, t.width / 2, &hits);
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
            w_.tracksIn(segmentBox(a, b, 0.01), found);
            for (size_t i : found) {
                const Track& o = w_.track(i);
                if (o.net == proto.net && o.layer == proto.layer && o.width >= proto.width - 1e-9 &&
                    pointSegmentDistance(a, o.a, o.b) <= 1e-7 && pointSegmentDistance(b, o.a, o.b) <= 1e-7)
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
                if (h.kind != HitKind::Pad && h.kind != HitKind::Track && h.kind != HitKind::Via) {
                    why = "A track of " + B_.netName(L.net) + " cannot be shoved past " + describeHit(w_, h);
                    restore();
                    return false;
                }
                const Shape s = shapeOfHit(h);
                const bool barrier = h.kind == HitKind::Pad && B_.barrierComps.count(B_.pads[h.index].componentId) > 0;
                hull = makeOctagon(s.points(), s.radius + B_.clearance(L.net, netOfHit(h), barrier) + hw + kMargin);
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
                if (pointSegmentDistance(v.position, t.a, t.b) <= r) {
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
    auto markRect = [&](const Rect& r, const std::function<bool(Vec2)>& bad) {
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
        markRect(segmentBox(t.a, t.b, need), [&](Vec2 c) { return pointSegmentDistance(c, t.a, t.b) < need; });
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
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            if (blocked[idx(i, j)]) continue;
            const Vec2 c = centre(i, j);
            if (s.edgeDistance(c) < s.edgeClearance + hw + slack || s.holeDistance(c) < hw + slack) blocked[idx(i, j)] = 1;
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
    using QItem = std::tuple<double, double, size_t>;
    std::priority_queue<QItem, std::vector<QItem>, std::greater<QItem>> open;
    const size_t s0 = idx(si, sj);
    g[s0] = 0;
    open.push({h(si, sj), h(si, sj), s0});
    size_t best = s0;
    double bestH = h(si, sj);
    size_t expanded = 0;
    while (!open.empty() && expanded < 600000) {
        const auto [f, hh, cur] = open.top();
        open.pop();
        (void)f;
        if (closed[cur]) continue;
        closed[cur] = 1;
        ++expanded;
        if (hh < bestH) {
            bestH = hh;
            best = cur;
        }
        if (cur == goal) break;
        const int ci = static_cast<int>(cur % static_cast<size_t>(cols)), cj = static_cast<int>(cur / static_cast<size_t>(cols));
        for (int d = 0; d < nd; ++d) {
            const int ni = ci + di[d], nj = cj + dj[d];
            if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
            const size_t nx = idx(ni, nj);
            if (blocked[nx] || closed[nx]) continue;
            if (d >= 4 && (blocked[idx(ci + di[d], cj)] || blocked[idx(ci, cj + dj[d])])) continue;  // no corner cutting
            const double ng = g[cur] + (d >= 4 ? std::sqrt(2.0) : 1.0);
            if (ng < g[nx] - 1e-12) {
                g[nx] = ng;
                parent[nx] = static_cast<long>(cur);
                open.push({ng + h(ni, nj), h(ni, nj), nx});
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
        if ((t.b - t.a).length() < 1e-9) continue;
        if (!out.empty()) {
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
           a.locked == b.locked;
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

}  // namespace

// ================================================================================================ the router

struct InteractiveRouter::Impl {
    PcbLayout& pcb;
    const Schematic& sch;
    RouterOptions opt;
    std::unique_ptr<Base> base;
    World committed, current;
    enum class Kind { None, Route, Pair, Drag, DragVia } kind = Kind::None;

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
        groupTarget = 0;
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
            if (t.net < 0 || t.layer != l || pointSegmentDistance(at, t.a, t.b) > t.width / 2) continue;
            Vec2 p = closestOnSegment(at, t.a, t.b);
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
            if (pointSegmentDistance(cursor, t.a, t.b) <= t.width / 2) {
                hit = true;
                Vec2 p = closestOnSegment(cursor, t.a, t.b);
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
    };

    /// Lays the head lines into `w` as fixed copper and makes room for them (shove) or checks them (walkaround).
    bool placeHead(World& w, const std::vector<HeadLine>& lines, bool shove, std::string& why) const {
        std::vector<size_t> added;
        for (const auto& hl : lines)
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
        for (const auto& c : candidates) {
            World w = committed;
            if (placeHead(w, build(c, true), shove, why)) {
                current = w;
                chosen = c;
                return;
            }
            if (firstWhy.empty()) firstWhy = why;
        }
        // Walk around what cannot be shoved.
        const GridPath walk = walkSearch();
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
            const auto& p = candidates.front();
            double lo = 0, hi = pathLength(p);
            for (int it = 0; it < 12 && hi - lo > 1e-3; ++it) {
                const double mid = (lo + hi) / 2;
                const auto q = pathPrefix(p, mid);
                World w = committed;
                if (q.size() >= 2 && placeHead(w, build(q, false), shove, why)) {
                    lo = mid;
                    bestPath = q;
                    bestWorld = w;
                } else {
                    hi = mid;
                }
            }
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
            auto lead = postureLinks(members[k].end, off.front(), RoutePosture::Diagonal45, false);
            pts.insert(pts.end(), lead.front().begin() + 1, lead.front().end());
            pts.insert(pts.end(), off.begin() + 1, off.end());
            if (targets) {
                auto tail = postureLinks(off.back(), targets[k], RoutePosture::Diagonal45, false);
                pts.insert(pts.end(), tail.front().begin() + 1, tail.front().end());
            }
            out.push_back({members[k].net, simplifyPath(pts)});
        }
        return out;
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
                if (l.size() < 2 || !acuteJoin(l.back() - l[l.size() - 2], drag.dir)) {
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
                if (l.size() < 2 || !acuteJoin(drag.dir, l[1] - l[0])) {
                    pick = l;
                    break;
                }
            pts.insert(pts.end(), pick.begin() + 1, pick.end());
        }
        return simplifyPath(pts);
    }

    void dragHead(Vec2 cursor) {
        Member& m = members[0];
        const double delta = (cursor - drag.grab).dot(drag.normal);
        const bool shove = opt.mode == RouterMode::Shove;
        std::string why;
        blocked = false;
        status = "Dragging a track of " + base->netName(m.net);
        auto tryDelta = [&](double d, World& w) { return placeHead(w, {{m.net, dragPath(d)}}, shove, why); };
        World w = committed;
        if (tryDelta(delta, w)) {
            current = w;
            m.head = dragPath(delta);
            return;
        }
        blocked = true;
        status = why;
        double lo = 0, hi = delta;
        World best = committed;
        bool any = false;
        {
            World w0 = committed;
            if (tryDelta(0, w0)) {
                best = w0;
                any = true;
            }
        }
        for (int it = 0; it < 14 && std::fabs(hi - lo) > 1e-3; ++it) {
            const double mid = (lo + hi) / 2;
            World wm = committed;
            if (tryDelta(mid, wm)) {
                lo = mid;
                best = wm;
                any = true;
            } else {
                hi = mid;
            }
        }
        current = any ? best : committed;
        m.head = any ? dragPath(lo) : std::vector<Vec2>{};
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
                if (pointSegmentDistance(v.position, t.a, t.b) <= r) return bail("A track runs through the via");
                continue;
            }
            if (aIn && bIn) return bail("A short track inside the via's land holds it");
            if (base->trackFixed[i]) return bail("A locked track ends on the via");
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
        for (int it = 0; it < 14 && (hi - lo) * span > 1e-3; ++it) {
            const double mid = (lo + hi) / 2;
            World wm = committed;
            if (placeVia(wm, from + (to - from) * mid, why)) {
                lo = mid;
                best = wm;
                any = true;
            } else {
                hi = mid;
            }
        }
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

    void computeHead(Vec2 cursor) {
        lastCursor = cursor;
        hasCursor = true;
        if (kind == Kind::Route) routeHead(cursor);
        if (kind == Kind::Pair) pairHead(cursor);
        if (kind == Kind::Drag) dragHead(cursor);
        if (kind == Kind::DragVia) viaDragHead(cursor);
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

    bool fixHead() {
        if (kind != Kind::Route && kind != Kind::Pair) return fail("Nothing to place");
        bool any = false;
        for (const auto& m : members) any = any || m.head.size() >= 2;
        if (!any) return fail("The head is empty");
        committed = current;
        for (auto& m : members) {
            if (m.head.size() < 2) continue;
            for (const Track& t : toTracks(m.head, m.net, layer, width)) m.placed.push_back(t);
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
        } else if (kind == Kind::Route && !members[0].placed.empty()) {
            lastDir = members[0].placed.back().b - members[0].placed.back().a;
        }
        buildPreview();
        return true;
    }

    bool addVia(int toLayer) {
        if (kind != Kind::Route && kind != Kind::Pair) return fail("Start a route first");
        const BoardSettings& s = base->s;
        if (s.layerCount < 2) return fail("A single-sided board has no vias");
        const int to = toLayer >= 0 ? toLayer : (layer == 0 ? s.bottomLayer() : 0);
        if (to == layer) return fail("The route is already on that layer");
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
            v.net = m.net;
            v.position = m.end;
            v.drill = s.viaDrill;
            v.diameter = s.viaDiameter;
            vias.push_back(v);
        } else {
            Vec2 dir = lastDir;
            if (dir.length() <= 0) {
                const Vec2 across = members[0].end - members[1].end;
                dir = across.length() > 0 ? Vec2{across.y, -across.x} : Vec2{1, 0};
            }
            dir = unit(dir);
            const Vec2 n = leftNormal(dir);
            const double off = std::max(spacing / 2, (s.viaDiameter + base->clearance(members[0].net, members[1].net)) / 2 + kMargin);
            int sd = side != 0 ? side : (cross(dir, members[0].end - centre) >= 0 ? 1 : -1);
            for (size_t k = 0; k < 2; ++k) {
                Via v;
                v.net = members[k].net;
                v.position = centre + n * ((k == 0 ? sd : -sd) * off);
                v.drill = s.viaDrill;
                v.diameter = s.viaDiameter;
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
        if (kind == Kind::Route || kind == Kind::Pair) {
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
        } else if (kind == Kind::Drag) {
            committed = current;
            for (const Track& t : mergeCollinear(toTracks(members[0].head, members[0].net, layer, width)))
                routeTracks.push_back(t);
        } else {
            for (const auto& m : members)
                for (const Track& t : mergeCollinear(m.placed)) routeTracks.push_back(t);
        }
        // Route pieces that run exactly over copper the net already has add nothing.
        std::vector<Track> kept;
        for (const Track& t : routeTracks) {
            bool covered = false;
            std::vector<size_t> found;
            committed.tracksIn(segmentBox(t.a, t.b, 0.01), found);
            for (size_t i : found) {
                const Track& o = committed.track(i);
                if (i >= committed.baseT() && committed.fixAddT[i - committed.baseT()]) continue;  // the route itself
                if (o.net == t.net && o.layer == t.layer && o.width >= t.width - 1e-9 &&
                    pointSegmentDistance(t.a, o.a, o.b) <= 1e-7 && pointSegmentDistance(t.b, o.a, o.b) <= 1e-7)
                    covered = true;
            }
            if (!covered) kept.push_back(t);
        }
        ch = applyWorld(pcb, committed, kept, routeVias);
        reset();
        return ch;
    }

    void buildPreview() {
        RoutePreview p;
        p.active = kind != Kind::None;
        if (!p.active) {
            prev = p;
            return;
        }
        p.kind = kind == Kind::Route ? "route" : kind == Kind::Pair ? "pair" : kind == Kind::Drag ? "drag" : "via";
        p.status = status;
        p.blocked = blocked;
        p.reachedTarget = reached;
        for (const auto& m : members) p.nets.push_back(m.net);
        p.layer = layer;
        p.layerCount = std::max(1, base->s.layerCount);
        p.width = width;
        p.gap = kind == Kind::Pair ? gap : 0;
        for (const auto& m : members) {
            p.placed.insert(p.placed.end(), m.placed.begin(), m.placed.end());
            for (const Track& t : toTracks(m.head, m.net, layer, width)) p.head.push_back(t);
        }
        if (kind == Kind::Pair)
            p.end = centreHead.size() >= 2 ? centreHead.back() : centre;
        else if (kind == Kind::DragVia)
            p.end = vdrag.at;
        else
            p.end = !members.empty() ? (members[0].head.size() >= 2 ? members[0].head.back() : members[0].end) : Vec2{};
        p.vias = placedVias;
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
            for (const Track& t : vdrag.tracks) p.length += (t.b - t.a).length();
        } else if (!members.empty()) {
            for (const Track& t : members[0].placed) p.length += (t.b - t.a).length();
            p.length += pathLength(members[0].head);
        }
        // The net's whole length: its copper in the overlay (the session's own pieces aside) plus the route.
        if (!members.empty()) {
            const int net = members[0].net;
            double other = 0;
            for (size_t i = 0; i < current.nT(); ++i) {
                if (!current.aliveT(i) || (i >= current.baseT() && current.fixAddT[i - current.baseT()])) continue;
                const Track& t = current.track(i);
                if (t.net == net) other += (t.b - t.a).length();
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
bool InteractiveRouter::beginPair(Vec2 at, int layer) {
    impl_->err.clear();
    return impl_->beginPair(at, layer);
}
bool InteractiveRouter::beginDrag(int trackId, Vec2 grab) {
    impl_->err.clear();
    return impl_->beginDrag(trackId, grab);
}
bool InteractiveRouter::beginViaDrag(int viaId, Vec2 grab) {
    impl_->err.clear();
    return impl_->beginViaDrag(viaId, grab);
}
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

LengthTuneResult tuneTrackLength(PcbLayout& pcb, const Schematic& sch, int trackId, const LengthTuneOptions& opt) {
    LengthTuneResult r;
    long ti = -1;
    for (size_t i = 0; i < pcb.tracks.size(); ++i)
        if (pcb.tracks[i].id == trackId) ti = static_cast<long>(i);
    if (ti < 0) {
        r.message = "No track with that id";
        return r;
    }
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
    return o;
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
    return j;
}

// ================================================================================================= JSON

RouterOptions routerOptionsFromJson(const Json& j, RouterOptions o) {
    if (!j.isObject()) return o;
    const std::string mode = j.get("mode").asString(std::string());
    if (mode == "shove") o.mode = RouterMode::Shove;
    if (mode == "walkaround") o.mode = RouterMode::Walkaround;
    const Json& posture = j.get("posture");
    const std::string ps = posture.isNumber() ? std::to_string(posture.asInt()) : posture.asString(std::string());
    if (ps == "45") o.posture = RoutePosture::Diagonal45;
    if (ps == "90") o.posture = RoutePosture::Orthogonal90;
    if (ps == "free") o.posture = RoutePosture::Free;
    if (j.has("swapPosture")) o.swapPosture = j.get("swapPosture").asBool(o.swapPosture);
    if (j.has("width")) o.width = std::max(0.0, j.get("width").asNumber(o.width));
    if (j.has("pairGap")) o.pairGap = std::max(0.0, j.get("pairGap").asNumber(o.pairGap));
    if (j.has("snap")) o.snapToPads = j.get("snap").asBool(o.snapToPads);
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
