import CryptoKit
import Foundation

// MARK: - Credentials

/// Where supplier API credentials are kept: the macOS Keychain in the app (like the AI provider keys), memory in
/// tests. Never UserDefaults, never the project file.
protocol SupplierCredentialStore: Sendable {
    func read(_ account: String) -> String
    func write(_ value: String, for account: String)
}

struct KeychainSupplierCredentials: SupplierCredentialStore {
    func read(_ account: String) -> String { KeychainStore.read("supplier." + account) }
    func write(_ value: String, for account: String) { KeychainStore.write(value, for: "supplier." + account) }
}

/// The credential fields of every supplier.
enum SupplierCredentialField: String, CaseIterable, Sendable {
    case nexarClientId, nexarClientSecret, digikeyClientId, digikeyClientSecret, mouserApiKey

    var source: SupplierSource {
        switch self {
        case .nexarClientId, .nexarClientSecret: return .nexar
        case .digikeyClientId, .digikeyClientSecret: return .digikey
        case .mouserApiKey: return .mouser
        }
    }

    var isSecret: Bool { self != .nexarClientId && self != .digikeyClientId }

    static func fields(for source: SupplierSource) -> [SupplierCredentialField] {
        allCases.filter { $0.source == source }
    }
}

/// A snapshot of the credentials for one search (value type: safe to hand to background tasks).
struct SupplierCredentials: Sendable, Equatable {
    var values: [SupplierCredentialField: String] = [:]

    func value(_ field: SupplierCredentialField) -> String {
        (values[field] ?? "").trimmingCharacters(in: .whitespacesAndNewlines)
    }

    /// Every field of the source is filled in.
    func isConfigured(_ source: SupplierSource) -> Bool {
        SupplierCredentialField.fields(for: source).allSatisfy { !value($0).isEmpty }
    }

    var configuredSources: [SupplierSource] { SupplierSource.allCases.filter(isConfigured) }
}

// MARK: - Errors

enum SupplierError: LocalizedError, Equatable {
    case notConfigured(SupplierSource)
    case unauthorized(SupplierSource, String)
    case rateLimited(SupplierSource, retryAfter: Int?)
    case http(SupplierSource, status: Int, message: String)
    case network(SupplierSource, String)
    case invalidResponse(SupplierSource, String)

    var errorDescription: String? {
        switch self {
        case .notConfigured(let s):
            return "\(s.displayName): no API key. Add one in Settings → Suppliers."
        case .unauthorized(let s, let message):
            return "\(s.displayName) refused the API key\(message.isEmpty ? "" : " (\(message))"). Check it in Settings → Suppliers."
        case .rateLimited(let s, let retryAfter):
            return "\(s.displayName) rate limit reached" + (retryAfter.map { ": try again in \($0) s." } ?? ": try again later.")
        case .http(let s, let status, let message):
            return "\(s.displayName) returned HTTP \(status)\(message.isEmpty ? "" : ": \(message)")"
        case .network(let s, let message):
            return "\(s.displayName) could not be reached: \(message)"
        case .invalidResponse(let s, let message):
            return "\(s.displayName): \(message)"
        }
    }

    /// Worth serving a stale cached result instead (the service, not the request, is the problem).
    var allowsStaleCache: Bool {
        switch self {
        case .network, .rateLimited: return true
        case .http(_, let status, _): return status >= 500
        default: return false
        }
    }
}

// MARK: - HTTP

/// URLSession calls for the supplier clients: request timeouts, one retry after a short HTTP 429 back-off,
/// cancellation passed through (a cancelled search stops its requests).
struct SupplierHTTP: Sendable {
    let session: URLSession
    /// Longest Retry-After (seconds) waited for before one retry; longer ones fail with `rateLimited`.
    var maxRetryWait: Double = 5

    static let shared = SupplierHTTP(session: makeSession())

