#include "sieda/Simulator.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <sstream>

#include "sieda/Units.hpp"

namespace sieda {

// ------------------------------------------------------------------ source specs

std::optional<SourceSpec> SourceSpec::parse(const std::string& raw) {
    std::string text;
    for (char c : raw) text += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    auto trim = [](std::string s) {
        size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t");
        return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
    };
    text = trim(text);
    if (text.empty()) return std::nullopt;

    auto args = [&](size_t skip) -> std::optional<std::vector<double>> {
        std::string body = text.substr(skip);
        for (char& c : body)
            if (c == '(' || c == ')' || c == ',') c = ' ';
        std::istringstream ss(body);
        std::vector<double> out;
        std::string tok;
        // Original-case tokens matter for m/M; recover them from the raw string positions.
        std::string rawBody = trim(raw).substr(skip);
        for (char& c : rawBody)
            if (c == '(' || c == ')' || c == ',') c = ' ';
        std::istringstream rs(rawBody);
        while (rs >> tok) {
            auto v = parseEngineeringValue(tok);
            if (!v) return std::nullopt;
            out.push_back(*v);
        }
        return out;
    };

    SourceSpec s;
    if (text.rfind("SIN", 0) == 0) {
        auto a = args(3);
        if (!a || a->size() < 3) return std::nullopt;
        s.kind = Kind::Sine;
        s.offset = (*a)[0];
        s.amplitude = (*a)[1];
        s.frequency = (*a)[2];
        if (s.frequency <= 0) return std::nullopt;
        return s;
    }
    if (text.rfind("PULSE", 0) == 0) {
        auto a = args(5);
        if (!a || a->size() < 3) return std::nullopt;
        s.kind = Kind::Pulse;
        s.v1 = (*a)[0];
        s.v2 = (*a)[1];
        s.period = (*a)[2];
        if (a->size() >= 4) s.duty = std::clamp((*a)[3], 0.01, 0.99);
        if (s.period <= 0) return std::nullopt;
        return s;
    }
    std::string dcText = trim(raw);
    if (dcText.size() > 2 && (dcText.rfind("DC", 0) == 0 || dcText.rfind("dc", 0) == 0)) dcText = trim(dcText.substr(2));
    auto v = parseEngineeringValue(dcText);
    if (!v) return std::nullopt;
    s.kind = Kind::DC;
    s.dc = *v;
    return s;
}

double SourceSpec::valueAt(double t) const {
    switch (kind) {
        case Kind::DC: return dc;
        case Kind::Sine: return offset + amplitude * std::sin(2 * kPi * frequency * t);
        case Kind::Pulse: {
            if (t <= 0) return v1;  // SPICE semantics: the pulse rises from v1 after t = 0
            double phase = std::fmod(t, period) / period;
            return phase < duty ? v2 : v1;
        }
    }
    return 0;
}

// ------------------------------------------------------------------ elements

namespace {
constexpr double kVt = 0.025852;  // thermal voltage @ 300 K
constexpr double kGmin = 1e-12;
constexpr double kOpAmpGain = 1e6;

double limexp(double x) {
    constexpr double kMax = 40.0;
    return x < kMax ? std::exp(x) : std::exp(kMax) * (1.0 + x - kMax);
}

enum class ElemType { Resistor, Capacitor, Inductor, VSource, ISource, Diode, NPN, NMOS, OpAmp };
}  // namespace

struct Simulator::Element {
    ElemType type;
    int componentId = -1;
    std::array<int, 3> n{{-1, -1, -1}};  // unknown indices, -1 = ground
    double value = 0;                    // R, C, L
    SourceSpec source;
    int branch = -1;  // unknown index for branch current
    // Device model parameters
    double is = 1e-14, emission = 1.0, betaF = 100, betaR = 1, vth = 1.5, kp = 0.02, lambda = 0.01, vsat = 15;
    // Transient state
    double prevV = 0, prevI = 0;
};

namespace {
using Elem = Simulator::Element;

double nodeV(const std::vector<double>& x, int i) { return i < 0 ? 0.0 : x[static_cast<size_t>(i)]; }

// Terminal currents flowing *into* a non-linear device from each terminal.
std::array<double, 3> deviceCurrents(const Elem& e, const std::array<double, 3>& v) {
    switch (e.type) {
        case ElemType::Diode: {
            double i = e.is * (limexp((v[0] - v[1]) / (e.emission * kVt)) - 1.0);
            return {i, -i, 0};
        }
        case ElemType::NPN: {  // terminals: B, C, E  (Ebers–Moll transport model)
            double vbe = v[0] - v[2], vbc = v[0] - v[1];
            double iF = e.is * (limexp(vbe / kVt) - 1.0);
            double iR = e.is * (limexp(vbc / kVt) - 1.0);
            double ic = (iF - iR) - iR / e.betaR;
            double ib = iF / e.betaF + iR / e.betaR;
            return {ib, ic, -(ib + ic)};
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

int terminalCount(ElemType t) { return t == ElemType::Diode ? 2 : 3; }

// Dense LU solve with partial pivoting. Returns false if singular.
bool luSolve(std::vector<double>& A, std::vector<double>& b, int n) {
    for (int k = 0; k < n; ++k) {
        int piv = k;
        double best = std::fabs(A[static_cast<size_t>(k * n + k)]);
        for (int r = k + 1; r < n; ++r) {
            double v = std::fabs(A[static_cast<size_t>(r * n + k)]);
            if (v > best) { best = v; piv = r; }
        }
        if (best < 1e-300) return false;
        if (piv != k) {
            for (int c = 0; c < n; ++c) std::swap(A[static_cast<size_t>(k * n + c)], A[static_cast<size_t>(piv * n + c)]);
            std::swap(b[static_cast<size_t>(k)], b[static_cast<size_t>(piv)]);
        }
        double d = A[static_cast<size_t>(k * n + k)];
        for (int r = k + 1; r < n; ++r) {
            double f = A[static_cast<size_t>(r * n + k)] / d;
            if (f == 0.0) continue;
            A[static_cast<size_t>(r * n + k)] = 0.0;
            for (int c = k + 1; c < n; ++c) A[static_cast<size_t>(r * n + c)] -= f * A[static_cast<size_t>(k * n + c)];
            b[static_cast<size_t>(r)] -= f * b[static_cast<size_t>(k)];
        }
    }
    for (int r = n - 1; r >= 0; --r) {
        double s = b[static_cast<size_t>(r)];
        for (int c = r + 1; c < n; ++c) s -= A[static_cast<size_t>(r * n + c)] * b[static_cast<size_t>(c)];
        b[static_cast<size_t>(r)] = s / A[static_cast<size_t>(r * n + r)];
    }
    return true;
}
}  // namespace

Simulator::Simulator(const Schematic& schematic) : sch_(schematic) {}
Simulator::~Simulator() = default;

bool Simulator::build(std::string& error) {
    elements_.clear();
    const auto& nets = sch_.nets();
    int gnd = sch_.groundNet();
    if (gnd < 0) {
        error = "No ground reference: add a Ground symbol to the schematic.";
        return false;
    }
    netToNode_.assign(nets.size(), -1);
    nodeCount_ = 0;
    for (const auto& n : nets)
        if (!n.isGround) netToNode_[static_cast<size_t>(n.index)] = nodeCount_++;
    unknowns_ = nodeCount_;

    auto node = [&](int compId, int pin) {
        int net = sch_.netOf({compId, pin});
        return net < 0 ? -1 : netToNode_[static_cast<size_t>(net)];
    };

    for (const auto& c : sch_.components()) {
        Element e{};
        e.componentId = c.id;
        auto twoTerminal = [&](ElemType t) {
            e.type = t;
            e.n = {node(c.id, 0), node(c.id, 1), -1};
        };
        switch (c.kind) {
            case ComponentKind::Resistor:
            case ComponentKind::Fuse: {
                twoTerminal(ElemType::Resistor);
                auto v = parseEngineeringValue(c.value);
                if (c.kind == ComponentKind::Fuse) v = 0.05;  // nominal cold resistance
                if (!v || *v <= 0) { error = c.ref + ": invalid resistance '" + c.value + "'"; return false; }
                e.value = *v;
                break;
            }
            case ComponentKind::Switch: {
                twoTerminal(ElemType::Resistor);
                std::string v = c.value;
                std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return std::tolower(ch); });
                bool closed = v == "on" || v == "closed" || v == "1" || v == "true";
                e.value = closed ? 0.01 : 1e9;
                break;
            }
            case ComponentKind::Capacitor:
            case ComponentKind::Inductor: {
                twoTerminal(c.kind == ComponentKind::Capacitor ? ElemType::Capacitor : ElemType::Inductor);
                auto v = parseEngineeringValue(c.value);
                if (!v || *v <= 0) { error = c.ref + ": invalid value '" + c.value + "'"; return false; }
                e.value = *v;
                if (e.type == ElemType::Inductor) e.branch = unknowns_++;
                break;
            }
            case ComponentKind::VoltageSource:
            case ComponentKind::CurrentSource: {
                twoTerminal(c.kind == ComponentKind::VoltageSource ? ElemType::VSource : ElemType::ISource);
                auto s = SourceSpec::parse(c.value);
                if (!s) { error = c.ref + ": invalid source value '" + c.value + "'"; return false; }
                e.source = *s;
                if (e.type == ElemType::VSource) e.branch = unknowns_++;
                break;
            }
            case ComponentKind::Diode:
            case ComponentKind::LED: {
                twoTerminal(ElemType::Diode);
                if (c.kind == ComponentKind::LED) {
                    // Tuned for ~1.9–2.0 V forward drop at 10–20 mA (red); blue/white parts are brighter/higher.
                    std::string v = c.value;
                    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char ch) { return std::tolower(ch); });
                    e.emission = 2.0;
                    e.is = (v.find("blue") != std::string::npos || v.find("white") != std::string::npos) ? 1e-26 : 1e-18;
                } else {
                    e.is = 2.52e-9;
                    e.emission = 1.752;  // 1N4148-like
                }
                break;
            }
            case ComponentKind::NPN:
                e.type = ElemType::NPN;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};
                e.betaF = 200;
                break;
            case ComponentKind::NMOS:
                e.type = ElemType::NMOS;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};
                e.vth = 1.6;
                e.kp = 0.1;
                break;
            case ComponentKind::OpAmp:
                e.type = ElemType::OpAmp;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};  // IN+, IN-, OUT
                e.branch = unknowns_++;
                break;
            default: continue;  // ground, labels, connectors, generic ICs: no electrical model
        }
        elements_.push_back(e);
    }
    if (unknowns_ == 0) {
        error = "Nothing to simulate: the circuit has no non-ground nodes.";
        return false;
    }
    return true;
}

