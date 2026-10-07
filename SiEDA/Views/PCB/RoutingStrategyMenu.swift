import SwiftUI

/// The autorouter's strategy menu next to Auto Route (docs/ROUTING.md, Strategies): preset strategies, the quality
/// options, one-shot scoped routes (selected nets, a net class, the selected parts' area), the per-class layer
/// settings and the routing report. A compact menu so the PCB options bar stays within the minimum window width.
struct RoutingStrategyMenu: View {
    @EnvironmentObject var store: DesignStore
    @State private var showSettings = false
    @State private var showReport = false

    var body: some View {
        Menu {
            presetSection
            optionSection
            scopeSection
            Divider()
            Button("Strategy Settings…") { showSettings = true }
            Button("Routing Report…") { showReport = true }
        } label: {
            Label("Routing Strategy", systemImage: "slider.vertical.3")
        }
        .menuStyle(.borderlessButton)
        .fixedSize()
        .disabled(store.isBusy)
        .help("Autorouter strategy: presets, quality passes, routing part of the board and the routing report")
        .sheet(isPresented: $showSettings) { RoutingStrategySheet().environmentObject(store) }
        .sheet(isPresented: $showReport) { RoutingReportSheet(report: store.routeReport) }
    }

    @ViewBuilder private var presetSection: some View {
        let current = store.autorouteOptions.preset
        Section("Strategy") {
            ForEach(EDAEngine.autoroutePresets().filter { !["nets", "netclass", "area"].contains($0.name) }) { preset in
                presetButton(preset, current: current)
            }
        }
    }

    private func presetButton(_ preset: AutoroutePreset, current: String) -> some View {
        Button { store.applyRoutingPreset(preset.name) } label: {
            if preset.name == current {
                Label(LocalizedStringKey(preset.title), systemImage: "checkmark")
            } else {
                Text(LocalizedStringKey(preset.title))
            }
        }
        .help(LocalizedStringKey(preset.description))
    }

    @ViewBuilder private var optionSection: some View {
        let o = store.autorouteOptions
        Section("Options") {
            optionToggle("Coupled differential pairs", o.coupledPairs, \.coupledPairs)
            optionToggle("Length-aware routing", o.lengthAware, \.lengthAware)
            optionToggle("Minimize vias", o.minimizeVias, \.minimizeVias)
            optionToggle("Gloss tracks", o.gloss, \.gloss)
            optionToggle("Arc corners", o.arcCorners, \.arcCorners)
            optionToggle("Teardrops", o.teardrops, \.teardrops)
            optionToggle("Protect locked copper", o.protectLocked, \.protectLocked)
            optionToggle("Swap Pins & Gates", o.pinSwap, \.pinSwap)
        }
    }

    private func optionToggle(_ title: LocalizedStringKey, _ on: Bool,
                              _ keyPath: WritableKeyPath<AutorouteOptions, Bool>) -> some View {
        Button { store.toggleRoutingOption(keyPath) } label: {
            if on { Label(title, systemImage: "checkmark") } else { Text(title) }
        }
    }

    @ViewBuilder private var scopeSection: some View {
        Section("Route Part of the Board") {
            Button("Route Selected Nets") { Task { await store.routeSelectedNets() } }
                .disabled(store.selectedRoutingNets.isEmpty)
            Menu("Route Net Class") { netClassItems }
                .disabled(store.snapshot.netClassDefs.isEmpty)
            Button("Route Selected Parts' Area") { routeSelectionArea() }
                .disabled(store.selectionRoutingArea == nil)
        }
    }

    @ViewBuilder private var netClassItems: some View {
        ForEach(store.snapshot.netClassDefs) { cls in
            Button(cls.name) { Task { await store.routeNetClass(cls.name) } }
        }
    }

    private func routeSelectionArea() {
        guard let area = store.selectionRoutingArea else { return }
        Task { await store.routeArea(area) }
    }
}
