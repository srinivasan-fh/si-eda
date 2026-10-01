import SwiftUI

/// Vector schematic symbols, drawn in local symbol coordinates (grid = 10 units, y down).
/// Pin positions match `Library.cpp` exactly.
enum SchematicSymbols {
    /// Local-space hit box for each symbol body.
    static func bounds(_ kind: ComponentKind) -> CGRect {
        switch kind {
        case .resistor, .capacitor, .inductor, .diode, .led, .switchSPST, .fuse:
            return CGRect(x: -30, y: -14, width: 60, height: 28)
        case .voltageSource, .currentSource: return CGRect(x: -18, y: -30, width: 36, height: 60)
        case .ground: return CGRect(x: -12, y: -2, width: 24, height: 22)
        case .npn, .nmos: return CGRect(x: -30, y: -30, width: 56, height: 60)
        case .opAmp: return CGRect(x: -40, y: -28, width: 80, height: 56)
        case .connector: return CGRect(x: -20, y: -22, width: 28, height: 44)
        case .ic8: return CGRect(x: -40, y: -42, width: 80, height: 84)
        case .netLabel: return CGRect(x: -2, y: -9, width: 60, height: 18)
        }
    }

    struct Shapes {
        var stroke = Path()
        var fill = Path()     // filled with the symbol fill colour
        var solid = Path()    // filled with the stroke colour (arrows, dots)
    }

