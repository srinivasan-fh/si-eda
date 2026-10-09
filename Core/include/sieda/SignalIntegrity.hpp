// SiEDA Core — signal integrity of the routed board: per-net transmission-line analysis (stack-up impedance and delay of
// every track section, vias, driver / receiver models from IBIS or logic-family defaults, terminations found in the
// schematic) solved in the time domain, crosstalk between neighbouring tracks, and return-path continuity.
//
// The physics is the textbook lossless-line set: IPC-2141 impedance (Stackup.hpp), Hammerstad–Jensen effective
// permittivity, Bergeron's method of characteristics for the lines, Johnson's via and crosstalk approximations and the
// standard backward / forward coupling coefficients. It is a pre-layout / post-layout screening tool in the spirit of
// board-level SI checkers; it is not a full-wave 3D field solver (no loss, dispersion, skin effect or broadside coupling).
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sieda/Ibis.hpp"
#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

// ---- physics -------------------------------------------------------------------------------------------------------

/// Effective permittivity of a track: Hammerstad & Jensen (IEEE MTT-S 1980) for a microstrip of width w over height h,
///   εeff = (εr + 1)/2 + (εr − 1)/2 · (1 + 12 h/w)^−½  (+ 0.04 (εr − 1)(1 − w/h)² … for w < h, Pozar eq. 3.195 form),
/// and εr for a stripline (homogeneous dielectric).
double effectivePermittivity(const BoardSettings& s, int layer, double w);
/// Propagation delay (s per mm) of a track: √εeff / c.
double propagationDelayPerMm(const BoardSettings& s, int layer, double w);
/// Via barrel capacitance (F), Johnson & Graham, High-Speed Digital Design §7.3: C[pF] = 1.41 εr T D1 / (D2 − D1), T the
/// barrel length, D1 the pad and D2 the anti-pad diameter (inches in the original; mm here, the ratio is unit-free).
double viaCapacitance(double lengthMm, double padMm, double antipadMm, double er);
/// Via barrel inductance (H), Johnson & Graham §7.3: L[nH] = 5.08 h [ln(4h/d) + 1], h the length and d the drill (inches).
double viaInductance(double lengthMm, double drillMm);
/// Longest unterminated line (mm) for a rise time: one-way delay ≤ RT / 6 (Bogatin's "1 inch per ns of rise time" on
/// FR-4); longer lines ring and need termination.
double criticalLength(double riseTime, double delayPerMm);

/// Crosstalk between two parallel tracks (fractions of the aggressor's launched step).
struct CouplingEstimate {
    double kl = 0;    // inductive coupling Lm/L
    double kc = 0;    // capacitive coupling Cm/C
    double kb = 0;    // backward (near-end) coefficient (Cm/C + Lm/L) / 4
    double next = 0;  // near-end noise: kb · min(1, 2·TD / RT)
    double fext = 0;  // far-end noise: ½ (Cm/C − Lm/L) · TD / RT (negative = inverted pulse; 0 on stripline)
};
/// Even- and odd-mode impedance of two zero-thickness edge-coupled striplines of width w, gap s, between planes b
/// apart: Cohn's exact conformal-mapping result (IRE Trans. MTT-3, 1955), Z = 30π/√εr · K(k')/K(k) with
/// k_e = tanh(πw/2b)·tanh(π(w+s)/2b) and k_o = tanh(πw/2b)·coth(π(w+s)/2b). With s → ∞ both tend to the single line.
std::pair<double, double> coupledStriplineImpedance(double w, double s, double b, double er);
/// Complete elliptic integral of the first kind K(k) (arithmetic–geometric mean).
double ellipticK(double k);
/// Coupling of two parallel tracks of widths w1, w2 at edge gap `gap` on `layer`, coupled over `coupledMm`:
///  • stripline (homogeneous): Lm/L = Cm/C = (Z0e − Z0o)/(Z0e + Z0o) from Cohn's coupled-stripline impedances;
///  • microstrip: Lm/L from image theory for conductors at height H = h + t/2 over the plane with the thin-strip
///    equivalent radius r = (w + t)/4, Lm/L = ln(1 + (2H/D)²) / (2 ln(2H/r)) (C. R. Paul, Introduction to EMC,
///    per-unit-length parameters of lines above a ground plane); Cm/C = Lm/L · ((εr + 1)/2) / εeff because part of the
///    mutual field runs through air.
/// TD = coupled length × delay per mm; NEXT saturates once 2·TD ≥ RT (Bogatin, Signal and Power Integrity — Simplified).
CouplingEstimate crosstalkCoupling(const BoardSettings& s, int layer, double w1, double w2, double gap, double coupledMm,
                                   double riseTime);

