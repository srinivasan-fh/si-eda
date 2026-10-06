import Foundation
import SwiftUI

/// A colour the user can pick by name for a custom schematic theme. The id is stable (it is what is saved); the name
/// is an English key of the string tables.
struct SchematicNamedColor: Identifiable, Hashable {
    let id: String
    let name: String
    let rgb: SchematicRGB

    private init(_ id: String, _ name: String, _ hex: UInt32) {
        self.id = id
        self.name = name
        self.rgb = SchematicRGB(hex: hex)
    }

    /// The colour table: neutrals, blues, greens, yellows, reds, purples, browns.
    static let all: [SchematicNamedColor] = [
        .init("white", "White", 0xFFFFFF), .init("ivory", "Ivory", 0xFFFFF0), .init("cream", "Cream", 0xF5F4EF),
        .init("beige", "Beige", 0xF5F5DC), .init("lightGrey", "Light Grey", 0xD3D3D3), .init("silver", "Silver", 0xC0C0C0),
        .init("grey", "Grey", 0x808080), .init("darkGrey", "Dark Grey", 0x505050), .init("charcoal", "Charcoal", 0x2B2B2B),
        .init("black", "Black", 0x000000), .init("midnight", "Midnight Blue", 0x0A1224), .init("navy", "Navy", 0x000080),
        .init("darkBlue", "Dark Blue", 0x00008B), .init("royalBlue", "Royal Blue", 0x4169E1), .init("blue", "Blue", 0x0000FF),
        .init("skyBlue", "Sky Blue", 0x7DD4FC), .init("lightBlue", "Light Blue", 0xADD8E6), .init("cyan", "Cyan", 0x00FFFF),
        .init("teal", "Teal", 0x008080), .init("darkGreen", "Dark Green", 0x006400),
        .init("forestGreen", "Forest Green", 0x228B22), .init("green", "Green", 0x009600), .init("lime", "Lime", 0x00FF00),
        .init("olive", "Olive", 0x808000), .init("paleYellow", "Pale Yellow", 0xFFFFC2), .init("yellow", "Yellow", 0xFFFF00),
        .init("gold", "Gold", 0xFFD700), .init("amber", "Amber", 0xFFBF00), .init("orange", "Orange", 0xFFA500),
        .init("coral", "Coral", 0xFF7F50), .init("red", "Red", 0xFF0000), .init("darkRed", "Dark Red", 0x8B0000),
        .init("maroon", "Maroon", 0x800000), .init("crimson", "Crimson", 0xDC143C), .init("pink", "Pink", 0xFFC0CB),
        .init("magenta", "Magenta", 0xFF00FF), .init("purple", "Purple", 0x800080), .init("violet", "Violet", 0x8A2BE2),
        .init("lavender", "Lavender", 0xE6E6FA), .init("brown", "Brown", 0x8B4513), .init("tan", "Tan", 0xD2B48C),
    ]

    private static let byId: [String: SchematicNamedColor] = Dictionary(uniqueKeysWithValues: all.map { ($0.id, $0) })

    static func named(_ id: String) -> SchematicNamedColor? { byId[id] }

    /// The named colour closest to `rgb` (sRGB distance).
    static func nearest(to rgb: SchematicRGB) -> SchematicNamedColor {
        all.min { $0.rgb.distance(to: rgb) < $1.rgb.distance(to: rgb) } ?? all[0]
    }
}

/// A palette role the user customises. Each role sets one or more palette colours.
enum SchematicColorRole: String, CaseIterable, Identifiable {
    case background, grid, wire, junction, bus, netLabel, powerLabel, symbolOutline, symbolFill, pin, text
    case unconnectedPin, selection, harness, error

    var id: String { rawValue }

    /// English title (a key of the string tables).
    var title: String {
        switch self {
        case .background: return "Background"
        case .grid: return "Grid"
        case .wire: return "Wire"
        case .junction: return "Junction"
        case .bus: return "Bus"
        case .netLabel: return "Net label"
        case .powerLabel: return "Power label"
        case .symbolOutline: return "Symbol outline"
        case .symbolFill: return "Symbol fill"
        case .pin: return "Pin"
        case .text: return "Text"
        case .unconnectedPin: return "Unconnected pin"
        case .selection: return "Selection"
        case .harness: return "Harness"
        case .error: return "Error marker"
        }
    }

    /// Drawn on the background, so it should reach 3:1 against it (the background, grid and fill are not checked).
    var needsContrast: Bool { ![.background, .grid, .symbolFill].contains(self) }

