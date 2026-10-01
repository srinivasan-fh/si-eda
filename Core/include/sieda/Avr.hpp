// SiEDA Core — AVR microcontroller simulator (ATmega328P / Arduino Uno-Nano, ATtiny85).
//
// A cycle-counted AVR instruction-set simulator with the peripherals Arduino-style firmware uses: GPIO ports, Timer0/1/2
// (normal, CTC, fast and phase-correct PWM), USART0 (TX/RX with the real bit waveform on TXD), the 10-bit ADC, EEPROM,
// external (INT0/1) and pin-change interrupts, and sleep. Firmware is an Intel HEX image (Arduino "Export compiled
// binary", avr-gcc + avr-objcopy). The circuit simulator co-simulates it: each analog step runs the core for the step's
// clock cycles, then drives every pin from the time it spent high, low or pulled up, and reads inputs and the ADC from
// the solved node voltages.
#pragma once

#include <array>
#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <vector>

namespace sieda {

enum class McuModel { ATmega328P, ATtiny85 };

/// The MCU model a component's part name stands for ("ATmega328P", "ATmega328", "ATtiny85"…); nullopt otherwise.
std::optional<McuModel> mcuModelForPart(const std::string& partName);
std::string mcuModelName(McuModel model);
/// Default CPU clock: 16 MHz (Arduino Uno/Nano) for the ATmega328P, 8 MHz (internal RC) for the ATtiny85.
double defaultMcuClock(McuModel model);

/// Intel HEX → flash image. `error` is empty on success.
struct HexImage {
    std::vector<uint8_t> bytes;
    std::string error;
    bool ok() const { return error.empty(); }
};
HexImage parseIntelHex(const std::string& text, size_t maxBytes = 1 << 20);
/// Flash image → Intel HEX (16-byte data records).
std::string toIntelHex(const std::vector<uint8_t>& bytes);

class AvrMcu {
public:
    explicit AvrMcu(McuModel model);

    McuModel model() const { return model_; }
    /// Loads a flash image (Intel HEX bytes) and resets. False (with `error`) if it does not fit the flash.
    bool loadFirmware(const std::vector<uint8_t>& image, std::string& error);
    bool hasFirmware() const { return hasFirmware_; }
    /// Power-on reset: registers, RAM and peripherals cleared, PC = 0, all pins inputs.
    void reset();

    // ---- pins: index = port × 8 + bit (port B = 0, C = 1, D = 2) --------------------------------------------------------
    /// Pin index for "PB5", "PD0/RXD", "PB5/RST"…; -1 if the name is not a port pin of this model.
    int pinIndex(const std::string& name) const;
    static std::string pinName(int index);
    /// Circuit inputs for the next run(): supply (VCC − GND), each pin's voltage, the AREF voltage.
    void setSupply(double volts) { vcc_ = volts; }
    double supply() const { return vcc_; }
    void setPinVoltage(int pin, double volts);
    void setAref(double volts) { aref_ = volts; }
    /// Fraction of the last run() each pin spent driven high, driven low and pulled up (the rest: high impedance).
    struct PinDrive {
        double high = 0, low = 0, pullup = 0;
    };
    PinDrive pinDrive(int pin) const;
    /// True while the pin is an output (driven by PORT, a timer compare output or the USART).
    bool pinIsOutput(int pin) const;
    /// Logic level the pin is driven to, or read from the circuit for an input.
    bool pinLevel(int pin) const;

    // ---- execution ----------------------------------------------------------------------------------------------------
    /// Runs `cycles` CPU clock cycles (peripherals included). Below 1.8 V supply the chip is held in reset.
    void run(uint64_t cycles);
    /// CLKPR system clock prescaler (1, 2, 4 … 256): the CPU runs at f_clk / clockDivider().
    int clockDivider() const { return 1 << clkps_; }
    uint64_t cycles() const { return cycles_; }
    uint64_t instructions() const { return instructions_; }
    bool sleeping() const { return sleeping_; }
    /// Set when the core met an instruction it cannot execute (it continues as a NOP) or ran off the flash.
    const std::string& fault() const { return fault_; }

    // ---- USART0 ------------------------------------------------------------------------------------------------------
    /// Bytes the firmware transmitted since the last call.
    std::string takeSerialOutput();
    const std::string& serialLog() const { return serialLog_; }
    /// Bytes for the receiver (delivered at the configured baud rate while RXEN0 is set).
    void feedSerialInput(const std::string& bytes);