/// Broadside coupling of tracks on adjacent layers `layerA` / `layerB` whose centre lines are `offset` apart laterally:
/// the same thin-wire image theory as the microstrip case (C. R. Paul) with the conductors at heights h1 and h2 over
/// the nearest plane outside the pair, Lm/L = ln((x² + (h1+h2)²)/(x² + (h1−h2)²)) / (2·√(ln(2h1/r)·ln(2h2/r))). Both
/// layers buried: homogeneous (Cm/C = Lm/L, no FEXT); otherwise the air-filled share lowers Cm/C as on a microstrip.
/// The second plane on the other side is ignored, so the estimate errs high (conservative). NEXT / FEXT as above.
CouplingEstimate broadsideCoupling(const BoardSettings& s, int layerA, int layerB, double w1, double w2, double offset,
                                   double coupledMm, double riseTime);

// ---- time-domain transmission-line network ----------------------------------------------------------------------------

/// A network of lossless lines and lumped elements to ground, driven by a Thévenin source with linear edges. Solved
/// with Bergeron's method of characteristics (Dommel 1969): every line is exact for its delay rounded to the time step,
/// capacitors use the trapezoidal companion model. Lines decouple the nodes, so each step is a scalar solve per node.
struct TlNetwork {
    struct Line {
        int a = 0, b = 0;
        double z0 = 50, delay = 0;  // Ω, s
    };
    int nodes = 0;
    std::vector<Line> lines;
    std::vector<double> capacitance;  // per node, F
    std::vector<double> conductance;  // per node, S to its rail
    std::vector<double> railCurrent;  // per node, Σ G·V_rail (A)
    int driverNode = 0;
    double rSource = 30;            // Ω
    double vHigh = 3.3;             // open-circuit source high level
    double riseTime = 1e-9, fallTime = 1e-9;  // 10–90 %
    explicit TlNetwork(int n = 0);
    int addNode();
    void addLine(int a, int b, double z0, double delay);
};

struct TlRun {
    double dt = 0, edgeStart = 0, fallStart = 0, stop = 0;
    std::vector<double> time;
    std::vector<double> source;                 // ideal source voltage
    std::vector<std::vector<double>> probes;    // per probe node
};

/// Simulates a rising edge at `edgeStart` and a falling edge at `fallStart` until `stop` (all > 0) with time step `dt`.
/// Lines shorter than half a step are lumped (their capacitance moves to the node). Starts from the DC state.
TlRun simulateTl(const TlNetwork& net, const std::vector<int>& probes, double dt, double edgeStart, double fallStart, double stop);

/// Measurements of one probe waveform.
struct EdgeMetrics {
    double vLow = 0, vHigh = 0;      // settled levels (V)
    double overshoot = 0;            // above the high level after the rising edge (V)
    double undershoot = 0;           // below the low level after the falling edge (V)
    double ringbackHigh = 0;         // min after first crossing VIH minus VIH (V; < 0 re-crosses the threshold)
    double ringbackLow = 0;          // VIL minus max after first crossing VIL (V; < 0 re-crosses)
    double settling = 0;             // rising edge: time to stay within 5 % of the swing (s from the edge start)
    double flightTime = 0;           // 50 % crossing minus the source's 50 % crossing (s)
    bool reachesHigh = true, reachesLow = true;  // crosses VIH / VIL at all
    bool settled = true;
};
EdgeMetrics measureEdges(const TlRun& run, size_t probe, double vih, double vil);

// ---- project settings ----------------------------------------------------------------------------------------------

/// Per-rail power-integrity inputs (0 = derived from the design).
struct PdnRailSettings {
    std::string net;
    double ripplePercent = 0;     // allowed AC ripple, % of the rail
    double transientCurrent = 0;  // A, the load step the PDN must hold within the ripple
    double dcCurrent = 0;         // A, total DC load for IR drop
    double vrmR = 0;              // Ω, regulator output resistance (0 = by regulator type)
    double vrmBandwidth = 0;      // Hz, regulator loop bandwidth (0 = by regulator type)
};

