#include "sieda/Medical.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "SystemParts.hpp"
#include "sieda/Isolation.hpp"
#include "sieda/Project.hpp"

namespace sieda {

namespace {
using namespace sysparts;

/// Nets that reach the patient: electrode / lead / probe nets.
std::vector<int> patientNets(const Schematic& sch) {
    return netsNamed(sch, {"ECG", "EEG", "EMG", "ELECTRODE", "PATIENT", "LEAD", "SPO2", "PROBE", "EKG"});
}

bool touchesNet(const Schematic& sch, const Component& c, int net) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (sch.netOf({c.id, i}) == net) return true;
    return false;
}

bool twoMopp(const std::string& cls) { return cls == "bf" || cls == "cf" || cls == "life" || cls == "home" || cls.empty(); }
bool lifeCritical(const std::string& cls) { return cls == "life" || cls == "implant"; }

struct Analysis {
    Parts parts;
    GalvanicDomains doms;
    std::vector<int> patient;                // patient-connected nets
    int patientDomain = -1;
    std::set<int> supplyDomains;             // domains holding a power source
    std::vector<const Component*> barriers;  // barrier parts with a pin in the patient domain
};

Analysis analyse(const Schematic& sch) {
    Analysis a;
    a.parts = classify(sch);
    a.doms = galvanicDomains(sch);
    a.patient = patientNets(sch);
    for (int n : a.patient)
        if (const int d = a.doms.domainOfNet(n); d >= 0) a.patientDomain = d;
    for (const Component* s : a.parts.sources)
        for (int pin = 0; pin < 2; ++pin)
            if (const int d = a.doms.domainOfNet(sch.netOf({s->id, pin})); d >= 0) a.supplyDomains.insert(d);
    if (a.patientDomain >= 0)
        for (const auto& c : sch.components()) {
            if (!isIsolationBarrier(c)) continue;
            bool patientSide = false, otherSide = false;
            for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
                const int d = a.doms.domainOfNet(sch.netOf({c.id, i}));
                if (d < 0) continue;
                (d == a.patientDomain ? patientSide : otherSide) = true;
            }
            if (patientSide && otherSide) a.barriers.push_back(&c);
        }
    return a;
}

