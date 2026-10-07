// SiEDA Core — pin and gate swapping from the board (Altium's PCB pin / gate swap): the swaps a placed multi-unit
// part allows (pins of one swap group, interchangeable gates of the same part), how much ratsnest each saves, and
// carrying a chosen swap out in the schematic (back-annotation), so board and schematic stay one design. The
// autorouter's automatic pin / gate swap (AutorouteOptions::pinSwap) makes these same swaps before it routes.
#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <memory>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/Project.hpp"

namespace sieda {

namespace {
struct PadInfo {
    Vec2 position;
    int net = -1;
};
using PadKey = std::pair<int, int>;  // (package component, part pin index)

/// Shortest distance from `p` to a pad of `net`, leaving out the pads in `skip` (0 when there is none).
double nearestOnNet(const std::map<PadKey, PadInfo>& pads, Vec2 p, int net, const std::set<PadKey>& skip) {
    if (net < 0) return 0;
    double best = -1;
    for (const auto& [key, pad] : pads) {
        if (pad.net != net || skip.count(key)) continue;
        const double d = std::hypot(pad.position.x - p.x, pad.position.y - p.y);
        if (best < 0 || d < best) best = d;
    }
    return best < 0 ? 0 : best;
}

/// Ratsnest saved when the nets of the pad pairs (a_k, b_k) change places (positive = shorter).
double swapGain(const std::map<PadKey, PadInfo>& pads, const std::vector<std::pair<PadKey, PadKey>>& pairs) {
    std::set<PadKey> skip;
    for (const auto& [a, b] : pairs) {
        skip.insert(a);
        skip.insert(b);
    }
    double gain = 0;
    for (const auto& [a, b] : pairs) {
        const auto ia = pads.find(a), ib = pads.find(b);
        if (ia == pads.end() || ib == pads.end()) continue;
        const PadInfo& pa = ia->second;
        const PadInfo& pb = ib->second;
        if (pa.net == pb.net) continue;
        gain += nearestOnNet(pads, pa.position, pa.net, skip) + nearestOnNet(pads, pb.position, pb.net, skip) -
                nearestOnNet(pads, pa.position, pb.net, skip) - nearestOnNet(pads, pb.position, pa.net, skip);
    }
    return gain;
}

/// The multi-unit part of package `pkg` (nullptr for anything else).
const CustomPart* multiUnitPart(const Schematic& s, int pkg) {
    const Component* package = s.find(pkg);
    if (!package) return nullptr;
    const CustomPart* part = CustomPartRegistry::instance().find(package->customPart);
    return part && !part->units.empty() ? part : nullptr;
}

std::map<PadKey, PadInfo> padMap(const Project& p) {
    std::map<PadKey, PadInfo> pads;
    for (const auto& pad : p.pcb.pads(p.schematic)) pads[{pad.componentId, pad.pinIndex}] = {pad.position, pad.net};
    return pads;
}

/// A swap pcbSwapOptions offers, with the pads whose nets change places.
struct SwapCandidate {
    PcbSwapOption option;
    std::vector<std::pair<PadKey, PadKey>> pairs;
};

/// Every pin / gate swap the package of `componentId` allows (unsorted); `gains`: estimate each one's gain.
std::vector<SwapCandidate> swapCandidates(const Schematic& s, int componentId, const std::map<PadKey, PadInfo>& pads, bool gains) {
    std::vector<SwapCandidate> out;
    const int pkg = s.unitPackage(componentId);
    const Component* package = s.find(pkg);
    const CustomPart* part = multiUnitPart(s, pkg);
    if (!package || !part) return out;
    const auto pinNumber = [&](int pin) {
        return pin >= 0 && pin < static_cast<int>(part->def.pins.size()) ? part->def.pins[static_cast<size_t>(pin)].number : std::string("?");
    };
    auto usable = [&](const Component* u) {
        // Units of a repeated sheet's channel copies follow their block: swaps are made on the block's own sheet.
        return u && u->kind == ComponentKind::PartUnit && u->instanceOf == 0 && u->unit >= 1 &&
               u->unit <= static_cast<int>(part->units.size());
    };
    for (int uid : s.placedUnits(pkg)) {
        const Component* u = s.find(uid);
        if (!usable(u)) continue;
        const PartUnitDef& def = part->units[static_cast<size_t>(u->unit - 1)];
        // Pin swaps inside the gate's swap groups.
        for (const auto& group : def.pinSwap)
            for (size_t i = 0; i < group.size(); ++i)
                for (size_t j = i + 1; j < group.size(); ++j) {
                    const int a = group[i], b = group[j];
                    if (a < 0 || b < 0 || a >= static_cast<int>(def.pins.size()) || b >= static_cast<int>(def.pins.size())) continue;
                    const int pa = def.pins[static_cast<size_t>(a)], pb = def.pins[static_cast<size_t>(b)];
                    SwapCandidate c;
                    PcbSwapOption& o = c.option;
                    o.kind = "pin";
                    o.component = uid;
                    o.pinA = a;
                    o.pinB = b;
                    o.label = s.displayRef(*u) + ": pins " + pinNumber(pa) + " \xE2\x86\x94 " + pinNumber(pb);
                    c.pairs = {{{pkg, pa}, {pkg, pb}}};
                    if (gains) o.gain = swapGain(pads, c.pairs);
                    out.push_back(std::move(c));
                }
        // Gate swaps with interchangeable gates of the same part (this package or another one of the same value).
        for (const auto& other : s.components()) {
            if (!usable(&other) || other.id == uid || other.customPart != u->customPart || other.sheet != u->sheet) continue;
            if (other.unitOf == pkg && other.id < uid) continue;  // each pair inside one package once
            const Component* otherPkg = s.find(other.unitOf);
            if (!otherPkg || otherPkg->value != package->value) continue;
            const PartUnitDef& od = part->units[static_cast<size_t>(other.unit - 1)];
            if (!unitsInterchangeable(def, od) || od.pins.size() != def.pins.size()) continue;
            SwapCandidate c;
            for (size_t k = 0; k < def.pins.size(); ++k) c.pairs.push_back({{pkg, def.pins[k]}, {other.unitOf, od.pins[k]}});
            PcbSwapOption& o = c.option;
            o.kind = "gate";
            o.component = uid;
            o.other = other.id;
            o.label = s.displayRef(*u) + " \xE2\x86\x94 " + s.displayRef(other);
            if (gains) o.gain = swapGain(pads, c.pairs);
            out.push_back(std::move(c));
        }
    }
    return out;
}

/// A swap and its reverse share one key.
std::string swapKey(const PcbSwapOption& o) {
    const int other = o.kind == "gate" ? o.other : o.component;
    return o.kind + ":" + std::to_string(std::min(o.component, other)) + ":" + std::to_string(std::max(o.component, other)) + ":" +
           std::to_string(std::min(o.pinA, o.pinB)) + ":" + std::to_string(std::max(o.pinA, o.pinB));
}

// ---- the automatic swap's ratsnest: per net, the minimum spanning tree of its pads (its airwires)
struct Airwire {
    Vec2 a, b;
};

/// Prim's minimum spanning tree over `pts` (in their order; ties go to the lower index): its length, and its edges.
double spanningTree(const std::vector<Vec2>& pts, std::vector<Airwire>* edges) {
    const size_t n = pts.size();
    if (n < 2) return 0;
    std::vector<double> dist(n, std::numeric_limits<double>::infinity());
    std::vector<size_t> from(n, 0);
    std::vector<char> in(n, 0);
    double total = 0;
    size_t cur = 0;
    in[0] = 1;
    for (size_t step = 1; step < n; ++step) {
        size_t next = n;
        for (size_t k = 0; k < n; ++k) {
            if (in[k]) continue;
            const double d = std::hypot(pts[k].x - pts[cur].x, pts[k].y - pts[cur].y);
            if (d < dist[k]) {
                dist[k] = d;
                from[k] = cur;
            }
            if (next == n || dist[k] < dist[next]) next = k;
        }
        in[next] = 1;
        total += dist[next];
        if (edges) edges->push_back({pts[from[next]], pts[next]});
        cur = next;
    }
    return total;
}

double orient(Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); }

/// The airwires cross at a point inside both (touching ends and collinear overlaps do not count).
bool airwiresCross(const Airwire& p, const Airwire& q) {
    if (std::max(p.a.x, p.b.x) < std::min(q.a.x, q.b.x) || std::max(q.a.x, q.b.x) < std::min(p.a.x, p.b.x) ||
        std::max(p.a.y, p.b.y) < std::min(q.a.y, q.b.y) || std::max(q.a.y, q.b.y) < std::min(p.a.y, p.b.y))
        return false;
    const double eps = 1e-9;
    const double d1 = orient(p.a, p.b, q.a), d2 = orient(p.a, p.b, q.b);
    const double d3 = orient(q.a, q.b, p.a), d4 = orient(q.a, q.b, p.b);
    return ((d1 > eps && d2 < -eps) || (d1 < -eps && d2 > eps)) && ((d3 > eps && d4 < -eps) || (d3 < -eps && d4 > eps));
}

using NetWires = std::map<int, std::vector<Airwire>>;

/// Crossings that involve the airwires in `mine` (by net): with the airwires in `all` of nets not in `mine`, and
/// between two nets of `mine`.
int crossingsOf(const NetWires& mine, const NetWires& all) {
    int n = 0;
    for (auto i = mine.begin(); i != mine.end(); ++i)
        for (const Airwire& w : i->second) {
            for (const auto& [net, wires] : all) {
                if (mine.count(net)) continue;
                for (const Airwire& v : wires) n += airwiresCross(w, v) ? 1 : 0;
            }
            for (auto j = std::next(i); j != mine.end(); ++j)
                for (const Airwire& v : j->second) n += airwiresCross(w, v) ? 1 : 0;
        }
    return n;
}

/// The board's airwires: per net (nets in `skip` — pours — left out), its pads in pad order and their spanning tree.
struct RatsnestModel {
    std::map<int, std::vector<PadKey>> netPads;
    NetWires wires;
    std::map<int, double> length;
    double total = 0;
};

RatsnestModel ratsnestModel(const std::map<PadKey, PadInfo>& pads, const std::set<int>& skip) {
    RatsnestModel m;
    for (const auto& [key, pad] : pads)
        if (pad.net >= 0 && !skip.count(pad.net)) m.netPads[pad.net].push_back(key);
    for (const auto& [net, keys] : m.netPads) {
        std::vector<Vec2> pts;
        for (const PadKey& k : keys) pts.push_back(pads.at(k).position);
        m.length[net] = spanningTree(pts, &m.wires[net]);
        m.total += m.length[net];
    }
    return m;
}

/// Every crossing of airwires of two different nets.
int totalCrossings(const RatsnestModel& m) {
    int twice = 0;  // each crossing is counted from both of its nets
    for (const auto& [net, wires] : m.wires) twice += crossingsOf(NetWires{{net, wires}}, m.wires);
    return twice / 2;
}

/// Nets that copper fills (pours): connected through the plane, they are no airwires.
std::set<int> pourNets(const Project& p) {
    std::set<int> out;
    for (const auto& n : p.schematic.nets())
        if (p.pcb.isZoneNet(p.schematic, n.index)) out.insert(n.index);
    return out;
}

/// Nets the automatic swap never moves: pours, nets with locked copper and tamper-mesh nets.
std::set<int> frozenNets(const Project& p, const std::set<int>& pours) {
    std::set<int> out = pours;
    for (const Track& t : p.pcb.tracks)
        if (t.locked && t.net >= 0) out.insert(t.net);
    for (const auto& n : p.schematic.nets())
        for (const TamperMesh& m : p.pcb.tamperMeshes)
            if (m.netA == n.name || m.netB == n.name) out.insert(n.index);
    return out;
}
}  // namespace

