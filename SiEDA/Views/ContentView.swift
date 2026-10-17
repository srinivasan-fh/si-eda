import AppKit
import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var agents: AgentOrchestrator
    /// The inspector starts hidden on laptop-size screens (decided once at launch: measuring the window and
    /// toggling panels during layout makes AppKit loop on constraint updates and abort).
    @State private var showInspector = ContentView.startsWithInspector
    /// Whether the inspector was open when focus mode hid it (restored on leaving).
    @State private var inspectorBeforeFocus = false

    static var startsWithInspector: Bool {
        (NSScreen.main?.visibleFrame.width ?? LayoutMetrics.defaultWindow.width) >= LayoutMetrics.inspectorWidthThreshold
    }

    var body: some View {
        // No workspace sidebar: the toolbar's workspace picker switches workspaces, and each editor keeps its own
        // side panel (the schematic's device palette, expanded or icons only).
        VStack(spacing: 0) {
            workspaceView
                .frame(maxWidth: .infinity, maxHeight: .infinity)
                .environment(\.editorFocusMode, store.focusMode)
            if !store.focusMode {
                StatusBar()
            }
        }
        .overlay(alignment: .topTrailing) {
            if store.focusMode { FocusModeBadge() }
        }
        .background(Theme.navy)
        .inspector(isPresented: $showInspector) {
            InspectorView()
                .inspectorColumnWidth(min: 240, ideal: 280, max: 380)
        }
        .navigationTitle(store.windowTitle)
        .onChange(of: store.inspectorRevealToken) { _, _ in showInspector = true }
        .toolbar { toolbarContent }
        .toolbar(store.focusMode ? .hidden : .visible, for: .windowToolbar)
        .background(FullScreenObserver { full in store.focusMode = full && FocusMode.hidesPanels })
        .onChange(of: store.focusMode) { _, focus in applyFocus(focus) }
        // Solid blue toolbar: the default translucent one takes its colour from the desktop picture.
        .toolbarBackground(Theme.deepBlue, for: .windowToolbar)
        .toolbarBackground(.visible, for: .windowToolbar)

        .overlay { if store.showCommandPalette { CommandPaletteView() } }
        .sheet(item: $store.designDiff) { DesignDiffView(diff: $0) }
        .sheet(isPresented: $store.showReview) { DesignReviewView().environmentObject(store) }
        .sheet(isPresented: $store.showGerberViewer) { GerberViewerView().environmentObject(store) }
        .sheet(isPresented: $store.showWelcomeTour) { WelcomeTourView() }
        .sheet(isPresented: $store.showShortcuts) { KeyboardShortcutsView() }
        .sheet(isPresented: $store.showPerformance) { PerformanceView().environmentObject(store) }
        .sheet(isPresented: Binding(get: { store.variantMatrix != nil }, set: { if !$0 { store.variantMatrix = nil } })) {
            if let matrix = store.variantMatrix { VariantMatrixView(matrix: matrix) }
        }
        .alert(item: $store.alert) { item in
            Alert(title: Text(item.title), message: Text(item.message), dismissButton: .default(Text("OK")))
        }
        // After the first layout pass, not during it: writing published state while the sidebar table is being
        // populated makes AppKit warn about reentrant table updates.
        .task {
            store.aiEnabled = settings.aiEnabled
            if store.snapshot.components.isEmpty { store.workspace = store.startWorkspace }
            if !CrashReporter.isRunningTests, !UserDefaults.standard.bool(forKey: WelcomeTourView.shownKey) {
                store.showWelcomeTour = true
            }
        }
        .onChange(of: settings.aiEnabled) { _, enabled in
            if !enabled { agents.cancel() }
            store.aiEnabled = enabled
        }
    }

    /// Focus mode hides the inspector, and puts them back as they were on leaving.
    private func applyFocus(_ focus: Bool) {
        if focus {
            inspectorBeforeFocus = showInspector
            showInspector = false
        } else {
            showInspector = inspectorBeforeFocus
        }
    }

    private var workspaceView: some View { Self.workspaceView(store.workspace) }

    /// The editor of a workspace (also used by the layout tests).
    @ViewBuilder
    static func workspaceView(_ workspace: Workspace) -> some View {
        switch workspace {
        case .promptStudio: PromptStudioView()
        case .schematic: SchematicEditorView()
        case .library: ComponentLibraryView()
        case .pcb: PCBEditorView()
        case .threeD: Board3DWorkspace()
        case .simulation: SimulationView()
        case .checks: RuleCheckView()
        case .bom: BomView()
        }
    }

    @ToolbarContentBuilder
    private var toolbarContent: some ToolbarContent {
        ToolbarItemGroup(placement: .navigation) {
            // Like ⌘Z: while a text field is being edited, undo belongs to the text.
            Button {
                if TextEditingFocus.isActive { TextEditingFocus.send("undo:") } else { store.undo() }
            } label: { Label("Undo", systemImage: "arrow.uturn.backward") }
                .disabled(!store.canUndo)
                .help("Undo (⌘Z)")
            Button {
                if TextEditingFocus.isActive { TextEditingFocus.send("redo:") } else { store.redo() }
            } label: { Label("Redo", systemImage: "arrow.uturn.forward") }
                .disabled(!store.canRedo)
                .help("Redo (⇧⌘Z)")
        }
        ToolbarItem(placement: .principal) {
            Picker("Workspace", selection: $store.workspace) {
                ForEach(Workspace.visible(aiEnabled: settings.aiEnabled)) { w in
                    Label(LocalizedStringKey(w.title), systemImage: w.systemImage).tag(w)
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
                if !store.snapshot.pads.isEmpty { store.runDRC() }
                store.showChecks(.rules)
            } label: { Label("Check", systemImage: "checkmark.seal") }
                .help("Run ERC and DRC")

            Button {
                Task {
                    await store.simulateDC()
                    store.workspace = .simulation
                }
            } label: { Label("Simulate", systemImage: "play.circle") }
                .help("DC operating point (⇧⌘D)")

            Button { Task { await store.exportFabricationPackage() } } label: {
                Label("Export", systemImage: "shippingbox")
            }
            .help("Export fabrication package (⇧⌘E)")

            Button { showInspector.toggle() } label: { Label("Inspector", systemImage: "sidebar.trailing") }
        }
    }
}

