// SiEDA Core (internal) — semiconductor device equations for imported SPICE models: diode (with junction and
// diffusion charge, breakdown), Gummel–Poon BJT (level 1 subset), MOSFET levels 1 and 3 (Shichman–Hodges, THETA
// mobility reduction, body effect, overlap / junction / gate charge) and JFET. Each device gives the currents into
// its terminals and the charges stored at them as functions of the terminal voltages; the simulator differentiates
// them (Newton, AC admittance, transient companion models). Series resistances become separate resistors.
#pragma once

#include <array>
#include <memory>
#include <string>
#include <vector>

#include "sieda/SpiceModels.hpp"

namespace sieda::spicedev {

constexpr double kCharge = 1.602176634e-19;     // C
constexpr double kBoltzmann = 1.380649e-23;     // J/K
constexpr double kNominalTemp = 300.15;         // K (27 °C, the SPICE default)
constexpr double kVtNominal = kBoltzmann * kNominalTemp / kCharge;

/// A noise current source between two terminals of a device: PSD(f) = white + flicker / f (A²/Hz).
struct NoiseTerm {
    int a = 0, b = 1;
    double white = 0, flicker = 0;
    const char* label = "";  // "shot", "flicker", "thermal"
};

class Device {
public:
    virtual ~Device() = default;
    int terminals = 2;
    /// Series resistance in front of each terminal (0: none); the simulator inserts it with an internal node.
    std::array<double, 4> seriesR{{0, 0, 0, 0}};
    /// Currents flowing into the device at each terminal.
    virtual void currents(const double* v, double* i) const = 0;
    /// Whether the device stores charge (junction, diffusion, gate capacitance).
    virtual bool hasCharge() const = 0;
    /// Charge stored at each terminal (sums to zero).
    virtual void charges(const double* v, double* q) const = 0;
    /// Noise sources at the operating point `v`.
    virtual void noise(const double* v, std::vector<NoiseTerm>& out) const = 0;
};

/// Builds the device of a flattened D / Q / M / J primitive. nullptr with `error` for an unknown type.
std::shared_ptr<const Device> makeDevice(const SpicePrimitive& p, std::string& error);

/// SPICE depletion charge of a junction with zero-bias capacitance cj0, potential vj, grading m and forward-bias
/// coefficient fc (linearised above fc·vj).
double depletionCharge(double cj0, double vj, double m, double fc, double v);

}  // namespace sieda::spicedev
