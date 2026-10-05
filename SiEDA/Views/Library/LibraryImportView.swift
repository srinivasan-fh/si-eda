import SwiftUI

/// Library import review (Component Library → Import Library…): the parts the core read from KiCad / Eagle / Altium
/// library files, each with its symbol, footprint and notes. Importable parts start selected; parts that cannot be
/// imported show why. For a symbol the footprint can be chosen: Automatic (the core's pairing), one of the imported
/// footprints that has a pad for every pin (best match first), or any other imported footprint; the files are then
/// read again with that pair. The selected row's symbol and footprint are previewed side by side. The selected parts
/// go to `onAdd`.
struct LibraryImportView: View {
    let files: [LibraryImportFile]
    let onAdd: ([CustomPartSpec]) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var result: LibraryImportResult
    @State private var pairs: [String: String] = [:]
    /// Parts the user deselected (by key), kept across re-imports.
    @State private var deselected: Set<String> = []
    @State private var focus: String?
    @State private var preview: CustomPartInfo?
    @State private var previewError: String?
    @State private var working = false
    @State private var task: Task<Void, Never>?

    init(result: LibraryImportResult, files: [LibraryImportFile] = [], onAdd: @escaping ([CustomPartSpec]) -> Void) {
        self.files = files
        self.onAdd = onAdd
        _result = State(initialValue: result)
        _focus = State(initialValue: result.parts.first.map(Self.key))
    }

    /// A part's identity across re-imports: its symbol (or, for a footprint on its own, the footprint).
    static func key(_ part: LibraryImportResult.Part) -> String {
        part.symbol.isEmpty ? "fp|\(part.footprint)|\(part.source)" : "sym|\(part.symbol)|\(part.name)"
    }

    private func isSelected(_ part: LibraryImportResult.Part) -> Bool { part.ok && !deselected.contains(Self.key(part)) }
    private var selectedParts: [LibraryImportResult.Part] { result.parts.filter(isSelected) }
    private var focused: LibraryImportResult.Part? { result.parts.first { Self.key($0) == focus } }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Import Library")
                    .font(.headline)
                    .foregroundStyle(Theme.skyBlue)
                if working { ProgressView().controlSize(.small) }
            }
            .padding(12)
            Divider()
            HStack(spacing: 0) {
                List(selection: $focus) {
                    ForEach(Array(result.files.enumerated()), id: \.offset) { _, file in
                        if !file.error.isEmpty {
                            Label {
                                Text(verbatim: "\(file.name): \(file.error)").font(.caption)
                            } icon: {
                                Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                            }
                        }
                    }
                    ForEach(result.parts, id: \.listKey) { part in
                        row(part).tag(Self.key(part))
                    }
                }
                .listStyle(.inset)
                .scrollContentBackground(.hidden)
                .frame(minWidth: 420)
                Divider()
                previewPanel
                    .frame(width: 300)
            }
            Divider()
            HStack {
                Text(verbatim: "\(selectedParts.count) / \(result.parts.count)")
                    .font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
                Button("Cancel") { dismiss() }
                    .keyboardShortcut(.cancelAction)
                Button("Add Selected Parts") {
                    onAdd(selectedParts.map(\.spec))
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
                .disabled(selectedParts.isEmpty || working)
            }
            .padding(12)
        }
        .frame(minWidth: 760, minHeight: 480)
        .background(Theme.navy)
        .onAppear { refreshPreview() }
        .onChange(of: focus) { _, _ in refreshPreview() }
        .onDisappear { task?.cancel() }
    }

    private func row(_ part: LibraryImportResult.Part) -> some View {
        HStack(alignment: .top, spacing: 8) {
            Toggle(isOn: Binding(get: { isSelected(part) },
                                 set: { on in
                                     if on { deselected.remove(Self.key(part)) } else { deselected.insert(Self.key(part)) }
                                 })) {
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
                if part.pairable == true, !(result.footprintList ?? []).isEmpty {
                    footprintPicker(part)
                }
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

    /// Automatic, the footprints with a pad for every pin (best first), then the other imported footprints.
    private func footprintPicker(_ part: LibraryImportResult.Part) -> some View {
        let candidates = part.candidates ?? []
        let others = (result.footprintList ?? []).map(\.name).filter { !candidates.contains($0) }
        let pads = Dictionary((result.footprintList ?? []).map { ($0.name, $0.pads) }, uniquingKeysWith: { a, _ in a })
        return Picker("Footprint", selection: Binding(get: { pairs[part.symbol] ?? "" }, set: { choose($0, for: part.symbol) })) {
            Text("Automatic").tag("")
            if !candidates.isEmpty {
                Section("Fits every pin") {
                    ForEach(candidates, id: \.self) { name in
                        Text(verbatim: "\(name) (\(pads[name] ?? 0) pads)").tag(name)
                    }
                }
            }
            if !others.isEmpty {
                Section("Other footprints") {
                    ForEach(others, id: \.self) { name in
                        Text(verbatim: "\(name) (\(pads[name] ?? 0) pads)").tag(name)
                    }
                }
            }
        }
        .font(.caption)
        .frame(maxWidth: 360)
        .help("Choose the footprint for this symbol: the import is read again with this pair")
        .disabled(files.isEmpty || working)
    }

    private func choose(_ footprint: String, for symbol: String) {
        if footprint.isEmpty { pairs[symbol] = nil } else { pairs[symbol] = footprint }
        reimport()
    }

    /// Reads the files again with the chosen pairs (off the main thread; a newer choice cancels an older read).
    private func reimport() {
        task?.cancel()
        let files = self.files
        let pairs = self.pairs
        working = true
        task = Task { @MainActor in
            let updated = await Task.detached(priority: .userInitiated) { EDAEngine.importLibrary(files: files, pairs: pairs) }.value
            guard !Task.isCancelled else { return }
            result = updated
            working = false
            if focused == nil { focus = updated.parts.first.map(Self.key) }
            refreshPreview()
        }
    }

    // MARK: - Preview

    private var previewPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("SYMBOL").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    SymbolPreview(kind: .custom, value: preview.name, custom: preview, showPinLabels: true)
                } else {
                    placeholder
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 200)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.schematicBackground))
            Text("FOOTPRINT").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    FootprintPreview(geometry: preview.footprintGeometry)
                } else {
                    placeholder
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 180)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.pcbBackground))
            if let preview {
                Text(verbatim: "\(preview.footprintGeometry.label) · \(preview.footprintGeometry.pads.count) pads")
                    .font(.caption).foregroundStyle(Theme.textSecondary)
            }
            Spacer()
        }
        .padding(12)
    }

    @ViewBuilder
    private var placeholder: some View {
        if let previewError {
            Text(verbatim: previewError).font(.caption).foregroundStyle(Theme.warning).padding(8)
        } else {
            Text("Select an importable part to preview it.").font(.caption).foregroundStyle(Theme.textMuted).padding(8)
        }
    }

    private func refreshPreview() {
        guard let part = focused, part.ok else {
            preview = nil
            previewError = focused.flatMap { $0.error.isEmpty ? nil : $0.error }
            return
        }
        switch EDAEngine.previewCustomPart(part.spec) {
        case .success(let info):
            preview = info
            previewError = nil
        case .failure(let error):
            preview = nil
            previewError = error.localizedDescription
        }
    }
}

private extension LibraryImportResult.Part {
    /// Unique row id (two parts of one symbol cannot occur; footprint parts carry their source).
    var listKey: String { LibraryImportView.key(self) }
}
