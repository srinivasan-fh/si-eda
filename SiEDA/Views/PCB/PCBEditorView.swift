import AppKit
import SwiftUI

/// Layers shown in the PCB editor: every copper layer of the stack-up plus documentation overlays.
enum PCBLayer: Hashable, Identifiable {
    case copper(Int)
    case silkscreen
    case ratsnest
    case courtyard
    case boardOutline

    var id: String {
        switch self {
        case .copper(let index): return "copper\(index)"
        case .silkscreen: return "silk"
        case .ratsnest: return "rats"
        case .courtyard: return "court"
        case .boardOutline: return "outline"
        }
    }

    static let overlays: [PCBLayer] = [.silkscreen, .ratsnest, .courtyard, .boardOutline]
    /// Everything visible by default (covers the largest supported stack-up).
    static let defaultVisible: Set<PCBLayer> = Set((0..<6).map { PCBLayer.copper($0) } + overlays)

    static func all(for board: BoardInfo) -> [PCBLayer] {
        (0..<max(1, board.layerCount)).map { PCBLayer.copper($0) } + overlays
    }

    func name(_ board: BoardInfo) -> String {
        switch self {
        case .copper(let index): return board.layerName(index) + " Layer"
        case .silkscreen: return "Top Overlay"
        case .ratsnest: return "Ratsnest"
        case .courtyard: return "Courtyard"
        case .boardOutline: return "Keep-Out / Outline"
        }
    }

    func color(_ board: BoardInfo) -> Color {
        switch self {
        case .copper(let index): return Theme.copperColor(index, layerCount: board.layerCount)
        case .silkscreen: return Theme.silkscreen
        case .ratsnest: return Theme.ratsnest
        case .courtyard: return Theme.lightBlue
        case .boardOutline: return Theme.boardEdge
        }
    }

    var copperIndex: Int? {
        if case .copper(let index) = self { return index }
        return nil
    }
}

