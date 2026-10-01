import Foundation

/// Anthropic Claude via the Messages API (`POST /v1/messages`), the default SiEDA model provider.
///
/// Uses structured outputs (`output_config.format` with a JSON schema) so agent replies always decode
/// into SiEDA's design types, adaptive thinking with a configurable effort level, and server-side
/// refusal fallbacks on the models that support them.
struct ClaudeProvider: AIProvider {
    var apiKey: String
    var model: String
    /// One of: low, medium, high, xhigh, max.
    var effort: String
    var endpoint = URL(string: "https://api.anthropic.com/v1/messages")!

    var displayName: String { "Claude" }
    var modelName: String { model }
    var acceptsPDF: Bool { true }
    var acceptsImages: Bool { true }

    static let efforts = ["low", "medium", "high", "xhigh", "max"]

    struct Capabilities {
        var adaptiveThinking: Bool
        var effort: Bool
        var serverFallback: Bool

        init(model: String) {
            let legacy = model.hasPrefix("claude-haiku") || model.hasPrefix("claude-3")
            adaptiveThinking = !legacy
            effort = !legacy
            serverFallback = ["claude-fable-5-1", "claude-opus-5-5", "claude-opus-5", "claude-sonnet-5-5"].contains(model)
        }
    }

    /// Plain text, or document/image blocks followed by the text prompt.
    static func content(for request: AIRequest) -> Any {
        guard !request.attachments.isEmpty else { return request.prompt }
        var blocks: [[String: Any]] = request.attachments.map { attachment in
            let source: [String: Any] = ["type": "base64", "media_type": attachment.mimeType,
                                         "data": attachment.data.base64EncodedString()]
            switch attachment.kind {
            case .pdf: return ["type": "document", "source": source, "title": attachment.fileName]
            case .image: return ["type": "image", "source": source]
            }
        }
        blocks.append(["type": "text", "text": request.prompt])
        return blocks
    }

    func complete(_ request: AIRequest) async throws -> String {
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { throw AIProviderError.missingAPIKey("Anthropic Claude") }

        let caps = Capabilities(model: model)
        var body: [String: Any] = [
            "model": model,
            "max_tokens": request.maxTokens,
            "system": request.system,
            "messages": [["role": "user", "content": Self.content(for: request)] as [String: Any]],
        ]
        var outputConfig: [String: Any] = ["format": ["type": "json_schema", "schema": request.schema] as [String: Any]]
        if caps.effort { outputConfig["effort"] = effort }
        body["output_config"] = outputConfig
        if caps.adaptiveThinking { body["thinking"] = ["type": "adaptive"] }

        var headers = ["x-api-key": key, "anthropic-version": "2023-06-01"]
        if caps.serverFallback {
            // Re-runs a declined request on a suitable fallback model inside the same call.
            headers["anthropic-beta"] = "server-side-fallback-2026-07-01"
            body["fallbacks"] = "default"
        }

        let json = try await AIHTTP.postJSON(url: endpoint, headers: headers, body: body)

        switch json["stop_reason"] as? String {
        case "refusal":
            let details = json["stop_details"] as? [String: Any]
            let explanation = details?["explanation"] as? String
            throw AIProviderError.refused(explanation ?? "safety policy")
        case "max_tokens":
            throw AIProviderError.truncated
        default:
            break
        }

        let blocks = json["content"] as? [[String: Any]] ?? []
        let text = blocks
            .filter { ($0["type"] as? String) == "text" }
            .compactMap { $0["text"] as? String }
            .joined()
        guard !text.isEmpty else {
            throw AIProviderError.invalidResponse("Claude returned no text content.")
        }
        return text
    }
}
