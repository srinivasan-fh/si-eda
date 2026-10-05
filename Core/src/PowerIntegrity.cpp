#include "sieda/PowerIntegrity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/Project.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {
constexpr double kEps0 = 8.8541878128e-12;
constexpr double kMu0 = 1.25663706212e-6;
constexpr double kLight = 299792458.0;
constexpr double kPlating = 0.025;  // via barrel plating, mm (IPC-6012 class 2/3 average ≥ 20–25 µm)

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string eng(double v, const char* unit) { return formatEngineeringValue(v, unit, 3); }
}  // namespace

double targetImpedance(double volts, double ripplePercent, double transientAmps) {
    if (!(transientAmps > 0)) return std::numeric_limits<double>::infinity();
    return std::fabs(volts) * ripplePercent / 100.0 / transientAmps;
}

std::complex<double> capacitorImpedance(double f, double c, double esr, double esl) {
    const double w = 2 * kPi * f;
    return {esr, w * esl - (c > 0 ? 1 / (w * c) : 0)};
}

double selfResonance(double c, double l) { return c > 0 && l > 0 ? 1 / (2 * kPi * std::sqrt(l * c)) : 0; }

double planeCapacitance(double areaMm2, double gapMm, double er) {
    return gapMm > 0 ? kEps0 * er * areaMm2 * 1e-6 / (gapMm * 1e-3) : 0;
}

double sheetResistance(double thicknessMm) { return thicknessMm > 0 ? kCopperResistivity / (thicknessMm * 1e-3) : 0; }

CapacitorParasitics capacitorParasitics(const std::string& footprint, double farads) {
    // Typical manufacturer data (MLCC X7R / X5R, MnO2 tantalum, aluminium electrolytic): ESL by case size, ESR by
    // value (larger MLCCs have lower ESR at their resonance).
    CapacitorParasitics p;
    const std::string f = upper(footprint);
    const double c = std::max(1e-15, farads);
    auto mlccEsr = [&] { return std::clamp(0.03 * std::pow(c / 100e-9, -0.3), 0.003, 0.5); };
    if (f.find("TANT_A") != std::string::npos) {
        p.esl = 1.2e-9;
        p.esr = 1.0;
    } else if (f.find("TANT") != std::string::npos) {
        p.esl = 1.5e-9;
        p.esr = 0.6;
    } else if (f.find("RADIAL") != std::string::npos || f.rfind("CP_", 0) == 0) {
        p.esl = 5e-9;
        p.esr = std::clamp(0.3 * std::pow(c / 100e-6, -0.5), 0.02, 3.0);
    } else if (f.find("DISC") != std::string::npos || f.find("THT") != std::string::npos) {
        p.esl = 3e-9;
        p.esr = 0.1;
    } else {
        p.esr = mlccEsr();
        p.esl = f.find("0201") != std::string::npos   ? 0.3e-9
                : f.find("0402") != std::string::npos ? 0.4e-9
                : f.find("0603") != std::string::npos ? 0.55e-9
                : f.find("0805") != std::string::npos ? 0.7e-9
                : f.find("1206") != std::string::npos ? 0.9e-9
                : f.find("1210") != std::string::npos ? 1.0e-9
                                                      : 0.7e-9;
    }
    return p;
}

