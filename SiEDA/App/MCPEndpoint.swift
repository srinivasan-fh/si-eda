import Foundation
import Network
import Security

// The app's live MCP endpoint, transport half (docs/MCP.md, "Live app mode"): an HTTP/1.1 listener bound to
// 127.0.0.1 only, the request parser and the request checks (Host / Origin against DNS rebinding, bearer token).
// What a request does with the open design is in DesignStore+MCP.swift (MCPLiveServer, MCPStoreBridge).

/// One HTTP request. Header names are stored lower-cased.
struct MCPHTTPRequest: Sendable, Equatable {
    var method: String
    var target: String
    var version: String = "HTTP/1.1"
    var headers: [String: String] = [:]
    var body = Data()

    init(method: String, target: String, version: String = "HTTP/1.1", headers: [String: String] = [:],
         body: Data = Data()) {
        self.method = method
        self.target = target
        self.version = version
        self.body = body
        for (name, value) in headers { self.headers[name.lowercased()] = value }
    }

    /// The path without the query string.
    var path: String {
        String(target.split(separator: "?", maxSplits: 1, omittingEmptySubsequences: false).first ?? "")
    }

    func header(_ name: String) -> String? { headers[name.lowercased()] }

    /// HTTP/1.0 without keep-alive, or an explicit `Connection: close`.
    var wantsClose: Bool {
        let connection = header("connection")?.lowercased() ?? ""
        if connection.contains("close") { return true }
        return version == "HTTP/1.0" && !connection.contains("keep-alive")
    }
}

/// One HTTP response.
struct MCPHTTPResponse: Sendable, Equatable {
    var status: Int
    var headers: [(String, String)] = []
    var body = Data()

    static func == (a: MCPHTTPResponse, b: MCPHTTPResponse) -> Bool {
        a.status == b.status && a.body == b.body && a.headers.map { $0.0 + ":" + $0.1 } == b.headers.map { $0.0 + ":" + $0.1 }
    }

    func header(_ name: String) -> String? {
        headers.first { $0.0.lowercased() == name.lowercased() }?.1
    }

    static func json(_ text: String, status: Int = 200, headers: [(String, String)] = []) -> MCPHTTPResponse {
        MCPHTTPResponse(status: status, headers: [("Content-Type", "application/json")] + headers, body: Data(text.utf8))
    }

    static func text(_ status: Int, _ message: String, headers: [(String, String)] = []) -> MCPHTTPResponse {
        MCPHTTPResponse(status: status, headers: [("Content-Type", "text/plain; charset=utf-8")] + headers,
                        body: Data(message.utf8))
    }

    static func reason(_ status: Int) -> String {
        switch status {
        case 200: return "OK"
        case 202: return "Accepted"
        case 204: return "No Content"
        case 400: return "Bad Request"
        case 401: return "Unauthorized"
        case 403: return "Forbidden"
        case 404: return "Not Found"
        case 405: return "Method Not Allowed"
        case 406: return "Not Acceptable"
        case 411: return "Length Required"
        case 413: return "Payload Too Large"
        case 415: return "Unsupported Media Type"
        case 431: return "Request Header Fields Too Large"
        case 500: return "Internal Server Error"
        case 501: return "Not Implemented"
        case 503: return "Service Unavailable"
        default: return "Status"
        }
    }

    /// The bytes on the wire (Content-Length always set; `close` adds Connection: close).
    func serialized(close: Bool = false) -> Data {
        var head = "HTTP/1.1 \(status) \(Self.reason(status))\r\n"
        for (name, value) in headers where name.lowercased() != "content-length" {
            head += "\(name): \(value)\r\n"
        }
        head += "Content-Length: \(body.count)\r\n"
        head += "Cache-Control: no-store\r\n"
        if close { head += "Connection: close\r\n" }
        head += "\r\n"
        var data = Data(head.utf8)
        data.append(body)
        return data
    }
}

/// Incremental HTTP/1.1 request parser: feed it the bytes as they arrive (any chunking), take requests out.
/// Bodies by Content-Length or chunked transfer coding.
struct MCPHTTPParser {
    enum Event: Equatable {
        case needMore
        case request(MCPHTTPRequest)
        /// A malformed or oversized request: answer with this status and close the connection.
        case failure(Int, String)
    }

    var maxHeaderBytes = 32 * 1024
    var maxBodyBytes = 16 * 1024 * 1024
    private(set) var buffer = Data()

    mutating func append(_ data: Data) { buffer.append(data) }

