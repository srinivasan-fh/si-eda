// SiEDA Core — interactive placement: one footprint at a candidate pose (cursor position snapped to the grid,
// rotation, side) with a legality report, committed only when legal (or when the caller insists), and a suggested
// starting spot next to the parts it connects to. Used by Update PCB's "place new parts" mode.
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>
#include <set>

#include "sieda/Embedded.hpp"
#include "sieda/Library.hpp"
#include "sieda/Project.hpp"

namespace sieda {

namespace {
constexpr double kEps = 1e-6;

int quarterTurn(int rotation) {
    const int r = static_cast<int>(std::lround(rotation / 90.0)) * 90;
    return ((r % 360) + 360) % 360;
}

double snap(double v, double grid) { return grid > 0 ? std::round(v / grid) * grid : v; }

/// Top (0), bottom (1) or inside the board (embedded, 100 + layer): courtyards collide only on one plane (as the DRC).
int plane(const Component& c, const BoardSettings& s) {
    if (auto e = embeddedElement(c, s)) return 100 + e->layer;
    return c.pcb.bottom ? 1 : 0;
}

/// Footprint pads of `c` at its (candidate) pose — the same geometry PcbLayout::pads gives a placed part.
std::vector<Pad> padsAt(const Component& c, const Schematic& sch, const BoardSettings& s) {
    if (auto e = embeddedElement(c, s)) return embeddedPads(c, *e, sch);
    std::vector<Pad> out;
    const FootprintDef* fp = Library::instance().footprint(c.footprintName());
    if (!fp) return out;
    const bool turned = ((c.pcb.rotation / 90) % 2 + 2) % 2 == 1;
    int number = 1;
    for (const auto& pd : fp->pads) {
        Pad p;
        p.componentId = c.id;
        p.pinIndex = pd.pinIndex;
        p.padNumber = number++;
        p.net = pd.pinIndex >= 0 ? sch.netOf({c.id, pd.pinIndex}) : -1;
        Vec2 local = pd.offset;
        if (c.pcb.bottom) local.x = -local.x;
        p.position = c.pcb.position + rotate90(local, c.pcb.rotation);
        p.size = turned ? Vec2{pd.size.y, pd.size.x} : pd.size;
        p.throughHole = pd.throughHole;
        p.round = pd.round;
        p.drill = pd.drill;
        p.bottom = c.pcb.bottom;
        p.smdLayer = c.pcb.bottom ? s.bottomLayer() : kTopLayer;
        out.push_back(p);
    }
    return out;
}

/// The issues of `cand` (a copy of the part at its candidate pose). `gap` > 0 keeps that much space to other
/// courtyards (the suggestion's search); the report itself uses 0, as the DRC does. Parts in `ignore` (sorted; the
/// placement queue still waiting) are no obstacles.
void collectIssues(const Project& p, const Component& cand, double gap, const std::vector<int>& ignore,
                   PlacementCheck& out) {
    const BoardSettings& s = p.pcb.settings;
    const Rect cy = p.pcb.courtyard(cand);
    out.courtyard = cy;
    if (cand.pcb.locked)
        out.issues.push_back({"PLACE_LOCKED", cand.ref + " is locked; unlock it to move it.", -1, true});
    // Outline and mounting holes (as DRC_OUT_OF_BOARD / DRC_HOLE_KEEPOUT). Not gated on rectInside: a segment
    // crossing the rectangle measures 0 there, which a negative margin accepts, so a part straddling the edge passed.
    {
        bool inHole = false;
        for (const auto& h : s.holes)
            if (pointRectDistance(h.position, cy) < h.keepout / 2 - kEps) inHole = true;
        const Rect inner = cy.inflated(-kEps);
        bool outside = !s.contains(cy.center());
        const auto poly = s.outlinePolygon();
        for (size_t a = 0, b = poly.size() - 1; a < poly.size() && !outside; b = a++)
            outside = segmentRectDistance(poly[b], poly[a], inner) <= 0;
        if (outside) out.issues.push_back({"PLACE_OUTSIDE", cand.ref + " would extend beyond the board outline.", -1, true});
        if (inHole) out.issues.push_back({"PLACE_HOLE", cand.ref + " would overlap a mounting-hole keep-out.", -1, true});
    }
    // Other parts' courtyards on the same side.
    const int own = plane(cand, s);
    const Rect probe = cy.inflated(gap);
    for (const auto& o : p.schematic.components()) {
        if (o.id == cand.id || !o.hasFootprint() || !o.pcb.placed || plane(o, s) != own) continue;
        if (std::binary_search(ignore.begin(), ignore.end(), o.id)) continue;
        if (p.pcb.courtyard(o).intersects(probe))
            out.issues.push_back({"PLACE_OVERLAP", cand.ref + " would overlap the courtyard of " + o.ref + ".", o.id, true});
    }
    // Routing keep-outs over its pads: legal, but no track can reach them there.
    for (const auto& k : s.keepouts) {
        if (!k.tracks) continue;
        const bool hit = std::any_of(out.pads.begin(), out.pads.end(), [&](const Pad& pad) {
            return (k.layer < 0 || pad.onLayer(k.layer)) && pad.bounds().intersects(k.area);
        });
        if (hit)
            out.issues.push_back({"PLACE_KEEPOUT",
                                  "Pads of " + cand.ref + " would lie inside the keep-out" + (k.name.empty() ? "" : " " + k.name) +
                                      ": no track can reach them.",
                                  -1, false});
    }
    out.legal = std::none_of(out.issues.begin(), out.issues.end(), [](const PlacementIssue& i) { return i.error; });
}

/// The part as it would stand at the pose; nullopt for an unknown part or one without a footprint.
std::optional<Component> candidate(const Schematic& sch, const BoardSettings& s, int id, Vec2 at, int rotation,
                                   bool bottom, double grid) {
    const Component* c = sch.find(id);
    if (!c || !c->hasFootprint() || isNetSymbolKind(c->kind) || !std::isfinite(at.x) || !std::isfinite(at.y))
        return std::nullopt;
    Component cand = *c;
    cand.pcb.position = {snap(at.x, grid), snap(at.y, grid)};
    cand.pcb.rotation = quarterTurn(rotation);
    cand.pcb.bottom = bottom && !embeddedElement(*c, s);  // an embedded part is inside the board
    cand.pcb.placed = true;
    return cand;
}

std::vector<int> sortedIds(const std::vector<int>& ids) {
    std::vector<int> out = ids;
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

PlacementCheck checkCandidate(const Project& p, const Component& cand, double gap, const std::vector<int>& ignore) {
    PlacementCheck out;
    out.component = cand.id;
    out.position = cand.pcb.position;
    out.rotation = cand.pcb.rotation;
    out.bottom = cand.pcb.bottom;
    out.pads = padsAt(cand, p.schematic, p.pcb.settings);
    collectIssues(p, cand, gap, ignore, out);
    return out;
}
}  // namespace

PlacementCheck Project::checkPlacement(int componentId, Vec2 at, int rotation, bool bottom, double grid,
                                       const std::vector<int>& ignore) const {
    const auto cand = candidate(schematic, pcb.settings, componentId, at, rotation, bottom, grid);
    if (!cand) return {};
    return checkCandidate(*this, *cand, 0, sortedIds(ignore));
}

PlacementCheck Project::placeComponent(int componentId, Vec2 at, int rotation, bool bottom, double grid,
                                       bool allowIllegal, const std::vector<int>& ignore) {
    PlacementCheck check = checkPlacement(componentId, at, rotation, bottom, grid, ignore);
    if (check.component < 0) return check;
    const bool locked = std::any_of(check.issues.begin(), check.issues.end(),
                                    [](const PlacementIssue& i) { return i.code == "PLACE_LOCKED"; });
    if ((!check.legal && !allowIllegal) || locked) return check;  // a locked part never moves
    Component* c = schematic.find(componentId);
    c->pcb.position = check.position;
    c->pcb.rotation = check.rotation;
    c->pcb.bottom = check.bottom;
    c->pcb.placed = true;
    schematicChanged();
    check.committed = true;
    return check;
}

PlacementCheck Project::suggestPlacement(int componentId, double grid, const std::vector<int>& ignoreIds) const {
    const std::vector<int> ignore = sortedIds(ignoreIds);
    const Component* c = schematic.find(componentId);
    if (!c || !c->hasFootprint() || isNetSymbolKind(c->kind)) return {};
    // The centroid of the pads it connects to on other placed parts, each net counting once.
    std::set<int> nets;
    for (size_t i = 0; i < c->def().pins.size(); ++i)
        if (const int n = schematic.netOf({c->id, static_cast<int>(i)}); n >= 0) nets.insert(n);
    std::map<int, std::pair<Vec2, int>> perNet;  // net → sum of pad positions, count
    if (!nets.empty())
        for (const Pad& pad : pcb.pads(schematic)) {
            if (pad.componentId == c->id || !nets.count(pad.net)) continue;
            auto& [sum, n] = perNet[pad.net];
            sum = sum + pad.position;
            ++n;
        }
    Vec2 target{pcb.settings.width / 2, pcb.settings.height / 2};
    if (!perNet.empty()) {
        Vec2 sum{0, 0};
        for (const auto& [net, acc] : perNet) sum = sum + acc.first * (1.0 / acc.second);
        target = sum * (1.0 / static_cast<double>(perNet.size()));
    }
    const int rotation = c->pcb.rotation;
    const bool bottom = c->pcb.bottom;
    // Rings of grid spots round the target, nearest legal spot first (a small gap keeps courtyards apart). The step
    // is a whole number of grid cells and coarse enough that at most ~200 rings cover the board.
    const double base = grid > 0 ? grid : 0.25;
    const double span = std::max(pcb.settings.width, pcb.settings.height);
    const double step = base * std::max(1.0, std::ceil(std::max(0.5, span / 200) / base));
    const int rings = static_cast<int>(std::ceil(span / step)) + 1;
    const Vec2 origin{snap(target.x, base), snap(target.y, base)};
    auto cand = candidate(schematic, pcb.settings, componentId, origin, rotation, bottom, base);
    if (!cand) return {};
    if (c->pcb.locked) return checkCandidate(*this, *cand, 0, ignore);  // reported as locked; nothing to search
    constexpr double kGap = 0.25;
    for (int r = 0; r <= rings; ++r) {
        bool found = false;
        Vec2 best;
        double bestD = 0;
        for (int i = -r; i <= r; ++i)
            for (int j = -r; j <= r; ++j) {
                if (std::max(std::abs(i), std::abs(j)) != r) continue;  // the ring only
                const Vec2 at{snap(origin.x + i * step, base), snap(origin.y + j * step, base)};
                const double d = (at - target).length();
                if (found && d >= bestD) continue;
                cand->pcb.position = at;
                PlacementCheck probe;
                collectIssues(*this, *cand, kGap, ignore, probe);
                if (!probe.legal) continue;
                found = true;
                best = at;
                bestD = d;
            }
        if (found) {
            cand->pcb.position = best;
            return checkCandidate(*this, *cand, 0, ignore);
        }
    }
    // Nowhere free: the spot next to the connections, reported as it is.
    cand->pcb.position = origin;
    return checkCandidate(*this, *cand, 0, ignore);
}

}  // namespace sieda
