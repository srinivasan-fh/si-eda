#include "sieda/Industry.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>

namespace sieda {

namespace {
std::vector<IndustryProfile> build() {
    std::vector<IndustryProfile> p;
    p.push_back({"general", "General Electronics", "Consumer and general-purpose boards",
                 "IPC-2221, IPC-A-610 Class 2", "IPC-2221 Class 2", 1.0, 1.0, false, 0, 70,
                 {"Decouple every IC supply pin with 100 nF next to the pin.",
                  "Use E24 resistors and E12 capacitors unless a tolerance demands E96."}});
    p.push_back({"robotics", "Robotics & Motor Control", "Motor drives, servo and actuator controllers, BLDC/stepper",
                 "IEC 61800-5-1, IEC 60034, IPC-2221 Class 3", "IPC-2221 Class 3", 0.8, 0.8, false, -20, 85,
                 {"Put a flyback/freewheel diode across every inductive load (motor, relay, solenoid).",
                  "Add a gate resistor (10-100 Ω) and a gate pull-down (10-100 kΩ) to every power MOSFET.",
                  "Measure motor current with a shunt or Hall sensor (ACS712) for stall and over-current protection.",
                  "Keep the power loop (bulk capacitor, switch, diode) tight; separate power and logic grounds at one point.",
                  "Use a half-bridge gate driver (IR2104) with a bootstrap diode and capacitor for high-side N-MOSFETs."}});
    p.push_back({"power", "Power Electronics", "SMPS, converters, inverters, chargers and battery systems",
                 "IEC 62368-1, IEC 61204, IPC-2221 B2 spacing, IPC-2152", "IPC-2221 Class 3", 0.7, 0.8, false,
                 -40, 85,
                 {"Keep the switching loop area minimal; place input capacitors at the switch.",
                  "Size tracks with IPC-2152/2221 for the RMS current and keep creepage/clearance for the working voltage.",
                  "Use current-mode control (UC3843) with a sense resistor and leading-edge blanking.",
                  "Add snubbers across switches and freewheel diodes for inductive energy.",
                  "Derate MOSFET voltage to 80 % and capacitor voltage to 50-80 % of rating.",
                  "Above 50 V switch to the High Voltage (IPC-2221 B2) rules; DRC checks voltage spacing automatically."}});
    p.push_back({"automotive", "Automotive", "Vehicle ECUs, body electronics, sensors, lighting (12/24 V)",
                 "AEC-Q100/Q101/Q200, ISO 16750-2, ISO 7637-2, ISO 11898 (CAN), IPC-6012 Class 3/A",
                 "Automotive (IPC-6012 Class 3/A)", 0.6, 0.7, false, -40, 125,
                 {"Protect the battery input: fuse, reverse-polarity diode or ideal diode, TVS for ISO 7637 pulses.",
                  "Use a load-dump tolerant regulator (LM2940) rated for 40 V transients.",
                  "Terminate CAN with 120 Ω at each end of the bus (TJA1050) and keep CANH/CANL routed as a pair.",
                  "Specify AEC-Q qualified parts and -40 °C to +125 °C ratings.",
                  "Avoid parts in the vibration-critical board edge; add mounting holes near heavy components."}});
    p.push_back({"uav", "Drones & UAV", "Flight controllers, ESCs, power distribution and radio for multirotors",
                 "IPC-2221 Class 3, IPC-A-610 Class 3, RTCA DO-160G (vibration, EMI), ASTM F3002 (sUAS C2), EN 4709-001",
                 "IPC-2221 Class 3", 0.8, 0.8, false, -20, 60,
                 {"Put a freewheel diode across every brushed motor; drive it with a logic-level MOSFET (SI2302, AO3400) "
                  "with a gate resistor and a gate pull-down so the motors stay off while the MCU resets.",
                  "Size battery and motor nets for the stall current (the autorouter widens them from the simulation) and "
                  "pour ground on both layers — or use a 4-layer board with a ground plane.",
                  "Place a low-ESR bulk capacitor (≥ 220 µF) at the battery input to absorb motor transients; run the "
                  "3.3 V logic and radio from an LDO.",
                  "Mount the IMU near the frame centre, away from the motor drivers, and decouple it at the pins.",
                  "Use the quad-X outline preset with motor connectors at the arm tips and an M3 30.5 × 30.5 mm "
                  "(or M2 20 × 20 mm) mounting pattern.",
                  "Single-cell LiPo: charge with a TP4056 (1.2 kΩ PROG = 1 A) and cut the motors at 3.3 V per cell."}});
    p.push_back({"rf", "RF & Wireless", "Radio front ends, filters, matching networks, antennas",
                 "IPC-2141 (controlled impedance), IPC-2221, ETSI EN 300 220 / FCC Part 15",
                 "RF (Controlled Impedance)", 0.8, 0.8, false, -20, 85,
                 {"Route RF on 50 Ω controlled-impedance microstrip over an unbroken ground plane.",
                  "Use C0G/NP0 capacitors and high-Q (wire-wound/multilayer) inductors in filters and matching networks.",
                  "Keep RF traces short and straight; stitch ground vias along RF lines and around the shield.",
                  "Decouple RF supplies with several values (100 pF, 10 nF, 1 µF) next to the pin.",
                  "Leave space for a pi-matching network at the antenna port."}});
    p.push_back({"space", "Space", "Satellite, launcher and payload electronics",
                 "ECSS-Q-ST-30-11C (derating), ECSS-Q-ST-70-12C, NASA EEE-INST-002, IPC-6012 Class 3/A",
                 "Space (IPC-6012 Class 3/A, ECSS)", 0.5, 0.5, true, -55, 125,
                 {"Derate parts to ECSS-Q-ST-30-11C: power to 50 %, semiconductor current to 50-75 %.",
                  "Use redundant (cold/hot) supplies with diode OR-ing and current limiting (LCL).",
                  "Prefer radiation-tolerant parts; avoid plastic parts without screening and plastic relays.",
                  "Voltage spacing follows IPC-2221 B3 (vacuum/altitude) — corona risk above 200 V.",
                  "No electrolytic or tantalum capacitors without derating; avoid pure-tin finishes (whiskers)."}});
    p.push_back({"marine", "Marine & Ship", "Navigation, engine-room, NMEA instruments and bridge equipment",
                 "IEC 60945, IEC 61162 (NMEA 0183/2000), DNV-CG-0339, IPC-2221 Class 3", "IPC-2221 Class 3", 0.7, 0.75,
                 false, -25, 70,
                 {"Isolate NMEA/RS-485 interfaces (optocoupler or isolated transceiver) and terminate the bus with 120 Ω.",
                  "Plan for conformal coating and salt-mist: wider spacing, no exposed copper, sealed connectors.",
                  "Protect 12/24 V ship supplies against reverse polarity and surges (IEC 60945 power tests).",
                  "Respect compass safe distance — avoid large magnetic parts near the enclosure top."}});
    p.push_back({"industrial", "Industrial Automation", "PLC I/O, sensors, drives and fieldbus devices (24 V)",
                 "IEC 61131-2, IEC 61000-6-2/4 (EMC), IEC 60664-1, IPC-2221 Class 3", "IPC-2221 Class 3", 0.75, 0.8, false,
                 -25, 70,
                 {"Isolate 24 V field I/O from logic with optocouplers (PC817) and keep the barrier clear of copper.",
                  "IEC 61131-2 Type 1/3 inputs: on above 15 V / 2 mA, off below 5 V.",
                  "Protect field inputs with series resistors, TVS diodes and reverse-polarity protection.",
                  "Use RS-485 (MAX485) for long fieldbus runs with termination and fail-safe bias."}});
    return p;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

const std::vector<IndustryProfile>& industryProfiles() {
    static const std::vector<IndustryProfile> profiles = build();
    return profiles;
}

const IndustryProfile* findIndustry(const std::string& id) {
    std::string key = lower(id);
    for (const auto& p : industryProfiles())
        if (p.id == key) return &p;
    return nullptr;
}

PartRatings deratedRatings(const IndustryProfile& profile, const PartRatings& base) {
    PartRatings r = base;
    r.resistorPower *= profile.powerDerating;
    r.npnPower *= profile.powerDerating;
    r.nmosPower *= profile.powerDerating;
    r.ledCurrent *= profile.currentDerating;
    r.diodeCurrent *= profile.currentDerating;
    r.npnCurrent *= profile.currentDerating;
    r.nmosCurrent *= profile.currentDerating;
    r.powerFactor = profile.powerDerating;
    r.currentFactor = profile.currentDerating;
    if (profile.powerDerating < 1.0 || profile.currentDerating < 1.0) {
        char buf[160];
        std::snprintf(buf, sizeof buf, "%s derating: power %.0f %%, current %.0f %% of rating", profile.name.c_str(),
                      profile.powerDerating * 100, profile.currentDerating * 100);
        r.derating = buf;
    }
    return r;
}

}  // namespace sieda
