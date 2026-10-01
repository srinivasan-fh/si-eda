import SwiftUI

/// Combined ERC / DRC results (Altium "Messages" panel style).
struct RuleCheckView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var filter: ViolationSeverity? = nil

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Image(systemName: "checkmark.seal").foregroundStyle(Theme.blue)
                Text("Design Checks").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Button { store.runERC() } label: { Label("Run ERC", systemImage: "bolt.badge.checkmark") }
                Button { store.runDRC() } label: { Label("Run DRC", systemImage: "square.grid.3x3.square") }
                Spacer()
                Picker("Show", selection: $filter) {
                    Text("All").tag(ViolationSeverity?.none)
                    Text("Errors").tag(ViolationSeverity?.some(.error))
                    Text("Warnings").tag(ViolationSeverity?.some(.warning))
                    Text("Info").tag(ViolationSeverity?.some(.info))
                }
                .pickerStyle(.segmented)
                .frame(width: 300)
            }
            .buttonStyle(.borderless)

            HStack(alignment: .top, spacing: 12) {
                checkColumn(title: "Electrical Rule Check", icon: "bolt.shield", results: store.ercResults,
                            target: .schematic)
                checkColumn(title: "Design Rule Check", icon: "square.grid.3x3.square", results: store.drcResults,
                            target: .pcb)
            }
            .padding(12)
        }
        .background(Theme.navy)
        .onAppear {
            if store.ercResults.isEmpty { store.runERC() }
            if store.drcResults.isEmpty, !store.snapshot.pads.isEmpty { store.runDRC() }
        }
    }

    private func checkColumn(title: String, icon: String, results: [RuleViolation], target: Workspace) -> some View {
        let visible = results.filter { filter == nil || $0.severity == filter }.sorted { $0.severity > $1.severity }
        let errors = results.filter { $0.severity == .error }.count
        let warnings = results.filter { $0.severity == .warning }.count
        return VStack(alignment: .leading, spacing: 10) {
            HStack {
                Label(title, systemImage: icon).font(.headline).foregroundStyle(Theme.textPrimary)
                Spacer()
                Badge(text: "\(errors) errors · \(warnings) warnings",
                      systemImage: errors == 0 ? "checkmark.circle" : "xmark.octagon")
            }
            if visible.isEmpty {
                Text(results.isEmpty ? "Not run yet." : "Nothing to show for this filter.")
                    .foregroundStyle(Theme.textMuted)
                    .padding(.vertical, 20)
                Spacer()
            } else {
                ScrollView {
                    LazyVStack(alignment: .leading, spacing: 6) {
                        ForEach(visible) { v in
                            Button {
                                if !v.components.isEmpty {
                                    store.selection = Set(v.components)
                                    store.selectedWire = nil
                                    store.workspace = target
                                }
                            } label: {
                                HStack(alignment: .top, spacing: 10) {
                                    Image(systemName: Theme.severityIcon(v.severity))
                                        .foregroundStyle(Theme.severityColor(v.severity))
                                    VStack(alignment: .leading, spacing: 2) {
                                        Text(v.message).foregroundStyle(Theme.textPrimary).multilineTextAlignment(.leading)
                                        Text(v.code).font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                                    }
                                    Spacer()
                                    if !v.components.isEmpty {
                                        Image(systemName: "scope").foregroundStyle(Theme.lightBlue)
                                    }
                                }
                                .padding(8)
                                .background(RoundedRectangle(cornerRadius: 8).fill(Theme.deepBlue.opacity(0.7)))
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                            .help(v.components.isEmpty ? "" : "Cross-probe to the affected parts")
                        }
                    }
                }
            }
        }
        .padding(12)
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .bluePanel()
    }
}
