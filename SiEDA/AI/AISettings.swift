import Foundation
import Security

/// Minimal Keychain wrapper for API keys (never stored in UserDefaults or project files).
enum KeychainStore {
    private static let service = "com.sieda.SiEDA.ai-keys"

    static func read(_ account: String) -> String {
        let query: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
            kSecReturnData as String: true,
            kSecMatchLimit as String: kSecMatchLimitOne,
        ]
        var item: CFTypeRef?
        guard SecItemCopyMatching(query as CFDictionary, &item) == errSecSuccess, let data = item as? Data else { return "" }
        return String(decoding: data, as: UTF8.self)
    }

    static func write(_ value: String, for account: String) {
        let base: [String: Any] = [
            kSecClass as String: kSecClassGenericPassword,
            kSecAttrService as String: service,
            kSecAttrAccount as String: account,
        ]
        SecItemDelete(base as CFDictionary)
        guard !value.isEmpty else { return }
        var add = base
        add[kSecValueData as String] = Data(value.utf8)
        add[kSecAttrAccessible as String] = kSecAttrAccessibleAfterFirstUnlock
        SecItemAdd(add as CFDictionary, nil)
    }
}

/// User-selectable AI configuration. Claude is the default provider.
@MainActor
final class AISettings: ObservableObject {
    /// Master switch. When off SiEDA is a classic manual EDA tool: no Prompt Studio, no network calls,
    /// datasheets are parsed offline.
    @Published var aiEnabled: Bool {
        didSet { defaults.set(aiEnabled, forKey: "ai.enabled") }
    }
    @Published var provider: AIProviderKind {
        didSet { defaults.set(provider.rawValue, forKey: "ai.provider") }
    }
    @Published var models: [AIProviderKind: String] {
        didSet { persistModels() }
    }
    @Published var baseURLs: [AIProviderKind: String] {
        didSet { persistBaseURLs() }
    }
    @Published var claudeEffort: String {
        didSet { defaults.set(claudeEffort, forKey: "ai.claude.effort") }
    }
    /// Run the Design Review agent loop after generation (ERC + simulation feedback).
    @Published var enableReviewAgent: Bool {
        didSet { defaults.set(enableReviewAgent, forKey: "ai.review") }
    }
    @Published var maxReviewRounds: Int {
        didSet { defaults.set(maxReviewRounds, forKey: "ai.reviewRounds") }
    }
    /// How each provider is signed in to (API key, or a browser sign-in).
    @Published var authModes: [AIProviderKind: AIAuthMode] {
        didSet { for (kind, mode) in authModes { defaults.set(mode.rawValue, forKey: "ai.auth.\(kind.rawValue)") } }
    }
    /// Browser sign-ins that completed (Claude Console, Google Cloud); the tokens themselves stay with the CLIs.
    @Published private(set) var signedIn: Set<AIAuthMode> {
        didSet { defaults.set(signedIn.map(\.rawValue), forKey: "ai.signedIn") }
    }
    @Published var ssoConfiguration: OIDCConfiguration {
        didSet { if let data = try? JSONEncoder().encode(ssoConfiguration) { defaults.set(data, forKey: "ai.sso") } }
    }
    /// Google Cloud project and Vertex AI region used by "Sign in with Google".
    @Published var googleProject: String {
        didSet { defaults.set(googleProject, forKey: "ai.google.project") }
    }
    @Published var googleRegion: String {
        didSet { defaults.set(googleRegion, forKey: "ai.google.region") }
    }
    /// A sign-in in progress (its browser window is open), and the outcome of the last one.
    @Published private(set) var signingIn: AIProviderKind?
    @Published var signInMessage: String?

    /// Keys are cached in memory after the first Keychain read (not @Published: reads happen during view updates).
    private var keys: [AIProviderKind: String] = [:]

    private let defaults: UserDefaults

    init(defaults: UserDefaults = .standard) {
        self.defaults = defaults
        aiEnabled = defaults.object(forKey: "ai.enabled") as? Bool ?? true
        provider = AIProviderKind(rawValue: defaults.string(forKey: "ai.provider") ?? "") ?? .claude
        var models: [AIProviderKind: String] = [:]
        var urls: [AIProviderKind: String] = [:]
        for kind in AIProviderKind.allCases {
            models[kind] = defaults.string(forKey: "ai.model.\(kind.rawValue)") ?? kind.defaultModel
            urls[kind] = defaults.string(forKey: "ai.baseURL.\(kind.rawValue)") ?? kind.defaultBaseURL
        }
        self.models = models
        self.baseURLs = urls
        claudeEffort = defaults.string(forKey: "ai.claude.effort") ?? "high"
        enableReviewAgent = defaults.object(forKey: "ai.review") as? Bool ?? true
        maxReviewRounds = defaults.object(forKey: "ai.reviewRounds") as? Int ?? 2
        var modes: [AIProviderKind: AIAuthMode] = [:]
        for kind in AIProviderKind.allCases where !kind.authModes.isEmpty {
            let stored = AIAuthMode(rawValue: defaults.string(forKey: "ai.auth.\(kind.rawValue)") ?? "")
            // Existing installs keep using their API key; new ones start with the provider's browser sign-in.
            let hasKey = !KeychainStore.read(kind.rawValue).isEmpty
            let preferred = kind.authModes.first { $0.isAvailable } ?? .apiKey
            modes[kind] = stored.flatMap { kind.authModes.contains($0) ? $0 : nil } ?? (hasKey ? .apiKey : preferred)
        }
        authModes = modes
        signedIn = Set((defaults.stringArray(forKey: "ai.signedIn") ?? []).compactMap(AIAuthMode.init(rawValue:)))
        ssoConfiguration = (defaults.data(forKey: "ai.sso")).flatMap { try? JSONDecoder().decode(OIDCConfiguration.self, from: $0) }
            ?? OIDCConfiguration()
        googleProject = defaults.string(forKey: "ai.google.project") ?? ""
        googleRegion = defaults.string(forKey: "ai.google.region") ?? "us-central1"
    }

