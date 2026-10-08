import AppKit
import SwiftUI

/// An sRGB colour of the schematic palettes: plain numbers, so contrast, nearest-name matching and persistence need no
/// colour-space conversion. `color` builds the SwiftUI colour exactly as `Theme` does (`Color(red:green:blue:)`).
struct SchematicRGB: Equatable, Hashable {
    var r: Double
    var g: Double
    var b: Double
    var a: Double = 1

    init(_ r: Double, _ g: Double, _ b: Double, a: Double = 1) {
        self.r = r
        self.g = g
        self.b = b
        self.a = a
    }

    init(hex: UInt32) {
        self.init(Double((hex >> 16) & 0xFF) / 255, Double((hex >> 8) & 0xFF) / 255, Double(hex & 0xFF) / 255)
    }

    /// "#RRGGBB" or "#RRGGBBAA" (case-insensitive, "#" optional); nil when it is neither.
    init?(hexString: String) {
        let digits = hexString.hasPrefix("#") ? String(hexString.dropFirst()) : hexString
        guard digits.count == 6 || digits.count == 8, let value = UInt32(digits, radix: 16) else { return nil }
        if digits.count == 6 {
            self.init(hex: value)
        } else {
            self.init(hex: value >> 8)
            a = Double(value & 0xFF) / 255
        }
    }

    var color: Color {
        let base = Color(red: r, green: g, blue: b)
        return a >= 1 ? base : base.opacity(a)
    }

    var nsColor: NSColor { NSColor(srgbRed: r, green: g, blue: b, alpha: a) }

    /// "#RRGGBB", or "#RRGGBBAA" for a translucent colour.
    var hexString: String {
        func byte(_ v: Double) -> Int { Int((min(max(v, 0), 1) * 255).rounded()) }
        let rgb = String(format: "#%02X%02X%02X", byte(r), byte(g), byte(b))
        return a >= 1 ? rgb : rgb + String(format: "%02X", byte(a))
    }

    /// WCAG 2 relative luminance.
    var luminance: Double {
        func channel(_ c: Double) -> Double { c <= 0.04045 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4) }
        return 0.2126 * channel(r) + 0.7152 * channel(g) + 0.0722 * channel(b)
    }

    /// WCAG 2 contrast ratio (1…21) between two opaque colours.
    func contrast(with other: SchematicRGB) -> Double {
        let l1 = luminance, l2 = other.luminance
        return (max(l1, l2) + 0.05) / (min(l1, l2) + 0.05)
    }

    /// Euclidean distance in sRGB (0…√3).
    func distance(to other: SchematicRGB) -> Double {
        let dr = r - other.r, dg = g - other.g, db = b - other.b
        return (dr * dr + dg * dg + db * db).squareRoot()
    }

    /// The same colour from a SwiftUI colour (ColorPicker), in sRGB; nil when it cannot be converted.
    init?(_ color: Color) {
        guard let c = NSColor(color).usingColorSpace(.sRGB) else { return nil }
        self.init(Double(c.redComponent), Double(c.greenComponent), Double(c.blueComponent))
    }
}

/// Every colour the schematic canvas draws with. Opacities applied by the drawing code (halos, frames) stay there.
struct SchematicPalette: Equatable {
    var background: SchematicRGB
    var gridMinor: SchematicRGB
    var gridMajor: SchematicRGB
    /// Sheet frame and title block outline.
    var sheetBorder: SchematicRGB
    /// Sheet template name.
    var sheetText: SchematicRGB
    var wire: SchematicRGB
    var junction: SchematicRGB
    var bus: SchematicRGB
    /// Global net labels.
    var netLabel: SchematicRGB
    /// Sheet-local net labels.
    var localLabel: SchematicRGB
    /// Hierarchical ports and sheet entries.
    var portLabel: SchematicRGB
    var symbol: SchematicRGB
    var symbolFill: SchematicRGB
    /// Connected pin dots.
    var pin: SchematicRGB
    var pinName: SchematicRGB
    /// Power pins' names (power label text).
    var powerLabel: SchematicRGB
    var pinNumber: SchematicRGB
    /// Designators, sheet names, title.
    var designator: SchematicRGB
    var value: SchematicRGB
    var variantValue: SchematicRGB
    var unconnectedPin: SchematicRGB
    var noConnect: SchematicRGB
    var selection: SchematicRGB
    /// Glow under selected items (drawn translucent).
    var selectionHalo: SchematicRGB
    var harness: SchematicRGB
    /// ERC / DNP markers.
    var error: SchematicRGB
    /// Rubber-band wire, placement ghost, hover, marquee.
    var preview: SchematicRGB
    /// Net directive flags.
    var directive: SchematicRGB
    /// Closed switches in the live simulation.
    var liveOn: SchematicRGB
    /// Live probe and switch pills.
    var overlayFill: SchematicRGB
    var overlayText: SchematicRGB
    var overlayMuted: SchematicRGB
    /// Text on a `liveOn` pill.
    var overlayOnText: SchematicRGB
    var probe: SchematicRGB
    /// Cursor coordinate read-out.
    var readout: SchematicRGB

