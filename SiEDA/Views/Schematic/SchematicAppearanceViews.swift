import AppKit
import SwiftUI

/// Small swatch images of schematic palettes and colours (menus show images, not shapes).
enum SchematicSwatch {
    /// A miniature sheet: background, a wire and a symbol body.
    static func image(_ p: SchematicPalette) -> NSImage {
        NSImage(size: NSSize(width: 30, height: 16), flipped: false) { rect in
            let sheet = NSBezierPath(roundedRect: rect.insetBy(dx: 0.5, dy: 0.5), xRadius: 3, yRadius: 3)
            p.background.nsColor.setFill()
            sheet.fill()
            NSColor.gray.setStroke()
            sheet.lineWidth = 1
            sheet.stroke()
            let wire = NSBezierPath()
            wire.move(to: NSPoint(x: 4, y: 8))
            wire.line(to: NSPoint(x: 16, y: 8))
            wire.lineWidth = 2
            p.wire.nsColor.setStroke()
            wire.stroke()
            let body = NSBezierPath(rect: NSRect(x: 16, y: 4, width: 9, height: 8))
            p.symbolFill.nsColor.setFill()
            body.fill()
            body.lineWidth = 1.4
            p.symbol.nsColor.setStroke()
            body.stroke()
            return true
        }
    }

    /// A square chip of one colour.
    static func chip(_ rgb: SchematicRGB) -> NSImage {
        NSImage(size: NSSize(width: 14, height: 14), flipped: false) { rect in
            let chip = NSBezierPath(roundedRect: rect.insetBy(dx: 0.5, dy: 0.5), xRadius: 3, yRadius: 3)
            rgb.nsColor.setFill()
            chip.fill()
            NSColor.gray.setStroke()
            chip.lineWidth = 1
            chip.stroke()
            return true
        }
    }
}

/// A scheme as a picker row: its swatch and its name.
struct SchematicSchemeLabel: View {
    let scheme: SchematicColorScheme
    let customJSON: String

    var body: some View {
        Label {
            Text(LocalizedStringKey(scheme.title))
        } icon: {
            Image(nsImage: SchematicSwatch.image(palette))
        }
    }

    private var palette: SchematicPalette {
        scheme.presetPalette ?? SchematicCustomTheme.load(customJSON).palette()
    }
}

/// Colour scheme and grid choices of the schematic canvas: the options-bar menu, the View menu and Settings.
struct SchematicAppearanceMenu: View {
    /// Opens the custom theme editor; nil hides "Customise…".
    var onCustomise: (() -> Void)?
    @AppStorage(SchematicColorScheme.storageKey) private var scheme = SchematicColorScheme.defaultScheme.rawValue
    @AppStorage(SchematicGridStyle.storageKey) private var grid = SchematicGridStyle.dots.rawValue
    @AppStorage(SchematicGridStyle.majorStorageKey) private var majorEvery = SchematicGridStyle.defaultMajorEvery
    @AppStorage(SchematicCustomTheme.storageKey) private var customJSON = ""

    var body: some View {
        schemePicker
        if let onCustomise {
            Button("Customise…", action: onCustomise)
        }
        Divider()
        Picker("Grid", selection: $grid) {
            ForEach(SchematicGridStyle.allCases) { Text($0.title).tag($0.rawValue) }
        }
        majorPicker
    }

    private var schemePicker: some View {
        Picker("Colour Scheme", selection: $scheme) {
            Section("Light") { rows(SchematicColorScheme.lightPresets) }
            Section("Dark") { rows(SchematicColorScheme.darkPresets) }
            Divider()
            rows([.custom])
        }
    }

    private func rows(_ schemes: [SchematicColorScheme]) -> some View {
        ForEach(schemes) { s in
            SchematicSchemeLabel(scheme: s, customJSON: customJSON).tag(s.rawValue)
        }
    }

    private var majorPicker: some View {
        Picker("Major Grid Line Every", selection: $majorEvery) {
            ForEach(SchematicGridStyle.majorChoices, id: \.self) { n in
                Text("Every \(n) lines").tag(n)
            }
        }
        .disabled(grid != SchematicGridStyle.lines.rawValue)
    }
}

