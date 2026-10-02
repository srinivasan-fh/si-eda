import AppKit
import SwiftUI

enum SchematicTool: Equatable {
    case select
    case wire
    case noConnect
    case pan
    case place(ComponentKind)
    case placeCustom(String)  // custom part id from the component library

    var title: String {
        switch self {
        case .select: return "Select / Move"
        case .wire: return "Wire"
        case .noConnect: return "No Connect"
        case .pan: return "Hand (Pan)"
        case .place(let kind): return "Place \(kind.displayName)"
        case .placeCustom: return "Place Custom Part"
        }
    }
}

/// Schematic capture workspace: Photoshop-style tool strip, Proteus-style device picker and simulation
/// transport, Altium-style properties in the inspector.
struct SchematicEditorView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings
    @State private var tool: SchematicTool = .select
    @State private var pickerKind: ComponentKind = .resistor
    @State private var showPicker = true
    @State private var viewport = Viewport(scale: 1.6, offset: CGSize(width: 260, height: 260))
    @State private var canvasSize: CGSize = .zero
    @State private var wireStart: String?
    /// What P (and the "Place selected device" tool) arms: the device or custom part last chosen in the picker.
    @State private var placementTool: SchematicTool = .place(.resistor)

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move (V)", isActive: tool == .select) { tool = .select }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: tool == .pan) { tool = .pan }
                ToolStripButton(systemImage: "line.diagonal", help: "Wire (W) — click two pins", isActive: tool == .wire) { tool = .wire }
                ToolStripButton(systemImage: "xmark", help: "No connect (Q) — click a pin to mark it intentionally open",
                                isActive: tool == .noConnect) { tool = .noConnect }
                ToolStripDivider()
                ToolStripButton(systemImage: "plus.square.on.square", help: "Place selected device (P)",
                                isActive: tool == placementTool) {
                    tool = placementTool
                }
                ToolStripButton(systemImage: "arrow.down.to.line", help: "Place ground (G)", isActive: tool == .place(.ground)) {
                    tool = .place(.ground)
                }
                ToolStripButton(systemImage: "tag", help: "Place net label (L)", isActive: tool == .place(.netLabel)) {
                    tool = .place(.netLabel)
                }
                ToolStripButton(systemImage: "bolt.circle", help: "Place voltage source", isActive: tool == .place(.voltageSource)) {
                    tool = .place(.voltageSource)
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "rotate.right", help: "Rotate selection (Space or R)") { store.rotateSelection() }
                ToolStripButton(systemImage: "trash", help: "Delete selection (⌫)") { store.deleteSelection() }
                ToolStripDivider()
                ToolStripButton(systemImage: "sidebar.left", help: "Show/hide device picker", isActive: showPicker) {
                    showPicker.toggle()
                }
                ToolStripButton(systemImage: "map", help: "Navigator overview (N)", isActive: store.showNavigator) {
                    store.showNavigator.toggle()
                }
                ToolStripButton(systemImage: "plus.magnifyingglass", help: "Zoom to area (Z) — drag a rectangle") {
                    store.requestView(.zoomArea)
                }
            }

            if showPicker {
                DevicePicker(selected: $pickerKind, customParts: store.snapshot.customParts,
                             isPlacing: tool == .place(pickerKind),
                             onPickCustom: { arm(.placeCustom($0)) },
                             onPickStandard: { part in
                                 guard let id = store.addStandardPartToLibrary(part) else { return nil }
                                 arm(.placeCustom(id))
                                 return id
                             },
                             onImport: { store.workspace = .library }) { kind in
                    pickerKind = kind
                    arm(.place(kind))
                }
                .frame(width: 220)
            }

            VStack(spacing: 0) {
                OptionsBar(scrollsWhenNarrow: false) {
                    Image(systemName: "wrench.and.screwdriver").foregroundStyle(Theme.blue)
                    Text(tool.title).foregroundStyle(Theme.textPrimary).fontWeight(.semibold)
                    Divider().frame(height: 18)
                    Text(hint)
                        .foregroundStyle(wireStart == nil ? Theme.textMuted : Theme.skyBlue)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Toggle("Live probes", isOn: $store.showDCOverlay)
                        .toggleStyle(.switch)
                        .controlSize(.mini)
                    Spacer()
                    if !store.selection.isEmpty {
                        Text("\(store.selection.count) selected").foregroundStyle(Theme.skyBlue)
                    }
                    ZoomControls(level: viewport.scale / Viewport.schematicBaseScale,
                                 zoomIn: { store.requestView(.zoomIn) }, zoomOut: { store.requestView(.zoomOut) },
                                 fit: { store.requestView(.fit) }, fitSelection: { store.requestView(.fitSelection) },
                                 setLevel: { store.requestView(.setLevel($0)) })
                }

                ZStack(alignment: .bottomLeading) {
                    SchematicCanvas(tool: $tool, viewport: $viewport, canvasSize: $canvasSize, wireStart: $wireStart,
                                    placementTool: placementTool, live: store.live)
                    if store.showNavigator, !store.snapshot.components.isEmpty {
                        navigator
                            .canvasScrollShield()
                            .padding(12)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    }
                    SimulationTransport(live: store.live)
                        .canvasScrollShield()
                        .padding(12)
                    if store.snapshot.components.isEmpty {
                        VStack(spacing: 4) {
                            if settings.aiEnabled {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left and click the canvas to place it, or describe your product in Super Intelligence and let the agents design it.",
                                               actionTitle: "Open Super Intelligence") { store.workspace = .promptStudio }
                            } else {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left (or press P), click the canvas to place it, then click two pins to wire them. Or start from a reference design:")
                                    .allowsHitTesting(false)  // clicks fall through to the canvas
                            }
                            Menu {
                                ForEach(OfflineProvider.categories, id: \.self) { category in
                                    Section(category) {
                                        ForEach(OfflineProvider.examples(in: category), id: \.plan.title) { template in
                                            Button(template.plan.title) { store.loadExample(template.industryPlan) }
                                        }
                                    }
                                }
                            } label: {
                                Label("Load Example Design", systemImage: "square.grid.2x2")
                            }
                            .fixedSize()
                            .canvasScrollShield()
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
        }
        .background(Theme.navy)
        .background(DeleteKeyMonitor { store.deleteSelection() } isActive: {
            store.selectedWire != nil || !store.selection.isEmpty
        })
    }

    private func arm(_ placement: SchematicTool) {
        placementTool = placement
        tool = placement
    }

    /// Contextual instruction for the active tool (Photoshop-style options bar hint).
    private var hint: String {
        if let wireStart {
            return "Wiring from \(wireStart) — click a pin or any wire (T-junction) to connect"
                + (tool == .wire ? " · click empty space for a corner" : "") + " · Esc cancels"
        }
        switch tool {
        case .select: return "Click a pin to wire · drag a wire to bend it · drag parts or junctions to move · Space or R rotates · ⇧-drag box-selects · Space+drag, empty-space drag or middle/right-drag pans · scroll/pinch zooms · Home fits"
        case .wire: return "Start on a pin or any wire · click empty space for corners · end on a pin or a wire (T-junction joins it in parallel) · Esc cancels"
        case .noConnect: return "Click a pin to mark it intentionally unconnected (click again to clear) · ERC stops reporting it"
        case .pan: return "Drag, scroll or arrow keys pan · pinch, ⌘-scroll or +/− zoom · Z zoom to area · Home fits"
        case .place, .placeCustom: return "Click to place (repeats) · Space or R rotates before placing · Esc returns to Select"
        }
    }

    /// Overview of the whole schematic; click or drag to move the view.
    private var navigator: some View {
        let bounds = SchematicCanvas.componentBounds(store.snapshot)
        let extent = bounds.reduce(CGRect.null) { $0.union($1.rect) }.insetBy(dx: -40, dy: -40)
        return CanvasNavigator(extent: extent, items: bounds.map { $0.rect },
                               highlighted: bounds.filter { store.selection.contains($0.id) }.map { $0.rect },
                               viewport: viewport, canvasSize: canvasSize,
                               onCenter: { viewport.center(on: $0, in: canvasSize) },
                               onClose: { store.showNavigator = false })
    }
}

