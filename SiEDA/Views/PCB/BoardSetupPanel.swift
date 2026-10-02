import SwiftUI

/// Board setup popover: mechanical outline, mounting holes, copper pours/planes and net classes.
struct BoardSetupPanel: View {
    @EnvironmentObject private var store: DesignStore

    @State private var preset: BoardOutlinePreset = .rectangle
    @State private var outlineWidth = "50"
    @State private var outlineHeight = "40"
    @State private var outlineParameter = "3"
    @State private var holePattern: MountingPattern = .m3FlightController
    @State private var pourNet = ""
    @State private var pourLayer = 0
    @State private var pourPlane = false
    @State private var classNet = ""
    @State private var classWidth = "0.6"

    var body: some View {
        let board = store.snapshot.board
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                section("Board Outline", systemImage: "square.dashed") {
                    Picker("Shape", selection: $preset) {
                        ForEach(BoardOutlinePreset.allCases) { Label($0.title, systemImage: $0.systemImage).tag($0) }
                    }
                    HStack(spacing: 8) {
                        field(preset == .quadX ? "Span" : (preset == .circle ? "Diameter" : "Width"), $outlineWidth)
                        if preset != .circle { field(preset == .quadX ? "Body" : "Height", $outlineHeight) }
                        if preset == .rounded || preset == .quadX {
                            field(preset == .quadX ? "Arm" : "Radius", $outlineParameter)
                        }
                    }
                    HStack {
                        Text(board.hasCustomOutline ? "Custom outline · \(board.outline.count) points" :
                                String(format: "Rectangle %.1f × %.1f mm", board.width, board.height))
                            .font(.caption).foregroundStyle(Theme.textMuted)
                        Spacer()
                        Button("Apply Outline") {
                            store.applyOutlinePreset(preset, width: value(outlineWidth, 50), height: value(outlineHeight, 40),
                                                     parameter: value(outlineParameter, 3))
                        }
                    }
                    Text("Placement, routing, pours, DRC, Gerber Edge.Cuts and the 3D board follow the outline. "
                         + "Quad-X: motor connectors go at the arm tips.")
                        .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
                }

                section("Mounting Holes", systemImage: "circle.circle") {
                    Picker("Pattern", selection: $holePattern) {
                        ForEach(MountingPattern.allCases) { Text($0.title).tag($0) }
                    }
                    HStack {
                        Text("\(board.holes.count) hole\(board.holes.count == 1 ? "" : "s") · NPTH drill file exported")
                            .font(.caption).foregroundStyle(Theme.textMuted)
                        Spacer()
                        Button("Clear") { store.clearMountingHoles() }.disabled(board.holes.isEmpty)
                        Button("Add Pattern") {
                            store.addMountingPattern(spacing: holePattern.spacing, drill: holePattern.drill,
                                                     keepout: holePattern.keepout)
                        }
                    }
                }

                section("Copper Pours & Planes", systemImage: "square.fill.on.square.fill") {
                    if store.snapshot.zones.isEmpty {
                        Text("No pours. Ground pours shorten return paths, carry high currents and leave the "
                             + "autorouter only the signals.")
                            .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
                    }
                    ForEach(Array(store.snapshot.zones.enumerated()), id: \.offset) { index, zone in
                        HStack {
                            Image(systemName: zone.plane ? "square.stack.3d.down.forward.fill" : "square.fill")
                                .foregroundStyle(Theme.copperColor(zone.layer, layerCount: board.layerCount))
                            Text("\(zone.net) — \(board.layerName(zone.layer))\(zone.plane ? " plane" : " pour")")
                                .foregroundStyle(Theme.textPrimary)
                            if let fill = store.snapshot.zoneFills.first(where: { $0.zone == index }) {
                                Text(String(format: "%.0f mm²", fill.area)).font(.caption).foregroundStyle(Theme.textMuted)
                            }
                            Spacer()
                            Button { store.removeZone(at: index) } label: { Image(systemName: "trash") }
                                .buttonStyle(.borderless)
                                .accessibilityLabel("Remove \(zone.net) pour")
                        }
                    }
                    HStack(spacing: 8) {
                        Picker("Net", selection: $pourNet) {
                            if !netChoices.contains(pourNet) { Text("Choose a net").tag(pourNet) }
                            ForEach(netChoices, id: \.self) { Text($0).tag($0) }
                        }
                        .frame(width: 150)
                        Picker("Layer", selection: $pourLayer) {
                            ForEach(0..<max(1, board.layerCount), id: \.self) { Text(board.layerName($0)).tag($0) }
                        }
                        .frame(width: 140)
                        Toggle("Plane", isOn: $pourPlane)
                            .help("Reserve the layer for this net: other nets only pass through it with vias")
                    }
                    HStack {
                        Button("Add Ground Pours") { store.addGroundPours() }
                            .help("2-layer: GND on top and bottom · 4+ layers: GND plane on Inner 1 and a bottom pour")
                        Spacer()
                        Button("Add Pour") { store.addZone(net: pourNet, layer: pourLayer, plane: pourPlane) }
                            .disabled(!netChoices.contains(pourNet))
                    }
                }

                section("Net Classes", systemImage: "line.3.horizontal") {
                    Toggle("Autorouter sizes power nets from the simulation (IPC-2221)", isOn: Binding(
                        get: { board.autoSizeNets }, set: { store.setAutoSizeNets($0) }))
                    ForEach(board.netWidths.keys.sorted(), id: \.self) { net in
                        let width = board.netWidths[net] ?? 0
                        HStack {
                            Text(net).foregroundStyle(Theme.textPrimary)
                            Spacer()
                            Text(String(format: "%.2f mm", width)).monospacedDigit().foregroundStyle(Theme.textSecondary)
                            Button { store.setNetWidth(net, width: 0) } label: { Image(systemName: "trash") }
                                .buttonStyle(.borderless)
                                .accessibilityLabel("Remove net class \(net)")
                        }
                    }
                    HStack(spacing: 8) {
                        Picker("Net", selection: $classNet) {
                            if !netChoices.contains(classNet) { Text("Choose a net").tag(classNet) }
                            ForEach(netChoices, id: \.self) { Text($0).tag($0) }
                        }
                        .frame(width: 150)
                        field("Width", $classWidth)
                        Button("Set") { store.setNetWidth(classNet, width: value(classWidth, 0)) }
                            .disabled(!netChoices.contains(classNet))
                        Spacer()
                        Button("Auto-Size") { store.autoSizeNetWidths() }
                            .help("Widen nets to the IPC-2221 width for their DC current (+25 %)")
                    }
                }
            }
            .padding(14)
        }
        .frame(width: 470, height: 560)
        .background(Theme.deepBlue)
        .onAppear {
            outlineWidth = String(format: "%.1f", board.width)
            outlineHeight = String(format: "%.1f", board.height)
            let routable = store.snapshot.nets.filter { $0.pinCount > 1 }
            if !netChoices.contains(pourNet) {
                pourNet = routable.first { $0.ground }?.name ?? routable.first?.name ?? ""
            }
            if !netChoices.contains(classNet) {
                classNet = routable.first { !$0.ground }?.name ?? routable.first?.name ?? ""
            }
            pourLayer = board.bottomLayer
        }
        .onChange(of: preset) { _, newValue in
            switch newValue {
            case .quadX:
                outlineWidth = "100"
                outlineHeight = "44"
                outlineParameter = "12"
            case .rounded:
                outlineParameter = "3"
            default:
                break
            }
        }
    }

    /// Nets with at least two pins (the ones a pour or a net class can apply to).
    private var netChoices: [String] { store.snapshot.nets.filter { $0.pinCount > 1 }.map(\.name) }

    private func value(_ text: String, _ fallback: Double) -> Double {
        Double(text.replacingOccurrences(of: ",", with: ".")) ?? fallback
    }

    private func field(_ title: String, _ text: Binding<String>) -> some View {
        HStack(spacing: 4) {
            Text(title).foregroundStyle(Theme.textMuted)
            TextField(title, text: text).textFieldStyle(.blue).frame(width: 56)
            Text("mm").font(.caption).foregroundStyle(Theme.textMuted)
        }
    }

    private func section<Content: View>(_ title: String, systemImage: String,
                                        @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(title, systemImage: systemImage).font(.headline).foregroundStyle(Theme.textPrimary)
            content()
        }
        .padding(12)
        .frame(maxWidth: .infinity, alignment: .leading)
        .bluePanel()
    }
}

/// Standard square mounting patterns (flight-controller stacks and generic M2/M3 corners).
enum MountingPattern: String, CaseIterable, Identifiable {
    case m3FlightController
    case m2FlightController
    case m3Wide

    var id: String { rawValue }

    var title: String {
        switch self {
        case .m3FlightController: return "M3 · 30.5 × 30.5 mm (FC stack)"
        case .m2FlightController: return "M2 · 20 × 20 mm (mini FC)"
        case .m3Wide: return "M3 · 45 × 45 mm"
        }
    }

    var spacing: Double {
        switch self {
        case .m3FlightController: return 30.5
        case .m2FlightController: return 20
        case .m3Wide: return 45
        }
    }

    var drill: Double { self == .m2FlightController ? 2.2 : 3.2 }
    var keepout: Double { self == .m2FlightController ? 4.4 : 6.4 }
}
