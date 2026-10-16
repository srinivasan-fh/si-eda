import AppKit
import CoreGraphics
import Foundation
import UniformTypeIdentifiers

enum Workspace: String, CaseIterable, Identifiable {
    case promptStudio, schematic, pcb, threeD, simulation, checks, bom, library

    var id: String { rawValue }

    /// Workspaces offered in the UI; the Super Intelligence workspace is hidden when AI assistance is off.
    static func visible(aiEnabled: Bool) -> [Workspace] {
        aiEnabled ? allCases : allCases.filter { $0 != .promptStudio }
    }

    var title: String {
        switch self {
        case .promptStudio: return "Super Intelligence"
        case .schematic: return "Schematic"
        case .library: return "Library"
        case .pcb: return "PCB Layout"
        case .threeD: return "3D Viewer"
        case .simulation: return "Simulation"
        case .checks: return "Design Checks"
        case .bom: return "BOM"
        }
    }

    /// Workspaces with a zoomable 2D canvas (View menu navigation applies to them).
    var hasCanvas: Bool { self == .schematic || self == .pcb }

    var systemImage: String {
        switch self {
        case .promptStudio: return "sparkles.rectangle.stack"
        case .schematic: return "point.3.connected.trianglepath.dotted"
        case .library: return "books.vertical"
        case .pcb: return "square.grid.3x3.square"
        case .threeD: return "cube.transparent"
        case .simulation: return "waveform.path.ecg"
        case .checks: return "checkmark.seal"
        case .bom: return "list.bullet.rectangle.portrait"
        }
    }
}

struct AlertItem: Identifiable {
    let id = UUID()
    var title: String
    var message: String
}

extension UTType {
    /// Declared in Info.plist (UTExportedTypeDeclarations) so Finder, the Dock and Open Recent route files to SiEDA.
    static var siedaProject: UTType { UTType("com.sieda.project") ?? UTType(filenameExtension: "siedaproj") ?? .json }
}

/// Single source of truth for the open design. Wraps the C++ engine and publishes snapshots to SwiftUI.
@MainActor
final class DesignStore: ObservableObject {
    let engine = EDAEngine()
    /// Real-time interactive simulation ("run the board").
    let live = LiveSimulation()

    @Published private(set) var snapshot: DesignSnapshot = .empty
    /// What the schematic canvas shows: the active sheet's components and wires (the whole design on one sheet).
    @Published private(set) var sheetSnapshot: DesignSnapshot = .empty
    @Published var workspace: Workspace = .promptStudio {
        didSet { if workspace != oldValue { CrashReporter.note("Workspace: \(workspace.title)") } }
    }
    @Published var selection: Set<Int> = [] {
        didSet { if selection != oldValue { followSelectionToSheet() } }
    }
    @Published var selectedWire: Int?
    /// Wires taken by a marquee (both ends inside the box); deleted with the selected parts.
    @Published var boxedWires: Set<Int> = []
    /// Pours refilled on demand (B / Refill Pours) instead of after every board edit (large boards edit faster).
    @Published var holdPours = UserDefaults.standard.bool(forKey: "pcb.holdPours") {
        didSet {
            UserDefaults.standard.set(holdPours, forKey: "pcb.holdPours")
            engine.holdPours(holdPours, refill: !holdPours)
            refresh()
        }
    }
    /// The part whose designator and value are being edited in place (double-click on the schematic).
    @Published var quickEdit: Int?
    /// The note / line / arrow / rectangle selected on the schematic.
    @Published var selectedGraphic: Int?
    /// The graphical bus selected on the schematic (its inspector rips entries and connects parts).
    @Published var selectedBus: Int?
    /// The schematic's Find & Replace panel.
    @Published var showFind = false
    /// ⌘K command palette (`CommandPaletteView`).
    @Published var showCommandPalette = false
    /// Help → Welcome Tour (shown once on first launch) and Help → Keyboard Shortcuts (Onboarding.swift).
    @Published var showWelcomeTour = false
    @Published var showShortcuts = false
    @Published var showPerformance = false
    /// The last edits' timings (Help → Performance…); not published, so recording one redraws nothing.
    private(set) var editTimings: [EditTiming] = []
    /// The custom schematic colour theme editor (opened from the colour scheme menus).
    @Published var showSchematicColours = false
    @Published var ercResults: [RuleViolation] = []
    /// The check finding opened from the messages / rule check list: its spot is ringed on the canvas.
    @Published var focusedViolation: String?
    @Published var drcResults: [RuleViolation] = []
    @Published var validationResults: [RuleViolation] = []
    @Published private(set) var verificationReport: VerificationReport?
    /// `revision` the verification report was computed at; any later edit makes it stale.
    @Published private(set) var verifiedRevision = -1
    @Published var dcResult: DCResult?
    @Published var transientResult: TransientResult?
    @Published var acResult: ACResult?
    @Published var dcSweepResult: DCSweepResult?
    @Published var monteCarloResult: MonteCarloResult?
    @Published var noiseResult: NoiseAnalysisResult?
    @Published var paramSweepResult: ParamSweepResult?
    @Published var fftResult: FFTResult?
    @Published var routeStats: RouteStats?
    /// Design revision the DRC results and route statistics describe; after any edit they are stale.
    @Published private(set) var drcRevision = -1
    @Published private(set) var routeRevision = -1
    var drcIsCurrent: Bool { drcRevision == revision }
    var routeStatsAreCurrent: Bool { routeStats != nil && routeRevision == revision }
    @Published var showDCOverlay = true
    /// Tab of the Design Checks workspace (set by the actions that open it, so they land on their results).
    @Published var checksMode: ChecksMode = .verification
    /// Mirrors `AISettings.aiEnabled` so document actions pick the right start workspace.
    @Published var aiEnabled = true {
        didSet { if !aiEnabled && workspace == .promptStudio { workspace = .schematic } }
    }
    var startWorkspace: Workspace { aiEnabled ? .promptStudio : .schematic }
    /// Custom part the Component Library should open (set when jumping from the inspector).
    @Published var libraryFocusPartId: String?
    @Published private(set) var isBusy = false
    /// Progress of the running autoroute (nil when none runs); `cancelAutoRoute()` stops it.
    @Published private(set) var routeProgress: RouteProgressReport?
    private var routeChannel: RouteProgressChannel?
    /// "Place new parts" after Update PCB (DesignStore+Placement.swift): the footprints still to place by hand.
    @Published var placementSession: PlacementSession?
    @Published private(set) var busyMessage = ""
    /// The snapshot as the core last sent it (`refresh` merges the next delta into it).
    private var coreSnapshot: DesignSnapshot?
    /// When the running busy task started (the status bar shows its elapsed time) and whether Stop can end it.
    @Published private(set) var busySince: Date?
    @Published private(set) var busyStoppable = false
    @Published var statusMessage = "Ready"
    @Published private(set) var documentURL: URL? {
        didSet { if documentURL == nil { collaboration.documentChanged(url: nil, json: "") } }
    }
    /// Live co-editing through the shared project file (File → Live Collaboration).
    lazy var collaboration = LiveCollaboration(store: self)
    @Published var liveCollaboration = false {
        didSet {
            collaboration.setEnabled(liveCollaboration)
            if liveCollaboration { ProjectLock.release(documentURL) } else if let url = documentURL { ProjectLock.take(url) }
        }
    }
    @Published private(set) var isDirty = false {
        didSet {
            // Saved, reverted or discarded: nothing left to recover.
            if oldValue, !isDirty { recoveryWork?.cancel(); recovery?.clear() }
        }
    }
    @Published var alert: AlertItem?
    /// Sheets of DesignStore+Team: what changed since another version, the variants side by side.
    @Published var designDiff: DesignDiff?
    @Published var variantMatrix: VariantMatrix?
    @Published var showReview = false
    @Published var showGerberViewer = false
    /// Reference layers' shapes, read once per layer ("index:name:bytes").
    var referenceArtworkCache: [String: GerberArtwork] = [:]
    /// Incremented whenever geometry changes so the 3D view knows to rebuild its mesh.
    @Published private(set) var revision = 0
    /// Incremented when a whole new design arrives (AI plan, open, example, re-placement) so editors re-fit.
    @Published private(set) var fitToken = 0
    /// Bumped to ask the window to show the inspector (a click on a part in a results list).
    @Published private(set) var inspectorRevealToken = 0
    /// Focus mode: full screen with only the editor (FocusMode.swift); set by the window's full-screen changes.
    @Published var focusMode = false
    /// Latest navigation command for the visible schematic/PCB canvas (View menu, zoom controls).
    @Published private(set) var viewRequest: ViewRequest?
    /// Navigator overview on the 2D canvases (persisted).
    @Published var showNavigator = UserDefaults.standard.object(forKey: "canvas.showNavigator") as? Bool ?? true {
        didSet { UserDefaults.standard.set(showNavigator, forKey: "canvas.showNavigator") }
    }

    func requestView(_ command: ViewCommand) { viewRequest = ViewRequest(command: command) }

    private var undoStack: [String] = []
    private var redoStack: [String] = []
    private let undoLimit = 100
    /// Memory budget for the undo + redo history (each step is a full design snapshot): big boards keep fewer
    /// steps instead of growing without bound.
    var historyByteLimit = 96 * 1024 * 1024

    /// Bytes held by the undo + redo history. O(steps): a native string's UTF-8 count is stored, not recounted.
    var historyBytes: Int {
        undoStack.reduce(0) { $0 + $1.utf8.count } + redoStack.reduce(0) { $0 + $1.utf8.count }
    }
    var undoDepth: Int { undoStack.count }

    /// Records an undo step and clears redo, keeping the history within `undoLimit` steps and `historyByteLimit`.
    private func pushUndo(_ state: String) {
        undoStack.append(state)
        redoStack.removeAll()
        trimHistory()
    }

    private func trimHistory() {
        if undoStack.count > undoLimit { undoStack.removeFirst(undoStack.count - undoLimit) }
        var bytes = historyBytes
        // The oldest steps go first (furthest back in undo, then furthest ahead in redo); the latest step on each
        // side is always kept, so Undo and Redo keep working right at the limit.
        while bytes > historyByteLimit, undoStack.count > 1 { bytes -= undoStack.removeFirst().utf8.count }
        while bytes > historyByteLimit, redoStack.count > 1 { bytes -= redoStack.removeFirst().utf8.count }
    }

