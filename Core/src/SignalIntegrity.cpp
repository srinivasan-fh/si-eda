#include "sieda/SignalIntegrity.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <functional>
#include <limits>
#include <map>
#include <numeric>
#include <queue>
#include <set>
#include <tuple>

#include "sieda/Channel.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/Eye.hpp"
#include "sieda/PowerIntegrity.hpp"
#include "sieda/Project.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {

constexpr double kLightMmPerS = 299792458.0e3;  // c in mm/s
constexpr double kEps0 = 8.8541878128e-12;       // F/m

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string ohms(double r) { return formatEngineeringValue(r, "Ω", 3); }
std::string secs(double t) { return formatEngineeringValue(t, "s", 3); }
std::string volts(double v) { return formatEngineeringValue(v, "V", 3); }

double agm(double a, double b) {
    for (int i = 0; i < 64 && std::fabs(a - b) > 1e-15 * a; ++i) {
        double an = 0.5 * (a + b);
        b = std::sqrt(a * b);
        a = an;
    }
    return 0.5 * (a + b);
}

/// K(k') / K(k) from both moduli (avoids cancellation in √(1 − k²) near k → 1).
double ellipticRatio(double k, double kp) { return agm(1.0, kp) / agm(1.0, k); }

}  // namespace

// ---- physics -----------------------------------------------------------------------------------------------------------

double effectivePermittivity(const BoardSettings& s, int layer, double w) {
    const double er = boardLaminate(s).er;
    if (isStriplineLayer(s, layer)) return er;
    const double h = std::max(1e-3, impedanceReferenceHeight(s, layer));
    const double u = std::max(1e-3, w) / h;
    double e = (er + 1) / 2 + (er - 1) / 2 / std::sqrt(1 + 12 / u);
    if (u < 1) e += (er - 1) / 2 * 0.04 * (1 - u) * (1 - u);
    return e;
}

double propagationDelayPerMm(const BoardSettings& s, int layer, double w) {
    return std::sqrt(effectivePermittivity(s, layer, w)) / kLightMmPerS;
}

double viaCapacitance(double lengthMm, double padMm, double antipadMm, double er) {
    if (!(lengthMm > 0) || !(padMm > 0)) return 0;
    const double gap = std::max(0.02, antipadMm - padMm);
    return 1.41e-12 * er * (lengthMm / 25.4) * padMm / gap;
}

double viaInductance(double lengthMm, double drillMm) {
    if (!(lengthMm > 0) || !(drillMm > 0)) return 0;
    const double h = lengthMm / 25.4, d = drillMm / 25.4;
    return std::max(0.0, 5.08e-9 * h * (std::log(4 * h / d) + 1));
}

double criticalLength(double riseTime, double delayPerMm) {
    return delayPerMm > 0 ? riseTime / (6.0 * delayPerMm) : 0;
}

double ellipticK(double k) {
    if (!(k < 1)) return std::numeric_limits<double>::infinity();
    return kPi / (2 * agm(1.0, std::sqrt(1 - k * k)));
}

std::pair<double, double> coupledStriplineImpedance(double w, double s, double b, double er) {
    const double a = std::tanh(kPi * w / (2 * b)), c = std::tanh(kPi * (w + s) / (2 * b));
    const double ke = a * c, ko = a / c;
    const double z = 30 * kPi / std::sqrt(er);
    auto zOf = [&](double k) { return z * ellipticRatio(k, std::sqrt(std::max(0.0, 1 - k * k))); };
    return {zOf(ke), zOf(ko)};
}

CouplingEstimate crosstalkCoupling(const BoardSettings& s, int layer, double w1, double w2, double gap, double coupledMm,
                                   double riseTime) {
    CouplingEstimate e;
    const double w = std::max(0.01, 0.5 * (w1 + w2));
    gap = std::max(0.01, gap);
    const double er = boardLaminate(s).er;
    const double tpd = propagationDelayPerMm(s, layer, w);
    if (isStriplineLayer(s, layer)) {
        const double b = std::max(0.05, impedanceReferenceHeight(s, layer));
        auto [ze, zo] = coupledStriplineImpedance(w, gap, b, er);
        e.kl = e.kc = ze + zo > 0 ? (ze - zo) / (ze + zo) : 0;
    } else {
        const double t = copperThickness(s);
        const double H = std::max(0.01, impedanceReferenceHeight(s, layer)) + t / 2;
        const double D = gap + w;
        const double r = (w + t) / 4;
        const double self = std::log(std::max(1.2, 2 * H / r));
        e.kl = std::min(0.9, std::log(1 + (2 * H / D) * (2 * H / D)) / (2 * self));
        const double gamma = std::clamp((er + 1) / 2 / effectivePermittivity(s, layer, w), 0.0, 1.0);
        e.kc = e.kl * gamma;
    }
    e.kb = (e.kl + e.kc) / 4;
    const double td = coupledMm * tpd, tr = std::max(1e-13, riseTime);
    e.next = e.kb * std::min(1.0, 2 * td / tr);
    e.fext = std::clamp(0.5 * (e.kc - e.kl) * td / tr, -0.5, 0.5);
    return e;
}

CouplingEstimate broadsideCoupling(const BoardSettings& s, int layerA, int layerB, double w1, double w2, double offset,
                                   double coupledMm, double riseTime) {
    CouplingEstimate e;
    const int lo = std::min(layerA, layerB), hi = std::max(layerA, layerB);
    const int n = std::max(1, s.layerCount);
    const double t = copperThickness(s);
    const double w = std::max(0.01, 0.5 * (w1 + w2));
    const double d = dielectricBelow(s, lo) + t;  // centre to centre, vertically
    // Image plane: the nearest plane outside the pair (below the lower conductor, or above the upper one on top).
    double h2;
    if (hi + 1 < n) h2 = dielectricBelow(s, hi) + t / 2;
    else h2 = lo > 0 ? dielectricBelow(s, lo - 1) + t / 2 : d;  // the pair's own spacing when no plane is found
    const double h1 = h2 + d;
    const double r = (w + t) / 4;
    const double x = std::max(0.0, offset);
    const double l1 = std::log(std::max(1.2, 2 * h1 / r)), l2 = std::log(std::max(1.2, 2 * h2 / r));
    const double mutual = std::log((x * x + (h1 + h2) * (h1 + h2)) / std::max(1e-12, x * x + (h1 - h2) * (h1 - h2)));
    e.kl = std::min(0.9, mutual / (2 * std::sqrt(l1 * l2)));
    const bool buried = isStriplineLayer(s, layerA) && isStriplineLayer(s, layerB);
    if (buried) {
        e.kc = e.kl;
    } else {
        const double er = boardLaminate(s).er;
        const int outer = isStriplineLayer(s, layerA) ? layerB : layerA;
        e.kc = e.kl * std::clamp((er + 1) / 2 / effectivePermittivity(s, outer, w), 0.0, 1.0);
    }
    e.kb = (e.kl + e.kc) / 4;
    const int layer = isStriplineLayer(s, layerA) ? layerA : layerB;
    const double td = coupledMm * propagationDelayPerMm(s, layer, w), tr = std::max(1e-13, riseTime);
    e.next = e.kb * std::min(1.0, 2 * td / tr);
    e.fext = std::clamp(0.5 * (e.kc - e.kl) * td / tr, -0.5, 0.5);
    return e;
}

// ---- time-domain network ---------------------------------------------------------------------------------------------

TlNetwork::TlNetwork(int n)
    : nodes(n), capacitance(static_cast<size_t>(n), 0.0), conductance(static_cast<size_t>(n), 0.0),
      railCurrent(static_cast<size_t>(n), 0.0) {}

int TlNetwork::addNode() {
    capacitance.push_back(0);
    conductance.push_back(0);
    railCurrent.push_back(0);
    return nodes++;
}

void TlNetwork::addLine(int a, int b, double z0, double delay) { lines.push_back({a, b, z0, delay}); }

TlRun simulateTl(const TlNetwork& net, const std::vector<int>& probes, double dt, double t0, double tFall, double stop) {
    TlRun run;
    run.dt = dt;
    run.edgeStart = t0;
    run.fallStart = tFall;
    run.stop = stop;
    const int n = net.nodes;
    if (n <= 0 || !(dt > 0) || !(stop > 0)) return run;
    // Lines shorter than half a step are electrically short: lump their capacitance (delay / Z0) and join the ends.
    std::vector<int> parent(static_cast<size_t>(n));
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    struct L {
        int a, b;
        double z;
        int steps;
    };
    std::vector<L> lines;
    std::vector<std::pair<int, double>> lumped;
    for (const auto& line : net.lines) {
        if (!(line.z0 > 0) || line.a < 0 || line.b < 0 || line.a >= n || line.b >= n) continue;
        const long steps = std::lround(line.delay / dt);
        if (steps < 1) {
            parent[static_cast<size_t>(find(line.a))] = find(line.b);
            lumped.push_back({line.a, line.delay / line.z0});
        } else {
            lines.push_back({line.a, line.b, line.z0, static_cast<int>(std::min(steps, 1000000L))});
        }
    }
    std::vector<int> id(static_cast<size_t>(n), -1), idx(static_cast<size_t>(n));
    int m = 0;
    for (int i = 0; i < n; ++i) {
        int r = find(i);
        if (id[static_cast<size_t>(r)] < 0) id[static_cast<size_t>(r)] = m++;
        idx[static_cast<size_t>(i)] = id[static_cast<size_t>(r)];
    }
    std::vector<double> C(static_cast<size_t>(m), 0.0), G(static_cast<size_t>(m), 0.0), J(static_cast<size_t>(m), 0.0);
    for (int i = 0; i < n; ++i) {
        const size_t k = static_cast<size_t>(idx[static_cast<size_t>(i)]);
        C[k] += net.capacitance[static_cast<size_t>(i)];
        G[k] += net.conductance[static_cast<size_t>(i)];
        J[k] += net.railCurrent[static_cast<size_t>(i)];
    }
    for (const auto& [node, c] : lumped) C[static_cast<size_t>(idx[static_cast<size_t>(node)])] += c;
    for (auto& l : lines) {
        l.a = idx[static_cast<size_t>(l.a)];
        l.b = idx[static_cast<size_t>(l.b)];
    }
    const size_t drv = static_cast<size_t>(idx[static_cast<size_t>(std::clamp(net.driverNode, 0, n - 1))]);
    const double gs = 1.0 / std::max(1e-3, net.rSource);
    // DC state with the source low: every node is joined by lines (shorts at DC), one voltage.
    double gsum = gs, jsum = 0;
    for (int k = 0; k < m; ++k) {
        gsum += G[static_cast<size_t>(k)];
        jsum += J[static_cast<size_t>(k)];
    }
    const double vdc = jsum / gsum;
    std::vector<double> v(static_cast<size_t>(m), vdc), gtot(static_cast<size_t>(m), 0.0), capHist(static_cast<size_t>(m), 0.0),
        inj(static_cast<size_t>(m), 0.0);
    for (size_t k = 0; k < static_cast<size_t>(m); ++k) {
        gtot[k] = G[k] + 2 * C[k] / dt;
        capHist[k] = 2 * C[k] / dt * vdc;
    }
    gtot[drv] += gs;
    // DC line currents: each node's lumped current (terminations, the driver) flows through the lines, which are
    // shorts at DC. Distributed over a spanning tree, leaves first (currents in loops of lines start at zero).
    std::vector<double> lineCurrent(lines.size(), 0.0);  // into the line at its `a` end
    {
        std::vector<double> excess(static_cast<size_t>(m), 0.0);  // current each node must push into its lines
        for (size_t k = 0; k < static_cast<size_t>(m); ++k) excess[k] = J[k] - G[k] * vdc;
        excess[drv] -= gs * vdc;
        std::vector<std::vector<std::pair<int, size_t>>> adj(static_cast<size_t>(m));
        for (size_t l = 0; l < lines.size(); ++l)
            if (lines[l].a != lines[l].b) {
                adj[static_cast<size_t>(lines[l].a)].push_back({lines[l].b, l});
                adj[static_cast<size_t>(lines[l].b)].push_back({lines[l].a, l});
            }
        std::vector<int> parentLine(static_cast<size_t>(m), -1), order;
        std::vector<bool> seen(static_cast<size_t>(m), false);
        for (int root = 0; root < m; ++root) {
            if (seen[static_cast<size_t>(root)]) continue;
            seen[static_cast<size_t>(root)] = true;
            order.push_back(root);
            for (size_t head = order.size() - 1; head < order.size(); ++head) {
                const int x = order[head];
                for (const auto& [y, l] : adj[static_cast<size_t>(x)])
                    if (!seen[static_cast<size_t>(y)]) {
                        seen[static_cast<size_t>(y)] = true;
                        parentLine[static_cast<size_t>(y)] = static_cast<int>(l);
                        order.push_back(y);
                    }
            }
        }
        for (size_t k = order.size(); k-- > 0;) {
            const int x = order[k];
            const int l = parentLine[static_cast<size_t>(x)];
            if (l < 0) continue;
            const double push = excess[static_cast<size_t>(x)];
            const L& line = lines[static_cast<size_t>(l)];
            const int other = line.a == x ? line.b : line.a;
            lineCurrent[static_cast<size_t>(l)] = line.a == x ? push : -push;
            excess[static_cast<size_t>(other)] += push;
        }
    }
    std::vector<std::vector<double>> qa(lines.size()), qb(lines.size());
    for (size_t l = 0; l < lines.size(); ++l) {
        gtot[static_cast<size_t>(lines[l].a)] += 1 / lines[l].z;
        gtot[static_cast<size_t>(lines[l].b)] += 1 / lines[l].z;
        qa[l].assign(static_cast<size_t>(lines[l].steps), vdc / lines[l].z + lineCurrent[l]);
        qb[l].assign(static_cast<size_t>(lines[l].steps), vdc / lines[l].z - lineCurrent[l]);
    }
    const double rampR = std::max(1e-15, net.riseTime / 0.8), rampF = std::max(1e-15, net.fallTime / 0.8);
    auto source = [&](double t) {
        if (t < t0) return 0.0;
        if (t < t0 + rampR) return net.vHigh * (t - t0) / rampR;
        if (t < tFall) return net.vHigh;
        if (t < tFall + rampF) return net.vHigh * (1 - (t - tFall) / rampF);
        return 0.0;
    };
    const size_t steps = static_cast<size_t>(std::ceil(stop / dt)) + 1;
    run.time.reserve(steps);
    run.source.reserve(steps);
    run.probes.assign(probes.size(), {});
    for (auto& p : run.probes) p.reserve(steps);
    std::vector<int> probeIdx;
    for (int p : probes) probeIdx.push_back(idx[static_cast<size_t>(std::clamp(p, 0, n - 1))]);
    std::vector<double> ha(lines.size()), hb(lines.size());
    for (size_t k = 0; k < steps; ++k) {
        const double t = static_cast<double>(k) * dt;
        const double vs = source(t);
        for (size_t j = 0; j < static_cast<size_t>(m); ++j) inj[j] = J[j] + capHist[j];
        inj[drv] += vs * gs;
        for (size_t l = 0; l < lines.size(); ++l) {
            const size_t slot = k % static_cast<size_t>(lines[l].steps);
            ha[l] = qb[l][slot];  // wave that left b one delay ago arrives at a
            hb[l] = qa[l][slot];
            inj[static_cast<size_t>(lines[l].a)] += ha[l];
            inj[static_cast<size_t>(lines[l].b)] += hb[l];
        }
        for (size_t j = 0; j < static_cast<size_t>(m); ++j) v[j] = gtot[j] > 0 ? inj[j] / gtot[j] : 0;
        for (size_t l = 0; l < lines.size(); ++l) {
            const size_t slot = k % static_cast<size_t>(lines[l].steps);
            qa[l][slot] = 2 * v[static_cast<size_t>(lines[l].a)] / lines[l].z - ha[l];
            qb[l][slot] = 2 * v[static_cast<size_t>(lines[l].b)] / lines[l].z - hb[l];
        }
        for (size_t j = 0; j < static_cast<size_t>(m); ++j) {
            if (C[j] <= 0) continue;
            const double g = 2 * C[j] / dt;
            const double i = g * v[j] - capHist[j];
            capHist[j] = g * v[j] + i;
        }
        run.time.push_back(t);
        run.source.push_back(vs);
        for (size_t p = 0; p < probeIdx.size(); ++p) run.probes[p].push_back(v[static_cast<size_t>(probeIdx[p])]);
    }
    return run;
}

