// SiEDA Core — SPICE-class circuit simulator.
// Modified Nodal Analysis, Newton–Raphson for non-linear devices (diode, LED, BJT, MOSFET,
// saturating op-amp), backward-Euler companion models for C and L in transient analysis.
#pragma once

#include <cmath>
#include <complex>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "sieda/Schematic.hpp"

namespace sieda {

struct NoiseOptions;  // sieda/Noise.hpp
struct NoiseResult;

/// Independent source waveform: "5", "5V", "SIN(off amp freq)", "PULSE(v1 v2 period [duty])", each optionally followed
/// by a SPICE-style small-signal stimulus "AC mag [phase°]" ("0 AC 1", "SIN(0 1 1k) AC 1"; "AC 1" alone is DC 0).
struct SourceSpec {
    enum class Kind { DC, Sine, Pulse } kind = Kind::DC;
    double dc = 0, offset = 0, amplitude = 0, frequency = 0, v1 = 0, v2 = 0, period = 0, duty = 0.5;
    bool hasAc = false;  // the value carries an "AC mag [phase]" stimulus for AC analysis
    double acMagnitude = 0, acPhaseDeg = 0;
    // SPICE-standard waveforms. They are Kind::Pulse (levels v1 … v2, period 0 unless periodic) so code that only
    // needs a source's range keeps working; `shape` says how valueAt draws them:
    //   Spice  PULSE(v1 v2 td tr tf pw [per]) — 5 or more arguments (4 or fewer: the square wave above)
    //   Pwl    PWL(t1 v1 t2 v2 …), held before the first and after the last point
    //   Exp    EXP(v1 v2 td1 tau1 [td2 tau2])
    enum class Shape { Square, Spice, Pwl, Exp } shape = Shape::Square;
    double td = 0, tr = 0, tf = 0, pw = 0, tau1 = 0, td2 = 0, tau2 = 0;
    std::vector<double> pwl;  // t0 v0 t1 v1 …
    // SIN(vo va freq td theta phase) — the delay, damping and phase of a sine with more than three arguments.
    bool sineExtended = false;
    double sinDelay = 0, sinDamping = 0, sinPhaseDeg = 0;
    std::string spiceText;  // the SPICE-standard waveform as written (the netlist export passes it through)

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

/// AC small-signal analysis settings (a logarithmic sweep, SPICE ".AC DEC points fstart fstop").
struct AcOptions {
    double fStart = 1;   // Hz
    double fStop = 1e6;  // Hz
    int pointsPerDecade = 50;
    /// Component driven with the stimulus (its "AC mag phase" when the value has one, else 1 V / 1 A at 0°). -1: every
    /// source whose value has an "AC" stimulus; when none has, the first SIN or AC source with AC 1.
    int sourceId = -1;
    /// Nets (schematic net indices) to measure (bandwidth, peak, unity gain, phase margin); empty = every node.
    std::vector<int> measureNets;
};

/// Bode-plot readouts of one node. Frequencies outside the sweep are NaN.
struct AcMetrics {
    double lowFreqDb = 0;  // gain at the sweep start: the pass-band reference of a low-pass response
    double peakDb = 0, peakHz = 0;
    double f3dbHz = NAN;    // first frequency above the start where the gain is 3.01 dB (half power) below lowFreqDb
    double bwLowHz = NAN;   // half-power points either side of an interior peak (band-pass, resonance)
    double bwHighHz = NAN;
    double unityHz = NAN;         // where the gain falls through 0 dB
    double phaseMarginDeg = NAN;  // 180° + phase at unityHz, wrapped to (−180°, 180°]
};

struct AcResult {
    bool ok = false;
    std::string error;
    std::vector<int> stimulus;                                  // component ids that drive the circuit
    std::vector<double> frequency;                              // Hz
    std::vector<std::vector<std::complex<double>>> netPhasors;  // [net][point], per unit stimulus
    std::map<int, AcMetrics> metrics;                           // net index → readouts
    std::vector<double> dcNetVoltages;                          // the operating point the circuit is linearised at
};

/// Gain in dB (floored at −400 dB for a zero response).
double magnitudeDb(std::complex<double> h);
/// Phase in degrees along a sweep, unwrapped so neighbouring points never differ by more than 180°.
std::vector<double> unwrappedPhaseDeg(const std::vector<std::complex<double>>& h);
/// Readouts of a response sampled at ascending `freq`. `eval` (optional) re-solves the circuit at any frequency; it
/// refines the crossings and the peak to full precision. Without it they are interpolated (dB linear in log f).
AcMetrics acMetrics(const std::vector<double>& freq, const std::vector<std::complex<double>>& h,
                    const std::function<std::complex<double>(double)>& eval = nullptr);

/// DC sweep of an independent source's value (SPICE ".DC").
struct DcSweepResult {
    bool ok = false;
    std::string error;
    std::vector<double> values;                    // swept source value per point
    std::vector<std::vector<double>> netVoltages;  // [net][point]
    std::map<int, std::vector<double>> currents;   // component id → current per point
};

/// Transient settings beyond stop and step (docs/SIMULATION.md). The defaults are the established fixed-step
/// backward-Euler analysis.
struct TransientOptions {
    double tStop = 1e-3, tStep = 1e-6;  // s; with `adaptive`, tStep is the largest step unless maxStep is set
    bool trapezoidal = false;  // trapezoidal rule (second order, no numerical damping) instead of backward Euler
    bool adaptive = false;     // step size from the local truncation error, landing on every PULSE edge
    double reltol = 1e-3;      // relative LTE tolerance (adaptive)
    double vntol = 1e-6;       // absolute LTE tolerance, V (adaptive)
    double maxStep = 0;        // largest adaptive step; 0 = tStep
};

/// Stops a running simulation from any thread (the app's Stop button): transient steps, AC / DC sweep points and
/// Monte Carlo / parameter sweep runs check it and end with `kSimulationStopped`. Results are unchanged while it is
/// clear; it stays set until cleared (`requestSimulationStop(false)`).
inline constexpr const char* kSimulationStopped = "Simulation stopped.";
void requestSimulationStop(bool stop = true);
bool simulationStopRequested();

class Simulator {
public:
    explicit Simulator(const Schematic& schematic);
    ~Simulator();
    Simulator(const Simulator&) = delete;
    Simulator& operator=(const Simulator&) = delete;
    DcResult dcOperatingPoint();
    /// Builds the circuit without solving it: false with the reason when it cannot be simulated (an invalid value,
    /// an imported SPICE model that does not flatten or whose pin map does not fit the part).
    bool check(std::string& error);
    /// Transient analysis. Microcontrollers with firmware run alongside: each step executes the step's clock cycles,
    /// then drives every pin from the time it spent high, low or pulled up (25 Ω outputs, 35 kΩ pull-ups) and reads
    /// inputs and the ADC from the solved node voltages.
    TransientResult transient(double tStop, double tStep);
    /// Transient with an integration method and, optionally, adaptive time steps (Convergence.cpp). Circuits with
    /// microcontrollers keep fixed steps (the firmware runs in lockstep).
    TransientResult transient(const TransientOptions& options);
    /// AC small-signal analysis: every device linearised at the DC operating point (diode, BJT and MOSFET small-signal
    /// conductances; op-amps as a single-pole gain–bandwidth model), solved with complex MNA at each frequency.
    AcResult ac(const AcOptions& options);
    /// DC sweep of the independent source `componentId` from `start` to `stop` in steps of |step|.
    DcSweepResult dcSweep(int componentId, double start, double stop, double step);
    /// Small-signal noise analysis (sieda/Noise.hpp, Noise.cpp).
    NoiseResult noise(const NoiseOptions& options);
    /// Multiplies the values of resistors, capacitors and inductors by a factor per component id (tolerance analysis).
    /// Takes effect at the next analysis.
    void setValueScale(std::map<int, double> scale) { valueScale_ = std::move(scale); }
    /// Extra convergence aids when the standard strategies fail (docs/SIMULATION.md): a DC operating point that
    /// Newton, Gmin stepping and source stepping could not find is retried with pn-junction limiting and adaptive
    /// source stepping; a transient step that failed its sub-steps is retried with junction limiting and adaptive
    /// sub-steps. They only ever turn a failure into a result. Off by default (the design checks keep the
    /// established behaviour), on in the app's analyses, always on with imported SPICE models.
    void setConvergenceAids(bool on) { aids_ = on; }

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
    /// Newton from `x`, then Gmin stepping, then source stepping (the DC operating point strategy).
    bool operatingPoint(std::vector<double>& x, int& iterations);