    // ---- debug / test access -----------------------------------------------------------------------------------------
    uint8_t reg(int r) const { return data_[static_cast<size_t>(r)]; }
    uint16_t pc() const { return static_cast<uint16_t>(pc_); }
    uint16_t sp() const;
    uint8_t sreg() const { return data_[0x5F]; }
    uint8_t dataAt(uint16_t address) const { return address < data_.size() ? data_[address] : 0; }
    uint8_t eeprom(int address) const { return eeprom_[static_cast<size_t>(address) % eeprom_.size()]; }

private:
    struct Timer8 {
        uint16_t tccra, tccrb, tcnt, ocra, ocrb, timsk, tifr;
        int vecCompA, vecCompB, vecOvf;   // vector numbers
        int ocaPin, ocbPin;               // output compare pins
        bool timer2;                      // asynchronous-capable prescaler table (1, 8, 32, 64, 128, 256, 1024)
        uint8_t bitOvf, bitCompA, bitCompB;  // TIFR/TIMSK bit positions
        uint8_t count = 0, ocrA = 0, ocrB = 0;  // active (double-buffered) compare values
        bool down = false;
        bool ocaLevel = false, ocbLevel = false;
        uint32_t prescale = 0;
    };
    struct Timer16 {
        uint16_t count = 0, ocrA = 0, ocrB = 0;
        bool down = false;
        bool ocaLevel = false, ocbLevel = false;
        uint32_t prescale = 0;
        uint8_t temp = 0;
    };

    // memory
    uint8_t read(uint16_t address);
    void write(uint16_t address, uint8_t value);
    uint8_t ioRead(uint16_t address);
    void ioWrite(uint16_t address, uint8_t value);
    uint16_t fetch(uint32_t wordAddress) const;
    void push(uint8_t v);
    uint8_t pop();
    void pushPc(uint32_t pc);
    uint32_t popPc();
    void setSp(uint16_t sp);

    // execution
    int step();  // one instruction (or interrupt entry); returns cycles
    bool isTwoWord(uint16_t op) const;
    void skipNext();
    bool serviceInterrupt();
    int pendingVector() const;
    void clearVectorFlag(int vector);
    void tick(int cycles);

    // peripherals
    void tickTimer8(Timer8& t, int cycles);
    void timer8Count(Timer8& t);
    void tickTimer16(int cycles);
    void timer16Count();
    void tickAdc(int cycles);
    void tickUsart(int cycles);
    void tickTiny85Timer1(int cycles);
    double adcInput(int mux) const;
    void updatePins();
    void accumulate();  // books the time since the last pin change
    int portBase(int port) const;  // data address of PINx (PINx, DDRx, PORTx follow)
    void onLevelChange(int pin, bool level);

    McuModel model_;
    std::vector<uint8_t> flash_;
    std::vector<uint8_t> data_;
    std::vector<uint8_t> eeprom_;
    uint32_t pc_ = 0;
    uint32_t flashWords_ = 0;
    uint16_t ramEnd_ = 0;
    bool hasFirmware_ = false;
    bool sleeping_ = false;
    int interruptDelay_ = 0;  // instructions to run before the next interrupt (after SEI / RETI)
    uint64_t cycles_ = 0, instructions_ = 0;
    std::string fault_;
    int clkps_ = 0, clkpceWindow_ = 0, eempeWindow_ = 0;
    double vcc_ = 0, aref_ = -1;
    bool inReset_ = true;

    // pins (3 ports × 8)
    std::array<double, 24> pinVolts_{};
    std::array<bool, 24> inputLevel_{};
    std::array<int, 24> pinState_{};  // 0 hi-Z, 1 pull-up, 2 low, 3 high
    std::array<std::array<uint64_t, 4>, 24> stateCycles_{};
    std::array<bool, 24> levelNow_{};
    uint64_t runStart_ = 0, lastBook_ = 0;
    std::array<PinDrive, 24> lastDrive_{};

    Timer8 t0_{}, t2_{};
    Timer16 t1_{};
    // ADC
    int adcRemaining_ = -1;  // cycles until the conversion completes, -1 idle
    bool adcFirst_ = true;
    uint16_t adcResult_ = 0;
    // USART0
    std::deque<uint8_t> txQueue_;  // UDR0 buffer (at most 1)
    bool txShifting_ = false;
    uint16_t txFrame_ = 0;  // bits LSB first incl. start/stop
    int txBits_ = 0, txBitIndex_ = 0;
    int64_t txBitCycles_ = 0, txBitLeft_ = 0;
    bool txLine_ = true;
    std::deque<uint8_t> rxInput_;
    int64_t rxLeft_ = -1;
    uint8_t rxByte_ = 0;
    std::string serialOut_, serialLog_;
    // ATtiny85 Timer1
    uint32_t t1PrescaleTiny_ = 0;
};

}  // namespace sieda