    static func shapes(for kind: ComponentKind, value: String) -> Shapes {
        var s = Shapes()
        func line(_ a: CGPoint, _ b: CGPoint) {
            s.stroke.move(to: a)
            s.stroke.addLine(to: b)
        }
        func p(_ x: CGFloat, _ y: CGFloat) -> CGPoint { CGPoint(x: x, y: y) }
        func arrow(from a: CGPoint, to b: CGPoint, size: CGFloat = 5) {
            line(a, b)
            let angle = atan2(b.y - a.y, b.x - a.x)
            var head = Path()
            head.move(to: b)
            head.addLine(to: p(b.x - size * cos(angle - 0.45), b.y - size * sin(angle - 0.45)))
            head.addLine(to: p(b.x - size * cos(angle + 0.45), b.y - size * sin(angle + 0.45)))
            head.closeSubpath()
            s.solid.addPath(head)
        }

        switch kind {
        case .resistor:
            line(p(-30, 0), p(-15, 0))
            line(p(15, 0), p(30, 0))
            s.stroke.move(to: p(-15, 0))
            let xs: [CGFloat] = [-12.5, -7.5, -2.5, 2.5, 7.5, 12.5]
            for (i, x) in xs.enumerated() { s.stroke.addLine(to: p(x, i % 2 == 0 ? -6 : 6)) }
            s.stroke.addLine(to: p(15, 0))

        case .capacitor:
            line(p(-30, 0), p(-3, 0))
            line(p(3, 0), p(30, 0))
            line(p(-3, -11), p(-3, 11))
            line(p(3, -11), p(3, 11))

        case .inductor:
            line(p(-30, 0), p(-16, 0))
            line(p(16, 0), p(30, 0))
            for i in 0..<4 {
                let cx = -12 + CGFloat(i) * 8
                s.stroke.move(to: p(cx - 4, 0))
                s.stroke.addArc(center: p(cx, 0), radius: 4, startAngle: .degrees(180), endAngle: .degrees(0), clockwise: false)
            }

        case .diode, .led:
            line(p(-30, 0), p(-8, 0))
            line(p(8, 0), p(30, 0))
            var tri = Path()
            tri.move(to: p(-8, -8))
            tri.addLine(to: p(-8, 8))
            tri.addLine(to: p(8, 0))
            tri.closeSubpath()
            s.fill.addPath(tri)
            s.stroke.addPath(tri)
            line(p(8, -8), p(8, 8))
            if kind == .led {
                arrow(from: p(0, -10), to: p(7, -19), size: 4)
                arrow(from: p(6, -8), to: p(13, -17), size: 4)
            }

        case .voltageSource:
            s.fill.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            s.stroke.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            line(p(0, -30), p(0, -15))
            line(p(0, 15), p(0, 30))
            let upper = value.uppercased()
            if upper.hasPrefix("SIN") {
                s.stroke.move(to: p(-8, 0))
                s.stroke.addCurve(to: p(0, 0), control1: p(-6, -9), control2: p(-2, -9))
                s.stroke.addCurve(to: p(8, 0), control1: p(2, 9), control2: p(6, 9))
            } else if upper.hasPrefix("PULSE") {
                s.stroke.move(to: p(-8, 5))
                s.stroke.addLine(to: p(-4, 5))
                s.stroke.addLine(to: p(-4, -5))
                s.stroke.addLine(to: p(4, -5))
                s.stroke.addLine(to: p(4, 5))
                s.stroke.addLine(to: p(8, 5))
            } else {
                line(p(-4, -8), p(4, -8))
                line(p(0, -12), p(0, -4))
                line(p(-4, 8), p(4, 8))
            }

        case .currentSource:
            s.fill.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            s.stroke.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            line(p(0, -30), p(0, -15))
            line(p(0, 15), p(0, 30))
            arrow(from: p(0, 9), to: p(0, -9), size: 5)

        case .ground:
            line(p(0, 0), p(0, 8))
            line(p(-11, 8), p(11, 8))
            line(p(-7, 12), p(7, 12))
            line(p(-3, 16), p(3, 16))

        case .npn:
            s.fill.addEllipse(in: CGRect(x: -16, y: -21, width: 42, height: 42))
            s.stroke.addEllipse(in: CGRect(x: -16, y: -21, width: 42, height: 42))
            line(p(-30, 0), p(-4, 0))
            line(p(-4, -12), p(-4, 12))
            line(p(-4, -5), p(20, -20))
            line(p(20, -20), p(20, -30))
            arrow(from: p(-4, 5), to: p(17, 18), size: 5)
            line(p(17, 18), p(20, 20))
            line(p(20, 20), p(20, 30))

        case .nmos:
            line(p(-30, 0), p(-10, 0))
            line(p(-10, -12), p(-10, 12))
            line(p(-4, -14), p(-4, -6))
            line(p(-4, -4), p(-4, 4))
            line(p(-4, 6), p(-4, 14))
            line(p(-4, -10), p(20, -10))
            line(p(20, -10), p(20, -30))
            line(p(-4, 10), p(20, 10))
            line(p(20, 10), p(20, 30))
            line(p(20, 0), p(20, 10))
            arrow(from: p(20, 0), to: p(-2, 0), size: 5)

        case .opAmp:
            var tri = Path()
            tri.move(to: p(-25, -26))
            tri.addLine(to: p(-25, 26))
            tri.addLine(to: p(30, 0))
            tri.closeSubpath()
            s.fill.addPath(tri)
            s.stroke.addPath(tri)
            line(p(-40, -10), p(-25, -10))
            line(p(-40, 10), p(-25, 10))
            line(p(30, 0), p(40, 0))
            line(p(-21, -10), p(-15, -10))          // minus
            line(p(-21, 10), p(-15, 10))            // plus
            line(p(-18, 7), p(-18, 13))

        case .switchSPST:
            line(p(-30, 0), p(-12, 0))
            line(p(12, 0), p(30, 0))
            s.solid.addEllipse(in: CGRect(x: -14, y: -2, width: 4, height: 4))
            s.solid.addEllipse(in: CGRect(x: 10, y: -2, width: 4, height: 4))
            let closed = ["on", "closed", "1", "true"].contains(value.lowercased())
            line(p(-12, 0), closed ? p(12, -2) : p(10, -11))

        case .connector:
            let body = CGRect(x: -10, y: -20, width: 16, height: 40)
            s.fill.addRect(body)
            s.stroke.addRect(body)
            line(p(-20, -10), p(-10, -10))
            line(p(-20, 10), p(-10, 10))
            s.solid.addRect(CGRect(x: -4, y: -12, width: 4, height: 4))
            s.solid.addRect(CGRect(x: -4, y: 8, width: 4, height: 4))

        case .ic8:
            let body = CGRect(x: -30, y: -40, width: 60, height: 80)
            s.fill.addRect(body)
            s.stroke.addRect(body)
            s.stroke.move(to: p(-6, -40))
            s.stroke.addArc(center: p(0, -40), radius: 6, startAngle: .degrees(180), endAngle: .degrees(0), clockwise: true)
            for y in [-30, -10, 10, 30] as [CGFloat] {
                line(p(-40, y), p(-30, y))
                line(p(30, y), p(40, y))
            }

        case .fuse:
            line(p(-30, 0), p(-15, 0))
            line(p(15, 0), p(30, 0))
            let body = CGRect(x: -15, y: -5, width: 30, height: 10)
            s.fill.addRect(body)
            s.stroke.addRect(body)
            line(p(-15, 0), p(15, 0))

        case .netLabel:
            let width = max(36, CGFloat(value.count) * 7 + 16)
            var tag = Path()
            tag.move(to: p(0, 0))
            tag.addLine(to: p(7, -8))
            tag.addLine(to: p(width, -8))
            tag.addLine(to: p(width, 8))
            tag.addLine(to: p(7, 8))
            tag.closeSubpath()
            s.fill.addPath(tag)
            s.stroke.addPath(tag)
        }
        return s
    }

