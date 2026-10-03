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
    var industry = "general"
    /// Robot platform id ("rover", "fpv", "arm", "quadruped", "humanoid"); empty when the design is not a robot.
    var robotPlatform = ""
    /// Automotive ECU type ("bcm", "powertrain", "adas", "ev", "chassis", "gateway"); empty when not set.
    var ecuType = ""
    /// Aerospace mission ("leo", "geo", "launcher", "military", "commercial"); empty when not set.
    var aerospaceMission = ""
    /// Naval platform ("combatant", "carrier", "submarine", "patrol", "commercial"); empty when not set.
    var navalPlatform = ""
    /// Medical device class ("bf", "cf", "life", "implant", "home"); empty when not set.
    var medicalClass = ""
    /// Retail device class ("countertop", "unattended", "mpos", "kiosk", "printer"); empty when not set.
    var retailDevice = ""
    var zones: [CopperZoneInfo] = []
    /// Active tamper meshes laid over secure elements by the autorouter.
    var tamperMeshes: [TamperMeshInfo] = []
    var zoneFills: [ZoneFillInfo] = []

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
        industry = try c.decodeIfPresent(String.self, forKey: .industry) ?? "general"
        robotPlatform = try c.decodeIfPresent(String.self, forKey: .robotPlatform) ?? ""
        ecuType = try c.decodeIfPresent(String.self, forKey: .ecuType) ?? ""
        aerospaceMission = try c.decodeIfPresent(String.self, forKey: .aerospaceMission) ?? ""
        navalPlatform = try c.decodeIfPresent(String.self, forKey: .navalPlatform) ?? ""
        medicalClass = try c.decodeIfPresent(String.self, forKey: .medicalClass) ?? ""
        retailDevice = try c.decodeIfPresent(String.self, forKey: .retailDevice) ?? ""
        tamperMeshes = try c.decodeIfPresent([TamperMeshInfo].self, forKey: .tamperMeshes) ?? []
        zones = try c.decodeIfPresent([CopperZoneInfo].self, forKey: .zones) ?? []
        zoneFills = try c.decodeIfPresent([ZoneFillInfo].self, forKey: .zoneFills) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case name, requirements, components, wires, nets, board, pads, tracks, vias, ratsnest, courtyards, bodies, customParts
        case industry, robotPlatform, ecuType, aerospaceMission, navalPlatform, medicalClass, retailDevice, zones, zoneFills
        case tamperMeshes
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
    /// Marked "no connect" (left open on purpose; ERC does not report it).
    var noConnect = false
    var point: CGPoint { CGPoint(x: x, y: y) }

    init(name: String, x: Double, y: Double, net: Int, connected: Bool, noConnect: Bool = false) {
        self.name = name
        self.x = x
        self.y = y
        self.net = net
        self.connected = connected
        self.noConnect = noConnect
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        name = try c.decode(String.self, forKey: .name)
        x = try c.decode(Double.self, forKey: .x)
        y = try c.decode(Double.self, forKey: .y)
        net = try c.decode(Int.self, forKey: .net)
        connected = try c.decode(Bool.self, forKey: .connected)
        noConnect = try c.decodeIfPresent(Bool.self, forKey: .noConnect) ?? false
    }

    private enum CodingKeys: String, CodingKey { case name, x, y, net, connected, noConnect }
}

struct PcbPlacement: Decodable, Equatable {
    var x: Double
    var y: Double
    var rotation: Int
    var bottom: Bool
    var placed: Bool
    /// Locked footprints stay where they are on Auto Place.
    var locked: Bool?
    /// Embedded passive: the inner copper layer it is formed on (0 / nil = surface part).
    var embeddedLayer: Int?
    var isEmbedded: Bool { (embeddedLayer ?? 0) > 0 }
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
    /// Present for microcontrollers the simulator can run (ATmega328P, ATtiny85).
    var mcu: McuInfo?

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

/// Where a schematic wire being drawn starts or ends: a pin, a point on an existing wire (a T-junction is made there)
/// or a free point (a bend: the wire continues from a junction there).
enum WireEnd: Equatable {
    case pin(PinAddress)
    case wire(Int, CGPoint)
    case point(CGPoint)
}

/// Schematic wire routing: a wire is drawn as an orthogonal L (horizontal first) between its two ends.
enum WireGeometry {
    static let grid: CGFloat = 10