    /// The next complete request in the buffer (removing it), `.needMore`, or a failure.
    mutating func next() -> Event {
        let separator = Data("\r\n\r\n".utf8)
        guard let headEnd = buffer.range(of: separator) else {
            return buffer.count > maxHeaderBytes ? .failure(431, "Request headers too large") : .needMore
        }
        if headEnd.lowerBound - buffer.startIndex > maxHeaderBytes { return .failure(431, "Request headers too large") }
        let headText = String(decoding: buffer[buffer.startIndex..<headEnd.lowerBound], as: UTF8.self)
        var lines = headText.components(separatedBy: "\r\n")
        let requestLine = lines.removeFirst().split(separator: " ", omittingEmptySubsequences: true)
        guard requestLine.count == 3, requestLine[2].hasPrefix("HTTP/1.") else { return .failure(400, "Malformed request line") }
        var request = MCPHTTPRequest(method: String(requestLine[0]), target: String(requestLine[1]),
                                     version: String(requestLine[2]))
        for line in lines where !line.isEmpty {
            guard let colon = line.firstIndex(of: ":") else { return .failure(400, "Malformed header") }
            let name = line[..<colon].trimmingCharacters(in: .whitespaces).lowercased()
            let value = line[line.index(after: colon)...].trimmingCharacters(in: .whitespaces)
            guard !name.isEmpty else { return .failure(400, "Malformed header") }
            if let existing = request.headers[name] {
                request.headers[name] = existing + ", " + value
            } else {
                request.headers[name] = value
            }
        }
        let bodyStart = headEnd.upperBound
        let chunked = request.header("transfer-encoding")?.lowercased().contains("chunked") ?? false
        if chunked {
            switch Self.dechunk(buffer[bodyStart...], limit: maxBodyBytes) {
            case .incomplete: return .needMore
            case .invalid: return .failure(400, "Malformed chunked body")
            case .tooLarge: return .failure(413, "Request body too large")
            case .complete(let body, let end):
                request.body = body
                buffer = Data(buffer[end...])
                return .request(request)
            }
        }
        var length = 0
        if let text = request.header("content-length") {
            guard let value = Int(text), value >= 0 else { return .failure(400, "Invalid Content-Length") }
            length = value
        }
        guard length <= maxBodyBytes else { return .failure(413, "Request body too large") }
        let available = buffer.endIndex - bodyStart
        guard available >= length else { return .needMore }
        let bodyEnd = bodyStart + length
        request.body = Data(buffer[bodyStart..<bodyEnd])
        buffer = Data(buffer[bodyEnd...])
        return .request(request)
    }

    enum Dechunked {
        case incomplete, invalid, tooLarge
        case complete(Data, Data.Index)
    }

    /// Decodes a chunked body starting at `data.startIndex`; `.complete` gives the body and where the message ends.
    static func dechunk(_ data: Data.SubSequence, limit: Int) -> Dechunked {
        var body = Data()
        var at = data.startIndex
        let crlf = Data("\r\n".utf8)
        while true {
            guard let eol = data.range(of: crlf, in: at..<data.endIndex) else { return .incomplete }
            var sizeText = String(decoding: data[at..<eol.lowerBound], as: UTF8.self)
            if let semi = sizeText.firstIndex(of: ";") { sizeText = String(sizeText[..<semi]) }
            guard let size = Int(sizeText.trimmingCharacters(in: .whitespaces), radix: 16), size >= 0 else { return .invalid }
            at = eol.upperBound
            if size == 0 {
                // Optional trailers, then an empty line.
                while true {
                    guard let end = data.range(of: crlf, in: at..<data.endIndex) else { return .incomplete }
                    let isBlank = end.lowerBound == at
                    at = end.upperBound
                    if isBlank { return .complete(body, at) }
                }
            }
            guard body.count + size <= limit else { return .tooLarge }
            guard data.endIndex - at >= size + 2 else { return .incomplete }
            body.append(data[at..<(at + size)])
            at += size
            guard data[at] == 0x0D, data[at + 1] == 0x0A else { return .invalid }
            at += 2
        }
    }
}

/// The checks every request passes before it reaches the design: Host and Origin name this Mac (a web page cannot
/// use DNS rebinding to reach the endpoint), and the bearer token matches.
enum MCPRequestGate {
    static let loopbackHosts: Set<String> = ["127.0.0.1", "localhost", "::1", "[::1]"]

    /// "127.0.0.1:39717" → "127.0.0.1"; "[::1]:39717" → "::1".
    static func hostName(_ hostHeader: String) -> String {
        let value = hostHeader.trimmingCharacters(in: .whitespaces).lowercased()
        if value.hasPrefix("["), let close = value.firstIndex(of: "]") {
            return String(value[value.index(after: value.startIndex)..<close])
        }
        if let colon = value.lastIndex(of: ":"), value.filter({ $0 == ":" }).count == 1 {
            return String(value[..<colon])
        }
        return value
    }

