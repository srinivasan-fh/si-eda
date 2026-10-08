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
    @State private var meshPart = ""
    @State private var fieldSolved: [String: FieldSolveInfo] = [:]
    @State private var meshNetA = "TAMPER_MESH_A"
    @State private var meshNetB = "TAMPER_MESH_B"

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

                section("Manufacturer Rules (DFM)", systemImage: "building.2") {
                    Picker("Manufacturer", selection: Binding(get: { board.dfmPack }, set: { store.setDfmPack($0) })) {
                        Text("None").tag("")
                        ForEach(EDAEngine.dfmPacks) { Text(verbatim: $0.name).tag($0.id) }
                    }
                    if let pack = EDAEngine.dfmPacks.first(where: { $0.id == board.dfmPack }) {
                        Text(verbatim: String(format: "%@ · %.3f / %.3f mm · Ø %.2f mm · %d", pack.notes, pack.minTrack,
                                              pack.minSpace, pack.minDrill, pack.maxLayers))
                            .font(.caption.monospacedDigit()).foregroundStyle(Theme.textSecondary)
                        DisclosureGroup("Sign-off Report") {
                            ForEach(store.engine.dfmReport()?.rows ?? []) { row in
                                HStack {
                                    Image(systemName: row.ok ? "checkmark.circle.fill" : "xmark.octagon.fill")
                                        .foregroundStyle(row.ok ? Color.green : Theme.warning)
                                    Text(verbatim: row.rule).foregroundStyle(Theme.textPrimary)
                                    Spacer()
                                    Text(verbatim: "\(row.actual) (\(row.limit))").font(.caption.monospacedDigit())
                                        .foregroundStyle(Theme.textSecondary)
                                }
                            }
                        }
                    }
                    Text("The DRC uses the manufacturer's minimum track, space, drill and annular ring, and adds fabrication and assembly checks: layers, board size, thickness, vias, solder-mask webs, silkscreen over pads, part spacing, part-to-edge distance, fiducials. Check the maker's current capability page before ordering.")
                        .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
                }

                panelSection(board.panel ?? PanelInfo())

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

                stackupSection(board)
                lengthMatchingSection(board)
                section("Protection & Reliability", systemImage: "shield.lefthalf.filled") {
                    Picker("Conformal coating", selection: Binding(get: { board.conformalCoating },
                                                                   set: { store.setCoating($0) })) {
                        ForEach(ConformalCoating.allCases) { Text($0.title).tag($0) }
                    }
                    Text(board.conformalCoating == .none
                         ? "Uncoated: voltage spacing follows IPC-2221B \(board.highAltitude ? "B3 (altitude)" : "B2"); high-impedance "
                           + "inputs keep 0.5 mm leakage spacing. Coating seals out moisture and contamination."
                         : "Coated (IPC-CC-830): voltage spacing follows IPC-2221B A5; leakage spacing 0.25 mm. Mask "
                           + "connectors and test points before coating.")
                        .font(.caption).foregroundStyle(Theme.textMuted)
                        .fixedSize(horizontal: false, vertical: true)
                    Picker("Board thickness", selection: Binding(get: { board.thickness },
                                                                 set: { store.setMechanical(thickness: $0) })) {
                        ForEach([0.8, 1.0, 1.6, 2.0, 2.4, 3.2], id: \.self) { Text(String(format: "%.1f mm", $0)).tag($0) }
                        if ![0.8, 1.0, 1.6, 2.0, 2.4, 3.2].contains(board.thickness) {
                            Text(String(format: "%.2f mm", board.thickness)).tag(board.thickness)
                        }
                    }
                    Picker("Isolation barrier", selection: Binding(get: { board.isolationGap },
                                                                   set: { store.setIsolationGap($0) })) {
                        Text("None").tag(0.0)
                        Text("2.5 mm (functional)").tag(2.5)
                        Text("4 mm (1 × MOPP)").tag(4.0)
                        Text("8 mm (2 × MOPP)").tag(8.0)
                        if ![0.0, 2.5, 4.0, 8.0].contains(board.isolationGap) {
                            Text(String(format: "%.1f mm", board.isolationGap)).tag(board.isolationGap)
                        }
                    }
                    .help("Parts, tracks and pours of separate galvanic domains (across isolators and isolated converters) "
                          + "keep this creepage apart; DRC checks it")
                    enclosurePicker("Tallest part, top", bottom: false) { store.setEnclosureHeight(top: $0) }
                    enclosurePicker("Tallest part, bottom", bottom: true) { store.setEnclosureHeight(bottom: $0) }
                    Toggle("Underfill / corner-bond heavy parts (shock)", isOn: Binding(
                        get: { board.underfill }, set: { store.setMechanical(underfill: $0) }))
                        .help("Epoxy under processors, BGAs and large capacitors so a MIL-STD-901E shock cannot tear them off")
                    tamperMeshRows(board)
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

    /// Active tamper meshes (PCI PTS): serpentine traces on two inner layers over a secure element.
    @ViewBuilder
    private func tamperMeshRows(_ board: BoardInfo) -> some View {
        let parts = store.snapshot.components.filter { $0.componentKind == .custom && $0.unitOf == nil }.map(\.ref).sorted()
        ForEach(store.snapshot.tamperMeshes) { mesh in
            HStack {
                Text("Tamper mesh over \(mesh.component): \(mesh.netA) (\(board.layerName(mesh.layerA))) · "
                     + "\(mesh.netB) (\(board.layerName(mesh.layerB)))")
                    .foregroundStyle(Theme.textPrimary)
                Spacer()
            }
        }
        HStack(spacing: 8) {
            Picker("Tamper mesh over", selection: $meshPart) {
                if !parts.contains(meshPart) { Text("Choose a part").tag(meshPart) }
                ForEach(parts, id: \.self) { Text($0).tag($0) }
            }
            Picker("Nets", selection: $meshNetA) {
                if !netChoices.contains(meshNetA) { Text(meshNetA).tag(meshNetA) }
                ForEach(netChoices, id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden()
            Picker("", selection: $meshNetB) {
                if !netChoices.contains(meshNetB) { Text(meshNetB).tag(meshNetB) }
                ForEach(netChoices, id: \.self) { Text($0).tag($0) }
            }
            .labelsHidden()
            Button("Add Mesh") { store.addTamperMesh(component: meshPart, netA: meshNetA, netB: meshNetB) }
                .disabled(!parts.contains(meshPart) || board.layerCount < 4 || meshNetA == meshNetB)
            if !store.snapshot.tamperMeshes.isEmpty {
                Button { store.clearTamperMeshes() } label: { Image(systemName: "trash") }
                    .buttonStyle(.borderless)
                    .accessibilityLabel("Remove tamper meshes")
            }
        }
        .help("Each mesh net joins the drive and sense pins of the secure element; Auto Route lays the serpentines on "
              + "Inner 1 / Inner 2 and keeps every other track and via out of the secure area (4+ layers)")
    }

    /// Laminate, construction and controlled-impedance targets, with the widths each copper layer needs.
    private func stackupSection(_ board: BoardInfo) -> some View {
        let report = store.stackup()
        return section("Stack-up & Impedance", systemImage: "square.3.layers.3d") {
            Picker("Laminate", selection: Binding(get: { board.material }, set: { store.setStackup(material: $0) })) {
                ForEach(report.materials) { Text($0.name).tag($0.id) }
            }
            if let m = report.materials.first(where: { $0.id == board.material }) {
                Text(String(format: "εr %.2f · Df %.4f · Tg %.0f °C — ", m.er, m.lossTangent, m.tg) + m.note)
                    .font(.caption).foregroundStyle(Theme.textMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Picker("Construction", selection: Binding(get: { board.boardConstruction },
                                                      set: { store.setStackup(construction: $0) })) {
                ForEach(BoardConstruction.allCases) { Text($0.title).tag($0) }
            }
            Stepper(String(format: "Single-ended %.0f Ω", board.singleEndedImpedance),
                    value: Binding(get: { board.singleEndedImpedance }, set: { store.setStackup(singleEnded: $0) }),
                    in: 20...150, step: 5)
            Stepper(String(format: "Differential %.0f Ω", board.differentialImpedance),
                    value: Binding(get: { board.differentialImpedance }, set: { store.setStackup(differential: $0) }),
                    in: 50...200, step: 5)
            Toggle("Backdrill via stubs on fast nets", isOn: Binding(get: { board.backdrill },
                                                                     set: { store.setStackup(backdrill: $0) }))
                .disabled(board.layerCount < 4)
                .help("Removes the unused via barrel below the last connected layer (≥ 4 layers); adds a back-drill file")
            Toggle("HDI vias: blind, buried and laser microvias (IPC-2226)", isOn: Binding(
                get: { board.hdi }, set: { store.setHDI(enabled: $0) }))
                .disabled(board.layerCount < 4)
                .help("Each via is cut to the layers it connects; one-dielectric spans become laser microvias over thin build-up layers")
            if board.hdi {
                Stepper(String(format: "Microvia drill %.2f mm / pad %.2f mm", board.microviaDrill, board.microviaDiameter),
                        value: Binding(get: { board.microviaDrill },
                                       set: { store.setHDI(microviaDrill: $0, microviaDiameter: max(board.microviaDiameter, $0 + 0.15)) }),
                        in: 0.05...0.15, step: 0.025)
            }
            Toggle("Via-in-pad plated over (VIPPO, IPC-4761 Type VII)", isOn: Binding(
                get: { board.viaInPad }, set: { store.setHDI(viaInPad: $0) }))
                .help("Vias in SMD pads are filled and capped by the fab, so fine-pitch BGA / QFN fan-out may use them")
            ForEach(report.layers) { layer in
                HStack {
                    Image(systemName: layer.isCopper ? "square.fill" : "square")
                        .foregroundStyle(layer.isCopper ? Color.orange : Theme.textMuted)
                    Text(layer.name).foregroundStyle(layer.isCopper ? Theme.textPrimary : Theme.textMuted)
                    Spacer()
                    if layer.isCopper, let se = layer.seWidth, let dw = layer.diffWidth, let gap = layer.diffGap {
                        Text(String(format: "%@ · SE %.3f · diff %.3f/%.3f mm", layer.line ?? "", se, dw, gap))
                            .font(.caption).monospacedDigit().foregroundStyle(Theme.textSecondary)
                        if let f = fieldSolved[layer.name] {
                            Text(String(format: "%.1f Ω", f.z0) + (f.zdiff.map { String(format: " / %.1f Ω", $0) } ?? ""))
                                .font(.caption).monospacedDigit().foregroundStyle(Theme.skyBlue)
                                .help(String(format: "Field solver: εeff %.2f, %.2f ps/mm, loss %.3f dB/in at 1 GHz, %.3f at 10 GHz",
                                             f.eeff, f.delayPsPerMm, f.dbPerInch(at: 1) ?? 0, f.dbPerInch(at: 10) ?? 0))
                        }
                    } else {
                        Text(String(format: "%.3f mm", layer.thickness))
                            .font(.caption).monospacedDigit().foregroundStyle(Theme.textMuted)
                    }
                }
            }
            Button("Check with Field Solver") { Task { fieldSolved = await store.fieldSolveStackup() } }
                .disabled(store.isBusy)
                .help("Solves each layer's cross-section (2D Laplace, with and without the dielectric) for the real Z0 and Zdiff of these widths")
            Text("The autorouter sizes RF lines and differential pairs (…_P/_N) to these impedances (IPC-2141).")
                .font(.caption).foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    /// Differential pairs and buses found from net names, their routed lengths and serpentine tuning.
    private func lengthMatchingSection(_ board: BoardInfo) -> some View {
        let report = store.lengthReport()
        return section("Length & Phase Matching", systemImage: "waveform.path") {
            Toggle("Auto Route adds serpentines to match lengths", isOn: Binding(
                get: { board.lengthTuning }, set: { store.setLengthMatching(enabled: $0) }))
            Stepper(String(format: "Pair skew ≤ %.2f mm", board.pairSkewTolerance),
                    value: Binding(get: { board.pairSkewTolerance }, set: { store.setLengthMatching(pairSkew: $0) }),
                    in: 0.03...2, step: 0.02)
            Stepper(String(format: "Bus length ≤ %.2f mm", board.busLengthTolerance),
                    value: Binding(get: { board.busLengthTolerance }, set: { store.setLengthMatching(bus: $0) }),
                    in: 0.1...10, step: 0.1)
            if report.groups.isEmpty {
                Text("No differential pairs (…_P/_N, …+/-) or buses (DQ0…, DATA[0..7], ADDR…) in this design.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
            ForEach(report.groups) { group in
                VStack(alignment: .leading, spacing: 2) {
                    HStack {
                        Image(systemName: group.matched ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                            .foregroundStyle(group.matched ? Color.green : Color.orange)
                        Text("\(group.isPair ? "Pair" : "Bus") \(group.name)").foregroundStyle(Theme.textPrimary)
                        Spacer()
                        Text(String(format: "target %.2f mm ± %.2f", group.target, group.tolerance))
                            .font(.caption).monospacedDigit().foregroundStyle(Theme.textSecondary)
                    }
                    ForEach(group.nets) { net in
                        HStack {
                            Text(net.name).font(.caption).foregroundStyle(Theme.textSecondary)
                            Spacer()
                            Text(net.routed ? String(format: "%.2f mm (−%.2f)", net.length, net.delta) : "unrouted")
                                .font(.caption).monospacedDigit()
                                .foregroundStyle(net.ok ? Theme.textMuted : Color.orange)
                        }
                        .padding(.leading, 22)
                    }
                }
            }
            HStack {
                Spacer()
                Button("Tune Lengths") { store.tuneLengths() }
                    .disabled(report.groups.allSatisfy(\.matched))
                    .help("Add serpentine (accordion) tuning to the short members of each group")
            }
        }
    }

    /// Enclosure height limit for one side (the DRC's 3D clearance check, MECH_HEIGHT).
    private func enclosurePicker(_ title: LocalizedStringKey, bottom: Bool,
                                 set: @escaping (Double) -> Void) -> some View {
        let options = [0.0, 2, 3, 5, 8, 12, 20, 30]
        let heights = store.engine.enclosureHeights()
        let current = bottom ? heights.bottom : heights.top
        return Picker(title, selection: Binding(get: { current }, set: set)) {
            Text("None").tag(0.0)
            ForEach(options.dropFirst(), id: \.self) { Text(String(format: "%.0f mm", $0)).tag($0) }
            if !options.contains(current) { Text(String(format: "%.1f mm", current)).tag(current) }
        }
        .help("The enclosure's room above the board: the DRC reports taller parts (and parts whose bodies collide)")
    }

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

    /// Production panel: counts, separation, gap and rails, with a preview of the layout the fab package will use.
    private func panelSection(_ panel: PanelInfo) -> some View {
        func set(_ change: (inout PanelInfo) -> Void) {
            var p = panel
            change(&p)
            store.setPanel(p)
        }
        return section("Production Panel", systemImage: "square.grid.3x2") {
            HStack {
                Stepper(value: Binding(get: { panel.nx }, set: { v in set { $0.nx = v } }), in: 1...20) {
                    Text("Boards across: \(panel.nx)")
                }
                Stepper(value: Binding(get: { panel.ny }, set: { v in set { $0.ny = v } }), in: 1...20) {
                    Text("Boards up: \(panel.ny)")
                }
            }
            Picker("Separation", selection: Binding(get: { panel.vscore }, set: { v in set { $0.vscore = v } })) {
                Text("Tabs and mouse bites").tag(false)
                Text("V-score").tag(true)
            }
            .pickerStyle(.segmented)
            HStack {
                Stepper(value: Binding(get: { panel.gap }, set: { v in set { $0.gap = v } }), in: 0...10, step: 0.5) {
                    Text(verbatim: String(format: "%@ %.1f mm", String(localized: "Gap"), panel.gap))
                }
                .disabled(panel.vscore)
                Stepper(value: Binding(get: { panel.rail }, set: { v in set { $0.rail = v } }), in: 0...15, step: 1) {
                    Text(verbatim: String(format: "%@ %.0f mm", String(localized: "Rails"), panel.rail))
                }
            }
            if panel.nx * panel.ny > 1, let layout = store.engine.panelLayout() {
                PanelPreview(layout: layout).frame(height: 140)
                Text(verbatim: String(format: "%.1f × %.1f mm · %d", layout.width, layout.height, layout.boards.count))
                    .font(.caption.monospacedDigit()).foregroundStyle(Theme.textSecondary)
            }
            Text("The fabrication package adds a panel/ folder: every layer stepped across the panel, rails with three fiducials and four tooling holes, and V-score lines or routed tabs with mouse bites.")
                .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
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

/// The panel drawn to scale: rails, boards, tabs, V-score lines, fiducials and tooling holes.
private struct PanelPreview: View {
    var layout: PanelLayoutInfo

    var body: some View {
        Canvas { context, size in
            let scale = min(size.width / max(layout.width, 1), size.height / max(layout.height, 1))
            let dx = (size.width - layout.width * scale) / 2, dy = (size.height - layout.height * scale) / 2
            func point(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: dx + x * scale, y: dy + (layout.height - y) * scale) }
            func rect(_ x0: Double, _ y0: Double, _ x1: Double, _ y1: Double) -> CGRect {
                let a = point(x0, y0), b = point(x1, y1)
                return CGRect(x: min(a.x, b.x), y: min(a.y, b.y), width: abs(b.x - a.x), height: abs(b.y - a.y))
            }
            context.fill(Path(rect(0, 0, layout.width, layout.height)), with: .color(Theme.darkBlue.opacity(0.6)))
            for b in layout.boards where b.count == 2 {
                context.fill(Path(rect(b[0], b[1], b[0] + layout.boardWidth, b[1] + layout.boardHeight)),
                             with: .color(Color(red: 0.13, green: 0.45, blue: 0.25)))
            }
            for t in layout.tabs where t.count == 2 {
                context.fill(Path(rect(t[0][0], t[0][1], t[1][0], t[1][1])), with: .color(Theme.lightBlue))
            }
            for v in layout.vscores where v.count == 2 {
                var line = Path()
                line.move(to: point(v[0][0], v[0][1]))
                line.addLine(to: point(v[1][0], v[1][1]))
                context.stroke(line, with: .color(Theme.warning), style: StrokeStyle(lineWidth: 1, dash: [3, 2]))
            }
            for (points, colour, d) in [(layout.fiducials, Color.yellow, 2.0), (layout.toolingHoles, Color.white, 2.0)] {
                for p in points where p.count == 2 {
                    let c = point(p[0], p[1]), r = max(1.5, d * scale / 2)
                    context.fill(Path(ellipseIn: CGRect(x: c.x - r, y: c.y - r, width: 2 * r, height: 2 * r)), with: .color(colour))
                }
            }
        }
        .accessibilityLabel(Text("Panel preview"))
    }
}
