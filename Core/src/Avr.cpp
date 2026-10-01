#include "sieda/Avr.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>

namespace sieda {

// ===================================================================== models, Intel HEX

std::optional<McuModel> mcuModelForPart(const std::string& partName) {
    std::string n;
    for (char c : partName)
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (n.rfind("atmega328", 0) == 0 || n == "arduinouno" || n == "arduinonano") return McuModel::ATmega328P;
    if (n.rfind("attiny85", 0) == 0) return McuModel::ATtiny85;
    return std::nullopt;
}

std::string mcuModelName(McuModel model) { return model == McuModel::ATmega328P ? "ATmega328P" : "ATtiny85"; }

double defaultMcuClock(McuModel model) { return model == McuModel::ATmega328P ? 16e6 : 8e6; }

namespace {
int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}
}  // namespace

HexImage parseIntelHex(const std::string& text, size_t maxBytes) {
    HexImage img;
    uint32_t base = 0;
    bool sawEof = false, sawData = false;
    size_t lineNo = 0, pos = 0;
    while (pos < text.size()) {
        size_t end = text.find('\n', pos);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(pos, end - pos);
        pos = end + 1;
        ++lineNo;
        while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back()))) line.pop_back();
        size_t start = 0;
        while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start]))) ++start;
        line = line.substr(start);
        if (line.empty()) continue;
        if (line[0] != ':' || line.size() < 11 || (line.size() - 1) % 2 != 0) {
            img.error = "Line " + std::to_string(lineNo) + " is not an Intel HEX record.";
            return img;
        }
        std::vector<uint8_t> rec;
        for (size_t i = 1; i + 1 < line.size(); i += 2) {
            int hi = hexDigit(line[i]), lo = hexDigit(line[i + 1]);
            if (hi < 0 || lo < 0) {
                img.error = "Line " + std::to_string(lineNo) + " has a non-hex character.";
                return img;
            }
            rec.push_back(static_cast<uint8_t>(hi * 16 + lo));
        }
        size_t count = rec[0];
        if (rec.size() != count + 5) {
            img.error = "Line " + std::to_string(lineNo) + ": record length does not match its byte count.";
            return img;
        }
        uint8_t sum = 0;
        for (uint8_t b : rec) sum = static_cast<uint8_t>(sum + b);
        if (sum != 0) {
            img.error = "Line " + std::to_string(lineNo) + ": checksum error.";
            return img;
        }
        uint32_t address = (static_cast<uint32_t>(rec[1]) << 8) | rec[2];
        uint8_t type = rec[3];
        if (type == 0x00) {
            uint32_t at = base + address;
            if (at + count > maxBytes) {
                img.error = "The image does not fit the flash (" + std::to_string(at + count) + " bytes).";
                return img;
            }
            if (img.bytes.size() < at + count) img.bytes.resize(at + count, 0xFF);
            for (size_t i = 0; i < count; ++i) img.bytes[at + i] = rec[4 + i];
            sawData = true;
        } else if (type == 0x01) {
            sawEof = true;
            break;
        } else if (type == 0x02 && count == 2) {
            base = ((static_cast<uint32_t>(rec[4]) << 8) | rec[5]) << 4;
        } else if (type == 0x04 && count == 2) {
            base = ((static_cast<uint32_t>(rec[4]) << 8) | rec[5]) << 16;
        }  // 03 / 05: start address, ignored
    }
    if (!sawData) img.error = "The file contains no program data.";
    else if (!sawEof) img.error = "The file has no end-of-file record (truncated?).";
    return img;
}

std::string toIntelHex(const std::vector<uint8_t>& bytes) {
    std::string out;
    char buf[64];
    uint32_t upper = 0;
    for (size_t at = 0; at < bytes.size(); at += 16) {
        if ((at >> 16) != upper) {
            upper = static_cast<uint32_t>(at >> 16);
            uint8_t sum = static_cast<uint8_t>(2 + 4 + (upper >> 8) + (upper & 0xFF));
            std::snprintf(buf, sizeof buf, ":02000004%04X%02X\n", upper, static_cast<uint8_t>(-sum) & 0xFF);
            out += buf;
        }
        size_t n = std::min<size_t>(16, bytes.size() - at);
        uint16_t addr = static_cast<uint16_t>(at & 0xFFFF);
        uint8_t sum = static_cast<uint8_t>(n + (addr >> 8) + (addr & 0xFF));
        std::snprintf(buf, sizeof buf, ":%02X%04X00", static_cast<unsigned>(n), addr);
        out += buf;
        for (size_t i = 0; i < n; ++i) {
            std::snprintf(buf, sizeof buf, "%02X", bytes[at + i]);
            out += buf;
            sum = static_cast<uint8_t>(sum + bytes[at + i]);
        }
        std::snprintf(buf, sizeof buf, "%02X\n", static_cast<uint8_t>(-sum) & 0xFF);
        out += buf;
    }
    out += ":00000001FF\n";
    return out;
}

// ===================================================================== register map

namespace {
// SREG bits
constexpr uint8_t C_ = 0x01, Z_ = 0x02, N_ = 0x04, V_ = 0x08, S_ = 0x10, H_ = 0x20, T_ = 0x40, I_ = 0x80;
constexpr uint16_t SPL = 0x5D, SPH = 0x5E, SREG = 0x5F;

// ATmega328P (data-space addresses)
namespace m328 {
constexpr uint16_t PINB = 0x23;
constexpr uint16_t TIFR0 = 0x35, TIFR1 = 0x36, TIFR2 = 0x37, PCIFR = 0x3B, EIFR = 0x3C, EIMSK = 0x3D;
constexpr uint16_t EECR = 0x3F, EEDR = 0x40, EEARL = 0x41, EEARH = 0x42;
constexpr uint16_t TCCR0A = 0x44, TCCR0B = 0x45, TCNT0 = 0x46, OCR0A = 0x47, OCR0B = 0x48;
constexpr uint16_t CLKPR = 0x61, PCICR = 0x68, EICRA = 0x69, PCMSK0 = 0x6B;
constexpr uint16_t TIMSK0 = 0x6E, TIMSK1 = 0x6F, TIMSK2 = 0x70;
constexpr uint16_t ADCL = 0x78, ADCH = 0x79, ADCSRA = 0x7A, ADMUX = 0x7C;
constexpr uint16_t TCCR1A = 0x80, TCCR1B = 0x81, TCCR1C = 0x82, TCNT1L = 0x84, TCNT1H = 0x85, ICR1L = 0x86,
                   ICR1H = 0x87, OCR1AL = 0x88, OCR1AH = 0x89, OCR1BL = 0x8A, OCR1BH = 0x8B;
constexpr uint16_t TCCR2A = 0xB0, TCCR2B = 0xB1, TCNT2 = 0xB2, OCR2A = 0xB3, OCR2B = 0xB4;
constexpr uint16_t UCSR0A = 0xC0, UCSR0B = 0xC1, UCSR0C = 0xC2, UBRR0L = 0xC4, UBRR0H = 0xC5, UDR0 = 0xC6;
// vector numbers
constexpr int vINT0 = 1, vINT1 = 2, vPCINT0 = 3, vT2COMPA = 7, vT2COMPB = 8, vT2OVF = 9, vT1CAPT = 10, vT1COMPA = 11,
              vT1COMPB = 12, vT1OVF = 13, vT0COMPA = 14, vT0COMPB = 15, vT0OVF = 16, vRX = 18, vUDRE = 19, vTX = 20,
              vADC = 21, vEE = 22;
// pins (port*8 + bit)
constexpr int PB1 = 1, PB2 = 2, PB3 = 3, PD1 = 17, PD3 = 19, PD5 = 21, PD6 = 22;
}  // namespace m328

// ATtiny85
namespace t85 {
constexpr uint16_t ADCL = 0x24, ADCH = 0x25, ADCSRA = 0x26, ADMUX = 0x27;
constexpr uint16_t PCMSK = 0x35, PINB = 0x36;
constexpr uint16_t EECR = 0x3C, EEDR = 0x3D, EEARL = 0x3E, EEARH = 0x3F;
constexpr uint16_t CLKPR = 0x46, OCR0B = 0x48, OCR0A = 0x49, TCCR0A = 0x4A, OCR1B = 0x4B, GTCCR = 0x4C, OCR1C = 0x4D,
                   OCR1A = 0x4E, TCNT1 = 0x4F, TCCR1 = 0x50, TCNT0 = 0x52, TCCR0B = 0x53, MCUCR = 0x55;
constexpr uint16_t TIFR = 0x58, TIMSK = 0x59, GIFR = 0x5A, GIMSK = 0x5B;
constexpr int vINT0 = 1, vPCINT0 = 2, vT1COMPA = 3, vT1OVF = 4, vT0OVF = 5, vEE = 6, vADC = 8, vT1COMPB = 9,
              vT0COMPA = 10, vT0COMPB = 11;
constexpr int PB0 = 0, PB1 = 1, PB2 = 2;
}  // namespace t85

const int kAdcPrescale[8] = {2, 2, 4, 8, 16, 32, 64, 128};
}  // namespace

// ===================================================================== construction, reset

