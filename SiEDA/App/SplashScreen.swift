import AppKit
import SwiftUI

/// The launch splash: preloads what the first screens need (the core's design rules, standard parts, industry
/// profiles and reference designs) while showing the brand for `minimumDuration`, then hands over to the main window.
@MainActor
final class SplashModel: ObservableObject {
    struct Step {
        let title: String
        let work: () -> Void
    }

    /// How long the splash stays up (unless skipped once everything is loaded).
    static let defaultDuration: TimeInterval = 5

    let steps: [Step]
    let minimumDuration: TimeInterval
    @Published private(set) var completedSteps = 0
    @Published private(set) var isFinished = false
    private(set) var startDate = Date()
    private var skipRequested = false

    init(steps: [Step] = SplashModel.standardSteps, minimumDuration: TimeInterval = SplashModel.defaultDuration) {
        self.steps = steps
        self.minimumDuration = minimumDuration
    }

    /// Loads the static data the workspaces read on first use, so the first clicks don't stall.
    nonisolated static var standardSteps: [Step] {
        [
            Step(title: "Starting the design engine") { _ = StandardLibrary.rulePresets.count },
            Step(title: "Loading the component library") { _ = StandardLibrary.parts.count },
            Step(title: "Loading industry profiles") { _ = StandardLibrary.industries.count },
            Step(title: "Loading reference designs") { _ = OfflineProvider.templates.count },
        ]
    }

    var isLoaded: Bool { completedSteps >= steps.count }

    /// Where the progress bar is: never ahead of the clock (the splash lasts `minimum`) nor of the work done.
    nonisolated static func progress(elapsed: TimeInterval, minimum: TimeInterval, completed: Int, total: Int) -> Double {
        let time = minimum > 0 ? min(max(elapsed / minimum, 0), 1) : 1
        let work = total > 0 ? min(Double(completed) / Double(total), 1) : 1
        return min(time, work)
    }

    /// The step whose part of the bar the progress is in; `total` (shown as "Ready") once the bar is full.
    nonisolated static func statusIndex(progress: Double, total: Int) -> Int {
        guard total > 0 else { return 0 }
        return min(max(Int(progress * Double(total)), 0), total)
    }

    func progress(at date: Date) -> Double {
        Self.progress(elapsed: date.timeIntervalSince(startDate), minimum: minimumDuration,
                      completed: completedSteps, total: steps.count)
    }

    func status(at date: Date) -> String {
        let index = Self.statusIndex(progress: progress(at: date), total: steps.count)
        return index < steps.count ? steps[index].title + "…" : "Ready"
    }

    /// Runs every step on the main thread (the core is not shared across threads), letting the splash draw between
    /// steps, then waits out the rest of `minimumDuration`.
    func run() async {
        startDate = Date()
        completedSteps = 0
        isFinished = false
        skipRequested = false
        for step in steps {
            try? await Task.sleep(nanoseconds: 30_000_000)
            step.work()
            completedSteps += 1
        }
        while !skipRequested && Date().timeIntervalSince(startDate) < minimumDuration {
            try? await Task.sleep(nanoseconds: 40_000_000)
        }
        isFinished = true
    }

    /// A click or Esc ends the splash early, once everything is loaded.
    func skip() {
        if isLoaded { skipRequested = true }
    }
}

/// Shows the splash window at launch and keeps the main window hidden until the splash is done.
@MainActor
final class SplashController {
    /// The splash being shown; main windows that appear meanwhile wait for it (see `SplashWindowGate`).
    static var current: SplashController?

    /// On unless turned off in Settings → Appearance (or with `-showSplashScreen NO` on the command line).
    static var isEnabled: Bool { UserDefaults.standard.object(forKey: "showSplashScreen") as? Bool ?? true }

    let model: SplashModel
    private(set) var window: NSWindow?
    private let held = NSHashTable<NSWindow>.weakObjects()
    private var observers: [NSObjectProtocol] = []
    private var sweep: Timer?
    private var onFinish: (() -> Void)?
    /// From `activate()` until the splash is done: main windows are kept out of sight.
    private(set) var isActive = false

    /// `model` defaults to the standard preload (made here: default arguments aren't main-actor isolated).
    init(model: SplashModel? = nil) {
        self.model = model ?? SplashModel()
    }

    var isShowing: Bool { window != nil }

