import CoreGraphics
import Foundation

// Codable mirrors of the JSON produced by the C++ core (`Project::snapshot`, analysis results).

struct DesignSnapshot: Decodable, Equatable {
    var name: String
    var requirements: String
    var components: [SnapComponent]
    var wires: [SnapWire]
    var nets: [SnapNet]
    var board: BoardInfo
    var pads: [SnapPad]
    var tracks: [SnapTrack]
    var vias: [SnapVia]
    var ratsnest: [SnapLine]
    var courtyards: [SnapCourtyard]
    var bodies: [SnapBody] = []
    var customParts: [CustomPartInfo] = []

    static let empty = DesignSnapshot(name: "Untitled", requirements: "", components: [], wires: [], nets: [],
                                      board: BoardInfo(), pads: [], tracks: [], vias: [], ratsnest: [], courtyards: [])

    init(name: String, requirements: String, components: [SnapComponent], wires: [SnapWire], nets: [SnapNet],
         board: BoardInfo, pads: [SnapPad], tracks: [SnapTrack], vias: [SnapVia], ratsnest: [SnapLine],
         courtyards: [SnapCourtyard], bodies: [SnapBody] = [], customParts: [CustomPartInfo] = []) {
        self.name = name
        self.requirements = requirements
        self.components = components
        self.wires = wires
        self.nets = nets
        self.board = board
        self.pads = pads
        self.tracks = tracks
        self.vias = vias
        self.ratsnest = ratsnest
        self.courtyards = courtyards
        self.bodies = bodies
        self.customParts = customParts
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        requirements = try c.decodeIfPresent(String.self, forKey: .requirements) ?? ""
        components = try c.decode([SnapComponent].self, forKey: .components)
        wires = try c.decode([SnapWire].self, forKey: .wires)
        nets = try c.decode([SnapNet].self, forKey: .nets)
        board = try c.decode(BoardInfo.self, forKey: .board)
        pads = try c.decode([SnapPad].self, forKey: .pads)
        tracks = try c.decode([SnapTrack].self, forKey: .tracks)
        vias = try c.decode([SnapVia].self, forKey: .vias)
        ratsnest = try c.decode([SnapLine].self, forKey: .ratsnest)
        courtyards = try c.decode([SnapCourtyard].self, forKey: .courtyards)
        bodies = try c.decodeIfPresent([SnapBody].self, forKey: .bodies) ?? []
        customParts = try c.decodeIfPresent([CustomPartInfo].self, forKey: .customParts) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case name, requirements, components, wires, nets, board, pads, tracks, vias, ratsnest, courtyards, bodies, customParts
    }

    func component(_ id: Int) -> SnapComponent? { components.first { $0.id == id } }
    func customPart(_ id: String?) -> CustomPartInfo? {
        guard let id else { return nil }
        return customParts.first { $0.id == id }
    }
    func customPart(for component: SnapComponent) -> CustomPartInfo? {
        component.componentKind == .custom ? customPart(component.customPart) : nil
    }
    func component(ref: String) -> SnapComponent? { components.first { $0.ref == ref } }
    func net(_ index: Int) -> SnapNet? { index >= 0 && index < nets.count ? nets[index] : nil }
}

struct SnapPin: Decodable, Equatable {
    var name: String
    var x: Double
    var y: Double
    var net: Int
    var connected: Bool
    var point: CGPoint { CGPoint(x: x, y: y) }
}

struct PcbPlacement: Decodable, Equatable {
    var x: Double
    var y: Double
    var rotation: Int
    var bottom: Bool
    var placed: Bool
}

struct SnapComponent: Decodable, Equatable, Identifiable {
    var id: Int
    var kind: Int
    var ref: String
    var value: String
    var x: Double
    var y: Double
    var rotation: Int
    var footprint: String
    var pins: [SnapPin]
    var pcb: PcbPlacement
    var customPart: String?

    var componentKind: ComponentKind { ComponentKind(rawValue: kind) ?? .ic8 }
    var position: CGPoint { CGPoint(x: x, y: y) }
}

struct PinAddress: Codable, Equatable, Hashable {
    var component: Int
    var pin: Int
}

struct SnapWire: Decodable, Equatable, Identifiable {
    var id: Int
    var a: PinAddress
    var b: PinAddress
    var ax: Double
    var ay: Double
    var bx: Double
    var by: Double
    var net: Int
    var start: CGPoint { CGPoint(x: ax, y: ay) }
    var end: CGPoint { CGPoint(x: bx, y: by) }
}

