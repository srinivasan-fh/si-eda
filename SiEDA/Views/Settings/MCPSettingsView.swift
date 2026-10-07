import AppKit
import SwiftUI

/// Settings → AI Access (MCP): the live MCP server that lets an AI client on this Mac (Claude Code, Claude Desktop,
/// Cursor, VS Code …) work on the design open in the window (docs/MCP.md, "Live app mode").
struct MCPSettingsView: View {
    @ObservedObject private var mcp = MCPLiveServer.shared
    @State private var showToken = false
    @State private var portDraft = ""

    var body: some View {
        Form {
            serverSection
            tokenSection
            clientsSection
        }
        .formStyle(.grouped)
        .onAppear { portDraft = String(mcp.port) }
    }

    // MARK: - Server

    private var serverSection: some View {
        Section {
            Toggle(isOn: $mcp.enabled) {
                VStack(alignment: .leading, spacing: 2) {
                    Text("Enable live MCP server")
                    Text("An AI client on this Mac works on the design open in this window. Each change it makes is one undo step. Only this Mac can connect, and only with the token below.")
                        .font(.caption)
                        .foregroundStyle(Theme.textMuted)
                }
            }
            HStack {
                Text("Port")
                Spacer()
                TextField("", text: $portDraft)
                    .frame(width: 80)
                    .multilineTextAlignment(.trailing)
                    .onSubmit(applyPort)
                Button("Apply", action: applyPort)
                    .disabled(portDraft == String(mcp.port))
            }
            Toggle("Read-only (the AI can inspect and check, not change)", isOn: $mcp.readOnly)
            folderRow
            statusRow
        } header: {
            Text("Live server")
        }
    }

    private func applyPort() {
        guard let value = Int(portDraft.trimmingCharacters(in: .whitespaces)), (1024...65_535).contains(value) else {
            portDraft = String(mcp.port)
            return
        }
        mcp.port = value
    }

    private var folderRow: some View {
        HStack {
            VStack(alignment: .leading, spacing: 2) {
                Text("Allowed folder")
                Text(verbatim: folderText)
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }
            Spacer()
            Button("Choose…", action: chooseFolder)
            if !mcp.allowedFolder.isEmpty {
                Button("Use Document Folder") { mcp.allowedFolder = "" }
            }
        }
    }

    private var folderText: String {
        if !mcp.allowedFolder.isEmpty { return mcp.allowedFolder }
        let root = mcp.store?.mcpAllowedRoot(chosen: "") ?? ""
        return root.isEmpty ? "—" : root
    }

    private func chooseFolder() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let url = panel.url else { return }
        mcp.allowedFolder = url.path
    }

    private var statusRow: some View {
        HStack(spacing: 8) {
            Image(systemName: mcp.isListening ? "dot.radiowaves.left.and.right" : "circle.dashed")
                .foregroundStyle(mcp.isListening ? Theme.success : Theme.textMuted)
            Text(verbatim: statusText)
                .font(.caption)
                .lineLimit(2)
            Spacer()
        }
    }

    private var statusText: String {
        var parts: [String] = []
        switch mcp.status {
        case .off: parts.append(String(localized: "Off"))
        case .starting: parts.append(String(localized: "Starting…"))
        case .listening: parts.append(mcp.endpointURL)
        case .failed(let message): parts.append(message)
        }
        if mcp.isListening {
            parts.append("\(mcp.connections) ⇄ · \(mcp.sessions.count) MCP")
            if let call = mcp.lastCall {
                let time = DateFormatter.localizedString(from: call.date, dateStyle: .none, timeStyle: .medium)
                parts.append("\(call.tool) @ \(time)")
            }
        }
        return parts.joined(separator: " · ")
    }

    // MARK: - Token

    private var tokenSection: some View {
        Section {
            HStack {
                Text(verbatim: showToken ? token : String(repeating: "•", count: 24))
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
                    .lineLimit(1)
                    .truncationMode(.middle)
                Spacer()
                if showToken {
                    Button("Hide") { showToken = false }
                } else {
                    Button("Show") { showToken = true }
                }
                Button("Copy") { Self.copy(token) }
                Button("Regenerate") { mcp.regenerateToken() }
            }
        } header: {
            Text("Access token")
        } footer: {
            Text("Clients send it as “Authorization: Bearer <token>”. Regenerating it disconnects every client until it is given the new one. Kept in the macOS Keychain.")
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
        }
        .id(mcp.tokenRevision)
    }

    private var token: String { mcp.token }

    // MARK: - Client setup

    private var clientsSection: some View {
        let url = mcp.endpointURL
        let shown = showToken ? token : "<token>"
        return Section {
            snippet("Claude Code", MCPLiveServer.claudeCodeCommand(url: url, token: shown),
                    copy: MCPLiveServer.claudeCodeCommand(url: url, token: token))
            snippet("Claude Desktop", MCPLiveServer.claudeDesktopConfig(url: url, token: shown,
                                                                         bridgePath: MCPLiveServer.bridgeExecutablePath),
                    copy: MCPLiveServer.claudeDesktopConfig(url: url, token: token,
                                                            bridgePath: MCPLiveServer.bridgeExecutablePath))
            snippet("Cursor", MCPLiveServer.cursorConfig(url: url, token: shown),
                    copy: MCPLiveServer.cursorConfig(url: url, token: token))
            snippet("VS Code", MCPLiveServer.vsCodeConfig(url: url, token: shown),
                    copy: MCPLiveServer.vsCodeConfig(url: url, token: token))
        } header: {
            Text("Client setup")
        } footer: {
            Text("Claude Desktop starts the sieda-mcp bridge (built with the core), which forwards to this server. Restart a client after changing its configuration. Full guide: docs/MCP.md.")
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
        }
    }

    private func snippet(_ client: String, _ text: String, copy: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(verbatim: client).font(.callout.weight(.semibold))
                Spacer()
                Button("Copy") { Self.copy(copy) }
                    .controlSize(.small)
            }
            Text(verbatim: text)
                .font(.system(size: 10, design: .monospaced))
                .textSelection(.enabled)
                .fixedSize(horizontal: false, vertical: true)
        }
    }

    static func copy(_ text: String) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(text, forType: .string)
    }
}

/// Status-bar indicator of the live MCP server: shown while it listens, highlighted after an AI client's call.
struct MCPStatusIndicator: View {
    @ObservedObject private var mcp = MCPLiveServer.shared

    var body: some View {
        if mcp.isListening {
            TimelineView(.periodic(from: .now, by: 5)) { context in
                indicator(active: mcp.recentlyActive(now: context.date))
            }
            .fixedSize()
        }
    }

    @ViewBuilder
    private func indicator(active: Bool) -> some View {
        let image = Image(systemName: "dot.radiowaves.left.and.right")
            .foregroundStyle(active ? Theme.skyBlue : Theme.textMuted)
        if active {
            image.help(Text("An AI client used the live MCP server recently"))
        } else {
            image.help(Text("Live MCP server on (Settings → AI Access)"))
        }
    }
}
