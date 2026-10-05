// SiEDA Core (internal) — the simulator's element representation, shared by Simulator.cpp (built-in models,
// analyses), SpiceBuild.cpp (imported SPICE models) and Noise.cpp (noise analysis).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>
#include <memory>
#include <string>
#include <vector>

#include "SpiceDevices.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/SpiceModels.hpp"

namespace sieda {

namespace simdetail {
inline constexpr double kVt = 0.025852;  // thermal voltage @ 300 K
inline constexpr double kGmin = 1e-12;
inline constexpr double kOpAmpGain = 1e6;

/// exp(x) continued linearly above `kMax` so Newton steps cannot overflow. The knee must sit above any realistic
/// operating point: for a junction with saturation current `is` that is ln(1 A / is), which matters for wide-gap
/// LEDs (blue/white, is ≈ 1e-26) whose forward drop needs exponents near 60.
inline double limexp(double x, double kMax = 40.0) {
    return x < kMax ? std::exp(x) : std::exp(kMax) * (1.0 + x - kMax);
}

inline double junctionLimit(double is) { return std::max(40.0, std::log(1.0 / std::max(is, 1e-300))); }

enum class ElemType {
    Resistor, Capacitor, Inductor, VSource, ISource, Diode, NPN, NMOS, OpAmp, Regulator, Load, McuPin, InAmp,
    Device,      // imported SPICE semiconductor (diode, BJT, MOSFET, JFET): `dev` between `dn`
    Ctrl,        // controlled / behavioural source (E F G H B, POLY, TABLE, op-amp macromodel stages)
    Coupling,    // mutual inductance between two inductors (K)
    ModelPorts,  // the pins of a part with an imported model: its reading
};

/// What a Ctrl element computes from its controlling values.
enum class CtrlKind {
    Poly,       // SPICE polynomial of the control pairs (linear E/F/G/H are POLY with two coefficients)
    Expr,       // B source / VALUE = {expression}
    Table,      // TABLE {expression} = (x, y) …, piecewise linear
    TanhStage,  // op-amp input stage: coeffs = {gm, imax}: imax·tanh(gm·u / imax)
    Clamp,      // op-amp output limiter: coeffs = {lo, hi}: u clamped smoothly into [lo, hi]
    DeadZone,   // op-amp internal-node clamp: coeffs = {g, vc}: g·(softplus(u − vc) − softplus(−u − vc))
};

// Microcontroller pins: 25 Ω push-pull outputs, 35 kΩ pull-ups (ATmega328P datasheet typical values).
inline constexpr double kMcuOutputConductance = 1.0 / 25.0;
inline constexpr double kMcuPullupConductance = 1.0 / 35000.0;

// Smooth max(0, z) with a 20 mV knee (keeps Newton derivatives continuous).
inline double softplus(double z) {
    constexpr double s = 0.02;
    return z / s > 30 ? z : s * std::log1p(std::exp(z / s));
}
}  // namespace simdetail

struct Simulator::Element {
    simdetail::ElemType type;
    int componentId = -1;
    std::array<int, 3> n{{-1, -1, -1}};  // unknown indices, -1 = ground
    double value = 0;                    // R, C, L
    SourceSpec source;
    int branch = -1;  // unknown index for branch current
    // Device model parameters
    double is = 1e-14, emission = 1.0, betaF = 100, betaR = 1, vth = 1.5, kp = 0.02, lambda = 0.01, vsat = 15;
    double gbw = 1e6;  // op-amp gain–bandwidth product (Hz): the dominant pole of the AC model
    // Behavioural regulator (in, out, ref): CV with dropout / CC at ilimit / off when it would have to sink.
    double dropout = 0.3, iq = 0, ilimit = 1.0, rout = 0.01;
    // Isolated DC-DC (aux[0] = primary return): the output loop closes through `ref`, the input draws
    // V_out·I_out / efficiency between `in` and the primary return.
    bool isolated = false;
    double efficiency = 0.8;
    mutable int mode = 0;  // 0 CV, 1 CC, 2 off
    int sub = 0;           // element index within a custom part (0 = regulator, 1… = supply loads)
    bool isSwitch = false;
    // Microcontroller pin (McuPin): n = {pin, GND, VCC}; conductances from the firmware's drive over the last step.
    int mcu = -1, mcuPin = -1;
    double gHigh = 0, gLow = 0, gPull = 0;
    // Instrumentation amplifier (InAmp): n = {+IN, −IN, OUT}, aux = {REF, V+, V−}, value = gain.
    std::array<int, 3> aux{{-1, -1, -1}};
    // Transient state
    double prevV = 0, prevI = 0;

    // ---- imported SPICE models and macromodel stages ----------------------------------------------------------------
    bool internal = false;  // part of a model: it has no reading of its own
    std::shared_ptr<const spicedev::Device> dev;  // Device
    std::array<int, 4> dn{{-1, -1, -1, -1}};      // Device terminals (unknown indices, -1 ground)
    std::array<double, 4> qPrev{{0, 0, 0, 0}}, iqPrev{{0, 0, 0, 0}};  // Device charges and their currents, last step
    double prevIc = 0;                            // capacitor current at the last step (trapezoidal rule)
    // Ctrl: f(controls) between n[0] (+) and n[1] (−): a voltage with `branch`, or a current from n[0] through the
    // source to n[1]. `refs` are the unknowns it reads (node voltages, branch currents); `pairs` index them per
    // control value (u = refs[a] − refs[b], −1 = 0).
    simdetail::CtrlKind ctrl = simdetail::CtrlKind::Poly;
    bool voltageOut = false;
    std::vector<int> refs;
    std::vector<std::array<int, 2>> pairs;
    std::vector<double> coeffs;
    std::vector<std::vector<int>> terms;
    std::shared_ptr<SpiceExpr> expr;
    std::vector<std::pair<double, double>> table;
    std::array<size_t, 2> coupled{{0, 0}};        // Coupling: the two inductor elements; value = M
    // ModelPorts: node of each pin of the part (-1 ground, -2 not used by the model) and the model's elements.
    std::vector<int> pinNodes;
    size_t first = 0, last = 0;
    ComponentKind partKind = ComponentKind::Custom;