/// strict: full severities (a class is set, or the segment checklist); otherwise everything is reported as Info.
std::vector<RuleViolation> checks(const Project& project, bool strict) {
    std::vector<RuleViolation> out;
    if (!isMedicalProject(project)) return out;
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    const std::string cls = project.medicalClass;
    const Analysis a = analyse(sch);
    const Parts& parts = a.parts;
    const int gnd = sch.groundNet();
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = strict ? sev : Severity::Info;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };

    // ---------------------------------------------------------------- 1. patient isolation & defibrillator protection
    if (!a.patient.empty() && cls != "implant") {
        if (a.patientDomain < 0 || a.supplyDomains.count(a.patientDomain))
            add(Severity::Warning, "REL_PATIENT_ISOLATION",
                "The patient connection (" + netName(sch, a.patient.front()) + ") is galvanically connected to the power "
                "supply: mains or charger leakage reaches the patient. Put the applied part on its own isolated domain "
                "(medical isolated DC-DC + digital isolators) and keep resistors and Y-capacitors off the barrier.");
        else {
            std::vector<const Component*> weak;
            for (const Component* b : a.barriers)
                if (std::find(parts.moppIsolators.begin(), parts.moppIsolators.end(), b) == parts.moppIsolators.end())
                    weak.push_back(b);
            if (!weak.empty() && twoMopp(cls))
                add(Severity::Warning, "REL_MOPP_BARRIER",
                    weak.front()->ref + " (" + partName(*weak.front()) + ") crosses the patient barrier but is not rated "
                    "for 2 × MOPP (4 kV AC, 8 mm creepage): use 5 kV reinforced, wide-body isolators (ADuM4401, ISO7741) "
                    "and a medical isolated converter.",
                    {weak.front()->id});
            const double need = twoMopp(cls) ? 8.0 : 4.0;
            if (s.isolationGap + 1e-9 < need)
                add(Severity::Warning, "REL_MOPP_CREEPAGE",
                    "The patient barrier needs " + fmt("%.0f mm", need) + " creepage / clearance (" +
                        (need >= 8 ? "2" : "1") + " × MOPP); the board's isolation barrier is " +
                        (s.isolationGap > 0 ? fmt("%.1f mm", s.isolationGap) : std::string("not set")) +
                        ". Set it in Board Setup → Protection so placement, routing, pours and DRC keep it.");
        }
        // Defibrillator protection: each patient lead through a high-voltage series resistor with a GDT / TVS clamp.
        std::vector<const Component*> clampParts = parts.gdts;
        clampParts.insert(clampParts.end(), parts.tvs.begin(), parts.tvs.end());
        clampParts.insert(clampParts.end(), parts.diodes.begin(), parts.diodes.end());
        for (int net : a.patient) {
            bool clamp = false, series = false;
            for (const Component* c : clampParts) clamp |= touchesNet(sch, *c, net);
            for (const auto& c : sch.components())
                if (c.kind == ComponentKind::Resistor && touchesNet(sch, c, net))
                    if (auto v = parseEngineeringValue(primaryValue(c.value)); v && *v >= 1000) series = true;
            if (!(clamp && series)) {
                add(cls == "cf" ? Severity::Warning : Severity::Info, "REL_DEFIB_PROTECTION",
                    netName(sch, net) + " is not defibrillator-proof: a 5 kV defibrillator pulse arrives on every lead. Fit "
                    "a gas discharge tube / TVS clamp and a pulse-rated series resistor (≥ 1 kΩ, typically 10 kΩ) before "
                    "the amplifier (IEC 60601-2-27).");
                break;
            }
        }
    }

    // ---------------------------------------------------------------- 2. biosignal acquisition
    if (!a.patient.empty()) {
        std::vector<const Component*> inas;
        for (const Component* amp : parts.amplifiers)
            if (containsAny(partName(*amp), {"INA", "AD842", "AD62", "ADS129", "ADS119", "AD8232", "MAX3000"})) inas.push_back(amp);
        if (inas.empty())
            add(Severity::Warning, "REL_BIOSIGNAL_AMP",
                "No instrumentation amplifier on the patient inputs: microvolt biosignals need a high-CMRR, high-impedance "
                "front end (INA333, AD8421, ADS129x).");
        const bool ecg = !netsNamed(sch, {"ECG", "EKG"}).empty();
        if (ecg && cls != "implant") {
            bool drl = false;
            for (int net : netsNamed(sch, {"DRL", "RLD", "RL_DRIVE", "RIGHT_LEG"}))
                for (const auto& c : sch.components())
                    if (c.kind == ComponentKind::OpAmp && sch.netOf({c.id, 2}) == net) drl = true;
            if (!drl)
                add(Severity::Warning, "REL_DRL",
                    "ECG without a driven right leg: an amplifier that senses the common mode and drives it back into the "
                    "patient (DRL / RLD electrode, current-limited) cancels 50 / 60 Hz mains hum.");
        }
    }

    // ---------------------------------------------------------------- 3. safety compute & power
    if (!parts.compute.empty()) {
        if (parts.safetyMcus.empty())
            add(lifeCritical(cls) ? Severity::Warning : Severity::Info, "REL_SAFETY_MCU",
                parts.compute.front()->ref + " is not a lockstep safety MCU: life-sustaining and implantable devices run on "
                "dual-core lockstep parts with hardware self-test (TI Hercules TMS570 / RM4x, STM32 safety-certified lines).",
                {parts.compute.front()->id});
        if (parts.windowWdt.empty() && (lifeCritical(cls) || parts.watchdogs.empty()))
            add(Severity::Warning, "REL_SAFE_STATE_WATCHDOG",
                std::string(parts.watchdogs.empty() ? "No external watchdog" : "The watchdog is not windowed") +
                    ": a hung or runaway processor must be caught by an independent windowed watchdog (TPS3850, TPS3430) "
                    "that forces the safe state — pump motor stopped, laser or heater off — in hardware.");
    }
    {
        bool battery = false;
        for (const Component* src : parts.sources) battery |= src->kind == ComponentKind::Battery;
        if (battery) {
            if (parts.bms.empty())
                add(Severity::Warning, "REL_BMS",
                    "Lithium cell without a protector: add an over-voltage / under-voltage / over-current protection IC "
                    "(DW01A + dual MOSFET, BQ2970) in series with the charger's own limits.");
            bool ntc = false;
            for (const auto& c : sch.components()) ntc |= containsAny(up(c.value), {"NTC", "THERM"});
            if (!ntc)
                add(Severity::Warning, "REL_BMS_THERMAL",
                    "No cell temperature sensing: an NTC on the charger's TEMP input (and the gauge) stops charging when "
                    "the cell overheats — a second, independent cut-off besides the protector.");
        }
    }

    // ---------------------------------------------------------------- 4. coexistence & clinical wireless
    {
        std::vector<const Component*> clamps = parts.tvs;
        clamps.insert(clamps.end(), parts.diodes.begin(), parts.diodes.end());
        clamps.insert(clamps.end(), parts.gdts.begin(), parts.gdts.end());
        clamps.insert(clamps.end(), parts.movs.begin(), parts.movs.end());
        for (const Component* j : parts.connectors) {
            bool bad = false;
            int badNet = -1;
            for (int i = 0; i < static_cast<int>(j->def().pins.size()) && !bad; ++i) {
                const int net = sch.netOf({j->id, i});
                if (net < 0 || net == gnd || sch.netRole(net) == NetRole::Ground) continue;
                if (sch.nets()[static_cast<size_t>(net)].pins.size() < 2) continue;  // unconnected pin
                bool ok = false;
                for (const Component* c : clamps) ok |= touchesNet(sch, *c, net);
                if (std::find(parts.coax.begin(), parts.coax.end(), j) != parts.coax.end()) ok = true;  // RF port
                if (!ok) {
                    bad = true;
                    badNet = net;
                }
            }
            if (bad) {
                add(Severity::Warning, "REL_CONNECTOR_ESD",
                    j->ref + " pin net " + netName(sch, badNet) + " has no ESD protection: every exterior pin needs a "
                    "low-capacitance ESD diode to survive ±15 kV air discharge (IEC 61000-4-2) when staff plug in leads.",
                    {j->id});
                break;
            }
        }
    }
    if (!parts.rf.empty()) {
        if (std::fabs(s.singleEndedImpedance - 50.0) > 5.0)
            add(Severity::Warning, "REL_TELEMETRY_IMPEDANCE",
                "The BLE / UWB antenna feed needs a 50 Ω matched line; the board target is " +
                    fmt("%.0f Ω", s.singleEndedImpedance) + " (Board Setup → Stack-up).");
        bool plane = false;
        for (const auto& z : project.pcb.zones) plane |= gnd >= 0 && z.net == netName(sch, gnd);
        if (!plane)
            add(Severity::Warning, "REL_TELEMETRY_GROUND",
                "No ground plane under the radio: the 50 Ω feed and the shield-can ring need solid ground.");
        add(Severity::Info, "REL_SHIELD_CAN",
            "Fit a soldered nickel-silver shield can over the processor and radio so electrosurgical (ESU) noise cannot "
            "crash the software (IEC 60601-1-2 immunity).");
    }
    return out;
}
}  // namespace

