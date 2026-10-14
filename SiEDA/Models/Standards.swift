import Foundation

// Built-in standards exposed by the core: standard parts, E-series values, PCB design-rule presets and the
// design verification report.

/// Core standards, loaded once (they are compiled into the core and never change at run time).
enum StandardLibrary {
    static let parts: [StandardPart] = EDAEngine.standardParts()
    static let rulePresets: [DesignRulePreset] = EDAEngine.designRulePresets()
    static let industries: [IndustryProfile] = EDAEngine.industryProfiles()
    /// Memory (RAM) design types for the 5-segment memory checks: SDR SDRAM, DDR memory-down, LPDDR, DIMM, RDIMM.
    static let memoryDesignTypes: [RobotPlatformInfo] = EDAEngine().memorySegments().platforms

    static func industry(_ id: String) -> IndustryProfile? { industries.first { $0.id == id } }
}

/// Industry design profile (standards, design rules, derating, altitude class, guidance) from the core.
struct IndustryProfile: Decodable, Equatable, Identifiable, Hashable {
    var id: String
    var name: String
    var description: String
    var standards: String
    var rulePreset: String
    var powerDerating: Double
    var currentDerating: Double
    var highAltitude: Bool
    var minAmbientC: Double
    var maxAmbientC: Double
    var guidance: [String]

    var systemImage: String {
        switch id {
        case "robotics": return "gearshape.2"
        case "uav": return "airplane"
        case "power": return "bolt.fill"
        case "automotive": return "car"
        case "rf": return "antenna.radiowaves.left.and.right"
        case "space": return "globe.americas"
        case "marine": return "ferry"
        case "industrial": return "building.2"
        case "medical": return "cross.case"
        case "defence": return "shield.lefthalf.filled"
        case "networking": return "network"
        case "vlsi": return "cpu"
        case "motherboard": return "desktopcomputer"
        case "server": return "server.rack"
        case "hpc": return "cpu.fill"
        case "arm": return "memorychip"
        case "addin": return "rectangle.on.rectangle"
        case "retail": return "creditcard"
        case "appliance": return "washer"
        case "memory": return "memorychip.fill"
        case "iot": return "applewatch"
        default: return "cpu"
        }
    }

    /// "Power 50 % · current 50 % of rating · −55…125 °C"
    var deratingSummary: String {
        let derate = powerDerating < 1 || currentDerating < 1
            ? "Power \(Int((powerDerating * 100).rounded())) % · current \(Int((currentDerating * 100).rounded())) % of rating"
            : "No derating"
        return "\(derate) · \(Int(minAmbientC))…\(Int(maxAmbientC)) °C"
    }
}

/// IEC 60063 preferred-number series (raw value = values per decade, as `sieda_nearest_standard_value` expects).
enum ESeries: Int, CaseIterable, Identifiable {
    case e12 = 12
    case e24 = 24
    case e96 = 96

    var id: Int { rawValue }
    var title: String { "E\(rawValue)" }
    var tolerance: String {
        switch self {
        case .e12: return "±10 %"
        case .e24: return "±5 %"
        case .e96: return "±1 %"
        }
    }

    func nearest(_ value: Double) -> Double { EDAEngine.nearestStandardValue(value, series: self) }
    func contains(_ value: Double) -> Bool { EDAEngine.isStandardValue(value, series: self) }

    /// Series a component kind's values are normally chosen from.
    static func preferred(for kind: ComponentKind) -> [ESeries] {
        switch kind {
        case .resistor: return [.e24, .e96]
        case .capacitor, .inductor: return [.e12, .e24]
        default: return []
        }
    }
}

/// Parametric part search, typed into the library and device-picker search fields:
/// `cat:sensors pkg:soic pins:8 mfr:ti 3.3 V`. Filters: `cat:` category, `pkg:` package (type or name, "sot-23"
/// matches SOT23), `pins:` pin count (`8`, `6-10`, `>40`, `<8`), `mfr:` manufacturer. Every other word must appear
/// in the part's name, description, category, manufacturer or package (`wordMatches`, as MCP `library_search`).
/// All terms must match.
struct PartQuery: Equatable {
    var words: [String] = []
    /// `fav:` — only the user's favourite parts; `tag:x` — parts the user tagged x (`LibraryMarks`).
    var favouritesOnly = false
    var tags: [String] = []
    var categories: [String] = []
    var packages: [String] = []
    var manufacturers: [String] = []
    var pinRange: ClosedRange<Int>?

