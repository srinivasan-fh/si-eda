#include "sieda/Pcb.hpp"
#include "sieda/Reliability.hpp"

#include "sieda/Simulator.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <tuple>
#include <numeric>
#include <queue>
#include <set>

namespace sieda {

// ===================================================================== pads & courtyards

namespace {
Vec2 transformFootprintPoint(Vec2 p, const PcbPlacement& pl) {
    if (pl.bottom) p.x = -p.x;
    return pl.position + rotate90(p, pl.rotation);
}

bool quarterTurned(int rotation) { return ((rotation / 90) % 2 + 2) % 2 == 1; }
}  // namespace

const std::vector<DesignRulePreset>& designRulePresets() {
    static const std::vector<DesignRulePreset> presets = {
        {"Prototype (Conservative)", "Wide tracks and gaps for hand soldering and home etching",
         0.30, 0.30, 0.40, 0.80, 0.50, 0.25, 0.25, 0.30, 0.15, 0.50},
        {"IPC-2221 Class 2", "General electronics — the SiEDA default",
         0.25, 0.20, 0.30, 0.60, 0.50, 0.15, 0.15, 0.20, 0.10, 0.25},
        {"IPC-2221 Class 3", "High-reliability products (medical, aerospace, automotive)",
         0.30, 0.25, 0.35, 0.75, 0.60, 0.20, 0.20, 0.25, 0.15, 0.30},
        {"Fab House Standard (6/6 mil)", "Typical low-cost prototype service limits",
         0.20, 0.16, 0.30, 0.60, 0.30, 0.152, 0.152, 0.20, 0.13, 0.254},
        {"Fab House Advanced (4/4 mil)", "Fine-pitch capable services",
         0.15, 0.12, 0.25, 0.45, 0.30, 0.10, 0.10, 0.15, 0.10, 0.20},
        {"High Voltage (IPC-2221 B2)", "Mains and high-voltage power stages up to 300 V: 1.25 mm spacing, wide tracks",
         0.50, 1.25, 0.40, 0.90, 2.00, 0.25, 0.60, 0.30, 0.15, 0.50},
        {"Automotive (IPC-6012 Class 3/A)", "Vehicle electronics: Class 3 reliability, wider edge keep-out for vibration",
         0.30, 0.25, 0.35, 0.75, 0.80, 0.20, 0.20, 0.25, 0.15, 0.30},
        {"Space (IPC-6012 Class 3/A, ECSS)", "Spacecraft hardware: Class 3/A, 0.2 mm annular ring, generous spacing",
         0.30, 0.30, 0.40, 0.90, 1.00, 0.20, 0.25, 0.30, 0.20, 0.40},
        {"RF (Controlled Impedance)", "RF and high-speed: ≈50 Ω microstrip width on a 4-layer stack (0.2 mm prepreg)",
         0.35, 0.30, 0.30, 0.60, 0.50, 0.15, 0.15, 0.20, 0.10, 0.25},
        {"Medical (IEC 60601-1, IPC Class 3)",
         "Medical devices: Class 3 reliability, wide spacing; patient isolation barriers need 4 mm (MOOP) / 8 mm (2 MOPP) creepage",
         0.30, 0.30, 0.35, 0.75, 0.80, 0.20, 0.25, 0.25, 0.15, 0.30},
        {"Defence (IPC-6012 Class 3/A, MIL)", "Military and avionics hardware: Class 3/A, wide edge keep-out for shock and vibration",
         0.30, 0.25, 0.35, 0.80, 1.00, 0.20, 0.20, 0.25, 0.15, 0.35},
        {"High-Speed Digital (100 Ω diff)",
         "Networking and high-speed I/O: 100 Ω differential pairs, small vias, tight spacing on a 4/6-layer stack",
         0.20, 0.15, 0.25, 0.50, 0.50, 0.125, 0.125, 0.20, 0.10, 0.20},
        {"HDI / Fine-Pitch BGA (IPC-2226)",
         "VLSI, FPGA and ASIC boards: 0.1 mm tracks and gaps, laser microvias, fan-out of fine-pitch BGA/QFN",
         0.10, 0.10, 0.15, 0.35, 0.30, 0.075, 0.075, 0.10, 0.075, 0.15},
    };
    return presets;
}

bool BoardSettings::applyPreset(const std::string& name) {
    for (const auto& p : designRulePresets()) {
        if (p.name != name) continue;
        rulePreset = p.name;
        trackWidth = p.trackWidth;
        clearance = p.clearance;
        viaDrill = p.viaDrill;
        viaDiameter = p.viaDiameter;
        edgeClearance = p.edgeClearance;
        minTrackWidth = p.minTrackWidth;
        minClearance = p.minClearance;
        minDrill = p.minDrill;
        minAnnularRing = p.minAnnularRing;
        minHoleToHole = p.minHoleToHole;
        return true;
    }
    return false;
}

double ipc2221TrackWidth(double amps, double tempRise, double oz, bool innerLayer) {
    if (!(amps > 0)) return 0;
    double k = innerLayer ? 0.024 : 0.048;
    double areaMil2 = std::pow(amps / (k * std::pow(tempRise, 0.44)), 1.0 / 0.725);
    double thicknessMil = 1.378 * oz;
    return areaMil2 / thicknessMil * 0.0254;
}

const char* ipc2221ColumnName(Ipc2221Column c) {
    static const char* names[] = {"B1", "B2", "B3", "B4", "A5", "A6", "A7"};
    return names[static_cast<int>(c)];
}

double ipc2221Spacing(double volts, Ipc2221Column column) {
    // Rows: peak volts 0-15, 16-30, 31-50, 51-100, 101-150, 151-170, 171-250, 251-300, 301-500; then mm per volt.
    static const double limits[] = {15, 30, 50, 100, 150, 170, 250, 300, 500};
    static const double table[7][10] = {
        {0.05, 0.05, 0.1, 0.1, 0.2, 0.2, 0.2, 0.2, 0.25, 0.0025},     // B1
        {0.1, 0.1, 0.6, 0.6, 0.6, 1.25, 1.25, 1.25, 2.5, 0.005},      // B2
        {0.1, 0.1, 0.6, 1.5, 3.2, 3.2, 6.4, 12.5, 12.5, 0.025},       // B3
        {0.05, 0.05, 0.13, 0.13, 0.4, 0.4, 0.4, 0.4, 0.8, 0.00305},   // B4
        {0.13, 0.13, 0.13, 0.13, 0.4, 0.4, 0.4, 0.4, 0.8, 0.00305},   // A5
        {0.13, 0.25, 0.4, 0.5, 0.8, 0.8, 0.8, 0.8, 1.5, 0.00305},     // A6
        {0.13, 0.13, 0.13, 0.13, 0.4, 0.4, 0.4, 0.8, 0.8, 0.00305},   // A7
    };
    const double v = std::fabs(volts);
    const auto& row = table[static_cast<int>(column)];
    for (int i = 0; i < 9; ++i)
        if (v <= limits[i]) return row[i];
    return std::max(row[8], v * row[9]);
}

Ipc2221Column externalSpacingColumn(bool highAltitude, bool coated) {
    if (coated) return Ipc2221Column::A5;
    return highAltitude ? Ipc2221Column::B3 : Ipc2221Column::B2;
}

double ipc2221Clearance(double volts, bool highAltitude, bool coated) {
    return ipc2221Spacing(volts, externalSpacingColumn(highAltitude, coated));
}

const std::vector<std::string>& conformalCoatings() {
    static const std::vector<std::string> coatings = {"none", "acrylic", "silicone", "urethane", "epoxy", "parylene"};
    return coatings;
}

std::string copperLayerName(int layer, int layerCount) {
    if (layer == 0) return "Top";
    if (layerCount > 1 && layer == layerCount - 1) return "Bottom";
    return "Inner " + std::to_string(layer);
}

// ===================================================================== board outline & mounting holes

std::vector<Vec2> BoardSettings::outlinePolygon() const {
    if (hasCustomOutline()) return outline;
    return {{0, 0}, {width, 0}, {width, height}, {0, height}};
}

bool BoardSettings::contains(Vec2 p) const {
    if (!hasCustomOutline()) return p.x >= 0 && p.y >= 0 && p.x <= width && p.y <= height;
    bool in = false;
    for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++) {
        const Vec2 &a = outline[i], &b = outline[j];
        if ((a.y > p.y) != (b.y > p.y) && p.x < (b.x - a.x) * (p.y - a.y) / (b.y - a.y) + a.x) in = !in;
    }
    return in;
}

double BoardSettings::edgeDistance(Vec2 p) const {
    if (!hasCustomOutline()) {
        double d = std::min({p.x, p.y, width - p.x, height - p.y});
        if (d >= 0) return d;
        return -pointRectDistance(p, Rect(0, 0, width, height));
    }
    double d = std::numeric_limits<double>::max();
    for (size_t i = 0, j = outline.size() - 1; i < outline.size(); j = i++)
        d = std::min(d, pointSegmentDistance(p, outline[j], outline[i]));
    return contains(p) ? d : -d;
}

double BoardSettings::segmentEdgeDistance(Vec2 a, Vec2 b) const {
    if (!contains(a) || !contains(b)) return -1;
    auto poly = outlinePolygon();
    double d = std::numeric_limits<double>::max();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        d = std::min(d, segmentSegmentDistance(a, b, poly[j], poly[i]));
    return d;
}

double BoardSettings::holeDistance(Vec2 p) const {
    double d = 1e9;
    for (const auto& h : holes) d = std::min(d, (p - h.position).length() - h.keepout / 2);
    return d;
}

bool BoardSettings::rectInside(const Rect& r, double margin) const {
    if (!contains(r.center())) return false;
    auto poly = outlinePolygon();
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if (segmentRectDistance(poly[j], poly[i], r) < margin) return false;
    for (const auto& h : holes)
        if (pointRectDistance(h.position, r) < h.keepout / 2) return false;
    return true;
}

void BoardSettings::setOutline(std::vector<Vec2> polygon) {
    if (polygon.size() < 3) {
        outline.clear();
        return;
    }
    double x0 = polygon[0].x, y0 = polygon[0].y, x1 = x0, y1 = y0;
    for (const auto& p : polygon) {
        x0 = std::min(x0, p.x);
        y0 = std::min(y0, p.y);
        x1 = std::max(x1, p.x);
        y1 = std::max(y1, p.y);
    }
    for (auto& p : polygon) p = p - Vec2{x0, y0};
    outline = std::move(polygon);
    width = x1 - x0;
    height = y1 - y0;
}

std::vector<Vec2> boardOutlinePreset(const std::string& kind, double w, double h, double param) {
    std::vector<Vec2> poly;
    if (!(w > 0)) return poly;
    if (kind == "rectangle") {
        if (!(h > 0)) return poly;
        return {{0, 0}, {w, 0}, {w, h}, {0, h}};
    }
    if (kind == "rounded") {
        if (!(h > 0)) return poly;
        double r = std::clamp(param, 0.0, std::min(w, h) / 2);
        const Vec2 centres[4] = {{w - r, r}, {w - r, h - r}, {r, h - r}, {r, r}};
        const int segs = 6;
        for (int c = 0; c < 4; ++c)
            for (int k = 0; k <= segs; ++k) {
                double a = (c - 1) * kPi / 2 + (kPi / 2) * k / segs;  // −90°, 0°, 90°, 180° quarter arcs
                poly.push_back(centres[c] + Vec2{r * std::cos(a), r * std::sin(a)});
            }
        return poly;
    }
    if (kind == "circle") {
        const int segs = 48;
        for (int k = 0; k < segs; ++k) {
            double a = 2 * kPi * k / segs;
            poly.push_back({w / 2 + w / 2 * std::cos(a), w / 2 + w / 2 * std::sin(a)});
        }
        return poly;
    }
    if (kind == "quad-x") {
        // Square body (side h) with four diagonal arms (width param) reaching the corners of the w × w span.
        double body = std::clamp(h, 5.0, w), arm = std::clamp(param, 2.0, body / std::sqrt(2.0) - 0.5);
        double s2 = std::sqrt(2.0);
        double R = w / s2 - arm / 2;  // arm length from the centre so the tips touch the span
        Vec2 q[4] = {{body / 2, body / 2 - arm / s2},
                     {R / s2 + arm / (2 * s2), R / s2 - arm / (2 * s2)},
                     {R / s2 - arm / (2 * s2), R / s2 + arm / (2 * s2)},
                     {body / 2 - arm / s2, body / 2}};
        for (int rot = 0; rot < 360; rot += 90)
            for (const auto& v : q) poly.push_back(rotate90(v, rot) + Vec2{w / 2, w / 2});
        return poly;
    }
    return poly;
}

namespace {

/// Distance from a point to pad copper (0 inside); round pads are true circles.
double padDistance(const Pad& p, Vec2 pt) {
    if (p.round) return std::max(0.0, (pt - p.position).length() - std::min(p.size.x, p.size.y) / 2);
    return pointRectDistance(pt, p.bounds());
}
}  // namespace

std::vector<Pad> PcbLayout::pads(const Schematic& sch) const {
    std::vector<Pad> out;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
        if (!fp) continue;
        int number = 1;
        for (const auto& pd : fp->pads) {
            Pad p;
            p.componentId = c.id;
            p.pinIndex = pd.pinIndex;
            p.padNumber = number++;
            p.net = pd.pinIndex >= 0 ? sch.netOf({c.id, pd.pinIndex}) : -1;
            p.position = transformFootprintPoint(pd.offset, c.pcb);
            p.size = quarterTurned(c.pcb.rotation) ? Vec2{pd.size.y, pd.size.x} : pd.size;
            p.throughHole = pd.throughHole;
            p.round = pd.round;
            p.drill = pd.drill;
            p.bottom = c.pcb.bottom;
            p.smdLayer = c.pcb.bottom ? settings.bottomLayer() : kTopLayer;
            out.push_back(p);
        }
    }
    return out;
}

