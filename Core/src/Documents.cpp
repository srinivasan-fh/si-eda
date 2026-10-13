#include "sieda/Documents.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "sieda/Bom.hpp"
#include "sieda/Library.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Project.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda {

namespace {

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", std::fabs(v) < 5e-5 ? 0.0 : v);
    return b;
}

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string csv(const std::string& s) {
    if (s.find_first_of(",\"\n") == std::string::npos) return s;
    std::string out = "\"";
    for (char ch : s) out += ch == '"' ? std::string("\"\"") : std::string(1, ch);
    return out + "\"";
}

std::string html(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

double polygonArea(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0, j = p.size() - 1; i < p.size(); j = i++) a += (p[j].x * p[i].y - p[i].x * p[j].y);
    return a / 2;
}

bool insidePolygon(const std::vector<Vec2>& poly, Vec2 p) {
    bool in = false;
    for (size_t i = 0, j = poly.size() - 1; i < poly.size(); j = i++)
        if ((poly[i].y > p.y) != (poly[j].y > p.y) &&
            p.x < (poly[j].x - poly[i].x) * (p.y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x)
            in = !in;
    return in;
}

/// Points along an arc from angle a0 to a1 (radians, counter-clockwise), centre c, radius r; the end points included.
std::vector<Vec2> arcPoints(Vec2 c, double r, double a0, double a1) {
    if (!std::isfinite(c.x + c.y + r + a0 + a1) || std::fabs(a0) > 1e3 || std::fabs(a1) > 1e3 || r <= 0) return {};
    while (a1 <= a0 + 1e-12) a1 += 2 * M_PI;
    const int n = std::max(2, static_cast<int>(std::ceil((a1 - a0) / (M_PI / 18))));  // 10° steps
    std::vector<Vec2> out;
    for (int i = 0; i <= n; ++i) {
        const double a = a0 + (a1 - a0) * i / n;
        out.push_back({c.x + r * std::cos(a), c.y + r * std::sin(a)});
    }
    return out;
}

bool isTestPoint(const Component& c) { return upper(c.ref).rfind("TP", 0) == 0; }

}  // namespace

// ===================================================================== DXF

std::string exportDxf(const Schematic& sch, const PcbLayout& pcb) {
    const BoardSettings& s = pcb.settings;
    std::ostringstream o;
    auto flip = [&](Vec2 p) { return Vec2{p.x, s.height - p.y}; };  // board (Y down) → MCAD (Y up)
    auto line = [&](const char* layer, Vec2 a, Vec2 b) {
        a = flip(a), b = flip(b);
        o << "0\nLINE\n8\n" << layer << "\n10\n" << num(a.x) << "\n20\n" << num(a.y) << "\n11\n" << num(b.x) << "\n21\n"
          << num(b.y) << "\n";
    };
    auto circle = [&](const char* layer, Vec2 c, double r) {
        c = flip(c);
        o << "0\nCIRCLE\n8\n" << layer << "\n10\n" << num(c.x) << "\n20\n" << num(c.y) << "\n40\n" << num(r) << "\n";
    };
    o << "0\nSECTION\n2\nHEADER\n9\n$ACADVER\n1\nAC1009\n9\n$INSUNITS\n70\n4\n0\nENDSEC\n";
    o << "0\nSECTION\n2\nENTITIES\n";
    s.forEachEdge([&](Vec2 a, Vec2 b) { line("OUTLINE", a, b); });
    for (const auto& h : s.holes) circle("HOLES", h.position, h.drill / 2);
    for (const auto& p : pcb.pads(sch))
        if (p.throughHole && p.drill > 0) circle(p.npth ? "HOLES" : "DRILLS", p.position, p.drill / 2);
    for (const auto& v : pcb.vias) circle("DRILLS", v.position, v.drill / 2);
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || !pcb.onBoard(c)) continue;
        const Rect r = pcb.courtyard(c);
        const Vec2 k[4] = {{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
        for (int i = 0; i < 4; ++i) line("COURTYARD", k[i], k[(i + 1) % 4]);
        for (const FootprintLine& l : pcb.footprintLines(c))
            line(l.fab ? "FAB" : c.pcb.bottom ? "SILK_BOTTOM" : "SILK_TOP", l.a, l.b);
    }
    for (const auto& d : s.dimensions) {  // the dimension line, its two extension lines and the value
        const Vec2 v = d.b - d.a;
        if (v.length() < 1e-9) continue;
        const Vec2 n = Vec2{-v.y, v.x} * (d.offset / v.length()), a = d.a + n, b = d.b + n, mid = flip((a + b) * 0.5);
        line("DIMENSIONS", d.a, a), line("DIMENSIONS", d.b, b), line("DIMENSIONS", a, b);
        o << "0\nTEXT\n8\nDIMENSIONS\n10\n" << num(mid.x) << "\n20\n" << num(mid.y) << "\n40\n1.5\n1\n" << num(v.length()) << "\n";
    }
    o << "0\nENDSEC\n0\nEOF\n";
    return o.str();
}

