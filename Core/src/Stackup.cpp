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

bool isStriplineLayer(const BoardSettings& s, int layer) { return s.layerCount >= 4 && layer > 0 && layer < s.layerCount - 1; }

double trackImpedance(const BoardSettings& s, int layer, double w) {
    const double er = boardLaminate(s).er, t = copperThickness(s), h = layerDielectric(s);
    return isStriplineLayer(s, layer) ? striplineImpedance(w, 2 * h + t, t, er) : microstripImpedance(w, h, t, er);
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
    const double h = layerDielectric(s), t = copperThickness(s);
    const bool strip = isStriplineLayer(s, layer);
    auto zdiff = [&](double w) {
        double gap = std::max(w, s.clearance);
        double z0 = trackImpedance(s, layer, w);
        return strip ? differentialStripline(z0, gap, 2 * h + t) : differentialMicrostrip(z0, gap, h);
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
    const double cu = copperThickness(s), d = layerDielectric(s);
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
            di["name"] = l == 0 ? std::string("Core") : "Prepreg / core " + std::to_string(l + 1);
            di["type"] = "dielectric";
            di["thickness"] = d;
            di["material"] = m.name;
            layers.push(di);
        }
    }
    root["layers"] = layers;
    return root;
}

}  // namespace sieda
