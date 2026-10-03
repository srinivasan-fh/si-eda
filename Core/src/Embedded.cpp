#include "sieda/Embedded.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <sstream>

#include "sieda/Units.hpp"

namespace sieda {

const std::vector<double>& embeddedResistorSheets() {
    static const std::vector<double> sheets = {25, 50, 100, 250};
    return sheets;
}

bool canEmbed(const Component& c) { return c.kind == ComponentKind::Resistor || c.kind == ComponentKind::Capacitor; }

Rect EmbeddedElement::bounds() const {
    const bool turned = ((rotation / 90) % 2 + 2) % 2 == 1;
    const double w = turned ? width : length, h = turned ? length : width;
    return Rect::centered(centre, w, h);
}

std::optional<EmbeddedElement> embeddedElement(const Component& c, const BoardSettings& s) {
    if (!c.pcb.embedded() || !canEmbed(c) || !c.pcb.placed) return std::nullopt;
    const int n = s.layerCount;
    const bool resistor = c.kind == ComponentKind::Resistor;
    // Resistive foil on an inner layer; a capacitor needs two neighbouring inner layers for its plates.
    if (n < 4 || c.pcb.embeddedLayer < 1 || c.pcb.embeddedLayer > n - (resistor ? 2 : 3)) return std::nullopt;
    EmbeddedElement e;
    e.componentId = c.id;
    e.resistor = resistor;
    e.layer = c.pcb.embeddedLayer;
    e.layer2 = resistor ? e.layer : e.layer + 1;
    e.centre = c.pcb.position;
    e.rotation = c.pcb.rotation;
    auto v = parseEngineeringValue(primaryValue(c.value));
    e.value = v && *v > 0 ? *v : (resistor ? 1000 : 1e-9);
    if (resistor) {
        // Pick the foil whose number of squares (R / Rs) is closest to 3: compact, and long enough to trim.
        double best = 1e18;
        for (double rs : embeddedResistorSheets()) {
            const double sq = e.value / rs;
            const double score = std::fabs(std::log(sq / 3.0));
            if (score < best) {
                best = score;
                e.sheet = rs;
                e.squares = sq;
            }
        }
        e.inRange = e.squares >= 0.3 && e.squares <= 40;
        const double sq = std::clamp(e.squares, 0.3, 40.0);
        e.width = e.squares > 20 ? 0.25 : 0.5;  // very long resistors use the narrowest etchable line
        e.length = std::max(0.25, sq * e.width);
        e.area = e.width * e.length;
        e.powerRating = kEmbeddedResistorWattsPerMm2 * e.area;
        char buf[96];
        std::snprintf(buf, sizeof buf, "Thin-film NiP / NiCr foil %.0f Ω/sq (Ohmega-Ply / TICER)", e.sheet);
        e.material = buf;
    } else {
        e.sheet = kBuriedCapacitanceDensity;
        e.area = e.value * 1e9 / kBuriedCapacitanceDensity;  // mm²
        const double side = std::sqrt(e.area);
        e.inRange = side <= 12.0;  // larger plates belong in a power / ground plane pair, not a discrete part
        e.width = e.length = std::clamp(side, 0.5, 30.0);
        e.material = "Buried-capacitance laminate, 8 µm ceramic-filled (1.6 nF/cm²)";
    }
    return e;
}

std::vector<Pad> embeddedPads(const Component& c, const EmbeddedElement& e, const Schematic& sch) {
    std::vector<Pad> out;
    const bool turned = ((e.rotation / 90) % 2 + 2) % 2 == 1;
    auto make = [&](int pin, Vec2 offset, Vec2 size, int layer) {
        Pad p;
        p.componentId = c.id;
        p.pinIndex = pin;
        p.padNumber = pin + 1;
        p.net = sch.netOf({c.id, pin});
        p.position = e.centre + rotate90(offset, e.rotation);
        p.size = turned ? Vec2{size.y, size.x} : size;
        p.smdLayer = layer;
        out.push_back(p);
    };
    if (e.resistor) {
        // Copper terminations overlapping each end of the foil.
        const double tw = std::max(e.width, 0.4), tl = 0.5;
        make(0, {-(e.length / 2 + tl / 2 - 0.1), 0}, {tl, tw}, e.layer);
        make(1, {e.length / 2 + tl / 2 - 0.1, 0}, {tl, tw}, e.layer);
    } else {
        // Two plates facing each other through the thin laminate.
        make(0, {0, 0}, {e.width, e.width}, e.layer);
        make(1, {0, 0}, {e.width, e.width}, e.layer2);
    }
    return out;
}

std::vector<EmbeddedElement> embeddedElements(const Schematic& sch, const BoardSettings& s) {
    std::vector<EmbeddedElement> out;
    for (const auto& c : sch.components())
        if (auto e = embeddedElement(c, s)) out.push_back(*e);
    return out;
}

std::string exportEmbeddedResistorGerber(const Schematic& sch, const PcbLayout& pcb, int layer) {
    std::vector<EmbeddedElement> res;
    for (const auto& e : embeddedElements(sch, pcb.settings))
        if (e.resistor && e.layer == layer) res.push_back(e);
    if (res.empty()) return "";
    auto coord = [&](double x, double y) {
        char b[48];
        std::snprintf(b, sizeof b, "X%lldY%lld", std::llround(x * 1e6), std::llround((pcb.settings.height - y) * 1e6));
        return std::string(b);
    };
    std::ostringstream o;
    o << "%TF.GenerationSoftware,SiEDA*%\n%TF.FileFunction,Other,Resistor-element L" << layer + 1 << "*%\n"
      << "%TF.FilePolarity,Positive*%\n%FSLAX46Y46*%\n%MOMM*%\n%LPD*%\n";
    for (const auto& e : res) {
        const Rect r = e.bounds();
        o << "G36*\n" << coord(r.x0, r.y0) << "D02*\n" << coord(r.x1, r.y0) << "D01*\n" << coord(r.x1, r.y1) << "D01*\n"
          << coord(r.x0, r.y1) << "D01*\n" << coord(r.x0, r.y0) << "D01*\nG37*\n";
    }
    o << "M02*\n";
    return o.str();
}

}  // namespace sieda
