import CoreGraphics
import Foundation

/// Structured circuit description exchanged with AI agents. Agents never touch the engine directly:
/// they emit a `DesignPlan`, which SiEDA validates and compiles into the schematic.
struct DesignPlan: Codable, Equatable {
    var title: String
    var summary: String
    var components: [PlannedComponent]
    var connections: [PlannedConnection]
    var notes: [String]
    var board: PlannedBoard
    /// Industry profile id ("general", "automotive", "space", …); nil keeps the project's current profile.
    var industry: String?
    /// Copper pours / planes (replace the project's pours when the plan is applied).
    var pours: [PlannedPour]
    /// Net classes: wider tracks for power and motor nets.
    var netClasses: [PlannedNetClass]
    /// Pins left open on purpose ("U3.9"): marked no-connect so ERC does not report them.
    var noConnect: [String]
    /// Robot platform ("rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"): turns on the 7-segment robotics checks.
    var robotPlatform: String?
    /// Automotive ECU type ("bcm", "powertrain", "adas", "ev", "chassis", "gateway"): turns on the 6-segment ECU checks.
    var ecuType: String?
    /// Aerospace mission ("leo", "geo", "launcher", "military", "commercial"): turns on the 5-segment aerospace checks.
    var aerospaceMission: String?
    /// Naval platform ("combatant", "carrier", "submarine", "patrol", "commercial"): turns on the 5-segment naval checks.
    var navalPlatform: String?
    /// Medical device class ("bf", "cf", "life", "implant", "home"): turns on the 4-segment medical checks.
    var medicalClass: String?
    /// Retail device class ("countertop", "unattended", "mpos", "kiosk", "printer"): turns on the 4-segment POS checks.
    var retailDevice: String?
    /// Active tamper meshes over secure elements (laid by the autorouter on two inner layers).
    var tamperMeshes: [PlannedTamperMesh]
    /// Home appliance type ("laundry", "kitchen", "refrigeration", "hvac", "small"): turns on the 4-segment checks.
    var applianceType: String?
    /// Memory design type ("sdram", "ddr", "lpddr", "dimm", "rdimm"): turns on the 5-segment memory checks.
    var memoryDesign: String?
    /// Schematic sheets of a multi-sheet design, in order (empty: everything on one sheet).
    var sheets: [PlannedSheet]
    /// Graphical buses (nil: none); bus entries are net labels naming their bus by index (`PlannedComponent.bus`).
    var buses: [PlannedBus]?
    /// Signal harness types, schematic net classes and the directives on nets (nil: none).
    var harnessTypes: [PlannedHarnessType]?
    var netClassDefs: [PlannedNetClassDef]?
    var directives: [PlannedDirective]?

    init(title: String, summary: String, components: [PlannedComponent], connections: [PlannedConnection],
         notes: [String] = [], board: PlannedBoard = PlannedBoard(), industry: String? = nil,
         pours: [PlannedPour] = [], netClasses: [PlannedNetClass] = [], noConnect: [String] = [],
         robotPlatform: String? = nil, ecuType: String? = nil, aerospaceMission: String? = nil,
         navalPlatform: String? = nil, medicalClass: String? = nil, retailDevice: String? = nil,
         tamperMeshes: [PlannedTamperMesh] = [], applianceType: String? = nil, memoryDesign: String? = nil,
         sheets: [PlannedSheet] = []) {
        self.title = title
        self.summary = summary
        self.components = components
        self.connections = connections
        self.notes = notes
        self.board = board
        self.industry = industry
        self.pours = pours
        self.netClasses = netClasses
        self.noConnect = noConnect
        self.robotPlatform = robotPlatform
        self.ecuType = ecuType
        self.aerospaceMission = aerospaceMission
        self.navalPlatform = navalPlatform
        self.medicalClass = medicalClass
        self.retailDevice = retailDevice
        self.tamperMeshes = tamperMeshes
        self.applianceType = applianceType
        self.memoryDesign = memoryDesign
        self.sheets = sheets
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? "Untitled design"
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        components = try c.decodeIfPresent([PlannedComponent].self, forKey: .components) ?? []
        connections = try c.decodeIfPresent([PlannedConnection].self, forKey: .connections) ?? []
        notes = try c.decodeIfPresent([String].self, forKey: .notes) ?? []
        board = try c.decodeIfPresent(PlannedBoard.self, forKey: .board) ?? PlannedBoard()
        industry = try c.decodeIfPresent(String.self, forKey: .industry)
        pours = try c.decodeIfPresent([PlannedPour].self, forKey: .pours) ?? []
        netClasses = try c.decodeIfPresent([PlannedNetClass].self, forKey: .netClasses) ?? []
        noConnect = try c.decodeIfPresent([String].self, forKey: .noConnect) ?? []
        robotPlatform = try c.decodeIfPresent(String.self, forKey: .robotPlatform)
        ecuType = try c.decodeIfPresent(String.self, forKey: .ecuType)
        aerospaceMission = try c.decodeIfPresent(String.self, forKey: .aerospaceMission)
        navalPlatform = try c.decodeIfPresent(String.self, forKey: .navalPlatform)
        medicalClass = try c.decodeIfPresent(String.self, forKey: .medicalClass)
        retailDevice = try c.decodeIfPresent(String.self, forKey: .retailDevice)
        tamperMeshes = try c.decodeIfPresent([PlannedTamperMesh].self, forKey: .tamperMeshes) ?? []
        applianceType = try c.decodeIfPresent(String.self, forKey: .applianceType)
        memoryDesign = try c.decodeIfPresent(String.self, forKey: .memoryDesign)
        sheets = try c.decodeIfPresent([PlannedSheet].self, forKey: .sheets) ?? []
        buses = try? c.decodeIfPresent([PlannedBus].self, forKey: .buses)
        harnessTypes = try? c.decodeIfPresent([PlannedHarnessType].self, forKey: .harnessTypes)
        netClassDefs = try? c.decodeIfPresent([PlannedNetClassDef].self, forKey: .netClassDefs)
        directives = try? c.decodeIfPresent([PlannedDirective].self, forKey: .directives)
    }

    private enum CodingKeys: String, CodingKey {
        case title, summary, components, connections, notes, board, industry, pours, netClasses, noConnect, robotPlatform, ecuType, aerospaceMission, navalPlatform, medicalClass
        case retailDevice, tamperMeshes, applianceType, memoryDesign, sheets, buses, harnessTypes, netClassDefs, directives
    }

    func jsonString(pretty: Bool = true) -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = pretty ? [.prettyPrinted, .sortedKeys] : [.sortedKeys]
        guard let data = try? encoder.encode(self) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }
}

struct PlannedComponent: Codable, Equatable {
    var ref: String
    var kind: String
    var value: String
    var x: Double
    var y: Double
    var rotation: Int
    /// Built-in example firmware id for a microcontroller (reference designs; not part of the AI schema).
    var firmware: String?
    /// Fixed board position (mm) and rotation: the footprint is placed there and locked, as designers fix
    /// connectors and matched-length bus parts before Auto Place fills in the rest.
    var pcb: PlannedPlacement?
    /// Multi-sheet designs: the sheet (by name) the part is drawn on; nil = the first sheet.
    var sheet: String?
    /// Net labels: "local", "port" or "entry" (nil = global); a sheet entry leads into `targetSheet` (by name).
    var scope: String?
    var targetSheet: String?
    /// Part of a repeated sheet: its designator inside the block ("R1"; `ref` is the first channel's, R201 / R1_A).
    var blockRef: String?
    /// Per-channel values of a repeated sheet's part, by channel path ("B", nested "B/A"); nil = every channel the same.
    var channelValues: [String: String]?
    /// Multi-unit part placed gate by gate: where each unit sits (nil = drawn as one symbol). Connections still name
    /// the part's pins ("U1.7"); they reach the unit that draws the pin.
    var units: [PlannedUnit]?
    /// Bus entries: index into `DesignPlan.buses` of the bus the label leaves.
    var bus: Int?
    /// Signal harnesses: a harness label's type, and for a harness entry the reference of its harness label.
    var harness: String?
    var harnessOf: String?