    /// Foregrounds drawn straight on the background (each must reach 3:1 against it).
    var foregrounds: [(name: String, colour: SchematicRGB)] {
        [("sheetBorder", sheetBorder), ("sheetText", sheetText), ("wire", wire), ("junction", junction), ("bus", bus),
         ("netLabel", netLabel), ("localLabel", localLabel), ("portLabel", portLabel), ("symbol", symbol), ("pin", pin),
         ("pinName", pinName), ("powerLabel", powerLabel), ("pinNumber", pinNumber), ("designator", designator),
         ("value", value), ("variantValue", variantValue), ("unconnectedPin", unconnectedPin), ("noConnect", noConnect),
         ("selection", selection), ("harness", harness), ("error", error), ("preview", preview),
         ("directive", directive), ("liveOn", liveOn), ("readout", readout)]
    }

    /// Text on the live-simulation pills, against the pill it sits on.
    var overlayPairs: [(name: String, text: SchematicRGB, fill: SchematicRGB)] {
        [("probe", probe, overlayFill), ("overlayText", overlayText, overlayFill),
         ("overlayMuted", overlayMuted, overlayFill), ("overlayOnText", overlayOnText, liveOn)]
    }
}

/// Grid drawn behind the schematic (the pitch adapts to the zoom in every style).
enum SchematicGridStyle: String, CaseIterable, Identifiable {
    case dots, lines, none
    var id: String { rawValue }
    static let storageKey = "schematic.gridStyle"
    static let majorStorageKey = "schematic.gridMajorEvery"
    static let defaultMajorEvery = 10
    static let majorChoices = [4, 5, 8, 10]

    var title: LocalizedStringKey {
        switch self {
        case .dots: return "Dots"
        case .lines: return "Lines"
        case .none: return "None"
        }
    }
}

/// Colour scheme of the schematic canvas: one name per scheme (shown with a swatch). The raw values are what is saved,
/// independent of the names. Each comment notes the familiar look it was inspired by (docs only, never in the UI).
/// Midnight Navy (`siedaDark`) is today's palette and the default.
enum SchematicColorScheme: String, CaseIterable, Identifiable {
    case siedaDark, altium, orcad, allegro, xpedition, pads, cr8000, kicad, eagle, proteus, easyeda, diptrace
    case monochrome, highContrast, matrix, custom

    var id: String { rawValue }
    static let storageKey = "schematic.colorScheme"
    static let defaultScheme = SchematicColorScheme.siedaDark

    /// English name (a key of the string tables).
    var title: String {
        switch self {
        case .siedaDark: return "Midnight Navy"        // SiEDA: navy, sky-blue wires
        case .altium: return "Classic Cream"           // Altium-like: cream, navy wires
        case .orcad: return "Paper White"              // OrCAD-like: white, dark-blue wires
        case .allegro: return "Blueprint Light"        // Allegro System Capture-like: white, blue wires
        case .xpedition: return "Night Forest"         // Xpedition-like: black, green wires
        case .pads: return "Amber Night"               // PADS-like: black, yellow wires
        case .cr8000: return "Slate Cyan"              // CR-8000-like: charcoal, cyan wires
        case .kicad: return "Meadow Cream"             // KiCad-like: cream, green wires
        case .eagle: return "Mint Paper"               // Eagle / Fusion-like: white, green wires
        case .proteus: return "Ivory Garden"           // Proteus-like: ivory, forest-green wires
        case .easyeda: return "Ink Blue"               // EasyEDA-like: white, navy wires
        case .diptrace: return "Silver Mist"           // DipTrace-like: light grey, dark-blue wires
        case .monochrome: return "Print Mono"          // white, black
        case .highContrast: return "High Contrast"     // black, white / bright colours
        case .matrix: return "Matrix Green"            // black, phosphor-green code-rain wires
        case .custom: return "Custom"                  // the user's own named colours
        }
    }

