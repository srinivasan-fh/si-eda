import Foundation

/// Trains SiEDA's own circuit model from the app (Super Intelligence → Own LLM): exports the reference circuits and
/// the user's designs, builds the dataset and trains with the bundled tools/circuit_lm scripts on this Mac's Python,
/// follows the loss live (a checkpoint every 200 steps, so Stop and Resume lose nothing), then scores the trained model
/// with the built-in engine on the held-out requests and installs it (docs/AI.md, "SiEDA's own circuit model").
@MainActor
final class CircuitTrainer: ObservableObject {
    static let shared = CircuitTrainer()

    enum Phase: Equatable { case idle, checking, installing, building, training, evaluating }

    struct DatasetSummary: Equatable {
        var circuits: [String]
        var ownDesigns: Int
        var train: Int
        var test: Int
        var kinds: Int
        var samples: [Sample]
    }

    struct Sample: Identifiable, Equatable {
        let id: Int
        let prompt: String
        let circuit: String
    }

    struct LossPoint: Identifiable, Equatable {
        let step: Int
        let loss: Double
        var id: Int { step }
    }

    struct CircuitScore: Identifiable, Equatable {
        let circuit: String
        var total = 0
        var usable = 0
        var exact = 0
        var id: String { circuit }
    }

    struct Score: Equatable {
        var total = 0
        var usable = 0
        var exact = 0
        var seconds = 0.0
        var circuits: [CircuitScore] = []
        var misses: [String] = []
    }

    static let packages = ["torch", "tokenizers", "gguf", "numpy"]

    // Environment
    @Published private(set) var phase = Phase.idle
    @Published private(set) var python: String?
    @Published private(set) var versions: [String: String] = [:]  // package → version ("" = missing)
    @Published private(set) var log: [String] = []
    @Published private(set) var message: String?

    // Dataset settings and result
    @Published var phrasings = 100
    @Published var contexts = 30
    @Published var includeOwnDesigns = true
    @Published private(set) var dataset: DatasetSummary?
    @Published private(set) var ownDesigns: [(prompt: String, title: String)] = []

    // Training settings and progress
    @Published var epochs = 16.0
    @Published var size = "small"
    @Published var learningRate = 2e-3
    @Published private(set) var losses: [LossPoint] = []
    @Published private(set) var step = 0
    @Published private(set) var totalSteps = 0
    @Published private(set) var elapsed = 0.0
    @Published private(set) var testLoss: Double?

    // Results
    @Published private(set) var score: Score?
    @Published private(set) var scoredModel = ""
    @Published private(set) var evaluated = 0
    @Published private(set) var evaluationTotal = 0

    private var process: Process?
    private var evaluation: Task<Void, Never>?

    var folder: URL { LocalModelStore.folder.deletingLastPathComponent().appendingPathComponent("Training", isDirectory: true) }
    var dataFolder: URL { folder.appendingPathComponent("data", isDirectory: true) }
    var outFolder: URL { folder.appendingPathComponent("out", isDirectory: true) }
    var ownDesignsFile: URL { folder.appendingPathComponent("my-designs.jsonl") }
    var trainedModel: URL { outFolder.appendingPathComponent("sieda-circuit-v1-f32.gguf") }
    var hasTrainedModel: Bool { FileManager.default.fileExists(atPath: trainedModel.path) }
    var hasCheckpoint: Bool { FileManager.default.fileExists(atPath: outFolder.appendingPathComponent("checkpoint.pt").path) }
    var pythonReady: Bool { python != nil && Self.packages.allSatisfy { !(versions[$0] ?? "").isEmpty } }
    var busy: Bool { phase != .idle }
    var progress: Double { totalSteps > 0 ? Double(step) / Double(totalSteps) : 0 }
    /// Seconds left at the pace so far.
    var remaining: Double? { step > 0 && totalSteps > step ? elapsed / Double(step) * Double(totalSteps - step) : nil }

    init() {
        reloadOwnDesigns()
        loadDatasetSummary()
    }

    // MARK: - Python

