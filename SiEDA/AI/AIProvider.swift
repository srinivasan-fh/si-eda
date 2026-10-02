import Foundation

/// A file sent alongside a prompt (datasheet PDF or a pinout screenshot).
struct AIAttachment {
    enum Kind: Equatable {
        case pdf
        case image(mimeType: String)
    }

    var kind: Kind
    var data: Data
    var fileName: String

    var mimeType: String {
        switch kind {
        case .pdf: return "application/pdf"
        case .image(let mime): return mime
        }
    }
}

/// A single structured-output request issued by an agent.
struct AIRequest {
    var system: String
    var prompt: String
    /// Identifies the schema (e.g. "design_plan"); also used by the offline provider to pick a template.
    var schemaName: String
    var schema: [String: Any]
    var maxTokens: Int = 16_000
    var attachments: [AIAttachment] = []
}

enum AIProviderError: LocalizedError {
    case missingAPIKey(String)
    case http(status: Int, body: String)
    case invalidResponse(String)
    case refused(String)
    case truncated
    case network(String)

    var errorDescription: String? {
        switch self {
        case .missingAPIKey(let provider):
            return "No API key configured for \(provider). Add one in Settings → AI Models."
        case .http(let status, let body):
            return "The AI service returned HTTP \(status): \(body.prefix(400))"
        case .invalidResponse(let message):
            return message
        case .refused(let message):
            return "The model declined this request: \(message)"
        case .truncated:
            return "The model response was cut off (max tokens reached). Try a smaller design or raise the limit."
        case .network(let message):
            return "Network error: \(message)"
        }
    }
}

protocol AIProvider {
    var displayName: String { get }
    var modelName: String { get }
    /// Whether the provider reads PDF attachments natively (otherwise callers send extracted text).
    var acceptsPDF: Bool { get }
    /// Whether the provider reads image attachments.
    var acceptsImages: Bool { get }
    /// Returns the model's JSON text for `request` (conforming to `request.schema` when supported).
    func complete(_ request: AIRequest) async throws -> String
}

/// Supported model back-ends. Claude is the default.
enum AIProviderKind: String, CaseIterable, Identifiable, Codable {
    case claude
    case openAI
    case gemini
    case openRouter
    case ollama
    case offline

    var id: String { rawValue }

    var displayName: String {
        switch self {
        case .claude: return "Anthropic Claude"
        case .openAI: return "OpenAI / Compatible"
        case .gemini: return "Google Gemini"
        case .openRouter: return "OpenRouter (Claude, GPT, Gemini…)"
        case .ollama: return "Ollama (Local)"
        case .offline: return "Offline Designer"
        }
    }

    var shortName: String {
        switch self {
        case .claude: return "Claude"
        case .openAI: return "OpenAI"
        case .gemini: return "Gemini"
        case .openRouter: return "OpenRouter"
        case .ollama: return "Ollama"
        case .offline: return "Offline"
        }
    }

    var systemImage: String {
        switch self {
        case .claude: return "sparkles"
        case .openAI: return "brain"
        case .gemini: return "diamond"
        case .openRouter: return "arrow.triangle.branch"
        case .ollama: return "desktopcomputer"
        case .offline: return "wand.and.stars"
        }
    }

    var requiresAPIKey: Bool { self == .claude || self == .openAI || self == .gemini || self == .openRouter }
    var supportsBaseURL: Bool { self == .openAI || self == .ollama }

    var defaultModel: String {
        switch self {
        case .claude: return "claude-opus-5-5"
        case .openAI: return "gpt-4.1"
        case .gemini: return "gemini-2.5-pro"
        case .openRouter: return "openrouter/auto"
        case .ollama: return "llama3.1"
        case .offline: return "templates"
        }
    }

    /// Suggestions shown in the model picker; any model id can also be typed in.
    var suggestedModels: [String] {
        switch self {
        case .claude: return ["claude-opus-5-5", "claude-fable-5-1", "claude-sonnet-5-5", "claude-haiku-4-5"]
        case .openAI: return ["gpt-4.1", "gpt-4o", "o4-mini"]
        case .gemini: return ["gemini-2.5-pro", "gemini-2.5-flash"]
        case .openRouter: return ["openrouter/auto"]
        case .ollama: return ["llama3.1", "qwen2.5", "mistral"]
        case .offline: return ["templates"]
        }
    }

    var defaultBaseURL: String {
        switch self {
        case .openAI: return "https://api.openai.com/v1"
        case .openRouter: return "https://openrouter.ai/api/v1"
        case .ollama: return "http://localhost:11434"
        default: return ""
        }
    }
}

extension AIProvider {
    var acceptsPDF: Bool { false }
    var acceptsImages: Bool { false }
}

// MARK: - HTTP helper shared by the network providers

enum AIHTTP {
    static let session: URLSession = {
        let config = URLSessionConfiguration.default
        // Non-streaming structured output: nothing arrives until the whole answer (with thinking) is done, which
        // can take many minutes at high effort.
        config.timeoutIntervalForRequest = 1800
        config.timeoutIntervalForResource = 3600
        return URLSession(configuration: config)
    }()

    static func postJSON(url: URL, headers: [String: String], body: [String: Any]) async throws -> [String: Any] {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "content-type")
        for (key, value) in headers { request.setValue(value, forHTTPHeaderField: key) }
        request.httpBody = try JSONSerialization.data(withJSONObject: body, options: [])

        let data: Data
        let response: URLResponse
        do {
            (data, response) = try await session.data(for: request)
        } catch {
            throw AIProviderError.network(error.localizedDescription)
        }
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status) else {
            throw AIProviderError.http(status: status, body: String(decoding: data, as: UTF8.self))
        }
        guard let object = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw AIProviderError.invalidResponse("The AI service returned a non-JSON response.")
        }
        return object
    }
}
