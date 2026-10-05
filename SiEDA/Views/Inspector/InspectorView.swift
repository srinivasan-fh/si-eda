import SwiftUI

/// Altium-style Properties panel.
struct InspectorView: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("PROPERTIES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                let selected = store.selectedComponents
                if selected.count == 1, let c = selected.first, c.componentKind == .junction {
                    JunctionProperties(junction: c)
                } else if selected.count == 1, let c = selected.first {
                    ComponentProperties(component: c)
                        .id("\(c.id)|\(c.ref)|\(c.value)|\(store.snapshot.activeVariant)|\(c.variantValue ?? "")|\(c.blockValue ?? "")")
                } else if selected.count > 1 {
                    MultiSelectionProperties(count: selected.count)
                } else if let busId = store.selectedBus, let bus = store.sheetSnapshot.bus(busId) {
                    BusProperties(bus: bus)
                        .id("\(bus.id)|\(bus.name)")
                } else if let wireId = store.selectedWire, let wire = store.snapshot.wires.first(where: { $0.id == wireId }) {
                    WireProperties(wire: wire)
                } else {
                    ProjectProperties()
                        .id(store.snapshot.name)
                }
            }
            .padding(14)
        }
        .background(Theme.deepBlue.opacity(0.65))
    }
}

private struct PropertyGroup<Content: View>: View {
    var title: String
    @ViewBuilder var content: Content

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(title).font(.caption.weight(.semibold)).foregroundStyle(Theme.lightBlue)
            content
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .bluePanel()
    }
}

private struct PropertyRow: View {
    var label: String
    var value: String

    var body: some View {
        HStack {
            Text(label).foregroundStyle(Theme.textMuted)
            Spacer()
            Text(value).foregroundStyle(Theme.textPrimary).font(.callout.monospacedDigit()).textSelection(.enabled)
        }
        .font(.callout)
    }
}

private struct ComponentProperties: View {
    @EnvironmentObject private var store: DesignStore
    let component: SnapComponent
    @State private var ref: String
    @State private var value: String
    @State private var variantValue: String
    @State private var channelValue: String
    @FocusState private var focus: Field?

    private enum Field { case ref, value }

    init(component: SnapComponent) {
        self.component = component
        _ref = State(initialValue: component.logicalRef ?? component.ref)
        _value = State(initialValue: component.blockValue ?? component.value)
        _variantValue = State(initialValue: component.variantValue ?? "")
        _channelValue = State(initialValue: component.value)
    }

