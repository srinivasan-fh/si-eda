// SiEDA autorouter, large-board path: coarse global routing, net corridors and deterministic parallel scheduling.
//
// Internal header (not installed). The detailed router (Core/src/Pcb.cpp, PcbLayout::routeAll) routes each connection
// on its fine grid, but on large boards only inside the corridor the global router found for it here: a chain of
// coarse tiles (a few millimetres square) chosen with negotiated congestion (PathFinder) against per-tile-boundary
// track capacities. Nets whose corridors are far enough apart are routed on several threads at once; the schedule
// depends only on the corridors, never on the thread count, so the copper is the same with 1 or N threads.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <utility>
#include <vector>

namespace sieda::routing {

/// Coarse tiles over the routing grid: tile (tx, ty) covers cells [tx·tile, (tx+1)·tile) × [ty·tile, (ty+1)·tile).
struct TileGrid {
    int tile = 8;  // cells per tile side
    int cols = 1, rows = 1;
    int tilesFor(int cells) const { return (cells + tile - 1) / tile; }
    int id(int tx, int ty) const { return ty * cols + tx; }
    int ofCell(int i, int j) const { return id(i / tile, j / tile); }
    int count() const { return cols * rows; }
};

/// One two-pin connection for the global router: from one tile to another, `demand` in quarter tracks.
struct GlobalConnection {
    int from = 0, to = 0;
    int demand = 4;
    bool exempt = false;  // does not pay the tile costs (setTileCost): the connection those costs make room for
};

/// 2-D global router over a TileGrid. Capacities are per tile boundary, in quarter tracks: `capEast[t]` is the
/// boundary between tile t and its east neighbour, `capSouth[t]` between t and the tile below. Integer costs only,
/// so the result does not depend on floating-point contraction (FMA) or on the compiler.
class GlobalRouter {
public:
    GlobalRouter(const TileGrid& tg, std::vector<int> capEast, std::vector<int> capSouth);
    /// Routes every connection with negotiated congestion; returns each connection's tile path (from … to).
    std::vector<std::vector<int>> route(const std::vector<GlobalConnection>& conns, int iterations = 6);
    /// Extra cost of entering each tile (rip-up history: tiles unrouted connections need), paid by every connection
    /// that is not `exempt`.
    void setTileCost(std::vector<int> cost) { tileCost_ = std::move(cost); }
    /// Total overflow (demand beyond capacity, quarter tracks) of the last route() call.
    long overflow() const { return overflow_; }

private:
    int edgeId(int a, int b) const;  // boundary between adjacent tiles a, b
    std::vector<int> search(const GlobalConnection& c, long presentFactor);
    void use(const std::vector<int>& path, int demand, int sign);
    TileGrid tg_;
    std::vector<int> cap_;    // per edge: 2·tile (east edge at 2t, south edge at 2t+1)
    std::vector<int> usage_;  // per edge, quarter tracks
    std::vector<long> history_;
    std::vector<int> tileCost_;
    long overflow_ = 0;
    // Search scratch, invalidated by a generation stamp.
    std::vector<long> cost_;
    std::vector<int> parent_;
    std::vector<uint32_t> stamp_;
    uint32_t gen_ = 0;
};

/// Tiles within Chebyshev distance `d` of any tile in `tiles` (sorted, unique).
std::vector<int> dilateTiles(const TileGrid& tg, const std::vector<int>& tiles, int d);

/// Deterministic batches for parallel routing. `corridor[k]` are the tiles net k may search, `reach[k]` the tiles
/// its copper may influence (its corridor dilated by the clearance reach). Nets are taken in index order; a net joins
/// the current batch only if its corridor is clear of the reach of every net already in the batch and of every
/// earlier net still waiting, so routing a batch's nets against the same board state gives exactly the result of
/// routing them one after the other in index order. `maxBatch` caps a batch, `lookahead` how far past the first
/// waiting net a batch may reach.
std::vector<std::vector<int>> scheduleBatches(const TileGrid& tg, const std::vector<std::vector<int>>& corridor,
                                              const std::vector<std::vector<int>>& reach, int maxBatch = 64,
                                              int lookahead = 512);

/// Runs work(i, worker) for i in [0, n) on up to `threads` std::threads (worker in [0, threads)). Each index is run
/// exactly once; which worker runs it is not fixed, so work must depend only on i (and per-worker scratch).
void parallelFor(int n, int threads, const std::function<void(int index, int worker)>& work);

}  // namespace sieda::routing