    func value(in p: SchematicPalette) -> SchematicRGB {
        switch self {
        case .background: return p.background
        case .grid: return p.gridMinor
        case .wire: return p.wire
        case .junction: return p.junction
        case .bus: return p.bus
        case .netLabel: return p.netLabel
        case .powerLabel: return p.powerLabel
        case .symbolOutline: return p.symbol
        case .symbolFill: return p.symbolFill
        case .pin: return p.pin
        case .text: return p.designator
        case .unconnectedPin: return p.unconnectedPin
        case .selection: return p.selection
        case .harness: return p.harness
        case .error: return p.error
        }
    }

    func apply(_ c: SchematicRGB, to p: inout SchematicPalette) {
        switch self {
        case .background: p.background = c
        case .grid: p.gridMinor = c; p.gridMajor = c
        case .wire: p.wire = c
        case .junction: p.junction = c
        case .bus: p.bus = c
        case .netLabel: p.netLabel = c
        case .powerLabel: p.powerLabel = c
        case .symbolOutline: p.symbol = c; p.sheetBorder = c
        case .symbolFill: p.symbolFill = c
        case .pin: p.pin = c
        case .text: p.designator = c; p.value = c; p.pinName = c
        case .unconnectedPin: p.unconnectedPin = c
        case .selection: p.selection = c
        case .harness: p.harness = c
        case .error: p.error = c
        }
    }
}

/// The user's own schematic theme: a seed preset plus a colour per role, by name (stable id) or exact ("#RRGGBB",
/// shown as Other; "#RRGGBBAA" keeps a translucent fill). Saved as JSON in the app preferences.
struct SchematicCustomTheme: Codable, Equatable {
    static let storageKey = "schematic.customTheme"
    /// Value of the Picker's "Other…" entry.
    static let otherTag = "other"

    var seed: String
    var roles: [String: String]

    var seedScheme: SchematicColorScheme {
        let scheme = SchematicColorScheme(rawValue: seed) ?? .siedaDark
        return scheme.presetPalette == nil ? .siedaDark : scheme
    }

    var seedPalette: SchematicPalette { seedScheme.presetPalette ?? .siedaDark }

    /// A theme that draws exactly as `scheme`: each role takes the named colour when one matches, otherwise Other.
    static func seeded(from scheme: SchematicColorScheme) -> SchematicCustomTheme {
        let palette = scheme.presetPalette ?? .siedaDark
        var roles: [String: String] = [:]
        for role in SchematicColorRole.allCases {
            roles[role.rawValue] = storage(for: role.value(in: palette))
        }
        return SchematicCustomTheme(seed: scheme.presetPalette == nil ? SchematicColorScheme.siedaDark.rawValue : scheme.rawValue,
                                    roles: roles)
    }

    /// Stored form of a colour: its name's id when it is (nearly) a named colour, otherwise "#RRGGBB(AA)".
    static func storage(for rgb: SchematicRGB) -> String {
        let named = SchematicNamedColor.nearest(to: rgb)
        return rgb.a >= 1 && named.rgb.distance(to: rgb) < 0.02 ? named.id : rgb.hexString
    }

    /// The colour stored for `role`; nil when missing or unreadable (the seed's colour applies).
    func colour(for role: SchematicColorRole) -> SchematicRGB? {
        guard let stored = roles[role.rawValue] else { return nil }
        if let named = SchematicNamedColor.named(stored) { return named.rgb }
        return SchematicRGB(hexString: stored)
    }

    /// The named colour id for `role`, or `otherTag` for an exact colour.
    func choice(for role: SchematicColorRole) -> String {
        guard let stored = roles[role.rawValue], SchematicNamedColor.named(stored) != nil else { return Self.otherTag }
        return stored
    }

    /// The palette: the seed preset with every changed role applied. Unknown ids keep the seed's colour, and so does
    /// a role still (nearly) at the seed's colour, so a freshly seeded theme draws exactly as its preset.
    func palette() -> SchematicPalette {
        let seed = seedPalette
        var p = seed
        for role in SchematicColorRole.allCases {
            guard let c = colour(for: role), c.distance(to: role.value(in: seed)) >= Self.sameColour else { continue }
            role.apply(c, to: &p)
        }
        return p
    }

    /// Colours closer than this (sRGB distance) count as the same (named colours and hex values round slightly).
    static let sameColour = 0.025

    /// Roles whose colour is below 3:1 against the background.
    func lowContrastRoles() -> [SchematicColorRole] {
        let p = palette()
        return SchematicColorRole.allCases.filter { $0.needsContrast && $0.value(in: p).contrast(with: p.background) < 3 }
    }

    var json: String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = .sortedKeys
        guard let data = try? encoder.encode(self) else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    /// The stored theme; an empty or unreadable value gives SiEDA Dark.
    static func load(_ json: String) -> SchematicCustomTheme {
        guard let data = json.data(using: .utf8),
              let theme = try? JSONDecoder().decode(SchematicCustomTheme.self, from: data) else { return seeded(from: .siedaDark) }
        return theme
    }
}