    static func path(_ a: CGPoint, _ b: CGPoint) -> [CGPoint] {
        if a.x == b.x || a.y == b.y { return [a, b] }
        return [a, CGPoint(x: b.x, y: a.y), b]
    }

    /// The point of a wire's route nearest to `p`, on the grid where the segment allows, and its distance from `p`.
    static func nearestPoint(on wire: SnapWire, to p: CGPoint) -> (point: CGPoint, distance: CGFloat) {
        let pts = path(wire.start, wire.end)
        var best = (point: wire.start, distance: CGFloat.greatestFiniteMagnitude)
        for i in 0..<(pts.count - 1) {
            let q = nearestPoint(onSegment: pts[i], pts[i + 1], to: p)
            let d = hypot(q.x - p.x, q.y - p.y)
            if d < best.distance { best = (q, d) }
        }
        return best
    }

    static func nearestPoint(onSegment a: CGPoint, _ b: CGPoint, to p: CGPoint) -> CGPoint {
        let dx = b.x - a.x, dy = b.y - a.y
        let len2 = dx * dx + dy * dy
        let t = len2 > 0 ? max(0, min(1, ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2)) : 0
        var q = CGPoint(x: a.x + t * dx, y: a.y + t * dy)
        // Along a horizontal or vertical run the junction lands on the grid, inside the segment.
        if dy == 0 { q.x = min(max((q.x / grid).rounded() * grid, min(a.x, b.x)), max(a.x, b.x)) }
        if dx == 0 { q.y = min(max((q.y / grid).rounded() * grid, min(a.y, b.y)), max(a.y, b.y)) }
        return q
    }
}

/// Bill of materials from the core (`sieda_bom_json`).
struct BomReport: Decodable, Equatable {
    var lines: [BomLineInfo]
    var summary: BomSummaryInfo
    var buildQuantity: Int
    var orderCost: Double

    static let empty = BomReport(lines: [], summary: BomSummaryInfo(), buildQuantity: 5, orderCost: 0)
}

struct BomSummaryInfo: Decodable, Equatable {
    var lines = 0
    var placements = 0
    var dnp = 0
    var missingMpn = 0
    var unpriced = 0
    var costPerBoard = 0.0
    var embedded: Int? = nil
}

/// One BOM line: identical parts (type, value, footprint and sourcing) with their designators.
struct BomLineInfo: Decodable, Equatable, Identifiable {
    var item: Int
    var refs: [String]
    var componentIds: [Int]
    var quantity: Int
    var type: String
    var value: String
    var footprint: String
    var description: String
    var rating: String
    var manufacturer: String
    var mpn: String
    var supplierPart: String
    var unitPrice: Double
    var dnp: Bool
    /// Formed inside the PCB (embedded passive): not bought or assembled.
    var embedded: Bool?
    var lineCost: Double
    var suggestedManufacturer: String
    var suggestedMpn: String
    var notes: [String]

    var id: String { refs.joined(separator: ",") }
    var designators: String { refs.joined(separator: ", ") }
}

/// Fields of a BOM line the user edits; nil leaves a field as it is.
struct SourcingUpdate: Equatable {
    var manufacturer: String?
    var mpn: String?
    var supplierPart: String?
    var unitPrice: Double?
    var dnp: Bool?

