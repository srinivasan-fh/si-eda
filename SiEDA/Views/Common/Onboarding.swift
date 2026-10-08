import AppKit
import SwiftUI

/// First-run tour (also Help → Welcome Tour): five pages from an empty project to the fabrication package.
struct WelcomeTourView: View {
    static let shownKey = "onboarding.tourShown"
    @Environment(\.dismiss) private var dismiss
    @State private var page = 0

    private let pages: [(icon: String, title: LocalizedStringKey, text: LocalizedStringKey)] = [
        ("sparkles", "Welcome to SiEDA",
         "From idea to fabrication files in one window: schematic, simulation, PCB layout, checks and manufacturing. Start from an example design or a blank project."),
        ("point.3.connected.trianglepath.dotted", "Draw the schematic",
         "Pick a part on the left (or press P), click to place it, then click two pins to wire them. ⌘K finds any command, part or net."),
        ("waveform.path.ecg", "Simulate and check",
         "Run DC, transient, AC, noise and Monte Carlo analyses; the status bar shows the time taken and Stop. The rule checks list every problem and show it on the canvas."),
        ("cpu", "Lay out the board",
         "Update PCB brings the parts over; Auto Place and Auto Route do the rest. Route by hand with push and shove where it matters."),
        ("shippingbox", "Order the board",
         "Export Fabrication Package (⇧⌘E) verifies the design and writes Gerbers, drills, IPC-2581, ODB++, the BOM, placement files and an optional production panel."),
    ]

    var body: some View {
        let p = pages[page]
        VStack(spacing: 14) {
            Image(systemName: p.icon).font(.system(size: 42)).foregroundStyle(Theme.skyBlue).frame(height: 52)
            Text(p.title).font(.title2.bold()).foregroundStyle(Theme.textPrimary)
            Text(p.text).multilineTextAlignment(.center).foregroundStyle(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 6) {
                ForEach(pages.indices, id: \.self) { i in
                    Circle().fill(i == page ? Theme.skyBlue : Theme.textMuted.opacity(0.5)).frame(width: 7, height: 7)
                }
            }
            HStack {
                Button("Skip", action: close)
                Spacer()
                if page > 0 { Button("Back") { page -= 1 } }
                Button(page == pages.count - 1 ? LocalizedStringKey("Get Started") : LocalizedStringKey("Next")) {
                    if page == pages.count - 1 { close() } else { page += 1 }
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 480)
        .background(Theme.deepBlue)
    }

    private func close() {
        UserDefaults.standard.set(true, forKey: Self.shownKey)
        dismiss()
    }
}

/// Help → Keyboard Shortcuts (⌘/): every menu command with a key, read from the menus (always current), and the
/// canvas keys.
struct KeyboardShortcutsView: View {
    @Environment(\.dismiss) private var dismiss

    private static let canvas: [(keys: String, action: LocalizedStringKey)] = [
        ("← ↑ → ↓", "Pan (⇧ for half a screen)"), ("+  −", "Zoom In"), ("Home  0", "Zoom to Fit"),
        ("⇧Z", "Zoom to Selection"), ("Z", "Zoom to Area"), ("N", "Show Navigator"), ("F11", "Focus mode"),
        ("V  H  W", "Select, pan and wire tools (schematic)"), ("P", "Place a part"),
    ]

    var body: some View {
        let entries = CommandPalette.menuEntries(NSApp.mainMenu).filter { !$0.shortcut.isEmpty }
        let menus = entries.reduce(into: [String]()) { if !$0.contains($1.menu) { $0.append($1.menu) } }
        VStack(spacing: 0) {
            HStack {
                Text("Keyboard Shortcuts").font(.title3.bold()).foregroundStyle(Theme.textPrimary)
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            .padding(14)
            List {
                Section("Canvas") {
                    ForEach(Self.canvas.indices, id: \.self) { i in
                        row(Text(verbatim: Self.canvas[i].keys), Text(Self.canvas[i].action))
                    }
                }
                ForEach(menus, id: \.self) { menu in
                    Section(menu) {
                        ForEach(entries.filter { $0.menu == menu }) { row(Text(verbatim: $0.shortcut), Text(verbatim: $0.title)) }
                    }
                }
            }
        }
        .frame(width: 520, height: 560)
        .background(Theme.deepBlue)
    }

    private func row(_ keys: Text, _ action: Text) -> some View {
        HStack {
            action.foregroundStyle(Theme.textPrimary)
            Spacer()
            keys.font(.body.monospaced()).foregroundStyle(Theme.skyBlue)
        }
    }
}
