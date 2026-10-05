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
    func align(_ mode: String) {
        let ids = Array(selection)
        guard ids.count >= 2 else { return }
        performChecked("Align \(mode)", failureMessage: mode.hasPrefix("distribute")
                       ? "Select at least three parts to distribute" : "The selection is already aligned") {
            $0.alignComponents(ids, mode: mode) > 0
        }
    }

    // MARK: - Copy and paste

    var canPaste: Bool { NSPasteboard.general.string(forType: .string).map(Self.isSchematicClip) ?? false }

    /// The clipboard text of the selection (nil when nothing is selected).
    func selectionClip() -> String? {
        let ids = Array(selection)
        return ids.isEmpty ? nil : engine.copyComponents(ids)
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
}
