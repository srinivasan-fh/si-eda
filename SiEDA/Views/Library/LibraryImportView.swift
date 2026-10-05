import SwiftUI

/// Library import review (Component Library → Import Library…): the parts the core read from KiCad / Eagle library
/// files, each with its symbol, footprint and notes. Importable parts start selected; parts that cannot be imported
/// show why. The selected parts go to `onAdd`.
struct LibraryImportView: View {
    let result: LibraryImportResult
    let onAdd: ([CustomPartSpec]) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var selected: Set<Int>

    init(result: LibraryImportResult, onAdd: @escaping ([CustomPartSpec]) -> Void) {
        self.result = result
        self.onAdd = onAdd
        _selected = State(initialValue: Set(result.parts.indices.filter { result.parts[$0].ok }))
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("Import Library")
                .font(.headline)
                .foregroundStyle(Theme.skyBlue)
                .padding(12)
            Divider()
            List {
                ForEach(Array(result.files.enumerated()), id: \.offset) { _, file in
                    if !file.error.isEmpty {
                        Label {
                            Text(verbatim: "\(file.name): \(file.error)").font(.caption)
                        } icon: {
                            Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                        }
                    }
                }
                ForEach(Array(result.parts.enumerated()), id: \.offset) { index, part in
                    row(index: index, part: part)
                }
            }
            .listStyle(.inset)
            .scrollContentBackground(.hidden)
            Divider()
            HStack {
                Text(verbatim: "\(selected.count) / \(result.parts.count)")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Add Selected Parts") {
                    onAdd(selected.sorted().map { result.parts[$0].spec })
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .disabled(selected.isEmpty)
            }
            .padding(12)
        }
        .frame(minWidth: 620, minHeight: 460)
        .background(Theme.navy)
    }

    private func row(index: Int, part: LibraryImportResult.Part) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Toggle(isOn: Binding(get: { selected.contains(index) },
                                 set: { if $0 { selected.insert(index) } else { selected.remove(index) } })) {
                EmptyView()
            }
            .labelsHidden()
            .disabled(!part.ok)
            VStack(alignment: .leading, spacing: 3) {
                Text(verbatim: part.name).foregroundStyle(Theme.textPrimary)
                Text(verbatim: [part.footprint.isEmpty ? part.spec.package.type : part.footprint,
                                "\(part.spec.pins.count) pins", part.source]
                    .filter { !$0.isEmpty }.joined(separator: " · "))
                    .font(.caption).foregroundStyle(Theme.textMuted)
                if !part.ok {
                    Text(verbatim: part.error).font(.caption).foregroundStyle(Theme.error)
                }
                ForEach(Array(part.warnings.enumerated()), id: \.offset) { _, warning in
                    Text(verbatim: warning).font(.caption2).foregroundStyle(Theme.warning)
                }
            }
        }
        .padding(.vertical, 2)
    }
}
