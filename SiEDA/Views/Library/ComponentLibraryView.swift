import AppKit
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
    @State private var standardSearch = ""
    @State private var kitFilter = ""
    @State private var robotKits: [RobotPlatformInfo] = []
    @State private var footprintSource: CustomPartSpec?
    @State private var symbolSource: CustomPartSpec?
    @State private var footprintError: String?
    @State private var libraryImport: LibraryImportResult?
    @State private var showSupplierSearch = false
    /// The files of the last library import (read again when a footprint is chosen for a symbol).
    @State private var libraryImportFiles: [LibraryImportFile] = []
    @State private var model3dSource: CustomPartSpec?
    /// Sourcing of a distributor part being drawn in the pin table: applied when it is saved and placed.
    @State private var pendingSourcing: SourcingUpdate?
    /// The distributor's datasheet of that part, offered for pin extraction.
    @State private var supplierDatasheet: URL?

    enum ImportState: Equatable {
        case idle
        case running(String)
        case done(String)
        case failed(String)
    }

    /// Selection tag prefix of standard-library rows (project parts are tagged with their id).
    static let standardTag = "std:"

    /// Below this width the symbol and footprint previews move under the editor instead of a third column.
    static let threeColumnWidth: CGFloat = 1000

    var body: some View {
        GeometryReader { geometry in
            let pinRows = Self.pinTableHeight(for: geometry.size.height)
            // Plain SwiftUI stacks (no split views inside the window's split view), sized from the proposal only.
            if geometry.size.width >= Self.threeColumnWidth {
                HStack(spacing: 0) {
                    libraryList
                        .frame(width: 250)
                    divider
                    ScrollView { editor(pinTableHeight: pinRows) }
                        .frame(maxWidth: .infinity)
                    divider
                    ScrollView { previews(stacked: true) }
                        .frame(width: 320)
                        .background(Theme.deepBlue.opacity(0.45))
                }
            } else {
                HStack(spacing: 0) {
                    libraryList
                        .frame(width: 220)
                    divider
                    ScrollView {
                        VStack(alignment: .leading, spacing: 0) {
                            editor(pinTableHeight: pinRows)
                            previews(stacked: false)
                                .background(Theme.deepBlue.opacity(0.45))
                        }
                    }
                    .frame(maxWidth: .infinity)
                }
            }
        }
        .background(Theme.navy)
        .fileImporter(isPresented: $showImporter,
                      allowedContentTypes: [.pdf, .image, .plainText, UTType(filenameExtension: "md") ?? .plainText],
                      allowsMultipleSelection: false) { result in
            if case .success(let urls) = result, let url = urls.first { importDatasheet(url) }
        }
        .onAppear {
            loadRobotKits()
            if let focus = store.libraryFocusPartId, let part = store.snapshot.customParts.first(where: { $0.id == focus }) {
                load(part)
                store.libraryFocusPartId = nil
            } else if draft.pins.isEmpty, let first = store.snapshot.customParts.first {
                load(first)
            }
            refreshPreview()
        }
        .onChange(of: draft) { _, _ in
            // An arranged symbol follows the pin list: new pins join the shorter side, removed pins leave it.
            if draft.symbolLayout != nil {
                var symbol = SymbolDraft(spec: draft)
                if symbol.reconcile(with: draft.pins) {
                    draft.symbolLayout = symbol.layout
                    return
                }
            }
            refreshPreview()
        }
        .sheet(isPresented: Binding(get: { symbolSource != nil }, set: { if !$0 { symbolSource = nil } })) {
            if let source = symbolSource {
                SymbolEditorView(spec: source) { edited in draft = edited }
            }
        }
        .sheet(isPresented: Binding(get: { footprintSource != nil }, set: { if !$0 { footprintSource = nil } })) {
            if let source = footprintSource {
                FootprintEditorView(spec: source) { edited in draft = edited }
            }
        }
        .sheet(isPresented: Binding(get: { model3dSource != nil }, set: { if !$0 { model3dSource = nil } })) {
            if let source = model3dSource {
                Model3DEditorView(spec: source) { edited in draft.model3d = edited.model3d }
            }
        }
        .sheet(isPresented: $showSupplierSearch) {
            SupplierSearchView(initialQuery: draft.pins.isEmpty ? draft.name : "") { part, place in
                chooseSupplierPart(part, place: place)
            }
        }
        .sheet(isPresented: Binding(get: { libraryImport != nil }, set: { if !$0 { libraryImport = nil } })) {
            if let result = libraryImport {
                LibraryImportView(result: result, files: libraryImportFiles) { specs in
                    let added = store.importLibraryParts(specs)
                    if let first = added.first { selectedId = first }
                    importState = .done("Added \(added.count) library parts.")
                }
            }
        }
    }

    // MARK: - Library list

    /// Robot platforms that carry a production parts kit (rover, drone, arm, quadruped, humanoid, 3D printer, CNC);
    /// read once when the library opens.
    private func loadRobotKits() {
        if robotKits.isEmpty { robotKits = store.robotSegments().platforms.filter { !($0.kit ?? []).isEmpty } }
    }
    private var kitName: String { robotKits.first(where: { $0.id == kitFilter })?.name ?? kitFilter }
    private var kitParts: Set<String>? {
        guard !kitFilter.isEmpty, let kit = robotKits.first(where: { $0.id == kitFilter })?.kit else { return nil }
        return Set(kit.flatMap(\.parts))
    }

    /// Standard parts matching a search and, if set, a robot kit. The search is a `PartQuery`: words in the name,
    /// category, manufacturer, description or package, and filters such as `cat:sensors pkg:soic pins:8 mfr:ti`.
    static func filterStandard(_ parts: [StandardPart], search: String, kit: Set<String>?) -> [StandardPart] {
        let query = PartQuery(search)
        return parts.filter { part in
            if let kit, !kit.contains(part.spec.name) { return false }
            return query.isEmpty || query.matches(part)
        }
    }

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
            Button { importLibrary() } label: { Label("Import Library…", systemImage: "books.vertical") }
                .buttonStyle(.bordered)
                .help("Import KiCad (.kicad_mod, .kicad_sym), Eagle (.lbr) or Altium (.SchLib, .PcbLib) libraries")
            Button { showSupplierSearch = true } label: { Label("Find Parts Online…", systemImage: "shippingbox") }
                .buttonStyle(.bordered)
                .help("Search Octopart, DigiKey and Mouser: stock, prices, lifecycle and datasheets")
            dropZone
            HStack(spacing: 6) {
                TextField("Search parts", text: $standardSearch)
                    .textFieldStyle(.roundedBorder)
                    .accessibilityLabel("Search the standard library")
                    .help("Words, or filters: cat:sensors pkg:soic pins:8 mfr:ti")
                Picker("", selection: $kitFilter) {
                    Text("All").tag("")
                    ForEach(robotKits) { Text($0.name).tag($0.id) }
                }
                .labelsHidden()
                .frame(width: 90)
                .help("Show one robot platform's production parts kit")
            }
            List(selection: $selectedId) {
                Section("Project Library") {
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
                let inLibrary = Set(store.snapshot.customParts.map(\.name))
                let standard = Self.filterStandard(StandardLibrary.parts.filter { !inLibrary.contains($0.spec.name) },
                                                   search: standardSearch, kit: kitParts)
                if !standard.isEmpty {
                    Section(kitFilter.isEmpty ? "Standard Library" : "Standard Library · \(kitName)") {
                        ForEach(standard) { part in
                            HStack {
                                Image(systemName: "cpu").foregroundStyle(Theme.lightBlue)
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(part.spec.name).foregroundStyle(Theme.textPrimary)
                                    Text("\(part.category) · \(part.packageSummary)")
                                        .font(.caption).foregroundStyle(Theme.textMuted)
                                }
                                Spacer()
                                Button {
                                    if let id = store.addStandardPartToLibrary(part) { selectedId = id }
                                } label: { Image(systemName: "plus.circle") }
                                    .buttonStyle(.borderless)
                                    .foregroundStyle(Theme.skyBlue)
                                    .help("Add \(part.spec.name) to the project library")
                                    .accessibilityLabel("Add \(part.spec.name) to the project library")
                            }
                            .help(part.spec.description)
                            .tag(Self.standardTag + part.id)
                        }
                    }
                }
            }
            .listStyle(.sidebar)
            .scrollContentBackground(.hidden)
            .onChange(of: selectedId) { _, id in
                guard let id else { return }
                if let part = store.snapshot.customParts.first(where: { $0.id == id }) {
                    load(part)
                } else if id.hasPrefix(Self.standardTag),
                          let part = StandardLibrary.parts.first(where: { Self.standardTag + $0.id == id }) {
                    // Preview a standard part as a new, unsaved draft: "Add to Library" copies it into the project.
                    draft = part.spec
                    editingId = nil
                    importNotes = ["\(part.spec.name) from the standard library (\(part.category)). Add it to the library to place it."]
                }
            }
            if store.snapshot.customParts.isEmpty {
                Text("Parts you import, create or add from the standard library appear here and in the schematic device picker.")
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
                .textFieldStyle(.blue)
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

    private var divider: some View {
        Rectangle().fill(Theme.blue.opacity(0.3)).frame(width: 1)
    }

    /// Pin table height: fills what the window leaves after the part fields and buttons.
    static func pinTableHeight(for available: CGFloat) -> CGFloat {
        min(max(available - 360, 160), 520)
    }

    private func editor(pinTableHeight: CGFloat) -> some View {
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
                            if PackageKind(rawValue: draft.package.type) == nil {
                                // Catalog packages (LGA land pattern, BGA, TO-263, SON …) keep their own geometry.
                                Text("\(draft.package.type) (from the part's datasheet)").tag(draft.package.type)
                            }
                        }
                        .labelsHidden()
                    }
                    VStack(alignment: .leading, spacing: 3) {
                        Text("Package pins (0 = from pin list)").font(.caption).foregroundStyle(Theme.textMuted)
                        Stepper(value: $draft.package.pinCount, in: 0...512) {
                            Text("\(draft.package.pinCount)").monospacedDigit().foregroundStyle(Theme.textPrimary)
                        }
                        .disabled(draft.package.usesLandPattern)
                    }
                }
                GridRow {
                    HStack {
                        Button { symbolSource = draft } label: { Label("Edit Symbol…", systemImage: "rectangle.connected.to.line.below") }
                            .disabled(draft.pins.isEmpty)
                            .help("Arrange the schematic symbol: pins on any side, groups, stacked power pins, auto arrange")
                        Button { openFootprintEditor() } label: { Label("Edit Footprint…", systemImage: "square.grid.3x3.topleft.filled") }
                            .disabled(draft.pins.isEmpty)
                            .help("Draw the land pattern pad by pad: position, size, shape, drill and pin of every pad")
                        Button { model3dSource = draft } label: { Label("3D Model…", systemImage: "cube") }
                            .disabled(draft.pins.isEmpty)
                            .help("Attach a VRML (.wrl), STL or OBJ model and align it on the footprint")
                        if let model = draft.model3d {
                            Text(verbatim: model.name).font(.caption).foregroundStyle(Theme.textSecondary).lineLimit(1)
                        }
                        if draft.symbolLayout != nil {
                            Text("Arranged symbol").font(.caption).foregroundStyle(Theme.textSecondary)
                        }
                        if draft.package.usesLandPattern {
                            Text(draft.package.type == "CUSTOM" ? "Custom footprint · \(draft.package.lands?.count ?? 0) pads"
                                                                : "Land pattern · \(draft.package.lands?.count ?? 0) pads")
                                .font(.caption).foregroundStyle(Theme.textSecondary)
                        }
                        if let footprintError {
                            Text(footprintError).font(.caption).foregroundStyle(Theme.error).lineLimit(2)
                        }
                    }
                    .gridCellColumns(2)
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
                .frame(height: pinTableHeight)

            if !importNotes.isEmpty {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(importNotes, id: \.self) { note in
                        Label(note, systemImage: "info.circle").font(.caption).foregroundStyle(Theme.textSecondary)
                    }
                    if let url = supplierDatasheet, draft.pins.isEmpty {
                        Button { readSupplierDatasheet(url) } label: { Label("Read Pins from Datasheet", systemImage: "doc.viewfinder") }
                            .help("Download the distributor's datasheet PDF and extract its pin table")
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
                    if draft.pins.isEmpty {
                        Text("No pins yet — import a datasheet or press Add Pin.")
                            .font(.caption)
                            .foregroundStyle(Theme.textMuted)
                            .frame(maxWidth: .infinity)
                            .padding(.vertical, 24)
                    }
                    ForEach(draft.pins) { row in
                        // Id-keyed bindings: an index-based `$draft.pins` binding outlives a deleted row (focused field
                        // commits after the removal) and traps with "index out of range".
                        let pin = pinBinding(row)
                        HStack(spacing: 8) {
                            TextField("1", text: pin.number).frame(width: 46)
                            TextField("Name", text: pin.name).frame(width: 110)
                            Picker("", selection: pin.type) {
                                ForEach(PinElectricalType.allCases) { type in
                                    Label(type.title, systemImage: type.symbol).tag(type)
                                }
                            }
                            .labelsHidden()
                            .frame(width: 130)
                            TextField("Description", text: pin.description)
                            Button {
                                removePin(row.id)
                            } label: { Image(systemName: "minus.circle") }
                                .accessibilityLabel("Remove pin \(row.number)")
                                .buttonStyle(.borderless)
                                .foregroundStyle(Theme.lightBlue)
                                .frame(width: 20)
                        }
                        .textFieldStyle(.blue)
                        .font(.system(.callout, design: .monospaced))
                        .padding(.horizontal, 8)
                        .padding(.vertical, 2)
                    }
                }
            }
        }
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.7)))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Theme.blue.opacity(0.3)))
    }

    private func field(_ title: String, _ text: Binding<String>) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            TextField(title, text: text).textFieldStyle(.blue)
        }
    }

    // MARK: - Previews

    private var symbolPreview: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("SYMBOL").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    SymbolPreview(kind: .custom, value: preview.name, custom: preview, showPinLabels: true)
                } else {
                    Text("Preview appears once the part has a name and pins.")
                        .font(.caption)
                        .foregroundStyle(Theme.textMuted)
                        .multilineTextAlignment(.center)
                        .padding(12)
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 240)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.schematicBackground))
        }
    }

    private var footprintPreview: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("FOOTPRINT").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Group {
                if let preview {
                    FootprintPreview(geometry: preview.footprintGeometry)
                } else {
                    Color.clear
                }
            }
            .frame(maxWidth: .infinity)
            .frame(height: 200)
            .background(RoundedRectangle(cornerRadius: 10).fill(Theme.pcbBackground))
        }
    }

    /// Symbol, footprint and package facts: one column (`stacked`) or symbol and footprint side by side.
    private func previews(stacked: Bool) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            if stacked {
                symbolPreview
                footprintPreview
            } else {
                HStack(alignment: .top, spacing: 12) {
                    symbolPreview
                    footprintPreview
                }
            }

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
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    // MARK: - Actions

    /// Opens the footprint editor on the draft: a land-pattern part as it is, any other part converted from its
    /// generated footprint first (same pads, same pins).
    private func openFootprintEditor() {
        footprintError = nil
        if draft.package.usesLandPattern, !(draft.package.lands ?? []).isEmpty {
            footprintSource = draft
            return
        }
        switch EDAEngine.landPattern(draft) {
        case .success(let editable): footprintSource = editable
        case .failure(let error): footprintError = error.localizedDescription
        }
    }

    private func load(_ part: CustomPartInfo) {
        supplierDatasheet = nil
        pendingSourcing = nil
        draft = part.spec
        editingId = part.id
        selectedId = part.id
        importNotes = []
    }

    private func newPart() {
        supplierDatasheet = nil
        pendingSourcing = nil
        draft = CustomPartSpec()
        editingId = nil
        selectedId = nil
        importNotes = []
        preview = nil
    }

    private func pinBinding(_ row: CustomPartSpec.Pin) -> Binding<CustomPartSpec.Pin> {
        Binding(
            get: { draft.pins.first { $0.id == row.id } ?? row },
            set: { value in
                if let i = draft.pins.firstIndex(where: { $0.id == row.id }) { draft.pins[i] = value }
            })
    }

    private func removePin(_ id: CustomPartSpec.Pin.ID) {
        NSApp.keyWindow?.makeFirstResponder(nil)  // commit (or drop) the edit in progress first
        DispatchQueue.main.async { draft.pins.removeAll { $0.id == id } }
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
            if let sourcing = pendingSourcing,
               sourcing.mpn.map(SupplierPlacement.normalized) == SupplierPlacement.normalized(part.name) {
                store.placeLibraryPart(part.id, sourcing: sourcing)
                pendingSourcing = nil
            } else {
                let x = (store.snapshot.components.map(\.x).max() ?? 0) + 160
                store.addCustomComponent(partId: part.id, at: CGPoint(x: x, y: 0))
            }
            store.workspace = .schematic
        }
    }

    // MARK: - Distributor parts

    /// A part chosen in Find Parts Online: linked to the project library or the catalog part with the same part number
    /// (placed with its sourcing, or added to the library), else opened as a new part in the pin table, prefilled.
    private func chooseSupplierPart(_ part: SupplierPartInfo, place: Bool) {
        let settings = SupplierSettings.shared
        let sourcing = SupplierPlacement.sourcing(for: part, boards: store.bomReport.buildQuantity, currency: settings.currency)
        var partId: String?
        var note: String?
        switch SupplierPlacement.link(for: part, library: store.snapshot.customParts) {
        case .library(let id, _):
            partId = id
        case .catalog(let name, let packingNote):
            if let standard = StandardLibrary.parts.first(where: { $0.spec.name == name }) {
                partId = store.addStandardPartToLibrary(standard)
                if !packingNote.isEmpty { note = packingNote }
            }
        case .none(let candidates):
            draft = SupplierPlacement.draft(for: part)
            editingId = nil
            selectedId = nil
            pendingSourcing = sourcing
            supplierDatasheet = URL(string: part.datasheet).flatMap { $0.scheme == "https" || $0.scheme == "http" ? $0 : nil }
            var notes = ["\(part.mpn) (\(part.manufacturer)) has no symbol or footprint yet: add its pins from the datasheet, "
                         + "then Save & Place. Its part number, supplier number and price are kept."]
            if !candidates.isEmpty { notes.append("Similar catalog parts: \(candidates.joined(separator: ", ")).") }
            importNotes = notes
            importState = .idle
            return
        }
        guard let partId else { return }
        if place {
            store.placeLibraryPart(partId, sourcing: sourcing)
            store.workspace = .schematic
        } else {
            selectedId = partId
        }
        importState = .done(note ?? "\(part.mpn) linked to its library symbol and footprint.")
    }

    /// Downloads the distributor's datasheet (PDF, up to 30 MB) and reads its pin table like a dropped datasheet.
    private func readSupplierDatasheet(_ url: URL) {
        importState = .running("Downloading \(url.lastPathComponent)…")
        let base = SupplierPlacement.normalized(draft.name)
        Task { @MainActor in
            do {
                var request = URLRequest(url: url, timeoutInterval: 30)
                request.setValue("application/pdf", forHTTPHeaderField: "Accept")
                let (data, response) = try await SupplierHTTP.shared.session.data(for: request)
                let status = (response as? HTTPURLResponse)?.statusCode ?? 0
                guard (200..<300).contains(status), data.count > 4, data.count <= 30 << 20,
                      data.prefix(4) == Data("%PDF".utf8) else {
                    importState = .failed("The datasheet link did not return a PDF: open it with the Datasheet link instead.")
                    return
                }
                let file = FileManager.default.temporaryDirectory
                    .appendingPathComponent((base.isEmpty ? "datasheet" : base) + ".pdf")
                try data.write(to: file, options: .atomic)
                importDatasheet(file)
            } catch {
                importState = .failed(error.localizedDescription)
            }
        }
    }

    /// File types the library importer reads (KiCad footprints and symbol libraries, Eagle libraries).
    static let libraryExtensions: Set<String> = ["kicad_mod", "kicad_sym", "lib", "lbr", "schlib", "pcblib", "intlib"]
    /// Binary library files, sent to the core as base64 (Altium compound files).
    static let binaryExtensions: Set<String> = ["schlib", "pcblib", "intlib", "stl"]
    /// 3D models imported with the footprints that name them (KiCad .3dshapes folders).
    static let modelExtensions: Set<String> = ["wrl", "vrml", "stl", "obj"]

    /// Library import: KiCad / Eagle files, or folders of them (a KiCad .pretty or .kicad_symdir). The core reads,
    /// pairs and checks the parts; the review sheet adds the chosen ones to the project library.
    private func importLibrary() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = true
        panel.canChooseFiles = true
        panel.canChooseDirectories = true
        panel.allowedContentTypes = (Self.libraryExtensions.union(Self.modelExtensions)).compactMap { UTType(filenameExtension: $0) }
            + [UTType.folder]
        panel.message = "Choose KiCad footprints (.kicad_mod), symbol libraries (.kicad_sym, KiCad 5 .lib), Eagle libraries (.lbr), "
            + "Altium libraries (.SchLib, .PcbLib), 3D models (.wrl, .stl, .obj) or folders of them."
        guard panel.runModal() == .OK else { return }
        let files = Self.libraryFiles(at: panel.urls)
        guard !files.isEmpty else {
            importState = .failed("No .kicad_mod, .kicad_sym, .lbr, .SchLib or .PcbLib files found.")
            return
        }
        importState = .running("Reading \(files.count) library files…")
        Task { @MainActor in
            let result = await Task.detached(priority: .userInitiated) { EDAEngine.importLibrary(files: files) }.value
            importState = .idle
            libraryImportFiles = files
            libraryImport = result
        }
    }

    /// The library files among `urls`, including those inside chosen folders; at most 2000 files of up to 32 MB.
    /// 3D models (.wrl, .stl, .obj) in the chosen folders come along, and a KiCad `X.pretty` folder also brings the
    /// models of its sibling `X.3dshapes` folder, so footprints get the models they name.
    static func libraryFiles(at urls: [URL]) -> [LibraryImportFile] {
        var found: [URL] = []
        var models: [URL] = []
        let wanted = libraryExtensions.union(modelExtensions)
        func scan(_ folder: URL) -> [URL] {
            let items = FileManager.default.enumerator(at: folder, includingPropertiesForKeys: nil)?.allObjects as? [URL] ?? []
            return items.filter { wanted.contains($0.pathExtension.lowercased()) }.sorted { $0.lastPathComponent < $1.lastPathComponent }
        }
        for url in urls {
            var isDirectory: ObjCBool = false
            guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) else { continue }
            if isDirectory.boolValue {
                found += scan(url)
                if url.pathExtension.lowercased() == "pretty" {
                    let shapes = url.deletingPathExtension().appendingPathExtension("3dshapes")
                    var shapesIsDirectory: ObjCBool = false
                    if FileManager.default.fileExists(atPath: shapes.path, isDirectory: &shapesIsDirectory), shapesIsDirectory.boolValue,
                       !urls.contains(shapes) {
                        models += scan(shapes).filter { modelExtensions.contains($0.pathExtension.lowercased()) }
                    }
                }
            } else {
                found.append(url)
            }
        }
        // Library files first (the limit keeps them), then the models they may name.
        let ordered = found.filter { !modelExtensions.contains($0.pathExtension.lowercased()) }
            + found.filter { modelExtensions.contains($0.pathExtension.lowercased()) } + models
        return ordered.prefix(2000).compactMap { url in
            guard let data = try? Data(contentsOf: url), data.count <= 32 << 20 else { return nil }
            if binaryExtensions.contains(url.pathExtension.lowercased()) {  // binary STL, Altium compound files
                return LibraryImportFile(name: url.lastPathComponent, content: "", contentBase64: data.base64EncodedString())
            }
            return LibraryImportFile(name: url.lastPathComponent, content: String(decoding: data, as: UTF8.self))
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
                var fallbackNote: String?
                let result: DatasheetAnalyst.Result
                do {
                    result = try await DatasheetAnalyst.extract(document, hint: hint, provider: provider)
                } catch let error as AIProviderError where usesAI {
                    // A local server (Ollama, LM Studio…) that isn't running, or no network: parse offline instead.
                    guard case .network(let reason) = error else { throw error }
                    fallbackNote = "Couldn't reach \(provider.displayName) (\(reason)) — parsed the pin table offline."
                    result = try await DatasheetAnalyst.extract(document, hint: hint, provider: OfflineProvider())
                }
                draft = result.spec
                editingId = nil
                selectedId = nil
                var notes = result.notes
                if let notice = result.notice { notes.insert(notice, at: 0) }
                if let fallbackNote { notes.insert(fallbackNote, at: 0) }
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