    var body: some View {
        let kind = component.componentKind
        VStack(alignment: .leading, spacing: 12) {
            HStack(spacing: 10) {
                Image(systemName: kind.systemImage)
                    .font(.title2)
                    .foregroundStyle(Theme.skyBlue)
                    .frame(width: 40, height: 40)
                    .background(RoundedRectangle(cornerRadius: 8).fill(Theme.blue.opacity(0.2)))
                VStack(alignment: .leading) {
                    Text(component.ref).font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                    Text(kind.displayName).font(.caption).foregroundStyle(Theme.textMuted)
                }
            }

            PropertyGroup(title: "General") {
                if !kind.isVirtual {
                    LabeledContent(component.logicalRef == nil ? "Designator" : "Block designator") {
                        TextField("Designator", text: $ref)
                            .textFieldStyle(.blue)
                            .focused($focus, equals: .ref)
                            .onSubmit { commitRef() }
                    }
                    if component.logicalRef != nil {
                        // A part of a repeated sheet: its designator in this channel follows the block designator.
                        PropertyRow(label: "Channel designator", value: component.ref)
                    }
                }
                LabeledContent(kind == .netLabel ? "Net name" : "Value") {
                    TextField("Value", text: $value)
                        .textFieldStyle(.blue)
                        .focused($focus, equals: .value)
                        .onSubmit { commitValue() }
                }
                Text(kind.valueHint).font(.caption).foregroundStyle(Theme.textMuted)
                if component.logicalRef != nil && !kind.isVirtual {
                    // Per-channel parameter: this channel's own value; the Value field above sets the whole block.
                    LabeledContent("Channel value") {
                        TextField(component.blockValue ?? component.value, text: $channelValue)
                            .textFieldStyle(.blue)
                            .onSubmit { store.setChannelValue(component.id, channelValue) }
                    }
                    .help("Value of this channel only; the other channels keep theirs")
                    if let block = component.blockValue, component.channelOverride != nil {
                        HStack {
                            Text("Block value: \(block)").font(.caption).foregroundStyle(Theme.textMuted)
                            Spacer()
                            Button("Use Block Value") { store.setChannelValue(component.id, "") }
                                .controlSize(.small)
                        }
                    }
                    ChannelParameterRows(component: component)
                }
                HStack {
                    Button { store.rotateSelection() } label: { Label("Rotate", systemImage: "rotate.right") }
                    Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete", systemImage: "trash") }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                // Cross-probing both ways between the schematic and the board.
                if !kind.isVirtual || component.unitOf != nil {
                    if store.workspace == .pcb {
                        Button { store.showInSchematic(component.id) } label: {
                            Label("Show in Schematic", systemImage: "point.3.connected.trianglepath.dotted")
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    } else {
                        Button { store.showOnPCB(component.id) } label: {
                            Label("Show on PCB", systemImage: "square.grid.3x3.square")
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                        .disabled(!(store.snapshot.component(component.unitOf ?? component.id)?.pcb.placed ?? false))
                    }
                }
            }

            if kind == .netLabel && !component.isHarnessLabel {
                // Net directive (net class, differential pair, parameter set) on the label's net.
                let anchor = PinAddress(component: component.instanceOf ?? component.id, pin: 0)
                let existing = store.directive(on: anchor)
                NetDirectiveEditor(anchor: anchor, directive: existing)
                    .id("directive|\(anchor.component)|\(existing?.id ?? 0)|\(existing?.summary ?? "")")
            }

            let series = ESeries.preferred(for: kind)
            if let options = component.packageOptions, options.count > 1 {
                PropertyGroup(title: "Package") {
                    Picker("Package", selection: Binding(get: { component.footprint },
                                                         set: { store.setPackage(component.id, $0) })) {
                        ForEach(options) { Text($0.label).tag($0.id) }
                    }
                    .labelsHidden()
                    .help("Footprint the part is fitted in: chip size, through-hole, tantalum or electrolytic case")
                    .accessibilityLabel("Package of \(component.ref)")
                }
            }
            if !series.isEmpty, let numeric = EDAEngine.parseValue(component.value), numeric > 0 {
                PropertyGroup(title: "Standard Values (IEC 60063)") {
                    ForEach(series) { s in
                        let isStandard = s.contains(numeric)
                        HStack {
                            Image(systemName: isStandard ? "checkmark.circle.fill" : "circle.dashed")
                                .foregroundStyle(isStandard ? Theme.skyBlue : Theme.textMuted)
                            Text("\(s.title) \(s.tolerance)").foregroundStyle(Theme.textSecondary)
                            Spacer()
                            if isStandard {
                                Text("standard").font(.caption).foregroundStyle(Theme.textMuted)
                            } else {
                                Button("Use \(EngineeringFormat.string(s.nearest(numeric), unit: kind.valueUnit))") {
                                    store.snapToStandardValue(component.id, series: s)
                                }
                                .buttonStyle(.bordered)
                                .controlSize(.small)
                                .help("Replace the value with the nearest \(s.title) value")
                            }
                        }
                        .font(.callout)
                    }
                }
            }

            if let part = store.snapshot.customPart(for: component) {
                PropertyGroup(title: "Library Part") {
                    PropertyRow(label: "Part", value: part.name)
                    if !part.manufacturer.isEmpty { PropertyRow(label: "Manufacturer", value: part.manufacturer) }
                    PropertyRow(label: "Package", value: part.footprint)
                    if let model = part.model, !model.summary.isEmpty {
                        PropertyRow(label: "Sim model", value: model.summary)
                    }
                    if !part.description.isEmpty {
                        Text(part.description).font(.caption).foregroundStyle(Theme.textSecondary)
                    }
                    if !part.datasheet.isEmpty {
                        Label(part.datasheet, systemImage: "doc.text").font(.caption).foregroundStyle(Theme.textMuted)
                    }
                    Button {
                        store.libraryFocusPartId = part.id
                        store.workspace = .library
                    } label: { Label("Open in Library", systemImage: "books.vertical") }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                }
            }

            PropertyGroup(title: "Schematic") {
                PropertyRow(label: "Position", value: String(format: "%.0f, %.0f", component.x, component.y))
                PropertyRow(label: "Rotation", value: "\(component.rotation)°")
                if store.snapshot.sheets.count > 1 {
                    if component.labelScope == "entry" {
                        PropertyRow(label: "Sheet entry", value: store.snapshot.sheet(component.targetSheet ?? 0)?.name ?? "—")
                    } else {
                        // Moves the selection (wires to parts left behind are removed).
                        Picker("Sheet", selection: Binding(get: { component.sheetId },
                                                           set: { store.moveSelection(toSheet: $0) })) {
                            ForEach(store.snapshot.sheets) { Text(verbatim: $0.name).tag($0.id) }
                        }
                    }
                }
                if kind == .netLabel && (component.labelScope == "entry" || component.labelScope == "port") {
                    // Cross-probe through the hierarchy: an entry to its child sheet's port, a port to its entry.
                    Button {
                        store.crossProbeHierarchy(from: component.id)
                    } label: {
                        Label(component.labelScope == "entry" ? "Go to Port" : "Go to Sheet Entry", systemImage: "arrow.up.right.square")
                    }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
                }
                if kind == .netLabel, let net = component.pins.first?.net, net >= 0 {
                    NetNavigatorView(net: net)
                }
                if kind == .netLabel && component.labelScope != "entry" {
                    Picker("Scope", selection: Binding(get: { component.labelScope },
                                                       set: { store.setLabelScope(component.id, scope: $0) })) {
                        Text("Global").tag("global")
                        Text("Local (this sheet)").tag("local")
                        Text("Port (to parent sheet)").tag("port")
                    }
                }
                if kind == .netLabel && component.harnessOf == nil
                    && (!store.snapshot.harnessTypes.isEmpty || component.harnessType != nil) {
                    Picker("Harness", selection: Binding(get: { component.harnessType ?? "" },
                                                         set: { store.setLabelHarness(component.id, type: $0) })) {
                        Text("None (single signal)").tag("")
                        ForEach(store.snapshot.harnessTypes) { Text(verbatim: $0.name).tag($0.name) }
                    }
                    .help("A harness label carries every member of its type across sheets (port, sheet entry or global)")
                    if component.isHarnessLabel {
                        if let type = store.snapshot.harnessTypes.first(where: { $0.name == component.harnessType }) {
                            PropertyRow(label: "Members", value: type.entries.map { component.value + "." + $0 }.joined(separator: ", "))
                        }
                        Button("Add Missing Entries") { store.placeHarnessEntries(component.id) }
                            .buttonStyle(.bordered)
                            .controlSize(.small)
                    }
                }
                if let owner = component.harnessOf, let harness = store.snapshot.component(owner) {
                    PropertyRow(label: "Harness member", value: harness.value + "." + component.value)
                }
            }

            if !kind.isVirtual && !store.snapshot.activeVariant.isEmpty {
                PropertyGroup(title: "Variant \(store.snapshot.activeVariant)") {
                    Toggle("Fitted", isOn: Binding(get: { component.isFitted },
                                                   set: { store.setFittedInVariant(component.id, $0) }))
                    LabeledContent("Value") {
                        TextField(component.value, text: $variantValue)
                            .textFieldStyle(.blue)
                            .onSubmit { store.setVariantValue(component.id, variantValue) }
                    }
                }
            }

            if let unitName = component.unitName, let package = component.unitOf {
                // A gate of a multi-unit part: its package carries the footprint; the other units are placed here.
                PropertyGroup(title: "Unit \(unitName) of \(component.ref)") {
                    let placed = store.snapshot.components.filter { $0.unitOf == package }.compactMap(\.unitName)
                    PropertyRow(label: "Units placed", value: placed.joined(separator: " "))
                    if let part = store.snapshot.customPart(component.customPart), let symbols = part.unitSymbols {
                        let missing = symbols.indices.filter { !placed.contains(symbols[$0].name) }
                        if !missing.isEmpty {
                            Button { store.placeNextUnit(of: component.id) } label: {
                                Label("Place Next Unit", systemImage: "plus.square.on.square")
                            }
                            Menu {
                                ForEach(missing, id: \.self) { index in
                                    Button(symbols[index].name) { store.placeUnit(index + 1, of: component.id) }
                                }
                            } label: {
                                Label("Place Unit", systemImage: "square.grid.2x2")
                            }
                            .fixedSize()
                        }
                        // Pin swap: interchangeable pins of this gate (the unit's pin-swap groups).
                        if let unit = component.unit, unit >= 1, unit <= symbols.count, let groups = symbols[unit - 1].pinSwap,
                           !groups.isEmpty {
                            let pins = symbols[unit - 1].symbol.pins
                            Menu {
                                ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
                                    ForEach(Self.pairs(group), id: \.self) { pair in
                                        if let a = pins.firstIndex(where: { $0.number == pair[0] }),
                                           let b = pins.firstIndex(where: { $0.number == pair[1] }) {
                                            Button(String(pins[a].name + " ↔ " + pins[b].name)) {
                                                store.swapPins(of: component.id, a, b)
                                            }
                                        }
                                    }
                                }
                            } label: {
                                Label("Swap Pins", systemImage: "arrow.left.arrow.right")
                            }
                            .fixedSize()
                            .help("Exchange the wires of two interchangeable pins of this gate")
                        }
                    }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }

            if !kind.isVirtual && component.unitOf == nil {
                PropertyGroup(title: "PCB Footprint") {
                    PropertyRow(label: "Footprint", value: component.footprint)
                    if component.pcb.placed {
                        PropertyRow(label: "Position", value: String(format: "%.2f, %.2f mm", component.pcb.x, component.pcb.y))
                        PropertyRow(label: "Rotation", value: "\(component.pcb.rotation)°")
                        PropertyRow(label: "Layer", value: component.pcb.bottom ? "Bottom" : "Top")
                        if kind == .resistor || kind == .capacitor, store.snapshot.board.layerCount >= 4 {
                            // Embedded passives: thin-film resistor foil / buried capacitance inside the stack-up.
                            let inner = Array(1..<(store.snapshot.board.layerCount - (kind == .capacitor ? 2 : 1)))
                            Picker("Mounting", selection: Binding(get: { component.pcb.embeddedLayer ?? 0 },
                                                                  set: { store.setEmbedded(layer: $0) })) {
                                Text("Surface").tag(0)
                                ForEach(inner, id: \.self) { layer in
                                    Text(kind == .capacitor ? "Embedded L\(layer + 1)–L\(layer + 2)" : "Embedded L\(layer + 1)")
                                        .tag(layer)
                                }
                            }
                            .font(.caption)
                            .help(kind == .resistor ? "Thin-film resistive foil (25–250 Ω/sq) inside the board: no assembly, shortest loop"
                                                    : "Buried-capacitance laminate between two inner layers (practical up to a few nF)")
                        }
                        Toggle("Locked (Auto Place keeps it)", isOn: Binding(
                            get: { component.pcb.locked ?? false }, set: { store.setFootprintsLocked($0) }))
                            .font(.caption)
                        HStack {
                            Button { store.rotateFootprints() } label: { Label("Rotate", systemImage: "rotate.right") }
                            Button { store.flipFootprints() } label: { Label("Flip", systemImage: "arrow.left.and.right") }
                        }
                        .buttonStyle(.bordered)
                        .controlSize(.small)
                    } else {
                        Text("Not placed yet").font(.caption).foregroundStyle(Theme.textMuted)
                    }
                }
            }

            PropertyGroup(title: "Pins") {
                ForEach(Array(component.pins.enumerated()), id: \.offset) { index, pin in
                    HStack {
                        Text(pin.name).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary).frame(width: 40, alignment: .leading)
                        Text(pin.noConnect && !pin.connected ? "no connect" : (store.snapshot.net(pin.net)?.name ?? "—"))
                            .font(.callout.monospaced())
                            .foregroundStyle(pin.connected ? Theme.lightBlue : (pin.noConnect ? Theme.textMuted : Theme.warning))
                        Spacer()
                        if let v = store.dcResult?.voltage(net: pin.net), pin.connected {
                            Text(EngineeringFormat.string(v, unit: "V")).font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                        }
                        if !kind.isVirtual, !pin.connected || pin.noConnect {
                            Button {
                                store.toggleNoConnect(PinAddress(component: component.id, pin: index))
                            } label: {
                                Image(systemName: pin.noConnect ? "xmark.circle.fill" : "xmark.circle")
                            }
                            .buttonStyle(.borderless)
                            .help(pin.noConnect ? "Clear the no-connect mark" : "Mark as intentionally unconnected (Q)")
                            .accessibilityLabel(pin.noConnect ? "Clear no-connect on \(pin.name)" : "Mark \(pin.name) no-connect")
                        }
                    }
                }
            }

            if let mcu = component.mcu {
                FirmwareProperties(component: component, mcu: mcu)
            }

            if DesignStore.acceptsSpiceModel(component) {
                SpiceModelProperties(component: component)
            }

            if let reading = store.dcResult?.reading(component: component.id) {
                PropertyGroup(title: "Operating Point") {
                    PropertyRow(label: kind == .npn ? "I_C" : (kind == .nmos ? "I_D" : "Current"),
                                value: EngineeringFormat.string(reading.current, unit: "A", digits: 4))
                    PropertyRow(label: kind == .npn ? "V_CE" : (kind == .nmos ? "V_DS" : "Voltage"),
                                value: EngineeringFormat.string(reading.voltage, unit: "V", digits: 4))
                    PropertyRow(label: "Power", value: EngineeringFormat.string(reading.power, unit: "W", digits: 3))
                    if let state = reading.stateTitle {
                        PropertyRow(label: "State", value: state)
                    }
                }
            }
        }
        // Edits are kept when the field loses focus or the selection changes, not only on Return.
        .onChange(of: focus) { old, _ in
            if old == .ref { commitRef() }
            if old == .value { commitValue() }
        }
        // A pending edit is kept when the selection changes, committed on the next run-loop turn (not during the
        // view update that removed this panel, which would mutate the store while the sidebar table reloads).
        .onDisappear {
            let commit = self
            DispatchQueue.main.async {
                commit.commitRef()
                commit.commitValue()
            }
        }
    }

    private func commitRef() {
        let trimmed = ref.trimmingCharacters(in: .whitespaces)
        let current = component.logicalRef ?? component.ref
        if trimmed.isEmpty || trimmed == current { ref = current; return }
        store.setRef(component.id, trimmed)
    }

    /// Every pair of a pin-swap group, in order.
    static func pairs(_ group: [String]) -> [[String]] {
        var out: [[String]] = []
        for i in group.indices {
            for j in group.indices where j > i { out.append([group[i], group[j]]) }
        }
        return out
    }

    private func commitValue() {
        if value != (component.blockValue ?? component.value) { store.setValue(component.id, value) }
    }
}

/// Microcontroller firmware: upload an Intel HEX file or pick a built-in example, set the clock; the simulator runs it.
private struct FirmwareProperties: View {
    @EnvironmentObject private var store: DesignStore
    var component: SnapComponent
    var mcu: McuInfo

    private static let clocks: [Double] = [1e6, 8e6, 12e6, 16e6, 20e6]

    var body: some View {
        PropertyGroup(title: "Firmware (\(mcu.model))") {
            if mcu.hasFirmware {
                HStack(spacing: 6) {
                    Image(systemName: "memorychip").foregroundStyle(Theme.skyBlue)
                    Text(mcu.firmwareName.isEmpty ? "firmware" : mcu.firmwareName)
                        .foregroundStyle(Theme.textPrimary)
                        .lineLimit(1)
                        .truncationMode(.middle)
                }
                PropertyRow(label: "Size", value: "\(mcu.firmwareBytes) bytes")
            } else {
                Text(mcu.firmwareError.isEmpty
                     ? "No firmware: the pins stay high-impedance inputs. Upload a .hex (Arduino IDE ▸ Sketch ▸ Export Compiled Binary) or pick an example."
                     : "Firmware error: \(mcu.firmwareError)")
                    .font(.caption)
                    .foregroundStyle(mcu.firmwareError.isEmpty ? Theme.textMuted : Theme.error)
                    .fixedSize(horizontal: false, vertical: true)
            }
            Picker("Clock", selection: Binding(get: { mcu.clockHz }, set: { store.setMcuClock(component.id, clockHz: $0) })) {
                ForEach(Self.clocks, id: \.self) { hz in Text(String(format: "%g MHz", hz / 1e6)).tag(hz) }
                if !Self.clocks.contains(mcu.clockHz) {
                    Text(String(format: "%g MHz", mcu.clockHz / 1e6)).tag(mcu.clockHz)
                }
            }
            .help("CPU clock the firmware was built for (Arduino Uno/Nano: 16 MHz)")
            Menu {
                ForEach(EDAEngine.firmwareExamples.filter { $0.model == mcu.model }) { example in
                    Button(example.name) { store.loadFirmwareExample(example, into: component.id) }
                        .help(example.description)
                }
            } label: {
                Label("Load Example Firmware", systemImage: "list.bullet")
            }
            .fixedSize()
            HStack {
                Button { store.uploadFirmware(to: component.id) } label: { Label("Upload .hex…", systemImage: "square.and.arrow.down") }
                if mcu.hasFirmware {
                    Button(role: .destructive) {
                        store.setFirmware(component.id, hex: "", name: "")
                    } label: { Image(systemName: "trash") }
                    .help("Remove the firmware")
                    .accessibilityLabel("Remove firmware")
                }
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            Text("Run a Transient analysis to execute it: pins drive the circuit, inputs and the ADC read it, and the serial output appears in Simulation.")
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
        }
    }
}

/// The part's simulation model: built-in, or an imported SPICE .model / .subckt (SpiceModelSheet).
private struct SpiceModelProperties: View {
    @EnvironmentObject private var store: DesignStore
    var component: SnapComponent
    @State private var editing = false

    var body: some View {
        PropertyGroup(title: "SPICE Model") {
            if let spice = component.spice {
                HStack(spacing: 6) {
                    Image(systemName: "function").foregroundStyle(Theme.skyBlue)
                    Text(verbatim: spice.model).foregroundStyle(Theme.textPrimary).lineLimit(1).truncationMode(.middle)
                    Spacer()
                    Button(role: .destructive) {
                        store.setSpiceModel(component.id, text: "", model: "", pins: "")
                    } label: { Image(systemName: "trash") }
                    .buttonStyle(.borderless)
                    .help("Remove the SPICE model")
                    .accessibilityLabel("Remove the SPICE model")
                }
                if !spice.pins.isEmpty {
                    HStack {
                        Text("Pin map").foregroundStyle(Theme.textMuted)
                        Spacer()
                        Text(verbatim: spice.pins).foregroundStyle(Theme.textPrimary).font(.callout.monospaced()).textSelection(.enabled)
                    }
                    .font(.callout)
                }
                Button { editing = true } label: { Label("Edit SPICE Model…", systemImage: "pencil") }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            } else {
                (component.componentKind == .custom || component.componentKind == .ic8 ? Text("No imported model") : Text("Built-in model"))
                    .font(.caption).foregroundStyle(Theme.textMuted)
                Button { editing = true } label: { Label("Attach SPICE Model…", systemImage: "square.and.arrow.down") }
                    .buttonStyle(.bordered)
                    .controlSize(.small)
            }
        }
        .sheet(isPresented: $editing) {
            SpiceModelSheet(component: component).environmentObject(store)
        }
    }
}

private struct MultiSelectionProperties: View {
    @EnvironmentObject private var store: DesignStore
    var count: Int

    var body: some View {
        PropertyGroup(title: "\(count) components selected") {
            HStack {
                Button { store.rotateSelection() } label: { Label("Rotate", systemImage: "rotate.right") }
                Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete", systemImage: "trash") }
            }
            .buttonStyle(.bordered)
            let units = store.selectedComponents.filter { $0.unit != nil && $0.unitOf != nil }
            if units.count == 2 && units[0].customPart == units[1].customPart {
                Button { store.swapGates(units[0].id, units[1].id) } label: {
                    Label("Swap Gates", systemImage: "arrow.triangle.swap")
                }
                .buttonStyle(.bordered)
                .help("The two gates exchange places in their packages; the symbols and wires stay")
            }
        }
    }
}

/// A graphical bus: its name (bus notation), members, entries, and the tools that connect it.
private struct BusProperties: View {
    @EnvironmentObject private var store: DesignStore
    var bus: BusInfo
    @State private var name: String

    init(bus: BusInfo) {
        self.bus = bus
        _name = State(initialValue: bus.name)
    }

    var body: some View {
        let entries = store.sheetSnapshot.components.filter { $0.bus == bus.id }
        let ripped = Set(entries.map(\.value))
        let parts = store.sheetSnapshot.components.filter { !$0.componentKind.isVirtual && $0.componentKind != .junction }
        PropertyGroup(title: "Bus") {
            LabeledContent("Name") {
                TextField("D[0..7]", text: $name)
                    .textFieldStyle(.blue)
                    .onSubmit { store.renameBus(bus.id, to: name) }
            }
            PropertyRow(label: "Members", value: "\(bus.members.count)")
            PropertyRow(label: "Entries", value: "\(entries.count)")
            let open = bus.members.filter { !ripped.contains($0) }
            if !open.isEmpty {
                Text(verbatim: open.prefix(12).joined(separator: " ") + (open.count > 12 ? " …" : ""))
                    .font(.caption.monospaced())
                    .foregroundStyle(Theme.warning)
            }
            Button { store.ripBusEntries(bus.id) } label: { Label("Rip Out Entries", systemImage: "arrow.turn.down.right") }
                .disabled(open.isEmpty)
                .help("An entry (a local net label on the bus) for every member that has none yet")
            Menu {
                ForEach(parts) { part in
                    Button(part.ref) { store.connectBus(bus.id, toPart: part.id) }
                }
            } label: {
                Label("Connect to Part", systemImage: "point.3.connected.trianglepath.dotted")
            }
            .fixedSize()
            .disabled(parts.isEmpty)
            .help("Wire members to the part's pins of the same names (D0 → D0), else to its open pins in order")
            Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete Bus", systemImage: "trash") }
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
    }
}

private struct WireProperties: View {
    @EnvironmentObject private var store: DesignStore
    var wire: SnapWire

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            PropertyGroup(title: "Wire") {
                PropertyRow(label: "Net", value: store.snapshot.net(wire.net)?.name ?? "—")
                if let v = store.dcResult?.voltage(net: wire.net) {
                    PropertyRow(label: "DC voltage", value: EngineeringFormat.string(v, unit: "V", digits: 4))
                }
                NetNavigatorView(net: wire.net)
                Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete Wire", systemImage: "trash") }
                    .buttonStyle(.bordered)
            }
            if let anchor = directiveAnchor {
                let existing = store.directive(on: anchor)
                NetDirectiveEditor(anchor: anchor, directive: existing)
                    .id("directive|\(anchor.component)|\(anchor.pin)|\(existing?.id ?? 0)|\(existing?.summary ?? "")")
            }
        }
    }

    /// The pin a directive on this wire's net sits on: an end that is a real pin (a block part's for a channel copy).
    private var directiveAnchor: PinAddress? {
        for end in [wire.a, wire.b] {
            guard let c = store.snapshot.component(end.component), c.componentKind != .junction else { continue }
            return PinAddress(component: c.instanceOf ?? c.id, pin: end.pin)
        }
        return nil
    }
}

/// A wire node: a T-junction where wires meet, or a bend point that shapes a wire.
private struct JunctionProperties: View {
    @EnvironmentObject private var store: DesignStore
    var junction: SnapComponent

    var body: some View {
        let wires = store.snapshot.wires.filter { $0.a.component == junction.id || $0.b.component == junction.id }
        let net = wires.first?.net ?? -1
        PropertyGroup(title: wires.count >= 3 ? "Junction" : (wires.count == 2 ? "Wire bend" : "Open wire end")) {
            PropertyRow(label: "Net", value: store.snapshot.net(net)?.name ?? "—")
            PropertyRow(label: "Wires", value: "\(wires.count)")
            Text(wires.count == 2 ? "Drag to reshape the wire. Delete straightens it."
                 : "Drag to move. Wires ending here are joined; a dot marks the T.")
                .font(.caption).foregroundStyle(Theme.textMuted)
            Button(role: .destructive) { store.deleteSelection() } label: {
                Label(wires.count == 2 ? "Remove Bend" : "Delete Junction", systemImage: "trash")
            }
            .buttonStyle(.bordered)
        }
    }
}

private struct ProjectProperties: View {
    @EnvironmentObject private var store: DesignStore
    @State private var name = ""
    @State private var original = ""
    @FocusState private var nameFocused: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            PropertyGroup(title: "Project") {
                TextField("Project name", text: $name)
                    .textFieldStyle(.blue)
                    .focused($nameFocused)
                    .onSubmit { commitName() }
                    .onChange(of: nameFocused) { _, isFocused in if !isFocused { commitName() } }
                PropertyRow(label: "Components", value: "\(store.snapshot.components.filter { !$0.componentKind.isVirtual }.count)")
                PropertyRow(label: "Nets", value: "\(store.snapshot.nets.filter { $0.pinCount > 1 }.count)")
                PropertyRow(label: "Wires", value: "\(store.snapshot.wires.count)")
            }
            PropertyGroup(title: "Title Block") {
                TitleBlockEditor()
            }
            IndustryProperties()
            RobotSystemProperties()
            EcuSystemProperties()
            AerospaceSystemProperties()
            NavalSystemProperties()
            MedicalSystemProperties()
            RetailSystemProperties()
            ApplianceSystemProperties()
            MemorySystemProperties()
            PropertyGroup(title: "Board") {
                PropertyRow(label: "Size", value: String(format: "%.1f × %.1f mm", store.snapshot.board.width, store.snapshot.board.height))
                PropertyRow(label: "Layers", value: "\(store.snapshot.board.layerCount)")
                PropertyRow(label: "Design rules", value: store.snapshot.board.rulePreset)
                PropertyRow(label: "Track / clearance", value: String(format: "%.2f / %.2f mm", store.snapshot.board.trackWidth, store.snapshot.board.clearance))
                PropertyRow(label: "Via", value: String(format: "%.2f / %.2f mm", store.snapshot.board.viaDiameter, store.snapshot.board.viaDrill))
                PropertyRow(label: "Tracks / vias", value: "\(store.snapshot.tracks.count) / \(store.snapshot.vias.count)")
            }
            if !store.snapshot.requirements.isEmpty {
                PropertyGroup(title: "Requirements") {
                    Text(store.snapshot.requirements)
                        .font(.caption)
                        .foregroundStyle(Theme.textSecondary)
                        .textSelection(.enabled)
                }
            }
            Text("Select a part on the schematic or PCB to edit its properties.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
        }
        .onAppear {
            name = store.snapshot.name
            original = store.snapshot.name
        }
    }

    /// Commits only a name the user typed: when another design is loaded this panel is rebuilt, and its stale
    /// text must not be written into the new design.
    private func commitName() {
        let trimmed = name.trimmingCharacters(in: .whitespaces)
        guard !trimmed.isEmpty, trimmed != original, original == store.snapshot.name else { return }
        original = trimmed
        store.setProjectName(trimmed)
    }
}

/// Robot platform and the seven modular design segments (power, compute, motion, sensors, comms, safety,
/// mechanical), each with its checklist evaluated on the current design.
private struct RobotSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Robot System Segments", report: store.robotSegments(), noneLabel: "Not a robot",
                            selection: Binding(get: { store.snapshot.robotPlatform }, set: { store.setRobotPlatform($0) }),
                            hint: "Pick a platform (or the Robotics / UAV industry) to check the power, compute, motion, "
                                + "sensor, communication, safety and mechanical segments.")
    }
}

