import SwiftUI
import UniformTypeIdentifiers

/// Component Library workspace (Altium "SCH Library" + "PCB Library" in one): import a datasheet, let the
/// Datasheet Analyst agent extract the pinout, review it in the pin table, and save a placeable part with a
/// generated symbol, footprint and 3D body.
struct ComponentLibraryView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings

    @State private var selectedId: String?
    @State private var draft = CustomPartSpec()
    @State private var editingId: String?
    @State private var preview: CustomPartInfo?
    @State private var previewError: String?
    @State private var importState: ImportState = .idle
    @State private var importNotes: [String] = []
    @State private var packageHint = ""
    @State private var showImporter = false
    @State private var dropTargeted = false
    @State private var previewTask: Task<Void, Never>?
    @State private var confirmDelete = false

    enum ImportState: Equatable {
        case idle
        case running(String)
        case done(String)
        case failed(String)
    }

    var body: some View {
        HSplitView {
            libraryList
                .frame(minWidth: 220, idealWidth: 250, maxWidth: 320)
            editor
                .frame(minWidth: 460)
            previews
                .frame(minWidth: 280, idealWidth: 340)
        }
        .background(Theme.navy)
        .fileImporter(isPresented: $showImporter,
                      allowedContentTypes: [.pdf, .image, .plainText, UTType(filenameExtension: "md") ?? .plainText],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first { importDatasheet(url) }
        }
        .onAppear {
            if let focus = store.libraryFocusPartId, let part = store.snapshot.customParts.first(where: { $0.id == focus }) {
                load(part)
                store.libraryFocusPartId = nil
            } else if draft.pins.isEmpty, let first = store.snapshot.customParts.first {
                load(first)
            }
            refreshPreview()
        }
        .onChange(of: draft) { _, _ in refreshPreview() }
    }

    // MARK: - Library list

    private var libraryList: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("COMPONENT LIBRARY").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            HStack {
                Button { showImporter = true } label: { Label("Import Datasheet…", systemImage: "doc.viewfinder") }
                    .buttonStyle(.borderedProminent)
                Button {
                    newPart()
                } label: { Image(systemName: "plus") }
                    .buttonStyle(.bordered)
                    .help("New blank part")
            }
            dropZone
            List(selection: $selectedId) {
                ForEach(store.snapshot.customParts) { part in
                    HStack {
                        Image(systemName: "cpu.fill").foregroundStyle(Theme.blue)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(part.name).foregroundStyle(Theme.textPrimary)
                            Text("\(part.footprint) · \(part.pins.count) pins")
                                .font(.caption).foregroundStyle(Theme.textMuted)
                        }
                    }
                    .tag(part.id)
                }
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)
            .onChange(of: selectedId) { _, id in
                if let id, let part = store.snapshot.customParts.first(where: { $0.id == id }) { load(part) }
            }
            if store.snapshot.customParts.isEmpty {
                Text("Parts you import or create appear here and in the schematic device picker.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
        }
        .padding(12)
        .background(Theme.deepBlue.opacity(0.6))
    }

    private var dropZone: some View {
        VStack(spacing: 6) {
            Image(systemName: "arrow.down.doc").font(.title2).foregroundStyle(Theme.skyBlue)
            Text("Drop a datasheet PDF or pinout image").font(.caption).foregroundStyle(Theme.textSecondary)
            TextField("Package (optional, e.g. SOIC-8)", text: $packageHint)
                .textFieldStyle(.roundedBorder)
                .font(.caption)
            importStatus
        }
        .padding(10)
        .frame(maxWidth: .infinity)
        .background(
            RoundedRectangle(cornerRadius: 10)
                .strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
                .foregroundStyle(dropTargeted ? Theme.skyBlue : Theme.blue.opacity(0.5))
        )
        .onDrop(of: [.fileURL], isTargeted: $dropTargeted) { providers in
            guard let provider = providers.first else { return false }
            _ = provider.loadObject(ofClass: URL.self) { url, _ in
                if let url { Task { @MainActor in importDatasheet(url) } }
            }
            return true
        }
    }

    @ViewBuilder
    private var importStatus: some View {
        switch importState {
        case .idle:
            EmptyView()
        case .running(let message):
            HStack(spacing: 6) {
                ProgressView().controlSize(.small)
                Text(message).font(.caption).foregroundStyle(Theme.skyBlue)
            }
        case .done(let message):
            Label(message, systemImage: "checkmark.circle.fill").font(.caption).foregroundStyle(Theme.skyBlue)
        case .failed(let message):
            Label(message, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(Theme.warning)
        }
    }

    // MARK: - Editor

    private var editor: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(editingId == nil ? "New Component" : "Edit Component")
                    .font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                Spacer()
                if !draft.datasheet.isEmpty {
                    Badge(text: draft.datasheet, systemImage: "doc.text")
                }
            }
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 8) {
                GridRow {
                    field("Part number", $draft.name)
                    field("Manufacturer", $draft.manufacturer)
                }
                GridRow {
                    field("Description", $draft.description)
                    HStack {
                        field("Ref prefix", $draft.refPrefix).frame(width: 90)
                        field("Default value", $draft.defaultValue)
                    }
                }
                GridRow {
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Package").font(.caption).foregroundStyle(Theme.textMuted)
                        Picker("", selection: $draft.package.type) {
                            ForEach(PackageKind.allCases) { Text($0.title).tag($0.rawValue) }
                        }
                        .labelsHidden()
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Package pins (0 = from pin list)").font(.caption).foregroundStyle(Theme.textMuted)
                        Stepper(value: $draft.package.pinCount, in: 0...256) {
                            Text("\(draft.package.pinCount)").monospacedDigit().foregroundStyle(Theme.textPrimary)
                        }
                    }
                }
            }

            HStack {
                Text("PINS (\(draft.pins.count))").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                Button { addPin() } label: { Label("Add Pin", systemImage: "plus") }
                Button { renumber() } label: { Label("Renumber", systemImage: "number") }
                    .disabled(draft.pins.isEmpty)
                Button { inferTypes() } label: { Label("Infer Types", systemImage: "wand.and.stars") }
                    .disabled(draft.pins.isEmpty)
            }
            .buttonStyle(.borderless)
            .foregroundStyle(Theme.lightBlue)

            pinTable

            if !importNotes.isEmpty {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(importNotes, id: \.self) { note in
                        Label(note, systemImage: "info.circle").font(.caption).foregroundStyle(Theme.textSecondary)
                    }
                }
            }
            let issues = draft.validationIssues + (previewError.map { [$0] } ?? [])
            if !issues.isEmpty {
                ForEach(issues, id: \.self) { issue in
                    Label(issue, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(Theme.warning)
                }
            }

            HStack {
                if let id = editingId {
                    Button(role: .destructive) {
                        confirmDelete = true
                    } label: { Label("Delete", systemImage: "trash") }
                    .confirmationDialog("Delete “\(draft.name)” from the library?", isPresented: $confirmDelete) {
                        Button("Delete Part", role: .destructive) {
                            store.deleteCustomPart(id)
                            if !store.snapshot.customParts.contains(where: { $0.id == id }) { newPart() }
                        }
                    } message: {
                        Text("You can undo this with ⌘Z. Parts that are placed in the schematic cannot be deleted.")
                    }
                }
                Spacer()
                Button {
                    save(andPlace: true)
                } label: { Label("Save & Place", systemImage: "square.and.arrow.down.on.square") }
                    .disabled(!draft.validationIssues.isEmpty || previewError != nil)
                Button {
                    save(andPlace: false)
                } label: { Label(editingId == nil ? "Add to Library" : "Save Changes", systemImage: "books.vertical") }
                    .buttonStyle(.borderedProminent)
                    .disabled(!draft.validationIssues.isEmpty || previewError != nil)
            }
        }
        .padding(16)
    }

    private var pinTable: some View {
        VStack(spacing: 0) {
            HStack(spacing: 8) {
                Text("No.").frame(width: 46, alignment: .leading)
                Text("Name").frame(width: 110, alignment: .leading)
                Text("Type").frame(width: 130, alignment: .leading)
                Text("Description").frame(maxWidth: .infinity, alignment: .leading)
                Spacer().frame(width: 20)
            }
            .font(.caption.weight(.semibold))
            .foregroundStyle(Theme.textMuted)
            .padding(.horizontal, 8)
            .padding(.vertical, 5)
            .background(Theme.deepBlue)
            ScrollView {
                LazyVStack(spacing: 2) {
                    ForEach($draft.pins) { $pin in
                        HStack(spacing: 8) {
                            TextField("1", text: $pin.number).frame(width: 46)
                            TextField("Name", text: $pin.name).frame(width: 110)
                            Picker("", selection: $pin.type) {
                                ForEach(PinElectricalType.allCases) { type in
                                    Label(type.title, systemImage: type.symbol).tag(type)
                                }
                            }
                            .labelsHidden()
                            .frame(width: 130)
                            TextField("Description", text: $pin.description)
                            Button {
                                draft.pins.removeAll { $0.id == pin.id }
                            } label: { Image(systemName: "minus.circle") }
                                .accessibilityLabel("Remove pin \(pin.number)")
                                .buttonStyle(.borderless)
                                .foregroundStyle(Theme.lightBlue)
                                .frame(width: 20)
                        }
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.callout, design: .monospaced))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 2)
                    }
                }
            }
            .frame(minHeight: 180)
        }
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.7)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Theme.blue.opacity(0.3)))
    }

    private func field(_ title: String, _ text: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            TextField(title, text: text).textFieldStyle(.roundedBorder)
        }
    }

    // MARK: - Previews

    private var previews: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("SYMBOL").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    SymbolPreview(kind: .custom, value: preview.name, custom: preview, showPinLabels: true)
                } else {
                    Text("Preview appears once the part has a name and pins.").font(.caption).foregroundStyle(Theme.textMuted)
                }
            }
            .frame(maxWidth: .infinity, minHeight: 220, maxHeight: 320)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.schematicBackground))

            Text("FOOTPRINT").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    FootprintPreview(geometry: preview.footprintGeometry)
                } else {
                    Color.clear
                }
            }
            .frame(maxWidth: .infinity, minHeight: 180, maxHeight: 260)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.pcbBackground))

            if let preview {
                VStack(alignment: .leading, spacing: 4) {
                    PreviewRow(label: "Package", value: preview.footprintGeometry.label)
                    PreviewRow(label: "Pads", value: "\(preview.footprintGeometry.pads.count)")
                    PreviewRow(label: "Body", value: String(format: "%.1f × %.1f × %.1f mm", preview.footprintGeometry.bodyW,
                                                            preview.footprintGeometry.bodyD, preview.footprintGeometry.bodyH))
                    PreviewRow(label: "Courtyard", value: String(format: "%.1f × %.1f mm", preview.footprintGeometry.courtyardW,
                                                                 preview.footprintGeometry.courtyardH))
                }
            }
            Spacer()
        }
        .padding(14)
        .background(Theme.deepBlue.opacity(0.45))
    }

    // MARK: - Actions

    private func load(_ part: CustomPartInfo) {
        draft = part.spec
        editingId = part.id
        selectedId = part.id
        importNotes = []
    }

    private func newPart() {
        draft = CustomPartSpec()
        editingId = nil
        selectedId = nil
        importNotes = []
        preview = nil
    }

    private func addPin() {
        let next = (draft.pins.compactMap { Int($0.number) }.max() ?? 0) + 1
        draft.pins.append(CustomPartSpec.Pin(number: String(next), name: "P\(next)"))
    }

    private func renumber() {
        for i in draft.pins.indices { draft.pins[i].number = String(i + 1) }
    }

    private func inferTypes() {
        for i in draft.pins.indices {
            draft.pins[i].type = PinElectricalType.infer(name: draft.pins[i].name, description: draft.pins[i].description)
        }
    }

    private func refreshPreview() {
        previewTask?.cancel()
        let spec = draft
        previewTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 200_000_000)
            guard !Task.isCancelled else { return }
            guard spec.validationIssues.isEmpty else {
                preview = nil
                previewError = nil
                return
            }
            switch EDAEngine.previewCustomPart(spec) {
            case .success(let part):
                preview = part
                previewError = nil
            case .failure(let error):
                preview = nil
                previewError = error.localizedDescription
            }
        }
    }

    private func save(andPlace: Bool) {
        guard let part = store.saveCustomPart(draft, replacing: editingId) else { return }
        editingId = part.id
        selectedId = part.id
        if andPlace {
            let x = (store.snapshot.components.map(\.x).max() ?? 0) + 160
            store.addCustomComponent(partId: part.id, at: CGPoint(x: x, y: 0))
            store.workspace = .schematic
        }
    }

    private func importDatasheet(_ url: URL) {
        let hint = packageHint
        let usesAI = settings.aiEnabled && (settings.hasCredentials(for: settings.provider) || !settings.provider.requiresAPIKey)
        let provider: AIProvider = usesAI ? settings.makeProvider() : OfflineProvider()
        importState = .running("Reading \(url.lastPathComponent)…")
        Task { @MainActor in
            let scoped = url.startAccessingSecurityScopedResource()
            defer { if scoped { url.stopAccessingSecurityScopedResource() } }
            do {
                let document = try await Task.detached(priority: .userInitiated) { try DatasheetDocument.load(url: url) }.value
                importState = .running("\(provider.displayName) is reading the pinout (\(document.pageCount) page\(document.pageCount == 1 ? "" : "s"))…")
                let result = try await DatasheetAnalyst.extract(document, hint: hint, provider: provider)
                draft = result.spec
                editingId = nil
                selectedId = nil
                var notes = result.notes
                if let notice = result.notice { notes.insert(notice, at: 0) }
                if !usesAI {
                    notes.insert(settings.aiEnabled
                                 ? "No API key configured — used the offline pin-table parser."
                                 : "AI assistance is off — parsed the pin table offline (nothing left this Mac).", at: 0)
                }
                importNotes = notes
                importState = .done("Extracted \(result.spec.pins.count) pins — review, then add to the library.")
            } catch {
                importState = .failed(error.localizedDescription)
            }
        }
    }
}

