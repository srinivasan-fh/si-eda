#include "sieda/Memory.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <map>
#include <set>

#include "SystemParts.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Project.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda {

namespace {
using namespace sysparts;

/// A pin name as the checks compare it: upper case, KiCad markup removed ("V_{DDQ}" → "VDDQ"), so a datasheet name
/// from any library matches.
std::string norm(std::string s) {
    s = up(s);
    s.erase(std::remove_if(s.begin(), s.end(), [](char ch) { return ch == '{' || ch == '}'; }), s.end());
    if (s.size() > 2 && s[0] == 'V' && s[1] == '_') s.erase(1, 1);
    return s;
}

/// The alternatives of a multi-function pin ("A10/AP" → A10, AP).
std::vector<std::string> aliases(const std::string& name) {
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= name.size(); ++i)
        if (i == name.size() || name[i] == '/') {
            if (i > start) out.push_back(name.substr(start, i - start));
            start = i + 1;
        }
    return out;
}

bool pinIs(const Component& c, int pin, std::initializer_list<const char*> names) {
    for (const auto& a : aliases(norm(c.def().pins[static_cast<size_t>(pin)].name)))
        for (const char* n : names)
            if (a == n) return true;
    return false;
}

/// "DQ7", "A12", "BA1": the prefix followed by digits only.
bool indexed(const std::string& name, const char* prefix) {
    const size_t n = std::string(prefix).size();
    if (name.size() <= n || name.compare(0, n, prefix) != 0) return false;
    return std::all_of(name.begin() + static_cast<long>(n), name.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)); });
}

/// The bit number of an indexed pin (DQ12 → 12), -1 when the pin is not `prefix` + digits.
int pinBit(const Component& c, int pin, const char* prefix) {
    for (const auto& a : aliases(norm(c.def().pins[static_cast<size_t>(pin)].name)))
        if (indexed(a, prefix)) return std::atoi(a.c_str() + std::string(prefix).size());
    return -1;
}

bool pinIndexed(const Component& c, int pin, const char* prefix) {
    for (const auto& a : aliases(norm(c.def().pins[static_cast<size_t>(pin)].name)))
        if (indexed(a, prefix)) return true;
    return false;
}

bool isDram(const Component& c) {
    if (c.kind != ComponentKind::Custom) return false;
    if (containsAny(partName(c), {"MT48LC", "W9812", "W9825", "W9864", "IS42S", "IS45S", "AS4C", "MT41K", "MT41J",
                                  "MT40A", "MT60B", "MT53", "IS43", "IS46", "K4B", "K4A", "K4F", "H5AN", "H5TQ", "SDRAM",
                                  "LPDDR", "DDR3", "DDR4", "DDR5"}))
        return true;
    int dq = 0;
    bool bank = false;
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
        dq += pinIndexed(c, i, "DQ");
        bank = bank || pinIndexed(c, i, "BA");
    }
    return dq >= 4 && bank;
}

bool isDdr(const Component& c) {
    if (containsAny(partName(c), {"MT41", "MT40A", "MT60B", "MT53", "AS4C256M16D3", "IS43", "IS46", "K4B", "K4A", "K4F", "H5AN",
                                  "H5TQ", "DDR"}))
        return true;
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (pinIs(c, i, {"ZQ", "ZQ0", "ZQ1", "VREFCA", "VREFDQ", "ODT"})) return true;
    return false;
}

bool moduleType(const std::string& t) { return t == "dimm" || t == "rdimm"; }

bool netConnected(const Schematic& sch, int net) {
    return net >= 0 && sch.nets()[static_cast<size_t>(net)].pins.size() >= 2;
}

struct Dram {
    const Component* part = nullptr;
    bool ddr = false;
    std::vector<int> dqNets, dqBits, addrNets, supplyNets, vrefNets, zqNets, resetNets;
    std::vector<std::pair<int, int>> strobes;  // (positive, negative) DQS nets
    std::vector<int> strobeLanes;              // byte lane of each strobe (LDQS / DQS0 = 0, UDQS / DQS1 = 1)
    int clock = -1, clockN = -1;               // CLK (SDR) or CK / CK# (DDR)
    int cke = -1;
    int supplyPins = 0;
    const Component* controller = nullptr;
};

struct Analysis {
    Parts parts;
    std::vector<Dram> drams;
    std::vector<const Component*> eeproms;  // SPD EEPROMs
    bool ddr = false;
};

