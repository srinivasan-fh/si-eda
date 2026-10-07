import AppKit
import SwiftUI

/// Device icons drawn from the real schematic symbol (`SchematicSymbols.shapes`), the way schematic editors show
/// their part palettes: a resistor is its zigzag, a capacitor two plates, a ground the ground bars. The images are
/// templates; `ComponentSymbolImage` tints them with the device's family colour (`ComponentKind.deviceColour`).
@MainActor
enum ComponentSymbolIcon {
    private static var cache: [String: NSImage] = [:]

    /// A template image of `kind`'s symbol fitted into `size` points.
    static func image(_ kind: ComponentKind, size: CGSize = CGSize(width: 24, height: 16)) -> NSImage {
        let key = "\(kind.rawValue)@\(size.width)x\(size.height)"
        if let cached = cache[key] { return cached }
        let image = render(kind, size: size)
        cache[key] = image
        return image
    }

    /// The symbol paths for an icon: a junction is its dot, a net label a short tag.
    static func shapes(_ kind: ComponentKind) -> (SchematicSymbols.Shapes, CGRect) {
        switch kind {
        case .junction:
            var s = SchematicSymbols.Shapes()
            s.solid.addEllipse(in: CGRect(x: -4, y: -4, width: 8, height: 8))
            return (s, CGRect(x: -5, y: -5, width: 10, height: 10))
        case .netLabel:
            return (SchematicSymbols.shapes(for: kind, value: ""), CGRect(x: -2, y: -9, width: 40, height: 18))
        default:
            return (SchematicSymbols.shapes(for: kind, value: ""), SchematicSymbols.bounds(kind))
        }
    }

    /// Scale that fits `bounds` into `size` with a one-point margin.
    static func fitScale(_ bounds: CGRect, into size: CGSize) -> CGFloat {
        let w = max(bounds.width, 1)
        let h = max(bounds.height, 1)
        return min((size.width - 2) / w, (size.height - 2) / h)
    }

    private static func render(_ kind: ComponentKind, size: CGSize) -> NSImage {
        let (shapes, bounds) = shapes(kind)
        let scale = fitScale(bounds, into: size)
        let image = NSImage(size: size, flipped: true) { rect in
            guard let cg = NSGraphicsContext.current?.cgContext else { return false }
            cg.translateBy(x: rect.midX, y: rect.midY)
            cg.scaleBy(x: scale, y: scale)
            cg.translateBy(x: -bounds.midX, y: -bounds.midY)
            cg.setStrokeColor(NSColor.black.cgColor)
            cg.setFillColor(NSColor.black.cgColor)
            cg.setLineWidth(1.3 / scale)
            cg.setLineCap(.round)
            cg.setLineJoin(.round)
            cg.addPath(shapes.stroke.cgPath)
            cg.strokePath()
            cg.addPath(shapes.solid.cgPath)
            cg.fillPath()
            return true
        }
        image.isTemplate = true
        image.accessibilityDescription = kind.displayName
        return image
    }
}

/// A device's schematic-symbol icon for lists and headers, in its device colour (an LED in the colour it lights).
struct ComponentSymbolImage: View {
    let kind: ComponentKind
    var size = CGSize(width: 24, height: 16)
    /// The part's value: picks an LED's colour.
    var value = ""

    var body: some View {
        Image(nsImage: ComponentSymbolIcon.image(kind, size: size))
            .renderingMode(.template)
            .foregroundStyle(kind.deviceColour(value: value).color)
            .frame(width: size.width, height: size.height)
            .accessibilityLabel(Text(kind.displayName))
    }
}

/// Device colours, grouped by family the way schematic part palettes colour-code their groups: power sources red
/// (power ports and flags are red), ground and net items green (earth green, wire and label green), passives in the
/// colours of the parts themselves (amber resistor body, blue capacitor sleeve, copper winding), discrete
/// semiconductors violet, ICs and op-amps the yellow of an IC body, electromechanical parts metal grey.
extension ComponentKind {
    enum DeviceFamily: CaseIterable {
        case power, groundAndNets, passive, discrete, integrated, electromechanical
    }

    var deviceFamily: DeviceFamily {
        switch self {
        case .voltageSource, .battery, .acSource, .currentSource: return .power
        case .ground, .netLabel, .junction: return .groundAndNets
        case .resistor, .capacitor, .inductor, .fuse: return .passive
        case .diode, .led, .npn, .nmos: return .discrete
        case .opAmp, .ic8, .custom: return .integrated
        case .switchSPST, .connector: return .electromechanical
        }
    }

    /// The device's colour; an LED takes the colour of its value ("Green", "Blue 0805", …), red by default.
    func deviceColour(value: String = "") -> SchematicRGB {
        if self == .led, !value.isEmpty { return Self.ledColour(value) }
        switch self {
        case .voltageSource: return SchematicRGB(hex: 0xFF5A5F)
        case .battery: return SchematicRGB(hex: 0xFF7F50)
        case .acSource: return SchematicRGB(hex: 0xFF6FA0)
        case .currentSource: return SchematicRGB(hex: 0xFF8E8E)
        case .ground: return SchematicRGB(hex: 0x3DDC84)
        case .netLabel: return SchematicRGB(hex: 0x9BE15D)
        case .junction: return SchematicRGB(hex: 0x5AD1A8)
        case .resistor: return SchematicRGB(hex: 0xF2B544)
        case .capacitor: return SchematicRGB(hex: 0x4FB8F0)
        case .inductor: return SchematicRGB(hex: 0xE3874A)
        case .fuse: return SchematicRGB(hex: 0xF5D98B)
        case .diode: return SchematicRGB(hex: 0xB98CFF)
        case .led: return SchematicRGB(hex: 0xFF5A4E)
        case .npn: return SchematicRGB(hex: 0xD69CF0)
        case .nmos: return SchematicRGB(hex: 0x9F8CFF)
        case .opAmp: return SchematicRGB(hex: 0xFFD24D)
        case .ic8, .custom: return SchematicRGB(hex: 0xF7C948)
        case .switchSPST: return SchematicRGB(hex: 0xB8C2CC)
        case .connector: return SchematicRGB(hex: 0xD0D7DE)
        }
    }

    /// The colour an LED lights in, from its value ("Red", "Green 0805", "Blue", …).
    static func ledColour(_ value: String) -> SchematicRGB {
        let v = value.lowercased()
        if v.contains("green") { return SchematicRGB(0.30, 1.0, 0.40) }
        if v.contains("blue") { return SchematicRGB(0.35, 0.55, 1.0) }
        if v.contains("yellow") || v.contains("amber") { return SchematicRGB(1.0, 0.85, 0.20) }
        if v.contains("orange") { return SchematicRGB(1.0, 0.55, 0.15) }
        if v.contains("white") { return SchematicRGB(0.95, 0.97, 1.0) }
        return SchematicRGB(1.0, 0.22, 0.18)
    }
}

extension SchematicRGB {
    /// This colour, darkened on a light background or lightened on a dark one until it reaches `minimum` contrast.
    func readable(on background: SchematicRGB, minimum: Double = 3) -> SchematicRGB {
        let target: Double = background.luminance > 0.18 ? 0 : 1
        var c = SchematicRGB(r, g, b)
        for _ in 0..<20 where c.contrast(with: background) < minimum {
            c = SchematicRGB(c.r + (target - c.r) * 0.15, c.g + (target - c.g) * 0.15, c.b + (target - c.b) * 0.15)
        }
        return c
    }
}
