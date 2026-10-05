// SiEDA Core — track geometry: straight and arc track segments (see TrackGeometry.hpp).
#include "sieda/TrackGeometry.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace sieda {

namespace {
constexpr double kTwoPi = 2 * kPi;
constexpr double kAngleTol = 1e-9;

double cross2(Vec2 a, Vec2 b) { return a.x * b.y - a.y * b.x; }
/// Angle wrapped into [0, 2π).
double wrap(double a) {
    a = std::fmod(a, kTwoPi);
    if (a < 0) a += kTwoPi;
    return a >= kTwoPi ? 0.0 : a;
}
Vec2 closestOnSeg(Vec2 p, Vec2 a, Vec2 b) {
    const Vec2 ab = b - a;
    const double len2 = ab.dot(ab);
    const double t = len2 > 0 ? std::clamp((p - a).dot(ab) / len2, 0.0, 1.0) : 0.0;
    return a + ab * t;
}
}  // namespace

// ------------------------------------------------------------------------------------------------------ ArcGeom

Vec2 ArcGeom::at(double angle) const { return {c.x + r * std::cos(angle), c.y + r * std::sin(angle)}; }

Vec2 ArcGeom::point(double t) const {
    if (t <= 0) return p0;
    if (t >= 1) return p1;
    return at(start + sweep * t);
}

double ArcGeom::length() const { return r * std::fabs(sweep); }

bool ArcGeom::containsDirection(Vec2 p) const {
    const Vec2 d = p - c;
    if (d.x == 0 && d.y == 0) return true;  // the centre sees every direction
    const double phi = std::atan2(d.y, d.x);
    const double rel = sweep >= 0 ? wrap(phi - start) : wrap(start - phi);
    return rel <= std::fabs(sweep) + kAngleTol || rel >= kTwoPi - kAngleTol;
}

ArcGeom arcThrough(Vec2 a, Vec2 m, Vec2 b) {
    ArcGeom g;
    const Vec2 M = m - a, B = b - a;
    const double lm = M.length(), lb = B.length();
    if (lm <= 1e-9 || lb <= 1e-9 || (m - b).length() <= 1e-9) return g;
    const double d = 2 * cross2(M, B);
    if (std::fabs(d) <= 2e-9 * lm * lb) return g;  // collinear within a nano-radian
    const double mm = M.dot(M), bb = B.dot(B);
    const Vec2 u{(B.y * mm - M.y * bb) / d, (M.x * bb - B.x * mm) / d};
    const double r = u.length();
    if (!(r > 0) || !std::isfinite(r) || r > 1e5 * lb) return g;
    g.c = a + u;
    g.r = r;
    g.p0 = a;
    g.p1 = b;
    g.start = std::atan2(a.y - g.c.y, a.x - g.c.x);
    const double end = std::atan2(b.y - g.c.y, b.x - g.c.x);
    if (d > 0) {  // a → m → b turns left: counter-clockwise
        g.sweep = wrap(end - g.start);
        if (g.sweep <= 0) g.sweep = kTwoPi;
    } else {
        g.sweep = -wrap(g.start - end);
        if (g.sweep >= 0) g.sweep = -kTwoPi;
    }
    g.valid = true;
    return g;
}

Track makeArcTrack(Vec2 a, Vec2 b, Vec2 c, bool ccw, int net, int layer, double width) {
    Track t;
    t.net = net;
    t.layer = layer;
    t.width = width;
    t.a = a;
    t.b = b;
    const double r = (a - c).length();
    const double s = std::atan2(a.y - c.y, a.x - c.x), e = std::atan2(b.y - c.y, b.x - c.x);
    const double sweep = ccw ? wrap(e - s) : -wrap(s - e);
    const double half = s + sweep / 2;
    t.mid = {c.x + r * std::cos(half), c.y + r * std::sin(half)};
    t.arc = true;
    return t;
}

// -------------------------------------------------------------------------------------------------- distances

double pointArcDistance(Vec2 p, const ArcGeom& g) {
    double d = std::min((p - g.p0).length(), (p - g.p1).length());
    if (g.containsDirection(p)) d = std::min(d, std::fabs((p - g.c).length() - g.r));
    return d;
}