Analysis analyse(const Project& project) {
    const Schematic& sch = project.schematic;
    Analysis a;
    a.parts = classify(sch);
    const int gnd = sch.groundNet();
    for (const auto& c : sch.components()) {
        if (c.kind == ComponentKind::Custom &&
            containsAny(partName(c), {"24C0", "24AA0", "24LC0", "AT24C", "AT24CS", "M24C", "EE1004", "SPD5118", "34C0", "34AA"}))
            a.eeproms.push_back(&c);
        if (!isDram(c)) continue;
        Dram d;
        d.part = &c;
        d.ddr = isDdr(c);
        std::map<std::string, int> byName;
        for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
            const int net = sch.netOf({c.id, i});
            for (const auto& al : aliases(norm(c.def().pins[static_cast<size_t>(i)].name))) byName[al] = net;
            if (net < 0) continue;
            if (pinIndexed(c, i, "DQ") && netConnected(sch, net)) {  // wired data lines
                d.dqNets.push_back(net);
                d.dqBits.push_back(pinBit(c, i, "DQ"));
            }
            if (pinIndexed(c, i, "A") || pinIndexed(c, i, "BA") || pinIndexed(c, i, "BG")) d.addrNets.push_back(net);
            if (pinIs(c, i, {"VDD", "VDDQ", "VDD1", "VDD2", "VDDQ1", "VDDQ2"})) {
                ++d.supplyPins;
                if (net != gnd && std::find(d.supplyNets.begin(), d.supplyNets.end(), net) == d.supplyNets.end())
                    d.supplyNets.push_back(net);
            }
            if (pinIs(c, i, {"VREFCA", "VREFDQ", "VREF"})) d.vrefNets.push_back(net);
            if (pinIs(c, i, {"ZQ", "ZQ0", "ZQ1"})) d.zqNets.push_back(net);
            if (pinIs(c, i, {"NRESET", "RESET#", "RESET_N", "RESET"})) d.resetNets.push_back(net);
            if (pinIs(c, i, {"CKE", "CKE0"})) d.cke = net;
        }
        auto net = [&](std::initializer_list<const char*> names) {
            for (const char* n : names)
                if (auto it = byName.find(n); it != byName.end() && it->second >= 0) return it->second;
            return -1;
        };
        d.clock = d.ddr ? net({"CK", "CK_T", "CKT", "CK_P"}) : net({"CLK", "CK", "SDCLK"});
        d.clockN = net({"NCK", "CK#", "CK_C", "CKC", "CK_N"});
        for (const char* s : {"DQS", "LDQS", "UDQS", "DQSL", "DQSU", "DQS0", "DQS1"}) {
            const std::string p = s;
            int pos = -1, neg = -1;
            for (const std::string& n : {p, p + "_T"})
                if (auto it = byName.find(n); it != byName.end() && it->second >= 0) pos = it->second;
            for (const std::string& n : {"N" + p, p + "#", p + "_C", p + "_N"})
                if (auto it = byName.find(n); it != byName.end() && it->second >= 0) neg = it->second;
            if (pos >= 0 || neg >= 0) {
                d.strobes.emplace_back(pos, neg);
                d.strobeLanes.push_back(p[0] == 'U' || p == "DQSU" || p == "DQS1" ? 1 : 0);
            }
        }
        // The controller: the part (other than this DRAM and passives) on most of the data nets.
        std::map<int, int> hits;
        for (int dq : d.dqNets)
            for (const auto& pin : sch.nets()[static_cast<size_t>(dq)].pins) {
                const Component* other = sch.find(pin.component);
                if (other && other != &c && other->kind == ComponentKind::Custom && !isDram(*other)) ++hits[other->id];
            }
        int best = 0;
        for (const auto& [id, n] : hits)
            if (n > best && n * 2 >= static_cast<int>(d.dqNets.size())) {
                best = n;
                d.controller = sch.find(id);
            }
        a.ddr = a.ddr || d.ddr;
        a.drams.push_back(std::move(d));
    }
    const std::string& t = project.memoryDesign;
    if (t == "ddr" || t == "lpddr" || moduleType(t)) a.ddr = true;
    return a;
}

/// Two-pin parts of `kind` with one pin on `net` (the other pin's net is returned alongside).
std::vector<std::pair<const Component*, int>> attached(const Schematic& sch, int net, ComponentKind kind) {
    std::vector<std::pair<const Component*, int>> out;
    if (net < 0) return out;
    for (const auto& c : sch.components()) {
        if (c.kind != kind || c.def().pins.size() != 2) continue;
        const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        if (n0 == net && n1 != net) out.push_back({&c, n1});
        else if (n1 == net && n0 != net) out.push_back({&c, n0});
    }
    return out;
}

bool isGround(const Schematic& sch, int net) {
    return net >= 0 && (net == sch.groundNet() || sch.netRole(net) == NetRole::Ground);
}

/// DDR layout limits on the routed bus, by memory type: typical values from public DRAM and controller layout guides
/// (Micron DDR3 / DDR4 / LPDDR technical notes, TI / NXP DDR layout application notes). A controller's own guide wins.
struct DdrLayoutLimits {
    double laneSkewPs, addrSkewPs, impedanceTol;  // byte-lane DQ skew; command / address to CK; ± fraction of target
    int laneViaSpread;                            // vias: every bit of a lane within this many of each other
    double dqsSkewPs;                             // each DQ of a lane to its strobe
    double pairSkewPs;                            // P to N of the clock and strobe pairs
};
DdrLayoutLimits ddrLayoutLimits(const std::string& type) {
    if (type == "sdram") return {50, 100, 0.15, 1, 50, 10};
    if (type == "lpddr") return {5, 15, 0.10, 0, 5, 1};
    return {10, 25, 0.10, 0, 10, 2};  // DDR3L / DDR4 memory-down, DIMMs
}

/// The type's limits with the project's overrides (from the controller's layout guide).
DdrLayoutLimits effectiveLimits(const Project& p) {
    DdrLayoutLimits l = ddrLayoutLimits(p.memoryDesign);
    const auto& o = p.memoryLimits;
    if (o.laneSkewPs > 0) l.laneSkewPs = o.laneSkewPs;
    if (o.dqsSkewPs > 0) l.dqsSkewPs = o.dqsSkewPs;
    if (o.addrSkewPs > 0) l.addrSkewPs = o.addrSkewPs;
    if (o.impedanceTolPercent > 0) l.impedanceTol = o.impedanceTolPercent / 100;
    if (o.laneViaSpread >= 0) l.laneViaSpread = o.laneViaSpread;
    if (o.pairSkewPs > 0) l.pairSkewPs = o.pairSkewPs;
    return l;
}

Json limitsJson(const DdrLayoutLimits& l) {
    Json j = Json::object();
    j["laneSkewPs"] = l.laneSkewPs, j["dqsSkewPs"] = l.dqsSkewPs, j["addrSkewPs"] = l.addrSkewPs;
    j["impedanceTolPercent"] = l.impedanceTol * 100, j["laneViaSpread"] = l.laneViaSpread, j["pairSkewPs"] = l.pairSkewPs;
    return j;
}