DxfOutline importDxfOutline(const std::string& text) {
    DxfOutline out;
    // Group code / value pairs.
    std::vector<std::pair<int, std::string>> pairs;
    {
        std::istringstream in(text);
        std::string code, value;
        while (std::getline(in, code) && std::getline(in, value)) {
            auto trim = [](std::string& s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
                size_t i = 0;
                while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
                s.erase(0, i);
            };
            trim(code), trim(value);
            char* end = nullptr;
            const long c = std::strtol(code.c_str(), &end, 10);
            if (end == code.c_str()) {
                out.error = "not a DXF file (group codes expected)";
                return out;
            }
            pairs.push_back({static_cast<int>(c), value});
            if (pairs.size() > 4'000'000) {
                out.error = "the DXF file is too large";
                return out;
            }
        }
    }
    double scale = 1;  // $INSUNITS: 1 inch, 4 mm (default), 5 cm, 6 m
    for (size_t i = 0; i + 1 < pairs.size(); ++i)
        if (pairs[i].first == 9 && pairs[i].second == "$INSUNITS") {
            const long u = std::strtol(pairs[i + 1].second.c_str(), nullptr, 10);
            scale = u == 1 ? 25.4 : u == 2 ? 304.8 : u == 5 ? 10 : u == 6 ? 1000 : 1;
        }
    // A file that names its layers: only the outline-like ones (OUTLINE, Edge.Cuts, BOARD, PROFILE, CUTOUT …) count.
    auto outlineLayer = [](std::string layer) {
        layer = upper(layer);
        for (const char* key : {"OUTLINE", "EDGE", "BOARD", "PROFILE", "CUT", "CONTOUR"})
            if (layer.find(key) != std::string::npos) return true;
        return false;
    };
    bool layered = false;
    for (size_t i = 0; i + 1 < pairs.size(); ++i)
        if (pairs[i].first == 8 && outlineLayer(pairs[i].second)) layered = true;
    std::vector<std::pair<Vec2, Vec2>> segments;
    std::vector<std::vector<Vec2>> loops;
    auto bulgeTo = [](std::vector<Vec2>& pts, Vec2 a, Vec2 b, double bulge) {  // LWPOLYLINE arc segment
        if (std::fabs(bulge) < 1e-9) {
            pts.push_back(b);
            return;
        }
        const double theta = 4 * std::atan(bulge), chord = (b - a).length();
        if (chord < 1e-12) return;
        const double r = chord / (2 * std::sin(std::fabs(theta) / 2));
        const Vec2 mid = (a + b) * 0.5, dir = (b - a) * (1 / chord);
        const Vec2 normal{-dir.y, dir.x};
        const double h = r * std::cos(std::fabs(theta) / 2) * (bulge > 0 ? 1 : -1);
        const Vec2 c = mid + normal * h;
        const double a0 = std::atan2(a.y - c.y, a.x - c.x), a1 = std::atan2(b.y - c.y, b.x - c.x);
        auto arc = bulge > 0 ? arcPoints(c, r, a0, a1) : arcPoints(c, r, a1, a0);
        if (arc.size() < 2) {  // a degenerate arc (non-finite or huge): a straight segment
            pts.push_back(b);
            return;
        }
        if (bulge < 0) std::reverse(arc.begin(), arc.end());
        pts.insert(pts.end(), arc.begin() + 1, arc.end());
    };
    // Entities: from each "0" code to the next.
    for (size_t i = 0; i < pairs.size(); ++i) {
        if (pairs[i].first != 0) continue;
        const std::string type = pairs[i].second;
        size_t j = i + 1;
        while (j < pairs.size() && pairs[j].first != 0) ++j;
        if (layered) {
            bool keep = false;
            for (size_t k = i + 1; k < j; ++k)
                if (pairs[k].first == 8) keep = outlineLayer(pairs[k].second);
            if (!keep) continue;
        }
        auto flags = [&](int code) {  // integer group values (70 = polyline flags), clamped by strtol
            for (size_t k = i + 1; k < j; ++k)
                if (pairs[k].first == code) return std::strtol(pairs[k].second.c_str(), nullptr, 10);
            return 0L;
        };
        auto value = [&](int code, double fallback = 0) {
            for (size_t k = i + 1; k < j; ++k)
                if (pairs[k].first == code) return std::atof(pairs[k].second.c_str());
            return fallback;
        };
        if (type == "LINE") {
            segments.push_back({{value(10), value(20)}, {value(11), value(21)}});
        } else if (type == "ARC") {
            const auto pts = arcPoints({value(10), value(20)}, value(40), value(50) * M_PI / 180, value(51) * M_PI / 180);
            for (size_t k = 1; k < pts.size(); ++k) segments.push_back({pts[k - 1], pts[k]});
        } else if (type == "CIRCLE") {
            auto pts = arcPoints({value(10), value(20)}, value(40), 0, 2 * M_PI);
            if (pts.size() > 3) pts.pop_back(), loops.push_back(pts);
        } else if (type == "LWPOLYLINE") {
            std::vector<Vec2> verts;
            std::vector<double> bulges;
            for (size_t k = i + 1; k < j; ++k) {
                if (pairs[k].first == 10) verts.push_back({std::atof(pairs[k].second.c_str()), 0}), bulges.push_back(0);
                if (pairs[k].first == 20 && !verts.empty()) verts.back().y = std::atof(pairs[k].second.c_str());
                if (pairs[k].first == 42 && !bulges.empty()) bulges.back() = std::atof(pairs[k].second.c_str());
            }
            const bool closed = (flags(70) & 1) != 0;
            if (verts.size() < 2) continue;
            std::vector<Vec2> pts{verts[0]};
            for (size_t k = 1; k < verts.size(); ++k) bulgeTo(pts, verts[k - 1], verts[k], bulges[k - 1]);
            if (closed) {
                bulgeTo(pts, verts.back(), verts[0], bulges.back());
                pts.pop_back();
                loops.push_back(pts);
            } else {
                for (size_t k = 1; k < pts.size(); ++k) segments.push_back({pts[k - 1], pts[k]});
            }
        } else if (type == "POLYLINE") {
            const bool closed = (flags(70) & 1) != 0;
            std::vector<Vec2> pts;
            size_t k = j;
            while (k < pairs.size() && !(pairs[k].first == 0 && pairs[k].second == "SEQEND")) {
                if (pairs[k].first == 0 && pairs[k].second == "VERTEX") pts.push_back({0, 0});
                if (pairs[k].first == 10 && !pts.empty()) pts.back().x = std::atof(pairs[k].second.c_str());
                if (pairs[k].first == 20 && !pts.empty()) pts.back().y = std::atof(pairs[k].second.c_str());
                ++k;
            }
            if (closed && pts.size() >= 3) loops.push_back(pts);
            else
                for (size_t m = 1; m < pts.size(); ++m) segments.push_back({pts[m - 1], pts[m]});
        }
    }
    auto sane = [](Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::fabs(p.x) < 1e7 && std::fabs(p.y) < 1e7; };
    segments.erase(std::remove_if(segments.begin(), segments.end(), [&](const auto& g) { return !sane(g.first) || !sane(g.second); }),
                   segments.end());
    // Chain loose segments end to end into closed loops (ends indexed on a 1 µm-unit grid, neighbours searched).
    const double tol = 1e-3;
    auto near = [&](Vec2 a, Vec2 b) { return std::fabs(a.x - b.x) < tol && std::fabs(a.y - b.y) < tol; };
    auto key = [&](Vec2 p) { return std::make_pair(std::llround(p.x / tol), std::llround(p.y / tol)); };
    std::map<std::pair<long long, long long>, std::vector<size_t>> ends;
    for (size_t k = 0; k < segments.size(); ++k) ends[key(segments[k].first)].push_back(k), ends[key(segments[k].second)].push_back(k);
    std::vector<bool> used(segments.size(), false);
    auto nextAt = [&](Vec2 p, Vec2* other) -> bool {
        const auto [kx, ky] = key(p);
        for (long long dx = -1; dx <= 1; ++dx)
            for (long long dy = -1; dy <= 1; ++dy)
                if (auto it = ends.find({kx + dx, ky + dy}); it != ends.end())
                    for (size_t k : it->second) {
                        if (used[k]) continue;
                        if (near(segments[k].first, p)) *other = segments[k].second;
                        else if (near(segments[k].second, p)) *other = segments[k].first;
                        else continue;
                        used[k] = true;
                        return true;
                    }
        return false;
    };
    for (size_t s0 = 0; s0 < segments.size(); ++s0) {
        if (used[s0]) continue;
        used[s0] = true;
        std::vector<Vec2> chain{segments[s0].first, segments[s0].second};
        Vec2 next;
        while (!near(chain.front(), chain.back()) && nextAt(chain.back(), &next)) chain.push_back(next);
        if (chain.size() >= 4 && near(chain.front(), chain.back())) {
            chain.pop_back();
            loops.push_back(chain);
        }
    }
    loops.erase(std::remove_if(loops.begin(), loops.end(),
                               [&](const auto& l) {
                                   if (!std::all_of(l.begin(), l.end(), sane)) return true;
                                   const double a = l.size() < 3 ? 0 : polygonArea(l);
                                   return !std::isfinite(a) || std::fabs(a) < 1e-6 || std::fabs(a) > 1e12;
                               }),
                loops.end());
    if (loops.empty()) {
        out.error = "no closed outline in the DXF (lines, arcs, polylines or circles that close)";
        return out;
    }
    for (auto& l : loops)
        for (auto& p : l) p = p * scale;
    std::sort(loops.begin(), loops.end(), [](const auto& a, const auto& b) { return std::fabs(polygonArea(a)) > std::fabs(polygonArea(b)); });
    double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
    for (const Vec2& p : loops[0]) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    if (x1 - x0 < 1 || y1 - y0 < 1 || x1 - x0 > 2000 || y1 - y0 > 2000) {
        out.error = "the outline is not between 1 mm and 2 m across";
        return out;
    }
    auto toBoard = [&](Vec2 p) { return Vec2{p.x - x0, y1 - p.y}; };  // Y up → board Y down, lower-left at the origin
    for (const Vec2& p : loops[0]) out.outline.push_back(toBoard(p));
    for (size_t k = 1; k < loops.size(); ++k)
        if (insidePolygon(loops[0], loops[k][0])) {
            std::vector<Vec2> cut;
            for (const Vec2& p : loops[k]) cut.push_back(toBoard(p));
            out.cutouts.push_back(std::move(cut));
        }
    out.width = x1 - x0, out.height = y1 - y0;
    return out;
}

