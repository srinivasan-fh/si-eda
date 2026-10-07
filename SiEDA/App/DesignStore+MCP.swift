import AppKit
import Foundation

// The app's live MCP endpoint, design half (docs/MCP.md, "Live app mode"): an AI client drives the design that is
// open in the window through the core's MCP engine (sieda_mcp_* in sieda_c.h), with undo.
//
// - MCPLiveServer: settings (off by default), the bearer token (Keychain), the listener's status, HTTP routing
//   (Streamable HTTP: POST /mcp → JSON; GET → 405; DELETE ends a session; Mcp-Session-Id).
// - MCPStoreBridge: runs one JSON-RPC message against the store's engine on the main actor. A tool that changes the
//   design runs inside `DesignStore.performExternalEdit("AI: <tool>")`: one AI tool call = one undo step (none when
//   it changed nothing), and the canvas refreshes. project_new / project_load_example replace the design in place (undoable); project_open goes through
//   the store's normal open path (refused while the design has unsaved changes); project_save without a path saves
//   the window's document like File > Save.

/// Where the bearer token is kept: the macOS Keychain in the app, memory in tests.
protocol MCPTokenStore: AnyObject {
    func readToken() -> String
    func writeToken(_ token: String)
}

final class KeychainMCPTokenStore: MCPTokenStore {
    private let account = "mcp.live-token"
    func readToken() -> String { KeychainStore.read(account) }
    func writeToken(_ token: String) { KeychainStore.write(token, for: account) }
}

final class MemoryMCPTokenStore: MCPTokenStore {
    var token: String
    init(token: String = "") { self.token = token }
    func readToken() -> String { token }
    func writeToken(_ token: String) { self.token = token }
}

/// Tools the change callback reported during one `sieda_mcp_handle` call (it runs synchronously inside it, on the
/// main thread).
final class MCPChangeLog {
    var tools: [String] = []
}

private let mcpChangeCallback: SiedaMcpChangeCallback = { user, tool in
    guard let user else { return }
    let log = Unmanaged<MCPChangeLog>.fromOpaque(user).takeUnretainedValue()
    log.tools.append(tool.map { String(cString: $0) } ?? "")
}

/// What one MCP message did.
struct MCPBridgeOutcome {
    /// JSON-RPC response text; "" when nothing is to be answered (notifications).
    var response: String
    /// The tool called (tools/call), if any.
    var tool: String?
    /// The design changed (an undo step was recorded).
    var changed = false
}

/// Runs MCP messages against a `DesignStore`'s engine. Owns the core MCP server. Main actor only (the engine's
/// project belongs to the main actor; the core server is not thread-safe).
final class MCPStoreBridge {
    private let server: OpaquePointer
    private let changes = MCPChangeLog()
    private var appliedOptions = ""

    /// Tool name → changes the design (from the core's tool table).
    static let mutatingTools: [String: Bool] = {
        guard let json = EDAEngine.take(sieda_mcp_tools_json()),
              let list = try? JSONSerialization.jsonObject(with: Data(json.utf8)) as? [[String: Any]] else { return [:] }
        var table: [String: Bool] = [:]
        for tool in list {
            if let name = tool["name"] as? String { table[name] = tool["mutates"] as? Bool ?? false }
        }
        return table
    }()

    /// Tools that replace the whole design in place (the views re-fit; Undo brings the previous design back).
    static let replacingTools: Set<String> = ["project_new", "project_load_example"]

    init() {
        guard let server = sieda_mcp_new("{\"serverName\":\"sieda-app\"}") else {
            fatalError("SiEDA core failed to create the MCP server")
        }
        self.server = server
        sieda_mcp_set_change_callback(server, mcpChangeCallback, Unmanaged.passUnretained(changes).toOpaque())
    }

    deinit {
        sieda_mcp_set_change_callback(server, nil, nil)
        sieda_mcp_free(server)
    }

