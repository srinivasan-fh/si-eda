import Charts
import SwiftUI

/// Super Intelligence → Own LLM: SiEDA's circuit model — build its dataset, train it on this Mac, score the result,
/// try it, and choose the model the agents use (docs/AI.md, "SiEDA's own circuit model").
struct CircuitModelView: View {
    enum Tab: Hashable { case dataset, training, results, playground, models }

    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var store: DesignStore
    @ObservedObject private var trainer = CircuitTrainer.shared
    @ObservedObject private var models = LocalModelStore.shared
    @State private var tab = Tab.dataset

    var body: some View {
        VStack(spacing: 0) {
            header
            Divider()
            TabView(selection: $tab) {
                OwnModelDatasetView().tabItem { Label("Dataset", systemImage: "tablecells") }.tag(Tab.dataset)
                OwnModelTrainingView().tabItem { Label("Training", systemImage: "waveform.path.ecg") }.tag(Tab.training)
                OwnModelResultsView().tabItem { Label("Results", systemImage: "checkmark.seal") }.tag(Tab.results)
                OwnModelPlaygroundView().tabItem { Label("Playground", systemImage: "wand.and.stars") }.tag(Tab.playground)
                OwnModelModelsView().tabItem { Label("Models", systemImage: "shippingbox") }.tag(Tab.models)
            }
            .padding(.top, 6)
        }
        .environmentObject(trainer)
    }

    /// The model in use and where the pipeline stands.
    private var header: some View {
        HStack(spacing: 14) {
            Image(systemName: "cpu.fill").font(.title2).foregroundStyle(Theme.skyBlue)
            VStack(alignment: .leading, spacing: 2) {
                Text("SiEDA Circuit Model").font(.headline)
                Text(verbatim: activeModelLine).font(.caption).foregroundStyle(Theme.textMuted)
            }
            Spacer()
            OwnModelStep(title: "Dataset", done: trainer.dataset != nil, active: trainer.phase == .building)
            OwnModelStep(title: "Training", done: trainer.hasTrainedModel, active: trainer.phase == .training)
            OwnModelStep(title: "Results", done: trainer.score != nil, active: trainer.phase == .evaluating)
        }
        .padding(.horizontal, 16)
        .padding(.vertical, 10)
    }

    private var activeModelLine: String {
        let active = settings.model(for: .builtIn)
        let inUse = settings.provider == .builtIn && CircuitModel.isCircuitModel(active)
        let card = CircuitModel.card.map { String(format: "%.1f M parameters · %ld circuits", Double($0.parameters) / 1e6, $0.circuits.count) } ?? ""
        return inUse ? "\(active) · in use · \(card)" : "Bundled: \(CircuitModel.file) · \(card)"
    }
}

/// A step of the pipeline in the header: done, running or to do.
private struct OwnModelStep: View {
    let title: LocalizedStringKey
    let done: Bool
    let active: Bool

    var body: some View {
        HStack(spacing: 5) {
            if active {
                ProgressView().controlSize(.mini)
            } else {
                Image(systemName: done ? "checkmark.circle.fill" : "circle").foregroundStyle(done ? Theme.skyBlue : Theme.textMuted)
            }
            Text(title).font(.caption.weight(.medium))
        }
        .padding(.horizontal, 8)
        .padding(.vertical, 4)
        .background(Capsule().fill(Theme.deepBlue.opacity(0.6)))
    }
}

/// A figure with its label (dataset size, scores…).
struct MetricTile: View {
    let title: LocalizedStringKey
    let value: String
    var detail: String = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            Text(verbatim: value).font(.title2.weight(.semibold).monospacedDigit())
            if !detail.isEmpty { Text(verbatim: detail).font(.caption2).foregroundStyle(Theme.textMuted) }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.deepBlue.opacity(0.5)))
    }
}

/// The trainer's recent output.
private struct TrainerLog: View {
    let lines: [String]

    var body: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 1) {
                    ForEach(Array(lines.enumerated()), id: \.offset) { index, line in
                        Text(verbatim: line).font(.system(.caption2, design: .monospaced)).textSelection(.enabled).id(index)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(height: 110)
            .padding(6)
            .background(RoundedRectangle(cornerRadius: 6).fill(Color.black.opacity(0.25)))
            .onChange(of: lines.count) { _, count in proxy.scrollTo(count - 1, anchor: .bottom) }
        }
    }
}

// MARK: - Dataset

private struct OwnModelDatasetView: View {
    @EnvironmentObject private var trainer: CircuitTrainer
    @EnvironmentObject private var store: DesignStore

