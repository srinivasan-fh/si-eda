import CoreGraphics
import Foundation

enum EDAEngineError: LocalizedError {
    case loadFailed(String)
    case operationFailed(String)

    var errorDescription: String? {
        switch self {
        case .loadFailed(let message): return "Could not open the design: \(message)"
        case .operationFailed(let message): return message
        }
    }
}

/// Thread-safe Swift façade over the C++ SiEDA core (`sieda_c.h`).
///
/// All calls are serialized with a lock, so long-running work (autorouting, transient simulation)
/// can be moved off the main thread while the UI keeps reading snapshots.
/// Where a running autoroute is (`sieda_pcb_autoroute_progress`). `phase`: 0 preparing, 1 routing, 2 rip-up pass,
/// 3 finishing. `done` of `total` nets of the current pass; `unrouted`: connections the best pass so far leaves
/// unrouted (-1 until a pass has finished).
struct RouteProgressReport: Equatable, Sendable {
    var phase = 0
    var pass = 0
    var done = 0
    var total = 0
    var unrouted = -1

    var fraction: Double { total > 0 ? min(1, max(0, Double(done) / Double(total))) : 0 }
}

/// Carries a running autoroute's progress from the routing thread to the app, and the user's cancel request back.
/// Thread-safe: `onProgress` runs on the routing thread.
final class RouteProgressChannel: @unchecked Sendable {
    private let lock = NSLock()
    private var cancelRequested = false
    private let onProgress: @Sendable (RouteProgressReport) -> Void

    init(onProgress: @escaping @Sendable (RouteProgressReport) -> Void) {
        self.onProgress = onProgress
    }

    func cancel() {
        lock.lock()
        cancelRequested = true
        lock.unlock()
    }

    var isCancelled: Bool {
        lock.lock()
        defer { lock.unlock() }
        return cancelRequested
    }

    /// Passes a report on; true when the route should stop.
    fileprivate func deliver(_ report: RouteProgressReport) -> Bool {
        onProgress(report)
        return isCancelled
    }
}

final class EDAEngine: @unchecked Sendable {
    private let lock = NSLock()
    private var handle: OpaquePointer

    init(name: String = "Untitled") {
        guard let project = sieda_project_new(name) else {
            fatalError("SiEDA core failed to allocate a project")
        }
        handle = project
    }

    deinit {
        sieda_project_free(handle)
    }

    static var coreVersion: String { String(cString: sieda_version()) }

    // MARK: - Helpers

    func withHandle<T>(_ body: (OpaquePointer) -> T) -> T {  // internal: the EDAEngine+… extensions use it
        lock.lock()
        defer { lock.unlock() }
        return body(handle)
    }

    /// Takes ownership of a malloc'd C string returned by the core.
    static func take(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { sieda_string_free(pointer) }
        return String(cString: pointer)
    }

    /// Error envelope the C API returns when the core throws.
    private struct CoreError: Decodable { let error: String }

    /// Decodes a core result; a `{"error": …}` reply or undecodable JSON becomes a failure with its message.
    private static func decodeChecked<T: Decodable>(_ type: T.Type, from json: String?) -> Result<T, EDAEngineError> {
        guard let json, let data = json.data(using: .utf8) else { return .failure(.operationFailed("no reply from the core")) }
        if let value = try? JSONDecoder().decode(type, from: data) { return .success(value) }
        if let failure = try? JSONDecoder().decode(CoreError.self, from: data) { return .failure(.operationFailed(failure.error)) }
        return .failure(.operationFailed("unreadable reply from the core"))
    }

    static func decode<T: Decodable>(_ type: T.Type, from json: String?) -> T? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    // MARK: - Project lifecycle

    func setName(_ name: String) { withHandle { sieda_project_set_name($0, name) } }
    func setRequirements(_ text: String) { withHandle { sieda_project_set_requirements($0, text) } }
    func clear() { withHandle { sieda_project_clear($0) } }
    /// Blank project: also resets the library, board, rules, net classes, pours, holes and outline.
    func reset() { withHandle { sieda_project_reset($0) } }

    func saveJSON() -> String {
        withHandle { Self.take(sieda_project_save_json($0)) } ?? "{}"
    }

    /// The project without indentation, for undo history and change detection (files use `saveJSON`).
    func stateJSON() -> String {
        withHandle { Self.take(sieda_project_state_json($0)) } ?? "{}"
    }

    func load(json: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        guard let newHandle = sieda_project_load_json(json, &errorPointer) else {
            let message = Self.take(errorPointer) ?? "unknown error"
            throw EDAEngineError.loadFailed(message)
        }
        lock.lock()
        let old = handle
        handle = newHandle
        lock.unlock()
        sieda_project_free(old)
    }

    func snapshot() -> DesignSnapshot? {
        try? snapshotChecked().get()
    }

    /// The snapshot, or why it could not be read (a core error, or the decoder's description of the bad field).
    /// With `base` (the snapshot this engine last returned from here): only the sections that changed since then come
    /// from the core and are merged into it (`sieda_project_snapshot_delta`); large boards decode a fraction per edit.
    func snapshotChecked(base: DesignSnapshot? = nil, delta: Bool = false) -> Result<DesignSnapshot, EDAEngineError> {
        let json = withHandle { delta ? Self.take(sieda_project_snapshot_delta($0, base == nil ? 1 : 0))
                                      : Self.take(sieda_project_snapshot($0)) }
        guard let json, let data = json.data(using: .utf8) else { return .failure(.operationFailed("no reply from the core")) }
        do {
            let decoder = JSONDecoder()
            if delta, let base { decoder.userInfo[DesignSnapshot.baseKey] = base }
            return .success(try decoder.decode(DesignSnapshot.self, from: data))
        } catch {
            if let failure = try? JSONDecoder().decode(CoreError.self, from: data) { return .failure(.operationFailed(failure.error)) }
            return .failure(.operationFailed("unreadable design snapshot: \(error)"))
        }
    }

    static func libraryJSON() -> String {
        take(sieda_library_json()) ?? "[]"
    }

    // MARK: - Schematic editing

    @discardableResult
    func addComponent(_ kind: ComponentKind, value: String? = nil, at point: CGPoint, rotation: Int = 0,
                      ref: String? = nil) -> Int {
        let v = value ?? kind.defaultValue
        let r = ref ?? ""
        return Int(withHandle {
            sieda_add_component($0, Int32(kind.rawValue), v, Double(point.x), Double(point.y), Int32(rotation), r)
        })
    }

    @discardableResult
    func removeComponent(_ id: Int) -> Bool { withHandle { sieda_remove_component($0, Int32(id)) } == 1 }

    @discardableResult
    func moveComponent(_ id: Int, to point: CGPoint) -> Bool {
        withHandle { sieda_move_component($0, Int32(id), Double(point.x), Double(point.y)) } == 1
    }

    @discardableResult
    func rotateComponent(_ id: Int, by degrees: Int = 90) -> Bool {
        withHandle { sieda_rotate_component($0, Int32(id), Int32(degrees)) } == 1
    }

    @discardableResult
    func setValue(_ id: Int, _ value: String) -> Bool {
        withHandle { sieda_set_component_value($0, Int32(id), value) } == 1
    }

    /// Package variant of a passive / diode ("" = the default footprint).
    @discardableResult
    func setPackage(_ id: Int, _ package: String) -> Bool {
        withHandle { sieda_set_component_package($0, Int32(id), package) } == 1
    }

    @discardableResult
    func setRef(_ id: Int, _ ref: String) -> Bool {
        withHandle { sieda_set_component_ref($0, Int32(id), ref) } == 1
    }

    func findComponent(ref: String) -> Int? {
        let id = withHandle { sieda_find_component($0, ref) }
        return id >= 0 ? Int(id) : nil
    }

    func findPin(component: Int, name: String) -> Int? {
        let index = withHandle { sieda_find_pin($0, Int32(component), name) }
        return index >= 0 ? Int(index) : nil
    }

    @discardableResult
    func connect(_ a: PinAddress, _ b: PinAddress) -> Int? {
        let id = withHandle { sieda_connect($0, Int32(a.component), Int32(a.pin), Int32(b.component), Int32(b.pin)) }
        return id >= 0 ? Int(id) : nil
    }

    /// Bill of materials: grouped lines, descriptions, ratings, sourcing, suggestions and cost totals.
    func bom() -> BomReport {
        Self.decode(BomReport.self, from: withHandle { Self.take(sieda_bom_json($0)) }) ?? .empty
    }

    @discardableResult
    func setSourcing(component: Int, _ update: SourcingUpdate) -> Bool {
        withHandle { sieda_set_component_sourcing($0, Int32(component), update.json) } == 1
    }

    func setBuildQuantity(_ quantity: Int) { withHandle { sieda_set_build_quantity($0, Int32(max(1, quantity))) } }

    /// Writes the complete fabrication package (Gerbers, drills, job file, IPC netlist, paste, assembly files, notes and
    /// the Gerber zip) into `folder`, naming the files after `base`.
    func writeFabricationPackage(to folder: URL, base: String) -> FabricationPackageResult {
        let json = withHandle { Self.take(sieda_write_fabrication_package($0, folder.path, base)) }
        return Self.decode(FabricationPackageResult.self, from: json)
            ?? FabricationPackageResult(ok: false, files: [], error: "The core returned no result")
    }

    @discardableResult
    func removeWire(_ id: Int) -> Bool { withHandle { sieda_remove_wire($0, Int32(id)) } == 1 }

    /// Splits a wire with a junction at `point` (T-junction or bend point); returns the junction's component id.
    @discardableResult
    func splitWire(_ id: Int, at point: CGPoint) -> Int? {
        let j = withHandle { sieda_split_wire($0, Int32(id), Double(point.x), Double(point.y)) }
        return j >= 0 ? Int(j) : nil
    }

    /// Removes a chain of dangling junctions (a wire abandoned half-way) starting at `junction`.
    @discardableResult
    func removeDanglingJunctions(from junction: Int) -> Int {
        Int(withHandle { sieda_remove_dangling_junctions($0, Int32(junction)) })
    }

    /// Marks a pin as intentionally unconnected (ERC no longer reports it) or clears the mark.
    @discardableResult
    func setPinNoConnect(_ pin: PinAddress, _ noConnect: Bool) -> Bool {
        withHandle { sieda_set_pin_no_connect($0, Int32(pin.component), Int32(pin.pin), noConnect ? 1 : 0) } == 1
    }

    // MARK: - Sheets, buses, annotation

    /// Adds a sheet under `parent` (0 = top level); nil when the name is empty or taken.
    func addSheet(_ name: String, parent: Int = 0) -> Int? {
        let id = withHandle { sieda_add_sheet($0, name, Int32(parent)) }
        return id >= 0 ? Int(id) : nil
    }

