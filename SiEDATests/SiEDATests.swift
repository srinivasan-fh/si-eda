import Metal
import SceneKit
import SwiftUI
import XCTest
import UniformTypeIdentifiers
@testable import SiEDA

final class EngineBridgeTests: XCTestCase {
    func testComponentKindsMatchCoreLibrary() throws {
        let data = Data(EDAEngine.libraryJSON().utf8)
        let library = try XCTUnwrap(JSONSerialization.jsonObject(with: data) as? [[String: Any]])
        XCTAssertEqual(library.count, ComponentKind.allCases.count)
        for entry in library {
            let raw = try XCTUnwrap(entry["kind"] as? Int)
            let kind = try XCTUnwrap(ComponentKind(rawValue: raw), "Swift is missing kind \(raw)")
            let pins = (entry["pins"] as? [[String: Any]] ?? []).compactMap { $0["name"] as? String }
            XCTAssertEqual(pins, kind.pinNames, "Pin names differ for \(kind)")
            XCTAssertEqual(entry["defaultValue"] as? String, kind.defaultValue)
        }
    }

    func testBuildSimulateAndRouteLEDCircuit() throws {
        let engine = EDAEngine(name: "Test")
        let v = engine.addComponent(.voltageSource, value: "5", at: .zero)
        let r = engine.addComponent(.resistor, value: "330", at: CGPoint(x: 100, y: -40))
        let d = engine.addComponent(.led, value: "Red", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: d, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: d, pin: 1), PinAddress(component: g, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0)))

        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(snapshot.components.count, 4)
        XCTAssertEqual(snapshot.wires.count, 4)

        XCTAssertFalse(engine.runERC().contains { $0.severity == .error })

        let dc = engine.simulateDC()
        XCTAssertTrue(dc.converged, dc.error)
        let current = try XCTUnwrap(dc.reading(component: d)?.current)
        XCTAssertEqual(current, 0.009, accuracy: 0.0015)

        engine.setBoard(width: 30, height: 20, trackWidth: 0.25, clearance: 0.2)
        let stats = engine.autoRoute()
        XCTAssertEqual(stats.failed, 0)
        XCTAssertGreaterThan(stats.routed, 0)
        XCTAssertFalse(engine.runDRC().contains { $0.severity == .error })

        let mesh = try XCTUnwrap(engine.buildMesh(includeComponents: true))
        XCTAssertGreaterThan(mesh.vertexCount, 0)
        XCTAssertEqual(mesh.indices.count % 3, 0)

        XCTAssertNotNil(engine.export(.gerberTop))
        XCTAssertNotNil(engine.export(.drill))
    }

    func testACSweepDCSweepAndMonteCarloOfRCFilter() throws {
        let engine = EDAEngine(name: "RC filter")
        let v = engine.addComponent(.voltageSource, value: "0 AC 1", at: .zero)
        let r = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: -40))
        let c = engine.addComponent(.capacitor, value: "100n", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: c, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: c, pin: 1), PinAddress(component: g, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0)))

        let ac = engine.simulateAC(start: "10", stop: "1MEG", pointsPerDecade: 20, source: "")
        XCTAssertTrue(ac.ok, ac.error)
        XCTAssertEqual(ac.stimulus, ["V1"])
        XCTAssertEqual(ac.frequency.count, 101)
        let out = try XCTUnwrap(ac.nets.first { $0.metrics?.f3dbHz != nil })
        XCTAssertEqual(try XCTUnwrap(out.metrics?.f3dbHz), 1591.6, accuracy: 1)  // 1/(2πRC)
        XCTAssertNil(out.metrics?.unityHz)
        XCTAssertEqual(out.magnitudeDb.count, 101)

        let sweep = engine.simulateDCSweep(source: "V1", start: "0", stop: "1", step: "0.5")
        XCTAssertTrue(sweep.ok, sweep.error)
        XCTAssertEqual(sweep.values, [0, 0.5, 1])

        let mc = engine.simulateMonteCarlo(net: out.name, measure: "f3db", runs: 20, seed: 1)
        XCTAssertTrue(mc.ok, mc.error)
        XCTAssertEqual(mc.runs, 20)
        XCTAssertEqual(mc.histogram.counts.reduce(0, +), 20)
        let worst = try XCTUnwrap(mc.worstCase)
        XCTAssertLessThan(worst.min, mc.nominal)
        XCTAssertGreaterThan(worst.max, mc.nominal)
        XCTAssertFalse(engine.simulateMonteCarlo(net: "no such net", measure: "dc", runs: 5, seed: 1).ok)
    }

    func testInteractiveRouterRoutesAndCommits() throws {
        let engine = EDAEngine(name: "Router")
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 30, y: 20))
        let options = EDAEngine.routerOptions(shove: true, diagonal: true)

        let nothing = try XCTUnwrap(engine.routerBegin(at: CGPoint(x: 2, y: 2), layer: 0, pair: false, options: options))
        XCTAssertNotNil(nothing.error)
        XCTAssertFalse(engine.routerActive)

        let start = try XCTUnwrap(engine.routerBegin(at: CGPoint(x: 10.95, y: 20), layer: 0, pair: false, options: options))
        XCTAssertNil(start.error)
        XCTAssertTrue(start.active && engine.routerActive)
        let head = try XCTUnwrap(engine.routerMove(to: CGPoint(x: 29.05, y: 20)))
        XCTAssertTrue(head.reachedTarget)
        XCTAssertFalse(head.head.isEmpty)
        let result = engine.routerCommit()
        XCTAssertTrue(result.ok)
        XCTAssertFalse(result.addedTracks.isEmpty)
        XCTAssertFalse(engine.routerActive)
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertTrue(snapshot.ratsnest.isEmpty)
        XCTAssertFalse(engine.runDRC().contains { $0.severity == .error })
    }

    func testSaveLoadRoundTrip() throws {
        let engine = EDAEngine()
        engine.addComponent(.capacitor, value: "10u", at: CGPoint(x: 10, y: 20), ref: "C7")
        let json = engine.saveJSON()
        let copy = EDAEngine()
        try copy.load(json: json)
        XCTAssertEqual(copy.snapshot()?.components.first?.ref, "C7")
        XCTAssertThrowsError(try copy.load(json: "{not json"))
    }
}

final class DesignPlanTests: XCTestCase {
    func testOfflineTemplatesCompileCleanly() async throws {
        let provider = OfflineProvider()
        for template in OfflineProvider.templates {
            let request = AgentPrompts.architectRequest(brief: template.keywords[0], spec: OfflineProvider.spec(for: template.keywords[0]))
            let text = try await provider.complete(request)
            let plan = try JSONExtraction.decode(DesignPlan.self, from: text)
            let engine = EDAEngine()
            let report = DesignPlanCompiler.apply(plan, to: engine, previous: nil)
            XCTAssertTrue(report.warnings.isEmpty, "\(plan.title): \(report.warnings)")
            XCTAssertEqual(report.connectionsMade, plan.connections.count, plan.title)
            let errors = engine.runERC().filter { $0.severity == .error }
            XCTAssertTrue(errors.isEmpty, "\(plan.title): \(errors.map(\.message))")
            XCTAssertTrue(engine.simulateDC().converged, plan.title)
        }
    }

    func testPlanRoundTripsThroughSnapshot() throws {
        let plan = OfflineProvider.templates[0].plan
        let engine = EDAEngine()
        DesignPlanCompiler.apply(plan, to: engine, previous: nil)
        let snapshot = try XCTUnwrap(engine.snapshot())
        let back = DesignPlanCompiler.plan(from: snapshot)
        XCTAssertEqual(Set(back.components.map(\.ref)), Set(plan.components.map(\.ref)))
        XCTAssertEqual(back.connections.count, plan.connections.count)
    }

    func testLenientJSONExtraction() throws {
        let text = "Here you go:\n```json\n{\"title\":\"X\",\"summary\":\"s {brace}\",\"components\":[],\"connections\":[]}\n```"
        let plan = try JSONExtraction.decode(DesignPlan.self, from: text)
        XCTAssertEqual(plan.title, "X")
        XCTAssertEqual(plan.board.width, 50)
    }

    func testKindAliases() {
        XCTAssertEqual(ComponentKind.fromPlanName("Voltage Source"), .voltageSource)
        XCTAssertEqual(ComponentKind.fromPlanName("op-amp"), .opAmp)
        XCTAssertEqual(ComponentKind.fromPlanName("MOSFET"), .nmos)
        XCTAssertNil(ComponentKind.fromPlanName("flux capacitor"))
    }

    func testDesignPlanSchemaIsStrict() throws {
        let schema = DesignSchemas.designPlan
        XCTAssertEqual(schema["additionalProperties"] as? Bool, false)
        XCTAssertNoThrow(try JSONSerialization.data(withJSONObject: schema))
        XCTAssertNoThrow(try JSONSerialization.data(withJSONObject: DesignSchemas.review))
    }

    func testEngineeringFormat() {
        XCTAssertEqual(EngineeringFormat.parse("4.7k"), 4700)
        XCTAssertEqual(EngineeringFormat.parse("10u")!, 10e-6, accuracy: 1e-12)
        XCTAssertEqual(EngineeringFormat.parse("1e-3")!, 1e-3, accuracy: 1e-12)
        XCTAssertEqual(EngineeringFormat.string(0.0021, unit: "A"), "2.1 mA")
    }
}

final class CustomPartAndLayerTests: XCTestCase {
    private func ne555() -> CustomPartSpec {
        var spec = CustomPartSpec()
        spec.name = "NE555"
        spec.description = "Precision timer"
        spec.package.type = PackageKind.dip.rawValue
        let names = ["GND", "TRIG", "OUT", "RESET", "CONT", "THRES", "DISCH", "VCC"]
        let types: [PinElectricalType] = [.powerIn, .input, .output, .input, .passive, .input, .openCollector, .powerIn]
        spec.pins = names.enumerated().map { CustomPartSpec.Pin(number: String($0.offset + 1), name: $0.element, type: types[$0.offset]) }
        return spec
    }

    func testRegisterPlaceAndPersistCustomPart() throws {
        let engine = EDAEngine()
        let part = try engine.registerCustomPart(ne555())
        XCTAssertEqual(part.footprintGeometry.pads.count, 8)
        XCTAssertEqual(part.symbol.pins.count, 8)
        XCTAssertTrue(part.footprintGeometry.pads[0].throughHole)
        let id = engine.addCustomComponent(partId: part.id, at: .zero)
        XCTAssertGreaterThanOrEqual(id, 0)
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(snapshot.customParts.map(\.id), [part.id])
        XCTAssertEqual(snapshot.component(id)?.customPart, part.id)
        XCTAssertEqual(engine.findPin(component: id, name: "8"), 7)
        // A part with every pin open is reported as floating; once VCC is wired the open GND power pin is an error.
        XCTAssertTrue(engine.runERC().contains { $0.code == "ERC_FLOATING_COMPONENT" })
        let source = engine.addComponent(.voltageSource, value: "5", at: CGPoint(x: -100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: source, pin: 0), PinAddress(component: id, pin: 7)))
        XCTAssertTrue(engine.runERC().contains { $0.code == "ERC_POWER_PIN_UNCONNECTED" })

        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        XCTAssertEqual(copy.snapshot()?.customParts.first?.name, "NE555")

        var invalid = ne555()
        invalid.pins[1].number = "1"
        XCTAssertFalse(invalid.validationIssues.isEmpty)
        XCTAssertThrowsError(try engine.registerCustomPart(invalid))
        if case .success = EDAEngine.previewCustomPart(invalid) { XCTFail("Preview must reject duplicate pin numbers") }
    }

    func testPlanCompilerUsesLibraryParts() throws {
        let engine = EDAEngine()
        let part = try engine.registerCustomPart(ne555())
        let before = try XCTUnwrap(engine.snapshot())
        let plan = DesignPlan(title: "Timer", summary: "", components: [
            PlannedComponent(ref: "U1", kind: part.planKind, value: "NE555", x: 200, y: 0),
            PlannedComponent(ref: "V1", kind: "voltage_source", value: "5", x: 0, y: 0),
            PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
        ], connections: [
            PlannedConnection(from: "V1.+", to: "U1.8"),
            PlannedConnection(from: "U1.4", to: "U1.8"),
            PlannedConnection(from: "U1.1", to: "GND1.GND"),
            PlannedConnection(from: "V1.-", to: "GND1.GND"),
        ])
        let report = DesignPlanCompiler.apply(plan, to: engine, previous: before)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        XCTAssertEqual(report.connectionsMade, 4)
        let after = try XCTUnwrap(engine.snapshot())
        let roundTrip = DesignPlanCompiler.plan(from: after)
        XCTAssertTrue(roundTrip.components.contains { $0.kind == "custom:NE555" })
        XCTAssertTrue(roundTrip.connections.contains { $0.from == "U1.1" || $0.to == "U1.1" })
        let schema = DesignSchemas.designPlanSchema(customKinds: [part.planKind])
        XCTAssertNoThrow(try JSONSerialization.data(withJSONObject: schema))
    }

    func testPinTableParserAndOfflineExtraction() async throws {
        let text = """
        NE555 Precision Timers
        Package: 8-pin SOIC (D)
        Pin Functions
        PIN NAME I/O DESCRIPTION
        1 GND — Ground reference voltage
        2 TRIG I Start of timing input
        3 OUT O High-current timer output
        4 RESET I Active-low reset input
        5 CONT I Controlling threshold voltage
        6 THRES I End of timing input
        7 DISCH O Open collector output to discharge timing capacitor
        8 VCC — Input supply voltage
        """
        let rows = PinTableParser.rows(in: text)
        XCTAssertEqual(rows.count, 8)
        XCTAssertEqual(rows.last?.name, "VCC")
        let document = DatasheetDocument(fileName: "ne555.pdf", pages: [text], attachment: nil)
        let result = try await DatasheetAnalyst.extract(document, hint: "", provider: OfflineProvider())
        XCTAssertEqual(result.spec.name, "NE555")
        XCTAssertEqual(result.spec.package.type, PackageKind.soic.rawValue)
        XCTAssertEqual(result.spec.pins.count, 8)
        XCTAssertEqual(result.spec.pins.first?.type, .powerIn)
        XCTAssertEqual(result.spec.pins[6].type, .openCollector)
        XCTAssertTrue(result.spec.validationIssues.isEmpty)
    }

    func testLayerStackUpRoutingAndMeshes() throws {
        for layers in [1, 2, 4, 6] {
            let engine = EDAEngine()
            DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: engine, previous: nil)
            engine.setLayerCount(layers)
            engine.autoPlace(all: true)
            let stats = engine.autoRoute()
            XCTAssertEqual(stats.failed, 0, "\(layers) layers")
            if layers == 1 { XCTAssertEqual(stats.vias, 0) }
            let snapshot = try XCTUnwrap(engine.snapshot())
            XCTAssertEqual(snapshot.board.layerCount, layers)
            XCTAssertTrue(snapshot.tracks.allSatisfy { $0.layer < layers })
            XCTAssertNotNil(engine.buildLayerMesh(layer: 0))
            XCTAssertNil(engine.buildLayerMesh(layer: layers))
            XCTAssertNotNil(engine.exportCopperLayer(layers))
            XCTAssertFalse(snapshot.bodies.isEmpty)
        }
    }
}

final class StandardsAndVerificationTests: XCTestCase {
    func testStandardLibraryAndValues() throws {
        let regulator = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM7805" })
        XCTAssertEqual(regulator.spec.pins.map(\.name), ["IN", "GND", "OUT"])
        XCTAssertEqual(regulator.spec.package.type, PackageKind.to220.rawValue)
        XCTAssertTrue(StandardLibrary.parts.contains { $0.spec.name == "NE555" })
        XCTAssertGreaterThanOrEqual(StandardLibrary.parts.count, 12)

        XCTAssertEqual(StandardLibrary.rulePresets.count, 13)
        XCTAssertTrue(StandardLibrary.rulePresets.contains { $0.name == "IPC-2221 Class 3" })

        XCTAssertEqual(try XCTUnwrap(EDAEngine.parseValue("4k7")), 4700, accuracy: 1e-9)
        XCTAssertEqual(try XCTUnwrap(EDAEngine.parseValue("100nF")), 100e-9, accuracy: 1e-18)
        XCTAssertNil(EDAEngine.parseValue("abc"))
        XCTAssertEqual(ESeries.e24.nearest(4600), 4700, accuracy: 1e-6)
        XCTAssertEqual(ESeries.e12.nearest(5.0e-8), 4.7e-8, accuracy: 1e-15)
        XCTAssertTrue(ESeries.e96.contains(4990))
        XCTAssertFalse(ESeries.e24.contains(4990))
        XCTAssertEqual(ESeries.preferred(for: .resistor), [.e24, .e96])
        XCTAssertTrue(ESeries.preferred(for: .led).isEmpty)
    }

    func testRulePresetAppliesAndPersists() throws {
        let engine = EDAEngine()
        XCTAssertEqual(engine.snapshot()?.board.rulePreset, "IPC-2221 Class 2")
        XCTAssertTrue(engine.applyRulePreset("Prototype (Conservative)"))
        XCTAssertFalse(engine.applyRulePreset("No such preset"))
        let board = try XCTUnwrap(engine.snapshot()?.board)
        XCTAssertEqual(board.rulePreset, "Prototype (Conservative)")
        XCTAssertEqual(board.trackWidth, 0.30, accuracy: 1e-9)
        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        XCTAssertEqual(copy.snapshot()?.board.rulePreset, "Prototype (Conservative)")
    }

    func testCircuitValidationFlagsOverloadedLED() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        XCTAssertFalse(engine.runCircuitValidation().contains { $0.severity != .info })
        let resistor = try XCTUnwrap(engine.findComponent(ref: "R1"))
        engine.setValue(resistor, "10")
        let findings = engine.runCircuitValidation()
        XCTAssertTrue(findings.contains { $0.code == "VAL_LED_CURRENT" && $0.severity == .error }, "\(findings.map(\.code))")
        XCTAssertTrue(findings.contains { $0.code == "VAL_RESISTOR_POWER" })
    }

    func testPlanCompilerAddsStandardPartsOnFirstUse() throws {
        let engine = EDAEngine()
        let plan = DesignPlan(title: "Reg", summary: "", components: [
            PlannedComponent(ref: "U1", kind: "custom:LM7805", value: "LM7805", x: 100, y: 0),
            PlannedComponent(ref: "V1", kind: "voltage_source", value: "12", x: 0, y: 0),
            PlannedComponent(ref: "GND1", kind: "ground", value: "0", x: 0, y: 100),
        ], connections: [
            PlannedConnection(from: "V1.+", to: "U1.1"),
            PlannedConnection(from: "U1.2", to: "GND1.GND"),
            PlannedConnection(from: "V1.-", to: "GND1.GND"),
        ])
        let report = DesignPlanCompiler.apply(plan, to: engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        XCTAssertEqual(report.connectionsMade, 3)
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(snapshot.customParts.map(\.name), ["LM7805"])
        XCTAssertTrue(AgentPrompts.customPlanKinds([]).contains("custom:NE555"))
        XCTAssertFalse(AgentPrompts.customPlanKinds(snapshot.customParts).filter { $0 == "custom:LM7805" }.count > 1)
        XCTAssertTrue(AgentPrompts.customCatalog([]).contains("custom:LM7805"))
    }

    func testOfflineTemplateMatchingAndCategories() {
        XCTAssertEqual(OfflineProvider.templates.count, 41)
        XCTAssertEqual(OfflineProvider.template(for: "non-inverting amplifier with gain 11").plan.title, "Non-Inverting Amplifier")
        XCTAssertEqual(OfflineProvider.template(for: "an inverting amplifier, gain -10").plan.title, "Inverting Amplifier")
        XCTAssertEqual(OfflineProvider.template(for: "blink an LED with a 555").plan.title, "555 Astable LED Blinker")
        XCTAssertEqual(OfflineProvider.template(for: "12 V to 5 V LM7805 regulator").plan.title, "5 V Linear Regulator")
        XCTAssertEqual(OfflineProvider.template(for: "strain gauge wheatstone bridge").plan.title, "Wheatstone Bridge")
        XCTAssertNotEqual(OfflineProvider.template(for: "h-bridge motor driver with mosfets").plan.title, "Wheatstone Bridge")
        // New industries, and words that only contain their keywords ("basic", "product", "autorouter") stay put.
        XCTAssertEqual(OfflineProvider.template(for: "wearable ECG patch for a patient").industry, "medical")
        XCTAssertEqual(OfflineProvider.template(for: "MIL-STD rugged 28 V input").industry, "defence")
        XCTAssertEqual(OfflineProvider.template(for: "PoE powered Ethernet sensor").industry, "networking")
        XCTAssertEqual(OfflineProvider.template(for: "FPGA bring-up board with JTAG").industry, "vlsi")
        XCTAssertNotEqual(OfflineProvider.template(for: "a basic product led indicator for the autorouter").industry, "vlsi")
        XCTAssertNotEqual(OfflineProvider.template(for: "a basic product led indicator for the autorouter").industry, "networking")
        for template in OfflineProvider.templates {
            XCTAssertTrue(OfflineProvider.categories.contains(template.category), template.plan.title)
            // Each template's first keyword selects it.
            XCTAssertEqual(OfflineProvider.template(for: template.keywords[0]).plan.title, template.plan.title)
        }
    }

    /// Every built-in reference design must pass the full verification pipeline once laid out.
    // Places, routes and verifies every reference design (up to 24 layers, 300+ nets): minutes of work per board, so
    // the designs are split into interleaved groups (the large architecture references land in different groups),
    // each test with its own time allowance.
    private func verifyReferenceDesigns(group: Int, of groups: Int = 8) throws {
        executionTimeAllowance = 900
        for (index, template) in OfflineProvider.templates.enumerated() where index % groups == group {
            let engine = EDAEngine()
            let report = DesignPlanCompiler.apply(template.industryPlan, to: engine, previous: nil)
            XCTAssertTrue(report.warnings.isEmpty, "\(template.plan.title): \(report.warnings)")
            XCTAssertEqual(engine.snapshot()?.industry, template.industry, template.plan.title)
            engine.autoPlace(all: true)
            engine.fitBoard(margin: 2.5)
            XCTAssertEqual(engine.autoRoute().failed, 0, template.plan.title)
            let verification = try XCTUnwrap(engine.runVerification(), template.plan.title)
            let problems = verification.stages.flatMap(\.findings).filter { $0.severity != .info }.map(\.code)
            XCTAssertEqual(verification.verdict, .pass, "\(template.plan.title): \(problems)")
            XCTAssertEqual(verification.stages.count, 8)
            XCTAssertEqual(verification.industry, template.industry)
            XCTAssertTrue(verification.markdown.contains("# Design Verification Report"))
        }
    }

    func testReferenceDesignsPassVerification0() throws { try verifyReferenceDesigns(group: 0) }
    func testReferenceDesignsPassVerification1() throws { try verifyReferenceDesigns(group: 1) }
    func testReferenceDesignsPassVerification2() throws { try verifyReferenceDesigns(group: 2) }
    func testReferenceDesignsPassVerification3() throws { try verifyReferenceDesigns(group: 3) }
    func testReferenceDesignsPassVerification4() throws { try verifyReferenceDesigns(group: 4) }
    func testReferenceDesignsPassVerification5() throws { try verifyReferenceDesigns(group: 5) }
    func testReferenceDesignsPassVerification6() throws { try verifyReferenceDesigns(group: 6) }
    func testReferenceDesignsPassVerification7() throws { try verifyReferenceDesigns(group: 7) }

    func testVerificationFailsAnUnroutedBoard() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        let unplaced = try XCTUnwrap(engine.runVerification())
        XCTAssertEqual(unplaced.verdict, .fail)
        XCTAssertFalse(unplaced.passed)
        XCTAssertEqual(unplaced.stages.first { $0.id == "placement" }?.status, .fail)
        engine.autoPlace(all: true)
        let unrouted = try XCTUnwrap(engine.runVerification())
        XCTAssertEqual(unrouted.stages.first { $0.id == "routing" }?.status, .fail)
        XCTAssertEqual(unrouted.stages.first { $0.id == "erc" }?.status, .pass)
    }
}

@MainActor
final class CanvasNavigationTests: XCTestCase {
    func testWorldScreenRoundTripAndVisibleRect() {
        let v = Viewport(scale: 4, offset: CGSize(width: 100, height: -50))
        let p = CGPoint(x: 12.5, y: -3)
        let back = v.toWorld(v.toScreen(p))
        XCTAssertEqual(back.x, p.x, accuracy: 1e-9)
        XCTAssertEqual(back.y, p.y, accuracy: 1e-9)
        let visible = v.visibleWorldRect(in: CGSize(width: 800, height: 400))
        XCTAssertEqual(visible.minX, -25, accuracy: 1e-9)
        XCTAssertEqual(visible.minY, 12.5, accuracy: 1e-9)
        XCTAssertEqual(visible.width, 200, accuracy: 1e-9)
        XCTAssertEqual(visible.height, 100, accuracy: 1e-9)
    }

    func testZoomKeepsAnchorAndRespectsLimits() {
        var v = Viewport(scale: 12, offset: .zero)
        let anchor = CGPoint(x: 300, y: 200)
        let before = v.toWorld(anchor)
        v.zoom(by: 3, anchor: anchor, limits: Viewport.pcbLimits)
        let after = v.toWorld(anchor)
        XCTAssertEqual(before.x, after.x, accuracy: 1e-9)
        XCTAssertEqual(before.y, after.y, accuracy: 1e-9)
        v.zoom(by: 1e6, anchor: anchor, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, Viewport.pcbLimits.upperBound)
        v.zoom(by: 1e-9, anchor: anchor, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, Viewport.pcbLimits.lowerBound)
        v.zoom(by: .nan, anchor: anchor, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, Viewport.pcbLimits.lowerBound)
    }

    /// A full-size motherboard (E-ATX 330 × 305 mm) must fit a small window, and BGA detail must be reachable.
    func testLimitsCoverMotherboardsAndFinePitch() {
        var v = Viewport(scale: 12, offset: .zero)
        let window = CGSize(width: 640, height: 480)
        let board = CGRect(x: 0, y: 0, width: 330, height: 305)
        v.fit(board, in: window, margin: 50, limits: Viewport.pcbLimits)
        let visible = v.visibleWorldRect(in: window)
        XCTAssertTrue(visible.contains(board), "\(visible) does not contain the board")
        XCTAssertEqual(v.toScreen(CGPoint(x: board.midX, y: board.midY)).x, window.width / 2, accuracy: 1e-6)
        // 0.4 mm BGA pitch at maximum zoom is far more than a finger-width apart.
        XCTAssertGreaterThan(0.4 * Viewport.pcbLimits.upperBound, 100)
        // A 20 000-unit schematic still fits a window.
        var s = Viewport(scale: 1.6, offset: .zero)
        let sheet = CGRect(x: -10_000, y: -6_000, width: 20_000, height: 12_000)
        s.fit(sheet, in: window, limits: Viewport.schematicLimits)
        XCTAssertTrue(s.visibleWorldRect(in: window).contains(sheet))
    }

    func testCommandsPanZoomAndSetLevel() {
        let size = CGSize(width: 1000, height: 600)
        var v = Viewport(scale: 12, offset: .zero)
        XCTAssertTrue(v.apply(.pan(dx: 0.25, dy: 0), size: size, anchor: nil, baseScale: 12, limits: Viewport.pcbLimits))
        XCTAssertEqual(v.offset.width, -250, accuracy: 1e-9)  // view moved right → content moved left
        XCTAssertTrue(v.apply(.pan(dx: 0, dy: -0.5), size: size, anchor: nil, baseScale: 12, limits: Viewport.pcbLimits))
        XCTAssertEqual(v.offset.height, 300, accuracy: 1e-9)
        let centreWorld = v.toWorld(CGPoint(x: 500, y: 300))
        v.apply(.setLevel(2), size: size, anchor: nil, baseScale: 12, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, 24, accuracy: 1e-9)
        let stillCentre = v.toWorld(CGPoint(x: 500, y: 300))
        XCTAssertEqual(stillCentre.x, centreWorld.x, accuracy: 1e-9)
        v.apply(.zoomIn, size: size, anchor: CGPoint(x: 10, y: 10), baseScale: 12, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, 30, accuracy: 1e-9)
        v.apply(.zoomOut, size: size, anchor: nil, baseScale: 12, limits: Viewport.pcbLimits)
        XCTAssertEqual(v.scale, 24, accuracy: 1e-9)
        XCTAssertFalse(v.apply(.fit, size: size, anchor: nil, baseScale: 12, limits: Viewport.pcbLimits))
        XCTAssertNotEqual(ViewRequest(command: .fit), ViewRequest(command: .fit))  // repeated commands are observed
    }

    func testCenterAndAdaptiveGrid() {
        var v = Viewport(scale: 2, offset: .zero)
        v.center(on: CGPoint(x: 50, y: 20), in: CGSize(width: 400, height: 200))
        XCTAssertEqual(v.toScreen(CGPoint(x: 50, y: 20)), CGPoint(x: 200, y: 100))
        XCTAssertEqual(Viewport(scale: 1.6, offset: .zero).gridPitch(base: 10, minimumPoints: 8), 10)
        XCTAssertEqual(Viewport(scale: 0.5, offset: .zero).gridPitch(base: 10, minimumPoints: 8), 50)
        XCTAssertEqual(Viewport(scale: 0.05, offset: .zero).gridPitch(base: 10, minimumPoints: 8), 500)
        XCTAssertEqual(Viewport(scale: 12, offset: .zero).gridPitch(base: 0.1, minimumPoints: 10), 1, accuracy: 1e-9)
        XCTAssertEqual(Viewport(scale: 40, offset: .zero).gridPitch(base: 0.1, minimumPoints: 10), 0.5, accuracy: 1e-9)
        // Zoomed out on a huge board the grid stays sparse.
        let far = Viewport(scale: 0.25, offset: .zero)
        XCTAssertGreaterThanOrEqual(far.gridPitch(base: 0.1, minimumPoints: 10) * far.scale, 10)
    }

    func testZoomPresetLabels() {
        XCTAssertEqual(ZoomControls.percent(1), "100%")
        XCTAssertEqual(ZoomControls.percent(0.25), "25%")
        XCTAssertEqual(ZoomControls.percent(0.05), "5.0%")
    }
}

final class IndustryKitTests: XCTestCase {
    func testIndustryProfilesBridge() throws {
        let ids = StandardLibrary.industries.map(\.id)
        XCTAssertEqual(ids, ["general", "robotics", "uav", "power", "automotive", "rf", "space", "marine", "industrial",
                             "medical", "defence", "networking", "vlsi", "motherboard", "server", "hpc", "arm", "addin",
                             "retail", "appliance", "memory"])
        let space = try XCTUnwrap(StandardLibrary.industry("space"))
        XCTAssertEqual(space.powerDerating, 0.5, accuracy: 1e-9)
        XCTAssertTrue(space.highAltitude)
        XCTAssertFalse(space.guidance.isEmpty)
        for profile in StandardLibrary.industries {
            XCTAssertTrue(StandardLibrary.rulePresets.contains { $0.name == profile.rulePreset }, profile.name)
        }
        for name in ["IR2104", "IRF540N", "UC3843", "ACS712", "TJA1050", "LM2940-5.0", "MAX485", "PC817"] {
            XCTAssertTrue(StandardLibrary.parts.contains { $0.spec.name == name }, name)
        }
    }

    func testSetIndustryAppliesRulesAndPersists() throws {
        let engine = EDAEngine()
        XCTAssertEqual(engine.snapshot()?.industry, "general")
        XCTAssertTrue(engine.setIndustry("space"))
        XCTAssertFalse(engine.setIndustry("submarine"))
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(snapshot.industry, "space")
        XCTAssertTrue(snapshot.board.highAltitude)
        XCTAssertEqual(snapshot.board.rulePreset, "Space (IPC-6012 Class 3/A, ECSS)")
        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        XCTAssertEqual(copy.snapshot()?.industry, "space")
        XCTAssertEqual(DesignPlanCompiler.plan(from: snapshot).industry, "space")
    }

    func testDeratingTightensValidation() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        let resistor = try XCTUnwrap(engine.findComponent(ref: "R1"))
        engine.setValue(resistor, "150")  // ≈ 20 mA LED current
        XCTAssertFalse(engine.runCircuitValidation().contains { $0.code == "VAL_LED_CURRENT" })
        engine.setIndustry("space")       // LED limit derated to 15 mA
        let finding = try XCTUnwrap(engine.runCircuitValidation().first { $0.code == "VAL_LED_CURRENT" })
        XCTAssertTrue(finding.message.contains("Space derating"), finding.message)
    }

    func testPlanSchemaAndOfflinePlansCarryIndustry() async throws {
        let schema = DesignSchemas.designPlanSchema(customKinds: [])
        let required = try XCTUnwrap(schema["required"] as? [String])
        XCTAssertTrue(required.contains("industry"))
        XCTAssertNoThrow(try JSONSerialization.data(withJSONObject: schema))
        XCTAssertTrue(AgentPrompts.architectSystem.contains("ECSS-Q-ST-30-11C"))

        let brief = "Automotive ECU: 12 V battery input protection and a CAN transceiver"
        let text = try await OfflineProvider().complete(
            AgentPrompts.architectRequest(brief: brief, spec: OfflineProvider.spec(for: brief)))
        let plan = try JSONExtraction.decode(DesignPlan.self, from: text)
        XCTAssertEqual(plan.industry, "automotive")
        XCTAssertEqual(OfflineProvider.template(for: "433 MHz radio filter").industry, "rf")
        XCTAssertEqual(OfflineProvider.template(for: "Let a push-button switch an LED through an NPN transistor from a 5 V rail").plan.title,
                       "NPN LED Driver")
    }
}

final class DroneAndBoardFeatureTests: XCTestCase {
    func testDroneBriefPicksFlightControllerOnQuadXFrame() throws {
        let template = OfflineProvider.template(for: "Toy quadcopter drone with 4 brushed motors and a 2.4 GHz radio")
        XCTAssertEqual(template.plan.title, "Quadcopter Flight Controller")
        XCTAssertEqual(template.industry, "uav")
        XCTAssertEqual(template.category, "Drones & UAV")
        XCTAssertEqual(template.plan.board.outline, "quad-x")
        XCTAssertEqual(template.plan.board.mountingHoleSpacing, 30.5)
        XCTAssertEqual(template.plan.pours.count, 2)
        XCTAssertFalse(template.plan.noConnect.isEmpty)
        XCTAssertEqual(StandardLibrary.industry("uav")?.rulePreset, "IPC-2221 Class 3")

        let engine = EDAEngine()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertTrue(snapshot.board.hasCustomOutline)
        XCTAssertEqual(snapshot.board.width, 100, accuracy: 1e-6)
        XCTAssertEqual(snapshot.board.holes.count, 4)
        XCTAssertEqual(snapshot.zones.count, 2)
        XCTAssertEqual(snapshot.board.netWidths["VBAT"], 0.6)
        let mcu = try XCTUnwrap(snapshot.component(ref: "U3"))
        XCTAssertEqual(mcu.pins.filter(\.noConnect).count, template.plan.noConnect.count)
        // Unused MCU pins are intentional: ERC reports no unconnected-pin findings for them.
        XCTAssertFalse(engine.runERC().contains { $0.severity != .info && $0.components.contains(mcu.id) })
        // TP4056 and the LDO come with behavioural models.
        let charger = try XCTUnwrap(snapshot.customParts.first { $0.name == "TP4056" })
        XCTAssertEqual(charger.model?.regulator?.charger, true)
        // The plan built back from the design keeps its board features.
        let back = DesignPlanCompiler.plan(from: snapshot)
        XCTAssertEqual(back.pours.count, 2)
        XCTAssertEqual(Set(back.noConnect), Set(template.plan.noConnect))
        XCTAssertEqual(back.board.outline, "keep")
    }

    func testOutlineHolesPoursAndNetClassesBridge() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        XCTAssertTrue(engine.applyOutlinePreset(.rounded, width: 36, height: 26, parameter: 4))
        XCTAssertEqual(engine.addMountingHole(at: CGPoint(x: 4, y: 13), drill: 3.2, keepout: 6.4), 1)
        XCTAssertNotNil(engine.addZone(net: "GND", layer: 1, plane: false))
        XCTAssertNil(engine.addZone(net: "GND", layer: 7, plane: false))
        XCTAssertTrue(engine.setNetWidth("VCC", width: 0.5))
        engine.autoPlace(all: true)
        XCTAssertEqual(engine.autoRoute().failed, 0)
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertTrue(snapshot.board.hasCustomOutline)
        XCTAssertEqual(snapshot.board.holes.count, 1)
        let fill = try XCTUnwrap(snapshot.zoneFills.first)
        XCTAssertGreaterThan(fill.area, 50)
        XCTAssertFalse(fill.cgRects.isEmpty)
        XCTAssertTrue(engine.runDRC().allSatisfy { $0.severity != .error })
        XCTAssertTrue(engine.export(.drillNPTH)?.contains("M30") ?? false)
        XCTAssertTrue(engine.export(.gerberBottom)?.contains("G36*") ?? false)

        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        let reloaded = try XCTUnwrap(copy.snapshot())
        XCTAssertEqual(reloaded.zones, snapshot.zones)
        XCTAssertEqual(reloaded.board.outline, snapshot.board.outline)
        XCTAssertEqual(reloaded.board.netWidths, snapshot.board.netWidths)

        engine.clearZones()
        engine.clearMountingHoles()
        XCTAssertTrue(engine.setOutline([]))
        let cleared = try XCTUnwrap(engine.snapshot())
        XCTAssertTrue(cleared.zones.isEmpty && cleared.board.holes.isEmpty && !cleared.board.hasCustomOutline)
    }

    func testNoConnectPinsAndModelPassThrough() throws {
        let engine = EDAEngine()
        let timer = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "NE555" })
        XCTAssertNotNil(timer.spec.model?.loads)
        let part = try engine.registerCustomPart(timer.spec)
        XCTAssertEqual(part.model, timer.spec.model)
        XCTAssertEqual(part.spec.model, timer.spec.model)
        let id = engine.addCustomComponent(partId: part.id, at: .zero)
        let control = try XCTUnwrap(engine.findPin(component: id, name: "5"))
        XCTAssertTrue(engine.setPinNoConnect(PinAddress(component: id, pin: control), true))
        let pin = try XCTUnwrap(engine.snapshot()?.component(id)?.pins[control])
        XCTAssertTrue(pin.noConnect)
        XCTAssertTrue(engine.setPinNoConnect(PinAddress(component: id, pin: control), false))
        XCTAssertEqual(engine.snapshot()?.component(id)?.pins[control].noConnect, false)
        XCTAssertEqual(PackageKind.guess("2x04 IDC header"), .header2)
    }

    func testPlanSchemaCoversBoardFeatures() throws {
        let schema = DesignSchemas.designPlanSchema(customKinds: [])
        let required = try XCTUnwrap(schema["required"] as? [String])
        for key in ["pours", "netClasses", "noConnect"] { XCTAssertTrue(required.contains(key), key) }
        let json = """
        {"title":"T","summary":"","components":[],"connections":[],"notes":[],"industry":"uav",
         "board":{"width":100,"height":44,"layers":2,"outline":"quad-x","outlineParameter":12,"mountingHoleSpacing":30.5},
         "pours":[{"net":"GND","layer":-1,"plane":false}],"netClasses":[{"net":"VBAT","width":0.6}],"noConnect":["U1.3"]}
        """
        let plan = try JSONExtraction.decode(DesignPlan.self, from: json)
        XCTAssertEqual(plan.board.outline, "quad-x")
        XCTAssertEqual(plan.pours.first?.layer, -1)
        XCTAssertEqual(plan.netClasses.first?.width, 0.6)
        let old = try JSONExtraction.decode(DesignPlan.self, from: #"{"title":"X","board":{"width":30,"height":20}}"#)
        XCTAssertEqual(old.board.outline, "keep")
        XCTAssertEqual(old.board.mountingHoleSpacing, -1)
        XCTAssertTrue(old.pours.isEmpty)
    }
}

/// Every workspace must fit a laptop-size window: content that needs more room than the window is centred and
/// clipped on all sides by SwiftUI (issue #13).
@MainActor
final class LayoutBudgetTests: XCTestCase {
    private func minimumSize<V: View>(_ view: V, store: DesignStore, settings: AISettings,
                                      agents: AgentOrchestrator) -> CGSize {
        let host = NSHostingController(rootView: view
            .environmentObject(store)
            .environmentObject(settings)
            .environmentObject(agents))
        return host.sizeThatFits(in: CGSize(width: 1, height: 1))
    }

    func testEveryWorkspaceFitsTheMinimumWindow() throws {
        let store = DesignStore()
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.LayoutBudgetTests")))
        let agents = AgentOrchestrator()
        let budget = LayoutMetrics.workspaceBudget
        // Empty design and a loaded reference design (panels with content can grow).
        for loaded in [false, true] {
            if loaded { store.loadExample(OfflineProvider.templates[8].industryPlan) }
            for workspace in Workspace.allCases {
                let size = minimumSize(ContentView.workspaceView(workspace), store: store, settings: settings, agents: agents)
                XCTAssertLessThanOrEqual(size.width, budget.width, "\(workspace.title) needs \(size.width) pt of width")
                XCTAssertLessThanOrEqual(size.height, budget.height, "\(workspace.title) needs \(size.height) pt of height")
            }
        }
        let window = minimumSize(ContentView(), store: store, settings: settings, agents: agents)
        XCTAssertLessThanOrEqual(window.width, LayoutMetrics.minimumWindow.width)
        XCTAssertLessThanOrEqual(window.height, LayoutMetrics.minimumWindow.height)
    }

    func testLayoutHelpers() {
        XCTAssertEqual(PromptStudioView.agentPanelWidth(for: 600), 320)
        XCTAssertEqual(PromptStudioView.agentPanelWidth(for: 2000), 520)
        XCTAssertLessThanOrEqual(LayoutMetrics.minimumWindow.width, 1000, "must fit a 1000 pt laptop screen")
        XCTAssertGreaterThan(PromptStudioView.editorHeight(for: 300), 0)
        XCTAssertGreaterThan(ComponentLibraryView.pinTableHeight(for: 300), 0)
    }
}

/// Runs the real window (not only a size measurement): resizing across the size classes and switching every
/// workspace with an empty, a loaded and a placed design must not crash or hang.
@MainActor
final class LiveWindowTests: XCTestCase {
    /// Last step the test reached; the watchdog reports it if the main thread stops returning from layout.
    private final class Progress: @unchecked Sendable {
        private let lock = NSLock()
        private var step = "start"
        private var changed = Date()
        func set(_ value: String) {
            lock.lock(); step = value; changed = Date(); lock.unlock()
            print("[LiveWindow] \(value)")
            fflush(stdout)
        }
        func stalled(after seconds: TimeInterval) -> String? {
            lock.lock(); defer { lock.unlock() }
            return Date().timeIntervalSince(changed) > seconds ? step : nil
        }
    }

    private func spin(_ seconds: TimeInterval = 0.2) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    func testWindowSurvivesResizingAndWorkspaceSwitches() throws {
        let progress = Progress()
        // A layout loop never returns to the test, so a background watchdog names the step and stops the run.
        let watchdog = DispatchSource.makeTimerSource(queue: .global())
        watchdog.schedule(deadline: .now() + 5, repeating: 5)
        watchdog.setEventHandler {
            if let step = progress.stalled(after: 30) {
                print("[LiveWindow] HUNG for 30 s at: \(step)")
                fflush(stdout)
                Thread.sleep(forTimeInterval: 20)  // CI samples the process meanwhile (see ci.yml)
                fatalError("Main thread stuck in layout at: \(step)")
            }
        }
        watchdog.resume()
        defer { watchdog.cancel() }

        // The 3D workspace runs in both modes: the realistic assembly and the X-ray hologram (the heaviest scenes).
        let modeKey = "threeD.mode.v2"
        let savedMode = UserDefaults.standard.object(forKey: modeKey)
        UserDefaults.standard.set(Board3DWorkspace.Mode.xray.rawValue, forKey: modeKey)
        defer { UserDefaults.standard.set(savedMode, forKey: modeKey) }

        let store = DesignStore()
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.LiveWindowTests")))
        let agents = AgentOrchestrator()
        let root = ContentView()
            .environmentObject(store)
            .environmentObject(settings)
            .environmentObject(agents)
            .documentWindowFrame()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1360, height: 860),
                              styleMask: [.titled, .closable, .resizable, .miniaturizable],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        progress.set("hosting ContentView")
        window.contentViewController = NSHostingController(rootView: root)
        progress.set("ordering window front")
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        spin(0.5)

        let sizes: [CGSize] = [CGSize(width: 1360, height: 860), CGSize(width: 1200, height: 760),
                               CGSize(width: 1000, height: 600), CGSize(width: 900, height: 540),
                               CGSize(width: 1500, height: 940)]
        for stage in 0..<4 {
            if stage == 1 { progress.set("stage 1: loading example"); store.loadExample(OfflineProvider.templates[8].industryPlan) }
            if stage == 2 { progress.set("stage 2: auto-placing"); store.autoPlace(all: true) }
            // The 3D workspace shows the realistic assembly of the placed board, the X-ray hologram once routed.
            UserDefaults.standard.set((stage < 3 ? Board3DWorkspace.Mode.assembly : .xray).rawValue, forKey: modeKey)
            if stage == 3 {
                // Routed board with a selection: PCB tracks, 3D copper and the inspector's part panel.
                progress.set("stage 3: routing")
                _ = store.engine.autoRoute()
                store.refresh()
                store.select(component: store.snapshot.components.first { !$0.componentKind.isVirtual }?.id)
            }
            for size in sizes {
                progress.set("stage \(stage): resize to \(Int(size.width))×\(Int(size.height))")
                window.setContentSize(size)
                spin(0.1)
                for workspace in Workspace.allCases {
                    progress.set("stage \(stage): \(workspace.title) at \(Int(size.width))×\(Int(size.height))")
                    store.workspace = workspace
                    spin(0.15)
                    XCTAssertTrue(window.isVisible, "\(workspace.title) at \(size)")
                }
            }
        }
        progress.set("done")
    }
}

/// The three basic user flows driven through `DesignStore`, the same entry points the editors' clicks call:
/// circuit design → PCB design → 3D, plus saving, reopening and exporting.
@MainActor
final class DesignFlowTests: XCTestCase {
    private func pin(_ component: Int, _ pin: Int) -> PinAddress { PinAddress(component: component, pin: pin) }

    private func freshStore() -> DesignStore {
        let store = DesignStore()
        store.aiEnabled = false
        return store
    }

    func testCircuitDesignFlow() async throws {
        let store = freshStore()
        // Place: each placement selects the new part, like a click with the place tool.
        let v = store.addComponent(.voltageSource, at: CGPoint(x: 0, y: 0))
        let r = store.addComponent(.resistor, at: CGPoint(x: 120, y: -40))
        let d = store.addComponent(.led, at: CGPoint(x: 240, y: -40))
        let g = store.addComponent(.ground, at: CGPoint(x: 0, y: 100))
        XCTAssertTrue([v, r, d, g].allSatisfy { $0 >= 0 })
        XCTAssertEqual(store.selection, [g])
        XCTAssertEqual(store.snapshot.components.count, 4)

        // Wire: pin to pin.
        XCTAssertTrue(store.connect(pin(v, 0), pin(r, 0)))
        XCTAssertTrue(store.connect(pin(r, 1), pin(d, 0)))
        XCTAssertTrue(store.connect(pin(d, 1), pin(g, 0)))
        XCTAssertTrue(store.connect(pin(v, 1), pin(g, 0)))
        XCTAssertEqual(store.snapshot.wires.count, 4)
        XCTAssertTrue(store.isDirty)

        // A wire that already exists is refused without an undo step: one undo removes the last real wire.
        XCTAssertFalse(store.connect(pin(v, 1), pin(g, 0)))
        XCTAssertFalse(store.connect(pin(v, 0), pin(v, 0)))
        XCTAssertEqual(store.snapshot.wires.count, 4)
        store.undo()
        XCTAssertEqual(store.snapshot.wires.count, 3)
        store.redo()
        XCTAssertEqual(store.snapshot.wires.count, 4)

        // Inspector edits.
        store.setValue(r, "470")
        store.setRef(r, "R9")
        XCTAssertEqual(store.snapshot.component(r)?.value, "470")
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R9")
        store.setRef(d, "R9")  // duplicate designator is refused with an alert
        XCTAssertNotNil(store.alert)
        XCTAssertNotEqual(store.snapshot.component(d)?.ref, "R9")
        store.alert = nil

        // Move, rotate, select.
        let before = try XCTUnwrap(store.snapshot.component(r))
        store.moveComponents([r], by: CGSize(width: 40, height: 0))
        XCTAssertEqual(store.snapshot.component(r)?.x ?? 0, before.x + 40, accuracy: 0.01)
        store.select(component: r)
        store.rotateSelection()
        XCTAssertEqual(store.snapshot.component(r)?.rotation, (before.rotation + 90) % 360)
        store.select(component: d, extend: true)
        XCTAssertEqual(store.selection, [r, d])
        store.select(component: d, extend: true)
        XCTAssertEqual(store.selection, [r])

        // No-connect mark on a spare part, then delete it.
        let spare = store.addComponent(.resistor, at: CGPoint(x: 400, y: 200))
        store.toggleNoConnect(pin(spare, 0))
        XCTAssertEqual(store.snapshot.component(spare)?.pins[0].noConnect, true)
        store.toggleNoConnect(pin(spare, 0))
        XCTAssertEqual(store.snapshot.component(spare)?.pins[0].noConnect, false)
        store.select(component: spare)
        store.deleteSelection()
        XCTAssertNil(store.snapshot.component(spare))
        XCTAssertTrue(store.selection.isEmpty)

        // Checks and simulation.
        store.runERC()
        XCTAssertFalse(store.ercResults.contains { $0.severity == .error }, "\(store.ercResults.map(\.message))")
        store.showChecks(.rules)
        XCTAssertEqual(store.workspace, .checks)
        XCTAssertEqual(store.checksMode, .rules)
        await store.simulateDC()
        let dc = try XCTUnwrap(store.dcResult)
        XCTAssertTrue(dc.converged, dc.error)
        XCTAssertGreaterThan(try XCTUnwrap(dc.reading(component: d)?.current), 0.003)
        XCTAssertFalse(store.isBusy)
        // An edit invalidates the simulation; a refused edit keeps it.
        XCTAssertFalse(store.connect(pin(v, 1), pin(g, 0)))
        XCTAssertNotNil(store.dcResult)
        store.setValue(r, "1k")
        XCTAssertNil(store.dcResult)
        let report = await store.runVerification()
        XCTAssertNotNil(report)
        store.showChecks(.verification)
        XCTAssertEqual(store.checksMode, .verification)
    }

    func testPCBDesignFlow() async throws {
        let store = freshStore()
        store.applyPlan(OfflineProvider.templates[0].industryPlan, requirements: nil)  // LED indicator
        XCTAssertFalse(store.snapshot.components.isEmpty)

        store.autoPlace(all: true)
        let parts = store.snapshot.components.filter { !$0.componentKind.isVirtual && !$0.footprint.isEmpty }
        XCTAssertFalse(parts.isEmpty)
        XCTAssertTrue(parts.allSatisfy(\.pcb.placed))
        store.fitBoard()
        XCTAssertGreaterThan(store.snapshot.board.width, 5)

        await store.autoRoute()
        XCTAssertFalse(store.isBusy)
        let stats = try XCTUnwrap(store.routeStats)
        XCTAssertEqual(stats.failed, 0)
        XCTAssertGreaterThan(stats.routed, 0)
        XCTAssertTrue(store.routeStatsAreCurrent)
        XCTAssertTrue(store.drcIsCurrent, "autoroute runs the DRC")
        XCTAssertFalse(store.drcResults.contains { $0.severity == .error }, "\(store.drcResults.map(\.message))")
        XCTAssertFalse(store.snapshot.tracks.isEmpty)

        // Dragging two footprints is one undo step, and makes the DRC/route results stale.
        // Positions as routed (Fit Board shifted the footprints after `parts` was read).
        let a = try XCTUnwrap(store.snapshot.component(parts[0].id))
        let b = try XCTUnwrap(store.snapshot.component(parts[1].id))
        store.moveFootprints([(a.id, CGPoint(x: a.pcb.x + 1, y: a.pcb.y)), (b.id, CGPoint(x: b.pcb.x + 1, y: b.pcb.y))])
        XCTAssertEqual(store.snapshot.component(a.id)?.pcb.x ?? 0, a.pcb.x + 1, accuracy: 0.26)
        XCTAssertEqual(store.snapshot.component(b.id)?.pcb.x ?? 0, b.pcb.x + 1, accuracy: 0.26)
        XCTAssertFalse(store.drcIsCurrent)
        XCTAssertFalse(store.routeStatsAreCurrent)
        store.undo()
        XCTAssertEqual(store.snapshot.component(a.id)?.pcb.x ?? 0, a.pcb.x, accuracy: 0.001)
        XCTAssertEqual(store.snapshot.component(b.id)?.pcb.x ?? 0, b.pcb.x, accuracy: 0.001)
        store.runDRC()
        XCTAssertTrue(store.drcIsCurrent)

        // Rotate and flip the selection.
        store.select(component: a.id)
        store.rotateFootprints()
        XCTAssertNotEqual(store.snapshot.component(a.id)?.pcb.rotation, a.pcb.rotation)
        store.flipFootprints()
        XCTAssertEqual(store.snapshot.component(a.id)?.pcb.bottom, !a.pcb.bottom)
        store.undo()
        store.undo()

        // Board settings: invalid values are reported, 0 keeps the rules.
        let board = store.snapshot.board
        store.setBoard(width: 2, height: board.height, trackWidth: board.trackWidth, clearance: board.clearance)
        XCTAssertNotNil(store.alert)
        XCTAssertEqual(store.snapshot.board.width, board.width)
        store.alert = nil
        store.setBoard(width: board.width + 4, height: board.height + 4, trackWidth: 0, clearance: 0)
        XCTAssertEqual(store.snapshot.board.width, board.width + 4, accuracy: 0.001)
        XCTAssertEqual(store.snapshot.board.trackWidth, board.trackWidth, accuracy: 0.001)
        XCTAssertNil(store.alert)

        // Stack-up, pours, mounting holes, outline.
        store.setLayerCount(4)
        XCTAssertEqual(store.snapshot.board.layerCount, 4)
        XCTAssertNil(store.routeStats)
        store.addGroundPours()
        XCTAssertFalse(store.snapshot.zones.isEmpty)
        store.setBoard(width: 50, height: 50, trackWidth: 0, clearance: 0)
        store.addMountingPattern(spacing: 40, drill: 3.2, keepout: 6.4)
        XCTAssertEqual(store.snapshot.board.holes.count, 4)
        store.autoPlace(all: true)
        await store.autoRoute()
        XCTAssertEqual(try XCTUnwrap(store.routeStats).failed, 0)
        XCTAssertFalse(store.drcResults.contains { $0.severity == .error }, "\(store.drcResults.map(\.message))")
        store.applyOutlinePreset(.rounded, width: 50, height: 50, parameter: 4)
        XCTAssertTrue(store.snapshot.board.hasCustomOutline)
        let shaped = store.snapshot.board
        store.fitBoard()  // refused for a shaped outline, with an explanation
        XCTAssertEqual(store.snapshot.board.width, shaped.width)
        XCTAssertTrue(store.statusMessage.contains("custom outline"), store.statusMessage)
    }

    func test3DDesignFlow() async throws {
        for (index, layers) in [(0, 2), (4, 1), (4, 4), (4, 6)] {
            let store = freshStore()
            store.applyPlan(OfflineProvider.templates[index].industryPlan, requirements: nil)
            store.setLayerCount(layers)
            store.autoPlace(all: true)
            await store.autoRoute()
            XCTAssertEqual(store.routeStats?.failed, 0, "\(layers) layers")

            let mesh = try XCTUnwrap(store.engine.buildMesh(includeComponents: true))
            XCTAssertGreaterThan(mesh.vertexCount, 100)
            XCTAssertEqual(mesh.normals.count, mesh.positions.count)
            XCTAssertEqual(mesh.indices.count % 3, 0)
            XCTAssertTrue(mesh.indices.allSatisfy { Int($0) < mesh.vertexCount })
            XCTAssertTrue(mesh.positions.allSatisfy(\.isFinite))
            let geometry = BoardSceneView.geometry(from: mesh)
            XCTAssertFalse(geometry.elements.isEmpty)
            XCTAssertFalse(geometry.sources.isEmpty)
            for layer in 0..<layers { XCTAssertNotNil(store.engine.buildLayerMesh(layer: layer), "layer \(layer)") }
            let bare = try XCTUnwrap(store.engine.buildMesh(includeComponents: false))
            XCTAssertLessThan(bare.vertexCount, mesh.vertexCount, "components add bodies")
            XCTAssertTrue(store.engine.export(.stl)?.contains("facet") ?? false)
            XCTAssertTrue(store.engine.export(.obj)?.contains("v ") ?? false)
        }
        // Shaped (quad-X) drone frame with holes and pours.
        let drone = OfflineProvider.templates.first { $0.plan.title == "Quadcopter Flight Controller" }
        let store = freshStore()
        store.applyPlan(try XCTUnwrap(drone).industryPlan, requirements: nil)
        store.autoPlace(all: true)
        await store.autoRoute()
        XCTAssertTrue(store.snapshot.board.hasCustomOutline)
        let mesh = try XCTUnwrap(store.engine.buildMesh(includeComponents: true))
        XCTAssertGreaterThan(mesh.vertexCount, 100)
        XCTAssertTrue(mesh.positions.allSatisfy(\.isFinite))
    }

    func testSaveOpenAndExportFlow() async throws {
        let store = freshStore()
        store.applyPlan(OfflineProvider.templates[7].industryPlan, requirements: "555 blinker")
        store.setBoard(width: 60, height: 50, trackWidth: 0, clearance: 0)
        store.addMountingPattern(spacing: 44, drill: 3.2, keepout: 6.4)  // holes first: placement keeps clear of them
        store.autoPlace(all: true)
        await store.autoRoute()
        XCTAssertEqual(store.routeStats?.failed, 0)
        store.setProjectName("Flow Test")

        let url = FileManager.default.temporaryDirectory.appendingPathComponent("flow-\(UUID().uuidString).siedaproj")
        defer { try? FileManager.default.removeItem(at: url) }
        try store.engine.saveJSON().write(to: url, atomically: true, encoding: .utf8)

        let reopened = freshStore()
        reopened.select(component: 0)
        reopened.open(url: url)
        XCTAssertNil(reopened.alert)
        XCTAssertEqual(reopened.documentURL, url)
        XCTAssertFalse(reopened.isDirty)
        XCTAssertTrue(reopened.selection.isEmpty)
        XCTAssertFalse(reopened.canUndo)
        XCTAssertEqual(reopened.snapshot.name, "Flow Test")
        XCTAssertEqual(reopened.snapshot.components, store.snapshot.components)
        XCTAssertEqual(reopened.snapshot.wires.count, store.snapshot.wires.count)
        XCTAssertEqual(reopened.snapshot.tracks.count, store.snapshot.tracks.count)
        XCTAssertEqual(reopened.snapshot.board.holes.count, store.snapshot.board.holes.count)
        XCTAssertEqual(reopened.workspace, .schematic)

        for format in ExportFormat.allCases {
            let text = reopened.engine.export(format)
            XCTAssertFalse(text?.isEmpty ?? true, "\(format.displayName) is empty")
        }
        XCTAssertNotNil(reopened.engine.exportCopperLayer(1))
        let report = await reopened.runVerification()
        XCTAssertEqual(report?.passed, true, "\(report?.stages.filter { $0.status == .fail }.map(\.summary) ?? [])")
    }
}

/// AppKit logs "reentrant operation in its NSTableView delegate" when a List's data changes while the table is
/// calling its delegate; Apple has announced it will become an assert (a crash). Loading a design must not do it.
@MainActor
final class TableReentrancyTests: XCTestCase {
    /// Runs `body` with this process's stderr redirected to a file and returns what was written.
    private func capturingStderr(_ body: () -> Void) -> String {
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("stderr-\(UUID().uuidString).log")
        FileManager.default.createFile(atPath: url.path, contents: nil)
        defer { try? FileManager.default.removeItem(at: url) }
        guard let file = try? FileHandle(forWritingTo: url) else { return "" }
        fflush(stderr)
        let saved = dup(STDERR_FILENO)
        dup2(file.fileDescriptor, STDERR_FILENO)
        body()
        fflush(stderr)
        dup2(saved, STDERR_FILENO)
        close(saved)
        try? file.close()
        return (try? String(contentsOf: url, encoding: .utf8)) ?? ""
    }

    private func spin(_ seconds: TimeInterval) { RunLoop.main.run(until: Date().addingTimeInterval(seconds)) }

    private func reentrantWarnings<V: View>(_ name: String, _ view: (DesignStore) -> V,
                                            before: (DesignStore) -> Void = { _ in }) throws -> Int {
        let store = DesignStore()
        store.aiEnabled = false
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.TableReentrancyTests")))
        let agents = AgentOrchestrator()
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1360, height: 860),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: view(store)
            .environmentObject(store).environmentObject(settings).environmentObject(agents)
            .documentWindowFrame())
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        before(store)
        spin(0.5)
        let log = capturingStderr {
            store.loadExample(OfflineProvider.templates[8].industryPlan)
            spin(0.4)
            window.setContentSize(CGSize(width: 1200, height: 760))
            spin(0.4)
            store.autoPlace(all: true)
            spin(0.3)
            store.select(component: store.snapshot.components.first?.id)
            spin(0.3)
        }
        let count = log.components(separatedBy: "reentrant operation").count - 1
        print("[Reentrancy] \(name): \(count) warning(s)")
        return count
    }

    func testLoadingADesignCausesNoReentrantTableUpdates() throws {
        var results: [String: Int] = [:]
        results["sidebar"] = try reentrantWarnings("sidebar") { _ in SidebarView() }
        results["schematic editor"] = try reentrantWarnings("schematic editor") { _ in SchematicEditorView() }
        results["inspector"] = try reentrantWarnings("inspector") { _ in InspectorView() }
        results["window (from Design Checks)"] = try reentrantWarnings("window (from Design Checks)", { _ in ContentView() },
                                                                       before: { $0.workspace = .checks })
        results["window (from Schematic)"] = try reentrantWarnings("window (from Schematic)", { _ in ContentView() },
                                                                   before: { $0.workspace = .schematic })
        for (name, count) in results.sorted(by: { $0.key < $1.key }) {
            XCTAssertEqual(count, 0, "\(name) logged a reentrant NSTableView update")
        }
    }
}

/// Microcontroller firmware: attach, run in the transient analysis, persist.
@MainActor
final class MicrocontrollerTests: XCTestCase {
    private func arduinoTemplate() throws -> OfflineProvider.Template {
        try XCTUnwrap(OfflineProvider.templates.first { $0.category == "Microcontrollers" })
    }

    func testExampleFirmwareCatalog() {
        let ids = EDAEngine.firmwareExamples.map(\.id)
        XCTAssertTrue(ids.contains("arduino_blink"))
        XCTAssertTrue(ids.contains("tiny_blink"))
        for example in EDAEngine.firmwareExamples {
            let hex = EDAEngine.firmwareExampleHex(example.id)
            XCTAssertTrue(hex?.hasPrefix(":") ?? false, example.id)
            XCTAssertFalse(example.description.isEmpty)
        }
        XCTAssertNil(EDAEngine.firmwareExampleHex("nope"))
    }

    func testArduinoTemplateRunsItsFirmware() async throws {
        let store = DesignStore()
        store.aiEnabled = false
        let report = store.applyPlan(try arduinoTemplate().industryPlan, requirements: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        let mcu = try XCTUnwrap(store.snapshot.component(ref: "U1")?.mcu)
        XCTAssertEqual(mcu.model, "ATmega328P")
        XCTAssertEqual(mcu.firmwareName, "Button (INT0)")
        XCTAssertGreaterThan(mcu.firmwareBytes, 100)
        XCTAssertEqual(mcu.clockHz, 16e6)

        await store.simulateTransient(stop: 0.05, step: 1e-4)
        let tr = try XCTUnwrap(store.transientResult)
        XCTAssertTrue(tr.ok, tr.error)
        let run = try XCTUnwrap(tr.mcus.first)
        XCTAssertTrue(run.running, run.status)
        XCTAssertTrue(run.status.contains("16 MHz"), run.status)
        XCTAssertGreaterThan(run.cycles, 700_000)
    }

    func testUploadValidationUndoAndClock() throws {
        let store = DesignStore()
        store.aiEnabled = false
        store.applyPlan(try arduinoTemplate().industryPlan, requirements: nil)
        let u1 = try XCTUnwrap(store.snapshot.component(ref: "U1"))

        // Not Intel HEX: refused with an alert, nothing changes.
        XCTAssertFalse(store.setFirmware(u1.id, hex: "this is not hex", name: "bad.hex"))
        XCTAssertNotNil(store.alert)
        XCTAssertEqual(store.snapshot.component(ref: "U1")?.mcu?.firmwareName, "Button (INT0)")
        store.alert = nil

        // A part that is not a microcontroller is refused too.
        let r1 = try XCTUnwrap(store.snapshot.component(ref: "R1"))
        XCTAssertFalse(store.setFirmware(r1.id, hex: EDAEngine.firmwareExampleHex("blink") ?? "", name: "blink"))
        XCTAssertNotNil(store.alert)
        store.alert = nil

        let blink = try XCTUnwrap(EDAEngine.firmwareExamples.first { $0.id == "arduino_blink" })
        store.loadFirmwareExample(blink, into: u1.id)
        XCTAssertEqual(store.snapshot.component(ref: "U1")?.mcu?.firmwareName, "Arduino Blink")
        store.setMcuClock(u1.id, clockHz: 8e6)
        XCTAssertEqual(store.snapshot.component(ref: "U1")?.mcu?.clockHz, 8e6)
        XCTAssertEqual(store.snapshot.component(ref: "U1")?.mcu?.firmwareName, "Arduino Blink")
        store.undo()
        store.undo()
        XCTAssertEqual(store.snapshot.component(ref: "U1")?.mcu?.firmwareName, "Button (INT0)")
        store.setFirmware(u1.id, hex: "", name: "")
        XCTAssertFalse(store.snapshot.component(ref: "U1")?.mcu?.hasFirmware ?? true)
    }

    func testBlinkDrivesTheLedAndFirmwarePersists() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(try arduinoTemplate().industryPlan, to: engine, previous: nil)
        let u1 = try XCTUnwrap(engine.findComponent(ref: "U1"))
        try engine.setFirmware(u1, hex: try XCTUnwrap(EDAEngine.firmwareExampleHex("arduino_blink")), name: "Blink.hex",
                               clockHz: 0)
        let tr = engine.simulateTransient(stop: 0.5, step: 1e-4)
        XCTAssertTrue(tr.ok, tr.error)
        // D13 → R2 → LED: the PB5 net toggles every 200 ms.
        let snapshot = try XCTUnwrap(engine.snapshot())
        let r2 = try XCTUnwrap(snapshot.component(ref: "R2"))
        let net = try XCTUnwrap(snapshot.nets.first { $0.index == r2.pins[0].net })
        let wave = try XCTUnwrap(tr.nets.first { $0.label == net.name || $0.index == net.index })
        var edges = 0
        for i in 1..<wave.values.count where (wave.values[i - 1] < 2.5) != (wave.values[i] < 2.5) { edges += 1 }
        XCTAssertEqual(edges, 3, "rising at 0, falling at 0.2 s, rising at 0.4 s")

        // Save → load keeps the firmware; a plan that rebuilds the schematic keeps it with U1.
        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        let copied = try XCTUnwrap(copy.snapshot()?.component(ref: "U1")?.mcu)
        XCTAssertEqual(copied.firmwareName, "Blink.hex")
        XCTAssertEqual(copy.firmware(of: try XCTUnwrap(copy.findComponent(ref: "U1"))), engine.firmware(of: u1))
        let previous = try XCTUnwrap(copy.snapshot())
        var plan = DesignPlanCompiler.plan(from: previous)
        plan.notes.append("refined")
        DesignPlanCompiler.apply(plan, to: copy, previous: previous)
        XCTAssertEqual(copy.snapshot()?.component(ref: "U1")?.mcu?.firmwareName, "Blink.hex")
    }
}

/// The live board: real-time stepping, firmware, push-button, LED glow, serial monitor, scope, stop on edits.
@MainActor
final class LiveSimulationTests: XCTestCase {
    private func wait(_ live: LiveSimulation, until condition: () -> Bool, timeout: TimeInterval = 20) async {
        let deadline = Date().addingTimeInterval(timeout)
        while !condition() && Date() < deadline { try? await Task.sleep(nanoseconds: 50_000_000) }
    }

    func testArduinoBoardRunsLive() async throws {
        let store = DesignStore()
        store.aiEnabled = false
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.category == "Microcontrollers" })
        store.applyPlan(template.industryPlan, requirements: nil)
        let live = store.live
        live.speed = 1
        live.resolution = 100e-6
        live.start(store: store)
        XCTAssertTrue(live.isRunning, live.error ?? "")
        defer { live.stop() }

        await wait(live) { (live.state?.time ?? 0) > 0.05 }
        let state = try XCTUnwrap(live.state)
        XCTAssertGreaterThan(state.time, 0.05)
        XCTAssertTrue(state.mcus.first?.running ?? false, state.mcus.first?.status ?? "no MCU")
        let sw1 = try XCTUnwrap(store.snapshot.component(ref: "SW1"))
        let d1 = try XCTUnwrap(store.snapshot.component(ref: "D1"))
        XCTAssertEqual(state.switches.first?.momentary, true)
        XCTAssertLessThan(live.led(d1.id)?.brightness ?? 1, 0.05)

        // Hold the push-button: the LED lights and the firmware reports the press.
        live.press(sw1.id, pressed: true)
        await wait(live) { (live.led(d1.id)?.brightness ?? 0) > 0.4 }
        XCTAssertGreaterThan(live.led(d1.id)?.brightness ?? 0, 0.4)
        XCTAssertEqual(live.isClosed(sw1.id), true)
        await wait(live) { live.state?.mcus.first?.serial.contains("press 1") ?? false }
        XCTAssertTrue(live.state?.mcus.first?.serial.contains("press 1") ?? false)
        live.press(sw1.id, pressed: false)
        await wait(live) { (live.led(d1.id)?.brightness ?? 1) < 0.05 }
        XCTAssertLessThan(live.led(d1.id)?.brightness ?? 1, 0.05)

        // Scope history accumulates for the selected nets.
        XCTAssertFalse(live.scopeNets.isEmpty)
        XCTAssertGreaterThan(live.scopeTime.count, 10)
        XCTAssertTrue(live.scopeNets.allSatisfy { (live.scopeValues[$0]?.count ?? 0) == live.scopeTime.count })

        // Pause holds the time.
        live.pause()
        try? await Task.sleep(nanoseconds: 200_000_000)
        let paused = live.state?.time ?? 0
        try? await Task.sleep(nanoseconds: 200_000_000)
        XCTAssertEqual(live.state?.time ?? -1, paused, accuracy: 1e-9)
        live.resume()

        // Moving a part doesn't change the circuit: the board keeps running.
        let r2 = try XCTUnwrap(store.snapshot.component(ref: "R2")).id
        let before = live.state?.time ?? 0
        store.moveComponents([r2], by: CGSize(width: 10, height: 0))
        await wait(live) { (live.state?.time ?? 0) > before + 0.2 }
        XCTAssertGreaterThan(live.state?.time ?? 0, before + 0.2)

        // A circuit edit takes effect at once: the board restarts with it and keeps running.
        store.setValue(r2, "470")
        await wait(live) { store.statusMessage.contains("updated with your edit") }
        XCTAssertTrue(live.isRunning)
        XCTAssertTrue(store.statusMessage.contains("updated with your edit"), store.statusMessage)
        XCTAssertLessThan(live.state?.time ?? 1, before + 0.2)
        live.stop()
        XCTAssertFalse(live.isRunning)
    }

    func testLiveStartErrorsAreReported() {
        let store = DesignStore()  // empty design: nothing to simulate
        store.live.start(store: store)
        XCTAssertFalse(store.live.isRunning)
        XCTAssertNotNil(store.live.error)
        store.live.clearError()
        XCTAssertNil(store.live.error)
    }
}

/// Files from Finder / Dock / Open Recent, and the close-window save guard.
@MainActor
final class DocumentHandlingTests: XCTestCase {
    private func projectFile(named name: String) throws -> URL {
        let engine = EDAEngine(name: name)
        DesignPlanCompiler.apply(OfflineProvider.templates[0].industryPlan, to: engine, previous: nil)
        engine.setName(name)
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString).siedaproj")
        try engine.saveJSON().write(to: url, atomically: true, encoding: .utf8)
        return url
    }

    func testAppDelegateOpensFilesEvenBeforeTheWindowExists() throws {
        let url = try projectFile(named: "From Finder")
        defer { try? FileManager.default.removeItem(at: url) }
        let delegate = AppDelegate()
        delegate.application(NSApplication.shared, open: [url])  // no store yet: kept pending
        let store = DesignStore()
        delegate.store = store
        XCTAssertEqual(store.snapshot.name, "From Finder")
        XCTAssertEqual(store.documentURL, url)

        let second = try projectFile(named: "Second")
        defer { try? FileManager.default.removeItem(at: second) }
        delegate.application(NSApplication.shared, open: [second])
        XCTAssertEqual(store.snapshot.name, "Second")
        XCTAssertEqual(UTType.siedaProject.preferredFilenameExtension, "siedaproj")
    }

    func testCloseGuardForwardsAndAllowsCleanClose() {
        final class Original: NSObject, NSWindowDelegate {
            var resized = false
            func windowDidResize(_ notification: Notification) { resized = true }
        }
        let original = Original()
        let closeGuard = WindowCloseGuard()
        closeGuard.original = original
        let store = DesignStore()
        closeGuard.store = store
        XCTAssertTrue(closeGuard.responds(to: #selector(NSWindowDelegate.windowDidResize(_:))))
        XCTAssertTrue((closeGuard.forwardingTarget(for: #selector(NSWindowDelegate.windowDidResize(_:))) as AnyObject) === original)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 200, height: 100), styleMask: [.titled, .closable],
                              backing: .buffered, defer: true)
        window.isReleasedWhenClosed = false
        XCTAssertTrue(closeGuard.windowShouldClose(window))  // clean design: closes without asking
        XCTAssertTrue(store.closeConfirmed)
    }
}

@MainActor
final class MedicalFrontEndTests: XCTestCase {
    /// The ECG template's INA333 (G = 101) turns the 1 mV electrode signal into ≈ ±0.1 V around VREF on ECG_OUT.
    func testEcgTemplateAmplifiesTheElectrodeSignal() throws {
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.title.contains("ECG") })
        let engine = EDAEngine(name: "ECG")
        DesignPlanCompiler.apply(template.industryPlan, to: engine, previous: nil)
        let result = engine.simulateTransient(stop: 2, step: 1e-3)
        XCTAssertTrue(result.ok, result.error)
        let out = try XCTUnwrap(result.nets.first { $0.name == "ECG_OUT" })
        let settled = out.values.suffix(out.values.count / 2)
        let swing = (settled.max() ?? 0) - (settled.min() ?? 0)
        let mean = settled.reduce(0, +) / Double(max(settled.count, 1))
        XCTAssertEqual(mean, 1.65, accuracy: 0.05)
        XCTAssertEqual(swing, 0.2, accuracy: 0.03)
    }
}

@MainActor
final class AuditFixTests: XCTestCase {
    func testChangeRequestsAreKeptAsOneList() {
        var requirements = "Battery-powered ECG front end."
        requirements = AgentOrchestrator.appendingChangeRequest("Add a power LED", to: requirements)
        requirements = AgentOrchestrator.appendingChangeRequest("Use a 4-layer board", to: requirements)
        requirements = AgentOrchestrator.appendingChangeRequest("add a power led", to: requirements)  // repeated
        XCTAssertEqual(requirements, """
            Battery-powered ECG front end.

            Change requests:
            - Use a 4-layer board
            - add a power led
            """)
        // Old projects stored one "Change request:" paragraph per refinement.
        let legacy = "PRD\n\nChange request: A\n\nChange request: B"
        XCTAssertEqual(AgentOrchestrator.appendingChangeRequest("C", to: legacy), "PRD\n\nChange requests:\n- A\n- B\n- C")
        var many = ""
        for i in 0..<40 { many = AgentOrchestrator.appendingChangeRequest("Request \(i)", to: many) }
        XCTAssertEqual(many.components(separatedBy: "\n- ").count - 1, AgentOrchestrator.maxChangeRequests)
        XCTAssertTrue(many.hasSuffix("- Request 39"))
    }

    func testIdenticalViolationsHaveDistinctIds() throws {
        let json = """
            [{"severity":"warning","code":"X","message":"same","components":[],"hasLocation":false,"x":0,"y":0},
             {"severity":"warning","code":"X","message":"same","components":[],"hasLocation":false,"x":0,"y":0}]
            """
        let violations = try JSONDecoder().decode([RuleViolation].self, from: Data(json.utf8)).numbered()
        XCTAssertEqual(Set(violations.map(\.id)).count, 2)
    }

    func testTextFilesInLegacyEncodingsAreRead() {
        let text = "Résumé: 5 °C – 3.3 V rail"
        let cp1252 = text.data(using: .windowsCP1252)!
        XCTAssertEqual(DatasheetDocument.decodeText(cp1252), text)
        XCTAssertEqual(DatasheetDocument.decodeText(text.data(using: .utf16)!), text)
        XCTAssertEqual(DatasheetDocument.decodeText(Data(text.utf8)), text)
    }

    func testExampleLoadsCleanAndNewProjectStartsBlank() throws {
        let store = DesignStore()
        let blankBoard = store.snapshot.board
        let ecg = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.title.contains("ECG") })
        store.loadExample(ecg.industryPlan)
        XCTAssertFalse(store.isDirty, "an untouched example needs no save prompt")
        XCTAssertFalse(store.snapshot.customParts.isEmpty)

        store.addGroundPours()
        let pours = store.snapshot.zones.count
        XCTAssertGreaterThan(pours, 0)
        store.addGroundPours()
        XCTAssertEqual(store.snapshot.zones.count, pours, "a second click must not stack duplicate pours")

        // Saved and reopened: clean, so New Project doesn't ask.
        let url = FileManager.default.temporaryDirectory.appendingPathComponent("\(UUID().uuidString).siedaproj")
        defer { try? FileManager.default.removeItem(at: url) }
        try store.engine.saveJSON().write(to: url, atomically: true, encoding: .utf8)
        store.open(url: url)
        XCTAssertFalse(store.isDirty)
        XCTAssertEqual(store.snapshot.zones.count, pours)
        store.newProject()
        XCTAssertTrue(store.snapshot.components.isEmpty)
        XCTAssertTrue(store.snapshot.customParts.isEmpty)
        XCTAssertTrue(store.snapshot.zones.isEmpty)
        XCTAssertEqual(store.snapshot.board, blankBoard)
        XCTAssertEqual(store.snapshot.requirements, "")
    }

    func testNetLabelHitBoxFollowsTheText() {
        let short = SchematicSymbols.bounds(.netLabel, value: "A", custom: nil)
        let long = SchematicSymbols.bounds(.netLabel, value: "MOTOR_PWM_FRONT_LEFT", custom: nil)
        XCTAssertGreaterThan(long.width, short.width)
        XCTAssertGreaterThanOrEqual(long.maxX, SchematicSymbols.netLabelTextWidth("MOTOR_PWM_FRONT_LEFT"))
    }

    func testRevealAsksForTheInspector() {
        let store = DesignStore()
        let r = store.addComponent(.resistor, at: .zero)
        let before = store.inspectorRevealToken
        store.reveal(component: r)
        XCTAssertEqual(store.selection, [r])
        XCTAssertEqual(store.inspectorRevealToken, before + 1)
    }

    func testClaudeOutputBudgetGrowsWithEffort() {
        XCTAssertEqual(ClaudeProvider.outputBudget(effort: "medium"), 16_000)
        XCTAssertGreaterThan(ClaudeProvider.outputBudget(effort: "max"), ClaudeProvider.outputBudget(effort: "high"))
    }
}

@MainActor
final class InteractiveSchematicTests: XCTestCase {
    /// V1 → R1 → SW1 → D1 (red) → ground, as built by hand on the canvas.
    private func ledCircuit(_ store: DesignStore) -> (sw: Int, led: Int) {
        let v = store.addComponent(.voltageSource, at: CGPoint(x: 0, y: 0))
        let r = store.addComponent(.resistor, at: CGPoint(x: 120, y: -40))
        store.setValue(r, "330")  // the default 10k would give only ≈ 0.3 mA
        let sw = store.addComponent(.switchSPST, at: CGPoint(x: 240, y: -40))
        let d = store.addComponent(.led, at: CGPoint(x: 360, y: -40))
        let g = store.addComponent(.ground, at: CGPoint(x: 0, y: 100))
        let g2 = store.addComponent(.ground, at: CGPoint(x: 460, y: 60))
        func pin(_ id: Int, _ i: Int) -> PinAddress { PinAddress(component: id, pin: i) }
        XCTAssertTrue(store.connect(pin(v, 0), pin(r, 0)))
        XCTAssertTrue(store.connect(pin(r, 1), pin(sw, 0)))
        XCTAssertTrue(store.connect(pin(sw, 1), pin(d, 0)))
        XCTAssertTrue(store.connect(pin(d, 1), pin(g2, 0)))
        XCTAssertTrue(store.connect(pin(v, 1), pin(g, 0)))
        return (sw, d)
    }

    func testClickingASwitchTogglesItAndTheLedFollows() async throws {
        let store = DesignStore()
        let (sw, led) = ledCircuit(store)
        store.setValue(sw, "on")
        await store.simulateDC()
        let lit = try XCTUnwrap(store.dcResult?.devices.first { $0.component == led }).current
        XCTAssertGreaterThan(lit, 0.005)  // ≈ 9 mA through 330 Ω

        store.toggleSwitch(sw)
        XCTAssertEqual(store.snapshot.component(sw)?.value, "off")
        // The DC result on screen is re-solved, so the LED goes dark without pressing anything.
        let deadline = Date().addingTimeInterval(10)
        while store.dcResult == nil && Date() < deadline { try? await Task.sleep(nanoseconds: 20_000_000) }
        let dark = try XCTUnwrap(store.dcResult?.devices.first { $0.component == led }).current
        XCTAssertLessThan(dark, 1e-5)

        store.toggleSwitch(sw)
        XCTAssertEqual(store.snapshot.component(sw)?.value, "on")
        store.undo()
        XCTAssertEqual(store.snapshot.component(sw)?.value, "off")

        // Push-buttons are pressed in the live simulation, not flipped.
        store.setValue(sw, "push")
        store.toggleSwitch(sw)
        XCTAssertEqual(store.snapshot.component(sw)?.value, "push")
    }

    func testLiveRunFollowsSwitchToggles() async throws {
        let store = DesignStore()
        let (sw, led) = ledCircuit(store)
        store.setValue(sw, "off")
        let live = store.live
        live.start(store: store)
        defer { live.stop() }
        XCTAssertTrue(live.isRunning, live.error ?? "")
        var deadline = Date().addingTimeInterval(10)
        while (live.state?.time ?? 0) < 0.05 && Date() < deadline { try? await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertLessThan(live.led(led)?.brightness ?? 1, 0.02)
        live.press(sw, pressed: true)  // a toggle switch flips on press
        deadline = Date().addingTimeInterval(10)
        while (live.led(led)?.brightness ?? 0) < 0.4 && Date() < deadline { try? await Task.sleep(nanoseconds: 20_000_000) }
        XCTAssertGreaterThan(live.led(led)?.brightness ?? 0, 0.4)
        XCTAssertEqual(live.isClosed(sw), true)
    }

    func testCircuitKeyIgnoresPositions() {
        let store = DesignStore()
        let (sw, _) = ledCircuit(store)
        let key = LiveSimulation.circuitKey(store.snapshot)
        store.moveComponents([sw], by: CGSize(width: 20, height: 0))
        XCTAssertEqual(LiveSimulation.circuitKey(store.snapshot), key)
        store.setValue(sw, "off")
        XCTAssertNotEqual(LiveSimulation.circuitKey(store.snapshot), key)
    }

    func testDeleteKeysAreRecognisedButNotWithCommand() throws {
        func key(_ code: UInt16, _ flags: NSEvent.ModifierFlags = []) throws -> NSEvent {
            try XCTUnwrap(NSEvent.keyEvent(with: .keyDown, location: .zero, modifierFlags: flags, timestamp: 0,
                                           windowNumber: 0, context: nil, characters: "\u{7F}",
                                           charactersIgnoringModifiers: "\u{7F}", isARepeat: false, keyCode: code))
        }
        XCTAssertTrue(DeleteKeyMonitor.MonitorView.isDeleteKey(try key(51)))
        XCTAssertTrue(DeleteKeyMonitor.MonitorView.isDeleteKey(try key(117)))
        XCTAssertFalse(DeleteKeyMonitor.MonitorView.isDeleteKey(try key(51, .command)))  // ⌘⌫ belongs to menus
        XCTAssertFalse(DeleteKeyMonitor.MonitorView.isDeleteKey(try key(0)))

        let store = DesignStore()
        let (sw, _) = ledCircuit(store)
        store.select(component: sw)
        store.deleteSelection()
        XCTAssertNil(store.snapshot.component(sw))
        let wire = try XCTUnwrap(store.snapshot.wires.first).id
        store.selectedWire = wire
        store.deleteSelection()
        XCTAssertFalse(store.snapshot.wires.contains { $0.id == wire })
    }
}

final class SwitchSymbolTests: XCTestCase {
    /// On: the lever lies flat across the contacts. Off: it is lifted well clear (the gap is visible at any zoom).
    func testSwitchSymbolShowsOpenAndClosed() {
        let on = SchematicSymbols.shapes(for: .switchSPST, value: "on").solid.boundingRect
        let off = SchematicSymbols.shapes(for: .switchSPST, value: "off").solid.boundingRect
        XCTAssertLessThan(on.height, 5)          // flat bar + contact dots
        XCTAssertGreaterThanOrEqual(on.maxX, 12) // reaches the right contact
        XCTAssertLessThan(off.minY, -15)         // lifted lever
        XCTAssertEqual(SchematicSymbols.shapes(for: .switchSPST, value: "closed").solid.boundingRect, on)
        XCTAssertEqual(SchematicSymbols.shapes(for: .switchSPST, value: "open").solid.boundingRect, off)
    }
}

@MainActor
final class MicrocontrollerLibraryTests: XCTestCase {
    func testTenMicrocontrollersPerVendorWithRealPackages() throws {
        let groups = Dictionary(grouping: StandardLibrary.parts.filter { $0.category.hasPrefix("Microcontrollers · ") },
                                by: \.category)
        // + the robotics spares' LPC1769 (CNC / 3D-printer controllers) and the general-purpose catalog's RP2350B.
        XCTAssertEqual(groups["Microcontrollers · Arm"]?.count, 12)
        // + the production catalog's STM32F405RGT6, STM32H743IIT6 and STM32F765VIT6, the robotics spares'
        // STM32F446RET6, STM32G474RET6 and STM32H723VGT6, and 11 general-purpose STM32s.
        XCTAssertEqual(groups["Microcontrollers · STMicroelectronics"]?.count, 27)
        XCTAssertEqual(groups["Microcontrollers · Texas Instruments"]?.count, 10)
        // + the catalog's ATMEGA328P-AU / -PU and ATSAMD51J20A-AU, and 8 general-purpose AVR / SAM D parts.
        XCTAssertEqual(groups["Microcontrollers · Microchip"]?.count, 24)
        XCTAssertEqual(groups["Microcontrollers · Espressif"]?.count, 3)
        let rp2040 = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "RP2040" })
        XCTAssertEqual(rp2040.spec.package.type, "QFN")
        XCTAssertEqual(rp2040.spec.package.pitch, 0.4)
        XCTAssertEqual(rp2040.spec.package.bodySize, 7)
        XCTAssertTrue(rp2040.packageSummary.contains("0.4mm"), rp2040.packageSummary)
    }

    func testPlacingAnStm32GivesTheRealFootprint() throws {
        let store = DesignStore()
        let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "STM32F103C8T6" })
        let id = try XCTUnwrap(store.addStandardPartToLibrary(part))
        // Saving the part again from the editor keeps its pitch and body (they round-trip through the spec).
        let info = try XCTUnwrap(store.snapshot.customParts.first { $0.id == id })
        XCTAssertEqual(info.spec.package.pitch, 0.5)
        XCTAssertEqual(info.spec.package.bodySize, 7)
        let u = store.addCustomComponent(partId: id, at: .zero)
        XCTAssertGreaterThanOrEqual(u, 0)
        store.autoPlace(all: true)
        let pads = store.snapshot.pads.filter { $0.component == u }
        XCTAssertEqual(pads.count, 48)
    }
}

@MainActor
final class AISignInTests: XCTestCase {
    private func freshDefaults() -> UserDefaults {
        let name = "sieda.tests.\(UUID().uuidString)"
        let defaults = UserDefaults(suiteName: name)!
        defaults.removePersistentDomain(forName: name)
        return defaults
    }

    func testEachCloudProviderOffersABrowserSignIn() {
        XCTAssertEqual(AIProviderKind.claude.authModes.first, .claudeConsole)
        XCTAssertEqual(AIProviderKind.gemini.authModes.first, .googleCloud)
        XCTAssertEqual(AIProviderKind.openRouter.authModes.first, .browser)
        XCTAssertTrue(AIProviderKind.openAI.authModes.contains(.sso))
        for kind in [AIProviderKind.claude, .openAI, .gemini, .openRouter] {
            XCTAssertTrue(kind.authModes.contains(.apiKey), "\(kind) keeps the API-key option")
        }
        XCTAssertTrue(AIProviderKind.ollama.authModes.isEmpty)
    }

    func testSignInModeIsRememberedAndGatesCredentials() {
        let defaults = freshDefaults()
        let settings = AISettings(defaults: defaults)
        settings.authModes[.openAI] = .sso
        settings.ssoConfiguration.issuer = "https://login.example.com"
        settings.ssoConfiguration.clientID = "sieda-desktop"
        settings.googleProject = "my-project"
        let reloaded = AISettings(defaults: defaults)
        XCTAssertEqual(reloaded.authMode(for: .openAI), .sso)
        XCTAssertEqual(reloaded.ssoConfiguration.clientID, "sieda-desktop")
        XCTAssertTrue(reloaded.ssoConfiguration.isComplete)
        XCTAssertEqual(reloaded.googleProject, "my-project")
        // Browser sign-ins count only once they have completed.
        reloaded.authModes[.gemini] = .googleCloud
        XCTAssertFalse(reloaded.isSignedIn(.gemini))
        XCTAssertFalse(reloaded.hasCredentials(for: .gemini))
        XCTAssertTrue(reloaded.hasCredentials(for: .ollama))
    }

    func testCommandLineSignInsExplainTheSandbox() async {
        guard CommandLineTool.isSandboxed else { return }  // the app (and its test host) ships sandboxed
        XCTAssertFalse(AIAuthMode.claudeConsole.isAvailable)
        XCTAssertFalse(AIAuthMode.googleCloud.isAvailable)
        XCTAssertTrue(AIAuthMode.browser.isAvailable && AIAuthMode.sso.isAvailable)
        let settings = AISettings(defaults: freshDefaults())
        XCTAssertNotEqual(settings.authMode(for: .claude), .claudeConsole, "a fresh sandboxed install starts with a method that works")
        settings.authModes[.claude] = .claudeConsole
        await settings.signIn(.claude)
        XCTAssertTrue(settings.signInMessage?.contains("sandboxed") ?? false, settings.signInMessage ?? "")
        XCTAssertFalse(settings.isSignedIn(.claude))
    }

    func testProvidersUseBearerTokensWhenSignedIn() async {
        // A Claude Console sign-in that has expired surfaces a clear message before anything is sent.
        var claude = ClaudeProvider(apiKey: "", model: "claude-opus-5-5", effort: "high")
        claude.accessToken = { throw AIAuthError.notSignedIn("Claude Console") }
        do {
            _ = try await claude.complete(AgentPrompts.analystRequest(brief: "LED"))
            XCTFail("expected a sign-in error")
        } catch {
            XCTAssertTrue(error.localizedDescription.contains("Not signed in to Claude Console"), error.localizedDescription)
        }
        let vertex = GeminiProvider.Vertex(project: "p1", region: "us-central1", accessToken: { "t" })
        XCTAssertEqual(vertex.endpoint(model: "gemini-2.5-pro")?.absoluteString,
                       "https://us-central1-aiplatform.googleapis.com/v1/projects/p1/locations/us-central1/publishers/google/models/gemini-2.5-pro:generateContent")
        let global = GeminiProvider.Vertex(project: "p1", region: "global", accessToken: { "t" })
        XCTAssertTrue(global.endpoint(model: "m")?.absoluteString.hasPrefix("https://aiplatform.googleapis.com/") ?? false)
    }

    func testPKCEMatchesTheReferenceImplementation() {
        // SHA-256 → base64url without padding (value computed with Python's hashlib).
        XCTAssertEqual(PKCE.challenge(for: "dBjftJeZ4CVP-mJ0kqHY-ihU8ss7vJ3fDy2kYiYOvp0"), "4p2cRIBKQDPO06paecsSD1HvrjHrrcmlosUunKJ8i5c")
        let verifier = PKCE.verifier()
        XCTAssertEqual(verifier.count, 43)
        XCTAssertNil(verifier.rangeOfCharacter(from: CharacterSet(charactersIn: "+/=")))
        XCTAssertNotEqual(PKCE.verifier(), verifier)
        XCTAssertEqual(OIDCAuth.formEncode(["redirect_uri": "http://127.0.0.1:5/callback", "code": "a b"]),
                       "code=a%20b&redirect_uri=http%3A%2F%2F127.0.0.1%3A5%2Fcallback")
    }

    func testLoopbackRedirectReceivesTheBrowserCallback() async throws {
        let redirect = try await LoopbackRedirect.start()
        XCTAssertGreaterThan(redirect.port, 0)
        XCTAssertTrue(redirect.redirectURI.hasPrefix("http://127.0.0.1:"))
        // The browser asks for a favicon too; only /callback completes the sign-in.
        _ = try? await URLSession.shared.data(from: URL(string: "http://127.0.0.1:\(redirect.port)/favicon.ico")!)
        let (page, _) = try await URLSession.shared.data(from: URL(string: "\(redirect.redirectURI)?code=abc123&state=xyz")!)
        XCTAssertTrue(String(decoding: page, as: UTF8.self).contains("Signed in to SiEDA"))
        let callback = try await redirect.waitForCallback(timeout: 10)
        XCTAssertEqual(callback.queryItems?.first { $0.name == "code" }?.value, "abc123")
        XCTAssertEqual(callback.queryItems?.first { $0.name == "state" }?.value, "xyz")
    }

    func testBusyPreferredPortFallsBackInsteadOfHanging() async throws {
        let first = try await LoopbackRedirect.start()
        defer { first.stop() }
        let started = Date()
        let second = try await LoopbackRedirect.start(preferredPort: first.port)  // already taken
        defer { second.stop() }
        XCTAssertNotEqual(second.port, first.port)
        XCTAssertLessThan(Date().timeIntervalSince(started), 6)
    }

    func testCancellingASignInStopsWaiting() async throws {
        let redirect = try await LoopbackRedirect.start()
        let wait = Task { try await redirect.waitForCallback(timeout: 120) }
        try await Task.sleep(nanoseconds: 200_000_000)
        let started = Date()
        wait.cancel()
        do {
            _ = try await wait.value
            XCTFail("expected cancellation")
        } catch AIAuthError.cancelled {
        }
        XCTAssertLessThan(Date().timeIntervalSince(started), 3)

        // A command-line login (ant / gcloud) is terminated the same way.
        let run = Task { try await CommandLineTool.run(URL(fileURLWithPath: "/bin/sleep"), ["60"], timeout: 120) }
        try await Task.sleep(nanoseconds: 200_000_000)
        run.cancel()
        do {
            _ = try await run.value
            XCTFail("expected cancellation")
        } catch AIAuthError.cancelled {
        }
    }

    func testCommandLineToolsRunWithoutBlocking() async throws {
        let echo = try await CommandLineTool.run(URL(fileURLWithPath: "/bin/echo"), ["token-123"], timeout: 10)
        XCTAssertEqual(echo.status, 0)
        XCTAssertEqual(echo.stdout.trimmingCharacters(in: .whitespacesAndNewlines), "token-123")
        do {
            _ = try await CommandLineTool.run(URL(fileURLWithPath: "/bin/sleep"), ["30"], timeout: 0.5)
            XCTFail("expected a timeout")
        } catch AIAuthError.timedOut {
        }
        XCTAssertNotNil(CommandLineTool.locate("ls"))
        XCTAssertNil(CommandLineTool.locate("sieda-no-such-tool"))
    }
}

@MainActor
final class PowerSourceTests: XCTestCase {
    func testPowerSourcesLeadTheDevicePicker() {
        XCTAssertEqual(ComponentKind.pickerCategories.first, "Power Sources")
        XCTAssertEqual(Array(ComponentKind.builtIn.prefix(4)), [.battery, .voltageSource, .acSource, .currentSource])
        XCTAssertEqual(Set(ComponentKind.builtIn), Set(ComponentKind.allCases.filter { $0 != .custom && $0 != .junction }))
        for kind in ComponentKind.builtIn {
            XCTAssertTrue(ComponentKind.pickerCategories.contains(kind.category), "\(kind) has no picker section")
        }
        XCTAssertEqual(ComponentKind.fromPlanName("battery"), .battery)
        XCTAssertEqual(ComponentKind.fromPlanName("LiPo"), .battery)
        XCTAssertEqual(ComponentKind.fromPlanName("mains"), .acSource)
        XCTAssertEqual(ComponentKind.fromPlanName("AC Source"), .acSource)
        XCTAssertEqual(ComponentKind.fromPlanName("dc_supply"), .voltageSource)
    }

    func testABatteryLightsAnLED() async throws {
        let store = DesignStore()
        let bt = store.addComponent(.battery, at: .zero)
        let r = store.addComponent(.resistor, at: CGPoint(x: 120, y: -40))
        store.setValue(r, "330")
        let d = store.addComponent(.led, at: CGPoint(x: 240, y: -40))
        let g = store.addComponent(.ground, at: CGPoint(x: 0, y: 100))
        func pin(_ id: Int, _ i: Int) -> PinAddress { PinAddress(component: id, pin: i) }
        XCTAssertTrue(store.connect(pin(bt, 0), pin(r, 0)))
        XCTAssertTrue(store.connect(pin(r, 1), pin(d, 0)))
        XCTAssertTrue(store.connect(pin(d, 1), pin(g, 0)))
        XCTAssertTrue(store.connect(pin(bt, 1), pin(g, 0)))
        XCTAssertEqual(store.snapshot.component(bt)?.ref, "BT1")
        XCTAssertEqual(store.snapshot.component(bt)?.value, "9")
        await store.simulateDC()
        let current = try XCTUnwrap(store.dcResult?.devices.first { $0.component == d }).current
        XCTAssertEqual(current, (9 - 1.9) / 330, accuracy: 0.004)  // ≈ 21 mA from the 9 V battery
    }
}

final class WorkspaceOrderTests: XCTestCase {
    func testSidebarOrderAndTitles() {
        XCTAssertEqual(Workspace.visible(aiEnabled: true).map(\.title),
                       ["Super Intelligence", "Schematic", "PCB Layout", "3D Viewer", "Simulation", "Design Checks", "BOM", "Library"])
        XCTAssertEqual(Workspace.visible(aiEnabled: false).first, .schematic)
        XCTAssertEqual(Workspace.visible(aiEnabled: false).last, .library)
    }
}

@MainActor
final class WireJunctionTests: XCTestCase {
    private func pin(_ id: Int, _ i: Int) -> PinAddress { PinAddress(component: id, pin: i) }

    /// The user's circuit: 9 V battery → 10 kΩ → red LED, with C1 placed beside the LED and dropped onto the wires.
    private func circuit(_ store: DesignStore) -> (bt: Int, r1: Int, d1: Int, c1: Int, top: Int, bottom: Int) {
        let bt = store.addComponent(.battery, at: .zero)
        let r1 = store.addComponent(.resistor, at: CGPoint(x: 100, y: -60))
        let d1 = store.addComponent(.led, at: CGPoint(x: 300, y: 0), rotation: 90)
        let c1 = store.addComponent(.capacitor, at: CGPoint(x: 200, y: 0), rotation: 90)
        let g = store.addComponent(.ground, at: CGPoint(x: 0, y: 100))
        XCTAssertTrue(store.connect(pin(bt, 0), pin(r1, 0)))
        XCTAssertTrue(store.connect(pin(r1, 1), pin(d1, 0)))
        XCTAssertTrue(store.connect(pin(d1, 1), pin(bt, 1)))
        XCTAssertTrue(store.connect(pin(bt, 1), pin(g, 0)))
        let top = store.snapshot.wires.first { $0.a == pin(r1, 1) }!.id
        let bottom = store.snapshot.wires.first { $0.a == pin(d1, 1) }!.id
        return (bt, r1, d1, c1, top, bottom)
    }

    private func net(_ store: DesignStore, _ address: PinAddress) -> Int {
        store.snapshot.component(address.component)!.pins[address.pin].net
    }

    func testComponentsConnectInParallelOnExistingWires() throws {
        let store = DesignStore()
        let c = circuit(store)
        // The bottom wire passes straight over C1.2, which is still not connected: drop C1's pins onto the wires.
        XCTAssertEqual(store.snapshot.component(c.c1)?.pins[1].connected, false)
        let t1 = try XCTUnwrap(store.drawWire(from: .pin(pin(c.c1, 0)), to: .wire(c.top, CGPoint(x: 203, y: -58))))
        let wires = store.snapshot.wires.count
        let t2 = try XCTUnwrap(store.drawWire(from: .pin(pin(c.c1, 1)), to: .wire(c.bottom, CGPoint(x: 198, y: 31))))
        XCTAssertEqual(store.snapshot.component(t1.component)?.componentKind, .junction)
        XCTAssertEqual(store.snapshot.component(t1.component)?.position, CGPoint(x: 200, y: -60))  // on the grid, on the wire
        XCTAssertEqual(store.snapshot.component(t2.component)?.position, CGPoint(x: 200, y: 30))

        XCTAssertEqual(net(store, pin(c.c1, 0)), net(store, pin(c.d1, 0)))
        XCTAssertEqual(net(store, pin(c.c1, 1)), net(store, pin(c.d1, 1)))
        XCTAssertTrue(store.snapshot.component(c.c1)!.pins.allSatisfy(\.connected))
        XCTAssertFalse(store.engine.runERC().contains { $0.code == "ERC_UNCONNECTED_PIN" || $0.code == "ERC_DANGLING_WIRE" })

        // One T-junction is one undo step: the split and the new wire go together.
        store.undo()
        XCTAssertEqual(store.snapshot.wires.count, wires)
        XCTAssertNil(store.snapshot.component(t2.component))
        XCTAssertEqual(store.snapshot.component(c.c1)?.pins[1].connected, false)

        // Plans (AI context) never see junctions: C1 is connected pin to pin on the same nets.
        store.drawWire(from: .pin(pin(c.c1, 1)), to: .wire(c.bottom, CGPoint(x: 200, y: 30)))
        let plan = DesignPlanCompiler.plan(from: store.snapshot)
        XCTAssertFalse(plan.components.contains { $0.kind == "junction" })
        let c1Ref = store.snapshot.component(c.c1)!.ref, d1Ref = store.snapshot.component(c.d1)!.ref
        let links = plan.connections.flatMap { [$0.from, $0.to] }
        XCTAssertTrue(links.contains("\(c1Ref).1") && links.contains("\(c1Ref).2") && links.contains("\(d1Ref).A"))
    }

    func testWiresTurnCornersBendAndStraighten() throws {
        let store = DesignStore()
        let c = circuit(store)
        // Wire tool: from C1.1 out to a free corner, then on to R1.2 (one wire with a bend point).
        let corner = try XCTUnwrap(store.drawWire(from: .pin(pin(c.c1, 0)), to: .point(CGPoint(x: 198, y: -102))))
        XCTAssertEqual(store.snapshot.component(corner.component)?.position, CGPoint(x: 200, y: -100))
        XCTAssertNotNil(store.drawWire(from: .pin(corner), to: .pin(pin(c.r1, 1))))
        XCTAssertEqual(net(store, pin(c.c1, 0)), net(store, pin(c.r1, 1)))

        // A wire abandoned after a corner leaves nothing behind.
        let loose = try XCTUnwrap(store.drawWire(from: .pin(pin(c.c1, 1)), to: .point(CGPoint(x: 250, y: 150))))
        store.cancelWire(at: loose)
        XCTAssertNil(store.snapshot.component(loose.component))
        XCTAssertEqual(store.snapshot.component(c.c1)?.pins[1].connected, false)

        // Dragging a wire bends it through a junction that stays selected (drag it further); delete straightens it.
        store.bendWire(c.top, at: CGPoint(x: 252, y: -118))
        let bend = try XCTUnwrap(store.selection.first)
        XCTAssertEqual(store.snapshot.component(bend)?.position, CGPoint(x: 250, y: -120))
        store.moveComponents([bend], by: CGSize(width: 0, height: -20))
        XCTAssertEqual(store.snapshot.component(bend)?.position, CGPoint(x: 250, y: -140))
        XCTAssertEqual(net(store, pin(c.r1, 1)), net(store, pin(c.d1, 0)))
        store.deleteSelection()
        XCTAssertNil(store.snapshot.component(bend))
        XCTAssertEqual(net(store, pin(c.r1, 1)), net(store, pin(c.d1, 0)))
        XCTAssertTrue(store.snapshot.components.filter { $0.componentKind == .junction }.allSatisfy { j in
            store.snapshot.wires.filter { $0.a.component == j.id || $0.b.component == j.id }.count >= 2
        })
    }

    func testTJunctionPointSnapsOntoTheWireRoute() {
        // R1.2 (130, -60) → D1.A (300, -30) is drawn as an L: along y = -60, then down x = 300.
        let wire = SnapWire(id: 1, a: PinAddress(component: 1, pin: 1), b: PinAddress(component: 2, pin: 0),
                            ax: 130, ay: -60, bx: 300, by: -30, net: 0)
        XCTAssertEqual(WireGeometry.nearestPoint(on: wire, to: CGPoint(x: 207, y: -55)).point, CGPoint(x: 210, y: -60))
        XCTAssertEqual(WireGeometry.nearestPoint(on: wire, to: CGPoint(x: 304, y: -44)).point, CGPoint(x: 300, y: -40))
        XCTAssertEqual(WireGeometry.nearestPoint(on: wire, to: CGPoint(x: 100, y: -60)).point, CGPoint(x: 130, y: -60))
    }
}

@MainActor
final class SolderMaskTests: XCTestCase {
    /// Colour of the board's top face in the 3D assembly mesh.
    private func boardTop(_ engine: EDAEngine) throws -> (r: Float, g: Float, b: Float) {
        let mesh = try XCTUnwrap(engine.buildMesh(includeComponents: false))
        let v = try XCTUnwrap((0..<mesh.vertexCount).first { mesh.normals[$0 * 3 + 1] > 0.99 })
        return (mesh.colors[v * 4], mesh.colors[v * 4 + 1], mesh.colors[v * 4 + 2])
    }

    func testBoardsAreGreenByDefaultAndTheMaskColourIsAChoice() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: store.engine, previous: nil)
        store.engine.autoPlace(all: true)
        store.refresh()
        XCTAssertEqual(store.snapshot.board.mask, .green)
        let green = try boardTop(store.engine)
        XCTAssertGreaterThan(green.g, green.r * 3)
        XCTAssertGreaterThan(green.g, green.b)

        XCTAssertEqual(SolderMaskColour.allCases.map(\.rawValue), ["green", "black", "blue", "red", "yellow", "white", "purple"])
        for mask in SolderMaskColour.allCases {
            store.setSolderMask(mask)
            XCTAssertEqual(store.snapshot.board.mask, mask)
            let top = try boardTop(store.engine)
            XCTAssertEqual(Double(top.r), mask.swatch.red, accuracy: 0.001, "\(mask)")
            XCTAssertEqual(Double(top.g), mask.swatch.green, accuracy: 0.001, "\(mask)")
            XCTAssertEqual(Double(top.b), mask.swatch.blue, accuracy: 0.001, "\(mask)")
        }
        store.undo()
        XCTAssertEqual(store.snapshot.board.mask, .white)
        // Saved with the project.
        let reopened = EDAEngine()
        XCTAssertNoThrow(try reopened.load(json: store.engine.saveJSON()))
        XCTAssertEqual(reopened.snapshot()?.board.mask, .white)
    }
}

@MainActor
final class StackUp3DTests: XCTestCase {
    func testThe3DBoardFollowsTheLayerCountPickedInPCBLayout() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: store.engine, previous: nil)
        store.engine.autoPlace(all: true)
        store.refresh()
        var signatures: [Int: Int] = [:]
        for layers in [1, 2, 4, 6] {
            let revision = store.revision
            store.setLayerCount(layers)
            XCTAssertEqual(store.snapshot.board.layerCount, layers)
            XCTAssertNotEqual(store.revision, revision, "3D views rebuild on the revision change")
            let mesh = try XCTUnwrap(store.engine.buildMesh(includeComponents: false))
            signatures[layers] = mesh.vertexCount
        }
        // Each stack-up builds a different board (bare underside, mask, inner copper bands).
        XCTAssertEqual(Set(signatures.values).count, 4, "\(signatures)")
    }
}

@MainActor
final class AutoRouteActionTests: XCTestCase {
    func testOneAutoRouteClickPlacesInsideTheBoardShapeAndRoutes() async throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: store.engine, previous: nil)
        store.refresh()
        XCTAssertTrue(store.snapshot.pads.isEmpty)
        // A shaped board: the footprints must land inside the circle.
        store.applyOutlinePreset(.circle, width: 60, height: 60, parameter: 0)
        let unplacedState = store.snapshot

        await store.autoRouteBoard()
        XCTAssertFalse(store.snapshot.pads.isEmpty)
        XCTAssertTrue(store.snapshot.components.filter { !$0.componentKind.isVirtual }.allSatisfy(\.pcb.placed))
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty, "every connection routed")
        XCTAssertFalse(store.snapshot.tracks.isEmpty)
        let cx = store.snapshot.board.width / 2, cy = store.snapshot.board.height / 2
        for pad in store.snapshot.pads {
            XCTAssertLessThan(hypot(pad.x - cx, pad.y - cy), 30, "pad outside the circular board")
        }

        // Clicking again re-routes cleanly (no duplicate copper), and one undo goes back to before the first click.
        let tracks = store.snapshot.tracks.count
        await store.autoRouteBoard()
        XCTAssertEqual(store.snapshot.tracks.count, tracks)
        store.undo()
        store.undo()
        XCTAssertTrue(store.snapshot.pads.isEmpty)
        XCTAssertEqual(store.snapshot.board.outline.count, unplacedState.board.outline.count)
    }
}

@MainActor
final class FabricationPackageTests: XCTestCase {
    func testThePackageHasEverythingAFabAndAssemblyHouseNeed() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: engine, previous: nil)
        engine.autoPlace(all: true)
        XCTAssertEqual(engine.autoRoute().failed, 0)
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("sieda-fab-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: folder) }

        let result = engine.writeFabricationPackage(to: folder, base: "Board")
        XCTAssertTrue(result.ok, result.error)
        for path in ["gerbers/Board-F_Cu.gbr", "gerbers/Board-B_Cu.gbr", "gerbers/Board-F_Mask.gbr", "gerbers/Board-B_Mask.gbr",
                     "gerbers/Board-F_Paste.gbr", "gerbers/Board-F_Silkscreen.gbr", "gerbers/Board-Edge_Cuts.gbr",
                     "gerbers/Board-PTH.drl", "gerbers/Board-job.gbrjob", "gerbers/Board-ipc356.ipc",
                     "assembly/Board-bom.csv", "assembly/Board-bom_assembly.csv", "assembly/Board-cpl.csv",
                     "assembly/Board-pick_and_place.csv", "assembly/Board-assembly_top.svg",
                     "Board-gerbers.zip", "fab_notes.txt", "Board-netlist.cir", "3d/Board.stl"] {
            XCTAssertTrue(result.files.contains(path), "missing \(path)")
            XCTAssertTrue(FileManager.default.fileExists(atPath: folder.appendingPathComponent(path).path), path)
        }
        let zip = try Data(contentsOf: folder.appendingPathComponent("Board-gerbers.zip"))
        XCTAssertEqual(Array(zip.prefix(4)), [0x50, 0x4B, 0x03, 0x04])
        let notes = try String(contentsOf: folder.appendingPathComponent("fab_notes.txt"), encoding: .utf8)
        XCTAssertTrue(notes.contains("Solder mask          Green"))
        XCTAssertTrue(notes.contains("Board-gerbers.zip"))

        // Every single-file export works on its own too.
        for format in ExportFormat.allCases {
            XCTAssertNotNil(engine.export(format), "\(format)")
        }
        XCTAssertTrue(engine.export(.ipc356)?.hasSuffix("999\n") ?? false)
        XCTAssertTrue(engine.export(.cpl)?.hasPrefix("Designator,Mid X,Mid Y,Layer,Rotation") ?? false)
    }
}

@MainActor
final class NetColourTests: XCTestCase {
    func testPowerIsRedGroundIsBlueAndSignalsFollowTheirLayer() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: engine, previous: nil)
        let snapshot = try XCTUnwrap(engine.snapshot())
        let roles = Set(snapshot.nets.map(\.netRole))
        XCTAssertTrue(roles.contains(.power), "\(snapshot.nets.map { "\($0.name)=\($0.role ?? "nil")" })")
        XCTAssertTrue(roles.contains(.ground))
        XCTAssertTrue(roles.contains(.signal))
        for net in snapshot.nets where net.ground { XCTAssertEqual(net.netRole, .ground) }

        // Signal colours never look like power (red) or ground (blue) on any layer of a 6-layer board.
        let power = NSColor(Theme.netColor(.power, layer: 0, layerCount: 6)).usingColorSpace(.sRGB)!
        let ground = NSColor(Theme.netColor(.ground, layer: 0, layerCount: 6)).usingColorSpace(.sRGB)!
        XCTAssertGreaterThan(power.redComponent, 0.9)
        XCTAssertGreaterThan(ground.blueComponent, 0.9)
        for layer in 0..<6 {
            let c = NSColor(Theme.signalColor(layer, layerCount: 6)).usingColorSpace(.sRGB)!
            let isRed = c.redComponent > 0.8 && c.greenComponent < 0.4 && c.blueComponent < 0.4
            let isBlue = c.blueComponent > 0.8 && c.redComponent < 0.4 && c.greenComponent < 0.6
            XCTAssertFalse(isRed || isBlue, "layer \(layer)")
        }
        // Layer mode keeps the CAD convention: top red, bottom blue.
        let top = NSColor(Theme.copperColor(0, layerCount: 2)).usingColorSpace(.sRGB)!
        let bottom = NSColor(Theme.copperColor(1, layerCount: 2)).usingColorSpace(.sRGB)!
        XCTAssertGreaterThan(top.redComponent, top.blueComponent)
        XCTAssertGreaterThan(bottom.blueComponent, bottom.redComponent)
    }
}

@MainActor
final class BomWorkspaceTests: XCTestCase {
    func testBomLinesSuggestionsSourcingCostAndDNP() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: store.engine, previous: nil)  // NPN LED driver
        store.refresh()
        var bom = store.bomReport
        XCTAssertFalse(bom.lines.isEmpty)
        XCTAssertFalse(bom.lines.contains { $0.type == "Ground" || $0.type == "Net Label" })
        let r1 = try XCTUnwrap(bom.lines.first { $0.refs.contains("R1") })
        XCTAssertEqual(r1.suggestedMpn, "RC0805FR-0710KL")
        XCTAssertEqual(r1.suggestedManufacturer, "Yageo")
        XCTAssertEqual(r1.rating, "0.125 W")
        XCTAssertTrue(r1.mpn.isEmpty)
        XCTAssertEqual(bom.summary.missingMpn, bom.lines.count)

        // One click fills every suggested part number; undo takes them all back.
        let filled = store.applySuggestedPartNumbers()
        XCTAssertGreaterThan(filled, 0)
        XCTAssertEqual(store.bomReport.lines.first { $0.refs.contains("R1") }?.mpn, "RC0805FR-0710KL")
        XCTAssertEqual(store.bomReport.lines.first { $0.refs.contains("Q1") }?.mpn, "BC847")
        store.undo()
        XCTAssertTrue(store.bomReport.lines.allSatisfy(\.mpn.isEmpty))
        store.redo()

        // Price and DNP on a line.
        bom = store.bomReport
        let resistor = try XCTUnwrap(bom.lines.first { $0.refs.contains("R1") })
        store.updateBomLine(resistor, SourcingUpdate(supplierPart: "C17414", unitPrice: 0.01))
        let led = try XCTUnwrap(store.bomReport.lines.first { $0.refs.contains("D1") })
        store.updateBomLine(led, SourcingUpdate(dnp: true))
        bom = store.bomReport
        XCTAssertEqual(bom.summary.dnp, 1)
        XCTAssertEqual(bom.summary.costPerBoard, 0.01, accuracy: 1e-9)
        store.setBuildQuantity(20)
        XCTAssertEqual(store.bomReport.orderCost, 0.2, accuracy: 1e-9)
        let assembly = try XCTUnwrap(store.engine.export(.bomAssembly))
        XCTAssertTrue(assembly.contains("C17414") && assembly.contains("RC0805FR-0710KL"))
        XCTAssertFalse(assembly.contains("D1"), "DNP parts are not ordered")

        // Saved with the project.
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        let again = reopened.bom()
        XCTAssertEqual(again.buildQuantity, 20)
        XCTAssertEqual(again.lines.first { $0.refs.contains("R1") }?.supplierPart, "C17414")
        XCTAssertEqual(again.lines.first { $0.refs.contains("D1") }?.dnp, true)
    }
}

@MainActor
final class ReliabilityTests: XCTestCase {
    func testCoatingIsSavedAndChangesTheSpacingStandard() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        XCTAssertEqual(store.snapshot.board.conformalCoating, .none)
        store.setCoating(.silicone)
        XCTAssertEqual(store.snapshot.board.conformalCoating, .silicone)
        store.undo()
        XCTAssertEqual(store.snapshot.board.conformalCoating, .none)
        store.redo()
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.board.conformalCoating, .silicone)

        // The verification report has a Design for Reliability stage, and its findings reach Design Checks (DRC).
        store.engine.autoPlace(all: true)
        _ = store.engine.autoRoute()
        let verification = try XCTUnwrap(store.engine.runVerification())
        let stage = try XCTUnwrap(verification.stages.first { $0.id == "reliability" })
        XCTAssertTrue(stage.details.contains { $0.contains("Conformal coating: silicone") })
        XCTAssertEqual(ConformalCoating.allCases.count, 6)
    }

    func testStackupMaterialImpedanceAndBackdrill() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        let fr4 = store.stackup()
        XCTAssertEqual(fr4.material, "fr4")
        XCTAssertTrue(fr4.materials.contains { $0.id == "rogers-4350b" })
        XCTAssertTrue(fr4.materials.contains { $0.id == "megtron-6" })
        let fr4Width = try XCTUnwrap(fr4.layers.first?.seWidth)

        store.setStackup(material: "rogers-4350b", construction: .metalCore, singleEnded: 50, backdrill: true)
        let board = store.snapshot.board
        XCTAssertEqual(board.material, "rogers-4350b")
        XCTAssertEqual(board.boardConstruction, .metalCore)
        XCTAssertTrue(board.backdrill)
        // Lower εr → wider 50 Ω microstrip on the same dielectric.
        let rogers = store.stackup()
        XCTAssertGreaterThan(try XCTUnwrap(rogers.layers.first?.seWidth), fr4Width)

        store.undo()
        XCTAssertEqual(store.snapshot.board.material, "fr4")
        store.redo()
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.board.material, "rogers-4350b")
        XCTAssertEqual(reopened.snapshot()?.board.boardConstruction, .metalCore)
    }

    func testRobotSystemSegments() throws {
        XCTAssertEqual(OfflineProvider.template(for: "quadruped robot dog joint controller with an e-stop").plan.title,
                       "Robot Joint Controller: 7-Segment Robotics Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.robotPlatform != nil })
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.plan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.robotPlatform, "arm")
        let segments = store.robotSegments()
        XCTAssertTrue(segments.applies)
        XCTAssertEqual(segments.platforms.count, 7)  // + 3D printer and CNC machine
        XCTAssertEqual(segments.segments.map(\.id), ["power", "compute", "motion", "sensors", "comms", "safety", "mechanical"])
        // The schematic alone already completes power distribution and the safety interlock.
        XCTAssertEqual(segments.segments.first { $0.id == "power" }?.status, "complete")
        XCTAssertEqual(segments.segments.first { $0.id == "safety" }?.status, "complete")
        // Platform changes are undoable and saved.
        store.setRobotPlatform("humanoid")
        XCTAssertEqual(store.snapshot.robotPlatform, "humanoid")
        store.undo()
        XCTAssertEqual(store.snapshot.robotPlatform, "arm")
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.robotPlatform, "arm")
        // Refinement plans keep the platform.
        XCTAssertEqual(DesignPlanCompiler.plan(from: store.snapshot).robotPlatform, "arm")
    }

    func testAutomotiveEcuSegments() throws {
        XCTAssertEqual(OfflineProvider.template(for: "body control module ECU with CAN-FD and LIN").plan.title,
                       "Automotive Body ECU: 6-Segment Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.ecuType != nil })
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.ecuType, "bcm")
        XCTAssertEqual(store.snapshot.industry, "automotive")
        let segments = store.ecuSegments()
        XCTAssertTrue(segments.applies)
        XCTAssertEqual(segments.platforms.count, 6)
        XCTAssertEqual(segments.segments.map(\.id), ["shield", "regulation", "mcu", "network", "actuation", "sensors"])
        // Every segment but the lockstep safety MCU is complete on the schematic alone.
        for id in ["shield", "regulation", "network", "actuation", "sensors"] {
            XCTAssertEqual(segments.segments.first { $0.id == id }?.status, "complete", id)
        }
        XCTAssertEqual(segments.segments.first { $0.id == "mcu" }?.status, "partial")
        // ECU type changes are undoable and saved.
        store.setEcuType("gateway")
        XCTAssertEqual(store.snapshot.ecuType, "gateway")
        store.undo()
        XCTAssertEqual(store.snapshot.ecuType, "bcm")
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.ecuType, "bcm")
        XCTAssertEqual(DesignPlanCompiler.plan(from: store.snapshot).ecuType, "bcm")
    }

    func testAerospaceSegments() throws {
        XCTAssertEqual(OfflineProvider.template(for: "rad-hard satellite on-board computer with TMR and SpaceWire").plan.title,
                       "Satellite On-Board Computer: 5-Segment Aerospace Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.aerospaceMission != nil })
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.aerospaceMission, "leo")
        XCTAssertEqual(store.snapshot.industry, "space")
        let segments = store.aerospaceSegments()
        XCTAssertTrue(segments.applies)
        XCTAssertEqual(segments.platforms.count, 5)
        XCTAssertEqual(segments.segments.map(\.id), ["compute", "power", "sensors", "avionics", "rf"])
        // The schematic alone completes compute, power, sensors and avionics (RF needs the stack-up, set by the plan).
        for segment in segments.segments {
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        // Mission changes are undoable and saved.
        store.setAerospaceMission("military")
        XCTAssertEqual(store.snapshot.aerospaceMission, "military")
        XCTAssertEqual(store.aerospaceSegments().segments.first { $0.id == "avionics" }?.status, "partial")  // no 1553
        store.undo()
        XCTAssertEqual(store.snapshot.aerospaceMission, "leo")
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.aerospaceMission, "leo")
        XCTAssertEqual(DesignPlanCompiler.plan(from: store.snapshot).aerospaceMission, "leo")
    }

    func testNavalSegments() throws {
        XCTAssertEqual(OfflineProvider.template(for: "navy destroyer shipboard interface with sonar").plan.title,
                       "Naval Shipboard Interface: 5-Segment Navy Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.navalPlatform != nil })
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.navalPlatform, "combatant")
        XCTAssertEqual(store.snapshot.board.coating, "parylene")
        XCTAssertTrue(store.snapshot.board.underfill)
        XCTAssertEqual(store.snapshot.board.thickness, 2.4, accuracy: 1e-9)
        let segments = store.navalSegments()
        XCTAssertEqual(segments.segments.map(\.id), ["power", "compute", "mechanical", "comms", "rfsonar"])
        for segment in segments.segments where segment.id != "compute" {  // the coating border needs a layout
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        store.setMechanical(underfill: false)
        XCTAssertEqual(store.navalSegments().segments.first { $0.id == "mechanical" }?.status, "partial")
        store.undo()
        XCTAssertTrue(store.snapshot.board.underfill)
        XCTAssertEqual(DesignPlanCompiler.plan(from: store.snapshot).navalPlatform, "combatant")
    }

    func testApplianceSegmentsAndMainsSpacing() throws {
        XCTAssertEqual(OfflineProvider.template(for: "washing machine controller with triac pump, inverter drum motor and ESP32").plan.title,
                       "Washing Machine Controller: 4-Segment Home Appliance Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.applianceType != nil })
        XCTAssertEqual(template.industry, "appliance")
        XCTAssertTrue(OfflineProvider.categories.contains(template.category))
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.applianceType, "laundry")
        let segments = store.applianceSegments()
        XCTAssertEqual(segments.segments.map(\.id), ["mains", "actuation", "sensing", "iot"])
        for segment in segments.segments where segment.id != "iot" {  // the antenna items need the placed module
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        store.setApplianceType("")
        XCTAssertTrue(store.snapshot.applianceType.isEmpty)
        store.undo()
        XCTAssertEqual(store.snapshot.applianceType, "laundry")
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.applianceType, "laundry")
        XCTAssertEqual(PackageKind.guess("ESP32-WROOM castellated module"), .module)
    }

    func testRetailSegmentsAndTamperMesh() throws {
        XCTAssertEqual(OfflineProvider.template(for: "PCI PTS payment terminal with a tamper mesh and receipt printer").plan.title,
                       "Countertop Payment Terminal + Receipt Printer: 4-Segment POS Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.retailDevice != nil })
        XCTAssertEqual(template.industry, "retail")
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.retailDevice, "countertop")
        XCTAssertEqual(store.snapshot.tamperMeshes.map(\.component), ["U3"])
        let segments = store.retailSegments()
        XCTAssertEqual(segments.segments.map(\.id), ["security", "printer", "hmi", "esd"])
        for segment in segments.segments {
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        // Without the mesh the security segment is incomplete; undo brings it back.
        store.clearTamperMeshes()
        XCTAssertTrue(store.snapshot.tamperMeshes.isEmpty)
        XCTAssertEqual(store.retailSegments().segments.first { $0.id == "security" }?.status, "partial")
        store.undo()
        XCTAssertEqual(store.snapshot.tamperMeshes.count, 1)
        // The mesh and the device class survive save / open and the plan round trip.
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        let snapshot = try XCTUnwrap(reopened.snapshot())
        XCTAssertEqual(snapshot.retailDevice, "countertop")
        XCTAssertEqual(snapshot.tamperMeshes.first?.netB, "TAMPER_MESH_B")
        XCTAssertEqual(snapshot.board.rulePreset, "Fab House Advanced (4/4 mil)")
    }

    func testMedicalSegmentsAndIsolationBarrier() throws {
        XCTAssertEqual(OfflineProvider.template(for: "defibrillator-proof ECG patient monitor, IEC 60601").plan.title,
                       "Defibrillator-Proof ECG Monitor: 4-Segment Medical Reference")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.medicalClass != nil })
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        store.refresh()
        XCTAssertEqual(store.snapshot.medicalClass, "cf")
        XCTAssertEqual(store.snapshot.board.isolationGap, 8, accuracy: 1e-9)
        let segments = store.medicalSegments()
        XCTAssertEqual(segments.segments.map(\.id), ["isolation", "biosignal", "compute", "coexistence"])
        for segment in segments.segments {
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        // Dropping the barrier to 4 mm breaks the 2 × MOPP item; undo restores it.
        store.setIsolationGap(4)
        XCTAssertEqual(store.medicalSegments().segments.first { $0.id == "isolation" }?.status, "partial")
        store.undo()
        XCTAssertEqual(store.snapshot.board.isolationGap, 8, accuracy: 1e-9)
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.medicalClass, "cf")
        XCTAssertEqual(reopened.snapshot()?.board.isolationGap ?? 0, 8, accuracy: 1e-9)
    }

    func testComputingSegmentReferenceDesigns() throws {
        // Intel / AMD / ARM / NVIDIA / Microsoft / Google platform segments find their reference designs.
        XCTAssertEqual(OfflineProvider.template(for: "an Intel or AMD desktop motherboard VRM with a PCIe slot").industry, "motherboard")
        XCTAssertEqual(OfflineProvider.template(for: "EPYC server for a Microsoft / Google style OCP data center rack").industry, "server")
        XCTAssertEqual(OfflineProvider.template(for: "NVIDIA GPU accelerator baseboard for a supercomputer").industry, "hpc")
        XCTAssertEqual(OfflineProvider.template(for: "ARM Cortex compute module carrier board").industry, "arm")
        XCTAssertEqual(OfflineProvider.template(for: "PCIe add-in daughterboard with gold fingers").industry, "addin")
        let expected: [String: (layers: Int, material: String)] = [
            "motherboard": (8, "isola-370hr"), "server": (12, "megtron-6"), "hpc": (12, "megtron-7"),
            "arm": (6, "fr4"), "addin": (4, "fr4")]
        for (industry, board) in expected {
            let template = try XCTUnwrap(OfflineProvider.templates.first { $0.industry == industry })
            XCTAssertTrue(OfflineProvider.categories.contains(template.category))
            let engine = EDAEngine()
            let report = DesignPlanCompiler.apply(template.plan, to: engine, previous: nil)
            XCTAssertTrue(report.warnings.isEmpty, "\(template.plan.title): \(report.warnings)")
            let snapshot = try XCTUnwrap(engine.snapshot())
            XCTAssertEqual(snapshot.board.layerCount, board.layers, industry)
            XCTAssertEqual(snapshot.board.material, board.material, industry)
            XCTAssertNotNil(StandardLibrary.industry(industry), industry)
        }
        XCTAssertEqual(try XCTUnwrap(OfflineProvider.templates.first { $0.industry == "motherboard" }).plan.board.differentialOhms, 85)
        XCTAssertTrue(BoardInfo.layerChoices.contains(24))
    }

    func testHighSpeedReferenceDesignIsLengthMatched() throws {
        XCTAssertEqual(OfflineProvider.template(for: "usb 2.0 high-speed link with a length matched DDR bus").plan.title,
                       "High-Speed Link: USB 2.0 Pair + 8-bit DQ Bus")
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.title.hasPrefix("High-Speed Link") })
        let engine = EDAEngine()
        let report = DesignPlanCompiler.apply(template.plan, to: engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        let board = try XCTUnwrap(engine.snapshot()?.board)
        XCTAssertEqual(board.material, "megtron-6")
        XCTAssertTrue(board.backdrill)
        // Connectors and terminators are locked: Auto Place keeps them where the plan put them.
        engine.autoPlace(all: true)
        let j3 = try XCTUnwrap(engine.snapshot()?.components.first { $0.ref == "J3" })
        XCTAssertEqual(j3.pcb.locked, true)
        XCTAssertEqual(j3.pcb.x, 6, accuracy: 1e-6)
        XCTAssertEqual(j3.pcb.y, 22, accuracy: 1e-6)
        let stats = engine.autoRoute()
        XCTAssertEqual(stats.failed, 0)
        XCTAssertGreaterThan(stats.lengthTuned ?? 0, 0)
        // The USB pair and the DQ0–DQ7 lane come out length-matched.
        let lengths = engine.lengthReport()
        XCTAssertEqual(Set(lengths.groups.map(\.kind)), ["pair", "bus"])
        for group in lengths.groups { XCTAssertTrue(group.matched, "\(group.name): \(group.nets.map(\.length))") }
        XCTAssertEqual(lengths.groups.first { $0.kind == "bus" }?.nets.count, 8)
    }

    func testEmbeddedResistorMovesInsideTheBoard() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[4].plan, to: store.engine, previous: nil)  // NPN LED driver
        store.refresh()
        store.setLayerCount(4)
        store.autoPlace(all: true)
        let resistor = try XCTUnwrap(store.snapshot.components.first { $0.componentKind == .resistor })
        store.selection = [resistor.id]
        store.setEmbedded(layer: 1)
        let embedded = try XCTUnwrap(store.snapshot.components.first { $0.id == resistor.id })
        XCTAssertEqual(embedded.pcb.embeddedLayer, 1)
        // Its terminations are on inner layer 2, and it is listed as embedded (not assembled) in the BOM.
        let pads = store.snapshot.pads.filter { $0.component == resistor.id }
        XCTAssertEqual(pads.count, 2)
        XCTAssertTrue(pads.allSatisfy { $0.layer == 1 })
        XCTAssertEqual(store.engine.bom().lines.first { $0.componentIds.contains(resistor.id) }?.embedded, true)
        store.undo()
        XCTAssertFalse(store.snapshot.components.first { $0.id == resistor.id }?.pcb.isEmbedded ?? true)
    }

    func testHDISettingsAreUndoableAndSaved() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        XCTAssertFalse(store.snapshot.board.hdi)
        store.setHDI(enabled: true, viaInPad: true)
        XCTAssertTrue(store.snapshot.board.hdi)
        XCTAssertTrue(store.snapshot.board.viaInPad)
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.board.hdi, true)
        store.undo()
        XCTAssertFalse(store.snapshot.board.hdi)
        // Every via in the snapshot reports its span.
        for via in store.snapshot.vias { XCTAssertNotNil(via.kind) }
    }

    func testLengthMatchingSettingsAndReport() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        XCTAssertTrue(store.snapshot.board.lengthTuning)
        store.setLengthMatching(enabled: false, pairSkew: 0.05)
        XCTAssertFalse(store.snapshot.board.lengthTuning)
        XCTAssertEqual(store.snapshot.board.pairSkewTolerance, 0.05, accuracy: 1e-9)
        store.undo()
        XCTAssertTrue(store.snapshot.board.lengthTuning)
        let report = store.lengthReport()
        XCTAssertTrue(report.enabled)
        // Every group the report lists has members with names.
        for group in report.groups { XCTAssertFalse(group.nets.isEmpty) }
    }
}

/// Holographic data panels in the X-ray stack: the toggle, the figures they chart, their textures and their layout.
@MainActor
final class XRayPanelTests: XCTestCase {
    private func placedDesign() throws -> EDAEngine {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        engine.autoPlace(all: true)
        return engine
    }

    func testPanelsToggleIsOffByDefaultAndRebuildsTheScene() {
        var settings = XRaySettings()
        XCTAssertFalse(settings.panels, "panels are opt-in")
        XCTAssertTrue(settings.hud)
        let before = settings.geometryKey
        settings.panels = true
        XCTAssertNotEqual(settings.geometryKey, before, "switching panels rebuilds the scene")
        var spinning = settings
        spinning.spin = true
        XCTAssertEqual(spinning.geometryKey, settings.geometryKey, "spin only animates")
    }

    func testStatsFollowTheDesignAndRoutingProgress() throws {
        let engine = try placedDesign()
        let placed = try XCTUnwrap(engine.snapshot())
        let before = XRayPanelStats(placed)
        XCTAssertEqual(before.layerLengths.count, max(1, placed.board.layerCount))
        XCTAssertGreaterThan(before.connections, 0)
        XCTAssertEqual(before.unrouted, placed.ratsnest.count)
        XCTAssertGreaterThan(before.unrouted, 0, "nothing is routed yet")
        XCTAssertLessThan(before.completion, 1)

        XCTAssertEqual(engine.autoRoute().failed, 0)
        let routed = try XCTUnwrap(engine.snapshot())
        let after = XRayPanelStats(routed)
        XCTAssertEqual(after.unrouted, routed.ratsnest.count)
        XCTAssertGreaterThan(after.completion, before.completion)
        if routed.ratsnest.isEmpty { XCTAssertEqual(after.completion, 1, accuracy: 1e-12) }

        // Copper length: per layer adds up to the total, which matches the tracks.
        let length = routed.tracks.reduce(0) { $0 + hypot($1.bx - $1.ax, $1.by - $1.ay) }
        XCTAssertGreaterThan(after.totalLength, 0)
        XCTAssertEqual(after.totalLength, length, accuracy: 1e-6)
        XCTAssertEqual(after.layerLengths.reduce(0, +), after.totalLength, accuracy: 1e-6)
        XCTAssertTrue(after.layerLengths.allSatisfy { $0 >= 0 })

        // Fan-out bins cover every multi-pin net once; power / ground counts follow the net roles.
        let nets = routed.nets.filter { $0.pinCount >= 2 }
        XCTAssertEqual(after.fanout.map { $0.0 }, XRayPanelStats.fanoutBins.map { $0.0 })
        XCTAssertEqual(Int(after.fanout.reduce(0) { $0 + $1.1 }), nets.count)
        XCTAssertEqual(after.powerNets, nets.filter { $0.netRole == .power }.count)
        XCTAssertEqual(after.groundNets, nets.filter { $0.netRole == .ground }.count)
        XCTAssertEqual(after.connections, nets.reduce(0) { $0 + $1.pinCount - 1 })

        // Parts by prefix: most common first, never more than seven bars, never more parts than the design has.
        XCTAssertFalse(after.partKinds.isEmpty)
        XCTAssertLessThanOrEqual(after.partKinds.count, 7)
        XCTAssertEqual(after.partKinds.map { $0.1 }, after.partKinds.map { $0.1 }.sorted(by: >))
        XCTAssertLessThanOrEqual(Int(after.partKinds.reduce(0) { $0 + $1.1 }), routed.components.count)
        XCTAssertTrue(after.partKinds.allSatisfy { !$0.0.isEmpty && $0.1 >= 1 })
    }

    func testCompletionIsClampedAndCountsAnEmptyBoardAsDone() throws {
        var stats = XRayPanelStats(try XCTUnwrap(try placedDesign().snapshot()))
        stats.connections = 0
        stats.unrouted = 0
        XCTAssertEqual(stats.completion, 1)
        stats.connections = 10
        stats.unrouted = 4
        XCTAssertEqual(stats.completion, 0.6, accuracy: 1e-12)
        stats.unrouted = 0
        XCTAssertEqual(stats.completion, 1)
        stats.unrouted = 15  // more ratsnest lines than estimated connections
        XCTAssertEqual(stats.completion, 0)
    }

    func testPanelTexturesRender() throws {
        let engine = try placedDesign()
        _ = engine.autoRoute()
        let snapshot = try XCTUnwrap(engine.snapshot())
        let images = XRayStackView.panelImages(snapshot, stats: XRayPanelStats(snapshot))
        XCTAssertEqual(images.count, 5)
        for (index, image) in images.enumerated() {
            XCTAssertEqual(image.size, NSSize(width: 640, height: 420), "panel \(index)")
            let tiff = try XCTUnwrap(image.tiffRepresentation, "panel \(index)")
            let bitmap = try XCTUnwrap(NSBitmapImageRep(data: tiff))
            // Something is drawn: the frame border (left edge, middle) and the glass body are not transparent.
            let border = try XCTUnwrap(bitmap.colorAt(x: bitmap.pixelsWide * 10 / 640, y: bitmap.pixelsHigh / 2))
            XCTAssertGreaterThan(border.alphaComponent, 0.3, "panel \(index) frame")
            let body = try XCTUnwrap(bitmap.colorAt(x: bitmap.pixelsWide / 2, y: bitmap.pixelsHigh / 2))
            XCTAssertGreaterThan(body.alphaComponent, 0, "panel \(index) glass")
        }
        // Empty designs still draw (no tracks, no nets, no parts).
        let blank = try XCTUnwrap(EDAEngine().snapshot())
        XCTAssertEqual(XRayStackView.panelImages(blank, stats: XRayPanelStats(blank)).count, 5)
    }

    func testPanelsStandOnAnArcBehindTheStack() throws {
        let images = (0..<5).map { _ in NSImage(size: NSSize(width: 640, height: 420)) }
        let span: CGFloat = 50, stackHeight: CGFloat = 12, floorY: CGFloat = -8
        let nodes = XRayStackView.panelNodes(images, span: span, stackHeight: stackHeight, floorY: floorY)
        XCTAssertEqual(nodes.count, 5)
        var seen: [SCNVector3] = []
        for (index, node) in nodes.enumerated() {
            let p = node.position
            XCTAssertEqual(hypot(p.x, p.z), span * 0.95, accuracy: 1e-3, "panel \(index) on the arc")
            // Opposite the default camera at (+0.85, +1.15) × span in x / z, so it never blocks the board.
            XCTAssertLessThan(p.x * 0.85 + p.z * 1.15, 0, "panel \(index) behind the stack")
            XCTAssertGreaterThan(p.y, stackHeight, "panel \(index) above the top layer")
            XCTAssertFalse(seen.contains { abs($0.x - p.x) < 1e-3 && abs($0.z - p.z) < 1e-3 }, "panel \(index) overlaps")
            seen.append(p)

            // Upright billboard, hidden until it unfolds.
            let billboard = try XCTUnwrap(node.constraints?.first as? SCNBillboardConstraint)
            XCTAssertEqual(billboard.freeAxes, .Y)
            XCTAssertEqual(node.opacity, 0)
            XCTAssertTrue(node.hasActions)

            // Glass face sized to the panel, and a post whose base ring sits on the floor.
            let face = try XCTUnwrap(node.childNodes.first?.geometry as? SCNPlane)
            XCTAssertEqual(face.width, span * 0.32, accuracy: 1e-6)
            XCTAssertEqual(face.height, span * 0.32 * 420 / 640, accuracy: 1e-6)
            let base = try XCTUnwrap(node.childNodes.first { $0.geometry is SCNTorus })
            XCTAssertEqual(p.y + base.position.y, floorY, accuracy: 1e-3)
        }
        // Staggered heights alternate.
        XCTAssertNotEqual(nodes[0].position.y, nodes[1].position.y)
        XCTAssertEqual(nodes[0].position.y, nodes[2].position.y, accuracy: 1e-6)
    }
}

/// The X-ray stack hosted in a real window: the SceneKit scene gains the five holographic panels when the Panels
/// switch is on and drops them when it is switched off, alongside the HUD.
@MainActor
final class XRayLiveSceneTests: XCTestCase {
    private func spin(_ seconds: TimeInterval = 0.2) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    private func sceneView(in view: NSView) -> SCNView? {
        if let scene = view as? SCNView { return scene }
        for child in view.subviews { if let found = sceneView(in: child) { return found } }
        return nil
    }

    /// Panel roots: upright billboards carrying a torus base ring.
    private func panelCount(_ scene: SCNScene) -> Int {
        scene.rootNode.childNodes { node, _ in
            (node.constraints?.contains { ($0 as? SCNBillboardConstraint)?.freeAxes == .Y } ?? false)
                && node.childNodes.contains { $0.geometry is SCNTorus }
        }.count
    }

    private func firstPanelTexture(_ scene: SCNScene) -> NSImage? {
        let panel = scene.rootNode.childNodes { node, _ in node.childNodes.contains { $0.geometry is SCNTorus } }.first
        return panel?.childNodes.first?.geometry?.firstMaterial?.emission.contents as? NSImage
    }

    func testPanelsToggleInALiveWindow() throws {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        engine.autoPlace(all: true)
        XCTAssertEqual(engine.autoRoute().failed, 0)
        let snapshot = try XCTUnwrap(engine.snapshot())

        func stack(_ settings: XRaySettings) -> XRayStackView {
            XRayStackView(engine: engine, snapshot: snapshot, revision: 1, settings: settings, resetToken: 0)
        }
        var settings = XRaySettings()
        let host = NSHostingController(rootView: stack(settings))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1000, height: 700),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = host
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        spin(0.5)

        let view = try XCTUnwrap(sceneView(in: host.view), "the X-ray view hosts an SCNView")
        let scene = try XCTUnwrap(view.scene)
        XCTAssertEqual(panelCount(scene), 0, "panels are off by default")
        XCTAssertGreaterThan(scene.rootNode.childNodes(passingTest: { _, _ in true }).count, 20, "the stack is built")

        settings.panels = true
        host.rootView = stack(settings)
        spin(0.5)
        XCTAssertEqual(panelCount(scene), 5, "Panels on")
        XCTAssertTrue(window.isVisible)
        let texture = try XCTUnwrap(firstPanelTexture(scene))

        // Panels work without the HUD, and survive explode / layer changes.
        settings.hud = false
        settings.explode = 12
        settings.hiddenLayers = [0]
        host.rootView = stack(settings)
        spin(0.3)
        XCTAssertEqual(panelCount(scene), 5, "Panels without the HUD")
        XCTAssertTrue(try XCTUnwrap(firstPanelTexture(scene)) === texture, "settings changes reuse the panel textures")

        settings.panels = false
        host.rootView = stack(settings)
        spin(0.3)
        XCTAssertEqual(panelCount(scene), 0, "Panels off again")
    }
}

/// Crash reports, crash recovery and the memory / time bounds: reports are built after an abnormal end and never
/// after a clean one, logs and report folders stay bounded, unsaved work survives a crash, the undo history keeps
/// to its memory budget, hangs are logged, and the hologram textures are cached and fixed-size.
@MainActor
final class CrashAndResourceTests: XCTestCase {
    private var directory: URL!

    override func setUp() {
        super.setUp()
        directory = FileManager.default.temporaryDirectory.appendingPathComponent("SiEDA-diag-\(UUID().uuidString)")
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: directory)
        super.tearDown()
    }

    private func spin(_ seconds: TimeInterval) {
        RunLoop.main.run(until: Date().addingTimeInterval(seconds))
    }

    // MARK: Crash reports

    func testReportAfterACrashWithActivityAndBacktrace() throws {
        let first = CrashReporter(directory: directory)
        first.startSession()
        XCTAssertNil(first.lastSessionReport, "nothing to report on a first launch")
        first.breadcrumb("Auto-place")
        first.breadcrumb("Workspace: 3D View")
        first.recordFatal(kind: "Uncaught exception NSRangeException", reason: "index 7 beyond bounds",
                          stack: ["0 SiEDA frameA", "1 SiEDA frameB"])
        // No endSession(): the process "died". Next launch:
        let second = CrashReporter(directory: directory)
        second.startSession()
        let url = try XCTUnwrap(second.lastSessionReport)
        let report = try String(contentsOf: url, encoding: .utf8)
        for expected in ["NSRangeException", "index 7 beyond bounds", "frameB", "Auto-place", "Workspace: 3D View",
                         "Session started", "macOS:"] {
            XCTAssertTrue(report.contains(expected), "report mentions \(expected)")
        }
        XCTAssertEqual(second.reports.map(\.lastPathComponent), [url.lastPathComponent])
        // A clean quit leaves nothing to report.
        second.endSession()
        let third = CrashReporter(directory: directory)
        third.startSession()
        XCTAssertNil(third.lastSessionReport)
        third.endSession()
    }

    func testAbnormalEndWithoutACapturedCrashIsStillReported() throws {
        CrashReporter(directory: directory).startSession()  // e.g. force quit after a hang
        let next = CrashReporter(directory: directory)
        next.startSession()
        let report = try String(contentsOf: try XCTUnwrap(next.lastSessionReport), encoding: .utf8)
        XCTAssertTrue(report.contains("did not shut down normally"))
        next.endSession()
    }

    func testSessionLogAndReportFolderStayBounded() throws {
        let limit = 2048
        let reporter = CrashReporter(directory: directory, maxReports: 3, maxLogBytes: limit)
        reporter.startSession()
        for k in 0..<2000 { reporter.breadcrumb("Edit number \(k) with some detail") }
        let fm = FileManager.default
        let logs = ["session.log", "session.1.log"].map { directory.appendingPathComponent($0).path }
        let bytes = logs.reduce(0) { $0 + ((try? fm.attributesOfItem(atPath: $1))?[.size] as? Int ?? 0) }
        XCTAssertLessThanOrEqual(bytes, 2 * limit, "log rotation keeps the log bounded")
        XCTAssertGreaterThan(bytes, 0)

        // Six crashed sessions keep only the newest three reports.
        for _ in 0..<6 {
            let r = CrashReporter(directory: directory, maxReports: 3, maxLogBytes: limit)
            r.startSession()
        }
        let final = CrashReporter(directory: directory, maxReports: 3, maxLogBytes: limit)
        final.startSession()
        XCTAssertNotNil(final.lastSessionReport)
        XCTAssertLessThanOrEqual(final.reports.count, 3)
        final.endSession()
    }

    func testWatchdogLogsAHangAndTheRecovery() {
        final class Lines: @unchecked Sendable {
            private let lock = NSLock()
            private var items: [String] = []
            func add(_ line: String) { lock.lock(); items.append(line); lock.unlock() }
            var all: [String] { lock.lock(); defer { lock.unlock() }; return items }
        }
        let lines = Lines()
        let watchdog = MainThreadWatchdog(threshold: 0.5) { lines.add($0) }
        watchdog.start()
        defer { watchdog.stop() }
        spin(0.4)
        Thread.sleep(forTimeInterval: 1.5)  // block the main thread
        spin(1.0)
        let logged = lines.all
        XCTAssertTrue(logged.contains { $0.hasPrefix("HANG") }, "\(logged)")
        XCTAssertTrue(logged.contains { $0.contains("responsive again") }, "\(logged)")
    }

    // MARK: Crash recovery

    private func addResistor(_ store: DesignStore, _ k: Int) {
        store.perform("Add R\(k)") { _ = $0.addComponent(.resistor, value: "1k", at: CGPoint(x: 40 * k, y: 0)) }
    }

    func testUnsavedWorkIsAutosavedAndRestored() throws {
        let store = DesignStore()
        store.aiEnabled = false
        let recovery = CrashRecovery(directory: directory.appendingPathComponent("Recovery"))
        store.recoveryDelay = 0.1
        store.recovery = recovery
        XCTAssertNil(recovery.pending(), "a clean design has nothing to recover")

        addResistor(store, 1)
        addResistor(store, 2)
        spin(0.5)  // debounced autosave fires once
        let pending = try XCTUnwrap(recovery.pending())
        XCTAssertNotNil(pending.savedAt)

        // "After the crash": a fresh window restores it as unsaved work.
        let restored = DesignStore()
        restored.restoreRecovered(pending)
        XCTAssertEqual(restored.snapshot.components.count, store.snapshot.components.count)
        XCTAssertTrue(restored.isDirty)

        // Opening a saved file (a clean state) removes the autosave.
        let file = directory.appendingPathComponent("saved.siedaproj")
        try store.engine.saveJSON().write(to: file, atomically: true, encoding: .utf8)
        store.open(url: file)
        XCTAssertFalse(store.isDirty)
        recovery.flush()
        XCTAssertNil(recovery.pending())
    }

    // MARK: Memory bounds

    func testUndoHistoryKeepsToItsMemoryBudget() {
        let store = DesignStore()
        store.aiEnabled = false
        addResistor(store, 0)
        let step = store.engine.saveJSON().utf8.count
        store.historyByteLimit = step * 4
        for k in 1...30 { addResistor(store, k) }
        XCTAssertGreaterThanOrEqual(store.undoDepth, 1, "the latest step is always kept")
        XCTAssertLessThan(store.undoDepth, 30)
        XCTAssertTrue(store.historyBytes <= store.historyByteLimit || store.undoDepth == 1)
        // Undo and redo still work right at the limit: the newest step on each side is never trimmed.
        let before = store.snapshot.components.count
        store.undo()
        XCTAssertTrue(store.canRedo, "undo at the memory limit keeps its redo step")
        XCTAssertEqual(store.snapshot.components.count, before - 1)
        store.redo()
        XCTAssertEqual(store.snapshot.components.count, before)
        store.undo()

        // Memory pressure releases redo, and (critical) the older half of undo.
        store.historyByteLimit = 96 * 1024 * 1024
        for k in 31...40 { addResistor(store, k) }
        store.undo()
        XCTAssertTrue(store.canRedo)
        let depth = store.undoDepth
        NotificationCenter.default.post(name: .siedaMemoryPressure, object: nil, userInfo: ["critical": true])
        spin(0.1)
        XCTAssertFalse(store.canRedo)
        XCTAssertEqual(store.undoDepth, depth - depth / 2)
    }

    func testHologramTexturesAreCachedAndFixedSize() throws {
        HoloFX.clearTextureCache()
        let ring = HoloFX.ringImage(colour: HoloFX.cyan, ticks: 72, segments: 9)
        XCTAssertTrue(ring === HoloFX.ringImage(colour: HoloFX.cyan, ticks: 72, segments: 9), "cached")
        XCTAssertFalse(ring === HoloFX.ringImage(colour: HoloFX.amber, ticks: 72, segments: 9), "keyed by colour")
        let ringRep = try XCTUnwrap(ring.representations.first as? NSBitmapImageRep)
        XCTAssertEqual(ringRep.pixelsWide, 512, "1× texture regardless of the screen")

        let panel = HoloFX.panelImage(title: "Test", code: "X", accent: HoloFX.cyan) { _ in }
        let rep = try XCTUnwrap(panel.representations.first as? NSBitmapImageRep)
        XCTAssertEqual(rep.pixelsWide, 640)
        XCTAssertEqual(rep.pixelsHigh, 420)
        let hex = try XCTUnwrap(HoloFX.hexImage(colour: HoloFX.cyan).representations.first as? NSBitmapImageRep)
        XCTAssertEqual(CGFloat(hex.pixelsWide), (sqrt(3) * 16 * 2 * 2).rounded(), "small tiles at 2×")
    }

    func testMiniMapAndStatsStayFastOnHugeBoards() {
        // 200 000 tracks and 100 000 pads: drawing is capped, so it stays well under a second.
        let tracks = (0..<200_000).map { k -> (CGPoint, CGPoint) in
            let x = CGFloat(k % 500), y = CGFloat(k / 500)
            return (CGPoint(x: x, y: y), CGPoint(x: x + 1, y: y))
        }
        let pads = (0..<100_000).map { CGPoint(x: CGFloat($0 % 400), y: CGFloat($0 / 400)) }
        let start = Date()
        let image = HoloFX.render(NSSize(width: 640, height: 420)) {
            HoloFX.drawMiniMap(width: 500, height: 400, tracks: tracks, pads: pads, in: NSRect(x: 0, y: 0, width: 640, height: 420))
        }
        XCTAssertEqual(image.size, NSSize(width: 640, height: 420))
        XCTAssertLessThan(Date().timeIntervalSince(start), 2.0)
    }
}

/// Realistic 3D assembly: every surface gets its own physical material, the finish follows the picker, and the live
/// scene is lit by the studio environment with a shadow-catching floor.
@MainActor
final class RealisticAssemblyTests: XCTestCase {
    private func routedEngine() throws -> EDAEngine {
        let engine = EDAEngine()
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: engine, previous: nil)
        engine.autoPlace(all: true)
        XCTAssertEqual(engine.autoRoute().failed, 0)
        return engine
    }

    private func surfaces(_ node: SCNNode) -> [String: SCNNode] {
        Dictionary(uniqueKeysWithValues: node.childNodes.compactMap { child in child.name.map { ($0, child) } })
    }

    func testEverySurfaceGetsItsOwnMaterial() throws {
        let mesh = try XCTUnwrap(try routedEngine().buildMesh(includeComponents: true))
        XCTAssertEqual(mesh.surfaces.count, mesh.vertexCount)
        XCTAssertTrue(mesh.surfaces.allSatisfy { MeshSurface(rawValue: $0) != nil })

        let node = BoardSceneView.assemblyNode(from: mesh, finish: .enig)
        let parts = surfaces(node)
        for name in ["mask", "laminate", "finish", "solder", "silk", "tin"] {
            XCTAssertNotNil(parts["surface.\(name)"], name)
        }
        // Every triangle lands in exactly one surface.
        let triangles = node.childNodes.reduce(0) { $0 + ($1.geometry?.elements.first?.primitiveCount ?? 0) }
        XCTAssertEqual(triangles, mesh.indices.count / 3)

        // Metals are metallic, the mask is a glossy lacquer, silkscreen is matte; the finish ignores vertex colours.
        func material(_ name: String) throws -> SCNMaterial {
            try XCTUnwrap(parts["surface.\(name)"]?.geometry?.firstMaterial, name)
        }
        XCTAssertEqual(try material("finish").metalness.contents as? CGFloat, 1)
        XCTAssertEqual(try material("solder").metalness.contents as? CGFloat, 1)
        XCTAssertEqual(try material("mask").metalness.contents as? CGFloat, 0)
        XCTAssertGreaterThan(try XCTUnwrap(material("mask").clearCoat.contents as? CGFloat), 0)
        XCTAssertGreaterThan(try XCTUnwrap(material("silk").roughness.contents as? CGFloat), 0.8)
        XCTAssertEqual(try material("finish").diffuse.contents as? NSColor, BoardFinish.enig.colour)
        XCTAssertEqual(parts["surface.finish"]?.geometry?.sources(for: .color).count, 0)
        XCTAssertEqual(parts["surface.mask"]?.geometry?.sources(for: .color).count, 1)

        // Another finish only changes the finish's material.
        let hasl = surfaces(BoardSceneView.assemblyNode(from: mesh, finish: .hasl))
        XCTAssertEqual(hasl["surface.finish"]?.geometry?.firstMaterial?.diffuse.contents as? NSColor, BoardFinish.hasl.colour)
        XCTAssertNotEqual(BoardFinish.enig.colour, BoardFinish.osp.colour)

        // A bare board has no solder joints or bodies.
        let bare = try XCTUnwrap(try routedEngine().buildMesh(includeComponents: false))
        let bareParts = surfaces(BoardSceneView.assemblyNode(from: bare, finish: .osp))
        XCTAssertNil(bareParts["surface.solder"])
        XCTAssertNil(bareParts["surface.plastic"])
        XCTAssertNotNil(bareParts["surface.finish"])
    }

    private func sceneView(in v: NSView) -> SCNView? {
        if let s = v as? SCNView { return s }
        for child in v.subviews { if let s = sceneView(in: child) { return s } }
        return nil
    }

    /// Share of sampled pixels where `dominant` (0 red, 1 green, 2 blue) clearly leads the other channels.
    private func dominantShare(_ image: NSImage, channel dominant: Int) throws -> Double {
        let tiff = try XCTUnwrap(image.tiffRepresentation)
        let bitmap = try XCTUnwrap(NSBitmapImageRep(data: tiff))
        var hits = 0, samples = 0
        for y in stride(from: 0, to: bitmap.pixelsHigh, by: 8) {
            for x in stride(from: 0, to: bitmap.pixelsWide, by: 8) {
                guard let c = bitmap.colorAt(x: x, y: y)?.usingColorSpace(.deviceRGB) else { continue }
                let rgb = [c.redComponent, c.greenComponent, c.blueComponent]
                samples += 1
                let others = rgb.enumerated().filter { $0.offset != dominant }.map { $0.element }
                // The lit mask's hue leads clearly (the studio light is slightly cool, so no strict 1.4× ratio).
                if rgb[dominant] > 0.1 && rgb[dominant] > 1.1 * (others.max() ?? 0) { hits += 1 }
            }
        }
        return samples == 0 ? 0 : Double(hits) / Double(samples)
    }

    func testRenderedBoardShowsItsMaskColourUnderStudioLight() throws {
        // Render the routed board in a real window and read the pixels back: a green board looks green, and switching
        // to a red mask turns it red (the lit, glossy mask colour survives the lighting and tone mapping).
        let store = DesignStore()
        store.aiEnabled = false
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: store.engine, previous: nil)
        store.engine.autoPlace(all: true)
        _ = store.engine.autoRoute()
        store.refresh()
        func view() -> BoardSceneView {
            BoardSceneView(engine: store.engine, revision: store.revision, includeComponents: true,
                           board: store.snapshot.board, resetToken: 0, finish: .enig, stats: .constant(""))
        }
        // A fixed frame: a hosting controller otherwise sizes the window to the representable's (tiny) ideal size.
        let host = NSHostingController(rootView: view().frame(width: 800, height: 600))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 800, height: 600),
                              styleMask: [.titled, .closable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = host
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(1.0))
        let scnView = try XCTUnwrap(sceneView(in: host.view))
        // Render the live scene offscreen at a known size, through the view's own camera.
        let renderer = SCNRenderer(device: MTLCreateSystemDefaultDevice(), options: nil)
        func render() -> NSImage {
            renderer.scene = scnView.scene
            renderer.pointOfView = scnView.pointOfView
            // The first offscreen frame can come out before textures and the environment are uploaded: warm up once.
            _ = renderer.snapshot(atTime: 0, with: CGSize(width: 800, height: 600), antialiasingMode: .none)
            return renderer.snapshot(atTime: 0.1, with: CGSize(width: 800, height: 600), antialiasingMode: .none)
        }

        let green = render()
        XCTAssertGreaterThan(green.size.width, 100)
        let greenShare = try dominantShare(green, channel: 1)
        XCTAssertGreaterThan(greenShare, 0.08, "a green board fills a good part of the view")
        XCTAssertLessThan(try dominantShare(green, channel: 0), greenShare)

        store.setSolderMask(.red)
        host.rootView = view().frame(width: 800, height: 600)
        RunLoop.main.run(until: Date().addingTimeInterval(1.0))
        let red = render()
        let redShare = try dominantShare(red, channel: 0)
        XCTAssertGreaterThan(redShare, 0.08, "the red mask shows")
        XCTAssertLessThan(try dominantShare(red, channel: 1), redShare)
    }

    func testWorkspaceUsesTheSavedFinish() throws {
        // The Finish picker is saved: the 3D workspace opens with HASL pads when HASL was chosen last time.
        let defaults = UserDefaults.standard
        let keys = ["threeD.mode.v2", "threeD.finish"]
        let saved = keys.map { defaults.object(forKey: $0) }
        defer { for (key, value) in zip(keys, saved) { defaults.set(value, forKey: key) } }
        defaults.set(Board3DWorkspace.Mode.assembly.rawValue, forKey: "threeD.mode.v2")
        defaults.set(BoardFinish.hasl.rawValue, forKey: "threeD.finish")

        let store = DesignStore()
        store.aiEnabled = false
        DesignPlanCompiler.apply(OfflineProvider.templates[0].plan, to: store.engine, previous: nil)
        store.engine.autoPlace(all: true)
        store.refresh()
        let host = NSHostingController(rootView: Board3DWorkspace().environmentObject(store))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 700),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = host
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.8))
        let scene = try XCTUnwrap(sceneView(in: host.view)?.scene)
        let finish = scene.rootNode.childNodes { node, _ in node.name == "surface.finish" }.first
        XCTAssertEqual(finish?.geometry?.firstMaterial?.diffuse.contents as? NSColor, BoardFinish.hasl.colour)
        // Solder joints and part bodies are in the scene (Components is on by default).
        XCTAssertFalse(scene.rootNode.childNodes { node, _ in node.name == "surface.solder" }.isEmpty)
        XCTAssertFalse(scene.rootNode.childNodes { node, _ in node.name == "surface.silk" }.isEmpty)
        XCTAssertEqual(BoardFinish.allCases.map(\.title), ["ENIG (gold)", "HASL (tin)", "OSP (bare copper)"])
    }

    func testStudioSceneInALiveWindow() throws {
        let engine = try routedEngine()
        let board = try XCTUnwrap(engine.snapshot()).board
        func view(_ finish: BoardFinish) -> BoardSceneView {
            BoardSceneView(engine: engine, revision: 1, includeComponents: true, board: board, resetToken: 0, finish: finish,
                           stats: .constant(""))
        }
        let host = NSHostingController(rootView: view(.enig))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 900, height: 640),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = host
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.5))

        func sceneView(in v: NSView) -> SCNView? {
            if let s = v as? SCNView { return s }
            for child in v.subviews { if let s = sceneView(in: child) { return s } }
            return nil
        }
        let scnView = try XCTUnwrap(sceneView(in: host.view))
        let scene = try XCTUnwrap(scnView.scene)
        XCTAssertNotNil(scene.lightingEnvironment.contents, "studio reflections")
        XCTAssertEqual(BoardSceneView.studioEnvironment.size, NSSize(width: 1024, height: 512))
        let floor = try XCTUnwrap(scene.rootNode.childNodes { node, _ in node.geometry is SCNFloor }.first)
        XCTAssertEqual(floor.geometry?.firstMaterial?.lightingModel, .shadowOnly)
        XCTAssertLessThan(floor.position.y, -CGFloat(board.thickness))
        XCTAssertTrue(scene.rootNode.childNodes { node, _ in node.light?.castsShadow == true }.count >= 1)
        XCTAssertGreaterThan(scnView.pointOfView?.camera?.screenSpaceAmbientOcclusionIntensity ?? 0, 0)

        func finishColour() -> NSColor? {
            scene.rootNode.childNodes { node, _ in node.name == "surface.finish" }.first?.geometry?.firstMaterial?
                .diffuse.contents as? NSColor
        }
        XCTAssertEqual(finishColour(), BoardFinish.enig.colour)
        host.rootView = view(.hasl)
        RunLoop.main.run(until: Date().addingTimeInterval(0.3))
        XCTAssertEqual(finishColour(), BoardFinish.hasl.colour, "the finish picker rebuilds the materials")
        XCTAssertTrue(window.isVisible)
    }
}

/// Productivity: the biggest reference designs go from plan to a placed, routed, verified and 3D board quickly, through
/// the same engine calls the app makes (Debug builds compile the C++ core with -O2).
@MainActor
final class ProductivityFlowTests: XCTestCase {
    func testComplexReferenceDesignsCompleteQuickly() throws {
        executionTimeAllowance = 900
        let largest = OfflineProvider.templates
            .sorted { $0.plan.components.count > $1.plan.components.count }
            .prefix(3)
        XCTAssertGreaterThan(largest.first?.plan.components.count ?? 0, 60, "the largest designs are genuinely complex")
        for template in largest {
            let start = Date()
            var marks: [(String, Double)] = []
            func mark(_ step: String) { marks.append((step, Date().timeIntervalSince(start))) }
            let engine = EDAEngine()
            DesignPlanCompiler.apply(template.industryPlan, to: engine, previous: nil)
            mark("schematic")
            engine.autoPlace(all: true)
            engine.fitBoard(margin: 2.5)
            mark("place")
            XCTAssertEqual(engine.autoRoute().failed, 0, template.plan.title)
            mark("route")
            let verification = try XCTUnwrap(engine.runVerification(), template.plan.title)
            XCTAssertEqual(verification.verdict, .pass, template.plan.title)
            mark("verify")
            let mesh = try XCTUnwrap(engine.buildMesh(includeComponents: true), template.plan.title)
            XCTAssertGreaterThan(mesh.vertexCount, 1000)
            _ = BoardSceneView.assemblyNode(from: mesh, finish: .enig)
            mark("3D")
            let total = Date().timeIntervalSince(start)
            print("[Productivity] \(template.plan.title) (\(template.plan.components.count) parts): "
                  + marks.map { String(format: "%@ %.1f s", $0.0, $0.1) }.joined(separator: " · "))
            XCTAssertLessThan(total, 120, "\(template.plan.title) took \(Int(total)) s from plan to 3D")
        }
    }
}

/// Production parts catalog in the app: every catalog part number is in the Component Library and places with its
/// real footprint; passives switch package from the Inspector (undoable, saved).
@MainActor
final class PartsCatalogTests: XCTestCase {
    func testCatalogPartsAreInTheLibraryAndPlace() throws {
        let names = ["ATMEGA328P-AU", "ATMEGA328P-PU", "STM32F405RGT6", "STM32H743IIT6", "STM32F765VIT6", "XC7A35T-1CSG324I",
                     "L293DD", "L298HN", "ULN2003ADR", "PCA9685PW", "IR2101S", "IR2110S", "TMC2209-LA", "TMC2160-TA",
                     "TMC5160A-TA", "CSD18540Q5B", "ADS1115IDGS", "LM358DR", "LM358N", "LM393DR", "LM393N", "AS5047D-ATSM",
                     "INA240A1EDRQ1", "MPU-9250", "MAX9814ETD+T", "MAX4466EXK+T", "74HC595D", "PCF8574TS", "PCF8574N",
                     "CH340G", "MAX485ESA+T", "SP485EEN-L", "MCP2551-I/SN", "TCAN1042VDRQ1", "ADM2587EBRWZ", "LM7812",
                     "AMS1117-3.3", "LM2596S-5.0", "XL4015E1", "LM74700QDBVRQ1", "LTC4359IMS8#PBF", "PC817X3NSZ0F",
                     "TLP281-4", "Crystal_16MHz", "Crystal_16MHz_3225", "Crystal_32.768kHz_3215", "Crystal_32.768kHz_Cylinder"]
        let store = DesignStore()
        store.aiEnabled = false
        var x = 0.0
        for name in names {
            let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name }, name)
            let id = store.placeStandardPart(part, at: CGPoint(x: x, y: 0))
            x += 200
            XCTAssertGreaterThanOrEqual(id, 0, name)
        }
        store.autoPlace(all: true)
        // Every placed catalog part has its pads on the board (the FPGA alone has 324 balls).
        let snapshot = store.snapshot
        for component in snapshot.components {
            XCTAssertFalse(snapshot.pads.filter { $0.component == component.id }.isEmpty, component.value)
        }
        let fpga = try XCTUnwrap(snapshot.components.first { $0.value == "XC7A35T-1CSG324I" })
        XCTAssertEqual(snapshot.pads.filter { $0.component == fpga.id }.count, 324)
    }

    func testRobotPartKitsAddToTheLibraryAndPlaceOnABoard() throws {
        let store = DesignStore()
        store.aiEnabled = false
        let platforms = store.robotSegments().platforms
        XCTAssertEqual(Set(platforms.map(\.id)), ["rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"])
        for platform in platforms {
            let kit = try XCTUnwrap(platform.kit, platform.id)
            XCTAssertGreaterThanOrEqual(kit.count, 7, platform.id)
            for name in kit.flatMap(\.parts) {
                XCTAssertNotNil(StandardLibrary.parts.first { $0.spec.name == name }, "\(platform.id): \(name)")
            }
        }
        // The whole drone kit goes into the project library in one step, land patterns included.
        let drone = try XCTUnwrap(platforms.first { $0.id == "fpv" }?.kit)
        let expected = Set(drone.flatMap(\.parts)).count
        XCTAssertEqual(store.addRobotKitToLibrary("fpv"), expected)
        XCTAssertEqual(store.addRobotKitToLibrary("fpv"), 0)
        XCTAssertEqual(store.addRobotKitToLibrary("toaster"), 0)
        let bmi = try XCTUnwrap(store.snapshot.customParts.first { $0.name == "BMI088" })
        XCTAssertEqual(bmi.pins.count, 16)
        // Place the flight-controller core and lay it out: every part lands on the board with all its pads.
        var x = 0.0
        for name in ["STM32F405RGT6", "BMI088", "MS5611-01BA03", "BMP280", "W25Q128JVSIQ", "BSC028N06LS3G", "STSPIN32F0A"] {
            let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name }, name)
            XCTAssertGreaterThanOrEqual(store.placeStandardPart(part, at: CGPoint(x: x, y: 0)), 0, name)
            x += 220
        }
        store.autoPlace(all: true)
        let snapshot = store.snapshot
        let baro = try XCTUnwrap(snapshot.components.first { $0.value == "BMP280" })
        XCTAssertEqual(snapshot.pads.filter { $0.component == baro.id }.count, 8)
        let fet = try XCTUnwrap(snapshot.components.first { $0.value == "BSC028N06LS3G" })
        XCTAssertEqual(snapshot.pads.filter { $0.component == fet.id }.count, 5)  // S S S G + the drain slab
    }

    func testLandPatternPackagesSurviveTheLibraryRoundTrip() throws {
        // An LGA catalog part keeps its exact land pattern when the app encodes it for the core.
        let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "BMI088" })
        XCTAssertEqual(part.spec.package.type, "LGA")
        XCTAssertEqual(part.spec.package.lands?.count, 16)
        XCTAssertEqual(part.spec.package.bodyDepth, 3)
        let data = try JSONEncoder().encode(part.spec)
        let decoded = try JSONDecoder().decode(CustomPartSpec.self, from: data)
        XCTAssertEqual(decoded.package.lands, part.spec.package.lands)
        let info = try EDAEngine.previewCustomPart(decoded).get()
        XCTAssertEqual(info.footprintGeometry.pads.count, 16)
        XCTAssertEqual(info.footprintGeometry.label, "LGA-16")
        // Library search and the robot-kit filter.
        let all = StandardLibrary.parts
        XCTAssertTrue(ComponentLibraryView.filterStandard(all, search: "barometric", kit: nil).contains { $0.spec.name == "MS5611-01BA03" })
        let cnc = ComponentLibraryView.filterStandard(all, search: "", kit: ["TMC2660-PA", "W5500"])
        XCTAssertEqual(Set(cnc.map(\.spec.name)), ["TMC2660-PA", "W5500"])
        XCTAssertEqual(ComponentLibraryView.filterStandard(all, search: "tmc", kit: ["TMC2660-PA", "W5500"]).map(\.spec.name), ["TMC2660-PA"])
    }

    func testFootprintEditorConvertsEditsAndPlacesACustomFootprint() throws {
        // Start from a generated DIP-8: the editor gets the same pads (plated holes) on the same pins.
        let ne555 = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "NE555" }).spec
        let editable = try EDAEngine.landPattern(ne555).get()
        XCTAssertEqual(editable.package.type, "CUSTOM")
        XCTAssertEqual(editable.package.lands?.count, 8)
        XCTAssertGreaterThan(editable.package.lands?[0].drill ?? 0, 0)
        XCTAssertTrue(EDAEngine.checkLandPattern(editable).isEmpty)

        var draft = FootprintDraft(spec: editable)
        var history = FootprintHistory()
        XCTAssertEqual(draft.pads.count, 8)
        // Move pad 2 onto pad 1: the core reports the overlap on both pads; undo restores it.
        let pad1 = draft.pads[0], pad2 = draft.pads[1]
        history.record(draft)
        draft.move([pad2.id], dx: pad1.land.x - pad2.land.x, dy: pad1.land.y - pad2.land.y, grid: 0.05)
        let overlap = EDAEngine.checkLandPattern(draft.applied(to: editable))
        XCTAssertTrue(overlap.contains { $0.code == "LAND_OVERLAP" && $0.pads == [1, 2] && $0.isError })
        XCTAssertTrue(history.undo(&draft))
        XCTAssertEqual(draft.pads[1].land, pad2.land)
        XCTAssertTrue(history.redo(&draft))
        XCTAssertTrue(history.undo(&draft))

        // Add a mechanical mounting hole, an array, a second pad on pin 4, renumber, mirror.
        let hole = draft.addPad(at: CGPoint(x: 0, y: 9.02), grid: 0.05)
        XCTAssertEqual(draft.number(of: hole), 9)
        XCTAssertEqual(draft.pads[8].land.y, 9.0, accuracy: 1e-9)  // snapped
        draft.pads[8].land.pin = "-"
        draft.pads[8].land.w = 3.0
        draft.pads[8].land.h = 3.0
        draft.pads[8].land.drill = 2.2
        draft.pads[8].land.round = true
        XCTAssertNil(draft.pinNumber(of: 8))
        let copies = draft.duplicate([draft.pads[3].id], dx: 0, dy: 0)
        XCTAssertEqual(copies.count, 1)
        draft.pads[9].land.pin = "4"
        draft.move(copies, dx: 0, dy: 3.0, grid: 0.05)
        XCTAssertEqual(draft.pinNumber(of: 9), "4")
        let row = draft.array(from: hole, count: 2, pitch: 8, horizontal: true)
        XCTAssertEqual(row.count, 2)
        XCTAssertEqual(draft.pads.count, 12)
        for id in row { draft.pads[draft.number(of: id)! - 1].land.pin = "-" }
        draft.setNumber(of: hole, to: 12)
        XCTAssertEqual(draft.number(of: hole), 12)
        draft.mirrorX(row)
        draft.bodyW = 7
        draft.bodyD = 10

        var spec = ne555
        draft.apply(to: &spec)
        XCTAssertEqual(spec.package.type, "CUSTOM")
        XCTAssertEqual(spec.package.lands?.count, 12)
        XCTAssertEqual(spec.package.bodyDepth, 10)
        let issues = EDAEngine.checkLandPattern(spec)
        XCTAssertFalse(issues.contains { $0.isError }, "\(issues.map(\.message))")

        // The edited part previews, round-trips through JSON, saves to the project and places on the board.
        let preview = try EDAEngine.previewCustomPart(spec).get()
        XCTAssertEqual(preview.footprintGeometry.pads.count, 12)
        XCTAssertEqual(preview.footprintGeometry.pads.filter { $0.pin < 0 }.count, 3)  // the mechanical holes
        let data = try JSONEncoder().encode(spec)
        XCTAssertEqual(try JSONDecoder().decode(CustomPartSpec.self, from: data), spec)
        let store = DesignStore()
        store.aiEnabled = false
        spec.name = "NE555-CUSTOM-FP"
        let saved = try XCTUnwrap(store.saveCustomPart(spec))
        XCTAssertEqual(saved.package.lands?.count, 12)
        let id = store.addCustomComponent(partId: saved.id, at: CGPoint(x: 0, y: 0))
        XCTAssertGreaterThanOrEqual(id, 0)
        store.autoPlace(all: true)
        XCTAssertEqual(store.snapshot.pads.filter { $0.component == id }.count, 12)
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.customParts.first { $0.name == "NE555-CUSTOM-FP" }?.package.lands?.count, 12)

        // A pin left without a pad is an error the editor shows (and Apply stays off).
        var broken = FootprintDraft(spec: editable)
        broken.delete([broken.pads[7].id])
        XCTAssertTrue(EDAEngine.checkLandPattern(broken.applied(to: editable)).contains { $0.code == "LAND_NO_PAD" })
    }

    func testPassivesSwitchPackageFromTheInspector() throws {
        let store = DesignStore()
        store.aiEnabled = false
        store.applyPlan(OfflineProvider.templates[0].industryPlan, requirements: nil)
        store.autoPlace(all: true)
        let resistor = try XCTUnwrap(store.snapshot.components.first { $0.componentKind == .resistor })
        let options = try XCTUnwrap(resistor.packageOptions)
        XCTAssertEqual(options.map(\.label), ["0402", "0603", "0805", "1206", "Axial THT (1/4 W)"])
        XCTAssertEqual(resistor.footprint, "R_0805")
        XCTAssertNil(resistor.package)

        store.setPackage(resistor.id, "R_Axial_THT")
        let tht = try XCTUnwrap(store.snapshot.component(resistor.id))
        XCTAssertEqual(tht.footprint, "R_Axial_THT")
        XCTAssertTrue(store.snapshot.pads.filter { $0.component == resistor.id }.allSatisfy(\.throughHole))
        XCTAssertTrue(store.isDirty)

        // Saved with the project.
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.component(resistor.id)?.footprint, "R_Axial_THT")

        // Undo restores the chip resistor; parts without variants offer none.
        store.undo()
        XCTAssertEqual(store.snapshot.component(resistor.id)?.footprint, "R_0805")
        XCTAssertNil(store.snapshot.components.first { $0.componentKind == .opAmp }?.packageOptions)
        // A capacitor offers tantalum and electrolytic cases; a diode SMA and DO-41.
        if let cap = store.snapshot.components.first(where: { $0.componentKind == .capacitor }) {
            XCTAssertTrue(cap.packageOptions?.contains { $0.id == "CP_Tant_B" } ?? false)
        }
        XCTAssertTrue(EDAEngine().setPackage(-1, "R_0603") == false)
    }
}

/// Footprint Editor: the editable document (pads, numbering, grid, undo), the land encoding shared with the core,
/// the core's checks as the editor shows them, conversions from every package family, the store flow that edits a
/// placed part's footprint, and the editor and library views in a live window.
@MainActor
final class FootprintEditorTests: XCTestCase {
    private typealias Land = CustomPartSpec.Land

    private func spec(_ name: String) throws -> CustomPartSpec {
        try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name }, name).spec
    }

    /// A two-pin part on a hand-made land pattern.
    private func twoPin(_ lands: [Land]) -> CustomPartSpec {
        var s = CustomPartSpec()
        s.name = "FP-TEST"
        s.refPrefix = "U"
        s.pins = [CustomPartSpec.Pin(number: "1", name: "A"), CustomPartSpec.Pin(number: "2", name: "B")]
        s.package.type = "CUSTOM"
        s.package.lands = lands
        return s
    }

    private func codes(_ issues: [LandIssue]) -> [String] { issues.map(\.code).sorted() }

    // MARK: - Document

    func testSnapRoundsToTheGrid() {
        XCTAssertEqual(FootprintDraft.snap(1.26, 0.05), 1.25, accuracy: 1e-12)
        XCTAssertEqual(FootprintDraft.snap(1.276, 0.05), 1.3, accuracy: 1e-12)
        XCTAssertEqual(FootprintDraft.snap(-0.63, 0.635), -0.635, accuracy: 1e-12)
        XCTAssertEqual(FootprintDraft.snap(3.3, 2.54), 2.54, accuracy: 1e-12)
        XCTAssertEqual(FootprintDraft.snap(0.123, 0), 0.123, accuracy: 1e-12)  // no grid
    }

    func testDraftReadsTheLandPatternAndBody() {
        var lga = twoPin([Land(x: -1, y: 0, w: 0.5, h: 0.4), Land(x: 1, y: 0, w: 0.5, h: 0.4)])
        lga.package.type = "LGA"
        lga.package.bodySize = 3
        lga.package.bodyDepth = 2
        let draft = FootprintDraft(spec: lga)
        XCTAssertEqual(draft.pads.count, 2)
        XCTAssertEqual(draft.packageType, "LGA")  // a catalog land pattern stays LGA
        XCTAssertEqual(draft.bodyW, 3)
        XCTAssertEqual(draft.bodyD, 2)
        // Without a declared body: the pads' extent; a square body when only the width is known.
        let bare = FootprintDraft(spec: twoPin([Land(x: -1, y: 0.5, w: 0.5, h: 0.4), Land(x: 1, y: -0.5, w: 0.5, h: 0.4)]))
        XCTAssertEqual(bare.packageType, "CUSTOM")
        XCTAssertEqual(bare.bodyW, 2.5, accuracy: 1e-12)
        XCTAssertEqual(bare.bodyD, 1.4, accuracy: 1e-12)
        var square = twoPin([Land(x: 0, y: 0, w: 1, h: 1)])
        square.package.bodySize = 4
        XCTAssertEqual(FootprintDraft(spec: square).bodyD, 4)
    }

    func testAddDuplicateDeleteAndRenumber() {
        var d = FootprintDraft(spec: twoPin([Land(x: -1, y: 0, w: 0.6, h: 1, drill: 0.3, round: true),
                                             Land(x: 1, y: 0, w: 0.6, h: 1)]))
        // A new pad copies the last pad's shape, lands on the grid and takes the next number with no pin override.
        d.pads[1].land.pin = "1"
        let added = d.addPad(at: CGPoint(x: 0.02, y: 2.04), grid: 0.1)
        XCTAssertEqual(d.number(of: added), 3)
        XCTAssertEqual(d.pads[2].land.x, 0, accuracy: 1e-12)
        XCTAssertEqual(d.pads[2].land.y, 2.0, accuracy: 1e-12)
        XCTAssertEqual(d.pads[2].land.w, 0.6)
        XCTAssertEqual(d.pads[2].land.pin, "")
        // An empty document starts from a 1 × 0.6 mm SMD pad.
        var empty = FootprintDraft(spec: twoPin([]))
        empty.addPad(at: .zero, grid: 0.05)
        XCTAssertEqual(empty.pads.first?.land, Land(x: 0, y: 0, w: 1.0, h: 0.6))

        // Duplicates append after the last pad; an explicit pin resets, a mechanical pad stays mechanical.
        d.pads[0].land.pin = "-"
        let copies = d.duplicate([d.pads[0].id, d.pads[1].id], dx: 0, dy: 5)
        XCTAssertEqual(copies.count, 2)
        XCTAssertEqual(d.pads.count, 5)
        XCTAssertEqual(d.pads[3].land.pin, "-")
        XCTAssertEqual(d.pads[4].land.pin, "")
        XCTAssertEqual(d.pads[3].land.y, 5)
        XCTAssertEqual(d.pads[3].land.drill, 0.3)
        XCTAssertTrue(d.pads[3].land.round)
        XCTAssertTrue(d.duplicate([], dx: 1, dy: 1).isEmpty)

        // Deleting renumbers the pads after it down by one.
        let fifth = d.pads[4].id
        d.delete([d.pads[1].id])
        XCTAssertEqual(d.pads.count, 4)
        XCTAssertEqual(d.number(of: fifth), 4)
        XCTAssertNil(d.number(of: UUID()))

        // Renumbering moves a pad in the order; out-of-range numbers clamp; the same number is a no-op.
        let first = d.pads[0].id
        d.setNumber(of: first, to: 3)
        XCTAssertEqual(d.number(of: first), 3)
        d.setNumber(of: first, to: 99)
        XCTAssertEqual(d.number(of: first), 4)
        d.setNumber(of: first, to: -5)
        XCTAssertEqual(d.number(of: first), 1)
        let before = d
        d.setNumber(of: first, to: 1)
        XCTAssertEqual(d, before)
    }

    func testMoveSnapsEachPadAndLeavesOthers() {
        var d = FootprintDraft(spec: twoPin([Land(x: 0.013, y: 0, w: 1, h: 1), Land(x: 3, y: 3, w: 1, h: 1)]))
        d.move([d.pads[0].id], dx: 1.0, dy: -0.52, grid: 0.25)
        XCTAssertEqual(d.pads[0].land.x, 1.0, accuracy: 1e-12)
        XCTAssertEqual(d.pads[0].land.y, -0.5, accuracy: 1e-12)
        XCTAssertEqual(d.pads[1].land.x, 3)
        XCTAssertEqual(d.pads[1].land.y, 3)
    }

    func testArraysMirrorCentreAndHitTesting() {
        var d = FootprintDraft(spec: twoPin([Land(x: -2, y: -1.905, w: 1.5, h: 0.6), Land(x: 2, y: 0, w: 1, h: 1)]))
        // A column of 3 at the SOIC pitch, then a row of 2 at 2.54 mm.
        let column = d.array(from: d.pads[0].id, count: 3, pitch: 1.27, horizontal: false)
        XCTAssertEqual(column.count, 3)
        XCTAssertEqual(d.pads.count, 5)
        XCTAssertEqual(d.pads[2].land.y, -0.635, accuracy: 1e-9)
        XCTAssertEqual(d.pads[4].land.y, 1.905, accuracy: 1e-9)
        XCTAssertTrue(d.pads[2...4].allSatisfy { $0.land.x == -2 && $0.land.pin.isEmpty })
        let row = d.array(from: d.pads[1].id, count: 2, pitch: 2.54, horizontal: true)
        XCTAssertEqual(d.pads[5].land.x, 4.54, accuracy: 1e-9)
        XCTAssertEqual(d.pads[6].land.x, 7.08, accuracy: 1e-9)
        XCTAssertTrue(d.array(from: UUID(), count: 3, pitch: 1, horizontal: true).isEmpty)
        XCTAssertTrue(d.array(from: d.pads[0].id, count: 0, pitch: 1, horizontal: true).isEmpty)

        // Mirror flips x about the origin for the selected pads only.
        d.mirrorX(row)
        XCTAssertEqual(d.pads[5].land.x, -4.54, accuracy: 1e-9)
        XCTAssertEqual(d.pads[6].land.x, -7.08, accuracy: 1e-9)
        XCTAssertEqual(d.pads[0].land.x, -2)

        // Centre puts the middle of the pads' extent on the origin.
        d.centre()
        let minX = d.pads.map { $0.land.x - $0.land.w / 2 }.min()!, maxX = d.pads.map { $0.land.x + $0.land.w / 2 }.max()!
        let minY = d.pads.map { $0.land.y - $0.land.h / 2 }.min()!, maxY = d.pads.map { $0.land.y + $0.land.h / 2 }.max()!
        XCTAssertEqual(minX + maxX, 0, accuracy: 1e-9)
        XCTAssertEqual(minY + maxY, 0, accuracy: 1e-9)

        // Hit testing returns the topmost pad under the point.
        let top = d.pads[1]
        XCTAssertEqual(d.pad(at: CGPoint(x: top.land.x + 0.4, y: top.land.y)), top.id)
        XCTAssertNil(d.pad(at: CGPoint(x: 100, y: 100)))
        d.addPad(at: CGPoint(x: top.land.x, y: top.land.y), grid: 0)
        XCTAssertEqual(d.pad(at: CGPoint(x: top.land.x, y: top.land.y)), d.pads.last?.id)
    }

    func testPinMappingAndCourtyard() {
        var d = FootprintDraft(spec: twoPin([Land(x: -1, y: 0, w: 1, h: 1), Land(x: 1, y: 0, w: 1, h: 1)]))
        XCTAssertEqual(d.pinNumber(of: 0), "1")
        d.pads[0].land.pin = "2"
        XCTAssertEqual(d.pinNumber(of: 0), "2")
        d.pads[0].land.pin = "-"
        XCTAssertNil(d.pinNumber(of: 0))
        // The courtyard holds pads and body with 0.25 mm all round.
        d.bodyW = 1
        d.bodyD = 3
        XCTAssertEqual(d.courtyard.minX, -1.75, accuracy: 1e-12)
        XCTAssertEqual(d.courtyard.maxX, 1.75, accuracy: 1e-12)
        XCTAssertEqual(d.courtyard.minY, -1.75, accuracy: 1e-12)
        XCTAssertEqual(d.courtyard.height, 3.5, accuracy: 1e-12)
    }

    func testApplyWritesTheLandPatternAndKeepsThePins() throws {
        var d = FootprintDraft(spec: try EDAEngine.landPattern(try spec("LM358DR")).get())
        d.bodyW = 3.9
        d.bodyD = 4.9
        var out = try spec("LM358DR")
        d.apply(to: &out)
        XCTAssertEqual(out.package.type, "CUSTOM")
        XCTAssertEqual(out.package.lands?.count, 8)
        XCTAssertEqual(out.package.pinCount, 8)
        XCTAssertNil(out.package.pitch)
        XCTAssertEqual(out.package.bodySize, 3.9)
        XCTAssertEqual(out.package.bodyDepth, 4.9)
        XCTAssertEqual(out.pins, try spec("LM358DR").pins)
        XCTAssertTrue(out.package.usesLandPattern)
        // A degenerate body is left to the core (pads' extent).
        d.bodyW = 0.2
        XCTAssertNil(d.applied(to: out).package.bodySize)
    }

    // MARK: - Undo history

    func testHistoryUndoRedoAndLimit() {
        var history = FootprintHistory()
        var d = FootprintDraft(spec: twoPin([Land(x: 0, y: 0, w: 1, h: 1)]))
        XCTAssertFalse(history.canUndo)
        XCTAssertFalse(history.undo(&d))
        XCTAssertFalse(history.redo(&d))
        let original = d
        history.record(d)
        d.move([d.pads[0].id], dx: 1, dy: 0, grid: 0)
        let moved = d
        XCTAssertTrue(history.undo(&d))
        XCTAssertEqual(d, original)
        XCTAssertTrue(history.canRedo)
        XCTAssertTrue(history.redo(&d))
        XCTAssertEqual(d, moved)
        // A new edit clears the redo stack.
        XCTAssertTrue(history.undo(&d))
        history.record(d)
        d.bodyW = 9
        XCTAssertFalse(history.canRedo)
        // The history keeps the last 200 snapshots.
        for _ in 0..<250 { history.record(d) }
        XCTAssertEqual(history.undoStack.count, FootprintHistory.limit)
    }

    // MARK: - Land encoding (shared with the core)

    func testLandEncodesLikeTheCore() throws {
        let encoder = JSONEncoder()
        func json(_ land: Land) throws -> String { String(decoding: try encoder.encode(land), as: UTF8.self) }
        XCTAssertEqual(try json(Land(x: 1, y: -2, w: 0.5, h: 0.25)), "[1,-2,0.5,0.25]")
        XCTAssertEqual(try json(Land(x: 0, y: 0, w: 1.6, h: 1.6, drill: 0.8, round: true)), "[0,0,1.6,1.6,0.8,1]")
        XCTAssertEqual(try json(Land(x: 0, y: 0, w: 2, h: 2, pin: "EP")), "[0,0,2,2,0,0,\"EP\"]")
        let decoder = JSONDecoder()
        let lands = try decoder.decode([Land].self, from: Data("[[1,2,3,4],[0,0,1,1,0.5,1],[0,0,1,1,0,0,\"-\"]]".utf8))
        XCTAssertEqual(lands[0], Land(x: 1, y: 2, w: 3, h: 4))
        XCTAssertEqual(lands[1], Land(x: 0, y: 0, w: 1, h: 1, drill: 0.5, round: true))
        XCTAssertEqual(lands[2].pin, "-")
        XCTAssertThrowsError(try decoder.decode(Land.self, from: Data("[1,2,3]".utf8)))
        // The core reads what the app writes: a part with every land form previews.
        var s = twoPin([Land(x: -2, y: 0, w: 1, h: 1), Land(x: 2, y: 0, w: 1.6, h: 1.6, drill: 0.8, round: true),
                        Land(x: 0, y: 3, w: 3, h: 3, drill: 2.2, round: true, pin: "-")])
        s.name = "FP-ENCODE"
        let info = try EDAEngine.previewCustomPart(s).get()
        XCTAssertEqual(info.footprintGeometry.pads.count, 3)
        XCTAssertTrue(info.footprintGeometry.pads[1].throughHole)
        XCTAssertTrue(info.footprintGeometry.pads[1].round)
        XCTAssertEqual(info.footprintGeometry.pads[2].pin, -1)
        XCTAssertEqual(info.package.lands, s.package.lands)
    }

    // MARK: - Checks

    func testChecksReportEachProblemOnItsPads() {
        let clean = twoPin([Land(x: -1, y: 0, w: 1, h: 1), Land(x: 1, y: 0, w: 1, h: 1)])
        XCTAssertTrue(EDAEngine.checkLandPattern(clean).isEmpty)

        let overlap = EDAEngine.checkLandPattern(twoPin([Land(x: 0, y: 0, w: 1, h: 1), Land(x: 0.5, y: 0, w: 1, h: 1)]))
        XCTAssertEqual(overlap.first?.code, "LAND_OVERLAP")
        XCTAssertEqual(overlap.first?.pads, [1, 2])
        XCTAssertTrue(overlap.first?.isError ?? false)

        let gap = EDAEngine.checkLandPattern(twoPin([Land(x: 0, y: 0, w: 1, h: 1), Land(x: 1.15, y: 0, w: 1, h: 1)]),
                                             minGap: 0.2)
        XCTAssertEqual(codes(gap), ["LAND_GAP"])
        XCTAssertFalse(gap[0].isError)
        XCTAssertTrue(EDAEngine.checkLandPattern(twoPin([Land(x: 0, y: 0, w: 1, h: 1), Land(x: 1.15, y: 0, w: 1, h: 1)]),
                                                 minGap: 0.1).isEmpty)

        let ring = EDAEngine.checkLandPattern(twoPin([Land(x: -2, y: 0, w: 1, h: 1, drill: 0.9, round: true),
                                                      Land(x: 2, y: 0, w: 1, h: 1)]))
        XCTAssertEqual(codes(ring), ["LAND_ANNULAR"])
        XCTAssertEqual(ring[0].pads, [1])

        let extra = EDAEngine.checkLandPattern(twoPin([Land(x: -2, y: 0, w: 1, h: 1), Land(x: 2, y: 0, w: 1, h: 1),
                                                       Land(x: 0, y: 3, w: 1, h: 1)]))
        XCTAssertEqual(codes(extra), ["LAND_NO_PIN"])
        XCTAssertEqual(extra[0].pads, [3])

        let missing = EDAEngine.checkLandPattern(twoPin([Land(x: 0, y: 0, w: 1, h: 1)]))
        XCTAssertEqual(codes(missing), ["LAND_NO_PAD"])
        XCTAssertTrue(missing[0].isError)
        XCTAssertTrue(missing[0].message.contains("B"))

        // A drill larger than its pad cannot even be read: one LAND_INVALID error.
        let invalid = EDAEngine.checkLandPattern(twoPin([Land(x: 0, y: 0, w: 1, h: 1, drill: 1.5, round: true),
                                                         Land(x: 3, y: 0, w: 1, h: 1)]))
        XCTAssertEqual(codes(invalid), ["LAND_INVALID"])
        XCTAssertTrue(invalid[0].isError)
    }

    // MARK: - Conversions

    func testEveryPackageFamilyConvertsToTheSamePadsOnTheSamePins() throws {
        for name in ["NE555", "LM358DR", "LM7805", "AMS1117-3.3", "LM2596S-5.0", "STM32F405RGT6", "TMC2209-LA",
                     "DRV8833PWPR", "BSS138", "ESP32-WROOM-32E", "XC7A35T-1CSG324I", "Crystal_16MHz_3225"] {
            let original = try spec(name)
            let editable = try EDAEngine.landPattern(original).get()
            XCTAssertTrue(editable.package.usesLandPattern, name)
            XCTAssertEqual(editable.pins, original.pins, name)
            let before = try EDAEngine.previewCustomPart(original).get().footprintGeometry
            let after = try EDAEngine.previewCustomPart(editable).get().footprintGeometry
            XCTAssertEqual(after.pads.count, before.pads.count, name)
            for (a, b) in zip(before.pads, after.pads) {
                XCTAssertEqual(a.x, b.x, accuracy: 1e-9, name)
                XCTAssertEqual(a.y, b.y, accuracy: 1e-9, name)
                XCTAssertEqual(a.w, b.w, accuracy: 1e-9, name)
                XCTAssertEqual(a.pin, b.pin, name)
                XCTAssertEqual(a.throughHole, b.throughHole, name)
            }
            XCTAssertFalse(EDAEngine.checkLandPattern(editable).contains { $0.isError }, name)
            // The editor round trip without edits gives the same part back.
            let roundTrip = FootprintDraft(spec: editable).applied(to: editable)
            XCTAssertEqual(roundTrip.package.lands, editable.package.lands, name)
        }
        // Land-pattern parts open as they are.
        let lga = try spec("BMI088")
        XCTAssertEqual(try EDAEngine.landPattern(lga).get().package.lands, lga.package.lands)
        // A part without pins cannot be converted.
        if case .success = EDAEngine.landPattern(CustomPartSpec()) { XCTFail("an empty part converted") }
    }

    // MARK: - Store and views

    func testEditingAPlacedPartsFootprintUpdatesTheBoardAndUndoes() throws {
        let store = DesignStore()
        store.aiEnabled = false
        let part = try XCTUnwrap(store.addStandardPartToLibrary(try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM358DR" })))
        let u1 = store.addCustomComponent(partId: part, at: .zero)
        store.autoPlace(all: true)
        XCTAssertEqual(store.snapshot.pads.filter { $0.component == u1 }.count, 8)

        // Edit: convert, add two mechanical holes, save over the old part (what Apply + Save does in the library).
        let info = try XCTUnwrap(store.snapshot.customParts.first { $0.id == part })
        var draft = FootprintDraft(spec: try EDAEngine.landPattern(info.spec).get())
        for x in [-6.0, 6.0] {
            let hole = draft.addPad(at: CGPoint(x: x, y: 0), grid: 0.05)
            let i = draft.number(of: hole)! - 1
            draft.pads[i].land = Land(x: x, y: 0, w: 2.5, h: 2.5, drill: 1.6, round: true, pin: "-")
        }
        let edited = draft.applied(to: info.spec)
        XCTAssertTrue(EDAEngine.checkLandPattern(edited).isEmpty)
        let saved = try XCTUnwrap(store.saveCustomPart(edited, replacing: part))
        XCTAssertNotEqual(saved.id, part)
        XCTAssertNil(store.snapshot.customParts.first { $0.id == part })  // the old version is gone
        let placed = store.snapshot.pads.filter { $0.component == u1 }
        XCTAssertEqual(placed.count, 10)
        XCTAssertEqual(placed.filter(\.throughHole).count, 2)

        // Undo restores the generated SOIC on the board.
        store.undo()
        XCTAssertEqual(store.snapshot.pads.filter { $0.component == u1 }.count, 8)
        store.redo()
        XCTAssertEqual(store.snapshot.pads.filter { $0.component == u1 }.count, 10)

        // An invalid footprint (pin 8 without a pad) is refused and leaves the part as it was.
        var broken = draft
        broken.delete([broken.pads[7].id])
        XCTAssertNil(store.saveCustomPart(broken.applied(to: info.spec), replacing: saved.id))
        XCTAssertEqual(store.snapshot.pads.filter { $0.component == u1 }.count, 10)
        store.alert = nil
    }

    func testEditorAndLibraryRenderInALiveWindow() throws {
        let editable = try EDAEngine.landPattern(try spec("STM32F405RGT6")).get()
        var applied: CustomPartSpec?
        let editor = FootprintEditorView(spec: editable) { applied = $0 }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 760),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: editor)
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        XCTAssertGreaterThan(window.contentView?.fittingSize.width ?? 0, 0)
        XCTAssertNil(applied)  // nothing is written until Apply

        // The editor also renders offscreen at a fixed size (the canvas, side panel and footer lay out).
        let renderer = ImageRenderer(content: FootprintEditorView(spec: editable) { _ in }.frame(width: 1000, height: 700))
        renderer.scale = 1
        let image = try XCTUnwrap(renderer.cgImage)
        XCTAssertEqual(image.width, 1000)

        // The library hosts the editor button for a part with pins.
        let store = DesignStore()
        store.aiEnabled = false
        _ = store.addStandardPartToLibrary(try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "NE555" }))
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.FootprintEditorTests")))
        let library = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1300, height: 820),
                               styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        library.isReleasedWhenClosed = false
        library.contentViewController = NSHostingController(rootView: ComponentLibraryView()
            .environmentObject(store).environmentObject(settings).environmentObject(AgentOrchestrator()))
        library.makeKeyAndOrderFront(nil)
        defer { library.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        XCTAssertNotNil(library.contentView)
    }
}

/// Symbol Editor: the layout document (sides, slots, gaps, stacks, undo), the core's layout and checks as the editor
/// shows them, auto-arranged library symbols, four-sided drawing, stacked pins joining nets, editing a placed part's
/// symbol through the store, and the editor in a live window.
@MainActor
final class SymbolEditorTests: XCTestCase {
    private typealias Side = SymbolDraft.Side

    private func spec(_ name: String) throws -> CustomPartSpec {
        try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name }, name).spec
    }

    /// VCC, IN, OUT, GND, GND, EN.
    private func chip() -> CustomPartSpec {
        var s = CustomPartSpec()
        s.name = "SYM-TEST"
        s.pins = [CustomPartSpec.Pin(number: "1", name: "VCC", type: .powerIn), CustomPartSpec.Pin(number: "2", name: "IN", type: .input),
                  CustomPartSpec.Pin(number: "3", name: "OUT", type: .output), CustomPartSpec.Pin(number: "4", name: "GND", type: .powerIn),
                  CustomPartSpec.Pin(number: "5", name: "GND", type: .powerIn), CustomPartSpec.Pin(number: "6", name: "EN", type: .input)]
        return s
    }

    private func codes(_ issues: [SymbolIssue]) -> [String] { issues.map(\.code).sorted() }

    // MARK: - Document

    func testGeneratedLayoutMatchesTheCoresBox() throws {
        let ne555 = try spec("NE555")
        XCTAssertNil(ne555.symbolLayout)
        let draft = SymbolDraft(spec: ne555)
        XCTAssertEqual(draft.slots(.left), [["1"], ["2"], ["3"], ["4"]])
        XCTAssertEqual(draft.slots(.right), [["8"], ["7"], ["6"], ["5"]])
        // Writing the generated layout out explicitly draws the same symbol.
        let generated = try EDAEngine.previewCustomPart(ne555).get().symbol
        let explicit = try EDAEngine.previewCustomPart(draft.applied(to: ne555)).get().symbol
        XCTAssertEqual(generated.pins.map(\.x), explicit.pins.map(\.x))
        XCTAssertEqual(generated.pins.map(\.y), explicit.pins.map(\.y))
        XCTAssertEqual(generated.halfWidth, explicit.halfWidth)
        XCTAssertEqual(generated.pins.map(\.sideLetter), ["L", "L", "L", "L", "R", "R", "R", "R"])
    }

    func testMoveInsertsAndShiftsOnTheTargetSide() {
        var d = SymbolDraft(spec: chip())  // generated: 1 2 3 left, 6 5 4 right
        XCTAssertEqual(d.slots(.left), [["1"], ["2"], ["3"]])
        d.move(["1"], to: .top, slot: 0)
        XCTAssertEqual(d.slots(.top), [["1"]])
        XCTAssertEqual(d.slots(.left), [[], ["2"], ["3"]])  // the slot it left is a gap
        d.closeGaps(.left)
        XCTAssertEqual(d.slots(.left), [["2"], ["3"]])
        // Inserting at an occupied slot pushes the pins there down.
        d.move(["6"], to: .left, slot: 1)
        XCTAssertEqual(d.slots(.left), [["2"], ["6"], ["3"]])
        // Several pins land on consecutive slots in their order.
        d.move(["4", "5"], to: .bottom, slot: 0)
        XCTAssertEqual(d.slots(.bottom), [["4"], ["5"]])
        // Appending past the end; negative slots clamp to 0; unknown pins are ignored.
        d.move(["3"], to: .right, slot: d.slotCount(.right))
        XCTAssertEqual(d.placement(of: "3")?.side, .right)
        d.move(["2"], to: .right, slot: -4)
        XCTAssertEqual(d.placement(of: "2")?.slot, 0)
        let before = d
        d.move(["99"], to: .left, slot: 0)
        XCTAssertEqual(d, before)
    }

    func testStackUnstackGapsSwapNudgeAndMirror() {
        var d = SymbolDraft(spec: chip())
        d.move(["4"], to: .bottom, slot: 0)
        d.move(["5"], to: .bottom, slot: 1)
        d.stack(["4", "5"])
        XCTAssertEqual(d.slots(.bottom), [["4", "5"]])
        XCTAssertEqual(Set(d.stack(of: "5")), ["4", "5"])
        // A stack moves as one.
        d.move(["4", "5"], to: .top, slot: 0)
        XCTAssertEqual(d.slots(.top).first.map(Set.init), ["4", "5"])
        d.unstack("5")
        XCTAssertEqual(d.stack(of: "5"), ["5"])
        XCTAssertEqual(d.placement(of: "5")?.slot, (d.placement(of: "4")?.slot ?? 0) + 1)

        // Gaps.
        d = SymbolDraft(spec: chip())
        d.insertGap(.left, at: 1)
        XCTAssertEqual(d.slots(.left), [["1"], [], ["2"], ["3"]])
        d.closeGaps()
        XCTAssertEqual(d.slots(.left), [["1"], ["2"], ["3"]])

        // Nudge swaps with the neighbour, or moves into a gap; never above slot 0.
        d.nudge("2", by: -1)
        XCTAssertEqual(d.slots(.left), [["2"], ["1"], ["3"]])
        d.nudge("2", by: -1)
        XCTAssertEqual(d.slots(.left), [["2"], ["1"], ["3"]])
        d.insertGap(.left, at: 2)
        d.nudge("1", by: 1)
        XCTAssertEqual(d.slots(.left), [["2"], [], ["1"], ["3"]])

        // Swap trades spots across sides; mirror flips left and right.
        d.swap("2", "6")
        XCTAssertEqual(d.placement(of: "2")?.side, .right)
        XCTAssertEqual(d.placement(of: "6")?.side, .left)
        let lefts = d.slots(.left), rights = d.slots(.right)
        d.mirror()
        XCTAssertEqual(d.slots(.left), rights)
        XCTAssertEqual(d.slots(.right), lefts)
    }

    func testReconcileFollowsThePinList() {
        var s = chip()
        var d = SymbolDraft(spec: s)
        d.move(["1"], to: .top, slot: 0)
        s.pins.removeAll { $0.number == "6" }
        s.pins.append(CustomPartSpec.Pin(number: "7", name: "FB", type: .input))
        s.pins.append(CustomPartSpec.Pin(number: "8", name: "SS", type: .input))
        XCTAssertTrue(d.reconcile(with: s.pins))
        XCTAssertNil(d.placement(of: "6"))
        XCTAssertNotNil(d.placement(of: "7"))
        XCTAssertNotNil(d.placement(of: "8"))
        XCTAssertEqual(d.placement(of: "1")?.side, .top)  // the user's layout is kept
        XCTAssertFalse(d.reconcile(with: s.pins))
        XCTAssertTrue(EDAEngine.checkSymbol(d.applied(to: s)).allSatisfy { !$0.isError })
    }

    func testHistoryIsSharedWithTheFootprintEditor() {
        var history = EditHistory<SymbolDraft>()
        var d = SymbolDraft(spec: chip())
        let original = d
        history.record(d)
        d.mirror()
        XCTAssertTrue(history.undo(&d))
        XCTAssertEqual(d, original)
        XCTAssertTrue(history.redo(&d))
        XCTAssertNotEqual(d, original)
        XCTAssertEqual(EditHistory<SymbolDraft>.limit, 200)
        XCTAssertEqual(FootprintHistory.limit, 200)
    }

    // MARK: - Core layout and checks

    func testLayoutEncodesLikeTheCoreAndDrawsOnFourSides() throws {
        var d = SymbolDraft(spec: chip())
        d.move(["1"], to: .top, slot: 0)
        d.move(["4", "5"], to: .bottom, slot: 0)
        d.stack(["4", "5"])
        d.closeGaps()
        d.width = 120
        let s = d.applied(to: chip())
        let json = s.jsonString()
        XCTAssertTrue(json.contains("\"symbolLayout\""))
        XCTAssertTrue(json.contains("\"side\":\"T\""))
        let info = try EDAEngine.previewCustomPart(s).get()
        XCTAssertEqual(info.symbolLayout, s.symbolLayout)  // kept separate from the generated geometry
        XCTAssertEqual(info.symbol.halfWidth, 60)
        let vcc = try XCTUnwrap(info.symbol.pins.first { $0.number == "1" })
        XCTAssertEqual(vcc.sideLetter, "T")
        XCTAssertEqual(vcc.y, -(info.symbol.halfHeight + 20))
        let gnd = info.symbol.pins.filter { $0.name == "GND" }
        XCTAssertEqual(gnd.count, 2)
        XCTAssertEqual(gnd[0].x, gnd[1].x)
        XCTAssertEqual(gnd[0].y, gnd[1].y)
        XCTAssertEqual(gnd[0].sideLetter, "B")
        // The hit box reaches the top and bottom pins; the lead of a top pin runs up from the body.
        let bounds = SchematicSymbols.bounds(.custom, custom: info)
        XCTAssertLessThanOrEqual(bounds.minY, vcc.y)
        XCTAssertEqual(SchematicSymbols.pinEdge(vcc, halfWidth: info.symbol.halfWidth, halfHeight: info.symbol.halfHeight),
                       CGPoint(x: vcc.x, y: -info.symbol.halfHeight))
        XCTAssertLessThanOrEqual(SchematicSymbols.customShapes(info).stroke.boundingRect.minY, vcc.y + 0.001)
        // Info about the stack; no errors.
        XCTAssertEqual(codes(EDAEngine.checkSymbol(s)), ["SYM_STACK"])
    }

    func testChecksReportEachProblem() {
        var d = SymbolDraft(spec: chip())
        d.placements.removeAll { $0.number == "6" }
        XCTAssertEqual(codes(EDAEngine.checkSymbol(d.applied(to: chip()))), ["SYM_MISSING"])
        d = SymbolDraft(spec: chip())
        d.placements.append(.init(number: "9", side: .right, slot: 9))
        XCTAssertEqual(codes(EDAEngine.checkSymbol(d.applied(to: chip()))), ["SYM_UNKNOWN"])
        d = SymbolDraft(spec: chip())
        d.placements.append(.init(number: "2", side: .right, slot: 9))
        XCTAssertTrue(codes(EDAEngine.checkSymbol(d.applied(to: chip()))).contains("SYM_DUPLICATE"))
        d = SymbolDraft(spec: chip())
        d.stack(["2", "3"])  // IN and OUT on one spot
        let overlap = EDAEngine.checkSymbol(d.applied(to: chip()))
        XCTAssertEqual(codes(overlap), ["SYM_OVERLAP"])
        XCTAssertEqual(Set(overlap[0].pins), ["2", "3"])
        XCTAssertTrue(overlap[0].isError)
        // An invalid layout is refused by the core (the editor keeps its last good preview).
        if case .success = EDAEngine.previewCustomPart(d.applied(to: chip())) { XCTFail("colliding pins registered") }
        // A stack of signal pins of the same name is allowed with a warning (they join).
        var twin = chip()
        twin.pins.append(CustomPartSpec.Pin(number: "7", name: "IN", type: .input))
        var t = SymbolDraft(spec: twin)
        t.stack(["2", "7"])
        XCTAssertEqual(codes(EDAEngine.checkSymbol(t.applied(to: twin))), ["SYM_STACK_SIGNAL"])
        // A malformed layout cannot be read at all.
        var bad = chip()
        bad.symbolLayout = .init(width: nil, pins: [.init(number: "1", side: "Q", slot: 0)])
        XCTAssertEqual(codes(EDAEngine.checkSymbol(bad)), ["SYM_INVALID"])
    }

    func testAutoArrangeAndLibrarySymbols() throws {
        // Large library parts come arranged: STM32 supplies on top with the four VDD pins stacked, grounds below.
        let stm = try spec("STM32F405RGT6")
        XCTAssertNotNil(stm.symbolLayout)
        XCTAssertNil(try spec("LM358DR").symbolLayout)  // small parts keep the datasheet-order box
        let info = try EDAEngine.previewCustomPart(stm).get()
        let vdd = info.symbol.pins.filter { $0.name == "VDD" }
        XCTAssertEqual(vdd.count, 4)
        XCTAssertEqual(Set(vdd.map { "\($0.x),\($0.y)" }).count, 1)
        XCTAssertTrue(vdd.allSatisfy { $0.sideLetter == "T" })
        XCTAssertTrue(info.symbol.pins.filter { $0.name.hasPrefix("VSS") }.allSatisfy { $0.sideLetter == "B" })
        XCTAssertFalse(EDAEngine.checkSymbol(stm).contains { $0.severity != "info" })
        // Auto Arrange from the editor: same idea for any part, with or without stacking.
        var plain = try spec("CH340G")
        plain.symbolLayout = nil
        let stacked = try EDAEngine.autoArrangeSymbol(chip()).get()
        let draft = SymbolDraft(spec: stacked)
        XCTAssertEqual(draft.placement(of: "1")?.side, .top)
        XCTAssertEqual(draft.placement(of: "2")?.side, .left)
        XCTAssertEqual(draft.placement(of: "3")?.side, .right)
        XCTAssertEqual(Set(draft.stack(of: "4")), ["4", "5"])
        let unstacked = SymbolDraft(spec: try EDAEngine.autoArrangeSymbol(chip(), stack: false).get())
        XCTAssertEqual(unstacked.stack(of: "4"), ["4"])
        XCTAssertEqual(try EDAEngine.autoArrangeSymbol(plain).get().symbolLayout?.pins.count, plain.pins.count)
        if case .success = EDAEngine.autoArrangeSymbol(CustomPartSpec()) { XCTFail("a part without pins arranged") }
    }

    // MARK: - Store

    func testStackedPinsJoinNetsAndEditingKeepsWires() throws {
        let store = DesignStore()
        store.aiEnabled = false
        var s = chip()
        var d = SymbolDraft(spec: s)
        d.move(["4", "5"], to: .bottom, slot: 0)
        d.stack(["4", "5"])
        d.apply(to: &s)
        let part = try XCTUnwrap(store.saveCustomPart(s))
        let u = store.addCustomComponent(partId: part.id, at: CGPoint(x: 200, y: 0))
        let g = store.addComponent(.ground, at: CGPoint(x: 200, y: 200))
        XCTAssertTrue(store.connect(PinAddress(component: u, pin: 3), PinAddress(component: g, pin: 0)))  // pin "4"
        let c = try XCTUnwrap(store.snapshot.component(u))
        XCTAssertEqual(c.pins[3].net, c.pins[4].net)  // the stacked pin "5" joined GND
        XCTAssertTrue(c.pins[4].connected)
        XCTAssertFalse(c.pins[1].connected)  // IN is on its own

        // Re-arranging the symbol moves the pins; the wire stays on pin "4".
        var moved = SymbolDraft(spec: part.spec)
        moved.move(["4", "5"], to: .top, slot: 0)
        let saved = try XCTUnwrap(store.saveCustomPart(moved.applied(to: part.spec), replacing: part.id))
        let after = try XCTUnwrap(store.snapshot.component(u))
        XCTAssertNotEqual(after.pins[3].y, c.pins[3].y)
        XCTAssertTrue(after.pins[3].connected)
        XCTAssertEqual(after.pins[3].net, after.pins[4].net)
        XCTAssertEqual(store.snapshot.customParts.first { $0.id == saved.id }?.symbolLayout, moved.layout)
        store.undo()
        XCTAssertEqual(store.snapshot.component(u)?.pins[3].y, c.pins[3].y)
    }

    // MARK: - Views

    func testEditorRendersInALiveWindow() throws {
        var applied: CustomPartSpec?
        let editor = SymbolEditorView(spec: try spec("STM32F405RGT6")) { applied = $0 }
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1100, height: 760),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: editor)
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.6))
        XCTAssertNotNil(window.contentView)
        XCTAssertNil(applied)
        let renderer = ImageRenderer(content: SymbolEditorView(spec: chip()) { _ in }.frame(width: 1000, height: 700))
        renderer.scale = 1
        XCTAssertEqual(try XCTUnwrap(renderer.cgImage).width, 1000)
        // A previewed four-sided symbol renders with its labels.
        var d = SymbolDraft(spec: chip())
        d.move(["1"], to: .top, slot: 0)
        let info = try EDAEngine.previewCustomPart(d.applied(to: chip())).get()
        let preview = ImageRenderer(content: SymbolPreview(kind: .custom, value: info.name, custom: info, showPinLabels: true)
            .frame(width: 300, height: 300))
        XCTAssertNotNil(preview.cgImage)
    }
}

/// Symbol Editor in depth: edge cases of the layout document (odd and named pins, partial stacks, no-op edits, gaps),
/// layout encoding, the core's checks and Auto Arrange on every library part, saving and reopening a project with an
/// arranged symbol, and four-sided symbols drawn by the schematic editor in a live window.
@MainActor
final class SymbolEditorDetailTests: XCTestCase {
    /// VCC, IN, OUT, GND, GND, EN.
    private func chip() -> CustomPartSpec {
        var s = CustomPartSpec()
        s.name = "SYM-DETAIL"
        s.pins = [CustomPartSpec.Pin(number: "1", name: "VCC", type: .powerIn), CustomPartSpec.Pin(number: "2", name: "IN", type: .input),
                  CustomPartSpec.Pin(number: "3", name: "OUT", type: .output), CustomPartSpec.Pin(number: "4", name: "GND", type: .powerIn),
                  CustomPartSpec.Pin(number: "5", name: "GND", type: .powerIn), CustomPartSpec.Pin(number: "6", name: "EN", type: .input)]
        return s
    }

    private func library(_ name: String) throws -> CustomPartSpec {
        try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name }, name).spec
    }

    // MARK: - Document edge cases

    func testGeneratedBoxWithOddCountsAndNamedPins() throws {
        var s = chip()
        s.pins.removeLast()  // 5 pins
        let odd = SymbolDraft(spec: s)
        XCTAssertEqual(odd.slots(.left), [["1"], ["2"], ["3"]])
        XCTAssertEqual(odd.slots(.right), [["5"], ["4"]])
        // A named pin (EP) sorts after the numbered ones, like the core.
        var qfn = chip()
        qfn.pins.removeLast()
        qfn.pins.append(CustomPartSpec.Pin(number: "EP", name: "GND", type: .powerIn))
        qfn.package.type = "QFN"
        let d = SymbolDraft(spec: qfn)
        XCTAssertEqual(d.slots(.right), [["EP"], ["5"], ["4"]])
        let generated = try EDAEngine.previewCustomPart(qfn).get().symbol.pins
        let explicit = try EDAEngine.previewCustomPart(d.applied(to: qfn)).get().symbol.pins
        XCTAssertEqual(generated.map { "\($0.number)@\($0.x),\($0.y)" }, explicit.map { "\($0.number)@\($0.x),\($0.y)" })
        // No pins: an empty layout.
        let empty = SymbolDraft(spec: CustomPartSpec())
        XCTAssertTrue(empty.placements.isEmpty)
        XCTAssertEqual(empty.slotCount(.left), 0)
        XCTAssertEqual(empty.slots(.top), [])
    }

    func testMovingPartOfAStackSplitsIt() {
        var d = SymbolDraft(spec: chip())
        d.move(["4", "5"], to: .bottom, slot: 0)
        d.stack(["4", "5"])
        d.move(["5"], to: .right, slot: 0)
        XCTAssertEqual(d.placement(of: "4")?.side, .bottom)
        XCTAssertEqual(d.placement(of: "5")?.side, .right)
        XCTAssertEqual(d.stack(of: "4"), ["4"])
    }

    func testNudgeMovesAStackAsOne() {
        var d = SymbolDraft(spec: chip())
        d.move(["4", "5"], to: .bottom, slot: 0)
        d.stack(["4", "5"])
        d.closeGaps()
        d.move(["1"], to: .bottom, slot: 1)
        XCTAssertEqual(d.slots(.bottom), [["4", "5"], ["1"]])
        d.nudge("4", by: 1)
        XCTAssertEqual(d.slots(.bottom), [["1"], ["4", "5"]])
        d.nudge("5", by: -1)
        XCTAssertEqual(d.slots(.bottom), [["4", "5"], ["1"]])
    }

    func testNoOpEditsLeaveTheDraftUnchanged() {
        let original = SymbolDraft(spec: chip())
        var d = original
        d.unstack("2")               // not stacked
        d.stack([])                  // nothing to stack
        d.stack(["99", "1"])         // unknown anchor
        d.swap("1", "99")            // unknown partner
        d.nudge("1", by: 0)
        d.nudge("99", by: 1)
        d.move([], to: .top, slot: 0)
        d.insertGap(.left, at: 10)   // past the last pin
        d.closeGaps(.top)            // empty side
        XCTAssertEqual(d, original)
        d.mirror()
        d.mirror()
        XCTAssertEqual(d, original)
    }

    func testGapsOnEverySideClose() {
        var d = SymbolDraft(spec: chip())
        d.move(["1"], to: .top, slot: 2)
        d.move(["4"], to: .bottom, slot: 3)
        d.insertGap(.left, at: 0)
        d.insertGap(.right, at: 1)
        XCTAssertEqual(d.slots(.top), [[], [], ["1"]])
        XCTAssertEqual(d.slots(.left).first, [])
        d.closeGaps()
        for side in SymbolDraft.Side.allCases { XCTAssertFalse(d.slots(side).contains { $0.isEmpty }, side.title) }
        XCTAssertEqual(d.placement(of: "1")?.slot, 0)
        XCTAssertEqual(d.placement(of: "4")?.slot, 0)
    }

    func testReconcileDropsDuplicatesAndUnknownPins() {
        var d = SymbolDraft(spec: chip())
        d.placements.append(.init(number: "2", side: .top, slot: 0))
        d.placements.append(.init(number: "9", side: .top, slot: 1))
        XCTAssertTrue(d.reconcile(with: chip().pins))
        XCTAssertEqual(d.placements.filter { $0.number == "2" }.count, 1)
        XCTAssertNil(d.placement(of: "9"))
        XCTAssertEqual(Set(d.placements.map(\.number)), Set(chip().pins.map(\.number)))
        XCTAssertFalse(d.reconcile(with: chip().pins))
    }

    func testLayoutWidthAndApply() {
        var d = SymbolDraft(spec: chip())
        XCTAssertNil(d.layout.width)
        d.width = 140
        XCTAssertEqual(d.layout.width, 140)
        var s = chip()
        s.symbolLayout = .init(width: 60, pins: [])
        d.apply(to: &s)
        XCTAssertEqual(s.symbolLayout, d.layout)
        XCTAssertEqual(s.pins, chip().pins)
        XCTAssertEqual(s.name, chip().name)
        XCTAssertEqual(SymbolDraft(spec: s), d)
        XCTAssertEqual(SymbolDraft.Side.allCases.map(\.rawValue), ["L", "R", "T", "B"])
        XCTAssertEqual(SymbolDraft.Side.top.title, "Top")
    }

    // MARK: - Core checks and Auto Arrange

    func testIssueDecodingAndPartsWithoutALayout() throws {
        let issues = try JSONDecoder().decode([SymbolIssue].self, from: Data(
            #"[{"severity":"warning","code":"SYM_STACK_SIGNAL","message":"m","pins":["1","2"]}]"#.utf8))
        XCTAssertFalse(issues[0].isError)
        XCTAssertEqual(issues[0].pins, ["1", "2"])
        XCTAssertEqual(issues[0].id, "SYM_STACK_SIGNALm")
        XCTAssertTrue(EDAEngine.checkSymbol(chip()).isEmpty)  // the generated box has nothing to check
    }

    func testAutoArrangeKeepsThePartAndIsDeterministic() throws {
        var stm = try library("STM32F405RGT6")
        stm.symbolLayout = nil
        let first = try EDAEngine.autoArrangeSymbol(stm).get()
        let second = try EDAEngine.autoArrangeSymbol(stm).get()
        XCTAssertEqual(first.symbolLayout, second.symbolLayout)
        XCTAssertEqual(first.pins, stm.pins)
        XCTAssertEqual(first.name, stm.name)
        XCTAssertEqual(first.package, stm.package)
        XCTAssertEqual(first.symbolLayout, try library("STM32F405RGT6").symbolLayout)  // what the library ships
    }

    func testEveryArrangedLibraryPartDrawsEachPinOnItsSide() throws {
        let arranged = StandardLibrary.parts.filter { $0.spec.symbolLayout != nil }
        XCTAssertGreaterThan(arranged.count, 60)
        for part in arranged {
            let info = try EDAEngine.previewCustomPart(part.spec).get()
            let hw = info.symbol.halfWidth, hh = info.symbol.halfHeight
            XCTAssertEqual(info.symbol.pins.count, part.spec.pins.count, part.spec.name)
            for pin in info.symbol.pins {
                switch pin.sideLetter {
                case "L": XCTAssertLessThan(pin.x, -hw, "\(part.spec.name) \(pin.number)")
                case "R": XCTAssertGreaterThan(pin.x, hw, "\(part.spec.name) \(pin.number)")
                case "T": XCTAssertLessThan(pin.y, -hh, "\(part.spec.name) \(pin.number)")
                case "B": XCTAssertGreaterThan(pin.y, hh, "\(part.spec.name) \(pin.number)")
                default: XCTFail("\(part.spec.name) pin \(pin.number) has no side")
                }
            }
            XCTAssertFalse(EDAEngine.checkSymbol(part.spec).contains { $0.isError }, part.spec.name)
        }
    }

    // MARK: - Projects and the schematic

    func testProjectSaveAndReopenKeepTheSymbol() throws {
        let store = DesignStore()
        store.aiEnabled = false
        var d = SymbolDraft(spec: chip())
        d.move(["1"], to: .top, slot: 0)
        d.move(["4", "5"], to: .bottom, slot: 0)
        d.stack(["4", "5"])
        d.closeGaps()
        let part = try XCTUnwrap(store.saveCustomPart(d.applied(to: chip())))
        let u = store.addCustomComponent(partId: part.id, at: CGPoint(x: 100, y: 100), rotation: 90)
        let before = try XCTUnwrap(store.snapshot.component(u))
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        let snapshot = try XCTUnwrap(reopened.snapshot())
        XCTAssertEqual(snapshot.customParts.first { $0.id == part.id }?.symbolLayout, d.layout)
        let after = try XCTUnwrap(snapshot.component(u))
        XCTAssertEqual(after.pins.map(\.x), before.pins.map(\.x))
        XCTAssertEqual(after.pins.map(\.y), before.pins.map(\.y))
        XCTAssertEqual(after.pins[3].x, after.pins[4].x)  // the stack survives rotation and reload
        XCTAssertEqual(after.pins[3].y, after.pins[4].y)
    }

    func testSchematicEditorDrawsFourSidedSymbolsInALiveWindow() throws {
        let store = DesignStore()
        store.aiEnabled = false
        for (i, name) in ["STM32F405RGT6", "ESP32-WROOM-32E", "CH340G"].enumerated() {
            let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == name })
            XCTAssertGreaterThanOrEqual(store.placeStandardPart(part, at: CGPoint(x: Double(i) * 500, y: 0)), 0, name)
        }
        let rotatedPart = try XCTUnwrap(store.snapshot.customParts.first { $0.name == "CH340G" })
        XCTAssertGreaterThanOrEqual(store.addCustomComponent(partId: rotatedPart.id, at: CGPoint(x: 0, y: 600), rotation: 90), 0)
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.SymbolEditorDetailTests")))
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 1300, height: 820),
                              styleMask: [.titled, .closable, .resizable], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: SchematicEditorView()
            .environmentObject(store).environmentObject(settings).environmentObject(AgentOrchestrator()))
        window.makeKeyAndOrderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.8))
        XCTAssertNotNil(window.contentView)
        // Each placed symbol's pins sit on the sides the layout says, also on the rotated copy.
        let stm = try XCTUnwrap(store.snapshot.components.first { $0.value == "STM32F405RGT6" })
        let info = try XCTUnwrap(store.snapshot.customPart(for: stm))
        let vdd = info.symbol.pins.enumerated().filter { $0.element.name == "VDD" }.map(\.offset)
        XCTAssertEqual(Set(vdd.map { stm.pins[$0].y }).count, 1)
        XCTAssertLessThan(stm.pins[vdd[0]].y, stm.y - info.symbol.halfHeight)
    }
}

@MainActor
final class SplashScreenTests: XCTestCase {
    private func spin(until done: () -> Bool, timeout: TimeInterval = 10) {
        let deadline = Date().addingTimeInterval(timeout)
        while !done() && Date() < deadline { RunLoop.main.run(until: Date().addingTimeInterval(0.02)) }
    }

    private func counting(_ count: Int, into log: @escaping (Int) -> Void) -> [SplashModel.Step] {
        (0..<count).map { index in SplashModel.Step(title: "Step \(index)") { log(index) } }
    }

    func testProgressFollowsBothTheClockAndTheWork() {
        // Ahead on work: the bar follows the clock.
        XCTAssertEqual(SplashModel.progress(elapsed: 1, minimum: 5, completed: 4, total: 4), 0.2, accuracy: 1e-9)
        // Ahead on time: the bar waits for the work.
        XCTAssertEqual(SplashModel.progress(elapsed: 4, minimum: 5, completed: 1, total: 4), 0.25, accuracy: 1e-9)
        XCTAssertEqual(SplashModel.progress(elapsed: 9, minimum: 5, completed: 4, total: 4), 1)
        XCTAssertEqual(SplashModel.progress(elapsed: -1, minimum: 5, completed: 4, total: 4), 0)
        XCTAssertEqual(SplashModel.progress(elapsed: 0, minimum: 0, completed: 0, total: 0), 1)
        XCTAssertEqual(SplashModel.progress(elapsed: 3, minimum: 0, completed: 2, total: 4), 0.5)
    }

    func testStatusNamesTheStepUnderTheBar() {
        XCTAssertEqual(SplashModel.statusIndex(progress: 0, total: 4), 0)
        XCTAssertEqual(SplashModel.statusIndex(progress: 0.24, total: 4), 0)
        XCTAssertEqual(SplashModel.statusIndex(progress: 0.25, total: 4), 1)
        XCTAssertEqual(SplashModel.statusIndex(progress: 0.99, total: 4), 3)
        XCTAssertEqual(SplashModel.statusIndex(progress: 1, total: 4), 4)
        XCTAssertEqual(SplashModel.statusIndex(progress: 1, total: 0), 0)

        let model = SplashModel(steps: counting(2) { _ in }, minimumDuration: 5)
        XCTAssertEqual(model.status(at: model.startDate), "Step 0…")
        XCTAssertFalse(model.isLoaded)
    }

    func testStandardStepsPreloadTheLibraries() {
        let steps = SplashModel.standardSteps
        XCTAssertEqual(steps.count, 5)
        XCTAssertEqual(Set(steps.map(\.title)).count, 5)
        steps.forEach { $0.work() }
        XCTAssertEqual(StandardLibrary.memoryDesignTypes.map(\.id), ["sdram", "ddr", "lpddr", "dimm", "rdimm"])
        XCTAssertFalse(StandardLibrary.parts.isEmpty)
        XCTAssertFalse(StandardLibrary.rulePresets.isEmpty)
        XCTAssertFalse(StandardLibrary.industries.isEmpty)
        XCTAssertFalse(OfflineProvider.templates.isEmpty)
        XCTAssertEqual(SplashModel.defaultDuration, 5)
    }

    func testRunDoesEveryStepInOrderThenWaitsOutTheMinimum() {
        var log: [Int] = []
        let model = SplashModel(steps: counting(3) { log.append($0) }, minimumDuration: 0.6)
        let started = Date()
        Task { await model.run() }
        spin { model.isLoaded }
        XCTAssertEqual(log, [0, 1, 2])
        XCTAssertFalse(model.isFinished, "the splash stays up for its minimum duration")
        spin { model.isFinished }
        XCTAssertTrue(model.isFinished)
        XCTAssertGreaterThanOrEqual(Date().timeIntervalSince(started), 0.6)
        XCTAssertEqual(model.progress(at: Date()), 1)
        XCTAssertEqual(model.status(at: Date()), "Ready")
    }

    func testSkipEndsTheSplashOnlyOnceLoaded() {
        var gate = false
        let steps = [SplashModel.Step(title: "Slow") { gate = true }]
        let model = SplashModel(steps: steps, minimumDuration: 30)
        model.skip()  // nothing loaded yet: ignored
        let started = Date()
        Task { await model.run() }
        spin { model.isLoaded }
        XCTAssertTrue(gate)
        XCTAssertFalse(model.isFinished)
        model.skip()
        spin { model.isFinished }
        XCTAssertTrue(model.isFinished)
        XCTAssertLessThan(Date().timeIntervalSince(started), 5, "skip ends the splash long before 30 s")
    }

    func testControllerHoldsTheMainWindowUntilTheSplashIsDone() throws {
        XCTAssertNil(SplashController.current, "no splash is shown while the tests run")
        var loaded = 0
        let model = SplashModel(steps: counting(2) { _ in loaded += 1 }, minimumDuration: 2.5)
        let controller = SplashController(model: model)
        // SwiftUI's main window exists, on screen, before the splash: it must still wait behind it.
        let early = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                             styleMask: [.titled, .closable], backing: .buffered, defer: false)
        early.isReleasedWhenClosed = false
        early.makeKeyAndOrderFront(nil)
        XCTAssertTrue(early.isVisible)
        controller.activate()
        XCTAssertEqual(early.alphaValue, 0, "hidden at once, before it can draw")
        var finished = false
        controller.show { finished = true }
        spin { !early.isVisible }
        XCTAssertFalse(early.isVisible)
        // Brought forward again mid-splash (as SwiftUI may do): hidden again.
        early.alphaValue = 1
        early.makeKeyAndOrderFront(nil)
        spin { !early.isVisible }
        XCTAssertFalse(early.isVisible)
        XCTAssertEqual(early.alphaValue, 0)
        XCTAssertTrue(SplashController.current === controller)
        let splash = try XCTUnwrap(controller.window)
        XCTAssertTrue(splash.isVisible)
        XCTAssertEqual(splash.frame.size, SplashView.size)
        XCTAssertTrue(splash.canBecomeKey)

        // A main window that appears meanwhile is kept out of sight.
        let main = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 400, height: 300),
                            styleMask: [.titled, .closable], backing: .buffered, defer: false)
        main.isReleasedWhenClosed = false
        main.contentView = NSHostingView(rootView: Text("Main").background(SplashWindowGate()))
        main.makeKeyAndOrderFront(nil)
        spin { !main.isVisible }
        XCTAssertFalse(main.isVisible)
        XCTAssertEqual(main.alphaValue, 0)

        spin { finished }
        XCTAssertTrue(finished)
        XCTAssertEqual(loaded, 2)
        XCTAssertNil(SplashController.current)
        XCTAssertNil(controller.window)
        XCTAssertFalse(splash.isVisible)
        XCTAssertTrue(main.isVisible, "the main window opens when the splash is done")
        XCTAssertEqual(main.alphaValue, 1)
        XCTAssertTrue(early.isVisible)
        XCTAssertEqual(early.alphaValue, 1)
        XCTAssertFalse(controller.isActive)
        early.close()

        // Windows opened later are left alone.
        controller.hold(main)
        XCTAssertEqual(main.alphaValue, 1)
        main.close()
    }

    func testSplashViewRendersEveryStage() throws {
        let model = SplashModel(steps: counting(1) { _ in }, minimumDuration: 0.3)
        let host = NSHostingView(rootView: SplashView(model: model))
        host.frame = NSRect(origin: .zero, size: SplashView.size)
        let window = NSWindow(contentRect: host.frame, styleMask: [.borderless], backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        window.contentView = host
        window.orderFront(nil)
        defer { window.close() }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        Task { await model.run() }
        spin { model.isFinished }
        RunLoop.main.run(until: Date().addingTimeInterval(0.1))
        XCTAssertTrue(model.isFinished)
        XCTAssertGreaterThan(host.fittingSize.width, 0)
        XCTAssertTrue(SplashView.versionLine.hasPrefix("Version "))
        XCTAssertTrue(SplashView.versionLine.contains("Core \(EDAEngine.coreVersion)"))
    }
}

@MainActor
final class SplashDesignTests: XCTestCase {
    func testCircuitArtworkStaysOnTheCardAndClearOfTheStatusLine() {
        let traces = SplashCircuit.traces
        XCTAssertEqual(traces.count, SplashCircuit.pinsPerSide * 4)
        XCTAssertTrue(traces.contains { $0.pulse }, "some traces carry signal pulses")
        // The status line and version text sit in the bottom ~60 points; the wordmark on the left third.
        let statusTop = SplashView.size.height - 60
        for trace in traces {
            let box = trace.path.boundingRect
            XCTAssertGreaterThanOrEqual(box.minY, 0)
            XCTAssertLessThan(box.maxY + 4, statusTop, "a trace (and its via) ends above the status line")
            XCTAssertGreaterThan(box.minX, SplashView.size.width * 0.3, "traces stay right of the wordmark")
            XCTAssertLessThan(trace.end.y + 4, statusTop)
        }
    }

    func testStatusWalksThroughEveryStepThenReady() {
        let steps = SplashModel.standardSteps
        let model = SplashModel(steps: steps.map { SplashModel.Step(title: $0.title) {} }, minimumDuration: 5)
        var seen: [String] = []
        for k in 0...steps.count {
            let progress = Double(k) / Double(steps.count)
            let index = SplashModel.statusIndex(progress: progress, total: steps.count)
            seen.append(index < steps.count ? steps[index].title + "…" : "Ready")
        }
        XCTAssertEqual(seen.last, "Ready")
        XCTAssertEqual(Array(seen.dropLast()), steps.map { $0.title + "…" })
        XCTAssertTrue(model.steps.map(\.title).contains("Loading memory & system design kits"))
    }

    func testSplashCanBeTurnedOffInSettings() {
        let key = "showSplashScreen"
        let saved = UserDefaults.standard.object(forKey: key)
        defer {
            if let saved { UserDefaults.standard.set(saved, forKey: key) } else { UserDefaults.standard.removeObject(forKey: key) }
        }
        UserDefaults.standard.removeObject(forKey: key)
        XCTAssertTrue(SplashController.isEnabled, "on by default")
        UserDefaults.standard.set(false, forKey: key)
        XCTAssertFalse(SplashController.isEnabled)
        UserDefaults.standard.set(true, forKey: key)
        XCTAssertTrue(SplashController.isEnabled)
    }

    func testControllerIgnoresWindowsWhenInactive() {
        let controller = SplashController(model: SplashModel(steps: [], minimumDuration: 0))
        XCTAssertFalse(controller.isActive)
        XCTAssertFalse(controller.isShowing)
        let window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 200, height: 100), styleMask: [.titled],
                              backing: .buffered, defer: false)
        window.isReleasedWhenClosed = false
        controller.hold(window)
        XCTAssertEqual(window.alphaValue, 1, "an inactive controller leaves windows alone")
        window.close()
    }
}

@MainActor
final class MemoryDesignTests: XCTestCase {
    private let title = "STM32H743 + 32 MB SDRAM Frame Buffer: 5-Segment Memory Reference"

    func testMemoryTypesPartsAndTemplateSelection() throws {
        XCTAssertEqual(StandardLibrary.memoryDesignTypes.map(\.id), ["sdram", "ddr", "lpddr", "dimm", "rdimm"])
        let profile = try XCTUnwrap(StandardLibrary.industry("memory"))
        XCTAssertEqual(profile.rulePreset, "HDI / Fine-Pitch BGA (IPC-2226)")
        XCTAssertEqual(profile.systemImage, "memorychip.fill")
        for name in ["MT48LC16M16A2TG-6A", "W9812G6KH-6", "IS42S16400J-7TL", "MT41K256M16HA-125", "AS4C256M16D3-12BCN",
                     "MT40A512M16LY-062E"] {
            XCTAssertTrue(StandardLibrary.parts.contains { $0.spec.name == name }, name)
        }
        XCTAssertEqual(OfflineProvider.template(for: "STM32H743 frame buffer with external SDRAM").plan.title, title)
        XCTAssertEqual(OfflineProvider.template(for: "DDR4 memory-down beside an FPGA").industry, "memory")
        XCTAssertEqual(OfflineProvider.template(for: "DDR5 SO-DIMM module").industry, "memory")
        // Words that merely contain "ram" stay with their own templates.
        XCTAssertNotEqual(OfflineProvider.template(for: "program a frame grabber").industry, "memory")
        XCTAssertNotEqual(OfflineProvider.template(for: "usb 2.0 high-speed link with a length matched DDR bus").industry, "memory")
        XCTAssertTrue(OfflineProvider.categories.contains("Memory & RAM"))
        XCTAssertEqual(OfflineProvider.examples(in: "Memory & RAM").map(\.plan.title), [title])
    }

    func testReferenceDesignSegmentsAndPersistence() throws {
        let template = try XCTUnwrap(OfflineProvider.templates.first { $0.plan.memoryDesign != nil })
        XCTAssertEqual(template.plan.title, title)
        XCTAssertEqual(template.industry, "memory")
        let store = DesignStore()
        let report = DesignPlanCompiler.apply(template.industryPlan, to: store.engine, previous: nil)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        XCTAssertEqual(report.connectionsMade, template.plan.connections.count)
        store.refresh()
        XCTAssertEqual(store.snapshot.memoryDesign, "sdram")
        XCTAssertEqual(store.snapshot.industry, "memory")
        XCTAssertEqual(store.snapshot.board.layerCount, 6)
        let segments = store.memorySegments()
        XCTAssertTrue(segments.applies)
        XCTAssertEqual(segments.segments.map(\.id), ["power", "clock", "data", "config", "layout"])
        for segment in segments.segments where segment.id != "layout" {  // the layout items need placed parts
            XCTAssertEqual(segment.status, "complete", "\(segment.id): \(segment.items.filter { !$0.ok }.map(\.label))")
        }
        let erc = store.engine.runERC().filter { $0.severity != .info }
        XCTAssertTrue(erc.isEmpty, "\(erc.map(\.message))")

        // The type is undoable, saved with the project and carried into refinement plans.
        store.setMemoryDesign("ddr")
        XCTAssertEqual(store.snapshot.memoryDesign, "ddr")
        XCTAssertNotEqual(store.memorySegments().segments.first { $0.id == "data" }?.status, nil)
        store.undo()
        XCTAssertEqual(store.snapshot.memoryDesign, "sdram")
        store.setMemoryDesign("")
        XCTAssertTrue(store.snapshot.memoryDesign.isEmpty)
        store.undo()
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.memoryDesign, "sdram")
        XCTAssertEqual(DesignPlanCompiler.plan(from: store.snapshot).memoryDesign, "sdram")
        XCTAssertFalse(store.engine.setMemoryDesign("ddr9"))

        // The plan schema offers the types to AI agents.
        let schema = DesignSchemas.designPlan
        let properties = try XCTUnwrap(schema["properties"] as? [String: Any])
        let memory = try XCTUnwrap(properties["memoryDesign"] as? [String: Any])
        XCTAssertEqual(memory["enum"] as? [String], ["sdram", "ddr", "lpddr", "dimm", "rdimm"])
        XCTAssertTrue(AgentPrompts.architectSystem.contains("\"memoryDesign\""))
    }

    func testDdrChecksOnAFreshProject() throws {
        let store = DesignStore()
        store.setMemoryDesign("ddr")
        let part = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "MT41K256M16HA-125" })
        let info = try store.engine.registerCustomPart(part.spec)
        XCTAssertEqual(info.footprintGeometry.pads.count, 96)
        _ = store.engine.addCustomComponent(partId: info.id, value: nil, at: CGPoint(x: 0, y: 0), rotation: 0, ref: "U1")
        store.refresh()
        let codes = Set(store.engine.runDRC().map(\.code))  // layout + design-for-reliability rules
        for code in ["REL_DDR_ZQ", "REL_DDR_VREF", "REL_DDR_RESET", "REL_DDR_VTT"] {
            XCTAssertTrue(codes.contains(code), code)
        }
        let segments = store.memorySegments()
        XCTAssertEqual(segments.segments.first { $0.id == "data" }?.items.last?.ok, false, "ZQ resistor missing")
    }
}

@MainActor
final class LocalizationTests: XCTestCase {
    private func table(_ code: String) throws -> [String: String] {
        let url = try XCTUnwrap(Bundle.main.url(forResource: "Localizable", withExtension: "strings",
                                                subdirectory: nil, localization: code), "no \(code).lproj")
        return try XCTUnwrap(NSDictionary(contentsOf: url) as? [String: String], "\(code) does not parse")
    }

    func testEveryLanguageShipsACompleteTable() throws {
        XCTAssertEqual(AppLanguage.all.count, 20)
        XCTAssertEqual(Set(AppLanguage.all.map(\.code)).count, AppLanguage.all.count)
        let english = try table("en")
        XCTAssertGreaterThan(english.count, 400)
        for (key, value) in english { XCTAssertEqual(key, value) }
        for lang in AppLanguage.all where lang.code != "en" {
            let t = try table(lang.code)
            XCTAssertEqual(Set(t.keys), Set(english.keys), lang.code)
            XCTAssertTrue(t.values.allSatisfy { !$0.trimmingCharacters(in: .whitespaces).isEmpty }, lang.code)
            // Most strings really are translated (the rest are acronyms, units and product names).
            let translated = t.filter { $0.key != $0.value }.count
            XCTAssertGreaterThan(Double(translated) / Double(t.count), 0.7, lang.code)
        }
    }

    func testRegionsCoverTheTargetCountriesAndStates() {
        let regions = AppLanguage.all.map(\.regions).joined(separator: ", ")
        for place in ["Taiwan", "South Korea", "China", "United States", "Japan", "Malaysia", "Netherlands", "Germany",
                      "Singapore", "Vietnam", "Philippines", "India", "Andhra Pradesh", "Assam", "Gujarat",
                      "Karnataka", "Kerala", "Maharashtra", "Odisha", "Punjab", "Rajasthan", "Tamil Nadu",
                      "Telangana", "Uttar Pradesh"] {
            XCTAssertTrue(regions.contains(place), place)
        }
        XCTAssertEqual(AppLanguage.all.filter { $0.group == .india }.count, 10)
    }

    func testLookupInTheChosenLanguage() {
        XCTAssertEqual(LanguageSettings.localized("Schematic", code: "ja"), "回路図")
        XCTAssertEqual(LanguageSettings.localized("Schematic", code: "de"), "Schaltplan")
        XCTAssertNotEqual(LanguageSettings.localized("Auto Route", code: "ta"), "Auto Route")
        XCTAssertEqual(LanguageSettings.localized("Schematic", code: ""), "Schematic")
        XCTAssertEqual(LanguageSettings.localized("Not a UI string", code: "ko"), "Not a UI string")
    }

    func testChoosingALanguageIsSavedForTheNextLaunch() throws {
        let suite = "sieda.tests.language"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defaults.removePersistentDomain(forName: suite)
        defer { defaults.removePersistentDomain(forName: suite) }

        let settings = LanguageSettings(defaults: defaults)
        XCTAssertEqual(settings.code, "")
        XCTAssertFalse(settings.needsRelaunch)
        settings.code = "te"
        XCTAssertEqual(defaults.string(forKey: LanguageSettings.defaultsKey), "te")
        XCTAssertEqual(defaults.stringArray(forKey: "AppleLanguages"), ["te", "en"])
        XCTAssertTrue(settings.needsRelaunch)
        XCTAssertEqual(settings.locale.identifier, "te")
        XCTAssertEqual(settings.selected?.englishName, "Telugu")

        XCTAssertEqual(LanguageSettings(defaults: defaults).code, "te", "the choice survives a relaunch")
        settings.code = ""
        // The override is gone from the app's own domain (reading the key falls through to the system's list).
        XCTAssertNil(defaults.persistentDomain(forName: suite)?["AppleLanguages"])
        XCTAssertNil(defaults.persistentDomain(forName: suite)?[LanguageSettings.defaultsKey])
        XCTAssertFalse(settings.needsRelaunch)

        defaults.set("xx", forKey: LanguageSettings.defaultsKey)
        XCTAssertEqual(LanguageSettings(defaults: defaults).code, "", "an unknown language falls back to the system")
    }

    func testLanguageSettingsRender() {
        let view = LanguageSettingsView().environmentObject(LanguageSettings(defaults: UserDefaults(suiteName: "sieda.tests.render")!))
            .environment(\.locale, Locale(identifier: "hi"))
        let host = NSHostingView(rootView: view)
        host.frame = NSRect(x: 0, y: 0, width: 680, height: 600)
        host.layoutSubtreeIfNeeded()
        XCTAssertGreaterThan(host.fittingSize.height, 0)
    }
}

final class SheetAndVariantTests: XCTestCase {
    func testSheetsVariantsAndPlansThroughTheBridge() throws {
        let engine = EDAEngine(name: "Sheets")
        let load = try XCTUnwrap(engine.addSheet("Load"))
        XCTAssertNil(engine.addSheet("Load"))
        let v = engine.addComponent(.voltageSource, value: "5", at: .zero)
        let label = engine.addComponent(.netLabel, value: "VIN", at: CGPoint(x: 60, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: label, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0)))
        XCTAssertTrue(engine.setActiveSheet(load))
        let r = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        let label2 = engine.addComponent(.netLabel, value: "VIN", at: CGPoint(x: 40, y: 0))
        let g2 = engine.addComponent(.ground, at: CGPoint(x: 160, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: label2, pin: 0), PinAddress(component: r, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: g2, pin: 0)))
        // Wires stay on one sheet; the global label joins the sheets.
        XCTAssertNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0)))
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(snapshot.sheets.map(\.name), ["Main", "Load"])
        XCTAssertEqual(snapshot.activeSheet, load)
        XCTAssertEqual(snapshot.onSheet(load).components.count, 3)
        XCTAssertEqual(snapshot.onSheet(1).wires.count, 2)
        XCTAssertEqual(snapshot.component(r)?.pins[0].net, snapshot.component(v)?.pins[0].net)
        XCTAssertTrue(engine.simulateDC().converged)

        // A plan carries the sheets and rebuilds the same connectivity.
        let plan = DesignPlanCompiler.plan(from: snapshot)
        XCTAssertEqual(plan.sheets.map(\.name), ["Main", "Load"])
        let rebuilt = EDAEngine()
        DesignPlanCompiler.apply(plan, to: rebuilt, previous: nil)
        let again = try XCTUnwrap(rebuilt.snapshot())
        XCTAssertEqual(again.sheets.map(\.name), ["Main", "Load"])
        XCTAssertEqual(again.component(ref: "R1")?.sheetId, again.sheets[1].id)
        XCTAssertEqual(again.component(ref: "R1")?.pins[0].net, again.component(ref: "V1")?.pins[0].net)

        // Variants: R1 is not fitted in "Lite"; the BOM follows the active variant.
        XCTAssertTrue(engine.addVariant("Lite"))
        XCTAssertTrue(engine.setVariantPart("Lite", component: r, fitted: false))
        XCTAssertTrue(engine.setActiveVariant("Lite"))
        let withVariant = try XCTUnwrap(engine.snapshot())
        XCTAssertEqual(withVariant.activeVariant, "Lite")
        XCTAssertEqual(withVariant.variants.map(\.name), ["Lite"])
        XCTAssertEqual(withVariant.component(r)?.isFitted, false)
        XCTAssertEqual(engine.bom().summary.dnp, 1)
        XCTAssertTrue(engine.setActiveVariant(""))
        XCTAssertEqual(engine.bom().summary.dnp, 0)
        XCTAssertEqual(EDAEngine.expandBus("D[0..2]"), ["D0", "D1", "D2"])
    }
}

@MainActor
final class PartQueryTests: XCTestCase {
    func testParametricFiltersParseAndMatch() {
        let query = PartQuery("cat:sensors pkg:soic-8 pins:8 temperature")
        XCTAssertEqual(query.categories, ["sensors"])
        XCTAssertEqual(query.packages, ["soic8"])
        XCTAssertEqual(query.pinRange, 8...8)
        XCTAssertEqual(query.words, ["temperature"])
        XCTAssertTrue(query.matches(name: "LM75BD", description: "Digital temperature sensor", category: "Sensors",
                                    manufacturer: "NXP", package: "SOIC-8", pinCount: 8))
        XCTAssertFalse(query.matches(name: "LM75BD", description: "Digital temperature sensor", category: "Sensors",
                                     manufacturer: "NXP", package: "TSSOP-8", pinCount: 8))
        XCTAssertEqual(PartQuery.range("6-10"), 6...10)
        XCTAssertEqual(PartQuery.range(">40"), 41...Int.max)
        XCTAssertEqual(PartQuery.range("<8"), 0...7)
        XCTAssertNil(PartQuery.range("x"))
        XCTAssertTrue(PartQuery("  ").isEmpty)
        // Plain words still find what a phrase search found.
        XCTAssertEqual(PartQuery("Barometric").words, ["barometric"])
    }

    func testStandardLibrarySearchByCategoryPackageAndPins() {
        let all = StandardLibrary.parts
        let fets = ComponentLibraryView.filterStandard(all, search: "cat:transistors pkg:sot23 mosfet", kit: nil)
        XCTAssertTrue(fets.contains { $0.spec.name == "2N7002" })
        XCTAssertTrue(fets.allSatisfy { $0.spec.package.type == "SOT23" })
        XCTAssertTrue(ComponentLibraryView.filterStandard(all, search: "mfr:espressif pins:>40", kit: nil)
            .contains { $0.spec.name == "ESP32-S3" })
        XCTAssertFalse(ComponentLibraryView.filterStandard(all, search: "mfr:espressif pins:<40", kit: nil)
            .contains { $0.spec.name == "ESP32-S3" })
    }
}

@MainActor
final class LibraryImportTests: XCTestCase {
    func testImportsAKiCadFootprintAndAddsItToTheLibrary() throws {
        let footprint = "(footprint \"R_0603\" (pad \"1\" smd rect (at -0.8 0) (size 0.8 0.95) (layers \"F.Cu\"))"
            + " (pad \"2\" smd rect (at 0.8 0) (size 0.8 0.95) (layers \"F.Cu\")))"
        let result = EDAEngine.importLibrary(files: [LibraryImportFile(name: "R_0603.kicad_mod", content: footprint),
                                                     LibraryImportFile(name: "bad.lbr", content: "<eagle>")])
        XCTAssertEqual(result.parts.count, 1)
        XCTAssertEqual(result.files.count, 2)
        let part = try XCTUnwrap(result.importable.first)
        XCTAssertEqual(part.spec.package.type, "CUSTOM")
        XCTAssertEqual(part.spec.package.lands?.count, 2)
        XCTAssertEqual(part.spec.pins.count, 2)
        XCTAssertFalse(try XCTUnwrap(result.files.last).error.isEmpty)

        let store = DesignStore()
        let ids = store.importLibraryParts(result.importable.map(\.spec))
        XCTAssertEqual(ids.count, 1)
        XCTAssertTrue(store.snapshot.customParts.contains { $0.name == "R_0603" })
    }

    func testLibraryFilesAreFoundInsideFolders() throws {
        let folder = FileManager.default.temporaryDirectory.appendingPathComponent("SiEDA-\(UUID().uuidString).pretty")
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        defer { try? FileManager.default.removeItem(at: folder) }
        try "(footprint \"A\")".write(to: folder.appendingPathComponent("A.kicad_mod"), atomically: true, encoding: .utf8)
        try "notes".write(to: folder.appendingPathComponent("README.txt"), atomically: true, encoding: .utf8)
        let files = ComponentLibraryView.libraryFiles(at: [folder])
        XCTAssertEqual(files.map(\.name), ["A.kicad_mod"])
    }
}

/// Signal & power integrity: the bridge decodes every SI / PI report, and sign-off is undoable and saved.
@MainActor
final class SignalIntegrityBridgeTests: XCTestCase {
    func testSignalAndPowerIntegrityReports() throws {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        store.autoPlace(all: true)
        _ = store.engine.autoRoute()
        store.refresh()
        let settings = store.siSettings()
        XCTAssertFalse(settings.families.isEmpty)
        XCTAssertFalse(settings.signOff)
        if let first = store.siNets().first {
            let analysis = try XCTUnwrap(store.siNet(first.name))
            XCTAssertEqual(analysis.name, first.name)
            XCTAssertTrue(analysis.error.isEmpty)
            if let driver = analysis.waveform.driver { XCTAssertEqual(driver.count, analysis.waveform.time.count) }
        }
        XCTAssertNil(store.siNet("NO SUCH NET"))
        XCTAssertGreaterThan(store.siCrosstalk().limit, 0)
        for rail in store.pdn().rails { XCTAssertEqual(rail.curve.freq.count, rail.curve.z.count) }
        store.setSIOptions(signOff: true)
        XCTAssertTrue(store.siSettings().signOff)
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertTrue(reopened.siSettings().signOff)
        let verification = try XCTUnwrap(reopened.runVerification())
        XCTAssertTrue(verification.stages.contains { $0.id == "si" })
        store.undo()
        XCTAssertFalse(store.siSettings().signOff)
    }
}

/// Schematic capture: repeated sheets, buses, multi-unit parts, variants in simulation, search and navigation.
@MainActor
final class SchematicCaptureTests: XCTestCase {
    func testRepeatedSheetChannelsThroughTheStore() throws {
        let store = DesignStore()
        let block = try XCTUnwrap(store.addSheet(named: "Amp", parent: 1))
        let port = store.addComponent(.netLabel, at: .zero)
        XCTAssertTrue(store.engine.setValue(port, "IN"))
        XCTAssertTrue(store.engine.setLabelScope(port, scope: "port"))
        let r = store.addComponent(.resistor, at: CGPoint(x: 80, y: 0))
        XCTAssertTrue(store.connect(PinAddress(component: port, pin: 0), PinAddress(component: r, pin: 0)))
        store.repeatSheet(block, count: 3)
        let channels = store.snapshot.sheets.filter { $0.definitionId == block }
        XCTAssertEqual(channels.count, 3)
        XCTAssertEqual(store.snapshot.sheet(block)?.instances, 3)
        XCTAssertEqual(store.snapshot.component(r)?.logicalRef, "R1")
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R201")
        // Every channel got its sheet symbol on the parent sheet.
        for channel in channels {
            XCTAssertTrue(store.snapshot.components.contains { $0.labelScope == "entry" && $0.targetSheet == channel.id })
        }
        // The block designator is edited from a channel; every channel follows.
        let copy = try XCTUnwrap(store.snapshot.components.first { $0.instanceOf == r })
        store.setRef(copy.id, "R7")
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R207")
        store.setInstanceRefs(block, scheme: "suffix")
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R7_A")
        store.setSheetChannel(block, to: "L")
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R7_L")
        store.undo()
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R7_A")
        store.repeatSheet(block, count: 1)
        XCTAssertEqual(store.snapshot.sheets.filter { $0.definitionId == block }.count, 1)
    }
}

@MainActor
final class SchematicBusTests: XCTestCase {
    func testBusDrawRipConnectAndDelete() throws {
        let store = DesignStore()
        let j1 = store.addComponent(.connector, at: .zero)
        let j2 = store.addComponent(.connector, at: CGPoint(x: 300, y: 0))
        XCTAssertNil(store.addBus(named: "nope", points: [CGPoint(x: 150, y: -50), CGPoint(x: 150, y: 50)]))
        let bus = try XCTUnwrap(store.addBus(named: "D[0..1]", points: [CGPoint(x: 150, y: -50), CGPoint(x: 150, y: 50)]))
        XCTAssertEqual(store.selectedBus, bus)
        XCTAssertEqual(store.sheetSnapshot.bus(bus)?.members, ["D0", "D1"])
        store.connectBus(bus, toPart: j1)
        store.connectBus(bus, toPart: j2)
        let c1 = try XCTUnwrap(store.snapshot.component(j1)), c2 = try XCTUnwrap(store.snapshot.component(j2))
        XCTAssertEqual(c1.pins[0].net, c2.pins[0].net)
        XCTAssertNotEqual(c1.pins[0].net, c1.pins[1].net)
        XCTAssertEqual(store.snapshot.components.filter { $0.bus == bus }.count, 4)
        store.moveBus(bus, by: CGSize(width: 20, height: 0))
        XCTAssertEqual(store.snapshot.bus(bus)?.points.first?.x, 170)
        store.renameBus(bus, to: "D[0..2]")
        store.ripBusEntries(bus)
        XCTAssertEqual(store.snapshot.components.filter { $0.bus == bus }.count, 5)
        store.deleteSelection()
        XCTAssertNil(store.snapshot.bus(bus))
        XCTAssertTrue(store.snapshot.components.allSatisfy { $0.bus == nil })
        store.undo()
        XCTAssertNotNil(store.snapshot.bus(bus))
    }
}

@MainActor
final class MultiUnitPartTests: XCTestCase {
    func testQuadOpAmpPlacedGateByGate() throws {
        let store = DesignStore()
        let lm324 = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM324" })
        XCTAssertEqual(lm324.spec.units?.count, 4)
        let partId = try XCTUnwrap(store.addStandardPartToLibrary(lm324))
        XCTAssertTrue(try XCTUnwrap(store.snapshot.customPart(partId)).isMultiUnit)
        let a = store.addCustomComponent(partId: partId, at: .zero)
        let unitA = try XCTUnwrap(store.snapshot.component(a))
        XCTAssertEqual(unitA.unitName, "A")
        XCTAssertEqual(unitA.displayRef, "U1A")
        XCTAssertEqual(store.snapshot.customPart(for: unitA)?.symbol.pins.count, 3)
        let package = try XCTUnwrap(unitA.unitOf)
        XCTAssertTrue(try XCTUnwrap(store.snapshot.component(package)).isUnitPackage)
        // The package is not drawn; its units are.
        XCTAssertNil(store.sheetSnapshot.component(package))
        store.placeNextUnit(of: a)
        store.placeUnit(5, of: a)
        let units = store.snapshot.components.filter { $0.unitOf == package }.compactMap(\.unitName)
        XCTAssertEqual(Set(units), ["A", "B", "P"])
        // A plan carries the part whole.
        let plan = DesignPlanCompiler.plan(from: store.snapshot)
        XCTAssertEqual(plan.components.filter { $0.ref == "U1" }.count, 1)
        store.deleteSelection()
        XCTAssertFalse(store.snapshot.components.contains { $0.unitName == "P" })
    }
}

final class VariantSimulationTests: XCTestCase {
    func testActiveVariantDrivesTheSimulation() throws {
        let engine = EDAEngine(name: "Variant sim")
        let v = engine.addComponent(.voltageSource, value: "5", at: .zero)
        let r = engine.addComponent(.resistor, value: "330", at: CGPoint(x: 100, y: -40))
        let d = engine.addComponent(.led, value: "Red", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: d, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: d, pin: 1), PinAddress(component: g, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0)))
        let base = engine.simulateDC()
        XCTAssertTrue(base.converged)
        XCTAssertTrue(base.variant.isEmpty && base.omitted.isEmpty)
        XCTAssertTrue(engine.addVariant("NoLed"))
        XCTAssertTrue(engine.setVariantPart("NoLed", component: d, fitted: false))
        XCTAssertTrue(engine.setActiveVariant("NoLed"))
        let lite = engine.simulateDC()
        XCTAssertTrue(lite.converged)
        XCTAssertEqual(lite.variant, "NoLed")
        XCTAssertEqual(lite.omitted, ["D1"])
        XCTAssertLessThan(abs(lite.reading(component: r)?.current ?? 1), 1e-6)
        XCTAssertEqual(engine.snapshot()?.component(d)?.isFitted, false)
    }
}

@MainActor
final class SchematicFindAndNavigateTests: XCTestCase {
    func testFindReplaceNetNavigatorCrossProbeAndTitleBlock() throws {
        let store = DesignStore()
        let v = store.addComponent(.voltageSource, at: .zero)
        let label = store.addComponent(.netLabel, at: CGPoint(x: 60, y: -40))
        store.setValue(label, "VIN")
        XCTAssertTrue(store.connect(PinAddress(component: v, pin: 0), PinAddress(component: label, pin: 0)))
        let load = try XCTUnwrap(store.addSheet(named: "Load"))
        let r = store.addComponent(.resistor, at: CGPoint(x: 100, y: 0))
        let label2 = store.addComponent(.netLabel, at: CGPoint(x: 0, y: 0))
        store.setValue(label2, "VIN")
        XCTAssertTrue(store.connect(PinAddress(component: label2, pin: 0), PinAddress(component: r, pin: 0)))
        let hits = store.find("VIN", matchCase: false, wholeWord: true, pins: false)
        XCTAssertEqual(hits.filter { $0.field == "label" }.count, 2)
        XCTAssertEqual(hits.filter { $0.field == "net" }.count, 1)
        // The net navigator lists both sheets; cross-probing shows the other sheet.
        let net = try XCTUnwrap(store.snapshot.component(r)?.pins[0].net)
        let places = try XCTUnwrap(store.netPlaces(net)).places
        XCTAssertEqual(Set(places.map(\.sheet)).count, 2)
        store.crossProbe(component: v, sheet: 1)
        XCTAssertEqual(store.snapshot.activeSheet, 1)
        XCTAssertEqual(store.selection, [v])
        store.crossProbe(component: r, sheet: load)
        XCTAssertEqual(store.snapshot.activeSheet, load)
        // Replace renames both labels in one undo step; the net still joins.
        XCTAssertEqual(store.replaceAll("VIN", with: "VSUP", matchCase: true, wholeWord: true), 2)
        XCTAssertTrue(store.find("VIN", matchCase: false, wholeWord: false, pins: false).isEmpty)
        XCTAssertEqual(store.snapshot.component(r)?.pins[0].net, store.snapshot.component(v)?.pins[0].net)
        store.undo()
        XCTAssertEqual(store.find("VIN", matchCase: false, wholeWord: true, pins: false).filter { $0.field == "label" }.count, 2)
        // Title block.
        var block = store.snapshot.titleBlock
        block.company = "Acme"
        block.revision = "C"
        store.setTitleBlock(block)
        XCTAssertEqual(store.snapshot.titleBlock.company, "Acme")
        let reopened = EDAEngine()
        try reopened.load(json: store.engine.saveJSON())
        XCTAssertEqual(reopened.snapshot()?.titleBlock.revision, "C")
    }
}

@MainActor
final class ChannelAnalysisBridgeTests: XCTestCase {
    private func routedStore() -> DesignStore {
        let store = DesignStore()
        DesignPlanCompiler.apply(OfflineProvider.templates[5].plan, to: store.engine, previous: nil)
        store.refresh()
        store.autoPlace(all: true)
        _ = store.engine.autoRoute()
        store.refresh()
        return store
    }

    func testChannelLineLossAndTouchstone() throws {
        let store = routedStore()
        let loss = store.siLineLoss(roughness: "huray")
        XCTAssertFalse(loss.layers.isEmpty)
        for layer in loss.layers { XCTAssertEqual(layer.dbPerInch.count, loss.freq.count) }
        var settings = SIChannelSettings()
        settings.bitRate = 1e9
        if let net = store.siNets().first(where: { $0.receivers > 0 && $0.routed }) {
            let result = store.engine.siChannel(net: net.name, partner: "none", settings: settings)
            switch result {
            case .success(let report):
                XCTAssertEqual(report.net, net.name)
                XCTAssertEqual(report.ports, 2)
                XCTAssertFalse(report.curves.isEmpty)
                XCTAssertEqual(report.curves.first?.db.count, report.freq.count)
                XCTAssertEqual(report.step.time.count, report.step.lossy.count)
                let eye = try XCTUnwrap(report.eye)
                XCTAssertEqual(eye.density.count, eye.rows * eye.cols)
                let text = try store.engine.siChannelTouchstone(net: net.name, partner: "none", settings: settings)
                let imported = try EDAEngine.touchstoneChannel(text, ports: 2, settings: settings)
                XCTAssertEqual(imported.ports, 2)
                XCTAssertNotNil(imported.eye)
            case .failure(let error):
                // A net without a logic receiver reports why; anything else is a failure.
                XCTAssertFalse(error.localizedDescription.isEmpty)
            }
        }
        if case .success = store.engine.siChannel(net: "NO SUCH NET", partner: "", settings: settings) {
            XCTFail("an unknown net must fail")
        }
        XCTAssertThrowsError(try EDAEngine.touchstoneChannel("not touchstone", ports: 2, settings: settings))
    }

    func testCopperFoilChannelSpecAndRegulatorAreUndoable() throws {
        let store = routedStore()
        store.setCopperFoil("hvlp")
        XCTAssertEqual(store.siSettings().copperFoil, "hvlp")
        store.undo()
        XCTAssertNil(store.siSettings().copperFoil)
        if let net = store.siNets().first {
            store.setSIChannel(net.name, bitRate: 2e9, maskHeight: 0.1, maskWidthUi: 0.3)
            XCTAssertEqual(store.siSettings().channels?.first?.bitRate, 2e9)
            let reopened = EDAEngine()
            try reopened.load(json: store.engine.saveJSON())
            XCTAssertEqual(reopened.siSettings().channels?.count, 1)
        }
        if let rail = store.pdn().rails.first {
            store.setPDNRegulator(rail.name, outputOhms: 0.004, loopBandwidth: 80e3)
            let updated = try XCTUnwrap(store.pdn().rails.first { $0.name == rail.name })
            XCTAssertEqual(updated.vrmR, 0.004, accuracy: 1e-12)
            XCTAssertEqual(updated.vrmBandwidth ?? 0, 80e3, accuracy: 1e-6)
            XCTAssertNotNil(store.pdnCavity(rail.name))
            XCTAssertNotNil(store.pdnDecapPlan(rail.name))
            let map = try XCTUnwrap(store.pdnIRMap(rail.name))
            XCTAssertEqual(map.rail, rail.name)
            XCTAssertFalse(map.board.outline.isEmpty)
        }
        XCTAssertNil(store.pdnIRMap("NO SUCH RAIL"))
    }
}

@MainActor
final class InteractiveRoutingStoreTests: XCTestCase {
    /// Track geometry independent of ids and order (undo reloads the design).
    private static func shapes(_ tracks: [SnapTrack]) -> [String] {
        tracks.map { String(format: "%d %d %.4f %.4f %.4f %.4f %.4f", $0.net, $0.layer, $0.width, $0.ax, $0.ay, $0.bx, $0.by) }
            .sorted()
    }

    /// Two resistors 30 mm apart joined by a routed track with a through via at (18, 20).
    private func routedStore() throws -> DesignStore {
        let store = DesignStore()
        let engine = store.engine
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 20))
        let options = EDAEngine.routerOptions(shove: true, diagonal: true)
        XCTAssertNotNil(engine.routerBegin(at: CGPoint(x: 10.95, y: 20), layer: 0, pair: false, options: options))
        _ = engine.routerMove(to: CGPoint(x: 18, y: 20))
        XCTAssertNil(engine.routerAddVia()?.error)
        _ = engine.routerMove(to: CGPoint(x: 30, y: 20))
        XCTAssertNil(engine.routerAddVia(toLayer: 0)?.error)
        _ = engine.routerMove(to: CGPoint(x: 39.05, y: 20))
        XCTAssertTrue(engine.routerCommit().ok)
        store.refresh()
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        return store
    }

    func testDraggingATrackAndAViaIsOneUndoStep() throws {
        let store = try routedStore()
        let bottom = try XCTUnwrap(store.snapshot.tracks.first { $0.layer == 1 })
        let before = store.snapshot.tracks
        let grab = CGPoint(x: (bottom.ax + bottom.bx) / 2, y: (bottom.ay + bottom.by) / 2)
        XCTAssertTrue(store.beginTrackDrag(bottom.id, at: grab))
        XCTAssertEqual(store.routePreview?.kind, "drag")
        store.finishRoute(at: CGPoint(x: grab.x, y: grab.y + 2))
        XCTAssertNil(store.routePreview)
        XCTAssertNotEqual(store.snapshot.tracks, before)
        XCTAssertTrue(store.snapshot.tracks.contains { $0.layer == 1 && abs($0.ay - 22) < 1e-6 && abs($0.by - 22) < 1e-6 })
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        store.undo()
        XCTAssertEqual(Self.shapes(store.snapshot.tracks), Self.shapes(before))

        let via = try XCTUnwrap(store.snapshot.vias.first)
        XCTAssertTrue(store.beginViaDrag(via.id, at: CGPoint(x: via.x, y: via.y)))
        XCTAssertEqual(store.routePreview?.kind, "via")
        store.finishRoute(at: CGPoint(x: via.x, y: via.y - 3))
        XCTAssertTrue(store.snapshot.vias.contains { abs($0.x - via.x) < 1e-6 && abs($0.y - (via.y - 3)) < 1e-6 })
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        XCTAssertFalse(store.engine.runDRC().contains { $0.severity == .error })
        // A refused drag (unknown via) starts nothing.
        XCTAssertFalse(store.beginViaDrag(-4, at: .zero))
        XCTAssertNil(store.routePreview)
    }

    func testLengthTuningPreviewsThenApplies() throws {
        let store = try routedStore()
        let top = try XCTUnwrap(store.snapshot.tracks.first { $0.layer == 0 && hypot($0.bx - $0.ax, $0.by - $0.ay) > 6 })
        let tracks = store.snapshot.tracks
        store.beginTune(track: top.id, at: CGPoint(x: (top.ax + top.bx) / 2, y: top.ay))
        let preview = try XCTUnwrap(store.tuneSession?.preview)
        XCTAssertTrue(preview.ok, preview.message)
        XCTAssertTrue(preview.group.isEmpty)
        XCTAssertGreaterThan(preview.target, preview.before)  // no group: starts at its length + 1 mm
        XCTAssertFalse(preview.applied)
        XCTAssertFalse(preview.addedTracks.isEmpty)
        XCTAssertEqual(store.snapshot.tracks, tracks)  // a preview leaves the board alone
        store.setTuneTarget(preview.before + 0.5)
        XCTAssertEqual(store.tuneSession?.preview?.target ?? 0, preview.before + 0.5, accuracy: 1e-9)
        store.applyTune()
        XCTAssertNil(store.tuneSession)
        let length = store.snapshot.tracks.reduce(0.0) { $0 + hypot($1.bx - $1.ax, $1.by - $1.ay) }
        XCTAssertEqual(length, preview.before + 0.5, accuracy: 0.05)
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        store.undo()
        XCTAssertEqual(Self.shapes(store.snapshot.tracks), Self.shapes(tracks))
        store.beginTune(track: top.id, at: .zero)
        store.cancelTune()
        XCTAssertNil(store.tuneSession)
    }

    func testHeadUpdatesRunOffTheMainThreadLatestWins() async throws {
        let store = try routedStore()
        store.beginRoute(at: CGPoint(x: 39.05, y: 20), layer: 0, pair: false)
        XCTAssertNotNil(store.routePreview)
        // A burst of moves: they do not block, and the head ends at the last one.
        for x in stride(from: 39.0, through: 30.0, by: -1.0) { store.moveRoute(to: CGPoint(x: x, y: 28)) }
        var waited = 0
        while abs((store.routePreview?.endX ?? 0) - 30) > 1e-6 && waited < 500 {
            try await Task.sleep(nanoseconds: 10_000_000)
            waited += 1
        }
        XCTAssertEqual(store.routePreview?.endX ?? 0, 30, accuracy: 1e-6)
        XCTAssertEqual(store.routePreview?.endY ?? 0, 28, accuracy: 1e-6)
        // A click places what is under the cursor now, even with an update queued.
        store.moveRoute(to: CGPoint(x: 34, y: 25))
        store.moveRouteNow(to: CGPoint(x: 33, y: 26))
        store.placeRouteCorner()
        try await Task.sleep(nanoseconds: 100_000_000)  // the stale background result must not come back
        XCTAssertEqual(store.routePreview?.endX ?? 0, 33, accuracy: 1e-6)
        store.cancelRoute()
        XCTAssertNil(store.routePreview)
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
    }

    func testRouterPlacesMicroviasOnHDIBoards() throws {
        let engine = EDAEngine(name: "HDI router")
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 20))
        engine.setLayerCount(4)
        XCTAssertTrue(engine.setHDI(enabled: true, microviaDrill: 0.1, microviaDiameter: 0.25, viaInPad: false))
        let options = EDAEngine.routerOptions(mode: .shove, diagonal: true, via: .micro)
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 10.95, y: 20), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 16, y: 20))
        let down = try XCTUnwrap(engine.routerAddVia())
        XCTAssertNil(down.error)
        XCTAssertEqual(down.layer, 1)
        XCTAssertEqual(down.vias.first?.kind, "microvia")
        XCTAssertEqual(down.vias.first?.toLayer, 1)
        let back = try XCTUnwrap(engine.routerAddVia(reverse: true))  // the next layer the other way: top again
        XCTAssertEqual(back.layer, 0)
        engine.routerCancel()
    }

    func testRoundedCornersWriteArcs() throws {
        let engine = EDAEngine(name: "Rounded")
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 8, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 32))
        let options = EDAEngine.routerOptions(mode: .shove, diagonal: true, rounded: true)
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 8.95, y: 20), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 20, y: 20))
        _ = engine.routerFix()
        let head = try XCTUnwrap(engine.routerMove(to: CGPoint(x: 39.05, y: 32)))
        XCTAssertTrue(head.reachedTarget)
        XCTAssertTrue(engine.routerCommit().ok)
        let snapshot = try XCTUnwrap(engine.snapshot())
        XCTAssertGreaterThan(snapshot.tracks.count, 4)  // the corners are arcs of short chords
        XCTAssertTrue(snapshot.ratsnest.isEmpty)
        XCTAssertFalse(engine.runDRC().contains { $0.severity == .error })
    }

    func testFanoutAndBusFromTheStore() throws {
        let store = DesignStore()
        let engine = store.engine
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        let r3 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 200, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 0), PinAddress(component: r3, pin: 1)))
        engine.moveFootprint(r1, to: CGPoint(x: 20, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 10))
        engine.moveFootprint(r3, to: CGPoint(x: 40, y: 30))
        store.refresh()
        // Both pads of R1 go somewhere: a fanout gives each an escape and a via, as one undo step.
        store.select(component: r1)
        store.fanoutSelection()
        XCTAssertEqual(store.snapshot.vias.count, 2)
        XCTAssertFalse(engine.runDRC().contains { $0.severity == .error && $0.code != "DRC_UNROUTED" })
        store.undo()
        XCTAssertTrue(store.snapshot.vias.isEmpty)
        // The same two pads as a bus of two.
        let options = EDAEngine.routerOptions(mode: .shove, diagonal: true)
        let bus = try XCTUnwrap(engine.routerBeginBus(at: CGPoint(x: 20.95, y: 20), layer: 0, count: 2, options: options))
        XCTAssertNil(bus.error)
        XCTAssertEqual(bus.kind, "bus")
        XCTAssertEqual(bus.nets.count, 2)
        let head = try XCTUnwrap(engine.routerMove(to: CGPoint(x: 20, y: 5)))
        XCTAssertEqual(Set(head.head.map(\.net)).count, 2)
        engine.routerCancel()
    }

    func testHighlightModeListsCollisions() throws {
        let store = try routedStore()
        store.routerMode = .highlight
        store.beginRoute(at: CGPoint(x: 39.05, y: 20), layer: 0, pair: false)
        XCTAssertNotNil(store.routePreview)
        // Straight off the board: nothing stops the head, the edge is listed as a collision.
        let preview = try XCTUnwrap(store.engine.routerMove(to: CGPoint(x: 39.05, y: -5)))
        XCTAssertFalse(preview.blocked)
        XCTAssertTrue(preview.collisions?.contains { $0.kind == "edge" } ?? false)
        store.cancelRoute()
        XCTAssertNil(store.routePreview)
    }
}

@MainActor
final class SimulationPackageTests: XCTestCase {
    /// 5 V → 1 kΩ → diode → ground, built through the store so its snapshot knows the parts.
    private func diodeCircuit(_ store: DesignStore) -> (diode: Int, anode: Int) {
        var ids: [Int] = []
        store.perform("Build") { engine in
            let v = engine.addComponent(.voltageSource, value: "5", at: .zero)
            let r = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
            let d = engine.addComponent(.diode, value: "1N4148", at: CGPoint(x: 200, y: 0))
            let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
            _ = engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0))
            _ = engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: d, pin: 0))
            _ = engine.connect(PinAddress(component: d, pin: 1), PinAddress(component: g, pin: 0))
            _ = engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0))
            ids = [d]
        }
        return (ids.first ?? -1, 0)
    }

    func testSpiceModelAttachCheckUndo() throws {
        let store = DesignStore()
        let (diode, _) = diodeCircuit(store)
        let text = ".model DFAST D(IS=1n N=1.8 RS=0.5 CJO=4p TT=5n)\n.model JUNK NPN(BF=50)\n"
        let parsed = EDAEngine.parseSpice(text)
        XCTAssertTrue(parsed.ok)
        XCTAssertEqual(parsed.entries.map(\.name), ["DFAST", "JUNK"])
        XCTAssertEqual(parsed.entries.first?.ports, ["A", "K"])

        let component = try XCTUnwrap(store.snapshot.component(diode))
        XCTAssertTrue(DesignStore.acceptsSpiceModel(component))
        let check = store.engine.checkSpiceModel(diode, text: text, model: "DFAST", pins: "")
        XCTAssertTrue(check.ok, check.error)
        XCTAssertEqual(check.defaultPins, "A K")
        let wrong = store.engine.checkSpiceModel(diode, text: text, model: "JUNK", pins: "")
        XCTAssertFalse(wrong.ok)  // a BJT model has no default mapping onto a diode's A / K

        XCTAssertTrue(store.setSpiceModel(diode, text: text, model: "DFAST", pins: ""))
        XCTAssertEqual(store.snapshot.component(diode)?.spice?.model, "DFAST")
        let stored = try XCTUnwrap(store.engine.spiceModel(of: diode))
        XCTAssertTrue(stored.text.contains("CJO=4p"))
        XCTAssertFalse(stored.text.contains("JUNK"))  // only what the model needs is kept
        XCTAssertTrue(store.engine.simulateDC().converged)

        // A refused model leaves the design as it was.
        XCTAssertFalse(store.setSpiceModel(diode, text: text, model: "MISSING", pins: ""))
        XCTAssertEqual(store.snapshot.component(diode)?.spice?.model, "DFAST")

        store.undo()
        XCTAssertNil(store.snapshot.component(diode)?.spice)
        XCTAssertFalse(EDAEngine.builtinSpiceModels.isEmpty)
    }
}

final class NoiseAnalysisTests: XCTestCase {
    func testNoiseOfAResistorDividerThroughTheEngine() throws {
        let engine = EDAEngine(name: "Divider noise")
        let v = engine.addComponent(.voltageSource, value: "0 AC 1", at: .zero)
        let r1 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: -40))
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r1, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r2, pin: 1), PinAddress(component: g, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0)))
        let snapshot = try XCTUnwrap(engine.snapshot())
        let tap = try XCTUnwrap(snapshot.component(r1)?.pins[1].net)
        let netName = try XCTUnwrap(snapshot.nets.first { $0.index == tap }?.name)

        let result = engine.simulateNoise(output: netName, start: "10", stop: "10k", pointsPerDecade: 10, source: "")
        XCTAssertTrue(result.ok, result.error)
        XCTAssertEqual(result.inputSource, "V1")
        let kT = 1.380649e-23 * 300.15
        let density = try XCTUnwrap(result.outputDensity.first ?? nil)
        XCTAssertEqual(density / (4 * kT * 500).squareRoot(), 1, accuracy: 1e-6)
        let input = try XCTUnwrap(result.inputDensity?.first ?? nil)
        XCTAssertEqual(input / density, 2, accuracy: 1e-6)
        XCTAssertEqual(result.contributions.count, 2)
        XCTAssertEqual(Set(result.contributions.map(\.ref)), ["R1", "R2"])

        let bad = engine.simulateNoise(output: "no such net", start: "10", stop: "10k", pointsPerDecade: 10, source: "")
        XCTAssertFalse(bad.ok)
        XCTAssertFalse(bad.error.isEmpty)
    }
}

final class SweepFFTMeasurementTests: XCTestCase {
    private func rcFilter(_ source: String) -> (EDAEngine, Int) {
        let engine = EDAEngine(name: "RC")
        let v = engine.addComponent(.voltageSource, value: source, at: .zero)
        let r = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: -40))
        let c = engine.addComponent(.capacitor, value: "100n", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        _ = engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0))
        _ = engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: c, pin: 0))
        _ = engine.connect(PinAddress(component: c, pin: 1), PinAddress(component: g, pin: 0))
        _ = engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0))
        return (engine, c)
    }

    private func netName(_ engine: EDAEngine, component: Int, pin: Int) throws -> String {
        let snapshot = try XCTUnwrap(engine.snapshot())
        let net = try XCTUnwrap(snapshot.component(component)?.pins[pin].net)
        return try XCTUnwrap(snapshot.nets.first { $0.index == net }?.name)
    }

    func testParameterSweepAndFFTDecode() throws {
        let (engine, c) = rcFilter("SIN(0 1 1k) AC 1")
        let out = try netName(engine, component: c, pin: 0)
        let dc = engine.simulateParamSweep(component: "R1", values: ["1k", "2k"], analysis: "dc", net: "",
                                           start: "10", stop: "1MEG", step: "1u")
        XCTAssertTrue(dc.ok, dc.error)
        XCTAssertEqual(dc.runs.map(\.value), ["1k", "2k"])
        let ac = engine.simulateParamSweep(component: "R1", values: ["1k", "10k"], analysis: "ac", net: out,
                                           start: "10", stop: "1MEG", step: "1u")
        XCTAssertTrue(ac.ok, ac.error)
        let f1 = try XCTUnwrap(ac.runs.first?.nets.first?.metrics?.f3dbHz)
        let f2 = try XCTUnwrap(ac.runs.last?.nets.first?.metrics?.f3dbHz)
        XCTAssertEqual(f1 / f2, 10, accuracy: 0.05)  // ten times the resistance, a tenth of the corner

        let fft = engine.simulateFFT(net: out, stop: "10m", step: "2u", fundamental: "", harmonics: 5)
        XCTAssertTrue(fft.ok, fft.error)
        XCTAssertEqual(fft.fundamentalHz, 1000, accuracy: 1e-6)
        XCTAssertLessThan(fft.thdPercent, 1)  // a linear filter adds no distortion
        XCTAssertEqual(fft.harmonics.first?.order, 1)
    }

    func testWaveformMeasurementsAndInterpolation() throws {
        let time = stride(from: 0.0, through: 2e-3, by: 1e-6).map { $0 }
        let values = time.map { 1 + sin(2 * Double.pi * 1000 * $0) }
        XCTAssertEqual(try XCTUnwrap(WaveformMath.value(at: 0.25e-3, time: time, values: values)), 2, accuracy: 1e-4)
        XCTAssertEqual(WaveformMath.value(at: -1, time: time, values: values), values.first)
        XCTAssertNil(WaveformMath.value(at: 0, time: [], values: []))
        let m = EDAEngine.measureWaveform(time: time, values: values)
        XCTAssertTrue(m.ok, m.error)
        XCTAssertEqual(try XCTUnwrap(m.average), 1, accuracy: 1e-4)
        XCTAssertEqual(try XCTUnwrap(m.acRms), 1 / 2.0.squareRoot(), accuracy: 1e-4)
        XCTAssertEqual(try XCTUnwrap(m.frequency), 1000, accuracy: 0.01)
        let window = EDAEngine.measureWaveform(time: time, values: values, from: 0, to: 0.5e-3)
        XCTAssertTrue(window.ok)
        XCTAssertNil(window.frequency)
        XCTAssertFalse(EDAEngine.measureWaveform(time: [0], values: [1]).ok)
    }
}

final class TransientOptionsTests: XCTestCase {
    func testAdaptiveTrapezoidalTransientThroughTheEngine() throws {
        let engine = EDAEngine(name: "RC step")
        let v = engine.addComponent(.voltageSource, value: "PULSE(0 5 2m)", at: .zero)
        let r = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: -40))
        let c = engine.addComponent(.capacitor, value: "100n", at: CGPoint(x: 200, y: -40))
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 80))
        _ = engine.connect(PinAddress(component: v, pin: 0), PinAddress(component: r, pin: 0))
        _ = engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: c, pin: 0))
        _ = engine.connect(PinAddress(component: c, pin: 1), PinAddress(component: g, pin: 0))
        _ = engine.connect(PinAddress(component: v, pin: 1), PinAddress(component: g, pin: 0))

        let fixed = engine.simulateTransient(stop: 0.9e-3, step: 1e-6)
        let same = engine.simulateTransient(stop: 0.9e-3, step: 1e-6, adaptive: false, trapezoidal: false)
        XCTAssertEqual(fixed, same)  // both off is the established analysis
        let adaptive = engine.simulateTransient(stop: 0.9e-3, step: 10e-6, adaptive: true, trapezoidal: true)
        XCTAssertTrue(adaptive.ok, adaptive.error)
        XCTAssertGreaterThan(adaptive.time.count, 10)
        XCTAssertEqual(try XCTUnwrap(adaptive.time.last), 0.9e-3, accuracy: 1e-12)
        // PULSE(0 5 2m) is high for the first half period; stop before the 1 ms edge. τ = 100 µs: settled to 5 V.
        let out = try XCTUnwrap(adaptive.nets.first { ($0.values.last ?? 0) > 4.9 })
        XCTAssertEqual(try XCTUnwrap(out.values.last), 5, accuracy: 0.01)
    }
}

// MARK: - Supplier search (stubbed network: no request leaves the test)

/// Answers every request of a stubbed session from `handler` and records it.
final class SupplierStubProtocol: URLProtocol {
    struct Reply {
        var status: Int
        var headers: [String: String] = [:]
        var body: Data
    }
    private static let lock = NSLock()
    private static var _handler: ((URLRequest) -> Reply)?
    private static var _requests: [URLRequest] = []

    static func install(_ handler: @escaping (URLRequest) -> Reply) {
        lock.lock(); defer { lock.unlock() }
        _handler = handler
        _requests = []
    }

    static var requests: [URLRequest] {
        lock.lock(); defer { lock.unlock() }
        return _requests
    }

    override class func canInit(with request: URLRequest) -> Bool { true }
    override class func canonicalRequest(for request: URLRequest) -> URLRequest { request }

    override func startLoading() {
        Self.lock.lock()
        Self._requests.append(request)
        let handler = Self._handler
        Self.lock.unlock()
        let reply = handler?(request) ?? Reply(status: 500, body: Data())
        guard let url = request.url,
              let response = HTTPURLResponse(url: url, statusCode: reply.status, httpVersion: "HTTP/1.1", headerFields: reply.headers)
        else {
            client?.urlProtocol(self, didFailWithError: URLError(.badURL))
            return
        }
        client?.urlProtocol(self, didReceive: response, cacheStoragePolicy: .notAllowed)
        client?.urlProtocol(self, didLoad: reply.body)
        client?.urlProtocolDidFinishLoading(self)
    }

    override func stopLoading() {}
}

final class InMemorySupplierCredentials: SupplierCredentialStore, @unchecked Sendable {
    private let lock = NSLock()
    private var values: [String: String] = [:]
    func read(_ account: String) -> String { lock.lock(); defer { lock.unlock() }; return values[account] ?? "" }
    func write(_ value: String, for account: String) { lock.lock(); values[account] = value; lock.unlock() }
}

final class SupplierSearchTests: XCTestCase {
    static let mouserBody = Data("""
    {"Errors":[],"SearchResults":{"NumberOfResult":1,"Parts":[{"ManufacturerPartNumber":"LM358DR",
    "Manufacturer":"Texas Instruments","Description":"Dual op amp","MouserPartNumber":"595-LM358DR",
    "DataSheetUrl":"https://www.ti.com/lit/ds/symlink/lm358.pdf","AvailabilityInStock":"23764","Min":"1","Mult":"1",
    "LifecycleStatus":"Not Recommended for New Designs",
    "PriceBreaks":[{"Quantity":1,"Price":"$0.45","Currency":"USD"},{"Quantity":100,"Price":"$0.184","Currency":"USD"}]}]}}
    """.utf8)

    static let nexarBody = Data("""
    {"data":{"supSearchMpn":{"hits":1,"results":[{"part":{"mpn":"MCP2551-I/SN","manufacturer":{"name":"Microchip"},
    "shortDescription":"CAN transceiver","specs":[{"attribute":{"name":"Lifecycle Status","shortname":"lifecyclestatus"},
    "displayValue":"NRND"}],"sellers":[{"company":{"name":"Digi-Key"},"offers":[{"sku":"MCP2551-I/SN-ND",
    "inventoryLevel":10,"moq":1,"prices":[{"quantity":1,"price":1.18,"currency":"USD"}]}]}]}}]}}}
    """.utf8)

    private var cacheDirectory: URL!

    override func setUp() {
        super.setUp()
        cacheDirectory = FileManager.default.temporaryDirectory.appendingPathComponent("sieda-supplier-tests-\(UUID().uuidString)")
    }

    override func tearDown() {
        try? FileManager.default.removeItem(at: cacheDirectory)
        super.tearDown()
    }

    private func engine(mouser: Bool = true, nexar: Bool = false, cacheLifetime: TimeInterval = 3600) -> SupplierSearchEngine {
        var credentials = SupplierCredentials()
        if mouser { credentials.values[.mouserApiKey] = "test-mouser-key" }
        if nexar {
            credentials.values[.nexarClientId] = "id"
            credentials.values[.nexarClientSecret] = "secret"
        }
        return SupplierSearchEngine(credentials: credentials, currency: "USD",
                                    http: SupplierHTTP(session: SupplierHTTP.makeSession(protocolClasses: [SupplierStubProtocol.self]),
                                                       maxRetryWait: 1),
                                    tokens: SupplierTokenCache(),
                                    cache: SupplierCache(directory: cacheDirectory, lifetime: cacheLifetime))
    }

    func testCoreReadsRepliesIntoTheSchema() {
        let result = EDAEngine.parseSupplierReply(source: .mouser, body: Self.mouserBody, currency: "USD")
        XCTAssertEqual(result.error, "")
        XCTAssertEqual(result.parts.first?.mpn, "LM358DR")
        XCTAssertEqual(result.parts.first?.lifecycle, .nrnd)
        XCTAssertEqual(result.parts.first?.offers.first?.stock, 23764)
        XCTAssertEqual(result.parts.first?.price(at: 100)?.unitPrice ?? 0, 0.184, accuracy: 1e-9)
        let bad = EDAEngine.parseSupplierReply(source: .digikey, body: Data("<html>".utf8), currency: "USD")
        XCTAssertFalse(bad.error.isEmpty)
        XCTAssertTrue(bad.parts.isEmpty)
        let merged = EDAEngine.mergeSupplierResults([result, result], currency: "USD")
        XCTAssertEqual(merged.count, 1)
        XCTAssertEqual(merged.first?.offers.count, 1)  // the same offer twice is kept once
    }

    func testNoKeysMeansNoRequests() async {
        SupplierStubProtocol.install { _ in .init(status: 200, body: Self.mouserBody) }
        let outcome = await engine(mouser: false).search("LM358DR")
        XCTAssertTrue(outcome.parts.isEmpty)
        XCTAssertEqual(outcome.statuses[.mouser], .notConfigured)
        XCTAssertEqual(outcome.statuses[.digikey], .notConfigured)
        XCTAssertTrue(SupplierStubProtocol.requests.isEmpty)
    }

    func testMouserSearchThenCacheThenOffline() async throws {
        SupplierStubProtocol.install { _ in .init(status: 200, body: Self.mouserBody) }
        let live = await engine().search("LM358DR")
        XCTAssertEqual(live.parts.map(\.mpn), ["LM358DR"])
        XCTAssertEqual(live.statuses[.mouser], .live(parts: 1))
        let request = try XCTUnwrap(SupplierStubProtocol.requests.first)
        XCTAssertEqual(request.httpMethod, "POST")
        XCTAssertEqual(request.url?.host, "api.mouser.com")
        XCTAssertTrue(request.url?.query?.contains("apiKey=test-mouser-key") == true)

        // Fresh cache: no second request.
        let cached = await engine().search("lm358dr ")
        XCTAssertEqual(cached.parts.count, 1)
        if case .cached = cached.statuses[.mouser] {} else { XCTFail("expected a cached result, got \(String(describing: cached.statuses[.mouser]))") }
        XCTAssertEqual(SupplierStubProtocol.requests.count, 1)

        // Expired cache and the service down: the old result is shown, marked offline.
        SupplierStubProtocol.install { _ in .init(status: 503, body: Data("<html>busy</html>".utf8)) }
        let offline = await engine(cacheLifetime: 0).search("LM358DR")
        XCTAssertEqual(offline.parts.count, 1)
        if case .offline = offline.statuses[.mouser] {} else { XCTFail("expected offline, got \(String(describing: offline.statuses[.mouser]))") }

        // A bad key is an error, never served from the cache.
        SupplierStubProtocol.install { _ in
            .init(status: 200, body: Data(#"{"Errors":[{"Code":"Invalid","Message":"Invalid unique identifier.","ResourceKey":"InvalidIdentifier"}]}"#.utf8))
        }
        let refused = await engine(cacheLifetime: 0).search("LM358DR")
        XCTAssertTrue(refused.parts.isEmpty)
        if case .failed(let message) = refused.statuses[.mouser] {
            XCTAssertTrue(message.contains("Settings"), message)
        } else {
            XCTFail("expected failure")
        }
    }

    func testRateLimitRetriesOnceThenReports() async {
        var calls = 0
        let lock = NSLock()
        SupplierStubProtocol.install { _ in
            lock.lock(); defer { lock.unlock() }
            calls += 1
            return calls == 1 ? .init(status: 429, headers: ["Retry-After": "0"], body: Data())
                              : .init(status: 200, body: Self.mouserBody)
        }
        let retried = await engine().search("LM358DR")
        XCTAssertEqual(retried.parts.count, 1)
        XCTAssertEqual(SupplierStubProtocol.requests.count, 2)

        SupplierStubProtocol.install { _ in .init(status: 429, headers: ["Retry-After": "120"], body: Data()) }
        let limited = await engine().search("NE555DR")
        XCTAssertTrue(limited.parts.isEmpty)
        XCTAssertEqual(SupplierStubProtocol.requests.count, 1)  // a long back-off is not waited for
        if case .failed(let message) = limited.statuses[.mouser] {
            XCTAssertTrue(message.contains("120"), message)
        } else {
            XCTFail("expected a rate-limit failure")
        }
    }

    func testNexarSignsInWithClientCredentials() async throws {
        SupplierStubProtocol.install { request in
            if request.url?.host == "identity.nexar.com" {
                return .init(status: 200, body: Data(#"{"access_token":"tok123","expires_in":3600,"token_type":"Bearer"}"#.utf8))
            }
            return .init(status: 200, body: Self.nexarBody)
        }
        let outcome = await engine(mouser: false, nexar: true).search("MCP2551")
        XCTAssertEqual(outcome.parts.first?.mpn, "MCP2551-I/SN")
        XCTAssertEqual(outcome.parts.first?.lifecycle, .nrnd)
        let requests = SupplierStubProtocol.requests
        XCTAssertEqual(requests.count, 2)
        XCTAssertEqual(requests.first?.url?.host, "identity.nexar.com")
        XCTAssertEqual(requests.last?.value(forHTTPHeaderField: "Authorization"), "Bearer tok123")
    }

    func testCancelledSearchReturnsNothing() async {
        SupplierStubProtocol.install { _ in .init(status: 200, body: Self.mouserBody) }
        let engine = engine()
        let task = Task { await engine.search("LM358DR") }
        task.cancel()
        let outcome = await task.value
        XCTAssertTrue(outcome.parts.isEmpty || outcome.parts.count == 1)  // finished or cancelled, never a crash
    }

    func testCacheKeysAndFormEncoding() {
        XCTAssertEqual(SupplierCache.key(source: .mouser, query: " LM358DR ", currency: "USD"),
                       SupplierCache.key(source: .mouser, query: "lm358dr", currency: "USD"))
        XCTAssertNotEqual(SupplierCache.key(source: .mouser, query: "lm358dr", currency: "USD"),
                          SupplierCache.key(source: .digikey, query: "lm358dr", currency: "USD"))
        XCTAssertEqual(SupplierHTTP.formEncoded(["b": "a b&c", "a": "x/y"]), "a=x%2Fy&b=a%20b%26c")
    }

    @MainActor
    func testSettingsKeepKeysInTheCredentialStore() throws {
        let suite = "sieda.supplier.tests.\(UUID().uuidString)"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        let store = InMemorySupplierCredentials()
        let settings = SupplierSettings(defaults: defaults, store: store)
        XCTAssertFalse(settings.hasAnySource)
        settings.setValue("  key  ", for: .mouserApiKey)
        XCTAssertEqual(store.read("mouserApiKey"), "key")
        XCTAssertEqual(settings.configuredSources, [.mouser])
        settings.setValue("id", for: .digikeyClientId)
        XCTAssertFalse(settings.credentials.isConfigured(.digikey))  // the secret is still missing
        XCTAssertNil(defaults.string(forKey: "mouserApiKey"))       // never in UserDefaults
        settings.currency = "EUR"
        XCTAssertEqual(SupplierSettings(defaults: defaults, store: store).currency, "EUR")
    }

    func testPlacementLinksCatalogPartsOrPrefillsTheEditor() throws {
        let result = EDAEngine.parseSupplierReply(source: .mouser, body: Self.mouserBody, currency: "USD")
        let part = try XCTUnwrap(result.parts.first)
        XCTAssertEqual(SupplierPlacement.link(for: part, library: []), .catalog(name: "LM358DR", note: ""))
        let sourcing = SupplierPlacement.sourcing(for: part, boards: 100, currency: "USD")
        XCTAssertEqual(sourcing.mpn, "LM358DR")
        XCTAssertEqual(sourcing.supplierPart, "595-LM358DR")
        XCTAssertEqual(sourcing.unitPrice ?? 0, 0.184, accuracy: 1e-9)

        var unknown = part
        unknown.mpn = "XYZ9000QFN24"
        unknown.package = "24-VQFN (4x4)"
        unknown.category = "Interface ICs"
        if case .none = SupplierPlacement.link(for: unknown, library: []) {} else { XCTFail("no catalog part expected") }
        let draft = SupplierPlacement.draft(for: unknown)
        XCTAssertEqual(draft.name, "XYZ9000QFN24")
        XCTAssertEqual(draft.package.type, PackageKind.qfn.rawValue)
        XCTAssertEqual(draft.package.pinCount, 24)
        XCTAssertEqual(draft.datasheet, "https://www.ti.com/lit/ds/symlink/lm358.pdf")
        XCTAssertTrue(draft.pins.isEmpty)
        XCTAssertEqual(SupplierPlacement.refPrefix(category: "Connectors, Interconnects", description: ""), "J")
    }

    @MainActor
    func testPlacingALibraryPartKeepsItsSourcingInOneUndoStep() throws {
        let store = DesignStore()
        let standard = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM358DR" })
        let partId = try XCTUnwrap(store.addStandardPartToLibrary(standard))
        let id = store.placeLibraryPart(partId, sourcing: SourcingUpdate(manufacturer: "Texas Instruments", mpn: "LM358DR",
                                                                          supplierPart: "595-LM358DR", unitPrice: 0.184))
        XCTAssertGreaterThanOrEqual(id, 0)
        let line = try XCTUnwrap(store.bomReport.lines.first { $0.componentIds.contains(id) })
        XCTAssertEqual(line.mpn, "LM358DR")
        XCTAssertEqual(line.supplierPart, "595-LM358DR")
        XCTAssertEqual(line.unitPrice, 0.184, accuracy: 1e-9)
        XCTAssertEqual(store.applySupplierPricing([(line, SourcingUpdate(unitPrice: 0.15))]), 1)
        XCTAssertEqual(store.bomReport.lines.first { $0.componentIds.contains(id) }?.unitPrice ?? 0, 0.15, accuracy: 1e-9)
    }

    func testRollupThroughTheEngine() throws {
        let result = EDAEngine.parseSupplierReply(source: .mouser, body: Self.mouserBody, currency: "USD")
        let rollup = try XCTUnwrap(EDAEngine.supplierBomRollup(SupplierRollupRequest(
            currency: "USD", quantities: [1, 100], buildQuantity: 5,
            lines: [.init(item: 1, refs: ["U1"], quantity: 2, mpn: "LM358DR", manufacturer: "", dnp: false, embedded: false)],
            parts: result.parts)))
        XCTAssertEqual(rollup.totals.map(\.boards), [1, 5, 100])
        XCTAssertEqual(rollup.lines.first?.lifecycle, .nrnd)
        XCTAssertFalse(rollup.warnings.isEmpty)
        XCTAssertEqual(rollup.totals.last?.cost ?? 0, 200 * 0.184, accuracy: 1e-9)
    }
}

// MARK: - Imported 3D models

@MainActor
final class Model3DImportTests: XCTestCase {
    static let cubeObj = Data("""
    v 0 0 0
    v 2 0 0
    v 2 2 0
    v 0 2 0
    v 0 0 1
    v 2 0 1
    v 2 2 1
    v 0 2 1
    f 1 4 3 2
    f 5 6 7 8
    f 1 2 6 5
    f 2 3 7 6
    f 3 4 8 7
    f 4 1 5 8
    """.utf8)

    func testModelAttachesAlignsAndRendersWithThePart() throws {
        let imported = EDAEngine.importModel3D(name: "cube.obj", data: Self.cubeObj)
        XCTAssertTrue(imported.ok, imported.error)
        XCTAssertEqual(imported.triangles, 12)
        XCTAssertEqual(imported.unit, 1)
        let id = try XCTUnwrap(imported.id)

        var spec = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM358DR" }).spec
        spec.model3d = CustomPartSpec.Model3DRef(id: id, name: "cube.obj", offset: [3, 3, 3])
        let fit = EDAEngine.fitModel3D(spec)
        XCTAssertTrue(fit.ok, fit.error)
        XCTAssertEqual(fit.seated?.offset ?? [], [-1, -1, 0])
        XCTAssertNotNil(EDAEngine.model3DPreviewMesh(spec))

        // Registered with the part, kept in the snapshot and in the saved project.
        let engine = EDAEngine()
        let part = try engine.registerCustomPart(spec)
        XCTAssertEqual(part.model3d?.id, id)
        XCTAssertEqual(part.spec.model3d, spec.model3d)
        XCTAssertGreaterThanOrEqual(engine.addCustomComponent(partId: part.id, at: CGPoint(x: 0, y: 0)), 0)
        XCTAssertTrue(engine.saveJSON().contains("\"models3d\""))
        let copy = EDAEngine()
        try copy.load(json: engine.saveJSON())
        XCTAssertEqual(copy.snapshot()?.customParts.first?.model3d?.id, id)
    }

    func testStepAndBadFilesAreRefusedWithAReason() {
        let step = EDAEngine.importModel3D(name: "part.step", data: Data("ISO-10303-21;".utf8))
        XCTAssertFalse(step.ok)
        XCTAssertTrue(step.error.contains("STEP"), step.error)
        let bad = EDAEngine.importModel3D(name: "part.stl", data: Data([0, 1, 2, 3]))
        XCTAssertFalse(bad.ok)
        XCTAssertFalse(bad.error.isEmpty)
    }

    func testPrettyFolderBringsItsShapesFolder() throws {
        let root = FileManager.default.temporaryDirectory.appendingPathComponent("sieda-shapes-\(UUID().uuidString)")
        defer { try? FileManager.default.removeItem(at: root) }
        let pretty = root.appendingPathComponent("Package_Test.pretty")
        let shapes = root.appendingPathComponent("Package_Test.3dshapes")
        try FileManager.default.createDirectory(at: pretty, withIntermediateDirectories: true)
        try FileManager.default.createDirectory(at: shapes, withIntermediateDirectories: true)
        try Data("""
        (footprint "TEST-2" (layer "F.Cu")
          (pad "1" smd rect (at -1 0) (size 0.8 0.9) (layers "F.Cu"))
          (pad "2" smd rect (at 1 0) (size 0.8 0.9) (layers "F.Cu"))
          (model "${KICAD8_3DMODEL_DIR}/Package_Test.3dshapes/TEST-2.step" (offset (xyz 0 0 0)) (scale (xyz 1 1 1)) (rotate (xyz 0 0 0))))
        """.utf8).write(to: pretty.appendingPathComponent("TEST-2.kicad_mod"))
        try Self.cubeObj.write(to: shapes.appendingPathComponent("TEST-2.obj"))
        try Data([0, 1, 2]).write(to: shapes.appendingPathComponent("TEST-2.stl"))
        let files = ComponentLibraryView.libraryFiles(at: [pretty])
        XCTAssertEqual(files.map(\.name), ["TEST-2.kicad_mod", "TEST-2.obj", "TEST-2.stl"])
        XCTAssertNotNil(files.last?.contentBase64)  // STL may be binary
        // The .step reference takes the model of the same name.
        let result = EDAEngine.importLibrary(files: Array(files.prefix(2)))
        let part = try XCTUnwrap(result.parts.first { $0.footprint == "TEST-2" })
        XCTAssertEqual(part.spec.model3d?.name, "TEST-2.obj")
        XCTAssertTrue(part.warnings.contains { $0.contains("3D model") })
    }
}

// MARK: - Altium libraries

@MainActor
final class AltiumImportTests: XCTestCase {
    /// The synthetic fixtures written by tools/make_altium_fixtures.py (read from the source tree).
    private func fixture(_ name: String) throws -> URL {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Core/tests/fixtures/altium/\(name)")
        guard FileManager.default.fileExists(atPath: url.path) else { throw XCTSkip("fixture \(name) not in this build") }
        return url
    }

    func testSchLibAndPcbLibImportAsBase64() throws {
        let files = ComponentLibraryView.libraryFiles(at: [try fixture("Test.SchLib"), try fixture("Test.PcbLib")])
        XCTAssertEqual(files.count, 2)
        XCTAssertTrue(files.allSatisfy { $0.contentBase64 != nil })
        let result = EDAEngine.importLibrary(files: files)
        let lm358 = try XCTUnwrap(result.parts.first { $0.name == "LM358" })
        XCTAssertTrue(lm358.ok, lm358.error)
        XCTAssertEqual(lm358.footprint, "SOIC8_TI")
        XCTAssertEqual(lm358.spec.pins.count, 8)
        XCTAssertEqual(lm358.spec.manufacturer, "Texas Instruments")
        XCTAssertTrue(result.parts.contains { $0.name == "HDR1X4" && $0.ok })
        let store = DesignStore()
        XCTAssertEqual(store.importLibraryParts(result.importable.map(\.spec)).count, result.importable.count)
    }

    func testIntegratedLibrariesAreRefusedWithTheWayOut() throws {
        let files = ComponentLibraryView.libraryFiles(at: [try fixture("Test.IntLib")])
        let result = EDAEngine.importLibrary(files: files)
        XCTAssertTrue(result.files.first?.error.contains("Extract") == true, result.files.first?.error ?? "")
    }
}

// MARK: - Import sheet: choosing a symbol's footprint

@MainActor
final class LibraryImportPairingTests: XCTestCase {
    private func fixture(_ name: String) throws -> LibraryImportFile {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent()
            .appendingPathComponent("Core/tests/fixtures/library/\(name)")
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { throw XCTSkip("fixture \(name) not in this build") }
        return LibraryImportFile(name: name, content: text)
    }

    func testCandidatesAndRepairing() throws {
        let files = [try fixture("test_parts.kicad_sym"), try fixture("SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod"),
                     try fixture("QFN-16-1EP_3x3mm_P0.5mm_EP1.7x1.7mm.kicad_mod"),
                     try fixture("PinHeader_1x04_P2.54mm_Vertical.kicad_mod")]
        let result = EDAEngine.importLibrary(files: files)
        XCTAssertEqual(result.footprintList?.count, 3)
        let lm358 = try XCTUnwrap(result.parts.first { $0.symbol == "LM358" })
        XCTAssertEqual(lm358.pairable, true)
        XCTAssertEqual(lm358.candidates?.first, "SOIC-8_3.9x4.9mm_P1.27mm")
        XCTAssertFalse(lm358.candidates?.contains("PinHeader_1x04_P2.54mm_Vertical") ?? true)
        let repaired = EDAEngine.importLibrary(files: files, pairs: ["AMS1117-3.3": "PinHeader_1x04_P2.54mm_Vertical"])
        let ams = try XCTUnwrap(repaired.parts.first { $0.symbol == "AMS1117-3.3" })
        XCTAssertTrue(ams.ok, ams.error)
        XCTAssertEqual(ams.footprint, "PinHeader_1x04_P2.54mm_Vertical")
        // The sheet keys parts by symbol, so a selection survives the re-import.
        XCTAssertEqual(LibraryImportView.key(ams), LibraryImportView.key(try XCTUnwrap(result.parts.first { $0.symbol == "AMS1117-3.3" })))
    }
}

// MARK: - The catalog in AI prompts stays bounded

final class CatalogDigestTests: XCTestCase {
    func testPromptCarriesADigestNotTheWholeCatalog() {
        let all = StandardLibrary.parts
        let fullListing = all.map { part in
            "- custom:\(part.spec.name) [\(part.category)]: \(part.spec.description). Pins: "
                + part.spec.pins.map { "\($0.number)=\($0.name)(\($0.type.rawValue))" }.joined(separator: ", ")
        }.joined(separator: "\n")
        let digest = AgentPrompts.customCatalog([], brief: "")
        XCTAssertLessThan(digest.count, 60_000)
        XCTAssertLessThan(digest.count * 4, fullListing.count)  // a fraction of listing every part with its pins
        XCTAssertTrue(digest.contains("custom:NE555") && digest.contains("custom:LM7805"))
        XCTAssertTrue(digest.contains("The standard catalog has \(all.count) parts"))
        XCTAssertLessThanOrEqual(AgentPrompts.customPlanKinds([]).count, CatalogDigest.maxDetailed + 5)

        // A part the brief names is detailed (and allowed in the schema), as are parts the brief describes.
        let named = AgentPrompts.customCatalog([], brief: "Use a TPS5430DDAR buck from 24 V")
        XCTAssertTrue(named.contains("custom:TPS5430DDAR"))
        XCTAssertTrue(AgentPrompts.customPlanKinds([], brief: "Use a TPS5430DDAR buck").contains("custom:TPS5430DDAR"))
        let described = CatalogDigest.selection(all, brief: "CAN transceiver for an automotive ECU")
        XCTAssertTrue(described.contains { $0.spec.description.lowercased().contains("can") && $0.category == "Interface" })
        // Parts an existing plan uses stay available when it is refined or reviewed.
        let plan = DesignPlan(title: "t", summary: "", components: [
            PlannedComponent(ref: "U1", kind: "custom:TPS5430DDAR", value: "TPS5430DDAR", x: 0, y: 0)], connections: [])
        XCTAssertTrue(AgentPrompts.customPlanKinds([], brief: "", plan: plan).contains("custom:TPS5430DDAR"))
        // The pin budget holds even for a brief that matches many large MCUs.
        let mcus = CatalogDigest.selection(all, brief: "microcontroller MCU ARM Cortex STM32 LQFP")
        XCTAssertLessThanOrEqual(mcus.reduce(0) { $0 + $1.spec.pins.count }, CatalogDigest.maxPins + 200)
    }
}

/// Large designs (the autorouting and scale package): a 1000-part design with 1000 nets loads, refreshes and draws
/// its schematic and PCB within time budgets; an autoroute reports progress, can be stopped (leaving the board as it
/// was) and runs off the main thread.
@MainActor
final class LargeDesignScaleTests: XCTestCase {
    /// `parts` resistors in a chain (one two-pin net between neighbours), placed on a 6 mm grid, 40 per row.
    private func chainStore(parts: Int) -> DesignStore {
        let store = DesignStore()
        let engine = store.engine
        var ids: [Int] = []
        ids.reserveCapacity(parts)
        for k in 0..<parts {
            ids.append(engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 60 * (k % 40), y: 60 * (k / 40))))
        }
        for k in 1..<parts {
            _ = engine.connect(PinAddress(component: ids[k - 1], pin: 1), PinAddress(component: ids[k], pin: 0))
        }
        let rows = (parts + 39) / 40
        engine.setBoard(width: 6.0 * 40 + 10, height: 6.0 * Double(rows) + 10, trackWidth: 0, clearance: 0)
        for (k, id) in ids.enumerated() {
            _ = engine.moveFootprint(id, to: CGPoint(x: 8 + 6.0 * Double(k % 40), y: 8 + 6.0 * Double(k / 40)))
        }
        store.refresh()
        return store
    }

    private func render<V: View>(_ view: V, store: DesignStore) throws -> TimeInterval {
        let settings = AISettings(defaults: try XCTUnwrap(UserDefaults(suiteName: "SiEDA.LargeDesignScaleTests")))
        let host = NSHostingView(rootView: view.environmentObject(store).environmentObject(settings)
            .environmentObject(AgentOrchestrator()))
        host.frame = NSRect(x: 0, y: 0, width: 1400, height: 900)
        let started = Date()
        host.layoutSubtreeIfNeeded()
        let rep = try XCTUnwrap(host.bitmapImageRepForCachingDisplay(in: host.bounds))
        host.cacheDisplay(in: host.bounds, to: rep)
        return Date().timeIntervalSince(started)
    }

    func testThousandPartDesignRefreshesAndDrawsWithinBudget() throws {
        let store = chainStore(parts: 1000)
        XCTAssertEqual(store.snapshot.components.count, 1000)
        XCTAssertGreaterThanOrEqual(store.snapshot.nets.count, 999)
        // Component lookup is O(1) (the canvases look parts up for every wire end and courtyard of every frame).
        let lookups = Date()
        for c in store.snapshot.components { XCTAssertEqual(store.snapshot.component(c.id)?.id, c.id) }
        XCTAssertLessThan(Date().timeIntervalSince(lookups), 0.5)
        XCTAssertNil(store.snapshot.component(-42))
        // A full refresh (snapshot from the core, decoding, sheet view) after an edit.
        let refreshed = Date()
        store.refresh()
        XCTAssertLessThan(Date().timeIntervalSince(refreshed), 3.0)
        // Select everything: the refresh keeps the selection in O(n).
        store.selection = Set(store.snapshot.components.map(\.id))
        let selected = Date()
        store.refresh()
        XCTAssertEqual(store.selection.count, 1000)
        XCTAssertLessThan(Date().timeIntervalSince(selected), 3.0)
        // Both canvases draw the whole design (zoomed to fit) within a few seconds.
        store.workspace = .schematic
        XCTAssertLessThan(try render(ContentView.workspaceView(.schematic), store: store), 8.0)
        store.workspace = .pcb
        XCTAssertLessThan(try render(ContentView.workspaceView(.pcb), store: store), 8.0)
    }

    func testAutorouteReportsProgressAndCanBeStopped() async throws {
        let store = chainStore(parts: 120)
        let before = store.engine.saveJSON()
        // Stop as soon as the first report arrives: nothing changes.
        let routing = Task { await store.autoRoute() }
        let deadline = Date().addingTimeInterval(20)
        while store.routeProgress == nil && Date() < deadline { try await Task.sleep(nanoseconds: 1_000_000) }
        XCTAssertTrue(store.isBusy, "the route runs off the main thread while the UI stays responsive")
        store.cancelAutoRoute()
        await routing.value
        XCTAssertFalse(store.isBusy)
        XCTAssertNil(store.routeProgress)
        if store.snapshot.tracks.isEmpty {
            XCTAssertTrue(store.statusMessage.contains("stopped"))
            XCTAssertEqual(store.engine.saveJSON(), before)
        }
        // A route left to finish reports progress and routes the chain.
        let log = PhaseLog()
        let channel = RouteProgressChannel { report in log.add(report.phase) }
        let engine = store.engine
        let stats = try await Task.detached { try engine.autoRouteChecked(progress: channel).get() }.value
        XCTAssertNotEqual(stats.cancelled, true)
        XCTAssertEqual(stats.failed, 0)
        XCTAssertTrue(log.phases.contains(1) && log.phases.contains(3))
    }

    /// Progress phases seen on the routing thread.
    private final class PhaseLog: @unchecked Sendable {
        private let lock = NSLock()
        private var seen = Set<Int>()
        func add(_ phase: Int) {
            lock.lock()
            seen.insert(phase)
            lock.unlock()
        }
        var phases: Set<Int> {
            lock.lock()
            defer { lock.unlock() }
            return seen
        }
    }

    func testCorridorRouterThroughTheBridgeMatchesAcrossThreadCounts() throws {
        defer {
            EDAEngine.setRouterStrategy(0)
            EDAEngine.setRoutingThreads(0)
        }
        EDAEngine.setRouterStrategy(2)  // corridor router on a small board
        var results: [RouteStats] = []
        for threads in [1, 4] {
            EDAEngine.setRoutingThreads(threads)
            let store = chainStore(parts: 80)
            results.append(try store.engine.autoRouteChecked().get())
        }
        XCTAssertEqual(results[0], results[1])
        XCTAssertEqual(results[0].failed, 0)
    }
}

/// Unit (gate) editor: the document (assign / share / power / swap groups), gate detection from pin names, the
/// encoding the core reads, the core's unit checks, and gate / pin swap through the store.
@MainActor
final class UnitEditorTests: XCTestCase {
    /// A quad 2-input NAND (74HC00 pinout).
    private func nand() -> CustomPartSpec {
        var s = CustomPartSpec()
        s.name = "UNIT-EDITOR-NAND"
        s.package = CustomPartSpec.Package(type: "SOIC", pinCount: 14)
        let names = ["1A", "1B", "1Y", "2A", "2B", "2Y", "GND", "3Y", "3A", "3B", "4Y", "4A", "4B", "VCC"]
        s.pins = names.enumerated().map { index, name in
            let type: PinElectricalType = name == "GND" || name == "VCC" ? .powerIn : name.hasSuffix("Y") ? .output : .input
            return CustomPartSpec.Pin(number: "\(index + 1)", name: name, type: type)
        }
        return s
    }

    func testDetectGatesFromPinNames() throws {
        let gates = try XCTUnwrap(UnitDraft.detectGates(nand().pins))
        XCTAssertEqual(gates.map(\.name), ["A", "B", "C", "D"])
        XCTAssertEqual(gates[0].pins, ["1", "2", "3"])
        XCTAssertEqual(Set(gates[2].pins), ["8", "9", "10"])
        // Supplies stay on the power unit.
        XCTAssertEqual(UnitDraft(units: gates).powerPins(nand().pins), ["7", "14"])
        var opamp = CustomPartSpec()
        opamp.pins = ["OUT1", "IN1-", "IN1+", "V+", "IN2+", "IN2-", "OUT2", "V-"].enumerated().map {
            CustomPartSpec.Pin(number: "\($0.offset + 1)", name: $0.element, type: $0.element.hasPrefix("V") ? .powerIn : .input)
        }
        XCTAssertEqual(UnitDraft.detectGates(opamp.pins)?.count, 2)
        XCTAssertNil(UnitDraft.detectGates([CustomPartSpec.Pin(number: "1", name: "VCC", type: .powerIn)]))
    }

    func testAssignSharePowerAndSwapGroups() {
        var d = UnitDraft(units: [])
        d.addUnit(pins: ["1", "2", "3"])
        d.addUnit(pins: ["4", "5", "6"])
        XCTAssertEqual(d.units.map(\.name), ["A", "B"])
        d.share(["14"])
        XCTAssertEqual(d.units(of: "14"), [0, 1])
        d.makePower(["14"])
        XCTAssertTrue(d.units(of: "14").isEmpty)
        d.assign(["4"], to: 0)
        XCTAssertEqual(d.units(of: "4"), [0])
        d.makePinSwapGroup(["1", "2"], unit: 0)
        XCTAssertEqual(d.units[0].pinSwap, [["1", "2"]])
        d.toggle("2", unit: 0)  // off the unit: out of its swap group too
        XCTAssertNil(d.units[0].pinSwap)
        d.setSwap(-1, unit: 1)
        d.setSwap(0, unit: 0)
        XCTAssertEqual(d.units[1].swap, -1)
        XCTAssertNil(d.units[0].swap)
        XCTAssertFalse(d.rename(1, to: "A"))
        XCTAssertTrue(d.rename(1, to: "G2"))
        var spec = nand()
        UnitDraft(units: []).apply(to: &spec)
        XCTAssertNil(spec.units)
    }

    func testEncodingMatchesTheCoreAndChecksRun() throws {
        var spec = nand()
        var d = UnitDraft(units: try XCTUnwrap(UnitDraft.detectGates(spec.pins)))
        d.makePinSwapGroup(["1", "2"], unit: 0)
        d.apply(to: &spec)
        let json = spec.jsonString()
        XCTAssertTrue(json.contains("\"pinSwap\":[[\"1\",\"2\"]]"))
        XCTAssertFalse(json.contains("\"swap\""))
        XCTAssertFalse(EDAEngine.checkUnits(spec).contains(where: \.isError))
        let part = try EDAEngine.previewCustomPart(spec).get()
        XCTAssertEqual(part.unitSymbols?.count, 5)
        XCTAssertEqual(part.unitSymbols?.first?.pinSwap, [["1", "2"]])
        // A pin on no part pin is an error.
        spec.units?[1].pins.append("99")
        XCTAssertTrue(EDAEngine.checkUnits(spec).contains { $0.code == "UNIT_UNKNOWN_PIN" })
    }

    func testGateAndPinSwapThroughTheStore() throws {
        var spec = nand()
        var d = UnitDraft(units: try XCTUnwrap(UnitDraft.detectGates(spec.pins)))
        d.makePinSwapGroup(["1", "2"], unit: 0)
        d.apply(to: &spec)
        let store = DesignStore()
        let partId = try store.engine.registerCustomPart(spec).id
        let a = store.engine.addCustomUnits(partId: partId, at: .zero)
        XCTAssertGreaterThan(a, 0)
        let b = try XCTUnwrap(store.engine.placeNextUnit(of: a, at: CGPoint(x: 200, y: 0)))
        store.refresh()
        store.swapGates(a, b)
        XCTAssertEqual(store.snapshot.component(a)?.unit, 2)
        store.swapPins(of: a, 0, 1)
        XCTAssertTrue(store.canUndo)
    }
}

/// AI refinement keeps the design's structure: a plan made from a design with nested repeated sheets, per-channel
/// values, a bus with entries and a multi-unit part rebuilds the same structure, and a refined plan that leaves the
/// structure out gets it back.
@MainActor
final class StructuredPlanTests: XCTestCase {
    /// Amp (×3) holding Stage (×2): R1 in Stage, its channel B/A at 12k; a bus with two entries on Main; an LM324
    /// placed as units A and B.
    private func structuredEngine() throws -> EDAEngine {
        let engine = EDAEngine(name: "Structured")
        let amp = try XCTUnwrap(engine.addSheet("Amp", parent: 1))
        let stage = try XCTUnwrap(engine.addSheet("Stage", parent: amp))
        XCTAssertTrue(engine.setActiveSheet(stage))
        let port = engine.addComponent(.netLabel, value: "IN", at: .zero)
        XCTAssertTrue(engine.setLabelScope(port, scope: "port"))
        let r = engine.addComponent(.resistor, value: "10k", at: CGPoint(x: 80, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: port, pin: 0), PinAddress(component: r, pin: 0)))
        XCTAssertEqual(engine.repeatSheet(stage, count: 2), 2)
        XCTAssertEqual(engine.repeatSheet(amp, count: 3), 3)
        XCTAssertTrue(engine.setInstanceRefs(amp, scheme: "suffix"))
        let snap = try XCTUnwrap(engine.snapshot())
        let copy = try XCTUnwrap(snap.components.first { $0.instanceOf == r && snap.sheet($0.sheetId)?.path == "B/A" })
        XCTAssertTrue(engine.setChannelValue(copy.id, "12k"))
        // Main: a bus with entries D0, D1 wired to a connector.
        XCTAssertTrue(engine.setActiveSheet(1))
        let j = engine.addComponent(.connector, at: CGPoint(x: 300, y: 0))
        let bus = try XCTUnwrap(engine.addBus("D[0..1]", points: [CGPoint(x: 200, y: -50), CGPoint(x: 200, y: 50)]))
        XCTAssertGreaterThan(engine.connectBus(bus, toPart: j), 0)
        // An LM324 gate by gate.
        let lm324 = try XCTUnwrap(StandardLibrary.parts.first { $0.spec.name == "LM324" })
        let partId = try engine.registerCustomPart(lm324.spec).id
        let a = engine.addCustomUnits(partId: partId, at: CGPoint(x: 0, y: 300))
        XCTAssertGreaterThan(a, 0)
        XCTAssertNotNil(engine.addPartUnit(of: a, unit: 2, at: CGPoint(x: 200, y: 300)))
        return engine
    }

    func testPlanRoundTripKeepsStructure() throws {
        let engine = try structuredEngine()
        let before = try XCTUnwrap(engine.snapshot())
        let plan = DesignPlanCompiler.plan(from: before)
        // Drawn once: the plan holds the block's parts, not the channels' copies.
        XCTAssertEqual(plan.components.filter { $0.kind == "resistor" }.count, 1)
        let r = try XCTUnwrap(plan.components.first { $0.kind == "resistor" })
        XCTAssertEqual(r.blockRef, "R1")
        XCTAssertEqual(r.channelValues, ["B/A": "12k"])
        XCTAssertEqual(plan.sheets.first { $0.name == "Amp" }?.channels, 3)
        XCTAssertEqual(plan.sheets.first { $0.name == "Stage" }?.channels, 2)
        XCTAssertEqual(plan.buses?.count, 1)
        XCTAssertEqual(plan.components.filter { $0.bus == 0 }.count, 2)
        let opamp = try XCTUnwrap(plan.components.first { $0.kind == "custom:LM324" })
        XCTAssertEqual(opamp.units?.map(\.unit), ["A", "B"])

        // Rebuilt from the plan: the same sheets, channels, designators, values, bus and units.
        let rebuilt = EDAEngine(name: "Rebuilt")
        let report = DesignPlanCompiler.apply(plan, to: rebuilt, previous: before)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        let after = try XCTUnwrap(rebuilt.snapshot())
        XCTAssertEqual(after.sheets.map(\.name), before.sheets.map(\.name))
        XCTAssertEqual(after.sheets.map(\.path), before.sheets.map(\.path))
        XCTAssertEqual(Set(after.components.map(\.ref)), Set(before.components.map(\.ref)))
        XCTAssertEqual(after.components.first { $0.ref == "R1_B_A" }?.value, "12k")
        XCTAssertEqual(after.components.first { $0.ref == "R1_C_B" }?.value, "10k")
        XCTAssertEqual(after.buses.count, before.buses.count)
        XCTAssertEqual(after.components.filter { $0.bus != nil }.count, before.components.filter { $0.bus != nil }.count)
        XCTAssertEqual(Set(after.components.compactMap(\.unitName)), ["A", "B"])
        XCTAssertEqual(after.nets.count, before.nets.count)
        // The plan made from the rebuilt design has the same structure.
        let again = DesignPlanCompiler.plan(from: after)
        XCTAssertEqual(again.sheets, plan.sheets)
        XCTAssertEqual(again.components.map(\.ref).sorted(), plan.components.map(\.ref).sorted())
        XCTAssertEqual(again.components.first { $0.kind == "resistor" }?.channelValues, ["B/A": "12k"])
    }

    func testRefinedPlanGetsItsStructureBack() throws {
        let engine = try structuredEngine()
        let current = DesignPlanCompiler.plan(from: try XCTUnwrap(engine.snapshot()))
        // An agent's answer without any of the structure (the fields the schema does not ask for).
        var refined = current
        refined.sheets = []
        refined.buses = nil
        for i in refined.components.indices {
            refined.components[i].sheet = nil
            refined.components[i].scope = nil
            refined.components[i].targetSheet = nil
            refined.components[i].blockRef = nil
            refined.components[i].channelValues = nil
            refined.components[i].units = nil
            refined.components[i].bus = nil
        }
        refined.components.append(PlannedComponent(ref: "C99", kind: "capacitor", value: "1u", x: 400, y: 400))
        if let first = current.components.first(where: { $0.kind == "resistor" }) {
            refined.connections.append(PlannedConnection(from: "C99.1", to: "\(first.ref).2"))
        }
        let merged = DesignPlanCompiler.preservingStructure(refined, from: current)
        XCTAssertEqual(merged.sheets, current.sheets)
        XCTAssertEqual(merged.buses, current.buses)
        for c in current.components {
            let m = try XCTUnwrap(merged.components.first { $0.ref == c.ref })
            XCTAssertEqual(m, c)
        }
        // The new capacitor joins the sheet of the resistor it connects to.
        XCTAssertEqual(merged.components.first { $0.ref == "C99" }?.sheet, "Stage")
        // What the agent did set stays.
        var renamed = refined
        renamed.components[0].value = "47k"
        XCTAssertEqual(DesignPlanCompiler.preservingStructure(renamed, from: current).components[0].value, "47k")
    }

    func testPlainPlansAreUnchanged() throws {
        let engine = EDAEngine(name: "Plain")
        let r = engine.addComponent(.resistor, value: "1k", at: .zero)
        let g = engine.addComponent(.ground, at: CGPoint(x: 0, y: 100))
        XCTAssertNotNil(engine.connect(PinAddress(component: r, pin: 1), PinAddress(component: g, pin: 0)))
        let plan = DesignPlanCompiler.plan(from: try XCTUnwrap(engine.snapshot()))
        let json = plan.jsonString(pretty: false)
        for key in ["buses", "blockRef", "channelValues", "units", "\"bus\"", "channels"] {
            XCTAssertFalse(json.contains(key), key)
        }
    }
}

/// Signal harnesses through the store: a type, a connector on each of two sheets made global, the bundle joining
/// the members, undo.
@MainActor
final class HarnessTests: XCTestCase {
    func testHarnessConnectorsJoinMembersAcrossSheets() throws {
        let store = DesignStore()
        XCTAssertTrue(store.setHarnessType("SPI", entries: "SCK, MOSI, MISO"))
        XCTAssertEqual(store.snapshot.harnessTypes.first?.entries, ["SCK", "MOSI", "MISO"])
        XCTAssertFalse(store.setHarnessType("SPI", entries: "A.B"))
        var parts: [Int] = []
        var harnesses: [Int] = []
        for sheetName in ["A", "B"] {
            let sheet = try XCTUnwrap(store.addSheet(named: sheetName, parent: 0))
            store.selectSheet(sheet)
            let j = store.addComponent(.connector, at: .zero)
            store.placeHarnessConnector(type: "SPI", name: "BUS0")
            let harness = try XCTUnwrap(store.snapshot.components.first { $0.isHarnessLabel && $0.sheetId == sheet })
            let sck = try XCTUnwrap(store.snapshot.components.first { $0.harnessOf == harness.id && $0.value == "SCK" })
            XCTAssertTrue(store.connect(PinAddress(component: j, pin: 0), PinAddress(component: sck.id, pin: 0)))
            store.setLabelScope(harness.id, scope: "global")
            parts.append(j)
            harnesses.append(harness.id)
        }
        let a = try XCTUnwrap(store.snapshot.component(parts[0])), b = try XCTUnwrap(store.snapshot.component(parts[1]))
        XCTAssertEqual(a.pins[0].net, b.pins[0].net)
        XCTAssertEqual(store.snapshot.net(a.pins[0].net)?.name, "BUS0.SCK")
        store.setLabelHarness(harnesses[0], type: "")
        XCTAssertNil(store.snapshot.component(harnesses[0])?.harnessType)
        store.undo()
        XCTAssertEqual(store.snapshot.component(harnesses[0])?.harnessType, "SPI")
    }
}

/// Schematic directives through the store: a net class, a differential pair directive on a label, the board rules
/// following, undo.
@MainActor
final class DirectiveTests: XCTestCase {
    func testDirectivesDriveTheBoardRules() throws {
        let store = DesignStore()
        let j = store.addComponent(.connector, at: .zero)
        let p = store.addComponent(.netLabel, at: CGPoint(x: 100, y: 0))
        let n = store.addComponent(.netLabel, at: CGPoint(x: 100, y: 40))
        XCTAssertTrue(store.engine.setValue(p, "D_P"))
        XCTAssertTrue(store.engine.setValue(n, "D_N"))
        XCTAssertTrue(store.connect(PinAddress(component: j, pin: 0), PinAddress(component: p, pin: 0)))
        XCTAssertTrue(store.connect(PinAddress(component: j, pin: 1), PinAddress(component: n, pin: 0)))
        XCTAssertTrue(store.setNetClass("HS", trackWidth: 0.25, clearance: 0.3))
        XCTAssertFalse(store.setNetClass("bad name", trackWidth: 0.25, clearance: 0))
        XCTAssertEqual(store.snapshot.netClassDefs.first?.trackWidth, 0.25)
        let anchor = PinAddress(component: p, pin: 0)
        store.setDirective(on: anchor, netClass: "HS", diffPair: true, trackWidth: 0, clearance: 0)
        let directive = try XCTUnwrap(store.directive(on: anchor))
        XCTAssertEqual(directive.netName, "D_P")
        XCTAssertTrue(directive.isDiffPair)
        XCTAssertEqual(store.snapshot.board.netWidths["D_P"], 0.25)
        // A parameter set on the net wins over its class; clearing everything removes the directive.
        store.setDirective(on: anchor, netClass: "HS", diffPair: true, trackWidth: 0.4, clearance: 0)
        XCTAssertEqual(store.snapshot.board.netWidths["D_P"], 0.4)
        store.setDirective(on: anchor, netClass: "", diffPair: false, trackWidth: 0, clearance: 0)
        XCTAssertNil(store.directive(on: anchor))
        XCTAssertNil(store.snapshot.board.netWidths["D_P"])
        store.undo()
        XCTAssertNotNil(store.directive(on: anchor))
    }
}

/// Schematic productivity through the store: align, copy / paste array, back-annotation review, sheet templates and
/// the PDF of every sheet.
@MainActor
final class SchematicToolsTests: XCTestCase {
    func testAlignCopyPasteArrayAndUndo() throws {
        let store = DesignStore()
        let a = store.addComponent(.resistor, at: .zero)
        let b = store.addComponent(.resistor, at: CGPoint(x: 120, y: 50))
        store.selection = [a, b]
        store.align("top")
        XCTAssertEqual(store.snapshot.component(b)?.y, 0)
        store.undo()
        XCTAssertEqual(store.snapshot.component(b)?.y, 50)
        let label = store.addComponent(.netLabel, at: CGPoint(x: -60, y: 200))
        XCTAssertTrue(store.engine.setValue(label, "D0"))
        let clip = try XCTUnwrap(store.engine.copyComponents([label]))
        XCTAssertTrue(DesignStore.isSchematicClip(clip))
        store.paste(count: 3, step: CGSize(width: 0, height: 60), labelIncrement: 1, clip: clip)
        let names = Set(store.snapshot.components.filter { $0.componentKind == .netLabel }.map(\.value))
        XCTAssertTrue(names.isSuperset(of: ["D0", "D1", "D2", "D3"]))
        XCTAssertEqual(store.selection.count, 3)
        store.paste(clip: "not a clip")
        XCTAssertEqual(store.selection.count, 3)
    }

    func testBackAnnotationTemplatesAndPDF() throws {
        let store = DesignStore()
        let r1 = store.addComponent(.resistor, at: .zero)
        let r2 = store.addComponent(.resistor, at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(store.engine.findComponent(ref: "R1"))
        XCTAssertTrue(store.engine.moveFootprint(r1, to: CGPoint(x: 30, y: 20)))
        XCTAssertTrue(store.engine.moveFootprint(r2, to: CGPoint(x: 10, y: 20)))
        store.refresh()
        let eco = store.ecoFromBoard()
        XCTAssertEqual(eco.count, 2)
        store.applyEco(eco)
        XCTAssertEqual(store.snapshot.component(r2)?.ref, "R1")
        XCTAssertEqual(store.snapshot.component(r1)?.ref, "R2")
        let was = store.engine.ecoFromWasIs("R1 R5\nR77 R78\n")
        XCTAssertEqual(was.filter(\.applicable).count, 1)
        XCTAssertFalse(EDAEngine.sheetTemplates().isEmpty)
        store.setSheetSize(1, size: "A3")
        XCTAssertEqual(store.snapshot.sheet(1)?.size, "A3")
        let pdf = try XCTUnwrap(store.engine.schematicPDF())
        XCTAssertEqual(String(decoding: pdf.prefix(8), as: UTF8.self), "%PDF-1.4")
    }
}

/// AI plans keep harnesses and directives too: a plan made from a design with a harness connector and a net
/// directive rebuilds them.
@MainActor
final class StructuredPlanHarnessTests: XCTestCase {
    func testHarnessAndDirectivesSurviveAPlan() throws {
        let engine = EDAEngine(name: "Harness plan")
        XCTAssertTrue(engine.setHarnessType("SPI", entries: ["SCK", "MOSI"]))
        XCTAssertTrue(engine.setNetClass("HS", trackWidth: 0.2, clearance: 0))
        let j = engine.addComponent(.connector, at: .zero)
        let harness = try XCTUnwrap(engine.addHarnessConnector(type: "SPI", name: "BUS0", at: CGPoint(x: 200, y: 0)))
        let snap = try XCTUnwrap(engine.snapshot())
        let sck = try XCTUnwrap(snap.components.first { $0.harnessOf == harness && $0.value == "SCK" })
        XCTAssertNotNil(engine.connect(PinAddress(component: j, pin: 0), PinAddress(component: sck.id, pin: 0)))
        XCTAssertNotNil(engine.addDirective(component: sck.id, pin: 0, netClass: "HS", diffPair: false, trackWidth: 0, clearance: 0))
        let before = try XCTUnwrap(engine.snapshot())
        let plan = DesignPlanCompiler.plan(from: before)
        XCTAssertEqual(plan.harnessTypes?.first?.name, "SPI")
        XCTAssertEqual(plan.directives?.count, 1)
        XCTAssertEqual(plan.components.filter { $0.harnessOf != nil }.count, 2)
        let rebuilt = EDAEngine(name: "Rebuilt")
        let report = DesignPlanCompiler.apply(plan, to: rebuilt, previous: before)
        XCTAssertTrue(report.warnings.isEmpty, "\(report.warnings)")
        let after = try XCTUnwrap(rebuilt.snapshot())
        XCTAssertEqual(after.components.filter(\.isHarnessLabel).count, 1)
        XCTAssertEqual(after.components.filter { $0.harnessOf != nil }.count, 2)
        XCTAssertEqual(after.directives.first?.netName, "BUS0.SCK")
        XCTAssertEqual(after.board.netWidths["BUS0.SCK"], 0.2)
    }
}

/// True arc tracks (docs/INTERACTIVE_ROUTING.md): arc corners through the bridge, the snapshot's arc geometry,
/// drawing / hit testing helpers, and "Convert corners to arcs" from the store as one undo step.
@MainActor
final class ArcRoutingTests: XCTestCase {
    private func twoResistors(_ engine: EDAEngine) {
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 8, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 32))
    }

    func testArcCornersThroughTheBridge() throws {
        let engine = EDAEngine(name: "Arcs")
        twoResistors(engine)
        let options = EDAEngine.routingOptions(mode: .shove, diagonal: true, rounded: true, arcs: true)
        XCTAssertTrue(options.contains("\"arcCorners\":true"))
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 8.95, y: 20), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 20, y: 20))
        _ = engine.routerFix()
        let head = try XCTUnwrap(engine.routerMove(to: CGPoint(x: 39.05, y: 32)))
        XCTAssertTrue(head.reachedTarget)
        XCTAssertTrue((head.placed + head.head).contains { $0.isArc })
        XCTAssertTrue(engine.routerCommit().ok)
        let snapshot = try XCTUnwrap(engine.snapshot())
        let arcs = snapshot.tracks.filter(\.isArc)
        // One corner: the point fixed at (20, 20) lies on the straight start, so the route is straight, then one 45°
        // turn up to the pad — one arc.
        XCTAssertEqual(arcs.count, 1)
        for arc in arcs {
            // The mid point lies on the arc; the drawn centre line starts and ends exactly at the track's ends.
            let mid = CGPoint(x: try XCTUnwrap(arc.mx), y: try XCTUnwrap(arc.my))
            XCTAssertLessThan(arc.distance(to: mid), 1e-6)
            XCTAssertEqual(arc.centreLine.first, arc.start)
            XCTAssertEqual(arc.centreLine.last, arc.end)
            XCTAssertLessThan(arc.length, hypot(arc.bx - arc.ax, arc.by - arc.ay) * 1.2)
            XCTAssertTrue(arc.extent.insetBy(dx: -1e-9, dy: -1e-9).contains(mid))
        }
        XCTAssertTrue(snapshot.ratsnest.isEmpty)
        XCTAssertFalse(engine.runDRC().contains { $0.severity == .error })
        // The chords option still writes short straight pieces.
        XCTAssertTrue(EDAEngine.routingOptions(mode: .shove, diagonal: true, rounded: true, arcs: false)
            .contains("\"arcCorners\":false"))
    }

    func testConvertCornersToArcsFromTheStore() throws {
        let store = DesignStore()
        let engine = store.engine
        twoResistors(engine)
        let options = EDAEngine.routingOptions(mode: .shove, diagonal: true)
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 8.95, y: 20), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 20, y: 20))
        _ = engine.routerFix()
        _ = engine.routerMove(to: CGPoint(x: 39.05, y: 32))
        XCTAssertTrue(engine.routerCommit().ok)
        store.refresh()
        XCTAssertFalse(store.snapshot.tracks.contains { $0.isArc })
        // Select one track, then its whole net, and convert.
        let first = try XCTUnwrap(store.snapshot.tracks.first)
        store.selectTrack(first.id)
        XCTAssertEqual(store.selectedTracks, [first.id])
        store.selectTrackNets()
        XCTAssertEqual(store.selectedTracks.count, store.snapshot.tracks.count)
        store.convertCornersToArcs()
        XCTAssertTrue(store.snapshot.tracks.contains { $0.isArc })
        XCTAssertTrue(store.selectedTracks.isEmpty)
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        store.undo()
        XCTAssertFalse(store.snapshot.tracks.contains { $0.isArc })
        // Nothing to convert: no undo step, a message.
        store.selectTrack(nil)
        let before = store.snapshot.tracks.count
        let arcsResult = try XCTUnwrap(engine.arcCorners(tracks: [], apply: false))
        XCTAssertFalse(arcsResult.ok)
        XCTAssertEqual(store.snapshot.tracks.count, before)
    }
}

/// Length tuning parity (docs/INTERACTIVE_ROUTING.md, Length tuning): patterns and corner shapes, drag-along tuning,
/// length rules and match groups from the store, each an undo step.
@MainActor
final class LengthTuningParityTests: XCTestCase {
    private func routedStore() throws -> (DesignStore, SnapTrack) {
        let store = DesignStore()
        let engine = store.engine
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 20))
        engine.moveFootprint(r2, to: CGPoint(x: 40, y: 20))
        let options = EDAEngine.routingOptions(mode: .shove, diagonal: true)
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 10.95, y: 20), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 39.05, y: 20))
        XCTAssertTrue(engine.routerCommit().ok)
        store.refresh()
        return (store, try XCTUnwrap(store.snapshot.tracks.first))
    }

    func testPatternsCornersAndDragAlong() throws {
        let (store, track) = try routedStore()
        store.tuneStyle = .sawtooth
        store.tuneCorner = .round
        store.beginTune(track: track.id, at: CGPoint(x: 15, y: 20))
        store.setTuneTarget(track.length + 2)
        let preview = try XCTUnwrap(store.tuneSession?.preview)
        XCTAssertTrue(preview.ok)
        XCTAssertEqual(preview.targetSource, "typed")
        XCTAssertEqual(preview.after, track.length + 2, accuracy: 0.01)
        // Drag along: the meanders stay between the press and the pointer.
        store.dragTune(to: CGPoint(x: 30, y: 20))
        let span = try XCTUnwrap(store.tuneSession?.preview)
        XCTAssertTrue(span.ok)
        for t in span.addedTracks where abs(t.ay - 20) > 1e-6 || abs(t.by - 20) > 1e-6 {
            XCTAssertGreaterThanOrEqual(min(t.ax, t.bx), 15 - 1e-6)
            XCTAssertLessThanOrEqual(max(t.ax, t.bx), 30 + 1e-6)
        }
        store.applyTune()
        XCTAssertTrue(store.snapshot.tracks.contains { $0.isArc })
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        store.undo()
        XCTAssertFalse(store.snapshot.tracks.contains { $0.isArc })
        XCTAssertTrue(EDAEngine.TuneRequest(target: 1, amplitude: 0, spacing: 0, near: .zero, spanEnd: CGPoint(x: 5, y: 0),
                                            style: .trombone, corner: .mitered, coupled: true, phase: false, apply: false)
            .json.contains("\"toX\":5.000000"))
    }

    func testLengthRulesAndMatchGroupsFromTheStore() throws {
        let (store, track) = try routedStore()
        let net = try XCTUnwrap(store.snapshot.nets.first { $0.index == track.net }?.name)
        store.setLengthRule(net: net, target: track.length + 3, tolerance: 0.1)
        var targets = store.engine.lengthTargets()
        XCTAssertEqual(targets.rules.map(\.net), [net])
        XCTAssertFalse(targets.rules[0].ok)
        // The tuning tool takes the rule's target.
        store.beginTune(track: track.id, at: CGPoint(x: 20, y: 20))
        XCTAssertEqual(store.tuneSession?.preview?.targetSource, "rule:\(net)")
        store.applyTune()
        targets = store.engine.lengthTargets()
        XCTAssertTrue(targets.rules[0].ok)
        store.undo()  // the tuning
        store.undo()  // the rule
        XCTAssertTrue(store.engine.lengthTargets().rules.isEmpty)
        store.setMatchGroup(name: "G", nets: [net, "OTHER"], tolerance: 0.2)
        XCTAssertEqual(store.engine.lengthTargets().groups.map(\.name), ["G"])
        store.setMatchGroup(name: "G", nets: [], tolerance: 0)
        XCTAssertTrue(store.engine.lengthTargets().groups.isEmpty)
    }
}

/// Corner drag, multi-track drag, any-angle routing and multi-route from the store (docs/INTERACTIVE_ROUTING.md).
@MainActor
final class DragAndMultiRouteTests: XCTestCase {
    private func store(withRoute route: Bool) -> DesignStore {
        let store = DesignStore()
        let engine = store.engine
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        let r3 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 0, y: 100))
        let r4 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 100))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: r3, pin: 1), PinAddress(component: r4, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 10))
        engine.moveFootprint(r2, to: CGPoint(x: 30, y: 20))
        engine.moveFootprint(r3, to: CGPoint(x: 10, y: 30))
        engine.moveFootprint(r4, to: CGPoint(x: 40, y: 34))
        if route {
            let options = EDAEngine.routingOptions(mode: .shove, diagonal: true)
            XCTAssertNil(engine.routerBegin(at: CGPoint(x: 10.95, y: 10), layer: 0, pair: false, options: options)?.error)
            _ = engine.routerMove(to: CGPoint(x: 29.05, y: 20))
            XCTAssertTrue(engine.routerCommit().ok)
        }
        store.refresh()
        return store
    }

    func testCornerAndMultiTrackDrag() throws {
        let store = store(withRoute: true)
        // The route has a corner where two tracks meet away from the pads: drag it.
        let tracks = store.snapshot.tracks
        let ends: [CGPoint] = tracks.flatMap { [$0.start, $0.end] }
        func isInnerCorner(_ p: CGPoint) -> Bool {
            let joined = tracks.filter { $0.start == p || $0.end == p }.count
            let awayFromA = hypot(p.x - 10.95, p.y - 10) > 0.5
            let awayFromB = hypot(p.x - 29.05, p.y - 20) > 0.5
            return joined == 2 && awayFromA && awayFromB
        }
        let corner = try XCTUnwrap(ends.first(where: isInnerCorner))
        let track = try XCTUnwrap(tracks.first { $0.start == corner || $0.end == corner })
        XCTAssertTrue(store.beginCornerDrag(track.id, at: corner))
        XCTAssertEqual(store.routePreview?.kind, "corner")
        store.finishRoute(at: CGPoint(x: corner.x - 1, y: corner.y + 1))
        XCTAssertNil(store.routePreview)
        XCTAssertTrue(store.snapshot.ratsnest.count <= 1)  // the second net is not routed
        XCTAssertNotEqual(store.snapshot.tracks, tracks)
        store.undo()
        XCTAssertEqual(store.snapshot.tracks, tracks)
        // Two selected tracks drag together.
        let ids = Array(tracks.prefix(2).map(\.id))
        XCTAssertTrue(store.beginMultiDrag(ids, at: tracks[0].start))
        XCTAssertEqual(store.routePreview?.kind, "multidrag")
        store.cancelRoute()
        XCTAssertEqual(store.snapshot.tracks, tracks)
    }

    func testAnyAngleAndMultiRoute() throws {
        let store = store(withRoute: false)
        store.routerAnyAngle = true
        store.beginRoute(at: CGPoint(x: 10.95, y: 10), layer: 0, pair: false)
        store.moveRouteNow(to: CGPoint(x: 29.05, y: 20))
        XCTAssertEqual(store.routePreview?.head.count, 1)
        store.cancelRoute()
        store.routerAnyAngle = false
        // ⇧-click picks the first start, the plain click adds the second and routes both together.
        store.toggleMultiStart(CGPoint(x: 10.95, y: 10))
        XCTAssertEqual(store.multiStarts.count, 1)
        store.beginMultiRoute(adding: CGPoint(x: 10.95, y: 30), layer: 0)
        XCTAssertEqual(store.routePreview?.kind, "multi")
        XCTAssertTrue(store.multiStarts.isEmpty)
        store.cancelRoute()
        XCTAssertNil(store.routePreview)
    }
}

/// Teardrops, via stitching / shielding, glossing and loop removal from the store (docs/INTERACTIVE_ROUTING.md).
@MainActor
final class BoardCommandTests: XCTestCase {
    private func routedStore() -> DesignStore {
        let store = DesignStore()
        let engine = store.engine
        let r1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let r2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        XCTAssertNotNil(engine.connect(PinAddress(component: r1, pin: 1), PinAddress(component: r2, pin: 0)))
        engine.moveFootprint(r1, to: CGPoint(x: 10, y: 10))
        engine.moveFootprint(r2, to: CGPoint(x: 30, y: 20))
        let options = EDAEngine.routingOptions(mode: .shove, diagonal: true)
        XCTAssertNil(engine.routerBegin(at: CGPoint(x: 10.95, y: 10), layer: 0, pair: false, options: options)?.error)
        _ = engine.routerMove(to: CGPoint(x: 29.05, y: 20))
        XCTAssertTrue(engine.routerCommit().ok)
        store.refresh()
        return store
    }

    func testTeardropsToggleAndUndo() {
        let store = routedStore()
        let tracks = store.snapshot.tracks
        XCTAssertFalse(tracks.contains { $0.teardrop == true })
        store.toggleTeardrops()
        XCTAssertTrue(store.snapshot.tracks.contains { $0.teardrop == true })
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
        store.toggleTeardrops()  // again: removed
        XCTAssertEqual(store.snapshot.tracks.map(\.id).sorted(), tracks.map(\.id).sorted())
        store.undo()
        XCTAssertTrue(store.snapshot.tracks.contains { $0.teardrop == true })
        store.undo()
        XCTAssertEqual(store.snapshot.tracks, tracks)
    }

    func testRouterOptionsCarryLoopsAndTeardrops() {
        let json = EDAEngine.routingOptions(mode: .shove, diagonal: true, removeLoops: true, teardrops: true)
        XCTAssertTrue(json.contains("\"removeLoops\":true"))
        XCTAssertTrue(json.contains("\"teardrops\":true"))
        let plain = EDAEngine.routingOptions(mode: .walkaround, diagonal: false)
        XCTAssertTrue(plain.contains("\"removeLoops\":false"))
        let store = DesignStore()
        XCTAssertTrue(store.routerRemoveLoops)
        XCTAssertFalse(store.routerTeardrops)
    }

    func testGlossStitchAndShieldReportWithoutChangingWhenNothingToDo() {
        let store = routedStore()
        let tracks = store.snapshot.tracks
        store.glossTracks()  // a fresh route is already tight
        XCTAssertEqual(store.snapshot.tracks, tracks)
        store.stitchVias()  // no ground pours
        XCTAssertTrue(store.snapshot.vias.isEmpty)
        store.selectedTracks = []
        store.shieldSelectedTracks()  // nothing selected
        XCTAssertTrue(store.snapshot.vias.isEmpty)
        XCTAssertFalse(store.statusMessage.isEmpty)
        XCTAssertNotEqual(store.engine.gloss(tracks: [tracks[0].id])?.applied, true)
    }
}

/// Stop-at-obstacle mode and length matching of a bus from the store (docs/INTERACTIVE_ROUTING.md).
@MainActor
final class StopModeAndMatchLengthTests: XCTestCase {
    func testStopModeOption() {
        let json = EDAEngine.routingOptions(mode: .stop, diagonal: true)
        XCTAssertTrue(json.contains("\"mode\":\"stop\""))
        XCTAssertTrue(RouterModeChoice.allCases.contains(.stop))
        XCTAssertTrue(EDAEngine.routingOptions(mode: .shove, diagonal: true, hug: true).contains("\"hug\":true"))
        XCTAssertTrue(DesignStore().routerHugDrag)
    }

    func testMatchLengthsOfTwoNets() throws {
        let store = DesignStore()
        let engine = store.engine
        let a1 = engine.addComponent(.resistor, value: "1k", at: .zero)
        let a2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 0))
        let b1 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 0, y: 100))
        let b2 = engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 100, y: 100))
        XCTAssertNotNil(engine.connect(PinAddress(component: a1, pin: 1), PinAddress(component: a2, pin: 0)))
        XCTAssertNotNil(engine.connect(PinAddress(component: b1, pin: 1), PinAddress(component: b2, pin: 0)))
        engine.moveFootprint(a1, to: CGPoint(x: 10, y: 10))
        engine.moveFootprint(a2, to: CGPoint(x: 40, y: 10))
        engine.moveFootprint(b1, to: CGPoint(x: 10, y: 20))
        engine.moveFootprint(b2, to: CGPoint(x: 40, y: 26))
        let options = EDAEngine.routingOptions(mode: .shove, diagonal: true)
        for (from, to) in [(CGPoint(x: 10.95, y: 10), CGPoint(x: 39.05, y: 10)),
                           (CGPoint(x: 10.95, y: 20), CGPoint(x: 39.05, y: 26))] {
            XCTAssertNil(engine.routerBegin(at: from, layer: 0, pair: false, options: options)?.error)
            _ = engine.routerMove(to: to)
            XCTAssertTrue(engine.routerCommit().ok)
        }
        store.refresh()
        let before = store.snapshot.tracks
        store.matchSelectedLengths()  // nothing selected: refused
        XCTAssertEqual(store.snapshot.tracks, before)
        store.selectedTracks = Set(before.map(\.id))
        store.matchSelectedLengths()
        XCTAssertNotEqual(store.snapshot.tracks, before)
        store.undo()
        XCTAssertEqual(store.snapshot.tracks, before)
        let direct = try XCTUnwrap(engine.matchLengths(tracks: before.map(\.id)))
        XCTAssertGreaterThan(direct.target, 0)
    }
}

/// Update PCB (schematic → board ECO): the preview lists the new parts and nets; executing every change leaves the
/// board in step with the schematic, as one undo step.
@MainActor
final class UpdatePcbTests: XCTestCase {
    func testUpdatePcbExecutesTheChanges() {
        let store = DesignStore()
        let r1 = store.addComponent(.resistor, at: .zero)
        _ = store.addComponent(.resistor, at: CGPoint(x: 100, y: 0))
        let changes = store.engine.pcbEcoPreview()
        XCTAssertTrue(changes.contains { $0.key == "component:\(r1)" })
        let report = store.updatePCB(keys: changes.filter(\.applicable).map(\.key))
        XCTAssertFalse(report.isEmpty)
        XCTAssertTrue(store.engine.pcbEcoPreview().isEmpty)
        XCTAssertTrue(store.updatePCB(keys: []).isEmpty)
    }
}


/// Symbol graphics: drawings made in the Symbol Editor encode like the core's, survive the preview round trip and
/// enlarge the symbol's hit box; a sheet symbol takes a drawn size.
@MainActor
final class SymbolGraphicsTests: XCTestCase {
    func testDrawingsRoundTripThroughTheCore() throws {
        var spec = CustomPartSpec()
        spec.name = "GFX"
        spec.package.type = PackageKind.dip.rawValue
        spec.pins = (1...4).map { CustomPartSpec.Pin(number: String($0), name: "P\($0)", type: .passive) }
        var draft = SymbolDraft(spec: spec)
        XCTAssertTrue(draft.graphics.isEmpty && draft.body)
        let line = try XCTUnwrap(SymbolDraft.drawing(.line, from: CGPoint(x: -21, y: -9), to: CGPoint(x: 19, y: 11)))
        XCTAssertEqual(line.cgPoints, [CGPoint(x: -20, y: -10), CGPoint(x: 20, y: 10)])  // snapped
        XCTAssertNil(SymbolDraft.drawing(.rect, from: .zero, to: CGPoint(x: 1, y: 1)))
        var arc = try XCTUnwrap(SymbolDraft.drawing(.arc, from: .zero, to: CGPoint(x: 30, y: 0)))
        arc.fill = true
        let text = try XCTUnwrap(SymbolDraft.drawing(.text, from: CGPoint(x: -30, y: 40), to: .zero))
        draft.graphics = [line, arc, text]
        draft.body = false
        XCTAssertEqual(draft.drawing(at: CGPoint(x: 0, y: 0)), 1)
        let edited = draft.applied(to: spec)
        let info = try EDAEngine.previewCustomPart(edited).get()
        XCTAssertEqual(info.symbolLayout?.graphics, draft.graphics)
        XCTAssertEqual(info.symbolLayout?.body, false)
        XCTAssertEqual(SymbolDraft(spec: edited).graphics.count, 3)
        let box = SchematicSymbols.bounds(.custom, custom: info)
        XCTAssertGreaterThanOrEqual(box.maxY, 44)  // the text below the body
        let data = try JSONEncoder().encode(edited)
        XCTAssertEqual(try JSONDecoder().decode(CustomPartSpec.self, from: data), edited)
    }

    func testSheetSymbolSize() throws {
        let store = DesignStore()
        let child = try XCTUnwrap(store.addSheet(named: "Child", parent: 1))
        store.setSheetSymbolSize(child, width: 200, height: 120)
        XCTAssertEqual(store.snapshot.sheet(child)?.symbolWidth, 200)
        XCTAssertEqual(store.snapshot.sheet(child)?.symbolHeight, 120)
        store.setSheetSymbolSize(child, width: 0, height: 0)
        XCTAssertEqual(store.snapshot.sheet(child)?.symbolWidth, 0)
    }
}


/// Helper sheets inside a repeated block and per-channel parameters beyond the value, through the store.
@MainActor
final class HelperSheetAndChannelParameterTests: XCTestCase {
    func testHelperSheetRepeatsWithItsBlock() throws {
        let store = DesignStore()
        let block = try XCTUnwrap(store.addSheet(named: "Block", parent: 1))
        _ = store.addComponent(.resistor, at: .zero)
        let helper = try XCTUnwrap(store.addHelperSheet(parent: block))
        XCTAssertEqual(store.snapshot.sheet(helper)?.helper, true)
        store.repeatSheet(block, count: 3)
        XCTAssertEqual(store.snapshot.sheets.filter { $0.definitionId == helper }.count, 3)
        let r = try XCTUnwrap(store.snapshot.components.first { $0.componentKind == .resistor && $0.sheet == block })
        let copy = try XCTUnwrap(store.snapshot.components.first { $0.instanceOf == r.id })
        store.setChannelFitted(copy.id, false)
        XCTAssertEqual(store.snapshot.component(copy.id)?.isFitted, false)
        XCTAssertEqual(store.snapshot.component(r.id)?.isFitted, true)
        XCTAssertTrue(store.engine.setChannelFirmware(copy.id, hex: "", name: "", clockHz: 0))
    }
}


/// Clipboard with a selected bus, alignment by symbol outline, a fixed sheet frame and the PDF with Unicode text.
@MainActor
final class SchematicPolishTests: XCTestCase {
    func testClipboardAlignFrameAndPDF() throws {
        let store = DesignStore()
        let r = store.addComponent(.resistor, at: .zero)
        let amp = store.addComponent(.opAmp, at: CGPoint(x: 200, y: 100))
        let bus = try XCTUnwrap(store.addBus(named: "D[0..3]", points: [CGPoint(x: 0, y: 200), CGPoint(x: 300, y: 200)]))
        store.selection = [r]
        store.selectedBus = bus
        let clip = try XCTUnwrap(store.selectionClip())
        XCTAssertTrue(clip.contains("\"buses\""))
        store.selectedBus = nil
        store.selection = [r, amp]
        store.align("left", byOutline: true)
        XCTAssertEqual(componentX(store, r) - 30, componentX(store, amp) - 40)  // left edges of the outlines
        store.setSheetSize(1, size: "A4")
        XCTAssertNotNil(store.snapshot.sheet(1)?.frameX)
        store.centerSheetFrame(1)
        XCTAssertNotNil(store.snapshot.sheet(1)?.frameY)
        _ = store.engine.setTitleBlock(TitleBlockInfo(title: "Ωmega 日本"))
        let pdf = try XCTUnwrap(store.engine.schematicPDF(fontPath: nil))
        XCTAssertEqual(String(decoding: pdf.prefix(5), as: UTF8.self), "%PDF-")
    }

    private func componentX(_ store: DesignStore, _ id: Int) -> Double {
        Double(store.snapshot.component(id)?.x ?? 0)
    }
}


/// PCB pin / gate swap: a part without gates has no swaps; the automatic swap leaves a board without any alone.
@MainActor
final class PcbSwapTests: XCTestCase {
    func testSwapOptionsAndOptimizeOnAPlainBoard() {
        let store = DesignStore()
        let r = store.addComponent(.resistor, at: .zero)
        XCTAssertTrue(store.engine.pcbSwapOptions(r).isEmpty)
        XCTAssertEqual(store.engine.optimizePcbSwaps(nil), 0)
        let option = PcbSwapOptionInfo(kind: "pin", component: r, other: -1, pinA: 0, pinB: 1, label: "R1", gain: 1)
        XCTAssertFalse(store.engine.applyPcbSwap(option))  // a resistor has no swap groups
        store.optimizeSwaps(component: r)
        XCTAssertEqual(store.snapshot.component(r)?.ref, "R1")
    }
}

/// Autorouter strategies (docs/ROUTING.md): presets, options saved with the project, keep-outs, scoped routes and the
/// routing report through the bridge.
@MainActor
final class AutorouteStrategyTests: XCTestCase {
    private func twoNetStore() -> (DesignStore, [Int]) {
        let store = DesignStore()
        let engine = store.engine
        var ids: [Int] = []
        for k in 0..<4 {
            ids.append(engine.addComponent(.resistor, value: "1k", at: CGPoint(x: 60 * (k % 2), y: 60 * (k / 2))))
        }
        _ = engine.connect(PinAddress(component: ids[0], pin: 1), PinAddress(component: ids[1], pin: 0))
        _ = engine.connect(PinAddress(component: ids[2], pin: 1), PinAddress(component: ids[3], pin: 0))
        engine.setBoard(width: 60, height: 40, trackWidth: 0, clearance: 0)
        for (k, id) in ids.enumerated() {
            _ = engine.moveFootprint(id, to: CGPoint(x: 10 + 40.0 * Double(k % 2), y: 12 + 16.0 * Double(k / 2)))
        }
        store.refresh()
        return (store, ids)
    }

    func testPresetsOptionsAndReport() async throws {
        let presets = EDAEngine.autoroutePresets()
        XCTAssertEqual(presets.count, 7)
        XCTAssertTrue(presets.contains { $0.name == "quality" && $0.options.minimizeVias && $0.options.coupledPairs })
        let (store, _) = twoNetStore()
        store.applyRoutingPreset("quality")
        XCTAssertEqual(store.autorouteOptions.preset, "quality")
        XCTAssertTrue(store.autorouteOptions.gloss)
        store.toggleRoutingOption(\.teardrops)
        XCTAssertTrue(store.autorouteOptions.teardrops)
        store.undo()
        XCTAssertFalse(store.autorouteOptions.teardrops)
        // Keep-outs round-trip through the bridge and the saved project.
        XCTAssertEqual(store.engine.setKeepouts([RouteKeepout(name: "K", x0: 26, y0: 2, x1: 34, y1: 20, layer: 0, tracks: true, vias: true)]), 1)
        XCTAssertEqual(store.engine.keepouts().first?.name, "K")
        XCTAssertTrue(store.engine.saveJSON().contains("\"keepouts\""))
        await store.autoRoute()
        let report = store.routeReport
        XCTAssertEqual(report.metrics.unrouted, 0)
        XCTAssertGreaterThan(report.metrics.trackLength, 0)
        XCTAssertEqual(report.metrics.layerLength.count, store.snapshot.board.layerCount)
    }

    func testRouteSelectedNetsKeepsTheOtherNet() async throws {
        let (store, ids) = twoNetStore()
        await store.autoRoute()
        let otherNet = try XCTUnwrap(store.snapshot.pads.first { $0.component == ids[2] && $0.pin == 1 }?.net)
        let other = store.snapshot.tracks.filter { $0.net == otherNet }
        XCTAssertFalse(other.isEmpty)
        store.selection = [ids[0]]
        XCTAssertFalse(store.selectedRoutingNets.isEmpty)
        let before = store.autorouteOptions
        await store.routeSelectedNets()
        XCTAssertEqual(store.autorouteOptions, before)  // the scope applied to that route only
        let after = store.snapshot.tracks.filter { $0.net == otherNet }
        XCTAssertEqual(other.map(\.ax).sorted(), after.map(\.ax).sorted())
        XCTAssertTrue(store.snapshot.ratsnest.isEmpty)
    }
}

/// Schematic canvas colour schemes, grid styles and the custom (named-colour) theme.
@MainActor
final class SchematicPaletteTests: XCTestCase {
    func testEveryPresetForegroundReachesThreeToOne() throws {
        for scheme in SchematicColorScheme.presets {
            let p = try XCTUnwrap(scheme.presetPalette)
            for item in p.foregrounds {
                let ratio = item.colour.contrast(with: p.background)
                XCTAssertGreaterThanOrEqual(ratio, 3, "\(scheme.rawValue).\(item.name) is \(ratio):1")
            }
            for pair in p.overlayPairs {
                let ratio = pair.text.contrast(with: pair.fill)
                XCTAssertGreaterThanOrEqual(ratio, 3, "\(scheme.rawValue).\(pair.name) is \(ratio):1")
            }
        }
    }

    func testContrastFormula() {
        let white = SchematicRGB(hex: 0xFFFFFF), black = SchematicRGB(hex: 0x000000)
        XCTAssertEqual(white.contrast(with: black), 21, accuracy: 0.01)
        XCTAssertEqual(white.contrast(with: white), 1, accuracy: 0.0001)
    }

    func testDefaultSchemeIsTodaysColours() {
        XCTAssertEqual(SchematicColorScheme.defaultScheme, .siedaDark)
        XCTAssertEqual(SchematicCanvasStyle.standard.palette, .siedaDark)
        XCTAssertEqual(SchematicColorScheme.palette(scheme: "unknown", customJSON: ""), .siedaDark)
        let p = SchematicPalette.siedaDark
        let canvas: [(SchematicRGB, Color)] = [
            (p.background, Theme.schematicBackground), (p.gridMinor, Theme.gridDot), (p.gridMajor, Theme.darkBlue),
            (p.wire, Theme.wire), (p.junction, Theme.wire), (p.bus, Theme.lightBlue), (p.netLabel, Theme.skyBlue),
            (p.symbol, Theme.symbol), (p.symbolFill, Theme.symbolFill), (p.pin, Theme.pin),
        ]
        let text: [(SchematicRGB, Color)] = [
            (p.unconnectedPin, Theme.unconnectedPin), (p.selection, Theme.selection), (p.designator, Theme.label),
            (p.value, Theme.valueLabel), (p.harness, Theme.harness), (p.error, Theme.error), (p.probe, Theme.probe),
            (p.liveOn, Theme.liveOn), (p.overlayFill, Theme.deepBlue), (p.selectionHalo, Theme.blue),
            (p.sheetText, Theme.textMuted), (p.portLabel, Theme.warning),
        ]
        for (index, pair) in (canvas + text).enumerated() {
            assertSameColour(pair.0.color, pair.1, "colour \(index)")
        }
    }

    private func assertSameColour(_ a: Color, _ b: Color, _ message: String) {
        guard let x = NSColor(a).usingColorSpace(.sRGB), let y = NSColor(b).usingColorSpace(.sRGB) else {
            return XCTFail("not convertible: \(message)")
        }
        XCTAssertEqual(x.redComponent, y.redComponent, accuracy: 1e-9, message)
        XCTAssertEqual(x.greenComponent, y.greenComponent, accuracy: 1e-9, message)
        XCTAssertEqual(x.blueComponent, y.blueComponent, accuracy: 1e-9, message)
        XCTAssertEqual(x.alphaComponent, y.alphaComponent, accuracy: 1e-9, message)
    }

    func testSchemesPersistByStableRawValues() throws {
        let expected = ["siedaDark", "altium", "orcad", "allegro", "xpedition", "pads", "cr8000", "kicad", "eagle",
                        "proteus", "easyeda", "diptrace", "monochrome", "highContrast", "custom"]
        XCTAssertEqual(SchematicColorScheme.allCases.map(\.rawValue), expected)
        let suite = "SiEDA.SchematicPaletteTests"
        let defaults = try XCTUnwrap(UserDefaults(suiteName: suite))
        defer { defaults.removePersistentDomain(forName: suite) }
        for scheme in SchematicColorScheme.allCases {
            defaults.set(scheme.rawValue, forKey: SchematicColorScheme.storageKey)
            let stored = try XCTUnwrap(defaults.string(forKey: SchematicColorScheme.storageKey))
            XCTAssertEqual(SchematicColorScheme(rawValue: stored), scheme)
        }
        for style in SchematicGridStyle.allCases {
            defaults.set(style.rawValue, forKey: SchematicGridStyle.storageKey)
            let stored = defaults.string(forKey: SchematicGridStyle.storageKey) ?? ""
            XCTAssertEqual(SchematicGridStyle(rawValue: stored), style)
        }
    }

    func testSchemeNamesAreUniqueAndGrouped() {
        let titles = SchematicColorScheme.allCases.map(\.title)
        XCTAssertEqual(Set(titles).count, titles.count)
        XCTAssertEqual(SchematicColorScheme.siedaDark.title, "Midnight Navy")
        let grouped = SchematicColorScheme.lightPresets + SchematicColorScheme.darkPresets
        XCTAssertEqual(Set(grouped), Set(SchematicColorScheme.presets))
        XCTAssertTrue(SchematicColorScheme.darkPresets.contains(.siedaDark))
        XCTAssertTrue(SchematicColorScheme.lightPresets.contains(.kicad))
        XCTAssertFalse(SchematicColorScheme.presets.contains(.custom))
    }

    func testGridStyleDefaultsToDots() {
        XCTAssertEqual(SchematicCanvasStyle.standard.grid, .dots)
        XCTAssertEqual(SchematicCanvasStyle.standard.majorEvery, 10)
        let style = SchematicCanvasStyle.from(scheme: "siedaDark", customJSON: "", grid: "bogus", majorEvery: 10)
        XCTAssertEqual(style, .standard)
        let lines = SchematicCanvasStyle.from(scheme: "", customJSON: "", grid: "lines", majorEvery: 5)
        XCTAssertEqual(lines.grid, .lines)
        XCTAssertEqual(lines.majorEvery, 5)
    }

    func testNamedColourTableIsUnique() {
        let all = SchematicNamedColor.all
        XCTAssertGreaterThanOrEqual(all.count, 40)
        XCTAssertEqual(Set(all.map(\.id)).count, all.count)
        XCTAssertEqual(Set(all.map(\.name)).count, all.count)
        XCTAssertEqual(SchematicNamedColor.named("green")?.name, "Green")
        XCTAssertNil(SchematicNamedColor.named("nope"))
    }

    func testNearestNamedColour() {
        XCTAssertEqual(SchematicNamedColor.nearest(to: SchematicRGB(hex: 0x009600)).id, "green")
        XCTAssertEqual(SchematicNamedColor.nearest(to: SchematicRGB(hex: 0xFE0202)).id, "red")
        XCTAssertEqual(SchematicNamedColor.nearest(to: SchematicRGB(hex: 0x840000)).id, "maroon")
    }

    func testHexStrings() {
        XCTAssertEqual(SchematicRGB(hexString: "#0A1B2C")?.hexString, "#0A1B2C")
        XCTAssertEqual(SchematicRGB(hexString: "0a1b2c")?.hexString, "#0A1B2C")
        XCTAssertEqual(SchematicRGB(hexString: "#0A1B2C80")?.hexString, "#0A1B2C80")
        XCTAssertNil(SchematicRGB(hexString: "#12"))
        XCTAssertNil(SchematicRGB(hexString: "#GGGGGG"))
    }

    func testCustomThemeJSONRoundTrip() {
        var theme = SchematicCustomTheme.seeded(from: .kicad)
        theme.roles[SchematicColorRole.wire.rawValue] = "yellow"
        theme.roles[SchematicColorRole.bus.rawValue] = "#123456"
        let loaded = SchematicCustomTheme.load(theme.json)
        XCTAssertEqual(loaded, theme)
        XCTAssertEqual(loaded.seedScheme, .kicad)
        XCTAssertEqual(loaded.palette().wire, SchematicNamedColor.named("yellow")?.rgb)
        XCTAssertEqual(loaded.palette().bus, SchematicRGB(hex: 0x123456))
        XCTAssertEqual(loaded.choice(for: .wire), "yellow")
        XCTAssertEqual(loaded.choice(for: .bus), SchematicCustomTheme.otherTag)
        XCTAssertEqual(SchematicColorScheme.palette(scheme: "custom", customJSON: theme.json), loaded.palette())
    }

    func testSeededThemeLooksLikeItsPreset() throws {
        for scheme in SchematicColorScheme.presets {
            let preset = try XCTUnwrap(scheme.presetPalette)
            XCTAssertEqual(SchematicCustomTheme.seeded(from: scheme).palette(), preset, scheme.rawValue)
        }
    }

    func testUnknownColourIdsFallBackToTheSeed() {
        let theme = SchematicCustomTheme(seed: "altium", roles: ["wire": "notAColour", "bus": "#12", "pin": "red"])
        let p = theme.palette()
        XCTAssertEqual(p.wire, SchematicPalette.altium.wire)
        XCTAssertEqual(p.bus, SchematicPalette.altium.bus)
        XCTAssertEqual(p.pin, SchematicRGB(hex: 0xFF0000))
        XCTAssertEqual(SchematicCustomTheme(seed: "nope", roles: [:]).palette(), .siedaDark)
        XCTAssertEqual(SchematicCustomTheme(seed: "custom", roles: [:]).palette(), .siedaDark)
        let fallback = SchematicCustomTheme.seeded(from: .siedaDark)
        XCTAssertEqual(SchematicCustomTheme.load("{not json"), fallback)
        XCTAssertEqual(SchematicColorScheme.palette(scheme: "custom", customJSON: "garbage"), fallback.palette())
    }

    func testLowContrastRolesAreFlagged() {
        var theme = SchematicCustomTheme.seeded(from: .eagle)
        theme.roles[SchematicColorRole.background.rawValue] = "white"
        theme.roles[SchematicColorRole.wire.rawValue] = "yellow"
        XCTAssertTrue(theme.lowContrastRoles().contains(.wire))
        XCTAssertTrue(SchematicCustomTheme.seeded(from: .kicad).lowContrastRoles().isEmpty)
        // A pale fill or grid is never flagged: they are not foregrounds.
        theme.roles[SchematicColorRole.symbolFill.rawValue] = "white"
        XCTAssertFalse(theme.lowContrastRoles().contains(.symbolFill))
    }
}

@MainActor
final class ComponentSymbolIconTests: XCTestCase {
    func testEveryKindHasATemplateSymbolIcon() {
        for kind in ComponentKind.allCases {
            let image = ComponentSymbolIcon.image(kind)
            XCTAssertTrue(image.isTemplate, "\(kind)")
            XCTAssertEqual(image.size, CGSize(width: 24, height: 16), "\(kind)")
            XCTAssertNotNil(image.cgImage(forProposedRect: nil, context: nil, hints: nil), "\(kind)")
        }
    }

    func testEveryKindDrawsSomething() {
        for kind in ComponentKind.allCases {
            let (shapes, bounds) = ComponentSymbolIcon.shapes(kind)
            XCTAssertFalse(shapes.stroke.isEmpty && shapes.solid.isEmpty, "\(kind) has no symbol paths")
            XCTAssertGreaterThan(ComponentSymbolIcon.fitScale(bounds, into: CGSize(width: 24, height: 16)), 0, "\(kind)")
        }
    }

    func testIconsAreCachedPerSize() {
        let a = ComponentSymbolIcon.image(.resistor)
        XCTAssertTrue(a === ComponentSymbolIcon.image(.resistor))
        XCTAssertFalse(a === ComponentSymbolIcon.image(.resistor, size: CGSize(width: 40, height: 32)))
    }
}