    static func makeSession(protocolClasses: [AnyClass]? = nil) -> URLSession {
        let config = URLSessionConfiguration.ephemeral  // no cookies or URL cache on disk: SiEDA keeps its own cache
        config.timeoutIntervalForRequest = 20
        config.timeoutIntervalForResource = 60
        config.waitsForConnectivity = false
        config.httpAdditionalHeaders = ["Accept": "application/json", "User-Agent": "SiEDA"]
        if let protocolClasses { config.protocolClasses = protocolClasses }
        return URLSession(configuration: config)
    }

    /// Sends `request`. Returns the body and status for 2xx and for 4xx / 5xx replies (whose JSON bodies carry the
    /// distributor's message); 401 / 403 without a readable body, and 429, become errors here.
    func send(_ request: URLRequest, source: SupplierSource) async throws -> (Data, Int) {
        var attempt = 0
        while true {
            try Task.checkCancellation()
            let data: Data
            let response: URLResponse
            do {
                (data, response) = try await session.data(for: request)
            } catch let error as URLError where error.code == .cancelled {
                throw CancellationError()
            } catch is CancellationError {
                throw CancellationError()
            } catch {
                throw SupplierError.network(source, error.localizedDescription)
            }
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            if status == 429 {
                let header = (response as? HTTPURLResponse)?.value(forHTTPHeaderField: "Retry-After") ?? ""
                let wait = Double(header.trimmingCharacters(in: .whitespaces)).map { max(0, $0) }
                if attempt == 0, let wait, wait <= maxRetryWait {
                    attempt += 1
                    try await Task.sleep(nanoseconds: UInt64(wait * 1_000_000_000))
                    continue
                }
                throw SupplierError.rateLimited(source, retryAfter: wait.map { Int($0.rounded(.up)) })
            }
            return (data, status)
        }
    }

    /// OAuth 2 client-credentials token request (Nexar identity server, DigiKey).
    func clientCredentialsToken(url: URL, form: [String: String], source: SupplierSource) async throws -> (String, Double) {
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        request.httpBody = Data(Self.formEncoded(form).utf8)
        let (data, status) = try await send(request, source: source)
        let object = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any]
        guard (200..<300).contains(status) else {
            let message = (object?["error_description"] as? String) ?? (object?["error"] as? String) ?? ""
            if status == 400 || status == 401 || status == 403 { throw SupplierError.unauthorized(source, message) }
            throw SupplierError.http(source, status: status, message: message)
        }
        guard let token = object?["access_token"] as? String, !token.isEmpty else {
            throw SupplierError.invalidResponse(source, "The sign-in reply has no access token.")
        }
        let lifetime = (object?["expires_in"] as? Double) ?? Double(object?["expires_in"] as? Int ?? 600)
        return (token, max(60, lifetime))
    }

    static func formEncoded(_ form: [String: String]) -> String {
        var allowed = CharacterSet.alphanumerics
        allowed.insert(charactersIn: "-._~")
        return form.sorted { $0.key < $1.key }.map { key, value in
            "\(key)=\(value.addingPercentEncoding(withAllowedCharacters: allowed) ?? "")"
        }.joined(separator: "&")
    }

    static func jsonBody(_ object: [String: Any]) throws -> Data {
        try JSONSerialization.data(withJSONObject: object, options: [])
    }
}

/// OAuth access tokens per source and credentials, reused until shortly before they expire.
actor SupplierTokenCache {
    static let shared = SupplierTokenCache()
    private var tokens: [String: (token: String, expires: Date)] = [:]

    func token(key: String, fetch: () async throws -> (String, Double)) async throws -> String {
        if let cached = tokens[key], cached.expires > Date().addingTimeInterval(30) { return cached.token }
        let (token, lifetime) = try await fetch()
        tokens[key] = (token, Date().addingTimeInterval(lifetime))
        return token
    }

    func invalidate(key: String) { tokens[key] = nil }
    func clear() { tokens = [:] }
}

// MARK: - Clients

/// A distributor search: returns the raw reply body, which the core reads into the normalised schema.
protocol SupplierClient: Sendable {
    var source: SupplierSource { get }
    func search(_ query: String, limit: Int, currency: String) async throws -> (Data, Int)
}