    @discardableResult
    func renameSheet(_ id: Int, to name: String) -> Bool { withHandle { sieda_rename_sheet($0, Int32(id), name) } == 1 }

    @discardableResult
    func setSheetParent(_ id: Int, parent: Int) -> Bool {
        withHandle { sieda_set_sheet_parent($0, Int32(id), Int32(parent)) } == 1
    }

    @discardableResult
    func removeSheet(_ id: Int, deleteContents: Bool) -> Bool {
        withHandle { sieda_remove_sheet($0, Int32(id), deleteContents ? 1 : 0) } == 1
    }

    /// The sheet new components are placed on.
    @discardableResult
    func setActiveSheet(_ id: Int) -> Bool { withHandle { sieda_set_active_sheet($0, Int32(id)) } == 1 }

    /// Moves components to another sheet; returns how many moved (junctions that follow them included).
    @discardableResult
    func moveToSheet(_ ids: [Int], sheet: Int) -> Int {
        let list = ids.map { Int32($0) }
        return Int(withHandle { handle in
            list.withUnsafeBufferPointer { sieda_move_to_sheet(handle, $0.baseAddress, Int32($0.count), Int32(sheet)) }
        })
    }

    /// Net label scope: "global", "local", "port" or "entry" (into `targetSheet`).
    @discardableResult
    func setLabelScope(_ id: Int, scope: String, targetSheet: Int = 0) -> Bool {
        withHandle { sieda_set_label_scope($0, Int32(id), scope, Int32(targetSheet)) } == 1
    }

    /// Adds the missing sheet entries of `child`'s sheet symbol on its parent sheet; returns the entries added.
    @discardableResult
    func placeSheetEntries(child: Int, at point: CGPoint) -> Int {
        Int(withHandle { sieda_place_sheet_entries($0, Int32(child), Double(point.x), Double(point.y)) })
    }

    /// Bus members of "D[0..7]"-style notation (empty when the text is not a bus).
    static func expandBus(_ text: String) -> [String] {
        decode([String].self, from: take(sieda_expand_bus(text))) ?? []
    }

    /// One net label per bus member on `pins` of a component; returns the labels added (nil on a mismatch).
    @discardableResult
    func addBusLabels(component: Int, pins: [Int], bus: String, scope: String = "global") -> Int? {
        let list = pins.map { Int32($0) }
        let added = withHandle { handle in
            list.withUnsafeBufferPointer { sieda_add_bus_labels(handle, Int32(component), $0.baseAddress, Int32($0.count), bus, scope) }
        }
        return added >= 0 ? Int(added) : nil
    }

    /// Re-numbers designators; returns the number changed.
    @discardableResult
    func annotate(byColumns: Bool, keepExisting: Bool, sheetNumbering: Bool, packUnits: Bool = false) -> Int {
        let options = "{\"order\":\"\(byColumns ? "columns" : "rows")\",\"keepExisting\":\(keepExisting),\"sheetNumbering\":\(sheetNumbering),\"packUnits\":\(packUnits)}"
        struct Reply: Decodable { struct Change: Decodable { let component: Int }; let changed: [Change] }
        return Self.decode(Reply.self, from: withHandle { Self.take(sieda_annotate($0, options)) })?.changed.count ?? 0
    }

    // MARK: - Repeated sheets

    /// Uses a sheet `count` times (channels, the sheet itself first); the number of channels, nil when it cannot be.
    @discardableResult
    func repeatSheet(_ id: Int, count: Int) -> Int? {
        let n = withHandle { sieda_repeat_sheet($0, Int32(id), Int32(count)) }
        return n > 0 ? Int(n) : nil
    }

    /// Channel designators of a repeated sheet: "sheet" (R201, R301…) or "suffix" (R1_A, R1_B…).
    @discardableResult
    func setInstanceRefs(_ id: Int, scheme: String) -> Bool {
        withHandle { sieda_set_instance_refs($0, Int32(id), scheme) } == 1
    }

    @discardableResult
    func setSheetChannel(_ id: Int, channel: String) -> Bool {
        withHandle { sieda_set_sheet_channel($0, Int32(id), channel) } == 1
    }

    /// Gate swap between two placed units of interchangeable gates.
    @discardableResult
    func swapUnits(_ a: Int, _ b: Int) -> Bool {
        withHandle { sieda_swap_units($0, Int32(a), Int32(b)) } == 1
    }

    /// Pin swap inside a unit (pins of one pin-swap group exchange their wires).
    @discardableResult
    func swapPins(_ component: Int, _ a: Int, _ b: Int) -> Bool {
        withHandle { sieda_swap_pins($0, Int32(component), Int32(a), Int32(b)) } == 1
    }

    /// Value of one channel of a repeated sheet's part ("" = the block's value again).
    @discardableResult
    func setChannelValue(_ id: Int, _ value: String) -> Bool {
        withHandle { sieda_set_channel_value($0, Int32(id), value) } == 1
    }

    @discardableResult
    func setChannelPackage(_ id: Int, _ package: String) -> Bool {
        withHandle { sieda_set_channel_package($0, Int32(id), package) } == 1
    }

    @discardableResult
    func clearChannelOverrides(_ id: Int) -> Bool {
        withHandle { sieda_clear_channel_overrides($0, Int32(id)) } == 1
    }

    /// One per-channel parameter ("value", "package", "spice", "firmware") back to the block's.
    @discardableResult
    func clearChannelOverride(_ id: Int, _ what: String) -> Bool {
        withHandle { sieda_clear_channel_override($0, Int32(id), what) } == 1
    }

    /// Fitted or DNP in this channel only.
    @discardableResult
    func setChannelFitted(_ id: Int, _ fitted: Bool) -> Bool {
        withHandle { sieda_set_channel_fitted($0, Int32(id), fitted ? 1 : 0) } == 1
    }

