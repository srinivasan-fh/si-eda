import AppKit
import Foundation
import UniformTypeIdentifiers

/// Schematic productivity: align / distribute, copy and paste (smart paste: a paste array with designators numbered
/// on and labels counted up), cross-probing between the schematic and the board, back-annotation (ECO review),
/// sheet templates and the PDF of every sheet.
extension DesignStore {
    /// A schematic clipboard is the core's "sieda.schematic-clip/1" JSON, kept on the pasteboard as text.
    static func isSchematicClip(_ text: String) -> Bool { text.contains("\"sieda.schematic-clip/1\"") }

    // MARK: - Align and distribute

    /// "left", "right", "top", "bottom", "centerX", "centerY", "distributeX", "distributeY".
    func align(_ mode: String, byOutline: Bool = false) {
        let ids = Array(selection)
        guard ids.count >= 2 else { return }
        performChecked("Align \(mode)", failureMessage: mode.hasPrefix("distribute")
                       ? "Select at least three parts to distribute" : "The selection is already aligned") {
            (byOutline ? $0.alignOutlines(ids, mode: mode) : $0.alignComponents(ids, mode: mode)) > 0
        }
    }

    /// Centres a sheet's fixed template frame on its drawing again.
    func centerSheetFrame(_ id: Int) {
        guard let sheet = snapshot.sheet(id), let size = sheet.size, !size.isEmpty else { return }
        performChecked("\(sheet.name): frame centred on the drawing", invalidatesAnalysis: false) {
            $0.setSheetFrame(id, fixed: false) && $0.setSheetSize(id, size: size)
        }
    }

    // MARK: - Mirror, duplicate, lock, wire clean-up, smart selection

    /// Parts locked on the schematic.
    var lockedParts: Set<Int> { Set(snapshot.components.filter(\.isLocked).map(\.id)) }

    /// The selection, else the part under the pointer (hover + shortcut acts on it without selecting first).
    func targets(hovered: Int?) -> Set<Int> {
        if !selection.isEmpty { return selection }
        if let hovered { return [hovered] }
        return []
    }

    /// Mirrors the parts left-right (vertical: top-bottom).
    func mirror(_ ids: Set<Int>, vertical: Bool) {
        guard !ids.isEmpty else { return }
        perform(vertical ? "Mirrored vertically" : "Mirrored horizontally", invalidatesAnalysis: false) { engine in
            ids.forEach { engine.mirrorComponent($0, vertical: vertical) }
        }
    }

    func rotate(_ ids: Set<Int>) {
        guard !ids.isEmpty else { return }
        perform("Rotated", invalidatesAnalysis: false) { engine in ids.forEach { engine.rotateComponent($0) } }
    }

    func delete(_ ids: Set<Int>) {
        guard !ids.isEmpty else { return }
        perform("Deleted \(ids.count) part(s)") { engine in ids.forEach { engine.removeComponent($0) } }
        selection.subtract(ids)
    }

    /// ⌘D: a copy of the selection (with the wires between its parts) beside it, with new designators.
    func duplicateSelection() {
        guard let clip = selectionClip() else { return }
        paste(clip: clip)
    }

    /// Pastes the clipboard with its top-left part at `world` (the pointer).
    func paste(at world: CGPoint) {
        guard let clip = NSPasteboard.general.string(forType: .string), Self.isSchematicClip(clip),
              let data = clip.data(using: .utf8),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let parts = json["components"] as? [[String: Any]], !parts.isEmpty else { return }
        let xs = parts.compactMap { ($0["x"] as? NSNumber)?.doubleValue }, ys = parts.compactMap { ($0["y"] as? NSNumber)?.doubleValue }
        guard let x0 = xs.min(), let y0 = ys.min() else { return }
        let target = SchematicGrid.snap(world)
        var pasted: [Int] = []
        performChecked("Paste", failureMessage: "Nothing to paste here") {
            pasted = $0.pasteComponents(clip, offset: CGSize(width: target.x - x0, height: target.y - y0), count: 1, step: .zero,
                                        labelIncrement: 0)
            return !pasted.isEmpty
        }
        if !pasted.isEmpty { selection = Set(pasted) }
    }

    func setLocked(_ ids: Set<Int>, _ locked: Bool) {
        guard !ids.isEmpty else { return }
        performChecked(locked ? "Locked \(ids.count) part(s)" : "Unlocked \(ids.count) part(s)", invalidatesAnalysis: false) { engine in
            ids.reduce(false) { engine.setLocked($1, locked) || $0 }
        }
    }

