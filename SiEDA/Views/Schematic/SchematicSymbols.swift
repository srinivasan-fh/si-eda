import SwiftUI

/// Vector schematic symbols, drawn in local symbol coordinates (grid = 10 units, y down).
/// Pin positions match `Library.cpp` exactly.
enum SchematicSymbols {
    /// Local-space hit box for each symbol body.
    static func bounds(_ kind: ComponentKind) -> CGRect {
        switch kind {
        case .resistor, .capacitor, .inductor, .diode, .led, .switchSPST, .fuse:
            return CGRect(x: -30, y: -14, width: 60, height: 28)
        case .voltageSource, .currentSource, .acSource: return CGRect(x: -18, y: -30, width: 36, height: 60)
        case .battery: return CGRect(x: -16, y: -30, width: 32, height: 60)
        case .ground: return CGRect(x: -12, y: -2, width: 24, height: 22)
        case .npn, .nmos: return CGRect(x: -30, y: -30, width: 56, height: 60)
        case .opAmp: return CGRect(x: -40, y: -28, width: 80, height: 56)
        case .connector: return CGRect(x: -20, y: -22, width: 28, height: 44)
        case .ic8: return CGRect(x: -40, y: -42, width: 80, height: 84)
        case .netLabel: return CGRect(x: -2, y: -9, width: 60, height: 18)
        case .custom: return CGRect(x: -40, y: -40, width: 80, height: 80)
        }
    }

    /// Drawn width of a net label's text area (the label text is centred in it).
    static func netLabelTextWidth(_ text: String) -> CGFloat { max(36, CGFloat(text.count) * 7 + 16) }

    /// Hit box of a component, using the generated symbol size for custom parts and the text width for net labels.
    static func bounds(_ kind: ComponentKind, value: String, custom: CustomPartInfo?) -> CGRect {
        guard kind == .netLabel else { return bounds(kind, custom: custom) }
        let base = bounds(kind)
        return CGRect(x: base.minX, y: base.minY, width: max(base.width, netLabelTextWidth(value) + 9), height: base.height)
    }

    /// Hit box of a component, using the generated symbol size for custom parts.
    static func bounds(_ kind: ComponentKind, custom: CustomPartInfo?) -> CGRect {
        guard kind == .custom, let custom else { return bounds(kind) }
        let w = custom.symbol.halfWidth + 20, h = custom.symbol.halfHeight
        return CGRect(x: -w, y: -h, width: 2 * w, height: 2 * h)
    }