    func authMode(for kind: AIProviderKind) -> AIAuthMode { authModes[kind] ?? kind.authModes.first ?? .apiKey }

    /// Whether the provider's selected sign-in is complete (an API key counts as signed in).
    func isSignedIn(_ kind: AIProviderKind) -> Bool {
        switch authMode(for: kind) {
        case .apiKey, .browser: return !apiKey(for: kind).isEmpty
        case .claudeConsole, .googleCloud: return signedIn.contains(authMode(for: kind))
        case .sso: return OIDCAuth.load() != nil
        }
    }

    /// Runs the provider's browser sign-in: opens the login page in the default browser and waits for it.
    func signIn(_ kind: AIProviderKind) async {
        let mode = authMode(for: kind)
        guard mode.usesBrowser, signingIn == nil else { return }
        signingIn = kind
        signInMessage = nil
        defer { signingIn = nil }
        do {
            switch mode {
            case .claudeConsole:
                try await ClaudeConsoleAuth.signIn()
                signedIn.insert(.claudeConsole)
            case .googleCloud:
                try await GoogleCloudAuth.signIn()
                signedIn.insert(.googleCloud)
            case .browser:
                let key = try await OpenRouterAuth.signIn()
                setAPIKey(key, for: kind)
            case .sso:
                let tokens = try await OIDCAuth.signIn(ssoConfiguration)
                OIDCAuth.save(tokens)
                objectWillChange.send()
            case .apiKey:
                break
            }
            signInMessage = "Signed in to \(kind.shortName)."
        } catch {
            signInMessage = error.localizedDescription
        }
    }

    func signOut(_ kind: AIProviderKind) async {
        switch authMode(for: kind) {
        case .claudeConsole:
            await ClaudeConsoleAuth.signOut()
            signedIn.remove(.claudeConsole)
        case .googleCloud:
            await GoogleCloudAuth.signOut()
            signedIn.remove(.googleCloud)
        case .browser, .apiKey:
            setAPIKey("", for: kind)
        case .sso:
            OIDCAuth.save(nil)
            objectWillChange.send()
        }
        signInMessage = "Signed out of \(kind.shortName)."
    }

    func model(for kind: AIProviderKind) -> String { models[kind] ?? kind.defaultModel }
    func baseURL(for kind: AIProviderKind) -> String { baseURLs[kind] ?? kind.defaultBaseURL }

    func apiKey(for kind: AIProviderKind) -> String {
        if let cached = keys[kind] { return cached }
        let value = KeychainStore.read(kind.rawValue)
        keys[kind] = value
        return value
    }

    func setAPIKey(_ value: String, for kind: AIProviderKind) {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        objectWillChange.send()
        keys[kind] = trimmed
        KeychainStore.write(trimmed, for: kind.rawValue)
    }

    func hasCredentials(for kind: AIProviderKind) -> Bool {
        !kind.requiresAPIKey || isSignedIn(kind) || (kind == .openAI && baseURL(for: kind).contains("localhost"))
    }

    /// Builds the provider for the current selection, or a specific kind.
    func makeProvider(_ kind: AIProviderKind? = nil) -> AIProvider {
        let kind = kind ?? provider
        switch kind {
        case .claude:
            var claude = ClaudeProvider(apiKey: apiKey(for: .claude), model: model(for: .claude), effort: claudeEffort)
            if authMode(for: .claude) == .claudeConsole {
                claude.accessToken = { try await ClaudeConsoleAuth.accessToken() }
            }
            return claude
        case .openAI:
            var openAI = OpenAIProvider(apiKey: apiKey(for: .openAI), model: model(for: .openAI), baseURL: baseURL(for: .openAI))
            if authMode(for: .openAI) == .sso {
                let sso = ssoConfiguration
                openAI.accessToken = { try await OIDCAuth.accessToken(sso) }
            }
            return openAI
        case .gemini:
            var gemini = GeminiProvider(apiKey: apiKey(for: .gemini), model: model(for: .gemini))
            if authMode(for: .gemini) == .googleCloud {
                gemini.vertex = GeminiProvider.Vertex(project: googleProject.trimmingCharacters(in: .whitespaces),
                                                      region: googleRegion.trimmingCharacters(in: .whitespaces),
                                                      accessToken: { try await GoogleCloudAuth.accessToken() })
            }
            return gemini
        case .openRouter:
            return OpenAIProvider(apiKey: apiKey(for: .openRouter), model: model(for: .openRouter),
                                  baseURL: AIProviderKind.openRouter.defaultBaseURL, name: "OpenRouter",
                                  extraHeaders: ["X-Title": "SiEDA"])
        case .ollama:
            return OllamaProvider(model: model(for: .ollama), baseURL: baseURL(for: .ollama))
        case .offline:
            return OfflineProvider()
        }
    }

    private func persistModels() {
        for (kind, model) in models { defaults.set(model, forKey: "ai.model.\(kind.rawValue)") }
    }

    private func persistBaseURLs() {
        for (kind, url) in baseURLs { defaults.set(url, forKey: "ai.baseURL.\(kind.rawValue)") }
    }
}
