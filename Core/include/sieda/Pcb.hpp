// SiEDA Core — PCB layout: board, pads, tracks, vias, auto-placement, autorouter and DRC.
#pragma once

#include <cmath>
#include <map>
#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// Copper layer index: 0 = top, `BoardSettings::bottomLayer()` = bottom, anything between = inner layer.
constexpr int kTopLayer = 0;

/// Non-plated mounting hole with a circular keep-out (screw head / washer) free of copper and parts.
struct MountingHole {
    Vec2 position;
    double drill = 3.2;    // mm (M3 clearance)
    double keepout = 6.4;  // keep-out diameter, mm
};

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
    /// Above 3050 m (aircraft, space): voltage clearances use IPC-2221 Table 6-1 column B3 instead of B2.
    bool highAltitude = false;
    /// Solder mask colour ordered from the fab: green (default), black, blue, red, yellow, white or purple.
    std::string solderMask = "green";
    /// Net classes: track width (mm) per net name, e.g. {"VBAT": 0.8} for motor and battery currents.
    std::map<std::string, double> netWidths;
    /// The autorouter first widens net classes to the IPC-2221 width for each net's simulated current.
    bool autoSizeNets = true;
    double widthFor(const std::string& netName) const {
        auto it = netWidths.find(netName);
        return it == netWidths.end() ? trackWidth : std::max(minTrackWidth, it->second);
    }

    /// Board outline polygon (mm, inside [0,width]×[0,height]); empty = the width × height rectangle.
    std::vector<Vec2> outline;
    std::vector<MountingHole> holes;

    /// Outline as a polygon (the rectangle when no custom outline is set).
    std::vector<Vec2> outlinePolygon() const;
    bool hasCustomOutline() const { return outline.size() >= 3; }
    bool contains(Vec2 p) const;
    /// Distance from `p` to the outline (positive inside, negative outside).
    double edgeDistance(Vec2 p) const;
    /// Smallest distance from a segment to the outline (negative if any part lies outside).
    double segmentEdgeDistance(Vec2 a, Vec2 b) const;
    /// Distance from `p` to the nearest mounting-hole keep-out circle (negative inside one); large if no holes.
    double holeDistance(Vec2 p) const;
    /// True if `r` lies inside the outline at least `margin` from it and clear of every hole keep-out.
    bool rectInside(const Rect& r, double margin) const;
    /// Sets a custom outline (empty = rectangle); shifts it to start at (0,0) and sets width/height to its bounds.
    void setOutline(std::vector<Vec2> polygon);

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

/// Board outline presets: "rectangle" (w × h), "rounded" (corner radius `param`), "circle" (diameter w),
/// "quad-x" (quadcopter frame: span w, square body h, arm width `param`). Returns an empty polygon for unknown names.
std::vector<Vec2> boardOutlinePreset(const std::string& kind, double w, double h, double param);

/// IPC-2221 minimum track width (mm) for `amps` at `tempRise` °C with `oz` copper; inner layers derate by 2×.
double ipc2221TrackWidth(double amps, double tempRise, double oz, bool innerLayer);

/// IPC-2221 Table 6-1 minimum spacing (mm) between external uncoated conductors for a peak voltage difference:
/// column B2 (sea level to 3050 m) or B3 (above 3050 m, e.g. space and avionics).
double ipc2221Clearance(double volts, bool highAltitude);

/// Voltage range (min, max) of every net: the DC operating point widened to SIN/PULSE source peaks. Empty when the
/// circuit has no source, no ground or does not converge.
std::map<int, std::pair<double, double>> netVoltageRanges(const Schematic& sch);
/// IPC-2221 spacing for the largest potential difference on the board (0 when unknown).
double voltageRoutingClearance(const Schematic& sch, bool highAltitude);

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

/// Copper pour rule: fills the free area of `layer` with `net` copper, keeping clearance to every other net, the
/// board edge and the mounting holes, with thermal-relief spokes on through-hole pads and floating islands removed.
struct CopperZone {
    std::string net;
    int layer = kTopLayer;
    /// Plane layer: reserved for this net — other nets cross it only with vias (anti-pads are cut automatically).
    bool plane = false;
    double clearance = 0;  // 0 = board clearance
};

/// Poured copper of one zone, as a raster of islands (cell centres at ((i+0.5)·cell, (j+0.5)·cell)).
struct ZoneFill {
    int zone = -1;
    int net = -1;
    int layer = kTopLayer;
    double cell = 0.1;
    int cols = 0, rows = 0;
    std::vector<int> island;  // island index per cell, -1 = no copper
    int islands = 0;
    std::vector<Rect> rects;  // the same copper as merged rectangles (drawing, Gerber regions, 3D)
    /// Island whose copper overlaps a disc of `radius` at `p`, or -1.
    int islandNear(Vec2 p, double radius) const;
    /// Island of the cell containing `p`, or -1.
    int islandAt(Vec2 p) const {
        int i = static_cast<int>(std::floor(p.x / cell)), j = static_cast<int>(std::floor(p.y / cell));
        if (i < 0 || j < 0 || i >= cols || j >= rows) return -1;
        return island[static_cast<size_t>(j * cols + i)];
    }
    double area() const;
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
    std::vector<CopperZone> zones;

    /// Pours every zone against the given copper (pads, tracks, vias). Zones fill in order; later zones keep
    /// clearance to earlier ones of other nets.
    std::vector<ZoneFill> fillZones(const Schematic& sch, const std::vector<Pad>& pads, const std::vector<Track>& tracks,
                                    const std::vector<Via>& vias) const;
    /// Current pours (cached; refilled automatically when copper, zones or the board change).
    const std::vector<ZoneFill>& zoneFills(const Schematic& sch) const;
    /// True if `net` has a zone (its connections may be made through poured copper).
    bool isZoneNet(const Schematic& sch, int net) const;

    /// Pads of every footprint currently placed on the board, with net assignments from the schematic.
    std::vector<Pad> pads(const Schematic& sch) const;
    Rect courtyard(const Component& c) const;

    /// Places any not-yet-placed footprints; with `all` re-places everything (connectivity-driven clustering).
    void autoPlace(Schematic& sch, bool all);
    /// Rips up existing routing and routes every net on `settings.layerCount` layers (grid A* with through vias,
    /// layer direction preferences and rip-up passes). A single-layer board routes on the top layer without vias.
    RouteStats autoRoute(const Schematic& sch);

private:
    RouteStats routeAll(const Schematic& sch);

public:
    void clearRouting() { tracks.clear(); vias.clear(); }
    /// Net classes from the simulated operating point: nets whose DC current needs a wider IPC-2221 track (+25 %
    /// margin, rounded up to 0.05 mm) get one. Existing wider classes are kept. Returns the widths it set.
    std::map<std::string, double> autoNetWidths(const Schematic& sch);
    /// Narrows track ends that enter pads smaller than the track (fine-pitch neck-down).
    void neckDown(std::vector<Track>& out, const std::vector<Pad>& pads) const;
    /// Per pad: the widest track that can leave it between its package neighbours (≥ the minimum track width).
    std::vector<double> padNeckWidths(const std::vector<Pad>& pads) const;
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
    mutable std::vector<ZoneFill> fillCache_;
    mutable size_t fillKey_ = 0;
    mutable bool fillValid_ = false;
};

}  // namespace sieda