/// Automotive ECU type and the six ECU design segments (protection front-end, regulation, safety MCU, vehicle
/// networks, actuation, sensor conditioning).
private struct EcuSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Automotive ECU Segments", report: store.ecuSegments(), noneLabel: "Not an ECU",
                            selection: Binding(get: { store.snapshot.ecuType }, set: { store.setEcuType($0) }),
                            hint: "Pick an ECU type (or the Automotive industry) to check the protection front-end, "
                                + "regulation, safety MCU, vehicle network, actuation and sensor segments.")
    }
}

/// Aerospace mission and the five aerospace design segments (rad-hard compute, power conditioning, sensor
/// interface, avionics communications, RF telemetry).
private struct AerospaceSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Aerospace Segments", report: store.aerospaceSegments(), noneLabel: "Not aerospace",
                            selection: Binding(get: { store.snapshot.aerospaceMission },
                                               set: { store.setAerospaceMission($0) }),
                            hint: "Pick a mission (or the Space industry) to check the rad-hard compute, power isolation, "
                                + "sensor, avionics bus and RF telemetry segments.")
    }
}

/// Naval platform and the five naval design segments (power isolation, hermetic compute, shock, data links,
/// radar / sonar).
private struct NavalSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Naval Segments", report: store.navalSegments(), noneLabel: "Not naval",
                            selection: Binding(get: { store.snapshot.navalPlatform }, set: { store.setNavalPlatform($0) }),
                            hint: "Pick a platform (or the Marine industry) to check the power isolation, corrosion, shock, "
                                + "data link and radar / sonar segments.")
    }
}

