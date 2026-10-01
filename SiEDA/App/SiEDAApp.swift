import AppKit
import SwiftUI

/// Guards against losing work: quitting (or closing the last window) asks to save an edited design.
final class AppDelegate: NSObject, NSApplicationDelegate {
    weak var store: DesignStore?

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }

    @MainActor
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        guard let store else { return .terminateNow }
        return store.confirmDiscardChanges() ? .terminateNow : .terminateCancel
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
            Button("Run Electrical Rule Check") { store.runERC(); store.workspace = .checks }
                .keyboardShortcut("k", modifiers: [.command, .shift])
            Button("DC Operating Point") {
                Task { await store.simulateDC(); store.workspace = .simulation }
            }
            .keyboardShortcut("d", modifiers: [.command, .shift])
            Divider()
            Button("Auto-Place Footprints") { store.autoPlace(all: true) }
            Button("Fit Board to Components") { store.fitBoard() }
            Button("Autoroute Board") { Task { await store.autoRoute() } }
                .keyboardShortcut("r", modifiers: [.command, .shift])
            Button("Run Design Rule Check") { store.runDRC(); store.workspace = .checks }
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
            Button("Validate Circuit") { store.runValidation(); store.workspace = .checks }
                .keyboardShortcut("l", modifiers: [.command, .shift])
            Button("Verify Design") {
                Task { await store.runVerification(); store.workspace = .checks }
            }
            .keyboardShortcut("v", modifiers: [.command, .shift])
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
