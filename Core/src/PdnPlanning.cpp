#include "sieda/PdnPlanning.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>

#include "sieda/LossyLine.hpp"
#include "sieda/Project.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Units.hpp"

namespace sieda {

namespace {

double sincX(double x) { return std::fabs(x) < 1e-12 ? 1.0 : std::sin(x) / x; }

/// Modal coefficients of every port and the mode wavenumbers, for one cavity.
struct CavityModel {
    double a = 0, b = 0, d = 0, er = 1, tanD = 0;  // m
    double copperT = 35e-6;                        // plate thickness, m
    int modes = 30;
    std::vector<double> kmn2;              // dynamic modes (m, n ≤ modes), (0,0) first
    std::vector<std::vector<double>> phi;  // [port][mode]
    CMat residual;                         // static inductance of the higher modes (H)

    CavityModel(double aMm, double bMm, double dMm, double erIn, double tanDIn, const std::vector<CavityPort>& ports, int m,
                double tMm)
        : a(aMm * 1e-3), b(bMm * 1e-3), d(dMm * 1e-3), er(erIn), tanD(tanDIn), modes(std::clamp(m, 1, 200)),
          residual(static_cast<int>(ports.size())) {
        copperT = std::max(1e-6, tMm * 1e-3);
        const size_t P = ports.size();
        phi.assign(P, {});
        auto coeff = [&](const CavityPort& p, int mm, int nn) {
            const double km = mm * kPi / a, kn = nn * kPi / b, w = p.width * 1e-3;
            const double chi = (mm ? std::sqrt(2.0) : 1.0) * (nn ? std::sqrt(2.0) : 1.0);
            return chi * std::cos(km * p.x * 1e-3) * std::cos(kn * p.y * 1e-3) * sincX(km * w / 2) * sincX(kn * w / 2);
        };
        for (int mm = 0; mm <= modes; ++mm)
            for (int nn = 0; nn <= modes; ++nn) {
                kmn2.push_back(std::pow(mm * kPi / a, 2) + std::pow(nn * kPi / b, 2));
                for (size_t i = 0; i < P; ++i) phi[i].push_back(coeff(ports[i], mm, nn));
            }
        // Static sum of the modes beyond the dynamic set: jω · μ d/(ab) Σ φiφj / k_mn².
        const int big = 6 * modes;
        std::vector<double> c(P);
        for (int mm = 0; mm <= big; ++mm)
            for (int nn = 0; nn <= big; ++nn) {
                if (mm <= modes && nn <= modes) continue;
                const double k2 = std::pow(mm * kPi / a, 2) + std::pow(nn * kPi / b, 2);
                for (size_t i = 0; i < P; ++i) c[i] = coeff(ports[i], mm, nn);
                for (size_t i = 0; i < P; ++i)
                    for (size_t j = i; j < P; ++j) {
                        const double v = kVacuumPermeability * d / (a * b) * c[i] * c[j] / k2;
                        residual(static_cast<int>(i), static_cast<int>(j)) += v;
                        if (i != j) residual(static_cast<int>(j), static_cast<int>(i)) += v;
                    }
            }
    }