EdgeMetrics measureEdges(const TlRun& run, size_t probe, double vih, double vil) {
    EdgeMetrics e;
    if (probe >= run.probes.size() || run.time.size() < 4) return e;
    const auto& v = run.probes[probe];
    const auto& t = run.time;
    const size_t n = v.size();
    size_t kEdge = 0, kFall = n;
    while (kEdge < n && t[kEdge] < run.edgeStart) ++kEdge;
    for (size_t k = 0; k < n; ++k)
        if (t[k] >= run.fallStart) {
            kFall = k;
            break;
        }
    if (kEdge + 2 >= kFall || kFall >= n) return e;
    e.vLow = v[kEdge > 0 ? kEdge - 1 : 0];
    e.vHigh = v[kFall - 1];
    const double lowEnd = v.back();
    const double swing = e.vHigh - e.vLow;
    if (swing < 1e-6) {
        e.reachesHigh = e.reachesLow = false;
        return e;
    }
    double vmax = -1e300, vmin = 1e300;
    for (size_t k = kEdge; k < kFall; ++k) vmax = std::max(vmax, v[k]);
    for (size_t k = kFall; k < n; ++k) vmin = std::min(vmin, v[k]);
    e.overshoot = std::max(0.0, vmax - e.vHigh);
    e.undershoot = std::max(0.0, lowEnd - vmin);
    // Ringback: after first reaching VIH (VIL), how far it comes back.
    size_t k = kEdge;
    while (k < kFall && v[k] < vih) ++k;
    if (k >= kFall) e.reachesHigh = false;
    else {
        double low = 1e300;
        for (size_t j = k; j < kFall; ++j) low = std::min(low, v[j]);
        e.ringbackHigh = low - vih;
    }
    k = kFall;
    while (k < n && v[k] > vil) ++k;
    if (k >= n) e.reachesLow = false;
    else {
        double high = -1e300;
        for (size_t j = k; j < n; ++j) high = std::max(high, v[j]);
        e.ringbackLow = vil - high;
    }
    // Settling: last time outside ±5 % of the swing around the high level.
    size_t last = kEdge;
    for (size_t j = kEdge; j < kFall; ++j)
        if (std::fabs(v[j] - e.vHigh) > 0.05 * swing) last = j;
    e.settling = t[std::min(last + 1, kFall - 1)] - run.edgeStart;
    e.settled = e.settling < 0.9 * (run.fallStart - run.edgeStart);
    // Flight time: 50 % crossings of the receiver and of the ideal source.
    auto crossing = [&](const std::vector<double>& w, double level, size_t from, size_t to) {
        for (size_t j = from + 1; j < to; ++j)
            if (w[j - 1] < level && w[j] >= level) return t[j - 1] + (t[j] - t[j - 1]) * (level - w[j - 1]) / (w[j] - w[j - 1]);
        return std::numeric_limits<double>::quiet_NaN();
    };
    double srcHigh = 0;
    for (size_t j = kEdge; j < kFall; ++j) srcHigh = std::max(srcHigh, run.source[j]);
    const double ts = crossing(run.source, 0.5 * srcHigh, kEdge > 0 ? kEdge - 1 : 0, kFall);
    const double tr = crossing(v, e.vLow + 0.5 * swing, kEdge > 0 ? kEdge - 1 : 0, kFall);
    e.flightTime = std::isfinite(ts) && std::isfinite(tr) ? tr - ts : 0;
    return e;
}

// ---- settings ----------------------------------------------------------------------------------------------------------

const DriverModel* SiSettings::findModel(const std::string& id) const {
    for (const auto& m : models)
        if (m.id == id) return &m;
    return findLogicFamily(id);
}

const PdnRailSettings* SiSettings::rail(const std::string& net) const {
    for (const auto& r : rails)
        if (r.net == net) return &r;
    return nullptr;
}

bool SiSettings::isDefault() const {
    return models.empty() && componentModels.empty() && pinModels.empty() && netModels.empty() && rails.empty() && !signOff &&
           std::fabs(overshootLimit - 0.15) < 1e-12 && std::fabs(crosstalkLimit - 0.05) < 1e-12 && copperFoil.empty() &&
           channels.empty();
}

const SiSettings::ChannelSpec* SiSettings::channel(const std::string& net) const {
    for (const auto& c : channels)
        if (c.net == net) return &c;
    return nullptr;
}

Json SiSettings::toJson() const {
    Json j = Json::object();
    j["signOff"] = signOff;
    j["overshootLimit"] = overshootLimit;
    j["crosstalkLimit"] = crosstalkLimit;
    Json ms = Json::array();
    for (const auto& m : models) ms.push(driverModelToJson(m));
    j["models"] = ms;
    auto map = [](const std::map<std::string, std::string>& m) {
        Json o = Json::object();
        for (const auto& [k, v] : m) o[k] = v;
        return o;
    };
    j["componentModels"] = map(componentModels);
    j["pinModels"] = map(pinModels);
    j["netModels"] = map(netModels);
    Json rs = Json::array();
    for (const auto& r : rails) {
        Json x = Json::object();
        x["net"] = r.net;
        x["ripplePercent"] = r.ripplePercent;
        x["transientCurrent"] = r.transientCurrent;
        x["dcCurrent"] = r.dcCurrent;
        if (r.vrmR > 0) x["vrmR"] = r.vrmR;
        if (r.vrmBandwidth > 0) x["vrmBandwidth"] = r.vrmBandwidth;
        rs.push(x);
    }
    j["rails"] = rs;
    if (!copperFoil.empty()) j["copperFoil"] = copperFoil;
    if (!channels.empty()) {
        Json cs = Json::array();
        for (const auto& c : channels) {
            Json x = Json::object();
            x["net"] = c.net;
            x["bitRate"] = c.bitRate;
            x["maskHeight"] = c.maskHeight;
            x["maskWidthUi"] = c.maskWidthUi;
            cs.push(x);
        }
        j["channels"] = cs;
    }
    return j;
}

SiSettings SiSettings::fromJson(const Json& j) {
    SiSettings s;
    if (!j.isObject()) return s;
    s.signOff = j.get("signOff").asBool(false);
    s.overshootLimit = std::clamp(j.get("overshootLimit").asNumber(0.15), 0.01, 1.0);
    s.crosstalkLimit = std::clamp(j.get("crosstalkLimit").asNumber(0.05), 0.005, 0.5);
    if (j.get("models").isArray())
        for (const auto& m : j.get("models").items()) {
            DriverModel d = driverModelFromJson(m);
            if (!d.id.empty()) s.models.push_back(d);
        }
    auto map = [](const Json& o, std::map<std::string, std::string>& out) {
        if (!o.isObject()) return;
        for (const auto& [k, v] : o.fields())
            if (v.isString() && !k.empty()) out[k] = v.asString();
    };
    map(j.get("componentModels"), s.componentModels);
    map(j.get("pinModels"), s.pinModels);
    map(j.get("netModels"), s.netModels);
    if (j.get("rails").isArray())
        for (const auto& r : j.get("rails").items()) {
            PdnRailSettings x;
            x.net = r.get("net").asString("");
            x.ripplePercent = std::clamp(r.get("ripplePercent").asNumber(0), 0.0, 50.0);
            x.transientCurrent = std::clamp(r.get("transientCurrent").asNumber(0), 0.0, 1000.0);
            x.dcCurrent = std::clamp(r.get("dcCurrent").asNumber(0), 0.0, 1000.0);
            x.vrmR = std::clamp(r.get("vrmR").asNumber(0), 0.0, 10.0);
            x.vrmBandwidth = std::clamp(r.get("vrmBandwidth").asNumber(0), 0.0, 100e6);
            if (!x.net.empty()) s.rails.push_back(x);
        }
    s.copperFoil = j.get("copperFoil").asString("");
    if (j.get("channels").isArray())
        for (const auto& c : j.get("channels").items()) {
            ChannelSpec x;
            x.net = c.get("net").asString("");
            x.bitRate = std::clamp(c.get("bitRate").asNumber(0), 0.0, 200e9);
            x.maskHeight = std::clamp(c.get("maskHeight").asNumber(0), 0.0, 100.0);
            x.maskWidthUi = std::clamp(c.get("maskWidthUi").asNumber(0), 0.0, 0.99);
            if (!x.net.empty() && x.bitRate > 0) s.channels.push_back(x);
        }
    return s;
}

