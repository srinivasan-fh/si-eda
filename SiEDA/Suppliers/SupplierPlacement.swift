import CoreGraphics
import Foundation

/// Turning a distributor part into a project part: link it to a symbol and footprint the project library or the
/// standard catalog already has, else prefill the pin-table editor; and its sourcing (manufacturer, MPN, the
/// supplier's part number and the unit price at the build quantity).
enum SupplierPlacement {
    /// Where the part's symbol and footprint come from.
    enum Link: Equatable {
        /// A project library part (imported or created) with the same part number.
        case library(partId: String, name: String)
        /// A standard catalog part; `note` asks for a package check when only the packing differs.
        case catalog(name: String, note: String)
        /// Nothing known: the pin table must be filled from the datasheet.
        case none(candidates: [String])
    }

    static func link(for part: SupplierPartInfo, library: [CustomPartInfo]) -> Link {
        let key = normalized(part.mpn)
        if let own = library.first(where: { normalized($0.name) == key }) { return .library(partId: own.id, name: own.name) }
        let match = EDAEngine.catalogMatch(mpn: part.mpn)
        if !match.match.isEmpty {
            if let own = library.first(where: { $0.name == match.match }) { return .library(partId: own.id, name: own.name) }
            return .catalog(name: match.match, note: match.note)
        }
        return .none(candidates: match.candidates)
    }

    /// Upper-case letters and digits (as the core matches part numbers).
    static func normalized(_ mpn: String) -> String {
        String(mpn.uppercased().unicodeScalars.filter { CharacterSet.alphanumerics.contains($0) && $0.isASCII }
            .map(Character.init))
    }

    /// Manufacturer, MPN, supplier part number and unit price of the best offer for `boards` boards (one part each).
    static func sourcing(for part: SupplierPartInfo, boards: Int, currency: String) -> SourcingUpdate {
        var update = SourcingUpdate(manufacturer: part.manufacturer.isEmpty ? nil : part.manufacturer, mpn: part.mpn)
        let request = SupplierRollupRequest(
            currency: currency, quantities: [max(1, boards)], buildQuantity: max(1, boards),
            lines: [.init(item: 1, refs: [], quantity: 1, mpn: part.mpn, manufacturer: part.manufacturer, dnp: false,
                          embedded: false)],
            parts: [part])
        if let offer = EDAEngine.supplierBomRollup(request)?.lines.first?.offer(boards: max(1, boards)) {
            if !offer.sku.isEmpty { update.supplierPart = offer.sku }
            if offer.unitPrice > 0, offer.currency.isEmpty || offer.currency == currency { update.unitPrice = offer.unitPrice }
        }
        return update
    }

    /// The pin-table editor's starting point for a part nothing is known about: name, manufacturer, description,
    /// datasheet link and a package guessed from the distributor's package text and pin count.
    static func draft(for part: SupplierPartInfo) -> CustomPartSpec {
        var spec = CustomPartSpec()
        spec.name = part.mpn
        spec.manufacturer = part.manufacturer
        spec.description = String(part.description.prefix(200))
        spec.defaultValue = part.mpn
        spec.datasheet = part.datasheet
        spec.refPrefix = refPrefix(category: part.category, description: part.description)
        let packageText = [part.package, part.parameter("Package / Case", "Supplier Device Package", "Case/Package") ?? ""]
            .filter { !$0.isEmpty }.joined(separator: " ")
        spec.package.type = (PackageKind.guess(packageText) ?? .soic).rawValue
        spec.package.pinCount = pinCount(part, packageText: packageText)
        return spec
    }

    static func pinCount(_ part: SupplierPartInfo, packageText: String) -> Int {
        if let pins = part.parameter("Number of Pins", "Pin Count", "Number of Terminals"),
           let n = Int(pins.prefix { $0.isNumber }), (1...512).contains(n) { return n }
        // "8-SOIC", "SOIC-8", "QFN-24": the number in the package name.
        let digits = packageText.split { !$0.isNumber }.compactMap { Int($0) }.filter { (2...512).contains($0) }
        return digits.first ?? 0
    }

    static func refPrefix(category: String, description: String) -> String {
        let text = (category + " " + description).lowercased()
        if text.contains("connector") || text.contains("header") || text.contains("receptacle") { return "J" }
        if text.contains("crystal") || text.contains("oscillator") || text.contains("resonator") { return "Y" }
        if text.contains("mosfet") || text.contains("transistor") || text.contains("bjt") { return "Q" }
        if text.contains("diode") || text.contains("rectifier") || text.contains("tvs") { return "D" }
        if text.contains("inductor") || text.contains("ferrite") { return "L" }
        if text.contains("capacitor") { return "C" }
        if text.contains("resistor") { return "R" }
        if text.contains("fuse") { return "F" }
        if text.contains("switch") && !text.contains("load switch") { return "SW" }
        return "U"
    }
}

extension DesignStore {
    /// Places a project library part with its sourcing as one undo step, to the right of the existing parts.
    @discardableResult
    func placeLibraryPart(_ partId: String, sourcing: SourcingUpdate) -> Int {
        var id = -1
        let x = (snapshot.components.map(\.x).max() ?? 0) + 160
        let point = SchematicAutoLayout.snap(CGPoint(x: x, y: 0))
        let name = snapshot.customPart(partId)?.name ?? "part"
        performChecked("Placed \(name)", failureMessage: "\(name) was not placed") { engine in
            id = engine.addCustomComponent(partId: partId, at: point)
            guard id >= 0 else { return false }
            if sourcing != SourcingUpdate() { engine.setSourcing(component: id, sourcing) }
            return true
        }
        if id >= 0 { selection = [id] }
        return id
    }

    /// Writes distributor prices and supplier part numbers into BOM lines as one undo step. Returns the lines changed.
    @discardableResult
    func applySupplierPricing(_ updates: [(line: BomLineInfo, update: SourcingUpdate)]) -> Int {
        let changes = updates.filter { $0.update != SourcingUpdate() }
        guard !changes.isEmpty else { return 0 }
        var changed = 0
        performChecked("Applied distributor prices to \(changes.count) BOM lines", invalidatesAnalysis: false,
                       failureMessage: "No BOM lines changed") { engine in
            for change in changes {
                var any = false
                for id in change.line.componentIds where engine.setSourcing(component: id, change.update) { any = true }
                if any { changed += 1 }
            }
            return changed > 0
        }
        return changed
    }
}
