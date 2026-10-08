import Charts
import SwiftUI

/// Power-integrity planning for one rail (Simulation ▸ SI / PI ▸ Power): the regulator model, the plane-pair cavity
/// with every capacitor at its position, the decoupling plan that meets the target, and the IR-drop heat map.
struct PowerPlanningSection: View {
    @EnvironmentObject private var store: DesignStore
    let rail: PDNRail

    @State private var cavity: PDNCavityReport?
    @State private var plan: PDNDecapPlanReport?
    @State private var irMap: PDNIRMapReport?
    @State private var vrmText = ""
    @State private var bandwidthText = ""
    @State private var showDensity = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Divider()
            regulator
            cavitySection
            planSection
            irSection
        }
        .task(id: "\(rail.name)#\(store.revision)") { reload() }
    }

    private func reload() {
        cavity = store.pdnCavity(rail.name)
        plan = store.pdnDecapPlan(rail.name)
        irMap = store.pdnIRMap(rail.name)
        vrmText = String(format: "%g", rail.vrmR * 1000)
        bandwidthText = String(format: "%g", (rail.vrmBandwidth ?? 0) / 1000)
    }

    private func number(_ text: String) -> Double {
        Double(text.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".")) ?? 0
    }

    // MARK: Regulator

    private var regulator: some View {
        HStack(spacing: 10) {
            Text("Regulator").font(.headline).foregroundStyle(Theme.textPrimary)
            Text(verbatim: rail.vrmRef.isEmpty ? rail.vrmKind : "\(rail.vrmRef) · \(rail.vrmKind)")
                .font(.callout).foregroundStyle(Theme.textSecondary)
            Spacer()
            Text("Output resistance (mΩ)").font(.caption).foregroundStyle(Theme.textMuted)
            TextField("", text: $vrmText).textFieldStyle(.blue).frame(width: 60)
            Text("Loop bandwidth (kHz)").font(.caption).foregroundStyle(Theme.textMuted)
            TextField("", text: $bandwidthText).textFieldStyle(.blue).frame(width: 60)
            Button("Apply") {
                store.setPDNRegulator(rail.name, outputOhms: max(0, min(10_000, number(vrmText))) / 1000,
                                      loopBandwidth: max(0, min(100_000, number(bandwidthText))) * 1000)
            }
            .buttonStyle(.borderless)
            .help("0 derives the value from the regulator type")
        }
    }

    // MARK: Cavity

    private struct ZPoint: Identifiable {
        let id: Int
        let series: Int  // 0 cavity, 1 lumped, 2 plane mesh (real pour shape)
        let logF: Double
        let logZ: Double
    }

    @ViewBuilder private var cavitySection: some View {
        Text("Plane cavity").font(.headline).foregroundStyle(Theme.textPrimary)
        if let c = cavity, c.available {
            Text(verbatim: String(format: "%.0f × %.0f mm · %.3f mm · εr %.2f · ", c.a, c.b, c.d, c.er)
                 + c.modes.prefix(4).map { "f\($0.m)\($0.n) " + EngineeringFormat.string($0.f, unit: "Hz", digits: 3) }.joined(separator: " · "))
                .font(.callout.monospacedDigit()).foregroundStyle(Theme.textSecondary)
            cavityChart(c)
            HStack(spacing: 14) {
                legendDot("Cavity model", Theme.probe)
                legendDot("Lumped model", Theme.textMuted)
                if !(c.zPlane ?? []).isEmpty { legendDot("Plane mesh (pour shape)", Theme.skyBlue) }
            }
            ForEach(c.recommendations, id: \.self) { text in
                Label(text, systemImage: "lightbulb").font(.callout).foregroundStyle(Theme.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        } else {
            Text(verbatim: cavity?.note ?? "").font(.caption).foregroundStyle(Theme.textMuted)
        }
    }

    @ViewBuilder private func cavityChart(_ c: PDNCavityReport) -> some View {
        let points: [ZPoint] = {
            var out: [ZPoint] = []
            for (series, values) in [(0, c.zCavity), (1, c.zLumped), (2, c.zPlane ?? [])] {
                for i in 0..<min(c.freq.count, values.count) where c.freq[i] > 0 && values[i] > 0 {
                    out.append(ZPoint(id: out.count, series: series, logF: log10(c.freq[i]), logZ: log10(values[i])))
                }
            }
            return out
        }()
        let target = c.target > 0 ? log10(c.target) : nil
        let zs = points.map(\.logZ) + (target.map { [$0] } ?? [])
        let low = floor(zs.min() ?? -3), high = ceil(zs.max() ?? 1)
        Chart {
            ForEach(points) { p in
                LineMark(x: .value("Frequency", p.logF), y: .value("Impedance", p.logZ), series: .value("Model", p.series))
                    .foregroundStyle(p.series == 0 ? Theme.probe : p.series == 2 ? Theme.skyBlue : Theme.textMuted)
            }
            if let target {
                RuleMark(y: .value("Target", target))
                    .foregroundStyle(Theme.warning)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
        }
        .chartXScale(domain: 6.0...9.5)
        .chartYScale(domain: low...max(high, low + 1))
        .chartXAxis {
            AxisMarks(values: [6.0, 7.0, 8.0, 9.0]) { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let x = value.as(Double.self) {
                        Text(EngineeringFormat.string(pow(10, x), unit: "Hz", digits: 1)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartYAxis {
            AxisMarks(values: Array(stride(from: low, through: max(high, low + 1), by: 1.0))) { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let y = value.as(Double.self) {
                        Text(EngineeringFormat.string(pow(10, y), unit: "Ω", digits: 1)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .frame(minHeight: 200)
    }

    // MARK: Decoupling plan

    @ViewBuilder private var planSection: some View {
        if let p = plan, p.needed {
            Text("Decoupling plan").font(.headline).foregroundStyle(Theme.textPrimary)
            Text(verbatim: String(format: "|Z| / Zt %.2f → %.2f · L ", p.worstBefore, p.worstAfter)
                 + EngineeringFormat.string(p.mounting, unit: "H", digits: 2))
                .font(.callout.monospacedDigit()).foregroundStyle(p.compliant ? Theme.success : Theme.warning)
            ForEach(p.additions) { a in
                Label {
                    Text(verbatim: "\(a.count) × \(a.value) \(a.footprint)")
                } icon: {
                    Image(systemName: "plus.circle")
                }
                .font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
            }
        }
    }

    // MARK: IR-drop map

    @ViewBuilder private var irSection: some View {
        if let m = irMap, m.analyzed {
            HStack {
                Text("IR-drop map").font(.headline).foregroundStyle(Theme.textPrimary)
                Spacer()
                Picker("", selection: $showDensity) {
                    Text("IR drop").tag(false)
                    Text("Current density").tag(true)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 240)
            }
            Text(verbatim: "ΔV " + EngineeringFormat.string(m.worst, unit: "V", digits: 3) + " / "
                 + EngineeringFormat.string(m.limit, unit: "V", digits: 3) + " · J ≤ "
                 + String(format: "%.1f A/mm²", m.maxDensity))
                .font(.callout.monospacedDigit()).foregroundStyle(m.worst > m.limit ? Theme.warning : Theme.textSecondary)
            IRDropMapView(map: m, density: showDensity).frame(height: 280)
            ForEach(Array(m.hotspots.prefix(3).enumerated()), id: \.offset) { _, h in
                Text(verbatim: String(format: "%.1f A/mm² @ (%.1f, %.1f) mm", h.density, h.x, h.y))
                    .font(.caption.monospacedDigit()).foregroundStyle(h.density > 30 ? Theme.warning : Theme.textMuted)
            }
        }
    }
}

/// Heat map of a rail's DC drop (or current density) over the board outline.
struct IRDropMapView: View {
    let map: PDNIRMapReport
    let density: Bool

    /// Blue (none) → amber → red (worst).
    static func heat(_ t: Double) -> Color {
        let x = min(1, max(0, t))
        if x < 0.5 {
            let k = x / 0.5
            return Color(red: 0.20 + 0.80 * k, green: 0.47 + 0.25 * k, blue: 0.96 - 0.66 * k)
        }
        let k = (x - 0.5) / 0.5
        return Color(red: 1.0, green: 0.72 - 0.34 * k, blue: 0.30 + 0.10 * k)
    }

    var body: some View {
        Canvas { context, size in
            let w = max(1, map.board.width), h = max(1, map.board.height)
            let scale = min(size.width / w, size.height / h)
            let ox = (size.width - w * scale) / 2, oy = (size.height - h * scale) / 2
            func p(_ x: Double, _ y: Double) -> CGPoint { CGPoint(x: ox + x * scale, y: oy + y * scale) }
            var outline = Path()
            if let first = map.board.outline.first {
                outline.move(to: p(first.x, first.y))
                for q in map.board.outline.dropFirst() { outline.addLine(to: p(q.x, q.y)) }
                outline.closeSubpath()
            }
            context.fill(outline, with: .color(Theme.boardFill))
            let top = density ? max(1e-9, map.maxDensity) : max(1e-12, map.worst)
            for c in map.cells {
                let v = density ? c.density : c.drop
                let rect = CGRect(x: ox + (c.x - c.size / 2) * scale, y: oy + (c.y - c.size / 2) * scale,
                                  width: c.size * scale + 0.5, height: c.size * scale + 0.5)
                context.fill(Path(rect), with: .color(Self.heat(v / top).opacity(0.85)))
            }
            for s in map.segments {
                var line = Path()
                line.move(to: p(s.ax, s.ay))
                line.addLine(to: p(s.bx, s.by))
                let v = density ? s.density : s.drop
                context.stroke(line, with: .color(Self.heat(v / top)), style: StrokeStyle(lineWidth: max(1.5, s.width * scale), lineCap: .round))
            }
            context.stroke(outline, with: .color(Theme.boardEdge.opacity(0.7)), lineWidth: 1)
            if let src = map.source {
                let r = CGRect(x: p(src.x, src.y).x - 5, y: p(src.x, src.y).y - 5, width: 10, height: 10)
                context.stroke(Path(ellipseIn: r), with: .color(Theme.liveOn), lineWidth: 2)
            }
            for l in map.loads {
                let c = p(l.x, l.y)
                context.fill(Path(ellipseIn: CGRect(x: c.x - 3, y: c.y - 3, width: 6, height: 6)),
                             with: .color(l.connected ? Theme.iceBlue : Theme.error))
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 6))
        .accessibilityLabel(Text("IR-drop map"))
    }
}