/// Medical device class and the four medical design segments (patient isolation, biosignal acquisition, safety
/// compute & power, coexistence & wireless).
private struct MedicalSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Medical Segments", report: store.medicalSegments(), noneLabel: "Not medical",
                            selection: Binding(get: { store.snapshot.medicalClass }, set: { store.setMedicalClass($0) }),
                            hint: "Pick a device class (or the Medical industry) to check patient isolation, biosignal, "
                                + "safety compute and wireless coexistence segments.")
    }
}

/// Retail device class and the four POS design segments (payment security & anti-tamper, printer drivers, HMI &
/// peripherals, ESD & environment).
private struct RetailSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Retail / POS Segments", report: store.retailSegments(), noneLabel: "Not retail",
                            selection: Binding(get: { store.snapshot.retailDevice }, set: { store.setRetailDevice($0) }),
                            hint: "Pick a device class (or the Retail & POS industry) to check payment security, printer "
                                + "drivers, peripherals and ESD segments.")
    }
}

/// Home appliance type and the four appliance design segments (mains entry, actuation & motor control, HMI &
/// sensing, IoT).
private struct ApplianceSystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Home Appliance Segments", report: store.applianceSegments(), noneLabel: "Not an appliance",
                            selection: Binding(get: { store.snapshot.applianceType }, set: { store.setApplianceType($0) }),
                            hint: "Pick an appliance type (or the Home Appliances industry) to check mains entry, actuation, "
                                + "sensing and IoT segments.")
    }
}

