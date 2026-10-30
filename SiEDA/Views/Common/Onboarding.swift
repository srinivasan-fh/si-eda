import AppKit
import SwiftUI

/// First-run tour (also Help → Welcome Tour): five pages from an empty project to the fabrication package.
struct WelcomeTourView: View {
    static let shownKey = "onboarding.tourShown"
    @Environment(\.dismiss) private var dismiss
    @State private var page = 0

    private let pages: [(icon: String, title: LocalizedStringKey, text: LocalizedStringKey)] = [
        ("sparkles", "Welcome to SiEDA",
         "From idea to fabrication files in one window: schematic, simulation, PCB layout, checks and manufacturing. Start from an example design or a blank project."),
        ("point.3.connected.trianglepath.dotted", "Draw the schematic",
         "Pick a part on the left (or press P), click to place it, then click two pins to wire them. ⌘K finds any command, part or net."),
        ("waveform.path.ecg", "Simulate and check",
         "Run DC, transient, AC, noise and Monte Carlo analyses; the status bar shows the time taken and Stop. The rule checks list every problem and show it on the canvas."),
        ("cpu", "Lay out the board",
         "Update PCB brings the parts over; Auto Place and Auto Route do the rest. Route by hand with push and shove where it matters."),
        ("shippingbox", "Order the board",
         "Export Fabrication Package (⇧⌘E) verifies the design and writes Gerbers, drills, IPC-2581, ODB++, the BOM, placement files and an optional production panel."),
    ]

    var body: some View {
        let p = pages[page]
        VStack(spacing: 14) {
            Image(systemName: p.icon).font(.system(size: 42)).foregroundStyle(Theme.skyBlue).frame(height: 52)
            Text(p.title).font(.title2.bold()).foregroundStyle(Theme.textPrimary)
            Text(p.text).multilineTextAlignment(.center).foregroundStyle(Theme.textSecondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 6) {
                ForEach(pages.indices, id: \.self) { i in
                    Circle().fill(i == page ? Theme.skyBlue : Theme.textMuted.opacity(0.5)).frame(width: 7, height: 7)
                }
            }
            HStack {
                Button("Skip", action: close)
                Spacer()
                if page > 0 { Button("Back") { page -= 1 } }
                Button(page == pages.count - 1 ? LocalizedStringKey("Get Started") : LocalizedStringKey("Next")) {
                    if page == pages.count - 1 { close() } else { page += 1 }
                }
                .keyboardShortcut(.defaultAction)
            }
        }
        .padding(24)
        .frame(width: 480)
        .background(Theme.deepBlue)
    }

    private func close() {
        UserDefaults.standard.set(true, forKey: Self.shownKey)
        dismiss()
    }
}

/// Help → Keyboard Shortcuts (⌘/): every menu command with a key, read from the menus (always current), and the
/// canvas keys.
struct KeyboardShortcutsView: View {
    @Environment(\.dismiss) private var dismiss

    private static let canvas: [(keys: String, action: LocalizedStringKey)] = [
        ("← ↑ → ↓", "Pan (⇧ for half a screen)"), ("+  −", "Zoom In"), ("Home  0", "Zoom to Fit"),
        ("⇧Z", "Zoom to Selection"), ("Z", "Zoom to Area"), ("N", "Show Navigator"), ("F11", "Focus mode"),
        ("V  H  W", "Select, pan and wire tools (schematic)"), ("P", "Place a part"),
        ("R  X  Y", "Rotate and mirror (the selection, or the part under the pointer)"),
        ("⌘D  ⌥-drag  /", "Duplicate, move without the wires, turn a wire's corner the other way"),
        ("⌥⌘ ← → ↑ ↓", "Align the selected parts (schematic)"), ("T  ⌘↑", "Note tool, up to the parent sheet (schematic)"),
    ]

