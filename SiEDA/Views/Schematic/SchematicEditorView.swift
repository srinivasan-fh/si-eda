import AppKit
import SwiftUI

enum SchematicTool: Equatable {
    case select
    case wire
    case bus
    case noConnect
    case pan
    case place(ComponentKind)
    case placeCustom(String)  // custom part id from the component library

    var title: String {
        switch self {
        case .select: return "Select / Move"
        case .wire: return "Wire"
        case .bus: return "Bus"
        case .noConnect: return "No Connect"
        case .pan: return "Hand (Pan)"
        case .place(let kind): return "Place \(kind.displayName)"
        case .placeCustom: return "Place Custom Part"
        }
    }
}

/// Schematic capture workspace: Photoshop-style tool strip, Proteus-style device picker and simulation
/// transport, Altium-style properties in the inspector.
struct SchematicEditorView: View {
    @EnvironmentObject private var store: DesignStore
    @EnvironmentObject private var settings: AISettings
    @State private var tool: SchematicTool = .select
    @State private var pickerKind: ComponentKind = .resistor
    @State private var showPicker = true
    @State private var viewport = Viewport(scale: 1.6, offset: CGSize(width: 260, height: 260))
    @State private var canvasSize: CGSize = .zero
    @State private var wireStart: String?
    /// What P (and the "Place selected device" tool) arms: the device or custom part last chosen in the picker.
    @State private var placementTool: SchematicTool = .place(.resistor)
    @State private var showMessages = false
    @State private var pastingArray = false
    @State private var arrayCount = "4"
    @State private var arrayStep = "0, 60"
    @State private var arrayIncrement = "1"
    @State private var eco: (title: String, changes: [EcoChangeInfo])?
    @State private var updatingPCB = false
    /// Arrange by the symbols' outlines (edges line up) rather than their reference points.
    @AppStorage("schematic.alignByOutline") private var alignByOutline = true
    /// Device palette as icons only (names in tooltips).
    @AppStorage(DevicePicker.iconsOnlyKey) private var pickerIconsOnly = false
    // Schematic Canvas preferences: colour scheme, custom theme and grid.
    @AppStorage(SchematicColorScheme.storageKey) private var colourScheme = SchematicColorScheme.defaultScheme.rawValue
    @AppStorage(SchematicCustomTheme.storageKey) private var customTheme = ""
    @AppStorage(SchematicGridStyle.storageKey) private var gridStyle = SchematicGridStyle.dots.rawValue
    @AppStorage(SchematicGridStyle.majorStorageKey) private var gridMajorEvery = SchematicGridStyle.defaultMajorEvery
    @AppStorage(SchematicCanvasStyle.colourByDeviceKey) private var colourByDevice = true

    private var canvasStyle: SchematicCanvasStyle {
        SchematicCanvasStyle.from(scheme: colourScheme, customJSON: customTheme, grid: gridStyle, majorEvery: gridMajorEvery,
                                  colourByDevice: colourByDevice)
    }

