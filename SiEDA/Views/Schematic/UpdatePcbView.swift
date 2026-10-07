import SwiftUI

/// A change an update from the schematic makes to the board (core `PcbEcoChange`).
struct PcbEcoChangeInfo: Decodable, Equatable, Identifiable {
    var section: String  // "component", "net", "zone", "rule"
    var action: String   // "add", "remove", "change"
    var object: String
    var detail: String
    var key: String
    var applicable: Bool
    var note: String
    var id: String { key }
}

/// Update PCB (Altium's Engineering Change Order, schematic → board): every component, net, pour and net rule the
/// update would add, remove or change since the last update, grouped by section, each with a check box. Validate
/// reads the changes again; Execute carries out the chosen ones (one undo step) and lists what was done.
struct UpdatePcbView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var changes: [PcbEcoChangeInfo] = []
    @State private var chosen: Set<String> = []
    @State private var report: [String] = []

    private static let sections = ["component", "net", "zone", "rule"]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("UPDATE PCB").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Text("Changes from the schematic to the board since the last update.")
                .font(.callout).foregroundStyle(Theme.textMuted)
            if changes.isEmpty && report.isEmpty {
                Label("The board is up to date with the schematic.", systemImage: "checkmark.seal.fill")
                    .foregroundStyle(Theme.liveOn)
            }
            List {
                ForEach(Self.sections, id: \.self) { section in
                    let rows = changes.filter { $0.section == section }
                    if !rows.isEmpty {
                        Section(LocalizedStringKey(Self.sectionTitle(section))) {
                            ForEach(rows) { row($0) }
                        }
                    }
                }
                if !report.isEmpty {
                    Section("Executed") {
                        ForEach(Array(report.enumerated()), id: \.offset) { _, line in
                            Label(line, systemImage: "checkmark.circle").font(.caption)
                        }
                    }
                }
            }
            .frame(minHeight: 260)
            HStack {
                Text("\(chosen.count) of \(changes.count) changes selected").font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
                Button("Validate") { reload() }
                    .help("Read the changes again from the schematic")
                Button("Close") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Execute Changes") {
                    report = store.updatePCB(keys: Array(chosen))
                    // New parts to place by hand: the PCB editor takes over (Preferences → PCB).
                    if store.placementSession != nil {
                        dismiss()
                    } else {
                        reload()
                    }
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .disabled(chosen.isEmpty)
            }
        }
        .padding(16)
        .frame(width: 680, height: 480)
        .onAppear { reload() }
    }

    private func row(_ c: PcbEcoChangeInfo) -> some View {
        HStack(spacing: 8) {
            Toggle("", isOn: Binding(get: { chosen.contains(c.key) },
                                     set: { if $0 { chosen.insert(c.key) } else { chosen.remove(c.key) } }))
                .toggleStyle(.checkbox)
                .labelsHidden()
                .disabled(!c.applicable)
            Image(systemName: c.action == "add" ? "plus.circle" : c.action == "remove" ? "minus.circle" : "pencil.circle")
                .foregroundStyle(c.action == "remove" ? Theme.warning : Theme.skyBlue)
            Text(verbatim: c.object).font(.caption.monospaced().weight(.semibold)).frame(width: 110, alignment: .leading)
            Text(verbatim: c.detail).font(.caption).foregroundStyle(Theme.textSecondary).lineLimit(2)
            Spacer()
        }
    }

    private func reload() {
        changes = store.engine.pcbEcoPreview()
        chosen = Set(changes.filter(\.applicable).map(\.key))
    }

    static func sectionTitle(_ section: String) -> String {
        switch section {
        case "component": return "Components"
        case "net": return "Nets"
        case "zone": return "Copper Pours"
        default: return "Net Rules"
        }
    }
}