    var json: String {
        var fields: [String: Any] = [:]
        if let manufacturer { fields["manufacturer"] = manufacturer }
        if let mpn { fields["mpn"] = mpn }
        if let supplierPart { fields["supplierPart"] = supplierPart }
        if let unitPrice { fields["unitPrice"] = unitPrice }
        if let dnp { fields["dnp"] = dnp }
        let data = (try? JSONSerialization.data(withJSONObject: fields)) ?? Data("{}".utf8)
        return String(decoding: data, as: UTF8.self)
    }
}

struct SnapNet: Decodable, Equatable, Identifiable {
    var index: Int
    var name: String
    var pinCount: Int
    var ground: Bool
    /// Core `NetRole`: "power", "ground", "negative" or "signal".
    var role: String?
    var id: Int { index }
    var netRole: NetRole { role.flatMap(NetRole.init(rawValue:)) ?? (ground ? .ground : .signal) }
}

/// What a net carries (core `Schematic::netRole`), used to colour copper: power red, ground blue, negative rails
/// purple, signals in their layer's colour.
enum NetRole: String, CaseIterable {
    case power, ground, negative, signal

    var title: String {
        switch self {
        case .power: return "Power +"
        case .ground: return "Ground −"
        case .negative: return "Negative rail"
        case .signal: return "Signal"
        }
    }
}

/// Conformal coatings (IPC-CC-830): sealing the assembly against moisture and contamination stops surface leakage
/// and lets voltage spacing use IPC-2221B column A5.
enum ConformalCoating: String, CaseIterable, Identifiable {
    case none, acrylic, silicone, urethane, epoxy, parylene

    var id: String { rawValue }
    var title: String {
        switch self {
        case .none: return "None"
        case .acrylic: return "Acrylic (AR)"
        case .silicone: return "Silicone (SR)"
        case .urethane: return "Urethane (UR)"
        case .epoxy: return "Epoxy (ER)"
        case .parylene: return "Parylene (XY)"
        }
    }
}

/// Board construction: rigid FR-4 style, rigid-flex (polyimide flex sections) or metal-core (IMS) for heat.
enum BoardConstruction: String, CaseIterable, Identifiable {
    case rigid
    case rigidFlex = "rigid-flex"
    case metalCore = "metal-core"

    var id: String { rawValue }
    var title: String {
        switch self {
        case .rigid: return "Rigid"
        case .rigidFlex: return "Rigid-flex"
        case .metalCore: return "Metal-core (IMS)"
        }
    }
}

/// Stack-up with impedance-controlled widths per copper layer (`sieda_stackup_json`).
struct StackupReport: Decodable, Equatable {
    var material = "fr4"
    var materialName = "FR-4"
    var er = 4.4
    var lossTangent = 0.02
    var tg = 140.0
    var construction = "rigid"
    var singleEndedOhms = 50.0
    var differentialOhms = 100.0
    var layers: [StackupLayerInfo] = []
    var materials: [LaminateInfo] = []

    static let empty = StackupReport()
}

struct StackupLayerInfo: Decodable, Equatable, Identifiable {
    var name: String
    var type: String
    var thickness: Double
    var line: String?
    var seWidth: Double?
    var diffWidth: Double?
    var diffGap: Double?
    var material: String?

    var id: String { name }
    var isCopper: Bool { type == "copper" }
}

struct LaminateInfo: Decodable, Equatable, Identifiable {
    var id: String
    var name: String
    var er: Double
    var lossTangent: Double
    var tg: Double
    var note: String
}

/// Solder mask colours fabs offer (core `solderMaskStyles()`), green first: the usual board colour.
enum SolderMaskColour: String, CaseIterable, Identifiable {
    case green, black, blue, red, yellow, white, purple

    var id: String { rawValue }
    var title: String { rawValue.capitalized }

