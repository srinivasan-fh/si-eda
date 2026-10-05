// SiEDA Core — interactive routing: a track (or a differential pair) follows the cursor from a pad, via or track, in
// walkaround mode (the head finds a way around obstacles) or push-and-shove mode (other nets' tracks and vias are
// shoved aside, recursively, keeping their clearance, net-class widths and the board edge; pads, locked tracks and
// tamper meshes stay fixed). 45° / 90° / free-angle postures, layer change with a via, snap to pads, corner placing,
// commit / cancel, segment drag with shove and length tuning (accordion meanders) on a selected track.
//
// The router never edits the layout until commit(): every step is computed on an overlay of the board taken when
// the route began, so a preview can be thrown away (cancel) and every result is deterministic for the same inputs.
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class RouterMode {
    Walkaround,  // the head goes around everything that is in its way
    Shove,       // the head pushes other nets' tracks and vias aside; falls back to walkaround when that fails
    Highlight,   // the head goes where it is pointed; nothing moves and what it violates is listed (`collisions`)
};

/// Copper (or a board rule) the route violates in Highlight mode. `kind` is "track", "via", "pad", "edge", "hole",
/// "plane" or "mesh"; `at` is the nearest point of the route; tracks give `a`-`b` and `width`, vias `a` and `width`
/// (diameter), pads `a` (centre) and `size`.
struct RouteCollision {
    std::string kind;
    int id = -1;  // track / via id on the board (-1 for pads, board rules and copper the session moved)
    Vec2 at, a, b, size;
    double width = 0;
};

enum class RoutePosture {
    Diagonal45,    // straight and 45° segments (two-segment head)
    Orthogonal90,  // horizontal / vertical only
    Free,          // one straight segment at any angle
};

/// The via addVia() places (HDI, IPC-2226). Blind / buried and micro vias need HDI on (Board Setup) and ≥ 4 layers.
enum class RouterViaType {
    Through,  // top to bottom (always available)
    Blind,    // exactly the layers it joins: blind (from an outer layer) or buried (inner layers only)
    Micro,    // laser microvia across one dielectric (neighbouring layers, microvia drill / pad)
    Auto,     // HDI boards: microvia for neighbouring layers, otherwise the span it joins; through otherwise
};

struct RouterOptions {
    RouterMode mode = RouterMode::Shove;
    RouterViaType viaType = RouterViaType::Through;
    RoutePosture posture = RoutePosture::Diagonal45;
    /// Which bend comes first in the two-segment head (the "/" key in other tools flips it).
    bool swapPosture = false;
    /// Track width (mm); 0 = the net's class width (BoardSettings::widthFor).
    double width = 0;
    /// Differential pair edge-to-edge gap (mm); 0 = the stack-up's coupled gap for the differential impedance target
    /// (never below the board clearance).
    double pairGap = 0;
    /// The head snaps to pads, vias and tracks of its own net under the cursor and reports reaching them.
    bool snapToPads = true;
    /// Most line / via shoves one step may make before it gives up (keeps a step fast on dense boards).
    int shoveLimit = 120;
    /// Rounded corners (single-track routes): every corner becomes an arc of this radius (mm), drawn as short
    /// straight chords of at most 15°, where it fits between its neighbours and keeps clearance; otherwise the
    /// corner stays sharp. 0 = sharp corners, < 0 = automatic (4 × track width, at least 0.5 mm).
    double cornerRadius = 0;
};

/// What commit() changed, so a caller can undo it exactly: removed items (with their old geometry and ids) and the
/// ids of the items that were added.
struct RouteChanges {
    bool ok = false;
    std::string error;
    std::vector<Track> removedTracks;
    std::vector<Via> removedVias;
    std::vector<int> addedTracks;
    std::vector<int> addedVias;
    bool empty() const { return removedTracks.empty() && removedVias.empty() && addedTracks.empty() && addedVias.empty(); }
};

/// The state shown while routing: the route's copper (placed corners and the head that follows the cursor), and the
/// other nets' items at their shoved positions (`hiddenTracks` / `hiddenVias` are the layout items they replace).
struct RoutePreview {
    bool active = false;
    std::string kind;          // "route", "pair", "drag" (a track segment) or "via" (a dragged via)
    std::string status;        // what the router is doing / why the head stopped
    bool blocked = false;      // the head stops short of the cursor
    bool reachedTarget = false;  // the head ends on a pad / via / track of its own net (the route can finish)
    std::vector<int> nets;     // routed net(s): one, or the positive and negative member of a pair
    int layer = 0;
    int layerCount = 0;        // the board's copper layers (via spans in the JSON)
    double width = 0;
    double gap = 0;            // differential pairs: edge-to-edge gap
    Vec2 end;                  // where the head ends (the pair's centre line for a pair)
    std::vector<Track> placed;  // fixed part of the route (corners already placed)
    std::vector<Track> head;    // the segments following the cursor
    std::vector<Via> vias;      // vias placed by this route
    std::vector<Track> shovedTracks;
    std::vector<Via> shovedVias;
    std::vector<int> hiddenTracks;
    std::vector<int> hiddenVias;
    double length = 0;          // route length so far (placed + head, first member of a pair)
    /// The routed net's whole length with this route (its other copper plus `length`; first member of a pair), and
    /// the length it should match: the longest other member of its matched-length group (0 = not in a group).
    double netLength = 0;
    double targetLength = 0;
    /// Highlight mode: what the route's copper violates (empty in the other modes, which never violate anything).
    std::vector<RouteCollision> collisions;
};

