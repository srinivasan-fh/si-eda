// SiEDA Core — pin and gate swapping from the board (Altium's PCB pin / gate swap): the swaps a placed multi-unit
// part allows (pins of one swap group, interchangeable gates of the same part), how much ratsnest each saves, and
// carrying a chosen swap out in the schematic (back-annotation), so board and schematic stay one design.
#include <algorithm>
#include <cmath>
#include <map>
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
}  // namespace

std::vector<PcbSwapOption> Project::pcbSwapOptions(int componentId) const {
    std::vector<PcbSwapOption> out;
    const Schematic& s = schematic;
    const int pkg = s.unitPackage(componentId);
    const Component* package = s.find(pkg);
    if (!package) return out;
    const CustomPart* part = CustomPartRegistry::instance().find(package->customPart);
    if (!part || part->units.empty()) return out;
    std::map<PadKey, PadInfo> pads;
    for (const auto& pad : pcb.pads(s)) pads[{pad.componentId, pad.pinIndex}] = {pad.position, pad.net};
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
                    PcbSwapOption o;
                    o.kind = "pin";
                    o.component = uid;
                    o.pinA = a;
                    o.pinB = b;
                    o.label = s.displayRef(*u) + ": pins " + pinNumber(pa) + " \xE2\x86\x94 " + pinNumber(pb);
                    o.gain = swapGain(pads, {{{pkg, pa}, {pkg, pb}}});
                    out.push_back(o);
                }
        // Gate swaps with interchangeable gates of the same part (this package or another one of the same value).
        for (const auto& other : s.components()) {
            if (!usable(&other) || other.id == uid || other.customPart != u->customPart || other.sheet != u->sheet) continue;
            if (other.unitOf == pkg && other.id < uid) continue;  // each pair inside one package once
            const Component* otherPkg = s.find(other.unitOf);
            if (!otherPkg || otherPkg->value != package->value) continue;
            const PartUnitDef& od = part->units[static_cast<size_t>(other.unit - 1)];
            if (!unitsInterchangeable(def, od) || od.pins.size() != def.pins.size()) continue;
            std::vector<std::pair<PadKey, PadKey>> pairs;
            for (size_t k = 0; k < def.pins.size(); ++k) pairs.push_back({{pkg, def.pins[k]}, {other.unitOf, od.pins[k]}});
            PcbSwapOption o;
            o.kind = "gate";
            o.component = uid;
            o.other = other.id;
            o.label = s.displayRef(*u) + " \xE2\x86\x94 " + s.displayRef(other);
            o.gain = swapGain(pads, pairs);
            out.push_back(o);
        }
    }
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
    auto keyOf = [](const PcbSwapOption& o) {
        const int other = o.kind == "gate" ? o.other : o.component;
        return o.kind + ":" + std::to_string(std::min(o.component, other)) + ":" + std::to_string(std::max(o.component, other)) +
               ":" + std::to_string(std::min(o.pinA, o.pinB)) + ":" + std::to_string(std::max(o.pinA, o.pinB));
    };
    while (done < maxSwaps) {
        PcbSwapOption best;
        best.gain = 0.01;  // mm: smaller savings are noise
        bool found = false;
        for (int pkg : packages)
            for (const auto& o : pcbSwapOptions(pkg))
                if (o.gain > best.gain && !made.count(keyOf(o))) {
                    best = o;
                    found = true;
                }
        if (!found) break;
        made.insert(keyOf(best));
        if (!applyPcbSwap(best, report)) break;
        ++done;
    }
    return done;
}

}  // namespace sieda
