import SwiftUI

@main
struct SiEDAApp: App {
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
                .preferredColorScheme(AppearancePreference(rawValue: appearance)?.colorScheme ?? .dark)
                .tint(Theme.blue)
                .frame(minWidth: 1100, minHeight: 700)
        }
        .windowToolbarStyle(.unified)
        .commands { SiEDACommands(store: store, agents: agents) }

        Settings {
            SettingsView()
                .environmentObject(settings)
                .environmentObject(store)
                .preferredColorScheme(AppearancePreference(rawValue: appearance)?.colorScheme ?? .dark)
                .tint(Theme.blue)
        }
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

    var body: some Commands {
        CommandGroup(replacing: .newItem) {
            Button("New Project") { store.newProject() }
                .keyboardShortcut("n")
            Button("Open…") { store.openProject() }
                .keyboardShortcut("o")
        }
        CommandGroup(replacing: .saveItem) {
            Button("Save") { store.save() }
                .keyboardShortcut("s")
            Button("Save As…") { store.saveAs() }
                .keyboardShortcut("s", modifiers: [.command, .shift])
            Divider()
            Button("Export Fabrication Package…") { store.exportFabricationPackage() }
                .keyboardShortcut("e", modifiers: [.command, .shift])
            Menu("Export") {
                ForEach(ExportFormat.allCases) { format in
                    Button(format.displayName) { store.export(format) }
                }
            }
        }
        CommandGroup(replacing: .undoRedo) {
            Button("Undo") { store.undo() }
                .keyboardShortcut("z")
                .disabled(!store.canUndo)
            Button("Redo") { store.redo() }
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .disabled(!store.canRedo)
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
            Button("Autoroute Board") { Task { await store.autoRoute() } }
                .keyboardShortcut("r", modifiers: [.command, .shift])
            Button("Run Design Rule Check") { store.runDRC(); store.workspace = .checks }
            Divider()
            Button("Cancel AI Generation") { agents.cancel() }
                .disabled(!agents.isRunning)
        }
        CommandMenu("Workspace") {
            ForEach(Array(Workspace.allCases.enumerated()), id: \.element) { index, workspace in
                Button(workspace.title) { store.workspace = workspace }
                    .keyboardShortcut(KeyEquivalent(Character("\(index + 1)")), modifiers: .command)
            }
        }
    }
}