    /// This channel's own SPICE model (checked like setSpiceModel; an empty `text` = no model in this channel).
    func setChannelSpiceModel(_ id: Int, text: String, model: String, pins: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let ok = withHandle { sieda_set_channel_spice_model($0, Int32(id), text, model, pins, &errorPointer) } == 1
        if !ok { throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "The model could not be attached.") }
    }

    /// This channel's own firmware (an empty `hex` = none in this channel).
    @discardableResult
    func setChannelFirmware(_ id: Int, hex: String, name: String, clockHz: Double) -> Bool {
        withHandle { sieda_set_channel_firmware($0, Int32(id), hex, name, clockHz) } == 1
    }

    /// A helper sheet below `parent` (a channel stands for its block): every channel gets a copy. Nil when refused.
    func addHelperSheet(_ name: String, parent: Int) -> Int? {
        let id = withHandle { sieda_add_helper_sheet($0, name, Int32(parent)) }
        return id > 0 ? Int(id) : nil
    }

    @discardableResult
    func setHelperSheet(_ id: Int, _ helper: Bool) -> Bool {
        withHandle { sieda_set_helper_sheet($0, Int32(id), helper ? 1 : 0) } == 1
    }

    // MARK: - Schematic directives

    private static func json(_ object: [String: Any]) -> String {
        (try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])).map { String(decoding: $0, as: UTF8.self) } ?? "{}"
    }

    /// Defines or replaces a net class (0 = the board default).
    @discardableResult
    func setNetClass(_ name: String, trackWidth: Double, clearance: Double) -> Bool {
        let body = Self.json(["name": name, "trackWidth": trackWidth, "clearance": clearance])
        return withHandle { sieda_set_net_class($0, body) } == 1
    }

    @discardableResult
    func removeNetClass(_ name: String) -> Bool { withHandle { sieda_remove_net_class($0, name) } == 1 }

    private static func directiveJSON(component: Int, pin: Int, netClass: String, diffPair: Bool, trackWidth: Double,
                                      clearance: Double) -> String {
        json(["component": component, "pin": pin, "netClass": netClass, "diffPair": diffPair, "trackWidth": trackWidth,
              "clearance": clearance])
    }

    /// Adds a directive on the net of a pin; its id, nil when refused.
    func addDirective(component: Int, pin: Int, netClass: String, diffPair: Bool, trackWidth: Double, clearance: Double) -> Int? {
        let body = Self.directiveJSON(component: component, pin: pin, netClass: netClass, diffPair: diffPair,
                                      trackWidth: trackWidth, clearance: clearance)
        let id = withHandle { sieda_add_directive($0, body) }
        return id > 0 ? Int(id) : nil
    }

    @discardableResult
    func updateDirective(_ id: Int, component: Int, pin: Int, netClass: String, diffPair: Bool, trackWidth: Double,
                         clearance: Double) -> Bool {
        let body = Self.directiveJSON(component: component, pin: pin, netClass: netClass, diffPair: diffPair,
                                      trackWidth: trackWidth, clearance: clearance)
        return withHandle { sieda_update_directive($0, Int32(id), body) } == 1
    }

    @discardableResult
    func removeDirective(_ id: Int) -> Bool { withHandle { sieda_remove_directive($0, Int32(id)) } == 1 }

    // MARK: - Signal harnesses

    /// Defines or replaces a harness type; an empty entry list removes it.
    @discardableResult
    func setHarnessType(_ name: String, entries: [String]) -> Bool {
        let json = (try? JSONSerialization.data(withJSONObject: entries)).map { String(decoding: $0, as: UTF8.self) } ?? "[]"
        return withHandle { sieda_set_harness_type($0, name, json) } == 1
    }

    /// Makes a net label a harness label of `type` ("" = an ordinary label again).
    @discardableResult
    func setLabelHarness(_ label: Int, type: String) -> Bool {
        withHandle { sieda_set_label_harness($0, Int32(label), type) } == 1
    }

    /// Harness connector on the active sheet (a harness label with one entry per member); its id, nil on failure.
    func addHarnessConnector(type: String, name: String, at point: CGPoint) -> Int? {
        let id = withHandle { sieda_add_harness_connector($0, type, name, Double(point.x), Double(point.y)) }
        return id >= 0 ? Int(id) : nil
    }

    /// Adds the missing member entries of a harness label; the number added, nil when it is not a harness.
    func placeHarnessEntries(_ label: Int) -> Int? {
        let n = withHandle { sieda_place_harness_entries($0, Int32(label)) }
        return n >= 0 ? Int(n) : nil
    }

    // MARK: - Graphical buses

    /// Draws a bus ("D[0..7]") through `points` on the active sheet; its id, nil when the name is not a bus.
    func addBus(_ name: String, points: [CGPoint]) -> Int? {
        let json = "[" + points.map { "{\"x\":\(Double($0.x)),\"y\":\(Double($0.y))}" }.joined(separator: ",") + "]"
        let id = withHandle { sieda_add_bus($0, name, json) }
        return id > 0 ? Int(id) : nil
    }

    @discardableResult
    func removeBus(_ id: Int) -> Bool { withHandle { sieda_remove_bus($0, Int32(id)) } == 1 }

    /// Makes a net label an entry of a bus on its sheet (0: an ordinary label again).
    @discardableResult
    func setLabelBus(_ label: Int, bus: Int) -> Bool { withHandle { sieda_set_label_bus($0, Int32(label), Int32(bus)) } == 1 }

    @discardableResult
    func renameBus(_ id: Int, to name: String) -> Bool { withHandle { sieda_rename_bus($0, Int32(id), name) } == 1 }

    @discardableResult
    func moveBus(_ id: Int, by delta: CGSize) -> Bool {
        withHandle { sieda_move_bus($0, Int32(id), Double(delta.width), Double(delta.height)) } == 1
    }

    /// Rips entries out of a bus for `members` (every member without one when empty); the number added.
    @discardableResult
    func ripBusEntries(_ id: Int, members: [String] = [], scope: String = "local") -> Int {
        let json = (try? JSONEncoder().encode(members)).map { String(decoding: $0, as: UTF8.self) } ?? "[]"
        return max(0, Int(withHandle { sieda_rip_bus_entries($0, Int32(id), json, scope) }))
    }

    /// Wires bus members to the pins of a part they name (else to its open pins); the connections made.
    @discardableResult
    func connectBus(_ id: Int, toPart component: Int, scope: String = "local") -> Int {
        max(0, Int(withHandle { sieda_connect_bus_to_part($0, Int32(id), Int32(component), scope) }))
    }

    // MARK: - Find / replace, net navigator, title block

    /// Finds text in designators, values, labels and nets (and pin names with `pins`) across every sheet.
    func find(_ text: String, matchCase: Bool = false, wholeWord: Bool = false, pins: Bool = false) -> [SchematicSearchHit] {
        var fields = ["ref", "value", "label", "net"]
        if pins { fields.append("pin") }
        let request: [String: Any] = ["text": text, "matchCase": matchCase, "wholeWord": wholeWord, "fields": fields]
        guard let data = try? JSONSerialization.data(withJSONObject: request) else { return [] }
        let json = String(decoding: data, as: UTF8.self)
        struct Reply: Decodable { var hits: [SchematicSearchHit] }
        return Self.decode(Reply.self, from: withHandle { Self.take(sieda_schematic_find($0, json)) })?.hits ?? []
    }

    /// Replaces text in part values and net label names; the number of fields changed.
    @discardableResult
    func replace(_ text: String, with replacement: String, matchCase: Bool = false, wholeWord: Bool = false) -> Int {
        let request: [String: Any] = ["text": text, "replacement": replacement, "matchCase": matchCase, "wholeWord": wholeWord,
                                      "fields": ["value", "label"]]
        guard let data = try? JSONSerialization.data(withJSONObject: request) else { return 0 }
        let json = String(decoding: data, as: UTF8.self)
        return Int(withHandle { sieda_schematic_replace($0, json) })
    }

    /// Every place a net appears (net navigator).
    func netPlaces(_ net: Int) -> NetPlacesReport? {
        Self.decode(NetPlacesReport.self, from: withHandle { Self.take(sieda_net_places($0, Int32(net))) })
    }

    @discardableResult
    func setTitleBlock(_ block: TitleBlockInfo) -> Bool {
        guard let data = try? JSONEncoder().encode(block) else { return false }
        let json = String(decoding: data, as: UTF8.self)
        return withHandle { sieda_set_title_block($0, json) } == 1
    }

    // MARK: - Design variants

    @discardableResult
    func addVariant(_ name: String, copying source: String? = nil) -> Bool {
        withHandle { sieda_add_variant($0, name, source ?? "") } == 1
    }

    @discardableResult
    func renameVariant(_ name: String, to newName: String) -> Bool { withHandle { sieda_rename_variant($0, name, newName) } == 1 }

    @discardableResult
    func removeVariant(_ name: String) -> Bool { withHandle { sieda_remove_variant($0, name) } == 1 }

    /// fitted: nil follows the base design. `value`: nil keeps the override, "" clears it.
    @discardableResult
    func setVariantPart(_ name: String, component: Int, fitted: Bool?, value: String? = nil) -> Bool {
        let state: Int32 = fitted.map { $0 ? 1 : 0 } ?? -1
        return withHandle { (handle) -> Int32 in
            if let value { return sieda_set_variant_part(handle, name, Int32(component), state, value) }
            return sieda_set_variant_part(handle, name, Int32(component), state, nil)
        } == 1
    }

    /// "" selects the base design; the BOM, CPL and assembly exports follow the active variant.
    @discardableResult
    func setActiveVariant(_ name: String) -> Bool { withHandle { sieda_set_active_variant($0, name) } == 1 }

    // MARK: - Custom parts

    /// Registers a part in the project library; returns the generated part (symbol + footprint).
    func registerCustomPart(_ spec: CustomPartSpec) throws -> CustomPartInfo {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let json = withHandle { Self.take(sieda_custom_part_register($0, spec.jsonString(), &errorPointer)) }
        if let message = Self.take(errorPointer) { throw EDAEngineError.operationFailed(message) }
        guard let part = Self.decode(CustomPartInfo.self, from: json) else {
            throw EDAEngineError.operationFailed("The core returned an unreadable part definition.")
        }
        return part
    }

    /// Generates symbol and footprint without adding the part to the project (live editor preview).
    static func previewCustomPart(_ spec: CustomPartSpec) -> Result<CustomPartInfo, EDAEngineError> {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let json = take(sieda_custom_part_preview(spec.jsonString(), &errorPointer))
        if let message = take(errorPointer) { return .failure(.operationFailed(message)) }
        guard let part = decode(CustomPartInfo.self, from: json) else {
            return .failure(.operationFailed("Preview unavailable."))
        }
        return .success(part)
    }

    /// Footprint editor: the part's generated footprint as an editable land pattern (package "CUSTOM", one land per
    /// pad, same pins). Parts already on a land pattern come back unchanged.
    static func landPattern(_ spec: CustomPartSpec) -> Result<CustomPartSpec, EDAEngineError> {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let json = take(sieda_custom_part_land_pattern(spec.jsonString(), &errorPointer))
        if let message = take(errorPointer) { return .failure(.operationFailed(message)) }
        guard let converted = decode(CustomPartSpec.self, from: json) else {
            return .failure(.operationFailed("Footprint unavailable."))
        }
        return .success(converted)
    }

    /// Symbol editor: the part with an auto-arranged symbol (supplies top, grounds bottom, inputs left, outputs right,
    /// ports grouped; repeated supply / ground pins stacked when `stack`).
    static func autoArrangeSymbol(_ spec: CustomPartSpec, stack: Bool = true) -> Result<CustomPartSpec, EDAEngineError> {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let json = take(sieda_symbol_auto_arrange(spec.jsonString(), stack ? 1 : 0, &errorPointer))
        if let message = take(errorPointer) { return .failure(.operationFailed(message)) }
        guard let arranged = decode(CustomPartSpec.self, from: json) else {
            return .failure(.operationFailed("Symbol unavailable."))
        }
        return .success(arranged)
    }

    /// Symbol editor checks: pins missing or placed twice, unknown pins, different pins on one spot (errors), stacked
    /// signal pins (warnings), stacked pins (info).
    static func checkSymbol(_ spec: CustomPartSpec) -> [SymbolIssue] {
        decode([SymbolIssue].self, from: take(sieda_check_symbol(spec.jsonString()))) ?? []
    }

    /// Unit (gate) editor checks of a multi-unit spec (codes UNIT_*; same shape as the symbol checks).
    static func checkUnits(_ spec: CustomPartSpec) -> [SymbolIssue] {
        decode([SymbolIssue].self, from: take(sieda_check_units(spec.jsonString()))) ?? []
    }

    /// Footprint editor checks: overlapping pads, copper gaps below `minGap` mm, annular rings, pads without pins and
    /// pins without pads.
    static func checkLandPattern(_ spec: CustomPartSpec, minGap: Double = 0.1) -> [LandIssue] {
        decode([LandIssue].self, from: take(sieda_check_land_pattern(spec.jsonString(), minGap))) ?? []
    }

    /// Library import: reads KiCad footprints (.kicad_mod), KiCad symbol libraries (.kicad_sym) and Eagle libraries
    /// (.lbr), pairs symbols with footprints (`pairs`: symbol name → footprint name, optional) and validates every
    /// part. Files that cannot be read are reported in `files[].error`.
    static func importLibrary(files: [LibraryImportFile], pairs: [String: String] = [:]) -> LibraryImportResult {
        struct Request: Encodable {
            var files: [LibraryImportFile]
            var pairs: [String: String]
        }
        guard let data = try? JSONEncoder().encode(Request(files: files, pairs: pairs)) else { return LibraryImportResult() }
        let json = String(decoding: data, as: UTF8.self)
        return decode(LibraryImportResult.self, from: take(sieda_library_import(json))) ?? LibraryImportResult()
    }

    @discardableResult
    func removeCustomPart(_ id: String) -> Bool { withHandle { sieda_custom_part_remove($0, id) } == 1 }

    @discardableResult
    func replaceCustomPart(_ oldId: String, with newId: String) -> Int {
        Int(withHandle { sieda_custom_part_replace($0, oldId, newId) })
    }

    @discardableResult
    func addCustomComponent(partId: String, value: String? = nil, at point: CGPoint, rotation: Int = 0,
                            ref: String? = nil) -> Int {
        let v = value ?? ""
        let r = ref ?? ""
        return Int(withHandle {
            sieda_add_custom_component($0, partId, v, Double(point.x), Double(point.y), Int32(rotation), r)
        })
    }

    /// Places unit A of a multi-unit part (with its hidden package); the unit's id, or -1.
    func addCustomUnits(partId: String, value: String? = nil, at point: CGPoint, rotation: Int = 0, ref: String? = nil) -> Int {
        let v = value ?? ""
        let r = ref ?? ""
        return Int(withHandle { sieda_add_custom_units($0, partId, v, Double(point.x), Double(point.y), Int32(rotation), r) })
    }

    /// Places the next unit not placed yet of a unit's package; nil when every unit is placed.
    func placeNextUnit(of component: Int, at point: CGPoint) -> Int? {
        let id = withHandle { sieda_place_next_unit($0, Int32(component), Double(point.x), Double(point.y)) }
        return id >= 0 ? Int(id) : nil
    }

    /// Places unit `unit` (1-based) of a unit's package; nil when it is placed already or out of range.
    func addPartUnit(of component: Int, unit: Int, at point: CGPoint, rotation: Int = 0) -> Int? {
        let id = withHandle {
            sieda_add_part_unit($0, Int32(component), Int32(unit), Double(point.x), Double(point.y), Int32(rotation))
        }
        return id >= 0 ? Int(id) : nil
    }

    // MARK: - Standards

    /// Built-in standard parts (regulators, timers, op-amps, MCUs, logic, headers).
    static func standardParts() -> [StandardPart] {
        decode([StandardPart].self, from: take(sieda_standard_parts_json())) ?? []
    }

    static func industryProfiles() -> [IndustryProfile] {
        decode([IndustryProfile].self, from: take(sieda_industry_profiles_json())) ?? []
    }

    /// Applies an industry profile (rule preset, altitude class, derating). False for an unknown id.
    @discardableResult
    func setIndustry(_ id: String) -> Bool { withHandle { sieda_project_set_industry($0, id) } == 1 }

    /// Robot platform ("rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"; "" = none).
    @discardableResult
    func setRobotPlatform(_ id: String) -> Bool { withHandle { sieda_set_robot_platform($0, id) } == 1 }

    func robotSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_robot_segments_json($0)) }) ?? .empty
    }

    /// Automotive ECU type ("bcm", "powertrain", "adas", "ev", "chassis", "gateway"; "" = none).
    @discardableResult
    func setEcuType(_ id: String) -> Bool { withHandle { sieda_set_ecu_type($0, id) } == 1 }

    func ecuSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_ecu_segments_json($0)) }) ?? .empty
    }

    /// Aerospace mission ("leo", "geo", "launcher", "military", "commercial"; "" = none).
    @discardableResult
    func setAerospaceMission(_ id: String) -> Bool { withHandle { sieda_set_aerospace_mission($0, id) } == 1 }

    /// Medical device class ("bf", "cf", "life", "implant", "home"; "" = none).
    @discardableResult
    func setMedicalClass(_ id: String) -> Bool { withHandle { sieda_set_medical_class($0, id) } == 1 }

    func medicalSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_medical_segments_json($0)) }) ?? .empty
    }

    /// Retail device class ("countertop", "unattended", "mpos", "kiosk", "printer"; "" = none).
    @discardableResult
    func setRetailDevice(_ id: String) -> Bool { withHandle { sieda_set_retail_device($0, id) } == 1 }

    func retailSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_retail_segments_json($0)) }) ?? .empty
    }

    /// Home appliance type ("laundry", "kitchen", "refrigeration", "hvac", "small"; "" = none).
    @discardableResult
    func setApplianceType(_ id: String) -> Bool { withHandle { sieda_set_appliance_type($0, id) } == 1 }

    func applianceSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_appliance_segments_json($0)) }) ?? .empty
    }

    /// Memory design type ("sdram", "ddr", "lpddr", "dimm", "rdimm"; "" = none).
    @discardableResult
    func setMemoryDesign(_ id: String) -> Bool { withHandle { sieda_set_memory_design($0, id) } == 1 }

    func memorySegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_memory_segments_json($0)) }) ?? .empty
    }

    /// Active tamper mesh over the part `ref` (laid by the autorouter on two inner layers); nil if rejected.
    func addTamperMesh(component ref: String, netA: String, netB: String, layerA: Int = 1, layerB: Int = 2,
                       margin: Double = 2) -> Int? {
        let index = withHandle { sieda_pcb_add_tamper_mesh($0, ref, netA, netB, Int32(layerA), Int32(layerB), margin) }
        return index >= 0 ? Int(index) : nil
    }

    func clearTamperMeshes() { withHandle { sieda_pcb_clear_tamper_meshes($0) } }

    /// Naval platform ("combatant", "carrier", "submarine", "patrol", "commercial"; "" = none).
    @discardableResult
    func setNavalPlatform(_ id: String) -> Bool { withHandle { sieda_set_naval_platform($0, id) } == 1 }

    func navalSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_naval_segments_json($0)) }) ?? .empty
    }

    /// Board thickness (mm, ≤ 0 keeps it) and underfill / corner bonding of heavy parts.
    @discardableResult
    func setMechanical(thickness: Double, underfill: Bool) -> Bool {
        withHandle { sieda_pcb_set_mechanical($0, thickness, underfill ? 1 : 0) } == 1
    }

    /// Isolation barrier spacing between galvanic domains (mm, 0 = none).
    @discardableResult
    func setIsolationGap(_ gap: Double) -> Bool { withHandle { sieda_pcb_set_isolation_gap($0, gap) } == 1 }

    func aerospaceSegments() -> RobotSegmentsReport {
        Self.decode(RobotSegmentsReport.self, from: withHandle { Self.take(sieda_aerospace_segments_json($0)) }) ?? .empty
    }

    /// Stitches thermal vias at a power part's drain / tab pad; returns the vias added.
    @discardableResult
    func addThermalVias(_ id: Int) -> Int { Int(withHandle { sieda_pcb_add_thermal_vias($0, Int32(id)) }) }

    static func designRulePresets() -> [DesignRulePreset] {
        decode([DesignRulePreset].self, from: take(sieda_design_rule_presets_json())) ?? []
    }

    static func nearestStandardValue(_ value: Double, series: ESeries) -> Double {
        sieda_nearest_standard_value(value, Int32(series.rawValue))
    }

    /// Engineering value as the core reads it ("4k7" → 4700, "100nF" → 1e-7, "2R2" → 2.2).
    static func parseValue(_ text: String) -> Double? {
        var value = 0.0
        return sieda_parse_value(text, &value) == 1 ? value : nil
    }

    static func isStandardValue(_ value: Double, series: ESeries) -> Bool {
        sieda_is_standard_value(value, Int32(series.rawValue)) == 1
    }

    @discardableResult
    func applyRulePreset(_ name: String) -> Bool { withHandle { sieda_pcb_apply_rule_preset($0, name) } == 1 }

    // MARK: - Analysis

    func runERC() -> [RuleViolation] {
        (Self.decode([RuleViolation].self, from: withHandle { Self.take(sieda_run_erc($0)) }) ?? []).numbered()
    }

    /// Standard values, decoupling and DC-derived part ratings.
    func runCircuitValidation() -> [RuleViolation] {
        (Self.decode([RuleViolation].self, from: withHandle { Self.take(sieda_run_circuit_validation($0)) }) ?? []).numbered()
    }

    /// Full sign-off: ERC, DC, validation, placement, routing, DRC and manufacturing outputs.
    func runVerification() -> VerificationReport? {
        Self.decode(VerificationReport.self, from: withHandle { Self.take(sieda_run_verification($0)) })
    }

    /// Stop flag for running simulations (`sieda_simulation_stop`); clear it before the next run.
    static func stopSimulation(_ stop: Bool) { sieda_simulation_stop(stop ? 1 : 0) }

    func simulateDC() -> DCResult {
        let json = withHandle { Self.take(sieda_simulate_dc($0)) }
        return Self.decode(DCResult.self, from: json) ?? DCResult(error: "Simulator returned no result.")
    }

    // MARK: - Microcontroller firmware

    /// Attaches Intel HEX firmware to a microcontroller (an empty `hex` removes it); `clockHz` 0 = the model default.
    func setFirmware(_ id: Int, hex: String, name: String, clockHz: Double) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let ok = withHandle { sieda_set_firmware($0, Int32(id), hex, name, clockHz, &errorPointer) } == 1
        if !ok { throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "The firmware could not be attached.") }
    }

    /// The component's firmware as Intel HEX ("" when none).
    func firmware(of id: Int) -> String {
        withHandle { Self.take(sieda_component_firmware($0, Int32(id))) } ?? ""
    }

    /// Starts a live simulation of the current schematic (a snapshot; restart after edits).
    func startLive() throws -> LiveSession {
        var errorPointer: UnsafeMutablePointer<CChar>?
        guard let session = withHandle({ sieda_live_start($0, &errorPointer) }) else {
            throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "The live simulation could not start.")
        }
        return LiveSession(session)
    }

    static let firmwareExamples: [FirmwareExample] =
        decode([FirmwareExample].self, from: take(sieda_firmware_examples_json())) ?? []

    static func firmwareExampleHex(_ id: String) -> String? { take(sieda_firmware_example_hex(id)) }

    func simulateTransient(stop: Double, step: Double) -> TransientResult {
        let json = withHandle { Self.take(sieda_simulate_transient($0, stop, step)) }
        return Self.decode(TransientResult.self, from: json) ?? TransientResult(error: "Simulator returned no result.")
    }

    // MARK: - AC, DC sweep and tolerance analyses (values are engineering text, parsed by the core: "10", "1MEG")

    /// AC small-signal sweep from `start` to `stop` Hz; `source` is the input's reference ("" = the source with an
    /// "AC" value, else the first SIN source).
    func simulateAC(start: String, stop: String, pointsPerDecade: Int, source: String) -> ACResult {
        var options: [String: Any] = ["start": start, "stop": stop, "pointsPerDecade": pointsPerDecade]
        if !source.isEmpty { options["source"] = source }
        let json = withHandle { Self.take(sieda_simulate_ac($0, Self.optionsJSON(options))) }
        return Self.decode(ACResult.self, from: json) ?? ACResult(error: "Simulator returned no result.")
    }

    /// DC sweep of source `source` (a reference such as "V1") from `start` to `stop` in steps of `step`.
    func simulateDCSweep(source: String, start: String, stop: String, step: String) -> DCSweepResult {
        let options: [String: Any] = ["source": source, "start": start, "stop": stop, "step": step]
        let json = withHandle { Self.take(sieda_simulate_dc_sweep($0, Self.optionsJSON(options))) }
        return Self.decode(DCSweepResult.self, from: json) ?? DCSweepResult(error: "Simulator returned no result.")
    }

    /// Monte Carlo + worst case of `net`: `measure` "dc" (voltage) or "f3db" (AC bandwidth, swept 1 Hz – 100 MHz).
    func simulateMonteCarlo(net: String, measure: String, runs: Int, seed: Int) -> MonteCarloResult {
        var options: [String: Any] = ["net": net, "measure": measure, "runs": runs, "seed": seed]
        if measure != "dc" {
            options["start"] = 1
            options["stop"] = 1e8
        }
        let json = withHandle { Self.take(sieda_simulate_monte_carlo($0, Self.optionsJSON(options))) }
        return Self.decode(MonteCarloResult.self, from: json) ?? MonteCarloResult(error: "Simulator returned no result.")
    }

    // MARK: - Transient options, noise, parameter sweep, FFT (docs/SIMULATION.md)

    /// Transient with the trapezoidal rule and / or adaptive (LTE-controlled) time steps; both off is the
    /// established fixed-step backward-Euler analysis.
    func simulateTransient(stop: Double, step: Double, adaptive: Bool, trapezoidal: Bool) -> TransientResult {
        guard adaptive || trapezoidal else { return simulateTransient(stop: stop, step: step) }
        let options: [String: Any] = ["stop": stop, "step": step, "adaptive": adaptive, "method": trapezoidal ? "trap" : "be"]
        let json = withHandle { Self.take(sieda_simulate_transient_ex($0, Self.optionsJSON(options))) }
        return Self.decode(TransientResult.self, from: json) ?? TransientResult(error: "Simulator returned no result.")
    }

    /// Output noise of `output` over a log sweep; `source` is the input for input-referred noise ("" = automatic).
    func simulateNoise(output: String, start: String, stop: String, pointsPerDecade: Int, source: String) -> NoiseAnalysisResult {
        var options: [String: Any] = ["output": output, "start": start, "stop": stop, "pointsPerDecade": pointsPerDecade]
        if !source.isEmpty { options["source"] = source }
        let json = withHandle { Self.take(sieda_simulate_noise($0, Self.optionsJSON(options))) }
        return Self.decode(NoiseAnalysisResult.self, from: json) ?? NoiseAnalysisResult(error: "Simulator returned no result.")
    }

    /// Runs `analysis` ("dc", "ac", "transient") once per value of `component`. AC uses start / stop; transient
    /// stop / step; `net` limits the result to one node ("" = every node).
    func simulateParamSweep(component: String, values: [String], analysis: String, net: String,
                            start: String, stop: String, step: String) -> ParamSweepResult {
        var options: [String: Any] = ["component": component, "values": values, "analysis": analysis]
        if !net.isEmpty { options["net"] = net }
        if analysis == "ac" {
            options["start"] = start
            options["stop"] = stop
            options["pointsPerDecade"] = 20
        } else if analysis == "transient" {
            options["stop"] = stop
            options["step"] = step
        }
        let json = withHandle { Self.take(sieda_simulate_param_sweep($0, Self.optionsJSON(options))) }
        return Self.decode(ParamSweepResult.self, from: json) ?? ParamSweepResult(error: "Simulator returned no result.")
    }

    /// Spectrum and THD of `net` from a transient of `stop` / `step` ("" fundamental = the first SIN source).
    func simulateFFT(net: String, stop: String, step: String, fundamental: String, harmonics: Int) -> FFTResult {
        var options: [String: Any] = ["net": net, "stop": stop, "step": step, "harmonics": harmonics]
        if !fundamental.isEmpty { options["fundamental"] = fundamental }
        let json = withHandle { Self.take(sieda_simulate_fft($0, Self.optionsJSON(options))) }
        return Self.decode(FFTResult.self, from: json) ?? FFTResult(error: "Simulator returned no result.")
    }

    /// Measures a waveform between `from` and `to` (nil: its ends): extremes, average, RMS, edges, period.
    static func measureWaveform(time: [Double], values: [Double], from: Double? = nil, to: Double? = nil) -> WaveformMeasurementsInfo {
        var request: [String: Any] = ["time": time.map { $0.isFinite ? $0 : 0 }, "values": values.map { $0.isFinite ? $0 : 0 }]
        if let from, from.isFinite { request["from"] = from }
        if let to, to.isFinite { request["to"] = to }
        return decode(WaveformMeasurementsInfo.self, from: take(sieda_measure_waveform(optionsJSON(request))))
            ?? WaveformMeasurementsInfo(ok: false, error: "The waveform could not be measured.")
    }

    // MARK: - SPICE models (docs/SIMULATION.md)

    /// Models and subcircuits in vendor model text, with the parser's diagnostics.
    static func parseSpice(_ text: String) -> SpiceParseResult {
        decode(SpiceParseResult.self, from: take(sieda_spice_parse(text))) ?? SpiceParseResult()
    }

    /// Tries `model` of `text` on a part without changing the design (ports, default pin map, problems).
    func checkSpiceModel(_ id: Int, text: String, model: String, pins: String) -> SpiceCheckResult {
        let json = withHandle { Self.take(sieda_spice_check($0, Int32(id), text, model, pins)) }
        return Self.decode(SpiceCheckResult.self, from: json) ?? SpiceCheckResult(ok: false, error: "The model could not be checked.")
    }

    /// Attaches a model to a part (an empty `text` removes it); the core stores what the model needs.
    func setSpiceModel(_ id: Int, text: String, model: String, pins: String) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let ok = withHandle { sieda_set_spice_model($0, Int32(id), text, model, pins, &errorPointer) } == 1
        if !ok { throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "The model could not be attached.") }
    }

    /// The model attached to a part (nil when none).
    func spiceModel(of id: Int) -> SpiceModelText? {
        let model = Self.decode(SpiceModelText.self, from: withHandle { Self.take(sieda_component_spice_model($0, Int32(id))) })
        return model?.text.isEmpty == false ? model : nil
    }

    static let builtinSpiceModels: [SpiceBuiltinModel] =
        decode([SpiceBuiltinModel].self, from: take(sieda_spice_builtin_models())) ?? []

    private static func optionsJSON(_ options: [String: Any]) -> String {
        guard let data = try? JSONSerialization.data(withJSONObject: options) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }

    // MARK: - PCB

    func setLayerCount(_ layers: Int) { withHandle { sieda_pcb_set_layer_count($0, Int32(layers)) } }

    func setBoard(width: Double, height: Double, trackWidth: Double, clearance: Double) {
        withHandle { sieda_pcb_set_board($0, width, height, trackWidth, clearance) }
    }

    func autoPlace(all: Bool) { withHandle { sieda_pcb_autoplace($0, all ? 1 : 0) } }

    @discardableResult
    func moveFootprint(_ id: Int, to point: CGPoint) -> Bool {
        withHandle { sieda_pcb_move_footprint($0, Int32(id), Double(point.x), Double(point.y)) } == 1
    }

    /// Forms a resistor / capacitor inside the board on inner layer `layer` (0 = surface part again).
    @discardableResult
    func setEmbedded(_ id: Int, layer: Int) -> Bool {
        withHandle { sieda_set_component_embedded($0, Int32(id), Int32(layer)) } == 1
    }

    /// Locks a placed footprint so Auto Place keeps it.
    @discardableResult
    func lockFootprint(_ id: Int, _ locked: Bool) -> Bool {
        withHandle { sieda_pcb_lock_footprint($0, Int32(id), locked ? 1 : 0) } == 1
    }

    @discardableResult
    func rotateFootprint(_ id: Int, by degrees: Int = 90) -> Bool {
        withHandle { sieda_pcb_rotate_footprint($0, Int32(id), Int32(degrees)) } == 1
    }

    @discardableResult
    func flipFootprint(_ id: Int) -> Bool { withHandle { sieda_pcb_flip_footprint($0, Int32(id)) } == 1 }

    @discardableResult
    func fitBoard(margin: Double) -> Bool { withHandle { sieda_pcb_fit_board($0, margin) } == 1 }

    func autoRoute() -> RouteStats {
        (try? autoRouteChecked().get()) ?? RouteStats()
    }

    /// Autoroutes, or reports the core's error message (instead of an empty "0/0 routed" result).
    func autoRouteChecked() -> Result<RouteStats, EDAEngineError> {
        Self.decodeChecked(RouteStats.self, from: withHandle { Self.take(sieda_pcb_autoroute($0)) })
    }

    /// Autoroute with progress and cancel. `channel` receives progress on the routing thread; after
    /// `channel.cancel()` the route stops at its next report and leaves the project as it was
    /// (`RouteStats.cancelled`).
    func autoRouteChecked(progress channel: RouteProgressChannel) -> Result<RouteStats, EDAEngineError> {
        let context = Unmanaged.passRetained(channel)
        defer { context.release() }
        let json = withHandle { handle in
            Self.take(sieda_pcb_autoroute_progress(handle, { user, phase, pass, done, total, unrouted in
                guard let user else { return 0 }
                let channel = Unmanaged<RouteProgressChannel>.fromOpaque(user).takeUnretainedValue()
                let report = RouteProgressReport(phase: Int(phase), pass: Int(pass), done: Int(done), total: Int(total),
                                                 unrouted: Int(unrouted))
                return channel.deliver(report) ? 1 : 0
            }, context.toOpaque()))
        }
        return Self.decodeChecked(RouteStats.self, from: json)
    }

    /// Autorouter strategy for the whole process (tests and benchmarks): 0 automatic, 1 classic, 2 corridor router.
    static func setRouterStrategy(_ strategy: Int) { _ = sieda_router_set_strategy(Int32(strategy)) }
    /// Threads of the corridor router (0 = the machine's cores, at most 8); the copper does not depend on it.
    static func setRoutingThreads(_ threads: Int) { sieda_router_set_threads(Int32(threads)) }

    func clearRouting() { withHandle { sieda_pcb_clear_routing($0) } }

    // MARK: - Net classes, outline, holes, pours

    /// Net class track width in mm (0 removes the class).
    @discardableResult
    func setNetWidth(_ net: String, width: Double) -> Bool {
        withHandle { sieda_pcb_set_net_width($0, net, width) } == 1
    }

    /// Sizes net classes from the DC operating point (IPC-2221 + 25 %); returns the widths it set.
    @discardableResult
    func autoNetWidths() -> [String: Double] {
        Self.decode([String: Double].self, from: withHandle { Self.take(sieda_pcb_auto_net_widths($0)) }) ?? [:]
    }

    func setAutoSizeNets(_ enabled: Bool) { withHandle { sieda_pcb_set_auto_size_nets($0, enabled ? 1 : 0) } }

    @discardableResult
    func setCoating(_ coating: ConformalCoating) -> Bool { withHandle { sieda_pcb_set_coating($0, coating.rawValue) } == 1 }

    /// Laminate, construction, impedance targets (Ω) and backdrilling.
    @discardableResult
    func setStackup(material: String, construction: BoardConstruction, singleEnded: Double, differential: Double,
                    backdrill: Bool) -> Bool {
        withHandle {
            sieda_pcb_set_stackup($0, material, construction.rawValue, singleEnded, differential, backdrill ? 1 : 0)
        } == 1
    }

    @discardableResult
    func setLengthMatching(enabled: Bool, pairSkew: Double, bus: Double) -> Bool {
        withHandle { sieda_pcb_set_length_matching($0, enabled ? 1 : 0, pairSkew, bus) } == 1
    }

    @discardableResult
    func setHDI(enabled: Bool, microviaDrill: Double, microviaDiameter: Double, viaInPad: Bool) -> Bool {
        withHandle { sieda_pcb_set_hdi($0, enabled ? 1 : 0, microviaDrill, microviaDiameter, viaInPad ? 1 : 0) } == 1
    }

    /// Cuts the routed vias to the layers they connect (blind / buried / microvias); returns the vias changed.
    @discardableResult
    func applyHDI() -> Int { Int(withHandle { sieda_pcb_apply_hdi($0) }) }

    /// Adds serpentines to the short members of differential pairs and buses; returns the nets tuned.
    @discardableResult
    func tuneLengths() -> Int { Int(withHandle { sieda_pcb_tune_lengths($0) }) }

    func lengthReport() -> LengthReport {
        Self.decode(LengthReport.self, from: withHandle { Self.take(sieda_length_report_json($0)) }) ?? .empty
    }

    // MARK: - Interactive routing

    /// Options for `sieda_router_*`: push-and-shove or walkaround, 45° or 90° corners.
    static func routerOptions(shove: Bool, diagonal: Bool) -> String {
        routerOptions(mode: shove ? .shove : .walkaround, diagonal: diagonal)
    }

    /// Options for `sieda_router_*` with any router mode (Highlight lets the head go anywhere and lists collisions).
    /// `rounded`: corners become arcs (short chords) of the automatic radius where they fit (single tracks).
    static func routerOptions(mode: RouterModeChoice, diagonal: Bool, via: RouterViaChoice = .through,
                              rounded: Bool = false) -> String {
        "{\"mode\":\"\(mode.rawValue)\",\"posture\":\"\(diagonal ? "45" : "90")\",\"viaType\":\"\(via.rawValue)\","
            + "\"cornerRadius\":\(rounded ? -1 : 0)}"
    }

    /// Starts a route (or a differential pair) on the pad, via or track at `point`; the preview carries `error` when
    /// there is nothing to start from.
    func routerBegin(at point: CGPoint, layer: Int, pair: Bool, options: String) -> RoutePreview? {
        let json = withHandle { handle in
            Self.take(pair ? sieda_router_begin_pair(handle, options, Double(point.x), Double(point.y), Int32(layer))
                           : sieda_router_begin(handle, options, Double(point.x), Double(point.y), Int32(layer)))
        }
        return Self.decode(RoutePreview.self, from: json)
    }

    /// Drags track segment `trackId` grabbed at `point` (it moves parallel to itself; other nets are shoved).
    func routerBeginDrag(track trackId: Int, at point: CGPoint, options: String) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_drag($0, options, Int32(trackId), Double(point.x), Double(point.y)))
        })
    }

    /// Drags via `viaId` grabbed at `point`; the tracks ending on it follow.
    func routerBeginViaDrag(via viaId: Int, at point: CGPoint, options: String) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_via_drag($0, options, Int32(viaId), Double(point.x), Double(point.y)))
        })
    }

    /// Length tuning of the net of `trackId`: a preview (`apply` false) or the change itself. `target` 0 = the
    /// longest member of the net's pair / bus group; `amplitude` / `spacing` 0 = defaults.
    func routerTune(track trackId: Int, target: Double, amplitude: Double, spacing: Double, near point: CGPoint?,
                    apply: Bool) -> TunePreview? {
        var fields = [String(format: "\"target\":%.6f", max(0, target)),
                      String(format: "\"maxAmplitude\":%.6f", max(0, amplitude)),
                      String(format: "\"spacing\":%.6f", max(0, spacing)),
                      "\"apply\":\(apply ? "true" : "false")"]
        if let point { fields.append(String(format: "\"x\":%.6f,\"y\":%.6f", Double(point.x), Double(point.y))) }
        let options = "{" + fields.joined(separator: ",") + "}"
        return Self.decode(TunePreview.self, from: withHandle { Self.take(sieda_router_tune($0, Int32(trackId), options)) })
    }

    /// Starts a bus on the pad at `point`: it and the next pads of its part's row (up to `count` nets) route together.
    func routerBeginBus(at point: CGPoint, layer: Int, count: Int, options: String) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_bus($0, options, Double(point.x), Double(point.y), Int32(layer), Int32(count)))
        })
    }

    /// Fanout of a part: an escape track and a via on every SMD pad whose net has other pins and no copper yet.
    func fanout(component: Int, via: RouterViaChoice = .through) -> FanoutResult? {
        let options = "{\"viaType\":\"\(via.rawValue)\"}"
        return Self.decode(FanoutResult.self, from: withHandle { Self.take(sieda_pcb_fanout($0, Int32(component), options)) })
    }

    func routerMove(to point: CGPoint) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle { Self.take(sieda_router_move($0, Double(point.x), Double(point.y))) })
    }

    /// Places the head (a click): the route continues from its end.
    func routerFix() -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle { Self.take(sieda_router_fix($0)) })
    }

    /// Places a via at the head's end and continues on `layer` (nil = the default layer for the via type: the other
    /// outer layer for through vias, the next layer for blind / micro vias; `reverse` = the next layer the other way).
    func routerAddVia(toLayer layer: Int? = nil, reverse: Bool = false) -> RoutePreview? {
        let target = layer ?? (reverse ? -2 : -1)
        return Self.decode(RoutePreview.self, from: withHandle { Self.take(sieda_router_add_via($0, Int32(target))) })
    }

    func routerSetOptions(_ options: String) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle { Self.take(sieda_router_set_options($0, options)) })
    }

    /// Writes the route and the shoved copper into the board; the session ends either way.
    func routerCommit() -> RouteCommitResult {
        Self.decode(RouteCommitResult.self, from: withHandle { Self.take(sieda_router_commit($0)) })
            ?? RouteCommitResult(ok: false, error: "no reply from the core", addedTracks: [], addedVias: [])
    }

    func routerCancel() { withHandle { sieda_router_cancel($0) } }

    /// Cancels the head update running on another thread (`routerMove` off the main thread): it returns soon with
    /// the preview from before it, marked `aborted`. Lock-free, so it does not wait for that update; call it from the
    /// main thread (the only thread that replaces the project handle).
    func routerAbort() { sieda_router_abort(handle) }

    var routerActive: Bool { withHandle { sieda_router_active($0) } == 1 }

    func stackup() -> StackupReport {
        Self.decode(StackupReport.self, from: withHandle { Self.take(sieda_stackup_json($0)) }) ?? .empty
    }

    // MARK: - Signal & power integrity

    /// Imported IBIS models, logic-family defaults, model assignments, rail inputs and sign-off.
    func siSettings() -> SISettings {
        Self.decode(SISettings.self, from: withHandle { Self.take(sieda_si_settings_json($0)) }) ?? .empty
    }

    /// Signal nets for the SI panel, critical and fast nets first.
    func siNets() -> [SINetSummary] {
        Self.decode([SINetSummary].self, from: withHandle { Self.take(sieda_si_net_list_json($0)) }) ?? []
    }

    /// Transmission-line analysis of one net with its waveforms; `seriesOhms` adds a what-if series resistor.
    func siNet(_ name: String, seriesOhms: Double? = nil) -> SINetAnalysis? {
        Self.decode(SINetAnalysis.self, from: withHandle { Self.take(sieda_si_net_json($0, name, seriesOhms ?? -1)) })
    }

    func siCrosstalk() -> SICrosstalkReport {
        Self.decode(SICrosstalkReport.self, from: withHandle { Self.take(sieda_si_crosstalk_json($0)) }) ?? .empty
    }

    func pdn() -> PDNReport {
        Self.decode(PDNReport.self, from: withHandle { Self.take(sieda_pi_json($0)) }) ?? .empty
    }

    /// Imports every model of an IBIS file (corner "typ", "min" or "max"); with `ref`, maps that part's pins to them.
    @discardableResult
    func importIBIS(_ text: String, corner: String, ref: String) throws -> Int {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let count = withHandle { sieda_si_import_ibis($0, text, corner, ref, &errorPointer) }
        if count <= 0 { throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "The IBIS file could not be read.") }
        return Int(count)
    }

    /// Assigns a model id to a "net", "component" or "pin" ("U1.12"); an empty id clears the assignment.
    @discardableResult
    func assignSIModel(kind: String, target: String, modelID: String) -> Bool {
        withHandle { sieda_si_assign_model($0, kind, target, modelID) } == 1
    }

    @discardableResult
    func setSIOptions(signOff: Bool, overshootLimit: Double, crosstalkLimit: Double) -> Bool {
        withHandle { sieda_si_set_options($0, signOff ? 1 : 0, overshootLimit, crosstalkLimit) } == 1
    }

    /// PDN inputs of a rail (0 derives the value from the design).
    @discardableResult
    func setPDNRail(_ net: String, ripplePercent: Double, transientAmps: Double, dcAmps: Double) -> Bool {
        withHandle { sieda_pi_set_rail($0, net, ripplePercent, transientAmps, dcAmps) } == 1
    }

    // MARK: - Channel analysis and PI planning

    /// Loss per stack-up layer at its single-ended width (`roughness`: "huray", "hammerstad" or "none").
    func siLineLoss(roughness: String) -> SILineLossReport {
        let options = Self.optionsJSON(["roughness": roughness])
        return Self.decode(SILineLossReport.self, from: withHandle { Self.take(sieda_si_line_loss_json($0, options)) }) ?? .empty
    }

    /// Copper foil for loss ("" = by laminate). False for an unknown id.
    @discardableResult
    func setCopperFoil(_ foil: String) -> Bool { withHandle { sieda_si_set_copper_foil($0, foil) } == 1 }
    func setFieldSolverLines(_ on: Bool) -> Bool { withHandle { sieda_si_set_field_solver_lines($0, on ? 1 : 0) } == 1 }

    /// S-parameters, step response and eye of a routed net or pair; `touchstone` text is cascaded at the receiver.
    func siChannel(net: String, partner: String, settings: SIChannelSettings, touchstone: String? = nil,
                   touchstonePorts: Int = 0) -> Result<SIChannelReport, EDAEngineError> {
        let json = Self.optionsJSON(settings.options(net: net, partner: partner, touchstone: touchstone,
                                                     touchstonePorts: touchstonePorts))
        return Self.decodeChecked(SIChannelReport.self, from: withHandle { Self.take(sieda_si_channel_json($0, json)) })
    }

    /// The channel as Touchstone text (.s2p / .s4p).
    func siChannelTouchstone(net: String, partner: String, settings: SIChannelSettings) throws -> String {
        let json = Self.optionsJSON(settings.options(net: net, partner: partner, touchstone: nil, touchstonePorts: 0))
        var errorPointer: UnsafeMutablePointer<CChar>?
        let text = withHandle { Self.take(sieda_si_channel_touchstone($0, json, &errorPointer)) }
        guard let text else { throw EDAEngineError.operationFailed(Self.take(errorPointer) ?? "No channel to export.") }
        return text
    }

    /// An imported Touchstone file run as a channel with an ideal driver (bit rate, swing, port order of `settings`).
    static func touchstoneChannel(_ text: String, ports: Int, settings: SIChannelSettings) throws -> SITouchstoneReport {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let options = optionsJSON(settings.options(net: "", partner: "", touchstone: nil, touchstonePorts: 0))
        let json = take(sieda_touchstone_channel_json(text, Int32(clamping: ports), options, &errorPointer))
        guard let json else { throw EDAEngineError.operationFailed(take(errorPointer) ?? "Not a Touchstone file.") }
        guard let report = decode(SITouchstoneReport.self, from: json) else {
            throw EDAEngineError.operationFailed("unreadable reply from the core")
        }
        return report
    }

    /// A serial channel checked by sign-off (bit rate 0 removes it).
    @discardableResult
    func setSIChannel(_ net: String, bitRate: Double, maskHeight: Double, maskWidthUi: Double) -> Bool {
        withHandle { sieda_si_set_channel($0, net, bitRate, maskHeight, maskWidthUi) } == 1
    }

    /// Regulator output resistance (Ω) and loop bandwidth (Hz) of a rail; 0 derives them.
    @discardableResult
    func setPDNRegulator(_ net: String, outputOhms: Double, loopBandwidth: Double) -> Bool {
        withHandle { sieda_pi_set_vrm($0, net, outputOhms, loopBandwidth) } == 1
    }

    func pdnCavity(_ net: String) -> PDNCavityReport? {
        Self.decode(PDNCavityReport.self, from: withHandle { Self.take(sieda_pi_cavity_json($0, net)) })
    }

    func pdnDecapPlan(_ net: String) -> PDNDecapPlanReport? {
        Self.decode(PDNDecapPlanReport.self, from: withHandle { Self.take(sieda_pi_decap_plan_json($0, net)) })
    }

    func pdnIRMap(_ net: String) -> PDNIRMapReport? {
        Self.decode(PDNIRMapReport.self, from: withHandle { Self.take(sieda_pi_ir_map_json($0, net)) })
    }

    @discardableResult
    func setSolderMask(_ mask: SolderMaskColour) -> Bool { withHandle { sieda_pcb_set_solder_mask($0, mask.rawValue) } == 1 }

    /// Custom board outline (≥ 3 points, mm); an empty array restores the rectangle.
    @discardableResult
    func setOutline(_ points: [CGPoint]) -> Bool {
        let json = "[" + points.map { "{\"x\":\($0.x),\"y\":\($0.y)}" }.joined(separator: ",") + "]"
        return withHandle { sieda_pcb_set_outline($0, json) } == 1
    }

    @discardableResult
    func applyOutlinePreset(_ preset: BoardOutlinePreset, width: Double, height: Double, parameter: Double) -> Bool {
        withHandle { sieda_pcb_outline_preset($0, preset.rawValue, width, height, parameter) } == 1
    }

    @discardableResult
    func addMountingHole(at point: CGPoint, drill: Double, keepout: Double) -> Int {
        Int(withHandle { sieda_pcb_add_mounting_hole($0, Double(point.x), Double(point.y), drill, keepout) })
    }

    func clearMountingHoles() { withHandle { sieda_pcb_clear_mounting_holes($0) } }

    /// Adds a copper pour (plane = reserved plane layer); returns the zone index or nil.
    @discardableResult
    func addZone(net: String, layer: Int, plane: Bool, clearance: Double = 0) -> Int? {
        let index = withHandle { sieda_pcb_add_zone($0, net, Int32(layer), plane ? 1 : 0, clearance) }
        return index >= 0 ? Int(index) : nil
    }

    @discardableResult
    func removeZone(at index: Int) -> Bool { withHandle { sieda_pcb_remove_zone($0, Int32(index)) } == 1 }

    func clearZones() { withHandle { sieda_pcb_clear_zones($0) } }

    func runDRC() -> [RuleViolation] {
        (try? runDRCChecked().get()) ?? []
    }

    /// Runs the DRC, or reports the core's error message (instead of an empty, "passed" result).
    func runDRCChecked() -> Result<[RuleViolation], EDAEngineError> {
        Self.decodeChecked([RuleViolation].self, from: withHandle { Self.take(sieda_pcb_run_drc($0)) }).map { $0.numbered() }
    }

    // MARK: - Export & 3D

    func export(_ format: ExportFormat) -> String? {
        withHandle { Self.take(sieda_export($0, format.rawValue)) }
    }

    /// Copper Gerber of layer `oneBasedLayer` (1 = top … layerCount = bottom).
    func exportCopperLayer(_ oneBasedLayer: Int) -> String? {
        withHandle { Self.take(sieda_export($0, "gerber_l\(oneBasedLayer)")) }
    }

    func buildMesh(includeComponents: Bool) -> MeshData? {
        withHandle { handle -> MeshData? in
            Self.copyMesh(sieda_mesh_build(handle, includeComponents ? 1 : 0))
        }
    }

    /// Copper of a single layer, flat at Y = 0 (X-ray stack view).
    func buildLayerMesh(layer: Int) -> MeshData? {
        withHandle { handle -> MeshData? in
            Self.copyMesh(sieda_mesh_build_layer(handle, Int32(layer)))
        }
    }

    private static func copyMesh(_ meshPointer: OpaquePointer?) -> MeshData? {
        guard let mesh = meshPointer else { return nil }
        defer { sieda_mesh_free(mesh) }
        let vertexCount = Int(sieda_mesh_vertex_count(mesh))
        let indexCount = Int(sieda_mesh_index_count(mesh))
        guard vertexCount > 0, indexCount > 0,
              let positions = sieda_mesh_positions(mesh),
              let normals = sieda_mesh_normals(mesh),
              let colors = sieda_mesh_colors(mesh),
              let indices = sieda_mesh_indices(mesh) else { return nil }
        let surfaces = sieda_mesh_surfaces(mesh).map { Array(UnsafeBufferPointer(start: $0, count: vertexCount)) } ?? []
        return MeshData(
            positions: Array(UnsafeBufferPointer(start: positions, count: vertexCount * 3)),
            normals: Array(UnsafeBufferPointer(start: normals, count: vertexCount * 3)),
            colors: Array(UnsafeBufferPointer(start: colors, count: vertexCount * 4)),
            indices: Array(UnsafeBufferPointer(start: indices, count: indexCount)),
            surfaces: surfaces
        )
    }
}