    init(_ text: String) {
        for raw in text.lowercased().split(whereSeparator: \.isWhitespace).map(String.init) {
            guard let colon = raw.firstIndex(of: ":"), colon != raw.startIndex else {
                words.append(raw)
                continue
            }
            let key = String(raw[..<colon]), value = String(raw[raw.index(after: colon)...])
            if key == "fav" || key == "favourites" || key == "favorites" { favouritesOnly = true; continue }
            guard !value.isEmpty else { continue }
            switch key {
            case "cat", "category": categories.append(value)
            case "pkg", "package": packages.append(Self.compact(value))
            case "mfr", "maker", "manufacturer": manufacturers.append(value)
            case "tag": tags.append(value)
            case "pins", "pin":
                if let range = Self.range(value) { pinRange = range } else { words.append(raw) }
            default: words.append(raw)
            }
        }
    }

    var isEmpty: Bool {
        words.isEmpty && categories.isEmpty && packages.isEmpty && manufacturers.isEmpty && pinRange == nil && !favouritesOnly && tags.isEmpty
    }

    /// Lower case without separators, so "SOT-23-5", "sot23 5" and "SOT23" compare alike.
    static func compact(_ s: String) -> String { s.lowercased().filter { $0.isLetter || $0.isNumber } }

    /// "8" → 8…8, "6-10" → 6…10, ">40" → 41…, "<8" → …7, ">=40" / "<=8" inclusive.
    static func range(_ value: String) -> ClosedRange<Int>? {
        if value.hasPrefix(">=") { return Int(value.dropFirst(2)).map { $0...Int.max } }
        if value.hasPrefix("<=") { return Int(value.dropFirst(2)).map { 0...max($0, 0) } }
        if value.hasPrefix(">") { return Int(value.dropFirst()).map { ($0 + 1)...Int.max } }
        if value.hasPrefix("<") { return Int(value.dropFirst()).flatMap { $0 >= 1 ? 0...($0 - 1) : nil } }
        let bounds = value.split(separator: "-", maxSplits: 1).map { Int($0) }
        if bounds.count == 2, let lo = bounds[0], let hi = bounds[1], lo <= hi { return lo...hi }
        if bounds.count == 1, let n = bounds[0] { return n...n }
        return nil
    }

    func matches(name: String, description: String, category: String, manufacturer: String, package: String,
                 pinCount: Int) -> Bool {
        let category = category.lowercased(), manufacturer = manufacturer.lowercased(), packageKey = Self.compact(package)
        if !categories.allSatisfy({ category.contains($0) }) { return false }
        if !manufacturers.allSatisfy({ manufacturer.contains($0) }) { return false }
        if !packages.allSatisfy({ packageKey.contains($0) }) { return false }
        if let pinRange, !pinRange.contains(pinCount) { return false }
        let haystack = [name, description, category, manufacturer, package].joined(separator: " ").lowercased()
        return words.allSatisfy { Self.wordMatches(haystack, $0) }
    }

    /// A short word (≤ 3 letters) starts a word of the text ("ble" finds "BLE-MODULE", not "adjustable"; "esp" finds
    /// "ESP32"), a longer one appears in it, separators ignored ("wifi" finds "Wi-Fi"); "ble" / "bt" also find
    /// Bluetooth and "wlan" Wi-Fi.
    static func wordMatches(_ haystack: String, _ word: String) -> Bool {
        let alias = ["ble": "bluetooth", "bt": "bluetooth", "wlan": "wifi"]
        if let other = alias[word], wordMatches(haystack, other) { return true }
        if word.count <= 3, word.allSatisfy(\.isLetter) {
            var rest = haystack[...]
            while let r = rest.range(of: word) {
                if r.lowerBound == haystack.startIndex || !(haystack[haystack.index(before: r.lowerBound)].isLetter
                    || haystack[haystack.index(before: r.lowerBound)].isNumber) { return true }
                rest = haystack[r.upperBound...]
            }
            return false
        }
        return haystack.contains(word) || (!compact(word).isEmpty && compact(haystack).contains(compact(word)))
    }

    func matches(_ part: StandardPart) -> Bool {
        let marks = LibraryMarks.tags[part.spec.name] ?? []
        if favouritesOnly && !LibraryMarks.favourites.contains(part.spec.name) { return false }
        if !tags.allSatisfy({ t in marks.contains { $0.lowercased().contains(t) } }) { return false }
        return matches(name: part.spec.name, description: ([part.spec.description] + marks).joined(separator: " "), category: part.category,
                manufacturer: part.spec.manufacturer, package: "\(part.spec.package.type)-\(part.spec.package.pinCount)",
                pinCount: part.spec.pins.count)
    }