AvrMcu::AvrMcu(McuModel model) : model_(model) {
    const bool mega = model == McuModel::ATmega328P;
    flash_.assign(mega ? 32768 : 8192, 0xFF);
    flashWords_ = static_cast<uint32_t>(flash_.size() / 2);
    data_.assign(mega ? 0x900 : 0x260, 0);
    ramEnd_ = static_cast<uint16_t>(data_.size() - 1);
    eeprom_.assign(mega ? 1024 : 512, 0xFF);
    if (mega) {
        t0_ = Timer8{m328::TCCR0A, m328::TCCR0B, m328::TCNT0, m328::OCR0A, m328::OCR0B, m328::TIMSK0, m328::TIFR0,
                     m328::vT0COMPA, m328::vT0COMPB, m328::vT0OVF, m328::PD6, m328::PD5, false, 0, 1, 2};
        t2_ = Timer8{m328::TCCR2A, m328::TCCR2B, m328::TCNT2, m328::OCR2A, m328::OCR2B, m328::TIMSK2, m328::TIFR2,
                     m328::vT2COMPA, m328::vT2COMPB, m328::vT2OVF, m328::PB3, m328::PD3, true, 0, 1, 2};
    } else {
        t0_ = Timer8{t85::TCCR0A, t85::TCCR0B, t85::TCNT0, t85::OCR0A, t85::OCR0B, t85::TIMSK, t85::TIFR,
                     t85::vT0COMPA, t85::vT0COMPB, t85::vT0OVF, t85::PB0, t85::PB1, false, 1, 4, 3};
    }
    reset();
}

bool AvrMcu::loadFirmware(const std::vector<uint8_t>& image, std::string& error) {
    if (image.size() > flash_.size()) {
        error = "The firmware is " + std::to_string(image.size()) + " bytes; the " + mcuModelName(model_) + " has " +
                std::to_string(flash_.size()) + " bytes of flash.";
        return false;
    }
    std::fill(flash_.begin(), flash_.end(), 0xFF);
    std::copy(image.begin(), image.end(), flash_.begin());
    hasFirmware_ = !image.empty();
    reset();
    return true;
}

void AvrMcu::reset() {
    std::fill(data_.begin(), data_.end(), 0);
    pc_ = 0;
    setSp(ramEnd_);
    sleeping_ = false;
    interruptDelay_ = 0;
    clkps_ = model_ == McuModel::ATtiny85 ? 0 : 0;
    clkpceWindow_ = eempeWindow_ = 0;
    t0_.count = t0_.ocrA = t0_.ocrB = 0;
    t0_.down = t0_.ocaLevel = t0_.ocbLevel = false;
    t0_.prescale = 0;
    t2_.count = t2_.ocrA = t2_.ocrB = 0;
    t2_.down = t2_.ocaLevel = t2_.ocbLevel = false;
    t2_.prescale = 0;
    t1_ = Timer16{};
    t1PrescaleTiny_ = 0;
    adcRemaining_ = -1;
    adcFirst_ = true;
    txQueue_.clear();
    txShifting_ = false;
    txLine_ = true;
    rxLeft_ = -1;
    if (model_ == McuModel::ATmega328P) data_[m328::UCSR0A] = 0x20;  // UDRE0
    if (model_ == McuModel::ATmega328P) data_[m328::UCSR0C] = 0x06;  // 8N1
    if (model_ == McuModel::ATtiny85) data_[t85::OCR1C] = 0xFF;
    updatePins();
}

uint16_t AvrMcu::sp() const { return static_cast<uint16_t>(data_[SPL] | (data_[SPH] << 8)); }

void AvrMcu::setSp(uint16_t sp) {
    data_[SPL] = static_cast<uint8_t>(sp & 0xFF);
    data_[SPH] = static_cast<uint8_t>(sp >> 8);
}

// ===================================================================== pins