    /// A missing Host (HTTP/1.0) is allowed; otherwise it must be a loopback name.
    static func isAllowedHost(_ host: String?) -> Bool {
        guard let host else { return true }
        return loopbackHosts.contains(hostName(host))
    }

    /// No Origin (command-line and desktop clients) is allowed; a browser origin must be http(s) on this Mac.
    static func isAllowedOrigin(_ origin: String?) -> Bool {
        guard let origin = origin?.trimmingCharacters(in: .whitespaces), !origin.isEmpty else { return true }
        guard let url = URL(string: origin), let scheme = url.scheme?.lowercased(), scheme == "http" || scheme == "https",
              let host = url.host?.lowercased() else { return false }
        return loopbackHosts.contains(host)
    }

    /// The token of an `Authorization: Bearer <token>` header.
    static func bearerToken(_ authorization: String?) -> String? {
        guard let value = authorization?.trimmingCharacters(in: .whitespaces) else { return nil }
        let parts = value.split(separator: " ", maxSplits: 1)
        guard parts.count == 2, parts[0].lowercased() == "bearer" else { return nil }
        let token = parts[1].trimmingCharacters(in: .whitespaces)
        return token.isEmpty ? nil : token
    }

    /// Compares in time independent of where the strings differ.
    static func tokensMatch(_ a: String, _ b: String) -> Bool {
        let x = Array(a.utf8), y = Array(b.utf8)
        guard x.count == y.count, !x.isEmpty else { return false }
        var diff: UInt8 = 0
        for i in 0..<x.count { diff |= x[i] ^ y[i] }
        return diff == 0
    }

    /// nil when the request may go on; otherwise the refusal (403 for a foreign Host / Origin, 401 for the token).
    static func refusal(for request: MCPHTTPRequest, token: String) -> MCPHTTPResponse? {
        guard isAllowedHost(request.header("host")) else {
            return .text(403, "Forbidden: only requests to this Mac (127.0.0.1 / localhost) are accepted.")
        }
        guard isAllowedOrigin(request.header("origin")) else {
            return .text(403, "Forbidden: requests from web pages on other origins are refused.")
        }
        guard let given = bearerToken(request.header("authorization")), tokensMatch(given, token) else {
            return .text(401, "Unauthorized: send Authorization: Bearer <token> (Settings → AI Access (MCP)).",
                         headers: [("WWW-Authenticate", "Bearer realm=\"SiEDA\"")])
        }
        return nil
    }

    /// A new random token: 32 bytes, base64url.
    static func generateToken() -> String {
        var bytes = [UInt8](repeating: 0, count: 32)
        if SecRandomCopyBytes(kSecRandomDefault, bytes.count, &bytes) != errSecSuccess {
            for i in bytes.indices { bytes[i] = UInt8.random(in: 0...255) }
        }
        return Data(bytes).base64EncodedString()
            .replacingOccurrences(of: "+", with: "-")
            .replacingOccurrences(of: "/", with: "_")
            .replacingOccurrences(of: "=", with: "")
    }
}

/// The TCP listener: 127.0.0.1 only, one `MCPHTTPConnection` per client. Requests are answered by `handler`
/// (the live server hops to the main actor there).
final class MCPEndpoint: @unchecked Sendable {
    enum State: Equatable, Sendable {
        case starting
        case listening(UInt16)
        case failed(String)
        case stopped
    }

    typealias Handler = @Sendable (MCPHTTPRequest) async -> MCPHTTPResponse

    let requestedPort: UInt16
    private let handler: Handler
    private let queue = DispatchQueue(label: "sieda.mcp.endpoint")
    private var listener: NWListener?
    // Guarded by `queue`.
    private var connections: [ObjectIdentifier: MCPHTTPConnection] = [:]
    private var onState: (@Sendable (State) -> Void)?
    private var onConnections: (@Sendable (Int) -> Void)?

    init(port: UInt16, handler: @escaping Handler) {
        requestedPort = port
        self.handler = handler
    }

    /// TCP on the loopback interface only: the listener's local endpoint is 127.0.0.1 (never 0.0.0.0) and only
    /// local connections are accepted. Port 0 = any free port.
    static func parameters(port: UInt16) -> NWParameters {
        let parameters = NWParameters.tcp
        parameters.acceptLocalOnly = true
        parameters.allowLocalEndpointReuse = true
        let nwPort = NWEndpoint.Port(rawValue: port) ?? .any
        parameters.requiredLocalEndpoint = NWEndpoint.hostPort(host: .ipv4(IPv4Address.loopback), port: nwPort)
        return parameters
    }