    var body: some View {
        HStack(spacing: 0) {
            ToolStrip {
                ToolStripButton(systemImage: "cursorarrow", help: "Select / move (V)", isActive: tool == .select) { tool = .select }
                ToolStripButton(systemImage: "hand.raised", help: "Pan (H)", isActive: tool == .pan) { tool = .pan }
                ToolStripButton(systemImage: "line.diagonal", help: "Wire (W) — click two pins", isActive: tool == .wire) { tool = .wire }
                ToolStripButton(systemImage: "line.3.horizontal", help: "Bus (B) — click the corners, click the last point again (or Return) to name it",
                                isActive: tool == .bus) { tool = .bus }
                ToolStripButton(systemImage: "xmark", help: "No connect (Q) — click a pin to mark it intentionally open",
                                isActive: tool == .noConnect) { tool = .noConnect }
                ToolStripDivider()
                ToolStripButton(systemImage: "plus.square.on.square", help: "Place selected device (P)",
                                isActive: tool == placementTool) {
                    tool = placementTool
                }
                ToolStripButton(systemImage: "arrow.down.to.line", help: "Place ground (G)", isActive: tool == .place(.ground)) {
                    tool = .place(.ground)
                }
                ToolStripButton(systemImage: "tag", help: "Place net label (L)", isActive: tool == .place(.netLabel)) {
                    tool = .place(.netLabel)
                }
                ToolStripButton(systemImage: "bolt.circle", help: "Place voltage source", isActive: tool == .place(.voltageSource)) {
                    tool = .place(.voltageSource)
                }
                ToolStripDivider()
                ToolStripButton(systemImage: "rotate.right", help: "Rotate selection (Space or R)") { store.rotateSelection() }
                ToolStripButton(systemImage: "trash", help: "Delete selection (⌫)") { store.deleteSelection() }
                ToolStripDivider()
                ToolStripButton(systemImage: "sidebar.left", help: "Show/hide device picker", isActive: showPicker) {
                    showPicker.toggle()
                }
                ToolStripButton(systemImage: "map", help: "Navigator overview (N)", isActive: store.showNavigator) {
                    store.showNavigator.toggle()
                }
                ToolStripButton(systemImage: "plus.magnifyingglass", help: "Zoom to area (Z) — drag a rectangle") {
                    store.requestView(.zoomArea)
                }
            }

            if showPicker {
                DevicePicker(selected: $pickerKind, customParts: store.snapshot.customParts,
                             isPlacing: tool == .place(pickerKind),
                             onPickCustom: { arm(.placeCustom($0)) },
                             onPickStandard: { part in
                                 guard let id = store.addStandardPartToLibrary(part) else { return nil }
                                 arm(.placeCustom(id))
                                 return id
                             },
                             onImport: { store.workspace = .library }) { kind in
                    pickerKind = kind
                    arm(.place(kind))
                }
                .frame(width: pickerIconsOnly ? DevicePicker.iconsOnlyWidth : 220)
            }

            VStack(spacing: 0) {
                OptionsBar {  // scrolls sideways on narrow windows: the Arrange, Back Annotate, Messages and PDF controls need the room
                    Image(systemName: "wrench.and.screwdriver").foregroundStyle(Theme.blue)
                    Text(tool.title).foregroundStyle(Theme.textPrimary).fontWeight(.semibold)
                    Divider().frame(height: 18)
                    Text(hint)
                        .foregroundStyle(wireStart == nil ? Theme.textMuted : Theme.skyBlue)
                        .lineLimit(1)
                        .truncationMode(.tail)
                    Toggle("Live probes", isOn: $store.showDCOverlay)
                        .toggleStyle(.switch)
                        .controlSize(.mini)
                    Button { store.showFind = true } label: { Image(systemName: "magnifyingglass") }
                        .buttonStyle(.borderless)
                        .keyboardShortcut("f", modifiers: [.command])
                        .help("Find & Replace across every sheet (⌘F)")
                        .accessibilityLabel("Find and replace")
                    Menu {
                        Button("Copy") { store.copySelection() }.disabled(store.selection.isEmpty)
                        Button("Cut") { store.cutSelection() }.disabled(store.selection.isEmpty)
                        Button("Paste") { store.paste() }.disabled(!store.canPaste)
                        Button("Paste Array…") { pastingArray = true }.disabled(!store.canPaste)
                        Divider()
                        Button("Align Left") { store.align("left", byOutline: alignByOutline) }
                        Button("Align Right") { store.align("right", byOutline: alignByOutline) }
                        Button("Align Top") { store.align("top", byOutline: alignByOutline) }
                        Button("Align Bottom") { store.align("bottom", byOutline: alignByOutline) }
                        Button("Align Horizontal Centres") { store.align("centerY", byOutline: alignByOutline) }
                        Button("Align Vertical Centres") { store.align("centerX", byOutline: alignByOutline) }
                        Button("Distribute Horizontally") { store.align("distributeX", byOutline: alignByOutline) }
                        Button("Distribute Vertically") { store.align("distributeY", byOutline: alignByOutline) }
                        Toggle("Align by Symbol Outline", isOn: $alignByOutline)
                    } label: {
                        Label("Arrange", systemImage: "square.on.square.dashed")
                    }
                    .fixedSize()
                    .help("Copy, paste, paste array (designators numbered on, labels counted up), align and distribute")
                    Menu {
                        Button("From Board Positions (Rows)") {
                            eco = (String(localized: "Re-annotate from the board (rows)"), store.ecoFromBoard())
                        }
                        Button("From Board Positions (Columns)") {
                            eco = (String(localized: "Re-annotate from the board (columns)"), store.ecoFromBoard(byColumns: true))
                        }
                        Button("From WAS / IS File…") {
                            if let changes = store.ecoFromWasIsFile() { eco = (String(localized: "Changes from a WAS / IS file"), changes) }
                        }
                        Divider()
                        // Forward annotation (schematic → board) lives here too, so the options bar keeps its width.
                        Button("Update PCB") { updatingPCB = true }
                            .help("Engineering change order: review and execute the schematic's changes on the board")
                    } label: {
                        Label("Back Annotate", systemImage: "arrow.uturn.left.square")
                    }
                    .fixedSize()
                    .help("Board changes (designators, pin swaps, gate swaps) proposed to the schematic for review")
                    Button { showMessages.toggle() } label: { Label("Messages", systemImage: "list.bullet.rectangle") }
                        .help("Every ERC message of every sheet; click one to go there")
                    Button { store.exportSchematicPDF() } label: { Label("PDF", systemImage: "doc.richtext") }
                        .help("PDF of every sheet with bookmarks, frames and title blocks")
                    appearanceMenu
                    Spacer()
                    if !store.selection.isEmpty {
                        Text("\(store.selection.count) selected").foregroundStyle(Theme.skyBlue)
                    }
                    ZoomControls(level: viewport.scale / Viewport.schematicBaseScale,
                                 zoomIn: { store.requestView(.zoomIn) }, zoomOut: { store.requestView(.zoomOut) },
                                 fit: { store.requestView(.fit) }, fitSelection: { store.requestView(.fitSelection) },
                                 setLevel: { store.requestView(.setLevel($0)) })
                }

                SheetBar()

                VStack(spacing: 0) {
                ZStack(alignment: .bottomLeading) {
                    SchematicCanvas(tool: $tool, viewport: $viewport, canvasSize: $canvasSize, wireStart: $wireStart,
                                    placementTool: placementTool, live: store.live)
                        .environment(\.schematicStyle, canvasStyle)
                    if store.showNavigator, !store.sheetSnapshot.components.isEmpty {
                        navigator
                            .canvasScrollShield()
                            .padding(12)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .bottomTrailing)
                    }
                    SimulationTransport(live: store.live)
                        .canvasScrollShield()
                        .padding(12)
                    if let shown = store.snapshot.sheet(store.snapshot.activeSheet), shown.isRepeated,
                       let definition = store.snapshot.sheet(shown.definitionId) {
                        // A channel of a repeated sheet: every edit applies to the whole block.
                        Text("Channel \(shown.channel ?? "") of \(definition.name) — edits apply to every channel")
                            .font(.caption.weight(.semibold))
                            .foregroundStyle(Theme.iceBlue)
                            .padding(.horizontal, 12)
                            .padding(.vertical, 5)
                            .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
                            .overlay(Capsule().strokeBorder(Theme.skyBlue))
                            .padding(10)
                            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .top)
                            .allowsHitTesting(false)
                    }
                    if store.snapshot.components.isEmpty {
                        VStack(spacing: 4) {
                            if settings.aiEnabled {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left and click the canvas to place it, or describe your product in Super Intelligence and let the agents design it.",
                                               actionTitle: "Open Super Intelligence") { store.workspace = .promptStudio }
                            } else {
                                BlueEmptyState(systemImage: "point.3.connected.trianglepath.dotted",
                                               title: "Empty schematic",
                                               message: "Pick a device on the left (or press P), click the canvas to place it, then click two pins to wire them. Or start from a reference design:")
                                    .allowsHitTesting(false)  // clicks fall through to the canvas
                            }
                            Menu {
                                ForEach(OfflineProvider.categories, id: \.self) { category in
                                    Section(category) {
                                        ForEach(OfflineProvider.examples(in: category), id: \.plan.title) { template in
                                            Button(template.plan.title) { store.loadExample(template.industryPlan) }
                                        }
                                    }
                                }
                            } label: {
                                Label("Load Example Design", systemImage: "square.grid.2x2")
                            }
                            .fixedSize()
                            .canvasScrollShield()
                        }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                    }
                }
                if showMessages {
                    Divider()
                    SchematicMessagesPanel(isShown: $showMessages)
                }
                }
            }
        }
        .background(Theme.navy)
        .sheet(isPresented: $updatingPCB) { UpdatePcbView().environmentObject(store) }
        .sheet(isPresented: Binding(get: { eco != nil }, set: { if !$0 { eco = nil } })) {
            if let eco { EcoReviewView(title: eco.title, changes: eco.changes).environmentObject(store) }
        }
        .alert("Paste Array", isPresented: $pastingArray) {
            TextField("Copies", text: $arrayCount)
            TextField("Step x, y", text: $arrayStep)
            TextField("Label increment", text: $arrayIncrement)
            Button("Paste") {
                let parts = arrayStep.split(separator: ",").map { Double($0.trimmingCharacters(in: .whitespaces)) ?? 0 }
                let step = CGSize(width: parts.first ?? 0, height: parts.count > 1 ? parts[1] : 0)
                store.paste(count: max(1, min(256, Int(arrayCount.trimmingCharacters(in: .whitespaces)) ?? 1)),
                            step: step == .zero ? CGSize(width: 0, height: 60) : step,
                            labelIncrement: Int(arrayIncrement.trimmingCharacters(in: .whitespaces)) ?? 0)
            }
            Button("Cancel", role: .cancel) {}
        } message: {
            Text("Copies of the clipboard, each one step further (schematic units, 10 = one grid). Parts get the next free designators; net label numbers count up by the increment (D0 → D1 …).")
        }
        .sheet(isPresented: $store.showFind) { SchematicFindPanel().environmentObject(store) }
        .sheet(isPresented: $store.showSchematicColours) { SchematicThemeEditor() }
        .background(DeleteKeyMonitor { store.deleteSelection() } isActive: {
            store.selectedWire != nil || !store.selection.isEmpty
        })
    }

    /// Colour scheme and grid of the canvas: a compact menu, so the options bar keeps its width.
    private var appearanceMenu: some View {
        Menu {
            SchematicAppearanceMenu(onCustomise: { store.showSchematicColours = true })
        } label: {
            Image(systemName: "paintpalette")
        }
        .fixedSize()
        .help("Colour scheme and grid of the schematic canvas")
        .accessibilityLabel("Colour Scheme")
    }

    private func arm(_ placement: SchematicTool) {
        placementTool = placement
        tool = placement
    }

    /// Contextual instruction for the active tool (Photoshop-style options bar hint).
    private var hint: String {
        if let wireStart {
            return "Wiring from \(wireStart) — click a pin or any wire (T-junction) to connect"
                + (tool == .wire ? " · click empty space for a corner" : "") + " · Esc cancels"
        }
        switch tool {
        case .select: return "Click a pin to wire · drag a wire to bend it · drag parts or junctions to move · Space or R rotates · ⇧-drag box-selects · Space+drag, empty-space drag or middle/right-drag pans · scroll/pinch zooms · Home fits"
        case .wire: return "Start on a pin or any wire · click empty space for corners · end on a pin or a wire (T-junction joins it in parallel) · Esc cancels"
        case .noConnect: return "Click a pin to mark it intentionally unconnected (click again to clear) · ERC stops reporting it"
        case .pan: return "Drag, scroll or arrow keys pan · pinch, ⌘-scroll or +/− zoom · Z zoom to area · Home fits"
        case .place, .placeCustom: return "Click to place (repeats) · Space or R rotates before placing · Esc returns to Select"
        case .bus: return "Click the bus corners · click the last point again or press Return to name it (D[0..7]) · Esc cancels"
        }
    }

    /// Overview of the whole schematic; click or drag to move the view.
    private var navigator: some View {
        let bounds = SchematicCanvas.componentBounds(store.sheetSnapshot)
        let extent = bounds.reduce(CGRect.null) { $0.union($1.rect) }.insetBy(dx: -40, dy: -40)
        return CanvasNavigator(extent: extent, items: bounds.map { $0.rect },
                               highlighted: bounds.filter { store.selection.contains($0.id) }.map { $0.rect },
                               viewport: viewport, canvasSize: canvasSize,
                               onCenter: { viewport.center(on: $0, in: canvasSize) },
                               onClose: { store.showNavigator = false })
    }
}