class InteractiveRouter {
public:
    /// The router reads `sch` and edits `pcb` only on commit(). Both must outlive it.
    InteractiveRouter(PcbLayout& pcb, const Schematic& sch);
    ~InteractiveRouter();
    InteractiveRouter(const InteractiveRouter&) = delete;
    InteractiveRouter& operator=(const InteractiveRouter&) = delete;

    void setOptions(const RouterOptions& options);  // applies from the next moveTo()
    const RouterOptions& options() const;

    /// Starts a route on the pad, via or track under `at` (the net comes from it). An SMD pad on another copper
    /// layer switches to that layer. False (see error()) when there is nothing with a net under the point, the layer
    /// does not exist or is another net's plane.
    bool beginRoute(Vec2 at, int layer);
    /// Starts a differential pair on a pad of either member (nets named X_P / X_N, X+ / X-, …): both members are
    /// routed together at the pair gap from the nearest pads of the two nets.
    bool beginPair(Vec2 at, int layer);
    /// Drags a track segment: it moves parallel to itself with the cursor, its neighbours follow with 45° joints,
    /// and (shove mode) other nets' copper is pushed aside.
    bool beginDrag(int trackId, Vec2 grab);
    /// Drags a via: it follows the cursor, the tracks ending on it follow with 45° joints, and (shove mode) other
    /// nets' copper is pushed aside. Vias inside a pad of their net, mesh vias and vias held by a locked track or a
    /// track running through them stay put.
    bool beginViaDrag(int viaId, Vec2 grab);

    /// Moves the head to the cursor and returns the preview.
    const RoutePreview& moveTo(Vec2 cursor);
    /// Places the head as it is (a click): the next head starts from its end. False when the head is empty.
    bool fixHead();
    /// Places the head, then a via (options().viaType) at its end and continues on `toLayer`. -1 = the default
    /// layer: the other outer layer for through vias, the next layer towards the other side for blind / micro vias;
    /// -2 = the next layer the other way (blind / micro vias).
    bool addVia(int toLayer = -1);
    /// Places the head and writes the route (and every shoved item) into the layout. The router is idle afterwards.
    RouteChanges commit();
    void cancel();

    bool active() const;
    const RoutePreview& preview() const;
    /// Why the last call failed.
    const std::string& error() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct LengthTuneResult {
    bool ok = false;
    std::string message;
    int net = -1;
    double before = 0, after = 0, target = 0;
    RouteChanges changes;
    /// Matched-length group of the net ("" when none), its kind ("pair" / "bus") and the tolerance tuned to (mm).
    std::string group, groupKind;
    double tolerance = 0;
    /// The copper the tuning adds and the ids of the tracks it replaces (also filled for a preview).
    std::vector<Track> addedTracks;
    std::vector<int> removedTracks;
    bool applied = false;  // the board was changed (false for a preview)
};

struct LengthTuneOptions {
    /// Target length (mm); <= 0 tunes to the longest member of the net's matched-length group (pair or bus).
    double target = 0;
    /// Meander height limit (mm, 0 = 2 mm).
    double maxAmplitude = 0;
    /// Gap between neighbouring meander legs, edge to edge (mm); 0 = the default pitch (3 × width, at least
    /// width + clearance between leg centres).
    double spacing = 0;
    /// Meanders go near this point of the selected track first (e.g. where it was clicked).
    bool hasNear = false;
    Vec2 near;
    /// False: compute the result and its copper (addedTracks / removedTracks) without changing the board.
    bool apply = true;
};

/// Interactive length tuning: lengthens the net of track `trackId` to `target` mm with accordion meanders, on that
/// track first and then on the net's other straight tracks, keeping clearance to everything. `target` <= 0 tunes to
/// the longest member of the net's matched-length group (differential pair or bus, see lengthGroups()).
/// `maxAmplitude` limits the meander height (mm, 0 = 2 mm).
LengthTuneResult tuneTrackLength(PcbLayout& pcb, const Schematic& sch, int trackId, double target,
                                 double maxAmplitude = 0);
/// The same with every option, including a preview that leaves the board alone (`options.apply = false`).
LengthTuneResult tuneTrackLength(PcbLayout& pcb, const Schematic& sch, int trackId, const LengthTuneOptions& options);
/// {"ok","message","net","group","groupKind","tolerance","before","after","target","applied",
///  "addedTracks":[track],"removedTracks":[id],"changes":{…}}
Json lengthTuneJson(const LengthTuneResult& r);
/// Options from {"target","maxAmplitude","spacing","x","y","apply"} (x and y together set the near point).
LengthTuneOptions lengthTuneOptionsFromJson(const Json& j);

/// Options from {"mode":"shove|walkaround","posture":"45|90|free","swapPosture","width","pairGap","snap"} — missing
/// fields keep their value in `base`.
RouterOptions routerOptionsFromJson(const Json& j, RouterOptions base = {});
Json routePreviewJson(const RoutePreview& p);
Json routeChangesJson(const RouteChanges& c);

}  // namespace sieda
