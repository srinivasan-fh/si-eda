import AppKit
import SwiftUI

/// Interactive schematic canvas: drawing, hit-testing, selection, moving, wiring and placement.
struct SchematicCanvas: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var tool: SchematicTool
    @Binding var viewport: Viewport
    @Binding var canvasSize: CGSize
    /// "R1.2" while a wire is being drawn (shown as a hint by the editor), nil otherwise.
    @Binding var wireStart: String?

    private enum DragMode {
        case move(Set<Int>)
        case pan(CGSize)
        case marquee
        case zoomBox
    }

    @State private var dragMode: DragMode?
    @State private var dragDelta: CGSize = .zero          // world units, for live move preview
    @State private var marquee: CGRect?
    @State private var pendingWire: PinAddress?
    @State private var hover: CGPoint?
    @State private var magnifyBase: CGFloat?
    @State private var didInitialFit = false
    @State private var placementRotation = 0  // Space / R rotate the part being placed (degrees)
    @State private var zoomArmed = false   // Z: the next drag defines the area to zoom to
    @FocusState private var focused: Bool

    private let scaleLimits = Viewport.schematicLimits

    var body: some View {
        GeometryReader { geo in
            Canvas(rendersAsynchronously: false) { ctx, size in
                draw(&ctx, size: size)
            }
            .contentShape(Rectangle())
            .accessibilityElement()
            .accessibilityLabel("Schematic canvas")
            .accessibilityValue("\(store.snapshot.components.count) components, \(store.snapshot.wires.count) wires, "
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
            .onKeyPress(.space) { rotate(); return .handled }
            .onKeyPress(.escape) {
                zoomArmed = false
                pendingWire = nil
                tool = .select
                store.select(component: nil)
                return .handled
            }
            .onKeyPress(.delete) { store.deleteSelection(); return .handled }
            .onKeyPress(.deleteForward) { store.deleteSelection(); return .handled }
            .onKeyPress(KeyEquivalent("r")) { rotate(); return .handled }
            .onKeyPress(KeyEquivalent("w")) { tool = .wire; return .handled }
            .onKeyPress(KeyEquivalent("v")) { tool = .select; return .handled }
            .onKeyPress(KeyEquivalent("h")) { tool = .pan; return .handled }
            .onKeyPress(KeyEquivalent("g")) { tool = .place(.ground); return .handled }
            .onKeyPress(KeyEquivalent("l")) { tool = .place(.netLabel); return .handled }
            .onAppear {
                canvasSize = geo.size
                focused = true
                if !didInitialFit {
                    didInitialFit = true
                    fitToContent(size: geo.size)
                }
            }
            .onChange(of: geo.size) { _, newSize in canvasSize = newSize }
            .onChange(of: store.viewRequest) { _, request in
                if let request { perform(request.command, size: geo.size) }
            }
            .onChange(of: store.fitToken) { _, _ in fitToContent(size: geo.size) }
            .onChange(of: pendingWire) { _, address in
                guard let address, let c = store.snapshot.component(address.component), address.pin < c.pins.count else {
                    wireStart = nil
                    return
                }
                wireStart = "\(c.ref).\(c.pins[address.pin].name)"
            }
            .onChange(of: store.snapshot.components.count) { old, new in
                if old == 0 && new > 0 { fitToContent(size: geo.size) }
            }
        }
    }

    // MARK: - Navigation

    /// World-space bounds of every component symbol (used for fit, zoom to selection and the navigator).
    static func componentBounds(_ snapshot: DesignSnapshot) -> [(id: Int, rect: CGRect)] {
        snapshot.components.map { c in
            let rect = SchematicSymbols.bounds(c.componentKind, custom: snapshot.customPart(for: c))
                .applying(SchematicSymbols.transform(position: c.position, rotation: c.rotation))
            return (c.id, rect)
        }
    }

    /// Space / R: rotates the part being placed (before the click) or the selected parts by 90°.
    private func rotate() {
        switch tool {
        case .place, .placeCustom: placementRotation = (placementRotation + 90) % 360
        default: store.rotateSelection()
        }
    }

    private func perform(_ command: ViewCommand, size: CGSize) {
        switch command {
        case .fit: fitToContent(size: size)
        case .fitSelection: fitSelection(size: size)
        case .zoomArea: zoomArmed = true
        default:
            viewport.apply(command, size: size, anchor: hover, baseScale: Viewport.schematicBaseScale, limits: scaleLimits)
        }
    }

    private func fitToContent(size: CGSize) {
        let rect = Self.componentBounds(store.snapshot).reduce(CGRect.null) { $0.union($1.rect) }
        guard !rect.isNull else { return }
        viewport.fit(rect.insetBy(dx: -30, dy: -30), in: size, limits: scaleLimits)
    }

    private func fitSelection(size: CGSize) {
        var rect = Self.componentBounds(store.snapshot).filter { store.selection.contains($0.id) }
            .reduce(CGRect.null) { $0.union($1.rect) }
        if let id = store.selectedWire, let w = store.snapshot.wires.first(where: { $0.id == id }) {
            rect = rect.union(CGRect(origin: w.start, size: .zero).union(CGRect(origin: w.end, size: .zero)))
        }
        guard !rect.isNull else { return fitToContent(size: size) }
        viewport.fit(rect.insetBy(dx: -40, dy: -40), in: size, limits: scaleLimits)
    }

    private var pickTolerance: CGFloat { max(4, 7 / viewport.scale) }

    private func pin(at world: CGPoint) -> (PinAddress, CGPoint)? {
        var best: (PinAddress, CGPoint, CGFloat)?
        for c in store.snapshot.components {
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
        for c in store.snapshot.components.reversed() {
            let t = SchematicSymbols.transform(position: c.position, rotation: c.rotation).inverted()
            if SchematicSymbols.bounds(c.componentKind, custom: store.snapshot.customPart(for: c)).contains(world.applying(t)) {
                return c.id
            }
        }
        return nil
    }

    private static func wirePath(_ a: CGPoint, _ b: CGPoint) -> [CGPoint] {
        if a.x == b.x || a.y == b.y { return [a, b] }
        return [a, CGPoint(x: b.x, y: a.y), b]
    }

    private func wire(at world: CGPoint) -> Int? {
        for w in store.snapshot.wires {
            let pts = Self.wirePath(w.start, w.end)
            for i in 0..<(pts.count - 1) where distance(world, pts[i], pts[i + 1]) <= pickTolerance {
                return w.id
            }
        }
        return nil
    }

    private func distance(_ p: CGPoint, _ a: CGPoint, _ b: CGPoint) -> CGFloat {
        let dx = b.x - a.x, dy = b.y - a.y
        let len2 = dx * dx + dy * dy
        let t = len2 > 0 ? max(0, min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2)) : 0
        return hypot(p.x - (a.x + t * dx), p.y - (a.y + t * dy))
    }

    private func pinPosition(_ address: PinAddress) -> CGPoint? {
        guard let c = store.snapshot.component(address.component), address.pin < c.pins.count else { return nil }
        return c.pins[address.pin].point
    }

    // MARK: - Interaction

    private var dragGesture: some Gesture {
        DragGesture(minimumDistance: 0, coordinateSpace: .local)
            .onChanged { value in
                if dragMode == nil { beginDrag(at: value.startLocation) }
                switch dragMode {
                case .move:
                    dragDelta = CGSize(width: value.translation.width / viewport.scale,
                                       height: value.translation.height / viewport.scale)
                case .pan(let start):
                    viewport.offset = CGSize(width: start.width + value.translation.width,
                                             height: start.height + value.translation.height)
                case .marquee, .zoomBox:
                    marquee = CGRect(origin: value.startLocation, size: .zero)
                        .union(CGRect(origin: value.location, size: .zero))
                case nil:
                    break
                }
            }
            .onEnded { value in
                focused = true
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
                    click(at: value.location)
                } else {
                    switch dragMode {
                    case .move(let ids):
                        let snapped = CGSize(width: (dragDelta.width / 10).rounded() * 10,
                                             height: (dragDelta.height / 10).rounded() * 10)
                        store.moveComponents(ids, by: snapped)
                    case .marquee:
                        if let rect = marquee {
                            let a = viewport.toWorld(rect.origin)
                            let b = viewport.toWorld(CGPoint(x: rect.maxX, y: rect.maxY))
                            let worldRect = CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero))
                            store.selection = Set(store.snapshot.components.filter { worldRect.contains($0.position) }.map(\.id))
                            store.selectedWire = nil
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
        let world = viewport.toWorld(screen)
        switch tool {
        case .select:
            if pin(at: world) != nil, component(at: world) == nil {
                dragMode = .pan(viewport.offset)
            } else if let id = component(at: world) {
                if !store.selection.contains(id) {
                    store.select(component: id, extend: NSEvent.modifierFlags.contains(.shift))
                }
                dragMode = .move(store.selection)
            } else if NSEvent.modifierFlags.contains(.shift) {
                dragMode = .marquee
            } else {
                dragMode = .pan(viewport.offset)
            }
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
        case .select, .wire:
            if let address = pin(at: world)?.0 {
                if let start = pendingWire {
                    if start != address { store.connect(start, address) }
                    pendingWire = nil
                } else {
                    pendingWire = address
                }
                return
            }
            if pendingWire != nil {
                pendingWire = nil
                return
            }
            guard tool == .select else { return }
            if let id = component(at: world) {
                store.select(component: id, extend: NSEvent.modifierFlags.contains(.shift))
            } else if let w = wire(at: world) {
                store.selection = []
                store.selectedWire = w
            } else {
                store.select(component: nil)
            }
        }
    }

    // MARK: - Drawing

    private func draw(_ ctx: inout GraphicsContext, size: CGSize) {
        let snap = store.snapshot
        ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.schematicBackground))
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
        var endpointCount: [String: (CGPoint, Int)] = [:]
        for w in snap.wires {
            guard let a = pinPoint(w.a), let b = pinPoint(w.b),
                  CGRect(origin: a, size: .zero).union(CGRect(origin: b, size: .zero)).insetBy(dx: -1, dy: -1).intersects(view)
            else { continue }
            var path = Path()
            path.addLines(Self.wirePath(a, b))
            let selected = store.selectedWire == w.id
            if selected {
                ctx.stroke(path.applying(screen), with: .color(Theme.blue.opacity(0.6)), lineWidth: 7)
            }
            ctx.stroke(path.applying(screen), with: .color(selected ? Theme.selection : Theme.wire),
                       style: StrokeStyle(lineWidth: 1.8, lineCap: .round, lineJoin: .round))
            for p in [a, b] {
                let key = "\(Int(p.x))_\(Int(p.y))"
                endpointCount[key] = (p, (endpointCount[key]?.1 ?? 0) + 1)
            }
        }
        for (_, entry) in endpointCount where entry.1 >= 2 {
            let s = entry.0.applying(screen)
            ctx.fill(Path(ellipseIn: CGRect(x: s.x - 3.5, y: s.y - 3.5, width: 7, height: 7)), with: .color(Theme.wire))
        }

        // Components.
        for c in snap.components {
            var position = c.position
            if movingIds.contains(c.id) { position.x += delta.width; position.y += delta.height }
            let local = SchematicSymbols.transform(position: position, rotation: c.rotation)
            let custom = snap.customPart(for: c)
            guard SchematicSymbols.bounds(c.componentKind, custom: custom).applying(local).intersects(view) else { continue }
            let t = local.concatenating(screen)
            let shapes = custom.map(SchematicSymbols.customShapes) ?? SchematicSymbols.shapes(for: c.componentKind, value: c.value)
            let selected = store.selection.contains(c.id)
            if selected {
                ctx.stroke(shapes.stroke.applying(t), with: .color(Theme.blue.opacity(0.55)), lineWidth: 6)
            }
            ctx.fill(shapes.fill.applying(t), with: .color(Theme.symbolFill))
            let strokeColor = selected ? Theme.selection : Theme.symbol
            ctx.stroke(shapes.stroke.applying(t), with: .color(strokeColor),
                       style: StrokeStyle(lineWidth: 1.6, lineCap: .round, lineJoin: .round))
            ctx.fill(shapes.solid.applying(t), with: .color(strokeColor))

            // Pins
            for (i, p) in c.pins.enumerated() where showPins {
                var pp = p.point
                if movingIds.contains(c.id) { pp.x += delta.width; pp.y += delta.height }
                let s = pp.applying(screen)
                let r: CGFloat = 2.5
                if p.connected {
                    ctx.fill(Path(ellipseIn: CGRect(x: s.x - r, y: s.y - r, width: 2 * r, height: 2 * r)), with: .color(Theme.pin))
                } else if !c.componentKind.isVirtual || c.componentKind == .netLabel {
                    ctx.stroke(Path(CGRect(x: s.x - 3, y: s.y - 3, width: 6, height: 6)), with: .color(Theme.unconnectedPin), lineWidth: 1.2)
                }
                if let custom, i < custom.symbol.pins.count, viewport.scale > 0.9 {
                    // Pin name inside the body, pin number on the lead (upright text).
                    let sp = custom.symbol.pins[i]
                    let left = sp.x < 0
                    let edge = left ? -custom.symbol.halfWidth : custom.symbol.halfWidth
                    let inner = CGPoint(x: edge + (left ? 4 : -4), y: sp.y).applying(t)
                    let centre = position.applying(screen)
                    let anchor: UnitPoint = abs(inner.x - centre.x) < 2 ? .center : (inner.x < centre.x ? .leading : .trailing)
                    let size = max(7, min(11, 7 * viewport.scale / 1.6))
                    ctx.draw(Text(sp.name).font(.system(size: size, design: .monospaced))
                                .foregroundColor(sp.type == PinElectricalType.powerIn.rawValue ? Theme.probe : Theme.skyBlue),
                             at: inner, anchor: anchor)
                    let lead = CGPoint(x: (sp.x + edge) / 2, y: sp.y - 5).applying(t)
                    ctx.draw(Text(sp.number).font(.system(size: size * 0.85, design: .monospaced)).foregroundColor(Theme.textMuted),
                             at: lead)
                }
                if c.componentKind == .ic8, viewport.scale > 1.2 {
                    let inward = CGPoint(x: (c.position.x - pp.x) * 0.18 + pp.x, y: pp.y).applying(screen)
                    ctx.draw(Text("\(i + 1)").font(.system(size: 8, design: .monospaced)).foregroundColor(Theme.textMuted),
                             at: inward)
                }
            }

            // Labels (kept upright)
            guard showLabels else { continue }
            let fontSize = max(8, min(13, 9 * viewport.scale / 1.6))
            switch c.componentKind {
            case .ground:
                break
            case .netLabel:
                let width = max(36, CGFloat(c.value.count) * 7 + 16)
                let center = CGPoint(x: (width + 7) / 2, y: 0).applying(t)
                ctx.draw(Text(c.value).font(.system(size: fontSize, weight: .semibold, design: .monospaced))
                            .foregroundColor(Theme.skyBlue), at: center)
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
                ctx.draw(Text(c.ref).font(.system(size: fontSize, weight: .bold, design: .monospaced))
                            .foregroundColor(selected ? Theme.selection : Theme.label), at: refPoint, anchor: anchor)
                ctx.draw(Text(c.value).font(.system(size: fontSize, design: .monospaced))
                            .foregroundColor(Theme.valueLabel), at: valPoint, anchor: anchor)
            }
        }

        // Live DC probes (Proteus-style).
        if store.showDCOverlay, let dc = store.dcResult, dc.converged {
            var labelled = Set<Int>()
            for w in snap.wires {
                guard w.net >= 0, !labelled.contains(w.net), let net = snap.net(w.net), !net.ground,
                      let v = dc.voltage(net: w.net), let a = pinPoint(w.a), let b = pinPoint(w.b) else { continue }
                labelled.insert(w.net)
                let pts = Self.wirePath(a, b)
                let mid = pts.count == 3 ? pts[1] : CGPoint(x: (a.x + b.x) / 2, y: (a.y + b.y) / 2)
                let s = mid.applying(screen)
                let text = EngineeringFormat.string(v, unit: "V", digits: 3)
                let width = CGFloat(text.count) * 6.4 + 14
                let rect = CGRect(x: s.x - width / 2, y: s.y - 20, width: width, height: 15)
                ctx.fill(Path(roundedRect: rect, cornerRadius: 7.5), with: .color(Theme.deepBlue.opacity(0.92)))
                ctx.stroke(Path(roundedRect: rect, cornerRadius: 7.5), with: .color(Theme.probe), lineWidth: 1)
                ctx.draw(Text(text).font(.system(size: 9.5, weight: .semibold, design: .monospaced))
                            .foregroundColor(Theme.probe), at: CGPoint(x: rect.midX, y: rect.midY))
            }
        }

        // Rubber-band wire.
        if let start = pendingWire, let a = pinPosition(start), let h = hover {
            var b = viewport.toWorld(h)
            if let snapPoint = pin(at: b)?.1 { b = snapPoint }
            var path = Path()
            path.addLines(Self.wirePath(a, b))
            ctx.stroke(path.applying(screen), with: .color(Theme.skyBlue),
                       style: StrokeStyle(lineWidth: 1.6, dash: [6, 4]))
        }

        // Placement ghost.
        var ghost: SchematicSymbols.Shapes?
        if case .place(let kind) = tool { ghost = SchematicSymbols.shapes(for: kind, value: kind.defaultValue) }
        if case .placeCustom(let partId) = tool, let part = snap.customPart(partId) { ghost = SchematicSymbols.customShapes(part) }
        if let shapes = ghost, let h = hover {
            let world = SchematicAutoLayout.snap(viewport.toWorld(h))
            let t = SchematicSymbols.transform(position: world, rotation: placementRotation).concatenating(screen)
            ctx.stroke(shapes.stroke.applying(t), with: .color(Theme.skyBlue.opacity(0.6)),
                       style: StrokeStyle(lineWidth: 1.4, dash: [4, 3]))
        }

        // Hover highlight on pins.
        if let h = hover, tool != .pan, let p = pin(at: viewport.toWorld(h))?.1 {
            let s = p.applying(screen)
            ctx.stroke(Path(ellipseIn: CGRect(x: s.x - 7, y: s.y - 7, width: 14, height: 14)), with: .color(Theme.skyBlue), lineWidth: 1.5)
        }

        if let m = marquee {
            ctx.fill(Path(m), with: .color(Theme.blue.opacity(0.12)))
            ctx.stroke(Path(m), with: .color(Theme.skyBlue), style: StrokeStyle(lineWidth: 1, dash: [5, 3]))
        }

        if zoomArmed {
            CanvasOverlays.banner("Zoom to area — drag a rectangle (click zooms 2×) · Esc cancels", in: &ctx, size: size)
        }
        if let h = hover {
            let w = viewport.toWorld(h)
            CanvasOverlays.readout(String(format: "X %.0f  Y %.0f", w.x, w.y), in: &ctx, size: size)
        }
    }

    private func drawGrid(_ ctx: inout GraphicsContext, size: CGSize) {
        // Pitch adapts to the zoom (10, 50, 100, 500 … units) so dots never get denser than 8 points.
        let minor = viewport.gridPitch(base: 10, minimumPoints: 8)
        let major = minor * 10
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
        ctx.fill(dots, with: .color(Theme.gridDot))
        ctx.fill(majors, with: .color(Theme.darkBlue))
    }
}
