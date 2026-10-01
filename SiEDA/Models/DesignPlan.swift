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

    init(title: String, summary: String, components: [PlannedComponent], connections: [PlannedConnection],
         notes: [String] = [], board: PlannedBoard = PlannedBoard()) {
        self.title = title
        self.summary = summary
        self.components = components
        self.connections = connections
        self.notes = notes
        self.board = board
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        title = try c.decodeIfPresent(String.self, forKey: .title) ?? "Untitled design"
        summary = try c.decodeIfPresent(String.self, forKey: .summary) ?? ""
        components = try c.decodeIfPresent([PlannedComponent].self, forKey: .components) ?? []
        connections = try c.decodeIfPresent([PlannedConnection].self, forKey: .connections) ?? []
        notes = try c.decodeIfPresent([String].self, forKey: .notes) ?? []
        board = try c.decodeIfPresent(PlannedBoard.self, forKey: .board) ?? PlannedBoard()
    }

    private enum CodingKeys: String, CodingKey { case title, summary, components, connections, notes, board }

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

    init(ref: String, kind: String, value: String, x: Double, y: Double, rotation: Int = 0) {
        self.ref = ref
        self.kind = kind
        self.value = value
        self.x = x
        self.y = y
        self.rotation = rotation
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
    }

    private enum CodingKeys: String, CodingKey { case ref, kind, value, x, y, rotation }
}

struct PlannedConnection: Codable, Equatable {
    /// "REF.PIN", e.g. "R1.2", "U1.IN+", "Q1.B".
    var from: String
    var to: String
}

struct PlannedBoard: Codable, Equatable {
    var width: Double = 50
    var height: Double = 40
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

    /// `customKinds` are "custom:<NAME>" identifiers of parts in the project library.
    static func designPlanSchema(customKinds: [String]) -> [String: Any] {
        let kinds = kindNames + customKinds
        return [
            "type": "object",
            "additionalProperties": false,
            "required": ["title", "summary", "components", "connections", "notes", "board"],
            "properties": [
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
                    "required": ["width", "height"],
                    "properties": [
                        "width": ["type": "number", "description": "Board width in millimetres"],
                        "height": ["type": "number", "description": "Board height in millimetres"],
                    ],
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

        engine.clear()
        engine.setName(plan.title)
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
        if board.width >= 10, board.height >= 10, board.width <= 500, board.height <= 500 {
            engine.setBoard(width: board.width, height: board.height, trackWidth: 0, clearance: 0)
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
        let components = snapshot.components.map { c -> PlannedComponent in
            let kind = snapshot.customPart(for: c).map(\.planKind) ?? c.componentKind.planName
            return PlannedComponent(ref: c.ref, kind: kind, value: c.value, x: c.x, y: c.y, rotation: c.rotation)
        }
        // Custom parts are addressed by datasheet pin number (names such as GND may repeat).
        func pinLabel(_ c: SnapComponent, _ pin: Int) -> String {
            if let part = snapshot.customPart(for: c), pin < part.symbol.pins.count { return part.symbol.pins[pin].number }
            return c.pins[pin].name
        }
        let connections: [PlannedConnection] = snapshot.wires.compactMap { wire in
            guard let a = byId[wire.a.component], let b = byId[wire.b.component],
                  wire.a.pin < a.pins.count, wire.b.pin < b.pins.count else { return nil }
            return PlannedConnection(from: "\(a.ref).\(pinLabel(a, wire.a.pin))", to: "\(b.ref).\(pinLabel(b, wire.b.pin))")
        }
        return DesignPlan(title: snapshot.name, summary: "", components: components, connections: connections,
                          notes: [], board: PlannedBoard(width: snapshot.board.width, height: snapshot.board.height))
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