Rect PcbLayout::courtyard(const Component& c) const {
    const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
    if (!fp) return Rect::centered(c.pcb.position, 1, 1);
    double w = fp->courtyardW, h = fp->courtyardH;
    if (quarterTurned(c.pcb.rotation)) std::swap(w, h);
    return Rect::centered(c.pcb.position, w, h);
}

int PcbLayout::addTrack(Track t) {
    t.id = nextId_++;
    tracks.push_back(t);
    return t.id;
}

int PcbLayout::addVia(Via v) {
    v.id = nextId_++;
    vias.push_back(v);
    return v.id;
}

bool PcbLayout::fitBoardToComponents(Schematic& sch, double margin) {
    if (settings.hasCustomOutline()) return false;  // a mechanical outline is fixed
    bool any = false;
    Rect box;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        Rect r = courtyard(c);
        box = any ? Rect(std::min(box.x0, r.x0), std::min(box.y0, r.y0), std::max(box.x1, r.x1), std::max(box.y1, r.y1)) : r;
        any = true;
    }
    if (!any) return false;
    margin = std::max(margin, settings.edgeClearance + 0.5);
    auto roundUp = [](double v) { return std::ceil(v * 2.0) / 2.0; };
    double width = std::max(10.0, roundUp(box.width() + 2 * margin));
    double height = std::max(10.0, roundUp(box.height() + 2 * margin));
    // Centre the parts in the new outline, snapped to the 0.25 mm placement grid.
    Vec2 shift{std::round(((width - box.width()) / 2 - box.x0) * 4) / 4, std::round(((height - box.height()) / 2 - box.y0) * 4) / 4};
    for (auto& c : sch.mutableComponents())
        if (c.hasFootprint() && c.pcb.placed) c.pcb.position = c.pcb.position + shift;
    for (auto& t : tracks) {
        t.a = t.a + shift;
        t.b = t.b + shift;
    }
    for (auto& v : vias) v.position = v.position + shift;
    for (auto& h : settings.holes) h.position = h.position + shift;
    settings.width = width;
    settings.height = height;
    return true;
}

// ===================================================================== auto placement

void PcbLayout::autoPlace(Schematic& sch, bool all) {
    auto& comps = sch.mutableComponents();
    std::vector<size_t> todo;
    for (size_t i = 0; i < comps.size(); ++i) {
        if (!comps[i].hasFootprint()) continue;
        if (all) comps[i].pcb.placed = false;
        if (!comps[i].pcb.placed) todo.push_back(i);
    }
    if (todo.empty()) return;
    if (all) clearRouting();

    // Connectivity weights between components (shared nets, ignoring huge nets like GND for ordering).
    std::map<std::pair<int, int>, double> weight;
    std::map<int, double> degree;
    for (const auto& net : sch.nets()) {
        std::set<int> members;
        for (const auto& p : net.pins)
            if (sch.find(p.component)->hasFootprint()) members.insert(p.component);
        if (members.size() < 2) continue;
        double w = 1.0 / static_cast<double>(members.size() - 1);
        for (int a : members)
            for (int b : members)
                if (a < b) {
                    weight[{a, b}] += w;
                    degree[a] += w;
                    degree[b] += w;
                }
    }
    auto w = [&](int a, int b) {
        auto it = weight.find({std::min(a, b), std::max(a, b)});
        return it == weight.end() ? 0.0 : it->second;
    };

    // Greedy ordering: start with the most connected part, then always the part most connected to the placed set.
    std::vector<size_t> order;
    std::vector<bool> used(comps.size(), false);
    std::set<int> placedIds;
    for (const auto& c : comps)
        if (c.hasFootprint() && c.pcb.placed) placedIds.insert(c.id);
    while (order.size() < todo.size()) {
        size_t best = todo[0];
        double bestScore = -1;
        for (size_t idx : todo) {
            if (used[idx]) continue;
            double s = 0;
            for (int pid : placedIds) s += w(comps[idx].id, pid) * 10.0;
            s += degree[comps[idx].id] * 0.01;
            if (s > bestScore) { bestScore = s; best = idx; }
        }
        used[best] = true;
        order.push_back(best);
        placedIds.insert(comps[best].id);
    }

    const bool custom = settings.hasCustomOutline();
    const double margin = 1.2;   // spacing between courtyards, leaves routing channels
    // Fine-pitch packages (pad pitch below 0.65 mm) get an escape area: every pin needs room to neck out and turn.
    auto escape = [&](const Component& comp) {
        const FootprintDef* fp = Library::instance().footprint(comp.def().footprint);
        if (!fp) return 0.0;
        double pitch = std::numeric_limits<double>::max();
        for (size_t a = 0; a < fp->pads.size(); ++a)
            for (size_t b = a + 1; b < fp->pads.size(); ++b)
                pitch = std::min(pitch, (fp->pads[a].offset - fp->pads[b].offset).length());
        return pitch < 0.65 ? 1.5 : 0.0;
    };
    const double step = 0.5;
    std::map<int, double> escapeOf;
    for (const auto& comp : comps)
        if (comp.hasFootprint()) escapeOf[comp.id] = escape(comp);
    for (size_t idx : order) {
        Component& c = comps[idx];
        // Net centroids of already-placed pads.
        std::map<int, std::pair<Vec2, int>> centroid;
        for (const auto& p : pads(sch)) {
            if (p.net < 0) continue;
            auto& e = centroid[p.net];
            e.first = e.first + p.position;
            e.second += 1;
        }
        bool done = false;
        for (int growth = 0; growth < 20 && !done; ++growth) {
            double bestCost = std::numeric_limits<double>::max();
            Vec2 bestPos;
            int bestRot = 0;
            for (int rot : {0, 90}) {
                c.pcb.rotation = rot;
                c.pcb.bottom = false;
                Rect cy0 = courtyard(c);
                double hw = cy0.width() / 2, hh = cy0.height() / 2;
                double e = settings.edgeClearance + 0.5;
                // Candidates on the placement grid itself, so the final snap does not move a checked position.
                for (double y = std::ceil((e + hh) / step - 1e-9) * step; y <= settings.height - e - hh + 1e-9; y += step) {
                    for (double x = std::ceil((e + hw) / step - 1e-9) * step; x <= settings.width - e - hw + 1e-9; x += step) {
                        Rect cy = Rect::centered({x, y}, cy0.width(), cy0.height()).inflated(margin / 2 + escapeOf[c.id]);
                        if (custom && !settings.rectInside(Rect::centered({x, y}, cy0.width(), cy0.height()), e)) continue;
                        if (!custom && !settings.holes.empty() &&
                            !settings.rectInside(Rect::centered({x, y}, cy0.width(), cy0.height()), 0))
                            continue;
                        bool clash = false;
                        for (const auto& o : comps) {
                            if (o.id == c.id || !o.hasFootprint() || !o.pcb.placed) continue;
                            if (courtyard(o).inflated(margin / 2 + escapeOf[o.id]).intersects(cy)) { clash = true; break; }
                        }
                        if (clash) continue;
                        // Cost: pad distances to the centroid of their nets, else pull toward board centre.
                        c.pcb.position = {x, y};
                        double cost = 0;
                        int terms = 0;
                        const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
                        for (const auto& pd : fp->pads) {
                            if (pd.pinIndex < 0) continue;
                            int net = sch.netOf({c.id, pd.pinIndex});
                            auto it = centroid.find(net);
                            if (it == centroid.end() || it->second.second == 0) continue;
                            Vec2 cen = it->second.first * (1.0 / it->second.second);
                            Vec2 pp = transformFootprintPoint(pd.offset, c.pcb);
                            cost += std::fabs(pp.x - cen.x) + std::fabs(pp.y - cen.y);
                            ++terms;
                        }
                        Vec2 centre{settings.width / 2, settings.height / 2};
                        cost += (terms ? 0.05 : 1.0) * ((Vec2{x, y} - centre).length());
                        if (cost < bestCost - 1e-9) {
                            bestCost = cost;
                            bestPos = {x, y};
                            bestRot = rot;
                        }
                    }
                }
            }
            if (bestCost < std::numeric_limits<double>::max()) {
                c.pcb.position = {std::round(bestPos.x / step) * step, std::round(bestPos.y / step) * step};
                c.pcb.rotation = bestRot;
                c.pcb.placed = true;
                done = true;
            } else if (custom) {
                break;  // a fixed outline cannot grow: leave the part for the user (DRC reports it)
            } else {
                settings.width += 10;
                settings.height += 6;
            }
        }
        if (!done) {  // no room — drop it beside the board so the user can move it (DRC_OUT_OF_BOARD)
            c.pcb.position = custom ? Vec2{settings.width + courtyard(c).width(), settings.height / 2}
                                    : Vec2{settings.width / 2, settings.height / 2};
            c.pcb.placed = true;
        }
    }
}

// ===================================================================== copper connectivity

namespace {
struct CopperItem {
    enum Kind { PadItem, TrackItem, ViaItem } kind;
    size_t index;
};

struct DSU {
    std::vector<size_t> p;
    explicit DSU(size_t n) : p(n) { std::iota(p.begin(), p.end(), size_t{0}); }
    size_t find(size_t x) {
        while (p[x] != x) x = p[x] = p[p[x]];
        return x;
    }
    void unite(size_t a, size_t b) { p[find(a)] = find(b); }
};

bool trackTouchesPad(const Track& t, const Pad& p) {
    if (!p.onLayer(t.layer)) return false;
    if (p.round) return pointSegmentDistance(p.position, t.a, t.b) <= std::min(p.size.x, p.size.y) / 2 + t.width / 2 - 1e-6;
    return segmentRectDistance(t.a, t.b, p.bounds()) <= t.width / 2 - 1e-6;
}

bool tracksTouch(const Track& a, const Track& b) {
    if (a.layer != b.layer) return false;
    return segmentSegmentDistance(a.a, a.b, b.a, b.b) <= std::min(a.width, b.width) / 2;
}

bool viaTouchesTrack(const Via& v, const Track& t) { return pointSegmentDistance(v.position, t.a, t.b) <= v.diameter / 2; }

bool viaTouchesPad(const Via& v, const Pad& p) { return padDistance(p, v.position) <= v.diameter / 2 - 1e-6; }

/// Union-find over pads + tracks + vias (+ poured islands) by physical contact. Item order: pads, tracks, vias, then
/// the islands of each fill in turn.
DSU copperClusters(const std::vector<Pad>& pads, const std::vector<Track>& tracks, const std::vector<Via>& vias,
                   const std::vector<ZoneFill>* fills = nullptr) {
    size_t np = pads.size(), nt = tracks.size(), nv = vias.size();
    size_t ni = 0;
    if (fills)
        for (const auto& f : *fills) ni += static_cast<size_t>(f.islands);
    DSU d(np + nt + nv + ni);
    if (fills) {
        size_t base = np + nt + nv;
        for (const auto& f : *fills) {
            if (f.islands == 0) continue;
            auto join = [&](size_t item, int island) {
                if (island >= 0) d.unite(item, base + static_cast<size_t>(island));
            };
            for (size_t p = 0; p < np; ++p) {
                const Pad& pd = pads[p];
                if (pd.net != f.net || !pd.onLayer(f.layer)) continue;
                // Sample the pad area; any island cell inside the pad copper connects.
                const double touch = f.cell * 0.45;
                Rect b = pd.bounds().inflated(touch);
                int i0 = std::max(0, static_cast<int>(std::floor(b.x0 / f.cell))), i1 = std::min(f.cols - 1, static_cast<int>(b.x1 / f.cell));
                int j0 = std::max(0, static_cast<int>(std::floor(b.y0 / f.cell))), j1 = std::min(f.rows - 1, static_cast<int>(b.y1 / f.cell));
                int last = -1;
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        int id = f.island[static_cast<size_t>(j * f.cols + i)];
                        if (id < 0 || id == last || padDistance(pd, {(i + 0.5) * f.cell, (j + 0.5) * f.cell}) > touch) continue;
                        join(p, id);
                        last = id;
                    }
            }
            for (size_t t = 0; t < nt; ++t) {
                const Track& tr = tracks[t];
                if (tr.net != f.net || tr.layer != f.layer) continue;
                double len = (tr.b - tr.a).length();
                int steps = std::max(1, static_cast<int>(std::ceil(len / f.cell)));
                for (int k = 0; k <= steps; ++k) {
                    int id = f.islandNear(tr.a + (tr.b - tr.a) * (static_cast<double>(k) / steps), tr.width / 2);
                    if (id >= 0) join(np + t, id);
                }
            }
            for (size_t v = 0; v < nv; ++v)
                if (vias[v].net == f.net) join(np + nt + v, f.islandNear(vias[v].position, vias[v].diameter / 2));
            base += static_cast<size_t>(f.islands);
        }
    }
    for (size_t t = 0; t < nt; ++t) {
        for (size_t p = 0; p < np; ++p)
            if (trackTouchesPad(tracks[t], pads[p])) d.unite(np + t, p);
        for (size_t u = t + 1; u < nt; ++u)
            if (tracksTouch(tracks[t], tracks[u])) d.unite(np + t, np + u);
        for (size_t v = 0; v < nv; ++v)
            if (viaTouchesTrack(vias[v], tracks[t])) d.unite(np + t, np + nt + v);
    }
    for (size_t v = 0; v < nv; ++v)
        for (size_t p = 0; p < np; ++p)
            if (viaTouchesPad(vias[v], pads[p])) d.unite(np + nt + v, p);
    return d;
}
}  // namespace

