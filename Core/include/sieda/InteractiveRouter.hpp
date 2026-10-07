// SiEDA Core — interactive routing: a track (or a differential pair) follows the cursor from a pad, via or track, in
// walkaround mode (the head finds a way around obstacles) or push-and-shove mode (other nets' tracks and vias are
// shoved aside, recursively, keeping their clearance, net-class widths and the board edge; pads, locked tracks and
// tamper meshes stay fixed). 45° / 90° / free-angle postures, layer change with a via, snap to pads, corner placing,
// commit / cancel, segment drag with shove and length tuning (accordion meanders) on a selected track.
//
// The router never edits the layout until commit(): every step is computed on an overlay of the board taken when
// the route began, so a preview can be thrown away (cancel) and every result is deterministic for the same inputs.
#pragma once

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/LengthRules.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class RouterMode {
    Walkaround,  // the head goes around everything that is in its way
    Shove,       // the head pushes other nets' tracks and vias aside; falls back to walkaround when that fails
    Highlight,   // the head goes where it is pointed; nothing moves and what it violates is listed (`collisions`)
    Stop,        // the head stops at the first obstacle (nothing is shoved or walked around)
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
    /// With a corner radius: corners become true arcs (Track::arc) instead of chords, also for differential pairs
    /// and buses, whose members then turn on concentric arcs at their exact spacing (the radius is the innermost
    /// member's).
    bool arcCorners = false;
    /// Loop removal: on commit, old tracks of the routed nets that the new route makes redundant (they were needed
    /// before, now another path joins the same pads) are removed, with vias left unconnected. Nets with pours or
    /// planes are left alone.
    bool removeLoops = false;
    /// Teardrops on commit where the new tracks meet pads and vias (addTeardrops).
    bool autoTeardrops = false;
    /// Outline of those teardrops (JSON "teardropStyle": "straight" | "curved").
    TeardropStyle teardropStyle = TeardropStyle::Straight;
    /// Hug: a dragged segment that runs into copper that cannot move (pads, locked tracks, fixed vias) bends around it
    /// on its clearance hull instead of stopping short; other nets' tracks are still shoved (Shove) or kept clear.
    bool hugDrag = false;
    /// Tune while routing: routing a bus, or a single net with a length target (length rule, match group or
    /// matched-length group), the preview reports each member's length against its target (RoutePreview::
    /// memberLengths), and on commit the members that are short get accordion meanders on the tracks this route adds
    /// (only those), keeping clearance, until they reach the target; members without room are named in
    /// RouteChanges::tuneStatus. A bus started with it leaves `tuneGap` between its members for the meanders.
    bool tuneWhileRouting = false;
    /// With tuneWhileRouting: extra room between a bus's members (mm, on top of width + clearance, centre to centre),
    /// which is also the meanders' height limit there; 0 = automatic (track width + clearance), < 0 = packed at pitch.
    double tuneGap = 0;
};

/// One member of a routed bus (or the routed net) against its length target (tune while routing).
struct MemberLength {
    int net = -1;
    double length = 0;  // mm: while routing, the net's copper with the route; after commit, as the length tuner measures
    double target = 0;  // mm: the length rule / match group target, else the longest member (0 = no target)
    double tolerance = 0;
    bool withinTolerance = false;
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
    /// Tune while routing: each member's length after the commit's meanders, and a summary naming the members that
    /// could not reach their target ("" when the option is off or no member has a target).
    std::vector<MemberLength> memberLengths;
    std::string tuneStatus;
    bool empty() const { return removedTracks.empty() && removedVias.empty() && addedTracks.empty() && addedVias.empty(); }
};

