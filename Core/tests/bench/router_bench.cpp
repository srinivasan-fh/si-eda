// sieda_router_bench — times interactive-router head updates on a generated, autorouted large board.
//
//   sieda_router_bench                    16 clusters (≈ 400 parts), 8 layers, seed 1
//   sieda_router_bench --clusters 6 --layers 6 --seed 1 --moves 400
//   sieda_router_bench --board routed.json  loads the autorouted board from the file (written there on first run)
//   sieda_router_bench --slowest 10         also lists the slowest updates (start, cursor) to reproduce them
//   sieda_router_bench --clusters 32 --fpga 1 --seed 3   the 923-part board of docs/ROUTING.md
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
#include <ctime>
#include <fstream>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "BoardGenerator.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Project.hpp"

using namespace sieda;

namespace {
using Clock = std::chrono::steady_clock;
double ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

/// CPU time of the calling thread (ms): with SIEDA_ROUTER_SERIAL=1 an update's own work, unaffected by other jobs
/// sharing the machine.
double cpuMs() {
    timespec ts{};
    clock_gettime(CLOCK_THREAD_CPUTIME_ID, &ts);
    return static_cast<double>(ts.tv_sec) * 1e3 + static_cast<double>(ts.tv_nsec) / 1e6;
}

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
    int slowest = 0;
    std::string boardFile;
    for (int i = 1; i + 1 < argc; i += 2) {
        if (!std::strcmp(argv[i], "--board")) boardFile = argv[i + 1];
        if (!std::strcmp(argv[i], "--slowest")) slowest = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--fpga")) spec.fpga = std::atoi(argv[i + 1]) != 0;
        if (!std::strcmp(argv[i], "--clusters")) spec.clusters = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--layers")) spec.layers = std::atoi(argv[i + 1]);
        if (!std::strcmp(argv[i], "--seed")) spec.seed = static_cast<uint32_t>(std::atoi(argv[i + 1]));
        if (!std::strcmp(argv[i], "--moves")) moves = std::atoi(argv[i + 1]);
    }
    bench::BenchBoard b = bench::makeBenchBoard(spec);
    Project& p = b.project;
    auto t0 = Clock::now();
    RouteStats st;
    bool loaded = false;
    if (!boardFile.empty()) {
        std::ifstream in(boardFile);
        if (in) {
            std::stringstream ss;
            ss << in.rdbuf();
            p = Project::fromJson(Json::parse(ss.str()));
            st.routed = st.connections = static_cast<int>(p.pcb.tracks.size() > 0);
            loaded = true;
        }
    }
    if (!loaded) {
        p.pcb.autoPlace(p.schematic, true);
        st = p.pcb.autoRoute(p.schematic);
        if (!boardFile.empty()) std::ofstream(boardFile) << p.toJson().dump();
    }
    std::printf("board: %d parts, %d nets, %d layers, %.0f × %.0f mm — %zu tracks, %zu vias, %d / %d routed (%.0f s)\n",
                b.components, b.nets, spec.layers, p.pcb.settings.width, p.pcb.settings.height, p.pcb.tracks.size(),
                p.pcb.vias.size(), st.routed, st.connections, ms(t0, Clock::now()) / 1000);
    const auto pads = p.pcb.pads(p.schematic);
    for (RouterMode mode : {RouterMode::Shove, RouterMode::Walkaround}) {
        Lcg rnd{7};
        std::vector<double> route, drag, begin, routeCpu, dragCpu;
        struct Slow {
            double ms;
            bool drag;
            Vec2 from, to;
        };
        std::vector<Slow> slow;
        int done = 0;
        while (done < moves) {
            InteractiveRouter r(p.pcb, p.schematic);
            RouterOptions o;
            o.mode = mode;
            r.setOptions(o);
            const bool isDrag = rnd.next() < 0.3;
            auto tb = Clock::now();
            bool ok;
            Vec2 from;
            if (isDrag) {
                const Track& t = p.pcb.tracks[static_cast<size_t>(rnd.next() * p.pcb.tracks.size()) % p.pcb.tracks.size()];
                from = (t.a + t.b) * 0.5;
                ok = r.beginDrag(t.id, from);
            } else {
                const Pad& pd = pads[static_cast<size_t>(rnd.next() * pads.size()) % pads.size()];
                from = pd.position;
                ok = r.beginRoute(pd.position, pd.throughHole ? 0 : pd.smdLayer);
            }
            begin.push_back(ms(tb, Clock::now()));
            if (!ok) continue;
            const Vec2 at = r.preview().end;
            for (int m = 0; m < 5 && done < moves; ++m, ++done) {
                const double reach = isDrag ? 1.5 : 12;
                const Vec2 c{at.x + (rnd.next() - 0.5) * 2 * reach, at.y + (rnd.next() - 0.5) * 2 * reach};
                const auto t1 = Clock::now();
                const double c1 = cpuMs();
                r.moveTo(c);
                const double took = ms(t1, Clock::now());
                (isDrag ? drag : route).push_back(took);
                (isDrag ? dragCpu : routeCpu).push_back(cpuMs() - c1);
                slow.push_back({took, isDrag, from, c});
            }
            r.cancel();
        }
        std::printf("%s\n", mode == RouterMode::Shove ? "Shove" : "Walk around");
        report("route head", route);
        report("segment drag", drag);
        report("begin (board snapshot)", begin);
        report("route head (CPU)", routeCpu);
        report("segment drag (CPU)", dragCpu);
        std::sort(slow.begin(), slow.end(), [](const Slow& a, const Slow& b) { return a.ms > b.ms; });
        for (int k = 0; k < slowest && k < static_cast<int>(slow.size()); ++k)
            std::printf("    %7.2f ms %s from (%.4f, %.4f) to (%.4f, %.4f)\n", slow[static_cast<size_t>(k)].ms,
                        slow[static_cast<size_t>(k)].drag ? "drag " : "route", slow[static_cast<size_t>(k)].from.x,
                        slow[static_cast<size_t>(k)].from.y, slow[static_cast<size_t>(k)].to.x, slow[static_cast<size_t>(k)].to.y);
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