    /// Starts holding main windows back. Called before launch finishes, because SwiftUI creates the main window
    /// before `applicationDidFinishLaunching` (when the splash itself appears). Until the splash is done, any
    /// main window that shows up, or that SwiftUI brings forward again, is hidden on the next sweep (every 50 ms).
    func activate() {
        guard !isActive else { return }
        isActive = true
        Self.current = self
        let center = NotificationCenter.default
        for name in [NSWindow.didBecomeKeyNotification, NSWindow.didBecomeMainNotification] {
            observers.append(center.addObserver(forName: name, object: nil, queue: .main) { [weak self] _ in
                MainActor.assumeIsolated { self?.holdVisibleWindows() }
            })
        }
        // A cheap sweep (only while the splash is up) catches windows ordered in without becoming key.
        let timer = Timer(timeInterval: 0.05, repeats: true) { [weak self] _ in
            MainActor.assumeIsolated { self?.holdVisibleWindows() }
        }
        RunLoop.main.add(timer, forMode: .common)
        sweep = timer
        holdVisibleWindows()
    }

    /// Shows the splash, preloads, then reveals the held windows and calls `onFinish`.
    func show(onFinish: @escaping () -> Void) {
        self.onFinish = onFinish
        activate()
        let window = SplashWindow(contentRect: NSRect(origin: .zero, size: SplashView.size), styleMask: [.borderless],
                                  backing: .buffered, defer: false)
        window.isOpaque = false
        window.backgroundColor = .clear
        window.hasShadow = true
        window.isMovableByWindowBackground = true
        window.isReleasedWhenClosed = false
        window.collectionBehavior = [.moveToActiveSpace]
        window.title = "SiEDA"
        window.setAccessibilityLabel("SiEDA is starting")
        let host = NSHostingView(rootView: SplashView(model: model))
        host.wantsLayer = true
        host.layer?.backgroundColor = .clear
        window.contentView = host
        window.onCancel = { [weak self] in self?.model.skip() }
        window.center()
        window.alphaValue = 0
        self.window = window
        holdVisibleWindows()
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.3
            window.animator().alphaValue = 1
        }
        // The shadow follows the rounded card once it has been drawn.
        DispatchQueue.main.async { MainActor.assumeIsolated { window.invalidateShadow() } }
        Task { @MainActor in
            await model.run()
            finish()
        }
    }

    /// Hides every main window that is on screen (other than the splash).
    private func holdVisibleWindows() {
        guard isActive else { return }
        for other in NSApp.windows where other !== window && other.isVisible && other.canBecomeMain {
            hold(other)
        }
    }

    /// Keeps a main window out of sight until the splash is done: transparent at once (so it never draws on
    /// screen), and ordered out so the splash stays in front and keeps the keyboard.
    func hold(_ other: NSWindow) {
        guard isActive, other !== window else { return }
        held.add(other)
        other.alphaValue = 0
        guard other.isVisible else { return }
        DispatchQueue.main.async { [weak self, weak other] in
            MainActor.assumeIsolated {
                guard let self, let other, self.isActive, other.isVisible else { return }
                other.orderOut(nil)
                if let splash = self.window, !splash.isKeyWindow { splash.makeKeyAndOrderFront(nil) }
            }
        }
    }

    private func finish() {
        guard let splash = window else { return }
        // Stop holding first, so revealing the main windows isn't undone.
        isActive = false
        for observer in observers { NotificationCenter.default.removeObserver(observer) }
        observers.removeAll()
        sweep?.invalidate()
        sweep = nil
        if Self.current === self { Self.current = nil }
        let windows = held.allObjects
        held.removeAllObjects()
        for main in windows {
            main.alphaValue = 0
            main.makeKeyAndOrderFront(nil)
        }
        NSApp.activate(ignoringOtherApps: true)
        NSAnimationContext.runAnimationGroup { context in
            context.duration = 0.4
            splash.animator().alphaValue = 0
            for main in windows { main.animator().alphaValue = 1 }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 0.45) { [weak self] in
            MainActor.assumeIsolated {
                splash.orderOut(nil)
                for main in windows { main.alphaValue = 1 }
                self?.window = nil
                let done = self?.onFinish
                self?.onFinish = nil
                done?()
            }
        }
    }
}