    var body: some View {
        let entries = CommandPalette.menuEntries(NSApp.mainMenu).filter { !$0.shortcut.isEmpty }
        let menus = entries.reduce(into: [String]()) { if !$0.contains($1.menu) { $0.append($1.menu) } }
        VStack(spacing: 0) {
            HStack {
                Text("Keyboard Shortcuts").font(.title3.bold()).foregroundStyle(Theme.textPrimary)
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            .padding(14)
            List {
                Section("Canvas") {
                    ForEach(Self.canvas.indices, id: \.self) { i in
                        row(Text(verbatim: Self.canvas[i].keys), Text(Self.canvas[i].action))
                    }
                }
                ForEach(menus, id: \.self) { menu in
                    Section(menu) {
                        ForEach(entries.filter { $0.menu == menu }) { row(Text(verbatim: $0.shortcut), Text(verbatim: $0.title)) }
                    }
                }
            }
        }
        .frame(width: 520, height: 560)
        .background(Theme.deepBlue)
    }

    private func row(_ keys: Text, _ action: Text) -> some View {
        HStack {
            action.foregroundStyle(Theme.textPrimary)
            Spacer()
            keys.font(.body.monospaced()).foregroundStyle(Theme.skyBlue)
        }
    }
}

/// Help → Lessons…: step-by-step exercises (core `project_lessons`): the built-in ones and the team's own from the shared
/// library folder's Lessons/ (Settings → Library). "Check My Work" checks the step against the design; "Show Me" does
/// it (one undo step each, like any tool the app runs). The lesson state lives in this window's own core server.
struct LessonsView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var lessons: [[String: Any]] = []
    @State private var problems: [String] = []
    @State private var step: [String: Any]?
    @State private var error = ""
    private static let bridge = MCPStoreBridge()

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Lessons").font(.title2.bold())
            List(lessons.indices, id: \.self) { i in
                let l = lessons[i]
                HStack {
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: l["title"] as? String ?? "").bold()
                        Text(verbatim: "\(l["summary"] as? String ?? "") · \(l["source"] as? String ?? "") · \(l["steps"] as? Int ?? 0)")
                            .font(.caption).foregroundStyle(Theme.textSecondary)
                    }
                    Spacer()
                    Button("Start") { call(["action": "start", "id": l["id"] as? String ?? ""]) }
                }
            }
            .frame(minHeight: 180)
            if !problems.isEmpty { Text(verbatim: problems.joined(separator: "; ")).font(.caption).foregroundStyle(Theme.warning) }
            if let step { stepCard(step) }
            if !error.isEmpty { Text(verbatim: error).font(.caption).foregroundStyle(Theme.error) }
            HStack {
                Button("Check My Work") { call(["action": "check"]) }.disabled(step == nil)
                Button("Show Me") { call(["action": "show"]) }.disabled(step == nil)
                Button("Stop Lesson") { call(["action": "stop"]); step = nil }.disabled(step == nil)
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(width: 620, height: 560)
        .onAppear { list() }
    }

    @ViewBuilder private func stepCard(_ s: [String: Any]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            if s["done"] as? Bool == true {
                Text(verbatim: s["title"] as? String ?? "").bold()
                Text("Lesson complete: every step checked.")
            } else {
                Text(verbatim: "\(s["title"] as? String ?? "") — \(s["step"] as? Int ?? 0) / \(s["steps"] as? Int ?? 0): \(s["stepTitle"] as? String ?? "")").bold()
                Text(verbatim: s["text"] as? String ?? "")
                if let hint = s["hint"] as? String { Text(verbatim: hint).font(.caption).foregroundStyle(Theme.textSecondary) }
                if let notYet = s["notYet"] as? [String] {
                    Text(verbatim: notYet.joined(separator: "\n")).font(.caption).foregroundStyle(Theme.warning)
                } else if s["passed"] as? Bool == true {
                    Text("Well done: next step.").font(.caption).foregroundStyle(Theme.success)
                }
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.darkBlue))
    }

    /// The team folder (Settings → Library → shared folder) is the root, so its Lessons/ is listed too.
    private var folderArgs: [String: Any] { UserFootprintLibrary.sharedFolder == nil ? [:] : ["folder": "."] }

    private func list() {
        guard let r = run(["action": "list"]) else { return }
        lessons = r["lessons"] as? [[String: Any]] ?? []
        problems = r["problems"] as? [String] ?? []
        step = r["current"] as? [String: Any]
    }

    private func call(_ args: [String: Any]) {
        if let r = run(args), args["action"] as? String != "stop" { step = r }
    }

    private func run(_ args: [String: Any]) -> [String: Any]? {
        Self.bridge.configure(readOnly: false, allowedRoot: UserFootprintLibrary.sharedFolder?.path ?? "", actor: "user")
        let request: [String: Any] = ["jsonrpc": "2.0", "id": 1, "method": "tools/call",
                                      "params": ["name": "project_lessons", "arguments": args.merging(folderArgs) { a, _ in a }]]
        guard let data = try? JSONSerialization.data(withJSONObject: request),
              let outcome = Self.bridge.handle(String(decoding: data, as: UTF8.self), store: store).first,
              let reply = try? JSONSerialization.jsonObject(with: Data(outcome.response.utf8)) as? [String: Any],
              let result = reply["result"] as? [String: Any] else { error = String(localized: "The core did not answer"); return nil }
        let text = ((result["content"] as? [[String: Any]])?.first?["text"] as? String) ?? ""
        if result["isError"] as? Bool == true { error = text; return nil }
        error = ""
        return (try? JSONSerialization.jsonObject(with: Data(text.utf8))) as? [String: Any]
    }
}