    /// Swatch colour (matches the 3D board).
    var swatch: (red: Double, green: Double, blue: Double) {
        switch self {
        case .green: return (0.04, 0.40, 0.20)
        case .black: return (0.05, 0.05, 0.06)
        case .blue: return (0.04, 0.13, 0.42)
        case .red: return (0.76, 0.04, 0.04)
        case .yellow: return (0.93, 0.74, 0.04)
        case .white: return (0.92, 0.93, 0.94)
        case .purple: return (0.32, 0.12, 0.44)
        }
    }
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
    var rulePreset: String = "IPC-2221 Class 2"
    var minTrackWidth: Double = 0.15
    var minClearance: Double = 0.15
    var minDrill: Double = 0.2
    var minAnnularRing: Double = 0.1
    var minHoleToHole: Double = 0.25
    var copperWeightOz: Double = 1.0
    var maxTempRise: Double = 10.0
    var highAltitude = false
    /// Solder mask colour name (core `BoardSettings::solderMask`).
    var solderMask = "green"
    /// Conformal coating (core `BoardSettings::coating`).
    var coating = "none"
    /// Underfill / corner bonding of heavy parts specified (core `BoardSettings::underfill`).
    var underfill = false
    /// Isolation barrier spacing between galvanic domains, mm (core `BoardSettings::isolationGap`; 0 = none).
    var isolationGap = 0.0
    var conformalCoating: ConformalCoating { ConformalCoating(rawValue: coating) ?? .none }
    var mask: SolderMaskColour { SolderMaskColour(rawValue: solderMask) ?? .green }
    /// Laminate id (core `laminateMaterials()`): "fr4", "rogers-4350b", "megtron-6", …
    var material = "fr4"
    /// Board construction (core `BoardSettings::construction`).
    var construction = "rigid"
    var boardConstruction: BoardConstruction { BoardConstruction(rawValue: construction) ?? .rigid }
    /// Controlled-impedance targets (Ω) the router sizes RF lines and differential pairs to.
    var singleEndedImpedance = 50.0
    var differentialImpedance = 100.0
    /// Back-drill via stubs on fast nets (≥ 4 layers).
    var backdrill = false
    /// Serpentine length / phase matching after Auto Route, with tolerances (mm).
    var lengthTuning = true
    /// HDI vias (blind / buried / laser microvias) and via-in-pad plated over (VIPPO).
    var hdi = false
    var microviaDrill = 0.1
    var microviaDiameter = 0.25
    var viaInPad = false
    var pairSkewTolerance = 0.13
    var busLengthTolerance = 0.5
    /// Net classes: track width (mm) per net name.
    var netWidths: [String: Double] = [:]
    var autoSizeNets = true
    /// Custom outline polygon (empty = width × height rectangle).
    var outline: [BoardPoint] = []
    var holes: [MountingHoleInfo] = []

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
        rulePreset = try c.decodeIfPresent(String.self, forKey: .rulePreset) ?? "IPC-2221 Class 2"
        minTrackWidth = try c.decodeIfPresent(Double.self, forKey: .minTrackWidth) ?? 0.15
        minClearance = try c.decodeIfPresent(Double.self, forKey: .minClearance) ?? 0.15
        minDrill = try c.decodeIfPresent(Double.self, forKey: .minDrill) ?? 0.2
        minAnnularRing = try c.decodeIfPresent(Double.self, forKey: .minAnnularRing) ?? 0.1
        minHoleToHole = try c.decodeIfPresent(Double.self, forKey: .minHoleToHole) ?? 0.25
        copperWeightOz = try c.decodeIfPresent(Double.self, forKey: .copperWeightOz) ?? 1.0
        maxTempRise = try c.decodeIfPresent(Double.self, forKey: .maxTempRise) ?? 10.0
        highAltitude = try c.decodeIfPresent(Bool.self, forKey: .highAltitude) ?? false
        solderMask = try c.decodeIfPresent(String.self, forKey: .solderMask) ?? "green"
        coating = try c.decodeIfPresent(String.self, forKey: .coating) ?? "none"
        underfill = try c.decodeIfPresent(Bool.self, forKey: .underfill) ?? false
        isolationGap = try c.decodeIfPresent(Double.self, forKey: .isolationGap) ?? 0
        material = try c.decodeIfPresent(String.self, forKey: .material) ?? "fr4"
        construction = try c.decodeIfPresent(String.self, forKey: .construction) ?? "rigid"
        singleEndedImpedance = try c.decodeIfPresent(Double.self, forKey: .singleEndedImpedance) ?? 50
        differentialImpedance = try c.decodeIfPresent(Double.self, forKey: .differentialImpedance) ?? 100
        backdrill = try c.decodeIfPresent(Bool.self, forKey: .backdrill) ?? false
        lengthTuning = try c.decodeIfPresent(Bool.self, forKey: .lengthTuning) ?? true
        hdi = try c.decodeIfPresent(Bool.self, forKey: .hdi) ?? false
        microviaDrill = try c.decodeIfPresent(Double.self, forKey: .microviaDrill) ?? 0.1
        microviaDiameter = try c.decodeIfPresent(Double.self, forKey: .microviaDiameter) ?? 0.25
        viaInPad = try c.decodeIfPresent(Bool.self, forKey: .viaInPad) ?? false
        pairSkewTolerance = try c.decodeIfPresent(Double.self, forKey: .pairSkewTolerance) ?? 0.13
        busLengthTolerance = try c.decodeIfPresent(Double.self, forKey: .busLengthTolerance) ?? 0.5
        netWidths = try c.decodeIfPresent([String: Double].self, forKey: .netWidths) ?? [:]
        autoSizeNets = try c.decodeIfPresent(Bool.self, forKey: .autoSizeNets) ?? true
        outline = try c.decodeIfPresent([BoardPoint].self, forKey: .outline) ?? []
        holes = try c.decodeIfPresent([MountingHoleInfo].self, forKey: .holes) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case layerCount, width, height, thickness, trackWidth, clearance, viaDrill, viaDiameter, edgeClearance, routingGrid
        case rulePreset, minTrackWidth, minClearance, minDrill, minAnnularRing, minHoleToHole, copperWeightOz, maxTempRise
        case highAltitude, solderMask, coating, underfill, isolationGap, netWidths, autoSizeNets, outline, holes
        case material, construction, singleEndedImpedance, differentialImpedance, backdrill
        case lengthTuning, pairSkewTolerance, busLengthTolerance, hdi, microviaDrill, microviaDiameter, viaInPad
    }