/// Memory design type and the five memory segments (power & decoupling, clock / command / address, data
/// integrity, configuration, layout).
private struct MemorySystemProperties: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        SystemSegmentsGroup(title: "Memory (RAM) Segments", report: store.memorySegments(), noneLabel: "Not a memory design",
                            selection: Binding(get: { store.snapshot.memoryDesign }, set: { store.setMemoryDesign($0) }),
                            hint: "Pick a memory design type (or the Memory & DRAM industry) to check power, clock / address, "
                                + "data, configuration and layout segments.")
    }
}

/// A segment checklist with a type picker: robot platforms, ECU types, aerospace missions, naval platforms and
/// medical device classes share it.
private struct SystemSegmentsGroup: View {
    @EnvironmentObject private var store: DesignStore
    let title: String
    let report: RobotSegmentsReport
    let noneLabel: String
    let selection: Binding<String>
    let hint: String
    @State private var expanded: Set<String> = []

    var body: some View {
        PropertyGroup(title: title) {
            Picker("Type", selection: selection) {
                Text(noneLabel).tag("")
                ForEach(report.platforms) { Text($0.name).tag($0.id) }
            }
            .labelsHidden()
            if let platform = report.platforms.first(where: { $0.id == selection.wrappedValue }) {
                Text(platform.description).font(.caption).foregroundStyle(Theme.textSecondary)
                ForEach(platform.guidance, id: \.self) { line in
                    Label(line, systemImage: "lightbulb").font(.caption).foregroundStyle(Theme.textSecondary)
                }
                if let kit = platform.kit, !kit.isEmpty {
                    RobotKitList(platformId: platform.id, kit: kit)
                }
            }
            if report.applies {
                ForEach(report.segments) { segment in
                    DisclosureGroup(isExpanded: Binding(get: { expanded.contains(segment.id) },
                                                        set: { if $0 { expanded.insert(segment.id) } else { expanded.remove(segment.id) } })) {
                        VStack(alignment: .leading, spacing: 3) {
                            ForEach(segment.items) { item in
                                Label {
                                    VStack(alignment: .leading, spacing: 0) {
                                        Text(item.label).foregroundStyle(Theme.textPrimary)
                                        Text(item.detail).foregroundStyle(Theme.textMuted)
                                    }
                                } icon: {
                                    Image(systemName: item.ok ? "checkmark.circle.fill" : "circle.dashed")
                                        .foregroundStyle(item.ok ? Color.green : Color.orange)
                                }
                                .font(.caption)
                            }
                            ForEach(segment.guidance, id: \.self) { Text($0).font(.caption2).foregroundStyle(Theme.textMuted) }
                            if segment.id == "motion" || segment.id == "actuation" {
                                Button("Add Thermal Vias") { store.addThermalVias() }
                                    .controlSize(.small)
                                    .disabled(store.snapshot.pads.isEmpty)
                                    .help("Stitch vias at the selected parts' drain / tab pads (every power MOSFET if none is selected)")
                            }
                        }
                        .padding(.top, 2)
                    } label: {
                        HStack {
                            Image(systemName: segment.status == "complete" ? "checkmark.seal.fill"
                                  : segment.status == "partial" ? "circle.lefthalf.filled" : "circle")
                                .foregroundStyle(segment.status == "complete" ? Color.green
                                                 : segment.status == "partial" ? Color.orange : Theme.textMuted)
                            Text(segment.name).font(.caption).foregroundStyle(Theme.textPrimary)
                        }
                    }
                }
            } else {
                Text(hint).font(.caption).foregroundStyle(Theme.textMuted)
            }
        }
    }
}

