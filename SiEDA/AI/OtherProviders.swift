import Foundation

/// OpenAI Chat Completions, or any OpenAI-compatible endpoint (Azure OpenAI proxies, LM Studio, vLLM, …).
struct OpenAIProvider: AIProvider {
    var apiKey: String
    var model: String
    var baseURL: String
    /// Organisation SSO: a current access token for the AI gateway, sent instead of the key.
    var accessToken: (() async throws -> String)? = nil
    var name = "OpenAI"
    var extraHeaders: [String: String] = [:]

    var displayName: String { name }
    var modelName: String { model }
    var acceptsImages: Bool { true }

    static func userContent(for request: AIRequest) -> Any {
        let images = request.attachments.filter { $0.kind != .pdf }
        guard !images.isEmpty else { return request.prompt }
        var parts: [[String: Any]] = [["type": "text", "text": request.prompt]]
        for image in images {
            let url = "data:\(image.mimeType);base64,\(image.data.base64EncodedString())"
            parts.append(["type": "image_url", "image_url": ["url": url]])
        }
        return parts
    }

    func complete(_ request: AIRequest) async throws -> String {
        let isLocal = baseURL.contains("localhost") || baseURL.contains("127.0.0.1")
        guard isLocal || accessToken != nil || !apiKey.trimmingCharacters(in: .whitespaces).isEmpty else {
            throw AIProviderError.missingAPIKey(name)
        }
        let root = baseURL.hasSuffix("/") ? String(baseURL.dropLast()) : baseURL
        guard let url = URL(string: root + "/chat/completions") else {
            throw AIProviderError.invalidResponse("Invalid base URL '\(baseURL)'.")
        }
        let body: [String: Any] = [
            "model": model,
            "messages": [
                ["role": "system", "content": request.system] as [String: Any],
                ["role": "user", "content": Self.userContent(for: request)] as [String: Any],
            ] as [[String: Any]],
            "response_format": [
                "type": "json_schema",
                "json_schema": ["name": request.schemaName, "schema": request.schema, "strict": true] as [String: Any],
            ] as [String: Any],
        ]
        var headers = extraHeaders
        if let accessToken {
            headers["authorization"] = "Bearer \(try await accessToken())"
        } else if !apiKey.isEmpty {
            headers["authorization"] = "Bearer \(apiKey)"
        }
        let json = try await AIHTTP.postJSON(url: url, headers: headers, body: body)
        let choices = json["choices"] as? [[String: Any]] ?? []
        guard let message = choices.first?["message"] as? [String: Any] else {
            throw AIProviderError.invalidResponse("\(name) returned no choices.")
        }
        if let refusal = message["refusal"] as? String, !refusal.isEmpty { throw AIProviderError.refused(refusal) }
        if (choices.first?["finish_reason"] as? String) == "length" { throw AIProviderError.truncated }
        guard let content = message["content"] as? String, !content.isEmpty else {
            throw AIProviderError.invalidResponse("\(name) returned an empty message.")
        }
        return content
    }
}

/// Google Gemini `generateContent` with JSON output mode.
struct GeminiProvider: AIProvider {
    var apiKey: String
    var model: String
    /// Google sign-in: Gemini on Vertex AI in this Google Cloud project and region, with the account's access token.
    struct Vertex {
        var project: String
        var region: String
        var accessToken: () async throws -> String

        /// `generateContent` on Vertex AI (the `global` region has no regional host prefix).
        func endpoint(model: String) -> URL? {
            let host = region == "global" ? "aiplatform.googleapis.com" : "\(region)-aiplatform.googleapis.com"
            return URL(string: "https://\(host)/v1/projects/\(project)/locations/\(region)/publishers/google/models/\(model):generateContent")
        }
    }
    var vertex: Vertex? = nil

    var displayName: String { "Gemini" }
    var modelName: String { model }
    var acceptsPDF: Bool { true }
    var acceptsImages: Bool { true }

