// SiEDA Core — track geometry: straight and arc track segments as one primitive.
//
// A Track is straight from `a` to `b`, or (Track::arc) a circular arc from `a` through `mid` to `b` (3-point form).
// Every consumer that measures copper (DRC, connectivity, the routers, pours, exports, lengths) goes through these
// functions. For a straight track each function is exactly the straight-segment formula the code used before arcs
// existed (same operations in the same order, inline), so boards without arcs give bit-identical results.
//
// Arc distances are exact (closed form: end points, the radial foot point and circle crossings) and continuous, so
// rounding (FMA contraction on one compiler, not on the other) only moves results by a few ulps; callers compare
// with tolerances. Included at the end of Pcb.hpp.
#pragma once

#include <algorithm>
#include <cmath>
#include <utility>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

/// A circular arc: centre, radius, start angle and signed sweep (radians, > 0 counter-clockwise in board axes).
/// `p0` / `p1` are the exact end points (the track's a / b), not recomputed from the angles.
struct ArcGeom {
    bool valid = false;
    Vec2 c, p0, p1;
    double r = 0, start = 0, sweep = 0;
    Vec2 at(double angle) const;
    /// Point at fraction t ∈ [0, 1] of the sweep (exact end points at 0 and 1).
    Vec2 point(double t) const;
    double length() const;
    /// The direction from the centre towards `p` lies within the sweep (with a tiny angular tolerance).
    bool containsDirection(Vec2 p) const;
};

/// The arc from a through m to b; invalid (a straight line) when the three points are (nearly) collinear, coincide
/// or the radius would exceed 10⁵ × the chord.
ArcGeom arcThrough(Vec2 a, Vec2 m, Vec2 b);
/// The arc of an arc track (invalid for a straight track or a degenerate arc, which is treated as straight).
inline ArcGeom trackArc(const Track& t) { return t.arc ? arcThrough(t.a, t.mid, t.b) : ArcGeom{}; }
inline bool isArcTrack(const Track& t) { return t.arc && arcThrough(t.a, t.mid, t.b).valid; }

/// The arc track from `a` to `b` about centre `c` (counter-clockwise when `ccw`), with `mid` on the arc halfway.
/// `b` should lie at the same distance from `c` as `a`.
Track makeArcTrack(Vec2 a, Vec2 b, Vec2 c, bool ccw, int net, int layer, double width);

double pointArcDistance(Vec2 p, const ArcGeom& g);
double segmentArcDistance(Vec2 p, Vec2 q, const ArcGeom& g);
double arcArcDistance(const ArcGeom& g, const ArcGeom& h);
double arcRectDistance(const ArcGeom& g, const Rect& r);
Rect arcBounds(const ArcGeom& g);
/// Points along the arc, ends included, with chord sagitta at most `maxError` (and at most 15° per chord).
std::vector<Vec2> arcPolyline(const ArcGeom& g, double maxError);

namespace detail {
// Arc cases of the track functions below (a degenerate arc falls back to the straight formula).
double arcTrackLength(const Track& t);
Rect arcTrackBox(const Track& t, double grow);
double arcTrackPointDistance(const Track& t, Vec2 p);
double arcTrackSegmentDistance(const Track& t, Vec2 p, Vec2 q);
double arcTrackTrackDistance(const Track& t, const Track& u);
double arcTrackRectDistance(const Track& t, const Rect& r);
Vec2 arcTrackClosestPoint(const Track& t, Vec2 p);
}  // namespace detail

/// Centre-line length (the arc length for an arc).
inline double trackLength(const Track& t) { return t.arc ? detail::arcTrackLength(t) : (t.b - t.a).length(); }
/// Bounding box of the centre line grown by `grow` (straight: Rect(a, b).inflated(grow)).
inline Rect trackBox(const Track& t, double grow) {
    return t.arc ? detail::arcTrackBox(t, grow) : Rect(t.a.x, t.a.y, t.b.x, t.b.y).inflated(grow);
}
/// Centre-line distances.
inline double trackPointDistance(const Track& t, Vec2 p) {
    return t.arc ? detail::arcTrackPointDistance(t, p) : pointSegmentDistance(p, t.a, t.b);
}
inline double trackSegmentDistance(const Track& t, Vec2 p, Vec2 q) {
    return t.arc ? detail::arcTrackSegmentDistance(t, p, q) : segmentSegmentDistance(t.a, t.b, p, q);
}
inline double trackTrackDistance(const Track& t, const Track& u) {
    return t.arc || u.arc ? detail::arcTrackTrackDistance(t, u) : segmentSegmentDistance(t.a, t.b, u.a, u.b);
}
inline double trackRectDistance(const Track& t, const Rect& r) {
    return t.arc ? detail::arcTrackRectDistance(t, r) : segmentRectDistance(t.a, t.b, r);
}
/// Nearest point of the centre line.
Vec2 trackClosestPoint(const Track& t, Vec2 p);
/// Centre-line points from a to b (two points for a straight track).
std::vector<Vec2> trackPolyline(const Track& t, double maxError = 0.001);
/// Unit direction in which the track leaves its end (`atB` false: a, true: b) towards its interior.
Vec2 trackEndDirection(const Track& t, bool atB);
/// Point at fraction `f` ∈ [0, 1] of the length from a (straight: a + (b − a)·f).
Vec2 trackPointAt(const Track& t, double f);
/// Distance from end a, along the centre line, of the point nearest to p.
double trackParamAt(const Track& t, Vec2 p);
/// The track reversed (b to a).
Track reversedTrack(const Track& t);
/// Sub-track between fractions f0 < f1 of the length (an arc stays an arc).
Track subTrack(const Track& t, double f0, double f1);

