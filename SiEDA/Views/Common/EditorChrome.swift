import AppKit
import SwiftUI

/// Photoshop-style vertical tool button.
struct ToolStripButton: View {
    var systemImage: String
    var help: String
    var isActive = false
    var action: () -> Void

    var body: some View {
        Button(action: action) {
            Image(systemName: systemImage)
                .font(.system(size: 15, weight: .medium))
                .frame(width: 34, height: 30)
                .foregroundStyle(isActive ? Color.white : Theme.lightBlue)
                .background(
                    RoundedRectangle(cornerRadius: 7, style: .continuous)
                        .fill(isActive ? Theme.blue : Color.clear)
                )
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
        .accessibilityLabel(help)
        .accessibilityAddTraits(isActive ? .isSelected : [])
    }
}

/// Vertical tool palette container (left edge of the editors).
struct ToolStrip<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        VStack(spacing: 4) {
            content
            Spacer()
        }
        .padding(.vertical, 8)
        .padding(.horizontal, 5)
        .frame(width: 44)
        .background(Theme.deepBlue)
        .overlay(Rectangle().frame(width: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .trailing)
    }
}

struct ToolStripDivider: View {
    var body: some View {
        Rectangle().fill(Theme.blue.opacity(0.3)).frame(width: 26, height: 1).padding(.vertical, 4)
    }
}

/// Photoshop-style options bar shown above the canvas.
struct OptionsBar<Content: View>: View {
    @ViewBuilder var content: Content

    var body: some View {
        HStack(spacing: 12) { content }
            .font(.callout)
            .foregroundStyle(Theme.textSecondary)
            .padding(.horizontal, 12)
            .frame(height: 36)
            .background(Theme.deepBlue.opacity(0.92))
            .overlay(Rectangle().frame(height: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .bottom)
    }
}

/// Zoom controls shared by the 2D editors: out / level menu / in, zoom to selection and zoom to fit.
struct ZoomControls: View {
    /// Current zoom as a fraction of 100 % (1.0 = 100 %).
    var level: CGFloat
    var zoomIn: () -> Void
    var zoomOut: () -> Void
    var fit: () -> Void
    var fitSelection: () -> Void
    var setLevel: (CGFloat) -> Void

    static let presets: [CGFloat] = [0.1, 0.25, 0.5, 1, 2, 4, 8, 16]

