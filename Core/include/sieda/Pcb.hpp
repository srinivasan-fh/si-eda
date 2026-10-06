// SiEDA Core — PCB layout: board, pads, tracks, vias, auto-placement, autorouter and DRC.
#pragma once

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

struct Track;

/// A net class's length rule: the net's routed length, pad to pad through series parts (its xSignal, see
/// LengthRules.hpp), should be `target` ± `tolerance` mm.
struct LengthRule {
    std::string net;
    double target = 0;
    double tolerance = 0.1;
};

/// A match group (as xSignal classes in other tools): the routed lengths of these nets' xSignals should match the
/// longest of them within `tolerance` mm.
struct MatchGroup {
    std::string name;
    std::vector<std::string> nets;
    double tolerance = 0.1;
};

/// Autorouter strategy options (Board Setup → Routing strategy; docs/ROUTING.md). Every option defaults to the
/// router's classic behaviour, so a board whose options are all default routes exactly as before.
struct AutorouteOptions {
    /// Differential pairs (differentialPairs(): net names and schematic directives) route as coupled pairs: both
    /// members together at the pair gap, symmetric fan-out from their pads, coupled vias side by side. A pair the
    /// coupled router cannot lay out routes as two single nets, as before.
    bool coupledPairs = false;
    /// Edge-to-edge gap of coupled pairs (mm); 0 = the stack-up's gap for the differential impedance target (never
    /// below the pair's clearance).
    double pairGap = 0;
    /// Length-aware routing: nets with a length rule or in a match group (BoardSettings::lengthRules / matchGroups,
    /// measured pad to pad through series parts as xSignals) route first, on their most direct paths, and after
    /// routing are lengthened to their target with the interactive tuner's meanders (mitred accordions); the routing
    /// report lists achieved against target for each.
    bool lengthAware = false;
    /// Quality passes after routing. Via minimisation: each net with vias is routed again, alone, with layer
    /// changes three times as expensive, and keeps the new copper when it has fewer vias and is at most 25 % longer.
    bool minimizeVias = false;
    /// Glossing: every routed line is pulled tight with the interactive router's gloss (45° shortcuts and a re-search,
    /// kept when shorter). Coupled pairs and tuned meanders are left alone.
    bool gloss = false;
    /// True-arc corners on autorouted copper (convertCornersToArcs), radius `arcRadius` mm (0 = automatic).
    bool arcCorners = false;
    double arcRadius = 0;
    /// Teardrops where tracks meet pads and vias (addTeardrops).
    bool teardrops = false;
    bool operator==(const AutorouteOptions& o) const;
    bool operator!=(const AutorouteOptions& o) const { return !(*this == o); }
    bool isDefault() const { return *this == AutorouteOptions{}; }
};

