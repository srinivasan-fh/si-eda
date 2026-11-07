import SwiftUI

/// si-llm (github.com/kernelsrinivas/si-llm): SiEDA's default model, first in Window → Super Intelligence. It runs as its
/// own small server on this Mac (127.0.0.1:8765, `python3 -m py.si.server` in its folder), turns a sentence into tool calls
/// with its own PCB / CAD model and sends them through its MCP client. Connected to the app's live MCP server, which
/// offers si-llm's tool names (`SiLlm.toolsEnabled`, core McpSiLlm.cpp), each call edits the open design as one undo step.
enum SiLlm {
    /// User default: the live MCP server offers si-llm's tool names (pcb_add, sch_wire, pcb_flow …). On unless turned off.
    static let toolsKey = "mcp.siLlmTools"
    static var toolsEnabled: Bool { UserDefaults.standard.object(forKey: toolsKey) as? Bool ?? true }

    static let serverURL = URL(string: "http://127.0.0.1:8765")!

    struct Health: Equatable {
        var model: String
        var parameters: Int?
        /// Connected MCP servers that offer SiEDA's si-llm tool names (the app, or sieda-mcp --si-llm).
        var drivesSiEDA: Bool
        var servers: [String]
    }

    /// One exchange: what was asked, si-llm's answer, the tool calls it made or wants approved.
    struct Turn: Identifiable, Equatable {
        let id = UUID()
        var prompt: String
        var reply: String
        var calls: [String]
        var proposal: String?
    }

    private static let session: URLSession = {
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 120  // a long tool call (a whole-board route) can take a while
        return URLSession(configuration: config)
    }()

    /// si-llm's /health, or nil when it is not running.
    static func health() async -> Health? {
        var request = URLRequest(url: serverURL.appendingPathComponent("health"))
        request.timeoutInterval = 2
        guard let (data, _) = try? await session.data(for: request),
              let json = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              json["ok"] as? Bool == true else { return nil }
        let model = json["model"] as? [String: Any] ?? [:]
        let tools = json["tools"] as? [String] ?? []
        let servers = (json["servers"] as? [[String: Any]] ?? []).compactMap { row -> String? in
            guard row["ok"] as? Bool == true else { return nil }
            return (row["server"] as? String).flatMap { $0.isEmpty ? nil : $0 } ?? row["id"] as? String
        }
        // sch_wire is only on servers offering si-llm's names; the app's server reports itself as sieda-app.
        let drives = tools.contains { $0.hasSuffix(".sch_wire") } && servers.contains { $0.hasPrefix("sieda") }
        return Health(model: model["name"] as? String ?? "si-llm", parameters: model["params"] as? Int,
                      drivesSiEDA: drives, servers: servers)
    }

    /// Sends a sentence to si-llm (/ask). mode: si-llm's approval mode ("man" asks before every change).
    static func ask(_ text: String) async throws -> Turn {
        let json = try await post("ask", ["text": String(text.prefix(500))])
        return try turn(prompt: text, json)
    }

    /// Answers a proposal: "allow", "always" or "deny".
    static func decide(_ proposal: String, _ decision: String, prompt: String) async throws -> Turn {
        let json = try await post("approve", ["proposalId": proposal, "decision": decision])
        return try turn(prompt: prompt, json)
    }

    private static func post(_ path: String, _ body: [String: Any]) async throws -> [String: Any] {
        var request = URLRequest(url: serverURL.appendingPathComponent(path))
        request.httpMethod = "POST"
        request.setValue("application/json", forHTTPHeaderField: "Content-Type")
        request.httpBody = try JSONSerialization.data(withJSONObject: body)
        let (data, response) = try await session.data(for: request)
        guard let json = try JSONSerialization.jsonObject(with: data) as? [String: Any] else {
            throw AIProviderError.invalidResponse("si-llm sent no JSON")
        }
        if let http = response as? HTTPURLResponse, http.statusCode != 200 {
            throw AIProviderError.http(status: http.statusCode, body: json["error"] as? String ?? "")
        }
        return json
    }

    private static func turn(prompt: String, _ json: [String: Any]) throws -> Turn {
        if json["ok"] as? Bool == false { throw AIProviderError.invalidResponse(json["error"] as? String ?? "si-llm refused") }
        let calls = (json["calls"] as? [[String: Any]] ?? []).map { call -> String in
            let args = (call["args"] as? [[String: Any]] ?? []).map { "\($0["key"] ?? "")=\($0["value"] ?? "")" }
            return "\(call["server"] ?? "").\(call["tool"] ?? "")(\(args.joined(separator: ", ")))"
        }
        let needs = json["needsApproval"] as? Bool == true
        return Turn(prompt: prompt, reply: json["reply"] as? String ?? "", calls: calls,
                    proposal: needs ? json["proposalId"] as? String : nil)
    }

    /// The command that connects si-llm to the app's live server (run in si-llm's folder).
    @MainActor static func connectCommand(url: String, token: String) -> String {
        MCPLiveServer.siLlmCommand(url: url, token: token, bridgePath: MCPLiveServer.bridgeExecutablePath)
    }
}

