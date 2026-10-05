import Charts
import SwiftUI

/// Measurement cursors and ".meas"-like measurements under the transient chart. Click the chart to place the active
/// cursor (A or B); the readout gives both times, Δt and 1/Δt and every shown signal's value at A, at B and their
/// difference. Measurements run in the core over the window between the cursors (the whole run without both).
struct WaveformToolsPanel: View {
    let time: [Double]
    let series: [WaveformSeries]
    let unit: String
    @Binding var cursorA: Double?
    @Binding var cursorB: Double?
    @Binding var placingB: Bool
    @State private var expanded = false
    @State private var measured = ""
    @State private var measurement: WaveformMeasurementsInfo?

    var body: some View {
        DisclosureGroup(isExpanded: $expanded) {
            VStack(alignment: .leading, spacing: 8) {
                cursorBar
                if cursorA != nil || cursorB != nil { cursorValues }
                Divider()
                measureBar
                if let measurement { MeasurementGrid(m: measurement, unit: unit) }
            }
            .padding(.top, 4)
        } label: {
            Label("Cursors & Measurements", systemImage: "ruler").font(.caption.weight(.semibold)).foregroundStyle(Theme.skyBlue)
        }
        .onChange(of: cursorA) { _, _ in measurement = nil }
        .onChange(of: cursorB) { _, _ in measurement = nil }
    }

    private var cursorBar: some View {
        HStack(spacing: 10) {
            Picker("Place cursor", selection: $placingB) {
                Text(verbatim: "A").tag(false)
                Text(verbatim: "B").tag(true)
            }
            .pickerStyle(.segmented)
            .frame(width: 180)
            .help("Click the chart to place the selected cursor")
            readout("A", cursorA.map { EngineeringFormat.string($0, unit: "s", digits: 4) })
            readout("B", cursorB.map { EngineeringFormat.string($0, unit: "s", digits: 4) })
            if let a = cursorA, let b = cursorB {
                readout("Δt", EngineeringFormat.string(b - a, unit: "s", digits: 4))
                if b != a { readout("1/Δt", EngineeringFormat.string(1 / abs(b - a), unit: "Hz", digits: 4)) }
            }
            Spacer()
            Button("Clear Cursors") {
                cursorA = nil
                cursorB = nil
            }
            .disabled(cursorA == nil && cursorB == nil)
        }
        .font(.caption)
    }

    private func readout(_ name: String, _ value: String?) -> some View {
        HStack(spacing: 4) {
            Text(verbatim: name).foregroundStyle(Theme.textMuted)
            Text(verbatim: value ?? "—").font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
        }
    }

    private var cursorValues: some View {
        ScrollView {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 3) {
                GridRow {
                    Text("Signal")
                    Text(verbatim: "A")
                    Text(verbatim: "B")
                    Text(verbatim: "B − A")
                }
                .font(.caption.weight(.bold))
                .foregroundStyle(Theme.skyBlue)
                ForEach(series) { s in
                    let a = cursorA.flatMap { WaveformMath.value(at: $0, time: time, values: s.values) }
                    let b = cursorB.flatMap { WaveformMath.value(at: $0, time: time, values: s.values) }
                    GridRow {
                        Text(verbatim: s.label).font(.caption.monospaced()).foregroundStyle(Theme.textPrimary)
                        cell(a)
                        cell(b)
                        cell(a.flatMap { av in b.map { $0 - av } })
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .frame(maxHeight: 110)
    }

    private func cell(_ v: Double?) -> some View {
        Text(verbatim: v.map { EngineeringFormat.string($0, unit: unit, digits: 4) } ?? "—")
            .font(.caption.monospacedDigit())
            .foregroundStyle(Theme.probe)
    }

    private var measureBar: some View {
        HStack(spacing: 10) {
            Picker("Measure", selection: $measured) {
                ForEach(series) { s in Text(verbatim: s.label).tag(s.label) }
            }
            .fixedSize()
            Button("Measure") { measure() }
                .disabled(series.isEmpty)
            (cursorA != nil && cursorB != nil ? Text("Between cursors A and B") : Text("Whole run (place A and B for a window)"))
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
            Spacer()
        }
        .font(.caption)
        .onAppear { if measured.isEmpty { measured = series.first?.label ?? "" } }
    }

    private func measure() {
        guard let s = series.first(where: { $0.label == measured }) ?? series.first else { return }
        measured = s.label
        let window = (cursorA, cursorB)
        var from: Double?, to: Double?
        if let a = window.0, let b = window.1 {
            from = min(a, b)
            to = max(a, b)
        }
        measurement = EDAEngine.measureWaveform(time: time, values: s.values, from: from, to: to)
    }
}

/// The measurement results of one waveform.
private struct MeasurementGrid: View {
    let m: WaveformMeasurementsInfo
    let unit: String

    var body: some View {
        if !m.ok {
            Label(m.error, systemImage: "exclamationmark.triangle.fill").font(.caption).foregroundStyle(Theme.warning)
        } else {
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 3) {
                GridRow {
                    item("Min", m.min.map { v($0) })
                    item("Max", m.max.map { v($0) })
                    item("Peak-to-peak", m.peakToPeak.map { v($0) })
                }
                GridRow {
                    item("Average", m.average.map { v($0) })
                    item("RMS", m.rms.map { v($0) })
                    item("AC RMS", m.acRms.map { v($0) })
                }
                GridRow {
                    item("Rise time (10–90 %)", m.riseTime.map { EngineeringFormat.string($0, unit: "s", digits: 4) })
                    item("Fall time (90–10 %)", m.fallTime.map { EngineeringFormat.string($0, unit: "s", digits: 4) })
                    item("Overshoot", m.overshootPercent.map { String(format: "%.2f %%", $0) })
                }
                GridRow {
                    item("Frequency", m.frequency.map { EngineeringFormat.string($0, unit: "Hz", digits: 5) })
                    item("Period", m.period.map { EngineeringFormat.string($0, unit: "s", digits: 5) })
                    item("Duty cycle", m.dutyCycle.map { String(format: "%.1f %%", $0 * 100) })
                }
                GridRow {
                    item("Settling (2 %)", m.settlingTime.map { EngineeringFormat.string($0, unit: "s", digits: 4) })
                    item("Window", window)
                    Color.clear.gridCellUnsizedAxes([.horizontal, .vertical])
                }
            }
            .font(.caption)
        }
    }

    private var window: String? {
        guard let from = m.from, let to = m.to else { return nil }
        return EngineeringFormat.string(from, unit: "s", digits: 3) + " – " + EngineeringFormat.string(to, unit: "s", digits: 3)
    }

    private func v(_ x: Double) -> String { EngineeringFormat.string(x, unit: unit, digits: 4) }

    private func item(_ title: LocalizedStringKey, _ value: String?) -> some View {
        HStack(spacing: 6) {
            Text(title).foregroundStyle(Theme.textMuted)
            Text(verbatim: value ?? "—").font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
        }
    }
}
