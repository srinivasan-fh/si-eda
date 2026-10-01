// SiEDA Core — SPICE-class circuit simulator.
// Modified Nodal Analysis, Newton–Raphson for non-linear devices (diode, LED, BJT, MOSFET,
// saturating op-amp), backward-Euler companion models for C and L in transient analysis.
#pragma once

#include <map>
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
    double voltage = 0;  // V across pin 1 → pin 2 (or V_CE / V_DS)
};

struct DcResult {
    bool converged = false;
    std::string error;
    int iterations = 0;
    std::vector<double> netVoltages;  // indexed by Schematic net index
    std::vector<DeviceReading> devices;
};

struct TransientResult {
    bool ok = false;
    std::string error;
    std::vector<double> time;
    std::vector<std::vector<double>> netVoltages;  // [net][sample]
    std::map<int, std::vector<double>> currents;   // component id → current samples
};

class Simulator {
public:
    explicit Simulator(const Schematic& schematic);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;
    DcResult dcOperatingPoint();
    TransientResult transient(double tStop, double tStep);

    struct Element;  // implementation detail (public so helpers in the .cpp can use it)

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
};

}  // namespace sieda
