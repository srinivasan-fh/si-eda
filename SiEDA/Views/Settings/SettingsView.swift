import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            AIModelSettings()
                .tabItem { Label("AI Models", systemImage: "sparkles") }
            AppearanceSettings()
                .tabItem { Label("Appearance", systemImage: "paintpalette") }
            AboutSettings()
                .tabItem { Label("About", systemImage: "info.circle") }
        }
        .frame(width: 640, height: 520)
    }
}

/// Choose the agent model back-end; Claude is the default.
private struct AIModelSettings: View {
    @EnvironmentObject private var settings: AISettings
    @State private var editingKind: AIProviderKind = .claude
    @State private var keyDraft = ""
    @State private var testState: TestState = .idle

    enum TestState: Equatable {
        case idle, running, ok(String), failed(String)
    }

    var body: some View {
        Form {
            Section {
                Toggle(isOn: $settings.aiEnabled) {
                    VStack(alignment: .leading, spacing: 2) {
                        Text("Enable AI assistance")
                        Text("Off: SiEDA works as a classic manual EDA tool. Schematic capture, simulation, PCB routing, 3D, datasheet parsing and exports all run locally, and nothing is sent to any AI service.")
                            .font(.caption)
                            .foregroundStyle(Theme.textMuted)
                    }
                }
            } header: {
                Text("Mode")
            }

            Section {
                Picker("Default provider", selection: $settings.provider) {
                    ForEach(AIProviderKind.allCases) { kind in
                        Label(kind.displayName, systemImage: kind.systemImage).tag(kind)
                    }
                }
                Toggle("Design Review agent (ERC + simulation feedback loop)", isOn: $settings.enableReviewAgent)
                Stepper("Review rounds: \(settings.maxReviewRounds)", value: $settings.maxReviewRounds, in: 1...4)
                    .disabled(!settings.enableReviewAgent)
            } header: {
                Text("Agents")
            }
            .disabled(!settings.aiEnabled)

            Section {
                Picker("Configure", selection: $editingKind) {
                    ForEach(AIProviderKind.allCases) { kind in Text(kind.shortName).tag(kind) }
                }
                .pickerStyle(.segmented)

                if editingKind == .offline {
                    Text("The offline designer uses SiEDA's built-in reference circuits. No network or API key needed — ideal for demos and air-gapped labs.")
                        .foregroundStyle(Theme.textMuted)
                } else {
                    HStack {
                        TextField("Model", text: modelBinding)
                        Menu("Suggested") {
                            ForEach(editingKind.suggestedModels, id: \.self) { model in
                                Button(model) { settings.models[editingKind] = model }
                            }
                        }
                        .fixedSize()
                    }
                    if editingKind.supportsBaseURL {
                        TextField("Base URL", text: baseURLBinding)
                    }
                    if editingKind.requiresAPIKey || editingKind == .openAI {
                        HStack {
                            SecureField("API key", text: $keyDraft)
                            Button("Save Key") {
                                settings.setAPIKey(keyDraft, for: editingKind)
                                testState = .idle
                            }
                        }
                        Text(settings.apiKey(for: editingKind).isEmpty
                             ? "No key stored."
                             : "Key stored in the macOS Keychain (••••\(String(settings.apiKey(for: editingKind).suffix(4)))).")
                            .font(.caption)
                            .foregroundStyle(Theme.textMuted)
                    }
                    if editingKind == .claude {
                        Picker("Effort", selection: $settings.claudeEffort) {
                            ForEach(ClaudeProvider.efforts, id: \.self) { Text($0).tag($0) }
                        }
                        .pickerStyle(.segmented)
                        Text("Higher effort lets Claude reason longer about circuit topology and values. “high” is recommended.")
                            .font(.caption)
                            .foregroundStyle(Theme.textMuted)
                    }
                }

                HStack {
                    Button("Test Connection") { test() }
                        .disabled(testState == .running)
                    switch testState {
                    case .idle: EmptyView()
                    case .running: ProgressView().controlSize(.small)
                    case .ok(let message): Label(message, systemImage: "checkmark.circle.fill").foregroundStyle(Theme.skyBlue)
                    case .failed(let message): Label(message, systemImage: "xmark.octagon.fill").foregroundStyle(Theme.error).lineLimit(3)
                    }
                }
            } header: {
                Text("Model back-ends")
            }
            .disabled(!settings.aiEnabled)
        }
        .formStyle(.grouped)
        .onAppear {
            editingKind = settings.provider
            keyDraft = settings.apiKey(for: editingKind)
        }
        .onChange(of: editingKind) { _, kind in
            keyDraft = settings.apiKey(for: kind)
            testState = .idle
        }
    }

    private var modelBinding: Binding<String> {
        Binding(get: { settings.model(for: editingKind) }, set: { settings.models[editingKind] = $0 })
    }

    private var baseURLBinding: Binding<String> {
        Binding(get: { settings.baseURL(for: editingKind) }, set: { settings.baseURLs[editingKind] = $0 })
    }

    private func test() {
        testState = .running
        // Test the key as typed, saving it first (the provider reads the saved key).
        if editingKind.requiresAPIKey, !keyDraft.isEmpty, keyDraft != settings.apiKey(for: editingKind) {
            settings.setAPIKey(keyDraft, for: editingKind)
        }
        let provider = settings.makeProvider(editingKind)
        Task { @MainActor in
            do {
                let text = try await provider.complete(AgentPrompts.analystRequest(brief: "A 5 V LED indicator."))
                let spec = try JSONExtraction.decode(RequirementsSpec.self, from: text)
                testState = .ok("OK — \(provider.modelName) answered “\(spec.title)”")
            } catch {
                testState = .failed(error.localizedDescription)
            }
        }
    }
}

private struct AppearanceSettings: View {
    @AppStorage("appearance") private var appearance = AppearancePreference.dark.rawValue

    var body: some View {
        Form {
            Picker("Appearance", selection: $appearance) {
                ForEach(AppearancePreference.allCases) { Text($0.title).tag($0.rawValue) }
            }
            .pickerStyle(.segmented)
            Text("SiEDA is designed for dark mode with a blue engineering palette; editors keep their dark canvases in every mode.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
        }
        .formStyle(.grouped)
    }
}

private struct AboutSettings: View {
    var body: some View {
        VStack(spacing: 12) {
            Image(systemName: "cpu")
                .font(.system(size: 54, weight: .light))
                .foregroundStyle(LinearGradient(colors: [Theme.skyBlue, Theme.blue], startPoint: .top, endPoint: .bottom))
            Text("SiEDA").font(.largeTitle.weight(.bold)).foregroundStyle(Theme.textPrimary)
            Text("AI-native electronic design automation").foregroundStyle(Theme.textSecondary)
            Text("Core engine \(EDAEngine.coreVersion) · C++17 · SwiftUI · SceneKit").font(.caption).foregroundStyle(Theme.textMuted)
            Text("Schematic capture · SPICE-class simulation · PCB autorouting · DRC · 3D · Gerber/Excellon export")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
                .multilineTextAlignment(.center)
        }
        .padding(30)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
