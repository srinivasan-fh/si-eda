import SwiftUI

/// App-wide colour theme (Settings → Appearance → App Theme): the panels, accents and text of every window
/// (`Theme`). Board colours (copper layers, pads, errors, warnings) stay standard in every theme. Choosing a theme
/// also picks the matching schematic scheme, which can still be changed on its own.
enum AppTheme: String, CaseIterable, Identifiable {
    case midnightNavy, matrixGreen, graphite

    static let storageKey = "appTheme"
    /// The theme in use, read by every `Theme` colour; set through `select`.
    static var current = AppTheme(rawValue: UserDefaults.standard.string(forKey: storageKey) ?? "") ?? .midnightNavy

    var id: String { rawValue }

    var title: String {
        switch self {
        case .midnightNavy: return "Midnight Navy"
        case .matrixGreen: return "Matrix Green"
        case .graphite: return "Graphite"
        }
    }

    var schematicScheme: SchematicColorScheme {
        switch self {
        case .midnightNavy: return .siedaDark
        case .matrixGreen: return .matrix
        case .graphite: return .cr8000
        }
    }

    /// Makes `theme` current and stores it (the app's `@AppStorage` then redraws every window), with its schematic scheme.
    static func select(_ theme: AppTheme) {
        current = theme
        UserDefaults.standard.set(theme.schematicScheme.rawValue, forKey: SchematicColorScheme.storageKey)
        UserDefaults.standard.set(theme.rawValue, forKey: storageKey)
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
}
