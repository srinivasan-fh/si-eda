import AppKit
import SwiftUI

/// One row of the ⌘K command palette.
struct CommandPaletteEntry: Identifiable {
    enum Kind { case command, part, net }
    let id: String
    let kind: Kind
    let title: String
    let detail: String
    let run: () -> Void

    var icon: String {
        switch kind {
        case .command: return "command"
        case .part: return "cpu"
        case .net: return "point.3.connected.trianglepath.dotted"
        }
    }
}

/// ⌘K: every menu command (read from the main menu, so new commands appear by themselves), every part by designator
/// or value and every net. Type to filter, ↑ ↓ to move, Return to run, Esc to close.
enum CommandPalette {
    /// Enabled leaf items of `menu` as "Menu › Submenu" entries; the Services menu is left out.
    @MainActor static func menuEntries(_ menu: NSMenu?, path: [String] = []) -> [CommandPaletteEntry] {
        guard let menu else { return [] }
        menu.update()
        var out: [CommandPaletteEntry] = []
        for item in menu.items where !item.isSeparatorItem && !item.isHidden {
            let name = item.title.isEmpty ? (item.submenu?.title ?? "") : item.title
            guard !name.isEmpty else { continue }
            if let sub = item.submenu {
                if sub !== NSApp.servicesMenu { out += menuEntries(sub, path: path + [name]) }
            } else if item.isEnabled {
                let key = item.keyEquivalent.isEmpty ? "" : "  " + shortcut(item)
                out.append(CommandPaletteEntry(id: "menu:" + (path + [name]).joined(separator: "›"), kind: .command,
                                               title: name, detail: path.joined(separator: " › ") + key) { [weak item] in
                    guard let item, let owner = item.menu, let index = owner.items.firstIndex(of: item) else { return }
                    owner.performActionForItem(at: index)
                })
            }
        }
        return out
    }

    /// Parts (not labels, grounds or junctions) and nets with at least two pins, from the snapshot.
    @MainActor static func designEntries(_ store: DesignStore) -> [CommandPaletteEntry] {
        let parts = store.snapshot.components.filter { !$0.componentKind.isVirtual && !$0.ref.isEmpty }.map { c in
            CommandPaletteEntry(id: "part:\(c.id)", kind: .part, title: c.ref, detail: c.value) { [weak store] in
                guard let store else { return }
                if !store.workspace.hasCanvas { store.workspace = .schematic }
                store.crossProbe(component: c.id, sheet: c.sheetId)
            }
        }
        let nets = store.snapshot.nets.filter { $0.pinCount > 1 }.map { n in
            CommandPaletteEntry(id: "net:\(n.index)", kind: .net, title: n.name, detail: "\(n.pinCount) pins") { [weak store] in
                guard let store, let place = store.netPlaces(n.index)?.places.first else { return }
                if !store.workspace.hasCanvas { store.workspace = .schematic }
                store.crossProbe(component: place.component, sheet: place.sheet)
            }
        }
        return parts + nets
    }

    /// Entries whose title or detail holds every word of `query`, best first: title prefix, then title, then detail.
    static func filter(_ entries: [CommandPaletteEntry], query: String, limit: Int = 60) -> [CommandPaletteEntry] {
        let words = query.lowercased().split(separator: " ").map(String.init)
        guard !words.isEmpty else { return Array(entries.prefix(limit)) }
        var scored: [(Int, Int, CommandPaletteEntry)] = []
        for (order, e) in entries.enumerated() {
            let title = e.title.lowercased(), all = title + " " + e.detail.lowercased()
            guard words.allSatisfy({ all.contains($0) }) else { continue }
            let score = title.hasPrefix(words[0]) ? 0 : title.contains(words[0]) ? 1 : 2
            scored.append((score, order, e))
        }
        return scored.sorted { ($0.0, $0.1) < ($1.0, $1.1) }.prefix(limit).map(\.2)
    }

    static func shortcut(_ item: NSMenuItem) -> String {
        let m = item.keyEquivalentModifierMask
        var s = ""
        if m.contains(.control) { s += "⌃" }
        if m.contains(.option) { s += "⌥" }
        if m.contains(.shift) || item.keyEquivalent != item.keyEquivalent.lowercased() { s += "⇧" }
        if m.contains(.command) { s += "⌘" }
        return s + item.keyEquivalent.uppercased()
    }
}

/// The palette panel, shown over the window (not a sheet, so the menu commands it lists stay enabled).
struct CommandPaletteView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var query = ""
    @State private var entries: [CommandPaletteEntry] = []
    @State private var highlighted = 0
    @FocusState private var focused: Bool

    private var results: [CommandPaletteEntry] { CommandPalette.filter(entries, query: query) }

    var body: some View {
        ZStack(alignment: .top) {
            Color.black.opacity(0.35).ignoresSafeArea().onTapGesture(perform: close)
            VStack(spacing: 0) {
                HStack {
                    Image(systemName: "magnifyingglass").foregroundStyle(Theme.textMuted)
                    TextField("Search commands, parts and nets", text: $query)
                        .textFieldStyle(.plain)
                        .font(.title3)
                        .foregroundStyle(Theme.textPrimary)
                        .focused($focused)
                        .onSubmit { run(highlighted) }
                        .onKeyPress(.downArrow) { highlighted = min(highlighted + 1, max(results.count - 1, 0)); return .handled }
                        .onKeyPress(.upArrow) { highlighted = max(highlighted - 1, 0); return .handled }
                        .onExitCommand(perform: close)
                }
                .padding(12)
                Divider()
                ScrollViewReader { proxy in
                    ScrollView {
                        LazyVStack(spacing: 0) {
                            let rows = results
                            ForEach(Array(rows.enumerated()), id: \.element.id) { index, entry in
                                row(entry, selected: index == highlighted)
                                    .id(index)
                                    .onTapGesture { run(index) }
                            }
                            if rows.isEmpty {
                                Text("No matches.").foregroundStyle(Theme.textMuted).padding(16)
                            }
                        }
                    }
                    .frame(maxHeight: 360)
                    .onChange(of: highlighted) { _, i in proxy.scrollTo(i) }
                }
            }
            .frame(width: 560)
            .background(RoundedRectangle(cornerRadius: 12, style: .continuous).fill(Theme.deepBlue))
            .overlay(RoundedRectangle(cornerRadius: 12, style: .continuous).strokeBorder(Theme.blue.opacity(0.5)))
            .shadow(radius: 20)
            .padding(.top, 80)
        }
        .onAppear {
            entries = CommandPalette.menuEntries(NSApp.mainMenu) + CommandPalette.designEntries(store)
            focused = true
        }
        .onChange(of: query) { _, _ in highlighted = 0 }
    }

    private func row(_ entry: CommandPaletteEntry, selected: Bool) -> some View {
        HStack(spacing: 10) {
            Image(systemName: entry.icon).frame(width: 18).foregroundStyle(Theme.skyBlue)
            Text(verbatim: entry.title).foregroundStyle(Theme.textPrimary)
            Spacer()
            Text(verbatim: entry.detail).font(.caption).foregroundStyle(Theme.textMuted).lineLimit(1)
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(selected ? Theme.blue.opacity(0.35) : .clear)
        .contentShape(Rectangle())
    }

    private func run(_ index: Int) {
        let rows = results
        guard rows.indices.contains(index) else { return }
        close()
        // After the panel is gone, so a command that opens a sheet or panel attaches to the window.
        DispatchQueue.main.async(execute: rows[index].run)
    }

    private func close() { store.showCommandPalette = false }
}
