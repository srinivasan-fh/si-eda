import Charts
import SwiftUI

/// Analyses of the Simulation workspace's right-hand panel (the DC operating point always shows on the left).
enum SimulationAnalysis: Hashable {
    case transient, ac, dcSweep, monteCarlo, noise
}

/// Toggle chips that show or hide the series of a chart (same look as the transient chips).
private struct SeriesChips: View {
    var labels: [String]
    @Binding var hidden: Set<String>

    var body: some View {
        ScrollView(.horizontal, showsIndicators: false) {
            HStack {
                ForEach(Array(labels.enumerated()), id: \.offset) { index, label in
                    let isHidden = hidden.contains(label)
                    Button {
                        if isHidden { hidden.remove(label) } else { hidden.insert(label) }
                    } label: {
                        HStack(spacing: 5) {
                            Circle().fill(Theme.seriesColors[index % Theme.seriesColors.count]).frame(width: 8, height: 8)
                            Text(label).font(.caption.monospaced())
                        }
                        .padding(.horizontal, 8).padding(.vertical, 4)
                        .foregroundStyle(isHidden ? Theme.textMuted : Theme.textPrimary)
                        .background(Capsule().fill(Theme.blue.opacity(isHidden ? 0.05 : 0.2)))
                    }
                    .buttonStyle(.plain)
                }
            }
        }
    }
}

private func seriesColors(_ count: Int) -> [Color] {
    (0..<count).map { Theme.seriesColors[$0 % Theme.seriesColors.count] }
}

// MARK: - AC (Bode plot)

/// Bode plot of an AC sweep: magnitude and phase over a logarithmic frequency axis, with the readouts of every shown
/// node (gain, −3 dB bandwidth, resonance peak, unity-gain frequency, phase margin).
struct ACAnalysisPanel: View {
    let result: ACResult
    @State private var hidden: Set<String> = []

    private struct Point: Identifiable {
        let id: Int
        let series: String
        let logF: Double
        let value: Double
    }

    private var labels: [String] { result.nets.map(\.name) }
    private var visible: [ACNetResponse] { result.nets.filter { !hidden.contains($0.name) } }

    /// Whole decades spanned by the sweep (x is log10 of the frequency).
    private var decades: [Double] {
        guard let first = result.frequency.first, let last = result.frequency.last, first > 0, last > first else { return [0, 1] }
        let lo = Int(floor(log10(first))), hi = max(Int(ceil(log10(last))), lo + 1)
        return (lo...hi).map(Double.init)
    }

