import Foundation

/// Real-time, interactive simulation of the schematic — the "real board": firmware runs, LEDs light with the current
/// through them, switches and push-buttons are clicked while it runs, probes and a scope show the voltages and the
/// serial monitor shows (and sends) what the microcontrollers say.
///
/// Simulated time follows the wall clock times `speed`; when the circuit is too heavy to keep up, the achieved
/// real-time factor drops and is shown. Stepping happens on a background task; the published state is updated on the
/// main actor after every slice.
@MainActor
final class LiveSimulation: ObservableObject {
    /// Simulation speed relative to real time.
    static let speeds: [Double] = [0.01, 0.1, 1, 10]
    /// Analog step (resolution). Finer steps resolve faster signals but cost more.
    static let resolutions: [Double] = [10e-6, 50e-6, 100e-6, 500e-6]

    @Published private(set) var isRunning = false
    @Published private(set) var isPaused = false
    @Published private(set) var state: LiveState?
    @Published private(set) var error: String?
    /// Simulated seconds per wall-clock second actually achieved (≈ speed when the computer keeps up).
    @Published private(set) var realTimeFactor = 0.0
    @Published var speed: Double = 1
    @Published var resolution: Double = 50e-6
    /// Scope: rolling history (simulated seconds) of the nets chosen in `scopeNets`.
    @Published private(set) var scopeTime: [Double] = []
    @Published private(set) var scopeValues: [Int: [Double]] = [:]
    @Published var scopeNets: Set<Int> = []
    @Published var scopeWindow: Double = 2

    private var session: LiveSession?
    private var loop: Task<Void, Never>?
    private var startedRevision = -1
    private var pendingSwitches: [Int: Bool] = [:]
    private var pendingSerial: [(Int, String)] = []
    private var switchState: [Int: Bool] = [:]

    var isActive: Bool { isRunning }

    nonisolated init() {}

    /// Starts (or restarts) the live simulation of the store's current design.
    func start(store: DesignStore) {
        stop()
        do {
            let session = try store.engine.startLive()
            self.session = session
            error = nil
            state = session.state()
            switchState = Dictionary(uniqueKeysWithValues: (state?.switches ?? []).map { ($0.component, $0.closed) })
            scopeTime = []
            scopeValues = [:]
            if scopeNets.isEmpty || !scopeNets.isSubset(of: Set(state?.nets.map(\.index) ?? [])) {
                scopeNets = Set((state?.nets ?? []).prefix(2).map(\.index))
            }
            startedRevision = store.revision
            isRunning = true
            isPaused = false
            runLoop(store: store)
            store.statusMessage = "Live simulation running"
        } catch {
            self.error = error.localizedDescription
            store.statusMessage = "Live simulation could not start"
        }
    }

    func clearError() { error = nil }

    func pause() { isPaused = true }
    func resume() { isPaused = false }

    func stop() {
        loop?.cancel()
        loop = nil
        session = nil
        isRunning = false
        isPaused = false
        realTimeFactor = 0
    }

    /// Clicks a switch: a push-button closes while held (`pressed`), a toggle switch flips on press.
    func press(_ componentId: Int, pressed: Bool) {
        guard isRunning, let sw = state?.switches.first(where: { $0.component == componentId }) else { return }
        let closed: Bool
        if sw.momentary {
            closed = pressed
        } else {
            guard pressed else { return }
            closed = !(switchState[componentId] ?? sw.closed)
        }
        switchState[componentId] = closed
        pendingSwitches[componentId] = closed
    }

    func isClosed(_ componentId: Int) -> Bool? {
        switchState[componentId] ?? state?.switches.first { $0.component == componentId }?.closed
    }

    func sendSerial(_ text: String, to componentId: Int) {
        guard isRunning else { return }
        pendingSerial.append((componentId, text))
    }

    func voltage(net: Int) -> Double? { state?.voltage(net: net) }

    func led(_ componentId: Int) -> LiveState.Led? { state?.leds.first { $0.component == componentId } }

    // MARK: - Run loop

    private func runLoop(store: DesignStore) {
        loop = Task { [weak self] in
            var last = Date()
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 33_000_000)
                guard let self, !Task.isCancelled, let session = self.session else { return }
                // An edited design is a different circuit: stop instead of simulating a stale snapshot.
                if store.revision != self.startedRevision {
                    self.stop()
                    store.statusMessage = "Live simulation stopped: the design changed — run it again to include the edits"
                    return
                }
                let now = Date()
                let wall = min(now.timeIntervalSince(last), 0.2)
                last = now
                if self.isPaused { continue }
                let switches = self.pendingSwitches
                let serial = self.pendingSerial
                self.pendingSwitches = [:]
                self.pendingSerial = []
                let duration = max(self.resolution, wall * self.speed)
                let step = min(self.resolution, duration)
                let started = Date()
                let result: Result<LiveState?, Error> = await Task.detached(priority: .userInitiated) {
                    for (id, closed) in switches { session.setSwitch(id, closed: closed) }
                    for (id, text) in serial { session.sendSerial(id, text: text) }
                    do {
                        try session.run(duration: duration, step: step, tracePoints: 120)
                        return .success(session.state())
                    } catch {
                        return .failure(error)
                    }
                }.value
                guard !Task.isCancelled, self.session === session else { return }
                switch result {
                case .success(let state):
                    guard let state else { continue }
                    self.state = state
                    self.appendScope(state.trace)
                    let compute = Date().timeIntervalSince(started)
                    self.realTimeFactor = duration / max(wall, compute, 1e-3)
                case .failure(let error):
                    self.error = error.localizedDescription
                    self.stop()
                    return
                }
            }
        }
    }

    private func appendScope(_ trace: LiveState.Trace) {
        guard !trace.time.isEmpty else { return }
        scopeTime.append(contentsOf: trace.time)
        for series in trace.nets {
            guard let index = series.index, scopeNets.contains(index) else { continue }
            var values = scopeValues[index] ?? Array(repeating: series.values.first ?? 0, count: scopeTime.count - trace.time.count)
            values.append(contentsOf: series.values)
            scopeValues[index] = values
        }
        // Keep the last `scopeWindow` seconds (and at most ~4000 points).
        let cutoff = (scopeTime.last ?? 0) - scopeWindow
        var drop = scopeTime.firstIndex { $0 >= cutoff } ?? 0
        drop = max(drop, scopeTime.count - 4000)
        if drop > 0 {
            scopeTime.removeFirst(drop)
            for key in scopeValues.keys { scopeValues[key]?.removeFirst(min(drop, scopeValues[key]?.count ?? 0)) }
        }
        for key in scopeValues.keys where !scopeNets.contains(key) { scopeValues[key] = nil }
    }
}