/// Routed delay of a net (ps): every track's length × the propagation delay of its layer and width.
double routedDelayPs(const PcbLayout& pcb, int net) {
    double ps = 0;
    for (const auto& t : pcb.tracks)
        if (t.net == net && !t.teardrop) ps += trackLength(t) * propagationDelayPerMm(pcb.settings, t.layer, t.width) * 1e12;
    return ps;
}

double valueOf(const Component& c) { return parseEngineeringValue(primaryValue(c.value)).value_or(0); }

/// Routed-bus checks of one DDR device: byte lanes (DQ0–7, DQ8–15, …) matched in delay, through the same vias and
/// layers; DQ tracks at the impedance target; fly-by command / address matched to the clock. Unrouted nets are skipped.
template <class Add>
void ddrLayoutChecks(const PcbLayout& pcb, const Dram& d, const DdrLayoutLimits& lim, Add add) {
    const Component& m = *d.part;
    const Severity sev = d.ddr ? Severity::Warning : Severity::Info;  // SDR SDRAM at ≤ 166 MHz: advice only
    auto routed = [&](int net) {
        return std::any_of(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return t.net == net; });
    };
    std::map<int, std::vector<int>> lanes;  // lane → nets
    for (size_t k = 0; k < d.dqNets.size(); ++k)
        if (d.dqBits[k] >= 0 && routed(d.dqNets[k])) lanes[d.dqBits[k] / 8].push_back(d.dqNets[k]);
    for (const auto& [lane, nets] : lanes) {
        if (nets.size() < 2) continue;
        double lo = 1e18, hi = 0;
        int vLo = 1 << 30, vHi = 0;
        std::set<std::set<int>> layerSets;
        for (int n : nets) {
            const double ps = routedDelayPs(pcb, n);
            lo = std::min(lo, ps), hi = std::max(hi, ps);
            const int vias = static_cast<int>(std::count_if(pcb.vias.begin(), pcb.vias.end(), [&](const Via& v) { return v.net == n; }));
            vLo = std::min(vLo, vias), vHi = std::max(vHi, vias);
            std::set<int> layers;
            for (const auto& t : pcb.tracks)
                if (t.net == n) layers.insert(t.layer);
            layerSets.insert(layers);
        }
        const std::string name = m.ref + " byte lane " + std::to_string(lane) + " (DQ" + std::to_string(lane * 8) + "–" +
                                 std::to_string(lane * 8 + 7) + ")";
        if (hi - lo > lim.laneSkewPs)
            add(sev, "MEM_DDR_LANE_SKEW",
                name + " has " + fmt("%.1f ps", hi - lo) + " of skew between its bits: keep it within " +
                    fmt("%.0f ps", lim.laneSkewPs) + " (Tune Lengths matches the lane by delay).",
                {m.id});
        if (vHi - vLo > lim.laneViaSpread)
            add(sev, "MEM_DDR_LANE_VIAS",
                name + " uses " + std::to_string(vLo) + " to " + std::to_string(vHi) + " vias per bit: route every bit of "
                "a lane through the same number of vias so their delay and discontinuities match.",
                {m.id});
        // Each bit against the lane's strobe (the clock its data is captured on).
        for (size_t k = 0; k < d.strobes.size(); ++k) {
            const int dqs = d.strobes[k].first >= 0 ? d.strobes[k].first : d.strobes[k].second;
            if (k >= d.strobeLanes.size() || d.strobeLanes[k] != lane || dqs < 0 || !routed(dqs)) continue;
            const double s = routedDelayPs(pcb, dqs), worst = std::max(std::fabs(hi - s), std::fabs(lo - s));
            if (worst > lim.dqsSkewPs)
                add(sev, "MEM_DDR_DQS_SKEW",
                    name + " differs from its strobe by up to " + fmt("%.1f ps", worst) + ": match DQ to DQS within " +
                        fmt("%.0f ps", lim.dqsSkewPs) + " (Tune Lengths with the strobe in the lane's group).",
                    {m.id});
        }
        if (layerSets.size() > 1)
            add(Severity::Info, "MEM_DDR_LANE_LAYERS",
                name + " is routed on different layers bit to bit: keep a lane on the same layers (same reference planes, "
                "same propagation delay).",
                {m.id});
    }
    // Routed DQ impedance against the board's single-ended target, on each net's main width (the layer and width
    // carrying most of its length; short fine-pitch neck-downs at the pins are left out).
    const double target = pcb.settings.singleEndedImpedance;
    std::set<std::pair<int, double>> seen;
    for (int net : d.dqNets) {
        std::map<std::pair<int, double>, double> runs;
        for (const auto& tr : pcb.tracks)
            if (tr.net == net && !tr.teardrop) runs[{tr.layer, tr.width}] += trackLength(tr);
        if (runs.empty()) continue;
        const auto main = std::max_element(runs.begin(), runs.end(), [](const auto& x, const auto& y) { return x.second < y.second; });
        const Track t = [&] { Track k; k.layer = main->first.first; k.width = main->first.second; return k; }();
        if (!seen.insert({t.layer, t.width}).second) continue;
        const double z = trackImpedance(pcb.settings, t.layer, t.width);
        if (target > 0 && z > 0 && std::fabs(z - target) > lim.impedanceTol * target) {
            add(sev, "MEM_DDR_IMPEDANCE",
                m.ref + "'s data tracks " + fmt("%.3f mm", t.width) + " wide on " +
                    copperLayerName(t.layer, pcb.settings.layerCount) + " are " + fmt("%.0f Ω", z) + ", not " +
                    fmt("%.0f Ω", target) + " ± " + fmt("%.0f %%", lim.impedanceTol * 100) +
                    ": use the stack-up's width for that layer.",
                {m.id});
            break;
        }
    }
    // Differential clock and strobes: P and N matched (a skewed pair shifts its crossing point and converts to common mode).
    std::vector<std::pair<std::string, std::pair<int, int>>> pairs{{"CK", {d.clock, d.clockN}}};
    for (size_t k = 0; k < d.strobes.size(); ++k)
        pairs.push_back({"DQS" + std::to_string(k < d.strobeLanes.size() ? d.strobeLanes[k] : 0), d.strobes[k]});
    for (const auto& [label, pn] : pairs)
        if (pn.first >= 0 && pn.second >= 0 && routed(pn.first) && routed(pn.second))
            if (const double skew = std::fabs(routedDelayPs(pcb, pn.first) - routedDelayPs(pcb, pn.second)); skew > lim.pairSkewPs)
                add(sev, "MEM_DDR_PAIR_SKEW",
                    m.ref + "'s " + label + " pair has " + fmt("%.1f ps", skew) + " between P and N: match it within " +
                        fmt("%.0f ps", lim.pairSkewPs) + " (Tune Lengths on the pair).",
                    {m.id});
    // Fly-by command / address against the clock.
    if (d.clock >= 0 && routed(d.clock)) {
        const double ck = routedDelayPs(pcb, d.clock);
        double worst = 0;
        for (int n : d.addrNets)
            if (routed(n)) worst = std::max(worst, std::fabs(routedDelayPs(pcb, n) - ck));
        if (worst > lim.addrSkewPs)
            add(sev, "MEM_DDR_ADDR_SKEW",
                m.ref + "'s command / address lines differ from its clock by up to " + fmt("%.1f ps", worst) +
                    ": match them to CK within " + fmt("%.0f ps", lim.addrSkewPs) + " (they are captured on its edge).",
                {m.id});
    }
}