    /// Options for the core: read-only and the folder files may be read from / written to ("" = no file access).
    static func optionsJSON(readOnly: Bool, allowedRoot: String) -> String {
        let options: [String: Any] = ["readOnly": readOnly, "allowedRoot": allowedRoot, "serverName": "sieda-app"]
        guard let data = try? JSONSerialization.data(withJSONObject: options, options: [.sortedKeys]) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }

    private(set) var readOnly = false
    private(set) var allowedRoot = ""

    func configure(readOnly: Bool, allowedRoot: String) {
        self.readOnly = readOnly
        self.allowedRoot = allowedRoot
        let json = Self.optionsJSON(readOnly: readOnly, allowedRoot: allowedRoot)
        guard json != appliedOptions else { return }
        if sieda_mcp_set_options(server, json) == 1 { appliedOptions = json }
    }

    // MARK: - Messages

    /// One HTTP body: a JSON-RPC message or a batch (array) of them.
    @MainActor
    func handle(_ body: String, store: DesignStore) -> [MCPBridgeOutcome] {
        let parsed = try? JSONSerialization.jsonObject(with: Data(body.utf8), options: [.fragmentsAllowed])
        guard let batch = parsed as? [Any] else {
            return [handleOne(body, message: parsed as? [String: Any], store: store)]
        }
        return batch.map { (element: Any) -> MCPBridgeOutcome in
            guard let message = element as? [String: Any],
                  let data = try? JSONSerialization.data(withJSONObject: message) else {
                return MCPBridgeOutcome(response: Self.rpcError(id: NSNull(), code: -32600, message: "Invalid Request"))
            }
            return handleOne(String(decoding: data, as: UTF8.self), message: message, store: store)
        }
    }

    /// The responses of a `handle` call as one HTTP body ("" when there is none to send).
    static func responseBody(_ outcomes: [MCPBridgeOutcome], batch: Bool) -> String {
        let texts = outcomes.map(\.response).filter { !$0.isEmpty }
        if texts.isEmpty { return "" }
        return batch ? "[" + texts.joined(separator: ",") + "]" : texts[0]
    }

    @MainActor
    private func handleOne(_ text: String, message: [String: Any]?, store: DesignStore) -> MCPBridgeOutcome {
        let method = message?["method"] as? String
        let params = message?["params"] as? [String: Any]
        let id: Any? = message?["id"]
        guard method == "tools/call", let name = params?["name"] as? String else {
            if store.isBusy, method == "resources/read", let id {
                return MCPBridgeOutcome(response: Self.rpcError(id: id, code: -32603, message: busyText(store)))
            }
            return MCPBridgeOutcome(response: callCore(text, store: store))
        }
        let arguments = params?["arguments"] as? [String: Any] ?? [:]
        if store.isBusy {
            return MCPBridgeOutcome(response: Self.toolResult(id: id, text: busyText(store), isError: true), tool: name)
        }
        if let special = specialTool(name, arguments: arguments, id: id, store: store) { return special }
        let mutates = Self.mutatingTools[name] ?? false
        guard mutates, !readOnly else {
            return MCPBridgeOutcome(response: callCore(text, store: store), tool: name)
        }
        var response = ""
        changes.tools.removeAll()
        let changed = store.performExternalEdit("AI: \(name)") { _ in
            response = self.callCore(text, store: store)
            return !self.changes.tools.isEmpty
        }
        if changed, Self.replacingTools.contains(name) { store.noteDesignReplaced() }
        return MCPBridgeOutcome(response: response, tool: name, changed: changed)
    }

    @MainActor
    private func busyText(_ store: DesignStore) -> String {
        let what = store.busyMessage.isEmpty ? "working" : store.busyMessage
        return "SiEDA is busy (\(what)). Try again when it finishes."
    }

