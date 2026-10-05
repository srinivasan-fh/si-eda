# Auto Place, Auto Route and DRC: performance and limits

This guide covers how the placer, the autorouter and the design-rule check scale to large boards, how the router
recovers connections that its rip-up passes leave unrouted, the scale benchmark and its measured numbers, and the
limits that remain. The router's features (layers, vias, BGA fan-out, neck-down, pours, length matching) are listed in
the README's **Autorouting** row.

- [How a board is routed](#how-a-board-is-routed)
- [Recovery: negotiated congestion](#recovery-negotiated-congestion)
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

Routing is deterministic: the same design and settings give the same copper on every run and platform.

## Recovery: negotiated congestion

Re-routing with the failed nets promoted to the front often just moves the failure to another net. When the 8 rip-up
passes end with unrouted connections, up to 8 recovery passes route the board in the original net order with a
**history cost** (the PathFinder idea of negotiated congestion):

- After each pass, every unrouted connection is routed once more on an *unobstructed* grid (pads, fences and fixed
  fan-out copper only). The cells along that path, widened by a track and its clearance, get a history cost of 2 per
  pass. Only the net that wanted the corridor is exempt; where several failing nets want it, everyone pays.
- Every other net then pays to cross those corridors, so it detours if it can, and the failing net finds its corridor
  free.
- Vias cost 8 instead of 12 in recovery, which helps nets change layer around a blockage.
- A recovery pass is kept only when it routes strictly more connections than the best pass so far, and recovery
  stops after 3 passes without improvement. Boards that the rip-up passes complete never reach recovery, so their
  copper is unchanged.

## Performance work

Every change below gives **exactly the same** placement, copper and DRC report as before (verified bit for bit on the
whole test suite and on all 40 reference designs, with gcc and with clang + fused multiply-add), except the recovery
passes and the large-grid search budget, which change nothing on any board of the regression suite.

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
  of the grid (at least 1 M expansions). An unroutable connection used to flood the whole grid, tens of millions of
  nodes, before failing; routable ones on the benchmark need at most 3.7 %. Every board in the regression suite and
  every reference design is below 1 M nodes, so the budget never applies to them.

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

## Scale benchmark

`Core/tests/bench/BoardGenerator.hpp` generates deterministic boards: one IC per cluster (96-ball 0.8 mm BGA DRAMs,
QFN-48/56, LQFP-48/64, TSSOP-20/24; a 324-ball FPGA on request), each with its decoupling capacitors, pull-ups,
filter capacitors and 33 Ω series resistors to the next clusters. Boards with 4 or more layers get a GND plane on
layer 1 and a +3V3 plane on layer n−2. The outline is sized for about 35 % courtyard fill.

```
cmake --build build --target sieda_route_bench
./build/sieda_route_bench                       # the medium (CI) board, then the large board
./build/sieda_route_bench --clusters 16 --layers 8 --seed 3 [--fpga] [--no-bga] [--drc-brute] [--snapshot out.json]
```

It prints component, net and pin counts and the time of auto-place, auto-route and DRC, completion and peak memory.
`--drc-brute` also times the pairwise DRC and confirms the reports are identical. The unit test
`scale_benchmark_medium_board` runs the medium board on every test run; with `SIEDA_BENCH_LARGE=1` it also runs the
large board.

## Measured numbers

Release build, gcc, one core of a 4-core Linux container (other jobs were running, so times are approximate). "Before"
is the code before this work, "after" the code now.

| Board | Parts / nets / layers | Grid nodes | Place | Route | DRC | Completion | Peak memory |
|---|---|---|---|---|---|---|---|
| Medium (CI): 6 clusters, 2 BGAs | 166 / 196 / 6 | 4.6 M | 0.88 → 0.13 s | 14.6 → 3.2 s | 1.26 → 0.09 s | 385 / 385 (same copper) | 119 → 188 MB |
| 2 clusters, BGA, 2 signal layers | 82 / 87 / 4 | 1.4 M | 0.09 → 0.03 s | 41.6 → 35.1 s | 0.53 → 0.03 s | 189 → 197 / 200 | 50 → 90 MB |
| 16 clusters, 4 BGAs | 403 / 496 / 8 | 14.8 M | 9.95 → 0.93 s | 284.7 → 39.6 s | 8.8 → 0.29 s | 922 / 922 (same copper) | 372 → 524 MB |
| Large: 32 clusters, FPGA + 4 BGAs | 923 / 1172 / 8 | 33.0 M | 131.2 → 6.0 s | 6209 → 1039 s | 58.5 → 0.35 s | 2140 / 2143 (same) | 953 MB → 1.8 GB |

Recovery on boards the rip-up passes do not complete (unrouted connections, before → after):

| Board | Before | After |
|---|---|---|
| 3 clusters, 2 layers, no BGA | 10 / 260 | 1 / 260 |
| 4 clusters, 2 layers, no BGA, seed 2 | 21 / 283 | 2 / 283 |
| 6 clusters, 2 layers, no BGA, seed 3 | 16 / 352 | 1 / 352 |
| 3 clusters, 4 layers, no BGA, seed 4 | 1 / 240 | 0 / 240 |
| 6 clusters, 4 layers, no BGA, seed 5 | 13 / 362 | 0 / 362 |
| 3 clusters, 4 layers, BGA, seed 8 | 2 / 249 | 1 / 249 |

In total 63 → 5 unrouted connections; recovery adds up to 8 passes to such boards (route times 4–27 s here). On the
large board the rip-up passes bring 29 unrouted connections down to 3 and recovery does not improve on that; its 11
whole-board passes take about 17 minutes.

The pairwise DRC on the same routed boards takes 1.12 s (medium) and 6.15 s (16 clusters) against 0.09 s and 0.29 s
with the index, with identical reports.

Memory is higher because the search workspace is kept for the whole route instead of being allocated per connection
(12 bytes per routing node, plus 8 bytes per cell), which is what removed the per-connection clearing cost.

## Limits

- **Grid size.** The grid is uniform. A board with a 0.8 mm BGA routes on a 0.1 mm grid everywhere, so a 234 × 176 mm
  8-layer board has 33 M routing nodes and needs about 1.1 GB while routing (1.8 GB during recovery, which builds a second, unobstructed grid). Very large fine-pitch boards are better
  split, or routed with the BGA areas fanned out first.
- **Rip-up is whole-board.** Each rip-up or recovery pass re-routes every net; on the largest boards a pass takes
  minutes, so a board that does not complete in its first pass takes correspondingly longer.
- **Two signal layers under a BGA.** Inner balls of a 0.8 mm BGA cannot escape on two signal layers (a 4-layer board
  whose inner layers are planes); the router reports them unrouted. Use 6 or more layers, or via-in-pad.
- **Completion is not guaranteed.** Recovery reduces unrouted connections on congested boards but does not prove
  that none remain; the DRC's DRC_UNROUTED and the ratsnest show what is left for manual routing.
- **Placement** is greedy and connectivity-driven, without iterative improvement; dense boards may route better after
  manual adjustment of the critical parts.

## Code map

| What | Where |
|---|---|
| Router: grid, A*, workspace, budget, recovery | `Core/src/Pcb.cpp` (`RoutingGrid`, `AStarWorkspace`, `astar`, `Congestion`, `PcbLayout::routeAll`) |
| Spatial index, DRC, connectivity, ratsnest | `Core/src/Pcb.cpp` (`RectIndex`, `PcbLayout::runDRC`, `copperClusters`, `PcbLayout::ratsnest`) |
| Clean-up | `Core/src/Pcb.cpp` (`PcbLayout::cleanupRouting`) |
| Placer | `Core/src/Pcb.cpp` (`PcbLayout::autoPlace`) |
| Brute-force switch | `Core/include/sieda/Pcb.hpp` (`setDrcBruteForce`, `drcBruteForce`) |
| Benchmark | `Core/tests/bench/BoardGenerator.hpp`, `Core/tests/bench/route_bench.cpp` |

## Tests

- `drc_spatial_index_matches_brute_force`: six boards crowded with deliberately bad copper (shorts, near misses,
  acute joins, stubs, vias in pads, tight holes, a 48 V net, an isolation barrier) plus a routed benchmark board. The
  indexed DRC report equals the pairwise one exactly (codes, messages, locations, parts, order), and so do the
  ratsnest and the routing clean-up. Every pairwise check fires at least once.
- `scale_benchmark_medium_board`: the medium benchmark board places, routes completely and passes DRC within a
  generous time budget.
- All earlier routing, DRC and placement tests are unchanged and pass with identical results.