/// Proteus-style device picker: searchable list with a live symbol preview.
struct DevicePicker: View {
    @Binding var selected: ComponentKind
    var customParts: [CustomPartInfo] = []
    /// Whether the highlighted built-in device is armed for placement. When it isn't, the list shows no selection so
    /// clicking that same row arms it again (a List only reports clicks that change its selection).
    var isPlacing = true
    var onPickCustom: (String) -> Void = { _ in }
    /// Adds a built-in standard part to the project library; returns its part id.
    var onPickStandard: (StandardPart) -> String? = { _ in nil }
    var onImport: () -> Void = {}
    var onPick: (ComponentKind) -> Void
    @State private var search = ""
    @State private var selectedCustom: String?

    private var filtered: [ComponentKind] {
        let q = search.lowercased()
        return ComponentKind.builtIn.filter { q.isEmpty || $0.displayName.lowercased().contains(q) || $0.planName.contains(q) }
    }

    private var filteredCustom: [CustomPartInfo] {
        let q = search.lowercased()
        return customParts.filter { q.isEmpty || $0.name.lowercased().contains(q) || $0.description.lowercased().contains(q) }
    }

    /// Standard parts not yet in the project library (those already added show under Custom Parts).
    private var filteredStandard: [StandardPart] {
        let q = search.lowercased()
        let inLibrary = Set(customParts.map(\.name))
        return StandardLibrary.parts.filter { part in
            !inLibrary.contains(part.spec.name)
                && (q.isEmpty || part.spec.name.lowercased().contains(q) || part.spec.description.lowercased().contains(q)
                    || part.category.lowercased().contains(q))
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("DEVICES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                Text("P").font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
            }
            .padding(.horizontal, 10)
            .padding(.top, 10)
            TextField("Search devices", text: $search)
                .textFieldStyle(.blue)
                .padding(8)
            List(selection: Binding<ComponentKind?>(get: { selectedCustom == nil && isPlacing ? selected : nil },
                                                    set: { if let k = $0 { selectedCustom = nil; onPick(k) } })) {
                // Basic components first: power sources, passives, semiconductors, nets, switches/connectors.
                ForEach(ComponentKind.pickerCategories, id: \.self) { category in
                    let items = filtered.filter { $0.category == category }
                    if !items.isEmpty {
                        Section(category) {
                            ForEach(items) { kind in
                                Label(kind.displayName, systemImage: kind.systemImage)
                                    .foregroundStyle(Theme.textPrimary)
                                    .tag(kind)
                            }
                        }
                    }
                }
                Section("Custom Parts") {
                    ForEach(filteredCustom) { part in
                        HStack {
                            Image(systemName: "cpu.fill").foregroundStyle(Theme.skyBlue)
                            VStack(alignment: .leading, spacing: 1) {
                                Text(part.name).foregroundStyle(Theme.textPrimary)
                                Text("\(part.footprint) · \(part.pins.count) pins").font(.caption2).foregroundStyle(Theme.textMuted)
                            }
                            Spacer()
                        }
                        .contentShape(Rectangle())
                        .padding(.vertical, 1)
                        .background(RoundedRectangle(cornerRadius: 5).fill(selectedCustom == part.id ? Theme.blue.opacity(0.3) : .clear))
                        .onTapGesture {
                            selectedCustom = part.id
                            onPickCustom(part.id)
                        }
                    }
                    Button(action: onImport) {
                        Label("Import Datasheet…", systemImage: "doc.viewfinder")
                    }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.lightBlue)
                }
                if !filteredStandard.isEmpty {
                    Section("Standard Parts") {
                        ForEach(filteredStandard) { part in
                            HStack {
                                Image(systemName: "cpu").foregroundStyle(Theme.lightBlue)
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(part.spec.name).foregroundStyle(Theme.textPrimary)
                                    Text("\(part.category) · \(part.packageSummary)").font(.caption2).foregroundStyle(Theme.textMuted)
                                }
                                Spacer()
                            }
                            .contentShape(Rectangle())
                            .padding(.vertical, 1)
                            .help(part.spec.description)
                            .onTapGesture {
                                if let id = onPickStandard(part) { selectedCustom = id }
                            }
                        }
                    }
                }
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)

