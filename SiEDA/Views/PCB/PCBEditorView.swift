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
    @State private var showLayersPanel = true
    @State private var showBoardSetup = false

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
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move footprints (V)", isActive: !panMode) { panMode = false }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: panMode) { panMode = true }
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
                    Divider().frame(height: 18)
                    Image(systemName: "square.3.layers.3d.down.right").foregroundStyle(Theme.blue)
                    Picker("Layers", selection: Binding(get: { store.snapshot.board.layerCount },
                                                        set: { store.setLayerCount($0) })) {
                        Text("1").tag(1)
                        Text("2").tag(2)
                        Text("4").tag(4)
                        Text("6").tag(6)
                    }
                    .pickerStyle(.segmented)
                    .frame(width: 150)
                    .help("Copper layers: 1 = single-sided (no vias), 2, 4 or 6-layer stack-up")
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
                    PCBCanvas(viewport: $viewport, canvasSize: $canvasSize, panMode: $panMode, visible: visible,
                              activeLayer: activeLayer)
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
        .onChange(of: store.snapshot.board) { old, board in
            syncRuleFields(changedFrom: old)
            if let index = activeLayer.copperIndex, index >= board.layerCount { activeLayer = .copper(0) }
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
        store.snapshot.components.filter { !$0.componentKind.isVirtual && !$0.pcb.placed }.count
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

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
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
struct PCBCanvas: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var viewport: Viewport
    @Binding var canvasSize: CGSize
    @Binding var panMode: Bool
    var visible: Set<PCBLayer>
    var activeLayer: PCBLayer

    private enum DragMode {
        case move(Set<Int>)
        case pan(CGSize)
        case zoomBox
    }

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
                    case .active(let p): hover = p
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
                .onKeyPress(keys: ["r", "f", "v", "h"], phases: .down) { press in
                    guard press.modifiers.subtracting(.shift).isEmpty else { return .ignored }
                    switch press.key {
                    case KeyEquivalent("r"): store.rotateFootprints()
                    case KeyEquivalent("f"): store.flipFootprints()
                    case KeyEquivalent("v"): panMode = false
                    case KeyEquivalent("h"): panMode = true
                    default: return .ignored
                    }
                    return .handled
                }
                .onKeyPress(.escape) {
                    if zoomArmed { zoomArmed = false } else { store.select(component: nil) }
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
                    } else if !panMode, let id = footprint(at: world) {
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
                } else if !moved {
                    if !spaceHeld && !panMode {  // a Space-click or a Hand-tool click pans, it doesn't select
                        let world = viewport.toWorld(value.location)
                        store.select(component: footprint(at: world), extend: NSEvent.modifierFlags.contains(.shift))
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
        let showDesignators = k >= 2.5

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
            let base = Theme.copperColor(fill.layer, layerCount: snap.board.layerCount)
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

        // Copper: bottom-most layers first, the active copper layer last so it sits on top.
        let layerCount = max(1, snap.board.layerCount)
        let activeCopper = activeLayer.copperIndex ?? 0
        let order = (0..<layerCount).reversed().filter { $0 != activeCopper } + [activeCopper]
        for layer in order where layer < layerCount {
            guard visible.contains(.copper(layer)) else { continue }
            let isActive = layer == activeCopper
            let base = Theme.copperColor(layer, layerCount: layerCount)
            for t in snap.tracks where t.layer == layer && onScreen(t.ax, t.ay, t.bx, t.by, pad: t.width) {
                var path = Path()
                path.move(to: CGPoint(x: t.ax, y: t.ay))
                path.addLine(to: CGPoint(x: t.bx, y: t.by))
                let highlight = hoveredNet == t.net
                ctx.stroke(path.applying(screen), with: .color(highlight ? Theme.iceBlue : base.opacity(isActive ? 1 : 0.45)),
                           style: StrokeStyle(lineWidth: max(1, t.width * k), lineCap: .round, lineJoin: .round))
            }
            for p in snap.pads where !p.throughHole && (p.bottom ? snap.board.bottomLayer : 0) == layer
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
            for v in snap.vias where onScreen(v.x, v.y, v.x, v.y, pad: v.diameter) {
                let r = CGRect(x: v.x - v.diameter / 2, y: v.y - v.diameter / 2, width: v.diameter, height: v.diameter)
                ctx.fill(Path(ellipseIn: r).applying(screen), with: .color(Theme.via))
                let hole = CGRect(x: v.x - v.drill / 2, y: v.y - v.drill / 2, width: v.drill, height: v.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
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
                let fontSize = max(7, min(14, 1.0 * k))
                ctx.draw(Text(c.ref).font(.system(size: fontSize, weight: .semibold, design: .monospaced))
                            .foregroundColor(Theme.silkscreen.opacity(c.pcb.bottom ? 0.5 : 1)),
                         at: CGPoint(x: rect.midX, y: rect.minY).applying(screen), anchor: .bottom)
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
