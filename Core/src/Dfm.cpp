// SiEDA Core — manufacturer DFM / DFA rule packs (sieda/Dfm.hpp).
#include "sieda/Dfm.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>

#include "sieda/Library.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda {

const std::vector<DfmPack>& dfmPacks() {
    // id, name, maker, notes | track, space, drill, ring, hole-hole, via Ø | mask web, silk-pad, copper-edge | layers,
    // thickness min / max, size | part spacing, part-edge | max aspect ratio
    static const std::vector<DfmPack> packs = {
        {"jlcpcb-standard", "JLCPCB Standard (1–2 layers)", "JLCPCB", "Low-cost prototypes; assembly needs 3 mm rails or edge clearance",
         0.127, 0.127, 0.30, 0.13, 0.50, 0.45, 0.10, 0.15, 0.30, 2, 0.4, 2.4, 500, 400, 0.30, 3.0, 8},
        {"jlcpcb-advanced", "JLCPCB Multilayer (4–20 layers)", "JLCPCB", "Multilayer service; 0.09 mm track / space on inner layers",
         0.09, 0.09, 0.15, 0.075, 0.25, 0.25, 0.10, 0.15, 0.30, 20, 0.6, 2.5, 500, 400, 0.30, 3.0, 10},
        {"pcbway-standard", "PCBWay Standard", "PCBWay", "Standard process; tighter options cost extra",
         0.10, 0.10, 0.20, 0.15, 0.50, 0.45, 0.10, 0.15, 0.30, 14, 0.4, 3.2, 500, 1100, 0.30, 3.0, 10},
        {"oshpark-2", "OSH Park 2-layer", "OSH Park", "6 / 6 mil, 10 mil drill, ENIG; no assembly",
         0.152, 0.152, 0.254, 0.127, 0.381, 0.508, 0.10, 0.127, 0.381, 2, 1.6, 1.6, 400, 400, 0.30, 1.0, 7},
        {"oshpark-4", "OSH Park 4-layer", "OSH Park", "5 / 5 mil, 10 mil drill, 4 mil annular ring",
         0.127, 0.127, 0.254, 0.102, 0.381, 0.457, 0.10, 0.127, 0.381, 4, 1.6, 1.6, 400, 400, 0.30, 1.0, 7},
        {"eurocircuits-6c", "Eurocircuits pattern class 6, drill class C", "Eurocircuits", "0.150 mm track / space, 0.35 mm finished drill",
         0.150, 0.150, 0.35, 0.125, 0.40, 0.60, 0.10, 0.125, 0.40, 16, 0.5, 3.2, 580, 425, 0.30, 3.0, 8},
        {"ipc-class3", "IPC Class 3 / Aerospace (conservative)", "IPC-6012 Class 3", "High-reliability builds: wider rings and spacing",
         0.15, 0.15, 0.25, 0.15, 0.50, 0.55, 0.10, 0.20, 0.50, 24, 0.8, 3.2, 600, 500, 0.50, 5.0, 8},
    };
    return packs;
}

namespace {
/// A pack value by name (maxLayers, an int, is handled by the callers).
double* field(DfmPack& p, const std::string& k) {
    static const std::map<std::string, double DfmPack::*> m{
        {"minTrack", &DfmPack::minTrack}, {"minSpace", &DfmPack::minSpace}, {"minDrill", &DfmPack::minDrill},
        {"minAnnularRing", &DfmPack::minAnnularRing}, {"minHoleToHole", &DfmPack::minHoleToHole},
        {"minViaDiameter", &DfmPack::minViaDiameter}, {"minMaskSliver", &DfmPack::minMaskSliver},
        {"minSilkToPad", &DfmPack::minSilkToPad}, {"minEdgeCopper", &DfmPack::minEdgeCopper},
        {"minThickness", &DfmPack::minThickness}, {"maxThickness", &DfmPack::maxThickness}, {"maxWidth", &DfmPack::maxWidth},
        {"maxHeight", &DfmPack::maxHeight}, {"minPartSpacing", &DfmPack::minPartSpacing},
        {"minPartToEdge", &DfmPack::minPartToEdge}, {"maxAspectRatio", &DfmPack::maxAspectRatio}};
    const auto it = m.find(k);
    return it == m.end() ? nullptr : &(p.*(it->second));
}
}  // namespace