    init(ref: String, kind: String, value: String, x: Double, y: Double, rotation: Int = 0, firmware: String? = nil,
         pcb: PlannedPlacement? = nil, sheet: String? = nil, scope: String? = nil, targetSheet: String? = nil) {
        self.ref = ref
        self.kind = kind
        self.value = value
        self.x = x
        self.y = y
        self.rotation = rotation
        self.firmware = firmware
        self.pcb = pcb
        self.sheet = sheet
        self.scope = scope
        self.targetSheet = targetSheet
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ref = try c.decode(String.self, forKey: .ref)
        kind = try c.decode(String.self, forKey: .kind)
        value = try c.decodeIfPresent(String.self, forKey: .value) ?? ""
        x = try c.decodeIfPresent(Double.self, forKey: .x) ?? 0
        y = try c.decodeIfPresent(Double.self, forKey: .y) ?? 0
        if let r = try? c.decodeIfPresent(Int.self, forKey: .rotation) {
            rotation = r
        } else if let r = try? c.decodeIfPresent(Double.self, forKey: .rotation) {
            rotation = Int(r)
        } else {
            rotation = 0
        }
        firmware = try c.decodeIfPresent(String.self, forKey: .firmware)
        pcb = try? c.decodeIfPresent(PlannedPlacement.self, forKey: .pcb)
        sheet = try? c.decodeIfPresent(String.self, forKey: .sheet)
        scope = try? c.decodeIfPresent(String.self, forKey: .scope)
        targetSheet = try? c.decodeIfPresent(String.self, forKey: .targetSheet)
        blockRef = try? c.decodeIfPresent(String.self, forKey: .blockRef)
        channelValues = try? c.decodeIfPresent([String: String].self, forKey: .channelValues)
        units = try? c.decodeIfPresent([PlannedUnit].self, forKey: .units)
        bus = try? c.decodeIfPresent(Int.self, forKey: .bus)
        harness = try? c.decodeIfPresent(String.self, forKey: .harness)
        harnessOf = try? c.decodeIfPresent(String.self, forKey: .harnessOf)
    }

    private enum CodingKeys: String, CodingKey {
        case ref, kind, value, x, y, rotation, firmware, pcb, sheet, scope, targetSheet, blockRef, channelValues, units, bus
        case harness, harnessOf
    }
}

/// A schematic sheet of a multi-sheet plan; `parent` names the sheet whose sheet symbol stands for it.
struct PlannedSheet: Codable, Equatable {
    var name: String
    var parent: String?
    /// Repeated sheet (multi-channel block): on the block, its channel count, own channel label and channel
    /// designator scheme ("sheet" | "suffix"); on each other channel, the block it repeats (`instanceOf`, by name) and
    /// its label. Channels hold no parts in a plan: the block's parts are drawn once.
    var channels: Int?
    var channel: String?
    var refs: String?
    var instanceOf: String?

    init(name: String, parent: String? = nil, channels: Int? = nil, channel: String? = nil, refs: String? = nil,
         instanceOf: String? = nil) {
        self.name = name
        self.parent = parent
        self.channels = channels
        self.channel = channel
        self.refs = refs
        self.instanceOf = instanceOf
    }
}

/// One placed unit (gate) of a multi-unit part in a plan.
struct PlannedUnit: Codable, Equatable {
    var unit: String  // unit name: "A", "B", "P"
    var x: Double
    var y: Double
    var rotation: Int = 0
    var sheet: String?
}

/// A graphical bus in a plan: its name in bus notation ("D[0..7]"), sheet and polyline.
struct PlannedBus: Codable, Equatable {
    var name: String
    var sheet: String?
    var points: [PlannedPoint]
}

/// A signal harness type in a plan.
struct PlannedHarnessType: Codable, Equatable {
    var name: String
    var entries: [String]
}

/// A schematic net class in a plan (mm; nil = the board default).
struct PlannedNetClassDef: Codable, Equatable {
    var name: String
    var trackWidth: Double?
    var clearance: Double?
}

/// A directive on the net of a pin ("REF.PIN", as in connections).
struct PlannedDirective: Codable, Equatable {
    var at: String
    var netClass: String?
    var diffPair: Bool?
    var trackWidth: Double?
    var clearance: Double?
}

struct PlannedPoint: Codable, Equatable {
    var x: Double
    var y: Double
}

/// A locked footprint position on the board (mm, y down) with its rotation (multiple of 90°).
struct PlannedPlacement: Codable, Equatable {
    var x: Double
    var y: Double
    var rotation: Int = 0
}

struct PlannedConnection: Codable, Equatable {
    /// "REF.PIN", e.g. "R1.2", "U1.IN+", "Q1.B".
    var from: String
    var to: String
}

struct PlannedBoard: Codable, Equatable {
    var width: Double
    var height: Double
    /// Copper layers (1, 2, 4 … 24, even); 0 keeps the current stack-up.
    var layers: Int
    /// "rectangle", "rounded", "circle", "quad-x", or "keep" (leave the current outline).
    var outline: String
    /// Rounded: corner radius · quad-X: arm width (width = span, height = body); 0 = default.
    var outlineParameter: Double
    /// Square mounting-hole pattern spacing in mm (30.5 = M3 flight-controller stack); 0 = none, −1 = keep.
    var mountingHoleSpacing: Double
    /// Laminate id ("megtron-6", "rogers-4350b", …); empty keeps the current material.
    var material: String
    /// Backdrill via stubs on fast nets (≥ 4 layers).
    var backdrill: Bool
    /// Controlled-impedance targets (Ω); 0 keeps the current ones (85 PCIe, 90 USB, 100 Ethernet / SerDes).
    var singleEndedOhms: Double
    var differentialOhms: Double
    /// Conformal coating ("parylene", "silicone", …); empty keeps the current one.
    var coating: String
    /// Board thickness in mm; 0 keeps it.
    var thickness: Double
    /// Underfill / corner bonding of heavy parts; nil keeps the current setting.
    var underfill: Bool?
    /// Isolation barrier spacing between galvanic domains (mm); 0 keeps the current one.
    var isolationGap: Double

    /// Defaults describe a fresh rectangular board without holes (reference designs); refinement plans built from
    /// the current design pass "keep" / −1 explicitly.
    init(width: Double = 50, height: Double = 40, layers: Int = 0, outline: String = "rectangle",
         outlineParameter: Double = 0, mountingHoleSpacing: Double = 0, material: String = "", backdrill: Bool = false,
         singleEndedOhms: Double = 0, differentialOhms: Double = 0, coating: String = "", thickness: Double = 0,
         underfill: Bool? = nil, isolationGap: Double = 0) {
        self.width = width
        self.height = height
        self.layers = layers
        self.outline = outline
        self.outlineParameter = outlineParameter
        self.mountingHoleSpacing = mountingHoleSpacing
        self.material = material
        self.backdrill = backdrill
        self.singleEndedOhms = singleEndedOhms
        self.differentialOhms = differentialOhms
        self.coating = coating
        self.thickness = thickness
        self.underfill = underfill
        self.isolationGap = isolationGap
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        width = try c.decodeIfPresent(Double.self, forKey: .width) ?? 50
        height = try c.decodeIfPresent(Double.self, forKey: .height) ?? 40
        layers = (try? c.decodeIfPresent(Int.self, forKey: .layers)) ?? 0
        outline = try c.decodeIfPresent(String.self, forKey: .outline) ?? "keep"
        outlineParameter = try c.decodeIfPresent(Double.self, forKey: .outlineParameter) ?? 0
        mountingHoleSpacing = try c.decodeIfPresent(Double.self, forKey: .mountingHoleSpacing) ?? -1
        material = try c.decodeIfPresent(String.self, forKey: .material) ?? ""
        backdrill = try c.decodeIfPresent(Bool.self, forKey: .backdrill) ?? false
        singleEndedOhms = try c.decodeIfPresent(Double.self, forKey: .singleEndedOhms) ?? 0
        differentialOhms = try c.decodeIfPresent(Double.self, forKey: .differentialOhms) ?? 0
        coating = try c.decodeIfPresent(String.self, forKey: .coating) ?? ""
        thickness = try c.decodeIfPresent(Double.self, forKey: .thickness) ?? 0
        underfill = try c.decodeIfPresent(Bool.self, forKey: .underfill)
        isolationGap = try c.decodeIfPresent(Double.self, forKey: .isolationGap) ?? 0
    }

    private enum CodingKeys: String, CodingKey {
        case width, height, layers, outline, outlineParameter, mountingHoleSpacing, material, backdrill
        case singleEndedOhms, differentialOhms, coating, thickness, underfill, isolationGap
    }
}

struct PlannedPour: Codable, Equatable {
    var net: String
    /// Copper layer: 0 = top; −1 = bottom (whatever the stack-up).
    var layer: Int
    var plane: Bool
}

/// Tamper mesh over a secure element: `netA` / `netB` each join two of its pins (drive and sense).
struct PlannedTamperMesh: Codable, Equatable {
    var component: String
    var netA: String
    var netB: String
    var margin: Double = 2

    init(component: String, netA: String, netB: String, margin: Double = 2) {
        self.component = component
        self.netA = netA
        self.netB = netB
        self.margin = margin
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        component = try c.decode(String.self, forKey: .component)
        netA = try c.decode(String.self, forKey: .netA)
        netB = try c.decode(String.self, forKey: .netB)
        margin = try c.decodeIfPresent(Double.self, forKey: .margin) ?? 2
    }

    private enum CodingKeys: String, CodingKey { case component, netA, netB, margin }
}

struct PlannedNetClass: Codable, Equatable {
    var net: String
    var width: Double
}