/// Sheet tabs (multi-sheet / hierarchical design), designator annotation and the assembly variant picker.
struct SheetBar: View {
    @EnvironmentObject private var store: DesignStore
    @State private var renaming: SheetInfo?
    @State private var sheetName = ""
    @State private var addingVariant = false
    @State private var variantName = ""
    @State private var repeating: SheetInfo?
    @State private var channelCount = "2"
    @State private var renamingChannel: SheetInfo?
    @State private var channelName = ""
    @State private var editingHarnesses = false
    @State private var editingNetClasses = false
    @State private var harnessConnectorType: HarnessTypeInfo?
    @State private var harnessName = ""

    var body: some View {
        OptionsBar {
            ForEach(store.snapshot.sheets) { sheet in
                let active = sheet.id == store.snapshot.activeSheet
                Button {
                    store.selectSheet(sheet.id)
                } label: {
                    // Child sheets are indented under their parent ("› Filter"); a repeated block shows its channels.
                    Text(verbatim: String(repeating: "› ", count: sheet.depth) + sheet.name
                         + (sheet.isRepeated && !sheet.isInstance ? " ×\(sheet.channels ?? sheet.instances ?? 1)" : ""))
                        .fontWeight(active ? .semibold : .regular)
                        .foregroundStyle(active ? Theme.textPrimary : Theme.textSecondary)
                        .padding(.horizontal, 8)
                        .padding(.vertical, 3)
                        .background(RoundedRectangle(cornerRadius: 5).fill(active ? Theme.blue.opacity(0.35) : Color.clear))
                }
                .buttonStyle(.plain)
                .contextMenu {
                    Button("Rename Sheet…") {
                        sheetName = sheet.name
                        renaming = sheet
                    }
                    if sheet.isRepeated || sheet.isInstance {
                        Button("Add Helper Sheet") { store.addHelperSheet(parent: sheet.id) }
                            .help("A child sheet every channel of this block gets a copy of")
                    } else {
                        Button("Add Child Sheet") { store.addSheet(parent: sheet.id) }
                    }
                    if sheet.parent != 0 && !sheet.isRepeated {
                        Toggle("Repeat With Its Block", isOn: Binding(get: { sheet.helper ?? false },
                                                                      set: { store.setHelperSheet(sheet.id, $0) }))
                    }
                    if sheet.parent != 0 {
                        Button("Place Sheet Symbol") { store.placeSheetSymbol(for: sheet.id) }
                    }
                    if !sheet.isInstance {
                        Button("Repeat Sheet…") {
                            channelCount = "\(max(2, sheet.instances ?? 1))"
                            repeating = sheet
                        }
                    }
                    if sheet.parent != 0 {
                        Menu("Sheet Symbol Size") {
                            Button("Fitted to Entries") { store.setSheetSymbolSize(sheet.id, width: 0, height: 0) }
                            ForEach([[120.0, 80.0], [160, 120], [200, 160], [280, 200], [360, 280]], id: \.self) { wh in
                                Button { store.setSheetSymbolSize(sheet.id, width: wh[0], height: wh[1]) } label: {
                                    Text(verbatim: "\(Int(wh[0])) × \(Int(wh[1]))")
                                }
                            }
                        }
                    }
                    Menu("Sheet Size") {
                        if !(sheet.size ?? "").isEmpty {
                            Button("Centre Frame on Drawing") { store.centerSheetFrame(sheet.id) }
                        }
                        Button("Auto (fits the drawing)") { store.setSheetSize(sheet.id, size: "") }
                        ForEach(EDAEngine.sheetTemplates()) { template in
                            Button(template.name + ((sheet.size ?? "") == template.name ? " ✓" : "")) {
                                store.setSheetSize(sheet.id, size: template.name)
                            }
                        }
                    }
                    if sheet.isRepeated {
                        Menu("Channel Designators") {
                            Button("By Sheet Number (R201, R301…)") { store.setInstanceRefs(sheet.id, scheme: "sheet") }
                            Button("With Channel Suffix (R1_A, R1_B…)") { store.setInstanceRefs(sheet.id, scheme: "suffix") }
                        }
                        Button("Rename Channel…") {
                            channelName = sheet.channel ?? ""
                            renamingChannel = sheet
                        }
                    }
                    if !store.selection.isEmpty && !active {
                        Button("Move Selection Here") { store.moveSelection(toSheet: sheet.id) }
                    }
                    Divider()
                    Button("Delete Sheet", role: .destructive) { store.removeSheet(sheet.id) }
                        .disabled(store.snapshot.sheets.count < 2)
                }
            }
            Button { store.addSheet() } label: { Image(systemName: "plus") }
                .buttonStyle(.borderless)
                .help("Add a sheet (right-click a tab for more)")
            Spacer()
            Menu {
                Button("Number by Rows") { store.annotate() }
                Button("Number by Columns") { store.annotate(byColumns: true) }
                Button("Number by Sheet (R101, R201…)") { store.annotate(sheetNumbering: true) }
                Button("Fix Duplicates Only") { store.annotate(keepExisting: true) }
                Divider()
                Button("Pack Units into Packages") { store.annotate(packUnits: true) }
                    .help("Gates of multi-unit parts (A, B, C, D of a quad op-amp) fill packages in placement order before numbering")
            } label: {
                Label("Annotate", systemImage: "number")
            }
            .fixedSize()
            Button { editingNetClasses = true } label: { Label("Net Classes", systemImage: "ruler") }
                .help("Net classes for the directives on nets: the schematic is the source of the board's net rules")
            Menu {
                Button("Harness Types…") { editingHarnesses = true }
                Divider()
                ForEach(store.snapshot.harnessTypes) { type in
                    Button("Place \(type.name) Connector…") {
                        harnessName = type.name + "1"
                        harnessConnectorType = type
                    }
                }
            } label: {
                Label("Harnesses", systemImage: "cable.connector.horizontal")
            }
            .fixedSize()
            .help("Signal harnesses: named bundles of signals carried across sheets as one")
            Menu {
                Button("Base Design") { store.selectVariant("") }
                ForEach(store.snapshot.variants) { variant in
                    Button(variant.name) { store.selectVariant(variant.name) }
                }
                Divider()
                Button("New Variant…") {
                    variantName = ""
                    addingVariant = true
                }
                if !store.snapshot.activeVariant.isEmpty {
                    Button("Delete Variant", role: .destructive) { store.removeVariant(store.snapshot.activeVariant) }
                }
            } label: {
                if store.snapshot.activeVariant.isEmpty {
                    Label("Base Design", systemImage: "square.stack.3d.up")
                } else {
                    Label(store.snapshot.activeVariant, systemImage: "square.stack.3d.up")
                }
            }
            .fixedSize()
            .help("Assembly variant for the BOM, CPL and assembly drawings")
        }
        .alert("Rename Sheet", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("Sheet name", text: $sheetName)
            Button("OK") {
                if let sheet = renaming { store.renameSheet(sheet.id, to: sheetName) }
                renaming = nil
            }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
        .alert("Repeat Sheet", isPresented: Binding(get: { repeating != nil }, set: { if !$0 { repeating = nil } })) {
            TextField("Channels", text: $channelCount)
            Button("OK") {
                if let sheet = repeating, let count = Int(channelCount.trimmingCharacters(in: .whitespaces)) {
                    store.repeatSheet(sheet.id, count: count)
                }
                repeating = nil
            }
            Button("Cancel", role: .cancel) { repeating = nil }
        } message: {
            Text("Use this sheet as several identical channels (1 to 64). Each channel gets its own designators, nets and sheet symbol; editing any channel edits them all.")
        }
        .alert("Rename Channel", isPresented: Binding(get: { renamingChannel != nil }, set: { if !$0 { renamingChannel = nil } })) {
            TextField("Channel label", text: $channelName)
            Button("OK") {
                if let sheet = renamingChannel { store.setSheetChannel(sheet.id, to: channelName) }
                renamingChannel = nil
            }
            Button("Cancel", role: .cancel) { renamingChannel = nil }
        }
        .sheet(isPresented: $editingHarnesses) { HarnessTypesView().environmentObject(store) }
        .sheet(isPresented: $editingNetClasses) { NetClassesView().environmentObject(store) }
        .alert("Place Harness Connector", isPresented: Binding(get: { harnessConnectorType != nil },
                                                               set: { if !$0 { harnessConnectorType = nil } })) {
            TextField("Harness name", text: $harnessName)
            Button("OK") {
                if let type = harnessConnectorType { store.placeHarnessConnector(type: type.name, name: harnessName) }
                harnessConnectorType = nil
            }
            Button("Cancel", role: .cancel) { harnessConnectorType = nil }
        } message: {
            Text("A harness label with one entry per member; wire each entry to its signal. Give a port or sheet entry the same name and type to carry the bundle to another sheet.")
        }
        .alert("New Variant", isPresented: $addingVariant) {
            TextField("Variant name", text: $variantName)
            Button("OK") { store.addVariant(named: variantName) }
            Button("Cancel", role: .cancel) {}
        }
    }
}

/// Proteus-style device picker: searchable list with a live symbol preview.
struct DevicePicker: View {
    @Binding var selected: ComponentKind
    var customParts: [CustomPartInfo] = []
    /// Whether the highlighted built-in device is armed for placement (only then is its row highlighted); clicking a
    /// row always arms it.
    var isPlacing = true
    var onPickCustom: (String) -> Void = { _ in }
    /// Adds a built-in standard part to the project library; returns its part id.
    var onPickStandard: (StandardPart) -> String? = { _ in nil }
    var onImport: () -> Void = {}
    var onPick: (ComponentKind) -> Void
    @State private var search = ""
    @State private var selectedCustom: String?
    /// Icons only: a narrow column of device icons, each named in its tooltip (the palette's compact mode).
    @AppStorage(DevicePicker.iconsOnlyKey) private var iconsOnly = false

