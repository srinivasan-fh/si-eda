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

    private static func decode<T: Decodable>(_ type: T.Type, from json: String?) -> T? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    // MARK: - Project lifecycle

    func setName(_ name: String) { withHandle { sieda_project_set_name($0, name) } }
    func setRequirements(_ text: String) { withHandle { sieda_project_set_requirements($0, text) } }
    func clear() { withHandle { sieda_project_clear($0) } }

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
        let json = withHandle { Self.take(sieda_project_snapshot($0)) }
        return Self.decode(DesignSnapshot.self, from: json)
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

    // MARK: - Analysis

    func runERC() -> [RuleViolation] {
        Self.decode([RuleViolation].self, from: withHandle { Self.take(sieda_run_erc($0)) }) ?? []
    }

    func simulateDC() -> DCResult {
        let json = withHandle { Self.take(sieda_simulate_dc($0)) }
        return Self.decode(DCResult.self, from: json) ?? DCResult(error: "Simulator returned no result.")
    }

    func simulateTransient(stop: Double, step: Double) -> TransientResult {
        let json = withHandle { Self.take(sieda_simulate_transient($0, stop, step)) }
        return Self.decode(TransientResult.self, from: json) ?? TransientResult(error: "Simulator returned no result.")
    }

    // MARK: - PCB

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

    func autoRoute() -> RouteStats {
        Self.decode(RouteStats.self, from: withHandle { Self.take(sieda_pcb_autoroute($0)) }) ?? RouteStats()
    }

    func clearRouting() { withHandle { sieda_pcb_clear_routing($0) } }

    func runDRC() -> [RuleViolation] {
        Self.decode([RuleViolation].self, from: withHandle { Self.take(sieda_pcb_run_drc($0)) }) ?? []
    }

    // MARK: - Export & 3D

    func export(_ format: ExportFormat) -> String? {
        withHandle { Self.take(sieda_export($0, format.rawValue)) }
    }

    func buildMesh(includeComponents: Bool) -> MeshData? {
        withHandle { handle -> MeshData? in
            guard let mesh = sieda_mesh_build(handle, includeComponents ? 1 : 0) else { return nil }
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
        case .stl: return "3D Model (STL)"
        case .obj: return "3D Model (OBJ)"
        }
    }

    static let fabricationPackage: [ExportFormat] = [.gerberTop, .gerberBottom, .gerberMaskTop, .gerberMaskBottom,
                                                     .gerberSilkTop, .gerberEdge, .drill, .bom, .pickAndPlace, .spice,
                                                     .stl]
}
