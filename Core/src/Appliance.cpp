#include "sieda/Appliance.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "SystemParts.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Project.hpp"

namespace sieda {

namespace {
using namespace sysparts;

bool touchesNet(const Schematic& sch, const Component& c, int net) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (sch.netOf({c.id, i}) == net) return true;
    return false;
}
bool anyTouches(const Schematic& sch, const std::vector<const Component*>& parts, int net) {
    for (const Component* c : parts)
        if (touchesNet(sch, *c, net)) return true;
    return false;
}
int pinNet(const Schematic& sch, const Component& c, const char* name) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (pinName(c, i) == name) return sch.netOf({c.id, i});
    return -1;
}
bool motorAppliance(const std::string& t) { return t == "laundry" || t == "refrigeration" || t == "hvac"; }

struct Analysis {
    Parts parts;
    std::set<int> mains;          // AC line / neutral nets (named, or driven by an AC source)
    std::set<int> highVoltage;    // nets more than 60 V from ground in the simulation
    std::vector<int> touchNets, ntcNodes;
    std::vector<const Component*> xCaps, ntcs, modules;
};

Analysis analyse(const Project& project) {
    const Schematic& sch = project.schematic;
    Analysis a;
    a.parts = classify(sch);
    for (int n : netsNamed(sch, {"AC_L", "AC_N", "LINE", "NEUTRAL", "MAINS", "L_IN", "N_IN", "L_FUSED", "L_FILT", "N_FILT"}))
        a.mains.insert(n);
    for (const auto& c : sch.components())
        if (c.kind == ComponentKind::ACSource || (isVoltageSourceKind(c.kind) && containsAny(up(c.value), {"SIN("}))) {
            auto spec = SourceSpec::parse(c.value);
            if (spec && std::fabs(spec->amplitude) > 60)
                for (int pin = 0; pin < 2; ++pin)
                    if (const int n = sch.netOf({c.id, pin}); n >= 0) a.mains.insert(n);
        }
    // Through series fuses and chokes the line and neutral stay mains (L after the fuse, L / N after the filter).
    for (bool grew = true; grew;) {
        grew = false;
        for (const auto& c : sch.components()) {
            if (c.kind != ComponentKind::Fuse && c.kind != ComponentKind::Inductor) continue;
            const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
            if (n0 < 0 || n1 < 0) continue;
            if (a.mains.count(n0) && a.mains.insert(n1).second) grew = true;
            if (a.mains.count(n1) && a.mains.insert(n0).second) grew = true;
        }
    }
    for (const auto& [net, r] : netVoltageRanges(sch))
        if (std::max(std::fabs(r.first), std::fabs(r.second)) > 60) a.highVoltage.insert(net);
    // X capacitors: both pins on mains nets (or an X2-rated part).
    for (const auto& c : sch.components()) {
        const bool cap = c.kind == ComponentKind::Capacitor || containsAny(partName(c), {"X2-", "X1-", "XCAP"});
        if (!cap || c.def().pins.size() < 2) continue;
        const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        if (n0 >= 0 && n1 >= 0 && n0 != n1 && a.mains.count(n0) && a.mains.count(n1)) a.xCaps.push_back(&c);
    }
    for (int n : netsNamed(sch, {"TOUCH", "CAP_KEY", "SLIDER"}))
        if (sch.nets()[static_cast<size_t>(n)].pins.size() >= 2) a.touchNets.push_back(n);
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Resistor || !containsAny(up(c.value), {"NTC", "THERM"})) continue;
        a.ntcs.push_back(&c);
        for (int pin = 0; pin < 2; ++pin) {
            const int n = sch.netOf({c.id, pin});
            if (n >= 0 && n != sch.groundNet() && sch.netRole(n) != NetRole::Ground && sch.netRole(n) != NetRole::Power)
                a.ntcNodes.push_back(n);
        }
    }
    for (const Component* r : a.parts.rf) a.modules.push_back(r);
    return a;
}

