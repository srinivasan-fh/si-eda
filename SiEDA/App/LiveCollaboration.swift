import AppKit
import Foundation
import SwiftUI

/// Live co-editing through a shared project file: iCloud Drive, Dropbox, OneDrive, a network share or a Git working copy.
/// No server: every SiEDA that has the file open with Live Collaboration on
///  • saves shortly after each edit (so the others see it within seconds),
///  • watches the file and three-way merges a teammate's save into the open design (`sieda_merge_projects`, base = the
///    file as this window last read or wrote it) as one undo step — both sides' parts, wires, tracks and settings are
///    kept; a field both changed differently keeps this side's value and is listed in `conflicts`,
///  • announces itself in a presence folder beside the file (".<name>.siedaproj.presence/<user>@<host>.json", refreshed
///    every 15 s, stale after 60 s) and lists the others in the status bar.
@MainActor
final class LiveCollaboration: ObservableObject {
    struct Peer: Codable, Equatable, Identifiable {
        var user: String
        var host: String
        var time: Date
        var id: String { user + "@" + host }
    }

    @Published private(set) var enabled = false
    @Published private(set) var peers: [Peer] = []
    @Published private(set) var conflicts: [String] = []

    private weak var store: DesignStore?
    private var url: URL?
    private var base = ""  // the file's content as this window last read, merged or wrote it
    private var lastModified: Date?
    private var timer: Timer?
    private var ticks = 0
    private var pendingSave: DispatchWorkItem?
    private let me = Peer(user: NSFullUserName().isEmpty ? NSUserName() : NSFullUserName(),
                          host: Host.current().localizedName ?? ProcessInfo.processInfo.hostName, time: Date())

    init(store: DesignStore) { self.store = store }

    /// The open document now matches `json` on disk at `url` (opened or saved); nil when it has no file.
    func documentChanged(url: URL?, json: String) {
        if url != self.url { leave() }
        self.url = url
        base = json
        lastModified = modificationDate()
        if enabled { heartbeat() }
    }

    func setEnabled(_ on: Bool) {
        guard on != enabled else { return }
        enabled = on
        timer?.invalidate()
        timer = nil
        if on {
            ticks = 0
            heartbeat()
            timer = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in
                Task { @MainActor in self?.tick() }
            }
        } else {
            pendingSave?.cancel()
            leave()
        }
    }

    /// An edit happened: while live, save it shortly (edits in quick succession share one save).
    func noteEdit() {
        guard enabled, url != nil else { return }
        pendingSave?.cancel()
        let work = DispatchWorkItem { [weak self] in Task { @MainActor in self?.sync(save: true) } }
        pendingSave = work
        DispatchQueue.main.asyncAfter(deadline: .now() + 1.0, execute: work)
    }

    /// Pulls a teammate's save into the open design, then writes ours when `save` and there is something to write.
    func sync(save: Bool) {
        guard let store, let url else { return }
        if let date = modificationDate(), date != lastModified,
           let theirs = try? String(contentsOf: url, encoding: .utf8), theirs != base {
            merge(theirs, into: store)
        }
        lastModified = modificationDate()
        if save, store.isDirty { store.save() }
    }

    private func tick() {
        ticks += 1
        sync(save: false)
        if ticks % 7 == 0 { heartbeat() }
    }

    private func merge(_ theirs: String, into store: DesignStore) {
        let ours = store.engine.saveJSON()
        guard let result = EDAEngine.mergeProjects(base: base, ours: ours, theirs: theirs) else {
            store.statusMessage = "A teammate's save could not be merged; it was left on disk"
            base = theirs
            return
        }
        base = theirs
        conflicts = result.conflicts
        let changed = store.performExternalEdit("Changes from teammates") { engine in
            (try? engine.load(json: result.merged)) != nil
        }
        guard changed else { return }
        store.statusMessage = result.conflicts.isEmpty
            ? "Merged a teammate's changes"
            : "Merged a teammate's changes — \(result.conflicts.count) conflicting field(s) kept yours"
    }

    // MARK: - Presence

    private var presenceFolder: URL? {
        guard let url else { return nil }
        return url.deletingLastPathComponent().appendingPathComponent("." + url.lastPathComponent + ".presence", isDirectory: true)
    }

    private var presenceFile: URL? {
        let name = me.id.map { $0.isLetter || $0.isNumber || $0 == "@" || $0 == "-" ? $0 : "_" }
        return presenceFolder?.appendingPathComponent(String(name) + ".json")
    }

    private func heartbeat() {
        guard let folder = presenceFolder, let file = presenceFile else { peers = []; return }
        var mine = me
        mine.time = Date()
        let encoder = JSONEncoder()
        encoder.dateEncodingStrategy = .iso8601
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        if let data = try? encoder.encode(mine) { try? data.write(to: file, options: .atomic) }
        let decoder = JSONDecoder()
        decoder.dateDecodingStrategy = .iso8601
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        peers = files.compactMap { f -> Peer? in
            guard f != file, let data = try? Data(contentsOf: f), let p = try? decoder.decode(Peer.self, from: data),
                  Date().timeIntervalSince(p.time) < 60 else { return nil }
            return p
        }
        .sorted { $0.user < $1.user }
    }

    private func leave() {
        if let file = presenceFile { try? FileManager.default.removeItem(at: file) }
        peers = []
    }

    private func modificationDate() -> Date? {
        guard let url else { return nil }
        return (try? FileManager.default.attributesOfItem(atPath: url.path))?[.modificationDate] as? Date
    }
}

/// Status-bar badge while live: who else has the file open, and the conflicts of the last merge.
struct LiveCollaborationIndicator: View {
    @ObservedObject var collaboration: LiveCollaboration

    var body: some View {
        if collaboration.enabled {
            HStack(spacing: 4) {
                Image(systemName: collaboration.peers.isEmpty ? "person.crop.circle" : "person.2.fill")
                    .foregroundStyle(collaboration.conflicts.isEmpty ? Theme.skyBlue : Theme.warning)
                Text(verbatim: collaboration.peers.isEmpty ? "Live" : collaboration.peers.map(\.user).joined(separator: ", "))
                    .foregroundStyle(Theme.textSecondary)
                    .lineLimit(1)
            }
            .help(collaboration.conflicts.isEmpty
                  ? "Live Collaboration: edits are saved and merged with teammates' saves automatically"
                  : "Last merge kept your value for: " + collaboration.conflicts.prefix(5).joined(separator: "; "))
        }
    }
}
