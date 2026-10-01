#include "sieda/Units.hpp"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

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
