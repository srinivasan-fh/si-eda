#include "sieda/Robotics.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Project.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {

std::string up(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

bool containsAny(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (hay.find(n) != std::string::npos) return true;
    return false;
}

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string partName(const Component& c) {
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* p = CustomPartRegistry::instance().find(c.customPart)) return up(p->spec.name);
    return up(c.def().name);
}

std::string pinName(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    return pin >= 0 && pin < static_cast<int>(pins.size()) ? up(pins[static_cast<size_t>(pin)].name) : "";
}

/// What a part is, for the segment checks.
struct Parts {
    std::vector<const Component*> drivers;     // gate / motor drivers (IR2104, L293D, ULN2003A, DRV…)
    std::vector<const Component*> powerFets;   // NMOS or power MOSFET parts
    std::vector<const Component*> shunts;      // current-sense resistors (≤ 0.1 Ω) and hall sensors (ACS712)
    std::vector<const Component*> imus;        // MPU-6050, ICM-, BMI…, LSM6…
    std::vector<const Component*> rf;          // radios (NRF24L01, ESP32, LoRa…)
    std::vector<const Component*> coax;        // SMA / U.FL connectors
    std::vector<const Component*> isolators;   // ADuM, ISO…, optocouplers
    std::vector<const Component*> fieldbus;    // TJA1050 (CAN), MAX485 (RS-485)
    std::vector<const Component*> compute;     // MCUs / SoCs
    std::vector<const Component*> fuses, tvs, diodes, leds, inductors, caps, sources;
};

Parts classify(const Schematic& sch) {
    Parts p;
    for (const auto& c : sch.components()) {
        const std::string n = partName(c), v = up(c.value);
        switch (c.kind) {
            case ComponentKind::NMOS: p.powerFets.push_back(&c); break;
            case ComponentKind::Fuse: p.fuses.push_back(&c); break;
            case ComponentKind::LED: p.leds.push_back(&c); break;
            case ComponentKind::Inductor: p.inductors.push_back(&c); break;
            case ComponentKind::Capacitor: p.caps.push_back(&c); break;
            case ComponentKind::Diode:
                (containsAny(v, {"SMBJ", "SMAJ", "SMCJ", "P6KE", "TVS", "PESD", "ESD"}) ? p.tvs : p.diodes).push_back(&c);
                break;
            case ComponentKind::Resistor: {
                auto r = parseEngineeringValue(primaryValue(c.value));
                if (r && *r > 0 && *r <= 0.1) p.shunts.push_back(&c);
                break;
            }
            case ComponentKind::Connector:
                if (containsAny(v, {"SMA", "U.FL", "UFL", "MMCX", "IPEX", "MHF"})) p.coax.push_back(&c);
                break;
            default: break;
        }
        if (isSourceKind(c.kind)) p.sources.push_back(&c);
        if (c.kind != ComponentKind::Custom) continue;
        if (containsAny(n, {"IR2104", "IR2110", "IR2184", "L293", "ULN2003", "DRV8", "TMC2", "A4988", "L298", "UCC27",
                            "FAN73", "LM5113", "DRV83"}))
            p.drivers.push_back(&c);
        if (n.rfind("IRF", 0) == 0 || n.rfind("IPB", 0) == 0 || n.rfind("BSC", 0) == 0 || containsAny(n, {"MOSFET", "GAN", "EPC2"}))
            p.powerFets.push_back(&c);
        if (containsAny(n, {"ACS7", "INA2", "INA1", "INA3", "AMC1"})) p.shunts.push_back(&c);
        if (containsAny(n, {"MPU-6050", "MPU6050", "MPU-9250", "ICM-", "BMI0", "BMI1", "BMI2", "LSM6", "BNO0", "IMU"}))
            p.imus.push_back(&c);
        if (containsAny(n, {"NRF24", "ESP32", "ESP8266", "SX127", "SX126", "LORA", "CC1101", "RFM9", "WIFI", "BLE", "LTE"}))
            p.rf.push_back(&c);
        if (containsAny(n, {"SMA", "U.FL", "UFL", "MMCX"})) p.coax.push_back(&c);
        if (containsAny(n, {"ADUM", "ISO77", "ISO15", "SI86", "PC817", "6N137", "TLP", "OPTO"})) p.isolators.push_back(&c);
        if (containsAny(n, {"TJA10", "MCP2551", "SN65HVD", "MAX485", "MAX3485", "ADM485", "THVD", "MCP2562", "ISO1050"}))
            p.fieldbus.push_back(&c);
        // Processors: a custom part with GPIO / SWD / XTAL style pins (MCUs, SoCs, module carriers).
        bool cpu = false;
        for (size_t i = 0; i < c.def().pins.size() && !cpu; ++i) {
            const std::string pn = pinName(c, static_cast<int>(i));
            cpu = pn.rfind("GPIO", 0) == 0 || pn == "PA0" || pn == "PB0" || pn == "PD0" || pn == "SWCLK" || pn == "SWDIO" ||
                  pn == "XTAL1" || pn == "XIN" || pn == "TCK";
        }
        if (cpu) p.compute.push_back(&c);
    }
    return p;
}

