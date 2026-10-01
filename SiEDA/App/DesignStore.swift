import AppKit
import CoreGraphics
import Foundation
import UniformTypeIdentifiers

enum Workspace: String, CaseIterable, Identifiable {
    case promptStudio, schematic, library, pcb, threeD, simulation, checks

    var id: String { rawValue }

    /// Workspaces offered in the UI; the AI Prompt Studio is hidden when AI assistance is off.
    static func visible(aiEnabled: Bool) -> [Workspace] {
        aiEnabled ? allCases : allCases.filter { $0 != .promptStudio }
    }

    var title: String {
        switch self {
        case .promptStudio: return "AI Prompt Studio"
        case .schematic: return "Schematic"
        case .library: return "Component Library"
        case .pcb: return "PCB Layout"
        case .threeD: return "3D Viewer"
        case .simulation: return "Simulation"
        case .checks: return "Design Checks"
        }
    }

    var systemImage: String {
        switch self {
        case .promptStudio: return "sparkles.rectangle.stack"
        case .schematic: return "point.3.connected.trianglepath.dotted"
        case .library: return "books.vertical"
        case .pcb: return "square.grid.3x3.square"
        case .threeD: return "cube.transparent"
        case .simulation: return "waveform.path.ecg"
        case .checks: return "checkmark.seal"
        }
    }
}

struct AlertItem: Identifiable {
    let id = UUID()
    var title: String
    var message: String
}

extension UTType {
    static var siedaProject: UTType { UTType(filenameExtension: "siedaproj") ?? .json }
}

/// Single source of truth for the open design. Wraps the C++ engine and publishes snapshots to SwiftUI.
@MainActor
final class DesignStore: ObservableObject {
    let engine = EDAEngine()

    @Published private(set) var snapshot: DesignSnapshot = .empty
    @Published var workspace: Workspace = .promptStudio
    @Published var selection: Set<Int> = []
    @Published var selectedWire: Int?
    @Published var ercResults: [RuleViolation] = []
    @Published var drcResults: [RuleViolation] = []
    @Published var dcResult: DCResult?
    @Published var transientResult: TransientResult?
    @Published var routeStats: RouteStats?
    @Published var showDCOverlay = true
    /// Mirrors `AISettings.aiEnabled` so document actions pick the right start workspace.
    @Published var aiEnabled = true {
        didSet { if !aiEnabled && workspace == .promptStudio { workspace = .schematic } }
    }
    var startWorkspace: Workspace { aiEnabled ? .promptStudio : .schematic }
    /// Custom part the Component Library should open (set when jumping from the inspector).
    @Published var libraryFocusPartId: String?
    @Published private(set) var isBusy = false
    @Published private(set) var busyMessage = ""
    @Published var statusMessage = "Ready"
    @Published private(set) var documentURL: URL?
    @Published private(set) var isDirty = false
    @Published var alert: AlertItem?
    /// Incremented whenever geometry changes so the 3D view knows to rebuild its mesh.
    @Published private(set) var revision = 0

