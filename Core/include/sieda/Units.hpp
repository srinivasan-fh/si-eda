// SiEDA Core — engineering-notation value parsing/formatting ("4.7k", "100nF", "2M2").
#pragma once

#include <optional>
#include <string>

namespace sieda {

/// Parses an engineering value. Accepts SI prefixes (f p n u µ m k M G T, "meg"),
/// RKM notation ("4k7", "2R2"), and trailing unit text ("10uF", "5V", "330Ω").
std::optional<double> parseEngineeringValue(const std::string& text);

/// Formats a value with an SI prefix, e.g. formatEngineeringValue(4700, "Ω") == "4.7kΩ".
std::string formatEngineeringValue(double value, const std::string& unit = "", int significantDigits = 3);

}  // namespace sieda
