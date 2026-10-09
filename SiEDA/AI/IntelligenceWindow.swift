import Metal
import SwiftUI

/// Window → Super Intelligence (⌥⌘I): the three ways to use AI in SiEDA — a local LLM on SiEDA's engine (set up one
/// step per tab, each marked ✓ once done), a cloud LLM agent client driving SiEDA over MCP, and SiEDA's own circuit
/// model on SiEDA's engine (docs/AI.md, "Super Intelligence window").
struct IntelligenceSetupView: View {
    enum UseCase: Hashable { case local, mcp, own }

    enum Step: Int, CaseIterable, Identifiable {
        case provider, model, gpu, test, design
        var id: Int { rawValue }

        var title: LocalizedStringKey {
            switch self {
            case .provider: return "Provider"
            case .model: return "Local Model"
            case .gpu: return "GPU"
            case .test: return "Test"
            case .design: return "Design"
            }
        }
    }

    enum TestState: Equatable {
        case idle, running, ok(String), failed(String)
    }

    @EnvironmentObject private var settings: AISettings
    @EnvironmentObject private var store: DesignStore
    @ObservedObject private var models = LocalModelStore.shared
    @ObservedObject private var mcp = MCPLiveServer.shared
    @AppStorage("ai.builtIn.gpu") private var useGPU = true
    @State private var step = Step.provider
    @State private var test = TestState.idle
    @State private var designed = false
    @State private var useCase = UseCase.local
    private let gpuName = MTLCreateSystemDefaultDevice()?.name

    private var local: Bool { settings.provider == .builtIn }

    func done(_ s: Step) -> Bool {
        switch s {
        case .provider: return settings.aiEnabled && settings.hasCredentials(for: settings.provider)
        case .model: return !local || models.files.contains(settings.model(for: .builtIn))
        case .gpu: return !local || gpuName != nil || !useGPU
        case .test: if case .ok = test { return true } else { return false }
        case .design: return designed
        }
    }

    private var mcpConnected: Bool {
        if case .listening = mcp.status { return mcp.callCount > 0 }
        return false
    }

    var body: some View {
        VStack(spacing: 0) {
            Picker("", selection: $useCase) {
                (Text(verbatim: "1  ") + Text("Local LLM (Our Engine)")).tag(UseCase.local)
                (Text(verbatim: mcpConnected ? "✓ " : "2  ") + Text("MCP + Cloud LLM")).tag(UseCase.mcp)
                (Text(verbatim: "3  ") + Text("Own Model (Our Engine)")).tag(UseCase.own)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            .fixedSize()
            .padding(.top, 10)
            switch useCase {
            case .local: setup
            case .mcp: mcpPage.padding(20).frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
            case .own: CircuitModelView()
            }
        }
        .frame(minWidth: 640, minHeight: 460)
    }

    private var setup: some View {
        VStack(spacing: 0) {
            TabView(selection: $step) {
                ForEach(Step.allCases) { s in
                    page(s)
                        .padding(20)
                        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
                        .tabItem { Text(verbatim: done(s) ? "✓ " : "\(s.rawValue + 1)  ") + Text(s.title) }
                        .tag(s)
                }
            }
            HStack {
                Button("Back") { step = Step(rawValue: step.rawValue - 1) ?? step }
                    .disabled(step == .provider)
                Spacer()
                Text(verbatim: "\(Step.allCases.filter(done).count) / \(Step.allCases.count)")
                    .foregroundStyle(Theme.textMuted)
                Spacer()
                Button("Next") { step = Step(rawValue: step.rawValue + 1) ?? step }
                    .disabled(step == .design)
            }
            .padding(12)
        }
    }

    @ViewBuilder
    private func page(_ s: Step) -> some View {
        switch s {
        case .provider:
            Form {
                Toggle("Use AI", isOn: $settings.aiEnabled)
                Picker("Provider", selection: $settings.provider) {
                    ForEach(AIProviderKind.allCases) { kind in
                        Label(kind.displayName, systemImage: kind.systemImage).tag(kind)
                    }
                }
                .pickerStyle(.radioGroup)
                Text("Built-in runs the model on this Mac: no account, no other app, and no network once the model is downloaded.")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
                if !settings.hasCredentials(for: settings.provider) {
                    SettingsLink { Text("Add the account or API key in Settings…") }
                }
            }
            .formStyle(.grouped)
        case .model:
            if local {
                Form { BuiltInModelsView() }.formStyle(.grouped)
            } else {
                Text("Only the built-in provider needs a model on this Mac; skip this step.")
                    .foregroundStyle(Theme.textMuted)
            }
        case .gpu:
            Form {
                Toggle("Use the GPU (Metal)", isOn: $useGPU)
                if let gpuName {
                    Label(gpuName, systemImage: "cpu")
                } else {
                    Label("No Metal GPU: the CPU runs the model.", systemImage: "exclamationmark.triangle")
                }
                Text("The GPU runs the model’s matrix products; it is much faster on Apple Silicon.")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
            }
            .formStyle(.grouped)
        case .test:
            VStack(alignment: .leading, spacing: 12) {
                Text("Asks the chosen model for a short requirements summary of a 5 V LED indicator.")
                    .foregroundStyle(Theme.textMuted)
                Button("Run Test") { runTest() }
                    .disabled(test == .running || !done(.provider) || !done(.model))
                switch test {
                case .idle: EmptyView()
                case .running: ProgressView().controlSize(.small)
                case .ok(let message): Label(message, systemImage: "checkmark.circle.fill").foregroundStyle(Theme.skyBlue)
                case .failed(let message): Label(message, systemImage: "xmark.octagon.fill").foregroundStyle(Theme.error)
                }
            }
        case .design:
            VStack(alignment: .leading, spacing: 12) {
                Text("Describe a circuit in Prompt Studio: the agents plan it, place and route it, and the checks verify it.")
                    .foregroundStyle(Theme.textMuted)
                Button("Open Prompt Studio") {
                    store.workspace = .promptStudio
                    designed = true
                }
                .disabled(!done(.test))
            }
        }
    }

    private var mcpPage: some View {
        VStack(alignment: .leading, spacing: 8) {
            VStack(alignment: .leading, spacing: 4) {
                Text("Let an outside AI agent (Claude Desktop, Claude Code, Cursor, VS Code …) work on the open design:")
                Text("1. Turn on the server below (it listens on this Mac only).")
                Text("2. Copy your client’s configuration into it and restart the client.")
                Text("3. Ask the agent to list SiEDA’s tools — this tab shows ✓ after its first call.")
            }
            .foregroundStyle(Theme.textMuted)
            MCPSettingsView()
        }
    }

    private func runTest() {
        test = .running
        let provider = settings.makeProvider()
        let start = Date()
        Task { @MainActor in
            do {
                let text = try await provider.complete(AgentPrompts.analystRequest(brief: "A 5 V LED indicator."))
                let spec = try JSONExtraction.decode(RequirementsSpec.self, from: text)
                test = .ok("\(provider.modelName): “\(spec.title)” · \(String(format: "%.1f", Date().timeIntervalSince(start))) s")
            } catch {
                test = .failed(error.localizedDescription)
            }
        }
    }
}
