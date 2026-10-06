// SiEDA Core — copper pours and plane layers (raster fill with clearance, thermal reliefs and island removal).
#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <string>
#include <tuple>

#include "sieda/Pcb.hpp"
#include "sieda/Isolation.hpp"

namespace sieda {

namespace {
double padCopperDistance(const Pad& p, Vec2 pt) {
    if (p.round) return std::max(0.0, (pt - p.position).length() - std::min(p.size.x, p.size.y) / 2);
    return pointRectDistance(pt, p.bounds());
}

/// Raster helper: calls f(cellIndex, centre) for every cell whose centre lies in `box`.
template <typename F>
void forCells(int cols, int rows, double cell, const Rect& box, F f) {
    int i0 = std::max(0, static_cast<int>(std::floor(box.x0 / cell - 0.5)));
    int i1 = std::min(cols - 1, static_cast<int>(std::ceil(box.x1 / cell - 0.5)));
    int j0 = std::max(0, static_cast<int>(std::floor(box.y0 / cell - 0.5)));
    int j1 = std::min(rows - 1, static_cast<int>(std::ceil(box.y1 / cell - 0.5)));
    for (int j = j0; j <= j1; ++j)
        for (int i = i0; i <= i1; ++i)
            f(static_cast<size_t>(j) * static_cast<size_t>(cols) + static_cast<size_t>(i),
              Vec2{(i + 0.5) * cell, (j + 0.5) * cell});
}

Rect segmentBox(Vec2 a, Vec2 b, double r) {
    return Rect(std::min(a.x, b.x) - r, std::min(a.y, b.y) - r, std::max(a.x, b.x) + r, std::max(a.y, b.y) + r);
}

/// Square min (erode) or max (dilate) filter of radius k, separable.
std::vector<char> squareFilter(const std::vector<char>& in, int cols, int rows, int k, bool erode) {
    std::vector<char> tmp(in.size()), out(in.size());
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            char v = erode ? 1 : 0;
            for (int d = -k; d <= k; ++d) {
                int x = i + d;
                char c = (x < 0 || x >= cols) ? 0 : in[static_cast<size_t>(j * cols + x)];
                v = erode ? static_cast<char>(v && c) : static_cast<char>(v || c);
            }
            tmp[static_cast<size_t>(j * cols + i)] = v;
        }
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            char v = erode ? 1 : 0;
            for (int d = -k; d <= k; ++d) {
                int y = j + d;
                char c = (y < 0 || y >= rows) ? 0 : tmp[static_cast<size_t>(y * cols + i)];
                v = erode ? static_cast<char>(v && c) : static_cast<char>(v || c);
            }
            out[static_cast<size_t>(j * cols + i)] = v;
        }
    return out;
}

void hashBytes(size_t& h, const void* data, size_t n) {
    const unsigned char* p = static_cast<const unsigned char*>(data);
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
}
void hashD(size_t& h, double v) { hashBytes(h, &v, sizeof v); }
void hashI(size_t& h, long long v) { hashBytes(h, &v, sizeof v); }
}  // namespace

int ZoneFill::islandNear(Vec2 p, double radius) const {
    int found = -1;
    double r = radius + cell * 0.45;
    forCells(cols, rows, cell, Rect::centered(p, 2 * r, 2 * r), [&](size_t c, Vec2 at) {
        if (found < 0 && island[c] >= 0 && (at - p).length() <= r) found = island[c];
    });
    return found;
}

double ZoneFill::area() const {
    double a = 0;
    for (const auto& r : rects) a += r.width() * r.height();
    return a;
}

