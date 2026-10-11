// SiEDA Core — the autorouter's passes after routing (RouteQuality.hpp).
#include "RouteQuality.hpp"

#include <algorithm>
#include <cmath>
#include <tuple>

#include "sieda/InteractiveRouter.hpp"
#include "sieda/LengthRules.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda::routequality {

namespace {
int netIndex(const Schematic& sch, const std::string& name) {
    for (const auto& n : sch.nets())
        if (n.name == name) return n.index;
    return -1;
}
}  // namespace

std::vector<LengthRouteReport> tuneLengthTargets(PcbLayout& pcb, const Schematic& sch, const std::map<int, int>& coupledPartner) {
    std::vector<LengthRouteReport> out;
    const BoardSettings& s = pcb.settings;
    if (s.lengthRules.empty() && s.matchGroups.empty()) return out;
    const auto pads = pcb.pads(sch);
    // Nets with a target, in Board Setup order: rules first, then each match group's members.
    std::vector<int> nets;
    auto add = [&](const std::string& name) {
        const int n = netIndex(sch, name);
        if (n >= 0 && std::find(nets.begin(), nets.end(), n) == nets.end()) nets.push_back(n);
    };
    for (const auto& r : s.lengthRules) add(r.net);
    for (const auto& g : s.matchGroups)
        for (const auto& n : g.nets) add(n);
    std::set<int> negatives;  // N members of coupled pairs: tuned together with their P member
    for (const auto& [p, n] : coupledPartner)
        if (std::find(nets.begin(), nets.end(), p) != nets.end()) negatives.insert(n);
    std::map<int, LengthRouteReport> reports;
    for (int net : nets) {
        LengthTarget lt;
        if (!lengthTargetFor(pcb, sch, pads, net, lt)) continue;
        LengthRouteReport rep;
        rep.net = sch.nets()[static_cast<size_t>(net)].name;
        rep.routed = xSignalLength(pcb, pads, lt.xsignal);
        if (!negatives.count(net) && rep.routed >= 0 && rep.routed < lt.target - lt.tolerance) {
            const auto partner = coupledPartner.find(net);
            for (int attempt = 0; attempt < 6; ++attempt) {
                LengthTarget now;
                if (!lengthTargetFor(pcb, sch, pads, net, now)) break;
                const double cur = xSignalLength(pcb, pads, now.xsignal);
                if (cur < 0 || cur >= now.target - now.tolerance) break;
                // The xSignal's longest straight tracks first (their ids change as meanders replace them).
                // A coupled pair is tuned from its P member's tracks (both members get the meander).
                const bool coupled = partner != coupledPartner.end();
                std::vector<const Track*> cands;
                for (const Track& t : pcb.tracks)
                    if (!t.arc && !t.teardrop && !t.locked && trackLength(t) >= 1.0 && (!coupled || t.net == net) &&
                        std::find(now.xsignal.nets.begin(), now.xsignal.nets.end(), t.net) != now.xsignal.nets.end())
                        cands.push_back(&t);
                if (cands.empty()) break;
                std::stable_sort(cands.begin(), cands.end(), [](const Track* a, const Track* b) {
                    const double la = trackLength(*a), lb = trackLength(*b);
                    return la > lb + 1e-9 || (std::fabs(la - lb) <= 1e-9 && a->id < b->id);
                });
                LengthTuneOptions o;
                o.target = 0;  // the rule's or the group's target
                o.style = MeanderStyle::Accordion;
                o.corner = MeanderCorner::Mitered;
                o.maxAmplitude = attempt < 2 ? 0.0 : 3.0;
                o.coupled = coupled;
                const Track* pick = cands[static_cast<size_t>(attempt) % cands.size()];
                const LengthTuneResult r = tuneTrackLength(pcb, sch, pick->id, o);
                if (r.applied) rep.tuned = true;
            }
        }
        reports[net] = rep;
    }
    for (int net : nets) {
        auto it = reports.find(net);
        if (it == reports.end()) continue;
        LengthRouteReport rep = it->second;
        LengthTarget lt;
        if (lengthTargetFor(pcb, sch, pads, net, lt)) {
            rep.source = lt.source;
            rep.target = lt.target;
            rep.tolerance = lt.tolerance;
            rep.achieved = xSignalLength(pcb, pads, lt.xsignal);
            rep.ok = rep.achieved >= 0 && std::fabs(rep.achieved - lt.target) <= lt.tolerance + 1e-6;
        }
        if (negatives.count(net))
            for (const auto& [p, n] : coupledPartner)
                if (n == net && reports.count(p) && reports[p].tuned) rep.tuned = true;
        out.push_back(rep);
    }
    return out;
}

