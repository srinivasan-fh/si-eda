import Foundation

/// System prompts and request builders for SiEDA's design agents.
enum AgentPrompts {
    static var componentCatalog: String {
        ComponentKind.builtIn.map { kind in
            "- \(kind.planName): pins \(kind.pinNames.joined(separator: ", ")). Value: \(kind.valueHint)."
        }.joined(separator: "\n")
    }

    /// Built-in standard parts that are not already in the project library.
    static func standardPartsOutsideLibrary(_ parts: [CustomPartInfo]) -> [StandardPart] {
        let names = Set(parts.map { $0.name.lowercased() })
        return StandardLibrary.parts.filter { !names.contains($0.spec.name.lowercased()) }
    }

    /// Every "custom:<NAME>" kind a plan may use: project library parts plus the standard parts.
    static func customPlanKinds(_ parts: [CustomPartInfo]) -> [String] {
        parts.map(\.planKind) + standardPartsOutsideLibrary(parts).map { "custom:\($0.spec.name)" }
    }

    /// Project library and standard parts, addressed as kind "custom:<NAME>" with pins referenced by number.
    static func customCatalog(_ parts: [CustomPartInfo]) -> String {
        var text = ""
        if !parts.isEmpty {
            let lines = parts.map { part in
                let pins = part.symbol.pins.map { "\($0.number)=\($0.name)(\($0.type))" }.joined(separator: ", ")
                return "- \(part.planKind): \(part.description.isEmpty ? part.name : part.description), "
                    + "\(part.footprint) package. Pins (number=name(type)): \(pins)."
            }
            text += """

            Custom parts from the user's component library (imported from datasheets). Use them when they fit the \
            brief; reference their pins by NUMBER (e.g. "U1.8"), connect every power_in pin, and leave no_connect pins open:
            \(lines.joined(separator: "\n"))
            """
        }
        let standard = standardPartsOutsideLibrary(parts)
        if !standard.isEmpty {
            let lines = standard.map { part in
                let pins = part.spec.pins.map { "\($0.number)=\($0.name)(\($0.type.rawValue))" }.joined(separator: ", ")
                return "- custom:\(part.spec.name) [\(part.category)]: \(part.spec.description), "
                    + "\(part.spec.package.type)-\(part.spec.package.pinCount). Pins: \(pins)."
            }
            text += """

            Standard parts built into SiEDA (no simulation model: prefer the simulated built-in kinds above for \
            analogue behaviour, and use these when the brief names the part or needs a regulator, timer, MCU, logic \
            or driver IC). Reference pins by NUMBER, connect every power_in pin and add a 100n decoupling capacitor \
            from each IC supply pin to ground:
            \(lines.joined(separator: "\n"))
            """
        }
        return text
    }

    /// Industry profiles the plan can select, with the domain guidance the architect should apply.
    static var industryCatalog: String {
        let lines: [String] = StandardLibrary.industries.map { p -> String in
            let header = "- \(p.id) (\(p.name); \(p.standards); derating: \(p.deratingSummary)):"
            let tips = p.guidance.map { tip -> String in "    · " + tip }
            return ([header] + tips).joined(separator: "\n")
        }
        guard !lines.isEmpty else { return "" }
        return """

        Industry profiles — set "industry" to the one that matches the brief (robotics/motor control, drones/UAV, \
        power electronics, automotive/car, RF/radio, space, marine/ship, industrial automation, medical devices, \
        defence/military/avionics, networking/Ethernet/PoE/telecom, VLSI/ASIC/FPGA bring-up; otherwise general). The \
        profile selects the design rules and derates part ratings in SiEDA's validation, so choose resistor \
        wattages and currents with margin, and follow its guidance:
        \(lines.joined(separator: "\n"))
        """
    }

    static let analystSystem = """
    You are the Requirements Analyst agent inside SiEDA, a professional electronic design automation suite.
    Turn a product prompt or PRD into a compact, testable electrical specification for a small PCB.
    Infer sensible defaults when the brief is silent (typical: 5 V supply, 0805 passives, a two-layer board \
    between 20×15 mm and 80×60 mm). Keep requirements measurable (voltages, currents, frequencies, gains). \
    List the functional design blocks in signal-flow order.
    """

