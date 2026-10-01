import SwiftUI

/// SiEDA's blue-first visual language. Dark mode is the default appearance.
enum Theme {
    // Core blues
    static let navy = Color(red: 0.04, green: 0.07, blue: 0.14)          // canvas background
    static let deepBlue = Color(red: 0.06, green: 0.12, blue: 0.25)      // panels
    static let darkBlue = Color(red: 0.10, green: 0.23, blue: 0.48)
    static let blue = Color(red: 0.20, green: 0.47, blue: 0.96)          // accent
    static let lightBlue = Color(red: 0.45, green: 0.68, blue: 1.00)
    static let skyBlue = Color(red: 0.49, green: 0.83, blue: 0.99)
    static let iceBlue = Color(red: 0.80, green: 0.91, blue: 1.00)

    // Text
    static let textPrimary = iceBlue
    static let textSecondary = lightBlue
    static let textMuted = Color(red: 0.42, green: 0.53, blue: 0.72)

    // Semantic (kept distinct so errors stay noticeable)
    static let error = Color(red: 1.00, green: 0.38, blue: 0.40)
    static let warning = Color(red: 1.00, green: 0.72, blue: 0.30)
    static let success = skyBlue

    // Schematic canvas
    static let schematicBackground = navy
    static let gridDot = Color(red: 0.16, green: 0.24, blue: 0.40)
    static let symbol = lightBlue
    static let symbolFill = Color(red: 0.10, green: 0.18, blue: 0.34).opacity(0.65)
    static let wire = skyBlue
    static let pin = Color(red: 0.62, green: 0.78, blue: 1.00)
    static let unconnectedPin = warning
    static let selection = Color(red: 0.98, green: 0.98, blue: 1.00)
    static let label = iceBlue
    static let valueLabel = Color(red: 0.55, green: 0.70, blue: 0.95)
    static let probe = Color(red: 0.36, green: 0.92, blue: 1.00)

    // PCB canvas (blue copper palette)
    static let pcbBackground = Color(red: 0.02, green: 0.04, blue: 0.09)
    static let boardFill = Color(red: 0.05, green: 0.12, blue: 0.27)
    static let boardEdge = skyBlue
    static let topCopper = Color(red: 0.36, green: 0.66, blue: 1.00)
    static let bottomCopper = Color(red: 0.22, green: 0.30, blue: 0.78)
    static let pad = Color(red: 0.70, green: 0.86, blue: 1.00)
    static let via = Color(red: 0.62, green: 0.80, blue: 1.00)
    static let silkscreen = Color(red: 0.88, green: 0.94, blue: 1.00)
    static let ratsnest = Color(red: 0.75, green: 0.85, blue: 1.00).opacity(0.75)

    static func severityColor(_ s: ViolationSeverity) -> Color {
        switch s {
        case .error: return error
        case .warning: return warning
        case .info: return lightBlue
        }
    }

    static func severityIcon(_ s: ViolationSeverity) -> String {
        switch s {
        case .error: return "xmark.octagon.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .info: return "info.circle.fill"
        }
    }

    /// Series colours for charts — all blues, distinguishable by lightness.
    static let seriesColors: [Color] = [
        skyBlue, blue, Color(red: 0.62, green: 0.55, blue: 1.0), iceBlue,
        Color(red: 0.15, green: 0.75, blue: 0.95), Color(red: 0.35, green: 0.40, blue: 0.95), lightBlue,
    ]
}

/// Blue gradient card used across workspaces.
struct PanelBackground: ViewModifier {
    func body(content: Content) -> some View {
        content
            .background(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .fill(LinearGradient(colors: [Theme.deepBlue.opacity(0.85), Theme.navy.opacity(0.9)],
                                         startPoint: .topLeading, endPoint: .bottomTrailing))
            )
            .overlay(
                RoundedRectangle(cornerRadius: 12, style: .continuous)
                    .strokeBorder(Theme.blue.opacity(0.25), lineWidth: 1)
            )
    }
}

extension View {
    func bluePanel() -> some View { modifier(PanelBackground()) }
}

/// Small blue capsule label.
struct Badge: View {
    var text: String
    var systemImage: String?

    var body: some View {
        HStack(spacing: 4) {
            if let systemImage { Image(systemName: systemImage) }
            Text(text)
        }
        .font(.caption.weight(.semibold))
        .foregroundStyle(Theme.skyBlue)
        .padding(.horizontal, 8)
        .padding(.vertical, 3)
        .background(Capsule().fill(Theme.blue.opacity(0.18)))
        .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.4), lineWidth: 1))
    }
}

/// Canvas viewport transform shared by the schematic and PCB editors.
struct Viewport: Equatable {
    var scale: CGFloat
    var offset: CGSize

    func toScreen(_ p: CGPoint) -> CGPoint {
        CGPoint(x: p.x * scale + offset.width, y: p.y * scale + offset.height)
    }

    func toWorld(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x - offset.width) / scale, y: (p.y - offset.height) / scale)
    }

    /// Zooms by `factor` keeping `anchor` (screen space) fixed.
    mutating func zoom(by factor: CGFloat, anchor: CGPoint, limits: ClosedRange<CGFloat>) {
        let world = toWorld(anchor)
        scale = min(max(scale * factor, limits.lowerBound), limits.upperBound)
        offset = CGSize(width: anchor.x - world.x * scale, height: anchor.y - world.y * scale)
    }

    /// Fits `rect` (world space) into `size` (screen space) with a margin.
    mutating func fit(_ rect: CGRect, in size: CGSize, margin: CGFloat = 40, limits: ClosedRange<CGFloat>) {
        guard rect.width > 0 || rect.height > 0, size.width > 0, size.height > 0 else { return }
        let sx = (size.width - 2 * margin) / max(rect.width, 1)
        let sy = (size.height - 2 * margin) / max(rect.height, 1)
        scale = min(max(min(sx, sy), limits.lowerBound), limits.upperBound)
        offset = CGSize(width: size.width / 2 - rect.midX * scale, height: size.height / 2 - rect.midY * scale)
    }
}