std::string netName(const Schematic& sch, int net) {
    const auto& nets = sch.nets();
    return net >= 0 && net < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(net)].name : "";
}

/// Nets whose (upper-case) name contains one of the needles.
std::vector<int> netsNamed(const Schematic& sch, std::initializer_list<const char*> needles) {
    std::vector<int> out;
    for (const auto& n : sch.nets())
        if (!n.name.empty() && containsAny(up(n.name), needles)) out.push_back(n.index);
    return out;
}

Rect courtyardOf(const PcbLayout& pcb, const Component& c) { return pcb.courtyard(c); }

/// The pad heat leaves a power part through: its drain / tab / exposed pad, else its largest pad.
const Pad* heatPad(const std::vector<Pad>& pads, const Component& c) {
    const Pad* best = nullptr;
    int bestRank = -1;
    for (const auto& p : pads) {
        if (p.componentId != c.id || p.net < 0) continue;
        const std::string pn = pinName(c, p.pinIndex);
        const int rank = (pn == "D" || pn == "DRAIN" || pn == "TAB" || pn == "EP" || pn == "PAD") ? 1 : 0;
        if (!best || rank > bestRank || (rank == bestRank && p.size.x * p.size.y > best->size.x * best->size.y)) {
            best = &p;
            bestRank = rank;
        }
    }
    return best;
}

double rectGap(const Rect& a, const Rect& b) {
    double dx = std::max({0.0, b.x0 - a.x1, a.x0 - b.x1}), dy = std::max({0.0, b.y0 - a.y1, a.y0 - b.y1});
    return std::hypot(dx, dy);
}

/// Resistors between two nets (one pin on each).
std::vector<const Component*> bridges(const Schematic& sch, int a, int b, std::initializer_list<ComponentKind> kinds) {
    std::vector<const Component*> out;
    for (const auto& c : sch.components()) {
        if (std::find(kinds.begin(), kinds.end(), c.kind) == kinds.end() || c.def().pins.size() != 2) continue;
        int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        if ((n0 == a && n1 == b) || (n0 == b && n1 == a)) out.push_back(&c);
    }
    return out;
}

}  // namespace

const std::vector<RobotPlatform>& robotPlatforms() {
    static const std::vector<RobotPlatform> p = {
        {"rover", "Mars / planetary rover",
         "Wheeled exploration rover: radiation, extreme thermal cycling, no repair",
         {"Space-grade or radiation-tolerant parts, latch-up protection and redundant compute (Space industry profile).",
          "Thermal cycling −120 … +20 °C on Mars: polyimide or high-Tg laminate, Class 3 plating, staked parts.",
          "Each wheel / mobility actuator on its own motor-drive segment with current limiting; never one point of "
          "failure on the power bus.",
          "Long-range radio and a UHF relay link: coax connectors at the board edge, shielded RF sections."}},
        {"fpv", "FPV drone",
         "Racing / camera drone: 4–6S LiPo, four 30–60 A ESCs, weight-critical",
         {"6S LiPo (25.2 V) with 30–60 A per motor: 2 oz copper or more on the battery and ESC rails, wide pours, "
          "TVS and a low-ESR bulk capacitor at the battery pads.",
          "Stack boards on the 30.5 / 20 mm mounting patterns; soft-mount the flight controller's IMU (gummies).",
          "Keep the video transmitter and receiver antennas away from ESC switching; U.FL / MMCX at the edge.",
          "Arming / kill switch and a current sensor for telemetry (OSD)."}},
        {"arm", "Industrial arm manipulator",
         "Multi-axis arm: 48 V servo drives, safe torque off, field-bus to every joint",
         {"48 V DC bus to each joint drive; brake chopper or regenerative clamp for back-EMF when axes decelerate.",
          "Safe Torque Off (STO) / E-stop wired in hardware to every gate driver enable (IEC 60204-1, ISO 13849).",
          "EtherCAT / CAN-FD / RS-485 daisy-chain with termination at both ends; isolated transceivers per joint.",
          "Encoders and current sensing for FOC at 10–40 kHz; thermal vias and a heat-sinking chassis under the FETs."}},
        {"quadruped", "Quadruped dog robot",
         "Legged robot: 12 high-torque joints, peak currents 30–60 A, constant impacts",
         {"High peak currents on every leg: 2–4 oz copper, eFuses per leg, bulk capacitance near each drive.",
          "Rigid-flex through the knee / hip joints instead of harnesses; IMU on an isolated, damped section.",
          "CAN-FD per leg with termination; hardware E-stop that drops all gate drivers at once.",
          "Shock and vibration: stake heavy parts, keep MLCCs away from mounting holes and board edges."}},
        {"humanoid", "Humanoid robot",
         "Bipedal humanoid: 20–40 joints, dense compute, people around it",
         {"Distributed joint controllers on a CAN-FD / EtherCAT backbone; each joint board carries its own power, "
          "motion and sensing segments.",
          "Rigid-flex harnesses through shoulders, elbows, hips and knees; strain relief at every mounting point.",
          "Main compute on a carrier for a Jetson-class SoM: HDI, DDR / PCIe / MIPI CSI length matching.",
          "Functional safety: hardware E-stop, current limits per joint and status indication people can see."}},
    };
    return p;
}