/// strict: full severities (a memory design type is set); otherwise everything is Info.
std::vector<RuleViolation> checks(const Project& project, bool strict, const Analysis& a) {
    std::vector<RuleViolation> out;
    if (!isMemoryProject(project)) return out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const std::string& type = project.memoryDesign;
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = strict ? sev : Severity::Info;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };

    if (a.drams.empty()) {
        add(Severity::Info, "REL_DRAM_NONE",
            "No DRAM found (a part with DQ and BA pins, e.g. MT48LC16M16A2, MT41K256M16 or MT40A512M16): the memory "
            "checks are skipped.");
    }

    for (const Dram& d : a.drams) {
        const Component& m = *d.part;
        // ------------------------------------------------------------ 1. power & decoupling
        int decoupling = 0;
        bool bulk = false;
        for (int net : d.supplyNets)
            for (const auto& [cap, other] : attached(sch, net, ComponentKind::Capacitor))
                if (isGround(sch, other)) {
                    ++decoupling;
                    bulk = bulk || valueOf(*cap) >= 4.7e-6;
                }
        const int needed = (d.supplyPins + 1) / 2;
        if (decoupling < needed)
            add(Severity::Warning, "REL_DRAM_DECOUPLING",
                m.ref + " has " + std::to_string(d.supplyPins) + " VDD / VDDQ pins but " + std::to_string(decoupling) +
                    " decoupling capacitors on its supplies: fit at least one 100 nF per pin pair (" +
                    std::to_string(needed) + "), next to the pins, to carry the burst current of the output drivers.",
                {m.id});
        if (!d.supplyNets.empty() && !bulk)
            add(Severity::Info, "REL_DRAM_BULK",
                m.ref + "'s supply has no bulk capacitor: add 10–22 µF near the DRAM for the refresh and burst current.",
                {m.id});

        if (d.ddr) {
            // VREF: a filtered half-VDDQ reference (divider or the VTT regulator's VTTREF).
            for (int vref : d.vrefNets) {
                const bool cap = !attached(sch, vref, ComponentKind::Capacitor).empty();
                const bool divider = attached(sch, vref, ComponentKind::Resistor).size() >= 2;
                bool regulator = false;
                for (const auto& pin : sch.nets()[static_cast<size_t>(vref)].pins)
                    if (const Component* other = sch.find(pin.component); other && other != &m && other->kind == ComponentKind::Custom)
                        regulator = true;
                if (!cap || !(divider || regulator)) {
                    add(Severity::Warning, "REL_DDR_VREF",
                        m.ref + "'s VREF is not a filtered VDDQ / 2: use a 1 % resistor divider from VDDQ (or the VTT "
                        "regulator's VTTREF output) with 100 nF to ground at the pin.",
                        {m.id});
                    break;
                }
            }
            // ZQ: 240 Ω 1 % to ground for driver / ODT calibration.
            for (int zq : d.zqNets) {
                const Component* r = nullptr;
                for (const auto& [res, other] : attached(sch, zq, ComponentKind::Resistor))
                    if (isGround(sch, other)) r = res;
                if (!r) {
                    add(Severity::Warning, "REL_DDR_ZQ",
                        m.ref + "'s ZQ pin needs a 240 Ω 1 % resistor to ground: the DRAM calibrates its output drivers "
                        "and on-die termination against it.",
                        {m.id});
                } else if (std::fabs(valueOf(*r) - 240) > 2.5) {
                    add(Severity::Warning, "REL_DDR_ZQ",
                        r->ref + " on " + m.ref + "'s ZQ pin is " + r->value + ": JEDEC calibration needs 240 Ω 1 %.",
                        {m.id, r->id});
                }
            }
            // RESET_n held low while the supplies ramp (DDR3 / DDR4).
            for (int reset : d.resetNets) {
                bool pulldown = false;
                for (const auto& [res, other] : attached(sch, reset, ComponentKind::Resistor)) pulldown |= isGround(sch, other);
                if (!pulldown) {
                    add(Severity::Warning, "REL_DDR_RESET",
                        m.ref + "'s RESET_n has no pull-down: a 10 kΩ to ground keeps the DRAM in reset until the "
                        "controller releases it, after VDD / VDDQ are stable (JESD79-3 / -4 power-up sequence).",
                        {m.id});
                    break;
                }
            }
            // Fly-by command / address bus terminated to VTT (not on LPDDR, which is point-to-point and unterminated).
            if (type != "lpddr" && !d.addrNets.empty()) {
                bool vtt = false;
                for (int an : d.addrNets)
                    for (const auto& [res, other] : attached(sch, an, ComponentKind::Resistor))
                        vtt = vtt || containsAny(up(netName(sch, other)), {"VTT"});
                if (!vtt)
                    add(Severity::Warning, "REL_DDR_VTT",
                        m.ref + "'s address / command lines are not terminated: end the fly-by bus with 36–40 Ω to a VTT "
                        "rail (VDDQ / 2 from a sink / source regulator such as TPS51200), placed after the last DRAM.",
                        {m.id});
            }
            // Differential clock with termination.
            if (d.clock >= 0 && d.clockN >= 0) {
                bool term = false;
                for (const Component* r : bridges(sch, d.clock, d.clockN, {ComponentKind::Resistor})) {
                    const double v = valueOf(*r);
                    term = term || (v >= 70 && v <= 140);
                }
                for (const auto& [res, other] : attached(sch, d.clock, ComponentKind::Resistor))
                    term = term || containsAny(up(netName(sch, other)), {"VTT"});
                if (!term)
                    add(Severity::Warning, "REL_DDR_CK_TERM",
                        m.ref + "'s CK / CK# pair has no termination: ~100 Ω across the pair (or 2 × 36 Ω to VTT) at the "
                        "end of the clock's fly-by route.",
                        {m.id});
            } else if (d.clock >= 0 || d.clockN >= 0) {
                add(Severity::Warning, "REL_DDR_CK_TERM",
                    m.ref + "'s differential clock is only half connected: route CK and CK# as a 100 Ω pair.", {m.id});
            }
            // Strobes as named differential pairs (so routing and length tuning treat them as pairs).
            const auto pairs = differentialPairs(sch);
            for (const auto& strobe : d.strobes) {
                const int pos = strobe.first, neg = strobe.second;
                if (pos < 0 || neg < 0) continue;
                const bool paired = std::any_of(pairs.begin(), pairs.end(), [&](const std::pair<int, int>& p) {
                    return (p.first == pos && p.second == neg) || (p.first == neg && p.second == pos);
                });
                if (!paired) {
                    add(Severity::Info, "REL_DQS_PAIR",
                        m.ref + "'s data strobes are not named as differential pairs (e.g. DQS0_P / DQS0_N): name them so "
                        "Auto Route keeps each pair together and matches its skew.",
                        {m.id});
                    break;
                }
            }
        } else if (d.clock >= 0) {
            // SDR: a 22–33 Ω series resistor at the controller damps clock ringing.
            const bool series = !attached(sch, d.clock, ComponentKind::Resistor).empty();
            if (!series)
                add(Severity::Info, "REL_DRAM_CLOCK_SERIES",
                    m.ref + "'s clock has no series resistor: 22–33 Ω next to the controller's SDCLK pin damps ringing "
                    "on the clock edge.",
                    {m.id});
        }

        // ------------------------------------------------------------ 3. data integrity (length matching)
        if (!d.dqNets.empty()) {
            std::set<int> grouped;
            if (pcb.settings.lengthTuning)
                for (const auto& g : lengthGroups(sch, pcb.settings))
                    if (g.kind == "bus") grouped.insert(g.nets.begin(), g.nets.end());
            const bool matched = std::all_of(d.dqNets.begin(), d.dqNets.end(), [&](int n) { return grouped.count(n) > 0; });
            if (!matched)
                add(d.ddr ? Severity::Warning : Severity::Info, "REL_DRAM_LENGTH_MATCH",
                    m.ref + "'s data lines are not a length-matched bus: name them as a bus (DQ0…DQ15 or SD_DQ0…) so "
                    "Auto Route tunes each byte lane to within " + fmt("%.2f mm", pcb.settings.busLengthTolerance) +
                    (d.ddr ? " and to its strobe." : "."),
                    {m.id});
        }

        // ------------------------------------------------------------ 4. configuration (controller)
        if (!d.controller)
            add(Severity::Warning, "REL_DRAM_CONTROLLER",
                m.ref + "'s data lines do not reach a memory controller: connect DQ0… to the MCU / SoC / FPGA memory "
                "interface (FMC, SEMC, EMC, a DDR PHY or MIG).",
                {m.id});
        if (!d.ddr && d.cke >= 0 && !netConnected(sch, d.cke))
            add(Severity::Warning, "REL_DRAM_CKE",
                m.ref + "'s CKE is not connected: the controller drives it to wake the SDRAM after power-up.", {m.id});

        // ------------------------------------------------------------ 5. layout
        if (d.controller && m.pcb.placed && d.controller->pcb.placed && m.hasFootprint() && d.controller->hasFootprint()) {
            const double gap = rectGap(pcb.courtyard(m), pcb.courtyard(*d.controller));
            const double limit = d.ddr ? 15.0 : 25.0;
            if (gap > limit)
                add(Severity::Warning, "REL_DRAM_DISTANCE",
                    m.ref + " is " + fmt("%.1f mm", gap) + " from its controller " + d.controller->ref + ": keep the DRAM within " +
                        fmt("%.0f mm", limit) + " so the bus fits the controller's timing budget and stays short to tune.",
                    {m.id, d.controller->id});
        }
        if (strict) ddrLayoutChecks(pcb, d, effectiveLimits(project), add);
    }

    // ---------------------------------------------------------------- modules: SPD, PMIC, RCD, mechanics
    if (moduleType(type)) {
        if (a.eeproms.empty()) {
            add(Severity::Warning, "REL_SPD",
                "No SPD EEPROM: a memory module carries its serial presence detect (EE1004 / SPD5118 hub on DDR4 / DDR5, a "
                "24C02-class EEPROM on DDR3) so the host reads its size and timings over I²C / I3C.");
        } else {
            for (const Component* e : a.eeproms) {
                bool pulled = true;
                for (int i = 0; i < static_cast<int>(e->def().pins.size()); ++i) {
                    if (!pinIs(*e, i, {"SDA", "SCL"})) continue;
                    const int n = sch.netOf({e->id, i});
                    bool up = false;
                    for (const auto& [res, other] : attached(sch, n, ComponentKind::Resistor))
                        up = up || (!isGround(sch, other) && other >= 0 && sch.netRole(other) == NetRole::Power);
                    pulled = pulled && up;
                }
                if (!pulled) {
                    add(Severity::Warning, "REL_SPD_PULLUPS",
                        e->ref + "'s SDA / SCL have no pull-ups: 2.2–4.7 kΩ to the SPD supply (on the host for a "
                        "plug-in module).",
                        {e->id});
                    break;
                }
            }
        }
        add(Severity::Info, "REL_DDR5_PMIC",
            "DDR5 modules regulate their own rails: a JESD301 PMIC on the module turns the 5 V (UDIMM / SO-DIMM) or 12 V "
            "(RDIMM) input into VDD, VDDQ and VPP. PMICs are not in the standard library yet: add yours as a custom part "
            "from its datasheet (Component Library → New Part).");
        if (type == "rdimm")
            add(Severity::Info, "REL_RCD",
                "An RDIMM buffers command / address and clocks through a registering clock driver (RCD, JESD82-511) in "
                "the centre of the module; LRDIMMs add data buffers per byte lane.");
        const double t = pcb.settings.thickness;
        if (t < 1.1 || t > 1.4)
            add(Severity::Warning, "REL_DIMM_THICKNESS",
                "Board thickness " + fmt("%.2f mm", t) + " does not fit a DIMM socket: DDR4 / DDR5 DIMMs and SO-DIMMs are "
                "1.2–1.27 mm (JEDEC MO-309 / MO-310) with hard-gold (≥ 0.76 µm) bevelled edge fingers.");
    }

    // ---------------------------------------------------------------- board build
    if (!a.drams.empty()) {
        if (pcb.settings.layerCount < 4)
            add(Severity::Warning, "REL_DRAM_LAYERS",
                "A " + std::to_string(pcb.settings.layerCount) + "-layer board under a DRAM bus: use ≥ 4 layers so every "
                "data, address and clock line runs over a solid ground plane (return path, crosstalk, impedance).");
        else {
            bool plane = false;
            for (const auto& z : pcb.zones) plane = plane || containsAny(up(z.net), {"GND", "VSS"});
            if (!plane)
                add(Severity::Info, "REL_DRAM_REFERENCE_PLANE",
                    "No ground pour or plane: give the memory bus an unbroken GND reference on the layer next to it.");
        }
        if (a.ddr) {
            const double z = pcb.settings.singleEndedImpedance;
            if (z < 34 || z > 50)
                add(Severity::Info, "REL_DRAM_IMPEDANCE",
                    "Single-ended impedance target is " + fmt("%.0f Ω", z) + ": DDR3 uses 40–50 Ω, DDR4 / DDR5 / LPDDR "
                    "≈ 40 Ω single-ended and 80 Ω differential for clocks and strobes (Board Setup → Stack-up & Impedance).");
        }
    }
    return out;
}
}  // namespace

