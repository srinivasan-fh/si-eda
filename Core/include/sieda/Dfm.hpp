// SiEDA Core — manufacturer DFM / DFA rule packs: what a fab and assembly house can build (minimum track and space,
// drill, annular ring, via, solder-mask web, silkscreen, layers, board size and thickness, part spacing). Picking a
// pack tightens the board's DRC limits to it; the DRC then adds the checks the copper rules do not cover (DFM_* for
// fabrication, DFA_* for assembly). Values follow the makers' published capability pages; check the current page
// before ordering.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct DfmPack {
    std::string id, name, maker, notes;
    // Fabrication (mm)
    double minTrack, minSpace, minDrill, minAnnularRing, minHoleToHole, minViaDiameter;
    double minMaskSliver, minSilkToPad, minEdgeCopper;
    int maxLayers;
    double minThickness, maxThickness, maxWidth, maxHeight;
    // Assembly (mm)
    double minPartSpacing, minPartToEdge;
};

const std::vector<DfmPack>& dfmPacks();
const DfmPack* findDfmPack(const std::string& id);
Json dfmPacksJson();

/// Selects a pack for the board (empty = none): its limits replace looser DRC minimums (never relax stricter ones).
/// False for an unknown id.
bool applyDfmPack(BoardSettings& s, const std::string& id);

/// The DFM / DFA checks of the board's pack (none without a pack): DFM_LAYERS, DFM_BOARD_SIZE, DFM_THICKNESS,
/// DFM_VIA_SIZE, DFM_MASK_SLIVER, DFM_SILK_TO_PAD, DFA_PART_SPACING, DFA_PART_TO_EDGE, DFA_FIDUCIALS,
/// DFA_TALL_NEAR_FINE_PITCH, DFA_HEAVY_BOTTOM.
std::vector<RuleViolation> dfmChecks(const Schematic& sch, const PcbLayout& pcb);

}  // namespace sieda
