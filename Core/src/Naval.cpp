#include "sieda/Naval.hpp"

#include <algorithm>
#include <cmath>
#include <set>

#include "SystemParts.hpp"
#include "sieda/Aerospace.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Project.hpp"
#include "sieda/Stackup.hpp"

namespace sieda {

namespace {
using namespace sysparts;

bool onNet(const Schematic& sch, const Component& c, int net) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        if (sch.netOf({c.id, i}) == net) return true;
    return false;
}

/// The net of a part's first pin named one of `names`, or -1.
int namedPinNet(const Schematic& sch, const Component& c, std::initializer_list<const char*> names) {
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
        for (const char* n : names)
            if (pinName(c, i) == n) return sch.netOf({c.id, i});
    return -1;
}

/// Parts heavy or large enough to need underfill / corner bonding under shock.
std::vector<const Component*> heavyParts(const Schematic& sch) {
    std::vector<const Component*> out;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint()) continue;
        bool heavy = c.def().pins.size() >= 28;
        if (c.kind == ComponentKind::Capacitor)
            if (auto v = parseEngineeringValue(primaryValue(c.value)); v && *v >= 47e-6) heavy = true;
        if (c.kind == ComponentKind::Inductor)
            if (auto v = parseEngineeringValue(primaryValue(c.value)); v && *v >= 10e-6) heavy = true;
        if (heavy) out.push_back(&c);
    }
    return out;
}

/// The analog return nets (AGND, sonar ground) and how many parts tie each to the logic ground.
int starTies(const Schematic& sch, int agnd) {
    const int gnd = sch.groundNet();
    int ties = 0;
    for (const auto& c : sch.components()) {
        if (c.def().pins.size() != 2) continue;
        const int a = sch.netOf({c.id, 0}), b = sch.netOf({c.id, 1});
        if ((a == agnd && b == gnd) || (a == gnd && b == agnd)) ++ties;
    }
    return ties;
}

bool needsFiber(const std::string& p) { return p == "combatant" || p == "carrier" || p == "submarine"; }
bool needsRadar(const std::string& p) { return p == "combatant" || p == "carrier"; }
bool needsSonar(const std::string& p) { return p == "combatant" || p == "submarine"; }

