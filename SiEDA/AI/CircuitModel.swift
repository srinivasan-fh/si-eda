import Foundation
import SwiftUI

/// SiEDA's own circuit model: a small transformer trained from scratch on SiEDA's reference circuits (active and
/// passive parts, their values and every pin-to-pin link) by tools/circuit_lm, shipped inside the app and run by the
/// built-in engine (docs/AI.md, "SiEDA's own circuit model").
enum CircuitModel {
    static let file = "sieda-circuit-v1.gguf"
    /// The system turn it was trained with (also what `sieda-cli --chat` sends).
    static let system = "You are an electronics design assistant."

    /// What tools/circuit_lm wrote beside the model: training data, results and a benchmark.
    struct Card: Codable {
        struct Case: Codable {
            var prompt: String
            var plan: String
        }
        var parameters: Int
        var trainPairs: Int
        var testPairs: Int
        var testLoss: Double
        var validPlans: Double
        var exactPlans: Double
        var modelExactPlans: Double?
        var circuits: [String]
        var kinds: [String]
        var benchmark: [Case]
    }

    static var bundledURL: URL? { Bundle.main.url(forResource: "sieda-circuit-v1", withExtension: "gguf") }
    static let card: Card? = Bundle.main.url(forResource: "sieda-circuit-v1", withExtension: "json")
        .flatMap { try? Data(contentsOf: $0) }.flatMap { try? JSONDecoder().decode(Card.self, from: $0) }

    static func isCircuitModel(_ file: String) -> Bool { file.hasPrefix("sieda-circuit") }

    /// Copies the bundled model into the models folder (once) and returns its file name.
    @discardableResult
    static func install() throws -> String {
        let target = LocalModelStore.folder.appendingPathComponent(file)
        if !FileManager.default.fileExists(atPath: target.path) {
            guard let source = bundledURL else { throw AIProviderError.invalidResponse("The circuit model is not in this build.") }
            try FileManager.default.createDirectory(at: LocalModelStore.folder, withIntermediateDirectories: true)
            try FileManager.default.copyItem(at: source, to: target)
        }
        return file
    }

    /// The model's plan for a request (with the values the request fixes computed by the core), and the reply. Throws
    /// when the reply is not a usable plan. The Offline Designer's keyword rule reads any wording ("a VFD with an IPM
    /// for a fan motor"); when it names a circuit the model knows, the model writes that circuit from its title.
    static func plan(for request: String, path: String) async throws -> (plan: DesignPlan, reply: String) {
        let text = " " + request.lowercased() + " "
        let routed = OfflineProvider.templates.reversed().first { $0.matches(text) }.map(\.plan.title)
        let prompt = routed.flatMap { title in card?.circuits.contains(title) == true ? "Design a \(title.lowercased())" : nil }
        let raw = try await LocalLLM.run(path: path, system: system, user: prompt ?? request, maxTokens: 2000, json: false)
        let reply = EDAEngine.take(sieda_circuit_plan_values(request, raw)) ?? raw
        let plan = try JSONExtraction.decode(DesignPlan.self, from: reply)
        if let problem = check(plan) { throw AIProviderError.invalidResponse("The circuit model's plan is not usable: \(problem)") }
        return (plan, reply)
    }

    /// nil when every link names a part in the plan and every part has a kind; else what is wrong.
    static func check(_ plan: DesignPlan) -> String? {
        guard !plan.components.isEmpty else { return "no parts" }
        let refs = Set(plan.components.map(\.ref))
        if refs.count != plan.components.count { return "repeated designators" }
        for c in plan.connections {
            for end in [c.from, c.to] where !refs.contains(String(end.split(separator: ".").first ?? "")) {
                return "link to unknown part \(end)"
            }
        }
        return nil
    }

    /// The same parts and links, in any order (links in either direction).
    static func sameCircuit(_ a: DesignPlan, _ b: DesignPlan) -> Bool {
        let links = { (p: DesignPlan) in p.connections.map { [$0.from, $0.to].sorted().joined(separator: "|") }.sorted() }
        return a.components.sorted { $0.ref < $1.ref } == b.components.sorted { $0.ref < $1.ref } && links(a) == links(b)
    }

    /// The agents' requests: a new design comes from the model; specifications, refinements and reviews (which a
    /// model this small was not trained for) from the Offline Designer.
    static func complete(_ request: AIRequest, path: String) async throws -> String {
        guard request.schemaName == "design_plan", OfflineProvider.section("current_plan", in: request.prompt) == nil else {
            return try await OfflineProvider().complete(request)
        }
        let brief = OfflineProvider.section("requirements", in: request.prompt) ?? request.prompt
        return try await plan(for: brief, path: path).plan.jsonString()
    }
}