namespace {

/// Inductance per mm (H/mm) of a track over its reference: Z0 · t_pd.
double inductancePerMm(const BoardSettings& s, int layer, double w) {
    return std::max(5.0, trackImpedance(s, layer, w)) * propagationDelayPerMm(s, layer, w);
}

/// Sparse symmetric conductance network solved by Jacobi-preconditioned conjugate gradients.
struct Network {
    std::vector<std::vector<std::pair<int, double>>> adj;
    explicit Network(size_t n) : adj(n) {}
    void add(int a, int b, double g) {
        if (a < 0 || b < 0 || a == b || !(g > 0)) return;
        adj[static_cast<size_t>(a)].push_back({b, g});
        adj[static_cast<size_t>(b)].push_back({a, g});
    }
    /// Drops v (V below the sources) for sink currents i; nodes not connected to a source keep NaN.
    std::vector<double> solve(const std::vector<bool>& source, const std::vector<double>& sink) const {
        const size_t n = adj.size();
        std::vector<double> v(n, std::numeric_limits<double>::quiet_NaN());
        // Nodes connected to a source.
        std::vector<int> unknown(n, -1);
        std::vector<int> order;
        std::vector<bool> seen(n, false);
        std::vector<int> stack;
        for (size_t k = 0; k < n; ++k)
            if (source[k]) {
                seen[k] = true;
                v[k] = 0;
                stack.push_back(static_cast<int>(k));
            }
        while (!stack.empty()) {
            int x = stack.back();
            stack.pop_back();
            for (const auto& e : adj[static_cast<size_t>(x)])
                if (!seen[static_cast<size_t>(e.first)]) {
                    seen[static_cast<size_t>(e.first)] = true;
                    stack.push_back(e.first);
                    unknown[static_cast<size_t>(e.first)] = static_cast<int>(order.size());
                    order.push_back(e.first);
                }
        }
        const size_t m = order.size();
        if (m == 0) return v;
        std::vector<double> diag(m, 0), b(m, 0), x(m, 0), r(m), z(m), p(m), q(m);
        for (size_t k = 0; k < m; ++k) {
            const int node = order[k];
            for (const auto& e : adj[static_cast<size_t>(node)]) diag[k] += e.second;
            b[k] = sink[static_cast<size_t>(node)];
        }
        auto apply = [&](const std::vector<double>& in, std::vector<double>& out) {
            for (size_t k = 0; k < m; ++k) {
                const int node = order[k];
                double s = diag[k] * in[k];
                for (const auto& e : adj[static_cast<size_t>(node)]) {
                    const int j = unknown[static_cast<size_t>(e.first)];
                    if (j >= 0) s -= e.second * in[static_cast<size_t>(j)];
                }
                out[k] = s;
            }
        };
        r = b;
        double bnorm = 0;
        for (double y : b) bnorm += y * y;
        if (bnorm <= 0) {
            for (size_t k = 0; k < m; ++k) v[static_cast<size_t>(order[k])] = 0;
            return v;
        }
        for (size_t k = 0; k < m; ++k) z[k] = r[k] / std::max(1e-30, diag[k]);
        p = z;
        double rz = 0;
        for (size_t k = 0; k < m; ++k) rz += r[k] * z[k];
        for (int it = 0; it < 20000; ++it) {
            apply(p, q);
            double pq = 0;
            for (size_t k = 0; k < m; ++k) pq += p[k] * q[k];
            if (!(pq > 0)) break;
            const double alpha = rz / pq;
            double rn = 0;
            for (size_t k = 0; k < m; ++k) {
                x[k] += alpha * p[k];
                r[k] -= alpha * q[k];
                rn += r[k] * r[k];
            }
            if (rn <= 1e-24 * bnorm) break;
            double rz2 = 0;
            for (size_t k = 0; k < m; ++k) {
                z[k] = r[k] / std::max(1e-30, diag[k]);
                rz2 += r[k] * z[k];
            }
            const double beta = rz2 / rz;
            rz = rz2;
            for (size_t k = 0; k < m; ++k) p[k] = z[k] + beta * p[k];
        }
        for (size_t k = 0; k < m; ++k) v[static_cast<size_t>(order[k])] = x[k];
        return v;
    }
};

std::string pinName(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    if (pin < 0 || pin >= static_cast<int>(pins.size())) return std::to_string(pin + 1);
    const auto& p = pins[static_cast<size_t>(pin)];
    return p.name.empty() ? p.number : p.name;
}

bool isLoadPin(const Component& c, int pin) {
    if (c.kind == ComponentKind::Custom) {
        const auto& pins = c.def().pins;
        return pin >= 0 && pin < static_cast<int>(pins.size()) && static_cast<PinType>(pins[static_cast<size_t>(pin)].type) == PinType::PowerIn;
    }
    return false;
}

}  // namespace