/// strict: full severities (a platform is set, or the segment checklist); otherwise everything is reported as Info.
std::vector<RuleViolation> checks(const Project& project, bool strict) {
    std::vector<RuleViolation> out;
    if (!isNavalProject(project)) return out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const Parts parts = classify(sch);
    const int gnd = sch.groundNet();
    const bool other = isRobotProject(project) || isEcuProject(project) || isAerospaceProject(project);
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = strict ? sev : Severity::Info;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        out.push_back(v);
    };

    // ---------------------------------------------------------------- 1. power filtration & galvanic isolation
    if (!parts.sources.empty()) {
        bool isolated = !parts.isoDcdc.empty();
        for (const Component* src : parts.sources)
            if (sch.netOf({src->id, 1}) == gnd) isolated = false;
        if (!isolated && !other)
            add(Severity::Warning, "REL_SHIP_ISOLATION",
                "Ship's power shares ground with the logic: weapon and propulsion transients and hull ground currents "
                "flow through the processor. Use an isolated forward / flyback DC-DC and keep the ship's return (RTN) "
                "separate from the logic ground.");
        if (parts.movs.empty() && parts.gdts.empty())
            add(Severity::Warning, "REL_SURGE_FRONT_END",
                "No surge front end on the ship's power input: MIL-STD-1399 spikes and weapon / generator switching "
                "surges need a high-energy varistor (MOV) and a gas discharge tube at the power entry, ahead of the TVS.");
        else if (parts.gdts.empty() || parts.movs.empty())
            add(Severity::Info, "REL_SURGE_FRONT_END",
                std::string("The surge front end has ") + (parts.gdts.empty() ? "no gas discharge tube" : "no varistor") +
                    ": combine a GDT (energy) with a MOV (speed) at the power entry.");
        bool ac = false;
        for (const Component* src : parts.sources) ac |= src->kind == ComponentKind::ACSource;
        if (ac && parts.pfc.empty())
            add(Severity::Warning, "REL_PFC",
                "AC ship's power without power-factor correction: an active PFC boost stage (UCC28019, L6562) keeps the "
                "harmonic currents returned to the ship's grid within MIL-STD-1399 Section 300 limits.");
        if (!other && !hasPiFilter(sch, parts))
            add(Severity::Warning, "REL_EMI_FILTER",
                "No EMI filter on the power input: a common-mode choke and C–L–C pi filter keep the board inside "
                "MIL-STD-461 CE102 conducted-emission limits.");
    }

    // ---------------------------------------------------------------- 2. corrosion-resistant hermetic compute
    if (!(s.coating == "silicone" || s.coating == "parylene" || s.coating == "urethane"))
        add(Severity::Warning, "REL_SALT_FOG_COATING",
            std::string(s.coated() ? "The " + s.coating + " coating" : "An uncoated board") +
                " will not survive salt fog (MIL-STD-810 Method 509): salt and condensation form an electrolyte that "
                "dissolves traces in days. Specify a thick military-grade silicone or Parylene-C coating (Board Setup → "
                "Protection).");
    {
        const double needed = s.coated() ? 0.25 : 0.30;
        if (s.clearance < needed - 1e-9)
            add(Severity::Warning, "REL_ECM_SPACING",
                fmt("%.2f mm", s.clearance) + " copper spacing invites electrochemical migration: under condensation, "
                "metal dendrites grow between biased conductors. Keep ≥ " + fmt("%.2f mm", needed) +
                " (beyond IPC-2221B Class 3) on every board.");
    }
    if (!parts.compute.empty()) {
        std::vector<const Component*> plastic;
        for (const Component* c : parts.compute)
            if (std::find(parts.hermetic.begin(), parts.hermetic.end(), c) == parts.hermetic.end()) plastic.push_back(c);
        if (!plastic.empty())
            add(Severity::Warning, "REL_HERMETIC",
                plastic.front()->ref + " is in a plastic package: moisture diffuses through the mould compound over years "
                "at sea. Use ceramic hermetic (QML / SMD 5962) parts for the critical ICs.",
                {plastic.front()->id});
    }
    const auto pads = pcb.pads(sch);
    const bool laidOut = !pads.empty();
    if (laidOut) {
        // Coating anchor zone: no copper (except edge connectors) within 2 mm of the board edge.
        for (const auto& pd : pads) {
            const Component* c = sch.find(pd.componentId);
            if (!c || c->kind == ComponentKind::Connector || std::find(parts.connectors.begin(), parts.connectors.end(), c) !=
                                                                 parts.connectors.end())
                continue;
            const double d = s.edgeDistance(pd.position) - std::max(pd.size.x, pd.size.y) / 2;
            if (d < 2.0) {
                add(Severity::Warning, "REL_COATING_BORDER",
                    c->ref + " has copper " + fmt("%.1f mm", std::max(0.0, d)) + " from the board edge: keep a 2 mm "
                    "copper-free border so the coating anchors and seals the edge (Fit Board margin).",
                    {c->id});
                break;
            }
        }
    }

    // ---------------------------------------------------------------- 3. high-shock mechanical stabilisation
    {
        const auto heavy = heavyParts(sch);
        if (!heavy.empty() && !s.underfill)
            add(Severity::Warning, "REL_SHOCK_UNDERFILL",
                heavy.front()->ref + " and other heavy parts are only held by solder: a MIL-STD-901E near-miss shock tears "
                "them off. Specify underfill / corner bonding for processors, BGAs and large capacitors (Board Setup → "
                "Protection).",
                {heavy.front()->id});
        const auto& holes = s.holes;
        double worst = 0;
        for (size_t i = 0; i < holes.size(); ++i) {
            double nearest = 1e9;
            for (size_t j = 0; j < holes.size(); ++j)
                if (i != j) nearest = std::min(nearest, (holes[i].position - holes[j].position).length());
            worst = std::max(worst, nearest);
        }
        if (holes.size() < 4 || worst > 75.0)
            add(Severity::Warning, "REL_SHOCK_MOUNTING",
                holes.size() < 4 ? "Fewer than four mounting holes: the board resonates and flexes under hull pounding. Add "
                                   "plated, via-stitched mounting holes at most 75 mm apart."
                                 : "Mounting holes are " + fmt("%.0f mm", worst) + " apart: at most 75 mm limits board flex "
                                   "and resonance under engine vibration and shock.");
        const std::string m = s.material;
        const bool rigid = m == "polyimide" || m == "fr4-hightg" || m == "isola-370hr";
        if (s.thickness < 2.0 || s.layerCount < 8 || !rigid)
            add(Severity::Warning, "REL_SHOCK_SUBSTRATE",
                "The board is " + fmt("%.1f mm", s.thickness) + ", " + std::to_string(s.layerCount) + " layers of " +
                    boardLaminate(s).name + ": shock-rated naval boards use 8–14 layers, ≥ 2.0 mm thick polyimide or "
                    "high-Tg FR-4 for rigidity.");
    }

    // ---------------------------------------------------------------- 4. marine data & weapons links
    for (const Component* f : parts.fieldbus) {
        const int g = namedPinNet(sch, *f, {"GND", "VSS", "GND2"});
        if (g >= 0 && g == gnd) {
            add(Severity::Warning, "REL_ISOLATED_BUS",
                f->ref + " (" + partName(*f) + ") shares the logic ground: across a ship the grounds differ by volts to "
                "hundreds of volts. Use a 2.5 kV+ isolated transceiver (or isolator + isolated supply) for CAN / RS-485.",
                {f->id});
            break;
        }
    }
    if (strict && needsFiber(project.navalPlatform) && parts.fiber.empty())
        add(Severity::Warning, "REL_FIBER_LINK",
            "No fibre-optic link: on a warship copper cables act as antennas for the ship's own radar. Route critical "
            "data over fibre (SFP / Ethernet-over-fibre or fibre UART transceivers).");

    // ---------------------------------------------------------------- 5. radar & sonar front ends
    for (const Component* l : parts.lnas) {
        const int in = namedPinNet(sch, *l, {"RFIN", "RF_IN", "IN", "RFI"});
        bool limited = false;
        if (in >= 0)
            for (const Component* d : parts.limiters) limited |= onNet(sch, *d, in);
        if (!limited) {
            add(Severity::Warning, "REL_LNA_LIMITER",
                l->ref + ": the LNA input has no PIN-diode limiter, so the ship's own multi-megawatt radar sweeping past "
                "burns it out. Fit antiparallel PIN limiter diodes (CLA4601, BAP64) right at the input.",
                {l->id});
            break;
        }
    }
    {
        auto sonar = netsNamed(sch, {"HYD", "SONAR", "TRANSDUCER", "XDCR"});
        if (!sonar.empty()) {
            auto agnd = netsNamed(sch, {"AGND", "GNDA", "ANA_GND", "SONAR_GND"});
            if (agnd.empty())
                add(Severity::Warning, "REL_SONAR_GROUND",
                    "Hydrophone inputs share the digital ground: microvolt sonar signals need their own analog return "
                    "(AGND) joined to GND at one star point, shielded from the processors.");
            else if (const int ties = starTies(sch, agnd.front()); ties != 1)
                add(Severity::Warning, "REL_SONAR_GROUND",
                    netName(sch, agnd.front()) + " joins GND at " + std::to_string(ties) + " points: the sonar analog "
                    "return must meet the digital ground at exactly one star point (ferrite or 0 Ω).");
        }
    }
    return out;
}
}  // namespace

