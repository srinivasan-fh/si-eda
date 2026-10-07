import Foundation

/// Variants side by side (`sieda_variant_matrix_json`).
struct VariantMatrix: Decodable {
    struct Totals: Decodable, Identifiable {
        var name: String
        var fitted: Int
        var notFitted: Int
        var valueChanges: Int
        var id: String { name }
    }
    struct Cell: Decodable {
        var fitted: Bool
        var value: String
    }
    struct Row: Decodable, Identifiable {
        var ref: String
        var value: String
        var cells: [Cell]
        var id: String { ref }
    }
    var variants: [Totals]
    var parts: [Row]
}

extension EDAEngine {
    func variantMatrix() -> VariantMatrix? {
        Self.decode(VariantMatrix.self, from: withHandle { Self.take(sieda_variant_matrix_json($0)) })
    }

    /// Moves parts to the placement of an IDF board file written back by MCAD; the designators moved.
    func importIDFPlacement(_ emn: String) -> [String] {
        struct Moved: Decodable { var moved: [String] }
        return Self.decode(Moved.self, from: withHandle { Self.take(sieda_import_idf_placement($0, emn)) })?.moved ?? []
    }

    /// Review text of what changed from `before` (a saved project's JSON) to the open design.
    func diffText(from before: String) -> String {
        let after = saveJSON()
        return Self.take(sieda_diff_projects(before, after, 1)) ?? ""
    }
}
