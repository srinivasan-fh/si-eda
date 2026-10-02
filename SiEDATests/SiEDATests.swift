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
        XCTAssertEqual(OfflineProvider.templates.count, 27)
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
    func testReferenceDesignsPassVerification() throws {
        for template in OfflineProvider.templates {
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
            XCTAssertEqual(verification.stages.count, 7)
            XCTAssertEqual(verification.industry, template.industry)
            XCTAssertTrue(verification.markdown.contains("# Design Verification Report"))
        }
    }

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
                             "medical", "defence", "networking", "vlsi"])
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
        XCTAssertEqual(groups["Microcontrollers · Arm"]?.count, 10)
        XCTAssertEqual(groups["Microcontrollers · STMicroelectronics"]?.count, 10)
        XCTAssertEqual(groups["Microcontrollers · Texas Instruments"]?.count, 10)
        XCTAssertEqual(groups["Microcontrollers · Microchip"]?.count, 12)
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
