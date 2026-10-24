import AppKit
import SwiftUI

/// The system `git` run in a folder (no shell): arguments go straight to the process, output and exit code come back.
enum GitClient: Sendable {
    static let executable = URL(fileURLWithPath: "/usr/bin/git")

    struct Result: Sendable {
        var status: Int32
        var output: String
        var ok: Bool { status == 0 }
    }

    static func run(_ arguments: [String], in folder: URL) -> Result {
        let process = Process()
        process.executableURL = executable
        process.arguments = arguments
        process.currentDirectoryURL = folder
        var env = ProcessInfo.processInfo.environment
        env["GIT_TERMINAL_PROMPT"] = "0"  // never wait on a password prompt nobody can see
        process.environment = env
        let pipe = Pipe()
        process.standardOutput = pipe
        process.standardError = pipe
        do { try process.run() } catch { return Result(status: -1, output: error.localizedDescription) }
        let data = pipe.fileHandleForReading.readDataToEndOfFile()
        process.waitUntilExit()
        return Result(status: process.terminationStatus, output: String(decoding: data, as: UTF8.self))
    }

    /// A branch or tag name git accepts (`git check-ref-format`'s main rules), so a typed name never becomes an option.
    static func validRef(_ name: String) -> Bool {
        !name.isEmpty && !name.hasPrefix("-") && !name.hasPrefix(".") && !name.hasSuffix(".lock") && !name.hasSuffix("/")
            && !name.contains("..") && !name.contains("@{")
            && name.unicodeScalars.allSatisfy { $0.value > 32 && $0.value != 127 && !"~^:?*[\\".unicodeScalars.contains($0) }
    }
}

/// File → Git…: status, history, commit, branch, tag and clone for the folder holding the project.
struct GitView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var status = ""
    @State private var log = ""
    @State private var branches: [String] = []
    @State private var branch = ""
    @State private var message = ""
    @State private var newRef = ""
    @State private var cloneURL = ""
    @State private var result = ""
    @State private var isRepository = false

    private var folder: URL? { store.documentURL?.deletingLastPathComponent() }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Git").font(.headline)
                Spacer()
                if isRepository {
                    Picker("Branch", selection: $branch) { ForEach(branches, id: \.self) { Text($0) } }
                        .frame(width: 220)
                        .onChange(of: branch) { _, new in
                            if let current = currentBranch(), new != current, !new.isEmpty { act(["switch", new]) }
                        }
                }
            }
            if folder == nil {
                Text("Save the project first: Git works on the folder holding it.").foregroundStyle(Theme.textSecondary)
            } else if !isRepository {
                Text("This folder is not a Git repository.").foregroundStyle(Theme.textSecondary)
                Button("Create Repository Here") { act(["init"]) }
            } else {
                Text("Changes").font(.subheadline)
                ScrollView { Text(status.isEmpty ? "No changes" : status).font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }
                    .frame(height: 90)
                HStack {
                    TextField("Commit message", text: $message).textFieldStyle(.roundedBorder)
                    Button("Save and Commit All") {
                        _ = store.save()
                        act(["add", "-A"], then: ["commit", "-m", message])
                        message = ""
                    }
                    .disabled(message.trimmingCharacters(in: .whitespaces).isEmpty)
                }
                HStack {
                    TextField("Branch or tag name", text: $newRef).textFieldStyle(.roundedBorder)
                    Button("New Branch") { act(["switch", "-c", newRef]); newRef = "" }.disabled(!GitClient.validRef(newRef))
                    Button("Tag") { act(["tag", newRef]); newRef = "" }.disabled(!GitClient.validRef(newRef))
                    Button("Pull") { act(["pull", "--ff-only"]) }
                    Button("Push") { act(["push", "--follow-tags"]) }
                }
                Text("History").font(.subheadline)
                ScrollView { Text(log).font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading).textSelection(.enabled) }
            }
            Divider()
            HStack {
                TextField("Clone from URL", text: $cloneURL).textFieldStyle(.roundedBorder)
                Button("Clone…") { clone() }
                    .disabled(cloneURL.isEmpty || cloneURL.hasPrefix("-") || cloneURL.lowercased().hasPrefix("ext::"))
            }
            if !result.isEmpty {
                Text(result).font(.caption).foregroundStyle(Theme.textSecondary).lineLimit(4).textSelection(.enabled)
            }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(minWidth: 640, minHeight: 480)
        .onAppear(perform: refresh)
    }

    private func currentBranch() -> String? {
        guard let folder else { return nil }
        let r = GitClient.run(["branch", "--show-current"], in: folder)
        return r.ok ? r.output.trimmingCharacters(in: .whitespacesAndNewlines) : nil
    }

    private func refresh() {
        guard let folder else { return }
        isRepository = GitClient.run(["rev-parse", "--is-inside-work-tree"], in: folder).ok
        guard isRepository else { return }
        status = GitClient.run(["status", "--short"], in: folder).output
        log = GitClient.run(["log", "--oneline", "--decorate", "-n", "30"], in: folder).output
        branches = GitClient.run(["branch", "--format=%(refname:short)"], in: folder).output
            .split(separator: "\n").map(String.init)
        branch = currentBranch() ?? ""
    }

    /// Runs git off the main thread (a push or pull may wait on the network), then shows its answer.
    private func act(_ arguments: [String], then next: [String]? = nil) {
        guard let folder else { return }
        result = "git \(arguments.first ?? "")…"
        Task.detached {
            var r = GitClient.run(arguments, in: folder)
            if r.ok, let next { r = GitClient.run(next, in: folder) }
            let text = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
            await MainActor.run {
                result = text
                refresh()
            }
        }
    }

    private func clone() {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = String(localized: "Clone Here")
        guard panel.runModal() == .OK, let parent = panel.url else { return }
        let url = cloneURL
        result = "git clone…"
        Task.detached {
            // "--" ends the options: the URL is never read as one.
            let r = GitClient.run(["clone", "--", url], in: parent)
            let name = (URL(string: url)?.deletingPathExtension().lastPathComponent).flatMap { $0.isEmpty ? nil : $0 }
            let repo = parent.appendingPathComponent(name ?? "", isDirectory: true)
            let project = r.ok ? (try? FileManager.default.contentsOfDirectory(at: repo, includingPropertiesForKeys: nil))?
                .first(where: { $0.pathExtension == "siedaproj" }) : nil
            await MainActor.run {
                result = r.output.trimmingCharacters(in: .whitespacesAndNewlines)
                if let project {
                    dismiss()
                    store.open(url: project)
                }
            }
        }
    }
}
