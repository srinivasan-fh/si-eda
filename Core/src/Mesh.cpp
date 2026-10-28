#include "sieda/Mesh.hpp"
#include "sieda/Model3D.hpp"

#include "sieda/Embedded.hpp"
#include "sieda/StandardParts.hpp"

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

/// The look a part's body is drawn with, from its footprint, part name, package and description: a generic box with
/// leads unless it is a kind of part people recognise by its shape.
enum class Look {
    Generic, Header, UsbC, UsbMicro, UsbA, Tactile, CrystalCan, CrystalSmd, Module, Diode, Led, Jst,
    // Through-hole power hardware: bobbin transformer, can electrolytic, upright toroid / choke, TO-220 / TO-247
    // with their tab, single-in-line bridge, screw terminal, fuse holder, boxed film capacitor, disc (ceramic /
    // safety cap, varistor, NTC),
    // axial resistor / diode.
    Transformer, Electrolytic, Toroid, To220, To247, SipBridge, ScrewTerminal, FuseHolder, Film, Disc, Axial
};

/// Upper-case words a part's look is read from: footprint, value, part name, package, description and the KiCad
/// footprint a catalog part follows.
std::string lookKey(const Component& c, const FootprintDef& fp) {
    std::string key = fp.name + " " + c.value;
    if (c.kind == ComponentKind::Custom)
        if (const CustomPart* part = CustomPartRegistry::instance().find(c.customPart))
            key += " " + part->spec.name + " " + part->spec.package.type + " " + part->spec.description;
    if (const StandardPart* sp = findStandardPart(c.def().name)) key += " " + sp->footprint;
    for (auto& ch : key) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return key;
}

Look lookOf(const Component& c, const FootprintDef& fp) {
    const std::string key = lookKey(c, fp);
    auto has = [&](const char* w) { return key.find(w) != std::string::npos; };
    const bool smd = !fp.pads.empty() && !fp.pads[0].throughHole;
    if (c.kind == ComponentKind::LED) return smd && !fp.body.cylinder ? Look::Led : Look::Generic;
    if (has("USB")) {
        if (has("MICRO")) return Look::UsbMicro;
        if (has("TYPE-C") || has("USB-C") || has("USB_C") || has("TYPE C")) return Look::UsbC;
        if (has("USB-A") || has("USB_A") || has("TYPE-A") || has("TYPE A")) return Look::UsbA;
    }
    if (has("TACTILE") || has("SW_PUSH") || has("PUSH-BUTTON") || has("PUSH BUTTON") || has("PUSHBUTTON")) return Look::Tactile;
    if ((has("MODULE") || has("WROOM") || has("WROVER")) &&
        (has("WI-FI") || has("WIFI") || has("BLUETOOTH") || has("BLE") || has("LORA") || has("ZIGBEE") || has("GNSS") ||
         has("GPS") || has("LTE") || has("RF_MODULE") || has("WROOM") || has("WROVER")))
        return Look::Module;
    if (has("HC49") || has("HC-49")) return Look::CrystalCan;
    if ((c.ref.size() > 1 && c.ref[0] == 'Y' && std::isdigit(static_cast<unsigned char>(c.ref[1]))) || has("CRYSTAL"))
        return smd ? Look::CrystalSmd : Look::CrystalCan;
    if (has("JST") || has("_XH") || has(" XH") || has("_PH_")) return Look::Jst;
    if (!smd && !fp.pads.empty()) {
        if (has("TRANSFORMER") || has("ETD") || has("EFD") || has("PQ2") || has("PQ3") || has("BOBBIN")) return Look::Transformer;
        if ((has("TERMINAL") || has("TERMINALBLOCK")) && !has("PINHEADER")) return Look::ScrewTerminal;
        if (has("FUSE") && (has("HOLDER") || has("5X20"))) return Look::FuseHolder;
        if (has("CHOKE") || has("TOROID") || has("COMMON-MODE") || has("COMMON MODE") || has("INDUCTOR")) return Look::Toroid;
        if (has("ELECTROLYTIC") || has("SNAP-IN") || has("SNAPIN") || has("CP_RADIAL") || has("CAP_RADIAL") || has("CAP-RADIAL"))
            return Look::Electrolytic;
        if (has("TO-247") || has("TO247") || has("TO-3P")) return Look::To247;
        if (has("TO-220") || has("TO220")) return Look::To220;
        if (has("GBU") || has("KBU") || has("BRIDGE")) return Look::SipBridge;
        if (has("X2") || has("FILM") || has("MKP") || has("MKT")) return Look::Film;
        if (has("VARISTOR") || has("S10K") || has("S14K") || has("S20K") || has("NTC") || has("DISC") || has(" Y1") ||
            has(" Y2") || has("-Y1") || has("-Y2"))
            return Look::Disc;
        if (has("AXIAL") || has("DO41") || has("DO-41") || has("DO201") || has("DO-201")) return Look::Axial;
    }
    if (has("HEADER") || has("PINHEADER")) return Look::Header;
    const bool diodeRef = c.ref.size() > 1 && c.ref[0] == 'D' && std::isdigit(static_cast<unsigned char>(c.ref[1]));
    if (smd && fp.pads.size() == 2 && (c.kind == ComponentKind::Diode || diodeRef)) return Look::Diode;
    return Look::Generic;
}

/// A part's own frame: footprint coordinates (mm, y down, as its pads) and height above the board face it sits on
/// (the far side for a bottom part), mapped onto the board.
struct PartFrame {
    Mesh& m;
    const PcbPlacement& pl;
    double base, out;  // board face (Y) the part sits on, and +1 / −1: away from the board
    Vec2 at(double x, double y) const { return pl.position + rotate90({pl.bottom ? -x : x, y}, pl.rotation); }
    double h(double above) const { return base + out * above; }
    void box(double x0, double y0, double x1, double y1, double h0, double h1, Rgba c) const {
        const Vec2 a = at(x0, y0), b = at(x1, y1);
        m.addBox({std::min(a.x, b.x), std::min(h(h0), h(h1)), std::min(a.y, b.y)},
                 {std::max(a.x, b.x), std::max(h(h0), h(h1)), std::max(a.y, b.y)}, c);
    }
    void post(double x, double y, double r, double h0, double h1, Rgba c, int segments = 20) const {
        const Vec2 p = at(x, y);
        m.addCylinder({p.x, std::min(h(h0), h(h1)), p.y}, r, std::fabs(h(h1) - h(h0)), c, segments);
    }
    /// A cross-section (across, height) of a part whose axis runs along local y (alongY) or x, from `from` to `to`.
    void extrude(const std::vector<Vec2>& profile, bool alongY, double from, double to, Rgba c) const {
        const Vec2 o = alongY ? at(0, from) : at(from, 0), across = alongY ? at(1, from) - o : at(from, 1) - o;
        const Vec2 end = alongY ? at(0, to) : at(to, 0);
        m.addExtrusion(profile, {o.x, base, o.y}, {across.x, 0, across.y}, {0, out, 0}, {end.x - o.x, 0, end.y - o.y}, c);
    }
    /// Text lying on a face `above` the board.
    void text(const std::string& s, double x, double y, double height, double above, Rgba c) const {
        m.addText(s, at(x, y), height, std::min(h(above), h(above + 0.006)), std::max(h(above), h(above + 0.006)), c, pl.bottom);
    }
};