    /// The core engine on the store's project. The engine lock is held for the call; the change callback only
    /// records the tool name (it must not call back into the engine).
    @MainActor
    private func callCore(_ text: String, store: DesignStore) -> String {
        let path = store.documentURL?.path ?? ""
        let server = self.server
        if store.isBusy {
            // Background work holds the engine. Only messages that never touch the project get here (initialize,
            // ping, tools/list, prompts …; tool calls and resources/read were answered "busy"), so skip the lock.
            return EDAEngine.take(sieda_mcp_handle(server, text)) ?? ""
        }
        return store.engine.withHandle { (project: OpaquePointer) -> String in
            // Undo and Open swap the engine's project, so attach on every call (a no-op when it is the same one).
            _ = sieda_mcp_attach_project(server, project)
            _ = sieda_mcp_set_project_path(server, path)
            return EDAEngine.take(sieda_mcp_handle(server, text)) ?? ""
        }
    }

    // MARK: - Tools the app handles itself

    @MainActor
    private func specialTool(_ name: String, arguments: [String: Any], id: Any?, store: DesignStore) -> MCPBridgeOutcome? {
        switch name {
        case "project_open" where !readOnly:
            return openProject(arguments["path"] as? String ?? "", id: id, store: store)
        case "project_save" where !readOnly && (arguments["path"] as? String ?? "").isEmpty && store.documentURL != nil:
            let saved = store.save()
            let path = store.documentURL?.path ?? ""
            let info: [String: Any] = ["saved": path, "document": true]
            let text = saved ? Self.jsonText(info) : "Saving failed: \(store.statusMessage)"
            return MCPBridgeOutcome(response: Self.toolResult(id: id, text: text, isError: !saved), tool: name)
        default:
            return nil
        }
    }

    /// project_open in the app: the store's normal open path (the window's document becomes that file; undo history
    /// starts afresh), so it is refused while the open design has unsaved changes.
    @MainActor
    private func openProject(_ path: String, id: Any?, store: DesignStore) -> MCPBridgeOutcome {
        func refuse(_ text: String) -> MCPBridgeOutcome {
            MCPBridgeOutcome(response: Self.toolResult(id: id, text: text, isError: true), tool: "project_open")
        }
        if store.isDirty {
            return refuse("The design open in SiEDA has unsaved changes. Ask the person to save (File > Save) or "
                          + "discard them first, or call project_save.")
        }
        let url: URL
        switch Self.resolve(path, root: allowedRoot) {
        case .failure(let message): return refuse(message)
        case .success(let resolved): url = resolved
        }
        store.open(url: url)
        guard store.documentURL?.standardizedFileURL == url.standardizedFileURL else {
            return refuse("Could not open \(url.lastPathComponent): \(store.alert?.message ?? store.statusMessage)")
        }
        let summary = Self.request(id: id, tool: "project_summary")
        return MCPBridgeOutcome(response: callCore(summary, store: store), tool: "project_open", changed: true)
    }

    enum Resolved: Equatable {
        case success(URL)
        case failure(String)
    }

    /// A project file inside `root` (relative paths resolve against it; "..", other absolute paths and symbolic
    /// links leading out of it are refused) — the same rule as the core's sandbox.
    static func resolve(_ path: String, root: String) -> Resolved {
        guard !root.isEmpty else {
            return .failure("File access is off: save the design first or choose an allowed folder in SiEDA's "
                            + "Settings > AI Access (MCP).")
        }
        guard !path.isEmpty else { return .failure("A path is required.") }
        if path.split(separator: "/").contains("..") { return .failure("Paths may not contain \"..\": \(path)") }
        let rootURL = URL(fileURLWithPath: root, isDirectory: true).standardizedFileURL.resolvingSymlinksInPath()
        let given = path.hasPrefix("/") ? URL(fileURLWithPath: path) : rootURL.appendingPathComponent(path)
        let full = given.standardizedFileURL.resolvingSymlinksInPath()
        let rootPath = rootURL.path.hasSuffix("/") ? rootURL.path : rootURL.path + "/"
        guard full.path.hasPrefix(rootPath) else { return .failure("Outside the allowed folder \(rootURL.path): \(path)") }
        var isDirectory: ObjCBool = false
        guard FileManager.default.fileExists(atPath: full.path, isDirectory: &isDirectory), !isDirectory.boolValue else {
            return .failure("No such project file: \(path)")
        }
        return .success(full)
    }

