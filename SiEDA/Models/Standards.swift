import Foundation

// Built-in standards exposed by the core: standard parts, E-series values, PCB design-rule presets and the
// design verification report.

/// Core standards, loaded once (they are compiled into the core and never change at run time).
enum StandardLibrary {
    static let parts: [StandardPart] = EDAEngine.standardParts()
    static let rulePresets: [DesignRulePreset] = EDAEngine.designRulePresets()
    static let industries: [IndustryProfile] = EDAEngine.industryProfiles()

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