/// A robot platform's production parts kit by subsystem, with one click to put the whole kit in the project library.
private struct RobotKitList: View {
    @EnvironmentObject private var store: DesignStore
    let platformId: String
    let kit: [RobotKitGroupInfo]
    @State private var expanded = false
    @State private var lastAdded: Int?

    var body: some View {
        DisclosureGroup(isExpanded: $expanded) {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(kit) { group in
                    VStack(alignment: .leading, spacing: 1) {
                        Text(group.subsystem).font(.caption.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                        Text(group.parts.joined(separator: " · ")).font(.caption2).foregroundStyle(Theme.textMuted)
                            .textSelection(.enabled)
                    }
                }
                HStack {
                    Button("Add Kit to Library") { lastAdded = store.addRobotKitToLibrary(platformId) }
                        .controlSize(.small)
                        .help("Add every part of this kit to the project library so it shows in the device picker")
                    if let lastAdded {
                        Text(lastAdded == 0 ? "Already in the library" : "\(lastAdded) parts added")
                            .font(.caption2).foregroundStyle(Theme.textMuted)
                    }
                }
            }
            .padding(.top, 2)
        } label: {
            Label("Parts Kit (\(Set(kit.flatMap(\.parts)).count) parts)", systemImage: "shippingbox")
                .font(.caption).foregroundStyle(Theme.skyBlue)
        }
    }
}

