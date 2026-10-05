import CoreGraphics
import SwiftUI

/// Geometry of a track from the core's snapshot: straight a → b, or a true arc (`arc`, centre `cx`/`cy`, `radius`,
/// `startAngle`, `sweep` in radians; the sweep turns from +x towards +y, the core's convention). Arcs are drawn as
/// short straight pieces (at most 5° each, ends exact), so the drawing does not depend on the platform's arc
/// direction convention in a flipped coordinate system.
extension SnapTrack {
    /// A drawable arc (the core leaves the centre out for a degenerate arc, which is drawn straight).
    var isArc: Bool {
        arc == true && cx != nil && cy != nil && (radius ?? 0) > 0 && startAngle != nil && sweep != nil
    }

    var start: CGPoint { CGPoint(x: ax, y: ay) }
    var end: CGPoint { CGPoint(x: bx, y: by) }

    /// Centre-line points from a to b (two for a straight track).
    var centreLine: [CGPoint] {
        guard isArc, let cx, let cy, let r = radius, let s = startAngle, let sw = sweep else { return [start, end] }
        let steps = max(2, Int((abs(sw) / (Double.pi / 36)).rounded(.up)))
        var points = [start]
        for i in 1..<steps {
            let a = s + sw * Double(i) / Double(steps)
            points.append(CGPoint(x: cx + r * cos(a), y: cy + r * sin(a)))
        }
        points.append(end)
        return points
    }

    /// Adds the centre line to `path` as one sub-path.
    func addCentreLine(to path: inout Path) {
        let points = centreLine
        guard let first = points.first else { return }
        path.move(to: first)
        for p in points.dropFirst() { path.addLine(to: p) }
    }

    /// Bounding box of the centre line.
    var extent: CGRect {
        guard isArc, let cx, let cy, let r = radius else {
            return CGRect(x: min(ax, bx), y: min(ay, by), width: abs(bx - ax), height: abs(by - ay))
        }
        var box = CGRect(x: min(ax, bx), y: min(ay, by), width: abs(bx - ax), height: abs(by - ay))
        for (dx, dy) in [(1.0, 0.0), (0.0, 1.0), (-1.0, 0.0), (0.0, -1.0)] where containsDirection(dx, dy) {
            box = box.union(CGRect(x: cx + dx * r, y: cy + dy * r, width: 0, height: 0))
        }
        return box
    }

    /// Length of the centre line (the arc length for an arc).
    var length: Double {
        if isArc, let r = radius, let sw = sweep { return r * abs(sw) }
        return hypot(bx - ax, by - ay)
    }

    /// Distance from `p` to the centre line.
    func distance(to p: CGPoint) -> Double {
        let px = Double(p.x), py = Double(p.y)
        guard isArc, let cx, let cy, let r = radius else {
            let dx = bx - ax, dy = by - ay
            let len2 = dx * dx + dy * dy
            let t = len2 > 0 ? min(1, max(0, ((px - ax) * dx + (py - ay) * dy) / len2)) : 0
            return hypot(px - (ax + t * dx), py - (ay + t * dy))
        }
        var d = min(hypot(px - ax, py - ay), hypot(px - bx, py - by))
        if containsDirection(px - cx, py - cy) { d = min(d, abs(hypot(px - cx, py - cy) - r)) }
        return d
    }

    /// The direction (dx, dy) from the centre lies within the arc's sweep.
    private func containsDirection(_ dx: Double, _ dy: Double) -> Bool {
        guard let s = startAngle, let sw = sweep else { return false }
        if dx == 0 && dy == 0 { return true }
        let twoPi = 2 * Double.pi
        var rel = sw >= 0 ? atan2(dy, dx) - s : s - atan2(dy, dx)
        rel = rel.truncatingRemainder(dividingBy: twoPi)
        if rel < 0 { rel += twoPi }
        return rel <= abs(sw) + 1e-9 || rel >= twoPi - 1e-9
    }
}