const std::vector<NavalPlatform>& navalPlatforms() {
    static const std::vector<NavalPlatform> p = {
        {"combatant", "Surface combatant (destroyer / frigate / battleship)",
         "Phased-array radar, sonar, weapons and combat systems on a ship that takes blast shock",
         {"MIL-STD-901E Grade A shock, MIL-STD-461 RE102 / RS103 near multi-megawatt radars: fibre between compartments, "
          "PIN-limited LNAs, isolated CAN / RS-485.",
          "MIL-STD-1399 Section 300 power: MOV + GDT surge front end, isolated converters, PFC on AC supplies."}},
        {"carrier", "Aircraft carrier / amphibious ship", "Flight-deck radars, catapult and aviation systems, 400 Hz power",
         {"Intense radar fields and 400 Hz / 60 Hz power with large transients; fibre data, hermetic parts, coating.",
          "Salt-fog spray on the flight deck: Parylene-C or silicone coating, 2 mm coating border."}},
        {"submarine", "Submarine", "Sonar arrays, sealed hull, closed atmosphere, extreme shock",
         {"Hydrophone front ends on a separate analog return with a single star point, low-noise amplifiers away from "
          "processors; fibre data links.",
          "Low-outgassing materials and hermetic packages in the sealed atmosphere."}},
        {"patrol", "Patrol / offshore vessel", "Fast craft: wave slamming, spray and wide temperature swings",
         {"High vibration and slamming: underfill, closely spaced mounting holes, thick high-Tg boards.",
          "Isolated RS-485 / CAN to bridge equipment; coating against spray."}},
        {"commercial", "Commercial marine (IEC 60945)", "Bridge, navigation and engine-room equipment",
         {"IEC 60945 vibration, salt mist and EMC; NMEA 0183 / 2000 on isolated interfaces.",
          "Conformal coating and corrosion-resistant finishes (ENIG)."}},
    };
    return p;
}