std::vector<ZoneFill> PcbLayout::fillZones(const Schematic& sch, const std::vector<Pad>& ps,
                                           const std::vector<Track>& trs, const std::vector<Via>& vs) const {
    std::vector<ZoneFill> fills;
    const auto& nets = sch.nets();
    const BoardSettings& s = settings;
    if (zones.empty() || s.width <= 0 || s.height <= 0) return fills;
    // 0.1 mm cells, coarser on very large boards to bound the work.
    const double cell = std::max(0.1, std::sqrt(s.width * s.height / 2.5e6));
    const int cols = std::max(1, static_cast<int>(std::ceil(s.width / cell)));
    const int rows = std::max(1, static_cast<int>(std::ceil(s.height / cell)));
    const size_t n = static_cast<size_t>(cols) * static_cast<size_t>(rows);
    const double half = cell * 0.7072;  // centre-to-corner of a cell

    // Board area available to copper: inside the outline by the edge clearance and outside the hole keep-outs.
    std::vector<char> board(n, 0);
    for (int j = 0; j < rows; ++j)
        for (int i = 0; i < cols; ++i) {
            Vec2 p{(i + 0.5) * cell, (j + 0.5) * cell};
            board[static_cast<size_t>(j * cols + i)] =
                s.edgeDistance(p) >= s.edgeClearance + half && s.holeDistance(p) >= half;
        }
    const int k = std::max(1, static_cast<int>(std::ceil(s.minTrackWidth / (2 * cell) - 1e-9)));
    // Isolation barrier: a pour keeps the barrier gap from copper of other galvanic domains.
    // Isolation barrier and mains spacing: a pour keeps the fence gap from copper of other domains.
    const SpacingDomains spacing = spacingDomains(sch, s);
    const GalvanicDomains& doms = spacing.domains;

    for (size_t zi = 0; zi < zones.size(); ++zi) {
        const CopperZone& z = zones[zi];
        ZoneFill f;
        f.zone = static_cast<int>(zi);
        f.layer = z.layer;
        f.cell = cell;
        f.cols = cols;
        f.rows = rows;
        f.island.assign(n, -1);
        for (const auto& net : nets)
            if (net.name == z.net) f.net = net.index;
        if (f.net < 0 || z.layer < 0 || z.layer >= s.layerCount) {
            fills.push_back(std::move(f));
            continue;
        }
        const int L = z.layer, net = f.net;
        const double clr = std::max(s.clearance, z.clearance);
        const double r = clr + half;
        const int zoneDom = doms.domainOfNet(net);
        // Keep-out radius around another net's copper: the clearance, or the barrier gap across domains.
        auto keep = [&](int other) {
            const int od = doms.domainOfNet(other);
            return zoneDom >= 0 && od >= 0 && od != zoneDom ? std::max(clr, spacing.gap) + half : r;
        };
        std::vector<char> ok = board;
        auto block = [&](size_t c, Vec2) { ok[c] = 0; };

        // Two-pad SMD chips (resistors, capacitors, LEDs) get thermal reliefs too: a pad joined solidly to the pour
        // heats slower than its partner and the part tombstones during reflow.
        std::map<int, int> padsOf, smdOf;
        for (const auto& p : ps) {
            ++padsOf[p.componentId];
            smdOf[p.componentId] += !p.throughHole;
        }
        auto chipPad = [&](const Pad& p) {
            return !p.throughHole && padsOf[p.componentId] == 2 && smdOf[p.componentId] == 2 && p.size.x * p.size.y <= 4.0;
        };
        for (const auto& p : ps) {
            if (!p.onLayer(L)) continue;
            const double rp = keep(p.net);
            Rect box = p.bounds().inflated(rp + cell);
            if (p.net != net || p.net < 0) {
                forCells(cols, rows, cell, box, [&](size_t c, Vec2 at) {
                    if (padCopperDistance(p, at) < rp) block(c, at);
                });
            } else if (p.throughHole || chipPad(p)) {
                // Thermal relief: a clearance ring around the pad crossed by four spokes.
                double spoke = std::max(s.trackWidth, (2 * k + 2) * cell);
                forCells(cols, rows, cell, box, [&](size_t c, Vec2 at) {
                    double d = padCopperDistance(p, at);
                    Vec2 rel = at - p.position;
                    bool onSpoke = std::fabs(rel.x) <= spoke / 2 || std::fabs(rel.y) <= spoke / 2;
                    if (d > 0 && d < r && !onSpoke) block(c, at);
                });
            }
        }
        for (const auto& t : trs) {
            if (t.layer != L || t.net == net) continue;
            double rr = keep(t.net) + t.width / 2;
            forCells(cols, rows, cell, trackBox(t, rr), [&](size_t c, Vec2 at) {
                if (trackPointDistance(t, at) < rr) block(c, at);
            });
        }
        for (const auto& v : vs) {
            if (v.net == net || !v.spans(L)) continue;
            double rr = keep(v.net) + v.diameter / 2;
            forCells(cols, rows, cell, segmentBox(v.position, v.position, rr), [&](size_t c, Vec2 at) {
                if ((at - v.position).length() < rr) block(c, at);
            });
        }
        // Earlier zones of other nets on this layer.
        for (const auto& e : fills) {
            if (e.layer != L || e.net == net || e.net < 0) continue;
            const double ez = keep(e.net) - half;  // clearance, or the barrier gap to another domain's pour
            const int kz = static_cast<int>(std::ceil((ez + 1.4143 * cell) / cell));
            for (int j = 0; j < rows; ++j)
                for (int i = 0; i < cols; ++i) {
                    if (e.island[static_cast<size_t>(j * cols + i)] < 0) continue;
                    ok[static_cast<size_t>(j * cols + i)] = 0;
                    // Only boundary cells can widen the keep-out.
                    bool edge = false;
                    for (auto [x, y] : {std::make_pair(i + 1, j), std::make_pair(i - 1, j), std::make_pair(i, j + 1),
                                        std::make_pair(i, j - 1)})
                        edge |= x < 0 || y < 0 || x >= cols || y >= rows || e.island[static_cast<size_t>(y * cols + x)] < 0;
                    if (!edge) continue;
                    for (int dj = -kz; dj <= kz; ++dj)
                        for (int di = -kz; di <= kz; ++di) {
                            int x = i + di, y = j + dj;
                            if (x < 0 || y < 0 || x >= cols || y >= rows) continue;
                            if (std::sqrt(double(di * di + dj * dj)) * cell < ez + 1.4143 * cell)
                                ok[static_cast<size_t>(y * cols + x)] = 0;
                        }
                }
        }
        // Opening removes slivers narrower than the minimum track width (unetchable copper).
        ok = squareFilter(squareFilter(ok, cols, rows, k, true), cols, rows, k, false);

        // Islands (4-connected), keeping only those that touch this net's copper on the layer.
        std::vector<char> own(n, 0);
        auto mark = [&](size_t c, Vec2) { own[c] = 1; };
        const double touch = cell * 0.45;
        for (const auto& p : ps)
            if (p.net == net && p.onLayer(L))
                forCells(cols, rows, cell, p.bounds().inflated(touch), [&](size_t c, Vec2 at) {
                    if (padCopperDistance(p, at) <= touch) mark(c, at);
                });
        for (const auto& t : trs)
            if (t.net == net && t.layer == L)
                forCells(cols, rows, cell, trackBox(t, t.width / 2 + touch), [&](size_t c, Vec2 at) {
                    if (trackPointDistance(t, at) <= t.width / 2 + touch) mark(c, at);
                });
        for (const auto& v : vs)
            if (v.net == net && v.spans(L))
                forCells(cols, rows, cell, segmentBox(v.position, v.position, v.diameter / 2 + touch), [&](size_t c, Vec2 at) {
                    if ((at - v.position).length() <= v.diameter / 2 + touch) mark(c, at);
                });
        std::vector<int> label(n, -1);
        std::vector<size_t> stack;
        int count = 0;
        for (size_t start = 0; start < n; ++start) {
            if (!ok[start] || label[start] != -1) continue;
            std::vector<size_t> members;
            bool touches = false;
            stack.push_back(start);
            label[start] = count;
            while (!stack.empty()) {
                size_t c = stack.back();
                stack.pop_back();
                members.push_back(c);
                touches |= own[c] != 0;
                int i = static_cast<int>(c % static_cast<size_t>(cols)), j = static_cast<int>(c / static_cast<size_t>(cols));
                const int di[4] = {1, -1, 0, 0}, dj[4] = {0, 0, 1, -1};
                for (int d = 0; d < 4; ++d) {
                    int x = i + di[d], y = j + dj[d];
                    if (x < 0 || y < 0 || x >= cols || y >= rows) continue;
                    size_t nc = static_cast<size_t>(y * cols + x);
                    if (ok[nc] && label[nc] == -1) {
                        label[nc] = count;
                        stack.push_back(nc);
                    }
                }
            }
            if (touches) {
                for (size_t c : members) f.island[c] = count;
                ++count;
            } else {
                for (size_t c : members) label[c] = -2;  // floating copper: removed
            }
        }
        f.islands = count;

        // Merge into rectangles: horizontal runs, extended downward while the run repeats.
        std::map<std::tuple<int, int, int>, size_t> open;  // (i0, i1, island) → rect index
        for (int j = 0; j < rows; ++j) {
            std::map<std::tuple<int, int, int>, size_t> next;
            int i = 0;
            while (i < cols) {
                int id = f.island[static_cast<size_t>(j * cols + i)];
                if (id < 0) {
                    ++i;
                    continue;
                }
                int i0 = i;
                while (i < cols && f.island[static_cast<size_t>(j * cols + i)] == id) ++i;
                auto key = std::make_tuple(i0, i, id);
                auto it = open.find(key);
                if (it != open.end()) {
                    f.rects[it->second].y1 = (j + 1) * cell;
                    next[key] = it->second;
                } else {
                    f.rects.push_back(Rect(i0 * cell, j * cell, i * cell, (j + 1) * cell));
                    next[key] = f.rects.size() - 1;
                }
            }
            open = std::move(next);
        }
        fills.push_back(std::move(f));
    }
    return fills;
}

