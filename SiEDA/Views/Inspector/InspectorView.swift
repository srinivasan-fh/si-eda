import SwiftUI

/// Altium-style Properties panel.
struct InspectorView: View {
    @EnvironmentObject private var store: DesignStore

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                Text("PROPERTIES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                let selected = store.selectedComponents
                if selected.count == 1, let c = selected.first {
                    ComponentProperties(component: c)
                        .id("\(c.id)|\(c.ref)|\(c.value)")
                } else if selected.count > 1 {
                    MultiSelectionProperties(count: selected.count)
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
    @FocusState private var focus: Field?

    private enum Field { case ref, value }

    init(component: SnapComponent) {
        self.component = component
        _ref = State(initialValue: component.ref)
        _value = State(initialValue: component.value)
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
                    LabeledContent("Designator") {
                        TextField("Designator", text: $ref)
                            .textFieldStyle(.blue)
                            .focused($focus, equals: .ref)
                            .onSubmit { commitRef() }
                    }
                }
                LabeledContent(kind == .netLabel ? "Net name" : "Value") {
                    TextField("Value", text: $value)
                        .textFieldStyle(.blue)
                        .focused($focus, equals: .value)
                        .onSubmit { commitValue() }
                }
                Text(kind.valueHint).font(.caption).foregroundStyle(Theme.textMuted)
                HStack {
                    Button { store.rotateSelection() } label: { Label("Rotate", systemImage: "rotate.right") }
                    Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete", systemImage: "trash") }
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
            }

            let series = ESeries.preferred(for: kind)
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
            }

            if !kind.isVirtual {
                PropertyGroup(title: "PCB Footprint") {
                    PropertyRow(label: "Footprint", value: component.footprint)
                    if component.pcb.placed {
                        PropertyRow(label: "Position", value: String(format: "%.2f, %.2f mm", component.pcb.x, component.pcb.y))
                        PropertyRow(label: "Rotation", value: "\(component.pcb.rotation)°")
                        PropertyRow(label: "Layer", value: component.pcb.bottom ? "Bottom" : "Top")
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
        if trimmed.isEmpty || trimmed == component.ref { ref = component.ref; return }
        store.setRef(component.id, trimmed)
    }

    private func commitValue() {
        if value != component.value { store.setValue(component.id, value) }
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
        }
    }
}

private struct WireProperties: View {
    @EnvironmentObject private var store: DesignStore
    var wire: SnapWire

    var body: some View {
        PropertyGroup(title: "Wire") {
            PropertyRow(label: "Net", value: store.snapshot.net(wire.net)?.name ?? "—")
            if let v = store.dcResult?.voltage(net: wire.net) {
                PropertyRow(label: "DC voltage", value: EngineeringFormat.string(v, unit: "V", digits: 4))
            }
            Button(role: .destructive) { store.deleteSelection() } label: { Label("Delete Wire", systemImage: "trash") }
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
            IndustryProperties()
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
