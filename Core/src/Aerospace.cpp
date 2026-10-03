#include "sieda/Aerospace.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "SystemParts.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Project.hpp"
#include "sieda/Stackup.hpp"

namespace sieda {

namespace {
using namespace sysparts;

/// Nets a part's pins sit on.
std::set<int> netsOf(const Schematic& sch, const Component& c) {
    std::set<int> out;
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (int n = sch.netOf({c.id, i}); n >= 0) out.insert(n);
    return out;
}

bool touches(const Schematic& sch, int net, const std::vector<const Component*>& parts) {
    for (const Component* c : parts)
        if (netsOf(sch, *c).count(net)) return true;
    return false;
}

bool shareNet(const Schematic& sch, const Component& a, const Component& b) {
    auto na = netsOf(sch, a);
    for (int n : netsOf(sch, b))
        if (n != sch.groundNet() && na.count(n)) return true;
    return false;
}

/// The net on a part's first pin named one of `names`, or -1.
int pinNet(const Schematic& sch, const Component& c, std::initializer_list<const char*> names) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
        const std::string pn = pinName(c, i);
        for (const char* n : names)
            if (pn == n) return sch.netOf({c.id, i});
    }
    return -1;
}

/// Processors grouped by part: three or more identical processors form a TMR set.
size_t largestIdenticalGroup(const Parts& parts) {
    std::map<std::string, size_t> count;
    size_t best = 0;
    for (const Component* c : parts.compute) best = std::max(best, ++count[partName(*c)]);
    return best;
}

bool severeRadiation(const std::string& mission) { return mission == "leo" || mission == "geo"; }
}  // namespace

const std::vector<AerospaceMission>& aerospaceMissions() {
    static const std::vector<AerospaceMission> m = {
        {"leo", "LEO satellite / CubeSat", "Low Earth orbit: trapped protons, SAA passes, 10–30 krad TID over the mission",
         {"Rad-tolerant processor (SAMRH71, ATmegaS128, Vorago VA416xx) with EDAC MRAM, latch-up current limiters that power-"
          "cycle the processor, SpaceWire to the payload, UHF / S-band telemetry on a low-loss laminate.",
          "IPC-6012DS (space addendum), polyimide or Rogers laminate, SnPb solder, no pure tin, staked and coated parts."}},
        {"geo", "GEO / deep-space spacecraft", "Van Allen belts and cosmic rays: 100 krad+ TID, SEL / SEU immunity mandatory",
         {"Radiation-hardened (SOI) processors such as RAD750 / GR740 / RTG4 in triple modular redundancy with majority "
          "voters, MRAM, cold-redundant power and SpaceWire / SpaceFibre networks.",
          "Derate per ECSS-Q-ST-30-11C; every part with a radiation lot acceptance test."}},
        {"launcher", "Launch vehicle avionics", "Short mission, extreme vibration and shock (20 g RMS), pyrotechnic events",
         {"Dual-redundant MIL-STD-1553 buses, hardened power with SSPCs, thermocouple and pressure inputs from the engines.",
          "Stake and underfill every heavy part; no MLCCs > 1210 at the board edge."}},
        {"military", "Military aircraft avionics", "MIL-STD-704 28 V bus, MIL-STD-461 EMI, MIL-STD-1553 mission bus",
         {"Transformer-coupled dual-redundant MIL-STD-1553 (bus A / B), isolated DC-DC from the 28 V bus with MIL-STD-704 "
          "surge and spike protection, SSPCs instead of breakers.",
          "MIL-STD-810 environment; conformal coat; atmospheric neutron SEUs at altitude still need EDAC."}},
        {"commercial", "Commercial avionics (DO-160 / DO-254)", "ARINC 429 and AFDX networks, DO-160 lightning, DAL A–E",
         {"ARINC 429 line drivers / receivers with lightning protection (DO-160 Section 22), 28 V bus front end with DO-160 "
          "Section 16 transients, isolated converters.",
          "DO-254 design assurance for programmable logic; neutron SEU mitigation at cruise altitude."}},
    };
    return m;
}

