import SwiftUI

/// SiEDA's blue-first visual language. Dark mode is the default appearance.
enum Theme {
    private static var c: AppTheme.Colours { AppTheme.current.colours }

    // Core blues (the app theme's colours; Midnight Navy keeps the original blues)
    static var navy: Color { c.navy.color }              // canvas background
    static var deepBlue: Color { c.deepBlue.color }      // panels
    static var darkBlue: Color { c.darkBlue.color }
    static var blue: Color { c.blue.color }              // accent
    static var lightBlue: Color { c.lightBlue.color }
    static var skyBlue: Color { c.skyBlue.color }
    static var iceBlue: Color { c.iceBlue.color }

    // Text
    static var textPrimary: Color { iceBlue }
    static var textSecondary: Color { lightBlue }
    static var textMuted: Color { c.textMuted.color }

    // Semantic (kept distinct so errors stay noticeable)
    static let error = Color(red: 1.00, green: 0.38, blue: 0.40)
    static let warning = Color(red: 1.00, green: 0.72, blue: 0.30)
    static var success: Color { skyBlue }

    // Schematic canvas
    static var schematicBackground: Color { navy }
    static var gridDot: Color { c.gridDot.color }
    static var symbol: Color { lightBlue }
    static var symbolFill: Color { c.symbolFill.color.opacity(0.65) }
    static var wire: Color { skyBlue }
    static var pin: Color { c.pin.color }
    static var unconnectedPin: Color { warning }
    static let selection = Color(red: 0.98, green: 0.98, blue: 1.00)
    static var label: Color { iceBlue }
    static var valueLabel: Color { c.valueLabel.color }
    static let probe = Color(red: 0.36, green: 0.92, blue: 1.00)
    /// Live simulation: closed switches, running indicators.
    static let liveOn = Color(red: 0.36, green: 0.90, blue: 0.52)
    /// Signal harnesses (bundles of signals) on the schematic.
    static let harness = Color(red: 0.78, green: 0.58, blue: 1.00)

    // PCB canvas (blue copper palette)
    static var pcbBackground: Color { c.pcbBackground.color }
    /// Near-black laminate, so layer and net colours read as in professional CAD (Altium / KiCad dark themes).
    static let boardFill = Color(red: 0.07, green: 0.08, blue: 0.10)
    static var boardEdge: Color { skyBlue }
    // Standard layer colours (KiCad / Altium convention): top red, bottom blue.
    static let topCopper = Color(red: 0.90, green: 0.22, blue: 0.20)
    static let bottomCopper = Color(red: 0.25, green: 0.45, blue: 0.98)
    static let pad = Color(red: 0.86, green: 0.72, blue: 0.38)   // gold (plated copper)
    static let via = Color(red: 0.78, green: 0.80, blue: 0.84)
    static let silkscreen = Color(red: 0.88, green: 0.94, blue: 1.00)
    static let ratsnest = Color(red: 0.75, green: 0.85, blue: 1.00).opacity(0.75)

    /// Copper layer colours (Layer colour mode, layer tabs, X-Ray stack): standard CAD colours per layer.
    static func copperColor(_ layer: Int, layerCount: Int) -> Color {
        if layer == 0 { return topCopper }
        if layer == max(1, layerCount) - 1 { return bottomCopper }
        // Inner layers: yellow, green, orange, magenta (Altium's mid-layer order).
        let inner: [Color] = [Color(red: 0.98, green: 0.84, blue: 0.25), Color(red: 0.30, green: 0.82, blue: 0.40),
                              Color(red: 1.00, green: 0.58, blue: 0.20), Color(red: 0.90, green: 0.40, blue: 0.85)]
        return inner[max(0, layer - 1) % inner.count]
    }

    /// Copper coloured by what it carries: power red, ground blue, negative rails purple; signals take a per-layer
    /// colour that is never red or blue (top yellow, bottom green, inner cyan / orange / pink / lime).
    static func netColor(_ role: NetRole, layer: Int, layerCount: Int) -> Color {
        switch role {
        case .power: return Color(red: 1.00, green: 0.25, blue: 0.22)
        case .ground: return Color(red: 0.27, green: 0.55, blue: 1.00)
        case .negative: return Color(red: 0.74, green: 0.42, blue: 1.00)
        case .signal: return signalColor(layer, layerCount: layerCount)
        }
    }

    static func signalColor(_ layer: Int, layerCount: Int) -> Color {
        if layer == 0 { return Color(red: 0.98, green: 0.84, blue: 0.30) }
        if layer == max(1, layerCount) - 1 { return Color(red: 0.35, green: 0.86, blue: 0.48) }
        let inner: [Color] = [Color(red: 0.30, green: 0.86, blue: 0.95), Color(red: 1.00, green: 0.62, blue: 0.25),
                              Color(red: 1.00, green: 0.52, blue: 0.80), Color(red: 0.72, green: 0.95, blue: 0.32)]
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
    static var seriesColors: [Color] {
        [skyBlue, blue, Color(red: 0.62, green: 0.55, blue: 1.0), iceBlue,
         Color(red: 0.15, green: 0.75, blue: 0.95), Color(red: 0.35, green: 0.40, blue: 0.95), lightBlue]
    }
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