const std::vector<MedicalClass>& medicalClasses() {
    static const std::vector<MedicalClass> c = {
        {"bf", "Type BF (body-floating)", "Patient monitors, wearables, SpO2 and temperature probes",
         {"Floating applied part: 2 × MOPP to mains (4 kV AC, 8 mm creepage), patient leakage ≤ 100 µA (normal).",
          "Isolated AFE with digital isolators; ESD on every connector pin."}},
        {"cf", "Type CF (cardiac)", "ECG, intracardiac and defibrillator-proof monitoring",
         {"Patient leakage ≤ 10 µA: medical isolated converter with < 10 pF barrier capacitance, no Y-capacitors.",
          "Defibrillator-proof leads (GDT + TVS + pulse-rated series resistors), driven right leg, AGND split."}},
        {"life", "Life-support / therapy", "Infusion pumps, ventilators, surgical lasers and energy delivery",
         {"Lockstep safety MCU and a windowed watchdog that forces the safe state (motor stop, laser off) in hardware.",
          "Redundant battery protection and thermal cut-offs; IEC 62304 class C software."}},
        {"implant", "Active implantable", "Pacemakers, neurostimulators and implanted monitors",
         {"Lockstep / safety MCU, windowed watchdog, hermetic packages and ultra-low-power operation.",
          "Battery protection with redundant over-voltage and over-temperature cut-off."}},
        {"home", "Home healthcare", "Connected home devices and consumer medical wearables (IEC 60601-1-11)",
         {"2 × MOPP to the charger / mains, ESD and moisture robustness, BLE telemetry with a 50 Ω feed."}},
    };
    return c;
}

