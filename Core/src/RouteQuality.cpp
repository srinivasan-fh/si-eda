// SiEDA Core — the autorouter's passes after routing (RouteQuality.hpp).
#include "RouteQuality.hpp"

#include <algorithm>
#include <cmath>

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
