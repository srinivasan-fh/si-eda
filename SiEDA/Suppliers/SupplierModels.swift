import Foundation

/// Distributor APIs SiEDA can search with the user's own keys.
enum SupplierSource: String, CaseIterable, Identifiable, Codable, Sendable {
    case nexar, digikey, mouser

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .nexar: return "Octopart (Nexar)"
        case .digikey: return "DigiKey"
        case .mouser: return "Mouser"
        }
    }

    /// Where the user gets the key.
    var signUpURL: URL? {
        switch self {
        case .nexar: return URL(string: "https://portal.nexar.com")
        case .digikey: return URL(string: "https://developer.digikey.com")
        case .mouser: return URL(string: "https://www.mouser.com/api-hub/")
        }
    }
}

/// Lifecycle of a manufacturer part (core `PartLifecycle`).
enum SupplierLifecycle: String, Codable, Sendable {
    case unknown, active, new, nrnd, eol, obsolete

    init(from decoder: Decoder) throws {
        let raw = (try? decoder.singleValueContainer().decode(String.self)) ?? ""
        self = SupplierLifecycle(rawValue: raw) ?? .unknown
    }

    /// NRND, EOL and obsolete parts are flagged in the search and the BOM.
    var needsAttention: Bool { self == .nrnd || self == .eol || self == .obsolete }

    /// Industry abbreviation shown on the badge (kept in English, like part numbers).
    var badge: String {
        switch self {
        case .unknown: return "—"
        case .active: return "ACTIVE"
        case .new: return "NEW"
        case .nrnd: return "NRND"
        case .eol: return "EOL"
        case .obsolete: return "OBSOLETE"
        }
    }
}

struct SupplierPriceBreak: Codable, Equatable, Sendable {
    var quantity: Int
    var price: Double
}

/// One distributor offer (one packaging) of a part.
struct SupplierOfferInfo: Codable, Equatable, Identifiable, Sendable {
    var supplier: String
    var sku: String
    /// Units in stock; -1 = not reported.
    var stock: Int
    var moq: Int
    var multiple: Int
    var packaging: String
    var currency: String
    /// Factory lead time in days; -1 = not reported.
    var leadTimeDays: Int
    var url: String
    var prices: [SupplierPriceBreak]

    var id: String { "\(supplier)|\(sku)|\(packaging)" }
}

struct SupplierParameterInfo: Codable, Equatable, Sendable {
    var name: String
    var value: String
}

/// Best unit price at a quantity across the offers (computed by the core).
struct SupplierPricePoint: Codable, Equatable, Sendable {
    var quantity: Int
    var unitPrice: Double
    var orderQuantity: Int
    var currency: String
    var supplier: String
    var inStock: Bool
}

/// A manufacturer part in the normalised supplier schema (`sieda.supplier/1`).
struct SupplierPartInfo: Codable, Equatable, Identifiable, Sendable {
    var mpn: String
    var manufacturer: String
    var description: String
    var category: String
    var package: String
    var datasheet: String
    var productUrl: String
    var imageUrl: String
    var lifecycle: SupplierLifecycle
    var lifecycleText: String
    var rohs: String
    var parameters: [SupplierParameterInfo]
    var offers: [SupplierOfferInfo]
    var sources: [String]
    /// Units in stock across the offers; -1 = not reported.
    var stock: Int
    var pricing: [SupplierPricePoint]

    var id: String { "\(mpn)|\(manufacturer)" }

    func price(at quantity: Int) -> SupplierPricePoint? { pricing.first { $0.quantity == quantity } }

    /// A parameter's value by name ("Number of Pins", "Package / Case"), case-insensitively.
    func parameter(_ names: String...) -> String? {
        for name in names {
            if let p = parameters.first(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame }), !p.value.isEmpty {
                return p.value
            }
        }
        return nil
    }
}

/// One distributor reply read by the core.
struct SupplierSearchResultInfo: Codable, Equatable, Sendable {
    var source: String
    var total: Int = 0
    var error: String = ""
    var errorKind: String = ""
    var notes: [String] = []
    var parts: [SupplierPartInfo] = []
}

struct SupplierMergeResult: Decodable {
    struct SourceError: Decodable {
        var source: String
        var error: String
        var errorKind: String
    }
    var parts: [SupplierPartInfo]
    var errors: [SourceError]
}

/// Core `catalogMatchForMpn`: the standard part to link a manufacturer part to.
struct SupplierCatalogMatch: Decodable, Equatable {
    var match: String = ""
    var exact: Bool = false
    var note: String = ""
    var candidates: [String] = []
}

/// BOM roll-up request (core `supplierBomRollup`).
struct SupplierRollupRequest: Encodable {
    struct Line: Encodable {
        var item: Int
        var refs: [String]
        var quantity: Int
        var mpn: String
        var manufacturer: String
        var dnp: Bool
        var embedded: Bool
    }
    var currency: String
    var quantities: [Int]
    var buildQuantity: Int
    var attritionPercent: Double = 0  // spares per line for assembly loss (the core adds the larger of this and attritionMin)
    var lines: [Line]
    var parts: [SupplierPartInfo]
}

/// BOM roll-up: per line the best offer at each board count, totals per board count, warnings.
struct SupplierRollup: Decodable, Equatable {
    struct Offer: Decodable, Equatable {
        var boards: Int
        var needed: Int
        var orderQuantity: Int
        var supplier: String
        var sku: String
        var unitPrice: Double
        var extended: Double
        var currency: String
        var inStock: Bool
    }
    struct Line: Decodable, Equatable, Identifiable {
        var item: Int
        var refs: [String]
        var mpn: String
        var quantity: Int
        /// "priced", "unpriced", "no_mpn", "not_found", "dnp".
        var status: String
        var lifecycle: SupplierLifecycle
        var lifecycleText: String
        var warning: String
        var datasheet: String
        var stock: Int
        var offers: [Offer]
        var id: Int { item }

        func offer(boards: Int) -> Offer? { offers.first { $0.boards == boards } }
    }
    struct Total: Decodable, Equatable, Identifiable {
        var boards: Int
        var cost: Double
        var perBoard: Double
        var priced: Int
        var unpriced: Int
        var shortages: Int
        var complete: Bool
        var id: Int { boards }
    }
    var currency: String
    var lines: [Line]
    var totals: [Total]
    var warnings: [String]
    var mixedCurrency: Bool
    var buildQuantity: Int
}
