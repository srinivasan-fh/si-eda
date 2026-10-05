import Charts
import SwiftUI

/// SPICE-class analysis workspace: DC operating point table beside transient waveforms, AC Bode plots, DC sweeps or
/// Monte Carlo tolerance analysis.
struct SimulationView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var stopText = "5m"
    @State private var stepText = "5u"
    @State private var hiddenSeries: Set<String> = []
    @State private var showCurrents = false
    @State private var analysis: SimulationAnalysis = .transient
    @State private var acStart = "10"
    @State private var acStop = "1MEG"
    @State private var acPoints = "50"
    @State private var acSource = ""
    @State private var sweepSource = "V1"
    @State private var sweepStart = "0"
    @State private var sweepStop = "5"
    @State private var sweepStep = "50m"
    @State private var mcNet = ""
    @State private var mcBandwidth = false
    @State private var mcRuns = "200"
    @State private var mode: AnalysisMode = .circuit
    @State private var noiseOutput = ""
    @State private var noiseStart = "10"
    @State private var noiseStop = "100k"
    @State private var noisePoints = "20"
    @State private var noiseSource = ""
    @State private var sweepPart = "R1"
    @State private var sweepValues = "1k 2k2 4k7 10k"
    @State private var sweepKind = "dc"
    @State private var sweepNet = ""
    @State private var fftNet = ""
    @State private var fftStop = "10m"
    @State private var fftStep = "2u"
    @State private var fftHarmonics = "10"
    @State private var cursorA: Double?
    @State private var cursorB: Double?
    @State private var placingB = false

    var body: some View {
        if mode == .integrity {
            SignalIntegrityView(mode: $mode)
        } else {
            circuitAnalysis
        }
    }

    private var circuitAnalysis: some View {
        VStack(spacing: 0) {
            OptionsBar {
                AnalysisModePicker(mode: $mode)
                Divider().frame(height: 18)
                Image(systemName: "waveform.path.ecg").foregroundStyle(Theme.blue)
                Text("Analysis").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                LiveRunButton(live: store.live)
                Button {
                    Task { await store.simulateDC() }
                } label: { Label("DC Operating Point", systemImage: "play.fill") }
                Divider().frame(height: 18)
                Picker("Analysis type", selection: $analysis) {
                    Text("Transient").tag(SimulationAnalysis.transient)
                    Text("AC Sweep").tag(SimulationAnalysis.ac)
                    Text("DC Sweep").tag(SimulationAnalysis.dcSweep)
                    Text("Monte Carlo").tag(SimulationAnalysis.monteCarlo)
                    Text("Noise").tag(SimulationAnalysis.noise)
                    Text("Parameter Sweep").tag(SimulationAnalysis.paramSweep)
                    Text("FFT / THD").tag(SimulationAnalysis.fft)
                }
                .labelsHidden()
                .fixedSize()
                switch analysis {
                case .transient:
                    Text("Transient: stop").foregroundStyle(Theme.textMuted)
                    TextField("stop", text: $stopText).textFieldStyle(.blue).frame(width: 64)
                    Text("step").foregroundStyle(Theme.textMuted)
                    TextField("step", text: $stepText).textFieldStyle(.blue).frame(width: 64)
                    Menu {
                        Button("Circuit — 5 ms, 5 µs") { stopText = "5m"; stepText = "5u" }
                        Button("Serial output — 200 ms, 10 µs") { stopText = "200m"; stepText = "10u" }
                        Button("Firmware — 1 s, 100 µs") { stopText = "1"; stepText = "100u" }
                        Button("Long firmware run — 5 s, 500 µs") { stopText = "5"; stepText = "500u" }
                    } label: {
                        Image(systemName: "timer")
                    }
                    .menuStyle(.borderlessButton)
                    .fixedSize()
                    .help("Analysis presets — microcontroller firmware needs a longer run (e.g. 1 s at 100 µs)")
                    .accessibilityLabel("Transient presets")
                    Button {
                        runTransient()
                    } label: { Label("Run Transient", systemImage: "waveform") }
                case .ac:
                    Text("from").foregroundStyle(Theme.textMuted)
                    TextField("from", text: $acStart).textFieldStyle(.blue).frame(width: 56)
                    Text("to").foregroundStyle(Theme.textMuted)
                    TextField("to", text: $acStop).textFieldStyle(.blue).frame(width: 56)
                    TextField("50", text: $acPoints).textFieldStyle(.blue).frame(width: 40)
                    Text("points/decade").foregroundStyle(Theme.textMuted)
                    Text("Input").foregroundStyle(Theme.textMuted)
                    TextField("auto", text: $acSource).textFieldStyle(.blue).frame(width: 52)
                    Button {
                        runAC()
                    } label: { Label("Run AC Sweep", systemImage: "chart.xyaxis.line") }
                case .dcSweep:
                    Text("Source").foregroundStyle(Theme.textMuted)
                    TextField("Source", text: $sweepSource).textFieldStyle(.blue).frame(width: 52)
                    Text("from").foregroundStyle(Theme.textMuted)
                    TextField("from", text: $sweepStart).textFieldStyle(.blue).frame(width: 52)
                    Text("to").foregroundStyle(Theme.textMuted)
                    TextField("to", text: $sweepStop).textFieldStyle(.blue).frame(width: 52)
                    Text("step").foregroundStyle(Theme.textMuted)
                    TextField("step", text: $sweepStep).textFieldStyle(.blue).frame(width: 52)
                    Button {
                        runDCSweep()
                    } label: { Label("Run DC Sweep", systemImage: "chart.line.uptrend.xyaxis") }
                case .monteCarlo:
                    Text("Net").foregroundStyle(Theme.textMuted)
                    TextField("Net", text: $mcNet).textFieldStyle(.blue).frame(width: 72)
                    Picker("Measure", selection: $mcBandwidth) {
                        Text("DC voltage").tag(false)
                        Text("Bandwidth").tag(true)
                    }
                    .labelsHidden()
                    .fixedSize()
                    Text("Runs").foregroundStyle(Theme.textMuted)
                    TextField("200", text: $mcRuns).textFieldStyle(.blue).frame(width: 48)
                    Button {
                        runMonteCarlo()
                    } label: { Label("Run Monte Carlo", systemImage: "chart.bar") }
                case .noise:
                    Text("Output").foregroundStyle(Theme.textMuted)
                    TextField("Net", text: $noiseOutput).textFieldStyle(.blue).frame(width: 64)
                    Text("from").foregroundStyle(Theme.textMuted)
                    TextField("from", text: $noiseStart).textFieldStyle(.blue).frame(width: 52)
                    Text("to").foregroundStyle(Theme.textMuted)
                    TextField("to", text: $noiseStop).textFieldStyle(.blue).frame(width: 52)
                    TextField("20", text: $noisePoints).textFieldStyle(.blue).frame(width: 36)
                    Text("points/decade").foregroundStyle(Theme.textMuted)
                    Text("Input").foregroundStyle(Theme.textMuted)
                    TextField("auto", text: $noiseSource).textFieldStyle(.blue).frame(width: 48)
                    Button {
                        runNoise()
                    } label: { Label("Run Noise", systemImage: "waveform.badge.magnifyingglass") }
                case .paramSweep:
                    Text("Part").foregroundStyle(Theme.textMuted)
                    TextField("R1", text: $sweepPart).textFieldStyle(.blue).frame(width: 44)
                    Text("Values").foregroundStyle(Theme.textMuted)
                    TextField("1k 2k2 4k7", text: $sweepValues).textFieldStyle(.blue).frame(width: 110)
                        .help("Values separated by spaces or commas (up to 100)")
                    Picker("Analysis", selection: $sweepKind) {
                        Text("DC").tag("dc")
                        Text("AC Sweep").tag("ac")
                        Text("Transient").tag("transient")
                    }
                    .labelsHidden()
                    .fixedSize()
                    .help("AC uses the AC Sweep range, Transient the Transient stop and step")
                    Text("Net").foregroundStyle(Theme.textMuted)
                    TextField("all", text: $sweepNet).textFieldStyle(.blue).frame(width: 56)
                    Button {
                        runParamSweep()
                    } label: { Label("Run Sweep", systemImage: "slider.horizontal.3") }
                case .fft:
                    Text("Net").foregroundStyle(Theme.textMuted)
                    TextField("Net", text: $fftNet).textFieldStyle(.blue).frame(width: 64)
                    Text("stop").foregroundStyle(Theme.textMuted)
                    TextField("stop", text: $fftStop).textFieldStyle(.blue).frame(width: 52)
                    Text("step").foregroundStyle(Theme.textMuted)
                    TextField("step", text: $fftStep).textFieldStyle(.blue).frame(width: 52)
                    Text("Harmonics").foregroundStyle(Theme.textMuted)
                    TextField("10", text: $fftHarmonics).textFieldStyle(.blue).frame(width: 36)
                    Button {
                        runFFT()
                    } label: { Label("Run FFT", systemImage: "chart.bar.xaxis") }
                }
                Spacer()
                Button {
                    store.export(.spice)
                } label: { Label("SPICE Netlist", systemImage: "doc.plaintext") }
            }
            .buttonStyle(.borderless)

            // Plain stack, not HSplitView: an AppKit split view nested in the SwiftUI split view adds
            // constraints of its own that fight the window's.
            LiveSwitcher(live: store.live) {
                HStack(spacing: 0) {
                    dcPanel
                        .frame(width: 300)
                    Rectangle().fill(Theme.blue.opacity(0.3)).frame(width: 1)
                    analysisPanel
                        .frame(minWidth: 300, maxWidth: .infinity)
                }
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

    // Values go to the core as typed ("1MEG", "50m"); it parses and validates them.
    private func runAC() {
        guard let points = Int(acPoints.trimmingCharacters(in: .whitespaces)), points > 0 else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Use a whole number of points per decade, e.g. 50.")
            return
        }
        let source = acSource.trimmingCharacters(in: .whitespaces)
        Task { await store.simulateAC(start: acStart, stop: acStop, pointsPerDecade: points, source: source) }
    }

    private func runDCSweep() {
        let source = sweepSource.trimmingCharacters(in: .whitespaces)
        Task { await store.simulateDCSweep(source: source, start: sweepStart, stop: sweepStop, step: sweepStep) }
    }

    private func runMonteCarlo() {
        let net = mcNet.trimmingCharacters(in: .whitespaces)
        guard !net.isEmpty, let runs = Int(mcRuns.trimmingCharacters(in: .whitespaces)), runs > 0 else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Name the net to measure (e.g. OUT) and the number of runs (e.g. 200).")
            return
        }
        Task { await store.simulateMonteCarlo(net: net, measure: mcBandwidth ? "f3db" : "dc", runs: runs) }
    }

    private func runNoise() {
        let output = noiseOutput.trimmingCharacters(in: .whitespaces)
        guard !output.isEmpty, let points = Int(noisePoints.trimmingCharacters(in: .whitespaces)), points > 0 else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Name the output net (e.g. OUT) and a whole number of points per decade, e.g. 20.")
            return
        }
        let source = noiseSource.trimmingCharacters(in: .whitespaces)
        Task { await store.simulateNoise(output: output, start: noiseStart, stop: noiseStop, pointsPerDecade: points, source: source) }
    }

    private func runParamSweep() {
        let part = sweepPart.trimmingCharacters(in: .whitespaces)
        let values = sweepValues.split(whereSeparator: { $0 == " " || $0 == "," || $0 == ";" }).map(String.init)
        let net = sweepNet.trimmingCharacters(in: .whitespaces)
        guard !part.isEmpty, !values.isEmpty, values.count <= 100, sweepKind == "dc" || !net.isEmpty else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Name the part (e.g. R1), its values (e.g. 1k 2k2 4k7) and, for AC or transient, the net to plot.")
            return
        }
        Task {
            await store.simulateParamSweep(component: part, values: values, analysis: sweepKind, net: net,
                                           start: acStart, stop: sweepKind == "ac" ? acStop : stopText, step: stepText)
        }
    }

    private func runFFT() {
        let net = fftNet.trimmingCharacters(in: .whitespaces)
        guard !net.isEmpty, let harmonics = Int(fftHarmonics.trimmingCharacters(in: .whitespaces)), harmonics >= 2 else {
            store.alert = AlertItem(title: "Invalid analysis settings", message: "Name the net to analyse (e.g. OUT) and at least 2 harmonics.")
            return
        }
        Task { await store.simulateFFT(net: net, stop: fftStop, step: fftStep, fundamental: "", harmonics: harmonics) }
    }

    // MARK: Analyses

    @ViewBuilder private var analysisPanel: some View {
        switch analysis {
        case .transient:
            transientPanel
        case .ac:
            resultPanel {
                if let ac = store.acResult {
                    if ac.ok { ACAnalysisPanel(result: ac) } else { failure(ac.error) }
                } else {
                    BlueEmptyState(systemImage: "chart.xyaxis.line", title: "AC analysis",
                                   message: "Frequency response of every node, linearised at the DC operating point. Give the input source an AC magnitude (for example 0 AC 1 or SIN(0 1 1k) AC 1) or name it as the input.",
                                   actionTitle: "Run AC Sweep") { runAC() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        case .dcSweep:
            resultPanel {
                if let sweep = store.dcSweepResult {
                    if sweep.ok { DCSweepPanel(result: sweep) } else { failure(sweep.error) }
                } else {
                    BlueEmptyState(systemImage: "chart.line.uptrend.xyaxis", title: "DC sweep",
                                   message: "Steps a voltage or current source (by reference, e.g. V1) and solves the operating point at every value: transfer curves, diode and transistor characteristics.",
                                   actionTitle: "Run DC Sweep") { runDCSweep() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        case .noise:
            resultPanel {
                if let noise = store.noiseResult {
                    if noise.ok { NoisePanel(result: noise) } else { failure(noise.error) }
                } else {
                    BlueEmptyState(systemImage: "waveform.badge.magnifyingglass", title: "Noise analysis",
                                   message: "Output noise density of a net over frequency: resistor thermal noise, diode and transistor shot and flicker noise, op-amp EN= / IN= densities. Referred to the input source and integrated to an RMS value.",
                                   actionTitle: "Run Noise") { runNoise() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        case .paramSweep:
            resultPanel {
                if let sweep = store.paramSweepResult {
                    if sweep.ok { ParamSweepPanel(result: sweep) } else { failure(sweep.error) }
                } else {
                    BlueEmptyState(systemImage: "slider.horizontal.3", title: "Parameter sweep",
                                   message: "Runs a DC operating point, an AC sweep or a transient once per value of one part (a resistor, capacitor, source or a part number such as TL072) and overlays the results. The design is not changed.",
                                   actionTitle: "Run Sweep") { runParamSweep() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        case .fft:
            resultPanel {
                if let fft = store.fftResult {
                    if fft.ok { FFTPanel(result: fft) } else { failure(fft.error) }
                } else {
                    BlueEmptyState(systemImage: "chart.bar.xaxis", title: "FFT and THD",
                                   message: "Runs a transient and analyses a net over whole periods of the first SIN source in the second half of the run: spectrum, harmonics and total harmonic distortion. Use a step of 1/500 of the period or finer.",
                                   actionTitle: "Run FFT") { runFFT() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        case .monteCarlo:
            resultPanel {
                if let mc = store.monteCarloResult {
                    if mc.ok { MonteCarloPanel(result: mc) } else { failure(mc.error) }
                } else {
                    BlueEmptyState(systemImage: "chart.bar", title: "Monte Carlo and worst case",
                                   message: "Varies every resistor, capacitor and inductor within its tolerance (a % in the value, else R 1 %, C 10 %, L 20 %) and measures a net's DC voltage or −3 dB bandwidth.",
                                   actionTitle: "Run Monte Carlo") { runMonteCarlo() }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                }
            }
        }
    }

    private func resultPanel<Content: View>(@ViewBuilder _ content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            content()
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading)
        .padding(14)
        .bluePanel()
        .padding(10)
    }

    private func failure(_ message: String) -> some View {
        VStack(alignment: .leading) {
            Label(message, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
            Spacer()
        }
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
                            .onTapGesture { store.reveal(component: d.component) }
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
                    Chart {
                        ForEach(samples(tr)) { s in
                            LineMark(x: .value("Time (s)", s.t), y: .value(showCurrents ? "Current (A)" : "Voltage (V)", s.v))
                                .foregroundStyle(by: .value("Signal", s.series))
                                .interpolationMethod(.linear)
                        }
                        if let a = cursorA {
                            RuleMark(x: .value("A", a)).foregroundStyle(Theme.selection.opacity(0.8))
                                .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
                                .annotation(position: .top, alignment: .leading) { Text(verbatim: "A").font(.caption2).foregroundStyle(Theme.selection) }
                        }
                        if let b = cursorB {
                            RuleMark(x: .value("B", b)).foregroundStyle(Theme.warning.opacity(0.9))
                                .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
                                .annotation(position: .top, alignment: .leading) { Text(verbatim: "B").font(.caption2).foregroundStyle(Theme.warning) }
                        }
                    }
                    .chartForegroundStyleScale(domain: seriesLabels(tr), range: seriesColors(tr))
                    .chartOverlay { proxy in
                        GeometryReader { geometry in
                            Rectangle().fill(.clear).contentShape(Rectangle())
                                .onTapGesture(coordinateSpace: .local) { location in
                                    guard let plot = proxy.plotFrame else { return }
                                    let x = location.x - geometry[plot].origin.x
                                    guard let t: Double = proxy.value(atX: x), t.isFinite else { return }
                                    if placingB { cursorB = t } else { cursorA = t }
                                }
                        }
                    }
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
                    .frame(minHeight: tr.mcus.isEmpty ? 220 : 150)  // leave room for the serial monitor

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
                    WaveformToolsPanel(time: tr.time,
                                       series: (showCurrents ? tr.currents : tr.nets).filter { !hiddenSeries.contains($0.label) },
                                       unit: showCurrents ? "A" : "V",
                                       cursorA: $cursorA, cursorB: $cursorB, placingB: $placingB)
                    if !tr.mcus.isEmpty { McuRunPanel(runs: tr.mcus) }
                } else {
                    Label(tr.error, systemImage: "exclamationmark.triangle.fill").foregroundStyle(Theme.warning)
                    if !tr.mcus.isEmpty { McuRunPanel(runs: tr.mcus) }
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

/// Microcontrollers of the last transient run: status and the serial monitor (USART0 output).
private struct McuRunPanel: View {
    var runs: [McuRun]

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ForEach(runs) { run in
                VStack(alignment: .leading, spacing: 4) {
                    HStack(spacing: 6) {
                        Image(systemName: run.running ? "cpu.fill" : "cpu")
                            .foregroundStyle(run.running ? Theme.skyBlue : Theme.warning)
                        Text("\(run.ref) · \(run.model)").font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                        Text(run.status)
                            .font(.caption)
                            .foregroundStyle(run.running ? Theme.textSecondary : Theme.warning)
                            .lineLimit(2)
                            .truncationMode(.tail)
                        Spacer(minLength: 0)
                    }
                    ScrollView {
                        Text(run.serial.isEmpty ? "No serial output." : run.serial)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(run.serial.isEmpty ? Theme.textMuted : Theme.probe)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                            .padding(6)
                    }
                    .frame(height: 96)
                    .background(RoundedRectangle(cornerRadius: 6).fill(Theme.navy))
                    .accessibilityLabel("Serial monitor for \(run.ref)")
                }
            }
        }
    }
}

/// Starts / stops the live board simulation.
private struct LiveRunButton: View {
    @EnvironmentObject private var store: DesignStore
    @ObservedObject var live: LiveSimulation

    var body: some View {
        if live.isRunning {
            Button { live.stop() } label: { Label("Stop Live", systemImage: "stop.circle.fill") }
                .foregroundStyle(Theme.liveOn)
                .help("Stop the live simulation")
        } else {
            Button { live.start(store: store) } label: { Label("Run Live", systemImage: "bolt.circle.fill") }
                .help("Run the board in real time: firmware, LEDs, switches, scope and serial monitor")
        }
    }
}

/// The live board panel while it runs, the analysis panels otherwise.
private struct LiveSwitcher<Normal: View>: View {
    @ObservedObject var live: LiveSimulation
    @ViewBuilder var normal: Normal

    var body: some View {
        if live.isRunning {
            LiveBoardPanel(live: live)
        } else {
            VStack(spacing: 0) {
                if let error = live.error {
                    Label(error, systemImage: "exclamationmark.triangle.fill")
                        .font(.callout)
                        .foregroundStyle(Theme.warning)
                        .padding(8)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(Theme.deepBlue)
                }
                normal
            }
        }
    }
}

/// Real-time board: controls, switches and push-buttons, LEDs, probes, a scope and the serial monitors.
private struct LiveBoardPanel: View {
    @EnvironmentObject private var store: DesignStore
    @ObservedObject var live: LiveSimulation
    @State private var serialInput: [Int: String] = [:]

    var body: some View {
        VStack(spacing: 0) {
            controls
            HStack(alignment: .top, spacing: 0) {
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        switchesSection
                        ledsSection
                        probesSection
                    }
                    .padding(12)
                }
                .frame(width: 250)
                Rectangle().fill(Theme.blue.opacity(0.3)).frame(width: 1)
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        scope
                        ForEach(live.state?.mcus ?? []) { run in serialMonitor(run) }
                    }
                    .padding(12)
                }
                .frame(minWidth: 300, maxWidth: .infinity)
            }
        }
        .background(Theme.navy)
    }

    private var controls: some View {
        HStack(spacing: 10) {
            Circle().fill(live.isPaused ? Theme.warning : Theme.liveOn).frame(width: 9, height: 9)
            Text("Live board").font(.headline).foregroundStyle(Theme.textPrimary)
            Text("t = \(EngineeringFormat.string(live.state?.time ?? 0, unit: "s", digits: 4))")
                .font(.callout.monospacedDigit())
                .foregroundStyle(Theme.textSecondary)
            Text(String(format: "×%.2f real time", live.realTimeFactor))
                .font(.caption.monospacedDigit())
                .foregroundStyle(Theme.textMuted)
                .help("Simulated seconds per second achieved")
            Spacer(minLength: 8)
            Picker("Speed", selection: $live.speed) {
                ForEach(LiveSimulation.speeds, id: \.self) { s in Text(s < 1 ? String(format: "%g×", s) : "\(Int(s))×").tag(s) }
            }
            .frame(width: 120)
            .help("Simulation speed relative to real time (slow motion shows fast signals)")
            Picker("Step", selection: $live.resolution) {
                ForEach(LiveSimulation.resolutions, id: \.self) { r in Text(EngineeringFormat.string(r, unit: "s")).tag(r) }
            }
            .frame(width: 120)
            .help("Analog time step: finer resolves faster signals, coarser runs faster")
            Button {
                if live.isPaused { live.resume() } else { live.pause() }
            } label: { Image(systemName: live.isPaused ? "play.fill" : "pause.fill") }
                .accessibilityLabel(live.isPaused ? "Resume" : "Pause")
            Button { live.start(store: store) } label: { Image(systemName: "arrow.counterclockwise") }
                .help("Restart from t = 0")
                .accessibilityLabel("Restart live simulation")
            Button { live.stop() } label: { Image(systemName: "stop.fill") }
                .accessibilityLabel("Stop live simulation")
        }
        .buttonStyle(.borderless)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(Theme.deepBlue)
    }

    @ViewBuilder private var switchesSection: some View {
        let switches = live.state?.switches ?? []
        sectionTitle("Switches & buttons", "switch.2")
        if switches.isEmpty {
            Text("No switches. Add a Switch to the schematic (value \"push\" for a push-button).")
                .font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
        }
        ForEach(switches, id: \.component) { sw in
            let closed = live.isClosed(sw.component) ?? sw.closed
            HStack {
                Text(sw.ref).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
                Spacer()
                Text(closed ? "closed" : "open").font(.caption).foregroundStyle(closed ? Theme.liveOn : Theme.textMuted)
                if sw.momentary {
                    Text("Hold")
                        .font(.caption.weight(.semibold))
                        .padding(.horizontal, 10).padding(.vertical, 4)
                        .background(Capsule().fill(closed ? Theme.liveOn : Theme.blue.opacity(0.35)))
                        .foregroundStyle(closed ? Theme.deepBlue : Theme.textPrimary)
                        .gesture(DragGesture(minimumDistance: 0)
                            .onChanged { _ in if !(live.isClosed(sw.component) ?? false) { live.press(sw.component, pressed: true) } }
                            .onEnded { _ in live.press(sw.component, pressed: false) })
                        .accessibilityLabel("Hold \(sw.ref)")
                        .accessibilityAddTraits(.isButton)
                } else {
                    Toggle("", isOn: Binding(get: { closed }, set: { _ in live.press(sw.component, pressed: true) }))
                        .toggleStyle(.switch)
                        .labelsHidden()
                        .controlSize(.small)
                        .accessibilityLabel("Switch \(sw.ref)")
                }
            }
        }
    }

    @ViewBuilder private var ledsSection: some View {
        let leds = live.state?.leds ?? []
        if !leds.isEmpty {
            sectionTitle("LEDs", "lightbulb.fill")
            ForEach(leds, id: \.component) { led in
                let colour = SchematicCanvas.ledColour(store.snapshot.component(led.component)?.value ?? "")
                HStack(spacing: 8) {
                    Circle()
                        .fill(colour.opacity(0.15 + 0.85 * led.brightness))
                        .frame(width: 14, height: 14)
                        .shadow(color: colour.opacity(led.brightness), radius: 6 * led.brightness)
                    Text(led.ref).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
                    Spacer()
                    Text(EngineeringFormat.string(led.current, unit: "A", digits: 3))
                        .font(.caption.monospacedDigit()).foregroundStyle(Theme.textSecondary)
                }
            }
        }
    }

    @ViewBuilder private var probesSection: some View {
        let nets = live.state?.nets ?? []
        sectionTitle("Probes", "scope")
        ForEach(nets, id: \.index) { net in
            let shown = live.scopeNets.contains(net.index)
            Button {
                if shown { live.scopeNets.remove(net.index) } else if live.scopeNets.count < 6 { live.scopeNets.insert(net.index) }
            } label: {
                HStack {
                    Image(systemName: shown ? "checkmark.square.fill" : "square")
                        .foregroundStyle(shown ? Theme.skyBlue : Theme.textMuted)
                    Text(net.name).font(.caption.monospaced()).foregroundStyle(Theme.textSecondary).lineLimit(1)
                    Spacer()
                    Text(EngineeringFormat.string(net.voltage, unit: "V", digits: 3))
                        .font(.caption.monospacedDigit()).foregroundStyle(Theme.probe)
                }
            }
            .buttonStyle(.plain)
            .help(shown ? "Remove from the scope" : "Show on the scope")
        }
    }

    private var scope: some View {
        let names = Dictionary(uniqueKeysWithValues: (live.state?.nets ?? []).map { ($0.index, $0.name) })
        // At most ~600 points per trace on screen.
        let stride = max(1, live.scopeTime.count / 600)
        let times = Swift.stride(from: 0, to: live.scopeTime.count, by: stride).map { live.scopeTime[$0] }
        let series = live.scopeNets.sorted().compactMap { index -> (String, [Double])? in
            guard let values = live.scopeValues[index], values.count == live.scopeTime.count else { return nil }
            return (names[index] ?? "N\(index)", Swift.stride(from: 0, to: values.count, by: stride).map { values[$0] })
        }
        return VStack(alignment: .leading, spacing: 6) {
            HStack {
                sectionTitle("Scope", "waveform.path.ecg")
                Spacer()
                Picker("Window", selection: $live.scopeWindow) {
                    Text("20 ms").tag(0.02)
                    Text("200 ms").tag(0.2)
                    Text("2 s").tag(2.0)
                    Text("10 s").tag(10.0)
                }
                .frame(width: 130)
            }
            Chart {
                ForEach(Array(series.enumerated()), id: \.offset) { i, s in
                    ForEach(Array(zip(times, s.1).enumerated()), id: \.offset) { _, point in
                        LineMark(x: .value("t", point.0), y: .value("V", point.1), series: .value("Net", s.0))
                            .foregroundStyle(Theme.seriesColors[i % Theme.seriesColors.count])
                    }
                }
            }
            .chartXAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let t = value.as(Double.self) {
                            Text(EngineeringFormat.string(t, unit: "s", digits: 3)).foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .chartYAxis {
                AxisMarks { value in
                    AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                    AxisValueLabel {
                        if let v = value.as(Double.self) {
                            Text(EngineeringFormat.string(v, unit: "V", digits: 2)).foregroundStyle(Theme.textMuted)
                        }
                    }
                }
            }
            .frame(height: 180)
            .accessibilityLabel("Live scope")
            HStack(spacing: 10) {
                ForEach(Array(series.enumerated()), id: \.offset) { i, s in
                    HStack(spacing: 4) {
                        Circle().fill(Theme.seriesColors[i % Theme.seriesColors.count]).frame(width: 8, height: 8)
                        Text(s.0).font(.caption.monospaced()).foregroundStyle(Theme.textSecondary)
                    }
                }
            }
        }
        .padding(10)
        .bluePanel()
    }

    private func serialMonitor(_ run: McuRun) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 6) {
                Image(systemName: run.running ? "cpu.fill" : "cpu").foregroundStyle(run.running ? Theme.liveOn : Theme.warning)
                Text("\(run.ref) · \(run.model) · serial monitor").font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                Spacer()
            }
            Text(run.status).font(.caption).foregroundStyle(run.running ? Theme.textSecondary : Theme.warning)
                .lineLimit(2)
            ScrollViewReader { proxy in
                ScrollView {
                    Text(run.serial.isEmpty ? "No serial output yet." : run.serial)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(run.serial.isEmpty ? Theme.textMuted : Theme.probe)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(6)
                    Color.clear.frame(height: 1).id("end")
                }
                .frame(height: 120)
                .background(RoundedRectangle(cornerRadius: 6).fill(Theme.navy))
                .onChange(of: run.serial) { _, _ in proxy.scrollTo("end", anchor: .bottom) }
            }
            HStack {
                TextField("Send to \(run.ref)…", text: Binding(get: { serialInput[run.component] ?? "" },
                                                                 set: { serialInput[run.component] = $0 }))
                    .textFieldStyle(.blue)
                    .onSubmit { send(run) }
                Button("Send") { send(run) }
                    .disabled((serialInput[run.component] ?? "").isEmpty)
            }
        }
        .padding(10)
        .bluePanel()
    }

    private func send(_ run: McuRun) {
        let text = serialInput[run.component] ?? ""
        guard !text.isEmpty else { return }
        live.sendSerial(text + "\n", to: run.component)
        serialInput[run.component] = ""
    }

    private func sectionTitle(_ title: String, _ icon: String) -> some View {
        Label(title, systemImage: icon).font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
    }
}