    // MARK: - JSON-RPC helpers

    static func jsonText(_ object: Any) -> String {
        guard JSONSerialization.isValidJSONObject(object),
              let data = try? JSONSerialization.data(withJSONObject: object, options: [.sortedKeys]) else { return "{}" }
        return String(decoding: data, as: UTF8.self)
    }

    /// A CallToolResult with one text item ("" when `id` is nil: a notification gets no answer).
    static func toolResult(id: Any?, text: String, isError: Bool) -> String {
        guard let id else { return "" }
        let content: [[String: Any]] = [["type": "text", "text": text]]
        let result: [String: Any] = ["content": content, "isError": isError]
        let message: [String: Any] = ["jsonrpc": "2.0", "id": id, "result": result]
        return jsonText(message)
    }

    static func rpcError(id: Any, code: Int, message: String) -> String {
        let error: [String: Any] = ["code": code, "message": message]
        let reply: [String: Any] = ["jsonrpc": "2.0", "id": id, "error": error]
        return jsonText(reply)
    }

    /// A tools/call request for `tool` without arguments, answered under `id`.
    static func request(id: Any?, tool: String) -> String {
        let params: [String: Any] = ["name": tool, "arguments": [String: Any]()]
        var message: [String: Any] = ["jsonrpc": "2.0", "method": "tools/call", "params": params]
        if let id { message["id"] = id }
        return jsonText(message)
    }
}

extension DesignStore {
    /// The folder an MCP client may read from and write into: the user's choice, else the open document's folder,
    /// else none ("" = no file access).
    func mcpAllowedRoot(chosen: String) -> String {
        if !chosen.isEmpty { return chosen }
        return documentURL?.deletingLastPathComponent().path ?? ""
    }
}

/// The live MCP server of the app (Settings → AI Access (MCP)). Off by default; listens on 127.0.0.1 only.
@MainActor
final class MCPLiveServer: ObservableObject {
    static let shared = MCPLiveServer()
    static let defaultPort = 39717
    /// Protocol revisions the core answers (MCP-Protocol-Version header).
    static let protocolVersions: Set<String> = ["2025-06-18", "2025-03-26", "2024-11-05"]

    enum Status: Equatable {
        case off
        case starting
        case listening(UInt16)
        case failed(String)
    }

    struct CallRecord: Equatable {
        var tool: String
        var date: Date
        var changed: Bool
    }

    private enum Key {
        static let enabled = "mcp.live.enabled"
        static let port = "mcp.live.port"
        static let readOnly = "mcp.live.readOnly"
        static let folder = "mcp.live.allowedFolder"
    }

    private let defaults: UserDefaults
    private let tokens: MCPTokenStore

    @Published var enabled: Bool {
        didSet {
            guard enabled != oldValue else { return }
            defaults.set(enabled, forKey: Key.enabled)
            restart()
        }
    }
    /// TCP port on 127.0.0.1 (0 = any free port, for tests).
    @Published var port: Int {
        didSet {
            guard port != oldValue else { return }
            defaults.set(port, forKey: Key.port)
            if enabled { restart() }
        }
    }
    @Published var readOnly: Bool {
        didSet { defaults.set(readOnly, forKey: Key.readOnly) }
    }
    /// Folder the AI may open projects from and write outputs into; "" = the open document's folder.
    @Published var allowedFolder: String {
        didSet { defaults.set(allowedFolder, forKey: Key.folder) }
    }
    @Published private(set) var status: Status = .off
    @Published private(set) var connections = 0
    @Published private(set) var sessions: Set<String> = []
    @Published private(set) var lastCall: CallRecord?
    @Published private(set) var callCount = 0
    /// Bumped when the token changes (views showing it re-read).
    @Published private(set) var tokenRevision = 0

