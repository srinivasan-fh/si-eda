import AppKit
import UniformTypeIdentifiers
import SwiftUI

/// Interactive schematic canvas: drawing, hit-testing, selection, moving, wiring and placement.
struct SchematicCanvas: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var tool: SchematicTool
    @Binding var viewport: Viewport
    @Binding var canvasSize: CGSize
    /// "R1.2" while a wire is being drawn (shown as a hint by the editor), nil otherwise.
    @Binding var wireStart: String?
    /// Tool armed by P: the device last chosen in the picker.
    var placementTool: SchematicTool = .place(.resistor)
    /// Live (real-time) simulation: probes show its voltages, LEDs glow, switches are clicked.
    @ObservedObject var live: LiveSimulation
    /// Colour scheme and grid (Schematic Canvas preferences).
    @Environment(\.schematicStyle) private var style

    private enum DragMode {
        case move(Set<Int>)
        case pan(CGSize)
        case marquee
        case zoomBox
        case wire(WireEnd)     // press on a pin (or, with the wire tool, a wire) and drag to a pin, wire or point
        case bend(Int)         // drag a wire to bend it through a new junction
        case segment(Int, CGPoint)  // drag a straight wire sideways (it stays straight)
        case wireEnd(Int, Bool)     // drag the selected wire's end (b: true) off its pin
        case moveBus(Int)      // drag a bus (and its entries)
        case livePress(Int)    // a switch held while the live simulation runs
        case drawGraphic(CGPoint)  // drag out a line / arrow / section rectangle from this point
        case moveGraphic(Int)      // drag a note or drawing
    }

    @State private var dragMode: DragMode?
    @State private var dragDelta: CGSize = .zero          // world units, for live move preview
    @State private var marquee: CGRect?
    @State private var pendingWire: WireEnd?
    @State private var hover: CGPoint?
    @State private var magnifyBase: CGFloat?
    @State private var didInitialFit = ViewMemory.value("sch.fitted", false)  // the first fit only, also across window rebuilds
    @State private var placementRotation = 0  // Space / R rotate the part being placed (degrees)
    @State private var spaceHeld = false        // Space + left-drag pans; a Space tap rotates
    @State private var spaceUsedForPan = false
    @State private var zoomArmed = false   // Z: the next drag defines the area to zoom to
    @State private var busPoints: [CGPoint] = []  // corners of the bus being drawn
    @State private var namingBus: [CGPoint]?     // a finished bus waiting for its name
    @State private var busName = "D[0..7]"
    @State private var flipCorner = false     // '/': the wire being drawn turns vertical-first
    @State private var notePoint: CGPoint?    // a note being typed (where it goes)
    @State private var editingNote: Int?      // a note whose text is being edited
    @State private var noteText = ""
    // Display settings (View options menu): what the canvas draws.
    @AppStorage(SchematicGrid.pinNumbersKey) private var showPinNumbers = true
    @AppStorage(SchematicGrid.pinNamesKey) private var showPinNames = true
    @AppStorage(SchematicGrid.valuesKey) private var showValues = true
    @AppStorage(SchematicGrid.refsKey) private var showRefs = true
    @AppStorage(SchematicGrid.stepKey) private var gridStep = 10.0
    @State private var moveAlone = false      // ⌥-drag: the parts move without their wires
    @FocusState private var focused: Bool

    private let scaleLimits = Viewport.schematicLimits

    var body: some View {
        GeometryReader { geo in
            canvasLayer(geo)
            // A half-drawn wire, an armed zoom box or a placement rotation never outlives its tool.
            .onChange(of: tool) { _, _ in
                if case .pin(let end)? = pendingWire { store.cancelWire(at: end) }
                pendingWire = nil
                zoomArmed = false
                flipCorner = false
                placementRotation = 0
                busPoints = []
                focused = true
            }
            .alert("Bus Name", isPresented: Binding(get: { namingBus != nil }, set: { if !$0 { namingBus = nil } })) {
                TextField("D[0..7]", text: $busName)
                Button("OK") {
                    if let points = namingBus { store.addBus(named: busName, points: points) }
                    namingBus = nil
                }
                Button("Cancel", role: .cancel) { namingBus = nil }
            } message: {
                Text("Bus notation: D[0..7], A[15..0] or a list such as D[0..3],WR,RD.")
            }
            .alert(editingNote == nil ? LocalizedStringKey("Note") : LocalizedStringKey("Edit Note"),
                   isPresented: Binding(get: { notePoint != nil || editingNote != nil },
                                        set: { if !$0 { notePoint = nil; editingNote = nil } })) {
                TextField("Text", text: $noteText)
                Button("OK") {
                    if let id = editingNote { store.setSheetGraphicText(id, noteText) }
                    else if let at = notePoint, !noteText.trimmingCharacters(in: .whitespaces).isEmpty {
                        store.addSheetGraphic(kind: "text", points: [at], text: noteText)
                    }
                    notePoint = nil
                    editingNote = nil
                }
                Button("Cancel", role: .cancel) { notePoint = nil; editingNote = nil }
            }
            .onAppear {
                canvasSize = geo.size
                focused = true
                if !didInitialFit {
                    didInitialFit = true
                    ViewMemory.set("sch.fitted", true)
                    fitToContent(size: geo.size)
                }
            }
            .settledSize(geo.size, into: $canvasSize)
            .onChange(of: store.viewRequest) { _, request in
                if let request { perform(request.command, size: geo.size) }
            }
            .onChange(of: store.fitToken) { _, _ in fitToContent(size: geo.size) }
            .onChange(of: pendingWire) { _, end in wireStart = end.flatMap { describe($0) } }
            .onChange(of: store.revision) { _, _ in
                // Undo or delete can remove the part or wire a wire was started from.
                if let end = pendingWire, endPoint(end) == nil { pendingWire = nil }
            }
            // A design appearing at once (example, AI plan, paste) is fitted; placing the first part by hand is not.
            .onChange(of: store.snapshot.components.count) { old, new in
                if old == 0 && new > 1 { fitToContent(size: geo.size) }
            }
        }
    }

    /// The canvas with its input handling; split from `body` so the type checker handles each chain in time.
    private func canvasLayer(_ geo: GeometryProxy) -> some View {
            Canvas(rendersAsynchronously: false) { ctx, size in
                draw(&ctx, size: size)
            }
            .contentShape(Rectangle())
            .accessibilityElement()
            .accessibilityLabel("Schematic canvas")
            .accessibilityValue("\(store.sheetSnapshot.components.count) components, \(store.sheetSnapshot.wires.count) wires, "
                + "\(store.selection.count) selected")
            .gesture(dragGesture)
            .simultaneousGesture(
                MagnifyGesture()
                    .onChanged { value in
                        if magnifyBase == nil { magnifyBase = viewport.scale }
                        let target = (magnifyBase ?? 1) * value.magnification
                        viewport.zoom(by: target / viewport.scale, anchor: value.startLocation, limits: scaleLimits)
                    }
                    .onEnded { _ in magnifyBase = nil }
            )
            .canvasMouseInput(onScroll: { event, point in viewport.handleScroll(event, at: point, limits: scaleLimits) },
                              onPan: { viewport.pan(by: $0) },
                              onRightClick: { contextMenu(at: viewport.toWorld($0)) })
            .onContinuousHover { phase in
                switch phase {
                case .active(let p):
                    // The pointer as its grid point (pin highlights, the ghost, the rubber band and the readout snap to
                    // the grid): the sheet is redrawn when the pointer reaches another grid point, not on every pixel.
                    let snapped = viewport.toScreen(SchematicGrid.snap(viewport.toWorld(p)))
                    if hover != snapped { hover = snapped }
                case .ended: hover = nil
                }
            }
            .focusable()
            .focusEffectDisabled()
            .focused($focused)
            .canvasNavigationKeys(toggleNavigator: { store.showNavigator.toggle() }, nudge: { dx, dy in
                // Arrow keys move the selected parts a grid step (⇧: five) instead of panning.
                guard !store.selection.isEmpty, tool == .select else { return false }
                store.moveComponents(store.selection, by: CGSize(width: dx * 10, height: dy * 10))
                return true
            }) { command in
                perform(command, size: geo.size)
            }
            .onCommand(#selector(NSText.selectAll(_:))) { store.selectAllOnSheet() }
            .onKeyPress(.space, phases: [.down, .repeat, .up]) { press in
                switch press.phase {
                case .down:
                    spaceHeld = true
                    spaceUsedForPan = false
                case .up:
                    spaceHeld = false
                    if !spaceUsedForPan { rotate() }  // a tap rotates; Space + drag panned instead
                default:
                    break
                }
                return .handled
            }
            .onChange(of: focused) { _, isFocused in if !isFocused { spaceHeld = false } }
            .onKeyPress(.escape) {
                zoomArmed = false
                if case .pin(let end)? = pendingWire { store.cancelWire(at: end) }
                pendingWire = nil
                tool = .select
                store.select(component: nil)
                return .handled
            }
            .onKeyPress(.return) {
                guard tool == .bus, busPoints.count >= 2 else { return .ignored }
                finishBus()
                return .handled
            }
            // Edit ▸ Copy / Cut / Paste on the focused canvas: parts and the wires between them.
            .onCopyCommand { store.selectionClip().map { [NSItemProvider(object: $0 as NSString)] } ?? [] }
            .onCutCommand {
                let items = store.selectionClip().map { [NSItemProvider(object: $0 as NSString)] } ?? []
                store.deleteSelection()
                return items
            }
            .onPasteCommand(of: [.plainText]) { providers in
                guard let provider = providers.first else { return }
                _ = provider.loadObject(ofClass: NSString.self) { object, _ in
                    guard let text = object as? String else { return }
                    DispatchQueue.main.async { store.paste(clip: text) }
                }
            }
            .onKeyPress(keys: [.leftArrow, .rightArrow, .upArrow, .downArrow], phases: .down) { press in
                // ⌥⌘ arrows align the selection's edges (by their outlines) left / right / top / bottom.
                guard press.modifiers == [.command, .option], store.selection.count >= 2 else { return .ignored }
                let mode = press.key == .leftArrow ? "left" : press.key == .rightArrow ? "right" : press.key == .upArrow ? "top" : "bottom"
                store.align(mode, byOutline: true)
                return .handled
            }
            .onKeyPress(keys: [.upArrow], phases: .down) { press in
                // ⌘↑: up to the parent sheet (the one whose sheet symbol this sheet is).
                guard press.modifiers == .command, let parent = store.snapshot.sheet(store.snapshot.activeSheet)?.parent,
                      parent > 0 else { return .ignored }
                store.selectSheet(parent)
                return .handled
            }
            .onKeyPress(.delete) { deleteTargets(); return .handled }
            .onKeyPress(.deleteForward) { deleteTargets(); return .handled }
            // ⌘D duplicates the selection beside it with new designators.
            .onKeyPress(keys: ["d"], phases: .down) { press in
                guard press.modifiers == .command else { return .ignored }
                store.duplicateSelection()
                return .handled
            }
            // Single-letter tool keys (Settings → Shortcuts); ⌘/⌥/⌃ combinations belong to menus and text editing.
            .onKeyPress(phases: .down) { press in
                guard press.modifiers.subtracting(.shift).isEmpty,
                      let key = ShortcutStore.shared.toolKey(press.characters, canvas: "sch") else { return .ignored }
                if key == ShortcutStore.sequenceStarted { return .handled }  // the first key of a two-key tool sequence
                switch key {
                case KeyEquivalent("r"): rotate()
                case KeyEquivalent("x"): store.mirror(store.targets(hovered: hoveredComponent), vertical: false)
                case KeyEquivalent("y"): store.mirror(store.targets(hovered: hoveredComponent), vertical: true)
                case KeyEquivalent("/"): flipCorner.toggle()  // the wire being drawn turns the other way
                case KeyEquivalent("t"): tool = .graphic("text")
                case KeyEquivalent("w"): tool = .wire
                case KeyEquivalent("b"): tool = .bus
                case KeyEquivalent("q"): tool = .noConnect
                case KeyEquivalent("v"): tool = .select
                case KeyEquivalent("h"): tool = .pan
                case KeyEquivalent("g"): tool = .place(.ground)
                case KeyEquivalent("l"): tool = .place(.netLabel)
                case KeyEquivalent("p"): tool = placementTool
                default: return .ignored
                }
                return .handled
            }
    }

    // MARK: - Navigation

    /// World-space bounds of every component symbol (used for fit, zoom to selection and the navigator).
    static func componentBounds(_ snapshot: DesignSnapshot) -> [(id: Int, rect: CGRect)] {
        snapshot.components.map { c in
            let rect = SchematicSymbols.bounds(c.componentKind, value: c.value, custom: snapshot.customPart(for: c))
                .applying(SchematicSymbols.transform(position: c.position, rotation: c.rotation, mirror: c.isMirrored))
            return (c.id, rect)
        }
    }

    /// Space / R: rotates the part being placed (before the click) or, if components are selected, the selection
    /// by 90°. With nothing selected it does nothing.
    private func rotate() {
        switch tool {
        case .place, .placeCustom: placementRotation = (placementRotation + 90) % 360
        default: store.rotate(store.targets(hovered: hoveredComponent))  // the selection, else the part under the pointer
        }
    }

    /// The part under the pointer (hover + shortcut acts on it when nothing is selected).
    private var hoveredComponent: Int? {
        guard let h = hover else { return nil }
        return component(at: viewport.toWorld(h))
    }

    /// Delete / ⌫: the selected wire, bus or parts, else the part under the pointer.
    private func deleteTargets() {
        if store.selectedWire != nil || store.selectedBus != nil || !store.selection.isEmpty || !store.boxedWires.isEmpty { store.deleteSelection() }
        else if let id = hoveredComponent { store.delete([id]) }
    }

    /// The right-click menu for what is at `world`: a part, a wire, or the empty sheet.
    private func contextMenu(at world: CGPoint) -> [CanvasMenuItem] {
        if let id = component(at: world), let c = store.sheetSnapshot.component(id) {
            if !store.selection.contains(id) { store.select(component: id) }
            let ids = store.selection
            let editSymbol: [CanvasMenuItem] = c.customPart.map { part in
                [CanvasMenuItem(title: String(localized: "Edit Symbol…")) {  // its pins and drawing, in the library
                    store.libraryFocusPartId = part
                    store.workspace = .library
                }]
            } ?? []
            let pinMap: [CanvasMenuItem] = c.pins.count >= 8 ? [  // MCU / FPGA: firmware and constraint files
                CanvasMenuItem(title: String(localized: "Plan Pins…")) { store.pinPlannerRef = c.ref },
                CanvasMenuItem(title: String(localized: "Export Pin Map…")) { store.exportPinMap(ref: c.ref) },
                CanvasMenuItem(title: String(localized: "Assign Pins from File…")) { store.assignPins(ref: c.ref) },
            ] : []
            return [
                CanvasMenuItem(title: String(localized: "Edit Properties…")) { store.quickEdit = id },
            ] + editSymbol + pinMap + [
                .separator,
                CanvasMenuItem(title: String(localized: "Rotate"), key: "r") { store.rotate(ids) },
                CanvasMenuItem(title: String(localized: "Mirror Horizontally"), key: "x") { store.mirror(ids, vertical: false) },
                CanvasMenuItem(title: String(localized: "Mirror Vertically"), key: "y") { store.mirror(ids, vertical: true) },
                CanvasMenuItem(title: String(localized: "Duplicate")) { store.duplicateSelection() },
                CanvasMenuItem(title: String(localized: "Copy")) { store.copySelection() },
                .separator,
                CanvasMenuItem(title: String(localized: "Select Same Value")) { store.selectSameValue() },
                CanvasMenuItem(title: String(localized: "Select Connected Parts")) { store.selectConnected() },
                CanvasMenuItem(title: String(localized: "Connect Touching Pins and Wires")) { store.connectTouching(ids) },
                CanvasMenuItem(title: c.isLocked ? String(localized: "Unlock") : String(localized: "Lock")) { store.setLocked(ids, !c.isLocked) },
                c.componentKind == .netLabel
                    ? CanvasMenuItem(title: c.isPower ? String(localized: "Show as Label") : String(localized: "Show as Power Symbol")) {
                        store.setPowerStyle(ids, !c.isPower)
                    }
                    : .separator,
                .separator,
                CanvasMenuItem(title: String(localized: "Delete")) { store.delete(ids) },
            ]
        }
        if let g = graphicHit(at: world), let info = store.sheetSnapshot.sheetGraphics.first(where: { $0.id == g }) {
            store.select(component: nil)
            store.selectedGraphic = g
            var items: [CanvasMenuItem] = []
            if info.kind == "text" {
                items.append(CanvasMenuItem(title: String(localized: "Edit Text…")) {
                    noteText = info.text ?? ""
                    editingNote = g
                })
            }
            items.append(CanvasMenuItem(title: String(localized: "Delete")) { store.deleteSelection() })
            return items
        }
        if let w = wire(at: world) {
            store.select(component: nil)
            store.selectedWire = w
            return [
                CanvasMenuItem(title: String(localized: "Select Connected Parts")) { store.selectConnected() },
                CanvasMenuItem(title: String(localized: "Clean Up Wires")) { store.cleanupWires() },
                .separator,
                CanvasMenuItem(title: String(localized: "Delete Wire")) { store.deleteSelection() },
            ]
        }
        return [
            CanvasMenuItem(title: String(localized: "Paste Here"), enabled: store.canPaste) { store.paste(at: world) },
            CanvasMenuItem(title: String(localized: "Select All")) { store.selectAllOnSheet() },
            CanvasMenuItem(title: String(localized: "Clean Up Wires")) { store.cleanupWires() },
            .separator,
            CanvasMenuItem(title: String(localized: "Zoom to Fit")) { store.requestView(.fit) },
            CanvasMenuItem(title: String(localized: "Zoom to Sheet Frame"), enabled: sheetFrame() != nil) {
                if let frame = sheetFrame() { viewport.fit(frame.insetBy(dx: -20, dy: -20), in: canvasSize, limits: scaleLimits) }
            },
        ]
    }

    private func perform(_ command: ViewCommand, size: CGSize) {
        switch command {
        case .fit: fitToContent(size: size)
        case .fitSelection: fitSelection(size: size)
        case .zoomArea: zoomArmed = true
        case .focus(let p):
            if viewport.scale < Viewport.schematicBaseScale { viewport.setScale(Viewport.schematicBaseScale * 1.5, anchor: CGPoint(x: size.width / 2, y: size.height / 2), limits: scaleLimits) }
            viewport.center(on: p, in: size)
        default:
            viewport.apply(command, size: size, anchor: hover, baseScale: Viewport.schematicBaseScale, limits: scaleLimits)
        }
    }

    private func fitToContent(size: CGSize) {
        let rect = Self.componentBounds(store.sheetSnapshot).reduce(CGRect.null) { $0.union($1.rect) }
        guard !rect.isNull else { return }
        viewport.fit(rect.insetBy(dx: -30, dy: -30), in: size, limits: scaleLimits)
    }

    private func fitSelection(size: CGSize) {
        var rect = Self.componentBounds(store.sheetSnapshot).filter { store.selection.contains($0.id) }
            .reduce(CGRect.null) { $0.union($1.rect) }
        if let id = store.selectedWire, let w = store.sheetSnapshot.wires.first(where: { $0.id == id }) {
            rect = rect.union(CGRect(origin: w.start, size: .zero).union(CGRect(origin: w.end, size: .zero)))
        }
        guard !rect.isNull else { return fitToContent(size: size) }
        viewport.fit(rect.insetBy(dx: -40, dy: -40), in: size, limits: scaleLimits)
    }

    private var pickTolerance: CGFloat { max(4, 7 / viewport.scale) }

    /// The pin under `world`. Junctions count as pins when wiring; in the select tool they are dragged instead.
    private func pin(at world: CGPoint, includeJunctions: Bool = true) -> (PinAddress, CGPoint)? {
        var best: (PinAddress, CGPoint, CGFloat)?
        for c in store.sheetSnapshot.components where includeJunctions || c.componentKind != .junction {
            for (i, p) in c.pins.enumerated() {
                let d = hypot(CGFloat(p.x) - world.x, CGFloat(p.y) - world.y)
                if d <= pickTolerance, d < (best?.2 ?? .greatestFiniteMagnitude) {
                    best = (PinAddress(component: c.id, pin: i), p.point, d)
                }
            }
        }
        return best.map { ($0.0, $0.1) }
    }

    private func component(at world: CGPoint) -> Int? {
        // Selection filters (View options): parts, and labels / power / ground symbols, separately.
        let parts = SchematicGrid.flag(SchematicGrid.pickPartsKey), labels = SchematicGrid.flag(SchematicGrid.pickLabelsKey)
        for c in store.sheetSnapshot.components.reversed() {
            let isLabel = c.componentKind == .netLabel || c.componentKind == .ground
            if isLabel ? !labels : !parts { continue }
            let t = SchematicSymbols.transform(position: c.position, rotation: c.rotation, mirror: c.isMirrored).inverted()
            if SchematicSymbols.bounds(c.componentKind, value: c.value, custom: store.sheetSnapshot.customPart(for: c)).contains(world.applying(t)) {
                return c.id
            }
        }
        return nil
    }

    private static func wirePath(_ a: CGPoint, _ b: CGPoint) -> [CGPoint] { WireGeometry.path(a, b) }

    /// Drawing templates (A4 … ANSI E), read once.
    static let templates: [SheetTemplateInfo] = EDAEngine.sheetTemplates()

    /// The wire under `world` and the point on it (on the grid) where a T-junction would go.
    private func wireHit(at world: CGPoint) -> (id: Int, point: CGPoint)? {
        // Wires stay pickable while one is being drawn (T-junctions); the filter only stops selecting them.
        if !SchematicGrid.flag(SchematicGrid.pickWiresKey), pendingWire == nil, tool == .select { return nil }
        var best: (id: Int, point: CGPoint, distance: CGFloat)?
        for w in store.sheetSnapshot.wires {
            let hit = WireGeometry.nearestPoint(on: w, to: world)
            if hit.distance <= pickTolerance, hit.distance < (best?.distance ?? .greatestFiniteMagnitude) {
                best = (w.id, hit.point, hit.distance)
            }
        }
        return best.map { ($0.id, $0.point) }
    }

    private func wire(at world: CGPoint) -> Int? { wireHit(at: world)?.id }

    private func pinPosition(_ address: PinAddress) -> CGPoint? {
        guard let c = store.sheetSnapshot.component(address.component), address.pin < c.pins.count else { return nil }
        return c.pins[address.pin].point
    }

    /// World position of a wire end (nil once its pin or wire is gone).
    private func endPoint(_ end: WireEnd) -> CGPoint? {
        switch end {
        case .pin(let address): return pinPosition(address)
        case .wire(let id, let near):
            return store.sheetSnapshot.wires.first { $0.id == id }.map { WireGeometry.nearestPoint(on: $0, to: near).point }
        case .point(let p): return SchematicGrid.snap(p)
        }
    }

    /// "R1.2" for the editor's wiring hint.
    private func describe(_ end: WireEnd) -> String? {
        switch end {
        case .pin(let address):
            guard let c = store.sheetSnapshot.component(address.component), address.pin < c.pins.count else { return nil }
            return c.componentKind == .junction ? "a wire corner" : "\(c.ref).\(c.pins[address.pin].name)"
        case .wire: return "a wire"
        case .point: return nil
        }
    }

    /// Junctions act as pins while wiring (wire tool, or a wire already started); the select tool drags them.
    private var wiringJunctions: Bool { tool == .wire || pendingWire != nil }

    /// Where a wire released or clicked at `world` ends: a pin, else a wire (T-junction), else a free corner.
    private func wireEnd(at world: CGPoint) -> WireEnd {
        if let address = pin(at: world)?.0 { return .pin(address) }
        if let hit = wireHit(at: world) { return .wire(hit.id, hit.point) }
        return .point(world)
    }

    /// Finishes the wire from `start` at `end`; a free corner keeps drawing from there.
    private func finishWire(from start: WireEnd, to end: WireEnd) {
        if end == start { pendingWire = nil; return }  // the start clicked again: dropped
        // '/' turned the corner the other way: run vertically first through a bend, then on to the end.
        if flipCorner, let a = endPoint(start), let b = endPoint(end), a.x != b.x, a.y != b.y,
           let bend = store.drawWire(from: start, to: .point(CGPoint(x: a.x, y: b.y))) {
            finishWire(from: .pin(bend), to: end)
            return
        }
        let reached = store.drawWire(from: start, to: end)
        if case .point = end, let reached {
            pendingWire = .pin(reached)
        } else {
            pendingWire = nil
        }
    }

    // MARK: - Interaction

    private var dragGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                if dragMode == nil { beginDrag(at: value.startLocation) }
                switch dragMode {
                case .move, .moveBus, .moveGraphic:
                    dragDelta = CGSize(width: value.translation.width / viewport.scale,
                                       height: value.translation.height / viewport.scale)
                case .drawGraphic:
                    hover = value.location
                case .pan(let start):
                    viewport.offset = CGSize(width: start.width + value.translation.width,
                                             height: start.height + value.translation.height)
                case .marquee, .zoomBox:
                    marquee = CGRect(origin: value.startLocation, size: .zero)
                        .union(CGRect(origin: value.location, size: .zero))
                case .wire, .bend, .segment, .wireEnd:
                    hover = value.location  // hover events stop during a drag; keep the rubber band on the cursor
                case .livePress, nil:
                    break
                }
            }
            .onEnded { value in
                focused = true
                if case .livePress(let id) = dragMode {  // release a held push-button
                    live.press(id, pressed: false)
                    dragMode = nil
                    return
                }
                let moved = hypot(value.translation.width, value.translation.height) > 3
                if case .zoomBox = dragMode {
                    zoomArmed = false
                    if moved, let rect = marquee {
                        let a = viewport.toWorld(rect.origin)
                        let b = viewport.toWorld(CGPoint(x: rect.maxX, y: rect.maxY))
                        viewport.fit(CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero)),
                                     in: canvasSize, margin: 8, limits: scaleLimits)
                    } else {
                        viewport.zoom(by: 2, anchor: value.location, limits: scaleLimits)
                    }
                } else if !moved {
                    if !spaceHeld { click(at: value.location) }
                } else {
                    switch dragMode {
                    case .move(let ids):
                        let snapped = SchematicGrid.snap(dragDelta)
                        store.moveComponents(ids, by: snapped, detach: moveAlone)
                        store.selection.formUnion(ids)
                    case .wire(let start):
                        // Onto a pin or a wire (T-junction) finishes it; in empty space it turns a corner there.
                        let end = wireEnd(at: viewport.toWorld(value.location))
                        if end == start { pendingWire = start } else { finishWire(from: start, to: end) }
                    case .bend(let id):
                        store.bendWire(id, at: viewport.toWorld(value.location))
                    case .segment(let id, let start):
                        let p = viewport.toWorld(value.location)
                        store.dragWireSegment(id, by: SchematicGrid.snap(CGSize(width: p.x - start.x, height: p.y - start.y)))
                    case .wireEnd(let id, let isB):
                        let p = SchematicGrid.snap(viewport.toWorld(value.location))
                        if case .pin(let address) = wireEnd(at: p) { store.moveWireEnd(id, b: isB, to: address, at: p) }
                        else { store.moveWireEnd(id, b: isB, to: nil, at: p) }
                    case .moveBus(let id):
                        store.moveBus(id, by: SchematicGrid.snap(dragDelta))
                    case .moveGraphic(let id):
                        store.moveSheetGraphic(id, by: SchematicGrid.snap(dragDelta))
                    case .drawGraphic(let start):
                        if case .graphic(let kind) = tool {
                            store.addSheetGraphic(kind: kind, points: [start, viewport.toWorld(value.location)])
                        }
                    case .marquee:
                        if let rect = marquee {
                            let a = viewport.toWorld(rect.origin)
                            let b = viewport.toWorld(CGPoint(x: rect.maxX, y: rect.maxY))
                            let worldRect = CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero))
                            // ⇧-drag adds every part the box touches to the selection.
                            let boxed = Self.componentBounds(store.sheetSnapshot).filter { $0.rect.intersects(worldRect) }.map(\.id)
                            store.selection.formUnion(boxed)
                            store.selectedWire = nil
                            // Wires with both ends in the box are selected too (Delete removes them with the parts).
                            store.boxedWires = Set(store.sheetSnapshot.wires.filter { worldRect.contains($0.start) && worldRect.contains($0.end) }.map(\.id))
                        }
                    default:
                        break
                    }
                }
                dragMode = nil
                dragDelta = .zero
                marquee = nil
            }
    }

    private func beginDrag(at screen: CGPoint) {
        if zoomArmed {
            dragMode = .zoomBox
            return
        }
        if spaceHeld {
            spaceUsedForPan = true
            dragMode = .pan(viewport.offset)
            return
        }
        let world = viewport.toWorld(screen)
        // While the board runs live, switches and push-buttons are operated, not edited.
        if live.isRunning, let id = component(at: world), store.sheetSnapshot.component(id)?.componentKind == .switchSPST {
            live.press(id, pressed: true)
            dragMode = .livePress(id)
            return
        }
        switch tool {
        case .select, .wire:
            let shift = NSEvent.modifierFlags.contains(.shift)
            if let address = pin(at: world, includeJunctions: wiringJunctions)?.0 {
                dragMode = .wire(pendingWire ?? .pin(address))
            } else if tool == .wire {
                // The wire tool starts a wire anywhere on another wire (a T-junction there).
                if let hit = wireHit(at: world) { dragMode = .wire(pendingWire ?? .wire(hit.id, hit.point)) }
                else { dragMode = .pan(viewport.offset) }
            } else if let id = component(at: world) {
                // ⇧ adds on release (`click` toggles); selecting here as well would toggle it straight back off.
                if !store.selection.contains(id) && !shift { store.select(component: id) }
                moveAlone = NSEvent.modifierFlags.contains(.option)  // ⌥-drag: move without the wires
                let moving = shift ? store.selection.union([id]) : store.selection
                dragMode = moving.isDisjoint(with: store.lockedParts) ? .move(moving) : .pan(viewport.offset)
            } else if !shift, tool == .select, pendingWire == nil, let g = graphicHit(at: world) {
                store.select(component: nil)
                store.selectedGraphic = g
                dragMode = .moveGraphic(g)  // a note or drawing moves as one
            } else if !shift, pendingWire == nil, let hit = wireHit(at: world) {
                let w = store.sheetSnapshot.wires.first { $0.id == hit.id }
                let a = w.flatMap { pinPosition($0.a) }, b = w.flatMap { pinPosition($0.b) }
                if store.selectedWire == hit.id, let a, let b, min(hypot(world.x - a.x, world.y - a.y), hypot(world.x - b.x, world.y - b.y)) <= pickTolerance {
                    dragMode = .wireEnd(hit.id, hypot(world.x - b.x, world.y - b.y) < hypot(world.x - a.x, world.y - a.y))
                } else if let a, let b, (a.x == b.x) != (a.y == b.y), !NSEvent.modifierFlags.contains(.option) {
                    dragMode = .segment(hit.id, world)  // a straight wire moves sideways; ⌥ bends it instead
                } else {
                    dragMode = .bend(hit.id)  // pull a wire into shape
                }
            } else if !shift, tool == .select, let bus = busHit(at: world) {
                store.select(component: nil)
                store.selectedBus = bus
                dragMode = .moveBus(bus)
            } else if NSEvent.modifierFlags.contains(.shift) {
                dragMode = .marquee
            } else {
                dragMode = .pan(viewport.offset)
            }
        case .graphic(let kind) where kind != "text":
            dragMode = .drawGraphic(SchematicGrid.snap(world))
        default:
            dragMode = .pan(viewport.offset)
        }
    }

    private func click(at screen: CGPoint) {
        let world = viewport.toWorld(screen)
        switch tool {
        case .place(let kind):
            store.addComponent(kind, at: world, rotation: placementRotation)
        case .placeCustom(let partId):
            store.addCustomComponent(partId: partId, at: world, rotation: placementRotation)
        case .pan:
            break
        case .bus:
            let p = SchematicGrid.snap(world)
            if let last = busPoints.last, hypot(last.x - p.x, last.y - p.y) < 1 {
                if busPoints.count >= 2 { finishBus() }
            } else {
                busPoints.append(p)
            }
        case .noConnect:
            if let address = pin(at: world)?.0 { store.toggleNoConnect(address) }
        case .graphic(let kind):
            if kind == "text" {
                noteText = ""
                notePoint = SchematicGrid.snap(world)
            }
        case .select, .wire:
            // Double-click inside a sheet symbol opens that sheet (⌘↑ goes back up to its parent).
            if tool == .select, pendingWire == nil, (NSApp.currentEvent?.clickCount ?? 1) >= 2,
               let child = sheetSymbol(at: world) {
                store.selectSheet(child)
                return
            }
            if let address = pin(at: world, includeJunctions: wiringJunctions)?.0 {
                if let start = pendingWire {
                    finishWire(from: start, to: .pin(address))
                } else {
                    pendingWire = .pin(address)
                }
                return
            }
            if let start = pendingWire {
                if let hit = wireHit(at: world) {
                    finishWire(from: start, to: .wire(hit.id, hit.point))  // T-junction on that wire
                } else if tool == .wire || isJunction(start) {
                    finishWire(from: start, to: .point(world))  // a corner; keep drawing
                } else {
                    pendingWire = nil  // select tool: a click in empty space drops the wire just started
                }
                return
            }
            if tool == .wire {
                if let hit = wireHit(at: world) { pendingWire = .wire(hit.id, hit.point) }
                return
            }
            if let id = component(at: world), (NSApp.currentEvent?.clickCount ?? 1) >= 2 {
                store.select(component: id)
                store.quickEdit = id  // double-click: edit its designator and value (a label's name) in place
                return
            }
            if let id = component(at: world) {
                // Clicking a switch's lever flips it (like the real thing); the rest of its body just selects.
                if !NSEvent.modifierFlags.contains(.shift), let c = store.sheetSnapshot.component(id), c.componentKind == .switchSPST {
                    let local = world.applying(SchematicSymbols.transform(position: c.position, rotation: c.rotation, mirror: c.isMirrored).inverted())
                    if abs(local.x) <= 16 && abs(local.y) <= 14 { store.toggleSwitch(id) }
                }
                store.select(component: id, extend: NSEvent.modifierFlags.contains(.shift))
            } else if let g = graphicHit(at: world) {
                store.select(component: nil)
                store.selectedGraphic = g
                if (NSApp.currentEvent?.clickCount ?? 1) >= 2,
                   let note = store.sheetSnapshot.sheetGraphics.first(where: { $0.id == g && $0.kind == "text" }) {
                    noteText = note.text ?? ""
                    editingNote = g
                }
            } else if let w = wire(at: world) {
                store.selection = []
                store.selectedBus = nil
                store.selectedWire = w
            } else if let bus = busHit(at: world) {
                store.select(component: nil)
                store.selectedBus = bus
            } else {
                store.select(component: nil)
            }
        }
    }

    /// The bus under `world` (buses are drawn thick: a little more tolerance than wires).
    private func busHit(at world: CGPoint) -> Int? {
        var best: (id: Int, distance: CGFloat)?
        for bus in store.sheetSnapshot.buses {
            let d = bus.nearest(to: world).distance
            if d <= pickTolerance * 1.5, d < (best?.distance ?? .greatestFiniteMagnitude) { best = (bus.id, d) }
        }
        return best?.id
    }

    /// The child sheet whose sheet symbol (the box round its entries) contains `world`.
    private func sheetSymbol(at world: CGPoint) -> Int? {
        let snap = store.sheetSnapshot
        for child in snap.sheets where child.parent == snap.activeSheet {
            let entries = snap.components.filter { $0.labelScope == "entry" && $0.targetSheet == child.id }
            guard !entries.isEmpty else { continue }
            var box = CGRect.null
            for e in entries {
                box = box.union(SchematicSymbols.bounds(.netLabel, value: e.value, custom: nil)
                    .applying(SchematicSymbols.transform(position: e.position, rotation: e.rotation)))
            }
            let left = entries.map { CGFloat($0.x) }.min() ?? box.minX
            let frame = CGRect(x: left, y: box.minY - 16, width: max(80, box.maxX + 16 - left, CGFloat(child.symbolWidth ?? 0)),
                               height: max(box.height + 26, CGFloat(child.symbolHeight ?? 0)))
            if frame.contains(world) { return child.id }
        }
        return nil
    }

    /// The drawing template's frame of the shown sheet (nil without a template).
    private func sheetFrame() -> CGRect? {
        let snap = store.sheetSnapshot
        guard let shown = snap.sheet(snap.activeSheet), let size = shown.size, !size.isEmpty,
              let template = Self.templates.first(where: { $0.name == size }) else { return nil }
        let w = template.width / 0.254, h = template.height / 0.254
        if let fx = shown.frameX, let fy = shown.frameY { return CGRect(x: fx, y: fy, width: w, height: h) }
        let content = Self.componentBounds(snap).reduce(CGRect.null) { $0.union($1.rect) }
        let center = content.isNull ? CGPoint.zero : CGPoint(x: content.midX, y: content.midY)
        return CGRect(x: center.x - w / 2, y: center.y - h / 2, width: w, height: h)
    }

    /// The note or drawing under `world`.
    private func graphicHit(at world: CGPoint) -> Int? {
        guard SchematicGrid.flag(SchematicGrid.pickDrawingsKey) else { return nil }
        var best: (id: Int, distance: CGFloat)?
        for g in store.sheetSnapshot.sheetGraphics {
            let d = g.distance(to: world)
            if d <= pickTolerance, d < (best?.distance ?? .greatestFiniteMagnitude) { best = (g.id, d) }
        }
        return best?.id
    }

    /// Ends the bus being drawn and asks for its name.
    private func finishBus() {
        guard busPoints.count >= 2 else { return }
        namingBus = busPoints
        busPoints = []
    }

    private func isJunction(_ end: WireEnd) -> Bool {
        if case .pin(let address) = end { return store.sheetSnapshot.component(address.component)?.componentKind == .junction }
        return false
    }

    // MARK: - Drawing

    /// How brightly an LED is lit (0…1, full at 15 mA): from the live board, otherwise from the shown DC result.
    private func ledBrightness(_ id: Int) -> Double {
        if live.isRunning { return live.led(id)?.brightness ?? 0 }
        guard store.showDCOverlay, let dc = store.dcResult, dc.converged,
              let device = dc.devices.first(where: { $0.component == id }) else { return 0 }
        return min(1, max(0, device.current / 0.015))
    }

    /// Glow colour of an LED from its value ("Red", "Green 0805", "Blue", …).
    static func ledColour(_ value: String) -> Color { ComponentKind.ledColour(value).color }

    private func draw(_ ctx: inout GraphicsContext, size: CGSize) {
        let snap = store.sheetSnapshot
        let pal = style.palette
        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(pal.background.color))
        drawGrid(&ctx, size: size)

        let screen = CGAffineTransform(translationX: viewport.offset.width, y: viewport.offset.height)
            .scaledBy(x: viewport.scale, y: viewport.scale)
        let movingIds: Set<Int> = { if case .move(let ids) = dragMode { return ids } else { return [] } }()
        let delta = dragDelta
        // Only what is on screen is drawn; pins and text fade out when zoomed far out (large designs).
        let view = viewport.visibleWorldRect(in: size).insetBy(dx: -40 / viewport.scale - 20, dy: -40 / viewport.scale - 20)
        let showPins = viewport.scale >= 0.3
        let showLabels = viewport.scale >= 0.25

        func pinPoint(_ address: PinAddress) -> CGPoint? {
            guard var p = pinPosition(address) else { return nil }
            if movingIds.contains(address.component) { p.x += delta.width; p.y += delta.height }
            return p
        }

        // Wires (orthogonal L-routes) and junctions.
        let junctionIds = Set(snap.components.filter { $0.componentKind == .junction }.map(\.id))
        var wiresAt: [Int: Int] = [:]  // junction id → wire ends on it
        for w in snap.wires {
            for end in [w.a, w.b] where junctionIds.contains(end.component) { wiresAt[end.component, default: 0] += 1 }
        }
        let bending: (id: Int, point: CGPoint)? = {
            guard case .bend(let id) = dragMode, let h = hover else { return nil }
            return (id, SchematicGrid.snap(viewport.toWorld(h)))
        }()
        var endpointCount: [SIMD2<Int>: (CGPoint, Int)] = [:]  // integer keys: no string per wire end per frame
        let highlighted = store.highlightedNets  // a selected wire lights its whole net
        for w in snap.wires {
            guard let a = pinPoint(w.a), let b = pinPoint(w.b),
                  CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero)).insetBy(dx: -1, dy: -1).intersects(view)
                    || bending?.id == w.id
            else { continue }
            var path = Path()
            if let bending, bending.id == w.id {  // the wire being pulled into shape
                path.addLines(Self.wirePath(a, bending.point))
                path.addLines(Self.wirePath(bending.point, b))
            } else if case .segment(let id, let start) = dragMode, id == w.id, let h = hover {  // moved sideways
                let p = viewport.toWorld(h), d = SchematicGrid.snap(CGSize(width: p.x - start.x, height: p.y - start.y))
                let off = a.y == b.y ? CGSize(width: 0, height: d.height) : CGSize(width: d.width, height: 0)
                path.addLines([a, CGPoint(x: a.x + off.width, y: a.y + off.height), CGPoint(x: b.x + off.width, y: b.y + off.height), b])
            } else if case .wireEnd(let id, let isB) = dragMode, id == w.id, let h = hover {  // an end pulled off its pin
                path.addLines(Self.wirePath(isB ? a : b, SchematicGrid.snap(viewport.toWorld(h))))
            } else {
                path.addLines(Self.wirePath(a, b))
            }
            let selected = store.selectedWire == w.id || store.boxedWires.contains(w.id) || (w.net >= 0 && highlighted.contains(w.net))
            if selected {
                ctx.stroke(path.applying(screen), with: .color(pal.selectionHalo.color.opacity(0.6)), lineWidth: 7)
            }
            ctx.stroke(path.applying(screen), with: .color(selected ? pal.selection.color : pal.wire.color),
                       style: StrokeStyle(lineWidth: 1.8, lineCap: .round, lineJoin: .round))
            // Two wires on one pin make a T with the pin's lead (a dot); junctions draw their own.
            for (end, p) in [(w.a, a), (w.b, b)] where !junctionIds.contains(end.component) {
                let key = SIMD2(Int(p.x), Int(p.y))
                endpointCount[key] = (p, (endpointCount[key]?.1 ?? 0) + 1)
            }
        }
        for (_, entry) in endpointCount where entry.1 >= 2 {
            let s = entry.0.applying(screen)
            ctx.fill(Path(ellipseIn: CGRect(x: s.x - 3.5, y: s.y - 3.5, width: 7, height: 7)), with: .color(pal.junction.color))
        }
        // Junctions: a dot where three or more wires meet, nothing on a plain bend, an open circle on a loose end.
        for c in snap.components where c.componentKind == .junction {
            var p = c.position
            if movingIds.contains(c.id) { p.x += delta.width; p.y += delta.height }
            guard view.contains(p) else { continue }
            let s = p.applying(screen)
            let count = wiresAt[c.id] ?? 0
            if store.selection.contains(c.id) {
                ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 7, y: s.y - 7, width: 14, height: 14)), with: .color(pal.selection.color), lineWidth: 2)
            }
            if count >= 3 {
                ctx.fill(Path(ellipseIn: CGRect(x: s.x - 4, y: s.y - 4, width: 8, height: 8)), with: .color(pal.junction.color))
            } else if count <= 1 {
                ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 3.5, y: s.y - 3.5, width: 7, height: 7)),
                           with: .color(pal.unconnectedPin.color), lineWidth: 1.4)
            } else if let h = hover, tool == .select, hypot(h.x - s.x, h.y - s.y) < 10 {
                // A bend shows a handle under the pointer: drag it to reshape the wire.
                ctx.stroke(Path(CGRect(x: s.x - 3.5, y: s.y - 3.5, width: 7, height: 7)), with: .color(pal.preview.color), lineWidth: 1.4)
            }
        }

        // Graphical buses: thick lines with their name; each entry is a short diagonal from the bus to its label.
        let movingBus: Int? = { if case .moveBus(let id) = dragMode { return id } else { return nil } }()
        for bus in snap.buses {
            let offset = movingBus == bus.id ? delta : .zero
            let pts = bus.path.map { CGPoint(x: $0.x + offset.width, y: $0.y + offset.height) }
            guard pts.count >= 2 else { continue }
            var path = Path()
            path.addLines(pts)
            let selected = store.selectedBus == bus.id
            if selected { ctx.stroke(path.applying(screen), with: .color(pal.selectionHalo.color.opacity(0.6)), lineWidth: 10) }
            ctx.stroke(path.applying(screen), with: .color(selected ? pal.selection.color : pal.bus.color),
                       style: StrokeStyle(lineWidth: 4, lineCap: .round, lineJoin: .round))
            if showLabels {
                ctx.draw(Text(verbatim: bus.name).font(.system(size: max(8, min(13, 9 * viewport.scale / 1.6)), weight: .bold, design: .monospaced))
                            .foregroundColor(pal.bus.color),
                         at: CGPoint(x: pts[0].x + 4, y: pts[0].y - 6).applying(screen), anchor: .bottomLeading)
            }
        }
        // Notes and drawings (not part of the circuit): text, lines, arrows and section rectangles.
        let movingGraphic: Int? = { if case .moveGraphic(let id) = dragMode { return id } else { return nil } }()
        for g in snap.sheetGraphics {
            let off = movingGraphic == g.id ? delta : .zero
            let pts = g.path.map { CGPoint(x: $0.x + off.width, y: $0.y + off.height) }
            guard let first = pts.first else { continue }
            let picked = store.selectedGraphic == g.id
            let colour = picked ? pal.selection.color : pal.symbol.color
            if g.kind == "text" {
                guard showLabels else { continue }
                var y = first.y
                for line in (g.text ?? "").split(separator: "\n", omittingEmptySubsequences: false) {
                    ctx.draw(Text(verbatim: String(line)).font(.system(size: max(7, 10 * viewport.scale / 1.6)))
                                .foregroundColor(colour), at: CGPoint(x: first.x, y: y).applying(screen), anchor: .topLeading)
                    y += 12
                }
                if picked { ctx.stroke(Path(g.bounds.offsetBy(dx: off.width, dy: off.height).applying(screen)), with: .color(colour), lineWidth: 0.8) }
                continue
            }
            guard pts.count == 2 else { continue }
            var path = Path()
            if g.kind == "rect" {
                path.addRect(CGRect(origin: pts[0], size: .zero).union(CGRect(origin: pts[1], size: .zero)))
            } else {
                path.move(to: pts[0])
                path.addLine(to: pts[1])
                if g.kind == "arrow" {
                    let dx = pts[1].x - pts[0].x, dy = pts[1].y - pts[0].y, len = max(1e-6, hypot(dx, dy))
                    let ux = dx / len, uy = dy / len
                    path.move(to: CGPoint(x: pts[1].x - ux * 8 - uy * 3.5, y: pts[1].y - uy * 8 + ux * 3.5))
                    path.addLine(to: pts[1])
                    path.addLine(to: CGPoint(x: pts[1].x - ux * 8 + uy * 3.5, y: pts[1].y - uy * 8 - ux * 3.5))
                }
            }
            ctx.stroke(path.applying(screen), with: .color(colour),
                       style: StrokeStyle(lineWidth: picked ? 2 : 1.2, dash: g.kind == "rect" ? [6, 4] : []))
        }
        if case .drawGraphic(let start) = dragMode, case .graphic(let kind) = tool, let h = hover {
            let end = SchematicGrid.snap(viewport.toWorld(h))
            var path = Path()
            if kind == "rect" { path.addRect(CGRect(origin: start, size: .zero).union(CGRect(origin: end, size: .zero))) }
            else { path.move(to: start); path.addLine(to: end) }
            ctx.stroke(path.applying(screen), with: .color(pal.preview.color), style: StrokeStyle(lineWidth: 1.2, dash: [4, 3]))
        }
        for c in snap.components where c.bus != nil {
            guard let busId = c.bus, let bus = snap.bus(busId) else { continue }
            let offset = movingBus == busId ? delta : (movingIds.contains(c.id) ? delta : .zero)
            let at = CGPoint(x: c.x + offset.width, y: c.y + offset.height)
            var q = bus.nearest(to: c.position).point
            if movingBus == busId { q.x += delta.width; q.y += delta.height }
            var stub = Path()
            stub.move(to: q.applying(screen))
            stub.addLine(to: at.applying(screen))
            ctx.stroke(stub, with: .color(pal.bus.color), lineWidth: 2)
        }
        // Harness connectors: each entry is joined to its harness label by a thin harness-coloured stub.
        for c in snap.components where c.harnessOf != nil {
            guard let owner = c.harnessOf, let harness = snap.component(owner) else { continue }
            var a = harness.position, b = c.position
            if movingIds.contains(owner) { a.x += delta.width; a.y += delta.height }
            if movingIds.contains(c.id) { b.x += delta.width; b.y += delta.height }
            var stub = Path()
            stub.move(to: a.applying(screen))
            stub.addLine(to: CGPoint(x: a.x, y: b.y).applying(screen))
            stub.addLine(to: b.applying(screen))
            ctx.stroke(stub, with: .color(pal.harness.color.opacity(0.7)), lineWidth: 1.5)
        }
        // Harness connector bodies: a box around each harness label's entries, notched towards the harness.
        drawHarnessBodies(&ctx, snap: snap, screen: screen, moving: movingIds, delta: delta)
        // Net directives: a small flag beside the pin they sit on (net class, ⇄ for a differential pair, sizes).
        if showLabels, !snap.directives.isEmpty {
            // The block part's pin on its own sheet, or its copy's on a channel sheet shown now (one pass, not per directive).
            var byBlock: [Int: SnapComponent] = [:]
            for c in snap.components {
                if byBlock[c.id] == nil { byBlock[c.id] = c }
                if let block = c.instanceOf, byBlock[block] == nil { byBlock[block] = c }
            }
            for d in snap.directives {
                guard let c = byBlock[d.component], d.pin < c.pins.count else { continue }
                let pin = c.pins[d.pin].point
                let at = CGPoint(x: pin.x + 6, y: pin.y - 10).applying(screen)
                ctx.draw(Text(verbatim: "◆ " + d.summary).font(.system(size: 9, weight: .semibold, design: .monospaced))
                            .foregroundColor(pal.directive.color), at: at, anchor: .bottomLeading)
            }
        }
        // The bus being drawn.
        if tool == .bus, !busPoints.isEmpty {
            var path = Path()
            path.addLines(busPoints + (hover.map { [SchematicGrid.snap(viewport.toWorld($0))] } ?? []))
            ctx.stroke(path.applying(screen), with: .color(pal.preview.color), style: StrokeStyle(lineWidth: 3, dash: [6, 4]))
        }

        // Sheet symbols: a box around the entries of each child sheet (its pins on the left edge) with the sheet's name.
        for child in snap.sheets where child.parent == snap.activeSheet {
            let entries = snap.components.filter { $0.labelScope == "entry" && $0.targetSheet == child.id }
            guard !entries.isEmpty else { continue }
            var box = CGRect.null
            for e in entries {
                var position = e.position
                if movingIds.contains(e.id) { position.x += delta.width; position.y += delta.height }
                box = box.union(SchematicSymbols.bounds(.netLabel, value: e.value, custom: nil)
                    .applying(SchematicSymbols.transform(position: position, rotation: e.rotation)))
                box = box.union(CGRect(origin: position, size: .zero))
            }
            let left: CGFloat = entries.map { CGFloat($0.x) + (movingIds.contains($0.id) ? delta.width : 0) }.min() ?? box.minX
            // A drawn size (sheet tab ▸ Sheet Symbol Size) enlarges the fitted box.
            let frame = CGRect(x: left, y: box.minY - 16, width: max(80, box.maxX + 16 - left, CGFloat(child.symbolWidth ?? 0)),
                               height: max(box.height + 26, CGFloat(child.symbolHeight ?? 0)))
            guard frame.intersects(view) else { continue }
            let rect = frame.applying(screen)
            ctx.fill(Path(rect), with: .color(pal.symbolFill.color))
            ctx.stroke(Path(rect), with: .color(pal.symbol.color), lineWidth: 1.6)
            if showLabels {
                ctx.draw(Text(verbatim: child.name).font(.system(size: max(8, min(13, 9 * viewport.scale / 1.6)), weight: .bold))
                            .foregroundColor(pal.designator.color), at: CGPoint(x: rect.minX, y: rect.minY - 4), anchor: .bottomLeading)
            }
        }

        // Components.
        for c in snap.components where c.componentKind != .junction {
            var position = c.position
            if movingIds.contains(c.id) { position.x += delta.width; position.y += delta.height }
            let local = SchematicSymbols.transform(position: position, rotation: c.rotation, mirror: c.isMirrored)
            let custom = snap.customPart(for: c)
            guard SchematicSymbols.bounds(c.componentKind, value: c.value, custom: custom).applying(local).intersects(view) else { continue }
            let t = local.concatenating(screen)
            if c.componentKind == .netLabel, c.isPower {
                // A power symbol: a stem from the pin to a bar, the rail's name beyond the bar (kept upright).
                var glyph = Path()
                glyph.move(to: .zero)
                glyph.addLine(to: CGPoint(x: 12, y: 0))
                glyph.move(to: CGPoint(x: 12, y: -9))
                glyph.addLine(to: CGPoint(x: 12, y: 9))
                let picked = store.selection.contains(c.id)
                if picked { ctx.stroke(glyph.applying(t), with: .color(pal.selectionHalo.color.opacity(0.55)), lineWidth: 6) }
                ctx.stroke(glyph.applying(t), with: .color(picked ? pal.selection.color : pal.netLabel.color),
                           style: StrokeStyle(lineWidth: 2, lineCap: .round))
                if showLabels {
                    let r = ((c.rotation % 360) + 360) % 360
                    let anchor: UnitPoint = r == 90 ? .top : r == 180 ? .trailing : r == 270 ? .bottom : .leading
                    ctx.draw(Text(verbatim: c.value).font(.system(size: max(8, min(13, 9 * viewport.scale / 1.6)), weight: .bold, design: .monospaced))
                                .foregroundColor(pal.netLabel.color), at: CGPoint(x: 15, y: 0).applying(t), anchor: anchor)
                }
                continue
            }
            // A switch is drawn in the state it is in now: the live board's, otherwise its value.
            var symbolValue = c.value
            if c.componentKind == .switchSPST, let closed = live.isRunning ? live.isClosed(c.id) : nil {
                symbolValue = closed ? "on" : "off"
            }
            let shapes = custom.map(SchematicSymbols.customShapes) ?? SchematicSymbols.shapes(for: c.componentKind, value: symbolValue)
            // A unit is highlighted when it or its package (picked from the PCB, BOM or a check) is selected.
            let selected = store.selection.contains(c.id) || (c.unitOf.map { store.selection.contains($0) } ?? false)
            if selected {
                ctx.stroke(shapes.stroke.applying(t), with: .color(pal.selectionHalo.color.opacity(0.55)), lineWidth: 6)
            }
            if c.componentKind == .led {
                // LEDs show their colour: a dim tint when dark, lit body and glow with the current through them.
                let colour = Self.ledColour(c.value)
                let b = ledBrightness(c.id)
                if b > 0.02 {
                    let box = SchematicSymbols.bounds(.led).applying(local)
                    let centre = CGPoint(x: box.midX, y: box.midY).applying(screen)
                    let radius = max(16, 34 * viewport.scale) * (0.6 + 0.4 * b)
                    ctx.fill(Path(ellipseIn: CGRect(x: centre.x - radius, y: centre.y - radius, width: 2 * radius, height: 2 * radius)),
                             with: .radialGradient(Gradient(colors: [colour.opacity(0.85 * b), colour.opacity(0.4 * b), colour.opacity(0)]),
                                                   center: centre, startRadius: 0, endRadius: radius))
                }
                ctx.fill(shapes.fill.applying(t), with: .color(b > 0.02 ? colour.opacity(0.45 + 0.55 * b) : colour.opacity(0.22)))
                let ledStroke = selected ? pal.selection.color : (b > 0.02 ? colour : colour.opacity(0.75))
                ctx.stroke(shapes.stroke.applying(t), with: .color(ledStroke),
                           style: StrokeStyle(lineWidth: b > 0.02 ? 2 : 1.6, lineCap: .round, lineJoin: .round))
                ctx.fill(shapes.solid.applying(t), with: .color(ledStroke))
            } else {
                ctx.fill(shapes.fill.applying(t), with: .color(pal.symbolFill.color))
                let symbolColour = style.colourByDevice ? c.componentKind.deviceColour().readable(on: pal.background) : pal.symbol
                var strokeColor = selected ? pal.selection.color : symbolColour.color
                if c.componentKind == .switchSPST, !selected, live.isRunning, live.isClosed(c.id) == true { strokeColor = pal.liveOn.color }
                ctx.stroke(shapes.stroke.applying(t), with: .color(strokeColor),
                           style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
                ctx.fill(shapes.solid.applying(t), with: .color(strokeColor))
            }

            // A part the active variant (or the BOM) leaves off is crossed out: not on the assembly, not simulated.
            if !c.isFitted, !c.componentKind.isVirtual {
                let box = SchematicSymbols.bounds(c.componentKind, value: c.value, custom: custom).insetBy(dx: 4, dy: 4).applying(t)
                var cross = Path()
                cross.move(to: CGPoint(x: box.minX, y: box.minY))
                cross.addLine(to: CGPoint(x: box.maxX, y: box.maxY))
                cross.move(to: CGPoint(x: box.minX, y: box.maxY))
                cross.addLine(to: CGPoint(x: box.maxX, y: box.minY))
                ctx.stroke(cross, with: .color(pal.error.color.opacity(0.85)), style: StrokeStyle(lineWidth: 2, lineCap: .round))
            }

            // Pins
            for (i, p) in c.pins.enumerated() where showPins {
                var pp = p.point
                if movingIds.contains(c.id) { pp.x += delta.width; pp.y += delta.height }
                let s = pp.applying(screen)
                let r: CGFloat = 2.5
                if p.noConnect {
                    // No-connect flag: a cross on the pin end.
                    var cross = Path()
                    let a: CGFloat = max(3.5, 4 * viewport.scale)
                    cross.move(to: CGPoint(x: s.x - a, y: s.y - a))
                    cross.addLine(to: CGPoint(x: s.x + a, y: s.y + a))
                    cross.move(to: CGPoint(x: s.x - a, y: s.y + a))
                    cross.addLine(to: CGPoint(x: s.x + a, y: s.y - a))
                    ctx.stroke(cross, with: .color(p.connected ? pal.error.color : pal.noConnect.color), lineWidth: 1.5)
                } else if p.connected {
                    ctx.fill(Path(ellipseIn: CGRect(x: s.x - r, y: s.y - r, width: 2 * r, height: 2 * r)), with: .color(pal.pin.color))
                } else if !c.componentKind.isVirtual || c.componentKind == .netLabel {
                    ctx.stroke(Path(CGRect(x: s.x - 3, y: s.y - 3, width: 6, height: 6)), with: .color(pal.unconnectedPin.color), lineWidth: 1.2)
                }
                if c.componentKind == .ic8, viewport.scale > 1.2, showPinNumbers {
                    let inward = CGPoint(x: (c.position.x - pp.x) * 0.18 + pp.x, y: pp.y).applying(screen)
                    ctx.draw(Text("\(i + 1)").font(.system(size: 8, design: .monospaced)).foregroundColor(pal.pinNumber.color),
                             at: inward)
                }
            }

            if let custom, showPins, viewport.scale > 0.9, showPinNames || showPinNumbers {
                // Pin names inside the body, pin numbers on the leads (readable at any rotation).
                SchematicSymbols.drawPinLabels(ctx, part: custom, transform: t, fontSize: max(7, min(11, 7 * viewport.scale / 1.6)),
                                               textColor: pal.symbol.color,
                                               nameColor: { $0.type == PinElectricalType.powerIn.rawValue ? pal.powerLabel.color : pal.pinName.color },
                                               numberColor: pal.pinNumber.color, showNames: showPinNames, showNumbers: showPinNumbers)
            }

            // Labels (kept upright)
            guard showLabels else { continue }
            let fontSize = max(8, min(13, 9 * viewport.scale / 1.6))
            switch c.componentKind {
            case .ground:
                break
            case .netLabel:
                let width = SchematicSymbols.netLabelTextWidth(c.value)
                let center = CGPoint(x: (width + 7) / 2, y: 0).applying(t)
                // Global labels sky blue, sheet-local ones muted, hierarchical ports and sheet entries amber.
                // Harness labels (a bundle) and their entries are drawn in the harness colour, the bundle marked ≡.
                let colour: Color
                switch c.labelScope {
                case _ where c.harnessType != nil: colour = pal.harness.color
                case "local": colour = pal.localLabel.color
                case "port", "entry": colour = pal.portLabel.color
                default: colour = pal.netLabel.color
                }
                ctx.draw(Text(verbatim: c.isHarnessLabel ? c.value + " ≡" : c.value)
                            .font(.system(size: fontSize, weight: c.isHarnessLabel ? .heavy : .semibold, design: .monospaced))
                            .foregroundColor(colour), at: center)
            default:
                // Labels sit above/below wide symbols and to the right of tall ones, always upright.
                let box = SchematicSymbols.bounds(c.componentKind, custom: custom).applying(local)
                let refPoint: CGPoint
                let valPoint: CGPoint
                let anchor: UnitPoint
                if box.width >= box.height {
                    refPoint = CGPoint(x: box.midX, y: box.minY - 7).applying(screen)
                    valPoint = CGPoint(x: box.midX, y: box.maxY + 7).applying(screen)
                    anchor = .center
                } else {
                    refPoint = CGPoint(x: box.maxX + 5, y: box.midY - 7).applying(screen)
                    valPoint = CGPoint(x: box.maxX + 5, y: box.midY + 7).applying(screen)
                    anchor = .leading
                }
                // A part the active variant leaves off is marked DNP; a variant value replaces the design value.
                if showRefs {
                    ctx.draw(Text(c.isFitted ? c.displayRef : c.displayRef + " DNP").font(.system(size: fontSize, weight: .bold, design: .monospaced))
                                .foregroundColor(selected ? pal.selection.color : (c.isFitted ? pal.designator.color : pal.error.color)),
                             at: refPoint, anchor: anchor)
                }
                if showValues {
                    ctx.draw(Text(c.variantValue ?? c.value).font(.system(size: fontSize, design: .monospaced))
                                .foregroundColor(c.variantValue == nil ? pal.value.color : pal.variantValue.color), at: valPoint, anchor: anchor)
                }
            }
        }

        // Drawing template of the sheet (A4 … ANSI E): its frame, centred on the drawing (10 units = 2.54 mm).
        if let shown = snap.sheet(snap.activeSheet), let size = shown.size, !size.isEmpty,
           let template = Self.templates.first(where: { $0.name == size }) {
            let w = template.width / 0.254, h = template.height / 0.254
            let frame: CGRect
            if let fx = shown.frameX, let fy = shown.frameY {
                frame = CGRect(x: fx, y: fy, width: w, height: h)  // fixed: stays put while parts move
            } else {
                let content = Self.componentBounds(snap).reduce(CGRect.null) { $0.union($1.rect) }
                let center = content.isNull ? CGPoint.zero : CGPoint(x: content.midX, y: content.midY)
                frame = CGRect(x: center.x - w / 2, y: center.y - h / 2, width: w, height: h)
            }
            ctx.stroke(Path(frame.applying(screen)), with: .color(pal.sheetBorder.color.opacity(0.55)),
                       style: StrokeStyle(lineWidth: 1, dash: [8, 5]))
            if showLabels {
                ctx.draw(Text(verbatim: template.name).font(.system(size: 11, weight: .semibold)).foregroundColor(pal.sheetText.color),
                         at: CGPoint(x: frame.minX + 6, y: frame.minY + 6).applying(screen), anchor: .topLeading)
            }
        }

        // Title block at the bottom right of the sheet's drawing.
        if showLabels, !snap.components.isEmpty {
            let content = Self.componentBounds(snap).reduce(CGRect.null) { $0.union($1.rect) }
            if !content.isNull {
                let block = CGRect(x: content.maxX - 220, y: content.maxY + 40, width: 260, height: 64)
                if block.intersects(view) {
                    let rect = block.applying(screen)
                    ctx.stroke(Path(rect), with: .color(pal.sheetBorder.color.opacity(0.8)), lineWidth: 1.2)
                    var rule = Path()
                    rule.move(to: CGPoint(x: rect.minX, y: rect.midY))
                    rule.addLine(to: CGPoint(x: rect.maxX, y: rect.midY))
                    ctx.stroke(rule, with: .color(pal.sheetBorder.color.opacity(0.5)), lineWidth: 0.8)
                    let size = max(7, min(12, 8 * viewport.scale / 1.6))
                    let tb = store.snapshot.titleBlock
                    let index = (snap.sheets.firstIndex { $0.id == snap.activeSheet } ?? 0) + 1
                    let total = max(1, snap.sheets.count)
                    ctx.draw(Text(verbatim: tb.title).font(.system(size: size + 1, weight: .bold)).foregroundColor(pal.designator.color),
                             at: CGPoint(x: rect.minX + 6, y: rect.minY + 4), anchor: .topLeading)
                    ctx.draw(Text(verbatim: snap.sheet(snap.activeSheet)?.name ?? "").font(.system(size: size)).foregroundColor(pal.value.color),
                             at: CGPoint(x: rect.maxX - 6, y: rect.minY + 4), anchor: .topTrailing)
                    ctx.draw(Text("Sheet \(index) of \(total)").font(.system(size: size)).foregroundColor(pal.value.color),
                             at: CGPoint(x: rect.maxX - 6, y: rect.midY - 3), anchor: .bottomTrailing)
                    let lower = [tb.company, tb.revision.isEmpty ? "" : "Rev " + tb.revision, tb.date, tb.drawnBy]
                        .filter { !$0.isEmpty }.joined(separator: "  ·  ")
                    ctx.draw(Text(verbatim: lower).font(.system(size: size)).foregroundColor(pal.value.color),
                             at: CGPoint(x: rect.minX + 6, y: rect.midY + 4), anchor: .topLeading)
                }
            }
        }

        // Live board: switches show their state (LEDs are lit where they are drawn).
        if live.isRunning, let liveState = live.state {
            for sw in liveState.switches {
                guard let c = snap.component(sw.component) else { continue }
                let local = SchematicSymbols.transform(position: c.position, rotation: c.rotation, mirror: c.isMirrored)
                let box = SchematicSymbols.bounds(c.componentKind, custom: nil).applying(local)
                let top = CGPoint(x: box.midX, y: box.minY).applying(screen)
                let closed = live.isClosed(sw.component) ?? sw.closed
                let text = closed ? (sw.momentary ? "PRESSED" : "ON") : (sw.momentary ? "PRESS" : "OFF")
                let width = CGFloat(text.count) * 6.2 + 14
                let rect = CGRect(x: top.x - width / 2, y: top.y - 22, width: width, height: 15)
                ctx.fill(Path(roundedRect: rect, cornerRadius: 7.5),
                         with: .color(closed ? pal.liveOn.color.opacity(0.9) : pal.overlayFill.color.opacity(0.92)))
                ctx.stroke(Path(roundedRect: rect, cornerRadius: 7.5), with: .color(closed ? pal.liveOn.color : pal.overlayMuted.color), lineWidth: 1)
                ctx.draw(Text(text).font(.system(size: 9, weight: .bold, design: .rounded))
                            .foregroundColor(closed ? pal.overlayOnText.color : pal.overlayText.color), at: CGPoint(x: rect.midX, y: rect.midY))
            }
        }

        // Live probes (Proteus-style): the running board's voltages, otherwise the DC operating point.
        let liveState = live.isRunning ? live.state : nil
        let probeVoltage: ((Int) -> Double?)? = {
            if let liveState { return { liveState.voltage(net: $0) } }
            if store.showDCOverlay, let dc = store.dcResult, dc.converged { return { dc.voltage(net: $0) } }
            return nil
        }()
        if let probeVoltage {
            var labelled = Set<Int>()
            for w in snap.wires {
                guard w.net >= 0, !labelled.contains(w.net), let net = snap.net(w.net), !net.ground,
                      let v = probeVoltage(w.net), let a = pinPoint(w.a), let b = pinPoint(w.b) else { continue }
                labelled.insert(w.net)
                let pts = Self.wirePath(a, b)
                let mid = pts.count == 3 ? pts[1] : CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
                let s = mid.applying(screen)
                let text = EngineeringFormat.string(v, unit: "V", digits: 3)
                let width = CGFloat(text.count) * 6.4 + 14
                let rect = CGRect(x: s.x - width / 2, y: s.y - 20, width: width, height: 15)
                ctx.fill(Path(roundedRect: rect, cornerRadius: 7.5), with: .color(pal.overlayFill.color.opacity(0.92)))
                ctx.stroke(Path(roundedRect: rect, cornerRadius: 7.5), with: .color(pal.probe.color), lineWidth: 1)
                ctx.draw(Text(text).font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                            .foregroundColor(pal.probe.color), at: CGPoint(x: rect.midX, y: rect.midY))
            }
        }

        // Rubber-band wire.
        let wireFrom: WireEnd? = { if case .wire(let start) = dragMode { return start } else { return pendingWire } }()
        if let start = wireFrom, let a = endPoint(start), let h = hover {
            let b = endPoint(wireEnd(at: viewport.toWorld(h))) ?? viewport.toWorld(h)
            var path = Path()
            path.addLines(flipCorner && a.x != b.x && a.y != b.y ? [a, CGPoint(x: a.x, y: b.y), b] : Self.wirePath(a, b))
            ctx.stroke(path.applying(screen), with: .color(pal.preview.color),
                       style: StrokeStyle(lineWidth: 1.6, dash: [6, 4]))
        }

        // Placement ghost.
        var ghost: SchematicSymbols.Shapes?
        if case .place(let kind) = tool { ghost = SchematicSymbols.shapes(for: kind, value: kind.defaultValue) }
        if case .placeCustom(let partId) = tool, let part = snap.customPart(partId) { ghost = SchematicSymbols.customShapes(part) }
        if let shapes = ghost, let h = hover {
            let world = SchematicGrid.snap(viewport.toWorld(h))
            let t = SchematicSymbols.transform(position: world, rotation: placementRotation).concatenating(screen)
            ctx.stroke(shapes.stroke.applying(t), with: .color(pal.preview.color.opacity(0.6)),
                       style: StrokeStyle(lineWidth: 1.4, dash: [4, 3]))
        }

        // Hover highlight on pins, and on the spot of a wire where a T-junction would go while wiring.
        if let h = hover, tool != .pan {
            let world = viewport.toWorld(h)
            if let p = pin(at: world, includeJunctions: wiringJunctions)?.1 {
                let s = p.applying(screen)
                ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 7, y: s.y - 7, width: 14, height: 14)), with: .color(pal.preview.color), lineWidth: 1.5)
            } else if wiringJunctions || wireFrom != nil, let hit = wireHit(at: world) {
                let s = hit.point.applying(screen)
                ctx.fill(Path(ellipseIn: CGRect(x: s.x - 4, y: s.y - 4, width: 8, height: 8)), with: .color(pal.preview.color))
                ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 8, y: s.y - 8, width: 16, height: 16)), with: .color(pal.preview.color), lineWidth: 1.2)
            }
        }

        // Check markers: every ERC error / warning on this sheet at its spot; the one opened from Messages is ringed.
        for v in store.ercResults where v.hasLocation && v.severity != .info && (v.sheet ?? snap.activeSheet) == snap.activeSheet {
            let s = CGPoint(x: v.x, y: v.y).applying(screen)
            let colour = v.severity == .error ? pal.error.color : Color.orange
            var tri = Path()
            tri.move(to: CGPoint(x: s.x, y: s.y - 9))
            tri.addLine(to: CGPoint(x: s.x + 6, y: s.y + 2))
            tri.addLine(to: CGPoint(x: s.x - 6, y: s.y + 2))
            tri.closeSubpath()
            ctx.fill(tri, with: .color(colour.opacity(0.85)))
            if store.focusedViolation == v.id {
                ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 16, y: s.y - 16, width: 32, height: 32)), with: .color(colour), lineWidth: 2.5)
            }
        }

        if let m = marquee {
            ctx.fill(Path(m), with: .color(pal.selectionHalo.color.opacity(0.12)))
            ctx.stroke(Path(m), with: .color(pal.preview.color), style: StrokeStyle(lineWidth: 1, dash: [5, 3]))
        }

        if zoomArmed {
            CanvasOverlays.banner("Zoom to area — drag a rectangle (click zooms 2×) · Esc cancels", in: &ctx, size: size)
        }
        if let h = hover {
            let w = viewport.toWorld(h)
            CanvasOverlays.readout(String(format: "X %.0f  Y %.0f", w.x, w.y), in: &ctx, size: size, colour: pal.readout.color)
        }
    }

    private func drawHarnessBodies(_ ctx: inout GraphicsContext, snap: DesignSnapshot, screen: CGAffineTransform,
                                   moving: Set<Int>, delta: CGSize) {
        for h in snap.components where h.isHarnessLabel {
            var box = CGRect.null
            for e in snap.components where e.harnessOf == h.id {
                var p = e.position
                if moving.contains(e.id) { p.x += delta.width; p.y += delta.height }
                box = box.union(CGRect(x: p.x - 6, y: p.y - 10, width: SchematicSymbols.netLabelTextWidth(e.value) + 16, height: 20))
            }
            guard !box.isNull else { continue }
            var anchor = h.position
            if moving.contains(h.id) { anchor.x += delta.width; anchor.y += delta.height }
            var outline = Path()
            let notch: CGFloat = 8
            if anchor.x < box.midX {
                outline.addLines([CGPoint(x: box.minX, y: box.minY), CGPoint(x: box.maxX, y: box.minY), CGPoint(x: box.maxX, y: box.maxY),
                                  CGPoint(x: box.minX, y: box.maxY), CGPoint(x: box.minX - notch, y: box.midY)])
            } else {
                outline.addLines([CGPoint(x: box.minX, y: box.minY), CGPoint(x: box.maxX, y: box.minY), CGPoint(x: box.maxX + notch, y: box.midY),
                                  CGPoint(x: box.maxX, y: box.maxY), CGPoint(x: box.minX, y: box.maxY)])
            }
            outline.closeSubpath()
            ctx.fill(outline.applying(screen), with: .color(style.palette.harness.color.opacity(0.08)))
            ctx.stroke(outline.applying(screen), with: .color(style.palette.harness.color.opacity(0.8)), lineWidth: 1.4)
        }
    }

    private func drawGrid(_ ctx: inout GraphicsContext, size: CGSize) {
        // Pitch adapts to the zoom (10, 50, 100, 500 … units) so dots and lines never get denser than 8 points.
        let minor = viewport.gridPitch(base: SchematicGrid.step, minimumPoints: 8)
        switch style.grid {
        case .none: return
        case .lines: drawGridLines(&ctx, size: size, minor: minor, major: minor * CGFloat(style.majorEvery))
        case .dots: drawGridDots(&ctx, size: size, minor: minor, major: minor * 10)
        }
    }

    private func drawGridDots(_ ctx: inout GraphicsContext, size: CGSize, minor: CGFloat, major: CGFloat) {
        let world = viewport.visibleWorldRect(in: size)
        var dots = Path()
        var majors = Path()
        var x = (world.minX / minor).rounded(.down) * minor
        while x <= world.maxX {
            var y = (world.minY / minor).rounded(.down) * minor
            while y <= world.maxY {
                let s = viewport.toScreen(CGPoint(x: x, y: y))
                let isMajor = abs(x.remainder(dividingBy: major)) < minor / 2 && abs(y.remainder(dividingBy: major)) < minor / 2
                if isMajor {
                    majors.addRect(CGRect(x: s.x - 1, y: s.y - 1, width: 2, height: 2))
                } else {
                    dots.addRect(CGRect(x: s.x - 0.5, y: s.y - 0.5, width: 1, height: 1))
                }
                y += minor
            }
            x += minor
        }
        ctx.fill(dots, with: .color(style.palette.gridMinor.color))
        ctx.fill(majors, with: .color(style.palette.gridMajor.color))
    }

    /// Line grid: thin minor lines, a stronger line every `style.majorEvery` minor lines.
    private func drawGridLines(_ ctx: inout GraphicsContext, size: CGSize, minor: CGFloat, major: CGFloat) {
        let world = viewport.visibleWorldRect(in: size)
        var lines = Path()
        var majors = Path()
        var x = (world.minX / minor).rounded(.down) * minor
        while x <= world.maxX {
            let sx = viewport.toScreen(CGPoint(x: x, y: 0)).x
            let isMajor = abs(x.remainder(dividingBy: major)) < minor / 2
            let a = CGPoint(x: sx, y: 0), b = CGPoint(x: sx, y: size.height)
            if isMajor { majors.move(to: a); majors.addLine(to: b) } else { lines.move(to: a); lines.addLine(to: b) }
            x += minor
        }
        var y = (world.minY / minor).rounded(.down) * minor
        while y <= world.maxY {
            let sy = viewport.toScreen(CGPoint(x: 0, y: y)).y
            let isMajor = abs(y.remainder(dividingBy: major)) < minor / 2
            let a = CGPoint(x: 0, y: sy), b = CGPoint(x: size.width, y: sy)
            if isMajor { majors.move(to: a); majors.addLine(to: b) } else { lines.move(to: a); lines.addLine(to: b) }
            y += minor
        }
        ctx.stroke(lines, with: .color(style.palette.gridMinor.color), lineWidth: 0.5)
        ctx.stroke(majors, with: .color(style.palette.gridMajor.color), lineWidth: 1)
    }
}