void Simulator::stamp(double t, double h, const std::vector<double>& x, double gminExtra, double sourceScale) {
    const int n = unknowns_;
    A_.assign(static_cast<size_t>(n * n), 0.0);
    b_.assign(static_cast<size_t>(n), 0.0);
    auto addA = [&](int r, int c, double v) {
        if (r >= 0 && c >= 0) A_[static_cast<size_t>(r * n + c)] += v;
    };
    auto addB = [&](int r, double v) {
        if (r >= 0) b_[static_cast<size_t>(r)] += v;
    };
    auto conductance = [&](int a, int bb, double g) {
        addA(a, a, g);
        addA(bb, bb, g);
        addA(a, bb, -g);
        addA(bb, a, -g);
    };

    for (int i = 0; i < nodeCount_; ++i) addA(i, i, kGmin + gminExtra);

    for (const auto& e : elements_) {
        int a = e.n[0], bb = e.n[1];
        switch (e.type) {
            case ElemType::Resistor: conductance(a, bb, 1.0 / e.value); break;
            case ElemType::Capacitor:
                if (h > 0) {
                    double geq = e.value / h;
                    conductance(a, bb, geq);
                    addB(a, geq * e.prevV);
                    addB(bb, -geq * e.prevV);
                }
                break;
            case ElemType::Inductor: {
                int k = e.branch;
                addA(a, k, 1);
                addA(bb, k, -1);
                addA(k, a, 1);
                addA(k, bb, -1);
                if (h > 0) {
                    double req = e.value / h;
                    addA(k, k, -req);
                    addB(k, -req * e.prevI);
                }
                break;
            }
            case ElemType::VSource: {
                int k = e.branch;
                addA(a, k, 1);
                addA(bb, k, -1);
                addA(k, a, 1);
                addA(k, bb, -1);
                addB(k, sourceScale * e.source.valueAt(t));
                break;
            }
            case ElemType::ISource: {
                double i = sourceScale * e.source.valueAt(t);
                addB(a, i);  // current leaves the "+" terminal into the circuit
                addB(bb, -i);
                break;
            }
            case ElemType::OpAmp: {
                int p = e.n[0], m = e.n[1], o = e.n[2], k = e.branch;
                double vd = nodeV(x, p) - nodeV(x, m);
                double arg = kOpAmpGain * vd / e.vsat;
                double th = std::tanh(arg);
                double slope = kOpAmpGain * (1.0 - th * th);
                double f0 = nodeV(x, o) - e.vsat * th;
                addA(o, k, 1);
                // f(x) = Vo - Vsat·tanh(A·(V+ − V−)/Vsat); J·x = J·x0 − f0
                addA(k, o, 1);
                addA(k, p, -slope);
                addA(k, m, slope);
                double jx0 = nodeV(x, o) - slope * vd;
                addB(k, jx0 - f0);
                break;
            }
            case ElemType::Diode:
            case ElemType::NPN:
            case ElemType::NMOS: {
                int tc = terminalCount(e.type);
                std::array<double, 3> v0{};
                for (int i = 0; i < tc; ++i) v0[static_cast<size_t>(i)] = nodeV(x, e.n[static_cast<size_t>(i)]);
                std::array<double, 3> i0 = deviceCurrents(e, v0);
                double J[3][3] = {};
                const double dv = 1e-6;
                for (int j = 0; j < tc; ++j) {
                    auto vp = v0, vm = v0;
                    vp[static_cast<size_t>(j)] += dv;
                    vm[static_cast<size_t>(j)] -= dv;
                    auto ip = deviceCurrents(e, vp), im = deviceCurrents(e, vm);
                    for (int r = 0; r < tc; ++r)
                        J[r][j] = (ip[static_cast<size_t>(r)] - im[static_cast<size_t>(r)]) / (2 * dv);
                }
                for (int r = 0; r < tc; ++r) {
                    double rhs = -i0[static_cast<size_t>(r)];
                    for (int j = 0; j < tc; ++j) {
                        addA(e.n[static_cast<size_t>(r)], e.n[static_cast<size_t>(j)], J[r][j]);
                        rhs += J[r][j] * v0[static_cast<size_t>(j)];
                    }
                    addB(e.n[static_cast<size_t>(r)], rhs);
                }
                // Small parallel conductances keep junctions/channels well conditioned.
                conductance(e.n[0], e.n[1], kGmin);
                if (tc == 3) conductance(e.n[1], e.n[2], 1e-9);
                break;
            }
        }
    }
}

