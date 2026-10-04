#include "sieda/Mesh.hpp"

#include "sieda/Embedded.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <map>
#include <string>

namespace sieda {

namespace {
Vec3 board3(Vec2 p, double y) { return {p.x, y, p.y}; }

/// Ear-clipping triangulation of a simple polygon; returns index triples.
std::vector<std::array<size_t, 3>> triangulate(const std::vector<Vec2>& poly) {
    std::vector<std::array<size_t, 3>> tris;
    const size_t n = poly.size();
    if (n < 3) return tris;
    double area = 0;
    for (size_t i = 0, j = n - 1; i < n; j = i++) area += poly[j].x * poly[i].y - poly[i].x * poly[j].y;
    std::vector<size_t> idx(n);
    for (size_t i = 0; i < n; ++i) idx[i] = area >= 0 ? i : n - 1 - i;  // counter-clockwise order
    auto cross = [](Vec2 a, Vec2 b, Vec2 c) { return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x); };
    size_t guard = 0;
    while (idx.size() > 3 && guard++ < 4 * n * n) {
        bool clipped = false;
        for (size_t k = 0; k < idx.size(); ++k) {
            size_t ia = idx[(k + idx.size() - 1) % idx.size()], ib = idx[k], ic = idx[(k + 1) % idx.size()];
            Vec2 a = poly[ia], b = poly[ib], c = poly[ic];
            if (cross(a, b, c) <= 1e-12) continue;  // reflex or degenerate
            bool inside = false;
            for (size_t m : idx) {
                if (m == ia || m == ib || m == ic) continue;
                Vec2 p = poly[m];
                if (cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0) { inside = true; break; }
            }
            if (inside) continue;
            tris.push_back({ia, ib, ic});
            idx.erase(idx.begin() + static_cast<long>(k));
            clipped = true;
            break;
        }
        if (!clipped) break;  // not simple — give up on the rest
    }
    if (idx.size() == 3) tris.push_back({idx[0], idx[1], idx[2]});
    return tris;
}

/// Single-stroke font on a 5 × 7 grid (x 0…4, y 0…6, y up), like the vector fonts PCB tools plot silkscreen with.
/// Each glyph is polylines of grid points ("xy" pairs), strokes separated by '|'.
const std::map<char, const char*>& strokeFont() {
    static const std::map<char, const char*> font = {
        {'0', "00 40 46 06 00|00 46"}, {'1', "15 26 20|10 30"}, {'2', "05 16 36 45 44 00 40"},
        {'3', "05 16 36 45 44 33 42 41 30 10 01|13 33"}, {'4', "30 36 02 42"}, {'5', "46 06 03 33 42 41 30 00"},
        {'6', "45 36 16 05 01 10 30 41 42 33 03"}, {'7', "06 46 20"},
        {'8', "13 04 05 16 36 45 44 33 13 02 01 10 30 41 42 33"}, {'9', "01 10 30 41 45 36 16 05 04 13 43"},
        {'A', "00 04 26 44 40|03 43"}, {'B', "00 06 36 45 44 33 03|33 42 41 30 00"},
        {'C', "45 36 16 05 01 10 30 41"}, {'D', "00 06 26 44 42 20 00"}, {'E', "40 00 06 46|03 33"},
        {'F', "00 06 46|03 33"}, {'G', "45 36 16 05 01 10 30 41 43 23"}, {'H', "00 06|40 46|03 43"},
        {'I', "10 30|16 36|20 26"}, {'J', "46 41 30 10 01"}, {'K', "00 06|46 02|13 40"}, {'L', "06 00 40"},
        {'M', "00 06 23 46 40"}, {'N', "00 06 40 46"}, {'O', "10 01 05 16 36 45 41 30 10"},
        {'P', "00 06 36 45 44 33 03"}, {'Q', "10 01 05 16 36 45 41 30 10|22 40"}, {'R', "00 06 36 45 44 33 03|23 40"},
        {'S', "45 36 16 05 04 13 33 42 41 30 10 01"}, {'T', "06 46|26 20"}, {'U', "06 01 10 30 41 46"},
        {'V', "06 20 46"}, {'W', "06 10 23 30 46"}, {'X', "00 46|06 40"}, {'Y', "06 23 46|23 20"},
        {'Z', "06 46 00 40"}, {'-', "03 43"}, {'+', "03 43|21 25"}, {'.', "20 21"}, {'/', "00 46"},
        {'_', "00 40"}, {'?', "05 16 36 45 44 22|21 20"}, {' ', ""},
    };
    return font;
}

constexpr double kGlyphAdvance = 6.0;  // grid units per character (4 wide + 2 gap)
}  // namespace

