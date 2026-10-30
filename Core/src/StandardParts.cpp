#include "sieda/StandardParts.hpp"

#include <cctype>
#include <cmath>
#include <tuple>

namespace sieda {

namespace {
using T = PinType;

StandardPart part(const char* category, const char* name, const char* manufacturer, const char* description,
                  const char* package, int pinCount, const char* refPrefix,
                  std::vector<std::pair<const char*, T>> pins) {
    StandardPart p;
    p.category = category;
    p.spec.name = name;
    p.spec.manufacturer = manufacturer;
    p.spec.description = description;
    p.spec.refPrefix = refPrefix;
    p.spec.defaultValue = name;
    p.spec.datasheet = std::string("SiEDA standard library (") + manufacturer + " datasheet pinout)";
    p.spec.package.type = package;
    p.spec.package.pinCount = pinCount;
    int number = 1;
    for (const auto& [pinName, type] : pins) p.spec.pins.push_back({std::to_string(number++), pinName, type, ""});
    return p;
}

/// A microcontroller with its real package (pitch and body from the footprint); categories by vendor group.
void mcu(std::vector<StandardPart>& parts, const char* group, const char* name, const char* manufacturer,
         const char* description, const char* package, int pinCount, double pitch, double bodySize, const char* footprint,
         std::vector<std::pair<const char*, T>> pins) {
    std::string category = std::string("Microcontrollers · ") + group;
    StandardPart p = part(category.c_str(), name, manufacturer, description, package, pinCount, "U", std::move(pins));
    p.spec.package.pitch = pitch;
    p.spec.package.bodySize = bodySize;
    p.spec.datasheet = std::string("Pinout and ") + footprint + " footprint from the " + manufacturer + " datasheet";
    // An exposed pad listed after the last lead (QFN-48 + pad 49) is the thermal / ground pad.
    if (static_cast<int>(p.spec.pins.size()) == pinCount + 1) p.spec.pins.back().number = "EP";
    parts.push_back(std::move(p));
}

void regulator(StandardPart& p, const char* in, const char* out, const char* ref, double vout, double dropout, double iq,
               double ilimit, double maxPower, bool charger = false) {
    p.spec.model.hasRegulator = true;
    p.spec.model.regulator = {in, out, ref, vout, dropout, iq, ilimit, maxPower, charger, {}, 0.8};
}
/// Isolated DC-DC module: primary +VIN / −VIN, secondary +VOUT / −VOUT; dropout = minimum input − vout.
void isolated(StandardPart& p, const char* in, const char* inRet, const char* out, const char* ref, double vout,
              double dropout, double iq, double ilimit, double maxPower, double efficiency) {
    regulator(p, in, out, ref, vout, dropout, iq, ilimit, maxPower);
    p.spec.model.regulator.inReturn = inRet;
    p.spec.model.regulator.efficiency = efficiency;
}
void load(StandardPart& p, const char* supply, const char* ret, double amps) { p.spec.model.loads.push_back({supply, ret, amps}); }

/// A part from the production catalog (generated: tools/fetch_catalog_parts.py) with explicit pin numbers, which
/// for BGAs are ball names (A1, B7…).
void catalog(std::vector<StandardPart>& parts, const char* category, const char* name, const char* manufacturer,
             const char* description, const char* refPrefix, const char* package, int pinCount, double pitch,
             double bodySize, const char* footprint, std::vector<std::tuple<const char*, const char*, T>> pins) {
    StandardPart p = part(category, name, manufacturer, description, package, pinCount, refPrefix, {});
    for (const auto& [number, pinName, type] : pins) p.spec.pins.push_back({number, pinName, type, ""});
    p.spec.package.pitch = pitch;
    p.spec.package.bodySize = bodySize;
    p.spec.datasheet = std::string("Pinout and ") + footprint + " footprint from the " + manufacturer + " datasheet";
    p.footprint = footprint;
    parts.push_back(std::move(p));
}

/// Behavioural simulation models (datasheet typical values; pins by number).
void applyModels(std::vector<StandardPart>& parts) {
    for (auto& p : parts) {
        const std::string& n = p.spec.name;
        if (n == "LM7805") regulator(p, "1", "3", "2", 5.0, 2.0, 0.005, 1.5, 2.0);
        else if (n == "LM7812") regulator(p, "1", "3", "2", 12.0, 2.0, 0.005, 1.5, 2.0);
        else if (n == "AMS1117-3.3") regulator(p, "3", "2", "1", 3.3, 1.1, 0.005, 1.0, 1.0);
        else if (n == "LM2596S-5.0") isolated(p, "1", "3", "2", "3", 5.0, 2.0, 0.005, 3.0, 3.0, 0.85);  // buck
        else if (n == "LM317") regulator(p, "3", "2", "1", 1.25, 2.0, 50e-6, 1.5, 2.0);  // ADJ is the reference
        else if (n == "LM2940-5.0") regulator(p, "1", "3", "2", 5.0, 0.5, 0.010, 1.0, 2.0);
        else if (n == "TJA1044GT") load(p, "3", "2", 0.010);
        else if (n == "TJA1021") load(p, "7", "5", 0.001);
        else if (n == "TPS3823-33-Q1" || n == "TPS3823-50-Q1") load(p, "5", "2", 15e-6);
        else if (n == "MCP2518FD") load(p, "14", "7", 0.010);
        // Aerospace.
        else if (n == "ATmegaS128") load(p, "21", "22", 0.008);
        else if (n == "MR25H40") load(p, "8", "4", 0.003);
        else if (n == "74HC10") load(p, "14", "7", 0.0001);
        else if (n == "UT54LVDS031" || n == "UT54LVDS032") load(p, "16", "8", 0.010);
        else if (n == "MCP3201") load(p, "8", "4", 0.0004);
        else if (n == "MAX31855K") load(p, "4", "1", 0.0015);
        else if (n == "CC1101") load(p, "9", "16", 0.017);
        else if (n == "UHF-TRX-MODULE") load(p, "2", "1", 0.030);
        else if (n == "FO-RX-820") load(p, "3", "2", 0.006);
        else if (n == "LNA-MMIC") load(p, "3", "2", 0.025);
        else if (n == "ADuM4401") {
            load(p, "1", "2", 0.002);
            load(p, "16", "15", 0.002);
        } else if (n == "ISO-DCDC-MED") isolated(p, "1", "2", "8", "7", 5.0, -2.0, 0.004, 0.2, 0.5, 0.7);
        else if (n == "DW01A") load(p, "5", "6", 3e-6);
        else if (n == "SECURE-MCU") load(p, "2", "3", 0.025);
        else if (n == "LNK306") regulator(p, "4", "5", "", 15.0, 25.0, 0.0001, 0.36, 1.0);  // buck: S node to 15 V
        else if (n == "IPM-SLLIMM-600V") load(p, "1", "2", 0.005);
        else if (n == "AT42QT1070") load(p, "1", "14", 0.0006);
        else if (n == "A3144") load(p, "1", "2", 0.004);
        else if (n == "ESP32-WROOM-32E") load(p, "2", "1", 0.08);
        else if (n == "LM2596-5.0") isolated(p, "1", "3", "2", "3", 5.0, 2.0, 0.005, 3.0, 3.0, 0.85);  // buck: shared ground
        else if (n == "DRV8833") load(p, "12", "13", 0.0017);
        else if (n == "TPH-80MM") {
            load(p, "1", "3", 0.02);   // idle head bias; firing current is a pulse the bulk capacitor supplies
            load(p, "10", "12", 0.01);
        }
        else if (n == "USB2514B") load(p, "15", "EP", 0.08);
        else if (n == "NCN8025") load(p, "9", "10", 0.004);
        else if (n == "BLE-MODULE") load(p, "2", "1", 0.008);
        else if (n == "TPS2553") {  // current-limited switch: OUT follows IN (≈ 85 mΩ), trips at the ILIM setting
            regulator(p, "1", "6", "2", 6.5, 0.02, 120e-6, 0.5, 0.5);
            p.spec.model.regulator.loadSwitch = true;
        }
        else if (n == "ISO-DCDC-2805S") isolated(p, "1", "2", "4", "3", 5.0, 11.0, 0.002, 2.0, 3.0, 0.82);
        else if (n == "ISO-DCDC-0505S") isolated(p, "1", "2", "4", "3", 5.0, 0.0, 0.005, 0.2, 0.4, 0.75);
        else if (n == "24LC256") load(p, "8", "4", 0.001);
        else if (n == "TP4056") regulator(p, "4", "5", "3", 4.2, 0.1, 0.0003, 1.0, 2.0, true);  // 1 A with R_PROG = 1.2 kΩ
        else if (n == "XC6206P332") regulator(p, "3", "2", "1", 3.3, 0.25, 1e-6, 0.25, 0.25);
        else if (n == "AP2112K-3.3") regulator(p, "1", "5", "2", 3.3, 0.25, 55e-6, 0.6, 0.4);
        else if (n == "NE555") load(p, "8", "1", 0.003);
        else if (n == "LM358") load(p, "8", "4", 0.0007);
        else if (n == "ATtiny85") load(p, "8", "4", 0.002);
        else if (n == "ATmega328P") {
            load(p, "7", "8", 0.004);    // 8 MHz internal oscillator at 3.3 V
            load(p, "20", "22", 0.0003); // AVCC (ADC)
        } else if (n == "74HC595" || n == "74HC00") load(p, n == "74HC595" ? "16" : "14", n == "74HC595" ? "8" : "7", 0.0001);
        else if (n == "L293D") load(p, "16", "4", 0.016);
        else if (n == "UC3843") load(p, "7", "5", 0.011);
        else if (n == "IR2104") load(p, "1", "4", 0.001);
        else if (n == "ACS712") load(p, "8", "5", 0.010);
        else if (n == "TJA1050") load(p, "3", "2", 0.010);
        else if (n == "MAX485") load(p, "8", "5", 0.0003);
        else if (n == "MPU-6050") load(p, "13", "18", 0.0039);
        else if (n == "NRF24L01_Module") load(p, "2", "1", 0.0135);  // receive mode
        else if (n == "INA333") load(p, "7", "4", 50e-6);
        else if (n == "ADuM1201") {
            load(p, "1", "4", 0.0011);
            load(p, "8", "5", 0.0011);
        }
    }
}

std::vector<StandardPart> build() {
    std::vector<StandardPart> parts;
    parts.push_back(part("Timers", "NE555", "Texas Instruments", "Precision timer", "DIP", 8, "U",
                         {{"GND", T::PowerIn}, {"TRIG", T::Input}, {"OUT", T::Output}, {"RESET", T::Input},
                          {"CONT", T::Passive}, {"THRES", T::Input}, {"DISCH", T::OpenCollector}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Regulators", "LM7805", "Texas Instruments", "5 V 1.5 A positive linear regulator", "TO220", 3, "U",
                         {{"IN", T::PowerIn}, {"GND", T::PowerIn}, {"OUT", T::PowerOut}}));
    parts.push_back(part("Regulators", "LM317", "Texas Instruments", "1.25–37 V adjustable linear regulator", "TO220", 3, "U",
                         {{"ADJ", T::Passive}, {"OUT", T::PowerOut}, {"IN", T::PowerIn}}));
    parts.push_back(part("Op-Amps", "LM358", "Texas Instruments", "Dual low-power operational amplifier", "SOIC", 8, "U",
                         {{"OUT1", T::Output}, {"IN1-", T::Input}, {"IN1+", T::Input}, {"GND", T::PowerIn},
                          {"IN2+", T::Input}, {"IN2-", T::Input}, {"OUT2", T::Output}, {"VCC", T::PowerIn}}));
    // Quad op-amp drawn gate by gate: units A–D and the power unit P (pins 4 and 11).
    parts.push_back(part("Op-Amps", "LM324", "Texas Instruments", "Quad low-power operational amplifier (one symbol per gate)",
                         "SOIC", 14, "U",
                         {{"OUT1", T::Output}, {"IN1-", T::Input}, {"IN1+", T::Input}, {"V+", T::PowerIn},
                          {"IN2+", T::Input}, {"IN2-", T::Input}, {"OUT2", T::Output}, {"OUT3", T::Output},
                          {"IN3-", T::Input}, {"IN3+", T::Input}, {"V-", T::PowerIn}, {"IN4+", T::Input},
                          {"IN4-", T::Input}, {"OUT4", T::Output}}));
    parts.back().spec.units = {{"A", {"1", "2", "3"}}, {"B", {"7", "6", "5"}}, {"C", {"8", "9", "10"}}, {"D", {"14", "13", "12"}}};
    parts.push_back(part("Microcontrollers · Microchip", "ATtiny85", "Microchip", "8-bit AVR MCU, 8 KB flash", "DIP", 8, "U",
                         {{"PB5/RST", T::Bidirectional}, {"PB3", T::Bidirectional}, {"PB4", T::Bidirectional},
                          {"GND", T::PowerIn}, {"PB0", T::Bidirectional}, {"PB1", T::Bidirectional},
                          {"PB2", T::Bidirectional}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Microcontrollers · Microchip", "ATmega328P", "Microchip", "8-bit AVR MCU, 32 KB flash", "DIP", 28, "U",
                         {{"PC6/RST", T::Input}, {"PD0/RXD", T::Bidirectional}, {"PD1/TXD", T::Bidirectional},
                          {"PD2", T::Bidirectional}, {"PD3", T::Bidirectional}, {"PD4", T::Bidirectional},
                          {"VCC", T::PowerIn}, {"GND", T::PowerIn}, {"PB6/XT1", T::Bidirectional},
                          {"PB7/XT2", T::Bidirectional}, {"PD5", T::Bidirectional}, {"PD6", T::Bidirectional},
                          {"PD7", T::Bidirectional}, {"PB0", T::Bidirectional}, {"PB1", T::Bidirectional},
                          {"PB2", T::Bidirectional}, {"PB3/MOSI", T::Bidirectional}, {"PB4/MISO", T::Bidirectional},
                          {"PB5/SCK", T::Bidirectional}, {"AVCC", T::PowerIn}, {"AREF", T::Passive}, {"GND", T::PowerIn},
                          {"PC0", T::Bidirectional}, {"PC1", T::Bidirectional}, {"PC2", T::Bidirectional},
                          {"PC3", T::Bidirectional}, {"PC4/SDA", T::Bidirectional}, {"PC5/SCL", T::Bidirectional}}));
    parts.push_back(part("Logic", "74HC595", "Texas Instruments", "8-bit shift register with output latches", "DIP", 16, "U",
                         {{"QB", T::Output}, {"QC", T::Output}, {"QD", T::Output}, {"QE", T::Output},
                          {"QF", T::Output}, {"QG", T::Output}, {"QH", T::Output}, {"GND", T::PowerIn},
                          {"QH'", T::Output}, {"SRCLR", T::Input}, {"SRCLK", T::Input}, {"RCLK", T::Input},
                          {"OE", T::Input}, {"SER", T::Input}, {"QA", T::Output}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Logic", "74HC00", "Texas Instruments", "Quad 2-input NAND gate", "DIP", 14, "U",
                         {{"1A", T::Input}, {"1B", T::Input}, {"1Y", T::Output}, {"2A", T::Input}, {"2B", T::Input},
                          {"2Y", T::Output}, {"GND", T::PowerIn}, {"3Y", T::Output}, {"3A", T::Input}, {"3B", T::Input},
                          {"4Y", T::Output}, {"4A", T::Input}, {"4B", T::Input}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Drivers", "ULN2003A", "Texas Instruments", "7-channel Darlington sink driver", "DIP", 16, "U",
                         {{"IN1", T::Input}, {"IN2", T::Input}, {"IN3", T::Input}, {"IN4", T::Input}, {"IN5", T::Input},
                          {"IN6", T::Input}, {"IN7", T::Input}, {"GND", T::PowerIn}, {"COM", T::Passive},
                          {"OUT7", T::OpenCollector}, {"OUT6", T::OpenCollector}, {"OUT5", T::OpenCollector},
                          {"OUT4", T::OpenCollector}, {"OUT3", T::OpenCollector}, {"OUT2", T::OpenCollector},
                          {"OUT1", T::OpenCollector}}));
    parts.push_back(part("Drivers", "L293D", "Texas Instruments", "Quad half-H motor driver with clamp diodes", "DIP", 16, "U",
                         {{"EN1,2", T::Input}, {"1A", T::Input}, {"1Y", T::Output}, {"GND", T::PowerIn},
                          {"GND", T::PowerIn}, {"2Y", T::Output}, {"2A", T::Input}, {"VCC2", T::PowerIn},
                          {"EN3,4", T::Input}, {"3A", T::Input}, {"3Y", T::Output}, {"GND", T::PowerIn},
                          {"GND", T::PowerIn}, {"4Y", T::Output}, {"4A", T::Input}, {"VCC1", T::PowerIn}}));
    parts.push_back(part("Connectors", "Header_1x04", "Generic", "2.54 mm pin header, 1×4", "HEADER", 4, "J",
                         {{"1", T::Passive}, {"2", T::Passive}, {"3", T::Passive}, {"4", T::Passive}}));
    parts.push_back(part("Connectors", "Header_1x06", "Generic", "2.54 mm pin header, 1×6", "HEADER", 6, "J",
                         {{"1", T::Passive}, {"2", T::Passive}, {"3", T::Passive}, {"4", T::Passive},
                          {"5", T::Passive}, {"6", T::Passive}}));

    // Industry kits: motor control / power electronics.
    parts.push_back(part("Power Electronics", "IR2104", "Infineon", "Half-bridge gate driver (600 V, bootstrap high side)",
                         "DIP", 8, "U",
                         {{"VCC", T::PowerIn}, {"IN", T::Input}, {"SD", T::Input}, {"COM", T::PowerIn},
                          {"LO", T::Output}, {"VS", T::Passive}, {"HO", T::Output}, {"VB", T::Passive}}));
    parts.push_back(part("Power Electronics", "IRF540N", "Infineon", "N-channel power MOSFET, 100 V 33 A", "TO220", 3, "Q",
                         {{"G", T::Input}, {"D", T::Passive}, {"S", T::Passive}}));
    parts.push_back(part("Power Electronics", "UC3843", "Texas Instruments", "Current-mode PWM controller", "DIP", 8, "U",
                         {{"COMP", T::Output}, {"VFB", T::Input}, {"ISENSE", T::Input}, {"RT/CT", T::Passive},
                          {"GND", T::PowerIn}, {"OUTPUT", T::Output}, {"VCC", T::PowerIn}, {"VREF", T::PowerOut}}));
    parts.push_back(part("Motor Control", "ACS712", "Allegro", "Hall-effect current sensor, ±5/20/30 A", "SOIC", 8, "U",
                         {{"IP+", T::Passive}, {"IP+", T::Passive}, {"IP-", T::Passive}, {"IP-", T::Passive},
                          {"GND", T::PowerIn}, {"FILTER", T::Passive}, {"VIOUT", T::Output}, {"VCC", T::PowerIn}}));
    // Automotive.
    parts.push_back(part("Automotive", "TJA1050", "NXP", "High-speed CAN transceiver (ISO 11898)", "SOIC", 8, "U",
                         {{"TXD", T::Input}, {"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"RXD", T::Output},
                          {"VREF", T::NoConnect}, {"CANL", T::Bidirectional}, {"CANH", T::Bidirectional}, {"S", T::Input}}));
    parts.push_back(part("Automotive", "LM2940-5.0", "Texas Instruments",
                         "5 V low-dropout regulator with load-dump and reverse-battery protection", "TO220", 3, "U",
                         {{"IN", T::PowerIn}, {"GND", T::PowerIn}, {"OUT", T::PowerOut}}));
    // Automotive ECU building blocks (AEC-Q100 grades): CAN-FD, LIN, watchdog supervisor, EEPROM, crystal.
    parts.push_back(part("Automotive", "TJA1044GT", "NXP",
                         "CAN-FD transceiver (5 Mbit/s, ISO 11898-2:2016), VIO for 3.3 V MCUs, standby", "SOIC", 8, "U",
                         {{"TXD", T::Input}, {"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"RXD", T::Output},
                          {"VIO", T::PowerIn}, {"CANL", T::Bidirectional}, {"CANH", T::Bidirectional}, {"STB", T::Input}}));
    parts.push_back(part("Automotive", "TJA1021", "NXP", "LIN 2.x / SAE J2602 transceiver (20 kbit/s), sleep and wake",
                         "SOIC", 8, "U",
                         {{"RXD", T::Output}, {"SLP_N", T::Input}, {"WAKE_N", T::Input}, {"TXD", T::Input},
                          {"GND", T::PowerIn}, {"LIN", T::Bidirectional}, {"BAT", T::PowerIn}, {"INH", T::Output}}));
    parts.push_back(part("Automotive", "TPS3823-33-Q1", "Texas Instruments",
                         "Supervisor with watchdog (1.6 s) and 2.93 V reset threshold, AEC-Q100", "SOT23", 5, "U",
                         {{"RESET", T::Output}, {"GND", T::PowerIn}, {"MR", T::Input}, {"WDI", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Automotive", "TPS3823-50-Q1", "Texas Instruments",
                         "Supervisor with watchdog (1.6 s) and 4.55 V reset threshold for 5 V rails, AEC-Q100", "SOT23", 5, "U",
                         {{"RESET", T::Output}, {"GND", T::PowerIn}, {"MR", T::Input}, {"WDI", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Automotive", "MCP2518FD", "Microchip",
                         "SPI CAN-FD controller (8 Mbit/s data phase) for MCUs without a CAN peripheral, AEC-Q100", "SOIC", 14,
                         "U",
                         {{"TXCAN", T::Output}, {"RXCAN", T::Input}, {"CLKO", T::Output}, {"INT", T::OpenCollector},
                          {"OSC2", T::Passive}, {"OSC1", T::Passive}, {"VSS", T::PowerIn}, {"INT1", T::OpenCollector},
                          {"INT0", T::OpenCollector}, {"SCK", T::Input}, {"SDI", T::Input}, {"SDO", T::Output},
                          {"CS", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Automotive", "24LC256", "Microchip", "256 Kbit I²C EEPROM (1 M cycles, 200-year retention), grade 1",
                         "SOIC", 8, "U",
                         {{"A0", T::Input}, {"A1", T::Input}, {"A2", T::Input}, {"VSS", T::PowerIn}, {"SDA", T::Bidirectional},
                          {"SCL", T::Input}, {"WP", T::Input}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Timing", "Crystal_8MHz", "Generic (AEC-Q200)", "8 MHz crystal, HC-49/US, ±30 ppm, 18 pF load",
                         "HC49", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_20MHz", "Generic (AEC-Q200)", "20 MHz crystal, HC-49/US, ±30 ppm, 18 pF load",
                         "HC49", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    // Aerospace: rad-tolerant compute, MRAM, latch-up current limiting, TMR voting, SpaceWire LVDS, sensor
    // conversion, isolated power and UHF telemetry.
    mcu(parts, "Microchip", "ATmegaS128", "Microchip",
        "Radiation-tolerant 8-bit AVR (ATmega128 pin-compatible): SEL-free to 62 MeV·cm²/mg, 30 krad TID, 3.3 V",
        "LQFP", 64, 0.8, 14.0, "TQFP-64",
        {{"PEN", T::Input}, {"PE0/RXD0", T::Bidirectional}, {"PE1/TXD0", T::Bidirectional}, {"PE2", T::Bidirectional},
         {"PE3", T::Bidirectional}, {"PE4", T::Bidirectional}, {"PE5", T::Bidirectional}, {"PE6", T::Bidirectional},
         {"PE7", T::Bidirectional}, {"PB0", T::Bidirectional}, {"PB1/SCK", T::Bidirectional},
         {"PB2/MOSI", T::Bidirectional}, {"PB3/MISO", T::Bidirectional}, {"PB4", T::Bidirectional},
         {"PB5", T::Bidirectional}, {"PB6", T::Bidirectional}, {"PB7", T::Bidirectional}, {"PG3", T::Bidirectional},
         {"PG4", T::Bidirectional}, {"RESET", T::Input}, {"VCC", T::PowerIn}, {"GND", T::PowerIn},
         {"XTAL2", T::Passive}, {"XTAL1", T::Passive}, {"PD0/SCL", T::Bidirectional}, {"PD1/SDA", T::Bidirectional},
         {"PD2/RXD1", T::Bidirectional}, {"PD3/TXD1", T::Bidirectional}, {"PD4", T::Bidirectional},
         {"PD5", T::Bidirectional}, {"PD6", T::Bidirectional}, {"PD7", T::Bidirectional}, {"PG0", T::Bidirectional},
         {"PG1", T::Bidirectional}, {"PC0", T::Bidirectional}, {"PC1", T::Bidirectional}, {"PC2", T::Bidirectional},
         {"PC3", T::Bidirectional}, {"PC4", T::Bidirectional}, {"PC5", T::Bidirectional}, {"PC6", T::Bidirectional},
         {"PC7", T::Bidirectional}, {"PG2", T::Bidirectional}, {"PA7", T::Bidirectional}, {"PA6", T::Bidirectional},
         {"PA5", T::Bidirectional}, {"PA4", T::Bidirectional}, {"PA3", T::Bidirectional}, {"PA2", T::Bidirectional},
         {"PA1", T::Bidirectional}, {"PA0", T::Bidirectional}, {"VCC", T::PowerIn}, {"GND", T::PowerIn},
         {"PF7", T::Bidirectional}, {"PF6", T::Bidirectional}, {"PF5", T::Bidirectional}, {"PF4", T::Bidirectional},
         {"PF3", T::Bidirectional}, {"PF2", T::Bidirectional}, {"PF1", T::Bidirectional}, {"PF0/ADC0", T::Bidirectional},
         {"AREF", T::Passive}, {"GND", T::PowerIn}, {"AVCC", T::PowerIn}});
    parts.push_back(part("Aerospace", "MR25H40", "Everspin", "4 Mbit SPI MRAM: radiation-immune storage, unlimited endurance",
                         "SOIC", 8, "U",
                         {{"CS", T::Input}, {"SO", T::Output}, {"WP", T::Input}, {"VSS", T::PowerIn}, {"SI", T::Input},
                          {"SCK", T::Input}, {"HOLD", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Aerospace", "TPS2553", "Texas Instruments",
                         "Current-limited power switch (75–1700 mA, 2 µs trip): single-event latch-up protection per processor",
                         "SOT23", 6, "U",
                         {{"IN", T::PowerIn}, {"GND", T::PowerIn}, {"EN", T::Input}, {"FAULT", T::OpenCollector},
                          {"ILIM", T::Passive}, {"OUT", T::PowerOut}}));
    parts.push_back(part("Logic", "74HC10", "Texas Instruments", "Triple 3-input NAND gate (TMR majority voter output stage)",
                         "DIP", 14, "U",
                         {{"1A", T::Input}, {"1B", T::Input}, {"2A", T::Input}, {"2B", T::Input}, {"2C", T::Input},
                          {"2Y", T::Output}, {"GND", T::PowerIn}, {"3Y", T::Output}, {"3A", T::Input}, {"3B", T::Input},
                          {"3C", T::Input}, {"1Y", T::Output}, {"1C", T::Input}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Aerospace", "UT54LVDS031", "CAES (Aeroflex)",
                         "Rad-hard quad LVDS driver (400 Mbit/s, 300 krad): SpaceWire data / strobe transmit", "SOIC", 16, "U",
                         {{"DIN1", T::Input}, {"DOUT1+", T::Output}, {"DOUT1-", T::Output}, {"EN", T::Input},
                          {"DOUT2-", T::Output}, {"DOUT2+", T::Output}, {"DIN2", T::Input}, {"GND", T::PowerIn},
                          {"DIN3", T::Input}, {"DOUT3+", T::Output}, {"DOUT3-", T::Output}, {"EN_N", T::Input},
                          {"DOUT4-", T::Output}, {"DOUT4+", T::Output}, {"DIN4", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Aerospace", "UT54LVDS032", "CAES (Aeroflex)",
                         "Rad-hard quad LVDS receiver (400 Mbit/s, 300 krad): SpaceWire data / strobe receive", "SOIC", 16, "U",
                         {{"RIN1-", T::Input}, {"RIN1+", T::Input}, {"ROUT1", T::Output}, {"EN", T::Input},
                          {"ROUT2", T::Output}, {"RIN2+", T::Input}, {"RIN2-", T::Input}, {"GND", T::PowerIn},
                          {"RIN3-", T::Input}, {"RIN3+", T::Input}, {"ROUT3", T::Output}, {"EN_N", T::Input},
                          {"ROUT4", T::Output}, {"RIN4+", T::Input}, {"RIN4-", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Aerospace", "MCP3201", "Microchip", "12-bit differential-input SPI ADC (100 ksps)", "SOIC", 8, "U",
                         {{"VREF", T::PowerIn}, {"IN+", T::Input}, {"IN-", T::Input}, {"VSS", T::PowerIn},
                          {"CS", T::Input}, {"DOUT", T::Output}, {"CLK", T::Input}, {"VDD", T::PowerIn}}));
    parts.push_back(part("Aerospace", "MAX31855K", "Analog Devices",
                         "K-type thermocouple-to-digital converter with cold-junction compensation (−270 … +1372 °C)", "SOIC",
                         8, "U",
                         {{"GND", T::PowerIn}, {"T-", T::Input}, {"T+", T::Input}, {"VCC", T::PowerIn}, {"SCK", T::Input},
                          {"CS", T::Input}, {"SO", T::Output}, {"DNC", T::NoConnect}}));
    parts.push_back(part("Aerospace", "ISO-DCDC-2805S", "Generic (MHF+2805S / SVR2805S class)",
                         "Isolated DC-DC function block: 28 V bus (16–50 V) to 5 V, 10 W, 1500 V isolation. Pins by function — "
                         "use the chosen module's footprint",
                         "HEADER", 4, "PS", {{"+VIN", T::PowerIn}, {"-VIN", T::PowerIn}, {"-VOUT", T::PowerOut}, {"+VOUT", T::PowerOut}}));
    parts.push_back(part("Aerospace", "ISO-DCDC-0505S", "Generic (NME0505 class)",
                         "Isolated DC-DC function block: 5 V to isolated 5 V, 1 W, 1000 V isolation for a sensor domain. Pins "
                         "by function — use the chosen module's footprint",
                         "HEADER", 4, "PS", {{"+VIN", T::PowerIn}, {"-VIN", T::PowerIn}, {"-VOUT", T::PowerOut}, {"+VOUT", T::PowerOut}}));
    {
        StandardPart rf = part("RF", "CC1101", "Texas Instruments",
                               "Sub-1 GHz transceiver (300–928 MHz, +12 dBm): UHF telemetry / beacon", "QFN", 20, "U",
                               {{"SCLK", T::Input}, {"SO", T::Output}, {"GDO2", T::Output}, {"DVDD", T::PowerIn},
                                {"DCOUPL", T::Passive}, {"GDO0", T::Output}, {"CSN", T::Input}, {"XOSC_Q1", T::Passive},
                                {"AVDD", T::PowerIn}, {"XOSC_Q2", T::Passive}, {"AVDD", T::PowerIn}, {"RF_P", T::Passive},
                                {"RF_N", T::Passive}, {"AVDD", T::PowerIn}, {"AVDD", T::PowerIn}, {"GND", T::PowerIn},
                                {"RBIAS", T::Passive}, {"DGUARD", T::PowerIn}, {"GND", T::PowerIn}, {"SI", T::Input},
                                {"GND", T::PowerIn}});
        rf.spec.package.pitch = 0.5;
        rf.spec.package.bodySize = 4.0;
        rf.spec.pins.back().number = "EP";
        parts.push_back(std::move(rf));
    }
    parts.push_back(part("RF", "UHF-TRX-MODULE", "Generic (CC1101 / Si4463 module class)",
                         "UHF telemetry transceiver module function block (430–440 MHz, +20 dBm, 50 Ω RF pin, SPI). Pins by "
                         "function — use the chosen module's footprint",
                         "HEADER", 10, "U",
                         {{"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"SCK", T::Input}, {"MOSI", T::Input},
                          {"MISO", T::Output}, {"CS", T::Input}, {"IRQ", T::Output}, {"GND", T::PowerIn},
                          {"RF", T::Passive}, {"GND", T::PowerIn}}));
    // Naval: surge front end, fibre links and a radar LNA.
    parts.push_back(part("Naval & Marine", "S14K35", "TDK (EPCOS)",
                         "Metal-oxide varistor, 35 V rms / 45 V DC, 14 mm disc, 6 kA (8/20 µs): ship's power surge clamp",
                         "DISC", 2, "RV", {{"1", T::Passive}, {"2", T::Passive}}));
    parts.push_back(part("Naval & Marine", "GDT-90V", "Generic (Bourns 2038 / Littelfuse CG2 class)",
                         "Two-electrode gas discharge tube, 90 V DC spark-over, 10 kA: high-energy surge diverter at the power "
                         "entry",
                         "DISC", 2, "GDT", {{"1", T::Passive}, {"2", T::Passive}}));
    parts.push_back(part("Naval & Marine", "FO-TX-820", "Generic (versatile-link / ST fibre class)",
                         "Fibre-optic transmitter function block, 820 nm LED, up to 5 MBd: drive the LED through a resistor. "
                         "Pins by function — use the chosen module's footprint",
                         "HEADER", 4, "U", {{"ANODE", T::Passive}, {"CATHODE", T::Passive}, {"NC", T::NoConnect}, {"NC", T::NoConnect}}));
    parts.push_back(part("Naval & Marine", "FO-RX-820", "Generic (versatile-link / ST fibre class)",
                         "Fibre-optic receiver function block, 820 nm, up to 5 MBd, open-collector output with internal pull-up "
                         "(RL). Pins by function — use the chosen module's footprint",
                         "HEADER", 4, "U", {{"VO", T::OpenCollector}, {"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"RL", T::Passive}}));
    parts.push_back(part("RF", "LNA-MMIC", "Generic (InGaP / pHEMT MMIC class)",
                         "Low-noise amplifier MMIC function block (0.05–6 GHz, NF ≈ 1 dB, 20 dB gain), biased through a choke "
                         "on RFOUT. Pins by function — check the chosen MMIC's pinout",
                         "SOT23", 3, "U", {{"RFIN", T::Passive}, {"GND", T::PowerIn}, {"RFOUT", T::Passive}}));
    // Medical: 2 × MOPP patient barrier, battery protection and BLE telemetry.
    {
        StandardPart iso = part("Medical", "ADuM4401", "Analog Devices",
                                "Quad digital isolator (3 forward, 1 reverse), 5 kV rms reinforced, wide-body SOIC-16 with 8 mm "
                                "creepage: 2 × MOPP patient barrier",
                                "SOIC", 16, "U",
                                {{"VDD1", T::PowerIn}, {"GND1", T::PowerIn}, {"VIA", T::Input}, {"VIB", T::Input},
                                 {"VIC", T::Input}, {"VOD", T::Output}, {"VE1", T::Input}, {"GND1", T::PowerIn},
                                 {"GND2", T::PowerIn}, {"VE2", T::Input}, {"VID", T::Input}, {"VOC", T::Output},
                                 {"VOB", T::Output}, {"VOA", T::Output}, {"GND2", T::PowerIn}, {"VDD2", T::PowerIn}});
        iso.spec.package.bodySize = 7.5;  // wide body
        parts.push_back(std::move(iso));
    }
    {
        StandardPart dc = part("Medical", "ISO-DCDC-MED", "Generic (RECOM REM / Murata MEV medical class)",
                               "Medical isolated DC-DC function block: 3.0–5.5 V in, 5 V out, 1 W, 5 kV AC reinforced "
                               "(2 × MOPP), < 10 pF barrier capacitance (patient leakage < 2 µA). Input and output on "
                               "opposite rows 15 mm apart; pins by function — use the chosen module's footprint",
                               "DIP", 8, "PS",
                               {{"+VIN", T::PowerIn}, {"-VIN", T::PowerIn}, {"NC", T::NoConnect}, {"NC", T::NoConnect},
                                {"NC", T::NoConnect}, {"NC", T::NoConnect}, {"-VOUT", T::PowerOut}, {"+VOUT", T::PowerOut}});
        dc.spec.package.bodySize = 15.24;  // wide row spacing across the barrier
        parts.push_back(std::move(dc));
    }
    {
        // Payment secure element (MAX32550 / MAX32560 class): mesh drive / sense pins at the four corners so each
        // tamper-mesh stub leaves straight to its mesh end; tamper switch inputs and battery-backed key storage.
        StandardPart se = part("Retail", "SECURE-MCU", "Generic (PCI PTS secure MCU class)",
                               "Payment secure MCU: battery-backed AES key storage zeroized on tamper, two active mesh "
                               "drive / sense pairs, tamper-switch inputs, smart-card (EMV) and magstripe interfaces",
                               "LQFP", 32, "U",
                               {{"MESH_A_DRV", T::Output}, {"VDD", T::PowerIn}, {"GND", T::PowerIn}, {"TAMPER1", T::Input},
                                {"TAMPER2", T::Input}, {"VBAT", T::PowerIn}, {"MSR_DATA", T::Input}, {"MSR_CLK", T::Input},
                                {"SC_IO", T::Bidirectional}, {"SC_CLK", T::Output}, {"SC_RST", T::Output}, {"SC_VCC", T::Output},
                                {"GND", T::PowerIn}, {"VDD", T::PowerIn}, {"NRST", T::Input}, {"MESH_B_SNS", T::Input},
                                {"MESH_A_SNS", T::Input}, {"SPI_SCK", T::Output}, {"SPI_MOSI", T::Output}, {"SPI_MISO", T::Input},
                                {"SPI_CS", T::Output}, {"UART_TX", T::Output}, {"UART_RX", T::Input}, {"VDD", T::PowerIn},
                                {"GND", T::PowerIn}, {"USB_DP", T::Bidirectional}, {"USB_DM", T::Bidirectional},
                                {"KEY_ROW", T::Output}, {"KEY_COL", T::Input}, {"SWDIO", T::Bidirectional}, {"SWCLK", T::Input},
                                {"MESH_B_DRV", T::Output}});
        se.spec.package.pitch = 0.8;
        se.spec.package.bodySize = 7.0;
        parts.push_back(std::move(se));
    }
    parts.push_back(part("Power", "LM2596-5.0", "Texas Instruments",
                         "3 A step-down (buck) converter, 5 V fixed, 150 kHz, 4.5–40 V input; needs a 33 µH inductor, a "
                         "Schottky catch diode and low-ESR output capacitance",
                         "TO220", 5, "U",
                         {{"VIN", T::PowerIn}, {"OUTPUT", T::PowerOut}, {"GND", T::PowerIn}, {"FEEDBACK", T::Input},
                          {"ON_OFF", T::Input}}));
    {
        StandardPart drv = part("Robotics & Motor Control", "DRV8833", "Texas Instruments",
                                "Dual H-bridge motor / stepper driver, 2.7–10.8 V, 1.5 A RMS per bridge, current "
                                "regulation, HTSSOP-16 PowerPAD (the pad sinks the heat into a ground pour)",
                                "TSSOP", 16, "U",
                                {{"nSLEEP", T::Input}, {"AOUT1", T::Output}, {"AISEN", T::Passive}, {"AOUT2", T::Output},
                                 {"BOUT2", T::Output}, {"BISEN", T::Passive}, {"BOUT1", T::Output}, {"nFAULT", T::OpenCollector},
                                 {"BIN1", T::Input}, {"BIN2", T::Input}, {"VCP", T::Passive}, {"VM", T::PowerIn},
                                 {"GND", T::PowerIn}, {"VINT", T::Passive}, {"AIN2", T::Input}, {"AIN1", T::Input},
                                 {"PPAD", T::PowerIn}});
        drv.spec.pins.back().number = "EP";
        parts.push_back(std::move(drv));
    }
    parts.push_back(part("Retail", "TPH-80MM", "Generic (80 mm, 576-dot receipt mechanism class)",
                         "80 mm thermal print head connector (576 dots): head supply VH 24 V (amps while a dot line fires), "
                         "serial dot data, latch, two strobe groups and the head thermistor",
                         "HEADER", 12, "J",
                         {{"VH", T::PowerIn}, {"VH", T::PowerIn}, {"GND", T::PowerIn}, {"GND", T::PowerIn}, {"DI", T::Input},
                          {"CLK", T::Input}, {"LAT", T::Input}, {"STB1", T::Input}, {"STB2", T::Input}, {"VDD", T::PowerIn},
                          {"TH", T::Passive}, {"GND", T::PowerIn}}));
    {
        StandardPart hub = part("Retail", "USB2514B", "Microchip",
                                "USB 2.0 high-speed 4-port hub controller, per-port power switching and over-current "
                                "sense, 24 MHz crystal, QFN-36 6 × 6 mm",
                                "QFN", 36, "U",
                                {{"USBDM_DN1", T::Bidirectional}, {"USBDP_DN1", T::Bidirectional}, {"USBDM_DN2", T::Bidirectional},
                                 {"USBDP_DN2", T::Bidirectional}, {"VDDA33", T::PowerIn}, {"USBDM_DN3", T::Bidirectional},
                                 {"USBDP_DN3", T::Bidirectional}, {"USBDM_DN4", T::Bidirectional}, {"USBDP_DN4", T::Bidirectional},
                                 {"VDDA33", T::PowerIn}, {"TEST", T::Input}, {"PRTPWR1", T::Output}, {"OCS_N1", T::Input},
                                 {"CRFILT", T::Passive}, {"VDD33", T::PowerIn}, {"PRTPWR2", T::Output}, {"OCS_N2", T::Input},
                                 {"PRTPWR3", T::Output}, {"OCS_N3", T::Input}, {"PRTPWR4", T::Output}, {"OCS_N4", T::Input},
                                 {"SDA", T::Bidirectional}, {"VDD33", T::PowerIn}, {"SCL", T::Input}, {"HS_IND", T::Output},
                                 {"RESET_N", T::Input}, {"VBUS_DET", T::Input}, {"SUSP_IND", T::Output}, {"VDDA33", T::PowerIn},
                                 {"USBDM_UP", T::Bidirectional}, {"USBDP_UP", T::Bidirectional}, {"XTALOUT", T::Output},
                                 {"XTALIN", T::Input}, {"PLLFILT", T::Passive}, {"RBIAS", T::Passive}, {"VDD33", T::PowerIn},
                                 {"GND", T::PowerIn}});
        hub.spec.pins.back().number = "EP";
        hub.spec.package.pitch = 0.5;
        parts.push_back(std::move(hub));
    }
    {
        StandardPart afe = part("Retail", "NCN8025", "onsemi",
                                "EMV / ISO 7816 smart-card interface: card VCC (1.8 / 3 / 5 V) from its own DC-DC, "
                                "level-shifted I/O, card-presence detect, 8 kV ESD on the card pins. Pins by function",
                                "QFN", 24, "U",
                                {{"CLKIN", T::Input}, {"RSTIN", T::Input}, {"CMDVCC", T::Input}, {"VSEL", T::Input},
                                 {"INT", T::OpenCollector}, {"IOUC", T::Bidirectional}, {"AUX1UC", T::Bidirectional},
                                 {"AUX2UC", T::Bidirectional}, {"VDD", T::PowerIn}, {"GND", T::PowerIn}, {"VDDP", T::PowerIn},
                                 {"LI", T::Passive}, {"PGND", T::PowerIn}, {"CVCC", T::PowerOut}, {"CRST", T::Output},
                                 {"CCLK", T::Output}, {"CIO", T::Bidirectional}, {"CAUX1", T::Bidirectional},
                                 {"CAUX2", T::Bidirectional}, {"PRES", T::Input}, {"PRESN", T::Input}, {"GND", T::PowerIn},
                                 {"NC", T::NoConnect}, {"NC", T::NoConnect}, {"GND", T::PowerIn}});
        afe.spec.pins.back().number = "EP";
        afe.spec.package.pitch = 0.5;
        parts.push_back(std::move(afe));
    }
    parts.push_back(part("Protection", "USBLC6-2SC6", "STMicroelectronics",
                         "Two-line USB ESD protection with VBUS clamp, ±15 kV air (IEC 61000-4-2 level 4), 3.5 pF",
                         "SOT23", 6, "D",
                         {{"IO1", T::Passive}, {"GND", T::PowerIn}, {"IO2", T::Passive}, {"IO2", T::Passive},
                          {"VBUS", T::Passive}, {"IO1", T::Passive}}));
    parts.push_back(part("Protection", "TPD4E1U06", "Texas Instruments",
                         "Four-channel low-capacitance ESD array (0.8 pF, ±15 kV contact) for LVDS / HDMI / smart-card lines",
                         "SOT23", 6, "D",
                         {{"IO1", T::Passive}, {"GND", T::PowerIn}, {"IO2", T::Passive}, {"IO3", T::Passive},
                          {"NC", T::NoConnect}, {"IO4", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_24MHz", "Generic", "24 MHz crystal (USB hub / PHY reference), HC-49/US, ±30 ppm, 18 pF load",
                         "HC49", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    // Home appliances: mains entry, off-line buck, AC switching, inverter motor, touch HMI, Wi-Fi.
    parts.push_back(part("Home Appliances", "S10K275", "TDK / EPCOS",
                         "275 VAC metal-oxide varistor, 10 mm disc, 2.5 kA surge: line-to-neutral clamp after the fuse",
                         "DISC", 2, "RV", {{"1", T::Passive}, {"2", T::Passive}}));
    {
        StandardPart x2 = part("Home Appliances", "X2-100N-275VAC", "Generic (MKP X2 film)",
                               "100 nF / 275 VAC class-X2 film capacitor across line and neutral (EMI filter, self-healing), "
                               "15 mm lead pitch",
                               "DISC", 2, "CX", {{"1", T::Passive}, {"2", T::Passive}});
        x2.spec.package.bodySize = 15.0;
        parts.push_back(std::move(x2));
    }
    parts.push_back(part("Home Appliances", "LNK306", "Power Integrations",
                         "LinkSwitch-TN off-line switcher for a non-isolated buck from the rectified mains (85–265 VAC), "
                         "up to 360 mA; source (S) is the switching node, the output is referenced to the DC-bus return. "
                         "Pins by function",
                         "DIP", 8, "U",
                         {{"BP", T::Passive}, {"FB", T::Input}, {"NC", T::NoConnect}, {"D", T::PowerIn}, {"S", T::Passive},
                          {"S", T::Passive}, {"S", T::Passive}, {"S", T::Passive}}));
    {
        StandardPart triac = part("Home Appliances", "BT136-600E", "WeEn",
                                  "4 A / 600 V sensitive-gate triac (TO-220, leads formed to 5.08 mm for mains creepage) for "
                                  "heaters, valves and pump motors; needs an RC snubber across MT1–MT2",
                                  "TO220", 3, "Q", {{"MT1", T::Passive}, {"MT2", T::Passive}, {"G", T::Input}});
        triac.spec.package.pitch = 5.08;
        parts.push_back(std::move(triac));
    }
    parts.push_back(part("Home Appliances", "MOC3021", "onsemi",
                         "Random-phase opto-triac driver, 400 V, 5.3 kV isolation: fires a power triac from a logic pin",
                         "DIP", 6, "U",
                         {{"A", T::Input}, {"K", T::Input}, {"NC", T::NoConnect}, {"MT1", T::Passive}, {"NC", T::NoConnect},
                          {"MT2", T::Passive}}));
    parts.push_back(part("Home Appliances", "H11AA1", "onsemi",
                         "AC-input optocoupler (anti-parallel LEDs): a pulse at every mains zero crossing for phase control "
                         "and triac timing",
                         "DIP", 6, "U",
                         {{"AC1", T::Passive}, {"AC2", T::Passive}, {"NC", T::NoConnect}, {"E", T::Passive},
                          {"C", T::OpenCollector}, {"B", T::Passive}}));
    parts.push_back(part("Home Appliances", "IPM-SLLIMM-600V", "Generic (STGIPQ / SLLIMM nano class)",
                         "600 V 3 A three-phase intelligent power module: six IGBTs, gate drivers, bootstrap diodes, "
                         "open emitters for shunt sensing and a fault output, for inverter compressors, drum and fan "
                         "motors; logic pins on one row, power pins on the other with empty positions between high-voltage pins. "
                         "Pins by function",
                         "DIP", 28, "U",
                         {{"VCC", T::PowerIn}, {"GND", T::PowerIn}, {"HIN_U", T::Input}, {"HIN_V", T::Input}, {"HIN_W", T::Input},
                          {"LIN_U", T::Input}, {"LIN_V", T::Input}, {"LIN_W", T::Input}, {"FAULT", T::OpenCollector},
                          {"CIN", T::Input}, {"NC", T::NoConnect}, {"NC", T::NoConnect}, {"NC", T::NoConnect},
                          {"NC", T::NoConnect}, {"NW", T::Passive}, {"NV", T::Passive}, {"NU", T::Passive},
                          {"NC", T::NoConnect}, {"P", T::PowerIn}, {"NC", T::NoConnect}, {"W", T::Output},
                          {"VBOOT_W", T::Passive}, {"NC", T::NoConnect}, {"V", T::Output}, {"VBOOT_V", T::Passive},
                          {"NC", T::NoConnect}, {"U", T::Output}, {"VBOOT_U", T::Passive}}));
    parts.push_back(part("Home Appliances", "AT42QT1070", "Microchip",
                         "Seven-key capacitive touch sensor (QTouch), I²C or standalone, automatic drift compensation; pads "
                         "need ~1 kΩ series resistors and a hatched ground guard. Pins by function",
                         "SOIC", 14, "U",
                         {{"VDD", T::PowerIn}, {"KEY0", T::Passive}, {"KEY1", T::Passive}, {"KEY2", T::Passive},
                          {"KEY3", T::Passive}, {"KEY4", T::Passive}, {"KEY5", T::Passive}, {"KEY6", T::Passive},
                          {"SCL", T::Input}, {"SDA", T::Bidirectional}, {"CHANGE", T::OpenCollector}, {"RESET", T::Input},
                          {"MODE", T::Input}, {"VSS", T::PowerIn}}));
    parts.push_back(part("Home Appliances", "A3144", "Allegro (generic)",
                         "Unipolar hall-effect switch for drum / fan speed (tacho) and door / lid sensing, open-collector",
                         "SOT23", 3, "U", {{"VCC", T::PowerIn}, {"GND", T::PowerIn}, {"OUT", T::OpenCollector}}));
    {
        StandardPart esp = part("RF", "ESP32-WROOM-32E", "Espressif",
                                "Wi-Fi + Bluetooth LE module (ESP32, 4 MB flash) with a PCB MIFA antenna: place the antenna "
                                "end at the board edge with no copper beneath it and well away from mains copper",
                                "MODULE", 38, "U",
                                {{"GND", T::PowerIn}, {"3V3", T::PowerIn}, {"EN", T::Input}, {"SENSOR_VP", T::Input},
                                 {"SENSOR_VN", T::Input}, {"IO34", T::Input}, {"IO35", T::Input}, {"IO32", T::Bidirectional},
                                 {"IO33", T::Bidirectional}, {"IO25", T::Bidirectional}, {"IO26", T::Bidirectional},
                                 {"IO27", T::Bidirectional}, {"IO14", T::Bidirectional}, {"IO12", T::Bidirectional},
                                 {"GND", T::PowerIn}, {"IO13", T::Bidirectional}, {"SD2", T::NoConnect}, {"SD3", T::NoConnect},
                                 {"CMD", T::NoConnect}, {"CLK", T::NoConnect}, {"SD0", T::NoConnect}, {"SD1", T::NoConnect},
                                 {"IO15", T::Bidirectional}, {"IO2", T::Bidirectional}, {"IO0", T::Bidirectional},
                                 {"IO4", T::Bidirectional}, {"IO16", T::Bidirectional}, {"IO17", T::Bidirectional},
                                 {"IO5", T::Bidirectional}, {"IO18", T::Bidirectional}, {"IO19", T::Bidirectional},
                                 {"NC", T::NoConnect}, {"IO21", T::Bidirectional}, {"RXD0", T::Input}, {"TXD0", T::Output},
                                 {"IO22", T::Bidirectional}, {"IO23", T::Bidirectional}, {"GND", T::PowerIn},
                                 {"GND", T::PowerIn}});
        esp.spec.pins.back().number = "EP";
        esp.spec.package.pitch = 1.27;
        esp.spec.package.bodySize = 18.0;
        parts.push_back(std::move(esp));
    }
    parts.push_back(part("Medical", "DW01A", "Fortune Semiconductor",
                         "One-cell Li-ion protector: over-charge, over-discharge and over-current cut-off through two external "
                         "MOSFETs",
                         "SOT23", 6, "U",
                         {{"OD", T::Output}, {"CS", T::Input}, {"OC", T::Output}, {"TD", T::Passive}, {"VCC", T::PowerIn},
                          {"GND", T::PowerIn}}));
    parts.push_back(part("RF", "BLE-MODULE", "Generic (nRF52832 module class)",
                         "Bluetooth LE module function block with a 50 Ω RF pad for an external antenna, UART host interface. "
                         "Pins by function — use the chosen module's footprint",
                         "HEADER", 8, "U",
                         {{"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"TXD", T::Output}, {"RXD", T::Input},
                          {"RESET", T::Input}, {"GND", T::PowerIn}, {"RF", T::Passive}, {"GND", T::PowerIn}}));
    parts.push_back(part("Timing", "Crystal_26MHz", "Generic (AEC-Q200)", "26 MHz crystal, HC-49/US, ±10 ppm, 10 pF load",
                         "HC49", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    // Marine / industrial communication and isolation.
    parts.push_back(part("Marine & Industrial", "MAX485", "Analog Devices", "RS-485 / RS-422 transceiver (NMEA 0183/2000 bus)",
                         "DIP", 8, "U",
                         {{"RO", T::Output}, {"RE", T::Input}, {"DE", T::Input}, {"DI", T::Input}, {"GND", T::PowerIn},
                          {"A", T::Bidirectional}, {"B", T::Bidirectional}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Marine & Industrial", "PC817", "Sharp", "Optocoupler, 5 kV isolation", "DIP", 4, "U",
                         {{"A", T::Passive}, {"K", T::Passive}, {"E", T::Passive}, {"C", T::Passive}}));
    // Drone / small-robot power and sensing.
    parts.push_back(part("Power", "TP4056", "NanJing Top Power", "1 A Li-ion CC/CV linear charger (ESOP-8)", "SOIC", 8, "U",
                         {{"TEMP", T::Input}, {"PROG", T::Passive}, {"GND", T::PowerIn}, {"VCC", T::PowerIn},
                          {"BAT", T::PowerOut}, {"STDBY", T::OpenCollector}, {"CHRG", T::OpenCollector}, {"CE", T::Input}}));
    parts.push_back(part("Power", "XC6206P332", "Torex", "3.3 V 200 mA LDO, SOT-23", "SOT23", 3, "U",
                         {{"VSS", T::PowerIn}, {"VOUT", T::PowerOut}, {"VIN", T::PowerIn}}));
    parts.push_back(part("Power", "AP2112K-3.3", "Diodes Inc.", "3.3 V 600 mA LDO with enable, SOT-23-5", "SOT23", 5, "U",
                         {{"VIN", T::PowerIn}, {"GND", T::PowerIn}, {"EN", T::Input}, {"NC", T::NoConnect},
                          {"VOUT", T::PowerOut}}));
    parts.push_back(part("Sensors", "MPU-6050", "TDK InvenSense", "6-axis gyroscope + accelerometer, I2C", "QFN", 24, "U",
                         {{"CLKIN", T::Input}, {"NC", T::NoConnect}, {"NC", T::NoConnect}, {"NC", T::NoConnect},
                          {"NC", T::NoConnect}, {"AUX_DA", T::NoConnect}, {"AUX_CL", T::NoConnect}, {"VLOGIC", T::PowerIn},
                          {"AD0", T::Input}, {"REGOUT", T::Passive}, {"FSYNC", T::Input}, {"INT", T::Output},
                          {"VDD", T::PowerIn}, {"NC", T::NoConnect}, {"NC", T::NoConnect}, {"NC", T::NoConnect},
                          {"NC", T::NoConnect}, {"GND", T::PowerIn}, {"RESV", T::NoConnect}, {"CPOUT", T::Passive},
                          {"RESV", T::NoConnect}, {"CLKOUT", T::NoConnect}, {"SCL", T::Bidirectional}, {"SDA", T::Bidirectional}}));
    parts.push_back(part("RF", "NRF24L01_Module", "Nordic Semiconductor", "2.4 GHz transceiver module, SPI, 2×4 header",
                         "HEADER2", 8, "U",
                         {{"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"CE", T::Input}, {"CSN", T::Input}, {"SCK", T::Input},
                          {"MOSI", T::Input}, {"MISO", T::Output}, {"IRQ", T::Output}}));
    parts.push_back(part("Connectors", "Header_2x04", "Generic", "2.54 mm pin header, 2×4", "HEADER2", 8, "J",
                         {{"1", T::Passive}, {"2", T::Passive}, {"3", T::Passive}, {"4", T::Passive},
                          {"5", T::Passive}, {"6", T::Passive}, {"7", T::Passive}, {"8", T::Passive}}));
    // Medical: biopotential front end and patient isolation.
    parts.push_back(part("Medical", "INA333", "Texas Instruments",
                         "Micro-power zero-drift instrumentation amplifier (G = 1 + 100 kΩ / RG), ECG/EEG front ends", "SOIC", 8,
                         "U",
                         {{"RG", T::Passive}, {"-IN", T::Input}, {"+IN", T::Input}, {"V-", T::PowerIn}, {"REF", T::Input},
                          {"VOUT", T::Output}, {"V+", T::PowerIn}, {"RG", T::Passive}}));
    parts.push_back(part("Medical", "ADuM1201", "Analog Devices",
                         "Dual-channel digital isolator (1 forward, 1 reverse), 2.5 kV rms, patient/SELV barriers", "SOIC", 8,
                         "U",
                         {{"VDD1", T::PowerIn}, {"VOA", T::Output}, {"VIB", T::Input}, {"GND1", T::PowerIn},
                          {"GND2", T::PowerIn}, {"VIA", T::Input}, {"VOB", T::Output}, {"VDD2", T::PowerIn}}));
    // Top microcontrollers of Arm-ecosystem vendors, Microchip, ST and TI (generated: tools/fetch_mcu_pinouts.py).
#include "StandardMcus.inc"
    // Production catalog for robotics, automotive and industrial boards (generated: tools/fetch_catalog_parts.py).
#include "StandardCatalog.inc"
    // Catalog growth from whole KiCad libraries: regulators, interface, logic, data converters, memory, MCUs,
    // drivers, sensors, pin headers (generated: tools/fetch_catalog_parts.py --extra; frozen list in
    // tools/catalog_extra_parts.tsv).
#include "StandardCatalogExtra.inc"
    // Microcontroller clock crystals in the packages fabs stock (HC-49 through-hole, 3225 SMD, 3215 / cylinder 32 kHz).
    parts.push_back(part("Timing", "Crystal_16MHz", "Generic", "16 MHz crystal, HC-49/S, ±30 ppm, 20 pF load (2 × 22 pF caps)",
                         "HC49", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_16MHz_3225", "Generic", "16 MHz crystal, 3.2 × 2.5 mm SMD, ±20 ppm, 12 pF load",
                         "XTAL3225", 4, "Y", {{"X1", T::Passive}, {"GND", T::Passive}, {"X2", T::Passive}, {"GND", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_8MHz_3225", "Generic", "8 MHz crystal, 3.2 × 2.5 mm SMD, ±20 ppm, 12 pF load",
                         "XTAL3225", 4, "Y", {{"X1", T::Passive}, {"GND", T::Passive}, {"X2", T::Passive}, {"GND", T::Passive}}));
    // Crystals the common radios / MCUs want: ESP32-C3 / S3 40 MHz, Ethernet PHY 25 MHz, USB 12 MHz.
    parts.push_back(part("Timing", "Crystal_40MHz_3225", "Generic", "40 MHz crystal (ESP32-C3 / S3 / C6 reference), 3.2 × 2.5 mm SMD, ±10 ppm, 10 pF load (2 × 15 pF caps)",
                         "XTAL3225", 4, "Y", {{"X1", T::Passive}, {"GND", T::Passive}, {"X2", T::Passive}, {"GND", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_25MHz_3225", "Generic", "25 MHz crystal (Ethernet PHY / switch reference), 3.2 × 2.5 mm SMD, ±10 ppm, 10 pF load",
                         "XTAL3225", 4, "Y", {{"X1", T::Passive}, {"GND", T::Passive}, {"X2", T::Passive}, {"GND", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_12MHz_3225", "Generic", "12 MHz crystal (USB full-speed / MCU), 3.2 × 2.5 mm SMD, ±20 ppm, 12 pF load",
                         "XTAL3225", 4, "Y", {{"X1", T::Passive}, {"GND", T::Passive}, {"X2", T::Passive}, {"GND", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_32.768kHz_3215", "Generic", "32.768 kHz RTC crystal, 3.2 × 1.5 mm SMD, 12.5 pF load",
                         "XTAL3215", 2, "Y", {{"1", T::Passive}, {"2", T::Passive}}));
    parts.push_back(part("Timing", "Crystal_32.768kHz_Cylinder", "Generic",
                         "32.768 kHz RTC crystal, 2 × 6 mm cylinder (through-hole), 12.5 pF load", "XTALCYL", 2, "Y",
                         {{"1", T::Passive}, {"2", T::Passive}}));
    // Board objects as parts: a probe pad and a net tie (generic lands, no datasheet).
    parts.push_back(part("Connectors", "TestPoint", "Generic",
                         "Test point: 1.5 mm round SMD probe pad for bed-of-nails / flying-probe test", "TESTPOINT", 1, "TP",
                         {{"TP", T::Passive}}));
    parts.push_back(part("Connectors", "NetTie-2", "Generic",
                         "Net tie: joins two nets at one point (AGND to GND under an ADC, a current-sense Kelvin tap); the "
                         "DRC accepts its own short", "NETTIE", 2, "NT", {{"1", T::Passive}, {"2", T::Passive}}));
    for (size_t k = parts.size() - 2; k < parts.size(); ++k) parts[k].spec.datasheet = "Generic land (no datasheet)";
    // Catalog parts the KiCad library does not carry, from their datasheets.
    catalog(parts, "Sensors", "ADS1256IDBR", "Texas Instruments",
            "24-bit 30 kSPS delta-sigma ADC, 8 channels, SPI",
            "U", "TSSOP", 28, 0.65, 5.3, "SSOP-28 (DB)",
            {{"1", "AVDD", T::PowerIn}, {"2", "AGND", T::PowerIn}, {"3", "VREFN", T::Input}, {"4", "VREFP", T::Input},
            {"5", "AINCOM", T::Input}, {"6", "AIN0", T::Input}, {"7", "AIN1", T::Input}, {"8", "AIN2", T::Input},
            {"9", "AIN3", T::Input}, {"10", "AIN4", T::Input}, {"11", "AIN5", T::Input}, {"12", "AIN6", T::Input},
            {"13", "AIN7", T::Input}, {"14", "SYNC/PDWN", T::Input}, {"15", "RESET", T::Input}, {"16", "DVDD", T::PowerIn},
            {"17", "DGND", T::PowerIn}, {"18", "XTAL2", T::Passive}, {"19", "XTAL1/CLKIN", T::Input}, {"20", "CS", T::Input},
            {"21", "DRDY", T::Output}, {"22", "DOUT", T::Output}, {"23", "DIN", T::Input}, {"24", "SCLK", T::Input},
            {"25", "D0/CLKOUT", T::Bidirectional}, {"26", "D1", T::Bidirectional}, {"27", "D2", T::Bidirectional}, {"28", "D3", T::Bidirectional}});
    catalog(parts, "Motor Control", "DRV8301DCAR", "Texas Instruments",
            "Three-phase gate driver with dual current-sense amplifiers and 1.5 A buck regulator",
            "U", "TSSOP", 56, 0.5, 6.1, "HTSSOP-56 (DCA)",
            {{"1", "RT_CLK", T::Input}, {"2", "COMP", T::Output}, {"3", "VSENSE", T::Input}, {"4", "PWRGD", T::OpenCollector},
            {"5", "nOCTW", T::OpenCollector}, {"6", "nFAULT", T::OpenCollector}, {"7", "DTC", T::Input}, {"8", "nSCS", T::Input},
            {"9", "SDI", T::Input}, {"10", "SDO", T::Output}, {"11", "SCLK", T::Input}, {"12", "DC_CAL", T::Input},
            {"13", "GVDD", T::PowerOut}, {"14", "CP1", T::Passive}, {"15", "CP2", T::Passive}, {"16", "EN_GATE", T::Input},
            {"17", "INH_A", T::Input}, {"18", "INL_A", T::Input}, {"19", "INH_B", T::Input}, {"20", "INL_B", T::Input},
            {"21", "INH_C", T::Input}, {"22", "INL_C", T::Input}, {"23", "DVDD", T::PowerOut}, {"24", "REF", T::Input},
            {"25", "SO1", T::Output}, {"26", "SO2", T::Output}, {"27", "AVDD", T::PowerOut}, {"28", "AGND", T::PowerIn},
            {"29", "PVDD1", T::PowerIn}, {"30", "SP2", T::Input}, {"31", "SN2", T::Input}, {"32", "SP1", T::Input},
            {"33", "SN1", T::Input}, {"34", "SL_C", T::Input}, {"35", "GL_C", T::Output}, {"36", "SH_C", T::Input},
            {"37", "GH_C", T::Output}, {"38", "BST_C", T::Passive}, {"39", "SL_B", T::Input}, {"40", "GL_B", T::Output},
            {"41", "SH_B", T::Input}, {"42", "GH_B", T::Output}, {"43", "BST_B", T::Passive}, {"44", "SL_A", T::Input},
            {"45", "GL_A", T::Output}, {"46", "SH_A", T::Input}, {"47", "GH_A", T::Output}, {"48", "BST_A", T::Passive},
            {"49", "VDD_SPI", T::PowerIn}, {"50", "PH", T::Output}, {"51", "PH", T::Output}, {"52", "BST_BK", T::Passive},
            {"53", "PVDD2", T::PowerIn}, {"54", "PVDD2", T::PowerIn}, {"55", "EN_BUCK", T::Input}, {"56", "SS_TR", T::Input},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Regulators", "LM5116MH/NOPB", "Texas Instruments",
            "Wide-input (6–100 V) synchronous buck controller",
            "U", "TSSOP", 20, 0.65, 4.4, "HTSSOP-20 (PWP)",
            {{"1", "VIN", T::PowerIn}, {"2", "UVLO", T::Input}, {"3", "RT/SYNC", T::Input}, {"4", "EN", T::Input},
            {"5", "RAMP", T::Input}, {"6", "AGND", T::PowerIn}, {"7", "SS", T::Input}, {"8", "FB", T::Input},
            {"9", "COMP", T::Output}, {"10", "VOUT", T::Input}, {"11", "DEMB", T::Input}, {"12", "CS", T::Input},
            {"13", "CSG", T::PowerIn}, {"14", "PGND", T::PowerIn}, {"15", "LO", T::Output}, {"16", "VCC", T::PowerOut},
            {"17", "VCCX", T::PowerIn}, {"18", "HB", T::Passive}, {"19", "HO", T::Output}, {"20", "SW", T::Output},
            {"EP", "EP", T::PowerIn}});
    catalog(parts, "Sensors", "MA702GQ", "Monolithic Power Systems",
            "12-bit angle sensor (magnetic encoder), SPI/ABZ/PWM",
            "U", "QFN", 16, 0.5, 3.0, "QFN-16 3×3",
            {{"1", "SSD", T::Output}, {"2", "A", T::Output}, {"3", "Z", T::Output}, {"4", "MOSI", T::Input},
            {"5", "CS", T::Input}, {"6", "B", T::Output}, {"7", "MISO", T::Output}, {"8", "GND", T::PowerIn},
            {"9", "PWM", T::Output}, {"10", "TEST", T::Input}, {"11", "MGL", T::Output}, {"12", "SCLK", T::Input},
            {"13", "VDD", T::PowerIn}, {"14", "NC", T::NoConnect}, {"15", "SSCK", T::Input}, {"16", "MGH", T::Output},
            {"EP", "EP", T::PowerIn}});
    catalog(parts, "Interface", "TJA1101BHN", "NXP Semiconductors",
            "100BASE-T1 automotive Ethernet PHY, MII/RMII",
            "U", "QFN", 36, 0.5, 6.0, "HVQFN-36 6×6",
            {{"1", "MDC", T::Input}, {"2", "INT_N", T::OpenCollector}, {"3", "RST_N", T::Input}, {"4", "SEL_1V8", T::Input},
            {"5", "XO", T::Output}, {"6", "XI", T::Input}, {"7", "VDDA(3V3)", T::PowerIn}, {"8", "WAKE_IN_OUT", T::Bidirectional},
            {"9", "VBAT", T::PowerIn}, {"10", "INH", T::Output}, {"11", "VDDA(TX)", T::PowerIn}, {"12", "TRX_P", T::Bidirectional},
            {"13", "TRX_M", T::Bidirectional}, {"14", "VDDA(TX)", T::PowerIn}, {"15", "VDDD(3V3)", T::PowerIn}, {"16", "VDDD(1V8)", T::PowerIn},
            {"17", "RXER/CONFIG3/TXCLK", T::Bidirectional}, {"18", "RXDV/CONFIG2/CRSDV", T::Bidirectional}, {"19", "VDD(IO)", T::PowerIn}, {"20", "CLK_IN_OUT", T::Bidirectional},
            {"21", "RXD3/CONFIG1", T::Bidirectional}, {"22", "RXD2/CONFIG0", T::Bidirectional}, {"23", "RXD1/PHYAD2", T::Bidirectional}, {"24", "RXD0/PHYAD1", T::Bidirectional},
            {"25", "RXC/REF_CLK", T::Bidirectional}, {"26", "GND", T::PowerIn}, {"27", "VDD(IO)", T::PowerIn}, {"28", "TXC", T::Bidirectional},
            {"29", "TXEN", T::Input}, {"30", "TXD3", T::Input}, {"31", "TXD2", T::Input}, {"32", "TXD1", T::Input},
            {"33", "TXD0", T::Input}, {"34", "TXER", T::Input}, {"35", "EN", T::Input}, {"36", "MDIO", T::Bidirectional},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Audio", "WT2003HP8-32N", "Waytronic",
            "MP3 / WAV voice IC with built-in storage, SD / SPI-flash / USB, UART control, PWM speaker drive",
            "U", "QFN", 32, 0.4, 4.0, "QFN-32 4×4",
            {{"1", "LED2/KEY11", T::Bidirectional}, {"2", "RXD/KEY9/DATA1/CLK2", T::Bidirectional}, {"3", "LED3/KEY12/ADC0", T::Bidirectional}, {"4", "AGND", T::PowerIn},
            {"5", "VREG", T::PowerOut}, {"6", "LED4/KEY13/DAC", T::Output}, {"7", "VOUT", T::PowerOut}, {"8", "VCC", T::PowerIn},
            {"9", "VCC-SPK", T::PowerIn}, {"10", "PWM+", T::Output}, {"11", "PWM-", T::Output}, {"12", "LED5/KEY14/BUSY", T::Bidirectional},
            {"13", "LED6", T::Bidirectional}, {"14", "LED7", T::Bidirectional}, {"15", "TXD/KEY15/ADC1/DATA2", T::Bidirectional}, {"16", "IO3", T::Bidirectional},
            {"17", "LED8/IO4", T::Bidirectional}, {"18", "NC", T::NoConnect}, {"19", "NC", T::NoConnect}, {"20", "NC", T::NoConnect},
            {"21", "NC", T::NoConnect}, {"22", "NC", T::NoConnect}, {"23", "COM0/KEY1/DAT/CS", T::Bidirectional}, {"24", "COM1/KEY2/CMD/DO", T::Bidirectional},
            {"25", "COM2/KEY3/CLK", T::Bidirectional}, {"26", "ICEDAT/KEY4/D-/IO1", T::Bidirectional}, {"27", "ICECLK/KEY5/D+/IO2", T::Bidirectional}, {"28", "COM3/KEY6/SDDAT", T::Bidirectional},
            {"29", "COM4/KEY7/SDCMD", T::Bidirectional}, {"30", "LED0/KEY8/SDCLK", T::Bidirectional}, {"31", "LED1/KEY10", T::Bidirectional}, {"32", "LED9/IO5", T::Bidirectional},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Power", "TPS65381QDAPRQ1", "Texas Instruments",
            "Automotive multi-rail power supply for safety MCUs (buck pre-regulator, 5 V / 3.3 V LDOs, sensor supply, watchdog, SPI)",
            "U", "TSSOP", 32, 0.65, 6.2, "HTSSOP-32 (DAP)",
            {{"1", "VBAT_SAFING", T::PowerIn}, {"2", "VCP", T::PowerOut}, {"3", "CP1", T::Passive}, {"4", "CP2", T::Passive},
            {"5", "PGND", T::PowerIn}, {"6", "NRES", T::OpenCollector}, {"7", "DIAG_OUT", T::Output}, {"8", "NCS", T::Input},
            {"9", "SDI", T::Input}, {"10", "SDO", T::Output}, {"11", "SCLK", T::Input}, {"12", "RSTEXT", T::Input},
            {"13", "ERROR/WDI", T::Input}, {"14", "CANWU", T::Input}, {"15", "VSFB1", T::Input}, {"16", "VSIN", T::PowerIn},
            {"17", "VSOUT1", T::PowerOut}, {"18", "VTRACK1", T::Input}, {"19", "GND", T::PowerIn}, {"20", "VDD5", T::PowerOut},
            {"21", "VDD3/5", T::PowerOut}, {"22", "VDDIO", T::PowerIn}, {"23", "GND", T::PowerIn}, {"24", "VDD1_SENSE", T::Input},
            {"25", "PGND", T::PowerIn}, {"26", "VDD1_G", T::Output}, {"27", "VDD6", T::PowerIn}, {"28", "SDN6", T::Passive},
            {"29", "VBATP", T::PowerIn}, {"30", "IGN", T::Input}, {"31", "SEL_VDD3/5", T::Input}, {"32", "ENDRV", T::OpenCollector},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Motor Control", "DRV8353SRTAR", "Texas Instruments",
            "100-V three-phase smart gate driver, SPI, three current-sense amplifiers",
            "U", "QFN", 40, 0.5, 6, "WQFN-40 6×6 (RTA)",
            {{"1", "CPL", T::Passive}, {"2", "CPH", T::Passive}, {"3", "VM", T::PowerIn}, {"4", "VDRAIN", T::Input},
            {"5", "VCP", T::PowerOut}, {"6", "GHA", T::Output}, {"7", "SHA", T::Input}, {"8", "GLA", T::Output},
            {"9", "SPA", T::Input}, {"10", "SNA", T::Input}, {"11", "SNB", T::Input}, {"12", "SPB", T::Input},
            {"13", "GLB", T::Output}, {"14", "SHB", T::Input}, {"15", "GHB", T::Output}, {"16", "GHC", T::Output},
            {"17", "SHC", T::Input}, {"18", "GLC", T::Output}, {"19", "SPC", T::Input}, {"20", "SNC", T::Input},
            {"21", "SOC", T::Output}, {"22", "SOB", T::Output}, {"23", "SOA", T::Output}, {"24", "VREF", T::PowerIn},
            {"25", "AGND", T::PowerIn}, {"26", "nFAULT", T::OpenCollector}, {"27", "SDO", T::OpenCollector}, {"28", "SDI", T::Input},
            {"29", "SCLK", T::Input}, {"30", "nSCS", T::Input}, {"31", "ENABLE", T::Input}, {"32", "INHA", T::Input},
            {"33", "INLA", T::Input}, {"34", "INHB", T::Input}, {"35", "INLB", T::Input}, {"36", "INHC", T::Input},
            {"37", "INLC", T::Input}, {"38", "DVDD", T::PowerOut}, {"39", "GND", T::PowerIn}, {"40", "VGLS", T::PowerOut},
            {"EP", "EP", T::PowerIn}});
    catalog(parts, "Sensors", "BMX160", "Bosch Sensortec",
            "9-axis IMU (accelerometer, gyroscope, geomagnetic sensor), I²C / SPI, LGA-14 2.5 × 3.0 mm",
            "U", "LGA", 14, 0, 3, "LGA-14 2.5×3.0 (Bosch land pattern)",
            {{"1", "SDO", T::Bidirectional}, {"2", "GND", T::PowerIn}, {"3", "GND", T::PowerIn}, {"4", "INT1", T::Bidirectional},
            {"5", "VDDIO", T::PowerIn}, {"6", "GNDIO", T::PowerIn}, {"7", "GND", T::PowerIn}, {"8", "VDD", T::PowerIn},
            {"9", "INT2", T::Bidirectional}, {"10", "DNC", T::NoConnect}, {"11", "DNC", T::NoConnect}, {"12", "CSB", T::Input},
            {"13", "SCx", T::Input}, {"14", "SDx", T::Bidirectional}});
    parts.back().spec.package.bodyDepth = 2.5;
    parts.back().spec.package.lands = {{-1.2625, -0.75, 0.675, 0.25}, {-1.2625, -0.25, 0.675, 0.25}, {-1.2625, 0.25, 0.675, 0.25}, {-1.2625, 0.75, 0.675, 0.25}, {-0.5, 1.0125, 0.25, 0.675}, {0, 1.0125, 0.25, 0.675}, {0.5, 1.0125, 0.25, 0.675}, {1.2625, 0.75, 0.675, 0.25}, {1.2625, 0.25, 0.675, 0.25}, {1.2625, -0.25, 0.675, 0.25}, {1.2625, -0.75, 0.675, 0.25}, {0.5, -1.0125, 0.25, 0.675}, {0, -1.0125, 0.25, 0.675}, {-0.5, -1.0125, 0.25, 0.675}};
    catalog(parts, "Microcontrollers · Texas Instruments", "TMS320F28379DPTPS", "Texas Instruments",
            "Dual-core C28x + dual CLA real-time MCU, 200 MHz, 1 MB flash, four 16-bit ADCs, HLQFP-176 PowerPAD",
            "U", "LQFP", 176, 0.5, 24, "HLQFP-176 (PTP)",
            {{"1", "GPIO10", T::Bidirectional}, {"2", "GPIO11", T::Bidirectional}, {"3", "VDDIO", T::PowerIn}, {"4", "GPIO12", T::Bidirectional},
            {"5", "GPIO13", T::Bidirectional}, {"6", "GPIO14", T::Bidirectional}, {"7", "GPIO15", T::Bidirectional}, {"8", "GPIO16", T::Bidirectional},
            {"9", "GPIO17", T::Bidirectional}, {"10", "GPIO18", T::Bidirectional}, {"11", "VDDIO", T::PowerIn}, {"12", "GPIO19", T::Bidirectional},
            {"13", "GPIO20", T::Bidirectional}, {"14", "GPIO21", T::Bidirectional}, {"15", "VDDIO", T::PowerIn}, {"16", "VDD", T::PowerIn},
            {"17", "GPIO99", T::Bidirectional}, {"18", "GPIO8", T::Bidirectional}, {"19", "GPIO9", T::Bidirectional}, {"20", "VDDIO", T::PowerIn},
            {"21", "VDD", T::PowerIn}, {"22", "GPIO22", T::Bidirectional}, {"23", "GPIO23", T::Bidirectional}, {"24", "GPIO24", T::Bidirectional},
            {"25", "GPIO25", T::Bidirectional}, {"26", "VDDIO", T::PowerIn}, {"27", "GPIO26", T::Bidirectional}, {"28", "GPIO27", T::Bidirectional},
            {"29", "ADCINC4", T::Input}, {"30", "ADCINC3", T::Input}, {"31", "ADCINC2", T::Input}, {"32", "VREFLOC", T::Input},
            {"33", "VREFLOA", T::Input}, {"34", "VSSA", T::PowerIn}, {"35", "VREFHIC", T::Input}, {"36", "VDDA", T::PowerIn},
            {"37", "VREFHIA", T::Input}, {"38", "ADCINA5", T::Input}, {"39", "ADCINA4", T::Input}, {"40", "ADCINA3", T::Input},
            {"41", "ADCINA2", T::Input}, {"42", "ADCINA1", T::Input}, {"43", "ADCINA0", T::Input}, {"44", "ADCIN14", T::Input},
            {"45", "ADCIN15", T::Input}, {"46", "ADCINB0", T::Input}, {"47", "ADCINB1", T::Input}, {"48", "ADCINB2", T::Input},
            {"49", "ADCINB3", T::Input}, {"50", "VREFLOB", T::Input}, {"51", "VREFLOD", T::Input}, {"52", "VSSA", T::PowerIn},
            {"53", "VREFHIB", T::Input}, {"54", "VDDA", T::PowerIn}, {"55", "VREFHID", T::Input}, {"56", "ADCIND0", T::Input},
            {"57", "ADCIND1", T::Input}, {"58", "ADCIND2", T::Input}, {"59", "ADCIND3", T::Input}, {"60", "ADCIND4", T::Input},
            {"61", "VDD", T::PowerIn}, {"62", "VDDIO", T::PowerIn}, {"63", "GPIO30", T::Bidirectional}, {"64", "GPIO28", T::Bidirectional},
            {"65", "GPIO29", T::Bidirectional}, {"66", "GPIO31", T::Bidirectional}, {"67", "GPIO32", T::Bidirectional}, {"68", "VDDIO", T::PowerIn},
            {"69", "GPIO33", T::Bidirectional}, {"70", "GPIO34", T::Bidirectional}, {"71", "GPIO35", T::Bidirectional}, {"72", "VDD3VFL", T::PowerIn},
            {"73", "FLT1", T::NoConnect}, {"74", "FLT2", T::NoConnect}, {"75", "VDDIO", T::PowerIn}, {"76", "VDD", T::PowerIn},
            {"77", "TDI", T::Input}, {"78", "TDO", T::Output}, {"79", "TRST", T::Input}, {"80", "TMS", T::Input},
            {"81", "TCK", T::Input}, {"82", "VDDIO", T::PowerIn}, {"83", "GPIO36", T::Bidirectional}, {"84", "GPIO37", T::Bidirectional},
            {"85", "GPIO38", T::Bidirectional}, {"86", "GPIO39", T::Bidirectional}, {"87", "GPIO40", T::Bidirectional}, {"88", "VDDIO", T::PowerIn},
            {"89", "GPIO41", T::Bidirectional}, {"90", "GPIO48", T::Bidirectional}, {"91", "VDDIO", T::PowerIn}, {"92", "ERRORSTS", T::Output},
            {"93", "GPIO49", T::Bidirectional}, {"94", "GPIO50", T::Bidirectional}, {"95", "GPIO51", T::Bidirectional}, {"96", "GPIO52", T::Bidirectional},
            {"97", "GPIO53", T::Bidirectional}, {"98", "GPIO54", T::Bidirectional}, {"99", "VDDIO", T::PowerIn}, {"100", "GPIO55", T::Bidirectional},
            {"101", "GPIO56", T::Bidirectional}, {"102", "GPIO57", T::Bidirectional}, {"103", "GPIO58", T::Bidirectional}, {"104", "GPIO59", T::Bidirectional},
            {"105", "GPIO60", T::Bidirectional}, {"106", "VDDIO", T::PowerIn}, {"107", "GPIO61", T::Bidirectional}, {"108", "GPIO62", T::Bidirectional},
            {"109", "GPIO63", T::Bidirectional}, {"110", "GPIO64", T::Bidirectional}, {"111", "GPIO65", T::Bidirectional}, {"112", "GPIO66", T::Bidirectional},
            {"113", "GPIO44", T::Bidirectional}, {"114", "VDDIO", T::PowerIn}, {"115", "GPIO45", T::Bidirectional}, {"116", "VDDIO", T::PowerIn},
            {"117", "VDD", T::PowerIn}, {"118", "GPIO133", T::Bidirectional}, {"119", "VREGENZ", T::Input}, {"120", "VDDOSC", T::PowerIn},
            {"121", "X2", T::Output}, {"122", "VSSOSC", T::PowerIn}, {"123", "X1", T::Input}, {"124", "XRS", T::Bidirectional},
            {"125", "VDDOSC", T::PowerIn}, {"126", "VDD", T::PowerIn}, {"127", "VDDIO", T::PowerIn}, {"128", "GPIO46", T::Bidirectional},
            {"129", "GPIO47", T::Bidirectional}, {"130", "GPIO42", T::Bidirectional}, {"131", "GPIO43", T::Bidirectional}, {"132", "GPIO67", T::Bidirectional},
            {"133", "GPIO68", T::Bidirectional}, {"134", "GPIO69", T::Bidirectional}, {"135", "GPIO70", T::Bidirectional}, {"136", "GPIO71", T::Bidirectional},
            {"137", "VDD", T::PowerIn}, {"138", "VDDIO", T::PowerIn}, {"139", "GPIO72", T::Bidirectional}, {"140", "GPIO73", T::Bidirectional},
            {"141", "GPIO74", T::Bidirectional}, {"142", "GPIO75", T::Bidirectional}, {"143", "GPIO76", T::Bidirectional}, {"144", "GPIO77", T::Bidirectional},
            {"145", "GPIO78", T::Bidirectional}, {"146", "GPIO79", T::Bidirectional}, {"147", "VDDIO", T::PowerIn}, {"148", "GPIO80", T::Bidirectional},
            {"149", "GPIO81", T::Bidirectional}, {"150", "GPIO82", T::Bidirectional}, {"151", "GPIO83", T::Bidirectional}, {"152", "VDDIO", T::PowerIn},
            {"153", "VDD", T::PowerIn}, {"154", "GPIO84", T::Bidirectional}, {"155", "GPIO85", T::Bidirectional}, {"156", "GPIO86", T::Bidirectional},
            {"157", "GPIO87", T::Bidirectional}, {"158", "VDD", T::PowerIn}, {"159", "VDDIO", T::PowerIn}, {"160", "GPIO0", T::Bidirectional},
            {"161", "GPIO1", T::Bidirectional}, {"162", "GPIO2", T::Bidirectional}, {"163", "GPIO3", T::Bidirectional}, {"164", "GPIO4", T::Bidirectional},
            {"165", "GPIO5", T::Bidirectional}, {"166", "GPIO6", T::Bidirectional}, {"167", "GPIO7", T::Bidirectional}, {"168", "VDDIO", T::PowerIn},
            {"169", "VDD", T::PowerIn}, {"170", "GPIO88", T::Bidirectional}, {"171", "GPIO89", T::Bidirectional}, {"172", "GPIO90", T::Bidirectional},
            {"173", "GPIO91", T::Bidirectional}, {"174", "GPIO92", T::Bidirectional}, {"175", "GPIO93", T::Bidirectional}, {"176", "GPIO94", T::Bidirectional},
            {"EP", "VSS", T::PowerIn}});
    catalog(parts, "Microcontrollers · Microchip", "ATSAMD51P20A-AUT", "Microchip",
            "Cortex-M4F 120 MHz, 1 MB flash, 256 KB SRAM, FPU (128-pin SAM D51: 99 I/O), TQFP-128",
            "U", "LQFP", 128, 0.4, 14, "TQFP-128 14×14",
            {{"1", "PA00", T::Bidirectional}, {"2", "PA01", T::Bidirectional}, {"3", "PC00", T::Bidirectional}, {"4", "PC01", T::Bidirectional},
            {"5", "GND", T::PowerIn}, {"6", "VDDANA", T::PowerIn}, {"7", "PC02", T::Bidirectional}, {"8", "PC03", T::Bidirectional},
            {"9", "PA02", T::Bidirectional}, {"10", "PA03", T::Bidirectional}, {"11", "PB04", T::Bidirectional}, {"12", "PB05", T::Bidirectional},
            {"13", "PD00", T::Bidirectional}, {"14", "GNDANA", T::PowerIn}, {"15", "VDDANA", T::PowerIn}, {"16", "PD01", T::Bidirectional},
            {"17", "PB06", T::Bidirectional}, {"18", "PB07", T::Bidirectional}, {"19", "PB08", T::Bidirectional}, {"20", "PB09", T::Bidirectional},
            {"21", "PA04", T::Bidirectional}, {"22", "PA05", T::Bidirectional}, {"23", "PA06", T::Bidirectional}, {"24", "PA07", T::Bidirectional},
            {"25", "GNDANA", T::PowerIn}, {"26", "VDDANA", T::PowerIn}, {"27", "PC04", T::Bidirectional}, {"28", "PC05", T::Bidirectional},
            {"29", "PC06", T::Bidirectional}, {"30", "PC07", T::Bidirectional}, {"31", "GND", T::PowerIn}, {"32", "VDDIOB", T::PowerIn},
            {"33", "PA08", T::Bidirectional}, {"34", "PA09", T::Bidirectional}, {"35", "PA10", T::Bidirectional}, {"36", "PA11", T::Bidirectional},
            {"37", "VDDIOB", T::PowerIn}, {"38", "GND", T::PowerIn}, {"39", "PB10", T::Bidirectional}, {"40", "PB11", T::Bidirectional},
            {"41", "PB12", T::Bidirectional}, {"42", "PB13", T::Bidirectional}, {"43", "PB14", T::Bidirectional}, {"44", "PB15", T::Bidirectional},
            {"45", "GND", T::PowerIn}, {"46", "VDDIO", T::PowerIn}, {"47", "PD08", T::Bidirectional}, {"48", "PD09", T::Bidirectional},
            {"49", "PD10", T::Bidirectional}, {"50", "PD11", T::Bidirectional}, {"51", "PD12", T::Bidirectional}, {"52", "PC10", T::Bidirectional},
            {"53", "GND", T::PowerIn}, {"54", "VDDIO", T::PowerIn}, {"55", "PC11", T::Bidirectional}, {"56", "PC12", T::Bidirectional},
            {"57", "PC13", T::Bidirectional}, {"58", "PC14", T::Bidirectional}, {"59", "PC15", T::Bidirectional}, {"60", "PA12", T::Bidirectional},
            {"61", "PA13", T::Bidirectional}, {"62", "PA14", T::Bidirectional}, {"63", "PA15", T::Bidirectional}, {"64", "GND", T::PowerIn},
            {"65", "VDDIO", T::PowerIn}, {"66", "PA16", T::Bidirectional}, {"67", "PA17", T::Bidirectional}, {"68", "PA18", T::Bidirectional},
            {"69", "PA19", T::Bidirectional}, {"70", "PC16", T::Bidirectional}, {"71", "PC17", T::Bidirectional}, {"72", "PC18", T::Bidirectional},
            {"73", "PC19", T::Bidirectional}, {"74", "PC20", T::Bidirectional}, {"75", "PC21", T::Bidirectional}, {"76", "PC22", T::Bidirectional},
            {"77", "PC23", T::Bidirectional}, {"78", "GND", T::PowerIn}, {"79", "VDDIO", T::PowerIn}, {"80", "PD20", T::Bidirectional},
            {"81", "PD21", T::Bidirectional}, {"82", "PB16", T::Bidirectional}, {"83", "PB17", T::Bidirectional}, {"84", "PB18", T::Bidirectional},
            {"85", "PB19", T::Bidirectional}, {"86", "PB20", T::Bidirectional}, {"87", "PB21", T::Bidirectional}, {"88", "PA20", T::Bidirectional},
            {"89", "PA21", T::Bidirectional}, {"90", "GND", T::PowerIn}, {"91", "VDDIO", T::PowerIn}, {"92", "PA22", T::Bidirectional},
            {"93", "PA23", T::Bidirectional}, {"94", "PA24", T::Bidirectional}, {"95", "PA25", T::Bidirectional}, {"96", "GND", T::PowerIn},
            {"97", "VDDIO", T::PowerIn}, {"98", "PB22", T::Bidirectional}, {"99", "PB23", T::Bidirectional}, {"100", "PB24", T::Bidirectional},
            {"101", "PB25", T::Bidirectional}, {"102", "PB26", T::Bidirectional}, {"103", "PB27", T::Bidirectional}, {"104", "PB28", T::Bidirectional},
            {"105", "PB29", T::Bidirectional}, {"106", "GND", T::PowerIn}, {"107", "VDDIO", T::PowerIn}, {"108", "PC24", T::Bidirectional},
            {"109", "PC25", T::Bidirectional}, {"110", "PC26", T::Bidirectional}, {"111", "PC27", T::Bidirectional}, {"112", "PC28", T::Bidirectional},
            {"113", "PA27", T::Bidirectional}, {"114", "RESETN", T::Input}, {"115", "VDDCORE", T::PowerOut}, {"116", "GNDIO", T::PowerIn},
            {"117", "VSW", T::Passive}, {"118", "VDDIO", T::PowerIn}, {"119", "PA30", T::Bidirectional}, {"120", "PA31", T::Bidirectional},
            {"121", "PB30", T::Bidirectional}, {"122", "PB31", T::Bidirectional}, {"123", "PC30", T::Bidirectional}, {"124", "PC31", T::Bidirectional},
            {"125", "PB00", T::Bidirectional}, {"126", "PB01", T::Bidirectional}, {"127", "PB02", T::Bidirectional}, {"128", "PB03", T::Bidirectional}});
    catalog(parts, "Op-Amps", "INA826AIDR", "Texas Instruments",
            "Precision instrumentation amplifier, 36 V, rail-to-rail output, gain set by one resistor",
            "U", "SOIC", 8, 1.27, 3.9, "SOIC-8 (D)",
            {{"1", "-IN", T::Input}, {"2", "RG", T::Passive}, {"3", "RG", T::Passive}, {"4", "+IN", T::Input},
            {"5", "-VS", T::PowerIn}, {"6", "REF", T::Input}, {"7", "VOUT", T::Output}, {"8", "+VS", T::PowerIn}});
    catalog(parts, "Op-Amps", "TLV1701AIDBVR", "Texas Instruments",
            "36 V micropower comparator, open-collector output",
            "U", "SOT23", 5, 0, 0, "SOT-23-5 (DBV)",
            {{"1", "IN+", T::Input}, {"2", "V-", T::PowerIn}, {"3", "IN-", T::Input}, {"4", "OUT", T::OpenCollector},
            {"5", "V+", T::PowerIn}});
    catalog(parts, "Op-Amps", "TLV3201AIDBVR", "Texas Instruments",
            "40 ns, push-pull rail-to-rail comparator, 1.8 V to 5.5 V",
            "U", "SOT23", 5, 0, 0, "SOT-23-5 (DBV)",
            {{"1", "OUT", T::Output}, {"2", "GND", T::PowerIn}, {"3", "IN+", T::Input}, {"4", "IN-", T::Input},
            {"5", "VCC", T::PowerIn}});
    catalog(parts, "Op-Amps", "OPA380AIDGKR", "Texas Instruments",
            "Precision, high-speed transimpedance amplifier (90 MHz GBW)",
            "U", "TSSOP", 8, 0.65, 3, "VSSOP-8 (DGK)",
            {{"1", "NC", T::NoConnect}, {"2", "-IN", T::Input}, {"3", "+IN", T::Input}, {"4", "V-", T::PowerIn},
            {"5", "NC", T::NoConnect}, {"6", "OUT", T::Output}, {"7", "V+", T::PowerIn}, {"8", "NC", T::NoConnect}});
    catalog(parts, "Data Converters", "DAC80501MDGSR", "Texas Instruments",
            "16-bit voltage-output DAC with 2.5 V reference, SPI / I2C",
            "U", "TSSOP", 10, 0.5, 3, "VSSOP-10 (DGS)",
            {{"1", "VDD", T::PowerIn}, {"2", "VOUT", T::Output}, {"3", "NC", T::NoConnect}, {"4", "AGND", T::PowerIn},
            {"5", "SPI2C", T::Input}, {"6", "SCLK/SCL", T::Input}, {"7", "SYNC/A0", T::Input}, {"8", "SDIN/SDA", T::Bidirectional},
            {"9", "NC", T::NoConnect}, {"10", "VREFIO", T::Bidirectional}});
    catalog(parts, "Regulators", "TPS61291DRVR", "Texas Instruments",
            "Boost converter with bypass mode, 15 µA Iq",
            "U", "SON", 6, 0.65, 2, "WSON-6 2×2 (DRV)",
            {{"1", "SW", T::Passive}, {"2", "VOUT", T::PowerOut}, {"3", "VIN", T::PowerIn}, {"4", "EN/BYP", T::Input},
            {"5", "VSEL", T::Input}, {"6", "GND", T::PowerIn}, {"EP", "GND", T::PowerIn}});
    catalog(parts, "Power", "TPS22918DBVR", "Texas Instruments",
            "5.5 V, 2 A, 52 mΩ load switch with adjustable rise time and quick output discharge",
            "U", "SOT23", 6, 0, 0, "SOT-23-6 (DBV)",
            {{"1", "VIN", T::PowerIn}, {"2", "GND", T::PowerIn}, {"3", "ON", T::Input}, {"4", "CT", T::Passive},
            {"5", "QOD", T::Passive}, {"6", "VOUT", T::PowerOut}});
    catalog(parts, "Power Electronics", "UCC28180D", "Texas Instruments",
            "8-pin continuous-conduction-mode PFC controller",
            "U", "SOIC", 8, 1.27, 3.9, "SOIC-8 (D)",
            {{"1", "GND", T::PowerIn}, {"2", "ICOMP", T::Passive}, {"3", "ISENSE", T::Input}, {"4", "FREQ", T::Passive},
            {"5", "VCOMP", T::Passive}, {"6", "VSENSE", T::Input}, {"7", "VCC", T::PowerIn}, {"8", "GATE", T::Output}});
    catalog(parts, "Interface", "SN65HVD3082EDR", "Texas Instruments",
            "5 V half-duplex RS-485 transceiver, 200 kbps, 1/8 unit load",
            "U", "SOIC", 8, 1.27, 3.9, "SOIC-8 (D)",
            {{"1", "R", T::Output}, {"2", "nRE", T::Input}, {"3", "DE", T::Input}, {"4", "D", T::Input},
            {"5", "GND", T::PowerIn}, {"6", "A", T::Bidirectional}, {"7", "B", T::Bidirectional}, {"8", "VCC", T::PowerIn}});
    catalog(parts, "Isolation", "ISO7741DWR", "Texas Instruments",
            "Quad-channel digital isolator, 3 forward / 1 reverse, 5 kVrms",
            "U", "SOIC", 16, 1.27, 7.5, "SOIC-16W (DW)",
            {{"1", "VCC1", T::PowerIn}, {"2", "GND1", T::PowerIn}, {"3", "INA", T::Input}, {"4", "INB", T::Input},
            {"5", "INC", T::Input}, {"6", "OUTD", T::Output}, {"7", "EN1", T::Input}, {"8", "GND1", T::PowerIn},
            {"9", "GND2", T::PowerIn}, {"10", "EN2", T::Input}, {"11", "IND", T::Input}, {"12", "OUTC", T::Output},
            {"13", "OUTB", T::Output}, {"14", "OUTA", T::Output}, {"15", "GND2", T::PowerIn}, {"16", "VCC2", T::PowerIn}});
    catalog(parts, "Data Converters", "TDC7200PWR", "Texas Instruments",
            "Time-to-digital converter for time-of-flight (55 ps resolution), SPI",
            "U", "TSSOP", 14, 0.65, 4.4, "TSSOP-14 (PW)",
            {{"1", "ENABLE", T::Input}, {"2", "TRIGG", T::Output}, {"3", "START", T::Input}, {"4", "STOP", T::Input},
            {"5", "CLOCK", T::Input}, {"6", "NC", T::NoConnect}, {"7", "GND", T::PowerIn}, {"8", "INTB", T::OpenCollector},
            {"9", "DOUT", T::Output}, {"10", "DIN", T::Input}, {"11", "CSB", T::Input}, {"12", "SCLK", T::Input},
            {"13", "VREG", T::PowerOut}, {"14", "VDD", T::PowerIn}});
    catalog(parts, "Sensors", "LMP91000SDX/NOPB", "Texas Instruments",
            "Configurable potentiostat AFE for electrochemical gas sensors, I2C",
            "U", "SON", 14, 0.5, 4, "WSON-14 4×4 (NHL)",
            {{"1", "DGND", T::PowerIn}, {"2", "MENB", T::Input}, {"3", "SCL", T::Input}, {"4", "SDA", T::Bidirectional},
            {"5", "NC", T::NoConnect}, {"6", "VDD", T::PowerIn}, {"7", "AGND", T::PowerIn}, {"8", "VOUT", T::Output},
            {"9", "C2", T::Passive}, {"10", "C1", T::Passive}, {"11", "VREF", T::Input}, {"12", "WE", T::Passive},
            {"13", "RE", T::Input}, {"14", "CE", T::Output}, {"EP", "AGND", T::PowerIn}});
    catalog(parts, "Op-Amps", "PGA280AIPW", "Texas Instruments",
            "Zero-drift, high-voltage programmable-gain instrumentation amplifier, SPI",
            "U", "TSSOP", 24, 0.65, 4.4, "TSSOP-24 (PW)",
            {{"1", "VON", T::Output}, {"2", "VOP", T::Output}, {"3", "VOCM", T::Bidirectional}, {"4", "VSOP", T::PowerIn},
            {"5", "VSON", T::PowerIn}, {"6", "VSP", T::PowerIn}, {"7", "INP2", T::Input}, {"8", "INN2", T::Input},
            {"9", "INP1", T::Input}, {"10", "INN1", T::Input}, {"11", "VSN", T::PowerIn}, {"12", "DGND", T::PowerIn},
            {"13", "DVDD", T::PowerIn}, {"14", "SDO", T::Output}, {"15", "SDI", T::Input}, {"16", "SCLK", T::Input},
            {"17", "nCS", T::Input}, {"18", "GPIO6", T::Bidirectional}, {"19", "GPIO5", T::Bidirectional}, {"20", "GPIO4", T::Bidirectional},
            {"21", "GPIO3", T::Bidirectional}, {"22", "GPIO2", T::Bidirectional}, {"23", "GPIO1", T::Bidirectional}, {"24", "GPIO0", T::Bidirectional}});
    catalog(parts, "Data Converters", "ADS124S08IPBS", "Texas Instruments",
            "24-bit 4 kSPS 12-channel delta-sigma ADC with PGA and reference, SPI",
            "U", "LQFP", 32, 0.5, 5, "TQFP-32 5×5 (PBS)",
            {{"1", "AINCOM", T::Input}, {"2", "AIN5", T::Input}, {"3", "AIN4", T::Input}, {"4", "AIN3", T::Input},
            {"5", "AIN2", T::Input}, {"6", "AIN1", T::Input}, {"7", "AIN0", T::Input}, {"8", "START/SYNC", T::Input},
            {"9", "nCS", T::Input}, {"10", "DIN", T::Input}, {"11", "SCLK", T::Input}, {"12", "DOUT/nDRDY", T::Output},
            {"13", "nDRDY", T::Output}, {"14", "DGND", T::PowerIn}, {"15", "IOVDD", T::PowerIn}, {"16", "DVDD", T::PowerIn},
            {"17", "CLK", T::Input}, {"18", "nRESET", T::Input}, {"19", "GPIO3/AIN11", T::Bidirectional}, {"20", "GPIO2/AIN10", T::Bidirectional},
            {"21", "GPIO1/AIN9", T::Bidirectional}, {"22", "GPIO0/AIN8", T::Bidirectional}, {"23", "REFOUT", T::PowerOut}, {"24", "REFCOM", T::PowerOut},
            {"25", "NC", T::NoConnect}, {"26", "AVDD", T::PowerIn}, {"27", "AVSS", T::PowerIn}, {"28", "AVSS-SW", T::PowerIn},
            {"29", "REFN0", T::Input}, {"30", "REFP0", T::Input}, {"31", "REFN1/AIN7", T::Input}, {"32", "REFP1/AIN6", T::Input}});
    catalog(parts, "Medical", "ADS1292RIPBS", "Texas Instruments",
            "2-channel 24-bit ECG front end with respiration measurement, SPI",
            "U", "LQFP", 32, 0.5, 5, "TQFP-32 5×5 (PBS)",
            {{"1", "PGA1N", T::Output}, {"2", "PGA1P", T::Output}, {"3", "IN1N", T::Input}, {"4", "IN1P", T::Input},
            {"5", "IN2N", T::Input}, {"6", "IN2P", T::Input}, {"7", "PGA2N", T::Output}, {"8", "PGA2P", T::Output},
            {"9", "VREFP", T::Bidirectional}, {"10", "VREFN", T::Input}, {"11", "VCAP1", T::Passive}, {"12", "AVDD", T::PowerIn},
            {"13", "AVSS", T::PowerIn}, {"14", "CLKSEL", T::Input}, {"15", "nPWDN/nRESET", T::Input}, {"16", "START", T::Input},
            {"17", "CLK", T::Input}, {"18", "nCS", T::Input}, {"19", "DIN", T::Input}, {"20", "SCLK", T::Input},
            {"21", "DOUT", T::Output}, {"22", "nDRDY", T::Output}, {"23", "DVDD", T::PowerIn}, {"24", "DGND", T::PowerIn},
            {"25", "GPIO2/RCLK2", T::Bidirectional}, {"26", "GPIO1/RCLK1", T::Bidirectional}, {"27", "VCAP2", T::Passive}, {"28", "RLDINV", T::Input},
            {"29", "RLDIN/RLDREF", T::Input}, {"30", "RLDOUT", T::Output}, {"31", "RESP_MODP/IN3P", T::Bidirectional}, {"32", "RESP_MODN/IN3N", T::Bidirectional}});
    catalog(parts, "Drivers", "LP5024RSMR", "Texas Instruments",
            "24-channel I2C constant-current RGB LED driver (8 RGB LEDs), 12-bit PWM",
            "U", "QFN", 32, 0.4, 4, "VQFN-32 4×4 (RSM)",
            {{"1", "OUT0", T::Output}, {"2", "OUT1", T::Output}, {"3", "OUT2", T::Output}, {"4", "OUT3", T::Output},
            {"5", "OUT4", T::Output}, {"6", "OUT5", T::Output}, {"7", "OUT6", T::Output}, {"8", "OUT7", T::Output},
            {"9", "OUT8", T::Output}, {"10", "OUT9", T::Output}, {"11", "OUT10", T::Output}, {"12", "OUT11", T::Output},
            {"13", "OUT12", T::Output}, {"14", "OUT13", T::Output}, {"15", "OUT14", T::Output}, {"16", "OUT15", T::Output},
            {"17", "OUT16", T::Output}, {"18", "OUT17", T::Output}, {"19", "OUT18", T::Output}, {"20", "OUT19", T::Output},
            {"21", "OUT20", T::Output}, {"22", "OUT21", T::Output}, {"23", "OUT22", T::Output}, {"24", "OUT23", T::Output},
            {"25", "ADDR0", T::Input}, {"26", "ADDR1", T::Input}, {"27", "VCC", T::PowerIn}, {"28", "SDA", T::Bidirectional},
            {"29", "SCL", T::Input}, {"30", "EN", T::Input}, {"31", "IREF", T::Passive}, {"32", "VCAP", T::PowerOut},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Power", "TPS6521901RHBR", "Texas Instruments",
            "PMIC for Arm Cortex-A53 processors: 3 bucks, 4 LDOs, I2C, sequencing",
            "U", "QFN", 32, 0.5, 5, "VQFN-32 5×5 (RHB)",
            {{"1", "FB_B1", T::Input}, {"2", "LX_B1_1", T::Passive}, {"3", "LX_B1_2", T::Passive}, {"4", "PVIN_B1_1", T::PowerIn},
            {"5", "PVIN_B1_2", T::PowerIn}, {"6", "PVIN_LDO1", T::PowerIn}, {"7", "VLDO1", T::PowerOut}, {"8", "GPO1", T::OpenCollector},
            {"9", "SDA", T::Bidirectional}, {"10", "SCL", T::Input}, {"11", "nINT", T::OpenCollector}, {"12", "VSEL_SD/VSEL_DDR", T::Input},
            {"13", "VSYS", T::PowerIn}, {"14", "VDD1P8", T::PowerOut}, {"15", "AGND", T::PowerIn}, {"16", "GPIO", T::Bidirectional},
            {"17", "GPO2", T::OpenCollector}, {"18", "nRSTOUT", T::Output}, {"19", "VLDO2", T::PowerOut}, {"20", "PVIN_LDO2", T::PowerIn},
            {"21", "VLDO3", T::PowerOut}, {"22", "PVIN_LDO34", T::PowerIn}, {"23", "VLDO4", T::PowerOut}, {"24", "FB_B3", T::Input},
            {"25", "EN/PB/VSENSE", T::Input}, {"26", "PVIN_B3", T::PowerIn}, {"27", "LX_B3", T::Passive}, {"28", "MODE/RESET", T::Input},
            {"29", "LX_B2", T::Passive}, {"30", "PVIN_B2", T::PowerIn}, {"31", "MODE/STBY", T::Input}, {"32", "FB_B2", T::Input},
            {"EP", "PGND", T::PowerIn}});
    catalog(parts, "Audio", "TLV320AIC3204IRHBR", "Texas Instruments",
            "Very-low-power stereo audio codec with miniDSP, headphone and line outputs",
            "U", "QFN", 32, 0.5, 5, "VQFN-32 5×5 (RHB)",
            {{"1", "MCLK", T::Input}, {"2", "BCLK", T::Bidirectional}, {"3", "WCLK", T::Bidirectional}, {"4", "DIN/MFP1", T::Input},
            {"5", "DOUT/MFP2", T::Output}, {"6", "IOVDD", T::PowerIn}, {"7", "IOVSS", T::PowerIn}, {"8", "SCLK/MFP3", T::Input},
            {"9", "SCL/SS", T::Input}, {"10", "SDA/MOSI", T::Bidirectional}, {"11", "MISO/MFP4", T::Output}, {"12", "SPI_SELECT", T::Input},
            {"13", "IN1_L", T::Input}, {"14", "IN1_R", T::Input}, {"15", "IN2_L", T::Input}, {"16", "IN2_R", T::Input},
            {"17", "AVSS", T::PowerIn}, {"18", "REF", T::Output}, {"19", "MICBIAS", T::PowerOut}, {"20", "IN3_L", T::Input},
            {"21", "IN3_R", T::Input}, {"22", "LOL", T::Output}, {"23", "LOR", T::Output}, {"24", "AVDD", T::PowerIn},
            {"25", "HPL", T::Output}, {"26", "LDOIN/HPVDD", T::PowerIn}, {"27", "HPR", T::Output}, {"28", "DVSS", T::PowerIn},
            {"29", "DVDD", T::PowerIn}, {"30", "LDO_SELECT", T::Input}, {"31", "nRESET", T::Input}, {"32", "GPIO/MFP5", T::Bidirectional},
            {"EP", "EP", T::Passive}});
    catalog(parts, "Timing", "CDCE6214RGET", "Texas Instruments",
            "Ultra-low-power clock generator: 2 inputs, 4 differential + 1 LVCMOS outputs, EEPROM",
            "U", "QFN", 24, 0.5, 4, "VQFN-24 4×4 (RGE)",
            {{"1", "SECREF_P", T::Input}, {"2", "SECREF_N", T::Input}, {"3", "VDD_REF", T::PowerIn}, {"4", "REFSEL", T::Input},
            {"5", "PRIREF_P", T::Input}, {"6", "PRIREF_N", T::Input}, {"7", "OUT0", T::Output}, {"8", "PDN", T::Input},
            {"9", "OUT4_N", T::Output}, {"10", "OUT4_P", T::Output}, {"11", "GPIO4", T::Bidirectional}, {"12", "SCL/GPIO3", T::Input},
            {"13", "OUT3_N", T::Output}, {"14", "OUT3_P", T::Output}, {"15", "VDDO_34", T::PowerIn}, {"16", "VDDO_12", T::PowerIn},
            {"17", "OUT2_N", T::Output}, {"18", "OUT2_P", T::Output}, {"19", "SDA/GPIO2", T::Bidirectional}, {"20", "GPIO1", T::Bidirectional},
            {"21", "OUT1_N", T::Output}, {"22", "OUT1_P", T::Output}, {"23", "HW_SW_CTRL", T::Input}, {"24", "VDD_VCO", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Audio", "TPA3255DDV", "Texas Instruments",
            "315 W stereo / 600 W mono class-D amplifier, analog input, PowerPAD up",
            "U", "TSSOP", 44, 0.635, 6.1, "HTSSOP-44 PowerPAD up (DDV)",
            {{"1", "GVDD_AB", T::PowerOut}, {"2", "VDD", T::PowerIn}, {"3", "M1", T::Input}, {"4", "M2", T::Input},
            {"5", "INPUT_A", T::Input}, {"6", "INPUT_B", T::Input}, {"7", "OC_ADJ", T::Passive}, {"8", "FREQ_ADJ", T::Passive},
            {"9", "OSC_IOM", T::Bidirectional}, {"10", "OSC_IOP", T::Bidirectional}, {"11", "DVDD", T::PowerOut}, {"12", "GND", T::PowerIn},
            {"13", "GND", T::PowerIn}, {"14", "AVDD", T::PowerOut}, {"15", "C_START", T::Passive}, {"16", "INPUT_C", T::Input},
            {"17", "INPUT_D", T::Input}, {"18", "nRESET", T::Input}, {"19", "nFAULT", T::OpenCollector}, {"20", "VBG", T::PowerOut},
            {"21", "nCLIP_OTW", T::OpenCollector}, {"22", "GVDD_CD", T::PowerOut}, {"23", "BST_D", T::Passive}, {"24", "BST_C", T::Passive},
            {"25", "GND", T::PowerIn}, {"26", "GND", T::PowerIn}, {"27", "OUT_D", T::Output}, {"28", "OUT_D", T::Output},
            {"29", "PVDD_CD", T::PowerIn}, {"30", "PVDD_CD", T::PowerIn}, {"31", "PVDD_CD", T::PowerIn}, {"32", "OUT_C", T::Output},
            {"33", "GND", T::PowerIn}, {"34", "GND", T::PowerIn}, {"35", "OUT_B", T::Output}, {"36", "PVDD_AB", T::PowerIn},
            {"37", "PVDD_AB", T::PowerIn}, {"38", "PVDD_AB", T::PowerIn}, {"39", "OUT_A", T::Output}, {"40", "OUT_A", T::Output},
            {"41", "GND", T::PowerIn}, {"42", "GND", T::PowerIn}, {"43", "BST_B", T::Passive}, {"44", "BST_A", T::Passive}});
    catalog(parts, "Audio", "TPA3116D2DAD", "Texas Instruments",
            "50 W stereo / 100 W mono class-D amplifier, analog input, PowerPAD up",
            "U", "TSSOP", 32, 0.65, 6.2, "HTSSOP-32 PowerPAD up (DAD)",
            {{"1", "MODSEL", T::Input}, {"2", "SDZ", T::Input}, {"3", "FAULTZ", T::OpenCollector}, {"4", "RINP", T::Input},
            {"5", "RINN", T::Input}, {"6", "PLIMIT", T::Input}, {"7", "GVDD", T::PowerOut}, {"8", "GAIN/SLV", T::Input},
            {"9", "GND", T::PowerIn}, {"10", "LINP", T::Input}, {"11", "LINN", T::Input}, {"12", "MUTE", T::Input},
            {"13", "AM2", T::Input}, {"14", "AM1", T::Input}, {"15", "AM0", T::Input}, {"16", "SYNC", T::Bidirectional},
            {"17", "AVCC", T::PowerIn}, {"18", "PVCC", T::PowerIn}, {"19", "PVCC", T::PowerIn}, {"20", "BSNL", T::Passive},
            {"21", "OUTNL", T::Output}, {"22", "GND", T::PowerIn}, {"23", "OUTPL", T::Output}, {"24", "BSPL", T::Passive},
            {"25", "GND", T::PowerIn}, {"26", "BSNR", T::Passive}, {"27", "OUTNR", T::Output}, {"28", "GND", T::PowerIn},
            {"29", "OUTPR", T::Output}, {"30", "BSPR", T::Passive}, {"31", "PVCC", T::PowerIn}, {"32", "PVCC", T::PowerIn}});
    catalog(parts, "RF", "LMX2594RHAR", "Texas Instruments",
            "15 GHz wideband PLL synthesizer with phase synchronization and JESD204B SYSREF",
            "U", "QFN", 40, 0.5, 6, "VQFN-40 6×6 (RHA)",
            {{"1", "CE", T::Input}, {"2", "GND", T::PowerIn}, {"3", "VbiasVCO", T::Passive}, {"4", "GND", T::PowerIn},
            {"5", "SYNC", T::Input}, {"6", "GND", T::PowerIn}, {"7", "VccDIG", T::PowerIn}, {"8", "OSCinP", T::Input},
            {"9", "OSCinM", T::Input}, {"10", "VregIN", T::Passive}, {"11", "VccCP", T::PowerIn}, {"12", "CPout", T::Output},
            {"13", "GND", T::PowerIn}, {"14", "GND", T::PowerIn}, {"15", "VccMASH", T::PowerIn}, {"16", "SCK", T::Input},
            {"17", "SDI", T::Input}, {"18", "RFoutBM", T::Output}, {"19", "RFoutBP", T::Output}, {"20", "MUXout", T::Output},
            {"21", "VccBUF", T::PowerIn}, {"22", "RFoutAM", T::Output}, {"23", "RFoutAP", T::Output}, {"24", "CSB", T::Input},
            {"25", "GND", T::PowerIn}, {"26", "VccVCO2", T::PowerIn}, {"27", "VbiasVCO2", T::Passive}, {"28", "SysRefReq", T::Input},
            {"29", "VrefVCO2", T::Passive}, {"30", "RampClk", T::Input}, {"31", "GND", T::PowerIn}, {"32", "RampDir", T::Input},
            {"33", "VbiasVARAC", T::Passive}, {"34", "GND", T::PowerIn}, {"35", "Vtune", T::Input}, {"36", "VrefVCO", T::Passive},
            {"37", "VccVCO", T::PowerIn}, {"38", "VregVCO", T::Passive}, {"39", "GND", T::PowerIn}, {"40", "GND", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Timing", "LMK04828BISQ/NOPB", "Texas Instruments",
            "JESD204B dual-loop jitter cleaner: 14 clock outputs (7 device clock / SYSREF pairs), 3 inputs",
            "U", "QFN", 64, 0.5, 9, "WQFN-64 9×9 (NKD)",
            {{"1", "DCLKout0", T::Output}, {"2", "DCLKout0*", T::Output}, {"3", "SDCLKout1", T::Output}, {"4", "SDCLKout1*", T::Output},
            {"5", "RESET/GPO", T::Bidirectional}, {"6", "SYNC/SYSREF_REQ", T::Input}, {"7", "NC", T::NoConnect}, {"8", "NC", T::NoConnect},
            {"9", "NC", T::NoConnect}, {"10", "Vcc1_VCO", T::PowerIn}, {"11", "LDObyp1", T::Passive}, {"12", "LDObyp2", T::Passive},
            {"13", "SDCLKout3", T::Output}, {"14", "SDCLKout3*", T::Output}, {"15", "DCLKout2", T::Output}, {"16", "DCLKout2*", T::Output},
            {"17", "Vcc2_CG1", T::PowerIn}, {"18", "CS*", T::Input}, {"19", "SCK", T::Input}, {"20", "SDIO", T::Bidirectional},
            {"21", "Vcc3_SYSREF", T::PowerIn}, {"22", "SDCLKout5", T::Output}, {"23", "SDCLKout5*", T::Output}, {"24", "DCLKout4", T::Output},
            {"25", "DCLKout4*", T::Output}, {"26", "Vcc4_CG2", T::PowerIn}, {"27", "DCLKout6", T::Output}, {"28", "DCLKout6*", T::Output},
            {"29", "SDCLKout7", T::Output}, {"30", "SDCLKout7*", T::Output}, {"31", "Status_LD1", T::Bidirectional}, {"32", "CPout1", T::Output},
            {"33", "Vcc5_DIG", T::PowerIn}, {"34", "CLKin1/Fin/FBCLKin", T::Input}, {"35", "CLKin1*/Fin*/FBCLKin*", T::Input}, {"36", "Vcc6_PLL1", T::PowerIn},
            {"37", "CLKin0", T::Input}, {"38", "CLKin0*", T::Input}, {"39", "Vcc7_OSCout", T::PowerIn}, {"40", "OSCout/CLKin2", T::Bidirectional},
            {"41", "OSCout*/CLKin2*", T::Bidirectional}, {"42", "Vcc8_OSCin", T::PowerIn}, {"43", "OSCin", T::Input}, {"44", "OSCin*", T::Input},
            {"45", "Vcc9_CP2", T::PowerIn}, {"46", "CPout2", T::Output}, {"47", "Vcc10_PLL2", T::PowerIn}, {"48", "Status_LD2", T::Bidirectional},
            {"49", "SDCLKout9", T::Output}, {"50", "SDCLKout9*", T::Output}, {"51", "DCLKout8", T::Output}, {"52", "DCLKout8*", T::Output},
            {"53", "Vcc11_CG3", T::PowerIn}, {"54", "DCLKout10", T::Output}, {"55", "DCLKout10*", T::Output}, {"56", "SDCLKout11", T::Output},
            {"57", "SDCLKout11*", T::Output}, {"58", "CLKin_SEL0", T::Bidirectional}, {"59", "CLKin_SEL1", T::Bidirectional}, {"60", "SDCLKout13", T::Output},
            {"61", "SDCLKout13*", T::Output}, {"62", "DCLKout12", T::Output}, {"63", "DCLKout12*", T::Output}, {"64", "Vcc12_CG0", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Interface", "DP83867IRRGZR", "Texas Instruments",
            "Gigabit Ethernet PHY (10/100/1000BASE-T), RGMII, industrial temperature",
            "U", "QFN", 48, 0.5, 7, "VQFN-48 7×7 (RGZ)",
            {{"1", "TD_P_A", T::Bidirectional}, {"2", "TD_M_A", T::Bidirectional}, {"3", "VDDA2P5", T::PowerIn}, {"4", "TD_P_B", T::Bidirectional},
            {"5", "TD_M_B", T::Bidirectional}, {"6", "VDD1P0", T::PowerIn}, {"7", "TD_P_C", T::Bidirectional}, {"8", "TD_M_C", T::Bidirectional},
            {"9", "VDDA2P5", T::PowerIn}, {"10", "TD_P_D", T::Bidirectional}, {"11", "TD_M_D", T::Bidirectional}, {"12", "RBIAS", T::Passive},
            {"13", "VDDA1P8", T::PowerIn}, {"14", "XO", T::Output}, {"15", "XI", T::Input}, {"16", "MDC", T::Input},
            {"17", "MDIO", T::Bidirectional}, {"18", "CLK_OUT", T::Output}, {"19", "VDDIO", T::PowerIn}, {"20", "JTAG_CLK", T::Input},
            {"21", "JTAG_TDO", T::Output}, {"22", "JTAG_TMS", T::Input}, {"23", "JTAG_TDI", T::Input}, {"24", "VDD1P0", T::PowerIn},
            {"25", "TX_D3", T::Input}, {"26", "TX_D2", T::Input}, {"27", "TX_D1", T::Input}, {"28", "TX_D0", T::Input},
            {"29", "GTX_CLK", T::Input}, {"30", "VDDIO", T::PowerIn}, {"31", "VDD1P0", T::PowerIn}, {"32", "RX_CLK", T::Output},
            {"33", "RX_D0", T::Output}, {"34", "RX_D1", T::Output}, {"35", "RX_D2", T::Output}, {"36", "RX_D3", T::Output},
            {"37", "TX_EN/TX_CTRL", T::Input}, {"38", "RX_DV/RX_CTRL", T::Output}, {"39", "GPIO_0", T::Bidirectional}, {"40", "GPIO_1", T::Bidirectional},
            {"41", "VDDIO", T::PowerIn}, {"42", "VDD1P0", T::PowerIn}, {"43", "nRESET", T::Input}, {"44", "nINT/nPWDN", T::Bidirectional},
            {"45", "LED_2", T::Bidirectional}, {"46", "LED_1", T::Bidirectional}, {"47", "LED_0", T::Bidirectional}, {"48", "VDDA1P8", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Drivers", "DRV8323SRTAR", "Texas Instruments",
            "6–60 V three-phase smart gate driver with 3 current-sense amplifiers, SPI (no buck regulator)",
            "U", "QFN", 40, 0.5, 6, "WQFN-40 6×6 (RTA)",
            {{"1", "CPL", T::Passive}, {"2", "CPH", T::Passive}, {"3", "VCP", T::PowerOut}, {"4", "VM", T::PowerIn},
            {"5", "VDRAIN", T::Input}, {"6", "GHA", T::Output}, {"7", "SHA", T::Bidirectional}, {"8", "GLA", T::Output},
            {"9", "SPA", T::Input}, {"10", "SNA", T::Input}, {"11", "SNB", T::Input}, {"12", "SPB", T::Input},
            {"13", "GLB", T::Output}, {"14", "SHB", T::Bidirectional}, {"15", "GHB", T::Output}, {"16", "GHC", T::Output},
            {"17", "SHC", T::Bidirectional}, {"18", "GLC", T::Output}, {"19", "SPC", T::Input}, {"20", "SNC", T::Input},
            {"21", "SOC", T::Output}, {"22", "SOB", T::Output}, {"23", "SOA", T::Output}, {"24", "VREF", T::PowerIn},
            {"25", "nFAULT", T::OpenCollector}, {"26", "SDO", T::OpenCollector}, {"27", "SDI", T::Input}, {"28", "SCLK", T::Input},
            {"29", "nSCS", T::Input}, {"30", "ENABLE", T::Input}, {"31", "CAL", T::Input}, {"32", "AGND", T::PowerIn},
            {"33", "DVDD", T::PowerOut}, {"34", "INHA", T::Input}, {"35", "INLA", T::Input}, {"36", "INHB", T::Input},
            {"37", "INLB", T::Input}, {"38", "INHC", T::Input}, {"39", "INLC", T::Input}, {"40", "PGND", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Power", "BQ79616PAPR", "Texas Instruments",
            "16-series battery monitor, balancer and protector with daisy-chain interface",
            "U", "LQFP", 64, 0.5, 10, "HTQFP-64 10×10 with exposed pad (PAP)",
            {{"1", "BAT", T::PowerIn}, {"2", "CB16", T::Bidirectional}, {"3", "VC16", T::Input}, {"4", "CB15", T::Bidirectional},
            {"5", "VC15", T::Input}, {"6", "CB14", T::Bidirectional}, {"7", "VC14", T::Input}, {"8", "CB13", T::Bidirectional},
            {"9", "VC13", T::Input}, {"10", "CB12", T::Bidirectional}, {"11", "VC12", T::Input}, {"12", "CB11", T::Bidirectional},
            {"13", "VC11", T::Input}, {"14", "CB10", T::Bidirectional}, {"15", "VC10", T::Input}, {"16", "CB9", T::Bidirectional},
            {"17", "VC9", T::Input}, {"18", "CB8", T::Bidirectional}, {"19", "VC8", T::Input}, {"20", "CB7", T::Bidirectional},
            {"21", "VC7", T::Input}, {"22", "CB6", T::Bidirectional}, {"23", "VC6", T::Input}, {"24", "CB5", T::Bidirectional},
            {"25", "VC5", T::Input}, {"26", "CB4", T::Bidirectional}, {"27", "VC4", T::Input}, {"28", "CB3", T::Bidirectional},
            {"29", "VC3", T::Input}, {"30", "CB2", T::Bidirectional}, {"31", "VC2", T::Input}, {"32", "CB1", T::Bidirectional},
            {"33", "VC1", T::Input}, {"34", "CB0", T::Bidirectional}, {"35", "VC0", T::Input}, {"36", "REFHM", T::PowerIn},
            {"37", "REFHP", T::PowerOut}, {"38", "AVDD", T::PowerOut}, {"39", "AVSS", T::PowerIn}, {"40", "COMLP", T::Bidirectional},
            {"41", "COMLN", T::Bidirectional}, {"42", "COMHN", T::Bidirectional}, {"43", "COMHP", T::Bidirectional}, {"44", "NEG5V", T::PowerOut},
            {"45", "CVDD", T::PowerOut}, {"46", "CVSS", T::PowerIn}, {"47", "LDOIN", T::PowerIn}, {"48", "NPN", T::Output},
            {"49", "DVDD", T::PowerOut}, {"50", "DVSS", T::PowerIn}, {"51", "TSREF", T::PowerOut}, {"52", "RX", T::Input},
            {"53", "TX", T::Output}, {"54", "GPIO8", T::Bidirectional}, {"55", "GPIO7", T::Bidirectional}, {"56", "GPIO6", T::Bidirectional},
            {"57", "GPIO5", T::Bidirectional}, {"58", "GPIO4", T::Bidirectional}, {"59", "GPIO3", T::Bidirectional}, {"60", "GPIO2", T::Bidirectional},
            {"61", "GPIO1", T::Bidirectional}, {"62", "nFAULT", T::Output}, {"63", "BBN", T::Input}, {"64", "BBP", T::Input},
            {"EP", "EP", T::Passive}});
    catalog(parts, "Interface", "SN65DSI86ZXHR", "Texas Instruments",
            "Dual-channel MIPI DSI to embedded DisplayPort (eDP) bridge, 4 lanes up to 5.4 Gbps",
            "U", "BGA", 64, 0.5, 5, "nFBGA-64 5×5 0.5 mm (ZXH)",
            {{"A1", "ADDR", T::Bidirectional}, {"H8", "AUXP", T::Bidirectional}, {"H9", "AUXN", T::Bidirectional}, {"H3", "DA0P", T::Bidirectional},
            {"J3", "DA0N", T::Bidirectional}, {"H4", "DA1P", T::Input}, {"J4", "DA1N", T::Input}, {"H6", "DA2P", T::Input},
            {"J6", "DA2N", T::Input}, {"H7", "DA3P", T::Input}, {"J7", "DA3N", T::Input}, {"H5", "DACP", T::Input},
            {"J5", "DACN", T::Input}, {"C2", "DB0P", T::Input}, {"C1", "DB0N", T::Input}, {"D2", "DB1P", T::Input},
            {"D1", "DB1N", T::Input}, {"F2", "DB2P", T::Input}, {"F1", "DB2N", T::Input}, {"G2", "DB3P", T::Input},
            {"G1", "DB3N", T::Input}, {"E2", "DBCP", T::Input}, {"E1", "DBCN", T::Input}, {"F8", "ML0P", T::Output},
            {"F9", "ML0N", T::Output}, {"E8", "ML1P", T::Output}, {"E9", "ML1N", T::Output}, {"C8", "ML2P", T::Output},
            {"C9", "ML2N", T::Output}, {"B8", "ML3P", T::Output}, {"B9", "ML3N", T::Output}, {"B1", "EN", T::Input},
            {"J8", "HPD", T::Input}, {"A3", "IRQ", T::Output}, {"A7", "REFCLK", T::Input}, {"H1", "SCL", T::Bidirectional},
            {"J1", "SDA", T::Bidirectional}, {"B3", "TEST1", T::Input}, {"B5", "TEST2", T::Bidirectional}, {"B7", "TEST3", T::Passive},
            {"A4", "GPIO1", T::Bidirectional}, {"A5", "GPIO2", T::Bidirectional}, {"A6", "GPIO3", T::Bidirectional}, {"B4", "GPIO4", T::Bidirectional},
            {"D9", "VPLL", T::PowerIn}, {"B6", "VCCIO", T::PowerIn}, {"A2", "VCCIO", T::PowerIn}, {"A8", "GND", T::PowerIn},
            {"D8", "GND", T::PowerIn}, {"E4", "GND", T::PowerIn}, {"E5", "GND", T::PowerIn}, {"F4", "GND", T::PowerIn},
            {"F5", "GND", T::PowerIn}, {"F6", "GND", T::PowerIn}, {"G8", "GND", T::PowerIn}, {"D6", "VCC", T::PowerIn},
            {"D5", "VCC", T::PowerIn}, {"J2", "VCC", T::PowerIn}, {"J9", "VCC", T::PowerIn}, {"A9", "VCCA", T::PowerIn},
            {"G9", "VCCA", T::PowerIn}, {"E6", "VCCA", T::PowerIn}, {"B2", "VCCA", T::PowerIn}, {"H2", "VCCA", T::PowerIn}});
    catalog(parts, "Memory", "24AA02E48T-I/OT", "Microchip",
            "2 Kbit I2C serial EEPROM with a factory-programmed EUI-48 node address",
            "U", "SOT23", 5, 0, 0, "SOT-23-5 (OT)",
            {{"1", "SCL", T::Input}, {"2", "VSS", T::PowerIn}, {"3", "SDA", T::Bidirectional}, {"4", "VCC", T::PowerIn},
            {"5", "NC", T::NoConnect}});
    catalog(parts, "Sensors", "CAP1298-1-A4-TR", "Microchip",
            "8-channel capacitive touch sensor controller with proximity detection and signal guard, SMBus / I2C",
            "U", "QFN", 16, 0.5, 3, "QFN-16 3×3 (A4)",
            {{"1", "nALERT", T::OpenCollector}, {"2", "SMDATA", T::Bidirectional}, {"3", "SMCLK", T::Input}, {"4", "VDD", T::PowerIn},
            {"5", "NC", T::NoConnect}, {"6", "NC", T::NoConnect}, {"7", "GND", T::PowerIn}, {"8", "GND", T::PowerIn},
            {"9", "CS8", T::Bidirectional}, {"10", "CS7", T::Bidirectional}, {"11", "CS6", T::Bidirectional}, {"12", "CS5/SG", T::Bidirectional},
            {"13", "CS4", T::Bidirectional}, {"14", "CS3", T::Bidirectional}, {"15", "CS2", T::Bidirectional}, {"16", "CS1", T::Bidirectional},
            {"EP", "EP", T::Passive}});
    catalog(parts, "Interface", "KSZ9897RTXC", "Microchip",
            "7-port Gigabit Ethernet switch: 5 integrated 10/100/1000BASE-T PHYs and 2 RGMII / MII / RMII ports",
            "U", "LQFP", 128, 0.4, 14, "TQFP-128 14×14 with exposed pad (TX)",
            {{"1", "TXRX1P_A", T::Bidirectional}, {"2", "TXRX1M_A", T::Bidirectional}, {"3", "AVDDL", T::PowerIn}, {"4", "TXRX1P_B", T::Bidirectional},
            {"5", "TXRX1M_B", T::Bidirectional}, {"6", "TXRX1P_C", T::Bidirectional}, {"7", "TXRX1M_C", T::Bidirectional}, {"8", "TXRX1P_D", T::Bidirectional},
            {"9", "TXRX1M_D", T::Bidirectional}, {"10", "AVDDH", T::PowerIn}, {"11", "DVDDL", T::PowerIn}, {"12", "TXRX2P_A", T::Bidirectional},
            {"13", "TXRX2M_A", T::Bidirectional}, {"14", "AVDDL", T::PowerIn}, {"15", "TXRX2P_B", T::Bidirectional}, {"16", "TXRX2M_B", T::Bidirectional},
            {"17", "TXRX2P_C", T::Bidirectional}, {"18", "TXRX2M_C", T::Bidirectional}, {"19", "AVDDL", T::PowerIn}, {"20", "TXRX2P_D", T::Bidirectional},
            {"21", "TXRX2M_D", T::Bidirectional}, {"22", "AVDDH", T::PowerIn}, {"23", "DVDDL", T::PowerIn}, {"24", "TXRX3P_A", T::Bidirectional},
            {"25", "TXRX3M_A", T::Bidirectional}, {"26", "TXRX3P_B", T::Bidirectional}, {"27", "TXRX3M_B", T::Bidirectional}, {"28", "TXRX3P_C", T::Bidirectional},
            {"29", "TXRX3M_C", T::Bidirectional}, {"30", "AVDDL", T::PowerIn}, {"31", "TXRX3P_D", T::Bidirectional}, {"32", "TXRX3M_D", T::Bidirectional},
            {"33", "AVDDH", T::PowerIn}, {"34", "TXRX4P_A", T::Bidirectional}, {"35", "TXRX4M_A", T::Bidirectional}, {"36", "AVDDL", T::PowerIn},
            {"37", "TXRX4P_B", T::Bidirectional}, {"38", "TXRX4M_B", T::Bidirectional}, {"39", "TXRX4P_C", T::Bidirectional}, {"40", "TXRX4M_C", T::Bidirectional},
            {"41", "AVDDL", T::PowerIn}, {"42", "TXRX4P_D", T::Bidirectional}, {"43", "TXRX4M_D", T::Bidirectional}, {"44", "AVDDH", T::PowerIn},
            {"45", "DVDDL", T::PowerIn}, {"46", "GND", T::PowerIn}, {"47", "GND", T::PowerIn}, {"48", "TX_CLK6/REFCLKI6", T::Bidirectional},
            {"49", "TX_EN6/TX_CTL6", T::Input}, {"50", "TX_ER6", T::Input}, {"51", "COL6", T::Bidirectional}, {"52", "TXD6_3", T::Input},
            {"53", "TXD6_2", T::Input}, {"54", "TXD6_1", T::Input}, {"55", "TXD6_0", T::Input}, {"56", "DVDDL", T::PowerIn},
            {"57", "RX_CLK6/REFCLKO6", T::Bidirectional}, {"58", "RX_DV6/CRS_DV6/RX_CTL6", T::Bidirectional}, {"59", "RX_ER6", T::Bidirectional}, {"60", "CRS6", T::Bidirectional},
            {"61", "VDDIO", T::PowerIn}, {"62", "RXD6_3", T::Bidirectional}, {"63", "RXD6_2", T::Bidirectional}, {"64", "RXD6_1", T::Bidirectional},
            {"65", "RXD6_0", T::Bidirectional}, {"66", "TX_CLK7/REFCLKI7", T::Bidirectional}, {"67", "TX_EN7/TX_CTL7", T::Input}, {"68", "TX_ER7", T::Input},
            {"69", "COL7", T::Bidirectional}, {"70", "TXD7_3", T::Input}, {"71", "TXD7_2", T::Input}, {"72", "TXD7_1", T::Input},
            {"73", "TXD7_0", T::Input}, {"74", "DVDDL", T::PowerIn}, {"75", "RX_CLK7/REFCLKO7", T::Bidirectional}, {"76", "RX_DV7/CRS_DV7/RX_CTL7", T::Bidirectional},
            {"77", "VDDIO", T::PowerIn}, {"78", "RX_ER7", T::Bidirectional}, {"79", "CRS7", T::Bidirectional}, {"80", "RXD7_3", T::Bidirectional},
            {"81", "RXD7_2", T::Bidirectional}, {"82", "RXD7_1", T::Bidirectional}, {"83", "RXD7_0", T::Bidirectional}, {"84", "GND", T::PowerIn},
            {"85", "LED4_0", T::Bidirectional}, {"86", "LED4_1", T::Bidirectional}, {"87", "DVDDL", T::PowerIn}, {"88", "LED3_0", T::Bidirectional},
            {"89", "LED3_1", T::Bidirectional}, {"90", "NC", T::NoConnect}, {"91", "LED2_0", T::Bidirectional}, {"92", "LED2_1", T::Bidirectional},
            {"93", "nPME", T::Output}, {"94", "nINTRP", T::Output}, {"95", "CLKO_25_125", T::Output}, {"96", "nRESET", T::Input},
            {"97", "SDO", T::Output}, {"98", "SDI/SDA/MDIO", T::Bidirectional}, {"99", "VDDIO", T::PowerIn}, {"100", "nSCS", T::Input},
            {"101", "SCL/MDC", T::Input}, {"102", "LED5_0", T::Bidirectional}, {"103", "LED5_1", T::Bidirectional}, {"104", "DVDDL", T::PowerIn},
            {"105", "LED1_0", T::Bidirectional}, {"106", "LED1_1", T::Bidirectional}, {"107", "GND", T::PowerIn}, {"108", "NC", T::NoConnect},
            {"109", "GND", T::PowerIn}, {"110", "DVDDL", T::PowerIn}, {"111", "AVDDH", T::PowerIn}, {"112", "TXRX5P_A", T::Bidirectional},
            {"113", "TXRX5M_A", T::Bidirectional}, {"114", "AVDDL", T::PowerIn}, {"115", "TXRX5P_B", T::Bidirectional}, {"116", "TXRX5M_B", T::Bidirectional},
            {"117", "TXRX5P_C", T::Bidirectional}, {"118", "TXRX5M_C", T::Bidirectional}, {"119", "AVDDL", T::PowerIn}, {"120", "TXRX5P_D", T::Bidirectional},
            {"121", "TXRX5M_D", T::Bidirectional}, {"122", "AVDDH", T::PowerIn}, {"123", "GND", T::PowerIn}, {"124", "AVDDL", T::PowerIn},
            {"125", "XO", T::Output}, {"126", "XI", T::Input}, {"127", "ISET", T::Passive}, {"128", "AVDDH", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Microcontrollers · Microchip", "PIC16F18446-I/SS", "Microchip",
            "8-bit PIC16 MCU, 28 KB flash, 2 KB RAM, 32 MHz, 12-bit ADC with computation, 20 pins",
            "U", "TSSOP", 20, 0.65, 5.3, "SSOP-20 5.30 mm (SS)",
            {{"1", "VDD", T::PowerIn}, {"2", "RA5", T::Bidirectional}, {"3", "RA4", T::Bidirectional}, {"4", "MCLR/VPP/RA3", T::Input},
            {"5", "RC5", T::Bidirectional}, {"6", "RC4", T::Bidirectional}, {"7", "RC3", T::Bidirectional}, {"8", "RC6", T::Bidirectional},
            {"9", "RC7", T::Bidirectional}, {"10", "RB7", T::Bidirectional}, {"11", "RB6", T::Bidirectional}, {"12", "RB5", T::Bidirectional},
            {"13", "RB4", T::Bidirectional}, {"14", "RC2", T::Bidirectional}, {"15", "RC1", T::Bidirectional}, {"16", "RC0", T::Bidirectional},
            {"17", "RA2", T::Bidirectional}, {"18", "RA1/ICSPCLK", T::Bidirectional}, {"19", "RA0/ICSPDAT", T::Bidirectional}, {"20", "VSS", T::PowerIn}});
    catalog(parts, "Motor Control", "6EDL7141XUMA1", "Infineon",
            "MOTIX three-phase smart gate driver for BLDC / PMSM motors, buck regulator, 3 current-sense amplifiers, SPI",
            "U", "QFN", 48, 0.5, 7, "VQFN-48 7×7 (PG-VQFN-48-78)",
            {{"1", "nSCS", T::Input}, {"2", "SCLK", T::Input}, {"3", "SDO", T::Output}, {"4", "SDI", T::Input},
            {"5", "nFAULT", T::OpenCollector}, {"6", "INHA", T::Input}, {"7", "INLA", T::Input}, {"8", "INHB", T::Input},
            {"9", "INLB", T::Input}, {"10", "INHC", T::Input}, {"11", "INLC", T::Input}, {"12", "CS_GAIN/AZ", T::Input},
            {"13", "VSENSE/nBRAKE", T::Input}, {"14", "DVDD", T::PowerOut}, {"15", "PVDD", T::PowerIn}, {"16", "PH", T::Passive},
            {"17", "PGND", T::PowerIn}, {"18", "VDDB", T::PowerOut}, {"19", "CP1L", T::Passive}, {"20", "CP1H", T::Passive},
            {"21", "CP2L", T::Passive}, {"22", "CP2H", T::Passive}, {"23", "VCCLS", T::PowerOut}, {"24", "VCCHS", T::PowerOut},
            {"25", "GHC", T::Output}, {"26", "SHC", T::Bidirectional}, {"27", "NC", T::NoConnect}, {"28", "GLC", T::Output},
            {"29", "SLC", T::Bidirectional}, {"30", "CSNC", T::Input}, {"31", "CSNB", T::Input}, {"32", "SLB", T::Bidirectional},
            {"33", "GLB", T::Output}, {"34", "NC", T::NoConnect}, {"35", "SHB", T::Bidirectional}, {"36", "GHB", T::Output},
            {"37", "GHA", T::Output}, {"38", "SHA", T::Bidirectional}, {"39", "NC", T::NoConnect}, {"40", "GLA", T::Output},
            {"41", "SLA", T::Bidirectional}, {"42", "CSNA", T::Input}, {"43", "CSOA", T::Output}, {"44", "CSOB", T::Output},
            {"45", "CSOC", T::Output}, {"46", "VREF", T::Input}, {"47", "CE", T::Input}, {"48", "EN_DRV", T::Input},
            {"EP", "DGND", T::PowerIn}});
    catalog(parts, "Memory", "FM24CL64B-GATR", "Infineon",
            "64 Kbit (8 K × 8) I2C F-RAM, 3 V, automotive grade (−40 to +125 °C)",
            "U", "SOIC", 8, 1.27, 3.9, "SOIC-8 150 mil",
            {{"1", "A0", T::Input}, {"2", "A1", T::Input}, {"3", "A2", T::Input}, {"4", "VSS", T::PowerIn},
            {"5", "SDA", T::Bidirectional}, {"6", "SCL", T::Input}, {"7", "WP", T::Input}, {"8", "VDD", T::PowerIn}});
    catalog(parts, "Security", "SLB9670VQ20FW785XTMA1", "Infineon",
            "OPTIGA TPM 2.0 trusted platform module, SPI up to 43 MHz, firmware 7.85",
            "U", "QFN", 32, 0.5, 5, "VQFN-32 5×5 (PG-VQFN-32-13)",
            {{"1", "NCI/VDD", T::NoConnect}, {"2", "GND", T::PowerIn}, {"3", "NCI", T::NoConnect}, {"4", "NCI", T::NoConnect},
            {"5", "NCI", T::NoConnect}, {"6", "GPIO", T::Bidirectional}, {"7", "PP", T::Input}, {"8", "VDD", T::PowerIn},
            {"9", "GND", T::PowerIn}, {"10", "NCI", T::NoConnect}, {"11", "NCI", T::NoConnect}, {"12", "NCI", T::NoConnect},
            {"13", "NCI", T::NoConnect}, {"14", "NCI/VDD", T::PowerIn}, {"15", "NCI", T::NoConnect}, {"16", "NCI/GND", T::PowerIn},
            {"17", "nRST", T::Input}, {"18", "nPIRQ", T::OpenCollector}, {"19", "SCLK", T::Input}, {"20", "nCS", T::Input},
            {"21", "MOSI", T::Input}, {"22", "VDD", T::PowerIn}, {"23", "GND", T::PowerIn}, {"24", "MISO", T::Output},
            {"25", "NCI", T::NoConnect}, {"26", "NCI", T::NoConnect}, {"27", "NCI", T::NoConnect}, {"28", "NCI", T::NoConnect},
            {"29", "NC", T::NoConnect}, {"30", "NC", T::NoConnect}, {"31", "NCI", T::NoConnect}, {"32", "GND", T::PowerIn},
            {"EP", "GND", T::PowerIn}});
    catalog(parts, "Power Electronics", "IKW40N120H3FKSA1", "Infineon",
            "1200 V 40 A high-speed TRENCHSTOP IGBT (H3) with soft fast-recovery anti-parallel diode",
            "Q", "CUSTOM", 3, 5.44, 15.9, "TO-247-3 (PG-TO247-3)",
            {{"1", "G", T::Input}, {"2", "C", T::Passive}, {"3", "E", T::Passive}});
    parts.back().spec.package.lands = {{-5.44, 0, 3.0, 4.5, 1.9, false, "1"}, {0, 0, 3.0, 4.5, 1.9, true, "2"}, {5.44, 0, 3.0, 4.5, 1.9, true, "3"}};
    catalog(parts, "Power Electronics", "IMW120R030M1HXKSA1", "Infineon",
            "1200 V 30 mΩ 56 A CoolSiC silicon-carbide MOSFET",
            "Q", "CUSTOM", 3, 5.44, 15.9, "TO-247-3 (PG-TO247-3)",
            {{"1", "G", T::Input}, {"2", "D", T::Passive}, {"3", "S", T::Passive}});
    parts.back().spec.package.lands = {{-5.44, 0, 3.0, 4.5, 1.9, false, "1"}, {0, 0, 3.0, 4.5, 1.9, true, "2"}, {5.44, 0, 3.0, 4.5, 1.9, true, "3"}};
    catalog(parts, "Microcontrollers · Arm", "LPC55S69JBD100", "NXP",
            "Dual Arm Cortex-M33 150 MHz, 640 KB flash, 320 KB SRAM, TrustZone, PowerQuad, USB FS + HS",
            "U", "LQFP", 100, 0.5, 14, "HLQFP-100 14×14 with exposed pad (SOT1570-3)",
            {{"1", "PIO1_4", T::Bidirectional}, {"2", "PIO1_13", T::Bidirectional}, {"3", "PIO1_24", T::Bidirectional}, {"4", "PIO1_20", T::Bidirectional},
            {"5", "PIO1_6", T::Bidirectional}, {"6", "PIO0_7", T::Bidirectional}, {"7", "PIO0_1", T::Bidirectional}, {"8", "PIO0_17", T::Bidirectional},
            {"9", "PIO1_7", T::Bidirectional}, {"10", "PIO1_9", T::Bidirectional}, {"11", "PIO1_0", T::Bidirectional}, {"12", "PIO0_12", T::Bidirectional},
            {"13", "PIO0_11", T::Bidirectional}, {"14", "PIO0_16", T::Bidirectional}, {"15", "VDD", T::PowerIn}, {"16", "VDDA", T::PowerIn},
            {"17", "VREFP", T::PowerIn}, {"18", "VREFN", T::PowerIn}, {"19", "VSSA", T::PowerIn}, {"20", "PIO0_23", T::Bidirectional},
            {"21", "PIO0_10", T::Bidirectional}, {"22", "PIO0_15", T::Bidirectional}, {"23", "PIO0_31", T::Bidirectional}, {"24", "PIO1_8", T::Bidirectional},
            {"25", "VDD", T::PowerIn}, {"26", "PIO0_8", T::Bidirectional}, {"27", "PIO0_27", T::Bidirectional}, {"28", "XTAL32M_N", T::Passive},
            {"29", "XTAL32M_P", T::Passive}, {"30", "PIO1_21", T::Bidirectional}, {"31", "PIO1_5", T::Bidirectional}, {"32", "nRESET", T::Input},
            {"33", "USB1_VSS", T::PowerIn}, {"34", "USB1_DP", T::Bidirectional}, {"35", "USB1_DM", T::Bidirectional}, {"36", "USB1_VBUS", T::Input},
            {"37", "USB1_VSS", T::PowerIn}, {"38", "USB1_3V3", T::PowerIn}, {"39", "VDD_PMU", T::PowerIn}, {"40", "PIO1_10", T::Bidirectional},
            {"41", "PIO1_22", T::Bidirectional}, {"42", "PIO1_23", T::Bidirectional}, {"43", "PIO1_17", T::Bidirectional}, {"44", "VDD", T::PowerIn},
            {"45", "FB", T::Input}, {"46", "VSS_DCDC", T::PowerIn}, {"47", "VSS_DCDC", T::PowerIn}, {"48", "LX", T::Passive},
            {"49", "VBAT_DCDC", T::PowerIn}, {"50", "VBAT_DCDC", T::PowerIn}, {"51", "VBAT_PMU", T::PowerIn}, {"52", "XTAL32K_P", T::Passive},
            {"53", "XTAL32K_N", T::Passive}, {"54", "PIO0_0", T::Bidirectional}, {"55", "PIO0_9", T::Bidirectional}, {"56", "PIO0_18", T::Bidirectional},
            {"57", "PIO1_14", T::Bidirectional}, {"58", "PIO1_19", T::Bidirectional}, {"59", "PIO1_1", T::Bidirectional}, {"60", "PIO0_26", T::Bidirectional},
            {"61", "PIO1_2", T::Bidirectional}, {"62", "PIO1_3", T::Bidirectional}, {"63", "VDD", T::PowerIn}, {"64", "PIO1_18", T::Bidirectional},
            {"65", "PIO1_30", T::Bidirectional}, {"66", "PIO0_28", T::Bidirectional}, {"67", "PIO1_12", T::Bidirectional}, {"68", "PIO1_26", T::Bidirectional},
            {"69", "VDD", T::PowerIn}, {"70", "PIO0_24", T::Bidirectional}, {"71", "PIO0_13", T::Bidirectional}, {"72", "PIO0_14", T::Bidirectional},
            {"73", "PIO1_28", T::Bidirectional}, {"74", "PIO0_20", T::Bidirectional}, {"75", "VDD", T::PowerIn}, {"76", "PIO0_21", T::Bidirectional},
            {"77", "PIO1_25", T::Bidirectional}, {"78", "PIO0_22", T::Bidirectional}, {"79", "PIO0_25", T::Bidirectional}, {"80", "PIO1_29", T::Bidirectional},
            {"81", "PIO0_2", T::Bidirectional}, {"82", "PIO1_15", T::Bidirectional}, {"83", "PIO0_3", T::Bidirectional}, {"84", "VDD", T::PowerIn},
            {"85", "PIO1_27", T::Bidirectional}, {"86", "PIO0_4", T::Bidirectional}, {"87", "PIO1_16", T::Bidirectional}, {"88", "PIO0_5", T::Bidirectional},
            {"89", "PIO0_6", T::Bidirectional}, {"90", "PIO0_19", T::Bidirectional}, {"91", "PIO1_31", T::Bidirectional}, {"92", "PIO0_29", T::Bidirectional},
            {"93", "PIO1_11", T::Bidirectional}, {"94", "PIO0_30", T::Bidirectional}, {"95", "VDD", T::PowerIn}, {"96", "USB0_3V3", T::PowerIn},
            {"97", "USB0_DP", T::Bidirectional}, {"98", "USB0_DM", T::Bidirectional}, {"99", "USB0_VSS", T::PowerIn}, {"100", "VDD", T::PowerIn},
            {"EP", "VSS", T::PowerIn}});
    catalog(parts, "Power", "TPS63070RNMR", "Texas Instruments",
            "2–16 V input, 2.5–9 V output buck-boost converter, 2 A (Vin ≥ 4 V), adjustable",
            "U", "LGA", 15, 0, 2.5, "VQFN-HR-15 2.5×3 (RNM0015A, TI land pattern)",
            {{"1", "PS/SYNC", T::Input}, {"2", "PG", T::OpenCollector}, {"3", "VAUX", T::PowerOut}, {"4", "GND", T::PowerIn},
            {"5", "FB", T::Input}, {"6", "FB2", T::Output}, {"7", "VOUT", T::PowerOut}, {"8", "VOUT", T::PowerOut},
            {"9", "L2", T::Passive}, {"10", "PGND", T::PowerIn}, {"11", "L1", T::Passive}, {"12", "VIN", T::PowerIn},
            {"13", "VIN", T::PowerIn}, {"14", "EN", T::Input}, {"15", "VSEL", T::Input}});
    parts.back().spec.package.lands = {{-1.15, -0.75, 0.6, 0.25, 0, false, "1"}, {-1.15, -0.25, 0.6, 0.25, 0, false, "2"}, {-1.15, 0.25, 0.6, 0.25, 0, false, "3"}, {-1.15, 0.75, 0.6, 0.25, 0, false, "4"}, {-0.725, 1.4, 0.25, 0.6, 0, false, "5"}, {-0.225, 1.4, 0.25, 0.6, 0, false, "6"}, {0.275, 1.4, 0.25, 0.6, 0, false, "7"}, {0.775, 1.4, 0.25, 0.6, 0, false, "8"}, {0.525, 1.525, 0.25, 0.35, 0, false, "8"}, {0.775, 0.5, 1.35, 0.25, 0, false, "9"}, {0.6, 0, 1.7, 0.25, 0, false, "10"}, {0.775, -0.5, 1.35, 0.25, 0, false, "11"}, {0.775, -1.4, 0.25, 0.6, 0, false, "12"}, {0.525, -1.525, 0.25, 0.35, 0, false, "12"}, {0.275, -1.4, 0.25, 0.6, 0, false, "13"}, {-0.225, -1.4, 0.25, 0.6, 0, false, "14"}, {-0.725, -1.4, 0.25, 0.6, 0, false, "15"}};
    parts.back().spec.package.bodyDepth = 3.0;
    catalog(parts, "Power", "TPS259470ARPWR", "Texas Instruments",
            "2.7–23 V, 5 A eFuse with adjustable OVLO, AUXOFF and fault flag (auto-retry)",
            "U", "LGA", 10, 0, 2, "VQFN-HR-10 2×2 (RPW0010A, TI land pattern)",
            {{"1", "EN/UVLO", T::Input}, {"2", "OVLO", T::Input}, {"3", "AUXOFF", T::OpenCollector}, {"4", "nFLT", T::OpenCollector},
            {"5", "IN", T::PowerIn}, {"6", "OUT", T::PowerOut}, {"7", "DVDT", T::Passive}, {"8", "GND", T::PowerIn},
            {"9", "ILM", T::Passive}, {"10", "ITIMER", T::Passive}});
    parts.back().spec.package.lands = {{-0.9, -0.7, 0.6, 0.3, 0, false, "1"}, {-0.725, -0.875, 0.25, 0.65, 0, false, "1"}, {-0.9, -0.225, 0.6, 0.25, 0, false, "2"}, {-0.9, 0.225, 0.6, 0.25, 0, false, "3"}, {-0.9, 0.7, 0.6, 0.3, 0, false, "4"}, {-0.725, 0.875, 0.25, 0.65, 0, false, "4"}, {-0.25, 0, 0.3, 2.4, 0, false, "5"}, {0.25, 0, 0.3, 2.4, 0, false, "6"}, {0.9, 0.7, 0.6, 0.3, 0, false, "7"}, {0.725, 0.875, 0.25, 0.65, 0, false, "7"}, {0.9, 0.225, 0.6, 0.25, 0, false, "8"}, {0.9, -0.225, 0.6, 0.25, 0, false, "9"}, {0.9, -0.7, 0.6, 0.3, 0, false, "10"}, {0.725, -0.875, 0.25, 0.65, 0, false, "10"}};
    parts.back().spec.package.bodyDepth = 2.0;
    catalog(parts, "Audio", "INMP441ACEZ-R0", "TDK InvenSense",
            "Omnidirectional MEMS microphone, I²S output, bottom port, LGA_CAV 4.72 × 3.76 mm",
            "MK", "LGA", 9, 0, 3.76, "LGA_CAV-9 4.72×3.76 (InvenSense land pattern)",
            {{"1", "SCK", T::Input}, {"2", "SD", T::Output}, {"3", "WS", T::Input}, {"4", "L/R", T::Input},
            {"5", "GND", T::PowerIn}, {"6", "GND", T::PowerIn}, {"7", "VDD", T::PowerIn}, {"8", "CHIPEN", T::Input},
            {"9", "GND", T::PowerIn}});
    {
        // Datasheet land pattern turned a quarter so pin 1 is top-left; pin 5 is the ground ring (Ø1.56 / Ø0.96)
        // round the sound port, drawn as a keyhole outline, and the port itself is a 0.25 mm non-plated hole.
        auto& pkg = parts.back().spec.package;
        pkg.bodyDepth = 4.72;
        pkg.lands = {{-1.33, -1.86, 0.6, 0.4}, {-1.33, -0.81, 0.6, 0.4}, {-1.33, 0.24, 0.6, 0.4}, {-1.33, 1.29, 0.6, 0.4},
                     {0, 1.29, 1.56, 1.56},    {1.33, 1.29, 0.6, 0.4},   {1.33, 0.24, 0.6, 0.4},  {1.33, -0.81, 0.6, 0.4},
                     {1.33, -1.86, 0.6, 0.4},  {0, 1.29, 0.3, 0.3, 0.25, true, "-"}};
        const double step = std::acos(-1.0) / 15;  // 31 + 31 points: within a pad outline's 64
        for (int k = 0; k <= 30; ++k) pkg.lands[4].outline.push_back({0.78 * std::cos(k * step), 0.78 * std::sin(k * step)});
        for (int k = 30; k >= 0; --k) pkg.lands[4].outline.push_back({0.48 * std::cos(k * step), 0.48 * std::sin(k * step)});
        pkg.lands.back().npth = true;
    }
    parts.push_back(part("Motor Control", "L293DD", "STMicroelectronics",
                         "Quad half-H driver with clamp diodes, 600 mA per channel, SOIC-20 (GND pins 4-7 / 14-17 sink heat)",
                         "SOIC", 20, "U",
                         {{"EN1", T::Input}, {"IN1", T::Input}, {"OUT1", T::Output}, {"GND", T::PowerIn},
                          {"GND", T::PowerIn}, {"GND", T::PowerIn}, {"GND", T::PowerIn}, {"OUT2", T::Output},
                          {"IN2", T::Input}, {"VS", T::PowerIn}, {"EN2", T::Input}, {"IN3", T::Input}, {"OUT3", T::Output},
                          {"GND", T::PowerIn}, {"GND", T::PowerIn}, {"GND", T::PowerIn}, {"GND", T::PowerIn},
                          {"OUT4", T::Output}, {"IN4", T::Input}, {"VSS", T::PowerIn}}));
    parts.back().spec.package.bodySize = 7.5;
    parts.push_back(part("Interface", "TCAN1042VDRQ1", "Texas Instruments",
                         "Automotive CAN FD transceiver, 5 Mbit/s, 1.8…5 V VIO logic supply, ±58 V bus fault protection",
                         "SOIC", 8, "U",
                         {{"TXD", T::Input}, {"GND", T::PowerIn}, {"VCC", T::PowerIn}, {"RXD", T::Output},
                          {"VIO", T::PowerIn}, {"CANL", T::Bidirectional}, {"CANH", T::Bidirectional}, {"STB", T::Input}}));
    parts.push_back(part("Interface", "SP485EEN-L", "MaxLinear (Exar)", "RS-485 half-duplex transceiver, ±15 kV ESD, SOIC-8",
                         "SOIC", 8, "U",
                         {{"RO", T::Output}, {"nRE", T::Input}, {"DE", T::Input}, {"DI", T::Input}, {"GND", T::PowerIn},
                          {"A", T::Bidirectional}, {"B", T::Bidirectional}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Optocouplers", "PC817X3NSZ0F", "Sharp", "Phototransistor optocoupler, 5 kV, CTR 80…160 %, DIP-4",
                         "DIP", 4, "U", {{"A", T::Passive}, {"K", T::Passive}, {"E", T::Passive}, {"C", T::Passive}}));
    parts.push_back(part("Optocouplers", "TLP281-4", "Toshiba",
                         "Quad phototransistor optocoupler, 2.5 kV, SOP-16 (motor-to-CPU galvanic isolation)", "SOIC", 16, "U",
                         {{"A1", T::Passive}, {"K1", T::Passive}, {"A2", T::Passive}, {"K2", T::Passive}, {"A3", T::Passive},
                          {"K3", T::Passive}, {"A4", T::Passive}, {"K4", T::Passive}, {"E4", T::Passive}, {"C4", T::Passive},
                          {"E3", T::Passive}, {"C3", T::Passive}, {"E2", T::Passive}, {"C2", T::Passive}, {"E1", T::Passive},
                          {"C1", T::Passive}}));
    parts.back().spec.package.bodySize = 4.4;
    parts.push_back(part("Audio", "MAX4466EXK+T", "Analog Devices (Maxim)",
                         "Low-power microphone pre-amplifier op-amp, 200 kHz GBW, SC70-5", "SOT23", 5, "U",
                         {{"OUT", T::Output}, {"VSS", T::PowerIn}, {"IN+", T::Input}, {"IN-", T::Input}, {"VDD", T::PowerIn}}));
    {
        // Power MOSFET in a 5 × 6 mm SON: source pins 1-3, gate 4, drain pins 5-8 and the drain tab.
        StandardPart fet = part("Power Electronics", "CSD18540Q5B", "Texas Instruments",
                                "60 V N-channel NexFET, 1.8 mΩ, 100 A — leg / phase switching stages, VSON-8 (5 × 6 mm)",
                                "SON", 8, "Q",
                                {{"S", T::Passive}, {"S", T::Passive}, {"S", T::Passive}, {"G", T::Input}, {"D", T::Passive},
                                 {"D", T::Passive}, {"D", T::Passive}, {"D", T::Passive}});
        fet.spec.pins.push_back({"EP", "D", T::Passive, ""});
        fet.spec.package.pitch = 1.27;
        fet.spec.package.bodySize = 5.0;
        parts.push_back(std::move(fet));
    }
    applyModels(parts);
    return parts;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

/// Parts with 16 or more pins get a readable symbol: supplies on top, grounds at the bottom, inputs left, outputs
/// right, MCU ports grouped, repeated supply / ground pins stacked. Small parts keep the classic datasheet-order box.
std::vector<StandardPart> withArrangedSymbols(std::vector<StandardPart> parts) {
    for (auto& p : parts)
        if (p.spec.pins.size() >= 16 && p.spec.symbol.empty()) p.spec.symbol = autoArrangeSymbol(p.spec);
    return parts;
}

const std::vector<StandardPart>& standardParts() {
    static const std::vector<StandardPart> parts = withArrangedSymbols(build());
    return parts;
}

const StandardPart* findStandardPart(const std::string& name) {
    std::string key = lower(name);
    for (const auto& p : standardParts())
        if (lower(p.spec.name) == key) return &p;
    return nullptr;
}

}  // namespace sieda