/// A routing keep-out (Board Setup → Keep-outs): no track and / or no via of any net inside `area` on `layer` (-1 = every
/// copper layer). The autorouter routes around it and the DRC reports copper inside it (DRC_KEEPOUT).
struct RouteKeepout {
    std::string name;
    Rect area;
    int layer = -1;
    bool tracks = true, vias = true;
};

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
    /// Conformal coating applied after assembly ("none" or an IPC-CC-830 type, see conformalCoatings()).
    std::string coating = "none";
    /// Underfill / corner-bond epoxy specified for BGAs, processors and heavy parts (shock: MIL-STD-901E, MIL-STD-810).
    bool underfill = false;
    /// Galvanic isolation barrier spacing (mm): parts, tracks, vias and pours of different isolation domains keep at
    /// least this far apart (8 mm = 2 × MOPP, 4 mm = 1 × MOPP; 0 = no barrier rule). See Isolation.hpp.
    double isolationGap = 0;
    bool coated() const { return !coating.empty() && coating != "none"; }
    /// Laminate (see laminateMaterials()): "fr4", "fr4-hightg", "isola-370hr", "rogers-4350b", "megtron-6",
    /// "polyimide", "ims-aluminium".
    std::string material = "fr4";
    /// "rigid", "rigid-flex" (IPC-2223) or "metal-core" (aluminium IMS for power / LEDs).
    std::string construction = "rigid";
    /// Controlled-impedance targets (Ω) for RF / single-ended high-speed nets and differential pairs.
    double singleEndedImpedance = 50;
    double differentialImpedance = 100;
    /// Backdrill through-via stubs on high-speed nets (multilayer boards).
    bool backdrill = false;
    /// HDI (IPC-2226): after routing, each via is cut to the layers it connects (blind / buried), and vias between
    /// neighbouring layers become laser microvias of this drill / pad size.
    bool hdi = false;
    double microviaDrill = 0.1;
    double microviaDiameter = 0.25;
    /// Via-in-pad plated over (VIPPO, IPC-4761 Type VII): vias in SMD pads are filled and capped, so they are allowed.
    bool viaInPad = false;
    /// Length / phase matching: after routing, serpentines are added so differential pairs (intra-pair skew) and
    /// parallel buses (DQ0…n, DATA[0..7], ADDR…) match within these tolerances (mm).
    bool lengthTuning = true;
    double pairSkewTolerance = 0.13;   // ≈ 5 mil (USB / PCIe / LVDS intra-pair)
    double busLengthTolerance = 0.5;   // ≈ 20 mil (DDR byte lane / parallel bus)
    /// Solder mask colour ordered from the fab: green (default), black, blue, red, yellow, white or purple.
    std::string solderMask = "green";
    /// Net classes: track width (mm) per net name, e.g. {"VBAT": 0.8} for motor and battery currents.
    std::map<std::string, double> netWidths;
    /// Length rules per net and match groups (Board Setup; the length tuning tool and the DRC use them).
    std::vector<LengthRule> lengthRules;
    std::vector<MatchGroup> matchGroups;
    /// Autorouter strategy (saved with the board only when it differs from the defaults).
    AutorouteOptions autorouter;
    /// Routing keep-outs (saved only when there are any).
    std::vector<RouteKeepout> keepouts;
    /// The autorouter first widens net classes to the IPC-2221 width for each net's simulated current.
    bool autoSizeNets = true;
    double widthFor(const std::string& netName) const {
        auto it = netWidths.find(netName);
        return it == netWidths.end() ? trackWidth : std::max(minTrackWidth, it->second);
    }
    /// Net classes from the schematic's directives: clearance (mm) per net name, kept at least `clearance`.
    std::map<std::string, double> netClearances;
    double clearanceFor(const std::string& netName) const {
        auto it = netClearances.find(netName);
        return it == netClearances.end() ? clearance : std::max(clearance, it->second);
    }
    /// Nets whose width / clearance came from schematic directives (Project::applySchematicRules replaces them).
    std::set<std::string> schematicRuleNets;

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
    /// The same for a track's centre line (segmentEdgeDistance for a straight track; exact for an arc).
    double trackEdgeDistance(const Track& t) const;
    /// Distance from `p` to the nearest mounting-hole keep-out circle (negative inside one); large if no holes.
    double holeDistance(Vec2 p) const;
    /// True if `r` lies inside the outline at least `margin` from it and clear of every hole keep-out.
    bool rectInside(const Rect& r, double margin) const;
    /// Sets a custom outline (empty = rectangle); shifts it to start at (0,0) and sets width/height to its bounds.
    void setOutline(std::vector<Vec2> polygon);

    /// Applies a named preset from designRulePresets(); returns false if the name is unknown.
    bool applyPreset(const std::string& name);

    int bottomLayer() const { return layerCount > 1 ? layerCount - 1 : 0; }
    /// Supported stack-ups: single-sided, then even layer counts up to 24 (servers, mainframes and GPU baseboards
    /// run 12–24 layers).
    static constexpr int kMaxLayers = 24;
    static int normalizeLayerCount(int n) {
        if (n <= 1) return 1;
        if (n <= 2) return 2;
        return std::min(kMaxLayers, n + (n % 2));
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

/// IPC-2221B Table 6-1 columns: B1 internal conductors; B2 external uncoated, sea level to 3050 m; B3 external
/// uncoated above 3050 m; B4 external with permanent polymer coating; A5 external with conformal coating over the
/// assembly; A6 external component lead / termination, uncoated; A7 component lead / termination, conformal coated.
enum class Ipc2221Column { B1, B2, B3, B4, A5, A6, A7 };
const char* ipc2221ColumnName(Ipc2221Column c);  // "B2", "A5", …
/// Minimum conductor spacing (mm) for a peak voltage difference, from IPC-2221B Table 6-1 (per-volt above 500 V).
double ipc2221Spacing(double volts, Ipc2221Column column);
/// The spacing the board's external copper needs: A5 when the assembly is conformal coated (any altitude), otherwise
/// B2 (sea level) or B3 (above 3050 m, e.g. space and avionics).
double ipc2221Clearance(double volts, bool highAltitude, bool coated = false);
Ipc2221Column externalSpacingColumn(bool highAltitude, bool coated);

/// Conformal coatings (IPC-CC-830 / IPC-HDBK-830): "none", "acrylic" (AR), "silicone" (SR), "urethane" (UR),
/// "epoxy" (ER), "parylene" (XY). Coating seals the surface against moisture, dust and contamination (surface leakage).
const std::vector<std::string>& conformalCoatings();

/// Voltage range (min, max) of every net: the DC operating point widened to SIN/PULSE source peaks. Empty when the
/// circuit has no source, no ground or does not converge.
std::map<int, std::pair<double, double>> netVoltageRanges(const Schematic& sch);
/// IPC-2221 spacing for the largest potential difference on the board (0 when unknown).
double voltageRoutingClearance(const Schematic& sch, bool highAltitude, bool coated = false);

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
    /// Locked tracks stay where they are: the interactive router never shoves or drags them.
    bool locked = false;
    /// True arc (3-point form): the track runs from `a` through `mid` to `b` on one circle. Straight when false; an
    /// arc whose three points are collinear is the straight a–b. Measure tracks with TrackGeometry.hpp, never a–b.
    bool arc = false;
    Vec2 mid;
    /// Part of a teardrop (InteractiveRouter.hpp addTeardrops): runs from inside a pad / via (`a`) onto the track
    /// it widens (`b`). Ordinary copper for DRC, Gerber and connectivity; the router leaves it in place.
    bool teardrop = false;
};

