import Foundation

/// What the core read from a 3D model file (`sieda_model3d_import`); the mesh is registered under `id`.
struct Model3DImportResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var id: String?
    var name: String?
    var format: String?
    /// Millimetres per file unit the format is usually written in (KiCad VRML 2.54, STL / OBJ 1).
    var unit: Double?
    var vertices: Int?
    var triangles: Int?
    var bounds: [Double]?
    var warnings: [String]?
}

/// `sieda_model3d_fit`: the aligned model's bounds and the alignment seated on the board.
struct Model3DFitResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var bounds: [Double]?
    var seated: CustomPartSpec.Model3DRef?
}

/// Imported 3D models through the core: reading VRML / STL / OBJ / STEP files, fitting them to the footprint and the
/// part-alone preview mesh. Pure functions of their arguments (the registry is the core's): safe from any thread.
extension EDAEngine {
    /// File types the 3D model importer reads.
    static let model3dExtensions: Set<String> = ["wrl", "vrml", "stl", "obj", "step", "stp"]
    /// Text formats, sent as they are (binary STL goes as base64).
    static let model3dTextExtensions: Set<String> = ["wrl", "vrml", "obj", "step", "stp"]

    private static func takeModelString(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { sieda_string_free(pointer) }
        return String(cString: pointer)
    }

    /// Reads a model file's bytes (`name` gives the format by its extension). Binary files go as base64.
    static func importModel3D(name: String, data: Data) -> Model3DImportResult {
        var request: [String: Any] = ["name": name]
        let isText = Self.model3dTextExtensions.contains((name as NSString).pathExtension.lowercased())
        if isText, let text = String(data: data, encoding: .utf8) {
            request["content"] = text
        } else {
            request["contentBase64"] = data.base64EncodedString()
        }
        guard let body = try? JSONSerialization.data(withJSONObject: request) else {
            return Model3DImportResult(ok: false, error: "The file could not be sent to the core.")
        }
        let json = takeModelString(sieda_model3d_import(String(decoding: body, as: UTF8.self)))
        guard let data = json?.data(using: .utf8), let result = try? JSONDecoder().decode(Model3DImportResult.self, from: data) else {
            return Model3DImportResult(ok: false, error: "The core returned an unreadable reply.")
        }
        return result
    }

    /// Bounds of the spec's aligned model and the alignment that centres it and seats it on the board.
    static func fitModel3D(_ spec: CustomPartSpec) -> Model3DFitResult {
        let json = takeModelString(sieda_model3d_fit(spec.jsonString()))
        guard let data = json?.data(using: .utf8), let result = try? JSONDecoder().decode(Model3DFitResult.self, from: data) else {
            return Model3DFitResult(ok: false, error: "The core returned an unreadable reply.")
        }
        return result
    }

    /// A footprint of the library browser (`sieda_footprint_library_json`).
    struct LibraryFootprint: Decodable, Identifiable, Hashable {
        var name: String
        var label: String
        var pads: Int
        var width: Double
        var height: Double
        var id: String { name }
    }

    /// Built-in footprints whose name or label holds every word of `query` (at most 200).
    static func footprintLibrary(_ query: String) -> [LibraryFootprint] {
        guard let json = takeModelString(sieda_footprint_library_json(nil, query)) else { return [] }
        return (try? JSONDecoder().decode([LibraryFootprint].self, from: Data(json.utf8))) ?? []
    }

    /// A library footprint's pads as lands (pad k on pin k + 1) and its body size.
    static func footprintLands(_ name: String) -> (lands: [CustomPartSpec.Land], bodyW: Double, bodyD: Double)? {
        struct Reply: Decodable { var lands: [CustomPartSpec.Land]; var bodySize: Double; var bodyDepth: Double }
        guard let json = takeModelString(sieda_footprint_lands_json(name)),
              let r = try? JSONDecoder().decode(Reply.self, from: Data(json.utf8)) else { return nil }
        return (r.lands, r.bodySize, r.bodyDepth)
    }

    /// The part alone on a small board (pads, solder, its model or generated body), for the alignment preview.
    static func model3DPreviewMesh(_ spec: CustomPartSpec) -> MeshData? {
        guard let mesh = sieda_model3d_preview(spec.jsonString()) else { return nil }
        defer { sieda_mesh_free(mesh) }
        let vertexCount = Int(sieda_mesh_vertex_count(mesh))
        let indexCount = Int(sieda_mesh_index_count(mesh))
        guard vertexCount > 0, indexCount > 0,
              let positions = sieda_mesh_positions(mesh),
              let normals = sieda_mesh_normals(mesh),
              let colors = sieda_mesh_colors(mesh),
              let indices = sieda_mesh_indices(mesh) else { return nil }
        let surfaces = sieda_mesh_surfaces(mesh).map { Array(UnsafeBufferPointer(start: $0, count: vertexCount)) } ?? []
        return MeshData(positions: Array(UnsafeBufferPointer(start: positions, count: vertexCount * 3)),
                        normals: Array(UnsafeBufferPointer(start: normals, count: vertexCount * 3)),
                        colors: Array(UnsafeBufferPointer(start: colors, count: vertexCount * 4)),
                        indices: Array(UnsafeBufferPointer(start: indices, count: indexCount)),
                        surfaces: surfaces)
    }
}
