import SwiftUI

/// The last autoroute's report (docs/ROUTING.md, Routing report): board metrics and layer usage, what the quality
/// passes did, the pin / gate swaps made before routing, differential pairs (coupled or not, skew) and length targets (achieved against target).
struct RoutingReportSheet: View {
    let report: RouteReport
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Routing Report").font(.headline)
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    metricsBox
                    layersBox
                    if report.metrics.pinSwaps != nil { swapsBox }
                    if !report.pairs.isEmpty { pairsBox }
                    if !report.lengths.isEmpty { lengthsBox }
                }
            }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 520, height: 520)
    }

    private func row(_ title: LocalizedStringKey, _ value: String) -> some View {
        LabeledContent(title) { Text(verbatim: value).monospacedDigit() }
    }

    private var metricsBox: some View {
        let m = report.metrics
        return GroupBox("Board") {
            VStack(alignment: .leading, spacing: 4) {
                row("Unrouted connections", "\(m.unrouted)")
                row("Vias", "\(m.vias)")
                row("Microvias", "\(m.microvias)")
                row("Blind / buried vias", "\(m.blindVias)")
                row("Track length", String(format: "%.1f mm", m.trackLength))
                row("Tracks", "\(m.segments)")
                row("Arcs", "\(m.arcs)")
                row("Teardrops", "\(m.teardrops)")
                row("Vias removed", "\(m.viasRemoved)")
                row("Lines glossed", "\(m.glossed)")
            }
        }
    }

    private var layersBox: some View {
        let total = max(1e-9, report.metrics.trackLength)
        return GroupBox("Layer Usage") {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(report.metrics.layerLength.enumerated()), id: \.offset) { item in
                    layerRow(item.offset, item.element, total)
                }
            }
        }
    }

    private func layerRow(_ layer: Int, _ length: Double, _ total: Double) -> some View {
        HStack {
            Text(verbatim: "L\(layer + 1)").frame(width: 40, alignment: .leading)
            ProgressView(value: length / total).frame(width: 220)
            Text(verbatim: String(format: "%.1f mm", length)).monospacedDigit()
        }
    }

    private var swapsBox: some View {
        let m = report.metrics
        return GroupBox("Pin & Gate Swaps") {
            VStack(alignment: .leading, spacing: 4) {
                row("Pins swapped", "\(m.pinSwaps ?? 0)")
                row("Gates swapped", "\(m.gateSwaps ?? 0)")
                row("Ratsnest before", String(format: "%.1f mm", m.ratsnestBefore ?? 0))
                row("Ratsnest after", String(format: "%.1f mm", m.ratsnestAfter ?? 0))
                row("Crossings", "\(m.crossingsBefore ?? 0) → \(m.crossingsAfter ?? 0)")
                ForEach(report.swaps ?? [], id: \.self) { line in
                    Text(verbatim: line).font(.caption).foregroundStyle(.secondary)
                }
            }
        }
    }

    private var pairsBox: some View {
        GroupBox("Differential Pairs") {
            VStack(alignment: .leading, spacing: 6) {
                ForEach(report.pairs) { pairRow($0) }
            }
        }
    }

    private func pairRow(_ p: RouteReport.Pair) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(verbatim: "\(p.positive) / \(p.negative)").bold()
            if p.coupled {
                Text(verbatim: String(format: "%.1f mm · %.1f mm · %.3f mm · ", p.coupledLength, p.uncoupledLength, p.skew)
                     + "\(p.viaPairs) · " + String(format: "%.2f / %.2f mm", p.width, p.gap))
                    .font(.caption).monospacedDigit()
                Text("Coupled length · uncoupled · skew · via pairs · width / gap").font(.caption2).foregroundStyle(.secondary)
            } else {
                Text("Routed as two nets").font(.caption)
                Text(verbatim: p.reason).font(.caption2).foregroundStyle(.secondary)
            }
        }
    }

    private var lengthsBox: some View {
        GroupBox("Length Targets") {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(report.lengths) { lengthRow($0) }
            }
        }
    }

    private func lengthRow(_ l: RouteReport.Length) -> some View {
        HStack {
            Image(systemName: l.ok ? "checkmark.circle.fill" : "exclamationmark.triangle.fill")
                .foregroundStyle(l.ok ? Color.green : Color.orange)
            Text(verbatim: l.net).frame(width: 120, alignment: .leading)
            Text(verbatim: String(format: "%.2f / %.2f ± %.2f mm", l.achieved, l.target, l.tolerance)).monospacedDigit()
        }
        .help(l.ok ? Text("Within tolerance") : Text("Out of tolerance"))
    }
}