/// PCB layout workspace: tool strip, options bar with design rules, canvas, Altium-style layer tabs.
struct PCBEditorView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var viewport = Viewport(scale: 12, offset: CGSize(width: 80, height: 80))
    @State private var canvasSize: CGSize = .zero
    @State private var visible: Set<PCBLayer> = PCBLayer.defaultVisible
    @State private var activeLayer: PCBLayer = .copper(0)
    @State private var panMode = false
    /// Route tool (X): interactive routing with walkaround / push-and-shove.
    @State private var routeTool = false
    @State private var routePair = false
    /// Tune Length tool (T): click a track, set the target, Enter adds the meanders.
    @State private var tuneTool = false
    @State private var tuneTargetText = ""
    @State private var tuneAmplitudeText = ""
    @State private var tuneSpacingText = ""
    @State private var showLayersPanel = true
    @State private var showBoardSetup = false
    @State private var showLengthRules = false

    @State private var boardWidth = ""
    @State private var boardHeight = ""
    @State private var trackWidth = ""
    @State private var clearance = ""

    /// The one Auto Route action (⇧⌘R): places any footprints not on the board yet, inside its shape, then routes
    /// every connection from scratch and runs DRC.
    private var autoRouteButton: some View {
        Button { Task { await store.autoRouteBoard() } } label: {
            Label("Auto Route", systemImage: "point.topleft.down.to.point.bottomright.curvepath.fill")
        }
        .buttonStyle(.borderedProminent)
        .controlSize(.small)
        .disabled(store.isBusy || !store.snapshot.components.contains { !$0.componentKind.isVirtual })
        .help("Place any unplaced footprints inside the board outline, then route all connections and check DRC (⇧⌘R)")
    }

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move footprints; drag a track or via to shove it (V)",
                                isActive: !panMode && !routeTool && !tuneTool) {
                    panMode = false
                    routeTool = false
                    tuneTool = false
                }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: panMode) {
                    panMode = true
                    routeTool = false
                    tuneTool = false
                }
                ToolStripButton(systemImage: "scribble.variable",
                                help: "Route tracks (X): click a pad, click to place corners, V adds a via, Enter or double-click finishes, Esc cancels",
                                isActive: routeTool) {
                    panMode = false
                    routeTool = true
                    tuneTool = false
                }
                ToolStripButton(systemImage: "waveform.path",
                                help: "Tune length (T): click a track, set the target, Enter adds the meanders, Esc cancels",
                                isActive: tuneTool) {
                    panMode = false
                    routeTool = false
                    tuneTool = true
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "rotate.right", help: "Rotate footprint (Space or R)") { store.rotateFootprints() }
                ToolStripButton(systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right", help: "Flip to other side (F)") {
                    store.flipFootprints()
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "square.grid.3x3.topleft.filled", help: "Auto-place all footprints") {
                    store.autoPlace(all: true)
                }
                ToolStripButton(systemImage: "arrow.down.right.and.arrow.up.left.rectangle", help: "Fit board to components") {
                    store.fitBoard()
                }
                ToolStripButton(systemImage: "point.topleft.down.to.point.bottomright.curvepath.fill", help: "Auto Route (⇧⌘R)") {
                    Task { await store.autoRouteBoard() }
                }
                ToolStripButton(systemImage: "arrow.up.left.and.arrow.down.right",
                                help: "Fan out the selected parts: an escape track and a via on each pad that still needs one") {
                    store.fanoutSelection()
                }
                ToolStripButton(systemImage: "point.topleft.down.curvedto.point.bottomright.up",
                                help: "Convert corners to arcs: the selected tracks (click a track, ⇧-click adds), or every track") {
                    store.convertCornersToArcs()
                }
                ToolStripButton(systemImage: "drop",
                                help: "Teardrops where the selected tracks (or every track) meet pads and vias; again removes them") {
                    store.toggleTeardrops()
                }
                ToolStripButton(systemImage: "wand.and.stars",
                                help: "Gloss: pull the selected routes (or every route) tight and retrace them shorter") {
                    store.glossTracks()
                }
                ToolStripButton(systemImage: "circle.grid.3x3",
                                help: "Via stitching: ground vias wherever ground pours or planes overlap on two layers") {
                    store.stitchVias()
                }
                ToolStripButton(systemImage: "shield.lefthalf.filled",
                                help: "Via shielding: ground vias on both sides of the selected tracks") {
                    store.shieldSelectedTracks()
                }
                ToolStripButton(systemImage: "equal.square",
                                help: "Match lengths: tune the nets of the selected tracks (a bus) to the longest of them") {
                    store.matchSelectedLengths()
                }
                ToolStripButton(systemImage: "eraser", help: "Clear all tracks and vias") { store.clearRouting() }
                ToolStripButton(systemImage: "checkmark.seal", help: "Design rule check") {
                    store.runDRC()
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "square.3.layers.3d", help: "Layers panel", isActive: showLayersPanel) {
                    showLayersPanel.toggle()
                }
                ToolStripButton(systemImage: "map", help: "Navigator overview (N)", isActive: store.showNavigator) {
                    store.showNavigator.toggle()
                }
                ToolStripButton(systemImage: "plus.magnifyingglass", help: "Zoom to area (Z) — drag a rectangle") {
                    store.requestView(.zoomArea)
                }
            }
            .disabled(store.isBusy)  // board edits wait for the autorouter

            VStack(spacing: 0) {
                OptionsBar {
                    autoRouteButton
                    if tuneTool {
                        Divider().frame(height: 18)
                        tuneControls
                    } else if !panMode {
                        Divider().frame(height: 18)
                        Picker("Router mode", selection: $store.routerMode) {
                            Text("Shove").tag(RouterModeChoice.shove)
                            Text("Walk around").tag(RouterModeChoice.walkaround)
                            Text("Highlight").tag(RouterModeChoice.highlight)
                            Text("Stop at obstacle").tag(RouterModeChoice.stop)
                        }
                        .pickerStyle(.segmented)
                        .labelsHidden()
                        .fixedSize()
                        .help("Shove pushes other nets' tracks and vias aside; Walk around routes around them; Highlight goes where you point and marks every collision in red")
                        Toggle("Hug obstacles", isOn: $store.routerHugDrag)
                            .toggleStyle(.checkbox)
                            .help("A dragged track bends around pads and other copper it cannot push instead of stopping short")
                    }
                    if routeTool {
                        Picker("Corners", selection: $store.routerDiagonal) {
                            Text(verbatim: "45°").tag(true)
                            Text(verbatim: "90°").tag(false)
                        }
                        .pickerStyle(.segmented)
                        .labelsHidden()
                        .fixedSize()
                        Toggle("Any angle", isOn: $store.routerAnyAngle)
                            .toggleStyle(.checkbox)
                            .help("Route and drag at any angle: the head is one straight track to the pointer")
                        Toggle("Remove loops", isOn: $store.routerRemoveLoops)
                            .toggleStyle(.checkbox)
                            .help("Finishing a route removes the old path of the net it makes redundant")
                        Toggle("Auto teardrops", isOn: $store.routerTeardrops)
                            .toggleStyle(.checkbox)
                            .help("Teardrops where each finished route meets pads and vias")
                        Toggle("Rounded corners", isOn: $store.routerRounded)
                            .toggleStyle(.checkbox)
                            .help("Corners become arcs (drawn as short straight chords) where they fit and keep clearance; single tracks only")
                        if store.routerRounded {
                            Toggle("True arcs", isOn: $store.routerArcs)
                                .toggleStyle(.checkbox)
                                .help("True arcs (G02/G03 in Gerber); pairs and buses turn on concentric arcs. Off: short straight chords")
                        }
                        Toggle("Differential pair", isOn: $routePair)
                            .toggleStyle(.checkbox)
                            .help("Route both nets of a differential pair (X_P / X_N) together at the pair gap")
                            .onChange(of: routePair) { _, on in if on { store.routerBus = false } }
                        Toggle("Bus", isOn: $store.routerBus)
                            .toggleStyle(.checkbox)
                            .help("Click a pad: it and the next pads of its row route together as a bundle at track pitch; finish, then continue each track")
                            .onChange(of: store.routerBus) { _, on in if on { routePair = false } }
                        if store.routerBus {
                            Picker("Bus width", selection: $store.routerBusWidth) {
                                ForEach(2...8, id: \.self) { Text(verbatim: "\($0)").tag($0) }
                            }
                            .pickerStyle(.menu)
                            .labelsHidden()
                            .fixedSize()
                            .help("Number of nets in the bus")
                        }
                        Picker("Via type", selection: $store.routerViaType) {
                            Text("Through").tag(RouterViaChoice.through)
                            Text("Blind / buried").tag(RouterViaChoice.blind)
                            Text("Microvia").tag(RouterViaChoice.micro)
                            Text("Auto").tag(RouterViaChoice.auto)
                        }
                        .pickerStyle(.menu)
                        .fixedSize()
                        .help("Via placed with V: through to the other side, or (HDI boards) blind / buried or a microvia to the next layer; Shift-V goes to the next layer the other way")
                    }
                    Divider().frame(height: 18)
                    Image(systemName: "square.3.layers.3d.down.right").foregroundStyle(Theme.blue)
                    Picker("Layers", selection: Binding(get: { store.snapshot.board.layerCount },
                                                        set: { store.setLayerCount($0) })) {
                        ForEach(BoardInfo.layerChoices, id: \.self) { Text("\($0) layer\($0 == 1 ? "" : "s")").tag($0) }
                    }
                    .pickerStyle(.menu)
                    .frame(width: 120)
                    .help("Copper layers: 1 = single-sided (no vias), 2–6 for most products, 8–24 for motherboards, servers, mainframes and GPU baseboards")
                    Menu {
                        ForEach(StandardLibrary.rulePresets) { preset in
                            Button {
                                store.applyRulePreset(preset)
                            } label: {
                                if preset.name == store.snapshot.board.rulePreset {
                                    Label(preset.name, systemImage: "checkmark")
                                } else {
                                    Text(preset.name)
                                }
                            }
                            .help(preset.description)
                        }
                    } label: {
                        Label(store.snapshot.board.rulePreset, systemImage: "ruler")
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .help("Design-rule preset: track, clearance and via sizes plus the fabrication minimums DRC enforces")
                    Button { showBoardSetup.toggle() } label: {
                        Label("Board Setup", systemImage: "slider.horizontal.3")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                    .help("Board outline (quad-X frame, rounded, circle), mounting holes, copper pours/planes and net classes")
                    .popover(isPresented: $showBoardSetup, arrowEdge: .bottom) { BoardSetupPanel().environmentObject(store) }
                    Divider().frame(height: 18)
                    HStack(spacing: 10) {
                        // A shaped outline is sized in Board Setup; W/H would only resize the bounding rectangle.
                        ruleField("Board W", $boardWidth, unit: "mm")
                            .disabled(store.snapshot.board.hasCustomOutline)
                        ruleField("H", $boardHeight, unit: "mm")
                            .disabled(store.snapshot.board.hasCustomOutline)
                        ruleField("Track", $trackWidth, unit: "mm")
                        ruleField("Clr", $clearance, unit: "mm")
                        Button("Apply") { applyRules() }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                    Spacer()
                    if let stats = store.routeStats, store.routeStatsAreCurrent {
                        Badge(text: "\(stats.routed)/\(stats.connections) routed · \(stats.vias) vias",
                              systemImage: stats.failed == 0 ? "checkmark.circle" : "exclamationmark.triangle")
                    }
                    let drcErrors = store.drcResults.filter { $0.severity == .error }.count
                    if store.drcIsCurrent {
                        Badge(text: drcErrors == 0 ? "DRC clean" : "DRC \(drcErrors)", systemImage: "checkmark.seal")
                    }
                    ZoomControls(level: viewport.scale / Viewport.pcbBaseScale,
                                 zoomIn: { store.requestView(.zoomIn) }, zoomOut: { store.requestView(.zoomOut) },
                                 fit: { store.requestView(.fit) }, fitSelection: { store.requestView(.fitSelection) },
                                 setLevel: { store.requestView(.setLevel($0)) })
                }

                ZStack(alignment: .topTrailing) {
                    PCBCanvas(viewport: $viewport, canvasSize: $canvasSize, panMode: $panMode, routeTool: $routeTool,
                              tuneTool: $tuneTool, routePair: routePair, visible: visible, activeLayer: activeLayer)
                        .disabled(store.isBusy)  // the engine is busy autorouting
                    if store.showNavigator, !store.snapshot.pads.isEmpty {
                        navigator
                            .canvasScrollShield()
                            .padding(10)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    }
                    if showLayersPanel {
                        LayersPanel(layers: PCBLayer.all(for: store.snapshot.board), board: store.snapshot.board,
                                    visible: $visible, active: $activeLayer)
                            .canvasScrollShield()
                            .padding(10)
                    }
                    if !store.snapshot.pads.isEmpty, unplacedCount == 0, !store.isBusy, !store.snapshot.ratsnest.isEmpty {
                        // Like "Place Now" for footprints: one click routes what is still a ratsnest line.
                        let openCount = store.snapshot.ratsnest.count
                        HStack(spacing: 8) {
                            Image(systemName: "point.topleft.down.to.point.bottomright.curvepath").foregroundStyle(Theme.skyBlue)
                            Text("\(openCount) connection\(openCount == 1 ? " is" : "s are") not routed yet")
                                .foregroundStyle(Theme.textPrimary)
                            Button("Auto Route") { Task { await store.autoRouteBoard() } }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                        }
                        .font(.callout)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
                        .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.5)))
                        .padding(10)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                    }
                    if !store.snapshot.pads.isEmpty, unplacedCount > 0 {
                        // Viewing the board never edits it; new parts are placed only on request.
                        HStack(spacing: 8) {
                            Image(systemName: "exclamationmark.square").foregroundStyle(Theme.warning)
                            Text("\(unplacedCount) part\(unplacedCount == 1 ? " is" : "s are") not on the board yet")
                                .foregroundStyle(Theme.textPrimary)
                            Button("Place Now") { store.autoPlace(all: false) }
                                .buttonStyle(.borderedProminent)
                                .controlSize(.small)
                        }
                        .font(.callout)
                        .padding(.horizontal, 12)
                        .padding(.vertical, 6)
                        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
                        .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.5)))
                        .padding(10)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                    }
                    if store.snapshot.pads.isEmpty {
                        BlueEmptyState(systemImage: "square.grid.3x3.square",
                                       title: "No footprints on the board",
                                       message: "Place the schematic's footprints automatically, then Auto Route the board.",
                                       actionTitle: "Auto-Place Footprints") { store.autoPlace(all: true) }
                            .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }

                LayerTabs(layers: PCBLayer.all(for: store.snapshot.board), board: store.snapshot.board,
                          visible: visible, active: $activeLayer)
            }
        }
        .background(Theme.pcbBackground)
        .onAppear { syncRuleFields() }
        .onDisappear { store.cancelRoute() }
        .onChange(of: store.snapshot.board) { old, board in
            syncRuleFields(changedFrom: old)
            if let index = activeLayer.copperIndex, index >= board.layerCount { activeLayer = .copper(0) }
        }
        // A via moves the route to another layer: that layer becomes the active (top-drawn) one.
        .onChange(of: store.routePreview?.layer) { _, layer in
            if let layer, layer < store.snapshot.board.layerCount { activeLayer = .copper(layer) }
        }
        .onChange(of: routeTool) { _, on in if !on { store.cancelRoute() } }
        .onChange(of: tuneTool) { _, on in if !on { store.cancelTune() } }
        .onChange(of: store.tuneSession?.preview?.target) { _, target in
            if let target { tuneTargetText = String(format: "%.2f", target) }
        }
    }

    /// Tune Length options: target (typed, or the net's pair / bus group), meander height and leg spacing, the
    /// live length read-out and Apply.
    @ViewBuilder private var tuneControls: some View {
        HStack(spacing: 6) {
            Text("Target").foregroundStyle(Theme.textMuted)
            TextField("Target", text: $tuneTargetText)
                .textFieldStyle(.blue)
                .frame(width: 60)
                .onSubmit { if let v = Double(tuneTargetText) { store.setTuneTarget(v) } }
            Text(verbatim: "mm").foregroundStyle(Theme.textMuted).font(.caption)
            Button("Match Group") { store.setTuneTarget(0) }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(store.tuneSession?.preview?.group.isEmpty ?? true)
                .help("Tune to the longest member of the net's differential pair or bus")
            Text("Amplitude").foregroundStyle(Theme.textMuted)
            TextField("Auto", text: $tuneAmplitudeText)
                .textFieldStyle(.blue)
                .frame(width: 44)
                .onSubmit { store.tuneAmplitude = max(0, Double(tuneAmplitudeText) ?? 0) }
                .help("Meander height limit in mm (empty: 2 mm)")
            Text("Spacing").foregroundStyle(Theme.textMuted)
            TextField("Auto", text: $tuneSpacingText)
                .textFieldStyle(.blue)
                .frame(width: 44)
                .onSubmit { store.tuneSpacing = max(0, Double(tuneSpacingText) ?? 0) }
                .help("Gap between meander legs, edge to edge, in mm (empty: three track widths between centres)")
            Picker("Pattern", selection: $store.tuneStyle) {
                Text("Accordion").tag(MeanderStyleChoice.accordion)
                Text("Trombone").tag(MeanderStyleChoice.trombone)
                Text("Sawtooth").tag(MeanderStyleChoice.sawtooth)
            }
            .pickerStyle(.menu)
            .fixedSize()
            .help("Accordion: rectangular meanders; Trombone: one wide loop; Sawtooth: triangular teeth")
            Picker("Meander corners", selection: $store.tuneCorner) {
                Text("Square").tag(MeanderCornerChoice.square)
                Text("Mitered").tag(MeanderCornerChoice.mitered)
                Text("Round").tag(MeanderCornerChoice.round)
            }
            .pickerStyle(.menu)
            .fixedSize()
            .help("Corners of the meanders: square, 45° mitered or round (true arcs)")
            Toggle("Coupled", isOn: $store.tuneCoupled)
                .toggleStyle(.checkbox)
                .help("Differential pair: meander both members together at their gap")
            Toggle("Phase", isOn: $store.tunePhase)
                .toggleStyle(.checkbox)
                .help("Skew tuning of a pair member: small bumps on the side away from its partner, to the partner's length")
            Button("Length Rules") { showLengthRules.toggle() }
                .buttonStyle(.bordered)
                .controlSize(.small)
                .help("Length targets per net and match groups, measured pad to pad through series parts")
                .popover(isPresented: $showLengthRules, arrowEdge: .bottom) { LengthRulesPanel().environmentObject(store) }
            Button("Apply Tuning") { store.applyTune() }
                .buttonStyle(.borderedProminent)
                .controlSize(.small)
                .disabled(!(store.tuneSession?.preview?.ok ?? false))
                .help("Add the previewed meanders to the board (Enter)")
        }
    }

    /// Overview of the board; click or drag to move the view.
    private var navigator: some View {
        let board = CGRect(x: 0, y: 0, width: store.snapshot.board.width, height: store.snapshot.board.height)
        let courtyards = store.snapshot.courtyards
        return CanvasNavigator(extent: courtyards.reduce(board) { $0.union($1.rect) }.insetBy(dx: -2, dy: -2),
                               items: [board] + courtyards.map(\.rect),
                               highlighted: courtyards.filter { store.selection.contains($0.component) }.map(\.rect),
                               viewport: viewport, canvasSize: canvasSize,
                               onCenter: { viewport.center(on: $0, in: canvasSize) },
                               onClose: { store.showNavigator = false })
    }

    private var unplacedCount: Int {
        store.snapshot.components.filter { !$0.componentKind.isVirtual && !$0.pcb.placed && $0.unitOf == nil }.count
    }

    private func ruleField(_ title: String, _ text: Binding<String>, unit: String) -> some View {
        HStack(spacing: 4) {
            Text(title).foregroundStyle(Theme.textMuted)
            TextField(title, text: text)
                .textFieldStyle(.blue)
                .frame(width: 54)
                .onSubmit { applyRules() }
            Text(unit).foregroundStyle(Theme.textMuted).font(.caption)
        }
    }

    /// Shows the board's size and rules in the fields. After a board change only the values that actually changed
    /// are rewritten, so text being typed survives unrelated edits (a mounting hole, a pour, a layer change).
    private func syncRuleFields(changedFrom old: BoardInfo? = nil) {
        let b = store.snapshot.board
        if old == nil || old?.width != b.width { boardWidth = String(format: "%.1f", b.width) }
        if old == nil || old?.height != b.height { boardHeight = String(format: "%.1f", b.height) }
        if old == nil || old?.trackWidth != b.trackWidth { trackWidth = String(format: "%.2f", b.trackWidth) }
        if old == nil || old?.clearance != b.clearance { clearance = String(format: "%.2f", b.clearance) }
    }

    private func applyRules() {
        let b = store.snapshot.board
        store.setBoard(width: Double(boardWidth) ?? b.width, height: Double(boardHeight) ?? b.height,
                       trackWidth: Double(trackWidth) ?? b.trackWidth, clearance: Double(clearance) ?? b.clearance)
    }
}

