import AppKit
import SwiftUI

/// Symbol Editor (Altium "SCH Library" / KiCad symbol editor): arrange where a part's pins sit on its schematic
/// symbol. Pins go on any of the four sides at any slot, groups are separated by gaps, repeated supply and ground
/// pins can be stacked (drawn once, joined into one net), and Auto Arrange lays a symbol out from the pins' names and
/// electrical types. The core lays the symbol out and checks it; the result is written back into the part.
struct SymbolEditorView: View {
    /// The part being edited; the edited part goes to `onApply`.
    let spec: CustomPartSpec
    let onApply: (CustomPartSpec) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var draft: SymbolDraft
    @State private var history = EditHistory<SymbolDraft>()
    @State private var selection: Set<String> = []  // pin numbers
    @State private var preview: CustomPartInfo?
    @State private var issues: [SymbolIssue] = []
    @State private var dragStart: CGPoint?
    @State private var dragLocation: CGPoint?
    @State private var stackDuplicates = true
    @State private var arrangeError: String?

    init(spec: CustomPartSpec, onApply: @escaping (CustomPartSpec) -> Void) {
        self.spec = spec
        self.onApply = onApply
        var initial = SymbolDraft(spec: spec)
        initial.reconcile(with: spec.pins)
        _draft = State(initialValue: initial)
    }

    var body: some View {
        VStack(spacing: 0) {
            toolbar
            Divider()
            HStack(spacing: 0) {
                canvas
                    .frame(minWidth: 460, maxWidth: .infinity, maxHeight: .infinity)
                Divider()
                ScrollView { sidePanel.padding(12) }
                    .frame(width: 320)
                    .background(Theme.deepBlue.opacity(0.45))
            }
            Divider()
            footer
        }
        .frame(minWidth: 960, minHeight: 640)
        .background(Theme.navy)
        .onAppear { refresh() }
        .onChange(of: draft) { _, _ in refresh() }
    }

    // MARK: - Toolbar