private struct PreviewRow: View {
    var label: String
    var value: String

    var body: some View {
        HStack {
            Text(label).foregroundStyle(Theme.textMuted)
            Spacer()
            Text(value).foregroundStyle(Theme.textPrimary).monospacedDigit()
        }
        .font(.caption)
    }
}

/// Top view of a generated footprint: pads, pin-1 marker, body and courtyard.
struct FootprintPreview: View {
    var geometry: CustomPartInfo.FootprintGeometry

    var body: some View {
        Canvas { ctx, size in
            let w = max(geometry.courtyardW, geometry.bodyW) + 1
            let h = max(geometry.courtyardH, geometry.bodyD) + 1
            let scale = min(size.width / w, size.height / h)
            let t = CGAffineTransform(translationX: size.width / 2, y: size.height / 2).scaledBy(x: scale, y: scale)
            let court = CGRect(x: -geometry.courtyardW / 2, y: -geometry.courtyardH / 2,
                               width: geometry.courtyardW, height: geometry.courtyardH)
            ctx.stroke(Path(court).applying(t), with: .color(Theme.lightBlue.opacity(0.4)), style: StrokeStyle(lineWidth: 1, dash: [3, 3]))
            let body = CGRect(x: -geometry.bodyW / 2, y: -geometry.bodyD / 2, width: geometry.bodyW, height: geometry.bodyD)
            ctx.fill(Path(body).applying(t), with: .color(Theme.boardFill))
            ctx.stroke(Path(body).applying(t), with: .color(Theme.silkscreen.opacity(0.8)), lineWidth: 1)
            for pad in geometry.pads {
                let r = CGRect(x: pad.x - pad.w / 2, y: pad.y - pad.h / 2, width: pad.w, height: pad.h)
                let shape = pad.round ? Path(ellipseIn: r) : Path(roundedRect: r, cornerRadius: min(pad.w, pad.h) * 0.15)
                ctx.fill(shape.applying(t), with: .color(pad.pin < 0 ? Theme.pad.opacity(0.35) : Theme.pad))
                if pad.throughHole {
                    let d = min(pad.w, pad.h) * 0.5
                    ctx.fill(Path(ellipseIn: CGRect(x: pad.x - d / 2, y: pad.y - d / 2, width: d, height: d)).applying(t),
                             with: .color(Theme.pcbBackground))
                }
                if scale > 18 {
                    ctx.draw(Text("\(pad.number)").font(.system(size: 8, weight: .bold, design: .monospaced))
                                .foregroundColor(Theme.navy), at: CGPoint(x: pad.x, y: pad.y).applying(t))
                }
            }
            if let first = geometry.pads.first {
                let marker = CGPoint(x: first.x + (first.x < 0 ? -1 : 1) * (first.w / 2 + 0.5), y: first.y)
                let s = marker.applying(t)
                ctx.fill(Path(ellipseIn: CGRect(x: s.x - 3, y: s.y - 3, width: 6, height: 6)), with: .color(Theme.silkscreen))
            }
        }
    }
}
