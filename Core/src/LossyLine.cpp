#include "sieda/LossyLine.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <mutex>

#include "sieda/FieldSolver.hpp"
#include "sieda/PowerIntegrity.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Stackup.hpp"

namespace sieda {

namespace {
const double kLn10 = std::log(10.0);
const double kEta0 = kVacuumPermeability * kSpeedOfLight;  // 376.73 Ω
}  // namespace

// ---- dielectric --------------------------------------------------------------------------------------------------------

DielectricModel DielectricModel::djordjevicSarkar(double er, double tanD, double fRef) {
    DielectricModel d;
    d.erRef = er;
    d.tanDRef = std::max(0.0, tanD);
    d.fRef = fRef > 0 ? fRef : 1e9;
    const double w = 2 * kPi * d.fRef;
    const cplx L = (std::log(cplx(d.w2, w)) - std::log(cplx(d.w1, w))) / kLn10;
    // Im L < 0 between ω1 and ω2: k > 0 gives ε'' = er·tanδ at fRef; ε∞ puts ε' at er.
    d.k = L.imag() < 0 ? -er * d.tanDRef / L.imag() : 0;
    d.epsInf = er - d.k * L.real();
    return d;
}

cplx DielectricModel::epsilon(double f) const {
    if (k == 0) return {epsInf, 0};
    const double w = 2 * kPi * std::max(0.0, f);
    return epsInf + k * (std::log(cplx(w2, w)) - std::log(cplx(w1, w))) / kLn10;
}

double DielectricModel::tanD(double f) const {
    const cplx e = epsilon(f);
    return e.real() > 0 ? -e.imag() / e.real() : 0;
}

// ---- copper ------------------------------------------------------------------------------------------------------------

const std::vector<CopperFoil>& copperFoils() {
    // Representative profiles of the IPC-4562 foil grades. Huray parameters are typical published fits (sphere radius,
    // high-frequency excess); fit your fabricator's data for sign-off.
    static const std::vector<CopperFoil> f = {
        {"smooth", "Smooth (ideal)", 0.0, 0.0, 0.0},
        {"hvlp", "HVLP (hyper very low profile)", 0.4, 0.4, 0.6},
        {"vlp", "VLP (very low profile)", 0.6, 0.5, 1.0},
        {"rtf", "RTF (reverse treated)", 0.8, 0.6, 1.4},
        {"std", "Standard ED", 1.5, 1.0, 2.2},
    };
    return f;
}

CopperFoil copperFoilFor(const BoardSettings& s, const std::string& id) {
    for (const auto& f : copperFoils())
        if (f.id == id) return f;
    const std::string pick = boardLaminate(s).lossTangent < 0.005 ? "hvlp" : "std";
    for (const auto& f : copperFoils())
        if (f.id == pick) return f;
    return copperFoils().back();
}

double skinDepth(double f) { return f > 0 ? std::sqrt(kCopperResistivity / (kPi * f * kVacuumPermeability)) : 1e9; }

double surfaceResistance(double f) { return f > 0 ? std::sqrt(kPi * f * kVacuumPermeability * kCopperResistivity) : 0; }

double hammerstadRoughness(double f, double rmsM) {
    if (!(rmsM > 0) || !(f > 0)) return 1;
    const double x = rmsM / skinDepth(f);
    return 1 + 2 / kPi * std::atan(1.4 * x * x);
}

double hurayRoughness(double f, double radiusM, double surfaceRatio) {
    if (!(radiusM > 0) || !(surfaceRatio > 0) || !(f > 0)) return 1;
    const double d = skinDepth(f) / radiusM;
    return 1 + surfaceRatio / (1 + d + d * d / 2);
}

cplx hurayRoughnessCausal(double f, double radiusM, double surfaceRatio) {
    if (!(radiusM > 0) || !(surfaceRatio > 0) || !(f > 0)) return 1;
    const cplx d = skinDepth(f) / radiusM * cplx(1, -1) / std::sqrt(2.0);
    return 1.0 + surfaceRatio / (1.0 + d + d * d / 2.0);
}

double striplineConductorFactor(double widthMm, double thicknessMm, double bMm, double z0, double er) {
    const double W = std::max(1e-6, widthMm * 1e-3), t = std::max(1e-7, thicknessMm * 1e-3);
    const double b = std::max(2 * t + 1e-6, bMm * 1e-3);
    double alphaPerRs;  // α_c / Rs (Np/m per Ω)
    if (std::sqrt(er) * z0 < 120) {
        const double A = 1 + 2 * W / (b - t) + (1 / kPi) * (b + t) / (b - t) * std::log((2 * b - t) / t);
        alphaPerRs = 2.7e-3 * er * z0 * A / (30 * kPi * (b - t));
    } else {
        const double B = 1 + b / (0.5 * W + 0.7 * t) * (0.5 + 0.414 * t / W + 1 / (2 * kPi) * std::log(4 * kPi * W / t));
        alphaPerRs = 0.16 * B / (z0 * b);
    }
    return 2 * z0 * alphaPerRs;
}

double hammerstadJensenZ0Air(double u) {
    u = std::max(1e-6, u);
    const double F = 6 + (2 * kPi - 6) * std::exp(-std::pow(30.666 / u, 0.7528));
    return kEta0 / (2 * kPi) * std::log(F / u + std::sqrt(1 + 4 / (u * u)));
}

double microstripConductorFactor(double widthMm, double thicknessMm, double heightMm) {
    // External inductance of the air-filled line with the Hammerstad–Jensen thickness correction of the width.
    auto inductance = [](double w, double h, double t) {
        const double u = w / h;
        double du = 0;
        if (t > 0) {
            const double T = t / h;
            const double ch = 1 / std::tanh(std::sqrt(6.517 * u));
            du = T / kPi * std::log(1 + 4 * std::exp(1.0) / (T * ch * ch));
        }
        return hammerstadJensenZ0Air(u + du) / kSpeedOfLight;
    };
    const double w = std::max(1e-4, widthMm), h = std::max(1e-4, heightMm), t = std::max(0.0, thicknessMm);
    // Every conductor wall recedes by dn: the strip narrows and thins by 2 dn, the strip-to-plane gap grows by 2 dn.
    const double dn = 1e-3 * std::min({w, h, t > 0 ? t : w}) / 2;
    const double lp = inductance(w - 2 * dn, h + 2 * dn, std::max(0.0, t - 2 * dn));
    const double lm = inductance(w + 2 * dn, h - 2 * dn, t > 0 ? t + 2 * dn : 0);
    const double dLdn = (lp - lm) / (2 * dn * 1e-3);  // H/m per m
    return std::max(0.0, dLdn / kVacuumPermeability);
}

RoughnessModel roughnessFromString(const std::string& s) {
    if (s == "none") return RoughnessModel::None;
    if (s == "hammerstad") return RoughnessModel::Hammerstad;
    return RoughnessModel::Huray;
}

// ---- line model --------------------------------------------------------------------------------------------------------

LineModel LineModel::ideal(double z0, double delayPerM) {
    LineModel m;
    m.z0 = std::max(1e-3, z0);
    m.epsEff = 1;
    m.er = 1;
    m.fill = 0;
    m.dielectric = DielectricModel::djordjevicSarkar(1, 0);
    m.lExt = m.z0 * delayPerM;
    m.cAir = delayPerM / m.z0;
    return m;
}

cplx LineModel::internalZ(double f) const {
    const double r0 = std::max(rdc, kResistanceFloor);
    if (!(acFactor > 0) || !(rdc > 0)) return r0;
    const double w = 2 * kPi * std::max(0.0, f);
    const double wc = rdc * rdc / (kCopperResistivity * kVacuumPermeability * acFactor * acFactor);
    const cplx skin = rdc * std::sqrt(cplx(1, w / wc)) - rdc;  // AC part, causal: analytic in jω
    cplx k = 1;
    if (roughness == RoughnessModel::Hammerstad) k = hammerstadRoughness(f, foil.rmsUm * 1e-6);
    else if (roughness == RoughnessModel::Huray) k = hurayRoughnessCausal(f, foil.radiusUm * 1e-6, foil.surfaceRatio);
    return rdc + skin * k;
}

cplx LineModel::seriesZ(double f) const { return internalZ(f) + cplx(0, 2 * kPi * f * lExt); }

cplx LineModel::shuntY(double f) const {
    const cplx eps = fill > 0 ? 1.0 + fill * (dielectric.epsilon(f) - 1.0) : cplx(1, 0);
    return cplx(0, 2 * kPi * f) * cAir * eps;
}

cplx LineModel::gamma(double f) const {
    cplx g = std::sqrt(seriesZ(f) * shuntY(f));
    if (g.real() < 0) g = -g;
    return g;
}

cplx LineModel::zc(double f) const {
    const cplx y = shuntY(f);
    if (std::abs(y) < 1e-300) return 1e12;
    cplx z = std::sqrt(seriesZ(f) / y);
    if (z.real() < 0) z = -z;
    return z;
}

LineModel lineModelFrom(bool stripline, double z0, double epsEff, double er, double tanD, double widthMm, double thicknessMm,
                        double heightMm, const CopperFoil& foil, const LossOptions& opt) {
    LineModel m;
    m.stripline = stripline;
    m.z0 = std::max(1.0, z0);
    m.er = std::max(1.0, er);
    m.epsEff = std::clamp(epsEff, 1.0, m.er);
    m.fill = stripline || m.er - 1 < 1e-9 ? 1.0 : std::clamp((m.epsEff - 1) / (m.er - 1), 0.0, 1.0);
    m.widthMm = std::max(1e-3, widthMm);
    m.thicknessMm = std::max(1e-4, thicknessMm);
    m.heightMm = std::max(1e-3, heightMm);
    m.foil = foil;
    const double se = std::sqrt(m.epsEff);
    m.lExt = m.z0 * se / kSpeedOfLight;
    m.cAir = 1 / (m.z0 * se * kSpeedOfLight);
    const bool dielLoss = !opt.lossless && !opt.noDielectricLoss;
    // Lossless: εr held at its 1 GHz value (no dispersion), so the line is exactly the reflection analysis' line.
    m.dielectric = DielectricModel::djordjevicSarkar(m.er, dielLoss ? tanD : 0.0);
    if (!opt.lossless && !opt.noConductorLoss) {
        m.rdc = kCopperResistivity / (m.widthMm * 1e-3 * m.thicknessMm * 1e-3);
        m.acFactor = stripline ? striplineConductorFactor(m.widthMm, m.thicknessMm, m.heightMm, m.z0, m.er)
                               : microstripConductorFactor(m.widthMm, m.thicknessMm, m.heightMm);
        m.roughness = opt.roughness;
    }
    return m;
}

LineModel lineModel(const BoardSettings& s, int layer, double widthMm, const LossOptions& opt) {
    const LaminateMaterial& lam = boardLaminate(s);
    const bool strip = isStriplineLayer(s, layer);
    if (opt.fieldSolver && widthMm > 0) {
        // The field-solved cross-section, once per geometry (a solve takes ~0.3 s).
        static std::mutex mu;
        static std::map<std::vector<double>, FieldResult> cache;
        const FieldGeometry g = trackGeometry(s, layer, widthMm);
        const std::vector<double> key{g.w, g.t, g.h, g.hTop, g.er};
        FieldResult r;
        {
            std::lock_guard<std::mutex> lock(mu);
            const auto it = cache.find(key);
            if (it != cache.end()) r = it->second;
        }
        if (!(r.z0 > 0)) {
            r = solveField(g);
            std::lock_guard<std::mutex> lock(mu);
            if (cache.size() > 256) cache.clear();
            cache[key] = r;
        }
        if (r.z0 > 0) {
            LineModel m = lineModelFrom(strip, r.z0, r.eeff, lam.er, lam.lossTangent, widthMm, g.t,
                                        impedanceReferenceHeight(s, layer), copperFoilFor(s, opt.foil), opt);
            if (m.acFactor > 0) m.acFactor = r.rGeom;  // signal and return surfaces from the solved current
            return m;
        }
    }
    return lineModelFrom(strip, std::max(5.0, trackImpedance(s, layer, widthMm)), effectivePermittivity(s, layer, widthMm),
                         lam.er, lam.lossTangent, widthMm, copperThickness(s), impedanceReferenceHeight(s, layer),
                         copperFoilFor(s, opt.foil), opt);
}

Json lineLossJson(const BoardSettings& s, const LossOptions& opt, double widthMm, double fMaxHz) {
    Json root = Json::object();
    const CopperFoil foil = copperFoilFor(s, opt.foil);
    const LaminateMaterial& lam = boardLaminate(s);
    root["foil"] = foil.id;
    root["foilName"] = foil.name;
    root["roughness"] = opt.roughness == RoughnessModel::None ? "none"
                        : opt.roughness == RoughnessModel::Hammerstad ? "hammerstad"
                                                                       : "huray";
    root["material"] = lam.name;
    root["er"] = lam.er;
    root["tanD"] = lam.lossTangent;
    fMaxHz = std::clamp(fMaxHz, 1e9, 100e9);
    std::vector<double> freq;
    const int n = 41;
    for (int k = 0; k < n; ++k) freq.push_back(1e8 * std::pow(fMaxHz / 1e8, k / double(n - 1)));
    Json fj = Json::array();
    for (double f : freq) fj.push(f);
    root["freq"] = fj;
    Json layers = Json::array();
    const int count = std::max(1, s.layerCount);
    constexpr double kInch = 0.0254;
    for (int l = 0; l < count; ++l) {
        double w = widthMm > 0 ? widthMm : widthForImpedance(s, l, s.singleEndedImpedance);
        if (!(w > 0)) w = s.trackWidth;
        LossOptions cOnly = opt, dOnly = opt;
        cOnly.noDielectricLoss = true;
        dOnly.noConductorLoss = true;
        const LineModel m = lineModel(s, l, w, opt), mc = lineModel(s, l, w, cOnly), md = lineModel(s, l, w, dOnly);
        Json j = Json::object();
        j["layer"] = l;
        j["name"] = copperLayerName(l, count);
        j["line"] = m.stripline ? "stripline" : "microstrip";
        j["width"] = w;
        j["z0"] = m.z0;
        j["epsEff"] = m.epsEff;
        Json tot = Json::array(), cond = Json::array(), diel = Json::array();
        for (double f : freq) {
            tot.push(m.attenuationDb(f) * kInch);
            cond.push(mc.attenuationDb(f) * kInch);
            diel.push(md.attenuationDb(f) * kInch);
        }
        j["dbPerInch"] = tot;
        j["conductorDbPerInch"] = cond;
        j["dielectricDbPerInch"] = diel;
        Json rl = Json::object();
        const double f1 = 1e9;
        const cplx z = m.seriesZ(f1), y = m.shuntY(f1);
        rl["r"] = z.real();
        rl["l"] = z.imag() / (2 * kPi * f1);
        rl["g"] = y.real();
        rl["c"] = y.imag() / (2 * kPi * f1);
        rl["rdc"] = m.rdc;
        j["rlgc1GHz"] = rl;
        layers.push(j);
    }
    root["layers"] = layers;
    return root;
}

}  // namespace sieda
