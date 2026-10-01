// SiEDA Core — SPICE-class circuit simulator.
// Modified Nodal Analysis, Newton–Raphson for non-linear devices (diode, LED, BJT, MOSFET,
// saturating op-amp), backward-Euler companion models for C and L in transient analysis.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sieda/Schematic.hpp"

namespace sieda {

/// Independent source waveform: "5", "5V", "SIN(off amp freq)", "PULSE(v1 v2 period [duty])".
struct SourceSpec {
    enum class Kind { DC, Sine, Pulse } kind = Kind::DC;
    double dc = 0, offset = 0, amplitude = 0, frequency = 0, v1 = 0, v2 = 0, period = 0, duty = 0.5;

    static std::optional<SourceSpec> parse(const std::string& text);
    double valueAt(double t) const;
};

struct DeviceReading {
    int componentId = -1;
    double current = 0;  // A, conventional current entering pin 1 (or collector/drain for transistors)
    double power = 0;    // W, dissipated (negative = delivered, i.e. sources)
    double voltage = 0;  // V across pin 1 → pin 2 (or V_CE / V_DS; regulator: V_in − V_out)
    // Regulator models: 0 regulating, 1 current limit, 2 off (would have to sink), 3 in dropout.
    int state = 0;
    int subIndex = 0;    // several elements per component (custom part loads): 0 = regulator, 1… = loads
};

struct DcResult {
    bool converged = false;
    std::string error;
    int iterations = 0;
    std::vector<double> netVoltages;  // indexed by Schematic net index
    std::vector<DeviceReading> devices;
};

/// A microcontroller after a (transient or live) run: what it did and why.
struct McuReport {
    int componentId = -1;
    std::string model;    // "ATmega328P"
    std::string status;   // "Running blink.hex at 16 MHz", "No firmware loaded…", "Held in reset…", fault text
    std::string serial;   // everything USART0 transmitted
    bool running = false;
    uint64_t cycles = 0;
    double clockHz = 0;
};

struct TransientResult {
    bool ok = false;
    std::string error;
    std::vector<double> time;
    std::vector<std::vector<double>> netVoltages;  // [net][sample]
    std::map<int, std::vector<double>> currents;   // component id → current samples
    std::map<int, std::vector<double>> powers;     // component id → dissipated power samples
    std::vector<McuReport> mcus;                   // microcontrollers with their firmware's serial output
};

class Simulator {
public:
    explicit Simulator(const Schematic& schematic);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;
    DcResult dcOperatingPoint();
    /// Transient analysis. Microcontrollers with firmware run alongside: each step executes the step's clock cycles,
    /// then drives every pin from the time it spent high, low or pulled up (25 Ω outputs, 35 kΩ pull-ups) and reads
    /// inputs and the ADC from the solved node voltages.
    TransientResult transient(double tStop, double tStep);

    // ---- incremental (live) simulation --------------------------------------------------------------------------------
    /// Starts at the t = 0 operating point. False with `error` when the circuit cannot be simulated.
    bool begin(std::string& error);
    /// Advances by one step of `h` seconds (firmware included).
    bool advance(double h, std::string& error);
    double time() const { return t_; }
    /// Net voltages (by schematic net index) and device readings at the current time.
    std::vector<double> netVoltages() const;
    const std::vector<DeviceReading>& deviceReadings() const { return lastReadings_; }
    /// Opens or closes a switch (push-buttons and toggles while the simulation runs).
    void setSwitch(int componentId, bool closed);
    /// Bytes for a microcontroller's USART receiver (serial monitor input).
    void feedSerial(int componentId, const std::string& bytes);
    std::vector<McuReport> mcuReports() const;

    struct Element;  // implementation detail (public so helpers in the .cpp can use it)
    struct McuState;

private:
    bool build(std::string& error);
    bool solve(double t, double h, std::vector<double>& x, int& iterations, double gminExtra, double sourceScale);
    void stamp(double t, double h, const std::vector<double>& x, double gminExtra, double sourceScale);
    std::vector<DeviceReading> readings(const std::vector<double>& x, double h) const;

    const Schematic& sch_;
    std::vector<int> netToNode_;  // net index → unknown index (-1 for ground)
    int nodeCount_ = 0;
    int unknowns_ = 0;
    std::vector<Element> elements_;
    std::vector<double> A_, b_;
    std::vector<std::unique_ptr<McuState>> mcus_;
    std::vector<double> x_;
    std::vector<DeviceReading> lastReadings_;
    double t_ = 0;
    bool started_ = false;

    void stepMcus(double h);
    void updateState();
};

}  // namespace sieda