    var bottomLayer: Int { max(1, layerCount) - 1 }
    /// Stack-ups the core supports: single-sided, then even layer counts up to 24.
    static let layerChoices = [1, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20, 22, 24]
    var hasCustomOutline: Bool { outline.count >= 3 }

    /// Board outline as a path in board millimetres.
    var outlinePath: CGPath {
        let path = CGMutablePath()
        if hasCustomOutline {
            path.addLines(between: outline.map(\.point))
            path.closeSubpath()
        } else {
            path.addRect(CGRect(x: 0, y: 0, width: width, height: height))
        }
        return path
    }

    /// "Top", "Inner 1", …, "Bottom"
    func layerName(_ layer: Int) -> String {
        if layer == 0 { return "Top" }
        if layer == bottomLayer { return "Bottom" }
        return "Inner \(layer)"
    }
}

struct BoardPoint: Codable, Equatable {
    var x: Double
    var y: Double
    var point: CGPoint { CGPoint(x: x, y: y) }
}

/// Non-plated mounting hole with its copper/part keep-out.
struct MountingHoleInfo: Decodable, Equatable {
    var x: Double
    var y: Double
    var drill: Double
    var keepout: Double
    var center: CGPoint { CGPoint(x: x, y: y) }
}

/// Copper pour rule: `net` poured on copper layer `layer`; a plane reserves the layer for that net.
/// Active tamper mesh (PCI PTS): serpentines of netA (horizontal stripes, inner layerA) and netB (vertical stripes,
/// inner layerB) over `component` plus `margin` mm.
struct TamperMeshInfo: Decodable, Equatable, Identifiable, Hashable {
    var component: String
    var netA: String
    var netB: String
    var layerA: Int
    var layerB: Int
    var margin: Double
    var id: String { "\(component)|\(netA)|\(netB)" }
}

