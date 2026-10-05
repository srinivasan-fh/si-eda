#include "GlobalRouter.hpp"

#include <algorithm>
#include <cstdlib>
#include <exception>
#include <mutex>
#include <queue>
#include <thread>
#include <utility>

namespace sieda::routing {

GlobalRouter::GlobalRouter(const TileGrid& tg, std::vector<int> capEast, std::vector<int> capSouth) : tg_(tg) {
    const size_t n = static_cast<size_t>(tg_.count());
    cap_.assign(2 * n, 0);
    for (size_t t = 0; t < n; ++t) {
        cap_[2 * t] = t < capEast.size() ? std::max(0, capEast[t]) : 0;
        cap_[2 * t + 1] = t < capSouth.size() ? std::max(0, capSouth[t]) : 0;
    }
    usage_.assign(2 * n, 0);
    history_.assign(2 * n, 0);
    cost_.assign(n, 0);
    parent_.assign(n, -1);
    stamp_.assign(n, 0);
}

int GlobalRouter::edgeId(int a, int b) const {
    if (a > b) std::swap(a, b);
    return b == a + 1 ? 2 * a : 2 * a + 1;  // east neighbour, else the tile below
}

void GlobalRouter::use(const std::vector<int>& path, int demand, int sign) {
    for (size_t k = 1; k < path.size(); ++k) usage_[static_cast<size_t>(edgeId(path[k - 1], path[k]))] += sign * demand;
}

std::vector<int> GlobalRouter::search(const GlobalConnection& c, long presentFactor) {
    const int from = c.from, to = c.to, demand = c.demand;
    const bool payTiles = !c.exempt && !tileCost_.empty();
    if (from == to) return {from};
    if (++gen_ == 0) {
        std::fill(stamp_.begin(), stamp_.end(), 0u);
        gen_ = 1;
    }
    const int tx = to % tg_.cols, ty = to / tg_.cols;
    auto h = [&](int t) { return 100L * (std::abs(t % tg_.cols - tx) + std::abs(t / tg_.cols - ty)); };
    using QE = std::pair<long, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    auto costOf = [&](int t) { return stamp_[static_cast<size_t>(t)] == gen_ ? cost_[static_cast<size_t>(t)] : -1L; };
    auto set = [&](int t, long c, int p) {
        cost_[static_cast<size_t>(t)] = c;
        parent_[static_cast<size_t>(t)] = p;
        stamp_[static_cast<size_t>(t)] = gen_;
    };
    set(from, 0, -1);
    open.push({h(from), from});
    while (!open.empty()) {
        auto [f, t] = open.top();
        open.pop();
        const long gc = costOf(t);
        if (f - h(t) > gc) continue;  // stale
        if (t == to) break;
        const int x = t % tg_.cols, y = t / tg_.cols;
        const int nb[4][2] = {{x + 1, y}, {x - 1, y}, {x, y + 1}, {x, y - 1}};
        for (const auto& q : nb) {
            if (q[0] < 0 || q[1] < 0 || q[0] >= tg_.cols || q[1] >= tg_.rows) continue;
            const int u = tg_.id(q[0], q[1]);
            const size_t e = static_cast<size_t>(edgeId(t, u));
            const long over = static_cast<long>(usage_[e]) + demand - cap_[e];
            long step = 100 + history_[e] + (over > 0 ? presentFactor * over : 0);
            if (cap_[e] == 0) step += 2000;  // a boundary with no free track at all
            if (payTiles) step += tileCost_[static_cast<size_t>(u)];
            const long nc = gc + step;
            const long old = costOf(u);
            if (old < 0 || nc < old) {
                set(u, nc, t);
                open.push({nc + h(u), u});
            }
        }
    }
    std::vector<int> path;
    if (costOf(to) < 0) return {from, to};  // unreachable (cannot happen on a connected tile grid)
    for (int t = to; t >= 0; t = parent_[static_cast<size_t>(t)]) path.push_back(t);
    std::reverse(path.begin(), path.end());
    return path;
}

std::vector<std::vector<int>> GlobalRouter::route(const std::vector<GlobalConnection>& conns, int iterations) {
    std::vector<std::vector<int>> paths(conns.size());
    long present = 10;
    for (size_t k = 0; k < conns.size(); ++k) {
        paths[k] = search(conns[k], present);
        use(paths[k], conns[k].demand, +1);
    }
    auto overflowOf = [&](size_t e) { return std::max(0, usage_[e] - cap_[e]); };
    for (int it = 1; it <= iterations; ++it) {
        overflow_ = 0;
        for (size_t e = 0; e < usage_.size(); ++e) {
            const int o = overflowOf(e);
            overflow_ += o;
            if (o > 0) history_[e] += 10L * o;
        }
        if (overflow_ == 0) break;
        present *= 2;
        for (size_t k = 0; k < conns.size(); ++k) {
            bool congested = false;
            for (size_t s = 1; s < paths[k].size() && !congested; ++s)
                congested = overflowOf(static_cast<size_t>(edgeId(paths[k][s - 1], paths[k][s]))) > 0;
            if (!congested) continue;
            use(paths[k], conns[k].demand, -1);
            paths[k] = search(conns[k], present);
            use(paths[k], conns[k].demand, +1);
        }
    }
    overflow_ = 0;
    for (size_t e = 0; e < usage_.size(); ++e) overflow_ += overflowOf(e);
    return paths;
}

std::vector<int> dilateTiles(const TileGrid& tg, const std::vector<int>& tiles, int d) {
    std::vector<char> mark(static_cast<size_t>(tg.count()), 0);
    for (int t : tiles) {
        if (t < 0 || t >= tg.count()) continue;
        const int x = t % tg.cols, y = t / tg.cols;
        for (int yy = std::max(0, y - d); yy <= std::min(tg.rows - 1, y + d); ++yy)
            for (int xx = std::max(0, x - d); xx <= std::min(tg.cols - 1, x + d); ++xx) mark[static_cast<size_t>(tg.id(xx, yy))] = 1;
    }
    std::vector<int> out;
    for (int t = 0; t < tg.count(); ++t)
        if (mark[static_cast<size_t>(t)]) out.push_back(t);
    return out;
}

std::vector<std::vector<int>> scheduleBatches(const TileGrid& tg, const std::vector<std::vector<int>>& corridor,
                                              const std::vector<std::vector<int>>& reach, int maxBatch, int lookahead) {
    const int n = static_cast<int>(corridor.size());
    std::vector<std::vector<int>> batches;
    std::vector<char> done(static_cast<size_t>(n), 0);
    std::vector<uint32_t> claim(static_cast<size_t>(tg.count()), 0);
    uint32_t gen = 0;
    int first = 0;  // first net not yet scheduled
    while (first < n) {
        ++gen;
        std::vector<int> batch;
        int scanned = 0;
        for (int k = first; k < n && static_cast<int>(batch.size()) < maxBatch && scanned <= lookahead; ++k) {
            if (done[static_cast<size_t>(k)]) continue;
            bool clear = true;
            for (int t : corridor[static_cast<size_t>(k)])
                if (claim[static_cast<size_t>(t)] == gen) {
                    clear = false;
                    break;
                }
            // Either way the net's reach is claimed: a batch member's, or a waiting net's that later nets must not
            // overtake.
            for (int t : reach[static_cast<size_t>(k)]) claim[static_cast<size_t>(t)] = gen;
            if (clear) {
                batch.push_back(k);
                done[static_cast<size_t>(k)] = 1;
            }
            if (!batch.empty() && !clear) ++scanned;
        }
        batches.push_back(std::move(batch));
        while (first < n && done[static_cast<size_t>(first)]) ++first;
    }
    return batches;
}

void parallelFor(int n, int threads, const std::function<void(int, int)>& work) {
    if (n <= 0) return;
    threads = std::max(1, std::min(threads, n));
    if (threads == 1) {
        for (int i = 0; i < n; ++i) work(i, 0);
        return;
    }
    std::atomic<int> next{0};
    std::exception_ptr error;
    std::mutex errorLock;
    auto run = [&](int worker) {
        for (int i = next.fetch_add(1); i < n; i = next.fetch_add(1)) {
            try {
                work(i, worker);
            } catch (...) {  // never let an exception escape a thread: rethrown on the calling thread
                std::lock_guard<std::mutex> lock(errorLock);
                if (!error) error = std::current_exception();
                next.store(n);
            }
        }
    };
    std::vector<std::thread> pool;
    pool.reserve(static_cast<size_t>(threads - 1));
    for (int w = 1; w < threads; ++w) pool.emplace_back(run, w);
    run(0);
    for (auto& t : pool) t.join();
    if (error) std::rethrow_exception(error);
}

}  // namespace sieda::routing