std::vector<PcbSwapOption> Project::pcbSwapOptions(int componentId) const {
    std::vector<PcbSwapOption> out;
    if (!multiUnitPart(schematic, schematic.unitPackage(componentId))) return out;
    for (auto& c : swapCandidates(schematic, componentId, padMap(*this), true)) out.push_back(std::move(c.option));
    std::stable_sort(out.begin(), out.end(), [](const PcbSwapOption& a, const PcbSwapOption& b) { return a.gain > b.gain; });
    return out;
}

bool Project::applyPcbSwap(const PcbSwapOption& o, std::vector<std::string>* report) {
    const bool inSync = pcbSync == currentSync();
    bool ok = false;
    if (o.kind == "pin") ok = schematic.swapPins(o.component, o.pinA, o.pinB);
    else if (o.kind == "gate") ok = schematic.swapUnits(o.component, o.other);
    if (!ok) return false;
    schematicChanged();
    pcb.pruneStaleRouting(schematic);
    if (inSync) pcbSync = currentSync();  // made on the board: nothing for Update PCB to bring over
    if (report) report->push_back((o.kind == "pin" ? "Swapped " : "Swapped gates ") + o.label + " (back-annotated to the schematic)");
    return true;
}

int Project::optimizePcbSwaps(int componentId, int maxSwaps, std::vector<std::string>* report) {
    std::vector<int> packages;
    if (componentId > 0) {
        const int pkg = schematic.unitPackage(componentId);
        if (pkg > 0) packages.push_back(pkg);
    } else {
        for (const auto& c : schematic.components())
            if (c.packageOnly && c.pcb.placed) packages.push_back(c.id);
    }
    int done = 0;
    maxSwaps = std::clamp(maxSwaps, 0, 1000);
    std::set<std::string> made;  // a swap and its reverse are made once (the gain estimate is local)
    while (done < maxSwaps) {
        PcbSwapOption best;
        best.gain = 0.01;  // mm: smaller savings are noise
        bool found = false;
        for (int pkg : packages)
            for (const auto& o : pcbSwapOptions(pkg))
                if (o.gain > best.gain && !made.count(swapKey(o))) {
                    best = o;
                    found = true;
                }
        if (!found) break;
        made.insert(swapKey(best));
        if (!applyPcbSwap(best, report)) break;
        ++done;
    }
    return done;
}