int AvrMcu::pinIndex(const std::string& name) const {
    std::string n;
    for (char c : name) {
        if (c == '/' || c == ' ') break;
        n += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    if (n.size() != 3 || n[0] != 'P' || n[2] < '0' || n[2] > '7') return -1;
    int bit = n[2] - '0';
    int port = n[1] == 'B' ? 0 : n[1] == 'C' ? 1 : n[1] == 'D' ? 2 : -1;
    if (port < 0) return -1;
    if (model_ == McuModel::ATtiny85 && (port != 0 || bit > 5)) return -1;
    return port * 8 + bit;
}

std::string AvrMcu::pinName(int index) {
    if (index < 0 || index >= 24) return "";
    return std::string("P") + "BCD"[index / 8] + static_cast<char>('0' + index % 8);
}

int AvrMcu::portBase(int port) const {
    if (model_ == McuModel::ATtiny85) return port == 0 ? t85::PINB : -1;
    return m328::PINB + 3 * port;
}

void AvrMcu::setPinVoltage(int pin, double volts) {
    if (pin < 0 || pin >= 24) return;
    pinVolts_[static_cast<size_t>(pin)] = volts;
    // Schmitt-trigger input: high above 0.6 VCC, low below 0.3 VCC.
    bool old = inputLevel_[static_cast<size_t>(pin)];
    bool level = old;
    if (volts >= 0.6 * vcc_) level = true;
    else if (volts <= 0.3 * vcc_) level = false;
    inputLevel_[static_cast<size_t>(pin)] = level;
    if (level != old && !pinIsOutput(pin)) {
        levelNow_[static_cast<size_t>(pin)] = level;
        onLevelChange(pin, level);
    }
}

bool AvrMcu::pinIsOutput(int pin) const { return pinState_[static_cast<size_t>(pin)] >= 2; }

bool AvrMcu::pinLevel(int pin) const {
    if (pin < 0 || pin >= 24) return false;
    int s = pinState_[static_cast<size_t>(pin)];
    if (s >= 2) return s == 3;
    return inputLevel_[static_cast<size_t>(pin)];
}

AvrMcu::PinDrive AvrMcu::pinDrive(int pin) const {
    if (pin < 0 || pin >= 24) return {};
    return lastDrive_[static_cast<size_t>(pin)];
}

void AvrMcu::accumulate() {
    uint64_t span = cycles_ - lastBook_;
    if (span == 0) return;
    for (size_t p = 0; p < 24; ++p) stateCycles_[p][static_cast<size_t>(pinState_[p])] += span;
    lastBook_ = cycles_;
}

void AvrMcu::updatePins() {
    accumulate();
    const bool mega = model_ == McuModel::ATmega328P;
    for (int pin = 0; pin < 24; ++pin) {
        int port = pin / 8, bit = pin % 8;
        int base = portBase(port);
        int state = 0;
        if (base >= 0 && !inReset_) {
            bool ddr = data_[static_cast<size_t>(base + 1)] >> bit & 1;
            bool portBit = data_[static_cast<size_t>(base + 2)] >> bit & 1;
            state = ddr ? (portBit ? 3 : 2) : (portBit ? 1 : 0);
            // Timer output-compare overrides (only while the pin is an output).
            auto oc = [&](uint16_t tccra, int shift, bool level) {
                int com = data_[tccra] >> shift & 3;
                if (com != 0 && ddr) state = level ? 3 : 2;
            };
            if (mega) {
                if (pin == t0_.ocaPin) oc(t0_.tccra, 6, t0_.ocaLevel);
                if (pin == t0_.ocbPin) oc(t0_.tccra, 4, t0_.ocbLevel);
                if (pin == t2_.ocaPin) oc(t2_.tccra, 6, t2_.ocaLevel);
                if (pin == t2_.ocbPin) oc(t2_.tccra, 4, t2_.ocbLevel);
                if (pin == m328::PB1) oc(m328::TCCR1A, 6, t1_.ocaLevel);
                if (pin == m328::PB2) oc(m328::TCCR1A, 4, t1_.ocbLevel);
                uint8_t ucsrb = data_[m328::UCSR0B];
                if (pin == m328::PD1 && (ucsrb & 0x08)) state = txLine_ ? 3 : 2;  // TXEN0: TXD is an output
                if (pin == 16 && (ucsrb & 0x10)) state = portBit ? 1 : 0;      // RXEN0: RXD is an input
            } else {
                if (pin == t0_.ocaPin) oc(t0_.tccra, 6, t0_.ocaLevel);
                if (pin == t0_.ocbPin) oc(t0_.tccra, 4, t0_.ocbLevel);
                if (pin == t85::PB1 && (data_[t85::TCCR1] & 0x40) && (data_[t85::TCCR1] >> 4 & 3) && ddr)
                    state = t1_.ocaLevel ? 3 : 2;
            }
        }
        if (state != pinState_[static_cast<size_t>(pin)]) {
            bool before = pinLevel(pin);
            pinState_[static_cast<size_t>(pin)] = state;
            bool after = pinLevel(pin);
            if (before != after) {
                levelNow_[static_cast<size_t>(pin)] = after;
                onLevelChange(pin, after);
            }
        }
    }
}

void AvrMcu::onLevelChange(int pin, bool level) {
    if (model_ == McuModel::ATmega328P) {
        // INT0 = PD2, INT1 = PD3: ISCn1:0 = 01 any change, 10 falling, 11 rising (00 = low level, polled).
        for (int k = 0; k < 2; ++k) {
            if (pin != 18 + k) continue;
            int isc = data_[m328::EICRA] >> (2 * k) & 3;
            if ((isc == 1) || (isc == 2 && !level) || (isc == 3 && level)) data_[m328::EIFR] |= static_cast<uint8_t>(1 << k);
        }
        // Pin change: PCINT0-7 = PB0-7, 8-14 = PC0-6, 16-23 = PD0-7.
        int group = pin / 8;
        if (data_[static_cast<size_t>(m328::PCMSK0 + group)] >> (pin % 8) & 1)
            data_[m328::PCIFR] |= static_cast<uint8_t>(1 << group);
    } else {
        if (pin == t85::PB2) {
            int isc = data_[t85::MCUCR] & 3;
            if ((isc == 1) || (isc == 2 && !level) || (isc == 3 && level)) data_[t85::GIFR] |= 0x40;
        }
        if (pin < 6 && (data_[t85::PCMSK] >> pin & 1)) data_[t85::GIFR] |= 0x20;
    }
}

// ===================================================================== memory

uint16_t AvrMcu::fetch(uint32_t wordAddress) const {
    uint32_t w = wordAddress % flashWords_;
    return static_cast<uint16_t>(flash_[2 * w] | (flash_[2 * w + 1] << 8));
}

uint8_t AvrMcu::read(uint16_t address) {
    if (address < 0x20) return data_[address];
    if (address < 0x100) return ioRead(address);
    if (address <= ramEnd_) return data_[address];
    return 0;
}

void AvrMcu::write(uint16_t address, uint8_t value) {
    if (address < 0x20) {
        data_[address] = value;
    } else if (address < 0x100) {
        ioWrite(address, value);
    } else if (address <= ramEnd_) {
        data_[address] = value;
    }
}

void AvrMcu::push(uint8_t v) {
    uint16_t s = sp();
    if (s <= ramEnd_) data_[s] = v;
    setSp(static_cast<uint16_t>(s - 1));
}

uint8_t AvrMcu::pop() {
    uint16_t s = static_cast<uint16_t>(sp() + 1);
    setSp(s);
    return s <= ramEnd_ ? data_[s] : 0;
}

void AvrMcu::pushPc(uint32_t pc) {
    push(static_cast<uint8_t>(pc & 0xFF));
    push(static_cast<uint8_t>((pc >> 8) & 0xFF));
}

uint32_t AvrMcu::popPc() {
    uint32_t hi = pop();
    uint32_t lo = pop();
    return (hi << 8) | lo;
}

uint8_t AvrMcu::ioRead(uint16_t a) {
    const bool mega = model_ == McuModel::ATmega328P;
    // PINx: the pin levels.
    for (int port = 0; port < (mega ? 3 : 1); ++port) {
        if (a == portBase(port)) {
            uint8_t v = 0;
            for (int bit = 0; bit < 8; ++bit)
                if (pinLevel(port * 8 + bit)) v |= static_cast<uint8_t>(1 << bit);
            if (!mega) v &= 0x3F;
            return v;
        }
    }
    if (a == t0_.tcnt) return t0_.count;
    if (mega) {
        if (a == t2_.tcnt) return t2_.count;
        if (a == m328::TCNT1L) {
            t1_.temp = static_cast<uint8_t>(t1_.count >> 8);
            return static_cast<uint8_t>(t1_.count & 0xFF);
        }
        if (a == m328::TCNT1H || a == m328::ICR1H || a == m328::OCR1AH || a == m328::OCR1BH) {
            if (a == m328::TCNT1H) return t1_.temp;
            return data_[a];
        }
        if (a == m328::UDR0) {
            data_[m328::UCSR0A] &= static_cast<uint8_t>(~0x80);  // reading clears RXC0
            return rxByte_;
        }
        if (a == m328::ADCL) return static_cast<uint8_t>(data_[m328::ADMUX] & 0x20 ? (adcResult_ << 6) & 0xFF : adcResult_ & 0xFF);
        if (a == m328::ADCH) return static_cast<uint8_t>(data_[m328::ADMUX] & 0x20 ? adcResult_ >> 2 : adcResult_ >> 8);
    } else {
        if (a == t85::TCNT1) return static_cast<uint8_t>(t1_.count);
        if (a == t85::ADCL) return static_cast<uint8_t>(data_[t85::ADMUX] & 0x20 ? (adcResult_ << 6) & 0xFF : adcResult_ & 0xFF);
        if (a == t85::ADCH) return static_cast<uint8_t>(data_[t85::ADMUX] & 0x20 ? adcResult_ >> 2 : adcResult_ >> 8);
    }
    return data_[a];
}

void AvrMcu::ioWrite(uint16_t a, uint8_t v) {
    const bool mega = model_ == McuModel::ATmega328P;
    // Port registers.
    for (int port = 0; port < (mega ? 3 : 1); ++port) {
        int base = portBase(port);
        if (a == base) {  // writing 1 to PINx toggles PORTx
            data_[static_cast<size_t>(base + 2)] ^= v;
            updatePins();
            return;
        }
        if (a == base + 1 || a == base + 2) {
            data_[a] = v;
            updatePins();
            return;
        }
    }
    // Interrupt flag registers: writing 1 clears.
    auto clearFlags = [&](uint16_t reg) {
        if (a != reg) return false;
        data_[a] &= static_cast<uint8_t>(~v);
        return true;
    };
    if (mega) {
        if (clearFlags(m328::TIFR0) || clearFlags(m328::TIFR1) || clearFlags(m328::TIFR2) || clearFlags(m328::PCIFR) ||
            clearFlags(m328::EIFR))
            return;
        if (a == m328::TCNT0) { t0_.count = v; return; }
        if (a == m328::TCNT2) { t2_.count = v; return; }
        if (a == m328::OCR0A || a == m328::OCR0B || a == m328::OCR2A || a == m328::OCR2B) {
            data_[a] = v;
            Timer8& t = (a == m328::OCR0A || a == m328::OCR0B) ? t0_ : t2_;
            int wgm = (data_[t.tccra] & 3) | ((data_[t.tccrb] >> 1) & 4);
            bool pwm = wgm == 1 || wgm == 3 || wgm == 5 || wgm == 7;
            if (!pwm) {  // immediate in normal / CTC mode, double-buffered in PWM modes
                if (a == t.ocra) t.ocrA = v;
                else t.ocrB = v;
            }
            return;
        }
        if (a == m328::TCCR0A || a == m328::TCCR2A || a == m328::TCCR1A) {
            data_[a] = v;
            updatePins();
            return;
        }
        if (a == m328::TCCR0B || a == m328::TCCR2B) {  // FOCnA/B strobes are not latched
            data_[a] = v & 0x0F;
            return;
        }
        if (a == m328::TCCR1C) return;
        // 16-bit Timer1 registers through the TEMP latch: high byte first, the low-byte write commits.
        if (a == m328::TCNT1H || a == m328::OCR1AH || a == m328::OCR1BH || a == m328::ICR1H) {
            t1_.temp = v;
            data_[a] = v;
            return;
        }
        if (a == m328::TCNT1L) {
            t1_.count = static_cast<uint16_t>(v | (t1_.temp << 8));
            return;
        }
        if (a == m328::OCR1AL || a == m328::OCR1BL || a == m328::ICR1L) {
            data_[a] = v;
            data_[a + 1] = t1_.temp;
            int wgm = (data_[m328::TCCR1A] & 3) | ((data_[m328::TCCR1B] >> 1) & 0x0C);
            bool buffered = !(wgm == 0 || wgm == 4 || wgm == 12);
            uint16_t value = static_cast<uint16_t>(v | (t1_.temp << 8));
            if (!buffered || a == m328::ICR1L) {
                if (a == m328::OCR1AL) t1_.ocrA = value;
                if (a == m328::OCR1BL) t1_.ocrB = value;
            }
            return;
        }
        if (a == m328::UDR0) {
            if (data_[m328::UCSR0B] & 0x08) {
                if (txQueue_.empty()) txQueue_.push_back(v);
                data_[m328::UCSR0A] &= static_cast<uint8_t>(~0x20);  // UDRE0 = 0 until the shifter takes it
            }
            return;
        }
        if (a == m328::UCSR0A) {  // TXC0 clears by writing 1; U2X0 and MPCM0 are writable
            uint8_t keep = data_[a] & 0xFC;
            if (v & 0x40) keep &= static_cast<uint8_t>(~0x40);
            data_[a] = static_cast<uint8_t>(keep | (v & 0x03));
            return;
        }
        if (a == m328::UCSR0B) {
            data_[a] = v;
            updatePins();
            return;
        }
        if (a == m328::ADCSRA) {
            uint8_t flagsCleared = (v & 0x10) ? static_cast<uint8_t>(data_[a] & ~0x10) : data_[a];
            data_[a] = static_cast<uint8_t>((v & ~0x10) | (flagsCleared & 0x10));
            if ((v & 0x80) == 0) {
                adcRemaining_ = -1;
                adcFirst_ = true;
                data_[a] &= static_cast<uint8_t>(~0x40);
            } else if ((v & 0x40) && adcRemaining_ < 0) {
                adcRemaining_ = (adcFirst_ ? 25 : 13) * kAdcPrescale[v & 7];
            }
            if (adcRemaining_ >= 0) data_[a] |= 0x40;  // ADSC reads 1 until the conversion completes
            return;
        }
        if (a == m328::CLKPR) {
            if (v == 0x80) clkpceWindow_ = 4;
            else if (clkpceWindow_ > 0 && (v & 0x80) == 0) clkps_ = std::min(8, static_cast<int>(v & 0x0F));
            data_[a] = static_cast<uint8_t>(clkps_);
            return;
        }
        if (a == m328::EECR) {
            if (v & 0x04) eempeWindow_ = 4;
            uint16_t addr = static_cast<uint16_t>((data_[m328::EEARL] | (data_[m328::EEARH] << 8)) % eeprom_.size());
            if ((v & 0x02) && eempeWindow_ > 0) {
                eeprom_[addr] = data_[m328::EEDR];
                v &= static_cast<uint8_t>(~0x02);
            }
            if (v & 0x01) {
                data_[m328::EEDR] = eeprom_[addr];
                v &= static_cast<uint8_t>(~0x01);
            }
            data_[a] = v & 0x3C;
            return;
        }
    } else {
        if (clearFlags(t85::TIFR) || clearFlags(t85::GIFR)) return;
        if (a == t85::TCNT0) { t0_.count = v; return; }
        if (a == t85::TCNT1) { t1_.count = v; return; }
        if (a == t85::OCR0A || a == t85::OCR0B) {
            data_[a] = v;
            int wgm = (data_[t85::TCCR0A] & 3) | ((data_[t85::TCCR0B] >> 1) & 4);
            if (!(wgm == 1 || wgm == 3 || wgm == 5 || wgm == 7)) {
                if (a == t85::OCR0A) t0_.ocrA = v;
                else t0_.ocrB = v;
            }
            return;
        }
        if (a == t85::TCCR0A || a == t85::TCCR1 || a == t85::GTCCR) {
            data_[a] = v;
            updatePins();
            return;
        }
        if (a == t85::TCCR0B) {
            data_[a] = v & 0x0F;
            return;
        }
        if (a == t85::ADCSRA) {
            uint8_t flagsCleared = (v & 0x10) ? static_cast<uint8_t>(data_[a] & ~0x10) : data_[a];
            data_[a] = static_cast<uint8_t>((v & ~0x10) | (flagsCleared & 0x10));
            if ((v & 0x80) == 0) {
                adcRemaining_ = -1;
                adcFirst_ = true;
                data_[a] &= static_cast<uint8_t>(~0x40);
            } else if ((v & 0x40) && adcRemaining_ < 0) {
                adcRemaining_ = (adcFirst_ ? 25 : 13) * kAdcPrescale[v & 7];
            }
            if (adcRemaining_ >= 0) data_[a] |= 0x40;  // ADSC reads 1 until the conversion completes
            return;
        }
        if (a == t85::CLKPR) {
            if (v == 0x80) clkpceWindow_ = 4;
            else if (clkpceWindow_ > 0 && (v & 0x80) == 0) clkps_ = std::min(8, static_cast<int>(v & 0x0F));
            data_[a] = static_cast<uint8_t>(clkps_);
            return;
        }
        if (a == t85::EECR) {
            if (v & 0x04) eempeWindow_ = 4;
            uint16_t addr = static_cast<uint16_t>((data_[t85::EEARL] | (data_[t85::EEARH] << 8)) % eeprom_.size());
            if ((v & 0x02) && eempeWindow_ > 0) {
                eeprom_[addr] = data_[t85::EEDR];
                v &= static_cast<uint8_t>(~0x02);
            }
            if (v & 0x01) {
                data_[t85::EEDR] = eeprom_[addr];
                v &= static_cast<uint8_t>(~0x01);
            }
            data_[a] = v & 0x3C;
            return;
        }
    }
    data_[a] = v;
}

// ===================================================================== interrupts

int AvrMcu::pendingVector() const {
    auto on = [&](uint16_t flagReg, uint16_t maskReg, int bit) {
        return (data_[flagReg] >> bit & 1) && (data_[maskReg] >> bit & 1);
    };
    if (model_ == McuModel::ATmega328P) {
        using namespace m328;
        uint8_t eimsk = data_[EIMSK];
        for (int k = 0; k < 2; ++k) {
            if (!(eimsk >> k & 1)) continue;
            int isc = data_[EICRA] >> (2 * k) & 3;
            if (isc == 0 ? !pinLevel(18 + k) : (data_[EIFR] >> k & 1)) return vINT0 + k;
        }
        for (int g = 0; g < 3; ++g)
            if ((data_[PCIFR] >> g & 1) && (data_[PCICR] >> g & 1)) return vPCINT0 + g;
        if (on(TIFR2, TIMSK2, 1)) return vT2COMPA;
        if (on(TIFR2, TIMSK2, 2)) return vT2COMPB;
        if (on(TIFR2, TIMSK2, 0)) return vT2OVF;
        if (on(TIFR1, TIMSK1, 5)) return vT1CAPT;
        if (on(TIFR1, TIMSK1, 1)) return vT1COMPA;
        if (on(TIFR1, TIMSK1, 2)) return vT1COMPB;
        if (on(TIFR1, TIMSK1, 0)) return vT1OVF;
        if (on(TIFR0, TIMSK0, 1)) return vT0COMPA;
        if (on(TIFR0, TIMSK0, 2)) return vT0COMPB;
        if (on(TIFR0, TIMSK0, 0)) return vT0OVF;
        uint8_t a = data_[UCSR0A], b = data_[UCSR0B];
        if ((a & 0x80) && (b & 0x80)) return vRX;
        if ((a & 0x20) && (b & 0x20)) return vUDRE;
        if ((a & 0x40) && (b & 0x40)) return vTX;
        if ((data_[ADCSRA] & 0x18) == 0x18) return vADC;
        if ((data_[EECR] & 0x08) && !(data_[EECR] & 0x02)) return vEE;
    } else {
        using namespace t85;
        if ((data_[GIMSK] & 0x40) && ((data_[MCUCR] & 3) == 0 ? !pinLevel(PB2) : (data_[GIFR] & 0x40))) return vINT0;
        if ((data_[GIMSK] & 0x20) && (data_[GIFR] & 0x20)) return vPCINT0;
        if (on(TIFR, TIMSK, 6)) return vT1COMPA;
        if (on(TIFR, TIMSK, 2)) return vT1OVF;
        if (on(TIFR, TIMSK, 1)) return vT0OVF;
        if ((data_[EECR] & 0x08) && !(data_[EECR] & 0x02)) return vEE;
        if ((data_[ADCSRA] & 0x18) == 0x18) return vADC;
        if (on(TIFR, TIMSK, 5)) return vT1COMPB;
        if (on(TIFR, TIMSK, 4)) return vT0COMPA;
        if (on(TIFR, TIMSK, 3)) return vT0COMPB;
    }
    return 0;
}

void AvrMcu::clearVectorFlag(int v) {
    if (model_ == McuModel::ATmega328P) {
        using namespace m328;
        switch (v) {
            case vINT0: data_[EIFR] &= static_cast<uint8_t>(~1); break;
            case vINT1: data_[EIFR] &= static_cast<uint8_t>(~2); break;
            case vPCINT0: case vPCINT0 + 1: case vPCINT0 + 2:
                data_[PCIFR] &= static_cast<uint8_t>(~(1 << (v - vPCINT0)));
                break;
            case vT2COMPA: data_[TIFR2] &= static_cast<uint8_t>(~2); break;
            case vT2COMPB: data_[TIFR2] &= static_cast<uint8_t>(~4); break;
            case vT2OVF: data_[TIFR2] &= static_cast<uint8_t>(~1); break;
            case vT1CAPT: data_[TIFR1] &= static_cast<uint8_t>(~0x20); break;
            case vT1COMPA: data_[TIFR1] &= static_cast<uint8_t>(~2); break;
            case vT1COMPB: data_[TIFR1] &= static_cast<uint8_t>(~4); break;
            case vT1OVF: data_[TIFR1] &= static_cast<uint8_t>(~1); break;
            case vT0COMPA: data_[TIFR0] &= static_cast<uint8_t>(~2); break;
            case vT0COMPB: data_[TIFR0] &= static_cast<uint8_t>(~4); break;
            case vT0OVF: data_[TIFR0] &= static_cast<uint8_t>(~1); break;
            case vTX: data_[UCSR0A] &= static_cast<uint8_t>(~0x40); break;
            case vADC: data_[ADCSRA] &= static_cast<uint8_t>(~0x10); break;
            default: break;  // RX, UDRE and EE_READY are level-triggered
        }
    } else {
        using namespace t85;
        switch (v) {
            case vINT0: data_[GIFR] &= static_cast<uint8_t>(~0x40); break;
            case vPCINT0: data_[GIFR] &= static_cast<uint8_t>(~0x20); break;
            case vT1COMPA: data_[TIFR] &= static_cast<uint8_t>(~0x40); break;
            case vT1COMPB: data_[TIFR] &= static_cast<uint8_t>(~0x20); break;
            case vT1OVF: data_[TIFR] &= static_cast<uint8_t>(~0x04); break;
            case vT0OVF: data_[TIFR] &= static_cast<uint8_t>(~0x02); break;
            case vT0COMPA: data_[TIFR] &= static_cast<uint8_t>(~0x10); break;
            case vT0COMPB: data_[TIFR] &= static_cast<uint8_t>(~0x08); break;
            case vADC: data_[ADCSRA] &= static_cast<uint8_t>(~0x10); break;
            default: break;
        }
    }
}

bool AvrMcu::serviceInterrupt() {
    if (!(data_[SREG] & I_) || interruptDelay_ > 0) return false;
    int v = pendingVector();
    if (v == 0) return false;
    clearVectorFlag(v);
    sleeping_ = false;
    pushPc(pc_);
    data_[SREG] &= static_cast<uint8_t>(~I_);
    pc_ = model_ == McuModel::ATmega328P ? static_cast<uint32_t>(v * 2) : static_cast<uint32_t>(v);
    return true;
}

// ===================================================================== peripherals

void AvrMcu::tick(int n) {
    cycles_ += static_cast<uint64_t>(n);
    if (clkpceWindow_ > 0) clkpceWindow_ = std::max(0, clkpceWindow_ - n);
    if (eempeWindow_ > 0) eempeWindow_ = std::max(0, eempeWindow_ - n);
    tickTimer8(t0_, n);
    if (model_ == McuModel::ATmega328P) {
        tickTimer8(t2_, n);
        tickTimer16(n);
        tickUsart(n);
    } else {
        tickTiny85Timer1(n);
    }
    tickAdc(n);
}

void AvrMcu::tickTimer8(Timer8& t, int cycles) {
    int cs = data_[t.tccrb] & 7;
    if (cs == 0) return;
    static const int normal[8] = {0, 1, 8, 64, 256, 1024, 0, 0};
    static const int async[8] = {0, 1, 8, 32, 64, 128, 256, 1024};
    int div = t.timer2 ? async[cs] : normal[cs];
    if (div == 0) return;  // external clock: not modelled
    t.prescale += static_cast<uint32_t>(cycles);
    while (t.prescale >= static_cast<uint32_t>(div)) {
        t.prescale -= static_cast<uint32_t>(div);
        timer8Count(t);
    }
}

void AvrMcu::timer8Count(Timer8& t) {
    uint8_t a = data_[t.tccra];
    int wgm = (a & 3) | ((data_[t.tccrb] >> 1) & 4);
    int comA = a >> 6 & 3, comB = a >> 4 & 3;
    const bool phase = wgm == 1 || wgm == 5;
    const bool fast = wgm == 3 || wgm == 7;
    uint8_t top = (wgm == 2 || wgm == 5 || wgm == 7) ? (fast || phase ? t.ocrA : data_[t.ocra]) : 0xFF;
    if (wgm == 2) top = t.ocrA;
    uint8_t& flags = data_[t.tifr];
    bool pinsChanged = false;
    auto setA = [&](bool level) {
        if (t.ocaLevel != level) {
            t.ocaLevel = level;
            pinsChanged = true;
        }
    };
    auto setB = [&](bool level) {
        if (t.ocbLevel != level) {
            t.ocbLevel = level;
            pinsChanged = true;
        }
    };
    if (phase) {
        if (!t.down) {
            if (t.count >= top) {
                t.down = true;
                t.count = static_cast<uint8_t>(top - (top > 0 ? 1 : 0));
                t.ocrA = data_[t.ocra];  // OCR update at TOP
                t.ocrB = data_[t.ocrb];
            } else {
                ++t.count;
            }
        } else {
            if (t.count == 0) {
                t.down = false;
                t.count = 1;
            } else {
                --t.count;
                if (t.count == 0) flags |= static_cast<uint8_t>(1 << t.bitOvf);  // TOV at BOTTOM
            }
        }
        if (t.count == t.ocrA && wgm == 1) {
            flags |= static_cast<uint8_t>(1 << t.bitCompA);
            if (comA == 2) setA(t.down);
            else if (comA == 3) setA(!t.down);
        } else if (t.count == t.ocrA && wgm == 5) {
            flags |= static_cast<uint8_t>(1 << t.bitCompA);
            if (comA == 1) setA(!t.ocaLevel);
        }
        if (t.count == t.ocrB) {
            flags |= static_cast<uint8_t>(1 << t.bitCompB);
            if (comB == 2) setB(t.down);
            else if (comB == 3) setB(!t.down);
        }
    } else {
        bool wrapped = false;
        if (t.count == top) {
            t.count = 0;
            wrapped = true;
            if (fast) {
                flags |= static_cast<uint8_t>(1 << t.bitOvf);  // TOV at TOP
                t.ocrA = data_[t.ocra];
                t.ocrB = data_[t.ocrb];
            } else if (top == 0xFF) {
                flags |= static_cast<uint8_t>(1 << t.bitOvf);  // normal / CTC: TOV at MAX
            }
        } else {
            ++t.count;
            if (t.count == 0) flags |= static_cast<uint8_t>(1 << t.bitOvf);
        }
        if (fast && wrapped) {  // BOTTOM: non-inverting outputs set, inverting cleared
            if (comA == 2) setA(true);
            if (comA == 3) setA(false);
            if (comB == 2) setB(true);
            if (comB == 3) setB(false);
        }
        if (t.count == t.ocrA) {
            flags |= static_cast<uint8_t>(1 << t.bitCompA);
            if (fast) {
                if (comA == 2) setA(false);
                else if (comA == 3) setA(true);
                else if (comA == 1 && wgm == 7) setA(!t.ocaLevel);
            } else {
                if (comA == 1) setA(!t.ocaLevel);
                else if (comA == 2) setA(false);
                else if (comA == 3) setA(true);
            }
        }
        if (t.count == t.ocrB) {
            flags |= static_cast<uint8_t>(1 << t.bitCompB);
            if (fast) {
                if (comB == 2) setB(false);
                else if (comB == 3) setB(true);
            } else {
                if (comB == 1) setB(!t.ocbLevel);
                else if (comB == 2) setB(false);
                else if (comB == 3) setB(true);
            }
        }
    }
    if (pinsChanged) updatePins();
}

void AvrMcu::tickTimer16(int cycles) {
    int cs = data_[m328::TCCR1B] & 7;
    static const int div[8] = {0, 1, 8, 64, 256, 1024, 0, 0};
    if (div[cs] == 0) return;
    t1_.prescale += static_cast<uint32_t>(cycles);
    while (t1_.prescale >= static_cast<uint32_t>(div[cs])) {
        t1_.prescale -= static_cast<uint32_t>(div[cs]);
        timer16Count();
    }
}

void AvrMcu::timer16Count() {
    using namespace m328;
    uint8_t a = data_[TCCR1A];
    int wgm = (a & 3) | ((data_[TCCR1B] >> 1) & 0x0C);
    int comA = a >> 6 & 3, comB = a >> 4 & 3;
    uint16_t icr = static_cast<uint16_t>(data_[ICR1L] | (data_[ICR1H] << 8));
    uint16_t top = 0xFFFF;
    switch (wgm) {
        case 1: case 5: top = 0x00FF; break;
        case 2: case 6: top = 0x01FF; break;
        case 3: case 7: top = 0x03FF; break;
        case 4: case 9: case 11: case 15: top = t1_.ocrA; break;
        case 8: case 10: case 12: case 14: top = icr; break;
        default: top = 0xFFFF;
    }
    if (wgm == 4) top = t1_.ocrA;
    const bool phase = wgm == 1 || wgm == 2 || wgm == 3 || wgm == 8 || wgm == 9 || wgm == 10 || wgm == 11;
    const bool fast = wgm == 5 || wgm == 6 || wgm == 7 || wgm == 14 || wgm == 15;
    uint8_t& flags = data_[TIFR1];
    bool changed = false;
    auto setA = [&](bool l) { if (t1_.ocaLevel != l) { t1_.ocaLevel = l; changed = true; } };
    auto setB = [&](bool l) { if (t1_.ocbLevel != l) { t1_.ocbLevel = l; changed = true; } };
    uint16_t bufA = static_cast<uint16_t>(data_[OCR1AL] | (data_[OCR1AH] << 8));
    uint16_t bufB = static_cast<uint16_t>(data_[OCR1BL] | (data_[OCR1BH] << 8));
    if (phase) {
        if (!t1_.down) {
            if (t1_.count >= top) {
                t1_.down = true;
                if (top > 0) --t1_.count;
                if (wgm == 10 || wgm == 11 || wgm == 1 || wgm == 2 || wgm == 3) { t1_.ocrA = bufA; t1_.ocrB = bufB; }
            } else {
                ++t1_.count;
            }
        } else {
            if (t1_.count == 0) {
                t1_.down = false;
                t1_.count = 1;
            } else {
                --t1_.count;
                if (t1_.count == 0) {
                    flags |= 0x01;
                    if (wgm == 8 || wgm == 9) { t1_.ocrA = bufA; t1_.ocrB = bufB; }
                }
            }
        }
        if (t1_.count == t1_.ocrA) {
            flags |= 0x02;
            if (comA == 2) setA(t1_.down);
            else if (comA == 3) setA(!t1_.down);
            else if (comA == 1 && (wgm == 9 || wgm == 11)) setA(!t1_.ocaLevel);
        }
        if (t1_.count == t1_.ocrB) {
            flags |= 0x04;
            if (comB == 2) setB(t1_.down);
            else if (comB == 3) setB(!t1_.down);
        }
        if ((wgm == 8 || wgm == 10) && t1_.count == icr) flags |= 0x20;
    } else {
        bool wrapped = false;
        if (t1_.count == top) {
            t1_.count = 0;
            wrapped = true;
            if (fast) {
                flags |= 0x01;
                t1_.ocrA = bufA;
                t1_.ocrB = bufB;
            } else if (top == 0xFFFF) {
                flags |= 0x01;
            }
            if (wgm == 12 || wgm == 14) flags |= 0x20;  // ICF1 at TOP when ICR1 is TOP
        } else {
            ++t1_.count;
            if (t1_.count == 0) flags |= 0x01;
        }
        if (fast && wrapped) {
            if (comA == 2) setA(true);
            if (comA == 3) setA(false);
            if (comB == 2) setB(true);
            if (comB == 3) setB(false);
        }
        if (t1_.count == t1_.ocrA) {
            flags |= 0x02;
            if (fast) {
                if (comA == 2) setA(false);
                else if (comA == 3) setA(true);
                else if (comA == 1 && wgm == 15) setA(!t1_.ocaLevel);
            } else {
                if (comA == 1) setA(!t1_.ocaLevel);
                else if (comA == 2) setA(false);
                else if (comA == 3) setA(true);
            }
        }
        if (t1_.count == t1_.ocrB) {
            flags |= 0x04;
            if (fast) {
                if (comB == 2) setB(false);
                else if (comB == 3) setB(true);
            } else {
                if (comB == 1) setB(!t1_.ocbLevel);
                else if (comB == 2) setB(false);
                else if (comB == 3) setB(true);
            }
        }
    }
    if (changed) updatePins();
}

void AvrMcu::tickTiny85Timer1(int cycles) {
    using namespace t85;
    uint8_t tccr1 = data_[TCCR1];
    int cs = tccr1 & 0x0F;
    if (cs == 0) return;
    uint32_t div = 1u << (cs - 1);
    t1PrescaleTiny_ += static_cast<uint32_t>(cycles);
    while (t1PrescaleTiny_ >= div) {
        t1PrescaleTiny_ -= div;
        uint8_t top = (tccr1 & 0xC0) ? data_[OCR1C] : 0xFF;
        bool changed = false;
        if (t1_.count >= top) {
            t1_.count = 0;
            data_[TIFR] |= 0x04;  // TOV1
            if ((tccr1 & 0x40) && (tccr1 >> 4 & 3) == 2 && !t1_.ocaLevel) {
                t1_.ocaLevel = true;
                changed = true;
            }
        } else {
            ++t1_.count;
        }
        if (t1_.count == data_[OCR1A]) {
            data_[TIFR] |= 0x40;
            if ((tccr1 & 0x40) && (tccr1 >> 4 & 3) == 2 && t1_.ocaLevel) {
                t1_.ocaLevel = false;
                changed = true;
            }
        }
        if (t1_.count == data_[OCR1B]) data_[TIFR] |= 0x20;
        if (changed) updatePins();
    }
}

double AvrMcu::adcInput(int mux) const {
    if (model_ == McuModel::ATmega328P) {
        if (mux <= 5) return pinVolts_[static_cast<size_t>(8 + mux)];
        if (mux == 8) return 0.314;  // temperature sensor ≈ 25 °C
        if (mux == 14) return 1.1;
        return 0.0;
    }
    static const int chan[4] = {5, 2, 4, 3};  // ADC0..3 = PB5, PB2, PB4, PB3
    if (mux <= 3) return pinVolts_[static_cast<size_t>(chan[mux])];
    if (mux == 12) return 1.1;
    if (mux == 15) return 0.3;
    return 0.0;
}

void AvrMcu::tickAdc(int cycles) {
    if (adcRemaining_ < 0) return;
    adcRemaining_ -= cycles;
    if (adcRemaining_ > 0) return;
    const bool mega = model_ == McuModel::ATmega328P;
    uint16_t admuxA = mega ? m328::ADMUX : t85::ADMUX, adcsraA = mega ? m328::ADCSRA : t85::ADCSRA;
    uint8_t admux = data_[admuxA];
    int refs = admux >> 6 & 3;
    double vref = vcc_;
    if (mega) {
        if (refs == 0) vref = aref_ > 0 ? aref_ : vcc_;
        else if (refs == 3) vref = 1.1;
    } else {
        if (refs == 1) vref = aref_ > 0 ? aref_ : vcc_;
        else if (refs >= 2) vref = (admux & 0x10) ? 2.56 : 1.1;
    }
    double vin = adcInput(admux & 0x0F);
    double code = vref > 0 ? std::floor(vin / vref * 1024.0) : 0;
    adcResult_ = static_cast<uint16_t>(std::clamp(code, 0.0, 1023.0));
    data_[adcsraA] |= 0x10;  // ADIF
    adcFirst_ = false;
    uint8_t adcsra = data_[adcsraA];
    if (adcsra & 0x20) {  // ADATE, free running
        adcRemaining_ = 13 * kAdcPrescale[adcsra & 7];
    } else {
        adcRemaining_ = -1;
        data_[adcsraA] &= static_cast<uint8_t>(~0x40);  // ADSC
    }
}

void AvrMcu::tickUsart(int cycles) {
    using namespace m328;
    uint8_t ucsra = data_[UCSR0A], ucsrb = data_[UCSR0B], ucsrc = data_[UCSR0C];
    uint16_t ubrr = static_cast<uint16_t>((data_[UBRR0L] | (data_[UBRR0H] << 8)) & 0x0FFF);
    int64_t bitCycles = static_cast<int64_t>((ucsra & 0x02) ? 8 : 16) * (ubrr + 1);
    int dataBits = 5 + (ucsrc >> 1 & 3);
    bool parity = (ucsrc >> 4 & 3) >= 2;
    int stopBits = (ucsrc & 0x08) ? 2 : 1;
    int frameBits = 1 + dataBits + (parity ? 1 : 0) + stopBits;

    int64_t left = cycles;
    while (left > 0) {
        if (!txShifting_) {
            if (!txQueue_.empty() && (ucsrb & 0x08)) {
                uint8_t byte = txQueue_.front();
                txQueue_.pop_front();
                serialOut_.push_back(static_cast<char>(byte));
                serialLog_.push_back(static_cast<char>(byte));
                if (serialLog_.size() > 65536) serialLog_.erase(0, serialLog_.size() - 65536);
                uint16_t frame = 0;  // start bit 0
                int pos = 1;
                int ones = 0;
                for (int i = 0; i < dataBits; ++i, ++pos) {
                    int b = byte >> i & 1;
                    ones += b;
                    frame |= static_cast<uint16_t>(b << pos);
                }
                if (parity) {
                    bool odd = (ucsrc >> 4 & 3) == 3;
                    int p = (ones & 1) ^ (odd ? 1 : 0);
                    frame |= static_cast<uint16_t>(p << pos++);
                }
                for (int i = 0; i < stopBits; ++i, ++pos) frame |= static_cast<uint16_t>(1 << pos);
                txFrame_ = frame;
                txBits_ = frameBits;
                txBitIndex_ = 0;
                txBitCycles_ = bitCycles;
                txBitLeft_ = bitCycles;
                txShifting_ = true;
                data_[UCSR0A] |= 0x20;  // UDR0 empty again
                if (txLine_) {
                    txLine_ = false;  // start bit
                    updatePins();
                }
            } else {
                break;
            }
        }
        int64_t d = std::min(left, txBitLeft_);
        txBitLeft_ -= d;
        left -= d;
        if (txBitLeft_ == 0) {
            ++txBitIndex_;
            if (txBitIndex_ >= txBits_) {
                txShifting_ = false;
                if (txQueue_.empty()) data_[UCSR0A] |= 0x40;  // TXC0
                if (!txLine_) {
                    txLine_ = true;
                    updatePins();
                }
            } else {
                bool level = txFrame_ >> txBitIndex_ & 1;
                txBitLeft_ = txBitCycles_;
                if (level != txLine_) {
                    txLine_ = level;
                    updatePins();
                }
            }
        }
    }
    // Receiver: bytes fed by the host arrive one frame time apart.
    if ((ucsrb & 0x10) && !rxInput_.empty()) {
        if (rxLeft_ < 0) rxLeft_ = bitCycles * frameBits;
        rxLeft_ -= cycles;
        if (rxLeft_ <= 0) {
            if (data_[UCSR0A] & 0x80) data_[UCSR0A] |= 0x08;  // DOR0: previous byte unread
            rxByte_ = rxInput_.front();
            rxInput_.pop_front();
            data_[UCSR0A] |= 0x80;  // RXC0
            rxLeft_ = -1;
        }
    }
}

std::string AvrMcu::takeSerialOutput() {
    std::string out;
    out.swap(serialOut_);
    return out;
}

void AvrMcu::feedSerialInput(const std::string& bytes) {
    for (char c : bytes) rxInput_.push_back(static_cast<uint8_t>(c));
}

// ===================================================================== run loop

void AvrMcu::run(uint64_t budget) {
    // Book the time per pin state for this run.
    for (auto& s : stateCycles_) s.fill(0);
    runStart_ = lastBook_ = cycles_;
    const bool powered = vcc_ >= 1.8 && hasFirmware_;
    if (!powered) {
        if (!inReset_) {
            inReset_ = true;
            updatePins();
        }
        cycles_ += budget;
    } else {
        if (inReset_) {  // power-on reset
            inReset_ = false;
            reset();
        }
        uint64_t end = cycles_ + budget;
        while (cycles_ < end) {
            if (sleeping_) {
                tick(1);
                if (pendingVector() != 0 && (data_[SREG] & I_)) {
                    sleeping_ = false;
                    if (serviceInterrupt()) tick(4);
                }
                continue;
            }
            int n = step();
            tick(n);
            if (interruptDelay_ > 0) {
                --interruptDelay_;
            } else if (serviceInterrupt()) {
                tick(4);
            }
        }
    }
    accumulate();
    uint64_t total = cycles_ - runStart_;
    for (size_t p = 0; p < 24; ++p) {
        PinDrive d;
        if (total > 0) {
            d.high = static_cast<double>(stateCycles_[p][3]) / static_cast<double>(total);
            d.low = static_cast<double>(stateCycles_[p][2]) / static_cast<double>(total);
            d.pullup = static_cast<double>(stateCycles_[p][1]) / static_cast<double>(total);
        }
        lastDrive_[p] = d;
    }
}

// ===================================================================== instruction set

bool AvrMcu::isTwoWord(uint16_t op) const {
    if ((op & 0xFE0F) == 0x9000 || (op & 0xFE0F) == 0x9200) return true;  // LDS / STS
    if ((op & 0xFE0E) == 0x940C || (op & 0xFE0E) == 0x940E) return true;  // JMP / CALL
    return false;
}

void AvrMcu::skipNext() { pc_ += isTwoWord(fetch(pc_)) ? 2 : 1; }

int AvrMcu::step() {
    ++instructions_;
    uint8_t* r = data_.data();
    uint8_t& sreg = data_[SREG];
    const uint16_t op = fetch(pc_);
    ++pc_;
    if (pc_ > flashWords_ && fault_.empty()) fault_ = "Program counter ran past the end of the flash.";
    pc_ %= flashWords_;

    auto setFlag = [&](uint8_t f, bool on) {
        if (on) sreg |= f;
        else sreg &= static_cast<uint8_t>(~f);
    };
    auto nzs = [&](uint8_t res) {
        setFlag(Z_, res == 0);
        setFlag(N_, res & 0x80);
        setFlag(S_, ((sreg & N_) != 0) != ((sreg & V_) != 0));
    };
    auto add = [&](uint8_t d, uint8_t s, bool carry) {
        uint8_t res = static_cast<uint8_t>(d + s + (carry ? 1 : 0));
        bool d7 = d & 0x80, s7 = s & 0x80, r7 = res & 0x80, d3 = d & 8, s3 = s & 8, r3 = res & 8;
        setFlag(H_, (d3 && s3) || (s3 && !r3) || (!r3 && d3));
        setFlag(V_, (d7 && s7 && !r7) || (!d7 && !s7 && r7));
        setFlag(C_, (d7 && s7) || (s7 && !r7) || (!r7 && d7));
        nzs(res);
        return res;
    };
    auto sub = [&](uint8_t d, uint8_t s, bool carry, bool keepZ) {
        uint8_t res = static_cast<uint8_t>(d - s - (carry ? 1 : 0));
        bool d7 = d & 0x80, s7 = s & 0x80, r7 = res & 0x80, d3 = d & 8, s3 = s & 8, r3 = res & 8;
        bool zOld = sreg & Z_;
        setFlag(H_, (!d3 && s3) || (s3 && r3) || (r3 && !d3));
        setFlag(V_, (d7 && !s7 && !r7) || (!d7 && s7 && r7));
        setFlag(C_, (!d7 && s7) || (s7 && r7) || (r7 && !d7));
        nzs(res);
        if (keepZ) setFlag(Z_, res == 0 && zOld);
        return res;
    };
    auto logic = [&](uint8_t res) {
        setFlag(V_, false);
        nzs(res);
        return res;
    };
    auto rd5 = [&]() { return (op >> 4) & 0x1F; };
    auto rr5 = [&]() { return (op & 0x0F) | ((op >> 5) & 0x10); };
    auto rd4 = [&]() { return 16 + ((op >> 4) & 0x0F); };
    auto k8 = [&]() { return static_cast<uint8_t>((op & 0x0F) | ((op >> 4) & 0xF0)); };
    auto Y = [&]() { return static_cast<uint16_t>(r[28] | (r[29] << 8)); };
    auto Z = [&]() { return static_cast<uint16_t>(r[30] | (r[31] << 8)); };
    auto setPair = [&](int lo, uint16_t v) {
        r[lo] = static_cast<uint8_t>(v & 0xFF);
        r[lo + 1] = static_cast<uint8_t>(v >> 8);
    };
    auto mulResult = [&](uint16_t res, bool carry) {
        r[0] = static_cast<uint8_t>(res & 0xFF);
        r[1] = static_cast<uint8_t>(res >> 8);
        setFlag(C_, carry);
        setFlag(Z_, res == 0);
    };

    switch (op >> 12) {
        case 0x0: {
            if (op == 0x0000) return 1;  // NOP
            switch (op >> 8) {
                case 0x01: {  // MOVW
                    int d = (op >> 4 & 0x0F) * 2, s = (op & 0x0F) * 2;
                    r[d] = r[s];
                    r[d + 1] = r[s + 1];
                    return 1;
                }
                case 0x02: {  // MULS
                    int d = 16 + (op >> 4 & 0x0F), s = 16 + (op & 0x0F);
                    int16_t res = static_cast<int16_t>(static_cast<int8_t>(r[d]) * static_cast<int8_t>(r[s]));
                    mulResult(static_cast<uint16_t>(res), res & 0x8000);
                    return 2;
                }
                case 0x03: {  // MULSU / FMUL / FMULS / FMULSU
                    int d = 16 + (op >> 4 & 7), s = 16 + (op & 7);
                    int kind = (op >> 7 & 1) << 1 | (op >> 3 & 1);
                    int32_t prod = 0;
                    if (kind == 0) prod = static_cast<int8_t>(r[d]) * static_cast<int32_t>(r[s]);  // MULSU
                    else if (kind == 1) prod = static_cast<int32_t>(r[d]) * r[s];                  // FMUL
                    else if (kind == 2) prod = static_cast<int8_t>(r[d]) * static_cast<int8_t>(r[s]);  // FMULS
                    else prod = static_cast<int8_t>(r[d]) * static_cast<int32_t>(r[s]);              // FMULSU
                    uint16_t res = static_cast<uint16_t>(prod);
                    if (kind == 0) {
                        mulResult(res, res & 0x8000);
                    } else {
                        bool c = res & 0x8000;
                        res = static_cast<uint16_t>(res << 1);
                        mulResult(res, c);
                    }
                    return 2;
                }
                default: break;
            }
            int d = rd5(), s = rr5();
            switch (op >> 10 & 3) {
                case 1: sub(r[d], r[s], sreg & C_, true); return 1;           // CPC
                case 2: r[d] = sub(r[d], r[s], sreg & C_, true); return 1;    // SBC
                case 3: r[d] = add(r[d], r[s], false); return 1;              // ADD / LSL
                default: break;
            }
            break;
        }
        case 0x1: {
            int d = rd5(), s = rr5();
            switch (op >> 10 & 3) {
                case 0:  // CPSE
                    if (r[d] == r[s]) {
                        bool two = isTwoWord(fetch(pc_));
                        skipNext();
                        return two ? 3 : 2;
                    }
                    return 1;
                case 1: sub(r[d], r[s], false, false); return 1;           // CP
                case 2: r[d] = sub(r[d], r[s], false, false); return 1;    // SUB
                default: r[d] = add(r[d], r[s], sreg & C_); return 1;      // ADC / ROL
            }
        }
        case 0x2: {
            int d = rd5(), s = rr5();
            switch (op >> 10 & 3) {
                case 0: r[d] = logic(r[d] & r[s]); return 1;  // AND
                case 1: r[d] = logic(r[d] ^ r[s]); return 1;  // EOR
                case 2: r[d] = logic(r[d] | r[s]); return 1;  // OR
                default: r[d] = r[s]; return 1;               // MOV
            }
        }
        case 0x3: sub(r[rd4()], k8(), false, false); return 1;                   // CPI
        case 0x4: r[rd4()] = sub(r[rd4()], k8(), sreg & C_, true); return 1;     // SBCI
        case 0x5: r[rd4()] = sub(r[rd4()], k8(), false, false); return 1;        // SUBI
        case 0x6: r[rd4()] = logic(r[rd4()] | k8()); return 1;                   // ORI
        case 0x7: r[rd4()] = logic(r[rd4()] & k8()); return 1;                   // ANDI
        case 0x8:
        case 0xA: {  // LDD / STD with displacement (Y or Z)
            int q = (op & 7) | ((op >> 7) & 0x18) | ((op >> 8) & 0x20);
            int d = rd5();
            uint16_t base = (op & 0x08) ? Y() : Z();
            uint16_t addr = static_cast<uint16_t>(base + q);
            if (op & 0x0200) write(addr, r[d]);
            else r[d] = read(addr);
            return 2;
        }
        case 0x9: {
            if ((op & 0xFC00) == 0x9C00) {  // MUL
                uint16_t res = static_cast<uint16_t>(r[rd5()] * r[rr5()]);
                mulResult(res, res & 0x8000);
                return 2;
            }
            if ((op & 0xFE00) == 0x9000 || (op & 0xFE00) == 0x9200) {  // loads / stores
                int d = rd5();
                bool store = op & 0x0200;
                switch (op & 0x0F) {
                    case 0x0: {  // LDS / STS
                        uint16_t addr = fetch(pc_);
                        ++pc_;
                        if (store) write(addr, r[d]);
                        else r[d] = read(addr);
                        return 2;
                    }
                    case 0x1: case 0x2: case 0x9: case 0xA: case 0xC: case 0xD: case 0xE: {
                        int idx = (op & 0x0F) >= 0xC ? 26 : ((op & 0x0F) >= 0x9 ? 28 : 30);
                        uint16_t p = static_cast<uint16_t>(r[idx] | (r[idx + 1] << 8));
                        int mode = op & 3;  // 0 plain (X only), 1 post-increment, 2 pre-decrement
                        if ((op & 0x0F) == 0xC) mode = 0;
                        if (mode == 2) --p;
                        if (store) write(p, r[d]);
                        else r[d] = read(p);
                        if (mode == 1) ++p;
                        if (mode != 0) setPair(idx, p);
                        return 2;
                    }
                    case 0x4: case 0x5:  // LPM Rd, Z / Z+   (store variants: XCH/LAS, not on these chips)
                    case 0x6: case 0x7:  // ELPM: no RAMPZ on these chips, same as LPM
                        if (!store) {
                            uint16_t z = Z();
                            r[d] = flash_[z % flash_.size()];
                            if (op & 1) setPair(30, static_cast<uint16_t>(z + 1));
                            return 3;
                        }
                        break;
                    case 0xF:
                        if (store) push(r[d]);
                        else r[d] = pop();
                        return 2;
                    default: break;
                }
                break;
            }
            if ((op & 0xFE08) == 0x9400 && (op & 0x0F) != 0x08) {  // one-operand ALU (1001 010d dddd 0xxx)
                int d = rd5();
                uint8_t v = r[d];
                switch (op & 0x0F) {
                    case 0x0: r[d] = logic(static_cast<uint8_t>(~v)); setFlag(C_, true); return 1;  // COM
                    case 0x1: {                                                                       // NEG
                        uint8_t res = static_cast<uint8_t>(0 - v);
                        setFlag(H_, ((res | v) & 0x08) != 0);
                        setFlag(V_, res == 0x80);
                        setFlag(C_, res != 0);
                        nzs(res);
                        r[d] = res;
                        return 1;
                    }
                    case 0x2: r[d] = static_cast<uint8_t>((v << 4) | (v >> 4)); return 1;  // SWAP
                    case 0x3: {                                                              // INC
                        uint8_t res = static_cast<uint8_t>(v + 1);
                        setFlag(V_, res == 0x80);
                        nzs(res);
                        r[d] = res;
                        return 1;
                    }
                    case 0x5: case 0x6: case 0x7: {  // ASR / LSR / ROR
                        bool c = v & 1;
                        uint8_t res;
                        if ((op & 0x0F) == 0x5) res = static_cast<uint8_t>((v >> 1) | (v & 0x80));
                        else if ((op & 0x0F) == 0x6) res = static_cast<uint8_t>(v >> 1);
                        else res = static_cast<uint8_t>((v >> 1) | ((sreg & C_) ? 0x80 : 0));
                        setFlag(C_, c);
                        setFlag(N_, res & 0x80);
                        setFlag(V_, ((res & 0x80) != 0) != c);
                        setFlag(S_, ((sreg & N_) != 0) != ((sreg & V_) != 0));
                        setFlag(Z_, res == 0);
                        r[d] = res;
                        return 1;
                    }
                    default: break;
                }
            }
            if ((op & 0xFE0F) == 0x940A) {  // DEC
                int d = rd5();
                uint8_t res = static_cast<uint8_t>(r[d] - 1);
                setFlag(V_, res == 0x7F);
                nzs(res);
                r[d] = res;
                return 1;
            }
            if ((op & 0xFF8F) == 0x9408) {  // BSET
                sreg |= static_cast<uint8_t>(1 << (op >> 4 & 7));
                if ((op >> 4 & 7) == 7) interruptDelay_ = 1;  // SEI: the next instruction runs first
                return 1;
            }
            if ((op & 0xFF8F) == 0x9488) {  // BCLR
                sreg &= static_cast<uint8_t>(~(1 << (op >> 4 & 7)));
                return 1;
            }
            switch (op) {
                case 0x9508: pc_ = popPc() % flashWords_; return 4;  // RET
                case 0x9518:                                        // RETI
                    pc_ = popPc() % flashWords_;
                    sreg |= I_;
                    interruptDelay_ = 1;
                    return 4;
                case 0x9588: sleeping_ = true; return 1;  // SLEEP
                case 0x9598: return 1;                    // BREAK
                case 0x95A8: return 1;                    // WDR
                case 0x95C8: case 0x95D8: r[0] = flash_[Z() % flash_.size()]; return 3;  // LPM / ELPM
                case 0x95E8: return 4;                    // SPM (self-programming: ignored)
                case 0x9409: case 0x9419: pc_ = Z() % flashWords_; return 2;             // IJMP / EIJMP
                case 0x9509: case 0x9519: pushPc(pc_); pc_ = Z() % flashWords_; return 3;  // ICALL / EICALL
                default: break;
            }
            if ((op & 0xFE0E) == 0x940C || (op & 0xFE0E) == 0x940E) {  // JMP / CALL
                uint32_t k = ((static_cast<uint32_t>(op & 0x01F0) >> 3 | (op & 1)) << 16) | fetch(pc_);
                ++pc_;
                if (op & 0x0002) {
                    pushPc(pc_);
                    pc_ = k % flashWords_;
                    return 4;
                }
                pc_ = k % flashWords_;
                return 3;
            }
            if ((op & 0xFE00) == 0x9600) {  // ADIW / SBIW
                int d = 24 + 2 * (op >> 4 & 3);
                uint8_t k = static_cast<uint8_t>((op & 0x0F) | (op >> 2 & 0x30));
                uint16_t v = static_cast<uint16_t>(r[d] | (r[d + 1] << 8));
                bool hi7 = r[d + 1] & 0x80;
                uint16_t res;
                if (op & 0x0100) {
                    res = static_cast<uint16_t>(v - k);
                    setFlag(V_, hi7 && !(res & 0x8000));
                    setFlag(C_, (res & 0x8000) && !hi7);
                } else {
                    res = static_cast<uint16_t>(v + k);
                    setFlag(V_, !hi7 && (res & 0x8000));
                    setFlag(C_, !(res & 0x8000) && hi7);
                }
                setFlag(N_, res & 0x8000);
                setFlag(Z_, res == 0);
                setFlag(S_, ((sreg & N_) != 0) != ((sreg & V_) != 0));
                setPair(d, res);
                return 2;
            }
            if ((op & 0xFC00) == 0x9800) {  // CBI / SBIC / SBI / SBIS (I/O 0..31)
                uint16_t addr = static_cast<uint16_t>(0x20 + (op >> 3 & 0x1F));
                int bit = op & 7;
                switch (op >> 8 & 3) {
                    case 0:  // CBI
                    case 2: {  // SBI
                        bool set = (op >> 8 & 3) == 2;
                        bool isPin = false;
                        for (int port = 0; port < 3; ++port) isPin |= addr == portBase(port);
                        if (isPin) {  // SBI on PINx toggles that one PORT bit; CBI on PINx has no effect
                            if (set) ioWrite(addr, static_cast<uint8_t>(1 << bit));
                        } else {
                            uint8_t v = read(addr);
                            v = set ? static_cast<uint8_t>(v | (1 << bit)) : static_cast<uint8_t>(v & ~(1 << bit));
                            // Flag registers clear by writing 1: only the addressed bit may be written.
                            write(addr, v);
                        }
                        return 2;
                    }
                    default: {  // SBIC / SBIS
                        bool bitSet = read(addr) >> bit & 1;
                        bool skip = ((op >> 8 & 3) == 3) ? bitSet : !bitSet;
                        if (skip) {
                            bool two = isTwoWord(fetch(pc_));
                            skipNext();
                            return two ? 3 : 2;
                        }
                        return 1;
                    }
                }
            }
            break;
        }
        case 0xB: {  // IN / OUT
            int d = rd5();
            uint16_t addr = static_cast<uint16_t>(0x20 + ((op & 0x0F) | ((op >> 5) & 0x30)));
            if (op & 0x0800) write(addr, r[d]);
            else r[d] = read(addr);
            return 1;
        }
        case 0xC: {  // RJMP
            int16_t k = static_cast<int16_t>(op << 4) >> 4;
            pc_ = static_cast<uint32_t>(static_cast<int32_t>(pc_) + k) % flashWords_;
            return 2;
        }
        case 0xD: {  // RCALL
            int16_t k = static_cast<int16_t>(op << 4) >> 4;
            pushPc(pc_);
            pc_ = static_cast<uint32_t>(static_cast<int32_t>(pc_) + k) % flashWords_;
            return 3;
        }
        case 0xE: r[rd4()] = k8(); return 1;  // LDI
        case 0xF: {
            if (!(op & 0x0800)) {  // BRBS / BRBC
                bool bitSet = sreg >> (op & 7) & 1;
                bool taken = (op & 0x0400) ? !bitSet : bitSet;
                if (taken) {
                    int k = static_cast<int8_t>(static_cast<uint8_t>((op >> 3) & 0x7F) << 1) >> 1;
                    pc_ = static_cast<uint32_t>(static_cast<int32_t>(pc_) + k) % flashWords_;
                    return 2;
                }
                return 1;
            }
            int d = rd5(), b = op & 7;
            switch (op >> 9 & 3) {
                case 0:  // BLD
                    if (sreg & T_) r[d] |= static_cast<uint8_t>(1 << b);
                    else r[d] &= static_cast<uint8_t>(~(1 << b));
                    return 1;
                case 1: setFlag(T_, r[d] >> b & 1); return 1;  // BST
                default: {                                       // SBRC / SBRS
                    bool bitSet = r[d] >> b & 1;
                    bool skip = (op & 0x0200) ? bitSet : !bitSet;
                    if (skip) {
                        bool two = isTwoWord(fetch(pc_));
                        skipNext();
                        return two ? 3 : 2;
                    }
                    return 1;
                }
            }
        }
        default: break;
    }
    if (fault_.empty()) {
        char buf[96];
        std::snprintf(buf, sizeof buf, "Unknown instruction 0x%04X at 0x%04X (executed as NOP).", op,
                      static_cast<unsigned>((pc_ + flashWords_ - 1) % flashWords_) * 2);
        fault_ = buf;
    }
    return 1;
}

}  // namespace sieda