    CMat at(double f) const {
        const int P = static_cast<int>(phi.size());
        CMat z(P);
        const double w = 2 * kPi * std::max(1.0, f);
        const cplx pre = cplx(0, w * kVacuumPermeability * d / (a * b));
        std::vector<cplx> g(kmn2.size());
        // Dielectric loss on every mode; conductor loss (two plates, surface resistance of the skin depth or of the
        // copper when thinner) on the propagating modes: k² = ω²με(1 − j(tan δ + 2·R_s/(ωμd))). The (0,0) mode is the
        // plate capacitor, whose spreading current is in the higher modes.
        const double rs = kCopperResistivity / std::max(1e-9, std::min(skinDepth(f), copperT));
        const double condLoss = 2 * rs / (w * kVacuumPermeability * std::max(1e-9, d));
        const double base = w * w * kVacuumPermeability * kVacuumPermittivity * er;
        const cplx k2Static = base * cplx(1, -tanD), k2Modes = base * cplx(1, -(tanD + condLoss));
        for (size_t q = 0; q < kmn2.size(); ++q) g[q] = pre / (kmn2[q] - (q == 0 ? k2Static : k2Modes));
        for (int i = 0; i < P; ++i)
            for (int j = i; j < P; ++j) {
                cplx s = 0;
                const auto& pi = phi[static_cast<size_t>(i)];
                const auto& pj = phi[static_cast<size_t>(j)];
                for (size_t q = 0; q < g.size(); ++q) s += pi[q] * pj[q] * g[q];
                s += cplx(0, w) * residual(i, j);
                z(i, j) = s;
                z(j, i) = s;
            }
        return z;
    }
};

const PdnRailResult* findRail(const std::vector<PdnRailResult>& rails, const std::string& net) {
    for (const auto& r : rails)
        if (r.name == net) return &r;
    return nullptr;
}

/// Lumped |Z(f)| of a rail (as in analyzePdn) with extra capacitors (c, esr, esl+mounting).
double lumpedZ(const PdnRailResult& r, double f, const std::vector<PdnDecap>& extra) {
    std::complex<double> y = 1.0 / std::complex<double>(r.vrmR, 2 * kPi * f * r.vrmL);
    for (const auto& d : r.decaps) y += 1.0 / capacitorImpedance(f, d.c, d.esr, d.esl + d.mounting);
    for (const auto& d : extra) y += 1.0 / capacitorImpedance(f, d.c, d.esr, d.esl + d.mounting);
    if (r.planeC > 0) y += 1.0 / capacitorImpedance(f, r.planeC, 1e-3, r.planeL);
    return std::abs(1.0 / y);
}

Json nums(const std::vector<double>& v) {
    Json a = Json::array();
    for (double x : v) a.push(std::isfinite(x) ? x : 0.0);
    return a;
}

}  // namespace

CMat cavityImpedance(double aMm, double bMm, double dMm, double er, double tanD, const std::vector<CavityPort>& ports, double f,
                     int modes, double copperMm) {
    return CavityModel(aMm, bMm, dMm, er, tanD, ports, modes, copperMm).at(f);
}

double cavityModeFrequency(double aMm, double bMm, double er, int m, int n) {
    return kSpeedOfLight / (2 * std::sqrt(er)) * std::sqrt(std::pow(m / (aMm * 1e-3), 2) + std::pow(n / (bMm * 1e-3), 2));
}

PdnCavityResult pdnCavity(const Project& project, const PdnRailResult& rail) {
    PdnCavityResult out;
    const BoardSettings& s = project.pcb.settings;
    if (!(rail.planeC > 0) || rail.planeX1 <= rail.planeX0 || rail.planeY1 <= rail.planeY0) {
        out.note = "No plane pair: pour the rail next to a ground plane for a cavity model.";
        return out;
    }
    out.available = true;
    out.a = rail.planeX1 - rail.planeX0;
    out.b = rail.planeY1 - rail.planeY0;
    out.d = rail.planeGap;
    out.er = boardLaminate(s).er;
    out.x0 = rail.planeX0;
    out.y0 = rail.planeY0;
    const double tanD = boardLaminate(s).lossTangent;
    auto clampIn = [&](Vec2 p) {
        return Vec2{std::clamp(p.x - out.x0, 0.0, out.a), std::clamp(p.y - out.y0, 0.0, out.b)};
    };
    // Observation: the centre of the load pins.
    Vec2 c{0, 0};
    int nl = 0;
    for (const auto& l : rail.loads) {
        c = c + l.position;
        ++nl;
    }
    out.observe = nl ? c * (1.0 / nl) : Vec2{out.x0 + out.a / 2, out.y0 + out.b / 2};
    std::vector<CavityPort> ports;
    const Vec2 o = clampIn(out.observe);
    ports.push_back({o.x, o.y, 0.5});
    // Decoupling capacitors, nearest first (at most 63), then the regulator.
    std::vector<const PdnDecap*> caps;
    for (const auto& d : rail.decaps) caps.push_back(&d);
    std::sort(caps.begin(), caps.end(), [&](const PdnDecap* x, const PdnDecap* y) {
        return (x->position - out.observe).length() < (y->position - out.observe).length();
    });
    if (caps.size() > 63) {
        out.note = "The 63 capacitors nearest the loads are modelled.";
        caps.resize(63);
    }
    for (const PdnDecap* d : caps) {
        const Vec2 p = clampIn(d->position);
        ports.push_back({p.x, p.y, 0.5});
    }
    const bool vrm = rail.hasVrmPosition && rail.vrmR > 0;
    if (vrm) {
        const Vec2 p = clampIn(rail.vrmPosition);
        ports.push_back({p.x, p.y, 1.0});
    }
    out.ports = static_cast<int>(ports.size());
    // Dynamic modes up to about twice the top frequency.
    const double fTop = 3e9;
    const double kTop = 2 * 2 * kPi * fTop * std::sqrt(out.er) / kSpeedOfLight;
    const int modes = std::clamp(static_cast<int>(std::ceil(kTop * std::max(out.a, out.b) * 1e-3 / kPi)), 4, 60);
    const CavityModel model(out.a, out.b, out.d, out.er, tanD, ports, modes, copperThickness(s));
    for (int k = 0; k <= 120; ++k) {
        const double f = 1e6 * std::pow(fTop / 1e6, k / 120.0);
        const CMat z = model.at(f);
        const int P = z.n;
        cplx zin = z(0, 0);
        if (P > 1) {
            // Ports 1… terminated by their element: Z_in = Z00 − z0D (Z_DD + Z_elem)⁻¹ zD0.
            const int D = P - 1;
            CMat m(D);
            for (int i = 0; i < D; ++i)
                for (int j = 0; j < D; ++j) m(i, j) = z(i + 1, j + 1);
            for (int i = 0; i < D; ++i) {
                cplx ze;
                if (static_cast<size_t>(i) < caps.size()) {
                    const PdnDecap* d = caps[static_cast<size_t>(i)];
                    ze = capacitorImpedance(f, d->c, d->esr, d->esl + d->mounting);
                } else {
                    ze = cplx(rail.vrmR, 2 * kPi * f * rail.vrmL);
                }
                m(i, i) += ze;
            }
            if (invertInPlace(m)) {
                cplx corr = 0;
                for (int i = 0; i < D; ++i)
                    for (int j = 0; j < D; ++j) corr += z(0, i + 1) * m(i, j) * z(j + 1, 0);
                zin -= corr;
            }
        }
        out.freq.push_back(f);
        out.zCavity.push_back(std::abs(zin));
        out.zLumped.push_back(lumpedZ(rail, f, {}));
        if (f <= 1e9 && std::isfinite(rail.target) && rail.target > 0) {
            const double ratio = std::abs(zin) / rail.target;
            if (ratio > out.worstRatio) {
                out.worstRatio = ratio;
                out.worstF = f;
            }
        }
    }
    // First resonances.
    for (int m = 0; m <= 6; ++m)
        for (int n = 0; n <= 6; ++n)
            if (m || n) out.modes.push_back({m, n, cavityModeFrequency(out.a, out.b, out.er, m, n)});
    std::sort(out.modes.begin(), out.modes.end(), [](const auto& x, const auto& y) { return x.f < y.f; });
    if (out.modes.size() > 8) out.modes.resize(8);
    if (out.worstRatio > 1 && !out.modes.empty()) {
        out.recommendations.push_back("The plane pair resonates near " + formatEngineeringValue(out.modes.front().f, "Hz", 3) +
                                      " (λ/2 across " + formatEngineeringValue(std::max(out.a, out.b) * 1e-3, "m", 3) +
                                      "): spread small capacitors over the plane and near its edges, and keep the plane "
                                      "spacing thin (" + formatEngineeringValue(out.d * 1e-3, "m", 2) + " now).");
    }
    return out;
}

PdnDecapPlan pdnDecapPlan(const PdnRailResult& rail) {
    PdnDecapPlan plan;
    if (!std::isfinite(rail.target) || !(rail.target > 0)) return plan;
    for (int k = 0; k <= 120; ++k) plan.freq.push_back(1e3 * std::pow(std::max(1e4, rail.fMax) / 1e3, k / 120.0));
    auto worst = [&](const std::vector<PdnDecap>& extra) {
        double w = 0;
        for (double f : plan.freq) w = std::max(w, lumpedZ(rail, f, extra) / rail.target);
        return w;
    };
    // Mounting of an added capacitor: the rail's median, else a capacitor with vias to the planes (≈ 0.5 nH).
    std::vector<double> mounts;
    for (const auto& d : rail.decaps) mounts.push_back(d.mounting);
    std::sort(mounts.begin(), mounts.end());
    plan.mounting = mounts.empty() ? 0.5e-9 : mounts[mounts.size() / 2];
    std::vector<PdnDecap> extra;
    plan.worstBefore = worst(extra);
    for (double f : plan.freq) plan.zBefore.push_back(lumpedZ(rail, f, extra));
    plan.needed = plan.worstBefore > 1;
    struct Cand {
        const char* value;
        const char* fp;
        double c;
    };
    static const Cand kCands[] = {{"1n", "C_0201", 1e-9},    {"10n", "C_0402", 10e-9}, {"100n", "C_0402", 100e-9},
                                  {"1u", "C_0402", 1e-6},    {"4.7u", "C_0603", 4.7e-6}, {"10u", "C_0805", 10e-6},
                                  {"22u", "C_0805", 22e-6},  {"47u", "C_1206", 47e-6}};
    double current = plan.worstBefore;
    for (int step = 0; step < 40 && current > 1; ++step) {
        int best = -1;
        double bestW = current;
        for (int i = 0; i < static_cast<int>(sizeof kCands / sizeof kCands[0]); ++i) {
            PdnDecap d;
            d.c = kCands[i].c;
            const CapacitorParasitics par = capacitorParasitics(kCands[i].fp, d.c);
            d.esr = par.esr;
            d.esl = par.esl;
            d.mounting = plan.mounting;
            extra.push_back(d);
            const double w = worst(extra);
            extra.pop_back();
            if (w < bestW - 1e-9) {
                bestW = w;
                best = i;
            }
        }
        if (best < 0) break;
        PdnDecap d;
        d.c = kCands[best].c;
        const CapacitorParasitics par = capacitorParasitics(kCands[best].fp, d.c);
        d.esr = par.esr;
        d.esl = par.esl;
        d.mounting = plan.mounting;
        extra.push_back(d);
        current = bestW;
        auto it = std::find_if(plan.additions.begin(), plan.additions.end(),
                               [&](const PdnDecapPlan::Add& a) { return a.value == kCands[best].value && a.footprint == kCands[best].fp; });
        if (it != plan.additions.end()) ++it->count;
        else plan.additions.push_back({kCands[best].value, kCands[best].fp, kCands[best].c, 1});
    }
    plan.worstAfter = current;
    plan.compliant = current <= 1;
    for (double f : plan.freq) plan.zAfter.push_back(lumpedZ(rail, f, extra));
    std::sort(plan.additions.begin(), plan.additions.end(), [](const auto& x, const auto& y) { return x.c > y.c; });
    return plan;
}

Json pdnCavityJson(const Project& project, const std::string& net) {
    Json j = Json::object();
    const auto rails = analyzePdn(project);
    const PdnRailResult* r = findRail(rails, net);
    j["rail"] = net;
    if (!r) {
        j["error"] = "Unknown rail " + net;
        return j;
    }
    const PdnCavityResult c = pdnCavity(project, *r);
    j["available"] = c.available;
    j["note"] = c.note;
    j["a"] = c.a;
    j["b"] = c.b;
    j["d"] = c.d;
    j["er"] = c.er;
    j["x0"] = c.x0;
    j["y0"] = c.y0;
    j["ports"] = c.ports;
    Json o = Json::object();
    o["x"] = c.observe.x;
    o["y"] = c.observe.y;
    j["observe"] = o;
    Json modes = Json::array();
    for (const auto& m : c.modes) {
        Json x = Json::object();
        x["m"] = m.m;
        x["n"] = m.n;
        x["f"] = m.f;
        modes.push(x);
    }
    j["modes"] = modes;
    j["freq"] = nums(c.freq);
    j["zCavity"] = nums(c.zCavity);
    j["zLumped"] = nums(c.zLumped);
    j["target"] = std::isfinite(r->target) ? r->target : 0.0;
    j["worstRatio"] = c.worstRatio;
    j["worstF"] = c.worstF;
    Json recs = Json::array();
    for (const auto& t : c.recommendations) recs.push(t);
    j["recommendations"] = recs;
    return j;
}

Json pdnDecapPlanJson(const Project& project, const std::string& net) {
    Json j = Json::object();
    const auto rails = analyzePdn(project);
    const PdnRailResult* r = findRail(rails, net);
    j["rail"] = net;
    if (!r) {
        j["error"] = "Unknown rail " + net;
        return j;
    }
    const PdnDecapPlan p = pdnDecapPlan(*r);
    j["needed"] = p.needed;
    j["compliant"] = p.compliant;
    j["worstBefore"] = p.worstBefore;
    j["worstAfter"] = p.worstAfter;
    j["mounting"] = p.mounting;
    j["target"] = std::isfinite(r->target) ? r->target : 0.0;
    Json adds = Json::array();
    for (const auto& a : p.additions) {
        Json x = Json::object();
        x["value"] = a.value;
        x["footprint"] = a.footprint;
        x["c"] = a.c;
        x["count"] = a.count;
        adds.push(x);
    }
    j["additions"] = adds;
    j["freq"] = nums(p.freq);
    j["zBefore"] = nums(p.zBefore);
    j["zAfter"] = nums(p.zAfter);
    return j;
}

Json pdnIrMapJson(const Project& project, const std::string& net) {
    Json j = Json::object();
    const auto rails = analyzePdn(project);
    const PdnRailResult* r = findRail(rails, net);
    j["rail"] = net;
    if (!r) {
        j["error"] = "Unknown rail " + net;
        return j;
    }
    const BoardSettings& s = project.pcb.settings;
    j["analyzed"] = r->irAnalyzed;
    j["note"] = r->irNote;
    j["voltage"] = r->voltage;
    j["limit"] = r->irLimitPercent / 100 * r->voltage;
    j["worst"] = r->irWorst;
    j["maxDensity"] = r->irMaxDensity;
    Json board = Json::object();
    board["width"] = s.width;
    board["height"] = s.height;
    Json outline = Json::array();
    for (const Vec2& p : s.outlinePolygon()) {
        Json x = Json::object();
        x["x"] = p.x;
        x["y"] = p.y;
        outline.push(x);
    }
    board["outline"] = outline;
    j["board"] = board;
    Json cells = Json::array();
    for (const auto& c : r->irCells) {
        Json x = Json::object();
        x["x"] = c.x;
        x["y"] = c.y;
        x["size"] = c.size;
        x["layer"] = c.layer;
        x["drop"] = c.drop;
        x["density"] = c.density;
        cells.push(x);
    }
    j["cells"] = cells;
    Json segs = Json::array();
    for (const auto& g : r->irSegments) {
        Json x = Json::object();
        x["ax"] = g.a.x;
        x["ay"] = g.a.y;
        x["bx"] = g.b.x;
        x["by"] = g.b.y;
        x["layer"] = g.layer;
        x["width"] = g.width;
        x["current"] = g.current;
        x["density"] = g.density;
        x["drop"] = g.drop;
        segs.push(x);
    }
    j["segments"] = segs;
    Json loads = Json::array();
    for (const auto& l : r->loads) {
        Json x = Json::object();
        x["ref"] = l.ref;
        x["pin"] = l.pin;
        x["x"] = l.position.x;
        x["y"] = l.position.y;
        x["drop"] = l.drop;
        x["connected"] = l.connected;
        loads.push(x);
    }
    j["loads"] = loads;
    if (r->hasVrmPosition) {
        Json src = Json::object();
        src["x"] = r->vrmPosition.x;
        src["y"] = r->vrmPosition.y;
        j["source"] = src;
    }
    // Hotspots: the five highest current densities.
    struct Spot {
        double x, y, density;
        int layer;
    };
    std::vector<Spot> spots;
    for (const auto& c : r->irCells) spots.push_back({c.x, c.y, c.density, c.layer});
    for (const auto& g : r->irSegments) spots.push_back({(g.a.x + g.b.x) / 2, (g.a.y + g.b.y) / 2, g.density, g.layer});
    std::sort(spots.begin(), spots.end(), [](const Spot& a, const Spot& b) { return a.density > b.density; });
    Json hs = Json::array();
    for (size_t k = 0; k < spots.size() && k < 5; ++k) {
        Json x = Json::object();
        x["x"] = spots[k].x;
        x["y"] = spots[k].y;
        x["density"] = spots[k].density;
        x["layer"] = spots[k].layer;
        hs.push(x);
    }
    j["hotspots"] = hs;
    return j;
}

}  // namespace sieda