/// Photoshop-style layers panel with visibility eyes.
struct LayersPanel: View {
    var layers: [PCBLayer]
    var board: BoardInfo
    @Binding var visible: Set<PCBLayer>
    @Binding var active: PCBLayer
    @AppStorage("pcb.colourByNet") private var colourByNet = true

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("COPPER COLOURS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue).padding(.bottom, 2)
            Picker("Copper colours", selection: $colourByNet) {
                Text("By Net").tag(true)
                Text("By Layer").tag(false)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .controlSize(.small)
            .help("By Net: power red, ground blue, negative rails purple, signals in their layer's colour. "
                  + "By Layer: top red, bottom blue, inner yellow / green / orange / magenta.")
            if colourByNet {
                ForEach([NetRole.power, .ground, .negative], id: \.self) { role in
                    legendRow(Theme.netColor(role, layer: 0, layerCount: board.layerCount), role.title)
                }
                ForEach(0..<max(1, board.layerCount), id: \.self) { layer in
                    legendRow(Theme.signalColor(layer, layerCount: board.layerCount), "Signal · \(board.layerName(layer))")
                }
            }
            Divider().padding(.vertical, 4)
            Text("LAYERS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue).padding(.bottom, 4)
            ForEach(layers) { layer in
                HStack(spacing: 8) {
                    Button {
                        if visible.contains(layer) { visible.remove(layer) } else { visible.insert(layer) }
                    } label: {
                        Image(systemName: visible.contains(layer) ? "eye" : "eye.slash")
                            .foregroundStyle(visible.contains(layer) ? Theme.skyBlue : Theme.textMuted)
                            .frame(width: 18)
                    }
                    .buttonStyle(.plain)
                    .accessibilityLabel(visible.contains(layer) ? "Hide \(layer.name(board))" : "Show \(layer.name(board))")
                    RoundedRectangle(cornerRadius: 3).fill(layer.color(board)).frame(width: 12, height: 12)
                    Text(layer.name(board)).font(.caption)
                        .foregroundStyle(active == layer ? Theme.textPrimary : Theme.textSecondary)
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 3)
                .padding(.horizontal, 6)
                .background(RoundedRectangle(cornerRadius: 5).fill(active == layer ? Theme.blue.opacity(0.3) : .clear))
                .contentShape(Rectangle())
                .onTapGesture {
                    // Only copper can be the active (editing/top-drawn) layer; overlays just show or hide.
                    if layer.copperIndex != nil {
                        active = layer
                    } else if visible.contains(layer) {
                        visible.remove(layer)
                    } else {
                        visible.insert(layer)
                    }
                }
            }
        }
        .padding(10)
        .frame(width: 210)
        .bluePanel()
    }
}

extension LayersPanel {
    fileprivate func legendRow(_ colour: Color, _ title: String) -> some View {
        HStack(spacing: 8) {
            Capsule().fill(colour).frame(width: 18, height: 4)
            Text(title).font(.caption2).foregroundStyle(Theme.textSecondary)
        }
        .padding(.horizontal, 6)
        .padding(.vertical, 1)
        .accessibilityElement(children: .combine)
    }
}

/// Altium-style coloured layer tabs along the bottom edge.
struct LayerTabs: View {
    var layers: [PCBLayer]
    var board: BoardInfo
    var visible: Set<PCBLayer>
    @Binding var active: PCBLayer

