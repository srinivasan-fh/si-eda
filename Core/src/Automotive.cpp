#include "sieda/Automotive.hpp"

#include <algorithm>
#include <map>
#include <set>

#include "SystemParts.hpp"
#include "sieda/Project.hpp"
#include "sieda/Simulator.hpp"

namespace sieda {

namespace {
using namespace sysparts;

/// A pin of `c` named one of `names` (upper case), or -1.
int pinNamed(const Component& c, std::initializer_list<const char*> names) {
    for (size_t i = 0; i < c.def().pins.size(); ++i) {
        const std::string pn = pinName(c, static_cast<int>(i));
        for (const char* n : names)
            if (pn == n) return static_cast<int>(i);
    }
    return -1;
}

struct CrankResult {
    double worstNeed = 0;  // battery voltage the most demanding battery-fed regulator needs
    std::string ref;
};

/// Cold crank (ISO 16750-2): the battery dips to ~4.5–6 V; a battery-fed regulator needs vout + dropout.
CrankResult crankHeadroom(const Schematic& sch, const Parts& parts) {
    CrankResult r;
    bool hasSource = !parts.sources.empty();
    if (!hasSource || sch.groundNet() < 0) return r;
    DcResult dc = Simulator(sch).dcOperatingPoint();
    if (!dc.converged) return r;
    auto volts = [&](int net) { return net >= 0 && net < static_cast<int>(dc.netVoltages.size()) ? dc.netVoltages[net] : 0.0; };
    for (const Component* c : parts.regulators) {
        const CustomPart* cp = CustomPartRegistry::instance().find(c->customPart);
        if (!cp || !cp->spec.model.hasRegulator) continue;
        const RegulatorModel& m = cp->spec.model.regulator;
        const int in = cp->spec.pinIndex(m.in), out = cp->spec.pinIndex(m.out);
        if (in < 0 || out < 0) continue;
        const double vin = volts(sch.netOf({c->id, in})), vout = volts(sch.netOf({c->id, out}));
        if (vin < 9.0 || vout <= 0) continue;  // fed from a rail, not from the battery
        const double need = vout + m.dropout;
        if (need > r.worstNeed) {
            r.worstNeed = need;
            r.ref = c->ref;
        }
    }
    return r;
}
}  // namespace

const std::vector<EcuType>& ecuTypes() {
    static const std::vector<EcuType> t = {
        {"bcm", "Body control module", "Lights, wipers, windows, locks and mirrors: high-side switches and LIN nodes",
         {"Smart high-side switches (PROFET) per load with current-sense (IS) diagnostics read by the MCU; LIN master for "
          "doors, mirrors and seats; wake-up from CAN / LIN / switches with sleep current below ~100 µA.",
          "Switch inputs from the harness through RC filters and ESD protection with wetting current."}},
        {"powertrain", "Engine / transmission ECU", "Injectors, ignition, knock / O2 / crank sensors under the hood",
         {"125 °C ambient (AEC-Q100 grade 1 / 0): high-Tg laminate, Class 3 plating, staked heavy parts.",
          "VR / Hall crank and cam inputs through differential conditioning; injector and ignition low-side drivers with "
          "flyback clamps; knock sensor charge amplifier."}},
        {"adas", "ADAS / domain controller", "Camera and radar fusion: SoC, automotive Ethernet, safety MCU",
         {"Application SoC plus an ASIL-D lockstep safety MCU (AURIX / Stellar) supervising it; PMIC with windowed watchdog.",
          "100/1000BASE-T1 PHYs, FPD-Link / GMSL camera links, PCIe and LPDDR length-matched on an HDI stack."}},
        {"ev", "EV battery / inverter / charger", "High-voltage domain: BMS, SiC inverter gate drives, OBC",
         {"Galvanic isolation between the HV and 12 V domains (ISO 6469-3): reinforced isolators, creepage per IEC 60664.",
          "Cell monitors on isoSPI daisy chains; desaturation-protected isolated gate drivers; HV interlock (HVIL) loop."}},
        {"chassis", "Chassis / brake / steering", "EPS, ABS / ESC: ASIL-D with redundant power and lockstep cores",
         {"Two supply paths with ideal-diode OR-ing, redundant sensors (dual Hall / resolver), lockstep MCU, safe-state "
          "drive shutdown in hardware.",
          "H-bridge / three-phase gate drivers with current sensing and phase-cut relays."}},
        {"gateway", "Central gateway / telematics", "Routes CAN-FD, LIN and automotive Ethernet; LTE / V2X modem",
         {"Several CAN-FD channels with termination and ESD per channel, an Ethernet switch with T1 PHYs, secure boot "
          "(HSM) and an external flash for logs and over-the-air updates.",
          "Always-on low-power domain with wake-up on bus activity."}},
    };
    return t;
}

const EcuType* findEcuType(const std::string& id) {
    for (const auto& t : ecuTypes())
        if (t.id == id) return &t;
    return nullptr;
}

bool isEcuProject(const Project& project) { return !project.ecuType.empty() || project.industry == "automotive"; }

std::vector<RuleViolation> automotiveChecks(const Project& project) {
    std::vector<RuleViolation> out;
    if (!isEcuProject(project)) return out;
    const Schematic& sch = project.schematic;
    const Parts parts = classify(sch);
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = sev;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };
    const bool robot = isRobotProject(project);  // the robotics checks already cover the shared rules