double Mesh::textWidth(const std::string& text, double height) {
    if (text.empty()) return 0;
    const double unit = height / 6.0;
    return (static_cast<double>(text.size()) * kGlyphAdvance - 2.0) * unit;
}

double Mesh::addText(const std::string& text, Vec2 centre, double height, double y0, double y1, Rgba color,
                     bool mirrored) {
    const double unit = height / 6.0, width = textWidth(text, height);
    const double stroke = std::max(0.08, height * 0.13);
    const auto& font = strokeFont();
    double x = -width / 2;  // left edge of the current glyph, relative to the centre
    for (char raw : text) {
        char ch = static_cast<char>(std::toupper(static_cast<unsigned char>(raw)));
        auto it = font.find(ch);
        if (it == font.end()) it = font.find('?');
        Vec2 prev;
        bool havePrev = false;
        for (const char* p = it->second; *p; ++p) {
            if (*p == '|') { havePrev = false; continue; }
            if (*p == ' ' || !std::isdigit(static_cast<unsigned char>(p[0])) || !p[1]) continue;
            double gx = p[0] - '0', gy = p[1] - '0';
            ++p;
            double lx = x + gx * unit, ly = (gy - 3.0) * unit;  // y up, centred on the text's mid-height
            Vec2 pt{centre.x + (mirrored ? -lx : lx), centre.y - ly};  // board Y runs down
            if (havePrev) addSegmentBox(prev, pt, stroke, y0, y1, color);
            prev = pt;
            havePrev = true;
        }
        x += kGlyphAdvance * unit;
    }
    return width;
}

size_t Mesh::surfaceVertices(Surface s) const {
    return static_cast<size_t>(std::count(surfaces.begin(), surfaces.end(), static_cast<uint8_t>(s)));
}

void Mesh::addPrism(const std::vector<Vec2>& poly, double y0, double y1, Rgba c) {
    for (const auto& t : triangulate(poly)) {
        Vec2 a = poly[t[0]], b = poly[t[1]], d = poly[t[2]];
        // Polygon is CCW in board XY; board Y maps to +Z, so top faces (normal +Y) use the reversed order.
        addQuad(board3(a, y1), board3(d, y1), board3(b, y1), board3(a, y1), c);
        addQuad(board3(a, y0), board3(b, y0), board3(d, y0), board3(a, y0), c);
    }
    for (size_t i = 0; i < poly.size(); ++i) {
        Vec2 s = poly[i], e = poly[(i + 1) % poly.size()];
        addQuad(board3(s, y0), board3(s, y1), board3(e, y1), board3(e, y0), c);
    }
}

