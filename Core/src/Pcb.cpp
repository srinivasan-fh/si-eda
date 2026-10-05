#include "sieda/Pcb.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Isolation.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Stackup.hpp"

#include "sieda/Simulator.hpp"
#include "GlobalRouter.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <tuple>
#include <unordered_map>
#include <numeric>
#include <queue>
#include <mutex>
#include <set>
#include <thread>

namespace sieda {

// ===================================================================== pads & courtyards

namespace {
Vec2 transformFootprintPoint(Vec2 p, const PcbPlacement& pl) {
    if (pl.bottom) p.x = -p.x;
    return pl.position + rotate90(p, pl.rotation);
}

bool quarterTurned(int rotation) { return ((rotation / 90) % 2 + 2) % 2 == 1; }

/// Where a part sits: top (0) or bottom (1) surface, or inside the board on an inner layer (embedded passives).
/// Courtyards only collide with parts on the same plane.
/// Placement classes the auto-placer keeps apart: radios away from switching power (≥ 10 mm), and ceramic
/// capacitors out of the flex zone around mounting holes (MLCC cracking).
std::string upperName(const Component& c) {
    std::string n = c.def().name;
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* p = CustomPartRegistry::instance().find(c.customPart)) n = p->spec.name;
    for (auto& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return n;
}
bool isRadioPart(const Component& c) {
    if (c.kind != ComponentKind::Custom) return false;
    const std::string n = upperName(c);
    for (const char* k : {"NRF24", "ESP32", "ESP8266", "SX127", "SX126", "LORA", "CC1101", "RFM9"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}
bool isImuPart(const Component& c) {
    if (c.kind != ComponentKind::Custom) return false;
    const std::string n = upperName(c);
    for (const char* k : {"MPU-6050", "MPU6050", "MPU-9250", "ICM-", "BMI0", "BMI1", "BMI2", "LSM6", "BNO0"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}
bool isSwitchingPart(const Component& c) {
    if (c.kind == ComponentKind::NMOS || c.kind == ComponentKind::Inductor) return true;
    if (c.kind != ComponentKind::Custom) return false;
    const std::string n = upperName(c);
    for (const char* k : {"IR2104", "IR2110", "L293", "ULN2003", "DRV8", "IRF", "UC3843", "L298"})
        if (n.find(k) != std::string::npos) return true;
    return false;
}

int mountPlane(const Component& c, const BoardSettings& s) {
    if (auto e = embeddedElement(c, s)) return 100 + e->layer;
    return c.pcb.bottom ? 1 : 0;
}
}  // namespace

namespace {
std::atomic<bool> g_drcBruteForce{false};
}  // namespace
void setDrcBruteForce(bool on) { g_drcBruteForce = on; }
bool drcBruteForce() { return g_drcBruteForce; }
namespace {
std::atomic<int> g_routerStrategy{static_cast<int>(RouterStrategy::Auto)};
std::atomic<int> g_routingThreads{0};
}  // namespace
void setRouterStrategy(RouterStrategy strategy) { g_routerStrategy = static_cast<int>(strategy); }
RouterStrategy routerStrategy() { return static_cast<RouterStrategy>(g_routerStrategy.load()); }
void setRoutingThreads(int threads) { g_routingThreads = std::max(0, threads); }
int routingThreads() { return g_routingThreads.load(); }
int effectiveRoutingThreads() {
    const int n = g_routingThreads.load();
    if (n > 0) return n;
    return std::clamp(static_cast<int>(std::thread::hardware_concurrency()), 1, 8);
}

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
        if (auto e = embeddedElement(c, settings)) {  // formed inside the board: terminations / plates
            for (auto& p : embeddedPads(c, *e, sch)) out.push_back(p);
            continue;
        }
        const FootprintDef* fp = Library::instance().footprint(c.footprintName());
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
    if (auto e = embeddedElement(c, settings)) return e->bounds().inflated(0.6);
    const FootprintDef* fp = Library::instance().footprint(c.footprintName());
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
    // Mounting holes stay inside too, keep-out ring and all.
    for (const auto& h : settings.holes) {
        const double r = std::max(h.keepout, h.drill) / 2;
        box = Rect(std::min(box.x0, h.position.x - r), std::min(box.y0, h.position.y - r), std::max(box.x1, h.position.x + r),
                   std::max(box.y1, h.position.y + r));
    }
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
        if (all && !comps[i].pcb.locked) comps[i].pcb.placed = false;
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
    // Greedy ordering: start with the most connected part, then always the part most connected to the placed set.
    // A part's score is the sum of its weights to the placed parts, added in ascending id order (exactly as a scan
    // over the placed set would), kept per part and updated only for the neighbours of each newly placed part.
    std::vector<size_t> order;
    std::vector<bool> used(comps.size(), false);
    std::set<int> placedIds;
    for (const auto& c : comps)
        if (c.hasFootprint() && c.pcb.placed) placedIds.insert(c.id);
    std::map<int, std::vector<std::pair<int, double>>> neighbours;  // ascending neighbour id
    for (const auto& [pair, weightAB] : weight) {
        neighbours[pair.first].push_back({pair.second, weightAB});
        neighbours[pair.second].push_back({pair.first, weightAB});
    }
    for (auto& [id, list] : neighbours) std::sort(list.begin(), list.end());
    std::map<int, double> placedSum;  // per todo part: Σ weight × 10 over placed neighbours, ascending id
    std::map<int, int> lastPlaced;    // per todo part: largest placed neighbour id in that sum
    auto rescore = [&](int id) {
        double sum = 0;
        int last = std::numeric_limits<int>::min();
        if (auto it = neighbours.find(id); it != neighbours.end())
            for (const auto& [nb, wt] : it->second)
                if (placedIds.count(nb)) {
                    sum += wt * 10.0;
                    last = nb;
                }
        placedSum[id] = sum;
        lastPlaced[id] = last;
    };
    for (size_t idx : todo) rescore(comps[idx].id);
    while (order.size() < todo.size()) {
        size_t best = todo[0];
        double bestScore = -1;
        for (size_t idx : todo) {
            if (used[idx]) continue;
            double s = placedSum[comps[idx].id];
            s += degree[comps[idx].id] * 0.01;
            if (s > bestScore) { bestScore = s; best = idx; }
        }
        used[best] = true;
        order.push_back(best);
        const int bestId = comps[best].id;
        placedIds.insert(bestId);
        if (auto it = neighbours.find(bestId); it != neighbours.end())
            for (const auto& [nb, wt] : it->second) {
                auto sum = placedSum.find(nb);
                if (sum == placedSum.end() || placedIds.count(nb)) continue;
                // Appending the largest id keeps the ascending order; otherwise the sum is redone in order.
                if (bestId > lastPlaced[nb]) {
                    sum->second += wt * 10.0;
                    lastPlaced[nb] = bestId;
                } else {
                    rescore(nb);
                }
            }
    }

    const bool custom = settings.hasCustomOutline();
    const double margin = 1.2;   // spacing between courtyards, leaves routing channels
    // Fine-pitch packages (pad pitch below 0.65 mm) get an escape area: every pin needs room to neck out and turn.
    auto escape = [&](const Component& comp) {
        const FootprintDef* fp = Library::instance().footprint(comp.footprintName());
        if (!fp) return 0.0;
        double pitch = std::numeric_limits<double>::max();
        for (size_t a = 0; a < fp->pads.size(); ++a)
            for (size_t b = a + 1; b < fp->pads.size(); ++b)
                pitch = std::min(pitch, (fp->pads[a].offset - fp->pads[b].offset).length());
        // High-pin-count packages (TQFP-64 and up) need a fan-out ring too, or their neighbours wall the pins in.
        const double fanout = fp->pads.size() >= 44 ? 1.0 : 0.0;
        return std::max(pitch < 0.65 ? 1.5 : 0.0, fanout);
    };
    const double step = 0.5;
    // Isolation barrier: parts of different galvanic domains keep the barrier gap between their courtyards.
    // Mains / high-voltage parts likewise keep their IPC-2221 voltage spacing from low-voltage parts.
    const SpacingDomains spacing = spacingDomains(sch, settings);
    const double isoGap = spacing.gap;
    std::map<int, int> domainOf;
    if (isoGap > 0)
        for (const auto& comp : comps) domainOf[comp.id] = spacing.domains.domainOfComponent(sch, comp);
    auto barrierGap = [&](int a, int b) {
        if (isoGap <= 0) return 0.0;
        const int da = domainOf[a], db = domainOf[b];
        return da >= 0 && db >= 0 && da != db ? isoGap : 0.0;
    };
    std::map<int, double> escapeOf;
    for (const auto& comp : comps)
        if (comp.hasFootprint()) escapeOf[comp.id] = escape(comp);
    // Tamper-meshed secure elements: the mesh area (margin) and its end vias stay free of other parts.
    std::map<int, double> meshKeep;
    for (const auto& tm : tamperMeshes)
        for (const auto& comp : comps)
            if (comp.ref == tm.componentRef && comp.hasFootprint()) {
                const double lead = settings.viaDiameter / 2 + settings.clearance + settings.trackWidth + settings.routingGrid;
                meshKeep[comp.id] = std::max(0.0, tm.margin) + lead + settings.viaDiameter / 2 + 0.5;
                escapeOf[comp.id] = std::max(escapeOf[comp.id], meshKeep[comp.id]);
            }
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
        // Radios and IMUs stay clear of switching power (noise, vibration).
        const bool radio = isRadioPart(c) || isImuPart(c), switching = isSwitchingPart(c);
        const bool mlcc = c.kind == ComponentKind::Capacitor;
        // Everything the candidate scan needs that does not depend on the candidate position is gathered once per
        // part (the scan visits thousands of grid positions): the other parts' keep-out rectangles, the radio /
        // switching neighbours, and each pad's net centroid.
        c.pcb.bottom = false;
        std::vector<Rect> keepOut, noiseNeighbours;
        for (const auto& o : comps) {
            if (o.id == c.id || !o.hasFootprint() || !o.pcb.placed) continue;
            if (mountPlane(o, settings) != mountPlane(c, settings)) continue;
            keepOut.push_back(courtyard(o).inflated(margin / 2 + escapeOf[o.id] + barrierGap(o.id, c.id)));
        }
        if (radio || switching)
            for (const auto& o : comps) {
                if (o.id == c.id || !o.hasFootprint() || !o.pcb.placed) continue;
                if ((radio && isSwitchingPart(o)) || (switching && (isRadioPart(o) || isImuPart(o))))
                    noiseNeighbours.push_back(courtyard(o));
            }
        const FootprintDef* placingFp = Library::instance().footprint(c.footprintName());
        std::vector<std::pair<Vec2, Vec2>> padPulls;  // (pad offset, centroid of its net's placed pads)
        if (placingFp)
            for (const auto& pd : placingFp->pads) {
                if (pd.pinIndex < 0) continue;
                auto it = centroid.find(sch.netOf({c.id, pd.pinIndex}));
                if (it == centroid.end() || it->second.second == 0) continue;
                padPulls.push_back({pd.offset, it->second.first * (1.0 / it->second.second)});
            }
        for (int growth = 0; growth < 20 && !done; ++growth) {
            double bestCost = std::numeric_limits<double>::max();
            Vec2 bestPos;
            int bestRot = 0;
            // First with the spacing rules (radios / switching, MLCCs / holes); without them only if nothing fits.
            for (int strict = 1; strict >= 0 && bestCost == std::numeric_limits<double>::max(); --strict)
            for (int rot : {0, 90}) {
                c.pcb.rotation = rot;
                c.pcb.bottom = false;
                Rect cy0 = courtyard(c);
                double hw = cy0.width() / 2, hh = cy0.height() / 2;
                double e = settings.edgeClearance + 0.5 + (meshKeep.count(c.id) ? meshKeep[c.id] : 0.0);
                // Candidates on the placement grid itself, so the final snap does not move a checked position.
                std::vector<double> xs, ys;
                for (double y = std::ceil((e + hh) / step - 1e-9) * step; y <= settings.height - e - hh + 1e-9; y += step) ys.push_back(y);
                for (double x = std::ceil((e + hw) / step - 1e-9) * step; x <= settings.width - e - hw + 1e-9; x += step) xs.push_back(x);
                // Keep-out clashes for every candidate at once: a candidate's inflated courtyard overlaps a keep-out
                // when both its x and its y extent do, and each extent grows with the position, so every keep-out
                // blocks one block of candidates (marked in a 2-D difference array instead of testing each pair).
                const double inflate = margin / 2 + escapeOf[c.id];
                std::vector<double> cx0(xs.size()), cx1(xs.size()), cyLo(ys.size()), cyHi(ys.size());
                for (size_t ix = 0; ix < xs.size(); ++ix) {
                    const Rect r = Rect::centered({xs[ix], 0}, cy0.width(), cy0.height()).inflated(inflate);
                    cx0[ix] = r.x0;
                    cx1[ix] = r.x1;
                }
                for (size_t iy = 0; iy < ys.size(); ++iy) {
                    const Rect r = Rect::centered({0, ys[iy]}, cy0.width(), cy0.height()).inflated(inflate);
                    cyLo[iy] = r.y0;
                    cyHi[iy] = r.y1;
                }
                const size_t nx = xs.size(), ny = ys.size();
                std::vector<int> blocked((nx + 1) * (ny + 1), 0);
                for (const Rect& k : keepOut) {
                    // x: k.x0 < cx1 && cx0 < k.x1 (both extents non-decreasing in x).
                    const size_t xa = static_cast<size_t>(std::upper_bound(cx1.begin(), cx1.end(), k.x0) - cx1.begin());
                    const size_t xb = static_cast<size_t>(std::lower_bound(cx0.begin(), cx0.end(), k.x1) - cx0.begin());
                    const size_t ya = static_cast<size_t>(std::upper_bound(cyHi.begin(), cyHi.end(), k.y0) - cyHi.begin());
                    const size_t yb = static_cast<size_t>(std::lower_bound(cyLo.begin(), cyLo.end(), k.y1) - cyLo.begin());
                    if (xa >= xb || ya >= yb) continue;
                    blocked[ya * (nx + 1) + xa] += 1;
                    blocked[ya * (nx + 1) + xb] -= 1;
                    blocked[yb * (nx + 1) + xa] -= 1;
                    blocked[yb * (nx + 1) + xb] += 1;
                }
                for (size_t iy = 0; iy <= ny; ++iy)
                    for (size_t ix = 0; ix <= nx; ++ix) {
                        int& b = blocked[iy * (nx + 1) + ix];
                        if (ix > 0) b += blocked[iy * (nx + 1) + ix - 1];
                        if (iy > 0) b += blocked[(iy - 1) * (nx + 1) + ix];
                        if (ix > 0 && iy > 0) b -= blocked[(iy - 1) * (nx + 1) + ix - 1];
                    }
                for (size_t iy = 0; iy < ny; ++iy) {
                    const double y = ys[iy];
                    for (size_t ix = 0; ix < nx; ++ix) {
                        const double x = xs[ix];
                        if (blocked[iy * (nx + 1) + ix] > 0) continue;
                        if (custom && !settings.rectInside(Rect::centered({x, y}, cy0.width(), cy0.height()), e)) continue;
                        if (!custom && !settings.holes.empty() &&
                            !settings.rectInside(Rect::centered({x, y}, cy0.width(), cy0.height()), 0))
                            continue;
                        bool clash = false;
                        if (strict) {
                            const Rect body = Rect::centered({x, y}, cy0.width(), cy0.height());
                            if (mlcc)
                                for (const auto& h : settings.holes)
                                    if (pointRectDistance(h.position, body) < h.keepout / 2 + 2.0) clash = true;
                            if (radio || switching)
                                for (const Rect& ob : noiseNeighbours) {
                                    const double dx = std::max({0.0, ob.x0 - body.x1, body.x0 - ob.x1});
                                    const double dy = std::max({0.0, ob.y0 - body.y1, body.y0 - ob.y1});
                                    if (std::hypot(dx, dy) < 10.0) { clash = true; break; }
                                }
                            if (clash) continue;
                        }
                        // Cost: pad distances to the centroid of their nets, else pull toward board centre.
                        c.pcb.position = {x, y};
                        double cost = 0;
                        const int terms = static_cast<int>(padPulls.size());
                        for (const auto& [offset, cen] : padPulls) {
                            Vec2 pp = transformFootprintPoint(offset, c.pcb);
                            cost += std::fabs(pp.x - cen.x) + std::fabs(pp.y - cen.y);
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

// ===================================================================== spatial index

namespace {
/// Uniform-grid index over item rectangles, for the pairwise copper checks (DRC, connectivity, clean-up): a query
/// returns the ids whose rectangle touches the query rectangle, sorted ascending, so callers visit candidates in the
/// same order as a full scan and produce identical results in O(n log n) instead of O(n²). In brute-force mode
/// (setDrcBruteForce) every id is returned, which is the reference the equivalence test compares against.
class RectIndex {
public:
    explicit RectIndex(std::vector<Rect> boxes) : boxes_(std::move(boxes)), brute_(drcBruteForce()) {
        const size_t n = boxes_.size();
        if (brute_ || n == 0) return;
        Rect all = boxes_[0];
        double sumW = 0, sumH = 0;
        for (const Rect& r : boxes_) {
            all = Rect(std::min(all.x0, r.x0), std::min(all.y0, r.y0), std::max(all.x1, r.x1), std::max(all.y1, r.y1));
            sumW += r.width();
            sumH += r.height();
        }
        origin_ = {all.x0, all.y0};
        // About one item per cell, but no smaller than the typical item (long tracks would fill many cells).
        const double area = std::max(1e-6, all.width() * all.height());
        cell_ = std::max({std::sqrt(area / static_cast<double>(n)), 0.5 * (sumW + sumH) / static_cast<double>(n), 0.05});
        // Never more than about four cells per item (an outlier far away must not blow up the grid).
        while ((all.width() / cell_ + 1) * (all.height() / cell_ + 1) > 4.0 * static_cast<double>(n) + 16) cell_ *= 1.5;
        cols_ = std::max(1, std::min(4096, static_cast<int>(all.width() / cell_) + 1));
        rows_ = std::max(1, std::min(4096, static_cast<int>(all.height() / cell_) + 1));
        start_.assign(static_cast<size_t>(cols_ * rows_) + 1, 0);
        for (size_t id = 0; id < n; ++id) forCells(boxes_[id], [&](size_t c) { ++start_[c + 1]; });
        for (size_t c = 1; c < start_.size(); ++c) start_[c] += start_[c - 1];
        items_.resize(start_.back());
        std::vector<size_t> fill(start_.begin(), start_.end() - 1);
        for (size_t id = 0; id < n; ++id) forCells(boxes_[id], [&](size_t c) { items_[fill[c]++] = id; });
        seen_.assign(n, 0);
    }
    size_t size() const { return boxes_.size(); }
    const Rect& box(size_t id) const { return boxes_[id]; }
    /// Ids (ascending) whose rectangle touches `q` (closed rectangles); with `after`, only ids greater than it.
    const std::vector<size_t>& query(const Rect& q, long long after = -1) const {
        out_.clear();
        const size_t from = static_cast<size_t>(after + 1);
        if (brute_) {
            for (size_t id = from; id < boxes_.size(); ++id) out_.push_back(id);
            return out_;
        }
        if (boxes_.empty()) return out_;
        if (++stamp_ == 0) {
            std::fill(seen_.begin(), seen_.end(), 0u);
            stamp_ = 1;
        }
        forCells(q, [&](size_t c) {
            for (size_t k = start_[c]; k < start_[c + 1]; ++k) {
                const size_t id = items_[k];
                if (id < from || seen_[id] == stamp_) continue;
                seen_[id] = stamp_;
                const Rect& r = boxes_[id];
                if (r.x0 <= q.x1 && q.x0 <= r.x1 && r.y0 <= q.y1 && q.y0 <= r.y1) out_.push_back(id);
            }
        });
        std::sort(out_.begin(), out_.end());
        return out_;
    }

private:
    template <typename F>
    void forCells(const Rect& r, F f) const {
        auto clampCell = [](double v, int count) {
            if (!(v > 0)) return 0;  // also catches NaN
            return v >= count - 1 ? count - 1 : static_cast<int>(v);
        };
        const int i0 = clampCell((r.x0 - origin_.x) / cell_, cols_), i1 = clampCell((r.x1 - origin_.x) / cell_, cols_);
        const int j0 = clampCell((r.y0 - origin_.y) / cell_, rows_), j1 = clampCell((r.y1 - origin_.y) / cell_, rows_);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) f(static_cast<size_t>(j * cols_ + i));
    }
    std::vector<Rect> boxes_;
    bool brute_ = false;
    Vec2 origin_{0, 0};
    double cell_ = 1;
    int cols_ = 1, rows_ = 1;
    std::vector<size_t> start_, items_;
    mutable std::vector<unsigned> seen_;
    mutable unsigned stamp_ = 0;
    mutable std::vector<size_t> out_;
};

Rect segmentBox(Vec2 a, Vec2 b, double half) { return Rect(a.x, a.y, b.x, b.y).inflated(half); }
std::vector<Rect> padBoxes(const std::vector<Pad>& ps) {
    std::vector<Rect> out;
    out.reserve(ps.size());
    for (const auto& p : ps) out.push_back(p.bounds());
    return out;
}
std::vector<Rect> trackBoxes(const std::vector<Track>& ts) {
    std::vector<Rect> out;
    out.reserve(ts.size());
    for (const auto& t : ts) out.push_back(segmentBox(t.a, t.b, t.width / 2));
    return out;
}
std::vector<Rect> viaBoxes(const std::vector<Via>& vs) {
    std::vector<Rect> out;
    out.reserve(vs.size());
    for (const auto& v : vs) out.push_back(Rect::centered(v.position, v.diameter, v.diameter));
    return out;
}
}  // namespace

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

bool viaTouchesTrack(const Via& v, const Track& t) {
    return v.spans(t.layer) && pointSegmentDistance(v.position, t.a, t.b) <= v.diameter / 2;
}

bool viaTouchesPad(const Via& v, const Pad& p) {
    return (p.throughHole || v.spans(p.smdLayer)) && padDistance(p, v.position) <= v.diameter / 2 - 1e-6;
}

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
                if (vias[v].net == f.net && vias[v].spans(f.layer))
                    join(np + nt + v, f.islandNear(vias[v].position, vias[v].diameter / 2));
            base += static_cast<size_t>(f.islands);
        }
    }
    // Contact tests on spatial-index candidates, in the order of a full scan (the same unions in the same order).
    const RectIndex padIx(padBoxes(pads)), trackIx(trackBoxes(tracks)), viaIx(viaBoxes(vias));
    const double slack = 1e-6;
    for (size_t t = 0; t < nt; ++t) {
        const Track& tr = tracks[t];
        const Rect body = segmentBox(tr.a, tr.b, tr.width / 2 + slack);
        for (size_t p : padIx.query(body))
            if (trackTouchesPad(tr, pads[p])) d.unite(np + t, p);
        for (size_t u : trackIx.query(body, static_cast<long long>(t)))
            if (tracksTouch(tr, tracks[u])) d.unite(np + t, np + u);
        for (size_t v : viaIx.query(segmentBox(tr.a, tr.b, slack)))
            if (viaTouchesTrack(vias[v], tr)) d.unite(np + t, np + nt + v);
    }
    for (size_t v = 0; v < nv; ++v)
        for (size_t p : padIx.query(Rect::centered(vias[v].position, vias[v].diameter + slack, vias[v].diameter + slack)))
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
    for (const auto& entry : byNet) {
        const std::vector<size_t>& list = entry.second;  // a plain reference: lambdas below capture it
        if (list.size() < 2) continue;
        if (drcBruteForce()) {  // reference: the pairwise scan the incremental Prim below must reproduce
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
            continue;
        }
        // Prim over clusters: each step adds the shortest line from a connected pad to an unconnected one (ties: the
        // first connected pad in list order, then the first unconnected one). Each unconnected pad keeps its nearest
        // connected pad, so a step costs O(k) instead of O(k²).
        const size_t k = list.size();
        std::vector<size_t> cluster(k);
        for (size_t a = 0; a < k; ++a) cluster[a] = d.find(list[a]);
        std::set<size_t> connectedClusters;
        std::vector<char> connected(k, 0);
        std::vector<double> nearest(k, std::numeric_limits<double>::max());
        std::vector<size_t> nearestFrom(k, k);  // list position of that connected pad
        auto join = [&](size_t root) {
            connectedClusters.insert(root);
            std::vector<size_t> added;
            for (size_t a = 0; a < k; ++a)
                if (!connected[a] && cluster[a] == root) {
                    connected[a] = 1;
                    added.push_back(a);
                }
            for (size_t b = 0; b < k; ++b) {
                if (connected[b]) continue;
                for (size_t a : added) {
                    const double dist = (ps[list[a]].position - ps[list[b]].position).length();
                    if (dist < nearest[b] || (dist == nearest[b] && a < nearestFrom[b])) {
                        nearest[b] = dist;
                        nearestFrom[b] = a;
                    }
                }
            }
        };
        join(cluster[0]);
        std::set<size_t> allClusters(cluster.begin(), cluster.end());
        while (connectedClusters.size() < allClusters.size()) {
            size_t bj = k;
            for (size_t b = 0; b < k; ++b) {
                if (connected[b] || nearestFrom[b] == k) continue;
                if (bj == k || nearest[b] < nearest[bj] || (nearest[b] == nearest[bj] && nearestFrom[b] < nearestFrom[bj])) bj = b;
            }
            if (bj == k) break;  // unreachable: every pad has a distance to the connected set
            lines.push_back({ps[list[nearestFrom[bj]]].position, ps[list[bj]].position});
            join(cluster[bj]);
        }
    }
    return lines;
}

namespace {
/// Pad indices grouped by component (or by net), each group in pad order.
std::unordered_map<int, std::vector<size_t>> groupPads(const std::vector<Pad>& ps, bool byNet) {
    std::unordered_map<int, std::vector<size_t>> out;
    for (size_t i = 0; i < ps.size(); ++i) out[byNet ? ps[i].net : ps[i].componentId].push_back(i);
    return out;
}

/// neckDown with the pads' neck widths and the pads grouped by net computed once (routing calls it per track).
void neckDownWith(std::vector<Track>& out, const std::vector<Pad>& ps, const std::vector<double>& neck,
                  const std::unordered_map<int, std::vector<size_t>>& padsByNet, double clearance) {
    static const std::vector<size_t> kNone;
    for (int iter = 0; iter < 4; ++iter) {
        bool changed = false;
        std::vector<Track> result;
        for (const Track& t : out) {
            bool split = false;
            auto group = padsByNet.find(t.net);
            const std::vector<size_t>& own = group == padsByNet.end() ? kNone : group->second;
            for (size_t gi = 0; gi < own.size() && !split; ++gi) {
                const size_t k = own[gi];
                const Pad& p = ps[k];
                if (p.net != t.net || !p.onLayer(t.layer) || neck[k] >= t.width - 1e-9) continue;
                Rect zone = p.bounds().inflated(clearance + t.width / 2);
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
}  // namespace

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
        {
            // Other-net copper must stay outside the via radius + clearance + half a track (copper_ marks
            // centrelines); the barrel ring beyond half a track must lie in cells the via's net may route through.
            const double rCopper = s.viaDiameter / 2 + s.clearance + s.trackWidth / 2;
            const double rBody = std::max(0.0, s.viaDiameter / 2 - s.trackWidth / 2) + 1e-9;
            const int k = static_cast<int>(std::ceil(rCopper / g_));
            for (int dj = -k; dj <= k; ++dj)
                for (int di = -k; di <= k; ++di) {
                    const double d = std::sqrt(double(di * di + dj * dj)) * g_;
                    if (d <= rCopper) viaDisc_.push_back({di, dj, d <= rBody});
                }
        }
        double e = s.edgeClearance + s.trackWidth / 2;
        for (int j = 0; j < rows_; ++j)
            for (int i = 0; i < cols_; ++i) {
                Vec2 p = pos(i, j);
                if (s.edgeDistance(p) < e || s.holeDistance(p) < s.clearance + s.trackWidth / 2)
                    for (int l = 0; l < layers_; ++l) owner_[L(l)][idx(i, j)] = -2;
            }
    }

    int layers() const { return layers_; }
    const BoardSettings& settings() const { return s_; }
    bool noViaAt(size_t c) const { return noVia_[c] != 0 || layers_ < 2; }
    /// Plane layers are reserved for their net; pour layers cost other nets more so the pour stays whole.
    void setPlane(int l, int net) { if (l >= 0 && l < layers_) planeNet_[L(l)] = net; }
    void setPour(int l, int net) { if (l >= 0 && l < layers_ && pourNet_[L(l)] < 0) pourNet_[L(l)] = net; }
    bool layerOpen(int l, int net) const { return planeNet_[L(l)] < 0 || planeNet_[L(l)] == net; }
    float layerCost(int l, int net) const { return pourNet_[L(l)] >= 0 && pourNet_[L(l)] != net ? 1.6f : 1.0f; }
    /// Escape band around a fine-pitch package: tracks may cross it but not run along it.
    void addEscapeBand(const Rect& body, double width) {
        Rect outer = body.inflated(width);
        forCellsIn(outer, [&](int i, int j) {
            Vec2 p = pos(i, j);
            if (outer.contains(p) && !body.contains(p)) band_[idx(i, j)] = 1;
        });
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
        if (!(o == -1 || o == net || padNet_[L(l)][c] == net)) return false;
        if (isoGap_ > 0 && !fence_.empty()) {  // isolation barrier: other domains' fenced area is closed
            const int f = fence_[static_cast<size_t>(l) * static_cast<size_t>(cols_ * rows_) + c];
            if (f == -3) return padNet_[L(l)][c] == net;
            if (f >= 0) {
                const int d = domainOf(net);
                if (d >= 0 && f != d) return false;
            }
        }
        return true;
    }

    /// Isolation barrier (BoardSettings::isolationGap): copper of one galvanic domain fences the cells within the
    /// gap (plus half a track) against every other domain. `exits` lets each barrier part's pads reach out through
    /// its own other-domain fence: cells within `exitRadius` of a pad of the part stay open to that pad's domain.
    /// `perLayer`: voltage spacing applies within a copper layer (a mains track fences its own layer only); a galvanic
    /// barrier fences every layer (creepage and clearance through the board).
    void setIsolation(std::vector<int> netDomain, double gap, const std::vector<Pad>& pads, double widestHalf,
                      bool perLayer = false) {
        netDomain_ = std::move(netDomain);
        isoGap_ = gap;
        perLayer_ = perLayer;
        // Cells are tested at their centres and tracks run between them (chords), and net-class tracks are wider
        // than the base track: fence by the widest half-width plus a grid pitch.
        fenceReach_ = std::max(widestHalf, s_.trackWidth / 2) + g_;
        fence_.assign(static_cast<size_t>(layers_ * cols_ * rows_), -1);
        if (gap <= 0) return;
        std::map<int, std::vector<const Pad*>> byComp;
        for (const auto& p : pads) byComp[p.componentId].push_back(&p);
        for (const auto& [comp, list] : byComp) {
            double sMin = 1e9;
            for (size_t a = 0; a < list.size(); ++a)
                for (size_t b = a + 1; b < list.size(); ++b) {
                    const int da = domainOf(list[a]->net), db = domainOf(list[b]->net);
                    if (da < 0 || db < 0 || da == db) continue;
                    sMin = std::min(sMin, rectRectDistance(list[a]->bounds(), list[b]->bounds()));
                }
            if (sMin < gap - 1e-9) {  // pins closer than the gap need exit corridors; wider-spaced ones do not
                exitRadius_[comp] = gap - sMin + s_.trackWidth + s_.clearance;
                barrierPads_[comp] = list;
            }
        }
    }
    int domainOf(int net) const {
        return net >= 0 && net < static_cast<int>(netDomain_.size()) ? netDomain_[static_cast<size_t>(net)] : -1;
    }
    /// Fences the cells around copper (a segment, or a pad's rectangle) of `net` against other domains.
    void fenceCopper(Vec2 a, Vec2 b, double half, int net, int layer = -1) {
        const int d = domainOf(net);
        if (isoGap_ <= 0 || d < 0) return;
        forCellsNear(a, b, isoGap_ + half + fenceReach_, [&](size_t c) { fenceCell(layer, c, d); });
    }
    void fencePad(const Pad& p) {
        const int d = domainOf(p.net);
        if (isoGap_ <= 0 || d < 0) return;
        auto exits = barrierPads_.find(p.componentId);
        const double reach = isoGap_ + fenceReach_;
        forRectNear(p.bounds(), reach, p, [&](size_t c, double) {
            if (exits != barrierPads_.end()) {
                const Vec2 at = cellPos(c);
                for (const Pad* q : exits->second)
                    if (domainOf(q->net) != d && padDistance(*q, at) < exitRadius_[p.componentId]) return;
            }
            if (p.throughHole || !perLayer_) fenceCell(-1, c, d);
            else fenceCell(p.smdLayer, c, d);
        });
    }
    /// Keeps the area `r` to domain `d` only (e.g. a radio module and its antenna kept clear of mains nets).
    void fenceArea(const Rect& r, int d) {
        if (isoGap_ <= 0 || d < 0) return;
        forCellsIn(r, [&](int i, int j) {
            if (r.contains(pos(i, j))) fenceCell(-1, idx(i, j), d);
        });
    }
    /// Exact clearance test for a wide (net-class) track segment a–b of half-width `half` on layer l: the grid only
    /// guarantees clearance for the base track width, so wide tracks are checked against the real copper.
    bool wideClear(int l, Vec2 a, Vec2 b, int net, double half) const {
        const double need = s_.clearance + half - 1e-9;
        for (Vec2 p : {a, b})
            if (s_.edgeDistance(p) < s_.edgeClearance + half - 1e-9 || s_.holeDistance(p) < half) return false;
        Rect box = Rect(a.x, a.y, b.x, b.y).inflated(need + maxHalf_);
        bool clash = false;
        forBuckets(box, 0, padBuckets_, [&](size_t id) {
            const Pad& p = pads_[id];
            if (clash || (p.net == net && net >= 0) || !p.onLayer(l)) return;
            double d = p.round ? std::max(0.0, pointSegmentDistance(p.position, a, b) - std::min(p.size.x, p.size.y) / 2)
                               : segmentRectDistance(a, b, p.bounds());
            if (d < need) clash = true;
        });
        if (clash) return false;
        forBuckets(box, 1, buckets_, [&](size_t id) {
            const Copper& c = copperItems_[id];
            if (clash || c.net == net || (c.layer >= 0 && c.layer != l)) return;
            if (segmentSegmentDistance(a, b, c.a, c.b) - c.half < need) clash = true;
        });
        return !clash;
    }

    bool viaAllowed(size_t c, int net) const {
        int ci = static_cast<int>(c % static_cast<size_t>(cols_)), cj = static_cast<int>(c / static_cast<size_t>(cols_));
        if (layers_ < 2) return false;  // single-sided boards have no vias
        if (noVia_[c]) return false;    // never inside an SMD pad (DRC_VIA_IN_PAD)
        // The disc around the via (viaDisc_): other-net copper must stay outside it, and its inner part (the barrel)
        // must lie in cells this net may route through.
        for (const ViaDiscCell& o : viaDisc_)
            if (!inside(ci + o.di, cj + o.dj)) return false;
        for (int l = 0; l < layers_; ++l) {  // through via: every layer must allow it
            if (!passable(l, c, net)) return false;
            const std::vector<int>& copper = copper_[L(l)];
            for (const ViaDiscCell& o : viaDisc_) {
                const size_t cc = idx(ci + o.di, cj + o.dj);
                const int cu = copper[cc];
                if (cu != -1 && cu != net) return false;
                if (o.body && !passable(l, cc, net)) return false;
            }
        }
        // Pad copper is sampled on the grid, so a pad edge can sit up to half a cell diagonal closer than the cells
        // suggest: check other-net pads exactly, and hole-to-hole spacing to drilled pads.
        Vec2 at = pos(ci, cj);
        double need = s_.viaDiameter / 2 + s_.clearance - 1e-9;
        // Only pads within the largest reach can matter (the bucket index holds every pad by its rectangle).
        bool padClash = false;
        const double q = std::max(need, maxPadReach_) + 1e-6;
        forBuckets(Rect(at.x - q, at.y - q, at.x + q, at.y + q), 0, padBuckets_, [&](size_t id) {
            if (padClash) return;
            const Pad& p = pads_[id];
            double holeNeed = p.throughHole && p.drill > 0 ? (p.drill + s_.viaDrill) / 2 + s_.minHoleToHole : 0.0;
            double reach = std::max(need, holeNeed - std::min(p.size.x, p.size.y) / 2);
            Rect b = p.bounds();
            if (at.x < b.x0 - reach || at.x > b.x1 + reach || at.y < b.y0 - reach || at.y > b.y1 + reach) return;
            if (holeNeed > 0 && (at - p.position).length() < holeNeed) padClash = true;
            else if (!(p.net == net && net >= 0) && padDistance(p, at) < need) padClash = true;
        });
        if (padClash) return false;
        // Tracks and vias are also tracked exactly (the grid only records their centrelines).
        bool clash = false;
        forNearbyCopper(at, need + maxHalf_ + s_.viaDrill + s_.minHoleToHole, [&](const Copper& c) {
            if (c.drill > 0 && (at - c.a).length() < (c.drill + s_.viaDrill) / 2 + s_.minHoleToHole - 1e-9) clash = true;
            if (c.net != net && pointSegmentDistance(at, c.a, c.b) < need + c.half) clash = true;
        });
        return !clash;
    }

    /// Corridor router, rip-up: copper recorded so far is fixed (pads, meshes, fan-outs); later copper is routed
    /// and may be ripped up.
    void markFixedEnd() { fixedCopper_ = copperItems_.size(); }
    /// Nets whose routed copper the rip-up may remove (signal nets); the copper of every other net (pour and plane
    /// nets' connections) is as fixed as fan-outs.
    void setRippable(std::vector<char> nets) { rippable_ = std::move(nets); }
    /// Whether copper item `id` (copperItems_) stays: laid before markFixedEnd, or of a net that is not rippable.
    bool fixedItem(size_t id) const {
        if (id < fixedCopper_) return true;
        const int net = copperItems_[id].net;
        return net < 0 || static_cast<size_t>(net) >= rippable_.size() || !rippable_[static_cast<size_t>(net)];
    }
    /// A cell `net` cannot use even with every routed (rippable) track removed: a plane of another net, the board edge
    /// or a hole, another net's pad, or fixed copper of another net within a base track's keep-out.
    bool hardBlocked(int l, size_t c, int net) const {
        if (!layerOpen(l, net)) return true;
        if (passable(l, c, net)) return false;
        const Vec2 at = cellPos(c);
        const double e = s_.edgeClearance + s_.trackWidth / 2;
        if (s_.edgeDistance(at) < e || s_.holeDistance(at) < s_.clearance + s_.trackWidth / 2) return true;
        const double need = s_.clearance + s_.trackWidth / 2 - 1e-9;
        bool hard = false;
        forBuckets(Rect::centered(at, 2 * need, 2 * need), 0, padBuckets_, [&](size_t id) {
            const Pad& p = pads_[id];
            if (!hard && !(p.net == net && net >= 0) && p.onLayer(l) && padDistance(p, at) < need) hard = true;
        });
        if (hard) return true;
        forNearbyCopper(at, need + maxHalf_, [&](const Copper& cu) {
            if (hard || cu.net == net || (cu.layer >= 0 && cu.layer != l)) return;
            if (fixedItem(static_cast<size_t>(&cu - copperItems_.data())) && pointSegmentDistance(at, cu.a, cu.b) < need + cu.half) hard = true;
        });
        return hard;
    }
    /// Whether a via of `net` could stand at cell c once every routed (rippable) track and via is removed: only the
    /// board edge, holes, pads and fixed copper count (the exact checks of viaAllowed, without the routed copper).
    bool viaAllowedFixedOnly(size_t c, int net) const {
        if (layers_ < 2 || noVia_[c]) return false;
        const int ci = static_cast<int>(c % static_cast<size_t>(cols_)), cj = static_cast<int>(c / static_cast<size_t>(cols_));
        for (const ViaDiscCell& o : viaDisc_)
            if (!inside(ci + o.di, cj + o.dj)) return false;
        const Vec2 at = pos(ci, cj);
        if (s_.edgeDistance(at) < s_.edgeClearance + s_.viaDiameter / 2 || s_.holeDistance(at) < s_.clearance + s_.viaDiameter / 2)
            return false;
        const double need = s_.viaDiameter / 2 + s_.clearance - 1e-9;
        bool clash = false;
        const double q = std::max(need, maxPadReach_) + 1e-6;
        forBuckets(Rect(at.x - q, at.y - q, at.x + q, at.y + q), 0, padBuckets_, [&](size_t id) {
            if (clash) return;
            const Pad& p = pads_[id];
            const double holeNeed = p.throughHole && p.drill > 0 ? (p.drill + s_.viaDrill) / 2 + s_.minHoleToHole : 0.0;
            if (holeNeed > 0 && (at - p.position).length() < holeNeed) clash = true;
            else if (!(p.net == net && net >= 0) && padDistance(p, at) < need) clash = true;
        });
        if (clash) return false;
        forNearbyCopper(at, need + maxHalf_ + s_.viaDrill + s_.minHoleToHole, [&](const Copper& cu) {
            if (clash || !fixedItem(static_cast<size_t>(&cu - copperItems_.data()))) return;
            if (cu.drill > 0 && (at - cu.a).length() < (cu.drill + s_.viaDrill) / 2 + s_.minHoleToHole - 1e-9) clash = true;
            if (cu.net != net && pointSegmentDistance(at, cu.a, cu.b) < need + cu.half) clash = true;
        });
        return !clash;
    }
    /// Nets other than `net` with routed (non-fixed) copper on layer l within `margin` (plus the copper's half
    /// width) of cell c.
    void routedNetsNear(int l, size_t c, int net, double margin, std::vector<int>& out) const {
        const Vec2 at = cellPos(c);
        forNearbyCopper(at, margin + maxHalf_, [&](const Copper& cu) {
            if (cu.net == net || cu.net < 0 || (cu.layer >= 0 && cu.layer != l)) return;
            if (fixedItem(static_cast<size_t>(&cu - copperItems_.data()))) return;
            if (pointSegmentDistance(at, cu.a, cu.b) < margin + cu.half) out.push_back(cu.net);
        });
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
        if (p.throughHole && p.drill > 0)
            maxPadReach_ = std::max(maxPadReach_, (p.drill + s_.viaDrill) / 2 + s_.minHoleToHole - std::min(p.size.x, p.size.y) / 2);
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
    /// Visits each item of `buckets` whose bucket range meets `box` once. The de-duplication stamps are per thread
    /// (`which` picks the pad or the copper set), so const queries are safe from several routing threads at once.
    template <typename F>
    void forBuckets(const Rect& box, int which, const std::vector<std::vector<size_t>>& buckets, F f) const {
        static thread_local unsigned stamps[2] = {0, 0};
        static thread_local std::vector<unsigned> seens[2];
        unsigned& stamp = stamps[which];
        std::vector<unsigned>& seen = seens[which];
        int x0 = bucketOf(box.x0, bCols_), x1 = bucketOf(box.x1, bCols_);
        int y0 = bucketOf(box.y0, bRows_), y1 = bucketOf(box.y1, bRows_);
        if (++stamp == 0) {  // wrapped: forget every old stamp
            std::fill(seen.begin(), seen.end(), 0u);
            stamp = 1;
        }
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
        forBuckets(Rect::centered(at, 2 * radius, 2 * radius), 1, buckets_,
                   [&](size_t id) { f(copperItems_[id]); });
    }
    static size_t L(int l) { return static_cast<size_t>(l); }
    /// Visits the cells whose centres may lie in `r` (a cell of margin on each side; callers test exactly), in the
    /// row-major order of a full scan.
    template <typename F>
    void forCellsIn(const Rect& r, F f) const {
        const int i0 = std::max(0, static_cast<int>(std::floor(r.x0 / g_)) - 1);
        const int i1 = std::min(cols_ - 1, static_cast<int>(std::ceil(r.x1 / g_)) + 1);
        const int j0 = std::max(0, static_cast<int>(std::floor(r.y0 / g_)) - 1);
        const int j1 = std::min(rows_ - 1, static_cast<int>(std::ceil(r.y1 / g_)) + 1);
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) f(i, j);
    }
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

    /// Fences cell `c` on `layer` (every layer when layer < 0 or the fence is not per layer).
    void fenceCell(int layer, size_t c, int d) {
        const size_t n = static_cast<size_t>(cols_ * rows_);
        for (int l = 0; l < layers_; ++l) {
            if (perLayer_ && layer >= 0 && l != layer) continue;
            int& f = fence_[static_cast<size_t>(l) * n + c];
            if (f == -1) f = d;
            else if (f != d) f = -3;
        }
    }
    bool perLayer_ = false;
    std::vector<int> fence_, netDomain_;
    double isoGap_ = 0, fenceReach_ = 0;
    std::map<int, double> exitRadius_;
    std::map<int, std::vector<const Pad*>> barrierPads_;
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
    size_t fixedCopper_ = 0;  // copperItems_ before this index are fixed (markFixedEnd)
    std::vector<char> rippable_;  // per net: routed copper may be ripped up (setRippable)
    double maxPadReach_ = 0;  // largest hole-to-hole reach beyond a drilled pad's rectangle (viaAllowed)
    struct ViaDiscCell {
        int di, dj;
        bool body;  // within the via barrel (beyond half a track): must be routable for the via's net
    };
    std::vector<ViaDiscCell> viaDisc_;  // cell offsets within a via's copper keep-out (viaAllowed)
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

struct Congestion;

/// Reusable search state for astar over one routing grid. The arrays are sized once per grid and invalidated by a
/// generation stamp instead of being cleared, so a connection on a large board costs only the cells it visits (the
/// old per-call allocation of cost / parent / target arrays over the whole grid dominated routing time).
///
/// Corridor mode (large boards): a search may visit only the cells of the coarse tiles given to setCorridor, and
/// storage exists for those cells only. Nodes keep their grid-wide numbers (heap order and tie-breaking are those of
/// a whole-grid search restricted to the corridor); only where their state is stored changes.
class AStarWorkspace {
public:
    explicit AStarWorkspace(size_t total, size_t cells)
        : cells_(cells), cost_(total), parent_(total), stamp_(total, 0), target_(cells, 0), targetLayers_(cells, 0),
          neck_(cells, 0.0f), neckStamp_(cells, 0), via_(cells, 0), viaStamp_(cells, 0) {}
    AStarWorkspace(int cols, int rows, int layers, const routing::TileGrid& tiles)
        : cells_(static_cast<size_t>(cols) * static_cast<size_t>(rows)), corridor_(true), cols_(cols), rows_(rows),
          layers_(static_cast<size_t>(layers)), tiles_(tiles), tileSlot_(static_cast<size_t>(tiles.count()), -1) {
        const int T = tiles_.tile;
        for (int i = 0; i < cols_; ++i) {
            colTile_.push_back(i / T);
            colOff_.push_back(i % T);
        }
        for (int j = 0; j < rows_; ++j) {
            rowTile_.push_back(j / T * tiles_.cols);
            rowOff_.push_back(j % T * T);
        }
    }
    bool corridorMode() const { return corridor_; }
    /// Restricts the following searches to these tiles (corridor mode only). Call before beginTargets.
    void setCorridor(const std::vector<int>& tiles) {
        for (int t : members_) tileSlot_[static_cast<size_t>(t)] = -1;
        members_.clear();
        for (int t : tiles) {
            if (t < 0 || t >= tiles_.count() || tileSlot_[static_cast<size_t>(t)] >= 0) continue;
            tileSlot_[static_cast<size_t>(t)] = static_cast<int>(members_.size());
            members_.push_back(t);
        }
        const size_t nl = members_.size() * static_cast<size_t>(tiles_.tile) * static_cast<size_t>(tiles_.tile);
        if (cost_.size() < nl * layers_) {
            cost_.resize(nl * layers_);
            parent_.resize(nl * layers_);
            stamp_.resize(nl * layers_, 0);
        }
        if (target_.size() < nl) {
            target_.resize(nl, 0);
            targetLayers_.resize(nl, 0);
            neck_.resize(nl, 0.0f);
            neckLayers_.resize(nl, 0);
            neckStamp_.resize(nl, 0);
            via_.resize(nl, 0);
            viaStamp_.resize(nl, 0);
        }
    }
    bool allowed(size_t c) const { return !corridor_ || slotOf(c) >= 0; }
    /// Target cells of the next search (cleared by beginTargets): per grid cell, a mask of its target layers.
    void beginTargets() { bump(targetGen_, target_); }
    void addTarget(size_t node) {
        const size_t c = node % cells_;
        if (!allowed(c)) return;  // outside the corridor: unreachable anyway
        const size_t k = ci(c);
        if (target_[k] != targetGen_) {
            target_[k] = targetGen_;
            targetLayers_[k] = 0;
        }
        targetLayers_[k] |= 1u << (node / cells_);
    }
    bool isTarget(size_t node) const {
        const size_t c = node % cells_;
        if (!allowed(c)) return false;
        const size_t k = ci(c);
        return target_[k] == targetGen_ && (targetLayers_[k] >> (node / cells_) & 1u);
    }
    /// Neck-down half-widths per cell (0 outside every neck zone), cleared by beginNeck.
    void beginNeck() {
        bump(neckGen_, neckStamp_);
        hasNeck_ = false;
    }
    float neck(size_t c) const {
        if (!allowed(c)) return 0.0f;
        const size_t k = ci(c);
        return neckStamp_[k] == neckGen_ ? neck_[k] : 0.0f;
    }
    /// The neck half-width for a track on `layer`. Neck-down narrows only tracks on the pad's own layers, so in
    /// corridor mode the zone applies on those layers only (the classic router applies it on every layer, as before).
    float neck(size_t c, int layer) const {
        if (!allowed(c)) return 0.0f;
        const size_t k = ci(c);
        if (neckStamp_[k] != neckGen_) return 0.0f;
        return !corridor_ || layer >= 32 || (neckLayers_[k] >> layer & 1u) ? neck_[k] : 0.0f;
    }
    void setNeck(size_t c, float h, uint32_t layers = ~0u) {
        if (!allowed(c)) return;
        const size_t k = ci(c);
        if (corridor_) neckLayers_[k] = neckStamp_[k] == neckGen_ ? neckLayers_[k] | layers : layers;
        neck_[k] = h;
        neckStamp_[k] = neckGen_;
        hasNeck_ = true;
    }
    bool hasNeck() const { return hasNeck_; }
    /// Vias the net being routed has placed but not yet committed to the grid (parallel routing): a new via keeps
    /// hole-to-hole spacing to them, as it would to committed ones (viaAllowed).
    void clearOwnVias() { ownVias_.clear(); }
    void addOwnVia(Vec2 at) { ownVias_.push_back(at); }
    bool ownViasAllow(Vec2 at, const BoardSettings& s) const {
        for (const Vec2& v : ownVias_)
            if ((at - v).length() < (s.viaDrill + s.viaDrill) / 2 + s.minHoleToHole - 1e-9) return false;
        return true;
    }

    /// Raw search state (astar and the corridor router's blocker search).
    void beginSearch() {
        bump(gen_, stamp_);
        bump(viaGen_, viaStamp_);
        open_.clear();
    }
    float cost(size_t s) const {
        const size_t k = ni(s);
        return stamp_[k] == gen_ ? cost_[k] : std::numeric_limits<float>::infinity();
    }
    int parent(size_t s) const {
        const size_t k = ni(s);
        return stamp_[k] == gen_ ? parent_[k] : -1;
    }
    void set(size_t s, float c, int p) {
        const size_t k = ni(s);
        cost_[k] = c;
        parent_[k] = p;
        stamp_[k] = gen_;
    }

private:
    friend RouteResult astar(const RoutingGrid&, int, const std::vector<std::pair<int, size_t>>&, AStarWorkspace&, Vec2,
                             double, double, bool, const std::function<bool(int, size_t)>*, const Congestion*);
    template <typename T>
    static void bump(uint32_t& gen, std::vector<T>& stamps) {
        if (++gen == 0) {  // wrapped: forget every old stamp
            std::fill(stamps.begin(), stamps.end(), T{0});
            gen = 1;
        }
    }
    /// Corridor mode: the slot of grid cell c's tile (-1 outside the corridor), and the cell's storage index.
    int slotOf(size_t c) const {
        const size_t j = c / static_cast<size_t>(cols_), i = c - j * static_cast<size_t>(cols_);
        return tileSlot_[static_cast<size_t>(rowTile_[j] + colTile_[i])];
    }
    size_t local(size_t c) const {
        const size_t j = c / static_cast<size_t>(cols_), i = c - j * static_cast<size_t>(cols_);
        const size_t T = static_cast<size_t>(tiles_.tile);
        return static_cast<size_t>(tileSlot_[static_cast<size_t>(rowTile_[j] + colTile_[i])]) * T * T +
               static_cast<size_t>(rowOff_[j] + colOff_[i]);
    }
    /// Storage index of grid cell c / grid node s (identity in dense mode).
    size_t ci(size_t c) const { return corridor_ ? local(c) : c; }
    size_t ni(size_t s) const { return corridor_ ? local(s % cells_) * layers_ + s / cells_ : s; }
    int viaCached(size_t c) const {
        const size_t k = ci(c);
        return viaStamp_[k] == viaGen_ ? via_[k] : -1;
    }
    void setVia(size_t c, int v) {
        const size_t k = ci(c);
        via_[k] = static_cast<char>(v);
        viaStamp_[k] = viaGen_;
    }
    size_t cells_;
    bool corridor_ = false;
    int cols_ = 0, rows_ = 0;
    size_t layers_ = 1;
    routing::TileGrid tiles_;
    std::vector<int> tileSlot_;    // corridor mode: storage slot per tile, -1 outside the corridor
    std::vector<int> members_;     // corridor mode: the corridor's tiles
    std::vector<int> colTile_, colOff_, rowTile_, rowOff_;  // cell column / row → tile and offset in it
    std::vector<float> cost_;
    std::vector<int> parent_;
    std::vector<uint32_t> stamp_, target_, targetLayers_;
    std::vector<float> neck_;
    std::vector<uint32_t> neckStamp_, neckLayers_;  // neckLayers_: corridor mode, the pads' layers per cell
    std::vector<char> via_;  // viaAllowed per cell for the current search (net and grid are fixed during a search)
    std::vector<uint32_t> viaStamp_;
    std::vector<std::pair<float, int>> open_;
    std::vector<Vec2> ownVias_;
    uint32_t gen_ = 0, targetGen_ = 0, neckGen_ = 0, viaGen_ = 0;
    bool hasNeck_ = false;
};

/// Negotiated-congestion history (PathFinder style) for the recovery passes: per routing node, the extra cost of
/// stepping onto it, and the one net that wanted it (that net does not pay; -1: several did, everyone pays).
///
/// Dense (cost / net per node: the classic router's recovery) or paged (the corridor router's rip-up on large boards:
/// storage only for the coarse tiles that have history, so the memory follows the congested area, not the board).
struct Congestion {
    std::vector<float> cost;
    std::vector<int> net;
    float at(size_t node, int forNet) const {
        if (pagedCells_ == 0) return net[node] == forNet ? 0.0f : cost[node];
        const size_t c = node % pagedCells_;
        const size_t page = static_cast<size_t>(tiles_.ofCell(static_cast<int>(c % cols_), static_cast<int>(c / cols_)));
        const Page* pg = pages_[page].get();
        if (!pg) return 0.0f;
        const size_t k = slot(c, node / pagedCells_);
        return pg->net[k] == forNet ? 0.0f : pg->cost[k];
    }
    /// Paged mode over a grid of cols × rows cells and `layers` layers.
    void page(const routing::TileGrid& tiles, int cols, int rows, int layers) {
        tiles_ = tiles;
        cols_ = static_cast<size_t>(cols);
        pagedCells_ = static_cast<size_t>(cols) * static_cast<size_t>(rows);
        layers_ = static_cast<size_t>(layers);
        pages_.clear();
        pages_.resize(static_cast<size_t>(tiles.count()));
    }
    /// Paged mode: adds `amount` to node's cost; `forNet` wanted it (exempt), -1 once several nets did.
    void add(size_t node, float amount, int forNet) {
        const size_t c = node % pagedCells_;
        auto& pg = pages_[static_cast<size_t>(tiles_.ofCell(static_cast<int>(c % cols_), static_cast<int>(c / cols_)))];
        if (!pg) {
            pg = std::make_unique<Page>();
            const size_t n = static_cast<size_t>(tiles_.tile) * static_cast<size_t>(tiles_.tile) * layers_;
            pg->cost.assign(n, 0.0f);
            pg->net.assign(n, -2);
        }
        const size_t k = slot(c, node / pagedCells_);
        pg->cost[k] += amount;
        int& owner = pg->net[k];
        owner = owner == -2 || owner == forNet ? forNet : -1;
    }

private:
    struct Page {
        std::vector<float> cost;
        std::vector<int> net;
    };
    size_t slot(size_t c, size_t layer) const {
        const size_t T = static_cast<size_t>(tiles_.tile);
        return ((c / cols_) % T * T + (c % cols_) % T) * layers_ + layer;
    }
    routing::TileGrid tiles_;
    size_t cols_ = 1, pagedCells_ = 0, layers_ = 1;
    std::vector<std::unique_ptr<Page>> pages_;
};

/// Grids of at least this many routing nodes (cells × layers) are "large": searches there get a budget (astar).
/// Every board of the regression suite and the reference designs is below it (the largest is under 1 M).
constexpr size_t kLargeGrid = 2000000;
constexpr size_t kLargeGridMinBudget = 1u << 20;  // a large grid's search budget is never below this

/// A* from `sources` to the workspace's target cells (or, with `lazyTarget`, to the cells that predicate accepts:
/// tested only for the cells the search reaches). `useNeck`: the workspace's neck zones apply (fine-pitch pads of
/// this net, see neckDown). `congestion`: recovery passes' history costs.
RouteResult astar(const RoutingGrid& g, int net, const std::vector<std::pair<int, size_t>>& sources, AStarWorkspace& ws,
                  Vec2 targetCentre, double viaCost, double wideHalf, bool useNeck,
                  const std::function<bool(int, size_t)>* lazyTarget = nullptr, const Congestion* congestion = nullptr) {
    const int cols = g.cols(), rows = g.rows();
    const size_t n = static_cast<size_t>(cols * rows);
    const int layers = g.layers();
    const size_t total = static_cast<size_t>(layers) * n;
    ws.beginSearch();
    using QE = std::pair<float, int>;
    // The same binary heap std::priority_queue<QE, std::vector<QE>, std::greater<QE>> keeps (identical pop order),
    // on a buffer reused across searches.
    std::vector<QE>& open = ws.open_;
    const std::greater<QE> later;
    auto push = [&](float f, int s) {
        open.push_back({f, s});
        std::push_heap(open.begin(), open.end(), later);
    };
    const bool neckOn = useNeck && ws.hasNeck();
    const bool corridor = ws.corridorMode();
    auto neckAt = [&](size_t c, int layer) { return neckOn ? ws.neck(c, layer) : 0.0f; };
    const double tx = targetCentre.x / g.pitch(), ty = targetCentre.y / g.pitch();
    auto h = [&](int i, int j) {
        double dx = std::fabs(i - tx), dy = std::fabs(j - ty);
        return static_cast<float>(std::max(dx, dy) + 0.414 * std::min(dx, dy));
    };
    for (auto [l, c] : sources) {
        if (!ws.allowed(c)) continue;  // corridor mode: a source outside the corridor cannot be stored
        int s = static_cast<int>(static_cast<size_t>(l) * n + c);
        if (ws.cost(static_cast<size_t>(s)) == 0) continue;
        ws.set(static_cast<size_t>(s), 0, -1);
        int i = static_cast<int>(c % static_cast<size_t>(cols)), j = static_cast<int>(c / static_cast<size_t>(cols));
        push(h(i, j), s);
    }
    static const int di[8] = {1, -1, 0, 0, 1, 1, -1, -1};
    static const int dj[8] = {0, 0, 1, -1, 1, -1, 1, -1};
    int found = -1;
    size_t expanded = 0;
    // Search budget. On a large grid a connection that cannot be routed would otherwise flood the whole grid (tens of
    // millions of nodes) before failing. Routable connections need far less: at most 3.7 % of the grid on the
    // benchmark boards (542 k of 14.8 M nodes), so a search stops after an eighth of it.
    const size_t budget = total >= kLargeGrid ? std::max(kLargeGridMinBudget, total / 8) : 4 * total;
    while (!open.empty()) {
        std::pop_heap(open.begin(), open.end(), later);
        auto [f, s] = open.back();
        open.pop_back();
        int l = s / static_cast<int>(n);
        size_t c = static_cast<size_t>(s) % n;
        int i = static_cast<int>(c % static_cast<size_t>(cols)), j = static_cast<int>(c / static_cast<size_t>(cols));
        float gc = ws.cost(static_cast<size_t>(s));
        if (f - h(i, j) > gc + 1e-3f) continue;  // stale entry
        if (lazyTarget ? (*lazyTarget)(l, c) : ws.isTarget(static_cast<size_t>(s))) { found = s; break; }
        if (++expanded > budget) break;
        const bool lateral = g.layerOpen(l, net);  // on another net's plane only a via may pass
        // Incoming direction (approximate turn penalty keeps tracks straight and avoids zig-zags).
        int inDi = 0, inDj = 0;
        int ps = ws.parent(static_cast<size_t>(s));
        if (ps >= 0 && ps / static_cast<int>(n) == l) {
            size_t pc = static_cast<size_t>(ps) % n;
            inDi = i - static_cast<int>(pc % static_cast<size_t>(cols));
            inDj = j - static_cast<int>(pc / static_cast<size_t>(cols));
        }
        for (int k = 0; k < 8 && lateral; ++k) {
            int ni = i + di[k], nj = j + dj[k];
            if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
            // Corridor mode: no turn sharper than 90° (an acute join is an acid trap; the classic router keeps its
            // historical moves).
            if (corridor && inDi * di[k] + inDj * dj[k] < 0) continue;
            size_t nc = g.idx(ni, nj);
            if (!ws.allowed(nc)) continue;
            // Next to its own fine-pitch pads the track is necked down (neckDown), so there the necked copper is
            // checked exactly against its neighbours instead of the base-width grid keep-outs.
            const float nh = neckAt(nc, l);
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
            if (congestion) step += congestion->at(static_cast<size_t>(ns), net);
            float nc2 = gc + step;
            if (nc2 < ws.cost(static_cast<size_t>(ns))) {
                ws.set(static_cast<size_t>(ns), nc2, s);
                push(nc2 + h(ni, nj), ns);
            }
        }
        // Through via to any other layer (checked lazily: viaAllowed is the expensive test, and its answer for a cell
        // holds for the whole search).
        int viaState = -1;  // -1 unknown, 0 no, 1 yes
        // Corridor mode: a via must also keep hole-to-hole spacing to the previous via of this very path (the classic
        // router keeps its historical behaviour, where such a pair could only come from an unusual detour).
        if (ws.corridorMode() && layers > 1) {
            const BoardSettings& bs = g.settings();
            const double spacing = bs.viaDrill + bs.minHoleToHole - 1e-9;
            const int back = static_cast<int>(std::ceil(spacing / g.pitch())) + 1;
            int q = s;
            for (int step = 0; step <= back && q >= 0; ++step) {
                const int pq = ws.parent(static_cast<size_t>(q));
                if (pq < 0) break;
                if (pq / static_cast<int>(n) != q / static_cast<int>(n)) {  // a via at q's cell
                    const size_t vc = static_cast<size_t>(q) % n;
                    if ((g.cellPos(vc) - g.pos(i, j)).length() < spacing && vc != c) viaState = 0;
                    break;
                }
                q = pq;
            }
        }
        for (int ol = 0; ol < layers && viaState != 0; ++ol) {
            if (ol == l || !g.layerOpen(ol, net)) continue;
            int os = static_cast<int>(static_cast<size_t>(ol) * n + c);
            float nc2 = gc + static_cast<float>(viaCost) + 0.5f * static_cast<float>(std::abs(ol - l) - 1);
            if (congestion) nc2 += congestion->at(static_cast<size_t>(os), net);
            if (nc2 >= ws.cost(static_cast<size_t>(os)) || !g.passable(ol, c, net)) continue;
            // A wide track continues from the via on the other layer: its body must fit there too.
            if (wideHalf > 0 && !(neckAt(c, ol) > 0) && !g.wideClear(ol, g.pos(i, j), g.pos(i, j), net, wideHalf))
                continue;
            if (viaState < 0) {
                viaState = ws.viaCached(c);
                if (viaState < 0) {
                    viaState = g.viaAllowed(c, net) && ws.ownViasAllow(g.pos(i, j), g.settings()) ? 1 : 0;
                    ws.setVia(c, viaState);
                }
            }
            if (viaState == 0) break;
            ws.set(static_cast<size_t>(os), nc2, s);
            push(nc2 + h(i, j), os);
        }
    }
    RouteResult r;
    if (found < 0) return r;
    for (int s = found; s >= 0; s = ws.parent(static_cast<size_t>(s))) {
        int l = s / static_cast<int>(n);
        size_t c = static_cast<size_t>(s) % n;
        r.path.push_back({l, static_cast<int>(c % static_cast<size_t>(cols)), static_cast<int>(c / static_cast<size_t>(cols))});
    }
    std::reverse(r.path.begin(), r.path.end());
    r.ok = true;
    return r;
}

/// The next pad a net connects: the unconnected pad nearest to a connected one (ties: the first connected pad in list
/// order, then the first unconnected one; with nothing connected, the first unconnected pad). The same choice as a
/// scan over every pair, kept incrementally: O(k) per connected pad instead of O(k²) per step.
class NearestPad {
public:
    NearestPad(const std::vector<Pad>& ps, const std::vector<size_t>& list)
        : ps_(ps), list_(list), connected_(list.size(), false), dist_(list.size(), std::numeric_limits<double>::max()),
          from_(list.size(), kNone) {}
    void connect(size_t a) {
        if (connected_[a]) return;
        connected_[a] = true;
        for (size_t b = 0; b < list_.size(); ++b) {
            if (connected_[b]) continue;
            const double d = (ps_[list_[a]].position - ps_[list_[b]].position).length();
            if (d < dist_[b] || (d == dist_[b] && a < from_[b])) {
                dist_[b] = d;
                from_[b] = a;
            }
        }
    }
    size_t next() const {
        size_t best = kNone, firstOpen = kNone;
        for (size_t b = 0; b < list_.size(); ++b) {
            if (connected_[b]) continue;
            if (firstOpen == kNone) firstOpen = b;
            if (from_[b] == kNone) continue;
            if (best == kNone || dist_[b] < dist_[best] || (dist_[b] == dist_[best] && from_[b] < from_[best])) best = b;
        }
        return best == kNone ? (firstOpen == kNone ? 0 : firstOpen) : best;
    }
    /// The connected pad nearest to pad b (list index), or SIZE_MAX when none is connected.
    size_t from(size_t b) const { return from_[b]; }

private:
    static constexpr size_t kNone = std::numeric_limits<size_t>::max();
    const std::vector<Pad>& ps_;
    const std::vector<size_t>& list_;
    std::vector<bool> connected_;
    std::vector<double> dist_;
    std::vector<size_t> from_;
};

struct NetRouteOutcome {
    std::vector<Track> tracks;
    std::vector<Via> vias;
    int connections = 0, routed = 0;
};
}  // namespace

namespace {
/// Content hash of a schematic for the voltage-range cache: parts, values and pin-to-net connectivity.
size_t schematicHash(const Schematic& sch) {
    size_t h = 1469598103934665603ULL;
    auto mix = [&](size_t v) { h = (h ^ v) * 1099511628211ULL; };
    std::hash<std::string> hs;
    for (const auto& c : sch.components()) {
        mix(static_cast<size_t>(c.id));
        mix(static_cast<size_t>(c.kind));
        mix(hs(c.value));
        mix(hs(c.customPart));
        for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) mix(static_cast<size_t>(sch.netOf({c.id, i}) + 7));
    }
    return h;
}

std::map<int, std::pair<double, double>> computeNetVoltageRanges(const Schematic& sch) {
    std::map<int, std::pair<double, double>> netRange;
    bool hasSource = false;
    for (const auto& c : sch.components())
        hasSource |= isSourceKind(c.kind);
    if (!hasSource || sch.groundNet() < 0) return netRange;
    DcResult dc = Simulator(sch).dcOperatingPoint();
    if (!dc.converged) return netRange;
    for (size_t n = 0; n < dc.netVoltages.size(); ++n) netRange[static_cast<int>(n)] = {dc.netVoltages[n], dc.netVoltages[n]};
    double mainsAmplitude = 0, period = 0;
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
            if (spec->frequency > 0 && spec->frequency <= 1000) {
                mainsAmplitude = std::max(mainsAmplitude, std::fabs(spec->amplitude));
                period = std::max(period, 1.0 / spec->frequency);
            }
        } else if (spec->kind == SourceSpec::Kind::Pulse) {
            lo = std::min(spec->v1, spec->v2);
            hi = std::max(spec->v1, spec->v2);
        }
        auto& r = netRange[plus];
        r.first = std::min(r.first, base + lo);
        r.second = std::max(r.second, base + hi);
    }
    // Mains and other large low-frequency AC sources: the DC point (t = 0) misses what a fuse, a choke or a
    // rectifier passes on, so simulate a few cycles and widen every net to its swing (L / N after the fuse, the
    // rectified bus, the switch node of an off-line converter).
    // The simulated swing replaces the source-peak estimate, which is wrong when a source's return floats (mains
    // through a bridge: L and N each swing 0…325 V against the bus return, not ±325 V).
    if (mainsAmplitude > 30 && period > 0) {
        TransientResult tr = Simulator(sch).transient(4 * period, period / 200);
        if (tr.ok && tr.time.size() > 4) {
            const size_t from = tr.time.size() / 4;  // after the first cycle
            for (size_t n = 0; n < tr.netVoltages.size(); ++n) {
                const auto& v = tr.netVoltages[n];
                if (v.size() <= from) continue;
                auto [mn, mx] = std::minmax_element(v.begin() + static_cast<std::ptrdiff_t>(from), v.end());
                const double dcv = n < dc.netVoltages.size() ? dc.netVoltages[n] : 0.0;
                netRange[static_cast<int>(n)] = {std::min(dcv, *mn), std::max(dcv, *mx)};
            }
        }
    }
    return netRange;
}
}  // namespace

std::map<int, std::pair<double, double>> netVoltageRanges(const Schematic& sch) {
    // Placement, routing passes, pours and DRC all ask for the ranges: cache the last few schematics.
    static std::mutex mutex;
    static std::vector<std::pair<size_t, std::map<int, std::pair<double, double>>>> cache;
    const size_t key = schematicHash(sch);
    {
        std::lock_guard<std::mutex> lock(mutex);
        for (const auto& [k, v] : cache)
            if (k == key) return v;
    }
    auto ranges = computeNetVoltageRanges(sch);
    std::lock_guard<std::mutex> lock(mutex);
    cache.insert(cache.begin(), {key, ranges});
    if (cache.size() > 4) cache.pop_back();
    return ranges;
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

namespace {
struct RouteCancelled {};  // thrown by routeAll's progress report when the RouteControl cancels
}  // namespace

RouteStats PcbLayout::autoRoute(const Schematic& sch) { return autoRoute(sch, RouteControl{}); }

RouteStats PcbLayout::autoRoute(const Schematic& sch, const RouteControl& control) {
    const BoardSettings before = settings;  // net classes and the grid are adjusted while routing
    try {
        return routeWithVoltageSpacing(sch, control.progress ? &control : nullptr);
    } catch (const RouteCancelled&) {
        settings = before;  // tracks and vias are only replaced once routing has finished
        RouteStats cancelled;
        cancelled.cancelled = true;
        return cancelled;
    }
}

RouteStats PcbLayout::routeWithVoltageSpacing(const Schematic& sch, const RouteControl* control) {
    // Voltage spacing: when the board's largest potential difference needs more than the design-rule clearance
    // (IPC-2221 B2/B3, above 30 V), route with that spacing so the copper meets the DRC voltage check.
    const double ruleClearance = settings.clearance;
    struct RestoreClearance {
        BoardSettings& s;
        double c;
        ~RestoreClearance() { s.clearance = c; }
    } restore{settings, ruleClearance};
    // Mains boards fence their high-voltage nets instead (spacingDomains), so fine-pitch parts keep their rules.
    if (spacingDomains(sch, settings).hvGap <= 0)
        settings.clearance = std::max(ruleClearance, voltageRoutingClearance(sch, settings.highAltitude, settings.coated()));
    return routeAll(sch, control);
}

std::vector<TamperMeshGeometry> PcbLayout::tamperMeshGeometry(const Schematic& sch, const std::vector<Pad>& ps) const {
    std::vector<TamperMeshGeometry> out;
    const double g = settings.routingGrid, w = settings.trackWidth, clr = settings.clearance;
    auto netIndex = [&](const std::string& name) {
        for (const auto& n : sch.nets())
            if (n.name == name) return n.index;
        return -1;
    };
    for (size_t m = 0; m < tamperMeshes.size(); ++m) {
        const TamperMesh& tm = tamperMeshes[m];
        TamperMeshGeometry geo;
        geo.mesh = static_cast<int>(m);
        auto fail = [&](const std::string& why) {
            geo.error = why;
            out.push_back(geo);
        };
        const Component* c = sch.findByRef(tm.componentRef);
        if (!c || !c->hasFootprint() || !c->pcb.placed) { fail(tm.componentRef + " is not placed on the board"); continue; }
        const int n = settings.layerCount;
        if (n < 4) { fail("a tamper mesh needs two inner layers (4 or more copper layers)"); continue; }
        if (tm.layerA < 1 || tm.layerA > n - 2 || tm.layerB < 1 || tm.layerB > n - 2 || tm.layerA == tm.layerB) {
            fail("the mesh layers must be two different inner layers");
            continue;
        }
        bool planeLayer = false;
        for (const auto& z : zones)
            if (z.plane && (z.layer == tm.layerA || z.layer == tm.layerB)) planeLayer = true;
        if (planeLayer) { fail("a mesh layer is reserved as a plane"); continue; }
        geo.netA = netIndex(tm.netA);
        geo.netB = netIndex(tm.netB);
        if (geo.netA < 0 || geo.netB < 0 || geo.netA == geo.netB) { fail("mesh nets " + tm.netA + " / " + tm.netB + " are not two nets of the schematic"); continue; }
        std::vector<size_t> padsA, padsB;
        for (size_t i = 0; i < ps.size(); ++i) {
            if (ps[i].net == geo.netA) padsA.push_back(i);
            if (ps[i].net == geo.netB) padsB.push_back(i);
        }
        if (padsA.size() != 2 || padsB.size() != 2) {
            fail("each mesh net must join exactly two pins (the drive and sense pins of " + tm.componentRef + ")");
            continue;
        }
        // Stripe pitch: one track plus clearance, on the routing grid so the end vias land on grid cells.
        const double p = std::ceil((w + clr) / g - 1e-9) * g;
        const double lead = std::ceil((settings.viaDiameter / 2 + clr + w) / g - 1e-9) * g;
        const Rect r = courtyard(*c).inflated(std::max(0.0, tm.margin));
        const double x0 = std::floor(r.x0 / g + 1e-9) * g, y0 = std::floor(r.y0 / g + 1e-9) * g;
        int cols = static_cast<int>(std::ceil((r.x1 - x0) / p - 1e-9)) + 1;
        int rows = static_cast<int>(std::ceil((r.y1 - y0) / p - 1e-9)) + 1;
        if (cols % 2 == 0) ++cols;  // odd stripe counts end each serpentine on the far side
        if (rows % 2 == 0) ++rows;
        const double x1 = x0 + (cols - 1) * p, y1 = y0 + (rows - 1) * p;
        geo.region = Rect(x0, y0, x1, y1);
        if (!settings.rectInside(geo.region.inflated(lead + settings.viaDiameter / 2), settings.edgeClearance)) {
            fail("the mesh around " + tm.componentRef + " runs off the board or into a mounting hole");
            continue;
        }
        auto add = [&](int net, int layer, Vec2 a, Vec2 b) {
            Track t;
            t.net = net;
            t.layer = layer;
            t.width = w;
            t.a = a;
            t.b = b;
            geo.tracks.push_back(t);
        };
        // Mesh A: horizontal stripes, entering top-left and leaving bottom-right.
        add(geo.netA, tm.layerA, {x0 - lead, y0}, {x0, y0});
        for (int k = 0; k < rows; ++k) {
            const double y = y0 + k * p;
            add(geo.netA, tm.layerA, {x0, y}, {x1, y});
            if (k + 1 < rows) {
                const double xe = k % 2 == 0 ? x1 : x0;
                add(geo.netA, tm.layerA, {xe, y}, {xe, y + p});
            }
        }
        add(geo.netA, tm.layerA, {x1, y1}, {x1 + lead, y1});
        // Mesh B: vertical stripes, entering top-left and leaving bottom-right.
        add(geo.netB, tm.layerB, {x0, y0 - lead}, {x0, y0});
        for (int k = 0; k < cols; ++k) {
            const double x = x0 + k * p;
            add(geo.netB, tm.layerB, {x, y0}, {x, y1});
            if (k + 1 < cols) {
                const double ye = k % 2 == 0 ? y1 : y0;
                add(geo.netB, tm.layerB, {x, ye}, {x + p, ye});
            }
        }
        add(geo.netB, tm.layerB, {x1, y1}, {x1, y1 + lead});
        geo.ends = {{x0 - lead, y0}, {x1 + lead, y1}, {x0, y0 - lead}, {x1, y1 + lead}};
        // Each pad goes to the nearer end (the pairing with the shorter total stub length).
        auto pair = [&](const std::vector<size_t>& two, Vec2 e0, Vec2 e1) {
            const double straight = (ps[two[0]].position - e0).length() + (ps[two[1]].position - e1).length();
            const double crossed = (ps[two[1]].position - e0).length() + (ps[two[0]].position - e1).length();
            if (straight <= crossed) {
                geo.endPads.push_back(two[0]);
                geo.endPads.push_back(two[1]);
            } else {
                geo.endPads.push_back(two[1]);
                geo.endPads.push_back(two[0]);
            }
        };
        pair(padsA, geo.ends[0], geo.ends[1]);
        pair(padsB, geo.ends[2], geo.ends[3]);
        out.push_back(geo);
    }
    return out;
}

RouteStats PcbLayout::routeAll(const Schematic& sch, const RouteControl* control) {
    // Progress and cancellation (RouteControl): reported from this thread only, between nets / batches.
    RouteProgress progressNow;
    auto report = [&] {
        if (control && control->progress && !control->progress(progressNow)) throw RouteCancelled{};
    };
    auto advance = [&](int nets) {  // nets routed: reports about every 2.5 % of the pass
        const int step = std::max(1, progressNow.total / 40), was = progressNow.done;
        progressNow.done += nets;
        if (was / step != progressNow.done / step || progressNow.done >= progressNow.total) report();
    };
    report();
    if (settings.autoSizeNets) autoNetWidths(sch);
    // Reliability net classes: RF lines get their 50 Ω width (when the stack-up allows a practical one);
    // leakage-sensitive and fast nets keep extra spacing and route first, so everything else keeps away from them.
    const NetClassification classes = classifyNets(sch, "");
    for (int net : classes.rf) {
        double z0 = widthForImpedance(settings, kTopLayer, settings.singleEndedImpedance);
        const std::string& name = sch.nets()[static_cast<size_t>(net)].name;
        if (z0 > 0 && z0 <= 1.5 && !settings.netWidths.count(name)) settings.netWidths[name] = std::ceil(z0 * 100) / 100;
    }
    // Differential pairs: both members at the width for the differential target (outer-layer geometry).
    for (auto [p, n] : classes.diffPairs) {
        double w = differentialPairGeometry(settings, kTopLayer, settings.differentialImpedance).first;
        if (w <= 0 || w > 1.0) continue;
        for (int net : {p, n}) {
            const std::string& name = sch.nets()[static_cast<size_t>(net)].name;
            if (!settings.netWidths.count(name)) settings.netWidths[name] = std::max(settings.minTrackWidth, std::ceil(w * 100) / 100);
        }
    }
    std::map<int, double> extraClearance;
    for (const auto& [net, guard] : classes.highImpedance)
        extraClearance[net] = std::max(0.0, leakageSpacing(settings) - settings.clearance);
    for (int net : classes.fast) {
        double w3 = 2 * settings.widthFor(sch.nets()[static_cast<size_t>(net)].name);  // 3W rule: 2 widths edge to edge
        extraClearance[net] = std::max(extraClearance[net], w3 - settings.clearance);
    }
    // Net classes from the schematic's directives: their clearance to other nets' copper.
    if (!settings.netClearances.empty())
        for (const auto& n : sch.nets()) {
            const double need = settings.clearanceFor(n.name) - settings.clearance;
            if (need > 1e-9) extraClearance[n.index] = std::max(extraClearance[n.index], need);
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
    const auto padsByNet = groupPads(ps, true);
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
    // Tamper meshes: laid as fixed copper; their nets only get the two stubs from the pads to the mesh ends.
    const std::vector<TamperMeshGeometry> meshGeo = tamperMeshGeometry(sch, ps);
    std::set<int> meshNets;
    for (const auto& m : meshGeo)
        if (m.error.empty()) {
            meshNets.insert(m.netA);
            meshNets.insert(m.netB);
        }
    auto isMeshNet = [&](int net) { return meshNets.count(net) > 0; };
    order.erase(std::remove_if(order.begin(), order.end(), isMeshNet), order.end());
    zoneOrder.erase(std::remove_if(zoneOrder.begin(), zoneOrder.end(), isMeshNet), zoneOrder.end());

    // BGA fan-out (dogbone): every ball with a net gets a short stub to a via in the gap between four balls,
    // pointing away from the package centre, so the router continues on the inner layers instead of threading the
    // top layer between balls. With via-in-pad (VIPPO) allowed, the via sits in the ball pad instead.
    struct BgaFanout {
        size_t pad;
        Vec2 via;
        double viaDiameter, viaDrill, trackWidth;
        bool inPad;
    };
    std::vector<BgaFanout> bgaFanouts;
    double bgaPitch = 0;
    if (settings.layerCount >= 2) {
        std::map<int, std::vector<size_t>> bgaPads;
        for (size_t i = 0; i < ps.size(); ++i) {
            const Component* c = sch.find(ps[i].componentId);
            if (!c || ps[i].throughHole) continue;
            const FootprintDef* fp = Library::instance().footprint(c->footprintName());
            if (fp && fp->label.rfind("BGA", 0) == 0) bgaPads[c->id].push_back(i);
        }
        for (const auto& [id, list] : bgaPads) {
            if (list.size() < 4) continue;
            double pitch = std::numeric_limits<double>::max(), ball = 0;
            Vec2 centre{0, 0};
            for (size_t a2 : list) {
                centre = centre + ps[a2].position;
                ball = std::max(ball, std::max(ps[a2].size.x, ps[a2].size.y) / 2);
                for (size_t b2 : list)
                    if (a2 != b2) pitch = std::min(pitch, (ps[a2].position - ps[b2].position).length());
            }
            centre = centre * (1.0 / static_cast<double>(list.size()));
            // Snap to 1 µm: the ball spacing comes out a rounding error off the nominal pitch, differently on every
            // compiler (fused multiply-add), and the routing grid below is derived from it.
            pitch = std::round(pitch * 1000.0) / 1000.0;
            if (!(pitch > 0.3 && pitch < 2.0)) continue;
            const double clr = settings.clearance;
            const double diag = pitch / std::sqrt(2.0);  // ball centre to the gap between four balls
            // Largest via (and stub) that keeps clearance to the four surrounding balls and to the next dogbone.
            double vd = std::min({settings.viaDiameter, 2 * (diag - ball - clr), pitch - clr});
            double ring = settings.minAnnularRing;
            double drill = std::min(settings.viaDrill, vd - 2 * ring);
            double tw = std::min(settings.trackWidth, 2 * (diag - ball - clr));
            const bool dogbone = drill >= settings.minDrill - 1e-9 && tw >= settings.minTrackWidth - 1e-9;
            if (!dogbone && !settings.viaInPad) continue;
            bgaPitch = bgaPitch > 0 ? std::min(bgaPitch, pitch) : pitch;
            for (size_t pi : list) {
                const int net = ps[pi].net;
                if (net < 0 || netPads[net].size() < 2) continue;
                BgaFanout f;
                f.pad = pi;
                if (settings.viaInPad) {
                    // Filled and capped via in the ball pad: no stub, nothing between the balls.
                    f.inPad = true;
                    f.via = ps[pi].position;
                    f.viaDiameter = std::min(settings.viaDiameter, 2 * ball);
                    f.viaDrill = std::min(settings.viaDrill, f.viaDiameter - 2 * ring);
                    if (f.viaDrill < settings.minDrill - 1e-9) continue;
                    f.trackWidth = 0;
                } else {
                    const Vec2 d = ps[pi].position - centre;
                    const double sx = d.x < -1e-6 ? -1.0 : 1.0, sy = d.y < -1e-6 ? -1.0 : 1.0;
                    f.inPad = false;
                    f.via = ps[pi].position + Vec2{sx * pitch / 2, sy * pitch / 2};
                    f.viaDiameter = vd;
                    f.viaDrill = drill;
                    f.trackWidth = tw;
                }
                bgaFanouts.push_back(f);
            }
        }
    }
    // The fan-out vias leave channels about one track wide between them: route BGA boards on a grid fine enough
    // to find those channels (eight cells per ball pitch).
    struct RestoreGrid {
        BoardSettings& s;
        double g;
        ~RestoreGrid() { s.routingGrid = g; }
    } restoreGrid{settings, settings.routingGrid};
    if (bgaPitch > 0) settings.routingGrid = std::min(settings.routingGrid, std::max(0.05, bgaPitch / 8));
    std::map<size_t, std::vector<std::pair<int, size_t>>> fanoutCells;  // ball pad → its via's cells

    // Pour/plane-net pads that could not reach their pour: fanned out first in the next pass.
    // Pour/plane-net pads that could not reach their pour: connected first in the next pass, by a track to where
    // the net's main pour was (signals then route around that connection).
    std::set<size_t> forcedConnect;
    std::map<int, std::vector<std::pair<int, size_t>>> mainPour;
    const SpacingDomains spacing = spacingDomains(sch, settings);
    // The routing grid before any net routes: planes and pours, pad keep-outs, isolation fences, tamper meshes and the
    // escape bands around fine-pitch packages.
    auto prepareGrid = [&](RoutingGrid& grid) {
        for (const auto& z : zones) {
            int zn = netIndex(z.net);
            if (zn < 0) continue;
            if (z.plane) grid.setPlane(z.layer, zn);
            else grid.setPour(z.layer, zn);
        }
        for (const auto& p : ps) grid.markPad(p, clr + w / 2);
        if (spacing.gap > 0) {
            double widest = settings.trackWidth;
            for (const auto& [name, width] : settings.netWidths) widest = std::max(widest, width);
            grid.setIsolation(spacing.domains.netDomain, spacing.gap, ps, widest / 2, settings.isolationGap <= 0);
            for (const auto& p : ps) grid.fencePad(p);
            // Mains nets stay ≥ 6 mm from radio modules and their antennas (creepage, and switching noise).
            if (spacing.hvGap > 0)
                if (const int lv = spacing.domains.domainOfNet(sch.groundNet()); lv >= 0)
                    for (const auto& comp : sch.components())
                        if (comp.hasFootprint() && comp.pcb.placed && isRadioPart(comp)) grid.fenceArea(courtyard(comp).inflated(6.0), lv);
        }
        // Mesh copper blocks every net (its own included, so no stub shortcuts the serpentine); the stripes cover the
        // secure area on both mesh layers, so no other track crosses it there and no via can be drilled through it.
        for (const auto& m : meshGeo)
            for (const Track& t : m.tracks) {
                grid.markSegment(t.layer, t.a, t.b, t.width / 2 + clr + w / 2, -3);
                grid.markCopperSegment(t.layer, t.a, t.b, t.width / 2 + 1e-6, -3);
                grid.addCopper(t.a, t.b, t.width / 2, t.net, t.layer);
            }
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
    };
    // Marks fixed copper (fan-out stubs and vias) on a grid, the way routing commits it.
    auto markFixedTrack = [&](RoutingGrid& grid, const Track& t) {
        grid.markSegment(t.layer, t.a, t.b, t.width / 2 + clr + extra(t.net) + w / 2, t.net);
        grid.markCopperSegment(t.layer, t.a, t.b, t.width / 2 + 1e-6, t.net);
        grid.addCopper(t.a, t.b, t.width / 2, t.net, t.layer);
        grid.fenceCopper(t.a, t.b, t.width / 2, t.net, t.layer);
    };
    auto markFixedVia = [&](RoutingGrid& grid, const Via& v) {
        const double r = v.diameter / 2 + clr + extra(v.net) + w / 2;
        for (int l = 0; l < grid.layers(); ++l) {
            grid.markDisc(l, v.position, r, v.net);
            grid.markCopperSegment(l, v.position, v.position, v.diameter / 2, v.net);
        }
        grid.addCopper(v.position, v.position, v.diameter / 2, v.net, -1, v.drill);
        grid.fenceCopper(v.position, v.position, v.diameter / 2, v.net);
    };
    std::unique_ptr<AStarWorkspace> astarWs;
    // Large boards: the corridor router (GlobalRouter.hpp) — coarse global routing, detailed routing only inside each
    // connection's corridor, independent nets on several threads, and targeted rip-up instead of whole-board passes.
    const int gridCols = static_cast<int>(std::floor(settings.width / settings.routingGrid)) + 1;
    const int gridRows = static_cast<int>(std::floor(settings.height / settings.routingGrid)) + 1;
    const size_t gridNodes = static_cast<size_t>(std::max(1, settings.layerCount)) * static_cast<size_t>(gridCols) *
                             static_cast<size_t>(gridRows);
    const RouterStrategy strategy = routerStrategy();
    const bool corridorMode =
        strategy == RouterStrategy::Corridor || (strategy == RouterStrategy::Auto && gridNodes >= kLargeGrid);
    routing::TileGrid tiles;
    tiles.tile = std::max(4, static_cast<int>(std::lround(2.0 / settings.routingGrid)));  // about 2 mm
    tiles.cols = tiles.tilesFor(gridCols);
    tiles.rows = tiles.tilesFor(gridRows);
    const int threads = corridorMode ? effectiveRoutingThreads() : 1;
    std::vector<std::unique_ptr<AStarWorkspace>> threadWs;  // one search workspace per routing thread
    // Each signal net's connections in routing order: (target pad, nearest connected pad), as list indices. The order
    // depends on pad positions only, so it is known before anything routes.
    std::map<int, std::vector<std::pair<size_t, size_t>>> plan;
    if (corridorMode)
        for (int net : order) {
            NearestPad np(ps, netPads[net]);
            np.connect(0);
            for (size_t done = 1; done < netPads[net].size(); ++done) {
                const size_t t = np.next();
                const size_t from = np.from(t);
                plan[net].push_back({t, from == std::numeric_limits<size_t>::max() ? 0 : from});
                np.connect(t);
            }
        }
    struct NetCopper {
        std::vector<Track> tracks;
        std::vector<Via> vias;
        int connections = 0, routed = 0, viaCount = 0;
        double length = 0;
    };
    std::map<int, NetCopper> prevCopper;  // corridor rip-up: the previous pass's signal copper of each net
    std::vector<int> ripOrder;            // nets the next corridor pass routes again (the others are replayed)
    size_t ripHead = 0, bestRipHead = 0;  // how many of them (the failing nets and their buses) route first
    struct RipUpChain {
        bool congestion;   // rip-up passes pay the negotiated-congestion history
        bool buses;        // a failing net's bus is ripped and re-routed with it, in order across the bus
        bool wholeBoard;   // every third pass without improvement re-routes the whole board
        bool headFirst;    // the failing nets are routed completely (retries included) before the ripped others
        bool revert;       // a pass much worse than the best goes back to the best pass
        double alongside;  // mm per aggression level: nets this close to the needed path are ripped too
    };
    static constexpr RipUpChain kRipUpChains[] = {
        {false, false, false, false, false, 0.75},  // plain walk: ripped nets in routing order
        {true, false, true, true, true, 1.5},       // negotiated congestion, whole-board passes
        {true, true, false, true, true, 1.5},       // buses re-ordered
    };
    constexpr int kRipUpRounds = 30, kRipUpStall = 4;
    constexpr int kCorridorWidth = 2;  // tiles either side of a connection's global route (and straight line)
    int ripChain = 0;
    std::map<int, NetCopper> bestCopper;  // the best pass's signal copper, and the nets its rip-up re-routes
    std::vector<int> bestRip;
    int ripRound = 0, ripStall = 0;
    std::set<int> ripPriority;     // nets that failed in the last pass: exempt from the tile history
    std::vector<int> tileHistory;  // global-routing cost of the tiles unrouted connections needed
    // Recovery: when the rip-up passes still leave connections unrouted, up to kRecoveryPasses more passes route
    // with negotiated congestion — each unrouted connection's unobstructed path (pads only) raises the cost of its
    // corridor for every other net, so they make room — and a cheaper via. A recovery pass's result is kept only
    // when it routes strictly more, so boards the rip-up passes complete are never affected.
    constexpr int kRipUpPasses = 8, kRecoveryPasses = 8, kRecoveryStall = 3;
    constexpr double kViaCost = 12.0, kRecoveryViaCost = 8.0;
    constexpr float kHistoryStep = 2.0f;  // corridor cost added per unrouted connection and recovery pass
    const std::vector<int> stableOrder = order;
    bool recovery = false;
    int recoveryPass = 0, recoveryStall = 0;
    std::unique_ptr<Congestion> congestion;
    struct FailedConnection {
        int net;
        size_t pad;  // the pad that could not be reached
    };
    for (int pass = 0;; ++pass) {
        progressNow.phase = pass == 0 ? RouteProgress::Routing : RouteProgress::RipUp;
        progressNow.pass = pass;
        progressNow.done = 0;
        progressNow.total = static_cast<int>(corridorMode && pass > 0 ? ripOrder.size() : order.size());
        report();
        const size_t forcedBefore = forcedConnect.size();
        // Recovery and corridor rip-up passes make layer changes cheaper, which helps a net past a blockage.
        const double passViaCost = recovery || (corridorMode && pass > 0) ? kRecoveryViaCost : kViaCost;
        const Congestion* passCongestion =
            recovery || (corridorMode && pass > 0 && kRipUpChains[ripChain].congestion) ? congestion.get() : nullptr;
        std::vector<FailedConnection> failedConnections;
        size_t fixedTracks = 0, fixedVias = 0;  // copper laid before any net routes (BGA fan-outs, tamper meshes)
        RoutingGrid grid(settings);
        // One search workspace for every pass (the grid's size is the same in each).
        // Corridor mode: one corridor workspace per routing thread; the first also serves the serial searches (fan-outs,
        // retries, pours), which never run while the threads do.
        if (corridorMode && threadWs.empty())
            for (int t = 0; t < threads; ++t)
                threadWs.push_back(std::make_unique<AStarWorkspace>(grid.cols(), grid.rows(), grid.layers(), tiles));
        if (!corridorMode && !astarWs)
            astarWs = std::make_unique<AStarWorkspace>(static_cast<size_t>(grid.layers() * grid.cols() * grid.rows()),
                                                       static_cast<size_t>(grid.cols() * grid.rows()));
        AStarWorkspace& ws = corridorMode ? *threadWs.front() : *astarWs;
        const size_t gridCells = static_cast<size_t>(grid.cols() * grid.rows());
        prepareGrid(grid);
        std::vector<Track> outT;
        std::vector<Via> outV;
        RouteStats stats;
        std::vector<int> failedNets;

        auto padCells = [&](const Pad& pad, std::vector<std::pair<int, size_t>>& cells) {
            for (int l = 0; l < grid.layers(); ++l)
                if (pad.onLayer(l))
                    for (size_t c : grid.padCoreCells(pad)) cells.push_back({l, c});
            // A fanned-out BGA ball is reached through its via, on any layer.
            if (!fanoutCells.empty() && &pad >= ps.data() && &pad < ps.data() + ps.size())
                if (auto it = fanoutCells.find(static_cast<size_t>(&pad - ps.data())); it != fanoutCells.end())
                    cells.insert(cells.end(), it->second.begin(), it->second.end());
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
            grid.fenceCopper(v.position, v.position, settings.viaDiameter / 2, net);
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
                        neckDownWith(pieces, ps, neckWidths, padsByNet, settings.clearance);
                        for (const Track& piece : pieces) {
                            outT.push_back(piece);
                            grid.markSegment(layer, piece.a, piece.b, piece.width / 2 + clr + extra(net) + w / 2, net);
                            grid.markCopperSegment(layer, piece.a, piece.b, piece.width / 2 + 1e-6, net);
                            grid.addCopper(piece.a, piece.b, piece.width / 2, net, layer);
                            grid.fenceCopper(piece.a, piece.b, piece.width / 2, net, layer);
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
        // Corridor mode: restricts a workspace to the tiles within `radius` of segment a–b (no-op otherwise).
        auto focus = [&](AStarWorkspace& w, Vec2 a, Vec2 b, double radius) {
            if (!w.corridorMode()) return;
            const double g = grid.pitch();
            const int span = tiles.tile;
            auto tileIndex = [&](double v, int count) {
                return std::clamp(static_cast<int>(std::floor(v / g)) / span, 0, count - 1);
            };
            const int x0 = tileIndex(std::min(a.x, b.x) - radius, tiles.cols), x1 = tileIndex(std::max(a.x, b.x) + radius, tiles.cols);
            const int y0 = tileIndex(std::min(a.y, b.y) - radius, tiles.rows), y1 = tileIndex(std::max(a.y, b.y) + radius, tiles.rows);
            std::vector<int> ids;
            for (int y = y0; y <= y1; ++y)
                for (int x = x0; x <= x1; ++x) ids.push_back(tiles.id(x, y));
            w.setCorridor(ids);
        };
        // Neck zones of the given pads for a track of width wn: per cell the necked half-width (0 outside), the same
        // zones and widths neckDown applies afterwards.
        auto neckZones = [&](AStarWorkspace& ws, const std::vector<size_t>& padList, double wn) {
            ws.beginNeck();
            for (size_t pi : padList) {
                if (neckWidths[pi] >= wn - 1e-9) continue;
                Rect zone = ps[pi].bounds().inflated(clr + wn / 2);
                int i0 = std::max(0, static_cast<int>(std::ceil(zone.x0 / grid.pitch() - 1e-9)));
                int i1 = std::min(grid.cols() - 1, static_cast<int>(std::floor(zone.x1 / grid.pitch() + 1e-9)));
                int j0 = std::max(0, static_cast<int>(std::ceil(zone.y0 / grid.pitch() - 1e-9)));
                int j1 = std::min(grid.rows() - 1, static_cast<int>(std::floor(zone.y1 / grid.pitch() + 1e-9)));
                const float half = static_cast<float>(neckWidths[pi] / 2);
                uint32_t padLayers = 0;
                for (int l = 0; l < grid.layers(); ++l)
                    if (l < 32 && ps[pi].onLayer(l)) padLayers |= 1u << l;
                for (int j = j0; j <= j1; ++j)
                    for (int i = i0; i <= i1; ++i) {
                        const float h = ws.neck(grid.idx(i, j));
                        ws.setNeck(grid.idx(i, j), h > 0 ? std::min(h, half) : half, padLayers);
                    }
            }
        };
        // Routes from `tree` to pad `tp` and commits the copper; returns false if no path exists.
        auto connect = [&](int net, const std::vector<size_t>& list, std::vector<std::pair<int, size_t>>& tree,
                           const Pad& tp, const std::vector<std::pair<int, size_t>>* extraTargets = nullptr) {
            std::vector<std::pair<int, size_t>> targetCells;
            padCells(tp, targetCells);
            if (extraTargets) targetCells.insert(targetCells.end(), extraTargets->begin(), extraTargets->end());
            ws.beginTargets();
            for (auto [l, c] : targetCells) ws.addTarget(static_cast<size_t>(l) * gridCells + c);
            const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
            const double wideHalf = wn > w + 1e-9 ? wn / 2 : 0.0;
            neckZones(ws, list, wn);
            RouteResult rr;
            if (!corridorMode) rr = astar(grid, net, tree, ws, tp.position, passViaCost, wideHalf, true, nullptr, passCongestion);
            // Corridor mode: the search covers the area around the pad, widened until it succeeds.
            for (double radius : {4.0, 12.0, 40.0}) {
                if (!corridorMode || rr.ok) break;
                focus(ws, tp.position, tp.position, radius);
                ws.beginTargets();
                for (auto [l, c] : targetCells) ws.addTarget(static_cast<size_t>(l) * gridCells + c);
                neckZones(ws, list, wn);
                rr = astar(grid, net, tree, ws, tp.position, passViaCost, wideHalf, true, nullptr, passCongestion);
            }
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
            const double reach = 3.0;
            const int ci = static_cast<int>(std::lround(pad.position.x / grid.pitch()));
            const int cj = static_cast<int>(std::lround(pad.position.y / grid.pitch()));
            const int k = static_cast<int>(std::ceil(reach / grid.pitch()));
            // A via site within reach of the pad: tested only where the search arrives (viaAllowed is expensive and
            // the nearest site is usually a few cells away).
            const std::function<bool(int, size_t)> viaSite = [&](int l, size_t c) {
                if (l != layer) return false;
                const int i = static_cast<int>(c % static_cast<size_t>(grid.cols())), j = static_cast<int>(c / static_cast<size_t>(grid.cols()));
                if (std::abs(i - ci) > k || std::abs(j - cj) > k) return false;
                if (!grid.inside(i, j) || (grid.pos(i, j) - pad.position).length() > reach) return false;
                if (!grid.passable(layer, c, net) || !grid.viaAllowed(c, net)) return false;
                return !(wn > w + 1e-9 &&
                         !grid.wideClear(layer, grid.pos(i, j), grid.pos(i, j), net, std::max(wn, settings.viaDiameter) / 2));
            };
            bool any = false;
            for (int dj = -k; dj <= k && !any; ++dj)
                for (int di = -k; di <= k && !any; ++di) {
                    const int i = ci + di, j = cj + dj;
                    if (grid.inside(i, j)) any = viaSite(layer, grid.idx(i, j));
                }
            if (!any) return false;
            std::vector<std::pair<int, size_t>> src;
            for (size_t c : grid.padCoreCells(pad)) src.push_back({layer, c});
            focus(ws, pad.position, pad.position, reach + 1.0);
            neckZones(ws, {pi}, wn);
            RouteResult rr = astar(grid, net, src, ws, pad.position, 1e6, wn > w + 1e-9 ? wn / 2 : 0.0, true, &viaSite);
            if (!rr.ok || rr.path.empty() || rr.path.back().layer != layer) return false;
            std::vector<std::pair<int, size_t>> scratch;
            commit(net, wn, rr, scratch);
            placeVia(net, grid.pos(rr.path.back().i, rr.path.back().j));
            return true;
        };

        // BGA fan-out: the dogbone stubs and vias (or vias in pad), laid before any net routes.
        fanoutCells.clear();
        for (const BgaFanout& f : bgaFanouts) {
            const Pad& pad = ps[f.pad];
            const int net = pad.net;
            if (!f.inPad) {
                Track t;
                t.net = net;
                t.layer = pad.smdLayer;
                t.width = f.trackWidth;
                t.a = pad.position;
                t.b = f.via;
                outT.push_back(t);
                grid.markSegment(t.layer, t.a, t.b, t.width / 2 + clr + extra(net) + w / 2, net);
                grid.markCopperSegment(t.layer, t.a, t.b, t.width / 2 + 1e-6, net);
                grid.addCopper(t.a, t.b, t.width / 2, net, t.layer);
                grid.fenceCopper(t.a, t.b, t.width / 2, net, t.layer);
            }
            Via v;
            v.net = net;
            v.position = f.via;
            v.drill = f.viaDrill;
            v.diameter = f.viaDiameter;
            outV.push_back(v);
            ++stats.vias;
            const double r = f.viaDiameter / 2 + clr + extra(net) + w / 2;
            for (int l = 0; l < grid.layers(); ++l) {
                grid.markDisc(l, v.position, r, net);
                grid.markCopperSegment(l, v.position, v.position, f.viaDiameter / 2, net);
            }
            grid.addCopper(v.position, v.position, f.viaDiameter / 2, net, -1, f.viaDrill);
            grid.fenceCopper(v.position, v.position, f.viaDiameter / 2, net);
            const int ci = static_cast<int>(std::lround(f.via.x / grid.pitch()));
            const int cj = static_cast<int>(std::lround(f.via.y / grid.pitch()));
            auto& cells = fanoutCells[f.pad];
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    const int i = ci + di, j = cj + dj;
                    if (!grid.inside(i, j) || (grid.pos(i, j) - f.via).length() > f.viaDiameter / 2) continue;
                    for (int l = 0; l < grid.layers(); ++l) cells.push_back({l, grid.idx(i, j)});
                }
            if (cells.empty() && grid.inside(ci, cj))
                for (int l = 0; l < grid.layers(); ++l) cells.push_back({l, grid.idx(ci, cj)});
        }

        // Tamper meshes: the serpentines, a via at each end and a stub from each drive / sense pad to its end.
        for (const auto& m : meshGeo) {
            if (!m.error.empty()) continue;
            for (const Track& t : m.tracks) outT.push_back(t);
            for (size_t e = 0; e < m.ends.size(); ++e) placeVia(e < 2 ? m.netA : m.netB, m.ends[e]);
        }
        for (const auto& m : meshGeo) {
            if (!m.error.empty()) continue;
            const TamperMesh& tm = tamperMeshes[static_cast<size_t>(m.mesh)];
            bool ok[2] = {true, true};
            for (size_t e = 0; e < m.ends.size(); ++e) {
                const int net = e < 2 ? m.netA : m.netB;
                const Pad& pad = ps[m.endPads[e]];
                std::vector<std::pair<int, size_t>> src;
                padCells(pad, src);
                focus(ws, pad.position, m.ends[e], 6.0);
                ws.beginTargets();
                const int ci = static_cast<int>(std::lround(m.ends[e].x / grid.pitch()));
                const int cj = static_cast<int>(std::lround(m.ends[e].y / grid.pitch()));
                if (grid.inside(ci, cj))
                    for (int l = 0; l < grid.layers(); ++l)
                        if (l != tm.layerA && l != tm.layerB) ws.addTarget(static_cast<size_t>(l) * gridCells + grid.idx(ci, cj));
                RouteResult rr = astar(grid, net, src, ws, m.ends[e], 12.0, 0.0, false);
                if (rr.ok) {
                    std::vector<std::pair<int, size_t>> scratch;
                    commit(net, settings.widthFor(nets[static_cast<size_t>(net)].name), rr, scratch);
                } else {
                    ok[e < 2 ? 0 : 1] = false;
                }
            }
            for (int k = 0; k < 2; ++k) {
                const int net = k == 0 ? m.netA : m.netB;
                ++stats.connections;
                if (ok[k]) ++stats.routed;
                else stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
            }
        }

        fixedTracks = outT.size();
        fixedVias = outV.size();
        // Corridor rip-up passes: the nets the previous pass kept are laid again as they were — before the pour
        // fan-outs and forced pour connections, which then route around them as they would around any signal.
        std::map<int, NetCopper> curCopper;
        if (corridorMode) {
            grid.markFixedEnd();  // everything laid so far stays; signal copper from here on may be ripped up
            {
                std::vector<char> signalNets(nets.size(), 0);  // pour and plane nets' copper is never ripped up
                for (int net : order) signalNets[static_cast<size_t>(net)] = 1;
                grid.setRippable(std::move(signalNets));
            }
            if (pass > 0) {
                const std::set<int> rip(ripOrder.begin(), ripOrder.end());
                for (int net : order) {
                    auto it = prevCopper.find(net);
                    if (rip.count(net) || it == prevCopper.end()) continue;
                    const NetCopper& kept = it->second;
                    for (const Track& t : kept.tracks) {
                        outT.push_back(t);
                        markFixedTrack(grid, t);
                    }
                    for (const Via& v : kept.vias) {
                        outV.push_back(v);
                        markFixedVia(grid, v);
                    }
                    stats.connections += kept.connections;
                    stats.routed += kept.routed;
                    stats.vias += kept.viaCount;
                    stats.trackLength += kept.length;
                    curCopper[net] = kept;
                }
            }
        }
        if (grid.layers() > 1)
            for (int net : zoneOrder)
                for (size_t pi : netPads[net]) {
                    const Pad& p = ps[pi];
                    if (p.throughHole) continue;
                    bool pourHere = false;
                    for (const auto& z : zones)
                        if (!z.plane && z.layer == p.smdLayer && netIndex(z.net) == net) pourHere = true;
                    bool fine = neckWidths[pi] < w - 1e-9 || std::min(p.size.x, p.size.y) < 0.4;
                    if ((!pourHere || fine) && !fanoutCells.count(pi)) fanout(net, pi);
                }
        for (size_t pi : forcedConnect) {
            int net = ps[pi].net;
            auto it = mainPour.find(net);
            if (it == mainPour.end() || it->second.empty()) continue;
            std::vector<std::pair<int, size_t>> from = it->second;
            connect(net, netPads[net], from, ps[pi]);
        }

        // Corridor mode (large boards): the nets to route (every signal net in the first pass, the ripped ones in a
        // rip-up pass) are global-routed, then routed inside their corridors — independent nets in parallel — and the
        // connections a corridor could not hold are retried in wider ones.
        std::map<std::pair<int, size_t>, std::vector<int>> failedPaths;  // (net, pad) → its global route
        std::vector<size_t> zoneFailed;  // pour / plane-net pads the pass could not join to their pour
        if (corridorMode) {
            std::vector<int> toRoute = pass > 0 ? ripOrder : order;
            // Routes a group of nets: global routing against the board as it stands, the nets in their corridors
            // (independent ones in parallel), then wider corridors for what is left.
            auto routeGroup = [&](const std::vector<int>& toRoute) {
                const double g = grid.pitch();
                const int T = tiles.tile;
                const double trackPitch = w + clr;
                // Global routing. Capacity of each tile boundary, in quarter tracks, from the free cells along it on every
                // signal layer (a layer counts half across its preferred direction), derated to 3/4.
                std::vector<int> capE(static_cast<size_t>(tiles.count()), 0), capS(static_cast<size_t>(tiles.count()), 0);
                for (int ty = 0; ty < tiles.rows; ++ty)
                    for (int tx = 0; tx < tiles.cols; ++tx) {
                        const size_t t = static_cast<size_t>(tiles.id(tx, ty));
                        for (int l = 0; l < grid.layers(); ++l) {
                            if (!grid.layerOpen(l, -100)) continue;
                            if (tx + 1 < tiles.cols) {
                                int free = 0;
                                for (int j = ty * T; j < std::min(grid.rows(), (ty + 1) * T); ++j)
                                    free += grid.passable(l, grid.idx((tx + 1) * T, j), -100) ? 1 : 0;
                                const int q = static_cast<int>(std::floor(4.0 * free * g / trackPitch));
                                capE[t] += l % 2 == 0 ? q : q / 2;
                            }
                            if (ty + 1 < tiles.rows) {
                                int free = 0;
                                for (int i = tx * T; i < std::min(grid.cols(), (tx + 1) * T); ++i)
                                    free += grid.passable(l, grid.idx(i, (ty + 1) * T), -100) ? 1 : 0;
                                const int q = static_cast<int>(std::floor(4.0 * free * g / trackPitch));
                                capS[t] += l % 2 == 1 ? q : q / 2;
                            }
                        }
                        capE[t] = capE[t] * 3 / 4;
                        capS[t] = capS[t] * 3 / 4;
                    }
                auto tileOf = [&](Vec2 at) {
                    const int i = std::clamp(static_cast<int>(std::lround(at.x / g)), 0, grid.cols() - 1);
                    const int j = std::clamp(static_cast<int>(std::lround(at.y / g)), 0, grid.rows() - 1);
                    return tiles.ofCell(i, j);
                };
                std::vector<routing::GlobalConnection> gconns;
                std::vector<size_t> firstConn(toRoute.size(), 0);
                std::vector<const std::vector<std::pair<size_t, size_t>>*> steps(toRoute.size(), nullptr);
                for (size_t n = 0; n < toRoute.size(); ++n) {
                    const int net = toRoute[n];
                    const auto& list = netPads[net];
                    steps[n] = &plan[net];
                    firstConn[n] = gconns.size();
                    const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
                    const int demand = 4 * std::max(1, static_cast<int>(std::ceil((wn + clr + 2 * extra(net)) / trackPitch - 1e-9)));
                    for (const auto& [target, from] : *steps[n])
                        gconns.push_back({tileOf(ps[list[from]].position), tileOf(ps[list[target]].position), demand,
                                          ripPriority.count(net) > 0});
                }
                routing::GlobalRouter globalRouter(tiles, capE, capS);
                if (!tileHistory.empty()) globalRouter.setTileCost(tileHistory);
                const std::vector<std::vector<int>> gpaths = globalRouter.route(gconns);
                // Corridors: each connection's tiles, widened (below). A net's reach — where its copper can change what
                // another net's search sees — is its corridor widened by the clearance reach of tracks, vias and fences.
                double widest = std::max(w, settings.viaDiameter);
                for (const auto& [name, width] : settings.netWidths) widest = std::max(widest, width);
                double maxExtra = 0;
                for (const auto& [net, e] : extraClearance) maxExtra = std::max(maxExtra, e);
                const double reachMm = 2 * (widest + clr + maxExtra + spacing.gap + 2 * g);
                const int reachTiles = std::max(1, static_cast<int>(std::ceil(reachMm / (T * g))));
                std::vector<std::vector<std::vector<int>>> connCorr(toRoute.size());
                std::vector<std::vector<int>> netCorr(toRoute.size()), netReach(toRoute.size());
                for (size_t n = 0; n < toRoute.size(); ++n) {
                    std::vector<int> all;
                    for (size_t k = 0; k < steps[n]->size(); ++k) {
                        // The global route plus the straight line between the ends (a staircase of tiles strays
                        // from the diagonal; the straight band keeps the 45° route open), widened by kCorridorWidth.
                        std::vector<int> base = gpaths[firstConn[n] + k];
                        const routing::GlobalConnection& gc = gconns[firstConn[n] + k];
                        const int x0 = gc.from % tiles.cols, y0 = gc.from / tiles.cols, x1 = gc.to % tiles.cols, y1 = gc.to / tiles.cols;
                        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
                        auto roundDiv = [](int a, int b) { return a >= 0 ? (2 * a + b) / (2 * b) : -((-2 * a + b) / (2 * b)); };
                        for (int q = 0; q <= steps; ++q) {
                            const int x = steps ? x0 + roundDiv((x1 - x0) * q, steps) : x0;
                            const int y = steps ? y0 + roundDiv((y1 - y0) * q, steps) : y0;
                            base.push_back(tiles.id(std::clamp(x, 0, tiles.cols - 1), std::clamp(y, 0, tiles.rows - 1)));
                        }
                        connCorr[n].push_back(routing::dilateTiles(tiles, base, kCorridorWidth));
                        all.insert(all.end(), connCorr[n].back().begin(), connCorr[n].back().end());
                    }
                    std::sort(all.begin(), all.end());
                    all.erase(std::unique(all.begin(), all.end()), all.end());
                    netReach[n] = routing::dilateTiles(tiles, all, reachTiles);
                    netCorr[n] = std::move(all);
                }
                struct ConnResult {
                    bool ok = false;
                    RouteResult rr;
                    std::vector<std::pair<int, size_t>> targetCells;
                    int island = -1;  // failed: the island its pad started
                };
                // A net's tree: its copper cells, each with the island it belongs to. A connection that fails still adds its
                // pad, as a new island, so later pads can reach it (as in the classic router); the retries below join
                // islands to the rest.
                struct NetSearch {
                    std::vector<ConnResult> conns;
                    std::vector<std::pair<int, size_t>> tree;
                    std::vector<int> island;  // per tree cell
                    std::vector<int> parent;  // island union-find
                    int find(int a) {
                        while (parent[static_cast<size_t>(a)] != a) a = parent[static_cast<size_t>(a)] = parent[static_cast<size_t>(parent[static_cast<size_t>(a)])];
                        return a;
                    }
                    int newIsland() {
                        parent.push_back(static_cast<int>(parent.size()));
                        return parent.back();
                    }
                    void add(const std::vector<std::pair<int, size_t>>& cells, int id) {
                        tree.insert(tree.end(), cells.begin(), cells.end());
                        island.insert(island.end(), cells.size(), id);
                    }
                    /// Island of the path's first cell (a tree cell).
                    int islandOf(const PathNode& start, size_t cell) {
                        for (size_t t = 0; t < tree.size(); ++t)
                            if (tree[t].first == start.layer && tree[t].second == cell) return find(island[t]);
                        return find(0);
                    }
                };
                std::vector<NetSearch> found(toRoute.size());
                // One net, every connection, against the board as it stands (read only: safe on several threads).
                auto searchNet = [&](size_t n, AStarWorkspace& tw) {
                    const int net = toRoute[n];
                    const auto& list = netPads[net];
                    NetSearch& out = found[n];
                    {
                        std::vector<std::pair<int, size_t>> root;
                        padCells(ps[list[0]], root);
                        out.add(root, out.newIsland());
                    }
                    tw.clearOwnVias();
                    const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
                    const double wideHalf = wn > w + 1e-9 ? wn / 2 : 0.0;
                    for (size_t k = 0; k < steps[n]->size(); ++k) {
                        ConnResult cr;
                        const Pad& tp = ps[list[(*steps[n])[k].first]];
                        tw.setCorridor(connCorr[n][k]);
                        padCells(tp, cr.targetCells);
                        tw.beginTargets();
                        for (auto [l, c] : cr.targetCells) tw.addTarget(static_cast<size_t>(l) * gridCells + c);
                        neckZones(tw, list, wn);
                        cr.rr = astar(grid, net, out.tree, tw, tp.position, passViaCost, wideHalf, true, nullptr, passCongestion);
                        cr.ok = cr.rr.ok;
                        if (cr.ok) {
                            const auto& path = cr.rr.path;
                            const int id = out.islandOf(path[0], grid.idx(path[0].i, path[0].j));
                            std::vector<std::pair<int, size_t>> cells;
                            for (size_t m = 0; m < path.size(); ++m) {
                                cells.push_back({path[m].layer, grid.idx(path[m].i, path[m].j)});
                                if (m + 1 < path.size() && path[m + 1].layer != path[m].layer) tw.addOwnVia(grid.pos(path[m].i, path[m].j));
                            }
                            out.add(cells, id);
                            out.add(cr.targetCells, id);
                        } else {
                            cr.island = out.newIsland();
                            out.add(cr.targetCells, cr.island);
                        }
                        out.conns.push_back(std::move(cr));
                    }
                };
                // Commits one routed path of `net`, recording its copper for a later rip-up pass.
                auto commitRecorded = [&](int net, const RouteResult& rr, std::vector<std::pair<int, size_t>>& tree) {
                    NetCopper& rec = curCopper[net];
                    const size_t t0 = outT.size(), v0 = outV.size();
                    const int vias0 = stats.vias;
                    const double len0 = stats.trackLength;
                    commit(net, settings.widthFor(nets[static_cast<size_t>(net)].name), rr, tree);
                    rec.tracks.insert(rec.tracks.end(), outT.begin() + static_cast<std::ptrdiff_t>(t0), outT.end());
                    rec.vias.insert(rec.vias.end(), outV.begin() + static_cast<std::ptrdiff_t>(v0), outV.end());
                    rec.viaCount += stats.vias - vias0;
                    rec.length += stats.trackLength - len0;
                };
                std::vector<std::pair<size_t, size_t>> deferred;  // (net, connection) a corridor could not hold
                for (const std::vector<int>& batch : routing::scheduleBatches(tiles, netCorr, netReach)) {
                    routing::parallelFor(static_cast<int>(batch.size()), threads, [&](int k, int worker) {
                        searchNet(static_cast<size_t>(batch[static_cast<size_t>(k)]), *threadWs[static_cast<size_t>(worker)]);
                    });
                    for (auto& tw : threadWs) tw->clearOwnVias();  // the first one also serves the serial searches
                    advance(static_cast<int>(batch.size()));
                    for (int bn : batch) {
                        const size_t n = static_cast<size_t>(bn);
                        const int net = toRoute[n];
                        NetCopper& rec = curCopper[net];
                        std::vector<std::pair<int, size_t>> scratch;
                        for (size_t k = 0; k < found[n].conns.size(); ++k) {
                            ConnResult& cr = found[n].conns[k];
                            ++stats.connections;
                            ++rec.connections;
                            if (!cr.ok) {
                                deferred.push_back({n, k});
                                continue;
                            }
                            commitRecorded(net, cr.rr, scratch);
                            scratch.clear();
                            ++stats.routed;
                            ++rec.routed;
                            cr.rr = RouteResult();
                        }
                    }
                }
                // Wider corridors for what is left, one connection at a time.
                std::set<int> failedHere;
                for (auto [n, k] : deferred) {
                    const int net = toRoute[n];
                    const auto& list = netPads[net];
                    const Pad& tp = ps[list[(*steps[n])[k].first]];
                    ConnResult& cr = found[n].conns[k];
                    NetSearch& ns = found[n];
                    const double wn = settings.widthFor(nets[static_cast<size_t>(net)].name);
                    const double wideHalf = wn > w + 1e-9 ? wn / 2 : 0.0;
                    // From the pad's island (with what later pads joined to it) to the rest of the net.
                    const int mine = ns.find(cr.island);
                    std::vector<std::pair<int, size_t>> from, to;
                    for (size_t t = 0; t < ns.tree.size(); ++t) (ns.find(ns.island[t]) == mine ? to : from).push_back(ns.tree[t]);
                    bool ok = from.empty();  // another retry joined it already
                    // Wider around the global route, then around a second global route that avoids the first one's
                    // corridor (a bus whose direct way is full may still get through elsewhere).
                    std::vector<int> alternative;
                    for (int attempt = 0; attempt < 3 && !ok; ++attempt) {
                        const int d = attempt == 1 ? 8 : 3;
                        std::vector<int> base = gpaths[firstConn[n] + k];
                        if (attempt == 1)
                            for (size_t pi : list) base.push_back(tileOf(ps[pi].position));
                        if (attempt == 2) {
                            std::vector<int> avoid(static_cast<size_t>(tiles.count()), 0);
                            for (int t : routing::dilateTiles(tiles, base, 1)) avoid[static_cast<size_t>(t)] = 3000;
                            routing::GlobalRouter detour(tiles, capE, capS);
                            detour.setTileCost(std::move(avoid));
                            routing::GlobalConnection again = gconns[firstConn[n] + k];
                            again.exempt = false;
                            base = detour.route({again}, 0)[0];
                        }
                        ws.setCorridor(routing::dilateTiles(tiles, base, d));
                        ws.beginTargets();
                        for (auto [l, c] : to) ws.addTarget(static_cast<size_t>(l) * gridCells + c);
                        neckZones(ws, list, wn);
                        const RouteResult rr = astar(grid, net, from, ws, tp.position, passViaCost, wideHalf, true, nullptr, passCongestion);
                        if (!rr.ok) continue;
                        const int other = ns.islandOf(rr.path[0], grid.idx(rr.path[0].i, rr.path[0].j));
                        std::vector<std::pair<int, size_t>> cells;
                        commitRecorded(net, rr, cells);
                        ns.add(cells, other);
                        ns.parent[static_cast<size_t>(mine)] = other;
                        ok = true;
                    }
                    if (ok) {
                        ++stats.routed;
                        ++curCopper[net].routed;
                    } else {
                        failedConnections.push_back({net, list[(*steps[n])[k].first]});
                        failedPaths[{net, list[(*steps[n])[k].first]}] = gpaths[firstConn[n] + k];
                        failedHere.insert(net);
                    }
                }
                for (int net : toRoute)
                    if (failedHere.count(net)) {
                        failedNets.push_back(net);
                        stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
                    }
            };
            // A rip-up pass routes the failing nets (and their bus) completely, retries included, before the nets that
            // were ripped to make room for them take any space back.
            if (pass > 0 && kRipUpChains[ripChain].headFirst && ripHead > 0 && ripHead < toRoute.size()) {
                routeGroup(std::vector<int>(toRoute.begin(), toRoute.begin() + static_cast<std::ptrdiff_t>(ripHead)));
                routeGroup(std::vector<int>(toRoute.begin() + static_cast<std::ptrdiff_t>(ripHead), toRoute.end()));
            } else {
                routeGroup(toRoute);
            }
        }
        const std::vector<int> noNets;
        for (int net : corridorMode ? noNets : order) {
            const auto& list = netPads[net];
            NearestPad nearest(ps, list);
            nearest.connect(0);
            std::vector<std::pair<int, size_t>> tree;
            padCells(ps[list[0]], tree);
            bool netFailed = false;
            for (size_t done = 1; done < list.size(); ++done) {
                size_t target = nearest.next();
                nearest.connect(target);
                ++stats.connections;
                if (connect(net, list, tree, ps[list[target]])) ++stats.routed;
                else {
                    netFailed = true;
                    failedConnections.push_back({net, list[target]});
                }
            }
            if (netFailed) {
                failedNets.push_back(net);
                stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
            }
            advance(1);
        }

        // Pour against the signal copper, then join each zone net's pads to its poured islands.
        if (!zoneOrder.empty()) {
            auto fills = fillZones(sch, ps, outT, outV);
            for (int net : zoneOrder) {
                const auto& list = netPads[net];
                DSU d = copperClusters(ps, outT, outV, &fills);
                const size_t base = ps.size() + outT.size() + outV.size();
                const size_t vbaseVias = outV.size();  // vias that existed when the clusters were computed
                // Their DSU items start after the pads and the tracks of that moment (connections made below add
                // tracks; indexing with the grown track count read past the DSU).
                const size_t vbase = ps.size() + outT.size();
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
                NearestPad nearest(ps, list);
                for (size_t a = 0; a < list.size(); ++a) {
                    connected[a] = joined.count(d.find(list[a])) > 0;
                    if (connected[a]) nearest.connect(a);
                }
                bool netFailed = false;
                stats.connections += static_cast<int>(list.size()) - 1;
                int pending = 0;
                for (size_t a = 0; a < list.size(); ++a) pending += !connected[a];
                stats.routed += static_cast<int>(list.size()) - 1 - pending;
                while (pending > 0) {
                    size_t target = nearest.next();
                    size_t cluster = d.find(list[target]);
                    std::vector<std::pair<int, size_t>> clusterTargets;
                    clusterCells(cluster, clusterTargets);
                    bool ok = connect(net, list, tree, ps[list[target]], &clusterTargets);
                    if (!ok) {
                        forcedConnect.insert(list[target]);
                        zoneFailed.push_back(list[target]);
                    }
                    // The whole pre-existing cluster (pads on the same island) joins with it.
                    for (size_t a = 0; a < list.size(); ++a) {
                        if (connected[a] || d.find(list[a]) != cluster) continue;
                        connected[a] = true;
                        nearest.connect(a);
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
        const bool improved = stats.failed < best.failed;
        if (improved) {
            best = stats;
            bestTracks = outT;
            bestVias = outV;
        }
        progressNow.unrouted = best.failed;
        if (stats.failed == 0) break;
        if (corridorMode) {
            // Targeted rip-up. Each unrouted connection searches again with other nets' routed copper passable at a
            // price; the failing nets and the nets that path crosses are ripped up, and the next pass routes them
            // again (failing nets first, completely, then the others) while every other net keeps its copper. The
            // cells and tiles those paths need get a history cost (negotiated congestion), so the re-routed nets keep
            // away from them. Passes that do not improve raise the aggression (a wider search, cheaper through routed
            // copper, and the nets alongside the path ripped too). Rip-up is a local search, so it runs as a few
            // strategies one after the other (kRipUpChains): each continues until kRipUpStall passes bring no
            // improvement, and the next starts again from the best pass. The best pass is kept; at most kRipUpRounds.
            ripStall = improved ? 0 : ripStall + 1;
            if (improved && failedConnections.empty() && forcedConnect.size() == forcedBefore && zoneFailed.empty()) break;
            if (++ripRound > kRipUpRounds) break;
            bool restart = false;
            if (ripStall >= kRipUpStall) {
                if (++ripChain >= static_cast<int>(sizeof kRipUpChains / sizeof kRipUpChains[0])) break;
                ripStall = 0;
                restart = true;
            }
            const RipUpChain& strategy = kRipUpChains[ripChain];
            {
                std::vector<int> failing;
                for (const FailedConnection& f : failedConnections)
                    if (std::find(failing.begin(), failing.end(), f.net) == failing.end()) failing.push_back(f.net);
                std::vector<int> head;  // failing nets in routing order
                for (int net : order)
                    if (std::find(failing.begin(), failing.end(), net) != failing.end()) head.push_back(net);
                const std::set<int> signal(order.begin(), order.end());
                if (tileHistory.empty()) tileHistory.assign(static_cast<size_t>(tiles.count()), 0);
                double maxExtra = 0;
                for (const auto& [net, x] : extraClearance) maxExtra = std::max(maxExtra, x);
                const double margin = clr + w / 2 + grid.pitch() + maxExtra;
                const double viaMargin = margin + settings.viaDiameter / 2;
                {
                    const int aggression = std::min(2, ripStall);
                    std::set<int> rip(failing.begin(), failing.end());
                    const float penalty = 24.0f / static_cast<float>(1 + aggression);
                    // Higher levels also rip the nets running alongside the path (a bus has to be re-ordered whole).
                    const double near = margin + strategy.alongside * aggression;
                    const double viaNear = viaMargin + strategy.alongside * aggression;
                    // What to repair: unrouted signal connections (around their global route) and pour pads that
                    // could not reach their pour (around the pad, from the pour's main island).
                    struct Repair {
                        int net;
                        size_t pad;
                        std::vector<int> corridor;
                        std::vector<std::pair<int, size_t>> from;
                    };
                    std::vector<Repair> repairs;
                    for (const FailedConnection& f : failedConnections) {
                        auto path = failedPaths.find({f.net, f.pad});
                        if (path == failedPaths.end()) continue;
                        Repair r{f.net, f.pad, routing::dilateTiles(tiles, path->second, 2 + aggression), {}};
                        for (size_t pi : netPads[f.net])
                            if (pi != f.pad) padCells(ps[pi], r.from);
                        repairs.push_back(std::move(r));
                    }
                    for (size_t pi : zoneFailed) {
                        auto pour = mainPour.find(ps[pi].net);
                        if (pour == mainPour.end() || pour->second.empty()) continue;
                        Repair r{ps[pi].net, pi, {}, pour->second};
                        const double reach = 6.0 + 2.0 * aggression;
                        const Vec2 at = ps[pi].position;
                        const int span = tiles.tile;
                        auto tileIndex = [&](double v, int count) {
                            return std::clamp(static_cast<int>(std::floor(v / grid.pitch())) / span, 0, count - 1);
                        };
                        for (int y = tileIndex(at.y - reach, tiles.rows); y <= tileIndex(at.y + reach, tiles.rows); ++y)
                            for (int x = tileIndex(at.x - reach, tiles.cols); x <= tileIndex(at.x + reach, tiles.cols); ++x)
                                r.corridor.push_back(tiles.id(x, y));
                        repairs.push_back(std::move(r));
                    }
                    for (const Repair& f : repairs) {
                        std::vector<std::pair<int, size_t>> to;
                        padCells(ps[f.pad], to);
                        const std::vector<std::pair<int, size_t>>& from = f.from;
                        ws.setCorridor(f.corridor);
                        ws.beginTargets();
                        for (auto [l, c] : to) ws.addTarget(static_cast<size_t>(l) * gridCells + c);
                        ws.beginSearch();
                        const int ti = static_cast<int>(std::lround(ps[f.pad].position.x / grid.pitch()));
                        const int tj = static_cast<int>(std::lround(ps[f.pad].position.y / grid.pitch()));
                        auto h = [&](size_t c) {  // admissible: every step costs at least one cell
                            return static_cast<float>(std::abs(static_cast<int>(c % static_cast<size_t>(grid.cols())) - ti) +
                                                      std::abs(static_cast<int>(c / static_cast<size_t>(grid.cols())) - tj));
                        };
                        using QE = std::pair<float, int>;
                        std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
                        for (auto [l, c] : from) {
                            if (!ws.allowed(c)) continue;
                            const size_t s0 = static_cast<size_t>(l) * gridCells + c;
                            ws.set(s0, 0.0f, -1);
                            open.push({h(c), static_cast<int>(s0)});
                        }
                        int hit = -1;
                        std::unordered_map<size_t, int> viaState;  // per cell: 0 no via, 1 via, 2 via once routed copper goes
                        while (!open.empty()) {
                            const int sn = open.top().second;
                            const float fq = open.top().first;
                            open.pop();
                            const float d = ws.cost(static_cast<size_t>(sn));
                            if (fq - h(static_cast<size_t>(sn) % gridCells) > d + 1e-3f) continue;  // stale
                            if (ws.isTarget(static_cast<size_t>(sn))) {
                                hit = sn;
                                break;
                            }
                            const int l = sn / static_cast<int>(gridCells);
                            const size_t c = static_cast<size_t>(sn) % gridCells;
                            const int i = static_cast<int>(c % static_cast<size_t>(grid.cols())), j = static_cast<int>(c / static_cast<size_t>(grid.cols()));
                            auto relax = [&](int nl, size_t nc, float step) {
                                if (!ws.allowed(nc) || grid.hardBlocked(nl, nc, f.net)) return;
                                if (!grid.passable(nl, nc, f.net)) step += penalty;
                                const size_t ns = static_cast<size_t>(nl) * gridCells + nc;
                                if (d + step < ws.cost(ns)) {
                                    ws.set(ns, d + step, sn);
                                    open.push({d + step + h(nc), static_cast<int>(ns)});
                                }
                            };
                            static const int di4[4] = {1, -1, 0, 0}, dj4[4] = {0, 0, 1, -1};
                            for (int q = 0; q < 4; ++q)
                                if (grid.inside(i + di4[q], j + dj4[q])) relax(l, grid.idx(i + di4[q], j + dj4[q]), 1.0f);
                            // A via where routed copper of other nets is in the way costs the penalty too (and those
                            // nets are ripped); one blocked by pads, holes or fixed copper is not an option.
                            if (!grid.noViaAt(c)) {
                                auto cached = viaState.find(c);
                                if (cached == viaState.end()) {
                                    const int state = grid.viaAllowed(c, f.net) ? 1 : grid.viaAllowedFixedOnly(c, f.net) ? 2 : 0;
                                    cached = viaState.emplace(c, state).first;
                                }
                                if (cached->second > 0)
                                    for (int ol = 0; ol < grid.layers(); ++ol)
                                        if (ol != l) relax(ol, c, cached->second == 1 ? 10.0f : 10.0f + penalty);
                            }
                        }
                        std::vector<int> blockers, needed;
                        for (int sn = hit; sn >= 0; sn = ws.parent(static_cast<size_t>(sn))) {
                            const int l = sn / static_cast<int>(gridCells);
                            const size_t c = static_cast<size_t>(sn) % gridCells;
                            needed.push_back(tiles.ofCell(static_cast<int>(c % static_cast<size_t>(grid.cols())),
                                                          static_cast<int>(c / static_cast<size_t>(grid.cols()))));
                            if (!grid.passable(l, c, f.net) || aggression > 0) grid.routedNetsNear(l, c, f.net, near, blockers);
                            const int prev = ws.parent(static_cast<size_t>(sn));
                            if (prev >= 0 && prev / static_cast<int>(gridCells) != l) {
                                auto vs = viaState.find(c);
                                if (vs != viaState.end() && vs->second == 2)
                                    for (int vl = 0; vl < grid.layers(); ++vl) grid.routedNetsNear(vl, c, f.net, viaNear, blockers);
                            }
                        }
                        int added = 0;
                        for (int b : blockers)
                            if (signal.count(b) && added < 24 * (1 + aggression) && rip.insert(b).second) ++added;
                        // Negotiated congestion (as the classic recovery): the cells along that path, widened by a track
                        // and its clearance, cost every other net more in the next passes.
                        if (!congestion) {
                            congestion = std::make_unique<Congestion>();
                            congestion->page(tiles, grid.cols(), grid.rows(), grid.layers());
                        }
                        {
                            const double wn = settings.widthFor(nets[static_cast<size_t>(f.net)].name);
                            const int k = static_cast<int>(std::ceil((wn / 2 + clr + w / 2) / grid.pitch()));
                            for (int sn = hit; sn >= 0; sn = ws.parent(static_cast<size_t>(sn))) {
                                const int l = sn / static_cast<int>(gridCells);
                                const size_t c = static_cast<size_t>(sn) % gridCells;
                                const int i = static_cast<int>(c % static_cast<size_t>(grid.cols())), j = static_cast<int>(c / static_cast<size_t>(grid.cols()));
                                for (int dj = -k; dj <= k; ++dj)
                                    for (int di = -k; di <= k; ++di)
                                        if (grid.inside(i + di, j + dj))
                                            congestion->add(static_cast<size_t>(l) * gridCells + grid.idx(i + di, j + dj), kHistoryStep, f.net);
                            }
                        }
                        // History: other nets' global routes keep away from these tiles.
                        std::sort(needed.begin(), needed.end());
                        needed.erase(std::unique(needed.begin(), needed.end()), needed.end());
                        for (int t : needed) tileHistory[static_cast<size_t>(t)] += 40;
                    }
                    // Buses: a failing two-pin net's neighbours with both ends within 6 mm of its ends compete for the
                    // same escape. They are ripped with it and re-routed first, all of them in their order across the
                    // bus, so they do not cross each other.
                    std::vector<int> ripList;
                    for (int fnet : head) {
                        if (std::find(ripList.begin(), ripList.end(), fnet) != ripList.end()) continue;
                        const auto& fp = netPads[fnet];
                        std::vector<int> group{fnet};
                        if (fp.size() == 2 && strategy.buses) {
                            const Vec2 a = ps[fp[0]].position, b = ps[fp[1]].position;
                            for (int net : order) {
                                const auto& q = netPads[net];
                                if (net == fnet || q.size() != 2) continue;
                                const Vec2 c = ps[q[0]].position, d = ps[q[1]].position;
                                const bool along = (c - a).length() < 6.0 && (d - b).length() < 6.0;
                                const bool across = (d - a).length() < 6.0 && (c - b).length() < 6.0;
                                if ((along || across) && std::find(ripList.begin(), ripList.end(), net) == ripList.end())
                                    group.push_back(net);
                            }
                            // Order across the bus: by the ends' offset perpendicular to the bus direction.
                            const Vec2 dir = b - a;
                            const double len = std::max(1e-9, dir.length());
                            auto offset = [&](int net) {
                                const auto& q = netPads[net];
                                Vec2 c = ps[q[0]].position;
                                if ((ps[q[1]].position - a).length() < (c - a).length()) c = ps[q[1]].position;
                                return ((c - a).x * -dir.y + (c - a).y * dir.x) / len;
                            };
                            std::stable_sort(group.begin(), group.end(), [&](int x, int y) { return offset(x) < offset(y) - 1e-6; });
                        }
                        for (int net : group) {
                            ripList.push_back(net);
                            rip.insert(net);
                        }
                    }
                    ripHead = ripList.size();
                    for (int net : order)
                        if (rip.count(net) && std::find(ripList.begin(), ripList.end(), net) == ripList.end()) ripList.push_back(net);
                    ripOrder = std::move(ripList);
                }
                ripPriority = std::set<int>(failing.begin(), failing.end());
            }
            if (improved) {
                bestCopper = curCopper;
                bestRip = ripOrder;
                bestRipHead = ripHead;
            }
            if (restart) {
                prevCopper = bestCopper;
                ripOrder = bestRip;
                ripHead = bestRipHead;
            } else if (!improved && strategy.wholeBoard && ripStall % 3 == 0) {
                // Every third pass without improvement: the whole board again in its original order, every net paying
                // the history the failures have built up (negotiated congestion, as the classic recovery).
                ripOrder = order;
                ripHead = 0;
            } else if (!improved && strategy.revert && stats.failed > best.failed + 2) {
                // Wandered off: start again from the best pass (the history added since makes it go differently).
                prevCopper = bestCopper;
                ripOrder = bestRip;
                ripHead = bestRipHead;
            } else {
                prevCopper = std::move(curCopper);
            }
            continue;
        }
        // Rip-up and retry with the failing signal nets promoted to the front (zone nets always connect last).
        std::vector<int> next;
        for (int n : failedNets)
            if (std::find(order.begin(), order.end(), n) != order.end()) next.push_back(n);
        for (int n : order)
            if (std::find(next.begin(), next.end(), n) == next.end()) next.push_back(n);
        if (!recovery) {
            const bool stuck = next == order && pass > 0 && forcedConnect.size() == forcedBefore;
            if (!stuck) order = next;
            if (!stuck && pass + 1 < kRipUpPasses) continue;
            recovery = true;  // the rip-up passes are done: recover what they left
        } else {
            // Negotiation keeps the original order: the history costs, not promotion, make room for what failed.
            order = stableOrder;
            recoveryStall = improved ? 0 : recoveryStall + 1;
            if (++recoveryPass >= kRecoveryPasses || recoveryStall >= kRecoveryStall) break;
        }
        if (failedConnections.empty()) break;  // nothing a corridor can help (zone nets only)
        // The unobstructed grid: only pads, fences and the fixed fan-out copper.
        RoutingGrid unobstructed(settings);
        prepareGrid(unobstructed);
        for (size_t t = 0; t < fixedTracks; ++t) markFixedTrack(unobstructed, outT[t]);
        for (size_t v = 0; v < fixedVias; ++v) markFixedVia(unobstructed, outV[v]);
        // Negotiated congestion: each unrouted connection's unobstructed path raises the cost of its corridor (one
        // track plus clearance either side) for every other net.
        if (!congestion) {
            congestion = std::make_unique<Congestion>();
            congestion->cost.assign(static_cast<size_t>(grid.layers()) * gridCells, 0.0f);
            congestion->net.assign(static_cast<size_t>(grid.layers()) * gridCells, -2);
        }
        for (const FailedConnection& f : failedConnections) {
            std::vector<std::pair<int, size_t>> from, to;
            for (size_t pi : netPads[f.net])
                if (pi != f.pad) padCells(ps[pi], from);
            padCells(ps[f.pad], to);
            ws.beginTargets();
            for (auto [l, c] : to) ws.addTarget(static_cast<size_t>(l) * gridCells + c);
            const double wn = settings.widthFor(nets[static_cast<size_t>(f.net)].name);
            neckZones(ws, netPads[f.net], wn);
            const RouteResult rr = astar(unobstructed, f.net, from, ws, ps[f.pad].position, passViaCost,
                                         wn > w + 1e-9 ? wn / 2 : 0.0, true);
            if (!rr.ok) continue;
            const int k = static_cast<int>(std::ceil((wn / 2 + clr + w / 2) / grid.pitch()));
            for (const PathNode& node : rr.path)
                for (int dj = -k; dj <= k; ++dj)
                    for (int di = -k; di <= k; ++di) {
                        if (!grid.inside(node.i + di, node.j + dj)) continue;
                        const size_t at = static_cast<size_t>(node.layer) * gridCells + grid.idx(node.i + di, node.j + dj);
                        congestion->cost[at] += kHistoryStep;
                        int& owner = congestion->net[at];
                        owner = owner == -2 || owner == f.net ? f.net : -1;
                    }
        }
    }

    progressNow.phase = RouteProgress::Finishing;
    report();  // the last point a route can be cancelled: from here on the board's copper is replaced
    tracks.clear();
    vias.clear();
    neckDownWith(bestTracks, ps, neckWidths, padsByNet, settings.clearance);
    for (auto& t : bestTracks) addTrack(t);
    for (auto& v : bestVias) addVia(v);
    cleanupRouting(sch);
    // HDI: blind / buried / microvias cut to the layers each via connects.
    if (settings.hdi) applyHdiVias(sch);
    // Length / phase matching: serpentines on the short members of differential pairs and buses.
    if (settings.lengthTuning) best.lengthTuned = tuneLengths(*this, sch);
    if (best.failed == std::numeric_limits<int>::max()) best.failed = 0;
    return best;
}

const char* viaKind(const Via& v, int layerCount) {
    const int last = v.lastLayer(layerCount);
    if (v.fromLayer <= 0 && last >= layerCount - 1) return "through";
    if (last - v.fromLayer == 1 && v.drill <= 0.15 + 1e-9) return "microvia";
    if (v.fromLayer == 0 || last == layerCount - 1) return "blind";
    return "buried";
}

int PcbLayout::applyHdiVias(const Schematic& sch) {
    const int n = settings.layerCount;
    if (n < 4) return 0;
    const auto ps = pads(sch);
    const std::vector<ZoneFill> fills = zoneFills(sch);
    const double laserRing = std::min(settings.minAnnularRing, 0.075);
    int changed = 0;
    for (auto& v : vias) {
        int first = n, last = -1;
        auto use = [&](int layer) {
            first = std::min(first, layer);
            last = std::max(last, layer);
        };
        for (const auto& t : tracks)
            if (t.net == v.net && pointSegmentDistance(v.position, t.a, t.b) <= v.diameter / 2 + 1e-6) use(t.layer);
        for (const auto& p : ps)
            if (p.net == v.net && padDistance(p, v.position) <= v.diameter / 2 - 1e-6) {
                if (p.throughHole) {
                    use(0);
                    use(n - 1);
                } else {
                    use(p.smdLayer);
                }
            }
        for (const auto& f : fills)
            if (f.net == v.net && f.islandNear(v.position, v.diameter / 2) >= 0) use(f.layer);
        if (last <= first) continue;  // joins a single layer (or nothing): leave it for DRC to report
        Via nv = v;
        nv.fromLayer = first;
        nv.toLayer = last >= n - 1 ? -1 : last;
        if (last - first == 1) {  // one dielectric: laser microvia (IPC-2226 Type I/II)
            nv.drill = settings.microviaDrill;
            nv.diameter = std::min(v.diameter, std::max(settings.microviaDiameter, nv.drill + 2 * laserRing));
        }
        if (nv.fromLayer != v.fromLayer || nv.toLayer != v.toLayer || nv.drill != v.drill || nv.diameter != v.diameter) {
            v = nv;
            ++changed;
        }
    }
    return changed;
}

int PcbLayout::cleanupRouting(const Schematic& sch) {
    const auto ps = pads(sch);
    const double clr = settings.clearance;
    int changes = 0;
    auto key = [](int layer, Vec2 p) {
        return std::make_tuple(layer, std::llround(p.x * 1e4), std::llround(p.y * 1e4));
    };
    // Pads and vias do not change here; tracks are indexed at the start of each pass (see mergedInto below).
    const RectIndex padIx(padBoxes(ps)), viaIx(viaBoxes(vias));
    double widestTrack = 0;
    for (const auto& t : tracks) widestTrack = std::max(widestTrack, t.width);
    std::unique_ptr<RectIndex> trackIx;
    std::vector<size_t> mergedInto;  // per track of this pass: the track it was merged into (a longer one), or none
    std::vector<size_t> nearTracks;
    // A joint can be reshaped only where two pieces of one track meet in open board: no pad or via there.
    auto anchored = [&](int layer, Vec2 p) {
        const Rect at = Rect::centered(p, 4e-6, 4e-6);
        for (size_t pi : padIx.query(at))
            if (ps[pi].onLayer(layer) && padDistance(ps[pi], p) <= 1e-6) return true;
        for (size_t vi : viaIx.query(at))
            if ((vias[vi].position - p).length() <= 1e-6) return true;
        return false;
    };
    // Clearance of a new piece of copper (segment a–b, width w, net) to everything of other nets on its layer.
    auto clear = [&](int layer, int net, Vec2 a, Vec2 b, double w, size_t skipA, size_t skipB) {
        const Rect reach = segmentBox(a, b, w / 2 + clr + 1e-3);
        // A track that absorbed another one this pass now also spans that one's rectangle.
        nearTracks.clear();
        for (size_t i : trackIx->query(reach.inflated(widestTrack / 2))) {
            nearTracks.push_back(i);
            if (mergedInto[i] != std::numeric_limits<size_t>::max()) nearTracks.push_back(mergedInto[i]);
        }
        for (size_t i : nearTracks) {
            const Track& t = tracks[i];
            if (i == skipA || i == skipB || t.layer != layer || t.net == net) continue;
            if (segmentSegmentDistance(a, b, t.a, t.b) - (w + t.width) / 2 < clr - 1e-6) return false;
        }
        for (size_t pi : padIx.query(reach)) {
            const Pad& pd = ps[pi];
            if (!pd.onLayer(layer) || (pd.net == net && net >= 0)) continue;
            double d = pd.round ? pointSegmentDistance(pd.position, a, b) - std::min(pd.size.x, pd.size.y) / 2
                                : segmentRectDistance(a, b, pd.bounds());
            if (d - w / 2 < clr - 1e-6) return false;
        }
        for (size_t vi : viaIx.query(reach)) {
            const Via& v = vias[vi];
            if (v.net == net || !v.spans(layer)) continue;
            if (pointSegmentDistance(v.position, a, b) - v.diameter / 2 - w / 2 < clr - 1e-6) return false;
        }
        Vec2 mid = (a + b) * 0.5;
        return settings.edgeDistance(mid) >= settings.edgeClearance + w / 2 - 1e-6;
    };

    for (int pass = 0; pass < 4; ++pass) {
        trackIx = std::make_unique<RectIndex>(trackBoxes(tracks));
        mergedInto.assign(tracks.size(), std::numeric_limits<size_t>::max());
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
                mergedInto[j] = i;
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
    const auto byComponent = groupPads(ps, false);
    for (size_t i = 0; i < ps.size(); ++i) {
        const Pad& p = ps[i];
        double minor = std::min(p.size.x, p.size.y);
        double gap = std::numeric_limits<double>::max();
        for (size_t j : byComponent.at(p.componentId)) {
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
    neckDownWith(out, ps, padNeckWidths(ps), groupPads(ps, true), settings.clearance);
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
        // A pin left open (one-pin net) has no defined voltage: a simulated floating node says nothing about it.
        if (sch.nets()[static_cast<size_t>(a)].pins.size() < 2 || sch.nets()[static_cast<size_t>(b)].pins.size() < 2)
            return std::make_pair(0.0, 0.0);
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
        // A net class from the schematic asks for more than the board's rule.
        double need = clr;
        if (!settings.netClearances.empty()) {
            const auto& ns = sch.nets();
            for (int n : {netA, netB})
                if (n >= 0 && n < static_cast<int>(ns.size())) need = std::max(need, settings.clearanceFor(ns[static_cast<size_t>(n)].name));
        }
        if (d >= need - eps) return;
        char buf[96];
        if (d <= 0) {
            add(Severity::Error, "DRC_SHORT", what + " — copper overlaps (short circuit).", loc, std::move(comps));
        } else if (d < fabClr - eps) {
            std::snprintf(buf, sizeof buf, " (fabrication minimum %.3f mm)", fabClr);
            add(Severity::Error, "DRC_CLEARANCE", what + buf + ".", loc, std::move(comps));
        } else if (d >= clr - eps) {
            std::snprintf(buf, sizeof buf, " (net class %.3f mm)", need);
            add(Severity::Warning, "DRC_NET_CLASS_CLEARANCE", what + buf + ".", loc, std::move(comps));
        } else {
            std::snprintf(buf, sizeof buf, " (design rule %.3f mm)", clr);
            add(Severity::Warning, "DRC_CLEARANCE_RULE", what + buf + ".", loc, std::move(comps));
        }
    };
    const auto& nets = sch.nets();
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(n)].name : std::string("(none)"); };
    // Spatial index reach: no spacing check can fire between copper further apart than the largest spacing any of
    // them applies (design clearance, the etch limit, or the IPC-2221 spacing for the board's largest voltage
    // difference; the table only grows with voltage), so only pairs within that distance are examined.
    double reach = std::max(clr, 0.1);
    for (const auto& [name, c] : settings.netClearances) reach = std::max(reach, c);
    if (!netRange.empty()) {
        double lo = std::numeric_limits<double>::max(), hi = -std::numeric_limits<double>::max();
        for (const auto& [net, range] : netRange) {
            lo = std::min({lo, range.first, range.second});
            hi = std::max({hi, range.first, range.second});
        }
        if (hi >= lo) reach = std::max(reach, ipc2221Clearance(hi - lo, settings.highAltitude, settings.coated()));
    }
    reach += 0.01;  // beyond every tolerance (eps) the checks use
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
    std::vector<Rect> courtyards;
    for (const Component* c : placed) courtyards.push_back(courtyard(*c));
    const RectIndex courtyardIx(courtyards);
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j : courtyardIx.query(courtyards[i], static_cast<long long>(i)))
            if (mountPlane(*placed[i], settings) == mountPlane(*placed[j], settings) &&
                courtyard(*placed[i]).intersects(courtyard(*placed[j])))
                add(Severity::Warning, "DRC_COURTYARD_OVERLAP",
                    "Courtyards of " + placed[i]->ref + " and " + placed[j]->ref + " overlap.",
                    courtyard(*placed[i]).center(), {placed[i]->id, placed[j]->id});

    auto ps = pads(sch);
    // Spacing inside one footprint is fixed by the package (0.5 mm-pitch QFN pads are ~0.2 mm apart): it is held to
    // the fabrication minimum only, not to the board's design clearance.
    constexpr double kMinEtchGap = 0.1;  // absolute etching limit, whatever the preset
    std::set<int> finePitchNoted;
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
        else if (d < kMinEtchGap - eps) {
            char b[96];
            std::snprintf(b, sizeof b, " (below the %.2f mm any fab can etch)", kMinEtchGap);
            add(Severity::Error, "DRC_CLEARANCE", what + b + ".", loc, std::move(comps));
        } else if (d < fabClr - eps && !finePitchNoted.count(comps.empty() ? -1 : comps.front())) {
            // The part's own land pattern (a 0.5 mm-pitch QFN has ≈ 0.22 mm gaps) is set by its datasheet: below the
            // preset's minimum it is a capability question for the fab, not a layout error.
            finePitchNoted.insert(comps.empty() ? -1 : comps.front());
            char b[200];
            std::snprintf(b, sizeof b, " inside one footprint: its land pattern is below the %.2f mm preset minimum — "
                          "confirm the fab's fine-pitch capability (or use a part with a coarser pitch).", fabClr);
            add(Severity::Info, "DRC_FINE_PITCH_PADS", what + b, loc, std::move(comps));
        }
    };
    // Spatial indexes of the copper (candidates come back in index order, so every check below runs in the order of
    // a full pairwise scan and reports exactly what it would).
    const RectIndex padIx(padBoxes(ps)), trackIx(trackBoxes(tracks)), viaIx(viaBoxes(vias));
    const auto padsOfComponent = groupPads(ps, false);
    double widestTrack = 0, widestVia = 0;
    for (const auto& t : tracks) widestTrack = std::max(widestTrack, t.width);
    for (const auto& v : vias) widestVia = std::max(widestVia, v.diameter);
    // Pad ↔ pad.
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j : padIx.query(ps[i].bounds().inflated(reach), static_cast<long long>(i))) {
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
        const Rect trackReach = segmentBox(tr.a, tr.b, tr.width / 2 + reach);
        for (size_t pi : padIx.query(trackReach)) {
            const Pad& p = ps[pi];
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
            for (size_t oi : padsOfComponent.at(p.componentId)) {
                const Pad& own = ps[oi];
                if (own.net == tr.net && own.componentId == p.componentId && own.onLayer(tr.layer) &&
                    padDistance(own, closest) <= 0)
                    inOwnPad = true;
            }
            if (inOwnPad) {
                footprintCheck(d, "Track (" + netName(tr.net) + ") to pad (" + netName(p.net) + ") clearance " +
                                      fmt(std::max(0.0, d)), p.position, {p.componentId}, tr.net, p.net);
                continue;
            }
            clearanceCheck(d, "Track (" + netName(tr.net) + ") to pad (" + netName(p.net) + ") clearance " + fmt(std::max(0.0, d)), p.position, {p.componentId}, tr.net, p.net);
        }
        for (size_t u : trackIx.query(trackReach, static_cast<long long>(t))) {
            const Track& o = tracks[u];
            if (o.net == tr.net || o.layer != tr.layer) continue;
            double d = segmentSegmentDistance(tr.a, tr.b, o.a, o.b) - (tr.width + o.width) / 2;
            clearanceCheck(d, "Track clearance " + fmt(std::max(0.0, d)) + " between " + netName(tr.net) + " and " + netName(o.net), (tr.a + tr.b) * 0.5, {}, tr.net, o.net);
        }
        for (size_t vi : viaIx.query(trackReach)) {
            const Via& v = vias[vi];
            if (v.net == tr.net || !v.spans(tr.layer)) continue;
            double d = pointSegmentDistance(v.position, tr.a, tr.b) - tr.width / 2 - v.diameter / 2;
            clearanceCheck(d, "Via (" + netName(v.net) + ") to track (" + netName(tr.net) + ") clearance " + fmt(std::max(0.0, d)), v.position, {}, v.net, tr.net);
        }
    }
    for (size_t i = 0; i < vias.size(); ++i) {
        const Rect viaReach = Rect::centered(vias[i].position, vias[i].diameter, vias[i].diameter).inflated(reach);
        for (size_t pi : padIx.query(viaReach)) {
            const Pad& p = ps[pi];
            if (p.net == vias[i].net || !(p.throughHole || vias[i].spans(p.smdLayer))) continue;
            double d = padDistance(p, vias[i].position) - vias[i].diameter / 2;
            clearanceCheck(d, "Via (" + netName(vias[i].net) + ") to pad (" + netName(p.net) + ") clearance " + fmt(std::max(0.0, d)), vias[i].position, {p.componentId}, vias[i].net, p.net);
        }
        for (size_t j : viaIx.query(viaReach, static_cast<long long>(i))) {
            if (vias[i].net == vias[j].net || !vias[i].overlaps(vias[j])) continue;
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
        // Laser microvias have their own (smaller) drill and annular ring limits (IPC-2226).
        const bool laser = std::string(viaKind(v, settings.layerCount)) == "microvia";
        if (!laser && v.drill < settings.minDrill - eps) {
            std::snprintf(buf, sizeof buf, "Via drill %.3f mm is below the minimum %.3f mm.", v.drill, settings.minDrill);
            add(Severity::Error, "DRC_DRILL_SIZE", buf, v.position);
        }
        double ring = (v.diameter - v.drill) / 2;
        const double minRing = laser ? std::min(settings.minAnnularRing, 0.075) : settings.minAnnularRing;
        if (ring < minRing - eps) {
            std::snprintf(buf, sizeof buf, "Via annular ring %.3f mm is below the minimum %.3f mm.", ring, minRing);
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
    std::vector<Rect> holeBoxes;
    for (const auto& h : holes) holeBoxes.push_back(Rect::centered(h.at, h.drill, h.drill));
    const RectIndex holeIx(holeBoxes);
    for (size_t i = 0; i < holes.size(); ++i)
        for (size_t j : holeIx.query(holeBoxes[i].inflated(settings.minHoleToHole + 0.01), static_cast<long long>(i))) {
            double gap = (holes[i].at - holes[j].at).length() - (holes[i].drill + holes[j].drill) / 2;
            if (gap < settings.minHoleToHole - eps) {
                std::snprintf(buf, sizeof buf, "%s-to-%s hole spacing %.3f mm is below the minimum %.3f mm.",
                              holes[i].what.c_str(), holes[j].what.c_str(), std::max(0.0, gap), settings.minHoleToHole);
                add(Severity::Error, "DRC_HOLE_SPACING", buf, (holes[i].at + holes[j].at) * 0.5);
            }
        }

    // Vias inside SMD pads wick solder away from the joint — unless they are filled and plated over (VIPPO).
    for (const auto& v : vias)
        for (size_t pi : padIx.query(Rect::centered(v.position, 1e-6, 1e-6))) {
            const Pad& p = ps[pi];
            if (!settings.viaInPad && !p.throughHole && p.net == v.net && v.spans(p.smdLayer) &&
                padDistance(p, v.position) <= 0) {
                add(Severity::Warning, "DRC_VIA_IN_PAD",
                    "Via inside an SMD pad on " + netName(v.net) +
                        " — tent/plug it, move it off the pad, or order via-in-pad plated over (VIPPO, Board Setup → HDI).", v.position,
                    {p.componentId});
                break;
            }
        }

    // Track geometry: dangling ends and acute (< 90°) joins that trap etchant.
    const auto& fills = zoneFills(sch);
    auto touchesCopper = [&](size_t self, Vec2 end) {
        const Track& t = tracks[self];
        for (const auto& f : fills)
            if (f.net == t.net && f.layer == t.layer && f.islandNear(end, t.width / 2) >= 0) return true;
        const Rect near = Rect::centered(end, 0, 0).inflated(t.width / 2 + 1e-6);
        for (size_t pi : padIx.query(near)) {
            const Pad& p = ps[pi];
            if (p.net == t.net && p.onLayer(t.layer) && padDistance(p, end) <= t.width / 2) return true;
        }
        for (size_t vi : viaIx.query(Rect::centered(end, 1e-6, 1e-6))) {
            const Via& v = vias[vi];
            if (v.net == t.net && (v.position - end).length() <= v.diameter / 2) return true;
        }
        for (size_t k : trackIx.query(Rect::centered(end, 0, 0).inflated((widestTrack + t.width) / 4 + 1e-6))) {
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
        for (size_t u : trackIx.query(segmentBox(tracks[t].a, tracks[t].b, 1e-5), static_cast<long long>(t))) {
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
                    const Rect at = Rect::centered(pa, 1e-6, 1e-6);
                    for (size_t pi : padIx.query(at))
                        if (ps[pi].net == a.net && ps[pi].onLayer(a.layer) && padDistance(ps[pi], pa) <= 0) covered = true;
                    for (size_t vi : viaIx.query(at))
                        if (vias[vi].net == a.net && (vias[vi].position - pa).length() <= vias[vi].diameter / 2) covered = true;
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

    // Isolation barrier: copper of different galvanic domains at least isolationGap apart on every layer. A barrier
    // part's own pads are rated by its datasheet; their exit corridors are exempt the way the router leaves them.
    if (settings.isolationGap > 0) {
        const GalvanicDomains doms = galvanicDomains(sch);
        const double gap = settings.isolationGap;
        struct Item {
            int net, dom, comp = -1;
            int layer;      // -1 = every layer (via)
            const Pad* pad = nullptr;
            Vec2 a, b;
            double half = 0;
        };
        std::vector<Item> items;
        std::map<int, std::vector<const Pad*>> compPads;
        for (const auto& p : ps) {
            const int d = doms.domainOfNet(p.net);
            if (d < 0) continue;
            compPads[p.componentId].push_back(&p);
            for (int l = 0; l < settings.layerCount; ++l)
                if (p.onLayer(l)) items.push_back({p.net, d, p.componentId, l, &p, p.position, p.position, 0});
        }
        for (const auto& t : tracks)
            if (const int d = doms.domainOfNet(t.net); d >= 0) items.push_back({t.net, d, -1, t.layer, nullptr, t.a, t.b, t.width / 2});
        for (const auto& v : vias)
            if (const int d = doms.domainOfNet(v.net); d >= 0)
                items.push_back({v.net, d, -1, -1, nullptr, v.position, v.position, v.diameter / 2});
        std::map<int, double> exitR;
        for (const auto& [comp, list] : compPads) {
            double sMin = 1e9;
            for (size_t i = 0; i < list.size(); ++i)
                for (size_t j = i + 1; j < list.size(); ++j)
                    if (doms.domainOfNet(list[i]->net) != doms.domainOfNet(list[j]->net))
                        sMin = std::min(sMin, rectRectDistance(list[i]->bounds(), list[j]->bounds()));
            if (sMin < gap - 1e-9) exitR[comp] = gap - sMin + settings.trackWidth + settings.clearance;
        }
        auto dist = [&](const Item& x, const Item& y) {
            if (x.pad && y.pad) return rectRectDistance(x.pad->bounds(), y.pad->bounds());
            if (x.pad) return segmentRectDistance(y.a, y.b, x.pad->bounds()) - y.half;
            if (y.pad) return segmentRectDistance(x.a, x.b, y.pad->bounds()) - x.half;
            return segmentSegmentDistance(x.a, x.b, y.a, y.b) - x.half - y.half;
        };
        // Within a barrier part's exit corridor: `y` sits next to a pad of barrier `x.comp` in its own domain.
        auto exempt = [&](const Item& x, const Item& y) {
            auto it = exitR.find(x.comp);
            if (it == exitR.end()) return false;
            for (const Pad* q : compPads[x.comp]) {
                if (doms.domainOfNet(q->net) != y.dom) continue;
                const double d = y.pad ? rectRectDistance(q->bounds(), y.pad->bounds())
                                       : segmentRectDistance(y.a, y.b, q->bounds()) - y.half;
                if (d < it->second) return true;
            }
            return false;
        };
        int reported = 0;
        std::vector<Rect> itemBoxes;
        for (const Item& x : items) itemBoxes.push_back(x.pad ? x.pad->bounds() : Rect(x.a.x, x.a.y, x.b.x, x.b.y).inflated(x.half));
        const RectIndex itemIx(itemBoxes);
        for (size_t i = 0; i < items.size() && reported < 5; ++i)
            for (size_t j : itemIx.query(itemBoxes[i].inflated(gap + 1e-6), static_cast<long long>(i))) {
                if (reported >= 5) break;
                const Item &x = items[i], &y = items[j];
                if (x.dom == y.dom) continue;
                if (x.layer >= 0 && y.layer >= 0 && x.layer != y.layer) continue;
                if (x.comp >= 0 && x.comp == y.comp) continue;  // inside one barrier part: its datasheet rating
                const Rect bx = x.pad ? x.pad->bounds() : Rect(x.a.x, x.a.y, x.b.x, x.b.y).inflated(x.half);
                const Rect by = y.pad ? y.pad->bounds() : Rect(y.a.x, y.a.y, y.b.x, y.b.y).inflated(y.half);
                if (!bx.inflated(gap).intersects(by)) continue;
                const double d = dist(x, y);
                if (d >= gap - 1e-6 || exempt(x, y) || exempt(y, x)) continue;
                char msg[200];
                std::snprintf(msg, sizeof msg, "%s and %s are %.2f mm apart across the isolation barrier (needs %.1f mm",
                              netName(x.net).c_str(), netName(y.net).c_str(), std::max(0.0, d), gap);
                add(Severity::Error, "DRC_ISOLATION_GAP",
                    std::string(msg) + " creepage / clearance between galvanic domains).", (x.a + y.a) * 0.5);
                ++reported;
            }
    }

    // Tamper meshes: laid out, intact over the whole secure area, and never crossed or drilled by other copper.
    for (const auto& m : tamperMeshGeometry(sch, ps)) {
        const TamperMesh& tm = tamperMeshes[static_cast<size_t>(m.mesh)];
        const Component* se = sch.findByRef(tm.componentRef);
        std::vector<int> comps;
        if (se) comps.push_back(se->id);
        const Vec2 at = se ? se->pcb.position : Vec2{};
        if (!m.error.empty()) {
            add(Severity::Error, "DRC_TAMPER_MESH", "Tamper mesh over " + tm.componentRef + ": " + m.error + ".", at, comps);
            continue;
        }
        if (tracks.empty()) continue;  // not routed yet (DRC_UNROUTED reports it)
        // Every stripe must be present on its layer (a missing one leaves a hole a probe can reach through).
        size_t missing = 0;
        for (const Track& want : m.tracks) {
            bool found = false;
            for (const Track& t : tracks)
                if (t.net == want.net && t.layer == want.layer &&
                    pointSegmentDistance((want.a + want.b) * 0.5, t.a, t.b) < 1e-3 + t.width / 2) {  // corners may be chamfered
                    found = true;
                    break;
                }
            if (!found) ++missing;
        }
        if (missing > 0)
            add(Severity::Error, "DRC_TAMPER_MESH",
                "Tamper mesh over " + tm.componentRef + " is incomplete: " + std::to_string(missing) +
                    " stripe(s) are missing. Re-run the autorouter to lay the mesh.",
                at, comps);
        const Rect area = m.region;
        for (const Via& v : vias)
            if (v.net != m.netA && v.net != m.netB && area.contains(v.position) &&
                (v.spans(tm.layerA) || v.spans(tm.layerB))) {
                add(Severity::Error, "DRC_TAMPER_MESH_BREACH",
                    "Via of " + netName(v.net) + " is drilled through the tamper-mesh area of " + tm.componentRef +
                        ": a probe could follow it to the secure element without cutting the mesh.",
                    v.position, comps);
                break;
            }
        for (const Track& t : tracks)
            if ((t.layer == tm.layerA || t.layer == tm.layerB) && t.net != m.netA && t.net != m.netB &&
                segmentRectDistance(t.a, t.b, area) <= 0) {
                add(Severity::Error, "DRC_TAMPER_MESH_BREACH",
                    netName(t.net) + " runs through the tamper-mesh layer of " + tm.componentRef + " (" +
                        copperLayerName(t.layer, settings.layerCount) + "), leaving a gap in the mesh.",
                    t.a, comps);
                break;
            }
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
