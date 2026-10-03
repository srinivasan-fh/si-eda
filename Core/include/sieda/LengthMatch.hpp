// SiEDA Core — length and phase matching: differential pairs (intra-pair skew) and parallel buses are found from
// net names, their routed lengths compared, and short members lengthened with serpentine (accordion) tuning.
#pragma once

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

#include <string>
#include <utility>
#include <vector>

namespace sieda {

/// Differential pairs by name (signal nets only): X_P / X_N, X+ / X-, X_DP / X_DN, XP / XN. (positive, negative).
std::vector<std::pair<int, int>> differentialPairs(const Schematic& sch);

struct LengthGroup {
    std::string name;  // "USB_D" (pair) or "DQ" (bus)
    std::string kind;  // "pair" or "bus"
    std::vector<int> nets;
    double tolerance = 0;  // mm
};

/// Matched-length groups: every differential pair, and buses of ≥ 2 signal nets sharing a high-speed prefix with an
/// index (DQ0…DQ7, DATA[0..7], ADDR0…, RXD0…, DDR_DQ…).
std::vector<LengthGroup> lengthGroups(const Schematic& sch, const BoardSettings& s);

/// Routed copper length of a net (sum of its track lengths, mm).
double routedNetLength(const PcbLayout& pcb, int net);

/// Lengthens the short members of every group to its longest member with serpentines on their longest straight
/// tracks, keeping clearance to other nets, pads, vias, holes and the board edge. Returns the nets tuned.
int tuneLengths(PcbLayout& pcb, const Schematic& sch);

/// {"groups":[{name,kind,tolerance,target,matched,nets:[{name,length,delta,ok}]}]} for the routed board.
Json lengthReportJson(const PcbLayout& pcb, const Schematic& sch);

}  // namespace sieda