/// Export formats understood by `sieda_export`.
enum ExportFormat: String, CaseIterable, Identifiable {
    case spice
    case bom
    case pickAndPlace = "pnp"
    case gerberTop = "gerber_top"
    case gerberBottom = "gerber_bottom"
    case gerberMaskTop = "gerber_mask_top"
    case gerberMaskBottom = "gerber_mask_bottom"
    case gerberSilkTop = "gerber_silk_top"
    case gerberEdge = "gerber_edge"
    case gerberPasteTop = "gerber_paste_top"
    case gerberPasteBottom = "gerber_paste_bottom"
    case gerberSilkBottom = "gerber_silk_bottom"
    case gerberJob = "gerber_job"
    case ipc356
    case drill
    case drillNPTH = "drill_npth"
    case bomAssembly = "bom_assembly"
    case cpl
    case assemblyTop = "assembly_top"
    case assemblyBottom = "assembly_bottom"
    case fabNotes = "fab_notes"
    case stl
    case obj
    case step
    case idfBoard = "idf_board"
    case idfLibrary = "idf_library"
    case idx
    case ipc2581

    var id: String { rawValue }

    var fileName: String {
        switch self {
        case .spice: return "netlist.cir"
        case .bom: return "bom.csv"
        case .pickAndPlace: return "pick_and_place.csv"
        case .gerberTop: return "board-F_Cu.gbr"
        case .gerberBottom: return "board-B_Cu.gbr"
        case .gerberMaskTop: return "board-F_Mask.gbr"
        case .gerberMaskBottom: return "board-B_Mask.gbr"
        case .gerberSilkTop: return "board-F_Silkscreen.gbr"
        case .gerberEdge: return "board-Edge_Cuts.gbr"
        case .gerberPasteTop: return "board-F_Paste.gbr"
        case .gerberPasteBottom: return "board-B_Paste.gbr"
        case .gerberSilkBottom: return "board-B_Silkscreen.gbr"
        case .gerberJob: return "board-job.gbrjob"
        case .ipc356: return "board-ipc356.ipc"
        case .bomAssembly: return "bom_assembly.csv"
        case .cpl: return "cpl.csv"
        case .assemblyTop: return "assembly_top.svg"
        case .assemblyBottom: return "assembly_bottom.svg"
        case .fabNotes: return "fab_notes.txt"
        case .drill: return "board.drl"
        case .drillNPTH: return "board-NPTH.drl"
        case .stl: return "assembly.stl"
        case .obj: return "assembly.obj"
        case .step: return "board.step"
        case .idfBoard: return "board.emn"
        case .idfLibrary: return "board.emp"
        case .idx: return "board.idx"
        case .ipc2581: return "board-ipc2581.xml"
        }
    }

