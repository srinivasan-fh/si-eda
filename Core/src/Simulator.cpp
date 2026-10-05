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
#include "SimulatorInternal.hpp"

namespace sieda {

using namespace simdetail;

// ------------------------------------------------------------------ source specs

std::optional<SourceSpec> SourceSpec::parse(const std::string& raw) {
    // "<waveform> AC mag [phase]": split off the small-signal stimulus (a standalone "AC" word), parse the rest.
    for (size_t pos = 0; pos + 1 < raw.size(); ++pos) {
        auto up = [&](size_t i) { return static_cast<char>(std::toupper(static_cast<unsigned char>(raw[i]))); };
        if (up(pos) != 'A' || up(pos + 1) != 'C') continue;
        bool startOk = pos == 0 || std::isspace(static_cast<unsigned char>(raw[pos - 1])) || raw[pos - 1] == ')';
        bool endOk = pos + 2 == raw.size() || std::isspace(static_cast<unsigned char>(raw[pos + 2]));
        if (!startOk || !endOk) continue;
        std::string tail = raw.substr(pos + 2);
        for (char& c : tail)
            if (c == ',') c = ' ';
        std::istringstream ts(tail);
        std::vector<double> nums;
        std::string tok;
        while (ts >> tok) {
            auto v = parseEngineeringValue(tok);
            if (!v) return std::nullopt;
            nums.push_back(*v);
        }
        if (nums.empty() || nums.size() > 2) return std::nullopt;
        std::string head = raw.substr(0, pos);
        bool headEmpty = head.find_first_not_of(" \t") == std::string::npos;
        std::optional<SourceSpec> s = headEmpty ? std::optional<SourceSpec>(SourceSpec{}) : parse(head);
        if (!s || s->hasAc) return std::nullopt;
        s->hasAc = true;
        s->acMagnitude = nums[0];
        s->acPhaseDeg = nums.size() > 1 ? nums[1] : 0.0;
        return s;
    }

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



/// Gain–bandwidth product of an op-amp from its value: "GBW=10MEG" explicitly, else the typical datasheet figure of a
/// known part number (prefix match), else 1 MHz.
double opAmpGainBandwidth(const std::string& value) {
    std::string v;
    for (char ch : value) v += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    if (auto pos = v.find("GBW="); pos != std::string::npos) {
        std::string rest = value.substr(pos + 4);
        rest = rest.substr(0, rest.find_first_of(" \t,;"));
        if (auto g = parseEngineeringValue(rest); g && *g > 0) return *g;
    }
    static const std::pair<const char*, double> kParts[] = {
        {"LM358", 1e6},   {"LM2904", 1e6},  {"LM324", 1e6},    {"LM741", 1e6},   {"UA741", 1e6},   {"LMV321", 1e6},
        {"LMV358", 1e6},  {"LMV324", 1e6},  {"MCP6001", 1e6},  {"MCP6002", 1e6}, {"MCP6004", 1e6}, {"TLV900", 1e6},
        {"TL07", 3e6},    {"TL08", 3e6},    {"NE5532", 10e6},  {"NE5534", 10e6}, {"OPA134", 8e6},  {"OPA2134", 8e6},
        {"OPA4134", 8e6}, {"OPA333", 350e3}, {"OPA2333", 350e3}, {"MCP6021", 10e6}, {"AD8605", 10e6}, {"AD8606", 10e6},
        {"AD8628", 2.5e6}, {"OPA1612", 40e6}, {"OPA350", 38e6}, {"LM833", 15e6},  {"OP07", 0.6e6},
    };
    size_t best = 0;
    double gbw = 1e6;
    for (const auto& [part, hz] : kParts) {
        std::string p = part;
        if (v.compare(0, p.size(), p) == 0 && p.size() > best) {
            best = p.size();
            gbw = hz;
        }
    }
    return gbw;
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
    internalNodes_.clear();
    rails_.clear();
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

    // Σ 1/R of the resistors connected directly between two nets (0 when none, or either net is unconnected).
    auto inverseResistanceBetween = [&](int netA, int netB) {
        double g = 0;
        if (netA < 0 || netB < 0 || netA == netB) return g;
        for (const auto& r : sch_.components()) {
            if (r.kind != ComponentKind::Resistor || sch_.omitsFromSimulation(r)) continue;
            int a = sch_.netOf({r.id, 0}), b = sch_.netOf({r.id, 1});
            if (!((a == netA && b == netB) || (a == netB && b == netA))) continue;
            if (auto v = parseEngineeringValue(primaryValue(r.value)); v && *v > 0) g += 1.0 / *v;
        }
        return g;
    };

    auto scaleOf = [&](int id) {
        auto it = valueScale_.find(id);
        return it == valueScale_.end() ? 1.0 : it->second;
    };

    for (const auto& c : sch_.components()) {
        if (sch_.omitsFromSimulation(c)) continue;  // not fitted in the assembly being simulated
        Element e{};
        e.componentId = c.id;
        if (!c.spice.empty() && spiceModelApplies(c)) {  // an imported SPICE model replaces the built-in one
            if (!addSpiceModel(c, error)) return false;
            continue;
        }
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
                if (c.kind == ComponentKind::Resistor) e.value *= scaleOf(c.id);
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
                e.value = *v * scaleOf(c.id);
                if (e.type == ElemType::Inductor) e.branch = unknowns_++;
                break;
            }
            case ComponentKind::VoltageSource:
            case ComponentKind::Battery:
            case ComponentKind::ACSource:
            case ComponentKind::CurrentSource: {
                twoTerminal(isVoltageSourceKind(c.kind) ? ElemType::VSource : ElemType::ISource);
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
                if (opAmpMacromodelRequested(c.value)) {  // SR=, P2=, VOH= …: the multi-pole dynamic model
                    if (!addOpAmpMacromodel(c, opAmpGainBandwidth(c.value), error)) return false;
                    continue;
                }
                e.type = ElemType::OpAmp;
                e.n = {node(c.id, 0), node(c.id, 1), node(c.id, 2)};  // IN+, IN-, OUT
                e.branch = unknowns_++;
                e.gbw = opAmpGainBandwidth(c.value);
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
                    if (r.isolated()) {
                        reg.isolated = true;
                        reg.efficiency = r.efficiency;
                        reg.aux = {pinNode(r.inReturn), -1, -1};
                    }
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
                if (std::string upper = spec.name;
                    (std::transform(upper.begin(), upper.end(), upper.begin(),
                                    [](unsigned char ch) { return static_cast<char>(std::toupper(ch)); }),
                     upper == "INA333") &&
                    spec.pins.size() >= 8) {
                    // Pins: 1 RG, 2 −IN, 3 +IN, 4 V−, 5 REF, 6 VOUT, 7 V+, 8 RG. Gain = 1 + 100 kΩ / RG, where RG is
                    // the (parallel) resistance of the resistors between the two RG pins; open RG gives unity gain.
                    Element amp = e;
                    amp.type = ElemType::InAmp;
                    amp.n = {node(c.id, 2), node(c.id, 1), node(c.id, 5)};
                    amp.aux = {node(c.id, 4), node(c.id, 6), node(c.id, 3)};
                    amp.value = 1.0 + 100e3 * inverseResistanceBetween(sch_.netOf({c.id, 0}), sch_.netOf({c.id, 7}));
                    amp.branch = unknowns_++;
                    amp.sub = sub++;
                    elements_.push_back(amp);
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
    strictNewton_ = false;
    for (const auto& el : elements_) strictNewton_ = strictNewton_ || el.internal;
    isNode_.assign(static_cast<size_t>(unknowns_), 0);
    for (int i = 0; i < nodeCount_; ++i) isNode_[static_cast<size_t>(i)] = 1;
    for (int i : internalNodes_) isNode_[static_cast<size_t>(i)] = 1;
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
    for (int i : internalNodes_) addA(i, i, kGmin + gminExtra);

    for (const auto& e : elements_) {
        int a = e.n[0], bb = e.n[1];
        switch (e.type) {
            case ElemType::Resistor: conductance(a, bb, 1.0 / e.value); break;
            case ElemType::McuPin:
                if (e.gHigh + e.gPull > 0) conductance(a, e.n[2], e.gHigh + e.gPull);
                if (e.gLow > 0) conductance(a, bb, e.gLow);
                break;
            case ElemType::Capacitor:
                if (h > 0 && trap_) {  // trapezoidal: i = 2C/h·(v − v_prev) − i_prev
                    double geq = 2.0 * e.value / h;
                    conductance(a, bb, geq);
                    addB(a, geq * e.prevV + e.prevIc);
                    addB(bb, -(geq * e.prevV + e.prevIc));
                } else if (h > 0) {
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
                if (h > 0 && trap_) {  // trapezoidal: v + v_prev = 2L/h·(i − i_prev)
                    double req = 2.0 * e.value / h;
                    addA(k, k, -req);
                    addB(k, -req * e.prevI - e.prevV);
                } else if (h > 0) {
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
            case ElemType::InAmp: {
                // f = Vo − g(V+IN, V−IN, REF, V+, V−) = 0, linearised with numerical partial derivatives.
                int o = e.n[2], k = e.branch;
                std::array<int, 5> ctl{{e.n[0], e.n[1], e.aux[0], e.aux[1], e.aux[2]}};
                std::array<double, 5> v{};
                for (size_t i = 0; i < 5; ++i) v[i] = nodeV(x, ctl[i]);
                auto g = [&](const std::array<double, 5>& u) { return Element::inAmpOut(e.value, u[0], u[1], u[2], u[3], u[4]); };
                double g0 = g(v), rhs = g0;
                addA(o, k, 1);
                addA(k, o, 1);
                for (size_t i = 0; i < 5; ++i) {
                    if (ctl[i] < 0) continue;
                    const double h = 1e-7;
                    auto up = v, dn = v;
                    up[i] += h;
                    dn[i] -= h;
                    double d = (g(up) - g(dn)) / (2 * h);
                    addA(k, ctl[i], -d);
                    rhs -= d * v[i];
                }
                addB(k, rhs);
                break;
            }
            case ElemType::Regulator: {
                int in = e.n[0], out = e.n[1], ref = e.n[2], k = e.branch;
                const int inRet = e.isolated ? e.aux[0] : ref;  // the input side's return
                double vin = nodeV(x, in), vo = nodeV(x, out) - nodeV(x, ref), vi = vin - nodeV(x, inRet);
                double k0 = x[static_cast<size_t>(k)], isrc = -k0, vset = e.setpoint(vi);
                // Operating mode from the present iterate.
                if (e.mode == 0 && isrc > e.ilimit) e.mode = 1;
                else if (e.mode == 0 && isrc < -1e-6) e.mode = 2;
                else if (e.mode == 1 && vo > vset + 1e-6) e.mode = 0;
                else if (e.mode == 2 && vo < vset - 1e-6) e.mode = 0;
                // KCL: k is the current leaving `out` into the regulator (negative while sourcing); the input
                // supplies it plus the quiescent current, which returns through `ref`.
                addA(out, k, 1);
                if (e.isolated) {
                    // Secondary: the output current returns through ref. Primary: power balance, linearised on the
                    // present iterate (input current = V_set·I_out / (efficiency·V_in)).
                    addA(ref, k, -1);
                    const double g = vset / (e.efficiency * std::max(vi, 1.0));
                    addA(in, k, -g);
                    addA(inRet, k, g);
                } else {
                    addA(in, k, -1);
                }
                addB(in, -e.iq);
                addB(inRet, e.iq);
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
                    if (e.isolated) {
                        addA(k, ref, -1);
                        addA(k, inRet, d);
                    } else {
                        addA(k, ref, -1 + d);
                    }
                    addA(k, k, -e.rout);
                    addB(k, vset - d * vi);
                }
                break;
            }
            case ElemType::Device:
            case ElemType::Ctrl:
            case ElemType::Coupling:
                stampModelElement(e, t, h, x);
                break;
            case ElemType::ModelPorts: break;
            case ElemType::Diode:
            case ElemType::NPN:
            case ElemType::NMOS:
            case ElemType::Load: {
                int tc = terminalCount(e.type);
                std::array<double, 3> v0{};
                for (int i = 0; i < tc; ++i) v0[static_cast<size_t>(i)] = nodeV(x, e.n[static_cast<size_t>(i)]);
                if (limitJunctions_ && !noLimit_ && (e.type == ElemType::Diode || e.type == ElemType::NPN))
                    limitBuiltinJunctions(e, v0);
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
    if (limitJunctions_ || strictNewton_)
        for (auto& e : elements_) e.vjValid = false;  // junction limiting starts from this solve's initial guess
    for (int it = 0; it < maxIter; ++it) {
        stamp(t, h, x, gminExtra, sourceScale);
        if (!luSolve(A_, b_, unknowns_)) return false;
        double maxDelta = 0;
        for (int i = 0; i < unknowns_; ++i) {
            double xn = b_[static_cast<size_t>(i)];
            if (!std::isfinite(xn)) return false;
            double delta = xn - x[static_cast<size_t>(i)];
            // Damp node-voltage steps to help exponential devices converge.
            if ((i < nodeCount_ || isNode_[static_cast<size_t>(i)]) && std::fabs(delta) > 2.0) delta = delta > 0 ? 2.0 : -2.0;
            // With imported models the tolerance is relative to where the (damped) step lands, so a far-off undamped
            // target cannot pass for convergence; circuits of built-in models keep their established criterion.
            double tol = 1e-6 + 1e-6 * std::fabs(strictNewton_ ? x[static_cast<size_t>(i)] + delta : xn);
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
        if (e.internal) continue;                   // inside an imported model: ModelPorts is the part's reading
        if (e.type == ElemType::ModelPorts) {
            out.push_back(modelReading(e, x, h));
            continue;
        }
        DeviceReading r;
        r.componentId = e.componentId;
        double va = nodeV(x, e.n[0]), vb = nodeV(x, e.n[1]);
        r.voltage = va - vb;
        switch (e.type) {
            case ElemType::McuPin:
            case ElemType::Device:
            case ElemType::Ctrl:
            case ElemType::Coupling:
            case ElemType::ModelPorts: break;
            case ElemType::Resistor: r.current = r.voltage / e.value; r.power = r.voltage * r.current; break;
            case ElemType::Capacitor:
                if (trap_ && h > 0) r.current = 2.0 * e.value / h * (r.voltage - e.prevV) - e.prevIc;
                else r.current = h > 0 ? e.value / h * (r.voltage - e.prevV) : 0.0;
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
            case ElemType::InAmp:
                r.voltage = nodeV(x, e.n[2]);
                r.current = -x[static_cast<size_t>(e.branch)];
                r.power = 0;
                break;
            case ElemType::Regulator: {
                double vin = va, vout = vb, vref = nodeV(x, e.n[2]);
                r.current = -x[static_cast<size_t>(e.branch)];  // output current
                r.voltage = vin - vout;
                r.power = (vin - vout) * std::max(0.0, r.current) + (vin - vref) * e.iq;
                double headroom = vin - vref;  // input relative to the regulator's input return
                if (e.isolated) {  // voltage = primary input; power = conversion loss
                    headroom = vin - nodeV(x, e.aux[0]);
                    r.voltage = headroom;
                    r.power = (vout - vref) * std::max(0.0, r.current) * (1.0 / e.efficiency - 1.0) + headroom * e.iq;
                }
                r.state = e.mode;
                if (e.mode == 0 && (vout - vref) < e.value - 0.02 && headroom > 0.1) r.state = 3;  // dropout
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

bool Simulator::operatingPoint(std::vector<double>& x, int& iterations) {
    int iters = 0;
    bool ok = solve(0, 0, x, iters, 0.0, 1.0);
    iterations = iters;
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
        iterations += iters;
        if (!ok && (aids_ || strictNewton_)) {  // failed every strategy above: junction limiting, adaptive source stepping
            const bool was = limitJunctions_;
            limitJunctions_ = true;
            std::fill(x.begin(), x.end(), 0.0);
            ok = solve(0, 0, x, iters, 0.0, 1.0);
            iterations += iters;
            if (!ok) ok = adaptiveSourceStepping(x, iterations);
            limitJunctions_ = was;
        }
    }
    return ok;
}

bool Simulator::check(std::string& error) { return build(error); }

DcResult Simulator::dcOperatingPoint() {
    DcResult res;
    if (!build(res.error)) return res;
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    bool ok = operatingPoint(x, res.iterations);
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
        if (e.type == ElemType::Capacitor) {  // capacitor current over the step just taken (trapezoidal history)
            const double v = nodeV(x_, e.n[0]) - nodeV(x_, e.n[1]);
            e.prevIc = !(stepH_ > 0) ? 0.0 : trap_ ? 2.0 * e.value / stepH_ * (v - e.prevV) - e.prevIc : e.value / stepH_ * (v - e.prevV);
        }
        if (e.type == ElemType::Device) updateCharges(e);
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
    stepH_ = 0;
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
        stepH_ = h;
        updateState();
    } else {
        // Retry with sub-steps.
        x_ = guess;
        const int sub = 10;
        double hs = h / sub;
        const std::vector<Element> saved = elements_;  // integration state before the sub-steps
        for (int k = 1; k <= sub; ++k) {
            if (!solve(t - h + k * hs, hs, x_, iters, 0.0, 1.0)) {
                // Last resort: the same step in finer sub-steps with pn-junction limiting.
                elements_ = saved;
                x_ = guess;
                if ((aids_ || strictNewton_) && retryWithLimiting(t - h, h, error)) {
                    t_ = t;
                    return true;
                }
                error = "Transient analysis failed to converge at t = " + formatEngineeringValue(t, "s");
                return false;
            }
            if (k == sub) lastReadings_ = readings(x_, hs);
            stepH_ = hs;
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

// ------------------------------------------------------------------ AC small-signal analysis

namespace {
using Cplx = std::complex<double>;

constexpr double kHalfPowerDb = 3.0102999566398120;  // 10·log10(2): the "−3 dB" point


double wrapDeg(double d) {
    d = std::fmod(d, 360.0);
    if (d > 180.0) d -= 360.0;
    if (d <= -180.0) d += 360.0;
    return d;
}
}  // namespace

double magnitudeDb(std::complex<double> h) {
    double m = std::abs(h);
    return m > 1e-20 ? 20.0 * std::log10(m) : -400.0;
}

std::vector<double> unwrappedPhaseDeg(const std::vector<std::complex<double>>& h) {
    std::vector<double> out;
    out.reserve(h.size());
    for (const auto& v : h) {
        double p = std::arg(v) * 180.0 / kPi;
        if (!out.empty()) p += 360.0 * std::round((out.back() - p) / 360.0);
        out.push_back(p);
    }
    return out;
}

AcMetrics acMetrics(const std::vector<double>& freq, const std::vector<std::complex<double>>& h,
                    const std::function<std::complex<double>(double)>& eval) {
    AcMetrics m;
    const size_t n = std::min(freq.size(), h.size());
    if (n == 0) return m;
    std::vector<double> f(freq.begin(), freq.begin() + static_cast<std::ptrdiff_t>(n)), db(n);
    for (size_t i = 0; i < n; ++i) db[i] = magnitudeDb(h[i]);
    const std::vector<double> phase =
        unwrappedPhaseDeg(std::vector<Cplx>(h.begin(), h.begin() + static_cast<std::ptrdiff_t>(n)));
    auto dbAt = [&](double u) { return magnitudeDb(eval(std::pow(10.0, u))); };

    // Frequency between samples i and i+1 where the gain equals `target` (the samples bracket it): linear in
    // (log f, dB), refined on the circuit itself with the Illinois variant of regula falsi when `eval` is given.
    auto crossing = [&](size_t i, double target) {
        double u0 = std::log10(f[i]), u1 = std::log10(f[i + 1]);
        double g0 = db[i] - target, g1 = db[i + 1] - target;
        double u = g0 == g1 ? u0 : u0 + (u1 - u0) * g0 / (g0 - g1);
        if (eval) {
            int side = 0;
            for (int it = 0; it < 60; ++it) {
                double g = dbAt(u) - target;
                if (std::fabs(g) < 1e-10) break;
                if ((g > 0) == (g0 > 0)) {
                    u0 = u;
                    g0 = g;
                    if (side == -1) g1 *= 0.5;
                    side = -1;
                } else {
                    u1 = u;
                    g1 = g;
                    if (side == 1) g0 *= 0.5;
                    side = 1;
                }
                if (u1 - u0 < 1e-13 || g0 == g1) break;
                u = (u0 * g1 - u1 * g0) / (g1 - g0);
            }
        }
        return std::pow(10.0, u);
    };

    m.lowFreqDb = db[0];
    size_t imax = 0;
    for (size_t i = 1; i < n; ++i)
        if (db[i] > db[imax]) imax = i;
    m.peakDb = db[imax];
    m.peakHz = f[imax];
    const bool interiorPeak = imax > 0 && imax + 1 < n && db[imax] > db[0] + 1e-3 && db[imax] > db[n - 1] + 1e-3;
    if (interiorPeak) {
        double ua = std::log10(f[imax - 1]), ub = std::log10(f[imax + 1]);
        double peakU = std::log10(f[imax]), peakDb = db[imax];
        if (eval) {  // golden-section search for the maximum
            const double r = 0.6180339887498949;
            double c = ub - r * (ub - ua), d = ua + r * (ub - ua), fc = dbAt(c), fd = dbAt(d);
            for (int it = 0; it < 60 && ub - ua > 1e-11; ++it) {
                if (fc > fd) {
                    ub = d;
                    d = c;
                    fd = fc;
                    c = ub - r * (ub - ua);
                    fc = dbAt(c);
                } else {
                    ua = c;
                    c = d;
                    fc = fd;
                    d = ua + r * (ub - ua);
                    fd = dbAt(d);
                }
            }
            double u = 0.5 * (ua + ub), v = dbAt(u);
            if (v > peakDb) {
                peakU = u;
                peakDb = v;
            }
        } else {  // vertex of the parabola through the three samples (dB over log f)
            double y0 = db[imax - 1], y1 = db[imax], y2 = db[imax + 1];
            double h0 = peakU - ua, h1 = ub - peakU;
            double denom = h0 * h1 * (h0 + h1);
            double a = (h0 * (y2 - y1) + h1 * (y0 - y1)) / denom;  // y ≈ y1 + b·t + a·t²
            double b = (h0 * h0 * (y2 - y1) - h1 * h1 * (y0 - y1)) / denom;
            if (a < 0) {
                double t = std::clamp(-b / (2 * a), -h0, h1);
                peakU += t;
                peakDb = y1 + b * t + a * t * t;
            }
        }
        m.peakHz = std::pow(10.0, peakU);
        m.peakDb = peakDb;
        f[imax] = m.peakHz;  // the refined peak replaces its sample so the half-power searches bracket it
        db[imax] = m.peakDb;
    }

    const double target3 = m.lowFreqDb - kHalfPowerDb;
    for (size_t i = 0; i + 1 < n; ++i)
        if (db[i] > target3 && db[i + 1] <= target3) {
            m.f3dbHz = crossing(i, target3);
            break;
        }
    if (interiorPeak) {
        const double target = m.peakDb - kHalfPowerDb;
        for (size_t j = imax; j > 0; --j)
            if (db[j - 1] <= target && db[j] > target) {
                m.bwLowHz = crossing(j - 1, target);
                break;
            }
        for (size_t j = imax; j + 1 < n; ++j)
            if (db[j] > target && db[j + 1] <= target) {
                m.bwHighHz = crossing(j, target);
                break;
            }
    }
    for (size_t i = 0; i + 1 < n; ++i)
        if (db[i] >= 0 && db[i + 1] < 0 && db[i] - db[i + 1] > 1e-9) {  // a real fall, not round-off on a flat 0 dB
            double fu = crossing(i, 0.0);
            double t = (std::log10(fu) - std::log10(f[i])) / (std::log10(f[i + 1]) - std::log10(f[i]));
            double p = phase[i] + t * (phase[i + 1] - phase[i]);
            if (eval) {
                double exact = std::arg(eval(fu)) * 180.0 / kPi;
                p = exact + 360.0 * std::round((p - exact) / 360.0);
            }
            m.unityHz = fu;
            m.phaseMarginDeg = wrapDeg(180.0 + p);
            break;
        }
    return m;
}

void Simulator::acMatrix(double freq, const std::vector<double>& G, const std::vector<double>& x,
                         std::vector<std::complex<double>>& M) const {
    using Cplx = std::complex<double>;
    const int n = unknowns_;
    const double w = 2 * kPi * freq;
    M.assign(G.begin(), G.end());
    auto add = [&](int r, int c, Cplx v) {
        if (r >= 0 && c >= 0) M[static_cast<size_t>(r * n + c)] += v;
    };
    for (const auto& e : elements_) {
        int a = e.n[0], bb = e.n[1];
        switch (e.type) {
            case ElemType::Capacitor: {
                Cplx y(0.0, w * e.value);
                add(a, a, y);
                add(bb, bb, y);
                add(a, bb, -y);
                add(bb, a, -y);
                break;
            }
            case ElemType::Inductor: add(e.branch, e.branch, Cplx(0.0, -w * e.value)); break;
            case ElemType::Device:
            case ElemType::Coupling: addModelAdmittance(e, w, x, M); break;
            case ElemType::OpAmp: {
                // Single pole that keeps the gain–bandwidth product: (1 + jf·A'/GBW)·Vo − A'·(V+ − V−) = 0, with
                // A' the open-loop gain at the operating point (falling towards 0 as the output saturates).
                double th = std::tanh(kOpAmpGain * (nodeV(x, e.n[0]) - nodeV(x, e.n[1])) / e.vsat);
                add(e.branch, e.n[2], Cplx(0.0, freq * kOpAmpGain * (1.0 - th * th) / e.gbw));
                break;
            }
            default: break;
        }
    }
}

AcResult Simulator::ac(const AcOptions& o) {
    AcResult res;
    if (!(o.fStart > 0) || !(o.fStop > o.fStart) || !std::isfinite(o.fStop)) {
        res.error = "AC analysis needs a start frequency above 0 and a stop frequency above it.";
        return res;
    }
    if (o.pointsPerDecade < 1 || o.pointsPerDecade > 1000) {
        res.error = "AC analysis needs 1 to 1000 points per decade.";
        return res;
    }
    const double decades = std::log10(o.fStop / o.fStart);
    const double intervals = std::max(1.0, std::ceil(decades * o.pointsPerDecade - 1e-9));
    if (intervals > 20000) {
        res.error = "AC analysis is limited to 20 000 frequency points: narrow the range or use fewer points per decade.";
        return res;
    }
    if (!build(res.error)) return res;
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    int iters = 0;
    if (!operatingPoint(x, iters)) {
        res.error = "AC analysis needs the DC operating point, which did not converge (check for floating nodes or "
                    "unrealistic values).";
        return res;
    }
    const auto& nets = sch_.nets();
    res.dcNetVoltages.assign(nets.size(), 0.0);
    for (size_t i = 0; i < nets.size(); ++i) res.dcNetVoltages[i] = nodeV(x, netToNode_[i]);

    // Stimulus.
    struct Drive {
        size_t elem;
        Cplx phasor;
    };
    std::vector<Drive> drives;
    auto isSource = [](const Element& e) { return !e.internal && (e.type == ElemType::VSource || e.type == ElemType::ISource); };
    auto phasorOf = [](const SourceSpec& s) {
        return s.hasAc ? std::polar(s.acMagnitude, s.acPhaseDeg * kPi / 180.0) : Cplx(1.0, 0.0);
    };
    if (o.sourceId >= 0) {
        for (size_t i = 0; i < elements_.size(); ++i)
            if (elements_[i].componentId == o.sourceId && isSource(elements_[i]))
                drives.push_back({i, phasorOf(elements_[i].source)});
        if (drives.empty()) {
            res.error = "The AC stimulus must be an independent voltage or current source.";
            return res;
        }
    } else {
        for (size_t i = 0; i < elements_.size(); ++i)
            if (isSource(elements_[i]) && elements_[i].source.hasAc && elements_[i].source.acMagnitude != 0)
                drives.push_back({i, phasorOf(elements_[i].source)});
        for (size_t i = 0; i < elements_.size() && drives.empty(); ++i) {
            const Element& e = elements_[i];
            const Component* c = sch_.find(e.componentId);
            if (isSource(e) && (e.source.kind == SourceSpec::Kind::Sine || (c && c->kind == ComponentKind::ACSource)))
                drives.push_back({i, Cplx(1.0, 0.0)});
        }
        if (drives.empty()) {
            res.error = "No AC stimulus: give the input source an AC magnitude (for example \"0 AC 1\" or "
                        "\"SIN(0 1 1k) AC 1\") or choose it as the input.";
            return res;
        }
    }
    for (const auto& d : drives) res.stimulus.push_back(elements_[d.elem].componentId);

    // Small-signal conductance matrix: the Newton Jacobian at the operating point (capacitors open, inductors short).
    noLimit_ = true;  // the Jacobian at the operating point itself
    stamp(0, 0, x, 0.0, 1.0);
    noLimit_ = false;
    const std::vector<double> G = A_;
    const int n = unknowns_;
    std::vector<Cplx> M, rhs;
    auto solveAt = [&](double freq) -> bool {
        acMatrix(freq, G, x, M);
        rhs.assign(static_cast<size_t>(n), Cplx(0.0, 0.0));
        for (const auto& d : drives) {
            const Element& e = elements_[d.elem];
            if (e.type == ElemType::VSource) {
                rhs[static_cast<size_t>(e.branch)] += d.phasor;
            } else {
                if (e.n[0] >= 0) rhs[static_cast<size_t>(e.n[0])] += d.phasor;
                if (e.n[1] >= 0) rhs[static_cast<size_t>(e.n[1])] -= d.phasor;
            }
        }
        return luSolveComplex(M, rhs, n);
    };

    const auto points = static_cast<size_t>(intervals) + 1;
    res.netPhasors.assign(nets.size(), {});
    for (auto& v : res.netPhasors) v.reserve(points);
    for (size_t i = 0; i < points; ++i) {
        double freq = i + 1 == points ? o.fStop
                                      : o.fStart * std::pow(10.0, static_cast<double>(i) / o.pointsPerDecade);
        if (!solveAt(freq)) {
            res.error = "AC analysis: the circuit matrix is singular at " + formatEngineeringValue(freq, "Hz") +
                        " (a floating node or a loop of voltage sources).";
            return res;
        }
        res.frequency.push_back(freq);
        for (size_t k = 0; k < nets.size(); ++k) {
            int nd = netToNode_[k];
            res.netPhasors[k].push_back(nd < 0 ? Cplx(0.0, 0.0) : rhs[static_cast<size_t>(nd)]);
        }
    }

    // Readouts, refined on the circuit while the solve budget lasts (large circuits fall back to interpolation).
    std::vector<int> measure = o.measureNets;
    if (measure.empty())
        for (size_t k = 0; k < nets.size(); ++k)
            if (netToNode_[k] >= 0) measure.push_back(static_cast<int>(k));
    const double cube = static_cast<double>(n) * n * n;
    long budget = static_cast<long>(std::min(1e6, std::max(400.0, 4e9 / std::max(cube, 1.0))));
    for (int net : measure) {
        if (net < 0 || static_cast<size_t>(net) >= nets.size()) continue;
        const int nd = netToNode_[static_cast<size_t>(net)];
        std::function<Cplx(double)> eval;
        if (nd >= 0 && budget > 0)
            eval = [&budget, &solveAt, &rhs, nd](double freq) {
                --budget;
                return solveAt(freq) ? rhs[static_cast<size_t>(nd)] : Cplx(0.0, 0.0);
            };
        res.metrics[net] = acMetrics(res.frequency, res.netPhasors[static_cast<size_t>(net)], eval);
    }
    res.ok = true;
    return res;
}

DcSweepResult Simulator::dcSweep(int componentId, double start, double stop, double step) {
    DcSweepResult res;
    step = std::fabs(step);
    if (!(step > 0) || !std::isfinite(start) || !std::isfinite(stop)) {
        res.error = "The DC sweep needs finite start and stop values and a non-zero step.";
        return res;
    }
    const double span = std::fabs(stop - start);
    if (span / step > 100000) {
        res.error = "The DC sweep is limited to 100 000 points: use a larger step.";
        return res;
    }
    if (!build(res.error)) return res;
    std::vector<size_t> swept;
    for (size_t i = 0; i < elements_.size(); ++i)
        if (elements_[i].componentId == componentId && !elements_[i].internal &&
            (elements_[i].type == ElemType::VSource || elements_[i].type == ElemType::ISource))
            swept.push_back(i);
    const Component* comp = sch_.find(componentId);
    if (swept.empty() || !comp) {
        res.error = "The DC sweep needs an independent voltage or current source.";
        return res;
    }
    const std::string unit = elements_[swept.front()].type == ElemType::VSource ? "V" : "A";
    std::vector<double> values;
    const double dir = stop >= start ? 1.0 : -1.0;
    const auto count = static_cast<size_t>(std::floor(span / step + 1e-9));
    for (size_t i = 0; i <= count; ++i) values.push_back(start + dir * static_cast<double>(i) * step);
    if (span - static_cast<double>(count) * step > 1e-9 * step) values.push_back(stop);

    const auto& nets = sch_.nets();
    res.netVoltages.assign(nets.size(), {});
    std::vector<double> x(static_cast<size_t>(unknowns_), 0.0);
    for (double v : values) {
        for (size_t i : swept) {
            SourceSpec dc;
            dc.dc = v;
            elements_[i].source = dc;
        }
        int iters = 0;
        bool ok = solve(0, 0, x, iters, 0.0, 1.0);  // continuation from the previous point
        if (!ok) {
            std::fill(x.begin(), x.end(), 0.0);
            ok = operatingPoint(x, iters);
        }
        if (!ok) {
            res.error = "DC sweep did not converge at " + comp->ref + " = " + formatEngineeringValue(v, unit) + ".";
            return res;
        }
        res.values.push_back(v);
        for (size_t k = 0; k < nets.size(); ++k) res.netVoltages[k].push_back(nodeV(x, netToNode_[k]));
        for (const auto& r : readings(x, 0))
            if (r.subIndex == 0) res.currents[r.componentId].push_back(r.current);
    }
    res.ok = true;
    return res;
}

}  // namespace sieda