    /// Crash-recovery autosave of unsaved work (attached by the app; nil in tests unless they set one).
    var recovery: CrashRecovery? {
        didSet { scheduleRecoverySave() }
    }
    /// Seconds of inactivity before unsaved work is autosaved for recovery.
    var recoveryDelay: TimeInterval = 3
    private var recoveryWork: DispatchWorkItem?
    private var memoryObserver: NSObjectProtocol?

    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }
    var windowTitle: String {
        let base = documentURL?.deletingPathExtension().lastPathComponent ?? snapshot.name
        return isDirty ? base + " — Edited" : base
    }
    var selectedComponents: [SnapComponent] { snapshot.components.filter { selection.contains($0.id) } }
    var verificationIsStale: Bool { verificationReport != nil && verifiedRevision != revision }

    init() {
        refresh()
        memoryObserver = NotificationCenter.default.addObserver(forName: .siedaMemoryPressure, object: nil,
                                                                queue: .main) { [weak self] note in
            let critical = note.userInfo?["critical"] as? Bool ?? false
            MainActor.assumeIsolated { self?.relieveMemoryPressure(critical: critical) }
        }
    }

    deinit {
        if let memoryObserver { NotificationCenter.default.removeObserver(memoryObserver) }
    }

    /// Under memory pressure: drop redo, and on critical pressure the older half of the undo history.
    func relieveMemoryPressure(critical: Bool) {
        redoStack.removeAll()
        if critical, undoStack.count > 1 { undoStack.removeFirst(undoStack.count / 2) }
        referenceArtworkCache.removeAll()  // read again when drawn
        if critical { LocalLLM.unload(); statusMessage = "Low memory — older undo steps released" }
    }

    // MARK: - Crash recovery

    /// Autosaves unsaved work after `recoveryDelay` seconds without further changes (debounced: one write per pause).
    func scheduleRecoverySave() {
        recoveryWork?.cancel()
        guard recovery != nil, isDirty else { return }
        let work = DispatchWorkItem { [weak self] in
            MainActor.assumeIsolated { self?.saveRecoveryNow() }
        }
        recoveryWork = work
        DispatchQueue.main.asyncAfter(deadline: .now() + recoveryDelay, execute: work)
    }

    /// Writes the recovery autosave now (the file write itself happens off the main thread).
    func saveRecoveryNow() {
        recoveryWork?.cancel()
        recoveryWork = nil
        guard let recovery, isDirty else { return }
        let json = engine.saveJSON()  // built once: for the project's own file or the recovery copy
        // Settings ▸ "Save projects automatically": a saved project is written to its own file after each pause instead.
        if UserDefaults.standard.bool(forKey: Self.autosaveKey), let url = documentURL, write(to: url, json: json) { return }
        recovery.save(json: json, documentURL: documentURL)
    }

    static let autosaveKey = "autosaveProject"

    /// Reopens work recovered after a crash: the design comes back unsaved, pointing at its original file if any.
    func restoreRecovered(_ pending: CrashRecovery.Pending) {
        do {
            try engine.load(json: pending.json)
            documentURL = pending.documentURL
            undoStack.removeAll()
            redoStack.removeAll()
            selection = []
            selectedWire = nil
            dcResult = nil
            transientResult = nil
            acResult = nil
            dcSweepResult = nil
            monteCarloResult = nil
            noiseResult = nil
            paramSweepResult = nil
            fftResult = nil
            resetChecks()
            refresh()
            isDirty = true
            fitToken &+= 1
            workspace = snapshot.components.isEmpty ? startWorkspace : .schematic
            statusMessage = "Recovered unsaved work"
            CrashReporter.note("Recovered unsaved work")
            scheduleRecoverySave()
        } catch {
            present(error, title: "Could not recover the unsaved work")
        }
    }

    // MARK: - Core plumbing

    /// One alert per run of failed snapshot reads.
    private var snapshotErrorShown = false

    /// Pours the copper zones again now (they also refill on every edit unless `holdPours`).
    func refillPours() {
        engine.holdPours(holdPours, refill: true)
        refresh()
        statusMessage = snapshot.zones.isEmpty ? "There are no pours to refill" : "Pours refilled"
    }

    func refresh() {
        if holdPours { engine.holdPours(true, refill: false) }  // a design just opened or undone starts unheld
        // Only the changed sections of the snapshot after an edit; `coreSnapshot` is the copy they apply to.
        switch engine.snapshotChecked(base: coreSnapshot, delta: true) {
        case .success(let snap):
            snapshot = snap
            coreSnapshot = snap
            snapshotErrorShown = false
        case .failure(let error):
            coreSnapshot = nil  // the next refresh asks for every section
            // Keep showing the last good state, but never silently: edits would otherwise look like they did nothing.
            NSLog("SiEDA: %@", error.localizedDescription)
            statusMessage = "Display not updated — \(error.localizedDescription)"
            if !snapshotErrorShown {
                snapshotErrorShown = true
                alert = AlertItem(title: "The design view could not be updated", message: error.localizedDescription)
            }
        }
        sheetSnapshot = snapshot.onSheet(snapshot.activeSheet)
        selection = selection.filter { snapshot.component($0) != nil }  // O(1) per part (large designs)
        if let wire = selectedWire, !snapshot.wires.contains(where: { $0.id == wire }) { selectedWire = nil }
        if let bus = selectedBus, sheetSnapshot.bus(bus) == nil { selectedBus = nil }
        if let g = selectedGraphic, !sheetSnapshot.sheetGraphics.contains(where: { $0.id == g }) { selectedGraphic = nil }
        // Undo, open or any edit that replaced the design ends a route in progress.
        if routePreview != nil, !engine.routerActive { routePreview = nil }
        if let tune = tuneSession, !snapshot.tracks.contains(where: { $0.id == tune.track }) { tuneSession = nil }
        if !selectedTracks.isEmpty { selectedTracks.formIntersection(snapshot.tracks.map(\.id)) }
        // A new design (open, example) or an undone Update PCB took the part being placed away.
        if let part = placementSession?.current, snapshot.component(part.id) == nil { placementSession = nil }
        revision &+= 1
    }

    /// Runs a mutating engine operation with undo support.
    func perform(_ actionName: String, recordUndo: Bool = true, invalidatesAnalysis: Bool = true,
                 _ body: (EDAEngine) -> Void) {
        performChecked(actionName, recordUndo: recordUndo, invalidatesAnalysis: invalidatesAnalysis) {
            body($0)
            return true
        }
    }

    /// Like `perform`, but the body reports whether the engine changed anything. A refused operation leaves no
    /// undo step, keeps the document clean and the analysis results, and shows `failureMessage` instead.
    @discardableResult
    func performChecked(_ actionName: String, recordUndo: Bool = true, invalidatesAnalysis: Bool = true,
                        failureMessage: String? = nil, _ body: (EDAEngine) -> Bool) -> Bool {
        // Long work (autorouting, simulation) holds the engine; an edit now would freeze the UI until it finishes.
        guard !isBusy else {
            statusMessage = "\(busyMessage.isEmpty ? "Busy" : busyMessage) — try again when it finishes"
            return false
        }
        let signpost = PerformanceLog.signposter.beginInterval("edit", "\(actionName)")
        defer { PerformanceLog.signposter.endInterval("edit", signpost) }
        let t0 = DispatchTime.now()
        let before = recordUndo ? engine.stateJSON() : nil
        let t1 = DispatchTime.now()
        guard body(engine) else {
            statusMessage = failureMessage ?? "\(actionName): not possible"
            return false
        }
        let t2 = DispatchTime.now()
        finishEdit(actionName, before: before, invalidatesAnalysis: invalidatesAnalysis,
                   stateMs: PerformanceLog.milliseconds(t0, t1), engineMs: PerformanceLog.milliseconds(t1, t2))
        return true
    }

    /// What follows a successful edit (`performChecked` and the background variants): the undo step, the document's
    /// dirty state, the refresh, the timing record and the status line. Main actor only.
    private func finishEdit(_ actionName: String, before: String?, invalidatesAnalysis: Bool,
                            stateMs: Double, engineMs: Double) {
        let t2 = DispatchTime.now()
        if let before { pushUndo(before) }
        isDirty = true
        collaboration.noteEdit()
        if invalidatesAnalysis {
            dcResult = nil
            transientResult = nil
            acResult = nil
            dcSweepResult = nil
            monteCarloResult = nil
            noiseResult = nil
            paramSweepResult = nil
            fftResult = nil
        }
        refresh()
        let t3 = DispatchTime.now()
        editTimings.append(EditTiming(action: actionName, stateMs: stateMs, engineMs: engineMs,
                                      refreshMs: PerformanceLog.milliseconds(t2, t3)))
        if editTimings.count > PerformanceLog.capacity { editTimings.removeFirst(editTimings.count - PerformanceLog.capacity) }
        statusMessage = actionName
        CrashReporter.note(actionName)
        scheduleRecoverySave()
    }

    /// Like `performChecked`, for long board commands (Auto Place, Gloss, Minimise Vias): the undo state and the engine
    /// work run off the main thread under `runBusy` (the window stays responsive; other edits are refused meanwhile),
    /// then the undo step, refresh and status follow here as for any edit. `changed` tells from `body`'s result whether
    /// the design changed: when it did not, no undo step is recorded and `failureMessage` is shown. Returns `body`'s
    /// result, or nil when the store was busy and nothing ran.
    @discardableResult
    func performInBackground<T: Sendable>(_ actionName: String, busy message: String, invalidatesAnalysis: Bool = true,
                                          failureMessage: String? = nil, changed: (T) -> Bool = { _ in true },
                                          _ body: @escaping @Sendable (EDAEngine) -> T) async -> T? {
        guard !isBusy else {
            statusMessage = "\(busyMessage.isEmpty ? "Busy" : busyMessage) — try again when it finishes"
            return nil
        }
        let signpost = PerformanceLog.signposter.beginInterval("edit", "\(actionName)")
        defer { PerformanceLog.signposter.endInterval("edit", signpost) }
        let engine = self.engine
        let run = await runBusy(message) { () -> BackgroundEdit<T> in
            let t0 = DispatchTime.now()
            let before = engine.stateJSON()
            let t1 = DispatchTime.now()
            let value = body(engine)
            let t2 = DispatchTime.now()
            return BackgroundEdit(before: before, value: value, stateMs: PerformanceLog.milliseconds(t0, t1),
                                  engineMs: PerformanceLog.milliseconds(t1, t2))
        }
        guard changed(run.value) else {
            statusMessage = failureMessage ?? "\(actionName): not possible"
            return run.value
        }
        finishEdit(actionName, before: run.before, invalidatesAnalysis: invalidatesAnalysis,
                   stateMs: run.stateMs, engineMs: run.engineMs)
        return run.value
    }

    /// An edit whose effect is known only afterwards (an AI client's tool call through the live MCP endpoint):
    /// one undo step labelled `actionName` when `body` reports a change and the design really differs, none otherwise
    /// (a refused tool leaves the history and the document's saved state alone).
    @discardableResult
    func performExternalEdit(_ actionName: String, _ body: (EDAEngine) -> Bool) -> Bool {
        guard !isBusy else { return performChecked(actionName) { _ in false } }
        let before = engine.stateJSON()
        let changed = performChecked(actionName, recordUndo: false, failureMessage: "\(actionName) — no change") {
            body($0) && $0.stateJSON() != before
        }
        if changed { pushUndo(before) }
        return changed
    }

    /// `performExternalEdit` with the engine work off the main thread (the live MCP endpoint): the state before,
    /// `body` and the state after run in one background task while the store is busy (no other edit can slip in
    /// between), then the one undo step, refresh and status follow here exactly as in `performExternalEdit`.
    func performExternalEditInBackground(_ actionName: String,
                                         _ body: @escaping @Sendable (EDAEngine) -> Bool) async -> Bool {
        guard !isBusy else { return performChecked(actionName) { _ in false } }
        let signpost = PerformanceLog.signposter.beginInterval("edit", "\(actionName)")
        defer { PerformanceLog.signposter.endInterval("edit", signpost) }
        let engine = self.engine
        let run = await runBusy(actionName) { () -> BackgroundEdit<Bool> in
            let t0 = DispatchTime.now()
            let before = engine.stateJSON()
            let t1 = DispatchTime.now()
            let changed = body(engine) && engine.stateJSON() != before
            let t2 = DispatchTime.now()
            return BackgroundEdit(before: before, value: changed, stateMs: PerformanceLog.milliseconds(t0, t1),
                                  engineMs: PerformanceLog.milliseconds(t1, t2))
        }
        guard run.value else {
            statusMessage = "\(actionName) — no change"
            return false
        }
        finishEdit(actionName, before: run.before, invalidatesAnalysis: true, stateMs: run.stateMs, engineMs: run.engineMs)
        return true
    }

    func undo() {
        guard let state = undoStack.popLast() else { return }
        redoStack.append(engine.stateJSON())
        trimHistory()
        restore(state, message: "Undo")
    }

    func redo() {
        guard let state = redoStack.popLast() else { return }
        undoStack.append(engine.stateJSON())
        trimHistory()
        restore(state, message: "Redo")
    }

    private func restore(_ json: String, message: String) {
        do {
            try engine.load(json: json)
            isDirty = true
            dcResult = nil
            transientResult = nil
            acResult = nil
            dcSweepResult = nil
            monteCarloResult = nil
            noiseResult = nil
            paramSweepResult = nil
            fftResult = nil
            refresh()
            statusMessage = message
            CrashReporter.note(message)
            scheduleRecoverySave()
        } catch {
            present(error, title: "Undo failed")
        }
    }

    /// Runs `work` off the main thread with the status bar's busy message and elapsed time. `stoppable`: a simulation,
    /// which the status bar's Stop button ends early (`sieda_simulation_stop`).
    func runBusy<T: Sendable>(_ message: String, stoppable: Bool = false, _ work: @escaping @Sendable () -> T) async -> T {
        isBusy = true
        busyMessage = message
        busySince = Date()
        busyStoppable = stoppable
        EDAEngine.stopSimulation(false)
        defer {
            isBusy = false
            busyMessage = ""
            busySince = nil
            busyStoppable = false
            EDAEngine.stopSimulation(false)
        }
        return await Task.detached(priority: .userInitiated) { work() }.value
    }

    /// Ends the running simulation early; its result reports "Simulation stopped.".
    func stopBusyTask() {
        guard isBusy, busyStoppable else { return }
        EDAEngine.stopSimulation(true)
        busyMessage = "Stopping…"
    }

    func present(_ error: Error, title: String) {
        alert = AlertItem(title: title, message: error.localizedDescription)
    }

    // MARK: - Schematic editing

    @discardableResult
    func addComponent(_ kind: ComponentKind, at point: CGPoint, rotation: Int = 0) -> Int {
        var id = -1
        let snapped = SchematicGrid.snap(point)
        perform("Placed \(kind.displayName)") {
            id = $0.addComponent(kind, at: snapped, rotation: rotation)
            if id >= 0 { _ = $0.connectTouching(id, wires: false) }  // a pin placed on another pin is wired to it
        }
        if id >= 0 { selection = [id] }
        return id
    }

    /// Moves parts with their wires following (drag); `detach` moves them alone, leaving the wires to other parts
    /// behind (⌥-drag). Pins that land on a wire or another pin are wired to it.
    func moveComponents(_ ids: Set<Int>, by delta: CGSize, detach: Bool = false) {
        guard !ids.isEmpty, delta != .zero else { return }
        if ids.contains(where: { lockedParts.contains($0) }) {
            statusMessage = "Locked parts stay where they are (unlock them first)"
            return
        }
        let moves = snapshot.components.filter { ids.contains($0.id) }
        let cut = detach ? snapshot.wires.filter { ids.contains($0.a.component) != ids.contains($0.b.component) }.map(\.id) : []
        perform(detach ? "Moved \(moves.count) part(s) alone" : "Moved \(moves.count) part(s)", invalidatesAnalysis: detach) { engine in
            cut.forEach { engine.removeWire($0) }
            for c in moves {
                engine.moveComponent(c.id, to: SchematicGrid.snap(CGPoint(x: c.x + delta.width, y: c.y + delta.height)))
            }
            for c in moves { engine.connectTouching(c.id, wires: false) }  // pin on pin; a wire it crosses stays apart
        }
    }

    func rotateSelection() {
        let ids = selection
        guard !ids.isEmpty else { return }
        perform("Rotated", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateComponent($0) } }
    }

    func deleteSelection() {
        if let graphic = selectedGraphic {
            perform("Deleted drawing", invalidatesAnalysis: false) { $0.removeSheetGraphic(graphic) }
            selectedGraphic = nil
            return
        }
        if let bus = selectedBus {
            perform("Deleted bus") { $0.removeBus(bus) }
            selectedBus = nil
            return
        }
        if let wire = selectedWire {
            perform("Deleted wire") { $0.removeWire(wire) }
            selectedWire = nil
            return
        }
        let ids = selection, wires = boxedWires.intersection(snapshot.wires.map(\.id))
        boxedWires = []
        guard !ids.isEmpty || !wires.isEmpty else { return }
        perform("Deleted \(ids.count) part(s), \(wires.count) wire(s)") { engine in
            wires.forEach { engine.removeWire($0) }
            ids.forEach { engine.removeComponent($0) }
        }
        selection = []
    }

    /// Joins the parts' pins to the pins and wires they touch (a pin dropped on a wire becomes a connection).
    func connectTouching(_ ids: Set<Int>) {
        var joined = 0
        perform("Connected touching pins") { engine in ids.forEach { joined += engine.connectTouching($0, wires: true) } }
        statusMessage = joined > 0 ? "Joined \(joined) touching pin(s)" : "No pin touches another pin or a wire"
    }

    // MARK: - Microcontroller firmware

    /// Attaches firmware (Intel HEX) to a microcontroller; an empty `hex` removes it. Undoable.
    @discardableResult
    func setFirmware(_ id: Int, hex: String, name: String, clockHz: Double? = nil) -> Bool {
        guard let component = snapshot.component(id) else { return false }
        let clock = clockHz ?? component.mcu?.clockHz ?? 0
        var failure: Error?
        let ok = performChecked(hex.isEmpty ? "Removed firmware from \(component.ref)" : "Loaded \(name) into \(component.ref)",
                                failureMessage: "\(component.ref): firmware not loaded") { engine in
            do {
                try engine.setFirmware(id, hex: hex, name: name, clockHz: clock)
                return true
            } catch {
                failure = error
                return false
            }
        }
        if let failure { present(failure, title: "Could not load the firmware") }
        return ok
    }

    /// Asks for a .hex file (Arduino IDE ▸ Sketch ▸ Export Compiled Binary, or avr-objcopy -O ihex) and loads it.
    func uploadFirmware(to id: Int) {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "hex") ?? .data, UTType(filenameExtension: "ihex") ?? .data, .plainText]
        panel.allowsMultipleSelection = false
        panel.message = "Choose an Intel HEX firmware file (Arduino IDE: Sketch ▸ Export Compiled Binary)."
        panel.prompt = "Upload"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        loadFirmwareFile(url, into: id)
    }

    func loadFirmwareFile(_ url: URL, into id: Int) {
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let hex = try String(contentsOf: url, encoding: .utf8)
            setFirmware(id, hex: hex, name: url.lastPathComponent)
        } catch {
            present(error, title: "Could not read \(url.lastPathComponent)")
        }
    }

    func loadFirmwareExample(_ example: FirmwareExample, into id: Int) {
        guard let hex = EDAEngine.firmwareExampleHex(example.id) else { return }
        setFirmware(id, hex: hex, name: example.name)
    }

    /// CPU clock of a microcontroller (keeps its firmware).
    func setMcuClock(_ id: Int, clockHz: Double) {
        guard let mcu = snapshot.component(id)?.mcu, mcu.clockHz != clockHz else { return }
        setFirmware(id, hex: engine.firmware(of: id), name: mcu.firmwareName, clockHz: clockHz)
    }

    /// Flips a toggle switch ("on" ⇄ "off"). Undoable; a DC result on screen is re-solved so the probes and LEDs
    /// follow at once. Push-buttons ("push", "button"…) are only pressed while the live simulation runs.
    func toggleSwitch(_ id: Int) {
        guard let c = snapshot.component(id), c.componentKind == .switchSPST else { return }
        let value = c.value.lowercased()
        if ["push", "button", "momentary", "tact"].contains(where: { value.contains($0) }) {
            statusMessage = "\(c.ref) is a push-button — press it while the live simulation runs"
            return
        }
        let closed = ["on", "closed", "1", "true"].contains(value)
        let resolve = showDCOverlay && dcResult != nil && !live.isRunning  // the edit clears the result
        perform(closed ? "Opened \(c.ref)" : "Closed \(c.ref)") { $0.setValue(id, closed ? "off" : "on") }
        statusMessage = "\(c.ref) \(closed ? "off" : "on")"
        if resolve { Task { await simulateDC() } }
    }

    func setValue(_ id: Int, _ value: String) {
        guard let current = snapshot.component(id), current.value != value else { return }
        perform("Changed value") { $0.setValue(id, value) }
    }

    /// Fits a passive / diode in another package (0603, through-hole, tantalum B…): an undoable edit that moves its
    /// pads, so the PCB and 3D views follow.
    func setPackage(_ id: Int, _ package: String) {
        guard let current = snapshot.component(id), current.footprint != package else { return }
        let label = current.packageOptions?.first { $0.id == package }?.label ?? package
        performChecked("\(current.ref) package \(label)", failureMessage: "\(current.ref) cannot use \(label)") {
            $0.setPackage(id, package)
        }
    }

    func setRef(_ id: Int, _ ref: String) {
        let trimmed = ref.trimmingCharacters(in: .whitespaces)
        if let current = snapshot.component(id), let logical = current.logicalRef {
            // A part of a repeated sheet: the designator inside the block; each channel's follows from it.
            guard !trimmed.isEmpty, logical != trimmed else { return }
            performChecked("Renamed to \(trimmed)", invalidatesAnalysis: false,
                           failureMessage: "\(trimmed) is already used in this block") { $0.setRef(id, trimmed) }
            return
        }
        guard !trimmed.isEmpty, let current = snapshot.component(id), current.ref != trimmed else { return }
        if snapshot.component(ref: trimmed) != nil {
            alert = AlertItem(title: "Duplicate designator", message: "\(trimmed) is already used in this design.")
            return
        }
        perform("Renamed to \(trimmed)", invalidatesAnalysis: false) { $0.setRef(id, trimmed) }
    }

    @discardableResult
    func connect(_ a: PinAddress, _ b: PinAddress) -> Bool {
        guard a != b else { return false }
        return performChecked("Connected wire", failureMessage: "Those pins are already connected") {
            $0.connect(a, b) != nil
        }
    }

    /// Draws one wire between two ends, as a single undo step: an end on a wire splits it with a T-junction (or uses
    /// the pin at that wire's end), a free end makes a bend point. Returns the pin the wire ended on (a new bend's
    /// junction, to continue drawing from it), or nil when nothing could be drawn.
    @discardableResult
    func drawWire(from start: WireEnd, to end: WireEnd) -> PinAddress? {
        if case .wire(let a, _) = start, case .wire(let b, _) = end, a == b { return nil }  // a wire onto itself
        let wires = snapshot.wires
        // Resolve on the current geometry first; only the engine calls below change the design.
        enum Resolved { case pin(PinAddress), split(Int, CGPoint), junction(CGPoint) }
        func resolve(_ end: WireEnd) -> Resolved? {
            switch end {
            case .pin(let address):
                return .pin(address)
            case .wire(let id, let near):
                guard let w = wires.first(where: { $0.id == id }) else { return nil }
                let p = WireGeometry.nearestPoint(on: w, to: near).point
                if p == w.start { return .pin(w.a) }
                if p == w.end { return .pin(w.b) }
                return .split(id, p)
            case .point(let p):
                return .junction(SchematicGrid.snap(p))
            }
        }
        guard let from = resolve(start), let to = resolve(end) else { return nil }
        if case .pin(let a) = from, case .pin(let b) = to, a == b { return nil }
        if case .pin(let a) = from, case .junction(let p) = to, pinPoint(a) == p { return a }  // no zero-length wire
        var result: PinAddress?
        let creates: Bool = {
            if case .pin = from, case .pin = to { return false }
            return true
        }()
        performChecked(creates ? "Drew wire" : "Connected wire", failureMessage: "Those pins are already connected") { engine in
            func make(_ r: Resolved) -> PinAddress? {
                switch r {
                case .pin(let address): return address
                case .split(let id, let p): return engine.splitWire(id, at: p).map { PinAddress(component: $0, pin: 0) }
                case .junction(let p):
                    let j = engine.addComponent(.junction, at: p)
                    return j >= 0 ? PinAddress(component: j, pin: 0) : nil
                }
            }
            guard let a = make(from), let b = make(to), a != b else { return creates }
            result = b
            return engine.connect(a, b) != nil || creates
        }
        return result
    }

    /// Bends a wire through `point`: a junction there splits it, and can be dragged further later.
    func bendWire(_ id: Int, at point: CGPoint) {
        let p = SchematicGrid.snap(point)
        var junction: Int?
        performChecked("Bent wire", invalidatesAnalysis: false) { engine in
            junction = engine.splitWire(id, at: p)
            return junction != nil
        }
        if let junction { selection = [junction]; selectedWire = nil }
    }

    /// Drags a straight wire sideways by `offset` (its perpendicular part); it stays straight. Undoable.
    func dragWireSegment(_ id: Int, by offset: CGSize) {
        guard offset != .zero else { return }
        performChecked("Moved wire", invalidatesAnalysis: false) { $0.dragWireSegment(id, dx: offset.width, dy: offset.height) }
    }

    /// Moves end b (else a) of a wire off its pin: onto `pin`, or a loose junction at `point`. Undoable.
    func moveWireEnd(_ id: Int, b: Bool, to pin: PinAddress?, at point: CGPoint) {
        performChecked("Moved wire end", failureMessage: "The wire end cannot go there") {
            $0.moveWireEnd(id, b: b, x: point.x, y: point.y, component: pin?.component ?? -1, pin: pin?.pin ?? 0)
        }
    }

    /// Abandons a wire being drawn: the corners already placed for it (dangling junctions) are removed.
    func cancelWire(at end: PinAddress) {
        guard snapshot.component(end.component)?.componentKind == .junction,
              snapshot.wires.filter({ $0.a.component == end.component || $0.b.component == end.component }).count <= 1
        else { return }
        perform("Cancelled wire") { $0.removeDanglingJunctions(from: end.component) }
    }

    private func pinPoint(_ address: PinAddress) -> CGPoint? {
        guard let c = snapshot.component(address.component), address.pin < c.pins.count else { return nil }
        return c.pins[address.pin].point
    }

    // MARK: - Bill of materials

    /// The current BOM (computed by the core from the schematic and each part's sourcing).
    var bomReport: BomReport { engine.bom() }

    /// Sets manufacturer, part numbers, price or DNP on every part of a BOM line, as one undo step.
    func updateBomLine(_ line: BomLineInfo, _ update: SourcingUpdate) {
        guard update != SourcingUpdate() else { return }
        performChecked("BOM: \(line.refs.first ?? "")\(line.refs.count > 1 ? "…" : "") updated", invalidatesAnalysis: false) { engine in
            line.componentIds.reduce(false) { changed, id in engine.setSourcing(component: id, update) || changed }
        }
    }

    /// Fills the suggested standard part numbers (e.g. Yageo RC0805 resistors, semiconductor part numbers) into every
    /// line that has none yet. One undo step. Returns how many lines were filled.
    @discardableResult
    func applySuggestedPartNumbers() -> Int {
        let lines = bomReport.lines.filter { $0.mpn.isEmpty && !$0.suggestedMpn.isEmpty }
        guard !lines.isEmpty else {
            statusMessage = "No suggested part numbers to apply"
            return 0
        }
        perform("Applied \(lines.count) suggested part number\(lines.count == 1 ? "" : "s")", invalidatesAnalysis: false) { engine in
            for line in lines {
                let update = SourcingUpdate(manufacturer: line.manufacturer.isEmpty ? line.suggestedManufacturer : nil,
                                            mpn: line.suggestedMpn)
                for id in line.componentIds { engine.setSourcing(component: id, update) }
            }
        }
        return lines.count
    }

    func setBuildQuantity(_ quantity: Int) {
        let q = max(1, quantity)
        guard q != bomReport.buildQuantity else { return }
        perform("Build quantity \(q)", invalidatesAnalysis: false) { $0.setBuildQuantity(q) }
    }

    /// Toggles the "no connect" mark of a pin (an intentionally open pin; ERC stops reporting it).
    func toggleNoConnect(_ pin: PinAddress) {
        guard let component = snapshot.component(pin.component), pin.pin >= 0, pin.pin < component.pins.count else { return }
        let marked = component.pins[pin.pin].noConnect
        let label = "\(component.ref).\(component.pins[pin.pin].name)"
        performChecked(marked ? "Cleared no-connect on \(label)" : "Marked \(label) no-connect", invalidatesAnalysis: false) {
            $0.setPinNoConnect(pin, !marked)
        }
        runERC()
    }

    /// Selects a part from a list (simulation results, reports) and shows its properties in the inspector.
    func reveal(component id: Int) {
        select(component: id)
        inspectorRevealToken &+= 1
    }

    func select(component id: Int?, extend: Bool = false) {
        selectedWire = nil
        selectedBus = nil
        selectedGraphic = nil
        if !extend { boxedWires = [] }
        guard let id else {
            if !extend { selection = [] }
            return
        }
        if extend {
            if selection.contains(id) { selection.remove(id) } else { selection.insert(id) }
        } else {
            selection = [id]
        }
    }

    // MARK: - Custom components

    /// Registers (or updates, when `replacing` is given) a custom part and returns it.
    @discardableResult
    func saveCustomPart(_ spec: CustomPartSpec, replacing oldId: String? = nil) -> CustomPartInfo? {
        var result: CustomPartInfo?
        var failure: Error?
        performChecked(oldId == nil ? "Added \(spec.name) to the library" : "Updated \(spec.name)",
                       failureMessage: "\(spec.name) was not saved") { engine in
            do {
                let part = try engine.registerCustomPart(spec)
                if let oldId, oldId != part.id {
                    engine.replaceCustomPart(oldId, with: part.id)
                    engine.removeCustomPart(oldId)
                }
                result = part
                return true
            } catch {
                failure = error
                return false
            }
        }
        if let failure { present(failure, title: "Could not save the component") }
        return result
    }

    func deleteCustomPart(_ id: String) {
        let removed = performChecked("Removed part from library", invalidatesAnalysis: false,
                                     failureMessage: "The part is still used in the schematic") { $0.removeCustomPart(id) }
        if !removed && !isBusy {
            alert = AlertItem(title: "Part is in use",
                              message: "Delete the components that use this part from the schematic first.")
        }
    }

    @discardableResult
    func addCustomComponent(partId: String, at point: CGPoint, rotation: Int = 0) -> Int {
        var id = -1
        let snapped = SchematicGrid.snap(point)
        let part = snapshot.customPart(partId)
        let name = part?.name ?? "part"
        if part?.isMultiUnit == true {
            // A multi-unit part is placed gate by gate: unit A now, the others with Place Next Unit.
            perform("Placed \(name) unit A") { id = $0.addCustomUnits(partId: partId, at: snapped, rotation: rotation) }
        } else {
            perform("Placed \(name)") {
                id = $0.addCustomComponent(partId: partId, at: snapped, rotation: rotation)
                if id >= 0 { _ = $0.connectTouching(id, wires: false) }
            }
        }
        if id >= 0 { selection = [id] }
        return id
    }

    /// Places the next unit of a multi-unit part beside the last one placed and selects it.
    func placeNextUnit(of id: Int) {
        guard let c = snapshot.component(id), let package = c.unitOf ?? (c.isUnitPackage ? c.id : nil) else { return }
        let placed = snapshot.components.filter { $0.unitOf == package && $0.sheetId == snapshot.activeSheet }
        let anchor = placed.max { $0.x < $1.x }?.position ?? c.position
        var unit: Int?
        performChecked("Placed next unit of \(c.ref)", failureMessage: "Every unit of \(c.ref) is placed") {
            unit = $0.placeNextUnit(of: package, at: SchematicAutoLayout.snap(CGPoint(x: anchor.x + 140, y: anchor.y)))
            return unit != nil
        }
        if let unit { selection = [unit] }
    }

    /// Gate swap: two selected units of interchangeable gates exchange their gates (symbols and wires stay).
    func swapGates(_ a: Int, _ b: Int) {
        guard let ua = snapshot.component(a), let ub = snapshot.component(b) else { return }
        performChecked("Swapped gates \(ua.displayRef) ↔ \(ub.displayRef)",
                       failureMessage: "\(ua.displayRef) and \(ub.displayRef) are not interchangeable gates of the same part") {
            $0.swapUnits(a, b)
        }
    }

    /// Pin swap: two pins of a unit's pin-swap group exchange their wires.
    func swapPins(of id: Int, _ a: Int, _ b: Int) {
        guard let c = snapshot.component(id) else { return }
        performChecked("Swapped pins of \(c.displayRef)", failureMessage: "These pins of \(c.displayRef) are not swappable") {
            $0.swapPins(id, a, b)
        }
    }

    /// Places one particular unit (1-based) of a multi-unit part.
    func placeUnit(_ unit: Int, of id: Int) {
        guard let c = snapshot.component(id), let package = c.unitOf ?? (c.isUnitPackage ? c.id : nil) else { return }
        let anchor = c.position
        var placed: Int?
        performChecked("Placed unit of \(c.ref)", failureMessage: "That unit of \(c.ref) is placed already") {
            placed = $0.addPartUnit(of: package, unit: unit, at: SchematicAutoLayout.snap(CGPoint(x: anchor.x + 140, y: anchor.y + 80)))
            return placed != nil
        }
        if let placed { selection = [placed] }
    }

    /// Adds a built-in standard part to the project library (if needed) and returns its part id.
    @discardableResult
    func addStandardPartToLibrary(_ part: StandardPart) -> String? {
        if let existing = snapshot.customParts.first(where: { $0.name == part.spec.name }) { return existing.id }
        return saveCustomPart(part.spec)?.id
    }

    /// Adds parts read by the library importer (KiCad / Eagle) to the project library as one undo step. Returns the
    /// ids of the parts added; parts the core refuses are reported together.
    @discardableResult
    func importLibraryParts(_ specs: [CustomPartSpec]) -> [String] {
        var added: [String] = []
        var failures: [String] = []
        performChecked("Imported \(specs.count) library parts", failureMessage: "No library parts were imported") { engine in
            for spec in specs {
                do {
                    added.append(try engine.registerCustomPart(spec).id)
                } catch {
                    failures.append("\(spec.name): \(error.localizedDescription)")
                }
            }
            return !added.isEmpty
        }
        if !failures.isEmpty {
            present(EDAEngineError.operationFailed(failures.joined(separator: "\n")), title: "Some parts were not imported")
        }
        return added
    }

    /// Places a built-in standard part (registering it in the project library first).
    @discardableResult
    func placeStandardPart(_ part: StandardPart, at point: CGPoint) -> Int {
        guard let partId = addStandardPartToLibrary(part) else { return -1 }
        return addCustomComponent(partId: partId, at: point)
    }

    /// Replaces a resistor/capacitor/inductor value with the nearest member of `series`.
    func snapToStandardValue(_ id: Int, series: ESeries) {
        guard let c = snapshot.component(id), let value = EDAEngine.parseValue(c.value), value > 0 else { return }
        let nearest = series.nearest(value)
        let text = EngineeringFormat.string(nearest, unit: "", digits: 3).replacingOccurrences(of: " ", with: "")
        guard text != c.value else { return }
        perform("\(c.ref) → \(text) (\(series.title))") { $0.setValue(id, text) }
    }

    // MARK: - Boards (one schematic, several boards)

    /// Shows another board in the PCB views; its parts are placed and routed there, the others wait.
    func selectBoard(_ index: Int) {
        let boards = snapshot.boards
        guard index != boards.active, boards.names.indices.contains(index) else { return }
        guard performChecked("Switched to \(boards.names[index])", invalidatesAnalysis: false, { $0.setActiveBoard(index) }) else { return }
        selection = []
        fitToken &+= 1
    }

    /// Adds a board with this board's rules and stack-up, and shows it.
    func addBoard() {
        let name = "Board \(snapshot.boards.names.count + 1)"
        var index: Int?
        performChecked("Added \(name)", invalidatesAnalysis: false) { engine in
            index = engine.addBoard(name)
            if let index { engine.setActiveBoard(index) }
            return index != nil
        }
        if index != nil {
            selection = []
            fitToken &+= 1
        }
    }

    func renameBoard(_ index: Int, to name: String) {
        let title = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty, snapshot.boards.names.indices.contains(index), snapshot.boards.names[index] != title else { return }
        performChecked("Renamed board to \(title)", invalidatesAnalysis: false) { $0.renameBoard(index, to: title) }
    }

    func removeBoard(_ index: Int) {
        let boards = snapshot.boards
        guard boards.names.count > 1, boards.names.indices.contains(index) else { return }
        let alert = NSAlert()
        alert.alertStyle = .warning
        alert.messageText = "Delete board “\(boards.names[index])”?"
        alert.informativeText = "Its copper is deleted; its parts move to another board, unplaced. You can undo this."
        alert.addButton(withTitle: "Delete")
        alert.addButton(withTitle: "Cancel")
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        if performChecked("Deleted \(boards.names[index])", invalidatesAnalysis: false, { $0.removeBoard(index) }) {
            selection = []
            fitToken &+= 1
        }
    }

    /// Moves parts (the selection, or every part of a sheet) to another board, where they wait unplaced.
    func assignParts(_ ids: [Int], toBoard index: Int) {
        guard !ids.isEmpty, snapshot.boards.names.indices.contains(index) else { return }
        performChecked("Moved \(ids.count) part(s) to \(snapshot.boards.names[index])",
                       failureMessage: "Those parts are already on \(snapshot.boards.names[index])") {
            $0.assignToBoard(ids, board: index) > 0
        }
    }

    // MARK: - Sheets

    /// Shows another sheet in the schematic (and places new parts there). Not an edit: no undo step.
    func selectSheet(_ id: Int, fit: Bool = true) {
        // Long work (autorouting, simulation) holds the engine: switching now would block the UI until it finishes.
        guard !isBusy, id != snapshot.activeSheet, snapshot.sheet(id) != nil, engine.setActiveSheet(id) else { return }
        selectedWire = nil
        refresh()
        selection = selection.filter { snapshot.component($0)?.sheetId == id }
        if fit { fitToken &+= 1 }
    }

    /// Selecting parts on another sheet (from the checks, BOM or simulation lists) brings their sheet up.
    private func followSelectionToSheet() {
        guard snapshot.sheets.count > 1, !selection.isEmpty else { return }
        let sheets = Set(selection.compactMap { snapshot.component($0)?.sheetId })
        guard !sheets.isEmpty, !sheets.contains(snapshot.activeSheet), let target = sheets.min() else { return }
        selectSheet(target)
    }

    /// "Sheet 2", "Sheet 3", … — the first free default name.
    var nextSheetName: String {
        var n = snapshot.sheets.count + 1
        while snapshot.sheets.contains(where: { $0.name == "Sheet \(n)" }) { n += 1 }
        return "Sheet \(n)"
    }

    /// Adds a sheet (a child of `parent`, 0 = top level) and shows it.
    @discardableResult
    func addSheet(named name: String? = nil, parent: Int = 0) -> Int? {
        let title = (name ?? nextSheetName).trimmingCharacters(in: .whitespacesAndNewlines)
        var id: Int?
        performChecked("Added sheet \(title)", invalidatesAnalysis: false,
                       failureMessage: "A sheet named \(title) already exists") { engine in
            id = engine.addSheet(title, parent: parent)
            if let id { engine.setActiveSheet(id) }
            return id != nil
        }
        if id != nil {
            selection = []
            fitToken &+= 1
        }
        return id
    }

    func renameSheet(_ id: Int, to name: String) {
        let title = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty, let sheet = snapshot.sheet(id), sheet.name != title else { return }
        performChecked("Renamed sheet \(sheet.name) to \(title)", invalidatesAnalysis: false,
                       failureMessage: "A sheet named \(title) already exists") { $0.renameSheet(id, to: title) }
    }

    /// Deletes a sheet with everything on it (after confirmation when it holds parts). The last sheet stays.
    func removeSheet(_ id: Int) {
        guard snapshot.sheets.count > 1, let sheet = snapshot.sheet(id) else { return }
        let count = snapshot.components.filter { $0.sheetId == id }.count
        if count > 0 {
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "Delete sheet “\(sheet.name)”?"
            alert.informativeText = "Its \(count) component(s) and their wires are deleted with it, and the sheet entries "
                + "that lead into it. Child sheets move up a level. You can undo this."
            alert.addButton(withTitle: "Delete")
            alert.addButton(withTitle: "Cancel")
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }
        selection = []
        performChecked("Deleted sheet \(sheet.name)") { $0.removeSheet(id, deleteContents: true) }
        fitToken &+= 1
    }

    /// Moves the selected parts to another sheet (wires to parts left behind are removed) and shows that sheet.
    func moveSelection(toSheet id: Int) {
        let ids = Array(selection)
        guard !ids.isEmpty, let sheet = snapshot.sheet(id) else { return }
        let moved = performChecked("Moved \(ids.count) part(s) to \(sheet.name)",
                                   failureMessage: "The selection is already on \(sheet.name)") { $0.moveToSheet(ids, sheet: id) > 0 }
        if moved { selectSheet(id) }
    }

    /// Net label scope: "global", "local" or "port".
    func setLabelScope(_ id: Int, scope: String) {
        guard let c = snapshot.component(id), c.componentKind == .netLabel, c.labelScope != scope else { return }
        performChecked("\(c.value): \(scope) label") { $0.setLabelScope(id, scope: scope) }
    }

    /// Sheet symbol of a child sheet: adds an entry on its parent sheet for every port that has none, to the right of
    /// the parent's parts, and shows the parent sheet.
    func placeSheetSymbol(for child: Int) {
        guard let sheet = snapshot.sheet(child), sheet.parent != 0 else { return }
        let bounds = SchematicCanvas.componentBounds(snapshot.onSheet(sheet.parent)).reduce(CGRect.null) { $0.union($1.rect) }
        let origin = bounds.isNull ? .zero : SchematicAutoLayout.snap(CGPoint(x: bounds.maxX + 80, y: bounds.minY))
        let placed = performChecked("Placed sheet symbol of \(sheet.name)",
                                    failureMessage: "\(sheet.name) has no ports without an entry — make net labels on it Port labels first") {
            $0.placeSheetEntries(child: child, at: origin) > 0
        }
        if placed { selectSheet(sheet.parent) }
    }

    /// Uses a sheet `count` times (channels) and gives every channel without one its sheet symbol on the parent
    /// sheet, side by side to the right of the parent's parts. One undo step.
    func repeatSheet(_ id: Int, count: Int) {
        guard let sheet = snapshot.sheet(id), !sheet.isInstance, count >= 1, count <= 64 else { return }
        let done = performChecked("\(sheet.name): \(count) channel(s)",
                                  failureMessage: "Only a sheet whose child sheets are repeated blocks can be repeated") { engine in
            guard engine.repeatSheet(id, count: count) != nil else { return false }
            guard sheet.parent != 0, let snap = engine.snapshot() else { return true }
            let parentParts = snap.onSheet(sheet.parent)
            var x = SchematicCanvas.componentBounds(parentParts).reduce(CGRect.null) { $0.union($1.rect) }.maxX
            if x.isInfinite || x.isNaN { x = 0 }
            let top = SchematicCanvas.componentBounds(parentParts).map { $0.rect.minY }.min() ?? 0
            for (index, channel) in snap.sheets.filter({ $0.definitionId == id }).enumerated() {
                let hasEntries = snap.components.contains { $0.labelScope == "entry" && $0.targetSheet == channel.id }
                if hasEntries { continue }
                let origin = SchematicAutoLayout.snap(CGPoint(x: x + 80 + CGFloat(index) * 160, y: top))
                engine.placeSheetEntries(child: channel.id, at: origin)
            }
            return true
        }
        if done { fitToken &+= 1 }
    }

    /// Channel designators of a repeated sheet: "sheet" (R201, R301…) or "suffix" (R1_A, R1_B…).
    func setInstanceRefs(_ id: Int, scheme: String) {
        guard let sheet = snapshot.sheet(id), sheet.isRepeated, sheet.refs != scheme else { return }
        performChecked("Channel designators: \(scheme)", invalidatesAnalysis: false) { $0.setInstanceRefs(id, scheme: scheme) }
    }

    func setSheetChannel(_ id: Int, to channel: String) {
        let label = channel.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let sheet = snapshot.sheet(id), sheet.isRepeated, !label.isEmpty, sheet.channel != label else { return }
        performChecked("Channel \(label)", invalidatesAnalysis: false,
                       failureMessage: "Channel labels are letters, digits, _ or -, unique in the block") {
            $0.setSheetChannel(id, channel: label)
        }
    }

    /// Value of this channel only (a part of a repeated sheet); the block's value when `value` is empty.
    func setChannelValue(_ id: Int, _ value: String) {
        let text = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let c = snapshot.component(id), c.logicalRef != nil, text != c.value || c.channelOverride != nil else { return }
        if text.isEmpty {
            performChecked("\(c.displayRef) takes the block value") { $0.clearChannelOverrides(id) }
        } else {
            performChecked("\(c.displayRef) = \(text) in this channel") { $0.setChannelValue(id, text) }
        }
    }

    // MARK: - Schematic directives (the source of the board's net rules)

    @discardableResult
    func setNetClass(_ name: String, trackWidth: Double, clearance: Double) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        return performChecked("Net class \(trimmed)",
                              failureMessage: "A net class needs a name (letters, digits, _ or -) and sizes of 0.05–10 mm (0 = board default)") {
            $0.setNetClass(trimmed, trackWidth: trackWidth, clearance: clearance)
        }
    }

    func removeNetClass(_ name: String) {
        performChecked("Removed net class \(name)") { $0.removeNetClass(name) }
    }

    /// The directive anchored on a pin (nil when there is none).
    func directive(on anchor: PinAddress) -> DirectiveInfo? {
        snapshot.directives.first { $0.component == anchor.component && $0.pin == anchor.pin }
    }

    /// Sets the directive on a pin's net: adds, updates or (when it says nothing) removes it.
    func setDirective(on anchor: PinAddress, netClass: String, diffPair: Bool, trackWidth: Double, clearance: Double) {
        let empty = netClass.isEmpty && !diffPair && trackWidth <= 0 && clearance <= 0
        if let existing = directive(on: anchor) {
            if empty {
                performChecked("Removed directive") { $0.removeDirective(existing.id) }
            } else {
                performChecked("Directive", failureMessage: "Widths and clearances are 0.05–10 mm (0 = none)") {
                    $0.updateDirective(existing.id, component: anchor.component, pin: anchor.pin, netClass: netClass,
                                       diffPair: diffPair, trackWidth: trackWidth, clearance: clearance)
                }
            }
        } else if !empty {
            performChecked("Directive", failureMessage: "Widths and clearances are 0.05–10 mm (0 = none)") {
                $0.addDirective(component: anchor.component, pin: anchor.pin, netClass: netClass, diffPair: diffPair,
                                trackWidth: trackWidth, clearance: clearance) != nil
            }
        }
    }

    // MARK: - Signal harnesses

    /// Defines or replaces a harness type from a name and its members ("DP, DN, VBUS").
    @discardableResult
    func setHarnessType(_ name: String, entries text: String) -> Bool {
        let trimmed = name.trimmingCharacters(in: .whitespacesAndNewlines)
        let entries = text.split(whereSeparator: { $0 == "," || $0 == "\n" || $0 == ";" })
            .map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        guard !trimmed.isEmpty, !entries.isEmpty else { return false }
        return performChecked("Harness type \(trimmed)", invalidatesAnalysis: false,
                              failureMessage: "A harness type needs a name (letters, digits, _ or -) and unique member names without dots") {
            $0.setHarnessType(trimmed, entries: entries)
        }
    }

    func removeHarnessType(_ name: String) {
        performChecked("Removed harness type \(name)", invalidatesAnalysis: false) { $0.setHarnessType(name, entries: []) }
    }

    /// Makes the label a harness label of `type` ("" = an ordinary label).
    func setLabelHarness(_ id: Int, type: String) {
        guard let c = snapshot.component(id), (c.harnessType ?? "") != type else { return }
        performChecked(type.isEmpty ? "\(c.value) is a single signal" : "\(c.value) carries harness \(type)",
                       failureMessage: "A harness label needs a name of letters, digits, _ or -") {
            $0.setLabelHarness(id, type: type)
        }
    }

    /// Places a harness connector of `type` named `name` on the shown sheet, to the right of its parts.
    func placeHarnessConnector(type: String, name: String) {
        let parts = sheetSnapshot
        var x = SchematicCanvas.componentBounds(parts).reduce(CGRect.null) { $0.union($1.rect) }.maxX
        if x.isInfinite || x.isNaN { x = 0 }
        let origin = SchematicAutoLayout.snap(CGPoint(x: x + 120, y: 0))
        var placed: Int?
        let done = performChecked("Placed harness \(name)", failureMessage: "Harness names are letters, digits, _ or -") {
            placed = $0.addHarnessConnector(type: type, name: name.trimmingCharacters(in: .whitespaces), at: origin)
            return placed != nil
        }
        if done, let placed { selection = [placed] }
    }

    func placeHarnessEntries(_ id: Int) {
        performChecked("Added harness entries", failureMessage: "Every member has its entry") { ($0.placeHarnessEntries(id) ?? 0) > 0 }
    }

    // MARK: - Find / replace and cross-probing

    func find(_ text: String, matchCase: Bool, wholeWord: Bool, pins: Bool) -> [SchematicSearchHit] {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return [] }
        return engine.find(trimmed, matchCase: matchCase, wholeWord: wholeWord, pins: pins)
    }

    /// Replaces text in part values and net label names on every sheet, as one undo step. Returns the count.
    @discardableResult
    func replaceAll(_ text: String, with replacement: String, matchCase: Bool, wholeWord: Bool) -> Int {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return 0 }
        var count = 0
        let done = performChecked("Replaced \(trimmed) with \(replacement)", failureMessage: "Nothing to replace") {
            count = $0.replace(trimmed, with: replacement, matchCase: matchCase, wholeWord: wholeWord)
            return count > 0
        }
        if done { statusMessage = "Replaced \(count) value(s), label(s) and designator(s)" }
        return count
    }

    /// Shows a place on its sheet and selects it (find results, the net navigator, sheet entries and ports).
    func crossProbe(component id: Int, sheet: Int) {
        if sheet != snapshot.activeSheet, snapshot.sheet(sheet) != nil { selectSheet(sheet, fit: false) }
        select(component: id)
        requestView(.fitSelection)
    }

    /// The other end of a hierarchical connection: a sheet entry opens its child sheet's port, a port its entry.
    func crossProbeHierarchy(from id: Int) {
        guard let c = snapshot.component(id), c.componentKind == .netLabel else { return }
        if c.labelScope == "entry", let child = c.targetSheet,
           let port = snapshot.components.first(where: { $0.sheetId == child && $0.labelScope == "port" && $0.value == c.value }) {
            crossProbe(component: port.id, sheet: child)
        } else if c.labelScope == "port",
                  let entry = snapshot.components.first(where: { $0.labelScope == "entry" && $0.targetSheet == c.sheetId && $0.value == c.value }) {
            crossProbe(component: entry.id, sheet: entry.sheetId)
        }
    }

    func netPlaces(_ net: Int) -> NetPlacesReport? { net >= 0 ? engine.netPlaces(net) : nil }

    func setTitleBlock(_ block: TitleBlockInfo) {
        guard block != snapshot.titleBlock else { return }
        performChecked("Title block", invalidatesAnalysis: false) { $0.setTitleBlock(block) }
    }

    // MARK: - Graphical buses

    /// Draws a bus through `points` (snapped to the grid) and selects it.
    @discardableResult
    func addBus(named name: String, points: [CGPoint]) -> Int? {
        let title = name.trimmingCharacters(in: .whitespacesAndNewlines)
        var id: Int?
        performChecked("Drew bus \(title)", failureMessage: "\(title) is not bus notation — use e.g. D[0..7] or A[15..0],WR") {
            id = $0.addBus(title, points: points.map(SchematicAutoLayout.snap))
            return id != nil
        }
        if let id {
            selection = []
            selectedWire = nil
            selectedBus = id
        }
        return id
    }

    func renameBus(_ id: Int, to name: String) {
        let title = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard let bus = sheetSnapshot.bus(id), bus.name != title, !title.isEmpty else { return }
        performChecked("Renamed bus to \(title)", failureMessage: "\(title) is not bus notation") { $0.renameBus(id, to: title) }
    }

    func moveBus(_ id: Int, by delta: CGSize) {
        guard delta != .zero else { return }
        perform("Moved bus", invalidatesAnalysis: false) { $0.moveBus(id, by: delta) }
    }

    /// Rips an entry out of the bus for every member that has none yet.
    func ripBusEntries(_ id: Int) {
        var added = 0
        let done = performChecked("Ripped out bus entries", failureMessage: "Every member already has an entry") {
            added = $0.ripBusEntries(id)
            return added > 0
        }
        if done { statusMessage = "Ripped out \(added) bus entries" }
    }

    /// Wires the bus members to a part's pins of the same names (else its open pins), through entries on the bus.
    func connectBus(_ id: Int, toPart component: Int) {
        guard let part = snapshot.component(component) else { return }
        var made = 0
        let done = performChecked("Connected bus to \(part.ref)", failureMessage: "\(part.ref) has no open pins for this bus") {
            made = $0.connectBus(id, toPart: component)
            return made > 0
        }
        if done { statusMessage = "Connected \(made) bus members to \(part.ref)" }
    }

    /// Re-numbers reference designators by sheet and position.
    func annotate(byColumns: Bool = false, keepExisting: Bool = false, sheetNumbering: Bool = false, packUnits: Bool = false) {
        var changed = 0
        let done = performChecked("Annotated designators", invalidatesAnalysis: false,
                                  failureMessage: "Designators are already in order") { engine in
            changed = engine.annotate(byColumns: byColumns, keepExisting: keepExisting, sheetNumbering: sheetNumbering,
                                      packUnits: packUnits)
            return changed > 0
        }
        if done { statusMessage = "Annotated: \(changed) designator(s) changed" }
    }

    // MARK: - Design variants

    /// Adds a variant and makes it the active one.
    @discardableResult
    func addVariant(named name: String) -> Bool {
        let title = name.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !title.isEmpty else { return false }
        return performChecked("Added variant \(title)", invalidatesAnalysis: false,
                              failureMessage: "A variant named \(title) already exists") { engine in
            engine.addVariant(title, copying: snapshot.activeVariant.isEmpty ? nil : snapshot.activeVariant)
                && engine.setActiveVariant(title)
        }
    }

    func removeVariant(_ name: String) {
        performChecked("Deleted variant \(name)", invalidatesAnalysis: false) { $0.removeVariant(name) }
    }

    /// "" shows and exports the base design.
    func selectVariant(_ name: String) {
        guard name != snapshot.activeVariant else { return }
        // Simulation follows the active variant: its results are cleared.
        performChecked(name.isEmpty ? "Variant: base design" : "Variant: \(name)") {
            $0.setActiveVariant(name)
        }
    }

    /// Fits (or leaves off) a part in the active variant.
    func setFittedInVariant(_ id: Int, _ fitted: Bool) {
        let variant = snapshot.activeVariant
        guard !variant.isEmpty, let c = snapshot.component(id), c.isFitted != fitted else { return }
        performChecked(fitted ? "\(c.ref) fitted in \(variant)" : "\(c.ref) not fitted in \(variant)") {
            $0.setVariantPart(variant, component: id, fitted: fitted)
        }
    }

    /// The value fitted in the active variant ("" = the design value).
    func setVariantValue(_ id: Int, _ value: String) {
        let variant = snapshot.activeVariant
        let text = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !variant.isEmpty, let c = snapshot.component(id), (c.variantValue ?? "") != text else { return }
        let state = snapshot.variants.first { $0.name == variant }?.part(id)?.fitted
        performChecked("\(c.ref) = \(text.isEmpty ? c.value : text) in \(variant)") {
            $0.setVariantPart(variant, component: id, fitted: state, value: text)
        }
    }

    // MARK: - Analysis

    func runERC() {
        ercResults = engine.runERC()
        let errors = ercResults.filter { $0.severity == .error }.count
        let warnings = ercResults.filter { $0.severity == .warning }.count
        statusMessage = "ERC: \(errors) error(s), \(warnings) warning(s)"
    }

    func runValidation() {
        validationResults = engine.runCircuitValidation()
        let errors = validationResults.filter { $0.severity == .error }.count
        let warnings = validationResults.filter { $0.severity == .warning }.count
        statusMessage = "Circuit validation: \(errors) error(s), \(warnings) warning(s)"
    }

    /// Runs the full verification pipeline off the main thread and refreshes the individual check panels.
    @discardableResult
    func runVerification() async -> VerificationReport? {
        let engine = self.engine
        let hasPads = !snapshot.pads.isEmpty
        // The individual check panels' runs go with the verification, off the main thread (large boards).
        let run = await runBusy("Verifying design…") { () -> VerificationRun in
            VerificationRun(report: engine.runVerification(), erc: engine.runERC(),
                            validation: engine.runCircuitValidation(), drc: hasPads ? engine.runDRCChecked() : nil)
        }
        let report = run.report
        verificationReport = report
        verifiedRevision = revision
        ercResults = run.erc
        validationResults = run.validation
        if let drc = run.drc { recordDRC(drc, at: revision) }
        if let report {
            ProjectHooks.run("after-verify", project: documentURL)
            statusMessage = "Verification: \(report.verdict.title) — \(report.errors) error(s), \(report.warnings) warning(s)"
        } else {
            statusMessage = "Verification could not run"
        }
        return report
    }

    /// Saves the last verification report as Markdown.
    func exportVerificationReport() {
        guard let report = verificationReport else { return }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "md") ?? .plainText]
        panel.nameFieldStringValue = "\(snapshot.name.replacingOccurrences(of: "/", with: "-"))-verification.md"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try report.markdown.write(to: url, atomically: true, encoding: .utf8)
            statusMessage = "Exported verification report"
        } catch {
            present(error, title: "Export failed")
        }
    }

    func simulateDC() async {
        guard !isBusy else { return }  // one analysis at a time: overlapping runs would reset isBusy early
        let engine = self.engine
        let result = await runBusy("Solving DC operating point…", stoppable: true) { engine.simulateDC() }
        dcResult = result
        statusMessage = result.converged
            ? "DC operating point converged in \(result.iterations) iterations"
            : "DC analysis failed: \(result.error)"
    }

    func simulateTransient(stop: Double, step: Double, adaptive: Bool = false, trapezoidal: Bool = false) async {
        guard !isBusy else { return }  // one analysis at a time: overlapping runs would reset isBusy early
        let engine = self.engine
        let result = await runBusy("Running transient analysis…", stoppable: true) {
            engine.simulateTransient(stop: stop, step: step, adaptive: adaptive, trapezoidal: trapezoidal)
        }
        transientResult = result
        statusMessage = result.ok ? "Transient analysis: \(result.time.count) points" : "Transient failed: \(result.error)"
    }

    func simulateAC(start: String, stop: String, pointsPerDecade: Int, source: String) async {
        guard !isBusy else { return }  // one analysis at a time
        let engine = self.engine
        let result = await runBusy("Running AC analysis…", stoppable: true) {
            engine.simulateAC(start: start, stop: stop, pointsPerDecade: pointsPerDecade, source: source)
        }
        acResult = result
        statusMessage = result.ok ? "AC analysis: \(result.frequency.count) frequencies" : "AC analysis failed: \(result.error)"
    }

    func simulateDCSweep(source: String, start: String, stop: String, step: String) async {
        guard !isBusy else { return }  // one analysis at a time
        let engine = self.engine
        let result = await runBusy("Running DC sweep…", stoppable: true) {
            engine.simulateDCSweep(source: source, start: start, stop: stop, step: step)
        }
        dcSweepResult = result
        statusMessage = result.ok ? "DC sweep: \(result.values.count) points" : "DC sweep failed: \(result.error)"
    }

    func simulateMonteCarlo(net: String, measure: String, runs: Int) async {
        guard !isBusy else { return }  // one analysis at a time
        let engine = self.engine
        let result = await runBusy("Running Monte Carlo analysis…", stoppable: true) {
            engine.simulateMonteCarlo(net: net, measure: measure, runs: runs, seed: 1)
        }
        monteCarloResult = result
        statusMessage = result.ok ? "Monte Carlo: \(result.runs) runs" : "Monte Carlo failed: \(result.error)"
    }

    // MARK: - PCB

    func autoPlace(all: Bool) {
        guard !isBusy else { return }
        perform(all ? "Auto-placed all footprints" : "Placed new footprints", invalidatesAnalysis: false) {
            $0.autoPlace(all: all)
        }
        if all { fitToken &+= 1 }
    }

    /// `autoPlace(all:)` for the menus, buttons and agents: the placement runs off the main thread (a large board takes
    /// seconds), one undo step as before.
    func autoPlaceInBackground(all: Bool) async {
        let placed = await performInBackground(all ? "Auto-placed all footprints" : "Placed new footprints",
                                               busy: all ? "Auto-placing footprints…" : "Placing new footprints…",
                                               invalidatesAnalysis: false) { engine -> Bool in
            engine.autoPlace(all: all)
            return true
        }
        if placed != nil, all { fitToken &+= 1 }
    }

    /// The single Auto Route action: footprints not on the board yet are placed inside its shape first (outline,
    /// mounting holes and edge clearance respected, connected parts kept close), then the whole board is routed
    /// cleanly from scratch and DRC-checked. One undo step.
    func autoRouteBoard() async {
        let unplaced = snapshot.components.contains { !$0.componentKind.isVirtual && !$0.footprint.isEmpty && !$0.pcb.placed }
        await autoRoute(clearFirst: !snapshot.tracks.isEmpty || !snapshot.vias.isEmpty, placeMissing: unplaced)
    }

    /// Routes the board. `clearFirst` rips up the existing tracks and vias first (a clean re-route); `placeMissing`
    /// first places footprints that are not on the board yet. Either way it is a single undo step.
    func autoRoute(clearFirst: Bool = false, placeMissing: Bool = false, undoState: String? = nil) async {
        guard !isBusy else { return }
        let before = undoState ?? self.engine.stateJSON()
        if clearFirst {
            self.engine.clearRouting()
            refresh()
        }
        if placeMissing {
            let empty = snapshot.pads.isEmpty
            self.engine.autoPlace(all: false)
            refresh()
            if empty { fitToken &+= 1 }
        }
        let engine = self.engine
        // Progress arrives on the routing thread; it is shown on the main actor while this route runs.
        let channel = RouteProgressChannel { report in
            Task { @MainActor [weak self] in
                guard let self, self.routeChannel != nil else { return }
                self.routeProgress = report
            }
        }
        routeChannel = channel
        routeProgress = RouteProgressReport()
        let result = await runBusy(placeMissing ? "Placing and routing…" : "Autorouting…") {
            engine.autoRouteChecked(progress: channel)
        }
        routeChannel = nil
        routeProgress = nil
        if case .success(let stats) = result, stats.cancelled == true {
            // The core left the project as it was; the rip-up and placement made before routing are undone too.
            if placeMissing || clearFirst { try? engine.load(json: before) }
            refresh()
            statusMessage = "Autoroute stopped — the board is as it was"
            return
        }
        func recordUndo() {
            pushUndo(before)
            isDirty = true
            scheduleRecoverySave()
        }
        guard case .success(let stats) = result else {
            if placeMissing || clearFirst { recordUndo() }  // the placement / rip-up already happened
            if case .failure(let error) = result { present(error, title: "Autorouting failed") }
            refresh()
            return
        }
        recordUndo()
        refresh()
        routeStats = stats
        routeRevision = revision
        let checked = revision
        let drc = await runBusy("Checking design rules…") { engine.runDRCChecked() }
        recordDRC(drc, at: checked)
        let routed = stats.failed == 0
            ? "routed \(stats.routed)/\(stats.connections) connections, \(stats.vias) vias"
            : "routed \(stats.routed)/\(stats.connections) — \(stats.failed) failed (\(stats.failedNets.joined(separator: ", ")))"
        statusMessage = (placeMissing ? "Placed footprints inside the board outline and " : "") + routed
            + ((stats.lengthTuned ?? 0) > 0 ? ", \(stats.lengthTuned ?? 0) nets length-matched with serpentines" : "")
            + swapSummary(stats)
        statusMessage = statusMessage.prefix(1).uppercased() + statusMessage.dropFirst()
    }

    /// Stops the running autoroute at its next progress report; the board is left as it was.
    func cancelAutoRoute() {
        routeChannel?.cancel()
    }

    func clearRouting() {
        guard !isBusy else { return }
        perform("Cleared routing", invalidatesAnalysis: false) { $0.clearRouting() }
        routeStats = nil
    }

    // MARK: Net classes, outline, mounting holes, pours

    func setNetWidth(_ net: String, width: Double) {
        perform(width > 0 ? String(format: "Net class %@ = %.2f mm", net, width) : "Removed net class \(net)",
                invalidatesAnalysis: false) { $0.setNetWidth(net, width: width) }
    }

    /// Widens net classes to the IPC-2221 width for each net's simulated current.
    func autoSizeNetWidths() {
        var set: [String: Double] = [:]
        perform("Sized net classes from the simulation", invalidatesAnalysis: false) { set = $0.autoNetWidths() }
        statusMessage = set.isEmpty ? "All nets fit the default track width"
            : "Sized \(set.count) net class(es): " + set.sorted { $0.key < $1.key }
                .map { String(format: "%@ %.2f mm", $0.key, $0.value) }.joined(separator: ", ")
    }

    /// Conformal coating applied after assembly (spacing column A5, fab notes, reliability checks). Undoable.
    func setCoating(_ coating: ConformalCoating) {
        guard coating != snapshot.board.conformalCoating else { return }
        performChecked(coating == .none ? "No conformal coating" : "\(coating.title) conformal coating",
                       invalidatesAnalysis: false) { $0.setCoating(coating) }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Laminate, construction, controlled-impedance targets and backdrilling. Undoable; re-checks the design.
    func setStackup(material: String? = nil, construction: BoardConstruction? = nil, singleEnded: Double? = nil,
                    differential: Double? = nil, backdrill: Bool? = nil) {
        let b = snapshot.board
        let m = material ?? b.material, c = construction ?? b.boardConstruction
        let se = singleEnded ?? b.singleEndedImpedance, diff = differential ?? b.differentialImpedance
        let bd = backdrill ?? b.backdrill
        guard m != b.material || c != b.boardConstruction || se != b.singleEndedImpedance
                || diff != b.differentialImpedance || bd != b.backdrill else { return }
        performChecked("Stack-up: \(m), \(c.title)", invalidatesAnalysis: false) {
            $0.setStackup(material: m, construction: c, singleEnded: se, differential: diff, backdrill: bd)
        }
        if !drcResults.isEmpty { runDRC() }
    }

    func stackup() -> StackupReport { engine.stackup() }

    func lengthReport() -> LengthReport { engine.lengthReport() }

    // MARK: - Signal & power integrity

    func siSettings() -> SISettings { engine.siSettings() }
    func siNets() -> [SINetSummary] { engine.siNets() }
    func siNet(_ name: String, seriesOhms: Double? = nil) -> SINetAnalysis? { engine.siNet(name, seriesOhms: seriesOhms) }
    func siCrosstalk() -> SICrosstalkReport { engine.siCrosstalk() }
    func pdn() -> PDNReport { engine.pdn() }

    /// SI / PI sign-off in design verification and its limits (fractions of the swing). Undoable.
    func setSIOptions(signOff: Bool? = nil, overshootLimit: Double? = nil, crosstalkLimit: Double? = nil) {
        let current = engine.siSettings()
        let s = signOff ?? current.signOff
        let o = overshootLimit ?? current.overshootLimit, c = crosstalkLimit ?? current.crosstalkLimit
        guard s != current.signOff || o != current.overshootLimit || c != current.crosstalkLimit else { return }
        performChecked(s ? "Signal & power integrity sign-off on" : "Signal & power integrity sign-off off",
                       invalidatesAnalysis: false) { $0.setSIOptions(signOff: s, overshootLimit: o, crosstalkLimit: c) }
    }

    /// Driver / receiver model of a net ("" = automatic: the part's IBIS model or its logic family). Undoable.
    func assignSIModel(net: String, modelID: String) {
        performChecked(modelID.isEmpty ? "\(net): automatic driver model" : "\(net): driver model \(modelID)",
                       invalidatesAnalysis: false) { $0.assignSIModel(kind: "net", target: net, modelID: modelID) }
    }

    /// PDN inputs of a rail: allowed ripple (%) and load step (A); 0 derives them from the design. Undoable.
    func setPDNRail(_ net: String, ripplePercent: Double, transientAmps: Double) {
        let dc = engine.siSettings().rails.first { $0.net == net }?.dcCurrent ?? 0
        performChecked("\(net): PDN target", invalidatesAnalysis: false) {
            $0.setPDNRail(net, ripplePercent: ripplePercent, transientAmps: transientAmps, dcAmps: dc)
        }
    }

    /// Asks for an IBIS (.ibs) file and imports its buffer models; with `ref`, that part's pins use them. Undoable.
    func importIBIS(assignTo ref: String = "") {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "ibs") ?? .data, .plainText]
        panel.allowsMultipleSelection = false
        panel.message = "Choose an IBIS model file (.ibs) from the part vendor."
        panel.prompt = "Import"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        let text: String
        do {
            text = try String(contentsOf: url, encoding: .utf8)
        } catch {
            guard let latin = try? String(contentsOf: url, encoding: .isoLatin1) else {
                present(error, title: "Could not read \(url.lastPathComponent)")
                return
            }
            text = latin
        }
        var failure: Error?
        var count = 0
        performChecked("Imported IBIS models from \(url.lastPathComponent)", invalidatesAnalysis: false,
                       failureMessage: "\(url.lastPathComponent): no IBIS models imported") { engine in
            do {
                count = try engine.importIBIS(text, corner: "typ", ref: ref)
                return true
            } catch {
                failure = error
                return false
            }
        }
        if let failure { present(failure, title: "Could not import \(url.lastPathComponent)") }
        else { statusMessage = "Imported \(count) IBIS model(s) from \(url.lastPathComponent)" }
    }

    // MARK: Channel analysis and PI planning

    func siLineLoss(roughness: String) -> SILineLossReport { engine.siLineLoss(roughness: roughness) }

    /// S-parameters, step response and eye of a net or differential pair, off the main thread.
    func analyzeChannel(net: String, partner: String, settings: SIChannelSettings, touchstone: String?,
                        touchstonePorts: Int) async -> Result<SIChannelReport, EDAEngineError> {
        let engine = self.engine
        return await runBusy("Analysing channel \(net)…") {
            engine.siChannel(net: net, partner: partner, settings: settings, touchstone: touchstone, touchstonePorts: touchstonePorts)
        }
    }

    /// Copper foil profile for loss ("" = by laminate). Undoable.
    func setCopperFoil(_ foil: String) {
        guard (engine.siSettings().copperFoil ?? "") != foil else { return }
        performChecked(foil.isEmpty ? "Copper foil by laminate" : "Copper foil \(foil)", invalidatesAnalysis: false) {
            $0.setCopperFoil(foil)
        }
    }

    /// Channel lines from the 2D field solver instead of the closed-form stack-up formulas. Undoable.
    func setFieldSolverLines(_ on: Bool) {
        guard (engine.siSettings().fieldSolverLines ?? false) != on else { return }
        performChecked(on ? "Field-solved lines" : "Closed-form lines", invalidatesAnalysis: false) { $0.setFieldSolverLines(on) }
    }

    /// Checks the eye of `net` at `bitRate` in sign-off (0 removes the check). Undoable.
    func setSIChannel(_ net: String, bitRate: Double, maskHeight: Double, maskWidthUi: Double) {
        performChecked(bitRate > 0 ? "\(net): channel check" : "\(net): no channel check", invalidatesAnalysis: false) {
            $0.setSIChannel(net, bitRate: bitRate, maskHeight: maskHeight, maskWidthUi: maskWidthUi)
        }
    }

    /// Regulator output resistance (Ω) and loop bandwidth (Hz) of a rail; 0 derives them. Undoable.
    func setPDNRegulator(_ net: String, outputOhms: Double, loopBandwidth: Double) {
        performChecked("\(net): regulator model", invalidatesAnalysis: false) {
            $0.setPDNRegulator(net, outputOhms: outputOhms, loopBandwidth: loopBandwidth)
        }
    }

    func pdnCavity(_ net: String) -> PDNCavityReport? { engine.pdnCavity(net) }
    func pdnDecapPlan(_ net: String) -> PDNDecapPlanReport? { engine.pdnDecapPlan(net) }
    func pdnIRMap(_ net: String) -> PDNIRMapReport? { engine.pdnIRMap(net) }

    /// Saves the channel's S-parameters as Touchstone (.s2p single-ended, .s4p differential).
    func exportChannelTouchstone(net: String, partner: String, differential: Bool, settings: SIChannelSettings) {
        let text: String
        do {
            text = try engine.siChannelTouchstone(net: net, partner: partner, settings: settings)
        } catch {
            present(error, title: "Touchstone export failed")
            return
        }
        let ext = differential ? "s4p" : "s2p"
        let panel = NSSavePanel()
        panel.allowedContentTypes = [UTType(filenameExtension: ext) ?? .plainText]
        panel.nameFieldStringValue = "\(net.replacingOccurrences(of: "/", with: "-")).\(ext)"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try text.write(to: url, atomically: true, encoding: .utf8)
            statusMessage = "Exported \(url.lastPathComponent)"
        } catch {
            present(error, title: "Touchstone export failed")
        }
    }

    /// Asks for a Touchstone (.sNp) file: its name, text and the port count from the extension (0 if none).
    func chooseTouchstoneFile() -> (name: String, text: String, ports: Int)? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = ["s1p", "s2p", "s4p", "snp"].compactMap { UTType(filenameExtension: $0) } + [.plainText, .data]
        panel.allowsMultipleSelection = false
        panel.message = "Choose a Touchstone S-parameter file (.s2p single-ended or .s4p differential)."
        panel.prompt = "Import"
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        guard let text = (try? String(contentsOf: url, encoding: .utf8)) ?? (try? String(contentsOf: url, encoding: .isoLatin1)) else {
            present(EDAEngineError.operationFailed("The file could not be read."), title: "Could not import \(url.lastPathComponent)")
            return nil
        }
        let ext = url.pathExtension.lowercased()
        var ports = 0
        if ext.count >= 3, ext.hasPrefix("s"), ext.hasSuffix("p"), let n = Int(ext.dropFirst().dropLast()) { ports = n }
        return (url.lastPathComponent, text, ports)
    }

    /// Length / phase matching on Auto Route and its tolerances (mm). Undoable.
    func setLengthMatching(enabled: Bool? = nil, pairSkew: Double? = nil, bus: Double? = nil) {
        let b = snapshot.board
        let e = enabled ?? b.lengthTuning, p = pairSkew ?? b.pairSkewTolerance, t = bus ?? b.busLengthTolerance
        guard e != b.lengthTuning || p != b.pairSkewTolerance || t != b.busLengthTolerance else { return }
        performChecked(e ? "Length matching" : "No length matching", invalidatesAnalysis: false) {
            $0.setLengthMatching(enabled: e, pairSkew: p, bus: t)
        }
    }

    /// HDI vias and via-in-pad (VIPPO). Turning HDI on converts the routed vias right away. Undoable.
    func setHDI(enabled: Bool? = nil, microviaDrill: Double? = nil, microviaDiameter: Double? = nil, viaInPad: Bool? = nil) {
        let b = snapshot.board
        let e = enabled ?? b.hdi, d = microviaDrill ?? b.microviaDrill, p = microviaDiameter ?? b.microviaDiameter
        let vip = viaInPad ?? b.viaInPad
        guard e != b.hdi || d != b.microviaDrill || p != b.microviaDiameter || vip != b.viaInPad else { return }
        var changed = 0
        performChecked(e ? "HDI vias" : "Through vias only", invalidatesAnalysis: false) {
            guard $0.setHDI(enabled: e, microviaDrill: d, microviaDiameter: p, viaInPad: vip) else { return false }
            if e { changed = $0.applyHDI() }
            return true
        }
        if changed > 0 { statusMessage = "\(changed) via\(changed == 1 ? "" : "s") cut to blind / buried / microvia spans" }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Tunes the routed board now: serpentines on the short members of pairs and buses. Undoable.
    func tuneLengths() {
        var tuned = 0
        performChecked("Tune lengths", invalidatesAnalysis: false, failureMessage: "Lengths already matched") {
            tuned = $0.tuneLengths()
            return tuned > 0
        }
        if tuned > 0 {
            statusMessage = "Added serpentines to \(tuned) net\(tuned == 1 ? "" : "s")"
            if !drcResults.isEmpty { runDRC() }
        }
    }

    // MARK: Interactive routing

    /// The route in progress with the Route tool (nil when idle). The board itself changes only when it finishes.
    @Published private(set) var routePreview: RoutePreview?
    /// Route tool settings: push-and-shove (otherwise walkaround) and 45° corners (otherwise 90°).
    /// Shove (push other nets aside), Walk around (route round them) or Highlight (go anywhere, list collisions).
    @Published var routerMode: RouterModeChoice = .shove {
        didSet { if routerMode != oldValue { applyRouterOptions() } }
    }
    @Published var routerDiagonal = true {
        didSet { if routerDiagonal != oldValue { applyRouterOptions() } }
    }

    /// The via V places (blind / buried and micro vias need HDI in Board Setup).
    @Published var routerViaType: RouterViaChoice = .through {
        didSet { if routerViaType != oldValue { applyRouterOptions() } }
    }

    /// Bus routing: the next route takes the clicked pad and the next pads of its row (`routerBusWidth` nets).
    @Published var routerBus = false
    @Published var routerBusWidth = 4

    /// Rounded corners: arcs (as short chords) instead of sharp 45° / 90° corners on single-track routes.
    @Published var routerRounded = false {
        didSet { if routerRounded != oldValue { applyRouterOptions() } }
    }

    /// Rounded corners as true arcs (pairs and buses turn on concentric arcs) instead of short straight chords.
    @Published var routerArcs = true {
        didSet { if routerArcs != oldValue { applyRouterOptions() } }
    }

    /// Tracks selected with the Select tool (click a track, ⇧-click adds or removes) for the track commands.
    @Published var selectedTracks: Set<Int> = []

    /// Any-angle routing (free posture): the head is one straight track at any angle; drags follow at any angle.
    @Published var routerAnyAngle = false {
        didSet { if routerAnyAngle != oldValue { applyRouterOptions() } }
    }

    /// Multi-route: start points picked with ⇧-click in the Route tool; the next plain click routes them together.
    @Published var multiStarts: [CGPoint] = []

    /// Loop removal: finishing a route removes the old path it makes redundant (and vias left unconnected).
    @Published var routerRemoveLoops = true {
        didSet { if routerRemoveLoops != oldValue { applyRouterOptions() } }
    }

    /// Teardrops on every finished route where it meets pads and vias.
    @Published var routerTeardrops = false {
        didSet { if routerTeardrops != oldValue { applyRouterOptions() } }
    }

    /// Outline of the teardrops the router and the Teardrops command add: straight or curved.
    @Published var teardropStyle = TeardropStyleChoice.straight {
        didSet { if teardropStyle != oldValue { applyRouterOptions() } }
    }

    /// Hug: a dragged track bends around pads and other copper it cannot push instead of stopping short.
    @Published var routerHugDrag = true {
        didSet { if routerHugDrag != oldValue { applyRouterOptions() } }
    }

    /// Tune lengths while routing: a bus's (or a matched net's, or a pair's) short members get meanders live in the
    /// preview, written when the route is finished.
    @Published var routerTuneWhileRouting = false {
        didSet { if routerTuneWhileRouting != oldValue { applyRouterOptions() } }
    }

    /// The PCB drawing tool in use (nil = none) and the net a drawn pour takes ("" = the ground net).
    @Published var pcbShapeTool: PCBShapeTool?
    /// Net highlighted on the board until cleared (right-click a pad or track → Highlight Net; Esc clears it).
    @Published var pcbHighlightNet: Int?
    /// The earlier version picked with Compare with File: the board draws what changed from it until cleared.
    @Published var pcbCompare: DesignSnapshot?
    @Published var pourNet = ""

    /// Route tool track width and differential pair gap in mm (0 = the board's net-class rules).
    @Published var routerWidth = 0.0 {
        didSet { if routerWidth != oldValue { applyRouterOptions() } }
    }
    @Published var routerPairGap = 0.0 {
        didSet { if routerPairGap != oldValue { applyRouterOptions() } }
    }

    private var routerOptions: String {
        EDAEngine.routingOptions(mode: routerMode, diagonal: routerDiagonal, via: routerViaType, rounded: routerRounded,
                                 arcs: routerArcs, anyAngle: routerAnyAngle, removeLoops: routerRemoveLoops,
                                 teardrops: routerTeardrops, hug: routerHugDrag, teardropStyle: teardropStyle,
                                 tune: routerTuneWhileRouting, width: routerWidth, pairGap: routerPairGap)
    }

    /// Shows a router reply: a refused step keeps the route and reports why.
    private func showRoute(_ preview: RoutePreview?) {
        guard let preview else {
            statusMessage = "Route: no reply from the core"
            return
        }
        routePreview = preview.active ? preview : nil
        statusMessage = preview.error ?? preview.status
    }

    /// Starts a route (or a differential pair) on the pad, via or track at `point` on copper layer `layer`.
    func beginRoute(at point: CGPoint, layer: Int, pair: Bool) {
        guard !isBusy else { return }
        settleRouteMoves()
        showRoute(engine.routerBegin(at: point, layer: layer, pair: pair, options: routerOptions))
    }

    /// Starts a bus on the pad at `point`: it and the next pads of the same part's row route as one bundle.
    func beginBus(at point: CGPoint, layer: Int) {
        guard !isBusy else { return }
        settleRouteMoves()
        showRoute(engine.routerBeginBus(at: point, layer: layer, count: routerBusWidth, options: routerOptions))
    }

    /// Fans out the selected parts: an escape track and a via (the Route tool's via type) on each SMD pad that has
    /// somewhere to go and no copper yet. One undo step.
    func fanoutSelection() {
        let parts = snapshot.components.filter { selection.contains($0.id) && $0.pcb.placed }.map(\.id).sorted()
        guard !parts.isEmpty else {
            statusMessage = "Fanout: select a part on the board first"
            return
        }
        if routePreview != nil { cancelRoute() }
        var fanned = 0, failed = 0
        var message = ""
        let done = performChecked("Fanout", invalidatesAnalysis: false, failureMessage: "Fanout: nothing to fan out") {
            for id in parts {
                guard let result = $0.fanout(component: id, via: routerViaType) else { continue }
                fanned += result.fanned
                failed += result.failed.count
                message = result.message
            }
            return fanned > 0
        }
        if done {
            statusMessage = parts.count == 1 ? message
                : "\(fanned) pad\(fanned == 1 ? "" : "s") fanned out" + (failed > 0 ? ", \(failed) without room" : "")
            if !drcResults.isEmpty { runDRC() }
        }
    }

    // Head updates run off the main thread, latest wins: while one is computed, newer cursor positions replace each
    // other in `routePendingPoint` and only the newest runs next. Results of an older session or of a superseded
    // update (`routeGeneration`) are dropped. Discrete steps (corner, via, finish, cancel, options) first cancel the
    // update in flight (`sieda_router_abort`, lock-free) so they never wait for a slow head.
    private var routeMoveRunning = false
    private var routePendingPoint: CGPoint?
    private var routeGeneration = 0
    // The same for the "place new parts" ghost (DesignStore+Placement.swift): one placement check in flight, the
    // newest cursor position waiting, results of a superseded check dropped.
    var placementCheckRunning = false
    var placementPendingPoint: CGPoint?
    var placementGeneration = 0

    /// Moves the head of the route to the cursor (shoving or walking around as set), off the main thread.
    func moveRoute(to point: CGPoint) {
        guard routePreview != nil, !isBusy else { return }
        if routeMoveRunning {
            routePendingPoint = point
            return
        }
        routeMoveRunning = true
        let generation = routeGeneration
        let engine = self.engine
        Task { @MainActor [weak self] in
            let preview = await Task.detached(priority: .userInitiated) { engine.routerMove(to: point) }.value
            self?.routeMoveFinished(preview, generation: generation)
        }
    }

    private func routeMoveFinished(_ preview: RoutePreview?, generation: Int) {
        routeMoveRunning = false
        if generation == routeGeneration, routePreview != nil, let preview, preview.aborted != true {
            routePreview = preview.active ? preview : nil
            statusMessage = preview.status
        }
        if let next = routePendingPoint {
            routePendingPoint = nil
            moveRoute(to: next)
        }
    }

    /// Before a synchronous router step: drops queued head moves, cancels the one in flight and makes its result
    /// stale. The step then waits at most for that update to stop, not for it to finish.
    private func settleRouteMoves() {
        routePendingPoint = nil
        routeGeneration &+= 1
        if routeMoveRunning { engine.routerAbort() }
    }

    /// Moves the head to `point` now (a click places what is under the cursor, not an older position).
    func moveRouteNow(to point: CGPoint) {
        guard routePreview != nil, !isBusy else { return }
        settleRouteMoves()
        guard let preview = engine.routerMove(to: point) else { return }
        routePreview = preview.active ? preview : nil
        statusMessage = preview.status
    }

    /// Places the head as it is (a click); the route continues from its end.
    func placeRouteCorner() {
        guard routePreview != nil, !isBusy else { return }
        settleRouteMoves()
        showRoute(engine.routerFix())
    }

    /// Places a via at the end of the head and continues on the other side of the board (V).
    func addRouteVia(reverse: Bool = false) {
        guard routePreview != nil, !isBusy else { return }
        settleRouteMoves()
        showRoute(engine.routerAddVia(reverse: reverse))
    }

    /// Writes the route and every shoved track and via into the board as one undo step (Enter / double-click, or
    /// the end of a drag). With `point` the head first moves there, so the board gets exactly what is dropped.
    func finishRoute(at point: CGPoint? = nil) {
        settleRouteMoves()
        if let point, routePreview != nil, !isBusy, let preview = engine.routerMove(to: point) {
            routePreview = preview.active ? preview : nil
        }
        guard let kind = routePreview?.kind else { return }
        var result = RouteCommitResult(ok: false, error: nil, addedTracks: [], addedVias: [])
        let action = kind == "drag" ? "Dragged track" : kind == "via" ? "Moved via"
            : kind == "corner" || kind == "multidrag" ? "Dragged tracks" : "Routed track"
        let done = performChecked(action, invalidatesAnalysis: false, failureMessage: "Nothing was routed") {
            result = $0.routerCommit()
            return result.ok && !(result.addedTracks.isEmpty && result.addedVias.isEmpty)
        }
        routePreview = nil
        if !done, let error = result.error { statusMessage = error }
        if done, let tuned = result.tuneStatus, !tuned.isEmpty { statusMessage = tuned }
        if done, !drcResults.isEmpty { runDRC() }
    }

    /// Drops the route in progress; the board is unchanged (Esc).
    func cancelRoute() {
        guard routePreview != nil else { return }
        settleRouteMoves()
        engine.routerCancel()
        routePreview = nil
        statusMessage = "Route cancelled"
    }

    private func applyRouterOptions() {
        guard routePreview != nil, !isBusy else { return }
        settleRouteMoves()
        showRoute(engine.routerSetOptions(routerOptions))
    }

    /// Starts dragging track segment `trackId` (Select tool): it follows the cursor parallel to itself and shoves
    /// other nets' copper (or stops at it with Walk around). Finish with `finishRoute`, drop with `cancelRoute`.
    @discardableResult
    func beginTrackDrag(_ trackId: Int, at point: CGPoint) -> Bool {
        guard !isBusy else { return false }
        settleRouteMoves()
        showRoute(engine.routerBeginDrag(track: trackId, at: point, options: routerOptions))
        return routePreview != nil
    }

    /// Starts dragging via `viaId` (Select tool); the tracks ending on it follow.
    @discardableResult
    func beginViaDrag(_ viaId: Int, at point: CGPoint) -> Bool {
        guard !isBusy else { return false }
        settleRouteMoves()
        showRoute(engine.routerBeginViaDrag(via: viaId, at: point, options: routerOptions))
        return routePreview != nil
    }

    /// Starts dragging the corner of track `trackId` nearest to `point` (Select tool on a track's corner).
    @discardableResult
    func beginCornerDrag(_ trackId: Int, at point: CGPoint) -> Bool {
        guard !isBusy else { return false }
        settleRouteMoves()
        showRoute(engine.routerBeginCornerDrag(track: trackId, at: point, options: routerOptions))
        return routePreview != nil
    }

    /// Starts dragging the selected tracks together (Select tool on one of several selected tracks).
    @discardableResult
    func beginMultiDrag(_ trackIds: [Int], at point: CGPoint) -> Bool {
        guard !isBusy, !trackIds.isEmpty else { return false }
        settleRouteMoves()
        showRoute(engine.routerBeginMultiDrag(tracks: trackIds, at: point, options: routerOptions))
        return routePreview != nil
    }

    /// Multi-route: ⇧-click in the Route tool picks (or drops) a start point.
    func toggleMultiStart(_ point: CGPoint) {
        if let i = multiStarts.firstIndex(where: { hypot($0.x - point.x, $0.y - point.y) < 0.3 }) {
            multiStarts.remove(at: i)
        } else {
            multiStarts.append(point)
        }
        statusMessage = multiStarts.isEmpty ? "Multi-route: no nets picked"
            : "Multi-route: \(multiStarts.count) picked — click the last pad (without ⇧) to route them together"
    }

    /// Routes the picked start points and `point` together as one bundle.
    func beginMultiRoute(adding point: CGPoint, layer: Int) {
        guard !isBusy else { return }
        settleRouteMoves()
        let starts = multiStarts + [point]
        multiStarts = []
        showRoute(engine.routerBeginMulti(starts: starts, layer: layer, options: routerOptions))
    }

    // MARK: Interactive length tuning

    /// The Tune Length tool's session: the track picked, where it was clicked, and the meanders it would add.
    struct TuneSession: Equatable {
        var track: Int
        var point: CGPoint
        /// Target length (mm); 0 = the length rule, match group, or the longest member of the net's pair / bus.
        var target: Double
        var preview: TunePreview?
        /// Drag-along: the meanders go between `point` and this point of the track (nil: anywhere on the net).
        var spanEnd: CGPoint?
    }

    @Published private(set) var tuneSession: TuneSession?
    /// Meander pattern and corner shape, coupled pair tuning and phase (skew) bumps.
    @Published var tuneStyle: MeanderStyleChoice = .accordion {
        didSet { if tuneStyle != oldValue { updateTunePreview() } }
    }
    @Published var tuneCorner: MeanderCornerChoice = .square {
        didSet { if tuneCorner != oldValue { updateTunePreview() } }
    }
    @Published var tuneCoupled = false {
        didSet { if tuneCoupled != oldValue { updateTunePreview() } }
    }
    @Published var tunePhase = false {
        didSet { if tunePhase != oldValue { updateTunePreview() } }
    }
    /// Meander height limit and leg spacing (edge to edge) in mm; 0 = the core's defaults.
    @Published var tuneAmplitude = 0.0 {
        didSet { if tuneAmplitude != oldValue { updateTunePreview() } }
    }
    @Published var tuneSpacing = 0.0 {
        didSet { if tuneSpacing != oldValue { updateTunePreview() } }
    }

    /// Picks the track to tune; the preview shows the meanders before anything changes.
    func beginTune(track trackId: Int, at point: CGPoint) {
        guard !isBusy else { return }
        if routePreview != nil { cancelRoute() }
        tuneSession = TuneSession(track: trackId, point: point, target: 0, preview: nil, spanEnd: nil)
        updateTunePreview()
        // A net outside any pair / bus group has nothing to match: start from its length plus 1 mm.
        if let preview = tuneSession?.preview, !preview.ok, preview.group.isEmpty {
            tuneSession?.target = ((preview.before + 1) * 10).rounded(.up) / 10
            updateTunePreview()
        }
    }

    /// Sets the target length (mm, 0 = match the group) and refreshes the preview.
    func setTuneTarget(_ target: Double) {
        guard tuneSession != nil, target.isFinite else { return }
        tuneSession?.target = max(0, target)
        updateTunePreview()
    }

    /// Drag-along tuning: the meanders' stretch follows the pointer along the track (Tune tool drag).
    func dragTune(to point: CGPoint) {
        guard tuneSession != nil, point.x.isFinite, point.y.isFinite else { return }
        tuneSession?.spanEnd = point
        updateTunePreview()
    }

    private func tuneRequest(_ session: TuneSession, apply: Bool) -> EDAEngine.TuneRequest {
        EDAEngine.TuneRequest(target: session.target, amplitude: tuneAmplitude, spacing: tuneSpacing, near: session.point,
                              spanEnd: session.spanEnd, style: tuneStyle, corner: tuneCorner, coupled: tuneCoupled,
                              phase: tunePhase, apply: apply)
    }

    private func updateTunePreview() {
        guard let session = tuneSession, !isBusy else { return }
        let preview = engine.routerTune(track: session.track, request: tuneRequest(session, apply: false))
        tuneSession?.preview = preview
        statusMessage = preview?.message ?? "Tune: no reply from the core"
    }

    /// Writes the previewed meanders into the board as one undo step (Enter).
    func applyTune() {
        guard let session = tuneSession else { return }
        var result: TunePreview?
        let done = performChecked("Tuned length", invalidatesAnalysis: false, failureMessage: "Length not changed") {
            result = $0.routerTune(track: session.track, request: tuneRequest(session, apply: true))
            return result?.applied == true
        }
        tuneSession = nil
        if let result {
            statusMessage = done ? String(format: "%@ — %.2f mm (target %.2f mm)", result.message, result.after, result.target)
                                 : result.message
        }
        if done, !drcResults.isEmpty { runDRC() }
    }

    func cancelTune() {
        guard tuneSession != nil else { return }
        tuneSession = nil
        statusMessage = "Tuning cancelled"
    }

    /// Solder mask colour of the board (3D assembly view and fabrication order). Undoable.
    func setSolderMask(_ mask: SolderMaskColour) {
        guard mask != snapshot.board.mask else { return }
        performChecked("\(mask.title) solder mask", invalidatesAnalysis: false) { $0.setSolderMask(mask) }
    }

    func setAutoSizeNets(_ enabled: Bool) {
        perform(enabled ? "Autorouter sizes power nets" : "Autorouter uses the net classes as set",
                invalidatesAnalysis: false) { $0.setAutoSizeNets(enabled) }
    }

    func applyOutlinePreset(_ preset: BoardOutlinePreset, width: Double, height: Double, parameter: Double) {
        var ok = false
        perform("Board outline: \(preset.title)", invalidatesAnalysis: false) {
            ok = $0.applyOutlinePreset(preset, width: width, height: height, parameter: parameter)
        }
        if !ok { alert = AlertItem(title: "Outline not changed", message: "Check the outline dimensions.") }
        fitToken &+= 1
    }

    /// Adds four mounting holes on a square pattern centred on the board (30.5 × 30.5 mm M3 flight-controller stack…).
    func addMountingPattern(spacing: Double, drill: Double, keepout: Double) {
        let board = snapshot.board
        let centre = CGPoint(x: board.width / 2, y: board.height / 2)
        perform(String(format: "Added %.1f × %.1f mm mounting holes", spacing, spacing), invalidatesAnalysis: false) {
            engine in
            for dx in [-spacing / 2, spacing / 2] {
                for dy in [-spacing / 2, spacing / 2] {
                    engine.addMountingHole(at: CGPoint(x: centre.x + dx, y: centre.y + dy), drill: drill, keepout: keepout)
                }
            }
        }
    }

    func addMountingHole(at point: CGPoint, drill: Double = 3.2, keepout: Double = 6.4) {
        perform("Added mounting hole", invalidatesAnalysis: false) {
            $0.addMountingHole(at: point, drill: drill, keepout: keepout)
        }
    }

    func clearMountingHoles() {
        perform("Removed mounting holes", invalidatesAnalysis: false) { $0.clearMountingHoles() }
    }

    func addZone(net: String, layer: Int, plane: Bool) {
        var index: Int?
        let name = snapshot.board.layerName(layer)
        perform(plane ? "\(net) plane on \(name)" : "\(net) pour on \(name)", invalidatesAnalysis: false) {
            index = $0.addZone(net: net, layer: layer, plane: plane)
        }
        if index == nil { alert = AlertItem(title: "Pour not added", message: "\(name) is not in the layer stack.") }
    }

    /// PCB drawing tools: a pour, the board outline or a cutout (click corners, double-click or Enter closes) and a
    /// routing keep-out (drag a rectangle).
    enum PCBShapeTool: String, CaseIterable { case pour, outline, cutout, keepout, slot, dimension }
    /// Closes a drawn shape (≥ 3 corners) as `tool`; a pour takes `pourNet` (empty = the ground net) on `layer`.
    func finishShape(_ tool: PCBShapeTool, points: [CGPoint], layer: Int) {
        guard points.count >= 3 else {
            statusMessage = "Draw at least three corners, then double-click or press Enter"
            return
        }
        switch tool {
        case .pour:
            let net = pourNet.isEmpty
                ? (snapshot.nets.first(where: { $0.ground })?.name ?? snapshot.nets.first?.name ?? "") : pourNet
            guard !net.isEmpty else {
                statusMessage = "Pour: the schematic has no nets yet"
                return
            }
            performChecked("\(net) pour on \(snapshot.board.layerName(layer))", invalidatesAnalysis: false,
                           failureMessage: "Pour: the shape needs three corners on a copper layer") {
                $0.addZone(net: net, layer: layer, polygon: points) != nil
            }
        case .outline:
            performChecked("Drew board outline", invalidatesAnalysis: false,
                           failureMessage: "Outline: the shape must not cross itself") { $0.setOutline(points) }
            fitToken &+= 1
        case .cutout:
            performChecked("Added board cutout", invalidatesAnalysis: false,
                           failureMessage: "Cutout: draw it inside the board") { $0.addCutout(points) }
        case .keepout, .slot, .dimension:
            break
        }
    }

    /// Width of the slots the slot tool cuts (mm).
    var slotWidth: Double {
        get { UserDefaults.standard.object(forKey: "pcb.slotWidth") as? Double ?? 1.0 }
        set { UserDefaults.standard.set(newValue, forKey: "pcb.slotWidth"); objectWillChange.send() }
    }

    /// A milled slot from `a` to `b` (`slotWidth` wide, round ends) as a board cutout.
    func addSlot(from a: CGPoint, to b: CGPoint) {
        let dx = Double(b.x - a.x), dy = Double(b.y - a.y), r = slotWidth / 2
        guard hypot(dx, dy) > 0.05 else { return }
        let angle = atan2(dy, dx), steps = 8
        var points: [CGPoint] = []
        for (centre, start) in [(b, angle - Double.pi / 2), (a, angle + Double.pi / 2)] {  // a half circle round each end
            for i in 0...steps {
                let t = start + Double(i) * Double.pi / Double(steps)
                points.append(CGPoint(x: Double(centre.x) + r * cos(t), y: Double(centre.y) + r * sin(t)))
            }
        }
        performChecked(String(format: "Added %.2f mm slot", slotWidth), invalidatesAnalysis: false,
                       failureMessage: "Slot: draw it inside the board") { $0.addCutout(points) }
    }

    /// Adds a routing keep-out (no tracks or vias; `pours` keeps copper pours out too) over `rect` on `layer`
    /// (-1 = every copper layer).
    func addKeepout(_ rect: CGRect, layer: Int, pours: Bool = false) {
        guard rect.width > 0.1, rect.height > 0.1 else { return }
        let r = rect.standardized
        let keepout = RouteKeepout(name: "Keep-out \(snapshot.board.keepouts.count + 1)", x0: r.minX, y0: r.minY,
                                   x1: r.maxX, y1: r.maxY, layer: layer, tracks: true, vias: true, pours: pours ? true : nil)
        let all = snapshot.board.keepouts + [keepout]
        perform("Added keep-out", invalidatesAnalysis: false) { _ = $0.setKeepouts(all) }
    }

    /// Copper thieving on (with these dots) or off (nil); one undo step through the core tool.
    func setThieving(_ t: ThievingInfo?) {
        let args: [String: Any] = t.map { ["pitch": $0.pitch, "size": $0.size, "clearance": $0.clearance] } ?? ["pitch": 0]
        _ = runTool("pcb_copper_thieving", arguments: args)
    }

    /// Turns a keep-out into an isolation barrier (no copper of any kind on any layer, checked against the isolation
    /// gap) or back.
    func setKeepoutBarrier(_ keepout: RouteKeepout, _ on: Bool) {
        let all = snapshot.board.keepouts.map { k -> RouteKeepout in
            guard k == keepout else { return k }
            var b = k
            b.barrier = on ? true : nil
            if on { (b.tracks, b.vias, b.pours, b.layer) = (true, true, true, -1) }
            return b
        }
        perform(on ? "Made isolation barrier" : "Made plain keep-out", invalidatesAnalysis: false) { _ = $0.setKeepouts(all) }
    }

    func removeKeepout(_ keepout: RouteKeepout) {
        let rest = snapshot.board.keepouts.filter { $0 != keepout }
        perform("Removed keep-out", invalidatesAnalysis: false) { _ = $0.setKeepouts(rest) }
    }

    /// A drawing dimension from `a` to `b` (board mm), its line 3 mm to the side.
    func addDimension(from a: CGPoint, to b: CGPoint) {
        guard hypot(b.x - a.x, b.y - a.y) > 0.05 else { return }
        let dims = snapshot.board.dimensions + [BoardDimensionInfo(a: BoardPoint(x: a.x, y: a.y), b: BoardPoint(x: b.x, y: b.y))]
        performChecked(String(format: "Added %.2f mm dimension", hypot(b.x - a.x, b.y - a.y)), invalidatesAnalysis: false) {
            $0.setDimensions(dims)
        }
    }

    func removeDimension(at index: Int) {
        var dims = snapshot.board.dimensions
        guard dims.indices.contains(index) else { return }
        dims.remove(at: index)
        performChecked("Removed dimension", invalidatesAnalysis: false) { $0.setDimensions(dims) }
    }

    func removeCutout(at index: Int) {
        perform("Removed board cutout", invalidatesAnalysis: false) { $0.removeCutout(at: index) }
    }

    func removeZone(at index: Int) {
        perform("Removed copper pour", invalidatesAnalysis: false) { $0.removeZone(at: index) }
    }

    /// One-click ground: GND pours on top and bottom (2-layer) or a ground plane on inner 1 (4+ layers).
    func addGroundPours() {
        guard let ground = snapshot.nets.first(where: { $0.ground })?.name ?? snapshot.nets.first(where: { $0.name == "GND" })?.name
        else {
            alert = AlertItem(title: "No ground net", message: "Add a ground symbol to the schematic first.")
            return
        }
        let board = snapshot.board
        var wanted: [(layer: Int, plane: Bool)] = []
        if board.layerCount >= 4 {
            wanted = [(layer: 1, plane: true), (layer: board.bottomLayer, plane: false)]
        } else {
            wanted = [(layer: board.bottomLayer, plane: false)]
            if board.layerCount > 1 { wanted.append((layer: 0, plane: false)) }
        }
        // Pressing it again (or after adding some by hand) never stacks a second pour of GND on the same layer.
        wanted.removeAll { w in snapshot.zones.contains { $0.net == ground && $0.layer == w.layer } }
        guard !wanted.isEmpty else {
            statusMessage = "Ground pours are already on the board"
            return
        }
        perform("Ground pours added", invalidatesAnalysis: false) { engine in
            for w in wanted { engine.addZone(net: ground, layer: w.layer, plane: w.plane) }
        }
    }

    /// Resizes the board outline to the placed footprints plus `margin` millimetres.
    func fitBoard(margin: Double = 2.5) {
        guard !isBusy else { return }
        let fitted = performChecked("Fitted board to components", invalidatesAnalysis: false,
                                    failureMessage: snapshot.board.hasCustomOutline
                                        ? "The board has a custom outline: change it in Board Setup"
                                        : "No placed footprints to fit the board to") { $0.fitBoard(margin: margin) }
        if fitted { fitToken &+= 1 }
    }

    /// Stores DRC results for the current revision; a core failure is shown instead of reading as "passed".
    /// `at`: the revision the check ran on (a background check may finish after later edits; its results then read
    /// as stale). Results older than the ones shown are dropped.
    @discardableResult
    private func recordDRC(_ result: Result<[RuleViolation], EDAEngineError>, at checked: Int) -> Bool {
        switch result {
        case .success(let violations):
            guard checked >= drcRevision else { return false }
            drcResults = violations
            drcRevision = checked
            return true
        case .failure(let error):
            present(error, title: "Design rule check failed")
            return false
        }
    }

    /// Opens the Design Checks workspace on the given tab.
    func showChecks(_ mode: ChecksMode) {
        checksMode = mode
        workspace = .checks
    }

    // The DRC runs off the main thread, one at a time: a request while one runs asks for one more run after it (the
    // design changed meanwhile), so a burst of edits costs at most two checks. `drcEpoch` changes when the results are
    // cleared (new design, stack-up change); a check started before that is dropped.
    private(set) var drcTask: Task<Void, Never>?
    private var drcAgain = false
    private var drcAnnounce = false
    private var drcEpoch = 0

    /// Runs the design rule check off the main thread and shows its results (the status line says how it went unless
    /// `announce` is false: the edit's own message stays). `drcSettled()` waits for it.
    func runDRC(announce: Bool = true) {
        guard !isBusy else { return }
        drcAnnounce = drcAnnounce || announce
        if drcTask != nil {
            drcAgain = true
            return
        }
        let engine = self.engine
        let checked = revision
        let epoch = drcEpoch
        drcTask = Task { @MainActor [weak self] in
            let result = await Task.detached(priority: .userInitiated) { engine.runDRCChecked() }.value
            self?.drcFinished(result, at: checked, epoch: epoch)
        }
    }

    private func drcFinished(_ result: Result<[RuleViolation], EDAEngineError>, at checked: Int, epoch: Int) {
        drcTask = nil
        let again = drcAgain
        drcAgain = false
        if epoch == drcEpoch, recordDRC(result, at: checked), !again, drcAnnounce {
            let errors = drcResults.filter { $0.severity == .error }.count
            statusMessage = errors == 0 ? "DRC passed" : "DRC: \(errors) error(s)"
        }
        if again { runDRC(announce: false) }  // keeps a pending announcement for that run
        if drcTask == nil { drcAnnounce = false }
    }

    /// Waits until the DRC started by `runDRC` (and any run it queued) has finished and its results are shown.
    func drcSettled() async {
        while let task = drcTask { await task.value }
    }

    /// Clears the DRC results; a check still running is dropped when it finishes.
    private func clearDRC() {
        drcEpoch &+= 1
        drcResults = []
    }

    func moveFootprint(_ id: Int, to point: CGPoint) {
        moveFootprints([(id, point)])
    }

    /// Moves several footprints as one undo step (snapped to the 0.25 mm placement grid unless `snap` is false); the ends
    /// of their tracks come along unless `withTracks` is false.
    func moveFootprints(_ moves: [(id: Int, point: CGPoint)], withTracks: Bool = true, snap: Bool = true) {
        guard !isBusy, !moves.isEmpty else { return }
        performChecked(moves.count == 1 ? "Moved footprint" : "Moved \(moves.count) footprints", invalidatesAnalysis: false,
                       failureMessage: "Footprints could not be moved there") { engine in
            moves.reduce(false) { moved, move in
                let to = snap ? CGPoint(x: (move.point.x / 0.25).rounded() * 0.25, y: (move.point.y / 0.25).rounded() * 0.25)
                    : move.point
                return (withTracks ? engine.moveFootprintWithTracks(move.id, to: to) : engine.moveFootprint(move.id, to: to))
                    || moved
            }
        }
    }

    enum FootprintAlignment { case left, right, top, bottom, centreX, centreY, distributeX, distributeY }

    /// Lines the selected footprints' courtyards up on the outermost one's edge (or centre line), or spaces them
    /// evenly between the outermost two. One undo step; their tracks come along.
    func alignFootprints(_ how: FootprintAlignment) {
        let boxes = snapshot.courtyards.filter { selection.contains($0.component) }
        guard boxes.count >= (how == .distributeX || how == .distributeY ? 3 : 2) else {
            statusMessage = "Align: select two or more footprints (three to distribute)"
            return
        }
        let all = boxes.reduce(CGRect.null) { $0.union($1.rect) }
        var shift: [Int: CGSize] = [:]
        func spread(_ key: (CGRect) -> CGFloat, _ size: (CGRect) -> CGFloat, _ make: (CGFloat) -> CGSize) {
            let sorted = boxes.sorted { key($0.rect) < key($1.rect) }
            let gap = (sorted.map { size($0.rect) }.reduce(0, -) + size(all)) / CGFloat(sorted.count - 1)
            var at = key(sorted[0].rect)
            for box in sorted {
                shift[box.component] = make(at - key(box.rect))
                at += size(box.rect) + gap
            }
        }
        switch how {
        case .left: boxes.forEach { shift[$0.component] = CGSize(width: all.minX - $0.rect.minX, height: 0) }
        case .right: boxes.forEach { shift[$0.component] = CGSize(width: all.maxX - $0.rect.maxX, height: 0) }
        case .top: boxes.forEach { shift[$0.component] = CGSize(width: 0, height: all.minY - $0.rect.minY) }
        case .bottom: boxes.forEach { shift[$0.component] = CGSize(width: 0, height: all.maxY - $0.rect.maxY) }
        case .centreX: boxes.forEach { shift[$0.component] = CGSize(width: all.midX - $0.rect.midX, height: 0) }
        case .centreY: boxes.forEach { shift[$0.component] = CGSize(width: 0, height: all.midY - $0.rect.midY) }
        case .distributeX: spread(\.minX, \.width) { CGSize(width: $0, height: 0) }
        case .distributeY: spread(\.minY, \.height) { CGSize(width: 0, height: $0) }
        }
        let moves = shift.compactMap { entry -> (id: Int, point: CGPoint)? in
            guard let c = snapshot.component(entry.key), entry.value != .zero else { return nil }
            return (entry.key, CGPoint(x: c.pcb.x + entry.value.width, y: c.pcb.y + entry.value.height))
        }
        if moves.isEmpty { statusMessage = "Align: the footprints are already lined up" } else { moveFootprints(moves, snap: false) }
    }

    func rotateFootprints() {
        let ids = selection
        guard !isBusy, !ids.isEmpty else { return }
        perform("Rotated footprint", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateFootprint($0) } }
    }

    /// Embeds the selected resistors / capacitors on inner layer `layer` (0 = surface parts). Undoable.
    func setEmbedded(layer: Int) {
        let ids = selection
        guard !isBusy, !ids.isEmpty else { return }
        performChecked(layer > 0 ? "Embedded passive on layer \(layer + 1)" : "Surface-mounted part",
                       invalidatesAnalysis: false, failureMessage: "Only resistors and capacitors can be embedded") { engine in
            ids.allSatisfy { engine.setEmbedded($0, layer: layer) }
        }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Locks (or unlocks) the selected footprints so Auto Place keeps them. Undoable.
    func setFootprintsLocked(_ locked: Bool) {
        let ids = selection
        guard !isBusy, !ids.isEmpty else { return }
        perform(locked ? "Locked footprint" : "Unlocked footprint", invalidatesAnalysis: false) { engine in
            ids.forEach { engine.lockFootprint($0, locked) }
        }
    }

    func flipFootprints() {
        let ids = selection
        guard !isBusy, !ids.isEmpty else { return }
        perform("Flipped footprint", invalidatesAnalysis: false) { engine in ids.forEach { engine.flipFootprint($0) } }
    }

    /// 1 (single-sided), 2, 4 or 6 copper layers. Tracks on removed layers are deleted.
    func setLayerCount(_ layers: Int) {
        guard layers != snapshot.board.layerCount else { return }
        perform("\(layers)-layer stack-up", invalidatesAnalysis: false) { $0.setLayerCount(layers) }
        routeStats = nil
        clearDRC()
    }

    /// Selects the project's industry profile: its design-rule preset, altitude class and component derating.
    func setIndustry(_ profile: IndustryProfile) {
        guard profile.id != snapshot.industry else { return }
        perform("Industry: \(profile.name)", invalidatesAnalysis: false) { $0.setIndustry(profile.id) }
        validationResults = []
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    /// Robot platform ("" = not a robot): turns on the 7-segment robotics checks. Undoable.
    func setRobotPlatform(_ id: String) {
        guard id != snapshot.robotPlatform else { return }
        perform(id.isEmpty ? "Not a robot" : "Robot platform: \(id)", invalidatesAnalysis: false) { $0.setRobotPlatform(id) }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func robotSegments() -> RobotSegmentsReport { engine.robotSegments() }

    /// Adds every part of a robot platform's production kit to the project library (parts already there are kept).
    /// Returns how many were added.
    @discardableResult
    func addRobotKitToLibrary(_ platform: String) -> Int {
        guard let kit = robotSegments().platforms.first(where: { $0.id == platform })?.kit else { return 0 }
        var added = 0
        for name in Set(kit.flatMap(\.parts)).sorted() {
            guard !snapshot.customParts.contains(where: { $0.name == name }),
                  let part = StandardLibrary.parts.first(where: { $0.spec.name == name }) else { continue }
            if addStandardPartToLibrary(part) != nil { added += 1 }
        }
        return added
    }

    /// Automotive ECU type ("" = none): turns on the 6-segment ECU checks. Undoable.
    func setEcuType(_ id: String) {
        guard id != snapshot.ecuType else { return }
        perform(id.isEmpty ? "No ECU type" : "ECU type: \(id)", invalidatesAnalysis: false) { $0.setEcuType(id) }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func ecuSegments() -> RobotSegmentsReport { engine.ecuSegments() }

    /// Aerospace mission ("" = none): turns on the 5-segment aerospace checks at full severity. Undoable.
    func setAerospaceMission(_ id: String) {
        guard id != snapshot.aerospaceMission else { return }
        perform(id.isEmpty ? "No aerospace mission" : "Aerospace mission: \(id)", invalidatesAnalysis: false) {
            $0.setAerospaceMission(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func aerospaceSegments() -> RobotSegmentsReport { engine.aerospaceSegments() }

    /// Naval platform ("" = none): turns on the 5-segment naval checks at full severity. Undoable.
    func setNavalPlatform(_ id: String) {
        guard id != snapshot.navalPlatform else { return }
        perform(id.isEmpty ? "No naval platform" : "Naval platform: \(id)", invalidatesAnalysis: false) {
            $0.setNavalPlatform(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func navalSegments() -> RobotSegmentsReport { engine.navalSegments() }

    /// Medical device class ("" = none): turns on the 4-segment medical checks at full severity. Undoable.
    func setMedicalClass(_ id: String) {
        guard id != snapshot.medicalClass else { return }
        perform(id.isEmpty ? "No medical class" : "Medical class: \(id)", invalidatesAnalysis: false) {
            $0.setMedicalClass(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func medicalSegments() -> RobotSegmentsReport { engine.medicalSegments() }

    /// Retail device class ("" = none): turns on the 4-segment retail / POS checks at full severity. Undoable.
    func setRetailDevice(_ id: String) {
        guard id != snapshot.retailDevice else { return }
        perform(id.isEmpty ? "No retail device" : "Retail device: \(id)", invalidatesAnalysis: false) {
            $0.setRetailDevice(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func retailSegments() -> RobotSegmentsReport { engine.retailSegments() }

    /// Home appliance type ("" = none): turns on the 4-segment appliance checks at full severity. Undoable.
    func setApplianceType(_ id: String) {
        guard id != snapshot.applianceType else { return }
        perform(id.isEmpty ? "No appliance type" : "Appliance type: \(id)", invalidatesAnalysis: false) {
            $0.setApplianceType(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func applianceSegments() -> RobotSegmentsReport { engine.applianceSegments() }

    /// Memory design type ("" = none): turns on the 5-segment memory checks at full severity. Undoable.
    func setMemoryDesign(_ id: String) {
        guard id != snapshot.memoryDesign else { return }
        perform(id.isEmpty ? "No memory design type" : "Memory design: \(id)", invalidatesAnalysis: false) {
            $0.setMemoryDesign(id)
        }
        verificationReport = nil
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func memorySegments() -> RobotSegmentsReport { engine.memorySegments() }

    /// The controller's own DDR layout limits (nil fields = the memory type's typical value). Undoable.
    func setMemoryLimits(_ limits: EDAEngine.MemoryLimits) {
        guard engine.memoryLimits()?.overrides != limits else { return }
        performChecked("DDR layout limits", invalidatesAnalysis: false) { $0.setMemoryLimits(limits) }
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    /// Lays an active tamper mesh over a secure element (two inner layers; the nets must each join two of its pins).
    /// Undoable; the mesh copper appears with the next Auto Route.
    func addTamperMesh(component ref: String, netA: String, netB: String) {
        var index: Int?
        perform("Tamper mesh over \(ref)", invalidatesAnalysis: false) {
            index = $0.addTamperMesh(component: ref, netA: netA, netB: netB)
        }
        if index == nil { alert = AlertItem(title: "Tamper mesh not added", message: "Choose a part and two mesh nets.") }
        else if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    func clearTamperMeshes() {
        guard !snapshot.tamperMeshes.isEmpty else { return }
        perform("Removed tamper meshes", invalidatesAnalysis: false) { $0.clearTamperMeshes() }
    }

    /// Isolation barrier spacing between galvanic domains (patient barrier: 8 mm = 2 × MOPP). Undoable; re-checks.
    func setIsolationGap(_ gap: Double) {
        guard abs(gap - snapshot.board.isolationGap) > 1e-9 else { return }
        performChecked(gap > 0 ? String(format: "Isolation barrier %.0f mm", gap) : "No isolation barrier",
                       invalidatesAnalysis: false) { $0.setIsolationGap(gap) }
        verificationReport = nil
        if !drcResults.isEmpty { runDRC() }
    }

    /// Board thickness and underfill / corner bonding (shock). Undoable.
    func setMechanical(thickness: Double? = nil, underfill: Bool? = nil) {
        let board = snapshot.board
        let t = thickness ?? board.thickness, u = underfill ?? board.underfill
        guard abs(t - board.thickness) > 1e-9 || u != board.underfill else { return }
        performChecked(u != board.underfill ? (u ? "Underfill heavy parts" : "No underfill") : "Board thickness",
                       invalidatesAnalysis: false) { $0.setMechanical(thickness: t, underfill: u) }
        verificationReport = nil
    }

    /// Stitches thermal vias at the selected parts' drain / tab pads (every power MOSFET when nothing is selected).
    func addThermalVias() {
        guard !isBusy else { return }
        let fets = snapshot.components.filter { $0.componentKind == .nmos || $0.value.uppercased().hasPrefix("IRF") }
        let targets = selection.isEmpty ? fets.map(\.id) : Array(selection)
        var added = 0
        perform("Added thermal vias", invalidatesAnalysis: false) { engine in
            for id in targets { added += engine.addThermalVias(id) }
        }
        statusMessage = added > 0 ? "Added \(added) thermal via\(added == 1 ? "" : "s")"
                                  : "No room for thermal vias (clearance, or vias inside pads need VIPPO)"
        if !drcResults.isEmpty { runDRC() }
    }

    /// Applies a design-rule preset (track/clearance/via design values and fabrication minimums).
    func applyRulePreset(_ preset: DesignRulePreset) {
        guard preset.name != snapshot.board.rulePreset else { return }
        perform("Design rules: \(preset.name)", invalidatesAnalysis: false) { $0.applyRulePreset(preset.name) }
        if !snapshot.pads.isEmpty { runDRC(announce: false) }
    }

    /// Board size and default track/clearance. Values the core would reject are reported instead of ignored.
    func setBoard(width: Double, height: Double, trackWidth: Double, clearance: Double) {
        guard !isBusy else { return }
        // 0 keeps the current track width / clearance (the layout agent only resizes the board).
        let trackWidth = trackWidth > 0 ? trackWidth : snapshot.board.trackWidth
        let clearance = clearance > 0 ? clearance : snapshot.board.clearance
        guard width > 5, height > 5, trackWidth > 0.05, clearance > 0.05 else {
            alert = AlertItem(title: "Board settings not applied",
                              message: "Width and height must be over 5 mm, track width and clearance over 0.05 mm.")
            return
        }
        let b = snapshot.board
        guard width != b.width || height != b.height || trackWidth != b.trackWidth || clearance != b.clearance else { return }
        perform("Updated board settings", invalidatesAnalysis: false) {
            $0.setBoard(width: width, height: height, trackWidth: trackWidth, clearance: clearance)
        }
    }

    // MARK: - AI plans

    @discardableResult
    func applyPlan(_ plan: DesignPlan, requirements: String?) -> PlanApplyReport {
        var report = PlanApplyReport()
        let previous = snapshot
        perform("Applied AI design “\(plan.title)”") { engine in
            report = DesignPlanCompiler.apply(plan, to: engine, previous: previous)
            if let requirements { engine.setRequirements(requirements) }
        }
        selection = []
        resetChecks()
        routeStats = nil
        fitToken &+= 1
        return report
    }

    var currentPlan: DesignPlan { DesignPlanCompiler.plan(from: snapshot) }

    private func resetChecks() {
        ercResults = []
        clearDRC()
        validationResults = []
        verificationReport = nil
    }

    // MARK: - Documents

    func newProject() {
        guard confirmDiscardChanges() else { return }
        perform("New project", recordUndo: false) { $0.reset() }
        undoStack.removeAll()
        redoStack.removeAll()
        documentURL = nil
        isDirty = false
        resetChecks()
        routeStats = nil
        workspace = startWorkspace
    }

    /// Loads one of the built-in reference designs (works fully offline, no AI involved).
    func loadExample(_ plan: DesignPlan) {
        guard confirmDiscardChanges() else { return }
        CrashReporter.note("Load example: \(plan.title)")
        // A fresh project: nothing (library parts, board, pours, placement) carries over from the previous design.
        perform("New project", recordUndo: false) { $0.reset() }
        applyPlan(plan, requirements: plan.summary)
        undoStack.removeAll()
        redoStack.removeAll()
        documentURL = nil
        isDirty = false  // an untouched example needs no save prompt
        statusMessage = "Loaded example “\(plan.title)”"
        workspace = .schematic
    }

    // MARK: - Project templates

    /// The user's project templates (Application Support/SiEDA/Templates/*.siedaproj), by name.
    static var templatesFolder: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SiEDA/Templates", isDirectory: true)
    }

    func templateURLs() -> [URL] {
        ((try? FileManager.default.contentsOfDirectory(at: Self.templatesFolder, includingPropertiesForKeys: nil)) ?? [])
            .filter { $0.pathExtension == "siedaproj" }.sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// Saves the open design (rules, stack-up, board, title block, parts) as a template for new projects.
    func saveAsTemplate() {
        let alert = NSAlert()
        alert.messageText = String(localized: "Save as Template")
        let field = NSTextField(string: documentURL?.deletingPathExtension().lastPathComponent ?? "My Template")
        field.frame = NSRect(x: 0, y: 0, width: 260, height: 24)
        alert.accessoryView = field
        alert.addButton(withTitle: String(localized: "Save"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        guard alert.runModal() == .alertFirstButtonReturn else { return }
        let name = field.stringValue.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: "/", with: "-")
        if !name.isEmpty { saveTemplate(named: name) }
    }

    @discardableResult
    func saveTemplate(named name: String) -> URL? {
        do {
            try FileManager.default.createDirectory(at: Self.templatesFolder, withIntermediateDirectories: true)
            let url = Self.templatesFolder.appendingPathComponent(name).appendingPathExtension("siedaproj")
            try engine.saveJSON().write(to: url, atomically: true, encoding: .utf8)
            statusMessage = "Saved template \(name)"
            return url
        } catch {
            present(error, title: "Could not save the template")
            return nil
        }
    }

    /// A new, untitled project started from a template.
    func newFromTemplate(_ url: URL) {
        guard confirmDiscardChanges() else { return }
        open(url: url)
        ProjectLock.release(url)
        documentURL = nil
        isDirty = false
        statusMessage = "New project from template \(url.deletingPathExtension().lastPathComponent)"
    }

    func openProject() {
        guard confirmDiscardChanges() else { return }
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.siedaProject, .json]
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        open(url: url)
    }

    func open(url: URL) {
        CrashReporter.note("Open \(url.lastPathComponent)")
        if !liveCollaboration, url != documentURL, let holder = ProjectLock.holder(of: url) {  // checked out on another Mac
            let alert = NSAlert()
            alert.messageText = String(localized: "This project is open elsewhere")
            alert.informativeText = String(format: String(localized: "%1$@ has had it open on %2$@ since %3$@. Edits made in both places will clash."),
                                           holder.user, holder.host, holder.since.formatted(date: .abbreviated, time: .shortened))
            alert.addButton(withTitle: String(localized: "Open Anyway"))
            alert.addButton(withTitle: String(localized: "Cancel"))
            guard alert.runModal() == .alertFirstButtonReturn else { return }
        }
        do {
            let json = try String(contentsOf: url, encoding: .utf8)
            try engine.load(json: json)
            if documentURL != url { ProjectLock.release(documentURL) }
            documentURL = url
            if !liveCollaboration { ProjectLock.take(url) }
            ProjectHooks.run("after-open", project: url)
            collaboration.documentChanged(url: url, json: json)
            undoStack.removeAll()
            redoStack.removeAll()
            isDirty = false
            selection = []
            selectedWire = nil
            dcResult = nil
            transientResult = nil
            acResult = nil
            dcSweepResult = nil
            monteCarloResult = nil
            noiseResult = nil
            paramSweepResult = nil
            fftResult = nil
            resetChecks()
            refresh()
            fitToken &+= 1
            workspace = snapshot.components.isEmpty ? startWorkspace : .schematic
            statusMessage = "Opened \(url.lastPathComponent)"
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
            let notes = engine.loadNotes()
            if !notes.isEmpty {  // a file from a newer SiEDA: say what would be lost before anyone saves over it
                let alert = NSAlert()
                alert.messageText = String(localized: "This file has settings this version cannot keep")
                alert.informativeText = notes.joined(separator: "\n\n")
                alert.runModal()
            }
        } catch {
            present(error, title: "Could not open project")
        }
    }

    @discardableResult
    func save() -> Bool {
        guard let url = documentURL else { return saveAs() }
        return write(to: url)
    }

    @discardableResult
    func saveAs() -> Bool {
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.siedaProject]
        panel.nameFieldStringValue = (snapshot.name.isEmpty ? "Untitled" : snapshot.name) + ".siedaproj"
        guard panel.runModal() == .OK, let url = panel.url else { return false }
        return write(to: url)
    }

    private func write(to url: URL, json saved: String? = nil) -> Bool {
        do {
            let json = saved ?? engine.saveJSON()
            try json.write(to: url, atomically: true, encoding: .utf8)
            if documentURL != url {
                ProjectLock.release(documentURL)
                if !liveCollaboration { ProjectLock.take(url) }
            }
            documentURL = url
            ProjectHooks.run("after-save", project: url)
            collaboration.documentChanged(url: url, json: json)
            isDirty = false
            statusMessage = "Saved \(url.lastPathComponent)"
            CrashReporter.note("Saved \(url.lastPathComponent)")
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
            return true
        } catch {
            present(error, title: "Could not save project")
            return false
        }
    }

    /// An AI client (live MCP endpoint) replaced the whole design in place — project_new / project_load_example,
    /// recorded as one undo step by `performChecked`: selections and check results of the old design go, views re-fit.
    func noteDesignReplaced() {
        selection = []
        selectedWire = nil
        selectedBus = nil
        resetChecks()
        routeStats = nil
        fitToken &+= 1
    }

    func setProjectName(_ name: String) {
        perform("Renamed project", invalidatesAnalysis: false) { $0.setName(name) }
    }

    /// Verifies the design, then writes the complete fabrication package into a folder: Gerbers for every copper layer,
    /// mask, paste (stencil) and silkscreen with designators, outline, drills, Gerber X2 job file, IPC-D-356A test
    /// netlist, assembly BOM / CPL / drawings, fab_notes.txt (the order sheet), the Gerber zip to upload, the project
    /// and the verification report. A failing design is only exported after confirmation.
    func exportFabricationPackage() async {
        guard let report = await runVerification() else {
            alert = AlertItem(title: "Export failed", message: "The design could not be verified, so nothing was exported.")
            return
        }
        if !report.passed {
            let alert = NSAlert()
            alert.alertStyle = .warning
            alert.messageText = "This design fails verification"
            let failing = report.stages.filter { $0.status == .fail }.map { "• \($0.title): \($0.summary)" }
            alert.informativeText = failing.joined(separator: "\n")
                + "\n\nBoards built from it are unlikely to work. Export the fabrication files anyway?"
            alert.addButton(withTitle: "Review Issues")
            alert.addButton(withTitle: "Export Anyway")
            if alert.runModal() == .alertFirstButtonReturn {
                showChecks(.verification)
                return
            }
        }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = "Export Here"
        panel.message = "Choose a folder for the fabrication package."
        guard panel.runModal() == .OK, let folder = panel.url else { return }
        let base = (documentURL?.deletingPathExtension().lastPathComponent ?? snapshot.name)
            .replacingOccurrences(of: "/", with: "-")
        let target = folder.appendingPathComponent("\(base)-fabrication", isDirectory: true)
        do {
            try FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
            // Gerbers (every layer, mask, paste, silkscreen), drills, job file, IPC netlist, assembly files, notes and
            // the upload zip all come from the core, the same package the command-line tool writes.
            let engine = self.engine
            let package = await runBusy("Writing the fabrication package…") { engine.writeFabricationPackage(to: target, base: base) }
            guard package.ok else {
                alert = AlertItem(title: "Export failed", message: package.error)
                return
            }
            try engine.saveJSON().write(to: target.appendingPathComponent("\(base).siedaproj"), atomically: true,
                                        encoding: .utf8)
            try report.markdown.write(to: target.appendingPathComponent("verification_report.md"), atomically: true,
                                      encoding: .utf8)
            lastFabricationPackage = package.files.map { target.appendingPathComponent($0) }
            statusMessage = "Exported the fabrication package (\(package.files.count + 2) files): upload the Gerber zip to the fab"
            let zip = package.files.first { $0.hasSuffix("-gerbers.zip") }.map { target.appendingPathComponent($0) }
            ProjectHooks.run("after-export", project: documentURL, output: target)
            NSWorkspace.shared.activateFileViewerSelecting([zip ?? target])
        } catch {
            present(error, title: "Export failed")
        }
    }

    /// Asks where to save, then writes the file off the main thread (STEP or 3D exports of a big board take a while).
    func export(_ format: ExportFormat) async {
        await exportFile(format.rawValue, name: format.fileName, title: format.displayName)
    }

    /// Writes one export (any `sieda_export_file` format, binary ones too, e.g. "glb") to a file the user picks.
    func exportFile(_ format: String, name: String, title: String) async {
        guard !isBusy else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let engine = self.engine, path = url.path
        guard await runBusy("Exporting \(title)…", { engine.exportFile(format, to: path) }) else {
            alert = AlertItem(title: "Export failed", message: "The core could not write \(title).")
            return
        }
        statusMessage = "Exported \(title)"
    }

    /// Files of the last fabrication package exported (for tests and the status line).
    var lastFabricationPackage: [URL] = []

    /// Set once the user agreed to close the window (saved or chose Don't Save), so quitting does not ask again.
    var closeConfirmed = false

    /// Returns false if the user cancels.
    func confirmDiscardChanges() -> Bool {
        guard isDirty, !snapshot.components.isEmpty else { return true }
        let alert = NSAlert()
        alert.messageText = "Save changes to “\(snapshot.name)”?"
        alert.informativeText = "Your changes will be lost if you don't save them."
        alert.addButton(withTitle: "Save")
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Don't Save")
        switch alert.runModal() {
        case .alertFirstButtonReturn: return save()
        case .alertSecondButtonReturn: return false
        default: return true
        }
    }
}

/// Tabs of the Design Checks workspace.
enum ChecksMode: String, CaseIterable, Identifiable {
    case verification = "Verification"
    case rules = "Rule Checks"
    var id: String { rawValue }
}

/// An edit run off the main thread (`performInBackground`, `performExternalEditInBackground`): the undo state taken
/// before it, the body's result and the timings recorded in Help → Performance….
private struct BackgroundEdit<T: Sendable>: Sendable {
    let before: String
    let value: T
    let stateMs: Double
    let engineMs: Double
}

/// What `runVerification` computes off the main thread: the report and the individual check panels' results.
private struct VerificationRun: Sendable {
    let report: VerificationReport?
    let erc: [RuleViolation]
    let validation: [RuleViolation]
    let drc: Result<[RuleViolation], EDAEngineError>?
}
