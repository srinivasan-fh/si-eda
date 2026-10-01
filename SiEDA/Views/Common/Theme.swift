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
    /// Live simulation: closed switches, running indicators.
    static let liveOn = Color(red: 0.36, green: 0.90, blue: 0.52)

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

    /// Copper layer colours: top light blue, bottom indigo, inner layers cyan → violet-blue.
    static func copperColor(_ layer: Int, layerCount: Int) -> Color {
        if layer == 0 { return topCopper }
        if layer == max(1, layerCount) - 1 { return bottomCopper }
        let inner: [Color] = [Color(red: 0.20, green: 0.85, blue: 0.95), Color(red: 0.55, green: 0.50, blue: 1.00),
                              Color(red: 0.25, green: 0.60, blue: 0.85), Color(red: 0.70, green: 0.78, blue: 1.00)]
        return inner[max(0, layer - 1) % inner.count]
    }

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

/// Text field drawn in the blue theme. The system rounded-border field takes its fill from the desktop picture
/// (wallpaper tinting), which turned fields brown over orange wallpapers.
struct BlueFieldStyle: TextFieldStyle {
    func _body(configuration: TextField<Self._Label>) -> some View {
        configuration
            .textFieldStyle(.plain)
            .foregroundStyle(Theme.textPrimary)
            .padding(.horizontal, 7)
            .padding(.vertical, 4)
            .background(RoundedRectangle(cornerRadius: 6, style: .continuous).fill(Theme.navy))
            .overlay(RoundedRectangle(cornerRadius: 6, style: .continuous).strokeBorder(Theme.blue.opacity(0.4), lineWidth: 1))
    }
}

extension TextFieldStyle where Self == BlueFieldStyle {
    /// Blue themed text field (see `BlueFieldStyle`).
    static var blue: BlueFieldStyle { BlueFieldStyle() }
}

/// Window size classes the workspaces adapt to (laptop screens give a window as little as ~900 × 540 pt).
enum LayoutMetrics {
    /// Smallest window the app supports; every workspace must fit it without clipping.
    static let minimumWindow = CGSize(width: 900, height: 540)
    /// Default size of a new window.
    static let defaultWindow = CGSize(width: 1360, height: 860)
    /// Screens narrower than this start with the inspector hidden (toggle it from the toolbar).
    static let inspectorWidthThreshold: CGFloat = 1300
    /// Largest minimum size a workspace may need (the detail area of a minimum-size window with the sidebar shown).
    static let workspaceBudget = CGSize(width: 620, height: 440)
}

extension View {
    /// Root frame of a document window. Its minimum is fixed (min and max both set), so the window's minimum
    /// size never follows the content: content whose minimum changes during layout would otherwise make AppKit
    /// re-solve the window constraints in a loop (macOS 26 aborts the app).
    func documentWindowFrame() -> some View {
        frame(minWidth: LayoutMetrics.minimumWindow.width, maxWidth: .infinity,
              minHeight: LayoutMetrics.minimumWindow.height, maxHeight: .infinity)
    }
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

/// Canvas viewport transform shared by the schematic and PCB editors (screen = world × scale + offset).
struct Viewport: Equatable {
    var scale: CGFloat
    var offset: CGSize

    /// Schematic: world units are grid units (10 = one grid step). 100 % = 1.6 points per unit. The range covers
    /// multi-sheet motherboard schematics (tens of thousands of units) down to single-pin detail.
    static let schematicLimits: ClosedRange<CGFloat> = 0.02...24
    static let schematicBaseScale: CGFloat = 1.6
    /// PCB: world units are millimetres. 100 % = 12 points per mm. 0.25 fits a 600 mm backplane in a small window;
    /// 400 shows 0.4 mm-pitch BGA pads 160 points apart.
    static let pcbLimits: ClosedRange<CGFloat> = 0.25...400
    static let pcbBaseScale: CGFloat = 12

    func toScreen(_ p: CGPoint) -> CGPoint {
        CGPoint(x: p.x * scale + offset.width, y: p.y * scale + offset.height)
    }

    func toWorld(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x - offset.width) / scale, y: (p.y - offset.height) / scale)
    }

    /// World-space rectangle currently visible in a canvas of `size` points.
    func visibleWorldRect(in size: CGSize) -> CGRect {
        let a = toWorld(.zero)
        return CGRect(x: a.x, y: a.y, width: size.width / scale, height: size.height / scale)
    }

    /// Zooms by `factor` keeping `anchor` (screen space) fixed.
    mutating func zoom(by factor: CGFloat, anchor: CGPoint, limits: ClosedRange<CGFloat>) {
        guard factor.isFinite, factor > 0 else { return }
        let world = toWorld(anchor)
        scale = min(max(scale * factor, limits.lowerBound), limits.upperBound)
        offset = CGSize(width: anchor.x - world.x * scale, height: anchor.y - world.y * scale)
    }

    /// Sets an absolute scale, keeping `anchor` (screen space) fixed.
    mutating func setScale(_ newScale: CGFloat, anchor: CGPoint, limits: ClosedRange<CGFloat>) {
        zoom(by: newScale / scale, anchor: anchor, limits: limits)
    }

    /// Moves the view by `delta` screen points (content follows the pointer).
    mutating func pan(by delta: CGSize) {
        offset.width += delta.width
        offset.height += delta.height
    }

    /// Puts world point `p` in the middle of a canvas of `size` points without changing the zoom.
    mutating func center(on p: CGPoint, in size: CGSize) {
        offset = CGSize(width: size.width / 2 - p.x * scale, height: size.height / 2 - p.y * scale)
    }

    /// Fits `rect` (world space) into `size` (screen space) with a margin.
    mutating func fit(_ rect: CGRect, in size: CGSize, margin: CGFloat = 40, limits: ClosedRange<CGFloat>) {
        guard !rect.isNull, rect.width > 0 || rect.height > 0, size.width > 0, size.height > 0 else { return }
        let usableW = max(size.width - 2 * margin, size.width * 0.5)
        let usableH = max(size.height - 2 * margin, size.height * 0.5)
        let sx = usableW / max(rect.width, 1e-6)
        let sy = usableH / max(rect.height, 1e-6)
        scale = min(max(min(sx, sy), limits.lowerBound), limits.upperBound)
        center(on: CGPoint(x: rect.midX, y: rect.midY), in: size)
    }

    /// Grid pitch (a multiple of `base` by factors of 5, then 2) whose on-screen spacing is at least `minimumPoints`.
    func gridPitch(base: CGFloat, minimumPoints: CGFloat) -> CGFloat {
        var pitch = base
        var step = 0
        while pitch * scale < minimumPoints, step < 40 {
            pitch *= step.isMultiple(of: 2) ? 5 : 2
            step += 1
        }
        return pitch
    }
}
