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

    /// Builds the schematic from another tool's schematic or netlist (KiCad .kicad_sch / .net, LTspice .asc,
    /// Protel / Tango, CSV) through the core's
    /// schematic_import_netlist tool, one undo step.
    func importNetlist() {
        guard let picked = chooseText("Choose a schematic or netlist: KiCad (.kicad_sch with its sub-sheets, .net), Eagle (.sch), "
                                      + "LTspice (.asc), Protel / Tango (Altium, OrCAD, EasyEDA, PADS) or CSV.",
                                      extensions: ["kicad_sch", "sch", "asc", "net", "netlist", "csv", "txt"]) else { return }
        let (url, text) = picked
        var arguments: [String: Any] = ["text": text]
        if url.pathExtension == "kicad_sch" { arguments["sheets"] = Self.kicadSheets(beside: url) }
        let r = runTool("schematic_import_netlist", arguments: arguments)
        guard !r.isError, let data = r.text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let imported = j["imported"] as? [String: Any] else {
            statusMessage = "\(url.lastPathComponent): \(r.text)"
            return
        }
        let errors = ((j["checks"] as? [String: Any])?["erc"] as? [String: Any])?["errors"] as? Int ?? 0
        statusMessage = "\(url.lastPathComponent): \(imported["parts"] as? Int ?? 0) parts, \(imported["nets"] as? Int ?? 0) nets, "
            + "\((imported["created"] as? [Any])?.count ?? 0) new library parts; ERC \(errors) error(s)"
        requestView(.fit)
    }

    /// The other KiCad sheets in a schematic's folder (its sub-sheets by file name): at most 200, each under 16 MB.
    static func kicadSheets(beside url: URL) -> [String: String] {
        let folder = url.deletingLastPathComponent()
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        var sheets: [String: String] = [:]
        for file in files.filter({ $0.pathExtension == "kicad_sch" }).prefix(200)
            where ((try? file.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) < 16 << 20 {
            if let text = try? String(contentsOf: file, encoding: .utf8) { sheets[file.lastPathComponent] = text }
        }
        return sheets
    }

    /// Brings a KiCad board in through the core's pcb_import_kicad tool, one undo step: on an empty project the
    /// circuit comes from the .kicad_sch of the same name beside the board when there is one, else from its pads.
    func importKicadBoard() {
        guard let (url, text) = chooseText("Choose a KiCad board (.kicad_pcb). A .kicad_sch of the same name beside it brings the "
                                           + "schematic too.", extensions: ["kicad_pcb"]) else { return }
        var arguments: [String: Any] = ["text": text]
        if let sch = try? String(contentsOf: url.deletingPathExtension().appendingPathExtension("kicad_sch"), encoding: .utf8) {
            arguments["schematicText"] = sch
            arguments["sheets"] = Self.kicadSheets(beside: url)
        }
        let r = runTool("pcb_import_kicad", arguments: arguments)
        guard !r.isError, let data = r.text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            statusMessage = "\(url.lastPathComponent): \(r.text)"
            return
        }
        let fidelity = j["fidelity"] as? [String: Any] ?? [:]
        statusMessage = "\(url.lastPathComponent): \(j["parts"] as? Int ?? 0) parts, \(j["tracks"] as? Int ?? 0) tracks, "
            + "\(j["vias"] as? Int ?? 0) vias; \((fidelity["misfit"] as? [Any])?.count ?? 0) part(s) off KiCad's pads, "
            + "\((fidelity["missing"] as? [Any])?.count ?? 0) not in the schematic"
        requestView(.fit)
    }

    /// Another tool's routing (PADS ASCII, HyperLynx, IPC-2581 from Allegro / OrCAD) onto this board's placed parts.
    func importRoutes() {
        guard let (url, text) = chooseText("Choose a PADS ASCII (.asc), HyperLynx (.hyp) or IPC-2581 (.xml / .cvg) file. Its "
                                           + "parts must be placed here as in that design.", extensions: ["asc", "hyp", "xml", "cvg"]) else { return }
        let r = runTool("pcb_import_routes", arguments: ["text": text])
        guard !r.isError, let data = r.text.data(using: .utf8),
              let j = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            statusMessage = "\(url.lastPathComponent): \(r.text)"
            return
        }
        statusMessage = "\(url.lastPathComponent): \(j["tracks"] as? Int ?? 0) tracks, \(j["vias"] as? Int ?? 0) vias; "
            + "\(j["padHits"] as? Int ?? 0) of \(j["ends"] as? Int ?? 0) track ends on pads, "
            + "\((j["unknownNets"] as? [Any])?.count ?? 0) net(s) not in this design"
    }

    /// Keeps a Gerber, drill or ODB++ features file over the board as a reference layer (never fabricated), one undo step.
    func importReferenceLayer() {
        guard let picked = chooseText("Choose a Gerber, drill or ODB++ layer features file to show over the board.",
                                      extensions: ["gbr", "gtl", "gbl", "gto", "gbo", "gko", "gm1", "drl", "xln", "txt"]) else { return }
        addReferenceLayer(name: picked.0.lastPathComponent, text: picked.1)
    }

    /// An image as a top-silkscreen logo, 10 mm wide in the middle of the board (one undo step).
    func importLogo() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.image]
        panel.message = "Choose an image for a silkscreen logo (dark pixels print)."
        guard panel.runModal() == .OK, let url = panel.url, let image = NSImage(contentsOf: url),
              let cg = image.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return }
        let rows = Self.logoRows(cg)
        addLogo(name: url.deletingPathExtension().lastPathComponent, rows: rows, width: 10)
    }

    /// Thresholded rows of an image ('1' = dark and opaque), at most `maxWidth` pixels across, top row first.
    nonisolated static func logoRows(_ image: CGImage, maxWidth: Int = 200) -> [String] {
        let w = min(maxWidth, image.width), h = max(1, Int((Double(image.height) * Double(w) / Double(max(1, image.width))).rounded()))
        var px = [UInt8](repeating: 0, count: w * h * 4)
        guard let ctx = CGContext(data: &px, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return [] }
        ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
        return (0..<h).map { y in
            String((0..<w).map { x -> Character in
                let i = (y * w + x) * 4
                let a = Int(px[i + 3]), lum = (Int(px[i]) * 3 + Int(px[i + 1]) * 6 + Int(px[i + 2])) / 10
                return a > 127 && lum < a / 2 ? "1" : "0"  // premultiplied: dark = luminance below half the alpha
            })
        }
    }

    func addLogo(name: String, rows: [String], width: Double) {
        let b = snapshot.board
        let height = width * Double(rows.count) / Double(max(1, rows.map(\.count).max() ?? 1))
        let raster: [String: Any] = ["rows": rows, "x": b.width / 2 - width / 2, "y": b.height / 2 - height / 2, "width": width]
        let r = runTool("pcb_graphics", arguments: ["add": ["name": name, "layer": "silk-top", "raster": raster]])
        statusMessage = r.isError ? "\(name): no dark pixels to print" : "\(name) placed on the top silkscreen"
    }

    /// Library update notices: project parts whose catalog part changed since they were added; Update All swaps the
    /// placed parts to the current catalog part (one undo step).
    func checkLibraryUpdates() {
        let r = runTool("library_updates", arguments: [:])
        let updates = (try? JSONSerialization.jsonObject(with: Data(r.text.utf8)) as? [String: Any])?["updates"] as? [[String: Any]] ?? []
        let alert = NSAlert()
        alert.messageText = String(localized: "Library Updates")
        guard !updates.isEmpty else {
            alert.informativeText = String(localized: "Every library part is up to date.")
            alert.runModal()
            return
        }
        alert.informativeText = updates.map { u in
            "\(u["name"] as? String ?? "") (\(u["instances"] as? Int ?? 0)): " + ((u["changes"] as? [String]) ?? []).joined(separator: ", ")
        }.joined(separator: "\n")
        alert.addButton(withTitle: String(localized: "Update All"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        if alert.runModal() == .alertFirstButtonReturn { _ = runTool("library_updates", arguments: ["apply": ["*"]]) }
    }

    /// A part's pin map as a firmware header (.h), Vivado (.xdc), Quartus (.qsf) or CSV file, by the extension chosen.
    func exportPinMap(ref: String) {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "\(ref)_pins.h"
        panel.message = "Pin map of \(ref): .h (firmware #defines), .xdc (Vivado), .qsf (Quartus) or .csv"
        guard panel.runModal() == .OK, let url = panel.url else { return }
        let format = ["h": "header", "xdc": "xdc", "qsf": "qsf", "csv": "csv"][url.pathExtension.lowercased()] ?? "header"
        let r = runTool("schematic_pin_map", arguments: ["ref": ref, "format": format])
        let text = (try? JSONSerialization.jsonObject(with: Data(r.text.utf8)) as? [String: Any])?["text"] as? String ?? ""
        do { try text.write(to: url, atomically: true, encoding: .utf8) } catch { present(error, title: "Could not write the pin map") }
    }

    /// Net labels on a part's pins from an STM32CubeMX .ioc, Vivado .xdc, Quartus .qsf or CSV (pin, net) file.
    func assignPins(ref: String) {
        guard let picked = chooseText("Choose pin assignments for \(ref): CubeMX .ioc, .xdc, .qsf or CSV (pin, net).",
                                      extensions: ["ioc", "xdc", "qsf", "csv", "txt"]) else { return }
        let r = runTool("schematic_pin_map", arguments: ["ref": ref, "assign": picked.1])
        let result = (try? JSONSerialization.jsonObject(with: Data(r.text.utf8)) as? [String: Any]) ?? [:]
        let unknown = (result["unknownPins"] as? [String]) ?? []
        statusMessage = r.isError ? r.text : "\(result["assigned"] as? Int ?? 0) pins of \(ref) labelled"
            + (unknown.isEmpty ? "" : "; not on the part: " + unknown.prefix(8).joined(separator: ", "))
    }

    func removeGraphic(at index: Int) {
        _ = runTool("pcb_graphics", arguments: ["remove": index])
    }

    func addReferenceLayer(name: String, text: String) {
        let ok = performExternalEdit("Reference layer \(name)") { $0.addReferenceLayer(name: name, text: text) }
        statusMessage = ok ? "\(name) shown over the board as a reference layer" : "\(name): not a Gerber or drill file with a drawing"
    }

    func setReferenceLayer(_ index: Int, visible: Bool) {
        performChecked(visible ? "Show reference layer" : "Hide reference layer", invalidatesAnalysis: false) {
            $0.setReferenceLayer(index, visible: visible)
        }
    }

    func removeReferenceLayer(_ index: Int) {
        performChecked("Remove reference layer", invalidatesAnalysis: false) { $0.setReferenceLayer(index, visible: false, remove: true) }
    }

    /// A reference layer's shapes (read once, then cached).
    func referenceArtwork(_ index: Int) -> GerberArtwork? {
        guard snapshot.referenceLayers.indices.contains(index) else { return nil }
        let layer = snapshot.referenceLayers[index]
        let key = "\(index):\(layer.name):\(layer.bytes)"
        if let cached = referenceArtworkCache[key] { return cached }
        guard let art = engine.referenceLayer(index) else { return nil }
        // Only the layers there now: a removed, re-imported or other project's layer is dropped.
        let live = Set(snapshot.referenceLayers.enumerated().map { "\($0.offset):\($0.element.name):\($0.element.bytes)" })
        referenceArtworkCache = referenceArtworkCache.filter { live.contains($0.key) }
        referenceArtworkCache[key] = art
        return art
    }

    /// Board outline and cutouts from a DXF drawing (MCAD), one undo step.
    func importDXFOutline() {
        guard let picked = chooseText("Choose the board outline drawing (.dxf) from mechanical CAD.", extensions: ["dxf"]) else { return }
        let (url, text) = picked
        var result: EDAEngine.DXFImport?
        performExternalEdit("Board outline from \(url.lastPathComponent)") { engine in
            result = engine.importDXFOutline(text)
            return result?.ok == true
        }
        guard let r = result else { statusMessage = "\(url.lastPathComponent): not a DXF file"; return }
        statusMessage = r.ok ? String(format: "%@: board %.1f × %.1f mm, %d cutout(s)", url.lastPathComponent, r.width, r.height, r.cutouts)
                             : "\(url.lastPathComponent): \(r.error)"
        if r.ok { requestView(.fit) }
    }

    /// Tallest part allowed per side by the enclosure (mm, 0 = no limit); the DRC reports MECH_HEIGHT. Undoable.
    func setEnclosureHeight(top: Double? = nil, bottom: Double? = nil) {
        let current = engine.enclosureHeights()
        let t = top ?? current.top, b = bottom ?? current.bottom
        guard t != current.top || b != current.bottom else { return }
        performChecked("Enclosure height", invalidatesAnalysis: false) { $0.setEnclosureHeights(top: t, bottom: b) }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Loads an enclosure (STEP / STL / OBJ / VRML, millimetres in the MCAD frame of the STEP export) the parts must
    /// clear; the DRC reports MECH_ENCLOSURE and the 3D view shows it see-through. Undoable.
    func loadEnclosure() {
        let panel = NSOpenPanel()
        panel.message = String(localized: "Choose the enclosure model (STEP, STL, OBJ or VRML) in the board's MCAD frame")
        guard panel.runModal() == .OK, let url = panel.url, let data = try? Data(contentsOf: url) else { return }
        let result = EDAEngine.importModel3D(name: url.lastPathComponent, data: data)
        guard result.ok, let id = result.id else {
            alert = AlertItem(title: "Enclosure not loaded", message: result.error)
            return
        }
        performChecked("Enclosure", invalidatesAnalysis: false) { $0.setEnclosureModel(id) }
        runDRC()
    }

    /// Gives a placed part (a catalog part too) its own 3D model from a STEP / STL / OBJ / VRML file. Undoable.
    func assignPartModel(_ component: Int) {
        let panel = NSOpenPanel()
        panel.message = String(localized: "Choose the part's 3D model (STEP, STL, OBJ or VRML)")
        guard panel.runModal() == .OK, let url = panel.url, let data = try? Data(contentsOf: url) else { return }
        let result = EDAEngine.importModel3D(name: url.lastPathComponent, data: data)
        guard result.ok, let id = result.id else {
            alert = AlertItem(title: "3D model not loaded", message: result.error)
            return
        }
        performChecked("3D model", invalidatesAnalysis: false) {
            $0.setPartModel(component, model: id, name: url.lastPathComponent, unit: result.unit ?? 1)
        }
    }

    func removePartModel(_ component: Int) {
        performChecked("Remove 3D model", invalidatesAnalysis: false) { $0.setPartModel(component, model: "") }
    }

    func removeEnclosure() {
        performChecked("Remove enclosure", invalidatesAnalysis: false) { $0.setEnclosureModel("") }
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

    /// Runs a plug-in (ProjectPlugins) on the open design: its changes are one undo step, its output is shown.
    func runPlugin(_ plugin: URL) async {
        let name = plugin.lastPathComponent
        statusMessage = "Running plug-in \(name)…"
        let result = await ProjectPlugins.run(plugin, json: engine.saveJSON())
        var note = result.json == nil ? "The design is unchanged." : "Its changes are one undo step."
        if let changed = result.json, !performExternalEdit("Plug-in \(name)", { (try? $0.load(json: changed)) != nil }) {
            note = "It left a project SiEDA could not read, so nothing changed."
        }
        statusMessage = "Plug-in \(name) finished"
        alert = AlertItem(title: "Plug-in \(name)", message: (result.output.isEmpty ? "" : String(result.output.prefix(2000)) + "\n\n") + note)
    }

    /// Replaces the board's custom DRC rules (a JSON array); undoable. Text that is not a JSON array changes nothing.
    func setCustomRules(_ json: String) {
        performChecked("Custom Rules", invalidatesAnalysis: false) { $0.customRules(json) != nil }
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
        let earlier = EDAEngine(name: url.lastPathComponent)
        pcbCompare = (try? earlier.load(json: text)) != nil ? earlier.snapshot() : nil
    }
}

struct DesignDiff: Identifiable {
    var title: String
    var text: String
    var id: String { title + text }
}

/// Layout reuse, rooms, partitions, routing policies, variant checks and the company library: the core's MCP tools
/// (pcb_layout_block, pcb_rooms, pcb_partitions, pcb_routing_policies, pcb_variant_check, library_company), each a
/// single undo step through runTool.
extension DesignStore {
    /// Designators of the selected footprints, top-left first (the first is a layout group's anchor).
    var selectedRefs: [String] {
        selection.compactMap { snapshot.component($0) }.filter { $0.pcb.placed }
            .sorted { ($0.pcb.y, $0.pcb.x) < ($1.pcb.y, $1.pcb.x) }.map(\.ref)
    }

    private func askText(_ title: String, _ message: String, _ value: String = "") -> String? {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = message
        let field = NSTextField(string: value)
        field.frame = NSRect(x: 0, y: 0, width: 340, height: 24)
        alert.accessoryView = field
        alert.addButton(withTitle: String(localized: "OK"))
        alert.addButton(withTitle: String(localized: "Cancel"))
        guard alert.runModal() == .alertFirstButtonReturn else { return nil }
        let text = field.stringValue.trimmingCharacters(in: .whitespaces)
        return text.isEmpty ? nil : text
    }

    private func inform(_ title: String, _ text: String) {
        let alert = NSAlert()
        alert.messageText = title
        alert.informativeText = text
        alert.runModal()
    }

    /// The tool's JSON result, or nil after showing its error.
    private func tool(_ name: String, _ arguments: [String: Any], root: String = "") -> [String: Any]? {
        let r = runTool(name, arguments: arguments, root: root)
        if r.isError {
            inform(name, r.text)
            return nil
        }
        return (try? JSONSerialization.jsonObject(with: Data(r.text.utf8)) as? [String: Any]) ?? [:]
    }

    /// The selected footprints' courtyards round, plus `margin` mm.
    private func selectionArea(margin: Double) -> CGRect? {
        let boxes = snapshot.courtyards.filter { selection.contains($0.component) }.map(\.rect)
        guard let first = boxes.first else { return nil }
        return boxes.dropFirst().reduce(first) { $0.union($1) }.insetBy(dx: -margin, dy: -margin)
    }

    private func needSelection(_ what: String, count: Int = 1) -> [String]? {
        let refs = selectedRefs
        if refs.count < count { inform(what, String(localized: "Select the footprints on the board first.")); return nil }
        return refs
    }

    func replicateLayout() {
        guard let refs = needSelection("Replicate Layout"),
              let text = askText(String(localized: "Replicate Layout"),
                                 "Target groups: the same parts in the same order as \(refs.joined(separator: " ")), groups split by \";\" (e.g. R5 C5; R9 C9).")
        else { return }
        let targets = text.split(separator: ";").map { $0.split(whereSeparator: { $0 == " " || $0 == "," }).map(String.init) }.filter { !$0.isEmpty }
        guard let r = tool("pcb_layout_block", ["action": "replicate", "refs": refs, "targets": targets]) else { return }
        let groups = (r["groups"] as? [[String: Any]]) ?? []
        statusMessage = "Replicated onto " + groups.map { g in
            "\(g["anchor"] as? String ?? "?") (\(g["tracks"] as? Int ?? 0) tracks, \(g["vias"] as? Int ?? 0) vias)"
        }.joined(separator: ", ")
    }

    func saveLayoutBlock() {
        guard let refs = needSelection("Save Layout Block") else { return }
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "\(refs[0])-layout.json"
        panel.message = String(localized: "Save the selected parts' placement and copper as a layout block.")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var arguments: [String: Any] = ["action": "save", "refs": refs, "path": url.lastPathComponent]
        if let by = askText(String(localized: "Approved by"), String(localized: "Who approved this layout (leave empty for none)?")) {
            arguments["approvedBy"] = by
        }
        guard let r = tool("pcb_layout_block", arguments, root: url.deletingLastPathComponent().path) else { return }
        statusMessage = "Layout block saved: \(r["parts"] as? Int ?? 0) parts, \(r["tracks"] as? Int ?? 0) tracks"
    }

    func applyLayoutBlock() {
        guard let refs = needSelection("Apply Layout Block") else { return }
        let panel = NSOpenPanel()
        panel.message = "Choose a layout block for \(refs.joined(separator: " ")) (the same parts in the same order)."
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let r = tool("pcb_layout_block", ["action": "apply", "refs": refs, "path": url.lastPathComponent],
                           root: url.deletingLastPathComponent().path) else { return }
        let approved = (r["approved"] as? Bool ?? false) ? "approved by \(r["approvedBy"] as? String ?? "")" : "not an approved layout"
        statusMessage = "Layout applied (\(approved)): \(r["tracks"] as? Int ?? 0) tracks, \(r["vias"] as? Int ?? 0) vias, \(r["skipped"] as? Int ?? 0) skipped"
    }

    private func listed(_ toolName: String, _ key: String) -> [[String: Any]] {
        (tool(toolName, ["action": "list"])?[key] as? [[String: Any]]) ?? []
    }

    private func area(_ entry: [String: Any]) -> [String: Any] {
        ["x0": entry["x0"] ?? 0, "y0": entry["y0"] ?? 0, "x1": entry["x1"] ?? 0, "y1": entry["y1"] ?? 0]
    }

    func roomFromSelection() {
        guard let refs = needSelection("Room"), let box = selectionArea(margin: 1),
              let name = askText(String(localized: "Room"), String(localized: "Name of the room these parts belong in:"),
                                 "Room \(listed("pcb_rooms", "rooms").count + 1)") else { return }
        var rooms = listed("pcb_rooms", "rooms").map { r in area(r).merging(["name": r["name"] ?? "", "refs": r["refs"] ?? [String]()] as [String: Any]) { a, _ in a } }
        rooms.append(["name": name, "refs": refs, "x0": Double(box.minX), "y0": Double(box.minY), "x1": Double(box.maxX), "y1": Double(box.maxY)])
        if tool("pcb_rooms", ["action": "set", "rooms": rooms]) != nil { statusMessage = "Room \(name): \(refs.joined(separator: ", "))" }
    }

    func placeRooms() {
        guard let r = tool("pcb_rooms", ["action": "place"]) else { return }
        let moved = (r["moved"] as? [String]) ?? [], stuck = (r["noRoom"] as? [String]) ?? []
        statusMessage = moved.isEmpty && stuck.isEmpty ? "Every part is in its room"
            : "Moved into their rooms: \(moved.joined(separator: ", "))" + (stuck.isEmpty ? "" : "; no room for \(stuck.joined(separator: ", "))")
    }

    func clearRooms() { _ = tool("pcb_rooms", ["action": "set", "rooms": [Any]()]) }

    /// Your name for partitions and approvals (kept in user defaults).
    private var ownerName: String? {
        get { UserDefaults.standard.string(forKey: "team.ownerName") }
        set { UserDefaults.standard.set(newValue, forKey: "team.ownerName") }
    }

    func partitionFromSelection() {
        guard needSelection("Partition") != nil, let box = selectionArea(margin: 1),
              let owner = askText(String(localized: "Partition"), String(localized: "Who owns this area of the board?"), ownerName ?? "")
        else { return }
        var parts = listed("pcb_partitions", "partitions").map { q in area(q).merging(["name": q["name"] ?? "", "owner": q["owner"] ?? ""] as [String: Any]) { a, _ in a } }
        parts.append(["name": "\(owner)'s area \(parts.count + 1)", "owner": owner, "x0": Double(box.minX), "y0": Double(box.minY), "x1": Double(box.maxX), "y1": Double(box.maxY)])
        if tool("pcb_partitions", ["action": "set", "partitions": parts]) != nil { statusMessage = "Partition for \(owner) added" }
    }

    func checkPartitions() {
        guard let me = askText(String(localized: "Check Partitions"), String(localized: "Your name (as the partitions' owners are named):"), ownerName ?? "")
        else { return }
        ownerName = me
        let panel = NSOpenPanel()
        panel.message = String(localized: "Choose the base version of the project (the one you pulled or opened).")
        panel.allowedContentTypes = [UTType(filenameExtension: "siedaproj") ?? .data]
        guard panel.runModal() == .OK, let url = panel.url,
              let r = tool("pcb_partitions", ["action": "check", "owner": me, "against": url.lastPathComponent],
                           root: url.deletingLastPathComponent().path) else { return }
        let found = (r["outsideYours"] as? [[String: Any]]) ?? []
        inform(String(localized: "Check Partitions"), found.isEmpty ? String(localized: "No change inside anyone else's partition.") : found.map { f in
            "\(f["partition"] as? String ?? "") (\(f["owner"] as? String ?? "")): parts \(((f["parts"] as? [String]) ?? []).joined(separator: ", ")); copper changes \(f["copperChanges"] as? Int ?? 0)"
        }.joined(separator: "\n"))
    }

    func editRoutingPolicy() {
        guard let text = askText(String(localized: "Routing Policy"),
                                 String(localized: "Net class: layers; most vias — e.g. \"HS: 0,3; 2\" (layer 0 = top). An empty layers or vias part leaves it free."))
        else { return }
        let parts = text.split(separator: ":", maxSplits: 1).map { $0.trimmingCharacters(in: .whitespaces) }
        guard parts.count == 2, !parts[0].isEmpty else { inform(String(localized: "Routing Policy"), String(localized: "Write it as class: layers; vias")); return }
        let fields = parts[1].split(separator: ";", omittingEmptySubsequences: false).map { $0.trimmingCharacters(in: .whitespaces) }
        var policy: [String: Any] = ["netClass": parts[0]]
        let layers = fields[0].split(separator: ",").compactMap { Int($0.trimmingCharacters(in: .whitespaces)) }
        if !layers.isEmpty { policy["layers"] = layers }
        if fields.count > 1, let vias = Int(fields[1]) { policy["maxVias"] = vias }
        var policies = listed("pcb_routing_policies", "policies").filter { ($0["netClass"] as? String) != parts[0] }
        policies.append(policy)
        if tool("pcb_routing_policies", ["action": "set", "policies": policies]) != nil { statusMessage = "Routing policy for \(parts[0]) set" }
    }

    func checkVariants() {
        guard let r = tool("pcb_variant_check", [:]) else { return }
        let variants = (r["variants"] as? [[String: Any]]) ?? []
        var lines = variants.map { v -> String in
            let open = (v["openNets"] as? [String]) ?? [], overlaps = (v["overlaps"] as? [[String]]) ?? []
            if v["ok"] as? Bool ?? false { return "\(v["variant"] as? String ?? ""): OK" }
            return "\(v["variant"] as? String ?? ""):" + (open.isEmpty ? "" : " open nets \(open.joined(separator: ", "))")
                + (overlaps.isEmpty ? "" : " overlaps \(overlaps.map { $0.joined(separator: "/") }.joined(separator: ", "))")
        }
        let alternates = (r["alternates"] as? [[String]]) ?? []
        if !alternates.isEmpty { lines.append("Alternates (never fitted together): " + alternates.map { $0.joined(separator: "/") }.joined(separator: ", ")) }
        inform(String(localized: "Check Variants"), lines.joined(separator: "\n"))
    }

    // MARK: Team rule packs (the board's design rules as one shared file)

    func saveRulePack() {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = "team.rules.json"
        panel.message = String(localized: "Save the board's design rules as a team rule pack.")
        guard panel.runModal() == .OK, let url = panel.url else { return }
        var arguments: [String: Any] = ["action": "save", "path": url.lastPathComponent,
                                        "name": url.deletingPathExtension().deletingPathExtension().lastPathComponent]
        if let by = askText(String(localized: "Approved by"), String(localized: "Who approved these rules (leave empty for none)?")) {
            arguments["approvedBy"] = by
        }
        guard let r = tool("pcb_rule_pack", arguments, root: url.deletingLastPathComponent().path) else { return }
        statusMessage = "Rule pack \(r["name"] as? String ?? "") saved (\(r["rules"] as? Int ?? 0) rules)"
    }

    /// The chosen rule file with pcb_rule_pack `action` (apply or check).
    private func rulePack(_ action: String) -> [String: Any]? {
        let panel = NSOpenPanel()
        panel.message = String(localized: "Choose a team rule pack.")
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        return tool("pcb_rule_pack", ["action": action, "path": url.lastPathComponent], root: url.deletingLastPathComponent().path)
    }

    func applyRulePack() {
        guard let r = rulePack("apply") else { return }
        let changed = (r["differences"] as? [String]) ?? []
        statusMessage = "Rule pack \(r["name"] as? String ?? "") applied" + (changed.isEmpty ? " (no change)" : ": \(changed.joined(separator: ", "))")
    }

    func compareRulePack() {
        guard let r = rulePack("check") else { return }
        let changed = (r["differences"] as? [String]) ?? []
        inform(String(localized: "Team Rule Pack"), changed.isEmpty ? String(localized: "The board's rules match the rule pack.")
            : String(localized: "These rules differ from the rule pack:") + "\n" + changed.joined(separator: "\n"))
    }

    // MARK: Company library (a team folder of parts: draft / released / obsolete)

    /// The team library folder (chosen once, kept in user defaults).
    func companyLibraryFolder(choose: Bool = false) -> URL? {
        if !choose, let path = UserDefaults.standard.string(forKey: "library.companyFolder"), FileManager.default.fileExists(atPath: path) {
            return URL(fileURLWithPath: path)
        }
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.message = String(localized: "Choose the company library folder (shared with your team).")
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        UserDefaults.standard.set(url.path, forKey: "library.companyFolder")
        return url
    }

    /// library_company with the folder given as its name inside its parent (the tool's root).
    func companyLibrary(_ arguments: [String: Any]) -> [String: Any]? {
        guard let folder = companyLibraryFolder() else { return nil }
        var args = arguments
        args["folder"] = folder.lastPathComponent
        return tool("library_company", args, root: folder.deletingLastPathComponent().path)
    }

    /// Publishes the selected part's library part (a project part) as a draft, version + 1.
    func companyLibraryPublish() {
        guard let id = selection.first, let part = snapshot.component(id)?.customPart else {
            inform(String(localized: "Company Library"), String(localized: "Select a placed library part (made in the part editor or imported) first."))
            return
        }
        if let r = companyLibrary(["action": "publish", "id": part]) {
            statusMessage = "\(r["name"] as? String ?? part) published as draft v\(r["version"] as? Int ?? 0)"
        }
    }

    /// Release (a librarian approves), obsolete, alternates or place, by part name (asked for when not given).
    func companyLibraryAction(_ action: String, name given: String? = nil) {
        guard let name = given ?? askText(String(localized: "Company Library"), String(localized: "Part name in the company library:")) else { return }
        var arguments: [String: Any] = ["action": action, "name": name]
        switch action {
        case "release":
            guard let by = askText(String(localized: "Release"), String(localized: "Released by (a librarian in librarians.json, if the folder has one):"), ownerName ?? "")
            else { return }
            arguments["approvedBy"] = by
        case "alternates":
            guard let list = askText(String(localized: "Approved Alternates"), String(localized: "Alternate part names, separated by commas:")) else { return }
            arguments["alternates"] = list.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        default:
            break
        }
        guard let r = companyLibrary(arguments) else { return }
        if action == "use" {
            let ref = (r["component"] as? [String: Any])?["ref"] as? String ?? ""
            statusMessage = "\(name) placed as \(ref)" + ((r["approved"] as? Bool ?? false) ? " (released)" : " (draft)")
        } else {
            statusMessage = "\(name): \(r["status"] as? String ?? "")"
        }
    }
}