void PcbLayout::pruneStaleRouting(const Schematic& sch) {
    if (tracks.empty() && vias.empty()) return;
    auto ps = pads(sch);
    DSU d = copperClusters(ps, tracks, vias);
    size_t np = ps.size(), nt = tracks.size();
    std::map<size_t, std::map<int, int>> clusterNets;
    for (size_t p = 0; p < np; ++p)
        if (ps[p].net >= 0) clusterNets[d.find(p)][ps[p].net]++;
    auto netFor = [&](size_t item) {
        auto it = clusterNets.find(d.find(item));
        if (it == clusterNets.end()) return -1;
        int best = -1, cnt = 0;
        for (auto [n, k] : it->second)
            if (k > cnt) { best = n; cnt = k; }
        return best;
    };
    std::vector<Track> keptT;
    for (size_t t = 0; t < nt; ++t) {
        int n = netFor(np + t);
        if (n < 0) continue;
        Track tr = tracks[t];
        tr.net = n;
        keptT.push_back(tr);
    }
    std::vector<Via> keptV;
    for (size_t v = 0; v < vias.size(); ++v) {
        int n = netFor(np + nt + v);
        if (n < 0) continue;
        Via via = vias[v];
        via.net = n;
        keptV.push_back(via);
    }
    tracks = std::move(keptT);
    vias = std::move(keptV);
}

std::vector<std::pair<Vec2, Vec2>> PcbLayout::ratsnest(const Schematic& sch) const {
    std::vector<std::pair<Vec2, Vec2>> lines;
    auto ps = pads(sch);
    DSU d = copperClusters(ps, tracks, vias, &zoneFills(sch));
    std::map<int, std::vector<size_t>> byNet;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].net >= 0) byNet[ps[i].net].push_back(i);
    for (auto& [net, list] : byNet) {
        if (list.size() < 2) continue;
        // Prim over clusters.
        std::set<size_t> connectedClusters{d.find(list[0])};
        std::set<size_t> allClusters;
        for (size_t i : list) allClusters.insert(d.find(i));
        while (connectedClusters.size() < allClusters.size()) {
            double best = std::numeric_limits<double>::max();
            size_t ba = 0, bb = 0;
            for (size_t i : list) {
                if (!connectedClusters.count(d.find(i))) continue;
                for (size_t j : list) {
                    if (connectedClusters.count(d.find(j))) continue;
                    double dist = (ps[i].position - ps[j].position).length();
                    if (dist < best) { best = dist; ba = i; bb = j; }
                }
            }
            lines.push_back({ps[ba].position, ps[bb].position});
            connectedClusters.insert(d.find(bb));
        }
    }
    return lines;
}

// ===================================================================== autorouter

namespace {
class RoutingGrid {
public:
    RoutingGrid(const BoardSettings& s) : s_(s) {
        g_ = s.routingGrid;
        layers_ = std::max(1, s.layerCount);
        owner_.resize(static_cast<size_t>(layers_));
        padNet_.resize(static_cast<size_t>(layers_));
        copper_.resize(static_cast<size_t>(layers_));
        cols_ = static_cast<int>(std::floor(s.width / g_)) + 1;
        rows_ = static_cast<int>(std::floor(s.height / g_)) + 1;
        size_t n = static_cast<size_t>(cols_ * rows_);
        for (int l = 0; l < layers_; ++l) {
            owner_[L(l)].assign(n, -1);
            padNet_[L(l)].assign(n, -1);
            copper_[L(l)].assign(n, -1);
        }
        noVia_.assign(n, 0);
        band_.assign(n, 0);
        bCols_ = std::max(1, static_cast<int>(std::ceil(s.width / kBucket)));
        bRows_ = std::max(1, static_cast<int>(std::ceil(s.height / kBucket)));
        buckets_.assign(static_cast<size_t>(bCols_ * bRows_), {});
        padBuckets_.assign(static_cast<size_t>(bCols_ * bRows_), {});
        planeNet_.assign(static_cast<size_t>(layers_), -1);
        pourNet_.assign(static_cast<size_t>(layers_), -1);
        double e = s.edgeClearance + s.trackWidth / 2;
        for (int j = 0; j < rows_; ++j)
            for (int i = 0; i < cols_; ++i) {
                Vec2 p = pos(i, j);
                if (s.edgeDistance(p) < e || s.holeDistance(p) < s.clearance + s.trackWidth / 2)
                    for (int l = 0; l < layers_; ++l) owner_[L(l)][idx(i, j)] = -2;
            }
    }

    int layers() const { return layers_; }
    /// Plane layers are reserved for their net; pour layers cost other nets more so the pour stays whole.
    void setPlane(int l, int net) { if (l >= 0 && l < layers_) planeNet_[L(l)] = net; }
    void setPour(int l, int net) { if (l >= 0 && l < layers_ && pourNet_[L(l)] < 0) pourNet_[L(l)] = net; }
    bool layerOpen(int l, int net) const { return planeNet_[L(l)] < 0 || planeNet_[L(l)] == net; }
    float layerCost(int l, int net) const { return pourNet_[L(l)] >= 0 && pourNet_[L(l)] != net ? 1.6f : 1.0f; }
    /// Escape band around a fine-pitch package: tracks may cross it but not run along it.
    void addEscapeBand(const Rect& body, double width) {
        Rect outer = body.inflated(width);
        for (int j = 0; j < rows_; ++j)
            for (int i = 0; i < cols_; ++i) {
                Vec2 p = pos(i, j);
                if (outer.contains(p) && !body.contains(p)) band_[idx(i, j)] = 1;
            }
    }
    float bandCost(size_t c) const { return band_[c] ? 4.0f : 1.0f; }
    int cols() const { return cols_; }
    int rows() const { return rows_; }
    double pitch() const { return g_; }
    size_t idx(int i, int j) const { return static_cast<size_t>(j * cols_ + i); }
    Vec2 pos(int i, int j) const { return {i * g_, j * g_}; }
    bool inside(int i, int j) const { return i >= 0 && j >= 0 && i < cols_ && j < rows_; }

    bool passable(int l, size_t c, int net) const {
        int o = owner_[L(l)][c];
        return o == -1 || o == net || padNet_[L(l)][c] == net;
    }
    /// Exact clearance test for a wide (net-class) track segment a–b of half-width `half` on layer l: the grid only
    /// guarantees clearance for the base track width, so wide tracks are checked against the real copper.
    bool wideClear(int l, Vec2 a, Vec2 b, int net, double half) const {
        const double need = s_.clearance + half - 1e-9;
        for (Vec2 p : {a, b})
            if (s_.edgeDistance(p) < s_.edgeClearance + half - 1e-9 || s_.holeDistance(p) < half) return false;
        Rect box = Rect(a.x, a.y, b.x, b.y).inflated(need + maxHalf_);
        bool clash = false;
        forBuckets(box, padStamp_, padSeen_, padBuckets_, [&](size_t id) {
            const Pad& p = pads_[id];
            if (clash || (p.net == net && net >= 0) || !p.onLayer(l)) return;
            double d = p.round ? std::max(0.0, pointSegmentDistance(p.position, a, b) - std::min(p.size.x, p.size.y) / 2)
                               : segmentRectDistance(a, b, p.bounds());
            if (d < need) clash = true;
        });
        if (clash) return false;
        forBuckets(box, stamp_, seen_, buckets_, [&](size_t id) {
            const Copper& c = copperItems_[id];
            if (clash || c.net == net || (c.layer >= 0 && c.layer != l)) return;
            if (segmentSegmentDistance(a, b, c.a, c.b) - c.half < need) clash = true;
        });
        return !clash;
    }

    bool viaAllowed(size_t c, int net) const {
        int ci = static_cast<int>(c % static_cast<size_t>(cols_)), cj = static_cast<int>(c / static_cast<size_t>(cols_));
        // Other-net copper must stay outside (via radius + clearance + half a track, since copper_ marks centrelines).
        double rCopper = s_.viaDiameter / 2 + s_.clearance + s_.trackWidth / 2;
        // The via barrel is wider than a track: the extra ring must lie in cells this net may route through.
        double rBody = std::max(0.0, s_.viaDiameter / 2 - s_.trackWidth / 2) + 1e-9;
        int k = static_cast<int>(std::ceil(rCopper / g_));
        if (layers_ < 2) return false;  // single-sided boards have no vias
        if (noVia_[c]) return false;    // never inside an SMD pad (DRC_VIA_IN_PAD)
        for (int l = 0; l < layers_; ++l) {  // through via: every layer must allow it
            if (!passable(l, c, net)) return false;
            for (int dj = -k; dj <= k; ++dj)
                for (int di = -k; di <= k; ++di) {
                    int i = ci + di, j = cj + dj;
                    double d = std::sqrt(double(di * di + dj * dj)) * g_;
                    if (d > rCopper) continue;
                    if (!inside(i, j)) return false;
                    size_t cc = idx(i, j);
                    int cu = copper_[L(l)][cc];
                    if (cu != -1 && cu != net) return false;
                    if (d <= rBody && !passable(l, cc, net)) return false;
                }
        }
        // Pad copper is sampled on the grid, so a pad edge can sit up to half a cell diagonal closer than the cells
        // suggest: check other-net pads exactly, and hole-to-hole spacing to drilled pads.
        Vec2 at = pos(ci, cj);
        double need = s_.viaDiameter / 2 + s_.clearance - 1e-9;
        for (const Pad& p : pads_) {
            double holeNeed = p.throughHole && p.drill > 0 ? (p.drill + s_.viaDrill) / 2 + s_.minHoleToHole : 0.0;
            double reach = std::max(need, holeNeed - std::min(p.size.x, p.size.y) / 2);
            Rect b = p.bounds();
            if (at.x < b.x0 - reach || at.x > b.x1 + reach || at.y < b.y0 - reach || at.y > b.y1 + reach) continue;
            if (holeNeed > 0 && (at - p.position).length() < holeNeed) return false;
            if (p.net == net && net >= 0) continue;
            if (padDistance(p, at) < need) return false;
        }
        // Tracks and vias are also tracked exactly (the grid only records their centrelines).
        bool clash = false;
        forNearbyCopper(at, need + maxHalf_ + s_.viaDrill + s_.minHoleToHole, [&](const Copper& c) {
            if (c.drill > 0 && (at - c.a).length() < (c.drill + s_.viaDrill) / 2 + s_.minHoleToHole - 1e-9) clash = true;
            if (c.net != net && pointSegmentDistance(at, c.a, c.b) < need + c.half) clash = true;
        });
        return !clash;
    }

    /// Records routed copper for the exact via checks (`drill` > 0 for a via).
    void addCopper(Vec2 a, Vec2 b, double half, int net, int layer, double drill = 0) {
        size_t id = copperItems_.size();
        copperItems_.push_back({a, b, half, net, drill, layer});
        maxHalf_ = std::max(maxHalf_, half);
        Rect box = Rect(a.x, a.y, b.x, b.y).inflated(half);
        for (int by = bucketOf(box.y0, bRows_); by <= bucketOf(box.y1, bRows_); ++by)
            for (int bx = bucketOf(box.x0, bCols_); bx <= bucketOf(box.x1, bCols_); ++bx)
                buckets_[static_cast<size_t>(by * bCols_ + bx)].push_back(id);
    }