    static var architectSystem: String {
        """
        You are the Circuit Architect agent inside SiEDA, a professional EDA suite. You design schematics that \
        SiEDA compiles, simulates (SPICE-class DC and transient), places and autoroutes on a 1–6 layer PCB with copper pours.

        Express the circuit as a design plan using ONLY these component kinds and pin names:
        \(componentCatalog)

        Rules:
        - Every circuit needs at least one ground symbol; power inputs are voltage_source parts (they become \
        2-pin input connectors on the PCB). The "-" pin of the main supply goes to ground.
        - Use net_label parts for rails (VCC, VIN, VOUT…): labels with identical values are the same net, which \
        keeps wiring tidy. A net_label valued GND joins ground.
        - Connections are point-to-point pin pairs written "REF.PIN" (e.g. "R1.2" → "D1.A"). Connect every pin \
        that should be used; never leave a component floating.
        - Reference designators are unique: R, C, L, D, V, I, Q, U, SW, J, F, GND1…, NL1…
        - Values use engineering notation (330, 4k7, 100n, 10u). LEDs need a series current-limiting resistor. \
        Op-amps are ideal with ±15 V output rails and have no supply pins in the schematic.
        - Use SIN(offset amplitude frequency) or PULSE(v1 v2 period duty) source values when the brief needs a \
        time-varying stimulus for simulation.
        - Lay the schematic out on a 10-unit grid: signal flow left → right, supplies on the left, x from 0 to \
        about 600, y from -200 (top) to 200 (bottom), parts at least 80 units apart. Rotation 90 turns a part \
        vertical (pin 1 on top).
        - Choose a board size (mm) that comfortably fits the footprints (≈ 6×4 mm per small part plus routing).
        - Board: "layers" 2 for most designs, 4 for dense or RF boards, 6–10 for motherboards and ARM compute \
        modules, 12–16 for servers / mainframes, 16–24 for GPU / AI baseboards (0 keeps the current stack-up). \
        "differentialOhms" sets the pair impedance (85 for PCIe, 90 for USB, 100 for Ethernet / SerDes) and \
        "singleEndedOhms" the single-ended target (40 for DDR5, 50 otherwise). "outline" \
        is "rectangle" unless the product needs a shape: "quad-x" for multirotor frame PCBs (width = span, height \
        = body, outlineParameter = arm width ≈ 12 mm), "rounded" or "circle" for enclosures. \
        "mountingHoleSpacing" 30.5 gives the M3 flight-controller stack pattern, 20 the M2 one, 0 none. \
        "material" picks the laminate for high-speed or RF boards ("megtron-6" for multi-Gb/s digital, \
        "rogers-4350b" for RF / microwave, "fr4-hightg" for power and automotive; omit for FR-4) and "backdrill" \
        true removes via stubs on fast nets of 4+ layer boards. Name differential pairs X_P / X_N (or X_DP / X_DN) \
        and parallel buses DQ0…DQ7 / DATA0… with net labels: Auto Route matches their lengths with serpentines.
        - Pour ground: on a 2-layer board GND on layer -1 (bottom) and 0 (top); on 4 layers a GND plane on layer \
        1 (plane true). Give battery, motor and regulator-output nets carrying ≥ 0.5 A a net class (≈ 0.6 mm per \
        amp of 1 oz copper). List unused IC pins in "noConnect" ("U3.9") so ERC knows they are open on purpose.
        - Robots (Mars rover, FPV drone, industrial arm, quadruped, humanoid): set "robotPlatform" and design the \
        seven segments — power distribution (battery input with reverse-polarity MOSFET, fuse / eFuse, TVS, \
        isolated logic rail), compute (MCU / SoM with debug header), motion (gate driver + MOSFET bridge, \
        low-value Kelvin shunt + amplifier), sensors (IMU, an AGND net tied to GND through one ferrite / 0 Ω star \
        point, CANH / CANL with a 120 Ω termination), communication (radio module), safety (a net named ESTOP \
        from a normally-closed switch to every gate driver SD / EN pin, status LEDs) and mechanical (mounting holes).
        - Put design calculations and assumptions in notes.
        \(industryCatalog)
        """
    }

