import Charts
import SwiftUI

/// Noise analysis: output noise density (and the input-referred density) on log–log axes, the integrated RMS noise
/// over the sweep and the parts that contribute most of it.
struct NoisePanel: View {
    let result: NoiseAnalysisResult

    private struct Point: Identifiable {
        let id: Int
        let series: String
        let logF: Double
        let logV: Double
    }

    /// The input-referred curve shares the axis when it is a voltage too.
    private var plotsInput: Bool { result.inputDensity != nil && result.inputUnit == "V" }

    private var points: [Point] {
        var out: [Point] = []
        func add(_ values: [Double?], series: String) {
            for i in 0..<min(values.count, result.frequency.count) {
                guard let v = values[i], v > 0, v.isFinite, result.frequency[i] > 0 else { continue }
                out.append(Point(id: out.count, series: series, logF: log10(result.frequency[i]), logV: log10(v)))
            }
        }
        add(result.outputDensity, series: "out")
        if plotsInput, let input = result.inputDensity { add(input, series: "in") }
        return out
    }

    private var decades: [Double] {
        guard let first = result.frequency.first, let last = result.frequency.last, first > 0, last > first else { return [0, 1] }
        let lo = Int(floor(log10(first))), hi = max(Int(ceil(log10(last))), lo + 1)
        return (lo...hi).map(Double.init)
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 14) {
                legend(Theme.seriesColors[0], Text("Output noise") + Text(verbatim: " · \(result.output)"))
                if plotsInput {
                    legend(Theme.seriesColors[1 % Theme.seriesColors.count],
                           Text("Input-referred") + Text(verbatim: " · \(result.inputSource)"))
                }
                Spacer()
                Text(verbatim: "\(result.frequency.count) · \(EngineeringFormat.string(result.frequency.first ?? 0, unit: "Hz")) – \(EngineeringFormat.string(result.frequency.last ?? 0, unit: "Hz"))")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            chart.frame(minHeight: 160)
            readouts
            contributions
        }
    }

    private func legend(_ color: Color, _ text: Text) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 8, height: 8)
            text.font(.caption).foregroundStyle(Theme.textPrimary)
        }
    }

    private var chart: some View {
        Chart(points) { p in
            LineMark(x: .value("Frequency", p.logF), y: .value("Density", p.logV))
                .foregroundStyle(by: .value("Series", p.series))
                .interpolationMethod(.linear)
        }
        .chartForegroundStyleScale(domain: ["out", "in"], range: [Theme.seriesColors[0], Theme.seriesColors[1 % Theme.seriesColors.count]])
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
                        Text(EngineeringFormat.string(pow(10, v), unit: "V/√Hz", digits: 2)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .chartLegend(.hidden)
    }

    private var readouts: some View {
        Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 4) {
            GridRow {
                Text("Integrated output noise").foregroundStyle(Theme.textMuted)
                Text(verbatim: rms(result.outputRms, unit: "V")).foregroundStyle(Theme.probe)
            }
            if let input = result.inputRms {
                GridRow {
                    Text("Integrated input noise").foregroundStyle(Theme.textMuted)
                    Text(verbatim: rms(input, unit: result.inputUnit) + " · \(result.inputSource)").foregroundStyle(Theme.probe)
                }
            } else {
                GridRow {
                    Text("No input source: give the input an AC magnitude or name it to see the input-referred noise.")
                        .foregroundStyle(Theme.textMuted)
                        .gridCellColumns(2)
                }
            }
        }
        .font(.caption.monospacedDigit())
    }

    private func rms(_ value: Double?, unit: String) -> String {
        guard let value else { return "—" }
        return EngineeringFormat.string(value, unit: unit, digits: 4) + " RMS"
    }

    private var contributions: some View {
        let total = max(result.outputRms ?? 0, 1e-300)
        return VStack(alignment: .leading, spacing: 4) {
            Text("Largest contributors").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            ScrollView {
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 3) {
                    ForEach(result.contributions.prefix(12)) { c in
                        let share = min(1, (c.rms / total) * (c.rms / total))
                        GridRow {
                            Text(verbatim: c.ref).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                            kindTitle(c.kind).font(.caption).foregroundStyle(Theme.textMuted)
                            Text(EngineeringFormat.string(c.rms, unit: "V", digits: 3)).font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                            HStack(spacing: 4) {
                                Capsule().fill(Theme.blue.opacity(0.7)).frame(width: max(2, 80 * share), height: 6)
                                Text(String(format: "%.1f %%", share * 100)).font(.caption2.monospacedDigit()).foregroundStyle(Theme.textMuted)
                            }
                        }
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 150)
        }
    }

    private func kindTitle(_ kind: String) -> Text {
        switch kind {
        case "thermal": return Text("Thermal")
        case "shot": return Text("Shot")
        case "flicker": return Text("Flicker (1/f)")
        case "voltage": return Text("Op-amp voltage noise")
        case "current": return Text("Op-amp current noise")
        default: return Text(verbatim: kind)
        }
    }
}