const AerospaceMission* findAerospaceMission(const std::string& id) {
    for (const auto& m : aerospaceMissions())
        if (m.id == id) return &m;
    return nullptr;
}

bool isAerospaceProject(const Project& project) {
    return !project.aerospaceMission.empty() || project.industry == "space";
}

namespace {
/// strict: full severities (a mission is set, or the segment checklist); otherwise everything is reported as Info,
/// since the space industry profile alone does not say where the board flies.
std::vector<RuleViolation> checks(const Project& project, bool strict) {
    std::vector<RuleViolation> out;
    if (!isAerospaceProject(project)) return out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const Parts parts = classify(sch);
    const std::string mission = project.aerospaceMission.empty() ? "leo" : project.aerospaceMission;
    const bool space = mission == "leo" || mission == "geo";
    const bool other = isRobotProject(project) || isEcuProject(project);  // shared rules already reported
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = strict ? sev : Severity::Info;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };
    const int gnd = sch.groundNet();

    // ---------------------------------------------------------------- 1. rad-hard & redundant compute
    if (!parts.compute.empty()) {
        std::vector<const Component*> soft;
        for (const Component* c : parts.compute)
            if (std::find(parts.radHard.begin(), parts.radHard.end(), c) == parts.radHard.end()) soft.push_back(c);
        if (!soft.empty())
            add(severeRadiation(mission) ? Severity::Warning : Severity::Info, "REL_RAD_HARD",
                soft.front()->ref + " is a commercial processor: cosmic rays flip its SRAM bits (SEU) and can latch it up "
                "(SEL). Use a radiation-hardened / tolerant part (Microchip SAMRH71 / ATmegaS128, Cobham GR712 / GR740, BAE "
                "RAD750, Microchip RTG4, AMD XQR Versal).",
                {soft.front()->id});
        const size_t lanes = largestIdenticalGroup(parts);
        if (lanes >= 3 && parts.voters.empty())
            add(Severity::Warning, "REL_TMR_VOTER",
                "Three identical processors form a TMR set, but nothing votes on their outputs: add majority voters "
                "(74HC00 + 74HC10, 74LVC1G57, or voting logic in a rad-hard FPGA) so one upset lane is outvoted.");
        else if (lanes < 3 && mission == "geo")
            add(Severity::Warning, "REL_TMR_VOTER",
                "A single processor lane: GEO and deep-space control functions run three identical lanes (triple modular "
                "redundancy) with hardware majority voters so a single-event upset never reaches an output.");
        if (parts.mram.empty())
            add(space ? Severity::Warning : Severity::Info, "REL_MRAM",
                std::string(parts.memories.empty() ? "No non-volatile memory" : "EEPROM / flash only") +
                    ": charge-trap memories lose bits to total dose and heavy ions. Store software, logs and state in "
                    "radiation-immune MRAM (Everspin MR25H40 / MR4A16, CAES UT8MR) with EDAC.");
        // Single-event latch-up: each processor's supply through a current-limited switch that a watchdog can cycle.
        std::vector<const Component*> unprotected, notCycled;
        for (const Component* c : parts.compute) {
            const int vcc = pinNet(sch, *c, {"VCC", "VDD", "VDDIO", "VDDCORE", "3V3"});
            const Component* sw = nullptr;
            for (const Component* e : parts.efuses)
                if (vcc >= 0 && netsOf(sch, *e).count(vcc)) sw = e;
            if (!sw) {
                unprotected.push_back(c);
                continue;
            }
            const int en = pinNet(sch, *sw, {"EN", "ON", "EN/UV", "SHDN", "CTRL"});
            if (en < 0 || !touches(sch, en, parts.watchdogs)) notCycled.push_back(c);
        }
        if (!unprotected.empty())
            add(space ? Severity::Warning : Severity::Info, "REL_SEL_PROTECTION",
                unprotected.front()->ref + " is powered straight from the rail: a single-event latch-up draws amps through "
                "the die until it burns. Feed each processor through a current-limited switch (TPS2553, latching current "
                "limiter) that trips within microseconds.",
                {unprotected.front()->id});
        if (!notCycled.empty())
            add(Severity::Warning, "REL_SEL_POWER_CYCLE",
                notCycled.front()->ref + ": its current-limited supply switch is not controlled by an independent watchdog, so "
                "a latched-up or hung processor is never power-cycled. Drive the switch enable from the watchdog's RESET.",
                {notCycled.front()->id});
        if (parts.watchdogs.empty())
            add(Severity::Warning, "REL_SEL_WATCHDOG",
                "No independent hardware watchdog: a processor hung by a single-event functional interrupt stays dead. Add "
                "a simple supervisor (TPS3823) the processor must kick, wired to cut and restore its power.");
    }