const MedicalClass* findMedicalClass(const std::string& id) {
    for (const auto& c : medicalClasses())
        if (c.id == id) return &c;
    return nullptr;
}

bool isMedicalProject(const Project& project) { return !project.medicalClass.empty() || project.industry == "medical"; }

std::vector<RuleViolation> medicalChecks(const Project& project) { return checks(project, !project.medicalClass.empty()); }

std::vector<RobotSegment> medicalSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    const std::string cls = project.medicalClass;
    const Analysis a = analyse(sch);
    const Parts& parts = a.parts;
    std::set<std::string> warn;
    for (const auto& v : checks(project, true))
        if (v.severity != Severity::Info) warn.insert(v.code);
    auto refs = [](const std::vector<const Component*>& v) {
        std::string r;
        for (size_t i = 0; i < v.size() && i < 6; ++i) r += (i ? ", " : "") + v[i]->ref;
        if (v.size() > 6) r += ", …";
        return r;
    };
    auto item = [](const std::string& label, bool ok, const std::string& found, const std::string& todo) {
        return RobotCheckItem{label, ok, ok ? found : todo};
    };
    const bool patient = !a.patient.empty();
    std::vector<RobotSegment> out;

    RobotSegment iso{"isolation", "Patient Isolation & Defibrillator Protection", "", {}, {}};
    iso.items.push_back(item("Isolated applied part", patient && !warn.count("REL_PATIENT_ISOLATION"),
                             "Patient domain separated by " + refs(a.barriers), "Patient connection on its own isolated domain"));
    iso.items.push_back(item("2 × MOPP barrier parts", patient && !a.barriers.empty() && !warn.count("REL_MOPP_BARRIER"),
                             "5 kV reinforced isolators / converter", "2 × MOPP-rated isolators and converter"));
    iso.items.push_back(item("8 mm creepage / clearance", patient && !warn.count("REL_MOPP_CREEPAGE"),
                             fmt("%.0f mm isolation barrier", s.isolationGap), "Isolation barrier 8 mm (Board Setup)"));
    iso.items.push_back(item("Defibrillator-proof inputs", patient && !warn.count("REL_DEFIB_PROTECTION"),
                             refs(parts.gdts) + " + series resistors", "GDT / TVS clamp and pulse-rated series resistor per lead"));
    iso.guidance = {"IEC 60601-1: 2 × MOPP = 4 kV AC, 8 mm creepage, 5 mm clearance; leakage ≤ 10 µA (CF) / 100 µA (BF); "
                    "defibrillator-proof applied parts per IEC 60601-2-27."};
    out.push_back(iso);

    RobotSegment bio{"biosignal", "High-Precision Biosignal Acquisition", "", {}, {}};
    bio.items.push_back(item("Instrumentation amplifier", patient && !warn.count("REL_BIOSIGNAL_AMP"), refs(parts.amplifiers),
                             "INA333 / AD8421 / ADS129x front end"));
    const bool ecg = !netsNamed(sch, {"ECG", "EKG"}).empty();
    bio.items.push_back(item("Driven right leg", ecg ? !warn.count("REL_DRL") : true, ecg ? "DRL amplifier drives RL" : "No ECG leads",
                             "DRL / RLD amplifier"));
    bool isolatedData = false;
    for (const Component* b : a.barriers) isolatedData |= std::find(parts.isolators.begin(), parts.isolators.end(), b) != parts.isolators.end() ||
                                                          std::find(parts.moppIsolators.begin(), parts.moppIsolators.end(), b) != parts.moppIsolators.end();
    bio.items.push_back(item("AGND split with isolated data", patient && isolatedData && !warn.count("REL_PATIENT_ISOLATION"),
                             "Patient ground separate; data through isolators", "Separate patient ground, digital isolators"));
    bio.guidance = {"Microvolt signals: high-CMRR INA, driven right leg against mains hum, patient AGND split from DGND "
                    "with data crossing only through isolators."};
    out.push_back(bio);

    RobotSegment cmp{"compute", "High-Reliability Medical Compute & Power", "", {}, {}};
    cmp.items.push_back(item("Lockstep / safety MCU", !parts.safetyMcus.empty() || (!lifeCritical(cls) && !parts.compute.empty()),
                             parts.safetyMcus.empty() ? refs(parts.compute) + " (QM / class B software)" : refs(parts.safetyMcus),
                             "Lockstep safety MCU (TMS570 / Hercules)"));
    std::vector<const Component*> wd = parts.windowWdt.empty() ? parts.watchdogs : parts.windowWdt;
    cmp.items.push_back(item("Fail-safe watchdog", !parts.compute.empty() && !warn.count("REL_SAFE_STATE_WATCHDOG"), refs(wd),
                             "Windowed watchdog forcing the safe state"));
    bool battery = false;
    for (const Component* src : parts.sources) battery |= src->kind == ComponentKind::Battery;
    cmp.items.push_back(item("Battery protection & thermal cut-off", battery ? !warn.count("REL_BMS") && !warn.count("REL_BMS_THERMAL") : true,
                             battery ? refs(parts.bms) + " + NTC" : "Mains / external supply", "Protector IC and NTC cut-off"));
    cmp.guidance = {"No silent freeze: lockstep cores, independent windowed watchdog to a hardware safe state, redundant "
                    "cell protection (IEC 62133, IEC 60601-1 §15.4.3)."};
    out.push_back(cmp);

    RobotSegment rf{"coexistence", "Coexistence & Clinical Wireless", "", {}, {}};
    rf.items.push_back(item("50 Ω BLE / UWB feed", parts.rf.empty() || (!warn.count("REL_TELEMETRY_IMPEDANCE") && !warn.count("REL_TELEMETRY_GROUND")),
                            parts.rf.empty() ? "No radio on this board" : refs(parts.rf) + ", 50 Ω over ground", "50 Ω feed over a ground plane"));
    rf.items.push_back(item("ESD on every connector pin", !warn.count("REL_CONNECTOR_ESD"), "Every exterior pin clamped",
                            "Low-capacitance ESD diodes on all connector pins"));
    rf.items.push_back(item("Shielding ground", parts.rf.empty() || !warn.count("REL_TELEMETRY_GROUND"), "Ground plane for shield cans",
                            "Ground plane / shield can ring"));
    rf.guidance = {"IEC 60601-1-2: MRI and ESU fields, ±8 kV contact / ±15 kV air ESD; shield cans over the processor and "
                   "radio, ferrites and ESD arrays on every lead."};
    out.push_back(rf);

    scoreSegments(out);
    return out;
}

Json medicalSegmentsJson(const Project& project) {
    std::vector<PlatformInfo> classes;
    for (const auto& c : medicalClasses()) classes.push_back({c.id, c.name, c.description, c.guidance});
    return segmentReportJson(project.medicalClass, isMedicalProject(project), classes, medicalSegments(project));
}

}  // namespace sieda