    var body: some View {
        HStack(spacing: 2) {
            Button(action: zoomOut) { Image(systemName: "minus.magnifyingglass") }
                .help("Zoom out (− or ⌘−)").accessibilityLabel("Zoom out")
            Menu {
                ForEach(Self.presets, id: \.self) { preset in
                    Button(ZoomControls.percent(preset)) { setLevel(preset) }
                }
                Divider()
                Button("Zoom to Selection", action: fitSelection)
                Button("Zoom to Fit", action: fit)
            } label: {
                Text(Self.percent(level))
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(Theme.skyBlue)
                    .frame(width: 56)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
            .help("Zoom level — choose a preset")
            .accessibilityLabel("Zoom level \(Self.percent(level))")
            Button(action: zoomIn) { Image(systemName: "plus.magnifyingglass") }
                .help("Zoom in (+ or ⌘=)").accessibilityLabel("Zoom in")
            Button(action: fitSelection) { Image(systemName: "viewfinder") }
                .help("Zoom to selection (⇧Z or ⌥⌘0)").accessibilityLabel("Zoom to selection")
            Button(action: fit) { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
                .help("Zoom to fit / Home (Home, 0 or ⌘0)").accessibilityLabel("Zoom to fit")
        }
        .buttonStyle(.borderless)
        .foregroundStyle(Theme.lightBlue)
    }

    static func percent(_ level: CGFloat) -> String {
        let p = level * 100
        return p < 10 ? String(format: "%.1f%%", p) : "\(Int(p.rounded()))%"
    }
}

// MARK: - View commands (menu, zoom controls and keyboard → active canvas)

/// Navigation request for the visible 2D canvas.
enum ViewCommand: Equatable {
    case zoomIn
    case zoomOut
    case fit                 // Home: everything in view
    case fitSelection        // selection in view (falls back to fit)
    case zoomArea            // arm zoom-to-rectangle; the next drag defines the area
    case setLevel(CGFloat)   // absolute zoom, 1.0 = 100 %
    case pan(dx: CGFloat, dy: CGFloat)  // fractions of the visible width/height
}

/// A `ViewCommand` with identity, so repeating the same command is still observed by `onChange`.
struct ViewRequest: Equatable {
    let id = UUID()
    var command: ViewCommand
}

extension Viewport {
    /// Applies the zoom/pan part of a command. `fit`, `fitSelection` and `zoomArea` need content knowledge and are
    /// handled by the canvas; this returns false for them.
    @discardableResult
    mutating func apply(_ command: ViewCommand, size: CGSize, anchor: CGPoint?, baseScale: CGFloat,
                        limits: ClosedRange<CGFloat>) -> Bool {
        let centre = CGPoint(x: size.width / 2, y: size.height / 2)
        switch command {
        case .zoomIn: zoom(by: 1.25, anchor: anchor ?? centre, limits: limits)
        case .zoomOut: zoom(by: 0.8, anchor: anchor ?? centre, limits: limits)
        case .setLevel(let level): setScale(level * baseScale, anchor: centre, limits: limits)
        case .pan(let dx, let dy): pan(by: CGSize(width: -dx * size.width, height: -dy * size.height))
        case .fit, .fitSelection, .zoomArea: return false
        }
        return true
    }
}

/// Keyboard navigation shared by the canvases (the canvas must be focused):
/// arrows pan (⇧ = half a screen), + / − zoom at the cursor, Home or 0 fit, Z zoom-to-area, ⇧Z zoom to selection,
/// N toggles the navigator, and holding Space turns any drag into a pan.
struct CanvasNavigationKeys: ViewModifier {
    var perform: (ViewCommand) -> Void
    var toggleNavigator: () -> Void
    @Binding var spaceHeld: Bool

    func body(content: Content) -> some View {
        content
            .onKeyPress(keys: [.upArrow, .downArrow, .leftArrow, .rightArrow]) { press in
                guard !press.modifiers.contains(.command) else { return .ignored }
                let step: CGFloat = press.modifiers.contains(.shift) ? 0.5 : 0.125
                switch press.key {
                case .upArrow: perform(.pan(dx: 0, dy: -step))
                case .downArrow: perform(.pan(dx: 0, dy: step))
                case .leftArrow: perform(.pan(dx: -step, dy: 0))
                default: perform(.pan(dx: step, dy: 0))
                }
                return .handled
            }
            .onKeyPress(keys: [.home]) { _ in
                perform(.fit)
                return .handled
            }
            .onKeyPress(characters: CharacterSet(charactersIn: "+=-_0zZnN")) { press in
                guard press.modifiers.intersection([.command, .control, .option]).isEmpty else { return .ignored }
                switch press.characters {
                case "+", "=": perform(.zoomIn)
                case "-", "_": perform(.zoomOut)
                case "0": perform(.fit)
                case "z": perform(.zoomArea)
                case "Z": perform(.fitSelection)
                case "n", "N": toggleNavigator()
                default: return .ignored
                }
                return .handled
            }
            .onKeyPress(.space, phases: [.down, .repeat, .up]) { press in
                spaceHeld = press.phase != .up
                return .handled
            }
    }
}

extension View {
    func canvasNavigationKeys(spaceHeld: Binding<Bool>, toggleNavigator: @escaping () -> Void,
                              perform: @escaping (ViewCommand) -> Void) -> some View {
        modifier(CanvasNavigationKeys(perform: perform, toggleNavigator: toggleNavigator, spaceHeld: spaceHeld))
    }
}

// MARK: - Canvas overlays drawn inside a Canvas

enum CanvasOverlays {
    /// Mode banner at the top centre (e.g. zoom-to-area armed).
    static func banner(_ text: String, in ctx: inout GraphicsContext, size: CGSize) {
        let resolved = ctx.resolve(Text(text).font(.system(size: 12, weight: .semibold)).foregroundColor(Theme.iceBlue))
        let measured = resolved.measure(in: size)
        let rect = CGRect(x: size.width / 2 - measured.width / 2 - 12, y: 10, width: measured.width + 24, height: measured.height + 10)
        ctx.fill(Path(roundedRect: rect, cornerRadius: rect.height / 2), with: .color(Theme.deepBlue.opacity(0.95)))
        ctx.stroke(Path(roundedRect: rect, cornerRadius: rect.height / 2), with: .color(Theme.skyBlue), lineWidth: 1)
        ctx.draw(resolved, at: CGPoint(x: rect.midX, y: rect.midY))
    }

    /// Cursor coordinate read-out at the top right.
    static func readout(_ text: String, in ctx: inout GraphicsContext, size: CGSize) {
        ctx.draw(Text(text).font(.system(size: 11, design: .monospaced)).foregroundColor(Theme.skyBlue),
                 at: CGPoint(x: size.width - 12, y: 10), anchor: .topTrailing)
    }
}

// MARK: - Navigator (overview)

/// Photoshop / Altium-style navigator: a thumbnail of the whole design with the visible area outlined.
/// Click or drag to move the view.
struct CanvasNavigator: View {
    /// World rectangle the thumbnail shows (all content).
    var extent: CGRect
    /// Rectangles sketched in the thumbnail (component bodies, the board outline, …).
    var items: [CGRect]
    var highlighted: [CGRect] = []
    var viewport: Viewport
    var canvasSize: CGSize
    var onCenter: (CGPoint) -> Void
    var onClose: () -> Void

    private let size = CGSize(width: 200, height: 140)

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("NAVIGATOR").font(.caption2.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                Button(action: onClose) { Image(systemName: "xmark") }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.textMuted)
                    .help("Hide navigator (N)")
                    .accessibilityLabel("Hide navigator")
            }
            Canvas { ctx, canvas in
                let map = mapping(in: canvas)
                ctx.fill(Path(CGRect(origin: .zero, size: canvas)), with: .color(Theme.navy))
                var sketch = Path()
                for r in items { sketch.addRect(map.rect(r)) }
                ctx.stroke(sketch, with: .color(Theme.lightBlue.opacity(0.55)), lineWidth: 0.7)
                var selected = Path()
                for r in highlighted { selected.addRect(map.rect(r)) }
                ctx.fill(selected, with: .color(Theme.selection.opacity(0.8)))
                let visible = map.rect(viewport.visibleWorldRect(in: canvasSize))
                    .intersection(CGRect(origin: .zero, size: canvas).insetBy(dx: -1, dy: -1))
                if !visible.isNull {
                    ctx.fill(Path(visible), with: .color(Theme.blue.opacity(0.18)))
                    ctx.stroke(Path(visible), with: .color(Theme.skyBlue), lineWidth: 1.5)
                }
            }
            .frame(width: size.width, height: size.height)
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0).onChanged { value in
                onCenter(mapping(in: size).world(value.location))
            })
            .accessibilityElement()
            .accessibilityLabel("Navigator")
            .accessibilityHint("Click or drag to move the visible area")
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.deepBlue.opacity(0.95)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.blue.opacity(0.45)))
    }

    private struct Mapping {
        var scale: CGFloat
        var origin: CGPoint  // thumbnail point of world (0, 0)
        func rect(_ r: CGRect) -> CGRect {
            CGRect(x: origin.x + r.minX * scale, y: origin.y + r.minY * scale, width: r.width * scale, height: r.height * scale)
        }
        func world(_ p: CGPoint) -> CGPoint { CGPoint(x: (p.x - origin.x) / scale, y: (p.y - origin.y) / scale) }
    }

    private func mapping(in canvas: CGSize) -> Mapping {
        let e = extent.isNull || extent.isEmpty ? CGRect(x: 0, y: 0, width: 100, height: 70) : extent
        let pad: CGFloat = 8
        let s = min((canvas.width - 2 * pad) / max(e.width, 1e-6), (canvas.height - 2 * pad) / max(e.height, 1e-6))
        let origin = CGPoint(x: canvas.width / 2 - e.midX * s, y: canvas.height / 2 - e.midY * s)
        return Mapping(scale: s, origin: origin)
    }
}