    static let iconsOnlyKey = "schematic.devicePaletteIconsOnly"
    static let iconsOnlyWidth: CGFloat = 52

    private var filtered: [ComponentKind] {
        let q = search.lowercased()
        return ComponentKind.builtIn.filter { q.isEmpty || $0.displayName.lowercased().contains(q) || $0.planName.contains(q) }
    }

    /// Parametric search (`PartQuery`): words, or filters such as `cat:sensors pkg:soic pins:8 mfr:ti`.
    private var filteredCustom: [CustomPartInfo] {
        let query = PartQuery(search)
        return customParts.filter { query.isEmpty || query.matches($0) }
    }

    /// Standard parts not yet in the project library (those already added show under Custom Parts).
    private var filteredStandard: [StandardPart] {
        let query = PartQuery(search)
        let inLibrary = Set(customParts.map(\.name))
        return StandardLibrary.parts.filter { part in
            !inLibrary.contains(part.spec.name) && (query.isEmpty || query.matches(part))
        }
    }

    private func header(_ title: String) -> some View {
        Text(title)
            .font(.caption.weight(.semibold))
            .foregroundStyle(Theme.textMuted)
            .padding(.horizontal, 8)
            .padding(.top, 10)
            .padding(.bottom, 3)
    }

    /// A full-width clickable row with the selection highlight.
    private func row<RowLabel: View>(highlighted: Bool, action: @escaping () -> Void,
                                     @ViewBuilder label: () -> RowLabel) -> some View {
        Button(action: action) {
            label()
                .frame(maxWidth: .infinity, alignment: .leading)
                .padding(.horizontal, 8)
                .padding(.vertical, 3)
                .background(RoundedRectangle(cornerRadius: 5).fill(highlighted ? Theme.blue.opacity(0.3) : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    var body: some View {
        Group {
            if iconsOnly { iconsBody } else { fullBody }
        }
        .background(Theme.deepBlue.opacity(0.75))
        .overlay(Rectangle().frame(width: 1).foregroundStyle(Theme.blue.opacity(0.3)), alignment: .trailing)
    }

    private var layoutToggle: some View {
        Button { iconsOnly.toggle() } label: {
            Image(systemName: iconsOnly ? "sidebar.squares.left" : "square.grid.2x2")
                .font(.caption)
        }
        .buttonStyle(.plain)
        .foregroundStyle(Theme.textMuted)
        .help(LocalizedStringKey(iconsOnly ? "Show device names" : "Show icons only (names in tooltips)"))
        .accessibilityLabel(Text(LocalizedStringKey(iconsOnly ? "Show device names" : "Show icons only")))
    }

    /// One device icon with its name in the tooltip.
    private func iconButton<Icon: View>(highlighted: Bool, help: String, action: @escaping () -> Void,
                                        @ViewBuilder icon: () -> Icon) -> some View {
        Button(action: action) {
            icon()
                .frame(width: 36, height: 28)
                .background(RoundedRectangle(cornerRadius: 5).fill(highlighted ? Theme.blue.opacity(0.3) : Color.clear))
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }

    /// Icons only: built-in devices by category (a thin rule between categories), then the project's custom parts.
    private var iconsBody: some View {
        VStack(spacing: 0) {
            layoutToggle
                .padding(.top, 10)
                .padding(.bottom, 6)
            ScrollView(.vertical, showsIndicators: false) {
                LazyVStack(spacing: 2) {
                    ForEach(ComponentKind.pickerCategories, id: \.self) { category in
                        iconSection(category)
                    }
                    if !customParts.isEmpty {
                        Divider().padding(.vertical, 4)
                        ForEach(customParts) { part in
                            iconButton(highlighted: selectedCustom == part.id, help: part.name) {
                                selectedCustom = part.id
                                onPickCustom(part.id)
                            } icon: { ComponentSymbolImage(kind: .custom) }
                        }
                    }
                    Divider().padding(.vertical, 4)
                    iconButton(highlighted: false, help: String(localized: "Import Datasheet…"), action: onImport) {
                        Image(systemName: "doc.viewfinder").foregroundStyle(Theme.lightBlue)
                    }
                }
                .padding(.bottom, 6)
            }
        }
        .frame(maxWidth: .infinity)
    }

    @ViewBuilder
    private func iconSection(_ category: String) -> some View {
        let items = ComponentKind.builtIn.filter { $0.category == category }
        if !items.isEmpty {
            Divider().padding(.vertical, 4).help(category)
            ForEach(items) { kind in
                iconButton(highlighted: selectedCustom == nil && isPlacing && selected == kind,
                           help: kind.displayName + " — " + kind.valueHint) {
                    selectedCustom = nil
                    onPick(kind)
                } icon: { ComponentSymbolImage(kind: kind) }
            }
        }
    }

    private var fullBody: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("DEVICES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                layoutToggle
                Text("P").font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
            }
            .padding(.horizontal, 10)
            .padding(.top, 10)
            TextField("Search devices", text: $search)
                .textFieldStyle(.blue)
                .help("Words, or filters: cat:sensors pkg:soic pins:8 mfr:ti")
                .padding(8)
            // A plain lazy scroll view, not a `List` (an AppKit table): with the full parts catalog the table was
            // updated re-entrantly while a design loaded, which AppKit warns will become an assert.
            ScrollView(.vertical) {
                LazyVStack(alignment: .leading, spacing: 1) {
                    // Basic components first: power sources, passives, semiconductors, nets, switches/connectors.
                    ForEach(ComponentKind.pickerCategories, id: \.self) { category in
                        let items = filtered.filter { $0.category == category }
                        if !items.isEmpty {
                            header(category)
                            ForEach(items) { kind in
                                row(highlighted: selectedCustom == nil && isPlacing && selected == kind) {
                                    selectedCustom = nil
                                    onPick(kind)
                                } label: {
                                    Label { Text(kind.displayName) } icon: { ComponentSymbolImage(kind: kind) }
                                        .foregroundStyle(Theme.textPrimary)
                                }
                            }
                        }
                    }
                    header("Custom Parts")
                    ForEach(filteredCustom) { part in
                        row(highlighted: selectedCustom == part.id) {
                            selectedCustom = part.id
                            onPickCustom(part.id)
                        } label: {
                            HStack {
                                ComponentSymbolImage(kind: .custom)
                                VStack(alignment: .leading, spacing: 1) {
                                    Text(part.name).foregroundStyle(Theme.textPrimary)
                                    Text("\(part.footprint) · \(part.pins.count) pins").font(.caption2).foregroundStyle(Theme.textMuted)
                                }
                            }
                        }
                    }
                    Button(action: onImport) {
                        Label("Import Datasheet…", systemImage: "doc.viewfinder")
                    }
                    .buttonStyle(.borderless)
                    .foregroundStyle(Theme.lightBlue)
                    .padding(.horizontal, 8)
                    .padding(.vertical, 4)
                    if !filteredStandard.isEmpty {
                        header("Standard Parts")
                        ForEach(filteredStandard) { part in
                            row(highlighted: false) {
                                if let id = onPickStandard(part) { selectedCustom = id }
                            } label: {
                                HStack {
                                    ComponentSymbolImage(kind: .ic8)
                                    VStack(alignment: .leading, spacing: 1) {
                                        Text(part.spec.name).foregroundStyle(Theme.textPrimary)
                                        Text("\(part.category) · \(part.packageSummary)").font(.caption2).foregroundStyle(Theme.textMuted)
                                    }
                                }
                            }
                            .help(part.spec.description)
                        }
                    }
                }
                .padding(.horizontal, 6)
                .padding(.bottom, 6)
            }

            if let id = selectedCustom, let part = customParts.first(where: { $0.id == id }) {
                VStack(alignment: .leading, spacing: 6) {
                    SymbolPreview(kind: .custom, value: part.name, custom: part, showPinLabels: true)
                        .frame(height: 120)
                        .frame(maxWidth: .infinity)
                        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy))
                    Text(part.name).font(.headline).foregroundStyle(Theme.textPrimary)
                    Text("\(part.manufacturer.isEmpty ? "" : part.manufacturer + " · ")\(part.footprint)")
                        .font(.caption).foregroundStyle(Theme.lightBlue)
                    Text(part.description).font(.caption).foregroundStyle(Theme.textMuted)
                }
                .padding(10)
            } else {
            VStack(alignment: .leading, spacing: 6) {
                SymbolPreview(kind: selected, value: selected.defaultValue)
                    .frame(height: 90)
                    .frame(maxWidth: .infinity)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy))
                Text(selected.displayName).font(.headline).foregroundStyle(Theme.textPrimary)
                Text("Pins: " + selected.pinNames.joined(separator: ", "))
                    .font(.caption.monospaced()).foregroundStyle(Theme.lightBlue)
                Text(selected.valueHint).font(.caption).foregroundStyle(Theme.textMuted)
            }
            .padding(10)
            }
        }
    }
}

