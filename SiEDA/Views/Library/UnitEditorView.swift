import AppKit
import SwiftUI

/// Unit (gate) editor inside the Symbol Editor (Altium "Part A / Part B", KiCad "units"): define the gates of a
/// multi-unit part, assign each pin to a gate, share pins between gates, leave supplies to the power unit, and say
/// which gates and pins may be swapped. The core checks the result and draws each unit's symbol; replaces editing the
/// part's "units" JSON by hand.
struct UnitEditorPanel: View {
    /// The part with the current symbol layout (the unit symbols are generated from it).
    let spec: CustomPartSpec
    @Binding var draft: UnitDraft
    /// Error count of the unit checks, for the editor's Apply button.
    @Binding var errorCount: Int

    @State private var history = EditHistory<UnitDraft>()
    @State private var selectedUnit = 0
    @State private var selectedPins: Set<String> = []
    @State private var issues: [SymbolIssue] = []
    @State private var preview: CustomPartInfo?
    @State private var detectFailed = false

    var body: some View {
        HStack(spacing: 0) {
            unitList
                .frame(width: 230)
                .background(Theme.deepBlue.opacity(0.45))
            Divider()
            pinMatrix
                .frame(minWidth: 380, maxWidth: .infinity, maxHeight: .infinity)
            Divider()
            ScrollView { rightPanel.padding(12) }
                .frame(width: 300)
                .background(Theme.deepBlue.opacity(0.45))
        }
        .onAppear { refresh() }
        .onChange(of: draft) { _, _ in refresh() }
        .onChange(of: spec) { _, _ in refresh() }
    }

    // MARK: - Units