    private var trainable: Int { OfflineProvider.templates.filter { $0.plan.components.count <= 30 }.count }

    var body: some View {
        Form {
            Section("Sources") {
                LabeledContent("Reference circuits") {
                    Text(verbatim: "\(trainable) of \(OfflineProvider.templates.count)")
                }
                Text("Every reference circuit of up to 30 parts (larger ones do not fit the model's 2,048-token context): parts with SiEDA's kind names and values, positions and every pin-to-pin link, including the VFD, servo and stepper drives.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                LabeledContent("Computed-value circuits") {
                    Text("LED, divider, RC filter, op-amp gain, NPN driver")
                }
                Toggle(isOn: $trainer.includeOwnDesigns) {
                    Text(verbatim: "\(String(localized: "Include my designs")) (\(trainer.ownDesigns.count))")
                }
                ForEach(Array(trainer.ownDesigns.enumerated()), id: \.offset) { index, design in
                    HStack {
                        Image(systemName: "doc.text").foregroundStyle(Theme.textMuted)
                        VStack(alignment: .leading) {
                            Text(verbatim: design.title)
                            Text(verbatim: design.prompt).font(.caption).foregroundStyle(Theme.textMuted).lineLimit(1)
                        }
                        Spacer()
                        Button(role: .destructive) { trainer.removeOwnDesign(at: index) } label: { Image(systemName: "trash") }
                            .buttonStyle(.borderless)
                            .help("Remove from the training data")
                    }
                }
                Button {
                    let plan = DesignPlanCompiler.plan(from: store.snapshot)
                    let request = store.snapshot.requirements.isEmpty ? store.snapshot.name : store.snapshot.requirements
                    trainer.addOwnDesign(prompt: request, plan: plan)
                } label: {
                    Label("Add the Open Design", systemImage: "plus.circle")
                }
                .disabled(store.snapshot.components.isEmpty)
            }
            Section("Settings") {
                Stepper(value: $trainer.phrasings, in: 20...200, step: 10) {
                    LabeledContent("Requests per circuit") { Text(verbatim: "\(trainer.phrasings)") }
                }
                Stepper(value: $trainer.contexts, in: 0...60, step: 5) {
                    LabeledContent("Requests with an application context") { Text(verbatim: "\(trainer.contexts)") }
                }
                Text("Each circuit is asked for by its title, summary and every keyword in many wordings; context requests (\"… for a pump motor\") are labelled by the Offline Designer's own keyword rule. About 6 % of the requests are held out for scoring.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                HStack {
                    Button {
                        Task { await trainer.buildDataset() }
                    } label: {
                        Label("Build Dataset", systemImage: "hammer")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(trainer.busy || !trainer.pythonReady)
                    if trainer.phase == .building { ProgressView().controlSize(.small) }
                    if !trainer.pythonReady {
                        Text("Set up Python in the Training tab first.").font(.caption).foregroundStyle(Theme.warning)
                    }
                }
            }
            if let dataset = trainer.dataset {
                Section("Dataset") {
                    HStack(spacing: 10) {
                        MetricTile(title: "Training requests", value: "\(dataset.train)")
                        MetricTile(title: "Held out", value: "\(dataset.test)")
                        MetricTile(title: "Circuits", value: "\(dataset.circuits.count)", detail: dataset.ownDesigns > 0 ? "+\(dataset.ownDesigns) own" : "")
                        MetricTile(title: "Part kinds", value: "\(dataset.kinds)")
                    }
                    Table(dataset.samples) {
                        TableColumn("Request") { Text(verbatim: $0.prompt) }
                        TableColumn("Circuit") { Text(verbatim: $0.circuit).foregroundStyle(Theme.textSecondary) }
                    }
                    .frame(minHeight: 180)
                }
            }
            if !trainer.log.isEmpty, trainer.phase == .building || trainer.phase == .idle {
                Section("Log") { TrainerLog(lines: trainer.log) }
            }
        }
        .formStyle(.grouped)
    }
}

// MARK: - Training

private struct OwnModelTrainingView: View {
    @EnvironmentObject private var trainer: CircuitTrainer