bool applyDxfOutline(PcbLayout& pcb, const DxfOutline& dxf) {
    if (!dxf.error.empty() || dxf.outline.size() < 3) return false;
    auto& s = pcb.settings;
    s.width = dxf.width;
    s.height = dxf.height;
    s.outline = dxf.outline;
    s.cutouts = dxf.cutouts;
    return true;
}

// ===================================================================== test points

std::string exportTestPointReport(const Schematic& sch, const PcbLayout& pcb) {
    const auto pads = pcb.pads(sch);
    const double h = pcb.settings.height;
    std::ostringstream o;
    o << "type,designator,net,pad,x_mm,y_mm,side,size_mm,access\n";
    auto row = [&](const char* type, const Pad& p, const std::string& access) {
        const Component* c = sch.find(p.componentId);
        const std::string side = p.throughHole ? "both" : (p.bottom ? "bottom" : "top");
        o << type << "," << csv(c ? c->ref : "") << "," << csv(sch.nets()[static_cast<size_t>(p.net)].name) << ","
          << p.padNumber << "," << num(p.position.x) << "," << num(h - p.position.y) << "," << side << ","
          << num(std::min(p.size.x, p.size.y)) << "," << access << "\n";
    };
    std::set<int> covered;
    for (const Pad& p : pads) {  // dedicated test points
        const Component* c = sch.find(p.componentId);
        if (!c || !isTestPoint(*c) || p.net < 0) continue;
        row("testpoint", p, p.throughHole ? "both sides" : (p.bottom ? "bottom" : "top"));
        covered.insert(p.net);
    }
    std::map<int, const Pad*> best;  // per net: a through-hole pad, else the largest probe-able SMD pad
    auto score = [](const Pad& p) { return (p.throughHole ? 100.0 : 0.0) + std::min(p.size.x, p.size.y); };
    for (const Pad& p : pads) {
        if (p.net < 0 || covered.count(p.net) || p.npth) continue;
        if (!p.throughHole && std::min(p.size.x, p.size.y) < 0.6 - 1e-9) continue;  // too small for a probe
        const Pad*& b = best[p.net];
        if (!b || score(p) > score(*b)) b = &p;
    }
    std::set<int> padNets;
    for (const Pad& p : pads)
        if (p.net >= 0) padNets.insert(p.net);
    for (int net : padNets) {
        if (covered.count(net)) continue;
        if (auto it = best.find(net); it != best.end() && it->second) {
            row("pad", *it->second, it->second->throughHole ? "both sides" : "probe on pad");
        } else {
            o << "no-access,," << csv(sch.nets()[static_cast<size_t>(net)].name)
              << ",,,,,,add a test point (no through-hole pad or SMD pad of 0.6 mm)\n";
        }
    }
    return o.str();
}