    /// The name, translated.
    var localizedTitle: String { NSLocalizedString(title, comment: "Schematic colour scheme") }

    /// The built-in palette (nil for Custom, which lives in `SchematicCustomTheme`).
    var presetPalette: SchematicPalette? {
        switch self {
        case .siedaDark: return SchematicPalette.siedaDark
        case .altium: return SchematicPalette.altium
        case .orcad: return SchematicPalette.orcad
        case .allegro: return SchematicPalette.allegro
        case .xpedition: return SchematicPalette.xpedition
        case .pads: return SchematicPalette.pads
        case .cr8000: return SchematicPalette.cr8000
        case .kicad: return SchematicPalette.kicad
        case .eagle: return SchematicPalette.eagle
        case .proteus: return SchematicPalette.proteus
        case .easyeda: return SchematicPalette.easyeda
        case .diptrace: return SchematicPalette.diptrace
        case .monochrome: return SchematicPalette.monochrome
        case .highContrast: return SchematicPalette.highContrast
        case .matrix: return SchematicPalette.matrix
        case .custom: return nil
        }
    }

    static var presets: [SchematicColorScheme] { allCases.filter { $0 != .custom } }
    static var lightPresets: [SchematicColorScheme] { presets.filter { !$0.isDark } }
    static var darkPresets: [SchematicColorScheme] { presets.filter(\.isDark) }

    var isDark: Bool { (presetPalette?.background.luminance ?? 0) < 0.5 }

    /// The palette to draw with: a preset, or the custom theme (stored JSON, see `SchematicCustomTheme`).
    static func palette(scheme raw: String, customJSON: String) -> SchematicPalette {
        let scheme = SchematicColorScheme(rawValue: raw) ?? defaultScheme
        if let preset = scheme.presetPalette { return preset }
        return SchematicCustomTheme.load(customJSON).palette()
    }
}

/// What the schematic canvas draws with: palette and grid. Passed through the environment.
struct SchematicCanvasStyle: Equatable {
    var palette: SchematicPalette = .siedaDark
    var grid: SchematicGridStyle = .dots
    var majorEvery: Int = SchematicGridStyle.defaultMajorEvery
    /// Symbols drawn in their device colour (`ComponentKind.deviceColour`, the default); off = the scheme's one symbol colour.
    var colourByDevice = true

    static let standard = SchematicCanvasStyle()
    static let colourByDeviceKey = "schematic.colourByDevice"

    /// The style from the stored preferences (the `@AppStorage` values).
    static func from(scheme: String, customJSON: String, grid: String, majorEvery: Int,
                     colourByDevice: Bool = true) -> SchematicCanvasStyle {
        SchematicCanvasStyle(palette: SchematicColorScheme.palette(scheme: scheme, customJSON: customJSON),
                             grid: SchematicGridStyle(rawValue: grid) ?? .dots,
                             majorEvery: max(2, majorEvery), colourByDevice: colourByDevice)
    }
}

private struct SchematicCanvasStyleKey: EnvironmentKey {
    static let defaultValue = SchematicCanvasStyle.standard
}

extension EnvironmentValues {
    var schematicStyle: SchematicCanvasStyle {
        get { self[SchematicCanvasStyleKey.self] }
        set { self[SchematicCanvasStyleKey.self] = newValue }
    }
}

// MARK: - Presets

extension SchematicPalette {
    private static func h(_ hex: UInt32) -> SchematicRGB { SchematicRGB(hex: hex) }

