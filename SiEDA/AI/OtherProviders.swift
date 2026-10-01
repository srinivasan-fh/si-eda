import Foundation

/// OpenAI Chat Completions, or any OpenAI-compatible endpoint (Azure OpenAI proxies, LM Studio, vLLM, …).
struct OpenAIProvider: AIProvider {
    var apiKey: String
    var model: String
    var baseURL: String

    var displayName: String { "OpenAI" }
    var modelName: String { model }

    func complete(_ request: AIRequest) async throws -> String {
        let isLocal = baseURL.contains("localhost") || baseURL.contains("127.0.0.1")
        guard isLocal || !apiKey.trimmingCharacters(in: .whitespaces).isEmpty else {
            throw AIProviderError.missingAPIKey("OpenAI")
        }
        let root = baseURL.hasSuffix("/") ? String(baseURL.dropLast()) : baseURL
        guard let url = URL(string: root + "/chat/completions") else {
            throw AIProviderError.invalidResponse("Invalid base URL '\(baseURL)'.")
        }
        let body: [String: Any] = [
            "model": model,
            "messages": [
                ["role": "system", "content": request.system],
                ["role": "user", "content": request.prompt],
            ],
            "response_format": [
                "type": "json_schema",
                "json_schema": ["name": request.schemaName, "schema": request.schema, "strict": true] as [String: Any],
            ] as [String: Any],
        ]
        var headers: [String: String] = [:]
        if !apiKey.isEmpty { headers["authorization"] = "Bearer \(apiKey)" }
        let json = try await AIHTTP.postJSON(url: url, headers: headers, body: body)
        let choices = json["choices"] as? [[String: Any]] ?? []
        guard let message = choices.first?["message"] as? [String: Any] else {
            throw AIProviderError.invalidResponse("OpenAI returned no choices.")
        }
        if let refusal = message["refusal"] as? String, !refusal.isEmpty { throw AIProviderError.refused(refusal) }
        if (choices.first?["finish_reason"] as? String) == "length" { throw AIProviderError.truncated }
        guard let content = message["content"] as? String, !content.isEmpty else {
            throw AIProviderError.invalidResponse("OpenAI returned an empty message.")
        }
        return content
    }
}

/// Google Gemini `generateContent` with JSON output mode.
struct GeminiProvider: AIProvider {
    var apiKey: String
    var model: String

    var displayName: String { "Gemini" }
    var modelName: String { model }

    func complete(_ request: AIRequest) async throws -> String {
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !key.isEmpty else { throw AIProviderError.missingAPIKey("Google Gemini") }
        guard let url = URL(string: "https://generativelanguage.googleapis.com/v1beta/models/\(model):generateContent") else {
            throw AIProviderError.invalidResponse("Invalid Gemini model name '\(model)'.")
        }
        // Gemini's schema dialect differs from JSON Schema, so the schema travels in the prompt instead.
        let schemaText = (try? JSONSerialization.data(withJSONObject: request.schema, options: [.prettyPrinted, .sortedKeys]))
            .map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        let prompt = request.prompt + "\n\nRespond with a single JSON object that conforms to this JSON Schema:\n" + schemaText
        let body: [String: Any] = [
            "systemInstruction": ["parts": [["text": request.system]]],
            "contents": [["role": "user", "parts": [["text": prompt]]]],
            "generationConfig": ["responseMimeType": "application/json", "maxOutputTokens": request.maxTokens] as [String: Any],
        ]
        let json = try await AIHTTP.postJSON(url: url, headers: ["x-goog-api-key": key], body: body)
        let candidates = json["candidates"] as? [[String: Any]] ?? []
        guard let first = candidates.first else {
            throw AIProviderError.invalidResponse("Gemini returned no candidates.")
        }
        if (first["finishReason"] as? String) == "MAX_TOKENS" { throw AIProviderError.truncated }
        if (first["finishReason"] as? String) == "SAFETY" { throw AIProviderError.refused("safety filter") }
        let parts = (first["content"] as? [String: Any])?["parts"] as? [[String: Any]] ?? []
        let text = parts.compactMap { $0["text"] as? String }.joined()
        guard !text.isEmpty else { throw AIProviderError.invalidResponse("Gemini returned an empty response.") }
        return text
    }
}

/// Local models served by Ollama (`/api/chat` with JSON-schema constrained output).
struct OllamaProvider: AIProvider {
    var model: String
    var baseURL: String

    var displayName: String { "Ollama" }
    var modelName: String { model }

    func complete(_ request: AIRequest) async throws -> String {
        let root = baseURL.hasSuffix("/") ? String(baseURL.dropLast()) : baseURL
        guard let url = URL(string: root + "/api/chat") else {
            throw AIProviderError.invalidResponse("Invalid Ollama URL '\(baseURL)'.")
        }
        let body: [String: Any] = [
            "model": model,
            "stream": false,
            "format": request.schema,
            "messages": [
                ["role": "system", "content": request.system],
                ["role": "user", "content": request.prompt],
            ],
        ]
        let json = try await AIHTTP.postJSON(url: url, headers: [:], body: body)
        guard let message = json["message"] as? [String: Any], let content = message["content"] as? String,
              !content.isEmpty else {
            throw AIProviderError.invalidResponse("Ollama returned an empty response.")
        }
        return content
    }
}