/// The state shown while routing: the route's copper (placed corners and the head that follows the cursor), and the
/// other nets' items at their shoved positions (`hiddenTracks` / `hiddenVias` are the layout items they replace).
struct RoutePreview {
    bool active = false;
    std::string kind;          // "route", "pair", "bus", "multi" (multi-route), "drag" (a track segment), "via" (a
                               // dragged via), "corner" (a dragged corner) or "multidrag" (several tracks)
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
    /// Tune while routing (RouterOptions::tuneWhileRouting): each routed member's length so far (its other copper plus
    /// the route) against its target; empty when the option is off or no member has a target.
    std::vector<MemberLength> memberLengths;
    /// Highlight mode: what the route's copper violates (empty in the other modes, which never violate anything).
    std::vector<RouteCollision> collisions;
    /// The last moveTo() was cancelled (requestAbort): this is the preview from before it, unchanged.
    bool aborted = false;
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
    /// Starts a bus on a pad of a part: that pad and the next pads of the same part along its row (up to `count`
    /// nets, 2–16) are routed together as one bundle at track pitch (widest member width + clearance). The bus ends
    /// where it is finished; each track is then continued on its own. Vias are placed track by track.
    bool beginBus(Vec2 at, int layer, int count);
    /// Drags a track segment: it moves parallel to itself with the cursor, its neighbours follow with 45° joints,
    /// and (shove mode) other nets' copper is pushed aside.
    bool beginDrag(int trackId, Vec2 grab);
    /// Drags a via: it follows the cursor, the tracks ending on it follow with 45° joints, and (shove mode) other
    /// nets' copper is pushed aside. Vias inside a pad of their net, mesh vias and vias held by a locked track or a
    /// track running through them stay put.
    bool beginViaDrag(int viaId, Vec2 grab);
    /// Drags the corner (the joint of two tracks of one line, not on a pad or via) at the end of `trackId` nearest
    /// to `grab`: the vertex follows the cursor and the two lines meeting there follow it (straight in the free
    /// posture, 45° links otherwise), shoving or walking around as set.
    bool beginCornerDrag(int trackId, Vec2 grab);
    /// Drags several tracks together by the cursor's movement (any nets and layers). Tracks joined to them rejoin
    /// their old paths from the moved ends; ends on pads and vias get a link to the pad or via.
    bool beginMultiDrag(const std::vector<int>& trackIds, Vec2 grab);
    /// Multi-route: the nets of the pads, vias or tracks at `starts` (2–16, one per net) are routed together as one
    /// bundle at track pitch, from wherever they start (a bus from anywhere). V places a via per member, spread to
    /// via pitch, and the bundle continues on the next layer.
    bool beginMultiRoute(const std::vector<Vec2>& starts, int layer);

    /// Moves the head to the cursor and returns the preview.
    const RoutePreview& moveTo(Vec2 cursor);
    /// Cancels the head computation running now (moveTo, or the head update of addVia / setOptions) on another
    /// thread: it returns soon with the state from before it (`preview().aborted`). Thread-safe; a request made while
    /// nothing runs has no effect on later calls.
    void requestAbort();
    /// An extra cancel counter (owned by the caller, e.g. the C API's project): incrementing it cancels like
    /// requestAbort(). It must outlive the router.
    void setAbortSource(const std::atomic<unsigned>* counter);
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
    /// Where the target came from: "typed", "partner", "rule:<net>", "group:<name>" ("" for the classic path).
    std::string targetSource;
    /// The nets measured (the xSignal through series parts; empty for the classic path).
    std::vector<int> xsignalNets;
    bool coupled = false;
    int partnerNet = -1;
};

/// Meander pattern of the length tuning tool: accordion (rectangular bumps), trombone (one bump as wide as the span)
/// or sawtooth (triangular teeth, never sharper than 90°).
enum class MeanderStyle { Accordion, Trombone, Sawtooth };
/// Corners of the meander: square, mitered (45° chamfers) or round (true arcs).
enum class MeanderCorner { Square, Mitered, Round };

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
    MeanderStyle style = MeanderStyle::Accordion;
    MeanderCorner corner = MeanderCorner::Square;
    /// Drag-along: the meanders go only between the points of the selected track nearest to spanFrom and spanTo.
    bool hasSpan = false;
    Vec2 spanFrom, spanTo;
    /// Differential pair: both members are tuned together (the pattern on the pair's centre line, the members at
    /// their gap), on the stretch where the selected track and its partner's track run side by side.
    bool coupled = false;
    /// Skew (phase) tuning of one pair member to its partner's length: small bumps on the side away from the
    /// partner (height 2 × width unless maxAmplitude is given).
    bool phase = false;
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

