#include "sieda/Mesh.hpp"

#include <cctype>
#include <cmath>
#include <string>

namespace sieda {

namespace {
Vec3 board3(Vec2 p, double y) { return {p.x, y, p.y}; }
}  // namespace

void Mesh::addQuad(Vec3 a, Vec3 b, Vec3 c, Vec3 d, Rgba color) {
    Vec3 n = (b - a).cross(c - a).normalized();
    uint32_t base = static_cast<uint32_t>(vertexCount());
    for (const Vec3& v : {a, b, c, d}) {
        positions.insert(positions.end(), {static_cast<float>(v.x), static_cast<float>(v.y), static_cast<float>(v.z)});
        normals.insert(normals.end(), {static_cast<float>(n.x), static_cast<float>(n.y), static_cast<float>(n.z)});
        colors.insert(colors.end(), {color.r, color.g, color.b, color.a});
    }
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

Mesh buildAssemblyMesh(const Schematic& sch, const PcbLayout& pcb, const MeshOptions& opt) {
    Mesh m;
    const BoardSettings& s = pcb.settings;
    const double t = s.thickness;
    const double cu = 0.035;
    const Rgba mask{0.05f, 0.22f, 0.55f, 1.0f};  // blue solder mask
    const Rgba trackColor{0.12f, 0.36f, 0.75f, 1.0f};
    const Rgba gold{0.86f, 0.70f, 0.30f, 1.0f};
    const Rgba silk{0.95f, 0.95f, 0.95f, 1.0f};
    const Rgba fr4Edge{0.75f, 0.68f, 0.45f, 1.0f};

    // Board core with a slightly darker FR-4 edge band.
    m.addBox({0, -t, 0}, {s.width, 0, s.height}, mask);
    m.addBox({-0.01, -t * 0.7, -0.01}, {s.width + 0.01, -t * 0.3, s.height + 0.01}, fr4Edge);

    auto ps = pcb.pads(sch);
    if (opt.copper) {
        for (const auto& tr : pcb.tracks) {
            bool top = tr.layer == CopperLayer::Top;
            m.addSegmentBox(tr.a, tr.b, tr.width, top ? 0.0 : -t - cu, top ? cu : -t, trackColor);
        }
        for (const auto& p : ps) {
            if (p.throughHole) {
                for (bool top : {true, false}) {
                    double y = top ? 0.0 : -t - cu;
                    if (p.round) m.addCylinder({p.position.x, y, p.position.y}, p.size.x / 2, cu, gold);
                    else
                        m.addBox({p.position.x - p.size.x / 2, y, p.position.y - p.size.y / 2},
                                 {p.position.x + p.size.x / 2, y + cu, p.position.y + p.size.y / 2}, gold);
                }
            } else {
                double y0 = p.bottom ? -t - cu : 0.0;
                m.addBox({p.position.x - p.size.x / 2, y0, p.position.y - p.size.y / 2},
                         {p.position.x + p.size.x / 2, y0 + cu, p.position.y + p.size.y / 2}, gold);
            }
        }
        for (const auto& v : pcb.vias) m.addCylinder({v.position.x, -t - cu, v.position.y}, v.diameter / 2, t + 2 * cu, gold, 14);
    }

    if (opt.silkscreen) {
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed) continue;
            Rect r = pcb.courtyard(c).inflated(-0.15);
            double y0 = c.pcb.bottom ? -t - 0.045 : cu, y1 = c.pcb.bottom ? -t - cu : cu + 0.01;
            Vec2 p0{r.x0, r.y0}, p1{r.x1, r.y0}, p2{r.x1, r.y1}, p3{r.x0, r.y1};
            m.addSegmentBox(p0, p1, 0.12, y0, y1, silk);
            m.addSegmentBox(p1, p2, 0.12, y0, y1, silk);
            m.addSegmentBox(p2, p3, 0.12, y0, y1, silk);
            m.addSegmentBox(p3, p0, 0.12, y0, y1, silk);
        }
    }

    if (opt.components) {
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed) continue;
            const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
            if (!fp) continue;
            const BodyDef& b = fp->body;
            double w = b.width, d = b.depth;
            if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) std::swap(w, d);
            Rgba col{b.r, b.g, b.b, 1.0f};
            if (c.kind == ComponentKind::LED) {
                std::string v = c.value;
                for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                if (v.find("green") != std::string::npos) col = {0.15f, 0.85f, 0.25f, 0.9f};
                else if (v.find("blue") != std::string::npos) col = {0.2f, 0.35f, 0.95f, 0.9f};
                else if (v.find("yellow") != std::string::npos) col = {0.95f, 0.85f, 0.15f, 0.9f};
                else if (v.find("white") != std::string::npos) col = {0.95f, 0.95f, 0.92f, 0.9f};
                else col = {0.92f, 0.15f, 0.12f, 0.9f};
            }
            double base = c.pcb.bottom ? -t - cu : cu;
            double y0 = c.pcb.bottom ? base - b.height : base, y1 = c.pcb.bottom ? base : base + b.height;
            Vec2 pc = c.pcb.position;
            if (b.cylinder) {
                m.addCylinder({pc.x, y0, pc.y}, std::min(w, d) / 2, b.height, col);
            } else {
                m.addBox({pc.x - w / 2, y0, pc.y - d / 2}, {pc.x + w / 2, y1, pc.y + d / 2}, col);
            }
            // Metallic terminations on two-terminal SMD chips (resistors, capacitors, inductors, fuses).
            if (fp->pads.size() == 2 && !fp->pads[0].throughHole &&
                (fp->name.rfind("R_", 0) == 0 || fp->name.rfind("C_", 0) == 0 || fp->name.rfind("L_", 0) == 0 ||
                 fp->name.rfind("Fuse_", 0) == 0)) {
                const Rgba tin{0.78f, 0.78f, 0.80f, 1.0f};
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
            // Header pins: post above the plastic body and solder tail through the board.
            for (const auto& p : ps) {
                if (p.componentId != c.id || !p.throughHole) continue;
                double postA = c.pcb.bottom ? y0 - 6.0 : y1, postB = c.pcb.bottom ? y0 : y1 + 6.0;
                m.addBox({p.position.x - 0.32, postA, p.position.y - 0.32}, {p.position.x + 0.32, postB, p.position.y + 0.32},
                         gold);
                double tailA = c.pcb.bottom ? cu : -t - cu - 2.5, tailB = c.pcb.bottom ? cu + 2.5 : -t - cu;
                m.addBox({p.position.x - 0.32, tailA, p.position.y - 0.32}, {p.position.x + 0.32, tailB, p.position.y + 0.32},
                         gold);
            }
        }
    }
    return m;
}

}  // namespace sieda