    private func points(phase: Bool) -> [Point] {
        var out: [Point] = []
        var id = 0
        for net in visible {
            let values = phase ? net.phaseDeg : net.magnitudeDb
            for i in 0..<min(values.count, result.frequency.count) where result.frequency[i] > 0 {
                // A node with no response sits at −400 dB; keep the axis readable.
                out.append(Point(id: id, series: net.name, logF: log10(result.frequency[i]),
                                 value: phase ? values[i] : max(values[i], -200)))
                id += 1
            }
        }
        return out
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text("Input").font(.caption).foregroundStyle(Theme.textMuted)
                Text(result.stimulus.joined(separator: ", ")).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                Spacer()
                Text("\(result.frequency.count) points · \(EngineeringFormat.string(result.frequency.first ?? 0, unit: "Hz")) – \(EngineeringFormat.string(result.frequency.last ?? 0, unit: "Hz"))")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            Text("Magnitude").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            chart(phase: false).frame(minHeight: 140)
            Text("Phase").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            chart(phase: true).frame(minHeight: 110)
            SeriesChips(labels: labels, hidden: $hidden)
            readouts
        }
    }

    private func chart(phase: Bool) -> some View {
        Chart(points(phase: phase)) { p in
            LineMark(x: .value("Frequency", p.logF), y: .value(phase ? "Phase" : "Magnitude", p.value))
                .foregroundStyle(by: .value("Net", p.series))
                .interpolationMethod(.linear)
        }
        .chartForegroundStyleScale(domain: labels, range: seriesColors(labels.count))
        .chartXScale(domain: (decades.first ?? 0)...(decades.last ?? 1))
        .chartXAxis {
            AxisMarks(values: decades) { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let v = value.as(Double.self) {
                        Text(EngineeringFormat.string(pow(10, v), unit: "Hz", digits: 2)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartYAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let v = value.as(Double.self) {
                        Text(String(format: phase ? "%.0f°" : "%.0f dB", v)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartLegend(.hidden)
    }

    private var readouts: some View {
        ScrollView {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
                GridRow {
                    Text("Net")
                    Text("Gain")
                    Text("−3 dB")
                    Text("Peak")
                    Text("Unity gain")
                    Text("Phase margin")
                }
                .font(.caption.weight(.bold))
                .foregroundStyle(Theme.skyBlue)
                ForEach(visible) { net in
                    GridRow {
                        Text(net.name).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                        cell(net.metrics?.lowFreqDb.map { String(format: "%.2f dB", $0) })
                        cell(net.metrics?.f3dbHz.map { EngineeringFormat.string($0, unit: "Hz", digits: 4) })
                        cell(peak(net.metrics))
                        cell(net.metrics?.unityHz.map { EngineeringFormat.string($0, unit: "Hz", digits: 4) })
                        cell(net.metrics?.phaseMarginDeg.map { String(format: "%.1f°", $0) })
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 130)
    }

    /// "12.3 dB @ 5.03 kHz" for a resonance (a peak inside the sweep), else nil.
    private func peak(_ m: ACMetrics?) -> String? {
        guard let m, let db = m.peakDb, let hz = m.peakHz, let low = m.lowFreqDb, db > low + 0.1 else { return nil }
        return String(format: "%.2f dB @ ", db) + EngineeringFormat.string(hz, unit: "Hz", digits: 4)
    }

    private func cell(_ text: String?) -> some View {
        Text(text ?? "—").font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
    }
}

// MARK: - DC sweep

/// Node voltages (or device currents) against the swept source value.
struct DCSweepPanel: View {
    let result: DCSweepResult
    @State private var hidden: Set<String> = []
    @State private var showCurrents = false

    private struct Point: Identifiable {
        let id: Int
        let series: String
        let x: Double
        let y: Double
    }

    private var series: [WaveformSeries] { showCurrents ? result.currents : result.nets }

    private var points: [Point] {
        var out: [Point] = []
        var id = 0
        for s in series where !hidden.contains(s.label) {
            for i in 0..<min(s.values.count, result.values.count) {
                out.append(Point(id: id, series: s.label, x: result.values[i], y: s.values[i]))
                id += 1
            }
        }
        return out
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Picker("", selection: $showCurrents) {
                    Text("Voltages").tag(false)
                    Text("Currents").tag(true)
                }
                .pickerStyle(.segmented)
                .frame(width: 200)
                Spacer()
                Text("\(result.source) · \(EngineeringFormat.string(result.values.first ?? 0, unit: result.unit)) – \(EngineeringFormat.string(result.values.last ?? 0, unit: result.unit))")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            Chart(points) { p in
                LineMark(x: .value("Source", p.x), y: .value(showCurrents ? "Current (A)" : "Voltage (V)", p.y))
                    .foregroundStyle(by: .value("Signal", p.series))
                    .interpolationMethod(.linear)
            }
            .chartForegroundStyleScale(domain: series.map(\.label), range: seriesColors(series.count))
            .chartXAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let v = value.as(Double.self) {
                            Text(EngineeringFormat.string(v, unit: result.unit, digits: 3)).foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .chartYAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let v = value.as(Double.self) {
                            Text(EngineeringFormat.string(v, unit: showCurrents ? "A" : "V", digits: 3))
                                .foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .chartLegend(.hidden)
            .frame(minHeight: 220)
            SeriesChips(labels: series.map(\.label), hidden: $hidden)
        }
    }
}

// MARK: - Monte Carlo

/// Distribution of the measured quantity over the Monte Carlo runs, with the nominal, statistics, worst case and the
/// tolerances used.
struct MonteCarloPanel: View {
    let result: MonteCarloResult

    private struct Bin: Identifiable {
        let id: Int
        let from: Double
        let to: Double
        let count: Int
    }

    private var bins: [Bin] {
        let h = result.histogram
        guard h.edges.count == h.counts.count + 1 else { return [] }
        return h.counts.indices.map { Bin(id: $0, from: h.edges[$0], to: h.edges[$0 + 1], count: h.counts[$0]) }
    }

    private func format(_ v: Double) -> String { EngineeringFormat.string(v, unit: result.unit, digits: 4) }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(result.net).font(.callout.monospaced().weight(.semibold)).foregroundStyle(Theme.textPrimary)
                Spacer()
                Text("Runs").font(.caption).foregroundStyle(Theme.textMuted)
                Text("\(result.runs)").font(.caption.monospacedDigit()).foregroundStyle(Theme.textPrimary)
            }
            Chart {
                ForEach(bins) { b in
                    BarMark(xStart: .value("From", b.from), xEnd: .value("To", b.to), y: .value("Runs", b.count))
                        .foregroundStyle(Theme.blue.opacity(0.8))
                }
                RuleMark(x: .value("Nominal", result.nominal))
                    .foregroundStyle(Theme.warning)
                    .lineStyle(StrokeStyle(lineWidth: 1.5, dash: [4, 3]))
            }
            .chartXAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let v = value.as(Double.self) {
                            Text(format(v)).foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .chartYAxis {
                AxisMarks { _ in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel().foregroundStyle(Theme.textMuted)
                }
            }
            .frame(minHeight: 180)

            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                GridRow {
                    Text("Nominal").foregroundStyle(Theme.textMuted)
                    Text(format(result.nominal)).foregroundStyle(Theme.probe)
                }
                GridRow {
                    Text("Mean").foregroundStyle(Theme.textMuted)
                    Text("\(format(result.mean))  σ \(format(result.sigma))").foregroundStyle(Theme.probe)
                }
                GridRow {
                    Text("Monte Carlo").foregroundStyle(Theme.textMuted)
                    Text("\(format(result.min)) … \(format(result.max))").foregroundStyle(Theme.probe)
                }
                if let wc = result.worstCase {
                    GridRow {
                        Text("Worst case").foregroundStyle(Theme.textMuted)
                        Text("\(format(wc.min)) … \(format(wc.max))").foregroundStyle(Theme.warning)
                    }
                }
            }
            .font(.callout.monospacedDigit())

            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(result.parts) { part in
                        HStack(spacing: 10) {
                            Text(part.ref).frame(width: 50, alignment: .leading).foregroundStyle(Theme.textPrimary)
                            Text(part.value).foregroundStyle(Theme.textSecondary)
                            Text(String(format: "±%.3g %%", part.tolerance * 100))
                                .foregroundStyle(part.fromValue ? Theme.textPrimary : Theme.textMuted)
                            Spacer()
                        }
                        .font(.caption.monospaced())
                    }
                }
            }
            .frame(maxHeight: 110)
        }
    }
}