/// strict: full severities (an appliance type is set, or the segment checklist); otherwise everything is Info.
std::vector<RuleViolation> checks(const Project& project, bool strict) {
    std::vector<RuleViolation> out;
    if (!isApplianceProject(project)) return out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const std::string type = project.applianceType;
    const Analysis a = analyse(project);
    const Parts& parts = a.parts;
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = strict ? sev : Severity::Info;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };
    auto mainsTouch = [&](const std::vector<const Component*>& list) {
        for (int n : a.mains)
            if (anyTouches(sch, list, n)) return true;
        return false;
    };

    // ---------------------------------------------------------------- 1. AC mains entry & power conversion
    if (a.mains.empty()) {
        add(Severity::Info, "REL_MAINS_INPUT",
            "No AC mains input found (an AC source or nets named AC_L / AC_N): the mains-entry checks are skipped.");
    } else {
        if (!mainsTouch(parts.fuses))
            add(Severity::Warning, "REL_MAINS_FUSE",
                "No fuse on the mains input: a slow-blow (T) fuse in the line conductor, ahead of everything else, "
                "clears a shorted bridge or MOV (IEC 60335-1 §19 abnormal operation).");
        if (!mainsTouch(parts.movs))
            add(Severity::Warning, "REL_MAINS_MOV",
                "No varistor across the mains: a 275 VAC MOV after the fuse clamps surges (IEC 61000-4-5, 1–2 kV) "
                "before they reach the rectifier and the off-line switcher.");
        if (a.xCaps.empty())
            add(Severity::Warning, "REL_X_CAP",
                "No X capacitor across line and neutral: a class-X2 film capacitor with the common-mode choke forms the "
                "EMI filter for CISPR 14-1 conducted emissions.");
        if (parts.cmChokes.empty())
            add(Severity::Warning, "REL_CM_CHOKE",
                "No common-mode choke on the mains input: the switcher's and the inverter's common-mode noise leaks out "
                "on the line cord (CISPR 14-1).");
        if (parts.offline.empty())
            add(Severity::Warning, "REL_OFFLINE_SUPPLY",
                "No off-line converter: a non-isolated LinkSwitch-TN / VIPer buck from the rectified mains powers the "
                "controller efficiently (a capacitive dropper or linear stage wastes watts in standby).");
        add(Severity::Info, "REL_MAINS_CREEPAGE",
            "Keep IEC 60335-1 / 60664-1 creepage between mains copper and everything else (≥ 2.5 mm functional, 6–8 mm "
            "reinforced to user-accessible SELV), route the mains section on its own side of the board and mill slots "
            "under parts that bridge it. The layout keeps the IPC-2221 voltage spacing automatically.");
    }

    // ---------------------------------------------------------------- 2. high-voltage actuation & motor control
    for (const Component* t : parts.triacs) {
        const int mt1 = pinNet(sch, *t, "MT1"), mt2 = pinNet(sch, *t, "MT2");
        if (mt1 < 0 || mt2 < 0) continue;
        // RC snubber: a capacitor and a resistor in series across MT1–MT2.
        bool snubber = false;
        for (const Component* c : parts.caps) {
            const int c0 = sch.netOf({c->id, 0}), c1 = sch.netOf({c->id, 1});
            for (int side = 0; side < 2 && !snubber; ++side) {
                const int end = side ? c1 : c0, mid = side ? c0 : c1;
                if (end != mt1 && end != mt2) continue;
                const int other = end == mt1 ? mt2 : mt1;
                for (const auto& r : sch.components())
                    if (r.kind == ComponentKind::Resistor && touchesNet(sch, r, mid) && touchesNet(sch, r, other)) snubber = true;
            }
        }
        if (!snubber) {
            add(Severity::Warning, "REL_TRIAC_SNUBBER",
                t->ref + " has no RC snubber across MT1–MT2: an inductive load (pump, valve, motor) turns it back on "
                "with dV/dt at turn-off. Fit ~39 Ω + 10 nF X2 across the triac.",
                {t->id});
            break;
        }
    }
    if (!parts.triacs.empty() && parts.optoTriacs.empty())
        add(Severity::Info, "REL_TRIAC_DRIVER",
            "Triac gates driven without an opto-triac: a MOC302x / MOC306x keeps the logic off the mains-referenced gate.");
    if (!parts.triacs.empty() && parts.zeroCross.empty() && netsNamed(sch, {"ZERO_CROSS", "ZC", "ZCD"}).empty())
        add(Severity::Warning, "REL_ZERO_CROSS",
            "AC loads switched without zero-crossing detection: an AC-input optocoupler (H11AA1) or a resistor divider "
            "into the MCU times triac firing for phase control and switches resistive loads at zero (less EMI, longer "
            "relay and triac life).");
    if (motorAppliance(type) && parts.ipms.empty())
        add(Severity::Warning, "REL_IPM",
            "No intelligent power module for the inverter motor: a variable-speed compressor, drum or fan motor runs "
            "from a three-phase IPM (SLLIMM / SPM) on the DC bus with field-oriented control.");
    if (!parts.ipms.empty() && parts.shunts.empty())
        add(Severity::Warning, "REL_IPM_SHUNT",
            "The IPM's emitters have no shunt resistor: phase-current sensing (one or three low-ohm shunts to the bus "
            "return) is needed for sensorless FOC and over-current trip.");

    // ---------------------------------------------------------------- 3. HMI & sensing
    for (int n : a.touchNets) {
        bool series = false;
        for (const auto& r : sch.components()) series |= r.kind == ComponentKind::Resistor && touchesNet(sch, r, n);
        if (!series) {
            add(Severity::Warning, "REL_TOUCH_SERIES",
                netName(sch, n) + " reaches a touch pad without a series resistor: ~1 kΩ next to the controller filters "
                "RF and ESD from the front panel (IEC 61000-4-2 / -4-6).");
            break;
        }
    }
    if (!parts.touch.empty())
        add(Severity::Info, "REL_TOUCH_GUARD",
            "Surround the touch pads with a hatched (≈ 25 %) ground guard on the pad layer, keep sensor traces short "
            "and away from mains copper, and avoid solid ground under the pads.");
    for (int n : a.ntcNodes) {
        if (!anyTouches(sch, parts.caps, n)) {
            add(Severity::Warning, "REL_NTC_FILTER",
                "NTC divider node " + netName(sch, n) + " has no filter capacitor: a 100 nF RC at the ADC pin stops "
                "triac / motor switching noise reading as temperature spikes.");
            break;
        }
    }
    if (motorAppliance(type) && parts.halls.empty() && netsNamed(sch, {"TACHO", "TACH", "SPEED", "HALL"}).empty())
        add(Severity::Info, "REL_SPEED_SENSOR",
            "No speed / position sensing (hall switch or tacho) for the motor: drum and fan speed feedback detects "
            "jams and imbalance.");

    // ---------------------------------------------------------------- 4. IoT connectivity
    if (a.modules.empty()) {
        if (type != "small")
            add(Severity::Info, "REL_WIFI_MODULE",
                "No Wi-Fi / BLE module: connected appliances use a pre-certified module (ESP32-WROOM) for remote "
                "control and energy reporting (Matter / cloud).");
    } else {
        const auto pads = pcb.pads(sch);
        for (const Component* m : a.modules) {
            if (!m->hasFootprint() || !m->pcb.placed) continue;
            const Rect body = pcb.courtyard(*m);
            const double edge = coaxEdgeGap(pcb, *m);
            if (edge > 3.0)
                add(Severity::Warning, "REL_ANTENNA_EDGE",
                    m->ref + "'s antenna end is " + fmt("%.1f mm", edge) + " inside the board: put the module at the edge "
                    "with its PCB antenna overhanging (or over a copper keep-out) or the metal around it detunes it.",
                    {m->id});
            double nearest = 1e9;
            for (const auto& p : pads)
                if (p.net >= 0 && a.highVoltage.count(p.net)) nearest = std::min(nearest, rectRectDistance(p.bounds(), body));
            for (const auto& t : pcb.tracks)
                if (a.highVoltage.count(t.net)) nearest = std::min(nearest, trackRectDistance(t, body) - t.width / 2);
            if (nearest < 6.0)
                add(Severity::Warning, "REL_ANTENNA_MAINS",
                    m->ref + " is " + fmt("%.1f mm", std::max(0.0, nearest)) + " from mains copper: keep the radio and its "
                    "antenna ≥ 6 mm from high-voltage copper (creepage, and mains switching noise couples into the "
                    "antenna).",
                    {m->id});
        }
    }
    return out;
}
}  // namespace

