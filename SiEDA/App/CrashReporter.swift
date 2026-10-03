import AppKit
import Darwin

/// Crash reporting for SiEDA, kept entirely on this Mac (nothing is uploaded).
///
/// * **Fatal signals** (Swift runtime traps, segmentation faults, aborts) and **uncaught Objective-C exceptions**
///   are written to `fatal.raw` with a symbolicated backtrace. The signal handler only uses async-signal-safe calls
///   on buffers prepared at start-up, runs on its own stack (so stack overflows are caught too), then re-raises the
///   signal so macOS still produces its own report.
/// * **Breadcrumbs**: every edit, open, save and workspace switch appends one line to a session log, rotated at
///   `maxLogBytes` so the log never grows past twice that size.
/// * **Next launch**: a session that did not end cleanly (crash, hang + force quit, power loss) is turned into a
///   readable report in `Reports/`, combining the crash, the recent activity and the system details. At most
///   `maxReports` reports are kept.
final class CrashReporter {
    /// The app's reporter (nil in unit tests, which create their own on a temporary directory).
    private(set) static var shared: CrashReporter?

    let directory: URL
    let maxReports: Int
    let maxLogBytes: Int
    /// Report of the previous session, when it ended abnormally.
    private(set) var lastSessionReport: URL?

    private let lock = NSLock()
    private var logFD: Int32 = -1
    private var logBytes = 0

    var reportsDirectory: URL { directory.appendingPathComponent("Reports", isDirectory: true) }
    var recoveryDirectory: URL { directory.appendingPathComponent("Recovery", isDirectory: true) }
    private var markerURL: URL { directory.appendingPathComponent("running.marker") }
    private var fatalURL: URL { directory.appendingPathComponent("fatal.raw") }
    private var logURL: URL { directory.appendingPathComponent("session.log") }
    private var oldLogURL: URL { directory.appendingPathComponent("session.1.log") }

    init(directory: URL, maxReports: Int = 20, maxLogBytes: Int = 128 * 1024) {
        self.directory = directory
        self.maxReports = maxReports
        self.maxLogBytes = maxLogBytes
    }