    // ---------------------------------------------------------------- 1. transient & power protection front-end
    if (!parts.sources.empty()) {
        if (!robot && !hasReverseProtection(sch, parts))
            add(Severity::Warning, "REL_REVERSE_POLARITY",
                "No reverse-battery protection: a battery connected backwards (ISO 16750-2 reverse voltage test) destroys "
                "the ECU. Add an ideal-diode controller with an N-MOSFET (LM74700-Q1), a P-MOSFET in the supply path or an "
                "N-MOSFET in the ground return.");
        if (!robot && parts.fuses.empty() && parts.efuses.empty())
            add(Severity::Warning, "REL_NO_FUSE",
                "No fuse or eFuse on the battery input: a short downstream draws the full battery current. Use a smart eFuse "
                "(TPS1213-Q1, with fast short-circuit cut-off and diagnostics) or a fuse sized to the harness.");
        bool sm8s = false;
        for (const Component* d : parts.tvs) sm8s |= containsAny(up(d->value), {"SM8S", "SM5S", "SMDJ"});
        if (!parts.tvs.empty() && !sm8s)
            add(Severity::Info, "REL_LOAD_DUMP_RATING",
                "The TVS on the battery input suits a centrally clamped load dump (ISO 16750-2 test B). For an "
                "unsuppressed load dump (test A: up to ~100 V for 400 ms) use an SM8S / SM5S-class TVS (5–6.6 kW).");
        if (!hasPiFilter(sch, parts))
            add(Severity::Warning, "REL_EMI_FILTER",
                "No EMI filter on the battery input: add a common-mode choke and a C–L–C pi filter (CISPR 25 conducted "
                "emissions, ISO 11452 immunity) between the connector protection and the regulators.");
    }

    // ---------------------------------------------------------------- 2. voltage regulation & management
    {
        const CrankResult crank = crankHeadroom(sch, parts);
        if (crank.worstNeed > 6.0)
            add(Severity::Warning, "REL_COLD_CRANK",
                crank.ref + " needs " + fmt("%.1f V", crank.worstNeed) + " from the battery, but cranking pulls it down to "
                "4.5–6 V (ISO 16750-2 starting profile): the ECU resets every engine start. Add a buck-boost pre-regulator or "
                "a low-dropout front end.");
        else if (crank.worstNeed > 4.5)
            add(Severity::Info, "REL_COLD_CRANK",
                crank.ref + " needs " + fmt("%.1f V", crank.worstNeed) + " from the battery: fine for normal cranking, but a "
                "severe cold crank (down to ~4 V) resets it; a buck-boost pre-regulator keeps the rails alive.");
    }
    if (!parts.compute.empty()) {
        if (parts.watchdogs.empty()) {
            add(Severity::Warning, "REL_WATCHDOG",
                "No external watchdog / supervisor: if the MCU software hangs nothing resets it. Add a (windowed) watchdog "
                "supervisor or a safety PMIC (TPS3851-Q1, TLF35584) that the MCU must kick, wired to its reset (ISO 26262 "
                "independent monitoring).");
        } else {
            bool resets = false, kicked = false;
            for (const Component* w : parts.watchdogs) {
                const int rst = pinNamed(*w, {"RESET", "RST", "NRST", "RESET_N", "ROUT"});
                const int wdi = pinNamed(*w, {"WDI", "WDT", "WDO_I", "WD_IN"});
                if (rst >= 0) resets |= connectsTo(sch, sch.netOf({w->id, rst}), parts.compute);
                if (wdi >= 0) kicked |= connectsTo(sch, sch.netOf({w->id, wdi}), parts.compute);
            }
            if (!resets || !kicked)
                add(Severity::Warning, "REL_WATCHDOG",
                    std::string("The watchdog supervisor is not fully wired: ") +
                        (!resets ? "its RESET output must drive the MCU reset" : "the MCU must kick its WDI input") + ".");
        }
    }

