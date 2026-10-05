# Auto Place, Auto Route and DRC: performance and limits

This guide covers how the placer, the autorouter and the design-rule check scale to large boards: the classic
router for small and medium boards, the corridor router for large ones (global routing, corridors, parallel nets,
targeted rip-up), progress and cancel, the scale benchmark and its measured numbers, and the limits that remain. The
router's features (layers, vias, BGA fan-out, neck-down, pours, length matching) are listed in the README's
**Autorouting** row.

- [How a board is routed](#how-a-board-is-routed)
- [Which router](#which-router)
- [The corridor router (large boards)](#the-corridor-router-large-boards)
- [Recovery: negotiated congestion](#recovery-negotiated-congestion)
- [Progress and cancel](#progress-and-cancel)
- [Performance work](#performance-work)
- [Scale benchmark](#scale-benchmark)
- [Measured numbers](#measured-numbers)
- [Limits](#limits)
- [Code map](#code-map)
- [Tests](#tests)

## How a board is routed

`PcbLayout::autoRoute` (`Core/src/Pcb.cpp`) routes on a grid of 0.25 mm cells (1/8 of the ball pitch when the board
has a BGA), one grid per copper layer:

1. **Fixed copper first.** BGA dogbones (or vias in pad), tamper meshes, and short fan-out stubs from plane / pour
   net pads to their own vias.
2. **Signal nets** in order: nets with extra spacing (high-impedance, fast) first, then nets with fine-pitch pads,
   then the rest by bounding-box size. Each net grows as a tree: the next pad is the unconnected one nearest to a
   connected one, and A* may start from any copper already on the net.
3. **Pours and planes** fill around the signal copper; pads of zone nets that the fill does not reach are joined by
   tracks.
4. **Rip-up passes.** If connections fail, the board is routed again from scratch with the failing nets first, up to
   8 passes; the pass with the fewest unrouted connections is kept.
5. **Recovery** (below), only when the rip-up passes still leave connections unrouted.
6. **Clean-up:** neck-down at fine-pitch pads, merging collinear pieces, 45° chamfers, HDI via spans and length
   matching.

Routing is deterministic: the same design and settings give the same copper on every run, and the same copper for
any number of threads.

## Which router

| Board | Router | Why |
|---|---|---|
| Routing grid below 2 M nodes (cells × copper layers): every reference design and every board of the regression suite | **Classic** (steps 1–6 above, unchanged) | Its copper and DRC reports are bit-identical to earlier releases |
| 2 M nodes or more (for example a 0.8 mm BGA on a 100 × 75 mm, 6-layer board, or any 1000-part board) | **Corridor router** | A whole-board search per connection and whole-board rip-up passes do not scale |

`setRouterStrategy(RouterStrategy::Classic | Corridor)` (C API `sieda_router_set_strategy`, benchmark `--router`)
forces one or the other; the tests run the corridor router on small boards that way too.

## The corridor router (large boards)

The board's fixed copper, pours and planes, clean-up, HDI vias and length matching are exactly those of the classic
router; what changes is how the signal nets are routed and how failures are repaired.

**Global routing.** The board is cut into tiles of about 2 mm (8 cells on a 0.25 mm grid, 20 on a 0.1 mm BGA grid).
Each boundary between two tiles gets a capacity in tracks, counted from the free cells along it on every signal layer
(a layer counts half across its preferred direction), derated to 3/4. Every two-pin connection of every net (the
same nearest-pad order the detailed router uses) is routed over the tiles with negotiated congestion (PathFinder):
a boundary used beyond its capacity costs more in every iteration, and its history cost grows, so connections spread
over the available channels. Costs are integers, so the result does not depend on the compiler or on fused
multiply-add.

**Corridors.** A connection's corridor is its tile route plus the straight line between its ends, widened by two
tiles. The detailed router is the classic A* — same costs, layer directions, turn penalties, neck-down, wide-track and
isolation checks — restricted to the corridor's cells. Its search state is paged per tile, so memory follows the
corridor, not the board (the classic router keeps arrays over the whole grid: 12 bytes per routing node).

**Parallel nets.** A net's *reach* is its corridors widened by the clearance reach of its copper (the widest track or
via, clearance, extra spacing and isolation gap). Nets are taken in routing order into batches: a net joins a batch
only if its corridors are clear of the reach of every net already in the batch and of every earlier net still
waiting. The nets of a batch search the same board state on up to 8 threads (`std::thread`, one search workspace per
thread; vias a net places are checked against its own earlier vias until it is committed), and their copper is then
committed one net after the other in routing order. Because nets in a batch cannot see each other's copper, the
result is exactly that of routing them one after the other, so the copper is the same for 1 or N threads
(`setRoutingThreads`, default: the machine's cores, at most 8).

**Retries.** A connection its corridor cannot hold is retried after the batches, one at a time: in a corridor
widened to 3 tiles, then to 8 tiles plus every pad of the net, then around a second global route that avoids the
first one's corridor. A pad whose connection failed still joins its net's tree as an island, so the net's other pads
can reach it; the retry then joins that island to the rest.

**Targeted rip-up.** Whole-board passes cost minutes on a 1000-part board, so the corridor router repairs locally:

1. Each unrouted connection searches again, on the full board state, with other signal nets' routed copper passable
   at a price (pads, the board edge, holes, fan-outs and pour connections stay hard; a via blocked only by routed
   copper is allowed at that price).
2. The failing nets and the nets that path crosses are ripped up. The next pass lays every other net's copper again
   as it was, routes the failing nets first — completely, retries included — and then the ripped ones.
3. The cells along those paths get a negotiated-congestion history cost (paged per tile), and their tiles a history
   cost in global routing, so the re-routed nets keep away from them.
4. A pass that does not improve raises the aggression: a wider search, cheaper through routed copper, and the nets
   running alongside the path ripped too.
5. Rip-up is a local search, so it runs as three strategies one after the other: a plain walk (ripped nets in
   routing order); negotiated congestion with an occasional whole-board pass; and bus re-ordering (a failing
   two-pin net's neighbours with both ends within 6 mm are ripped with it and re-routed in their order across the
   bus). Each strategy runs until four passes bring no improvement, and the next starts again from the best pass. At
   most 30 passes; the best pass is kept.

Layer changes cost 8 instead of 12 in rip-up passes (as in the classic recovery).

## Recovery: negotiated congestion

This is the classic router's repair (boards below 2 M nodes). Re-routing with the failed nets promoted to the front
often just moves the failure to another net. When the 8 rip-up passes end with unrouted connections, up to 8
recovery passes route the board in the original net order with a **history cost** (the PathFinder idea of negotiated
congestion):

- After each pass, every unrouted connection is routed once more on an *unobstructed* grid (pads, fences and fixed
  fan-out copper only). The cells along that path, widened by a track and its clearance, get a history cost of 2 per
  pass. Only the net that wanted the corridor is exempt; where several failing nets want it, everyone pays.
- Every other net then pays to cross those corridors, so it detours if it can, and the failing net finds its corridor
  free.
- Vias cost 8 instead of 12 in recovery, which helps nets change layer around a blockage.
- A recovery pass is kept only when it routes strictly more connections than the best pass so far, and recovery
  stops after 3 passes without improvement. Boards that the rip-up passes complete never reach recovery, so their
  copper is unchanged.

## Progress and cancel

`PcbLayout::autoRoute(sch, RouteControl)` reports `RouteProgress` (phase: preparing, routing, rip-up pass,
finishing; nets done of the pass; connections the best pass so far leaves unrouted) about 40 times per pass, on the
routing thread, never concurrently. Returning `false` cancels: the route stops at its next report and leaves the
board exactly as it was (tracks, vias, net classes; the C API also restores parts it placed first) and returns
`RouteStats::cancelled`. C API: `sieda_pcb_autoroute_progress(project, callback, user)`.

In the app, **Auto Route** runs off the main thread as before; the status bar now shows a progress bar with the phase
and **Stop**, which leaves the board as it was (the rip-up and placement done before routing are undone too).

## Performance work

Every change in this section gives **exactly the same** placement, copper and DRC report as before (verified bit for
bit on the whole test suite and on all 40 reference designs, with gcc and with clang + fused multiply-add), except
the recovery passes and the large-grid search budget, which change nothing on any board of the regression suite.

**Router**

- *Search workspace.* A* used to allocate and clear cost, parent and target arrays over the whole grid (all layers)
  for every connection, which dominated routing time on big grids. One workspace is now allocated per route and
  invalidated with a generation stamp, so a search costs only the cells it visits. Targets are a per-cell layer mask.
- *Via test.* `viaAllowed` checked every pad on the board; it now asks the pad bucket index, walks a precomputed disc
  of cells, and is cached per cell for the duration of a search.
- *Fan-out search.* A pour pad's fan-out via site is tested only where the search reaches, not on every cell within
  3 mm beforehand.
- *Neck-down* (`padNeckWidths`, `neckDown`) compared every pad with every pad for each committed track; pads are now
  grouped by component and by net once per route.
- *Net trees and ratsnest.* "Nearest unconnected pad" and the ratsnest's Prim tree keep each pad's nearest connected
  pad (O(k) per step instead of O(k²)), with the same tie-breaking as the pairwise scan.
- *Large-grid search budget.* On grids of at least 2 M routing nodes (cells × layers) a search gives up after an eighth
  of the grid (at least 1 M expansions). Every board in the regression suite and every reference design is below 1 M
  nodes, so the budget never applies to them.
- *Thread-safe queries.* The routing grid's bucket queries de-duplicate with per-thread stamps, so several threads can
  query one grid; the answers are unchanged.

**DRC and connectivity**

- A uniform-grid spatial index (`RectIndex`) over pads, tracks, vias, holes, courtyards and isolation items replaces
  the O(n²) pair loops: pad–pad, track–pad, track–track, track–via, via–pad, via–via, hole spacing, via-in-pad,
  dangling ends, acute angles and the isolation barrier. The query radius is the largest spacing any check can apply
  (design clearance, the 0.1 mm etch limit, or the IPC-2221 voltage spacing for the board's largest voltage
  difference), and candidates come back in index order, so every check runs in the order of the full scan and
  reports exactly the same violations in the same order.
- Copper connectivity (`copperClusters`, used by the ratsnest, DRC_UNROUTED and pruning after schematic edits) and
  the routing clean-up (`cleanupRouting`) use the same index.
- `setDrcBruteForce(true)` switches all of these back to the pairwise scan; the equivalence test compares both.

**Placer**

- The placement order (the part most connected to the placed set next) keeps each part's score and updates it only
  for neighbours of the part just placed, summing in the same order as before.
- Keep-out clashes for all candidate positions of a part are marked at once: each other part blocks one rectangle of
  candidate positions (found by binary search), recorded in a 2-D difference array, instead of testing every
  candidate against every placed part.

**App (large designs)**

- `DesignSnapshot.component(_:)` was a linear scan, called for every wire end and every courtyard of every frame, so
  drawing a 1000-part design was O(n²) per frame; it is now an O(1) index lookup. Keeping the selection after an edit
  is O(n) instead of O(n²).
- The PCB canvas strokes tracks in batches (same highlight, colour role and width) instead of one stroke per segment,
  and draws only what is on screen (as before for pads, vias and parts).
- The ratsnest and the snapshot the app redraws from are measured by the benchmark: 13 ms and 0.17 s (4 MB of JSON)
  for the routed 923-part board.

## Scale benchmark

`Core/tests/bench/BoardGenerator.hpp` generates deterministic boards: one IC per cluster (96-ball 0.8 mm BGA DRAMs,
QFN-48/56, LQFP-48/64, TSSOP-20/24; a 324-ball FPGA on request), each with its decoupling capacitors, pull-ups,
filter capacitors and 33 Ω series resistors to the next clusters. Boards with 4 or more layers get a GND plane on
layer 1 and a +3V3 plane on layer n−2. The outline is sized for about 35 % courtyard fill.

```
cmake --build build --target sieda_route_bench
./build/sieda_route_bench                       # the medium (CI) board, then the large board
./build/sieda_route_bench --clusters 16 --layers 8 --seed 3 [--fpga] [--no-bga] [--drc-brute] [--snapshot out.json]
                          [--threads N] [--router auto|classic|corridor]
```

It prints component, net and pin counts, the time of auto-place, auto-route (wall and CPU) and DRC, completion, the
ratsnest and snapshot times, and peak memory. `--drc-brute` also times the pairwise DRC and confirms the reports are
identical; `SIEDA_BENCH_VERBOSE=1` lists failed nets and DRC errors. The unit test `scale_benchmark_medium_board`
runs the medium board (6 layers, 4.6 M nodes: the corridor router) on every test run; with `SIEDA_BENCH_LARGE=1` it
also runs the large board.

## Measured numbers

Release build, gcc, a 4-core Linux container shared with other jobs (times are approximate; CPU time in brackets).
"Classic" is the router before this work (still used below 2 M nodes), "corridor" the corridor router with 4 threads.

| Board | Parts / nets / layers | Grid nodes | Router | Route | Completion | Vias | Length | Peak memory |
|---|---|---|---|---|---|---|---|---|
| Medium (CI): 6 clusters, 2 BGAs | 166 / 196 / 6 | 4.6 M | classic | 2.5 s | 385 / 385 | 458 | 3102 mm | 189 MB |
| | | | corridor | 2.2 s (2.7 s) | 385 / 385 | 458 | 3102 mm | 151 MB |
| 6 clusters, 2 BGAs, 2 signal layers | 166 / 196 / 4 | 3.1 M | classic | 40.4 s | 380 / 385 | 571 | 3311 mm | 192 MB |
| | | | corridor | 35.2 s | **385 / 385** | 683 | 3448 mm | 159 MB |
| 16 clusters, 4 BGAs | 403 / 496 / 8 | 14.8 M | classic | 32.2 s | 922 / 922 | 1174 | 10573 mm | 525 MB |
| | | | corridor | 18.4 s (23.3 s) | 922 / 922 | 1153 | 10590 mm | 415 MB |
| Large: 32 clusters, FPGA + 4 BGAs | 923 / 1172 / 8 | 33.0 M | classic | 815.6 s | 2140 / 2143 | 3023 | 29455 mm | 1652 MB |
| | | | corridor | **106.7 s** (133.5 s) | **2143 / 2143** | 3062 | 29626 mm | **876 MB** |

On eight benchmark boards of 166–550 parts (6 and 8 layers, BGAs: 6, 8 ×3, 10, 12, 16 and 20 clusters) both routers
complete every board; the corridor router uses 0.3 % fewer vias (6689 against 6711) and the same track length
(53.78 against 53.79 m) in 69 s against 247 s. Two harder boards route more with the corridor router (the 4-layer one above,
and an 8-cluster, 6-layer, 302-part FPGA board: 720 / 724 against 712 / 724 in 70 s against 93 s); a 24-cluster board
(635 parts) routes 1445 / 1445 in 37 s. DRC reports no error other than unrouted connections on any of them.

Rip-up on congested boards the classic router handles by default (corridor router forced, unrouted connections):

| Board (corridor router forced) | Classic | Corridor |
|---|---|---|
| 3 clusters, 2 layers, no BGA | 1 / 260 | 0 / 260 |
| 4 clusters, 2 layers, no BGA, seed 2 | 2 / 283 | 1 / 283 |
| 6 clusters, 2 layers, no BGA, seed 3 | 1 / 352 | 0 / 352 |
| 3 clusters, 4 layers, no BGA, seed 4 | 0 / 240 | 0 / 240 |
| 6 clusters, 4 layers, no BGA, seed 5 | 0 / 362 | 0 / 362 |
| 3 clusters, 4 layers, BGA, seed 8 | 1 / 249 | 1 / 249 |
| 2 clusters, 4 layers, BGA (inner balls on 2 signal layers) | 3 / 200 | 5 / 200 |
| 3 clusters, 2 layers, no BGA, seed 7 | 2 / 244 | 2 / 244 |

In total 10 → 9 unrouted connections, each board in 1–43 s against 4–31 s.

The reference designs with the corridor router forced (2- and 4-layer defaults): with gcc all 40 pass verification as
with the classic router, except that the satellite computer gets one DRC warning (an acute T-join on a length-matched
lane); with clang and fused multiply-add the quadcopter also leaves one connection unrouted. No short or clearance
error on any. By default they use the classic router, with unchanged results.

Thread scaling on the corridor router is modest: on the large board 1 → 3 → 4 threads take 128.6 → 107.6 → 106.2 s
(peak memory 748 → 840 → 860 MB), with byte-identical results. Most of a pass is parallel, but the rip-up passes route few nets each, and grid set-up, pours and pour
connections are serial.

## Limits

- **Grid.** The detailed grid is still uniform (1/8 of the finest BGA pitch everywhere): the 923-part board has 33 M
  routing nodes and the routing grid itself takes about 400 MB. The corridor router only searches corridors, so time
  no longer grows with the grid, but memory does. Boards much larger than 250 × 180 mm with a 0.8 mm BGA will exceed
  1 GB.
- **Completion is not guaranteed.** Rip-up is a local search; it completes every routable board of the benchmark set,
  but on boards that are not fully routable (inner BGA balls on two signal layers, two-layer boards without planes) it
  can leave one or two more connections unrouted than the classic router's whole-board passes. DRC_UNROUTED and the
  ratsnest show what is left.
- **Two signal layers under a BGA.** Inner balls of a 0.8 mm BGA cannot escape on two signal layers (a 4-layer board
  whose inner layers are planes); the router reports them unrouted. Use 6 or more layers, or via-in-pad.
- **Differential pairs and length groups** route as individual nets with their net-class widths and spacing; pairs
  are not yet routed as coupled pairs, and length matching is applied afterwards with serpentines (as in the classic
  router).
- **Parallel speed-up** is limited (see above); the gain on large boards comes mainly from corridors and local rip-up.
- **T-joins.** A path never turns sharper than 90°, but a connection that leaves its net's existing track at a 45°
  angle can still form an acute join (a DRC_ACUTE_ANGLE warning), as with the classic router.
- **Compilers.** Routing is deterministic for a given build and any thread count; builds with and without fused
  multiply-add can round differently and route differently (as the classic router can).
- **Placement** is greedy and connectivity-driven, without iterative improvement; dense boards may route better after
  manual adjustment of the critical parts.

## Code map

| What | Where |
|---|---|
| Global router, corridors, batch scheduling, thread pool | `Core/src/GlobalRouter.{hpp,cpp}` (`GlobalRouter`, `dilateTiles`, `scheduleBatches`, `parallelFor`) |
| Router: grid, A*, workspace (dense or corridor), budget, recovery, corridor passes and rip-up | `Core/src/Pcb.cpp` (`RoutingGrid`, `AStarWorkspace`, `astar`, `Congestion`, `PcbLayout::routeAll`) |
| Strategy, threads, progress / cancel | `Core/include/sieda/Pcb.hpp` (`setRouterStrategy`, `setRoutingThreads`, `RouteControl`, `RouteProgress`) |
| C API | `sieda_pcb_autoroute_progress`, `sieda_router_set_strategy`, `sieda_router_set_threads` (`sieda_c.h`) |
| App | `EDAEngine.autoRouteChecked(progress:)`, `RouteProgressChannel`, `DesignStore.cancelAutoRoute`, `RouteProgressView` |
| Spatial index, DRC, connectivity, ratsnest | `Core/src/Pcb.cpp` (`RectIndex`, `PcbLayout::runDRC`, `copperClusters`, `PcbLayout::ratsnest`) |
| Clean-up | `Core/src/Pcb.cpp` (`PcbLayout::cleanupRouting`) |
| Placer | `Core/src/Pcb.cpp` (`PcbLayout::autoPlace`) |
| Brute-force switch | `Core/include/sieda/Pcb.hpp` (`setDrcBruteForce`, `drcBruteForce`) |
| Benchmark | `Core/tests/bench/BoardGenerator.hpp`, `Core/tests/bench/route_bench.cpp` |

## Tests

- `global_router_capacity_batches_and_threads`: the global router detours around a boundary without capacity,
  spreads connections over a crowded boundary and keeps away from history tiles unless exempt; batches never let a
  net overtake an earlier conflicting one; `parallelFor` runs every index once on any thread count and returns worker
  exceptions to the caller.
- `corridor_router_same_copper_for_any_thread_count`: the medium board on 1, 2 and 4 threads — complete, DRC-clean
  and the same copper bit for bit.
- `corridor_router_forced_on_smaller_boards`: five congested boards with the corridor router forced: DRC-clean apart
  from unrouted connections; boards the classic router completes, it completes.
- `autoroute_progress_and_cancel`: progress reaches the finishing phase; cancelling leaves tracks, vias and net classes
  as they were (both routers); the C API test (`sieda_c_api_autoroute_progress_test`) cancels, routes with progress,
  and routes with the corridor router on 1 and 4 threads.
- `drc_spatial_index_matches_brute_force`, `scale_benchmark_medium_board` and every earlier routing, DRC and placement
  test pass; the classic router's copper and DRC reports are bit-identical to before on the whole suite and all 40
  reference designs (gcc, and clang with fused multiply-add).
- App (XCTest, `LargeDesignScaleTests`): a 1000-part, 1000-net design refreshes and draws its schematic and PCB within
  time budgets; Auto Route reports progress and Stop leaves the board as it was; the corridor router gives the same
  result on 1 and 4 threads through the bridge.
