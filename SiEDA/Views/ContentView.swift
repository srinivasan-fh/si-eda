import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var agents: AgentOrchestrator
    @State private var showInspector = true

    var body: some View {
        NavigationSplitView {
            SidebarView()
                .navigationSplitViewColumnWidth(min: 210, ideal: 240, max: 320)
        } detail: {
            VStack(spacing: 0) {
                workspaceView
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                StatusBar()
            }
            .background(Theme.navy)
            .inspector(isPresented: $showInspector) {
                InspectorView()
                    .inspectorColumnWidth(min: 240, ideal: 280, max: 380)
            }
        }
        .navigationTitle(store.windowTitle)
        .toolbar { toolbarContent }
        .alert(item: $store.alert) { item in
            Alert(title: Text(item.title), message: Text(item.message), dismissButton: .default(Text("OK")))
        }
        .onAppear {
            store.aiEnabled = settings.aiEnabled
            if store.snapshot.components.isEmpty { store.workspace = store.startWorkspace }
        }
        .onChange(of: settings.aiEnabled) { _, enabled in
            if !enabled { agents.cancel() }
            store.aiEnabled = enabled
        }
    }

    @ViewBuilder
    private var workspaceView: some View {
        switch store.workspace {
        case .promptStudio: PromptStudioView()
        case .schematic: SchematicEditorView()
        case .library: ComponentLibraryView()
        case .pcb: PCBEditorView()
        case .threeD: Board3DWorkspace()
        case .simulation: SimulationView()
        case .checks: RuleCheckView()
        }
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .navigation) {
            Button { store.undo() } label: { Label("Undo", systemImage: "arrow.uturn.backward") }
                .disabled(!store.canUndo)
                .help("Undo (⌘Z)")
            Button { store.redo() } label: { Label("Redo", systemImage: "arrow.uturn.forward") }
                .disabled(!store.canRedo)
                .help("Redo (⇧⌘Z)")
        }
        ToolbarItem(placement: .principal) {
            Picker("Workspace", selection: $store.workspace) {
                ForEach(Workspace.visible(aiEnabled: settings.aiEnabled)) { w in
                    Label(w.title, systemImage: w.systemImage).tag(w)
                }
            }
            .pickerStyle(.segmented)
            .labelStyle(.iconOnly)
            .help("Switch workspace (⌘1 – ⌘7)")
        }
        ToolbarItemGroup(placement: .primaryAction) {
            Menu {
                Toggle("AI Assistance", isOn: $settings.aiEnabled)
                if settings.aiEnabled {
                    Divider()
                    Picker("AI Provider", selection: $settings.provider) {
                        ForEach(AIProviderKind.allCases) { kind in
                            Label(kind.displayName, systemImage: kind.systemImage).tag(kind)
                        }
                    }
                }
                Divider()
                SettingsLink { Text("Configure Models…") }
            } label: {
                if settings.aiEnabled {
                    Label(settings.provider.shortName + " · " + settings.model(for: settings.provider),
                          systemImage: settings.provider.systemImage)
                } else {
                    Label("AI Off", systemImage: "sparkles.slash")
                }
            }
            .help(settings.aiEnabled ? "AI model used by the design agents" : "AI assistance is off — SiEDA works fully manually")

            Button {
                store.runERC()
                store.runDRC()
                store.workspace = .checks
            } label: { Label("Check", systemImage: "checkmark.seal") }
                .help("Run ERC and DRC")

            Button {
                Task {
                    await store.simulateDC()
                    store.workspace = .simulation
                }
            } label: { Label("Simulate", systemImage: "play.circle") }
                .help("DC operating point (⇧⌘D)")

            Button { store.exportFabricationPackage() } label: {
                Label("Export", systemImage: "shippingbox")
            }
            .help("Export fabrication package (⇧⌘E)")

            Button { showInspector.toggle() } label: { Label("Inspector", systemImage: "sidebar.trailing") }
        }
    }
}