    /// Last-resort DC strategy (Convergence.cpp): sources ramped from zero with a step that adapts to Newton's success.
    bool adaptiveSourceStepping(std::vector<double>& x, int& iterations);
    /// PULSE edges in (0, tStop]: the adaptive transient lands a step on each.
    std::vector<double> sourceBreakpoints(double tStop) const;

    // Imported SPICE models (SpiceBuild.cpp).
    bool spiceModelApplies(const Component& c) const;
    bool addSpiceModel(const Component& c, std::string& error);
    int newInternalNode();
    /// Op-amp value with any of SR=, P2=, VOH=, VOL=, ROUT=, AOL=, EN=, IN=: the dynamic macromodel.
    static bool opAmpMacromodelRequested(const std::string& value);
    bool addOpAmpMacromodel(const Component& c, double gbw, std::string& error);
    void stampModelElement(const Element& e, double t, double h, const std::vector<double>& x);
    void addModelAdmittance(const Element& e, double w, const std::vector<double>& x,
                            std::vector<std::complex<double>>& M) const;
    void updateCharges(Element& e) const;
    /// The complex MNA matrix at `freq`: the small-signal conductances G plus every reactive element at operating
    /// point x (shared by AC and noise analysis).
    void acMatrix(double freq, const std::vector<double>& G, const std::vector<double>& x,
                  std::vector<std::complex<double>>& M) const;
    DeviceReading modelReading(const Element& e, const std::vector<double>& x, double h) const;
    void terminalCurrents(const Element& e, const std::vector<double>& x, double h,
                          std::vector<std::pair<int, double>>& out) const;