    /// Generated DIP-style symbol for a datasheet part: body, pin leads, pin-1 marker.
    static func customShapes(_ part: CustomPartInfo) -> Shapes {
        var s = Shapes()
        let hw = part.symbol.halfWidth, hh = part.symbol.halfHeight
        let body = CGRect(x: -hw, y: -hh, width: 2 * hw, height: 2 * hh)
        s.fill.addRect(body)
        s.stroke.addRect(body)
        for pin in part.symbol.pins {
            let edgeX = pin.x < 0 ? -hw : hw
            s.stroke.move(to: CGPoint(x: edgeX, y: pin.y))
            s.stroke.addLine(to: CGPoint(x: pin.x, y: pin.y))
            if pin.type == PinElectricalType.noConnect.rawValue {
                s.stroke.move(to: CGPoint(x: pin.x - 3, y: pin.y - 3))
                s.stroke.addLine(to: CGPoint(x: pin.x + 3, y: pin.y + 3))
                s.stroke.move(to: CGPoint(x: pin.x - 3, y: pin.y + 3))
                s.stroke.addLine(to: CGPoint(x: pin.x + 3, y: pin.y - 3))
            }
        }
        s.solid.addEllipse(in: CGRect(x: -hw + 4, y: -hh + 4, width: 5, height: 5))
        return s
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

        case .battery:
            // Two cells: long (+) and short (−) plates, positive terminal on top.
            line(p(0, -30), p(0, -10))
            line(p(0, 10), p(0, 30))
            for y in [CGFloat(-10), 2] {
                line(p(-12, y), p(12, y))                                        // long plate (+)
                s.solid.addRect(CGRect(x: -6, y: y + 6, width: 12, height: 2.4))  // short, thick plate (−)
            }
            line(p(0, -2), p(0, 2))  // link between the cells
            line(p(9, -20), p(15, -20))  // "+" mark
            line(p(12, -23), p(12, -17))

        case .acSource:
            s.fill.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            s.stroke.addEllipse(in: CGRect(x: -15, y: -15, width: 30, height: 30))
            line(p(0, -30), p(0, -15))
            line(p(0, 15), p(0, 30))
            s.stroke.move(to: p(-9, 0))
            s.stroke.addCurve(to: p(0, 0), control1: p(-7, -10), control2: p(-2, -10))
            s.stroke.addCurve(to: p(9, 0), control1: p(2, 10), control2: p(7, 10))

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
            // Closed (on): the lever lies flat across both contacts, ──●━━━━●──.
            // Open (off): the lever is lifted well clear of the right contact, ──●╱  ●──.
            let closed = ["on", "closed", "1", "true"].contains(value.lowercased())
            let tip = closed ? p(12, 0) : p(7, -17)
            line(p(-12, 0), tip)
            // A solid bar along the lever so the state reads at any zoom.
            let dx = tip.x + 12, dy = tip.y
            let length = max(hypot(dx, dy), 1)
            let nx = -dy / length * 1.3, ny = dx / length * 1.3
            var lever = Path()
            lever.move(to: p(-12 + nx, ny))
            lever.addLine(to: p(tip.x + nx, tip.y + ny))
            lever.addLine(to: p(tip.x - nx, tip.y - ny))
            lever.addLine(to: p(-12 - nx, -ny))
            lever.closeSubpath()
            s.solid.addPath(lever)

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

        case .custom:
            let body = CGRect(x: -30, y: -30, width: 60, height: 60)
            s.fill.addRect(body)
            s.stroke.addRect(body)

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
        case .voltageSource, .currentSource, .acSource, .battery: return (CGPoint(x: 20, y: -8), CGPoint(x: 20, y: 8))
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

/// A small standalone preview of a symbol (used by the Proteus-style device picker and the library editor).
struct SymbolPreview: View {
    var kind: ComponentKind
    var value: String
    var custom: CustomPartInfo? = nil
    var showPinLabels = false

    var body: some View {
        Canvas { ctx, size in
            let b = SchematicSymbols.bounds(kind, custom: custom).insetBy(dx: -6, dy: custom == nil ? -6 : -16)
            let scale = min(size.width / b.width, size.height / b.height)
            let t = CGAffineTransform(translationX: size.width / 2, y: size.height / 2)
                .scaledBy(x: scale, y: scale)
                .translatedBy(x: -b.midX, y: -b.midY)
            let shapes = custom.map(SchematicSymbols.customShapes) ?? SchematicSymbols.shapes(for: kind, value: value)
            ctx.fill(shapes.fill.applying(t), with: .color(Theme.symbolFill))
            ctx.stroke(shapes.stroke.applying(t), with: .color(Theme.symbol), lineWidth: 1.4)
            ctx.fill(shapes.solid.applying(t), with: .color(Theme.symbol))
            if let custom, showPinLabels {
                let font = max(6, min(11, 7.5 * scale))
                for pin in custom.symbol.pins {
                    let left = pin.x < 0
                    let inner = CGPoint(x: left ? -custom.symbol.halfWidth + 4 : custom.symbol.halfWidth - 4, y: pin.y).applying(t)
                    ctx.draw(Text(pin.name).font(.system(size: font, design: .monospaced)).foregroundColor(Theme.skyBlue),
                             at: inner, anchor: left ? .leading : .trailing)
                    let num = CGPoint(x: (pin.x + (left ? -custom.symbol.halfWidth : custom.symbol.halfWidth)) / 2, y: pin.y - 5).applying(t)
                    ctx.draw(Text(pin.number).font(.system(size: font * 0.85, design: .monospaced)).foregroundColor(Theme.textMuted),
                             at: num)
                }
                let title = CGPoint(x: 0, y: -custom.symbol.halfHeight - 8).applying(t)
                ctx.draw(Text(custom.name).font(.system(size: font + 1, weight: .bold, design: .monospaced))
                            .foregroundColor(Theme.iceBlue), at: title)
            }
        }
    }
}