    /// Finds python3 and the versions of the packages the trainer needs.
    func checkPython() async {
        guard !busy else { return }
        phase = .checking
        defer { phase = .idle }
        python = CommandLineTool.locate("python3")?.path
        guard let python else {
            message = "Python 3 was not found. Install it from python.org or with Homebrew (brew install python)."
            return
        }
        let script = "import importlib.metadata as m, json\nv = {}\nfor p in \(Self.packages):\n    try: v[p] = m.version(p)\n    except Exception: v[p] = ''\nprint(json.dumps(v))"
        if let output = try? await CommandLineTool.run(URL(fileURLWithPath: python), ["-c", script], timeout: 60),
           let line = output.stdout.split(separator: "\n").last,
           let found = try? JSONDecoder().decode([String: String].self, from: Data(line.utf8)) {
            versions = found
            message = pythonReady ? nil : "Some packages are missing: install them below."
        } else {
            message = "\(python) could not be run."
        }
    }

    /// pip-installs the missing packages for this user.
    func installPackages() async {
        guard !busy, python != nil else { return }
        phase = .installing
        log = []
        let missing = Self.packages.filter { (versions[$0] ?? "").isEmpty }
        let status = await run(["-m", "pip", "install", "--user", "--upgrade"] + missing) { [weak self] in self?.append($0) }
        phase = .idle
        message = status == 0 ? "Packages installed." : "pip stopped with status \(status) — see the log."
        await checkPython()
    }

    // MARK: - Dataset

    /// The reference circuits as tools/circuit_lm/make_dataset.py reads them (--templates).
    private func exportTemplates(to url: URL) throws {
        let rows: [[String: Any]] = OfflineProvider.templates.map { t in
            [
                "title": t.plan.title, "summary": t.plan.summary, "keywords": t.keywords, "excludes": t.excludes,
                "components": t.plan.components.map { c in
                    ["ref": c.ref, "kind": c.kind, "value": c.value, "x": c.x, "y": c.y, "rotation": c.rotation] as [String: Any]
                },
                "connections": t.plan.connections.map { ["from": $0.from, "to": $0.to] },
                "board": ["width": t.plan.board.width, "height": t.plan.board.height],
            ]
        }
        try JSONSerialization.data(withJSONObject: rows, options: [.sortedKeys]).write(to: url)
    }

