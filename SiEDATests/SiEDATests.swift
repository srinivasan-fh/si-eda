import SwiftUI
import XCTest
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

        XCTAssertEqual(StandardLibrary.rulePresets.count, 9)
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
        XCTAssertEqual(OfflineProvider.templates.count, 22)
        XCTAssertEqual(OfflineProvider.template(for: "non-inverting amplifier with gain 11").plan.title, "Non-Inverting Amplifier")
        XCTAssertEqual(OfflineProvider.template(for: "an inverting amplifier, gain -10").plan.title, "Inverting Amplifier")
        XCTAssertEqual(OfflineProvider.template(for: "blink an LED with a 555").plan.title, "555 Astable LED Blinker")
        XCTAssertEqual(OfflineProvider.template(for: "12 V to 5 V LM7805 regulator").plan.title, "5 V Linear Regulator")
        XCTAssertEqual(OfflineProvider.template(for: "strain gauge wheatstone bridge").plan.title, "Wheatstone Bridge")
        XCTAssertNotEqual(OfflineProvider.template(for: "h-bridge motor driver with mosfets").plan.title, "Wheatstone Bridge")
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
        XCTAssertEqual(ids, ["general", "robotics", "uav", "power", "automotive", "rf", "space", "marine", "industrial"])
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
        for stage in 0..<3 {
            if stage == 1 { progress.set("stage 1: loading example"); store.loadExample(OfflineProvider.templates[8].industryPlan) }
            if stage == 2 { progress.set("stage 2: auto-placing"); store.autoPlace(all: true) }
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