    var body: some View {
        HStack(spacing: 0) {
            ForEach(layers) { layer in
                Button {
                    if layer.copperIndex != nil { active = layer }
                } label: {
                    HStack(spacing: 6) {
                        RoundedRectangle(cornerRadius: 2).fill(layer.color(board)).frame(width: 10, height: 10)
                        Text(layer.name(board)).font(.caption)
                    }
                    .padding(.horizontal, 12)
                    .padding(.vertical, 5)
                    .foregroundStyle(active == layer ? Color.white : (visible.contains(layer) ? Theme.textSecondary : Theme.textMuted))
                    .background(active == layer ? Theme.blue.opacity(0.45) : Color.clear)
                }
                .buttonStyle(.plain)
                .help(layer.copperIndex != nil ? "Make \(layer.name(board)) the active layer"
                                               : "Overlay layer — show or hide it in the Layers panel")
                Rectangle().fill(Theme.blue.opacity(0.25)).frame(width: 1, height: 18)
            }
            Spacer()
        }
        .background(Theme.deepBlue)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .top)
    }
}

/// The PCB drawing surface (millimetre world coordinates, y down).
/// Tracks drawn with one stroke: same highlight, colour role and width (µm).
private struct TrackBatch: Hashable {
    let highlight: Bool
    let role: NetRole?
    let microns: Int
}