const RobotPlatform* findRobotPlatform(const std::string& id) {
    for (const auto& p : robotPlatforms())
        if (p.id == id) return &p;
    return nullptr;
}

bool isRobotProject(const Project& project) {
    return !project.robotPlatform.empty() || project.industry == "robotics" || project.industry == "uav";
}

std::vector<RuleViolation> roboticsChecks(const Project& project) {
    std::vector<RuleViolation> out;
    if (!isRobotProject(project)) return out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const Parts parts = classify(sch);
    const int gnd = sch.groundNet();
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {},
                   Vec2 at = {}, bool hasAt = false) {
        RuleViolation v;
        v.severity = sev;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        v.location = at;
        v.hasLocation = hasAt;
        out.push_back(v);
    };
    const bool motorDrive = !parts.drivers.empty() || !parts.powerFets.empty();

    // ---------------------------------------------------------------- 1. power distribution
    if (!parts.sources.empty()) {
        // Reverse polarity: a series diode / ideal-diode MOSFET on the supply, or a reverse-protected regulator.
        bool reverse = false;
        for (const Component* src : parts.sources) {
            const int plus = sch.netOf({src->id, 0}), minus = sch.netOf({src->id, 1});
            for (const Component* d : parts.diodes)
                reverse |= sch.netOf({d->id, 0}) == plus;  // anode on the battery +
            for (const Component* q : parts.powerFets)
                for (size_t i = 0; i < q->def().pins.size(); ++i)
                    if (pinName(*q, static_cast<int>(i)) == "S" && sch.netOf({q->id, static_cast<int>(i)}) == minus &&
                        minus != gnd)
                        reverse = true;  // low-side ideal diode in the return
        }
        for (const auto& c : sch.components()) reverse |= containsAny(partName(c), {"LM2940", "LTC4359", "LM74", "IDEAL"});
        if (!reverse && motorDrive)
            add(Severity::Warning, "REL_REVERSE_POLARITY",
                "No reverse-polarity protection on the battery input: a reversed pack destroys the drivers and logic "
                "instantly. Add an ideal-diode controller with a MOSFET (low loss), a P-MOSFET high-side or an N-MOSFET in "
                "the return path, or a series Schottky for small loads.");
        if (parts.fuses.empty() && motorDrive)
            add(Severity::Warning, "REL_NO_FUSE",
                "No fuse or eFuse on the battery input: a stalled motor or a shorted FET draws the full pack current and "
                "can start a battery fire. Add an eFuse (current limit + auto-retry) or a fuse sized for the stall current.");
    }
    {
        double widest = 0;
        for (const auto& [net, w] : s.netWidths) widest = std::max(widest, w);
        if (widest >= 2.0 && s.copperWeightOz < 2.0)
            add(Severity::Warning, "REL_COPPER_WEIGHT",
                "High-current rails need " + fmt("%.1f", widest) + " mm tracks on " + fmt("%.0f oz", s.copperWeightOz) +
                    " copper. Use 2–4 oz copper (Board Setup → copper weight) and poured planes for the battery and motor "
                    "rails, so tracks halve in width and run cooler.");
    }
    if (motorDrive && !parts.compute.empty() && parts.isolators.empty())
        add(Severity::Info, "REL_ISOLATION",
            "Motor power and logic share a ground. Isolating the gate-driver side (digital isolator such as ADuM1201 / "
            "ISO77xx, or optocouplers) keeps motor switching noise and ground bounce out of the processor.");

    // ---------------------------------------------------------------- 3. motion control
    const auto pads = pcb.pads(sch);
    const bool laidOut = !pads.empty();
    if (laidOut) {
        for (const Component* q : parts.powerFets) {
            if (!q->hasFootprint() || !q->pcb.placed) continue;
            const Pad* big = heatPad(pads, *q);
            if (!big || big->throughHole) continue;  // through-hole (TO-220) parts bolt to a heat sink instead
            int near = 0;
            for (const auto& v : pcb.vias)
                if (v.net == big->net && pointRectDistance(v.position, big->bounds()) <= 2.0) ++near;
            // Power packages (DPAK, PowerPAK, LFPAK: ≥ 2 mm² tab) take an array; small SOT-23 FETs a couple of vias.
            const int need = big->size.x * big->size.y >= 2.0 ? 4 : 2;
            if (near < need)
                add(Severity::Warning, "REL_THERMAL_VIAS",
                    q->ref + " has " + std::to_string(near) + " thermal via(s) at its drain / tab pad (needs ≥ " +
                        std::to_string(need) + "): a power FET switching motor current needs stitched vias (0.3 mm, 1 mm "
                        "pitch) into the inner planes and the chassis. Use Design → Add Thermal Vias.",
                    {q->id}, big->position, true);
        }
    }
    for (const Component* r : parts.shunts) {
        if (r->kind != ComponentKind::Resistor) continue;
        add(Severity::Info, "REL_SHUNT_KELVIN",
            r->ref + " (" + r->value + ") is a current-sense shunt: take the amplifier inputs from the inner edges of its "
            "pads with a separate pair of tracks (Kelvin connection), route them together to the amplifier, and keep the "
            "motor current out of them; a high-bandwidth amplifier tracks phase current for FOC.",
            {r->id}, r->pcb.position, r->pcb.placed);
    }
    if (!parts.drivers.empty() && parts.shunts.empty())
        add(Severity::Info, "REL_NO_CURRENT_SENSE",
            "The motor drive has no current sensing: add a low-side or in-line shunt (1–10 mΩ) with an amplifier, or a "
            "hall sensor, for torque control (FOC), stall detection and overcurrent shutdown.");

    // ---------------------------------------------------------------- 4. sensors
    if (laidOut) {
        std::vector<const Component*> noisy;
        for (auto* v : {&parts.drivers, &parts.powerFets, &parts.inductors})
            for (const Component* c : *v) noisy.push_back(c);
        for (const Component* imu : parts.imus) {
            if (!imu->hasFootprint() || !imu->pcb.placed) continue;
            double best = 1e9;
            const Component* culprit = nullptr;
            for (const Component* n : noisy)
                if (n->hasFootprint() && n->pcb.placed) {
                    double g = rectGap(courtyardOf(pcb, *imu), courtyardOf(pcb, *n));
                    if (g < best) { best = g; culprit = n; }
                }
            if (culprit && best < 10.0)
                add(Severity::Warning, "REL_IMU_PLACEMENT",
                    imu->ref + " (IMU) sits " + fmt("%.1f mm", best) + " from " + culprit->ref + ": motor vibration and "
                    "switching fields corrupt the gyro / accelerometer. Keep it ≥ 10 mm from the power stage, near the "
                    "robot's centre of mass, on a slotted / isolated board area or a soft-mounted sub-board.",
                    {imu->id, culprit->id}, imu->pcb.position, true);
        }
    }
    for (int agnd : netsNamed(sch, {"AGND", "GNDA", "ANA_GND"})) {
        if (agnd == gnd) continue;
        auto ties = bridges(sch, agnd, gnd, {ComponentKind::Resistor, ComponentKind::Inductor});
        if (ties.empty())
            add(Severity::Warning, "REL_STAR_GROUND",
                netName(sch, agnd) + " is not tied to the digital ground: join AGND and GND at exactly one star point "
                "(0 Ω resistor, net tie or ferrite) next to the ADC so analog return currents stay out of the digital plane.");
        else if (ties.size() > 1)
            add(Severity::Warning, "REL_STAR_GROUND",
                netName(sch, agnd) + " joins GND at " + std::to_string(ties.size()) + " points: that forms a ground loop "
                "through which digital and motor return currents flow in the analog ground. Keep a single star point.");
    }
    {
        auto checkBus = [&](const std::vector<int>& hi, const std::vector<int>& lo, const char* bus) {
            for (int h : hi)
                for (int l : lo) {
                    if (h == l) continue;
                    bool term = false;
                    for (const Component* r : bridges(sch, h, l, {ComponentKind::Resistor})) {
                        auto v = parseEngineeringValue(primaryValue(r->value));
                        term |= v && *v >= 100 && *v <= 140;
                    }
                    if (!term)
                        add(Severity::Warning, "REL_BUS_TERMINATION",
                            std::string(bus) + " " + netName(sch, h) + " / " + netName(sch, l) + " has no 120 Ω termination: "
                            "terminate the two ends of the bus (or note that this node is in the middle), route the pair "
                            "as a twisted / 120 Ω differential pair with a common-mode choke and TVS at the connector.");
                    return;  // one report per bus
                }
        };
        checkBus(netsNamed(sch, {"CANH", "CAN_H"}), netsNamed(sch, {"CANL", "CAN_L"}), "CAN bus");
        checkBus(netsNamed(sch, {"RS485_A", "485_A", "RS422_A"}), netsNamed(sch, {"RS485_B", "485_B", "RS422_B"}), "RS-485 bus");
    }

    // ---------------------------------------------------------------- 5. communication
    if (!parts.rf.empty()) {
        add(Severity::Info, "REL_RF_SHIELD",
            "Put a grounded shield can over the radio (" + parts.rf.front()->ref + ") with a via fence around it, and keep "
            "motor drives and DC-DC converters on the far side of the board so their switching harmonics do not jam it.");
        if (laidOut)
            for (const Component* r : parts.rf) {
                if (!r->hasFootprint() || !r->pcb.placed) continue;
                for (auto* v : {&parts.drivers, &parts.powerFets, &parts.inductors})
                    for (const Component* n : *v)
                        if (n->hasFootprint() && n->pcb.placed &&
                            rectGap(courtyardOf(pcb, *r), courtyardOf(pcb, *n)) < 10.0) {
                            add(Severity::Warning, "REL_RF_NOISE",
                                r->ref + " (radio) is within 10 mm of " + n->ref + " (switching power): move it away or "
                                "shield it; motor PWM edges couple straight into the receiver.",
                                {r->id, n->id}, r->pcb.position, true);
                            goto nextRadio;
                        }
            nextRadio:;
            }
    }
    if (laidOut)
        for (const Component* c : parts.coax)
            if (c->hasFootprint() && c->pcb.placed && s.edgeDistance(c->pcb.position) > 3.0)
                add(Severity::Warning, "REL_COAX_EDGE",
                    c->ref + " (" + c->value + ") is " + fmt("%.1f mm", s.edgeDistance(c->pcb.position)) + " from the board "
                    "edge: place coax connectors at the edge with solid ground under them and a short 50 Ω feed.",
                    {c->id}, c->pcb.position, true);

    // ---------------------------------------------------------------- 6. safety & UI
    if (motorDrive && !parts.drivers.empty()) {
        auto estop = netsNamed(sch, {"ESTOP", "E_STOP", "E-STOP", "EMERG", "KILL", "STO_", "SAFE_TORQUE"});
        if (estop.empty()) {
            add(Severity::Warning, "REL_ESTOP",
                "No hardwired emergency stop: wire an E-stop (normally-closed) to every gate driver's enable / shutdown "
                "input (IR2104 SD, DRV EN) so it cuts the drive without the microcontroller (IEC 60204-1 stop category 0, "
                "ISO 13849). Name the net ESTOP.");
        } else {
            bool reaches = false;
            for (const Component* d : parts.drivers)
                for (size_t i = 0; i < d->def().pins.size(); ++i) {
                    const std::string pn = pinName(*d, static_cast<int>(i));
                    if (containsAny(pn, {"SD", "EN", "INH", "DIS", "SLEEP", "RESET", "NFAULT"}) &&
                        std::find(estop.begin(), estop.end(), sch.netOf({d->id, static_cast<int>(i)})) != estop.end())
                        reaches = true;
                }
            if (!reaches)
                add(Severity::Warning, "REL_ESTOP",
                    "The " + netName(sch, estop.front()) + " net does not reach a gate driver enable / shutdown pin: a "
                    "hardware E-stop must cut the drivers directly, not only through the microcontroller.");
        }
    }
    if (motorDrive && parts.leds.empty())
        add(Severity::Info, "REL_STATUS_UI",
            "No status indicator: add bright diagnostic LEDs (power, armed, fault) and a buzzer / display for error codes "
            "so operators can see the robot's state.");

    // ---------------------------------------------------------------- 7. mechanical
    if (laidOut)
        for (const Component* c : parts.caps) {
            if (!c->hasFootprint() || !c->pcb.placed || c->pcb.embedded()) continue;
            const Rect cy = courtyardOf(pcb, *c);
            for (const auto& h : s.holes)
                if (pointRectDistance(h.position, cy) < h.keepout / 2 + 2.0) {
                    add(Severity::Warning, "REL_MLCC_STRAIN",
                        c->ref + " (ceramic capacitor) is " + fmt("%.1f mm", pointRectDistance(h.position, cy)) + " from a "
                        "mounting hole: chassis twist flexes the board there and cracks MLCCs, which then short (a fire "
                        "risk on the battery rail). Keep ≥ 2 mm beyond the hole keep-out and orient them parallel to the "
                        "bend line, or use soft-termination MLCCs.",
                        {c->id}, c->pcb.position, true);
                    break;
                }
        }
    if ((project.robotPlatform == "humanoid" || project.robotPlatform == "quadruped") && s.construction != "rigid-flex")
        add(Severity::Info, "REL_RIGID_FLEX_JOINTS",
            "Legged and humanoid robots route signals through articulating joints: a rigid-flex board (Board Setup → "
            "construction) replaces bulky wire harnesses there (IPC-2223, dynamic bend radius ≥ 100× flex thickness).");
    return out;
}

