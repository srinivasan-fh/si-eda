import SwiftUI

/// Harness types of the design (Altium's harness definitions): named bundles of signals that harness labels carry
/// across sheets in one go. Define a type by its name and members; place a harness connector of it from the sheet
/// bar's Harnesses menu.
struct HarnessTypesView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var entries = ""
    @State private var selected: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("HARNESS TYPES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Text("A harness type is a named bundle of signals (USB: DP, DN, VBUS, GND). A harness label of the type carries every member — \"USB1.DP\", \"USB1.DN\" … — through a port and its sheet entry, or as a global label; its entries join the members on the sheet.")
                .font(.caption).foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
            List(selection: $selected) {
                ForEach(store.snapshot.harnessTypes) { type in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: type.name).font(.body.weight(.semibold))
                        Text(verbatim: type.entries.joined(separator: ", ")).font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                    }
                    .tag(type.name)
                }
            }
            .frame(minHeight: 140)
            .onChange(of: selected) { _, name in
                if let name, let type = store.snapshot.harnessTypes.first(where: { $0.name == name }) {
                    self.name = type.name
                    entries = type.entries.joined(separator: ", ")
                }
            }
            LabeledContent("Name") {
                TextField("USB", text: $name).textFieldStyle(.roundedBorder)
            }
            LabeledContent("Members") {
                TextField("DP, DN, VBUS, GND", text: $entries).textFieldStyle(.roundedBorder)
            }
            HStack {
                Button("Save Type") {
                    if store.setHarnessType(name, entries: entries) { selected = name.trimmingCharacters(in: .whitespaces) }
                }
                .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty || entries.trimmingCharacters(in: .whitespaces).isEmpty)
                Button("Delete Type", role: .destructive) {
                    if let selected { store.removeHarnessType(selected) }
                    selected = nil
                }
                .disabled(selected == nil)
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 460, height: 440)
    }
}