const std::vector<std::string>& dfmFieldNames() {
    static const std::vector<std::string> n{"minTrack", "minSpace", "minDrill", "minAnnularRing", "minHoleToHole", "minViaDiameter",
                                            "minMaskSliver", "minSilkToPad", "minEdgeCopper", "minThickness", "maxThickness",
                                            "maxWidth", "maxHeight", "minPartSpacing", "minPartToEdge", "maxAspectRatio", "maxLayers"};
    return n;
}

std::optional<DfmPack> boardDfmPack(const BoardSettings& s) {
    const DfmPack* base = findDfmPack(s.dfmPack);
    if (!base) return std::nullopt;
    DfmPack p = *base;
    for (const auto& [k, v] : s.dfmOverrides)
        if (k == "maxLayers") p.maxLayers = static_cast<int>(std::lround(v));
        else if (double* f = field(p, k)) *f = v;
    return p;
}

const DfmPack* findDfmPack(const std::string& id) {
    for (const auto& p : dfmPacks())
        if (p.id == id) return &p;
    return nullptr;
}

Json dfmPacksJson() {
    Json arr = Json::array();
    for (const auto& p : dfmPacks()) {
        Json j = Json::object();
        j["id"] = p.id;
        j["name"] = p.name;
        j["maker"] = p.maker;
        j["notes"] = p.notes;
        DfmPack q = p;
        for (const auto& k : dfmFieldNames()) j[k] = k == "maxLayers" ? Json(q.maxLayers) : Json(*field(q, k));
        arr.push(j);
    }
    return arr;
}

bool applyDfmPack(BoardSettings& s, const std::string& id) {
    if (id.empty()) {
        s.dfmPack.clear();
        return true;
    }
    if (!findDfmPack(id)) return false;
    s.dfmPack = id;
    const auto p = boardDfmPack(s);
    s.minTrackWidth = std::max(s.minTrackWidth, p->minTrack);
    s.minClearance = std::max(s.minClearance, p->minSpace);
    s.minDrill = std::max(s.minDrill, p->minDrill);
    s.minAnnularRing = std::max(s.minAnnularRing, p->minAnnularRing);
    s.minHoleToHole = std::max(s.minHoleToHole, p->minHoleToHole);
    s.edgeClearance = std::max(s.edgeClearance, p->minEdgeCopper);
    return true;
}