// ---- Pad copper. A round pad (`Pad::round`) is a circle, or an oval (KiCad "oval", size.x != size.y): a stadium,
// every point within `padRadius` of its centre segment (`padSpine`; both ends at the centre for a circle). Every
// consumer that measures pad copper (DRC, connectivity, pours, routers) goes through these; a circle keeps the
// exact arithmetic it had (distance to the centre), so boards without ovals give bit-identical results.
inline double padRadius(const Pad& p) { return std::min(p.size.x, p.size.y) / 2; }
inline std::pair<Vec2, Vec2> padSpine(const Pad& p) {
    const double h = std::fabs(p.size.x - p.size.y) / 2;
    const Vec2 d = p.size.x > p.size.y ? Vec2{h, 0} : Vec2{0, h};
    return {p.position - d, p.position + d};
}
/// Centre-line distances from a point / segment / track to a round pad's centre (point or spine): subtract padRadius
/// for the copper.
inline double roundPadPointDistance(const Pad& p, Vec2 q) {
    if (p.size.x == p.size.y) return (q - p.position).length();
    const auto [a, b] = padSpine(p);
    return pointSegmentDistance(q, a, b);
}
inline double roundPadSegmentDistance(const Pad& p, Vec2 a, Vec2 b) {
    if (p.size.x == p.size.y) return pointSegmentDistance(p.position, a, b);
    const auto [c, d] = padSpine(p);
    return segmentSegmentDistance(a, b, c, d);
}
inline double roundPadTrackDistance(const Pad& p, const Track& t) {
    if (p.size.x == p.size.y) return trackPointDistance(t, p.position);
    const auto [c, d] = padSpine(p);
    return trackSegmentDistance(t, c, d);
}
/// Distance from a point / segment / track centre line to the pad copper (0 inside); a polygon pad by its bounds.
inline double padPointDistance(const Pad& p, Vec2 q) {
    return p.round ? std::max(0.0, roundPadPointDistance(p, q) - padRadius(p)) : pointRectDistance(q, p.bounds());
}
inline double padSegmentDistance(const Pad& p, Vec2 a, Vec2 b) {
    return p.round ? std::max(0.0, roundPadSegmentDistance(p, a, b) - padRadius(p)) : segmentRectDistance(a, b, p.bounds());
}
inline double padTrackDistance(const Pad& p, const Track& t) {
    return p.round ? std::max(0.0, roundPadTrackDistance(p, t) - padRadius(p)) : trackRectDistance(t, p.bounds());
}
/// Copper-to-copper distance of two pads (0 when they touch).
inline double padPadDistance(const Pad& a, const Pad& b) {
    if (!a.round && !b.round) return rectRectDistance(a.bounds(), b.bounds());
    if (a.round && b.round) {
        if (a.size.x == a.size.y && b.size.x == b.size.y)
            return std::max(0.0, (a.position - b.position).length() - padRadius(a) - padRadius(b));
        const auto [p, q] = padSpine(b);
        return std::max(0.0, roundPadSegmentDistance(a, p, q) - padRadius(a) - padRadius(b));
    }
    const Pad &r = a.round ? a : b, &box = a.round ? b : a;
    if (r.size.x == r.size.y) return std::max(0.0, padPointDistance(box, r.position) - padRadius(r));
    const auto [p, q] = padSpine(r);
    return std::max(0.0, segmentRectDistance(p, q, box.bounds()) - padRadius(r));
}
/// Pad stacks: the pad's copper on copper layer `layer` of a board whose bottom layer is `bottom` — a through-hole
/// pad's `inner` (inner layers) or `solder` (the side opposite the part) diameter as a circle, as the Gerbers draw
/// it; any other pad as it is. A changed pad is built in `scratch`.
inline const Pad& padOnLayer(const Pad& p, int layer, int bottom, Pad& scratch) {
    if (!p.throughHole || p.npth) return p;
    const double d = p.inner > 0 && layer != kTopLayer && layer != bottom ? p.inner
                     : p.solder > 0 && layer == (p.bottom ? kTopLayer : bottom) ? p.solder
                                                                                : 0.0;
    if (d <= 0) return p;
    scratch = p;
    scratch.size = {d, d};
    scratch.round = true;
    scratch.outline.clear();
    return scratch;
}
/// A through-hole pad with a pad stack (another size on inner layers or the solder side).
inline bool padStacked(const Pad& p) { return p.throughHole && !p.npth && (p.inner > 0 || p.solder > 0); }
/// Bounds of the pad's copper on every layer (pad stack sizes included).
inline Rect padStackBounds(const Pad& p) {
    const double s = p.throughHole ? std::max(p.inner, p.solder) : 0.0;
    return Rect::centered(p.position, std::max(p.size.x, s), std::max(p.size.y, s));
}

}  // namespace sieda