bool Simulator::solve(double t, double h, std::vector<double>& x, int& iterations, double gminExtra,
                      double sourceScale) {
    const int maxIter = 300;
    for (int it = 0; it < maxIter; ++it) {
        stamp(t, h, x, gminExtra, sourceScale);
        if (!luSolve(A_, b_, unknowns_)) return false;
        double maxDelta = 0;
        for (int i = 0; i < unknowns_; ++i) {
            double xn = b_[static_cast<size_t>(i)];
            if (!std::isfinite(xn)) return false;
            double delta = xn - x[static_cast<size_t>(i)];
            // Damp node-voltage steps to help exponential devices converge.
            if (i < nodeCount_ && std::fabs(delta) > 2.0) delta = delta > 0 ? 2.0 : -2.0;
            double tol = 1e-6 + 1e-6 * std::fabs(xn);
            maxDelta = std::max(maxDelta, std::fabs(delta) / tol);
            x[static_cast<size_t>(i)] += delta;
        }
        iterations = it + 1;
        if (maxDelta <= 1.0) return true;
    }
    return false;
}

std::vector<DeviceReading> Simulator::readings(const std::vector<double>& x, double h) const {
    std::vector<DeviceReading> out;
    for (const auto& e : elements_) {
        DeviceReading r;
        r.componentId = e.componentId;
        double va = nodeV(x, e.n[0]), vb = nodeV(x, e.n[1]);
        r.voltage = va - vb;
        switch (e.type) {
            case ElemType::Resistor: r.current = r.voltage / e.value; r.power = r.voltage * r.current; break;
            case ElemType::Capacitor:
                r.current = h > 0 ? e.value / h * (r.voltage - e.prevV) : 0.0;
                r.power = r.voltage * r.current;
                break;
            case ElemType::Inductor:
                r.current = x[static_cast<size_t>(e.branch)];
                r.power = r.voltage * r.current;
                break;
            case ElemType::VSource:
                r.current = -x[static_cast<size_t>(e.branch)];  // delivered out of "+"
                r.power = -r.voltage * r.current;
                break;
            case ElemType::ISource:
                r.current = e.source.valueAt(0);
                r.power = -r.voltage * r.current;
                break;
            case ElemType::OpAmp:
                r.voltage = nodeV(x, e.n[2]);
                r.current = -x[static_cast<size_t>(e.branch)];
                r.power = 0;
                break;
            case ElemType::Diode:
            case ElemType::NPN:
            case ElemType::NMOS: {
                std::array<double, 3> v{va, vb, nodeV(x, e.n[2])};
                auto i = deviceCurrents(e, v);
                if (e.type == ElemType::Diode) {
                    r.current = i[0];
                    r.power = r.voltage * r.current;
                } else {
                    r.current = i[1];          // collector / drain current
                    r.voltage = v[1] - v[2];   // V_CE / V_DS
                    r.power = v[0] * i[0] + v[1] * i[1] + v[2] * i[2];
                }
                break;
            }
        }
        out.push_back(r);
    }
    return out;
}

