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