    /// Today's colours, taken from the same numbers as `Theme` (bit-for-bit the same rendering).
    static let siedaDark = SchematicPalette(
        background: SchematicRGB(0.04, 0.07, 0.14),         // Theme.navy
        gridMinor: SchematicRGB(0.16, 0.24, 0.40),          // Theme.gridDot
        gridMajor: SchematicRGB(0.10, 0.23, 0.48),          // Theme.darkBlue
        sheetBorder: SchematicRGB(0.45, 0.68, 1.00),        // Theme.symbol
        sheetText: SchematicRGB(0.42, 0.53, 0.72),          // Theme.textMuted
        wire: SchematicRGB(0.49, 0.83, 0.99),               // Theme.wire
        junction: SchematicRGB(0.49, 0.83, 0.99),
        bus: SchematicRGB(0.45, 0.68, 1.00),                // Theme.lightBlue
        netLabel: SchematicRGB(0.49, 0.83, 0.99),           // Theme.skyBlue
        localLabel: SchematicRGB(0.42, 0.53, 0.72),
        portLabel: SchematicRGB(1.00, 0.72, 0.30),          // Theme.warning
        symbol: SchematicRGB(0.45, 0.68, 1.00),
        symbolFill: SchematicRGB(0.10, 0.18, 0.34, a: 0.65),  // Theme.symbolFill
        pin: SchematicRGB(0.62, 0.78, 1.00),                // Theme.pin
        pinName: SchematicRGB(0.49, 0.83, 0.99),
        powerLabel: SchematicRGB(0.36, 0.92, 1.00),         // Theme.probe
        pinNumber: SchematicRGB(0.42, 0.53, 0.72),
        designator: SchematicRGB(0.80, 0.91, 1.00),         // Theme.label
        value: SchematicRGB(0.55, 0.70, 0.95),              // Theme.valueLabel
        variantValue: SchematicRGB(1.00, 0.72, 0.30),
        unconnectedPin: SchematicRGB(1.00, 0.72, 0.30),     // Theme.unconnectedPin
        noConnect: SchematicRGB(0.49, 0.83, 0.99),
        selection: SchematicRGB(0.98, 0.98, 1.00),          // Theme.selection
        selectionHalo: SchematicRGB(0.20, 0.47, 0.96),      // Theme.blue
        harness: SchematicRGB(0.78, 0.58, 1.00),            // Theme.harness
        error: SchematicRGB(1.00, 0.38, 0.40),              // Theme.error
        preview: SchematicRGB(0.49, 0.83, 0.99),
        directive: SchematicRGB(0.36, 0.90, 0.52),          // Theme.liveOn
        liveOn: SchematicRGB(0.36, 0.90, 0.52),
        overlayFill: SchematicRGB(0.06, 0.12, 0.25),        // Theme.deepBlue
        overlayText: SchematicRGB(0.45, 0.68, 1.00),        // Theme.textSecondary
        overlayMuted: SchematicRGB(0.42, 0.53, 0.72),
        overlayOnText: SchematicRGB(0.06, 0.12, 0.25),
        probe: SchematicRGB(0.36, 0.92, 1.00),
        readout: SchematicRGB(0.49, 0.83, 0.99))

