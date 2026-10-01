#include "sieda/Firmware.hpp"

namespace sieda {

namespace {
#include "FirmwareHex.inc"

std::vector<FirmwareExample> build() {
    return {
        {"arduino_blink", "Arduino Blink", "ATmega328P",
         "Arduino core sketch: the built-in LED on pin 13 (PB5) blinks 200 ms on / 200 ms off.", kHex_arduino_Blink},
        {"arduino_analog_serial", "Arduino AnalogReadSerial + Fade", "ATmega328P",
         "Arduino core sketch: reads A0 (PC0) every 50 ms, prints \"<millis> <value>\" at 115200 baud and dims pin 9 "
         "(PB1, PWM) with it.",
         kHex_arduino_AnalogSerial},
        {"blink", "Blink (bare metal)", "ATmega328P", "PB5 (Arduino D13) toggles every 250 ms (avr-libc _delay_ms).",
         kHex_blink},
        {"uart_hello", "UART Hello", "ATmega328P",
         "USART0 at 9600 baud on TXD (PD1): \"Hello, SiEDA!\", then a counter every 20 ms.", kHex_hello},
        {"timer_interrupt", "Timer interrupt (1 kHz)", "ATmega328P",
         "Timer1 CTC compare interrupt at 1 kHz; PB5 toggles and the tick count is printed (115200 baud) every 100 ms.",
         kHex_timer_isr},
        {"adc_pwm", "ADC → PWM", "ATmega328P",
         "Reads ADC0 (PC0) every 10 ms, prints it at 115200 baud and sets OC0A (PD6) fast PWM to value / 4; OC1A (PB1) "
         "runs 25 % phase-correct PWM.",
         kHex_adc_pwm},
        {"button_interrupt", "Button (INT0)", "ATmega328P",
         "Button from PD2 (INT0, internal pull-up) to ground: the LED on PB5 lights while pressed and each press prints "
         "\"press N\" at 115200 baud.",
         kHex_button},
        {"cpu_selftest", "CPU self-test", "ATmega328P",
         "Integer, float, string, sort and recursion checks printed at 1 Mbaud, ending with END.", kHex_alu},
        {"tiny_blink", "ATtiny85 Blink + PWM", "ATtiny85", "8 MHz: PB0 toggles every 100 ms; PB1 (OC0B) 50 % fast PWM.",
         kHex_tiny_blink},
    };
}
}  // namespace

const std::vector<FirmwareExample>& firmwareExamples() {
    static const std::vector<FirmwareExample> examples = build();
    return examples;
}

const FirmwareExample* findFirmwareExample(const std::string& id) {
    for (const auto& e : firmwareExamples())
        if (e.id == id) return &e;
    return nullptr;
}

}  // namespace sieda