    private var unitList: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("UNITS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            if !draft.isMultiUnit {
                Text("One symbol for the whole part. Add units to draw each gate on its own (a quad op-amp's A–D); pins on no unit form the power unit P.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 4) {
                    ForEach(Array(draft.units.enumerated()), id: \.offset) { index, unit in
                        unitRow(index, unit)
                    }
                }
            }
            HStack {
                Button { edit { $0.addUnit(pins: selectedPins.sorted(by: Self.pinOrder)) } } label: {
                    Label("Add Unit", systemImage: "plus")
                }
                .help("A new gate; the selected pins go on it")
                Button(role: .destructive) { edit { $0.removeUnit(at: selectedUnit) } } label: { Image(systemName: "minus") }
                    .disabled(!draft.units.indices.contains(selectedUnit))
                    .help("Remove the selected unit (its pins go to the power unit)")
                    .accessibilityLabel("Remove unit")
            }
            HStack {
                Button {
                    if let gates = UnitDraft.detectGates(spec.pins) {
                        edit { $0 = UnitDraft(units: gates) }
                        detectFailed = false
                    } else {
                        detectFailed = true
                    }
                } label: { Label("Detect Gates", systemImage: "wand.and.stars") }
                    .help("Split the pins into gates from their names (1A, 1B, 1Y… or OUT1, IN1-, IN1+…)")
                Button("One Symbol") { edit { $0 = UnitDraft(units: []) } }
                    .disabled(!draft.isMultiUnit)
                    .help("No units: the part is drawn as one symbol")
            }
            if detectFailed {
                Text("No gates found in the pin names.").font(.caption).foregroundStyle(Theme.warning)
            }
            HStack {
                Button { _ = history.undo(&draft) } label: { Image(systemName: "arrow.uturn.backward") }
                    .disabled(!history.canUndo)
                    .help("Undo the last unit edit")
                    .accessibilityLabel("Undo unit edit")
                Button { _ = history.redo(&draft) } label: { Image(systemName: "arrow.uturn.forward") }
                    .disabled(!history.canRedo)
                    .help("Redo")
                    .accessibilityLabel("Redo unit edit")
            }
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .padding(10)
    }

    private func unitRow(_ index: Int, _ unit: CustomPartSpec.Unit) -> some View {
        let selected = index == selectedUnit
        return VStack(alignment: .leading, spacing: 4) {
            HStack {
                TextField("Name", text: Binding(get: { unit.name }, set: { name in edit { $0.rename(index, to: name) } }))
                    .textFieldStyle(.blue)
                    .frame(width: 60)
                Text("\(unit.pins.count) pins").font(.caption).foregroundStyle(Theme.textMuted)
                Spacer()
            }
            Picker("Swap", selection: Binding(get: { unit.swap ?? 0 }, set: { v in edit { $0.setSwap(v, unit: index) } })) {
                Text("Identical gates").tag(0)
                ForEach(1...4, id: \.self) { group in Text("Swap group \(group)").tag(group) }
                Text("Never swap").tag(-1)
            }
            .font(.caption)
            .help("Gates of one swap group (or identical gates) can be exchanged by Swap Gates and packing")
        }
        .padding(6)
        .background(RoundedRectangle(cornerRadius: 6).fill(selected ? Theme.blue.opacity(0.35) : Color.clear))
        .contentShape(Rectangle())
        .onTapGesture { selectedUnit = index }
    }

    // MARK: - Pin matrix

    private static func pinOrder(_ a: String, _ b: String) -> Bool {
        let na = Int(a) ?? Int.max, nb = Int(b) ?? Int.max
        return na == nb ? a < b : na < nb
    }

    private var pinMatrix: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 6) {
                Text("PINS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                let pins = selectedPins.sorted(by: Self.pinOrder)
                if !pins.isEmpty && draft.isMultiUnit {
                    if draft.units.indices.contains(selectedUnit) {
                        Button("Only on \(draft.units[selectedUnit].name)") { edit { $0.assign(pins, to: selectedUnit) } }
                    }
                    Button("Shared by All") { edit { $0.share(pins) } }
                        .help("One pin drawn on every gate (a shared enable or supply)")
                    Button("Power Unit") { edit { $0.makePower(pins) } }
                        .help("On no gate: the pin goes to the power unit P with the supplies")
                }
            }
            .buttonStyle(.bordered)
            .controlSize(.small)
            .padding(10)
            Divider()
            ScrollView([.vertical, .horizontal]) {
                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 6) {
                        Text("Pin").frame(width: 44, alignment: .leading)
                        Text("Name").frame(width: 90, alignment: .leading)
                        Text("Type").frame(width: 84, alignment: .leading)
                        ForEach(Array(draft.units.enumerated()), id: \.offset) { _, unit in
                            Text(verbatim: unit.name).frame(width: 30)
                        }
                        Text("P").frame(width: 24).help("Power unit: pins on no gate")
                    }
                    .font(.caption.weight(.semibold))
                    .foregroundStyle(Theme.textMuted)
                    ForEach(spec.pins) { pin in pinRow(pin) }
                }
                .padding(10)
            }
        }
    }

    private func pinRow(_ pin: CustomPartSpec.Pin) -> some View {
        let on = draft.units(of: pin.number)
        let selected = selectedPins.contains(pin.number)
        return HStack(spacing: 6) {
            Text(verbatim: pin.number).frame(width: 44, alignment: .leading)
            Text(verbatim: pin.name).frame(width: 90, alignment: .leading).lineLimit(1)
            Text(pin.type.title).frame(width: 84, alignment: .leading).lineLimit(1).foregroundStyle(Theme.textMuted)
            ForEach(Array(draft.units.indices), id: \.self) { index in
                Toggle("", isOn: Binding(get: { on.contains(index) }, set: { _ in edit { $0.toggle(pin.number, unit: index) } }))
                    .toggleStyle(.checkbox)
                    .labelsHidden()
                    .frame(width: 30)
                    .accessibilityLabel("Pin \(pin.number) on unit \(draft.units[index].name)")
            }
            Group {
                if on.count > 1 {
                    Image(systemName: "link").foregroundStyle(Theme.warning).help("Shared by several gates")
                } else if on.isEmpty && draft.isMultiUnit {
                    Image(systemName: "bolt.fill").foregroundStyle(Theme.textMuted).help("On the power unit P")
                } else {
                    Color.clear
                }
            }
            .frame(width: 24, height: 14)
        }
        .font(.system(.caption, design: .monospaced))
        .foregroundStyle(selected ? Theme.selection : Theme.textSecondary)
        .padding(.vertical, 1)
        .background(selected ? Theme.blue.opacity(0.2) : Color.clear)
        .contentShape(Rectangle())
        .onTapGesture {
            if NSEvent.modifierFlags.contains(.command) || NSEvent.modifierFlags.contains(.shift) {
                if selected { selectedPins.remove(pin.number) } else { selectedPins.insert(pin.number) }
            } else {
                selectedPins = [pin.number]
            }
        }
    }

    // MARK: - Preview, pin swap and checks

    @ViewBuilder
    private var rightPanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            if draft.units.indices.contains(selectedUnit) {
                let unit = draft.units[selectedUnit]
                Text("UNIT \(unit.name)").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                if let symbol = preview?.unitSymbols?.first(where: { $0.name == unit.name })?.symbol {
                    UnitSymbolPreview(symbol: symbol)
                        .frame(height: 180)
                        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.navy))
                }
                Text("PIN SWAP").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                let groups = unit.pinSwap ?? []
                if groups.isEmpty {
                    Text("No interchangeable pins. Select pins of this gate (⌘-click) and make them a swap group — the inputs of a NAND gate.")
                        .font(.caption).foregroundStyle(Theme.textMuted)
                }
                ForEach(Array(groups.enumerated()), id: \.offset) { _, group in
                    Text(verbatim: group.map { number in
                        number + " " + (spec.pins.first { $0.number == number }?.name ?? "")
                    }.joined(separator: " ↔ "))
                    .font(.caption.monospaced())
                    .foregroundStyle(Theme.textPrimary)
                }
                HStack {
                    Button("Make Swap Group") {
                        edit { $0.makePinSwapGroup(selectedPins.sorted(by: Self.pinOrder), unit: selectedUnit) }
                    }
                    .disabled(selectedPins.filter { unit.pins.contains($0) }.count < 2)
                    Button("Clear") { edit { $0.clearPinSwap(unit: selectedUnit) } }
                        .disabled(groups.isEmpty)
                }
                .buttonStyle(.bordered)
                .controlSize(.small)
                Divider()
            }
            VStack(alignment: .leading, spacing: 4) {
                Text("CHECKS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                if draft.isMultiUnit && !issues.contains(where: { $0.severity != "info" }) {
                    Label("Every unit is valid", systemImage: "checkmark.seal.fill")
                        .font(.caption).foregroundStyle(Theme.liveOn)
                }
                ForEach(issues) { issue in
                    Button { selectedPins = Set(issue.pins) } label: {
                        Label(issue.message, systemImage: issue.isError ? "xmark.octagon.fill"
                              : issue.severity == "warning" ? "exclamationmark.triangle.fill" : "info.circle")
                            .font(.caption)
                            .foregroundStyle(issue.isError ? Theme.error : issue.severity == "warning" ? Theme.warning : Theme.textSecondary)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }

    // MARK: - Editing

    private func edit(_ change: (inout UnitDraft) -> Void) {
        var next = draft
        change(&next)
        guard next != draft else { return }
        history.record(draft)
        draft = next
    }

    private func refresh() {
        let edited = draft.applied(to: spec)
        issues = draft.isMultiUnit ? EDAEngine.checkUnits(edited) : []
        errorCount = issues.filter(\.isError).count
        if errorCount == 0, draft.isMultiUnit, case .success(let part) = EDAEngine.previewCustomPart(edited) {
            preview = part
        }
        if !draft.units.indices.contains(selectedUnit) { selectedUnit = max(0, draft.units.count - 1) }
    }
}

/// A unit's generated symbol: the body and its pins with names.
private struct UnitSymbolPreview: View {
    let symbol: CustomPartInfo.Symbol

    var body: some View {
        Canvas { context, size in
            let extentX = symbol.halfWidth + 30, extentY = symbol.halfHeight + 30
            let scale = min(size.width / (2 * extentX), size.height / (2 * extentY))
            let center = CGPoint(x: size.width / 2, y: size.height / 2)
            func point(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: center.x + x * scale, y: center.y + y * scale) }
            let body = CGRect(x: center.x - symbol.halfWidth * scale, y: center.y - symbol.halfHeight * scale,
                              width: 2 * symbol.halfWidth * scale, height: 2 * symbol.halfHeight * scale)
            context.stroke(Path(body), with: .color(Theme.skyBlue), lineWidth: 1.5)
            for pin in symbol.pins {
                let end = point(pin.x, pin.y)
                let edge: CGPoint
                switch pin.sideLetter {
                case "T": edge = CGPoint(x: end.x, y: body.minY)
                case "B": edge = CGPoint(x: end.x, y: body.maxY)
                case "R": edge = CGPoint(x: body.maxX, y: end.y)
                default: edge = CGPoint(x: body.minX, y: end.y)
                }
                var lead = Path()
                lead.move(to: edge)
                lead.addLine(to: end)
                context.stroke(lead, with: .color(Theme.textSecondary), lineWidth: 1)
                let label = Text(verbatim: pin.name).font(.system(size: 9)).foregroundColor(Theme.textPrimary)
                let inside = CGPoint(x: edge.x + (pin.sideLetter == "R" ? -4 : pin.sideLetter == "L" ? 4 : 0),
                                     y: edge.y + (pin.sideLetter == "T" ? 7 : pin.sideLetter == "B" ? -7 : 0))
                let anchor: UnitPoint = pin.sideLetter == "R" ? .trailing : pin.sideLetter == "L" ? .leading : .center
                context.draw(label, at: inside, anchor: anchor)
            }
        }
        .accessibilityLabel("Unit symbol preview")
    }
}