    // ---------------------------------------------------------------- 2. power conditioning & isolation
    if (!parts.sources.empty()) {
        // Isolated: a converter separates the bus, and no source returns straight into the logic ground.
        bool isolated = !parts.isoDcdc.empty();
        for (const Component* src : parts.sources)
            if (sch.netOf({src->id, 1}) == gnd) isolated = false;
        if (!isolated)
            add(Severity::Warning, "REL_ISOLATED_POWER",
                "The vehicle bus shares ground with the logic: fault currents, lightning and ground shifts on the 28 V bus "
                "or solar array reach the processor. Use a galvanically isolated DC-DC converter (Crane Interpoint MHF+, VPT "
                "SVR, IR HiRel) and keep the bus return (RTN) separate from the logic ground.");
        if (parts.tvs.empty())
            add(Severity::Warning, "REL_LIGHTNING_TVS",
                "No transient suppressor on the power input: MIL-STD-704 spikes, DO-160 Section 22 lightning and solar-array "
                "arcing go straight into the converter. Add a MIL-PRF-19500 / lightning-rated TVS (MPLAD15KP, JANTX 1N6xxx).");
        else {
            bool qualified = false;
            for (const Component* d : parts.tvs) qualified |= containsAny(up(d->value), {"JAN", "MPLAD", "MIL"});
            if (!qualified)
                add(Severity::Info, "REL_LIGHTNING_TVS",
                    "The input TVS is a commercial part: use a MIL-PRF-19500 (JANTX / JANTXV) or DO-160 lightning-rated "
                    "device (MPLAD15KP series) screened for the mission.");
        }
        if (!other && parts.fuses.empty() && parts.efuses.empty())
            add(Severity::Warning, "REL_SSPC",
                "No protection on the bus input: use a solid-state power controller / eFuse that trips in microseconds and "
                "reports its state (replaces the mechanical breaker), or at least a fuse.");
        else if (parts.efuses.empty())
            add(Severity::Info, "REL_SSPC",
                "Input protection is a plain fuse: aerospace boards use solid-state power controllers / eFuses with "
                "programmable trip curves, status telemetry and remote reset.");
    }

