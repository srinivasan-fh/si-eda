import AppKit
import SwiftUI

/// Device icons drawn from the real schematic symbol (`SchematicSymbols.shapes`), the way schematic editors show
/// their part palettes: a resistor is its zigzag, a capacitor two plates, a ground the ground bars. The images are
/// templates, so they take the colour of the surrounding `foregroundStyle`.
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

/// A device's schematic-symbol icon for lists and headers.
struct ComponentSymbolImage: View {
    let kind: ComponentKind
    var size = CGSize(width: 24, height: 16)

    var body: some View {
        Image(nsImage: ComponentSymbolIcon.image(kind, size: size))
            .renderingMode(.template)
            .frame(width: size.width, height: size.height)
            .accessibilityLabel(Text(kind.displayName))
    }
}
