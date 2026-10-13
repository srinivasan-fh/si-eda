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
            ShortcutSettings()
                .tabItem { Label("Shortcuts", systemImage: "keyboard") }
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

                if editingKind == .builtIn {
                    BuiltInModelsView()
                } else if editingKind == .offline {
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
struct AccountSettings: View {
    @EnvironmentObject private var settings: AISettings
    let kind: AIProviderKind
    @Binding var keyDraft: String
    var changed: () -> Void

    init(kind: AIProviderKind, keyDraft: Binding<String>, changed: @escaping () -> Void) {
        self.kind = kind
        _keyDraft = keyDraft
        self.changed = changed
    }

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
    @AppStorage(DesignStore.autosaveKey) private var autosave = false
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
            Toggle("Save projects automatically", isOn: $autosave)
            Text("A project that has a file is saved to it a few seconds after each change (a recovery copy is always kept).")
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

/// User-assignable key equivalents of the main menu commands (Settings → Shortcuts), kept in the user defaults.
final class ShortcutStore: ObservableObject {
    static let shared = ShortcutStore()
    struct Command: Identifiable {
        let id: String
        let title: String
        let standard: String  // "cmd+shift+k"; "" = none
    }
    static let commands: [Command] = [
        .init(id: "newProject", title: "New Project", standard: "cmd+n"),
        .init(id: "open", title: "Open…", standard: "cmd+o"),
        .init(id: "save", title: "Save", standard: "cmd+s"),
        .init(id: "saveAs", title: "Save As…", standard: "cmd+shift+s"),
        .init(id: "fabPackage", title: "Export Fabrication Package…", standard: "cmd+shift+e"),
        .init(id: "palette", title: "Command Palette…", standard: "cmd+k"),
        .init(id: "zoomFit", title: "Zoom to Fit", standard: "cmd+0"),
        .init(id: "erc", title: "Run Electrical Rule Check", standard: "cmd+shift+k"),
        .init(id: "dcOp", title: "DC Operating Point", standard: "cmd+shift+d"),
        .init(id: "autoPlace", title: "Auto-Place Footprints", standard: ""),
        .init(id: "autoRoute", title: "Auto Route Board", standard: "cmd+shift+r"),
        .init(id: "drc", title: "Run Design Rule Check", standard: ""),
    ]
    /// Single-key canvas tools ("pcb." / "sch." canvas); the canvases switch on the standard key.
    static let toolKeys: [Command] = [
        .init(id: "sch.select", title: "Schematic: Select", standard: "v"),
        .init(id: "sch.wire", title: "Schematic: Wire", standard: "w"),
        .init(id: "sch.bus", title: "Schematic: Bus", standard: "b"),
        .init(id: "sch.noConnect", title: "Schematic: No-Connect", standard: "q"),
        .init(id: "sch.label", title: "Schematic: Net Label", standard: "l"),
        .init(id: "sch.ground", title: "Schematic: Ground", standard: "g"),
        .init(id: "sch.place", title: "Schematic: Place Part", standard: "p"),
        .init(id: "sch.text", title: "Schematic: Text", standard: "t"),
        .init(id: "sch.pan", title: "Schematic: Pan", standard: "h"),
        .init(id: "sch.rotate", title: "Schematic: Rotate", standard: "r"),
        .init(id: "sch.mirrorX", title: "Schematic: Mirror Horizontally", standard: "x"),
        .init(id: "sch.mirrorY", title: "Schematic: Mirror Vertically", standard: "y"),
        .init(id: "sch.corner", title: "Schematic: Wire Corner", standard: "/"),
        .init(id: "pcb.select", title: "PCB: Select (Via While Routing)", standard: "v"),
        .init(id: "pcb.route", title: "PCB: Route", standard: "x"),
        .init(id: "pcb.tune", title: "PCB: Length Tuning", standard: "t"),
        .init(id: "pcb.pan", title: "PCB: Pan", standard: "h"),
        .init(id: "pcb.rotate", title: "PCB: Rotate", standard: "r"),
        .init(id: "pcb.flip", title: "PCB: Flip Side", standard: "f"),
        .init(id: "pcb.refill", title: "PCB: Refill Pours", standard: "b"),
    ]
    private static let key = "app.shortcuts"
    @Published private(set) var overrides: [String: String]

    init() { overrides = UserDefaults.standard.dictionary(forKey: Self.key) as? [String: String] ?? [:] }

    /// The text of a command's shortcut ("cmd+shift+k"), or "" for none.
    func text(_ id: String) -> String { overrides[id] ?? (Self.commands + Self.toolKeys).first { $0.id == id }?.standard ?? "" }

    /// nil = back to the standard one.
    func set(_ id: String, _ text: String?) {
        overrides[id] = text
        UserDefaults.standard.set(overrides, forKey: Self.key)
    }

    func shortcut(_ id: String) -> KeyboardShortcut? { Self.parse(text(id)) }

    /// Canvas key presets for people coming from another tool: overrides of the standard keys (SiEDA = none).
    static let toolPresetNames = ["SiEDA", "KiCad"]
    static let toolPresets: [String: [String: String]] = [
        "SiEDA": [:],
        "KiCad": ["sch.place": "a", "sch.ground": "p", "sch.select": "", "sch.pan": "", "pcb.tune": "7", "pcb.pan": ""],
    ]

    func applyToolPreset(_ keys: [String: String]) {
        for tool in Self.toolKeys { overrides[tool.id] = keys[tool.id] }
        UserDefaults.standard.set(overrides, forKey: Self.key)
    }

    /// The standard key of the `canvas` tool ("sch", "pcb") the typed key is set to; nil when no tool has it.
    func toolKey(_ characters: String, canvas: String) -> KeyEquivalent? {
        let typed = characters.lowercased()
        guard !typed.isEmpty, let tool = Self.toolKeys.first(where: { $0.id.hasPrefix(canvas + ".") && text($0.id).lowercased() == typed })
        else { return nil }
        return KeyEquivalent(Character(tool.standard))
    }

    /// "cmd+shift+k" (or ⌘⇧K-style symbols) → a shortcut; nil when there is no single key.
    static func parse(_ text: String) -> KeyboardShortcut? {
        var modifiers: EventModifiers = []
        var key: Character?
        for part in text.lowercased().replacingOccurrences(of: "⌘", with: "cmd+").replacingOccurrences(of: "⇧", with: "shift+")
            .replacingOccurrences(of: "⌥", with: "opt+").replacingOccurrences(of: "⌃", with: "ctrl+").split(separator: "+") {
            switch part.trimmingCharacters(in: .whitespaces) {
            case "cmd", "command": modifiers.insert(.command)
            case "shift": modifiers.insert(.shift)
            case "opt", "option", "alt": modifiers.insert(.option)
            case "ctrl", "control": modifiers.insert(.control)
            case let k where k.count == 1: key = k.first
            default: return nil
            }
        }
        return key.map { KeyboardShortcut(KeyEquivalent($0), modifiers: modifiers) }
    }

    /// Commands that share a shortcut with another one.
    var clashes: Set<String> {
        var seen: [String: String] = [:], out = Set<String>()
        for c in Self.commands + Self.toolKeys {  // menus clash with menus, a canvas's tools with each other
            let t = text(c.id).lowercased()
            guard !t.isEmpty else { continue }
            let key = (c.id.contains(".") ? String(c.id.prefix { $0 != "." }) : "") + "|" + t
            if let other = seen[key] { out.formUnion([other, c.id]) } else { seen[key] = c.id }
        }
        return out
    }
}

/// Settings → Shortcuts: type a command's keys ("cmd+shift+k"), empty for none; Reset restores the standard one.
struct ShortcutSettings: View {
    @ObservedObject private var shortcuts = ShortcutStore.shared

    var body: some View {
        Form {
            ForEach([ShortcutStore.commands, ShortcutStore.toolKeys], id: \.first?.id) { group in
                Section(LocalizedStringKey(group.first?.id.contains(".") == true ? "Canvas Tool Keys" : "Menu Commands")) {
                    ForEach(group) { command in
                        HStack {
                            Text(LocalizedStringKey(command.title))
                            Spacer()
                            if shortcuts.clashes.contains(command.id) {
                                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                                    .help("Another command uses the same keys")
                            }
                            TextField("", text: Binding(get: { shortcuts.text(command.id) }, set: { shortcuts.set(command.id, $0) }),
                                      prompt: Text(verbatim: command.standard.isEmpty ? "—" : command.standard))
                                .frame(width: 150)
                                .font(.body.monospaced())
                            Button("Reset") { shortcuts.set(command.id, nil) }
                                .disabled(shortcuts.overrides[command.id] == nil)
                        }
                    }
                }
            }
            Text("Type the keys as cmd, shift, opt and ctrl plus one key, joined by +, e.g. cmd+shift+k. Leave empty for no shortcut.")
                .font(.caption).foregroundStyle(.secondary)
            Text("A canvas tool takes one key.").font(.caption).foregroundStyle(.secondary)
            Picker("Canvas Key Preset", selection: Binding(get: { "" }, set: { name in
                if let keys = ShortcutStore.toolPresets[name] { shortcuts.applyToolPreset(keys) }
            })) {
                Text("Choose…").tag("")
                ForEach(ShortcutStore.toolPresetNames, id: \.self) { Text(verbatim: $0).tag($0) }
            }
        }
        .formStyle(.grouped)
    }
}
