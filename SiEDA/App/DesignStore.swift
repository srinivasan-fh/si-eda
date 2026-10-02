import AppKit
import CoreGraphics
import Foundation
import UniformTypeIdentifiers

enum Workspace: String, CaseIterable, Identifiable {
    case promptStudio, schematic, pcb, threeD, simulation, checks, library

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
    @Published var workspace: Workspace = .promptStudio
    @Published var selection: Set<Int> = []
    @Published var selectedWire: Int?
    @Published var ercResults: [RuleViolation] = []
    @Published var drcResults: [RuleViolation] = []
    @Published var validationResults: [RuleViolation] = []
    @Published private(set) var verificationReport: VerificationReport?
    /// `revision` the verification report was computed at; any later edit makes it stale.
    @Published private(set) var verifiedRevision = -1
    @Published var dcResult: DCResult?
    @Published var transientResult: TransientResult?
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
    @Published private(set) var busyMessage = ""
    @Published var statusMessage = "Ready"
    @Published private(set) var documentURL: URL?
    @Published private(set) var isDirty = false
    @Published var alert: AlertItem?
    /// Incremented whenever geometry changes so the 3D view knows to rebuild its mesh.
    @Published private(set) var revision = 0
    /// Incremented when a whole new design arrives (AI plan, open, example, re-placement) so editors re-fit.
    @Published private(set) var fitToken = 0
    /// Bumped to ask the window to show the inspector (a click on a part in a results list).
    @Published private(set) var inspectorRevealToken = 0
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
    }

    // MARK: - Core plumbing

    /// One alert per run of failed snapshot reads.
    private var snapshotErrorShown = false

    func refresh() {
        switch engine.snapshotChecked() {
        case .success(let snap):
            snapshot = snap
            snapshotErrorShown = false
        case .failure(let error):
            // Keep showing the last good state, but never silently: edits would otherwise look like they did nothing.
            NSLog("SiEDA: %@", error.localizedDescription)
            statusMessage = "Display not updated — \(error.localizedDescription)"
            if !snapshotErrorShown {
                snapshotErrorShown = true
                alert = AlertItem(title: "The design view could not be updated", message: error.localizedDescription)
            }
        }
        selection = selection.filter { id in snapshot.components.contains { $0.id == id } }
        if let wire = selectedWire, !snapshot.wires.contains(where: { $0.id == wire }) { selectedWire = nil }
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
        let before = recordUndo ? engine.saveJSON() : nil
        guard body(engine) else {
            statusMessage = failureMessage ?? "\(actionName): not possible"
            return false
        }
        if let before {
            undoStack.append(before)
            if undoStack.count > undoLimit { undoStack.removeFirst() }
            redoStack.removeAll()
        }
        isDirty = true
        if invalidatesAnalysis {
            dcResult = nil
            transientResult = nil
        }
        refresh()
        statusMessage = actionName
        return true
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
    func addComponent(_ kind: ComponentKind, at point: CGPoint, rotation: Int = 0) -> Int {
        var id = -1
        let snapped = SchematicAutoLayout.snap(point)
        perform("Placed \(kind.displayName)") { id = $0.addComponent(kind, at: snapped, rotation: rotation) }
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

    func setRef(_ id: Int, _ ref: String) {
        let trimmed = ref.trimmingCharacters(in: .whitespaces)
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
                return .junction(SchematicAutoLayout.snap(p))
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
        let p = SchematicAutoLayout.snap(point)
        var junction: Int?
        performChecked("Bent wire", invalidatesAnalysis: false) { engine in
            junction = engine.splitWire(id, at: p)
            return junction != nil
        }
        if let junction { selection = [junction]; selectedWire = nil }
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
        let snapped = SchematicAutoLayout.snap(point)
        let name = snapshot.customPart(partId)?.name ?? "part"
        perform("Placed \(name)") { id = $0.addCustomComponent(partId: partId, at: snapped, rotation: rotation) }
        if id >= 0 { selection = [id] }
        return id
    }

    /// Adds a built-in standard part to the project library (if needed) and returns its part id.
    @discardableResult
    func addStandardPartToLibrary(_ part: StandardPart) -> String? {
        if let existing = snapshot.customParts.first(where: { $0.name == part.spec.name }) { return existing.id }
        return saveCustomPart(part.spec)?.id
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
        let report = await runBusy("Verifying design…") { engine.runVerification() }
        verificationReport = report
        verifiedRevision = revision
        ercResults = engine.runERC()
        validationResults = engine.runCircuitValidation()
        if !snapshot.pads.isEmpty { recordDRC(engine.runDRCChecked()) }
        if let report {
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
        let result = await runBusy("Solving DC operating point…") { engine.simulateDC() }
        dcResult = result
        statusMessage = result.converged
            ? "DC operating point converged in \(result.iterations) iterations"
            : "DC analysis failed: \(result.error)"
    }

    func simulateTransient(stop: Double, step: Double) async {
        guard !isBusy else { return }  // one analysis at a time: overlapping runs would reset isBusy early
        let engine = self.engine
        let result = await runBusy("Running transient analysis…") { engine.simulateTransient(stop: stop, step: step) }
        transientResult = result
        statusMessage = result.ok ? "Transient analysis: \(result.time.count) points" : "Transient failed: \(result.error)"
    }

    // MARK: - PCB

    func autoPlace(all: Bool) {
        guard !isBusy else { return }
        perform(all ? "Auto-placed all footprints" : "Placed new footprints", invalidatesAnalysis: false) {
            $0.autoPlace(all: all)
        }
        if all { fitToken &+= 1 }
    }

    func autoRoute() async {
        guard !isBusy else { return }
        let before = engine.saveJSON()
        let engine = self.engine
        let result = await runBusy("Autorouting…") { engine.autoRouteChecked() }
        guard case .success(let stats) = result else {
            if case .failure(let error) = result { present(error, title: "Autorouting failed") }
            refresh()
            return
        }
        undoStack.append(before)
        if undoStack.count > undoLimit { undoStack.removeFirst() }
        redoStack.removeAll()
        isDirty = true
        refresh()
        routeStats = stats
        routeRevision = revision
        recordDRC(engine.runDRCChecked())
        statusMessage = stats.failed == 0
            ? "Routed \(stats.routed)/\(stats.connections) connections, \(stats.vias) vias"
            : "Routed \(stats.routed)/\(stats.connections) — \(stats.failed) failed (\(stats.failedNets.joined(separator: ", ")))"
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
    @discardableResult
    private func recordDRC(_ result: Result<[RuleViolation], EDAEngineError>) -> Bool {
        switch result {
        case .success(let violations):
            drcResults = violations
            drcRevision = revision
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

    func runDRC() {
        guard !isBusy else { return }
        guard recordDRC(engine.runDRCChecked()) else { return }
        let errors = drcResults.filter { $0.severity == .error }.count
        statusMessage = errors == 0 ? "DRC passed" : "DRC: \(errors) error(s)"
    }

    func moveFootprint(_ id: Int, to point: CGPoint) {
        moveFootprints([(id, point)])
    }

    /// Moves several footprints as one undo step (snapped to the 0.25 mm placement grid).
    func moveFootprints(_ moves: [(id: Int, point: CGPoint)]) {
        guard !isBusy, !moves.isEmpty else { return }
        performChecked(moves.count == 1 ? "Moved footprint" : "Moved \(moves.count) footprints", invalidatesAnalysis: false,
                       failureMessage: "Footprints could not be moved there") { engine in
            moves.reduce(false) { moved, move in
                let snapped = CGPoint(x: (move.point.x / 0.25).rounded() * 0.25, y: (move.point.y / 0.25).rounded() * 0.25)
                return engine.moveFootprint(move.id, to: snapped) || moved
            }
        }
    }

    func rotateFootprints() {
        let ids = selection
        guard !isBusy, !ids.isEmpty else { return }
        perform("Rotated footprint", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateFootprint($0) } }
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
        drcResults = []
    }

    /// Selects the project's industry profile: its design-rule preset, altitude class and component derating.
    func setIndustry(_ profile: IndustryProfile) {
        guard profile.id != snapshot.industry else { return }
        perform("Industry: \(profile.name)", invalidatesAnalysis: false) { $0.setIndustry(profile.id) }
        validationResults = []
        verificationReport = nil
        if !snapshot.pads.isEmpty { recordDRC(engine.runDRCChecked()) }
    }

    /// Applies a design-rule preset (track/clearance/via design values and fabrication minimums).
    func applyRulePreset(_ preset: DesignRulePreset) {
        guard preset.name != snapshot.board.rulePreset else { return }
        perform("Design rules: \(preset.name)", invalidatesAnalysis: false) { $0.applyRulePreset(preset.name) }
        if !snapshot.pads.isEmpty { recordDRC(engine.runDRCChecked()) }
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
        drcResults = []
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
            selection = []
            selectedWire = nil
            dcResult = nil
            transientResult = nil
            resetChecks()
            refresh()
            fitToken &+= 1
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

    /// Verifies the design, then writes Gerbers, drill, BOM, pick & place, netlist, 3D model and the
    /// verification report into a folder. A failing design is only exported after confirmation.
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
            var written = 0
            for format in ExportFormat.fabricationPackage {
                if format == .drillNPTH, snapshot.board.holes.isEmpty { continue }
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
            try report.markdown.write(to: target.appendingPathComponent("verification_report.md"), atomically: true,
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
