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

/// Design review (`sieda_review_json`).
struct DesignReview: Decodable {
    struct Reply: Decodable, Hashable {
        var author: String
        var text: String
    }
    struct Comment: Decodable, Identifiable {
        var id: Int
        var author: String
        var text: String
        var ref: String?
        var view: String
        var resolved: Bool
        var replies: [Reply]?
    }
    var comments: [Comment]
    var open: Int
    var markdown: String
}

extension EDAEngine {
    func review() -> DesignReview? {
        Self.decode(DesignReview.self, from: withHandle { Self.take(sieda_review_json($0)) })
    }

    /// A review command (add, reply, resolve, reopen, delete); nil on success, else the core's reason.
    func reviewCommand(_ request: [String: Any]) -> String? {
        guard let data = try? JSONSerialization.data(withJSONObject: request),
              let json = String(data: data, encoding: .utf8) else { return "Invalid request" }
        struct Reply: Decodable { var error: String? }
        let reply = Self.decode(Reply.self, from: withHandle { Self.take(sieda_review_command($0, json)) })
        return reply == nil ? "No reply from the core" : reply?.error
    }

    func variantMatrix() -> VariantMatrix? {
        Self.decode(VariantMatrix.self, from: withHandle { Self.take(sieda_variant_matrix_json($0)) })
    }

    /// Moves parts to the placement of an IDF board file written back by MCAD; the designators moved.
    func importIDFPlacement(_ emn: String) -> [String] {
        struct Moved: Decodable { var moved: [String] }
        return Self.decode(Moved.self, from: withHandle { Self.take(sieda_import_idf_placement($0, emn)) })?.moved ?? []
    }

    /// Enclosure height limit per side for the 3D clearance DRC (mm, 0 = none); height zones are kept as they are.
    func enclosureHeights() -> (top: Double, bottom: Double) {
        struct Limits: Decodable { var maxHeightTop: Double?; var maxHeightBottom: Double? }
        let l = Self.decode(Limits.self, from: withHandle { Self.take(sieda_pcb_mechanical_limits($0)) })
        return (l?.maxHeightTop ?? 0, l?.maxHeightBottom ?? 0)
    }

    func setEnclosureHeights(top: Double, bottom: Double) -> Bool {
        withHandle { handle -> Bool in
            let current = Self.take(sieda_pcb_mechanical_limits(handle)) ?? "{}"
            guard var limits = (try? JSONSerialization.jsonObject(with: Data(current.utf8))) as? [String: Any] else { return false }
            limits["maxHeightTop"] = top
            limits["maxHeightBottom"] = bottom
            guard let data = try? JSONSerialization.data(withJSONObject: limits),
                  let json = String(data: data, encoding: .utf8) else { return false }
            return sieda_pcb_set_mechanical_limits(handle, json) == 1
        }
    }

    /// Review text of what changed from `before` (a saved project's JSON) to the open design.
    func diffText(from before: String) -> String {
        let after = saveJSON()
        return Self.take(sieda_diff_projects(before, after, 1)) ?? ""
    }
}