    void markDisc(int l, Vec2 centre, double radius, int net) {
        forCellsNear(centre, centre, radius, [&](size_t c) { claim(l, c, net); });
    }
    void markSegment(int l, Vec2 a, Vec2 b, double radius, int net) {
        forCellsNear(a, b, radius, [&](size_t c) { claim(l, c, net); });
    }
    void markCopperSegment(int l, Vec2 a, Vec2 b, double radius, int net) {
        forCellsNear(a, b, radius, [&](size_t c) { copper_[L(l)][c] = net; });
    }
    void markPad(const Pad& p, double keepout) {
        Rect r = p.bounds();
        pads_.push_back(p);
        for (int by = bucketOf(r.y0, bRows_); by <= bucketOf(r.y1, bRows_); ++by)
            for (int bx = bucketOf(r.x0, bCols_); bx <= bucketOf(r.x1, bCols_); ++bx)
                padBuckets_[static_cast<size_t>(by * bCols_ + bx)].push_back(pads_.size() - 1);
        for (int l = 0; l < layers_; ++l) {
            if (!p.onLayer(l)) continue;
            bool anyCore = false;
            forRectNear(r, keepout, p, [&](size_t c, double dist) {
                if (p.net >= 0) claim(l, c, p.net);
                else claim(l, c, -3);  // unconnected pad: blocks every net
                if (dist <= 0) {
                    if (!p.throughHole) noVia_[c] = 1;
                    copper_[L(l)][c] = p.net >= 0 ? p.net : -3;
                    if (p.net >= 0 && inCore(p, cellPos(c))) { padNet_[L(l)][c] = p.net; anyCore = true; }
                }
            });
            if (!anyCore && p.net >= 0) {
                int i = static_cast<int>(std::lround(p.position.x / g_)), j = static_cast<int>(std::lround(p.position.y / g_));
                if (inside(i, j)) {
                    padNet_[L(l)][idx(i, j)] = p.net;
                    if (!p.throughHole) noVia_[idx(i, j)] = 1;
                }
            }
        }
    }
    /// Track entry points of a pad: inside it by half a base track width, so a track leaving from there never
    /// overhangs the pad edge toward a neighbour (pads narrower than a track collapse to their centre line).
    bool inCore(const Pad& p, Vec2 at) const {
        const double inset = s_.trackWidth / 2;
        if (p.round) return (at - p.position).length() <= std::max(0.0, std::min(p.size.x, p.size.y) / 2 - inset) + 1e-9;
        double ix = std::min(inset, p.size.x / 2), iy = std::min(inset, p.size.y / 2);
        return std::fabs(at.x - p.position.x) <= p.size.x / 2 - ix + 1e-9 && std::fabs(at.y - p.position.y) <= p.size.y / 2 - iy + 1e-9;
    }
    Vec2 cellPos(size_t c) const {
        return pos(static_cast<int>(c % static_cast<size_t>(cols_)), static_cast<int>(c / static_cast<size_t>(cols_)));
    }
    std::vector<size_t> padCoreCells(const Pad& p) const {
        std::vector<size_t> out;
        Rect r = p.bounds();
        int i0 = std::max(0, static_cast<int>(std::ceil(r.x0 / g_))), i1 = std::min(cols_ - 1, static_cast<int>(std::floor(r.x1 / g_)));
        int j0 = std::max(0, static_cast<int>(std::ceil(r.y0 / g_))), j1 = std::min(rows_ - 1, static_cast<int>(std::floor(r.y1 / g_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (inCore(p, pos(i, j))) out.push_back(idx(i, j));
        if (out.empty()) {
            int i = static_cast<int>(std::lround(p.position.x / g_)), j = static_cast<int>(std::lround(p.position.y / g_));
            if (inside(i, j)) out.push_back(idx(i, j));
        }
        return out;
    }

private:
    struct Copper {
        Vec2 a, b;
        double half;
        int net;
        double drill;
        int layer;  // -1 = every layer (via)
    };
    static constexpr double kBucket = 2.0;
    int bucketOf(double v, int count) const {
        return std::clamp(static_cast<int>(std::floor(v / kBucket)), 0, count - 1);
    }
    template <typename F>
    void forBuckets(const Rect& box, unsigned& stamp, std::vector<unsigned>& seen,
                    const std::vector<std::vector<size_t>>& buckets, F f) const {
        int x0 = bucketOf(box.x0, bCols_), x1 = bucketOf(box.x1, bCols_);
        int y0 = bucketOf(box.y0, bRows_), y1 = bucketOf(box.y1, bRows_);
        ++stamp;
        for (int by = y0; by <= y1; ++by)
            for (int bx = x0; bx <= x1; ++bx)
                for (size_t id : buckets[static_cast<size_t>(by * bCols_ + bx)]) {
                    if (seen.size() <= id) seen.resize(id + 1, 0);
                    if (seen[id] == stamp) continue;
                    seen[id] = stamp;
                    f(id);
                }
    }
    template <typename F>
    void forNearbyCopper(Vec2 at, double radius, F f) const {
        forBuckets(Rect::centered(at, 2 * radius, 2 * radius), stamp_, seen_, buckets_,
                   [&](size_t id) { f(copperItems_[id]); });
    }
    static size_t L(int l) { return static_cast<size_t>(l); }
    void claim(int l, size_t c, int net) {
        int& o = owner_[L(l)][c];
        if (o == -1) o = net;
        else if (o != net) o = -2;
    }
    template <typename F>
    void forCellsNear(Vec2 a, Vec2 b, double radius, F f) {
        double x0 = std::min(a.x, b.x) - radius, x1 = std::max(a.x, b.x) + radius;
        double y0 = std::min(a.y, b.y) - radius, y1 = std::max(a.y, b.y) + radius;
        int i0 = std::max(0, static_cast<int>(std::floor(x0 / g_))), i1 = std::min(cols_ - 1, static_cast<int>(std::ceil(x1 / g_)));
        int j0 = std::max(0, static_cast<int>(std::floor(y0 / g_))), j1 = std::min(rows_ - 1, static_cast<int>(std::ceil(y1 / g_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (pointSegmentDistance(pos(i, j), a, b) < radius - 1e-9) f(idx(i, j));
    }
    template <typename F>
    void forRectNear(const Rect& r, double radius, const Pad& pad, F f) {
        int i0 = std::max(0, static_cast<int>(std::floor((r.x0 - radius) / g_)));
        int i1 = std::min(cols_ - 1, static_cast<int>(std::ceil((r.x1 + radius) / g_)));
        int j0 = std::max(0, static_cast<int>(std::floor((r.y0 - radius) / g_)));
        int j1 = std::min(rows_ - 1, static_cast<int>(std::ceil((r.y1 + radius) / g_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                double d = padDistance(pad, pos(i, j));
                if (d < radius - 1e-9) f(idx(i, j), d);
            }
    }

    const BoardSettings& s_;
    double g_ = 0.25;
    int cols_ = 0, rows_ = 0, layers_ = 2;
    std::vector<std::vector<int>> owner_;   // per layer routing keep-out: -1 free, net, -2 shared/blocked, -3 NC pad
    std::vector<std::vector<int>> padNet_;  // per layer pad copper reachable by its own net
    std::vector<std::vector<int>> copper_;  // per layer actual copper occupancy
    std::vector<char> noVia_;               // cells inside SMD pads (any layer): no via may be placed there
    std::vector<char> band_;                // escape bands around fine-pitch packages (costly to run along)
    std::vector<Pad> pads_;                 // every marked pad, for exact via clearance
    std::vector<int> planeNet_, pourNet_;   // per layer: reserved plane net / poured net, -1 none
    std::vector<Copper> copperItems_;       // routed tracks and vias, for exact via clearance
    std::vector<std::vector<size_t>> buckets_;
    int bCols_ = 1, bRows_ = 1;
    double maxHalf_ = 0;
    mutable std::vector<unsigned> seen_, padSeen_;
    mutable unsigned stamp_ = 0, padStamp_ = 0;
    std::vector<std::vector<size_t>> padBuckets_;
};

struct PathNode {
    int layer;
    int i, j;
};

struct RouteResult {
    bool ok = false;
    std::vector<PathNode> path;
};

RouteResult astar(const RoutingGrid& g, int net, const std::vector<std::pair<int, size_t>>& sources,
                  const std::vector<std::vector<char>>& targetMask, Vec2 targetCentre, double viaCost,
                  double wideHalf = 0, const std::vector<float>* neckHalf = nullptr) {
    const int cols = g.cols(), rows = g.rows();
    const size_t n = static_cast<size_t>(cols * rows);
    const int layers = g.layers();
    const size_t total = static_cast<size_t>(layers) * n;
    std::vector<float> cost(total, std::numeric_limits<float>::infinity());
    std::vector<int> parent(total, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    auto h = [&](int i, int j) {
        double dx = std::fabs(i - targetCentre.x / g.pitch()), dy = std::fabs(j - targetCentre.y / g.pitch());
        return static_cast<float>(std::max(dx, dy) + 0.414 * std::min(dx, dy));
    };
    for (auto [l, c] : sources) {
        int s = static_cast<int>(static_cast<size_t>(l) * n + c);
        if (cost[static_cast<size_t>(s)] == 0) continue;
        cost[static_cast<size_t>(s)] = 0;
        int i = static_cast<int>(c % static_cast<size_t>(cols)), j = static_cast<int>(c / static_cast<size_t>(cols));
        open.push({h(i, j), s});
    }
    static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int found = -1;
    size_t expanded = 0;
    while (!open.empty()) {
        auto [f, s] = open.top();
        open.pop();
        int l = s / static_cast<int>(n);
        size_t c = static_cast<size_t>(s) % n;
        int i = static_cast<int>(c % static_cast<size_t>(cols)), j = static_cast<int>(c / static_cast<size_t>(cols));
        float gc = cost[static_cast<size_t>(s)];
        if (f - h(i, j) > gc + 1e-3f) continue;  // stale entry
        if (targetMask[static_cast<size_t>(l)][c]) { found = s; break; }
        if (++expanded > 4 * total) break;
        const bool lateral = g.layerOpen(l, net);  // on another net's plane only a via may pass
        // Incoming direction (approximate turn penalty keeps tracks straight and avoids zig-zags).
        int inDi = 0, inDj = 0;
        int ps = parent[static_cast<size_t>(s)];
        if (ps >= 0 && ps / static_cast<int>(n) == l) {
            size_t pc = static_cast<size_t>(ps) % n;
            inDi = i - static_cast<int>(pc % static_cast<size_t>(cols));
            inDj = j - static_cast<int>(pc / static_cast<size_t>(cols));
        }
        for (int k = 0; k < 8 && lateral; ++k) {
            int ni = i + di[k], nj = j + dj[k];
            if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
            size_t nc = g.idx(ni, nj);
            // Next to its own fine-pitch pads the track is necked down (neckDown), so there the necked copper is
            // checked exactly against its neighbours instead of the base-width grid keep-outs.
            const float nh = neckHalf ? (*neckHalf)[nc] : 0.0f;
            if (nh > 0) {
                if (!g.inside(ni, nj) || !g.wideClear(l, g.pos(i, j), g.pos(ni, nj), net, nh)) continue;
            } else {
                if (!g.passable(l, nc, net)) continue;
                // Wide (net-class) tracks need their whole body clear.
                if (wideHalf > 0 && !g.wideClear(l, g.pos(i, j), g.pos(ni, nj), net, wideHalf)) continue;
            }
            float step = g.layerCost(l, net) * g.bandCost(nc);
            if (k >= 4) {
                // Diagonal: both orthogonal neighbours must be free so the centreline keeps clearance.
                if (nh <= 0 && (!g.passable(l, g.idx(ni, j), net) || !g.passable(l, g.idx(i, nj), net))) continue;
                step *= 1.4142f;
            }
            // Layer direction preference: even layers (top, inner 2…) horizontal, odd layers vertical.
            bool horizontal = dj[k] == 0, vertical = di[k] == 0;
            if (layers > 1 && ((l % 2 == 0 && vertical) || (l % 2 == 1 && horizontal))) step *= 1.25f;
            if ((inDi != 0 || inDj != 0) && (inDi != di[k] || inDj != dj[k])) step += 0.6f;
            int ns = static_cast<int>(static_cast<size_t>(l) * n + nc);
            float nc2 = gc + step;
            if (nc2 < cost[static_cast<size_t>(ns)]) {
                cost[static_cast<size_t>(ns)] = nc2;
                parent[static_cast<size_t>(ns)] = s;
                open.push({nc2 + h(ni, nj), ns});
            }
        }
        // Through via to any other layer (checked lazily: viaAllowed is the expensive test).
        int viaState = -1;  // -1 unknown, 0 no, 1 yes
        for (int ol = 0; ol < layers; ++ol) {
            if (ol == l || !g.layerOpen(ol, net)) continue;
            int os = static_cast<int>(static_cast<size_t>(ol) * n + c);
            float nc2 = gc + static_cast<float>(viaCost) + 0.5f * static_cast<float>(std::abs(ol - l) - 1);
            if (nc2 >= cost[static_cast<size_t>(os)] || !g.passable(ol, c, net)) continue;
            // A wide track continues from the via on the other layer: its body must fit there too.
            if (wideHalf > 0 && !(neckHalf && (*neckHalf)[c] > 0) && !g.wideClear(ol, g.pos(i, j), g.pos(i, j), net, wideHalf))
                continue;
            if (viaState < 0) viaState = g.viaAllowed(c, net) ? 1 : 0;
            if (viaState == 0) break;
            cost[static_cast<size_t>(os)] = nc2;
            parent[static_cast<size_t>(os)] = s;
            open.push({nc2 + h(i, j), os});
        }
    }
    RouteResult r;
    if (found < 0) return r;
    for (int s = found; s >= 0; s = parent[static_cast<size_t>(s)]) {
        int l = s / static_cast<int>(n);
        size_t c = static_cast<size_t>(s) % n;
        r.path.push_back({l, static_cast<int>(c % static_cast<size_t>(cols)), static_cast<int>(c / static_cast<size_t>(cols))});
    }
    std::reverse(r.path.begin(), r.path.end());
    r.ok = true;
    return r;
}

struct NetRouteOutcome {
    std::vector<Track> tracks;
    std::vector<Via> vias;
    int connections = 0, routed = 0;
};
}  // namespace

std::map<int, std::pair<double, double>> netVoltageRanges(const Schematic& sch) {
    std::map<int, std::pair<double, double>> netRange;
    bool hasSource = false;
    for (const auto& c : sch.components())
        hasSource |= isSourceKind(c.kind);
    if (!hasSource || sch.groundNet() < 0) return netRange;
    DcResult dc = Simulator(sch).dcOperatingPoint();
    if (!dc.converged) return netRange;
    for (size_t n = 0; n < dc.netVoltages.size(); ++n) netRange[static_cast<int>(n)] = {dc.netVoltages[n], dc.netVoltages[n]};
    for (const auto& c : sch.components()) {
        if (!isVoltageSourceKind(c.kind)) continue;
        auto spec = SourceSpec::parse(c.value);
        int plus = sch.netOf({c.id, 0}), minus = sch.netOf({c.id, 1});
        if (!spec || plus < 0) continue;
        double base = minus >= 0 && netRange.count(minus) ? netRange[minus].first : 0.0;
        double lo = spec->dc, hi = spec->dc;
        if (spec->kind == SourceSpec::Kind::Sine) {
            lo = spec->offset - std::fabs(spec->amplitude);
            hi = spec->offset + std::fabs(spec->amplitude);
        } else if (spec->kind == SourceSpec::Kind::Pulse) {
            lo = std::min(spec->v1, spec->v2);
            hi = std::max(spec->v1, spec->v2);
        }
        auto& r = netRange[plus];
        r.first = std::min(r.first, base + lo);
        r.second = std::max(r.second, base + hi);
    }
    return netRange;
}

double voltageRoutingClearance(const Schematic& sch, bool highAltitude, bool coated) {
    auto ranges = netVoltageRanges(sch);
    if (ranges.empty()) return 0;
    double lo = 0, hi = 0;
    for (const auto& [net, r] : ranges) {
        lo = std::min(lo, r.first);
        hi = std::max(hi, r.second);
    }
    return ipc2221Clearance(hi - lo, highAltitude, coated);
}

RouteStats PcbLayout::autoRoute(const Schematic& sch) {
    // Voltage spacing: when the board's largest potential difference needs more than the design-rule clearance
    // (IPC-2221 B2/B3, above 30 V), route with that spacing so the copper meets the DRC voltage check.
    const double ruleClearance = settings.clearance;
    struct RestoreClearance {
        BoardSettings& s;
        double c;
        ~RestoreClearance() { s.clearance = c; }
    } restore{settings, ruleClearance};
    settings.clearance = std::max(ruleClearance, voltageRoutingClearance(sch, settings.highAltitude, settings.coated()));
    return routeAll(sch);
}

RouteStats PcbLayout::routeAll(const Schematic& sch) {
    if (settings.autoSizeNets) autoNetWidths(sch);
    // Reliability net classes: RF lines get their 50 Ω width (when the stack-up allows a practical one);
    // leakage-sensitive and fast nets keep extra spacing and route first, so everything else keeps away from them.
    const NetClassification classes = classifyNets(sch, "");
    for (int net : classes.rf) {
        double z0 = microstripWidth(50.0, settings);
        const std::string& name = sch.nets()[static_cast<size_t>(net)].name;
        if (z0 <= 1.5 && !settings.netWidths.count(name)) settings.netWidths[name] = std::ceil(z0 * 100) / 100;
    }
    std::map<int, double> extraClearance;
    for (const auto& [net, guard] : classes.highImpedance)
        extraClearance[net] = std::max(0.0, leakageSpacing(settings) - settings.clearance);
    for (int net : classes.fast) {
        double w3 = 2 * settings.widthFor(sch.nets()[static_cast<size_t>(net)].name);  // 3W rule: 2 widths edge to edge
        extraClearance[net] = std::max(extraClearance[net], w3 - settings.clearance);
    }
    auto extra = [&](int net) {
        auto it = extraClearance.find(net);
        return it == extraClearance.end() ? 0.0 : it->second;
    };
    const auto ps = pads(sch);
    const auto& nets = sch.nets();
    std::map<int, std::vector<size_t>> netPads;
    for (size_t i = 0; i < ps.size(); ++i)
        if (ps[i].net >= 0) netPads[ps[i].net].push_back(i);

    std::vector<int> order;
    for (auto& [net, list] : netPads)
        if (list.size() >= 2) order.push_back(net);
    // Short nets first: compute bounding-box half perimeter.
    auto hpwl = [&](int net) {
        Rect r(ps[netPads[net][0]].position.x, ps[netPads[net][0]].position.y, ps[netPads[net][0]].position.x,
               ps[netPads[net][0]].position.y);
        for (size_t i : netPads[net]) {
            r.x0 = std::min(r.x0, ps[i].position.x);
            r.x1 = std::max(r.x1, ps[i].position.x);
            r.y0 = std::min(r.y0, ps[i].position.y);
            r.y1 = std::max(r.y1, ps[i].position.y);
        }
        return r.width() + r.height();
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return hpwl(a) < hpwl(b); });

    const double w = settings.trackWidth, clr = settings.clearance;
    const std::vector<double> neckWidths = padNeckWidths(ps);
    // Escape routing first: nets with a fine-pitch pad (one a base-width track must neck down to leave) route
    // before the rest, which would otherwise box those pads in.
    auto fineNet = [&](int net) {
        for (size_t pi : netPads[net])
            if (neckWidths[pi] < w - 1e-9) return true;
        return false;
    };
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return fineNet(a) && !fineNet(b); });
    std::stable_sort(order.begin(), order.end(), [&](int a, int b) { return extra(a) > 0 && extra(b) <= 0; });
    RouteStats best;
    best.failed = std::numeric_limits<int>::max();
    std::vector<Track> bestTracks;
    std::vector<Via> bestVias;

    // Zone nets (pours/planes) connect last, through the poured copper; plane layers are reserved.
    std::vector<int> zoneOrder;
    for (int net : order)
        if (isZoneNet(sch, net)) zoneOrder.push_back(net);
    order.erase(std::remove_if(order.begin(), order.end(), [&](int net) { return isZoneNet(sch, net); }), order.end());
    auto netIndex = [&](const std::string& name) {
        for (const auto& n : nets)
            if (n.name == name) return n.index;
        return -1;
    };

    // Pour/plane-net pads that could not reach their pour: fanned out first in the next pass.
    // Pour/plane-net pads that could not reach their pour: connected first in the next pass, by a track to where
    // the net's main pour was (signals then route around that connection).
    std::set<size_t> forcedConnect;
    std::map<int, std::vector<std::pair<int, size_t>>> mainPour;  // net → grid cells of its main poured cluster
    for (int pass = 0; pass < 8; ++pass) {
        const size_t forcedBefore = forcedConnect.size();
        RoutingGrid grid(settings);
        for (const auto& z : zones) {
            int zn = netIndex(z.net);
            if (zn < 0) continue;
            if (z.plane) grid.setPlane(z.layer, zn);
            else grid.setPour(z.layer, zn);
        }
        for (const auto& p : ps) grid.markPad(p, clr + w / 2);
        // Escape bands: the pad field of each fine-pitch package, widened by ~1 mm.
        {
            std::map<int, Rect> fine;
            for (size_t i = 0; i < ps.size(); ++i) {
                if (neckWidths[i] >= w - 1e-9) continue;
                Rect b = ps[i].bounds();
                auto it = fine.find(ps[i].componentId);
                if (it == fine.end()) fine[ps[i].componentId] = b;
                else it->second = Rect(std::min(it->second.x0, b.x0), std::min(it->second.y0, b.y0),
                                       std::max(it->second.x1, b.x1), std::max(it->second.y1, b.y1));
            }
            for (const auto& [id, body] : fine) grid.addEscapeBand(body, 1.0);
        }
        std::vector<Track> outT;
        std::vector<Via> outV;
        RouteStats stats;
        std::vector<int> failedNets;

        auto padCells = [&](const Pad& pad, std::vector<std::pair<int, size_t>>& cells) {
            for (int l = 0; l < grid.layers(); ++l)
                if (pad.onLayer(l))
                    for (size_t c : grid.padCoreCells(pad)) cells.push_back({l, c});
        };

        auto placeVia = [&](int net, Vec2 at) {
            Via v;
            v.net = net;
            v.position = at;
            v.drill = settings.viaDrill;
            v.diameter = settings.viaDiameter;
            outV.push_back(v);
            ++stats.vias;
            double r = settings.viaDiameter / 2 + clr + extra(net) + w / 2;
            for (int l = 0; l < grid.layers(); ++l) {
                grid.markDisc(l, v.position, r, net);
                grid.markCopperSegment(l, v.position, v.position, settings.viaDiameter / 2, net);
            }
            grid.addCopper(v.position, v.position, settings.viaDiameter / 2, net, -1, settings.viaDrill);
        };
        // Turns an A* path into tracks and through vias, marking them on the grid.
        auto commit = [&](int net, double wn, const RouteResult& rr, std::vector<std::pair<int, size_t>>& tree) {
            size_t k = 0;
            while (k < rr.path.size()) {
                size_t e = k;
                while (e + 1 < rr.path.size() && rr.path[e + 1].layer == rr.path[k].layer) ++e;
                // Polyline path[k..e] on one layer; merge collinear steps.
                int layer = rr.path[k].layer;
                size_t segStart = k;
                for (size_t m = k + 1; m <= e; ++m) {
                    bool last = m == e;
                    bool turn = false;
                    if (!last) {
                        int dx1 = rr.path[m].i - rr.path[m - 1].i, dy1 = rr.path[m].j - rr.path[m - 1].j;
                        int dx2 = rr.path[m + 1].i - rr.path[m].i, dy2 = rr.path[m + 1].j - rr.path[m].j;
                        turn = dx1 != dx2 || dy1 != dy2;
                    }
                    if (last || turn) {
                        Track t;
                        t.net = net;
                        t.layer = layer;
                        t.width = wn;
                        t.a = grid.pos(rr.path[segStart].i, rr.path[segStart].j);
                        t.b = grid.pos(rr.path[m].i, rr.path[m].j);
                        stats.trackLength += (t.b - t.a).length();
                        // Neck down now, so the grid keeps out only what the final (narrowed) copper needs.
                        std::vector<Track> pieces{t};
                        neckDown(pieces, ps);
                        for (const Track& piece : pieces) {
                            outT.push_back(piece);
                            grid.markSegment(layer, piece.a, piece.b, piece.width / 2 + clr + extra(net) + w / 2, net);
                            grid.markCopperSegment(layer, piece.a, piece.b, piece.width / 2 + 1e-6, net);
                            grid.addCopper(piece.a, piece.b, piece.width / 2, net, layer);
                        }
                        segStart = m;
                    }
                }
                for (size_t m = k; m <= e; ++m) tree.push_back({layer, grid.idx(rr.path[m].i, rr.path[m].j)});
                Vec2 viaPos = grid.pos(rr.path[e].i, rr.path[e].j);
                bool duplicate = !outV.empty() && outV.back().net == net && outV.back().position == viaPos;
                if (e + 1 < rr.path.size() && !duplicate) placeVia(net, viaPos);  // layer change → through via
                k = e + 1;
            }
        };
        // Neck zones of the given pads for a track of width wn: per cell the necked half-width (0 outside), the same
        // zones and widths neckDown applies afterwards.
        auto neckZones = [&](const std::vector<size_t>& padList, double wn) {
            std::vector<float> neck;
            for (size_t pi : padList) {
                if (neckWidths[pi] >= wn - 1e-9) continue;
                if (neck.empty()) neck.assign(static_cast<size_t>(grid.cols() * grid.rows()), 0.0f);
                Rect zone = ps[pi].bounds().inflated(clr + wn / 2);
                int i0 = std::max(0, static_cast<int>(std::ceil(zone.x0 / grid.pitch() - 1e-9)));
                int i1 = std::min(grid.cols() - 1, static_cast<int>(std::floor(zone.x1 / grid.pitch() + 1e-9)));
                int j0 = std::max(0, static_cast<int>(std::ceil(zone.y0 / grid.pitch() - 1e-9)));
                int j1 = std::min(grid.rows() - 1, static_cast<int>(std::floor(zone.y1 / grid.pitch() + 1e-9)));
                const float half = static_cast<float>(neckWidths[pi] / 2);
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        float& h = neck[grid.idx(i, j)];
                        h = h > 0 ? std::min(h, half) : half;
                    }
            }
            return neck;
        };
        // Routes from `tree` to pad `tp` and commits the copper; returns false if no path exists.
        auto connect = [&](int net, const std::vector<size_t>& list, std::vector<std::pair<int, size_t>>& tree,
                           const Pad& tp, const std::vector<std::pair<int, size_t>>* extraTargets = nullptr) {
            std::vector<std::vector<char>> mask(static_cast<size_t>(grid.layers()),
                                                std::vector<char>(static_cast<size_t>(grid.cols() * grid.rows()), 0));
            std::vector<std::pair<int, size_t>> targetCells;
            padCells(tp, targetCells);
            if (extraTargets) targetCells.insert(targetCells.end(), extraTargets->begin(), extraTargets->end());
            for (auto [l, c] : targetCells) mask[static_cast<size_t>(l)][c] = 1;
            const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
            const double wideHalf = wn > w + 1e-9 ? wn / 2 : 0.0;
            std::vector<float> neck = neckZones(list, wn);
            RouteResult rr = astar(grid, net, tree, mask, tp.position, 12.0, wideHalf, neck.empty() ? nullptr : &neck);
            for (auto tc : targetCells) tree.push_back(tc);  // later pads may still reach it
            if (!rr.ok) return false;
            commit(net, wn, rr, tree);
            return true;
        };
        // Fan-out: a short stub from a pour/plane-net pad to a via of its own, made before the signal nets so they
        // route around it (fine-pitch ground pins would otherwise be boxed in by signal tracks).
        auto fanout = [&](int net, size_t pi) {
            const Pad& pad = ps[pi];
            const int layer = pad.smdLayer;
            const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
            const size_t n = static_cast<size_t>(grid.cols() * grid.rows());
            std::vector<std::vector<char>> mask(static_cast<size_t>(grid.layers()), std::vector<char>(n, 0));
            const double reach = 3.0;
            const int ci = static_cast<int>(std::lround(pad.position.x / grid.pitch()));
            const int cj = static_cast<int>(std::lround(pad.position.y / grid.pitch()));
            const int k = static_cast<int>(std::ceil(reach / grid.pitch()));
            bool any = false;
            for (int dj = -k; dj <= k; ++dj)
                for (int di = -k; di <= k; ++di) {
                    int i = ci + di, j = cj + dj;
                    if (!grid.inside(i, j) || (grid.pos(i, j) - pad.position).length() > reach) continue;
                    size_t c = grid.idx(i, j);
                    if (!grid.passable(layer, c, net) || !grid.viaAllowed(c, net)) continue;
                    if (wn > w + 1e-9 && !grid.wideClear(layer, grid.pos(i, j), grid.pos(i, j), net, std::max(wn, settings.viaDiameter) / 2))
                        continue;
                    mask[static_cast<size_t>(layer)][c] = 1;
                    any = true;
                }
            if (!any) return false;
            std::vector<std::pair<int, size_t>> src;
            for (size_t c : grid.padCoreCells(pad)) src.push_back({layer, c});
            std::vector<float> neck = neckZones({pi}, wn);
            RouteResult rr = astar(grid, net, src, mask, pad.position, 1e6, wn > w + 1e-9 ? wn / 2 : 0.0,
                                   neck.empty() ? nullptr : &neck);
            if (!rr.ok || rr.path.empty() || rr.path.back().layer != layer) return false;
            std::vector<std::pair<int, size_t>> scratch;
            commit(net, wn, rr, scratch);
            placeVia(net, grid.pos(rr.path.back().i, rr.path.back().j));
            return true;
        };

        auto nearestUnconnected = [&](const std::vector<size_t>& list, const std::vector<bool>& connected) {
            size_t target = 0;
            double bestD = std::numeric_limits<double>::max();
            for (size_t a = 0; a < list.size(); ++a) {
                if (!connected[a]) continue;
                for (size_t b = 0; b < list.size(); ++b) {
                    if (connected[b]) continue;
                    double d = (ps[list[a]].position - ps[list[b]].position).length();
                    if (d < bestD) { bestD = d; target = b; }
                }
            }
            if (bestD == std::numeric_limits<double>::max())
                for (size_t b = 0; b < list.size(); ++b)
                    if (!connected[b]) return b;
            return target;
        };

        if (grid.layers() > 1)
            for (int net : zoneOrder)
                for (size_t pi : netPads[net]) {
                    const Pad& p = ps[pi];
                    if (p.throughHole) continue;
                    bool pourHere = false;
                    for (const auto& z : zones)
                        if (!z.plane && z.layer == p.smdLayer && netIndex(z.net) == net) pourHere = true;
                    bool fine = neckWidths[pi] < w - 1e-9 || std::min(p.size.x, p.size.y) < 0.4;
                    if (!pourHere || fine) fanout(net, pi);
                }
        for (size_t pi : forcedConnect) {
            int net = ps[pi].net;
            auto it = mainPour.find(net);
            if (it == mainPour.end() || it->second.empty()) continue;
            std::vector<std::pair<int, size_t>> from = it->second;
            connect(net, netPads[net], from, ps[pi]);
        }

        for (int net : order) {
            const auto& list = netPads[net];
            std::vector<bool> connected(list.size(), false);
            connected[0] = true;
            std::vector<std::pair<int, size_t>> tree;
            padCells(ps[list[0]], tree);
            bool netFailed = false;
            for (size_t done = 1; done < list.size(); ++done) {
                size_t target = nearestUnconnected(list, connected);
                connected[target] = true;
                ++stats.connections;
                if (connect(net, list, tree, ps[list[target]])) ++stats.routed;
                else netFailed = true;
            }
            if (netFailed) {
                failedNets.push_back(net);
                stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
            }
        }

        // Pour against the signal copper, then join each zone net's pads to its poured islands.
        if (!zoneOrder.empty()) {
            auto fills = fillZones(sch, ps, outT, outV);
            for (int net : zoneOrder) {
                const auto& list = netPads[net];
                DSU d = copperClusters(ps, outT, outV, &fills);
                const size_t base = ps.size() + outT.size() + outV.size();
                const size_t vbaseVias = outV.size();  // vias that existed when the clusters were computed
                // Seed: the cluster holding the largest island of this net (else the first pad's cluster).
                size_t seed = d.find(list[0]);
                double bestArea = 0;
                size_t ib = base;
                for (const auto& f : fills) {
                    if (f.net == net) {
                        std::vector<double> areas(static_cast<size_t>(f.islands), 0.0);
                        for (int id : f.island)
                            if (id >= 0) areas[static_cast<size_t>(id)] += f.cell * f.cell;
                        for (int id = 0; id < f.islands; ++id)
                            if (areas[static_cast<size_t>(id)] > bestArea) {
                                bestArea = areas[static_cast<size_t>(id)];
                                seed = d.find(ib + static_cast<size_t>(id));
                            }
                    }
                    ib += static_cast<size_t>(f.islands);
                }
                std::vector<std::pair<int, size_t>> tree;
                std::set<size_t> joined;  // clusters already part of the tree
                // Routing-grid cells of a copper cluster: its poured islands, pads and vias.
                auto clusterCells = [&](size_t cluster, std::vector<std::pair<int, size_t>>& cells) {
                    size_t off = base;
                    for (const auto& f : fills) {
                        if (f.net == net)
                            for (int j = 0; j < grid.rows(); ++j)
                                for (int i = 0; i < grid.cols(); ++i) {
                                    int id = f.islandAt(grid.pos(i, j));
                                    if (id >= 0 && d.find(off + static_cast<size_t>(id)) == cluster)
                                        cells.push_back({f.layer, grid.idx(i, j)});
                                }
                        off += static_cast<size_t>(f.islands);
                    }
                    for (size_t pi : list)
                        if (d.find(pi) == cluster) padCells(ps[pi], cells);
                    const size_t vbase = ps.size() + outT.size();
                    for (size_t v = 0; v < vbaseVias; ++v) {
                        if (outV[v].net != net || d.find(vbase + v) != cluster) continue;
                        int i = static_cast<int>(std::lround(outV[v].position.x / grid.pitch()));
                        int j = static_cast<int>(std::lround(outV[v].position.y / grid.pitch()));
                        if (grid.inside(i, j))
                            for (int l = 0; l < grid.layers(); ++l) cells.push_back({l, grid.idx(i, j)});
                    }
                };
                auto absorb = [&](size_t cluster) {
                    if (!joined.insert(cluster).second) return;
                    clusterCells(cluster, tree);
                };
                absorb(seed);
                mainPour[net].clear();
                clusterCells(seed, mainPour[net]);
                std::vector<bool> connected(list.size(), false);
                for (size_t a = 0; a < list.size(); ++a) connected[a] = joined.count(d.find(list[a])) > 0;
                bool netFailed = false;
                stats.connections += static_cast<int>(list.size()) - 1;
                int pending = 0;
                for (size_t a = 0; a < list.size(); ++a) pending += !connected[a];
                stats.routed += static_cast<int>(list.size()) - 1 - pending;
                while (pending > 0) {
                    size_t target = nearestUnconnected(list, connected);
                    size_t cluster = d.find(list[target]);
                    std::vector<std::pair<int, size_t>> clusterTargets;
                    clusterCells(cluster, clusterTargets);
                    bool ok = connect(net, list, tree, ps[list[target]], &clusterTargets);
                    if (!ok) forcedConnect.insert(list[target]);
                    // The whole pre-existing cluster (pads on the same island) joins with it.
                    for (size_t a = 0; a < list.size(); ++a) {
                        if (connected[a] || d.find(list[a]) != cluster) continue;
                        connected[a] = true;
                        --pending;
                        if (ok) ++stats.routed;
                        else netFailed = true;
                    }
                    if (ok) absorb(cluster);
                }
                if (netFailed) {
                    failedNets.push_back(net);
                    stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
                }
            }
        }
        stats.failed = stats.connections - stats.routed;
        if (stats.failed < best.failed) {
            best = stats;
            bestTracks = outT;
            bestVias = outV;
        }
        if (stats.failed == 0) break;
        // Rip-up and retry with the failing signal nets promoted to the front (zone nets always connect last).
        std::vector<int> next;
        for (int n : failedNets)
            if (std::find(order.begin(), order.end(), n) != order.end()) next.push_back(n);
        for (int n : order)
            if (std::find(next.begin(), next.end(), n) == next.end()) next.push_back(n);
        if (next == order && pass > 0 && forcedConnect.size() == forcedBefore) break;
        order = next;
    }

    tracks.clear();
    vias.clear();
    neckDown(bestTracks, ps);
    for (auto& t : bestTracks) addTrack(t);
    for (auto& v : bestVias) addVia(v);
    cleanupRouting(sch);
    if (best.failed == std::numeric_limits<int>::max()) best.failed = 0;
    return best;
}

int PcbLayout::cleanupRouting(const Schematic& sch) {
    const auto ps = pads(sch);
    const double clr = settings.clearance;
    int changes = 0;
    auto key = [](int layer, Vec2 p) {
        return std::make_tuple(layer, std::llround(p.x * 1e4), std::llround(p.y * 1e4));
    };
    // A joint can be reshaped only where two pieces of one track meet in open board: no pad or via there.
    auto anchored = [&](int layer, Vec2 p) {
        for (const auto& pd : ps)
            if (pd.onLayer(layer) && padDistance(pd, p) <= 1e-6) return true;
        for (const auto& v : vias)
            if ((v.position - p).length() <= 1e-6) return true;
        return false;
    };
    // Clearance of a new piece of copper (segment a–b, width w, net) to everything of other nets on its layer.
    auto clear = [&](int layer, int net, Vec2 a, Vec2 b, double w, size_t skipA, size_t skipB) {
        for (size_t i = 0; i < tracks.size(); ++i) {
            const Track& t = tracks[i];
            if (i == skipA || i == skipB || t.layer != layer || t.net == net) continue;
            if (segmentSegmentDistance(a, b, t.a, t.b) - (w + t.width) / 2 < clr - 1e-6) return false;
        }
        for (const auto& pd : ps) {
            if (!pd.onLayer(layer) || (pd.net == net && net >= 0)) continue;
            double d = pd.round ? pointSegmentDistance(pd.position, a, b) - std::min(pd.size.x, pd.size.y) / 2
                                : segmentRectDistance(a, b, pd.bounds());
            if (d - w / 2 < clr - 1e-6) return false;
        }
        for (const auto& v : vias) {
            if (v.net == net) continue;
            if (pointSegmentDistance(v.position, a, b) - v.diameter / 2 - w / 2 < clr - 1e-6) return false;
        }
        Vec2 mid = (a + b) * 0.5;
        return settings.edgeDistance(mid) >= settings.edgeClearance + w / 2 - 1e-6;
    };

    for (int pass = 0; pass < 4; ++pass) {
        std::map<std::tuple<int, long long, long long>, std::vector<size_t>> ends;
        for (size_t i = 0; i < tracks.size(); ++i) {
            ends[key(tracks[i].layer, tracks[i].a)].push_back(i);
            ends[key(tracks[i].layer, tracks[i].b)].push_back(i);
        }
        std::vector<bool> removed(tracks.size(), false), touched(tracks.size(), false);
        std::vector<Track> added;
        int before = changes;
        for (const auto& [k, list] : ends) {
            if (list.size() != 2) continue;
            size_t i = list[0], j = list[1];
            if (i == j || removed[i] || removed[j] || touched[i] || touched[j]) continue;
            Track& ti = tracks[i];
            Track& tj = tracks[j];
            if (ti.net != tj.net || std::fabs(ti.width - tj.width) > 1e-9) continue;
            Vec2 p{std::get<1>(k) / 1e4, std::get<2>(k) / 1e4};
            if (anchored(ti.layer, p)) continue;
            bool iAtA = (ti.a - p).length() < 1e-3, jAtA = (tj.a - p).length() < 1e-3;
            Vec2 oi = iAtA ? ti.b : ti.a, oj = jAtA ? tj.b : tj.a;
            Vec2 u = oi - p, v = oj - p;
            double li = u.length(), lj = v.length();
            if (li < 1e-6 || lj < 1e-6) continue;
            double cosine = (u.x * v.x + u.y * v.y) / (li * lj);
            if (cosine < -0.99999) {  // collinear: one segment
                (iAtA ? ti.a : ti.b) = oj;
                removed[j] = true;
                touched[i] = true;
                ++changes;
            } else if (std::fabs(cosine) < 1e-6) {  // right angle: 45° chamfer
                // The largest chamfer (up to 1 mm, under half of each leg) that keeps clearance.
                double c = std::min(1.0, 0.45 * std::min(li, lj));
                Vec2 a, b;
                bool fits = false;
                for (; c >= ti.width && !fits; c *= 0.7) {
                    a = p + u * (c / li);
                    b = p + v * (c / lj);
                    fits = clear(ti.layer, ti.net, a, b, ti.width, i, j);
                }
                if (!fits) continue;
                (iAtA ? ti.a : ti.b) = a;
                (jAtA ? tj.a : tj.b) = b;
                Track diag = ti;
                diag.a = a;
                diag.b = b;
                added.push_back(diag);
                touched[i] = touched[j] = true;
                ++changes;
            }
        }
        std::vector<Track> kept;
        for (size_t i = 0; i < tracks.size(); ++i)
            if (!removed[i]) kept.push_back(tracks[i]);
        tracks = std::move(kept);
        for (auto& t : added) addTrack(t);
        if (changes == before) break;
    }
    return changes;
}

std::vector<double> PcbLayout::padNeckWidths(const std::vector<Pad>& ps) const {
    // Widest track that can leave a pad between its package neighbours: the pad's own width, limited so the track
    // keeps the clearance to the nearest other-net pad of the same footprint; never below the fabrication minimum.
    std::vector<double> out(ps.size());
    for (size_t i = 0; i < ps.size(); ++i) {
        const Pad& p = ps[i];
        double minor = std::min(p.size.x, p.size.y);
        double gap = std::numeric_limits<double>::max();
        for (size_t j = 0; j < ps.size(); ++j) {
            const Pad& o = ps[j];
            if (j == i || o.componentId != p.componentId || (o.net == p.net && p.net >= 0)) continue;
            bool share = false;
            for (int l = 0; l < settings.layerCount && !share; ++l) share = p.onLayer(l) && o.onLayer(l);
            if (share) gap = std::min(gap, rectRectDistance(p.bounds(), o.bounds()));
        }
        double limit = gap < 1e6 ? 2 * (gap + minor / 2 - settings.clearance) : minor;
        out[i] = std::max(settings.minTrackWidth, std::min(minor, limit));
    }
    return out;
}

void PcbLayout::neckDown(std::vector<Track>& out, const std::vector<Pad>& ps) const {
    // Wherever a track runs within a clearance of one of its own pads that is narrower than the track (or whose
    // package neighbours are too close for it), that stretch is narrowed to the pad's neck width, so it never
    // overhangs into the gap to the neighbouring fine-pitch pads.
    const auto neck = padNeckWidths(ps);
    for (int iter = 0; iter < 4; ++iter) {
        bool changed = false;
        std::vector<Track> result;
        for (const Track& t : out) {
            bool split = false;
            for (size_t k = 0; k < ps.size() && !split; ++k) {
                const Pad& p = ps[k];
                if (p.net != t.net || !p.onLayer(t.layer) || neck[k] >= t.width - 1e-9) continue;
                Rect zone = p.bounds().inflated(settings.clearance + t.width / 2);
                // Liang–Barsky clip of the segment to the zone.
                double t0 = 0, t1 = 1;
                Vec2 d = t.b - t.a;
                const double pq[4][2] = {{-d.x, t.a.x - zone.x0}, {d.x, zone.x1 - t.a.x}, {-d.y, t.a.y - zone.y0}, {d.y, zone.y1 - t.a.y}};
                bool inside = true;
                for (const auto& e : pq) {
                    if (std::fabs(e[0]) < 1e-12) {
                        if (e[1] < 0) inside = false;
                        continue;
                    }
                    double r = e[1] / e[0];
                    if (e[0] < 0) t0 = std::max(t0, r);
                    else t1 = std::min(t1, r);
                }
                if (!inside || t1 - t0 < 1e-9) continue;
                Vec2 a = t.a + d * t0, b = t.a + d * t1;
                if (t0 > 1e-9 && (a - t.a).length() > 1e-6) {
                    Track head = t;
                    head.b = a;
                    result.push_back(head);
                }
                Track mid = t;
                mid.a = a;
                mid.b = b;
                mid.width = neck[k];
                result.push_back(mid);
                if (t1 < 1 - 1e-9 && (t.b - b).length() > 1e-6) {
                    Track tail = t;
                    tail.a = b;
                    result.push_back(tail);
                }
                split = changed = true;
            }
            if (!split) result.push_back(t);
        }
        out = std::move(result);
        if (!changed) break;
    }
}

std::map<std::string, double> PcbLayout::autoNetWidths(const Schematic& sch) {
    std::map<std::string, double> set;
    if (sch.groundNet() < 0) return set;
    DcResult dc = Simulator(sch).dcOperatingPoint();
    if (!dc.converged) return set;
    std::map<int, double> current;
    for (const auto& d : dc.devices) {
        const Component* c = sch.find(d.componentId);
        if (!c) continue;
        for (size_t pin = 0; pin < c->def().pins.size(); ++pin) {
            int n = sch.netOf({c->id, static_cast<int>(pin)});
            if (n >= 0) current[n] = std::max(current[n], std::fabs(d.current));
        }
    }
    const auto& nets = sch.nets();
    for (auto [net, amps] : current) {
        double need = ipc2221TrackWidth(amps, settings.maxTempRise, settings.copperWeightOz, false) * 1.25;
        need = std::min(3.0, std::ceil(need / 0.05) * 0.05);
        if (need <= settings.trackWidth + 1e-9) continue;
        const std::string& name = nets[static_cast<size_t>(net)].name;
        double& w = settings.netWidths[name];
        if (w < need) {
            w = need;
            set[name] = need;
        }
    }
    return set;
}

// ===================================================================== DRC

std::vector<RuleViolation> PcbLayout::runDRC(const Schematic& sch) const {
    std::vector<RuleViolation> out;
    const double eps = 1e-3;
    const double clr = settings.clearance;
    const double fabClr = std::min(settings.minClearance, clr);
    auto add = [&](Severity s, const std::string& code, const std::string& msg, Vec2 loc, std::vector<int> comps = {}) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        v.location = loc;
        v.hasLocation = true;
        v.components = std::move(comps);
        out.push_back(std::move(v));
    };
    // Voltage range of each net (DC operating point, widened to SIN/PULSE source peaks).
    std::map<int, std::pair<double, double>> netRange = netVoltageRanges(sch);
    DcResult dc;
    if (!netRange.empty()) dc = Simulator(sch).dcOperatingPoint();
    const std::string spacingColumn =
        std::string(ipc2221ColumnName(externalSpacingColumn(settings.highAltitude, settings.coated()))) +
        (settings.coated() ? " (conformal coated)" : settings.highAltitude ? " (altitude)" : "");
    auto voltageNeed = [&](int a, int b) {
        auto ia = netRange.find(a), ib = netRange.find(b);
        if (a < 0 || b < 0 || ia == netRange.end() || ib == netRange.end()) return std::make_pair(0.0, 0.0);
        double dv = std::max(std::fabs(ia->second.second - ib->second.first), std::fabs(ib->second.second - ia->second.first));
        return std::make_pair(dv, ipc2221Clearance(dv, settings.highAltitude, settings.coated()));
    };

    // Below the fabrication minimum → error; between it and the design rule → warning; below the IPC-2221 voltage
    // spacing for the nets' potential difference → error.
    auto clearanceCheck = [&](double d, const std::string& what, Vec2 loc, std::vector<int> comps = {}, int netA = -1,
                              int netB = -1) {
        auto [dv, hvNeed] = voltageNeed(netA, netB);
        if (d > 0 && hvNeed > clr + eps && d < hvNeed - eps) {
            char hv[160];
            std::snprintf(hv, sizeof hv, " is below the IPC-2221B %s spacing %.2f mm for %.0f V.",
                          spacingColumn.c_str(), hvNeed, dv);
            add(Severity::Error, "DRC_HV_CLEARANCE", what + hv, loc, std::move(comps));
            return;
        }
        if (d >= clr - eps) return;
        char buf[96];
        if (d <= 0) {
            add(Severity::Error, "DRC_SHORT", what + " — copper overlaps (short circuit).", loc, std::move(comps));
        } else if (d < fabClr - eps) {
            std::snprintf(buf, sizeof buf, " (fabrication minimum %.3f mm)", fabClr);
            add(Severity::Error, "DRC_CLEARANCE", what + buf + ".", loc, std::move(comps));
        } else {
            std::snprintf(buf, sizeof buf, " (design rule %.3f mm)", clr);
            add(Severity::Warning, "DRC_CLEARANCE_RULE", what + buf + ".", loc, std::move(comps));
        }
    };
    const auto& nets = sch.nets();
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(n)].name : std::string("(none)"); };
    auto fmt = [](double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.3f mm", v);
        return std::string(b);
    };

    // Placement checks.
    std::vector<const Component*> placed;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint()) continue;
        if (!c.pcb.placed) {
            add(Severity::Warning, "DRC_UNPLACED", c.ref + " has not been placed on the board.", {0, 0}, {c.id});
            continue;
        }
        placed.push_back(&c);
        Rect cy = courtyard(c);
        if (!settings.rectInside(cy, -eps)) {
            bool inHole = false;
            for (const auto& h : settings.holes)
                if (pointRectDistance(h.position, cy) < h.keepout / 2 - eps) inHole = true;
            Rect inner = cy.inflated(-eps);
            bool outside = !settings.contains(cy.center());
            auto poly = settings.outlinePolygon();
            for (size_t a = 0, b = poly.size() - 1; a < poly.size() && !outside; b = a++)
                outside = segmentRectDistance(poly[b], poly[a], inner) <= 0;
            if (outside)
                add(Severity::Error, "DRC_OUT_OF_BOARD", c.ref + " extends beyond the board outline.", c.pcb.position, {c.id});
            if (inHole)
                add(Severity::Error, "DRC_HOLE_KEEPOUT", c.ref + " overlaps a mounting-hole keep-out (screw head / washer).",
                    c.pcb.position, {c.id});
        }
    }
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j)
            if (placed[i]->pcb.bottom == placed[j]->pcb.bottom && courtyard(*placed[i]).intersects(courtyard(*placed[j])))
                add(Severity::Warning, "DRC_COURTYARD_OVERLAP",
                    "Courtyards of " + placed[i]->ref + " and " + placed[j]->ref + " overlap.",
                    courtyard(*placed[i]).center(), {placed[i]->id, placed[j]->id});

    auto ps = pads(sch);
    // Spacing inside one footprint is fixed by the package (0.5 mm-pitch QFN pads are ~0.2 mm apart): it is held to
    // the fabrication minimum only, not to the board's design clearance.
    auto footprintCheck = [&](double d, const std::string& what, Vec2 loc, std::vector<int> comps, int netA, int netB) {
        auto [dv, hvNeed] = voltageNeed(netA, netB);  // voltage spacing still applies inside a footprint
        if (d > 0 && hvNeed > fabClr + eps && d < hvNeed - eps) {
            char hv[160];
            std::snprintf(hv, sizeof hv, " is below the IPC-2221B %s spacing %.2f mm for %.0f V.",
                          spacingColumn.c_str(), hvNeed, dv);
            add(Severity::Error, "DRC_HV_CLEARANCE", what + hv, loc, std::move(comps));
            return;
        }
        if (d <= 0) add(Severity::Error, "DRC_SHORT", what + " — copper overlaps (short circuit).", loc, std::move(comps));
        else if (d < fabClr - eps) {
            char b[96];
            std::snprintf(b, sizeof b, " (fabrication minimum %.3f mm)", fabClr);
            add(Severity::Error, "DRC_CLEARANCE", what + b + ".", loc, std::move(comps));
        }
    };
    // Pad ↔ pad.
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) {
            if (ps[i].net == ps[j].net && ps[i].net >= 0) continue;
            bool share = false;
            for (int l = 0; l < settings.layerCount && !share; ++l) share = ps[i].onLayer(l) && ps[j].onLayer(l);
            if (!share) continue;
            double d = rectRectDistance(ps[i].bounds(), ps[j].bounds());
            if (ps[i].componentId == ps[j].componentId) {
                footprintCheck(d, "Pad clearance " + fmt(d) + " between " + netName(ps[i].net) + " and " + netName(ps[j].net),
                               (ps[i].position + ps[j].position) * 0.5, {ps[i].componentId}, ps[i].net, ps[j].net);
                continue;
            }
            clearanceCheck(d, "Pad clearance " + fmt(d) + " between " + netName(ps[i].net) + " and " + netName(ps[j].net), (ps[i].position + ps[j].position) * 0.5, {ps[i].componentId, ps[j].componentId}, ps[i].net, ps[j].net);
        }
    // Track ↔ pad, track ↔ track, track ↔ edge.
    for (size_t t = 0; t < tracks.size(); ++t) {
        const Track& tr = tracks[t];
        if (tr.layer < 0 || tr.layer >= settings.layerCount)
            add(Severity::Error, "DRC_LAYER", "Track on " + netName(tr.net) + " uses a layer that is not in the " +
                std::to_string(settings.layerCount) + "-layer stack-up.", tr.a);
        if (settings.segmentEdgeDistance(tr.a, tr.b) - tr.width / 2 < settings.edgeClearance - eps)
            add(Severity::Error, "DRC_EDGE_CLEARANCE", "Track on " + netName(tr.net) + " is too close to the board edge.", tr.a);
        for (const auto& h : settings.holes)
            if (pointSegmentDistance(h.position, tr.a, tr.b) - tr.width / 2 < h.keepout / 2 - eps) {
                add(Severity::Error, "DRC_HOLE_KEEPOUT", "Track on " + netName(tr.net) + " enters a mounting-hole keep-out.",
                    h.position);
                break;
            }
        for (const auto& p : ps) {
            if (p.net == tr.net || !p.onLayer(tr.layer)) continue;
            double d = (p.round ? std::max(0.0, pointSegmentDistance(p.position, tr.a, tr.b) - std::min(p.size.x, p.size.y) / 2)
                                : segmentRectDistance(tr.a, tr.b, p.bounds())) -
                       tr.width / 2;
            // Where the track is still inside its own pad of the same footprint, the gap is the package's pad gap.
            Vec2 ab = tr.b - tr.a;
            double len2 = ab.dot(ab);
            double tt = len2 > 0 ? std::clamp((p.position - tr.a).dot(ab) / len2, 0.0, 1.0) : 0.0;
            Vec2 closest = tr.a + ab * tt;
            bool inOwnPad = false;
            for (const auto& own : ps)
                if (own.net == tr.net && own.componentId == p.componentId && own.onLayer(tr.layer) &&
                    padDistance(own, closest) <= 0)
                    inOwnPad = true;
            if (inOwnPad) {
                footprintCheck(d, "Track (" + netName(tr.net) + ") to pad (" + netName(p.net) + ") clearance " +
                                      fmt(std::max(0.0, d)), p.position, {p.componentId}, tr.net, p.net);
                continue;
            }
            clearanceCheck(d, "Track (" + netName(tr.net) + ") to pad (" + netName(p.net) + ") clearance " + fmt(std::max(0.0, d)), p.position, {p.componentId}, tr.net, p.net);
        }
        for (size_t u = t + 1; u < tracks.size(); ++u) {
            const Track& o = tracks[u];
            if (o.net == tr.net || o.layer != tr.layer) continue;
            double d = segmentSegmentDistance(tr.a, tr.b, o.a, o.b) - (tr.width + o.width) / 2;
            clearanceCheck(d, "Track clearance " + fmt(std::max(0.0, d)) + " between " + netName(tr.net) + " and " + netName(o.net), (tr.a + tr.b) * 0.5, {}, tr.net, o.net);
        }
        for (const auto& v : vias) {
            if (v.net == tr.net) continue;
            double d = pointSegmentDistance(v.position, tr.a, tr.b) - tr.width / 2 - v.diameter / 2;
            clearanceCheck(d, "Via (" + netName(v.net) + ") to track (" + netName(tr.net) + ") clearance " + fmt(std::max(0.0, d)), v.position, {}, v.net, tr.net);
        }
    }
    for (size_t i = 0; i < vias.size(); ++i) {
        for (const auto& p : ps) {
            if (p.net == vias[i].net) continue;
            double d = padDistance(p, vias[i].position) - vias[i].diameter / 2;
            clearanceCheck(d, "Via (" + netName(vias[i].net) + ") to pad (" + netName(p.net) + ") clearance " + fmt(std::max(0.0, d)), vias[i].position, {p.componentId}, vias[i].net, p.net);
        }
        for (size_t j = i + 1; j < vias.size(); ++j) {
            if (vias[i].net == vias[j].net) continue;
            double d = (vias[i].position - vias[j].position).length() - (vias[i].diameter + vias[j].diameter) / 2;
            clearanceCheck(d, "Via-to-via clearance " + fmt(std::max(0.0, d)), vias[i].position, {}, vias[i].net, vias[j].net);
        }
        if (settings.edgeDistance(vias[i].position) < settings.edgeClearance - eps)
            add(Severity::Error, "DRC_EDGE_CLEARANCE", "Via is too close to the board edge.", vias[i].position);
        if (settings.holeDistance(vias[i].position) < vias[i].diameter / 2 - eps)
            add(Severity::Error, "DRC_HOLE_KEEPOUT", "Via inside a mounting-hole keep-out.", vias[i].position);
    }

    // ---- Manufacturability (fabrication limits of the selected rule preset).
    char buf[200];
    for (const auto& tr : tracks)
        if (tr.width < settings.minTrackWidth - eps) {
            std::snprintf(buf, sizeof buf, "Track width %.3f mm on %s is below the minimum %.3f mm.", tr.width,
                          netName(tr.net).c_str(), settings.minTrackWidth);
            add(Severity::Error, "DRC_TRACK_WIDTH", buf, (tr.a + tr.b) * 0.5);
        }
    struct Hole {
        Vec2 at;
        double drill;
        std::string what;
    };
    std::vector<Hole> holes;
    for (const auto& v : vias) {
        holes.push_back({v.position, v.drill, "Via"});
        if (v.drill < settings.minDrill - eps) {
            std::snprintf(buf, sizeof buf, "Via drill %.3f mm is below the minimum %.3f mm.", v.drill, settings.minDrill);
            add(Severity::Error, "DRC_DRILL_SIZE", buf, v.position);
        }
        double ring = (v.diameter - v.drill) / 2;
        if (ring < settings.minAnnularRing - eps) {
            std::snprintf(buf, sizeof buf, "Via annular ring %.3f mm is below the minimum %.3f mm.", ring, settings.minAnnularRing);
            add(Severity::Error, "DRC_ANNULAR_RING", buf, v.position);
        }
    }
    for (const auto& h : settings.holes) holes.push_back({h.position, h.drill, "Mounting"});
    for (size_t i = 0; i < settings.holes.size(); ++i)
        if (settings.edgeDistance(settings.holes[i].position) < settings.holes[i].keepout / 2 - eps)
            add(Severity::Warning, "DRC_HOLE_EDGE", "Mounting hole keep-out extends past the board edge.",
                settings.holes[i].position);
    for (const auto& p : ps) {
        if (!p.throughHole || p.drill <= 0) continue;
        holes.push_back({p.position, p.drill, "Pad"});
        if (p.drill < settings.minDrill - eps) {
            std::snprintf(buf, sizeof buf, "Pad drill %.3f mm is below the minimum %.3f mm.", p.drill, settings.minDrill);
            add(Severity::Error, "DRC_DRILL_SIZE", buf, p.position, {p.componentId});
        }
        double ring = (std::min(p.size.x, p.size.y) - p.drill) / 2;
        if (ring < settings.minAnnularRing - eps) {
            std::snprintf(buf, sizeof buf, "Pad annular ring %.3f mm is below the minimum %.3f mm.", ring, settings.minAnnularRing);
            add(Severity::Error, "DRC_ANNULAR_RING", buf, p.position, {p.componentId});
        }
    }
    for (size_t i = 0; i < holes.size(); ++i)
        for (size_t j = i + 1; j < holes.size(); ++j) {
            double gap = (holes[i].at - holes[j].at).length() - (holes[i].drill + holes[j].drill) / 2;
            if (gap < settings.minHoleToHole - eps) {
                std::snprintf(buf, sizeof buf, "%s-to-%s hole spacing %.3f mm is below the minimum %.3f mm.",
                              holes[i].what.c_str(), holes[j].what.c_str(), std::max(0.0, gap), settings.minHoleToHole);
                add(Severity::Error, "DRC_HOLE_SPACING", buf, (holes[i].at + holes[j].at) * 0.5);
            }
        }

    // Vias inside SMD pads wick solder away from the joint.
    for (const auto& v : vias)
        for (const auto& p : ps)
            if (!p.throughHole && p.net == v.net && padDistance(p, v.position) <= 0) {
                add(Severity::Warning, "DRC_VIA_IN_PAD",
                    "Via inside an SMD pad on " + netName(v.net) + " — tent/plug it or move it off the pad.", v.position,
                    {p.componentId});
                break;
            }

    // Track geometry: dangling ends and acute (< 90°) joins that trap etchant.
    const auto& fills = zoneFills(sch);
    auto touchesCopper = [&](size_t self, Vec2 end) {
        const Track& t = tracks[self];
        for (const auto& f : fills)
            if (f.net == t.net && f.layer == t.layer && f.islandNear(end, t.width / 2) >= 0) return true;
        for (const auto& p : ps)
            if (p.net == t.net && p.onLayer(t.layer) && padDistance(p, end) <= t.width / 2) return true;
        for (const auto& v : vias)
            if (v.net == t.net && (v.position - end).length() <= v.diameter / 2) return true;
        for (size_t k = 0; k < tracks.size(); ++k) {
            if (k == self || tracks[k].net != t.net || tracks[k].layer != t.layer) continue;
            if (pointSegmentDistance(end, tracks[k].a, tracks[k].b) <= (tracks[k].width + t.width) / 4) return true;
        }
        return false;
    };
    for (size_t t = 0; t < tracks.size(); ++t)
        for (Vec2 end : {tracks[t].a, tracks[t].b})
            if (!touchesCopper(t, end)) {
                add(Severity::Warning, "DRC_DANGLING_TRACK", "Track on " + netName(tracks[t].net) + " ends without a connection (stub).",
                    end);
                break;
            }
    for (size_t t = 0; t < tracks.size(); ++t)
        for (size_t u = t + 1; u < tracks.size(); ++u) {
            const Track& a = tracks[t];
            const Track& b = tracks[u];
            if (a.net != b.net || a.layer != b.layer) continue;
            for (Vec2 pa : {a.a, a.b})
                for (Vec2 pb : {b.a, b.b}) {
                    if ((pa - pb).length() > 1e-6) continue;
                    Vec2 da = (pa == a.a ? a.b : a.a) - pa, db = (pb == b.a ? b.b : b.a) - pb;
                    double la = da.length(), lb = db.length();
                    if (la < 1e-9 || lb < 1e-9) continue;
                    double angle = std::acos(std::clamp(da.dot(db) / (la * lb), -1.0, 1.0)) * 180.0 / kPi;
                    // A join inside a pad or via is covered by copper and cannot trap etchant.
                    bool covered = false;
                    for (const auto& p : ps)
                        if (p.net == a.net && p.onLayer(a.layer) && padDistance(p, pa) <= 0) covered = true;
                    for (const auto& v : vias)
                        if (v.net == a.net && (v.position - pa).length() <= v.diameter / 2) covered = true;
                    if (angle < 89.0 && !covered) {
                        std::snprintf(buf, sizeof buf, "Acute %.0f° track join on %s (acid trap) — use 90° or 135° corners.",
                                      angle, netName(a.net).c_str());
                        add(Severity::Warning, "DRC_ACUTE_ANGLE", buf, pa);
                    }
                }
        }

    // IPC-2221 current capacity using the DC operating point (largest device current on each net).
    if (!tracks.empty()) {
        if (dc.converged) {
            std::map<int, double> netCurrent;
            for (const auto& d : dc.devices) {
                const Component* c = sch.find(d.componentId);
                if (!c) continue;
                for (size_t pin = 0; pin < c->def().pins.size(); ++pin) {
                    int n = sch.netOf({c->id, static_cast<int>(pin)});
                    if (n >= 0) netCurrent[n] = std::max(netCurrent[n], std::fabs(d.current));
                }
            }
            std::set<std::pair<int, int>> reported;
            for (const auto& tr : tracks) {
                auto it = netCurrent.find(tr.net);
                if (it == netCurrent.end()) continue;
                // A poured net carries its current in the pour; its tracks are short pad stubs.
                if (isZoneNet(sch, tr.net)) continue;
                bool inner = tr.layer != kTopLayer && tr.layer != settings.bottomLayer();
                double need = ipc2221TrackWidth(it->second, settings.maxTempRise, settings.copperWeightOz, inner);
                if (tr.width + eps < need && reported.insert({tr.net, inner ? 1 : 0}).second) {
                    std::snprintf(buf, sizeof buf,
                                  "%s carries %.0f mA: IPC-2221 needs %.2f mm %s track width for a %.0f °C rise (track is %.2f mm).",
                                  netName(tr.net).c_str(), it->second * 1000, need, inner ? "inner" : "outer",
                                  settings.maxTempRise, tr.width);
                    add(Severity::Warning, "DRC_TRACK_CURRENT", buf, (tr.a + tr.b) * 0.5);
                }
            }
        }
    }

    for (const auto& f : fills) {
        const CopperZone& z = zones[static_cast<size_t>(f.zone)];
        if (f.net < 0)
            add(Severity::Warning, "DRC_ZONE_NET", "Copper pour on " + copperLayerName(z.layer, settings.layerCount) +
                " refers to net \"" + z.net + "\" which does not exist.", {0, 0});
        else if (z.layer < 0 || z.layer >= settings.layerCount)
            add(Severity::Warning, "DRC_ZONE_LAYER", "Copper pour for " + z.net + " is on a layer outside the " +
                std::to_string(settings.layerCount) + "-layer stack-up.", {0, 0});
        else if (f.islands == 0)
            add(Severity::Warning, "DRC_ZONE_EMPTY", "Copper pour for " + z.net + " on " +
                copperLayerName(z.layer, settings.layerCount) + " has no copper (nothing of the net to connect to).", {0, 0});
    }
    for (size_t i = 0; i < zones.size(); ++i)
        for (size_t j = 0; j < zones.size(); ++j)
            if (i != j && zones[i].plane && zones[i].layer == zones[j].layer && zones[i].net != zones[j].net) {
                add(Severity::Warning, "DRC_PLANE_SHARED", copperLayerName(zones[i].layer, settings.layerCount) +
                    " is a plane for " + zones[i].net + " but also has a pour for " + zones[j].net + ".", {0, 0});
                break;
            }
    // Tracks of other nets on a reserved plane layer.
    for (const auto& tr : tracks)
        for (const auto& z : zones)
            if (z.plane && z.layer == tr.layer && z.net != netName(tr.net)) {
                add(Severity::Warning, "DRC_PLANE_TRACK", "Track on " + netName(tr.net) + " runs on the " + z.net +
                    " plane layer and cuts it.", tr.a);
                break;
            }

    // Connectivity.
    auto lines = ratsnest(sch);
    if (!lines.empty()) {
        add(Severity::Error, "DRC_UNROUTED",
            std::to_string(lines.size()) + " connection(s) are not routed. Run the autorouter or draw tracks.", lines[0].first);
    }
    if (out.empty()) {
        RuleViolation v;
        v.severity = Severity::Info;
        v.code = "DRC_PASS";
        v.message = "Design rule check passed: " + std::to_string(tracks.size()) + " tracks, " +
                    std::to_string(vias.size()) + " vias, " + std::to_string(ps.size()) + " pads" +
                    (zones.empty() ? std::string(".") : ", " + std::to_string(zones.size()) + " copper pours.");
        out.push_back(v);
    }
    return out;
}

}  // namespace sieda