/// Nexar Supply API (Octopart data): GraphQL `supSearchMpn`, OAuth client credentials (scope supply.domain).
struct NexarClient: SupplierClient {
    var clientId: String
    var clientSecret: String
    var http: SupplierHTTP = .shared
    var tokens: SupplierTokenCache = .shared
    var tokenURL = URL(string: "https://identity.nexar.com/connect/token")!
    var endpoint = URL(string: "https://api.nexar.com/graphql")!
    var source: SupplierSource { .nexar }

    static let query = """
    query SiedaPartSearch($q: String!, $limit: Int!, $currency: String!) {
      supSearchMpn(q: $q, limit: $limit, currency: $currency) {
        hits
        results {
          part {
            mpn
            manufacturer { name }
            shortDescription
            category { name }
            bestDatasheet { url }
            bestImage { url }
            octopartUrl
            specs { attribute { name shortname } displayValue }
            sellers(authorizedOnly: true) {
              company { name }
              offers {
                sku inventoryLevel moq orderMultiple packaging factoryLeadDays clickUrl
                prices { quantity price currency convertedPrice convertedCurrency }
              }
            }
          }
        }
      }
    }
    """

    func search(_ query: String, limit: Int, currency: String) async throws -> (Data, Int) {
        let key = "nexar|\(clientId)"
        let token = try await tokens.token(key: key) {
            try await http.clientCredentialsToken(url: tokenURL, form: [
                "grant_type": "client_credentials", "client_id": clientId, "client_secret": clientSecret,
                "scope": "supply.domain",
            ], source: .nexar)
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.httpBody = try SupplierHTTP.jsonBody([
            "query": Self.query,
            "variables": ["q": query, "limit": limit, "currency": currency] as [String: Any],
        ])
        let reply = try await http.send(request, source: .nexar)
        if reply.1 == 401 { await tokens.invalidate(key: key) }
        return reply
    }
}

/// DigiKey Product Information API v4: keyword search, OAuth client credentials.
struct DigiKeyClient: SupplierClient {
    var clientId: String
    var clientSecret: String
    var site = "US"
    var language = "en"
    var http: SupplierHTTP = .shared
    var tokens: SupplierTokenCache = .shared
    var tokenURL = URL(string: "https://api.digikey.com/v1/oauth2/token")!
    var endpoint = URL(string: "https://api.digikey.com/products/v4/search/keyword")!
    var source: SupplierSource { .digikey }

    func search(_ query: String, limit: Int, currency: String) async throws -> (Data, Int) {
        let key = "digikey|\(clientId)"
        let token = try await tokens.token(key: key) {
            try await http.clientCredentialsToken(url: tokenURL, form: [
                "grant_type": "client_credentials", "client_id": clientId, "client_secret": clientSecret,
            ], source: .digikey)
        }
        var request = URLRequest(url: endpoint)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.setValue("Bearer \(token)", forHTTPHeaderField: "Authorization")
        request.setValue(clientId, forHTTPHeaderField: "X-DIGIKEY-Client-Id")
        request.setValue(site, forHTTPHeaderField: "X-DIGIKEY-Locale-Site")
        request.setValue(language, forHTTPHeaderField: "X-DIGIKEY-Locale-Language")
        request.setValue(currency, forHTTPHeaderField: "X-DIGIKEY-Locale-Currency")
        request.httpBody = try SupplierHTTP.jsonBody(["Keywords": query, "Limit": min(max(limit, 1), 50), "Offset": 0])
        let reply = try await http.send(request, source: .digikey)
        if reply.1 == 401 { await tokens.invalidate(key: key) }
        return reply
    }
}

/// Mouser Search API v2: keyword search with the API key as a query parameter (as Mouser requires).
struct MouserClient: SupplierClient {
    var apiKey: String
    var http: SupplierHTTP = .shared
    var endpoint = URL(string: "https://api.mouser.com/api/v2/search/keyword")!
    var source: SupplierSource { .mouser }