double segmentArcDistance(Vec2 p, Vec2 q, const ArcGeom& g) {
    double d = std::min({pointArcDistance(p, g), pointArcDistance(q, g), pointSegmentDistance(g.p0, p, q),
                         pointSegmentDistance(g.p1, p, q)});
    const Vec2 pq = q - p;
    const double len2 = pq.dot(pq);
    if (len2 <= 0) return d;
    // Crossings of the segment's line with the circle.
    const Vec2 f = p - g.c;
    const double bq = f.dot(pq), cq = f.dot(f) - g.r * g.r;
    const double disc = bq * bq - len2 * cq;
    if (disc >= 0) {
        const double sq = std::sqrt(disc);
        for (double s : {(-bq - sq) / len2, (-bq + sq) / len2})
            if (s >= 0 && s <= 1 && g.containsDirection(p + pq * s)) return 0.0;
    }
    // Interior pair: the foot of the centre on the segment and the arc point on the same radius.
    const double t = (g.c - p).dot(pq) / len2;
    if (t > 0 && t < 1) {
        const Vec2 foot = p + pq * t;
        const Vec2 v = foot - g.c;
        const double h = v.length();
        if (h > 0) {
            const Vec2 u = v * (1 / h);
            if (g.containsDirection(g.c + u)) d = std::min(d, std::fabs(g.r - h));
            if (g.containsDirection(g.c - u)) d = std::min(d, g.r + h);
        }
    }
    return d;
}

double arcArcDistance(const ArcGeom& g, const ArcGeom& h) {
    double d = std::min({pointArcDistance(g.p0, h), pointArcDistance(g.p1, h), pointArcDistance(h.p0, g),
                         pointArcDistance(h.p1, g)});
    const Vec2 cc = h.c - g.c;
    const double D = cc.length();
    if (D <= 0) return d;  // concentric: the end points above cover every overlap
    const Vec2 u = cc * (1 / D);
    // Circle crossings.
    if (D <= g.r + h.r && D >= std::fabs(g.r - h.r)) {
        const double x = (D * D + g.r * g.r - h.r * h.r) / (2 * D);
        const double y = std::sqrt(std::max(0.0, g.r * g.r - x * x));
        const Vec2 base = g.c + u * x, n{-u.y, u.x};
        for (Vec2 X : {base + n * y, base - n * y})
            if (g.containsDirection(X) && h.containsDirection(X)) return 0.0;
    }
    // Interior pairs on the line of centres.
    for (double sg : {1.0, -1.0})
        for (double sh : {1.0, -1.0}) {
            const Vec2 P = g.c + u * (sg * g.r), Q = h.c + u * (sh * h.r);
            if (g.containsDirection(g.c + u * sg) && h.containsDirection(h.c + u * sh)) d = std::min(d, (P - Q).length());
        }
    return d;
}

double arcRectDistance(const ArcGeom& g, const Rect& r) {
    if (r.contains(g.p0) || r.contains(g.p1)) return 0.0;
    const Vec2 c0{r.x0, r.y0}, c1{r.x1, r.y0}, c2{r.x1, r.y1}, c3{r.x0, r.y1};
    return std::min({segmentArcDistance(c0, c1, g), segmentArcDistance(c1, c2, g), segmentArcDistance(c2, c3, g),
                     segmentArcDistance(c3, c0, g)});
}

Rect arcBounds(const ArcGeom& g) {
    Rect b(g.p0.x, g.p0.y, g.p1.x, g.p1.y);
    for (Vec2 dir : {Vec2{1, 0}, Vec2{0, 1}, Vec2{-1, 0}, Vec2{0, -1}})
        if (g.containsDirection(g.c + dir)) {
            const Vec2 p = g.c + dir * g.r;
            b = Rect(std::min(b.x0, p.x), std::min(b.y0, p.y), std::max(b.x1, p.x), std::max(b.y1, p.y));
        }
    return b;
}

std::vector<Vec2> arcPolyline(const ArcGeom& g, double maxError) {
    const double err = std::clamp(maxError, 1e-6, g.r);
    const double step = std::min(kPi / 12, 2 * std::acos(1 - err / g.r));
    const int n = std::max(1, static_cast<int>(std::ceil(std::fabs(g.sweep) / std::max(step, 1e-6))));
    std::vector<Vec2> out;
    out.reserve(static_cast<size_t>(n) + 1);
    for (int k = 0; k <= n; ++k) out.push_back(g.point(static_cast<double>(k) / n));
    return out;
}

// ------------------------------------------------------------------------------------------------ tracks

