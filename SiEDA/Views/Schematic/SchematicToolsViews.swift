import SwiftUI

/// Messages panel (Altium's "Messages" after a compile): every ERC finding of every sheet, grouped by sheet, with the
/// sheet named; click a message to show it on its sheet, selected and zoomed.
struct SchematicMessagesPanel: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var isShown: Bool
    @State private var showInfo = false

    var body: some View {
        let shown = store.ercResults.filter { showInfo || $0.severity != .info }
        let errors = store.ercResults.filter { $0.severity == .error }.count
        let warnings = store.ercResults.filter { $0.severity == .warning }.count
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Text("MESSAGES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Text("\(errors) errors · \(warnings) warnings").font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
                Toggle("Info", isOn: $showInfo).toggleStyle(.checkbox).font(.caption)
                Button { store.runERC() } label: { Label("Compile", systemImage: "hammer") }
                    .help("Run the electrical rule check on every sheet")
                Button { isShown = false } label: { Image(systemName: "xmark") }
                    .buttonStyle(.borderless)
                    .accessibilityLabel("Close messages")
            }
            .controlSize(.small)
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            Divider()
            if shown.isEmpty {
                Text(LocalizedStringKey(store.ercResults.isEmpty ? "Compile to check every sheet." : "No errors or warnings."))
                    .font(.caption).foregroundStyle(Theme.textMuted)
                    .padding(10)
                Spacer(minLength: 0)
            } else {
                List(shown.numbered()) { v in
                    Button { open(v) } label: {
                        HStack(alignment: .firstTextBaseline, spacing: 8) {
                            Image(systemName: v.severity == .error ? "xmark.octagon.fill"
                                  : v.severity == .warning ? "exclamationmark.triangle.fill" : "info.circle")
                                .foregroundStyle(v.severity == .error ? Theme.error : v.severity == .warning ? Theme.warning : Theme.textMuted)
                            Text(verbatim: sheetName(v)).font(.caption.monospaced()).foregroundStyle(Theme.skyBlue)
                                .frame(width: 110, alignment: .leading).lineLimit(1)
                            Text(verbatim: v.message).font(.caption).foregroundStyle(Theme.textPrimary).lineLimit(2)
                            Spacer()
                            Text(verbatim: v.code).font(.caption2.monospaced()).foregroundStyle(Theme.textMuted)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .contextMenu {
                        // Error reporting: this rule at another severity, everywhere in the project.
                        let current = store.snapshot.ercSeverities[v.code] ?? "default"
                        ForEach(Self.levels, id: \.self) { level in
                            Button {
                                store.setErcSeverity(v.code, level: level)
                            } label: {
                                if current == level {
                                    Label(LocalizedStringKey(Self.levelTitle(level)), systemImage: "checkmark")
                                } else {
                                    Text(LocalizedStringKey(Self.levelTitle(level)))
                                }
                            }
                        }
                    }
                }
                .listStyle(.plain)
            }
        }
        .frame(height: 170)
        .background(Theme.deepBlue.opacity(0.92))
        .onAppear { if store.ercResults.isEmpty { store.runERC() } }
    }

    static let levels = ["default", "error", "warning", "info", "off"]

    static func levelTitle(_ level: String) -> String {
        switch level {
        case "error": return "Report as Error"
        case "warning": return "Report as Warning"
        case "info": return "Report as Info"
        case "off": return "Do Not Report"
        default: return "Rule's Own Severity"
        }
    }

    private func sheetName(_ v: RuleViolation) -> String {
        guard let sheet = v.sheet, let info = store.snapshot.sheet(sheet) else { return "—" }
        return info.name
    }

    private func open(_ v: RuleViolation) {
        if let sheet = v.sheet, let first = v.components.first {
            store.crossProbe(component: first, sheet: sheet)
        } else if let first = v.components.first {
            store.reveal(component: first)
        }
    }
}

/// Back-annotation review (Altium's ECO dialog): the changes the board proposes to the schematic — designator
/// renames, pin swaps, gate swaps — each with a check box; changes that cannot be applied say why.
struct EcoReviewView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    let title: String
    @State var changes: [EcoChangeInfo]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("ENGINEERING CHANGE ORDER").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Text(verbatim: title).font(.headline).foregroundStyle(Theme.textPrimary)
            if changes.isEmpty {
                Text("No changes: the schematic already matches the board.").font(.callout).foregroundStyle(Theme.textMuted)
            }
            List {
                ForEach(changes.indices, id: \.self) { i in
                    let c = changes[i]
                    HStack(spacing: 8) {
                        Toggle("", isOn: Binding(get: { changes[i].applicable }, set: { changes[i].applicable = $0 }))
                            .toggleStyle(.checkbox)
                            .labelsHidden()
                            .disabled(!c.note.isEmpty)
                        Text(LocalizedStringKey(Self.kindTitle(c.kind))).font(.caption.weight(.semibold)).frame(width: 80, alignment: .leading)
                        Text(verbatim: c.from).font(.caption.monospaced())
                        Image(systemName: "arrow.right").font(.caption2).foregroundStyle(Theme.textMuted)
                        Text(verbatim: c.to).font(.caption.monospaced())
                        Spacer()
                        if !c.note.isEmpty {
                            Text(verbatim: c.note).font(.caption).foregroundStyle(Theme.warning).lineLimit(2)
                        }
                    }
                }
            }
            .frame(minHeight: 220)
            HStack {
                let count = changes.filter(\.applicable).count
                Text("\(count) of \(changes.count) changes selected").font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Apply Changes") {
                    store.applyEco(changes)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .disabled(count == 0)
            }
        }
        .padding(16)
        .frame(width: 620, height: 420)
    }

    static func kindTitle(_ kind: String) -> String {
        switch kind {
        case "rename": return "Rename"
        case "pinSwap": return "Pin swap"
        case "gateSwap": return "Gate swap"
        default: return "—"
        }
    }
}
