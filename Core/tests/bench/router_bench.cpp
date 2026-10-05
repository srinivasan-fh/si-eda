// sieda_router_bench — times interactive-router head updates on a generated, autorouted large board.
//
//   sieda_router_bench                    16 clusters (≈ 400 parts), 8 layers, seed 1
//   sieda_router_bench --clusters 6 --layers 6 --seed 1 --moves 400
//
// Places and autoroutes the board once, then starts routes on random pads (and drags random tracks) and moves the
// head to random points nearby, timing every update in Shove and Walk around mode. Prints the median, 90th and 99th
// percentile and the worst update, and how long a cancelled update takes to return (sieda_router_abort).
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include "BoardGenerator.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/Pcb.hpp"

using namespace sieda;

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Lcg {
    uint32_t s;
    double next() {
        s = s * 1103515245u + 12345u;
        return ((s >> 8) & 0xFFFFFF) / double(0xFFFFFF);
    }
};

void report(const char* what, std::vector<double> t) {
    if (t.empty()) return;
    std::sort(t.begin(), t.end());
    auto at = [&](double q) { return t[std::min(t.size() - 1, static_cast<size_t>(q * static_cast<double>(t.size())))]; };
    std::printf("  %-22s %5zu updates   median %7.2f ms   p90 %7.2f ms   p99 %7.2f ms   worst %7.2f ms\n", what, t.size(),
                at(0.5), at(0.9), at(0.99), t.back());
}
}  // namespace

int main(int argc, char** argv) {
    bench::BenchSpec spec;
    spec.clusters = 16;
    spec.layers = 8;
    int moves = 300;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--clusters")) spec.clusters = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--layers")) spec.layers = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seed")) spec.seed = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--moves")) moves = std::atoi(argv[i + 1]);
    }
    bench::BenchBoard b = bench::makeBenchBoard(spec);
    Project& p = b.project;
    auto t0 = Clock::now();
    p.pcb.autoPlace(p.schematic, true);
    const RouteStats st = p.pcb.autoRoute(p.schematic);
    std::printf("board: %d parts, %d nets, %d layers, %.0f × %.0f mm — %zu tracks, %zu vias, %d / %d routed (%.0f s)\n",
                b.components, b.nets, spec.layers, p.pcb.settings.width, p.pcb.settings.height, p.pcb.tracks.size(),
                p.pcb.vias.size(), st.routed, st.connections, ms(t0, Clock::now()) / 1000);
    const auto pads = p.pcb.pads(p.schematic);
    for (RouterMode mode : {RouterMode::Shove, RouterMode::Walkaround}) {
        Lcg rnd{7};
        std::vector<double> route, drag, begin;
        int done = 0;
        while (done < moves) {
            InteractiveRouter r(p.pcb, p.schematic);
            RouterOptions o;
            o.mode = mode;
            r.setOptions(o);
            const bool isDrag = rnd.next() < 0.3;
            auto tb = Clock::now();
            bool ok;
            if (isDrag) {
                const Track& t = p.pcb.tracks[static_cast<size_t>(rnd.next() * p.pcb.tracks.size()) % p.pcb.tracks.size()];
                ok = r.beginDrag(t.id, (t.a + t.b) * 0.5);
            } else {
                const Pad& pd = pads[static_cast<size_t>(rnd.next() * pads.size()) % pads.size()];
                ok = r.beginRoute(pd.position, pd.throughHole ? 0 : pd.smdLayer);
            }
            begin.push_back(ms(tb, Clock::now()));
            if (!ok) continue;
            const Vec2 at = r.preview().end;
            for (int m = 0; m < 5 && done < moves; ++m, ++done) {
                const double reach = isDrag ? 1.5 : 12;
                const Vec2 c{at.x + (rnd.next() - 0.5) * 2 * reach, at.y + (rnd.next() - 0.5) * 2 * reach};
                const auto t1 = Clock::now();
                r.moveTo(c);
                (isDrag ? drag : route).push_back(ms(t1, Clock::now()));
            }
            r.cancel();
        }
        std::printf("%s\n", mode == RouterMode::Shove ? "Shove" : "Walk around");
        report("route head", route);
        report("segment drag", drag);
        report("begin (board snapshot)", begin);
    }
    // Cancel latency: the slowest kind of update (aimed into another net's pad far away), cancelled after 1 ms.
    {
        Lcg rnd{11};
        std::vector<double> full, cancelled;
        std::atomic<unsigned> counter{0};
        for (int k = 0; k < 20; ++k) {
            const Pad& a = pads[static_cast<size_t>(rnd.next() * pads.size()) % pads.size()];
            const Pad& z = pads[static_cast<size_t>(rnd.next() * pads.size()) % pads.size()];
            InteractiveRouter r(p.pcb, p.schematic);
            r.setAbortSource(&counter);
            if (!r.beginRoute(a.position, a.throughHole ? 0 : a.smdLayer)) continue;
            auto t1 = Clock::now();
            r.moveTo(z.position);
            full.push_back(ms(t1, Clock::now()));
            r.moveTo(a.position);
            std::thread killer([&] {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
                counter.fetch_add(1);
            });
            t1 = Clock::now();
            r.moveTo(z.position);
            const double took = ms(t1, Clock::now());
            killer.join();
            if (r.preview().aborted) cancelled.push_back(took);
        }
        std::printf("Cancel\n");
        report("slow update, full", full);
        report("cancelled after 1 ms", cancelled);
    }
    return 0;
}
