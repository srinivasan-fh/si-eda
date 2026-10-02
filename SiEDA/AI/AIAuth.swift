import AppKit
import CryptoKit
import Foundation
import Network

/// How SiEDA signs in to an AI provider. Every browser-based option uses the provider's own, documented login:
///
/// - **Claude Console** — the official Anthropic CLI (`ant auth login`) runs the browser OAuth flow against the Claude
///   Console and keeps the workspace-scoped token; SiEDA asks it for a fresh access token (`ant auth
///   print-credentials`) and sends it as a bearer token. Usage is billed to the Console workspace picked in the browser.
/// - **Google Cloud** — the official Google Cloud CLI (`gcloud auth application-default login`) signs in with a Google
///   account; SiEDA calls Gemini on Vertex AI with the account's access token.
/// - **Browser (OpenRouter)** — OAuth 2.0 with PKCE in the default browser; OpenRouter returns a key for this app.
/// - **Organisation SSO** — OpenID Connect + PKCE against the company identity provider (Okta, Entra ID, Google
///   Workspace, Keycloak…) for an OpenAI-compatible AI gateway that accepts its access tokens.
enum AIAuthMode: String, CaseIterable, Identifiable, Codable {
    case apiKey
    case claudeConsole
    case googleCloud
    case browser
    case sso

    var id: String { rawValue }

    var title: String {
        switch self {
        case .apiKey: return "API key"
        case .claudeConsole: return "Sign in with Claude Console"
        case .googleCloud: return "Sign in with Google (Vertex AI)"
        case .browser: return "Sign in with OpenRouter"
        case .sso: return "Organisation SSO (OpenID Connect)"
        }
    }

    var usesBrowser: Bool { self != .apiKey }

    /// Signs in through a vendor command-line tool (which keeps the tokens under the user's home folder).
    var usesCommandLineTool: Bool { self == .claudeConsole || self == .googleCloud }

    /// Command-line sign-ins need the app to run outside the App Sandbox: a sandboxed app's child processes inherit
    /// its sandbox and cannot write the CLI's credential files.
    var isAvailable: Bool { !(usesCommandLineTool && CommandLineTool.isSandboxed) }
}

extension AIProviderKind {
    /// Sign-in options the provider offers, the preferred one first.
    var authModes: [AIAuthMode] {
        switch self {
        case .claude: return [.claudeConsole, .apiKey]
        case .openAI: return [.apiKey, .sso]
        case .gemini: return [.googleCloud, .apiKey]
        case .openRouter: return [.browser, .apiKey]
        case .ollama, .offline: return []
        }
    }
}

enum AIAuthError: LocalizedError {
    case toolMissing(tool: String, install: String)
    case toolFailed(String)
    case notSignedIn(String)
    case cancelled
    case timedOut
    case badResponse(String)
    case configuration(String)

    var errorDescription: String? {
        switch self {
        case .toolMissing(let tool, let install): return "\(tool) is not installed. \(install)"
        case .toolFailed(let message): return message
        case .notSignedIn(let provider): return "Not signed in to \(provider). Use Sign In in Settings → AI Models."
        case .cancelled: return "Sign-in was cancelled."
        case .timedOut: return "Sign-in timed out — finish the login in the browser within 5 minutes."
        case .badResponse(let message): return message
        case .configuration(let message): return message
        }
    }
}

// MARK: - Command-line tools (ant, gcloud)

enum CommandLineTool {
    /// True when this process runs in the App Sandbox (macOS sets the container id for sandboxed apps).
    static var isSandboxed: Bool { ProcessInfo.processInfo.environment["APP_SANDBOX_CONTAINER_ID"] != nil }

    static let sandboxMessage = "This sign-in runs the vendor's command-line tool, which a sandboxed build of SiEDA cannot start. Use a build without the App Sandbox, or choose another sign-in method (API key, OpenRouter or organisation SSO)."