std::vector<RobotSegment> robotSegments(const Project& project) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const Parts parts = classify(sch);
    std::set<std::string> issues;
    for (const auto& v : roboticsChecks(project))
        if (v.severity != Severity::Info) issues.insert(v.code);
    auto refs = [](const std::vector<const Component*>& v) {
        std::string s;
        for (size_t i = 0; i < v.size() && i < 6; ++i) s += (i ? ", " : "") + v[i]->ref;
        if (v.size() > 6) s += ", …";
        return s;
    };
    auto item = [](const std::string& label, bool ok, const std::string& found, const std::string& todo) {
        return RobotCheckItem{label, ok, ok ? found : todo};
    };
    std::vector<RobotSegment> out;
    const bool motor = !parts.drivers.empty() || !parts.powerFets.empty();

    RobotSegment pdn{"power", "Power Distribution Network", "", {}, {}};
    pdn.items.push_back(item("Battery / supply input", !parts.sources.empty(), refs(parts.sources), "Add the battery or bus input"));
    pdn.items.push_back(item("Reverse-polarity protection", !issues.count("REL_REVERSE_POLARITY") && !parts.sources.empty(),
                             "Present", "Ideal diode / MOSFET in the input"));
    pdn.items.push_back(item("Fuse / eFuse", !parts.fuses.empty(), refs(parts.fuses), "eFuse or fuse sized for motor stall"));
    pdn.items.push_back(item("TVS on the bus", !parts.tvs.empty(), refs(parts.tvs), "TVS (e.g. SMBJ) for inductive spikes"));
    pdn.items.push_back(item("Motor / logic isolation", !parts.isolators.empty(), refs(parts.isolators),
                             "Digital isolator or optocoupler between gate drivers and logic"));
    pdn.items.push_back(item("High-current copper", !issues.count("REL_COPPER_WEIGHT"),
                             fmt("%.0f oz copper", pcb.settings.copperWeightOz), "2–4 oz copper for the battery / motor rails"));
    pdn.guidance = {"Thick 2–4 oz pours from the battery (6S LiPo to 48 V) to the drives; logic rails from a separate, "
                    "filtered regulator.",
                    "Ideal-diode reverse protection, TVS at the connector, eFuse per motor branch."};
    out.push_back(pdn);

    RobotSegment cpu{"compute", "Main Compute & Brain", "", {}, {}};
    cpu.items.push_back(item("Processor / SoM", !parts.compute.empty(), refs(parts.compute), "MCU, SoC or SoM carrier (Jetson, CM4)"));
    const bool matched = !issues.count("REL_LENGTH_MISMATCH");
    const bool groups = !lengthGroups(sch, pcb.settings).empty();
    cpu.items.push_back(item("High-speed links matched", matched,
                             groups ? "Pairs and buses length-matched" : "No DDR / PCIe / MIPI links on this board",
                             "Length-match the DDR / PCIe / MIPI groups"));
    bool debug = !netsNamed(sch, {"SWD", "SWCLK", "TCK", "JTAG", "UART_TX", "TXD"}).empty();
    for (const Component* c : parts.compute)
        for (size_t i = 0; i < c->def().pins.size(); ++i) {
            const std::string pn = pinName(*c, static_cast<int>(i));
            if ((pn == "SWCLK" || pn == "TCK") && sch.netOf({c->id, static_cast<int>(i)}) >= 0) {
                for (const auto& o : sch.components())
                    if (o.id != c->id && o.kind == ComponentKind::Custom) {
                        const int net = sch.netOf({c->id, static_cast<int>(i)});
                        for (size_t k = 0; k < o.def().pins.size(); ++k)
                            debug |= sch.netOf({o.id, static_cast<int>(k)}) == net;
                    }
            }
        }
    cpu.items.push_back(item("Debug / programming access", debug, "SWD / JTAG / UART brought out", "Bring out SWD / JTAG and a UART"));
    // Fine-pitch processors (≤ 0.5 mm pitch BGA / QFN) need HDI or ≥ 6 layers to break out.
    double finest = 1e9;
    {
        std::map<int, std::vector<Vec2>> byPart;
        for (const auto& p : pcb.pads(sch))
            if (!p.throughHole) byPart[p.componentId].push_back(p.position);
        for (const Component* c : parts.compute) {
            const auto& pts = byPart[c->id];
            for (size_t a = 0; a < pts.size(); ++a)
                for (size_t b = a + 1; b < pts.size(); ++b) finest = std::min(finest, (pts[a] - pts[b]).length());
        }
    }
    const bool needHdi = finest <= 0.5 + 1e-9;
    cpu.items.push_back(item("HDI for fine-pitch parts", !needHdi || pcb.settings.hdi || pcb.settings.layerCount >= 6,
                             needHdi ? std::to_string(pcb.settings.layerCount) + " layers" + (pcb.settings.hdi ? ", HDI" : "")
                                     : "No fine-pitch processor",
                             "HDI / VIPPO (Board Setup) to break out the fine-pitch processor"));
    cpu.guidance = {"SoM carrier (Jetson Orin, CM4): HDI, microvias and VIPPO; DDR4/5, PCIe (NVMe) and MIPI CSI length-"
                    "matched and impedance-controlled, backdrilled."};
    out.push_back(cpu);

    RobotSegment mot{"motion", "Motion Control & Actuation", "", {}, {}};
    mot.items.push_back(item("Gate / motor driver", !parts.drivers.empty(), refs(parts.drivers), "Gate driver or motor driver IC"));
    mot.items.push_back(item("Power stage (MOSFET / GaN)", !parts.powerFets.empty(), refs(parts.powerFets), "Low-RDS(on) MOSFETs / GaN FETs"));
    mot.items.push_back(item("Current sensing", !parts.shunts.empty(), refs(parts.shunts), "Kelvin shunt + amplifier or hall sensor"));
    mot.items.push_back(item("Thermal vias under FETs", motor && !issues.count("REL_THERMAL_VIAS"), "Stitched", "PCB → Add Thermal Vias"));
    mot.items.push_back(item("Flyback / shoot-through safe", motor && !issues.count("REL_FLYBACK") && !issues.count("REL_SHOOT_THROUGH"),
                             "Checked", "Flyback paths and a gate driver with dead time"));
    mot.guidance = {"H-bridges / three-phase bridges with gate drivers and dead time; Kelvin-sensed shunts for FOC; thermal "
                    "via arrays into the chassis."};
    out.push_back(mot);

    RobotSegment sen{"sensors", "Sensor Ingestion & Conditioning", "", {}, {}};
    sen.items.push_back(item("IMU", !parts.imus.empty(), refs(parts.imus), "IMU / gyroscope"));
    sen.items.push_back(item("IMU away from motors", !parts.imus.empty() && !issues.count("REL_IMU_PLACEMENT"), "≥ 10 mm from the power stage",
                             "Isolate the IMU from vibration and switching"));
    const bool agnd = !netsNamed(sch, {"AGND", "GNDA", "ANA_GND"}).empty();
    sen.items.push_back(item("Analog ground star point", agnd && !issues.count("REL_STAR_GROUND"), "AGND tied at one point",
                             "AGND plane joined to GND at one star point"));
    sen.items.push_back(item("Field bus (CAN-FD / RS-485)", !parts.fieldbus.empty() && !issues.count("REL_BUS_TERMINATION"),
                             refs(parts.fieldbus) + ", terminated", "CAN / RS-485 transceiver with 120 Ω termination"));
    sen.guidance = {"IMU on an isolated or soft-mounted area; AGND / DGND star ground; differential CAN-FD / RS-485 with "
                    "termination and TVS."};
    out.push_back(sen);

    RobotSegment com{"comms", "Communication & Telemetry", "", {}, {}};
    com.items.push_back(item("Radio module", !parts.rf.empty(), refs(parts.rf), "Wi-Fi / BLE / LTE / LoRa / telemetry radio"));
    com.items.push_back(item("Radio clear of switching noise", !parts.rf.empty() && !issues.count("REL_RF_NOISE"), "≥ 10 mm, shield can",
                             "Shield can and distance from drives"));
    com.items.push_back(item("Antenna / coax at the edge", !issues.count("REL_COAX_EDGE"),
                             parts.coax.empty() ? "Module antenna" : refs(parts.coax), "U.FL / SMA at the board edge"));
    com.guidance = {"50 Ω controlled-impedance antenna feeds (Rogers or RF FR-4), shield cans with via fences, coax at the "
                    "edge over solid ground."};
    out.push_back(com);

    RobotSegment saf{"safety", "Feedback, UI & Safety Interlock", "", {}, {}};
    const bool estop = !netsNamed(sch, {"ESTOP", "E_STOP", "E-STOP", "EMERG", "KILL", "STO_", "SAFE_TORQUE"}).empty();
    saf.items.push_back(item("Hardwired E-stop to the drivers", estop && !issues.count("REL_ESTOP"), "ESTOP cuts the gate drivers",
                             "E-stop wired to every driver enable"));
    saf.items.push_back(item("Status indicators", !parts.leds.empty(), refs(parts.leds), "LEDs / buzzer / display"));
    saf.guidance = {"E-stop bypasses the microcontroller (stop category 0); bright status LEDs, buzzer codes, OLED / LCD."};
    out.push_back(saf);

    RobotSegment mech{"mechanical", "Mechanical & Form Factor", "", {}, {}};
    mech.items.push_back(item("Mounting holes", !pcb.settings.holes.empty(), std::to_string(pcb.settings.holes.size()) + " holes",
                              "Mounting holes with keep-outs"));
    mech.items.push_back(item("MLCC strain relief", !issues.count("REL_MLCC_STRAIN"), "No MLCC near the holes", "Move MLCCs away from holes"));
    const bool joints = project.robotPlatform == "humanoid" || project.robotPlatform == "quadruped";
    mech.items.push_back(item("Rigid-flex through joints", !joints || pcb.settings.construction == "rigid-flex",
                              joints ? "Rigid-flex" : "Not needed on this platform", "Rigid-flex construction for the joints"));
    mech.guidance = {"Rigid-flex through articulating joints; keep-outs and strain relief around mounting holes; outline "
                     "and stacking patterns from the mechanical design."};
    out.push_back(mech);

    for (auto& seg : out) {
        int ok = 0;
        for (const auto& i : seg.items) ok += i.ok;
        seg.status = ok == static_cast<int>(seg.items.size()) ? "complete" : ok == 0 ? "missing" : "partial";
    }
    return out;
}