    // ---------------------------------------------------------------- 3. safety microcontroller
    if (!parts.compute.empty()) {
        if (parts.safetyMcus.empty())
            add(Severity::Info, "REL_SAFETY_MCU",
                parts.compute.front()->ref + " is a general-purpose MCU (QM / up to ASIL-B with software measures). ASIL-C/D "
                "functions need a lockstep MCU with ECC and safety mechanisms (Infineon AURIX TC3xx, ST Stellar, Renesas "
                "RH850, NXP S32K3, TI TMS570).");
        if (parts.memories.empty())
            add(Severity::Info, "REL_NVM",
                "No external EEPROM / flash: diagnostic trouble codes (DTCs), calibration and logs need grade-1 non-volatile "
                "memory with 20-year retention (e.g. 24LC256, M95 SPI EEPROM).");
        const bool can = !netsNamed(sch, {"CANH", "CAN_H"}).empty();
        if (parts.crystals.empty())
            add(can ? Severity::Warning : Severity::Info, "REL_CLOCK",
                std::string("No crystal / oscillator: the MCU's internal RC drifts ±1–2 % across −40 … 125 °C") +
                    (can ? ", outside CAN-FD's ±0.3 % bit-timing tolerance. Fit an AEC-Q200 crystal or oscillator."
                         : ". Fit an AEC-Q200 crystal for timing-critical functions."));
    }

    // ---------------------------------------------------------------- 4. vehicle networks
    {
        auto bus = [&](std::initializer_list<const char*> names, const char* what) {
            for (int net : netsNamed(sch, names))
                if (!hasEsdDiode(sch, net)) {
                    add(Severity::Warning, "REL_BUS_ESD",
                        std::string(what) + " line " + netName(sch, net) + " has no ESD / transient protection at the "
                        "connector: add a bus TVS (PESD2CAN / NUP2105 for CAN, PESD1LIN for LIN) — ISO 10605, ISO 7637-3.");
                    return;
                }
        };
        bus({"CANH", "CAN_H", "CANL", "CAN_L"}, "CAN");
        bus({"LIN"}, "LIN");
        if (!robot) {
            auto hi = netsNamed(sch, {"CANH", "CAN_H"}), lo = netsNamed(sch, {"CANL", "CAN_L"});
            if (!hi.empty() && !lo.empty() && !hasTermination(sch, hi.front(), lo.front()))
                add(Severity::Warning, "REL_BUS_TERMINATION",
                    "CAN bus " + netName(sch, hi.front()) + " / " + netName(sch, lo.front()) + " has no termination: fit "
                    "120 Ω (or split 2 × 60 Ω with 4.7 nF to ground) at the two ends of the bus.");
        }
        for (const Component* l : parts.lin) {
            const int pin = pinNamed(*l, {"LIN"});
            const int net = pin >= 0 ? sch.netOf({l->id, pin}) : -1;
            if (net < 0) continue;
            bool pullUp = false, cap = false;
            for (const auto& c : sch.components())
                for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
                    if (sch.netOf({c.id, i}) == net) {
                        if (c.kind == ComponentKind::Resistor) pullUp = true;
                        if (c.kind == ComponentKind::Capacitor) cap = true;
                    }
            if (!pullUp && !cap)
                add(Severity::Info, "REL_LIN_NODE",
                    l->ref + ": a LIN master needs 1 kΩ + diode from the bus to battery; a slave a 220 pF capacitor to ground "
                    "(LIN 2.x / SAE J2602).",
                    {l->id});
        }
    }

    // ---------------------------------------------------------------- 5. actuation
    for (const Component* h : parts.highSide) {
        const int is = pinNamed(*h, {"IS", "SENSE", "CS", "SNS", "DIAG"});
        if (is >= 0 && !connectsTo(sch, sch.netOf({h->id, is}), parts.compute))
            add(Severity::Warning, "REL_HIGHSIDE_DIAG",
                h->ref + ": its current-sense / diagnostic output is not read by the MCU, so open load, short circuit and "
                "overtemperature go unnoticed. Route IS through a sense resistor to an ADC input.",
                {h->id});
    }