/// Builds the Custom scheme: a seed preset and a named colour (or an exact one) per role, previewed live.
struct SchematicThemeEditor: View {
    @Environment(\.dismiss) private var dismiss
    @AppStorage(SchematicColorScheme.storageKey) private var scheme = SchematicColorScheme.defaultScheme.rawValue
    @AppStorage(SchematicCustomTheme.storageKey) private var customJSON = ""

    var body: some View {
        VStack(spacing: 0) {
            Form {
                Section {
                    seedPicker
                } header: {
                    Text("Custom Schematic Colours")
                }
                Section("Colours") {
                    ForEach(SchematicColorRole.allCases) { role in
                        SchematicRoleRow(role: role, theme: themeBinding)
                    }
                }
            }
            .formStyle(.grouped)
            Divider()
            buttons
        }
        .frame(minWidth: 380, idealWidth: 440, minHeight: 340, idealHeight: 560)
        .onAppear(perform: start)
    }

    private var theme: SchematicCustomTheme { SchematicCustomTheme.load(customJSON) }

    private var themeBinding: Binding<SchematicCustomTheme> {
        Binding(get: { SchematicCustomTheme.load(customJSON) }, set: { customJSON = $0.json })
    }

    private var seedBinding: Binding<String> {
        Binding(get: { theme.seedScheme.rawValue }, set: { raw in
            let seed = SchematicColorScheme(rawValue: raw) ?? .siedaDark
            customJSON = SchematicCustomTheme.seeded(from: seed).json
        })
    }

    private var seedPicker: some View {
        Picker("Start from", selection: seedBinding) {
            ForEach(SchematicColorScheme.presets) { s in
                SchematicSchemeLabel(scheme: s, customJSON: customJSON).tag(s.rawValue)
            }
        }
    }

    private var buttons: some View {
        HStack {
            Button("Reset") { customJSON = SchematicCustomTheme.seeded(from: theme.seedScheme).json }
            Spacer()
            Button("Done") { dismiss() }
                .keyboardShortcut(.defaultAction)
        }
        .padding(12)
    }

    /// The first time, the custom theme starts as the scheme in use; the canvas switches to Custom (live preview).
    private func start() {
        if customJSON.isEmpty {
            let current = SchematicColorScheme(rawValue: scheme) ?? .siedaDark
            customJSON = SchematicCustomTheme.seeded(from: current).json
        }
        scheme = SchematicColorScheme.custom.rawValue
    }
}

/// One role of the custom theme: a named colour (swatch + name), or Other with a colour well; warns on low contrast.
struct SchematicRoleRow: View {
    let role: SchematicColorRole
    @Binding var theme: SchematicCustomTheme

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Picker(selection: choice) { options } label: { roleLabel }
                if theme.choice(for: role) == SchematicCustomTheme.otherTag {
                    ColorPicker("Other…", selection: exact, supportsOpacity: false)
                        .labelsHidden()
                }
            }
            if lowContrast {
                Label("Contrast below 3:1 against the background", systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(Theme.warning)
            }
        }
    }

    private var palette: SchematicPalette { theme.palette() }
    private var current: SchematicRGB { role.value(in: palette) }

    private var lowContrast: Bool {
        role.needsContrast && current.contrast(with: palette.background) < 3
    }

    private var roleLabel: some View {
        Label {
            Text(LocalizedStringKey(role.title))
        } icon: {
            Image(nsImage: SchematicSwatch.chip(current))
        }
    }

    @ViewBuilder private var options: some View {
        ForEach(SchematicNamedColor.all) { named in
            namedRow(named).tag(named.id)
        }
        Divider()
        Text("Other…").tag(SchematicCustomTheme.otherTag)
    }

    private func namedRow(_ named: SchematicNamedColor) -> some View {
        Label {
            Text(LocalizedStringKey(named.name))
        } icon: {
            Image(nsImage: SchematicSwatch.chip(named.rgb))
        }
    }

    private var choice: Binding<String> {
        Binding(get: { theme.choice(for: role) }, set: { select($0) })
    }

    private func select(_ tag: String) {
        // Other keeps the colour shown now, as an exact value the colour well then edits.
        theme.roles[role.rawValue] = tag == SchematicCustomTheme.otherTag ? current.hexString : tag
    }

    private var exact: Binding<Color> {
        Binding(get: { current.color }, set: { colour in
            if let rgb = SchematicRGB(colour) { theme.roles[role.rawValue] = rgb.hexString }
        })
    }
}