const std::vector<ZoneFill>& PcbLayout::zoneFills(const Schematic& sch) const {
    if (zones.empty()) {
        fillCache_.clear();
        fillValid_ = false;
        return fillCache_;
    }
    auto ps = pads(sch);
    size_t h = 14695981039346656037ull;
    const BoardSettings& s = settings;
    for (double v : {s.width, s.height, s.clearance, s.edgeClearance, s.minTrackWidth, s.trackWidth, s.isolationGap}) hashD(h, v);
    hashI(h, s.layerCount);
    for (const auto& p : s.outline) {
        hashD(h, p.x);
        hashD(h, p.y);
    }
    for (const auto& m : s.holes) {
        hashD(h, m.position.x);
        hashD(h, m.position.y);
        hashD(h, m.keepout);
    }
    for (const auto& z : zones) {
        hashBytes(h, z.net.data(), z.net.size());
        hashI(h, z.layer);
        hashI(h, z.plane);
        hashD(h, z.clearance);
    }
    for (const auto& net : sch.nets()) hashBytes(h, net.name.data(), net.name.size());
    for (const auto& p : ps) {
        for (double v : {p.position.x, p.position.y, p.size.x, p.size.y}) hashD(h, v);
        hashI(h, p.net);
        hashI(h, p.smdLayer * 2 + p.throughHole);
    }
    for (const auto& t : tracks) {
        for (double v : {t.a.x, t.a.y, t.b.x, t.b.y, t.width}) hashD(h, v);
        if (t.arc) {
            hashD(h, t.mid.x);
            hashD(h, t.mid.y);
        }
        hashI(h, t.net);
        hashI(h, t.layer);
    }
    for (const auto& v : vias) {
        for (double d : {v.position.x, v.position.y, v.diameter}) hashD(h, d);
        hashI(h, v.net);
    }
    if (!fillValid_ || h != fillKey_) {
        fillCache_ = fillZones(sch, ps, tracks, vias);
        fillKey_ = h;
        fillValid_ = true;
    }
    return fillCache_;
}

bool PcbLayout::isZoneNet(const Schematic& sch, int net) const {
    if (net < 0 || net >= static_cast<int>(sch.nets().size())) return false;
    const std::string& name = sch.nets()[static_cast<size_t>(net)].name;
    for (const auto& z : zones)
        if (z.net == name && z.layer >= 0 && z.layer < settings.layerCount) return true;
    return false;
}

}  // namespace sieda