    var body: some View {
        Form {
            Section("Python environment") {
                LabeledContent("Python 3") {
                    Text(verbatim: trainer.python ?? String(localized: "Not found")).foregroundStyle(trainer.python == nil ? Theme.warning : Theme.textSecondary)
                }
                ForEach(CircuitTrainer.packages, id: \.self) { name in
                    let version = trainer.versions[name] ?? ""
                    LabeledContent(name) {
                        Label(version.isEmpty ? String(localized: "Missing") : version,
                              systemImage: version.isEmpty ? "xmark.circle" : "checkmark.circle.fill")
                            .foregroundStyle(version.isEmpty ? Theme.warning : Theme.skyBlue)
                    }
                }
                HStack {
                    Button("Check Again") { Task { await trainer.checkPython() } }.disabled(trainer.busy)
                    Button {
                        Task { await trainer.installPackages() }
                    } label: {
                        Label("Install Missing Packages", systemImage: "arrow.down.circle")
                    }
                    .disabled(trainer.busy || trainer.python == nil || trainer.pythonReady)
                    if trainer.phase == .checking || trainer.phase == .installing { ProgressView().controlSize(.small) }
                }
            }
            Section("Settings") {
                Picker("Model size", selection: $trainer.size) {
                    Text("Small · 4.9 M parameters").tag("small")
                    Text("Tiny · 2.0 M parameters (about twice as fast)").tag("tiny")
                }
                Stepper(value: $trainer.epochs, in: 2...40, step: 1) {
                    LabeledContent("Epochs") { Text(verbatim: String(format: "%.0f", trainer.epochs)) }
                }
                Picker("Peak learning rate", selection: $trainer.learningRate) {
                    Text(verbatim: "1e-3").tag(1e-3)
                    Text(verbatim: "2e-3").tag(2e-3)
                    Text(verbatim: "3e-3").tag(3e-3)
                }
                Text("Training runs on this Mac's CPU with PyTorch: about 2 hours for the full dataset at 16 epochs. A checkpoint is saved every 200 steps, so Stop and Resume lose almost nothing.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            Section("Progress") {
                HStack(spacing: 10) {
                    if trainer.phase == .training {
                        Button(role: .destructive) { trainer.stop() } label: { Label("Stop", systemImage: "stop.fill") }
                    } else {
                        Button {
                            Task { await trainer.train() }
                        } label: {
                            Label(trainer.hasCheckpoint ? "Resume Training" : "Start Training", systemImage: "play.fill")
                        }
                        .buttonStyle(.borderedProminent)
                        .disabled(trainer.busy || !trainer.pythonReady || trainer.dataset == nil)
                    }
                    if trainer.dataset == nil {
                        Text("Build the dataset first.").font(.caption).foregroundStyle(Theme.warning)
                    }
                    Spacer()
                }
                if trainer.totalSteps > 0 {
                    ProgressView(value: trainer.progress) {
                        Text(verbatim: progressLine)
                            .font(.caption.monospacedDigit())
                    }
                    HStack(spacing: 10) {
                        MetricTile(title: "Step", value: "\(trainer.step)", detail: "of \(trainer.totalSteps)")
                        MetricTile(title: "Loss", value: trainer.losses.last.map { String(format: "%.4f", $0.loss) } ?? "—")
                        MetricTile(title: "Held-out loss", value: trainer.testLoss.map { String(format: "%.4f", $0) } ?? "—")
                        MetricTile(title: "Time", value: Self.duration(trainer.elapsed),
                                   detail: trainer.remaining.map { "≈ \(Self.duration($0)) left" } ?? "")
                    }
                }
                if !trainer.losses.isEmpty {
                    Chart(trainer.losses) { point in
                        LineMark(x: .value("Step", point.step), y: .value("Loss", max(point.loss, 1e-5)))
                            .foregroundStyle(Theme.skyBlue)
                    }
                    .chartYScale(type: .log)
                    .chartXAxisLabel("Step")
                    .chartYAxisLabel("Training loss (log)")
                    .frame(height: 180)
                }
                if let message = trainer.message { Text(verbatim: message).font(.caption).foregroundStyle(Theme.textSecondary) }
            }
            if !trainer.log.isEmpty { Section("Log") { TrainerLog(lines: trainer.log) } }
        }
        .formStyle(.grouped)
        .task { if trainer.python == nil { await trainer.checkPython() } }
    }

    private var progressLine: String {
        String(format: "%.1f %%", trainer.progress * 100)
    }

    static func duration(_ seconds: Double) -> String {
        let s = Int(seconds)
        return s >= 3600 ? String(format: "%d h %02d min", s / 3600, s % 3600 / 60) : String(format: "%d min %02d s", s / 60, s % 60)
    }
}

// MARK: - Results

private struct OwnModelResultsView: View {
    @EnvironmentObject private var trainer: CircuitTrainer
    @EnvironmentObject private var settings: AISettings
    @ObservedObject private var models = LocalModelStore.shared
    @State private var limit = 50
    @State private var installed = ""

