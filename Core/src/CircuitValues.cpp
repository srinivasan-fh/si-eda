// SiEDA Core — the values of a circuit model plan that the request fixes (LocalModel.hpp, circuitPlanValues).
// The model (tools/circuit_lm) picks the circuit and writes its parts and links; values that follow from numbers in the
// request are computed here with the rules of its training data (tools/circuit_lm/make_dataset.py, E12 values), so a
// 7 V LED or a 3.3 kHz filter gets the right part even where the model's guess is off.
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <regex>
#include <string>

#include "sieda/Json.hpp"
#include "sieda/LocalModel.hpp"
#include "sieda/Units.hpp"

namespace sieda {
namespace {

double e12(double x) {
    static const double series[] = {1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2, 10.0};
    const double decade = std::pow(10.0, std::floor(std::log10(x)));
    double best = decade;
    for (double m : series)
        if (std::fabs(std::log(m * decade / x)) < std::fabs(std::log(best / x))) best = m * decade;
    return best;
}

std::string g(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%g", v);
    return buf;
}

/// 10000 → "10k", 3.3e-6 → "3u3" (the training data's value strings).
std::string eng(double x, const std::string& unit = "") {
    static const std::pair<double, const char*> prefixes[] = {{1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1, ""},
                                                              {1e-3, "m"}, {1e-6, "u"}, {1e-9, "n"}, {1e-12, "p"}};
    for (const auto& [p, s] : prefixes) {
        if (std::fabs(x) < p * 0.999) continue;
        char buf[32];
        std::snprintf(buf, sizeof buf, "%.3g", x / p);
        std::string t = buf;
        const size_t dot = t.find('.');
        if (dot != std::string::npos && *s && t.size() <= 3) return t.replace(dot, 1, s) + unit;
        return t + s + unit;
    }
    return g(x) + unit;
}

bool number(const std::string& text, const std::regex& re, double& out, int group = 1) {
    std::smatch m;
    if (!std::regex_search(text, m, re)) return false;
    out = std::strtod(m[group].str().c_str(), nullptr);  // no throw on a huge digit run
    return std::isfinite(out);
}

}  // namespace

std::string circuitPlanValues(const std::string& request, const std::string& planJson) {
    Json plan;
    try {
        plan = Json::parse(planJson);
    } catch (const std::exception&) {
        return planJson;
    }
    if (!plan.isObject() || !plan.get("components").isArray()) return planJson;
    const std::string title = plan.get("title").asString("");
    const auto icase = std::regex::icase;
    struct Colour {
        const char* word;
        const char* value;
        double vf;
    };
    static const Colour colours[] = {
        {"red", "Red", 2.0}, {"green", "Green", 2.1}, {"blue", "Blue", 3.0}, {"white", "White", 3.0}, {"yellow", "Yellow", 2.1}};
    const Colour* colour = nullptr;
    for (const Colour& c : colours)
        if (std::regex_search(request, std::regex(std::string("\\b") + c.word + "\\b", icase))) colour = &c;
    double v = 0, ma = 0, vin = 0, vout = 0, fc = 0, gain = 0;
    const bool hasV = number(request, std::regex(R"((\d+(?:\.\d+)?)\s*V\b)", icase), v);
    std::map<std::string, std::string> values;
    std::string newTitle;
    if (title.find("LED Indicator") != std::string::npos && hasV && colour &&
        number(request, std::regex(R"((\d+(?:\.\d+)?)\s*mA\b)", icase), ma) && ma > 0 && v > colour->vf) {
        values = {{"V1", g(v)}, {"R1", eng(e12((v - colour->vf) / (ma / 1000)))}, {"D1", colour->value}};
        newTitle = g(v) + " V " + colour->value + " LED Indicator";
    } else if (title.find("LED Driver") != std::string::npos && hasV && colour && v > colour->vf + 0.1) {
        values = {{"V1", g(v)}, {"R2", eng(e12((v - colour->vf - 0.1) / 0.009))}, {"D1", colour->value}};
        newTitle = std::string("NPN ") + colour->value + " LED Driver, " + g(v) + " V";
    } else if (title.find("Divider") != std::string::npos) {
        const std::regex re(R"((\d+(?:\.\d+)?)\s*V\s+(?:down\s+)?to\s+(\d+(?:\.\d+)?)\s*V)", icase);
        if (number(request, re, vin, 1) && number(request, re, vout, 2) && vin > vout && vout > 0) {
            values = {{"V1", g(vin)}, {"R1", "10k"}, {"R2", eng(e12(10e3 * vout / (vin - vout)))}};
            newTitle = g(vin) + " V to " + g(vout) + " V Divider";
        }
    } else if (title.find("RC Low-Pass") != std::string::npos || title.find("RC High-Pass") != std::string::npos) {
        std::smatch m;
        if (std::regex_search(request, m, std::regex(R"((\d[\d.]*)\s*([kKMG]?)(\d*)\s*Hz)", icase))) {  // "1k6Hz", "10 kHz"
            const auto parsed = parseEngineeringValue(m[1].str() + m[2].str() + m[3].str());
            if (parsed && (fc = *parsed) > 0) {
                const bool low = title.find("Low-Pass") != std::string::npos;
                values = {{"R1", "1k"}, {"C1", eng(e12(1 / (2 * M_PI * 1e3 * fc)))}};
                newTitle = std::string("RC ") + (low ? "Low-Pass" : "High-Pass") + " Filter, " + eng(fc, "Hz");
            }
        }
    } else if (title.find("Inverting Amplifier") != std::string::npos &&
               number(request, std::regex(R"(gain\s*(?:of\s*)?-?\s*(\d+(?:\.\d+)?)(?![\d.]|\s*dB))", icase), gain) && gain > 1) {
        if (title.find("Non-Inverting") != std::string::npos) {
            values = {{"R1", eng(e12(10e3 * (gain - 1)))}, {"R2", "10k"}};
            newTitle = "Non-Inverting Amplifier, Gain " + g(gain);
        } else {
            values = {{"R1", "10k"}, {"R2", eng(e12(10e3 * gain))}};
            newTitle = "Inverting Amplifier, Gain −" + g(gain);
        }
    }
    if (values.empty()) return planJson;
    Json parts = Json::array();
    for (Json c : plan.get("components").items()) {
        const auto it = values.find(c.get("ref").asString(""));
        if (it != values.end()) c["value"] = it->second;
        parts.push(c);
    }
    plan["components"] = parts;
    plan["title"] = newTitle;
    return plan.dump();
}

}  // namespace sieda