    // ---------------------------------------------------------------- 6. sensor conditioning
    {
        std::vector<const Component*> inputs = parts.compute;
        inputs.insert(inputs.end(), parts.amplifiers.begin(), parts.amplifiers.end());
        int reported = 0;
        for (const Component* j : parts.connectors) {
            for (int i = 0; i < static_cast<int>(j->def().pins.size()) && reported < 5; ++i) {
                const int net = sch.netOf({j->id, i});
                if (net < 0 || sch.netRole(net) != NetRole::Signal || !connectsTo(sch, net, inputs)) continue;
                bool filtered = false;
                for (const auto& c : sch.components())
                    for (int k = 0; k < static_cast<int>(c.def().pins.size()); ++k)
                        if (sch.netOf({c.id, k}) == net &&
                            (c.kind == ComponentKind::Resistor || c.kind == ComponentKind::Capacitor || c.kind == ComponentKind::Diode))
                            filtered = true;
                if (!filtered) {
                    add(Severity::Warning, "REL_INPUT_PROTECTION",
                        j->ref + " pin " + std::to_string(i + 1) + " (" + netName(sch, net) + ") goes straight from the "
                        "harness to an IC input: add a series resistor, an RC filter and an ESD diode (ISO 10605 ±8 kV "
                        "contact / ±15 kV air) so service handling cannot destroy it.",
                        {j->id});
                    ++reported;
                }
            }
        }
    }
    return out;
}