    var displayName: String {
        switch self {
        case .spice: return "SPICE Netlist"
        case .bom: return "Bill of Materials (CSV)"
        case .pickAndPlace: return "Pick & Place (CSV)"
        case .gerberTop: return "Gerber — Top Copper"
        case .gerberBottom: return "Gerber — Bottom Copper"
        case .gerberMaskTop: return "Gerber — Top Solder Mask"
        case .gerberMaskBottom: return "Gerber — Bottom Solder Mask"
        case .gerberSilkTop: return "Gerber — Top Silkscreen"
        case .gerberEdge: return "Gerber — Board Outline"
        case .gerberPasteTop: return "Gerber — Top Solder Paste (stencil)"
        case .gerberPasteBottom: return "Gerber — Bottom Solder Paste (stencil)"
        case .gerberSilkBottom: return "Gerber — Bottom Silkscreen"
        case .gerberJob: return "Gerber X2 Job File (stack-up)"
        case .ipc356: return "IPC-D-356A Test Netlist"
        case .bomAssembly: return "BOM for Assembly (JLCPCB / PCBWay)"
        case .cpl: return "Component Placement List (CPL)"
        case .assemblyTop: return "Assembly Drawing — Top (SVG)"
        case .assemblyBottom: return "Assembly Drawing — Bottom (SVG)"
        case .fabNotes: return "Fabrication Notes (order sheet)"
        case .drill: return "Excellon Drill"
        case .drillNPTH: return "Excellon Drill — Mounting Holes (NPTH)"
        case .stl: return "3D Model (STL)"
        case .obj: return "3D Model (OBJ)"
        case .step: return "STEP AP214 — Board and Parts (MCAD)"
        case .idfBoard: return "IDF 3.0 Board (.emn)"
        case .idfLibrary: return "IDF 3.0 Library (.emp)"
        case .idx: return "IDX / EDMD Baseline for MCAD (.idx)"
        case .ipc2581: return "IPC-2581C — Layers, Nets, Parts and BOM (XML)"
        }
    }
}