std::vector<PdnRailResult> analyzePdn(const Project& project) {
    std::vector<PdnRailResult> out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const auto& nets = sch.nets();
    const auto pads = pcb.pads(sch);
    static const std::vector<ZoneFill> kNoFills;
    const std::vector<ZoneFill>& fills = pcb.zones.empty() ? kNoFills : pcb.zoneFills(sch);
    const double er = boardLaminate(s).er;
    const double cu = copperThickness(s);
    auto isGround = [&](int n) { return n >= 0 && n < static_cast<int>(nets.size()) && sch.netRole(n) == NetRole::Ground; };

    // Direct DC load per rail and component: behavioural load models, else 10 mA per IC (estimated).
    std::map<int, std::map<int, double>> direct;  // net → component → A
    std::map<int, bool> estimated;
    for (const auto& c : sch.components()) {
        const auto& pins = c.def().pins;
        std::set<int> railsOfPart;
        for (size_t k = 0; k < pins.size(); ++k)
            if (isLoadPin(c, static_cast<int>(k))) {
                int n = sch.netOf({c.id, static_cast<int>(k)});
                if (n >= 0 && !isGround(n)) railsOfPart.insert(n);
            }
        if (railsOfPart.empty()) continue;
        const CustomPart* part = c.kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
        bool modelled = false;
        if (part)
            for (const auto& l : part->spec.model.loads) {
                int idx = part->spec.pinIndex(l.supply);
                int n = idx >= 0 ? sch.netOf({c.id, idx}) : -1;
                if (n >= 0 && railsOfPart.count(n)) {
                    direct[n][c.id] += l.current;
                    modelled = true;
                }
            }
        // Regulators' own input current is added below from their output rail.
        if (!modelled && !(part && part->spec.model.hasRegulator))
            for (int n : railsOfPart) {
                direct[n][c.id] += 0.010;
                estimated[n] = true;
            }
    }
    // A regulator's input carries its output rail's load (switching: P_out / (η V_in)).
    std::map<int, double> railTotal;
    for (const auto& [n, comps] : direct)
        for (const auto& [id, a] : comps) railTotal[n] += a;
    std::map<int, std::map<int, double>> withRegs = direct;
    for (const auto& c : sch.components()) {
        const CustomPart* part = c.kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
        if (!part || !part->spec.model.hasRegulator) continue;
        const auto& rm = part->spec.model.regulator;
        const int in = part->spec.pinIndex(rm.in), o = part->spec.pinIndex(rm.out);
        const int nin = in >= 0 ? sch.netOf({c.id, in}) : -1, nout = o >= 0 ? sch.netOf({c.id, o}) : -1;
        if (nin < 0 || nout < 0 || nin == nout) continue;
        double iout = railTotal.count(nout) ? railTotal[nout] : 0;
        double vin = railVoltageFromName(nets[static_cast<size_t>(nin)].name), vout = rm.vout;
        double iin = rm.isolated() && vin > 0 && vout > 0 ? iout * vout / (std::max(0.3, rm.efficiency) * vin) : iout;
        withRegs[nin][c.id] += iin + rm.iq;
    }

    for (const auto& net : nets) {
        const int n = net.index;
        const NetRole role = sch.netRole(n);
        if (role != NetRole::Power && role != NetRole::NegativeSupply) continue;
        std::vector<size_t> railPads;
        for (size_t k = 0; k < pads.size(); ++k)
            if (pads[k].net == n) railPads.push_back(k);
        if (railPads.size() < 2) continue;
        PdnRailResult r;
        r.net = n;
        r.name = net.name;
        const PdnRailSettings* rs = project.si.rail(net.name);

        // Voltage: from the name, a regulator's output, a source's value.
        double v = railVoltageFromName(net.name);
        std::vector<size_t> sourcePads;
        for (const auto& c : sch.components()) {
            const CustomPart* part = c.kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
            if (part && part->spec.model.hasRegulator) {
                const auto& rm = part->spec.model.regulator;
                const int o = part->spec.pinIndex(rm.out);
                if (o >= 0 && sch.netOf({c.id, o}) == n) {
                    if (v <= 0) v = rm.vout;
                    if (r.vrmRef.empty()) {
                        r.vrmRef = c.ref;
                        r.vrmKind = rm.isolated() ? "switching regulator" : rm.loadSwitch ? "load switch" : "LDO";
                    }
                    for (size_t k : railPads)
                        if (pads[k].componentId == c.id && pads[k].pinIndex == o) sourcePads.push_back(k);
                }
            }
            if (isVoltageSourceKind(c.kind) && sch.netOf({c.id, 0}) == n) {
                if (v <= 0) {
                    auto spec = SourceSpec::parse(c.value);
                    if (spec && spec->kind == SourceSpec::Kind::DC) v = std::fabs(spec->dc);
                }
                if (r.vrmRef.empty()) {
                    r.vrmRef = c.ref;
                    r.vrmKind = "supply";
                }
                for (size_t k : railPads)
                    if (pads[k].componentId == c.id) sourcePads.push_back(k);
            }
        }
        if (sourcePads.empty())
            for (size_t k : railPads) {
                const Component* c = sch.find(pads[k].componentId);
                if (c && c->kind == ComponentKind::Connector) {
                    sourcePads.push_back(k);
                    if (r.vrmRef.empty()) {
                        r.vrmRef = c->ref;
                        r.vrmKind = "connector";
                    }
                }
            }
        if (v <= 0) {
            v = 3.3;
            r.voltageEstimated = true;
        }
        r.voltage = v;
        r.ripplePercent = rs && rs->ripplePercent > 0 ? rs->ripplePercent : (v <= 1.5 ? 3.0 : 5.0);
        // VRM: output resistance R with the inductance that puts its loop bandwidth at f_bw (ωL = R).
        double fbw = 100e3;
        if (r.vrmKind == "switching regulator") {
            r.vrmR = 0.005;
            fbw = 50e3;
        } else if (r.vrmKind == "LDO" || r.vrmKind == "load switch") {
            r.vrmR = 0.02;
            fbw = 100e3;
        } else if (r.vrmKind == "supply") {
            r.vrmR = 0.01;
            fbw = 10e3;
        } else if (r.vrmKind == "connector") {
            r.vrmR = 0.02;
            fbw = 10e3;
        } else {
            r.vrmKind = "unknown source";
            r.vrmR = 0.05;
            fbw = 10e3;
        }
        r.vrmL = r.vrmR / (2 * kPi * fbw);

        // Loads.
        double total = 0;
        auto it = withRegs.find(n);
        if (it != withRegs.end())
            for (const auto& [id, amps] : it->second) {
                const Component* c = sch.find(id);
                if (!c) continue;
                std::vector<size_t> mine;
                for (size_t k : railPads)
                    if (pads[k].componentId == id && isLoadPin(*c, pads[k].pinIndex)) mine.push_back(k);
                if (mine.empty())
                    for (size_t k : railPads)
                        if (pads[k].componentId == id) mine.push_back(k);
                for (size_t k : mine) {
                    PdnLoad l;
                    l.componentId = id;
                    l.ref = c->ref;
                    l.pin = pinName(*c, pads[k].pinIndex);
                    l.current = amps / static_cast<double>(mine.size());
                    r.loads.push_back(l);
                }
                total += amps;
            }
        r.currentEstimated = estimated.count(n) > 0;
        if (rs && rs->dcCurrent > 0 && total > 0) {
            for (auto& l : r.loads) l.current *= rs->dcCurrent / total;
            total = rs->dcCurrent;
            r.currentEstimated = false;
        } else if (rs && rs->dcCurrent > 0) {
            total = rs->dcCurrent;
        }
        r.dcCurrent = total;
        r.transientCurrent = rs && rs->transientCurrent > 0 ? rs->transientCurrent : 0.5 * total;
        r.target = targetImpedance(v, r.ripplePercent, r.transientCurrent);

        // Rail and ground pours.
        std::vector<size_t> railFills, gndFills;
        for (size_t k = 0; k < fills.size(); ++k) {
            if (fills[k].net == n) railFills.push_back(k);
            else if (isGround(fills[k].net)) gndFills.push_back(k);
        }
        // Load pad positions (for distances) and ground pads of loads.
        std::vector<Vec2> loadPos;
        std::set<int> loadComps;
        for (const auto& l : r.loads) loadComps.insert(l.componentId);
        for (size_t k : railPads)
            if (loadComps.count(pads[k].componentId)) loadPos.push_back(pads[k].position);
        std::vector<Vec2> loadGndPos;
        for (const auto& p : pads)
            if (loadComps.count(p.componentId) && isGround(p.net)) loadGndPos.push_back(p.position);
        auto nearest = [](const std::vector<Vec2>& pts, Vec2 p) {
            double d = std::numeric_limits<double>::infinity();
            for (const Vec2& q : pts) d = std::min(d, (q - p).length());
            return d;
        };
        auto inFill = [&](const std::vector<size_t>& list, int layer, Vec2 p) {
            for (size_t k : list)
                if (fills[k].layer == layer && fills[k].islandNear(p, 0.05) >= 0) return true;
            return false;
        };
        auto planeLayerOf = [&](const std::vector<size_t>& list, int from) {
            int best = -1;
            for (size_t k : list)
                if (best < 0 || std::abs(fills[k].layer - from) < std::abs(best - from)) best = fills[k].layer;
            return best;
        };
        // One side of a capacitor's mounting loop: pad → (track) → via → plane, or pad → track → load pin.
        auto sideInductance = [&](const Pad& pad, int sideNet, const std::vector<size_t>& sideFills, const std::vector<Vec2>& loadsOfSide) {
            const int layer = pad.throughHole ? 0 : pad.smdLayer;
            const double lper = inductancePerMm(s, layer, s.trackWidth);
            if (inFill(sideFills, layer, pad.position)) return 0.05e-9;
            const int plane = planeLayerOf(sideFills, layer);
            if (plane >= 0) {
                double dist = std::numeric_limits<double>::infinity();
                for (const auto& via : pcb.vias)
                    if (via.net == sideNet) dist = std::min(dist, (via.position - pad.position).length());
                for (const auto& p : pads)
                    if (p.net == sideNet && p.throughHole) dist = std::min(dist, (p.position - pad.position).length());
                if (!std::isfinite(dist)) dist = 10;
                const double depth = std::fabs(layerDepth(s, plane) - layerDepth(s, layer));
                return dist * lper + viaInductance(std::max(0.05, depth), s.viaDrill);
            }
            double dist = nearest(loadsOfSide, pad.position);
            if (!std::isfinite(dist)) dist = 10;
            return dist * lper;
        };

        // Decoupling capacitors: rail to ground.
        for (const auto& c : sch.components()) {
            if (c.kind != ComponentKind::Capacitor || !c.pcb.placed) continue;
            const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
            if (!((n0 == n && isGround(n1)) || (n1 == n && isGround(n0)))) continue;
            auto value = parseEngineeringValue(primaryValue(c.value));
            if (!value || *value <= 0) continue;
            PdnDecap d;
            d.componentId = c.id;
            d.ref = c.ref;
            d.value = c.value;
            d.footprint = c.footprintName();
            d.c = *value;
            const CapacitorParasitics par = capacitorParasitics(d.footprint, d.c);
            d.esr = par.esr;
            d.esl = par.esl;
            const Pad* railPad = nullptr;
            const Pad* gndPad = nullptr;
            for (const auto& p : pads)
                if (p.componentId == c.id) (p.net == n ? railPad : gndPad) = &p;
            d.mounting = 0.1e-9;  // pads and escape
            if (railPad) d.mounting += sideInductance(*railPad, n, railFills, loadPos);
            if (gndPad) d.mounting += sideInductance(*gndPad, gndPad->net, gndFills, loadGndPos);
            d.srf = selfResonance(d.c, d.esl + d.mounting);
            d.distance = railPad ? nearest(loadPos, railPad->position) : 0;
            if (!std::isfinite(d.distance)) d.distance = 0;
            r.decaps.push_back(d);
        }
        std::sort(r.decaps.begin(), r.decaps.end(), [](const PdnDecap& a, const PdnDecap& b) { return a.c > b.c; });

        // Plane pair: rail pour over a ground pour on another layer.
        for (size_t rk : railFills) {
            const ZoneFill& rf = fills[rk];
            for (size_t gk : gndFills) {
                const ZoneFill& gf = fills[gk];
                if (gf.layer == rf.layer) continue;
                int overlap = 0;
                for (int j = 0; j < rf.rows; ++j)
                    for (int i = 0; i < rf.cols; ++i) {
                        if (rf.island[static_cast<size_t>(j * rf.cols + i)] < 0) continue;
                        if (gf.islandAt({(i + 0.5) * rf.cell, (j + 0.5) * rf.cell}) >= 0) ++overlap;
                    }
                const double area = overlap * rf.cell * rf.cell;
                const double gap = std::max(0.05, std::fabs(layerDepth(s, gf.layer) - layerDepth(s, rf.layer)) - cu);
                const double c = planeCapacitance(area, gap, er);
                if (c > r.planeC) {
                    r.planeC = c;
                    r.planeArea = area;
                    r.planeGap = gap;
                    r.planeLayers = copperLayerName(rf.layer, s.layerCount) + " / " + copperLayerName(gf.layer, s.layerCount);
                    // Spreading inductance ≈ μ0·d per square (half a square for a central feed).
                    r.planeL = 0.5 * kMu0 * gap * 1e-3;
                    double x0 = 1e9, x1 = -1e9, y0 = 1e9, y1 = -1e9;
                    for (const auto& rc : rf.rects) {
                        x0 = std::min(x0, rc.x0);
                        x1 = std::max(x1, rc.x1);
                        y0 = std::min(y0, rc.y0);
                        y1 = std::max(y1, rc.y1);
                    }
                    const double span = std::max(x1 - x0, y1 - y0);
                    r.cavityResonance = span > 0 ? kLight / (2 * span * 1e-3 * std::sqrt(er)) : 0;
                }
            }
        }

        // Impedance profile, 1 kHz – 1 GHz, 40 points per decade.
        for (int k = 0; k <= 240; ++k) {
            const double f = std::pow(10.0, 3.0 + k / 40.0);
            std::complex<double> y = 1.0 / std::complex<double>(r.vrmR, 2 * kPi * f * r.vrmL);
            for (const auto& d : r.decaps) y += 1.0 / capacitorImpedance(f, d.c, d.esr, d.esl + d.mounting);
            if (r.planeC > 0) y += 1.0 / capacitorImpedance(f, r.planeC, 1e-3, r.planeL);
            r.freq.push_back(f);
            r.z.push_back(std::abs(1.0 / y));
        }
        for (size_t k = 1; k + 1 < r.z.size(); ++k) {
            if (!(r.z[k] > r.z[k - 1] && r.z[k] >= r.z[k + 1])) continue;
            double lo = r.z[k];
            for (size_t j = (k > 12 ? k - 12 : 0); j < std::min(r.z.size(), k + 13); ++j) lo = std::min(lo, r.z[j]);
            if (r.z[k] > 1.3 * lo) r.peaks.push_back({r.freq[k], r.z[k]});
        }
        double worstRatio = 0;
        for (size_t k = 0; k < r.z.size(); ++k) {
            if (r.freq[k] > r.fMax) break;
            const double ratio = std::isfinite(r.target) ? r.z[k] / r.target : 0;
            if (ratio > worstRatio) {
                worstRatio = ratio;
                r.worstZ = r.z[k];
                r.worstF = r.freq[k];
            }
        }
        r.compliant = !(worstRatio > 1.0);

        // Recommendations.
        if (!r.compliant) {
            double typicalMount = 0.6e-9;
            if (!r.decaps.empty()) {
                typicalMount = 0;
                for (const auto& d : r.decaps) typicalMount += d.esl + d.mounting;
                typicalMount /= static_cast<double>(r.decaps.size());
            }
            const PdnRailResult::Peak* peak = nullptr;
            for (const auto& pk : r.peaks)
                if (pk.f <= r.fMax && pk.z > r.target && (!peak || pk.z > peak->z)) peak = &pk;
            if (r.worstF < 1e6) {
                const double c = 1 / (2 * kPi * r.worstF * r.target);
                r.recommendations.push_back("Add bulk capacitance near " + (r.vrmRef.empty() ? std::string("the supply entry") : r.vrmRef) +
                                            ": at least " + eng(nearestStandardValue(c, ESeries::E12), "F") + " (|Z| of 1/(2πfC) at " +
                                            eng(r.worstF, "Hz") + " below the target).");
            }
            if (peak) {
                const double c = 1 / ((2 * kPi * peak->f) * (2 * kPi * peak->f) * typicalMount);
                r.recommendations.push_back("Anti-resonance of " + eng(peak->z, "Ω") + " at " + eng(peak->f, "Hz") +
                                            ": add a " + eng(nearestStandardValue(c, ESeries::E12), "F") +
                                            " capacitor (self-resonance near that frequency) beside the loads.");
            }
            if (r.worstF >= 1e6 && r.target > 0) {
                const double allowed = r.target / (2 * kPi * r.worstF);
                const int needed = static_cast<int>(std::ceil(typicalMount / std::max(1e-15, allowed)));
                const int more = std::max(1, needed - static_cast<int>(r.decaps.size()));
                r.recommendations.push_back("Above " + eng(r.worstF, "Hz") + " the PDN is inductive: total inductance must be ≤ " +
                                            eng(allowed, "H") + ". Add about " + std::to_string(more) +
                                            " more 100 nF 0402 capacitors, put vias right at their pads and keep them on the "
                                            "side nearest the planes.");
            }
            if (r.planeC <= 0 && s.layerCount >= 4)
                r.recommendations.push_back("Pour " + r.name + " as a plane next to a ground plane: the plane pair adds "
                                            "low-inductance capacitance at high frequency.");
        }
        if (r.planeC > 0 && r.cavityResonance > 0 && r.cavityResonance < 1e9)
            r.recommendations.push_back("First plane-cavity resonance of " + r.planeLayers + " near " + eng(r.cavityResonance, "Hz") +
                                        " (λ/2 across the pour): keep decoupling spread over the plane.");

        // IR drop: tracks, vias and poured copper from the source pads to every load pin.
        if (sourcePads.empty()) {
            r.irNote = "No regulator output, source or connector pin on the rail: IR drop not analysed.";
        } else if (r.loads.empty() || total <= 0) {
            r.irNote = "No load current on the rail.";
        } else {
            NetCopperGraph g = buildNetCopperGraph(pcb, pads, n);
            const size_t base = g.nodes.size();
            // Coarse mesh of the rail's pours.
            struct Mesh {
                size_t fill;
                double cg;
                int cols, rows;
                std::vector<int> node;  // coarse cell → network node or -1
            };
            std::vector<Mesh> meshes;
            size_t total_nodes = base;
            for (size_t k : railFills) {
                const ZoneFill& f = fills[k];
                Mesh m;
                m.fill = k;
                m.cg = std::max({0.5, f.cell, std::max(s.width, s.height) / 120.0});
                m.cols = std::max(1, static_cast<int>(std::ceil(f.cols * f.cell / m.cg)));
                m.rows = std::max(1, static_cast<int>(std::ceil(f.rows * f.cell / m.cg)));
                std::vector<int> count(static_cast<size_t>(m.cols * m.rows), 0), all(static_cast<size_t>(m.cols * m.rows), 0);
                for (int j = 0; j < f.rows; ++j)
                    for (int i = 0; i < f.cols; ++i) {
                        const int ci = std::min(m.cols - 1, static_cast<int>((i + 0.5) * f.cell / m.cg));
                        const int cj = std::min(m.rows - 1, static_cast<int>((j + 0.5) * f.cell / m.cg));
                        const size_t c = static_cast<size_t>(cj * m.cols + ci);
                        ++all[c];
                        if (f.island[static_cast<size_t>(j * f.cols + i)] >= 0) ++count[c];
                    }
                m.node.assign(count.size(), -1);
                for (size_t c = 0; c < count.size(); ++c)
                    if (all[c] > 0 && count[c] * 2 >= all[c]) m.node[c] = static_cast<int>(total_nodes++);
                meshes.push_back(std::move(m));
            }
            Network net2(total_nodes);
            const double rsCu = sheetResistance(cu);
            for (const auto& e : g.edges) {
                double res;
                if (e.via) res = kCopperResistivity * e.length * 1e-3 / (kPi * kPlating * (e.width + kPlating) * 1e-6);
                else res = kCopperResistivity * e.length * 1e-3 / (std::max(0.01, e.width) * cu * 1e-6);
                net2.add(e.a, e.b, 1 / std::max(1e-9, res));
            }
            for (const auto& m : meshes) {
                for (int j = 0; j < m.rows; ++j)
                    for (int i = 0; i < m.cols; ++i) {
                        const int a = m.node[static_cast<size_t>(j * m.cols + i)];
                        if (a < 0) continue;
                        if (i + 1 < m.cols) net2.add(a, m.node[static_cast<size_t>(j * m.cols + i + 1)], 1 / rsCu);
                        if (j + 1 < m.rows) net2.add(a, m.node[static_cast<size_t>((j + 1) * m.cols + i)], 1 / rsCu);
                    }
                // Contacts: copper nodes on the pour's layer and pads on it join the pour where it has copper.
                const ZoneFill& f = fills[m.fill];
                std::set<int> padNodes;
                std::map<int, size_t> padOf;
                for (const auto& [pi, node] : g.padNode) {
                    padNodes.insert(node);
                    padOf[node] = pi;
                }
                for (size_t k = 0; k < base; ++k) {
                    const auto& nd = g.nodes[k];
                    bool onLayer = nd.layer == f.layer;
                    if (padOf.count(static_cast<int>(k))) onLayer = onLayer || pads[padOf[static_cast<int>(k)]].onLayer(f.layer);
                    if (!onLayer) continue;
                    const int ci = std::clamp(static_cast<int>(nd.p.x / m.cg), 0, m.cols - 1);
                    const int cj = std::clamp(static_cast<int>(nd.p.y / m.cg), 0, m.rows - 1);
                    const int cell = m.node[static_cast<size_t>(cj * m.cols + ci)];
                    if (cell >= 0 && f.islandNear(nd.p, 0.2) >= 0) net2.add(static_cast<int>(k), cell, 1e4);
                }
            }
            std::vector<bool> isSource(total_nodes, false);
            for (size_t k : sourcePads)
                if (g.padNode.count(k)) isSource[static_cast<size_t>(g.padNode[k])] = true;
            std::vector<double> sink(total_nodes, 0);
            std::vector<int> loadNode(r.loads.size(), -1);
            for (size_t li = 0; li < r.loads.size(); ++li) {
                for (size_t k : railPads)
                    if (pads[k].componentId == r.loads[li].componentId && pinName(*sch.find(pads[k].componentId), pads[k].pinIndex) == r.loads[li].pin &&
                        g.padNode.count(k)) {
                        loadNode[li] = g.padNode[k];
                        break;
                    }
                if (loadNode[li] >= 0) sink[static_cast<size_t>(loadNode[li])] += r.loads[li].current;
            }
            const std::vector<double> drop = net2.solve(isSource, sink);
            r.irAnalyzed = true;
            const Component* src = sch.find(pads[sourcePads.front()].componentId);
            r.irSource = src ? src->ref + " " + pinName(*src, pads[sourcePads.front()].pinIndex) : "source";
            int unreached = 0;
            for (size_t li = 0; li < r.loads.size(); ++li) {
                const int node = loadNode[li];
                if (node < 0 || !std::isfinite(drop[static_cast<size_t>(node)])) {
                    r.loads[li].connected = false;
                    ++unreached;
                    continue;
                }
                r.loads[li].drop = drop[static_cast<size_t>(node)];
                if (r.loads[li].drop > r.irWorst) {
                    r.irWorst = r.loads[li].drop;
                    r.irWorstRef = r.loads[li].ref + " " + r.loads[li].pin;
                }
            }
            if (unreached) r.irNote = std::to_string(unreached) + " load pin(s) not connected to the source by copper (unrouted).";
            r.irLimitPercent = std::max(0.5, r.ripplePercent / 2);
        }
        out.push_back(std::move(r));
    }
    return out;
}

