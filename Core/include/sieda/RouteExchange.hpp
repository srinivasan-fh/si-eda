// SiEDA Core — routed copper exchanged with other tools: HyperLynx BoardSim (.hyp) export for signal-integrity work,
// and route import (tracks, arcs, vias) from PADS ASCII (.asc), HyperLynx (.hyp) and IPC-2581 (.xml / .cvg, the
// route-carrying export of Cadence Allegro / OrCAD PCB Editor) onto a board whose parts are already placed.
#pragma once

#include <string>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"

namespace sieda {

/// HyperLynx .hyp (VERSION 2.14, inches, Y up): outline, stack-up, devices, pad stacks, then every net's pins, tracks,
/// arcs, vias and pours.
std::string exportHyperlynx(const Project& project);

/// Reads the routes of a PADS ASCII, HyperLynx or IPC-2581 file (detected from its text) and adds them to the board,
/// replacing its tracks and vias when `replace`. Nets match by name (then ignoring case). The file's frame (Y up) is
/// fitted to this board by its named pins (HyperLynx PIN, IPC-2581 PinRef) when most agree, else by the track ends
/// that land in pads of their net (then on pad centres, as PADS runs end), this board's own export frame winning ties.
/// Result: format, tracks, vias, nets, unknownNets, offset, padHits (ends in a pad of their net) / ends, or "error".
Json importRoutes(Project& project, const std::string& text, bool replace = true);

}  // namespace sieda