    /// GUI apps start with a minimal PATH; these are where the CLIs are usually installed.
    static var searchPaths: [String] {
        let home = NSHomeDirectory()
        return ["/opt/homebrew/bin", "/usr/local/bin", "/usr/bin", "/bin", home + "/.local/bin", home + "/bin",
                home + "/google-cloud-sdk/bin", "/opt/homebrew/share/google-cloud-sdk/bin",
                "/usr/local/share/google-cloud-sdk/bin"]
    }

    static func locate(_ name: String) -> URL? {
        let fromEnvironment = (ProcessInfo.processInfo.environment["PATH"] ?? "").split(separator: ":").map(String.init)
        for directory in fromEnvironment + searchPaths {
            let url = URL(fileURLWithPath: directory).appendingPathComponent(name)
            if FileManager.default.isExecutableFile(atPath: url.path) { return url }
        }
        return nil
    }

    struct Output {
        var status: Int32
        var stdout: String
        var stderr: String

        /// The most useful line of the error output (CLIs print progress first and the error last).
        var errorSummary: String {
            let lines = (stderr + "\n" + stdout).split(separator: "\n").map { $0.trimmingCharacters(in: .whitespaces) }
            return lines.last { !$0.isEmpty }.map(String.init) ?? "exit status \(status)"
        }
    }

    /// Runs a tool to completion without blocking the caller; the process is terminated after `timeout`.
    static func run(_ tool: URL, _ arguments: [String], timeout: TimeInterval) async throws -> Output {
        try await withCheckedThrowingContinuation { (continuation: CheckedContinuation<Output, Error>) in
            let process = Process()
            process.executableURL = tool
            process.arguments = arguments
            var environment = ProcessInfo.processInfo.environment
            environment["PATH"] = (searchPaths + [environment["PATH"] ?? ""]).joined(separator: ":")
            process.environment = environment
            let out = Pipe(), err = Pipe()
            process.standardOutput = out
            process.standardError = err
            process.standardInput = FileHandle.nullDevice

            final class Box: @unchecked Sendable {
                var stdout = Data(), stderr = Data(), timedOut = false
            }
            let box = Box()
            let reads = DispatchGroup()
            for (pipe, isOut) in [(out, true), (err, false)] {
                reads.enter()
                DispatchQueue.global().async {
                    let data = pipe.fileHandleForReading.readDataToEndOfFile()
                    if isOut { box.stdout = data } else { box.stderr = data }
                    reads.leave()
                }
            }
            process.terminationHandler = { finished in
                // A browser started by the tool may keep the pipes open; don't wait for it.
                _ = reads.wait(timeout: .now() + 2)
                if box.timedOut {
                    continuation.resume(throwing: AIAuthError.timedOut)
                    return
                }
                continuation.resume(returning: Output(status: finished.terminationStatus,
                                                      stdout: String(decoding: box.stdout, as: UTF8.self),
                                                      stderr: String(decoding: box.stderr, as: UTF8.self)))
            }
            do {
                try process.run()
            } catch {
                continuation.resume(throwing: AIAuthError.toolFailed("Could not start \(tool.lastPathComponent): \(error.localizedDescription)"))
                return
            }
            DispatchQueue.global().asyncAfter(deadline: .now() + timeout) {
                if process.isRunning {
                    box.timedOut = true
                    process.terminate()
                }
            }
        }
    }
}

/// Short-lived access tokens fetched from a CLI, reused for a few minutes.
actor TokenCache {
    static let shared = TokenCache()
    private var tokens: [String: (value: String, until: Date)] = [:]

    func token(_ key: String, validFor seconds: TimeInterval, fetch: () async throws -> String) async throws -> String {
        if let cached = tokens[key], cached.until > Date() { return cached.value }
        let value = try await fetch()
        tokens[key] = (value, Date().addingTimeInterval(seconds))
        return value
    }

    func clear(_ key: String) { tokens[key] = nil }
}

/// Claude Console sign-in through the official Anthropic CLI (`ant`), under SiEDA's own profile.
enum ClaudeConsoleAuth {
    static let profile = "sieda"
    static let installHint = "Install the Anthropic CLI (see platform.claude.com/docs/en/cli-sdks-libraries/cli/quickstart), then press Sign In again."