// ---- copper graph ------------------------------------------------------------------------------------------------------

std::vector<bool> NetCopperGraph::reachable(int start) const {
    std::vector<bool> seen(nodes.size(), false);
    if (start < 0 || start >= static_cast<int>(nodes.size())) return seen;
    std::vector<std::vector<int>> adj(nodes.size());
    for (const auto& e : edges) {
        adj[static_cast<size_t>(e.a)].push_back(e.b);
        adj[static_cast<size_t>(e.b)].push_back(e.a);
    }
    std::vector<int> stack{start};
    seen[static_cast<size_t>(start)] = true;
    while (!stack.empty()) {
        int x = stack.back();
        stack.pop_back();
        for (int y : adj[static_cast<size_t>(x)])
            if (!seen[static_cast<size_t>(y)]) {
                seen[static_cast<size_t>(y)] = true;
                stack.push_back(y);
            }
    }
    return seen;
}

NetCopperGraph buildNetCopperGraph(const PcbLayout& pcb, const std::vector<Pad>& pads, int net) {
    NetCopperGraph g;
    const BoardSettings& s = pcb.settings;
    const int layers = std::max(1, s.layerCount);
    constexpr double kTol = 1e-3;
    std::vector<const Track*> tracks;
    for (const auto& t : pcb.tracks)
        if (t.net == net && (t.b - t.a).length() > 1e-9) tracks.push_back(&t);
    std::vector<const Via*> vias;
    for (const auto& v : pcb.vias)
        if (v.net == net) vias.push_back(&v);

    std::map<std::tuple<int, long long, long long>, int> byKey;
    auto nodeAt = [&](int layer, Vec2 p) {
        auto key = std::make_tuple(layer, std::llround(p.x / kTol), std::llround(p.y / kTol));
        auto it = byKey.find(key);
        if (it != byKey.end()) return it->second;
        g.nodes.push_back({p, layer});
        int id = static_cast<int>(g.nodes.size()) - 1;
        byKey[key] = id;
        return id;
    };
    // Track sections, split where another track of the net ends on them (T-junctions).
    for (const Track* t : tracks) {
        const Vec2 d = t->b - t->a;
        const double len = d.length();
        std::vector<double> cuts{0.0, 1.0};
        for (const Track* o : tracks) {
            if (o == t || o->layer != t->layer) continue;
            for (Vec2 p : {o->a, o->b}) {
                const double u = (p - t->a).dot(d) / (len * len);
                if (u * len > kTol && (1 - u) * len > kTol && pointSegmentDistance(p, t->a, t->b) < kTol) cuts.push_back(u);
            }
        }
        for (const Via* v : vias)
            if (v->spans(t->layer)) {
                const double u = (v->position - t->a).dot(d) / (len * len);
                if (u * len > kTol && (1 - u) * len > kTol && pointSegmentDistance(v->position, t->a, t->b) < kTol)
                    cuts.push_back(u);
            }
        std::sort(cuts.begin(), cuts.end());
        for (size_t k = 1; k < cuts.size(); ++k) {
            if ((cuts[k] - cuts[k - 1]) * len < 1e-6) continue;
            NetCopperGraph::Edge e;
            e.a = nodeAt(t->layer, t->a + d * cuts[k - 1]);
            e.b = nodeAt(t->layer, t->a + d * cuts[k]);
            e.layer = t->layer;
            e.width = t->width;
            e.length = (cuts[k] - cuts[k - 1]) * len;
            g.edges.push_back(e);
        }
    }
    // Via landings on every layer the barrel spans, joined by barrel sections.
    for (const Via* v : vias) {
        const int from = std::max(0, v->fromLayer), to = std::min(layers - 1, v->lastLayer(layers));
        int prev = -1;
        for (int l = from; l <= to; ++l) {
            int node = nodeAt(l, v->position);
            if (prev >= 0) {
                NetCopperGraph::Edge e;
                e.a = prev;
                e.b = node;
                e.via = true;
                e.layer = l - 1;
                e.width = v->drill;
                e.diameter = v->diameter;
                e.length = std::max(1e-3, layerDepth(s, l) - layerDepth(s, l - 1));
                g.edges.push_back(e);
            }
            prev = node;
        }
    }
    // Union-find joins: track ends inside a via land or a pad.
    const size_t copperNodes = g.nodes.size();
    std::vector<int> parent(copperNodes);
    std::iota(parent.begin(), parent.end(), 0);
    std::function<int(int)> find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    };
    auto unite = [&](int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[static_cast<size_t>(a)] = b;
    };
    for (const Via* v : vias)
        for (size_t i = 0; i < copperNodes; ++i) {
            const auto& nd = g.nodes[i];
            if (nd.layer < 0 || !v->spans(nd.layer)) continue;
            if ((nd.p - v->position).length() <= v->diameter / 2 + kTol)
                unite(static_cast<int>(i), nodeAt(nd.layer, v->position));
        }
    // Pads: one node each (extra nodes past the copper ones).
    std::vector<std::pair<size_t, int>> padNodes;
    for (size_t pi = 0; pi < pads.size(); ++pi) {
        const Pad& pad = pads[pi];
        if (pad.net != net) continue;
        g.nodes.push_back({pad.position, -1});
        padNodes.push_back({pi, static_cast<int>(g.nodes.size()) - 1});
    }
    parent.resize(g.nodes.size());
    for (size_t i = copperNodes; i < g.nodes.size(); ++i) parent[i] = static_cast<int>(i);
    for (const auto& [pi, pn] : padNodes) {
        const Pad& pad = pads[pi];
        const Rect box = pad.bounds().inflated(kTol);
        for (size_t i = 0; i < copperNodes; ++i) {
            const auto& nd = g.nodes[i];
            if (nd.layer < 0 || !pad.onLayer(nd.layer)) continue;
            bool inside = pad.round ? (nd.p - pad.position).length() <= std::max(pad.size.x, pad.size.y) / 2 + kTol
                                    : box.contains(nd.p);
            if (inside) unite(static_cast<int>(i), pn);
        }
    }
    // Compress.
    std::vector<int> remap(g.nodes.size(), -1);
    std::vector<NetCopperGraph::Node> nodes;
    for (size_t i = 0; i < g.nodes.size(); ++i) {
        int r = find(static_cast<int>(i));
        if (remap[static_cast<size_t>(r)] < 0) {
            remap[static_cast<size_t>(r)] = static_cast<int>(nodes.size());
            nodes.push_back(g.nodes[static_cast<size_t>(r)]);
        }
    }
    // A node that holds a pad keeps the pad's position and spans its layers.
    for (const auto& [pi, pn] : padNodes) {
        int id = remap[static_cast<size_t>(find(pn))];
        g.padNode[pi] = id;
        nodes[static_cast<size_t>(id)].p = pads[pi].position;
        if (pads[pi].throughHole) nodes[static_cast<size_t>(id)].layer = -1;
    }
    std::vector<NetCopperGraph::Edge> edges;
    for (auto e : g.edges) {
        e.a = remap[static_cast<size_t>(find(e.a))];
        e.b = remap[static_cast<size_t>(find(e.b))];
        if (e.a != e.b) edges.push_back(e);
    }
    g.nodes = std::move(nodes);
    g.edges = std::move(edges);
    return g;
}

double railVoltageFromName(const std::string& name) {
    const std::string n = upper(name);
    for (size_t i = 0; i < n.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(n[i]))) continue;
        if (i > 0 && (std::isdigit(static_cast<unsigned char>(n[i - 1])) || n[i - 1] == '.')) continue;
        size_t j = i;
        while (j < n.size() && std::isdigit(static_cast<unsigned char>(n[j]))) ++j;
        std::string whole = n.substr(i, j - i), frac;
        if (j < n.size() && n[j] == '.') {
            size_t k = j + 1;
            while (k < n.size() && std::isdigit(static_cast<unsigned char>(n[k]))) ++k;
            frac = n.substr(j + 1, k - j - 1);
            j = k;
        }
        if (j >= n.size() || n[j] != 'V') continue;
        size_t k = j + 1;
        std::string after;
        while (k < n.size() && std::isdigit(static_cast<unsigned char>(n[k]))) after += n[k++];
        if (k < n.size() && std::isalpha(static_cast<unsigned char>(n[k]))) continue;  // "5VA…" is not a rail voltage
        if (!frac.empty() && !after.empty()) continue;
        const std::string text = whole + "." + (frac.empty() ? (after.empty() ? "0" : after) : frac);
        const double v = std::strtod(text.c_str(), nullptr);
        if (v > 0 && v <= 1000) return v;
    }
    return 0;
}

// ---- per-net analysis ---------------------------------------------------------------------------------------------------