const std::vector<ApplianceType>& applianceTypes() {
    static const std::vector<ApplianceType> t = {
        {"laundry", "Laundry (washer / dryer)", "Inverter drum motor, drain pump, door lock, heater and water valves",
         {"IPM inverter for the drum motor with shunt sensing, triacs with snubbers for pump and valves, zero-cross timing.",
          "Hall / tacho speed feedback; conformal coat against condensation."}},
        {"kitchen", "Kitchen & cooking", "Ovens, hobs, dishwashers and coffee machines",
         {"Heater triacs or relays with zero-cross switching, NTC sensing with RC filters, capacitive touch panel."}},
        {"refrigeration", "Refrigeration", "Inverter fridge / freezer compressor controllers",
         {"Inverter compressor IPM, defrost heater triac, multiple NTC probes, door switch."}},
        {"hvac", "HVAC / air conditioning", "Indoor / outdoor units and heat pumps",
         {"PFC + inverter compressor and fan IPMs, louvre steppers, NTC coil and room sensors, Wi-Fi control."}},
        {"small", "Small appliances", "Kettles, fans, blenders and small kitchen gadgets",
         {"Off-line buck, one triac or relay, touch keys; IoT optional."}},
    };
    return t;
}

const ApplianceType* findApplianceType(const std::string& id) {
    for (const auto& t : applianceTypes())
        if (t.id == id) return &t;
    return nullptr;
}

bool isApplianceProject(const Project& project) {
    return !project.applianceType.empty() || project.industry == "appliance";
}

std::vector<RuleViolation> applianceChecks(const Project& project) { return checks(project, !project.applianceType.empty()); }