struct Via {
    int id = -1;
    int net = -1;
    Vec2 position;
    double drill = 0.3, diameter = 0.6;
    /// Copper layers the barrel connects (HDI, IPC-2226): 0 … -1 is a through via (top to bottom); otherwise a blind
    /// (one outer layer), buried (inner layers only) or laser microvia (one dielectric, small drill).
    int fromLayer = 0;
    int toLayer = -1;  // -1 = bottom layer
    bool isThrough() const { return fromLayer <= 0 && toLayer < 0; }
    bool spans(int layer) const { return layer >= fromLayer && (toLayer < 0 || layer <= toLayer); }
    int lastLayer(int layerCount) const { return toLayer < 0 ? std::max(0, layerCount - 1) : toLayer; }
    bool overlaps(const Via& o) const {
        return (toLayer < 0 || o.fromLayer <= toLayer) && (o.toLayer < 0 || fromLayer <= o.toLayer);
    }
};

/// "through", "blind", "buried" or "microvia" (one dielectric, drill ≤ 0.15 mm).
const char* viaKind(const Via& v, int layerCount);

/// Testing aid: with brute force on, runDRC (and the copper-connectivity scan) tests every pair of items instead of
/// only the candidates from its spatial index. Both paths give identical results; brute force is O(n²).
void setDrcBruteForce(bool on);
bool drcBruteForce();

