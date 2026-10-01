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
    @State private var fitRequest = 0
    @State private var showLayersPanel = true

    @State private var boardWidth = ""
    @State private var boardHeight = ""
    @State private var trackWidth = ""
    @State private var clearance = ""

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move footprints (V)", isActive: !panMode) { panMode = false }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: panMode) { panMode = true }
                ToolStripDivider()
                ToolStripButton(systemImage: "rotate.right", help: "Rotate footprint (R)") { store.rotateFootprints() }
                ToolStripButton(systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right", help: "Flip to other side (F)") {
                    store.flipFootprints()
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "square.grid.3x3.topleft.filled", help: "Auto-place all footprints") {
                    store.autoPlace(all: true)
                }
                ToolStripButton(systemImage: "arrow.down.right.and.arrow.up.left.rectangle", help: "Fit board to components") {
                    store.fitBoard()
                    fitRequest += 1
                }
                ToolStripButton(systemImage: "point.topleft.down.to.point.bottomright.curvepath.fill", help: "Autoroute (⇧⌘R)") {
                    Task { await store.autoRoute() }
                }
                ToolStripButton(systemImage: "eraser", help: "Clear all tracks and vias") { store.clearRouting() }
                ToolStripButton(systemImage: "checkmark.seal", help: "Design rule check") {
                    store.runDRC()
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "square.3.layers.3d", help: "Layers panel", isActive: showLayersPanel) {
                    showLayersPanel.toggle()
                }
            }

            VStack(spacing: 0) {
                OptionsBar {
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
                    Divider().frame(height: 18)
                    HStack(spacing: 10) {
                        ruleField("Board W", $boardWidth, unit: "mm")
                        ruleField("H", $boardHeight, unit: "mm")
                        ruleField("Track", $trackWidth, unit: "mm")
                        ruleField("Clr", $clearance, unit: "mm")
                        Button("Apply") { applyRules() }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                    Spacer()
                    if let stats = store.routeStats {
                        Badge(text: "\(stats.routed)/\(stats.connections) routed · \(stats.vias) vias",
                              systemImage: stats.failed == 0 ? "checkmark.circle" : "exclamationmark.triangle")
                    }
                    let drcErrors = store.drcResults.filter { $0.severity == .error }.count
                    if !store.drcResults.isEmpty {
                        Badge(text: drcErrors == 0 ? "DRC clean" : "DRC \(drcErrors)", systemImage: "checkmark.seal")
                    }
                    ZoomControls(scale: viewport.scale / 12,
                                 zoomIn: { viewport.zoom(by: 1.25, anchor: center, limits: 1...200) },
                                 zoomOut: { viewport.zoom(by: 0.8, anchor: center, limits: 1...200) },
                                 fit: { fitRequest += 1 })
                }

                ZStack(alignment: .topTrailing) {
                    PCBCanvas(viewport: $viewport, canvasSize: $canvasSize, panMode: panMode, visible: visible,
                              activeLayer: activeLayer, fitRequest: fitRequest)
                    if showLayersPanel {
                        LayersPanel(layers: PCBLayer.all(for: store.snapshot.board), board: store.snapshot.board,
                                    visible: $visible, active: $activeLayer)
                            .padding(10)
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
                                       message: "Place the schematic's footprints automatically, then run the autorouter.",
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
        .onChange(of: store.snapshot.board) { _, board in
            syncRuleFields()
            if let index = activeLayer.copperIndex, index >= board.layerCount { activeLayer = .copper(0) }
        }
    }

    private var center: CGPoint { CGPoint(x: canvasSize.width / 2, y: canvasSize.height / 2) }

    private var unplacedCount: Int {
        store.snapshot.components.filter { !$0.componentKind.isVirtual && !$0.pcb.placed }.count
    }

    private func ruleField(_ title: String, _ text: Binding<String>, unit: String) -> some View {
        HStack(spacing: 4) {
            Text(title).foregroundStyle(Theme.textMuted)
            TextField(title, text: text)
                .textFieldStyle(.roundedBorder)
                .frame(width: 54)
                .onSubmit { applyRules() }
            Text(unit).foregroundStyle(Theme.textMuted).font(.caption)
        }
    }

    private func syncRuleFields() {
        let b = store.snapshot.board
        boardWidth = String(format: "%.1f", b.width)
        boardHeight = String(format: "%.1f", b.height)
        trackWidth = String(format: "%.2f", b.trackWidth)
        clearance = String(format: "%.2f", b.clearance)
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
                    RoundedRectangle(cornerRadius: 3).fill(layer.color(board)).frame(width: 12, height: 12)
                    Text(layer.name(board)).font(.caption)
                        .foregroundStyle(active == layer ? Theme.textPrimary : Theme.textSecondary)
                    Spacer(minLength: 0)
                }
                .padding(.vertical, 3)
                .padding(.horizontal, 6)
                .background(RoundedRectangle(cornerRadius: 5).fill(active == layer ? Theme.blue.opacity(0.3) : .clear))
                .contentShape(Rectangle())
                .onTapGesture { active = layer }
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
                    active = layer
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
    var panMode: Bool
    var visible: Set<PCBLayer>
    var activeLayer: PCBLayer
    var fitRequest: Int

    private enum DragMode {
        case move(Set<Int>)
        case pan(CGSize)
    }

    @State private var dragMode: DragMode?
    @State private var dragDelta: CGSize = .zero
    @State private var hover: CGPoint?
    @State private var magnifyBase: CGFloat?
    @State private var didFit = false
    @FocusState private var focused: Bool

    private let limits: ClosedRange<CGFloat> = 1...200

    var body: some View {
        GeometryReader { geo in
            Canvas(rendersAsynchronously: false) { ctx, size in draw(&ctx, size: size) }
                .contentShape(Rectangle())
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
                .onScrollWheel { event, point in viewport.handleScroll(event, at: point, limits: limits) }
                .onContinuousHover { phase in
                    switch phase {
                    case .active(let p): hover = p
                    case .ended: hover = nil
                    }
                }
                .focusable()
                .focusEffectDisabled()
                .focused($focused)
                .onKeyPress(KeyEquivalent("r")) { store.rotateFootprints(); return .handled }
                .onKeyPress(KeyEquivalent("f")) { store.flipFootprints(); return .handled }
                .onKeyPress(.escape) { store.select(component: nil); return .handled }
                .onAppear {
                    canvasSize = geo.size
                    if !didFit {
                        didFit = true
                        fit(geo.size)
                    }
                }
                .onChange(of: geo.size) { _, s in canvasSize = s }
                .onChange(of: fitRequest) { _, _ in fit(geo.size) }
                .onChange(of: store.fitToken) { _, _ in fit(geo.size) }
        }
    }

    private func fit(_ size: CGSize) {
        let b = store.snapshot.board
        viewport.fit(CGRect(x: 0, y: 0, width: b.width, height: b.height), in: size, margin: 50, limits: limits)
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
                    if !panMode, let id = footprint(at: world) {
                        if !store.selection.contains(id) {
                            store.select(component: id, extend: NSEvent.modifierFlags.contains(.shift))
                        }
                        dragMode = .move(store.selection)
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
                case nil: break
                }
            }
            .onEnded { value in
                focused = true
                if hypot(value.translation.width, value.translation.height) <= 3 {
                    let world = viewport.toWorld(value.location)
                    store.select(component: footprint(at: world), extend: NSEvent.modifierFlags.contains(.shift))
                } else if case .move(let ids) = dragMode {
                    for id in ids {
                        guard let c = store.snapshot.component(id) else { continue }
                        store.moveFootprint(id, to: CGPoint(x: c.pcb.x + dragDelta.width, y: c.pcb.y + dragDelta.height))
                    }
                }
                dragMode = nil
                dragDelta = .zero
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

        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.pcbBackground))

        // Board
        let boardRect = CGRect(x: 0, y: 0, width: snap.board.width, height: snap.board.height)
        ctx.fill(Path(roundedRect: boardRect, cornerRadius: 0.8).applying(screen), with: .color(Theme.boardFill))
        if visible.contains(.boardOutline) {
            ctx.stroke(Path(roundedRect: boardRect, cornerRadius: 0.8).applying(screen), with: .color(Theme.boardEdge), lineWidth: 1.5)
            let keepout = boardRect.insetBy(dx: snap.board.edgeClearance, dy: snap.board.edgeClearance)
            ctx.stroke(Path(keepout).applying(screen), with: .color(Theme.boardEdge.opacity(0.25)),
                       style: StrokeStyle(lineWidth: 1, dash: [4, 4]))
        }

        // Copper: bottom-most layers first, the active copper layer last so it sits on top.
        let layerCount = max(1, snap.board.layerCount)
        let activeCopper = activeLayer.copperIndex ?? 0
        let order = (0..<layerCount).reversed().filter { $0 != activeCopper } + [activeCopper]
        for layer in order where layer < layerCount {
            guard visible.contains(.copper(layer)) else { continue }
            let isActive = layer == activeCopper
            let base = Theme.copperColor(layer, layerCount: layerCount)
            for t in snap.tracks where t.layer == layer {
                var path = Path()
                path.move(to: CGPoint(x: t.ax, y: t.ay))
                path.addLine(to: CGPoint(x: t.bx, y: t.by))
                let highlight = hoveredNet == t.net
                ctx.stroke(path.applying(screen), with: .color(highlight ? Theme.iceBlue : base.opacity(isActive ? 1 : 0.45)),
                           style: StrokeStyle(lineWidth: max(1, t.width * k), lineCap: .round, lineJoin: .round))
            }
            for p in snap.pads where !p.throughHole && (p.bottom ? snap.board.bottomLayer : 0) == layer {
                let r = CGRect(x: p.x - p.w / 2, y: p.y - p.h / 2, width: p.w, height: p.h)
                let moved = r.offsetBy(dx: moving.contains(p.component) ? d.width : 0, dy: moving.contains(p.component) ? d.height : 0)
                ctx.fill(Path(roundedRect: moved, cornerRadius: min(p.w, p.h) * 0.15).applying(screen),
                         with: .color(hoveredNet == p.net && p.net >= 0 ? Theme.iceBlue : (isActive ? Theme.pad : Theme.pad.opacity(0.45))))
            }
        }
        // Through-hole pads and through vias (every layer)
        if (0..<layerCount).contains(where: { visible.contains(.copper($0)) }) {
            for p in snap.pads where p.throughHole {
                let c = shifted(p.component, CGPoint(x: p.x, y: p.y))
                let r = CGRect(x: c.x - p.w / 2, y: c.y - p.h / 2, width: p.w, height: p.h)
                let shape = p.round ? Path(ellipseIn: r) : Path(r)
                ctx.fill(shape.applying(screen), with: .color(hoveredNet == p.net && p.net >= 0 ? Theme.iceBlue : Theme.pad))
                let hole = CGRect(x: c.x - p.drill / 2, y: c.y - p.drill / 2, width: p.drill, height: p.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
            for v in snap.vias {
                let r = CGRect(x: v.x - v.diameter / 2, y: v.y - v.diameter / 2, width: v.diameter, height: v.diameter)
                ctx.fill(Path(ellipseIn: r).applying(screen), with: .color(Theme.via))
                let hole = CGRect(x: v.x - v.drill / 2, y: v.y - v.drill / 2, width: v.drill, height: v.drill)
                ctx.fill(Path(ellipseIn: hole).applying(screen), with: .color(Theme.pcbBackground))
            }
        }

        // Silkscreen / courtyards / designators
        for cy in snap.courtyards {
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
                let fontSize = max(7, min(14, 1.0 * k))
                ctx.draw(Text(c.ref).font(.system(size: fontSize, weight: .semibold, design: .monospaced))
                            .foregroundColor(Theme.silkscreen.opacity(c.pcb.bottom ? 0.5 : 1)),
                         at: CGPoint(x: rect.midX, y: rect.minY).applying(screen), anchor: .bottom)
            }
        }

        // Ratsnest
        if visible.contains(.ratsnest) {
            var rats = Path()
            for l in snap.ratsnest {
                rats.move(to: CGPoint(x: l.ax, y: l.ay))
                rats.addLine(to: CGPoint(x: l.bx, y: l.by))
            }
            ctx.stroke(rats.applying(screen), with: .color(Theme.ratsnest), lineWidth: 1)
        }

        // DRC markers
        for v in store.drcResults where v.severity == .error && v.hasLocation && v.code != "DRC_UNROUTED" {
            let s = CGPoint(x: v.x, y: v.y).applying(screen)
            ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 8, y: s.y - 8, width: 16, height: 16)), with: .color(Theme.error), lineWidth: 2)
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
}