    static func tool() throws -> URL {
        guard !CommandLineTool.isSandboxed else { throw AIAuthError.configuration(CommandLineTool.sandboxMessage) }
        guard let url = CommandLineTool.locate("ant") else {
            throw AIAuthError.toolMissing(tool: "The Anthropic CLI (ant)", install: installHint)
        }
        return url
    }

    /// Opens the Claude Console login in the browser (organisation + workspace picker) and waits for it to finish.
    static func signIn() async throws {
        let result = try await CommandLineTool.run(tool(), ["auth", "login", "--profile", profile], timeout: 300)
        guard result.status == 0 else { throw AIAuthError.toolFailed("Claude Console sign-in failed: \(result.errorSummary)") }
        await TokenCache.shared.clear("claude")
        _ = try await accessToken()
    }

    static func signOut() async {
        if let tool = try? tool() { _ = try? await CommandLineTool.run(tool, ["auth", "logout", "--profile", profile], timeout: 30) }
        await TokenCache.shared.clear("claude")
    }

    /// A current access token for `Authorization: Bearer` (the CLI refreshes it when it has expired).
    static func accessToken() async throws -> String {
        try await TokenCache.shared.token("claude", validFor: 300) {
            let result = try await CommandLineTool.run(try tool(), ["auth", "print-credentials", "--profile", profile, "--access-token"],
                                                       timeout: 60)
            let token = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            guard result.status == 0, !token.isEmpty, !token.contains(" ") else {
                throw AIAuthError.notSignedIn("Claude Console (\(result.errorSummary))")
            }
            return token
        }
    }
}

/// Google account sign-in through the official Google Cloud CLI (Application Default Credentials).
enum GoogleCloudAuth {
    static let installHint = "Install the Google Cloud CLI (cloud.google.com/sdk/docs/install), then press Sign In again."

    static func tool() throws -> URL {
        guard !CommandLineTool.isSandboxed else { throw AIAuthError.configuration(CommandLineTool.sandboxMessage) }
        guard let url = CommandLineTool.locate("gcloud") else {
            throw AIAuthError.toolMissing(tool: "The Google Cloud CLI (gcloud)", install: installHint)
        }
        return url
    }

    static func signIn() async throws {
        let result = try await CommandLineTool.run(tool(), ["auth", "application-default", "login", "--quiet"], timeout: 300)
        guard result.status == 0 else { throw AIAuthError.toolFailed("Google sign-in failed: \(result.errorSummary)") }
        await TokenCache.shared.clear("google")
        _ = try await accessToken()
    }

    static func signOut() async {
        if let tool = try? tool() { _ = try? await CommandLineTool.run(tool, ["auth", "application-default", "revoke", "--quiet"], timeout: 30) }
        await TokenCache.shared.clear("google")
    }

    static func accessToken() async throws -> String {
        try await TokenCache.shared.token("google", validFor: 300) {
            let result = try await CommandLineTool.run(try tool(), ["auth", "application-default", "print-access-token"], timeout: 60)
            let token = result.stdout.trimmingCharacters(in: .whitespacesAndNewlines)
            guard result.status == 0, !token.isEmpty, !token.contains(" ") else {
                throw AIAuthError.notSignedIn("Google Cloud (\(result.errorSummary))")
            }
            return token
        }
    }
}

// MARK: - OAuth 2.0 authorization code + PKCE with a loopback redirect (RFC 7636, RFC 8252)