DcResult Simulator::dcOperatingPoint() {
    DcResult res;
    if (!build(res.error)) return res;
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    int iters = 0;
    bool ok = solve(0, 0, x, iters, 0.0, 1.0);
    res.iterations = iters;
    if (!ok) {
        // Gmin stepping, then source stepping.
        std::fill(x.begin(), x.end(), 0.0);
        ok = true;
        for (double g = 1e-2; g >= 1e-13 && ok; g /= 10) ok = solve(0, 0, x, iters, g, 1.0);
        if (ok) ok = solve(0, 0, x, iters, 0.0, 1.0);
        if (!ok) {
            std::fill(x.begin(), x.end(), 0.0);
            ok = true;
            for (int s = 1; s <= 20 && ok; ++s) ok = solve(0, 0, x, iters, 0.0, s / 20.0);
        }
        res.iterations += iters;
    }
    if (!ok) {
        res.error = "DC operating point did not converge (check for floating nodes or unrealistic values).";
        return res;
    }
    res.converged = true;
    const auto& nets = sch_.nets();
    res.netVoltages.assign(nets.size(), 0.0);
    for (size_t i = 0; i < nets.size(); ++i) res.netVoltages[i] = nodeV(x, netToNode_[i]);
    res.devices = readings(x, 0);
    return res;
}