/// Borderless, but able to take Esc (which skips the splash once loading is done).
private final class SplashWindow: NSWindow {
    var onCancel: (() -> Void)?
    override var canBecomeKey: Bool { true }
    override func cancelOperation(_ sender: Any?) { onCancel?() }
}

/// Invisible view in the main window: while the splash is up, the window waits behind it.
struct SplashWindowGate: NSViewRepresentable {
    final class GateView: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let window else { return }
            MainActor.assumeIsolated { SplashController.current?.hold(window) }
        }
    }

    func makeNSView(context: Context) -> NSView { GateView() }
    func updateNSView(_ nsView: NSView, context: Context) {}
}

// MARK: - Design

/// The splash card: the wordmark on the left, a quietly animated circuit on the right, the loading status and a
/// hairline progress bar along the bottom.
struct SplashView: View {
    static let size = CGSize(width: 680, height: 420)
    private static let corner: CGFloat = 18

    @ObservedObject var model: SplashModel
    @Environment(\.accessibilityReduceMotion) private var reduceMotion

    var body: some View {
        TimelineView(.animation(minimumInterval: 1 / 60, paused: model.isFinished)) { context in
            let progress = model.progress(at: context.date)
            let elapsed = context.date.timeIntervalSince(model.startDate)
            ZStack {
                SplashBackground()
                SplashCircuit(time: reduceMotion ? 10 : elapsed, animated: !reduceMotion)
                    .mask {
                        LinearGradient(stops: [.init(color: .clear, location: 0.30),
                                               .init(color: .black, location: 0.62)],
                                       startPoint: .leading, endPoint: .trailing)
                    }
                    .mask {
                        LinearGradient(stops: [.init(color: .black, location: 0.78),
                                               .init(color: .clear, location: 0.86)],
                                       startPoint: .top, endPoint: .bottom)
                    }
                content(progress: progress, status: model.status(at: context.date))
            }
        }
        .frame(width: Self.size.width, height: Self.size.height)
        .clipShape(RoundedRectangle(cornerRadius: Self.corner, style: .continuous))
        .overlay(
            RoundedRectangle(cornerRadius: Self.corner, style: .continuous)
                .strokeBorder(Color.white.opacity(0.09), lineWidth: 1)
        )
        .contentShape(Rectangle())
        .onTapGesture { model.skip() }
        .environment(\.colorScheme, .dark)
        .accessibilityElement(children: .combine)
        .accessibilityLabel("SiEDA, Electronic Design Automation. \(model.isLoaded ? "Ready" : "Loading")")
    }

    private func content(progress: Double, status: String) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 12) {
                Image(nsImage: NSApp.applicationIconImage)
                    .resizable()
                    .interpolation(.high)
                    .frame(width: 44, height: 44)
                Text("SIEDA")
                    .font(.system(size: 11, weight: .semibold))
                    .tracking(3)
                    .foregroundStyle(Theme.textMuted)
            }
            Spacer()
            Text("SiEDA")
                .font(.system(size: 54, weight: .semibold))
                .tracking(-0.5)
                .foregroundStyle(LinearGradient(colors: [.white, Theme.iceBlue], startPoint: .top, endPoint: .bottom))
            Text("ELECTRONIC DESIGN AUTOMATION")
                .font(.system(size: 11.5, weight: .medium))
                .tracking(2.6)
                .foregroundStyle(Theme.lightBlue.opacity(0.85))
                .padding(.top, 2)
            Text("Schematic · Simulation · PCB · 3D · Fabrication")
                .font(.system(size: 12))
                .foregroundStyle(Theme.textMuted)
                .padding(.top, 14)
            Spacer()
            HStack(alignment: .firstTextBaseline) {
                Text(status)
                    .font(.system(size: 11))
                    .foregroundStyle(Theme.textMuted)
                    .contentTransition(.opacity)
                    .animation(.easeInOut(duration: 0.25), value: status)
                Spacer()
                Text(Self.versionLine)
                    .font(.system(size: 11).monospacedDigit())
                    .foregroundStyle(Theme.textMuted.opacity(0.8))
            }
            .padding(.bottom, 10)
            SplashProgressBar(progress: progress)
        }
        .padding(.horizontal, 40)
        .padding(.top, 34)
        .padding(.bottom, 30)
    }

    static var versionLine: String {
        let info = Bundle.main.infoDictionary
        let version = info?["CFBundleShortVersionString"] as? String ?? "1.0"
        let build = info?["CFBundleVersion"] as? String
        let app = build.map { "Version \(version) (\($0))" } ?? "Version \(version)"
        return app + " · Core \(EDAEngine.coreVersion)"
    }
}