// ===================================================================== HTML BOM

std::string exportHtmlBom(const Project& project) {
    const Schematic sch = project.variantSchematic(project.activeVariant);
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    std::ostringstream svg[2];
    for (int side = 0; side < 2; ++side) {
        std::ostringstream& v = svg[side];
        const double m = 2;
        v << "<svg viewBox=\"" << num(-m) << " " << num(-m) << " " << num(s.width + 2 * m) << " " << num(s.height + 2 * m)
          << "\"" << (side ? " style=\"transform:scaleX(-1)\"" : "") << "><path class=\"board\" d=\"";
        auto loop = [&](const std::vector<Vec2>& pts) {
            for (size_t i = 0; i < pts.size(); ++i) v << (i ? "L" : "M") << num(pts[i].x) << " " << num(pts[i].y);
            v << "Z";
        };
        loop(s.outlinePolygon());
        for (const auto& c : s.cutouts) loop(c);
        v << "\" fill-rule=\"evenodd\"/>\n";
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed || !pcb.onBoard(c) || c.pcb.bottom != (side == 1)) continue;
            const Rect r = pcb.courtyard(c);
            v << "<g class=\"part\" data-ref=\"" << html(c.ref) << "\"><rect class=\"court\" x=\"" << num(r.x0) << "\" y=\""
              << num(r.y0) << "\" width=\"" << num(r.width()) << "\" height=\"" << num(r.height()) << "\"/>";
            for (const Pad& p : pcb.pads(sch))
                if (p.componentId == c.id)
                    v << "<rect class=\"pad\" x=\"" << num(p.position.x - p.size.x / 2) << "\" y=\""
                      << num(p.position.y - p.size.y / 2) << "\" width=\"" << num(p.size.x) << "\" height=\""
                      << num(p.size.y) << "\" rx=\"" << num(p.round ? std::min(p.size.x, p.size.y) / 2 : 0) << "\"/>";
            v << "</g>\n";
        }
        v << "</svg>";
    }
    std::ostringstream o;
    o << "<!doctype html>\n<html><head><meta charset=\"utf-8\"><title>" << html(project.name)
      << " — interactive BOM</title>\n<style>\n"
         "body{font:13px -apple-system,Helvetica,sans-serif;margin:0;display:flex;height:100vh}\n"
         "#bom{width:46%;overflow:auto;border-right:1px solid #ccc}#boards{flex:1;overflow:auto;padding:8px}\n"
         "table{border-collapse:collapse;width:100%}td,th{padding:4px 6px;border-bottom:1px solid #eee;text-align:left}\n"
         "tr.sel{background:#ffe9a8}tr:hover{background:#f3f6ff;cursor:pointer}tr.done td{color:#999;text-decoration:line-through}\n"
         "svg{width:100%;max-height:46vh;background:#0b2a17;margin-bottom:8px}.board{fill:#1f6b3a;stroke:#ddd;stroke-width:.2}\n"
         ".court{fill:none;stroke:#9fc;stroke-width:.08}.pad{fill:#c9a24a}.part.hl .court{stroke:#ff0;stroke-width:.4}\n"
         ".part.hl .pad{fill:#f33}h3{margin:6px 0}\n</style></head><body>\n<div id=\"bom\"><table><tr><th>✓</th><th>Qty</th>"
         "<th>Value</th><th>Footprint</th><th>References</th></tr>\n";
    for (const BomLine& line : buildBom(sch)) {
        if (line.sourcing.dnp || line.embedded) continue;
        std::string refs;
        for (const auto& r : line.refs) refs += (refs.empty() ? "" : ",") + r;
        o << "<tr data-refs=\"" << html(refs) << "\"><td><input type=\"checkbox\"></td><td>" << line.quantity() << "</td><td>"
          << html(line.value) << "</td><td>" << html(line.footprint) << "</td><td>" << html(refs) << "</td></tr>\n";
    }
    o << "</table></div>\n<div id=\"boards\"><h3>Top</h3>" << svg[0].str() << "<h3>Bottom (seen from below)</h3>"
      << svg[1].str() << "</div>\n<script>\n"
         "const rows=[...document.querySelectorAll('tr[data-refs]')];\n"
         "rows.forEach(r=>{r.onclick=e=>{if(e.target.type==='checkbox'){r.classList.toggle('done',e.target.checked);return}\n"
         "rows.forEach(x=>x.classList.remove('sel'));r.classList.add('sel');const set=new Set(r.dataset.refs.split(','));\n"
         "document.querySelectorAll('.part').forEach(p=>p.classList.toggle('hl',set.has(p.dataset.ref)))}});\n"
         "document.querySelectorAll('.part').forEach(p=>p.onclick=()=>{const r=rows.find(x=>x.dataset.refs.split(',')"
         ".includes(p.dataset.ref));if(r){r.click();r.scrollIntoView({block:'center'})}});\n</script></body></html>\n";
    return o.str();
}