namespace {

std::string mm(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f mm", v);
    return b;
}

std::string fmt(const char* f, double v) {
    char b[48];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

/// Gap between two rectangles (0 when they touch or overlap).
double gap(const Rect& a, const Rect& b) {
    const double dx = std::max({0.0, b.x0 - a.x1, a.x0 - b.x1}), dy = std::max({0.0, b.y0 - a.y1, a.y0 - b.y1});
    return std::hypot(dx, dy);
}

double pointSegment(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = ab.x * ab.x + ab.y * ab.y;
    const double t = len2 > 0 ? std::clamp(((p.x - a.x) * ab.x + (p.y - a.y) * ab.y) / len2, 0.0, 1.0) : 0.0;
    return (p - (a + ab * t)).length();
}

}  // namespace

std::vector<RuleViolation> dfmChecks(const Schematic& sch, const PcbLayout& pcb) {
    std::vector<RuleViolation> out;
    const BoardSettings& s = pcb.settings;
    const auto p = boardDfmPack(s);
    if (!p) return out;
    auto add = [&](Severity sev, const char* code, const std::string& msg, std::vector<int> comps = {}, Vec2 at = {},
                   bool located = false) {
        RuleViolation v;
        v.severity = sev;
        v.code = code;
        v.message = msg + " (" + p->name + ")";
        v.components = std::move(comps);
        v.location = at;
        v.hasLocation = located;
        out.push_back(std::move(v));
    };

    // Fabrication limits of the whole board.
    if (s.layerCount > p->maxLayers)
        add(Severity::Error, "DFM_LAYERS", std::to_string(s.layerCount) + " layers; this service builds at most " +
                                               std::to_string(p->maxLayers));
    if (std::max(s.width, s.height) > std::max(p->maxWidth, p->maxHeight) ||
        std::min(s.width, s.height) > std::min(p->maxWidth, p->maxHeight))
        add(Severity::Error, "DFM_BOARD_SIZE", "Board " + mm(s.width) + " × " + mm(s.height) + " is larger than the " +
                                                   mm(p->maxWidth) + " × " + mm(p->maxHeight) + " the service builds");
    if (s.thickness < p->minThickness - 1e-9 || s.thickness > p->maxThickness + 1e-9)
        add(Severity::Error, "DFM_THICKNESS", "Board thickness " + mm(s.thickness) + " is outside " + mm(p->minThickness) +
                                                  " – " + mm(p->maxThickness));
    int smallVias = 0;
    Vec2 firstVia;
    for (const auto& v : pcb.vias)
        if (v.diameter < p->minViaDiameter - 1e-9 && smallVias++ == 0) firstVia = v.position;
    if (smallVias)
        add(Severity::Error, "DFM_VIA_SIZE", std::to_string(smallVias) + " via(s) smaller than the " +
                                                 mm(p->minViaDiameter) + " pad the service drills", {}, firstVia, true);

    const auto pads = pcb.pads(sch);
    std::map<int, std::vector<const Pad*>> byPart;
    for (const auto& pad : pads) byPart[pad.componentId].push_back(&pad);

    // Solder-mask web between neighbouring pads of a part (mask openings grow 0.05 mm per side).
    for (const auto& [id, list] : byPart) {
        double web = 1e9;
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j) {
                if (list[i]->smdLayer != list[j]->smdLayer && !list[i]->throughHole && !list[j]->throughHole) continue;
                const double g = gap(list[i]->bounds(), list[j]->bounds()) - 2 * 0.05;
                if (g > 0) web = std::min(web, g);
            }
        if (web < p->minMaskSliver - 1e-9) {
            const Component* c = sch.find(id);
            add(Severity::Warning, "DFM_MASK_SLIVER",
                (c ? c->ref : std::string("A part")) + ": " + mm(web) + " solder-mask web between pads, below " +
                    mm(p->minMaskSliver) + " (the fab opens the mask over the gap: no solder dam)",
                {id}, list.front()->position, true);
        }
    }

    // Silkscreen outlines (courtyard − 0.15 mm, 0.15 mm wide) over other parts' pads on the same side.
    std::vector<const Component*> placed;
    for (const auto& c : sch.components())
        if (c.hasFootprint() && c.pcb.placed && !c.pcb.embedded()) placed.push_back(&c);
    for (const Component* c : placed) {
        const Rect r = pcb.courtyard(*c).inflated(-0.15);
        const int side = c->pcb.bottom ? s.bottomLayer() : 0;
        for (const auto& pad : pads) {
            if (pad.componentId == c->id || !pad.onLayer(side)) continue;
            const Rect pr = pad.bounds().inflated(p->minSilkToPad + 0.075);
            const bool crosses = pr.intersects(r) && !(pr.x0 > r.x0 && pr.x1 < r.x1 && pr.y0 > r.y0 && pr.y1 < r.y1);
            if (crosses) {
                add(Severity::Warning, "DFM_SILK_TO_PAD",
                    c->ref + ": silkscreen outline runs within " + mm(p->minSilkToPad) + " of a pad (the fab clips it)", {c->id},
                    pad.position, true);
                break;
            }
        }
    }