    /// `~/Library/Application Support/SiEDA/Diagnostics`.
    static var defaultDirectory: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask).first
            ?? URL(fileURLWithPath: NSTemporaryDirectory())
        return base.appendingPathComponent("SiEDA/Diagnostics", isDirectory: true)
    }

    /// True while XCTest drives the app: no handlers, dialogs or watchdogs then.
    static var isRunningTests: Bool {
        ProcessInfo.processInfo.environment["XCTestConfigurationFilePath"] != nil || NSClassFromString("XCTestCase") != nil
    }

    /// Starts the app-wide reporter: collects the previous session, starts a new one and installs the handlers.
    @discardableResult
    static func installShared(directory: URL = defaultDirectory) -> CrashReporter {
        if let shared { return shared }
        let reporter = CrashReporter(directory: directory)
        reporter.startSession()
        reporter.installHandlers()
        shared = reporter
        return reporter
    }

    /// Records one line of recent activity on the app's reporter (no-op without one).
    static func note(_ message: String) { shared?.breadcrumb(message) }

    // MARK: - Session lifecycle

    /// Turns the previous session's leftovers into a report (if it ended abnormally), prunes old reports and marks
    /// this session as running.
    func startSession() {
        let fm = FileManager.default
        try? fm.createDirectory(at: reportsDirectory, withIntermediateDirectories: true)
        lastSessionReport = collectPreviousSession()
        prune()
        try? Self.systemSummary().write(to: markerURL, atomically: true, encoding: .utf8)
        lock.lock()
        logFD = open(logURL.path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0o644)
        logBytes = 0
        lock.unlock()
        breadcrumb("Session started")
    }

    /// A clean shutdown: the next launch reports nothing.
    func endSession() {
        breadcrumb("Session ended normally")
        lock.lock()
        if logFD >= 0 { close(logFD) }
        logFD = -1
        lock.unlock()
        let fm = FileManager.default
        for url in [markerURL, logURL, oldLogURL, fatalURL] { try? fm.removeItem(at: url) }
    }

    /// Appends a timestamped line to the session log; rotates it past `maxLogBytes`. O(1) per call.
    func breadcrumb(_ message: String) {
        let line = "\(Self.timestamp(Date())) \(message.replacingOccurrences(of: "\n", with: " "))\n"
        lock.lock()
        defer { lock.unlock() }
        guard logFD >= 0 else { return }
        if logBytes + line.utf8.count > maxLogBytes {
            close(logFD)
            try? FileManager.default.removeItem(at: oldLogURL)
            try? FileManager.default.moveItem(at: logURL, to: oldLogURL)
            logFD = open(logURL.path, O_WRONLY | O_CREAT | O_TRUNC | O_APPEND, 0o644)
            logBytes = 0
            guard logFD >= 0 else { return }
        }
        let written = line.withCString { write(logFD, $0, strlen($0)) }
        if written > 0 { logBytes += written }
    }

    /// Records a fatal error caught in Swift (an uncaught exception); the process is expected to end right after.
    func recordFatal(kind: String, reason: String, stack: [String]) {
        let text = "\(kind)\nReason: \(reason)\n\nBacktrace:\n" + stack.joined(separator: "\n") + "\n"
        try? text.write(to: fatalURL, atomically: false, encoding: .utf8)
        breadcrumb("FATAL: \(kind) — \(reason)")
    }

    /// Builds a report when the previous session did not end cleanly; clears its leftovers either way.
    func collectPreviousSession() -> URL? {
        let fm = FileManager.default
        defer { for url in [markerURL, logURL, oldLogURL, fatalURL] { try? fm.removeItem(at: url) } }
        let crashed = fm.fileExists(atPath: markerURL.path) || fm.fileExists(atPath: fatalURL.path)
        guard crashed else { return nil }
        let previous = (try? String(contentsOf: markerURL, encoding: .utf8)) ?? "(unknown)"
        let fatal = (try? String(contentsOf: fatalURL, encoding: .utf8))
        let activity = [oldLogURL, logURL].compactMap { try? String(contentsOf: $0, encoding: .utf8) }.joined()
        let recent = activity.split(separator: "\n", omittingEmptySubsequences: true).suffix(200).joined(separator: "\n")
        var report = "SiEDA crash report\n==================\n\n"
        report += "Detected: \(Self.timestamp(Date()))\n\n"
        report += fatal.map { "What happened\n-------------\n\($0)\n" }
            ?? "What happened\n-------------\nSiEDA did not shut down normally (force quit after a hang, power loss, or a crash the reporter could not capture).\n\n"
        report += "Recent activity (oldest first)\n------------------------------\n\(recent.isEmpty ? "(none)" : recent)\n\n"
        report += "Previous session\n----------------\n\(previous)\n"
        let url = reportsDirectory.appendingPathComponent("SiEDA-\(Self.fileStamp(Date()))-\(UUID().uuidString.prefix(6)).crash.txt")
        do {
            try fm.createDirectory(at: reportsDirectory, withIntermediateDirectories: true)
            try report.write(to: url, atomically: true, encoding: .utf8)
            return url
        } catch {
            NSLog("SiEDA: could not write crash report: %@", error.localizedDescription)
            return nil
        }
    }

    /// Saved reports, newest first.
    var reports: [URL] {
        let urls = (try? FileManager.default.contentsOfDirectory(at: reportsDirectory, includingPropertiesForKeys: nil)) ?? []
        return urls.filter { $0.lastPathComponent.hasSuffix(".crash.txt") }
            .sorted { $0.lastPathComponent > $1.lastPathComponent }
    }

    /// Keeps the newest `maxReports` reports.
    func prune() {
        for url in reports.dropFirst(maxReports) { try? FileManager.default.removeItem(at: url) }
    }

    // MARK: - Handlers

    func installHandlers() {
        installFatalHandlers(path: fatalURL.path, header: "SiEDA \(Self.appVersion) — fatal signal\n")
        NSSetUncaughtExceptionHandler { exception in
            CrashReporter.shared?.recordFatal(kind: "Uncaught exception \(exception.name.rawValue)",
                                              reason: exception.reason ?? "", stack: exception.callStackSymbols)
        }
    }

    // MARK: - Helpers

    static var appVersion: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "?"
        let build = info?["CFBundleVersion"] as? String ?? "?"
        return "\(version) (\(build))"
    }

    static func systemSummary() -> String {
        let p = ProcessInfo.processInfo
        return """
        App: SiEDA \(appVersion)
        Launched: \(timestamp(Date()))
        macOS: \(p.operatingSystemVersionString)
        CPU cores: \(p.activeProcessorCount)
        Memory: \(p.physicalMemory / 1_048_576) MB
        """
    }

    /// Shared (thread-safe) so a breadcrumb costs no formatter set-up.
    private static let isoFormatter: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withInternetDateTime, .withFractionalSeconds]
        return f
    }()

    private static func timestamp(_ date: Date) -> String { isoFormatter.string(from: date) }

    private static func fileStamp(_ date: Date) -> String {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "yyyy-MM-dd-HHmmss"
        return f.string(from: date)
    }
}

