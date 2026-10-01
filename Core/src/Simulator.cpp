#include "sieda/Simulator.hpp"
#include "sieda/Avr.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/DeviceModels.hpp"

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

/// exp(x) continued linearly above `kMax` so Newton steps cannot overflow. The knee must sit above any realistic
/// operating point: for a junction with saturation current `is` that is ln(1 A / is), which matters for wide-gap
/// LEDs (blue/white, is ≈ 1e-26) whose forward drop needs exponents near 60.
double limexp(double x, double kMax = 40.0) {
    return x < kMax ? std::exp(x) : std::exp(kMax) * (1.0 + x - kMax);
}

double junctionLimit(double is) { return std::max(40.0, std::log(1.0 / std::max(is, 1e-300))); }

enum class ElemType { Resistor, Capacitor, Inductor, VSource, ISource, Diode, NPN, NMOS, OpAmp, Regulator, Load, McuPin };

// Microcontroller pins: 25 Ω push-pull outputs, 35 kΩ pull-ups (ATmega328P datasheet typical values).
constexpr double kMcuOutputConductance = 1.0 / 25.0;
constexpr double kMcuPullupConductance = 1.0 / 35000.0;

// Smooth max(0, z) with a 20 mV knee (keeps Newton derivatives continuous).
double softplus(double z) {
    constexpr double s = 0.02;
    return z / s > 30 ? z : s * std::log1p(std::exp(z / s));
}
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
    // Behavioural regulator (in, out, ref): CV with dropout / CC at ilimit / off when it would have to sink.
    double dropout = 0.3, iq = 0, ilimit = 1.0, rout = 0.01;
    mutable int mode = 0;  // 0 CV, 1 CC, 2 off
    int sub = 0;           // element index within a custom part (0 = regulator, 1… = supply loads)
    bool isSwitch = false;
    // Microcontroller pin (McuPin): n = {pin, GND, VCC}; conductances from the firmware's drive over the last step.
    int mcu = -1, mcuPin = -1;
    double gHigh = 0, gLow = 0, gPull = 0;
    // Transient state
    double prevV = 0, prevI = 0;

    /// Regulated output for input headroom vi (V_in − V_ref): min(vout, vi − dropout), never negative.
    double setpoint(double vi) const { return softplus(value - softplus(value - (vi - dropout))); }
};

struct Simulator::McuState {
    int componentId = -1;
    std::string name;
    std::unique_ptr<AvrMcu> mcu;
    double clockHz = 16e6;
    double carry = 0;
    int vcc = -1, gnd = -1, aref = -1;  // unknown indices (-1 is also the ground node)
    bool powered = false;               // VCC and GND pins are both wired
    std::vector<std::pair<int, size_t>> pins;  // (pin index, element index)
    std::string loadError;
    bool hasFirmware = false;
};

