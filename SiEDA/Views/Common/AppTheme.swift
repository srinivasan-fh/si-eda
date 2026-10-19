import SwiftUI

/// App-wide colour theme (Settings → Appearance → App Theme): the panels, accents and text of every window
/// (`Theme`). Board colours (copper layers, pads, errors, warnings) stay standard in every theme. Choosing a theme
/// also picks the matching schematic scheme, which can still be changed on its own.
enum AppTheme: String, CaseIterable, Identifiable {
    case midnightNavy, matrixGreen, graphite, light

    static let storageKey = "appTheme"
    /// The theme in use, read by every `Theme` colour; set through `select`. Observed: a view whose body read a
    /// `Theme` colour redraws when the theme changes, in place (no window rebuilt, no editor state lost).
    static var current: AppTheme {
        get { Model.shared.theme }
        set { Model.shared.theme = newValue }
    }

    @Observable private final class Model {
        static let shared = Model()
        var theme = AppTheme(rawValue: UserDefaults.standard.string(forKey: AppTheme.storageKey) ?? "") ?? .midnightNavy
    }

    /// The light theme puts the system controls in light mode too; the dark themes keep the Appearance setting.
    var colorScheme: ColorScheme? { self == .light ? .light : nil }

    var id: String { rawValue }

    var title: String {
        switch self {
        case .midnightNavy: return "Midnight Navy"
        case .matrixGreen: return "Matrix Green"
        case .graphite: return "Graphite"
        case .light: return "Light"
        }
    }

    var schematicScheme: SchematicColorScheme {
        switch self {
        case .midnightNavy: return .siedaDark
        case .matrixGreen: return .matrix
        case .graphite: return .cr8000
        case .light: return .allegro
        }
    }

    /// Makes `theme` current (every view drawn with it redraws at once) and stores it, with its schematic scheme.
    static func select(_ theme: AppTheme) {
        current = theme
        UserDefaults.standard.set(theme.schematicScheme.rawValue, forKey: SchematicColorScheme.storageKey)
        UserDefaults.standard.set(theme.rawValue, forKey: storageKey)
    }

    /// One of SiEDA's original dark navy backdrop shades (splash, 3D views) in the current theme: Midnight Navy keeps
    /// it exactly; the other themes give their own background colour at the same brightness.
    static func backdrop(_ r: Double, _ g: Double, _ b: Double, alpha: Double = 1) -> SchematicRGB {
        guard current != .midnightNavy else { return SchematicRGB(r, g, b, a: alpha) }
        let ref = midnight.navy, own = current.colours.navy
        let k = (r + g + b) / max(1e-6, ref.r + ref.g + ref.b)
        return SchematicRGB(min(1, own.r * k), min(1, own.g * k), min(1, own.b * k), a: alpha)
    }

    struct Colours {
        var navy, deepBlue, darkBlue, blue, lightBlue, skyBlue, iceBlue, textMuted: SchematicRGB
        var gridDot, symbolFill, pin, valueLabel, pcbBackground: SchematicRGB
    }

    var colours: Colours {
        switch self {
        case .midnightNavy: return Self.midnight
        case .matrixGreen: return Self.matrix
        case .graphite: return Self.graphiteColours
        case .light: return Self.lightColours
        }
    }

    /// SiEDA's original blues, unchanged.
    private static let midnight = Colours(
        navy: .init(0.04, 0.07, 0.14), deepBlue: .init(0.06, 0.12, 0.25), darkBlue: .init(0.10, 0.23, 0.48),
        blue: .init(0.20, 0.47, 0.96), lightBlue: .init(0.45, 0.68, 1.00), skyBlue: .init(0.49, 0.83, 0.99),
        iceBlue: .init(0.80, 0.91, 1.00), textMuted: .init(0.42, 0.53, 0.72), gridDot: .init(0.16, 0.24, 0.40),
        symbolFill: .init(0.10, 0.18, 0.34), pin: .init(0.62, 0.78, 1.00), valueLabel: .init(0.55, 0.70, 0.95),
        pcbBackground: .init(0.02, 0.04, 0.09))

    /// Phosphor green on black (the schematic's Matrix Green scheme).
    private static let matrix = Colours(
        navy: .init(hex: 0x020A04), deepBlue: .init(hex: 0x06200E), darkBlue: .init(hex: 0x0C4A1C),
        blue: .init(hex: 0x00B33C), lightBlue: .init(hex: 0x4FE07A), skyBlue: .init(hex: 0x00FF41),
        iceBlue: .init(hex: 0xD2FFDC), textMuted: .init(hex: 0x6FAF7F), gridDot: .init(hex: 0x0F3A18),
        symbolFill: .init(hex: 0x021A08), pin: .init(hex: 0x2EE068), valueLabel: .init(hex: 0x8FE6A2),
        pcbBackground: .init(hex: 0x010602))

    /// Neutral charcoal greys with a steel-blue accent.
    private static let graphiteColours = Colours(
        navy: .init(hex: 0x16181C), deepBlue: .init(hex: 0x22252B), darkBlue: .init(hex: 0x3A3F48),
        blue: .init(hex: 0x4F7FC9), lightBlue: .init(hex: 0xA9B6C8), skyBlue: .init(hex: 0x8FC3E8),
        iceBlue: .init(hex: 0xECEFF3), textMuted: .init(hex: 0x8A919C), gridDot: .init(hex: 0x2E323A),
        symbolFill: .init(hex: 0x2A2E35), pin: .init(hex: 0xBFC8D6), valueLabel: .init(hex: 0x9FB0C6),
        pcbBackground: .init(hex: 0x0E0F11))

    /// White panels and dark text with a blue accent; the board keeps a dark background for its standard colours.
    private static let lightColours = Colours(
        navy: .init(hex: 0xF7F8FA), deepBlue: .init(hex: 0xE9EDF3), darkBlue: .init(hex: 0xC9D3E2),
        blue: .init(hex: 0x1F5FD1), lightBlue: .init(hex: 0x2E3A4D), skyBlue: .init(hex: 0x0B63C4),
        iceBlue: .init(hex: 0x111827), textMuted: .init(hex: 0x5B6576), gridDot: .init(hex: 0xC9D0DA),
        symbolFill: .init(hex: 0xFFFFFF), pin: .init(hex: 0x1D3F7A), valueLabel: .init(hex: 0x3A5A8C),
        pcbBackground: .init(hex: 0x14171C))
}
