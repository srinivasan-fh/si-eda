// SiEDA Core — rule scripting: design rules the user writes as expressions over a board object's properties, run by
// the DRC. Syntax: numbers (with an optional unit: mm, mil, in), "strings", properties, ( ), + - * /, comparisons
// == != < <= > >=, ~ (glob match: net ~ "USB_*"), && / and, || / or, ! / not. Properties per object:
//   track: net, width, length, layer ("Top", "Bottom", "Inner 1"…), layerIndex (0 = top), arc, locked
//   via:   net, drill, diameter, ring, through
//   pad:   net, ref, pin, width, height, drill, through, npth
//   part:  ref, value, footprint, mpn, x, y, rotation, bottom, dnp
//   net:   net, length (routed), tracks, vias, pads, minWidth
// e.g. {"name": "usb-width", "object": "track", "when": "net ~ \"USB_*\"", "require": "width >= 0.2mm"}.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// RULE_<NAME> per object breaking a rule (at most 200 per rule), RULE_SYNTAX for a rule that cannot be read.
std::vector<RuleViolation> customRuleChecks(const Schematic& sch, const PcbLayout& pcb);
/// "" when the rule reads, else why not (unknown object, syntax, unknown property).
std::string customRuleError(const CustomRule& rule);
Json customRulesToJson(const std::vector<CustomRule>& rules);
std::vector<CustomRule> customRulesFromJson(const Json& j);

}  // namespace sieda