            if let id = selectedCustom, let part = customParts.first(where: { $0.id == id }) {
                VStack(alignment: .leading, spacing: 6) {
                    SymbolPreview(kind: .custom, value: part.name, custom: part, showPinLabels: true)
                        .frame(height: 120)
                        .frame(maxWidth: .infinity)
                        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy))
                    Text(part.name).font(.headline).foregroundStyle(Theme.textPrimary)
                    Text("\(part.manufacturer.isEmpty ? "" : part.manufacturer + " · ")\(part.footprint)")
                        .font(.caption).foregroundStyle(Theme.lightBlue)
                    Text(part.description).font(.caption).foregroundStyle(Theme.textMuted)
                }
                .padding(10)
            } else {
            VStack(alignment: .leading, spacing: 6) {
                SymbolPreview(kind: selected, value: selected.defaultValue)
                    .frame(height: 90)
                    .frame(maxWidth: .infinity)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy))
                Text(selected.displayName).font(.headline).foregroundStyle(Theme.textPrimary)
                Text("Pins: " + selected.pinNames.joined(separator: ", "))
                    .font(.caption.monospaced()).foregroundStyle(Theme.lightBlue)
                Text(selected.valueHint).font(.caption).foregroundStyle(Theme.textMuted)
            }
            .padding(10)
            }
        }
        .background(Theme.deepBlue.opacity(0.75))
        .overlay(Rectangle().frame(width: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .trailing)
    }
}