// ===================================================================== bring-up

std::string bringUpChecklist(const Project& project) {
    const Schematic& sch = project.schematic;
    std::ostringstream o;
    o << "# Bring-up checklist — " << project.name << "\n\n";
    o << "Work top to bottom; tick each line and write the measured value next to it.\n\n";
    o << "## 1. Before power\n\n";
    o << "- [ ] Inspect the board: solder bridges on fine-pitch parts, polarity of diodes, LEDs, electrolytics and "
         "connectors, pin 1 of every IC.\n";
    std::vector<int> rails;
    const int gnd = sch.groundNet();
    for (const auto& n : sch.nets()) {
        const NetRole role = sch.netRole(n.index);
        if (role == NetRole::Power || role == NetRole::NegativeSupply) rails.push_back(n.index);
    }
    for (int r : rails)
        o << "- [ ] Resistance " << sch.nets()[static_cast<size_t>(r)].name << " → "
          << (gnd >= 0 ? sch.nets()[static_cast<size_t>(gnd)].name : std::string("GND"))
          << " is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).\n";
    o << "\n## 2. First power\n\n";
    o << "- [ ] Bench supply at the lowest input voltage, current limit about 2× the expected idle current.\n";
    o << "- [ ] Note the input current at switch-on and after 10 s; feel for hot parts.\n\n";
    o << "## 3. Rails\n\n";
    if (rails.empty()) o << "No supply rails found by name or source (VCC, 3V3, +5V, VBAT …).\n";
    for (int r : rails) {
        const std::string& name = sch.nets()[static_cast<size_t>(r)].name;
        // Expected voltage from the name: 3V3 → 3.3, +5V → 5, 1V8 → 1.8, -12V → -12.
        std::string expect;
        std::string u = upper(name);
        for (size_t i = 0; i < u.size() && expect.empty(); ++i)
            if (std::isdigit(static_cast<unsigned char>(u[i]))) {
                size_t j = i;
                while (j < u.size() && std::isdigit(static_cast<unsigned char>(u[j]))) ++j;
                std::string whole = u.substr(i, j - i), frac;
                if (j < u.size() && (u[j] == 'V' || u[j] == '.')) {
                    size_t k = j + 1;
                    while (k < u.size() && std::isdigit(static_cast<unsigned char>(u[k]))) ++k;
                    frac = u.substr(j + 1, k - j - 1);
                    if (u[j] == '.' && k < u.size() && u[k] != 'V') frac.clear(), whole.clear();
                    if (!whole.empty())
                        expect = ((i > 0 && u[i - 1] == '-') || sch.netRole(r) == NetRole::NegativeSupply ? "-" : "") + whole +
                                 (frac.empty() ? "" : "." + frac) + " V";
                }
                i = j;
            }
        std::set<std::string> parts;
        std::string source;
        for (const auto& pin : sch.nets()[static_cast<size_t>(r)].pins) {
            const Component* c = sch.find(pin.component);
            if (!c) continue;
            if (c->hasFootprint()) parts.insert(c->ref);
            if (expect.empty() && (c->kind == ComponentKind::VoltageSource || c->kind == ComponentKind::Battery) &&
                !c->value.empty())  // a rail named N$1: the source's own voltage
                expect = c->value + (std::isalpha(static_cast<unsigned char>(c->value.back())) ? "" : " V"), source = c->ref;
        }
        o << "- [ ] " << name << (expect.empty() ? "" : " = " + expect + " ±5 %") << (source.empty() ? "" : " (from " + source + ")")
          << " — measure at the farthest load; ripple "
          << "under 50 mV p-p (" << parts.size() << " parts on the rail).\n";
    }
    o << "\n## 4. Clocks, resets, boot\n\n";
    bool any = false;
    for (const auto& c : sch.components()) {
        const std::string ref = upper(c.ref), name = upper(c.def().name + " " + c.value);
        if ((ref[0] == 'Y' || ref[0] == 'X' || name.find("CRYSTAL") != std::string::npos ||
             name.find("XTAL") != std::string::npos || name.find("OSC") != std::string::npos) &&
            c.hasFootprint()) {
            o << "- [ ] " << c.ref << " (" << c.value << ") oscillates: probe with a 10× probe on the output side; "
              << "check the frequency.\n";
            any = true;
        }
    }
    for (const auto& n : sch.nets()) {
        const std::string u = upper(n.name);
        for (const char* key : {"RST", "RESET", "NRST", "EN", "BOOT", "PWRGD", "PG"})
            if (u == key || u.find(std::string(key) + "_") == 0 || u.find("_" + std::string(key)) != std::string::npos ||
                (std::string(key).size() > 2 && u.find(key) != std::string::npos)) {
                o << "- [ ] " << n.name << " sits at its run level after power-up (high unless active-high).\n";
                any = true;
                break;
            }
    }
    if (!any) o << "No clocks or reset / enable nets found.\n";
    o << "\n## 5. Programming and interfaces\n\n";
    const std::vector<std::pair<std::string, std::vector<std::string>>> buses = {
        {"SWD / JTAG — connect the debugger, read the chip ID", {"SWDIO", "SWCLK", "TCK", "TMS", "TDI", "TDO"}},
        {"USB — enumerates on a host; D+ / D− not swapped", {"USB", "D+", "D-", "DP", "DM"}},
        {"UART — loop-back or console at the expected baud rate", {"TX", "RX", "UART"}},
        {"I2C — pull-ups present; a bus scan finds every device", {"SDA", "SCL", "I2C"}},
        {"SPI — read a device ID register", {"MOSI", "MISO", "SCK", "SCLK", "SPI"}},
        {"CAN — terminated 120 Ω at each end", {"CANH", "CANL", "CAN_"}},
    };
    any = false;
    for (const auto& [what, keys] : buses) {
        std::vector<std::string> hits;
        for (const auto& n : sch.nets()) {
            const std::string u = upper(n.name);
            for (const auto& k : keys)
                if (u.find(k) != std::string::npos) {
                    hits.push_back(n.name);
                    break;
                }
        }
        if (hits.empty()) continue;
        any = true;
        o << "- [ ] " << what << " (";
        for (size_t i = 0; i < hits.size() && i < 6; ++i) o << (i ? ", " : "") << hits[i];
        o << (hits.size() > 6 ? ", …" : "") << ").\n";
    }
    if (!any) o << "No programming or bus nets found by name.\n";
    o << "\n## 6. Sign-off\n\n- [ ] Record the board's serial number, revision and every measured value above.\n";
    return o.str();
}