private struct SplashProgressBar: View {
    var progress: Double

    var body: some View {
        GeometryReader { geo in
            ZStack(alignment: .leading) {
                Capsule().fill(Color.white.opacity(0.08))
                Capsule()
                    .fill(LinearGradient(colors: [Theme.blue, Theme.skyBlue], startPoint: .leading, endPoint: .trailing))
                    .frame(width: max(2, geo.size.width * progress))
                    .shadow(color: Theme.skyBlue.opacity(0.55), radius: 4)
            }
        }
        .frame(height: 2)
        .accessibilityHidden(true)
    }
}

/// Deep navy with a soft glow behind the circuit and a faint dot grid, like an empty schematic sheet.
private struct SplashBackground: View {
    var body: some View {
        ZStack {
            LinearGradient(colors: [Color(red: 0.045, green: 0.075, blue: 0.15), Color(red: 0.012, green: 0.025, blue: 0.06)],
                           startPoint: .topLeading, endPoint: .bottomTrailing)
            RadialGradient(colors: [Theme.blue.opacity(0.22), .clear], center: UnitPoint(x: 0.74, y: 0.42),
                           startRadius: 0, endRadius: 300)
            Canvas { context, size in
                let step: CGFloat = 18
                var dots = Path()
                var y = step / 2
                while y < size.height {
                    var x = step / 2
                    while x < size.width {
                        dots.addEllipse(in: CGRect(x: x - 0.6, y: y - 0.6, width: 1.2, height: 1.2))
                        x += step
                    }
                    y += step
                }
                context.fill(dots, with: .color(Color.white.opacity(0.05)))
            }
        }
    }
}

/// A QFN-style chip with traces fanning out to vias, drawn in over the first second and a half, with a few signal
/// pulses running along the traces afterwards.
private struct SplashCircuit: View {
    var time: TimeInterval
    var animated: Bool

    struct Trace {
        var path: Path
        var end: CGPoint
        var pulse: Bool
        var phase: Double
    }

    static let chipCenter = CGPoint(x: 505, y: 185)
    static let chipSize: CGFloat = 96
    static let pinsPerSide = 6
    static let traces: [Trace] = makeTraces()

    /// Deterministic layout: each pin leaves its side, bends 45° and runs on to a via.
    static func makeTraces() -> [Trace] {
        var out: [Trace] = []
        let half = chipSize / 2
        let pitch = chipSize / CGFloat(pinsPerSide + 1)
        let directions: [(CGVector, CGVector)] = [(CGVector(dx: -1, dy: 0), CGVector(dx: 0, dy: 1)),
                                                  (CGVector(dx: 1, dy: 0), CGVector(dx: 0, dy: 1)),
                                                  (CGVector(dx: 0, dy: -1), CGVector(dx: 1, dy: 0)),
                                                  (CGVector(dx: 0, dy: 1), CGVector(dx: 1, dy: 0))]
        for (side, (out1, along)) in directions.enumerated() {
            for k in 0..<pinsPerSide {
                let offset = -half + pitch * CGFloat(k + 1)
                let seed = Double((side * 7 + k * 13) % 17) / 17
                let start = CGPoint(x: chipCenter.x + out1.dx * (half + 8) + along.dx * offset,
                                    y: chipCenter.y + out1.dy * (half + 8) + along.dy * offset)
                let first: CGFloat = 14 + CGFloat(seed) * 22
                let bend: CGFloat = (k < pinsPerSide / 2 ? -1 : 1) * (10 + CGFloat(k % 3) * 9)
                // Top and bottom traces stay short, clear of the status line along the bottom.
                let last: CGFloat = side >= 2 ? 10 + CGFloat((side + k) % 3) * 10 : 30 + CGFloat((side + k) % 4) * 26
                let p1 = CGPoint(x: start.x + out1.dx * first, y: start.y + out1.dy * first)
                let p2 = CGPoint(x: p1.x + out1.dx * abs(bend) + along.dx * bend,
                                 y: p1.y + out1.dy * abs(bend) + along.dy * bend)
                let p3 = CGPoint(x: p2.x + out1.dx * last, y: p2.y + out1.dy * last)
                var path = Path()
                path.move(to: start)
                path.addLine(to: p1)
                path.addLine(to: p2)
                path.addLine(to: p3)
                out.append(Trace(path: path, end: p3, pulse: (side + k) % 3 == 0, phase: seed))
            }
        }
        return out
    }

