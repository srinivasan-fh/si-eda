import CoreGraphics
import Foundation

// Codable mirrors of the JSON produced by the C++ core (`Project::snapshot`, analysis results).

struct DesignSnapshot: Decodable, Equatable {
    var name: String
    var requirements: String
    var components: [SnapComponent] {
        didSet { componentIndex = Self.index(of: components) }
    }
    /// Component id → its position in `components`, so `component(_:)` is O(1): the canvases look parts up for every
    /// wire end and courtyard of every frame, which made drawing O(n²) on designs with 1000+ parts.
    private(set) var componentIndex: [Int: Int] = [:]
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
    /// Robot platform id ("rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"); empty when the design is not a robot.
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
    /// Home appliance type ("laundry", "kitchen", "refrigeration", "hvac", "small"); empty when not set.
    var applianceType = ""
    /// Memory design type ("sdram", "ddr", "lpddr", "dimm", "rdimm"); empty when not set.
    var memoryDesign = ""
    var zones: [CopperZoneInfo] = []
    /// Active tamper meshes laid over secure elements by the autorouter.
    var tamperMeshes: [TamperMeshInfo] = []
    var zoneFills: [ZoneFillInfo] = []
    /// Schematic sheets in tab order (one for a single-sheet design) and the sheet new parts are placed on.
    var sheets: [SheetInfo] = []
    var activeSheet = 1
    /// Assembly variants and the active one ("" = the base design).
    var variants: [VariantInfo] = []
    var activeVariant = ""
    /// Graphical buses on every sheet (filtered to one sheet by `onSheet`).
    var buses: [BusInfo] = []
    /// Title block printed on every schematic sheet.
    var titleBlock = TitleBlockInfo()
    /// Signal harness types (named bundles of signals).
    var harnessTypes: [HarnessTypeInfo] = []
    /// Schematic directives: net classes and the directives on nets (the source of the board's net rules).
    var netClassDefs: [NetClassDefInfo] = []
    var directives: [DirectiveInfo] = []
    /// ERC error reporting: rule code → "error", "warning", "info" or "off" (rules reported at another severity).
    var ercSeverities: [String: String] = [:]

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
        componentIndex = Self.index(of: components)
    }

    /// `JSONDecoder.userInfo` key of the snapshot a delta (`"delta": true`) is merged into: absent sections keep its value.
    static let baseKey = CodingUserInfoKey(rawValue: "sieda.snapshotBase")!

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        let isDelta = try c.decodeIfPresent(Bool.self, forKey: .delta) ?? false
        let base = isDelta ? decoder.userInfo[Self.baseKey] as? DesignSnapshot : nil
        /// A section from the reply, else the base's (a delta), else `fallback`; nil fallback = required.
        func field<T: Decodable>(_ key: CodingKeys, _ old: KeyPath<DesignSnapshot, T>, _ fallback: T?) throws -> T {
            if let value = try c.decodeIfPresent(T.self, forKey: key) { return value }
            if let base { return base[keyPath: old] }
            if let fallback { return fallback }
            return try c.decode(T.self, forKey: key)  // throws: a required section is missing
        }
        name = try field(.name, \.name, nil)
        requirements = try field(.requirements, \.requirements, "")
        components = try field(.components, \.components, nil)
        wires = try field(.wires, \.wires, nil)
        nets = try field(.nets, \.nets, nil)
        board = try field(.board, \.board, nil)
        pads = try field(.pads, \.pads, nil)
        tracks = try field(.tracks, \.tracks, nil)
        vias = try field(.vias, \.vias, nil)
        ratsnest = try field(.ratsnest, \.ratsnest, nil)
        courtyards = try field(.courtyards, \.courtyards, nil)
        bodies = try field(.bodies, \.bodies, [])
        customParts = try field(.customParts, \.customParts, [])
        industry = try field(.industry, \.industry, "general")
        robotPlatform = try field(.robotPlatform, \.robotPlatform, "")
        ecuType = try field(.ecuType, \.ecuType, "")
        aerospaceMission = try field(.aerospaceMission, \.aerospaceMission, "")
        navalPlatform = try field(.navalPlatform, \.navalPlatform, "")
        medicalClass = try field(.medicalClass, \.medicalClass, "")
        retailDevice = try field(.retailDevice, \.retailDevice, "")
        applianceType = try field(.applianceType, \.applianceType, "")
        memoryDesign = try field(.memoryDesign, \.memoryDesign, "")
        tamperMeshes = try field(.tamperMeshes, \.tamperMeshes, [])
        zones = try field(.zones, \.zones, [])
        zoneFills = try field(.zoneFills, \.zoneFills, [])
        sheets = try field(.sheets, \.sheets, [])
        activeSheet = try field(.activeSheet, \.activeSheet, 1)
        variants = try field(.variants, \.variants, [])
        activeVariant = try field(.activeVariant, \.activeVariant, "")
        buses = try field(.buses, \.buses, [])
        titleBlock = try field(.titleBlock, \.titleBlock, TitleBlockInfo())
        harnessTypes = try field(.harnessTypes, \.harnessTypes, [])
        netClassDefs = try field(.netClassDefs, \.netClassDefs, [])
        directives = try field(.directives, \.directives, [])
        ercSeverities = try field(.ercSeverities, \.ercSeverities, [:])
        // Unchanged parts: keep the base's index instead of rebuilding it.
        componentIndex = base != nil && !c.contains(.components) ? base!.componentIndex : Self.index(of: components)
    }

    private static func index(of components: [SnapComponent]) -> [Int: Int] {
        var index = [Int: Int](minimumCapacity: components.count)
        for (i, c) in components.enumerated() where index[c.id] == nil { index[c.id] = i }
        return index
    }

    private enum CodingKeys: String, CodingKey {
        case name, requirements, components, wires, nets, board, pads, tracks, vias, ratsnest, courtyards, bodies, customParts
        case industry, robotPlatform, ecuType, aerospaceMission, navalPlatform, medicalClass, retailDevice, zones, zoneFills
        case tamperMeshes, applianceType, memoryDesign, sheets, activeSheet, variants, activeVariant, buses, titleBlock
        case harnessTypes, netClassDefs, directives, ercSeverities, delta
    }

    func component(_ id: Int) -> SnapComponent? {
        guard let i = componentIndex[id], i < components.count, components[i].id == id else {
            return componentIndex.isEmpty && !components.isEmpty ? components.first { $0.id == id } : nil
        }
        return components[i]
    }
    func sheet(_ id: Int) -> SheetInfo? { sheets.first { $0.id == id } }
    func bus(_ id: Int) -> BusInfo? { buses.first { $0.id == id } }

    /// The part of the design drawn on one sheet: its components and the wires between them (wires never cross
    /// sheets). A single-sheet design is returned unchanged.
    func onSheet(_ sheet: Int) -> DesignSnapshot {
        guard sheets.count > 1 || components.contains(where: { $0.isUnitPackage }) else { return self }
        var copy = self
        copy.buses = buses.filter { $0.sheet == sheet }
        copy.components = components.filter { $0.sheetId == sheet && !$0.isUnitPackage }
        let ids = Set(copy.components.map(\.id))
        copy.wires = wires.filter { ids.contains($0.a.component) && ids.contains($0.b.component) }
        return copy
    }
    func customPart(_ id: String?) -> CustomPartInfo? {
        guard let id else { return nil }
        return customParts.first { $0.id == id }
    }
    func customPart(for component: SnapComponent) -> CustomPartInfo? {
        guard component.componentKind == .custom, let part = customPart(component.customPart) else { return nil }
        if let unit = component.unit { return part.forUnit(unit) }  // a placed unit draws its own symbol
        return part
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

/// A package a part can be fitted in ("R_0603" shown as "0603").
struct PackageOption: Decodable, Equatable, Identifiable, Hashable {
    var id: String
    var label: String
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
    var spice: SpiceAttachment?
    /// Package variant the part is fitted in (nil = the kind's default footprint).
    var package: String?
    /// Packages a passive / diode can be switched to (chip sizes, through-hole, tantalum…); nil for fixed parts.
    var packageOptions: [PackageOption]?
    /// Sheet the component is drawn on (nil from an older core: the first sheet).
    var sheet: Int?
    /// Net labels: "global", "local", "port" or "entry" (sheet entry into `targetSheet`).
    var scope: String?
    var targetSheet: Int?
    /// false when the part is not fitted in the active variant (or marked DNP); nil = fitted.
    var fitted: Bool?
    /// Value fitted in the active variant, when it differs from the design value.
    var variantValue: String?
    /// Part of a repeated sheet: the block part an instance copies, and the designator inside the block ("R1").
    var instanceOf: Int?
    var logicalRef: String?
    /// Per-channel parameters of a repeated sheet's part: what this channel sets itself (1 value, 2 package) and the
    /// value the block gives the other channels; nil when the channel takes the block's.
    var channelOverride: Int?
    var blockValue: String?
    /// Bus entries: the bus the label leaves (BusInfo.id).
    var bus: Int?
    /// Signal harnesses: a harness label's type (a bundle named by its value), and for a harness entry the harness
    /// label it belongs to (its member net is "<harness>.<entry>").
    var harnessType: String?
    var harnessOf: Int?
    var isHarnessLabel: Bool { componentKind == .netLabel && harnessType != nil && harnessOf == nil }
    /// Multi-unit parts: a placed unit (its 1-based index, name "A"… and package), or the package itself
    /// (`unitPackage`: not drawn on the schematic; `units` lists the placed units).
    var unit: Int?
    var unitName: String?
    var unitOf: Int?
    var unitPackage: Bool?
    var units: [Int]?

    var isUnitPackage: Bool { unitPackage ?? false }
    /// "U1A" for a unit, the designator otherwise.
    var displayRef: String { ref + (unitName ?? "") }

    var sheetId: Int { sheet ?? 1 }
    var labelScope: String { scope ?? "global" }
    var isFitted: Bool { fitted ?? true }

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

/// A schematic sheet (page): components belong to one sheet; nets cross sheets through global labels, ground and
/// hierarchical ports joined to sheet entries.
struct SheetInfo: Decodable, Equatable, Identifiable, Hashable {
    var id: Int
    var name: String
    /// The sheet whose sheet symbol stands for this one; 0 = top level.
    var parent: Int
    var depth: Int
    /// Hierarchical port names on the sheet (the entries its sheet symbol offers).
    var ports: [String]
    /// Repeated sheet (nil for an ordinary one): the definition an instance copies (0 on the definition), the channel
    /// label, how channel designators are made ("sheet" or "suffix") and the number of channels.
    var instanceOf: Int?
    var channel: String?
    var refs: String?
    var instances: Int?
    /// Repeat count of the block under one parent (nested blocks: per outer channel) and the channel path ("B/A").
    var channels: Int?
    var path: String?
    /// Drawing template ("A4" … "ANSI E"; "" = sized to the drawing) and the one it prints on.
    var size: String?
    var template: String?
    /// Drawn size of the sheet's sheet symbol on its parent (schematic units; 0 = fitted to its entries).
    var symbolWidth: Double?
    var symbolHeight: Double?
    /// Helper sheet of a block: every channel of its parent gets a copy of it.
    var helper: Bool?
    /// Fixed template frame: its top-left corner (schematic units); nil = centred on the drawing.
    var frameX: Double?
    var frameY: Double?

    var isRepeated: Bool { (instances ?? 0) > 1 }
    var isInstance: Bool { (instanceOf ?? 0) != 0 }
    /// The definition sheet of a repeated block (the sheet itself otherwise).
    var definitionId: Int { isInstance ? (instanceOf ?? id) : id }
}

/// A signal harness type: a named bundle of signals (USB = DP, DN, VBUS, GND).
struct HarnessTypeInfo: Decodable, Equatable, Identifiable, Hashable {
    var name: String
    var entries: [String]
    var id: String { name }
}

/// A change the board proposes to the schematic (back-annotation ECO).
struct EcoChangeInfo: Codable, Equatable, Identifiable {
    var kind: String  // "rename", "pinSwap", "gateSwap", "invalid"
    var component: Int
    var from: String
    var to: String
    var pinA: Int
    var pinB: Int
    var other: Int
    var applicable: Bool
    var note: String
    var id: String { "\(kind)|\(component)|\(from)|\(to)|\(other)" }
}

/// A drawing sheet size (mm, landscape).
struct SheetTemplateInfo: Decodable, Equatable, Identifiable, Hashable {
    var name: String
    var width: Double
    var height: Double
    var id: String { name }
}

/// A net class defined on the schematic: track width and clearance (mm; nil = the board default).
struct NetClassDefInfo: Decodable, Equatable, Identifiable, Hashable {
    var name: String
    var trackWidth: Double?
    var clearance: Double?
    var id: String { name }
}

/// A directive on a net, anchored on a component pin: a net class, a differential-pair marker and / or a parameter
/// set (its own width and clearance).
struct DirectiveInfo: Decodable, Equatable, Identifiable {
    var id: Int
    var component: Int
    var pin: Int
    var netClass: String?
    var diffPair: Bool?
    var trackWidth: Double?
    var clearance: Double?
    var net: Int?
    var netName: String?

    var isDiffPair: Bool { diffPair ?? false }
    /// "HS ◆ diff 0.2 mm" — what the canvas shows beside the anchor.
    var summary: String {
        var parts: [String] = []
        if let c = netClass, !c.isEmpty { parts.append(c) }
        if isDiffPair { parts.append("⇄") }
        if let w = trackWidth, w > 0 { parts.append(String(format: "w%.2f", w)) }
        if let c = clearance, c > 0 { parts.append(String(format: "c%.2f", c)) }
        return parts.joined(separator: " ")
    }
}

/// Schematic title block fields (the title defaults to the project name).
struct TitleBlockInfo: Codable, Equatable {
    var title = ""
    var company = ""
    var revision = ""
    var date = ""
    var drawnBy = ""
}

/// A find result (`sieda_schematic_find`): a designator, value, label, pin or net that matches.
struct SchematicSearchHit: Decodable, Equatable, Identifiable {
    var component: Int
    var net: Int
    var pin: Int
    var sheet: Int
    var field: String  // "ref", "value", "label", "net", "pin"
    var text: String
    var id: String { "\(field)|\(component)|\(net)|\(pin)|\(text)" }
}

/// Net navigator (`sieda_net_places`): every place a net appears, sheet by sheet.
struct NetPlacesReport: Decodable, Equatable {
    struct Place: Decodable, Equatable, Identifiable {
        var component: Int
        var pin: Int
        var sheet: Int
        var x: Double
        var y: Double
        var kind: String  // "pin", "label", "global", "port", "entry", "bus", "ground"
        var ref: String
        var name: String
        var id: String { "\(component).\(pin)" }
    }
    var net: Int
    var name: String
    var places: [Place]
}

/// A graphical bus: a named polyline ("D[0..7]") whose members leave it through bus entries (net labels with `bus`).
struct BusInfo: Decodable, Equatable, Identifiable {
    var id: Int
    var sheet: Int
    var name: String
    var points: [BoardPoint]
    var members: [String] = []
    var instanceOf: Int?

    var path: [CGPoint] { points.map(\.point) }

    /// The point of the bus nearest to `p` and its distance.
    func nearest(to p: CGPoint) -> (point: CGPoint, distance: CGFloat) {
        var best = (point: path.first ?? p, distance: CGFloat.greatestFiniteMagnitude)
        let pts = path
        for i in pts.indices.dropLast() {
            let q = WireGeometry.nearestPoint(onSegment: pts[i], pts[i + 1], to: p)
            let d = hypot(q.x - p.x, q.y - p.y)
            if d < best.distance { best = (q, d) }
        }
        return best
    }
}

/// A named assembly variant: parts fitted / not fitted and value overrides.
struct VariantInfo: Decodable, Equatable, Identifiable {
    struct Part: Decodable, Equatable {
        var component: Int
        var ref: String
        var fitted: Bool?
        var value: String?
    }
    var name: String
    var description: String
    var parts: [Part]
    var id: String { name }

    func part(_ component: Int) -> Part? { parts.first { $0.component == component } }
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
    /// Production panel (core `PanelSettings`); nil = a single board.
    var panel: PanelInfo?
    /// Manufacturer DFM / DFA rule pack id (core `Dfm.hpp`); "" = none.
    var dfmPack = ""

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
        panel = try c.decodeIfPresent(PanelInfo.self, forKey: .panel)
        dfmPack = try c.decodeIfPresent(String.self, forKey: .dfmPack) ?? ""
    }

    private enum CodingKeys: String, CodingKey {
        case layerCount, width, height, thickness, trackWidth, clearance, viaDrill, viaDiameter, edgeClearance, routingGrid
        case rulePreset, minTrackWidth, minClearance, minDrill, minAnnularRing, minHoleToHole, copperWeightOz, maxTempRise
        case highAltitude, solderMask, coating, underfill, isolationGap, netWidths, autoSizeNets, outline, holes
        case material, construction, singleEndedImpedance, differentialImpedance, backdrill
        case lengthTuning, pairSkewTolerance, busLengthTolerance, hdi, microviaDrill, microviaDiameter, viaInPad, panel, dfmPack
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
    /// True arc a → (mx, my) → b; the core adds its centre, radius and angles (radians, sweep > 0 turns from +x
    /// towards +y). Absent on straight tracks (see TrackArcs.swift for drawing and hit testing).
    var arc: Bool?
    var mx: Double?
    var my: Double?
    var cx: Double?
    var cy: Double?
    var radius: Double?
    var startAngle: Double?
    var sweep: Double?
    /// Part of a teardrop (from inside a pad / via onto the track it widens).
    var teardrop: Bool?
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

/// Interactive routing state from the core (`sieda_router_*`): the route's copper, and the other nets' copper at its
/// shoved position (`hiddenTracks` / `hiddenVias` are the board items those replace while the route is previewed).
struct RoutePreview: Decodable, Equatable {
    var active: Bool
    var kind: String
    var status: String
    var blocked: Bool
    var reachedTarget: Bool
    var nets: [Int]
    var layer: Int
    var width: Double
    var gap: Double
    var endX: Double
    var endY: Double
    var length: Double
    var placed: [SnapTrack]
    var head: [SnapTrack]
    var vias: [SnapVia]
    var shovedTracks: [SnapTrack]
    var shovedVias: [SnapVia]
    var hiddenTracks: [Int]
    var hiddenVias: [Int]
    /// Why the last router call was refused (the preview is still the current state).
    var error: String?
    /// The routed net's whole length with this route, and the length its matched-length group asks for (0 = none).
    var netLength: Double?
    var targetLength: Double?
    /// Highlight mode: what the route violates (nil / empty otherwise).
    var collisions: [RouteCollision]?
    /// The update was cancelled (`routerAbort`): the preview from before it, to be ignored.
    var aborted: Bool?
    /// Tune while routing: each routed member's length so far against its target (nil when off).
    var memberLengths: [RouteMemberLength]?
    /// Tune while routing: the route with its live meanders (and a pair's skew bumps), drawn in place of
    /// `placed` + `head` (nil when nothing is meandered).
    var tunedTracks: [SnapTrack]?
    /// Tune while routing: the live summary (members within tolerance, a pair's skew).
    var tuneStatus: String?

    /// The route's copper to draw: the live meanders when there are any, else the placed tracks and the head.
    var routeCopper: [SnapTrack] {
        if let tuned = tunedTracks, !tuned.isEmpty { return tuned }
        return placed + head
    }
}

/// One member of a routed bus (or the routed net) against its length target (tune while routing).
struct RouteMemberLength: Decodable, Equatable {
    var net: Int
    var length: Double
    var target: Double
    var tolerance: Double
    var withinTolerance: Bool
}

/// Interactive router mode (`sieda_router_*` options "mode").
enum RouterModeChoice: String, CaseIterable, Identifiable {
    case shove, walkaround, highlight, stop  // stop: the head stops at the first obstacle
    var id: String { rawValue }
}

/// The via V places while routing (`sieda_router_*` options "viaType"): through, blind / buried (the span it
/// joins), laser microvia (neighbouring layers) or automatic (microvia / blind / through by span on HDI boards).
enum RouterViaChoice: String, CaseIterable, Identifiable {
    case through, blind, micro, auto
    var id: String { rawValue }
}

/// Copper or a board rule the route violates in Highlight mode: `kind` "track" (a-b, width), "via" (a, width =
/// diameter), "pad" (a = centre, w × h), "hole" (a, width = keep-out), or "edge" / "plane" / "mesh" (x, y only).
struct RouteCollision: Decodable, Equatable {
    var kind: String
    var id: Int
    var x: Double
    var y: Double
    var ax: Double
    var ay: Double
    var bx: Double
    var by: Double
    var w: Double
    var h: Double
    var width: Double
}

/// Fanout of a part (`sieda_pcb_fanout`): pads given an escape and a via, pads skipped, pads without room.
struct FanoutResult: Decodable, Equatable {
    var ok: Bool
    var message: String
    var fanned: Int
    var skipped: Int
    var failed: [Int]
}

/// Interactive length tuning (`sieda_router_tune`): the meanders it would add (or added) and the lengths.
struct TunePreview: Decodable, Equatable {
    var ok: Bool
    var message: String
    var net: Int
    /// Matched-length group of the net ("" when none) and its kind ("pair" / "bus").
    var group: String
    var groupKind: String
    var tolerance: Double
    var before: Double
    var after: Double
    var target: Double
    var applied: Bool
    var addedTracks: [SnapTrack]
    var removedTracks: [Int]
    /// Where the target came from ("typed", "partner", "rule:<net>", "group:<name>"); nil for the plain accordion.
    var targetSource: String?
    /// Nets measured (the xSignal through series parts), a coupled pair tuning and the pair partner.
    var xsignalNets: [Int]?
    var coupled: Bool?
    var partnerNet: Int?

    /// Within tolerance of the target.
    var onTarget: Bool { abs(after - target) <= max(tolerance, 0.01) + 1e-6 }
}

/// Result of committing a route (`sieda_router_commit`).
struct RouteCommitResult: Decodable, Equatable {
    var ok: Bool
    var error: String?
    var addedTracks: [Int]
    var addedVias: [Int]
    /// Tune while routing: the members' lengths after the meanders, and which could not reach the target.
    var memberLengths: [RouteMemberLength]?
    var tuneStatus: String?
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
    /// Schematic sheet of the location (ERC), nil when not a schematic location.
    var sheet: Int? = nil
    /// Distinguishes identical findings (same rule, message and place), so list identities stay unique.
    var occurrence = 0
    var id: String { "\(code)|\(message)|\(x)|\(y)|\(occurrence)" }

    private enum CodingKeys: String, CodingKey {
        case severity, code, message, components, hasLocation, x, y, sheet
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
    /// The assembly simulated: the active variant ("" = base design) and the parts left out as not fitted.
    var variant = ""
    var omitted: [String] = []

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
        variant = try c.decodeIfPresent(String.self, forKey: .variant) ?? ""
        omitted = try c.decodeIfPresent([String].self, forKey: .omitted) ?? []
    }

    private enum CodingKeys: String, CodingKey { case converged, error, iterations, nets, devices, variant, omitted }

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

/// Bode readouts of one node (`sieda_simulate_ac`); nil where the sweep does not reach the point.
struct ACMetrics: Decodable, Equatable {
    var lowFreqDb: Double?
    var peakDb: Double?
    var peakHz: Double?
    var f3dbHz: Double?
    var bwLowHz: Double?
    var bwHighHz: Double?
    var unityHz: Double?
    var phaseMarginDeg: Double?
}

struct ACNetResponse: Decodable, Equatable, Identifiable {
    var index: Int
    var name: String
    var dc: Double?
    var magnitudeDb: [Double]
    var phaseDeg: [Double]
    var metrics: ACMetrics?
    var id: Int { index }
}

/// AC small-signal sweep: magnitude / phase per node over a logarithmic frequency axis.
struct ACResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var stimulus: [String]
    var frequency: [Double]
    var nets: [ACNetResponse]

    init(ok: Bool = false, error: String = "", stimulus: [String] = [], frequency: [Double] = [], nets: [ACNetResponse] = []) {
        self.ok = ok
        self.error = error
        self.stimulus = stimulus
        self.frequency = frequency
        self.nets = nets
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        stimulus = try c.decodeIfPresent([String].self, forKey: .stimulus) ?? []
        frequency = try c.decodeIfPresent([Double].self, forKey: .frequency) ?? []
        nets = try c.decodeIfPresent([ACNetResponse].self, forKey: .nets) ?? []
    }

    private enum CodingKeys: String, CodingKey { case ok, error, stimulus, frequency, nets }
}

/// DC sweep of a source: node voltages and device currents per swept value.
struct DCSweepResult: Decodable, Equatable {
    var ok: Bool
    var error: String
    var source: String
    var unit: String
    var values: [Double]
    var nets: [WaveformSeries]
    var currents: [WaveformSeries]

    init(ok: Bool = false, error: String = "", source: String = "", unit: String = "V", values: [Double] = [],
         nets: [WaveformSeries] = [], currents: [WaveformSeries] = []) {
        self.ok = ok
        self.error = error
        self.source = source
        self.unit = unit
        self.values = values
        self.nets = nets
        self.currents = currents
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        source = try c.decodeIfPresent(String.self, forKey: .source) ?? ""
        unit = try c.decodeIfPresent(String.self, forKey: .unit) ?? "V"
        values = try c.decodeIfPresent([Double].self, forKey: .values) ?? []
        nets = try c.decodeIfPresent([WaveformSeries].self, forKey: .nets) ?? []
        currents = try c.decodeIfPresent([WaveformSeries].self, forKey: .currents) ?? []
    }

    private enum CodingKeys: String, CodingKey { case ok, error, source, unit, values, nets, currents }
}

/// Monte Carlo and worst-case tolerance analysis of one measured quantity (`sieda_simulate_monte_carlo`).
struct MonteCarloResult: Decodable, Equatable {
    struct Histogram: Decodable, Equatable {
        var edges: [Double] = []
        var counts: [Int] = []
    }
    struct WorstCase: Decodable, Equatable {
        var min: Double
        var max: Double
    }
    struct Part: Decodable, Equatable, Identifiable {
        var ref: String
        var value: String
        var tolerance: Double
        var fromValue: Bool
        var sensitivity: Double?
        var id: String { ref }
    }

    var ok = false
    var error = ""
    var net = ""
    var unit = "V"
    var nominal = 0.0
    var runs = 0
    var failedRuns = 0
    var min = 0.0
    var max = 0.0
    var mean = 0.0
    var sigma = 0.0
    var histogram = Histogram()
    var worstCase: WorstCase?
    var parts: [Part] = []

    init(error: String) { self.error = error }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        net = try c.decodeIfPresent(String.self, forKey: .net) ?? ""
        unit = try c.decodeIfPresent(String.self, forKey: .unit) ?? "V"
        nominal = try c.decodeIfPresent(Double.self, forKey: .nominal) ?? 0
        runs = try c.decodeIfPresent(Int.self, forKey: .runs) ?? 0
        failedRuns = try c.decodeIfPresent(Int.self, forKey: .failedRuns) ?? 0
        min = try c.decodeIfPresent(Double.self, forKey: .min) ?? 0
        max = try c.decodeIfPresent(Double.self, forKey: .max) ?? 0
        mean = try c.decodeIfPresent(Double.self, forKey: .mean) ?? 0
        sigma = try c.decodeIfPresent(Double.self, forKey: .sigma) ?? 0
        histogram = try c.decodeIfPresent(Histogram.self, forKey: .histogram) ?? Histogram()
        worstCase = try c.decodeIfPresent(WorstCase.self, forKey: .worstCase)
        parts = try c.decodeIfPresent([Part].self, forKey: .parts) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case ok, error, net, unit, nominal, runs, failedRuns, min, max, mean, sigma, histogram, worstCase, parts
    }
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
    /// The user stopped the route (`sieda_pcb_autoroute_progress`): the board was left as it was.
    var cancelled: Bool?
    /// Pin / gate swaps made before routing (only when the strategy swaps).
    var pinSwaps: Int?
    var gateSwaps: Int?
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
    /// Robot platforms: the production parts kit, part numbers per subsystem (all in the standard library).
    var kit: [RobotKitGroupInfo]?
}

struct RobotKitGroupInfo: Decodable, Equatable, Identifiable, Hashable {
    var subsystem: String
    var parts: [String]
    var id: String { subsystem }
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
    /// One `MeshSurface` raw value per vertex (empty when the core did not tag them).
    var surfaces: [UInt8] = []
    var vertexCount: Int { positions.count / 3 }
}

/// What a mesh triangle is made of (mirrors the core's `Surface`); the 3D view gives each its own physical material.
enum MeshSurface: UInt8, CaseIterable {
    case mask = 0, laminate, finish, gold, tin, solder, silk, plastic, ceramic, glass, hole, marking
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

/// Production panel settings (`sieda_pcb_panel` / `sieda_pcb_set_panel`): nx × ny boards, routed gap (mm, tab panels),
/// rail width (mm), V-score instead of tabs and mouse bites.
struct PanelInfo: Codable, Equatable {
    var nx = 1, ny = 1
    var gap = 2.0, rail = 5.0
    var vscore = false
}

/// Where the panel puts everything (mm, Y up), for the Board Setup preview.
struct PanelLayoutInfo: Decodable, Equatable {
    var settings: PanelInfo
    var width, height, boardWidth, boardHeight: Double
    var boards, fiducials, toolingHoles, mouseBites: [[Double]]
    var tabs, vscores: [[[Double]]]
}

/// A manufacturer DFM / DFA rule pack (`sieda_dfm_packs_json`).
/// Field-solver result for one track (and, with a gap, the pair) on a stack-up layer.
struct FieldSolveInfo: Decodable, Equatable {
    struct Loss: Decodable, Equatable { var ghz, totalDbPerIn: Double }
    var z0, eeff, delayPsPerMm: Double
    var zdiff: Double?
    var loss: [Loss]?

    /// Total loss at `ghz` (dB per inch), when the solver reported that frequency.
    func dbPerInch(at ghz: Double) -> Double? { loss?.first { abs($0.ghz - ghz) < 1e-9 }?.totalDbPerIn }
}

/// Sign-off of the board against its manufacturer pack (`sieda_dfm_report_json`).
struct DfmReport: Decodable, Equatable {
    struct Row: Decodable, Equatable, Identifiable {
        var rule, actual, limit: String
        var ok: Bool
        var id: String { rule }
    }
    var pack: String
    var pass: Bool?
    var rows: [Row]?
}

struct DfmPackInfo: Decodable, Identifiable, Equatable {
    var id: String
    var name: String
    var maker: String
    var notes: String
    var minTrack, minSpace, minDrill, minAnnularRing: Double
    var maxLayers: Int
}
