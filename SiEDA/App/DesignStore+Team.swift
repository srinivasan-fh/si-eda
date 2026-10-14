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
        guard let picked = chooseText("Choose a schematic or netlist: KiCad (.kicad_sch, .net), LTspice (.asc), Protel / Tango "
                                      + "(Altium, OrCAD, EasyEDA, PADS) or CSV.",
                                      extensions: ["kicad_sch", "asc", "net", "netlist", "csv", "txt"]) else { return }
        let (url, text) = picked
        let r = runTool("schematic_import_netlist", arguments: ["text": text])
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

    /// Brings a KiCad board in through the core's pcb_import_kicad tool, one undo step: on an empty project the
    /// circuit comes from the .kicad_sch of the same name beside the board when there is one, else from its pads.
    func importKicadBoard() {
        guard let (url, text) = chooseText("Choose a KiCad board (.kicad_pcb). A .kicad_sch of the same name beside it brings the "
                                           + "schematic too.", extensions: ["kicad_pcb"]) else { return }
        var arguments: [String: Any] = ["text": text]
        if let sch = try? String(contentsOf: url.deletingPathExtension().appendingPathExtension("kicad_sch"), encoding: .utf8) {
            arguments["schematicText"] = sch
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