struct SnapNet: Decodable, Equatable, Identifiable {
    var index: Int
    var name: String
    var pinCount: Int
    var ground: Bool
    var id: Int { index }
}

struct BoardInfo: Decodable, Equatable {
    var layerCount: Int = 2
    var width: Double = 50
    var height: Double = 40
    var thickness: Double = 1.6
    var trackWidth: Double = 0.25
    var clearance: Double = 0.2
    var viaDrill: Double = 0.3
    var viaDiameter: Double = 0.6
    var edgeClearance: Double = 0.5
    var routingGrid: Double = 0.25

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        layerCount = try c.decodeIfPresent(Int.self, forKey: .layerCount) ?? 2
        width = try c.decode(Double.self, forKey: .width)
        height = try c.decode(Double.self, forKey: .height)
        thickness = try c.decodeIfPresent(Double.self, forKey: .thickness) ?? 1.6
        trackWidth = try c.decodeIfPresent(Double.self, forKey: .trackWidth) ?? 0.25
        clearance = try c.decodeIfPresent(Double.self, forKey: .clearance) ?? 0.2
        viaDrill = try c.decodeIfPresent(Double.self, forKey: .viaDrill) ?? 0.3
        viaDiameter = try c.decodeIfPresent(Double.self, forKey: .viaDiameter) ?? 0.6
        edgeClearance = try c.decodeIfPresent(Double.self, forKey: .edgeClearance) ?? 0.5
        routingGrid = try c.decodeIfPresent(Double.self, forKey: .routingGrid) ?? 0.25
    }

    private enum CodingKeys: String, CodingKey {
        case layerCount, width, height, thickness, trackWidth, clearance, viaDrill, viaDiameter, edgeClearance, routingGrid
    }

    var bottomLayer: Int { max(1, layerCount) - 1 }

    /// "Top", "Inner 1", …, "Bottom"
    func layerName(_ layer: Int) -> String {
        if layer == 0 { return "Top" }
        if layer == bottomLayer { return "Bottom" }
        return "Inner \(layer)"
    }
}

/// Component body (mm) used by the 3D X-ray view.
struct SnapBody: Decodable, Equatable {
    var component: Int
    var x: Double
    var y: Double
    var w: Double
    var d: Double
    var h: Double
    var bottom: Bool
    var package: String
}

struct SnapPad: Decodable, Equatable {
    var component: Int
    var pin: Int
    var number: Int
    var net: Int
    var x: Double
    var y: Double
    var w: Double
    var h: Double
    var throughHole: Bool
    var round: Bool
    var drill: Double
    var bottom: Bool
    var rect: CGRect { CGRect(x: x - w / 2, y: y - h / 2, width: w, height: h) }
}

struct SnapTrack: Decodable, Equatable, Identifiable {
    var id: Int
    var net: Int
    var layer: Int
    var width: Double
    var ax: Double
    var ay: Double
    var bx: Double
    var by: Double
}

struct SnapVia: Decodable, Equatable, Identifiable {
    var id: Int
    var net: Int
    var x: Double
    var y: Double
    var drill: Double
    var diameter: Double
}

struct SnapLine: Decodable, Equatable {
    var ax: Double
    var ay: Double
    var bx: Double
    var by: Double
}

struct SnapCourtyard: Decodable, Equatable {
    var component: Int
    var x0: Double
    var y0: Double
    var x1: Double
    var y1: Double
    var rect: CGRect { CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0) }
}

// MARK: - Rule checks

enum ViolationSeverity: String, Decodable, Comparable {
    case info, warning, error

    private var rank: Int {
        switch self {
        case .info: return 0
        case .warning: return 1
        case .error: return 2
        }
    }
    static func < (lhs: ViolationSeverity, rhs: ViolationSeverity) -> Bool { lhs.rank < rhs.rank }
}

struct RuleViolation: Decodable, Equatable, Identifiable {
    var severity: ViolationSeverity
    var code: String
    var message: String
    var components: [Int]
    var hasLocation: Bool
    var x: Double
    var y: Double
    var id: String { "\(code)|\(message)|\(x)|\(y)" }
}

// MARK: - Simulation results

struct DCNetVoltage: Decodable, Equatable, Identifiable {
    var index: Int
    var name: String
    var voltage: Double
    var id: Int { index }
}

struct DCDeviceReading: Decodable, Equatable, Identifiable {
    var component: Int
    var ref: String
    var current: Double
    var power: Double
    var voltage: Double
    var id: Int { component }
}