struct SidebarView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings

    var body: some View {
        List {
            Section("Workspaces") {
                ForEach(Workspace.visible(aiEnabled: settings.aiEnabled)) { w in
                    Button {
                        store.workspace = w
                    } label: {
                        Label(w.title, systemImage: w.systemImage)
                            .foregroundStyle(store.workspace == w ? Theme.skyBlue : Theme.textSecondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .listRowBackground(store.workspace == w ? Theme.blue.opacity(0.22) : Color.clear)
                }
            }
            Section("Components (\(store.snapshot.components.filter { !$0.componentKind.isVirtual }.count))") {
                ForEach(store.snapshot.components.filter { !$0.componentKind.isVirtual }) { c in
                    Button {
                        store.select(component: c.id)
                        // Make the selection visible: jump to the schematic unless an editor that shows it is open.
                        if ![.schematic, .pcb, .threeD].contains(store.workspace) { store.workspace = .schematic }
                    } label: {
                        HStack {
                            Image(systemName: c.componentKind.systemImage)
                                .foregroundStyle(Theme.blue)
                                .frame(width: 18)
                            Text(c.ref).font(.system(.body, design: .monospaced)).foregroundStyle(Theme.textPrimary)
                            Spacer()
                            Text(c.value).foregroundStyle(Theme.textMuted).lineLimit(1)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .listRowBackground(store.selection.contains(c.id) ? Theme.blue.opacity(0.22) : Color.clear)
                }
            }
            Section("Nets (\(store.snapshot.nets.filter { $0.pinCount > 1 }.count))") {
                ForEach(store.snapshot.nets.filter { $0.pinCount > 1 }) { net in
                    HStack {
                        Image(systemName: net.ground ? "arrow.down.to.line" : "point.topleft.down.to.point.bottomright.curvepath")
                            .foregroundStyle(Theme.skyBlue)
                            .frame(width: 18)
                        Text(net.name).font(.system(.callout, design: .monospaced)).foregroundStyle(Theme.textSecondary)
                        Spacer()
                        if let v = store.dcResult?.voltage(net: net.index) {
                            Text(EngineeringFormat.string(v, unit: "V")).font(.caption.monospacedDigit())
                                .foregroundStyle(Theme.probe)
                        }
                    }
                }
            }
        }
        .listStyle(.sidebar)
        .scrollContentBackground(.hidden)
        .background(Theme.deepBlue.opacity(0.6))
    }
}

struct StatusBar: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var agents: AgentOrchestrator
    @EnvironmentObject private var settings: AISettings

    var body: some View {
        HStack(spacing: 14) {
            if store.isBusy || agents.isRunning {
                ProgressView().controlSize(.small)
                Text(store.isBusy ? store.busyMessage : "AI agents working · \(agents.activeModel)")
                    .foregroundStyle(Theme.skyBlue)
                    .lineLimit(1)
                    .truncationMode(.tail)
            } else {
                Image(systemName: "bolt.horizontal.circle").foregroundStyle(Theme.blue)
                Text(store.statusMessage)
                    .foregroundStyle(Theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            Spacer(minLength: 12)
            // Full statistics when there is room, a compact summary otherwise (never wraps).
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 14) {
                    partsLabel
                    netsLabel
                    boardLabel
                    Label("\(store.snapshot.tracks.count) tracks · \(store.snapshot.vias.count) vias", systemImage: "line.diagonal")
                    modeLabel
                    Text("Core \(EDAEngine.coreVersion)").foregroundStyle(Theme.textMuted)
                }
                HStack(spacing: 12) {
                    partsLabel
                    boardLabel
                    modeLabel
                }
                modeLabel
            }
            .fixedSize(horizontal: false, vertical: true)
        }
        .font(.caption)
        .foregroundStyle(Theme.lightBlue)
        .labelStyle(.titleAndIcon)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Theme.deepBlue)
        .overlay(Rectangle().frame(height: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .top)
    }

    private var partsLabel: some View {
        Label("\(store.snapshot.components.filter { !$0.componentKind.isVirtual }.count) parts", systemImage: "cpu")
            .lineLimit(1)
    }

    private var netsLabel: some View {
        Label("\(store.snapshot.nets.filter { $0.pinCount > 1 }.count) nets", systemImage: "point.3.connected.trianglepath.dotted")
            .lineLimit(1)
    }

    private var boardLabel: some View {
        Label(String(format: "%.0f × %.0f mm · %dL", store.snapshot.board.width, store.snapshot.board.height,
                     store.snapshot.board.layerCount), systemImage: "square.dashed")
            .lineLimit(1)
    }

    private var modeLabel: some View {
        Label(settings.aiEnabled ? "AI on" : "Manual mode", systemImage: settings.aiEnabled ? "sparkles" : "hand.raised")
            .lineLimit(1)
    }
}
