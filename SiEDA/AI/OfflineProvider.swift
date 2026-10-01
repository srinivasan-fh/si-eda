import Foundation

/// Deterministic, network-free designer. It answers the same structured requests as the LLM
/// providers using a library of reference circuits, so the full agent pipeline works offline,
/// in demos and in tests.
struct OfflineProvider: AIProvider {
    var displayName: String { "Offline Designer" }
    var modelName: String { "templates" }

    func complete(_ request: AIRequest) async throws -> String {
        let brief = Self.section("requirements", in: request.prompt) ?? request.prompt
        switch request.schemaName {
        case "requirements_spec":
            return Self.spec(for: brief).jsonString()
        case "design_plan":
            if let current = Self.section("current_plan", in: request.prompt),
               let plan = try? JSONExtraction.decode(DesignPlan.self, from: current) {
                return Self.refine(plan, instruction: Self.section("change_request", in: request.prompt) ?? "").jsonString()
            }
            return Self.template(for: brief).plan.jsonString()
        case "design_review":
            let planText = Self.section("current_plan", in: request.prompt) ?? "{}"
            let plan = (try? JSONExtraction.decode(DesignPlan.self, from: planText)) ?? Self.template(for: brief).plan
            let review = DesignReview(approved: true, issues: [], plan: plan)
            let data = try JSONEncoder().encode(review)
            return String(decoding: data, as: UTF8.self)
        default:
            throw AIProviderError.invalidResponse("Offline designer cannot answer '\(request.schemaName)'.")
        }
    }

    /// Extracts the text between <name> and </name>.
    static func section(_ name: String, in text: String) -> String? {
        guard let start = text.range(of: "<\(name)>"), let end = text.range(of: "</\(name)>", range: start.upperBound..<text.endIndex)
        else { return nil }
        return String(text[start.upperBound..<end.lowerBound]).trimmingCharacters(in: .whitespacesAndNewlines)
    }

    // MARK: - Templates

    struct Template {
        var keywords: [String]
        var plan: DesignPlan
        var blocks: [String]
        var supply: Double
    }

    static func template(for brief: String) -> Template {
        let text = brief.lowercased()
        // Most specific circuits first; the LED indicator (index 0) is the fallback.
        for t in templates.reversed() where t.keywords.contains(where: { text.contains($0) }) {
            return t
        }
        return templates[0]
    }

    static func spec(for brief: String) -> RequirementsSpec {
        let t = template(for: brief)
        let summary = brief.trimmingCharacters(in: .whitespacesAndNewlines)
        return RequirementsSpec(
            title: t.plan.title,
            summary: summary.isEmpty ? t.plan.summary : String(summary.prefix(400)),
            functionalRequirements: [t.plan.summary],
            electricalConstraints: ["Supply \(EngineeringFormat.string(t.supply, unit: "V"))", "All parts within ratings"],
            supplyVoltage: t.supply,
            boardWidthMM: t.plan.board.width,
            boardHeightMM: t.plan.board.height,
            designBlocks: t.blocks
        )
    }

    /// Small keyword-driven edits so "Refine" also works without a network model.
    static func refine(_ plan: DesignPlan, instruction: String) -> DesignPlan {
        var plan = plan
        let text = instruction.lowercased()
        for colour in ["red", "green", "blue", "yellow", "white"] where text.contains(colour) {
            for i in plan.components.indices where plan.components[i].kind == "led" {
                plan.components[i].value = colour.capitalized
            }
        }
        if let range = text.range(of: #"(\d+(\.\d+)?)\s*v\b"#, options: .regularExpression) {
            let volts = String(text[range]).filter { $0.isNumber || $0 == "." }
            for i in plan.components.indices where plan.components[i].kind == "voltage_source"
                && !plan.components[i].value.uppercased().hasPrefix("SIN") && !plan.components[i].value.uppercased().hasPrefix("PULSE") {
                plan.components[i].value = volts
            }
        }
        plan.notes.append("Offline designer applied keyword edits for: \(instruction.prefix(120))")
        return plan
    }

