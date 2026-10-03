#include "sieda/Stackup.hpp"

#include <algorithm>
#include <cmath>

namespace sieda {

const std::vector<LaminateMaterial>& laminateMaterials() {
    static const std::vector<LaminateMaterial> m = {
        {"fr4", "FR-4 (standard, Tg 140 °C)", 4.4, 0.020, 140, 0.3, "General purpose up to ~1 GHz"},
        {"fr4-hightg", "High-Tg FR-4 (IPC-4101/126, Tg 170 °C)", 4.2, 0.018, 170, 0.3,
         "Automotive, power, lead-free reflow, thick multilayers"},
        {"isola-370hr", "Isola 370HR", 4.04, 0.021, 180, 0.4, "High-reliability multilayer, thermal cycling"},
        {"rogers-4350b", "Rogers RO4350B", 3.66, 0.0037, 280, 0.69, "RF / microwave to ~10 GHz, low loss"},
        {"megtron-6", "Panasonic Megtron 6", 3.61, 0.004, 185, 0.4, "High-speed digital: PCIe, 25G+ SerDes, DDR5"},
        {"polyimide", "Polyimide (IPC-4101/40)", 4.2, 0.010, 250, 0.3, "Space, high temperature, flex / rigid-flex"},
        {"ims-aluminium", "Aluminium IMS (metal core, 2 W/m·K dielectric)", 4.5, 0.020, 130, 2.0,
         "Power LEDs, motor drives, EV inverters: heat into the metal base"},
    };
    return m;
}

const LaminateMaterial* findLaminate(const std::string& id) {
    for (const auto& m : laminateMaterials())
        if (m.id == id) return &m;
    return nullptr;
}

const LaminateMaterial& boardLaminate(const BoardSettings& s) {
    const LaminateMaterial* m = findLaminate(s.material);
    return m ? *m : laminateMaterials().front();
}

double microstripImpedance(double w, double h, double t, double er) {
    double x = 5.98 * h / (0.8 * w + t);
    return x > 1 ? 87.0 / std::sqrt(er + 1.41) * std::log(x) : 0;
}

double striplineImpedance(double w, double b, double t, double er) {
    double x = 4 * b / (0.67 * 3.14159265358979 * (0.8 * w + t));
    return x > 1 ? 60.0 / std::sqrt(er) * std::log(x) : 0;
}

double differentialMicrostrip(double z0, double s, double h) { return 2 * z0 * (1 - 0.48 * std::exp(-0.96 * s / h)); }
double differentialStripline(double z0, double s, double b) { return 2 * z0 * (1 - 0.374 * std::exp(-2.9 * s / b)); }

double copperThickness(const BoardSettings& s) { return 0.035 * std::max(0.5, s.copperWeightOz); }

double layerDielectric(const BoardSettings& s) {
    const int layers = std::max(1, s.layerCount);
    const double cu = copperThickness(s);
    if (layers <= 2) return std::max(0.05, s.thickness - layers * cu);
    return std::max(0.05, (s.thickness - layers * cu) / (layers - 1));
}

namespace {
constexpr double kBuildUp = 0.075;  // HDI laser-drillable build-up dielectric (≈ 3 mil)
bool hdiStack(const BoardSettings& s) { return s.hdi && s.layerCount >= 4; }
}  // namespace

double dielectricBelow(const BoardSettings& s, int layer) {
    const int n = std::max(1, s.layerCount);
    if (n <= 2 || !hdiStack(s)) return layerDielectric(s);
    if (layer == 0 || layer == n - 2) return kBuildUp;
    const double cu = copperThickness(s);
    return std::max(0.05, (s.thickness - n * cu - 2 * kBuildUp) / (n - 3));
}

double layerDepth(const BoardSettings& s, int layer) {
    double d = 0;
    for (int k = 0; k < layer; ++k) d += copperThickness(s) + dielectricBelow(s, k);
    return d;
}

namespace {
/// Microstrip: height to the plane below / above; stripline: plane-to-plane spacing b.
double referenceHeight(const BoardSettings& s, int layer) {
    const int n = std::max(1, s.layerCount);
    if (isStriplineLayer(s, layer)) return dielectricBelow(s, layer - 1) + dielectricBelow(s, layer) + copperThickness(s);
    return dielectricBelow(s, layer <= 0 ? 0 : std::max(0, n - 2));
}
}  // namespace

bool isStriplineLayer(const BoardSettings& s, int layer) { return s.layerCount >= 4 && layer > 0 && layer < s.layerCount - 1; }

double trackImpedance(const BoardSettings& s, int layer, double w) {
    const double er = boardLaminate(s).er, t = copperThickness(s), h = referenceHeight(s, layer);
    return isStriplineLayer(s, layer) ? striplineImpedance(w, h, t, er) : microstripImpedance(w, h, t, er);
}

double widthForImpedance(const BoardSettings& s, int layer, double ohms) {
    // Impedance falls as the track widens: bisection on width.
    double lo = 0.02, hi = 10.0;
    if (trackImpedance(s, layer, lo) < ohms || trackImpedance(s, layer, hi) > ohms) return 0;
    for (int i = 0; i < 60; ++i) {
        double mid = (lo + hi) / 2;
        (trackImpedance(s, layer, mid) > ohms ? lo : hi) = mid;
    }
    return (lo + hi) / 2;
}

std::pair<double, double> differentialPairGeometry(const BoardSettings& s, int layer, double ohms) {
    const double h = referenceHeight(s, layer);
    const bool strip = isStriplineLayer(s, layer);
    auto zdiff = [&](double w) {
        double gap = std::max(w, s.clearance);
        double z0 = trackImpedance(s, layer, w);
        return strip ? differentialStripline(z0, gap, h) : differentialMicrostrip(z0, gap, h);
    };
    double lo = 0.02, hi = 10.0;
    if (zdiff(lo) < ohms || zdiff(hi) > ohms) return {0, 0};
    for (int i = 0; i < 60; ++i) {
        double mid = (lo + hi) / 2;
        (zdiff(mid) > ohms ? lo : hi) = mid;
    }
    double w = (lo + hi) / 2;
    return {w, std::max(w, s.clearance)};
}

double viaBarrelDepth(const BoardSettings& s, const Via& v) {
    const int n = std::max(1, s.layerCount);
    const int last = v.lastLayer(n);
    if (v.fromLayer <= 0 && last >= n - 1) return s.thickness;
    return layerDepth(s, last) + copperThickness(s) - layerDepth(s, v.fromLayer);
}

Json stackupJson(const BoardSettings& s) {
    const LaminateMaterial& m = boardLaminate(s);
    Json root = Json::object();
    root["material"] = m.id;
    root["materialName"] = m.name;
    root["er"] = m.er;
    root["lossTangent"] = m.lossTangent;
    root["tg"] = m.tg;
    root["construction"] = s.construction;
    root["singleEndedOhms"] = s.singleEndedImpedance;
    root["differentialOhms"] = s.differentialImpedance;
    Json layers = Json::array();
    const int n = std::max(1, s.layerCount);
    const double cu = copperThickness(s);
    root["hdi"] = hdiStack(s);
    for (int l = 0; l < n; ++l) {
        Json c = Json::object();
        c["name"] = copperLayerName(l, n);
        c["type"] = "copper";
        c["thickness"] = cu;
        c["line"] = isStriplineLayer(s, l) ? "stripline" : "microstrip";
        double w = widthForImpedance(s, l, s.singleEndedImpedance);
        c["seWidth"] = w;
        auto [dw, gap] = differentialPairGeometry(s, l, s.differentialImpedance);
        c["diffWidth"] = dw;
        c["diffGap"] = gap;
        layers.push(c);
        if (l < n - 1 || n == 1) {
            Json di = Json::object();
            const bool buildUp = hdiStack(s) && (l == 0 || l == n - 2);
            di["name"] = buildUp ? std::string("Build-up ") + std::to_string(l + 1) + " (laser microvias)"
                         : l == 0 ? std::string("Core")
                                  : "Prepreg / core " + std::to_string(l + 1);
            di["type"] = "dielectric";
            di["thickness"] = dielectricBelow(s, l);
            di["material"] = m.name;
            layers.push(di);
        }
    }
    root["layers"] = layers;
    return root;
}

}  // namespace sieda