/// Empty-state placeholder.
struct BlueEmptyState: View {
    var systemImage: String
    var title: String
    var message: String
    var actionTitle: String?
    var action: (() -> Void)?

    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: systemImage)
                .font(.system(size: 44, weight: .light))
                .foregroundStyle(LinearGradient(colors: [Theme.skyBlue, Theme.blue], startPoint: .top, endPoint: .bottom))
            Text(title).font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
            Text(message).font(.callout).foregroundStyle(Theme.textMuted).multilineTextAlignment(.center)
                .frame(maxWidth: 380)
            if let actionTitle, let action {
                Button(actionTitle, action: action).buttonStyle(.borderedProminent)
            }
        }
        .padding(30)
    }
}

// MARK: - Mouse input: scroll wheel, trackpad, middle/right-button pan

/// Invisible background view that receives scroll-wheel events and middle/right-button drags over its frame without
/// blocking normal clicks.
struct CanvasInputMonitor: NSViewRepresentable {
    var onScroll: (NSEvent, CGPoint) -> Void
    var onPan: (CGSize) -> Void

    func makeNSView(context: Context) -> MonitorView {
        let view = MonitorView()
        view.onScroll = onScroll
        view.onPan = onPan
        return view
    }

    func updateNSView(_ view: MonitorView, context: Context) {
        view.onScroll = onScroll
        view.onPan = onPan
    }