    /// Starts listening; `onState` reports .listening(port) (the real port when 0 was asked) or .failed.
    func start(onState: @escaping @Sendable (State) -> Void, onConnections: @escaping @Sendable (Int) -> Void) {
        queue.async {
            self.onState = onState
            self.onConnections = onConnections
            do {
                let listener = try NWListener(using: Self.parameters(port: self.requestedPort))
                self.listener = listener
                listener.stateUpdateHandler = { [weak self, weak listener] state in
                    guard let self else { return }
                    switch state {
                    case .ready:
                        let port = listener?.port?.rawValue ?? self.requestedPort
                        self.onState?(.listening(port))
                    case .failed(let error):
                        self.onState?(.failed(Self.describe(error)))
                        listener?.cancel()
                    case .waiting(let error):
                        // Usually: the port is in use by another program.
                        self.onState?(.failed(Self.describe(error)))
                        listener?.cancel()
                    case .cancelled:
                        self.onState?(.stopped)
                    default:
                        break
                    }
                }
                listener.newConnectionHandler = { [weak self] connection in self?.accept(connection) }
                onState(.starting)
                listener.start(queue: self.queue)
            } catch {
                onState(.failed(Self.describe(error)))
            }
        }
    }

    func stop() {
        queue.async {
            self.listener?.cancel()
            self.listener = nil
            for connection in self.connections.values { connection.cancel() }
            self.connections.removeAll()
            self.onConnections?(0)
        }
    }

    private static func describe(_ error: NWError) -> String {
        if case .posix(let code) = error, code == .EADDRINUSE { return "Port in use by another program" }
        return error.localizedDescription
    }

    private static func describe(_ error: Error) -> String {
        if let nw = error as? NWError { return describe(nw) }
        return error.localizedDescription
    }

    /// At most this many clients at once: AI clients use one or two; a local program opening thousands of
    /// connections cannot exhaust the app's memory or file descriptors.
    static let maxConnections = 16

    /// Runs on `queue`.
    private func accept(_ connection: NWConnection) {
        guard connections.count < Self.maxConnections else {
            connection.cancel()
            return
        }
        let client = MCPHTTPConnection(connection: connection, queue: queue, handler: handler)
        let key = ObjectIdentifier(client)
        connections[key] = client
        onConnections?(connections.count)
        client.onClose = { [weak self] in
            guard let self else { return }
            self.connections[key] = nil
            self.onConnections?(self.connections.count)
        }
        client.start()
    }
}

/// One client connection: reads requests (keep-alive, one at a time), answers each through the handler.
/// Everything runs on the endpoint's queue.
final class MCPHTTPConnection: @unchecked Sendable {
    private let connection: NWConnection
    private let queue: DispatchQueue
    private let handler: MCPEndpoint.Handler
    private var parser = MCPHTTPParser()
    private var receiving = false
    private var processing = false
    private var closed = false
    var onClose: (() -> Void)?

    init(connection: NWConnection, queue: DispatchQueue, handler: @escaping MCPEndpoint.Handler) {
        self.connection = connection
        self.queue = queue
        self.handler = handler
    }

    func start() {
        connection.stateUpdateHandler = { [weak self] state in
            switch state {
            case .failed, .cancelled: self?.finish()
            default: break
            }
        }
        connection.start(queue: queue)
        drain()
    }

    func cancel() { connection.cancel() }

    private func finish() {
        guard !closed else { return }
        closed = true
        onClose?()
        onClose = nil
    }

    /// Answers the next complete request, or reads more bytes.
    private func drain() {
        guard !processing, !closed else { return }
        switch parser.next() {
        case .needMore:
            receive()
        case .failure(let status, let message):
            send(.text(status, message), close: true)
        case .request(let request):
            processing = true
            let handler = self.handler
            Task {
                let response = await handler(request)
                self.queue.async {
                    self.send(response, close: request.wantsClose)
                }
            }
        }
    }

    private func receive() {
        guard !receiving, !closed else { return }
        receiving = true
        connection.receive(minimumIncompleteLength: 1, maximumLength: 65_536) { [weak self] data, _, isComplete, error in
            guard let self else { return }
            self.receiving = false
            if let data, !data.isEmpty { self.parser.append(data) }
            if error != nil || (isComplete && (data?.isEmpty ?? true)) {
                // The client closed its side (or the connection failed): nothing more can arrive.
                self.connection.cancel()
                return
            }
            self.drain()
        }
    }

    private func send(_ response: MCPHTTPResponse, close: Bool) {
        connection.send(content: response.serialized(close: close), completion: .contentProcessed { [weak self] _ in
            guard let self else { return }
            self.processing = false
            if close {
                self.connection.cancel()
            } else {
                self.drain()
            }
        })
    }
}
