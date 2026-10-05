// SiEDA Core — small-signal noise analysis (SPICE ".NOISE"): output noise density of a node over a logarithmic
// frequency sweep, the input-referred density, integrated RMS noise and the parts that contribute it.
//
// Noise sources at the DC operating point: resistor thermal noise 4kT/R; diode shot 2qI and flicker KF·I^AF/f; BJT
// collector and base shot noise and base flicker noise; MOSFET / JFET channel thermal noise (8/3)kT·g_m and flicker
// noise; op-amp voltage and current noise densities from the value (EN=, IN=, FNC= 1/f corner). Each source is
// carried to the output by the adjoint of the AC system (one solve per frequency). Guide: docs/SIMULATION.md.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct NoiseOptions {
    double fStart = 10, fStop = 100e3;  // Hz
    int pointsPerDecade = 20;
    int outputNet = -1;                 // schematic net index
    int referenceNet = -1;              // output = V(out) − V(ref); -1 = ground
    /// Input source for input-referred noise: a component id, or -1 for the AC stimulus rule (the source with an "AC"
    /// value, else the first SIN / AC source). No input: the result has no input-referred density.
    int sourceId = -1;
    double temperature = 300.15;        // K (27 °C, as SPICE)
};

struct NoiseContribution {
    int componentId = -1;
    std::string kind;    // "thermal", "shot", "flicker", "voltage", "current"
    double rms = 0;      // its integrated output noise (V RMS) over the sweep
};

struct NoiseResult {
    bool ok = false;
    std::string error;
    std::vector<double> frequency;      // Hz
    std::vector<double> outputDensity;  // V/√Hz
    std::vector<double> inputDensity;   // V/√Hz or A/√Hz (inputIsCurrent); empty without an input source
    std::vector<double> gain;           // |output / input|; empty without an input source
    double outputRms = 0;               // √∫ S_out df over the sweep (V)
    double inputRms = 0;                // √∫ S_in df (V or A); 0 without an input source
    int inputSource = -1;
    bool inputIsCurrent = false;
    std::vector<NoiseContribution> contributions;  // largest first
};

/// Runs the noise analysis on a schematic.
NoiseResult noiseAnalysis(const Schematic& schematic, const NoiseOptions& options);

/// C API form: options {"output","reference"?,"source"?,"start","stop","pointsPerDecade"} (nets by name, the source by
/// reference); result documented at sieda_simulate_noise.
Json simulateNoiseJson(const Schematic& schematic, const Json& options);

}  // namespace sieda