// ===================================================================== Net lengths

std::string exportNetLengthReport(const Schematic& sch, const PcbLayout& pcb) {
    struct Row { double length = 0; int segments = 0, vias = 0; };
    std::map<int, Row> rows;
    for (const auto& t : pcb.tracks)
        if (t.net >= 0 && !t.teardrop) rows[t.net].length += trackLength(t), ++rows[t.net].segments;
    for (const auto& v : pcb.vias)
        if (v.net >= 0) ++rows[v.net].vias;
    std::ostringstream o;
    o << "Net,Length (mm),Segments,Vias,Target (mm),Tolerance (mm),Status\n";
    const auto& nets = sch.nets();
    for (const auto& [net, r] : rows) {
        if (net >= static_cast<int>(nets.size())) continue;
        const std::string& name = nets[static_cast<size_t>(net)].name;
        o << csv(name) << "," << num(r.length) << "," << r.segments << "," << r.vias;
        const auto rule = std::find_if(pcb.settings.lengthRules.begin(), pcb.settings.lengthRules.end(),
                                       [&](const LengthRule& l) { return l.net == name; });
        if (rule != pcb.settings.lengthRules.end())
            o << "," << num(rule->target) << "," << num(rule->tolerance) << ","
              << (std::fabs(r.length - rule->target) <= rule->tolerance + 1e-9 ? "met" : r.length < rule->target ? "short" : "long");
        else
            o << ",,,";
        o << "\n";
    }
    return o.str();
}

// ===================================================================== Gerber / Excellon viewing

namespace {

/// Points along an arc from a to b around c (counter-clockwise when ccw), b included; a full circle when a == b.
std::vector<Vec2> gerberArc(Vec2 a, Vec2 b, Vec2 c, bool ccw) {
    const double r = (a - c).length();
    double t0 = std::atan2(a.y - c.y, a.x - c.x), t1 = std::atan2(b.y - c.y, b.x - c.x);
    if (ccw && t1 <= t0 + 1e-12) t1 += 2 * M_PI;
    if (!ccw && t1 >= t0 - 1e-12) t1 -= 2 * M_PI;
    const int steps = std::clamp(static_cast<int>(std::fabs(t1 - t0) * std::max(r, 0.05) / 0.1), 4, 360);
    std::vector<Vec2> pts;
    for (int i = 1; i <= steps; ++i) {
        const double t = t0 + (t1 - t0) * i / steps;
        pts.push_back(i == steps ? b : Vec2{c.x + r * std::cos(t), c.y + r * std::sin(t)});
    }
    return pts;
}

void excellon(const std::string& text, GerberImage& img) {
    img.kind = "drill";
    double scale = 1;  // mm per unit
    bool metric = true;
    std::map<int, double> tools;
    double dia = 0;
    std::istringstream in(text);
    std::string line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line.rfind("INCH", 0) == 0 || line.rfind("M72", 0) == 0) metric = false, scale = 25.4;
        if (line.rfind("METRIC", 0) == 0 || line.rfind("M71", 0) == 0) metric = true, scale = 1;
        if (line[0] == 'T' && line.size() > 1 && std::isdigit(static_cast<unsigned char>(line[1]))) {
            const int t = std::atoi(line.c_str() + 1);
            const size_t c = line.find('C');
            if (c != std::string::npos) tools[t] = std::atof(line.c_str() + c + 1) * scale;
            dia = tools.count(t) ? tools[t] : 0;
            continue;
        }
        if (line[0] != 'X' && line[0] != 'Y') continue;
        static Vec2 last;
        auto coord = [&](char axis, double prev) {
            const size_t at = line.find(axis);
            if (at == std::string::npos) return prev;
            size_t end = at + 1;
            while (end < line.size() && (std::isdigit(static_cast<unsigned char>(line[end])) || line[end] == '.' || line[end] == '-' || line[end] == '+')) ++end;
            const std::string v = line.substr(at + 1, end - at - 1);
            if (v.empty()) return prev;
            const double n = std::atof(v.c_str());
            return (v.find('.') != std::string::npos ? n : n / (metric ? 1000.0 : 10000.0)) * scale;
        };
        last = {coord('X', last.x), coord('Y', last.y)};
        if (std::isfinite(last.x) && std::isfinite(last.y) && img.shapes.size() < 2000000) {
            GerberShape h;
            h.kind = GerberShape::Flash, h.a = last, h.width = h.height = dia > 0 ? dia : 0.3;
            img.shapes.push_back(h);
        }
    }
}

}  // namespace