const std::vector<MemoryDesignType>& memoryDesignTypes() {
    static const std::vector<MemoryDesignType> t = {
        {"sdram", "SDR SDRAM on an MCU", "16 / 32-bit SDR SDRAM on an MCU memory controller (STM32 FMC, NXP SEMC / EMC)",
         {"One 3.3 V rail with 100 nF per VDD / VDDQ pin pair, a 22–33 Ω series resistor on SDCLK, the data bus length-"
          "matched, the SDRAM within ~25 mm of the MCU on a 4-layer board with a ground plane."}},
        {"ddr", "DDR3L / DDR4 memory-down", "DDR3L / DDR4 DRAM soldered beside an SoC, FPGA or processor",
         {"Fly-by command / address with 36–40 Ω to VTT after the last DRAM, a 100 Ω-terminated CK pair, VREF = VDDQ / 2 "
          "filtered at each pin, 240 Ω 1 % on ZQ, RESET_n pulled low; byte lanes matched to their DQS pair; 40 Ω / 80 Ω "
          "impedance on ≥ 6 layers."}},
        {"lpddr", "LPDDR4 / LPDDR5 point-to-point", "Mobile DRAM next to an application processor or SoM",
         {"Point-to-point, unterminated (on-die termination only); 40 Ω single-ended / 80 Ω differential; DRAM within a "
          "few millimetres of the SoC, often on the opposite side or as PoP; byte-lane length matching."}},
        {"dimm", "DDR5 UDIMM / SO-DIMM module", "Unbuffered desktop / laptop memory modules",
         {"1.2–1.27 mm board with hard-gold bevelled fingers, SPD5118 hub (with temperature sensor) on I3C, a JESD301 PMIC "
          "from 5 V, two 32-bit sub-channels per module, 8–10 layers."}},
        {"rdimm", "DDR5 RDIMM / LRDIMM server module", "Registered server memory modules",
         {"An RCD buffering command / address and clocks in the centre, data buffers per byte lane on LRDIMMs, a 12 V PMIC, "
          "SPD hub and temperature sensors, 10–12 layers."}},
    };
    return t;
}

