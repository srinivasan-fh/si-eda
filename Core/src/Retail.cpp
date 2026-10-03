#include "sieda/Retail.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "SystemParts.hpp"
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
/// Nets with at least two pins whose name contains one of the needles.
std::vector<int> wiredNets(const Schematic& sch, std::initializer_list<const char*> needles) {
    std::vector<int> out;
    for (int n : netsNamed(sch, needles))
        if (sch.nets()[static_cast<size_t>(n)].pins.size() >= 2) out.push_back(n);
    return out;
}
/// Nets that leave the enclosure through a customer- or staff-facing port (USB, drawer, card slot, display,
/// serial, Ethernet, DC input).
bool exteriorNet(const Schematic& sch, int net) {
    return containsAny(up(netName(sch, net)), {"USB", "VBUS", "DRAWER", "KICK", "CARD", "SC_", "CIO", "CCLK", "CRST",
                                               "CVCC", "CAUX", "PRES", "LVDS", "EDP", "HDMI", "RS232", "TXD", "RXD",
                                               "DC_IN", "VIN", "ETH", "RJ"});
}
bool paymentDevice(const std::string& cls) { return cls != "printer"; }
bool exposedDevice(const std::string& cls) { return cls == "unattended" || cls == "kiosk" || cls == "mpos"; }

struct Analysis {
    Parts parts;
    std::vector<const Component*> meshed;  // secure elements with a tamper mesh laid over them
    std::vector<std::string> meshErrors;
    std::vector<int> tamperNets;           // tamper-switch / zeroize nets (mesh nets excluded)
    bool keyBattery = false;               // a secure element's VBAT pin is on a battery
    std::vector<int> solenoidNets;         // cutter / drawer / solenoid coils
    std::vector<int> drawerNets;
    std::vector<int> displayNets;
    int usbPorts = 0;
};

Analysis analyse(const Project& project) {
    const Schematic& sch = project.schematic;
    Analysis a;
    a.parts = classify(sch);
    std::set<int> meshNets;
    const auto geo = project.pcb.tamperMeshGeometry(sch, project.pcb.pads(sch));
    for (const auto& g : geo) {
        const TamperMesh& tm = project.pcb.tamperMeshes[static_cast<size_t>(g.mesh)];
        const Component* part = sch.findByRef(tm.componentRef);
        if (!g.error.empty() && part && part->hasFootprint() && part->pcb.placed)  // laid out only once placed
            a.meshErrors.push_back(tm.componentRef + ": " + g.error);
    }
    // A mesh declared over a secure element covers it (the autorouter lays it once the part is placed).
    for (const auto& tm : project.pcb.tamperMeshes)
        for (const Component* se : a.parts.secureElements)
            if (se->ref == tm.componentRef && std::find(a.meshed.begin(), a.meshed.end(), se) == a.meshed.end())
                a.meshed.push_back(se);
    // A mesh declared but not yet placed (geometry needs placement): count it by its nets on the secure element.
    for (const auto& tm : project.pcb.tamperMeshes)
        for (const auto& n : sch.nets())
            if (n.name == tm.netA || n.name == tm.netB) meshNets.insert(n.index);
    for (int n : wiredNets(sch, {"TAMPER", "ZEROIZE", "CASE_OPEN", "INTRUSION"}))
        if (!meshNets.count(n)) a.tamperNets.push_back(n);
    for (const Component* se : a.parts.secureElements)
        for (int i = 0; i < static_cast<int>(se->def().pins.size()); ++i) {
            if (pinName(*se, i) != "VBAT") continue;
            const int net = sch.netOf({se->id, i});
            if (net < 0) continue;
            for (const auto& c : sch.components())
                if (c.kind == ComponentKind::Battery && touchesNet(sch, c, net)) a.keyBattery = true;
            if (containsAny(up(netName(sch, net)), {"COIN", "BACKUP", "VBAT_KEY", "CR20"})) a.keyBattery = true;
        }
    a.solenoidNets = wiredNets(sch, {"SOLENOID", "CUTTER", "KICK", "DRAWER_SOL"});
    a.drawerNets = wiredNets(sch, {"DRAWER", "KICK"});
    a.displayNets = wiredNets(sch, {"LVDS", "EDP", "HDMI", "TMDS", "DSI"});
    for (const Component* j : a.parts.connectors)
        if (containsAny(partName(*j) + " " + up(j->value), {"USB"})) ++a.usbPorts;
    return a;
}