namespace {

std::string pinLabel(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    if (pin < 0 || pin >= static_cast<int>(pins.size())) return std::to_string(pin + 1);
    const auto& p = pins[static_cast<size_t>(pin)];
    if (!p.number.empty()) return p.number + (p.name.empty() || p.name == p.number ? "" : " (" + p.name + ")");
    return p.name.empty() ? std::to_string(pin + 1) : p.name;
}

PinType pinTypeOf(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    if (c.kind != ComponentKind::Custom || pin < 0 || pin >= static_cast<int>(pins.size())) return PinType::Passive;
    return static_cast<PinType>(pins[static_cast<size_t>(pin)].type);
}

std::string pinNameUpper(const Component& c, int pin) {
    const auto& pins = c.def().pins;
    return pin >= 0 && pin < static_cast<int>(pins.size()) ? upper(pins[static_cast<size_t>(pin)].name) : std::string();
}

bool isIcLike(const Component& c) {
    return c.kind == ComponentKind::Custom || c.kind == ComponentKind::IC8 || c.kind == ComponentKind::OpAmp;
}

/// Model assigned to a pin: by pin, then component, then net. nullptr when none.
const DriverModel* assignedModel(const Project& p, const Component& c, int pin, const std::string& netName) {
    const SiSettings& si = p.si;
    const auto& pins = c.def().pins;
    if (pin >= 0 && pin < static_cast<int>(pins.size())) {
        const auto& pd = pins[static_cast<size_t>(pin)];
        for (const std::string& key : {pd.number, pd.name}) {
            if (key.empty()) continue;
            auto it = si.pinModels.find(c.ref + "." + key);
            if (it != si.pinModels.end())
                if (const DriverModel* m = si.findModel(it->second)) return m;
        }
    }
    if (auto it = si.componentModels.find(c.ref); it != si.componentModels.end())
        if (const DriverModel* m = si.findModel(it->second)) return m;
    if (!netName.empty())
        if (auto it = si.netModels.find(netName); it != si.netModels.end())
            if (const DriverModel* m = si.findModel(it->second)) return m;
    return nullptr;
}

/// Logic family when nothing is assigned: DDR nets by name / memory design, else by the part's I/O supply voltage.
const DriverModel& defaultModel(const Project& p, const Component* c, const std::string& netName) {
    const Schematic& sch = p.schematic;
    double maxSupply = 0, minSupply = 0;
    if (c && c->kind == ComponentKind::Custom) {
        const auto& pins = c->def().pins;
        for (size_t k = 0; k < pins.size(); ++k) {
            if (static_cast<PinType>(pins[k].type) != PinType::PowerIn) continue;
            int n = sch.netOf({c->id, static_cast<int>(k)});
            if (n < 0 || sch.netRole(n) != NetRole::Power) continue;
            double v = railVoltageFromName(sch.nets()[static_cast<size_t>(n)].name);
            if (v <= 0 || v > 15) continue;
            maxSupply = std::max(maxSupply, v);
            if (v >= 1.1 && (minSupply == 0 || v < minSupply)) minSupply = v;
        }
    }
    const std::string n = upper(netName);
    const bool ddrDesign = p.memoryDesign == "ddr" || p.memoryDesign == "lpddr" || p.memoryDesign == "dimm" || p.memoryDesign == "rdimm";
    bool ddr = n.find("DDR") != std::string::npos;
    if (ddrDesign && !ddr)
        for (const char* k : {"DQ", "DM", "CK", "ADDR", "BA", "RAS", "CAS", "WE", "ODT", "CKE", "CS"})
            if (n.find(k) != std::string::npos) ddr = true;
    const char* id = "lvcmos33";
    if (ddr) id = minSupply > 0 && minSupply <= 1.25 ? "pod12" : minSupply > 0 && minSupply <= 1.4 ? "sstl135" : "sstl15";
    else if (maxSupply >= 4.5) id = "cmos5";
    else if (maxSupply >= 3.0) id = "lvcmos33";
    else if (maxSupply >= 2.3) id = "lvcmos25";
    else if (maxSupply >= 1.65) id = "lvcmos18";
    else if (maxSupply >= 1.1) id = "lvcmos12";
    return *findLogicFamily(id);
}

DriverModel modelFor(const Project& p, const Component& c, int pin, const std::string& netName) {
    if (const DriverModel* m = assignedModel(p, c, pin, netName)) return *m;
    return defaultModel(p, &c, netName);
}

/// How likely a pin drives its net (0 = never).
int driveScore(const Project& p, const Component& c, int pin) {
    if (const DriverModel* m = assignedModel(p, c, pin, "")) return m->canDrive() ? (m->type == "output" ? 90 : 60) : 0;
    const std::string name = pinNameUpper(c, pin);
    switch (c.kind) {
        case ComponentKind::Custom:
            switch (pinTypeOf(c, pin)) {
                case PinType::Output: return 50;
                case PinType::Bidirectional: return 30;
                case PinType::OpenCollector: return 25;
                case PinType::Passive: return 5;
                case PinType::Input: return 1;
                default: return 0;
            }
        case ComponentKind::OpAmp: return name == "OUT" ? 40 : 0;
        case ComponentKind::IC8: return name == "OUT" || name == "Q" ? 30 : 1;
        case ComponentKind::VoltageSource: {
            auto spec = SourceSpec::parse(c.value);
            return pin == 0 && spec && spec->kind != SourceSpec::Kind::DC ? 40 : 0;
        }
        case ComponentKind::Connector: return 10;
        case ComponentKind::NPN:
        case ComponentKind::NMOS: return pin == 1 ? 20 : 0;
        default: return 0;
    }
}

struct DriverChoice {
    size_t pad = static_cast<size_t>(-1);  // driver pad on this net
    int component = -1, pin = -1;
    int score = 0;
    double seriesR = 0;
    std::string seriesRef;
    int driverNet = -1;  // net the driver pin is on (≠ net through a series resistor)
};

/// First pad of every (component, pin) on the net.
std::vector<size_t> netPads(const std::vector<Pad>& pads, int net) {
    std::vector<size_t> out;
    std::set<std::pair<int, int>> seen;
    for (size_t i = 0; i < pads.size(); ++i)
        if (pads[i].net == net && seen.insert({pads[i].componentId, pads[i].pinIndex}).second) out.push_back(i);
    return out;
}

DriverChoice chooseDriver(const Project& p, const std::vector<Pad>& pads, const std::vector<size_t>& onNet, int net) {
    const Schematic& sch = p.schematic;
    DriverChoice best;
    for (size_t pi : onNet) {
        const Component* c = sch.find(pads[pi].componentId);
        if (!c) continue;
        int sc = driveScore(p, *c, pads[pi].pinIndex);
        if (sc > best.score) {
            best.score = sc;
            best.pad = pi;
            best.component = c->id;
            best.pin = pads[pi].pinIndex;
            best.driverNet = net;
        }
    }
    if (best.score >= 25) return best;
    // Series termination: a resistor ≤ 200 Ω on this net whose other side carries a driver.
    for (size_t pi : onNet) {
        const Component* r = sch.find(pads[pi].componentId);
        if (!r || r->kind != ComponentKind::Resistor) continue;
        auto value = parseEngineeringValue(primaryValue(r->value));
        if (!value || *value > 200) continue;
        const int other = sch.netOf({r->id, pads[pi].pinIndex == 0 ? 1 : 0});
        if (other < 0 || other == net || sch.netRole(other) != NetRole::Signal) continue;
        for (const auto& pin : sch.nets()[static_cast<size_t>(other)].pins) {
            const Component* c = sch.find(pin.component);
            if (!c || c->id == r->id) continue;
            int sc = driveScore(p, *c, pin.pin);
            if (sc >= 25 && sc > best.score) {
                best.score = sc;
                best.pad = pi;
                best.component = c->id;
                best.pin = pin.pin;
                best.seriesR = *value;
                best.seriesRef = r->ref;
                best.driverNet = other;
            }
        }
    }
    if (best.pad == static_cast<size_t>(-1) && !onNet.empty()) {
        best.pad = onNet.front();
        best.component = pads[best.pad].componentId;
        best.pin = pads[best.pad].pinIndex;
        best.driverNet = net;
    }
    return best;
}

/// One-way delays (and lengths) from `start` over the graph edges.
struct PathInfo {
    std::vector<double> delay, length;
};

double edgeDelay(const BoardSettings& s, const NetCopperGraph::Edge& e) {
    if (e.via) {
        const double er = boardLaminate(s).er;
        const double c = viaCapacitance(s.thickness, e.diameter, e.diameter + 2 * s.clearance, er) / s.thickness;
        const double l = viaInductance(s.thickness, e.width) / s.thickness;
        return e.length * std::sqrt(l * c);
    }
    return e.length * propagationDelayPerMm(s, e.layer, e.width);
}

double edgeImpedance(const BoardSettings& s, const NetCopperGraph::Edge& e) {
    if (e.via) {
        const double er = boardLaminate(s).er;
        const double c = viaCapacitance(s.thickness, e.diameter, e.diameter + 2 * s.clearance, er);
        const double l = viaInductance(s.thickness, e.width);
        return c > 0 && l > 0 ? std::sqrt(l / c) : 50;
    }
    return std::max(5.0, trackImpedance(s, e.layer, e.width));
}

PathInfo shortestPaths(const BoardSettings& s, const NetCopperGraph& g, int start) {
    PathInfo out;
    const size_t n = g.nodes.size();
    out.delay.assign(n, std::numeric_limits<double>::infinity());
    out.length.assign(n, 0);
    if (start < 0 || start >= static_cast<int>(n)) return out;
    std::vector<std::vector<std::pair<int, size_t>>> adj(n);
    for (size_t k = 0; k < g.edges.size(); ++k) {
        adj[static_cast<size_t>(g.edges[k].a)].push_back({g.edges[k].b, k});
        adj[static_cast<size_t>(g.edges[k].b)].push_back({g.edges[k].a, k});
    }
    using Item = std::pair<double, int>;
    std::priority_queue<Item, std::vector<Item>, std::greater<Item>> q;
    out.delay[static_cast<size_t>(start)] = 0;
    q.push({0, start});
    while (!q.empty()) {
        auto [d, x] = q.top();
        q.pop();
        if (d > out.delay[static_cast<size_t>(x)]) continue;
        for (const auto& [y, k] : adj[static_cast<size_t>(x)]) {
            const double nd = d + edgeDelay(s, g.edges[k]);
            if (nd < out.delay[static_cast<size_t>(y)]) {
                out.delay[static_cast<size_t>(y)] = nd;
                out.length[static_cast<size_t>(y)] = out.length[static_cast<size_t>(x)] + (g.edges[k].via ? 0 : g.edges[k].length);
                q.push({nd, y});
            }
        }
    }
    return out;
}

/// Star of straight (Manhattan) lines from the driver pad to every other pad: the pre-route estimate.
NetCopperGraph estimatedGraph(const BoardSettings& s, const std::vector<Pad>& pads, const std::vector<size_t>& onNet,
                              size_t driverPad, const std::string& netName) {
    NetCopperGraph g;
    g.nodes.push_back({pads[driverPad].position, -1});
    g.padNode[driverPad] = 0;
    for (size_t pi : onNet) {
        if (pi == driverPad) continue;
        g.nodes.push_back({pads[pi].position, -1});
        const int id = static_cast<int>(g.nodes.size()) - 1;
        g.padNode[pi] = id;
        NetCopperGraph::Edge e;
        e.a = 0;
        e.b = id;
        e.layer = 0;
        e.width = s.widthFor(netName);
        const Vec2 d = pads[pi].position - pads[driverPad].position;
        e.length = std::max(1.0, std::fabs(d.x) + std::fabs(d.y));
        g.edges.push_back(e);
    }
    return g;
}

/// Removes via barrel sections that lead only to unused landings (backdrilled stubs).
void pruneViaStubs(NetCopperGraph& g) {
    for (bool changed = true; changed;) {
        changed = false;
        std::vector<int> degree(g.nodes.size(), 0);
        for (const auto& e : g.edges) {
            ++degree[static_cast<size_t>(e.a)];
            ++degree[static_cast<size_t>(e.b)];
        }
        std::set<int> padNodes;
        for (const auto& [pi, n] : g.padNode) padNodes.insert(n);
        std::vector<NetCopperGraph::Edge> kept;
        for (const auto& e : g.edges) {
            const bool leafA = degree[static_cast<size_t>(e.a)] == 1 && !padNodes.count(e.a);
            const bool leafB = degree[static_cast<size_t>(e.b)] == 1 && !padNodes.count(e.b);
            if (e.via && (leafA || leafB)) {
                changed = true;
                continue;
            }
            kept.push_back(e);
        }
        g.edges = std::move(kept);
    }
}

struct LoadInfo {
    int node = -1;  // graph node of the pad
    size_t pad = 0;
    int component = -1, pin = -1;
    bool receiver = false;  // a logic input (measured)
    DriverModel model;
};

/// The transmission-line network of a net and the nodes measured.
struct NetCircuit {
    TlNetwork tl;
    int driverPad = -1;
    std::vector<int> receiverNodes;  // per measured receiver (TL node)
    std::vector<size_t> receiverIndex;  // into SiNetResult::receivers
    double totalC = 0;
};

double padCapacitance(const BoardSettings& s, const Pad& pad) {
    const double er = boardLaminate(s).er;
    if (pad.throughHole) return viaCapacitance(s.thickness, std::max(pad.size.x, pad.size.y), std::max(pad.size.x, pad.size.y) + 2 * s.clearance, er);
    if (s.layerCount < 2) return 0;
    const double h = std::max(0.05, dielectricBelow(s, pad.smdLayer == 0 ? 0 : std::max(0, s.layerCount - 2)));
    return kEps0 * er * (pad.size.x * pad.size.y * 1e-6) / (h * 1e-3);
}

bool isFastOrModelled(const Project& p, const NetClassification& cls, int net) {
    if (cls.fast.count(net) || cls.rf.count(net)) return true;
    const auto& name = p.schematic.nets()[static_cast<size_t>(net)].name;
    return p.si.netModels.count(name) > 0;
}

SiNetResult analyzeNetImpl(const Project& project, const std::vector<Pad>& pads, const NetClassification& cls, int net,
                           double extraSeriesR, bool simulate, SiNetCircuit* out = nullptr) {
    SiNetResult r;
    r.net = net;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    if (net < 0 || net >= static_cast<int>(sch.nets().size())) {
        r.error = "Unknown net";
        return r;
    }
    r.name = sch.nets()[static_cast<size_t>(net)].name;
    if (sch.netRole(net) != NetRole::Signal) {
        r.error = "Not a signal net (power and ground nets are analysed as power distribution)";
        return r;
    }
    if (pcb.isZoneNet(sch, net)) {
        r.error = "The net is poured as a copper zone";
        return r;
    }
    const std::vector<size_t> onNet = netPads(pads, net);
    if (onNet.size() < 2) {
        r.error = "The net has fewer than two placed pads";
        return r;
    }
    DriverChoice dc = chooseDriver(project, pads, onNet, net);
    const Component* drvComp = sch.find(dc.component);
    if (!drvComp) {
        r.error = "No driver";
        return r;
    }
    r.driverComponent = drvComp->id;
    r.driverRef = drvComp->ref;
    r.driverPin = pinLabel(*drvComp, dc.pin);
    r.driverAssumed = dc.score < 25;
    const std::string driverNetName = sch.nets()[static_cast<size_t>(dc.driverNet)].name;
    r.driver = modelFor(project, *drvComp, dc.pin, r.name);
    if (dc.driverNet != net)
        if (const DriverModel* m = assignedModel(project, *drvComp, dc.pin, driverNetName)) r.driver = *m;
    if (!r.driver.canDrive()) {
        r.notes.push_back(r.driverRef + " pin " + r.driverPin + " has an input model; the logic family default drives instead");
        r.driver = defaultModel(project, drvComp, r.name);
    }
    r.seriesR = dc.seriesR;
    r.seriesRef = dc.seriesRef;
    if (r.driverAssumed)
        r.notes.push_back("No output pin found on the net: " + r.driverRef + " pin " + r.driverPin + " is taken as the driver");

    // Copper.
    NetCopperGraph g = buildNetCopperGraph(pcb, pads, net);
    for (const auto& e : g.edges) r.length += e.via ? 0 : e.length;
    r.routed = std::any_of(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return t.net == net; });
    int dnode = g.padNode.count(dc.pad) ? g.padNode[dc.pad] : -1;
    std::vector<bool> reach = g.reachable(dnode);
    bool anyReached = false;
    for (size_t pi : onNet)
        if (pi != dc.pad && g.padNode.count(pi) && reach[static_cast<size_t>(g.padNode[pi])]) anyReached = true;
    if (!anyReached) {
        g = estimatedGraph(s, pads, onNet, dc.pad, r.name);
        r.estimated = true;
        r.length = 0;
        for (const auto& e : g.edges) r.length += e.length;
        dnode = 0;
        reach = g.reachable(0);
        r.notes.push_back(std::string(r.routed ? "The driver's copper reaches no receiver" : "Not routed") +
                          ": analysed on straight-line lengths on the top layer");
    } else if (s.backdrill && (cls.fast.count(net) || cls.rf.count(net))) {
        pruneViaStubs(g);
        reach = g.reachable(dnode);
    }
    for (const auto& e : g.edges) r.vias += e.via && reach[static_cast<size_t>(e.a)] ? 1 : 0;
    const PathInfo paths = shortestPaths(s, g, dnode);