// MARK: - Async-signal-safe fatal signal handler

/// Everything the handler touches is allocated before it is installed: it never allocates, locks or calls Swift.
private var fatalPath: UnsafeMutablePointer<CChar>?
private var fatalHeader: UnsafeMutablePointer<CChar>?
private let fatalFrames = UnsafeMutablePointer<UnsafeMutableRawPointer?>.allocate(capacity: 128)
private let signalNames = UnsafeMutablePointer<UnsafeMutablePointer<CChar>?>.allocate(capacity: 32)
private let fatalSignals: [(Int32, String)] = [
    (SIGSEGV, "SIGSEGV — invalid memory access"), (SIGBUS, "SIGBUS — misaligned or unmapped memory"),
    (SIGILL, "SIGILL — Swift runtime trap (force unwrap of nil, index out of range, fatalError…)"),
    (SIGTRAP, "SIGTRAP — Swift runtime trap (force unwrap of nil, index out of range, fatalError…)"),
    (SIGABRT, "SIGABRT — abort (failed assertion or uncaught exception)"), (SIGFPE, "SIGFPE — arithmetic error"),
]

private func installFatalHandlers(path: String, header: String) {
    fatalPath = strdup(path)
    fatalHeader = strdup(header)
    signalNames.initialize(repeating: nil, count: 32)
    for (sig, name) in fatalSignals where sig < 32 { signalNames[Int(sig)] = strdup("Signal: \(name)\n\nBacktrace:\n") }
    _ = backtrace(fatalFrames, 1)  // loads the unwinder now, not inside the handler

    // A separate stack so a stack overflow can still be reported.
    let altSize = 128 * 1024
    var alt = stack_t(ss_sp: UnsafeMutableRawPointer.allocate(byteCount: altSize, alignment: 16), ss_size: altSize, ss_flags: 0)
    sigaltstack(&alt, nil)

    for (sig, _) in fatalSignals {
        var action = sigaction()
        action.__sigaction_u = __sigaction_u(__sa_handler: siedaFatalSignal)
        action.sa_mask = 0
        action.sa_flags = SA_ONSTACK
        sigaction(sig, &action, nil)
    }
}

private func siedaFatalSignal(_ sig: Int32) {
    if let path = fatalPath {
        let fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0o644)
        if fd >= 0 {
            if let header = fatalHeader { _ = write(fd, header, strlen(header)) }
            if sig > 0, sig < 32, let name = signalNames[Int(sig)] { _ = write(fd, name, strlen(name)) }
            let count = backtrace(fatalFrames, 128)
            backtrace_symbols_fd(fatalFrames, count, fd)
            close(fd)
        }
    }
    // Let macOS finish the crash as usual (and write its own report).
    var action = sigaction()
    action.__sigaction_u = __sigaction_u(__sa_handler: SIG_DFL)
    sigaction(sig, &action, nil)
    raise(sig)
}

// MARK: - Crash recovery

/// Autosaves the open design while it has unsaved changes, so a crash never loses more than a few seconds of work.
/// Writes happen off the main thread; the file is removed when the design is saved, discarded or closed cleanly.
final class CrashRecovery {
    let directory: URL
    private let queue = DispatchQueue(label: "SiEDA.recovery", qos: .utility)
    private var designURL: URL { directory.appendingPathComponent("autosave.siedaproj") }
    private var originURL: URL { directory.appendingPathComponent("autosave.origin") }

    init(directory: URL) {
        self.directory = directory
    }

