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


/// IEC 60063 preferred-number series used for resistors and capacitors.
enum class ESeries { E12 = 12, E24 = 24, E96 = 96 };

/// Nearest standard value of `series` (any decade). Returns `value` unchanged for non-positive input.
double nearestStandardValue(double value, ESeries series);

/// True if `value` matches a member of `series` within `relTolerance` (default 0.5 %).
bool isStandardValue(double value, ESeries series, double relTolerance = 0.005);

}  // namespace sieda
