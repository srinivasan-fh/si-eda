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
