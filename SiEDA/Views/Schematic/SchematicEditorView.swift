import AppKit
import SwiftUI

enum SchematicTool: Equatable {
    case select
    case wire
    case pan
    case place(ComponentKind)
    case placeCustom(String)  // custom part id from the component library

    var title: String {
        switch self {
        case .select: return "Select / Move"
        case .wire: return "Wire"
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

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move (V)", isActive: tool == .select) { tool = .select }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: tool == .pan) { tool = .pan }
                ToolStripButton(systemImage: "line.diagonal", help: "Wire (W) — click two pins", isActive: tool == .wire) { tool = .wire }
                ToolStripDivider()
                ToolStripButton(systemImage: "plus.square.on.square", help: "Place selected device (P)",
                                isActive: {
                                    switch tool {
                                    case .place, .placeCustom: return true
                                    default: return false
                                    }
                                }()) {
                    tool = .place(pickerKind)
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
                             onPickCustom: { tool = .placeCustom($0) },
                             onPickStandard: { part in
                                 guard let id = store.addStandardPartToLibrary(part) else { return nil }
                                 tool = .placeCustom(id)
                                 return id
                             },
                             onImport: { store.workspace = .library }) { kind in
                    pickerKind = kind
                    tool = .place(kind)
                }
                .frame(width: 220)
            }

            VStack(spacing: 0) {
                OptionsBar {
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
                    SchematicCanvas(tool: $tool, viewport: $viewport, canvasSize: $canvasSize, wireStart: $wireStart)
                    if store.showNavigator, !store.snapshot.components.isEmpty {
                        navigator
                            .padding(12)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    }
                    SimulationTransport()
                        .padding(12)
                    if store.snapshot.components.isEmpty {
                        VStack(spacing: 4) {
                            if settings.aiEnabled {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left and click the canvas to place it, or describe your product in the AI Prompt Studio and let the agents design it.",
                                               actionTitle: "Open AI Prompt Studio") { store.workspace = .promptStudio }
                            } else {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left (or press P), click the canvas to place it, then click two pins to wire them. Or start from a reference design:")
                            }
                            Menu {
                                ForEach(OfflineProvider.categories, id: \.self) { category in
                                    Section(category) {
                                        ForEach(OfflineProvider.examples(in: category), id: \.plan.title) { template in
                                            Button(template.plan.title) { store.loadExample(template.plan) }
                                        }
                                    }
                                }
                            } label: {
                                Label("Load Example Design", systemImage: "square.grid.2x2")
                            }
                            .fixedSize()
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
        }
        .background(Theme.navy)
    }

    /// Contextual instruction for the active tool (Photoshop-style options bar hint).
    private var hint: String {
        if let wireStart { return "Wiring from \(wireStart) — click another pin to connect · Esc cancels" }
        switch tool {
        case .select: return "Click a pin to wire · drag parts to move · Space or R rotates · ⇧-drag box-selects · drag empty space or middle/right-drag pans · scroll/pinch zooms · Home fits"
        case .wire: return "Click a pin, then a second pin to connect them"
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
                .textFieldStyle(.roundedBorder)
                .padding(8)
            List(selection: Binding<ComponentKind?>(get: { selectedCustom == nil ? selected : nil },
                                                    set: { if let k = $0 { selectedCustom = nil; onPick(k) } })) {
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
                ForEach(["Passives", "Semiconductors", "Power & Nets", "Electromechanical"], id: \.self) { category in
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

    var body: some View {
        HStack(spacing: 6) {
            Button {
                Task { await store.simulateDC() }
            } label: {
                Image(systemName: "play.fill")
            }
            .help("Run DC operating point — shows live voltage probes")
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
                Image(systemName: "stop.fill")
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
        .buttonStyle(.borderless)
        .foregroundStyle(Theme.skyBlue)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
        .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.5)))
    }
}
