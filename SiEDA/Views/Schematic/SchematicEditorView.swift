import AppKit
import SwiftUI

enum SchematicTool: Equatable {
    case select
    case wire
    case pan
    case place(ComponentKind)

    var title: String {
        switch self {
        case .select: return "Select / Move"
        case .wire: return "Wire"
        case .pan: return "Hand (Pan)"
        case .place(let kind): return "Place \(kind.displayName)"
        }
    }
}

/// Schematic capture workspace: Photoshop-style tool strip, Proteus-style device picker and simulation
/// transport, Altium-style properties in the inspector.
struct SchematicEditorView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var tool: SchematicTool = .select
    @State private var pickerKind: ComponentKind = .resistor
    @State private var showPicker = true
    @State private var viewport = Viewport(scale: 1.6, offset: CGSize(width: 260, height: 260))
    @State private var canvasSize: CGSize = .zero
    @State private var fitRequest = 0

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move (V)", isActive: tool == .select) { tool = .select }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: tool == .pan) { tool = .pan }
                ToolStripButton(systemImage: "line.diagonal", help: "Wire (W) — click two pins", isActive: tool == .wire) { tool = .wire }
                ToolStripDivider()
                ToolStripButton(systemImage: "plus.square.on.square", help: "Place selected device (P)",
                                isActive: { if case .place = tool { return true } else { return false } }()) {
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
                ToolStripButton(systemImage: "rotate.right", help: "Rotate selection (R)") { store.rotateSelection() }
                ToolStripButton(systemImage: "trash", help: "Delete selection (⌫)") { store.deleteSelection() }
                ToolStripDivider()
                ToolStripButton(systemImage: "sidebar.left", help: "Show/hide device picker", isActive: showPicker) {
                    showPicker.toggle()
                }
            }

            if showPicker {
                DevicePicker(selected: $pickerKind) { kind in
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
                    Text("Grid 10").foregroundStyle(Theme.textMuted)
                    Toggle("Live probes", isOn: $store.showDCOverlay)
                        .toggleStyle(.switch)
                        .controlSize(.mini)
                    Spacer()
                    if !store.selection.isEmpty {
                        Text("\(store.selection.count) selected").foregroundStyle(Theme.skyBlue)
                    }
                    ZoomControls(scale: viewport.scale / 1.6,
                                 zoomIn: { zoom(1.25) }, zoomOut: { zoom(0.8) }, fit: { fitRequest += 1 })
                }

                ZStack(alignment: .bottomLeading) {
                    SchematicCanvas(tool: $tool, viewport: $viewport, canvasSize: $canvasSize, fitRequest: fitRequest)
                    SimulationTransport()
                        .padding(12)
                    if store.snapshot.components.isEmpty {
                        BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                       title: "Empty schematic",
                                       message: "Pick a device on the left and click the canvas to place it, or describe your product in the AI Prompt Studio and let the agents design it.",
                                       actionTitle: "Open AI Prompt Studio") { store.workspace = .promptStudio }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
            }
        }
        .background(Theme.navy)
    }

    private func zoom(_ factor: CGFloat) {
        viewport.zoom(by: factor, anchor: CGPoint(x: canvasSize.width / 2, y: canvasSize.height / 2), limits: 0.2...12)
    }
}

/// Proteus-style device picker: searchable list with a live symbol preview.
struct DevicePicker: View {
    @Binding var selected: ComponentKind
    var onPick: (ComponentKind) -> Void
    @State private var search = ""

    private var filtered: [ComponentKind] {
        let q = search.lowercased()
        return ComponentKind.allCases.filter { q.isEmpty || $0.displayName.lowercased().contains(q) || $0.planName.contains(q) }
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
            List(selection: Binding(get: { selected }, set: { if let k = $0 { onPick(k) } })) {
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
            Button {
                store.workspace = .simulation
            } label: {
                Image(systemName: "waveform")
            }
            .help("Transient analysis")
            Button {
                store.dcResult = nil
            } label: {
                Image(systemName: "stop.fill")
            }
            .help("Clear simulation results")
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