const MemoryDesignType* findMemoryDesignType(const std::string& id) {
    for (const auto& t : memoryDesignTypes())
        if (t.id == id) return &t;
    return nullptr;
}

bool isMemoryProject(const Project& project) { return !project.memoryDesign.empty() || project.industry == "memory"; }

std::vector<RuleViolation> memoryChecks(const Project& project) {
    if (!isMemoryProject(project)) return {};
    return checks(project, !project.memoryDesign.empty(), analyse(project));
}

std::vector<RobotSegment> memorySegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const std::string& type = project.memoryDesign;
    const Analysis a = analyse(project);
    std::set<std::string> warn;
    for (const auto& v : checks(project, true, a))
        if (v.severity != Severity::Info) warn.insert(v.code);
    // A few Info-level findings still leave an item open.
    std::set<std::string> info;
    for (const auto& v : checks(project, true, a))
        if (v.severity == Severity::Info) info.insert(v.code);
    auto refs = [](const std::vector<const Component*>& v) {
        std::string r;
        for (size_t i = 0; i < v.size() && i < 6; ++i) r += (i ? ", " : "") + v[i]->ref;
        if (v.size() > 6) r += ", …";
        return r;
    };
    auto item = [](const std::string& label, bool ok, const std::string& found, const std::string& todo) {
        return RobotCheckItem{label, ok, ok ? found : todo};
    };
    std::vector<const Component*> drams;
    for (const auto& d : a.drams) drams.push_back(d.part);
    const bool any = !drams.empty();
    const bool module = moduleType(type);
    bool clockOk = any, addrOk = any, controllerOk = any, ckeOk = any;
    for (const auto& d : a.drams) {
        clockOk = clockOk && d.clock >= 0 && netConnected(sch, d.clock);
        for (int n : d.addrNets) addrOk = addrOk && netConnected(sch, n);
        controllerOk = controllerOk && d.controller;
        ckeOk = ckeOk && (d.ddr || (d.cke >= 0 && netConnected(sch, d.cke)));
    }
    std::vector<const Component*> controllers;
    for (const auto& d : a.drams)
        if (d.controller && std::find(controllers.begin(), controllers.end(), d.controller) == controllers.end())
            controllers.push_back(d.controller);
    std::vector<RobotSegment> out;

    RobotSegment pwr{"power", "Memory Power & Decoupling", "", {}, {}};
    pwr.items.push_back(item("Decoupling at every supply pin pair", any && !warn.count("REL_DRAM_DECOUPLING"),
                             "100 nF per VDD / VDDQ pin pair on " + refs(drams), "100 nF per VDD / VDDQ pin pair"));
    pwr.items.push_back(item("Bulk capacitance", any && !info.count("REL_DRAM_BULK"), "≥ 4.7 µF on the DRAM supply",
                             "10–22 µF next to the DRAM"));
    pwr.items.push_back(item("VTT / VREF termination supply",
                             a.ddr ? any && !warn.count("REL_DDR_VTT") && !warn.count("REL_DDR_VREF") : any,
                             a.ddr ? "VTT rail and filtered VREF = VDDQ / 2" : "SDR SDRAM: one 3.3 V rail, no VTT / VREF",
                             a.ddr ? "VTT regulator (TPS51200 class) and a VREF divider" : "DRAM on its supply"));
    pwr.guidance = {"JEDEC power-up: VDD and VDDQ together (VPP first on DDR4), RESET_n low until they are stable; decouple "
                    "every pin pair, add bulk at the DRAM and, on DDR, a sink / source VTT regulator tracking VDDQ / 2."};
    out.push_back(pwr);

    RobotSegment clk{"clock", "Clock, Command & Address", "", {}, {}};
    clk.items.push_back(item("Clock routed and terminated", clockOk && !warn.count("REL_DDR_CK_TERM") &&
                                                                (a.ddr || !info.count("REL_DRAM_CLOCK_SERIES")),
                             a.ddr ? "CK / CK# pair with 100 Ω termination" : "SDCLK with a series resistor at the controller",
                             a.ddr ? "100 Ω-terminated CK / CK# pair" : "22–33 Ω series resistor on SDCLK"));
    clk.items.push_back(item("Address, bank and command lines", addrOk, "Every A / BA line reaches the controller",
                             "Connect every address and bank line"));
    clk.items.push_back(item("Fly-by topology / termination", a.ddr ? any && !warn.count("REL_DDR_VTT") : any,
                             a.ddr ? "Fly-by bus terminated to VTT" : "Point-to-point SDR bus, no VTT needed",
                             a.ddr ? "36–40 Ω to VTT after the last DRAM" : "DRAM connected to its controller"));
    clk.guidance = {"SDR: a series resistor on SDCLK, short point-to-point lines. DDR3 / DDR4: fly-by command / address and "
                    "clock from DRAM to DRAM, terminated to VTT at the far end; write leveling absorbs the fly-by skew."};
    out.push_back(clk);

    RobotSegment data{"data", "Data Integrity", "", {}, {}};
    data.items.push_back(item("Byte lanes length-matched", any && !warn.count("REL_DRAM_LENGTH_MATCH") &&
                                                               !info.count("REL_DRAM_LENGTH_MATCH"),
                              "Data bus tuned as a matched group", "Name the data nets as a bus (DQ0… / SD_DQ0…)"));
    data.items.push_back(item("Strobe pairs (DQS)", a.ddr ? any && !info.count("REL_DQS_PAIR") : any,
                              a.ddr ? "DQS pairs named and routed as pairs" : "SDR: no strobes",
                              "Name DQS as differential pairs"));
    data.items.push_back(item("ZQ calibration resistor", a.ddr ? any && !warn.count("REL_DDR_ZQ") : any,
                              a.ddr ? "240 Ω 1 % on every ZQ pin" : "SDR: no ZQ calibration", "240 Ω 1 % from ZQ to ground"));
    data.guidance = {"Match DQ / DM to their strobe within each byte lane (DDR) or as one bus (SDR), keep lanes on one layer "
                     "pair over ground, 40 Ω single-ended for DDR4 / DDR5 / LPDDR."};
    out.push_back(data);

    RobotSegment cfg{"config", "Configuration & Management", "", {}, {}};
    cfg.items.push_back(item("Memory controller", controllerOk, refs(controllers) + " drives the bus",
                             "Connect the DRAM to an MCU / SoC / FPGA memory controller"));
    cfg.items.push_back(item("Power-up control (RESET_n / CKE)", ckeOk && !warn.count("REL_DDR_RESET") && !warn.count("REL_DRAM_CKE"),
                             a.ddr ? "RESET_n pulled low until the rails are up" : "CKE driven by the controller",
                             a.ddr ? "10 kΩ pull-down on RESET_n" : "Connect CKE to the controller"));
    cfg.items.push_back(item("SPD EEPROM", module ? !warn.count("REL_SPD") && !warn.count("REL_SPD_PULLUPS") : any,
                             module ? refs(a.eeproms) + " with SDA / SCL pull-ups" : "Memory-down: the controller holds the timings",
                             "SPD EEPROM / hub with I²C pull-ups"));
    cfg.guidance = {"Memory-down: the controller (FMC, DDR PHY, MIG) is configured with the DRAM's timings in firmware. Modules "
                    "carry an SPD EEPROM / hub that the host reads at boot; DDR5 adds a PMIC and RDIMMs an RCD."};
    out.push_back(cfg);

    RobotSegment lay{"layout", "Layout & Fabrication", "", {}, {}};
    bool placed = any;
    for (const auto& d : a.drams) placed = placed && d.part->pcb.placed;
    lay.items.push_back(item("DRAM next to its controller", placed && !warn.count("REL_DRAM_DISTANCE"),
                             "Within the bus length budget", "Place the DRAM close to the controller"));
    lay.items.push_back(item("Ground reference plane", any && !warn.count("REL_DRAM_LAYERS") && !info.count("REL_DRAM_REFERENCE_PLANE"),
                             "≥ 4 layers with a GND plane under the bus", "≥ 4 layers and a GND pour / plane"));
    lay.items.push_back(item("Impedance & board build", any && !info.count("REL_DRAM_IMPEDANCE") && !warn.count("REL_DIMM_THICKNESS"),
                             module ? "1.2–1.27 mm module board, impedance-controlled" : "Impedance-controlled stack-up",
                             module ? "1.2–1.27 mm board, 40 Ω / 80 Ω stack-up" : "40–50 Ω single-ended target"));
    lay.guidance = {"Keep the DRAM close to the controller, route the bus over an unbroken ground plane, set the stack-up's "
                    "impedance targets, and use HDI / via-in-pad for 0.8 mm BGA DRAM fan-out."};
    out.push_back(lay);

    scoreSegments(out);
    return out;
}

