import Foundation

/// Supplier part data through the core (`sieda_supplier_*`): distributor replies read into the normalised schema,
/// merged across distributors, the BOM cost roll-up and catalog matching. All pure functions: safe from any thread.
extension EDAEngine {
    /// Takes ownership of a string the core returned.
    private static func takeSupplierString(_ pointer: UnsafeMutablePointer<CChar>?) -> String? {
        guard let pointer else { return nil }
        defer { sieda_string_free(pointer) }
        return String(cString: pointer)
    }

    private static func decodeSupplier<T: Decodable>(_ type: T.Type, _ json: String?) -> T? {
        guard let json, let data = json.data(using: .utf8) else { return nil }
        return try? JSONDecoder().decode(type, from: data)
    }

    /// Reads a distributor reply body into the normalised schema (errors are in `error` / `errorKind`).
    static func parseSupplierReply(source: SupplierSource, body: Data, currency: String) -> SupplierSearchResultInfo {
        let text = String(decoding: body, as: UTF8.self)
        let json = takeSupplierString(sieda_supplier_parse(source.rawValue, text, currency))
        return decodeSupplier(SupplierSearchResultInfo.self, json)
            ?? SupplierSearchResultInfo(source: source.rawValue, error: "The core could not read the reply.", errorKind: "parse")
    }

    /// Decodes a normalised result kept in the offline cache (nil when unreadable).
    static func decodeSupplierResult(_ json: String) -> SupplierSearchResultInfo? {
        decodeSupplier(SupplierSearchResultInfo.self, json)
    }

    /// One part per manufacturer part number with every distributor's offers.
    static func mergeSupplierResults(_ results: [SupplierSearchResultInfo], currency: String) -> [SupplierPartInfo] {
        struct Request: Encodable {
            var results: [SupplierSearchResultInfo]
            var currency: String
        }
        guard let data = try? JSONEncoder().encode(Request(results: results, currency: currency)) else { return [] }
        let json = takeSupplierString(sieda_supplier_merge(String(decoding: data, as: UTF8.self)))
        return decodeSupplier(SupplierMergeResult.self, json)?.parts ?? []
    }

    /// BOM cost roll-up per build quantity with stock and lifecycle warnings.
    static func supplierBomRollup(_ request: SupplierRollupRequest) -> SupplierRollup? {
        guard let data = try? JSONEncoder().encode(request) else { return nil }
        let json = takeSupplierString(sieda_supplier_bom_rollup(String(decoding: data, as: UTF8.self)))
        return decodeSupplier(SupplierRollup.self, json)
    }

    /// The standard-catalog part for a manufacturer part number (exact, or the same part in other packing).
    static func catalogMatch(mpn: String) -> SupplierCatalogMatch {
        decodeSupplier(SupplierCatalogMatch.self, takeSupplierString(sieda_supplier_catalog_match(mpn)))
            ?? SupplierCatalogMatch()
    }
}