    // Line sections, grouped by layer and width.
    {
        std::map<std::pair<int, long long>, SiNetResult::Section> sections;
        double zmin = 1e9, zmax = 0;
        for (const auto& e : g.edges) {
            if (e.via || !reach[static_cast<size_t>(e.a)]) continue;
            const double z = edgeImpedance(s, e);
            zmin = std::min(zmin, z);
            zmax = std::max(zmax, z);
            auto& sec = sections[{e.layer, std::llround(e.width * 1000)}];
            sec.layer = copperLayerName(e.layer, s.layerCount);
            sec.width = e.width;
            sec.z0 = z;
            sec.length += e.length;
            sec.delay += edgeDelay(s, e);
        }
        for (auto& [k, sec] : sections) r.sections.push_back(sec);
        r.z0Min = zmax > 0 ? zmin : 0;
        r.z0Max = zmax;
        // Trunk: the first track section leaving the driver.
        for (const auto& e : g.edges)
            if (!e.via && (e.a == dnode || e.b == dnode)) {
                r.z0Trunk = edgeImpedance(s, e);
                break;
            }
        if (r.z0Trunk == 0) r.z0Trunk = r.z0Max > 0 ? r.z0Max : 50;
    }

    // Loads on the net.
    std::vector<LoadInfo> loads;
    NetCircuit circuit;
    TlNetwork& tl = circuit.tl;
    tl = TlNetwork(static_cast<int>(g.nodes.size()));
    for (size_t pi : onNet) {
        if (!g.padNode.count(pi)) continue;
        const int node = g.padNode[pi];
        if (!reach[static_cast<size_t>(node)]) {
            if (pi != dc.pad) {
                SiReceiver rx;
                const Component* c = sch.find(pads[pi].componentId);
                rx.componentId = pads[pi].componentId;
                rx.ref = c ? c->ref : "";
                rx.pin = c ? pinLabel(*c, pads[pi].pinIndex) : "";
                rx.connected = false;
                rx.ok = false;
                if (c && isIcLike(*c)) r.receivers.push_back(rx);
            }
            continue;
        }
        tl.capacitance[static_cast<size_t>(node)] += padCapacitance(s, pads[pi]);
        if (pi == dc.pad) continue;
        const Component* c = sch.find(pads[pi].componentId);
        if (!c) continue;
        const int pin = pads[pi].pinIndex;
        const int other = c->def().pins.size() == 2 ? sch.netOf({c->id, pin == 0 ? 1 : 0}) : -1;
        auto otherRail = [&](double& v) {
            if (other < 0) return false;
            const NetRole role = sch.netRole(other);
            if (role == NetRole::Ground) {
                v = 0;
                return true;
            }
            if (role == NetRole::Power || role == NetRole::NegativeSupply) {
                double rv = railVoltageFromName(sch.nets()[static_cast<size_t>(other)].name);
                v = role == NetRole::NegativeSupply ? -rv : (rv > 0 ? rv : r.driver.vHigh);
                return true;
            }
            return false;
        };
        double rail = 0;
        if (c->kind == ComponentKind::Resistor) {
            auto value = parseEngineeringValue(primaryValue(c->value));
            if (!value || *value <= 0) continue;
            if (otherRail(rail)) {
                tl.conductance[static_cast<size_t>(node)] += 1 / *value;
                tl.railCurrent[static_cast<size_t>(node)] += rail / *value;
                r.terminations.push_back(c->ref + " " + ohms(*value) + " to " + sch.nets()[static_cast<size_t>(other)].name);
            } else if (c->ref != r.seriesRef) {
                r.notes.push_back(c->ref + " (" + ohms(*value) + ") continues the signal to another net, analysed there");
            }
        } else if (c->kind == ComponentKind::Capacitor) {
            auto value = parseEngineeringValue(primaryValue(c->value));
            if (value && *value > 0 && otherRail(rail)) {
                tl.capacitance[static_cast<size_t>(node)] += *value;
                r.terminations.push_back(c->ref + " " + formatEngineeringValue(*value, "F", 3) + " to " +
                                         sch.nets()[static_cast<size_t>(other)].name);
            } else if (value) {
                r.notes.push_back(c->ref + " is a series (AC-coupling) capacitor: the line beyond it is not modelled");
            }
        } else if (c->kind == ComponentKind::Connector) {
            tl.capacitance[static_cast<size_t>(node)] += 1e-12;
            r.notes.push_back(c->ref + ": the off-board load beyond the connector is not modelled (1 pF assumed)");
        } else if (isIcLike(*c)) {
            LoadInfo li;
            li.node = node;
            li.pad = pi;
            li.component = c->id;
            li.pin = pin;
            li.receiver = true;
            li.model = modelFor(project, *c, pin, r.name);
            loads.push_back(li);
        } else {
            tl.capacitance[static_cast<size_t>(node)] += 2e-12;  // discrete semiconductor pin
        }
    }
    if (out) {
        out->graph = g;
        out->reach = reach;
        out->driverNode = dnode;
        out->driverPad = dc.pad;
        out->padC = tl.capacitance;
        out->padG = tl.conductance;
        out->padJ = tl.railCurrent;
    }
    // Lines.
    for (const auto& e : g.edges) {
        if (!reach[static_cast<size_t>(e.a)]) continue;
        tl.addLine(e.a, e.b, edgeImpedance(s, e), edgeDelay(s, e));
    }
    // Driver: die node behind the package.
    const DriverModel& d = r.driver;
    int die = dnode;
    if (d.lPkg > 0 && d.cPkg > 0) {
        die = tl.addNode();
        tl.addLine(die, dnode, std::sqrt(d.lPkg / d.cPkg), std::sqrt(d.lPkg * d.cPkg));
    } else {
        tl.capacitance[static_cast<size_t>(dnode)] += d.cPkg;
    }
    tl.capacitance[static_cast<size_t>(die)] += d.cComp;
    tl.driverNode = die;
    const double extra = extraSeriesR > 0 ? extraSeriesR : 0;
    tl.rSource = d.rOut + d.rPkg + r.seriesR + extra;
    tl.vHigh = d.vHigh;
    tl.riseTime = d.riseTime;
    tl.fallTime = d.fallTime;
    // Receivers.
    for (const auto& li : loads) {
        const DriverModel& m = li.model;
        int rnode = li.node;
        if (m.lPkg > 0 && m.cPkg > 0) {
            rnode = tl.addNode();
            tl.addLine(li.node, rnode, std::sqrt(m.lPkg / m.cPkg), std::sqrt(m.lPkg * m.cPkg));
        } else {
            tl.capacitance[static_cast<size_t>(li.node)] += m.cPkg;
        }
        tl.capacitance[static_cast<size_t>(rnode)] += m.cIn;
        if (m.rTerm > 0) {
            tl.conductance[static_cast<size_t>(rnode)] += 1 / m.rTerm;
            tl.railCurrent[static_cast<size_t>(rnode)] += m.vTerm / m.rTerm;
            r.terminations.push_back("On-die termination " + ohms(m.rTerm) + " at " + sch.find(li.component)->ref);
        }
        SiReceiver rx;
        const Component* c = sch.find(li.component);
        rx.componentId = li.component;
        rx.ref = c ? c->ref : "";
        rx.pin = c ? pinLabel(*c, li.pin) : "";
        rx.model = m.name;
        rx.pathDelay = paths.delay[static_cast<size_t>(li.node)];
        rx.pathLength = paths.length[static_cast<size_t>(li.node)];
        if (!std::isfinite(rx.pathDelay)) rx.pathDelay = 0;
        if (out) {
            SiNetCircuit::Receiver cr;
            cr.node = li.node;
            cr.pad = li.pad;
            cr.componentId = li.component;
            cr.ref = rx.ref;
            cr.pin = rx.pin;
            cr.model = m;
            cr.result = r.receivers.size();
            out->receivers.push_back(cr);
        }
        circuit.receiverNodes.push_back(rnode);
        circuit.receiverIndex.push_back(r.receivers.size());
        r.receivers.push_back(rx);
    }
    // Longest path (to any pad, receivers or terminations).
    for (size_t pi : onNet)
        if (g.padNode.count(pi)) {
            double dl = paths.delay[static_cast<size_t>(g.padNode[pi])];
            if (std::isfinite(dl)) r.maxDelay = std::max(r.maxDelay, dl);
        }
    const double tr = std::max(1e-12, std::min(d.riseTime, d.fallTime));
    {
        double len = 0, delay = 0;
        for (const auto& e : g.edges)
            if (!e.via && reach[static_cast<size_t>(e.a)]) {
                len += e.length;
                delay += edgeDelay(s, e);
            }
        r.criticalLength = criticalLength(tr, len > 0 ? delay / len : propagationDelayPerMm(s, 0, s.trackWidth));
    }
    r.critical = r.maxDelay > tr / 6;
    if (!simulate) return r;
    if (circuit.receiverNodes.empty()) {
        r.notes.push_back("No logic receiver on the net: only the line is reported");
        return r;
    }

    // Time step and span: the edge resolved by ~40 steps; settle over 20 round trips or 6 RC time constants.
    double totalC = 0;
    for (double c : tl.capacitance) totalC += c;
    for (const auto& l : tl.lines) totalC += l.delay / l.z0;
    double dt = std::max(1e-13, tr / 40);
    const double rampR = d.riseTime / 0.8;
    const double half = rampR + std::max({20 * 2 * r.maxDelay, 6 * tl.rSource * totalC, 4 * std::max(d.riseTime, d.fallTime)});
    const double edgeStart = 2 * dt;
    double stop = edgeStart + 2 * half;
    if (stop / dt > 20000) dt = stop / 20000;
    std::vector<int> probes{die};
    for (int n : circuit.receiverNodes) probes.push_back(n);
    TlRun run = simulateTl(tl, probes, dt, edgeStart, edgeStart + half, stop);
    const double limit = project.si.overshootLimit;
    double worstScore = -1;
    for (size_t k = 0; k < circuit.receiverNodes.size(); ++k) {
        SiReceiver& rx = r.receivers[circuit.receiverIndex[k]];
        const DriverModel& m = loads[k].model;
        rx.metrics = measureEdges(run, k + 1, m.vih, m.vil);
        const EdgeMetrics& e = rx.metrics;
        const double swing = std::max(1e-9, e.vHigh - e.vLow);
        rx.ok = e.reachesHigh && e.reachesLow && e.overshoot <= limit * swing && e.undershoot <= limit * swing &&
                e.ringbackHigh >= 0 && e.ringbackLow >= 0;
        const double score = (rx.ok ? 0 : 10) + std::max(e.overshoot, e.undershoot) / swing;
        if (score > worstScore) {
            worstScore = score;
            r.worstReceiver = static_cast<int>(circuit.receiverIndex[k]);
        }
    }
    for (const auto& rx : r.receivers) r.ok = r.ok && (rx.ok || !rx.connected);  // open copper is a routing error
    // Keep the driver and the worst receiver.
    size_t worstProbe = 1;
    for (size_t k = 0; k < circuit.receiverIndex.size(); ++k)
        if (static_cast<int>(circuit.receiverIndex[k]) == r.worstReceiver) worstProbe = k + 1;
    r.run = run;
    r.run.probes = {run.probes[0], run.probes[worstProbe]};