enum PKCE {
    static func verifier() -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        _ = SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes)
        return base64URL(Data(bytes))
    }

    static func challenge(for verifier: String) -> String {
        base64URL(Data(SHA256.hash(data: Data(verifier.utf8))))
    }

    static func base64URL(_ data: Data) -> String {
        data.base64EncodedString().replacingOccurrences(of: "+", with: "-").replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

/// A one-shot HTTP listener on this Mac (local connections only) that receives the browser's OAuth redirect.
final class LoopbackRedirect: @unchecked Sendable {
    private let listener: NWListener
    private let queue: DispatchQueue
    // Guarded by `queue`.
    private var continuation: CheckedContinuation<URLComponents, Error>?
    private var pending: Result<URLComponents, Error>?
    private var delivered = false
    let port: UInt16

    /// Listens on `preferredPort` when it is free, otherwise on any free port.
    static func start(preferredPort: UInt16? = nil) async throws -> LoopbackRedirect {
        if let preferredPort, let port = NWEndpoint.Port(rawValue: preferredPort),
           let redirect = try? await open(on: port) {
            return redirect
        }
        return try await open(on: .any)
    }

    private static func open(on port: NWEndpoint.Port) async throws -> LoopbackRedirect {
        let parameters = NWParameters.tcp
        parameters.acceptLocalOnly = true
        let listener = try NWListener(using: parameters, on: port)
        let queue = DispatchQueue(label: "sieda.oauth.loopback")
        final class Once: @unchecked Sendable { var done = false }
        let once = Once()
        let bound: UInt16 = try await withCheckedThrowingContinuation { ready in
            listener.stateUpdateHandler = { state in
                guard !once.done else { return }
                switch state {
                case .ready:
                    once.done = true
                    ready.resume(returning: listener.port?.rawValue ?? 0)
                case .failed(let error):
                    once.done = true
                    listener.cancel()
                    ready.resume(throwing: error)
                default:
                    break
                }
            }
            listener.newConnectionHandler = { connection in connection.cancel() }
            listener.start(queue: queue)
        }
        let redirect = LoopbackRedirect(listener: listener, queue: queue, port: bound)
        listener.newConnectionHandler = { [weak redirect] connection in redirect?.handle(connection) }
        return redirect
    }

    private init(listener: NWListener, queue: DispatchQueue, port: UInt16) {
        self.listener = listener
        self.queue = queue
        self.port = port
    }

    var redirectURI: String { "http://127.0.0.1:\(port)/callback" }
    var localhostRedirectURI: String { "http://localhost:\(port)/callback" }

    /// Waits for `GET /callback?…` (other paths such as /favicon.ico are answered and ignored).
    func waitForCallback(timeout: TimeInterval = 300) async throws -> URLComponents {
        defer { stop() }
        return try await withCheckedThrowingContinuation { continuation in
            queue.async {
                self.continuation = continuation
                if let pending = self.pending {
                    self.pending = nil
                    self.deliver(pending)
                }
                self.queue.asyncAfter(deadline: .now() + timeout) { self.deliver(.failure(AIAuthError.timedOut)) }
            }
        }
    }

    func stop() {
        queue.async {
            self.listener.cancel()
            if self.continuation != nil { self.deliver(.failure(AIAuthError.cancelled)) }
        }
    }

    /// Runs on `queue`: hands the result to the waiting caller, or keeps it until someone waits.
    private func deliver(_ result: Result<URLComponents, Error>) {
        guard !delivered else { return }
        guard let continuation else {
            if pending == nil { pending = result }
            return
        }
        delivered = true
        self.continuation = nil
        continuation.resume(with: result)
    }

    private func handle(_ connection: NWConnection) {
        connection.start(queue: queue)
        connection.receive(minimumIncompleteLength: 1, maximumLength: 16_384) { [weak self] data, _, _, _ in
            guard let self else { return }
            let request = String(decoding: data ?? Data(), as: UTF8.self)
            let target = request.split(separator: "\r\n").first?.split(separator: " ").dropFirst().first.map(String.init) ?? "/"
            let components = URLComponents(string: "http://localhost" + target)
            let isCallback = components?.path == "/callback"
            let failed = components?.queryItems?.contains { $0.name == "error" } ?? false
            let body = isCallback
                ? "<html><body style=\"font-family:-apple-system;background:#0b1a33;color:#e8f0ff;padding:48px\"><h2>\(failed ? "Sign-in was not completed" : "Signed in to SiEDA")</h2><p>You can close this tab and return to SiEDA.</p></body></html>"
                : "Not found"
            let response = "HTTP/1.1 \(isCallback ? "200 OK" : "404 Not Found")\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: \(body.utf8.count)\r\nConnection: close\r\n\r\n" + body
            connection.send(content: Data(response.utf8), completion: .contentProcessed { _ in connection.cancel() })
            if isCallback, let components { self.deliver(.success(components)) }
        }
    }
}

/// OpenRouter OAuth PKCE: the user approves SiEDA in the browser and OpenRouter returns an API key for it.
enum OpenRouterAuth {
    static func signIn() async throws -> String {
        let redirect = try await LoopbackRedirect.start(preferredPort: 3000)
        let verifier = PKCE.verifier()
        var authorize = URLComponents(string: "https://openrouter.ai/auth")!
        authorize.queryItems = [
            URLQueryItem(name: "callback_url", value: redirect.localhostRedirectURI),
            URLQueryItem(name: "code_challenge", value: PKCE.challenge(for: verifier)),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
        ]
        guard let url = authorize.url else { throw AIAuthError.configuration("Could not build the OpenRouter login URL.") }
        await MainActor.run { _ = NSWorkspace.shared.open(url) }
        let callback = try await redirect.waitForCallback()
        guard let code = callback.queryItems?.first(where: { $0.name == "code" })?.value else {
            throw AIAuthError.cancelled
        }
        let json = try await AIHTTP.postJSON(url: URL(string: "https://openrouter.ai/api/v1/auth/keys")!, headers: [:],
                                             body: ["code": code, "code_verifier": verifier, "code_challenge_method": "S256"])
        guard let key = json["key"] as? String, !key.isEmpty else {
            throw AIAuthError.badResponse("OpenRouter did not return a key.")
        }
        return key
    }
}

/// Organisation single sign-on with OpenID Connect (authorization code + PKCE, public client, loopback redirect).
struct OIDCConfiguration: Codable, Equatable {
    var issuer = ""
    var clientID = ""
    var scopes = "openid profile email offline_access"
    /// Optional `audience` parameter (Auth0 and some gateways need it to issue an API access token).
    var audience = ""

    var isComplete: Bool { !issuer.trimmingCharacters(in: .whitespaces).isEmpty && !clientID.trimmingCharacters(in: .whitespaces).isEmpty }
}

struct OIDCTokens: Codable, Equatable {
    var accessToken: String
    var refreshToken: String?
    var expiresAt: Date
    var tokenEndpoint: String
}

enum OIDCAuth {
    static let keychainAccount = "sso.tokens"

    struct Discovery: Decodable {
        var authorization_endpoint: String
        var token_endpoint: String
    }

    static func discover(_ issuer: String) async throws -> Discovery {
        let root = issuer.trimmingCharacters(in: .whitespaces).trimmingCharacters(in: CharacterSet(charactersIn: "/"))
        guard let url = URL(string: root + "/.well-known/openid-configuration") else {
            throw AIAuthError.configuration("Invalid issuer URL '\(issuer)'.")
        }
        do {
            let (data, _) = try await AIHTTP.session.data(from: url)
            return try JSONDecoder().decode(Discovery.self, from: data)
        } catch {
            throw AIAuthError.configuration("Could not read the identity provider's OpenID configuration at \(url.absoluteString): \(error.localizedDescription)")
        }
    }

    static func signIn(_ config: OIDCConfiguration) async throws -> OIDCTokens {
        guard config.isComplete else { throw AIAuthError.configuration("Enter the issuer URL and client ID of your identity provider first.") }
        let discovery = try await discover(config.issuer)
        let redirect = try await LoopbackRedirect.start()
        let verifier = PKCE.verifier()
        let state = PKCE.verifier()
        guard var authorize = URLComponents(string: discovery.authorization_endpoint) else {
            throw AIAuthError.configuration("The identity provider returned an invalid authorization endpoint.")
        }
        var items = authorize.queryItems ?? []
        items += [
            URLQueryItem(name: "response_type", value: "code"),
            URLQueryItem(name: "client_id", value: config.clientID.trimmingCharacters(in: .whitespaces)),
            URLQueryItem(name: "redirect_uri", value: redirect.redirectURI),
            URLQueryItem(name: "scope", value: config.scopes),
            URLQueryItem(name: "state", value: state),
            URLQueryItem(name: "code_challenge", value: PKCE.challenge(for: verifier)),
            URLQueryItem(name: "code_challenge_method", value: "S256"),
        ]
        if !config.audience.isEmpty { items.append(URLQueryItem(name: "audience", value: config.audience)) }
        authorize.queryItems = items
        guard let url = authorize.url else { throw AIAuthError.configuration("Could not build the sign-in URL.") }
        await MainActor.run { _ = NSWorkspace.shared.open(url) }
        let callback = try await redirect.waitForCallback()
        let query = callback.queryItems ?? []
        if let error = query.first(where: { $0.name == "error" })?.value {
            let detail = query.first { $0.name == "error_description" }?.value ?? error
            throw AIAuthError.badResponse("The identity provider refused the sign-in: \(detail)")
        }
        guard query.first(where: { $0.name == "state" })?.value == state,
              let code = query.first(where: { $0.name == "code" })?.value else {
            throw AIAuthError.badResponse("The sign-in response did not match this request.")
        }
        return try await tokenRequest(discovery.token_endpoint, [
            "grant_type": "authorization_code", "code": code, "redirect_uri": redirect.redirectURI,
            "client_id": config.clientID.trimmingCharacters(in: .whitespaces), "code_verifier": verifier,
        ], previousRefresh: nil)
    }

    /// A valid access token, refreshed with the refresh token when it is about to expire.
    static func accessToken(_ config: OIDCConfiguration) async throws -> String {
        guard let stored = load() else { throw AIAuthError.notSignedIn("your organisation (SSO)") }
        if stored.expiresAt.timeIntervalSinceNow > 60 { return stored.accessToken }
        guard let refresh = stored.refreshToken else { throw AIAuthError.notSignedIn("your organisation (SSO session expired)") }
        let tokens = try await tokenRequest(stored.tokenEndpoint, [
            "grant_type": "refresh_token", "refresh_token": refresh,
            "client_id": config.clientID.trimmingCharacters(in: .whitespaces),
        ], previousRefresh: refresh)
        save(tokens)
        return tokens.accessToken
    }

    static func tokenRequest(_ endpoint: String, _ form: [String: String], previousRefresh: String?) async throws -> OIDCTokens {
        guard let url = URL(string: endpoint) else { throw AIAuthError.configuration("Invalid token endpoint.") }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "content-type")
        request.setValue("application/json", forHTTPHeaderField: "accept")
        request.httpBody = Data(formEncode(form).utf8)
        let (data, response) = try await AIHTTP.session.data(for: request)
        let status = (response as? HTTPURLResponse)?.statusCode ?? 0
        guard (200..<300).contains(status),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let access = json["access_token"] as? String, !access.isEmpty else {
            throw AIAuthError.badResponse("The identity provider's token endpoint returned HTTP \(status): \(String(decoding: data, as: UTF8.self).prefix(300))")
        }
        let lifetime = (json["expires_in"] as? Double) ?? (json["expires_in"] as? Int).map(Double.init) ?? 3600
        return OIDCTokens(accessToken: access, refreshToken: (json["refresh_token"] as? String) ?? previousRefresh,
                          expiresAt: Date().addingTimeInterval(lifetime), tokenEndpoint: endpoint)
    }

    static func formEncode(_ form: [String: String]) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return form.sorted { $0.key < $1.key }.map { key, value in
            "\(key)=\(value.addingPercentEncoding(withAllowedCharacters: allowed) ?? value)"
        }.joined(separator: "&")
    }

    static func load() -> OIDCTokens? {
        let text = KeychainStore.read(keychainAccount)
        guard !text.isEmpty else { return nil }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .secondsSince1970
        return try? decoder.decode(OIDCTokens.self, from: Data(text.utf8))
    }

    static func save(_ tokens: OIDCTokens?) {
        guard let tokens else {
            KeychainStore.write("", for: keychainAccount)
            return
        }
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .secondsSince1970
        if let data = try? encoder.encode(tokens) { KeychainStore.write(String(decoding: data, as: UTF8.self), for: keychainAccount) }
    }
}
