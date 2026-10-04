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

    init(title: String, summary: String, components: [PlannedComponent], connections: [PlannedConnection],
         notes: [String] = [], board: PlannedBoard = PlannedBoard(), industry: String? = nil,
         pours: [PlannedPour] = [], netClasses: [PlannedNetClass] = [], noConnect: [String] = [],
         robotPlatform: String? = nil, ecuType: String? = nil, aerospaceMission: String? = nil,
         navalPlatform: String? = nil, medicalClass: String? = nil, retailDevice: String? = nil,
         tamperMeshes: [PlannedTamperMesh] = [], applianceType: String? = nil) {
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
    }

    private enum CodingKeys: String, CodingKey {
        case title, summary, components, connections, notes, board, industry, pours, netClasses, noConnect, robotPlatform, ecuType, aerospaceMission, navalPlatform, medicalClass
        case retailDevice, tamperMeshes, applianceType
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

    init(ref: String, kind: String, value: String, x: Double, y: Double, rotation: Int = 0, firmware: String? = nil,
         pcb: PlannedPlacement? = nil) {
        self.ref = ref
        self.kind = kind
        self.value = value
        self.x = x
        self.y = y
        self.rotation = rotation
        self.firmware = firmware
        self.pcb = pcb
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
    }

    private enum CodingKeys: String, CodingKey { case ref, kind, value, x, y, rotation, firmware, pcb }
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
        var positions = plan.components.map { CGPoint(x: $0.x, y: $0.y) }
        positions = SchematicAutoLayout.resolveOverlaps(positions)

        var seenRefs = Set<String>()
        let library = previous?.customParts ?? []
        for (index, item) in plan.components.enumerated() {
            if item.kind.lowercased().hasPrefix("custom:") {
                let name = String(item.kind.dropFirst("custom:".count)).trimmingCharacters(in: .whitespaces)
                var partId = library.first(where: { $0.name.caseInsensitiveCompare(name) == .orderedSame })?.id
                if partId == nil,
                   let standard = StandardLibrary.parts.first(where: { $0.spec.name.caseInsensitiveCompare(name) == .orderedSame }) {
                    // Built-in standard part (LM7805, NE555, …): add it to the project library on first use.
                    partId = try? engine.registerCustomPart(standard.spec).id
                }
                guard let partId else {
                    report.warnings.append("Skipped \(item.ref): '\(name)' is not in the component library.")
                    continue
                }
                var ref = item.ref.trimmingCharacters(in: .whitespaces)
                if seenRefs.contains(ref) { ref = "" }
                let rotation = ((item.rotation % 360) + 360) % 360 / 90 * 90
                let id = engine.addCustomComponent(partId: partId, value: item.value.isEmpty ? nil : item.value,
                                                   at: positions[index], rotation: rotation, ref: ref.isEmpty ? nil : ref)
                if id >= 0 {
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
                report.componentsAdded += 1
                if !ref.isEmpty { seenRefs.insert(ref) }
            }
        }

        for connection in plan.connections {
            guard let a = resolve(connection.from, engine: engine, report: &report),
                  let b = resolve(connection.to, engine: engine, report: &report) else { continue }
            if a == b { continue }
            if engine.connect(a, b) != nil { report.connectionsMade += 1 }
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
            if let pin = resolve(endpoint, engine: engine, report: &report) { engine.setPinNoConnect(pin, true) }
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

    /// "R1.2" → (component id, pin index)
    static func resolve(_ endpoint: String, engine: EDAEngine, report: inout PlanApplyReport) -> PinAddress? {
        let trimmed = endpoint.trimmingCharacters(in: .whitespaces)
        guard let dot = trimmed.lastIndex(of: ".") else {
            report.warnings.append("Connection endpoint '\(endpoint)' is not in REF.PIN form.")
            return nil
        }
        let ref = String(trimmed[..<dot])
        let pinName = String(trimmed[trimmed.index(after: dot)...])
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

    /// Converts the current schematic back into a plan (context for refinement requests).
    static func plan(from snapshot: DesignSnapshot) -> DesignPlan {
        let byId = Dictionary(uniqueKeysWithValues: snapshot.components.map { ($0.id, $0) })
        let junctions = Set(snapshot.components.filter { $0.componentKind == .junction }.map(\.id))
        let components = snapshot.components.filter { !junctions.contains($0.id) }.map { c -> PlannedComponent in
            let kind = snapshot.customPart(for: c).map(\.planKind) ?? c.componentKind.planName
            return PlannedComponent(ref: c.ref, kind: kind, value: c.value, x: c.x, y: c.y, rotation: c.rotation)
        }
        // Custom parts are addressed by datasheet pin number (names such as GND may repeat).
        func pinLabel(_ c: SnapComponent, _ pin: Int) -> String {
            if let part = snapshot.customPart(for: c), pin < part.symbol.pins.count { return part.symbol.pins[pin].number }
            return c.pins[pin].name
        }
        func label(_ address: PinAddress) -> String? {
            guard let c = byId[address.component], address.pin < c.pins.count else { return nil }
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
            for (index, pin) in c.pins.enumerated() where pin.noConnect { noConnect.append("\(c.ref).\(pinLabel(c, index))") }
        }
        let bottom = snapshot.board.bottomLayer
        return DesignPlan(title: snapshot.name, summary: "", components: components, connections: connections,
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
                          applianceType: snapshot.applianceType.isEmpty ? nil : snapshot.applianceType)
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