struct DCResult: Decodable, Equatable {
    var converged: Bool
    var error: String
    var iterations: Int
    var nets: [DCNetVoltage]
    var devices: [DCDeviceReading]

    init(converged: Bool = false, error: String = "", iterations: Int = 0, nets: [DCNetVoltage] = [],
         devices: [DCDeviceReading] = []) {
        self.converged = converged
        self.error = error
        self.iterations = iterations
        self.nets = nets
        self.devices = devices
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        converged = try c.decodeIfPresent(Bool.self, forKey: .converged) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        iterations = try c.decodeIfPresent(Int.self, forKey: .iterations) ?? 0
        nets = try c.decodeIfPresent([DCNetVoltage].self, forKey: .nets) ?? []
        devices = try c.decodeIfPresent([DCDeviceReading].self, forKey: .devices) ?? []
    }

    private enum CodingKeys: String, CodingKey { case converged, error, iterations, nets, devices }

    func voltage(net: Int) -> Double? { nets.first { $0.index == net }?.voltage }
    func reading(component: Int) -> DCDeviceReading? { devices.first { $0.component == component } }
}

struct WaveformSeries: Decodable, Equatable, Identifiable {
    var index: Int?
    var component: Int?
    var name: String?
    var ref: String?
    var values: [Double]
    var id: String { name ?? ref ?? "\(index ?? component ?? -1)" }
    var label: String { name ?? ref ?? "?" }
}

struct TransientResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var time: [Double]
    var nets: [WaveformSeries]
    var currents: [WaveformSeries]

    init(ok: Bool = false, error: String = "", time: [Double] = [], nets: [WaveformSeries] = [],
         currents: [WaveformSeries] = []) {
        self.ok = ok
        self.error = error
        self.time = time
        self.nets = nets
        self.currents = currents
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        time = try c.decodeIfPresent([Double].self, forKey: .time) ?? []
        nets = try c.decodeIfPresent([WaveformSeries].self, forKey: .nets) ?? []
        currents = try c.decodeIfPresent([WaveformSeries].self, forKey: .currents) ?? []
    }

    private enum CodingKeys: String, CodingKey { case ok, error, time, nets, currents }
}

struct RouteStats: Decodable, Equatable {
    var connections: Int = 0
    var routed: Int = 0
    var failed: Int = 0
    var vias: Int = 0
    var trackLength: Double = 0
    var failedNets: [String] = []
}

// MARK: - 3D

struct MeshData {
    var positions: [Float]
    var normals: [Float]
    var colors: [Float]
    var indices: [UInt32]
    var vertexCount: Int { positions.count / 3 }
}

// MARK: - Formatting

enum EngineeringFormat {
    /// 4700 → "4.7 k", 0.0021 → "2.1 m" (with unit appended).
    static func string(_ value: Double, unit: String, digits: Int = 3) -> String {
        guard value.isFinite else { return "—" }
        if value == 0 || abs(value) < 1e-15 { return "0 \(unit)" }
        let prefixes: [(Double, String)] = [(1e12, "T"), (1e9, "G"), (1e6, "M"), (1e3, "k"), (1, ""),
                                            (1e-3, "m"), (1e-6, "µ"), (1e-9, "n"), (1e-12, "p"), (1e-15, "f")]
        let mag = abs(value)
        for (scale, prefix) in prefixes where mag >= scale * 0.9995 || scale == 1e-15 {
            let scaled = value / scale
            let formatted = String(format: "%.\(digits)g", scaled)
            return "\(formatted) \(prefix)\(unit)"
        }
        return "\(value) \(unit)"
    }

    /// Parses "4.7k", "10u", "1e-3", "2m" → Double (SI prefixes, case-sensitive m/M).
    static func parse(_ text: String) -> Double? {
        let trimmed = text.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty else { return nil }
        var numberPart = ""
        var rest = Substring(trimmed)
        while let ch = rest.first, ch.isNumber || ch == "." || ch == "-" || ch == "+" || ch == "e" || ch == "E" {
            if (ch == "e" || ch == "E"), !(rest.dropFirst().first.map { $0.isNumber || $0 == "-" || $0 == "+" } ?? false) {
                break
            }
            numberPart.append(ch)
            rest = rest.dropFirst()
        }
        guard let base = Double(numberPart) else { return nil }
        let multipliers: [Character: Double] = ["f": 1e-15, "p": 1e-12, "n": 1e-9, "u": 1e-6, "µ": 1e-6,
                                                "m": 1e-3, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9]
        if let first = rest.first, let mult = multipliers[first] { return base * mult }
        return base
    }
}