GerberImage parseGerber(const std::string& text) {
    GerberImage img;
    img.kind = "gerber";
    if (text.find("M48") != std::string::npos && text.find("%FS") == std::string::npos) {
        excellon(text, img);
    } else {
        struct Aperture { char shape = 'C'; double w = 0.1, h = 0.1; };
        std::map<int, Aperture> apertures;
        int xDec = 6, yDec = 6, xInt = 3, yInt = 3, current = -1, interp = 1, dcode = 2;
        bool trailing = false, region = false, clear = false, multi = true, macroWarned = false;
        double scale = 1;
        Vec2 pos;
        std::vector<Vec2> contour;
        auto flushContour = [&] {
            if (contour.size() >= 3 && img.shapes.size() < 2000000) {
                GerberShape r;
                r.kind = GerberShape::Region, r.points = contour, r.clear = clear;
                img.shapes.push_back(std::move(r));
            }
            contour.clear();
        };
        auto number = [&](const std::string& v, int ints, int decs) {
            if (v.find('.') != std::string::npos) return std::atof(v.c_str()) * scale;
            std::string digits = v;
            const bool neg = !digits.empty() && digits[0] == '-';
            if (!digits.empty() && (digits[0] == '-' || digits[0] == '+')) digits.erase(0, 1);
            if (trailing) digits.resize(static_cast<size_t>(std::max<int>(static_cast<int>(digits.size()), ints + decs)), '0');
            const double n = std::atof(digits.c_str()) / std::pow(10.0, decs);
            return (neg ? -n : n) * scale;
        };
        auto statement = [&](const std::string& st) {
            if (st.empty()) return;
            if (st.rfind("FS", 0) == 0) {
                trailing = st.size() > 2 && st[2] == 'T';
                const size_t x = st.find('X'), y = st.find('Y');
                if (x != std::string::npos && x + 2 < st.size()) xInt = st[x + 1] - '0', xDec = st[x + 2] - '0';
                if (y != std::string::npos && y + 2 < st.size()) yInt = st[y + 1] - '0', yDec = st[y + 2] - '0';
                xDec = std::clamp(xDec, 0, 9), yDec = std::clamp(yDec, 0, 9);
            } else if (st == "MOMM") {
                scale = 1;
            } else if (st == "MOIN") {
                scale = 25.4;
            } else if (st == "LPD" || st == "LPC") {
                clear = st == "LPC";
            } else if (st.rfind("ADD", 0) == 0) {
                size_t i = 3;
                while (i < st.size() && std::isdigit(static_cast<unsigned char>(st[i]))) ++i;
                const int code = std::atoi(st.substr(3, i - 3).c_str());
                const size_t comma = st.find(',', i);
                const std::string shape = st.substr(i, comma == std::string::npos ? std::string::npos : comma - i);
                std::vector<double> params;
                if (comma != std::string::npos) {
                    std::string rest = st.substr(comma + 1);
                    size_t k = 0;
                    while (k <= rest.size()) {
                        const size_t xs = rest.find('X', k);
                        params.push_back(std::atof(rest.substr(k, xs == std::string::npos ? std::string::npos : xs - k).c_str()) * scale);
                        if (xs == std::string::npos) break;
                        k = xs + 1;
                    }
                }
                Aperture ap;
                const double first = params.empty() || !std::isfinite(params[0]) ? 0.1 : std::fabs(params[0]);
                ap.w = ap.h = first;
                if (shape == "C" || shape == "P") ap.shape = shape[0];
                else if (shape == "R" || shape == "O") ap.shape = shape[0], ap.h = params.size() > 1 && std::isfinite(params[1]) ? std::fabs(params[1]) : first;
                else if (!macroWarned) img.warnings.push_back("macro apertures drawn as circles"), macroWarned = true;
                apertures[code] = ap;
            } else if (st[0] == 'G' || st[0] == 'D' || st[0] == 'X' || st[0] == 'Y' || st[0] == 'I' || st[0] == 'J' || st[0] == 'M') {
                if (st.rfind("G04", 0) == 0) return;  // comment
                double x = pos.x, y = pos.y, ci = 0, cj = 0;
                size_t i = 0;
                while (i < st.size()) {
                    const char c = st[i++];
                    size_t end = i;
                    while (end < st.size() && (std::isdigit(static_cast<unsigned char>(st[end])) || st[end] == '-' || st[end] == '+' || st[end] == '.')) ++end;
                    const std::string v = st.substr(i, end - i);
                    i = end;
                    const int n = std::atoi(v.c_str());
                    switch (c) {
                        case 'G':
                            if (n == 1 || n == 2 || n == 3) interp = n;
                            else if (n == 36) region = true, contour.clear();
                            else if (n == 37) flushContour(), region = false;
                            else if (n == 74) multi = false;
                            else if (n == 75) multi = true;
                            else if (n == 70) scale = 25.4;
                            else if (n == 71) scale = 1;
                            break;
                        case 'X': x = number(v, xInt, xDec); break;
                        case 'Y': y = number(v, yInt, yDec); break;
                        case 'I': ci = number(v, xInt, xDec); break;
                        case 'J': cj = number(v, yInt, yDec); break;
                        case 'D':
                            if (n >= 10) current = n;
                            else dcode = n;
                            break;
                        default: break;
                    }
                }
                const bool moved = st.find_first_of("XY") != std::string::npos;
                if (!moved && st.find('D') == std::string::npos) return;
                if (!moved && !(st.find("D03") != std::string::npos)) return;
                const Vec2 to{x, y};
                if (!std::isfinite(to.x) || !std::isfinite(to.y)) return;
                const Aperture ap = apertures.count(current) ? apertures[current] : Aperture{};
                if (dcode == 1) {
                    std::vector<Vec2> path;
                    if (interp == 1 || std::hypot(ci, cj) < 1e-9) {  // a line (or an arc with no radius)
                        path.push_back(to);
                    } else {  // arc; single-quadrant mode picks the centre on the side of the shorter arc
                        Vec2 c = pos + Vec2{ci, cj};
                        if (!multi) {
                            const Vec2 options[] = {pos + Vec2{ci, cj}, pos + Vec2{-ci, cj}, pos + Vec2{ci, -cj}, pos + Vec2{-ci, -cj}};
                            double best = 1e300;
                            for (const Vec2& o : options) {
                                const double err = std::fabs((o - pos).length() - (o - to).length());
                                if (err < best) best = err, c = o;
                            }
                        }
                        path = gerberArc(pos, to, c, interp == 3);
                    }
                    if (region) {
                        if (contour.empty()) contour.push_back(pos);
                        contour.insert(contour.end(), path.begin(), path.end());
                    } else {
                        Vec2 from = pos;
                        for (const Vec2& p : path) {
                            if (img.shapes.size() >= 2000000) break;
                            GerberShape sg;
                            sg.kind = GerberShape::Stroke, sg.a = from, sg.b = p, sg.width = ap.w, sg.clear = clear;
                            img.shapes.push_back(sg);
                            from = p;
                        }
                    }
                } else if (dcode == 2) {
                    if (region) flushContour();
                } else if (dcode == 3 && img.shapes.size() < 2000000) {
                    GerberShape f;
                    f.kind = GerberShape::Flash, f.a = to, f.width = ap.w, f.height = ap.h, f.aperture = ap.shape, f.clear = clear;
                    img.shapes.push_back(f);
                }
                pos = to;
            }
        };
        size_t i = 0;
        while (i < text.size()) {
            if (text[i] == '%') {  // extended command: statements up to the closing '%'
                const size_t end = text.find('%', i + 1);
                const std::string block = text.substr(i + 1, end == std::string::npos ? std::string::npos : end - i - 1);
                if (block.rfind("AM", 0) != 0) {  // macro bodies are skipped
                    size_t k = 0;
                    while (k < block.size()) {
                        const size_t star = block.find('*', k);
                        std::string st = block.substr(k, star == std::string::npos ? std::string::npos : star - k);
                        st.erase(std::remove_if(st.begin(), st.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), st.end());
                        statement(st);
                        if (star == std::string::npos) break;
                        k = star + 1;
                    }
                }
                i = end == std::string::npos ? text.size() : end + 1;
            } else {
                const size_t star = text.find('*', i);
                std::string st = text.substr(i, star == std::string::npos ? std::string::npos : star - i);
                st.erase(std::remove_if(st.begin(), st.end(), [](char c) { return std::isspace(static_cast<unsigned char>(c)); }), st.end());
                statement(st);
                i = star == std::string::npos ? text.size() : star + 1;
            }
        }
        flushContour();
    }
    bool any = false;
    auto grow = [&](Vec2 p, double r) {
        if (!any) img.x0 = img.x1 = p.x, img.y0 = img.y1 = p.y, any = true;
        img.x0 = std::min(img.x0, p.x - r), img.x1 = std::max(img.x1, p.x + r);
        img.y0 = std::min(img.y0, p.y - r), img.y1 = std::max(img.y1, p.y + r);
    };
    for (const auto& sh : img.shapes) {
        if (sh.kind == GerberShape::Region) for (const Vec2& p : sh.points) grow(p, 0);
        else grow(sh.a, std::max(sh.width, sh.height) / 2);
        if (sh.kind == GerberShape::Stroke) grow(sh.b, sh.width / 2);
    }
    if (!any) img.error = "no drawing in the file (not a Gerber or Excellon file?)";
    return img;
}