    /// The window's design (the app has one store for its lifetime).
    private(set) var store: DesignStore?
    private var endpoint: MCPEndpoint?
    private var bridge: MCPStoreBridge?
    private var cachedToken: String?

    init(defaults: UserDefaults = .standard, tokens: MCPTokenStore = KeychainMCPTokenStore()) {
        self.defaults = defaults
        self.tokens = tokens
        enabled = defaults.bool(forKey: Key.enabled)  // false unless the user turned it on
        let saved = defaults.integer(forKey: Key.port)
        port = defaults.object(forKey: Key.port) == nil ? Self.defaultPort : saved
        readOnly = defaults.bool(forKey: Key.readOnly)
        allowedFolder = defaults.string(forKey: Key.folder) ?? ""
    }

    /// The bearer token clients must send; created (and stored in the Keychain) on first use.
    var token: String {
        if let cachedToken { return cachedToken }
        var value = tokens.readToken()
        if value.isEmpty {
            value = MCPRequestGate.generateToken()
            tokens.writeToken(value)
        }
        cachedToken = value
        return value
    }

    /// A new token: every client must be given it again; open sessions end.
    func regenerateToken() {
        let value = MCPRequestGate.generateToken()
        tokens.writeToken(value)
        cachedToken = value
        sessions.removeAll()
        tokenRevision &+= 1
    }

    /// Connects the server to the window's design and starts listening when enabled.
    func attach(_ store: DesignStore) {
        guard self.store !== store else { return }
        self.store = store
        restart()
    }

    var endpointURL: String { "http://127.0.0.1:\(boundPort ?? UInt16(clamping: port))/mcp" }

    var boundPort: UInt16? {
        if case .listening(let port) = status { return port }
        return nil
    }

    var isListening: Bool { boundPort != nil }

    /// True for `seconds` after an AI client's tool call.
    func recentlyActive(within seconds: TimeInterval = 30, now: Date = Date()) -> Bool {
        guard let lastCall else { return false }
        return now.timeIntervalSince(lastCall.date) < seconds
    }

    // MARK: - Listener

    private func restart() {
        endpoint?.stop()
        endpoint = nil
        connections = 0
        sessions.removeAll()
        guard enabled, store != nil else {
            status = .off
            return
        }
        guard (0...65_535).contains(port) else {
            status = .failed("Invalid port \(port)")
            return
        }
        _ = token  // make sure one exists before the first client arrives
        let endpoint = MCPEndpoint(port: UInt16(port)) { [weak self] request in
            await self?.respond(to: request) ?? .text(503, "SiEDA is shutting down")
        }
        self.endpoint = endpoint
        status = .starting
        endpoint.start(onState: { [weak self, weak endpoint] state in
            Task { @MainActor in
                guard let self, let endpoint, self.endpoint === endpoint else { return }
                self.apply(state)
            }
        }, onConnections: { [weak self, weak endpoint] count in
            Task { @MainActor in
                guard let self, let endpoint, self.endpoint === endpoint else { return }
                self.connections = count
            }
        })
    }

    private func apply(_ state: MCPEndpoint.State) {
        switch state {
        case .starting: status = .starting
        case .listening(let port): status = .listening(port)
        case .failed(let message): status = .failed(message)
        case .stopped: if case .listening = status { status = .off }
        }
    }

    // MARK: - HTTP