    // Termination advice.
    const bool odt = std::any_of(loads.begin(), loads.end(), [](const LoadInfo& l) { return l.model.rTerm > 0; });
    const double matched = r.z0Trunk - d.rOut - d.rPkg;
    if (!r.ok || r.critical) {
        if (odt) {
            r.recommendation = "Receivers terminate on die; keep the line at " + ohms(r.z0Trunk) + " and check the ODT setting.";
        } else if (r.seriesR > 0) {
            const double ideal = nearestStandardValue(std::max(1.0, matched), ESeries::E24);
            r.recommendation = r.ok ? "Series termination " + r.seriesRef + " (" + ohms(r.seriesR) + ") holds the line."
                                    : "Series termination " + r.seriesRef + " (" + ohms(r.seriesR) + ") does not match the " +
                                          ohms(r.z0Trunk) + " line: use about " + ohms(ideal) + ".";
            if (!r.ok && std::fabs(ideal - r.seriesR) > 1) r.recommendedSeriesR = ideal;
        } else if (matched < 5) {
            r.recommendation = "The driver (" + ohms(d.rOut) + ") already matches the " + ohms(r.z0Trunk) +
                               " line; shorten stubs or terminate the far end (R ≈ " + ohms(r.z0Trunk) + " to VTT).";
        } else {
            r.recommendedSeriesR = nearestStandardValue(matched, ESeries::E24);
            r.recommendation = "Add a " + ohms(r.recommendedSeriesR) + " series resistor at " + r.driverRef + " (Z0 " +
                               ohms(r.z0Trunk) + " − driver " + ohms(d.rOut) + ")";
            if (circuit.receiverNodes.size() > 2)
                r.recommendation += "; with " + std::to_string(circuit.receiverNodes.size()) +
                                    " receivers route a daisy chain and terminate the far end (R ≈ Z0 to VTT) instead.";
            else r.recommendation += ".";
        }
    } else {
        r.recommendation = r.maxDelay > 0 && !r.critical ? "Electrically short (delay below a sixth of the rise time): no termination needed."
                                                         : "";
    }
    if (r.recommendedSeriesR > 0) {
        TlNetwork t2 = tl;
        // The what-if replaces an existing series resistor by the recommended one.
        t2.rSource = d.rOut + d.rPkg + r.recommendedSeriesR + extra;
        TlRun run2 = simulateTl(t2, probes, dt, edgeStart, edgeStart + half, stop);
        const DriverModel& m = loads[worstProbe - 1].model;
        r.terminatedMetrics = measureEdges(run2, worstProbe, m.vih, m.vil);
        r.terminatedRun = run2;
        r.terminatedRun.probes = {run2.probes[0], run2.probes[worstProbe]};
    }
    return r;
}

Json metricsJson(const EdgeMetrics& e) {
    Json j = Json::object();
    j["vLow"] = e.vLow;
    j["vHigh"] = e.vHigh;
    j["overshoot"] = e.overshoot;
    j["undershoot"] = e.undershoot;
    j["ringbackHigh"] = e.ringbackHigh;
    j["ringbackLow"] = e.ringbackLow;
    j["settling"] = e.settling;
    j["settled"] = e.settled;
    j["flightTime"] = e.flightTime;
    j["reachesHigh"] = e.reachesHigh;
    j["reachesLow"] = e.reachesLow;
    const double swing = e.vHigh - e.vLow;
    j["overshootPercent"] = swing > 1e-9 ? 100 * e.overshoot / swing : 0;
    j["undershootPercent"] = swing > 1e-9 ? 100 * e.undershoot / swing : 0;
    return j;
}

/// Min / max-preserving decimation of a run to about `maxPoints` samples per series.
Json waveformJson(const TlRun& run, size_t maxPoints) {
    Json j = Json::object();
    Json time = Json::array(), source = Json::array();
    std::vector<Json> series(run.probes.size(), Json::array());
    const size_t n = run.time.size();
    if (n == 0) {
        j["time"] = time;
        return j;
    }
    const size_t buckets = std::max<size_t>(1, maxPoints / 2);
    const size_t step = std::max<size_t>(1, (n + buckets - 1) / buckets);
    for (size_t b = 0; b < n; b += step) {
        const size_t e = std::min(n, b + step);
        // Two samples per bucket: where the first probe (or the last) is lowest and highest, in time order.
        const auto& ref = run.probes.empty() ? run.source : run.probes.back();
        size_t lo = b, hi = b;
        for (size_t k = b; k < e; ++k) {
            if (ref[k] < ref[lo]) lo = k;
            if (ref[k] > ref[hi]) hi = k;
        }
        std::vector<size_t> pick{std::min(lo, hi)};
        if (lo != hi) pick.push_back(std::max(lo, hi));
        for (size_t k : pick) {
            time.push(run.time[k]);
            source.push(run.source[k]);
            for (size_t p = 0; p < run.probes.size(); ++p) series[p].push(run.probes[p][k]);
        }
    }
    j["time"] = time;
    j["source"] = source;
    if (series.size() >= 1) j["driver"] = series[0];
    if (series.size() >= 2) j["receiver"] = series[1];
    return j;
}

}  // namespace

SiNetResult analyzeNet(const Project& project, int net, double extraSeriesR) {
    const auto pads = project.pcb.pads(project.schematic);
    const NetClassification cls = classifyNets(project);
    return analyzeNetImpl(project, pads, cls, net, extraSeriesR, true);
}

SiNetResult analyzeNetCircuit(const Project& project, int net, SiNetCircuit& out) {
    out = SiNetCircuit();
    const auto pads = project.pcb.pads(project.schematic);
    const NetClassification cls = classifyNets(project);
    return analyzeNetImpl(project, pads, cls, net, -1, false, &out);
}

Json siNetJson(const SiNetResult& r, size_t maxPoints) {
    Json j = Json::object();
    j["net"] = r.net;
    j["name"] = r.name;
    j["error"] = r.error;
    j["routed"] = r.routed;
    j["estimated"] = r.estimated;
    Json drv = driverModelToJson(r.driver);
    drv["ref"] = r.driverRef;
    drv["pin"] = r.driverPin;
    drv["component"] = r.driverComponent;
    drv["assumed"] = r.driverAssumed;
    j["driver"] = drv;
    j["seriesR"] = r.seriesR;
    j["seriesRef"] = r.seriesRef;
    Json terms = Json::array();
    for (const auto& t : r.terminations) terms.push(t);
    j["terminations"] = terms;
    j["length"] = r.length;
    j["delay"] = r.maxDelay;
    j["z0Min"] = r.z0Min;
    j["z0Max"] = r.z0Max;
    j["z0Trunk"] = r.z0Trunk;
    j["criticalLength"] = r.criticalLength;
    j["critical"] = r.critical;
    j["vias"] = r.vias;
    Json secs = Json::array();
    for (const auto& s : r.sections) {
        Json x = Json::object();
        x["layer"] = s.layer;
        x["width"] = s.width;
        x["length"] = s.length;
        x["z0"] = s.z0;
        x["delay"] = s.delay;
        secs.push(x);
    }
    j["sections"] = secs;
    Json rx = Json::array();
    for (const auto& x : r.receivers) {
        Json o = Json::object();
        o["component"] = x.componentId;
        o["ref"] = x.ref;
        o["pin"] = x.pin;
        o["model"] = x.model;
        o["delay"] = x.pathDelay;
        o["length"] = x.pathLength;
        o["connected"] = x.connected;
        o["ok"] = x.ok;
        o["metrics"] = metricsJson(x.metrics);
        rx.push(o);
    }
    j["receivers"] = rx;
    j["worstReceiver"] = r.worstReceiver;
    j["ok"] = r.ok && r.error.empty();
    j["recommendedSeriesR"] = r.recommendedSeriesR;
    j["recommendation"] = r.recommendation;
    j["terminated"] = metricsJson(r.terminatedMetrics);
    j["waveform"] = waveformJson(r.run, maxPoints);
    if (!r.terminatedRun.time.empty()) j["terminatedWaveform"] = waveformJson(r.terminatedRun, maxPoints);
    Json notes = Json::array();
    for (const auto& n : r.notes) notes.push(n);
    j["notes"] = notes;
    return j;
}

Json siNetsJson(const Project& project) {
    const Schematic& sch = project.schematic;
    const auto pads = project.pcb.pads(sch);
    const NetClassification cls = classifyNets(project);
    struct Row {
        Json j;
        int rank;
        double length;
    };
    std::vector<Row> rows;
    for (const auto& n : sch.nets()) {
        if (n.isGround || sch.netRole(n.index) != NetRole::Signal) continue;
        SiNetResult r = analyzeNetImpl(project, pads, cls, n.index, -1, false);
        if (!r.error.empty()) continue;
        Json j = Json::object();
        j["net"] = r.net;
        j["name"] = r.name;
        j["length"] = r.length;
        j["delay"] = r.maxDelay;
        j["critical"] = r.critical;
        j["criticalLength"] = r.criticalLength;
        j["driver"] = r.driverRef + " " + r.driverPin;
        j["model"] = r.driver.name;
        j["modelId"] = r.driver.id;
        const bool fast = cls.fast.count(n.index) > 0 || cls.rf.count(n.index) > 0;
        j["fast"] = fast;
        j["routed"] = r.routed;
        j["receivers"] = static_cast<int>(r.receivers.size());
        rows.push_back({j, (r.critical ? 2 : 0) + (fast ? 1 : 0), r.length});
    }
    std::stable_sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        return a.rank != b.rank ? a.rank > b.rank : a.length > b.length;
    });
    Json out = Json::array();
    for (auto& r : rows) out.push(r.j);
    return out;
}

// ---- crosstalk ----------------------------------------------------------------------------------------------------------

