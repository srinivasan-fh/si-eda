import SwiftUI

/// Length rules (a net's target length) and match groups (nets whose lengths match the longest), measured pad to pad
/// through series parts (xSignals), with a gauge per net. The Tune Length tool and the DRC use them.
struct LengthRulesPanel: View {
    @EnvironmentObject private var store: DesignStore

    @State private var targets = LengthTargets.empty
    @State private var ruleNet = ""
    @State private var ruleTarget = "25"
    @State private var ruleTolerance = "0.1"
    @State private var groupName = ""
    @State private var groupNets: Set<String> = []
    @State private var groupTolerance = "0.1"

    private var signalNets: [String] {
        store.snapshot.nets.filter { $0.netRole == .signal && $0.pinCount >= 2 }.map(\.name).sorted()
    }

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                VStack(alignment: .leading, spacing: 8) {
                    Label("Length Rules", systemImage: "ruler").font(.headline).foregroundStyle(Theme.textPrimary)
                    ForEach(targets.rules) { rule in
                        HStack(spacing: 8) {
                            Text(verbatim: rule.net).foregroundStyle(Theme.textPrimary).frame(width: 110, alignment: .leading)
                            LengthGaugeBar(length: rule.length, target: rule.target, tolerance: rule.tolerance, routed: rule.routed)
                            Text(verbatim: String(format: "%.2f / %.2f ± %.2f mm", rule.length, rule.target, rule.tolerance))
                                .monospacedDigit().font(.caption).foregroundStyle(Theme.textSecondary)
                            Button { store.setLengthRule(net: rule.net, target: 0, tolerance: 0); reload() } label: {
                                Image(systemName: "trash")
                            }
                            .buttonStyle(.borderless)
                            .help("Remove the length rule")
                        }
                    }
                    HStack(spacing: 8) {
                        Picker("Net", selection: $ruleNet) {
                            if !signalNets.contains(ruleNet) { Text("Choose a net").tag(ruleNet) }
                            ForEach(signalNets, id: \.self) { Text(verbatim: $0).tag($0) }
                        }
                        .frame(width: 160)
                        Text("Target").foregroundStyle(Theme.textMuted)
                        TextField("Target", text: $ruleTarget).textFieldStyle(.blue).frame(width: 56)
                        Text(verbatim: "±").foregroundStyle(Theme.textMuted)
                        TextField("Tolerance", text: $ruleTolerance).textFieldStyle(.blue).frame(width: 48)
                        Text(verbatim: "mm").font(.caption).foregroundStyle(Theme.textMuted)
                        Button("Set") {
                            store.setLengthRule(net: ruleNet, target: number(ruleTarget), tolerance: number(ruleTolerance))
                            reload()
                        }
                        .disabled(!signalNets.contains(ruleNet) || number(ruleTarget) <= 0)
                    }
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .bluePanel()

                VStack(alignment: .leading, spacing: 8) {
                    Label("Match Groups", systemImage: "equal.square").font(.headline).foregroundStyle(Theme.textPrimary)
                    ForEach(targets.groups) { group in
                        VStack(alignment: .leading, spacing: 4) {
                            HStack {
                                Text(verbatim: group.name).font(.subheadline.weight(.semibold))
                                Text(verbatim: String(format: "%.2f mm ± %.2f", group.target, group.tolerance))
                                    .font(.caption).monospacedDigit().foregroundStyle(Theme.textMuted)
                                Spacer()
                                Button { store.setMatchGroup(name: group.name, nets: [], tolerance: 0); reload() } label: {
                                    Image(systemName: "trash")
                                }
                                .buttonStyle(.borderless)
                                .help("Remove the match group")
                            }
                            ForEach(group.members) { member in
                                HStack(spacing: 8) {
                                    Text(verbatim: member.xsignal.joined(separator: " → "))
                                        .font(.caption).frame(width: 160, alignment: .leading).lineLimit(1)
                                    LengthGaugeBar(length: member.length, target: group.target,
                                                   tolerance: group.tolerance, routed: member.routed)
                                    Text(verbatim: String(format: "%.2f mm", member.length))
                                        .monospacedDigit().font(.caption).foregroundStyle(Theme.textSecondary)
                                }
                            }
                        }
                    }
                    HStack(spacing: 8) {
                        TextField("Group name", text: $groupName).textFieldStyle(.blue).frame(width: 120)
                        Text(verbatim: "±").foregroundStyle(Theme.textMuted)
                        TextField("Tolerance", text: $groupTolerance).textFieldStyle(.blue).frame(width: 48)
                        Text(verbatim: "mm").font(.caption).foregroundStyle(Theme.textMuted)
                        Button("Add Group") {
                            store.setMatchGroup(name: groupName, nets: groupNets.sorted(), tolerance: number(groupTolerance))
                            groupNets = []
                            groupName = ""
                            reload()
                        }
                        .disabled(groupName.trimmingCharacters(in: .whitespaces).isEmpty || groupNets.count < 2)
                    }
                    Text("Members: pick two or more nets. A net through series resistors or capacitors is measured pad to pad across them.")
                        .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
                    ScrollView {
                        LazyVStack(alignment: .leading, spacing: 2) {
                            ForEach(signalNets, id: \.self) { net in
                                Toggle(isOn: Binding(get: { groupNets.contains(net) },
                                                     set: { if $0 { groupNets.insert(net) } else { groupNets.remove(net) } })) {
                                    Text(verbatim: net)
                                }
                                .toggleStyle(.checkbox)
                            }
                        }
                    }
                    .frame(height: 120)
                }
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .bluePanel()
            }
            .padding(14)
        }
        .frame(width: 520, height: 520)
        .onAppear { reload() }
        .onChange(of: store.revision) { _, _ in reload() }
    }

    private func reload() { targets = store.engine.lengthTargets() }

    private func number(_ text: String) -> Double {
        Double(text.replacingOccurrences(of: ",", with: ".")) ?? 0
    }
}