    static let templates: [Template] = [
        Template(
            keywords: ["led", "indicator", "light", "lamp"],
            plan: DesignPlan(
                title: "LED Indicator",
                summary: "5 V powered LED indicator with a series current-limiting resistor (~9 mA).",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "330", x: 120, y: -60),
                    PlannedComponent(ref: "D1", kind: "led", value: "Red", x: 240, y: -60),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 80),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 300, y: 20),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "GND2.GND"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                ],
                notes: ["R1 = (5 V − 2.0 V) / 9 mA ≈ 330 Ω", "Use a 0805 LED; reverse the part if it does not light."],
                board: PlannedBoard(width: 30, height: 20)),
            blocks: ["Power input", "Current limiter", "Indicator"],
            supply: 5),
        Template(
            keywords: ["divider", "reference", "bias"],
            plan: DesignPlan(
                title: "Voltage Divider",
                summary: "12 V to 6 V resistive divider with a labelled output net.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10k", x: 140, y: -60, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "10k", x: 140, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 220, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 120),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 140, y: 120),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "R2.1"),
                    PlannedConnection(from: "R2.1", to: "NL1.N"),
                    PlannedConnection(from: "R2.2", to: "GND2.GND"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                ],
                notes: ["VOUT = 12 V × R2 / (R1 + R2) = 6 V", "Load VOUT with ≥ 100 kΩ to keep the error below 5 %."],
                board: PlannedBoard(width: 30, height: 20)),
            blocks: ["Power input", "Divider"],
            supply: 12),
        Template(
            keywords: ["filter", "low-pass", "lowpass", "low pass", "rc ", "anti-alias"],
            plan: DesignPlan(
                title: "RC Low-Pass Filter",
                summary: "First-order RC low-pass filter, fc ≈ 1.6 kHz, driven by a 1 kHz test sine.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 1 1k)", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "1k", x: 120, y: -60),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 200, y: 0, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 260, y: -60),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 80),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 200, y: 80),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "C1.1"),
                    PlannedConnection(from: "R1.2", to: "NL1.N"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                ],
                notes: ["fc = 1 / (2π · 1 kΩ · 100 nF) ≈ 1.59 kHz", "Run a transient analysis (5 ms, 5 µs) to see the attenuation."],
                board: PlannedBoard(width: 30, height: 20)),
            blocks: ["Signal source", "Filter"],
            supply: 1),
        Template(
            keywords: ["amplif", "op-amp", "opamp", "op amp", "gain", "sensor"],
            plan: DesignPlan(
                title: "Non-Inverting Amplifier",
                summary: "Op-amp non-inverting amplifier with a gain of 11 (100 mV → 1.1 V peak).",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 0.1 1k)", x: 0, y: 40),
                    PlannedComponent(ref: "U1", kind: "opamp", value: "LM358", x: 180, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "100k", x: 180, y: -90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "10k", x: 80, y: -90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 280, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 120),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 30, y: -40),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "U1.IN+"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "U1.IN-", to: "R2.2"),
                    PlannedConnection(from: "R2.1", to: "GND2.GND"),
                    PlannedConnection(from: "U1.IN-", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "U1.OUT"),
                    PlannedConnection(from: "U1.OUT", to: "NL1.N"),
                ],
                notes: ["Gain = 1 + R1/R2 = 11", "Supply pins of the SOIC-8 package must be wired to ±rails on the real board."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Signal source", "Gain stage", "Output"],
            supply: 5),
        Template(
            keywords: ["transistor", "npn", "bjt", "driver", "relay", "buzzer"],
            plan: DesignPlan(
                title: "NPN LED Driver",
                summary: "Logic-controlled NPN low-side switch driving a 9 mA LED from 5 V.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VCC", x: 0, y: -80),
                    PlannedComponent(ref: "SW1", kind: "switch", value: "on", x: 100, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10k", x: 200, y: 0),
                    PlannedComponent(ref: "Q1", kind: "npn", value: "BC847", x: 300, y: 0),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "330", x: 320, y: -120, rotation: 90),
                    PlannedComponent(ref: "D1", kind: "led", value: "Green", x: 320, y: -50, rotation: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "VCC", x: 320, y: -170),
                    PlannedComponent(ref: "NL3", kind: "net_label", value: "VCC", x: 60, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 80),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 320, y: 60),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "NL1.N"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "NL3.N", to: "SW1.1"),
                    PlannedConnection(from: "SW1.2", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "Q1.B"),
                    PlannedConnection(from: "NL2.N", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "Q1.C"),
                    PlannedConnection(from: "Q1.E", to: "GND2.GND"),
                ],
                notes: ["Base current ≈ 0.43 mA keeps Q1 saturated (forced β ≈ 20).", "Replace SW1 with an MCU GPIO in production."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Power input", "Control input", "Driver", "Load"],
            supply: 5),
        Template(
            keywords: ["mosfet", "pwm", "load switch", "motor", "n-channel"],
            plan: DesignPlan(
                title: "MOSFET Low-Side PWM Switch",
                summary: "N-MOSFET low-side switch driven by a 100 Hz PWM signal into a 120 Ω load.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VIN", x: 0, y: -80),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "PULSE(0 5 10m 0.5)", x: 100, y: 60),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "100", x: 180, y: 0),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "100k", x: 230, y: 60, rotation: 90),
                    PlannedComponent(ref: "Q1", kind: "nmos", value: "2N7002", x: 300, y: 0),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "120", x: 320, y: -90, rotation: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "VIN", x: 320, y: -140),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 80),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 100, y: 140),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 320, y: 80),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "NL1.N"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "V2.+", to: "R1.1"),
                    PlannedConnection(from: "V2.-", to: "GND2.GND"),
                    PlannedConnection(from: "R1.2", to: "Q1.G"),
                    PlannedConnection(from: "Q1.G", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND3.GND"),
                    PlannedConnection(from: "NL2.N", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "Q1.D"),
                    PlannedConnection(from: "Q1.S", to: "GND3.GND"),
                ],
                notes: ["R2 keeps the gate low during reset.", "Load current ≈ 100 mA; check MOSFET dissipation in the DC report."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Power input", "PWM input", "Gate driver", "Load"],
            supply: 12),
    ]
}