namespace detail {
double arcTrackLength(const Track& t) {
    const ArcGeom g = trackArc(t);
    return g.valid ? g.length() : (t.b - t.a).length();
}

Rect arcTrackBox(const Track& t, double grow) {
    const ArcGeom g = trackArc(t);
    return g.valid ? arcBounds(g).inflated(grow) : Rect(t.a.x, t.a.y, t.b.x, t.b.y).inflated(grow);
}

double arcTrackPointDistance(const Track& t, Vec2 p) {
    const ArcGeom g = trackArc(t);
    return g.valid ? pointArcDistance(p, g) : pointSegmentDistance(p, t.a, t.b);
}

double arcTrackSegmentDistance(const Track& t, Vec2 p, Vec2 q) {
    const ArcGeom g = trackArc(t);
    return g.valid ? segmentArcDistance(p, q, g) : segmentSegmentDistance(t.a, t.b, p, q);
}

double arcTrackTrackDistance(const Track& t, const Track& u) {
    const ArcGeom g = trackArc(t), h = trackArc(u);
    if (g.valid && h.valid) return arcArcDistance(g, h);
    if (g.valid) return segmentArcDistance(u.a, u.b, g);
    if (h.valid) return segmentArcDistance(t.a, t.b, h);
    return segmentSegmentDistance(t.a, t.b, u.a, u.b);
}

double arcTrackRectDistance(const Track& t, const Rect& r) {
    const ArcGeom g = trackArc(t);
    return g.valid ? arcRectDistance(g, r) : segmentRectDistance(t.a, t.b, r);
}

Vec2 arcTrackClosestPoint(const Track& t, Vec2 p) { return trackClosestPoint(t, p); }
}  // namespace detail

Vec2 trackClosestPoint(const Track& t, Vec2 p) {
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) {
            const Vec2 d = p - g.c;
            const double l = d.length();
            Vec2 best = (p - g.p0).length() <= (p - g.p1).length() ? g.p0 : g.p1;
            if (l > 0 && g.containsDirection(p)) {
                const Vec2 on = g.c + d * (g.r / l);
                if ((on - p).length() < (best - p).length()) best = on;
            }
            return best;
        }
    }
    return closestOnSeg(p, t.a, t.b);
}

std::vector<Vec2> trackPolyline(const Track& t, double maxError) {
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) return arcPolyline(g, maxError);
    }
    return {t.a, t.b};
}

Vec2 trackEndDirection(const Track& t, bool atB) {
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) {
            // Travel direction a → b at an end point: the radius turned by 90° towards the sweep.
            const Vec2 rp = (atB ? g.p1 : g.p0) - g.c;
            const double l = rp.length();
            Vec2 tangent = g.sweep > 0 ? Vec2{-rp.y / l, rp.x / l} : Vec2{rp.y / l, -rp.x / l};
            return atB ? tangent * -1 : tangent;
        }
    }
    const Vec2 d = atB ? t.a - t.b : t.b - t.a;
    const double l = d.length();
    return l > 0 ? d * (1 / l) : Vec2{};
}

Vec2 trackPointAt(const Track& t, double f) {
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) return g.point(f);
    }
    return t.a + (t.b - t.a) * f;  // the straight formula as callers wrote it before arcs (bit-identical)
}

double trackParamAt(const Track& t, Vec2 p) {
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) {
            const Vec2 q = trackClosestPoint(t, p);
            const double phi = std::atan2(q.y - g.c.y, q.x - g.c.x);
            double rel = g.sweep >= 0 ? wrap(phi - g.start) : wrap(g.start - phi);
            if (rel > std::fabs(g.sweep)) rel = (q - g.p0).length() <= (q - g.p1).length() ? 0 : std::fabs(g.sweep);
            return rel * g.r;
        }
    }
    const Vec2 c = closestOnSeg(p, t.a, t.b);
    return (c - t.a).length();
}

Track reversedTrack(const Track& t) {
    Track r = t;
    std::swap(r.a, r.b);
    return r;
}

Track subTrack(const Track& t, double f0, double f1) {
    Track s = t;
    s.a = trackPointAt(t, f0);
    s.b = trackPointAt(t, f1);
    if (t.arc) {
        const ArcGeom g = trackArc(t);
        if (g.valid) s.mid = g.point((f0 + f1) / 2);
    }
    return s;
}

}  // namespace sieda