struct PCBCanvas: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var viewport: Viewport
    @Binding var canvasSize: CGSize
    @Binding var panMode: Bool
    @Binding var routeTool: Bool
    @Binding var tuneTool: Bool
    var routePair: Bool
    var visible: Set<PCBLayer>
    var activeLayer: PCBLayer
    /// Copper coloured by what it carries (power red, ground blue, …) or by layer (top red, bottom blue, …).
    @AppStorage("pcb.colourByNet") private var colourByNet = true

    private enum DragMode {
        case move(Set<Int>)
        case pan(CGSize)
        case zoomBox
        /// Select tool on a track or via: the shove-drag starts once the pointer has moved a few points.
        case dragTrack(Int, CGPoint)
        case dragVia(Int, CGPoint)
        /// Tune tool drag along a track: the meanders' stretch follows the pointer.
        case tuneDrag(Int, CGPoint)
    }

    /// A track or via drag session is running in the router.
    @State private var copperDragActive = false

    @State private var dragMode: DragMode?
    @State private var dragDelta: CGSize = .zero
    @State private var hover: CGPoint?
    @State private var magnifyBase: CGFloat?
    @State private var didFit = false
    @State private var zoomArmed = false   // Z: the next drag defines the area to zoom to
    @State private var spaceHeld = false   // Space + left-drag pans; a Space tap rotates
    @State private var spaceUsedForPan = false
    @State private var zoomRect: CGRect?   // screen space, while dragging a zoom area
    @FocusState private var focused: Bool

    private let limits = Viewport.pcbLimits

    var body: some View {
        GeometryReader { geo in
            Canvas(rendersAsynchronously: false) { ctx, size in draw(&ctx, size: size) }
                .contentShape(Rectangle())
                .accessibilityElement()
                .accessibilityLabel("PCB layout canvas")
                .accessibilityValue(String(format: "%.0f by %.0f millimetre board, %d layers, %d tracks, %d unrouted connections",
                                           store.snapshot.board.width, store.snapshot.board.height,
                                           store.snapshot.board.layerCount, store.snapshot.tracks.count,
                                           store.snapshot.ratsnest.count))
                .gesture(drag)
                .simultaneousGesture(
                    MagnifyGesture()
                        .onChanged { value in
                            if magnifyBase == nil { magnifyBase = viewport.scale }
                            let target = (magnifyBase ?? 1) * value.magnification
                            viewport.zoom(by: target / viewport.scale, anchor: value.startLocation, limits: limits)
                        }
                        .onEnded { _ in magnifyBase = nil }
                )
                .canvasMouseInput(onScroll: { event, point in viewport.handleScroll(event, at: point, limits: limits) },
                                  onPan: { viewport.pan(by: $0) })
                .onContinuousHover { phase in
                    switch phase {
                    case .active(let p):
                        hover = p
                        // The route's head follows the cursor (shoving or walking around as set).
                        if routeTool, store.routePreview != nil { store.moveRoute(to: viewport.toWorld(p)) }
                    case .ended: hover = nil
                    }
                }
                .focusable()
                .focusEffectDisabled()
                .focused($focused)
                .canvasNavigationKeys(toggleNavigator: { store.showNavigator.toggle() }) { command in
                    perform(command, size: geo.size)
                }
                .onKeyPress(.space, phases: [.down, .repeat, .up]) { press in
                    switch press.phase {
                    case .down:
                        spaceHeld = true
                        spaceUsedForPan = false
                    case .up:
                        spaceHeld = false
                        if !spaceUsedForPan { store.rotateFootprints() }  // a tap rotates; Space + drag panned instead
                    default:
                        break
                    }
                    return .handled
                }
                .onChange(of: focused) { _, isFocused in if !isFocused { spaceHeld = false } }
                // Single-letter keys; ⌘/⌥/⌃ combinations belong to menus and text editing.
                .onKeyPress(keys: ["r", "f", "v", "V", "h", "x", "t"], phases: .down) { press in
                    guard press.modifiers.subtracting(.shift).isEmpty else { return .ignored }
                    switch press.key {
                    case KeyEquivalent("t"):
                        panMode = false
                        routeTool = false
                        tuneTool = true
                    case KeyEquivalent("r"): store.rotateFootprints()
                    case KeyEquivalent("f"): store.flipFootprints()
                    case KeyEquivalent("v"), KeyEquivalent("V"):
                        // While routing, V places a via and continues on the other side (as in other PCB tools);
                        // Shift-V goes to the next layer the other way (blind / micro vias).
                        if store.routePreview != nil {
                            store.addRouteVia(reverse: press.modifiers.contains(.shift))
                        } else {
                            panMode = false
                            routeTool = false
                            tuneTool = false
                        }
                    case KeyEquivalent("h"):
                        panMode = true
                        routeTool = false
                        tuneTool = false
                    case KeyEquivalent("x"):
                        panMode = false
                        routeTool = true
                        tuneTool = false
                    default: return .ignored
                    }
                    return .handled
                }
                .onKeyPress(.escape) {
                    if store.routePreview != nil {
                        store.cancelRoute()
                    } else if !store.multiStarts.isEmpty {
                        store.multiStarts = []
                    } else if store.tuneSession != nil {
                        store.cancelTune()
                    } else if tuneTool {
                        tuneTool = false
                    } else if zoomArmed {
                        zoomArmed = false
                    } else if routeTool {
                        routeTool = false
                    } else {
                        store.select(component: nil)
                    }
                    return .handled
                }
                .onKeyPress(.return) {
                    if store.tuneSession != nil {
                        store.applyTune()
                        return .handled
                    }
                    guard store.routePreview != nil else { return .ignored }
                    store.finishRoute()
                    return .handled
                }
                .onAppear {
                    canvasSize = geo.size
                    focused = true
                    if !didFit {
                        didFit = true
                        fit(geo.size)
                    }
                }
                .onChange(of: geo.size) { _, s in canvasSize = s }
                .onChange(of: store.viewRequest) { _, request in
                    if let request { perform(request.command, size: geo.size) }
                }
                .onChange(of: store.fitToken) { _, _ in fit(geo.size) }
        }
    }

    private func fit(_ size: CGSize) {
        let b = store.snapshot.board
        let board = CGRect(x: 0, y: 0, width: b.width, height: b.height)
        viewport.fit(store.snapshot.courtyards.reduce(board) { $0.union($1.rect) }, in: size, margin: 50, limits: limits)
    }

    private func perform(_ command: ViewCommand, size: CGSize) {
        switch command {
        case .fit: fit(size)
        case .fitSelection:
            let rect = store.snapshot.courtyards.filter { store.selection.contains($0.component) }
                .reduce(CGRect.null) { $0.union($1.rect) }
            if rect.isNull { fit(size) } else { viewport.fit(rect.insetBy(dx: -2, dy: -2), in: size, limits: limits) }
        case .zoomArea: zoomArmed = true
        default:
            viewport.apply(command, size: size, anchor: hover, baseScale: Viewport.pcbBaseScale, limits: limits)
        }
    }

    private func footprint(at world: CGPoint) -> Int? {
        store.snapshot.courtyards.last { $0.rect.contains(world) }?.component
    }

    private func pad(at world: CGPoint) -> SnapPad? {
        store.snapshot.pads.first { $0.rect.insetBy(dx: -0.1, dy: -0.1).contains(world) }
    }

    /// The via, or else the track (active layer first, then the other visible copper layers), under `world`.
    private func copperHit(at world: CGPoint, vias: Bool = true) -> (id: Int, isVia: Bool)? {
        let snap = store.snapshot
        let slop = 3 / max(0.01, Double(viewport.scale))  // three points on screen
        let copperShown = (0..<max(1, snap.board.layerCount)).contains { visible.contains(.copper($0)) }
        if vias, copperShown,
           let v = snap.vias.last(where: { hypot($0.x - Double(world.x), $0.y - Double(world.y)) <= $0.diameter / 2 + slop }) {
            return (v.id, true)
        }
        let hits = snap.tracks.filter {
            visible.contains(.copper($0.layer)) && $0.distance(to: world) <= $0.width / 2 + slop
        }
        let active = activeLayer.copperIndex ?? 0
        if let t = hits.last(where: { $0.layer == active }) ?? hits.last { return (t.id, false) }
        return nil
    }

    /// Select-tool drag on copper: starts the router's drag session once the pointer has really moved, then the
    /// track (or via) follows the cursor. A refused drag (locked track, via in a pad) pans instead.
    private func copperDrag(_ id: Int, isVia: Bool, grab: CGPoint, value: DragGesture.Value) {
        if !copperDragActive {
            guard hypot(value.translation.width, value.translation.height) > 3 else { return }
            let started: Bool
            if isVia {
                started = store.beginViaDrag(id, at: grab)
            } else if store.selectedTracks.count > 1, store.selectedTracks.contains(id) {
                // Several selected tracks move together.
                started = store.beginMultiDrag(Array(store.selectedTracks).sorted(), at: grab)
            } else if let t = store.snapshot.tracks.first(where: { $0.id == id }), !t.isArc,
                      min(hypot(Double(grab.x) - t.ax, Double(grab.y) - t.ay), hypot(Double(grab.x) - t.bx, Double(grab.y) - t.by))
                          <= max(2 * t.width, 0.4),
                      store.beginCornerDrag(id, at: grab) {
                started = true  // pressed on a corner: the vertex follows the pointer
            } else {
                started = store.beginTrackDrag(id, at: grab)
            }
            guard started else {
                dragMode = .pan(CGSize(width: viewport.offset.width - value.translation.width,
                                       height: viewport.offset.height - value.translation.height))
                return
            }
            copperDragActive = true
        }
        store.moveRoute(to: viewport.toWorld(value.location))
    }

    /// Tune tool click: picks the track to lengthen (meanders go near the click).
    private func tuneClick(at world: CGPoint) {
        if let hit = copperHit(at: world, vias: false) {
            store.beginTune(track: hit.id, at: world)
        } else {
            store.statusMessage = "Tune length: click a routed track"
        }
    }

    /// Route tool click: the first click starts on a pad, via or track; later clicks place corners, and a click that
    /// reaches the net's pad (or a double-click) finishes the route.
    private func routeClick(at world: CGPoint) {
        guard store.routePreview != nil else {
            // Multi-route: ⇧-click picks start points; the next plain click adds its own and routes them together.
            if NSEvent.modifierFlags.contains(.shift) {
                store.toggleMultiStart(world)
                return
            }
            if !store.multiStarts.isEmpty {
                store.beginMultiRoute(adding: world, layer: activeLayer.copperIndex ?? 0)
                return
            }
            if store.routerBus {
                store.beginBus(at: world, layer: activeLayer.copperIndex ?? 0)
            } else {
                store.beginRoute(at: world, layer: activeLayer.copperIndex ?? 0, pair: routePair)
            }
            return
        }
        store.moveRouteNow(to: world)
        if (NSApp.currentEvent?.clickCount ?? 1) >= 2 || store.routePreview?.reachedTarget == true {
            store.finishRoute()
        } else if store.routePreview?.head.isEmpty == false {
            store.placeRouteCorner()
        }
    }

    private var drag: some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                if dragMode == nil {
                    let world = viewport.toWorld(value.startLocation)
                    if zoomArmed {
                        dragMode = .zoomBox
                    } else if spaceHeld {
                        spaceUsedForPan = true
                        dragMode = .pan(viewport.offset)
                    } else if tuneTool, !panMode, let hit = copperHit(at: world, vias: false) {
                        dragMode = .tuneDrag(hit.id, world)
                    } else if !panMode, !routeTool, !tuneTool, pad(at: world) == nil, let hit = copperHit(at: world) {
                        dragMode = hit.isVia ? .dragVia(hit.id, world) : .dragTrack(hit.id, world)
                    } else if !panMode, !routeTool, !tuneTool, let id = footprint(at: world) {
                        // ⇧ adds on release (select toggles); selecting here as well would toggle it straight back off.
                        let shift = NSEvent.modifierFlags.contains(.shift)
                        if !store.selection.contains(id) && !shift { store.select(component: id) }
                        dragMode = .move(shift ? store.selection.union([id]) : store.selection)
                    } else {
                        dragMode = .pan(viewport.offset)
                    }
                }
                switch dragMode {
                case .move:
                    dragDelta = CGSize(width: value.translation.width / viewport.scale,
                                       height: value.translation.height / viewport.scale)
                case .pan(let start):
                    viewport.offset = CGSize(width: start.width + value.translation.width,
                                             height: start.height + value.translation.height)
                case .zoomBox:
                    zoomRect = CGRect(origin: value.startLocation, size: .zero)
                        .union(CGRect(origin: value.location, size: .zero))
                case .dragTrack(let id, let grab):
                    copperDrag(id, isVia: false, grab: grab, value: value)
                case .dragVia(let id, let grab):
                    copperDrag(id, isVia: true, grab: grab, value: value)
                case .tuneDrag(let id, let start):
                    guard hypot(value.translation.width, value.translation.height) > 3 else { break }
                    if store.tuneSession?.track != id || store.tuneSession?.point != start { store.beginTune(track: id, at: start) }
                    store.dragTune(to: viewport.toWorld(value.location))
                case nil: break
                }
            }
            .onEnded { value in
                focused = true
                let moved = hypot(value.translation.width, value.translation.height) > 3
                if case .zoomBox = dragMode {
                    zoomArmed = false
                    if moved, let rect = zoomRect {
                        let a = viewport.toWorld(rect.origin)
                        let b = viewport.toWorld(CGPoint(x: rect.maxX, y: rect.maxY))
                        viewport.fit(CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero)),
                                     in: canvasSize, margin: 8, limits: limits)
                    } else {
                        viewport.zoom(by: 2, anchor: value.location, limits: limits)
                    }
                } else if copperDragActive {
                    copperDragActive = false
                    store.finishRoute(at: viewport.toWorld(value.location))
                } else if !moved && routeTool && !spaceHeld {
                    routeClick(at: viewport.toWorld(value.location))
                } else if !moved && tuneTool && !spaceHeld {
                    tuneClick(at: viewport.toWorld(value.location))
                } else if case .tuneDrag = dragMode {
                    // Drag-along tuning: the preview stays; Enter or Apply Tuning writes it.
                } else if !moved {
                    if !spaceHeld && !panMode {  // a Space-click or a Hand-tool click pans, it doesn't select
                        let world = viewport.toWorld(value.location)
                        let shift = NSEvent.modifierFlags.contains(.shift)
                        if case .dragTrack(let id, _) = dragMode {
                            store.selectTrack(id, extend: shift)  // a click on a track selects it for the track commands
                        } else {
                            store.selectTrack(nil, extend: shift)
                            store.select(component: footprint(at: world), extend: shift)
                        }
                    }
                } else if case .move(let ids) = dragMode {
                    let moves = ids.compactMap { id -> (id: Int, point: CGPoint)? in
                        guard let c = store.snapshot.component(id) else { return nil }
                        return (id, CGPoint(x: c.pcb.x + dragDelta.width, y: c.pcb.y + dragDelta.height))
                    }
                    store.moveFootprints(moves)
                    store.selection.formUnion(ids)
                }
                dragMode = nil
                dragDelta = .zero
                zoomRect = nil
            }
    }

    // MARK: - Drawing

    private func draw(_ ctx: inout GraphicsContext, size: CGSize) {
        let snap = store.snapshot
        let screen = CGAffineTransform(translationX: viewport.offset.width, y: viewport.offset.height)
            .scaledBy(x: viewport.scale, y: viewport.scale)
        let k = viewport.scale
        let moving: Set<Int> = { if case .move(let ids) = dragMode { return ids } else { return [] } }()
        let d = dragDelta
        func shifted(_ component: Int, _ p: CGPoint) -> CGPoint {
            moving.contains(component) ? CGPoint(x: p.x + d.width, y: p.y + d.height) : p
        }
        let hoveredNet: Int? = hover.flatMap { pad(at: viewport.toWorld($0)) }.flatMap { $0.net >= 0 ? $0.net : nil }
        // Only what is on screen is drawn (large boards); designators fade out when zoomed far out.
        let view = viewport.visibleWorldRect(in: size).insetBy(dx: -2, dy: -2)
        func onScreen(_ ax: Double, _ ay: Double, _ bx: Double, _ by: Double, pad: Double = 0) -> Bool {
            CGRect(x: min(ax, bx) - pad, y: min(ay, by) - pad, width: abs(bx - ax) + 2 * pad, height: abs(by - ay) + 2 * pad)
                .intersects(view)
        }
        let showDesignators = k >= 1.2
        let roles = Dictionary(snap.nets.map { ($0.index, $0.netRole) }, uniquingKeysWith: { a, _ in a })
        let layerTotal = max(1, snap.board.layerCount)
        func copper(_ net: Int, _ layer: Int) -> Color {
            colourByNet ? Theme.netColor(roles[net] ?? .signal, layer: layer, layerCount: layerTotal)
                        : Theme.copperColor(layer, layerCount: layerTotal)
        }

        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.pcbBackground))

        // Board (rectangle or custom outline) and mounting holes
        let boardRect = CGRect(x: 0, y: 0, width: snap.board.width, height: snap.board.height)
        let outline = snap.board.hasCustomOutline ? Path(snap.board.outlinePath)
                                                  : Path(roundedRect: boardRect, cornerRadius: 0.8)
        ctx.fill(outline.applying(screen), with: .color(Theme.boardFill))
        if visible.contains(.boardOutline) {
            ctx.stroke(outline.applying(screen), with: .color(Theme.boardEdge), lineWidth: 1.5)
            if !snap.board.hasCustomOutline {
                let keepout = boardRect.insetBy(dx: snap.board.edgeClearance, dy: snap.board.edgeClearance)
                ctx.stroke(Path(keepout).applying(screen), with: .color(Theme.boardEdge.opacity(0.25)),
                           style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
            }
        }
        drawGrid(&ctx, board: boardRect, view: view)

        // Copper pours (under tracks and pads), bottom-most first.
        for fill in snap.zoneFills.sorted(by: { $0.layer > $1.layer })
        where fill.layer < max(1, snap.board.layerCount) && visible.contains(.copper(fill.layer)) {
            let isActive = fill.layer == (activeLayer.copperIndex ?? 0)
            var path = Path()
            for r in fill.cgRects where r.intersects(view) { path.addRect(r) }
            let base = copper(fill.net, fill.layer)
            let highlight = hoveredNet == fill.net
            ctx.fill(path.applying(screen), with: .color(highlight ? Theme.iceBlue.opacity(0.35) : base.opacity(isActive ? 0.32 : 0.14)))
        }
        for hole in snap.board.holes {
            let keep = CGRect(x: hole.x - hole.keepout / 2, y: hole.y - hole.keepout / 2, width: hole.keepout, height: hole.keepout)
            let bore = CGRect(x: hole.x - hole.drill / 2, y: hole.y - hole.drill / 2, width: hole.drill, height: hole.drill)
            ctx.stroke(Path(ellipseIn: keep).applying(screen), with: .color(Theme.boardEdge.opacity(0.6)),
                       style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
            ctx.fill(Path(ellipseIn: bore).applying(screen), with: .color(Theme.pcbBackground))
            ctx.stroke(Path(ellipseIn: bore).applying(screen), with: .color(Theme.boardEdge), lineWidth: 1)
        }

        // While routing, the board's copper shows as the router has shoved it.
        let route = store.routePreview
        let tune = store.tuneSession?.preview.flatMap { $0.ok ? $0 : nil }
        let boardTracks: [SnapTrack]
        let boardVias: [SnapVia]
        if let route {
            let hiddenTracks = Set(route.hiddenTracks), hiddenVias = Set(route.hiddenVias)
            boardTracks = snap.tracks.filter { !hiddenTracks.contains($0.id) } + route.shovedTracks
            boardVias = snap.vias.filter { !hiddenVias.contains($0.id) } + route.shovedVias
        } else if let tune {
            // Length tuning preview: the meanders in place of the tracks they replace.
            let hiddenTracks = Set(tune.removedTracks)
            boardTracks = snap.tracks.filter { !hiddenTracks.contains($0.id) } + tune.addedTracks
            boardVias = snap.vias
        } else {
            boardTracks = snap.tracks
            boardVias = snap.vias
        }

        // Copper: bottom-most layers first, the active copper layer last so it sits on top.
        let layerCount = max(1, snap.board.layerCount)
        let activeCopper = activeLayer.copperIndex ?? 0
        let order = (0..<layerCount).reversed().filter { $0 != activeCopper } + [activeCopper]
        for layer in order where layer < layerCount {
            guard visible.contains(.copper(layer)) else { continue }
            let isActive = layer == activeCopper
            // Tracks are batched by colour and width: one stroke per batch instead of one per track (a large board
            // has tens of thousands of segments).
            var batches: [TrackBatch: Path] = [:]
            for t in boardTracks where t.layer == layer {
                if t.isArc {
                    let e = t.extent
                    guard onScreen(e.minX, e.minY, e.maxX, e.maxY, pad: t.width) else { continue }
                } else if !onScreen(t.ax, t.ay, t.bx, t.by, pad: t.width) {
                    continue
                }
                let key = TrackBatch(highlight: hoveredNet == t.net, role: colourByNet ? (roles[t.net] ?? .signal) : nil,
                                     microns: Int((t.width * 1000).rounded()))
                if t.isArc {
                    t.addCentreLine(to: &batches[key, default: Path()])
                } else {
                    batches[key, default: Path()].move(to: CGPoint(x: t.ax, y: t.ay))
                    batches[key, default: Path()].addLine(to: CGPoint(x: t.bx, y: t.by))
                }
            }
            // Highlighted net last (on top), the rest in a fixed order.
            for (key, path) in batches.sorted(by: { ($0.key.highlight ? 1 : 0, $0.key.microns, $0.key.role?.rawValue ?? "")
                                                    < ($1.key.highlight ? 1 : 0, $1.key.microns, $1.key.role?.rawValue ?? "") }) {
                let base = key.role.map { Theme.netColor($0, layer: layer, layerCount: layerTotal) }
                    ?? Theme.copperColor(layer, layerCount: layerTotal)
                ctx.stroke(path.applying(screen), with: .color(key.highlight ? Theme.iceBlue : base.opacity(isActive ? 1 : 0.45)),
                           style: StrokeStyle(lineWidth: max(1, Double(key.microns) / 1000 * k), lineCap: .round, lineJoin: .round))
            }
            for p in snap.pads where !p.throughHole && (p.layer ?? (p.bottom ? snap.board.bottomLayer : 0)) == layer
                && (moving.contains(p.component) || onScreen(p.x, p.y, p.x, p.y, pad: max(p.w, p.h))) {
                let r = CGRect(x: p.x - p.w / 2, y: p.y - p.h / 2, width: p.w, height: p.h)
                let moved = r.offsetBy(dx: moving.contains(p.component) ? d.width : 0, dy: moving.contains(p.component) ? d.height : 0)
                ctx.fill(Path(roundedRect: moved, cornerRadius: min(p.w, p.h) * 0.15).applying(screen),
                         with: .color(hoveredNet == p.net && p.net >= 0 ? Theme.iceBlue : (isActive ? Theme.pad : Theme.pad.opacity(0.45))))
            }
        }
        // Through-hole pads and through vias (every layer)
        if (0..<layerCount).contains(where: { visible.contains(.copper($0)) }) {
            for p in snap.pads where p.throughHole && (moving.contains(p.component) || onScreen(p.x, p.y, p.x, p.y, pad: max(p.w, p.h))) {
                let c = shifted(p.component, CGPoint(x: p.x, y: p.y))
                let r = CGRect(x: c.x - p.w / 2, y: c.y - p.h / 2, width: p.w, height: p.h)
                let shape = p.round ? Path(ellipseIn: r) : Path(r)
                ctx.fill(shape.applying(screen), with: .color(hoveredNet == p.net && p.net >= 0 ? Theme.iceBlue : Theme.pad))
                let hole = CGRect(x: c.x - p.drill / 2, y: c.y - p.drill / 2, width: p.drill, height: p.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
            for v in boardVias where onScreen(v.x, v.y, v.x, v.y, pad: v.diameter) {
                let r = CGRect(x: v.x - v.diameter / 2, y: v.y - v.diameter / 2, width: v.diameter, height: v.diameter)
                ctx.fill(Path(ellipseIn: r).applying(screen), with: .color(Theme.via))
                if !v.isThrough {
                    // Blind / buried / microvias: a ring marks the partial span (as CAD tools draw them).
                    ctx.stroke(Path(ellipseIn: r.insetBy(dx: -0.05, dy: -0.05)).applying(screen),
                               with: .color(v.kind == "microvia" ? Theme.iceBlue : Theme.lightBlue), lineWidth: 1)
                }
                let hole = CGRect(x: v.x - v.drill / 2, y: v.y - v.drill / 2, width: v.drill, height: v.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
        }

        // Multi-route start points picked so far.
        for p in store.multiStarts {
            let s = p.applying(screen)
            ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 7, y: s.y - 7, width: 14, height: 14)), with: .color(Theme.iceBlue),
                       lineWidth: 2)
        }

        // Tracks selected for the track commands.
        if !store.selectedTracks.isEmpty {
            var selected = Path()
            for t in boardTracks where store.selectedTracks.contains(t.id) { t.addCentreLine(to: &selected) }
            ctx.stroke(selected.applying(screen), with: .color(Theme.selection.opacity(0.9)),
                       style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))
        }

        // The route in progress on top: placed segments in their copper colour, the head following the cursor
        // outlined, and its vias.
        if let route {
            for t in route.placed + route.head {
                var path = Path()
                t.addCentreLine(to: &path)
                ctx.stroke(path.applying(screen), with: .color(copper(t.net, t.layer)),
                           style: StrokeStyle(lineWidth: max(1, t.width * k), lineCap: .round, lineJoin: .round))
            }
            for t in route.head {
                var path = Path()
                t.addCentreLine(to: &path)
                ctx.stroke(path.applying(screen), with: .color(route.blocked ? Theme.warning : Theme.iceBlue),
                           style: StrokeStyle(lineWidth: 1, lineCap: .round, lineJoin: .round))
            }
            for v in route.vias {
                let r = CGRect(x: v.x - v.diameter / 2, y: v.y - v.diameter / 2, width: v.diameter, height: v.diameter)
                ctx.fill(Path(ellipseIn: r).applying(screen), with: .color(Theme.via))
                if !v.isThrough {
                    ctx.stroke(Path(ellipseIn: r.insetBy(dx: -0.05, dy: -0.05)).applying(screen),
                               with: .color(v.kind == "microvia" ? Theme.iceBlue : Theme.lightBlue), lineWidth: 1)
                }
                let hole = CGRect(x: v.x - v.drill / 2, y: v.y - v.drill / 2, width: v.drill, height: v.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
        }
        // Highlight mode: what the route violates, in red.
        for c in route?.collisions ?? [] {
            var mark = Path()
            switch c.kind {
            case "track":
                mark.move(to: CGPoint(x: c.ax, y: c.ay))
                mark.addLine(to: CGPoint(x: c.bx, y: c.by))
                ctx.stroke(mark.applying(screen), with: .color(Theme.error.opacity(0.85)),
                           style: StrokeStyle(lineWidth: max(2, c.width * k), lineCap: .round))
                continue
            case "via", "hole":
                mark.addEllipse(in: CGRect(x: c.ax - c.width / 2, y: c.ay - c.width / 2, width: c.width, height: c.width))
            case "pad":
                mark.addRect(CGRect(x: c.ax - c.w / 2, y: c.ay - c.h / 2, width: c.w, height: c.h))
            default:
                break
            }
            if !mark.isEmpty { ctx.stroke(mark.applying(screen), with: .color(Theme.error), lineWidth: 2) }
            let s = CGPoint(x: c.x, y: c.y).applying(screen)
            ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 6, y: s.y - 6, width: 12, height: 12)), with: .color(Theme.error), lineWidth: 2)
        }
        if let tune {
            var outline = Path()
            for t in tune.addedTracks { t.addCentreLine(to: &outline) }
            ctx.stroke(outline.applying(screen), with: .color(tune.onTarget ? Theme.iceBlue : Theme.warning),
                       style: StrokeStyle(lineWidth: 1, lineCap: .round, lineJoin: .round))
        }

        // Silkscreen / courtyards / designators
        for cy in snap.courtyards where moving.contains(cy.component) || cy.rect.intersects(view) {
            let rect = cy.rect.offsetBy(dx: moving.contains(cy.component) ? d.width : 0, dy: moving.contains(cy.component) ? d.height : 0)
            let selected = store.selection.contains(cy.component)
            if visible.contains(.courtyard) || selected {
                ctx.stroke(Path(rect).applying(screen), with: .color(selected ? Theme.selection : Theme.lightBlue.opacity(0.35)),
                           style: StrokeStyle(lineWidth: selected ? 1.6 : 0.8, dash: selected ? [] : [3, 3]))
            }
            if selected {
                ctx.fill(Path(rect).applying(screen), with: .color(Theme.blue.opacity(0.12)))
            }
            if visible.contains(.silkscreen), let c = snap.component(cy.component) {
                let inner = rect.insetBy(dx: 0.15, dy: 0.15)
                ctx.stroke(Path(inner).applying(screen), with: .color(Theme.silkscreen.opacity(c.pcb.bottom ? 0.35 : 0.9)), lineWidth: max(0.6, 0.12 * k))
                guard showDesignators else { continue }
                // Designator above the part, like the silkscreen; the value inside it once there is room.
                let fontSize = max(8, min(15, 1.0 * k))
                ctx.draw(Text(c.ref).font(.system(size: fontSize, weight: .semibold, design: .monospaced))
                            .foregroundColor(Theme.silkscreen.opacity(c.pcb.bottom ? 0.5 : 1)),
                         at: CGPoint(x: rect.midX, y: rect.minY - 0.2).applying(screen), anchor: .bottom)
                if k >= 5, !c.value.isEmpty, rect.width * k > CGFloat(c.value.count) * 6 + 8 {
                    ctx.draw(Text(c.value).font(.system(size: max(7, min(11, 0.7 * k)), design: .monospaced))
                                .foregroundColor(Theme.silkscreen.opacity(0.6)),
                             at: CGPoint(x: rect.midX, y: rect.midY).applying(screen))
                }
            }
        }

        // Pad numbers (zoomed in) and net names along tracks, like professional CAD.
        if k >= 8 {
            for p in snap.pads where onScreen(p.x, p.y, p.x, p.y, pad: max(p.w, p.h)) && min(p.w, p.h) * k >= 12 {
                let c = shifted(p.component, CGPoint(x: p.x, y: p.y))
                ctx.draw(Text("\(p.number)").font(.system(size: max(7, min(12, min(p.w, p.h) * k * 0.45)), weight: .bold,
                                                         design: .monospaced))
                            .foregroundColor(Color.black.opacity(0.8)),
                         at: c.applying(screen))
            }
        }
        if k >= 4 {
            // One label per net and layer, on its longest visible segment that has room for the name.
            var best: [String: (SnapTrack, CGFloat)] = [:]
            for t in snap.tracks where !t.isArc && visible.contains(.copper(t.layer)) && onScreen(t.ax, t.ay, t.bx, t.by) {
                let length = CGFloat(hypot(t.bx - t.ax, t.by - t.ay)) * k
                let key = "\(t.net)/\(t.layer)"
                if length > (best[key]?.1 ?? 0) { best[key] = (t, length) }
            }
            for (t, length) in best.values {
                guard let net = snap.net(t.net) else { continue }
                let size = max(7, min(11, t.width * k * 0.9 + 3))
                guard length > CGFloat(net.name.count) * size * 0.62 + 14 else { continue }
                var angle = atan2(t.by - t.ay, t.bx - t.ax)
                if angle > .pi / 2 { angle -= .pi } else if angle <= -.pi / 2 { angle += .pi }  // keep text upright
                let mid = CGPoint(x: (t.ax + t.bx) / 2, y: (t.ay + t.by) / 2).applying(screen)
                var label = ctx
                label.translateBy(x: mid.x, y: mid.y)
                label.rotate(by: .radians(angle))
                let text = Text(net.name).font(.system(size: size, weight: .semibold, design: .monospaced))
                let width = CGFloat(net.name.count) * size * 0.62 + 6
                label.fill(Path(roundedRect: CGRect(x: -width / 2, y: -size * 0.65, width: width, height: size * 1.3),
                                cornerRadius: size * 0.3),
                           with: .color(Theme.boardFill.opacity(0.85)))
                label.draw(text.foregroundColor(copper(t.net, t.layer)), at: .zero)
            }
        }

        // Ratsnest
        if visible.contains(.ratsnest) {
            var rats = Path()
            for l in snap.ratsnest where onScreen(l.ax, l.ay, l.bx, l.by, pad: 0.5) {
                rats.move(to: CGPoint(x: l.ax, y: l.ay))
                rats.addLine(to: CGPoint(x: l.bx, y: l.by))
            }
            ctx.stroke(rats.applying(screen), with: .color(Theme.ratsnest), lineWidth: 1)
        }

        // DRC markers
        for v in store.drcResults where store.drcIsCurrent && v.severity == .error && v.hasLocation && v.code != "DRC_UNROUTED" {
            let s = CGPoint(x: v.x, y: v.y).applying(screen)
            ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 8, y: s.y - 8, width: 16, height: 16)), with: .color(Theme.error), lineWidth: 2)
        }

        if let r = zoomRect {
            ctx.fill(Path(r), with: .color(Theme.blue.opacity(0.12)))
            ctx.stroke(Path(r), with: .color(Theme.skyBlue), style: StrokeStyle(lineWidth: 1, dash: [5, 3]))
        }
        if zoomArmed {
            CanvasOverlays.banner("Zoom to area — drag a rectangle (click zooms 2×) · Esc cancels", in: &ctx, size: size)
        } else if let route {
            var length = String(format: "%.2f mm", route.length)
            if let net = route.netLength, let target = route.targetLength, target > 0 {
                length += String(format: " · net %.2f / %.2f mm", net, target)
            }
            let hint = route.kind == "drag" || route.kind == "via" ? "release to drop · Esc cancels"
                : "click places a corner · V via · Enter finishes · Esc cancels"
            CanvasOverlays.banner("\(route.status) · \(length) · \(hint)", in: &ctx, size: size)
            if let net = route.netLength, let target = route.targetLength, target > 0 {
                // Live length gauge of the routed net against its target.
                LengthGauge.draw(in: &ctx, rect: CGRect(x: size.width / 2 - 90, y: 40, width: 180, height: 8), length: net,
                                 target: target, tolerance: 0.1)
            }
        } else if tuneTool {
            if let preview = store.tuneSession?.preview {
                let name = snap.net(preview.net)?.name ?? "net"
                let group = preview.group.isEmpty ? "" : " · \(preview.groupKind == "pair" ? "pair" : "bus") \(preview.group)"
                let skew = preview.after - preview.target
                let text = preview.ok
                    ? String(format: "%@%@ · %.2f → %.2f mm · target %.2f mm (%+.2f, ±%.2f) · Enter applies · Esc cancels",
                             name, group, preview.before, preview.after, preview.target, skew, max(preview.tolerance, 0.01))
                    : "\(name) · \(preview.message)"
                CanvasOverlays.banner(text, in: &ctx, size: size)
                if preview.target > 0 {
                    LengthGauge.draw(in: &ctx, rect: CGRect(x: size.width / 2 - 90, y: 40, width: 180, height: 8),
                                     length: preview.after, target: preview.target, tolerance: max(preview.tolerance, 0.01))
                }
            } else {
                CanvasOverlays.banner("Tune length — click a track (meanders go near the click), or drag along it", in: &ctx, size: size)
            }
        } else if routeTool {
            CanvasOverlays.banner("Route — click a pad, via or track to start · \(store.routerBus ? "bus of \(store.routerBusWidth)" : routePair ? "differential pair" : "single track")",
                                  in: &ctx, size: size)
        }

        // Cursor read-out
        if let h = hover {
            let w = viewport.toWorld(h)
            var text = String(format: "X %.2f  Y %.2f mm", w.x, w.y)
            if let net = hoveredNet, let n = snap.net(net) { text += "  ·  \(n.name)" }
            ctx.draw(Text(text).font(.system(size: 11, design: .monospaced)).foregroundColor(Theme.skyBlue),
                     at: CGPoint(x: 12, y: size.height - 12), anchor: .bottomLeading)
        }
    }

    /// Dot grid on the board (1 mm at 100 %, coarser when zoomed out, 0.5/0.1 mm when zoomed in).
    private func drawGrid(_ ctx: inout GraphicsContext, board: CGRect, view: CGRect) {
        let pitch = viewport.gridPitch(base: 0.1, minimumPoints: 10)
        let area = board.intersection(view)
        guard !area.isNull, area.width > 0, area.height > 0 else { return }
        var dots = Path()
        var x = (area.minX / pitch).rounded(.up) * pitch
        while x <= area.maxX {
            var y = (area.minY / pitch).rounded(.up) * pitch
            while y <= area.maxY {
                let p = viewport.toScreen(CGPoint(x: x, y: y))
                dots.addRect(CGRect(x: p.x - 0.5, y: p.y - 0.5, width: 1, height: 1))
                y += pitch
            }
            x += pitch
        }
        ctx.fill(dots, with: .color(Theme.gridDot.opacity(0.7)))
    }
}
