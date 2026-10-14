import AppKit
import Foundation

/// Check-out lock beside an open project file ("<name>.siedaproj.lock": who has it open, on which Mac, since when), so
/// a teammate opening the same file on a shared drive hears who is editing it. Live Collaboration does not lock.
enum ProjectLock {
    struct Holder: Codable, Equatable {
        var user: String
        var host: String
        var pid: Int32
        var since: Date
    }

    static func url(for project: URL) -> URL { project.appendingPathExtension("lock") }

    static var me: Holder {
        Holder(user: NSUserName(), host: ProcessInfo.processInfo.hostName, pid: ProcessInfo.processInfo.processIdentifier, since: Date())
    }

    /// Someone else's live lock on the file; nil when there is none, it is ours, or it is stale (its process on this
    /// Mac is gone, or it is older than 12 hours).
    static func holder(of project: URL) -> Holder? {
        guard let data = try? Data(contentsOf: url(for: project)), let h = try? JSONDecoder().decode(Holder.self, from: data) else { return nil }
        let me = self.me
        if h.host == me.host && (h.pid == me.pid || kill(h.pid, 0) != 0) { return nil }
        return Date().timeIntervalSince(h.since) > 12 * 3600 ? nil : h
    }

    static func take(_ project: URL) {
        try? JSONEncoder().encode(me).write(to: url(for: project))
    }

    /// Removes the lock if it is this app's.
    static func release(_ project: URL?) {
        guard let project, let data = try? Data(contentsOf: url(for: project)),
              let h = try? JSONDecoder().decode(Holder.self, from: data), h.host == me.host, h.pid == me.pid else { return }
        try? FileManager.default.removeItem(at: url(for: project))
    }
}

/// Event hooks: executables the user puts in Application Support/SiEDA/Hooks, named after an event — after-open,
/// after-save, after-verify, after-export — run in the background with SIEDA_EVENT, SIEDA_PROJECT (the project file)
/// and SIEDA_OUTPUT (the fabrication folder) set, their output appended to hooks.log there and stopped after a
/// minute. Only that folder is searched: a project never brings scripts of its own.
enum ProjectHooks {
    static var folder: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("SiEDA/Hooks", isDirectory: true)
    }

    @discardableResult
    static func run(_ event: String, project: URL?, output: URL? = nil, in folder: URL = folder) -> Process? {
        let script = folder.appendingPathComponent(event)
        guard FileManager.default.isExecutableFile(atPath: script.path) else { return nil }
        let process = Process()
        process.executableURL = script
        process.currentDirectoryURL = project?.deletingLastPathComponent() ?? folder
        var env = ProcessInfo.processInfo.environment
        env["SIEDA_EVENT"] = event
        env["SIEDA_PROJECT"] = project?.path ?? ""
        env["SIEDA_OUTPUT"] = output?.path ?? ""
        process.environment = env
        let log = folder.appendingPathComponent("hooks.log")
        if !FileManager.default.fileExists(atPath: log.path) { FileManager.default.createFile(atPath: log.path, contents: nil) }
        if let handle = try? FileHandle(forWritingTo: log) {
            handle.seekToEndOfFile()
            handle.write(Data("[\(Date())] \(event) \(project?.lastPathComponent ?? "")\n".utf8))
            process.standardOutput = handle
            process.standardError = handle
            process.terminationHandler = { _ in try? handle.close() }
        }
        do { try process.run() } catch { return nil }
        DispatchQueue.global().asyncAfter(deadline: .now() + 60) { if process.isRunning { process.terminate() } }
        return process
    }

    static func openFolder() {
        try? FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        NSWorkspace.shared.open(folder)
    }
}