/// Result of writing the fabrication package (`sieda_write_fabrication_package`).
struct FabricationPackageResult: Decodable {
    var ok: Bool
    var files: [String]
    var error: String
}

/// Board outline presets understood by `sieda_pcb_outline_preset`.
enum BoardOutlinePreset: String, CaseIterable, Identifiable {
    case rectangle
    case rounded
    case circle
    case quadX = "quad-x"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .rectangle: return "Rectangle"
        case .rounded: return "Rounded Rectangle"
        case .circle: return "Circle"
        case .quadX: return "Quadcopter X Frame"
        }
    }

    var systemImage: String {
        switch self {
        case .rectangle: return "rectangle"
        case .rounded: return "app"
        case .circle: return "circle"
        case .quadX: return "xmark"
        }
    }
}

/// A running live simulation (C `SiedaLiveSim`). Thread-safe: calls are serialized, so it can step on a background
/// queue while the UI sends switch presses and serial input.
final class LiveSession: @unchecked Sendable {
    private let lock = NSLock()
    private var handle: OpaquePointer?

    fileprivate init(_ handle: OpaquePointer) { self.handle = handle }

    deinit {
        if let handle { sieda_live_free(handle) }
    }

    private func locked<T>(_ body: (OpaquePointer) -> T) -> T? {
        lock.lock()
        defer { lock.unlock() }
        guard let handle else { return nil }
        return body(handle)
    }