const NavalPlatform* findNavalPlatform(const std::string& id) {
    for (const auto& p : navalPlatforms())
        if (p.id == id) return &p;
    return nullptr;
}

bool isNavalProject(const Project& project) { return !project.navalPlatform.empty() || project.industry == "marine"; }

std::vector<RuleViolation> navalChecks(const Project& project) { return checks(project, !project.navalPlatform.empty()); }

std::vector<RobotSegment> navalSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const BoardSettings& s = project.pcb.settings;
    const Parts parts = classify(sch);
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
    const std::string platform = project.navalPlatform;
    std::vector<RobotSegment> out;

    RobotSegment pwr{"power", "Heavy-Duty Power Filtration & Galvanic Isolation", "", {}, {}};
    pwr.items.push_back(item("Ship's power input", !parts.sources.empty(), refs(parts.sources), "28 V / 270 V DC or 115 / 440 V AC input"));
    pwr.items.push_back(item("Isolated forward / flyback DC-DC", !parts.isoDcdc.empty() && !warn.count("REL_SHIP_ISOLATION"),
                             refs(parts.isoDcdc), "Isolated converter, separate ship's return"));
    std::vector<const Component*> surge = parts.movs;
    surge.insert(surge.end(), parts.gdts.begin(), parts.gdts.end());
    pwr.items.push_back(item("MIL-STD-1399 surge front end (MOV + GDT)", !parts.movs.empty() && !parts.gdts.empty(), refs(surge),
                             "Varistor and gas discharge tube at the power entry"));
    bool ac = false;
    for (const Component* src : parts.sources) ac |= src->kind == ComponentKind::ACSource;
    pwr.items.push_back(item("Active PFC (AC supplies)", !ac || !parts.pfc.empty(), ac ? refs(parts.pfc) : "DC supply: not needed",
                             "PFC boost controller"));
    pwr.items.push_back(item("EMI filter", !warn.count("REL_EMI_FILTER"), "C–L–C pi filter", "CM choke + pi filter (MIL-STD-461)"));
    pwr.guidance = {"Weapon, launcher and propulsion transients: MOV + GDT at entry, TVS, pi filter, isolated converters; PFC "
                    "on AC supplies."};
    out.push_back(pwr);

    RobotSegment cmp{"compute", "Corrosion-Resistant & Hermetic Compute", "", {}, {}};
    cmp.items.push_back(item("Military-grade coating", !warn.count("REL_SALT_FOG_COATING"), s.coating,
                             "Parylene-C or silicone conformal coating"));
    cmp.items.push_back(item("Coating anchor border", !warn.count("REL_COATING_BORDER"), "2 mm copper-free edge",
                             "Keep copper 2 mm from the edge"));
    std::vector<const Component*> hermeticCpu;
    for (const Component* c : parts.compute)
        if (std::find(parts.hermetic.begin(), parts.hermetic.end(), c) != parts.hermetic.end()) hermeticCpu.push_back(c);
    cmp.items.push_back(item("Hermetic ceramic ICs", !parts.compute.empty() && !warn.count("REL_HERMETIC"), refs(hermeticCpu),
                             "Ceramic (QML / SMD 5962) processor"));
    cmp.items.push_back(item("Anti-dendrite spacing", !warn.count("REL_ECM_SPACING"), fmt("%.2f mm", s.clearance) + " clearance",
                             "Wider than Class 3 spacing"));
    cmp.guidance = {"Salt fog + condensation = electrolyte: thick Parylene-C / silicone, a coating border, hermetic parts and "
                    "wide spacing against dendrites."};
    out.push_back(cmp);

    RobotSegment mech{"mechanical", "High-Shock & Vibration Stabilization", "", {}, {}};
    mech.items.push_back(item("Underfill / corner bonding", !warn.count("REL_SHOCK_UNDERFILL"), "Specified for heavy parts",
                              "Underfill processors, BGAs and large capacitors"));
    mech.items.push_back(item("Close-spaced stitched mounting holes", !warn.count("REL_SHOCK_MOUNTING"),
                              std::to_string(s.holes.size()) + " holes ≤ 75 mm apart", "≥ 4 holes, ≤ 75 mm apart"));
    mech.items.push_back(item("Thick rigid substrate", !warn.count("REL_SHOCK_SUBSTRATE"),
                              fmt("%.1f mm", s.thickness) + ", " + std::to_string(s.layerCount) + " layers, " +
                                  boardLaminate(s).name,
                              "8–14 layers, ≥ 2.0 mm polyimide / high-Tg FR-4"));
    mech.guidance = {"MIL-STD-901E near-miss shock and MIL-STD-167 vibration: underfill, closely spaced mounting, thick "
                     "multilayer polyimide."};
    out.push_back(mech);

    RobotSegment com{"comms", "Ruggedized Marine Data & Weapons Communication", "", {}, {}};
    com.items.push_back(item("Fibre-optic link", !parts.fiber.empty() || !needsFiber(platform),
                             parts.fiber.empty() ? "Not needed on this platform" : refs(parts.fiber),
                             "Fibre transceivers for radar-immune data"));
    com.items.push_back(item("Isolated CAN / RS-485 (2.5 kV+)", !parts.fieldbus.empty() && !warn.count("REL_ISOLATED_BUS"),
                             refs(parts.fieldbus) + " on an isolated domain", "Isolated transceiver"));
    com.items.push_back(item("NTDS (legacy)", true, parts.ntds.empty() ? "Not used" : refs(parts.ntds), ""));
    com.guidance = {"Fibre between compartments, galvanically isolated CAN / RS-485 for short runs, NTDS (MIL-STD-1397) "
                    "drivers for legacy combat systems."};
    out.push_back(com);

    RobotSegment rf{"rfsonar", "High-Power Radar & Sonar RF", "", {}, {}};
    rf.items.push_back(item("LNA with PIN limiter", parts.lnas.empty() ? !needsRadar(platform) : !warn.count("REL_LNA_LIMITER"),
                            parts.lnas.empty() ? "No radar receiver on this board" : refs(parts.lnas) + " behind " + refs(parts.limiters),
                            "Low-noise amplifier protected by PIN limiter diodes"));
    const bool sonar = !netsNamed(sch, {"HYD", "SONAR", "TRANSDUCER", "XDCR"}).empty();
    rf.items.push_back(item("Isolated sonar analog return", sonar ? !warn.count("REL_SONAR_GROUND") : !needsSonar(platform),
                            sonar ? "AGND with a single star point" : "No sonar inputs on this board",
                            "Hydrophone front end on its own analog ground"));
    rf.guidance = {"Limiters ahead of every LNA, shielded and separately returned hydrophone preamplifiers, fibre out of the "
                   "RF compartment."};
    out.push_back(rf);

    scoreSegments(out);
    return out;
}

Json navalSegmentsJson(const Project& project) {
    std::vector<PlatformInfo> platforms;
    for (const auto& p : navalPlatforms()) platforms.push_back({p.id, p.name, p.description, p.guidance});
    return segmentReportJson(project.navalPlatform, isNavalProject(project), platforms, navalSegments(project));
}

}  // namespace sieda
