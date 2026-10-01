import Charts
import SwiftUI

/// SPICE-class analysis workspace: DC operating point table and transient waveforms.
struct SimulationView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var stopText = "5m"
    @State private var stepText = "5u"
    @State private var hiddenSeries: Set<String> = []
    @State private var showCurrents = false

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Image(systemName: "waveform.path.ecg").foregroundStyle(Theme.blue)
                Text("Analysis").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Button {
                    Task { await store.simulateDC() }
                } label: { Label("DC Operating Point", systemImage: "play.fill") }
                Divider().frame(height: 18)
                Text("Transient: stop").foregroundStyle(Theme.textMuted)
                TextField("stop", text: $stopText).textFieldStyle(.blue).frame(width: 64)
                Text("step").foregroundStyle(Theme.textMuted)
                TextField("step", text: $stepText).textFieldStyle(.blue).frame(width: 64)
                Button {
                    runTransient()
                } label: { Label("Run Transient", systemImage: "waveform") }
                Spacer()
                Button {
                    store.export(.spice)
                } label: { Label("SPICE Netlist", systemImage: "doc.plaintext") }
            }
            .buttonStyle(.borderless)

            HSplitView {
                dcPanel
                    .frame(minWidth: 240, idealWidth: 320, maxWidth: 480)
                transientPanel
                    .frame(minWidth: 360)
            }
        }
        .background(Theme.navy)
    }

    private func runTransient() {
        guard let stop = EngineeringFormat.parse(stopText), let step = EngineeringFormat.parse(stepText), stop > 0, step > 0 else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Use values like 5m (stop) and 5u (step).")
            return
        }
        Task { await store.simulateTransient(stop: stop, step: step) }
    }

    // MARK: DC

    private var dcPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 14) {
                if let dc = store.dcResult {
                    if dc.converged {
                        Badge(text: "Converged in \(dc.iterations) Newton iterations", systemImage: "checkmark.circle")
                        sectionTitle("Node Voltages")
                        ForEach(dc.nets.filter { netIsInteresting($0.index) }) { n in
                            row(n.name, EngineeringFormat.string(n.voltage, unit: "V", digits: 4), icon: "circle.dotted")
                        }
                        sectionTitle("Device Currents & Power")
                        ForEach(dc.devices) { d in
                            HStack {
                                Text(d.ref).font(.system(.body, design: .monospaced)).foregroundStyle(Theme.textPrimary)
                                    .frame(width: 50, alignment: .leading)
                                Text(EngineeringFormat.string(d.current, unit: "A", digits: 4))
                                    .font(.callout.monospacedDigit()).foregroundStyle(Theme.skyBlue)
                                Spacer()
                                Text(EngineeringFormat.string(d.power, unit: "W", digits: 3))
                                    .font(.callout.monospacedDigit())
                                    .foregroundStyle(d.power > 0.25 ? Theme.warning : Theme.lightBlue)
                            }
                            .padding(.vertical, 2)
                            .contentShape(Rectangle())
                            .onTapGesture { store.select(component: d.component) }
                        }
                    } else {
                        Label(dc.error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                    }
                } else {
                    BlueEmptyState(systemImage: "bolt.horizontal", title: "No DC solution yet",
                                   message: "Run a DC operating point to see node voltages, branch currents and dissipation.",
                                   actionTitle: "Run DC Analysis") { Task { await store.simulateDC() } }
                }
            }
            .padding(14)
        }
        .bluePanel()
        .padding(10)
    }

    private func netIsInteresting(_ index: Int) -> Bool {
        guard let net = store.snapshot.net(index) else { return false }
        return net.pinCount > 1
    }

    private func sectionTitle(_ s: String) -> some View {
        Text(s.uppercased()).font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue).padding(.top, 6)
    }

    private func row(_ name: String, _ value: String, icon: String) -> some View {
        HStack {
            Image(systemName: icon).foregroundStyle(Theme.blue)
            Text(name).font(.system(.body, design: .monospaced)).foregroundStyle(Theme.textPrimary)
            Spacer()
            Text(value).font(.callout.monospacedDigit()).foregroundStyle(Theme.probe)
        }
    }

    // MARK: Transient

    private struct Sample: Identifiable {
        let id: Int
        let series: String
        let t: Double
        let v: Double
    }

    private func samples(_ result: TransientResult) -> [Sample] {
        let source = showCurrents ? result.currents : result.nets
        var out: [Sample] = []
        var id = 0
        for s in source where !hiddenSeries.contains(s.label) {
            let count = min(s.values.count, result.time.count)
            for i in 0..<count {
                out.append(Sample(id: id, series: s.label, t: result.time[i], v: s.values[i]))
                id += 1
            }
        }
        return out
    }

    private func seriesLabels(_ result: TransientResult) -> [String] {
        (showCurrents ? result.currents : result.nets).map(\.label)
    }

    private func seriesColors(_ result: TransientResult) -> [Color] {
        seriesLabels(result).indices.map { Theme.seriesColors[$0 % Theme.seriesColors.count] }
    }

    private var transientPanel: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let tr = store.transientResult {
                if tr.ok {
                    HStack {
                        Picker("", selection: $showCurrents) {
                            Text("Voltages").tag(false)
                            Text("Currents").tag(true)
                        }
                        .pickerStyle(.segmented)
                        .frame(width: 200)
                        Spacer()
                        Text("\(tr.time.count) points · 0 – \(EngineeringFormat.string(tr.time.last ?? 0, unit: "s"))")
                            .font(.caption).foregroundStyle(Theme.textMuted)
                    }
                    Chart(samples(tr)) { s in
                        LineMark(x: .value("Time (s)", s.t), y: .value(showCurrents ? "Current (A)" : "Voltage (V)", s.v))
                            .foregroundStyle(by: .value("Signal", s.series))
                            .interpolationMethod(.linear)
                    }
                    .chartForegroundStyleScale(domain: seriesLabels(tr), range: seriesColors(tr))
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

                    ScrollView(.horizontal, showsIndicators: false) {
                        HStack {
                            ForEach(Array((showCurrents ? tr.currents : tr.nets).enumerated()), id: \.element.id) { index, s in
                                let hidden = hiddenSeries.contains(s.label)
                                Button {
                                    if hidden { hiddenSeries.remove(s.label) } else { hiddenSeries.insert(s.label) }
                                } label: {
                                    HStack(spacing: 5) {
                                        Circle().fill(Theme.seriesColors[index % Theme.seriesColors.count]).frame(width: 8, height: 8)
                                        Text(s.label).font(.caption.monospaced())
                                    }
                                    .padding(.horizontal, 8).padding(.vertical, 4)
                                    .foregroundStyle(hidden ? Theme.textMuted : Theme.textPrimary)
                                    .background(Capsule().fill(Theme.blue.opacity(hidden ? 0.05 : 0.2)))
                                }
                                .buttonStyle(.plain)
                            }
                        }
                    }
                } else {
                    Label(tr.error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                    Spacer()
                }
            } else {
                BlueEmptyState(systemImage: "waveform", title: "Transient analysis",
                               message: "Simulate the circuit over time. Use SIN(off amp freq) or PULSE(v1 v2 period) sources to stimulate it.",
                               actionTitle: "Run Transient") { runTransient() }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .padding(14)
        .bluePanel()
        .padding(10)
    }
}