    func reloadOwnDesigns() {
        let text = (try? String(contentsOf: ownDesignsFile, encoding: .utf8)) ?? ""
        ownDesigns = text.split(separator: "\n").compactMap { line in
            guard let row = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: Any],
                  let prompt = row["prompt"] as? String, let plan = row["plan"] as? String else { return nil }
            let title = (try? JSONSerialization.jsonObject(with: Data(plan.utf8)) as? [String: Any])?["title"] as? String
            return (prompt, title ?? prompt)
        }
    }

    /// Adds a design (the open project) as a training pair.
    func addOwnDesign(prompt: String, plan: DesignPlan) {
        guard let line = try? JSONSerialization.data(withJSONObject: ["prompt": prompt, "plan": plan.jsonString(pretty: false)]) else { return }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let data = line + Data("\n".utf8)
        if let handle = try? FileHandle(forWritingTo: ownDesignsFile) {
            handle.seekToEndOfFile()
            handle.write(data)
            try? handle.close()
        } else {
            try? data.write(to: ownDesignsFile)
        }
        reloadOwnDesigns()
    }

    func removeOwnDesign(at index: Int) {
        let lines = ((try? String(contentsOf: ownDesignsFile, encoding: .utf8)) ?? "").split(separator: "\n")
        guard lines.indices.contains(index) else { return }
        let kept = lines.enumerated().filter { $0.offset != index }.map { String($0.element) + "\n" }.joined()
        try? kept.write(to: ownDesignsFile, atomically: true, encoding: .utf8)
        reloadOwnDesigns()
    }

    /// Writes train.jsonl / test.jsonl / card.json into the training folder.
    func buildDataset() async {
        guard !busy, let script = Self.script("make_dataset") else { return }
        phase = .building
        log = []
        defer { phase = .idle }
        do {
            try FileManager.default.createDirectory(at: dataFolder, withIntermediateDirectories: true)
            let templates = folder.appendingPathComponent("templates.json")
            try exportTemplates(to: templates)
            var arguments = [script, dataFolder.path, "--templates", templates.path,
                             "--phrasings", String(phrasings), "--contexts", String(contexts)]
            if includeOwnDesigns, !ownDesigns.isEmpty { arguments += ["--extra", ownDesignsFile.path] }
            let status = await run(arguments) { [weak self] in self?.append($0) }
            message = status == 0 ? nil : "Building the dataset failed — see the log."
            loadDatasetSummary()
        } catch {
            message = error.localizedDescription
        }
    }

    private func loadDatasetSummary() {
        guard let data = try? Data(contentsOf: dataFolder.appendingPathComponent("card.json")),
              let card = try? JSONSerialization.jsonObject(with: data) as? [String: Any] else { dataset = nil; return }
        let text = (try? String(contentsOf: dataFolder.appendingPathComponent("train.jsonl"), encoding: .utf8)) ?? ""
        let samples = text.split(separator: "\n").prefix(40).enumerated().compactMap { index, line -> Sample? in
            guard let row = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: String],
                  let plan = row["plan"].flatMap({ try? JSONSerialization.jsonObject(with: Data($0.utf8)) as? [String: Any] })
            else { return nil }
            return Sample(id: index, prompt: row["prompt"] ?? "", circuit: plan["title"] as? String ?? "")
        }
        dataset = DatasetSummary(circuits: card["circuits"] as? [String] ?? [], ownDesigns: card["ownDesigns"] as? Int ?? 0,
                                 train: card["train"] as? Int ?? 0, test: card["test"] as? Int ?? 0,
                                 kinds: (card["kinds"] as? [Any])?.count ?? 0, samples: samples)
    }

    // MARK: - Training

    /// Trains (or resumes from the last checkpoint with the same settings) and follows the loss.
    func train() async {
        guard !busy, dataset != nil, let script = Self.script("train") else { return }
        phase = .training
        log = []
        if !hasCheckpoint { losses = [] }
        testLoss = nil
        message = nil
        let environment = ["EPOCHS": String(epochs), "SIZE": size, "LR": String(learningRate)]
        let status = await run([script, dataFolder.path, outFolder.path], environment: environment) { [weak self] line in
            self?.trainingLine(line)
        }
        phase = .idle
        if status == 0, hasTrainedModel {
            message = "Training finished. Score the model in Results."
        } else if status == 15 || status == 9 {
            message = "Training stopped at step \(step) — Resume continues from the last checkpoint."
        } else {
            message = "Training stopped with status \(status) — see the log."
        }
    }

    func stop() {
        process?.terminate()
        evaluation?.cancel()
    }

    /// "step 150/5886 loss 0.7263 lr 1.99e-03 198s", "resumed at step 2400", "wrote … test loss 0.0038"
    private func trainingLine(_ line: String) {
        append(line)
        let words = line.split(separator: " ")
        if words.first == "step", words.count >= 7 {
            let parts = words[1].split(separator: "/")
            if parts.count == 2, let s = Int(parts[0]), let t = Int(parts[1]), let loss = Double(words[3]) {
                step = s
                totalSteps = t
                elapsed = Double(words[6].dropLast()) ?? elapsed
                losses.removeAll { $0.step >= s }
                losses.append(LossPoint(step: s, loss: loss))
            }
        } else if line.hasPrefix("resumed at step"), let s = Int(words.last ?? "") {
            step = s
            losses.removeAll { $0.step > s }
        } else if line.hasPrefix("wrote"), let value = Double(words.last ?? "") {
            testLoss = value
            step = totalSteps
        }
    }

    // MARK: - Results

    /// Scores the trained model (or `model`) on the held-out requests with the built-in engine: usable = a plan whose
    /// links name its parts, exact = the reference's parts and links. `limit` 0 = every request.
    func evaluate(model: URL? = nil, limit: Int = 0) {
        guard !busy else { return }
        let path = (model ?? trainedModel).path
        scoredModel = (model ?? trainedModel).lastPathComponent
        let text = (try? String(contentsOf: dataFolder.appendingPathComponent("test.jsonl"), encoding: .utf8)) ?? ""
        var cases = text.split(separator: "\n").compactMap { line -> (prompt: String, plan: String)? in
            guard let row = try? JSONSerialization.jsonObject(with: Data(line.utf8)) as? [String: String],
                  let prompt = row["prompt"], let plan = row["plan"] else { return nil }
            return (prompt, plan)
        }
        if limit > 0 { cases = Array(cases.prefix(limit)) }
        guard !cases.isEmpty else { message = "Build the dataset first: the held-out requests come with it."; return }
        phase = .evaluating
        evaluated = 0
        evaluationTotal = cases.count
        message = nil
        evaluation = Task { @MainActor in
            var result = Score()
            var byCircuit: [String: CircuitScore] = [:]
            let start = Date()
            for c in cases {
                if Task.isCancelled { break }
                guard let expected = try? JSONExtraction.decode(DesignPlan.self, from: c.plan) else { continue }
                var entry = byCircuit[expected.title] ?? CircuitScore(circuit: expected.title)
                entry.total += 1
                result.total += 1
                if let raw = try? await LocalLLM.run(path: path, system: CircuitModel.system, user: c.prompt, maxTokens: 2000, json: false),
                   let plan = try? JSONExtraction.decode(DesignPlan.self, from: EDAEngine.take(sieda_circuit_plan_values(c.prompt, raw)) ?? raw),
                   CircuitModel.check(plan) == nil {
                    entry.usable += 1
                    result.usable += 1
                    if CircuitModel.sameCircuit(expected, plan) {
                        entry.exact += 1
                        result.exact += 1
                    } else {
                        result.misses.append(c.prompt)
                    }
                } else {
                    result.misses.append(c.prompt)
                }
                byCircuit[expected.title] = entry
                evaluated += 1
            }
            result.seconds = Date().timeIntervalSince(start)
            result.circuits = byCircuit.values.sorted { $0.circuit < $1.circuit }
            score = result
            phase = .idle
        }
    }

    /// Copies the trained model into the models folder (named by date) and returns its file name.
    func install() throws -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyyMMdd-HHmm"
        let name = "sieda-circuit-custom-\(formatter.string(from: Date())).gguf"
        try FileManager.default.createDirectory(at: LocalModelStore.folder, withIntermediateDirectories: true)
        let target = LocalModelStore.folder.appendingPathComponent(name)
        try? FileManager.default.removeItem(at: target)
        try FileManager.default.copyItem(at: trainedModel, to: target)
        LocalModelStore.shared.refresh()
        return name
    }

    // MARK: - Processes

    /// A bundled tools/circuit_lm script (Xcode copies them into the app's resources).
    static func script(_ name: String) -> String? { Bundle.main.url(forResource: name, withExtension: "py")?.path }

    private func append(_ line: String) {
        log.append(line)
        if log.count > 400 { log.removeFirst(log.count - 400) }
    }

    /// Runs python3 with `arguments`, handing each output line to `onLine` on the main actor, in order.
    private func run(_ arguments: [String], environment: [String: String] = [:],
                     onLine: @escaping @MainActor (String) -> Void) async -> Int32 {
        guard let python else { return -1 }
        let process = Process()
        process.executableURL = URL(fileURLWithPath: python)
        process.arguments = arguments
        process.currentDirectoryURL = folder
        var env = ProcessInfo.processInfo.environment
        env["PYTHONUNBUFFERED"] = "1"
        env["PATH"] = (CommandLineTool.searchPaths + [env["PATH"] ?? ""]).joined(separator: ":")
        for (key, value) in environment { env[key] = value }
        process.environment = env
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        let lines = LineSplitter()
        let deliver: @Sendable ([String]) -> Void = { batch in
            guard !batch.isEmpty else { return }
            DispatchQueue.main.async { MainActor.assumeIsolated { batch.forEach(onLine) } }
        }
        pipe.fileHandleForReading.readabilityHandler = { handle in deliver(lines.append(handle.availableData)) }
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        self.process = process
        let status: Int32 = await withCheckedContinuation { continuation in
            process.terminationHandler = { finished in
                pipe.fileHandleForReading.readabilityHandler = nil
                deliver(lines.append(pipe.fileHandleForReading.readDataToEndOfFile()) + lines.flush())
                DispatchQueue.main.async { continuation.resume(returning: finished.terminationStatus) }
            }
            do {
                try process.run()
            } catch {
                deliver([error.localizedDescription])
                continuation.resume(returning: -1)
            }
        }
        self.process = nil
        return status
    }
}

/// Splits a byte stream into lines (thread-safe: the pipe's handler runs on a background queue).
private final class LineSplitter: @unchecked Sendable {
    private let lock = NSLock()
    private var pending = Data()

    func append(_ data: Data) -> [String] {
        lock.lock()
        defer { lock.unlock() }
        pending.append(data)
        var lines: [String] = []
        while let newline = pending.firstIndex(of: 10) {
            lines.append(String(decoding: pending[pending.startIndex..<newline], as: UTF8.self))
            pending.removeSubrange(pending.startIndex...newline)
        }
        return lines
    }

    func flush() -> [String] {
        lock.lock()
        defer { lock.unlock() }
        let rest = String(decoding: pending, as: UTF8.self)
        pending = Data()
        return rest.isEmpty ? [] : [rest]
    }
}