/// Autorouter strategy. Auto (default): boards whose routing grid has at least 2 M nodes (cells × copper layers) use
/// the corridor router — coarse global routing, detailed routing only inside each connection's corridor, independent
/// nets on several threads, targeted rip-up — and smaller boards the classic whole-board router, whose results are
/// unchanged. Classic / Corridor force one of them (tests, benchmarks). See docs/ROUTING.md.
enum class RouterStrategy { Auto, Classic, Corridor };
void setRouterStrategy(RouterStrategy strategy);
RouterStrategy routerStrategy();
/// Worker threads of the corridor router; 0 (default) = the hardware's concurrency, at most 8. The routed copper is
/// the same for every thread count.
void setRoutingThreads(int threads);
int routingThreads();
int effectiveRoutingThreads();

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

/// Active tamper mesh (PCI PTS / FIPS 140-3 level 3+): two serpentine traces on inner layers, one running in
/// horizontal stripes and one in vertical stripes, cover the secure element `componentRef` plus `margin` mm. Each
/// mesh net has exactly two pads (drive and sense pins of the secure element); the autorouter lays the mesh, ties
/// each pad to one end of it and keeps every other net — and every via — out of the covered area, so drilling or
/// cutting into the secure area breaks or shorts a mesh line and the secure element zeroizes its keys.
struct TamperMesh {
    std::string componentRef;
    std::string netA, netB;  // horizontal-stripe mesh on layerA, vertical-stripe mesh on layerB
    int layerA = 1, layerB = 2;
    double margin = 2.0;
};

/// The copper a tamper mesh lays out: `region` is the covered (secure) area; tracks are the serpentines with their
/// leaders; `ends` are the via positions at the two ends of each serpentine ([0],[1] = netA, [2],[3] = netB).
struct TamperMeshGeometry {
    int mesh = -1;
    std::string error;  // why the mesh cannot be laid out (empty when it can)
    Rect region;
    int netA = -1, netB = -1;
    std::vector<Track> tracks;
    std::vector<Vec2> ends;
    /// Pads tied to each end, in the same order as `ends` (indices into PcbLayout::pads()).
    std::vector<size_t> endPads;
};

/// Where a running autoroute is (RouteControl::progress).
struct RouteProgress {
    enum Phase { Preparing = 0, Routing = 1, RipUp = 2, Finishing = 3 };
    int phase = Preparing;
    int pass = 0;              // routing pass: 0 the first, then rip-up / recovery passes
    int done = 0, total = 0;   // nets routed in this pass, nets this pass routes
    int unrouted = -1;         // connections the best pass so far leaves unrouted (-1 before the first pass ends)
};

/// Hooks for a long autoroute. `progress` is called on the routing thread (never concurrently, a few hundred times
/// at most); returning false cancels the route.
struct RouteControl {
    std::function<bool(const RouteProgress&)> progress;
};

