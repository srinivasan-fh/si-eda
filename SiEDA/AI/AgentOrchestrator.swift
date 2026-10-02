import Foundation

/// Coordinates the design agents:
///
///   Requirements Analyst → Circuit Architect → Plan Compiler (core) → Verification (ERC + SPICE)
///   → Design Reviewer (loop) → Layout (auto-place, autoroute, DRC)
///
/// Language-model agents only ever produce structured JSON; every electrical decision is checked by
/// the C++ core before it reaches the user.
@MainActor
final class AgentOrchestrator: ObservableObject {
    enum Role: String {
        case analyst = "Requirements Analyst"
        case architect = "Circuit Architect"
        case compiler = "Plan Compiler"
        case verifier = "Verification"
        case reviewer = "Design Reviewer"
        case layout = "PCB Layout"

        var systemImage: String {
            switch self {
            case .analyst: return "doc.text.magnifyingglass"
            case .architect: return "point.3.connected.trianglepath.dotted"
            case .compiler: return "hammer"
            case .verifier: return "checkmark.shield"
            case .reviewer: return "person.crop.circle.badge.checkmark"
            case .layout: return "square.grid.3x3.square"
            }
        }
    }

    enum StepStatus: Equatable {
        case running, done, warning, failed, skipped
    }

    struct Step: Identifiable, Equatable {
        let id = UUID()
        var role: Role
        var title: String
        var detail: String = ""
        var status: StepStatus = .running
        var started = Date()
        var duration: TimeInterval?
    }

    struct Message: Identifiable, Equatable {
        enum Sender { case user, agent, system }
        let id = UUID()
        var sender: Sender
        var text: String
        var date = Date()
    }

    @Published private(set) var steps: [Step] = []
    @Published private(set) var messages: [Message] = []
    @Published private(set) var isRunning = false
    @Published private(set) var lastSpec: RequirementsSpec?
    @Published private(set) var lastPlan: DesignPlan?
    @Published private(set) var activeModel = ""

    private var task: Task<Void, Never>?
    /// Identifies the current run: a cancelled run that is still unwinding must not touch the next one.
    private var runID: UUID?

    func cancel() {
        task?.cancel()
        task = nil
        runID = nil
        if isRunning {
            isRunning = false
            if let index = steps.lastIndex(where: { $0.status == .running }) {
                steps[index].status = .skipped
                steps[index].detail = "Cancelled"
            }
            post(.system, "Generation cancelled.")
        }
    }

    func clearConversation() {
        guard !isRunning else { return }
        steps = []
        messages = []
    }

    // MARK: - Entry points

    func generate(brief: String, store: DesignStore, settings: AISettings) {
        let trimmed = brief.trimmingCharacters(in: .whitespacesAndNewlines)
        guard settings.aiEnabled, !trimmed.isEmpty, !isRunning, !store.isBusy else { return }
        post(.user, trimmed)
        let provider = resolveProvider(settings)
        let review = settings.enableReviewAgent
        let rounds = max(0, min(settings.maxReviewRounds, 4))
        start(provider: provider) { [weak self] in
            guard let self else { return }
            try await self.runGeneration(brief: trimmed, provider: provider, store: store, review: review, rounds: rounds)
        }
    }

    func refine(instruction: String, store: DesignStore, settings: AISettings) {
        let trimmed = instruction.trimmingCharacters(in: .whitespacesAndNewlines)
        guard settings.aiEnabled, !trimmed.isEmpty, !isRunning, !store.isBusy else { return }
        post(.user, trimmed)
        let provider = resolveProvider(settings)
        let review = settings.enableReviewAgent
        let rounds = max(0, min(settings.maxReviewRounds, 4))
        start(provider: provider) { [weak self] in
            guard let self else { return }
            try await self.runRefinement(instruction: trimmed, provider: provider, store: store, review: review,
                                         rounds: rounds)
        }
    }

    /// The selected model, or the offline designer when that model has no API key yet (so a fresh install works).
    private func resolveProvider(_ settings: AISettings) -> AIProvider {
        guard settings.hasCredentials(for: settings.provider) else {
            post(.system, "Not signed in to \(settings.provider.displayName) — using the offline designer. "
                 + "Sign in (or add a key) in Settings → AI Models to use the model.")
            return OfflineProvider()
        }
        return settings.makeProvider()
    }

    private func start(provider: AIProvider, _ work: @escaping @MainActor () async throws -> Void) {
        steps = []
        isRunning = true
        activeModel = "\(provider.displayName) · \(provider.modelName)"
        let id = UUID()
        runID = id
        task = Task { @MainActor [weak self] in
            do {
                try await work()
            } catch is CancellationError {
                // already reported by cancel()
            } catch {
                // A cancelled URLSession request surfaces as a network error; cancel() already reported it.
                if !Task.isCancelled { self?.reportFailure(error) }
            }
            if self?.runID == id {
                self?.isRunning = false
                self?.runID = nil
            }
        }
    }