    /// Advances `duration` seconds in `step`-second steps and keeps a scope trace of at most `tracePoints` samples.
    func run(duration: Double, step: Double, tracePoints: Int = 200) throws {
        var errorPointer: UnsafeMutablePointer<CChar>?
        let ok = locked { sieda_live_run($0, duration, step, Int32(tracePoints), &errorPointer) } ?? 0
        if ok != 1 {
            var message = "The live simulation stopped."
            if let errorPointer {
                message = String(cString: errorPointer)
                sieda_string_free(errorPointer)
            }
            throw EDAEngineError.operationFailed(message)
        }
    }

    func state() -> LiveState? {
        guard let pointer = locked({ sieda_live_state($0) }) ?? nil else { return nil }
        defer { sieda_string_free(pointer) }
        return try? JSONDecoder().decode(LiveState.self, from: Data(String(cString: pointer).utf8))
    }

    func setSwitch(_ id: Int, closed: Bool) { _ = locked { sieda_live_set_switch($0, Int32(id), closed ? 1 : 0) } }

    func sendSerial(_ id: Int, text: String) { _ = locked { sieda_live_serial_input($0, Int32(id), text) } }
}

// MARK: - Schematic capture: editing productivity, back-annotation, templates and PDF

extension EDAEngine {

    /// Aligns or distributes components: "left", "right", "top", "bottom", "centerX", "centerY", "distributeX",
    /// "distributeY". The number moved (0 when nothing moved).
    @discardableResult
    func alignComponents(_ ids: [Int], mode: String) -> Int {
        let ids = Self.idList(ids)
        return max(0, Int(withHandle { sieda_align_components($0, ids, mode) }))
    }