/// A differential pair after autorouting (RouteReport::pairs).
struct PairRouteReport {
    std::string positive, negative;  // net names
    bool coupled = false;            // laid out by the coupled pair router (false: routed as two single nets)
    std::string reason;              // why it was not coupled ("" when it was)
    double width = 0, gap = 0;       // mm
    double coupledLength = 0;        // centre-line length the members run side by side at the gap (mm)
    double uncoupledLength = 0;      // fan-out stubs and via jogs, both members together (mm)
    double skew = 0;                 // |length P − length N| of the routed copper after tuning (mm)
    int viaPairs = 0;                // coupled via transitions
};
/// A net with a length target after autorouting (RouteReport::lengths).
struct LengthRouteReport {
    std::string net;
    std::string source;      // "rule:<net>" or "group:<name>" (LengthTarget::source)
    double target = 0, tolerance = 0;
    double routed = -1;      // xSignal length as routed, before tuning (-1: not routed pad to pad)
    double achieved = -1;    // after the length-aware tuning
    bool ok = false;         // achieved within target ± tolerance
    bool tuned = false;      // meanders were added
};
/// Quality metrics of the routed board (RouteReport::metrics).
struct RouteMetrics {
    int vias = 0;                     // every via on the board after routing
    int microvias = 0, blindVias = 0;  // HDI spans (viaKind)
    double trackLength = 0;           // mm, every track but teardrops
    std::vector<double> layerLength;  // mm per copper layer
    int segments = 0;                 // tracks
    int arcs = 0;                     // true-arc tracks
    int teardrops = 0;                // teardrop tracks
    int unrouted = 0;                 // ratsnest lines left
    // What the quality passes did.
    int viasRemoved = 0;              // via minimisation
    int netsRerouted = 0;             // nets whose copper via minimisation replaced
    int glossed = 0;                  // lines glossing improved
    int arcsAdded = 0;                // corners made arcs
    int teardropsAdded = 0;
};
/// What an autoroute did beyond the counts in RouteStats (the routing report sheet in the app).
struct RouteReport {
    std::vector<PairRouteReport> pairs;
    std::vector<LengthRouteReport> lengths;
    RouteMetrics metrics;
};
struct RouteStats {
    RouteReport report;
    int connections = 0;
    int routed = 0;
    int failed = 0;
    int vias = 0;
    double trackLength = 0;
    int lengthTuned = 0;  // nets lengthened with serpentines (length / phase matching)
    bool cancelled = false;  // the RouteControl cancelled the route: the board was left as it was
    std::vector<std::string> failedNets;
};

class PcbLayout {
public:
    BoardSettings settings;
    std::vector<Track> tracks;
    std::vector<Via> vias;
    std::vector<CopperZone> zones;
    std::vector<TamperMesh> tamperMeshes;
    /// Layout of every tamper mesh for the current placement (see TamperMesh).
    std::vector<TamperMeshGeometry> tamperMeshGeometry(const Schematic& sch, const std::vector<Pad>& pads) const;

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
    /// The same, reporting progress and allowing cancellation (RouteControl). A cancelled route leaves the board
    /// exactly as it was (tracks, vias, net classes) and returns stats with `cancelled` set.
    RouteStats autoRoute(const Schematic& sch, const RouteControl& control);

private:
    RouteStats routeWithVoltageSpacing(const Schematic& sch, const RouteControl* control);
    RouteStats routeAll(const Schematic& sch, const RouteControl* control);

public:
    void clearRouting() { tracks.clear(); vias.clear(); }
    /// Net classes from the simulated operating point: nets whose DC current needs a wider IPC-2221 track (+25 %
    /// margin, rounded up to 0.05 mm) get one. Existing wider classes are kept. Returns the widths it set.
    std::map<std::string, double> autoNetWidths(const Schematic& sch);
    /// Final polish of routed copper, as a designer would leave it: collinear pieces of a track are merged and
    /// right-angle corners are chamfered to 45° wherever the chamfer keeps clearance to other nets and the board edge.
    /// Connectivity never changes. Returns the number of corners chamfered plus segments merged.
    int cleanupRouting(const Schematic& sch);
    /// HDI: shrinks every via to the copper layers it actually connects (tracks, pads, pours) and turns one-dielectric
    /// spans into laser microvias. Returns the vias changed.
    int applyHdiVias(const Schematic& sch);
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
    /// Set by the corridor router around its clean-up: corners a T-join or a via's edge depends on are not reshaped.
    bool cleanupKeepsJoins_ = false;
    /// The corridor router's multi-resolution grid: off for a second, full-resolution route (fullResolutionOnly_), and
    /// whether the last route used it (usedCoarse_).
    bool fullResolutionOnly_ = false, usedCoarse_ = false;
    mutable std::vector<ZoneFill> fillCache_;
    mutable size_t fillKey_ = 0;
    mutable bool fillValid_ = false;
};

}  // namespace sieda

// Track geometry (straight and arc tracks); needs Track above.
#include "sieda/TrackGeometry.hpp"
