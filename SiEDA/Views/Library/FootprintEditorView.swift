import AppKit
import SwiftUI

/// Footprint Editor (Altium "PCB Library" / KiCad footprint editor): draw a part's land pattern pad by pad. Pads are
/// numbered in order; each connects to the pin with its number, or to a chosen pin (tab, exposed pad, a second pad on
/// a pin) or to none (mechanical). Opens from the Component Library on any part — a generated footprint converts to
/// an editable one first — and writes the result back into the part as a "CUSTOM" land pattern.
struct FootprintEditorView: View {
    /// The part being edited (its pins name the pads); the result goes to `onApply`.
    let spec: CustomPartSpec
    let onApply: (CustomPartSpec) -> Void
    @Environment(\.dismiss) private var dismiss

    @State private var draft: FootprintDraft
    @State private var history = FootprintHistory()
    @State private var selection: Set<FootprintDraft.Pad.ID> = []
    @State private var grid: Double = 0.05
    @State private var zoom: Double = 1
    @State private var dragStart: CGPoint?
    @State private var dragMoved = CGSize.zero
    @State private var issues: [LandIssue] = []
    @State private var arrayCount = 4
    @State private var arrayPitch = 1.27
    @State private var arrayHorizontal = false
    @State private var checkTask: Task<Void, Never>?

    static let grids: [Double] = [0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 0.635, 1.0, 1.27, 2.54]