    func search(_ query: String, limit: Int, currency: String) async throws -> (Data, Int) {
        guard var components = URLComponents(url: endpoint, resolvingAgainstBaseURL: false) else {
            throw SupplierError.invalidResponse(.mouser, "Bad endpoint.")
        }
        components.queryItems = [URLQueryItem(name: "apiKey", value: apiKey)]
        guard let url = components.url else { throw SupplierError.invalidResponse(.mouser, "Bad endpoint.") }
        var request = URLRequest(url: url)
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try SupplierHTTP.jsonBody(["SearchByKeywordRequest": [
            "keyword": query, "records": min(max(limit, 1), 50), "startingRecord": 0, "searchOptions": "",
            "searchWithYourSignUpLanguage": "",
        ] as [String: Any]])
        return try await http.send(request, source: .mouser)
    }
}

// MARK: - Offline cache

/// Normalised search results on disk (Caches/SiEDA/Suppliers), one file per source, currency and query. Fresh
/// results are reused for `lifetime`; older ones only when the distributor cannot be reached.
struct SupplierCache: Sendable {
    let directory: URL
    var lifetime: TimeInterval = 24 * 3600
    var maxFiles = 500

    static var defaultDirectory: URL {
        let base = FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask).first
            ?? FileManager.default.temporaryDirectory
        return base.appendingPathComponent("SiEDA/Suppliers", isDirectory: true)
    }

    static func key(source: SupplierSource, query: String, currency: String) -> String {
        let text = "\(source.rawValue)|\(currency)|\(query.trimmingCharacters(in: .whitespacesAndNewlines).lowercased())"
        return SHA256.hash(data: Data(text.utf8)).map { String(format: "%02x", $0) }.joined()
    }

    private struct Entry: Codable {
        var savedAt: Date
        var result: SupplierSearchResultInfo
    }

    private func file(_ key: String) -> URL { directory.appendingPathComponent(key + ".json") }

    /// The cached result and when it was saved; `fresh` false also returns expired entries.
    func load(_ key: String, fresh: Bool, now: Date = Date()) -> (SupplierSearchResultInfo, Date)? {
        guard let data = try? Data(contentsOf: file(key)), data.count < 32 << 20,
              let entry = try? JSONDecoder().decode(Entry.self, from: data) else { return nil }
        if fresh && now.timeIntervalSince(entry.savedAt) > lifetime { return nil }
        return (entry.result, entry.savedAt)
    }

    func store(_ result: SupplierSearchResultInfo, key: String, now: Date = Date()) {
        guard result.error.isEmpty else { return }  // errors are never cached
        do {
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            let data = try JSONEncoder().encode(Entry(savedAt: now, result: result))
            try data.write(to: file(key), options: .atomic)
            prune()
        } catch {
            // A cache that cannot be written only costs a network call next time.
        }
    }

    func clear() {
        try? FileManager.default.removeItem(at: directory)
    }

    private func prune() {
        let keys: [URLResourceKey] = [.contentModificationDateKey]
        guard let files = try? FileManager.default.contentsOfDirectory(at: directory, includingPropertiesForKeys: keys),
              files.count > maxFiles else { return }
        let dated = files.map { url in
            (url, (try? url.resourceValues(forKeys: Set(keys)).contentModificationDate) ?? .distantPast)
        }
        for (url, _) in dated.sorted(by: { $0.1 < $1.1 }).prefix(files.count - maxFiles) {
            try? FileManager.default.removeItem(at: url)
        }
    }
}

// MARK: - Search

/// What one source contributed to a search.
enum SupplierSourceStatus: Equatable, Sendable {
    case notConfigured
    case live(parts: Int)
    case cached(parts: Int, savedAt: Date)
    /// The distributor could not be reached; an older cached result is shown.
    case offline(parts: Int, savedAt: Date, reason: String)
    case failed(String)
}

struct SupplierSearchOutcome: Sendable {
    var parts: [SupplierPartInfo] = []
    var statuses: [SupplierSource: SupplierSourceStatus] = [:]
}