    private func reportFailure(_ error: Error) {
        if let index = steps.lastIndex(where: { $0.status == .running }) {
            finish(index, .failed, error.localizedDescription)
        }
        post(.system, "⚠️ \(error.localizedDescription)")
    }

    // MARK: - Pipelines

    private func runGeneration(brief: String, provider: AIProvider, store: DesignStore, review: Bool,
                               rounds: Int) async throws {
        // 1. Requirements
        var index = begin(.analyst, "Analysing requirements")
        let specText = try await provider.complete(AgentPrompts.analystRequest(brief: brief))
        let spec = try JSONExtraction.decode(RequirementsSpec.self, from: specText)
        lastSpec = spec
        finish(index, .done, "\(spec.title): \(spec.functionalRequirements.count) requirements, "
            + "\(spec.designBlocks.count) blocks, \(EngineeringFormat.string(spec.supplyVoltage, unit: "V")) supply")
        try Task.checkCancellation()

        // 2. Architecture
        index = begin(.architect, "Designing circuit")
        let planText = try await provider.complete(AgentPrompts.architectRequest(brief: brief, spec: spec,
                                                                                         customParts: store.snapshot.customParts))
        var plan = try JSONExtraction.decode(DesignPlan.self, from: planText)
        if plan.board.width < 10 || plan.board.height < 10 {
            plan.board.width = max(20, spec.boardWidthMM)
            plan.board.height = max(15, spec.boardHeightMM)
        }
        finish(index, .done, "\(plan.components.count) parts, \(plan.connections.count) connections")
        try Task.checkCancellation()

        try await compileVerifyReview(plan: plan, spec: spec, brief: brief, provider: provider, store: store,
                                      review: review, rounds: rounds)
    }

    private func runRefinement(instruction: String, provider: AIProvider, store: DesignStore, review: Bool,
                               rounds: Int) async throws {
        let requirements = store.snapshot.requirements
        let index = begin(.architect, "Applying change request")
        let current = store.currentPlan
        let planText = try await provider.complete(
            AgentPrompts.refineRequest(instruction: instruction, current: current, requirements: requirements,
                                       customParts: store.snapshot.customParts))
        var plan = try JSONExtraction.decode(DesignPlan.self, from: planText)
        if plan.board.width < 10 || plan.board.height < 10 { plan.board = current.board }
        finish(index, .done, "\(plan.components.count) parts, \(plan.connections.count) connections")
        try Task.checkCancellation()
        let brief = Self.appendingChangeRequest(instruction, to: requirements)
        try await compileVerifyReview(plan: plan, spec: lastSpec, brief: brief, provider: provider, store: store,
                                      review: review, rounds: rounds)
    }

    static let changeRequestsHeader = "Change requests:"
    static let maxChangeRequests = 12

    /// The original requirements followed by one "Change requests:" list (most recent last, duplicates dropped,
    /// at most `maxChangeRequests`), instead of a new "Change request:" paragraph appended on every refinement.
    static func appendingChangeRequest(_ instruction: String, to requirements: String) -> String {
        let request = instruction.trimmingCharacters(in: .whitespacesAndNewlines)
        var base = "\n\n" + requirements  // so a list at the very start is found like one after the PRD
        var requests: [String] = []
        if let range = base.range(of: "\n\n" + changeRequestsHeader + "\n") {
            requests = base[range.upperBound...].split(separator: "\n").map {
                String($0).trimmingCharacters(in: .whitespaces)
            }.filter { $0.hasPrefix("- ") }.map { String($0.dropFirst(2)) }
            base = String(base[..<range.lowerBound])
        }
        // Earlier versions appended "\n\nChange request: …" paragraphs.
        let legacy = base.components(separatedBy: "\n\nChange request: ")
        base = legacy[0]
        requests = legacy.dropFirst().map { $0.trimmingCharacters(in: .whitespacesAndNewlines) } + requests
        if !request.isEmpty {
            requests.removeAll { $0.caseInsensitiveCompare(request) == .orderedSame }
            requests.append(request.replacingOccurrences(of: "\n", with: " "))
        }
        requests = Array(requests.suffix(maxChangeRequests))
        base = base.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !requests.isEmpty else { return base }
        let list = changeRequestsHeader + "\n" + requests.map { "- " + $0 }.joined(separator: "\n")
        return base.isEmpty ? list : base + "\n\n" + list
    }

