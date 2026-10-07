// SiEDA Core — what changed between two versions of a project (parts, connectivity, placement, copper, board,
// variants), for reviews and Git (`sieda-mcp --diff old.siedaproj new.siedaproj`), and the variants side by side.
#pragma once

#include <string>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"

namespace sieda {

/// {"identical", "summary", "components":{"added","removed","changed"}, "nets":{"added","removed","changed"},
///  "copper":[{"net","tracks","vias","length"}], "board":{field:[old,new]}, "variants":{"added","removed","changed"}}.
/// Parts are matched by designator, nets by their pins (a renamed net with the same pins is a rename only).
Json diffProjects(const Project& before, const Project& after);
/// The diff as review text: one line per change ("+ R5 10k R_0603", "~ U1 moved …", "~ net SDA +U2.5 −U3.7").
std::string diffText(const Json& diff);

/// Every part a variant changes, with its fitting and value in each variant, and per-variant totals:
/// {"variants":[{"name","fitted","notFitted","valueChanges"}], "parts":[{"ref","value","cells":[{"fitted","value"}]}]}.
Json variantMatrix(const Project& project);

}  // namespace sieda