/// Window → Super Intelligence → si-llm: status, the two set-up commands, and a conversation with si-llm that edits the
/// open design through the live MCP server (each change asks first unless si-llm's mode allows it).
struct SiLlmView: View {
    @ObservedObject private var mcp = MCPLiveServer.shared
    @State private var health: SiLlm.Health?
    @State private var checked = false
    @State private var draft = ""
    @State private var turns: [SiLlm.Turn] = []
    @State private var busy = false
    @State private var failure: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: "brain.head.profile").font(.title2).foregroundStyle(Theme.skyBlue)
                VStack(alignment: .leading, spacing: 2) {
                    Text(verbatim: "si-llm").font(.title3.weight(.semibold))
                    Text("SiEDA's default model").font(.caption).foregroundStyle(Theme.textMuted)
                }
                Spacer()
                if let health {
                    Text(verbatim: health.model + (health.parameters.map { " · \($0 / 1_000_000)M" } ?? ""))
                        .font(.caption.monospaced()).foregroundStyle(Theme.textSecondary)
                    if health.drivesSiEDA {
                        statusBadge("Connected", on: true)
                    } else {
                        statusBadge("Listening", on: false)
                    }
                }
                Button("Refresh") { Task { await refresh() } }.controlSize(.small)
            }
            if health?.drivesSiEDA != true { setup }
            conversation
        }
        .padding(20)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .task { await refresh() }
    }

    private func statusBadge(_ text: LocalizedStringKey, on: Bool) -> some View {
        Text(text)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(Capsule().fill((on ? Theme.skyBlue : Theme.textMuted).opacity(0.2)))
            .foregroundStyle(on ? Theme.skyBlue : Theme.textSecondary)
    }

    private var setup: some View {
        VStack(alignment: .leading, spacing: 8) {
            if checked && health == nil {
                Text("si-llm is not running on this Mac.").foregroundStyle(Theme.warning)
            }
            Text("1. Start si-llm's server in its folder:").foregroundStyle(Theme.textMuted)
            command("python3 -m py.si.server")
            Text("2. Connect it to this window (turn on the MCP server first):").foregroundStyle(Theme.textMuted)
            if mcp.isListening {
                command(SiLlm.connectCommand(url: mcp.endpointURL, token: mcp.token))
            } else {
                Toggle("Live MCP server", isOn: $mcp.enabled).toggleStyle(.switch)
            }
            Text("3. Ask in a sentence. Each change is one undo step.").foregroundStyle(Theme.textMuted)
        }
        .font(.callout)
    }

    private func command(_ text: String) -> some View {
        HStack(alignment: .top) {
            Text(verbatim: text).font(.system(size: 11, design: .monospaced)).textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
            Spacer()
            Button("Copy") { MCPSettingsView.copy(text) }.controlSize(.small)
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.deepBlue))
    }

    private var conversation: some View {
        VStack(alignment: .leading, spacing: 8) {
            ScrollView {
                VStack(alignment: .leading, spacing: 10) {
                    ForEach(turns) { turn in
                        VStack(alignment: .leading, spacing: 4) {
                            Text(verbatim: turn.prompt).font(.callout.weight(.medium))
                            if !turn.reply.isEmpty { Text(verbatim: turn.reply).font(.callout).textSelection(.enabled) }
                            ForEach(turn.calls, id: \.self) { Text(verbatim: $0).font(.caption.monospaced()).foregroundStyle(Theme.textMuted) }
                            if let proposal = turn.proposal {
                                HStack {
                                    Button("Allow") { decide(turn, proposal, "allow") }.buttonStyle(.borderedProminent)
                                    Button("Always Allow") { decide(turn, proposal, "always") }
                                    Button("Deny", role: .destructive) { decide(turn, proposal, "deny") }
                                }
                                .controlSize(.small).disabled(busy)
                            }
                        }
                        .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
            if let failure { Text(verbatim: failure).font(.caption).foregroundStyle(Theme.warning) }
            HStack {
                TextField("Ask si-llm…", text: $draft).textFieldStyle(.roundedBorder).onSubmit(send)
                Button("Send", action: send).disabled(busy || draft.trimmingCharacters(in: .whitespaces).isEmpty || health == nil)
                if busy { ProgressView().controlSize(.small) }
            }
        }
    }

    private func refresh() async {
        health = await SiLlm.health()
        checked = true
    }

    private func send() {
        let text = draft.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !busy else { return }
        draft = ""
        run { try await SiLlm.ask(text) }
    }

    private func decide(_ turn: SiLlm.Turn, _ proposal: String, _ decision: String) {
        if let i = turns.firstIndex(where: { $0.id == turn.id }) { turns[i].proposal = nil }
        run { try await SiLlm.decide(proposal, decision, prompt: turn.prompt) }
    }

    private func run(_ work: @escaping () async throws -> SiLlm.Turn) {
        busy = true
        failure = nil
        Task { @MainActor in
            do {
                turns.append(try await work())
            } catch {
                failure = error.localizedDescription
            }
            busy = false
            health = await SiLlm.health()
        }
    }
}