    /// Text annotations inside symbols (pin numbers, op-amp signs) that should stay upright are drawn by the editor;
    /// this returns the local anchor for the reference/value labels.
    static func labelAnchor(_ kind: ComponentKind) -> (ref: CGPoint, value: CGPoint) {
        switch kind {
        case .voltageSource, .currentSource: return (CGPoint(x: 20, y: -8), CGPoint(x: 20, y: 8))
        case .npn, .nmos: return (CGPoint(x: 30, y: -6), CGPoint(x: 30, y: 8))
        case .opAmp: return (CGPoint(x: 0, y: -36), CGPoint(x: 0, y: 36))
        case .ic8: return (CGPoint(x: 0, y: -50), CGPoint(x: 0, y: 50))
        case .connector: return (CGPoint(x: 0, y: -30), CGPoint(x: 0, y: 30))
        default: return (CGPoint(x: 0, y: -20), CGPoint(x: 0, y: 20))
        }
    }

    /// Transform from symbol space to world space (rotation is clockwise on screen).
    static func transform(position: CGPoint, rotation: Int) -> CGAffineTransform {
        CGAffineTransform(translationX: position.x, y: position.y)
            .rotated(by: CGFloat(rotation) * .pi / 180)
    }
}

/// A small standalone preview of a symbol (used by the Proteus-style device picker).
struct SymbolPreview: View {
    var kind: ComponentKind
    var value: String

    var body: some View {
        Canvas { ctx, size in
            let b = SchematicSymbols.bounds(kind).insetBy(dx: -6, dy: -6)
            let scale = min(size.width / b.width, size.height / b.height)
            let t = CGAffineTransform(translationX: size.width / 2, y: size.height / 2)
                .scaledBy(x: scale, y: scale)
                .translatedBy(x: -b.midX, y: -b.midY)
            let shapes = SchematicSymbols.shapes(for: kind, value: value)
            ctx.fill(shapes.fill.applying(t), with: .color(Theme.symbolFill))
            ctx.stroke(shapes.stroke.applying(t), with: .color(Theme.symbol), lineWidth: 1.4)
            ctx.fill(shapes.solid.applying(t), with: .color(Theme.symbol))
        }
    }
}