/// Product-level requirements extracted from a prompt or PRD by the Requirements Analyst agent.
struct RequirementsSpec: Codable, Equatable {
    var title: String
    var summary: String
    var functionalRequirements: [String]
    var electricalConstraints: [String]
    var supplyVoltage: Double
    var boardWidthMM: Double
    var boardHeightMM: Double
    var designBlocks: [String]

    enum CodingKeys: String, CodingKey {
        case title, summary
        case functionalRequirements = "functional_requirements"
        case electricalConstraints = "electrical_constraints"
        case supplyVoltage = "supply_voltage"
        case boardWidthMM = "board_width_mm"
        case boardHeightMM = "board_height_mm"
        case designBlocks = "design_blocks"
    }

    func jsonString() -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        guard let data = try? encoder.encode(self) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }
}

/// Output of the Design Review agent.
struct DesignReview: Codable, Equatable {
    var approved: Bool
    var issues: [String]
    var plan: DesignPlan
}

// MARK: - JSON Schemas (used for structured outputs on every provider)

enum DesignSchemas {
    static var kindNames: [String] { ComponentKind.builtIn.map(\.planName) }

    static var designPlan: [String: Any] { designPlanSchema(customKinds: []) }

    static var industryIds: [String] {
        let ids = StandardLibrary.industries.map(\.id)
        return ids.isEmpty ? ["general"] : ids
    }