/// Searches every configured distributor at once (results from the cache when fresh) and merges the replies.
struct SupplierSearchEngine: Sendable {
    var credentials: SupplierCredentials
    var currency = "USD"
    var http: SupplierHTTP = .shared
    var tokens: SupplierTokenCache = .shared
    var cache: SupplierCache? = SupplierCache(directory: SupplierCache.defaultDirectory)
    /// Clients by source; nil uses the real distributor clients built from the credentials.
    var clients: [SupplierSource: SupplierClient]?

    func client(for source: SupplierSource) -> SupplierClient? {
        if let clients { return clients[source] }
        guard credentials.isConfigured(source) else { return nil }
        switch source {
        case .nexar:
            return NexarClient(clientId: credentials.value(.nexarClientId), clientSecret: credentials.value(.nexarClientSecret),
                               http: http, tokens: tokens)
        case .digikey:
            return DigiKeyClient(clientId: credentials.value(.digikeyClientId),
                                 clientSecret: credentials.value(.digikeyClientSecret), http: http, tokens: tokens)
        case .mouser:
            return MouserClient(apiKey: credentials.value(.mouserApiKey), http: http)
        }
    }

    func search(_ rawQuery: String, limit: Int = 20) async -> SupplierSearchOutcome {
        let query = String(rawQuery.trimmingCharacters(in: .whitespacesAndNewlines).prefix(120))
        var outcome = SupplierSearchOutcome()
        guard !query.isEmpty else { return outcome }
        var results: [SupplierSearchResultInfo] = []
        await withTaskGroup(of: (SupplierSource, SupplierSearchResultInfo?, SupplierSourceStatus).self) { group in
            for source in SupplierSource.allCases {
                guard let client = client(for: source) else {
                    outcome.statuses[source] = .notConfigured
                    continue
                }
                group.addTask { await searchOne(client, query: query, limit: limit) }
            }
            for await (source, result, status) in group {
                outcome.statuses[source] = status
                if let result { results.append(result) }
            }
        }
        guard !Task.isCancelled else { return SupplierSearchOutcome() }
        // A stable order (Nexar, DigiKey, Mouser) so merged fields do not depend on which reply came first.
        results.sort { a, b in
            (SupplierSource.allCases.firstIndex { $0.rawValue == a.source } ?? 9)
                < (SupplierSource.allCases.firstIndex { $0.rawValue == b.source } ?? 9)
        }
        outcome.parts = EDAEngine.mergeSupplierResults(results, currency: currency)
        return outcome
    }

    private func searchOne(_ client: SupplierClient, query: String, limit: Int)
        async -> (SupplierSource, SupplierSearchResultInfo?, SupplierSourceStatus) {
        let source = client.source
        let key = SupplierCache.key(source: source, query: query, currency: currency)
        if let hit = cache?.load(key, fresh: true) {
            return (source, hit.0, .cached(parts: hit.0.parts.count, savedAt: hit.1))
        }
        do {
            let (body, status) = try await client.search(query, limit: limit, currency: currency)
            let result = EDAEngine.parseSupplierReply(source: source, body: body, currency: currency)
            if !result.error.isEmpty {
                let error: SupplierError
                switch result.errorKind {
                case "auth": error = .unauthorized(source, result.error)
                case "rate_limit": error = .rateLimited(source, retryAfter: nil)
                case "service": error = .http(source, status: max(status, 500), message: result.error)
                default: error = .invalidResponse(source, result.error)
                }
                return fallback(source, key: key, error: error)
            }
            guard (200..<300).contains(status) else {
                let error: SupplierError = status == 401 || status == 403 ? .unauthorized(source, "")
                    : .http(source, status: status, message: "")
                return fallback(source, key: key, error: error)
            }
            cache?.store(result, key: key)
            return (source, result, .live(parts: result.parts.count))
        } catch let error as SupplierError {
            return fallback(source, key: key, error: error)
        } catch {
            return (source, nil, .failed(Task.isCancelled ? "Cancelled" : error.localizedDescription))
        }
    }

