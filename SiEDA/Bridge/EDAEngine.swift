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

    private func withHandle<T>(_ body: (OpaquePointer) -> T) -> T {
        lock.lock()
        defer { lock.unlock() }
        return body(handle)
    }

    /// Takes ownership of a malloc'd C string returned by the core.
    private static func take(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
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

    private static func decode<T: Decodable>(_ type: T.Type, from json: String?) -> T? {
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
    func snapshotChecked() -> Result<DesignSnapshot, EDAEngineError> {
        let json = withHandle { Self.take(sieda_project_snapshot($0)) }
        guard let json, let data = json.data(using: .utf8) else { return .failure(.operationFailed("no reply from the core")) }
        do {
            return .success(try JSONDecoder().decode(DesignSnapshot.self, from: data))
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
        return MeshData(
            positions: Array(UnsafeBufferPointer(start: positions, count: vertexCount * 3)),
            normals: Array(UnsafeBufferPointer(start: normals, count: vertexCount * 3)),
            colors: Array(UnsafeBufferPointer(start: colors, count: vertexCount * 4)),
            indices: Array(UnsafeBufferPointer(start: indices, count: indexCount))
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
    case drill
    case drillNPTH = "drill_npth"
    case stl
    case obj

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
        case .drill: return "board.drl"
        case .drillNPTH: return "board-NPTH.drl"
        case .stl: return "assembly.stl"
        case .obj: return "assembly.obj"
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
        case .drill: return "Excellon Drill"
        case .drillNPTH: return "Excellon Drill — Mounting Holes (NPTH)"
        case .stl: return "3D Model (STL)"
        case .obj: return "3D Model (OBJ)"
        }
    }

    static let fabricationPackage: [ExportFormat] = [.gerberTop, .gerberBottom, .gerberMaskTop, .gerberMaskBottom,
                                                     .gerberSilkTop, .gerberEdge, .drill, .drillNPTH, .bom, .pickAndPlace,
                                                     .spice,
                                                     .stl]
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
