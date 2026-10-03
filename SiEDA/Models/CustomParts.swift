import Foundation

/// Electrical pin types understood by the core's ERC (raw values match `pinTypeName` in CustomParts.cpp).
enum PinElectricalType: String, CaseIterable, Identifiable, Codable {
    case passive
    case input
    case output
    case bidirectional
    case powerIn = "power_in"
    case powerOut = "power_out"
    case openCollector = "open_collector"
    case noConnect = "no_connect"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .passive: return "Passive"
        case .input: return "Input"
        case .output: return "Output"
        case .bidirectional: return "I/O"
        case .powerIn: return "Power In"
        case .powerOut: return "Power Out"
        case .openCollector: return "Open Collector"
        case .noConnect: return "No Connect"
        }
    }

    var symbol: String {
        switch self {
        case .passive: return "minus"
        case .input: return "arrow.right"
        case .output: return "arrow.left"
        case .bidirectional: return "arrow.left.arrow.right"
        case .powerIn: return "bolt"
        case .powerOut: return "bolt.fill"
        case .openCollector: return "arrow.down.right"
        case .noConnect: return "xmark"
        }
    }

    init(lenient raw: String) {
        let key = raw.lowercased().filter { $0.isLetter || $0.isNumber }
        switch key {
        case "input", "in", "i", "digitalinput": self = .input
        case "output", "out", "o", "digitaloutput": self = .output
        case "bidirectional", "io", "inout", "bidir", "gpio": self = .bidirectional
        case "powerin", "power", "pwr", "supply", "vcc", "gnd", "ground": self = .powerIn
        case "powerout", "pwrout", "vout": self = .powerOut
        case "opencollector", "opendrain", "od", "oc": self = .openCollector
        case "noconnect", "nc", "notconnected", "unused": self = .noConnect
        default: self = .passive
        }
    }

    /// Guesses a type from a datasheet pin name/description.
    static func infer(name: String, description: String) -> PinElectricalType {
        let n = name.uppercased()
        let d = description.lowercased()
        if n == "NC" || n == "N/C" || n == "DNC" || d.contains("no connect") || d.contains("not connected") { return .noConnect }
        if ["VCC", "VDD", "VSS", "GND", "AGND", "DGND", "PGND", "VEE", "V+", "V-", "VIN", "AVCC", "AVDD", "DVDD", "VBAT"].contains(n)
            || d.contains("supply") || d.contains("ground") { return .powerIn }
        if n.hasPrefix("VOUT") || d.contains("regulated output") { return .powerOut }
        if d.contains("open drain") || d.contains("open-drain") || d.contains("open collector") { return .openCollector }
        let isPortPin = n.range(of: #"^P[A-Z]?\d{1,2}$"#, options: .regularExpression) != nil  // PB0, P13
        if d.contains("input/output") || d.contains("i/o") || d.contains("bidirectional") || n.hasPrefix("GPIO") || isPortPin {
            return .bidirectional
        }
        if d.contains("output") || n == "OUT" || n.hasSuffix("OUT") || n == "TX" || n == "TXD" || n == "MISO" { return .output }
        if d.contains("input") || n == "IN" || n.hasPrefix("IN") || n == "RX" || n == "RXD" || n == "MOSI" || n == "SCK" || n == "CS" || n == "EN" {
            return .input
        }
        return .passive
    }
}

/// Packages the core can generate footprints for (matches `supportedPackages()`).
enum PackageKind: String, CaseIterable, Identifiable, Codable {
    case soic = "SOIC"
    case tssop = "TSSOP"
    case dip = "DIP"
    case qfn = "QFN"
    case lqfp = "LQFP"
    case sot23 = "SOT23"
    case header = "HEADER"
    case header2 = "HEADER2"
    case to220 = "TO220"
    case hc49 = "HC49"
    case disc = "DISC"
    case module = "MODULE"

    var id: String { rawValue }

    var title: String {
        switch self {
        case .soic: return "SOIC (1.27 mm)"
        case .tssop: return "TSSOP (0.65 mm)"
        case .dip: return "DIP (2.54 mm, THT)"
        case .qfn: return "QFN (0.5 mm)"
        case .lqfp: return "LQFP (0.5 mm)"
        case .sot23: return "SOT-23 (3/5/6)"
        case .header: return "Pin Header 1×N"
        case .header2: return "Pin Header 2×N (IDC)"
        case .to220: return "TO-220 (THT)"
        case .hc49: return "HC-49 crystal (THT)"
        case .disc: return "Radial disc (MOV / GDT, THT)"
        case .module: return "RF module, castellated (1.27 mm)"
        }
    }