/// Proteus-style simulation transport (bottom-left of the schematic).
struct SimulationTransport: View {
    @EnvironmentObject private var store: DesignStore
    @ObservedObject var live: LiveSimulation

    var body: some View {
        HStack(spacing: 6) {
            if live.isRunning {
                // Live board: time, speed, pause / stop.
                Circle().fill(live.isPaused ? Theme.warning : Theme.liveOn).frame(width: 7, height: 7)
                Text("LIVE \(EngineeringFormat.string(live.state?.time ?? 0, unit: "s", digits: 3))")
                    .font(.caption.monospacedDigit().weight(.semibold))
                    .foregroundStyle(Theme.textPrimary)
                Button {
                    if live.isPaused { live.resume() } else { live.pause() }
                } label: {
                    Image(systemName: live.isPaused ? "play.fill" : "pause.fill")
                }
                .help(live.isPaused ? "Resume" : "Pause")
                .accessibilityLabel(live.isPaused ? "Resume live simulation" : "Pause live simulation")
                Button { live.stop() } label: { Image(systemName: "stop.fill") }
                    .help("Stop the live simulation")
                    .accessibilityLabel("Stop live simulation")
                Button { store.workspace = .simulation } label: { Image(systemName: "waveform.path.ecg.rectangle") }
                    .help("Scope, serial monitor and switches")
                    .accessibilityLabel("Open live instruments")
            } else {
                // ▶ is the real-time run (like powering the board); DC is a one-shot operating point.
                Button {
                    live.start(store: store)
                } label: {
                    Label("Run", systemImage: "play.fill")
                }
                .keyboardShortcut("r", modifiers: [.command])
                .help("Run the board in real time (⌘R): LEDs light, click switches, firmware runs, live probes")
                .accessibilityLabel("Run live simulation")
                Divider().frame(height: 14)
                Button {
                    Task { await store.simulateDC() }
                } label: {
                    Text("DC").font(.caption.weight(.semibold))
                }
                .help("Solve the DC operating point once — shows voltage probes")
                .accessibilityLabel("Run DC operating point")
                Button {
                    store.workspace = .simulation
                } label: {
                    Image(systemName: "waveform")
                }
                .help("Transient analysis")
                .accessibilityLabel("Open transient analysis")
                Button {
                    store.dcResult = nil
                } label: {
                    Image(systemName: "xmark.circle")
                }
                .help("Clear simulation results")
                .accessibilityLabel("Clear simulation results")
                .disabled(store.dcResult == nil)
                if let dc = store.dcResult {
                    Text(dc.converged ? "DC ✓" : "DC ✗")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(dc.converged ? Theme.skyBlue : Theme.error)
                    if !dc.variant.isEmpty || !dc.omitted.isEmpty {
                        // The simulated assembly: the active variant, with its unfitted parts left out.
                        (dc.variant.isEmpty ? Text("Base Design") : Text(verbatim: dc.variant))
                            .font(.caption)
                            .foregroundStyle(Theme.warning)
                            .help("Simulated as assembled: variant values applied, parts not fitted left out (\(dc.omitted.joined(separator: ", ")))")
                    }
                }
            }
        }
        .buttonStyle(.borderless)
        .foregroundStyle(Theme.skyBlue)
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
        .overlay(Capsule().strokeBorder(live.isRunning ? Theme.liveOn.opacity(0.8) : Theme.blue.opacity(0.5)))
        .alert("Live simulation", isPresented: Binding(get: { live.error != nil && !live.isRunning },
                                                        set: { if !$0 { live.clearError() } })) {
            Button("OK") { live.clearError() }
        } message: {
            Text(live.error ?? "")
        }
    }
}
