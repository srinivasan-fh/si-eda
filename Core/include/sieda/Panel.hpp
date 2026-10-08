// SiEDA Core — production panels: the board stepped nx × ny between two rails, with three fiducials and four tooling
// holes on the rails, separated by V-score lines or by a routed gap held by breakaway tabs with mouse-bite holes. The
// panel files are made from the board's own Gerbers and drills (shifted, then repeated with Gerber step-and-repeat), so
// every board in the panel is exactly the single-board artwork.
#pragma once

#include <string>
#include <vector>

#include "sieda/Fabrication.hpp"
#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

/// Where everything sits on the panel (mm, Gerber coordinates: origin bottom-left, Y up).
struct PanelLayout {
    double width = 0, height = 0;          // panel
    double boardWidth = 0, boardHeight = 0;
    std::vector<Vec2> boards;              // bottom-left corner of each board
    std::vector<Vec2> fiducials;           // 1 mm copper dots, 2 mm mask openings (top and bottom)
    std::vector<Vec2> toolingHoles;        // 2 mm non-plated
    std::vector<Rect> tabs;                // breakaway tabs (tab panels)
    std::vector<Vec2> mouseBites;          // 0.5 mm non-plated holes along each tab's board edge
    std::vector<std::pair<Vec2, Vec2>> vscores;  // V-score lines across the panel
};

PanelLayout panelLayout(const BoardSettings& s);

/// {"nx","ny","gap","rail","vscore"}; null when there is no panel (1 × 1).
Json panelToJson(const PanelSettings& p);
/// Reads the settings (counts clamped to 1 … 20, gap 0 … 20 mm, rail 0 … 20 mm); null = no panel.
void panelFromJson(const Json& j, PanelSettings& p);
/// The layout as JSON for previews: settings, sizes, boards, fiducials, tooling holes, tabs, mouse bites, V-scores.
Json panelLayoutJson(const BoardSettings& s);

/// Panel files under panel/ (Gerbers, drills, outline, V-score layer, zip) from the board package `boardFiles` (the
/// gerbers/ entries of fabricationPackage). Empty when the board has no panel.
std::vector<FabFile> panelFiles(const PcbLayout& pcb, const std::string& base, const std::vector<FabFile>& boardFiles);

}  // namespace sieda