void Mesh::addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Rgba color) {
    Vec3 n = (b - a).cross(c - a).normalized();
    uint32_t base = static_cast<uint32_t>(vertexCount());
    for (const Vec3& v : {a, b, c, d}) {
        positions.insert(positions.end(), {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
        normals.insert(normals.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
        colors.insert(colors.end(), {color.r, color.g, color.b, color.a});
    }
    surfaces.insert(surfaces.end(), 4, static_cast<uint8_t>(surface));
    indices.insert(indices.end(), {base, base + 1, base + 2, base, base + 2, base + 3});
}

void Mesh::addBox(Vec3 mn, Vec3 mx, Rgba c) {
    double x0 = mn.x, y0 = mn.y, z0 = mn.z, x1 = mx.x, y1 = mx.y, z1 = mx.z;
    addQuad({x0, y1, z0}, {x0, y1, z1}, {x1, y1, z1}, {x1, y1, z0}, c);  // +Y
    addQuad({x0, y0, z0}, {x1, y0, z0}, {x1, y0, z1}, {x0, y0, z1}, c);  // -Y
    addQuad({x1, y0, z0}, {x1, y1, z0}, {x1, y1, z1}, {x1, y0, z1}, c);  // +X
    addQuad({x0, y0, z0}, {x0, y0, z1}, {x0, y1, z1}, {x0, y1, z0}, c);  // -X
    addQuad({x0, y0, z1}, {x1, y0, z1}, {x1, y1, z1}, {x0, y1, z1}, c);  // +Z
    addQuad({x0, y0, z0}, {x0, y1, z0}, {x1, y1, z0}, {x1, y0, z0}, c);  // -Z
}

void Mesh::addSegmentBox(Vec2 a, Vec2 b, double width, double y0, double y1, Rgba c) {
    Vec2 d = b - a;
    double len = d.length();
    Vec2 u = len > 1e-9 ? d * (1.0 / len) : Vec2{1, 0};
    Vec2 p{-u.y, u.x};
    double hw = width / 2;
    // Extend by half the width so consecutive segments overlap cleanly at corners.
    Vec2 a2 = a - u * hw, b2 = b + u * hw;
    Vec2 q0 = a2 - p * hw, q1 = b2 - p * hw, q2 = b2 + p * hw, q3 = a2 + p * hw;
    // Ensure a consistent winding: if (q1-q0)x(q3-q0) points down in board space, swap sides.
    double crossY = (q1.x - q0.x) * (q3.y - q0.y) - (q1.y - q0.y) * (q3.x - q0.x);
    if (crossY < 0) {
        std::swap(q0, q3);
        std::swap(q1, q2);
    }
    // In XZ with Y up, top face must be wound so that normal is +Y: order q0, q3, q2, q1.
    addQuad(board3(q0, y1), board3(q3, y1), board3(q2, y1), board3(q1, y1), c);
    addQuad(board3(q0, y0), board3(q1, y0), board3(q2, y0), board3(q3, y0), c);
    const Vec2 ring[4] = {q0, q1, q2, q3};
    for (int i = 0; i < 4; ++i) {
        Vec2 s = ring[i], e = ring[(i + 1) % 4];
        addQuad(board3(s, y0), board3(s, y1), board3(e, y1), board3(e, y0), c);
    }
}

void Mesh::addCylinder(Vec3 base, double radius, double height, Rgba c, int segments) {
    for (int i = 0; i < segments; ++i) {
        double a0 = 2 * kPi * i / segments, a1 = 2 * kPi * (i + 1) / segments;
        Vec3 p0{base.x + radius * std::cos(a0), base.y, base.z + radius * std::sin(a0)};
        Vec3 p1{base.x + radius * std::cos(a1), base.y, base.z + radius * std::sin(a1)};
        Vec3 t0 = p0 + Vec3{0, height, 0}, t1 = p1 + Vec3{0, height, 0};
        addQuad(p0, t0, t1, p1, c);  // side (outward because angle increases toward +Z from +X)
        Vec3 bc = base, tc = base + Vec3{0, height, 0};
        addQuad(tc, t1, t0, tc, c);  // top fan triangle (degenerate quad)
        addQuad(bc, p0, p1, bc, c);  // bottom fan triangle
    }
}

double copperLayerBase(int layer, int layerCount, double thickness, double copper) {
    if (layer <= 0) return 0.0;
    int bottom = layerCount > 1 ? layerCount - 1 : 0;
    if (layer >= bottom) return -thickness - copper;
    // Inner layers are evenly spaced through the core.
    return -thickness * static_cast<double>(layer) / static_cast<double>(bottom) - copper / 2;
}

Mesh buildCopperLayerMesh(const Schematic& sch, const PcbLayout& pcb, int layer) {
    Mesh m;
    m.surface = Surface::Finish;
    const double cu = 0.035;
    const Rgba trace{0.35f, 0.75f, 1.0f, 1.0f};
    const Rgba pad{0.80f, 0.92f, 1.0f, 1.0f};
    const Rgba pour{0.22f, 0.55f, 0.90f, 1.0f};
    for (const auto& f : pcb.zoneFills(sch))
        if (f.layer == layer)
            for (const auto& r : f.rects) m.addBox({r.x0, 0.0, r.y0}, {r.x1, cu * 0.9, r.y1}, pour);
    for (const auto& tr : pcb.tracks)
        if (tr.layer == layer) m.addSegmentBox(tr.a, tr.b, tr.width, 0.0, cu, trace);
    for (const auto& p : pcb.pads(sch)) {
        if (!p.onLayer(layer)) continue;
        if (p.round) m.addCylinder({p.position.x, 0.0, p.position.y}, std::min(p.size.x, p.size.y) / 2, cu, pad, 16);
        else m.addBox({p.position.x - p.size.x / 2, 0.0, p.position.y - p.size.y / 2},
                      {p.position.x + p.size.x / 2, cu, p.position.y + p.size.y / 2}, pad);
    }
    for (const auto& v : pcb.vias)
        if (v.spans(layer)) m.addCylinder({v.position.x, 0.0, v.position.y}, v.diameter / 2, cu, pad, 14);
    return m;
}

const std::vector<SolderMaskStyle>& solderMaskStyles() {
    static const std::vector<SolderMaskStyle> styles = {
        {"green", {0.04f, 0.40f, 0.20f, 1}, {0.10f, 0.53f, 0.27f, 1}, {0.96f, 0.96f, 0.94f, 1}},
        {"black", {0.05f, 0.05f, 0.06f, 1}, {0.13f, 0.13f, 0.14f, 1}, {0.96f, 0.96f, 0.94f, 1}},
        {"blue", {0.04f, 0.13f, 0.42f, 1}, {0.09f, 0.22f, 0.60f, 1}, {0.96f, 0.96f, 0.94f, 1}},
        {"red", {0.76f, 0.04f, 0.04f, 1}, {0.92f, 0.12f, 0.10f, 1}, {0.97f, 0.97f, 0.95f, 1}},
        {"yellow", {0.93f, 0.74f, 0.04f, 1}, {0.98f, 0.62f, 0.08f, 1}, {0.04f, 0.04f, 0.05f, 1}},
        {"white", {0.92f, 0.93f, 0.94f, 1}, {0.84f, 0.85f, 0.86f, 1}, {0.04f, 0.04f, 0.05f, 1}},
        {"purple", {0.32f, 0.12f, 0.44f, 1}, {0.52f, 0.14f, 0.66f, 1}, {0.97f, 0.97f, 0.95f, 1}},
    };
    return styles;
}

const SolderMaskStyle* findSolderMask(const std::string& name) {
    for (const auto& style : solderMaskStyles())
        if (name == style.name) return &style;
    return nullptr;
}

Mesh buildAssemblyMesh(const Schematic& sch, const PcbLayout& pcb, const MeshOptions& opt) {
    Mesh m;
    const BoardSettings& s = pcb.settings;
    const double t = s.thickness;
    const double cu = 0.035;
    const SolderMaskStyle* style = findSolderMask(s.solderMask);
    if (!style) style = &solderMaskStyles().front();
    const Rgba mask = style->mask;
    const Rgba trackColor = style->maskOverCopper;
    const Rgba gold{0.86f, 0.70f, 0.30f, 1.0f};
    const Rgba silk = style->silk;
    const Rgba fr4Edge{0.75f, 0.68f, 0.45f, 1.0f};

    // The board is built from its stack-up, top to bottom: solder mask, FR-4 laminate with a copper band at each inner
    // layer (visible on the board edge of 4- and 6-layer boards), and solder mask underneath — except on a single-sided
    // board, whose underside is bare FR-4.
    auto slab = [&](double y0, double y1, Rgba colour) {
        if (y1 - y0 <= 1e-6) return;
        const bool isMask = colour.r == mask.r && colour.g == mask.g && colour.b == mask.b;
        m.surface = isMask ? Surface::Mask : Surface::Laminate;
        if (s.hasCustomOutline()) m.addPrism(s.outline, y0, y1, colour);
        else m.addBox({0, y0, 0}, {s.width, y1, s.height}, colour);
    };
    const double skin = std::min(0.03, t / 8);
    const Rgba innerCopper{0.80f, 0.52f, 0.28f, 1.0f};
    const int layers = std::max(1, s.layerCount);
    const int bottomLayer = layers > 1 ? layers - 1 : 0;
    slab(-skin, 0, mask);
    double y = -skin;  // top of the laminate still to be built
    for (int layer = 1; layer < bottomLayer; ++layer) {
        double centre = copperLayerBase(layer, layers, t, cu) + cu / 2;
        double band = std::min(0.07, t / (4.0 * layers));
        slab(centre + band / 2, y, fr4Edge);
        slab(centre - band / 2, centre + band / 2, innerCopper);
        y = centre - band / 2;
    }
    double underside = layers > 1 ? -t + skin : -t;
    slab(underside, y, fr4Edge);
    if (layers > 1) slab(-t, underside, mask);
    // Mounting holes: dark bore through the board with a bare-FR-4 ring for the screw head.
    const Rgba bore{0.02f, 0.03f, 0.05f, 1.0f};
    for (const auto& h : s.holes) {
        m.surface = Surface::Hole;
        m.addCylinder({h.position.x, -t - 0.02, h.position.y}, h.drill / 2, t + 0.04, bore, 24);
        m.surface = Surface::Laminate;
        m.addCylinder({h.position.x, 0.0, h.position.y}, h.keepout / 2, 0.005, fr4Edge, 32);
    }

    auto ps = pcb.pads(sch);
    // Which components are drawn (their pads carry solder joints; bare pads show the copper finish).
    auto populated = [&](int componentId) {
        if (!opt.components) return false;
        const Component* c = sch.find(componentId);
        return c && c->hasFootprint() && c->pcb.placed && !embeddedElement(*c, s);
    };
    const Rgba solder{0.80f, 0.81f, 0.83f, 1.0f};
    const Rgba tin{0.78f, 0.78f, 0.80f, 1.0f};
    if (opt.copper) {
        const Rgba pourColor{(mask.r + trackColor.r) / 2, (mask.g + trackColor.g) / 2, (mask.b + trackColor.b) / 2, 1.0f};
        // Tracks and pours lie under the mask: they read as raised, slightly lighter mask.
        m.surface = Surface::Mask;
        for (const auto& f : pcb.zoneFills(sch)) {
            double y0 = copperLayerBase(f.layer, s.layerCount, t, cu);
            for (const auto& r : f.rects) m.addBox({r.x0, y0, r.y0}, {r.x1, y0 + cu * 0.9, r.y1}, pourColor);
        }
        for (const auto& tr : pcb.tracks) {
            double y0 = copperLayerBase(tr.layer, s.layerCount, t, cu);
            m.addSegmentBox(tr.a, tr.b, tr.width, y0, y0 + cu, trackColor);
        }
        for (const auto& p : ps) {
            const bool joint = populated(p.componentId);
            if (p.throughHole) {
                for (bool top : {true, false}) {
                    if (!top && layers == 1) continue;  // single-sided: copper on the top layer only
                    double y = top ? 0.0 : -t - cu;
                    m.surface = Surface::Finish;
                    if (p.round) m.addCylinder({p.position.x, y, p.position.y}, p.size.x / 2, cu, gold);
                    else
                        m.addBox({p.position.x - p.size.x / 2, y, p.position.y - p.size.y / 2},
                                 {p.position.x + p.size.x / 2, y + cu, p.position.y + p.size.y / 2}, gold);
                }
                // The drilled hole: dark where no lead fills it.
                m.surface = Surface::Hole;
                m.addCylinder({p.position.x, -t - cu - 0.01, p.position.y}, std::max(0.15, p.drill / 2), t + 2 * cu + 0.02,
                              bore, 16);
                if (joint) {
                    // Solder joint on the solder side (opposite the part): a stepped cone of solder hugging the lead.
                    const Component* part = sch.find(p.componentId);
                    const bool jointOnTop = part && part->pcb.bottom;
                    const double r0 = std::min(p.size.x, p.size.y) / 2 * 0.92;
                    m.surface = Surface::Solder;
                    if (jointOnTop) {
                        m.addCylinder({p.position.x, cu, p.position.y}, r0, 0.08, solder, 20);
                        m.addCylinder({p.position.x, cu, p.position.y}, r0 * 0.55, 0.18, solder, 16);
                    } else {
                        m.addCylinder({p.position.x, -t - cu - 0.18, p.position.y}, r0 * 0.55, 0.18, solder, 16);
                        m.addCylinder({p.position.x, -t - cu - 0.08, p.position.y}, r0, 0.08, solder, 20);
                    }
                }
            } else {
                // Surface pads, or the terminations / plates of an embedded passive on an inner layer.
                double y0 = p.smdLayer == kTopLayer ? 0.0 : p.bottom ? -t - cu : copperLayerBase(p.smdLayer, layers, t, cu);
                m.surface = Surface::Finish;
                m.addBox({p.position.x - p.size.x / 2, y0, p.position.y - p.size.y / 2},
                         {p.position.x + p.size.x / 2, y0 + cu, p.position.y + p.size.y / 2}, gold);
                const bool outer = p.smdLayer == kTopLayer || p.bottom;
                if (joint && outer) {
                    // Reflowed solder: a low, slightly inset dome over the pad (it wets up the part's terminal).
                    const double inset = std::min(p.size.x, p.size.y) * 0.08, h = 0.06;
                    const double ya = p.bottom ? y0 - h : y0 + cu, yb = p.bottom ? y0 : y0 + cu + h;
                    m.surface = Surface::Solder;
                    m.addBox({p.position.x - p.size.x / 2 + inset, ya, p.position.y - p.size.y / 2 + inset},
                             {p.position.x + p.size.x / 2 - inset, yb, p.position.y + p.size.y / 2 - inset}, solder);
                }
            }
        }
        // Embedded resistors: the dark thin-film foil between their terminations.
        m.surface = Surface::Laminate;
        for (const auto& e : embeddedElements(sch, s)) {
            if (!e.resistor) continue;
            const Rect r = e.bounds();
            const double y0 = copperLayerBase(e.layer, layers, t, cu);
            m.addBox({r.x0, y0, r.y0}, {r.x1, y0 + cu * 0.6, r.y1}, Rgba{0.18f, 0.16f, 0.14f, 1.0f});
        }
        for (const auto& v : pcb.vias) {
            // The barrel runs between the copper layers it spans (blind / buried / microvias stop inside the board);
            // an outer layer it reaches shows a plated land around the drilled hole.
            const double top = copperLayerBase(v.fromLayer, layers, t, cu) + cu;
            const double bottom = copperLayerBase(v.lastLayer(layers), layers, t, cu);
            m.surface = Surface::Finish;
            if (v.fromLayer <= 0) m.addCylinder({v.position.x, 0.0, v.position.y}, v.diameter / 2, cu, gold, 18);
            if (v.lastLayer(layers) >= bottomLayer && layers > 1)
                m.addCylinder({v.position.x, -t - cu, v.position.y}, v.diameter / 2, cu, gold, 18);
            m.surface = Surface::Hole;
            m.addCylinder({v.position.x, bottom - 0.005, v.position.y}, std::max(0.05, v.drill / 2), top - bottom + 0.01,
                          bore, 12);
        }
    }

    if (opt.silkscreen) {
        m.surface = Surface::Silk;
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed || c.pcb.embedded()) continue;
            Rect r = pcb.courtyard(c).inflated(-0.15);
            double y0 = c.pcb.bottom ? -t - 0.045 : cu, y1 = c.pcb.bottom ? -t - cu : cu + 0.01;
            Vec2 p0{r.x0, r.y0}, p1{r.x1, r.y0}, p2{r.x1, r.y1}, p3{r.x0, r.y1};
            m.addSegmentBox(p0, p1, 0.12, y0, y1, silk);
            m.addSegmentBox(p1, p2, 0.12, y0, y1, silk);
            m.addSegmentBox(p2, p3, 0.12, y0, y1, silk);
            m.addSegmentBox(p3, p0, 0.12, y0, y1, silk);
            // Reference designator just above the outline (scaled down for tiny parts and long names).
            if (!c.ref.empty()) {
                double h = std::clamp(std::min(r.width(), r.height()) * 0.55, 0.6, 1.0);
                const double maxWidth = std::max(r.width() * 1.6, 3.0);
                if (Mesh::textWidth(c.ref, h) > maxWidth) h *= maxWidth / Mesh::textWidth(c.ref, h);
                Vec2 at{(r.x0 + r.x1) / 2, r.y0 - 0.25 - h / 2};
                m.addText(c.ref, at, h, y0, y1, silk, c.pcb.bottom);
            }
            // Pin-1 dot for multi-pin packages, just outside pad 1's corner of the outline.
            int padCount = 0;
            const Pad* first = nullptr;
            for (const auto& p : ps) {
                if (p.componentId != c.id) continue;
                ++padCount;
                if (p.padNumber == 1) first = &p;
            }
            if (padCount > 2 && first) {
                Vec2 corner{first->position.x < (r.x0 + r.x1) / 2 ? r.x0 - 0.35 : r.x1 + 0.35,
                            first->position.y < (r.y0 + r.y1) / 2 ? r.y0 - 0.35 : r.y1 + 0.35};
                m.addCylinder({corner.x, y0, corner.y}, 0.18, y1 - y0, silk, 12);
            }
        }
    }

    if (opt.components) {
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed || embeddedElement(c, s)) continue;  // embedded: inside the board
            const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
            if (!fp) continue;
            const BodyDef& b = fp->body;
            double w = b.width, d = b.depth;
            if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) std::swap(w, d);
            Rgba col{b.r, b.g, b.b, 1.0f};
            const bool chip = fp->pads.size() == 2 && !fp->pads[0].throughHole &&
                              (fp->name.rfind("R_", 0) == 0 || fp->name.rfind("C_", 0) == 0 ||
                               fp->name.rfind("L_", 0) == 0 || fp->name.rfind("Fuse_", 0) == 0);
            const bool led = c.kind == ComponentKind::LED;
            if (led) {
                std::string v = c.value;
                for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (v.find("green") != std::string::npos) col = {0.15f, 0.85f, 0.25f, 0.9f};
                else if (v.find("blue") != std::string::npos) col = {0.2f, 0.35f, 0.95f, 0.9f};
                else if (v.find("yellow") != std::string::npos) col = {0.95f, 0.85f, 0.15f, 0.9f};
                else if (v.find("white") != std::string::npos) col = {0.95f, 0.95f, 0.92f, 0.9f};
                else col = {0.92f, 0.15f, 0.12f, 0.9f};
            }
            const bool bottomSide = c.pcb.bottom;
            const double base = bottomSide ? -t - cu : cu;
            const double y0 = bottomSide ? base - b.height : base, y1 = bottomSide ? base : base + b.height;
            const double topFace = bottomSide ? y0 : y1;  // the face that points away from the board
            const double out = bottomSide ? -1.0 : 1.0;   // away from the board
            const Vec2 pc = c.pcb.position;
            m.surface = led ? Surface::Glass : chip ? Surface::Ceramic : Surface::Plastic;
            if (b.cylinder) {
                m.addCylinder({pc.x, y0, pc.y}, std::min(w, d) / 2, b.height, col, 28);
                if (!led) {
                    // Aluminium can top (electrolytics) with its pressure-vent cross.
                    const double r = std::min(w, d) / 2 * 0.9;
                    m.surface = Surface::Tin;
                    m.addCylinder({pc.x, bottomSide ? y0 - 0.02 : y1, pc.y}, r, 0.02, tin, 28);
                    m.surface = Surface::Hole;
                    const double vy0 = bottomSide ? y0 - 0.03 : y1 + 0.02, vy1 = bottomSide ? y0 - 0.02 : y1 + 0.03;
                    m.addSegmentBox({pc.x - r * 0.6, pc.y}, {pc.x + r * 0.6, pc.y}, 0.08, vy0, vy1, bore);
                    m.addSegmentBox({pc.x, pc.y - r * 0.6}, {pc.x, pc.y + r * 0.6}, 0.08, vy0, vy1, bore);
                }
            } else {
                m.addBox({pc.x - w / 2, y0, pc.y - d / 2}, {pc.x + w / 2, y1, pc.y + d / 2}, col);
            }
            // Metallic terminations on two-terminal SMD chips (resistors, capacitors, inductors, fuses).
            if (chip) {
                m.surface = Surface::Tin;
                for (const auto& p : ps) {
                    if (p.componentId != c.id) continue;
                    Vec2 toPad = p.position - pc;
                    double len = toPad.length();
                    Vec2 u = len > 1e-9 ? toPad * (1.0 / len) : Vec2{1, 0};
                    double halfAlong = (std::fabs(u.x) > 0.5 ? w : d) / 2;
                    double capLen = std::min(0.45, halfAlong * 0.45);
                    Vec2 cc = pc + u * (halfAlong - capLen / 2 + 0.01);
                    double cw = std::fabs(u.x) > 0.5 ? capLen : w + 0.02;
                    double cd = std::fabs(u.x) > 0.5 ? d + 0.02 : capLen;
                    m.addBox({cc.x - cw / 2, y0 - 0.005, cc.y - cd / 2}, {cc.x + cw / 2, y1 + 0.01, cc.y + cd / 2}, tin);
                }
            }
            // Gull-wing leads on leaded SMD packages (SOIC, SOT, QFP…): out of the body side at mid-height, bent
            // down onto the pad. Pads under the body (QFN, BGA) stay hidden.
            const bool smdMulti = !chip && !led && !b.cylinder && fp->pads.size() > 2 && !fp->pads[0].throughHole;
            if (smdMulti) {
                m.surface = Surface::Tin;
                for (const auto& p : ps) {
                    if (p.componentId != c.id || p.throughHole) continue;
                    Vec2 rel = p.position - pc;
                    const bool alongX = std::fabs(rel.x) - w / 2 > std::fabs(rel.y) - d / 2;
                    const double edge = alongX ? w / 2 : d / 2, reach = alongX ? std::fabs(rel.x) : std::fabs(rel.y);
                    if (reach <= edge) continue;  // under the body
                    const double sign = (alongX ? rel.x : rel.y) < 0 ? -1.0 : 1.0;
                    const double lw = std::clamp(std::min(p.size.x, p.size.y) * 0.6, 0.15, 0.5), th = 0.12;
                    const double armY = base + out * std::min(b.height * 0.45, 0.6);
                    auto pt = [&](double along) {
                        return alongX ? Vec2{pc.x + sign * along, p.position.y} : Vec2{p.position.x, pc.y + sign * along};
                    };
                    const double knee = edge + std::max(0.12, (reach - edge) * 0.35);
                    const double footEnd = reach + std::min(p.size.x, p.size.y) * 0.25;
                    auto yr = [&](double a, double bb) { return std::make_pair(std::min(a, bb), std::max(a, bb)); };
                    auto [a0, a1] = yr(armY, armY + out * th);
                    m.addSegmentBox(pt(edge - 0.05), pt(knee), lw, a0, a1, tin);  // shoulder
                    auto [d0, d1] = yr(base, armY + out * th);
                    m.addSegmentBox(pt(knee), pt(knee + 0.001), lw, d0, d1, tin);  // bend down
                    auto [f0, f1] = yr(base, base + out * th);
                    m.addSegmentBox(pt(knee), pt(footEnd), lw, f0, f1, tin);  // foot on the pad
                }
                // Pin-1 dimple and laser-etched part marking on the package top.
                const Pad* first = nullptr;
                for (const auto& p : ps)
                    if (p.componentId == c.id && p.padNumber == 1) first = &p;
                m.surface = Surface::Marking;
                const Rgba etch{0.62f, 0.63f, 0.65f, 1.0f};
                const double my0 = bottomSide ? topFace - 0.006 : topFace, my1 = bottomSide ? topFace : topFace + 0.006;
                if (first) {
                    Vec2 dir = first->position - pc;
                    Vec2 dimple{pc.x + std::clamp(dir.x, -w / 2 + 0.6, w / 2 - 0.6) * 0.8,
                                pc.y + std::clamp(dir.y, -d / 2 + 0.6, d / 2 - 0.6) * 0.8};
                    if (w > 1.4 && d > 1.4) m.addCylinder({dimple.x, my0, dimple.y}, std::min(w, d) * 0.07, my1 - my0, etch, 14);
                }
                std::string mark = c.value.empty() ? c.ref : c.value;
                if (mark.size() > 12) mark.resize(12);
                double mh = std::min(d * 0.22, 1.1);
                if (mh >= 0.35) {
                    if (Mesh::textWidth(mark, mh) > w * 0.8) mh *= w * 0.8 / Mesh::textWidth(mark, mh);
                    if (mh >= 0.3) m.addText(mark, pc, mh, my0, my1, etch, bottomSide);
                }
            }
            // Through-hole pins: header posts stand above the plastic; every lead pokes out under the board.
            const bool header = fp->name.find("Header") != std::string::npos || fp->name.find("Pin") != std::string::npos ||
                                c.kind == ComponentKind::Connector;
            for (const auto& p : ps) {
                if (p.componentId != c.id || !p.throughHole) continue;
                if (header) {
                    m.surface = Surface::Gold;
                    double postA = bottomSide ? y0 - 6.0 : y1, postB = bottomSide ? y0 : y1 + 6.0;
                    m.addBox({p.position.x - 0.32, postA, p.position.y - 0.32}, {p.position.x + 0.32, postB, p.position.y + 0.32},
                             gold);
                }
                m.surface = header ? Surface::Gold : Surface::Tin;
                const double r = header ? 0.32 : std::min(0.3, std::max(0.2, p.drill * 0.35));
                double tailA = bottomSide ? cu : -t - cu - (header ? 2.5 : 1.2), tailB = bottomSide ? cu + (header ? 2.5 : 1.2) : -t - cu;
                m.addBox({p.position.x - r, tailA, p.position.y - r}, {p.position.x + r, tailB, p.position.y + r}, header ? gold : tin);
                if (!header) {
                    // The lead also bridges from the board up into the body.
                    const double la = bottomSide ? y1 : cu, lb = bottomSide ? -t - cu : y0;
                    if (lb > la) m.addBox({p.position.x - r, la, p.position.y - r}, {p.position.x + r, lb, p.position.y + r}, tin);
                }
            }
        }
    }
    return m;
}

}  // namespace sieda
