// Imported SPICE models in the simulator: a part's flattened model (sieda/SpiceModels.hpp) becomes internal elements
// — resistors, capacitors, inductors and sources of the built-in kinds plus Device (D Q M J), Ctrl (E F G H B) and
// Coupling (K) elements — between the part's pins (by its pin map) and internal nodes.
#include <cctype>
#include <functional>
#include <map>

#include "SimulatorInternal.hpp"
#include "sieda/Avr.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/Geometry.hpp"
#include "sieda/Units.hpp"

namespace sieda {

using namespace simdetail;

namespace {
std::string upperCase(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

/// Value of a Ctrl element from the values of its refs, and ∂f/∂ref.
double ctrlEval(const Simulator::Element& e, const std::vector<double>& vals, double t, std::vector<double>& grad) {
    grad.assign(vals.size(), 0.0);
    auto pairValue = [&](const std::array<int, 2>& p) {
        return (p[0] >= 0 ? vals[static_cast<size_t>(p[0])] : 0.0) - (p[1] >= 0 ? vals[static_cast<size_t>(p[1])] : 0.0);
    };
    auto addGrad = [&](const std::array<int, 2>& p, double d) {
        if (p[0] >= 0) grad[static_cast<size_t>(p[0])] += d;
        if (p[1] >= 0) grad[static_cast<size_t>(p[1])] -= d;
    };
    switch (e.ctrl) {
        case CtrlKind::Poly: {
            std::vector<double> u(e.pairs.size());
            for (size_t j = 0; j < u.size(); ++j) u[j] = pairValue(e.pairs[j]);
            std::vector<double> du(u.size(), 0.0);
            double f = 0;
            for (size_t k = 0; k < e.coeffs.size() && k < e.terms.size(); ++k) {
                const auto& term = e.terms[k];
                double prod = e.coeffs[k];
                for (int idx : term) prod *= u[static_cast<size_t>(idx)];
                f += prod;
                for (size_t p = 0; p < term.size(); ++p) {
                    double d = e.coeffs[k];
                    for (size_t q = 0; q < term.size(); ++q)
                        if (q != p) d *= u[static_cast<size_t>(term[q])];
                    du[static_cast<size_t>(term[p])] += d;
                }
            }
            for (size_t j = 0; j < u.size(); ++j) addGrad(e.pairs[j], du[j]);
            return f;
        }
        case CtrlKind::TanhStage: {
            const double u = pairValue(e.pairs[0]), gm = e.coeffs[0], imax = e.coeffs[1];
            const double th = std::tanh(gm * u / imax);
            addGrad(e.pairs[0], gm * (1 - th * th));
            return imax * th;
        }
        case CtrlKind::Clamp: {
            // lo + softplus(u − lo) − softplus(u − hi): linear between the limits, smooth (10 mV) at them.
            const double u = pairValue(e.pairs[0]), lo = e.coeffs[0], hi = e.coeffs[1], s = 0.01;
            auto sp = [&](double z) { return z / s > 30 ? z : s * std::log1p(std::exp(z / s)); };
            auto sig = [&](double z) { return z / s > 30 ? 1.0 : z / s < -30 ? 0.0 : 1.0 / (1.0 + std::exp(-z / s)); };
            addGrad(e.pairs[0], sig(u - lo) - sig(u - hi));
            return lo + sp(u - lo) - sp(u - hi);
        }
        case CtrlKind::DeadZone: {
            const double u = pairValue(e.pairs[0]), g = e.coeffs[0], vc = e.coeffs[1], s = 0.01;
            auto sp = [&](double z) { return z / s > 30 ? z : s * std::log1p(std::exp(z / s)); };
            auto sig = [&](double z) { return z / s > 30 ? 1.0 : z / s < -30 ? 0.0 : 1.0 / (1.0 + std::exp(-z / s)); };
            addGrad(e.pairs[0], g * (sig(u - vc) + sig(-u - vc)));
            return g * (sp(u - vc) - sp(-u - vc));
        }
        case CtrlKind::Expr:
        case CtrlKind::Table: {
            auto value = [&](const std::vector<double>& v) {
                double y = evalSpiceExpression(*e.expr, v.data(), t);
                if (e.ctrl == CtrlKind::Expr) return y;
                const auto& tb = e.table;
                if (y <= tb.front().first) return tb.front().second;
                for (size_t k = 1; k < tb.size(); ++k)
                    if (y <= tb[k].first) {
                        const double x0 = tb[k - 1].first, x1 = tb[k].first;
                        return x1 > x0 ? tb[k - 1].second + (tb[k].second - tb[k - 1].second) * (y - x0) / (x1 - x0)
                                       : tb[k].second;
                    }
                return tb.back().second;
            };
            const double f = value(vals);
            std::vector<double> v = vals;
            for (size_t r = 0; r < v.size(); ++r) {
                const double h = 1e-6 * (1.0 + std::fabs(vals[r]));
                v[r] = vals[r] + h;
                const double up = value(v);
                v[r] = vals[r] - h;
                const double dn = value(v);
                v[r] = vals[r];
                grad[r] = (up - dn) / (2 * h);
                if (!std::isfinite(grad[r])) grad[r] = 0.0;
            }
            return f;
        }
    }
    return 0.0;
}

/// Copies an expression, giving each node-voltage and source-current leaf its slot.
std::shared_ptr<SpiceExpr> withSlots(const SpiceExpr& e, const std::function<int(const std::string&)>& nodeSlot,
                                     const std::function<int(const std::string&)>& currentSlot) {
    auto r = std::make_shared<SpiceExpr>(e);
    if (e.op == SpiceExpr::Op::Voltage) {
        r->slot = nodeSlot(e.name);
        r->slot2 = nodeSlot(e.name2);
    } else if (e.op == SpiceExpr::Op::Current) {
        r->slot = currentSlot(e.name);
    }
    for (auto& a : r->args) a = withSlots(*a, nodeSlot, currentSlot);
    return r;
}

/// Numerical ∂q/∂v of a device at `v` (tc × tc, row-major).
void chargeJacobian(const spicedev::Device& d, const std::array<double, 4>& v, double* C) {
    const int tc = d.terminals;
    const double dv = 1e-6;
    for (int j = 0; j < tc; ++j) {
        std::array<double, 4> vp = v, vm = v, qp{}, qm{};
        vp[static_cast<size_t>(j)] += dv;
        vm[static_cast<size_t>(j)] -= dv;
        d.charges(vp.data(), qp.data());
        d.charges(vm.data(), qm.data());
        for (int r = 0; r < tc; ++r) C[r * tc + j] = (qp[static_cast<size_t>(r)] - qm[static_cast<size_t>(r)]) / (2 * dv);
    }
}
}  // namespace

bool Simulator::spiceModelApplies(const Component& c) const {
    switch (c.kind) {
        case ComponentKind::Diode:
        case ComponentKind::LED:
        case ComponentKind::NPN:
        case ComponentKind::NMOS:
        case ComponentKind::OpAmp:
        case ComponentKind::IC8: return true;
        case ComponentKind::Custom: {
            const CustomPart* part = CustomPartRegistry::instance().find(c.customPart);
            return part && !mcuModelForPart(part->spec.name);
        }
        default: return false;
    }
}

int Simulator::newInternalNode() {
    int i = unknowns_++;
    internalNodes_.push_back(i);
    return i;
}

bool Simulator::addSpiceModel(const Component& c, std::string& error) {
    auto fail = [&](const std::string& m) {
        error = c.ref + ": " + m;
        return false;
    };
    std::shared_ptr<const SpiceFlatCircuit> flat = cachedSpiceFlatten(c.spice.text, c.spice.model);
    if (!flat->ok) {
        std::string first = "it has no elements";
        for (const auto& d : flat->diagnostics)
            if (d.level == SpiceDiagnostic::Level::Error) {
                first = d.message;
                break;
            }
        return fail("SPICE model " + c.spice.model + " cannot be simulated: " + first);
    }
    // The part's pins as (number, name).
    std::vector<std::pair<std::string, std::string>> pins;
    if (c.kind == ComponentKind::Custom) {
        const CustomPart* part = CustomPartRegistry::instance().find(c.customPart);
        if (!part) return fail("unknown part.");
        for (const auto& p : part->spec.pins) pins.push_back({p.number, p.name});
    } else {
        for (const auto& p : c.def().pins) pins.push_back({std::string(), p.name});
    }
    const std::string mapText =
        c.spice.pins.empty() ? defaultSpicePinMap(*flat, pins, c.kind == ComponentKind::OpAmp) : c.spice.pins;
    std::vector<std::vector<std::string>> instances;
    {
        std::vector<std::string> cur;
        std::string tok;
        auto flushTok = [&] {
            if (!tok.empty()) cur.push_back(tok);
            tok.clear();
        };
        for (char ch : mapText + ";") {
            if (ch == ';') {
                flushTok();
                if (!cur.empty()) instances.push_back(cur);
                cur.clear();
            } else if (std::isspace(static_cast<unsigned char>(ch)) || ch == ',') {
                flushTok();
            } else {
                tok += ch;
            }
        }
    }
    std::string portList;
    for (const auto& p : flat->ports) portList += (portList.empty() ? "" : " ") + p;
    if (instances.empty())
        return fail("SPICE model " + c.spice.model + ": set the pin map (one entry per port: " + portList + ").");
    if (instances.size() > 16) return fail("the pin map has more than 16 instances.");

    // Pin-map entries → unknowns.
    std::vector<int> pinNode(pins.size(), -2);
    auto resolve = [&](const std::string& rawTok, int& out) -> bool {
        const std::string up = upperCase(rawTok);
        if (up == "0") {
            out = -1;
            return true;
        }
        if (up == "NC") {
            out = newInternalNode();
            return true;
        }
        if (up.rfind("DC:", 0) == 0) {
            double v = 0;
            if (!parseSpiceNumber(rawTok.substr(3), v)) return fail("pin map: invalid supply '" + rawTok + "'.");
            auto it = rails_.find(v);
            if (it != rails_.end()) {
                out = it->second;
                return true;
            }
            Element vs{};
            vs.type = ElemType::VSource;
            vs.componentId = c.id;
            vs.internal = true;
            out = newInternalNode();
            vs.n = {out, -1, -1};
            vs.source.dc = v;
            vs.branch = unknowns_++;
            elements_.push_back(vs);
            rails_[v] = out;
            return true;
        }
        if (up.rfind("NET:", 0) == 0) {
            const std::string want = upperCase(rawTok.substr(4));
            for (const auto& n : sch_.nets())
                if (upperCase(n.name) == want) {
                    out = netToNode_[static_cast<size_t>(n.index)];
                    return true;
                }
            return fail("pin map: no net named '" + rawTok.substr(4) + "'.");
        }
        int pin = -1;
        for (size_t i = 0; i < pins.size() && pin < 0; ++i)
            if (!pins[i].first.empty() && upperCase(pins[i].first) == up) pin = static_cast<int>(i);
        for (size_t i = 0; i < pins.size() && pin < 0; ++i)
            if (upperCase(pins[i].second) == up) pin = static_cast<int>(i);
        if (pin < 0) return fail("pin map: the part has no pin '" + rawTok + "'.");
        int& node = pinNode[static_cast<size_t>(pin)];
        if (node == -2) {
            const int net = sch_.netOf({c.id, pin});
            node = net < 0 ? newInternalNode() : netToNode_[static_cast<size_t>(net)];  // an open pin floats
        }
        out = node;
        return true;
    };
    std::vector<std::vector<int>> portNodes;
    for (size_t k = 0; k < instances.size(); ++k) {
        if (instances[k].size() != flat->ports.size())
            return fail("pin map entry " + std::to_string(k + 1) + " has " + std::to_string(instances[k].size()) +
                        " pins; model " + c.spice.model + " has " + std::to_string(flat->ports.size()) + " ports (" +
                        portList + ").");
        std::vector<int> nodes;
        for (const auto& tok : instances[k]) {
            int nd = -1;
            if (!resolve(tok, nd)) return false;
            nodes.push_back(nd);
        }
        portNodes.push_back(nodes);
    }

    const size_t first = elements_.size();
    for (const auto& ports : portNodes) {
        std::map<std::string, int> nodes;
        for (size_t i = 0; i < ports.size(); ++i) nodes["#" + std::to_string(i)] = ports[i];
        auto nodeOf = [&](const std::string& name) {
            if (name == "0") return -1;
            auto it = nodes.find(name);
            if (it != nodes.end()) return it->second;
            int nd = newInternalNode();
            nodes[name] = nd;
            return nd;
        };
        auto base = [&](ElemType t) {
            Element e{};
            e.type = t;
            e.componentId = c.id;
            e.internal = true;
            return e;
        };
        std::map<std::string, int> vbranch;
        std::map<std::string, size_t> inductor;
        for (const auto& p : flat->prims) {  // sources and inductors first: their branches are controls
            if (p.type == 'V') {
                Element e = base(ElemType::VSource);
                e.n = {nodeOf(p.nodes[0]), nodeOf(p.nodes[1]), -1};
                e.source.dc = p.value;
                e.branch = unknowns_++;
                vbranch[p.name] = e.branch;
                elements_.push_back(e);
            } else if (p.type == 'L') {
                Element e = base(ElemType::Inductor);
                e.n = {nodeOf(p.nodes[0]), nodeOf(p.nodes[1]), -1};
                e.value = p.value;
                e.branch = unknowns_++;
                inductor[p.name] = elements_.size();
                elements_.push_back(e);
            }
        }
        for (const auto& p : flat->prims) {
            switch (p.type) {
                case 'V':
                case 'L': break;
                case 'R':
                case 'C': {
                    Element e = base(p.type == 'R' ? ElemType::Resistor : ElemType::Capacitor);
                    e.n = {nodeOf(p.nodes[0]), nodeOf(p.nodes[1]), -1};
                    e.value = p.value;
                    elements_.push_back(e);
                    break;
                }
                case 'I': {  // SPICE: current flows from n+ through the source to n−
                    Element e = base(ElemType::ISource);
                    e.n = {nodeOf(p.nodes[1]), nodeOf(p.nodes[0]), -1};
                    e.source.dc = p.value;
                    elements_.push_back(e);
                    break;
                }
                case 'K': {
                    auto a = inductor.find(p.controlSources[0]), b = inductor.find(p.controlSources[1]);
                    if (a == inductor.end() || b == inductor.end()) return fail(p.name + ": coupled inductor missing.");
                    Element e = base(ElemType::Coupling);
                    e.coupled = {a->second, b->second};
                    e.value = p.value * std::sqrt(std::fabs(elements_[a->second].value * elements_[b->second].value));
                    elements_.push_back(e);
                    break;
                }
                case 'E':
                case 'F':
                case 'G':
                case 'H':
                case 'B': {
                    Element e = base(ElemType::Ctrl);
                    e.n = {nodeOf(p.nodes[0]), nodeOf(p.nodes[1]), -1};
                    e.voltageOut = p.voltageOutput;
                    auto ref = [&e](int unknown) -> int {
                        if (unknown < 0) return -1;
                        for (size_t i = 0; i < e.refs.size(); ++i)
                            if (e.refs[i] == unknown) return static_cast<int>(i);
                        e.refs.push_back(unknown);
                        return static_cast<int>(e.refs.size() - 1);
                    };
                    auto branchOf = [&](const std::string& name) {
                        auto it = vbranch.find(name);
                        return it == vbranch.end() ? -1 : it->second;
                    };
                    if (p.expr) {
                        e.ctrl = p.table.empty() ? CtrlKind::Expr : CtrlKind::Table;
                        e.table = p.table;
                        e.expr = withSlots(*p.expr, [&](const std::string& n) { return ref(nodeOf(n)); },
                                           [&](const std::string& n) { return ref(branchOf(n)); });
                    } else {
                        e.ctrl = CtrlKind::Poly;
                        for (const auto& [a, b] : p.controlNodes) e.pairs.push_back({ref(nodeOf(a)), ref(nodeOf(b))});
                        for (const auto& s : p.controlSources) {
                            int br = branchOf(s);
                            if (br < 0) return fail(p.name + ": controlling source " + s + " missing.");
                            e.pairs.push_back({ref(br), -1});
                        }
                        e.coeffs = p.coeffs;
                        e.terms = spicePolyTerms(static_cast<int>(e.pairs.size()), e.coeffs.size());
                    }
                    if (e.voltageOut) e.branch = unknowns_++;
                    elements_.push_back(e);
                    break;
                }
                case 'D':
                case 'Q':
                case 'M':
                case 'J': {
                    std::string err;
                    auto dev = spicedev::makeDevice(p, err);
                    if (!dev) return fail(err);
                    Element e = base(ElemType::Device);
                    e.dev = dev;
                    for (int t = 0; t < dev->terminals && static_cast<size_t>(t) < p.nodes.size(); ++t) {
                        const int ext = nodeOf(p.nodes[static_cast<size_t>(t)]);
                        const double rs = dev->seriesR[static_cast<size_t>(t)];
                        if (rs > 0) {
                            Element r = base(ElemType::Resistor);
                            const int in = newInternalNode();
                            r.n = {ext, in, -1};
                            r.value = rs;
                            elements_.push_back(r);
                            e.dn[static_cast<size_t>(t)] = in;
                        } else {
                            e.dn[static_cast<size_t>(t)] = ext;
                        }
                    }
                    elements_.push_back(e);
                    break;
                }
                default: return fail(p.name + ": element type not supported.");
            }
        }
    }
    Element ports{};
    ports.type = ElemType::ModelPorts;
    ports.componentId = c.id;
    ports.pinNodes = pinNode;
    ports.first = first;
    ports.last = elements_.size();
    ports.partKind = c.kind;
    // What the part's reading reports (as the built-in models do): the current into aux[0] (delivered out of it when
    // value < 0) and the voltage aux[1] − aux[2] (-1: 0 V). Built-in symbols by kind, catalog parts by pin name.
    auto pinNamed = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            for (size_t i = 0; i < pins.size(); ++i)
                if (upperCase(pins[i].second) == n) return static_cast<int>(i);
        return -1;
    };
    const int npins = static_cast<int>(pins.size());
    ports.value = 1;
    if (c.kind == ComponentKind::NPN || c.kind == ComponentKind::NMOS) {
        ports.aux = {1, 1, 2};  // B C E / G D S: collector (drain) current, V_CE (V_DS)
    } else if (c.kind == ComponentKind::OpAmp) {
        ports.aux = {2, 2, -1};
        ports.value = -1;
    } else if (pinNamed({"C"}) >= 0 && pinNamed({"E"}) >= 0) {
        ports.aux = {pinNamed({"C"}), pinNamed({"C"}), pinNamed({"E"})};
    } else if (pinNamed({"D"}) >= 0 && pinNamed({"S"}) >= 0) {
        ports.aux = {pinNamed({"D"}), pinNamed({"D"}), pinNamed({"S"})};
    } else if (pinNamed({"OUT", "VOUT", "OUTA", "1OUT"}) >= 0) {
        const int o = pinNamed({"OUT", "VOUT", "OUTA", "1OUT"});
        ports.aux = {o, o, -1};
        ports.value = -1;
    } else if (pinNamed({"A"}) >= 0 && pinNamed({"K"}) >= 0) {
        ports.aux = {pinNamed({"A"}), pinNamed({"A"}), pinNamed({"K"})};
    } else {
        ports.aux = {npins > 0 ? 0 : -1, npins > 0 ? 0 : -1, npins > 1 ? 1 : -1};
    }
    elements_.push_back(ports);
    return true;
}

bool simdetail::opAmpValueParam(const std::string& value, const char* key, double& out) {
    const std::string up = upperCase(value), k = std::string(key) + "=";
    for (size_t pos = up.find(k); pos != std::string::npos; pos = up.find(k, pos + 1)) {
        if (pos > 0 && !std::isspace(static_cast<unsigned char>(up[pos - 1])) && up[pos - 1] != ',' && up[pos - 1] != ';')
            continue;
        std::string tok = value.substr(pos + k.size());
        tok = tok.substr(0, tok.find_first_of(" \t,;"));
        std::string utok = upperCase(tok);
        double scale = 1;
        auto strip = [&](const char* suffix, double s) {
            const std::string sfx = suffix;
            if (utok.size() > sfx.size() && utok.compare(utok.size() - sfx.size(), sfx.size(), sfx) == 0) {
                tok = tok.substr(0, tok.size() - sfx.size());
                utok = utok.substr(0, utok.size() - sfx.size());
                scale = s;
                return true;
            }
            return false;
        };
        bool db = false;
        if (!strip("V/US", 1e6) && !strip("V/\xC2\xB5S", 1e6) && !strip("V/NS", 1e9) && !strip("V/MS", 1e3) && !strip("V/S", 1.0))
            db = strip("DB", 1.0);
        auto v = parseEngineeringValue(tok);
        if (!v || !std::isfinite(*v)) return false;
        out = db ? std::pow(10.0, *v / 20.0) : *v * scale;
        return true;
    }
    return false;
}

namespace {
bool opAmpParam(const std::string& value, const char* key, double& out) { return opAmpValueParam(value, key, out); }
}  // namespace

bool Simulator::opAmpMacromodelRequested(const std::string& value) {
    double v = 0;
    for (const char* k : {"SR", "P2", "VOH", "VOL", "ROUT", "AOL", "EN", "IN"})
        if (opAmpParam(value, k, v)) return true;
    return false;
}

bool Simulator::addOpAmpMacromodel(const Component& c, double gbw, std::string& error) {
    // Two-stage macromodel (all internal):
    //   input stage   i1 = Imax·tanh(gm·(V+ − V−) / Imax) into n1, with R1 = AOL / gm and C1 = gm / (2π·GBW) to
    //                 ground: DC gain AOL, dominant pole GBW / AOL, slew rate Imax / C1 = SR (gm = 1 S); n1 is
    //                 clamped 1 V beyond the output limits;
    //   second pole   unity-gain buffer n1 → n2 through R2 = 1 Ω, C2 = 1 / (2π·P2) (only with P2=);
    //   output        V = clamp(V(n2), VOL, VOH), smooth at the limits, behind ROUT.
    double aol = kOpAmpGain, sr = 0, p2 = 0, voh = 15, vol = -15, rout = 0, dummy = 0;
    opAmpParam(c.value, "AOL", aol);
    opAmpParam(c.value, "SR", sr);
    opAmpParam(c.value, "P2", p2);
    opAmpParam(c.value, "VOH", voh);
    opAmpParam(c.value, "VOL", vol);
    opAmpParam(c.value, "ROUT", rout);
    auto bad = [&](const char* what) {
        error = c.ref + ": invalid op-amp " + what + " in '" + c.value + "'";
        return false;
    };
    if (!(aol >= 1) || !std::isfinite(aol)) return bad("AOL (open-loop gain, at least 1)");
    if (!(gbw > 0)) return bad("GBW");
    if (sr < 0 || (opAmpParam(c.value, "SR", dummy) && !(sr > 0))) return bad("SR (slew rate)");
    if (p2 < 0 || (opAmpParam(c.value, "P2", dummy) && !(p2 > 0))) return bad("P2 (second pole)");
    if (!(voh > vol)) return bad("output limits (VOH must be above VOL)");
    if (rout < 0) return bad("ROUT");
    auto pinNode = [&](int pin) {
        const int net = sch_.netOf({c.id, pin});
        return net < 0 ? -1 : netToNode_[static_cast<size_t>(net)];
    };
    const int inp = pinNode(0), inm = pinNode(1), out = pinNode(2);
    const size_t first = elements_.size();
    auto base = [&](ElemType t) {
        Element e{};
        e.type = t;
        e.componentId = c.id;
        e.internal = true;
        return e;
    };
    auto ref = [](Element& e, int unknown) -> int {
        if (unknown < 0) return -1;
        e.refs.push_back(unknown);
        return static_cast<int>(e.refs.size() - 1);
    };
    const double gm = 1.0, c1 = gm / (2 * kPi * gbw);
    const int n1 = newInternalNode();
    {
        Element s = base(ElemType::Ctrl);
        s.ctrl = CtrlKind::TanhStage;
        s.n = {-1, n1, -1};  // from ground into n1
        int a = ref(s, inp), b = ref(s, inm);
        s.pairs = {{a, b}};
        // Without SR= the stage is linear to ±10 V of input (slew 2π·GBW·10 V, far above any real part).
        s.coeffs = {gm, sr > 0 ? sr * c1 : gm * 10.0};
        elements_.push_back(s);
        // The integrating node stays within 1 V beyond the output limits (as the rails bound a real second stage),
        // so it recovers from saturation at once and Newton never has to walk it to AOL·V.
        Element z = base(ElemType::Ctrl);
        z.ctrl = CtrlKind::DeadZone;
        z.n = {n1, -1, -1};
        int zn = ref(z, n1);
        z.pairs = {{zn, -1}};
        z.coeffs = {100.0, std::max(std::fabs(voh), std::fabs(vol)) + 1.0};
        elements_.push_back(z);
        Element r = base(ElemType::Resistor);
        r.n = {n1, -1, -1};
        r.value = aol / gm;
        elements_.push_back(r);
        Element cap = base(ElemType::Capacitor);
        cap.n = {n1, -1, -1};
        cap.value = c1;
        elements_.push_back(cap);
    }
    int stage = n1;
    if (p2 > 0) {
        const int n2 = newInternalNode();
        Element s = base(ElemType::Ctrl);
        s.ctrl = CtrlKind::Poly;
        s.n = {-1, n2, -1};
        int a = ref(s, n1);
        s.pairs = {{a, -1}};
        s.coeffs = {0.0, 1.0};
        s.terms = spicePolyTerms(1, 2);
        elements_.push_back(s);
        Element r = base(ElemType::Resistor);
        r.n = {n2, -1, -1};
        r.value = 1.0;
        elements_.push_back(r);
        Element cap = base(ElemType::Capacitor);
        cap.n = {n2, -1, -1};
        cap.value = 1.0 / (2 * kPi * p2);
        elements_.push_back(cap);
        stage = n2;
    }
    {
        const int driven = rout > 0 ? newInternalNode() : out;
        Element o = base(ElemType::Ctrl);
        o.ctrl = CtrlKind::Clamp;
        o.voltageOut = true;
        o.n = {driven, -1, -1};
        int a = ref(o, stage);
        o.pairs = {{a, -1}};
        o.coeffs = {vol, voh};
        o.branch = unknowns_++;
        elements_.push_back(o);
        if (rout > 0) {
            Element r = base(ElemType::Resistor);
            r.n = {driven, out, -1};
            r.value = rout;
            elements_.push_back(r);
        }
    }
    Element ports{};
    ports.type = ElemType::ModelPorts;
    ports.componentId = c.id;
    ports.pinNodes = {inp, inm, out};
    ports.first = first;
    ports.last = elements_.size();
    ports.partKind = ComponentKind::OpAmp;
    ports.aux = {2, 2, -1};
    ports.value = -1;
    elements_.push_back(ports);
    return true;
}

void Simulator::stampModelElement(const Element& e, double t, double h, const std::vector<double>& x) {
    const int n = unknowns_;
    auto addA = [&](int r, int c, double v) {
        if (r >= 0 && c >= 0) A_[static_cast<size_t>(r * n + c)] += v;
    };
    auto addB = [&](int r, double v) {
        if (r >= 0) b_[static_cast<size_t>(r)] += v;
    };
    switch (e.type) {
        case ElemType::Device: {
            const spicedev::Device& d = *e.dev;
            const int tc = d.terminals;
            std::array<double, 4> v0{}, i0{};
            for (int k = 0; k < tc; ++k) v0[static_cast<size_t>(k)] = nodeV(x, e.dn[static_cast<size_t>(k)]);
            if (h >= 0 && !noLimit_) d.limit(v0.data(), e.vjOld, e.vjValid);  // linearise at the limited junction voltages
            d.currents(v0.data(), i0.data());
            double J[4][4] = {};
            const double dv = 1e-6;
            for (int j = 0; j < tc; ++j) {
                std::array<double, 4> vp = v0, vm = v0, ip{}, im{};
                vp[static_cast<size_t>(j)] += dv;
                vm[static_cast<size_t>(j)] -= dv;
                d.currents(vp.data(), ip.data());
                d.currents(vm.data(), im.data());
                for (int r = 0; r < tc; ++r) J[r][j] = (ip[static_cast<size_t>(r)] - im[static_cast<size_t>(r)]) / (2 * dv);
            }
            std::array<double, 4> rhs{};
            for (int r = 0; r < tc; ++r) rhs[static_cast<size_t>(r)] = -i0[static_cast<size_t>(r)];
            if (h > 0 && d.hasCharge()) {
                // Companion model of the stored charge: i = (q − q_prev)/h (backward Euler) or 2(q − q_prev)/h − i_prev.
                std::array<double, 4> q0{};
                d.charges(v0.data(), q0.data());
                double C[16] = {};
                chargeJacobian(d, v0, C);
                const double f = trap_ ? 2.0 / h : 1.0 / h;
                for (int r = 0; r < tc; ++r) {
                    const size_t rr = static_cast<size_t>(r);
                    double iq = f * (q0[rr] - e.qPrev[rr]) - (trap_ ? e.iqPrev[rr] : 0.0);
                    rhs[rr] -= iq;
                    for (int j = 0; j < tc; ++j) J[r][j] += f * C[r * tc + j];
                }
            }
            for (int r = 0; r < tc; ++r) {
                double b = rhs[static_cast<size_t>(r)];
                for (int j = 0; j < tc; ++j) {
                    addA(e.dn[static_cast<size_t>(r)], e.dn[static_cast<size_t>(j)], J[r][j]);
                    b += J[r][j] * v0[static_cast<size_t>(j)];
                }
                addB(e.dn[static_cast<size_t>(r)], b);
            }
            // GMIN across every terminal pair keeps junctions and channels conditioned.
            for (int a = 0; a < tc; ++a)
                for (int b = a + 1; b < tc; ++b) {
                    const int na = e.dn[static_cast<size_t>(a)], nb = e.dn[static_cast<size_t>(b)];
                    addA(na, na, kGmin);
                    addA(nb, nb, kGmin);
                    addA(na, nb, -kGmin);
                    addA(nb, na, -kGmin);
                }
            break;
        }
        case ElemType::Ctrl: {
            std::vector<double> vals(e.refs.size()), grad;
            for (size_t r = 0; r < vals.size(); ++r) vals[r] = nodeV(x, e.refs[r]);
            const double f0 = ctrlEval(e, vals, t, grad);
            double lin = f0;
            for (size_t r = 0; r < vals.size(); ++r) lin -= grad[r] * vals[r];
            const int p = e.n[0], m = e.n[1];
            if (e.voltageOut) {
                const int k = e.branch;
                addA(p, k, 1);
                addA(m, k, -1);
                addA(k, p, 1);
                addA(k, m, -1);
                for (size_t r = 0; r < vals.size(); ++r) addA(k, e.refs[r], -grad[r]);
                addB(k, lin);
            } else {
                for (size_t r = 0; r < vals.size(); ++r) {
                    addA(p, e.refs[r], grad[r]);
                    addA(m, e.refs[r], -grad[r]);
                }
                addB(p, -lin);
                addB(m, lin);
            }
            break;
        }
        case ElemType::Coupling: {
            if (!(h > 0)) break;
            const Element& l1 = elements_[e.coupled[0]];
            const Element& l2 = elements_[e.coupled[1]];
            const double f = (trap_ ? 2.0 : 1.0) * e.value / h;
            addA(l1.branch, l2.branch, -f);
            addB(l1.branch, -f * l2.prevI);
            addA(l2.branch, l1.branch, -f);
            addB(l2.branch, -f * l1.prevI);
            break;
        }
        default: break;
    }
}

void Simulator::addModelAdmittance(const Element& e, double w, const std::vector<double>& x,
                                   std::vector<std::complex<double>>& M) const {
    const int n = unknowns_;
    auto add = [&](int r, int c, std::complex<double> v) {
        if (r >= 0 && c >= 0) M[static_cast<size_t>(r * n + c)] += v;
    };
    if (e.type == ElemType::Device) {
        const spicedev::Device& d = *e.dev;
        if (!d.hasCharge()) return;
        const int tc = d.terminals;
        std::array<double, 4> v0{};
        for (int k = 0; k < tc; ++k) v0[static_cast<size_t>(k)] = nodeV(x, e.dn[static_cast<size_t>(k)]);
        double C[16] = {};
        chargeJacobian(d, v0, C);
        for (int r = 0; r < tc; ++r)
            for (int j = 0; j < tc; ++j)
                if (C[r * tc + j] != 0.0)
                    add(e.dn[static_cast<size_t>(r)], e.dn[static_cast<size_t>(j)], std::complex<double>(0.0, w * C[r * tc + j]));
    } else if (e.type == ElemType::Coupling) {
        const int k1 = elements_[e.coupled[0]].branch, k2 = elements_[e.coupled[1]].branch;
        add(k1, k2, std::complex<double>(0.0, -w * e.value));
        add(k2, k1, std::complex<double>(0.0, -w * e.value));
    }
}

void Simulator::updateCharges(Element& e) const {
    if (!e.dev || !e.dev->hasCharge()) return;
    std::array<double, 4> v{}, q{};
    for (int k = 0; k < e.dev->terminals; ++k) v[static_cast<size_t>(k)] = nodeV(x_, e.dn[static_cast<size_t>(k)]);
    e.dev->charges(v.data(), q.data());
    for (size_t k = 0; k < 4; ++k) {
        double iq = 0;
        if (stepH_ > 0) iq = trap_ ? 2.0 / stepH_ * (q[k] - e.qPrev[k]) - e.iqPrev[k] : (q[k] - e.qPrev[k]) / stepH_;
        e.qPrev[k] = q[k];
        e.iqPrev[k] = iq;
    }
}

void Simulator::terminalCurrents(const Element& e, const std::vector<double>& x, double h,
                                 std::vector<std::pair<int, double>>& out) const {
    const int a = e.n[0], b = e.n[1];
    const double v = nodeV(x, a) - nodeV(x, b);
    auto two = [&](double i) {
        out.push_back({a, i});
        out.push_back({b, -i});
    };
    switch (e.type) {
        case ElemType::Resistor: two(v / e.value); break;
        case ElemType::Capacitor:
            if (h > 0) two(trap_ ? 2.0 * e.value / h * (v - e.prevV) - e.prevIc : e.value / h * (v - e.prevV));
            break;
        case ElemType::Inductor:
        case ElemType::VSource: two(x[static_cast<size_t>(e.branch)]); break;
        case ElemType::ISource: two(-e.source.valueAt(0)); break;
        case ElemType::Ctrl:
            if (e.voltageOut) {
                two(x[static_cast<size_t>(e.branch)]);
            } else {
                std::vector<double> vals(e.refs.size()), grad;
                for (size_t r = 0; r < vals.size(); ++r) vals[r] = nodeV(x, e.refs[r]);
                two(ctrlEval(e, vals, t_, grad));
            }
            break;
        case ElemType::Device: {
            const spicedev::Device& d = *e.dev;
            std::array<double, 4> vv{}, ii{};
            for (int k = 0; k < d.terminals; ++k) vv[static_cast<size_t>(k)] = nodeV(x, e.dn[static_cast<size_t>(k)]);
            d.currents(vv.data(), ii.data());
            if (h > 0 && d.hasCharge()) {
                std::array<double, 4> q{};
                d.charges(vv.data(), q.data());
                for (size_t k = 0; k < 4; ++k)
                    ii[k] += trap_ ? 2.0 / h * (q[k] - e.qPrev[k]) - e.iqPrev[k] : (q[k] - e.qPrev[k]) / h;
            }
            for (int k = 0; k < d.terminals; ++k) out.push_back({e.dn[static_cast<size_t>(k)], ii[static_cast<size_t>(k)]});
            break;
        }
        default: break;
    }
}

DeviceReading Simulator::modelReading(const Element& e, const std::vector<double>& x, double h) const {
    std::map<int, double> into;  // node → current flowing into the model's elements
    std::vector<std::pair<int, double>> cur;
    for (size_t i = e.first; i < e.last; ++i) {
        cur.clear();
        terminalCurrents(elements_[i], x, h, cur);
        for (const auto& [node, current] : cur) into[node] += current;
    }
    DeviceReading r;
    r.componentId = e.componentId;
    std::vector<double> pinI(e.pinNodes.size(), 0.0), pinV(e.pinNodes.size(), 0.0);
    std::map<int, bool> taken;
    for (size_t p = 0; p < e.pinNodes.size(); ++p) {
        const int nd = e.pinNodes[p];
        if (nd == -2) continue;
        pinV[p] = nodeV(x, nd);
        if (!taken[nd]) {
            pinI[p] = into[nd];
            taken[nd] = true;
        }
    }
    double power = 0;
    for (const auto& [node, current] : into) power += nodeV(x, node) * current;
    r.power = power;
    auto pin = [&](int i) { return i >= 0 && static_cast<size_t>(i) < pinI.size() ? pinI[static_cast<size_t>(i)] : 0.0; };
    auto volt = [&](int i) { return i >= 0 && static_cast<size_t>(i) < pinV.size() ? pinV[static_cast<size_t>(i)] : 0.0; };
    r.current = e.value * pin(e.aux[0]);
    r.voltage = volt(e.aux[1]) - volt(e.aux[2]);
    return r;
}

}  // namespace sieda
