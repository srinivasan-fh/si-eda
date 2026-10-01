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
    parts.push_back(part("Microcontrollers", "ATtiny85", "Microchip", "8-bit AVR MCU, 8 KB flash", "DIP", 8, "U",
                         {{"PB5/RST", T::Bidirectional}, {"PB3", T::Bidirectional}, {"PB4", T::Bidirectional},
                          {"GND", T::PowerIn}, {"PB0", T::Bidirectional}, {"PB1", T::Bidirectional},
                          {"PB2", T::Bidirectional}, {"VCC", T::PowerIn}}));
    parts.push_back(part("Microcontrollers", "ATmega328P", "Microchip", "8-bit AVR MCU, 32 KB flash", "DIP", 28, "U",
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