    // ---------------------------------------------------------------- 3. flight sensor interface
    {
        auto tc = netsNamed(sch, {"TC+", "TC-", "TC_P", "TC_N", "THERMOCOUPLE", "TCPL"});
        if (!tc.empty() && parts.cjc.empty())
            add(Severity::Warning, "REL_THERMOCOUPLE_CJC",
                "Thermocouple input without cold-junction compensation: the reading drifts with the board temperature. Use "
                "a converter with an on-chip cold junction (MAX31855 / MAX31856, AD8495) placed next to the connector.");
        if (!parts.adcs.empty()) {
            bool isolatedAdc = false;
            for (const Component* a : parts.adcs)
                for (const Component* i : parts.isolators) isolatedAdc |= shareNet(sch, *a, *i);
            if (!isolatedAdc)
                add(Severity::Info, "REL_ISOLATED_ADC",
                    "The sensor ADC shares ground with the processor: ground shifts along a long flight harness appear as "
                    "measurement error. Put the ADC on an isolated supply and pass its data through a digital isolator.");
        }
        if (!other) {
            std::vector<const Component*> inputs = parts.compute;
            inputs.insert(inputs.end(), parts.amplifiers.begin(), parts.amplifiers.end());
            inputs.insert(inputs.end(), parts.adcs.begin(), parts.adcs.end());
            int reported = 0;
            for (const Component* j : parts.connectors)
                for (int i = 0; i < static_cast<int>(j->def().pins.size()) && reported < 5; ++i) {
                    const int net = sch.netOf({j->id, i});
                    if (net < 0 || sch.netRole(net) != NetRole::Signal || !touches(sch, net, inputs)) continue;
                    bool filtered = false;
                    for (const auto& c : sch.components())
                        for (int k = 0; k < static_cast<int>(c.def().pins.size()); ++k)
                            if (sch.netOf({c.id, k}) == net && (c.kind == ComponentKind::Resistor ||
                                                                c.kind == ComponentKind::Capacitor ||
                                                                c.kind == ComponentKind::Diode || c.kind == ComponentKind::Inductor))
                                filtered = true;
                    if (!filtered) {
                        add(Severity::Warning, "REL_HARNESS_INPUT",
                            j->ref + " pin " + std::to_string(i + 1) + " (" + netName(sch, net) + ") runs from the flight "
                            "harness straight into an IC: add series resistance, an RC filter and transient protection "
                            "(DO-160 Section 22 pin injection, MIL-STD-461 CS114).",
                            {j->id});
                        ++reported;
                    }
                }
        }
    }

    // ---------------------------------------------------------------- 4. avionics communications
    if (!parts.mil1553.empty()) {
        if (parts.transformers.empty())
            add(Severity::Warning, "REL_1553_COUPLING",
                "MIL-STD-1553 transceiver without coupling transformers: the bus must be transformer-coupled (1:1.41 "
                "direct or 1:2 stub coupler) with 0.75·Z0 fault-isolation resistors so a shorted terminal cannot take the "
                "bus down.");
        const bool a = !netsNamed(sch, {"BUS_A", "1553A", "1553_A", "BUSA"}).empty();
        const bool b = !netsNamed(sch, {"BUS_B", "1553B", "1553_B", "BUSB"}).empty();
        if (!(a && b))
            add(Severity::Warning, "REL_1553_REDUNDANCY",
                "MIL-STD-1553 is dual-redundant: wire both bus A and bus B (nets BUS_A_P / _N and BUS_B_P / _N) to "
                "separate transformers and connectors.");
    }
    {
        bool unprotected = false;
        for (int net : netsNamed(sch, {"A429", "ARINC", "BUS_A", "BUS_B", "1553"}))
            unprotected |= !hasEsdDiode(sch, net) && !touches(sch, net, parts.transformers);
        if (unprotected)
            add(Severity::Warning, "REL_AVIONICS_BUS_PROTECTION",
                "An avionics bus line leaves the box without lightning / transient protection: add TVS diodes on ARINC 429 "
                "lines (DO-160 Section 22) and transformer coupling on MIL-STD-1553.");
    }
    for (const Component* r : parts.spacewire) {
        // LVDS receivers: each RIN+ / RIN− pair terminated with 100 Ω at the receiver.
        const auto& pins = r->def().pins;
        for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
            const std::string pn = pinName(*r, i);
            if (pn.rfind("RIN", 0) != 0 || pn.back() != '+') continue;
            const std::string neg = pn.substr(0, pn.size() - 1) + "-";
            const int p = sch.netOf({r->id, i});
            int n = -1;
            for (int k = 0; k < static_cast<int>(pins.size()); ++k)
                if (pinName(*r, k) == neg) n = sch.netOf({r->id, k});
            if (p >= 0 && n >= 0 && sch.nets()[static_cast<size_t>(p)].pins.size() > 1 && !hasTermination(sch, p, n)) {
                bool hundred = false;
                for (const Component* t : bridges(sch, p, n, {ComponentKind::Resistor})) {
                    auto v = parseEngineeringValue(primaryValue(t->value));
                    hundred |= v && *v >= 90 && *v <= 110;
                }
                if (!hundred) {
                    add(Severity::Warning, "REL_LVDS_TERMINATION",
                        r->ref + " " + pn + " / " + neg + ": SpaceWire / LVDS receivers need a 100 Ω termination across the "
                        "pair, right at the receiver pins.",
                        {r->id});
                    break;
                }
            }
        }
    }

    // ---------------------------------------------------------------- 5. RF & telemetry
    const bool rf = !parts.coax.empty() || !parts.rf.empty();
    if (rf) {
        const auto& lam = boardLaminate(s);
        if (lam.lossTangent > 0.005)
            add(Severity::Warning, "REL_RF_LAMINATE",
                lam.name + " (tan δ " + fmt("%.3f", lam.lossTangent) + ") is too lossy and moisture-sensitive for space "
                "telemetry: use a low-loss, low-outgassing laminate (Rogers RO4350B / RT/duroid, PTFE) for the RF section "
                "(Board Setup → Stack-up).");
        if (std::fabs(s.singleEndedImpedance - 50.0) > 5.0)
            add(Severity::Warning, "REL_RF_IMPEDANCE",
                "RF feeds need 50 Ω controlled impedance; the board's single-ended target is " +
                    fmt("%.0f Ω", s.singleEndedImpedance) + " (Board Setup → Stack-up).");
        bool plane = false;
        for (const auto& z : pcb.zones) plane |= gnd >= 0 && z.net == netName(sch, gnd);
        if (!plane)
            add(Severity::Warning, "REL_RF_GROUND",
                "No ground pour: RF lines need a solid reference plane under them, and the shield-can frame and via fence "
                "need ground to land on (add a GND plane).");
        if (!parts.rf.empty())
            add(Severity::Info, "REL_RF_SHIELD_CAN",
                "Fit a machined aluminium or mu-metal shield can over " + parts.rf.front()->ref + " and its matching "
                "network, soldered to a via-fenced ground ring, to keep cosmic and local EMI out of the receiver.");
        const bool laidOut = !pcb.pads(sch).empty();
        if (laidOut && !isRobotProject(project))
            for (const Component* c : parts.coax)
                if (c->hasFootprint() && c->pcb.placed && coaxEdgeGap(pcb, *c) > 3.0)
                    add(Severity::Warning, "REL_COAX_EDGE",
                        c->ref + " (" + c->value + ") is " + fmt("%.1f mm", coaxEdgeGap(pcb, *c)) + " from the "
                        "board edge: put coax launchers at the edge with a short 50 Ω feed.",
                        {c->id});
    }
    return out;
}
}  // namespace

