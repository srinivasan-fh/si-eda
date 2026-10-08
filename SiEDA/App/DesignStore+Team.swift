import AppKit
import UniformTypeIdentifiers

/// Exchange with mechanical CAD and design review: IDF placement import, compare with another version.
extension DesignStore {
    private func chooseText(_ message: String, extensions: [String]) -> (URL, String)? {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = extensions.compactMap { UTType(filenameExtension: $0) } + [.data]
        panel.allowsMultipleSelection = false
        panel.message = message
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            return (url, try String(contentsOf: url, encoding: .utf8))
        } catch {
            present(error, title: "Could not read \(url.lastPathComponent)")
            return nil
        }
    }

    /// Asks for an IDF board file (.emn) from MCAD and moves the parts it places. One undo step.
    func importIDFPlacement() {
        guard let picked = chooseText("Choose the IDF board file (.emn) from mechanical CAD.", extensions: ["emn"]) else { return }
        let (url, text) = picked
        var moved: [String] = []
        performExternalEdit("MCAD placement from \(url.lastPathComponent)") { engine in
            moved = engine.importIDFPlacement(text)
            return !moved.isEmpty
        }
        statusMessage = moved.isEmpty ? "\(url.lastPathComponent): no part moved"
                                      : "Moved \(moved.count) parts: \(moved.prefix(8).joined(separator: ", "))"
    }

    /// Asks for an IDX file (baseline or changes) from MCAD and applies its placements. One undo step.
    func importIDX() {
        guard let picked = chooseText("Choose the IDX file (.idx) from mechanical CAD.", extensions: ["idx", "xml"]) else { return }
        let (url, text) = picked
        var result: EDAEngine.IDXImport?
        performExternalEdit("MCAD changes from \(url.lastPathComponent)") { engine in
            result = engine.importIDX(text)
            guard let r = result else { return false }
            return !r.moved.isEmpty || r.outline || r.thickness || r.keepouts > 0 || r.heightZones > 0
        }
        guard let r = result else { statusMessage = "\(url.lastPathComponent): not an IDX file"; return }
        var parts: [String] = []
        if !r.moved.isEmpty { parts.append("moved \(r.moved.count) parts (\(r.moved.prefix(6).joined(separator: ", ")))") }
        if r.outline { parts.append("new board outline") }
        if r.thickness { parts.append("new thickness") }
        if r.keepouts > 0 { parts.append("\(r.keepouts) keep-outs") }
        if r.heightZones > 0 { parts.append("\(r.heightZones) height zones") }
        statusMessage = parts.isEmpty ? "\(url.lastPathComponent): no change" : "\(url.lastPathComponent): " + parts.joined(separator: ", ")
    }

    /// Tallest part allowed per side by the enclosure (mm, 0 = no limit); the DRC reports MECH_HEIGHT. Undoable.
    func setEnclosureHeight(top: Double? = nil, bottom: Double? = nil) {
        let current = engine.enclosureHeights()
        let t = top ?? current.top, b = bottom ?? current.bottom
        guard t != current.top || b != current.bottom else { return }
        performChecked("Enclosure height", invalidatesAnalysis: false) { $0.setEnclosureHeights(top: t, bottom: b) }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Manufacturer rule pack ("" = none): tightens the DRC minimums and adds the DFM / DFA checks. Undoable.
    /// Field-solved Z0 and Zdiff of each copper layer's impedance-controlled widths (from the stack-up report), keyed by
    /// layer name; solved off the main thread.
    func fieldSolveStackup() async -> [String: FieldSolveInfo] {
        let engine = self.engine
        let copper = stackup().layers.filter(\.isCopper)
        return await runBusy("Solving fields…", stoppable: false) {
            var out: [String: FieldSolveInfo] = [:]
            for (index, layer) in copper.enumerated() {
                guard let se = layer.seWidth, var r = engine.fieldSolve(layer: index, width: se) else { continue }
                if let w = layer.diffWidth, let gap = layer.diffGap { r.zdiff = engine.fieldSolve(layer: index, width: w, gap: gap)?.zdiff }
                out[layer.name] = r
            }
            return out
        }
    }

    func setDfmPack(_ id: String) {
        guard id != snapshot.board.dfmPack else { return }
        let name = EDAEngine.dfmPacks.first { $0.id == id }?.name ?? "No manufacturer rules"
        performChecked(name, invalidatesAnalysis: false) { $0.setDfmPack(id) }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Production panel for the fabrication package (1 × 1 removes it). Undoable.
    func setPanel(_ panel: PanelInfo) {
        guard panel != (snapshot.board.panel ?? PanelInfo()) else { return }
        let name = panel.nx * panel.ny > 1 ? "Panel \(panel.nx) × \(panel.ny)" : "No panel"
        performChecked(name, invalidatesAnalysis: false) { $0.setPanel(panel) }
    }

    /// A design review command as one undo step; the store's alert shows a refusal (unknown part, empty text).
    func review(_ request: [String: Any], _ actionName: String) {
        var failure: String?
        performExternalEdit(actionName) { engine in
            failure = engine.reviewCommand(request)
            return failure == nil
        }
        if let failure { alert = AlertItem(title: "Review", message: failure) }
        objectWillChange.send()
    }

    /// Asks for another version of the project and shows what changed from it to the open design.
    func compareWithFile() {
        guard let picked = chooseText("Choose the earlier version to compare the open design with.",
                                      extensions: ["siedaproj"]) else { return }
        let (url, text) = picked
        designDiff = DesignDiff(title: url.lastPathComponent, text: engine.diffText(from: text))
    }
}

struct DesignDiff: Identifiable {
    var title: String
    var text: String
    var id: String { title + text }
}
