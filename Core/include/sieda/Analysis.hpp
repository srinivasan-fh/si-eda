// SiEDA Core — analyses built on the circuit simulator: parameter sweeps, Monte Carlo and worst-case tolerance
// analysis, FFT / THD of transient waveforms, and the JSON forms the C API returns for them and for AC / DC sweeps.
#pragma once

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/Simulator.hpp"

namespace sieda {

// ------------------------------------------------------------------ tolerances

/// Default tolerances of parts whose value states none.
struct ToleranceDefaults {
    double resistor = 0.01;   // 1 % thick-film SMD
    double capacitor = 0.10;  // X7R / film, 10 %
    double inductor = 0.20;   // power inductors, 20 %
};

/// Relative tolerance of a resistor, capacitor or inductor: a percentage in its value ("10k 1%", "100n ±5%") or the
/// kind's default. 0 for every other part (sources, semiconductors and fuses are taken at their nominal values).
double componentTolerance(const Component& c, const ToleranceDefaults& defaults, bool* fromValue = nullptr);

/// Deterministic pseudo-random numbers (SplitMix64) with portable uniform and normal deviates: the same seed gives the
/// same sequence on every platform and standard library.
class SeededRandom {
public:
    explicit SeededRandom(uint64_t seed) : state_(seed) {}
    uint64_t next();
    double uniform();  // [0, 1)
    double normal();   // mean 0, σ 1 (Box–Muller)

private:
    uint64_t state_;
    bool haveSpare_ = false;
    double spare_ = 0;
};

/// The quantity a tolerance analysis measures on one net.
struct Measurement {
    enum class Kind { DcVoltage, AcGainDb, AcF3db, AcPeakHz } kind = Kind::DcVoltage;
    int net = -1;   // schematic net index
    AcOptions ac;   // sweep for the AC measurements (measureNets is set to `net`)
};

/// Runs the measurement with the given value scale factors. nullopt with `error` when the analysis fails or the
/// quantity is undefined (no −3 dB point inside the sweep, …).
std::optional<double> measure(const Schematic& schematic, const Measurement& m, const std::map<int, double>& scale,
                              std::string& error);

struct ToleranceOptions {
    int runs = 100;            // Monte Carlo runs (0: worst case only)
    uint64_t seed = 1;         // RNG seed: equal seeds give identical runs
    bool gaussian = false;     // uniform within ±tol (default), or normal with σ = tol / 3 clipped at ±tol
    bool worstCase = true;     // sensitivity-based worst case (EVA)
    ToleranceDefaults defaults;
};

struct ToleranceEntry {
    int componentId = -1;
    double tolerance = 0;      // relative
    bool fromValue = false;    // stated in the value (else the default)
    double sensitivity = 0;    // change of the measurement for the part at +tolerance
};

struct ToleranceResult {
    bool ok = false;
    std::string error;
    double nominal = 0;
    std::vector<double> samples;  // measurement of each successful Monte Carlo run, in run order
    int failedRuns = 0;
    double min = 0, max = 0, mean = 0, sigma = 0;  // over the samples (σ: sample standard deviation)
    bool hasWorstCase = false;
    double worstMin = 0, worstMax = 0;
    std::map<int, double> worstMinScale, worstMaxScale;  // component id → value factor of each worst case
    std::vector<ToleranceEntry> parts;
};

/// Monte Carlo (seeded) and worst-case analysis of `m` over the tolerances of every resistor, capacitor and inductor.
ToleranceResult toleranceAnalysis(const Schematic& schematic, const Measurement& m, const ToleranceOptions& options);

// ------------------------------------------------------------------ parameter sweep

enum class SweepAnalysis { Dc, Ac, Transient };

struct ParamSweepOptions {
    int componentId = -1;
    std::vector<std::string> values;  // component value per run ("1k", "2k2", "10n", "TL072"…), at most 100
    SweepAnalysis analysis = SweepAnalysis::Dc;
    AcOptions ac;
    double tStop = 1e-3, tStep = 1e-6;
};

struct ParamSweepRun {
    std::string value;
    DcResult dc;
    AcResult ac;
    TransientResult transient;
    bool ok() const;
    std::string error() const;
};

/// Runs the analysis once per value of the swept component (on copies of the schematic).
std::vector<ParamSweepRun> parameterSweep(const Schematic& schematic, const ParamSweepOptions& options,
                                          std::string& error);

// ------------------------------------------------------------------ spectrum

struct Harmonic {
    int order = 1;
    double frequency = 0, amplitude = 0;  // peak amplitude
    double dbc = 0;                       // relative to the fundamental
};

struct SpectrumResult {
    bool ok = false;
    std::string error;
    double fundamentalHz = 0;
    double dc = 0;
    double thd = 0;                   // √(Σ A_n², n = 2…) / A_1 (ratio)
    int cycles = 0;                   // whole fundamental periods analysed
    double windowStart = 0, windowStop = 0;
    std::vector<double> frequency;    // single-sided bins
    std::vector<double> magnitudeDb;  // 20·log10(peak amplitude)
    std::vector<Harmonic> harmonics;  // fundamental first
};

/// FFT of a sampled waveform over the last whole number of fundamental periods after time `from` (coherent,
/// rectangular window), with the THD over orders 2…`harmonics`. `fundamentalHz` ≤ 0 detects the fundamental (the
/// strongest non-DC component). Start `from` after the circuit has settled: start-up transients leak into every bin.
SpectrumResult spectrum(const std::vector<double>& time, const std::vector<double>& values, double fundamentalHz,
                        int harmonics = 10, double from = 0);

// ------------------------------------------------------------------ JSON (C API)
// Options and results are documented in docs/SIMULATION.md and at the C declarations in sieda_c.h.

Json simulateAcJson(const Schematic& schematic, const Json& options);
Json simulateDcSweepJson(const Schematic& schematic, const Json& options);
Json simulateParamSweepJson(const Schematic& schematic, const Json& options);
Json simulateToleranceJson(const Schematic& schematic, const Json& options);
Json simulateFftJson(const Schematic& schematic, const Json& options);

}  // namespace sieda