/// strict: full severities (a device class is set, or the segment checklist); otherwise everything is Info.
std::vector<RuleViolation> checks(const Project& project, bool strict) {
    std::vector<RuleViolation> out;
    if (!isRetailProject(project)) return out;
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    const std::string cls = project.retailDevice;
    const Analysis a = analyse(project);
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

    // ---------------------------------------------------------------- 1. payment security & anti-tamper
    if (paymentDevice(cls)) {
        if (parts.secureElements.empty())
            add(Severity::Warning, "REL_SECURE_ELEMENT",
                "No payment secure element: PIN entry and card data need a secure MCU / element (MAX32550 class, "
                "SE050) whose battery-backed keys are erased the instant the case is opened (PCI PTS POI).");
        for (const Component* se : parts.secureElements) {
            if (std::find(a.meshed.begin(), a.meshed.end(), se) != a.meshed.end()) continue;
            add(Severity::Warning, "REL_TAMPER_MESH",
                se->ref + " has no active tamper mesh: lay serpentine mesh traces over it on two inner layers (Board "
                "Setup → Protection → Tamper mesh) so drilling or probing toward the secure area cuts a mesh line.",
                {se->id});
            break;
        }
        for (const auto& e : a.meshErrors) add(Severity::Warning, "REL_TAMPER_MESH", "Tamper mesh " + e + ".");
        if (!parts.secureElements.empty()) {
            if (a.tamperNets.empty())
                add(Severity::Warning, "REL_TAMPER_SWITCHES",
                    "No tamper switches: case-open / removal switches (carbon-pill domes under the keypad, PCB "
                    "switches against the housing) on the secure element's TAMPER inputs zeroize the keys.");
            if (!a.keyBattery)
                add(Severity::Warning, "REL_KEY_BATTERY",
                    "The secure element's VBAT is not on a backup cell: keys and tamper detection must stay alive "
                    "with mains off (coin cell / supercap on VBAT), or the device can be opened unpowered.");
        }
        if (parts.cardAfes.empty() && cls != "kiosk")
            add(Severity::Info, "REL_CARD_AFE",
                "No EMV contact (TDA8035 / NCN8025) or magstripe front end on this board: card data then enters "
                "through a separate certified reader module.");
    }

    // ---------------------------------------------------------------- 2. printer mechanism drivers
    for (const Component* head : parts.printHeads) {
        bool bulk = false;
        for (int i = 0; i < static_cast<int>(head->def().pins.size()) && !bulk; ++i) {
            const auto& pin = head->def().pins[static_cast<size_t>(i)];
            if (pin.type != static_cast<int>(PinType::PowerIn)) continue;
            const int net = sch.netOf({head->id, i});
            if (net < 0 || net == gnd || sch.netRole(net) == NetRole::Ground) continue;
            for (const Component* c : parts.caps)
                if (touchesNet(sch, *c, net))
                    if (auto v = parseEngineeringValue(primaryValue(c->value)); v && *v >= 470e-6) bulk = true;
        }
        if (!bulk) {
            add(Severity::Warning, "REL_TPH_BULK",
                head->ref + ": the print-head supply has no bulk capacitor (≥ 470 µF low-ESR next to the head). Firing a "
                "full dot line draws several amps in microseconds; without it the supply sags and the logic resets.",
                {head->id});
            break;
        }
    }
    if (cls == "printer" || !parts.printHeads.empty()) {
        if (parts.drivers.empty())
            add(Severity::Warning, "REL_PRINTER_MOTOR",
                "No motor driver for the paper-feed stepper: use an H-bridge / stepper driver (DRV8833, DRV8825) with "
                "current limiting rather than discrete transistors.");
        for (const Component* d : parts.drivers) {
            int pad = -1;
            for (int i = 0; i < static_cast<int>(d->def().pins.size()); ++i)
                if (containsAny(pinName(*d, i), {"EP", "PAD", "PPAD"})) pad = sch.netOf({d->id, i});
            if (pad < 0) continue;
            if (!project.pcb.isZoneNet(sch, pad)) {
                add(Severity::Warning, "REL_DRIVER_THERMAL",
                    d->ref + ": the PowerPAD is not on a copper pour — the stepper driver sheds its heat through the "
                    "exposed pad into a ground pour with thermal vias to the inner / bottom planes.",
                    {d->id});
                break;
            }
        }
    }
    for (int net : a.solenoidNets) {
        // The coil side: a MOSFET drain (or a driver output) on the net, not its gate drive.
        bool switched = anyTouches(sch, parts.drivers, net);
        for (const Component* q : parts.powerFets)
            switched |= q->kind == ComponentKind::NMOS ? sch.netOf({q->id, 1}) == net : touchesNet(sch, *q, net);
        if (!switched) continue;
        if (!anyTouches(sch, parts.diodes, net) && !anyTouches(sch, parts.tvs, net)) {
            add(Severity::Warning, "REL_SOLENOID_FLYBACK",
                netName(sch, net) + " switches a coil (cutter / drawer solenoid) without a flyback diode: the turn-off "
                "spike avalanches the MOSFET and couples into the logic.");
            break;
        }
    }

    // ---------------------------------------------------------------- 3. HMI & peripheral expansion
    if (a.usbPorts >= 2 && parts.usbHubs.empty())
        add(Severity::Warning, "REL_USB_HUB",
            std::to_string(a.usbPorts) + " USB ports without a hub controller: a single host port fans out to scanner, "
            "printer and customer display through a USB hub (USB2514 / USB251x) with per-port power switching.");
    if (!a.drawerNets.empty()) {
        bool isolated = false;
        for (int net : a.drawerNets) isolated |= anyTouches(sch, parts.isolators, net);
        if (!isolated)
            add(Severity::Warning, "REL_CASH_DRAWER",
                "The 24 V cash-drawer kick is not optically isolated from the logic: drive the MOSFET through an "
                "optocoupler (PC817) so the solenoid's ground bounce and the drawer cable's ESD stay off the MCU.");
    }
    if (!a.displayNets.empty() && std::fabs(s.differentialImpedance - 100.0) > 10.0)
        add(Severity::Warning, "REL_DISPLAY_PAIRS",
            "LVDS / eDP / HDMI display lanes need 100 Ω differential pairs; the board target is " +
                fmt("%.0f Ω", s.differentialImpedance) + " (Board Setup → Stack-up).");

    // ---------------------------------------------------------------- 4. ESD & environmental robustness
    {
        std::vector<const Component*> clamps = parts.tvs;
        clamps.insert(clamps.end(), parts.diodes.begin(), parts.diodes.end());
        for (const Component* j : parts.connectors) {
            int badNet = -1;
            for (int i = 0; i < static_cast<int>(j->def().pins.size()) && badNet < 0; ++i) {
                const int net = sch.netOf({j->id, i});
                if (net < 0 || net == gnd || sch.netRole(net) == NetRole::Ground) continue;
                if (sch.nets()[static_cast<size_t>(net)].pins.size() < 2) continue;
                if (!exteriorNet(sch, net)) continue;  // internal harness (print head, motor, tamper switches)
                if (!anyTouches(sch, clamps, net) && std::find(parts.coax.begin(), parts.coax.end(), j) == parts.coax.end())
                    badNet = net;
            }
            if (badNet >= 0) {
                add(Severity::Warning, "REL_PORT_ESD",
                    j->ref + " pin net " + netName(sch, badNet) + " has no TVS: every customer-facing port (USB, RJ-11 "
                    "drawer, RS-232, card slot) needs a clamp at the connector for ±15 kV air discharge (IEC 61000-4-2 "
                    "level 4) from staff and shoppers.",
                    {j->id});
                break;
            }
        }
    }
    if (!s.coated())
        add(exposedDevice(cls) ? Severity::Warning : Severity::Info, "REL_RETAIL_COATING",
            "No conformal coating: spilled drinks, cleaning sprays and forecourt humidity reach the board — an acrylic "
            "(AR) coat keeps the keypad matrix and card-slot electronics from leaking and corroding.");
    return out;
}
}  // namespace