namespace {
using Elem = Simulator::Element;

double nodeV(const std::vector<double>& x, int i) { return i < 0 ? 0.0 : x[static_cast<size_t>(i)]; }

// Terminal currents flowing *into* a non-linear device from each terminal.
std::array<double, 3> deviceCurrents(const Elem& e, const std::array<double, 3>& v) {
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

int terminalCount(ElemType t) { return t == ElemType::Diode || t == ElemType::Load ? 2 : 3; }

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
    mcus_.clear();
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
                auto v = parseEngineeringValue(primaryValue(c.value));
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
                e.isSwitch = true;
                break;
            }
            case ComponentKind::Capacitor:
            case ComponentKind::Inductor: {
                twoTerminal(c.kind == ComponentKind::Capacitor ? ElemType::Capacitor : ElemType::Inductor);
                auto v = parseEngineeringValue(primaryValue(c.value));
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
                } else if (const DeviceModel* m = findDeviceModel(c.kind, c.value)) {
                    e.is = m->is;
                    e.emission = m->emission;
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
                if (const DeviceModel* m = findDeviceModel(c.kind, c.value)) e.betaF = m->betaF;
                break;
            case ComponentKind::NMOS:
                e.type = ElemType::NMOS;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};
                e.vth = 1.6;  // generic small-signal (2N7002-like)
                e.kp = 0.1;
                if (const DeviceModel* m = findDeviceModel(c.kind, c.value)) {
                    e.vth = m->vth;
                    e.kp = m->kp;
                    e.lambda = m->lambda;
                }
                break;
            case ComponentKind::OpAmp:
                e.type = ElemType::OpAmp;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};  // IN+, IN-, OUT
                e.branch = unknowns_++;
                break;
            case ComponentKind::Custom: {
                const CustomPart* part = CustomPartRegistry::instance().find(c.customPart);
                if (!part || part->spec.model.empty()) continue;  // no behavioural model
                const auto& spec = part->spec;
                auto pinNode = [&](const std::string& key) {
                    int idx = spec.pinIndex(key);
                    return idx < 0 ? -1 : node(c.id, idx);
                };
                if (spec.model.hasRegulator) {
                    const auto& r = spec.model.regulator;
                    Element reg = e;
                    reg.type = ElemType::Regulator;
                    reg.n = {pinNode(r.in), pinNode(r.out), pinNode(r.ref)};
                    reg.value = r.vout;
                    reg.dropout = r.dropout;
                    reg.iq = r.iq;
                    reg.ilimit = r.ilimit;
                    reg.branch = unknowns_++;
                    elements_.push_back(reg);
                }
                int sub = 1;
                for (const auto& l : spec.model.loads) {
                    Element load = e;
                    load.type = ElemType::Load;
                    load.n = {pinNode(l.supply), pinNode(l.ret), -1};
                    load.value = l.current;
                    load.sub = sub++;
                    elements_.push_back(load);
                }
                if (auto model = mcuModelForPart(spec.name)) {
                    auto st = std::make_unique<McuState>();
                    st->componentId = c.id;
                    st->mcu = std::make_unique<AvrMcu>(*model);
                    st->clockHz = c.clockHz > 0 ? c.clockHz : defaultMcuClock(*model);
                    st->name = c.firmwareName.empty() ? std::string("firmware") : c.firmwareName;
                    std::vector<std::pair<int, int>> pinNodes;  // (pin index, node)
                    bool vccWired = false, gndWired = false;
                    for (size_t i = 0; i < spec.pins.size(); ++i) {
                        std::string pn;
                        for (char ch : spec.pins[i].name) pn += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                        int net = sch_.netOf({c.id, static_cast<int>(i)});
                        // A pin alone on its net is not wired (the second GND pin of a DIP-28 left open, …).
                        bool wired = net >= 0 && sch_.nets()[static_cast<size_t>(net)].pins.size() > 1;
                        int nd = node(c.id, static_cast<int>(i));
                        if (pn == "VCC" && wired && !vccWired) {
                            st->vcc = nd;
                            vccWired = true;
                        } else if (pn == "GND" && wired && !gndWired) {
                            st->gnd = nd;
                            gndWired = true;
                        } else if (pn == "AREF" && wired) {
                            st->aref = nd;
                        }
                        int pin = st->mcu->pinIndex(spec.pins[i].name);
                        if (pin >= 0 && wired) pinNodes.push_back({pin, nd});
                    }
                    st->powered = vccWired && gndWired;
                    for (const auto& [pin, nd] : pinNodes) {
                        Element pe{};
                        pe.type = ElemType::McuPin;
                        pe.componentId = c.id;
                        pe.mcu = static_cast<int>(mcus_.size());
                        pe.mcuPin = pin;
                        pe.sub = 1000 + pin;
                        pe.n = {nd, st->gnd, st->vcc};
                        st->pins.push_back({pin, elements_.size()});
                        elements_.push_back(pe);
                    }
                    if (!c.firmware.empty()) {
                        HexImage img = parseIntelHex(c.firmware);
                        std::string err = img.error;
                        if (img.ok() && !st->mcu->loadFirmware(img.bytes, err)) img.error = err;
                        if (!err.empty()) st->loadError = err;
                        else st->hasFirmware = true;
                    }
                    mcus_.push_back(std::move(st));
                }
                continue;
            }
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
            case ElemType::McuPin:
                if (e.gHigh + e.gPull > 0) conductance(a, e.n[2], e.gHigh + e.gPull);
                if (e.gLow > 0) conductance(a, bb, e.gLow);
                break;
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
            case ElemType::Regulator: {
                int in = e.n[0], out = e.n[1], ref = e.n[2], k = e.branch;
                double vin = nodeV(x, in), vo = nodeV(x, out) - nodeV(x, ref), vi = vin - nodeV(x, ref);
                double k0 = x[static_cast<size_t>(k)], isrc = -k0, vset = e.setpoint(vi);
                // Operating mode from the present iterate.
                if (e.mode == 0 && isrc > e.ilimit) e.mode = 1;
                else if (e.mode == 0 && isrc < -1e-6) e.mode = 2;
                else if (e.mode == 1 && vo > vset + 1e-6) e.mode = 0;
                else if (e.mode == 2 && vo < vset - 1e-6) e.mode = 0;
                // KCL: k is the current leaving `out` into the regulator (negative while sourcing); the input
                // supplies it plus the quiescent current, which returns through `ref`.
                addA(out, k, 1);
                addA(in, k, -1);
                addB(in, -e.iq);
                addB(ref, e.iq);
                if (e.mode == 1) {         // constant current: −k = ilimit
                    addA(k, k, -1);
                    addB(k, e.ilimit);
                } else if (e.mode == 2) {  // off: k = 0
                    addA(k, k, 1);
                } else {                   // V(out) − V(ref) − setpoint(V(in) − V(ref)) − rout·k = 0
                    const double h = 1e-6;
                    double d = (e.setpoint(vi + h) - e.setpoint(vi - h)) / (2 * h);
                    addA(k, out, 1);
                    addA(k, in, -d);
                    addA(k, ref, -1 + d);
                    addA(k, k, -e.rout);
                    addB(k, vset - d * vi);
                }
                break;
            }
            case ElemType::Diode:
            case ElemType::NPN:
            case ElemType::NMOS:
            case ElemType::Load: {
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
        if (e.type == ElemType::McuPin) continue;  // part of the microcontroller (its supply load is the reading)
        DeviceReading r;
        r.componentId = e.componentId;
        double va = nodeV(x, e.n[0]), vb = nodeV(x, e.n[1]);
        r.voltage = va - vb;
        switch (e.type) {
            case ElemType::McuPin: break;
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
            case ElemType::Regulator: {
                double vin = va, vout = vb, vref = nodeV(x, e.n[2]);
                r.current = -x[static_cast<size_t>(e.branch)];  // output current
                r.voltage = vin - vout;
                r.power = (vin - vout) * std::max(0.0, r.current) + (vin - vref) * e.iq;
                r.state = e.mode;
                if (e.mode == 0 && (vout - vref) < e.value - 0.02 && vin - vref > 0.1) r.state = 3;  // dropout
                break;
            }
            case ElemType::Diode:
            case ElemType::NPN:
            case ElemType::NMOS:
            case ElemType::Load: {
                std::array<double, 3> v{va, vb, nodeV(x, e.n[2])};
                auto i = deviceCurrents(e, v);
                r.subIndex = e.sub;
                if (e.type == ElemType::Diode || e.type == ElemType::Load) {
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

void Simulator::updateState() {
    for (auto& e : elements_) {
        e.prevV = nodeV(x_, e.n[0]) - nodeV(x_, e.n[1]);
        if (e.type == ElemType::Inductor) e.prevI = x_[static_cast<size_t>(e.branch)];
    }
}

bool Simulator::begin(std::string& error) {
    started_ = false;
    t_ = 0;
    if (!build(error)) return false;
    // Initial condition: DC operating point with t = 0 source values (microcontrollers still in reset).
    x_.assign(static_cast<size_t>(unknowns_), 0.0);
    int iters = 0;
    if (!solve(0, 0, x_, iters, 0.0, 1.0)) {
        std::fill(x_.begin(), x_.end(), 0.0);
        bool ok = true;
        for (double g = 1e-2; g >= 1e-13 && ok; g /= 10) ok = solve(0, 0, x_, iters, g, 1.0);
        if (!ok || !solve(0, 0, x_, iters, 0.0, 1.0)) {
            error = "Could not find initial operating point for transient analysis.";
            return false;
        }
    }
    lastReadings_ = readings(x_, 0);
    updateState();
    started_ = true;
    return true;
}

void Simulator::stepMcus(double h) {
    for (auto& st : mcus_) {
        AvrMcu& m = *st->mcu;
        double gnd = nodeV(x_, st->gnd);
        m.setSupply(st->powered ? nodeV(x_, st->vcc) - gnd : 0.0);
        if (st->aref >= 0) m.setAref(nodeV(x_, st->aref) - gnd);
        for (const auto& [pin, idx] : st->pins) m.setPinVoltage(pin, nodeV(x_, elements_[idx].n[0]) - gnd);
        double cycles = h * st->clockHz / m.clockDivider() + st->carry;
        double whole = std::floor(cycles);
        st->carry = cycles - whole;
        m.run(static_cast<uint64_t>(whole));
        for (const auto& [pin, idx] : st->pins) {
            AvrMcu::PinDrive d = m.pinDrive(pin);
            Element& e = elements_[idx];
            e.gHigh = d.high * kMcuOutputConductance;
            e.gLow = d.low * kMcuOutputConductance;
            e.gPull = d.pullup * kMcuPullupConductance;
        }
    }
}

bool Simulator::advance(double h, std::string& error) {
    if (!started_) {
        error = "The simulation has not started.";
        return false;
    }
    if (!(h > 0)) {
        error = "The time step must be positive.";
        return false;
    }
    stepMcus(h);
    const double t = t_ + h;
    std::vector<double> guess = x_;
    int iters = 0;
    if (solve(t, h, x_, iters, 0.0, 1.0)) {
        lastReadings_ = readings(x_, h);  // pre-update state: capacitor currents use the previous voltage
        updateState();
    } else {
        // Retry with sub-steps.
        x_ = guess;
        const int sub = 10;
        double hs = h / sub;
        for (int k = 1; k <= sub; ++k) {
            if (!solve(t - h + k * hs, hs, x_, iters, 0.0, 1.0)) {
                error = "Transient analysis failed to converge at t = " + formatEngineeringValue(t, "s");
                return false;
            }
            if (k == sub) lastReadings_ = readings(x_, hs);
            updateState();
        }
    }
    t_ = t;
    return true;
}

std::vector<double> Simulator::netVoltages() const {
    const auto& nets = sch_.nets();
    std::vector<double> v(nets.size(), 0.0);
    if (x_.empty()) return v;
    for (size_t i = 0; i < nets.size(); ++i) v[i] = nodeV(x_, netToNode_[i]);
    return v;
}

void Simulator::setSwitch(int componentId, bool closed) {
    for (auto& e : elements_)
        if (e.isSwitch && e.componentId == componentId) e.value = closed ? 0.01 : 1e9;
}

void Simulator::feedSerial(int componentId, const std::string& bytes) {
    for (auto& st : mcus_)
        if (st->componentId == componentId) st->mcu->feedSerialInput(bytes);
}

std::vector<McuReport> Simulator::mcuReports() const {
    std::vector<McuReport> out;
    for (const auto& st : mcus_) {
        const AvrMcu& m = *st->mcu;
        McuReport r;
        r.componentId = st->componentId;
        r.model = mcuModelName(m.model());
        r.serial = m.serialLog();
        r.cycles = m.cycles();
        r.clockHz = st->clockHz;
        char mhz[32];
        std::snprintf(mhz, sizeof mhz, "%g MHz", st->clockHz / 1e6);
        if (!st->loadError.empty()) {
            r.status = "Firmware error: " + st->loadError;
        } else if (!st->hasFirmware) {
            r.status = "No firmware loaded — the pins are high-impedance inputs. Upload a .hex file in the inspector.";
        } else if (!st->powered) {
            r.status = "VCC or GND is not connected — the chip has no supply.";
        } else if (m.supply() < 1.8) {
            char buf[96];
            std::snprintf(buf, sizeof buf, "Held in reset: supply %.2f V (needs at least 1.8 V).", m.supply());
            r.status = buf;
        } else {
            r.running = true;
            r.status = "Running " + st->name + " at " + mhz;
            if (!m.fault().empty()) r.status += " — " + m.fault();
            else if (m.sleeping()) r.status += " (sleeping)";
        }
        out.push_back(std::move(r));
    }
    return out;
}

TransientResult Simulator::transient(double tStop, double tStep) {
    TransientResult res;
    if (!(tStop > 0) || !(tStep > 0)) {
        res.error = "Transient analysis needs positive stop time and step.";
        return res;
    }
    const double maxSamples = 200000;
    if (tStop / tStep > maxSamples) tStep = tStop / maxSamples;

    if (!begin(res.error)) return res;

    const auto& nets = sch_.nets();
    res.netVoltages.assign(nets.size(), {});
    auto record = [&](double t) {
        res.time.push_back(t);
        for (size_t i = 0; i < nets.size(); ++i) res.netVoltages[i].push_back(nodeV(x_, netToNode_[i]));
        for (const auto& r : lastReadings_) {
            if (r.subIndex > 0) continue;  // one series per component (regulator or first element)
            res.currents[r.componentId].push_back(r.current);
            res.powers[r.componentId].push_back(r.power);
        }
    };
    record(0);

    int steps = static_cast<int>(std::ceil(tStop / tStep - 1e-9));
    for (int s = 1; s <= steps; ++s) {
        double t = std::min(s * tStep, tStop);
        if (!advance(t - t_, res.error)) {
            res.mcus = mcuReports();
            return res;
        }
        record(t);
    }
    res.mcus = mcuReports();
    res.ok = true;
    return res;
}

}  // namespace sieda