    final class MonitorView: NSView {
        var onScroll: ((NSEvent, CGPoint) -> Void)?
        var onPan: ((CGSize) -> Void)?
        private var monitors: [Any] = []
        private var panning = false

        override var isFlipped: Bool { true }
        override func hitTest(_ point: NSPoint) -> NSView? { nil }  // never intercept mouse clicks

        private func contains(_ event: NSEvent) -> CGPoint? {
            guard event.window === window else { return nil }
            let point = convert(event.locationInWindow, from: nil)
            return bounds.contains(point) ? point : nil
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            removeMonitors()
            guard window != nil else { return }
            if let m = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel, handler: { [weak self] event in
                guard let self, let point = self.contains(event) else { return event }
                self.onScroll?(event, point)
                return nil
            }) { monitors.append(m) }
            let panEvents: NSEvent.EventTypeMask = [.otherMouseDown, .otherMouseDragged, .otherMouseUp,
                                                    .rightMouseDown, .rightMouseDragged, .rightMouseUp]
            if let m = NSEvent.addLocalMonitorForEvents(matching: panEvents, handler: { [weak self] event in
                guard let self else { return event }
                switch event.type {
                case .otherMouseDown, .rightMouseDown:
                    guard self.contains(event) != nil else { return event }
                    self.panning = true
                    NSCursor.closedHand.push()
                    return event.type == .otherMouseDown ? nil : event
                case .otherMouseDragged, .rightMouseDragged:
                    guard self.panning else { return event }
                    self.onPan?(CGSize(width: event.deltaX, height: event.deltaY))
                    return nil
                case .otherMouseUp, .rightMouseUp:
                    guard self.panning else { return event }
                    self.panning = false
                    NSCursor.pop()
                    return event.type == .otherMouseUp ? nil : event
                default:
                    return event
                }
            }) { monitors.append(m) }
        }

        private func removeMonitors() {
            monitors.forEach(NSEvent.removeMonitor)
            monitors.removeAll()
            if panning {
                panning = false
                NSCursor.pop()
            }
        }

        deinit {
            monitors.forEach(NSEvent.removeMonitor)
        }
    }
}

extension View {
    /// Scroll / pinch-free mouse navigation: scroll events (location in the view's top-left coordinates) and
    /// middle- or right-button drags (screen-point deltas).
    func canvasMouseInput(onScroll: @escaping (NSEvent, CGPoint) -> Void, onPan: @escaping (CGSize) -> Void) -> some View {
        background(CanvasInputMonitor(onScroll: onScroll, onPan: onPan))
    }
}

extension Viewport {
    /// Trackpad scroll pans; pinch, a mouse wheel, or scrolling with ⌘/⌥/⌃ held zooms around the cursor;
    /// ⇧ + mouse wheel pans horizontally.
    mutating func handleScroll(_ event: NSEvent, at point: CGPoint, limits: ClosedRange<CGFloat>) {
        let flags = event.modifierFlags
        let zoomModifier = !flags.intersection([.command, .option, .control]).isEmpty
        if !event.hasPreciseScrollingDeltas, flags.contains(.shift), !zoomModifier {
            // Mouse wheel with ⇧: macOS reports it as horizontal scrolling.
            let delta = event.scrollingDeltaX != 0 ? event.scrollingDeltaX : event.scrollingDeltaY
            pan(by: CGSize(width: delta * 8, height: 0))
        } else if zoomModifier || !event.hasPreciseScrollingDeltas {
            let delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 8
            guard delta != 0 else { return }
            zoom(by: exp(delta * 0.01), anchor: point, limits: limits)
        } else {
            pan(by: CGSize(width: event.scrollingDeltaX, height: event.scrollingDeltaY))
        }
    }
}
