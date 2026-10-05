// SiEDA Core — length targets: length rules, match groups and xSignals (see LengthRules.hpp).
#include "sieda/LengthRules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <queue>
#include <set>

#include "sieda/SignalIntegrity.hpp"

namespace sieda {

namespace {
bool isSeriesKind(const Component& c) {
    if (c.def().pins.size() != 2 || !c.hasFootprint()) return false;
    switch (c.kind) {
        case ComponentKind::Resistor:
        case ComponentKind::Capacitor:
        case ComponentKind::Inductor:
        case ComponentKind::Fuse: return true;
        default: break;
    }
    // Library parts: two-pin passives by their designator (R, C, L, FB ferrite bead).
    const std::string& r = c.ref;
    return (r.size() >= 2 && (r[0] == 'R' || r[0] == 'C' || r[0] == 'L') && std::isdigit(static_cast<unsigned char>(r[1]))) ||
           (r.size() >= 3 && r.compare(0, 2, "FB") == 0 && std::isdigit(static_cast<unsigned char>(r[2])));
}
std::string netNameOf(const Schematic& sch, int n) {
    return n >= 0 && n < static_cast<int>(sch.nets().size()) ? sch.nets()[static_cast<size_t>(n)].name : std::string();
}
int netIndexOf(const Schematic& sch, const std::string& name) {
    for (const auto& n : sch.nets())
        if (n.name == name) return n.index;
    return -1;
}
}  // namespace

XSignal xSignalOf(const Schematic& sch, const std::vector<Pad>& pads, int net) {
    XSignal x;
    if (net < 0) return x;
    x.nets = {net};
    std::map<int, std::vector<size_t>> padsOfComp;
    for (size_t i = 0; i < pads.size(); ++i) padsOfComp[pads[i].componentId].push_back(i);
    std::set<int> parts;
    for (size_t q = 0; q < x.nets.size() && x.nets.size() < 8; ++q) {
        const int n = x.nets[q];
        for (size_t i = 0; i < pads.size(); ++i) {
            if (pads[i].net != n || parts.count(pads[i].componentId)) continue;
            const Component* c = sch.find(pads[i].componentId);
            if (!c || !isSeriesKind(*c)) continue;
            const auto& own = padsOfComp[c->id];
            if (own.size() != 2) continue;
            const Pad& other = pads[own[0] == i ? own[1] : own[0]];
            const int o = other.net;
            if (o < 0 || o == n || sch.netRole(o) != NetRole::Signal || sch.netRole(n) != NetRole::Signal) continue;
            if (sch.nets()[static_cast<size_t>(o)].pins.size() < 2) continue;  // an open pin: nothing beyond the part
            parts.insert(c->id);
            x.seriesParts.push_back(c->id);
            if (std::find(x.nets.begin(), x.nets.end(), o) == x.nets.end() && x.nets.size() < 8) x.nets.push_back(o);
        }
    }
    for (size_t i = 0; i < pads.size(); ++i)
        if (std::find(x.nets.begin(), x.nets.end(), pads[i].net) != x.nets.end() && !parts.count(pads[i].componentId))
            x.endPads.push_back(i);
    return x;
}

double xSignalLength(const PcbLayout& pcb, const std::vector<Pad>& pads, const XSignal& x) {
    if (x.endPads.size() < 2) {
        double len = 0;
        for (const auto& t : pcb.tracks)
            if (std::find(x.nets.begin(), x.nets.end(), t.net) != x.nets.end()) len += trackLength(t);
        return len;
    }
    // One graph over the xSignal's nets: track sections (vias add nothing, as in the net lengths), joined through
    // each series part's two pads at no length.
    std::vector<std::vector<std::pair<int, double>>> adj;
    std::map<size_t, int> padNode;
    for (int net : x.nets) {
        const NetCopperGraph g = buildNetCopperGraph(pcb, pads, net);
        const int base = static_cast<int>(adj.size());
        adj.resize(adj.size() + g.nodes.size());
        for (const auto& e : g.edges) {
            const double l = e.via ? 0.0 : e.length;
            adj[static_cast<size_t>(base + e.a)].push_back({base + e.b, l});
            adj[static_cast<size_t>(base + e.b)].push_back({base + e.a, l});
        }
        for (const auto& [pad, node] : g.padNode) padNode[pad] = base + node;
    }
    std::map<int, std::vector<size_t>> partPads;
    for (size_t i = 0; i < pads.size(); ++i)
        if (std::find(x.seriesParts.begin(), x.seriesParts.end(), pads[i].componentId) != x.seriesParts.end())
            partPads[pads[i].componentId].push_back(i);
    for (const auto& [comp, list] : partPads) {
        if (list.size() != 2 || !padNode.count(list[0]) || !padNode.count(list[1])) continue;
        const int a = padNode[list[0]], b = padNode[list[1]];
        adj[static_cast<size_t>(a)].push_back({b, 0.0});
        adj[static_cast<size_t>(b)].push_back({a, 0.0});
    }
    double longest = 0;
    for (size_t i = 0; i < x.endPads.size(); ++i) {
        auto it = padNode.find(x.endPads[i]);
        if (it == padNode.end()) return -1;
        std::vector<double> dist(adj.size(), std::numeric_limits<double>::infinity());
        using Q = std::pair<double, int>;
        std::priority_queue<Q, std::vector<Q>, std::greater<Q>> open;
        dist[static_cast<size_t>(it->second)] = 0;
        open.push({0, it->second});
        while (!open.empty()) {
            const auto [d, n] = open.top();
            open.pop();
            if (d > dist[static_cast<size_t>(n)]) continue;
            for (const auto& [m, l] : adj[static_cast<size_t>(n)])
                if (d + l < dist[static_cast<size_t>(m)]) {
                    dist[static_cast<size_t>(m)] = d + l;
                    open.push({d + l, m});
                }
        }
        for (size_t j = i + 1; j < x.endPads.size(); ++j) {
            auto jt = padNode.find(x.endPads[j]);
            if (jt == padNode.end() || !std::isfinite(dist[static_cast<size_t>(jt->second)])) return -1;
            longest = std::max(longest, dist[static_cast<size_t>(jt->second)]);
        }
    }
    return longest;
}

bool lengthTargetFor(const PcbLayout& pcb, const Schematic& sch, const std::vector<Pad>& pads, int net, LengthTarget& out) {
    const BoardSettings& s = pcb.settings;
    if (net < 0 || (s.lengthRules.empty() && s.matchGroups.empty())) return false;
    const std::string name = netNameOf(sch, net);
    out.xsignal = xSignalOf(sch, pads, net);
    for (const auto& r : s.lengthRules)
        if (r.net == name && r.target > 0) {
            out.target = r.target;
            out.tolerance = std::max(0.0, r.tolerance);
            out.source = "rule:" + r.net;
            return true;
        }
    for (const auto& g : s.matchGroups) {
        bool member = false;
        for (const auto& n : g.nets) {
            const int ni = netIndexOf(sch, n);
            member = member || std::find(out.xsignal.nets.begin(), out.xsignal.nets.end(), ni) != out.xsignal.nets.end();
        }
        if (!member) continue;
        double longest = 0;
        for (const auto& n : g.nets) {
            const int ni = netIndexOf(sch, n);
            if (ni >= 0) longest = std::max(longest, xSignalLength(pcb, pads, xSignalOf(sch, pads, ni)));
        }
        if (longest <= 0) continue;
        out.target = longest;
        out.tolerance = std::max(0.0, g.tolerance);
        out.source = "group:" + g.name;
        return true;
    }
    return false;
}

Json lengthTargetsJson(const PcbLayout& pcb, const Schematic& sch) {
    const auto pads = pcb.pads(sch);
    Json root = Json::object();
    Json rules = Json::array();
    for (const auto& r : pcb.settings.lengthRules) {
        Json j = Json::object();
        j["net"] = r.net;
        j["target"] = r.target;
        j["tolerance"] = r.tolerance;
        const int n = netIndexOf(sch, r.net);
        const double len = n >= 0 ? xSignalLength(pcb, pads, xSignalOf(sch, pads, n)) : -1;
        j["length"] = std::max(0.0, len);
        j["routed"] = len > 0;
        j["ok"] = len > 0 && std::fabs(len - r.target) <= r.tolerance + 1e-9;
        rules.push(j);
    }
    root["rules"] = rules;
    Json groups = Json::array();
    for (const auto& g : pcb.settings.matchGroups) {
        Json j = Json::object();
        j["name"] = g.name;
        j["tolerance"] = g.tolerance;
        Json members = Json::array();
        double longest = 0;
        std::vector<std::pair<Json, double>> rows;
        for (const auto& n : g.nets) {
            Json m = Json::object();
            m["net"] = n;
            const int ni = netIndexOf(sch, n);
            const XSignal x = ni >= 0 ? xSignalOf(sch, pads, ni) : XSignal{};
            Json xs = Json::array();
            for (int xn : x.nets) xs.push(netNameOf(sch, xn));
            m["xsignal"] = xs;
            const double len = ni >= 0 ? xSignalLength(pcb, pads, x) : -1;
            m["length"] = std::max(0.0, len);
            m["routed"] = len > 0;
            longest = std::max(longest, len);
            rows.push_back({m, len});
        }
        for (auto& [m, len] : rows) {
            m["ok"] = len > 0 && longest - len <= g.tolerance + 1e-9;
            members.push(m);
        }
        j["target"] = longest;
        j["members"] = members;
        groups.push(j);
    }
    root["groups"] = groups;
    return root;
}

std::vector<RuleViolation> lengthRuleViolations(const PcbLayout& pcb, const Schematic& sch) {
    std::vector<RuleViolation> out;
    if (pcb.settings.lengthRules.empty() && pcb.settings.matchGroups.empty()) return out;
    const auto pads = pcb.pads(sch);
    auto add = [&](const std::string& msg, int net) {
        RuleViolation v;
        v.severity = Severity::Warning;
        v.code = "DRC_LENGTH";
        v.message = msg;
        for (const auto& p : pads)
            if (p.net == net) {
                v.location = p.position;
                v.hasLocation = true;
                break;
            }
        out.push_back(v);
    };
    char buf[240];
    for (const auto& r : pcb.settings.lengthRules) {
        const int n = netIndexOf(sch, r.net);
        if (n < 0 || r.target <= 0) continue;
        const double len = xSignalLength(pcb, pads, xSignalOf(sch, pads, n));
        if (len <= 0 || std::fabs(len - r.target) <= r.tolerance + 1e-9) continue;
        std::snprintf(buf, sizeof buf, "%s is %.2f mm long; its length rule asks for %.2f ± %.2f mm.", r.net.c_str(), len,
                      r.target, r.tolerance);
        add(buf, n);
    }
    for (const auto& g : pcb.settings.matchGroups) {
        double longest = 0;
        std::vector<std::pair<int, double>> lens;
        for (const auto& name : g.nets) {
            const int n = netIndexOf(sch, name);
            if (n < 0) continue;
            const double len = xSignalLength(pcb, pads, xSignalOf(sch, pads, n));
            lens.push_back({n, len});
            longest = std::max(longest, len);
        }
        for (const auto& [n, len] : lens) {
            if (len <= 0 || longest - len <= g.tolerance + 1e-9) continue;
            std::snprintf(buf, sizeof buf, "%s is %.2f mm long, %.2f mm shorter than the longest of match group %s "
                          "(tolerance %.2f mm).", netNameOf(sch, n).c_str(), len, longest - len, g.name.c_str(), g.tolerance);
            add(buf, n);
        }
    }
    return out;
}

}  // namespace sieda
