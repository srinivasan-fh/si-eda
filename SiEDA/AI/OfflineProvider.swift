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
            return Self.template(for: brief).industryPlan.jsonString()
        case "component_definition":
            let text = Self.section("datasheet_text", in: request.prompt) ?? Self.section("ocr_text", in: request.prompt) ?? ""
            let hint = Self.section("package_hint", in: request.prompt) ?? ""
            let file = Self.section("file_name", in: request.prompt) ?? ""
            return PinTableParser.extractionJSON(text: text, fileName: file, hint: hint == "none" ? "" : hint)
        case "design_review":
            let planText = Self.section("current_plan", in: request.prompt) ?? "{}"
            let plan = (try? JSONExtraction.decode(DesignPlan.self, from: planText)) ?? Self.template(for: brief).industryPlan
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
        /// Basic-circuit library group ("Basic", "Power", "Analog", …) used by File ▸ New from Example.
        var category = "Basic"
        /// Phrases that rule this template out even if a keyword matches ("non-inverting" for the inverting amp).
        var excludes: [String] = []
        /// Industry profile applied with the design (derating, design rules, altitude class).
        var industry = "general"

        /// The plan with its industry profile set (what loading the example or the offline designer produces).
        var industryPlan: DesignPlan {
            var p = plan
            p.industry = industry
            return p
        }

        func matches(_ text: String) -> Bool {
            keywords.contains(where: { text.contains($0) }) && !excludes.contains(where: { text.contains($0) })
        }
    }

    static func template(for brief: String) -> Template {
        let text = " " + brief.lowercased() + " "  // padded so word keywords like " rf " match at the ends
        // Later (more specific) circuits win; the LED indicator (index 0) is the fallback.
        for t in templates.reversed() where t.matches(text) {
            return t
        }
        return templates[0]
    }

    /// Library groups in display order.
    static let categories = ["Basic", "Power", "Analog", "Filters", "Drivers", "Timers", "Sensors",
                             "Robotics", "Power Electronics", "Automotive", "RF", "Space", "Marine", "Industrial"]

    static func examples(in category: String) -> [Template] { templates.filter { $0.category == category } }

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
            supply: 1,
            category: "Filters"),
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
            supply: 5,
            category: "Analog"),
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
            supply: 5,
            category: "Drivers"),
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
                    PlannedComponent(ref: "R3", kind: "resistor", value: "120 2W", x: 320, y: -90, rotation: 90),
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
            supply: 12,
            category: "Drivers"),
        Template(
            keywords: ["regulator", "7805", "lm7805", "linear reg", "ldo"],
            plan: DesignPlan(
                title: "5 V Linear Regulator",
                summary: "LM7805 regulator: 12 V in, 5 V out with input/output capacitors and a power-good LED.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
                    PlannedComponent(ref: "U1", kind: "custom:LM7805", value: "LM7805", x: 200, y: -60),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "330n", x: 100, y: 20, rotation: 90),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "100n", x: 300, y: 20, rotation: 90),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "1k", x: 380, y: -60),
                    PlannedComponent(ref: "D1", kind: "led", value: "Green", x: 460, y: 0, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "+5V", x: 300, y: -120),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 200, y: 100),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 460, y: 100),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "U1.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "U1.1", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.3", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.3", to: "NL1.N"),
                    PlannedConnection(from: "U1.3", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "GND3.GND"),
                ],
                notes: ["C1 330 nF and C2 100 nF are the LM7805 datasheet values; keep them within 10 mm of U1.",
                        "Dropout ≈ 2 V: the input must stay above 7 V.",
                        "U1 dissipates (12 V − 5 V) × I_load — add a heatsink above ≈ 250 mA.",
                        "U1 has no simulation model; the DC analysis covers the source and the LED branch only."],
                board: PlannedBoard(width: 40, height: 25)),
            blocks: ["Power input", "Linear regulator", "Power-good indicator"],
            supply: 12,
            category: "Power"),
        Template(
            keywords: ["555", "astable", "blink", "flasher", "flashing", "timer", "oscillator"],
            plan: DesignPlan(
                title: "555 Astable LED Blinker",
                summary: "NE555 astable oscillator (≈ 1.5 Hz) blinking an LED from 5 V.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VCC", x: 0, y: -80),
                    PlannedComponent(ref: "U1", kind: "custom:NE555", value: "NE555", x: 260, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "1k", x: 120, y: -120, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "47k", x: 120, y: -20, rotation: 90),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "10u", x: 120, y: 80, rotation: 90),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "10n", x: 200, y: 120, rotation: 90),
                    PlannedComponent(ref: "C3", kind: "capacitor", value: "100n", x: 360, y: -120, rotation: 90),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "330", x: 400, y: 0),
                    PlannedComponent(ref: "D1", kind: "led", value: "Red", x: 480, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "VCC", x: 260, y: -160),
                    PlannedComponent(ref: "NL3", kind: "net_label", value: "VCC", x: 120, y: -200),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 200, y: 200),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 480, y: 160),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "NL1.N"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "NL2.N", to: "U1.8"),
                    PlannedConnection(from: "U1.8", to: "U1.4"),
                    PlannedConnection(from: "U1.8", to: "C3.1"),
                    PlannedConnection(from: "C3.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.1", to: "GND2.GND"),
                    PlannedConnection(from: "NL3.N", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "U1.7"),
                    PlannedConnection(from: "U1.7", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "U1.6"),
                    PlannedConnection(from: "U1.6", to: "U1.2"),
                    PlannedConnection(from: "U1.6", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.5", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.3", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "GND3.GND"),
                ],
                notes: ["f = 1.44 / ((R1 + 2·R2) · C1) = 1.44 / (95 kΩ · 10 µF) ≈ 1.5 Hz, duty ≈ 51 %",
                        "C2 decouples the CONT pin; C3 decouples VCC — place both next to U1.",
                        "U1 has no simulation model; verify timing on the bench."],
                board: PlannedBoard(width: 40, height: 30)),
            blocks: ["Power input", "Astable timer", "LED output"],
            supply: 5,
            category: "Timers"),
        Template(
            keywords: ["rectifier", "half-wave", "half wave", "ac to dc", "ac-dc", "ac/dc"],
            plan: DesignPlan(
                title: "Half-Wave Rectifier",
                summary: "Diode half-wave rectifier with a reservoir capacitor turning a 10 V, 50 Hz sine into DC.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 10 50)", x: 0, y: 0),
                    PlannedComponent(ref: "D1", kind: "diode", value: "1N4148", x: 120, y: -60),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100u", x: 220, y: 0, rotation: 90),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10k", x: 300, y: 0, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VDC", x: 300, y: -100),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 260, y: 100),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "D1.A"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "D1.K", to: "C1.1"),
                    PlannedConnection(from: "D1.K", to: "R1.1"),
                    PlannedConnection(from: "D1.K", to: "NL1.N"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "R1.2", to: "GND2.GND"),
                ],
                notes: ["VDC ≈ 10 V − 0.7 V ≈ 9.3 V peak", "Ripple ≈ I / (f · C) = 0.93 mA / (50 Hz · 100 µF) ≈ 0.19 V",
                        "Run a transient analysis (100 ms, 50 µs) to see the ripple."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["AC source", "Rectifier", "Reservoir", "Load"],
            supply: 10,
            category: "Power"),
        Template(
            keywords: ["inverting amplifier", "inverting amp", "inverting op", "inverting gain", "inverter amp"],
            plan: DesignPlan(
                title: "Inverting Amplifier",
                summary: "Op-amp inverting amplifier with a gain of −10 (100 mV → −1 V peak).",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 0.1 1k)", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10k", x: 100, y: -40),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "100k", x: 200, y: -120),
                    PlannedComponent(ref: "U1", kind: "opamp", value: "LM358", x: 220, y: -20),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 320, y: -20),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 160, y: 80),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "R1.2", to: "U1.IN-"),
                    PlannedConnection(from: "U1.IN-", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "U1.OUT"),
                    PlannedConnection(from: "U1.IN+", to: "GND2.GND"),
                    PlannedConnection(from: "U1.OUT", to: "NL1.N"),
                ],
                notes: ["Gain = −R2/R1 = −10; input impedance = R1 = 10 kΩ",
                        "Supply pins of the SOIC-8 package must be wired to ±rails on the real board."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Signal source", "Inverting gain stage", "Output"],
            supply: 5,
            category: "Analog",
            excludes: ["non-inverting", "noninverting", "non inverting"]),
        Template(
            keywords: ["high-pass", "highpass", "high pass", "ac coupling", "ac-coupling", "dc block"],
            plan: DesignPlan(
                title: "RC High-Pass Filter",
                summary: "First-order RC high-pass filter, fc ≈ 1.6 kHz, driven by a 1 kHz test sine.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 1 1k)", x: 0, y: 0),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 120, y: -60),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "1k", x: 200, y: 0, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 260, y: -60),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 80),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 200, y: 80),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "R1.1"),
                    PlannedConnection(from: "C1.2", to: "NL1.N"),
                    PlannedConnection(from: "R1.2", to: "GND2.GND"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                ],
                notes: ["fc = 1 / (2π · 1 kΩ · 100 nF) ≈ 1.59 kHz", "Below fc the output falls at 20 dB/decade."],
                board: PlannedBoard(width: 30, height: 20)),
            blocks: ["Signal source", "Filter"],
            supply: 1,
            category: "Filters"),
        Template(
            keywords: ["follower", "buffer", "unity gain", "unity-gain"],
            plan: DesignPlan(
                title: "Voltage Follower",
                summary: "Unity-gain op-amp buffer driving a 1 kΩ load from a high-impedance 2.5 V divider.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "100k", x: 100, y: -60, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "100k", x: 100, y: 60, rotation: 90),
                    PlannedComponent(ref: "U1", kind: "opamp", value: "LM358", x: 220, y: 0),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "1k", x: 320, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 320, y: -40),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 120),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 100, y: 140),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 320, y: 140),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "R1.2", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND2.GND"),
                    PlannedConnection(from: "R1.2", to: "U1.IN+"),
                    PlannedConnection(from: "U1.OUT", to: "U1.IN-"),
                    PlannedConnection(from: "U1.OUT", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.OUT", to: "NL1.N"),
                ],
                notes: ["VOUT = V(IN+) = 2.5 V; the divider sees only the op-amp input, so loading does not pull it down.",
                        "Without U1 the 1 kΩ load would drop the divider output to ≈ 0.1 V."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Power input", "Reference divider", "Buffer", "Load"],
            supply: 5,
            category: "Analog"),
        Template(
            keywords: ["wheatstone", "bridge", "strain gauge", "strain-gauge", "load cell", "rtd"],
            plan: DesignPlan(
                title: "Wheatstone Bridge",
                summary: "Resistive Wheatstone bridge from 5 V with one 2 % unbalanced arm (≈ 25 mV differential output).",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "1k", x: 120, y: -60, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "1k", x: 120, y: 60, rotation: 90),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "1k", x: 260, y: -60, rotation: 90),
                    PlannedComponent(ref: "R4", kind: "resistor", value: "1.02k", x: 260, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VA", x: 180, y: 0),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "VB", x: 320, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 120),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 190, y: 140),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "V1.+", to: "R3.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "R1.2", to: "R2.1"),
                    PlannedConnection(from: "R3.2", to: "R4.1"),
                    PlannedConnection(from: "R1.2", to: "NL1.N"),
                    PlannedConnection(from: "R3.2", to: "NL2.N"),
                    PlannedConnection(from: "R2.2", to: "GND2.GND"),
                    PlannedConnection(from: "R4.2", to: "GND2.GND"),
                ],
                notes: ["VA = 2.5 V, VB = 5 V × 1.02k / 2.02k ≈ 2.525 V → VB − VA ≈ 25 mV",
                        "Replace R4 with the sensor (strain gauge, RTD); feed VA/VB to an instrumentation amplifier."],
                board: PlannedBoard(width: 35, height: 25)),
            blocks: ["Excitation", "Bridge", "Differential output"],
            supply: 5,
            category: "Sensors",
            excludes: ["h-bridge", "h bridge", "full bridge", "bridge rectifier"]),
        Template(
            keywords: ["motor driver", "motor drive", "dc motor", "robot", "actuator", "h-bridge", "h bridge"],
            plan: DesignPlan(
                title: "DC Motor PWM Drive",
                summary: "Low-side N-MOSFET PWM drive (20 kHz) for a 12 V brushed DC motor with flyback diode, bulk capacitor and current shunt.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VM", x: 0, y: -90),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100u", x: 80, y: 40, rotation: 90),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "120 2W", x: 220, y: -120, rotation: 90),
                    PlannedComponent(ref: "L1", kind: "inductor", value: "10m", x: 220, y: -40, rotation: 90),
                    PlannedComponent(ref: "D1", kind: "diode", value: "1N4148", x: 300, y: -80, rotation: 270),
                    PlannedComponent(ref: "Q1", kind: "nmos", value: "2N7002", x: 220, y: 60),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "PULSE(0 5 50u 0.5)", x: 0, y: 160),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "100", x: 100, y: 100),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "100k", x: 160, y: 160, rotation: 90),
                    PlannedComponent(ref: "R4", kind: "resistor", value: "1", x: 240, y: 160, rotation: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "ISENSE", x: 320, y: 120),
                    PlannedComponent(ref: "NL3", kind: "net_label", value: "VM", x: 220, y: -200),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 80, y: 120),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 0, y: 240),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 200, y: 240),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "NL1.N"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "C1.1", to: "NL1.N"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "NL3.N", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "L1.1"),
                    PlannedConnection(from: "L1.2", to: "Q1.D"),
                    PlannedConnection(from: "D1.A", to: "Q1.D"),
                    PlannedConnection(from: "D1.K", to: "R3.1"),
                    PlannedConnection(from: "V2.+", to: "R1.1"),
                    PlannedConnection(from: "V2.-", to: "GND3.GND"),
                    PlannedConnection(from: "R1.2", to: "Q1.G"),
                    PlannedConnection(from: "Q1.G", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND4.GND"),
                    PlannedConnection(from: "Q1.S", to: "R4.1"),
                    PlannedConnection(from: "R4.2", to: "GND4.GND"),
                    PlannedConnection(from: "Q1.S", to: "NL2.N"),
                ],
                notes: ["R3 + L1 model the motor winding (120 Ω, 10 mH); replace them with a 2-pin motor connector on the real board.",
                        "Run a transient analysis (2 ms, 1 µs) to see the PWM current ramp in L1 and the flyback through D1.",
                        "ISENSE = I_motor × 1 Ω: feed it to an ADC or comparator for stall/over-current protection (or use an ACS712).",
                        "For motors above ~100 mA use a power MOSFET such as the IRF540N (standard library) and a gate driver."],
                board: PlannedBoard(width: 45, height: 30)),
            blocks: ["Motor supply", "PWM input", "Gate drive", "Power switch", "Motor + flyback", "Current sense"],
            supply: 12,
            category: "Robotics",
            industry: "robotics"),
        Template(
            keywords: ["boost", "step-up", "step up", "dc-dc", "dc/dc", "smps", "switching regulator", "converter"],
            plan: DesignPlan(
                title: "Boost Converter 5 V → 12 V",
                summary: "Open-loop boost converter: 100 kHz PWM, 58 % duty, 100 µH inductor, AO3400 switch, SS14 Schottky, 180 Ω (66 mA) load in continuous conduction.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "10u", x: 60, y: 60, rotation: 90),
                    PlannedComponent(ref: "L1", kind: "inductor", value: "100u", x: 140, y: -60),
                    PlannedComponent(ref: "Q1", kind: "nmos", value: "AO3400", x: 220, y: 40),
                    PlannedComponent(ref: "D1", kind: "diode", value: "SS14", x: 280, y: -60),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "22u", x: 340, y: 20, rotation: 90),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "180 2W", x: 420, y: 20, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "VOUT", x: 420, y: -80),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "PULSE(0 5 10u 0.58)", x: 100, y: 160),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10", x: 160, y: 100),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "100k", x: 200, y: 160, rotation: 90),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 60, y: 130),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 220, y: 130),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 380, y: 110),
                    PlannedComponent(ref: "GND5", kind: "ground", value: "0", x: 100, y: 240),
                    PlannedComponent(ref: "GND6", kind: "ground", value: "0", x: 200, y: 240),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "L1.1"),
                    PlannedConnection(from: "V1.+", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND2.GND"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "L1.2", to: "Q1.D"),
                    PlannedConnection(from: "L1.2", to: "D1.A"),
                    PlannedConnection(from: "Q1.S", to: "GND3.GND"),
                    PlannedConnection(from: "D1.K", to: "C1.1"),
                    PlannedConnection(from: "D1.K", to: "R3.1"),
                    PlannedConnection(from: "D1.K", to: "NL1.N"),
                    PlannedConnection(from: "C1.2", to: "GND4.GND"),
                    PlannedConnection(from: "R3.2", to: "GND4.GND"),
                    PlannedConnection(from: "V2.+", to: "R1.1"),
                    PlannedConnection(from: "V2.-", to: "GND5.GND"),
                    PlannedConnection(from: "R1.2", to: "Q1.G"),
                    PlannedConnection(from: "Q1.G", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND6.GND"),
                ],
                notes: ["VOUT = VIN / (1 − D) = 5 V / 0.42 ≈ 12 V in continuous conduction (load above ~60 mA); lighter loads push an open-loop boost higher. Run a transient analysis (3 ms, 0.5 µs) to watch it settle.",
                        "Q1 AO3400 (logic-level) and D1 SS14 Schottky are sized for the switching stress; use a shielded inductor and close the loop with a UC3843 for regulation.",
                        "Keep the loop L1 → Q1 → GND and D1 → C1 → GND as small as possible."],
                board: PlannedBoard(width: 40, height: 30)),
            blocks: ["Input", "Inductor", "Switch", "Rectifier", "Output filter", "PWM"],
            supply: 5,
            category: "Power Electronics",
            industry: "power"),
        Template(
            keywords: ["half-bridge", "half bridge", "gate driver", "inverter", "ir2104", "bldc"],
            plan: DesignPlan(
                title: "Half-Bridge Gate Driver",
                summary: "IR2104 bootstrap half-bridge driving two IRF540N MOSFETs from a 24 V bus (motor phase / inverter leg).",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "+12V", x: 0, y: -80),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "24", x: 0, y: 200),
                    PlannedComponent(ref: "U1", kind: "custom:IR2104", value: "IR2104", x: 200, y: 0),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "1u", x: 120, y: 80, rotation: 90),
                    PlannedComponent(ref: "D1", kind: "diode", value: "1N4148", x: 120, y: -120),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 320, y: -120, rotation: 90),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "10", x: 320, y: -40),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "10", x: 320, y: 60),
                    PlannedComponent(ref: "Q1", kind: "custom:IRF540N", value: "IRF540N", x: 440, y: -40),
                    PlannedComponent(ref: "Q2", kind: "custom:IRF540N", value: "IRF540N", x: 440, y: 80),
                    PlannedComponent(ref: "C3", kind: "capacitor", value: "100u", x: 540, y: -120, rotation: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "PHASE", x: 540, y: 20),
                    PlannedComponent(ref: "NL3", kind: "net_label", value: "+12V", x: 120, y: -200),
                    PlannedComponent(ref: "J1", kind: "custom:Header_1x04", value: "Header_1x04", x: 60, y: 120),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 0, y: 280),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 120, y: 160),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 440, y: 180),
                    PlannedComponent(ref: "GND5", kind: "ground", value: "0", x: 540, y: -40),
                    PlannedComponent(ref: "GND6", kind: "ground", value: "0", x: 200, y: 120),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "NL1.N"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "V2.-", to: "GND2.GND"),
                    PlannedConnection(from: "NL1.N", to: "U1.1"),
                    PlannedConnection(from: "U1.1", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.4", to: "GND6.GND"),
                    PlannedConnection(from: "NL3.N", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "U1.8"),
                    PlannedConnection(from: "U1.8", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "U1.6"),
                    PlannedConnection(from: "U1.7", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "Q1.1"),
                    PlannedConnection(from: "U1.5", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "Q2.1"),
                    PlannedConnection(from: "V2.+", to: "Q1.2"),
                    PlannedConnection(from: "Q1.2", to: "C3.1"),
                    PlannedConnection(from: "C3.2", to: "GND5.GND"),
                    PlannedConnection(from: "Q1.3", to: "U1.6"),
                    PlannedConnection(from: "Q2.2", to: "U1.6"),
                    PlannedConnection(from: "Q2.3", to: "GND4.GND"),
                    PlannedConnection(from: "U1.6", to: "NL2.N"),
                    PlannedConnection(from: "J1.1", to: "U1.2"),
                    PlannedConnection(from: "J1.2", to: "U1.3"),
                    PlannedConnection(from: "J1.3", to: "U1.1"),
                    PlannedConnection(from: "J1.4", to: "GND6.GND"),
                ],
                notes: ["D1 + C1 form the bootstrap supply for the high-side gate (VB − VS ≈ 11.3 V); use a fast diode (UF4007/BAT46) on the board.",
                        "IR2104 inserts ~520 ns dead time; SD (J1.2) high enables the outputs.",
                        "Put C3 (bulk) and a 100 nF ceramic directly across Q1 drain and Q2 source.",
                        "The IR2104 and IRF540N have no simulation model — verify switching on the bench."],
                board: PlannedBoard(width: 50, height: 35)),
            blocks: ["Logic supply", "Gate driver", "Bootstrap", "High-side switch", "Low-side switch", "DC bus"],
            supply: 24,
            category: "Power Electronics",
            industry: "power"),
        Template(
            keywords: ["automotive", "vehicle", "car ", "car electronics", "ecu", "can bus", "can node", "12 v battery", "load dump"],
            plan: DesignPlan(
                title: "Automotive 12 V Input + CAN Node",
                summary: "Battery input with fuse, reverse-polarity diode and TVS, LM2940 5 V LDO and a TJA1050 CAN transceiver with 120 Ω termination.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "13.5", x: 0, y: 0),
                    PlannedComponent(ref: "F1", kind: "fuse", value: "1", x: 80, y: -60),
                    PlannedComponent(ref: "D1", kind: "diode", value: "1N4148", x: 160, y: -60),
                    PlannedComponent(ref: "D2", kind: "diode", value: "SMBJ24A", x: 220, y: 20, rotation: 270),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "470n", x: 280, y: 20, rotation: 90),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "2.2k", x: 340, y: -120),
                    PlannedComponent(ref: "D3", kind: "led", value: "Green", x: 420, y: -80, rotation: 90),
                    PlannedComponent(ref: "U1", kind: "custom:LM2940-5.0", value: "LM2940-5.0", x: 380, y: -20),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "22u", x: 480, y: 40, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "+5V", x: 480, y: -60),
                    PlannedComponent(ref: "U2", kind: "custom:TJA1050", value: "TJA1050", x: 640, y: 0),
                    PlannedComponent(ref: "C3", kind: "capacitor", value: "100n", x: 560, y: 80, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "120", x: 760, y: -40, rotation: 90),
                    PlannedComponent(ref: "J1", kind: "custom:Header_1x04", value: "Header_1x04", x: 640, y: 160),
                    PlannedComponent(ref: "J2", kind: "connector", value: "CAN", x: 840, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 250, y: 110),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 420, y: 0),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 480, y: 130),
                    PlannedComponent(ref: "GND5", kind: "ground", value: "0", x: 600, y: 160),
                    PlannedComponent(ref: "GND6", kind: "ground", value: "0", x: 380, y: 60),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "F1.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "F1.2", to: "D1.A"),
                    PlannedConnection(from: "D1.K", to: "U1.1"),
                    PlannedConnection(from: "D1.K", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "D1.K", to: "D2.K"),
                    PlannedConnection(from: "D2.A", to: "GND2.GND"),
                    PlannedConnection(from: "D1.K", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "D3.A"),
                    PlannedConnection(from: "D3.K", to: "GND3.GND"),
                    PlannedConnection(from: "U1.2", to: "GND6.GND"),
                    PlannedConnection(from: "U1.3", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND4.GND"),
                    PlannedConnection(from: "U1.3", to: "NL1.N"),
                    PlannedConnection(from: "U1.3", to: "U2.3"),
                    PlannedConnection(from: "U2.3", to: "C3.1"),
                    PlannedConnection(from: "C3.2", to: "GND5.GND"),
                    PlannedConnection(from: "U2.2", to: "GND5.GND"),
                    PlannedConnection(from: "U2.8", to: "GND5.GND"),
                    PlannedConnection(from: "U2.1", to: "J1.1"),
                    PlannedConnection(from: "U2.4", to: "J1.2"),
                    PlannedConnection(from: "U1.3", to: "J1.3"),
                    PlannedConnection(from: "J1.4", to: "GND5.GND"),
                    PlannedConnection(from: "U2.7", to: "R2.1"),
                    PlannedConnection(from: "U2.6", to: "R2.2"),
                    PlannedConnection(from: "U2.7", to: "J2.1"),
                    PlannedConnection(from: "U2.6", to: "J2.2"),
                ],
                notes: ["D1 blocks reverse battery; D2 (SMBJ24A TVS) clamps ISO 7637-2 transients; LM2940 survives 45 V load dump.",
                        "C2 must be ≥ 22 µF with low ESR for LM2940 stability.",
                        "R2 terminates one end of the CAN bus; fit it only on the two end nodes. Route CANH/CANL as a 120 Ω differential pair.",
                        "Select AEC-Q100/Q200 qualified parts rated −40 °C to +125 °C."],
                board: PlannedBoard(width: 55, height: 35)),
            blocks: ["Battery input", "Protection", "5 V LDO", "CAN transceiver", "Bus termination"],
            supply: 13.5,
            category: "Automotive",
            industry: "automotive"),
        Template(
            keywords: [" rf ", "rf:", "rf filter", "rf front", "radio", "433", "ism band", "lc filter", "harmonic filter", "antenna", "matching network", "wireless"],
            plan: DesignPlan(
                title: "433 MHz LC Harmonic Filter",
                summary: "50 Ω 5th-order Butterworth LC low-pass (fc ≈ 500 MHz) that passes 433 MHz and attenuates its harmonics.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "SIN(0 1 433meg)", x: 0, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "49.9", x: 80, y: -60),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "3.9p", x: 160, y: 20, rotation: 90),
                    PlannedComponent(ref: "L1", kind: "inductor", value: "27n", x: 220, y: -60),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "12p", x: 280, y: 20, rotation: 90),
                    PlannedComponent(ref: "L2", kind: "inductor", value: "27n", x: 340, y: -60),
                    PlannedComponent(ref: "C3", kind: "capacitor", value: "3.9p", x: 400, y: 20, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "49.9", x: 460, y: 20, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "RF_OUT", x: 460, y: -100),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 280, y: 100),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "R1.1"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "R1.2", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "R1.2", to: "L1.1"),
                    PlannedConnection(from: "L1.2", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND2.GND"),
                    PlannedConnection(from: "L1.2", to: "L2.1"),
                    PlannedConnection(from: "L2.2", to: "C3.1"),
                    PlannedConnection(from: "C3.2", to: "GND2.GND"),
                    PlannedConnection(from: "L2.2", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND2.GND"),
                    PlannedConnection(from: "L2.2", to: "NL1.N"),
                ],
                notes: ["Butterworth g = 0.618, 1.618, 2.0, 1.618, 0.618 at 50 Ω, fc = 500 MHz → C1/C3 3.9 pF, L1/L2 27 nH, C2 12 pF.",
                        "Use C0G/NP0 capacitors and high-Q multilayer inductors; route RF_IN → RF_OUT as 50 Ω microstrip over solid ground.",
                        "R1 is the 50 Ω source impedance and R2 the 50 Ω load (antenna) — they are not fitted on the real board.",
                        "Transient check: 20 ns with a 10 ps step."],
                board: PlannedBoard(width: 40, height: 22)),
            blocks: ["Source (50 Ω)", "LC ladder", "Load (50 Ω)"],
            supply: 1,
            category: "RF",
            industry: "rf"),
        Template(
            keywords: ["space", "satellite", "spacecraft", "cubesat", "payload", "launcher", "redundant supply", "or-ing", "oring"],
            plan: DesignPlan(
                title: "Redundant 28 V Bus OR-ing + 5 V Regulator",
                summary: "Spacecraft-style cold-redundant 28 V buses diode OR-ed into an LM317 5 V regulator, derated to ECSS-Q-ST-30-11.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "28", x: 0, y: 0),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "28", x: 0, y: 160),
                    PlannedComponent(ref: "D1", kind: "diode", value: "1N4148", x: 100, y: -40),
                    PlannedComponent(ref: "D2", kind: "diode", value: "1N4148", x: 100, y: 120),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 200, y: 60, rotation: 90),
                    PlannedComponent(ref: "U1", kind: "custom:LM317", value: "LM317", x: 280, y: 0),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "240", x: 380, y: -60, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "715", x: 380, y: 60, rotation: 90),
                    PlannedComponent(ref: "C2", kind: "capacitor", value: "1u", x: 460, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "+5V", x: 460, y: -80),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "1k", x: 540, y: -40),
                    PlannedComponent(ref: "D3", kind: "led", value: "Green", x: 620, y: 20, rotation: 90),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 0, y: 250),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 300, y: 140),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 620, y: 100),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "D1.A"),
                    PlannedConnection(from: "V2.+", to: "D2.A"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "V2.-", to: "GND2.GND"),
                    PlannedConnection(from: "D1.K", to: "U1.3"),
                    PlannedConnection(from: "D2.K", to: "U1.3"),
                    PlannedConnection(from: "U1.3", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.2", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "U1.1"),
                    PlannedConnection(from: "U1.1", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.2", to: "C2.1"),
                    PlannedConnection(from: "C2.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.2", to: "NL1.N"),
                    PlannedConnection(from: "U1.2", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "D3.A"),
                    PlannedConnection(from: "D3.K", to: "GND4.GND"),
                ],
                notes: ["VOUT = 1.25 V × (1 + 715/240) ≈ 4.97 V.",
                        "Each bus feeds through its own OR-ing diode, so a shorted bus cannot pull the other down; add a latching current limiter (LCL) per bus.",
                        "Space profile: parts derated to 50 % power / 50 % current and IPC-2221 B3 (vacuum) spacing is checked.",
                        "U1 dissipates (28 V − 5 V) × I_load — use a rad-tolerant LM117/RH117 and a heatsink path to the chassis."],
                board: PlannedBoard(width: 45, height: 30)),
            blocks: ["Bus A", "Bus B", "OR-ing diodes", "Regulator", "Status LED"],
            supply: 28,
            category: "Space",
            industry: "space"),
        Template(
            keywords: ["marine", "ship", "boat", "nmea", "rs-485", "rs485", "vessel", "maritime"],
            plan: DesignPlan(
                title: "NMEA / RS-485 Interface",
                summary: "MAX485 RS-485 transceiver for NMEA 0183/2000-style ship buses with 120 Ω termination and fail-safe bias.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
                    PlannedComponent(ref: "U1", kind: "custom:MAX485", value: "MAX485", x: 200, y: 0),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 100, y: 80, rotation: 90),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "120", x: 360, y: 0, rotation: 90),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "680", x: 360, y: -120, rotation: 90),
                    PlannedComponent(ref: "R3", kind: "resistor", value: "680", x: 440, y: 100, rotation: 90),
                    PlannedComponent(ref: "J1", kind: "custom:Header_1x04", value: "Header_1x04", x: 60, y: 160),
                    PlannedComponent(ref: "J2", kind: "connector", value: "NMEA", x: 520, y: 0),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 100, y: 160),
                    PlannedComponent(ref: "GND3", kind: "ground", value: "0", x: 440, y: 180),
                    PlannedComponent(ref: "GND4", kind: "ground", value: "0", x: 200, y: 240),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "U1.8"),
                    PlannedConnection(from: "V1.-", to: "GND1.GND"),
                    PlannedConnection(from: "U1.8", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.5", to: "GND2.GND"),
                    PlannedConnection(from: "U1.2", to: "U1.3"),
                    PlannedConnection(from: "U1.1", to: "J1.1"),
                    PlannedConnection(from: "U1.4", to: "J1.2"),
                    PlannedConnection(from: "U1.3", to: "J1.3"),
                    PlannedConnection(from: "J1.4", to: "GND4.GND"),
                    PlannedConnection(from: "U1.6", to: "R1.1"),
                    PlannedConnection(from: "U1.7", to: "R1.2"),
                    PlannedConnection(from: "U1.8", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "U1.6"),
                    PlannedConnection(from: "U1.7", to: "R3.1"),
                    PlannedConnection(from: "R3.2", to: "GND3.GND"),
                    PlannedConnection(from: "U1.6", to: "J2.1"),
                    PlannedConnection(from: "U1.7", to: "J2.2"),
                ],
                notes: ["R2/R3 (680 Ω) bias the idle bus so A > B (fail-safe); R1 terminates the bus end (120 Ω).",
                        "J1: RO, DI, DIR (DE + RE tied: high = transmit), GND.",
                        "Marine (IEC 60945): isolate the transceiver (ISO1410 or opto + isolated DC-DC), add TVS on A/B, and conformal-coat the board."],
                board: PlannedBoard(width: 40, height: 28)),
            blocks: ["Supply", "Transceiver", "Termination & bias", "Bus connector", "Logic header"],
            supply: 5,
            category: "Marine",
            industry: "marine"),
        Template(
            keywords: ["plc", "industrial", "24 v input", "24v input", "digital input", "opto", "optocoupler", "isolated input", "factory"],
            plan: DesignPlan(
                title: "24 V PLC Digital Input",
                summary: "IEC 61131-2 Type 1 style 24 V input: reverse protection, current-limiting resistor and PC817 optocoupler to 5 V logic.",
                components: [
                    PlannedComponent(ref: "V1", kind: "voltage_source", value: "24", x: 0, y: 0),
                    PlannedComponent(ref: "SW1", kind: "switch", value: "on", x: 80, y: -60),
                    PlannedComponent(ref: "D2", kind: "diode", value: "1N4148", x: 160, y: -60),
                    PlannedComponent(ref: "R1", kind: "resistor", value: "2.2k", x: 240, y: -60),
                    PlannedComponent(ref: "U1", kind: "custom:PC817", value: "PC817", x: 340, y: 0),
                    PlannedComponent(ref: "NL1", kind: "net_label", value: "0V_FIELD", x: 0, y: 90),
                    PlannedComponent(ref: "NL2", kind: "net_label", value: "0V_FIELD", x: 300, y: 90),
                    PlannedComponent(ref: "V2", kind: "voltage_source", value: "5", x: 560, y: 0),
                    PlannedComponent(ref: "R2", kind: "resistor", value: "10k", x: 460, y: -80, rotation: 90),
                    PlannedComponent(ref: "NL3", kind: "net_label", value: "IN0", x: 520, y: -20),
                    PlannedComponent(ref: "C1", kind: "capacitor", value: "100n", x: 520, y: 60, rotation: 90),
                    PlannedComponent(ref: "NL4", kind: "net_label", value: "+5V", x: 560, y: -80),
                    PlannedComponent(ref: "NL5", kind: "net_label", value: "+5V", x: 460, y: -160),
                    PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 560, y: 90),
                    PlannedComponent(ref: "GND2", kind: "ground", value: "0", x: 420, y: 100),
                ],
                connections: [
                    PlannedConnection(from: "V1.+", to: "SW1.1"),
                    PlannedConnection(from: "V1.-", to: "NL1.N"),
                    PlannedConnection(from: "SW1.2", to: "D2.A"),
                    PlannedConnection(from: "D2.K", to: "R1.1"),
                    PlannedConnection(from: "R1.2", to: "U1.1"),
                    PlannedConnection(from: "U1.2", to: "NL2.N"),
                    PlannedConnection(from: "V2.+", to: "NL4.N"),
                    PlannedConnection(from: "V2.-", to: "GND1.GND"),
                    PlannedConnection(from: "NL5.N", to: "R2.1"),
                    PlannedConnection(from: "R2.2", to: "U1.4"),
                    PlannedConnection(from: "U1.4", to: "NL3.N"),
                    PlannedConnection(from: "U1.4", to: "C1.1"),
                    PlannedConnection(from: "C1.2", to: "GND2.GND"),
                    PlannedConnection(from: "U1.3", to: "GND2.GND"),
                ],
                notes: ["LED current ≈ (24 V − 0.7 V − 1.2 V) / 2.2 kΩ ≈ 10 mA; IEC 61131-2 Type 1: on above 15 V, off below 5 V.",
                        "0V_FIELD is the isolated field ground — keep ≥ 3 mm creepage between field and logic copper under U1.",
                        "IN0 is active-low (pulled up by R2); C1 filters contact bounce.",
                        "Add a TVS (SMBJ33A) across the field input for surge immunity (IEC 61000-4-5)."],
                board: PlannedBoard(width: 45, height: 28)),
            blocks: ["Field input", "Protection", "Optocoupler", "Logic output"],
            supply: 24,
            category: "Industrial",
            industry: "industrial"),
    ]
}