/// Length matching of several nets at once (a routed bus, a byte lane): the nets of the given tracks are tuned to
/// the longest of them within `tolerance` mm (pad to pad through series parts, as a match group), each on its
/// longest straight track with `options`' pattern and corners. Nets in a match group or with a length rule keep
/// their own target.
struct MatchLengthsResult {
    bool ok = false;  // at least one net was lengthened
    std::string message;
    double target = 0;
    int tuned = 0, matched = 0, short_ = 0;  // lengthened / already within tolerance / still too short
    std::vector<LengthTuneResult> nets;
};
MatchLengthsResult matchTrackLengths(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                                     const LengthTuneOptions& options = {}, double tolerance = 0.1);
/// {"ok","message","target","tuned","matched","short","nets":[lengthTuneJson]}
Json matchLengthsJson(const MatchLengthsResult& r);
/// Options from {"target","maxAmplitude","spacing","x","y","apply","style":"accordion|trombone|sawtooth",
/// "corner":"square|mitered|round","fromX","fromY","toX","toY" (drag-along span),"coupled","phase"} (x and y
/// together set the near point).
LengthTuneOptions lengthTuneOptionsFromJson(const Json& j);

struct FanoutOptions {
    /// Walk around (default: nothing else moves) or shove other nets' copper out of the way.
    bool shove = false;
    /// Skip pads that already have a track or via of their net.
    bool onlyUnrouted = true;
    /// Pad centre to via centre (mm); 0 = just clear of the pad (pad half size + clearance + via radius).
    double distance = 0;
    RouterViaType viaType = RouterViaType::Through;
};

struct FanoutResult {
    bool ok = false;  // at least one pad was fanned out
    std::string message;
    int fanned = 0, skipped = 0;  // skipped: through-hole, no net / single-pin net, already routed
    std::vector<int> failedPads;  // pad numbers with no room for an escape and a via
    std::string firstProblem;
};

/// Fanout of a part: every SMD pad whose net has other pins gets a short escape track and a via (dog-bone), outward
/// along the pad's long side (diagonal for square pads such as BGA balls), placed through the interactive router so
/// it keeps every rule (walkaround by default). Each pad tries a few positions (straight out, ±45°, further out)
/// before it is reported as failed. Changes the layout.
FanoutResult fanoutComponent(PcbLayout& pcb, const Schematic& sch, int componentId, const FanoutOptions& options = {});
/// Options from {"shove","onlyUnrouted","distance","viaType":"through|blind|micro|auto"}.
FanoutOptions fanoutOptionsFromJson(const Json& j);
/// {"ok","message","fanned","skipped","failed":[pad number]}
Json fanoutJson(const FanoutResult& r);

struct ArcCornersOptions {
    /// Arc radius (mm); <= 0 = automatic (4 × track width, at least 0.5 mm). For parallel tracks of a differential
    /// pair it is the inner track's radius; the outer track turns on a concentric arc.
    double radius = 0;
    /// False: compute the result (addedTracks / removedTracks) without changing the board.
    bool apply = true;
};

struct ArcCornersResult {
    bool ok = false;  // at least one corner became an arc
    std::string message;
    int converted = 0;  // corners now arcs
    int kept = 0;       // corners that stay sharp (no room for an arc that keeps clearance)
    std::vector<Track> addedTracks;
    std::vector<int> removedTracks;
    RouteChanges changes;
    bool applied = false;
};

/// "Convert corners to arcs": every corner between two of the selected straight tracks (same net, layer and width,
/// not inside a pad or via, no third track there) becomes a true arc tangent to both, where it keeps clearance to
/// everything (radius halved once, otherwise sharp). Parallel selected tracks of a differential pair turn on
/// concentric arcs at their exact gap. Locked tracks and arcs are left alone.
ArcCornersResult convertCornersToArcs(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                                      const ArcCornersOptions& options = {});
/// {"ok","message","converted","kept","applied","addedTracks":[track],"removedTracks":[id],"changes":{…}}
Json arcCornersJson(const ArcCornersResult& r);

// ------------------------------------------------------------------------------------------- board commands