    var body: some View {
        Canvas { context, _ in
            let reveal = animated ? min(max(time / 1.6, 0), 1) : 1
            let eased = 1 - pow(1 - reveal, 3)
            let traceColor = Theme.lightBlue.opacity(0.26)

            for (index, trace) in Self.traces.enumerated() {
                // Traces draw in one after another, ending together.
                let delay = Double(index % Self.pinsPerSide) * 0.04
                let local = min(max((eased - delay) / (1 - delay), 0), 1)
                guard local > 0 else { continue }
                context.stroke(trace.path.trimmedPath(from: 0, to: local), with: .color(traceColor),
                               style: StrokeStyle(lineWidth: 1.3, lineCap: .round, lineJoin: .round))
                if local >= 1 {
                    let via = CGRect(x: trace.end.x - 3.5, y: trace.end.y - 3.5, width: 7, height: 7)
                    context.stroke(Path(ellipseIn: via), with: .color(Theme.pad.opacity(0.55)), lineWidth: 1.4)
                }
            }

            // Signal pulses once everything is drawn.
            if animated && reveal >= 1 {
                for trace in Self.traces where trace.pulse {
                    let t = (time * 0.42 + trace.phase).truncatingRemainder(dividingBy: 1.6)
                    guard t < 1 else { continue }
                    let head = t, tail = max(0, t - 0.18)
                    let glow = trace.path.trimmedPath(from: tail, to: head)
                    context.stroke(glow, with: .color(Theme.skyBlue.opacity(0.18)),
                                   style: StrokeStyle(lineWidth: 5, lineCap: .round, lineJoin: .round))
                    context.stroke(glow, with: .color(Theme.skyBlue.opacity(0.9)),
                                   style: StrokeStyle(lineWidth: 1.4, lineCap: .round, lineJoin: .round))
                }
            }

            // The chip: body, pins and the pin-1 mark.
            let half = Self.chipSize / 2
            let body = CGRect(x: Self.chipCenter.x - half, y: Self.chipCenter.y - half,
                              width: Self.chipSize, height: Self.chipSize)
            let chip = Path(roundedRect: body, cornerRadius: 7)
            context.fill(chip, with: .linearGradient(Gradient(colors: [Color(red: 0.09, green: 0.15, blue: 0.29),
                                                                        Color(red: 0.04, green: 0.07, blue: 0.15)]),
                                                     startPoint: CGPoint(x: body.minX, y: body.minY),
                                                     endPoint: CGPoint(x: body.maxX, y: body.maxY)))
            context.stroke(chip, with: .color(Theme.lightBlue.opacity(0.45)), lineWidth: 1)
            let pitch = Self.chipSize / CGFloat(Self.pinsPerSide + 1)
            var pins = Path()
            for k in 0..<Self.pinsPerSide {
                let o = -half + pitch * CGFloat(k + 1)
                pins.addRect(CGRect(x: body.minX - 8, y: Self.chipCenter.y + o - 1.6, width: 8, height: 3.2))
                pins.addRect(CGRect(x: body.maxX, y: Self.chipCenter.y + o - 1.6, width: 8, height: 3.2))
                pins.addRect(CGRect(x: Self.chipCenter.x + o - 1.6, y: body.minY - 8, width: 3.2, height: 8))
                pins.addRect(CGRect(x: Self.chipCenter.x + o - 1.6, y: body.maxY, width: 3.2, height: 8))
            }
            context.fill(pins, with: .color(Theme.pad.opacity(0.75)))
            context.fill(Path(ellipseIn: CGRect(x: body.minX + 9, y: body.minY + 9, width: 5, height: 5)),
                         with: .color(Theme.lightBlue.opacity(0.5)))
            context.draw(Text("Si").font(.system(size: 22, weight: .light)).foregroundColor(Theme.iceBlue.opacity(0.7)),
                         at: Self.chipCenter)
        }
        .accessibilityHidden(true)
    }
}
