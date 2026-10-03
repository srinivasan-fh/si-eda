#include "sieda/Reliability.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/Industry.hpp"
#include "sieda/Library.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {
std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

bool contains(const std::string& haystack, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (haystack.find(n) != std::string::npos) return true;
    return false;
}

std::string pinName(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    return pin >= 0 && pin < static_cast<int>(pins.size()) ? upper(pins[static_cast<size_t>(pin)].name) : "";
}

std::string partName(const Component& c) {
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* p = CustomPartRegistry::instance().find(c.customPart)) return upper(p->spec.name);
    return upper(c.def().name);
}

bool isAmplifierInput(const Component& c, int pin) {
    std::string n = pinName(c, pin);
    if (c.kind == ComponentKind::OpAmp) return n == "IN+" || n == "IN-";
    if (c.kind != ComponentKind::Custom) return false;
    return n == "IN+" || n == "IN-" || n == "+IN" || n == "-IN" || n == "INP" || n == "INN" || n == "VIN+" ||
           n == "VIN-" || n == "NONINV" || n == "INV";
}

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

const IndustryProfile& profileOf(const Project& p) {
    const IndustryProfile* prof = findIndustry(p.industry);
    return prof ? *prof : *findIndustry("general");
}

bool isOneOf(const std::string& id, std::initializer_list<const char*> ids) {
    for (const char* x : ids)
        if (id == x) return true;
    return false;
}

/// Junction-to-ambient thermal resistance (°C/W) of a part on a typical two-layer board (datasheet-typical values).
double thetaJA(const Component& c) {
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* part = CustomPartRegistry::instance().find(c.customPart)) {
            const std::string type = upper(part->spec.package.type);
            // Exposed thermal pad (ESOP / PowerPAD / HSOP / DFN): heat goes straight into the board.
            if (contains(upper(part->spec.description), {"ESOP", "POWERPAD", "HSOP", "EXPOSED", "DFN", "THERMAL PAD"}))
                return 50;
            int pins = part->spec.package.pinCount > 0 ? part->spec.package.pinCount : static_cast<int>(part->spec.pins.size());
            if (type.find("SOT23") != std::string::npos) return pins > 3 ? 200 : 250;
            if (type.find("SOIC") != std::string::npos) return pins >= 14 ? 90 : 120;
            if (type.find("TSSOP") != std::string::npos) return pins >= 16 ? 95 : 130;
            if (type.find("DIP") != std::string::npos) return pins >= 14 ? 70 : 90;
            if (type.find("QFN") != std::string::npos) return 45;
            if (type.find("LQFP") != std::string::npos) return pins >= 64 ? 55 : 70;
            if (type.find("TO220") != std::string::npos) return 60;
        }
    const std::string fp = c.def().footprint;
    if (fp.find("0402") != std::string::npos) return 250;
    if (fp.find("0603") != std::string::npos) return 200;
    if (fp.find("0805") != std::string::npos) return 160;
    if (fp.find("1206") != std::string::npos) return 120;
    if (fp.find("SOT23") != std::string::npos) return 250;
    if (fp.find("SOD123") != std::string::npos) return 250;
    if (fp.find("SOIC") != std::string::npos) return 120;
    return 120;
}

/// Glass transition temperature of the laminate the profile asks for.
double laminateTg(const std::string& industry) {
    if (industry == "space") return 250;  // polyimide
    if (isOneOf(industry, {"automotive", "power", "industrial", "defence", "marine"})) return 170;
    return 140;
}
}  // namespace

double referencePlaneHeight(const BoardSettings& s) {
    const double cu = 0.035 * std::max(0.5, s.copperWeightOz);
    if (s.layerCount <= 2) return std::max(0.1, s.thickness - 2 * cu);
    return std::max(0.08, (s.thickness - s.layerCount * cu) / (s.layerCount - 1));
}

double microstripWidth(double ohms, const BoardSettings& s) {
    // IPC-2141: Z0 = 87 / sqrt(εr + 1.41) · ln(5.98 h / (0.8 w + t)), solved for w.
    const double er = 4.4, t = 0.035 * std::max(0.5, s.copperWeightOz), h = referencePlaneHeight(s);
    double w = (5.98 * h / std::exp(ohms * std::sqrt(er + 1.41) / 87.0) - t) / 0.8;
    return std::max(0.05, w);
}

double leakageSpacing(const BoardSettings& s) { return s.coated() ? 0.25 : 0.5; }

NetClassification classifyNets(const Project& project) { return classifyNets(project.schematic, project.industry); }

