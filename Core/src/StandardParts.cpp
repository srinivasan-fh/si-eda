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
    p.spec.model.regulator = {in, out, ref, vout, dropout, iq, ilimit, maxPower, charger};
}
void load(StandardPart& p, const char* supply, const char* ret, double amps) { p.spec.model.loads.push_back({supply, ret, amps}); }

/// Behavioural simulation models (datasheet typical values; pins by number).
void applyModels(std::vector<StandardPart>& parts) {
    for (auto& p : parts) {
        const std::string& n = p.spec.name;
        if (n == "LM7805") regulator(p, "1", "3", "2", 5.0, 2.0, 0.005, 1.5, 2.0);
        else if (n == "LM317") regulator(p, "3", "2", "1", 1.25, 2.0, 50e-6, 1.5, 2.0);  // ADJ is the reference
        else if (n == "LM2940-5.0") regulator(p, "1", "3", "2", 5.0, 0.5, 0.010, 1.0, 2.0);
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