namespace {
/// Runs a board edit and undoes it when it leaves more connections unrouted than before (a safety net: the edits keep
/// every rule, but a line pulled tight could still let go of a join it did not know about).
template <typename Edit>
int keepingConnections(PcbLayout& pcb, const Schematic& sch, Edit edit) {
    const std::vector<Track> tracks = pcb.tracks;
    const std::vector<Via> vias = pcb.vias;
    const size_t before = pcb.ratsnest(sch).size();
    const int done = edit();
    if (done > 0 && pcb.ratsnest(sch).size() > before) {
        pcb.tracks = tracks;
        pcb.vias = vias;
        return 0;
    }
    return done;
}
}  // namespace

int shapeComplete(PcbLayout& pcb, const Schematic& sch) {
    std::vector<std::pair<Vec2, Vec2>> lines = pcb.ratsnest(sch);
    std::stable_sort(lines.begin(), lines.end(), [](const auto& x, const auto& y) {
        return (x.second - x.first).length() < (y.second - y.first).length();
    });
    const int layers = std::max(1, pcb.settings.layerCount);
    int done = 0;
    for (const auto& line : lines) {
        // One attempt: from `from` on layer l, straight to `to` or through a via at `site` onto layer l2 (-1: none).
        auto attempt = [&](Vec2 from, Vec2 to, int l, RouterMode mode, const Vec2* site, int l2) {
            return keepingConnections(pcb, sch, [&] {
                InteractiveRouter r(pcb, sch);
                RouterOptions o;
                o.mode = mode;
                r.setOptions(o);
                if (!r.beginRoute(from, l)) return 0;
                if (site) {
                    const RoutePreview& leg = r.moveTo(*site);
                    if (leg.blocked || (leg.end - *site).length() > 1e-6 || !r.addVia(l2)) {
                        r.cancel();
                        return 0;
                    }
                }
                if (!r.moveTo(to).reachedTarget) {
                    r.cancel();
                    return 0;
                }
                return r.commit().empty() ? 0 : 1;
            });
        };
        const size_t before = pcb.ratsnest(sch).size();
        auto routed = [&](int n) { return n > 0 && pcb.ratsnest(sch).size() < before; };
        bool ok = false;
        for (int dir = 0; dir < 2 && !ok; ++dir) {
            const Vec2 from = dir ? line.second : line.first, to = dir ? line.first : line.second;
            for (RouterMode mode : {RouterMode::Walkaround, RouterMode::Shove})
                for (int l = 0; l < layers && !ok; ++l) ok = routed(attempt(from, to, l, mode, nullptr, -1));
        }
        // Via sites along the line and beside it, nearest the middle first.
        const Vec2 d = line.second - line.first;
        const Vec2 side = d.length() > 0 ? Vec2{-d.y, d.x} * (2.0 / d.length()) : Vec2{0, 2};
        for (double f : {0.5, 0.3, 0.7, 0.15, 0.85})
            for (double s : {0.0, 1.0, -1.0})
                for (int l = 0; l < layers && !ok && layers > 1; ++l)
                    for (int l2 = 0; l2 < layers && !ok; ++l2) {
                        if (l2 == l) continue;
                        const Vec2 site = line.first + d * f + side * s;
                        ok = routed(attempt(line.first, line.second, l, RouterMode::Walkaround, &site, l2));
                    }
        if (ok) ++done;
    }
    return done;
}

int glossRouted(PcbLayout& pcb, const Schematic& sch, const std::set<int>& skipNets) {
    std::vector<int> ids;
    for (const Track& t : pcb.tracks)
        if (t.net >= 0 && !t.locked && !t.arc && !t.teardrop && !skipNets.count(t.net)) ids.push_back(t.id);
    if (ids.empty()) return 0;
    return keepingConnections(pcb, sch, [&] {
        GlossOptions o;
        o.retrace = true;
        const BoardEditResult r = glossTracks(pcb, sch, ids, o);
        return r.applied ? r.added : 0;
    });
}