std::vector<RuleViolation> aerospaceChecks(const Project& project) {
    return checks(project, !project.aerospaceMission.empty());
}

std::vector<RobotSegment> aerospaceSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const Parts parts = classify(sch);
    std::set<std::string> warn;
    for (const auto& v : checks(project, true))
        if (v.severity != Severity::Info) warn.insert(v.code);
    auto refs = [](const std::vector<const Component*>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 6; ++i) s += (i ? ", " : "") + v[i]->ref;
        if (v.size() > 6) s += ", …";
        return s;
    };
    auto item = [](const std::string& label, bool ok, const std::string& found, const std::string& todo) {
        return RobotCheckItem{label, ok, ok ? found : todo};
    };
    const std::string mission = project.aerospaceMission;
    std::vector<RobotSegment> out;

    RobotSegment cpu{"compute", "Rad-Hardened & Redundant Compute", "", {}, {}};
    std::vector<const Component*> hardCpu;
    for (const Component* c : parts.compute)
        if (std::find(parts.radHard.begin(), parts.radHard.end(), c) != parts.radHard.end()) hardCpu.push_back(c);
    cpu.items.push_back(item("Rad-hard processor", !parts.compute.empty() && hardCpu.size() == parts.compute.size(),
                             refs(hardCpu), "SOI rad-hard / rad-tolerant MCU, FPGA or SoC"));
    const size_t lanes = largestIdenticalGroup(parts);
    cpu.items.push_back(item("TMR voting", lanes >= 3 && !parts.voters.empty(),
                             std::to_string(lanes) + " lanes, voters " + refs(parts.voters),
                             "Three identical lanes with hardware majority voters"));
    cpu.items.push_back(item("MRAM", !parts.mram.empty(), refs(parts.mram), "Radiation-immune MRAM"));
    cpu.items.push_back(item("Latch-up power cycling", !parts.compute.empty() && !warn.count("REL_SEL_PROTECTION") &&
                                                           !warn.count("REL_SEL_POWER_CYCLE") && !warn.count("REL_SEL_WATCHDOG"),
                             refs(parts.watchdogs) + " cycle " + refs(parts.efuses),
                             "Watchdog-controlled current-limited supply per processor"));
    cpu.guidance = {"SEU: EDAC memories and TMR; SEL: current limiters that power-cycle the die; TID: rad-hard parts "
                    "with lot acceptance data."};
    out.push_back(cpu);

    RobotSegment pwr{"power", "Ruggedized Power Conditioning & Isolation", "", {}, {}};
    pwr.items.push_back(item("Vehicle bus input", !parts.sources.empty(), refs(parts.sources), "28 V bus / solar array input"));
    pwr.items.push_back(item("Isolated DC-DC", !parts.isoDcdc.empty() && !warn.count("REL_ISOLATED_POWER"), refs(parts.isoDcdc),
                             "Magnetically isolated DC-DC converter"));
    pwr.items.push_back(item("MIL / lightning TVS", !parts.tvs.empty() && !warn.count("REL_LIGHTNING_TVS"), refs(parts.tvs),
                             "MIL-PRF-19500 / DO-160 lightning TVS"));
    pwr.items.push_back(item("SSPC / eFuse", !parts.efuses.empty() || !parts.fuses.empty(),
                             refs(parts.efuses.empty() ? parts.fuses : parts.efuses), "Solid-state power controller"));
    pwr.guidance = {"MIL-STD-704 / DO-160 Section 16 bus transients, EMI filter, isolated converters, SSPCs with telemetry."};
    out.push_back(pwr);

    RobotSegment sen{"sensors", "Flight Telemetry & Rugged Sensor Interface", "", {}, {}};
    sen.items.push_back(item("Instrumentation amplifier", !parts.amplifiers.empty(), refs(parts.amplifiers),
                             "High-CMRR differential amplifier"));
    bool isolatedAdc = false;
    for (const Component* a : parts.adcs)
        for (const Component* i : parts.isolators) isolatedAdc |= shareNet(sch, *a, *i);
    sen.items.push_back(item("Isolated ADC", isolatedAdc, refs(parts.adcs) + " behind " + refs(parts.isolators),
                             "ADC on an isolated supply with a digital isolator"));
    const bool tc = !netsNamed(sch, {"TC+", "TC-", "TC_P", "TC_N", "THERMOCOUPLE", "TCPL"}).empty();
    sen.items.push_back(item("Thermocouple cold-junction compensation", !parts.cjc.empty() || !tc,
                             parts.cjc.empty() ? "No thermocouple inputs" : refs(parts.cjc), "MAX31855 / AD8495 at the connector"));
    sen.items.push_back(item("Protected harness inputs", !warn.count("REL_HARNESS_INPUT"), "Series R, RC and TVS",
                             "Filter and protect every harness pin"));
    sen.guidance = {"Gyros, star trackers, pitot / pressure and engine thermocouples over long harnesses: high-CMRR "
                    "amplifiers, isolated converters, cold-junction compensation."};
    out.push_back(sen);

    RobotSegment com{"avionics", "Space & Military Avionics Communications", "", {}, {}};
    const bool need1553 = mission == "military" || mission == "launcher";
    com.items.push_back(item("MIL-STD-1553 (dual, transformer-coupled)",
                             need1553 ? !parts.mil1553.empty() && !warn.count("REL_1553_COUPLING") && !warn.count("REL_1553_REDUNDANCY")
                                      : parts.mil1553.empty() || (!warn.count("REL_1553_COUPLING") && !warn.count("REL_1553_REDUNDANCY")),
                             parts.mil1553.empty() ? "Not needed on this mission" : refs(parts.mil1553),
                             "1553 terminal with coupling transformers on bus A and B"));
    const bool needSpw = mission == "leo" || mission == "geo";
    com.items.push_back(item("SpaceWire / LVDS", (!needSpw || !parts.spacewire.empty()) && !warn.count("REL_LVDS_TERMINATION"),
                             parts.spacewire.empty() ? "Not needed on this mission" : refs(parts.spacewire) + ", 100 Ω terminated",
                             "SpaceWire LVDS drivers / receivers with 100 Ω termination"));
    const bool needArinc = mission == "commercial";
    com.items.push_back(item("ARINC 429", (!needArinc || !parts.arinc.empty()) && !warn.count("REL_AVIONICS_BUS_PROTECTION"),
                             parts.arinc.empty() ? "Not needed on this mission" : refs(parts.arinc),
                             "ARINC 429 line driver / receiver with lightning TVS"));
    com.guidance = {"Deterministic, redundant buses: MIL-STD-1553 (1 Mbit/s, transformer-coupled), SpaceWire (ECSS-E-ST-50-12C, "
                    "LVDS data / strobe), ARINC 429 (±10 V differential)."};
    out.push_back(com);

    RobotSegment rf{"rf", "High-Frequency RF & Satellite Telemetry", "", {}, {}};
    const bool hasRf = !parts.coax.empty() || !parts.rf.empty();
    const bool needRf = mission == "leo" || mission == "geo";
    rf.items.push_back(item("50 Ω coax launch", hasRf ? !warn.count("REL_RF_IMPEDANCE") && !warn.count("REL_COAX_EDGE") : !needRf,
                            hasRf ? refs(parts.coax) + " at the edge, 50 Ω" : "No RF on this board", "SMA / SMP launcher with a 50 Ω feed"));
    rf.items.push_back(item("Low-loss laminate", hasRf ? !warn.count("REL_RF_LAMINATE") : !needRf,
                            hasRf ? boardLaminate(pcb.settings).name : "No RF on this board", "Rogers / PTFE laminate"));
    rf.items.push_back(item("Shield-can ground & via fence", hasRf ? !warn.count("REL_RF_GROUND") : !needRf,
                            hasRf ? "GND plane for the can and fence" : "No RF on this board", "Ground plane and shield can"));
    rf.guidance = {"Microstrip / stripline over Rogers RT/duroid or PTFE, machined shield cans soldered to via-fenced ground "
                   "rings over LNAs, PAs and mixers."};
    out.push_back(rf);

    for (auto& seg : out) {
        int ok = 0;
        for (const auto& i : seg.items) ok += i.ok;
        seg.status = ok == static_cast<int>(seg.items.size()) ? "complete" : ok == 0 ? "missing" : "partial";
    }
    return out;
}

Json aerospaceSegmentsJson(const Project& project) {
    Json root = Json::object();
    root["platform"] = project.aerospaceMission;
    root["applies"] = isAerospaceProject(project);
    Json types = Json::array();
    for (const auto& t : aerospaceMissions()) {
        Json j = Json::object();
        j["id"] = t.id;
        j["name"] = t.name;
        j["description"] = t.description;
        Json g = Json::array();
        for (const auto& x : t.guidance) g.push(x);
        j["guidance"] = g;
        types.push(j);
    }
    root["platforms"] = types;
    Json segs = Json::array();
    for (const auto& seg : aerospaceSegments(project)) {
        Json j = Json::object();
        j["id"] = seg.id;
        j["name"] = seg.name;
        j["status"] = seg.status;
        Json items = Json::array();
        for (const auto& i : seg.items) {
            Json x = Json::object();
            x["label"] = i.label;
            x["ok"] = i.ok;
            x["detail"] = i.detail;
            items.push(x);
        }
        j["items"] = items;
        Json g = Json::array();
        for (const auto& x : seg.guidance) g.push(x);
        j["guidance"] = g;
        segs.push(j);
    }
    root["segments"] = segs;
    return root;
}

}  // namespace sieda