    func complete(_ request: AIRequest) async throws -> String {
        let key = apiKey.trimmingCharacters(in: .whitespacesAndNewlines)
        let url: URL
        let headers: [String: String]
        if let vertex {
            guard !vertex.project.trimmingCharacters(in: .whitespaces).isEmpty else {
                throw AIProviderError.invalidResponse("Enter your Google Cloud project ID in Settings → AI Models → Gemini.")
            }
            guard let endpoint = vertex.endpoint(model: model) else {
                throw AIProviderError.invalidResponse("Invalid Vertex AI project, region or model.")
            }
            url = endpoint
            headers = ["authorization": "Bearer \(try await vertex.accessToken())", "x-goog-user-project": vertex.project]
        } else {
            guard !key.isEmpty else { throw AIProviderError.missingAPIKey("Google Gemini") }
            guard let endpoint = URL(string: "https://generativelanguage.googleapis.com/v1beta/models/\(model):generateContent") else {
                throw AIProviderError.invalidResponse("Invalid Gemini model name '\(model)'.")
            }
            url = endpoint
            headers = ["x-goog-api-key": key]
        }
        // Gemini's schema dialect differs from JSON Schema, so the schema travels in the prompt instead.
        let schemaText = (try? JSONSerialization.data(withJSONObject: request.schema, options: [.prettyPrinted, .sortedKeys]))
            .map { String(decoding: $0, as: UTF8.self) } ?? "{}"
        let prompt = request.prompt + "\n\nRespond with a single JSON object that conforms to this JSON Schema:\n" + schemaText
        var parts: [[String: Any]] = request.attachments.map {
            ["inlineData": ["mimeType": $0.mimeType, "data": $0.data.base64EncodedString()]]
        }
        parts.append(["text": prompt])
        let body: [String: Any] = [
            "systemInstruction": ["parts": [["text": request.system]]],
            "contents": [["role": "user", "parts": parts] as [String: Any]],
            "generationConfig": ["responseMimeType": "application/json", "maxOutputTokens": request.maxTokens] as [String: Any],
        ]
        let json = try await AIHTTP.postJSON(url: url, headers: headers, body: body)
        let candidates = json["candidates"] as? [[String: Any]] ?? []
        guard let first = candidates.first else {
            throw AIProviderError.invalidResponse("Gemini returned no candidates.")
        }
        if (first["finishReason"] as? String) == "MAX_TOKENS" { throw AIProviderError.truncated }
        if (first["finishReason"] as? String) == "SAFETY" { throw AIProviderError.refused("safety filter") }
        let responseParts = (first["content"] as? [String: Any])?["parts"] as? [[String: Any]] ?? []
        let text = responseParts.compactMap { $0["text"] as? String }.joined()
        guard !text.isEmpty else { throw AIProviderError.invalidResponse("Gemini returned an empty response.") }
        return text
    }
}

/// Local models served by Ollama (`/api/chat` with JSON-schema constrained output).
struct OllamaProvider: AIProvider {
    func userMessage(_ request: AIRequest) -> [String: Any] {
        var message: [String: Any] = ["role": "user", "content": request.prompt]
        let images = request.attachments.filter { $0.kind != .pdf }.map { $0.data.base64EncodedString() }
        if !images.isEmpty { message["images"] = images }
        return message
    }

    var model: String
    var baseURL: String

    var displayName: String { "Ollama" }
    var modelName: String { model }
    var acceptsImages: Bool { true }  // vision models such as llava; others ignore images

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
                ["role": "system", "content": request.system] as [String: Any],
                userMessage(request),
            ] as [[String: Any]],
        ]
        let json = try await AIHTTP.postJSON(url: url, headers: [:], body: body)
        guard let message = json["message"] as? [String: Any], let content = message["content"] as? String,
              !content.isEmpty else {
            throw AIProviderError.invalidResponse("Ollama returned an empty response.")
        }
        return content
    }
}