NetClassification classifyNets(const Schematic& sch, const std::string& industry) {
    NetClassification out;
    const auto& nets = sch.nets();
    const int gnd = sch.groundNet();
    // Amplifier inputs: net → the amplifier and the other input's net.
    for (const auto& c : sch.components()) {
        const auto& pins = c.def().pins;
        std::vector<int> inputs;
        int outNet = -1;
        for (size_t p = 0; p < pins.size(); ++p) {
            if (isAmplifierInput(c, static_cast<int>(p))) inputs.push_back(static_cast<int>(p));
            if (pinName(c, static_cast<int>(p)) == "OUT" || pinName(c, static_cast<int>(p)) == "VOUT")
                outNet = sch.netOf({c.id, static_cast<int>(p)});
        }
        if (inputs.size() != 2) continue;
        for (int k = 0; k < 2; ++k) {
            int net = sch.netOf({c.id, inputs[k]}), other = sch.netOf({c.id, inputs[1 - k]});
            if (net < 0 || net == gnd || sch.netRole(net) != NetRole::Signal) continue;
            // High impedance: no low-value resistor (< 100 kΩ) or source drives it, or a ≥ 1 MΩ resistor sits on it.
            bool lowZ = false, megohm = false;
            for (const auto& pin : nets[static_cast<size_t>(net)].pins) {
                const Component* o = sch.find(pin.component);
                if (!o || o->id == c.id) continue;
                if (o->kind == ComponentKind::Resistor) {
                    auto r = parseEngineeringValue(primaryValue(o->value));
                    if (r && *r >= 1e6) megohm = true;
                    else if (r && *r < 1e5) lowZ = true;
                } else if (isSourceKind(o->kind) || o->kind == ComponentKind::NPN || o->kind == ComponentKind::NMOS ||
                           o->kind == ComponentKind::OpAmp || o->kind == ComponentKind::LED || o->kind == ComponentKind::Diode) {
                    lowZ = true;
                }
            }
            if (!megohm && lowZ) continue;
            // Guard at the input's own potential: the other input (follower: its output), else ground.
            int guard = other >= 0 ? other : (outNet >= 0 ? outNet : gnd);
            out.highImpedance[net] = guard;
        }
    }
    // Any net with a ≥ 10 MΩ resistor is leakage-sensitive too.
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Resistor) continue;
        auto r = parseEngineeringValue(primaryValue(c.value));
        if (!r || *r < 1e7) continue;
        for (int p = 0; p < 2; ++p) {
            int net = sch.netOf({c.id, p});
            if (net >= 0 && net != gnd && sch.netRole(net) == NetRole::Signal && !out.highImpedance.count(net))
                out.highImpedance[net] = gnd;
        }
    }
    // Fast nets: pulse-driven, named clocks / buses / PWM, or a switching node (MOSFET drain with an inductor).
    for (const auto& n : nets) {
        if (n.isGround) continue;
        std::string name = upper(n.name);
        bool fast = contains(name, {"CLK", "SCK", "XTAL", "OSC", "PWM", "MOSI", "MISO", "USB", "D+", "D-", "ETH", "LVDS",
                                    "HDMI", "MIPI", "SW_NODE", "SWNODE", "PHASE"});
        bool inductor = false, drain = false;
        for (const auto& pin : n.pins) {
            const Component* c = sch.find(pin.component);
            if (!c) continue;
            if (c->kind == ComponentKind::VoltageSource && pin.pin == 0) {
                auto spec = SourceSpec::parse(c->value);
                if (spec && spec->kind == SourceSpec::Kind::Pulse) fast = true;
            }
            std::string pn = pinName(*c, pin.pin);
            if (c->kind == ComponentKind::Custom && contains(pn, {"CLK", "SCK", "XTAL", "TX", "RX", "PWM", "MOSI", "MISO"}))
                fast = true;
            if (c->kind == ComponentKind::Inductor) inductor = true;
            if (c->kind == ComponentKind::NMOS && pn == "D") drain = true;
        }
        if (inductor && drain) fast = true;
        if (fast && sch.netRole(n.index) == NetRole::Signal) out.fast.insert(n.index);
    }
    // RF nets: by name, or on RF/networking boards any net on an SMA / U.FL / antenna connector.
    const bool rfBoard = industry == "rf" || industry == "networking";
    for (const auto& n : nets) {
        if (n.isGround || sch.netRole(n.index) != NetRole::Signal) continue;
        std::string name = upper(n.name);
        bool rf = name == "RF" || contains(name, {"RF_", "_RF", "RFIN", "RFOUT", "RF_IN", "RF_OUT", "ANT", "LNA", "50R"});
        if (rfBoard)
            for (const auto& pin : n.pins)
                if (const Component* c = sch.find(pin.component))
                    if (c->kind == ComponentKind::Connector && contains(upper(c->value), {"SMA", "U.FL", "UFL", "ANT", "MMCX"}))
                        rf = true;
        if (rf) out.rf.insert(n.index);
    }
    return out;
}

