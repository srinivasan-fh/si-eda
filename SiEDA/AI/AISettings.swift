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
        !kind.requiresAPIKey || !apiKey(for: kind).isEmpty || (kind == .openAI && baseURL(for: kind).contains("localhost"))
    }

    /// Builds the provider for the current selection, or a specific kind.
    func makeProvider(_ kind: AIProviderKind? = nil) -> AIProvider {
        let kind = kind ?? provider
        switch kind {
        case .claude:
            return ClaudeProvider(apiKey: apiKey(for: .claude), model: model(for: .claude), effort: claudeEffort)
        case .openAI:
            return OpenAIProvider(apiKey: apiKey(for: .openAI), model: model(for: .openAI), baseURL: baseURL(for: .openAI))
        case .gemini:
            return GeminiProvider(apiKey: apiKey(for: .gemini), model: model(for: .gemini))
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
