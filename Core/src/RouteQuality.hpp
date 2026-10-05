// Internal header (not installed): the autorouter's passes after routing (Core/src/RouteQuality.cpp), called by
// PcbLayout::routeAll when the board's AutorouteOptions ask for them. They work on the finished board through the
// interactive router's engines (length tuner), so every change keeps the same rules the interactive tools keep.
#pragma once

#include <map>
#include <set>
#include <vector>

#include "sieda/Pcb.hpp"

namespace sieda::routequality {

/// Length-aware routing, after routing: every net with a length rule or in a match group that is shorter than its
/// target is lengthened with mitred accordion meanders (tuneTrackLength, its rule / group target), on its longest
/// tracks first, a wider meander when the first try falls short. A member of a coupled pair (`coupledPartner`: P net
/// → N net) is tuned with its partner as a pair. Returns one report per net with a target.
std::vector<LengthRouteReport> tuneLengthTargets(PcbLayout& pcb, const Schematic& sch, const std::map<int, int>& coupledPartner);

}  // namespace sieda::routequality