/// Proteus-style simulation transport (bottom-left of the schematic).
struct SimulationTransport: View {
    @EnvironmentObject private var store: DesignStore
    @ObservedObject var live: LiveSimulation

    var body: some View {
        HStack(spacing: 6) {
            if live.isRunning {
                // Live board: time, speed, pause / stop.
                Circle().fill(live.isPaused ? Theme.warning : Theme.liveOn).frame(width: 7, height: 7)
                Text("LIVE \(EngineeringFormat.string(live.state?.time ?? 0, unit: "s", digits: 3))")
                    .font(.caption.monospacedDigit().weight(.semibold))
                    .foregroundStyle(Theme.textPrimary)
                Button {
                    if live.isPaused { live.resume() } else { live.pause() }
                } label: {
                    Image(systemName: live.isPaused ? "play.fill" : "pause.fill")
                }
                .help(live.isPaused ? "Resume" : "Pause")
                .accessibilityLabel(live.isPaused ? "Resume live simulation" : "Pause live simulation")
                Button { live.stop() } label: { Image(systemName: "stop.fill") }
                    .help("Stop the live simulation")
                    .accessibilityLabel("Stop live simulation")
                Button { store.workspace = .simulation } label: { Image(systemName: "waveform.path.ecg.rectangle") }
                    .help("Scope, serial monitor and switches")
                    .accessibilityLabel("Open live instruments")
            } else {
                // ▶ is the real-time run (like powering the board); DC is a one-shot operating point.
                Button {
                    live.start(store: store)
                } label: {
                    Label("Run", systemImage: "play.fill")
                }
                .keyboardShortcut("r", modifiers: [.command])
                .help("Run the board in real time (⌘R): LEDs light, click switches, firmware runs, live probes")
                .accessibilityLabel("Run live simulation")
                Divider().frame(height: 14)
                Button {
                    Task { await store.simulateDC() }
                } label: {
                    Text("DC").font(.caption.weight(.semibold))
                }
                .help("Solve the DC operating point once — shows voltage probes")
                .accessibilityLabel("Run DC operating point")
                Button {
                    store.workspace = .simulation
                } label: {
                    Image(systemName: "waveform")
                }
                .help("Transient analysis")
                .accessibilityLabel("Open transient analysis")
                Button {
                    store.dcResult = nil
                } label: {
                    Image(systemName: "xmark.circle")
                }
                .help("Clear simulation results")
                .accessibilityLabel("Clear simulation results")
                .disabled(store.dcResult == nil)
                if let dc = store.dcResult {
                    Text(dc.converged ? "DC ✓" : "DC ✗")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(dc.converged ? Theme.skyBlue : Theme.error)
                }
            }
        }
        .buttonStyle(.borderless)
        .foregroundStyle(Theme.skyBlue)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
        .overlay(Capsule().strokeBorder(live.isRunning ? Theme.liveOn.opacity(0.8) : Theme.blue.opacity(0.5)))
        .alert("Live simulation", isPresented: Binding(get: { live.error != nil && !live.isRunning },
                                                        set: { if !$0 { live.clearError() } })) {
            Button("OK") { live.clearError() }
        } message: {
            Text(live.error ?? "")
        }
    }
}