    private var undoStack: [String] = []
    private var redoStack: [String] = []
    private let undoLimit = 100

    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }
    var windowTitle: String {
        let base = documentURL?.deletingPathExtension().lastPathComponent ?? snapshot.name
        return isDirty ? base + " — Edited" : base
    }
    var selectedComponents: [SnapComponent] { snapshot.components.filter { selection.contains($0.id) } }

    init() {
        refresh()
    }

    // MARK: - Core plumbing

    func refresh() {
        if let snap = engine.snapshot() {
            snapshot = snap
        }
        selection = selection.filter { id in snapshot.components.contains { $0.id == id } }
        if let wire = selectedWire, !snapshot.wires.contains(where: { $0.id == wire }) { selectedWire = nil }
        revision &+= 1
    }

    /// Runs a mutating engine operation with undo support.
    func perform(_ actionName: String, recordUndo: Bool = true, invalidatesAnalysis: Bool = true,
                 _ body: (EDAEngine) -> Void) {
        if recordUndo {
            undoStack.append(engine.saveJSON())
            if undoStack.count > undoLimit { undoStack.removeFirst() }
            redoStack.removeAll()
        }
        body(engine)
        isDirty = true
        if invalidatesAnalysis {
            dcResult = nil
            transientResult = nil
        }
        refresh()
        statusMessage = actionName
    }

    func undo() {
        guard let state = undoStack.popLast() else { return }
        redoStack.append(engine.saveJSON())
        restore(state, message: "Undo")
    }

    func redo() {
        guard let state = redoStack.popLast() else { return }
        undoStack.append(engine.saveJSON())
        restore(state, message: "Redo")
    }

    private func restore(_ json: String, message: String) {
        do {
            try engine.load(json: json)
            isDirty = true
            dcResult = nil
            transientResult = nil
            refresh()
            statusMessage = message
        } catch {
            present(error, title: "Undo failed")
        }
    }

    private func runBusy<T: Sendable>(_ message: String, _ work: @escaping @Sendable () -> T) async -> T {
        isBusy = true
        busyMessage = message
        defer {
            isBusy = false
            busyMessage = ""
        }
        return await Task.detached(priority: .userInitiated) { work() }.value
    }

    func present(_ error: Error, title: String) {
        alert = AlertItem(title: title, message: error.localizedDescription)
    }

    // MARK: - Schematic editing

    @discardableResult
    func addComponent(_ kind: ComponentKind, at point: CGPoint) -> Int {
        var id = -1
        let snapped = SchematicAutoLayout.snap(point)
        perform("Placed \(kind.displayName)") { id = $0.addComponent(kind, at: snapped) }
        if id >= 0 { selection = [id] }
        return id
    }

    func moveComponents(_ ids: Set<Int>, by delta: CGSize) {
        guard !ids.isEmpty, delta != .zero else { return }
        let moves = snapshot.components.filter { ids.contains($0.id) }
        perform("Moved \(moves.count) part(s)", invalidatesAnalysis: false) { engine in
            for c in moves {
                engine.moveComponent(c.id, to: SchematicAutoLayout.snap(CGPoint(x: c.x + delta.width, y: c.y + delta.height)))
            }
        }
    }

    func rotateSelection() {
        let ids = selection
        guard !ids.isEmpty else { return }
        perform("Rotated", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateComponent($0) } }
    }

    func deleteSelection() {
        if let wire = selectedWire {
            perform("Deleted wire") { $0.removeWire(wire) }
            selectedWire = nil
            return
        }
        let ids = selection
        guard !ids.isEmpty else { return }
        perform("Deleted \(ids.count) part(s)") { engine in ids.forEach { engine.removeComponent($0) } }
        selection = []
    }

    func setValue(_ id: Int, _ value: String) {
        guard snapshot.component(id)?.value != value else { return }
        perform("Changed value") { $0.setValue(id, value) }
    }

    func setRef(_ id: Int, _ ref: String) {
        let trimmed = ref.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, snapshot.component(id)?.ref != trimmed else { return }
        if snapshot.component(ref: trimmed) != nil {
            alert = AlertItem(title: "Duplicate designator", message: "\(trimmed) is already used in this design.")
            return
        }
        perform("Renamed to \(trimmed)", invalidatesAnalysis: false) { $0.setRef(id, trimmed) }
    }

    func connect(_ a: PinAddress, _ b: PinAddress) {
        guard a != b else { return }
        perform("Connected wire") { $0.connect(a, b) }
    }

    func select(component id: Int?, extend: Bool = false) {
        selectedWire = nil
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
        perform(oldId == nil ? "Added \(spec.name) to the library" : "Updated \(spec.name)") { engine in
            do {
                let part = try engine.registerCustomPart(spec)
                if let oldId, oldId != part.id {
                    engine.replaceCustomPart(oldId, with: part.id)
                    engine.removeCustomPart(oldId)
                }
                result = part
            } catch {
                failure = error
            }
        }
        if let failure { present(failure, title: "Could not save the component") }
        return result
    }

    func deleteCustomPart(_ id: String) {
        var removed = false
        perform("Removed part from library", invalidatesAnalysis: false) { removed = $0.removeCustomPart(id) }
        if !removed {
            alert = AlertItem(title: "Part is in use",
                              message: "Delete the components that use this part from the schematic first.")
        }
    }

    @discardableResult
    func addCustomComponent(partId: String, at point: CGPoint) -> Int {
        var id = -1
        let snapped = SchematicAutoLayout.snap(point)
        let name = snapshot.customPart(partId)?.name ?? "part"
        perform("Placed \(name)") { id = $0.addCustomComponent(partId: partId, at: snapped) }
        if id >= 0 { selection = [id] }
        return id
    }

    // MARK: - Analysis

    func runERC() {
        ercResults = engine.runERC()
        let errors = ercResults.filter { $0.severity == .error }.count
        let warnings = ercResults.filter { $0.severity == .warning }.count
        statusMessage = "ERC: \(errors) error(s), \(warnings) warning(s)"
    }

    func simulateDC() async {
        let engine = self.engine
        let result = await runBusy("Solving DC operating point…") { engine.simulateDC() }
        dcResult = result
        statusMessage = result.converged
            ? "DC operating point converged in \(result.iterations) iterations"
            : "DC analysis failed: \(result.error)"
    }

    func simulateTransient(stop: Double, step: Double) async {
        let engine = self.engine
        let result = await runBusy("Running transient analysis…") { engine.simulateTransient(stop: stop, step: step) }
        transientResult = result
        statusMessage = result.ok ? "Transient analysis: \(result.time.count) points" : "Transient failed: \(result.error)"
    }

    // MARK: - PCB

    func autoPlace(all: Bool) {
        perform(all ? "Auto-placed all footprints" : "Placed new footprints", invalidatesAnalysis: false) {
            $0.autoPlace(all: all)
        }
    }

    func autoRoute() async {
        undoStack.append(self.engine.saveJSON())
        redoStack.removeAll()
        let engine = self.engine
        let stats = await runBusy("Autorouting…") { engine.autoRoute() }
        routeStats = stats
        isDirty = true
        refresh()
        drcResults = engine.runDRC()
        statusMessage = stats.failed == 0
            ? "Routed \(stats.routed)/\(stats.connections) connections, \(stats.vias) vias"
            : "Routed \(stats.routed)/\(stats.connections) — \(stats.failed) failed (\(stats.failedNets.joined(separator: ", ")))"
    }

    func clearRouting() {
        perform("Cleared routing", invalidatesAnalysis: false) { $0.clearRouting() }
        routeStats = nil
    }

    /// Resizes the board outline to the placed footprints plus `margin` millimetres.
    func fitBoard(margin: Double = 2.5) {
        perform("Fitted board to components", invalidatesAnalysis: false) { $0.fitBoard(margin: margin) }
    }

    func runDRC() {
        drcResults = engine.runDRC()
        let errors = drcResults.filter { $0.severity == .error }.count
        statusMessage = errors == 0 ? "DRC passed" : "DRC: \(errors) error(s)"
    }

    func moveFootprint(_ id: Int, to point: CGPoint) {
        let snapped = CGPoint(x: (point.x / 0.25).rounded() * 0.25, y: (point.y / 0.25).rounded() * 0.25)
        perform("Moved footprint", invalidatesAnalysis: false) { $0.moveFootprint(id, to: snapped) }
    }

    func rotateFootprints() {
        let ids = selection
        perform("Rotated footprint", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateFootprint($0) } }
    }

    func flipFootprints() {
        let ids = selection
        perform("Flipped footprint", invalidatesAnalysis: false) { engine in ids.forEach { engine.flipFootprint($0) } }
    }

    /// 1 (single-sided), 2, 4 or 6 copper layers. Tracks on removed layers are deleted.
    func setLayerCount(_ layers: Int) {
        guard layers != snapshot.board.layerCount else { return }
        perform("\(layers)-layer stack-up", invalidatesAnalysis: false) { $0.setLayerCount(layers) }
        routeStats = nil
        drcResults = []
    }

    func setBoard(width: Double, height: Double, trackWidth: Double, clearance: Double) {
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
        ercResults = []
        drcResults = []
        routeStats = nil
        return report
    }

    var currentPlan: DesignPlan { DesignPlanCompiler.plan(from: snapshot) }

    // MARK: - Documents

    func newProject() {
        guard confirmDiscardChanges() else { return }
        perform("New project", recordUndo: false) { $0.clear(); $0.setName("Untitled"); $0.setRequirements("") }
        undoStack.removeAll()
        redoStack.removeAll()
        documentURL = nil
        isDirty = false
        ercResults = []
        drcResults = []
        routeStats = nil
        workspace = startWorkspace
    }

    /// Loads one of the built-in reference designs (works fully offline, no AI involved).
    func loadExample(_ plan: DesignPlan) {
        guard confirmDiscardChanges() else { return }
        applyPlan(plan, requirements: plan.summary)
        undoStack.removeAll()
        redoStack.removeAll()
        documentURL = nil
        statusMessage = "Loaded example “\(plan.title)”"
        workspace = .schematic
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
        do {
            let json = try String(contentsOf: url, encoding: .utf8)
            try engine.load(json: json)
            documentURL = url
            undoStack.removeAll()
            redoStack.removeAll()
            isDirty = false
            dcResult = nil
            transientResult = nil
            ercResults = []
            drcResults = []
            refresh()
            workspace = snapshot.components.isEmpty ? startWorkspace : .schematic
            statusMessage = "Opened \(url.lastPathComponent)"
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
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

    private func write(to url: URL) -> Bool {
        do {
            try engine.saveJSON().write(to: url, atomically: true, encoding: .utf8)
            documentURL = url
            isDirty = false
            statusMessage = "Saved \(url.lastPathComponent)"
            NSDocumentController.shared.noteNewRecentDocumentURL(url)
            return true
        } catch {
            present(error, title: "Could not save project")
            return false
        }
    }

    func setProjectName(_ name: String) {
        perform("Renamed project", invalidatesAnalysis: false) { $0.setName(name) }
    }

    /// Writes Gerbers, drill, BOM, pick & place, netlist and 3D model into a folder.
    func exportFabricationPackage() {
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
            var written = 0
            for format in ExportFormat.fabricationPackage {
                guard let content = engine.export(format) else { continue }
                try content.write(to: target.appendingPathComponent(format.fileName), atomically: true, encoding: .utf8)
                written += 1
            }
            // Inner copper layers of multi-layer boards (L2 … Ln-1).
            let layers = snapshot.board.layerCount
            if layers > 2 {
                for layer in 2..<layers {
                    guard let content = engine.exportCopperLayer(layer) else { continue }
                    try content.write(to: target.appendingPathComponent("board-In\(layer - 1)_Cu.gbr"), atomically: true,
                                      encoding: .utf8)
                    written += 1
                }
            }
            try engine.saveJSON().write(to: target.appendingPathComponent("\(base).siedaproj"), atomically: true,
                                        encoding: .utf8)
            statusMessage = "Exported \(written) fabrication files to \(target.lastPathComponent)"
            NSWorkspace.shared.activateFileViewerSelecting([target])
        } catch {
            present(error, title: "Export failed")
        }
    }

    func export(_ format: ExportFormat) {
        guard let content = engine.export(format) else {
            alert = AlertItem(title: "Export failed", message: "The core could not produce \(format.displayName).")
            return
        }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = format.fileName
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try content.write(to: url, atomically: true, encoding: .utf8)
            statusMessage = "Exported \(format.displayName)"
        } catch {
            present(error, title: "Export failed")
        }
    }

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