/// Industry profile picker: standards, derating and design guidance for the project's domain.
private struct IndustryProperties: View {
    @EnvironmentObject private var store: DesignStore
    @State private var showGuidance = false

    var body: some View {
        PropertyGroup(title: "Industry Profile") {
            Picker("Industry", selection: Binding(
                get: { store.snapshot.industry },
                set: { id in if let profile = StandardLibrary.industry(id) { store.setIndustry(profile) } }
            )) {
                ForEach(StandardLibrary.industries) { profile in
                    Label(profile.name, systemImage: profile.systemImage).tag(profile.id)
                }
            }
            .labelsHidden()
            if let profile = StandardLibrary.industry(store.snapshot.industry) {
                Text(profile.description).font(.caption).foregroundStyle(Theme.textSecondary)
                PropertyRow(label: "Derating", value: profile.deratingSummary)
                Text(profile.standards).font(.caption).foregroundStyle(Theme.lightBlue).textSelection(.enabled)
                DisclosureGroup("Design guidance", isExpanded: $showGuidance) {
                    VStack(alignment: .leading, spacing: 4) {
                        ForEach(profile.guidance, id: \.self) { line in
                            Label(line, systemImage: "checkmark.circle")
                                .font(.caption)
                                .foregroundStyle(Theme.textSecondary)
                        }
                    }
                    .padding(.top, 4)
                }
                .font(.caption)
                .foregroundStyle(Theme.skyBlue)
            }
        }
    }
}

/// Per-channel parameters of a part on a repeated sheet beyond its value: fitted in this channel, and this channel's
/// own SPICE model or firmware (set from the SPICE model sheet's "This channel only") with a way back to the block's.
private struct ChannelParameterRows: View {
    @EnvironmentObject private var store: DesignStore
    var component: SnapComponent

    var body: some View {
        let bits = component.channelOverride ?? 0
        Toggle("Fitted in this channel", isOn: Binding(get: { component.isFitted }, set: { store.setChannelFitted(component.id, $0) }))
            .toggleStyle(.checkbox)
            .font(.caption)
            .help("Leave the part off (DNP) in this channel only; the other channels keep theirs")
        if bits & 4 != 0 {
            override("This channel has its own SPICE model", what: "spice")
        }
        if bits & 8 != 0 {
            override("This channel has its own firmware", what: "firmware")
        }
    }

    private func override(_ title: LocalizedStringKey, what: String) -> some View {
        HStack {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            Spacer()
            Button("Use Block's") { store.clearChannelOverride(component.id, what) }
                .controlSize(.small)
        }
    }
}
