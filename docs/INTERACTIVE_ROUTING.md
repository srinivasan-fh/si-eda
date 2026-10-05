# Interactive Routing

The **Route** tool in the PCB editor draws tracks by hand. The track follows the cursor from a pad, via or track.
It either walks around everything in its way or shoves other nets' tracks and vias aside, so you can route through
a dense area without first moving the copper that is already there. The same engine routes a differential pair or
a bus as one unit, drags existing tracks and vias, lengthens a net with meanders, places blind / buried / micro vias,
rounds corners and fans out parts.

- [Using the Route tool](#using-the-route-tool)
- [Modes](#modes)
- [What stays fixed](#what-stays-fixed)
- [Rules the router keeps](#rules-the-router-keeps)
- [Rounded corners](#rounded-corners)
- [Arc tracks](#arc-tracks)
- [Vias and HDI](#vias-and-hdi)
- [Differential pairs](#differential-pairs)
- [Bus routing](#bus-routing)
- [Fanout](#fanout)
- [Dragging a segment](#dragging-a-segment)
- [Dragging a via](#dragging-a-via)
- [Length tuning](#length-tuning)
- [Core API](#core-api)
- [C API](#c-api)
- [How shoving works](#how-shoving-works)
- [Limits](#limits)
- [Responsiveness](#responsiveness)
- [Code map](#code-map)
- [Tests](#tests)

## Using the Route tool

| Action | How |
|---|---|
| Pick the Route tool | **X**, or the scribble button in the tool strip |
| Start a route | Click a pad, via or track that has a net. The route starts on the active copper layer. A click on an SMD pad of the other side switches to that side. |
| Preview | Move the mouse. The head (the last one or two segments) follows the cursor. Copper that would be shoved is shown in its new place. |
| Place corners | Click. The head is fixed and the next head starts from its end. |
| Change layer | **V** places a via (the **Via type** in the options bar) at the end of the head and continues on the next layer: the other outer layer for a through via, the neighbouring layer for blind / buried and micro vias. **⇧V** goes to the neighbouring layer the other way. The new layer becomes the active layer. |
| Finish | Click a pad, via or track of the same net (the head snaps to it), press **Enter**, or double-click. |
| Cancel | **Esc**. The board is not changed. A second **Esc** leaves the Route tool. |
| Undo | **⌘Z** undoes a finished route and every track it shoved, as one step. |

The options bar shows the router settings while the Route tool is active:

- **Shove / Walk around / Highlight**: the routing mode (see below). It also applies to dragging tracks and vias
  with the Select tool.
- **45° / 90°**: corner style. With 45° the head is a straight and a diagonal segment. With 90° it is a horizontal
  and a vertical segment. **Any angle** makes the head one straight track at any angle to the pointer (free
  posture); drags then follow at any angle too.
- **Differential pair**: the next route starts as a pair (see [Differential pairs](#differential-pairs)).
- **Via type**: Through, Blind / buried, Microvia or Auto (see [Vias and HDI](#vias-and-hdi)).
- **Rounded corners**: corners become arcs (see [Rounded corners](#rounded-corners)); **True arcs** (on by
  default) writes them as true arc tracks, off writes short straight chords.
- **Bus** and its width: the next route is a bus (see [Bus routing](#bus-routing)).

The banner at the top of the canvas shows what the router is doing, the route length so far, and why the head stopped
if it is blocked. A blocked head is outlined in amber.

Track width is the net's class width (**Board Setup → Net classes**, otherwise the board track width). Vias use the
board's via drill and diameter.

## Modes

**Shove** (default). The head is laid where you point. Tracks and vias of other nets that are too close are pushed
aside. Pushed copper pushes further copper in turn, so a whole bundle of tracks moves. If the push is not possible,
for example because a pad or the board edge is in the way, the head walks around instead. If walking around does not
reach the cursor either, the head stops at the last point where it fits.

**Walk around**. Nothing else moves. The head takes the shortest way around obstacles: an octilinear search on a
fine grid near the head, then pulled tight with 45° (or 90°) segments. If the cursor cannot be reached, the head ends
as close to it as it can.

**Highlight** (highlight collisions). Nothing moves and nothing stops the head: it goes straight where you point
(45° / 90° posture) and every track, via, pad, keep-out, plane or the board edge it violates is marked in red, with a
count in the banner. You can still place and finish the route; the DRC then reports the violations. Use it to sketch
a route through a congested area before cleaning it up, as with the "highlight collisions" mode of other tools.

In Shove and Walk around the head never makes an acute (acid-trap) corner with the board's copper: a head that would
is refused like a collision, and the router tries the other bend, walks around or stops short. A route that starts
at the free end of a track continues it without folding back.

The router recomputes the head from the state at the last placed corner every time the mouse moves. Moving back
therefore lets shoved copper spring back into place.

## What stays fixed

Shoving never moves:

- pads (SMD and through-hole);
- locked tracks (`Track::locked`, `sieda_pcb_lock_track`);
- vias inside a pad of their own net;
- the route itself, and other copper of the net being routed;
- tamper-mesh stripes and their vias;
- arcs and teardrop tracks (a teardrop whose track moved away is removed on commit);
- the board edge, mounting-hole keep-outs and plane layers (a net cannot be routed on another net's plane layer).

A track whose end sits on a pad or a junction can bend, but its end stays put. Shoving it past the obstacle has to
succeed with both ends where they are. A track that ends on a via can move that via, and the via's other tracks
follow it.

## Rules the router keeps

Every position the router accepts passes the same checks as the DRC:

- Net-to-net clearance: the design rule, raised to the IPC-2221B voltage spacing for the potential difference of the
  two nets, and to the isolation gap between galvanic domains when the board has one.
- Inside a footprint, where a track is still in its own pad, the package's pad gap applies (as in the DRC), so
  fine-pitch pads can be left.
- Edge clearance from the board outline, mounting-hole keep-outs, via-to-via and via-to-pad hole spacing.
- No via in an SMD pad unless via-in-pad (VIPPO) is set in Board Setup.
- Tamper meshes: no other copper on the mesh layers inside the secure area.

Shoved copper keeps its width and layer. It also keeps every connection it had to pads, vias and other tracks. A push
that would break a connection is refused.

Committing a route therefore adds no DRC error and no clearance warning. The tests run the DRC after every commit to
check this. The DRC can still report an unrouted connection if you finish a route in free space. A track that ends
in free space is reported as a dangling track.

## Rounded corners

With **Rounded corners** set, every corner of a route becomes an arc. The radius is 4 × the track width (at least
0.5 mm; `cornerRadius` in the options sets it in mm). An arc may use up to half of each neighbouring segment (all of
an end segment); on short segments the radius shrinks. A corner placed in the middle of a straight line does not
count as a segment end. Every arc is checked for clearance against the board as shoved; where it does not fit, the
radius is halved once, and otherwise that corner stays sharp. The preview shows the corners as they will be written.

With **True arcs** (the default in the app; `"arcCorners": true` in the options) each corner is one true arc track,
tangent to both neighbours, so the route has no kink at all. This works for single tracks, **differential pairs and
buses**: corners that the members turn together become **concentric** arcs. The radius is the innermost member's;
each outer member's radius is larger by exactly its offset, so the pair gap (or bus pitch) stays exact through the
turn. Every arc is also checked against the other members' final copper; a corner that does not fit stays sharp for
all members, so a pair never turns unevenly. A pair also turns correctly at a placed corner (a click): each member
turns at its own miter point, the inner one ends its last track early.

With **True arcs** off (and in the core by default) the arcs are cut into straight chords of at most 15° (a 45°
corner gets 3, a 90° corner 6), on single tracks only, as before.

**Convert corners to arcs** (tool strip, the curved-arrow button) turns the corners between the selected tracks
(click a track with the Select tool, ⇧-click adds; with nothing selected, every track of the board) into true arcs:
each corner between two selected straight tracks of one net, layer and width, not inside a pad or via and with no
third track there. Selected tracks of a differential pair that run side by side turn on concentric arcs. A corner
whose arc would come too close to anything gets half the radius, otherwise it stays sharp; locked tracks and arcs
are left alone. It is one undo step. Core: `convertCornersToArcs(pcb, sch, trackIds, ArcCornersOptions)`; C:
`sieda_pcb_arc_corners`.

## Arc tracks

A track can be a true circular arc (`Track::arc`, 3-point form: from `a` through `mid` to `b`). Every part of SiEDA
that reads copper treats it as an arc:

| Where | How |
|---|---|
| DRC | Clearance arc–arc, arc–segment, arc–pad (round and rectangular), arc–via, arc–hole keep-out and arc–board edge, exact (closed form: end points, the radial foot point, circle crossings); acid traps use the tangent at the arc's end; dangling ends, isolation gap, tamper mesh. |
| Connectivity and ratsnest | Arc–pad, arc–track, arc–via contact and pour islands along the arc. |
| Gerber RS-274X | `G75` multi-quadrant circular interpolation, `G02` / `G03` with `I`/`J` from the start to the centre (the file's Y axis is flipped, so a counter-clockwise arc on the board is `G02`). Files of boards without arcs are byte-identical to before. Excellon drill files are unchanged. |
| Lengths | Arc length in net lengths, length matching, the route banner and the SI channel copper graph. |
| Copper pours | Clearance to the arc itself, not its chord. |
| 3D view | The arc as short boxes (5 µm sagitta). |
| Project files | `"mid": {"x","y"}` on an arc track; files without it load as before (straight). |
| Snapshot / preview JSON | `"arc": true`, `"mx","my"` and, for drawing, `"cx","cy","radius","startAngle","sweep"` (radians, sweep > 0 turns from +x towards +y). |
| Interactive router | Arcs are obstacles measured exactly (walkaround, shove, highlight, grid search); the shove engine moves straight lines and leaves arcs where they are, like locked tracks. |

Arc geometry is in `Core/include/sieda/TrackGeometry.hpp`; for a straight track every function is exactly the
segment formula used before arcs existed, so boards without arcs give bit-identical results. The functions are
continuous (an FMA-contracted build differs by a few ulps, never by a jump); checks use tolerances. Autorouted copper
is straight: the autorouter rips up all routing first.

## Vias and HDI

The via **V** places follows **Via type**:

| Via type | Span | Size |
|---|---|---|
| Through | top to bottom (always available) | board via drill / diameter |
| Blind / buried | exactly the two layers it joins: blind when one is an outer layer, buried when both are inner | board via drill / diameter |
| Microvia | neighbouring layers only (one dielectric, IPC-2226) | microvia drill / pad from Board Setup |
| Auto | HDI boards: a microvia to a neighbouring layer, otherwise the span it joins (top to bottom = through). Without HDI: through | as above |

Blind, buried and micro vias need **HDI** on (**Board Setup → Stack-up & Impedance**) and 4 or more layers; otherwise
V reports why and places nothing. With a blind / micro / auto type on an HDI board, V steps to the neighbouring layer
towards the far side (down from the top half, up from the bottom half) and ⇧V steps the other way; a typed layer
(`sieda_router_add_via(project, layer)`) can be any layer the via type allows. A pair places two vias of the same
type side by side. The router's clearance, hole-spacing and shove checks only see a via on the layers it spans, as
the DRC, the drill files (one Excellon file per span) and the Gerbers do. Dragging a via keeps its span.

## Differential pairs

With **Differential pair** set, click a pad of either member of a pair. A pair is two nets named `X_P` / `X_N`,
`X+` / `X-`, `X_DP` / `X_DN` or `XP` / `XN`. The router starts the second member from the nearest pad of the other
net and routes both together:

- the two tracks run parallel at the pair gap, edge to edge, and the corners of the inner and outer track are offset
  so the gap stays constant;
- the gap comes from the stack-up: the coupled gap for the board's differential impedance target (**Board Setup →
  Stack-up & Impedance**), never below the clearance. `RouterOptions::pairGap` sets it explicitly;
- the width is the members' net class width;
- pointing at a pad of either member ends the pair on that pad and the nearest pad of the other member;
- **V** places two vias side by side, far enough apart for their clearance, and the pair continues on the other side;
- walkaround and shove treat the pair as one unit.

## Bus routing

With **Bus** set (and a width of 2–8 nets), click a pad of a part. The bus takes that pad and the next pads of the
same part along its row (onwards, then back the other way if the row ends first), skipping pads whose net goes
nowhere, and routes them as one bundle:

- the tracks run parallel at track pitch: the widest member's width plus the largest clearance between members;
- before the first corner the bundle starts clear of the pad row on the cursor's side, and each pad leaves straight
  out, then fans in at 45° to its lane, so neighbours never come closer than the clearance;
- members keep their order across the bundle; at a corner the bundle first runs on by its half width, so each
  member turns at its own miter point and the pitch stays exact through 45° and 90° turns;
- walkaround and shove treat the bundle as one unit, like a pair.

The bus ends where you finish it (**Enter** or double-click) with each track ending in the bundle. Continue each
track on its own from there (start a route on its end) to its pad, with vias as needed. **V** inside a bus is
refused: vias are placed track by track. Core: `InteractiveRouter::beginBus(at, layer, count)`; C:
`sieda_router_begin_bus`.

## Fanout

Select one or more parts and press the fanout button in the tool strip (the outward-arrows icon). Every SMD pad whose
net has other pins and no copper yet gets a short escape track and a via (a dog-bone): straight out along the pad's
long side for gull-wing and QFN pads, diagonally for square pads such as BGA balls. Each escape is placed through the
router in walkaround mode, so it keeps every rule and moves nothing; a pad tries straight out, ±45° and up to 1 mm
further out before it is reported as without room. The via is the Route tool's **Via type**. Fanout is one undo step.
Core: `fanoutComponent(pcb, sch, componentId, FanoutOptions)`; C: `sieda_pcb_fanout`.

## Dragging a segment

With the **Select** tool (**V**), press on a track and drag. The segment moves parallel to itself with the cursor and
its neighbours follow with 45° joints. Where an end of the segment sits on a pad, a via or a junction, a 45° leg
joins it to the moved segment. With **Shove** set in the options bar, other nets' copper is pushed aside; with
**Walk around** the segment stops where it would collide. Release to drop it (one undo step); **Esc** while dragging
puts everything back. Locked tracks cannot be dragged: the drag pans the view instead. A press on a pad still moves
the footprint, and a click without dragging still selects.

Core: `InteractiveRouter::beginDrag` (C: `sieda_router_begin_drag`).

## Dragging a corner

Press on a track with the **Select** tool close to its end (within two track widths) where it meets the next track
of its line, and drag: the corner follows the pointer and both tracks follow it. With **Any angle** they stay two
straight tracks at any angle; otherwise each rejoins its old path with 45° / 90° links, cutting a corner where that is
shorter and makes no acute corner. Shove and walk around work as for a segment drag. A corner on a pad or via, or
where a third track or a locked track meets, is not dragged (the press drags the segment instead).

Core: `InteractiveRouter::beginCornerDrag(trackId, grab)` (C: `sieda_router_begin_corner_drag`); preview kind
`corner`.

## Dragging several tracks

Select tracks (click, ⇧-click adds) and press on one of them to drag them all together by the pointer's movement,
whatever their nets and layers. At each end of the selection the next track of the line rejoins its old path from
the moved end; an end on a pad or via gets a link to it. One undo step; shove and walk around as set.

Core: `InteractiveRouter::beginMultiDrag(trackIds, grab)` (C: `sieda_router_begin_multi_drag`); preview kind
`multidrag`.

## Multi-route

With the Route tool, **⇧-click** pads, vias or tracks of different nets to pick them (each shows a ring; ⇧-click
again drops one, **Esc** clears), then click the last one without ⇧: all the picked nets route together as one
bundle at track pitch, like a bus but from anywhere (they need not be a row of one part). The bundle leaves towards
the pointer along the nearest of the eight directions, each member joining its lane with a 45° lead. **V** places a
via for every member at once: the members fan out to via pitch (via diameter + clearance, and the hole-to-hole
spacing), and the bundle continues on the next layer. A bus started from a pad row (**Bus**) takes vias the same way
after its first corner.

Core: `InteractiveRouter::beginMultiRoute(starts, layer)` (C: `sieda_router_begin_multi`); preview kind `multi`.

## Dragging a via

Press on a via with the **Select** tool and drag. The via follows the cursor and every track that ends on it follows
too: each rejoins its old path with a 45° link, cutting a corner where that is shorter and makes no acute corner.
Shove mode pushes other nets' tracks and vias out of the way (recursively, as for a route); walkaround stops the via
at the last position that fits. These vias stay put: a via inside a pad of its own net, tamper-mesh vias, a via with
a locked track on it, and a via that a track runs straight through.

Core: `InteractiveRouter::beginViaDrag` (C: `sieda_router_begin_via_drag`). The preview has kind `via`.

## Teardrops

**Teardrops** (tool strip, drop icon) adds teardrops where the selected tracks (or, with none selected, every
track) meet pads and vias of their net; pressing it again on tracks that have them removes them. A teardrop is a
fan of short tracks of the track's width (`Track::teardrop`, saved as `"teardrop": true`) from inside the pad or via
onto the track, spaced closer than the track width so the copper is solid; it reaches as far beyond the pad edge as
the pad / via is wide (option `length`, 0.3–3 × the size). Each teardrop keeps clearance to everything: one that does
not fit is shortened to half once, otherwise left out ("without room"). Their ends on the track are 0.02 mm apart,
so the DRC sees no acute angle and no dangling end. Teardrops are ordinary copper for DRC, connectivity, Gerber,
3D and length (the track length the tuner measures excludes nothing; teardrops sit at the pads).

**Auto teardrops** (Route tool options) adds them on every finished route. Teardrops whose track was moved away
(dragged, shoved, deleted) are removed on the next commit.

Core: `addTeardrops`, `removeTeardrops`, `pruneTeardrops`; C: `sieda_pcb_teardrops`; router option `"teardrops"`.

## Via stitching and shielding

**Via stitching** (grid icon) places ground vias on a grid (2 mm pitch, the board via size, through vias) wherever
ground pours or planes overlap on two or more layers, inside the board and keeping every clearance; grid points
without room are skipped. **Via shielding** (shield icon) places rows of ground vias on both sides of the selected
tracks at 1 mm pitch, just clear of the track (half width + clearance + via radius), arcs included. Vias that reach no
ground pour yet are counted in the message (pour the ground afterwards). Both are one undo step.

Core: `stitchVias`, `shieldTracks` (`ViaPatternOptions`: net, pitch, offset, area); C: `sieda_pcb_stitch_vias`,
`sieda_pcb_shield_tracks`.

## Glossing and loop removal

**Gloss** (wand icon) pulls the routes through the selected tracks (all routes with none selected) tight: each line
(a chain of tracks between pads, vias and junctions) gets 45° shortcuts where they keep clearance and make no acute
corner, then is searched again (walkaround on the router's grid) and the shorter result is kept. Arcs, locked tracks
and teardrops end a line and stay. A line that cannot get shorter is left exactly as it was.

**Remove loops** (Route tool options, on by default): when a finished route joins pads of its net that an older
path already joined, the older tracks that were needed before but are redundant now are removed with the route, as
are vias they leave unconnected (one undo step). Spare copper that was already redundant before stays. Nets with a
pour or plane are left alone, and so are nets with more than 600 tracks.

Core: `glossTracks`, `RouterOptions::removeLoops`; C: `sieda_pcb_gloss`, router option `"removeLoops"`.

## Length tuning

The **Tune length** tool (**T**, the waveform button in the tool strip) lengthens a net with meanders:

1. Click a routed track. The meanders are placed near the click first, then on the net's other straight tracks,
   longest first. Or **drag along the track**: the meanders go only between where you pressed and the pointer,
   and the preview follows the pointer (drag-along tuning); release keeps the preview, **Enter** writes it.
2. The target starts at the longest member of the net's matched-length group: the other member of its differential
   pair (intra-pair skew) or the longest net of its bus (`DQ0…DQ7`, `ADDR…`, see `lengthGroups`). A net outside any
   group starts at its own length plus 1 mm. Type another target in the options bar, or **Match Group** to go back.
3. **Amplitude** limits the meander height (empty: 2 mm). **Spacing** sets the gap between meander legs, edge to
   edge (empty: three track widths centre to centre; never closer than the clearance).
4. The canvas shows the meanders before anything changes, and the banner reads the net's length before and after,
   the target, the remaining difference and the tolerance (the group's tolerance, or 0.01 mm). The preview is
   outlined in blue when it reaches the target, amber when the free space runs out first.
5. **Enter** or **Apply Tuning** writes it (one undo step). **Esc** drops it.

**Pattern** and **Meander corners** in the options bar shape the meanders:

| Pattern | Shape |
|---|---|
| Accordion | Rectangular bumps side by side, one pitch apart (the default). |
| Trombone | One loop as wide as the stretch; its height grows to the target. |
| Sawtooth | Triangular teeth two pitches wide, never sharper than 90° (no acid trap). |

Corners are **Square**, **Mitered** (45° chamfers) or **Round** (true arcs). For every pattern the number of bumps is
the fewest that can reach the target at the allowed height, and then the height is solved exactly, so the net ends
within 0.01 mm of the target whatever the corners take off.

**Coupled** tunes a differential pair as one: click a track of either member where the two run side by side; the
pattern is laid on the pair's centre line and both members follow it at their gap (round corners are concentric
arcs), so each member gains exactly the same length and the gap stays exact everywhere. **Phase** tunes the skew of a
pair: the clicked member is brought to its partner's length with small bumps (2 × the track width unless an
amplitude is given) on the side away from the partner, so the coupling is kept.

**Length rules and match groups** (the **Length Rules** button of the Tune tool, also in the project file):

- A *length rule* gives a net a target length ± tolerance (a net class's length constraint).
- A *match group* lists nets whose lengths must match the longest of them within a tolerance (an xSignal class).
- Lengths are measured as **xSignals**: from pad to pad, through two-pin series parts (resistors, capacitors,
  inductors, fuses, ferrite beads) whose both pins are on signal nets; a net with a series termination resistor is
  measured from the driver's pad through the resistor to the receiver's pad. Vias add nothing (as everywhere else).
- With no typed target, the Tune tool tunes to the net's rule, else its match group's longest member, else (as
  before) the pair / bus group. The preview reports where the target came from (`targetSource`) and the nets
  measured.
- The DRC reports a routed net outside its rule or group tolerance (`DRC_LENGTH`, warning).
- The panel lists every rule and group with a **gauge** per net (green inside the tolerance, amber short, red long).
  A gauge also shows under the banner while tuning and while routing a net that has a target.

Every meander keeps clearance to other nets, to the net's other pads and to the board edge. While routing, the banner
also shows the net's whole length against its group's target (`netLength` / `targetLength` in the preview).

Core: `tuneTrackLength(pcb, sch, trackId, LengthTuneOptions)` with `apply = false` for the preview (`style`,
`corner`, `hasSpan` / `spanFrom` / `spanTo`, `coupled`, `phase`); targets in `LengthRules.hpp` (`xSignalOf`,
`xSignalLength`, `lengthTargetFor`, `lengthTargetsJson`); C: `sieda_router_tune(project, track_id, options_json)` (the
older `sieda_router_tune_length` still works), `sieda_pcb_set_length_rule`, `sieda_pcb_set_match_group`,
`sieda_length_targets_json`. Plain accordion tuning without rules is exactly as before. Board-wide pair and bus
matching after Auto Route is unchanged (`tuneLengths`, **Board Setup → Stack-up & Impedance**).

## Core API

`Core/include/sieda/InteractiveRouter.hpp`:

```cpp
InteractiveRouter router(project.pcb, project.schematic);
RouterOptions opt;                       // mode Shove, posture Diagonal45, width 0 = net class
router.setOptions(opt);
router.beginRoute(padPosition, layer);   // or beginPair(...), beginDrag(trackId, grab)
const RoutePreview& p = router.moveTo(cursor);   // p.head, p.shovedTracks, p.hiddenTracks, p.blocked, p.status
router.fixHead();                        // place corners
router.addVia();                         // through via, continue on the other outer layer
RouteChanges ch = router.commit();       // removed tracks / vias (with geometry), ids of added ones
router.cancel();
```

The router copies the board when a route begins and changes the `PcbLayout` only in `commit()`. If the layout or
the placement changed in the meantime, `commit()` refuses with an error and leaves the board alone. `RouteChanges`
lists exactly what was removed and added, so a caller can undo a commit without a full snapshot. Results are
deterministic: the same board and the same sequence of calls give the same copper.

## C API

All functions are in `sieda_c.h`. Each project has one route session.

| Function | Purpose |
|---|---|
| `sieda_router_begin(project, options_json, x, y, layer)` | Start a route; returns the preview JSON |
| `sieda_router_begin_pair(...)` | Start a differential pair |
| `sieda_router_begin_drag(project, options_json, track_id, x, y)` | Drag a segment |
| `sieda_router_move(project, x, y)` | Move the head |
| `sieda_router_fix(project)` | Place corners |
| `sieda_router_add_via(project, to_layer)` | Via and layer change (`-1` = the default next layer for the via type, `-2` = the neighbouring layer the other way) |
| `sieda_router_set_options(project, options_json)` | Change mode / posture during a route |
| `sieda_router_commit(project)` | Write the route; returns the changes |
| `sieda_router_cancel(project)`, `sieda_router_active(project)` | Session control |
| `sieda_router_abort(project)` | Cancel the head update running on another thread (lock-free, thread-safe) |
| `sieda_router_begin_via_drag(project, options_json, via_id, x, y)` | Drag a via |
| `sieda_router_begin_bus(project, options_json, x, y, layer, count)` | Start a bus of `count` nets from a pad row |
| `sieda_pcb_fanout(project, component_id, options_json)` | Fanout: escape + via per pad (`{"shove","onlyUnrouted","distance","viaType"}`) |
| `sieda_router_tune(project, track_id, options_json)` | Length tuning with preview: `{"target","maxAmplitude","spacing","x","y","apply","style","corner","fromX","fromY","toX","toY","coupled","phase"}` |
| `sieda_router_tune_length(project, track_id, target_mm, max_amplitude_mm)` | Length tuning (applies at once) |
| `sieda_pcb_arc_corners(project, track_ids_json, options_json)` | Convert corners to arcs: `[id, …]`, `{"radius","apply"}` |
| `sieda_router_begin_corner_drag(project, options_json, track_id, x, y)` | Drag a corner |
| `sieda_router_begin_multi_drag(project, options_json, track_ids_json, x, y)` | Drag several tracks together |
| `sieda_router_begin_multi(project, options_json, points_json, layer)` | Multi-route `[{"x","y"}, …]` |
| `sieda_pcb_teardrops(project, track_ids_json, options_json)` | Add (or `"remove"`) teardrops |
| `sieda_pcb_stitch_vias(project, options_json)` | Via stitching of a net's pours |
| `sieda_pcb_shield_tracks(project, track_ids_json, options_json)` | Via shielding along tracks |
| `sieda_pcb_gloss(project, track_ids_json, options_json)` | Gloss / retrace routes |
| `sieda_pcb_set_length_rule(project, net, target_mm, tolerance_mm)` | Length rule of a net (target 0 removes it) |
| `sieda_pcb_set_match_group(project, group_json)` | Match group `{"name","nets":[…],"tolerance"}` (fewer than two nets removes it) |
| `sieda_length_targets_json(project)` | Rules and groups with their members' xSignal lengths |
| `sieda_pcb_lock_track`, `sieda_pcb_remove_track`, `sieda_pcb_remove_via` | Track editing |

Options JSON: `{"mode":"shove"|"walkaround"|"highlight", "posture":"45"|"90"|"free", "swapPosture":bool, "width":mm,
"pairGap":mm, "snap":bool, "viaType":"through"|"blind"|"micro"|"auto", "cornerRadius":mm (0 sharp, < 0 auto),
"arcCorners":bool}`. Fields that are left out keep their value.

Preview JSON: `active`, `kind` (`route` / `pair` / `drag` / `via`), `status`, `blocked`, `reachedTarget`, `nets`,
`layer`, `width`, `gap`, `endX`, `endY`, `length`, `netLength`, `targetLength`, `placed`, `head`, `vias`,
`shovedTracks`, `shovedVias`, `hiddenTracks`, `hiddenVias`, and `error` when the call was refused. Vias carry
`fromLayer`, `toLayer` and `kind` as in the snapshot. To draw a preview, draw the board without `hiddenTracks` /
`hiddenVias`, then `shovedTracks` / `shovedVias`, then the route (`placed`, `head`, `vias`).

## How shoving works

1. The head is added to a copy of the board as fixed copper.
2. For each item that is too close to it, the router takes the item's whole *line*. A line is the chain of segments
   of one net, layer and width between pads, vias, junctions and locked segments. The line is walked around the
   octagonal hull of the pusher. The hull is the pusher grown by the clearance and both half widths, with edges at
   0°, 45° and 90°. The router takes whichever way round is shorter and does not run into fixed copper. If the moved
   line then hits fixed copper, it is walked around that too. Mounting-hole keep-outs and tamper-mesh areas are
   walked around the same way (their own octagonal hull) instead of stopping the push.
3. The moved stretch is pulled tight with 45° shortcuts that stay clear. Acute corners are cut, and segments that
   another line of the same net already covers are dropped.
4. A via is moved to the nearest side of the pusher's hull that is free of fixed copper, or one via pitch further
   out on the same sides when all of those are taken. Its tracks follow with 45° joints.
5. Every moved item pushes in turn, breadth first, until nothing collides. The step fails if a fixed item is hit, a
   connection would break, or more than 120 shoves are needed.
6. Every moved item is checked against the full rule set.
7. Post-shove optimiser: every join that the step made between track ends of one net is checked like the DRC's
   acid-trap rule (an angle under 89° that no pad or via covers). Two lines that were walked round the same hull and
   now run over each other (0° joins) are merged: the longer is trimmed to start where the shorter ends, or the
   covered one is removed. Other acute joins are chamfered with a short cut (one, two, half or four track widths)
   where the cut keeps clearance and every connection. If an acute join is still left, the step fails like a
   blocked push, so the router falls back to walking around or stops short.

Violations that the board already had before the route began are tolerated on the items that carry them. A push
never fails because of an old DRC problem elsewhere, and it never creates a new one.

## Limits

- Arcs are written by rounded corners, Convert corners to arcs and the commands below; the shove engine does not
  bend or move an existing arc (it routes around it), a segment drag refuses an arc, and a via with an arc on it
  stays put. Chord corners (True arcs off) are single-track only. Free-angle routing is posture `free` (core and C
  API only).
- The shove engine moves tracks and vias, not footprints.
- Shoving a meandered (length-tuned) track can flatten part of the meander. Run length tuning again afterwards.
- Highlight mode lets you commit copper that violates the rules (the DRC reports it); the other modes never do.
- The walkaround search covers the area around the head: about 4 mm, or ¾ of the head length, beyond the start and
  the cursor. A detour further away needs a corner placed on the way.
- Meanders go on straight tracks (not on arcs). Coupled tuning needs the two members side by side on one straight
  stretch; drag-along works on the track pressed (not across a corner). xSignals follow two-pin series parts only
  (not through ICs or multi-pin resistor networks) and measure track length (vias add nothing).
- A bus (or multi-route) ends in the bundle; each track is finished to its pad on its own. Members all use the
  widest member's width. Bundle vias need a placed corner first (they are laid across the bundle's direction).
- Teardrops are straight-track fans (no curved outline) on straight tracks, at pads and vias only (not at T
  junctions between tracks); a track as wide as 90 % of the pad gets none. Stitching uses one via size on a square
  grid (no hexagonal or edge-of-pour patterns). Glossing works line by line on one layer (it does not move vias).
  Loop removal skips poured nets.
- Fanout covers SMD pads only (through-hole pads already reach every layer) and walks around (it never shoves).
- The Select-tool drag picks the via, then the track on the active layer, under the pointer (near a track's end: its
  corner; on one of several selected tracks: all of them); a press on a pad moves the footprint as before. Arcs are
  not dragged.
- Auto Route rips up all routing, locked tracks included.
- A blocked head on a dense board still takes up to about 50 ms to compute (the router searches for the furthest
  position that fits; p99 under 30 ms). It never stalls the window (see [Responsiveness](#responsiveness)), but the
  head lags the cursor by that much there. A shove drag of a segment into a crowded spot reaches 40 ms (p99).

## Responsiveness

The head is computed off the main thread. While one update runs, newer mouse positions replace each other and only
the newest runs next (latest wins), so the head never falls behind a queue of stale positions and the canvas keeps
drawing and scrolling. A click, **V**, **Enter**, **Esc** or an option change first cancels the update in flight
(`InteractiveRouter::requestAbort`, C: `sieda_router_abort`, lock-free) and then acts on the cursor position of the
click itself, so a step never waits for a slow head and never uses a stale one. A cancelled update restores the
state from before it and reports `aborted` in its preview; the app drops such results, and results of a session that
has since ended.

Measured with `sieda_router_bench` (Release, gcc, one thread: `SIEDA_ROUTER_SERIAL=1`, a 4-core Linux container
shared with other jobs, so ±20 %): a generated board of 422 parts, 512 nets and 8 layers (159 × 120 mm, 3341 tracks,
1176 vias, autorouted). Routes start on random pads (and drags on random tracks), the head moves to random points
within 12 mm (1.5 mm for drags). "Before" is the same benchmark, same board and same machine load, on the previous
core:

| Update | Median | p90 | p99 | Worst | Before: p99 / worst |
|---|---|---|---|---|---|
| Route head, Shove | 4.1 ms | 13.7 ms | **29.4 ms** | 49.7 ms | 52.3 ms / 97.2 ms |
| Route head, Walk around | 4.0 ms | 10.5 ms | **19.5 ms** | 21.9 ms | 31.7 ms / 34.2 ms |
| Segment drag, Shove | 0.12 ms | 7.9 ms | 40.7 ms | 40.7 ms | 65.8 ms / 65.8 ms |
| Segment drag, Walk around | 0.04 ms | 0.07 ms | 0.16 ms | 0.16 ms | 0.19 ms / 0.19 ms |
| Starting a route (board snapshot) | 5.6 ms | 6.5 ms | 14.7 ms | 14.7 ms | 6.1 ms / 6.1 ms |
| Slow update (into another net's pad, far away) | 11.6 ms | 33.3 ms | 33.8 ms | 33.8 ms | 70.5 ms / 70.5 ms |
| The same, cancelled after 1 ms (time to return, incl. the 1 ms) | 5.3 ms | 6.4 ms | 9.8 ms | 9.8 ms | 11.9 ms / 11.9 ms |

The 923-part board of [ROUTING.md](ROUTING.md) (32 clusters, FPGA and 4 BGAs, 8 layers, 234 × 176 mm, 8688 tracks,
2996 vias; `--clusters 32 --fpga 1 --seed 3`) scales with it: route head p99 28.8 ms (Shove) and 26.7 ms (Walk
around), segment drag p99 28.1 ms, a board snapshot 15 ms.

What made it faster (every result is the same as before, bit for bit: the tests and the recorded router results are
unchanged):

- The clearance checks query a per-layer track grid, and the tracks a shove step adds and drops again are kept out
  of the queries (a live list of the overlay's added copper instead of a scan over hundreds of dead ones).
- Bounding-box gaps reject far pads and tracks before the exact distance, with a margin far above rounding; the
  board-edge check is skipped where a straight track is well inside a rectangular board.
- The walkaround grid search stops as soon as it has found the nearest reachable cell of an unreachable target (a
  flood fill knows the region beforehand), and its open list and obstacle marking avoid indirect calls.
- With spare cores (load average below cores − 1.5) the candidate heads and the two halves of each bisection step
  ("how far does the head get") run side by side and are then taken in the sequential order, so the outcome never
  depends on timing; on a busy machine everything runs on one thread. `SIEDA_ROUTER_SERIAL=1` forces one thread,
  `SIEDA_ROUTER_PARALLEL=1` always uses threads (the tests run both ways).

`./build/sieda_router_bench [--clusters 16 --layers 8 --seed 1 --moves 300] [--board routed.json] [--slowest 10]`
reproduces it (the board takes about a minute to autoroute first; `--board` keeps it for the next run); it also
prints the per-update CPU time.

## Code map

| What | Where |
|---|---|
| Router, shove engine, walkaround search, length tuning, arc corners | `Core/src/InteractiveRouter.cpp`, `Core/include/sieda/InteractiveRouter.hpp` |
| Arc track geometry (exact distances, lengths, bounds, tangents) | `Core/include/sieda/TrackGeometry.hpp`, `Core/src/TrackGeometry.cpp` |
| C API additions (`sieda_pcb_arc_corners`, …) | `Core/src/sieda_c_routing.cpp` (project handle in `Core/src/SiedaProjectInternal.hpp`) |
| App: arc drawing / hit testing, track selection and commands | `SiEDA/Models/TrackArcs.swift`, `SiEDA/Bridge/EDAEngine+Routing.swift`, `SiEDA/App/DesignStore+Routing.swift` |
| Locked tracks (`Track::locked`, saved as `"locked": true`) | `Core/include/sieda/Pcb.hpp`, `Core/src/Project.cpp` |
| C API | `Core/include/sieda/sieda_c.h`, `Core/src/sieda_c.cpp` (`sieda_router_*`) |
| Swift bridge | `SiEDA/Bridge/EDAEngine.swift` (`routerBegin`, `routerMove`, `routerFix`, `routerAddVia`, `routerCommit`, …) |
| App state | `SiEDA/App/DesignStore.swift` (`routePreview`, `beginRoute`, `moveRoute` (off the main thread, latest wins), `moveRouteNow`, `finishRoute`, `beginTrackDrag`, `beginViaDrag`, `tuneSession`, `beginTune`, `applyTune`, …) |
| Route and Tune tools, Select-tool drags, preview drawing | `SiEDA/Views/PCB/PCBEditorView.swift` |
| Benchmark | `Core/tests/bench/router_bench.cpp` (`sieda_router_bench`) |

## Tests

Core (`Core/tests/core_tests.cpp`):

| Test | Checks |
|---|---|
| `router_shoves_a_chain_of_three_tracks` | A route pushes three parallel tracks in a chain. All lanes stay connected and every net pair keeps the clearance. DRC adds no error. |
| `router_is_deterministic` | The same session twice gives identical previews and copper. |
| `router_walkaround_finds_a_path` | Walkaround routes round a wall without moving it, in 45° segments. |
| `router_refuses_to_shove_pads_and_locked_tracks` | The head stops short of another net's pad. A locked track is walked around. The same track unlocked is shoved. |
| `router_changes_layer_with_vias` | Two vias cross a locked wall. Single-sided boards refuse vias. |
| `router_routes_differential_pairs_at_the_pair_gap` | Both members routed and connected. The coupled run is exactly at the gap, and nowhere is closer. |
| `router_drags_a_segment_and_shoves` | A dragged segment pushes two neighbours. Cancel leaves the board alone. Locked tracks cannot be dragged. |
| `router_commit_refuses_a_stale_board` | A board changed during a route is not overwritten. |
| `router_tunes_length_with_meanders` | A net reaches its target length within 0.05 mm and stays DRC clean. |
| `router_drags_a_via_and_shoves` | A dragged via shoves another net's track, its tracks follow; walkaround stops short; in-pad vias refuse. |
| `router_previews_length_tuning` | A preview leaves the board alone, puts the meander near the click at the asked spacing, and applying writes exactly the preview. |
| `router_tunes_pair_skew_and_reports_the_target` | Tuning a pair member matches its partner; the route preview reports net and target length. |
| `c_api_router_drag_and_tune` | Via drag and `sieda_router_tune` through the C API. |
| `router_shoved_copper_adds_no_drc_warning` | 160 routes, segment drags and via drags with shove on four autorouted boards (2 and 4 layers): no commit adds a DRC error, a clearance warning or an acute-angle warning (before the optimiser, 15 of 160 commits added acute corners). |
| `router_shoves_lines_around_hole_keepouts` | A shoved line walks round a mounting-hole keep-out instead of stopping the head. |
| `router_places_blind_buried_and_micro_vias` | Microvia, buried microvia, blind and through vias on a 4-layer HDI board with the asked spans and sizes; a microvia refuses a non-neighbouring layer; Auto and the no-HDI cases. |
| `router_rounds_corners` | 45° corners become chord arcs (every join ≥ 165°), written exactly as previewed and DRC clean; a via inside a corner keeps that corner sharp. |
| `track_arcs_measure_exactly` | Arc distances (point, segment, arc, rectangle), lengths, bounds and tangents against a dense polyline on 300 random arcs; degenerate arcs are straight; straight tracks use exactly the segment formulas. |
| `drc_checks_arc_tracks_exactly` | Copper next to an arc's bulge is reported although the chord is far; copper between chord and arc is clear; an arc that bulges past the board edge; connectivity and lengths along the arc. |
| `arc_tracks_export_save_and_draw` | Gerber `G75`/`G02`/`G03` with exact `I`/`J`, none without arcs; project files keep arcs and old files load straight; the snapshot's centre, radius and angles. |
| `router_writes_true_arc_corners` | Arc corners tangent everywhere (every join 180°), written as previewed, DRC clean; a via in a corner keeps it sharp. |
| `router_turns_pairs_and_buses_on_concentric_arcs` | A pair (with a turn at a placed corner) and a four-net bus turn on concentric arcs at their exact gap / pitch; DRC clean. |
| `convert_corners_to_arcs_command` | Preview changes nothing; corners become arcs, the net gets shorter, DRC clean; a via halves the radius; a 0.2 mm jog stays sharp. |
| `router_respects_arc_tracks` | A route past an arc's bulge keeps clearance to the arc in walkaround and shove, and never moves the arc. |
| `c_api_arc_corners` | Arc corners and `sieda_pcb_arc_corners` through the C API. |
| `length_tuning_patterns_and_corners` | Accordion, trombone and sawtooth with square, mitered and round corners: within 0.01 mm of the target, written as previewed, DRC clean, no acute corner. |
| `length_tuning_drags_along_the_track` | Drag-along: meanders only between the two points; a short stretch takes what fits. |
| `length_tuning_couples_pairs_and_tunes_phase` | Coupled pair tuning: both members gain the same length, the gap stays exact (square and round); phase tuning matches the partner with bumps away from it. |
| `length_rules_match_groups_and_xsignals` | xSignal through a series resistor measured pad to pad; match group and length rule targets, `DRC_LENGTH`, saved and loaded. |
| `c_api_length_tuning_and_rules` | Rules, groups, targets JSON and the new tuning options through the C API. |
| `router_drags_a_corner` | A corner follows the pointer at any angle (two straight tracks) and with 45° links; a corner on a pad is refused; DRC clean, connected. |
| `router_drags_several_tracks_together` | Two nets' tracks move together, links to the pads, both nets connected, DRC clean. |
| `router_routes_at_any_angle` | Free posture: one straight track at an arbitrary angle onto the pad. |
| `router_multi_routes_nets_with_vias` | Three scattered nets route as one bundle; V places three vias at via pitch and the bundle continues on the bottom layer; DRC clean. |
| `c_api_corner_multi_drag_and_multi_route` | Corner drag, multi drag and multi-route through the C API. |
| `teardrops_on_pads_and_vias` | Six teardrops on a pad → via → via → pad route; DRC clean (no acute angle, no dangling end), saved, in the Gerber, removed exactly; auto teardrops on commit; pruning. |
| `via_stitching_and_shielding` | Shielding rows clear of the track; stitching grid inside the area on two GND pours; DRC clean. |
| `gloss_pulls_routes_tight` | A detour is pulled > 10 mm shorter, connected, DRC clean; a tight or locked route stays. |
| `router_removes_loops_on_commit` | A new direct route removes the old detour, its stub and its two vias; without the option all stays. |
| `c_api_board_commands` | Teardrops, gloss, shielding, stitching and the router options through the C API. |
| `router_head_update_can_be_cancelled` | A cancelled update leaves the router exactly as before; the next one matches a router that was never cancelled; a request while idle changes nothing. |
| `router_routes_a_bus_together` | A bus of four SOIC pins ends at track pitch in pin order; each track is then finished to its pad; DRC clean. |
| `router_bus_turns_corners_at_pitch` | A bus through a 90° turn keeps the clearance between members, and is packed at it. |
| `router_fans_out_a_part` | Four wired pins get an escape and a via, four unconnected pins are skipped, a second fanout does nothing. |
| `router_highlights_collisions` | Highlight mode lists the three lanes a straight head crosses, moves nothing, and commits as asked. |
| `router_keeps_an_autorouted_board_drc_clean` | 30 pseudo-random routes and drags with shove on an autorouted board, with DRC after every commit. |
| `c_api_router` | The C API end to end (`Core/tests/c_api_test.c`). |

App (`SiEDATests`): `testInteractiveRouterRoutesAndCommits` routes and commits through `EDAEngine`;
`InteractiveRoutingStoreTests` drags a track and a via through `DesignStore` (one undo step each); previews, applies
and undoes a length tuning; checks head updates run off the main thread with the newest position winning and a click
acting on its own position; places microvias on an HDI board; writes rounded corners; fans out a part and starts a
bus; and lists Highlight-mode collisions.