std::vector<RobotSegment> ecuSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const Parts parts = classify(sch);
    std::set<std::string> warn;
    for (const auto& v : automotiveChecks(project))
        if (v.severity != Severity::Info) warn.insert(v.code);
    for (const auto& v : roboticsChecks(project))
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
    const std::string type = project.ecuType;
    std::vector<RobotSegment> out;

    RobotSegment shield{"shield", "Transient & Power Protection Front-End", "", {}, {}};
    shield.items.push_back(item("Battery input", !parts.sources.empty(), refs(parts.sources), "Battery / KL30 input"));
    shield.items.push_back(item("Reverse-polarity protection", !parts.sources.empty() && !warn.count("REL_REVERSE_POLARITY"),
                                "Present", "Ideal-diode controller / P-MOSFET"));
    bool sm8s = false;
    for (const Component* d : parts.tvs) sm8s |= containsAny(up(d->value), {"SM8S", "SM5S", "SMDJ"});
    shield.items.push_back(item("Load-dump TVS", !parts.tvs.empty(), refs(parts.tvs) + (sm8s ? " (SM8S-class)" : ""),
                                "SM8S-class TVS (ISO 16750-2 load dump)"));
    shield.items.push_back(item("EMI pi filter / CM choke", !warn.count("REL_EMI_FILTER"), "C–L–C filter on the input",
                                "Common-mode choke + pi filter (CISPR 25)"));
    shield.items.push_back(item("eFuse / fuse", !parts.fuses.empty() || !parts.efuses.empty(), refs(parts.efuses.empty() ? parts.fuses : parts.efuses),
                                "Smart eFuse (TPS1213-Q1) or fuse"));
    shield.guidance = {"ISO 7637-2 pulses 1–3b and ISO 16750-2 load dump, reverse battery and jump start (24 V for 1 min)."};
    out.push_back(shield);

    RobotSegment reg{"regulation", "Voltage Regulation & Management", "", {}, {}};
    reg.items.push_back(item("Regulated rails", !parts.regulators.empty(), refs(parts.regulators), "5 V / 3.3 V regulators"));
    reg.items.push_back(item("Survives cold crank", !parts.regulators.empty() && !warn.count("REL_COLD_CRANK"),
                             "Front end holds up at crank", "Buck-boost pre-regulator for 4 V cranking"));
    reg.items.push_back(item("Watchdog / PMIC", !parts.watchdogs.empty() && !warn.count("REL_WATCHDOG"),
                             refs(parts.watchdogs) + " resets the MCU", "Windowed watchdog wired to the MCU reset"));
    const bool analog = !parts.amplifiers.empty();
    reg.items.push_back(item("Clean analog supply", !analog || parts.regulators.size() >= 2,
                             analog ? "Separate LDO for the sensor front end" : "No analog front end",
                             "Ultra-low-noise LDO for the sensors"));
    reg.guidance = {"9–16 V nominal, 4 V crank, 24 V jump start: buck-boost pre-regulator, synchronous bucks for 5 V / 3.3 V, "
                    "low-noise LDOs for analog; a windowed watchdog the MCU must kick."};
    out.push_back(reg);

    RobotSegment mcu{"mcu", "Automotive Safety Microcontroller", "", {}, {}};
    mcu.items.push_back(item("Microcontroller", !parts.compute.empty(), refs(parts.compute), "Automotive MCU"));
    mcu.items.push_back(item("Lockstep / ASIL-D core", !parts.safetyMcus.empty(), refs(parts.safetyMcus),
                             "AURIX TC3xx / Stellar / RH850 / S32K3 / TMS570 for ASIL-C/D"));
    mcu.items.push_back(item("External EEPROM / flash", !parts.memories.empty(), refs(parts.memories), "Grade-1 EEPROM / flash"));
    mcu.items.push_back(item("Crystal / oscillator", !parts.crystals.empty(), refs(parts.crystals), "AEC-Q200 crystal"));
    mcu.guidance = {"ISO 26262: lockstep cores, ECC memories, independent watchdog and clock monitoring."};
    out.push_back(mcu);

    RobotSegment net{"network", "Vehicle Network & Communication", "", {}, {}};
    const auto canH = netsNamed(sch, {"CANH", "CAN_H"});
    net.items.push_back(item("CAN-FD node", !canH.empty() && !parts.fieldbus.empty(), refs(parts.fieldbus), "CAN-FD transceiver"));
    net.items.push_back(item("Bus termination & ESD", !canH.empty() && !warn.count("REL_BUS_TERMINATION") && !warn.count("REL_BUS_ESD"),
                             "120 Ω and bus TVS", "Termination and bus ESD protection"));
    const bool needLin = type == "bcm";
    net.items.push_back(item("LIN node", !parts.lin.empty() || !needLin, parts.lin.empty() ? "Not needed on this ECU" : refs(parts.lin),
                             "LIN transceiver for body nodes"));
    const bool needEth = type == "adas" || type == "gateway";
    net.items.push_back(item("Automotive Ethernet PHY", !parts.ethernetPhys.empty() || !needEth,
                             parts.ethernetPhys.empty() ? "Not needed on this ECU" : refs(parts.ethernetPhys),
                             "100/1000BASE-T1 PHY"));
    net.guidance = {"Physical-layer transceivers between the MCU and the harness: CAN-FD, LIN, 100/1000BASE-T1, each with "
                    "termination, common-mode chokes and bus ESD protection."};
    out.push_back(net);

    RobotSegment act{"actuation", "High-Side / Low-Side Power Actuation", "", {}, {}};
    const bool drivers = !parts.highSide.empty() || !parts.drivers.empty() || !parts.powerFets.empty();
    std::vector<const Component*> stage = parts.highSide;
    stage.insert(stage.end(), parts.drivers.begin(), parts.drivers.end());
    stage.insert(stage.end(), parts.powerFets.begin(), parts.powerFets.end());
    act.items.push_back(item("Load drivers", drivers, refs(stage), "PROFET high-side switches / gate drivers"));
    act.items.push_back(item("Inductive loads clamped", drivers && !warn.count("REL_FLYBACK"), "Freewheel / clamp paths",
                             "Freewheel diodes or active clamps"));
    act.items.push_back(item("Diagnostics read back", drivers && !warn.count("REL_HIGHSIDE_DIAG"),
                             parts.highSide.empty() ? "Current sense on the driven loads" : "IS feedback to the MCU",
                             "Read IS / current sense on the MCU"));
    act.guidance = {"Smart high-side switches with open-load / short / overtemperature diagnostics; H-bridge gate drivers "
                    "for motors with current sense and dead time."};
    out.push_back(act);

    RobotSegment sen{"sensors", "Sensor Conditioning & Ingestion", "", {}, {}};
    sen.items.push_back(item("Sensor amplifier", analog, refs(parts.amplifiers), "Differential / instrumentation amplifier"));
    sen.items.push_back(item("Protected inputs (ISO 10605)", !warn.count("REL_INPUT_PROTECTION"), "RC + ESD on harness inputs",
                             "Series R, RC filter and ESD diode on each input"));
    sen.guidance = {"VR / Hall wheel-speed, O2 and strain-gauge inputs through instrumentation amplifiers; RC + ESD "
                    "networks on every harness pin; isolated ADCs where the ground differs."};
    out.push_back(sen);

    for (auto& seg : out) {
        int ok = 0;
        for (const auto& i : seg.items) ok += i.ok;
        seg.status = ok == static_cast<int>(seg.items.size()) ? "complete" : ok == 0 ? "missing" : "partial";
    }
    return out;
}

Json ecuSegmentsJson(const Project& project) {
    Json root = Json::object();
    root["platform"] = project.ecuType;
    root["applies"] = isEcuProject(project);
    Json types = Json::array();
    for (const auto& t : ecuTypes()) {
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
    for (const auto& seg : ecuSegments(project)) {
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