    static let reviewerSystem = """
    You are the Design Review agent inside SiEDA. You receive the specification, the current design plan, the \
    electrical rule check (ERC) report and the DC operating point from SiEDA's simulator.
    Approve the design only if it meets the specification, has no ERC errors, no floating parts and sensible \
    operating values (LED currents 2–20 mA, transistors saturated when used as switches, no component \
    dissipating beyond typical 0805/SOT-23 ratings ≈ 125–250 mW, reduced by the plan's industry derating — e.g. \
    50 % for space, 60 % for automotive). Check the industry guidance (protection, decoupling, termination, \
    isolation) was followed and keep the plan's industry field.
    If anything is wrong, set approved to false, list the issues, and return the COMPLETE corrected plan (not a \
    diff), keeping reference designators and positions of unchanged parts. If approved, return the plan unchanged.
    """

    static func analystRequest(brief: String) -> AIRequest {
        AIRequest(system: analystSystem,
                  prompt: "<requirements>\n\(brief)\n</requirements>\n\nProduce the electrical specification.",
                  schemaName: "requirements_spec", schema: DesignSchemas.requirements)
    }

    static func architectRequest(brief: String, spec: RequirementsSpec, customParts: [CustomPartInfo] = []) -> AIRequest {
        AIRequest(system: architectSystem + customCatalog(customParts),
                  prompt: """
                  <requirements>
                  \(brief)
                  </requirements>

                  <specification>
                  \(spec.jsonString())
                  </specification>

                  Design the complete circuit as a design plan.
                  """,
                  schemaName: "design_plan", schema: DesignSchemas.designPlanSchema(customKinds: customPlanKinds(customParts)))
    }

    static func refineRequest(instruction: String, current: DesignPlan, requirements: String,
                              customParts: [CustomPartInfo] = []) -> AIRequest {
        AIRequest(system: architectSystem + customCatalog(customParts),
                  prompt: """
                  <requirements>
                  \(requirements)
                  </requirements>

                  <current_plan>
                  \(current.jsonString())
                  </current_plan>

                  <change_request>
                  \(instruction)
                  </change_request>

                  Apply the change request and return the COMPLETE updated design plan. Keep reference designators \
                  and positions of parts that do not need to change.
                  """,
                  schemaName: "design_plan", schema: DesignSchemas.designPlanSchema(customKinds: customPlanKinds(customParts)))
    }

    static func reviewRequest(spec: RequirementsSpec?, brief: String, plan: DesignPlan, erc: [RuleViolation],
                              dc: DCResult?, customParts: [CustomPartInfo] = []) -> AIRequest {
        let ercText = erc.isEmpty
            ? "No findings."
            : erc.map { "[\($0.severity.rawValue)] \($0.code): \($0.message)" }.joined(separator: "\n")
        var dcText = "Not available."
        if let dc {
            if dc.converged {
                let nets = dc.nets.map { "V(\($0.name)) = \(EngineeringFormat.string($0.voltage, unit: "V", digits: 4))" }
                let devices = dc.devices.map {
                    "\($0.ref): I = \(EngineeringFormat.string($0.current, unit: "A", digits: 4)), "
                        + "P = \(EngineeringFormat.string($0.power, unit: "W", digits: 3))"
                }
                dcText = (nets + devices).joined(separator: "\n")
            } else {
                dcText = "Did not converge: \(dc.error)"
            }
        }
        return AIRequest(system: reviewerSystem + customCatalog(customParts),
                         prompt: """
                         <requirements>
                         \(brief)
                         </requirements>

                         <specification>
                         \(spec?.jsonString() ?? "{}")
                         </specification>

                         <current_plan>
                         \(plan.jsonString())
                         </current_plan>

                         <erc>
                         \(ercText)
                         </erc>

                         <simulation>
                         \(dcText)
                         </simulation>

                         Review the design.
                         """,
                         schemaName: "design_review", schema: DesignSchemas.reviewSchema(customKinds: customPlanKinds(customParts)))
    }
}