    private var toolbar: some View {
        HStack(spacing: 8) {
            Text("SYMBOL EDITOR · \(spec.name.isEmpty ? "untitled" : spec.name)")
                .font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Spacer()
            Toggle("Stack repeated power pins", isOn: $stackDuplicates)
                .toggleStyle(.checkbox)
                .font(.caption)
            Button { autoArrange() } label: { Label("Auto Arrange", systemImage: "wand.and.stars") }
                .help("Supplies on top, grounds at the bottom, inputs left, outputs right, MCU ports grouped")
            Button { edit { $0.placements = SymbolDraft.generated(spec.pins); $0.width = 0 } } label: {
                Label("Datasheet Order", systemImage: "list.number")
            }
            .help("The plain box: pins in number order down the left, then up the right")
            Button { edit { $0.mirror() } } label: { Label("Mirror", systemImage: "arrow.left.and.right") }
                .help("Swap the left and right sides")
            Button { edit { $0.closeGaps() } } label: { Label("Close Gaps", systemImage: "arrow.down.right.and.arrow.up.left") }
                .help("Remove the empty slots on every side")
            Divider().frame(height: 18)
            Button { _ = history.undo(&draft) } label: { Image(systemName: "arrow.uturn.backward") }
                .disabled(!history.canUndo)
                .help("Undo the last symbol edit")
                .accessibilityLabel("Undo symbol edit")
            Button { _ = history.redo(&draft) } label: { Image(systemName: "arrow.uturn.forward") }
                .disabled(!history.canRedo)
                .help("Redo")
                .accessibilityLabel("Redo symbol edit")
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .padding(10)
    }

    // MARK: - Canvas

    /// Symbol units → points, fitting the symbol with its leads and some margin.
    private func scale(for size: CGSize, part: CustomPartInfo) -> Double {
        let b = SchematicSymbols.bounds(.custom, custom: part).insetBy(dx: -40, dy: -40)
        return min(size.width / b.width, size.height / b.height, 3)
    }

    private func transform(for size: CGSize, part: CustomPartInfo) -> CGAffineTransform {
        let s = scale(for: size, part: part)
        return CGAffineTransform(translationX: size.width / 2, y: size.height / 2).scaledBy(x: s, y: s)
    }

    /// The stack under a point (view coordinates): the pin whose lead or name area is closest, within reach.
    private func pin(at point: CGPoint, size: CGSize, part: CustomPartInfo) -> String? {
        let inverse = transform(for: size, part: part).inverted()
        let p = point.applying(inverse)
        var best: (String, Double)?
        for pin in part.symbol.pins {
            let edge = SchematicSymbols.pinEdge(pin, halfWidth: part.symbol.halfWidth, halfHeight: part.symbol.halfHeight)
            // Reach: the lead plus the first part of the name inside the body.
            let inward = CGPoint(x: edge.x + (edge.x - pin.x) * 1.2, y: edge.y + (edge.y - pin.y) * 1.2)
            let d = distance(p, segment: (CGPoint(x: pin.x, y: pin.y), inward))
            if d < 7, d < (best?.1 ?? .infinity) { best = (pin.number, d) }
        }
        return best?.0
    }

    private func distance(_ p: CGPoint, segment s: (CGPoint, CGPoint)) -> Double {
        let dx = s.1.x - s.0.x, dy = s.1.y - s.0.y
        let len2 = dx * dx + dy * dy
        let t = len2 > 0 ? max(0, min(1, ((p.x - s.0.x) * dx + (p.y - s.0.y) * dy) / len2)) : 0
        return hypot(p.x - (s.0.x + t * dx), p.y - (s.0.y + t * dy))
    }

    /// Where a drop lands (view coordinates): the nearest side of the body and the slot along it.
    private func target(at point: CGPoint, size: CGSize, part: CustomPartInfo) -> (side: SymbolDraft.Side, slot: Int) {
        let p = point.applying(transform(for: size, part: part).inverted())
        let hw = part.symbol.halfWidth, hh = part.symbol.halfHeight
        let side: SymbolDraft.Side
        if p.x < -hw { side = .left } else if p.x > hw { side = .right } else if p.y < -hh { side = .top } else if p.y > hh {
            side = .bottom
        } else {
            let distances: [(SymbolDraft.Side, Double)] = [(.left, p.x + hw), (.right, hw - p.x), (.top, p.y + hh), (.bottom, hh - p.y)]
            side = distances.min { $0.1 < $1.1 }!.0
        }
        switch side {
        case .left, .right:
            let rows = max(1, max(draft.slotCount(.left), draft.slotCount(.right)))
            let top = -Double(rows - 1) * 10
            return (side, max(0, Int(((p.y - top) / 20).rounded())))
        case .top, .bottom:
            let count = draft.slotCount(side)
            let x0 = -Double(max(count, 1) - 1) * 10
            return (side, max(0, Int(((p.x - x0) / 20).rounded())))
        }
    }

    /// Symbol position of a slot on a side (for the drop marker).
    private func slotPoint(_ side: SymbolDraft.Side, _ slot: Int, part: CustomPartInfo) -> CGPoint {
        let hw = part.symbol.halfWidth, hh = part.symbol.halfHeight
        let rows = max(1, max(draft.slotCount(.left), draft.slotCount(.right)))
        let top = -Double(rows - 1) * 10
        switch side {
        case .left: return CGPoint(x: -hw - 20, y: top + Double(slot) * 20)
        case .right: return CGPoint(x: hw + 20, y: top + Double(slot) * 20)
        case .top: return CGPoint(x: -Double(max(draft.slotCount(.top), 1) - 1) * 10 + Double(slot) * 20, y: -hh - 20)
        case .bottom: return CGPoint(x: -Double(max(draft.slotCount(.bottom), 1) - 1) * 10 + Double(slot) * 20, y: hh + 20)
        }
    }

    private var flagged: Set<String> { Set(issues.filter { $0.severity != "info" }.flatMap(\.pins)) }

    private var canvas: some View {
        GeometryReader { geo in
            let size = geo.size
            ZStack {
                Theme.schematicBackground
                if let part = preview {
                    Canvas { ctx, _ in
                        let t = transform(for: size, part: part)
                        let shapes = SchematicSymbols.customShapes(part)
                        ctx.fill(shapes.fill.applying(t), with: .color(Theme.symbolFill))
                        ctx.stroke(shapes.stroke.applying(t), with: .color(Theme.symbol), style: StrokeStyle(lineWidth: 1.6, lineCap: .round))
                        ctx.fill(shapes.solid.applying(t), with: .color(Theme.symbol))
                        SchematicSymbols.drawPinLabels(ctx, part: part, transform: t, fontSize: max(7, min(12, 6 * scale(for: size, part: part))),
                                                       nameColor: { pin in
                                                           selection.contains(pin.number) ? Theme.selection
                                                               : flagged.contains(pin.number) ? Theme.error
                                                               : pin.type == PinElectricalType.powerIn.rawValue ? Theme.probe : Theme.skyBlue
                                                       },
                                                       numberColor: Theme.textMuted)
                        // Selected pins: a highlight on their pin ends.
                        for pin in part.symbol.pins where selection.contains(pin.number) || flagged.contains(pin.number) {
                            let e = CGPoint(x: pin.x, y: pin.y).applying(t)
                            ctx.stroke(Path(ellipseIn: CGRect(x: e.x - 6, y: e.y - 6, width: 12, height: 12)),
                                       with: .color(selection.contains(pin.number) ? Theme.selection : Theme.error), lineWidth: 2)
                        }
                        // While dragging: where the pins will land.
                        if let location = dragLocation, let start = dragStart, hypot(location.x - start.x, location.y - start.y) > 4,
                           !selection.isEmpty {
                            let drop = target(at: location, size: size, part: part)
                            let m = slotPoint(drop.side, drop.slot, part: part).applying(t)
                            ctx.fill(Path(ellipseIn: CGRect(x: m.x - 7, y: m.y - 7, width: 14, height: 14)), with: .color(Theme.blue.opacity(0.6)))
                            ctx.draw(Text("\(selection.count) → \(drop.side.title) \(drop.slot)").font(.caption2.weight(.semibold))
                                        .foregroundColor(Theme.iceBlue), at: CGPoint(x: location.x + 10, y: location.y - 12), anchor: .leading)
                        }
                    }
                    .contentShape(Rectangle())
                    .gesture(
                        DragGesture(minimumDistance: 0)
                            .onChanged { value in
                                if dragStart == nil {
                                    dragStart = value.startLocation
                                    let extend = NSEvent.modifierFlags.contains(.shift) || NSEvent.modifierFlags.contains(.command)
                                    if let hit = pin(at: value.startLocation, size: size, part: part) {
                                        let stack = Set(draft.stack(of: hit))
                                        if extend { selection = selection.isSuperset(of: stack) ? selection.subtracting(stack) : selection.union(stack) }
                                        else if !selection.contains(hit) { selection = stack }
                                    } else if !extend {
                                        selection = []
                                    }
                                }
                                if !selection.isEmpty, pin(at: value.startLocation, size: size, part: part) != nil {
                                    dragLocation = value.location
                                }
                            }
                            .onEnded { value in
                                defer {
                                    dragStart = nil
                                    dragLocation = nil
                                }
                                guard dragLocation != nil, hypot(value.translation.width, value.translation.height) > 4 else { return }
                                let drop = target(at: value.location, size: size, part: part)
                                let numbers = draft.placements.map(\.number).filter { selection.contains($0) }
                                if NSEvent.modifierFlags.contains(.option),
                                   let onto = draft.slots(drop.side).indices.contains(drop.slot) ? draft.slots(drop.side)[drop.slot].first : nil {
                                    // ⌥-drop onto a pin stacks the selection on it.
                                    edit { $0.stack([onto] + numbers) }
                                } else {
                                    edit { $0.move(numbers, to: drop.side, slot: drop.slot) }
                                }
                            }
                    )
                    .accessibilityElement()
                    .accessibilityLabel("Symbol canvas")
                    .accessibilityValue("\(spec.pins.count) pins, \(selection.count) selected")
                } else {
                    Text(issues.first?.message ?? "Add pins to the part to lay out its symbol.")
                        .font(.caption).foregroundStyle(Theme.textMuted).padding()
                }
            }
            .overlay(alignment: .bottomLeading) {
                Text("Click a pin to select (⇧ to add) · drag it to any side or slot · ⌥-drop onto a pin to stack")
                    .font(.caption2).foregroundStyle(Theme.textMuted).padding(8)
            }
        }
    }

    // MARK: - Side panel

    private var sidePanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            selectionPanel
            Divider()
            VStack(alignment: .leading, spacing: 6) {
                Text("BODY").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                HStack {
                    Text("Width").font(.caption).foregroundStyle(Theme.textMuted)
                    Stepper(draft.width > 0 ? "\(Int(draft.width)) (\(Int(draft.width / 10)) grid)" : "Auto (from pin names)",
                            value: Binding(get: { draft.width }, set: { w in edit { $0.width = w < 20 ? 0 : min(400, w) } }),
                            in: 0...400, step: 20)
                        .font(.caption)
                }
            }
            Divider()
            issueList
            Divider()
            pinTable
        }
    }

    private var selectedNumbers: [String] { draft.placements.map(\.number).filter { selection.contains($0) } }

    @ViewBuilder
    private var selectionPanel: some View {
        let numbers = selectedNumbers
        VStack(alignment: .leading, spacing: 6) {
            if numbers.isEmpty {
                Text("Select pins to move them to a side, stack them or change their order.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            } else {
                let pins = spec.pins.filter { selection.contains($0.number) }
                Text(numbers.count == 1 ? "PIN \(numbers[0])" : "\(numbers.count) PINS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                ForEach(pins.prefix(6)) { pin in
                    Text("\(pin.number) · \(pin.name) · \(pin.type.rawValue)").font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                }
                if pins.count > 6 { Text("…and \(pins.count - 6) more").font(.caption2).foregroundStyle(Theme.textMuted) }
                Text("Send to").font(.caption).foregroundStyle(Theme.textMuted)
                HStack {
                    ForEach(SymbolDraft.Side.allCases) { side in
                        Button(side.title) { edit { $0.move(numbers, to: side, slot: $0.slotCount(side)) } }
                            .help("Move to the end of the \(side.title.lowercased()) side")
                    }
                }
                .controlSize(.small)
                HStack {
                    Button { nudge(-1) } label: { Image(systemName: "arrow.up") }
                        .help("Earlier on its side (up, or left on the top / bottom)")
                        .accessibilityLabel("Move earlier")
                    Button { nudge(1) } label: { Image(systemName: "arrow.down") }
                        .help("Later on its side (down, or right on the top / bottom)")
                        .accessibilityLabel("Move later")
                    Button("Gap Before") {
                        if let p = draft.placement(of: numbers[0]) { edit { $0.insertGap(p.side, at: p.slot) } }
                    }
                    .help("Insert an empty slot before the pin to separate a group")
                }
                .controlSize(.small)
                HStack {
                    Button("Stack") { edit { $0.stack(numbers) } }
                        .disabled(numbers.count < 2)
                        .help("Put the selected pins on one spot: drawn once and joined into one net (repeated VDD / GND)")
                    Button("Unstack") { edit { d in numbers.dropFirst().forEach { d.unstack($0) } } }
                        .disabled(!numbers.contains { draft.stack(of: $0).count > 1 })
                        .help("Give each selected pin its own slot")
                }
                .controlSize(.small)
            }
        }
    }

    private var issueList: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("CHECKS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            if !issues.contains(where: { $0.severity != "info" }) {
                Label("Every pin is placed once; no pins collide", systemImage: "checkmark.seal.fill")
                    .font(.caption).foregroundStyle(Theme.liveOn)
            }
            ForEach(issues) { issue in
                Button { selection = Set(issue.pins) } label: {
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

    private var pinTable: some View {
        VStack(alignment: .leading, spacing: 6) {
            ForEach(SymbolDraft.Side.allCases) { side in
                let slots = draft.slots(side)
                if !slots.isEmpty {
                    Text("\(side.title.uppercased()) (\(slots.filter { !$0.isEmpty }.count) spots)")
                        .font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                    ForEach(Array(slots.enumerated()), id: \.offset) { slot, numbers in
                        if numbers.isEmpty {
                            Text("\(slot) — gap").font(.system(.caption2, design: .monospaced)).foregroundStyle(Theme.textMuted)
                        } else {
                            Button { selection = Set(numbers) } label: {
                                HStack {
                                    Text("\(slot)").frame(width: 28, alignment: .trailing)
                                    Text(numbers.joined(separator: ",")).frame(width: 70, alignment: .leading).lineLimit(1)
                                    Text(spec.pins.first { $0.number == numbers[0] }?.name ?? "?").lineLimit(1)
                                    Spacer()
                                }
                                .font(.system(.caption2, design: .monospaced))
                                .foregroundStyle(numbers.contains { selection.contains($0) } ? Theme.selection : Theme.textSecondary)
                                .contentShape(Rectangle())
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
        }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack {
            let errors = issues.filter(\.isError).count
            let spots = Set(draft.placements.map { "\($0.side.rawValue)\($0.slot)" }).count
            Text("\(spec.pins.count) pins on \(spots) spots" + (errors > 0 ? " · \(errors) errors" : ""))
                .font(.caption).foregroundStyle(errors > 0 ? Theme.error : Theme.textMuted)
            if let arrangeError { Text(arrangeError).font(.caption).foregroundStyle(Theme.error) }
            Spacer()
            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Button("Apply Symbol") {
                onApply(draft.applied(to: spec))
                dismiss()
            }
            .keyboardShortcut(.defaultAction)
            .buttonStyle(.borderedProminent)
            .disabled(errors > 0 || spec.pins.isEmpty)
            .help(errors > 0 ? "Fix the errors (unplaced or colliding pins) first" : "Use this symbol for the part")
        }
        .padding(10)
    }

    // MARK: - Editing

    /// Applies an edit with an undo snapshot (none when nothing changed).
    private func edit(_ change: (inout SymbolDraft) -> Void) {
        var next = draft
        change(&next)
        guard next != draft else { return }
        history.record(draft)
        draft = next
    }

    private func nudge(_ delta: Int) {
        let numbers = selectedNumbers
        // Move each selected stack once, in the direction's order so neighbours don't swap back.
        var done = Set<String>()
        let ordered = numbers.sorted { (draft.placement(of: $0)?.slot ?? 0) < (draft.placement(of: $1)?.slot ?? 0) }
        let sequence = delta < 0 ? ordered : Array(ordered.reversed())
        edit { d in
            for n in sequence where !done.contains(n) {
                d.stack(of: n).forEach { done.insert($0) }
                d.nudge(n, by: delta)
            }
        }
    }

    private func autoArrange() {
        arrangeError = nil
        switch EDAEngine.autoArrangeSymbol(spec, stack: stackDuplicates) {
        case .success(let arranged):
            edit { $0 = SymbolDraft(spec: arranged) }
        case .failure(let error):
            arrangeError = error.localizedDescription
        }
    }

    /// Lays the symbol out in the core and checks it; keeps the last good preview while the layout has errors.
    private func refresh() {
        let edited = draft.applied(to: spec)
        issues = EDAEngine.checkSymbol(edited)
        if !issues.contains(where: \.isError), case .success(let part) = EDAEngine.previewCustomPart(edited) {
            preview = part
        } else if preview == nil, case .success(let part) = EDAEngine.previewCustomPart(spec) {
            preview = part
        }
    }
}
