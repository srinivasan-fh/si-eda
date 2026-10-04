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
    p.push_back({"medical", "Medical Devices", "Patient monitors, wearables, diagnostic and therapy equipment",
                 "IEC 60601-1 (MOOP/MOPP isolation, leakage), IEC 60601-1-2 (EMC), ISO 14971 (risk), IEC 62304 (software), "
                 "ISO 13485, IPC-A-610 Class 3",
                 "Medical (IEC 60601-1, IPC Class 3)", 0.6, 0.7, false, 0, 50,
                 {"Isolate every patient-connected (applied) part: 2 MOPP = 4 kV AC test, 8 mm creepage, 5 mm clearance; use "
                  "medical-grade isolated DC/DC converters and digital isolators (ADuM1201) across the barrier.",
                  "Keep patient leakage below 10 µA (type CF, cardiac) or 100 µA (type BF) — no Y-capacitors across the "
                  "patient barrier.",
                  "Protect electrode inputs with series resistors (≥ 10 kΩ) and clamp diodes; they limit fault current "
                  "into the patient and survive defibrillation with a gas discharge tube.",
                  "Bias the instrumentation amplifier (INA333) to mid-supply and add a right-leg drive to reject 50/60 Hz "
                  "common-mode interference.",
                  "Run the device from a battery or an IEC 60601-1 certified supply; fuse the battery.",
                  "Develop firmware to IEC 62304 and document hazards in an ISO 14971 risk file."}});
    p.push_back({"defence", "Defence & Military", "Rugged military, avionics, radar and tactical communication electronics",
                 "MIL-STD-810H (environment), MIL-STD-461G (EMI), MIL-STD-704F (28 V aircraft power), MIL-STD-1275E "
                 "(vehicle 28 V), MIL-PRF-31032, IPC-6012 Class 3/A, MIL-HDBK-1547 / NAVSO P-3641A (derating)",
                 "Defence (IPC-6012 Class 3/A, MIL)", 0.5, 0.6, true, -55, 125,
                 {"Protect the 28 V input to MIL-STD-704/1275: fuse, reverse-polarity diode, TVS (SMBJ33A) for spikes and "
                  "a regulator that survives 50-100 V surges.",
                  "Filter conducted emissions for MIL-STD-461 CE102/CS101 with an LC input filter (inductor + ceramic "
                  "capacitors) at the connector.",
                  "Derate to MIL-HDBK-1547: power 50 %, semiconductor current 50-60 %, capacitor voltage 50 %.",
                  "Specify −55 °C to +125 °C parts, avoid pure-tin finishes (whiskers) and conformal-coat the board.",
                  "Stake heavy parts and add mounting holes near them for MIL-STD-810 shock and vibration.",
                  "Airborne equipment uses the IPC-2221 B3 (altitude) voltage spacing — DRC checks it."}});
    p.push_back({"networking", "Networking & Telecom", "Ethernet, Power over Ethernet, switches, routers and line cards",
                 "IEEE 802.3 (Ethernet; PoE 802.3af/at/bt), IEC 62368-1, ITU-T K.21 (surge), IPC-2141 (100 Ω "
                 "differential), CISPR 32 (EMC)",
                 "High-Speed Digital (100 Ω diff)", 0.8, 0.8, false, 0, 70,
                 {"Route Ethernet MDI pairs as 100 Ω differential pairs, length-matched within 0.5 mm, over an "
                  "unbroken ground plane; keep them away from the board edge.",
                  "Isolate the RJ45 with 1500 V rms magnetics and terminate unused pairs with Bob Smith termination "
                  "(75 Ω to a 1 nF / 2 kV capacitor to chassis).",
                  "PoE powered device: diode or ideal-diode bridge for either polarity, a 24.9 kΩ detection signature, "
                  "a 58 V TVS and inrush limiting before the bulk capacitor.",
                  "Place the 25 MHz PHY crystal close to the PHY with short, guarded traces.",
                  "Protect ports with TVS arrays for ITU-T K.21 surges and leave chassis-to-ground spacing for "
                  "1500 V isolation."}});
    p.push_back({"vlsi", "VLSI / ASIC & FPGA", "Chip bring-up, ASIC/FPGA evaluation boards, high-pin-count BGA and QFN",
                 "IPC-2226 (HDI), IPC-7351 (BGA/QFN land patterns), IPC-2152, JEDEC JESD8 (I/O levels), "
                 "IEEE 1149.1 (JTAG), IPC-6012 Class 3",
                 "HDI / Fine-Pitch BGA (IPC-2226)", 0.8, 0.8, false, 0, 85,
                 {"Give every rail (core, auxiliary, I/O) its own regulator and plane; sequence core → auxiliary → I/O "
                  "as the device datasheet requires.",
                  "Decouple each rail with bulk (10 µF), mid (1 µF) and one 100 nF per power-ball pair, placed under "
                  "the BGA on the back side with via-in-pad.",
                  "Bring out IEEE 1149.1 JTAG (TCK, TMS, TDI, TDO, VREF, GND) on a header for programming and "
                  "boundary scan.",
                  "Fan out fine-pitch BGAs with dog-bone or via-in-pad microvias (HDI rules) and keep length-matched "
                  "high-speed nets on inner layers between planes.",
                  "Add test points and current-sense resistors on each rail for bring-up power measurements."}});
    // Computing platforms: the publicly documented form factors and interfaces (Intel, AMD, ARM, NVIDIA, Microsoft
    // and Google platforms build on them); vendor platform design guides still govern a real product.
    p.push_back({"motherboard", "Motherboards & PCs (Intel / AMD / ARM)",
                 "Desktop, workstation and laptop mainboards: CPU socket, VRM, DDR5, PCIe, USB4",
                 "ATX 3.1 / ATX12VO, PCI Express CEM 5.0 / 6.0, JEDEC JESD79-5 (DDR5), USB4 / USB 3.2, "
                 "Intel VR / AMD SVI3 voltage-regulator interfaces, IPC-6012 Class 2, IPC-2152",
                 "High-Speed Digital (100 Ω diff)", 0.8, 0.8, false, 0, 70,
                 {"CPU VRM: one phase per ~40–60 A of core current (Intel Core / AMD Ryzen: 8–20 phases), interleaved, "
                  "inductors and power stages close to the socket, wide 2 oz copper and dozens of vias per phase.",
                  "PCIe: 85 Ω differential pairs, 0.22 µF AC-coupling capacitors on the transmitter side, ≤ 5 mil "
                  "intra-pair skew; Gen 5 (32 GT/s) and Gen 6 need mid-/low-loss laminate and backdrilled vias.",
                  "DDR5: 40 Ω single-ended data / 80 Ω differential strobes; match DQ to DQS within each byte lane and "
                  "keep the fly-by command / address bus on inner layers between planes.",
                  "USB 3.2 / USB4: 90 Ω pairs with common-mode chokes and ESD arrays at the connector.",
                  "Typical stack-up: 6–10 layers FR-4 mid-loss (8-layer is common), GND reference under every "
                  "high-speed layer; ATX12VO boards make 5 V / 3.3 V on board from 12 V."}});
    p.push_back({"server", "Servers & Mainframes",
                 "Intel Xeon / AMD EPYC / ARM Neoverse servers, OCP and hyperscale boards, mainframe-class RAS",
                 "OCP DC-MHS (modular hardware system), OCP Open Rack v3 48 V, PCI Express CEM / EDSFF, JEDEC DDR5 "
                 "RDIMM, PMBus 1.3 / SMBus, DMTF Redfish (BMC), IPC-6012 Class 3 for mainframe RAS",
                 "HDI / Fine-Pitch BGA (IPC-2226)", 0.7, 0.7, false, 0, 55,
                 {"48 V rack power (OCP Open Rack v3, the hyperscale 48 V bus): hot-swap controller with inrush "
                  "limiting, OR-ing between redundant PSUs, then 48 V → 12 V intermediate bus converters.",
                  "Two CPU sockets (Xeon / EPYC) with 8–12 DDR5 RDIMM channels each: length-match each channel, "
                  "route on 12–16 layers of low-loss laminate (Megtron 6 / 7) and backdrill the via stubs.",
                  "A BMC (management controller) watches every rail over PMBus / I2C with pull-ups, and feeds Redfish "
                  "/ IPMI; give each rail a current monitor and power-good.",
                  "Mainframe-class RAS: redundant regulators (N+1), ECC memory, no single point of failure on the "
                  "power path, IPC Class 3 fabrication and conformal / clean-room assembly.",
                  "OCP NIC 3.0, EDSFF E1/E3 and PCIe risers carry I/O: keep their connectors on the board edge with "
                  "the high-speed lanes routed straight to them."}});
    p.push_back({"hpc", "Supercomputers & AI Accelerators",
                 "GPU / accelerator baseboards (NVIDIA HGX-class, AMD Instinct, TPU-style trays), HPC compute blades",
                 "OCP OAM (Accelerator Module) & UBB (Universal Baseboard), OCP Open Rack v3 48 V / ORv3 busbar, "
                 "PCI Express 5.0 / 6.0, OIF CEI-112G, IPC-6012 Class 3, IPC-2152",
                 "HDI / Fine-Pitch BGA (IPC-2226)", 0.7, 0.7, false, 0, 45,
                 {"Power: 48 V in, 48 V → 12 V (or direct 48 V → core) converters next to each accelerator; a 700–1000 W "
                  "module draws 1000 A+ at < 1 V, so use multi-phase vertical power delivery under the package.",
                  "Accelerator-to-accelerator links (NVLink / Infinity Fabric / ICI-class) and 112G PAM4 SerDes: 85–100 Ω "
                  "pairs on ultra-low-loss laminate (Megtron 7 / Tachyon 100G), backdrilled, length-matched in each "
                  "lane group.",
                  "Stack-ups of 20–26 layers with HDI (laser microvias, VIPPO) to fan out 0.8–1 mm-pitch BGAs and "
                  "connector fields.",
                  "Cold-plate liquid cooling: keep-outs for the plates and leak detection; derate every part for the "
                  "hot spots around the accelerators.",
                  "Supercomputers scale these nodes: management (BMC), clock distribution and per-rail telemetry on "
                  "every board."}});
    p.push_back({"arm", "ARM SoC & Compute Modules",
                 "Cortex-A / Neoverse SoCs, systems-on-module and carrier boards, single-board computers",
                 "SGET SMARC 2.1, PICMG COM-HPC / COM Express, Qseven, JEDEC LPDDR4X / LPDDR5, MIPI CSI-2 / DSI, "
                 "IPC-2226 (HDI), IPC-6012 Class 2",
                 "HDI / Fine-Pitch BGA (IPC-2226)", 0.8, 0.8, false, -20, 85,
                 {"The SoC's PMIC sequences the rails (core, DDR, I/O); follow the SoC vendor's power-up order and keep "
                  "each rail's decoupling under the BGA.",
                  "LPDDR4X / LPDDR5: point-to-point, 40 Ω single-ended, byte-lane length matching; place the memory "
                  "next to the SoC (or package-on-package).",
                  "MIPI CSI-2 / DSI and USB: 100 Ω / 90 Ω pairs; Ethernet RGMII length-matched (or SGMII pairs).",
                  "System-on-module + carrier: put the high-speed SoC/DRAM on a small HDI module (8–10 layers), and the "
                  "connectors, power input and I/O on a cheaper 4–6 layer carrier through a SMARC / COM-HPC connector.",
                  "Boot and debug: boot-mode straps, UART console and SWD / JTAG on test points."}});
    p.push_back({"addin", "Daughterboards & Add-in Cards",
                 "PCIe add-in cards, OCP NIC 3.0 / mezzanines, M.2 modules, risers and daughter cards",
                 "PCI Express CEM 5.0 / 6.0 (card outline, gold fingers), OCP NIC 3.0, PCI-SIG M.2, "
                 "IPC-6012 Class 2/3, IPC-4552 / IPC-4556 (ENIG / hard gold)",
                 "High-Speed Digital (100 Ω diff)", 0.8, 0.8, false, 0, 55,
                 {"PCIe card: 1.57 mm thick, edge fingers in hard (electrolytic) gold ~0.76 µm over nickel, chamfered "
                  "20–45°; respect the CEM outline, bracket and keep-outs.",
                  "Power from the slot (12 V / 3.3 V, 75 W for x16; more through 12V-2x6 connectors) with a "
                  "hot-swap / inrush limiter and per-rail fuses.",
                  "AC-couple the transmit lanes (0.22 µF) on the card, 85 Ω pairs from the fingers straight to the "
                  "device, REFCLK as a 100 Ω pair.",
                  "Mezzanine / daughterboards (OCP NIC 3.0, risers): board-to-board connectors with ground pins between "
                  "pairs, stacking height and keep-outs from the mechanical drawing.",
                  "Hot-plug: PRSNT# / presence detect and PERST# from the host; ESD on any external connector."}});
    p.push_back({"retail", "Retail & POS", "Payment terminals, self-service kiosks and receipt printers",
                 "PCI PTS POI v6 (tamper response), EMVCo Level 1 (contact / contactless), IEC 62368-1 (safety), "
                 "IEC 61000-4-2 level 4 (±15 kV ESD), FCC Part 15 B / EN 55032",
                 "Fab House Advanced (4/4 mil)", 0.8, 0.8, false, 0, 50,
                 {"Put the secure element under an active tamper mesh on two inner layers (no vias through the secure "
                  "area), wire case-open switches to its tamper inputs and keep the keys on a backup cell.",
                  "Thermal printers: ≥ 470 µF low-ESR at the head supply, a current-limited stepper H-bridge on a thermal "
                  "pad, flyback diodes on the cutter and drawer solenoids.",
                  "Opto-isolate the 24 V cash-drawer kick; fan peripherals out through a USB hub; 100 Ω display pairs.",
                  "TVS on every customer-facing port (±15 kV air) and an acrylic conformal coat against spills."}});
    p.push_back({"appliance", "Home Appliances", "White goods, kitchen and HVAC controllers on AC mains",
                 "IEC 60335-1 / UL 60335-1 (household safety), IEC 60730-1 (controls, Class B software), IEC 60664-1 "
                 "(creepage), IEC 61000-4-4 / -4-5 (burst / surge), CISPR 14-1 (emissions), UL 94 V-0",
                 "IPC-2221 Class 2", 0.7, 0.8, false, 0, 85,
                 {"Mains entry: slow-blow fuse, MOV, X capacitor and common-mode choke before the converter; Y capacitors "
                  "only across the safety barrier, sized for leakage.",
                  "Keep IEC 60335 / 60664 creepage between mains and SELV (≥ 2.5 mm functional, 6–8 mm reinforced); route "
                  "mains on its own side of the board.",
                  "Triacs and SSRs with RC snubbers, a zero-crossing detector for phase control, an IPM for the inverter "
                  "compressor or drum motor.",
                  "Capacitive touch pads with guard rings, NTC dividers with RC filters, Wi-Fi module antenna kept clear "
                  "of mains copper."}});
    p.push_back({"memory", "Memory & DRAM Design", "SDRAM, DDR3L / DDR4 / LPDDR memory-down and DDR5 memory modules",
                 "JEDEC JESD79-3 / -4 / -5 (DDR3 / DDR4 / DDR5), JESD209-4 / -5 (LPDDR4 / LPDDR5), JESD21-C (SDR SDRAM, "
                 "SPD), JESD301 (DDR5 PMIC), JESD82 (RCD), IPC-2141 (controlled impedance), IPC-2226 (HDI)",
                 "HDI / Fine-Pitch BGA (IPC-2226)", 0.8, 0.8, false, 0, 85,
                 {"Decouple every VDD / VDDQ pin pair with 100 nF at the pin, add bulk at the DRAM; DDR adds a sink / "
                  "source VTT regulator (VDDQ / 2) and a filtered VREF.",
                  "SDR: a 22–33 Ω series resistor on SDCLK and a length-matched data bus. DDR3 / DDR4: fly-by command / "
                  "address and clock terminated to VTT, CK as a 100 Ω pair, 240 Ω 1 % on ZQ, RESET_n pulled low.",
                  "Match each byte lane to its DQS strobe; 40 Ω single-ended / 80 Ω differential for DDR4 / DDR5 / LPDDR; "
                  "route the bus over an unbroken ground plane on ≥ 4 layers (6–10 for DDR).",
                  "Modules: SPD EEPROM / hub with I²C pull-ups, DDR5 PMIC, RDIMM RCD; 1.2–1.27 mm board with hard-gold "
                  "bevelled fingers."}});
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