    /// Answers one HTTP request (Streamable HTTP transport, JSON responses).
    func respond(to request: MCPHTTPRequest) async -> MCPHTTPResponse {
        guard request.path == "/mcp" else { return .text(404, "Not found: the MCP endpoint is /mcp") }
        guard enabled, let store else { return .text(503, "The SiEDA live MCP server is off") }
        if let refusal = MCPRequestGate.refusal(for: request, token: token) { return refusal }
        let sessionHeader = request.header("mcp-session-id")
        switch request.method {
        case "POST":
            break
        case "DELETE":
            guard let id = sessionHeader, sessions.remove(id) != nil else { return .text(404, "Unknown session") }
            return MCPHTTPResponse(status: 204)
        default:
            // No server-initiated stream (GET): every answer comes back on its POST.
            return .text(405, "Method not allowed: POST JSON-RPC messages to /mcp", headers: [("Allow", "POST, DELETE")])
        }
        if let version = request.header("mcp-protocol-version"), !Self.protocolVersions.contains(version) {
            return .text(400, "Unsupported MCP-Protocol-Version \(version)")
        }
        if let id = sessionHeader, !sessions.contains(id) {
            return .text(404, "Unknown or expired session: initialize again")
        }
        if let type = request.header("content-type")?.lowercased(), !type.contains("json") {
            return .text(415, "Content-Type must be application/json")
        }
        if let accept = request.header("accept")?.lowercased(), !Self.acceptsJSON(accept) {
            return .text(406, "This endpoint answers with application/json")
        }
        let body = String(decoding: request.body, as: UTF8.self)
        return answer(body, store: store)
    }

    static func acceptsJSON(_ accept: String) -> Bool {
        accept.contains("application/json") || accept.contains("*/*") || accept.contains("application/*")
    }

    private func answer(_ body: String, store: DesignStore) -> MCPHTTPResponse {
        let bridge = self.bridge ?? MCPStoreBridge()
        self.bridge = bridge
        bridge.configure(readOnly: readOnly, allowedRoot: store.mcpAllowedRoot(chosen: allowedFolder))
        let trimmed = body.trimmingCharacters(in: .whitespacesAndNewlines)
        let outcomes = bridge.handle(body, store: store)
        for outcome in outcomes {
            if let tool = outcome.tool { record(tool, changed: outcome.changed) }
        }
        let text = MCPStoreBridge.responseBody(outcomes, batch: trimmed.hasPrefix("["))
        guard !text.isEmpty else { return MCPHTTPResponse(status: 202) }
        var headers: [(String, String)] = []
        if Self.isInitialize(body), text.contains("\"result\"") {
            let session = UUID().uuidString
            sessions.insert(session)
            headers.append(("Mcp-Session-Id", session))
        }
        return .json(text, headers: headers)
    }

    private static func isInitialize(_ body: String) -> Bool {
        let message = try? JSONSerialization.jsonObject(with: Data(body.utf8)) as? [String: Any]
        return message?["method"] as? String == "initialize"
    }

    private func record(_ tool: String, changed: Bool) {
        lastCall = CallRecord(tool: tool, date: Date(), changed: changed)
        callCount += 1
        CrashReporter.note("MCP: \(tool)")
    }

    // MARK: - Client configuration snippets

    static func claudeCodeCommand(url: String, token: String) -> String {
        "claude mcp add --transport http sieda-app \(url) --header \"Authorization: Bearer \(token)\""
    }

    static func cursorConfig(url: String, token: String) -> String {
        """
        {
          "mcpServers": {
            "sieda-app": {
              "url": "\(url)",
              "headers": { "Authorization": "Bearer \(token)" }
            }
          }
        }
        """
    }

    static func vsCodeConfig(url: String, token: String) -> String {
        """
        {
          "servers": {
            "sieda-app": {
              "type": "http",
              "url": "\(url)",
              "headers": { "Authorization": "Bearer \(token)" }
            }
          }
        }
        """
    }

    /// Claude Desktop launches stdio servers only: `sieda-mcp --connect` bridges to the app.
    static func claudeDesktopConfig(url: String, token: String, bridgePath: String) -> String {
        """
        {
          "mcpServers": {
            "sieda-app": {
              "command": "\(bridgePath)",
              "args": ["--connect", "\(url)"],
              "env": { "SIEDA_MCP_TOKEN": "\(token)" }
            }
          }
        }
        """
    }

    /// The sieda-mcp executable when it ships inside the app, else where a source build puts it.
    static var bridgeExecutablePath: String {
        Bundle.main.url(forAuxiliaryExecutable: "sieda-mcp")?.path ?? "/path/to/si-eda/build/sieda-mcp"
    }
}