std::vector<CrosstalkPair> crosstalkPairs(const Project& project) {
    std::vector<CrosstalkPair> out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    if (pcb.tracks.empty()) return out;
    const auto pads = pcb.pads(sch);
    const NetClassification cls = classifyNets(project);
    const int netCount = static_cast<int>(sch.nets().size());
    // Aggressors: fast / RF nets, nets with an assigned model, and nets driven by an output pin.
    std::map<int, DriverModel> aggressors;
    std::map<int, double> victimSwing;
    auto signal = [&](int n) { return n >= 0 && n < netCount && sch.netRole(n) == NetRole::Signal && !pcb.isZoneNet(sch, n); };
    for (int n = 0; n < netCount; ++n) {
        if (!signal(n)) continue;
        const std::vector<size_t> onNet = netPads(pads, n);
        if (onNet.size() < 2) continue;
        DriverChoice dc = chooseDriver(project, pads, onNet, n);
        const Component* c = sch.find(dc.component);
        if (!c) continue;
        DriverModel m = modelFor(project, *c, dc.pin, sch.nets()[static_cast<size_t>(n)].name);
        victimSwing[n] = m.vHigh;
        if (dc.score >= 50 || cls.fast.count(n) || cls.rf.count(n) || project.si.netModels.count(sch.nets()[static_cast<size_t>(n)].name))
            aggressors[n] = m;
    }
    std::set<std::pair<int, int>> pairNets;
    for (const auto& [a, b] : cls.diffPairs) {
        pairNets.insert({a, b});
        pairNets.insert({b, a});
    }
    struct Acc {
        double coupled = 0, nextSum = 0, fext = 0, spacing = 1e9, maxKb = 0;
        int layer = 0, layerB = -1;  // layerB: the victim's layer of a broadside pair
        Vec2 at;
        double tdSum = 0;
    };
    std::map<std::pair<int, int>, Acc> acc;
    for (const auto& a : pcb.tracks) {
        auto ait = aggressors.find(a.net);
        if (ait == aggressors.end()) continue;
        const Vec2 da = a.b - a.a;
        const double la = da.length();
        if (la < 0.5) continue;
        const Vec2 u = da * (1.0 / la);
        const double reach = 6 * std::max(0.1, impedanceReferenceHeight(s, a.layer)) + a.width;
        const Rect box = Rect(a.a.x, a.a.y, a.b.x, a.b.y).inflated(reach);
        for (const auto& b : pcb.tracks) {
            // Same layer (edge coupled), or the next layer (broadside: no plane can lie between adjacent layers that both
            // carry tracks here).
            const bool broadside = std::abs(b.layer - a.layer) == 1;
            if ((b.layer != a.layer && !broadside) || b.net == a.net || !signal(b.net) || pairNets.count({a.net, b.net})) continue;
            if (!box.intersects(Rect(b.a.x, b.a.y, b.b.x, b.b.y).inflated(1e-6))) continue;
            const Vec2 db = b.b - b.a;
            const double lb = db.length();
            if (lb < 0.5 || std::fabs(u.dot(db) / lb) < 0.97) continue;
            const double t0 = (b.a - a.a).dot(u), t1 = (b.b - a.a).dot(u);
            const double overlap = std::min(la, std::max(t0, t1)) - std::max(0.0, std::min(t0, t1));
            if (overlap < 0.5) continue;
            const DriverModel& m = ait->second;
            double gap;
            CouplingEstimate e;
            if (broadside) {
                // Lateral offset of the centre lines; coupling within three layer spacings of overlap.
                const double off = (pointSegmentDistance(b.a, a.a, a.b) + pointSegmentDistance(b.b, a.a, a.b)) / 2;
                const double vert = dielectricBelow(s, std::min(a.layer, b.layer)) + copperThickness(s);
                if (off > (a.width + b.width) / 2 + 3 * vert) continue;
                gap = std::max(0.0, off - (a.width + b.width) / 2);
                e = broadsideCoupling(s, a.layer, b.layer, a.width, b.width, off, overlap, m.riseTime);
            } else {
                gap = std::max(0.0, pointSegmentDistance(b.a, a.a, a.b) - (a.width + b.width) / 2);
                if (gap > reach) continue;
                e = crosstalkCoupling(s, a.layer, a.width, b.width, gap, overlap, m.riseTime);
            }
            Acc& x = acc[{a.net, b.net}];
            x.coupled += overlap;
            x.fext += e.fext;
            x.nextSum += e.kb * overlap;
            x.tdSum += overlap * propagationDelayPerMm(s, a.layer, a.width);
            if (gap < x.spacing) {
                x.spacing = gap;
                x.layer = a.layer;
                x.layerB = broadside ? b.layer : -1;
                x.at = (a.a + a.b) * 0.5;
            }
            x.maxKb = std::max(x.maxKb, e.kb);
        }
    }
    for (const auto& [key, x] : acc) {
        const DriverModel& m = aggressors[key.first];
        CrosstalkPair p;
        p.aggressor = key.first;
        p.victim = key.second;
        p.layer = copperLayerName(x.layer, s.layerCount);
        p.broadside = x.layerB >= 0;
        if (p.broadside) p.layer += " / " + copperLayerName(x.layerB, s.layerCount);
        p.coupledLength = x.coupled;
        p.spacing = x.spacing;
        const double kbAvg = x.coupled > 0 ? x.nextSum / x.coupled : 0;
        p.next = kbAvg * std::min(1.0, 2 * x.tdSum / std::max(1e-13, m.riseTime));
        p.fext = std::clamp(x.fext, -0.5, 0.5);
        // The aggressor launches V·Z0/(Z0 + Rs) into the line.
        const double z0 = std::max(5.0, trackImpedance(s, x.layer, s.trackWidth));
        const double launched = m.vHigh * z0 / (z0 + m.rOut);
        p.noise = launched * std::max(std::fabs(p.next), std::fabs(p.fext));
        const double vs = victimSwing.count(p.victim) ? victimSwing[p.victim] : m.vHigh;
        p.limit = project.si.crosstalkLimit * vs;
        p.at = x.at;
        p.ok = p.noise <= p.limit;
        out.push_back(p);
    }
    std::sort(out.begin(), out.end(), [](const CrosstalkPair& a, const CrosstalkPair& b) {
        return a.noise / std::max(1e-12, a.limit) > b.noise / std::max(1e-12, b.limit);
    });
    return out;
}

// ---- return path ---------------------------------------------------------------------------------------------------------

std::vector<ReturnPathIssue> returnPathIssues(const Project& project) {
    std::vector<ReturnPathIssue> out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    if (s.layerCount < 2 || pcb.zones.empty() || pcb.tracks.empty()) return out;
    const auto& fills = pcb.zoneFills(sch);
    const double boardArea = std::max(1.0, s.width * s.height);
    // Plane fills per layer: plane zones, or pours covering a quarter of the board.
    std::map<int, std::vector<size_t>> planes;
    for (size_t k = 0; k < fills.size(); ++k) {
        const auto& f = fills[k];
        const bool plane = f.zone >= 0 && f.zone < static_cast<int>(pcb.zones.size()) && pcb.zones[static_cast<size_t>(f.zone)].plane;
        if (plane || f.area() >= 0.25 * boardArea) planes[f.layer].push_back(k);
    }
    if (planes.empty()) return out;
    const NetClassification cls = classifyNets(project);
    std::set<int> checked(cls.fast.begin(), cls.fast.end());
    checked.insert(cls.rf.begin(), cls.rf.end());
    for (const auto& [name, id] : project.si.netModels)
        for (const auto& n : sch.nets())
            if (n.name == name) checked.insert(n.index);
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(sch.nets().size()) ? sch.nets()[static_cast<size_t>(n)].name : std::string("?"); };
    auto refLayers = [&](int layer) {
        std::vector<int> r;
        for (int l : {layer - 1, layer + 1})
            if (planes.count(l)) r.push_back(l);
        return r;
    };
    // Copper id under a point on a plane layer: fill index × 100000 + island, -1 = none.
    auto copperAt = [&](int layer, Vec2 p) -> long {
        for (size_t k : planes[layer]) {
            int isl = fills[k].islandAt(p);
            if (isl >= 0) return static_cast<long>(k) * 100000 + isl;
        }
        return -1;
    };
    std::vector<Vec2> holes;  // anti-pads of vias and through-hole pads punch small holes in every plane
    for (const auto& v : pcb.vias) holes.push_back(v.position);
    const auto pads = pcb.pads(sch);
    for (const auto& p : pads)
        if (p.throughHole) holes.push_back(p.position);
    const double holeR = s.viaDiameter / 2 + s.clearance + 0.15;
    const double minGap = std::max(1.5, s.viaDiameter + 2 * s.clearance + 0.5);
    std::set<std::pair<int, long long>> reported;
    int count = 0;
    for (const auto& t : pcb.tracks) {
        if (count >= 40) break;
        if (!checked.count(t.net) || sch.netRole(t.net) != NetRole::Signal) continue;
        const double len = (t.b - t.a).length();
        if (len < 0.5) continue;
        for (int ref : refLayers(t.layer)) {
            const int samples = std::max(2, static_cast<int>(std::ceil(len / 0.25)));
            long lastCopper = -2;
            double gapRun = 0;
            bool inGap = false;
            Vec2 gapAt;
            for (int k = 0; k <= samples; ++k) {
                const Vec2 p = t.a + (t.b - t.a) * (static_cast<double>(k) / samples);
                bool nearHole = false;
                for (const Vec2& h : holes)
                    if ((h - p).length() < holeR) {
                        nearHole = true;
                        break;
                    }
                if (nearHole) continue;
                const long c = copperAt(ref, p);
                if (c < 0) {
                    if (!inGap) gapAt = p;
                    inGap = true;
                    gapRun += len / samples;
                    continue;
                }
                const auto key = std::make_pair(t.net, std::llround(p.x) * 100000 + std::llround(p.y));
                if (inGap && lastCopper >= 0 && gapRun >= minGap && !reported.count(key)) {
                    reported.insert(key);
                    ReturnPathIssue is;
                    is.code = "SI_PLANE_GAP";
                    is.net = t.net;
                    is.at = gapAt;
                    is.message = "Net " + netName(t.net) + " on " + copperLayerName(t.layer, s.layerCount) + " crosses a " +
                                 fmt("%.1f mm", gapRun) + " gap in its reference plane on " + copperLayerName(ref, s.layerCount) +
                                 ": the return current detours around the slot (inductance, crosstalk, radiation). Route "
                                 "around the gap or bridge it with a stitching capacitor.";
                    out.push_back(is);
                    ++count;
                } else if (lastCopper >= 0 && c != lastCopper && !inGap && !reported.count(key)) {
                    reported.insert(key);
                    const int na = fills[static_cast<size_t>(lastCopper / 100000)].net, nb = fills[static_cast<size_t>(c / 100000)].net;
                    ReturnPathIssue is;
                    is.code = "SI_PLANE_SPLIT";
                    is.net = t.net;
                    is.at = p;
                    is.message = "Net " + netName(t.net) + " on " + copperLayerName(t.layer, s.layerCount) +
                                 " crosses a split in its reference plane on " + copperLayerName(ref, s.layerCount) + " (" +
                                 netName(na) + (na == nb ? " islands" : " / " + netName(nb)) +
                                 "): the return path is broken. Keep fast signals over one continuous plane.";
                    out.push_back(is);
                    ++count;
                }
                if (inGap) {
                    inGap = false;
                    gapRun = 0;
                }
                lastCopper = c;
            }
        }
    }
    // Layer changes: the reference plane changes too; its return current needs a nearby path.
    for (const auto& v : pcb.vias) {
        if (count >= 60) break;
        if (!checked.count(v.net) || sch.netRole(v.net) != NetRole::Signal) continue;
        std::set<int> used;
        for (const auto& t : pcb.tracks)
            if (t.net == v.net && ((t.a - v.position).length() < v.diameter / 2 + 1e-3 || (t.b - v.position).length() < v.diameter / 2 + 1e-3))
                used.insert(t.layer);
        if (used.size() < 2) continue;
        // Reference nets and plane layers of the first and last layer.
        const int la = *used.begin(), lb = *used.rbegin();
        auto refs = [&](int layer) {
            std::set<std::pair<int, int>> r;  // (plane layer, net)
            for (int pl : refLayers(layer)) {
                long c = copperAt(pl, v.position + Vec2(holeR + 0.2, 0));
                if (c < 0) c = copperAt(pl, v.position - Vec2(holeR + 0.2, 0));
                if (c >= 0) r.insert({pl, fills[static_cast<size_t>(c / 100000)].net});
            }
            return r;
        };
        const auto ra = refs(la), rb = refs(lb);
        if (ra.empty() || rb.empty() || ra == rb) continue;
        bool shared = false;
        for (const auto& x : ra)
            if (rb.count(x)) shared = true;
        if (shared) continue;
        std::set<int> netsA, netsB;
        for (const auto& x : ra) netsA.insert(x.second);
        for (const auto& x : rb) netsB.insert(x.second);
        bool sameNet = false;
        int refNet = -1;
        for (int n : netsA)
            if (netsB.count(n)) {
                sameNet = true;
                refNet = n;
            }
        bool stitched = false;
        constexpr double kStitch = 2.0;
        if (sameNet) {
            for (const auto& o : pcb.vias)
                if (o.net == refNet && (o.position - v.position).length() <= kStitch) stitched = true;
            for (const auto& p : pads)
                if (p.net == refNet && p.throughHole && (p.position - v.position).length() <= kStitch) stitched = true;
        } else {
            // A capacitor between the two reference nets close by.
            for (const auto& c : sch.components()) {
                if (c.kind != ComponentKind::Capacitor || !c.pcb.placed) continue;
                const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
                const bool joins = (netsA.count(n0) && netsB.count(n1)) || (netsA.count(n1) && netsB.count(n0));
                if (joins && (c.pcb.position - v.position).length() <= 3 * kStitch) stitched = true;
            }
        }
        if (stitched) continue;
        ReturnPathIssue is;
        is.code = "SI_REFERENCE_CHANGE";
        is.net = v.net;
        is.at = v.position;
        is.message = "Net " + netName(v.net) + " changes from " + copperLayerName(la, s.layerCount) + " to " +
                     copperLayerName(lb, s.layerCount) + " and its reference plane changes with it" +
                     (sameNet ? " (" + netName(refNet) + " on another layer) with no " + netName(refNet) +
                                    " stitching via within 2 mm: place one beside the signal via."
                              : " to a different net with no stitching capacitor nearby: add a 100 nF capacitor between the "
                                "two planes next to the via, or keep the signal on layers referenced to the same plane.");
        out.push_back(is);
        ++count;
    }
    return out;
}