struct TeardropOptions {
    /// Tracks whose ends get teardrops (empty: every track of the board).
    std::vector<int> trackIds;
    bool pads = true, vias = true;
    /// Length of the teardrop beyond the pad / via edge, as a fraction of the pad / via size (0.3 … 3).
    double length = 1.0;
    /// Straight (the default): every fan member ends near the same point on the track. Curved: the members' ends
    /// spread along the track so their envelope (the copper outline) is a concave quadratic Bézier from inside the
    /// pad / via, tangent to the track; it lies inside the straight teardrop's outline.
    TeardropStyle style = TeardropStyle::Straight;
    /// False: compute the result without changing the board.
    bool apply = true;
};

struct BoardEditResult {
    bool ok = false;  // something was (or would be) added or changed
    std::string message;
    int added = 0;    // teardrops, vias or improved lines
    int skipped = 0;  // places without room (clearance)
    std::vector<Track> addedTracks;
    std::vector<Via> addedVias;
    std::vector<int> removedTracks;
    std::vector<int> removedVias;
    RouteChanges changes;
    bool applied = false;
};

/// Teardrops where tracks meet pads and vias of their net: a fan of tracks (Track::teardrop) from the pad / via to the
/// track, the gaps between them narrower than the track, so the copper fills the joint. Each teardrop keeps
/// clearance to everything (shortened once, otherwise left out); ends on the track are staggered so the DRC sees no
/// acute join. Tracks narrower than 90 % of the pad / via only; straight tracks only.
BoardEditResult addTeardrops(PcbLayout& pcb, const Schematic& sch, const TeardropOptions& options = {});
/// Removes the teardrops on the given tracks' ends (empty: all teardrops).
BoardEditResult removeTeardrops(PcbLayout& pcb, const std::vector<int>& trackIds = {});
/// Removes teardrops that no longer join a pad / via to a track of their net (after an edit moved the track).
int pruneTeardrops(PcbLayout& pcb, const Schematic& sch, std::vector<Track>* removed = nullptr);

struct ViaPatternOptions {
    /// Net of the vias (empty: the ground net).
    std::string net;
    /// Via pitch (mm, centre to centre); 0 = 2 mm for stitching, 1 mm for shielding.
    double pitch = 0;
    /// Shielding: distance from the track's centre line to the via centres (0 = just clear: half width + clearance
    /// + via radius).
    double offset = 0;
    /// Stitching: only inside this rectangle (empty = the whole board).
    Rect area;
    bool hasArea = false;
    bool apply = true;
};

/// Via stitching: a grid of vias of the net (through vias, board via size) wherever its pours or planes overlap on
/// two or more layers, keeping every clearance (grid points without room are skipped).
BoardEditResult stitchVias(PcbLayout& pcb, const Schematic& sch, const ViaPatternOptions& options = {});
/// Via shielding: rows of vias of the net (ground by default) on both sides of the given tracks, at `pitch` along
/// them, keeping every clearance.
BoardEditResult shieldTracks(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                             const ViaPatternOptions& options = {});

struct GlossOptions {
    /// Also search a new path for each line (walkaround on a fine grid) and keep it when it is shorter.
    bool retrace = true;
    bool apply = true;
};

/// Glossing: each line (chain of tracks between pads, vias and junctions) through the given tracks is pulled tight
/// with 45° shortcuts and (retrace) re-searched, kept when it gets shorter, keeping every rule. Arcs, locked tracks and
/// teardrops stay as they are (they end the lines).
BoardEditResult glossTracks(PcbLayout& pcb, const Schematic& sch, const std::vector<int>& trackIds,
                            const GlossOptions& options = {});

/// {"ok","message","added","skipped","applied","addedTracks","addedVias","removedTracks","removedVias","changes"}
Json boardEditJson(const BoardEditResult& r, int layerCount = 0);

/// Options from {"mode":"shove|walkaround","posture":"45|90|free","swapPosture","width","pairGap","snap"} — missing
/// fields keep their value in `base`.
RouterOptions routerOptionsFromJson(const Json& j, RouterOptions base = {});
Json routePreviewJson(const RoutePreview& p);
/// [{"net","length","target","tolerance","withinTolerance"}] (tune while routing: "memberLengths" of the preview and
/// of the commit's changes, written only when not empty).
Json memberLengthsJson(const std::vector<MemberLength>& members);
Json routeChangesJson(const RouteChanges& c);

}  // namespace sieda