    // Assembly: spacing between parts on the same side, distance to the board edge.
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j) {
            const Component *a = placed[i], *b = placed[j];
            if (a->pcb.bottom != b->pcb.bottom) continue;
            const Rect ra = pcb.courtyard(*a), rb = pcb.courtyard(*b);
            if (ra.intersects(rb)) continue;  // overlaps are DRC_COURTYARD_OVERLAP
            const double g = gap(ra, rb);
            if (g < p->minPartSpacing - 1e-9)
                add(Severity::Warning, "DFA_PART_SPACING",
                    a->ref + " and " + b->ref + " are " + mm(g) + " apart; placement needs " + mm(p->minPartSpacing),
                    {a->id, b->id}, (ra.center() + rb.center()) * 0.5, true);
        }
    const bool rails = s.panel.enabled() && s.panel.rail >= p->minPartToEdge;
    const auto outline = s.outlinePolygon();
    if (!rails)
        for (const Component* c : placed) {
            const Rect r = pcb.courtyard(*c);
            double d = 1e9;
            for (size_t i = 0; i < outline.size(); ++i) {
                const Vec2 a = outline[i], b = outline[(i + 1) % outline.size()];
                for (Vec2 corner : {Vec2{r.x0, r.y0}, Vec2{r.x1, r.y0}, Vec2{r.x1, r.y1}, Vec2{r.x0, r.y1}})
                    d = std::min(d, pointSegment(corner, a, b));
            }
            if (d < p->minPartToEdge - 1e-9)
                add(Severity::Warning, "DFA_PART_TO_EDGE",
                    c->ref + " is " + mm(d) + " from the board edge; the assembly line needs " + mm(p->minPartToEdge) +
                        " (or a panel with rails)",
                    {c->id}, r.center(), true);
        }

    // Fine-pitch parts (pads closer than 0.65 mm centre to centre): fiducials, and tall neighbours that shadow them.
    std::vector<const Component*> finePitch;
    for (const auto& [id, list] : byPart) {
        double pitch = 1e9;
        for (size_t i = 0; i < list.size(); ++i)
            for (size_t j = i + 1; j < list.size(); ++j)
                if (!list[i]->throughHole && !list[j]->throughHole)
                    pitch = std::min(pitch, (list[i]->position - list[j]->position).length());
        if (pitch <= 0.65)
            if (const Component* c = sch.find(id)) finePitch.push_back(c);
    }
    if (!finePitch.empty() && !(s.panel.enabled() && s.panel.rail >= 4))
        add(Severity::Warning, "DFA_FIDUCIALS",
            std::to_string(finePitch.size()) + " fine-pitch part(s) (" + finePitch.front()->ref +
                " …): add fiducials, e.g. a production panel with rails (Board Setup → Production Panel)",
            {finePitch.front()->id});
    auto height = [](const Component& c) {
        const FootprintDef* fp = Library::instance().footprint(c.footprintName());
        return fp ? fp->body.height : 0.0;
    };
    for (const Component* f : finePitch)
        for (const Component* c : placed) {
            if (c == f || c->pcb.bottom != f->pcb.bottom || height(*c) <= 5.0) continue;
            if (gap(pcb.courtyard(*c), pcb.courtyard(*f)) < 3.0) {
                add(Severity::Info, "DFA_TALL_NEAR_FINE_PITCH",
                    c->ref + " (" + mm(height(*c)) + " tall) stands within 3 mm of fine-pitch " + f->ref +
                        ": it shadows inspection and rework",
                    {c->id, f->id}, pcb.courtyard(*c).center(), true);
            }
        }
    for (const Component* c : placed)
        if (c->pcb.bottom && height(*c) > 8.0)
            add(Severity::Warning, "DFA_HEAVY_BOTTOM",
                c->ref + " (" + mm(height(*c)) + " tall) on the bottom side may drop off in the second reflow; move it to the top",
                {c->id}, pcb.courtyard(*c).center(), true);
    // Plated holes deeper than the service plates reliably.
    int deep = 0;
    double worst = 0;
    Vec2 deepAt;
    for (const auto& v : pcb.vias) {
        const double r = viaBarrelDepth(s, v) / std::max(v.drill, 1e-3);
        if (r > p->maxAspectRatio + 1e-9 && deep++ == 0) deepAt = v.position;
        worst = std::max(worst, r);
    }
    for (const auto& pad : pads)
        if (pad.throughHole && pad.drill > 0) {
            const double r = s.thickness / pad.drill;
            if (r > p->maxAspectRatio + 1e-9 && deep++ == 0) deepAt = pad.position;
            worst = std::max(worst, r);
        }
    if (deep)
        add(Severity::Error, "DFM_ASPECT_RATIO",
            std::to_string(deep) + " plated hole(s) up to " + fmt("%.1f", worst) + ":1 (depth ÷ drill), above the " +
                fmt("%.0f", p->maxAspectRatio) + ":1 the service plates; use a larger drill or a thinner board",
            {}, deepAt, true);
    // Copper balance between mirror layers (1 ↔ n, 2 ↔ n−1 …): uneven copper bows and twists the board in reflow.
    const auto cover = copperCoverage(sch, pcb);
    const int n = static_cast<int>(cover.size());
    for (int k = 0; k < n / 2; ++k)
        if (std::fabs(cover[static_cast<size_t>(k)] - cover[static_cast<size_t>(n - 1 - k)]) > 0.35)
            add(Severity::Warning, "DFM_COPPER_BALANCE",
                copperLayerName(k, n) + " has " + fmt("%.0f %%", cover[static_cast<size_t>(k)] * 100) + " copper, " +
                    copperLayerName(n - 1 - k, n) + " " + fmt("%.0f %%", cover[static_cast<size_t>(n - 1 - k)] * 100) +
                    ": balance them (a pour or copper thieving on the lighter layer) to limit bow and twist");
    return out;
}