    func matches(_ part: CustomPartInfo) -> Bool {
        matches(name: part.name, description: part.description, category: "", manufacturer: part.manufacturer,
                package: part.footprint, pinCount: part.pins.count)
    }
}

/// A part from the core's built-in standard library (NE555, LM7805, LM358, ATmega328P, …).
struct StandardPart: Decodable, Equatable, Identifiable {
    var category: String
    var spec: CustomPartSpec
    var id: String { spec.name }

    /// "U1 · DIP-8 · 8 pins"
    var packageSummary: String {
        var text = "\(spec.package.type)-\(spec.package.pinCount)"
        if let pitch = spec.package.pitch { text += String(format: " %gmm", pitch) }
        return text + " · \(spec.pins.count) pins"
    }
}

/// Named design-rule set (design values and fabrication minimums, millimetres).
struct DesignRulePreset: Decodable, Equatable, Identifiable, Hashable {
    var name: String
    var description: String
    var trackWidth: Double
    var clearance: Double
    var viaDrill: Double
    var viaDiameter: Double
    var edgeClearance: Double
    var minTrackWidth: Double
    var minClearance: Double
    var minDrill: Double
    var minAnnularRing: Double
    var minHoleToHole: Double
    var id: String { name }
}

// MARK: - Verification

enum VerificationStatus: String, Decodable {
    case pass, warning, fail, skipped

    var title: String {
        switch self {
        case .pass: return "Pass"
        case .warning: return "Pass with warnings"
        case .fail: return "Fail"
        case .skipped: return "Skipped"
        }
    }

    var systemImage: String {
        switch self {
        case .pass: return "checkmark.seal.fill"
        case .warning: return "exclamationmark.triangle.fill"
        case .fail: return "xmark.octagon.fill"
        case .skipped: return "minus.circle"
        }
    }
}

struct VerificationStage: Decodable, Equatable, Identifiable {
    var id: String
    var title: String
    var status: VerificationStatus
    var summary: String
    var details: [String]
    var findings: [RuleViolation]

    private enum CodingKeys: String, CodingKey { case id, title, status, summary, details, findings }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        id = try c.decode(String.self, forKey: .id)
        title = try c.decode(String.self, forKey: .title)
        status = try c.decode(VerificationStatus.self, forKey: .status)
        summary = try c.decode(String.self, forKey: .summary)
        details = try c.decode([String].self, forKey: .details)
        findings = try c.decode([RuleViolation].self, forKey: .findings).numbered()
    }

    var systemImage: String {
        switch id {
        case "erc": return "bolt.shield"
        case "simulation": return "waveform.path.ecg"
        case "validation": return "checklist"
        case "placement": return "square.on.square.dashed"
        case "routing": return "point.topleft.down.to.point.bottomright.curvepath"
        case "drc": return "square.grid.3x3.square"
        case "reliability": return "shield.lefthalf.filled"
        case "manufacturing": return "shippingbox"
        default: return "checkmark.circle"
        }
    }

    /// Editor that shows the parts a finding refers to.
    var workspace: Workspace {
        switch id {
        case "erc", "validation": return .schematic
        case "simulation": return .simulation
        default: return .pcb
        }
    }
}

struct VerificationReport: Decodable, Equatable {
    var project: String
    var industry: String
    var industryName: String
    var industryStandards: String
    var rulePreset: String
    var layerCount: Int
    var verdict: VerificationStatus
    var passed: Bool
    var errors: Int
    var warnings: Int
    var infos: Int
    var stages: [VerificationStage]
    var markdown: String
}

/// The user's own library marks, kept in user defaults across projects: favourite parts and free tags (by part name).
/// The library search finds them with `fav:` and `tag:x`, and plain words match tags too.
enum LibraryMarks {
    static var favourites: Set<String> {
        get { Set(UserDefaults.standard.stringArray(forKey: "library.favourites") ?? []) }
        set { UserDefaults.standard.set(newValue.sorted(), forKey: "library.favourites") }
    }
    static var tags: [String: [String]] {
        get { UserDefaults.standard.dictionary(forKey: "library.tags") as? [String: [String]] ?? [:] }
        set { UserDefaults.standard.set(newValue, forKey: "library.tags") }
    }
    static func toggleFavourite(_ name: String) {
        if favourites.contains(name) { favourites.remove(name) } else { favourites.insert(name) }
    }
    /// Tags from comma-separated text ("" removes them).
    static func setTags(_ name: String, _ text: String) {
        let list = text.split(separator: ",").map { $0.trimmingCharacters(in: .whitespaces) }.filter { !$0.isEmpty }
        tags[name] = list.isEmpty ? nil : list
    }
}
