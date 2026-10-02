import AppKit
import SwiftUI

/// Guards against losing work (quitting or closing the window asks to save an edited design) and opens .siedaproj
/// files handed over by Finder, the Dock or Open Recent.
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var store: DesignStore? {
        didSet { openPending() }
    }
    /// A file opened before the window (and its store) existed — opened as soon as the store is attached.
    private var pendingURL: URL?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let store, !store.closeConfirmed else { return .terminateNow }
        return store.confirmDiscardChanges() ? .terminateNow : .terminateCancel
    }

    func application(_ application: NSApplication, open urls: [URL]) {
        guard let url = urls.first(where: { $0.pathExtension.lowercased() == "siedaproj" }) ?? urls.first else { return }
        pendingURL = url
        openPending()
    }

    private func openPending() {
        guard let url = pendingURL, let store else { return }
        pendingURL = nil
        MainActor.assumeIsolated {
            if store.confirmDiscardChanges() { store.open(url: url) }
        }
    }
}

/// Asks to save before the window closes (red button, ⌘W). Installed as a proxy in front of SwiftUI's own window
/// delegate, to which every other delegate call is forwarded.
final class WindowCloseGuard: NSObject, NSWindowDelegate {
    /// Weak reference holder readable from the (nonisolated) Objective-C forwarding overrides.
    final class Box {
        weak var delegate: NSObjectProtocol?
    }
    private let originalBox = Box()
    var original: NSWindowDelegate? {
        get { originalBox.delegate as? NSWindowDelegate }
        set { originalBox.delegate = newValue }
    }
    weak var store: DesignStore?

    override func responds(to selector: Selector!) -> Bool {
        super.responds(to: selector) || (originalBox.delegate?.responds(to: selector) ?? false)
    }

    override func forwardingTarget(for selector: Selector!) -> Any? {
        if let target = originalBox.delegate, target.responds(to: selector) { return target }
        return super.forwardingTarget(for: selector)
    }

    func windowShouldClose(_ sender: NSWindow) -> Bool {
        if original?.windowShouldClose?(sender) == false { return false }
        guard let store else { return true }
        return MainActor.assumeIsolated {
            let ok = store.confirmDiscardChanges()
            if ok { store.closeConfirmed = true }
            return ok
        }
    }
}

/// Invisible view that installs the `WindowCloseGuard` on its window.
struct WindowCloseGuardInstaller: NSViewRepresentable {
    let store: DesignStore

    final class Coordinator {
        let guardDelegate = WindowCloseGuard()
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> NSView {
        let view = NSView()
        DispatchQueue.main.async { install(on: view.window, context.coordinator) }
        return view
    }

    func updateNSView(_ view: NSView, context: Context) {
        DispatchQueue.main.async { install(on: view.window, context.coordinator) }
    }

    private func install(on window: NSWindow?, _ coordinator: Coordinator) {
        guard let window else { return }
        let proxy = coordinator.guardDelegate
        proxy.store = store
        if window.delegate !== proxy {
            proxy.original = window.delegate
            window.delegate = proxy
        }
    }
}

@main
struct SiEDAApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var store = DesignStore()
    @StateObject private var settings = AISettings()
    @StateObject private var agents = AgentOrchestrator()
    @AppStorage("appearance") private var appearance = AppearancePreference.dark.rawValue

    var body: some Scene {
        WindowGroup("SiEDA") {
            ContentView()
                .environmentObject(store)
                .environmentObject(settings)
                .environmentObject(agents)
                .preferredColorScheme((AppearancePreference(rawValue: appearance) ?? .dark).colorScheme)
                .tint(Theme.blue)
                .documentWindowFrame()
                .background(WindowCloseGuardInstaller(store: store))
                .onAppear { appDelegate.store = store }
        }
        .defaultSize(width: LayoutMetrics.defaultWindow.width, height: LayoutMetrics.defaultWindow.height)
        .windowToolbarStyle(.unified)
        .commands { SiEDACommands(store: store, agents: agents, settings: settings) }

        Settings {
            SettingsView()
                .environmentObject(settings)
                .environmentObject(store)
                .preferredColorScheme((AppearancePreference(rawValue: appearance) ?? .dark).colorScheme)
                .tint(Theme.blue)
        }
    }
}

/// Detects whether keyboard focus is in a text field/view (AppKit field editor) and forwards actions to it.
enum TextEditingFocus {
    @MainActor static var isActive: Bool { NSApp.keyWindow?.firstResponder is NSText }

    @MainActor static func send(_ action: String) {
        NSApp.sendAction(Selector((action)), to: nil, from: nil)
    }
}

enum AppearancePreference: String, CaseIterable, Identifiable {
    case dark, light, system
    var id: String { rawValue }
    var title: String { rawValue.capitalized }
    var colorScheme: ColorScheme? {
        switch self {
        case .dark: return .dark
        case .light: return .light
        case .system: return nil
        }
    }
}

