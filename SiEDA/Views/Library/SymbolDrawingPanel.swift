import SwiftUI

/// The Symbol Editor's drawing tools (Altium's symbol graphics): pick a tool and drag on the canvas to draw a line,
/// rectangle, circle, arc or polygon, or click to place a text; select a drawing (on the canvas or in the list) to
/// change its fill, line width, text, size, radius or angles, nudge it, or delete it. "Body box" turns the generated
/// rectangle off so the drawings are the symbol's body.
struct SymbolDrawingPanel: View {
    @Binding var draft: SymbolDraft
    @Binding var tool: CustomPartSpec.SymbolGraphic.Kind?
    @Binding var selected: Int?
    let edit: ((inout SymbolDraft) -> Void) -> Void

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("DRAWING").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            toolRow
            Toggle("Body box", isOn: Binding(get: { draft.body }, set: { on in edit { $0.body = on } }))
                .toggleStyle(.checkbox)
                .font(.caption)
                .help("Draw the generated rectangle; turn it off when the drawings are the body")
            list
            if let i = selected, draft.graphics.indices.contains(i) {
                inspector(i)
            }
        }
    }

    private var toolRow: some View {
        HStack(spacing: 4) {
            Button { tool = nil } label: { Image(systemName: "cursorarrow") }
                .help("Select and move pins and drawings")
                .accessibilityLabel("Select")
                .background(tool == nil ? Theme.blue.opacity(0.35) : Color.clear, in: RoundedRectangle(cornerRadius: 4))
            ForEach(CustomPartSpec.SymbolGraphic.Kind.allCases) { kind in
                Button { tool = kind } label: { Image(systemName: kind.systemImage) }
                    .help(LocalizedStringKey(kind.title))
                    .accessibilityLabel(LocalizedStringKey(kind.title))
                    .background(tool == kind ? Theme.blue.opacity(0.35) : Color.clear, in: RoundedRectangle(cornerRadius: 4))
            }
        }
        .buttonStyle(.borderless)
        .controlSize(.small)
    }

    private var list: some View {
        VStack(alignment: .leading, spacing: 2) {
            if draft.graphics.isEmpty {
                Text("No drawings yet: pick a tool and drag on the symbol.")
                    .font(.caption2).foregroundStyle(Theme.textMuted)
            }
            ForEach(Array(draft.graphics.enumerated()), id: \.offset) { i, g in
                Button { selected = i } label: {
                    HStack {
                        Image(systemName: g.shapeKind.systemImage).frame(width: 16)
                        Text(LocalizedStringKey(g.shapeKind.title))
                        if g.shapeKind == .text { Text(verbatim: g.text ?? "").lineLimit(1).foregroundStyle(Theme.textMuted) }
                        Spacer()
                    }
                    .font(.caption)
                    .foregroundStyle(selected == i ? Theme.selection : Theme.textSecondary)
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
            }
        }
    }

    @ViewBuilder
    private func inspector(_ i: Int) -> some View {
        let g = draft.graphics[i]
        VStack(alignment: .leading, spacing: 6) {
            if g.shapeKind == .text {
                TextField("Text", text: Binding(get: { g.text ?? "" }, set: { v in
                    edit { $0.graphics[i].text = v.isEmpty ? "Text" : String(v.prefix(120)) }
                }))
                .textFieldStyle(.roundedBorder)
                .font(.caption)
                Stepper("Size \(Int(g.size ?? 8))", value: Binding(get: { g.size ?? 8 }, set: { v in edit { $0.graphics[i].size = v } }),
                        in: 2...60, step: 1)
                    .font(.caption)
            } else {
                if g.shapeKind == .rect || g.shapeKind == .circle || g.shapeKind == .polygon {
                    Toggle("Filled", isOn: Binding(get: { g.isFilled }, set: { on in edit { $0.graphics[i].fill = on ? true : nil } }))
                        .toggleStyle(.checkbox)
                        .font(.caption)
                }
                Stepper("Line width \(String(format: "%.1f", g.lineWidth ?? 1))",
                        value: Binding(get: { g.lineWidth ?? 1 }, set: { v in edit { $0.graphics[i].lineWidth = v == 1 ? nil : v } }),
                        in: 0.5...5, step: 0.5)
                    .font(.caption)
            }
            if g.shapeKind == .circle || g.shapeKind == .arc {
                Stepper("Radius \(Int(g.radius ?? 0))", value: Binding(get: { g.radius ?? 0 }, set: { v in edit { $0.graphics[i].radius = v } }),
                        in: 2...400, step: 5)
                    .font(.caption)
            }
            if g.shapeKind == .arc {
                Stepper("Start \(Int(g.startAngle ?? 0))°",
                        value: Binding(get: { g.startAngle ?? 0 }, set: { v in edit { $0.graphics[i].startAngle = v } }), in: -360...360, step: 15)
                    .font(.caption)
                Stepper("End \(Int(g.endAngle ?? 360))°",
                        value: Binding(get: { g.endAngle ?? 360 }, set: { v in edit { $0.graphics[i].endAngle = v } }), in: -360...720, step: 15)
                    .font(.caption)
            }
            HStack(spacing: 4) {
                nudge("arrow.left", dx: -5, dy: 0, i)
                nudge("arrow.right", dx: 5, dy: 0, i)
                nudge("arrow.up", dx: 0, dy: -5, i)
                nudge("arrow.down", dx: 0, dy: 5, i)
                Spacer()
                Button(role: .destructive) {
                    edit { $0.graphics.remove(at: i) }
                    selected = nil
                } label: { Label("Delete", systemImage: "trash") }
                    .help("Delete the selected drawing")
            }
            .controlSize(.small)
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 6).fill(Theme.deepBlue.opacity(0.6)))
    }

    private func nudge(_ image: String, dx: Double, dy: Double, _ i: Int) -> some View {
        Button { edit { $0.graphics[i] = $0.graphics[i].moved(dx: dx, dy: dy) } } label: { Image(systemName: image) }
            .help("Move the drawing half a grid step")
            .accessibilityLabel("Move drawing")
    }
}
