#include "sieda/StandardParts.hpp"

#include <cctype>

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

/// Behavioural simulation models (datasheet typical values; pins by number).
void applyModels(std::vector<StandardPart>& parts) {
    for (auto& p : parts) {
        const std::string& n = p.spec.name;
        if (n == "LM7805") regulator(p, "1", "3", "2", 5.0, 2.0, 0.005, 1.5, 2.0);
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
    applyModels(parts);
    return parts;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
}  // namespace

const std::vector<StandardPart>& standardParts() {
    static const std::vector<StandardPart> parts = build();
    return parts;
}

const StandardPart* findStandardPart(const std::string& name) {
    std::string key = lower(name);
    for (const auto& p : standardParts())
        if (lower(p.spec.name) == key) return &p;
    return nullptr;
}

}  // namespace sieda