Json robotSegmentsJson(const Project& project) {
    Json root = Json::object();
    root["platform"] = project.robotPlatform;
    root["applies"] = isRobotProject(project);
    Json plats = Json::array();
    for (const auto& p : robotPlatforms()) {
        Json j = Json::object();
        j["id"] = p.id;
        j["name"] = p.name;
        j["description"] = p.description;
        Json g = Json::array();
        for (const auto& x : p.guidance) g.push(x);
        j["guidance"] = g;
        plats.push(j);
    }
    root["platforms"] = plats;
    Json segs = Json::array();
    for (const auto& seg : robotSegments(project)) {
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

int autoThermalVias(Project& project) {
    if (!isRobotProject(project) || project.pcb.settings.layerCount < 2) return 0;
    const Parts parts = classify(project.schematic);
    int added = 0;
    for (const Component* q : parts.powerFets)
        if (q->hasFootprint() && q->pcb.placed) added += addThermalVias(project, q->id, 6);
    return added;
}

int addThermalVias(Project& project, int componentId, int maxVias) {
    PcbLayout& pcb = project.pcb;
    const Schematic& sch = project.schematic;
    const BoardSettings& s = pcb.settings;
    if (s.layerCount < 2) return 0;
    const auto pads = pcb.pads(sch);
    const Component* comp = sch.find(componentId);
    const Pad* big = comp ? heatPad(pads, *comp) : nullptr;
    if (!big) return 0;
    const double drill = std::max(0.3, s.minDrill), dia = std::max(drill + 2 * s.minAnnularRing, 0.6), pitch = dia + 0.5;
    const Rect area = big->bounds().inflated(big->throughHole ? 2.0 : 1.5);
    const Pad bigPad = *big;
    auto fits = [&](Vec2 p) {
        if (s.edgeDistance(p) < s.edgeClearance + dia / 2 || s.holeDistance(p) < dia / 2) return false;
        for (const auto& pd : pads) {
            const double d = pd.round ? (p - pd.position).length() - std::min(pd.size.x, pd.size.y) / 2
                                      : pointRectDistance(p, pd.bounds());
            if (pd.net == bigPad.net && !pd.throughHole) continue;  // own-net SMD copper: vias may sit in it
            if (d - dia / 2 < s.clearance) return false;
        }
        for (const auto& t : pcb.tracks)
            if (t.net != bigPad.net && pointSegmentDistance(p, t.a, t.b) - t.width / 2 - dia / 2 < s.clearance) return false;
        for (const auto& v : pcb.vias)
            if ((v.position - p).length() - (v.diameter + dia) / 2 < (v.net == bigPad.net ? 0.15 : s.clearance)) return false;
        return true;
    };
    int added = 0;
    std::vector<Vec2> cand;
    for (double y = area.y0 + dia / 2; y <= area.y1 - dia / 2 + 1e-9; y += pitch)
        for (double x = area.x0 + dia / 2; x <= area.x1 - dia / 2 + 1e-9; x += pitch) cand.push_back({x, y});
    // Closest to the pad first: under an SMD pad the via is in the copper; outside it a short track joins it.
    std::sort(cand.begin(), cand.end(), [&](Vec2 a, Vec2 b) {
        return pointRectDistance(a, bigPad.bounds()) < pointRectDistance(b, bigPad.bounds());
    });
    const int layer = bigPad.throughHole ? kTopLayer : bigPad.smdLayer;
    // A via's keep-out in a pour can split it into islands: keep only vias that leave connectivity as it was.
    const size_t openBefore = pcb.ratsnest(sch).size();
    for (Vec2 p : cand) {
        if (added >= maxVias) break;
        if (!fits(p)) continue;
        const bool inside = pointRectDistance(p, bigPad.bounds()) <= 1e-9;
        if (inside && (!s.viaInPad || bigPad.throughHole)) continue;  // in-pad vias need VIPPO (filled and capped)
        if (!inside) {
            // Short stub from the via to the pad on the pad's layer, clear of other nets.
            Vec2 target{std::clamp(p.x, bigPad.bounds().x0, bigPad.bounds().x1), std::clamp(p.y, bigPad.bounds().y0, bigPad.bounds().y1)};
            const double w = std::max(s.trackWidth, 0.4);
            bool ok = true;
            for (const auto& pd : pads)
                if (pd.net != bigPad.net && pd.onLayer(layer) &&
                    segmentRectDistance(p, target, pd.bounds()) - w / 2 < s.clearance)
                    ok = false;
            for (const auto& t : pcb.tracks)
                if (ok && t.net != bigPad.net && t.layer == layer &&
                    segmentSegmentDistance(p, target, t.a, t.b) - (w + t.width) / 2 < s.clearance)
                    ok = false;
            if (!ok) continue;
            Track t;
            t.net = bigPad.net;
            t.layer = layer;
            t.width = w;
            t.a = p;
            t.b = target;
            pcb.addTrack(t);
        }
        Via v;
        v.net = bigPad.net;
        v.position = p;
        v.drill = drill;
        v.diameter = dia;
        pcb.addVia(v);
        if (pcb.ratsnest(sch).size() > openBefore) {
            pcb.vias.pop_back();
            if (!inside) pcb.tracks.pop_back();
            continue;
        }
        ++added;
    }
    return added;
}

}  // namespace sieda
