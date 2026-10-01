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

/// Zoom controls shared by the 2D editors.
struct ZoomControls: View {
    var scale: CGFloat
    var zoomIn: () -> Void
    var zoomOut: () -> Void
    var fit: () -> Void

    var body: some View {
        HStack(spacing: 2) {
            Button(action: zoomOut) { Image(systemName: "minus.magnifyingglass") }.help("Zoom out")
            Text("\(Int(scale * 100))%")
                .font(.caption.monospacedDigit())
                .foregroundStyle(Theme.skyBlue)
                .frame(width: 52)
            Button(action: zoomIn) { Image(systemName: "plus.magnifyingglass") }.help("Zoom in")
            Button(action: fit) { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }.help("Zoom to fit")
        }
        .buttonStyle(.borderless)
        .foregroundStyle(Theme.lightBlue)
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

// MARK: - Scroll-wheel / trackpad navigation

/// Invisible background view that receives scroll-wheel events over its frame without blocking clicks.
struct ScrollWheelHandler: NSViewRepresentable {
    var onScroll: (NSEvent, CGPoint) -> Void

    func makeNSView(context: Context) -> MonitorView {
        let view = MonitorView()
        view.onScroll = onScroll
        return view
    }

    func updateNSView(_ view: MonitorView, context: Context) { view.onScroll = onScroll }

    final class MonitorView: NSView {
        var onScroll: ((NSEvent, CGPoint) -> Void)?
        private var monitor: Any?

        override var isFlipped: Bool { true }
        override func hitTest(_ point: NSPoint) -> NSView? { nil }  // never intercept mouse clicks

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let monitor {
                NSEvent.removeMonitor(monitor)
                self.monitor = nil
            }
            guard window != nil else { return }
            monitor = NSEvent.addLocalMonitorForEvents(matching: .scrollWheel) { [weak self] event in
                guard let self, event.window === self.window else { return event }
                let point = self.convert(event.locationInWindow, from: nil)
                guard self.bounds.contains(point) else { return event }
                self.onScroll?(event, point)
                return nil
            }
        }

        deinit {
            if let monitor { NSEvent.removeMonitor(monitor) }
        }
    }
}

extension View {
    /// Calls `action` with scroll events over this view (location in the view's top-left coordinates).
    func onScrollWheel(_ action: @escaping (NSEvent, CGPoint) -> Void) -> some View {
        background(ScrollWheelHandler(onScroll: action))
    }
}

extension Viewport {
    /// Trackpad scroll pans; a mouse wheel, or scrolling with ⌘/⌥ held, zooms around the cursor.
    mutating func handleScroll(_ event: NSEvent, at point: CGPoint, limits: ClosedRange<CGFloat>) {
        let zoomModifier = !event.modifierFlags.intersection([.command, .option]).isEmpty
        if zoomModifier || !event.hasPreciseScrollingDeltas {
            let delta = event.hasPreciseScrollingDeltas ? event.scrollingDeltaY : event.scrollingDeltaY * 8
            guard delta != 0 else { return }
            zoom(by: exp(delta * 0.01), anchor: point, limits: limits)
        } else {
            offset.width += event.scrollingDeltaX
            offset.height += event.scrollingDeltaY
        }
    }
}