    static let altium = SchematicPalette(
        background: h(0xFFFCF0),
        gridMinor: h(0xE6E4DA),
        gridMajor: h(0xC8C6BC),
        sheetBorder: h(0x8B0000),
        sheetText: h(0x5A5A5A),
        wire: h(0x000080),
        junction: h(0x000080),
        bus: h(0x000080),
        netLabel: h(0x1F3FBF),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x8B0000),
        symbolFill: h(0xFFFFB0),
        pin: h(0x000000),
        pinName: h(0x000000),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x000080),
        value: h(0x3C3C3C),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0x00A000),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x00703A),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let orcad = SchematicPalette(
        background: h(0xFFFFFF),
        gridMinor: h(0xD2D2D2),
        gridMajor: h(0xAAAAAA),
        sheetBorder: h(0x990000),
        sheetText: h(0x404040),
        wire: h(0x000099),
        junction: h(0x000099),
        bus: h(0x000099),
        netLabel: h(0x000099),
        localLabel: h(0x404040),
        portLabel: h(0x9A5200),
        symbol: h(0x990000),
        symbolFill: h(0xFFFFFF),
        pin: h(0x000000),
        pinName: h(0x000000),
        powerLabel: h(0xA00060),
        pinNumber: h(0x404040),
        designator: h(0x000000),
        value: h(0x000000),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xE000E0),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x00703A),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let allegro = SchematicPalette(
        background: h(0xFFFFFF),
        gridMinor: h(0xE2E6EC),
        gridMajor: h(0xC2C8D2),
        sheetBorder: h(0x2B2B2B),
        sheetText: h(0x5A5A5A),
        wire: h(0x1F4E9E),
        junction: h(0x1F4E9E),
        bus: h(0x00205B),
        netLabel: h(0x1F4E9E),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x2B2B2B),
        symbolFill: h(0xF7F9FC),
        pin: h(0x2B2B2B),
        pinName: h(0x1A1A1A),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x1A1A1A),
        value: h(0x4A4A4A),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xE05A00),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x00703A),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let xpedition = SchematicPalette(
        background: h(0x000000),
        gridMinor: h(0x2A2A2A),
        gridMajor: h(0x4A4A4A),
        sheetBorder: h(0xFFFF00),
        sheetText: h(0xA0A0A0),
        wire: h(0x00C800),
        junction: h(0x00C800),
        bus: h(0x00E0E0),
        netLabel: h(0xFFFFFF),
        localLabel: h(0xA8A8A8),
        portLabel: h(0xFFB84D),
        symbol: h(0xFFFF00),
        symbolFill: h(0x1A1A00),
        pin: h(0x00E0E0),
        pinName: h(0xFFFFFF),
        powerLabel: h(0xFF8FD0),
        pinNumber: h(0xA8A8A8),
        designator: h(0xFFFFFF),
        value: h(0xD0D0D0),
        variantValue: h(0xFFB84D),
        unconnectedPin: h(0xFF9F1A),
        noConnect: h(0x5CB8FF),
        selection: h(0xFF40FF),
        selectionHalo: h(0x3D7BF5),
        harness: h(0xC895FF),
        error: h(0xFF5A5F),
        preview: h(0x5CD6FF),
        directive: h(0x5CE68A),
        liveOn: h(0x5CE68A),
        overlayFill: h(0x10203F),
        overlayText: h(0xD0DCF0),
        overlayMuted: h(0x8090A8),
        overlayOnText: h(0x0A1830),
        probe: h(0x5CEBFF),
        readout: h(0x5CD6FF))

    /// Matrix Green: black screen, code-rain green (#00FF41) wires and symbols, pale glyph green for the text;
    /// amber / red keep warnings and errors distinct.
    static let matrix = SchematicPalette(
        background: h(0x020A04),
        gridMinor: h(0x0A2410),
        gridMajor: h(0x0F3A18),
        sheetBorder: h(0x00C832),
        sheetText: h(0x3FAF5C),
        wire: h(0x00FF41),
        junction: h(0x00FF41),
        bus: h(0x9CFFB4),
        netLabel: h(0xD2FFDC),
        localLabel: h(0x6FE08A),
        portLabel: h(0xB6FF3B),
        symbol: h(0x00D838),
        symbolFill: h(0x021A08),
        pin: h(0x2EE068),
        pinName: h(0xB8F5C6),
        powerLabel: h(0xE4FF6A),
        pinNumber: h(0x4FA866),
        designator: h(0xE0FFE6),
        value: h(0x8FE6A2),
        variantValue: h(0xFFD166),
        unconnectedPin: h(0xFF9F1A),
        noConnect: h(0x5CB8FF),
        selection: h(0xFFFFFF),
        selectionHalo: h(0x00FF41),
        harness: h(0x7DE8D0),
        error: h(0xFF5A5F),
        preview: h(0x9CFFB4),
        directive: h(0x5CE68A),
        liveOn: h(0x00FF41),
        overlayFill: h(0x041A0A),
        overlayText: h(0xD2FFDC),
        overlayMuted: h(0x6FAF7F),
        overlayOnText: h(0x021006),
        probe: h(0xB6FF3B),
        readout: h(0x9CFFB4))

    static let pads = SchematicPalette(
        background: h(0x000000),
        gridMinor: h(0x2A2A2A),
        gridMajor: h(0x484848),
        sheetBorder: h(0xD8D8D8),
        sheetText: h(0xA0A0A0),
        wire: h(0xE6E600),
        junction: h(0xE6E600),
        bus: h(0x00D200),
        netLabel: h(0xFFFFFF),
        localLabel: h(0xA8A8A8),
        portLabel: h(0xFFB84D),
        symbol: h(0xD8D8D8),
        symbolFill: h(0x141414),
        pin: h(0x00D200),
        pinName: h(0xFFFFFF),
        powerLabel: h(0xFF8FD0),
        pinNumber: h(0xA8A8A8),
        designator: h(0xFFFFFF),
        value: h(0xC0C0C0),
        variantValue: h(0xFFB84D),
        unconnectedPin: h(0xFF9F1A),
        noConnect: h(0x5CB8FF),
        selection: h(0xFF6060),
        selectionHalo: h(0x3D7BF5),
        harness: h(0xC895FF),
        error: h(0xFF5A5F),
        preview: h(0x5CD6FF),
        directive: h(0x5CE68A),
        liveOn: h(0x5CE68A),
        overlayFill: h(0x10203F),
        overlayText: h(0xD0DCF0),
        overlayMuted: h(0x8090A8),
        overlayOnText: h(0x0A1830),
        probe: h(0x5CEBFF),
        readout: h(0x5CD6FF))

    static let cr8000 = SchematicPalette(
        background: h(0x1A1C20),
        gridMinor: h(0x2E3238),
        gridMajor: h(0x4A4F58),
        sheetBorder: h(0xE0E4EA),
        sheetText: h(0xA0A0A0),
        wire: h(0x00D8E8),
        junction: h(0x00D8E8),
        bus: h(0x7FB2FF),
        netLabel: h(0xE8E8E8),
        localLabel: h(0xA8A8A8),
        portLabel: h(0xFFB84D),
        symbol: h(0xE0E4EA),
        symbolFill: h(0x262A31),
        pin: h(0xB0E0FF),
        pinName: h(0xE8E8E8),
        powerLabel: h(0xFF8FD0),
        pinNumber: h(0xA8A8A8),
        designator: h(0xFFFFFF),
        value: h(0xB8BEC8),
        variantValue: h(0xFFB84D),
        unconnectedPin: h(0xFF9F1A),
        noConnect: h(0x5CB8FF),
        selection: h(0xFFD23F),
        selectionHalo: h(0x3D7BF5),
        harness: h(0xC895FF),
        error: h(0xFF5A5F),
        preview: h(0x5CD6FF),
        directive: h(0x5CE68A),
        liveOn: h(0x5CE68A),
        overlayFill: h(0x10203F),
        overlayText: h(0xD0DCF0),
        overlayMuted: h(0x8090A8),
        overlayOnText: h(0x0A1830),
        probe: h(0x5CEBFF),
        readout: h(0x5CD6FF))

    static let kicad = SchematicPalette(
        background: h(0xF5F4EF),
        gridMinor: h(0xC8C8C8),
        gridMajor: h(0xA0A0A0),
        sheetBorder: h(0x840000),
        sheetText: h(0x5A5A5A),
        wire: h(0x009600),
        junction: h(0x009600),
        bus: h(0x0000C8),
        netLabel: h(0x0000C8),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x840000),
        symbolFill: h(0xFFFFC2),
        pin: h(0x840000),
        pinName: h(0x006464),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x006464),
        value: h(0x006464),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xC040C0),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x7A3D00),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let eagle = SchematicPalette(
        background: h(0xFFFFFF),
        gridMinor: h(0xE4E4E4),
        gridMajor: h(0xC8C8C8),
        sheetBorder: h(0x4A4A4A),
        sheetText: h(0x5A5A5A),
        wire: h(0x008000),
        junction: h(0x008000),
        bus: h(0x0000A0),
        netLabel: h(0x0000A0),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x4A4A4A),
        symbolFill: h(0xFFFFFF),
        pin: h(0x4A4A4A),
        pinName: h(0x800000),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x800000),
        value: h(0x800000),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xE05A00),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x7A3D00),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let proteus = SchematicPalette(
        background: h(0xFFFFF0),
        gridMinor: h(0xE2DFC8),
        gridMajor: h(0xC6C2A8),
        sheetBorder: h(0x00007F),
        sheetText: h(0x5A5A5A),
        wire: h(0x228B22),
        junction: h(0x228B22),
        bus: h(0x1F3F8F),
        netLabel: h(0x1F3F8F),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x00007F),
        symbolFill: h(0xFFFFFF),
        pin: h(0x800000),
        pinName: h(0x00007F),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x800000),
        value: h(0x800000),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xE000A0),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x7A3D00),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let easyeda = SchematicPalette(
        background: h(0xFFFFFF),
        gridMinor: h(0xE0E0E0),
        gridMajor: h(0xC0C0C0),
        sheetBorder: h(0xA00000),
        sheetText: h(0x5A5A5A),
        wire: h(0x000080),
        junction: h(0x000080),
        bus: h(0x000080),
        netLabel: h(0x0000CC),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0xA00000),
        symbolFill: h(0xFFFFF0),
        pin: h(0xA00000),
        pinName: h(0x000000),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x000080),
        value: h(0x000080),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0xE06000),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x00703A),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let diptrace = SchematicPalette(
        background: h(0xE8E8E8),
        gridMinor: h(0xE2E2DE),
        gridMajor: h(0xC4C4BE),
        sheetBorder: h(0x8B0000),
        sheetText: h(0x5A5A5A),
        wire: h(0x00008B),
        junction: h(0x00008B),
        bus: h(0x00008B),
        netLabel: h(0x1F3FBF),
        localLabel: h(0x5A5A5A),
        portLabel: h(0x9A5200),
        symbol: h(0x8B0000),
        symbolFill: h(0xFFFFF0),
        pin: h(0x8B0000),
        pinName: h(0x000000),
        powerLabel: h(0xA00060),
        pinNumber: h(0x505050),
        designator: h(0x000000),
        value: h(0x404040),
        variantValue: h(0x9A5200),
        unconnectedPin: h(0xC04A00),
        noConnect: h(0x0050C0),
        selection: h(0x007A7A),
        selectionHalo: h(0x66A3FF),
        harness: h(0x7B2FBE),
        error: h(0xC00000),
        preview: h(0x0062D6),
        directive: h(0x00703A),
        liveOn: h(0x00803A),
        overlayFill: h(0xF2F6FC),
        overlayText: h(0x34445E),
        overlayMuted: h(0x6B7A90),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x00558F),
        readout: h(0x00508F))

    static let monochrome = SchematicPalette(
        background: h(0xFFFFFF),
        gridMinor: h(0xE0E0E0),
        gridMajor: h(0xBDBDBD),
        sheetBorder: h(0x000000),
        sheetText: h(0x404040),
        wire: h(0x000000),
        junction: h(0x000000),
        bus: h(0x000000),
        netLabel: h(0x000000),
        localLabel: h(0x404040),
        portLabel: h(0x333333),
        symbol: h(0x000000),
        symbolFill: h(0xFFFFFF),
        pin: h(0x000000),
        pinName: h(0x000000),
        powerLabel: h(0x000000),
        pinNumber: h(0x333333),
        designator: h(0x000000),
        value: h(0x333333),
        variantValue: h(0x333333),
        unconnectedPin: h(0x555555),
        noConnect: h(0x333333),
        selection: h(0x0050E0),
        selectionHalo: h(0x66A3FF),
        harness: h(0x333333),
        error: h(0x000000),
        preview: h(0x0050E0),
        directive: h(0x333333),
        liveOn: h(0x333333),
        overlayFill: h(0xFFFFFF),
        overlayText: h(0x000000),
        overlayMuted: h(0x555555),
        overlayOnText: h(0xFFFFFF),
        probe: h(0x000000),
        readout: h(0x333333))

    static let highContrast = SchematicPalette(
        background: h(0x000000),
        gridMinor: h(0x333333),
        gridMajor: h(0x666666),
        sheetBorder: h(0xFFFF00),
        sheetText: h(0xC0C0C0),
        wire: h(0xFFFFFF),
        junction: h(0xFFFFFF),
        bus: h(0x00FFFF),
        netLabel: h(0x00FFFF),
        localLabel: h(0xC0C0C0),
        portLabel: h(0xFFA500),
        symbol: h(0xFFFF00),
        symbolFill: h(0x000000),
        pin: h(0xFFFF00),
        pinName: h(0xFFFFFF),
        powerLabel: h(0xFF8FD0),
        pinNumber: h(0xC0C0C0),
        designator: h(0xFFFFFF),
        value: h(0xFFFF00),
        variantValue: h(0xFFA500),
        unconnectedPin: h(0xFFA500),
        noConnect: h(0x00BFFF),
        selection: h(0xFF00FF),
        selectionHalo: h(0x3D7BF5),
        harness: h(0xD08CFF),
        error: h(0xFF3030),
        preview: h(0x00BFFF),
        directive: h(0x5CE68A),
        liveOn: h(0x5CE68A),
        overlayFill: h(0x000000),
        overlayText: h(0xFFFFFF),
        overlayMuted: h(0x8090A8),
        overlayOnText: h(0x0A1830),
        probe: h(0x00FFFF),
        readout: h(0xFFFFFF))
}
