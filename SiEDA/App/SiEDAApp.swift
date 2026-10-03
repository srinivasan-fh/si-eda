import AppKit
import SwiftUI

/// Guards against losing work (quitting or closing the window asks to save an edited design) and opens .siedaproj
/// files handed over by Finder, the Dock or Open Recent.
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var store: DesignStore? {
        didSet {
            if let store, store !== oldValue { attachDiagnostics(to: store) }
            openPending()
        }
    }
    /// A file opened before the window (and its store) existed — opened as soon as the store is attached.
    private var pendingURL: URL?
    private var crashReporter: CrashReporter?
    private let watchdog = MainThreadWatchdog()
    private let memoryMonitor = MemoryPressureMonitor()

    func applicationWillFinishLaunching(_ notification: Notification) {
        // Unit tests drive the app themselves: no handlers, monitors or launch dialogs there.
        guard !CrashReporter.isRunningTests else { return }
        crashReporter = CrashReporter.installShared()
        watchdog.start()
        memoryMonitor.start()
    }

    @MainActor
    func applicationWillTerminate(_ notification: Notification) {
        watchdog.stop()
        memoryMonitor.stop()
        // A clean quit: unsaved changes were saved or deliberately discarded, so there is nothing to recover.
        if let recovery = store?.recovery {
            recovery.clear()
            recovery.flush()
        }
        crashReporter?.endSession()
    }

    /// After an abnormal end of the previous session: offers the work that was autosaved and the crash report, then
    /// starts autosaving this session's unsaved work.
    private func attachDiagnostics(to store: DesignStore) {
        guard let reporter = crashReporter else { return }
        let recovery = CrashRecovery(directory: reporter.recoveryDirectory)
        let report = reporter.lastSessionReport
        let pending = recovery.pending()
        DispatchQueue.main.async {
            MainActor.assumeIsolated {
                if report != nil || pending != nil {
                    Self.offerRecovery(report: report, pending: pending, store: store)
                }
                if pending != nil && !store.isDirty { recovery.clear() }  // declined or not restorable
                store.recovery = recovery
            }
        }
    }

    @MainActor
    private static func offerRecovery(report: URL?, pending: CrashRecovery.Pending?, store: DesignStore) {
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = report != nil ? "SiEDA quit unexpectedly" : "Unsaved work was recovered"
        var info = report != nil ? "A crash report was saved on this Mac (nothing is sent anywhere)." : ""
        if let pending {
            let when = pending.savedAt.map { DateFormatter.localizedString(from: $0, dateStyle: .none, timeStyle: .short) }
            info += (info.isEmpty ? "" : "\n\n") + "Unsaved changes" + (when.map { " from \($0)" } ?? "")
                + " can be restored" + (pending.documentURL.map { " to “\($0.lastPathComponent)”" } ?? "") + "."
        }
        alert.informativeText = info
        var actions: [() -> Void] = []
        if let pending {
            alert.addButton(withTitle: "Restore Unsaved Work")
            actions.append { store.restoreRecovered(pending) }
        }
        if let report {
            alert.addButton(withTitle: "Show Report")
            actions.append { NSWorkspace.shared.activateFileViewerSelecting([report]) }
        }
        alert.addButton(withTitle: pending != nil ? "Discard" : "OK")
        actions.append {}
        let index = alert.runModal().rawValue - NSApplication.ModalResponse.alertFirstButtonReturn.rawValue
        if actions.indices.contains(index) { actions[index]() }
    }

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
        CommandGroup(after: .help) {
            Button("Show Crash Reports") {
                let folder = (CrashReporter.shared?.directory ?? CrashReporter.defaultDirectory)
                    .appendingPathComponent("Reports", isDirectory: true)
                try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
                NSWorkspace.shared.open(folder)
            }
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
            Button("Add Thermal Vias") { store.addThermalVias() }
                .help("Stitch vias at the selected (or every) power MOSFET's drain / tab pad")
            Menu("Robot Platform") {
                Toggle("Not a robot", isOn: Binding(get: { store.snapshot.robotPlatform.isEmpty },
                                                    set: { if $0 { store.setRobotPlatform("") } }))
                ForEach(store.robotSegments().platforms) { platform in
                    Toggle(platform.name, isOn: Binding(get: { store.snapshot.robotPlatform == platform.id },
                                                        set: { if $0 { store.setRobotPlatform(platform.id) } }))
                }
            }
            Menu("ECU Type") {
                Toggle("Not an ECU", isOn: Binding(get: { store.snapshot.ecuType.isEmpty },
                                                   set: { if $0 { store.setEcuType("") } }))
                ForEach(store.ecuSegments().platforms) { type in
                    Toggle(type.name, isOn: Binding(get: { store.snapshot.ecuType == type.id },
                                                    set: { if $0 { store.setEcuType(type.id) } }))
                }
            }
            Menu("Aerospace Mission") {
                Toggle("Not aerospace", isOn: Binding(get: { store.snapshot.aerospaceMission.isEmpty },
                                                      set: { if $0 { store.setAerospaceMission("") } }))
                ForEach(store.aerospaceSegments().platforms) { mission in
                    Toggle(mission.name, isOn: Binding(get: { store.snapshot.aerospaceMission == mission.id },
                                                       set: { if $0 { store.setAerospaceMission(mission.id) } }))
                }
            }
            Menu("Naval Platform") {
                Toggle("Not naval", isOn: Binding(get: { store.snapshot.navalPlatform.isEmpty },
                                                  set: { if $0 { store.setNavalPlatform("") } }))
                ForEach(store.navalSegments().platforms) { platform in
                    Toggle(platform.name, isOn: Binding(get: { store.snapshot.navalPlatform == platform.id },
                                                        set: { if $0 { store.setNavalPlatform(platform.id) } }))
                }
            }
            Menu("Medical Device Class") {
                Toggle("Not medical", isOn: Binding(get: { store.snapshot.medicalClass.isEmpty },
                                                    set: { if $0 { store.setMedicalClass("") } }))
                ForEach(store.medicalSegments().platforms) { cls in
                    Toggle(cls.name, isOn: Binding(get: { store.snapshot.medicalClass == cls.id },
                                                   set: { if $0 { store.setMedicalClass(cls.id) } }))
                }
            }
            Menu("Retail Device") {
                Toggle("Not retail", isOn: Binding(get: { store.snapshot.retailDevice.isEmpty },
                                                   set: { if $0 { store.setRetailDevice("") } }))
                ForEach(store.retailSegments().platforms) { device in
                    Toggle(device.name, isOn: Binding(get: { store.snapshot.retailDevice == device.id },
                                                      set: { if $0 { store.setRetailDevice(device.id) } }))
                }
            }
            Menu("Appliance Type") {
                Toggle("Not an appliance", isOn: Binding(get: { store.snapshot.applianceType.isEmpty },
                                                         set: { if $0 { store.setApplianceType("") } }))
                ForEach(store.applianceSegments().platforms) { type in
                    Toggle(type.name, isOn: Binding(get: { store.snapshot.applianceType == type.id },
                                                    set: { if $0 { store.setApplianceType(type.id) } }))
                }
            }
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
