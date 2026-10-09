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

    /// The model's plan for a request, and its raw reply. Throws when the reply is not a usable plan.
    static func plan(for request: String, path: String) async throws -> (plan: DesignPlan, reply: String) {
        let reply = try await LocalLLM.run(path: path, system: system, user: request, maxTokens: 1000, json: false)
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

/// Super Intelligence → Own Model: SiEDA's circuit model in four tabs.
struct CircuitModelView: View {
    enum Tab: Hashable { case train, test, models, use }

    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var store: DesignStore
    @ObservedObject private var models = LocalModelStore.shared
    @State private var tab = Tab.train
    @State private var prompt = "Design a 5 V red LED indicator at 10 mA"
    @State private var reply = ""
    @State private var verdict = ""
    @State private var running = false
    @State private var bench: (valid: Int, exact: Int, total: Int)?
    @State private var exported = ""

    private let card = CircuitModel.card

    var body: some View {
        TabView(selection: $tab) {
            train.tabItem { Text("Train") }.tag(Tab.train)
            test.tabItem { Text("Test") }.tag(Tab.test)
            modelsList.tabItem { Text("Models") }.tag(Tab.models)
            use.tabItem { Text("Use") }.tag(Tab.use)
        }
    }

    private var train: some View {
        Form {
            if let card {
                LabeledContent("Training pairs") { Text(verbatim: "\(card.trainPairs) + \(card.testPairs) held out") }
                LabeledContent("Reference circuits") { Text(verbatim: "\(card.circuits.count)") }
                LabeledContent("Part kinds (active and passive)") { Text(verbatim: "\(card.kinds.count)") }
                LabeledContent("Held-out requests: usable / exact plans") {
                    Text(verbatim: String(format: "%.0f %% / %.0f %%", card.validPlans * 100, card.exactPlans * 100))
                }
            }
            Text("The model learns from SiEDA’s reference circuits and from circuits whose values are computed from the request (LED resistors, dividers, RC filters, amplifier gains, transistor drivers). Add your own designs to its training data, then retrain on a Mac or Linux machine with Python:")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            Text(verbatim: "python3 tools/circuit_lm/make_dataset.py data\npython3 tools/circuit_lm/train.py data out")
                .font(.system(.caption, design: .monospaced))
                .textSelection(.enabled)
            HStack {
                Button("Add This Design to the Training Data") { exportDesign() }
                    .disabled(store.snapshot.components.isEmpty)
                if !exported.isEmpty { Text(verbatim: exported).font(.caption).foregroundStyle(Theme.textMuted) }
            }
        }
        .formStyle(.grouped)
    }

    private var test: some View {
        Form {
            TextField("Request", text: $prompt)
            HStack {
                Button("Generate") { generate() }.disabled(running || prompt.isEmpty)
                Button("Run Benchmark") { runBenchmark() }.disabled(running || (card?.benchmark.isEmpty ?? true))
                if running { ProgressView().controlSize(.small) }
            }
            if !verdict.isEmpty { Text(verbatim: verdict) }
            if let bench {
                Text(verbatim: "\(bench.valid) / \(bench.total) usable plans · \(bench.exact) / \(bench.total) exactly the reference circuit")
            }
            if !reply.isEmpty {
                ScrollView { Text(verbatim: reply).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
                    .frame(maxHeight: 160)
            }
        }
        .formStyle(.grouped)
    }

    private var modelsList: some View {
        Form {
            ForEach(models.files.filter(CircuitModel.isCircuitModel), id: \.self) { file in
                LabeledContent(file) { Text(verbatim: sizeText(file)) }
            }
            if let card {
                LabeledContent(CircuitModel.file) {
                    Text(verbatim: String(format: "%.1f M parameters · bundled", Double(card.parameters) / 1e6))
                }
                Text(verbatim: card.circuits.joined(separator: " · ")).font(.caption).foregroundStyle(Theme.textMuted)
            }
        }
        .formStyle(.grouped)
    }

    private var use: some View {
        Form {
            Text("Make SiEDA’s own circuit model the AI that designs new circuits. It runs on this Mac, needs no account and no download, and answers in a few seconds; specifications and reviews come from the Offline Designer.")
                .foregroundStyle(Theme.textMuted)
            let active = settings.provider == .builtIn && CircuitModel.isCircuitModel(settings.model(for: .builtIn))
            Button {
                if let file = try? CircuitModel.install() {
                    settings.aiEnabled = true
                    settings.provider = .builtIn
                    settings.models[.builtIn] = file
                    models.refresh()
                }
            } label: {
                if active { Text("In Use") } else { Text("Use SiEDA Circuit Model") }
            }
            .disabled(active || CircuitModel.bundledURL == nil)
        }
        .formStyle(.grouped)
    }

    private func modelPath() throws -> String {
        LocalModelStore.folder.appendingPathComponent(try CircuitModel.install()).path
    }

    private func generate() {
        running = true
        Task { @MainActor in
            defer { running = false }
            let start = Date()
            do {
                let result = try await CircuitModel.plan(for: prompt, path: try modelPath())
                reply = result.reply
                verdict = "\(result.plan.title): \(result.plan.components.count) parts, \(result.plan.connections.count) links · "
                    + String(format: "%.1f s", Date().timeIntervalSince(start))
            } catch {
                verdict = error.localizedDescription
            }
        }
    }

    private func runBenchmark() {
        guard let cases = card?.benchmark else { return }
        running = true
        Task { @MainActor in
            defer { running = false }
            var valid = 0, exact = 0
            for c in cases {
                guard let result = try? await CircuitModel.plan(for: c.prompt, path: try modelPath()) else { continue }
                valid += 1
                if let expected = try? JSONExtraction.decode(DesignPlan.self, from: c.plan),
                   expected.components == result.plan.components, expected.connections == result.plan.connections { exact += 1 }
            }
            bench = (valid, exact, cases.count)
        }
    }

    /// Appends the open design as one training pair (request = the project's requirements or name).
    private func exportDesign() {
        let plan = DesignPlanCompiler.plan(from: store.snapshot)
        let request = store.snapshot.requirements.isEmpty ? store.snapshot.name : store.snapshot.requirements
        let folder = LocalModelStore.folder.deletingLastPathComponent().appendingPathComponent("Training", isDirectory: true)
        let file = folder.appendingPathComponent("my-designs.jsonl")
        guard let line = try? JSONSerialization.data(withJSONObject: ["prompt": request, "plan": plan.jsonString(pretty: false)]) else { return }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        if let handle = try? FileHandle(forWritingTo: file) {
            handle.seekToEndOfFile()
            handle.write(line + Data("\n".utf8))
            try? handle.close()
        } else {
            try? (line + Data("\n".utf8)).write(to: file)
        }
        exported = file.path
    }

    private func sizeText(_ file: String) -> String {
        let size = (try? FileManager.default.attributesOfItem(atPath: LocalModelStore.folder.appendingPathComponent(file).path)[.size]
                    as? NSNumber)?.int64Value ?? 0
        return ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
    }
}
