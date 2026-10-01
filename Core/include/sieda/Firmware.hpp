// SiEDA Core — built-in example firmware for the simulated microcontrollers (Intel HEX, built with avr-gcc and the
// Arduino AVR core by Core/tests/firmware/build.sh).
#pragma once

#include <string>
#include <vector>

namespace sieda {

struct FirmwareExample {
    std::string id;           // "arduino_blink"
    std::string name;         // "Arduino Blink"
    std::string model;        // "ATmega328P" / "ATtiny85"
    std::string description;  // what it does and which pins it uses
    std::string hex;          // Intel HEX
};

const std::vector<FirmwareExample>& firmwareExamples();
const FirmwareExample* findFirmwareExample(const std::string& id);

}  // namespace sieda