    /// Aligns by the symbols' outlines (edges line up; distributing leaves equal gaps between outlines).
    func alignOutlines(_ ids: [Int], mode: String) -> Int {
        let ids = Self.idList(ids)
        return max(0, Int(withHandle { sieda_align_outlines($0, ids, mode) }))
    }

    /// Fixes a sheet's template frame at a top-left corner, or lets it follow the drawing.
    @discardableResult
    func setSheetFrame(_ id: Int, fixed: Bool, origin: CGPoint = .zero) -> Bool {
        withHandle { sieda_set_sheet_frame($0, Int32(id), fixed ? 1 : 0, Double(origin.x), Double(origin.y)) } == 1
    }

    /// Clipboard text of components and the wires between them.
    func copyComponents(_ ids: [Int], buses: [Int] = []) -> String? {
        let request: String
        if buses.isEmpty {
            request = Self.idList(ids)
        } else {
            request = "{\"components\":\(Self.idList(ids)),\"buses\":\(Self.idList(buses))}"
        }
        return Self.take(withHandle { sieda_copy_components($0, request) })
    }

    /// Pastes a clipboard (a paste array when `count` > 1); the new components' ids.
    func pasteComponents(_ clip: String, offset: CGSize, count: Int = 1, step: CGSize = .zero, labelIncrement: Int = 0) -> [Int] {
        let options = "{\"dx\":\(Double(offset.width)),\"dy\":\(Double(offset.height)),\"count\":\(count),"
            + "\"stepX\":\(Double(step.width)),\"stepY\":\(Double(step.height)),\"labelIncrement\":\(labelIncrement)}"
        return Self.decode([Int].self, from: Self.take(withHandle { sieda_paste_components($0, clip, options) })) ?? []
    }

    /// Back-annotation: designators re-numbered by board position (proposed, not applied).
    func reannotateFromBoard(byColumns: Bool) -> [EcoChangeInfo] {
        Self.decode([EcoChangeInfo].self, from: Self.take(withHandle { sieda_reannotate_from_board($0, byColumns ? 1 : 0) })) ?? []
    }

    /// Back-annotation: the changes of a WAS / IS text (proposed, not applied).
    func ecoFromWasIs(_ text: String) -> [EcoChangeInfo] {
        Self.decode([EcoChangeInfo].self, from: Self.take(withHandle { sieda_eco_from_was_is($0, text) })) ?? []
    }

    /// Applies the changes; the number applied.
    @discardableResult
    func applyEco(_ changes: [EcoChangeInfo]) -> Int {
        let encoder = JSONEncoder()
        guard let data = try? encoder.encode(changes) else { return 0 }
        let json = String(decoding: data, as: UTF8.self)
        return max(0, Int(withHandle { sieda_apply_eco($0, json) }))
    }

    @discardableResult
    func setSheetSize(_ id: Int, size: String) -> Bool { withHandle { sieda_set_sheet_size($0, Int32(id), size) } == 1 }

    /// Drawn size of a sheet's sheet symbol (schematic units; 0 × 0 = fitted to its entries).
    func setSheetSymbolSize(_ id: Int, width: Double, height: Double) -> Bool {
        withHandle { sieda_set_sheet_symbol_size($0, Int32(id), width, height) } == 1
    }

    /// Drawing templates (A4 … A0, ANSI A … E).
    static func sheetTemplates() -> [SheetTemplateInfo] {
        decode([SheetTemplateInfo].self, from: take(sieda_sheet_templates_json())) ?? []
    }

    /// PDF of every sheet with hierarchy bookmarks, frames and title blocks.
    func schematicPDF(fontPath: String? = EDAEngine.unicodeFontPath) -> Data? {
        Self.take(withHandle { sieda_export_schematic_pdf_with_font($0, fontPath) }).map { Data($0.utf8) }
    }

    /// A system TrueType font with wide Unicode coverage, embedded (as a subset) in schematic PDFs for text beyond
    /// Latin and Greek; nil when none is installed.
    static let unicodeFontPath: String? = [
        "/System/Library/Fonts/Supplemental/Arial Unicode.ttf",
        "/Library/Fonts/Arial Unicode.ttf",
    ].first { FileManager.default.fileExists(atPath: $0) }
}

extension EDAEngine {
    /// ERC error reporting: report `code` as "error", "warning", "info" or "off"; "default" restores its severity.
    @discardableResult
    func setErcSeverity(_ code: String, level: String) -> Bool {
        withHandle { sieda_set_erc_severity($0, code, level) } == 1
    }
}

extension EDAEngine {
    /// Makes a net label an entry of a harness label on its sheet (0: an ordinary label again).
    @discardableResult
    func setHarnessEntry(_ label: Int, harness: Int) -> Bool {
        withHandle { sieda_set_harness_entry($0, Int32(label), Int32(harness)) } == 1
    }
}

extension EDAEngine {
    /// Update PCB: the changes an update from the schematic would make to the board (nothing applied).
    func pcbEcoPreview() -> [PcbEcoChangeInfo] {
        Self.decode([PcbEcoChangeInfo].self, from: Self.take(withHandle { sieda_pcb_eco_preview($0) })) ?? []
    }

    /// Executes the changes with these keys; what was done, one line per change.
    func applyPcbEco(keys: [String]) -> [String] {
        applyPcbEcoResult(keys: keys).report
    }

    /// Executes the changes with these keys; what was done and the new footprints to place by hand.
    func applyPcbEcoResult(keys: [String]) -> PcbEcoResult {
        let json = (try? JSONEncoder().encode(keys)).map { String(decoding: $0, as: UTF8.self) } ?? "[]"
        let reply = Self.take(withHandle { sieda_apply_pcb_eco($0, json) })
        return Self.decode(PcbEcoResult.self, from: reply) ?? PcbEcoResult(executed: 0, report: [], placementQueue: [])
    }
}

/// A pin or gate swap a placed multi-unit part allows on the board (core `PcbSwapOption`).
struct PcbSwapOptionInfo: Codable, Equatable, Identifiable {
    var kind: String  // "pin", "gate"
    var component: Int
    var other: Int
    var pinA: Int
    var pinB: Int
    var label: String
    /// Ratsnest saved (mm; negative = longer).
    var gain: Double
    var id: String { "\(kind):\(component):\(other):\(pinA):\(pinB)" }
}

extension EDAEngine {
    /// The pin / gate swaps of a component's package on the board, best first.
    func pcbSwapOptions(_ component: Int) -> [PcbSwapOptionInfo] {
        Self.decode([PcbSwapOptionInfo].self, from: Self.take(withHandle { sieda_pcb_swap_options($0, Int32(component)) })) ?? []
    }

    /// Makes one swap, back-annotated to the schematic.
    func applyPcbSwap(_ option: PcbSwapOptionInfo) -> Bool {
        guard let data = try? JSONEncoder().encode(option) else { return false }
        let json = String(decoding: data, as: UTF8.self)
        return withHandle { sieda_apply_pcb_swap($0, json) } == 1
    }

    /// Automatic pin / gate swap for one package (or every one with nil); the number of swaps made.
    func optimizePcbSwaps(_ component: Int?, maxSwaps: Int = 100) -> Int {
        max(0, Int(withHandle { sieda_optimize_pcb_swaps($0, Int32(component ?? -1), Int32(maxSwaps)) }))
    }
}