Json crosstalkJson(const Project& project) {
    const Schematic& sch = project.schematic;
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(sch.nets().size()) ? sch.nets()[static_cast<size_t>(n)].name : std::string(); };
    Json root = Json::object();
    Json pairs = Json::array();
    for (const auto& p : crosstalkPairs(project)) {
        Json j = Json::object();
        j["aggressor"] = netName(p.aggressor);
        j["victim"] = netName(p.victim);
        j["layer"] = p.layer;
        j["coupledLength"] = p.coupledLength;
        j["spacing"] = p.spacing;
        j["next"] = p.next;
        j["fext"] = p.fext;
        j["noise"] = p.noise;
        j["limit"] = p.limit;
        j["ok"] = p.ok;
        j["broadside"] = p.broadside;
        j["x"] = p.at.x;
        j["y"] = p.at.y;
        pairs.push(j);
    }
    root["pairs"] = pairs;
    Json rp = Json::array();
    for (const auto& r : returnPathIssues(project)) {
        Json j = Json::object();
        j["code"] = r.code;
        j["net"] = netName(r.net);
        j["message"] = r.message;
        j["x"] = r.at.x;
        j["y"] = r.at.y;
        rp.push(j);
    }
    root["returnPath"] = rp;
    root["limit"] = project.si.crosstalkLimit;
    return root;
}

// ---- checks ----------------------------------------------------------------------------------------------------------------

std::vector<RuleViolation> signalPowerIntegrityChecks(const Project& project) {
    std::vector<RuleViolation> out;
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    auto add = [&](Severity sev, const std::string& code, const std::string& msg, std::vector<int> comps = {}, Vec2 at = {},
                   bool hasAt = false) {
        RuleViolation v;
        v.severity = sev;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        v.location = at;
        v.hasLocation = hasAt;
        out.push_back(std::move(v));
    };
    bool placed = false;
    for (const auto& c : sch.components()) placed |= c.hasFootprint() && c.pcb.placed;
    if (!placed) return out;
    const auto pads = pcb.pads(sch);
    const NetClassification cls = classifyNets(project);
    // Nets with a real driver, fast nets and nets with an assigned model; fast and long first, at most 120.
    struct Cand {
        int net;
        int rank;
        double length;
    };
    std::vector<Cand> cands;
    for (const auto& n : sch.nets()) {
        if (n.isGround || sch.netRole(n.index) != NetRole::Signal || pcb.isZoneNet(sch, n.index)) continue;
        const auto onNet = netPads(pads, n.index);
        if (onNet.size() < 2) continue;
        DriverChoice dc = chooseDriver(project, pads, onNet, n.index);
        const bool modelled = isFastOrModelled(project, cls, n.index);
        if (dc.score < 25 && !modelled) continue;
        cands.push_back({n.index, modelled ? 1 : 0, routedNetLength(pcb, n.index)});
    }
    std::stable_sort(cands.begin(), cands.end(), [](const Cand& a, const Cand& b) {
        return a.rank != b.rank ? a.rank > b.rank : a.length > b.length;
    });
    if (cands.size() > 120) cands.resize(120);
    int warnings = 0;
    for (const auto& c : cands) {
        SiNetResult r = analyzeNetImpl(project, pads, cls, c.net, -1, true);
        if (!r.error.empty()) continue;
        Vec2 at;
        bool hasAt = false;
        for (const auto& p : pads)
            if (p.componentId == r.driverComponent && (!hasAt || p.net == c.net)) {
                at = p.position;
                hasAt = true;
            }
        const std::string advice = r.recommendation.empty() ? "" : " " + r.recommendation;
        if (!r.ok && r.worstReceiver >= 0 && warnings < 40) {
            const SiReceiver& rx = r.receivers[static_cast<size_t>(r.worstReceiver)];
            const EdgeMetrics& e = rx.metrics;
            const double swing = std::max(1e-9, e.vHigh - e.vLow);
            std::string what, code;
            if (!e.reachesHigh || !e.reachesLow) {
                code = "SI_THRESHOLD";
                what = "the signal at " + rx.ref + " never crosses " + (!e.reachesHigh ? "VIH" : "VIL") + " (levels " +
                       volts(e.vLow) + " / " + volts(e.vHigh) + ")";
            } else if (e.ringbackHigh < 0 || e.ringbackLow < 0) {
                code = "SI_RINGBACK";
                what = "ringing at " + rx.ref + " re-crosses the input threshold by " +
                       volts(-std::min(e.ringbackHigh, e.ringbackLow)) + " (false edges)";
            } else {
                code = "SI_OVERSHOOT";
                what = "overshoot " + fmt("%.0f %%", 100 * e.overshoot / swing) + " / undershoot " +
                       fmt("%.0f %%", 100 * e.undershoot / swing) + " of the swing at " + rx.ref + " (limit " +
                       fmt("%.0f %%", 100 * project.si.overshootLimit) + ")";
            }
            add(Severity::Warning, code,
                "Net " + r.name + " (" + r.driver.name + ", " + secs(r.driver.riseTime) + " edge, " + fmt("%.1f mm", r.length) +
                    (r.estimated ? " estimated" : "") + "): " + what + "." + advice,
                {r.driverComponent}, at, hasAt);
            ++warnings;
        } else if (r.critical && r.ok && r.recommendedSeriesR > 0) {
            add(Severity::Info, "SI_LONG_LINE",
                "Net " + r.name + " is longer than its critical length (" + fmt("%.0f mm", r.criticalLength) + " for a " +
                    secs(r.driver.riseTime) + " edge) but rings within limits." + advice,
                {r.driverComponent}, at, hasAt);
        }
    }
    int xt = 0;
    for (const auto& p : crosstalkPairs(project)) {
        if (p.ok || xt >= 25) continue;
        const auto& nets = sch.nets();
        add(Severity::Warning, "SI_CROSSTALK",
            "Crosstalk from " + nets[static_cast<size_t>(p.aggressor)].name + " into " + nets[static_cast<size_t>(p.victim)].name +
                ": " + fmt("%.1f mm", p.coupledLength) + " coupled on " + p.layer + " at " + fmt("%.2f mm", p.spacing) +
                " spacing, NEXT " + fmt("%.1f %%", 100 * p.next) + ", FEXT " + fmt("%.1f %%", 100 * p.fext) + " → " +
                volts(p.noise) + " noise (limit " + volts(p.limit) + "). Increase the spacing (≥ 3× the dielectric height) "
                "or shorten the parallel run.",
            {}, p.at, true);
        ++xt;
    }
    for (const auto& r : returnPathIssues(project)) add(Severity::Warning, r.code, r.message, {}, r.at, true);
    // Power integrity.
    for (const auto& rail : analyzePdn(project)) {
        if (rail.decaps.empty() && !rail.loads.empty()) {
            add(Severity::Warning, "PI_NO_DECOUPLING",
                "Rail " + rail.name + " feeds " + std::to_string(rail.loads.size()) + " load pin(s) but has no decoupling "
                "capacitor: place a 100 nF capacitor at every supply pin and bulk capacitance at the regulator.");
        } else if (!rail.compliant) {
            std::string msg = "Rail " + rail.name + " PDN impedance reaches " + ohms(rail.worstZ) + " at " +
                              formatEngineeringValue(rail.worstF, "Hz", 3) + ", above the " + ohms(rail.target) + " target (" +
                              volts(rail.voltage) + " × " + fmt("%.1f %%", rail.ripplePercent) + " / " +
                              formatEngineeringValue(rail.transientCurrent, "A", 3) + ").";
            if (!rail.recommendations.empty()) msg += " " + rail.recommendations.front();
            add(Severity::Warning, "PI_TARGET_IMPEDANCE", msg);
        }
        if (rail.irAnalyzed && rail.voltage > 0 && rail.irWorst > rail.irLimitPercent / 100 * rail.voltage)
            add(Severity::Warning, "PI_IR_DROP",
                "Rail " + rail.name + " drops " + formatEngineeringValue(rail.irWorst, "V", 3) + " (" +
                    fmt("%.1f %%", 100 * rail.irWorst / rail.voltage) + ") from " + rail.irSource + " to " + rail.irWorstRef +
                    " at " + formatEngineeringValue(rail.dcCurrent, "A", 3) + ": widen the supply track, add vias or pour the "
                    "rail as a plane (limit " + fmt("%.1f %%", rail.irLimitPercent) + ").");
    }
    // Serial channels given a bit rate: the eye at the receiver against its mask.
    for (const auto& spec : project.si.channels) {
        const ChannelCheck c = checkChannel(project, spec);
        if (!c.ok) add(Severity::Warning, "SI_EYE_MASK", c.message);
    }
    return out;
}

ChannelCheck checkChannel(const Project& project, const SiSettings::ChannelSpec& spec) {
    ChannelCheck c;
    ChannelOptions co;
    co.net = spec.net;
    const ChannelModel m = extractChannel(project, co);
    if (!m.error.empty()) {
        c.ok = false;
        c.message = "Channel " + spec.net + " cannot be analysed: " + m.error;
        return c;
    }
    // An imported IBIS driver drives the channel; a logic-family default (not a SerDes model) is replaced by an ideal
    // 50 Ω source and termination with a 1 V swing.
    ChannelDrive drive;
    drive.idealDriver = m.driver.source != "ibis";
    EyeOptions eo;
    eo.bitRate = spec.bitRate;
    eo.maskHeight = spec.maskHeight;
    eo.maskWidthUi = spec.maskWidthUi;
    eo.prbs = 7;
    eo.riseTime = drive.idealDriver ? 0.25 / spec.bitRate : m.driver.riseTime;
    const double swing = drive.idealDriver ? drive.swing : m.driver.vHigh;
    const double vMid = channelDcLevel(m, swing / 2, swing / 2, drive);
    const EyeResult e = simulateEye([&](const std::vector<double>& f) { return channelTransfer(m, f, co.refOhms, drive); },
                                    swing / 2, vMid, std::max(m.delayP, m.delayN), eo);
    c.eyeHeight = e.eyeHeight;
    c.eyeWidth = e.eyeWidth;
    c.maskMargin = e.maskMargin;
    const bool masked = spec.maskHeight > 0 && spec.maskWidthUi > 0;
    c.ok = e.error.empty() && e.open && (!masked || e.maskPass);
    const std::string name = spec.net + (m.differential ? " / " + m.netN : "");
    c.message = "Channel " + name + " at " + formatEngineeringValue(spec.bitRate, "b/s", 3) + " (" +
                (drive.idealDriver ? std::string("ideal 50 Ω driver") : m.driver.name) + "): eye height " +
                formatEngineeringValue(e.eyeHeight, "V", 3) + ", width " + formatEngineeringValue(e.eyeWidth, "s", 3);
    if (masked)
        c.message += ", mask " + formatEngineeringValue(spec.maskHeight, "V", 3) + " × " + fmt("%.2f UI", spec.maskWidthUi) +
                     (e.maskPass ? " passes" : " violated by " + formatEngineeringValue(-e.maskMargin, "V", 3));
    if (!e.open) c.message += " — the eye is closed: shorten the channel, use a lower-loss laminate or add equalisation";
    else if (!c.ok) c.message += ": add equalisation (CTLE / FFE) or reduce loss and reflections";
    c.message += ".";
    return c;
}

}  // namespace sieda