    /// Regulated output for input headroom vi (V_in − V_ref): min(vout, vi − dropout), never negative.
    double setpoint(double vi) const { return simdetail::softplus(value - simdetail::softplus(value - (vi - dropout))); }

    /// Instrumentation-amplifier output REF + G·(V+IN − V−IN), limited (smoothly) to 50 mV inside the supply rails.
    static double inAmpOut(double gain, double vp, double vm, double vref, double vpos, double vneg) {
        double mid = 0.5 * (vpos + vneg);
        double half = std::max(0.5 * (vpos - vneg) - 0.05, 1e-3);
        return mid + half * std::tanh((vref + gain * (vp - vm) - mid) / half);
    }
};

namespace simdetail {
/// "KEY=value" in an op-amp value (case-insensitive, a whole word): engineering notation ("5MEG", "100k"), a slew
/// rate in V/µs, V/ns, V/ms or V/s ("SR=0.5V/us"), a gain in dB ("AOL=100dB"), a noise density ("EN=10n").
bool opAmpValueParam(const std::string& value, const char* key, double& out);

inline double nodeV(const std::vector<double>& x, int i) { return i < 0 ? 0.0 : x[static_cast<size_t>(i)]; }

// Terminal currents flowing *into* a non-linear device from each terminal.
inline std::array<double, 3> deviceCurrents(const Simulator::Element& e, const std::array<double, 3>& v) {
    switch (e.type) {
        case ElemType::Diode: {
            double i = e.is * (limexp((v[0] - v[1]) / (e.emission * kVt), junctionLimit(e.is)) - 1.0);
            return {i, -i, 0};
        }
        case ElemType::NPN: {  // terminals: B, C, E  (Ebers–Moll transport model)
            double vbe = v[0] - v[2], vbc = v[0] - v[1];
            double iF = e.is * (limexp(vbe / kVt, junctionLimit(e.is)) - 1.0);
            double iR = e.is * (limexp(vbc / kVt, junctionLimit(e.is)) - 1.0);
            double ic = (iF - iR) - iR / e.betaR;
            double ib = iF / e.betaF + iR / e.betaR;
            return {ib, ic, -(ib + ic)};
        }
        case ElemType::Load: {  // supply → return, saturating at the operating current above ~0.3 V
            double i = e.value * std::tanh((v[0] - v[1]) / 0.3);
            return {i, -i, 0};
        }
        case ElemType::NMOS: {  // terminals: G, D, S  (square law with channel-length modulation)
            double vd = v[1], vs = v[2];
            double sign = 1.0;
            if (vd < vs) {
                std::swap(vd, vs);
                sign = -1.0;
            }
            double vgs = v[0] - vs, vds = vd - vs, vov = vgs - e.vth, id = 0.0;
            if (vov > 0) {
                if (vds < vov) id = e.kp * (vov * vds - 0.5 * vds * vds) * (1 + e.lambda * vds);
                else id = 0.5 * e.kp * vov * vov * (1 + e.lambda * vds);
            }
            return {0.0, sign * id, -sign * id};
        }
        default: return {0, 0, 0};
    }
}

// Dense complex LU solve with partial pivoting (same scheme as luSolve). Returns false if singular.
inline bool luSolveComplex(std::vector<std::complex<double>>& A, std::vector<std::complex<double>>& b, int n) {
    for (int k = 0; k < n; ++k) {
        int piv = k;
        double best = std::abs(A[static_cast<size_t>(k * n + k)]);
        for (int r = k + 1; r < n; ++r) {
            double v = std::abs(A[static_cast<size_t>(r * n + k)]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-300) return false;
        if (piv != k) {
            for (int c = 0; c < n; ++c) std::swap(A[static_cast<size_t>(k * n + c)], A[static_cast<size_t>(piv * n + c)]);
            std::swap(b[static_cast<size_t>(k)], b[static_cast<size_t>(piv)]);
        }
        std::complex<double> d = A[static_cast<size_t>(k * n + k)];
        for (int r = k + 1; r < n; ++r) {
            std::complex<double> a = A[static_cast<size_t>(r * n + k)];
            if (a.real() == 0.0 && a.imag() == 0.0) continue;  // sparse rows: nothing to eliminate
            std::complex<double> f = a / d;
            A[static_cast<size_t>(r * n + k)] = 0.0;
            for (int c = k + 1; c < n; ++c) A[static_cast<size_t>(r * n + c)] -= f * A[static_cast<size_t>(k * n + c)];
            b[static_cast<size_t>(r)] -= f * b[static_cast<size_t>(k)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        std::complex<double> s = b[static_cast<size_t>(r)];
        for (int c = r + 1; c < n; ++c) s -= A[static_cast<size_t>(r * n + c)] * b[static_cast<size_t>(c)];
        b[static_cast<size_t>(r)] = s / A[static_cast<size_t>(r * n + r)];
    }
    return true;
}
}  // namespace simdetail

}  // namespace sieda