    /// `customKinds` are "custom:<NAME>" identifiers of parts in the project library.
    static func designPlanSchema(customKinds: [String]) -> [String: Any] {
        let kinds = kindNames + customKinds
        return [
            "type": "object",
            "additionalProperties": false,
            "required": ["title", "summary", "components", "connections", "notes", "board", "industry", "pours",
                         "netClasses", "noConnect"],
            "properties": [
                "pours": [
                    "type": "array",
                    "description": "Copper pours: usually GND on both layers of a 2-layer board, or a GND plane on layer 1 of a 4-layer board",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["net", "layer", "plane"],
                        "properties": [
                            "net": ["type": "string", "description": "Net name, e.g. GND"],
                            "layer": ["type": "integer", "description": "Copper layer: 0 = top, -1 = bottom, 1.. = inner"],
                            "plane": ["type": "boolean", "description": "Reserve the layer as a plane (inner layers)"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "netClasses": [
                    "type": "array",
                    "description": "Wider tracks for high-current nets (battery, motor, regulator output)",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["net", "width"],
                        "properties": [
                            "net": ["type": "string"],
                            "width": ["type": "number", "description": "Track width in mm"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "noConnect": ["type": "array", "items": ["type": "string"],
                              "description": "REF.PIN of pins intentionally left open (unused MCU pins)"] as [String: Any],
                "industry": ["type": "string", "enum": industryIds,
                             "description": "Industry profile that sets derating and design rules"] as [String: Any],
                "robotPlatform": ["type": "string", "enum": ["rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"],
                                  "description": "Robot platform: turns on the 7-segment robotics checks"] as [String: Any],
                "ecuType": ["type": "string", "enum": ["bcm", "powertrain", "adas", "ev", "chassis", "gateway"],
                            "description": "Automotive ECU type: turns on the 6-segment ECU checks"] as [String: Any],
                "aerospaceMission": ["type": "string", "enum": ["leo", "geo", "launcher", "military", "commercial"],
                                     "description": "Aerospace mission: turns on the 5-segment aerospace checks"] as [String: Any],
                "navalPlatform": ["type": "string", "enum": ["combatant", "carrier", "submarine", "patrol", "commercial"],
                                  "description": "Naval platform: turns on the 5-segment naval checks"] as [String: Any],
                "medicalClass": ["type": "string", "enum": ["bf", "cf", "life", "implant", "home"],
                                 "description": "Medical device class: turns on the 4-segment medical checks"] as [String: Any],
                "retailDevice": ["type": "string", "enum": ["countertop", "unattended", "mpos", "kiosk", "printer"],
                                 "description": "Retail / POS device class: turns on the 4-segment POS checks"] as [String: Any],
                "applianceType": ["type": "string", "enum": ["laundry", "kitchen", "refrigeration", "hvac", "small"],
                                  "description": "Home appliance type: turns on the 4-segment appliance checks"] as [String: Any],
                "memoryDesign": ["type": "string", "enum": ["sdram", "ddr", "lpddr", "dimm", "rdimm"],
                                 "description": "Memory design type: turns on the 5-segment memory (RAM) checks"] as [String: Any],
                "tamperMeshes": [
                    "type": "array",
                    "description": "Active tamper meshes (PCI PTS) over secure elements; needs 4+ layers",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["component", "netA", "netB"],
                        "properties": [
                            "component": ["type": "string", "description": "Secure element reference, e.g. U1"],
                            "netA": ["type": "string", "description": "Mesh net joining two pins (horizontal stripes, inner 1)"],
                            "netB": ["type": "string", "description": "Mesh net joining two pins (vertical stripes, inner 2)"],
                            "margin": ["type": "number", "description": "Mesh overhang around the part, mm (default 2)"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "title": ["type": "string"],
                "summary": ["type": "string"],
                "components": [
                    "type": "array",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["ref", "kind", "value", "x", "y", "rotation"],
                        "properties": [
                            "ref": ["type": "string", "description": "Unique reference designator, e.g. R1, C2, U1"],
                            "kind": ["type": "string", "enum": kinds] as [String: Any],
                            "value": ["type": "string"],
                            "x": ["type": "number"],
                            "y": ["type": "number"],
                            "rotation": ["type": "integer", "enum": [0, 90, 180, 270]] as [String: Any],
                            "sheet": ["type": "string", "description": "Multi-sheet designs: the sheet (by name) the part is on"],
                            "scope": ["type": "string", "enum": ["global", "local", "port", "entry"],
                                      "description": "Net labels: how far the label reaches; keep the current plan's"] as [String: Any],
                            "targetSheet": ["type": "string", "description": "Sheet entries: the child sheet (by name)"],
                            "blockRef": ["type": "string",
                                         "description": "Parts of a repeated sheet: the designator inside the block; keep it"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "sheets": [
                    "type": "array",
                    "description": "Schematic sheets of a multi-sheet design: keep the current plan's sheets, hierarchy and repeated channels",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["name"],
                        "properties": [
                            "name": ["type": "string"],
                            "parent": ["type": "string", "description": "Parent sheet (by name)"],
                            "channels": ["type": "integer", "description": "Repeated sheet: number of channels (block only)"],
                            "channel": ["type": "string", "description": "Channel label"],
                            "refs": ["type": "string", "enum": ["sheet", "suffix"]] as [String: Any],
                            "instanceOf": ["type": "string", "description": "A channel sheet: the block it repeats (by name)"],
                        ] as [String: Any],
                    ] as [String: Any],
                ] as [String: Any],
                "connections": [
                    "type": "array",
                    "items": [
                        "type": "object",
                        "additionalProperties": false,
                        "required": ["from", "to"],
                        "properties": [
                            "from": ["type": "string", "description": "REF.PIN, e.g. R1.2"],
                            "to": ["type": "string", "description": "REF.PIN, e.g. D1.A"],
                        ],
                    ] as [String: Any],
                ] as [String: Any],
                "notes": ["type": "array", "items": ["type": "string"]] as [String: Any],
                "board": [
                    "type": "object",
                    "additionalProperties": false,
                    "required": ["width", "height", "layers", "outline", "outlineParameter", "mountingHoleSpacing"],
                    "properties": [
                        "width": ["type": "number", "description": "Board width in millimetres (quad-x: frame span)"],
                        "height": ["type": "number", "description": "Board height in millimetres (quad-x: body size)"],
                        "layers": ["type": "integer", "enum": [0] + BoardInfo.layerChoices,
                                   "description": "Copper layers; 0 keeps the current stack-up"] as [String: Any],
                        "outline": ["type": "string", "enum": ["keep", "rectangle", "rounded", "circle", "quad-x"],
                                    "description": "Board shape; quad-x for multirotor frames"] as [String: Any],
                        "outlineParameter": ["type": "number",
                                             "description": "rounded: corner radius; quad-x: arm width (mm); 0 = default"],
                        "mountingHoleSpacing": ["type": "number",
                                                "description": "Square mounting pattern in mm (30.5 = M3 FC stack, 20 = M2); 0 none, -1 keep"],
                        "material": ["type": "string",
                                     "enum": ["", "fr4", "fr4-hightg", "isola-370hr", "rogers-4350b", "megtron-6",
                                              "megtron-7", "tachyon-100g", "polyimide", "ims-aluminium"],
                                     "description": "Laminate; empty keeps FR-4 / the current one"] as [String: Any],
                        "backdrill": ["type": "boolean", "description": "Backdrill via stubs on fast nets (4+ layers)"],
                        "singleEndedOhms": ["type": "number", "description": "Single-ended impedance target; 0 keeps"],
                        "differentialOhms": ["type": "number",
                                             "description": "Differential impedance: 85 PCIe, 90 USB, 100 Ethernet/SerDes; 0 keeps"],
                        "coating": ["type": "string", "enum": ["", "none", "acrylic", "silicone", "urethane", "epoxy", "parylene"],
                                    "description": "Conformal coating; empty keeps"] as [String: Any],
                        "thickness": ["type": "number", "description": "Board thickness in mm (1.6 standard, 2.4 for shock); 0 keeps"],
                        "underfill": ["type": "boolean", "description": "Underfill / corner-bond heavy parts (shock, vibration)"],
                        "isolationGap": ["type": "number",
                                         "description": "Creepage between galvanic domains in mm (8 = 2 × MOPP patient barrier); 0 keeps"],
                    ] as [String: Any],
                ] as [String: Any],
            ] as [String: Any],
        ]
    }

    static var requirements: [String: Any] {
        let stringArray: [String: Any] = ["type": "array", "items": ["type": "string"]]
        return [
            "type": "object",
            "additionalProperties": false,
            "required": ["title", "summary", "functional_requirements", "electrical_constraints", "supply_voltage",
                         "board_width_mm", "board_height_mm", "design_blocks"],
            "properties": [
                "title": ["type": "string"],
                "summary": ["type": "string"],
                "functional_requirements": stringArray,
                "electrical_constraints": stringArray,
                "supply_voltage": ["type": "number"],
                "board_width_mm": ["type": "number"],
                "board_height_mm": ["type": "number"],
                "design_blocks": stringArray,
            ] as [String: Any],
        ]
    }

    static var review: [String: Any] { reviewSchema(customKinds: []) }

    static func reviewSchema(customKinds: [String]) -> [String: Any] {
        [
            "type": "object",
            "additionalProperties": false,
            "required": ["approved", "issues", "plan"],
            "properties": [
                "approved": ["type": "boolean"],
                "issues": ["type": "array", "items": ["type": "string"]] as [String: Any],
                "plan": designPlanSchema(customKinds: customKinds),
            ] as [String: Any],
        ]
    }
}

// MARK: - Compiling a plan into the engine

struct PlanApplyReport: Equatable {
    var componentsAdded = 0
    var connectionsMade = 0
    var warnings: [String] = []
}

enum DesignPlanCompiler {
    /// Replaces the schematic with `plan`, preserving PCB placement of parts whose reference survives.
    @discardableResult
    static func apply(_ plan: DesignPlan, to engine: EDAEngine, previous: DesignSnapshot?) -> PlanApplyReport {
        var report = PlanApplyReport()
        var keptPlacement: [String: PcbPlacement] = [:]
        for c in previous?.components ?? [] where c.pcb.placed {
            keptPlacement[c.ref] = c.pcb
        }
        // Firmware stays with its microcontroller (by reference) when a plan rebuilds the schematic.
        var keptFirmware: [String: (hex: String, name: String, clock: Double)] = [:]
        for c in previous?.components ?? [] {
            if let mcu = c.mcu, mcu.hasFirmware {
                keptFirmware[c.ref] = (engine.firmware(of: c.id), mcu.firmwareName, mcu.clockHz)
            }
        }

        engine.clear()
        engine.setName(plan.title)
        if let industry = plan.industry, !engine.setIndustry(industry) {
            report.warnings.append("Unknown industry profile '\(industry)' — kept the current profile.")
        }
        if let platform = plan.robotPlatform, !engine.setRobotPlatform(platform) {
            report.warnings.append("Unknown robot platform '\(platform)'.")
        }
        if let type = plan.ecuType, !engine.setEcuType(type) {
            report.warnings.append("Unknown ECU type '\(type)'.")
        }
        if let mission = plan.aerospaceMission, !engine.setAerospaceMission(mission) {
            report.warnings.append("Unknown aerospace mission '\(mission)'.")
        }
        if let platform = plan.navalPlatform, !engine.setNavalPlatform(platform) {
            report.warnings.append("Unknown naval platform '\(platform)'.")
        }
        if let cls = plan.medicalClass, !engine.setMedicalClass(cls) {
            report.warnings.append("Unknown medical class '\(cls)'.")
        }
        if let device = plan.retailDevice, !engine.setRetailDevice(device) {
            report.warnings.append("Unknown retail device '\(device)'.")
        }
        if let type = plan.applianceType, !engine.setApplianceType(type) {
            report.warnings.append("Unknown appliance type '\(type)'.")
        }
        if let type = plan.memoryDesign, !engine.setMemoryDesign(type) {
            report.warnings.append("Unknown memory design type '\(type)'.")
        }
        // Sheets of a multi-sheet plan: the first takes over the blank project's sheet; parents are set by name.
        // Channels of repeated sheets are made by repeating their block (below), not drawn: parts an agent put on a
        // channel go to the block.
        var sheetIds: [String: Int] = [:]
        let channelOf = Dictionary(plan.sheets.compactMap { s in s.instanceOf.map { (s.name, $0) } },
                                   uniquingKeysWith: { a, _ in a })
        func blockSheet(_ name: String?) -> String? { name.map { channelOf[$0] ?? $0 } }
        for sheet in plan.sheets where sheetIds[sheet.name] == nil && sheet.instanceOf == nil {
            if sheetIds.isEmpty {
                if engine.renameSheet(1, to: sheet.name) { sheetIds[sheet.name] = 1 }
            } else if let id = engine.addSheet(sheet.name) {
                sheetIds[sheet.name] = id
            }
        }
        for sheet in plan.sheets where sheet.instanceOf == nil {
            if let parent = sheet.parent, let id = sheetIds[sheet.name], let parentId = sheetIds[parent] {
                engine.setSheetParent(id, parent: parentId)
            }
        }
        // Harness types and net classes, before the labels and directives that use them.
        for type in plan.harnessTypes ?? [] where !engine.setHarnessType(type.name, entries: type.entries) {
            report.warnings.append("Harness type '\(type.name)' could not be defined.")
        }
        for def in plan.netClassDefs ?? []
        where !engine.setNetClass(def.name, trackWidth: def.trackWidth ?? 0, clearance: def.clearance ?? 0) {
            report.warnings.append("Net class '\(def.name)' could not be defined.")
        }
        // Buses, before their entries.
        var busIds: [Int: Int] = [:]
        for (index, bus) in (plan.buses ?? []).enumerated() {
            if !sheetIds.isEmpty { engine.setActiveSheet(blockSheet(bus.sheet).flatMap { sheetIds[$0] } ?? 1) }
            if let id = engine.addBus(bus.name, points: bus.points.map { CGPoint(x: $0.x, y: $0.y) }) {
                busIds[index] = id
            } else {
                report.warnings.append("Bus '\(bus.name)' could not be drawn.")
            }
        }

        var positions = plan.components.map { CGPoint(x: $0.x, y: $0.y) }
        if sheetIds.isEmpty {
            positions = SchematicAutoLayout.resolveOverlaps(positions)
        } else {
            // Each sheet is its own drawing: overlaps are resolved sheet by sheet.
            let groups = Dictionary(grouping: plan.components.indices) { blockSheet(plan.components[$0].sheet) ?? "" }
            for indices in groups.values {
                let resolved = SchematicAutoLayout.resolveOverlaps(indices.map { positions[$0] })
                for (k, i) in indices.enumerated() { positions[i] = resolved[k] }
            }
        }

        var seenRefs = Set<String>()
        let library = previous?.customParts ?? []
        var unitPins: [String: [String: PinAddress]] = [:]  // multi-unit part ref → pin number → the unit pin drawing it
        var deferredScopes: [(id: Int, ref: String, scope: String, target: String)] = []  // entries into channels
        var placedIds: [Int: Int] = [:]  // plan index → component id
        for (index, item) in plan.components.enumerated() {
            if !sheetIds.isEmpty { engine.setActiveSheet(blockSheet(item.sheet).flatMap { sheetIds[$0] } ?? 1) }
            if item.kind.lowercased().hasPrefix("custom:") {
                let name = String(item.kind.dropFirst("custom:".count)).trimmingCharacters(in: .whitespaces)
                var partId = library.first(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame })?.id
                if partId == nil,
                   let standard = StandardLibrary.parts.first(where: { $0.spec.name.caseInsensitiveCompare(name) == .orderedSame }) {
                    // Built-in standard part (LM7805, NE555, …): add it to the project library on first use.
                    partId = try? engine.registerCustomPart(standard.spec).id
                }
                if partId == nil, let near = Self.closestStandardPart(name) {
                    // The model wrote the part number with other punctuation or an ordering suffix ("lm358-dr").
                    partId = library.first(where: { $0.name == near.spec.name })?.id ?? (try? engine.registerCustomPart(near.spec).id)
                    if partId != nil { report.warnings.append("\(item.ref): used catalog part \(near.spec.name) for '\(name)'.") }
                }
                guard let partId else {
                    report.warnings.append("Skipped \(item.ref): '\(name)' is not in the component library.")
                    continue
                }
                var ref = item.ref.trimmingCharacters(in: .whitespaces)
                if seenRefs.contains(ref) { ref = "" }
                let rotation = ((item.rotation % 360) + 360) % 360 / 90 * 90
                if let units = item.units, !units.isEmpty,
                   let id = placeUnits(units, partId: partId, item: item, ref: ref, engine: engine, library: library, sheetIds: sheetIds,
                                       blockSheet: blockSheet, unitPins: &unitPins, report: &report) {
                    placedIds[index] = id
                    report.componentsAdded += 1
                    if !ref.isEmpty { seenRefs.insert(ref) }
                    continue
                }
                let id = engine.addCustomComponent(partId: partId, value: item.value.isEmpty ? nil : item.value,
                                                   at: positions[index], rotation: rotation, ref: ref.isEmpty ? nil : ref)
                if id >= 0 {
                    placedIds[index] = id
                    report.componentsAdded += 1
                    if !ref.isEmpty { seenRefs.insert(ref) }
                }
                continue
            }
            guard let kind = ComponentKind.fromPlanName(item.kind), kind != .custom else {
                report.warnings.append("Skipped \(item.ref): unknown component kind '\(item.kind)'.")
                continue
            }
            var ref = item.ref.trimmingCharacters(in: .whitespaces)
            if seenRefs.contains(ref) {
                report.warnings.append("Duplicate reference \(ref) renamed automatically.")
                ref = ""
            }
            let value = item.value.isEmpty ? kind.defaultValue : item.value
            let rotation = ((item.rotation % 360) + 360) % 360 / 90 * 90
            let id = engine.addComponent(kind, value: value, at: positions[index], rotation: rotation,
                                         ref: ref.isEmpty ? nil : ref)
            if id >= 0 {
                placedIds[index] = id
                report.componentsAdded += 1
                if !ref.isEmpty { seenRefs.insert(ref) }
                if kind == .netLabel, let scope = item.scope, scope != "global" {
                    if scope == "entry", let target = item.targetSheet, channelOf[target] != nil {
                        deferredScopes.append((id, item.ref, scope, target))  // its channel exists once the block repeats
                    } else if !engine.setLabelScope(id, scope: scope, targetSheet: item.targetSheet.flatMap { sheetIds[$0] } ?? 0) {
                        report.warnings.append("\(item.ref): label scope '\(scope)' could not be set; the label stays global.")
                    }
                }
                if kind == .netLabel, let bus = item.bus {
                    if busIds[bus].map({ engine.setLabelBus(id, bus: $0) }) != true {
                        report.warnings.append("\(item.ref): not an entry of bus \(bus).")
                    }
                }
            }
        }
        if !sheetIds.isEmpty { engine.setActiveSheet(1) }

        for connection in plan.connections {
            guard let a = resolve(connection.from, engine: engine, report: &report, unitPins: unitPins),
                  let b = resolve(connection.to, engine: engine, report: &report, unitPins: unitPins) else { continue }
            if a == b { continue }
            if engine.connect(a, b) != nil { report.connectionsMade += 1 }
        }
        // Harness labels, then their entries (after the wiring: an entry joins its member net by name).
        for (index, item) in plan.components.enumerated() {
            guard let id = placedIds[index], let type = item.harness, !type.isEmpty else { continue }
            if !engine.setLabelHarness(id, type: type) { report.warnings.append("\(item.ref): harness '\(type)' could not be set.") }
        }
        let refIndex = Dictionary(plan.components.enumerated().map { ($0.element.ref, $0.offset) }, uniquingKeysWith: { a, _ in a })
        for (index, item) in plan.components.enumerated() {
            guard let id = placedIds[index], let owner = item.harnessOf, let ownerIndex = refIndex[owner],
                  let harness = placedIds[ownerIndex] else { continue }
            if !engine.setHarnessEntry(id, harness: harness) {
                report.warnings.append("\(item.ref): not an entry of harness \(owner).")
            }
        }
        if plan.sheets.contains(where: { $0.channels ?? 1 > 1 || $0.instanceOf != nil }) {
            applyRepeatedSheets(plan, engine: engine, sheetIds: &sheetIds, placedIds: placedIds,
                                deferredScopes: deferredScopes, report: &report)
        }
        for directive in plan.directives ?? [] {
            guard let pin = resolve(directive.at, engine: engine, report: &report, unitPins: unitPins) else { continue }
            if engine.addDirective(component: pin.component, pin: pin.pin, netClass: directive.netClass ?? "",
                                   diffPair: directive.diffPair ?? false, trackWidth: directive.trackWidth ?? 0,
                                   clearance: directive.clearance ?? 0) == nil {
                report.warnings.append("Directive on \(directive.at) could not be added.")
            }
        }

        let board = plan.board
        if BoardInfo.layerChoices.contains(board.layers) { engine.setLayerCount(board.layers) }
        if !board.material.isEmpty || board.backdrill || board.singleEndedOhms > 0 || board.differentialOhms > 0,
           let current = engine.snapshot()?.board {
            let material = board.material.isEmpty ? current.material : board.material
            if !engine.setStackup(material: material, construction: current.boardConstruction,
                                  singleEnded: board.singleEndedOhms > 0 ? board.singleEndedOhms : current.singleEndedImpedance,
                                  differential: board.differentialOhms > 0 ? board.differentialOhms : current.differentialImpedance,
                                  backdrill: board.backdrill) {
                report.warnings.append("Board material '\(board.material)' is not in the laminate list.")
            }
        }
        if !board.coating.isEmpty {
            if let coating = ConformalCoating(rawValue: board.coating) { _ = engine.setCoating(coating) } else {
                report.warnings.append("Unknown conformal coating '\(board.coating)'.")
            }
        }
        if board.isolationGap > 0, !engine.setIsolationGap(board.isolationGap) {
            report.warnings.append("Isolation gap \(board.isolationGap) mm is outside 0–25 mm.")
        }
        if board.thickness > 0 || board.underfill != nil, let current = engine.snapshot()?.board {
            if !engine.setMechanical(thickness: board.thickness > 0 ? board.thickness : current.thickness,
                                     underfill: board.underfill ?? current.underfill) {
                report.warnings.append("Board thickness \(board.thickness) mm is outside 0.4–6.4 mm.")
            }
        }
        let sized = board.width >= 10 && board.height >= 5 && board.width <= 500 && board.height <= 500
        if let preset = BoardOutlinePreset(rawValue: board.outline.lowercased()), sized {
            if preset == .rectangle {
                engine.setOutline([])
                engine.setBoard(width: board.width, height: board.height, trackWidth: 0, clearance: 0)
            } else {
                let parameter = board.outlineParameter > 0 ? board.outlineParameter : (preset == .quadX ? 12 : 3)
                if !engine.applyOutlinePreset(preset, width: board.width, height: board.height, parameter: parameter) {
                    report.warnings.append("Board outline '\(board.outline)' could not be applied.")
                }
            }
        } else if sized, board.outline.lowercased() == "keep", previous?.board.hasCustomOutline != true {
            engine.setBoard(width: board.width, height: board.height, trackWidth: 0, clearance: 0)
        } else if board.outline.lowercased() != "keep" {
            report.warnings.append("Unknown board outline '\(board.outline)'.")
        }
        if board.mountingHoleSpacing >= 0 {
            engine.clearMountingHoles()
            let spacing = board.mountingHoleSpacing
            if spacing > 0, let snap = engine.snapshot() {
                let metric2 = spacing < 25
                let centre = CGPoint(x: snap.board.width / 2, y: snap.board.height / 2)
                for dx in [-spacing / 2, spacing / 2] {
                    for dy in [-spacing / 2, spacing / 2] {
                        engine.addMountingHole(at: CGPoint(x: centre.x + dx, y: centre.y + dy),
                                               drill: metric2 ? 2.2 : 3.2, keepout: metric2 ? 4.4 : 6.4)
                    }
                }
            }
        }

        // Pours and net classes replace the previous ones; no-connect marks follow the pins.
        engine.clearZones()
        let layerCount = engine.snapshot()?.board.layerCount ?? 2
        for pour in plan.pours {
            let layer = pour.layer < 0 ? max(0, layerCount - 1) : pour.layer
            if engine.addZone(net: pour.net, layer: layer, plane: pour.plane) == nil {
                report.warnings.append("Pour for \(pour.net) on layer \(pour.layer) does not fit the \(layerCount)-layer stack-up.")
            }
        }
        for net in previous?.board.netWidths.keys.sorted() ?? [] { engine.setNetWidth(net, width: 0) }
        for netClass in plan.netClasses where netClass.width > 0 {
            engine.setNetWidth(netClass.net, width: min(5, netClass.width))
        }
        for endpoint in plan.noConnect {
            if let pin = resolve(endpoint, engine: engine, report: &report, unitPins: unitPins) { engine.setPinNoConnect(pin, true) }
        }
        engine.clearTamperMeshes()
        for mesh in plan.tamperMeshes
        where engine.addTamperMesh(component: mesh.component, netA: mesh.netA, netB: mesh.netB, margin: mesh.margin) == nil {
            report.warnings.append("Tamper mesh over \(mesh.component) could not be added.")
        }
        for item in plan.components {
            guard let id = engine.findComponent(ref: item.ref) else { continue }
            if let place = item.pcb {
                engine.moveFootprint(id, to: CGPoint(x: place.x, y: place.y))
                let current = engine.snapshot()?.components.first { $0.id == id }?.pcb.rotation ?? 0
                let delta = ((place.rotation - current) % 360 + 360) % 360
                if delta != 0 { engine.rotateFootprint(id, by: delta) }
                engine.lockFootprint(id, true)
            }
            if let example = item.firmware {
                guard let hex = EDAEngine.firmwareExampleHex(example) else {
                    report.warnings.append("\(item.ref): unknown example firmware '\(example)'.")
                    continue
                }
                let name = EDAEngine.firmwareExamples.first { $0.id == example }?.name ?? example
                do { try engine.setFirmware(id, hex: hex, name: name, clockHz: 0) } catch {
                    report.warnings.append("\(item.ref): \(error.localizedDescription)")
                }
            } else if let kept = keptFirmware[item.ref] {
                try? engine.setFirmware(id, hex: kept.hex, name: kept.name, clockHz: kept.clock)
            }
        }

        for (ref, placement) in keptPlacement {
            guard let id = engine.findComponent(ref: ref) else { continue }
            engine.moveFootprint(id, to: CGPoint(x: placement.x, y: placement.y))
            if placement.rotation != 0 { engine.rotateFootprint(id, by: placement.rotation) }
            if placement.bottom { engine.flipFootprint(id) }
        }
        return report
    }

    /// Places a multi-unit part gate by gate where the plan puts its units. Returns the id of a placed unit (the part
    /// answers to it), and records which unit draws each pin of the part (by number, and by name when unique).
    private static func placeUnits(_ units: [PlannedUnit], partId: String, item: PlannedComponent, ref: String,
                                   engine: EDAEngine, library: [CustomPartInfo], sheetIds: [String: Int],
                                   blockSheet: (String?) -> String?,
                                   unitPins: inout [String: [String: PinAddress]],
                                   report: inout PlanApplyReport) -> Int? {
        guard let part = library.first(where: { $0.id == partId }) ?? engine.snapshot()?.customPart(partId),
              let symbols = part.unitSymbols, !symbols.isEmpty else {
            return nil
        }
        let wanted: [(unit: PlannedUnit, index: Int)] = units.compactMap { u in
            symbols.firstIndex { $0.name == u.unit }.map { (unit: u, index: $0 + 1) }
        }
        guard !wanted.isEmpty else { return nil }
        func activate(_ u: PlannedUnit) {
            if !sheetIds.isEmpty { engine.setActiveSheet(blockSheet(u.sheet).flatMap { sheetIds[$0] } ?? 1) }
        }
        func rotation(_ r: Int) -> Int { ((r % 360) + 360) % 360 / 90 * 90 }
        // Unit A comes with the package; when the plan leaves A out it only anchors the package and goes again.
        let anchor = wanted.first { $0.index == 1 } ?? wanted[0]
        activate(anchor.unit)
        let first = engine.addCustomUnits(partId: partId, value: item.value.isEmpty ? nil : item.value,
                                          at: CGPoint(x: anchor.unit.x, y: anchor.unit.y),
                                          rotation: rotation(anchor.unit.rotation), ref: ref.isEmpty ? nil : ref)
        guard first >= 0 else { return nil }
        var placed: [Int: Int] = [1: first]
        for (u, index) in wanted where index != 1 {
            activate(u)
            if let id = engine.addPartUnit(of: first, unit: index, at: CGPoint(x: u.x, y: u.y), rotation: rotation(u.rotation)) {
                placed[index] = id
            } else {
                report.warnings.append("\(item.ref): unit \(u.unit) could not be placed.")
            }
        }
        if anchor.index != 1, placed.count > 1 {
            engine.removeComponent(first)
            placed[1] = nil
        }
        var pins: [String: PinAddress] = [:]
        var nameCount: [String: Int] = [:]
        for symbol in symbols { for pin in symbol.symbol.pins { nameCount[pin.name, default: 0] += 1 } }
        for (index, id) in placed.sorted(by: { $0.key < $1.key }) {
            for (k, pin) in symbols[index - 1].symbol.pins.enumerated() {
                let address = PinAddress(component: id, pin: k)
                if pins[pin.number] == nil { pins[pin.number] = address }
                if nameCount[pin.name] == 1, pins[pin.name] == nil { pins[pin.name] = address }
            }
        }
        unitPins[item.ref.trimmingCharacters(in: .whitespaces)] = pins
        engine.setActiveSheet(sheetIds.isEmpty ? 1 : (blockSheet(item.sheet).flatMap { sheetIds[$0] } ?? 1))
        return placed.sorted { $0.key < $1.key }.first?.value
    }

    /// Repeated sheets of a plan: repeats every block (inner blocks first), names and labels its channels as planned,
    /// points the sheet entries into the channels, gives the block parts their block designators and sets the
    /// per-channel values.
    private static func applyRepeatedSheets(_ plan: DesignPlan, engine: EDAEngine, sheetIds: inout [String: Int],
                                            placedIds: [Int: Int],
                                            deferredScopes: [(id: Int, ref: String, scope: String, target: String)],
                                            report: inout PlanApplyReport) {
        let parentOf = Dictionary(plan.sheets.map { ($0.name, $0.parent) }, uniquingKeysWith: { a, _ in a })
        func depth(_ name: String) -> Int {
            var d = 0
            var current = parentOf[name] ?? nil
            while let c = current, d < 64 {
                d += 1
                current = parentOf[c] ?? nil
            }
            return d
        }
        // Inner blocks first: a block's child sheets must be repeated blocks before it can be repeated itself. A
        // block with one channel inside a repeated block is repeated twice first and brought back to one after.
        let blocks = plan.sheets.filter { $0.instanceOf == nil && $0.channels != nil }.sorted { depth($0.name) > depth($1.name) }
        var single: [Int] = []
        for block in blocks {
            guard let id = sheetIds[block.name], let count = block.channels, count >= 1 else { continue }
            if let label = block.channel, !label.isEmpty { engine.setSheetChannel(id, channel: label) }
            if engine.repeatSheet(id, count: min(64, max(2, count))) == nil {
                report.warnings.append("Sheet \(block.name) could not be repeated ×\(count).")
                continue
            }
            if count == 1 { single.append(id) }
            if let refs = block.refs { engine.setInstanceRefs(id, scheme: refs) }
        }
        for id in single { engine.repeatSheet(id, count: 1) }
        // Channel sheets: the planned names and labels (outer channels come before the channels inside them).
        if let snap = engine.snapshot() {
            for planned in plan.sheets {
                guard let blockName = planned.instanceOf, let def = sheetIds[blockName],
                      let block = snap.sheets.first(where: { $0.id == def }) else { continue }
                let parentId = planned.parent.flatMap { sheetIds[$0] } ?? block.parent
                let group = snap.sheets.filter { $0.id == def && $0.parent == parentId }
                    + snap.sheets.filter { $0.instanceOf == def && $0.id != def && $0.parent == parentId }
                var plannedGroup: [String] = []
                if (parentOf[blockName].flatMap { $0 }.flatMap { sheetIds[$0] } ?? 0) == parentId { plannedGroup.append(blockName) }
                plannedGroup += plan.sheets.filter {
                    $0.instanceOf == blockName && ($0.parent.flatMap { sheetIds[$0] } ?? block.parent) == parentId
                }.map(\.name)
                guard let k = plannedGroup.firstIndex(of: planned.name), k < group.count else {
                    report.warnings.append("Channel sheet \(planned.name) has no channel of \(blockName) to match.")
                    continue
                }
                let target = group[k]
                if target.name != planned.name { engine.renameSheet(target.id, to: planned.name) }
                sheetIds[planned.name] = target.id
                if let label = planned.channel, !label.isEmpty, target.channel != label {
                    engine.setSheetChannel(target.id, channel: label)
                }
            }
        }
        for entry in deferredScopes where !engine.setLabelScope(entry.id, scope: entry.scope, targetSheet: sheetIds[entry.target] ?? 0) {
            report.warnings.append("\(entry.ref): sheet entry into \(entry.target) could not be set; the label stays global.")
        }
        // Block designators, then the channels' own values.
        for (index, item) in plan.components.enumerated() {
            guard let id = placedIds[index], let block = item.blockRef, !block.isEmpty else { continue }
            engine.setRef(id, block)
        }
        let valued = plan.components.indices.filter { !(plan.components[$0].channelValues ?? [:]).isEmpty }
        guard !valued.isEmpty, let snap = engine.snapshot() else { return }
        for index in valued {
            guard let id = placedIds[index] else { continue }
            let item = plan.components[index]
            for (path, value) in (item.channelValues ?? [:]).sorted(by: { $0.key < $1.key }) {
                if let copy = snap.components.first(where: { $0.instanceOf == id && snap.sheet($0.sheetId)?.path == path }),
                   engine.setChannelValue(copy.id, value) {
                    continue
                }
                report.warnings.append("\(item.ref): no channel \(path) for its value \(value).")
            }
        }
    }

    /// "R1.2" → (component id, pin index). A multi-unit part placed gate by gate answers through `unitPins`: the
    /// unit that draws the pin.
    static func resolve(_ endpoint: String, engine: EDAEngine, report: inout PlanApplyReport,
                        unitPins: [String: [String: PinAddress]] = [:]) -> PinAddress? {
        let trimmed = endpoint.trimmingCharacters(in: .whitespaces)
        guard let dot = trimmed.lastIndex(of: ".") else {
            report.warnings.append("Connection endpoint '\(endpoint)' is not in REF.PIN form.")
            return nil
        }
        let ref = String(trimmed[..<dot])
        let pinName = String(trimmed[trimmed.index(after: dot)...])
        if let pins = unitPins[ref] {
            if let address = pins[pinName] ?? pins[pinName.uppercased()] { return address }
            report.warnings.append("Part \(ref) has no pin '\(pinName)' on its placed units.")
            return nil
        }
        guard let component = engine.findComponent(ref: ref) else {
            report.warnings.append("Connection references unknown part '\(ref)'.")
            return nil
        }
        guard let pin = engine.findPin(component: component, name: pinName)
                ?? engine.findPin(component: component, name: pinName.uppercased()) else {
            report.warnings.append("Part \(ref) has no pin '\(pinName)'.")
            return nil
        }
        return PinAddress(component: component, pin: pin)
    }

    /// Converts the current schematic back into a plan (context for refinement requests). The plan keeps the
    /// design's structure: sheets and hierarchy, repeated sheets (the block drawn once, its channels as sheets with
    /// labels and per-channel values), graphical buses with their entries, and multi-unit parts gate by gate.
    static func plan(from snapshot: DesignSnapshot) -> DesignPlan {
        let byId = Dictionary(uniqueKeysWithValues: snapshot.components.map { ($0.id, $0) })
        let junctions = Set(snapshot.components.filter { $0.componentKind == .junction }.map(\.id))
        let multiSheet = snapshot.sheets.count > 1
        // Channels of repeated sheets hold copies of their block: the plan draws the block once.
        let channelSheets = Set(snapshot.sheets.filter(\.isInstance).map(\.id))
        let onChannel: (SnapComponent) -> Bool = { channelSheets.contains($0.sheetId) }
        // Buses drawn on a block (not their channel copies), indexed for their entries.
        let plannedBuses = snapshot.buses.filter { $0.instanceOf == nil && !channelSheets.contains($0.sheet) }
        let busIndex = Dictionary(uniqueKeysWithValues: plannedBuses.enumerated().map { ($0.element.id, $0.offset) })
        // Per-channel values: the copies that set their own, by the channel path of their sheet.
        var channelValues: [Int: [String: String]] = [:]
        for c in snapshot.components where onChannel(c) {
            guard let master = c.instanceOf, (c.channelOverride ?? 0) & 1 != 0,
                  let path = snapshot.sheet(c.sheetId)?.path, !path.isEmpty else { continue }
            channelValues[master, default: [:]][path] = c.value
        }
        // A multi-unit part is planned whole (its package) with where each unit sits; connections to its units name
        // the package's pins.
        var unitsOf: [Int: [PlannedUnit]] = [:]
        for c in snapshot.components where !onChannel(c) {
            guard let package = c.unitOf, let name = c.unitName else { continue }
            unitsOf[package, default: []].append(PlannedUnit(unit: name, x: c.x, y: c.y, rotation: c.rotation,
                                                             sheet: multiSheet ? snapshot.sheet(c.sheetId)?.name : nil))
        }
        let planned = snapshot.components.filter { !junctions.contains($0.id) && $0.unitOf == nil && !onChannel($0) }
        let components = planned.map { c -> PlannedComponent in
            let kind = snapshot.customPart(for: c).map(\.planKind) ?? c.componentKind.planName
            let scoped = c.componentKind == .netLabel && c.labelScope != "global"
            var item = PlannedComponent(ref: c.ref, kind: kind, value: c.blockValue ?? c.value, x: c.x, y: c.y,
                                        rotation: c.rotation, sheet: multiSheet ? snapshot.sheet(c.sheetId)?.name : nil,
                                        scope: scoped ? c.labelScope : nil,
                                        targetSheet: c.targetSheet.flatMap { snapshot.sheet($0)?.name })
            item.blockRef = c.logicalRef
            item.channelValues = channelValues[c.id]
            item.units = unitsOf[c.id].map { units in units.sorted { $0.unit < $1.unit } }
            item.bus = c.bus.flatMap { busIndex[$0] }
            if c.isHarnessLabel { item.harness = c.harnessType }
            item.harnessOf = c.harnessOf.flatMap { byId[$0]?.ref }
            return item
        }
        let plannedIds = Set(planned.map(\.id))
        // Custom parts are addressed by datasheet pin number (names such as GND may repeat).
        func pinLabel(_ c: SnapComponent, _ pin: Int) -> String {
            if let part = snapshot.customPart(for: c), pin < part.symbol.pins.count { return part.symbol.pins[pin].number }
            return c.pins[pin].name
        }
        /// "REF.PIN" of a pin of a planned part, or of a unit of one (named by its package's designator).
        func label(_ address: PinAddress) -> String? {
            guard let c = byId[address.component], address.pin < c.pins.count, !onChannel(c) else { return nil }
            if let package = c.unitOf {
                guard plannedIds.contains(package) else { return nil }
            } else if !plannedIds.contains(c.id) {
                return nil
            }
            return "\(c.ref).\(pinLabel(c, address.pin))"
        }
        var connections: [PlannedConnection] = snapshot.wires.compactMap { wire in
            guard !junctions.contains(wire.a.component), !junctions.contains(wire.b.component),
                  let a = label(wire.a), let b = label(wire.b) else { return nil }
            return PlannedConnection(from: a, to: b)
        }
        // Plans connect pins directly: the pins joined through a cluster of junctions are chained pin to pin.
        var neighbours: [Int: [PinAddress]] = [:]
        for wire in snapshot.wires {
            if junctions.contains(wire.a.component) { neighbours[wire.a.component, default: []].append(wire.b) }
            if junctions.contains(wire.b.component) { neighbours[wire.b.component, default: []].append(wire.a) }
        }
        var visited = Set<Int>()
        for start in junctions.sorted() where !visited.contains(start) {
            var stack = [start], pins: [String] = []
            visited.insert(start)
            while let j = stack.popLast() {
                for n in neighbours[j] ?? [] {
                    if junctions.contains(n.component) {
                        if visited.insert(n.component).inserted { stack.append(n.component) }
                    } else if let text = label(n), !pins.contains(text) {
                        pins.append(text)
                    }
                }
            }
            for i in pins.indices.dropFirst() { connections.append(PlannedConnection(from: pins[i - 1], to: pins[i])) }
        }
        var noConnect: [String] = []
        for c in snapshot.components {
            for (index, pin) in c.pins.enumerated() where pin.noConnect {
                if let text = label(PinAddress(component: c.id, pin: index)), !noConnect.contains(text) { noConnect.append(text) }
            }
        }
        let sheets: [PlannedSheet] = multiSheet ? snapshot.sheets.map { s in
            var sheet = PlannedSheet(name: s.name, parent: s.parent == 0 ? nil : snapshot.sheet(s.parent)?.name)
            if s.isRepeated {
                sheet.channel = s.channel
                if s.isInstance {
                    sheet.instanceOf = snapshot.sheet(s.definitionId)?.name
                } else {
                    sheet.channels = s.channels ?? s.instances
                    sheet.refs = s.refs
                }
            }
            return sheet
        } : []
        let buses = plannedBuses.map { b in
            PlannedBus(name: b.name, sheet: multiSheet ? snapshot.sheet(b.sheet)?.name : nil,
                       points: b.points.map { PlannedPoint(x: $0.x, y: $0.y) })
        }
        let bottom = snapshot.board.bottomLayer
        var plan = DesignPlan(title: snapshot.name, summary: "", components: components, connections: connections,
                              notes: [], board: PlannedBoard(width: snapshot.board.width, height: snapshot.board.height,
                                                             layers: snapshot.board.layerCount, outline: "keep",
                                                             mountingHoleSpacing: -1),
                              industry: snapshot.industry,
                              pours: snapshot.zones.map {
                                  PlannedPour(net: $0.net, layer: $0.layer == bottom && bottom > 0 ? -1 : $0.layer, plane: $0.plane)
                              },
                              netClasses: snapshot.board.netWidths.keys.sorted().map {
                                  PlannedNetClass(net: $0, width: snapshot.board.netWidths[$0] ?? 0)
                              },
                              noConnect: noConnect,
                              robotPlatform: snapshot.robotPlatform.isEmpty ? nil : snapshot.robotPlatform,
                              ecuType: snapshot.ecuType.isEmpty ? nil : snapshot.ecuType,
                              aerospaceMission: snapshot.aerospaceMission.isEmpty ? nil : snapshot.aerospaceMission,
                              navalPlatform: snapshot.navalPlatform.isEmpty ? nil : snapshot.navalPlatform,
                              medicalClass: snapshot.medicalClass.isEmpty ? nil : snapshot.medicalClass,
                              retailDevice: snapshot.retailDevice.isEmpty ? nil : snapshot.retailDevice,
                              tamperMeshes: snapshot.tamperMeshes.map {
                                  PlannedTamperMesh(component: $0.component, netA: $0.netA, netB: $0.netB, margin: $0.margin)
                              },
                              applianceType: snapshot.applianceType.isEmpty ? nil : snapshot.applianceType,
                              memoryDesign: snapshot.memoryDesign.isEmpty ? nil : snapshot.memoryDesign,
                              sheets: sheets)
        plan.buses = buses.isEmpty ? nil : buses
        if !snapshot.harnessTypes.isEmpty {
            plan.harnessTypes = snapshot.harnessTypes.map { PlannedHarnessType(name: $0.name, entries: $0.entries) }
        }
        if !snapshot.netClassDefs.isEmpty {
            plan.netClassDefs = snapshot.netClassDefs.map {
                PlannedNetClassDef(name: $0.name, trackWidth: $0.trackWidth, clearance: $0.clearance)
            }
        }
        let directives = snapshot.directives.compactMap { d -> PlannedDirective? in
            guard let at = label(PinAddress(component: d.component, pin: d.pin)) else { return nil }
            return PlannedDirective(at: at, netClass: d.netClass, diffPair: d.diffPair, trackWidth: d.trackWidth,
                                    clearance: d.clearance)
        }
        plan.directives = directives.isEmpty ? nil : directives
        return plan
    }

    /// A refined plan from an agent keeps the structure of the plan it was made from: when the agent leaves out
    /// sheets, repeated-sheet settings, buses, bus entries, block designators, per-channel values or unit
    /// placements, they come back from `current` (matched by sheet name and reference designator). A new part with
    /// no sheet goes on the sheet of a part it connects to. Nothing the agent did set is overridden.
    static func preservingStructure(_ refined: DesignPlan, from current: DesignPlan) -> DesignPlan {
        var plan = refined
        if plan.sheets.isEmpty {
            plan.sheets = current.sheets
        } else {
            let old = Dictionary(current.sheets.map { ($0.name, $0) }, uniquingKeysWith: { a, _ in a })
            for i in plan.sheets.indices {
                guard let was = old[plan.sheets[i].name] else { continue }
                if plan.sheets[i].parent == nil { plan.sheets[i].parent = was.parent }
                if plan.sheets[i].channels == nil { plan.sheets[i].channels = was.channels }
                if plan.sheets[i].channel == nil { plan.sheets[i].channel = was.channel }
                if plan.sheets[i].refs == nil { plan.sheets[i].refs = was.refs }
                if plan.sheets[i].instanceOf == nil { plan.sheets[i].instanceOf = was.instanceOf }
            }
        }
        if plan.buses == nil { plan.buses = current.buses }
        if plan.harnessTypes == nil { plan.harnessTypes = current.harnessTypes }
        if plan.netClassDefs == nil { plan.netClassDefs = current.netClassDefs }
        if plan.directives == nil { plan.directives = current.directives }
        let busCount = plan.buses?.count ?? 0
        let old = Dictionary(current.components.map { ($0.ref, $0) }, uniquingKeysWith: { a, _ in a })
        let sheetNames = Set(plan.sheets.map(\.name))
        for i in plan.components.indices {
            guard let was = old[plan.components[i].ref] else { continue }
            var c = plan.components[i]
            if c.sheet == nil, let s = was.sheet, sheetNames.contains(s) { c.sheet = s }
            if c.kind == was.kind {
                if c.scope == nil { c.scope = was.scope }
                if c.targetSheet == nil, let t = was.targetSheet, sheetNames.contains(t) { c.targetSheet = t }
                if c.blockRef == nil { c.blockRef = was.blockRef }
                if c.channelValues == nil { c.channelValues = was.channelValues }
                if c.units == nil { c.units = was.units }
                if c.bus == nil, let b = was.bus, b < busCount { c.bus = b }
                if c.harness == nil { c.harness = was.harness }
                if c.harnessOf == nil { c.harnessOf = was.harnessOf }
            }
            plan.components[i] = c
        }
        // New parts without a sheet: the sheet of a part they connect to.
        if !plan.sheets.isEmpty {
            let sheetOf = Dictionary(plan.components.compactMap { c in c.sheet.map { (c.ref, $0) } },
                                     uniquingKeysWith: { a, _ in a })
            func ref(_ endpoint: String) -> String {
                guard let dot = endpoint.lastIndex(of: ".") else { return endpoint }
                return String(endpoint[..<dot]).trimmingCharacters(in: .whitespaces)
            }
            for i in plan.components.indices where plan.components[i].sheet == nil {
                let me = plan.components[i].ref
                for link in plan.connections {
                    let a = ref(link.from), b = ref(link.to)
                    let other = a == me ? b : b == me ? a : nil
                    if let other, let sheet = sheetOf[other] {
                        plan.components[i].sheet = sheet
                        break
                    }
                }
            }
        }
        return plan
    }
}

/// Keeps generated schematics readable when an agent stacks symbols on top of each other.
enum SchematicAutoLayout {
    static let minimumSpacing: CGFloat = 70

    static func resolveOverlaps(_ points: [CGPoint]) -> [CGPoint] {
        guard points.count > 1 else { return points.map(snap) }
        var hasOverlap = false
        outer: for i in 0..<points.count {
            for j in (i + 1)..<points.count where hypot(points[i].x - points[j].x, points[i].y - points[j].y) < minimumSpacing {
                hasOverlap = true
                break outer
            }
        }
        guard hasOverlap else { return points.map(snap) }
        // Fall back to a tidy grid in plan order (signal flow left → right, rows of five).
        let columns = 5
        return points.indices.map { index in
            CGPoint(x: CGFloat(index % columns) * 140, y: CGFloat(index / columns) * 140)
        }
    }

    static func snap(_ p: CGPoint) -> CGPoint {
        CGPoint(x: (p.x / 10).rounded() * 10, y: (p.y / 10).rounded() * 10)
    }
}

// MARK: - Lenient JSON extraction from model output

enum JSONExtraction {
    /// Returns the outermost JSON object in `text` (handles Markdown code fences and prose around it).
    static func objectString(in text: String) -> String? {
        guard let start = text.firstIndex(of: "{") else { return nil }
        var depth = 0
        var inString = false
        var escaped = false
        var index = start
        while index < text.endIndex {
            let ch = text[index]
            if inString {
                if escaped { escaped = false }
                else if ch == "\\" { escaped = true }
                else if ch == "\"" { inString = false }
            } else {
                if ch == "\"" { inString = true }
                else if ch == "{" { depth += 1 }
                else if ch == "}" {
                    depth -= 1
                    if depth == 0 { return String(text[start...index]) }
                }
            }
            index = text.index(after: index)
        }
        return nil
    }

    static func decode<T: Decodable>(_ type: T.Type, from text: String) throws -> T {
        guard let json = objectString(in: text), let data = json.data(using: .utf8) else {
            throw AIProviderError.invalidResponse("The model did not return a JSON object.")
        }
        do {
            return try JSONDecoder().decode(type, from: data)
        } catch {
            throw AIProviderError.invalidResponse("The model returned JSON that does not match the expected schema: \(error.localizedDescription)")
        }
    }
}

extension DesignPlanCompiler {
    /// The catalog part a model-written name means when it differs only in punctuation, case or an ordering suffix
    /// ("lm358-dr" → LM358DR, "NE555P" → NE555): same letters and digits, or one a prefix of the other (≥ 4
    /// characters), the closest in length. Nil when nothing is that close.
    static func closestStandardPart(_ name: String) -> StandardPart? {
        let key = SupplierPlacement.normalized(name)
        guard key.count >= 4 else { return nil }
        let candidates = StandardLibrary.parts.compactMap { part -> (StandardPart, Int)? in
            let n = SupplierPlacement.normalized(part.spec.name)
            guard n.count >= 4, n == key || key.hasPrefix(n) || n.hasPrefix(key) else { return nil }
            return (part, abs(n.count - key.count))
        }
        return candidates.min(by: { $0.1 < $1.1 })?.0
    }
}
