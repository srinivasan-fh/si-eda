// SiEDA Core — 2D/3D geometry primitives shared by schematic, PCB and 3D engines.
#pragma once

#include <algorithm>
#include <cmath>

namespace sieda {

constexpr double kPi = 3.14159265358979323846;

struct Vec2 {
    double x = 0.0, y = 0.0;
    Vec2() = default;
    Vec2(double x_, double y_) : x(x_), y(y_) {}
    Vec2 operator+(Vec2 o) const { return {x + o.x, y + o.y}; }
    Vec2 operator-(Vec2 o) const { return {x - o.x, y - o.y}; }
    Vec2 operator*(double s) const { return {x * s, y * s}; }
    bool operator==(Vec2 o) const { return x == o.x && y == o.y; }
    double dot(Vec2 o) const { return x * o.x + y * o.y; }
    double length() const { return std::sqrt(x * x + y * y); }
};

struct Vec3 {
    double x = 0.0, y = 0.0, z = 0.0;
    Vec3() = default;
    Vec3(double x_, double y_, double z_) : x(x_), y(y_), z(z_) {}
    Vec3 operator+(Vec3 o) const { return {x + o.x, y + o.y, z + o.z}; }
    Vec3 operator-(Vec3 o) const { return {x - o.x, y - o.y, z - o.z}; }
    Vec3 operator*(double s) const { return {x * s, y * s, z * s}; }
    Vec3 cross(Vec3 o) const { return {y * o.z - z * o.y, z * o.x - x * o.z, x * o.y - y * o.x}; }
    double length() const { return std::sqrt(x * x + y * y + z * z); }
    Vec3 normalized() const {
        double l = length();
        return l > 0 ? Vec3{x / l, y / l, z / l} : Vec3{0, 0, 0};
    }
};

struct Rect {
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    Rect() = default;
    Rect(double ax, double ay, double bx, double by)
        : x0(std::min(ax, bx)), y0(std::min(ay, by)), x1(std::max(ax, bx)), y1(std::max(ay, by)) {}
    static Rect centered(Vec2 c, double w, double h) { return {c.x - w / 2, c.y - h / 2, c.x + w / 2, c.y + h / 2}; }
    double width() const { return x1 - x0; }
    double height() const { return y1 - y0; }
    Vec2 center() const { return {(x0 + x1) / 2, (y0 + y1) / 2}; }
    bool contains(Vec2 p) const { return p.x >= x0 && p.x <= x1 && p.y >= y0 && p.y <= y1; }
    bool intersects(const Rect& o) const { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
    Rect inflated(double d) const { return {x0 - d, y0 - d, x1 + d, y1 + d}; }
};

/// Rotate a point about the origin by a multiple of 90 degrees (counter-clockwise in math axes).
inline Vec2 rotate90(Vec2 p, int rotationDeg) {
    int r = ((rotationDeg % 360) + 360) % 360;
    switch (r) {
        case 90: return {-p.y, p.x};
        case 180: return {-p.x, -p.y};
        case 270: return {p.y, -p.x};
        default: return p;
    }
}

inline double pointSegmentDistance(Vec2 p, Vec2 a, Vec2 b) {
    Vec2 ab = b - a;
    double len2 = ab.dot(ab);
    double t = len2 > 0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return (p - (a + ab * t)).length();
}

/// Proper crossing of two segments. Orientations within a rounding error of zero count as collinear (no crossing;
/// touching and overlapping collinear segments are then measured by `segmentSegmentDistance`): with fused
/// multiply-add (clang on Apple silicon) collinear points give tiny products of either sign.
inline bool segmentsIntersect(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    auto orient = [](Vec2 p, Vec2 q, Vec2 r) {
        const double v = (q.x - p.x) * (r.y - p.y) - (q.y - p.y) * (r.x - p.x);
        return std::fabs(v) < 1e-9 ? 0.0 : v;
    };
    double d1 = orient(c, d, a), d2 = orient(c, d, b), d3 = orient(a, b, c), d4 = orient(a, b, d);
    return ((d1 > 0 && d2 < 0) || (d1 < 0 && d2 > 0)) && ((d3 > 0 && d4 < 0) || (d3 < 0 && d4 > 0));
}

inline double segmentSegmentDistance(Vec2 a, Vec2 b, Vec2 c, Vec2 d) {
    if (segmentsIntersect(a, b, c, d)) return 0.0;
    return std::min({pointSegmentDistance(a, c, d), pointSegmentDistance(b, c, d), pointSegmentDistance(c, a, b),
                     pointSegmentDistance(d, a, b)});
}

inline double pointRectDistance(Vec2 p, const Rect& r) {
    double dx = std::max({r.x0 - p.x, 0.0, p.x - r.x1});
    double dy = std::max({r.y0 - p.y, 0.0, p.y - r.y1});
    return std::sqrt(dx * dx + dy * dy);
}

inline double segmentRectDistance(Vec2 a, Vec2 b, const Rect& r) {
    if (r.contains(a) || r.contains(b)) return 0.0;
    Vec2 c0{r.x0, r.y0}, c1{r.x1, r.y0}, c2{r.x1, r.y1}, c3{r.x0, r.y1};
    return std::min({segmentSegmentDistance(a, b, c0, c1), segmentSegmentDistance(a, b, c1, c2),
                     segmentSegmentDistance(a, b, c2, c3), segmentSegmentDistance(a, b, c3, c0)});
}

inline double rectRectDistance(const Rect& a, const Rect& b) {
    double dx = std::max({a.x0 - b.x1, 0.0, b.x0 - a.x1});
    double dy = std::max({a.y0 - b.y1, 0.0, b.y0 - a.y1});
    return std::sqrt(dx * dx + dy * dy);
}

}  // namespace sieda