const std::vector<RetailDevice>& retailDevices() {
    static const std::vector<RetailDevice> d = {
        {"countertop", "Countertop payment terminal", "Attended PIN pad / card terminal at the checkout (PCI PTS POI)",
         {"Secure element under an active tamper mesh, tamper switches and battery-backed keys erased on intrusion.",
          "EMV contact + contactless and magstripe front ends; TVS on every port."}},
        {"unattended", "Unattended payment terminal", "Vending, fuel-dispenser and parking terminals (PCI PTS UPT)",
         {"Everything a countertop terminal needs, plus conformal coating and wide-temperature parts for the outdoors.",
          "Anti-skimming: card slot electronics under the mesh, intrusion switches on the enclosure."}},
        {"mpos", "Mobile POS", "Battery-powered handheld card reader paired to a phone or tablet",
         {"Secure element with mesh and coin-cell / supercap backup; BLE link; charger protection.",
          "Coating against handling sweat and drops; ESD on the charge port."}},
        {"kiosk", "Self-service kiosk", "Self-checkout, ticketing and ordering kiosks with display and peripherals",
         {"USB hub fans out to scanner, printer and payment module; LVDS / eDP / HDMI display links at 100 Ω.",
          "24 V cash drawer and peripherals behind isolation; ESD on every exposed port."}},
        {"printer", "Receipt / label printer", "Thermal receipt printer mechanism controller with auto-cutter",
         {"Bulk capacitance at the print-head supply, H-bridge stepper driver on a thermal pad, flyback on the cutter.",
          "ESD on the host interface; head temperature and paper-out sensing."}},
    };
    return d;
}