/// A rounded slot (stadium) `w` wide and `h` tall standing on the board, for connector shells, as a convex profile.
std::vector<Vec2> stadium(double w, double h, double inset = 0) {
    std::vector<Vec2> pts;
    const double r = std::max(0.05, h / 2 - inset), hw = std::max(r, w / 2 - inset), cy = h / 2;
    for (int i = 0; i <= 8; ++i) {  // right end, bottom to top
        const double a = -kPi / 2 + kPi * i / 8;
        pts.push_back({hw - r + r * std::cos(a), cy + r * std::sin(a)});
    }
    for (int i = 0; i <= 8; ++i) {  // left end, top to bottom
        const double a = kPi / 2 + kPi * i / 8;
        pts.push_back({-hw + r + r * std::cos(a), cy + r * std::sin(a)});
    }
    return pts;
}
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

void Mesh::addExtrusion(const std::vector<Vec2>& profile, Vec3 origin, Vec3 u, Vec3 v, Vec3 along, Rgba c) {
    const size_t n = profile.size();
    if (n < 3) return;
    auto at = [&](size_t i, bool far) { return origin + u * profile[i].x + v * profile[i].y + (far ? along : Vec3{0, 0, 0}); };
    // Faces wound so their normals point out whichever way u × v faces `along`.
    const Vec3 nu = u.cross(v);
    const bool flip = nu.x * along.x + nu.y * along.y + nu.z * along.z < 0;
    for (size_t i = 1; i + 1 < n; ++i) {
        if (flip) {
            addQuad(at(0, false), at(i, false), at(i + 1, false), at(0, false), c);
            addQuad(at(0, true), at(i + 1, true), at(i, true), at(0, true), c);
        } else {
            addQuad(at(0, false), at(i + 1, false), at(i, false), at(0, false), c);
            addQuad(at(0, true), at(i, true), at(i + 1, true), at(0, true), c);
        }
    }
    for (size_t i = 0; i < n; ++i) {
        const size_t j = (i + 1) % n;
        if (flip) addQuad(at(i, false), at(i, true), at(j, true), at(j, false), c);
        else addQuad(at(i, false), at(j, false), at(j, true), at(i, true), c);
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
    for (const auto& f : copperFills(sch, pcb))
        if (f.layer == layer)
            for (const auto& r : f.rects) m.addBox({r.x0, 0.0, r.y0}, {r.x1, cu * 0.9, r.y1}, pour);
    for (const auto& tr : pcb.tracks)
        if (tr.layer == layer) {
            const auto pts = trackPolyline(tr, 0.005);  // an arc as short chords
            for (size_t k = 0; k + 1 < pts.size(); ++k) m.addSegmentBox(pts[k], pts[k + 1], tr.width, 0.0, cu, trace);
        }
    for (const auto& p : pcb.pads(sch)) {
        if (!p.onLayer(layer) || p.npth) continue;  // an NPTH hole has no copper
        if (p.round && p.size.x != p.size.y) {  // oval: a stadium round its centre segment
            const auto [a, b] = padSpine(p);
            m.addSegmentBox(a, b, 2 * padRadius(p), 0.0, cu, pad);
            for (Vec2 e : {a, b}) m.addCylinder({e.x, 0.0, e.y}, padRadius(p), cu, pad, 16);
        } else if (p.round) m.addCylinder({p.position.x, 0.0, p.position.y}, std::min(p.size.x, p.size.y) / 2, cu, pad, 16);
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

namespace {
/// A circle of radius `r` round (`cx`, `cy`) as a convex polygon (counter-clockwise), for extruded round bodies.
std::vector<Vec2> circlePoints(double r, double cx, double cy, int n = 24) {
    std::vector<Vec2> pts;
    for (int i = 0; i < n; ++i) pts.push_back({cx + r * std::cos(2 * kPi * i / n), cy + r * std::sin(2 * kPi * i / n)});
    return pts;
}

/// Through-hole power hardware drawn as it looks on a real board: a bobbin transformer with its ferrite core and
/// winding tape, an aluminium electrolytic can with its sleeve stripe and vent, an upright copper-wound toroid on its
/// base, TO-220 / TO-247 packages standing on their leads with the metal tab and its hole, a single-in-line bridge, a
/// screw terminal block with its screws and wire entries, a 5 × 20 mm fuse in its clips, a disc (ceramic / safety
/// capacitor, varistor, NTC) on its leads, a boxed film capacitor and an axial resistor / diode with its colour bands. Sizes come from the
/// footprint's body, else the pads; heights the package data lacks are the parts' usual ones.
bool drawPowerLook(Look look, const PartFrame& f, const Component& c, const FootprintDef& fp, double t, double cu) {
    Mesh& m = f.m;
    const Rgba tin{0.78f, 0.78f, 0.80f, 1}, steel{0.82f, 0.83f, 0.85f, 1}, black{0.07f, 0.07f, 0.08f, 1};
    const Rgba dark{0.02f, 0.02f, 0.03f, 1}, ferrite{0.20f, 0.20f, 0.22f, 1}, copper{0.80f, 0.45f, 0.20f, 1};
    double px0 = 1e9, py0 = 1e9, px1 = -1e9, py1 = -1e9;
    for (const auto& pd : fp.pads)
        px0 = std::min(px0, pd.offset.x), px1 = std::max(px1, pd.offset.x), py0 = std::min(py0, pd.offset.y), py1 = std::max(py1, pd.offset.y);
    // The pads' row: along local x or y; `rc` its middle, `tc` the line the pads stand on across it.
    const bool rowX = px1 - px0 >= py1 - py0;
    const double r0 = rowX ? px0 : py0, r1 = rowX ? px1 : py1, rc = (r0 + r1) / 2, tc = rowX ? (py0 + py1) / 2 : (px0 + px1) / 2;
    const double rowLen = r1 - r0;
    const double w = fp.body.width > 0 ? fp.body.width : px1 - px0 + 2, d = fp.body.depth > 0 ? fp.body.depth : py1 - py0 + 2;
    const double along = rowX ? w : d, across = rowX ? d : w;
    // A box given along the row (a0..a1) and across it (x0..x1).
    auto rbox = [&](double a0, double a1, double x0, double x1, double h0, double h1, Rgba col) {
        if (rowX) f.box(a0, x0, a1, x1, h0, h1, col);
        else f.box(x0, a0, x1, a1, h0, h1, col);
    };
    // A round profile (in the row / height plane, x = along the row) pushed across the row from x0 to x1 — a disc
    // or a plate's hole; or (in the across / height plane) pushed along the row — a tube, an axial body.
    auto acrossRow = [&](const std::vector<Vec2>& profile, double x0, double x1, Rgba col) { f.extrude(profile, rowX, x0, x1, col); };
    auto alongRow = [&](const std::vector<Vec2>& profile, double a0, double a1, Rgba col) { f.extrude(profile, !rowX, a0, a1, col); };
    auto tails = [&](double len) {
        m.surface = Surface::Tin;
        for (const auto& pd : fp.pads)
            if (pd.throughHole) {
                const double r = std::clamp(pd.drill * 0.35, 0.25, 0.5);
                f.box(pd.offset.x - r, pd.offset.y - r, pd.offset.x + r, pd.offset.y + r, -(t + 2 * cu + len), 0.0, tin);
            }
    };
    // Leads from the board up to height h at each pad.
    auto legs = [&](double h, double width) {
        m.surface = Surface::Tin;
        for (const auto& pd : fp.pads) f.box(pd.offset.x - width / 2, pd.offset.y - width / 2, pd.offset.x + width / 2, pd.offset.y + width / 2, 0, h, tin);
    };
    const std::string key = lookKey(c, fp);
    auto has = [&](const char* s) { return key.find(s) != std::string::npos; };
    switch (look) {
        case Look::Transformer: {
            // Bobbin base, winding tape round the centre leg, the ferrite core's outer legs and yokes.
            const double H = fp.body.height > 3 ? fp.body.height : std::clamp(std::max(w, d) * 0.9, 12.0, 45.0);
            const double L = std::max(w, d), D = std::min(w, d), leg = L * 0.13, coreD = D * 0.55, base = 2.5;
            const bool coreX = w >= d;
            auto cbox = [&](double a0, double a1, double x0, double x1, double h0, double h1, Rgba col) {
                if (coreX) f.box(a0, x0, a1, x1, h0, h1, col);
                else f.box(x0, a0, x1, a1, h0, h1, col);
            };
            m.surface = Surface::Plastic;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0, base, black);
            m.surface = Surface::Plastic;  // winding tape (polyester, yellow)
            cbox(-L / 2 + leg + 0.4, L / 2 - leg - 0.4, -D / 2 + 0.4, D / 2 - 0.4, base + leg + 0.2, H - leg - 0.2, Rgba{0.93f, 0.78f, 0.16f, 1});
            m.surface = Surface::Ceramic;  // ferrite
            cbox(-L / 2, -L / 2 + leg, -coreD / 2, coreD / 2, base, H, ferrite);
            cbox(L / 2 - leg, L / 2, -coreD / 2, coreD / 2, base, H, ferrite);
            cbox(-L / 2, L / 2, -coreD / 2, coreD / 2, H - leg, H, ferrite);
            cbox(-L / 2, L / 2, -coreD / 2, coreD / 2, base, base + leg, ferrite);
            tails(2.0);
            return true;
        }
        case Look::Electrolytic: {
            const double r = std::min(w, d) / 2, D = 2 * r;
            const double H = fp.body.height >= D * 0.8 ? fp.body.height : std::clamp(D * 1.6, 7.0, 45.0);
            const bool snapIn = has("SNAP");
            m.surface = Surface::Plastic;  // the printed sleeve
            f.post(0, 0, r, 0.4, H - 0.3, snapIn ? black : Rgba{0.08f, 0.10f, 0.24f, 1}, 36);
            // The minus stripe, on the side of the second pad.
            const Vec2 minus = fp.pads.size() > 1 ? fp.pads[1].offset : Vec2{-1, 0};
            const double ml = std::hypot(minus.x, minus.y) > 1e-6 ? std::hypot(minus.x, minus.y) : 1.0;
            const double sx = minus.x / ml * (r - 0.25), sy = minus.y / ml * (r - 0.25);
            m.surface = Surface::Marking;
            f.box(sx - 0.45 - std::fabs(minus.y / ml) * r * 0.25, sy - 0.45 - std::fabs(minus.x / ml) * r * 0.25,
                  sx + 0.45 + std::fabs(minus.y / ml) * r * 0.25, sy + 0.45 + std::fabs(minus.x / ml) * r * 0.25, 0.6, H - 0.5,
                  Rgba{0.70f, 0.72f, 0.76f, 1});
            m.surface = Surface::Tin;  // aluminium top with the pressure vent
            f.post(0, 0, r * 0.93, H - 0.3, H, steel, 36);
            m.surface = Surface::Hole;
            f.box(-r * 0.6, -0.06, r * 0.6, 0.06, H, H + 0.01, dark);
            f.box(-0.06, -r * 0.6, 0.06, r * 0.6, H, H + 0.01, dark);
            m.surface = Surface::Plastic;  // rubber bung
            f.post(0, 0, r * 0.9, 0, 0.4, black, 36);
            tails(2.0);
            return true;
        }
        case Look::Toroid: {
            // An upright ring in the pads' row plane, wound with copper, on a plastic base.
            const double Ro = std::min(along * 0.48, 16.0), Ri = Ro * 0.55, T = std::clamp(across * 0.55, 4.0, Ro * 0.75);
            const double base = 1.5, cz = base + Ro;
            const bool split = has("CMC") || has("COMMON");  // a common-mode choke: two windings apart
            m.surface = Surface::Plastic;
            rbox(rc - Ro, rc + Ro, tc - T / 2 - 0.6, tc + T / 2 + 0.6, 0, base, black);
            const int n = 32;
            for (int i = 0; i < n; ++i) {
                const double a0 = 2 * kPi * i / n, a1 = 2 * kPi * (i + 1) / n, mid = (a0 + a1) / 2;
                const bool core = split && std::fabs(std::cos(mid)) < 0.2;  // the bare core between the windings
                const std::vector<Vec2> q = {{rc + Ri * std::cos(a0), cz + Ri * std::sin(a0)}, {rc + Ro * std::cos(a0), cz + Ro * std::sin(a0)},
                                             {rc + Ro * std::cos(a1), cz + Ro * std::sin(a1)}, {rc + Ri * std::cos(a1), cz + Ri * std::sin(a1)}};
                m.surface = core ? Surface::Ceramic : Surface::Gold;
                acrossRow(q, tc - T / 2, tc + T / 2, core ? ferrite : (i % 2 ? copper : Rgba{0.72f, 0.40f, 0.17f, 1}));
            }
            tails(2.0);
            return true;
        }
        case Look::To220: case Look::To247: case Look::SipBridge: {
            // Standing on its leads: moulded body, the metal tab behind it (with its mounting hole), and the leads.
            const bool big = look == Look::To247, sip = look == Look::SipBridge;
            const double bw = sip ? std::max(22.0, rowLen + 4) : big ? 15.9 : 10.2, bt = sip ? 3.6 : big ? 5.0 : 4.5;
            const double bh = sip ? 18.0 : big ? 20.5 : 9.2, lift = sip ? 1.5 : 3.0, tabH = sip ? 0 : big ? 4.5 : 6.3;
            legs(lift + 0.5, big ? 1.2 : 0.8);
            m.surface = Surface::Plastic;
            rbox(rc - bw / 2, rc + bw / 2, tc - bt / 2 + (sip ? 0 : 1.3), tc + bt / 2, lift, lift + bh, black);
            const double holeZ = sip ? lift + bh / 2 : lift + bh + tabH / 2 - (big ? 0 : 0.6);
            if (!sip) {
                m.surface = Surface::Tin;
                rbox(rc - bw / 2, rc + bw / 2, tc - bt / 2, tc - bt / 2 + 1.3, lift, lift + bh + tabH, steel);
            }
            m.surface = Surface::Hole;  // the mounting hole, through the tab (or the bridge's body)
            acrossRow(circlePoints(big ? 1.8 : 1.6, rc, holeZ, 20), tc - bt / 2 - 0.01, tc + bt / 2 + 0.01, dark);
            tails(1.5);
            return true;
        }
        case Look::ScrewTerminal: {
            const double pitch = fp.pads.size() > 1 ? rowLen / static_cast<double>(fp.pads.size() - 1) : 5.08;
            const double H = 10.0, X = std::max(7.6, across * 0.9);
            m.surface = Surface::Plastic;
            rbox(r0 - pitch / 2, r1 + pitch / 2, tc - X / 2, tc + X / 2, 0, H, Rgba{0.10f, 0.45f, 0.24f, 1});
            for (const auto& pd : fp.pads) {
                const double a = rowX ? pd.offset.x : pd.offset.y;
                m.surface = Surface::Tin;  // the clamp screw, slotted
                const double sx = rowX ? a : tc - X * 0.15, sy = rowX ? tc - X * 0.15 : a;
                f.post(sx, sy, pitch * 0.3, H - 0.6, H + 0.05, steel, 20);
                m.surface = Surface::Hole;
                rbox(a - pitch * 0.25, a + pitch * 0.25, tc - X * 0.15 - 0.15, tc - X * 0.15 + 0.15, H + 0.05, H + 0.07, dark);
                rbox(a - pitch * 0.3, a + pitch * 0.3, tc + X / 2, tc + X / 2 + 0.02, 2.5, 6.0, dark);  // wire entry
            }
            tails(1.5);
            return true;
        }
        case Look::FuseHolder: {
            // Black base, two spring clips, a 5 × 20 mm glass fuse with its metal caps.
            const double H = 9.0, R = 2.6, cz = 6.6, len = std::min(20.0, rowLen + 6);
            m.surface = Surface::Plastic;
            rbox(rc - along / 2, rc + along / 2, tc - across / 2, tc + across / 2, 0, 1.5, black);
            m.surface = Surface::Tin;
            for (double e : {-1.0, 1.0}) {
                const double a = rc + e * (len / 2 - 2.5);
                rbox(a - 3.0, a + 3.0, tc - 3.2, tc - 2.8, 1.5, H, steel);
                rbox(a - 3.0, a + 3.0, tc + 2.8, tc + 3.2, 1.5, H, steel);
                alongRow(circlePoints(R + 0.15, tc, cz, 20), a - 2.5 + (e > 0 ? 0 : -0.01), a + 2.5, steel);
            }
            m.surface = Surface::Glass;
            alongRow(circlePoints(R, tc, cz, 20), rc - len / 2 + 5, rc + len / 2 - 5, Rgba{0.80f, 0.88f, 0.92f, 0.45f});
            legs(1.5, 0.8);
            tails(1.5);
            return true;
        }
        case Look::Film: {
            // A boxed film capacitor (X2 / MKP) standing on its leads, its rating printed on the side.
            const double L = rowLen + 3.0, X = std::clamp(across * 0.5, 5.0, 9.0), H = std::clamp(X * 1.7, 9.0, 16.0);
            m.surface = Surface::Plastic;
            rbox(rc - L / 2, rc + L / 2, tc - X / 2, tc + X / 2, 0.5, H, Rgba{0.95f, 0.80f, 0.22f, 1});
            legs(0.5, 0.7);
            tails(1.5);
            return true;
        }
        case Look::Disc: {
            // A disc standing on its two leads in the pads' plane.
            const bool varistor = has("VARISTOR") || has("S10K") || has("S14K") || has("S20K");
            const bool ntc = has("NTC"), safety = has("Y1") || has("Y2");
            const double D = std::clamp(std::max({rowLen + 2.5, std::min(std::max(w, d), rowLen * 1.6 + 4)}), 4.0, 22.0);
            const double T = std::clamp(std::min(w, d) * 0.6, 2.5, 6.0), lead = 3.0, cz = lead + D / 2;
            const Rgba col = varistor ? Rgba{0.12f, 0.22f, 0.55f, 1} : ntc ? Rgba{0.10f, 0.14f, 0.10f, 1}
                           : safety ? Rgba{0.18f, 0.40f, 0.78f, 1} : Rgba{0.86f, 0.52f, 0.14f, 1};
            m.surface = Surface::Ceramic;
            acrossRow(circlePoints(D / 2, rc, cz, 28), tc - T / 2, tc + T / 2, col);
            legs(cz - D / 2 * 0.5, 0.6);
            tails(1.5);
            return true;
        }
        case Look::Axial: {
            // Lying on the board between its bent leads; colour bands (resistor) or the cathode band (diode).
            const bool diode = c.kind == ComponentKind::Diode || has("DO41") || has("DO-41") || has("DO201") || has("DO-201");
            const bool power = has("2W") || has("3W") || has("5W");
            const double L = std::clamp(std::min(along, rowLen - 2.0), 3.0, 30.0) * (power ? 1.0 : 0.9);
            const double r = std::clamp(across / 2, 0.9, 2.5) * (power ? 1.4 : 1.0), cz = r + 0.3;
            m.surface = Surface::Tin;  // leads: along the row to above each pad, then down
            rbox(r0, r1, tc - 0.3, tc + 0.3, cz - 0.3, cz + 0.3, tin);
            legs(cz, 0.6);
            m.surface = diode ? Surface::Plastic : Surface::Ceramic;
            const Rgba body = diode ? black : power ? Rgba{0.30f, 0.52f, 0.36f, 1} : Rgba{0.82f, 0.71f, 0.52f, 1};
            alongRow(circlePoints(r, tc, cz, 20), rc - L / 2, rc + L / 2, body);
            m.surface = Surface::Marking;
            if (diode) {
                // The band at the cathode (pin K) end.
                double ks = -1;
                for (const auto& pd : fp.pads) {
                    const int pin = pd.pinIndex;
                    if (pin < 0 || pin >= static_cast<int>(c.def().pins.size())) continue;
                    std::string n = c.def().pins[static_cast<size_t>(pin)].name;
                    for (auto& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                    if (n == "K" || n.rfind("CATH", 0) == 0) ks = ((rowX ? pd.offset.x : pd.offset.y) < rc) ? -1 : 1;
                }
                const double b0 = rc + ks * (L / 2 - L * 0.22), b1 = rc + ks * (L / 2 - L * 0.08);
                alongRow(circlePoints(r + 0.02, tc, cz, 20), std::min(b0, b1), std::max(b0, b1), Rgba{0.80f, 0.80f, 0.82f, 1});
            } else {
                const Rgba bands[4] = {{0.55f, 0.30f, 0.10f, 1}, {0.05f, 0.05f, 0.05f, 1}, {0.90f, 0.75f, 0.10f, 1}, {0.85f, 0.70f, 0.25f, 1}};
                for (int i = 0; i < 4; ++i) {
                    const double b = rc - L / 2 + L * (0.18 + 0.17 * i + (i == 3 ? 0.08 : 0));
                    alongRow(circlePoints(r + 0.02, tc, cz, 20), b, b + L * 0.07, bands[i]);
                }
            }
            tails(1.5);
            return true;
        }
        default: return false;
    }
}

/// Draws a part with a recognisable shape (connectors, switches, crystals, modules, diodes, LEDs) in its own frame;
/// false for a generic part (box and leads below). Through-hole leads poke `t` + copper through the board.
bool drawLook(Look look, const PartFrame& f, const Component& c, const FootprintDef& fp, double t, double cu, Rgba lens) {
    if (look == Look::Generic) return false;
    Mesh& m = f.m;
    const Rgba tin{0.78f, 0.78f, 0.80f, 1}, steel{0.82f, 0.83f, 0.85f, 1}, gold{0.86f, 0.70f, 0.30f, 1};
    const Rgba black{0.07f, 0.07f, 0.08f, 1}, dark{0.02f, 0.02f, 0.03f, 1}, cream{0.93f, 0.91f, 0.84f, 1};
    const Rgba etch{0.62f, 0.63f, 0.65f, 1}, white{0.94f, 0.94f, 0.91f, 1};
    // Body size (the footprint's, else the pads' extent) and the pads' centroid (connectors open away from it).
    double px0 = 1e9, py0 = 1e9, px1 = -1e9, py1 = -1e9, cx = 0, cy = 0;
    for (const auto& pd : fp.pads) {
        px0 = std::min(px0, pd.offset.x - pd.size.x / 2), px1 = std::max(px1, pd.offset.x + pd.size.x / 2);
        py0 = std::min(py0, pd.offset.y - pd.size.y / 2), py1 = std::max(py1, pd.offset.y + pd.size.y / 2);
        cx += pd.offset.x / static_cast<double>(fp.pads.size()), cy += pd.offset.y / static_cast<double>(fp.pads.size());
    }
    if (fp.pads.empty()) return false;
    const double w = fp.body.width > 0 ? fp.body.width : px1 - px0 + 1, d = fp.body.depth > 0 ? fp.body.depth : py1 - py0 + 1;
    // Heights the generic package data lacks: a module's shield can (3.2 mm), a 6 mm tact switch with its plunger
    // (4.3 mm), an HC-49/S can (3.5 mm).
    const double floorHeight = look == Look::Module ? 3.2 : look == Look::Tactile ? 4.3 : look == Look::CrystalCan ? 3.5 : 0.0;
    const double H = std::max(fp.body.height > 0 ? fp.body.height : 1.5, floorHeight);
    auto tails = [&](double len, bool goldPins) {
        m.surface = goldPins ? Surface::Gold : Surface::Tin;
        for (const auto& pd : fp.pads)
            if (pd.throughHole) {
                const double r = goldPins ? 0.32 : std::clamp(pd.drill * 0.35, 0.2, 0.3);
                f.box(pd.offset.x - r, pd.offset.y - r, pd.offset.x + r, pd.offset.y + r, -(t + 2 * cu + len), 0.0, goldPins ? gold : tin);
            }
    };
    // Axis helpers: a part whose axis runs along local y (alongY) or x; `a` along it, `x` across it.
    auto abox = [&](bool alongY, double a0, double a1, double x0, double x1, double h0, double h1, Rgba col) {
        if (alongY) f.box(x0, a0, x1, a1, h0, h1, col);
        else f.box(a0, x0, a1, x1, h0, h1, col);
    };
    const bool padsAlongY = std::fabs(cy) >= std::fabs(cx);
    switch (look) {
        case Look::Header: {
            double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
            for (const auto& pd : fp.pads)
                if (pd.throughHole)
                    x0 = std::min(x0, pd.offset.x), x1 = std::max(x1, pd.offset.x), y0 = std::min(y0, pd.offset.y), y1 = std::max(y1, pd.offset.y);
            if (x0 > x1) return false;
            m.surface = Surface::Plastic;  // the moulded strip, one 2.54 mm cell per pin
            f.box(x0 - 1.27, y0 - 1.27, x1 + 1.27, y1 + 1.27, 0, 2.5, black);
            m.surface = Surface::Gold;
            for (const auto& pd : fp.pads)
                if (pd.throughHole) f.box(pd.offset.x - 0.32, pd.offset.y - 0.32, pd.offset.x + 0.32, pd.offset.y + 0.32, 2.5, 8.5, gold);
            tails(3.0, true);
            return true;
        }
        case Look::Tactile: {
            const double hb = std::min(3.1, H * 0.62);
            m.surface = Surface::Plastic;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0, hb, black);
            m.surface = Surface::Tin;  // the steel cover with its folded legs
            f.box(-w / 2 - 0.05, -d / 2 - 0.05, w / 2 + 0.05, d / 2 + 0.05, hb, hb + 0.3, steel);
            for (const auto& pd : fp.pads) {
                const bool alongX = std::fabs(pd.offset.x) - w / 2 > std::fabs(pd.offset.y) - d / 2;
                const double edge = alongX ? w / 2 : d / 2, reach = alongX ? std::fabs(pd.offset.x) : std::fabs(pd.offset.y);
                const double sgn = (alongX ? pd.offset.x : pd.offset.y) < 0 ? -1 : 1, across = alongX ? pd.offset.y : pd.offset.x;
                const double tip = reach > edge ? reach : edge + 0.3;
                abox(!alongX, sgn * edge, sgn * tip, across - 0.35, across + 0.35, 0, 0.25, steel);
                abox(!alongX, sgn * edge, sgn * (edge + 0.15), across - 0.35, across + 0.35, 0, hb, steel);
            }
            m.surface = Surface::Plastic;  // the round plunger
            f.post(0, 0, std::min(w, d) * 0.29, hb + 0.3, std::max(H, hb + 0.8), black, 28);
            tails(1.0, false);
            return true;
        }
        case Look::UsbC: case Look::UsbMicro: case Look::UsbA: {
            // The shell opens on the side away from its pads (the signal pins sit at the back).
            const bool alongY = std::hypot(cx, cy) < 0.05 ? true : padsAlongY;
            const double sgn = std::hypot(cx, cy) < 0.05 ? 1.0 : ((alongY ? cy : cx) > 0 ? -1.0 : 1.0);
            const double across = alongY ? w : d, length = alongY ? d : w;
            // Shell heights of the standard receptacles (USB-C 3.26 mm, micro-B 2.6 mm, USB-A up to 7 mm).
            const double sh = look == Look::UsbC ? 3.26 : look == Look::UsbMicro ? 2.6 : std::clamp(H, 5.0, 7.0);
            std::vector<Vec2> shell, hole;
            if (look == Look::UsbC) shell = stadium(across, sh), hole = stadium(across, sh, 0.35);
            else if (look == Look::UsbMicro)
                shell = {{-across * 0.41, 0}, {across * 0.41, 0}, {across / 2, sh}, {-across / 2, sh}},
                hole = {{-across * 0.37, 0.35}, {across * 0.37, 0.35}, {across / 2 - 0.35, sh - 0.35}, {-across / 2 + 0.35, sh - 0.35}};
            else shell = {{-across / 2, 0}, {across / 2, 0}, {across / 2, sh}, {-across / 2, sh}},
                 hole = {{-across / 2 + 0.4, 0.4}, {across / 2 - 0.4, 0.4}, {across / 2 - 0.4, sh - 0.4}, {-across / 2 + 0.4, sh - 0.4}};
            m.surface = Surface::Tin;
            f.extrude(shell, alongY, -length / 2, length / 2, steel);
            const double mouth = sgn * length / 2;
            m.surface = Surface::Hole;  // the dark opening on the mouth face
            f.extrude(hole, alongY, mouth, mouth + sgn * 0.015, dark);
            m.surface = Surface::Plastic;  // the contact tongue in the middle of the opening
            const double tw = look == Look::UsbA ? across * 0.75 : across * 0.6, tc = look == Look::UsbA ? sh * 0.62 : sh / 2;
            abox(alongY, std::min(mouth, mouth + sgn * 0.03), std::max(mouth, mouth + sgn * 0.03), -tw / 2, tw / 2, tc - 0.3, tc + 0.3,
                 look == Look::UsbA ? white : black);
            tails(0.8, false);
            return true;
        }
        case Look::Module: {
            // Module PCB with a shield can; the antenna end (no pads) shows its printed antenna.
            const bool alongY = padsAlongY;
            const double sgn = (alongY ? cy : cx) > 0 ? -1.0 : 1.0;  // towards the antenna
            const double across = alongY ? w : d, length = alongY ? d : w, ant = std::clamp(length * 0.28, 3.0, 7.0);
            const double pcbTop = 0.8;
            m.surface = Surface::Mask;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0, pcbTop, Rgba{0.05f, 0.09f, 0.16f, 1});
            const double c0 = sgn > 0 ? -length / 2 + 0.5 : -length / 2 + ant, c1 = sgn > 0 ? length / 2 - ant : length / 2 - 0.5;
            m.surface = Surface::Tin;
            abox(alongY, c0, c1, -across / 2 + 0.5, across / 2 - 0.5, pcbTop, H, steel);
            m.surface = Surface::Finish;  // meander antenna on the module's own board
            const double a0 = sgn > 0 ? length / 2 - ant + 0.6 : -length / 2 + 0.6, a1 = a0 + ant - 1.2;
            const int runs = 7;
            std::vector<Vec2> path;
            for (int i = 0; i <= runs; ++i) {
                const double a = a0 + (a1 - a0) * i / runs, x = (i % 2 ? 1 : -1) * (across / 2 - 1.2);
                path.push_back(alongY ? f.at(-x, a) : f.at(a, -x));
                path.push_back(alongY ? f.at(x, a) : f.at(a, x));
            }
            for (size_t i = 0; i + 1 < path.size(); ++i)
                m.addSegmentBox(path[i], path[i + 1], 0.35, std::min(f.h(pcbTop), f.h(pcbTop + 0.02)), std::max(f.h(pcbTop), f.h(pcbTop + 0.02)), gold);
            m.surface = Surface::Marking;
            std::string mark = c.value.empty() ? c.ref : c.value;
            if (mark.size() > 18) mark.resize(18);
            const double mx = (c0 + c1) / 2, mh = std::min(1.2, (across - 2) / std::max(1.0, Mesh::textWidth(mark, 1.0)));
            if (mh >= 0.4) f.text(mark, alongY ? 0 : mx, alongY ? mx : 0, mh, H, etch);
            return true;
        }
        case Look::CrystalCan: {
            const bool alongX = w >= d;
            const double L = std::max(w, d), W = std::min(w, d);
            m.surface = Surface::Tin;  // the flange and the rounded can
            abox(!alongX, -L / 2 - 0.2 + W / 2, L / 2 + 0.2 - W / 2, -W / 2 - 0.2, W / 2 + 0.2, 0, 0.3, steel);
            for (double e : {-1.0, 1.0}) f.post(alongX ? e * (L / 2 - W / 2) : 0, alongX ? 0 : e * (L / 2 - W / 2), W / 2 + 0.2, 0, 0.3, steel, 24);
            abox(!alongX, -L / 2 + W / 2, L / 2 - W / 2, -W / 2, W / 2, 0.3, H, steel);
            for (double e : {-1.0, 1.0}) f.post(alongX ? e * (L / 2 - W / 2) : 0, alongX ? 0 : e * (L / 2 - W / 2), W / 2, 0.3, H, steel, 24);
            m.surface = Surface::Marking;
            std::string mark;  // the frequency, as cans are marked ("16.000")
            for (char ch : c.value)
                if (std::isdigit(static_cast<unsigned char>(ch)) || ch == '.') mark += ch;
            const double mh = std::min(W * 0.32, 1.0);
            if (!mark.empty() && Mesh::textWidth(mark, mh) < L - W / 2) f.text(mark, 0, 0, mh, H, etch);
            tails(1.0, false);
            return true;
        }
        case Look::CrystalSmd: {
            m.surface = Surface::Ceramic;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0, std::min(0.3, H / 2), Rgba{0.86f, 0.80f, 0.66f, 1});
            m.surface = Surface::Tin;  // the seam-welded lid
            f.box(-w / 2 + 0.2, -d / 2 + 0.2, w / 2 - 0.2, d / 2 - 0.2, std::min(0.3, H / 2), H, steel);
            return true;
        }
        case Look::Diode: case Look::Led: {
            const Vec2 a = fp.pads[0].offset, b = fp.pads[1].offset;
            const bool alongX = std::fabs(b.x - a.x) >= std::fabs(b.y - a.y);
            const double L = alongX ? w : d, X = alongX ? d : w;
            if (look == Look::Led) {
                m.surface = Surface::Ceramic;  // white reflector body, coloured lens on top, tinned ends
                f.box(-w / 2, -d / 2, w / 2, d / 2, 0, H * 0.55, white);
                m.surface = Surface::Glass;
                f.box(-w / 2 + 0.06, -d / 2 + 0.06, w / 2 - 0.06, d / 2 - 0.06, H * 0.55, H, lens);
                m.surface = Surface::Tin;
                const double cap = std::min(0.3, L * 0.2);
                for (double e : {-1.0, 1.0})
                    abox(!alongX, std::min(e * (L / 2 - cap), e * (L / 2 + 0.01)), std::max(e * (L / 2 - cap), e * (L / 2 + 0.01)),
                         -X / 2 - 0.01, X / 2 + 0.01, -0.005, H * 0.55 + 0.01, tin);
                return true;
            }
            // Moulded body, J-bend leads folded under each end, a band on the cathode end.
            m.surface = Surface::Plastic;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0.05, H, black);
            int cathode = 0;
            for (int i = 0; i < 2; ++i) {
                const int pin = fp.pads[static_cast<size_t>(i)].pinIndex;
                if (pin < 0 || pin >= static_cast<int>(c.def().pins.size())) continue;
                std::string n = c.def().pins[static_cast<size_t>(pin)].name;
                for (auto& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                if (n == "K" || n.rfind("CATH", 0) == 0) cathode = i;
            }
            const Vec2 kp = fp.pads[static_cast<size_t>(cathode)].offset;
            const double ks = (alongX ? kp.x : kp.y) < 0 ? -1 : 1;
            m.surface = Surface::Marking;
            abox(!alongX, ks > 0 ? L / 2 - L * 0.26 : -L / 2 + L * 0.1, ks > 0 ? L / 2 - L * 0.1 : -L / 2 + L * 0.26, -X / 2, X / 2, H,
                 H + 0.008, Rgba{0.78f, 0.78f, 0.80f, 1});
            m.surface = Surface::Tin;
            for (const auto& pd : fp.pads) {
                const double e = (alongX ? pd.offset.x : pd.offset.y) < 0 ? -1 : 1, at = alongX ? pd.offset.x : pd.offset.y;
                const double lw = std::min(X * 0.32, (alongX ? pd.size.y : pd.size.x) / 2);
                abox(!alongX, std::min(e * (L / 2 - 0.4), at), std::max(e * (L / 2 - 0.4), at), -lw, lw, 0, 0.15, tin);
                abox(!alongX, std::min(e * L / 2, e * (L / 2 + 0.12)), std::max(e * L / 2, e * (L / 2 + 0.12)), -lw, lw, 0, H * 0.45, tin);
            }
            return true;
        }
        case Look::Jst: {
            m.surface = Surface::Plastic;
            f.box(-w / 2, -d / 2, w / 2, d / 2, 0, H, cream);
            f.box(-w / 2 + 0.6, -d / 2 + 0.6, w / 2 - 0.6, d / 2 - 0.9, H, H + 0.005, Rgba{0.55f, 0.53f, 0.48f, 1});  // the opening
            m.surface = Surface::Gold;
            for (const auto& pd : fp.pads)
                if (pd.throughHole) f.box(pd.offset.x - 0.32, pd.offset.y - 0.32, pd.offset.x + 0.32, pd.offset.y + 0.32, H, H + 0.01, gold);
            tails(1.2, false);
            return true;
        }
        case Look::Transformer: case Look::Electrolytic: case Look::Toroid: case Look::To220: case Look::To247:
        case Look::SipBridge: case Look::ScrewTerminal: case Look::FuseHolder: case Look::Film: case Look::Disc: case Look::Axial:
            return drawPowerLook(look, f, c, fp, t, cu);
        case Look::Generic: break;
    }
    return false;
}
}  // namespace