FabricationRequirements fabricationRequirements(const Project& project) {
    const BoardSettings& s = project.pcb.settings;
    const std::string id = project.industry;
    FabricationRequirements r;
    r.ipcClass = isOneOf(id, {"automotive", "space", "defence", "medical"}) || s.rulePreset.find("Class 3") != std::string::npos
                     ? 3
                     : 2;
    r.material = "FR-4, Tg ≥ 140 °C (IPC-4101/21)";
    if (isOneOf(id, {"automotive", "power", "industrial", "defence", "marine"}))
        r.material = "High-Tg FR-4, Tg ≥ 170 °C, low Z-axis CTE (IPC-4101/126)";
    if (id == "space") r.material = "Polyimide (IPC-4101/40), low outgassing: ASTM E595 TML ≤ 1.0 %, CVCM ≤ 0.10 %";
    if (id == "rf") r.material = "Low-loss laminate for > 2 GHz (e.g. Rogers RO4350B, εr 3.48) or FR-4 below 2 GHz";
    r.solder = isOneOf(id, {"space", "defence"}) ? "Sn63Pb37 tin-lead (no pure-tin finishes: tin-whisker risk, GEIA-STD-0005-1)"
                                                 : "SAC305 lead-free";
    if (isOneOf(id, {"space", "defence"})) r.finish = "HASL tin-lead (SnPb) or ENIG; no pure tin";
    else if (id == "rf") r.finish = "Immersion silver or ENEPIG (smooth, low-loss; avoid HASL on RF lines)";
    else r.finish = "";  // fine-pitch decides (see fabrication notes)
    if (s.coated())
        r.notes.push_back("Conformal coating: " + s.coating + " (IPC-CC-830), applied after cleaning; mask connectors, "
                          "test points and switches");
    r.notes.push_back("Bake boards before assembly to drive out absorbed moisture (IPC-1601); store sealed with desiccant");
    if (isOneOf(id, {"automotive", "uav", "robotics", "defence", "space", "marine"}))
        r.notes.push_back("Stake tall or heavy parts (inductors, electrolytic capacitors, connectors) with adhesive "
                          "against vibration");
    if (isOneOf(id, {"automotive", "space", "defence"}))
        r.notes.push_back("Thermal cycling −40 … +125 °C: AEC-Q100/Q200 parts, IPC-6012 Class 3 plating (via barrel "
                          "≥ 25 µm copper)");
    if (id == "space") r.notes.push_back("Radiation: radiation-hardened / latch-up-protected parts and redundant design");
    if (id == "rf" || id == "networking")
        r.notes.push_back("Controlled impedance: RF lines 50 Ω ± 10 % (fab to adjust width to their stack-up); low-profile "
                          "copper foil for skin-effect losses");
    return r;
}