TransientResult Simulator::transient(double tStop, double tStep) {
    TransientResult res;
    if (!(tStop > 0) || !(tStep > 0)) {
        res.error = "Transient analysis needs positive stop time and step.";
        return res;
    }
    const double maxSamples = 200000;
    if (tStop / tStep > maxSamples) tStep = tStop / maxSamples;

    if (!build(res.error)) return res;
    // Initial condition: DC operating point with t = 0 source values.
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    int iters = 0;
    if (!solve(0, 0, x, iters, 0.0, 1.0)) {
        std::fill(x.begin(), x.end(), 0.0);
        bool ok = true;
        for (double g = 1e-2; g >= 1e-13 && ok; g /= 10) ok = solve(0, 0, x, iters, g, 1.0);
        if (!ok || !solve(0, 0, x, iters, 0.0, 1.0)) {
            res.error = "Could not find initial operating point for transient analysis.";
            return res;
        }
    }
    auto updateState = [&](const std::vector<double>& xs) {
        for (auto& e : elements_) {
            e.prevV = nodeV(xs, e.n[0]) - nodeV(xs, e.n[1]);
            if (e.type == ElemType::Inductor) e.prevI = xs[static_cast<size_t>(e.branch)];
        }
    };
    updateState(x);

    const auto& nets = sch_.nets();
    res.netVoltages.assign(nets.size(), {});
    auto record = [&](double t, double h) {
        res.time.push_back(t);
        for (size_t i = 0; i < nets.size(); ++i) res.netVoltages[i].push_back(nodeV(x, netToNode_[i]));
        for (const auto& r : readings(x, h)) res.currents[r.componentId].push_back(r.current);
    };
    record(0, 0);

    int steps = static_cast<int>(std::ceil(tStop / tStep - 1e-9));
    for (int s = 1; s <= steps; ++s) {
        double t = std::min(s * tStep, tStop);
        std::vector<double> guess = x;
        if (!solve(t, tStep, x, iters, 0.0, 1.0)) {
            // Retry with sub-steps.
            x = guess;
            bool ok = true;
            const int sub = 10;
            double h = tStep / sub;
            for (int k = 1; k <= sub && ok; ++k) {
                ok = solve(t - tStep + k * h, h, x, iters, 0.0, 1.0);
                if (ok) updateState(x);
            }
            if (!ok) {
                res.error = "Transient analysis failed to converge at t = " + formatEngineeringValue(t, "s");
                return res;
            }
        }
        // Readings use the pre-update state so capacitor currents are correct, then advance.
        record(t, tStep);
        updateState(x);
    }
    res.ok = true;
    return res;
}

}  // namespace sieda
