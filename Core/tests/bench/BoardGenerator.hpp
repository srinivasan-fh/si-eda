// SiEDA scale benchmark — deterministic generator of large boards (ICs + BGAs + passives) for timing placement,
// routing and DRC. Header-only so both the unit tests and the `sieda_route_bench` tool use the same boards.
#pragma once

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include "sieda/CustomParts.hpp"
#include "sieda/Project.hpp"
#include "sieda/StandardParts.hpp"

namespace sieda::bench {

struct BenchSpec {
    uint32_t seed = 1;
    int clusters = 6;       // one IC per cluster, with its passives
    int layers = 4;         // copper layers; ≥ 4 get a GND plane (layer 1) and a +3V3 plane (layer n-2)
    bool bga = true;        // DDR3-style 96-ball 0.8 mm BGAs in the IC mix
    bool fpga = false;      // cluster 0 is a 324-ball FPGA
    double crossLinks = 0.45;  // share of IC signal pins wired to another cluster (through a series resistor)
};

struct BenchBoard {
    Project project;
    int components = 0;  // parts with a footprint
    int nets = 0;        // nets with ≥ 2 pins
    int pins = 0;        // pins on those nets
};

namespace detail {
/// Small deterministic generator (same sequence on every platform and compiler).
struct Lcg {
    uint32_t s;
    uint32_t next() {
        s = s * 1664525u + 1013904223u;
        return s >> 8;
    }
    double unit() { return static_cast<double>(next() & 0xFFFFFF) / static_cast<double>(0x1000000); }
};

inline bool isGroundPin(const std::string& n) {
    return n == "GND" || n == "VSS" || n == "VSSQ" || n == "VSSA" || n == "EP" || n.rfind("GND", 0) == 0 ||
           n.rfind("VSS", 0) == 0;
}
inline bool isSupplyPin(const std::string& n) {
    return n.rfind("VDD", 0) == 0 || n.rfind("VCC", 0) == 0 || n == "V+" || n == "VBAT" || n == "VIO" ||
           n.rfind("VREF", 0) == 0 || n.rfind("AVDD", 0) == 0 || n.rfind("DVDD", 0) == 0 || n == "IOVDD";
}
}  // namespace detail

/// Builds the benchmark board: `clusters` ICs (BGA / QFN / LQFP / TSSOP / SOIC) each surrounded by its decoupling
/// capacitors, pull-ups, filter capacitors and series resistors, with cross-links between neighbouring clusters.
/// The board outline is sized for about 35 % courtyard fill; components are left unplaced (run autoPlace).
inline BenchBoard makeBenchBoard(const BenchSpec& spec) {
    BenchBoard b;
    Project& p = b.project;
    Schematic& s = p.schematic;
    detail::Lcg rng{spec.seed * 2654435761u + 12345u};
    const char* mix[] = {"MT41K256M16HA-125", "STM32G431CBU6", "STM32F446RET6", "TXS0108EPWR",
                         "STM32F103C8T6",     "ADS131M04IPWR", "nRF52832",      "TCA9548APWR"};
    const int gnd = s.addComponent(ComponentKind::Ground, "", {0, 0});
    const int vcc = s.addComponent(ComponentKind::NetLabel, "+3V3", {0, -100});
    struct Ic {
        int id;
        std::vector<int> signals;  // free signal pins
        size_t next = 0;
    };
    std::vector<Ic> ics;
    std::vector<std::string> partIds;
    for (int k = 0; k < spec.clusters; ++k) {
        std::string name = mix[k % 8];
        if (!spec.bga && k % 8 == 0) name = "STM32F407VGT6";
        if (spec.fpga && k == 0) name = "XC7A35T-1CSG324I";
        const StandardPart* sp = findStandardPart(name);
        if (!sp) continue;
        const std::string id = p.addCustomPart(sp->spec);
        Ic ic;
        ic.id = s.addCustomComponent(id, "", {200.0 * (k % 10), 300.0 * (k / 10)});
        const auto& pins = s.find(ic.id)->def().pins;
        int supplies = 0;
        for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
            const auto& pin = pins[static_cast<size_t>(i)];
            if (pin.type == static_cast<int>(PinType::NoConnect) || pin.name == "NC") continue;
            if (detail::isGroundPin(pin.name)) s.connect({ic.id, i}, {gnd, 0});
            else if (detail::isSupplyPin(pin.name)) {
                s.connect({ic.id, i}, {vcc, 0});
                // One 100 nF decoupling capacitor per two supply pins.
                if (supplies++ % 2 == 0) {
                    const int c = s.addComponent(ComponentKind::Capacitor, "100n", {0, 0});
                    s.setPackage(c, "C_0603");
                    s.connect({c, 0}, {ic.id, i});
                    s.connect({c, 1}, {gnd, 0});
                }
            } else ic.signals.push_back(i);
        }
        ics.push_back(ic);
    }
    const int n = static_cast<int>(ics.size());
    for (int k = 0; k < n; ++k) {
        Ic& ic = ics[static_cast<size_t>(k)];
        while (ic.next < ic.signals.size()) {
            const int pin = ic.signals[ic.next++];
            const double r = rng.unit();
            if (r < spec.crossLinks && n > 1) {
                // Series resistor to a free signal pin of one of the next three clusters (a bus between neighbours).
                Ic* other = nullptr;
                for (int step = 1; step <= 3 && !other; ++step) {
                    Ic& o = ics[static_cast<size_t>((k + step + static_cast<int>(rng.next() % 2)) % n)];
                    if (&o != &ic && o.next < o.signals.size()) other = &o;
                }
                if (!other) continue;
                const int opin = other->signals[other->next++];
                const int res = s.addComponent(ComponentKind::Resistor, "33", {0, 0});
                s.setPackage(res, "R_0603");
                s.connect({ic.id, pin}, {res, 0});
                s.connect({res, 1}, {other->id, opin});
            } else if (r < spec.crossLinks + 0.25) {
                const int res = s.addComponent(ComponentKind::Resistor, "10k", {0, 0});
                s.setPackage(res, "R_0603");
                s.connect({ic.id, pin}, {res, 0});
                s.connect({res, 1}, {vcc, 0});
            } else if (r < spec.crossLinks + 0.40) {
                const int c = s.addComponent(ComponentKind::Capacitor, "1n", {0, 0});
                s.setPackage(c, "C_0603");
                s.connect({ic.id, pin}, {c, 0});
                s.connect({c, 1}, {gnd, 0});
            }
            // else: left open (unused I/O)
        }
    }
    for (const auto& c : s.components()) b.components += c.hasFootprint() ? 1 : 0;
    for (const auto& net : s.nets())
        if (net.pins.size() >= 2) {
            ++b.nets;
            b.pins += static_cast<int>(net.pins.size());
        }
    // Board: about 35 % of the area under courtyards, 4:3.
    double area = 0;
    for (const auto& c : s.components())
        if (c.hasFootprint()) {
            const Rect cy = p.pcb.courtyard(c);
            area += (cy.width() + 1.2) * (cy.height() + 1.2);
        }
    const double side = std::sqrt(area / 0.35 / 12.0);
    p.pcb.settings.applyPreset("HDI / Fine-Pitch BGA (IPC-2226)");
    p.pcb.settings.width = std::ceil(4 * side);
    p.pcb.settings.height = std::ceil(3 * side);
    p.pcb.settings.layerCount = spec.layers;
    if (spec.layers >= 4) {
        p.pcb.zones.push_back({"GND", 1, true, 0});
        p.pcb.zones.push_back({"+3V3", spec.layers - 2, true, 0});
    }
    return b;
}

}  // namespace sieda::bench
