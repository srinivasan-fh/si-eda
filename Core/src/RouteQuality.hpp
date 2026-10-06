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

/// Glossing (interactive gloss with re-search) of every routed line except those of `skipNets` and locked tracks.
/// Returns the lines it improved.
int glossRouted(PcbLayout& pcb, const Schematic& sch, const std::set<int>& skipNets);
/// True-arc corners on the routed copper (convertCornersToArcs; radius 0 = automatic). Returns the corners converted.
int arcRouted(PcbLayout& pcb, const Schematic& sch, double radius);
/// Teardrops on every pad and via joint (addTeardrops). Returns the teardrops added.
int teardropsRouted(PcbLayout& pcb, const Schematic& sch);
/// Fills the board measurements of `m` (vias by kind, length per layer, segments, arcs, teardrops, unrouted).
void measure(const PcbLayout& pcb, const Schematic& sch, RouteMetrics& m);

}  // namespace sieda::routequality