    init(spec: CustomPartSpec, onApply: @escaping (CustomPartSpec) -> Void) {
        self.spec = spec
        self.onApply = onApply
        _draft = State(initialValue: FootprintDraft(spec: spec))
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
                    .frame(width: 300)
                    .background(Theme.deepBlue.opacity(0.45))
            }
            Divider()
            footer
        }
        .frame(minWidth: 900, minHeight: 620)
        .background(Theme.navy)
        .onAppear { runChecks() }
        .onChange(of: draft) { _, _ in scheduleChecks() }
        .onDeleteCommand {
            let ids = selection
            edit { $0.delete(ids) }
            selection = []
        }
    }

    // MARK: - Toolbar

    private var toolbar: some View {
        HStack(spacing: 8) {
            Text("FOOTPRINT EDITOR · \(spec.name.isEmpty ? "untitled" : spec.name)")
                .font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Spacer()
            Button { addPad() } label: { Label("Add Pad", systemImage: "plus.square") }
                .help("Add a pad like the last one (numbered next)")
            Button {
                let offset = grid * 10, ids = selection
                selection = editMaking { $0.duplicate(ids, dx: offset, dy: 0) }
            } label: { Label("Duplicate", systemImage: "plus.square.on.square") }
                .disabled(selection.isEmpty)
            Button(role: .destructive) {
                let ids = selection
                edit { $0.delete(ids) }
                selection = []
            } label: { Label("Delete", systemImage: "trash") }
                .disabled(selection.isEmpty)
            Button {
                let ids = selection
                edit { $0.mirrorX(ids) }
            } label: { Label("Mirror", systemImage: "arrow.left.and.right.righttriangle.left.righttriangle.right") }
                .disabled(selection.isEmpty)
                .help("Mirror the selected pads left ↔ right")
            Button { edit { $0.centre() } } label: { Label("Centre", systemImage: "scope") }
                .help("Move the footprint origin to the centre of the pads")
            Divider().frame(height: 18)
            Button { _ = history.undo(&draft) } label: { Image(systemName: "arrow.uturn.backward") }
                .disabled(!history.canUndo)
                .help("Undo the last footprint edit")
                .accessibilityLabel("Undo footprint edit")
            Button { _ = history.redo(&draft) } label: { Image(systemName: "arrow.uturn.forward") }
                .disabled(!history.canRedo)
                .help("Redo")
                .accessibilityLabel("Redo footprint edit")
            Divider().frame(height: 18)
            Picker("Grid", selection: $grid) {
                ForEach(Self.grids, id: \.self) { Text(String(format: "%g mm", $0)).tag($0) }
            }
            .frame(width: 130)
            Button { zoom = max(0.25, zoom / 1.4) } label: { Image(systemName: "minus.magnifyingglass") }
                .accessibilityLabel("Zoom out")
            Button { zoom = 1 } label: { Image(systemName: "arrow.up.left.and.down.right.magnifyingglass") }
                .accessibilityLabel("Zoom to fit")
            Button { zoom = min(16, zoom * 1.4) } label: { Image(systemName: "plus.magnifyingglass") }
                .accessibilityLabel("Zoom in")
        }
        .buttonStyle(.bordered)
        .controlSize(.small)
        .padding(10)
    }

    // MARK: - Canvas

    /// Millimetres → points: fit the courtyard (with margin), times the zoom.
    private func scale(for size: CGSize) -> Double {
        let court = draft.courtyard
        let w = max(court.width, 2) + 2, h = max(court.height, 2) + 2
        return min(size.width / w, size.height / h) * zoom
    }

    private func toView(_ p: CGPoint, _ size: CGSize) -> CGPoint {
        let s = scale(for: size)
        return CGPoint(x: size.width / 2 + p.x * s, y: size.height / 2 + p.y * s)
    }

    private func toModel(_ p: CGPoint, _ size: CGSize) -> CGPoint {
        let s = scale(for: size)
        return CGPoint(x: (p.x - size.width / 2) / s, y: (p.y - size.height / 2) / s)
    }

    private var issuePads: Set<Int> { Set(issues.flatMap(\.pads)) }

    private var canvas: some View {
        GeometryReader { geo in
            let size = geo.size
            Canvas { ctx, _ in
                let s = scale(for: size)
                let t = CGAffineTransform(translationX: size.width / 2, y: size.height / 2).scaledBy(x: s, y: s)
                // Grid (only while its lines stay ≥ 6 pt apart), axes through the origin.
                let step = grid * s >= 6 ? grid : (grid * 10 * s >= 6 ? grid * 10 : 0)
                if step > 0 {
                    let half = CGPoint(x: size.width / 2 / s, y: size.height / 2 / s)
                    var lines = Path()
                    var x = (-half.x / step).rounded(.down) * step
                    while x <= half.x { lines.move(to: CGPoint(x: x, y: -half.y)); lines.addLine(to: CGPoint(x: x, y: half.y)); x += step }
                    var y = (-half.y / step).rounded(.down) * step
                    while y <= half.y { lines.move(to: CGPoint(x: -half.x, y: y)); lines.addLine(to: CGPoint(x: half.x, y: y)); y += step }
                    ctx.stroke(lines.applying(t), with: .color(Theme.gridDot.opacity(0.5)), lineWidth: 0.5)
                }
                var axes = Path()
                axes.move(to: CGPoint(x: -1, y: 0)); axes.addLine(to: CGPoint(x: 1, y: 0))
                axes.move(to: CGPoint(x: 0, y: -1)); axes.addLine(to: CGPoint(x: 0, y: 1))
                ctx.stroke(axes.applying(t), with: .color(Theme.skyBlue.opacity(0.6)), lineWidth: 1)
                // Body and courtyard.
                let body = CGRect(x: -draft.bodyW / 2, y: -draft.bodyD / 2, width: draft.bodyW, height: draft.bodyD)
                ctx.fill(Path(body).applying(t), with: .color(Theme.boardFill.opacity(0.8)))
                ctx.stroke(Path(body).applying(t), with: .color(Theme.silkscreen.opacity(0.8)), lineWidth: 1)
                ctx.stroke(Path(draft.courtyard).applying(t), with: .color(Theme.lightBlue.opacity(0.4)),
                           style: StrokeStyle(lineWidth: 1, dash: [4, 3]))
                // Pads (selected ones follow the drag preview).
                let flagged = issuePads
                let preview = CGSize(width: dragMoved.width / s, height: dragMoved.height / s)
                for (index, pad) in draft.pads.enumerated() {
                    var l = pad.land
                    let selected = selection.contains(pad.id)
                    if selected {
                        l.x = FootprintDraft.snap(l.x + preview.width, grid)
                        l.y = FootprintDraft.snap(l.y + preview.height, grid)
                    }
                    let r = CGRect(x: l.x - l.w / 2, y: l.y - l.h / 2, width: l.w, height: l.h)
                    let shape = l.round ? Path(ellipseIn: r) : Path(roundedRect: r, cornerRadius: min(l.w, l.h) * 0.12)
                    let mechanical = l.pin == "-"
                    ctx.fill(shape.applying(t), with: .color(mechanical ? Theme.via.opacity(0.6) : Theme.pad))
                    if l.drill > 0 {
                        let d = l.drill
                        ctx.fill(Path(ellipseIn: CGRect(x: l.x - d / 2, y: l.y - d / 2, width: d, height: d)).applying(t),
                                 with: .color(Theme.pcbBackground))
                    }
                    if flagged.contains(index + 1) {
                        ctx.stroke(shape.applying(t), with: .color(Theme.error), lineWidth: 2)
                    }
                    if selected {
                        ctx.stroke(shape.applying(t), with: .color(Theme.selection), lineWidth: 2)
                    }
                    if min(l.w, l.h) * s > 14 {
                        let label = draft.pinNumber(of: index).map { $0 == String(index + 1) ? $0 : "\(index + 1)→\($0)" }
                            ?? "\(index + 1)"
                        ctx.draw(Text(label).font(.system(size: min(11, min(l.w, l.h) * s * 0.4), weight: .bold, design: .monospaced))
                                    .foregroundColor(Theme.navy), at: CGPoint(x: l.x, y: l.y).applying(t))
                    }
                }
                // Pin-1 marker.
                if let first = draft.pads.first?.land {
                    let m = CGPoint(x: first.x + (first.x <= 0 ? -1 : 1) * (first.w / 2 + 0.4), y: first.y).applying(t)
                    ctx.fill(Path(ellipseIn: CGRect(x: m.x - 3, y: m.y - 3, width: 6, height: 6)), with: .color(Theme.silkscreen))
                }
            }
            .contentShape(Rectangle())
            .gesture(
                DragGesture(minimumDistance: 0)
                    .onChanged { value in
                        if dragStart == nil {
                            dragStart = value.startLocation
                            let hit = draft.pad(at: toModel(value.startLocation, size))
                            let extend = NSEvent.modifierFlags.contains(.shift) || NSEvent.modifierFlags.contains(.command)
                            if let hit {
                                if extend { selection.formSymmetricDifference([hit]) }
                                else if !selection.contains(hit) { selection = [hit] }
                            } else if !extend {
                                selection = []
                            }
                        }
                        if !selection.isEmpty, draft.pad(at: toModel(value.startLocation, size)) != nil {
                            dragMoved = value.translation
                        }
                    }
                    .onEnded { _ in
                        let s = scale(for: size)
                        if abs(dragMoved.width) + abs(dragMoved.height) > 1 {
                            let dx = dragMoved.width / s, dy = dragMoved.height / s, ids = selection, step = grid
                            edit { $0.move(ids, dx: dx, dy: dy, grid: step) }
                        }
                        dragStart = nil
                        dragMoved = .zero
                    }
            )
            .simultaneousGesture(
                SpatialTapGesture(count: 2).onEnded { value in
                    // Double-click on empty board adds a pad there.
                    let point = toModel(value.location, size)
                    guard draft.pad(at: point) == nil else { return }
                    let step = grid
                    selection = editMaking { [$0.addPad(at: point, grid: step)] }
                }
            )
            .overlay(alignment: .bottomLeading) {
                Text("Click a pad to select (⇧ to add) · drag to move on the grid · double-click to add a pad · ⌫ deletes")
                    .font(.caption2).foregroundStyle(Theme.textMuted).padding(8)
            }
            .accessibilityElement()
            .accessibilityLabel("Footprint canvas")
            .accessibilityValue("\(draft.pads.count) pads, \(selection.count) selected")
        }
        .background(Theme.pcbBackground)
    }

    // MARK: - Side panel

    private var selectedIndex: Int? {
        guard selection.count == 1, let id = selection.first else { return nil }
        return draft.pads.firstIndex { $0.id == id }
    }

    private var sidePanel: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let index = selectedIndex {
                padProperties(index)
            } else if selection.count > 1 {
                Text("\(selection.count) pads selected — drag, duplicate, mirror or delete them together.")
                    .font(.caption).foregroundStyle(Theme.textSecondary)
            } else {
                Text("Select a pad to edit its position, size, shape, drill and pin.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            Divider()
            arrayTool
            Divider()
            bodyFields
            Divider()
            issueList
            Divider()
            padTable
        }
    }

    private func number(_ title: String, _ value: Binding<Double>, step: Double) -> some View {
        HStack {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted).frame(width: 62, alignment: .leading)
            TextField(title, value: value, format: .number.precision(.fractionLength(0...4)))
                .textFieldStyle(.blue)
                .frame(width: 90)
            Stepper("", value: value, step: step).labelsHidden()
            Text("mm").font(.caption2).foregroundStyle(Theme.textMuted)
        }
    }

    /// A binding to one land field that records undo once per edit.
    private func landBinding(_ index: Int, _ key: WritableKeyPath<CustomPartSpec.Land, Double>, min lower: Double = -60,
                             max upper: Double = 60) -> Binding<Double> {
        Binding(
            get: { index < draft.pads.count ? draft.pads[index].land[keyPath: key] : 0 },
            set: { value in
                guard index < draft.pads.count, value.isFinite else { return }
                let clamped = Swift.min(Swift.max(value, lower), upper)
                guard clamped != draft.pads[index].land[keyPath: key] else { return }
                edit { $0.pads[index].land[keyPath: key] = clamped }
            })
    }

    private func padProperties(_ index: Int) -> some View {
        let pad = draft.pads[index]
        return VStack(alignment: .leading, spacing: 6) {
            Text("PAD \(index + 1)").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            number("X", landBinding(index, \.x), step: grid)
            number("Y", landBinding(index, \.y), step: grid)
            number("Width", landBinding(index, \.w, min: 0.06, max: 30), step: grid)
            number("Height", landBinding(index, \.h, min: 0.06, max: 30), step: grid)
            number("Drill", landBinding(index, \.drill, min: 0, max: 29), step: 0.05)
            Picker("Shape", selection: Binding(get: { pad.land.round }, set: { round in edit { $0.pads[index].land.round = round } })) {
                Text("Rectangle").tag(false)
                Text(pad.land.w == pad.land.h ? "Circle" : "Oval").tag(true)
            }
            .pickerStyle(.segmented)
            Picker("Pin", selection: Binding(get: { pad.land.pin }, set: { pin in edit { $0.pads[index].land.pin = pin } })) {
                Text("Pin \(index + 1) (its number)").tag("")
                ForEach(spec.pins.filter { $0.number != String(index + 1) }, id: \.number) { pin in
                    Text("Pin \(pin.number) · \(pin.name)").tag(pin.number)
                }
                Text("None (mechanical)").tag("-")
            }
            if let name = spec.pins.first(where: { $0.number == (draft.pinNumber(of: index) ?? "") })?.name {
                Text("Connects to \(name)").font(.caption2).foregroundStyle(Theme.textSecondary)
            }
            Stepper("Number \(index + 1)", value: Binding(get: { index + 1 }, set: { n in
                edit { $0.setNumber(of: pad.id, to: n) }
            }), in: 1...max(1, draft.pads.count))
            .font(.caption)
            .help("Move the pad in the numbering; pads between shift by one")
        }
    }

    private var arrayTool: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("PAD ARRAY").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Stepper("Copies: \(arrayCount)", value: $arrayCount, in: 1...128).font(.caption)
            number("Pitch", $arrayPitch, step: 0.05)
            Picker("", selection: $arrayHorizontal) {
                Text("Down ↓").tag(false)
                Text("Right →").tag(true)
            }
            .pickerStyle(.segmented)
            .labelsHidden()
            Button("Add Row from Pad \(selectedIndex.map { String($0 + 1) } ?? "–")") {
                guard let id = selection.first else { return }
                let pitch = arrayPitch, count = arrayCount, horizontal = arrayHorizontal
                selection = editMaking { $0.array(from: id, count: count, pitch: pitch, horizontal: horizontal) }
            }
            .disabled(selectedIndex == nil)
            .controlSize(.small)
        }
    }

    private var bodyFields: some View {
        VStack(alignment: .leading, spacing: 6) {
            Text("BODY").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            number("Width", Binding(get: { draft.bodyW }, set: { v in edit { $0.bodyW = Swift.min(Swift.max(v, 0.5), 60) } }), step: 0.1)
            number("Length", Binding(get: { draft.bodyD }, set: { v in edit { $0.bodyD = Swift.min(Swift.max(v, 0.5), 60) } }), step: 0.1)
        }
    }

    private var issueList: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text("CHECKS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            if issues.isEmpty {
                Label("No overlaps, gaps ≥ 0.1 mm, every pin has a pad", systemImage: "checkmark.seal.fill")
                    .font(.caption).foregroundStyle(Theme.liveOn)
            }
            ForEach(issues) { issue in
                Button {
                    selection = Set(issue.pads.compactMap { n in n >= 1 && n <= draft.pads.count ? draft.pads[n - 1].id : nil })
                } label: {
                    Label(issue.message, systemImage: issue.isError ? "xmark.octagon.fill" : "exclamationmark.triangle.fill")
                        .font(.caption)
                        .foregroundStyle(issue.isError ? Theme.error : Theme.warning)
                        .frame(maxWidth: .infinity, alignment: .leading)
                }
                .buttonStyle(.plain)
            }
        }
    }

    private var padTable: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text("PADS (\(draft.pads.count))").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            ForEach(Array(draft.pads.enumerated()), id: \.element.id) { index, pad in
                let pin = draft.pinNumber(of: index)
                Button {
                    selection = [pad.id]
                } label: {
                    HStack {
                        Text("\(index + 1)").frame(width: 30, alignment: .trailing)
                        Text(pin.map { p in spec.pins.first { $0.number == p }?.name ?? "pin \(p)?" } ?? "mech")
                            .frame(width: 70, alignment: .leading)
                            .lineLimit(1)
                        Text(String(format: "%.2f, %.2f", pad.land.x, pad.land.y))
                        Spacer()
                        Text(String(format: "%.2f×%.2f", pad.land.w, pad.land.h))
                    }
                    .font(.system(.caption2, design: .monospaced))
                    .foregroundStyle(selection.contains(pad.id) ? Theme.selection : Theme.textSecondary)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    // MARK: - Footer

    private var footer: some View {
        HStack {
            let errors = issues.filter(\.isError).count
            Text("\(draft.pads.count) pads · \(spec.pins.count) pins" + (errors > 0 ? " · \(errors) errors" : ""))
                .font(.caption).foregroundStyle(errors > 0 ? Theme.error : Theme.textMuted)
            Spacer()
            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Button("Apply Footprint") {
                onApply(draft.applied(to: spec))
                dismiss()
            }
            .keyboardShortcut(.defaultAction)
            .buttonStyle(.borderedProminent)
            .disabled(draft.pads.isEmpty || errors > 0)
            .help(errors > 0 ? "Fix the errors (overlapping pads, pins without a pad) first" : "Use this footprint for the part")
        }
        .padding(10)
    }

    // MARK: - Editing

    /// Applies an edit with an undo snapshot (none when nothing changed).
    private func edit(_ change: (inout FootprintDraft) -> Void) {
        var next = draft
        change(&next)
        guard next != draft else { return }
        history.record(draft)
        draft = next
    }

    /// `edit` for operations that return the pads they made.
    private func editMaking(_ change: (inout FootprintDraft) -> Set<FootprintDraft.Pad.ID>) -> Set<FootprintDraft.Pad.ID> {
        var made = Set<FootprintDraft.Pad.ID>()
        edit { made = change(&$0) }
        return made
    }

    private func addPad() {
        let last = draft.pads.last?.land
        let point = CGPoint(x: last?.x ?? 0, y: (last?.y ?? 0) + (last.map { $0.h + 0.5 } ?? 0))
        let step = grid
        selection = editMaking { [$0.addPad(at: point, grid: step)] }
    }

    private func scheduleChecks() {
        checkTask?.cancel()
        checkTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            runChecks()
        }
    }

    private func runChecks() {
        issues = EDAEngine.checkLandPattern(draft.applied(to: spec))
    }
}