AutoSwapResult Project::autoSwapForRouting(int maxSwaps) {
    AutoSwapResult r;
    maxSwaps = std::clamp(maxSwaps, 0, 5000);
    std::set<int> pours = pourNets(*this);
    std::map<PadKey, PadInfo> pads = padMap(*this);
    RatsnestModel model = ratsnestModel(pads, pours);
    r.ratsnestBefore = r.ratsnestAfter = model.total;
    r.crossingsBefore = r.crossingsAfter = totalCrossings(model);
    const AutorouteOptions& strategy = pcb.settings.autorouter;
    if (strategy.scoped() || strategy.fanoutOnly) return r;  // nets outside the route keep their pins
    // Units of a repeated sheet's block are copied into its channels: a swap there would move the copies' pads too.
    std::set<int> copied;
    for (const auto& c : schematic.components())
        if (c.instanceOf > 0) copied.insert(c.instanceOf);
    auto movable = [&](int pkg) {
        const Component* c = schematic.find(pkg);
        return c && c->packageOnly && c->pcb.placed && !c->pcb.locked && c->instanceOf == 0 && !copied.count(c->id);
    };
    std::vector<int> packages;
    for (const auto& c : schematic.components())
        if (movable(c.id) && multiUnitPart(schematic, c.id)) packages.push_back(c.id);
    std::sort(packages.begin(), packages.end());
    std::set<std::string> refused;
    constexpr double kMinGain = 0.01;   // mm: smaller savings are noise
    constexpr double kCrossing = 2.0;   // mm of ratsnest one crossing is worth when ranking swaps
    while (r.pinSwaps + r.gateSwaps < maxSwaps) {
        const std::set<int> frozen = frozenNets(*this, pours);
        bool found = false;
        SwapCandidate best;
        double bestScore = 0, bestLength = 0;
        for (int pkg : packages)
            for (SwapCandidate& c : swapCandidates(schematic, pkg, pads, false)) {
                const PcbSwapOption& o = c.option;
                if (copied.count(o.component) || refused.count(swapKey(o))) continue;
                if (o.kind == "gate") {
                    const Component* other = schematic.find(o.other);
                    if (!other || copied.count(other->id) || !movable(other->unitOf)) continue;
                }
                // Pads that change nets: all on the board, on movable nets, and some pair of different nets.
                std::map<PadKey, int> moved;
                std::set<int> nets;
                bool legal = true, differs = false;
                for (const auto& [a, b] : c.pairs) {
                    const auto ia = pads.find(a), ib = pads.find(b);
                    if (ia == pads.end() || ib == pads.end()) {
                        legal = false;
                        break;
                    }
                    const int na = ia->second.net, nb = ib->second.net;
                    if ((na >= 0 && frozen.count(na)) || (nb >= 0 && frozen.count(nb))) legal = false;
                    differs = differs || na != nb;
                    moved[a] = nb;
                    moved[b] = na;
                    if (na >= 0) nets.insert(na);
                    if (nb >= 0) nets.insert(nb);
                }
                if (!legal || !differs) continue;
                // The affected nets' airwires before and after the swap.
                NetWires before, after;
                double dLength = 0;
                for (int net : nets) {
                    std::vector<PadKey> keys;
                    const auto it = model.netPads.find(net);
                    if (it != model.netPads.end())
                        for (const PadKey& k : it->second)
                            if (!moved.count(k)) keys.push_back(k);
                    for (const auto& [k, n] : moved)
                        if (n == net) keys.push_back(k);
                    std::sort(keys.begin(), keys.end());
                    std::vector<Vec2> pts;
                    for (const PadKey& k : keys) pts.push_back(pads.at(k).position);
                    dLength += spanningTree(pts, &after[net]);
                    const auto w = model.wires.find(net);
                    if (w != model.wires.end()) before[net] = w->second;
                    const auto l = model.length.find(net);
                    if (l != model.length.end()) dLength -= l->second;
                }
                const int dCross = crossingsOf(after, model.wires) - crossingsOf(before, model.wires);
                // Shorter without more crossings, or fewer crossings without being longer.
                const bool better = (dLength < -kMinGain && dCross <= 0) || (dLength <= 1e-9 && dCross < 0);
                if (!better) continue;
                const double score = dLength + kCrossing * dCross;
                if (!found || score < bestScore - 1e-12) {
                    found = true;
                    best = std::move(c);
                    bestScore = score;
                    bestLength = dLength;
                }
            }
        if (!found) break;
        const std::string key = swapKey(best.option);
        std::vector<std::string> lines;
        if (!applyPcbSwap(best.option, &lines)) {
            refused.insert(key);
            continue;
        }
        pours = pourNets(*this);  // net indices can change with the schematic's connectivity
        pads = padMap(*this);
        RatsnestModel next = ratsnestModel(pads, pours);
        if (next.total > model.total + bestLength + 1e-6) {
            // The schematic moved more than the swap's pads (it should not): undo it and never try it again.
            (void)applyPcbSwap(best.option);
            refused.insert(key);
            pours = pourNets(*this);
            pads = padMap(*this);
            model = ratsnestModel(pads, pours);
            continue;
        }
        model = std::move(next);
        (best.option.kind == "pin" ? r.pinSwaps : r.gateSwaps)++;
        for (auto& line : lines) r.report.push_back(std::move(line));
    }
    r.ratsnestAfter = model.total;
    if (r.pinSwaps + r.gateSwaps > 0) r.crossingsAfter = totalCrossings(model);
    return r;
}

RouteStats Project::autoRoute(const RouteControl& control) {
    if (!pcb.settings.autorouter.pinSwap) return pcb.autoRoute(schematic, control);
    // A cancelled route leaves the project as it was: the swaps are undone with it.
    std::unique_ptr<Project> before = control.progress ? std::make_unique<Project>(*this) : nullptr;
    const AutoSwapResult swaps = autoSwapForRouting();
    RouteStats st = pcb.autoRoute(schematic, control);
    if (st.cancelled) {
        if (before && swaps.pinSwaps + swaps.gateSwaps > 0) *this = std::move(*before);
        return st;
    }
    RouteMetrics& m = st.report.metrics;
    m.swapRun = true;
    m.pinSwaps = swaps.pinSwaps;
    m.gateSwaps = swaps.gateSwaps;
    m.ratsnestBefore = swaps.ratsnestBefore;
    m.ratsnestAfter = swaps.ratsnestAfter;
    m.crossingsBefore = swaps.crossingsBefore;
    m.crossingsAfter = swaps.crossingsAfter;
    st.report.swaps = swaps.report;
    pcb.lastRouteReport = st.report;
    return st;
}

}  // namespace sieda
