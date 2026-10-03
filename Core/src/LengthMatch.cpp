#include "sieda/LengthMatch.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>

namespace sieda {

namespace {

std::string upperName(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

/// "DQ7" → ("DQ", 7), "DATA[3]" → ("DATA", 3), "DDR_DQ12" → ("DDR_DQ", 12); index -1 when there is none.
std::pair<std::string, int> splitIndex(const std::string& name) {
    std::string n = name;
    if (!n.empty() && n.back() == ']') {
        auto open = n.rfind('[');
        if (open == std::string::npos) return {name, -1};
        n = n.substr(0, open) + n.substr(open + 1, n.size() - open - 2);
    }
    size_t i = n.size();
    while (i > 0 && std::isdigit(static_cast<unsigned char>(n[i - 1]))) --i;
    if (i == n.size() || i == 0 || n.size() - i > 3) return {name, -1};
    std::string prefix = n.substr(0, i);
    while (!prefix.empty() && (prefix.back() == '_' || prefix.back() == '.')) prefix.pop_back();
    return {prefix, std::stoi(n.substr(i))};
}

/// Bus prefixes that carry matched-length parallel data (plain A / D are left out: they are MCU pin names).
bool busPrefix(const std::string& p) {
    static const char* names[] = {"DQ", "DATA", "DB", "ADDR", "ADR", "BA", "DM", "RXD", "TXD", "RD", "TD", "LVDS"};
    for (const char* n : names)
        if (p == n || (p.size() > std::string(n).size() + 1 && p.compare(p.size() - std::string(n).size(),
                                                                            std::string::npos, n) == 0 &&
                       p[p.size() - std::string(n).size() - 1] == '_'))
            return true;
    return p.find("DDR") != std::string::npos;
}

}  // namespace

std::vector<std::pair<int, int>> differentialPairs(const Schematic& sch) {
    std::vector<std::pair<int, int>> out;
    std::map<std::string, int> byName;
    for (const auto& n : sch.nets())
        if (!n.isGround && !n.name.empty()) byName[upperName(n.name)] = n.index;
    for (const auto& entry : byName) {
        const std::string& name = entry.first;
        const int idx = entry.second;
        auto partner = [&](const std::string& pos, const std::string& neg) -> int {
            if (name.size() <= pos.size() || name.compare(name.size() - pos.size(), pos.size(), pos) != 0) return -1;
            auto it = byName.find(name.substr(0, name.size() - pos.size()) + neg);
            return it == byName.end() ? -1 : it->second;
        };
        for (auto [pos, neg] : std::initializer_list<std::pair<const char*, const char*>>{
                 {"_P", "_N"}, {"+", "-"}, {"_DP", "_DN"}, {"P", "N"}}) {
            int other = partner(pos, neg);
            if (other >= 0 && sch.netRole(idx) == NetRole::Signal && sch.netRole(other) == NetRole::Signal) {
                out.push_back({idx, other});
                break;
            }
        }
    }
    return out;
}

std::vector<LengthGroup> lengthGroups(const Schematic& sch, const BoardSettings& s) {
    std::vector<LengthGroup> out;
    const auto& nets = sch.nets();
    auto nameOf = [&](int idx) { return idx >= 0 && idx < static_cast<int>(nets.size()) ? nets[idx].name : ""; };
    for (auto [p, n] : differentialPairs(sch)) {
        LengthGroup g;
        std::string a = nameOf(p);
        // Common stem: the positive name without its polarity suffix.
        size_t k = 0;
        std::string b = nameOf(n);
        while (k < a.size() && k < b.size() && a[k] == b[k]) ++k;
        g.name = a.substr(0, k);
        while (!g.name.empty() && (g.name.back() == '_' || g.name.back() == '.')) g.name.pop_back();
        if (g.name.empty()) g.name = a;
        g.kind = "pair";
        g.nets = {p, n};
        g.tolerance = s.pairSkewTolerance;
        out.push_back(g);
    }
    std::map<std::string, std::vector<std::pair<int, int>>> buses;
    for (const auto& n : nets) {
        if (n.isGround || sch.netRole(n.index) != NetRole::Signal) continue;
        auto [prefix, index] = splitIndex(upperName(n.name));
        if (index >= 0 && busPrefix(prefix)) buses[prefix].push_back({index, n.index});
    }
    for (auto& [prefix, members] : buses) {
        if (members.size() < 2) continue;
        std::sort(members.begin(), members.end());
        LengthGroup g;
        g.name = prefix;
        g.kind = "bus";
        for (const auto& m : members) g.nets.push_back(m.second);
        g.tolerance = s.busLengthTolerance;
        out.push_back(g);
    }
    return out;
}

double routedNetLength(const PcbLayout& pcb, int net) {
    double len = 0;
    for (const auto& t : pcb.tracks)
        if (t.net == net) len += (t.b - t.a).length();
    return len;
}

namespace {

/// Serpentine replacing track `t`, adding `extra` mm: `bumps` bumps of height extra / (2·bumps) on one side,
/// centred on the track.
std::vector<Vec2> serpentine(const Track& t, double extra, double side, double pitch, int bumps) {
    Vec2 d = t.b - t.a;
    double len = d.length();
    Vec2 u = d * (1.0 / len);
    Vec2 n = Vec2{-u.y, u.x} * side;
    double a = extra / (2.0 * bumps);
    double start = (len - 2.0 * pitch * bumps) / 2.0;
    std::vector<Vec2> pts = {t.a};
    Vec2 p = t.a + u * start;
    pts.push_back(p);
    for (int i = 0; i < bumps; ++i) {
        p = p + n * a;
        pts.push_back(p);
        p = p + u * pitch;
        pts.push_back(p);
        p = p - n * a;
        pts.push_back(p);
        p = p + u * pitch;
        pts.push_back(p);
    }
    pts.push_back(t.b);
    return pts;
}

}  // namespace

int tuneLengths(PcbLayout& pcb, const Schematic& sch) {
    const BoardSettings& s = pcb.settings;
    const auto pads = pcb.pads(sch);
    const double clr = s.clearance;
    auto clear = [&](int layer, int net, Vec2 a, Vec2 b, double w, size_t skip) {
        for (size_t i = 0; i < pcb.tracks.size(); ++i) {
            const Track& t = pcb.tracks[i];
            if (i == skip || t.layer != layer || t.net == net) continue;
            if (segmentSegmentDistance(a, b, t.a, t.b) - (w + t.width) / 2 < clr - 1e-6) return false;
        }
        for (const auto& pd : pads) {
            if (!pd.onLayer(layer) || (pd.net == net && net >= 0)) continue;
            double d = pd.round ? pointSegmentDistance(pd.position, a, b) - std::min(pd.size.x, pd.size.y) / 2
                                : segmentRectDistance(a, b, pd.bounds());
            if (d - w / 2 < clr - 1e-6) return false;
        }
        for (const auto& v : pcb.vias) {
            if (v.net == net || !v.spans(layer)) continue;
            if (pointSegmentDistance(v.position, a, b) - v.diameter / 2 - w / 2 < clr - 1e-6) return false;
        }
        if (s.segmentEdgeDistance(a, b) < s.edgeClearance + w / 2 - 1e-6) return false;
        for (Vec2 p : {a, b, (a + b) * 0.5})
            if (s.holeDistance(p) < w / 2) return false;
        return true;
    };
    // Same-net pads the serpentine must stay clear of too (it may not run over another pad of its own net).
    auto clearOwnPads = [&](int layer, int net, Vec2 a, Vec2 b, double w) {
        for (const auto& pd : pads) {
            if (!pd.onLayer(layer) || pd.net != net) continue;
            double d = segmentRectDistance(a, b, pd.bounds());
            if (d - w / 2 < clr - 1e-6) return false;
        }
        return true;
    };

    int tuned = 0;
    for (const auto& g : lengthGroups(sch, s)) {
        double target = 0;
        for (int n : g.nets) target = std::max(target, routedNetLength(pcb, n));
        if (target <= 0) continue;
        for (int net : g.nets) {
            double len = routedNetLength(pcb, net);
            if (len <= 0) continue;  // unrouted: nothing to tune
            double extra = target - len;
            if (extra <= g.tolerance / 2) continue;
            // Candidate tracks: this net's, longest first.
            std::vector<size_t> cand;
            for (size_t i = 0; i < pcb.tracks.size(); ++i)
                if (pcb.tracks[i].net == net) cand.push_back(i);
            std::sort(cand.begin(), cand.end(), [&](size_t x, size_t y) {
                return (pcb.tracks[x].b - pcb.tracks[x].a).length() > (pcb.tracks[y].b - pcb.tracks[y].a).length();
            });
            bool done = false;
            for (size_t ci : cand) {
                if (done) break;
                const Track t = pcb.tracks[ci];
                const double w = t.width, L = (t.b - t.a).length();
                const double pitch = std::max(w + clr, 3 * w);  // leg spacing: ≥ 3W keeps self-coupling low
                const double margin = std::max(2 * w, 0.5);
                const int maxBumps = static_cast<int>(std::floor((L - 2 * margin) / (2 * pitch)));
                if (maxBumps < 1) continue;
                for (double amax : {0.6, 1.0, 1.5, 2.0, 3.0}) {
                    if (done) break;
                    int bumps = std::max(1, static_cast<int>(std::ceil(extra / (2 * amax))));
                    if (bumps > maxBumps) continue;
                    for (double side : {1.0, -1.0}) {
                        auto pts = serpentine(t, extra, side, pitch, bumps);
                        bool ok = true;
                        for (size_t k = 0; ok && k + 1 < pts.size(); ++k) {
                            ok = clear(t.layer, net, pts[k], pts[k + 1], w, ci);
                            // The bumps (not the lead-in / lead-out on the original line) must not touch own pads.
                            if (ok && k > 0 && k + 2 < pts.size()) ok = clearOwnPads(t.layer, net, pts[k], pts[k + 1], w);
                        }
                        if (!ok) continue;
                        pcb.tracks.erase(pcb.tracks.begin() + static_cast<long>(ci));
                        for (size_t k = 0; k + 1 < pts.size(); ++k) {
                            if ((pts[k + 1] - pts[k]).length() < 1e-6) continue;
                            Track nt = t;
                            nt.a = pts[k];
                            nt.b = pts[k + 1];
                            pcb.addTrack(nt);
                        }
                        ++tuned;
                        done = true;
                        break;
                    }
                }
            }
        }
    }
    return tuned;
}

Json lengthReportJson(const PcbLayout& pcb, const Schematic& sch) {
    const auto& nets = sch.nets();
    Json groups = Json::array();
    for (const auto& g : lengthGroups(sch, pcb.settings)) {
        Json j = Json::object();
        j["name"] = g.name;
        j["kind"] = g.kind;
        j["tolerance"] = g.tolerance;
        double target = 0;
        for (int n : g.nets) target = std::max(target, routedNetLength(pcb, n));
        j["target"] = target;
        bool matched = true;
        Json members = Json::array();
        for (int n : g.nets) {
            double len = routedNetLength(pcb, n);
            Json m = Json::object();
            m["name"] = n >= 0 && n < static_cast<int>(nets.size()) ? nets[n].name : "";
            m["length"] = len;
            m["delta"] = target - len;
            m["routed"] = len > 0;
            bool ok = len > 0 && target - len <= g.tolerance + 1e-9;
            m["ok"] = ok;
            matched = matched && ok;
            members.push(m);
        }
        j["matched"] = matched;
        j["nets"] = members;
        groups.push(j);
    }
    Json root = Json::object();
    root["groups"] = groups;
    root["enabled"] = pcb.settings.lengthTuning;
    root["pairSkewTolerance"] = pcb.settings.pairSkewTolerance;
    root["busLengthTolerance"] = pcb.settings.busLengthTolerance;
    return root;
}

}  // namespace sieda