std::vector<double> copperCoverage(const Schematic& sch, const PcbLayout& pcb) {
    const BoardSettings& s = pcb.settings;
    const int n = std::max(1, s.layerCount);
    std::vector<double> area(static_cast<size_t>(n), 0);
    for (const auto& t : pcb.tracks)
        if (t.layer >= 0 && t.layer < n) area[static_cast<size_t>(t.layer)] += trackLength(t) * t.width;
    for (const auto& pad : pcb.pads(sch))
        for (int l = 0; l < n; ++l)
            if (pad.onLayer(l)) area[static_cast<size_t>(l)] += pad.size.x * pad.size.y;
    for (const auto& f : pcb.zoneFills(sch))
        if (f.layer >= 0 && f.layer < n) area[static_cast<size_t>(f.layer)] += f.area();
    double board = 0;
    const auto poly = s.outlinePolygon();
    for (size_t i = 0; i < poly.size(); ++i) board += poly[i].x * poly[(i + 1) % poly.size()].y - poly[(i + 1) % poly.size()].x * poly[i].y;
    board = std::max(1.0, std::fabs(board) / 2);
    for (auto& a : area) a = std::min(1.0, a / board);
    return area;
}

Json dfmReportJson(const Schematic& sch, const PcbLayout& pcb) {
    const BoardSettings& s = pcb.settings;
    Json j = Json::object();
    j["pack"] = s.dfmPack;
    const auto p = boardDfmPack(s);
    if (!p) return j;
    j["name"] = p->name;
    if (!s.dfmOverrides.empty()) {
        Json o = Json::object();
        for (const auto& [k, v] : s.dfmOverrides) o[k] = v;
        j["overrides"] = o;
    }
    Json rows = Json::array();
    bool pass = true;
    auto row = [&](const std::string& rule, const std::string& actual, const std::string& limit, bool ok) {
        Json r = Json::object();
        r["rule"] = rule, r["actual"] = actual, r["limit"] = limit, r["ok"] = ok;
        rows.push(r);
        pass = pass && ok;
    };
    double minTrack = 1e9, minDrill = 1e9, minRing = 1e9, minVia = 1e9, aspect = 0;
    for (const auto& t : pcb.tracks)
        if (!t.teardrop) minTrack = std::min(minTrack, t.width);
    for (const auto& v : pcb.vias) {
        minDrill = std::min(minDrill, v.drill), minVia = std::min(minVia, v.diameter);
        minRing = std::min(minRing, (v.diameter - v.drill) / 2);
        aspect = std::max(aspect, viaBarrelDepth(s, v) / std::max(v.drill, 1e-3));
    }
    for (const auto& pad : pcb.pads(sch))
        if (pad.throughHole && pad.drill > 0) {
            minDrill = std::min(minDrill, pad.drill);
            minRing = std::min(minRing, (std::min(pad.size.x, pad.size.y) - pad.drill) / 2);
            aspect = std::max(aspect, s.thickness / pad.drill);
        }
    auto val = [&](double v) { return v < 1e8 ? mm(v) : std::string("—"); };
    row("Layers", std::to_string(s.layerCount), "≤ " + std::to_string(p->maxLayers), s.layerCount <= p->maxLayers);
    row("Board size", mm(s.width) + " × " + mm(s.height), mm(p->maxWidth) + " × " + mm(p->maxHeight),
        std::max(s.width, s.height) <= std::max(p->maxWidth, p->maxHeight) &&
            std::min(s.width, s.height) <= std::min(p->maxWidth, p->maxHeight));
    row("Thickness", mm(s.thickness), mm(p->minThickness) + " – " + mm(p->maxThickness),
        s.thickness >= p->minThickness - 1e-9 && s.thickness <= p->maxThickness + 1e-9);
    row("Narrowest track", val(minTrack), "≥ " + mm(p->minTrack), minTrack >= p->minTrack - 1e-9);
    row("Clearance rule", mm(s.clearance), "≥ " + mm(p->minSpace), s.clearance >= p->minSpace - 1e-9);
    row("Smallest drill", val(minDrill), "≥ " + mm(p->minDrill), minDrill >= p->minDrill - 1e-9);
    row("Smallest annular ring", val(minRing), "≥ " + mm(p->minAnnularRing), minRing >= p->minAnnularRing - 1e-9);
    row("Smallest via pad", val(minVia), "≥ " + mm(p->minViaDiameter), minVia >= p->minViaDiameter - 1e-9);
    row("Deepest hole (aspect ratio)", aspect > 0 ? fmt("%.1f:1", aspect) : "—", "≤ " + fmt("%.0f:1", p->maxAspectRatio),
        aspect <= p->maxAspectRatio + 1e-9);
    const auto cover = copperCoverage(sch, pcb);
    double imbalance = 0;
    for (size_t k = 0; k < cover.size() / 2; ++k) imbalance = std::max(imbalance, std::fabs(cover[k] - cover[cover.size() - 1 - k]));
    row("Copper balance (mirror layers)", fmt("%.0f %%", imbalance * 100), "≤ 35 %", imbalance <= 0.35);
    // The geometric checks, by their findings.
    std::map<std::string, int> found;
    for (const auto& v : dfmChecks(sch, pcb)) ++found[v.code];
    for (const auto& [code, rule] : std::vector<std::pair<std::string, std::string>>{
             {"DFM_MASK_SLIVER", "Solder-mask webs"}, {"DFM_SILK_TO_PAD", "Silkscreen clear of pads"},
             {"DFA_PART_SPACING", "Part spacing"}, {"DFA_PART_TO_EDGE", "Parts clear of the edge"},
             {"DFA_FIDUCIALS", "Fiducials for fine pitch"}, {"DFA_HEAVY_BOTTOM", "No heavy bottom-side parts"}}) {
        const int k = found.count(code) ? found[code] : 0;
        row(rule, k ? std::to_string(k) + " finding(s)" : "OK", "none", k == 0);
    }
    j["rows"] = rows;
    j["pass"] = pass;
    return j;
}

}  // namespace sieda