Json memoryLimitsJson(const Project::MemoryLayoutLimits& l) {
    Json j = Json::object();
    if (l.laneSkewPs > 0) j["laneSkewPs"] = l.laneSkewPs;
    if (l.dqsSkewPs > 0) j["dqsSkewPs"] = l.dqsSkewPs;
    if (l.addrSkewPs > 0) j["addrSkewPs"] = l.addrSkewPs;
    if (l.impedanceTolPercent > 0) j["impedanceTolPercent"] = l.impedanceTolPercent;
    if (l.laneViaSpread >= 0) j["laneViaSpread"] = l.laneViaSpread;
    if (l.pairSkewPs > 0) j["pairSkewPs"] = l.pairSkewPs;
    return j;
}

Project::MemoryLayoutLimits memoryLimitsFromJson(const Json& j) {
    Project::MemoryLayoutLimits l;
    if (!j.isObject()) return l;
    l.laneSkewPs = std::clamp(j.get("laneSkewPs").asNumber(0), 0.0, 1000.0);
    l.dqsSkewPs = std::clamp(j.get("dqsSkewPs").asNumber(0), 0.0, 1000.0);
    l.addrSkewPs = std::clamp(j.get("addrSkewPs").asNumber(0), 0.0, 1000.0);
    l.impedanceTolPercent = std::clamp(j.get("impedanceTolPercent").asNumber(0), 0.0, 50.0);
    l.laneViaSpread = std::clamp(j.get("laneViaSpread").asInt(-1), -1, 8);
    l.pairSkewPs = std::clamp(j.get("pairSkewPs").asNumber(0), 0.0, 1000.0);
    return l;
}

Json memoryLimitsReportJson(const Project& project) {
    Json j = Json::object();
    j["type"] = project.memoryDesign;
    j["effective"] = limitsJson(effectiveLimits(project));
    j["defaults"] = limitsJson(ddrLayoutLimits(project.memoryDesign));
    j["overrides"] = memoryLimitsJson(project.memoryLimits);
    return j;
}

Json memorySegmentsJson(const Project& project) {
    std::vector<PlatformInfo> types;
    for (const auto& t : memoryDesignTypes()) types.push_back({t.id, t.name, t.description, t.guidance});
    return segmentReportJson(project.memoryDesign, isMemoryProject(project), types, memorySegments(project));
}

}  // namespace sieda