struct StatusBar: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var agents: AgentOrchestrator
    @EnvironmentObject private var settings: AISettings

    var body: some View {
        HStack(spacing: 14) {
            if store.isBusy, let progress = store.routeProgress {
                RouteProgressView(progress: progress) { store.cancelAutoRoute() }
            } else if store.isBusy || agents.isRunning {
                ProgressView().controlSize(.small)
                Text(store.isBusy ? store.busyMessage : "AI agents working · \(agents.activeModel)")
                    .foregroundStyle(Theme.skyBlue)
                    .lineLimit(1)
                    .truncationMode(.tail)
                if store.isBusy, let since = store.busySince {
                    // Elapsed time once a task takes longer than a second.
                    TimelineView(.periodic(from: since, by: 1)) { context in
                        let seconds = Int(context.date.timeIntervalSince(since))
                        if seconds >= 1 { Text(verbatim: "\(seconds) s").monospacedDigit().foregroundStyle(Theme.textMuted) }
                    }
                }
                if store.busyStoppable {
                    Button("Stop", action: store.stopBusyTask)
                        .controlSize(.small)
                        .help("End the simulation early")
                }
            } else {
                Image(systemName: "bolt.horizontal.circle").foregroundStyle(Theme.blue)
                Text(store.statusMessage)
                    .foregroundStyle(Theme.textSecondary)
                    .lineLimit(1)
                    .truncationMode(.tail)
            }
            Spacer(minLength: 12)
            LiveCollaborationIndicator(collaboration: store.collaboration)
            MCPStatusIndicator()
            // Full statistics when there is room, a compact summary otherwise (never wraps).
            ViewThatFits(in: .horizontal) {
                HStack(spacing: 14) {
                    partsLabel
                    netsLabel
                    boardLabel
                    Label("\(store.snapshot.tracks.count) tracks · \(store.snapshot.vias.count) vias", systemImage: "line.diagonal")
                    verificationLabel
                    modeLabel
                    Text("Core \(EDAEngine.coreVersion)").foregroundStyle(Theme.textMuted)
                }
                HStack(spacing: 12) {
                    partsLabel
                    boardLabel
                    verificationLabel
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

    /// Last verification verdict; opens Design Checks.
    @ViewBuilder private var verificationLabel: some View {
        if let report = store.verificationReport {
            Button { store.showChecks(.verification) } label: {
                Label(store.verificationIsStale ? "Verification out of date" : "Verification: \(report.verdict.title)",
                      systemImage: store.verificationIsStale ? "clock.arrow.circlepath" : report.verdict.systemImage)
            }
            .buttonStyle(.plain)
            .lineLimit(1)
            .help("Open the design verification report")
        }
    }

    private var modeLabel: some View {
        Label(settings.aiEnabled ? "AI on" : "Manual mode", systemImage: settings.aiEnabled ? "sparkles" : "hand.raised")
            .lineLimit(1)
    }
}
