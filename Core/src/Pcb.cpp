#include "sieda/Pcb.hpp"

#include "sieda/Simulator.hpp"

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <map>
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

double ipc2221Clearance(double volts, bool highAltitude) {
    double v = std::fabs(volts);
    if (highAltitude) {  // B3
        if (v <= 30) return 0.1;
        if (v <= 50) return 0.6;
        if (v <= 100) return 1.5;
        if (v <= 170) return 3.2;
        if (v <= 250) return 6.4;
        if (v <= 500) return 12.5;
        return 12.5 + (v - 500) * 0.025;
    }
    // B2
    if (v <= 30) return 0.1;
    if (v <= 150) return 0.6;
    if (v <= 300) return 1.25;
    if (v <= 500) return 2.5;
    return 2.5 + (v - 500) * 0.005;
}

std::string copperLayerName(int layer, int layerCount) {
    if (layer == 0) return "Top";
    if (layerCount > 1 && layer == layerCount - 1) return "Bottom";
    return "Inner " + std::to_string(layer);
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

    const double margin = 1.2;   // spacing between courtyards, leaves routing channels
    const double step = 0.5;
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
                for (double y = e + hh; y <= settings.height - e - hh + 1e-9; y += step) {
                    for (double x = e + hw; x <= settings.width - e - hw + 1e-9; x += step) {
                        Rect cy = Rect::centered({x, y}, cy0.width(), cy0.height()).inflated(margin / 2);
                        bool clash = false;
                        for (const auto& o : comps) {
                            if (o.id == c.id || !o.hasFootprint() || !o.pcb.placed) continue;
                            if (courtyard(o).inflated(margin / 2).intersects(cy)) { clash = true; break; }
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
            } else {
                settings.width += 10;
                settings.height += 6;
            }
        }
        if (!done) {  // pathological — drop it at the centre so the user can move it
            c.pcb.position = {settings.width / 2, settings.height / 2};
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

/// Union-find over pads + tracks + vias by physical contact. Item order: pads, tracks, vias.
DSU copperClusters(const std::vector<Pad>& pads, const std::vector<Track>& tracks, const std::vector<Via>& vias) {
    size_t np = pads.size(), nt = tracks.size(), nv = vias.size();
    DSU d(np + nt + nv);
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
    DSU d = copperClusters(ps, tracks, vias);
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
        double e = s.edgeClearance + s.trackWidth / 2;
        for (int j = 0; j < rows_; ++j)
            for (int i = 0; i < cols_; ++i) {
                Vec2 p = pos(i, j);
                if (p.x < e || p.y < e || p.x > s.width - e || p.y > s.height - e)
                    for (int l = 0; l < layers_; ++l) owner_[L(l)][idx(i, j)] = -2;
            }
    }

    int layers() const { return layers_; }
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
        // suggest: check other-net pads exactly.
        Vec2 at = pos(ci, cj);
        double need = s_.viaDiameter / 2 + s_.clearance - 1e-9;
        for (const Pad& p : pads_) {
            if (p.net == net && net >= 0) continue;
            Rect b = p.bounds();
            if (at.x < b.x0 - need || at.x > b.x1 + need || at.y < b.y0 - need || at.y > b.y1 + need) continue;
            if (padDistance(p, at) < need) return false;
        }
        return true;
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
        for (int l = 0; l < layers_; ++l) {
            if (!p.onLayer(l)) continue;
            bool anyCore = false;
            forRectNear(r, keepout, p, [&](size_t c, double dist) {
                if (p.net >= 0) claim(l, c, p.net);
                else claim(l, c, -3);  // unconnected pad: blocks every net
                if (dist <= 0) {
                    if (!p.throughHole) noVia_[c] = 1;
                    copper_[L(l)][c] = p.net >= 0 ? p.net : -3;
                    if (p.net >= 0) { padNet_[L(l)][c] = p.net; anyCore = true; }
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
    std::vector<size_t> padCoreCells(const Pad& p) const {
        std::vector<size_t> out;
        Rect r = p.bounds();
        int i0 = std::max(0, static_cast<int>(std::ceil(r.x0 / g_))), i1 = std::min(cols_ - 1, static_cast<int>(std::floor(r.x1 / g_)));
        int j0 = std::max(0, static_cast<int>(std::ceil(r.y0 / g_))), j1 = std::min(rows_ - 1, static_cast<int>(std::floor(r.y1 / g_)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i)
                if (padDistance(p, pos(i, j)) <= 0) out.push_back(idx(i, j));
        if (out.empty()) {
            int i = static_cast<int>(std::lround(p.position.x / g_)), j = static_cast<int>(std::lround(p.position.y / g_));
            if (inside(i, j)) out.push_back(idx(i, j));
        }
        return out;
    }

private:
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
    std::vector<Pad> pads_;                 // every marked pad, for exact via clearance
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
                  const std::vector<std::vector<char>>& targetMask, Vec2 targetCentre, double viaCost) {
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
        // Incoming direction (approximate turn penalty keeps tracks straight and avoids zig-zags).
        int inDi = 0, inDj = 0;
        int ps = parent[static_cast<size_t>(s)];
        if (ps >= 0 && ps / static_cast<int>(n) == l) {
            size_t pc = static_cast<size_t>(ps) % n;
            inDi = i - static_cast<int>(pc % static_cast<size_t>(cols));
            inDj = j - static_cast<int>(pc / static_cast<size_t>(cols));
        }
        for (int k = 0; k < 8; ++k) {
            int ni = i + di[k], nj = j + dj[k];
            if (ni < 0 || nj < 0 || ni >= cols || nj >= rows) continue;
            size_t nc = g.idx(ni, nj);
            if (!g.passable(l, nc, net)) continue;
            float step = 1.0f;
            if (k >= 4) {
                // Diagonal: both orthogonal neighbours must be free so the centreline keeps clearance.
                if (!g.passable(l, g.idx(ni, j), net) || !g.passable(l, g.idx(i, nj), net)) continue;
                step = 1.4142f;
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
            if (ol == l) continue;
            int os = static_cast<int>(static_cast<size_t>(ol) * n + c);
            float nc2 = gc + static_cast<float>(viaCost) + 0.5f * static_cast<float>(std::abs(ol - l) - 1);
            if (nc2 >= cost[static_cast<size_t>(os)] || !g.passable(ol, c, net)) continue;
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

RouteStats PcbLayout::autoRoute(const Schematic& sch) {
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
    RouteStats best;
    best.failed = std::numeric_limits<int>::max();
    std::vector<Track> bestTracks;
    std::vector<Via> bestVias;

    for (int pass = 0; pass < 4; ++pass) {
        RoutingGrid grid(settings);
        for (const auto& p : ps) grid.markPad(p, clr + w / 2);
        std::vector<Track> outT;
        std::vector<Via> outV;
        RouteStats stats;
        std::vector<int> failedNets;

        for (int net : order) {
            const auto& list = netPads[net];
            std::vector<bool> connected(list.size(), false);
            connected[0] = true;
            std::vector<std::pair<int, size_t>> tree;
            for (int l = 0; l < grid.layers(); ++l)
                if (ps[list[0]].onLayer(l))
                    for (size_t c : grid.padCoreCells(ps[list[0]])) tree.push_back({l, c});
            bool netFailed = false;
            for (size_t done = 1; done < list.size(); ++done) {
                // Nearest unconnected pad to any connected pad.
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
                connected[target] = true;
                ++stats.connections;
                const Pad& tp = ps[list[target]];
                std::vector<std::vector<char>> mask(static_cast<size_t>(grid.layers()),
                                                    std::vector<char>(static_cast<size_t>(grid.cols() * grid.rows()), 0));
                std::vector<std::pair<int, size_t>> targetCells;
                for (int l = 0; l < grid.layers(); ++l)
                    if (tp.onLayer(l))
                        for (size_t c : grid.padCoreCells(tp)) {
                            mask[static_cast<size_t>(l)][c] = 1;
                            targetCells.push_back({l, c});
                        }
                RouteResult rr = astar(grid, net, tree, mask, tp.position, 12.0);
                if (!rr.ok) {
                    netFailed = true;
                    // Keep the pad in the tree anyway so later pads may still reach it.
                    for (auto tc : targetCells) tree.push_back(tc);
                    continue;
                }
                ++stats.routed;
                for (auto tc : targetCells) tree.push_back(tc);
                // Convert path into tracks/vias.
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
                            t.width = w;
                            t.a = grid.pos(rr.path[segStart].i, rr.path[segStart].j);
                            t.b = grid.pos(rr.path[m].i, rr.path[m].j);
                            outT.push_back(t);
                            grid.markSegment(layer, t.a, t.b, w + clr, net);
                            grid.markCopperSegment(layer, t.a, t.b, w / 2 + 1e-6, net);
                            stats.trackLength += (t.b - t.a).length();
                            segStart = m;
                        }
                    }
                    for (size_t m = k; m <= e; ++m)
                        tree.push_back({layer, grid.idx(rr.path[m].i, rr.path[m].j)});
                    Vec2 viaPos = grid.pos(rr.path[e].i, rr.path[e].j);
                    bool duplicate = !outV.empty() && outV.back().net == net && outV.back().position == viaPos;
                    if (e + 1 < rr.path.size() && !duplicate) {
                        // Layer change → through via at this cell.
                        Via v;
                        v.net = net;
                        v.position = grid.pos(rr.path[e].i, rr.path[e].j);
                        v.drill = settings.viaDrill;
                        v.diameter = settings.viaDiameter;
                        outV.push_back(v);
                        ++stats.vias;
                        double r = settings.viaDiameter / 2 + clr + w / 2;
                        for (int l = 0; l < grid.layers(); ++l) {
                            grid.markDisc(l, v.position, r, net);
                            grid.markCopperSegment(l, v.position, v.position, settings.viaDiameter / 2, net);
                        }
                    }
                    k = e + 1;
                }
            }
            if (netFailed) {
                failedNets.push_back(net);
                stats.failedNets.push_back(nets[static_cast<size_t>(net)].name);
            }
        }
        stats.failed = stats.connections - stats.routed;
        if (stats.failed < best.failed) {
            best = stats;
            bestTracks = outT;
            bestVias = outV;
        }
        if (stats.failed == 0) break;
        // Rip-up and retry with the failing nets promoted to the front.
        std::vector<int> next = failedNets;
        for (int n : order)
            if (std::find(failedNets.begin(), failedNets.end(), n) == failedNets.end()) next.push_back(n);
        order = next;
    }

    tracks.clear();
    vias.clear();
    for (auto& t : bestTracks) addTrack(t);
    for (auto& v : bestVias) addVia(v);
    if (best.failed == std::numeric_limits<int>::max()) best.failed = 0;
    return best;
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
    // DC operating point (used for voltage clearances and current capacity).
    bool hasSource = false;
    for (const auto& c : sch.components())
        hasSource |= c.kind == ComponentKind::VoltageSource || c.kind == ComponentKind::CurrentSource;
    DcResult dc;
    if (hasSource && sch.groundNet() >= 0 && (!tracks.empty() || !vias.empty() || !sch.components().empty()))
        dc = Simulator(sch).dcOperatingPoint();
    // Voltage range of each net: the DC value, widened to a source's peaks for nets driven by SIN/PULSE sources.
    std::map<int, std::pair<double, double>> netRange;
    if (dc.converged) {
        for (size_t n = 0; n < dc.netVoltages.size(); ++n)
            netRange[static_cast<int>(n)] = {dc.netVoltages[n], dc.netVoltages[n]};
        for (const auto& c : sch.components()) {
            if (c.kind != ComponentKind::VoltageSource) continue;
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
    }
    auto voltageNeed = [&](int a, int b) {
        auto ia = netRange.find(a), ib = netRange.find(b);
        if (a < 0 || b < 0 || ia == netRange.end() || ib == netRange.end()) return std::make_pair(0.0, 0.0);
        double dv = std::max(std::fabs(ia->second.second - ib->second.first), std::fabs(ib->second.second - ia->second.first));
        return std::make_pair(dv, ipc2221Clearance(dv, settings.highAltitude));
    };

    // Below the fabrication minimum → error; between it and the design rule → warning; below the IPC-2221 voltage
    // spacing for the nets' potential difference → error.
    auto clearanceCheck = [&](double d, const std::string& what, Vec2 loc, std::vector<int> comps = {}, int netA = -1,
                              int netB = -1) {
        auto [dv, hvNeed] = voltageNeed(netA, netB);
        if (d > 0 && hvNeed > clr + eps && d < hvNeed - eps) {
            char hv[160];
            std::snprintf(hv, sizeof hv, " is below the IPC-2221 %s spacing %.2f mm for %.0f V.",
                          settings.highAltitude ? "B3 (altitude)" : "B2", hvNeed, dv);
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

    Rect board(0, 0, settings.width, settings.height);
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
        if (cy.x0 < 0 || cy.y0 < 0 || cy.x1 > settings.width || cy.y1 > settings.height)
            add(Severity::Error, "DRC_OUT_OF_BOARD", c.ref + " extends beyond the board outline.", c.pcb.position, {c.id});
    }
    for (size_t i = 0; i < placed.size(); ++i)
        for (size_t j = i + 1; j < placed.size(); ++j)
            if (placed[i]->pcb.bottom == placed[j]->pcb.bottom && courtyard(*placed[i]).intersects(courtyard(*placed[j])))
                add(Severity::Warning, "DRC_COURTYARD_OVERLAP",
                    "Courtyards of " + placed[i]->ref + " and " + placed[j]->ref + " overlap.",
                    courtyard(*placed[i]).center(), {placed[i]->id, placed[j]->id});

    auto ps = pads(sch);
    // Pad ↔ pad.
    for (size_t i = 0; i < ps.size(); ++i)
        for (size_t j = i + 1; j < ps.size(); ++j) {
            if (ps[i].net == ps[j].net && ps[i].net >= 0) continue;
            bool share = false;
            for (int l = 0; l < settings.layerCount && !share; ++l) share = ps[i].onLayer(l) && ps[j].onLayer(l);
            if (!share) continue;
            double d = rectRectDistance(ps[i].bounds(), ps[j].bounds());
            clearanceCheck(d, "Pad clearance " + fmt(d) + " between " + netName(ps[i].net) + " and " + netName(ps[j].net), (ps[i].position + ps[j].position) * 0.5, {ps[i].componentId, ps[j].componentId}, ps[i].net, ps[j].net);
        }
    // Track ↔ pad, track ↔ track, track ↔ edge.
    for (size_t t = 0; t < tracks.size(); ++t) {
        const Track& tr = tracks[t];
        if (tr.layer < 0 || tr.layer >= settings.layerCount)
            add(Severity::Error, "DRC_LAYER", "Track on " + netName(tr.net) + " uses a layer that is not in the " +
                std::to_string(settings.layerCount) + "-layer stack-up.", tr.a);
        for (const Vec2& p : {tr.a, tr.b}) {
            double edge = std::min({p.x, p.y, settings.width - p.x, settings.height - p.y}) - tr.width / 2;
            if (edge < settings.edgeClearance - eps) {
                add(Severity::Error, "DRC_EDGE_CLEARANCE", "Track on " + netName(tr.net) + " is too close to the board edge.", p);
                break;
            }
        }
        for (const auto& p : ps) {
            if (p.net == tr.net || !p.onLayer(tr.layer)) continue;
            double d = (p.round ? std::max(0.0, pointSegmentDistance(p.position, tr.a, tr.b) - std::min(p.size.x, p.size.y) / 2)
                                : segmentRectDistance(tr.a, tr.b, p.bounds())) -
                       tr.width / 2;
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
        if (!board.inflated(-settings.edgeClearance).contains(vias[i].position))
            add(Severity::Error, "DRC_EDGE_CLEARANCE", "Via is too close to the board edge.", vias[i].position);
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
    auto touchesCopper = [&](size_t self, Vec2 end) {
        const Track& t = tracks[self];
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
                    std::to_string(vias.size()) + " vias, " + std::to_string(ps.size()) + " pads.";
        out.push_back(v);
    }
    return out;
}

}  // namespace sieda
