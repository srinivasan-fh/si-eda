// SiEDA Core — PCB layout: board, pads, tracks, vias, auto-placement, autorouter and DRC.
#pragma once

#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class CopperLayer : int { Top = 0, Bottom = 1 };

struct BoardSettings {
    double width = 50.0;   // mm
    double height = 40.0;  // mm
    double thickness = 1.6;
    double trackWidth = 0.25;
    double clearance = 0.2;
    double viaDrill = 0.3;
    double viaDiameter = 0.6;
    double edgeClearance = 0.5;
    double routingGrid = 0.25;
};

struct Pad {
    int componentId = -1;
    int pinIndex = -1;
    int padNumber = 0;  // 1-based footprint pad number
    int net = -1;       // schematic net index, -1 = no net
    Vec2 position;      // mm, board space
    Vec2 size;          // mm, after rotation
    bool throughHole = false;
    bool round = false;
    double drill = 0;
    bool bottom = false;  // SMD pad side
    Rect bounds() const { return Rect::centered(position, size.x, size.y); }
    bool onLayer(CopperLayer l) const { return throughHole || (bottom == (l == CopperLayer::Bottom)); }
};

struct Track {
    int id = -1;
    int net = -1;
    CopperLayer layer = CopperLayer::Top;
    double width = 0.25;
    Vec2 a, b;
};

struct Via {
    int id = -1;
    int net = -1;
    Vec2 position;
    double drill = 0.3, diameter = 0.6;
};

struct RouteStats {
    int connections = 0;
    int routed = 0;
    int failed = 0;
    int vias = 0;
    double trackLength = 0;
    std::vector<std::string> failedNets;
};

class PcbLayout {
public:
    BoardSettings settings;
    std::vector<Track> tracks;
    std::vector<Via> vias;

    /// Pads of every footprint currently placed on the board, with net assignments from the schematic.
    std::vector<Pad> pads(const Schematic& sch) const;
    Rect courtyard(const Component& c) const;

    /// Places any not-yet-placed footprints; with `all` re-places everything (connectivity-driven clustering).
    void autoPlace(Schematic& sch, bool all);
    /// Rips up existing routing and routes every net (two-layer grid A* with vias and rip-up passes).
    RouteStats autoRoute(const Schematic& sch);
    void clearRouting() { tracks.clear(); vias.clear(); }
    /// Shrinks/grows the board to the placed footprints plus `margin` mm, shifting parts and copper together.
    bool fitBoardToComponents(Schematic& sch, double margin);
    /// Removes tracks/vias of nets that no longer exist after schematic edits.
    void pruneStaleRouting(const Schematic& sch);

    std::vector<RuleViolation> runDRC(const Schematic& sch) const;
    /// Number of unrouted pad-to-pad connections (ratsnest lines) — also returns them for display.
    std::vector<std::pair<Vec2, Vec2>> ratsnest(const Schematic& sch) const;

    int addTrack(Track t);
    int addVia(Via v);

private:
    int nextId_ = 1;
};

}  // namespace sieda