/// A length gauge: the bar fills towards the target; the tolerance band is marked; green inside it, amber short,
/// red long.
struct LengthGaugeBar: View {
    var length: Double
    var target: Double
    var tolerance: Double
    var routed = true

    var body: some View {
        Canvas { ctx, size in
            LengthGauge.draw(in: &ctx, rect: CGRect(origin: .zero, size: size), length: routed ? length : 0, target: target,
                             tolerance: tolerance)
        }
        .frame(width: 120, height: 10)
        .accessibilityElement()
        .accessibilityLabel(Text(verbatim: String(format: "%.2f of %.2f mm", length, target)))
    }
}

/// Drawing of the length gauge, shared by the panel and the canvas (Tune tool and routing banner).
enum LengthGauge {
    static func colour(length: Double, target: Double, tolerance: Double) -> Color {
        let delta = length - target
        if abs(delta) <= max(tolerance, 0.01) + 1e-9 { return Theme.success }
        return delta < 0 ? Theme.warning : Theme.error
    }

    /// The bar spans 0 … 1.25 × target; the tolerance band is a lighter block around the target mark.
    static func draw(in ctx: inout GraphicsContext, rect: CGRect, length: Double, target: Double, tolerance: Double) {
        guard target > 0, rect.width > 4 else { return }
        let scale = Double(rect.width) / (target * 1.25)
        ctx.fill(Path(roundedRect: rect, cornerRadius: rect.height / 2), with: .color(Theme.textMuted.opacity(0.25)))
        let band = CGRect(x: rect.minX + CGFloat((target - tolerance) * scale), y: rect.minY,
                          width: CGFloat(max(2 * tolerance * scale, 2)), height: rect.height)
        ctx.fill(Path(band), with: .color(Theme.success.opacity(0.35)))
        let fill = CGRect(x: rect.minX, y: rect.minY, width: CGFloat(min(max(0, length), target * 1.25) * scale),
                          height: rect.height)
        ctx.fill(Path(roundedRect: fill, cornerRadius: rect.height / 2),
                 with: .color(colour(length: length, target: target, tolerance: tolerance)))
        let mark = rect.minX + CGFloat(target * scale)
        var tick = Path()
        tick.move(to: CGPoint(x: mark, y: rect.minY - 2))
        tick.addLine(to: CGPoint(x: mark, y: rect.maxY + 2))
        ctx.stroke(tick, with: .color(Theme.textPrimary), lineWidth: 1.5)
    }
}