    /// Net labels drawn as power symbols (a bar on a stem) or as flags.
    func setPowerStyle(_ ids: Set<Int>, _ power: Bool) {
        let labels = ids.filter { snapshot.component($0)?.componentKind == .netLabel }
        guard !labels.isEmpty else { return }
        performChecked(power ? "Power symbol" : "Net label flag", invalidatesAnalysis: false) { engine in
            labels.reduce(false) { engine.setLabelPower($1, power) || $0 }
        }
    }

    /// Removes wire bends lying on straight runs of the shown sheet.
    func cleanupWires() {
        let sheet = snapshot.activeSheet
        var removed = 0
        performChecked("Cleaned up wires", invalidatesAnalysis: false, failureMessage: "No wire bends to clean up") {
            removed = $0.cleanupWires(sheet: sheet)
            return removed > 0
        }
        if removed > 0 { statusMessage = "Removed \(removed) needless wire bend(s)" }
    }

    /// One value for several parts at once (one undo step).
    func setValue(of ids: Set<Int>, to value: String) {
        let v = value.trimmingCharacters(in: .whitespaces)
        guard !v.isEmpty, !ids.isEmpty else { return }
        performChecked("Value \(v) for \(ids.count) part(s)", failureMessage: "No value changed") { engine in
            ids.reduce(false) { engine.setValue($1, v) || $0 }
        }
    }

    /// One package for several parts of a kind at once (one undo step).
    func setPackage(of ids: Set<Int>, to package: String) {
        guard !ids.isEmpty else { return }
        performChecked("Package \(package) for \(ids.count) part(s)", failureMessage: "No package changed") { engine in
            ids.reduce(false) { engine.setPackage($1, package) || $0 }
        }
    }

    // MARK: - Notes and drawings on sheets

    /// Adds a note ("text", one point) or a line / arrow / section rectangle (two points) on the shown sheet.
    func addSheetGraphic(kind: String, points: [CGPoint], text: String = "") {
        var id: Int?
        performChecked(kind == "text" ? "Added note" : "Added \(kind)", invalidatesAnalysis: false, failureMessage: "Nothing to draw") {
            id = $0.addSheetGraphic(kind: kind, points: points.map { SchematicGrid.snap($0) }, text: text)
            return id != nil
        }
        if let id {
            select(component: nil)
            selectedGraphic = id
        }
    }

    func moveSheetGraphic(_ id: Int, by delta: CGSize) {
        guard delta != .zero else { return }
        perform("Moved drawing", invalidatesAnalysis: false) { $0.moveSheetGraphic(id, by: delta) }
    }

    func setSheetGraphicText(_ id: Int, _ text: String) {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        performChecked("Edited note", invalidatesAnalysis: false) { $0.setSheetGraphicText(id, t) }
    }

    /// Every part on the shown sheet.
    func selectAllOnSheet() {
        selectedWire = nil
        selectedBus = nil
        selection = Set(sheetSnapshot.components.filter { !$0.componentKind.isVirtual || $0.componentKind == .netLabel }.map(\.id))
    }

    /// Parts with the same value (and kind) as the selected ones.
    func selectSameValue() {
        let picked = sheetSnapshot.components.filter { selection.contains($0.id) }
        let keys = Set(picked.map { "\($0.kind)|\($0.value)" })
        guard !keys.isEmpty else { return }
        selection = Set(sheetSnapshot.components.filter { keys.contains("\($0.kind)|\($0.value)") }.map(\.id))
    }

    /// The net of the selected wire, else of the selected parts' pins.
    var highlightedNets: Set<Int> {
        if let id = selectedWire, let w = snapshot.wires.first(where: { $0.id == id }), w.net >= 0 { return [w.net] }
        return []
    }

    /// Every part on the selected wire's net (or on the nets of the selected parts' pins).
    func selectConnected() {
        var nets = highlightedNets
        if nets.isEmpty {
            for c in sheetSnapshot.components where selection.contains(c.id) {
                for p in c.pins where p.net >= 0 { nets.insert(p.net) }
            }
        }
        guard !nets.isEmpty else { return }
        selectedWire = nil
        selection = Set(sheetSnapshot.components.filter { c in c.pins.contains { nets.contains($0.net) } }.map(\.id))
    }

    // MARK: - Copy and paste

    var canPaste: Bool { NSPasteboard.general.string(forType: .string).map(Self.isSchematicClip) ?? false }

    /// The clipboard text of the selection (nil when nothing is selected).
    func selectionClip() -> String? {
        let ids = Array(selection)
        let buses = selectedBus.map { [$0] } ?? []
        return ids.isEmpty && buses.isEmpty ? nil : engine.copyComponents(ids, buses: buses)
    }

