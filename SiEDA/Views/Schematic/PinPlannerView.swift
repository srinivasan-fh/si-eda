// SiEDA — Pin Planner: an MCU / FPGA package drawn pad by pad, coloured by what each pin's net does (signal, power,
// ground, unused), from the core's pin plan (schematic_pin_map format "plan"). Clicking a pad picks it; a net typed
// for it goes on the pin through the same assignment path as a CubeMX / XDC / QSF file (one undo step).
import SwiftUI

struct PinPlannerView: View {
    let ref: String
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var plan: PinPlan?
    @State private var selected: Int?
    @State private var netText = ""
    @State private var filter = ""
    @State private var message = ""

    struct PinPlan: Decodable {
        struct Pad: Decodable {
            var x, y, w, h: Double
            var round: Bool
            var number, name, type, net, role: String?
        }
        var package: String
        var width, height: Double
        var pads: [Pad]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Pin Planner").font(.headline).foregroundStyle(Theme.skyBlue)
                Text(verbatim: "\(ref) · \(plan?.package ?? "")").foregroundStyle(Theme.textSecondary)
                Spacer()
                TextField("Find pins (name, net)", text: $filter).textFieldStyle(.roundedBorder).frame(width: 200)
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
            HStack(alignment: .top, spacing: 12) {
                package.frame(minWidth: 460, minHeight: 460)
                VStack(alignment: .leading, spacing: 8) {
                    legend
                    Divider()
                    if let i = selected, let pad = plan?.pads[i], let number = pad.number {
                        Text(verbatim: "\(number) · \(pad.name ?? "")").font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                        Text(verbatim: pad.type ?? "").font(.caption).foregroundStyle(Theme.textMuted)
                        TextField("Net", text: $netText).textFieldStyle(.roundedBorder).onSubmit { assign(number) }
                        Button("Assign Net") { assign(number) }.disabled(netText.isEmpty)
                    } else {
                        Text("Click a pad to see its pin and put a net on it.").font(.callout).foregroundStyle(Theme.textMuted)
                    }
                    Text(verbatim: message).font(.caption).foregroundStyle(Theme.textMuted)
                    Spacer()
                }
                .frame(width: 220)
            }
        }
        .padding(16)
        .frame(minWidth: 760, minHeight: 560)
        .background(Theme.navy)
        .onAppear(perform: load)
    }

    private var legend: some View {
        VStack(alignment: .leading, spacing: 4) {
            ForEach([("signal", "Signal"), ("power", "Power"), ("ground", "Ground"), ("none", "Unused")], id: \.0) { role, title in
                Label { Text(LocalizedStringKey(title)) } icon: { RoundedRectangle(cornerRadius: 2).fill(Self.colour(role)).frame(width: 14, height: 10) }
                    .font(.caption)
            }
        }
    }

    static func colour(_ role: String?) -> Color {
        switch role {
        case "signal": return Theme.skyBlue
        case "power", "negative": return Theme.warning
        case "ground": return Theme.textMuted
        default: return Theme.darkBlue
        }
    }

    /// Footprint millimetres → view points: the package fitted with a margin, y down as in the footprint.
    private func transform(_ size: CGSize) -> (scale: CGFloat, origin: CGPoint) {
        let p = plan.map { CGSize(width: max($0.width, 1), height: max($0.height, 1)) } ?? CGSize(width: 1, height: 1)
        let scale = min(size.width / p.width, size.height / p.height) * 0.9
        return (scale, CGPoint(x: size.width / 2, y: size.height / 2))
    }

    private var package: some View {
        GeometryReader { geo in
            let t = transform(geo.size)
            Canvas { ctx, size in
                ctx.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.deepBlue))
                guard let plan else { return }
                let body = CGRect(x: t.origin.x - plan.width * t.scale / 2, y: t.origin.y - plan.height * t.scale / 2,
                                  width: plan.width * t.scale, height: plan.height * t.scale)
                ctx.stroke(Path(roundedRect: body, cornerRadius: 4), with: .color(Theme.textMuted.opacity(0.5)), lineWidth: 1)
                let query = filter.lowercased()
                for (i, pad) in plan.pads.enumerated() {
                    let r = CGRect(x: t.origin.x + (pad.x - pad.w / 2) * t.scale, y: t.origin.y + (pad.y - pad.h / 2) * t.scale,
                                   width: pad.w * t.scale, height: pad.h * t.scale)
                    let shape = pad.round ? Path(ellipseIn: r) : Path(roundedRect: r, cornerRadius: min(r.width, r.height) * 0.15)
                    let hit = !query.isEmpty && ((pad.name ?? "").lowercased().contains(query) || (pad.net ?? "").lowercased().contains(query))
                    ctx.fill(shape, with: .color(Self.colour(pad.role).opacity(query.isEmpty || hit ? 1 : 0.25)))
                    if i == selected { ctx.stroke(shape, with: .color(.white), lineWidth: 2) }
                    if min(r.width, r.height) > 9, let number = pad.number {
                        ctx.draw(Text(verbatim: number).font(.system(size: min(r.width, r.height) * 0.5)).foregroundColor(.black), at: CGPoint(x: r.midX, y: r.midY))
                    }
                }
            }
            .contentShape(Rectangle())
            .onTapGesture(coordinateSpace: .local) { point in pick(point, t) }
            .help(selected.flatMap { plan?.pads[$0] }.map { "\($0.number ?? "") \($0.name ?? "") — \($0.net ?? "")" } ?? "")
        }
    }

    private func pick(_ point: CGPoint, _ t: (scale: CGFloat, origin: CGPoint)) {
        guard let plan else { return }
        let x = (point.x - t.origin.x) / t.scale, y = (point.y - t.origin.y) / t.scale
        selected = plan.pads.indices.filter { plan.pads[$0].number != nil }
            .min { hypot(plan.pads[$0].x - x, plan.pads[$0].y - y) < hypot(plan.pads[$1].x - x, plan.pads[$1].y - y) }
        netText = selected.flatMap { plan.pads[$0].net } ?? ""
    }

    private func load() {
        let r = store.runTool("schematic_pin_map", arguments: ["ref": ref, "format": "plan"])
        struct Reply: Decodable { var plan: PinPlan? }
        plan = (try? JSONDecoder().decode(Reply.self, from: Data(r.text.utf8)))?.plan
        if plan == nil { message = r.isError ? r.text : String(localized: "This part has no footprint to plan.") }
    }

    private func assign(_ number: String) {
        let net = netText.trimmingCharacters(in: .whitespaces)
        guard !net.isEmpty else { return }
        let r = store.runTool("schematic_pin_map", arguments: ["ref": ref, "assign": "pin,net\n\(number),\(net)\n"])
        message = r.isError ? r.text : String(localized: "Net placed on the pin.")
        load()
    }
}