/// Signal / power-integrity setup stored with the project.
struct SiSettings {
    std::vector<DriverModel> models;                     // imported IBIS models (id "ibis:<name>")
    std::map<std::string, std::string> componentModels;  // "U1" → model id (every pin of the part)
    std::map<std::string, std::string> pinModels;        // "U1.12" (ref.pin number or name) → model id
    std::map<std::string, std::string> netModels;        // net name → model id (its driver and receivers)
    std::vector<PdnRailSettings> rails;
    bool signOff = false;          // add a "Signal & Power Integrity" stage to design verification
    double overshootLimit = 0.15;  // fraction of the swing
    double crosstalkLimit = 0.05;  // fraction of the victim's swing
    std::string copperFoil;        // copper foil profile for loss ("" = by laminate, see LossyLine.hpp)
    double copperTempC = 20;       // copper temperature for IR drop (°C): ρ rises 0.393 %/°C above 20 °C
    bool fieldSolverLines = false; // channel lines take Z0, εeff and R_ac from the 2D field solver (FieldSolver.hpp)
    /// Serial channels checked in sign-off: an eye at `bitRate` against a mask (SI_EYE_MASK).
    struct ChannelSpec {
        std::string net;
        double bitRate = 0;       // b/s
        double maskHeight = 0;    // V
        double maskWidthUi = 0;   // UI
    };
    std::vector<ChannelSpec> channels;
    const ChannelSpec* channel(const std::string& net) const;
    /// Imported model, else a logic family; nullptr when unknown.
    const DriverModel* findModel(const std::string& id) const;
    const PdnRailSettings* rail(const std::string& net) const;
    bool isDefault() const;
    Json toJson() const;
    static SiSettings fromJson(const Json& j);
};

// ---- copper connectivity of one net ----------------------------------------------------------------------------------

/// The routed copper of a net as a graph: nodes are track junctions, via landings (one per copper layer the barrel
/// spans) and pads; edges are track sections and via barrel sections.
struct NetCopperGraph {
    struct Node {
        Vec2 p;
        int layer = -1;  // -1 = a pad on several layers
    };
    struct Edge {
        int a = 0, b = 0;
        bool via = false;
        int layer = 0;      // track layer (via: upper layer of the section)
        double width = 0;   // track width; via: drill
        double length = 0;  // mm (via: barrel depth of the section)
        double diameter = 0;  // via pad
    };
    std::vector<Node> nodes;
    std::vector<Edge> edges;
    std::map<size_t, int> padNode;  // index into PcbLayout::pads() → node
    /// Nodes reachable from `start`.
    std::vector<bool> reachable(int start) const;
};
NetCopperGraph buildNetCopperGraph(const PcbLayout& pcb, const std::vector<Pad>& pads, int net);

/// Supply voltage named by a rail ("3V3" 3.3, "+1V8" 1.8, "VCC_5V" 5, "1.2V" 1.2); 0 when the name has none.
double railVoltageFromName(const std::string& name);

// ---- per-net analysis ------------------------------------------------------------------------------------------------

struct SiReceiver {
    int componentId = -1;
    std::string ref, pin, model;
    double pathDelay = 0;  // one-way line delay from the driver (s)
    double pathLength = 0; // mm
    bool connected = true;  // reached by copper from the driver
    EdgeMetrics metrics;
    bool ok = true;
};

struct SiNetResult {
    int net = -1;
    std::string name;
    std::string error;  // why the net cannot be analysed (empty when it was)
    bool routed = false, estimated = false;  // estimated: unrouted net analysed on straight-line lengths
    // Driver
    int driverComponent = -1;
    std::string driverRef, driverPin;
    bool driverAssumed = false;  // no output pin identified
    DriverModel driver;
    double seriesR = 0;          // series termination found in the schematic (Ω)
    std::string seriesRef;
    std::vector<std::string> terminations;  // "R5 49.9 Ω to GND", "ODT 60 Ω"
    // Lines
    double length = 0;         // copper length of the net (mm)
    double maxDelay = 0;       // longest driver → receiver line delay (s)
    double z0Min = 0, z0Max = 0, z0Trunk = 0;
    double criticalLength = 0; // mm
    bool critical = false;     // longest path exceeds the critical length
    struct Section {
        std::string layer;
        double width = 0, length = 0, z0 = 0, delay = 0;
    };
    std::vector<Section> sections;
    int vias = 0;
    // Results
    std::vector<SiReceiver> receivers;
    int worstReceiver = -1;
    bool ok = true;
    double recommendedSeriesR = 0;  // 0 = no change advised
    std::string recommendation;
    EdgeMetrics terminatedMetrics;  // worst receiver with the recommended series resistor
    TlRun run, terminatedRun;       // probes: 0 driver pad, 1 worst receiver
    std::vector<std::string> notes;
};