    private func compileVerifyReview(plan initialPlan: DesignPlan, spec: RequirementsSpec?, brief: String,
                                     provider: AIProvider, store: DesignStore, review: Bool, rounds: Int) async throws {
        var plan = initialPlan
        compile(plan, brief: brief, store: store)
        await verify(store: store)
        try Task.checkCancellation()

        if review && rounds > 0 {
            for round in 1...rounds {
                try Task.checkCancellation()
                let index = begin(.reviewer, "Design review — round \(round)")
                let text = try await provider.complete(AgentPrompts.reviewRequest(
                    spec: spec, brief: brief, plan: plan, erc: store.ercResults, dc: store.dcResult,
                    customParts: store.snapshot.customParts))
                let verdict = try JSONExtraction.decode(DesignReview.self, from: text)
                if verdict.approved {
                    finish(index, .done, verdict.issues.isEmpty ? "Approved" : "Approved with notes: "
                        + verdict.issues.joined(separator: "; "))
                    break
                }
                finish(index, .warning, verdict.issues.joined(separator: "; "))
                plan = verdict.plan
                compile(plan, brief: brief, store: store)
                await verify(store: store)
                try Task.checkCancellation()
            }
        } else {
            steps.append(Step(role: .reviewer, title: "Design review", detail: "Disabled in Settings", status: .skipped))
        }
        try Task.checkCancellation()

        try await layout(store: store, boardHint: plan.board)
        try Task.checkCancellation()
        lastPlan = plan
        summarize(plan, store: store)
    }

    private func compile(_ plan: DesignPlan, brief: String, store: DesignStore) {
        let index = begin(.compiler, "Compiling plan into schematic")
        let report = store.applyPlan(plan, requirements: brief)
        let detail = "\(report.componentsAdded) parts, \(report.connectionsMade) wires"
            + (report.warnings.isEmpty ? "" : " — " + report.warnings.joined(separator: " "))
        finish(index, report.warnings.isEmpty ? .done : .warning, detail)
    }

    private func verify(store: DesignStore) async {
        let index = begin(.verifier, "ERC + DC operating point")
        store.runERC()
        await store.simulateDC()
        let errors = store.ercResults.filter { $0.severity == .error }.count
        let warnings = store.ercResults.filter { $0.severity == .warning }.count
        var detail = "ERC \(errors) error(s), \(warnings) warning(s)"
        if let dc = store.dcResult {
            detail += dc.converged ? "; DC converged (\(dc.iterations) it.)" : "; DC: \(dc.error)"
        }
        finish(index, errors == 0 && (store.dcResult?.converged ?? false) ? .done : .warning, detail)
    }

    private func layout(store: DesignStore, boardHint: PlannedBoard) async throws {
        let index = begin(.layout, "Placing and routing PCB")
        store.autoPlace(all: true)
        store.fitBoard(margin: 2.5)  // compact outline around the placed parts
        await store.autoRoute()
        try Task.checkCancellation()
        if let stats = store.routeStats, stats.failed > 0 {
            // Give the router more room and try once more.
            let board = store.snapshot.board
            store.setBoard(width: board.width * 1.3, height: board.height * 1.3, trackWidth: 0, clearance: 0)
            store.autoPlace(all: true)
            await store.autoRoute()
            try Task.checkCancellation()
        }
        store.runDRC()
        let stats = store.routeStats ?? RouteStats()
        let drcErrors = store.drcResults.filter { $0.severity == .error }.count
        let board = store.snapshot.board
        let detail = String(format: "%.0f×%.0f mm, %d/%d routed, %d vias, DRC %@", board.width, board.height,
                            stats.routed, stats.connections, stats.vias, drcErrors == 0 ? "clean" : "\(drcErrors) error(s)")
        finish(index, stats.failed == 0 && drcErrors == 0 ? .done : .warning, detail)
    }

    private func summarize(_ plan: DesignPlan, store: DesignStore) {
        var lines = ["**\(plan.title)**"]
        if !plan.summary.isEmpty { lines.append(plan.summary) }
        if !plan.notes.isEmpty { lines.append(plan.notes.map { "• " + $0 }.joined(separator: "\n")) }
        if let dc = store.dcResult, dc.converged {
            let interesting = dc.devices.prefix(6).map {
                "\($0.ref) \(EngineeringFormat.string($0.current, unit: "A")) / \(EngineeringFormat.string(abs($0.power), unit: "W"))"
            }
            if !interesting.isEmpty { lines.append("Operating point: " + interesting.joined(separator: ", ")) }
        }
        lines.append("Open the Schematic, PCB Layout and 3D Viewer workspaces to inspect the result.")
        post(.agent, lines.joined(separator: "\n\n"))
    }

    // MARK: - Step bookkeeping

    private func begin(_ role: Role, _ title: String) -> Int {
        guard !Task.isCancelled else { return -1 }
        steps.append(Step(role: role, title: title))
        return steps.count - 1
    }

    private func finish(_ index: Int, _ status: StepStatus, _ detail: String) {
        guard !Task.isCancelled, steps.indices.contains(index) else { return }
        steps[index].status = status
        steps[index].detail = detail
        steps[index].duration = Date().timeIntervalSince(steps[index].started)
    }

    private func post(_ sender: Message.Sender, _ text: String) {
        // Called from a run's task: a cancelled run stays silent (cancel() reported it). Outside a task this is false.
        guard !Task.isCancelled else { return }
        messages.append(Message(sender: sender, text: text))
    }
}
