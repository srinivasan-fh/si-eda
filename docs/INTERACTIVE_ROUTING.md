# Interactive Routing

The **Route** tool in the PCB editor draws tracks by hand. The track follows the cursor from a pad, via or track.
It either walks around everything in its way or shoves other nets' tracks and vias aside, so you can route through
a dense area without first moving the copper that is already there. The same engine routes a differential pair as
one unit, drags an existing track segment, and lengthens a net with meanders.

- [Using the Route tool](#using-the-route-tool)
- [Modes](#modes)
- [What stays fixed](#what-stays-fixed)
- [Rules the router keeps](#rules-the-router-keeps)
- [Differential pairs](#differential-pairs)
- [Dragging a segment](#dragging-a-segment)
- [Length tuning](#length-tuning)
- [Core API](#core-api)
- [C API](#c-api)
- [How shoving works](#how-shoving-works)
- [Limits](#limits)
- [Code map](#code-map)
- [Tests](#tests)

## Using the Route tool

| Action | How |
|---|---|
| Pick the Route tool | **X**, or the scribble button in the tool strip |
| Start a route | Click a pad, via or track that has a net. The route starts on the active copper layer. A click on an SMD pad of the other side switches to that side. |
| Preview | Move the mouse. The head (the last one or two segments) follows the cursor. Copper that would be shoved is shown in its new place. |
| Place corners | Click. The head is fixed and the next head starts from its end. |
| Change layer | **V** places a through via at the end of the head and continues on the other outer layer. The new layer becomes the active layer. |
| Finish | Click a pad, via or track of the same net (the head snaps to it), press **Enter**, or double-click. |
| Cancel | **Esc**. The board is not changed. A second **Esc** leaves the Route tool. |
| Undo | **⌘Z** undoes a finished route and every track it shoved, as one step. |

The options bar shows the router settings while the Route tool is active:

- **Shove / Walk around / Highlight**: the routing mode (see below). It also applies to dragging tracks and vias
  with the Select tool.
- **45° / 90°**: corner style. With 45° the head is a straight and a diagonal segment. With 90° it is a horizontal
  and a vertical segment.
- **Differential pair**: the next route starts as a pair (see [Differential pairs](#differential-pairs)).

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

## Dragging a segment

With the **Select** tool (**V**), press on a track and drag. The segment moves parallel to itself with the cursor and
its neighbours follow with 45° joints. Where an end of the segment sits on a pad, a via or a junction, a 45° leg
joins it to the moved segment. With **Shove** set in the options bar, other nets' copper is pushed aside; with
**Walk around** the segment stops where it would collide. Release to drop it (one undo step); **Esc** while dragging
puts everything back. Locked tracks cannot be dragged: the drag pans the view instead. A press on a pad still moves
the footprint, and a click without dragging still selects.

Core: `InteractiveRouter::beginDrag` (C: `sieda_router_begin_drag`).

## Dragging a via

Press on a via with the **Select** tool and drag. The via follows the cursor and every track that ends on it follows
too: each rejoins its old path with a 45° link, cutting a corner where that is shorter and makes no acute corner.
Shove mode pushes other nets' tracks and vias out of the way (recursively, as for a route); walkaround stops the via
at the last position that fits. These vias stay put: a via inside a pad of its own net, tamper-mesh vias, a via with
a locked track on it, and a via that a track runs straight through.

Core: `InteractiveRouter::beginViaDrag` (C: `sieda_router_begin_via_drag`). The preview has kind `via`.

## Length tuning

The **Tune length** tool (**T**, the waveform button in the tool strip) lengthens a net with accordion meanders:

1. Click a routed track. The meanders are placed near the click first, then on the net's other straight tracks,
   longest first.
2. The target starts at the longest member of the net's matched-length group: the other member of its differential
   pair (intra-pair skew) or the longest net of its bus (`DQ0…DQ7`, `ADDR…`, see `lengthGroups`). A net outside any
   group starts at its own length plus 1 mm. Type another target in the options bar, or **Match Group** to go back.
3. **Amplitude** limits the meander height (empty: 2 mm). **Spacing** sets the gap between meander legs, edge to
   edge (empty: three track widths centre to centre; never closer than the clearance).
4. The canvas shows the meanders before anything changes, and the banner reads the net's length before and after,
   the target, the remaining difference and the tolerance (the group's tolerance, or 0.01 mm). The preview is
   outlined in blue when it reaches the target, amber when the free space runs out first.
5. **Enter** or **Apply Tuning** writes it (one undo step). **Esc** drops it.

Every meander keeps clearance to other nets, to the net's other pads and to the board edge. While routing, the banner
also shows the net's whole length against its group's target (`netLength` / `targetLength` in the preview).

Core: `tuneTrackLength(pcb, sch, trackId, LengthTuneOptions)` with `apply = false` for the preview; C:
`sieda_router_tune(project, track_id, options_json)` (the older `sieda_router_tune_length` still works). Board-wide
pair and bus matching after Auto Route is unchanged (`tuneLengths`, **Board Setup → Stack-up & Impedance**).

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
| `sieda_router_add_via(project, to_layer)` | Via and layer change (`-1` = the other outer layer) |
| `sieda_router_set_options(project, options_json)` | Change mode / posture during a route |
| `sieda_router_commit(project)` | Write the route; returns the changes |
| `sieda_router_cancel(project)`, `sieda_router_active(project)` | Session control |
| `sieda_router_begin_via_drag(project, options_json, via_id, x, y)` | Drag a via |
| `sieda_router_tune(project, track_id, options_json)` | Length tuning with preview: `{"target","maxAmplitude","spacing","x","y","apply"}` |
| `sieda_router_tune_length(project, track_id, target_mm, max_amplitude_mm)` | Length tuning (applies at once) |
| `sieda_pcb_lock_track`, `sieda_pcb_remove_track`, `sieda_pcb_remove_via` | Track editing |

Options JSON: `{"mode":"shove"|"walkaround", "posture":"45"|"90"|"free", "swapPosture":bool, "width":mm,
"pairGap":mm, "snap":bool}`. Fields that are left out keep their value.

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

- Vias placed by the router are through vias. Blind and buried vias come from the HDI pass after Auto Route.
- Arcs are not supported. Corners are 45° or 90°, or free-angle with posture `free` (core and C API only).
- The shove engine moves tracks and vias, not footprints.
- Shoving a meandered (length-tuned) track can flatten part of the meander. Run length tuning again afterwards.
- Highlight mode lets you commit copper that violates the rules (the DRC reports it); the other modes never do.
- The walkaround search covers the area around the head: about 4 mm, or ¾ of the head length, beyond the start and
  the cursor. A detour further away needs a corner placed on the way.
- Auto Route rips up all routing, locked tracks included.
- Head updates take a few milliseconds on typical boards. A blocked head on a dense board can take up to about
  0.2 s, because the router searches for the furthest position that fits.

## Code map

| What | Where |
|---|---|
| Router, shove engine, walkaround search, length tuning | `Core/src/InteractiveRouter.cpp`, `Core/include/sieda/InteractiveRouter.hpp` |
| Locked tracks (`Track::locked`, saved as `"locked": true`) | `Core/include/sieda/Pcb.hpp`, `Core/src/Project.cpp` |
| C API | `Core/include/sieda/sieda_c.h`, `Core/src/sieda_c.cpp` (`sieda_router_*`) |
| Swift bridge | `SiEDA/Bridge/EDAEngine.swift` (`routerBegin`, `routerMove`, `routerFix`, `routerAddVia`, `routerCommit`, …) |
| App state | `SiEDA/App/DesignStore.swift` (`routePreview`, `beginRoute`, `moveRoute`, `finishRoute`, …) |
| Route tool, preview drawing | `SiEDA/Views/PCB/PCBEditorView.swift` |

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
| `router_highlights_collisions` | Highlight mode lists the three lanes a straight head crosses, moves nothing, and commits as asked. |
| `router_keeps_an_autorouted_board_drc_clean` | 30 pseudo-random routes and drags with shove on an autorouted board, with DRC after every commit. |
| `c_api_router` | The C API end to end (`Core/tests/c_api_test.c`). |

App (`SiEDATests`): `testInteractiveRouterRoutesAndCommits` routes and commits through `EDAEngine`;
`InteractiveRoutingStoreTests` drags a track and a via through `DesignStore` (one undo step each) and previews, applies
and undoes a length tuning.