std::vector<RobotSegment> applianceSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const std::string type = project.applianceType;
    const Analysis a = analyse(project);
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
    const bool mains = !a.mains.empty();
    std::vector<RobotSegment> out;

    RobotSegment pwr{"mains", "AC Mains Entry & Power Conversion", "", {}, {}};
    pwr.items.push_back(item("Fuse + varistor", mains && !warn.count("REL_MAINS_FUSE") && !warn.count("REL_MAINS_MOV"),
                             refs(parts.fuses) + " + " + refs(parts.movs), "Slow-blow fuse and 275 VAC MOV"));
    pwr.items.push_back(item("EMI filter (X cap + CM choke)", mains && !warn.count("REL_X_CAP") && !warn.count("REL_CM_CHOKE"),
                             refs(a.xCaps) + " + " + refs(parts.cmChokes), "X2 capacitor and common-mode choke"));
    pwr.items.push_back(item("Off-line converter", !parts.offline.empty(), refs(parts.offline) + " non-isolated buck",
                             "LinkSwitch-TN / VIPer buck"));
    pwr.guidance = {"IEC 60335-1: fuse, MOV, X / Y capacitors and a common-mode choke at the mains entry; creepage per "
                    "IEC 60664-1 between mains and SELV; the layout keeps the voltage spacing on every high-voltage net."};
    out.push_back(pwr);

    RobotSegment act{"actuation", "High-Voltage Actuation & Motor Control", "", {}, {}};
    act.items.push_back(item("Triacs / SSRs with snubbers", !parts.triacs.empty() && !warn.count("REL_TRIAC_SNUBBER"),
                             refs(parts.triacs) + " with RC snubbers", "Triac or SSR with an RC snubber"));
    act.items.push_back(item("Zero-crossing detection", !warn.count("REL_ZERO_CROSS") && (!parts.zeroCross.empty() ||
                                                                                       !netsNamed(sch, {"ZERO_CROSS"}).empty()),
                             refs(parts.zeroCross) + " → MCU", "H11AA1 / divider zero-cross input"));
    act.items.push_back(item("Inverter IPM", motorAppliance(type) ? !warn.count("REL_IPM") && !warn.count("REL_IPM_SHUNT")
                                                                  : !parts.ipms.empty(),
                             parts.ipms.empty() ? "No inverter motor" : refs(parts.ipms) + " + shunts",
                             "Three-phase IPM with shunt sensing"));
    act.guidance = {"Heaters, pumps and valves on triacs fired at the zero crossing with RC snubbers; inverter motors on a "
                    "three-phase IPM with bootstrap supplies and shunt current sensing."};
    out.push_back(act);

    RobotSegment hmi{"sensing", "HMI & Sensing", "", {}, {}};
    hmi.items.push_back(item("Capacitive touch", !parts.touch.empty() && !warn.count("REL_TOUCH_SERIES"),
                             refs(parts.touch) + " with series resistors", "Touch controller with ~1 kΩ series resistors"));
    hmi.items.push_back(item("NTC sensing with RC filters", !a.ntcs.empty() && !warn.count("REL_NTC_FILTER"),
                             refs(a.ntcs) + " dividers filtered", "NTC divider + 100 nF at the ADC"));
    hmi.items.push_back(item("Speed / position sensing", !parts.halls.empty() || !netsNamed(sch, {"TACHO", "TACH"}).empty(),
                             refs(parts.halls), "Hall switch or tacho input"));
    hmi.guidance = {"Touch pads behind glass with a hatched guard, NTC probes with RC filters, hall / tacho feedback for "
                    "drum and fan speed."};
    out.push_back(hmi);

    RobotSegment iot{"iot", "IoT Connectivity", "", {}, {}};
    iot.items.push_back(item("Wi-Fi / BLE module", !a.modules.empty(), refs(a.modules), "ESP32-WROOM module"));
    iot.items.push_back(item("Antenna at the board edge", !a.modules.empty() && !warn.count("REL_ANTENNA_EDGE"),
                             "Antenna end at the edge, keep-out below", "Module at the edge, antenna outward"));
    iot.items.push_back(item("HV clearance to the antenna", !a.modules.empty() && !warn.count("REL_ANTENNA_MAINS"),
                             "≥ 6 mm from mains copper", "Keep the module ≥ 6 mm from mains copper"));
    iot.guidance = {"A pre-certified module with its PCB (MIFA) antenna at the board edge, no copper under the antenna "
                    "and well clear of mains copper."};
    out.push_back(iot);

    scoreSegments(out);
    return out;
}

Json applianceSegmentsJson(const Project& project) {
    std::vector<PlatformInfo> types;
    for (const auto& t : applianceTypes()) types.push_back({t.id, t.name, t.description, t.guidance});
    return segmentReportJson(project.applianceType, isApplianceProject(project), types, applianceSegments(project));
}

}  // namespace sieda