const RetailDevice* findRetailDevice(const std::string& id) {
    for (const auto& d : retailDevices())
        if (d.id == id) return &d;
    return nullptr;
}

bool isRetailProject(const Project& project) { return !project.retailDevice.empty() || project.industry == "retail"; }

std::vector<RuleViolation> retailChecks(const Project& project) { return checks(project, !project.retailDevice.empty()); }

std::vector<RobotSegment> retailSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    const std::string cls = project.retailDevice;
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
    const bool se = !parts.secureElements.empty();
    std::vector<RobotSegment> out;

    RobotSegment sec{"security", "Payment Security & Anti-Tamper", "", {}, {}};
    sec.items.push_back(item("Secure element", se, refs(parts.secureElements), "Payment secure MCU / element"));
    sec.items.push_back(item("Active tamper mesh (inner layers)", se && !warn.count("REL_TAMPER_MESH"),
                             "Mesh over " + refs(a.meshed) + " on two inner layers", "Tamper mesh over the secure element"));
    sec.items.push_back(item("Tamper switches & zeroization", se && !warn.count("REL_TAMPER_SWITCHES") && !warn.count("REL_KEY_BATTERY"),
                             "Tamper inputs wired; keys on a backup cell", "Tamper switches and battery-backed key store"));
    sec.items.push_back(item("Card reader front ends", !parts.cardAfes.empty() || cls == "printer" || cls == "kiosk",
                             parts.cardAfes.empty() ? "Card reading in a separate module" : refs(parts.cardAfes),
                             "EMV contact / magstripe AFE"));
    sec.guidance = {"PCI PTS POI: any attempt to open, drill or probe the secure area must erase the keys — mesh lines on "
                    "inner layers over the secure element, no vias through it, switches under the case, keys alive on a "
                    "backup cell."};
    out.push_back(sec);

    RobotSegment prn{"printer", "Printer Mechanism Drivers", "", {}, {}};
    const bool printer = cls == "printer" || !parts.printHeads.empty();
    prn.items.push_back(item("Print-head bulk capacitance", !parts.printHeads.empty() && !warn.count("REL_TPH_BULK"),
                             refs(parts.printHeads) + " supply with ≥ 470 µF", printer ? "≥ 470 µF at the head supply" : "No print head on this board"));
    prn.items.push_back(item("Stepper H-bridge on a thermal pad", printer && !parts.drivers.empty() && !warn.count("REL_DRIVER_THERMAL") &&
                                                                    !warn.count("REL_PRINTER_MOTOR"),
                             refs(parts.drivers) + " PowerPAD on the ground pour", printer ? "DRV8833 / DRV8825 with thermal vias" : "No paper-feed motor"));
    prn.items.push_back(item("Cutter / solenoid flyback", !a.solenoidNets.empty() && !warn.count("REL_SOLENOID_FLYBACK"),
                             "Flyback diode across the coil", a.solenoidNets.empty() ? "No cutter / solenoid" : "Diode across the cutter coil"));
    prn.guidance = {"The head fires amps in microseconds: bulk + ceramic at the head, a current-limited stepper driver "
                    "sinking its heat into a pour, and every coil clamped."};
    out.push_back(prn);

    RobotSegment hmi{"hmi", "HMI & Peripheral Expansion", "", {}, {}};
    hmi.items.push_back(item("USB hub", a.usbPorts >= 2 ? !warn.count("REL_USB_HUB") : !parts.usbHubs.empty(),
                             parts.usbHubs.empty() ? std::to_string(a.usbPorts) + " USB port(s)" : refs(parts.usbHubs),
                             a.usbPorts >= 2 ? "USB2514 / USB251x hub" : "USB hub for scanner / printer / display"));
    hmi.items.push_back(item("Isolated cash-drawer kick", !a.drawerNets.empty() && !warn.count("REL_CASH_DRAWER"),
                             "Opto-isolated 24 V MOSFET kick", a.drawerNets.empty() ? "No cash drawer port" : "Optocoupler + MOSFET"));
    hmi.items.push_back(item("Display links at 100 Ω", !a.displayNets.empty() && !warn.count("REL_DISPLAY_PAIRS"),
                             refs(parts.displayLinks) + " pairs at " + fmt("%.0f Ω", s.differentialImpedance),
                             a.displayNets.empty() ? "No LVDS / eDP / HDMI display" : "100 Ω differential pairs"));
    hmi.guidance = {"Peripherals hang off a powered USB hub; the drawer solenoid sits behind an optocoupler; display "
                    "lanes are 100 Ω pairs, length matched."};
    out.push_back(hmi);

    RobotSegment esd{"esd", "ESD & Environmental Robustness", "", {}, {}};
    esd.items.push_back(item("TVS on every port", !parts.connectors.empty() && !warn.count("REL_PORT_ESD"), "Every exterior pin clamped",
                             "TVS / ESD arrays at each connector"));
    esd.items.push_back(item("Conformal coating", s.coated(), s.coating + " coating", "Acrylic (AR) conformal coating"));
    esd.guidance = {"Retail hardware lives with static, spills and cleaning sprays: ±15 kV air discharge on every port "
                    "(IEC 61000-4-2 level 4) and an acrylic conformal coat."};
    out.push_back(esd);

    scoreSegments(out);
    return out;
}

Json retailSegmentsJson(const Project& project) {
    std::vector<PlatformInfo> devices;
    for (const auto& d : retailDevices()) devices.push_back({d.id, d.name, d.description, d.guidance});
    return segmentReportJson(project.retailDevice, isRetailProject(project), devices, retailSegments(project));
}

}  // namespace sieda