struct SiEDACommands: Commands {
    @ObservedObject var store: DesignStore
    @ObservedObject var agents: AgentOrchestrator
    @ObservedObject var settings: AISettings

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Project") { store.newProject() }
                .keyboardShortcut("n")
            Menu("New from Example") {
                ForEach(OfflineProvider.categories, id: \.self) { category in
                    Section(category) {
                        ForEach(OfflineProvider.examples(in: category), id: \.plan.title) { template in
                            Button(template.plan.title) { store.loadExample(template.industryPlan) }
                        }
                    }
                }
            }
            Button("Open…") { store.openProject() }
                .keyboardShortcut("o")
            Menu("Open Recent") {
                let recents = NSDocumentController.shared.recentDocumentURLs
                ForEach(recents, id: \.self) { url in
                    Button(url.deletingPathExtension().lastPathComponent) {
                        if store.confirmDiscardChanges() { store.open(url: url) }
                    }
                }
                if !recents.isEmpty {
                    Divider()
                    Button("Clear Menu") { NSDocumentController.shared.clearRecentDocuments(nil) }
                }
            }
        }
        CommandGroup(replacing: .saveItem) {
            Button("Save") { store.save() }
                .keyboardShortcut("s")
            Button("Save As…") { store.saveAs() }
                .keyboardShortcut("s", modifiers: [.command, .shift])
            Divider()
            Button("Export Fabrication Package…") { Task { await store.exportFabricationPackage() } }
                .keyboardShortcut("e", modifiers: [.command, .shift])
            Menu("Export") {
                ForEach(ExportFormat.allCases) { format in
                    Button(format.displayName) { store.export(format) }
                }
            }
        }
        CommandGroup(replacing: .undoRedo) {
            // While a text field is being edited, ⌘Z/⇧⌘Z belong to the text, not to the design.
            Button("Undo") {
                if TextEditingFocus.isActive { TextEditingFocus.send("undo:") } else { store.undo() }
            }
            .keyboardShortcut("z")
            Button("Redo") {
                if TextEditingFocus.isActive { TextEditingFocus.send("redo:") } else { store.redo() }
            }
            .keyboardShortcut("z", modifiers: [.command, .shift])
        }
        CommandGroup(after: .toolbar) {
            let canvas = store.workspace.hasCanvas
            Divider()
            Button("Zoom In") { store.requestView(.zoomIn) }
                .keyboardShortcut("=", modifiers: .command)
                .disabled(!canvas)
            Button("Zoom Out") { store.requestView(.zoomOut) }
                .keyboardShortcut("-", modifiers: .command)
                .disabled(!canvas)
            Button("Zoom to Fit") { store.requestView(.fit) }
                .keyboardShortcut("0", modifiers: .command)
                .disabled(!canvas)
            Button("Zoom to Selection") { store.requestView(.fitSelection) }
                .keyboardShortcut("0", modifiers: [.command, .option])
                .disabled(!canvas)
            Button("Zoom to Area") { store.requestView(.zoomArea) }
                .disabled(!canvas)
            Menu("Zoom Level") {
                ForEach(ZoomControls.presets, id: \.self) { level in
                    Button(ZoomControls.percent(level)) { store.requestView(.setLevel(level)) }
                }
            }
            .disabled(!canvas)
            Toggle("Show Navigator", isOn: $store.showNavigator)
            Divider()
        }
        CommandMenu("Design") {
            Button("Run Electrical Rule Check") { store.runERC(); store.showChecks(.rules) }
                .keyboardShortcut("k", modifiers: [.command, .shift])
            Button("DC Operating Point") {
                Task { await store.simulateDC(); store.workspace = .simulation }
            }
            .keyboardShortcut("d", modifiers: [.command, .shift])
            Divider()
            Button("Auto-Place Footprints") { store.autoPlace(all: true) }
            Button("Fit Board to Components") { store.fitBoard() }
            Button("Auto Route Board") { Task { await store.autoRouteBoard() } }
                .keyboardShortcut("r", modifiers: [.command, .shift])
            Button("Run Design Rule Check") { store.runDRC(); store.showChecks(.rules) }
            Menu("Industry Profile") {
                ForEach(StandardLibrary.industries) { profile in
                    Toggle(profile.name, isOn: Binding(
                        get: { store.snapshot.industry == profile.id },
                        set: { if $0 { store.setIndustry(profile) } }
                    ))
                }
            }
            Menu("Design Rules") {
                ForEach(StandardLibrary.rulePresets) { preset in
                    Toggle(preset.name, isOn: Binding(
                        get: { store.snapshot.board.rulePreset == preset.name },
                        set: { if $0 { store.applyRulePreset(preset) } }
                    ))
                }
            }
            Divider()
            Button("Validate Circuit") { store.runValidation(); store.showChecks(.rules) }
                .keyboardShortcut("l", modifiers: [.command, .shift])
            Button("Verify Design") {
                Task { await store.runVerification(); store.showChecks(.verification) }
            }
            // ⌥⌘V: ⇧⌘V is Paste and Match Style in text fields.
            .keyboardShortcut("v", modifiers: [.command, .option])
            Divider()
            Toggle("AI Assistance", isOn: $settings.aiEnabled)
                .keyboardShortcut("a", modifiers: [.command, .option])
            Button("Cancel AI Generation") { agents.cancel() }
                .disabled(!agents.isRunning)
        }
        CommandMenu("Workspace") {
            ForEach(Array(Workspace.visible(aiEnabled: settings.aiEnabled).enumerated()), id: \.element) { index, workspace in
                Button(workspace.title) { store.workspace = workspace }
                    .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .command)
            }
        }
    }
}