    /// Normalises datasheet/LLM package strings ("SOIC-8", "PDIP", "SOT-23-5", "TO-220").
    static func guess(_ raw: String) -> PackageKind? {
        let u = raw.uppercased()
        if u.contains("TSSOP") || u.contains("MSOP") || u.contains("SSOP") { return .tssop }
        if u.contains("SOIC") || u.contains("SOP") || u.hasPrefix("SO-") || u.hasPrefix("SO8") { return .soic }
        if u.contains("DIP") { return .dip }
        if u.contains("QFN") || u.contains("DFN") || u.contains("MLF") { return .qfn }
        if u.contains("QFP") { return .lqfp }
        if u.contains("SOT") { return .sot23 }
        if u.contains("TO-220") || u.contains("TO220") || u.contains("TO-92") || u.contains("TO92") { return .to220 }
        if u.contains("HC49") || u.contains("HC-49") { return .hc49 }
        if u.contains("RADIAL DISC") || u.hasPrefix("DISC") { return .disc }
        if u.contains("CASTELLATED") || u.contains("WROOM") || u.hasPrefix("MODULE") { return .module }
        if u.contains("2X") || u.contains("IDC") || u.contains("DUAL ROW") || u.contains("BOX HEADER") { return .header2 }
        if u.contains("HEADER") || u.contains("SIP") || u.contains("1X") { return .header }
        return nil
    }
}

/// Behavioural simulation model of a part (regulator/charger and IC supply loads), as the core's JSON "model".
struct BehaviorModel: Codable, Equatable {
    struct Regulator: Codable, Equatable {
        var input: String
        var output: String
        var ref: String
        var vout: Double
        var dropout: Double = 0.3
        var iq: Double = 0
        var ilimit: Double = 1
        var maxPower: Double = 0.5
        var charger = false
        /// Isolated DC-DC: primary return pin and conversion efficiency (nil = non-isolated regulator).
        var inReturn: String?
        var efficiency: Double?
        /// Current-limited load switch: the output follows the input.
        var loadSwitch: Bool?

        private enum CodingKeys: String, CodingKey {
            case input = "in", output = "out", ref, vout, dropout, iq, ilimit, maxPower, charger, inReturn, efficiency, loadSwitch
        }
    }

    struct Load: Codable, Equatable {
        var supply: String
        var ret: String
        var current: Double
    }

    var regulator: Regulator?
    var loads: [Load]?

    /// "Regulator 3.3 V" / "Charger 4.2 V" / "Supply 4 mA"
    var summary: String {
        var parts: [String] = []
        if let r = regulator {
            parts.append("\(r.charger ? "Charger" : "Regulator") \(String(format: "%g", r.vout)) V")
        }
        let total = (loads ?? []).reduce(0) { $0 + $1.current }
        if total > 0 { parts.append("Supply \(EngineeringFormat.string(total, unit: "A"))") }
        return parts.joined(separator: " · ")
    }
}

/// Editable definition of a custom component (sent to the core as JSON).
struct CustomPartSpec: Codable, Equatable {
    struct Package: Codable, Equatable {
        var type: String = PackageKind.soic.rawValue
        var pinCount: Int = 0
        /// Lead pitch and body size in mm (nil = the package type's default); set for the standard microcontrollers.
        var pitch: Double?
        var bodySize: Double?
    }

    struct Pin: Codable, Equatable, Identifiable {
        var id = UUID()
        var number: String
        var name: String
        var type: PinElectricalType = .passive
        var description: String = ""

        private enum CodingKeys: String, CodingKey { case number, name, type, description }

        /// Content equality (the random `id` only keeps SwiftUI rows stable).
        static func == (lhs: Pin, rhs: Pin) -> Bool {
            lhs.number == rhs.number && lhs.name == rhs.name && lhs.type == rhs.type && lhs.description == rhs.description
        }

        init(number: String, name: String, type: PinElectricalType = .passive, description: String = "") {
            self.number = number
            self.name = name
            self.type = type
            self.description = description
        }

        init(from decoder: Decoder) throws {
            let c = try decoder.container(keyedBy: CodingKeys.self)
            if let n = try? c.decode(String.self, forKey: .number) {
                number = n
            } else if let n = try? c.decode(Int.self, forKey: .number) {
                number = String(n)
            } else {
                number = ""
            }
            name = try c.decodeIfPresent(String.self, forKey: .name) ?? ""
            type = PinElectricalType(lenient: try c.decodeIfPresent(String.self, forKey: .type) ?? "passive")
            description = try c.decodeIfPresent(String.self, forKey: .description) ?? ""
        }
    }

    var name: String = ""
    var manufacturer: String = ""
    var description: String = ""
    var refPrefix: String = "U"
    var defaultValue: String = ""
    var datasheet: String = ""
    var package = Package()
    var pins: [Pin] = []
    /// Simulation model (kept when a standard or library part is re-registered; nil = no model).
    var model: BehaviorModel?