Json pdnJson(const std::vector<PdnRailResult>& rails) {
    Json root = Json::object();
    Json arr = Json::array();
    for (const auto& r : rails) {
        Json j = Json::object();
        j["net"] = r.net;
        j["name"] = r.name;
        j["voltage"] = r.voltage;
        j["voltageEstimated"] = r.voltageEstimated;
        j["ripplePercent"] = r.ripplePercent;
        j["transientCurrent"] = r.transientCurrent;
        j["dcCurrent"] = r.dcCurrent;
        j["currentEstimated"] = r.currentEstimated;
        j["target"] = std::isfinite(r.target) ? r.target : 0.0;
        j["fMax"] = r.fMax;
        j["vrmRef"] = r.vrmRef;
        j["vrmKind"] = r.vrmKind;
        j["vrmR"] = r.vrmR;
        j["vrmL"] = r.vrmL;
        Json decaps = Json::array();
        for (const auto& d : r.decaps) {
            Json x = Json::object();
            x["component"] = d.componentId;
            x["ref"] = d.ref;
            x["value"] = d.value;
            x["footprint"] = d.footprint;
            x["c"] = d.c;
            x["esr"] = d.esr;
            x["esl"] = d.esl;
            x["mounting"] = d.mounting;
            x["srf"] = d.srf;
            x["distance"] = d.distance;
            decaps.push(x);
        }
        j["decaps"] = decaps;
        Json plane = Json::object();
        plane["area"] = r.planeArea;
        plane["gap"] = r.planeGap;
        plane["capacitance"] = r.planeC;
        plane["inductance"] = r.planeL;
        plane["layers"] = r.planeLayers;
        plane["cavityResonance"] = r.cavityResonance;
        j["plane"] = plane;
        Json curve = Json::object();
        Json f = Json::array(), z = Json::array();
        for (size_t k = 0; k < r.freq.size(); ++k) {
            f.push(r.freq[k]);
            z.push(r.z[k]);
        }
        curve["freq"] = f;
        curve["z"] = z;
        j["curve"] = curve;
        Json peaks = Json::array();
        for (const auto& p : r.peaks) {
            Json x = Json::object();
            x["f"] = p.f;
            x["z"] = p.z;
            peaks.push(x);
        }
        j["peaks"] = peaks;
        j["worstZ"] = r.worstZ;
        j["worstF"] = r.worstF;
        j["compliant"] = r.compliant;
        Json ir = Json::object();
        ir["analyzed"] = r.irAnalyzed;
        ir["source"] = r.irSource;
        ir["note"] = r.irNote;
        ir["worst"] = r.irWorst;
        ir["worstRef"] = r.irWorstRef;
        ir["limitPercent"] = r.irLimitPercent;
        Json loads = Json::array();
        for (const auto& l : r.loads) {
            Json x = Json::object();
            x["component"] = l.componentId;
            x["ref"] = l.ref;
            x["pin"] = l.pin;
            x["current"] = l.current;
            x["drop"] = l.drop;
            x["connected"] = l.connected;
            loads.push(x);
        }
        ir["loads"] = loads;
        j["irDrop"] = ir;
        Json recs = Json::array();
        for (const auto& t : r.recommendations) recs.push(t);
        j["recommendations"] = recs;
        arr.push(j);
    }
    root["rails"] = arr;
    return root;
}

}  // namespace sieda
