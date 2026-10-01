#include "sieda/Units.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace sieda {

namespace {
std::optional<double> prefixMultiplier(const std::string& s, size_t& consumed) {
    consumed = 0;
    if (s.empty()) return 1.0;
    // "meg" must be checked before "m".
    if (s.size() >= 3 && std::tolower(static_cast<unsigned char>(s[0])) == 'm' &&
        std::tolower(static_cast<unsigned char>(s[1])) == 'e' && std::tolower(static_cast<unsigned char>(s[2])) == 'g') {
        consumed = 3;
        return 1e6;
    }
    // UTF-8 micro sign (U+00B5) and Greek mu (U+03BC).
    if (s.size() >= 2 && ((unsigned char)s[0] == 0xC2 && (unsigned char)s[1] == 0xB5)) { consumed = 2; return 1e-6; }
    if (s.size() >= 2 && ((unsigned char)s[0] == 0xCE && (unsigned char)s[1] == 0xBC)) { consumed = 2; return 1e-6; }
    consumed = 1;
    switch (s[0]) {
        case 'f': return 1e-15;
        case 'p': return 1e-12;
        case 'n': return 1e-9;
        case 'u': return 1e-6;
        case 'm': return 1e-3;
        case 'k': case 'K': return 1e3;
        case 'M': return 1e6;
        case 'G': return 1e9;
        case 'T': return 1e12;
        case 'R': case 'r': return 1.0;  // RKM "2R2"
        default: consumed = 0; return 1.0;
    }
}
}  // namespace

std::optional<double> parseEngineeringValue(const std::string& raw) {
    std::string text;
    for (char c : raw)
        if (!std::isspace(static_cast<unsigned char>(c))) text += c;
    if (text.empty()) return std::nullopt;

    size_t i = 0;
    if (text[i] == '+' || text[i] == '-') ++i;
    size_t digitsStart = i;
    while (i < text.size() && (std::isdigit(static_cast<unsigned char>(text[i])) || text[i] == '.')) ++i;
    if (i == digitsStart) return std::nullopt;

    // Exponent (e.g. 1e-6) — only if followed by a digit or sign+digit.
    if (i + 1 < text.size() && (text[i] == 'e' || text[i] == 'E') &&
        (std::isdigit(static_cast<unsigned char>(text[i + 1])) ||
         ((text[i + 1] == '-' || text[i + 1] == '+') && i + 2 < text.size() &&
          std::isdigit(static_cast<unsigned char>(text[i + 2]))))) {
        i += 2;
        while (i < text.size() && std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
    }

    double mantissa = std::strtod(text.substr(0, i).c_str(), nullptr);
    std::string rest = text.substr(i);
    size_t used = 0;
    auto mult = prefixMultiplier(rest, used);
    if (!mult) return std::nullopt;
    rest = rest.substr(used);

    // RKM: digits after the prefix letter are fractional ("4k7" -> 4.7k).
    if (used > 0 && !rest.empty() && std::isdigit(static_cast<unsigned char>(rest[0])) &&
        text.substr(0, i).find('.') == std::string::npos) {
        size_t j = 0;
        while (j < rest.size() && std::isdigit(static_cast<unsigned char>(rest[j]))) ++j;
        std::string frac = "0." + rest.substr(0, j);
        mantissa += std::strtod(frac.c_str(), nullptr) * (mantissa < 0 ? -1 : 1);
    }
    return mantissa * *mult;
}

std::string formatEngineeringValue(double value, const std::string& unit, int significantDigits) {
    if (value == 0.0 || !std::isfinite(value) || std::fabs(value) < 1e-15) return "0" + unit;
    static const struct { double scale; const char* prefix; } kPrefixes[] = {
        {1e12, "T"}, {1e9, "G"}, {1e6, "M"}, {1e3, "k"}, {1.0, ""},
        {1e-3, "m"}, {1e-6, "µ"}, {1e-9, "n"}, {1e-12, "p"}, {1e-15, "f"},
    };
    double mag = std::fabs(value);
    for (const auto& p : kPrefixes) {
        if (mag >= p.scale * 0.9995 || p.scale == 1e-15) {
            char buf[48];
            std::snprintf(buf, sizeof buf, "%.*g", significantDigits, value / p.scale);
            return std::string(buf) + p.prefix + unit;
        }
    }
    return std::to_string(value) + unit;
}

}  // namespace sieda

namespace sieda {

namespace {
const std::vector<double>& mantissas(ESeries series) {
    static const std::vector<double> e12 = {1.0, 1.2, 1.5, 1.8, 2.2, 2.7, 3.3, 3.9, 4.7, 5.6, 6.8, 8.2};
    static const std::vector<double> e24 = {1.0, 1.1, 1.2, 1.3, 1.5, 1.6, 1.8, 2.0, 2.2, 2.4, 2.7, 3.0,
                                            3.3, 3.6, 3.9, 4.3, 4.7, 5.1, 5.6, 6.2, 6.8, 7.5, 8.2, 9.1};
    static const std::vector<double> e96 = {
        1.00, 1.02, 1.05, 1.07, 1.10, 1.13, 1.15, 1.18, 1.21, 1.24, 1.27, 1.30, 1.33, 1.37, 1.40, 1.43,
        1.47, 1.50, 1.54, 1.58, 1.62, 1.65, 1.69, 1.74, 1.78, 1.82, 1.87, 1.91, 1.96, 2.00, 2.05, 2.10,
        2.15, 2.21, 2.26, 2.32, 2.37, 2.43, 2.49, 2.55, 2.61, 2.67, 2.74, 2.80, 2.87, 2.94, 3.01, 3.09,
        3.16, 3.24, 3.32, 3.40, 3.48, 3.57, 3.65, 3.74, 3.83, 3.92, 4.02, 4.12, 4.22, 4.32, 4.42, 4.53,
        4.64, 4.75, 4.87, 4.99, 5.11, 5.23, 5.36, 5.49, 5.62, 5.76, 5.90, 6.04, 6.19, 6.34, 6.49, 6.65,
        6.81, 6.98, 7.15, 7.32, 7.50, 7.68, 7.87, 8.06, 8.25, 8.45, 8.66, 8.87, 9.09, 9.31, 9.53, 9.76};
    switch (series) {
        case ESeries::E12: return e12;
        case ESeries::E24: return e24;
        case ESeries::E96: return e96;
    }
    return e24;
}
}  // namespace

double nearestStandardValue(double value, ESeries series) {
    if (!(value > 0) || !std::isfinite(value)) return value;
    double decade = std::pow(10.0, std::floor(std::log10(value)));
    double best = value, bestErr = 1e300;
    for (double scale : {decade / 10, decade, decade * 10}) {
        for (double m : mantissas(series)) {
            double candidate = m * scale;
            double err = std::fabs(std::log(candidate / value));  // ratio error: fair across decades
            if (err < bestErr) {
                bestErr = err;
                best = candidate;
            }
        }
    }
    return best;
}

bool isStandardValue(double value, ESeries series, double relTolerance) {
    if (!(value > 0)) return false;
    double nearest = nearestStandardValue(value, series);
    return std::fabs(nearest - value) <= relTolerance * nearest;
}

}  // namespace sieda