    func jsonString() -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(self) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }

    /// Problems that prevent registration, phrased for the editor.
    var validationIssues: [String] {
        var issues: [String] = []
        if name.trimmingCharacters(in: .whitespaces).isEmpty { issues.append("Give the part a name.") }
        if pins.isEmpty { issues.append("Add at least one pin.") }
        var seen = Set<String>()
        for pin in pins {
            let key = pin.number.trimmingCharacters(in: .whitespaces).uppercased()
            if key.isEmpty { issues.append("Every pin needs a number."); break }
            if !seen.insert(key).inserted { issues.append("Pin number \(pin.number) is used twice."); break }
        }
        return issues
    }
}

/// A registered part as returned by the core (spec + generated symbol + footprint geometry).
struct CustomPartInfo: Decodable, Equatable, Identifiable {
    struct SymbolPin: Decodable, Equatable {
        var name: String
        var number: String
        var type: String
        var x: Double
        var y: Double
    }

    struct Symbol: Decodable, Equatable {
        var halfWidth: Double
        var halfHeight: Double
        var pins: [SymbolPin]
    }

    struct FootprintPad: Decodable, Equatable {
        var number: Int
        var pin: Int
        var x: Double
        var y: Double
        var w: Double
        var h: Double
        var throughHole: Bool
        var round: Bool
    }

    struct FootprintGeometry: Decodable, Equatable {
        var label: String
        var pads: [FootprintPad]
        var courtyardW: Double
        var courtyardH: Double
        var bodyW: Double
        var bodyD: Double
        var bodyH: Double
    }

    var id: String
    var name: String
    var manufacturer: String
    var description: String
    var refPrefix: String
    var defaultValue: String
    var datasheet: String
    var package: CustomPartSpec.Package
    var pins: [CustomPartSpec.Pin]
    var footprint: String
    var symbol: Symbol
    var footprintGeometry: FootprintGeometry
    var model: BehaviorModel?

    var spec: CustomPartSpec {
        CustomPartSpec(name: name, manufacturer: manufacturer, description: description, refPrefix: refPrefix,
                       defaultValue: defaultValue, datasheet: datasheet, package: package, pins: pins, model: model)
    }

    /// Kind identifier used in AI design plans.
    var planKind: String { "custom:\(name)" }
}

/// JSON schema for datasheet extraction (structured outputs).
enum DatasheetSchema {
    static var componentDefinition: [String: Any] {
        [
            "type": "object",
            "additionalProperties": false,
            "required": ["name", "manufacturer", "description", "ref_prefix", "default_value", "package", "pins", "notes"],
            "properties": [
                "name": ["type": "string", "description": "Part number without ordering suffix, e.g. NE555"],
                "manufacturer": ["type": "string"],
                "description": ["type": "string", "description": "One-line function, e.g. Precision timer"],
                "ref_prefix": ["type": "string", "description": "U for ICs, Q for transistors, J for connectors"],
                "default_value": ["type": "string"],
                "package": [
                    "type": "object",
                    "additionalProperties": false,
                    "required": ["type", "pin_count"],
                    "properties": [
                        "type": ["type": "string", "enum": PackageKind.allCases.map(\.rawValue)] as [String: Any],
                        "pin_count": ["type": "integer"],
                    ] as [String: Any],
                ] as [String: Any],
                "pins": [
                    "type": "array",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["number", "name", "type", "description"],
                        "properties": [
                            "number": ["type": "string", "description": "Pin number as printed, e.g. 1 or EP"],
                            "name": ["type": "string"],
                            "type": ["type": "string", "enum": PinElectricalType.allCases.map(\.rawValue)] as [String: Any],
                            "description": ["type": "string"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "notes": ["type": "array", "items": ["type": "string"]] as [String: Any],
            ] as [String: Any],
        ]
    }

    /// Model reply → editable spec.
    struct Extraction: Decodable {
        struct Package: Decodable {
            var type: String
            var pinCount: Int?
            enum CodingKeys: String, CodingKey { case type, pinCount = "pin_count" }
        }
        var name: String
        var manufacturer: String?
        var description: String?
        var refPrefix: String?
        var defaultValue: String?
        var package: Package?
        var pins: [CustomPartSpec.Pin]
        var notes: [String]?

        enum CodingKeys: String, CodingKey {
            case name, manufacturer, description, pins, notes, package
            case refPrefix = "ref_prefix"
            case defaultValue = "default_value"
        }

        func spec(datasheet: String) -> CustomPartSpec {
            var s = CustomPartSpec()
            s.name = name
            s.manufacturer = manufacturer ?? ""
            s.description = description ?? ""
            s.refPrefix = (refPrefix?.isEmpty == false ? refPrefix : nil) ?? "U"
            s.defaultValue = (defaultValue?.isEmpty == false ? defaultValue : nil) ?? name
            s.datasheet = datasheet
            // An exact enum value first ("HEADER2" contains no "2X", so guessing alone turns it into a 1×N header).
            s.package.type = (package.flatMap { PackageKind(rawValue: $0.type.uppercased()) ?? PackageKind.guess($0.type) }
                              ?? .soic).rawValue
            s.package.pinCount = package?.pinCount ?? 0
            s.pins = pins
            return s
        }
    }
}