int arcRouted(PcbLayout& pcb, const Schematic& sch, double radius) {
    std::vector<int> ids;
    for (const Track& t : pcb.tracks)
        if (t.net >= 0 && !t.locked && !t.arc && !t.teardrop) ids.push_back(t.id);
    if (ids.empty()) return 0;
    return keepingConnections(pcb, sch, [&] {
        ArcCornersOptions o;
        o.radius = radius;
        const ArcCornersResult r = convertCornersToArcs(pcb, sch, ids, o);
        return r.applied ? r.converted : 0;
    });
}

int teardropsRouted(PcbLayout& pcb, const Schematic& sch, TeardropStyle style) {
    return keepingConnections(pcb, sch, [&] {
        TeardropOptions to;
        to.style = style;
        const BoardEditResult r = addTeardrops(pcb, sch, to);
        return r.applied ? r.added : 0;
    });
}

int fixAcuteJoins(PcbLayout& pcb, const Schematic& sch) {
    return keepingConnections(pcb, sch, [&] {
        const auto ps = pcb.pads(sch);
        const BoardSettings& s = pcb.settings;
        auto clearanceOf = [&](int net) {
            return net >= 0 && static_cast<size_t>(net) < sch.nets().size() ? s.clearanceFor(sch.nets()[static_cast<size_t>(net)].name)
                                                                            : s.clearance;
        };
        // A joint inside a pad or via of its net is covered by copper (as the DRC sees it).
        auto covered = [&](int net, int layer, Vec2 p) {
            for (const Pad& pd : ps)
                if (pd.net == net && pd.onLayer(layer) &&
                    (pd.round ? (p - pd.position).length() - std::min(pd.size.x, pd.size.y) / 2 : pointRectDistance(p, pd.bounds())) <= 0)
                    return true;
            for (const Via& v : pcb.vias)
                if (v.net == net && (v.position - p).length() <= v.diameter / 2) return true;
            return false;
        };
        // A new link a–b (width w) keeps every other net's clearance, the keep-outs and the board edge.
        auto clear = [&](int net, int layer, Vec2 a, Vec2 b, double w) {
            const double own = clearanceOf(net);
            for (const Track& t : pcb.tracks)
                if (t.layer == layer && t.net != net &&
                    trackSegmentDistance(t, a, b) - (w + t.width) / 2 < std::max(own, clearanceOf(t.net)) + 1e-3) return false;
            for (const Pad& pd : ps) {
                if (!pd.onLayer(layer) || (pd.net == net && net >= 0)) continue;
                const double d = pd.round ? pointSegmentDistance(pd.position, a, b) - std::min(pd.size.x, pd.size.y) / 2
                                          : segmentRectDistance(a, b, pd.bounds());
                if (d - w / 2 < std::max(own, clearanceOf(pd.net)) + 1e-3) return false;
            }
            for (const Via& v : pcb.vias)
                if (v.net != net && v.spans(layer) &&
                    pointSegmentDistance(v.position, a, b) - (v.diameter + w) / 2 < std::max(own, clearanceOf(v.net)) + 1e-3) return false;
            for (const RouteKeepout& k : s.keepouts)
                if (k.tracks && (k.layer < 0 || k.layer == layer) && segmentRectDistance(a, b, k.area) < w / 2) return false;
            return s.segmentEdgeDistance(a, b) >= s.edgeClearance + w / 2 - 1e-6;
        };
        const double minCos = std::cos(89.0 * kPi / 180);  // the DRC's limit
        int fixed = 0;
        for (int pass = 0; pass < 4; ++pass) {
            // Straight tracks by the 1 mm cells their boxes cover, so a joint finds the tracks through it.
            std::map<std::tuple<int, long long, long long>, std::vector<size_t>> cells;
            auto cell = [](double v) { return static_cast<long long>(std::floor(v)); };
            for (size_t i = 0; i < pcb.tracks.size(); ++i) {
                const Track& t = pcb.tracks[i];
                if (t.arc || t.teardrop || t.net < 0) continue;
                for (long long x = cell(std::min(t.a.x, t.b.x) - 1e-6); x <= cell(std::max(t.a.x, t.b.x) + 1e-6); ++x)
                    for (long long y = cell(std::min(t.a.y, t.b.y) - 1e-6); y <= cell(std::max(t.a.y, t.b.y) + 1e-6); ++y)
                        cells[{t.layer, x, y}].push_back(i);
            }
            std::vector<char> touched(pcb.tracks.size(), 0);
            std::vector<Track> added;
            const int before = fixed;
            for (size_t j = 0; j < pcb.tracks.size(); ++j)
                for (int end = 0; end < 2 && !touched[j]; ++end) {
                    const Track& b = pcb.tracks[j];
                    if (b.arc || b.teardrop || b.locked || b.net < 0) break;
                    const Vec2 p = end == 0 ? b.a : b.b, q = end == 0 ? b.b : b.a;
                    const double lb = (q - p).length();
                    const auto it = cells.find({b.layer, cell(p.x), cell(p.y)});
                    if (lb < 1e-6 || it == cells.end()) continue;
                    int isCovered = -1;  // tested only at an acute angle
                    const Vec2 ub = (q - p) * (1.0 / lb);
                    // The net's other tracks through the joint: when only one stays there and ends there, it is
                    // trimmed back to the new link (it would dangle otherwise).
                    int others = 0;
                    for (size_t i : it->second)
                        others += i != j && pcb.tracks[i].net == b.net && trackPointDistance(pcb.tracks[i], p) <= 1e-6;
                    for (size_t i : it->second) {
                        const Track& a = pcb.tracks[i];
                        // Joints of two track ends, as the DRC checks them (a track's middle is left alone: tuned
                        // meanders put their short pieces against each other there).
                        const bool atA = (a.a - p).length() <= 1e-6;
                        if (i == j || touched[i] || touched[j] || a.net != b.net || a.locked || (!atA && (a.b - p).length() > 1e-6))
                            continue;
                        const Vec2 far = atA ? a.b : a.a;
                        const double la = (far - p).length();
                        if (la < 1e-6) continue;
                        const Vec2 ua = (far - p) * (1.0 / la);
                        const double cosine = ua.dot(ub), sine = std::sqrt(std::max(0.0, 1 - cosine * cosine));
                        if (cosine <= minCos || sine < 0.1) continue;
                        if (isCovered < 0) isCovered = covered(b.net, b.layer, p) ? 1 : 0;
                        if (isCovered) break;
                        // At an acute angle, b's end moves back off the joint to R on b, joined to `a` at F by a link
                        // square to `a` (90° at F, 90° + the angle at R).
                        for (double k : {1.0, 1.5, 0.75}) {
                            const double d = k * std::max(a.width, b.width) / sine, along = d * cosine;
                            if (d > lb - 1e-3 || along > la - 1e-3) continue;
                            const Vec2 r = p + ub * d, f = p + ua * along;
                            if (!clear(b.net, b.layer, f, r, b.width)) continue;
                            Track link = b;
                            link.a = f;
                            link.b = r;
                            added.push_back(link);
                            (end == 0 ? pcb.tracks[j].a : pcb.tracks[j].b) = r;
                            if (others == 1) (atA ? pcb.tracks[i].a : pcb.tracks[i].b) = f;
                            touched[i] = touched[j] = 1;
                            ++fixed;
                            break;
                        }
                    }
                }
            for (Track& t : added) pcb.addTrack(t);
            if (fixed == before) break;
        }
        return fixed;
    });
}

void measure(const PcbLayout& pcb, const Schematic& sch, RouteMetrics& m) {
    const int layers = std::max(1, pcb.settings.layerCount);
    m.layerLength.assign(static_cast<size_t>(layers), 0.0);
    m.vias = static_cast<int>(pcb.vias.size());
    m.microvias = m.blindVias = 0;
    for (const Via& v : pcb.vias) {
        const std::string kind = viaKind(v, layers);
        if (kind == "microvia") ++m.microvias;
        else if (kind != "through") ++m.blindVias;
    }
    m.trackLength = 0;
    m.segments = static_cast<int>(pcb.tracks.size());
    m.arcs = m.teardrops = 0;
    for (const Track& t : pcb.tracks) {
        const double len = t.teardrop ? 0.0 : trackLength(t);  // teardrops are joints, not routed length
        m.trackLength += len;
        if (t.layer >= 0 && t.layer < layers) m.layerLength[static_cast<size_t>(t.layer)] += len;
        m.arcs += t.arc ? 1 : 0;
        m.teardrops += t.teardrop ? 1 : 0;
    }
    m.unrouted = static_cast<int>(pcb.ratsnest(sch).size());
}

}  // namespace sieda::routequality