    var body: some View {
        Form {
            Section("Score") {
                Picker("Held-out requests", selection: $limit) {
                    Text("First 50").tag(50)
                    Text("First 100").tag(100)
                    Text("All").tag(0)
                }
                HStack(spacing: 10) {
                    Button {
                        trainer.evaluate(limit: limit)
                    } label: {
                        Label("Score the Trained Model", systemImage: "checkmark.seal")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(trainer.busy || !trainer.hasTrainedModel)
                    Button("Score the Shipped Model") {
                        if let url = CircuitModel.bundledURL { trainer.evaluate(model: url, limit: limit) }
                    }
                    .disabled(trainer.busy || CircuitModel.bundledURL == nil || trainer.dataset == nil)
                    if trainer.phase == .evaluating {
                        ProgressView(value: Double(trainer.evaluated), total: Double(max(trainer.evaluationTotal, 1)))
                            .frame(width: 140)
                        Text(verbatim: "\(trainer.evaluated) / \(trainer.evaluationTotal)").font(.caption.monospacedDigit())
                        Button("Stop") { trainer.stop() }
                    }
                }
                Text("Each held-out request runs through the built-in engine; the plan counts as usable when every link names one of its parts, and as exact when its parts and links are the reference circuit's (in any order).")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                if !trainer.hasTrainedModel {
                    Text("Train a model first, or score the shipped one for comparison.").font(.caption).foregroundStyle(Theme.warning)
                }
            }
            if let score = trainer.score {
                Section {
                    HStack(spacing: 10) {
                        MetricTile(title: "Usable plans", value: Self.percent(score.usable, score.total), detail: "\(score.usable) / \(score.total)")
                        MetricTile(title: "Exactly the reference", value: Self.percent(score.exact, score.total), detail: "\(score.exact) / \(score.total)")
                        MetricTile(title: "Time per request", value: String(format: "%.1f s", score.seconds / Double(max(score.total, 1))))
                        if let card = CircuitModel.card {
                            MetricTile(title: "Shipped model", value: String(format: "%.1f %%", card.exactPlans * 100),
                                       detail: "exact on \(card.testPairs)")
                        }
                    }
                    Table(score.circuits) {
                        TableColumn("Circuit") { Text(verbatim: $0.circuit) }
                        TableColumn("Requests") { Text(verbatim: "\($0.total)").monospacedDigit() }
                            .width(70)
                        TableColumn("Usable") { Text(verbatim: "\($0.usable)").monospacedDigit() }
                            .width(60)
                        TableColumn("Exact") { row in
                            Text(verbatim: "\(row.exact)")
                                .monospacedDigit()
                                .foregroundStyle(row.exact == row.total ? Theme.skyBlue : Theme.warning)
                        }
                        .width(60)
                    }
                    .frame(minHeight: 200)
                    if !score.misses.isEmpty {
                        DisclosureGroup {
                            ForEach(score.misses, id: \.self) { Text(verbatim: $0).font(.caption) }
                        } label: {
                            Text(verbatim: "\(String(localized: "Requests not exactly right")) (\(score.misses.count))")
                        }
                    }
                } header: {
                    Text(verbatim: trainer.scoredModel)
                }
                Section("Install") {
                    HStack {
                        Button {
                            if let name = try? trainer.install() { installed = name }
                        } label: {
                            Label("Install the Trained Model", systemImage: "square.and.arrow.down")
                        }
                        .disabled(!trainer.hasTrainedModel || trainer.busy)
                        if !installed.isEmpty {
                            Button("Use It") {
                                settings.aiEnabled = true
                                settings.provider = .builtIn
                                settings.models[.builtIn] = installed
                            }
                            .buttonStyle(.borderedProminent)
                            Text(verbatim: installed).font(.caption).foregroundStyle(Theme.textMuted)
                        }
                    }
                    Text("The model is copied into the models folder beside the bundled one; the Models tab switches between them.")
                        .font(.caption).foregroundStyle(Theme.textMuted)
                }
            }
        }
        .formStyle(.grouped)
    }

    static func percent(_ part: Int, _ total: Int) -> String {
        total > 0 ? String(format: "%.1f %%", Double(part) / Double(total) * 100) : "—"
    }
}

// MARK: - Playground

private struct OwnModelPlaygroundView: View {
    @State private var prompt = "Design a VFD for a 0.75 kW induction motor"
    @State private var reply = ""
    @State private var verdict = ""
    @State private var running = false
    @State private var bench: (valid: Int, exact: Int, total: Int)?

