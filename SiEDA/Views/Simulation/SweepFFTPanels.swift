import Charts
import SwiftUI

// MARK: - Parameter sweep

/// One analysis per value of a part: DC values in a table and chart, AC responses or transient waveforms of the chosen
/// net overlaid, one curve per value.
struct ParamSweepPanel: View {
    let result: ParamSweepResult

    private struct Point: Identifiable {
        let id: Int
        let series: String
        let x: Double
        let y: Double
    }

    private var okRuns: [ParamSweepRunResult] { result.runs.filter(\.ok) }
    private var labels: [String] { okRuns.map(\.value) }
    private var colors: [Color] { labels.indices.map { Theme.seriesColors[$0 % Theme.seriesColors.count] } }
    private var netName: String { okRuns.first?.nets.first?.name ?? "" }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(verbatim: result.component).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                Text("\(result.runs.count) values").font(.caption).foregroundStyle(Theme.textMuted)
                if result.analysis != "dc" && !netName.isEmpty {
                    Text(verbatim: "· \(netName)").font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                }
                Spacer()
            }
            switch result.analysis {
            case "ac": acChart.frame(minHeight: 180)
            case "transient": transientChart.frame(minHeight: 180)
            default: dcChart.frame(minHeight: 140)
            }
            table
        }
    }

    // DC: each net's voltage over the values (categorical axis).
    private var dcChart: some View {
        let nets = Array((okRuns.first?.nets ?? []).prefix(8)).map(\.name)
        var points: [Point] = []
        for (i, run) in okRuns.enumerated() {
            for net in run.nets where nets.contains(net.name) {
                if let v = net.voltage { points.append(Point(id: points.count, series: net.name, x: Double(i), y: v)) }
            }
        }
        return Chart(points) { p in
            LineMark(x: .value("Value", p.x), y: .value("Voltage", p.y)).foregroundStyle(by: .value("Net", p.series))
            PointMark(x: .value("Value", p.x), y: .value("Voltage", p.y)).foregroundStyle(by: .value("Net", p.series))
        }
        .chartXAxis {
            AxisMarks(values: Array(okRuns.indices).map(Double.init)) { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let i = value.as(Double.self), Int(i) >= 0, Int(i) < okRuns.count {
                        Text(verbatim: okRuns[Int(i)].value).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartYAxis { voltageAxis("V") }
    }

    private var acChart: some View {
        var points: [Point] = []
        var decadesLo = Double.infinity, decadesHi = -Double.infinity
        for run in okRuns {
            guard let x = run.x, let net = run.nets.first, let mag = net.magnitudeDb else { continue }
            for i in 0..<min(x.count, mag.count) where x[i] > 0 {
                guard let db = mag[i] else { continue }
                let lf = log10(x[i])
                decadesLo = min(decadesLo, lf)
                decadesHi = max(decadesHi, lf)
                points.append(Point(id: points.count, series: run.value, x: lf, y: max(db, -200)))
            }
        }
        let lo = decadesLo.isFinite ? floor(decadesLo) : 0, hi = decadesHi.isFinite ? max(ceil(decadesHi), lo + 1) : 1
        return Chart(points) { p in
            LineMark(x: .value("Frequency", p.x), y: .value("Magnitude", p.y)).foregroundStyle(by: .value("Value", p.series))
        }
        .chartForegroundStyleScale(domain: labels, range: colors)
        .chartXScale(domain: lo...hi)
        .chartXAxis {
            AxisMarks(values: Array(stride(from: lo, through: hi, by: 1))) { value in
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
                    if let v = value.as(Double.self) { Text(String(format: "%.0f dB", v)).foregroundStyle(Theme.textMuted) }
                }
            }
        }
    }

    private var transientChart: some View {
        var points: [Point] = []
        for run in okRuns {
            guard let x = run.x, let net = run.nets.first, let values = net.values else { continue }
            for i in 0..<min(x.count, values.count) {
                if let v = values[i] { points.append(Point(id: points.count, series: run.value, x: x[i], y: v)) }
            }
        }
        return Chart(points) { p in
            LineMark(x: .value("Time", p.x), y: .value("Voltage", p.y)).foregroundStyle(by: .value("Value", p.series))
        }
        .chartForegroundStyleScale(domain: labels, range: colors)
        .chartXAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let t = value.as(Double.self) {
                        Text(EngineeringFormat.string(t, unit: "s", digits: 2)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartYAxis { voltageAxis("V") }
    }

    private func voltageAxis(_ unit: String) -> some AxisContent {
        AxisMarks { value in
            AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
            AxisValueLabel {
                if let v = value.as(Double.self) {
                    Text(EngineeringFormat.string(v, unit: unit, digits: 3)).foregroundStyle(Theme.textMuted)
                }
            }
        }
    }

    private var table: some View {
        ScrollView {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 3) {
                ForEach(result.runs) { run in
                    GridRow {
                        Text(verbatim: run.value).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                        if !run.ok {
                            Text(verbatim: run.error).font(.caption).foregroundStyle(Theme.warning)
                        } else if result.analysis == "dc" {
                            Text(verbatim: run.nets.prefix(6).map { n in
                                "\(n.name) \(n.voltage.map { EngineeringFormat.string($0, unit: "V", digits: 4) } ?? "—")"
                            }.joined(separator: "   "))
                            .font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                        } else if result.analysis == "ac", let m = run.nets.first?.metrics {
                            Text(verbatim: [m.lowFreqDb.map { String(format: "%.2f dB", $0) },
                                            m.f3dbHz.map { "−3 dB " + EngineeringFormat.string($0, unit: "Hz", digits: 4) },
                                            m.unityHz.map { "0 dB " + EngineeringFormat.string($0, unit: "Hz", digits: 4) }]
                                .compactMap { $0 }.joined(separator: "   "))
                                .font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                        } else if let net = run.nets.first, let values = net.values?.compactMap({ $0 }), !values.isEmpty {
                            Text(verbatim: "min \(EngineeringFormat.string(values.min() ?? 0, unit: "V", digits: 4))   max \(EngineeringFormat.string(values.max() ?? 0, unit: "V", digits: 4))   end \(EngineeringFormat.string(values.last ?? 0, unit: "V", digits: 4))")
                                .font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                        } else {
                            Text(verbatim: "—").font(.caption)
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 130)
    }
}

// MARK: - FFT / THD

/// Spectrum of a node's transient waveform with its harmonics and total harmonic distortion.
struct FFTPanel: View {
    let result: FFTResult

    private struct Bin: Identifiable {
        let id: Int
        let f: Double
        let db: Double
    }

    /// Bins up to just past the last harmonic analysed (the rest of the spectrum is noise floor).
    private var bins: [Bin] {
        let limit = (Double((result.harmonics.last?.order ?? 10) + 1)) * max(result.fundamentalHz, 1e-30)
        var out: [Bin] = []
        for i in 0..<min(result.frequency.count, result.magnitudeDb.count) where result.frequency[i] <= limit {
            out.append(Bin(id: i, f: result.frequency[i], db: max(result.magnitudeDb[i] ?? -200, -200)))
        }
        return out
    }

    private var floorDb: Double { max(-200, (bins.map(\.db).min() ?? -200) - 5) }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 16) {
                stat("THD", String(format: "%.4f %%", result.thdPercent))
                stat("THD", result.thdPercent > 0 ? String(format: "%.1f dB", 20 * log10(result.thdPercent / 100)) : "—")
                stat("Fundamental", EngineeringFormat.string(result.fundamentalHz, unit: "Hz", digits: 5))
                stat("DC", EngineeringFormat.string(result.dc, unit: "V", digits: 4))
                Spacer()
                Text(verbatim: "\(result.net) · \(result.cycles) × · \(EngineeringFormat.string(result.windowStart, unit: "s", digits: 3)) – \(EngineeringFormat.string(result.windowStop, unit: "s", digits: 3))")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            Chart(bins) { b in
                RuleMark(x: .value("Frequency", b.f), yStart: .value("Floor", floorDb), yEnd: .value("Magnitude", b.db))
                    .foregroundStyle(Theme.skyBlue)
            }
            .chartYScale(domain: floorDb...max(floorDb + 10, (bins.map(\.db).max() ?? 0) + 5))
            .chartXAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let f = value.as(Double.self) {
                            Text(EngineeringFormat.string(f, unit: "Hz", digits: 2)).foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .chartYAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let v = value.as(Double.self) { Text(String(format: "%.0f dB", v)).foregroundStyle(Theme.textMuted) }
                    }
                }
            }
            .frame(minHeight: 170)
            ScrollView {
                Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 3) {
                    GridRow {
                        Text("Harmonic")
                        Text("Frequency")
                        Text("Amplitude")
                        Text(verbatim: "dBc")
                    }
                    .font(.caption.weight(.bold))
                    .foregroundStyle(Theme.skyBlue)
                    ForEach(result.harmonics) { h in
                        GridRow {
                            Text(verbatim: "\(h.order)").font(.caption.monospacedDigit()).foregroundStyle(Theme.textPrimary)
                            Text(EngineeringFormat.string(h.frequency, unit: "Hz", digits: 5)).font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                            Text(EngineeringFormat.string(h.amplitude, unit: "V", digits: 4)).font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                            Text(verbatim: h.order == 1 ? "0" : (h.dbc.map { String(format: "%.1f", $0) } ?? "—"))
                                .font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 130)
        }
    }

    private func stat(_ title: LocalizedStringKey, _ value: String) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption2).foregroundStyle(Theme.textMuted)
            Text(verbatim: value).font(.callout.monospacedDigit()).foregroundStyle(Theme.probe)
        }
    }
}
