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
int teardropsRouted(PcbLayout& pcb, const Schematic& sch, TeardropStyle style = TeardropStyle::Straight);
/// Acute joins (corridor routes): where two tracks of a net end at one point at less than 90° (an acid trap, the DRC's
/// DRC_ACUTE_ANGLE), outside pads and vias, one end moves back along its track and a short link joins it square to the
/// other track (90° there, 90° + the angle at the bend); a track left dangling at the old joint is trimmed to the
/// link. Each link keeps every clearance, keep-out and the board edge; undone if a connection opens. Returns the joins
/// fixed.
int fixAcuteJoins(PcbLayout& pcb, const Schematic& sch);
/// Shape-based completion (AutorouteOptions::shapeCompletion): each ratsnest line left after the grid passes,
/// shortest first, is routed by the interactive router on the exact copper shapes, from either end and on every copper
/// layer of its start pad: walkaround, then push-and-shove, then (multi-layer boards) a first leg to a via site on or
/// beside the line and the rest on another layer. A route is kept only when it reaches its target and no other
/// connection opens. Returns the connections completed.
int shapeComplete(PcbLayout& pcb, const Schematic& sch);
/// Fills the board measurements of `m` (vias by kind, length per layer, segments, arcs, teardrops, unrouted).
void measure(const PcbLayout& pcb, const Schematic& sch, RouteMetrics& m);

}  // namespace sieda::routequality