    /// Copies the selected parts and the wires between them.
    func copySelection() {
        guard let clip = selectionClip() else { return }
        let board = NSPasteboard.general
        board.clearContents()
        board.setString(clip, forType: .string)
        statusMessage = "Copied \(selection.count) part(s)"
    }

    func cutSelection() {
        copySelection()
        deleteSelection()
    }

    /// Pastes the clipboard on the shown sheet, one grid step down and right of the copied parts, or `count` copies
    /// each `step` further (a paste array), with net label numbers counted up by `labelIncrement` per copy.
    func paste(count: Int = 1, step: CGSize = CGSize(width: 0, height: 0), labelIncrement: Int = 0, clip given: String? = nil) {
        guard let clip = given ?? NSPasteboard.general.string(forType: .string), Self.isSchematicClip(clip) else { return }
        let first = step == .zero ? CGSize(width: 40, height: 40) : step
        var pasted: [Int] = []
        performChecked(count > 1 ? "Paste array ×\(count)" : "Paste", failureMessage: "Nothing to paste here") {
            pasted = $0.pasteComponents(clip, offset: first, count: max(1, count), step: step, labelIncrement: labelIncrement)
            return !pasted.isEmpty
        }
        if !pasted.isEmpty { selection = Set(pasted) }
    }

    // MARK: - Cross-probing between the schematic and the board

    /// Shows a part on the board: the PCB workspace, the part selected and zoomed (a unit shows its package).
    func showOnPCB(_ id: Int) {
        guard let c = snapshot.component(id) else { return }
        let part = c.unitOf ?? c.id
        workspace = .pcb
        select(component: part)
        requestView(.fitSelection)
    }

    /// Shows a part on its schematic sheet: the schematic workspace, the sheet, the part (its first unit) selected.
    func showInSchematic(_ id: Int) {
        guard let c = snapshot.component(id) else { return }
        let target = c.isUnitPackage ? (snapshot.components.first { $0.unitOf == c.id } ?? c) : c
        workspace = .schematic
        crossProbe(component: target.id, sheet: target.sheetId)
    }

    // MARK: - Back-annotation (board → schematic ECO)

    func ecoFromBoard(byColumns: Bool = false) -> [EcoChangeInfo] { engine.reannotateFromBoard(byColumns: byColumns) }