void foldFlexRegions(Mesh& m, const BoardSettings& s) {
    std::vector<const FlexRegion*> folds;
    for (const FlexRegion& f : s.flexRegions)
        if (f.bendAngle != 0 && f.area.width() > 0 && f.area.height() > 0) folds.push_back(&f);
    auto alongX = [](const FlexRegion& f) { return f.area.width() <= f.area.height(); };  // bends across its short side
    auto start = [&](const FlexRegion& f) { return alongX(f) ? f.area.x0 : f.area.y0; };
    std::sort(folds.begin(), folds.end(), [&](auto* a, auto* b) { return start(*a) > start(*b); });  // far folds first
    const double mid = -s.thickness / 2;
    for (const FlexRegion* f : folds) {
        const int axis = alongX(*f) ? 0 : 2;  // mesh x = board x, mesh z = board y
        const double u0 = start(*f), w = alongX(*f) ? f->area.width() : f->area.height();
        const double theta = std::fabs(f->bendAngle) * M_PI / 180, sign = f->bendAngle > 0 ? 1 : -1, R = w / theta;
        // Split every triangle crossing a cut so the area curves smoothly (one cut per 5°, at least 8).
        const int n = std::max(8, static_cast<int>(std::ceil(std::fabs(f->bendAngle) / 5)));
        for (int k = 0; k <= n; ++k) {
            const double c = u0 + w * k / n;
            std::vector<uint32_t> out;
            out.reserve(m.indices.size());
            auto lerp = [&](uint32_t a, uint32_t b, double t) {
                const uint32_t v = static_cast<uint32_t>(m.vertexCount());
                for (int q = 0; q < 3; ++q) m.positions.push_back(static_cast<float>(m.positions[3 * a + q] + t * (m.positions[3 * b + q] - m.positions[3 * a + q])));
                for (int q = 0; q < 3; ++q) m.normals.push_back(m.normals[3 * a + q]);
                for (int q = 0; q < 4; ++q) m.colors.push_back(m.colors[4 * a + q]);
                m.surfaces.push_back(m.surfaces[a]);
                return v;
            };
            for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
                uint32_t v[3] = {m.indices[i], m.indices[i + 1], m.indices[i + 2]};
                double d[3];
                for (int q = 0; q < 3; ++q) d[q] = m.positions[3 * v[q] + axis] - c;
                const bool lo = d[0] < -1e-7 || d[1] < -1e-7 || d[2] < -1e-7, hi = d[0] > 1e-7 || d[1] > 1e-7 || d[2] > 1e-7;
                if (!lo || !hi) {
                    out.insert(out.end(), v, v + 3);
                    continue;
                }
                // Rotate so v[0] is alone on its side, then cut its two edges.
                int lone = 0;
                for (int q = 0; q < 3; ++q)
                    if ((d[q] > 0) != (d[(q + 1) % 3] > 0) && (d[q] > 0) != (d[(q + 2) % 3] > 0)) lone = q;
                std::rotate(v, v + lone, v + 3);
                std::rotate(d, d + lone, d + 3);
                const uint32_t p = lerp(v[0], v[1], d[0] / (d[0] - d[1])), q = lerp(v[0], v[2], d[0] / (d[0] - d[2]));
                out.insert(out.end(), {v[0], p, q, p, v[1], v[2], p, v[2], q});
            }
            m.indices = std::move(out);
        }
        for (size_t i = 0; i < m.vertexCount(); ++i) {
            float* p = &m.positions[3 * i];
            float* nn = &m.normals[3 * i];
            const double u = p[axis];
            if (u <= u0) continue;
            const double hh = sign * (p[1] - mid), r = R - hh, phi = std::min(u - u0, w) / R;
            double nu = u0 + r * std::sin(phi), nh = R - r * std::cos(phi);
            if (u > u0 + w) nu += (u - u0 - w) * std::cos(phi), nh += (u - u0 - w) * std::sin(phi);
            p[axis] = static_cast<float>(nu), p[1] = static_cast<float>(mid + sign * nh);
            const double a = nn[axis], b = sign * nn[1];
            nn[axis] = static_cast<float>(a * std::cos(phi) - b * std::sin(phi));
            nn[1] = static_cast<float>(sign * (a * std::sin(phi) + b * std::cos(phi)));
        }
    }
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
    // Mounting holes: dark bore through the board with a plated ring (twice the drill) for the screw head, both sides.
    const Rgba bore{0.02f, 0.03f, 0.05f, 1.0f};
    for (const auto& c : s.cutouts) {  // cutouts: a dark opening through the board, like the holes
        m.surface = Surface::Hole;
        m.addPrism(c, -t - 0.02, 0.02, bore);
    }
    for (const auto& h : s.holes) {
        m.surface = Surface::Hole;
        m.addCylinder({h.position.x, -t - 0.02, h.position.y}, h.drill / 2, t + 0.04, bore, 24);
        m.surface = Surface::Finish;
        const double ring = std::max(h.drill / 2 + 0.3, std::min(h.keepout / 2, h.drill));
        m.addCylinder({h.position.x, 0.0, h.position.y}, ring, cu, gold, 32);
        if (layers > 1) m.addCylinder({h.position.x, -t - cu, h.position.y}, ring, cu, gold, 32);
        if (opt.standoffs > 0) {  // brass hex standoff (six-sided cylinder) below, steel pan-head screw above
            m.surface = Surface::Gold;
            m.addCylinder({h.position.x, -t - cu - opt.standoffs, h.position.y}, h.drill, opt.standoffs, {0.78f, 0.62f, 0.32f, 1}, 6);
            m.surface = Surface::Tin;
            m.addCylinder({h.position.x, cu, h.position.y}, h.drill * 0.9, h.drill * 0.6, {0.55f, 0.56f, 0.58f, 1}, 24);
        }
    }
    if (opt.enclosure && !s.enclosure.empty()) {  // MCAD frame (underside at Z = 0, Y = -board y), see-through
        const size_t first = m.vertexCount();
        Model3DRef ref;
        ref.id = s.enclosure;
        if (appendModel3D(m, ref, PcbPlacement{}, -t, -t))
            for (size_t v = first; v < m.vertexCount(); ++v) {
                m.surfaces[v] = static_cast<uint8_t>(Surface::Glass);
                const float grey[4] = {0.80f, 0.82f, 0.86f, 0.25f};
                std::copy(grey, grey + 4, m.colors.begin() + static_cast<std::ptrdiff_t>(v * 4));
            }
    }

    auto ps = pcb.pads(sch);
    // Which components are drawn (their pads carry solder joints; bare pads show the copper finish).
    auto populated = [&](int componentId) {
        if (!opt.components || opt.hidden.count(componentId)) return false;
        const Component* c = sch.find(componentId);
        return c && c->hasFootprint() && c->pcb.placed && !embeddedElement(*c, s);
    };
    const Rgba solder{0.80f, 0.81f, 0.83f, 1.0f};
    const Rgba tin{0.78f, 0.78f, 0.80f, 1.0f};
    if (opt.copper) {
        const Rgba pourColor{(mask.r + trackColor.r) / 2, (mask.g + trackColor.g) / 2, (mask.b + trackColor.b) / 2, 1.0f};
        // Tracks and pours lie under the mask: they read as raised, slightly lighter mask.
        m.surface = Surface::Mask;
        for (const auto& f : copperFills(sch, pcb)) {
            double y0 = copperLayerBase(f.layer, s.layerCount, t, cu);
            for (const auto& r : f.rects) m.addBox({r.x0, y0, r.y0}, {r.x1, y0 + cu * 0.9, r.y1}, pourColor);
        }
        for (const auto& tr : pcb.tracks) {
            double y0 = copperLayerBase(tr.layer, s.layerCount, t, cu);
            const auto pts = trackPolyline(tr, 0.005);  // an arc as short chords
            for (size_t k = 0; k + 1 < pts.size(); ++k) m.addSegmentBox(pts[k], pts[k + 1], tr.width, y0, y0 + cu, trackColor);
        }
        // Polygon and rounded-rectangle pads as prisms of their outline (board XY); empty = a plain box.
        auto padShape = [](const Pad& p) {
            std::vector<Vec2> out;
            for (const Vec2& v : p.outline) out.push_back(p.position + v);
            if (out.empty() && p.round && p.size.x != p.size.y) {  // oval: two half circles round its centre segment
                const auto [a, b] = padSpine(p);
                const double r = padRadius(p), a0 = std::atan2(b.y - a.y, b.x - a.x);
                for (int i = 0; i <= 16; ++i) {
                    const double t = a0 + M_PI / 2 + i * M_PI / 16;
                    out.push_back(a + Vec2{r * std::cos(t), r * std::sin(t)});
                }
                for (int i = 0; i <= 16; ++i) {
                    const double t = a0 - M_PI / 2 + i * M_PI / 16;
                    out.push_back(b + Vec2{r * std::cos(t), r * std::sin(t)});
                }
            }
            if (out.empty() && p.corner > 0 && !p.round) {
                const double r = p.corner * std::min(p.size.x, p.size.y), hx = p.size.x / 2 - r, hy = p.size.y / 2 - r;
                const Vec2 centres[4] = {{hx, hy}, {-hx, hy}, {-hx, -hy}, {hx, -hy}};
                for (int k = 0; k < 4; ++k)
                    for (int i = 0; i <= 4; ++i) {
                        const double a = (k + i / 4.0) * M_PI / 2;
                        out.push_back(p.position + centres[k] + Vec2{r * std::cos(a), r * std::sin(a)});
                    }
            }
            double area = 0;  // counter-clockwise for addPrism (bottom-side parts arrive mirrored)
            for (size_t i = 0; i < out.size(); ++i) area += out[i].x * out[(i + 1) % out.size()].y - out[(i + 1) % out.size()].x * out[i].y;
            if (area < 0) std::reverse(out.begin(), out.end());
            return out;
        };
        for (const auto& p : ps) {
            const bool joint = populated(p.componentId);
            const std::vector<Vec2> shape = padShape(p);
            if (p.throughHole) {
                for (bool top : {true, false}) {
                    if (!top && layers == 1) continue;  // single-sided: copper on the top layer only
                    double y = top ? 0.0 : -t - cu;
                    m.surface = Surface::Finish;
                    if (p.round && shape.empty()) m.addCylinder({p.position.x, y, p.position.y}, p.size.x / 2, cu, gold);
                    else if (!shape.empty()) m.addPrism(shape, y, y + cu, gold);
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
                if (!shape.empty()) m.addPrism(shape, y0, y0 + cu, gold);
                else m.addBox({p.position.x - p.size.x / 2, y0, p.position.y - p.size.y / 2},
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
            for (const FootprintLine& l : pcb.footprintLines(c))
                if (!l.fab) m.addSegmentBox(l.a, l.b, l.width, y0, y1, silk);
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
            if (!c.hasFootprint() || !c.pcb.placed || embeddedElement(c, s) || opt.hidden.count(c.id)) continue;  // embedded: inside the board
            const FootprintDef* fp = Library::instance().footprint(c.footprintName());
            if (!fp) continue;
            // An imported 3D model (VRML / STL / OBJ / STEP) replaces the generated body, leads and pins: the part's
            // own (PcbLayout::partModels) first, else its custom part's.
            if (auto own = pcb.partModels.find(c.id); own != pcb.partModels.end() && appendModel3D(m, own->second, c.pcb, cu, -t - cu))
                continue;
            if (c.kind == ComponentKind::Custom)
                if (const CustomPart* part = CustomPartRegistry::instance().find(c.customPart))
                    if (appendModel3D(m, part->spec.model3d, c.pcb, cu, -t - cu)) continue;
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
            if (drawLook(lookOf(c, *fp), PartFrame{m, c.pcb, base, out}, c, *fp, t, cu, col)) continue;
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
                    if (reach <= edge) {
                        // A no-lead package (QFN / DFN): its terminal shows as a tinned flank on the body's side.
                        const double across = std::min(p.size.x, p.size.y) * 0.8;
                        if (reach + std::max(p.size.x, p.size.y) / 2 >= edge - 0.05 && edge - reach < 1.0) {
                            const double sgn = (alongX ? rel.x : rel.y) < 0 ? -1.0 : 1.0, e0 = sgn * edge - sgn * 0.02, e1 = sgn * edge + sgn * 0.02;
                            const Vec2 a = alongX ? Vec2{pc.x + std::min(e0, e1), p.position.y - across / 2} : Vec2{p.position.x - across / 2, pc.y + std::min(e0, e1)};
                            const Vec2 b = alongX ? Vec2{pc.x + std::max(e0, e1), p.position.y + across / 2} : Vec2{p.position.x + across / 2, pc.y + std::max(e0, e1)};
                            m.addBox({a.x, std::min(base, base + out * 0.2), a.y}, {b.x, std::max(base, base + out * 0.2), b.y}, tin);
                        }
                        continue;  // under the body
                    }
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
    if (opt.fold) foldFlexRegions(m, s);
    return m;
}

}  // namespace sieda