struct CopperZoneInfo: Decodable, Equatable, Identifiable, Hashable {
    var net: String
    var layer: Int
    var plane: Bool
    var clearance: Double
    var id: String { "\(net)|\(layer)|\(plane)" }
}

/// Poured copper of one zone as rectangles (flat x0, y0, x1, y1 quadruples from the core).
struct ZoneFillInfo: Decodable, Equatable {
    var zone: Int
    var net: Int
    var layer: Int
    var islands: Int
    var area: Double
    var rects: [Double]

    var cgRects: [CGRect] {
        stride(from: 0, to: rects.count - 3, by: 4).map {
            CGRect(x: rects[$0], y: rects[$0 + 1], width: rects[$0 + 2] - rects[$0], height: rects[$0 + 3] - rects[$0 + 1])
        }
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
    /// Copper layer of an SMD pad (inner layers for embedded passives); -1 for through-hole.
    var layer: Int?
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
    /// Copper layers the barrel spans (HDI): through vias run from 0 to the bottom layer.
    var fromLayer: Int?
    var toLayer: Int?
    /// "through", "blind", "buried" or "microvia".
    var kind: String?

    var isThrough: Bool { (kind ?? "through") == "through" }
    func spans(_ layer: Int) -> Bool {
        guard let from = fromLayer, let to = toLayer else { return true }
        return layer >= from && layer <= to
    }
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
    /// Distinguishes identical findings (same rule, message and place), so list identities stay unique.
    var occurrence = 0
    var id: String { "\(code)|\(message)|\(x)|\(y)|\(occurrence)" }

    private enum CodingKeys: String, CodingKey {
        case severity, code, message, components, hasLocation, x, y
    }
}

extension Array where Element == RuleViolation {
    /// Numbers repeated findings so every element has a distinct `id`.
    func numbered() -> [RuleViolation] {
        var seen: [String: Int] = [:]
        return map { v in
            var v = v
            let key = v.id
            v.occurrence = seen[key, default: 0]
            seen[key] = v.occurrence + 1
            return v
        }
    }
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
    /// Behavioural-model state: 0 regulating (CV), 1 current limit / charging (CC), 2 off, 3 dropout.
    var state: Int?
    var id: Int { component }

    var stateTitle: String? {
        switch state {
        case 0: return "Regulating"
        case 1: return "Current limit"
        case 2: return "Off"
        case 3: return "Dropout"
        default: return nil
        }
    }
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

/// Simulated microcontroller of a component: model, clock and the attached firmware.
struct McuInfo: Decodable, Equatable {
    var model: String
    var clockHz: Double
    var firmwareName: String
    var firmwareBytes: Int
    var firmwareError: String

    var hasFirmware: Bool { firmwareBytes > 0 }
}

/// A microcontroller after a transient run: status and everything its USART transmitted.
struct McuRun: Decodable, Equatable, Identifiable {
    var component: Int
    var ref: String
    var model: String
    var status: String
    var running: Bool
    var serial: String
    var cycles: Double
    var clockHz: Double
    var id: Int { component }
}

/// Built-in example firmware (Intel HEX bundled with the core).
struct FirmwareExample: Decodable, Equatable, Identifiable {
    var id: String
    var name: String
    var model: String
    var description: String
}

/// Snapshot of a live simulation: present values plus the scope trace of the last run interval.
struct LiveState: Decodable, Equatable {
    struct Net: Decodable, Equatable { var index: Int; var name: String; var voltage: Double }
    struct Device: Decodable, Equatable { var component: Int; var ref: String; var current: Double; var power: Double }
    struct Led: Decodable, Equatable { var component: Int; var ref: String; var current: Double; var brightness: Double }
    struct Switch: Decodable, Equatable { var component: Int; var ref: String; var closed: Bool; var momentary: Bool }
    struct Trace: Decodable, Equatable {
        var time: [Double]
        var nets: [WaveformSeries]
    }

