import SwiftUI

struct SettingsView: View {
    var body: some View {
        TabView {
            AIModelSettings()
                .tabItem { Label("AI Models", systemImage: "sparkles") }
            AppearanceSettings()
                .tabItem { Label("Appearance", systemImage: "paintpalette") }
            LanguageSettingsView()
                .tabItem { Label("Language", systemImage: "globe") }
            SupplierSettingsView()
                .tabItem { Label("Suppliers", systemImage: "shippingbox") }
            MCPSettingsView()
                .tabItem { Label("AI Access (MCP)", systemImage: "dot.radiowaves.left.and.right") }
            AboutSettings()
                .tabItem { Label("About", systemImage: "info.circle") }
        }
        .frame(width: 680, height: 600)
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
                    if !editingKind.authModes.isEmpty {
                        AccountSettings(kind: editingKind, keyDraft: $keyDraft) { testState = .idle }
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
        if settings.authMode(for: editingKind) == .apiKey, !keyDraft.isEmpty, keyDraft != settings.apiKey(for: editingKind) {
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

/// Sign-in for one provider: a button for each browser sign-in it offers, then an API key as the alternative.
private struct AccountSettings: View {
    @EnvironmentObject private var settings: AISettings
    let kind: AIProviderKind
    @Binding var keyDraft: String
    var changed: () -> Void

    var body: some View {
        let inUse = settings.authMode(for: kind)
        LabeledContent("Signed in with") {
            Text(settings.isSignedIn(kind) ? inUse.title.replacingOccurrences(of: "Sign in with ", with: "") : "Not signed in")
                .foregroundStyle(settings.isSignedIn(kind) ? Theme.skyBlue : Theme.textMuted)
        }
        ForEach(kind.authModes.filter(\.usesBrowser)) { mode in
            signInRow(mode, inUse: inUse == mode)
        }
        VStack(alignment: .leading, spacing: 6) {
            Text(kind.authModes.contains { $0.usesBrowser } ? "Or use an API key" : "API key")
                .font(.callout.weight(.semibold))
            HStack {
                SecureField("API key", text: $keyDraft, prompt: Text("Paste API key"))
                    .textFieldStyle(.roundedBorder)
                    .labelsHidden()
                Button("Save Key") {
                    settings.setAPIKey(keyDraft, for: kind)
                    if !keyDraft.isEmpty { settings.authModes[kind] = .apiKey }
                    changed()
                }
                .disabled(keyDraft.isEmpty && settings.apiKey(for: kind).isEmpty)
            }
            Text(settings.apiKey(for: kind).isEmpty
                 ? "No key stored."
                 : "Key stored in the macOS Keychain (••••\(String(settings.apiKey(for: kind).suffix(4))))\(inUse == .apiKey ? " — in use." : ".")")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
        }
        if let message = settings.signInMessage {
            Text(message).font(.caption).foregroundStyle(Theme.textSecondary).fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder
    private func signInRow(_ mode: AIAuthMode, inUse: Bool) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            if mode == .googleCloud {
                TextField("Google Cloud project ID", text: $settings.googleProject, prompt: Text("my-gcp-project"))
                TextField("Vertex AI region", text: $settings.googleRegion, prompt: Text("us-central1"))
            }
            if mode == .sso {
                TextField("Issuer URL", text: $settings.ssoConfiguration.issuer, prompt: Text("https://login.example.com/oauth2/default"))
                TextField("Client ID", text: $settings.ssoConfiguration.clientID, prompt: Text("public client id"))
                TextField("Scopes", text: $settings.ssoConfiguration.scopes)
                TextField("Audience (optional)", text: $settings.ssoConfiguration.audience)
            }
            HStack(spacing: 10) {
                let signedIn = settings.isSignedIn(kind, with: mode)
                Button {
                    changed()
                    settings.beginSignIn(kind, with: mode)
                } label: {
                    Label(signedIn ? mode.title.replacingOccurrences(of: "Sign in", with: "Sign in again") + "…" : mode.title + "…",
                          systemImage: "person.badge.key.fill")
                }
                .buttonStyle(.borderedProminent)
                .controlSize(.large)
                .disabled(settings.signingIn != nil || !mode.isAvailable || (mode == .sso && !settings.ssoConfiguration.isComplete))
                if settings.signingIn == kind && settings.authMode(for: kind) == mode {
                    ProgressView().controlSize(.small)
                    Text("Finish the login in your browser…").font(.caption).foregroundStyle(Theme.textMuted)
                    Button("Cancel") { settings.cancelSignIn() }
                } else if signedIn {
                    Label(inUse ? "Signed in · in use" : "Signed in", systemImage: "checkmark.seal.fill")
                        .font(.caption)
                        .foregroundStyle(Theme.skyBlue)
                    Button("Sign Out") {
                        changed()
                        Task { await settings.signOut(kind, from: mode) }
                    }
                    .disabled(settings.signingIn != nil)
                }
                Spacer(minLength: 0)
            }
            if mode == .sso && !settings.ssoConfiguration.isComplete {
                Label("Enter your identity provider's issuer URL and SiEDA's client ID above to enable this button (ask your IT administrator).",
                      systemImage: "info.circle")
                    .font(.caption)
                    .foregroundStyle(Theme.textSecondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if !mode.isAvailable {
                Label(CommandLineTool.sandboxMessage, systemImage: "exclamationmark.triangle")
                    .font(.caption)
                    .foregroundStyle(Theme.warning)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Text(Self.explanation(mode))
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
        }
        .padding(.vertical, 4)
    }

    static func explanation(_ mode: AIAuthMode) -> String {
        switch mode {
        case .claudeConsole:
            return "Opens the Claude Console login in your browser through the official Anthropic CLI (ant). Choose your organisation and workspace there. SiEDA then asks the CLI for short-lived access tokens, and usage is billed to that workspace. No API key is stored in SiEDA. Claude.ai Pro/Max chat subscriptions cannot be used by other apps."
        case .googleCloud:
            return "Opens Google sign-in in your browser through the official Google Cloud CLI (gcloud). SiEDA calls Gemini on Vertex AI in the project above with your account's access token. The Vertex AI API must be enabled in that project."
        case .browser:
            return "Opens OpenRouter in your browser. Approve SiEDA there and OpenRouter issues it a key, kept in the macOS Keychain. One account gives access to Claude, GPT, Gemini and other models; see openrouter.ai/models for model IDs."
        case .sso:
            return "Signs in with your organisation's identity provider (OpenID Connect: Okta, Microsoft Entra ID, Google Workspace, Keycloak…) for an OpenAI-compatible AI gateway at the base URL above that accepts its access tokens. Register SiEDA as a public (native) client with the redirect URI http://127.0.0.1/callback (any port). Tokens are kept in the Keychain and refreshed automatically."
        case .apiKey:
            return ""
        }
    }
}

private struct AppearanceSettings: View {
    @AppStorage("appearance") private var appearance = AppearancePreference.dark.rawValue
    @AppStorage("showSplashScreen") private var showSplashScreen = true
    /// DesignStore.placeInteractivelyKey (Update PCB's "place new parts").
    @AppStorage("pcb.placeNewPartsInteractively") private var placeNewPartsInteractively = true
    @AppStorage(FocusMode.hidesPanelsKey) private var fullScreenHidesPanels = true
    @State private var customisingSchematic = false

    var body: some View {
        Form {
            Picker("Appearance", selection: $appearance) {
                ForEach(AppearancePreference.allCases) { Text($0.title).tag($0.rawValue) }
            }
            .pickerStyle(.segmented)
            Picker("App Theme", selection: Binding(get: { AppTheme.current }, set: AppTheme.select)) {
                ForEach(AppTheme.allCases) { Text(LocalizedStringKey($0.title)).tag($0) }
            }
            Text("Colours every window: panels, accents and text, and picks the matching schematic scheme. Copper, pads and error colours stay standard.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            Text("SiEDA is designed for dark mode with a blue engineering palette; editors keep their dark canvases in every mode.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            Toggle("Show splash screen at launch", isOn: $showSplashScreen)
            Text("The splash preloads the component library and reference designs for five seconds, then opens the main window. Click it or press Esc to skip once loading is done.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            schematicSection
            pcbSection
        }
        .formStyle(.grouped)
        .sheet(isPresented: $customisingSchematic) { SchematicThemeEditor() }
    }

    /// Colour scheme and grid of the schematic canvas (also in the editor's options bar and the View menu).
    private var schematicSection: some View {
        Section {
            SchematicAppearanceMenu(onCustomise: { customisingSchematic = true })
            Text("The schematic PDF keeps its print colours.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
        } header: {
            Text("Schematic Canvas")
        }
    }

    /// Update PCB: new parts placed one by one at the cursor, or left where Auto Place puts them.
    private var pcbSection: some View {
        Section {
            Toggle("Full screen hides the panels (focus mode)", isOn: $fullScreenHidesPanels)
            Text("F11, ⌃⌘F or Fn-F: full screen with only the editor and its tools. Point at the top edge of an editor to show its options bar.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            Toggle("Place new parts interactively after Update PCB", isOn: $placeNewPartsInteractively)
            Text("After Update PCB the PCB editor opens with each new part at the cursor: R rotates, F flips, a click places it and Esc leaves it where Auto Place put it. Off: new parts stay where Auto Place puts them.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
        } header: {
            Text("PCB Editor")
        }
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
