// SiEDA Core — PCB layout: board, pads, tracks, vias, auto-placement, autorouter and DRC.
#pragma once

#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// Copper layer index: 0 = top, `BoardSettings::bottomLayer()` = bottom, anything between = inner layer.
constexpr int kTopLayer = 0;

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
    int layerCount = 2;  // 1 (single-sided), 2, 4 or 6 copper layers

    // Fabrication limits (DRC errors below these) — set together with the design values by a rule preset.
    std::string rulePreset = "IPC-2221 Class 2";
    double minTrackWidth = 0.15;
    double minClearance = 0.15;
    double minDrill = 0.2;
    double minAnnularRing = 0.1;
    double minHoleToHole = 0.25;
    double copperWeightOz = 1.0;  // outer layers; inner layers use the same weight
    double maxTempRise = 10.0;    // °C, IPC-2221 current-capacity target

    /// Applies a named preset from designRulePresets(); returns false if the name is unknown.
    bool applyPreset(const std::string& name);

    int bottomLayer() const { return layerCount > 1 ? layerCount - 1 : 0; }
    static int normalizeLayerCount(int n) {
        if (n <= 1) return 1;
        if (n <= 2) return 2;
        if (n <= 4) return 4;
        return 6;
    }
};

/// Standard design-rule sets (design values + fabrication minimums), in millimetres.
struct DesignRulePreset {
    std::string name;
    std::string description;
    double trackWidth, clearance, viaDrill, viaDiameter, edgeClearance;
    double minTrackWidth, minClearance, minDrill, minAnnularRing, minHoleToHole;
};
const std::vector<DesignRulePreset>& designRulePresets();

/// IPC-2221 minimum track width (mm) for `amps` at `tempRise` °C with `oz` copper; inner layers derate by 2×.
double ipc2221TrackWidth(double amps, double tempRise, double oz, bool innerLayer);

/// Human-readable copper layer name ("Top", "Inner 1", "Bottom").
std::string copperLayerName(int layer, int layerCount);

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
    int smdLayer = 0;     // copper layer of an SMD pad (0 = top, bottomLayer() for parts on the bottom side)
    Rect bounds() const { return Rect::centered(position, size.x, size.y); }
    bool onLayer(int layer) const { return throughHole || layer == smdLayer; }
};

struct Track {
    int id = -1;
    int net = -1;
    int layer = kTopLayer;
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
    /// Rips up existing routing and routes every net on `settings.layerCount` layers (grid A* with through vias,
    /// layer direction preferences and rip-up passes). A single-layer board routes on the top layer without vias.
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