std::string gerberImageJson(const GerberImage& img) {
    std::ostringstream o;
    o << "{\"kind\":\"" << img.kind << "\",\"bounds\":[" << num(img.x0) << "," << num(img.y0) << "," << num(img.x1) << "," << num(img.y1)
      << "],\"strokes\":[";
    std::string sep;
    std::vector<size_t> clears;
    for (size_t i = 0; i < img.shapes.size(); ++i) {
        const auto& sh = img.shapes[i];
        if (sh.kind != GerberShape::Stroke) continue;
        o << sep << "[" << num(sh.a.x) << "," << num(sh.a.y) << "," << num(sh.b.x) << "," << num(sh.b.y) << "," << num(sh.width) << "," << (sh.clear ? 1 : 0) << "]";
        sep = ",";
    }
    o << "],\"flashes\":[";
    sep.clear();
    for (const auto& sh : img.shapes) {
        if (sh.kind != GerberShape::Flash) continue;
        o << sep << "[" << num(sh.a.x) << "," << num(sh.a.y) << "," << num(sh.width) << "," << num(sh.height) << ",\"" << sh.aperture << "\"," << (sh.clear ? 1 : 0) << "]";
        sep = ",";
    }
    o << "],\"regions\":[";
    sep.clear();
    for (const auto& sh : img.shapes) {
        if (sh.kind != GerberShape::Region) continue;
        o << sep << "[" << (sh.clear ? 1 : 0);
        for (const Vec2& p : sh.points) o << "," << num(p.x) << "," << num(p.y);
        o << "]";
        sep = ",";
    }
    o << "],\"warnings\":[";
    sep.clear();
    for (const auto& w : img.warnings) o << sep << "\"" << w << "\"", sep = ",";
    o << "],\"error\":\"" << img.error << "\"}";
    return o.str();
}

}  // namespace sieda