    /// An older cached result when the service is down or busy; otherwise the error.
    private func fallback(_ source: SupplierSource, key: String, error: SupplierError)
        -> (SupplierSource, SupplierSearchResultInfo?, SupplierSourceStatus) {
        let message = error.localizedDescription
        if error.allowsStaleCache, let hit = cache?.load(key, fresh: false) {
            return (source, hit.0, .offline(parts: hit.0.parts.count, savedAt: hit.1, reason: message))
        }
        return (source, nil, .failed(message))
    }
}

// MARK: - Settings

/// Supplier search settings: which distributors have keys (kept in the Keychain), the currency and how long
/// results are cached.
@MainActor
final class SupplierSettings: ObservableObject {
    static let shared = SupplierSettings()

    @Published var currency: String {
        didSet { defaults.set(currency, forKey: "suppliers.currency") }
    }
    @Published var cacheHours: Int {
        didSet { defaults.set(cacheHours, forKey: "suppliers.cacheHours") }
    }
    /// Bumped when a credential changes, so views showing the configured state refresh.
    @Published private(set) var revision = 0

    static let currencies = ["USD", "EUR", "GBP", "JPY", "CNY", "INR", "SGD", "MYR", "KRW", "TWD"]

    private let defaults: UserDefaults
    private let store: SupplierCredentialStore
    private var cachedValues: [SupplierCredentialField: String] = [:]

    init(defaults: UserDefaults = .standard, store: SupplierCredentialStore = KeychainSupplierCredentials()) {
        self.defaults = defaults
        self.store = store
        let stored = defaults.string(forKey: "suppliers.currency") ?? "USD"
        currency = Self.currencies.contains(stored) ? stored : "USD"
        cacheHours = min(max(defaults.object(forKey: "suppliers.cacheHours") as? Int ?? 24, 1), 24 * 30)
    }

    func value(_ field: SupplierCredentialField) -> String {
        if let cached = cachedValues[field] { return cached }
        let value = store.read(field.rawValue)
        cachedValues[field] = value
        return value
    }

    func setValue(_ value: String, for field: SupplierCredentialField) {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        store.write(trimmed, for: field.rawValue)
        cachedValues[field] = trimmed
        revision &+= 1
        Task { await SupplierTokenCache.shared.clear() }
    }

    var credentials: SupplierCredentials {
        var c = SupplierCredentials()
        for field in SupplierCredentialField.allCases { c.values[field] = value(field) }
        return c
    }

    var configuredSources: [SupplierSource] { credentials.configuredSources }
    var hasAnySource: Bool { !configuredSources.isEmpty }

    var cache: SupplierCache {
        SupplierCache(directory: SupplierCache.defaultDirectory, lifetime: TimeInterval(cacheHours) * 3600)
    }

    func engine() -> SupplierSearchEngine {
        SupplierSearchEngine(credentials: credentials, currency: currency, cache: cache)
    }
}

/// The search sheet's model: one search at a time, cancelled when a new one starts or the sheet closes.
@MainActor
final class SupplierSearchModel: ObservableObject {
    @Published private(set) var parts: [SupplierPartInfo] = []
    @Published private(set) var statuses: [SupplierSource: SupplierSourceStatus] = [:]
    @Published private(set) var isSearching = false
    @Published private(set) var lastQuery = ""
    private var task: Task<Void, Never>?

    func search(_ query: String, engine: SupplierSearchEngine) {
        task?.cancel()
        let trimmed = query.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        isSearching = true
        lastQuery = trimmed
        task = Task { [weak self] in
            let outcome = await engine.search(trimmed)  // runs off the main actor; cancelled with this task
            guard !Task.isCancelled, let self else { return }
            self.parts = outcome.parts
            self.statuses = outcome.statuses
            self.isSearching = false
        }
    }

    func cancel() {
        task?.cancel()
        task = nil
        isSearching = false
    }
}