    var body: some View {
        Form {
            Section("Request") {
                TextField("Request", text: $prompt, axis: .vertical).lineLimit(2...4)
                HStack {
                    Button {
                        generate()
                    } label: {
                        Label("Generate", systemImage: "wand.and.stars")
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(running || prompt.isEmpty)
                    Button("Run Benchmark") { runBenchmark() }.disabled(running || (CircuitModel.card?.benchmark.isEmpty ?? true))
                    if running { ProgressView().controlSize(.small) }
                }
                if !verdict.isEmpty { Text(verbatim: verdict) }
                if let bench {
                    Text(verbatim: "\(bench.valid) / \(bench.total) usable plans · \(bench.exact) / \(bench.total) exactly the reference circuit")
                }
            }
            if !reply.isEmpty {
                Section("Plan") {
                    ScrollView { Text(verbatim: reply).font(.system(.caption, design: .monospaced)).textSelection(.enabled) }
                        .frame(minHeight: 160)
                }
            }
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
        guard let cases = CircuitModel.card?.benchmark else { return }
        running = true
        Task { @MainActor in
            defer { running = false }
            var valid = 0, exact = 0
            for c in cases {
                guard let result = try? await CircuitModel.plan(for: c.prompt, path: try modelPath()) else { continue }
                valid += 1
                if let expected = try? JSONExtraction.decode(DesignPlan.self, from: c.plan),
                   CircuitModel.sameCircuit(expected, result.plan) { exact += 1 }
            }
            bench = (valid, exact, cases.count)
        }
    }
}

// MARK: - Models

private struct OwnModelModelsView: View {
    @EnvironmentObject private var settings: AISettings
    @ObservedObject private var models = LocalModelStore.shared

    var body: some View {
        Form {
            Section("Circuit models") {
                ForEach(models.files.filter(CircuitModel.isCircuitModel), id: \.self) { file in
                    let inUse = settings.provider == .builtIn && settings.model(for: .builtIn) == file
                    HStack {
                        Image(systemName: inUse ? "checkmark.circle.fill" : "cpu").foregroundStyle(inUse ? Theme.skyBlue : Theme.textMuted)
                        VStack(alignment: .leading) {
                            Text(verbatim: file)
                            Text(verbatim: sizeText(file)).font(.caption).foregroundStyle(Theme.textMuted)
                        }
                        Spacer()
                        if inUse {
                            Text("In Use").font(.caption.weight(.semibold)).foregroundStyle(Theme.skyBlue)
                        } else {
                            Button("Use") { use(file) }
                        }
                    }
                }
                if !models.files.contains(CircuitModel.file) {
                    Button {
                        if let file = try? CircuitModel.install() { models.refresh(); use(file) }
                    } label: {
                        Label("Use the Bundled Model", systemImage: "shippingbox")
                    }
                    .disabled(CircuitModel.bundledURL == nil)
                }
            }
            if let card = CircuitModel.card {
                Section("Bundled model") {
                    LabeledContent("Parameters") { Text(verbatim: String(format: "%.1f M", Double(card.parameters) / 1e6)) }
                    LabeledContent("Training requests") { Text(verbatim: "\(card.trainPairs) + \(card.testPairs) held out") }
                    LabeledContent("Held-out: usable / exact") {
                        Text(verbatim: String(format: "%.0f %% / %.1f %%", card.validPlans * 100, card.exactPlans * 100))
                    }
                    LabeledContent("Part kinds (active and passive)") { Text(verbatim: "\(card.kinds.count)") }
                    Text(verbatim: card.circuits.joined(separator: " · ")).font(.caption).foregroundStyle(Theme.textMuted)
                }
            }
            Section {
                Text("Using a circuit model makes it the AI that designs new circuits: it runs on this Mac with no account and no download; specifications and reviews come from the Offline Designer.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
        }
        .formStyle(.grouped)
    }

    private func use(_ file: String) {
        settings.aiEnabled = true
        settings.provider = .builtIn
        settings.models[.builtIn] = file
    }

    private func sizeText(_ file: String) -> String {
        let size = (try? FileManager.default.attributesOfItem(atPath: LocalModelStore.folder.appendingPathComponent(file).path)[.size]
                    as? NSNumber)?.int64Value ?? 0
        return ByteCountFormatter.string(fromByteCount: size, countStyle: .file)
    }
}