/// Analyses one net (signal nets with two or more pads). `extraSeriesR` ≥ 0 adds a what-if series resistor at the driver.
SiNetResult analyzeNet(const Project& project, int net, double extraSeriesR = -1);
/// The circuit behind analyzeNet, for the frequency-domain channel solver (Channel.hpp): the copper graph actually
/// analysed (the straight-line estimate when unrouted, via stubs removed when backdrilled), the driver pad's node, the
/// passive loads per graph node (pad capacitance, terminations to their rails, connectors, discretes — no driver or
/// receiver device models) and every logic receiver with its model.
struct SiNetCircuit {
    NetCopperGraph graph;
    std::vector<bool> reach;  // per graph node: connected to the driver pad
    int driverNode = -1;
    size_t driverPad = static_cast<size_t>(-1);
    std::vector<double> padC, padG, padJ;  // per graph node: F, S to the rail, sum G*V_rail (A)
    struct Receiver {
        int node = -1;
        size_t pad = 0;
        int componentId = -1;
        std::string ref, pin;
        DriverModel model;
        size_t result = 0;  // index into SiNetResult::receivers
    };
    std::vector<Receiver> receivers;
};
/// analyzeNet without the time-domain run, filling `out` with its circuit.
SiNetResult analyzeNetCircuit(const Project& project, int net, SiNetCircuit& out);

/// Net result with waveforms decimated to at most `maxPoints` samples.
Json siNetJson(const SiNetResult& r, size_t maxPoints = 400);

/// Signal nets the SI panel offers: [{net,name,length,delay,critical,criticalLength,driver,model,fast,routed}],
/// critical and fast nets first.
Json siNetsJson(const Project& project);

// ---- crosstalk and return path -----------------------------------------------------------------------------------------

struct CrosstalkPair {
    int aggressor = -1, victim = -1;
    std::string layer;
    double coupledLength = 0;  // mm
    double spacing = 0;        // narrowest edge-to-edge gap (mm)
    double next = 0, fext = 0; // fractions of the aggressor's launched step
    double noise = 0;          // worst of |NEXT|, |FEXT| at the victim (V)
    double limit = 0;          // allowed noise (V)
    Vec2 at;
    bool ok = true;
    bool broadside = false;  // the victim runs on the adjacent layer ("Top / In1")
};
/// Parallel neighbours of fast / driven nets on the same layer (edge coupled) or the adjacent layer (broadside),
/// differential-pair partners excluded.
std::vector<CrosstalkPair> crosstalkPairs(const Project& project);

struct ReturnPathIssue {
    std::string code;  // "SI_PLANE_GAP", "SI_PLANE_SPLIT", "SI_REFERENCE_CHANGE"
    int net = -1;
    Vec2 at;
    std::string message;
};
/// Signals that cross a gap or split in their reference plane, or change reference plane through a via without a
/// stitching via (same net) or stitching capacitor (different nets) within 2 mm.
std::vector<ReturnPathIssue> returnPathIssues(const Project& project);

Json crosstalkJson(const Project& project);

/// Every SI and PI finding as warnings / notes: overshoot, ringback, critical length, crosstalk, return path,
/// PDN target impedance and anti-resonance, decoupling and IR drop. Codes SI_* and PI_*.
std::vector<RuleViolation> signalPowerIntegrityChecks(const Project& project);

/// Eye of a serial channel spec (PRBS7 at its bit rate, IBIS driver when assigned, else an ideal 50 Ω source and
/// termination) against its mask: the SI_EYE_MASK finding.
struct ChannelCheck {
    bool ok = true;
    double eyeHeight = 0, eyeWidth = 0, maskMargin = 0;
    std::string message;
};
ChannelCheck checkChannel(const Project& project, const SiSettings::ChannelSpec& spec);

}  // namespace sieda