std::vector<RuleViolation> reliabilityChecks(const Project& project) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const IndustryProfile& prof = profileOf(project);
    const std::string& id = prof.id;
    std::vector<RuleViolation> out;
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {},
                   Vec2 loc = {}, bool hasLoc = false) {
        RuleViolation v;
        v.severity = sev;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        v.location = loc;
        v.hasLocation = hasLoc;
        out.push_back(std::move(v));
    };
    const auto& nets = sch.nets();
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(n)].name : "?"; };
    const NetClassification cls = classifyNets(project);
    const auto pads = pcb.pads(sch);
    const bool laidOut = !pads.empty();
    const int gnd = sch.groundNet();

    // ------------------------------------------------------------------ circuit (schematic) reliability
    // Inductive loads switched by a transistor need a flyback / freewheeling diode across them.
    for (const auto& c : sch.components()) {
        std::string name = partName(c);
        bool inductive = c.kind == ComponentKind::Inductor ||
                         (c.kind == ComponentKind::Custom && contains(name, {"MOTOR", "RELAY", "SOLENOID", "COIL", "FAN"}));
        if (!inductive || c.def().pins.size() < 2) continue;
        int a = sch.netOf({c.id, 0}), b = sch.netOf({c.id, 1});
        if (a < 0 || b < 0) continue;
        bool switched = false, diode = false;
        for (int n : {a, b})
            for (const auto& pin : nets[static_cast<size_t>(n)].pins) {
                const Component* o = sch.find(pin.component);
                if (!o) continue;
                std::string pn = pinName(*o, pin.pin);
                if ((o->kind == ComponentKind::NMOS && pn == "D") || (o->kind == ComponentKind::NPN && pn == "C")) switched = true;
                // A diode across the load, or a converter's rectifier / freewheeling diode on the switched node,
                // gives the inductor current a path when the switch opens.
                if (o->kind == ComponentKind::Diode) diode = true;
            }
        // A buck/boost inductor is not a switched load; only flag loads with one side on a supply rail.
        bool railSide = sch.netRole(a) == NetRole::Power || sch.netRole(b) == NetRole::Power;
        if (switched && railSide && !diode)
            add(Severity::Warning, "REL_FLYBACK",
                c.ref + " is an inductive load switched by a transistor with no flyback diode across it. When the switch "
                "turns off, V = L·di/dt kicks back far above the supply and destroys the transistor. Add a fast "
                "freewheeling diode across " + c.ref + " (cathode to the supply side), and an RC snubber at the switch for "
                "motor drives.",
                {c.id});
    }
    // Half-bridges (two N-MOSFETs stacked) driven without a gate driver: shoot-through on fast dv/dt.
    {
        bool gateDriver = false;
        for (const auto& c : sch.components())
            if (c.kind == ComponentKind::Custom &&
                contains(partName(c), {"IR21", "IRS2", "DRV8", "UCC2", "UCC5", "ADUM", "SI82", "L638", "MIC44", "FAN73", "GATE"}))
                gateDriver = true;
        for (const auto& hi : sch.components()) {
            if (hi.kind != ComponentKind::NMOS) continue;
            int hiSource = sch.netOf({hi.id, 2});
            for (const auto& lo : sch.components()) {
                if (lo.kind != ComponentKind::NMOS || lo.id == hi.id) continue;
                if (hiSource < 0 || sch.netOf({lo.id, 1}) != hiSource) continue;  // low-side drain = high-side source
                if (!gateDriver)
                    add(Severity::Warning, "REL_SHOOT_THROUGH",
                        hi.ref + " / " + lo.ref + " form a half-bridge driven without a gate driver. Fast dv/dt on the "
                        "switch node couples through the Miller capacitance and can turn the off transistor on "
                        "(shoot-through: a short across the rails). Use a gate driver with dead time and a negative or "
                        "clamped turn-off (e.g. IR2104, UCC21520, isolated ADuM drivers).",
                        {hi.id, lo.id});
                if (isOneOf(id, {"robotics", "power", "industrial", "automotive", "uav"}))
                    add(Severity::Info, "REL_SNUBBER",
                        "Half-bridge " + hi.ref + " / " + lo.ref + ": place an RC snubber across each switch and a local "
                        "bulk + ceramic decoupling capacitor at the bridge to absorb switching ringing.",
                        {hi.id, lo.id});
            }
        }
    }
    // High currents next to logic: ground bounce.
    DcResult dc;
    bool haveDc = false;
    {
        bool hasSource = false;
        for (const auto& c : sch.components()) hasSource |= isSourceKind(c.kind);
        if (hasSource && gnd >= 0) {
            dc = Simulator(sch).dcOperatingPoint();
            haveDc = dc.converged;
        }
    }
    if (haveDc && isOneOf(id, {"robotics", "uav", "power", "automotive", "industrial", "general"})) {
        double peak = 0;
        for (const auto& d : dc.devices) {
            const Component* c = sch.find(d.componentId);
            if (c && !isSourceKind(c->kind)) peak = std::max(peak, std::fabs(d.current));
        }
        bool logic = false;
        for (const auto& c : sch.components()) logic |= c.kind == ComponentKind::Custom || c.kind == ComponentKind::IC8;
        if (peak > 1.0 && logic)
            add(Severity::Info, "REL_GROUND_BOUNCE",
                "Load currents reach " + fmt("%.1f A", peak) + " on a board with logic ICs. Pulsed motor/power currents "
                "through shared ground copper make the logic ground bounce (corrupted sensor readings, missed steps, "
                "resets). Keep power and signal grounds separate and join them at one point near the supply (star "
                "ground), and keep the power loop small.");
    }
    // Supply inputs: transient protection.
    {
        bool tvs = false;
        for (const auto& c : sch.components())
            if (c.kind == ComponentKind::Diode &&
                contains(upper(c.value), {"TVS", "SMBJ", "SMAJ", "SMCJ", "P6KE", "SM8S", "P4SMA", "ESD", "SMF"}))
                tvs = true;
        bool supplyInput = false;
        for (const auto& c : sch.components()) supplyInput |= c.kind == ComponentKind::Battery || c.kind == ComponentKind::VoltageSource;
        if (supplyInput && !tvs) {
            if (id == "automotive")
                add(Severity::Warning, "REL_LOAD_DUMP",
                    "No TVS diode on the supply input. Automotive supplies see load-dump surges up to ~100 V for hundreds "
                    "of milliseconds (ISO 16750-2) and fast transients (ISO 7637-2). Add a TVS (e.g. SM8S / SMBJ-series "
                    "rated for load dump) plus reverse-polarity protection at the input.");
            else if (isOneOf(id, {"industrial", "marine", "robotics", "uav", "defence"}))
                add(Severity::Info, "REL_SUPPLY_TRANSIENT",
                    "No TVS diode on the supply input: long cables and motors put surges on the supply. A TVS sized for "
                    "the supply (IEC 61000-4-5) protects the board.");
        }
    }
    // Vibration: tall / heavy parts.
    if (isOneOf(id, {"automotive", "uav", "robotics", "defence", "space", "marine"}) && laidOut) {
        std::vector<int> tall;
        std::string refs;
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed) continue;
            const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
            if (fp && (fp->body.height >= 5.0 || c.kind == ComponentKind::Inductor)) {
                tall.push_back(c.id);
                refs += (refs.empty() ? "" : ", ") + c.ref;
            }
        }
        if (!tall.empty())
            add(Severity::Info, "REL_VIBRATION",
                "Tall or heavy parts (" + refs + ") can tear off their pads under vibration and shock. Stake them with "
                "adhesive (e.g. per NASA-STD-8739.1 / IPC-A-610) or use mechanically anchored parts.", tall);
    }
    // Environment guidance from the profile's standards.
    if (isOneOf(id, {"automotive", "space", "defence"}))
        add(Severity::Info, "REL_THERMAL_CYCLING",
            "Thermal cycling (" + fmt("%.0f", prof.minAmbientC) + " … " + fmt("%.0f °C", prof.maxAmbientC) +
            "): copper, FR-4 and silicon expand at different rates (CTE mismatch), fatiguing solder joints and via "
            "barrels. Use AEC-Q100/Q200 parts, IPC Class 3 plating, avoid large ceramic chips (≥ 1210) near board edges.");
    if (isOneOf(id, {"space", "defence"}))
        add(Severity::Info, "REL_TIN_WHISKER",
            "Pure-tin finishes grow tin whiskers that break off and short circuits, worst in vacuum. Use tin-lead solder "
            "(Sn63Pb37) and no pure-tin finishes (GEIA-STD-0005-1); the fabrication notes ask for it.");
    if (id == "space") {
        add(Severity::Info, "REL_OUTGASSING",
            "Vacuum outgassing: volatiles in FR-4 and solder mask condense on optics and solar cells. Use polyimide and "
            "low-outgassing materials (ASTM E595: TML ≤ 1.0 %, CVCM ≤ 0.10 %).");
        add(Severity::Info, "REL_RADIATION",
            "Radiation (single-event upsets and latch-up): use radiation-hardened or latch-up-protected parts, current "
            "limiting on supplies, EDAC memory and redundant (TMR) architecture for critical functions.");
    }
    if (id == "robotics")
        add(Severity::Info, "REL_FLEX",
            "Moving joints: if a flexible / rigid-flex section bends in service, follow IPC-2223 (curved traces, no vias "
            "or plating in the bend, staggered conductors, hatched planes, bend radius ≥ 100 × thickness for dynamic "
            "flex) to avoid copper fatigue cracks.");

    // Leakage: high-impedance inputs.
    if (!cls.highImpedance.empty()) {
        std::string list;
        for (const auto& [net, guard] : cls.highImpedance) list += (list.empty() ? "" : ", ") + netName(net);
        add(Severity::Info, "REL_HIGH_IMPEDANCE",
            "High-impedance nodes (" + list + "): surface leakage through moisture absorbed by FR-4 and contamination "
            "swamps nA/pA signals. Surround them with a guard ring driven at their own potential, keep " +
            fmt("%.2f mm", leakageSpacing(s)) + " from other copper, clean and conformal-coat the board" +
            (s.coated() ? "" : " (Board Setup → Conformal coating)") +
            ", and for pA-level measurement wire the input in the air to a PTFE standoff.");
    }
    // HV boards: coating / spacing guidance.
    {
        double vmax = 0;
        for (const auto& [net, r] : netVoltageRanges(sch)) vmax = std::max({vmax, std::fabs(r.first), std::fabs(r.second)});
        if (vmax > 50 && !s.coated())
            add(Severity::Info, "REL_COATING",
                "Up to " + fmt("%.0f V", vmax) + " on the board: humid or contaminated air and surfaces leak and arc "
                "across gaps. Clearances follow IPC-2221B " + std::string(ipc2221ColumnName(externalSpacingColumn(s.highAltitude, false))) +
                "; a conformal coating (IPC-CC-830) uses column A5 and protects the surface. For mains, also check "
                "creepage to IEC 62368-1 / 60664-1 and add routed slots where spacing is short.");
    }

    if (!laidOut) return out;

    // ------------------------------------------------------------------ PCB layout reliability
    const double eps = 1e-6;
    std::set<int> zoneNets;
    for (const auto& z : pcb.zones)
        for (const auto& n : nets)
            if (n.name == z.net) zoneNets.insert(n.index);

    // Acid traps: two pieces of one track meeting at an acute angle.
    {
        int count = 0;
        for (size_t i = 0; i < pcb.tracks.size() && count < 20; ++i) {
            const Track& a = pcb.tracks[i];
            for (size_t j = i + 1; j < pcb.tracks.size() && count < 20; ++j) {
                const Track& b = pcb.tracks[j];
                if (a.layer != b.layer || a.net != b.net) continue;
                for (Vec2 p : {a.a, a.b}) {
                    Vec2 pa = (p - a.a).length() < eps ? a.b : a.a;
                    Vec2 pb;
                    if ((p - b.a).length() < eps) pb = b.b;
                    else if ((p - b.b).length() < eps) pb = b.a;
                    else continue;
                    Vec2 u = pa - p, v = pb - p;
                    double lu = u.length(), lv = v.length();
                    if (lu < eps || lv < eps) continue;
                    double cosine = (u.x * v.x + u.y * v.y) / (lu * lv);
                    bool onPad = false;
                    for (const auto& pd : pads) onPad |= pd.onLayer(a.layer) && pd.bounds().contains(p);
                    if (cosine > 0.02 && !onPad) {
                        add(Severity::Warning, "REL_ACID_TRAP",
                            "Net " + netName(a.net) + " turns at an acute angle (" +
                                fmt("%.0f°", std::acos(std::min(1.0, cosine)) * 180 / 3.14159265358979) +
                                "): etchant pools in the narrow crevice and eats the copper. Reroute with 45°/90° bends.",
                            {}, p, true);
                        ++count;
                    }
                }
            }
        }
    }
    // Solder bridges: SMD pads of different nets too close for a solder-mask dam.
    {
        const double minGap = 0.2;
        int count = 0;
        for (size_t i = 0; i < pads.size() && count < 20; ++i) {
            if (pads[i].throughHole) continue;
            for (size_t j = i + 1; j < pads.size() && count < 20; ++j) {
                const Pad& a = pads[i];
                const Pad& b = pads[j];
                if (b.throughHole || a.smdLayer != b.smdLayer || (a.net == b.net && a.net >= 0)) continue;
                Rect ra = a.bounds(), rb = b.bounds();
                double dx = std::max({0.0, rb.x0 - ra.x1, ra.x0 - rb.x1}), dy = std::max({0.0, rb.y0 - ra.y1, ra.y0 - rb.y1});
                double gap = std::hypot(dx, dy);
                if (gap < minGap - eps) {
                    const Component* ca = sch.find(a.componentId);
                    const Component* cb = sch.find(b.componentId);
                    add(Severity::Warning, "REL_SOLDER_BRIDGE",
                        "Pads of " + (ca ? ca->ref : "?") + " and " + (cb ? cb->ref : "?") + " are " + fmt("%.2f mm", gap) +
                            " apart: too little for a solder-mask dam (≥ 0.2 mm), so solder can bridge them. Move the parts "
                            "apart or use a smaller paste aperture.",
                        {a.componentId, b.componentId}, (a.position + b.position) * 0.5, true);
                    ++count;
                }
            }
        }
    }
    // Tombstoning: unbalanced copper on the two pads of a small chip part.
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        std::vector<const Pad*> mine;
        for (const auto& p : pads)
            if (p.componentId == c.id) mine.push_back(&p);
        if (mine.size() != 2 || mine[0]->throughHole || mine[1]->throughHole) continue;
        if (mine[0]->size.x * mine[0]->size.y > 4.0) continue;  // only small chips tombstone
        double mass[2] = {0, 0};
        for (int k = 0; k < 2; ++k) {
            const Pad& p = *mine[static_cast<size_t>(k)];
            if (zoneNets.count(p.net)) mass[k] += 2 * s.trackWidth;  // pours reach chip pads through thermal-relief spokes
            for (const auto& t : pcb.tracks)
                if (t.net == p.net && t.layer == p.smdLayer &&
                    (p.bounds().contains(t.a) || p.bounds().contains(t.b)))
                    mass[k] += t.width;
        }
        double hi = std::max(mass[0], mass[1]), lo = std::min(mass[0], mass[1]);
        const int wideNet = mass[0] >= mass[1] ? mine[0]->net : mine[1]->net;
        const bool currentCarrying = sch.netRole(wideNet) != NetRole::Signal;  // a deliberate power / ground width
        if (hi >= 0.6 && hi > 3 * std::max(lo, 0.05))
            add(currentCarrying ? Severity::Info : Severity::Warning, "REL_TOMBSTONE",
                c.ref + ": one pad joins much more copper (" + fmt("%.2f mm", hi) + " of track) than the other (" +
                    fmt("%.2f mm", lo) + "), so its solder melts later and surface tension stands the part on end "
                    "(tombstoning). Neck the wide track down at the pad or use similar widths on both pads.",
                {c.id}, c.pcb.position, true);
    }
    // Via aspect ratio: barrel cracking under thermal expansion (CTE).
    {
        double worst = 0;
        Vec2 at;
        for (const auto& v : pcb.vias)
            if (v.drill > 0 && s.thickness / v.drill > worst) {
                worst = s.thickness / v.drill;
                at = v.position;
            }
        for (const auto& p : pads)
            if (p.throughHole && p.drill > 0 && s.thickness / p.drill > worst) {
                worst = s.thickness / p.drill;
                at = p.position;
            }
        const bool class3 = fabricationRequirements(project).ipcClass >= 3;
        const double limit = class3 ? 10.0 : 12.0, advisory = class3 ? 8.0 : 10.0;
        if (worst > advisory)
            add(worst > limit ? Severity::Warning : Severity::Info, "REL_VIA_ASPECT",
                "A hole has an aspect ratio of " + fmt("%.1f:1", worst) + " (board thickness ÷ drill), above " +
                    fmt("%.0f:1", worst > limit ? limit : advisory) + ". Thin plating in deep holes cracks as the board expands with heat (Z-axis "
                    "CTE). Use a larger drill or a thinner board.",
                {}, at, true);
    }
    // Hot spots: delamination near FR-4 Tg.
    if (haveDc) {
        for (const auto& d : dc.devices) {
            const Component* c = sch.find(d.componentId);
            if (!c || !c->hasFootprint() || isSourceKind(c->kind) || std::fabs(d.power) < 0.1) continue;
            double theta = thetaJA(*c);
            bool pour = false;
            for (const auto& p : pads)
                if (p.componentId == c->id && zoneNets.count(p.net)) pour = true;
            if (pour) theta *= 0.7;  // copper pour spreads the heat
            const double rise = std::fabs(d.power) * theta;
            const double tj = prof.maxAmbientC + rise, board = prof.maxAmbientC + 0.5 * rise, tg = laminateTg(id);
            std::string why;
            Severity sev = Severity::Warning;
            if (tj > 150) {
                sev = Severity::Error;
                why = "a junction near " + fmt("%.0f °C", tj) + ", above the 150 °C most parts allow";
            } else if (board > tg - 25) {
                why = "the board under it near " + fmt("%.0f °C", board) + ", close to the laminate's glass transition "
                      "(Tg ≈ " + fmt("%.0f °C", tg) + "): it softens, expands in Z and delaminates";
            } else if (tj > 140) {
                why = "a junction near " + fmt("%.0f °C", tj) + ", little margin below 150 °C";
            } else {
                continue;
            }
            add(sev, "REL_HOT_SPOT",
                c->ref + " dissipates " + fmt("%.2f W", std::fabs(d.power)) + " at " + fmt("%.0f °C", prof.maxAmbientC) +
                    " ambient: " + why + ". Spread the heat with a copper pour and thermal vias, use a larger package or a "
                    "high-Tg laminate.",
                {c->id}, c->pcb.position, true);
        }
    }
    // Crosstalk: fast nets running parallel and close to other signals (3W rule).
    {
        int count = 0;
        for (const auto& a : pcb.tracks) {
            if (!cls.fast.count(a.net) || count >= 20) continue;
            Vec2 da = a.b - a.a;
            double la = da.length();
            if (la < 1.0) continue;
            for (const auto& b : pcb.tracks) {
                if (b.layer != a.layer || b.net == a.net || sch.netRole(b.net) != NetRole::Signal) continue;
                Vec2 db = b.b - b.a;
                double lb = db.length();
                if (lb < 1.0 || std::fabs((da.x * db.x + da.y * db.y) / (la * lb)) < 0.98) continue;
                // Overlap of b projected on a, and the gap between them.
                Vec2 u = da * (1.0 / la);
                double t0 = (b.a - a.a).x * u.x + (b.a - a.a).y * u.y, t1 = (b.b - a.a).x * u.x + (b.b - a.a).y * u.y;
                double overlap = std::min(la, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1));
                double gap = pointSegmentDistance(b.a, a.a, a.b) - (a.width + b.width) / 2;
                double w = std::max(a.width, b.width);
                if (overlap > 5.0 && gap < 2 * w) {
                    add(Severity::Warning, "REL_CROSSTALK",
                        "Fast net " + netName(a.net) + " runs " + fmt("%.0f mm", overlap) + " alongside " + netName(b.net) +
                            " only " + fmt("%.2f mm", std::max(0.0, gap)) + " away: its edges couple into the neighbour "
                            "(crosstalk). Keep ≥ 2 track widths edge-to-edge (3W rule), or route over a ground plane with a "
                            "ground track between them.",
                        {}, (a.a + a.b) * 0.5, true);
                    if (++count >= 20) break;
                }
            }
        }
    }
    // RF lines matter as transmission lines once they are longer than about a tenth of a wavelength on the board.
    double rfHz = 0;
    for (const auto& c : sch.components())
        if (isVoltageSourceKind(c.kind))
            if (auto spec = SourceSpec::parse(c.value); spec && spec->kind == SourceSpec::Kind::Sine)
                rfHz = std::max(rfHz, spec->frequency);
    if (rfHz < 1e6) rfHz = 2.4e9;  // unknown: assume the 2.4 GHz band
    const double lambdaTenthMm = 299792458.0 / (rfHz * std::sqrt(3.3)) * 1000.0 / 10.0;  // ε_eff ≈ 3.3 for FR-4
    auto netLength = [&](int net) {
        double l = 0;
        for (const auto& t : pcb.tracks)
            if (t.net == net) l += (t.b - t.a).length();
        return l;
    };
    bool longRf = false;
    for (int net : cls.rf) longRf |= netLength(net) > lambdaTenthMm;

    // Reference plane for fast / RF signals (return current, EMI).
    if (!cls.fast.empty() || !cls.rf.empty()) {
        bool groundPour = false;
        for (const auto& z : pcb.zones)
            for (const auto& n : nets)
                if (n.name == z.net && n.isGround) groundPour = true;
        const bool critical = longRf || (!cls.fast.empty() && isOneOf(id, {"networking", "vlsi"}));
        if (!groundPour)
            add(critical ? Severity::Warning : Severity::Info, "REL_NO_REFERENCE_PLANE",
                "Fast or RF signals but no ground pour/plane: return currents take long loops that radiate (EMI failing "
                "FCC / CE) and the impedance is undefined. Add a solid ground plane (Board Setup → copper pour on "
                "GND), ideally on the layer next to the signals, and do not route across splits in it.");
    }
    // RF impedance: 50 Ω width, no vias, no width changes.
    for (int net : cls.rf) {
        double target = microstripWidth(50.0, s);
        std::set<long long> widths;
        int vias = 0;
        Vec2 at;
        bool any = false;
        for (const auto& t : pcb.tracks)
            if (t.net == net) {
                widths.insert(std::llround(t.width * 1000));
                at = t.a;
                any = true;
            }
        for (const auto& v : pcb.vias) vias += v.net == net;
        if (!any) continue;
        const double length = netLength(net);
        if (length <= lambdaTenthMm) {
            add(Severity::Info, "REL_RF_SHORT",
                "RF net " + netName(net) + " is " + fmt("%.0f mm", length) + " long, under a tenth of a wavelength (" +
                    fmt("%.0f mm", lambdaTenthMm) + " at " + fmt("%.0f MHz", rfHz / 1e6) + "): electrically short, so its "
                    "width is not critical. Keep it short; above λ/10 route it as a 50 Ω line.",
                {}, at, true);
            continue;
        }
        double w0 = static_cast<double>(*widths.begin()) / 1000.0;
        if (std::fabs(w0 - target) > 0.15 * target)
            add(Severity::Warning, "REL_RF_IMPEDANCE",
                "RF net " + netName(net) + " is " + fmt("%.2f mm", w0) + " wide; 50 Ω on this stack-up (h = " +
                    fmt("%.2f mm", referencePlaneHeight(s)) + ", FR-4) needs about " + fmt("%.2f mm", target) +
                    ". Impedance mismatch reflects power back to the transmitter. Set a net class of that width" +
                    (target > 1.5 ? " (or use a 4-layer board / grounded coplanar waveguide for a narrower line)" : "") + ".",
                {}, at, true);
        if (widths.size() > 1)
            add(Severity::Warning, "REL_RF_DISCONTINUITY",
                "RF net " + netName(net) + " changes width along its length: every step is an impedance discontinuity "
                "that reflects the signal.", {}, at, true);
        if (vias > 0)
            add(Severity::Warning, "REL_RF_VIA",
                "RF net " + netName(net) + " uses " + std::to_string(vias) + " via(s): each adds inductance and an "
                "impedance mismatch. Keep RF lines on one layer over an unbroken ground plane.", {}, at, true);
    }
    // Leakage spacing around high-impedance copper (guard net and ground excepted).
    {
        const double need = leakageSpacing(s);
        int count = 0;
        for (const auto& [net, guard] : cls.highImpedance) {
            for (const auto& t : pcb.tracks) {
                if (t.net != net || count >= 10) continue;
                for (const auto& o : pcb.tracks) {
                    if (o.layer != t.layer || o.net == net || o.net == guard || o.net == gnd) continue;
                    double gap = segmentSegmentDistance(t.a, t.b, o.a, o.b) - (t.width + o.width) / 2;
                    if (gap < need - eps) {
                        add(Severity::Warning, "REL_LEAKAGE",
                            "High-impedance net " + netName(net) + " passes " + fmt("%.2f mm", std::max(0.0, gap)) +
                                " from " + netName(o.net) + " (needs " + fmt("%.2f mm", need) + "): surface leakage "
                                "through moisture and residue adds error current. Add a guard ring driven by " +
                                netName(guard) + " around it, or increase the spacing.",
                            {}, (t.a + t.b) * 0.5, true);
                        ++count;
                        break;
                    }
                }
            }
        }
    }
    return out;
}

}  // namespace sieda
