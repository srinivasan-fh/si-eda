import AppKit
import SwiftUI

/// Focus mode: the editor alone, full screen. F11, ⌃⌘F, Fn-F or the green button enter it; the sidebar, inspector,
/// window toolbar, status bar and options bars hide, leaving each editor's tool strip and canvas. Moving the pointer
/// to the top edge of an editor shows its options bar again while the pointer is over it.
enum FocusMode {
    /// Settings: full screen hides the panels (on by default). Off: full screen keeps the normal window layout.
    static let hidesPanelsKey = "window.fullScreenHidesPanels"

    static var hidesPanels: Bool {
        UserDefaults.standard.object(forKey: hidesPanelsKey) as? Bool ?? true
    }

    /// F11 as a menu key equivalent (AppKit's private-use code for the function key).
    static let f11 = KeyEquivalent(Character(UnicodeScalar(NSF11FunctionKey)!))

    /// Enters or leaves full screen on the key window; the window notifications switch focus mode.
    @MainActor
    static func toggle() {
        guard let window = NSApp.keyWindow ?? NSApp.mainWindow else { return }
        window.toggleFullScreen(nil)
    }
}

private struct EditorFocusModeKey: EnvironmentKey {
    static let defaultValue = false
}

extension EnvironmentValues {
    /// True in focus mode: options bars collapse to a hover strip.
    var editorFocusMode: Bool {
        get { self[EditorFocusModeKey.self] }
        set { self[EditorFocusModeKey.self] = newValue }
    }
}

/// Reports when its window enters or leaves full screen (whichever way: F11, ⌃⌘F, Fn-F, the green button).
struct FullScreenObserver: NSViewRepresentable {
    var changed: (Bool) -> Void

    func makeNSView(context: Context) -> ObserverView {
        let view = ObserverView()
        view.changed = changed
        return view
    }

    func updateNSView(_ view: ObserverView, context: Context) {
        view.changed = changed
    }

    final class ObserverView: NSView {
        var changed: ((Bool) -> Void)?
        private var tokens: [NSObjectProtocol] = []

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            tokens.forEach(NotificationCenter.default.removeObserver)
            tokens = []
            guard let window else { return }
            let centre = NotificationCenter.default
            tokens.append(centre.addObserver(forName: NSWindow.didEnterFullScreenNotification, object: window,
                                             queue: .main) { [weak self] _ in self?.changed?(true) })
            tokens.append(centre.addObserver(forName: NSWindow.didExitFullScreenNotification, object: window,
                                             queue: .main) { [weak self] _ in self?.changed?(false) })
        }

        deinit { tokens.forEach(NotificationCenter.default.removeObserver) }
    }
}

/// The small "Exit Focus" pill in the corner of focus mode; faint until the pointer is over it.
struct FocusModeBadge: View {
    @State private var hovering = false

    var body: some View {
        Button { FocusMode.toggle() } label: {
            Label("Exit Focus", systemImage: "arrow.down.right.and.arrow.up.left")
                .font(.caption.weight(.semibold))
                .padding(.horizontal, 10)
                .padding(.vertical, 5)
                .background(Capsule().fill(Theme.deepBlue.opacity(0.9)))
                .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.5)))
        }
        .buttonStyle(.plain)
        .foregroundStyle(Theme.textSecondary)
        .opacity(hovering ? 1 : 0.35)
        .onHover { hovering = $0 }
        .help("Leave focus mode (F11, ⌃⌘F)")
        .padding(10)
    }
}