    /// Asks for a WAS / IS file and reads its changes (nothing applied yet).
    func ecoFromWasIsFile() -> [EcoChangeInfo]? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [UTType(filenameExtension: "was") ?? .plainText, .plainText]
        panel.allowsOtherFileTypes = true
        guard panel.runModal() == .OK, let url = panel.url,
              let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return engine.ecoFromWasIs(text)
    }

    /// Applies the reviewed changes as one undo step.
    func applyEco(_ changes: [EcoChangeInfo]) {
        let chosen = changes.filter(\.applicable)
        guard !chosen.isEmpty else { return }
        var applied = 0
        performChecked("Back-annotated \(chosen.count) change(s)", failureMessage: "None of the changes could be applied") {
            applied = $0.applyEco(chosen)
            return applied > 0
        }
        if applied > 0 { statusMessage = "Applied \(applied) change(s) from the board" }
    }

    // MARK: - Sheet templates and PDF

    func setSheetSize(_ id: Int, size: String) {
        guard let sheet = snapshot.sheet(id), (sheet.size ?? "") != size else { return }
        performChecked("\(sheet.name): sheet \(size.isEmpty ? "auto" : size)", invalidatesAnalysis: false) {
            $0.setSheetSize(id, size: size)
        }
    }

    // MARK: - Helper sheets and per-channel parameters

    /// Adds a helper sheet below a block (every channel gets a copy) and shows it.
    @discardableResult
    func addHelperSheet(parent: Int) -> Int? {
        var id: Int?
        let title = nextSheetName
        performChecked("Added helper sheet \(title)", invalidatesAnalysis: false, failureMessage: "The helper sheet could not be added") {
            id = $0.addHelperSheet(title, parent: parent)
            if let id { $0.setActiveSheet(id) }
            return id != nil
        }
        return id
    }

    func setHelperSheet(_ id: Int, _ helper: Bool) {
        guard let sheet = snapshot.sheet(id), (sheet.helper ?? false) != helper else { return }
        performChecked(helper ? "\(sheet.name) repeats with its block" : "\(sheet.name) is an ordinary child sheet",
                       invalidatesAnalysis: false, failureMessage: "The sheet's block is repeated: its channels hold copies of it") {
            $0.setHelperSheet(id, helper)
        }
    }

    /// Fitted or DNP in this channel only; one undo step.
    func setChannelFitted(_ id: Int, _ fitted: Bool) {
        guard let c = snapshot.component(id) else { return }
        performChecked(fitted ? "\(c.displayRef) fitted in this channel" : "\(c.displayRef) not fitted in this channel") {
            $0.setChannelFitted(id, fitted)
        }
    }

    /// One per-channel parameter back to the block's ("value", "package", "spice", "firmware").
    func clearChannelOverride(_ id: Int, _ what: String) {
        guard let c = snapshot.component(id) else { return }
        performChecked("\(c.displayRef) takes the block's \(what)") { $0.clearChannelOverride(id, what) }
    }

    /// This channel's own SPICE model; undoable, refused with the core's reason.
    @discardableResult
    func setChannelSpiceModel(_ id: Int, text: String, model: String, pins: String) -> Bool {
        guard let c = snapshot.component(id) else { return false }
        var failure: Error?
        let ok = performChecked("\(c.displayRef): SPICE model \(model) in this channel",
                                failureMessage: "\(c.displayRef): SPICE model not attached") { engine in
            do {
                try engine.setChannelSpiceModel(id, text: text, model: model, pins: pins)
                return true
            } catch {
                failure = error
                return false
            }
        }
        if let failure { present(failure, title: "Could not attach the SPICE model") }
        return ok
    }

    /// Sets the drawn size of a sheet's sheet symbol (0 × 0 = fitted to its entries); one undo step.
    func setSheetSymbolSize(_ id: Int, width: Double, height: Double) {
        guard let sheet = snapshot.sheet(id), sheet.symbolWidth != width || sheet.symbolHeight != height else { return }
        performChecked("\(sheet.name): sheet symbol size", invalidatesAnalysis: false) {
            $0.setSheetSymbolSize(id, width: width, height: height)
        }
    }

    /// Saves the PDF of every sheet (bookmarks follow the hierarchy; frames and title blocks on every page).
    func exportSchematicPDF() {
        guard let data = engine.schematicPDF() else {
            statusMessage = "PDF export failed"
            return
        }
        let panel = NSSavePanel()
        panel.allowedContentTypes = [.pdf]
        panel.nameFieldStringValue = "\(snapshot.name.replacingOccurrences(of: "/", with: "-"))-schematic.pdf"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        do {
            try data.write(to: url, options: .atomic)
            statusMessage = "Exported \(url.lastPathComponent)"
        } catch {
            present(error, title: "Export failed")
        }
    }

    // MARK: - ERC error reporting

    /// Reports an ERC rule at another severity ("error", "warning", "info", "off") or its own ("default"), then
    /// re-checks.
    func setErcSeverity(_ code: String, level: String) {
        let current = snapshot.ercSeverities[code] ?? "default"
        guard current != level else { return }
        performChecked("\(code): \(level)", invalidatesAnalysis: false) { $0.setErcSeverity(code, level: level) }
        runERC()
    }

    // MARK: - Update PCB (schematic → board ECO)

    /// Executes the chosen Update PCB changes as one undo step; what was done. New footprints stand where Auto Place
    /// put them; with "Place new parts interactively" on (the default) the PCB editor then opens to place them one
    /// by one (DesignStore+Placement.swift).
    func updatePCB(keys: [String]) -> [String] {
        guard !keys.isEmpty else { return [] }
        var result = PcbEcoResult(executed: 0, report: [], placementQueue: [])
        performChecked("Update PCB (\(keys.count) change(s))", failureMessage: "None of the changes could be executed") {
            result = $0.applyPcbEcoResult(keys: keys)
            return !result.report.isEmpty
        }
        if !result.report.isEmpty { statusMessage = "Updated the board: \(result.report.count) change(s)" }
        if Self.placesNewPartsInteractively, !result.placementQueue.isEmpty {
            beginPlacement(result.placementQueue)
        }
        return result.report
    }

    // MARK: - PCB pin / gate swap (back-annotated)

    /// Makes a pin or gate swap on the board; the schematic follows (one undo step).
    func swapOnBoard(_ option: PcbSwapOptionInfo) {
        performChecked("Swap \(option.label)", failureMessage: "The swap is not allowed") { $0.applyPcbSwap(option) }
    }

    /// Automatic pin / gate swap: the swaps that shorten the ratsnest most, for one package or the whole board.
    func optimizeSwaps(component: Int?) {
        var made = 0
        performChecked("Automatic pin / gate swap", failureMessage: "No swap shortens the connections") {
            made = $0.optimizePcbSwaps(component)
            return made > 0
        }
        if made > 0 { statusMessage = "Made \(made) swap(s), back-annotated to the schematic" }
    }
}