    const Schematic& sch_;
    std::vector<int> netToNode_;
    std::vector<int> internalNodes_;   // unknowns that are node voltages inside imported models
    std::vector<char> isNode_;         // per unknown: a node voltage (Newton steps are damped)
    std::map<double, int> rails_;      // "dc:V" supplies of pin maps: voltage → internal node
    bool trap_ = false;                // trapezoidal integration (transient options)
    bool strictNewton_ = false;        // the circuit has imported models: stricter Newton convergence test
    bool limitJunctions_ = false;      // pnjlim on the built-in diodes and NPNs (new transient modes, retries)
    bool aids_ = false;                // setConvergenceAids
    bool noLimit_ = false;             // the AC / noise linearisation: devices at their exact operating point
    /// Junction limiting for a built-in diode or NPN's terminal voltages (Convergence.cpp).
    void limitBuiltinJunctions(const Element& e, std::array<double, 3>& v) const;
    /// A transient step from t0 over h that failed: 20, then 200 sub-steps with junction limiting (Convergence.cpp).
    bool retryWithLimiting(double t0, double h, std::string& error);
    double stepH_ = 0;                 // the step the state is being advanced over  // net index → unknown index (-1 for ground)
    int nodeCount_ = 0;
    int unknowns_ = 0;
    std::vector<Element> elements_;
    std::vector<double> A_, b_;
    std::vector<std::unique_ptr<McuState>> mcus_;
    std::vector<double> x_;
    std::vector<DeviceReading> lastReadings_;
    double t_ = 0;
    bool started_ = false;
    std::map<int, double> valueScale_;

    void stepMcus(double h);
    void updateState();
};

}  // namespace sieda
