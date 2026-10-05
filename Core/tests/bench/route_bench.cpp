// sieda_route_bench — times auto-place, auto-route and DRC on generated large boards.
//
//   sieda_route_bench                      medium board (the CI case) and the large board
//   sieda_route_bench --clusters 32 --layers 8 --seed 3 [--fpga] [--no-bga] [--drc-brute]
//
// Prints component / net counts, the time of each stage, routing completion and the peak resident memory.
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#if defined(__unix__) || defined(__APPLE__)
#include <sys/resource.h>
#endif

#include "BoardGenerator.hpp"
#include "sieda/Pcb.hpp"

using namespace sieda;

namespace {
double peakMegabytes() {
#if defined(__unix__) || defined(__APPLE__)
    rusage u{};
    getrusage(RUSAGE_SELF, &u);
#if defined(__APPLE__)
    return static_cast<double>(u.ru_maxrss) / (1024.0 * 1024.0);  // bytes
#else
    return static_cast<double>(u.ru_maxrss) / 1024.0;  // kilobytes
#endif
#else
    return 0;
#endif
}

std::string g_snapshot;  // --snapshot: where to write the routed board (Project::snapshot JSON)

void run(const bench::BenchSpec& spec, bool brute) {
    using clock = std::chrono::steady_clock;
    auto secs = [](clock::time_point a) { return std::chrono::duration<double>(clock::now() - a).count(); };
    bench::BenchBoard b = bench::makeBenchBoard(spec);
    Project& p = b.project;
    std::printf("board: seed %u, %d clusters, %d layers%s%s — %d components, %d nets, %d pins, %.0f × %.0f mm\n",
                spec.seed, spec.clusters, spec.layers, spec.bga ? ", BGAs" : "", spec.fpga ? ", FPGA" : "",
                b.components, b.nets, b.pins, p.pcb.settings.width, p.pcb.settings.height);
    std::fflush(stdout);
    auto t0 = clock::now();
    p.pcb.autoPlace(p.schematic, true);
    const double place = secs(t0);
    std::printf("  place   %8.2f s   (board %.0f × %.0f mm)\n", place, p.pcb.settings.width, p.pcb.settings.height);
    std::fflush(stdout);
    t0 = clock::now();
    const RouteStats st = p.pcb.autoRoute(p.schematic);
    const double route = secs(t0);
    std::printf("  route   %8.2f s   %d / %d connections (%.1f %%), %d vias, %zu tracks, %.0f mm\n", route, st.routed,
                st.connections, st.connections ? 100.0 * st.routed / st.connections : 100.0, st.vias,
                p.pcb.tracks.size(), st.trackLength);
    if (std::getenv("SIEDA_BENCH_VERBOSE")) {
        std::printf("  failed nets:");
        for (const auto& n : st.failedNets) std::printf(" %s", n.c_str());
        std::printf("\n");
    }
    std::fflush(stdout);
    t0 = clock::now();
    const auto drc =p.pcb.runDRC(p.schematic);
    const double drcTime = secs(t0);
    int errors = 0, warnings = 0;
    for (const auto& v : drc) {
        errors += v.severity == Severity::Error;
        warnings += v.severity == Severity::Warning;
    }
    std::printf("  drc     %8.2f s   %d errors, %d warnings\n", drcTime, errors, warnings);
    if (brute) {
        setDrcBruteForce(true);
        t0 = clock::now();
        const auto ref = p.pcb.runDRC(p.schematic);
        const double bruteTime = secs(t0);
        setDrcBruteForce(false);
        bool same = ref.size() == drc.size();
        for (size_t i = 0; same && i < ref.size(); ++i)
            same = ref[i].code == drc[i].code && ref[i].message == drc[i].message;
        std::printf("  drc (brute force) %8.2f s — %s\n", bruteTime, same ? "identical" : "DIFFERENT");
    }
    if (!g_snapshot.empty())
        if (FILE* f = std::fopen(g_snapshot.c_str(), "w")) {
            const std::string json = p.snapshot().dump();
            std::fwrite(json.data(), 1, json.size(), f);
            std::fclose(f);
        }
    std::printf("  peak memory %.0f MB\n", peakMegabytes());
    std::fflush(stdout);
}
}  // namespace

int main(int argc, char** argv) {
    bench::BenchSpec spec;
    bool custom = false, brute = false;
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto num = [&](int& out) {
            if (i + 1 < argc) out = std::atoi(argv[++i]);
            custom = true;
        };
        if (a == "--clusters") num(spec.clusters);
        else if (a == "--layers") num(spec.layers);
        else if (a == "--seed") {
            int s = 1;
            num(s);
            spec.seed = static_cast<uint32_t>(s);
        } else if (a == "--fpga") spec.fpga = custom = true;
        else if (a == "--no-bga") {
            spec.bga = false;
            custom = true;
        } else if (a == "--drc-brute") brute = true;
        else if (a == "--snapshot" && i + 1 < argc) g_snapshot = argv[++i];
        else {
            std::printf("usage: sieda_route_bench [--clusters N] [--layers L] [--seed S] [--fpga] [--no-bga] [--drc-brute] [--snapshot file.json]\n");
            return 2;
        }
    }
    if (custom) {
        run(spec, brute);
        return 0;
    }
    // Default: the medium CI board, then the large board.
    run(bench::BenchSpec{1, 6, 4, true, false, 0.45}, brute);
    run(bench::BenchSpec{2, 32, 8, true, true, 0.45}, brute);
    return 0;
}