    struct Pending {
        let json: String
        /// Where the design was saved before, if anywhere.
        let documentURL: URL?
        let savedAt: Date?
    }

    func save(json: String, documentURL: URL?) {
        let designURL = designURL, originURL = originURL, directory = directory
        queue.async {
            do {
                try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
                try json.write(to: designURL, atomically: true, encoding: .utf8)
                try (documentURL?.path ?? "").write(to: originURL, atomically: true, encoding: .utf8)
            } catch {
                NSLog("SiEDA: recovery autosave failed: %@", error.localizedDescription)
            }
        }
    }

    func clear() {
        let designURL = designURL, originURL = originURL
        queue.async {
            try? FileManager.default.removeItem(at: designURL)
            try? FileManager.default.removeItem(at: originURL)
        }
    }

    /// Waits for pending writes (tests, and before reading at launch).
    func flush() { queue.sync {} }

    func pending() -> Pending? {
        flush()
        guard let json = try? String(contentsOf: designURL, encoding: .utf8), !json.isEmpty else { return nil }
        let origin = (try? String(contentsOf: originURL, encoding: .utf8)) ?? ""
        let date = (try? FileManager.default.attributesOfItem(atPath: designURL.path))?[.modificationDate] as? Date
        return Pending(json: json, documentURL: origin.isEmpty ? nil : URL(fileURLWithPath: origin), savedAt: date)
    }
}

// MARK: - Hang and memory monitors

/// Logs a breadcrumb when the main thread stops responding for `threshold` seconds, and when it recovers, so a
/// report after a force quit shows where the app was stuck.
final class MainThreadWatchdog {
    private let threshold: TimeInterval
    private let lock = NSLock()
    private var lastPong = Date()
    private var pingPending = false
    private var hangReported = false
    private var timer: DispatchSourceTimer?
    private let log: @Sendable (String) -> Void

    /// `log` is called from a background queue (the hang) and the main queue (the recovery).
    init(threshold: TimeInterval = 4, log: @escaping @Sendable (String) -> Void = { CrashReporter.note($0) }) {
        self.threshold = threshold
        self.log = log
    }

    func start() {
        lock.lock()
        lastPong = Date()
        lock.unlock()
        let timer = DispatchSource.makeTimerSource(queue: .global(qos: .utility))
        let interval = min(1, threshold / 4)
        timer.schedule(deadline: .now() + interval, repeating: interval)
        timer.setEventHandler { [weak self] in self?.tick() }
        timer.resume()
        self.timer = timer
    }

    func stop() {
        timer?.cancel()
        timer = nil
    }

    private func tick() {
        lock.lock()
        let stalled = Date().timeIntervalSince(lastPong)
        if stalled > threshold, !hangReported {
            hangReported = true
            lock.unlock()
            log(String(format: "HANG: main thread unresponsive for %.0f s", stalled))
            return
        }
        let shouldPing = !pingPending
        pingPending = true
        lock.unlock()
        guard shouldPing else { return }
        DispatchQueue.main.async { [weak self] in
            guard let self else { return }
            self.lock.lock()
            let was = self.hangReported ? Date().timeIntervalSince(self.lastPong) : nil
            self.lastPong = Date()
            self.pingPending = false
            self.hangReported = false
            self.lock.unlock()
            if let was { self.log(String(format: "Main thread responsive again after %.0f s", was)) }
        }
    }
}

extension Notification.Name {
    /// Posted on the main queue when macOS reports memory pressure; `userInfo["critical"]` is a Bool.
    static let siedaMemoryPressure = Notification.Name("SiEDA.memoryPressure")
}

/// Turns macOS memory-pressure events into a notification the app's caches respond to, and a breadcrumb.
final class MemoryPressureMonitor {
    private var source: DispatchSourceMemoryPressure?

    func start() {
        let source = DispatchSource.makeMemoryPressureSource(eventMask: [.warning, .critical], queue: .main)
        source.setEventHandler { [weak source] in
            let critical = source?.data.contains(.critical) ?? false
            CrashReporter.note("Memory pressure: \(critical ? "critical" : "warning")")
            NotificationCenter.default.post(name: .siedaMemoryPressure, object: nil, userInfo: ["critical": critical])
        }
        source.resume()
        self.source = source
    }

    func stop() {
        source?.cancel()
        source = nil
    }
}