    var time: Double
    var nets: [Net]
    var devices: [Device]
    var leds: [Led]
    var switches: [Switch]
    var mcus: [McuRun]
    var trace: Trace

    func voltage(net: Int) -> Double? { nets.first { $0.index == net }?.voltage }
}

struct TransientResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var time: [Double]
    var nets: [WaveformSeries]
    var currents: [WaveformSeries]
    var mcus: [McuRun]

    init(ok: Bool = false, error: String = "", time: [Double] = [], nets: [WaveformSeries] = [],
         currents: [WaveformSeries] = [], mcus: [McuRun] = []) {
        self.ok = ok
        self.error = error
        self.time = time
        self.nets = nets
        self.currents = currents
        self.mcus = mcus
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        time = try c.decodeIfPresent([Double].self, forKey: .time) ?? []
        nets = try c.decodeIfPresent([WaveformSeries].self, forKey: .nets) ?? []
        currents = try c.decodeIfPresent([WaveformSeries].self, forKey: .currents) ?? []
        mcus = try c.decodeIfPresent([McuRun].self, forKey: .mcus) ?? []
    }

    private enum CodingKeys: String, CodingKey { case ok, error, time, nets, currents, mcus }
}

struct RouteStats: Decodable, Equatable {
    var connections: Int = 0
    var routed: Int = 0
    var failed: Int = 0
    var vias: Int = 0
    var trackLength: Double = 0
    /// Nets lengthened with serpentines (length / phase matching).
    var lengthTuned: Int?
    var failedNets: [String] = []
}

/// The seven robot design segments (`sieda_robot_segments_json`) or the six automotive ECU segments
/// (`sieda_ecu_segments_json`) or the five aerospace segments (`sieda_aerospace_segments_json`) checked on the
/// design; `platforms` lists the robot platforms / ECU types / missions.
struct RobotSegmentsReport: Decodable, Equatable {
    var platform = ""
    var applies = false
    var platforms: [RobotPlatformInfo] = []
    var segments: [RobotSegmentInfo] = []

    static let empty = RobotSegmentsReport()
}

struct RobotPlatformInfo: Decodable, Equatable, Identifiable {
    var id: String
    var name: String
    var description: String
    var guidance: [String]
}

struct RobotSegmentInfo: Decodable, Equatable, Identifiable {
    var id: String
    var name: String
    var status: String  // "complete", "partial", "missing"
    var items: [RobotCheckItemInfo]
    var guidance: [String]
}

struct RobotCheckItemInfo: Decodable, Equatable, Identifiable {
    var label: String
    var ok: Bool
    var detail: String
    var id: String { label }
}

/// Matched-length groups (differential pairs, buses) and their routed lengths (`sieda_length_report_json`).
struct LengthReport: Decodable, Equatable {
    var enabled = true
    var pairSkewTolerance = 0.13
    var busLengthTolerance = 0.5
    var groups: [LengthGroupInfo] = []

    static let empty = LengthReport()
}

struct LengthGroupInfo: Decodable, Equatable, Identifiable {
    var name: String
    var kind: String
    var tolerance: Double
    var target: Double
    var matched: Bool
    var nets: [LengthNetInfo]

    var id: String { kind + ":" + name }
    var isPair: Bool { kind == "pair" }
}

struct LengthNetInfo: Decodable, Equatable, Identifiable {
    var name: String
    var length: Double
    var delta: Double
    var routed: Bool
    var ok: Bool

    var id: String { name }
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
