// SiEDA Core (internal) — classifying a design's parts for the system-architecture checks shared by the robotics
// segments and the automotive ECU segments: drivers, power stages, sensors, radios, transceivers, protection…
#pragma once

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "sieda/CustomParts.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/Units.hpp"

namespace sieda::sysparts {

inline std::string up(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

inline bool containsAny(const std::string& hay, std::initializer_list<const char*> needles) {
    for (const char* n : needles)
        if (hay.find(n) != std::string::npos) return true;
    return false;
}

inline std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

inline std::string partName(const Component& c) {
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* p = CustomPartRegistry::instance().find(c.customPart)) return up(p->spec.name);
    return up(c.def().name);
}

inline std::string pinName(const Component& c, int pin) {
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
    // Automotive ECU blocks.
    std::vector<const Component*> watchdogs;    // supervisors / PMICs with a watchdog (TPS38xx, TLF35584, TPS65381…)
    std::vector<const Component*> memories;     // external EEPROM / flash (24LC, 25LC, AT24, W25, S25FL, M95…)
    std::vector<const Component*> crystals;     // crystals / oscillators
    std::vector<const Component*> lin;          // LIN transceivers
    std::vector<const Component*> ethernetPhys; // 100/1000BASE-T1 PHYs
    std::vector<const Component*> highSide;     // smart high-side switches (PROFET BTS / BTT, VN…)
    std::vector<const Component*> regulators;   // linear / switching regulators
    std::vector<const Component*> connectors;   // headers and connectors
    std::vector<const Component*> amplifiers;   // op-amps and instrumentation amplifiers
    std::vector<const Component*> safetyMcus;   // lockstep / ASIL-D MCUs (AURIX, Stellar, RH850, S32K3, TMS570…)
    std::vector<const Component*> efuses;       // electronic fuses / ideal-diode controllers / current-limited switches
    // Aerospace blocks.
    std::vector<const Component*> radHard;      // radiation-hardened / tolerant parts (ATmegaS, SAMRH, GR712, RTG4, UT…)
    std::vector<const Component*> mram;         // MRAM (Everspin MR25H / MR4A, CAES UT8MR…)
    std::vector<const Component*> voters;       // TMR majority voters (74HC10 / 74LVC1G57 / majority gates)
    std::vector<const Component*> adcs;         // ADCs (MCP320x, ADS…, AD7…, MAX318xx thermocouple converters)
    std::vector<const Component*> cjc;          // thermocouple converters with cold-junction compensation
    std::vector<const Component*> spacewire;    // LVDS / SpaceWire physical layer and codecs
    std::vector<const Component*> mil1553;      // MIL-STD-1553 transceivers / terminals
    std::vector<const Component*> arinc;        // ARINC 429 line drivers / receivers
    std::vector<const Component*> transformers; // coupling transformers
    std::vector<const Component*> isoDcdc;      // galvanically isolated DC-DC converters
};

inline Parts classify(const Schematic& sch) {
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
        if (c.kind == ComponentKind::Connector) p.connectors.push_back(&c);
        if (c.kind == ComponentKind::OpAmp) p.amplifiers.push_back(&c);
        if (c.kind == ComponentKind::Diode && containsAny(v, {"SM8S", "SM5S", "SMDJ"})) p.tvs.push_back(&c);
        if (c.kind != ComponentKind::Custom) continue;
        if (containsAny(n, {"TPS38", "TPS37", "TLF3558", "TPS6538", "TPS6539", "MAX6369", "MAX6746", "STWD", "WATCHDOG"}))
            p.watchdogs.push_back(&c);
        if (n.rfind("24LC", 0) == 0 || n.rfind("24AA", 0) == 0 || n.rfind("25LC", 0) == 0 || n.rfind("AT24", 0) == 0 ||
            n.rfind("W25", 0) == 0 || n.rfind("S25FL", 0) == 0 || n.rfind("M95", 0) == 0 || n.rfind("MX25", 0) == 0)
            p.memories.push_back(&c);
        if (containsAny(n, {"CRYSTAL", "XTAL", "OSC", "MHZ"})) p.crystals.push_back(&c);
        if (containsAny(n, {"TJA102", "TLIN", "MCP2003", "MCP2004", "ATA663"})) p.lin.push_back(&c);
        if (containsAny(n, {"TJA110", "TJA111", "DP83TC", "DP83TG", "88Q2", "BCM8988", "LAN887"})) p.ethernetPhys.push_back(&c);
        if (n.rfind("BTS", 0) == 0 || n.rfind("BTT", 0) == 0 || n.rfind("BTH", 0) == 0 || n.rfind("VN7", 0) == 0 ||
            n.rfind("VND", 0) == 0 || n.rfind("VNQ", 0) == 0 || n.rfind("TPS1H", 0) == 0 || n.rfind("TPS2H", 0) == 0 ||
            n.rfind("TPS4H", 0) == 0)
            p.highSide.push_back(&c);
        if (c.def().name.find("Regulator") != std::string::npos ||
            containsAny(n, {"LM317", "LM7805", "LM2940", "AP2112", "XC6206", "LM2596", "TPS54", "TPS55", "LM5", "LMR", "LT8",
                            "TPS7B", "TLE42", "TLS"}))
            p.regulators.push_back(&c);
        else if (const CustomPart* cp = CustomPartRegistry::instance().find(c.customPart); cp && cp->spec.model.hasRegulator)
            p.regulators.push_back(&c);
        if (n.rfind("HEADER", 0) == 0 || containsAny(n, {"CONN", "MOLEX", "TE_", "JST", "DEUTSCH", "MQS"})) p.connectors.push_back(&c);
        if (containsAny(n, {"INA", "AD62", "AD82", "LM358", "OPA", "MCP60", "TLV9", "LMV"})) p.amplifiers.push_back(&c);
        if (containsAny(n, {"TC2", "TC3", "TC4", "SPC5", "SR6", "RH850", "S32K3", "S32E", "TMS570", "RM4", "RM5", "MPC57",
                            "TRAVEO", "CYT"}))
            p.safetyMcus.push_back(&c);
        if (containsAny(n, {"TPS1213", "TPS1211", "TPS2HC", "LM7470", "LM7472", "LTC4359", "LM74", "EFUSE", "TPS255", "TPS2596",
                            "TPS1663", "TPS2660", "SSPC", "LCL"}))
            p.efuses.push_back(&c);
        if (n.rfind("ATMEGAS", 0) == 0 || n.rfind("UT", 0) == 0 || n.rfind("RH", 0) == 0 || n.rfind("RAD", 0) == 0 ||
            containsAny(n, {"SAMRH", "GR712", "GR740", "LEON", "RTG4", "RTAX", "XQR", "VA108", "VA416", "RT_", "-RT", "RADHARD"}))
            p.radHard.push_back(&c);
        if (containsAny(n, {"MR25H", "MR4A", "MR2A", "MR10Q", "MR0A", "UT8MR", "MRAM"})) p.mram.push_back(&c);
        if (containsAny(n, {"74HC10", "74LVC1G57", "74LVC1G58", "MAJORITY", "VOTER", "4530"})) p.voters.push_back(&c);
        if (containsAny(n, {"MCP320", "MCP330", "ADS1", "ADS8", "AD7", "MAX318", "MAX1112", "LTC24", "AMC13", "ADC"}))
            p.adcs.push_back(&c);
        if (containsAny(n, {"MAX31855", "MAX31856", "MAX6675", "AD849", "LT1025", "LTC2983"})) p.cjc.push_back(&c);
        if (containsAny(n, {"LVDS", "DS90C03", "DS90LV", "GR718", "SPACEWIRE", "SPW"})) p.spacewire.push_back(&c);
        if (containsAny(n, {"HI-15", "HI15", "BU-6", "BU6", "61580", "1553"})) p.mil1553.push_back(&c);
        if (containsAny(n, {"HI-8", "HI8", "DEI10", "ARINC", "429"})) p.arinc.push_back(&c);
        if (containsAny(n, {"XFMR", "TRANSFORMER", "PM-DB", "B-3818"})) p.transformers.push_back(&c);
        if (containsAny(n, {"ISO-DCDC", "NME", "MHF", "SVR28", "DCDC_ISO", "ISOLATED DC"}) ||
            ([&] {
                const CustomPart* cp = CustomPartRegistry::instance().find(c.customPart);
                return cp && cp->spec.model.hasRegulator && cp->spec.model.regulator.isolated();
            })())
            p.isoDcdc.push_back(&c);
        if (containsAny(n, {"IR2104", "IR2110", "IR2184", "L293", "ULN2003", "DRV8", "TMC2", "A4988", "L298", "UCC27",
                            "FAN73", "LM5113", "DRV83"}))
            p.drivers.push_back(&c);
        if (n.rfind("IRF", 0) == 0 || n.rfind("IPB", 0) == 0 || n.rfind("BSC", 0) == 0 || containsAny(n, {"MOSFET", "GAN", "EPC2"}))
            p.powerFets.push_back(&c);
        if (containsAny(n, {"ACS7", "INA2", "INA1", "INA3", "AMC1"})) p.shunts.push_back(&c);
        if (containsAny(n, {"MPU-6050", "MPU6050", "MPU-9250", "ICM-", "BMI0", "BMI1", "BMI2", "LSM6", "BNO0", "IMU"}))
            p.imus.push_back(&c);
        if (containsAny(n, {"NRF24", "ESP32", "ESP8266", "SX127", "SX126", "LORA", "CC1101", "RFM9", "WIFI", "BLE", "LTE", "UHF-TRX"}))
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

inline std::string netName(const Schematic& sch, int net) {
    const auto& nets = sch.nets();
    return net >= 0 && net < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(net)].name : "";
}

/// Nets whose (upper-case) name contains one of the needles.
inline std::vector<int> netsNamed(const Schematic& sch, std::initializer_list<const char*> needles) {
    std::vector<int> out;
    for (const auto& n : sch.nets())
        if (!n.name.empty() && containsAny(up(n.name), needles)) out.push_back(n.index);
    return out;
}

inline Rect courtyardOf(const PcbLayout& pcb, const Component& c) { return pcb.courtyard(c); }


/// How far a connector's body sits from the board edge: the nearest side of its courtyard (an edge-launch coax
/// connector has its courtyard against the edge, whatever its size).
inline double coaxEdgeGap(const PcbLayout& pcb, const Component& c) {
    const Rect r = pcb.courtyard(c);
    const Vec2 mids[4] = {{r.x0, (r.y0 + r.y1) / 2}, {r.x1, (r.y0 + r.y1) / 2}, {(r.x0 + r.x1) / 2, r.y0},
                          {(r.x0 + r.x1) / 2, r.y1}};
    double best = 1e9;
    for (const Vec2& m : mids) best = std::min(best, pcb.settings.edgeDistance(m));
    return std::max(0.0, best);
}

inline double rectGap(const Rect& a, const Rect& b) {
    double dx = std::max({0.0, b.x0 - a.x1, a.x0 - b.x1}), dy = std::max({0.0, b.y0 - a.y1, a.y0 - b.y1});
    return std::hypot(dx, dy);
}

/// Resistors between two nets (one pin on each).
inline std::vector<const Component*> bridges(const Schematic& sch, int a, int b, std::initializer_list<ComponentKind> kinds) {
    std::vector<const Component*> out;
    for (const auto& c : sch.components()) {
        if (std::find(kinds.begin(), kinds.end(), c.kind) == kinds.end() || c.def().pins.size() != 2) continue;
        int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        if ((n0 == a && n1 == b) || (n0 == b && n1 == a)) out.push_back(&c);
    }
    return out;
}


/// Reverse-battery protection on a supply input: a series diode from the battery +, a MOSFET ideal diode in the
/// return path, a reverse-protected regulator, or an ideal-diode / eFuse controller.
inline bool hasReverseProtection(const Schematic& sch, const Parts& parts) {
    const int gnd = sch.groundNet();
    for (const Component* src : parts.sources) {
        const int plus = sch.netOf({src->id, 0}), minus = sch.netOf({src->id, 1});
        for (const Component* d : parts.diodes)
            if (sch.netOf({d->id, 0}) == plus) return true;  // anode on the battery +
        for (const Component* q : parts.powerFets)
            for (size_t i = 0; i < q->def().pins.size(); ++i)
                if (pinName(*q, static_cast<int>(i)) == "S" && sch.netOf({q->id, static_cast<int>(i)}) == minus && minus != gnd)
                    return true;  // low-side ideal diode in the return
    }
    for (const auto& c : sch.components())
        if (containsAny(partName(c), {"LM2940", "LTC4359", "LM74", "IDEAL", "TPS1213", "TPS1211"})) return true;
    return false;
}

/// A 100–140 Ω resistor between the two lines of a bus pair.
inline bool hasTermination(const Schematic& sch, int a, int b) {
    for (const Component* r : bridges(sch, a, b, {ComponentKind::Resistor})) {
        auto v = parseEngineeringValue(primaryValue(r->value));
        if (v && *v >= 100 && *v <= 140) return true;
    }
    return false;
}

/// True if a TVS / ESD diode (any diode) has a pin on the net.
inline bool hasEsdDiode(const Schematic& sch, int net) {
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Diode) continue;
        for (int pin = 0; pin < static_cast<int>(c.def().pins.size()); ++pin)
            if (sch.netOf({c.id, pin}) == net) return true;
    }
    return false;
}

}  // namespace sieda::sysparts
