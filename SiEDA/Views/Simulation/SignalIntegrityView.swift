import Charts
import SwiftUI

/// Circuit simulation or board-level signal / power integrity in the Simulation workspace.
enum AnalysisMode: String, CaseIterable, Identifiable {
    case circuit, integrity
    var id: String { rawValue }
}

struct AnalysisModePicker: View {
    @Binding var mode: AnalysisMode

    var body: some View {
        Picker("", selection: $mode) {
            Text("Circuit").tag(AnalysisMode.circuit)
            Text("SI / PI").tag(AnalysisMode.integrity)
        }
        .pickerStyle(.segmented)
        .labelsHidden()
        .frame(width: 150)
    }
}

/// Signal and power integrity of the routed board: transmission-line waveforms per net, crosstalk and return path,
/// and the PDN impedance of every rail against its target.
struct SignalIntegrityView: View {
    @EnvironmentObject private var store: DesignStore
    @Binding var mode: AnalysisMode
    @State private var tab: Panel = .signals
    @State private var settings: SISettings = .empty
    @State private var nets: [SINetSummary] = []
    @State private var selectedNet: String?
    @State private var analysis: SINetAnalysis?
    @State private var crosstalk: SICrosstalkReport = .empty
    @State private var pdn: PDNReport = .empty
    @State private var selectedRail: String?
    @State private var rippleText = ""
    @State private var stepText = ""

    enum Panel: String, CaseIterable, Identifiable {
        case signals, crosstalk, power, channel
        var id: String { rawValue }
    }

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                AnalysisModePicker(mode: $mode)
                Divider().frame(height: 18)
                Image(systemName: "waveform.path").foregroundStyle(Theme.blue)
                Text("Signal & Power Integrity").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Picker("", selection: $tab) {
                    Text("Signals").tag(Panel.signals)
                    Text("Crosstalk").tag(Panel.crosstalk)
                    Text("Power").tag(Panel.power)
                    Text("Channel").tag(Panel.channel)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 360)
                Spacer()
                Button {
                    store.importIBIS()
                    reload()
                } label: { Label("Import IBIS…", systemImage: "square.and.arrow.down") }
                    .help("Import an IBIS (.ibs) file; assign its models to nets below")
                Toggle("Sign-off in verification", isOn: Binding(get: { settings.signOff },
                                                                 set: { store.setSIOptions(signOff: $0); reload() }))
                    .help("Adds a Signal & Power Integrity stage to Design Checks")
            }
            .buttonStyle(.borderless)

            switch tab {
            case .signals: signalsPanel
            case .crosstalk: crosstalkPanel
            case .power: powerPanel
            case .channel: ChannelPanel(nets: nets, settings: settings)
            }
        }
        .background(Theme.navy)
        .task(id: store.revision) { reload() }
    }

    private func reload() {
        settings = store.siSettings()
        nets = store.siNets()
        if selectedNet == nil || !nets.contains(where: { $0.name == selectedNet }) { selectedNet = nets.first?.name }
        analysis = selectedNet.flatMap { store.siNet($0) }
        crosstalk = store.siCrosstalk()
        pdn = store.pdn()
        if selectedRail == nil || !pdn.rails.contains(where: { $0.name == selectedRail }) { selectedRail = pdn.rails.first?.name }
        loadRailFields()
    }

    private func select(net: String) {
        selectedNet = net
        analysis = store.siNet(net)
    }

    // MARK: Signals

    private var signalsPanel: some View {
        HStack(spacing: 0) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    if nets.isEmpty {
                        Text("No signal nets to analyse").font(.callout).foregroundStyle(Theme.textMuted).padding(8)
                    }
                    ForEach(nets) { net in
                        Button { select(net: net.name) } label: {
                            HStack(spacing: 6) {
                                Image(systemName: net.critical ? "exclamationmark.triangle.fill" : "checkmark.circle")
                                    .foregroundStyle(net.critical ? Theme.warning : Theme.textMuted)
                                Text(verbatim: net.name).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
                                    .lineLimit(1)
                                Spacer(minLength: 4)
                                Text(verbatim: String(format: "%.1f mm", net.length))
                                    .font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
                            }
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .background(RoundedRectangle(cornerRadius: 5)
                                .fill(selectedNet == net.name ? Theme.blue.opacity(0.3) : Color.clear))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(8)
            }
            .frame(width: 260)
            .bluePanel()
            .padding(10)
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if let a = analysis {
                        netDetail(a)
                    } else {
                        Text("Select a net to analyse").foregroundStyle(Theme.textMuted)
                    }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .bluePanel()
            .padding(10)
        }
    }

    @ViewBuilder private func netDetail(_ a: SINetAnalysis) -> some View {
        HStack {
            Image(systemName: a.ok ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                .foregroundStyle(a.ok ? Theme.success : Theme.warning)
            Text(verbatim: a.name).font(.headline).foregroundStyle(Theme.textPrimary)
            Spacer()
            Picker("Driver model", selection: Binding(get: { settings.netModels[a.name] ?? "" },
                                                      set: { store.assignSIModel(net: a.name, modelID: $0); reload() })) {
                Text("Automatic").tag("")
                ForEach(settings.allModels) { m in Text(verbatim: m.name).tag(m.id) }
            }
            .frame(width: 320)
        }
        if !a.error.isEmpty {
            Label(a.error, systemImage: "info.circle").foregroundStyle(Theme.textMuted)
        } else {
            Text(verbatim: driverSummary(a)).font(.callout).foregroundStyle(Theme.textSecondary)
            metricsGrid(a)
            waveformChart(a)
            if !a.recommendation.isEmpty {
                Label(a.recommendation, systemImage: "lightbulb").font(.callout).foregroundStyle(Theme.textPrimary)
                    .fixedSize(horizontal: false, vertical: true)
            }
            ForEach(a.receivers) { r in
                Text(verbatim: receiverSummary(r))
                    .font(.caption.monospaced())
                    .foregroundStyle(r.ok ? Theme.textSecondary : Theme.warning)
            }
            ForEach(a.sections) { s in
                Text(verbatim: sectionSummary(s))
                    .font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
            }
            ForEach(a.terminations + a.notes, id: \.self) { note in
                Text(verbatim: note).font(.caption).foregroundStyle(Theme.textMuted)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
    }

    /// "U1 1 (OUT) · LVCMOS 3.3 V · 1 ns · 30 Ω + R1 22 Ω"
    private func driverSummary(_ a: SINetAnalysis) -> String {
        var text = "\(a.driver.ref) \(a.driver.pin) · \(a.driver.name) · "
        text += EngineeringFormat.string(a.driver.riseTime, unit: "s", digits: 2)
        text += String(format: " · %.0f Ω", a.driver.rOut)
        if a.seriesR > 0 { text += String(format: " + %@ %.0f Ω", a.seriesRef, a.seriesR) }
        return text
    }

    /// "U2 2 (IN) · 30 % / 28 % · 861 ps" (overshoot / undershoot of the swing, flight time).
    private func receiverSummary(_ r: SIReceiverResult) -> String {
        guard r.connected else { return "\(r.ref) \(r.pin) · —" }
        let levels = String(format: "%.0f %% / %.0f %% · ", r.metrics.overshootPercent, r.metrics.undershootPercent)
        return "\(r.ref) \(r.pin) · " + levels + EngineeringFormat.string(r.metrics.flightTime, unit: "s", digits: 2)
    }

    /// "Top · 0.250 mm · 98.5 mm · 58.7 Ω · 650 ps"
    private func sectionSummary(_ s: SILineSection) -> String {
        String(format: "%@ · %.3f mm · %.1f mm · %.1f Ω · ", s.layer, s.width, s.length, s.z0)
            + EngineeringFormat.string(s.delay, unit: "s", digits: 3)
    }

    private func metricsGrid(_ a: SINetAnalysis) -> some View {
        let m = a.worst?.metrics
        return Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 4) {
            GridRow {
                metric("Length", String(format: "%.1f mm", a.length) + (a.estimated ? " ≈" : ""))
                metric("Delay", EngineeringFormat.string(a.delay, unit: "s", digits: 3))
                metric("Impedance", a.z0Min > 0 ? String(format: "%.0f – %.0f Ω", a.z0Min, a.z0Max) : "—")
                metric("Critical length", String(format: "%.0f mm", a.criticalLength), warn: a.critical)
            }
            GridRow {
                metric("Overshoot", m.map { String(format: "%.0f %%", $0.overshootPercent) } ?? "—", warn: !a.ok)
                metric("Undershoot", m.map { String(format: "%.0f %%", $0.undershootPercent) } ?? "—", warn: !a.ok)
                metric("Settling", m.map { EngineeringFormat.string($0.settling, unit: "s", digits: 2) } ?? "—")
                metric("Flight time", m.map { EngineeringFormat.string($0.flightTime, unit: "s", digits: 2) } ?? "—")
            }
        }
    }

    private func metric(_ title: LocalizedStringKey, _ value: String, warn: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            Text(verbatim: value).font(.callout.monospacedDigit()).foregroundStyle(warn ? Theme.warning : Theme.probe)
        }
    }

    private struct WavePoint: Identifiable {
        let id: Int
        let series: Int  // 0 driver, 1 receiver, 2 receiver with the recommended termination
        let t: Double
        let v: Double
    }

    private static let waveColors: [Color] = [Theme.lightBlue, Theme.probe, Theme.liveOn]

    private func wavePoints(_ a: SINetAnalysis) -> [WavePoint] {
        var out: [WavePoint] = []
        let traces: [(Int, [Double]?, [Double])] = [
            (0, a.waveform.driver, a.waveform.time),
            (1, a.waveform.receiver, a.waveform.time),
            (2, a.terminatedWaveform?.receiver, a.terminatedWaveform?.time ?? []),
        ]
        for (series, values, time) in traces {
            guard let values else { continue }
            for i in 0..<min(values.count, time.count) {
                out.append(WavePoint(id: out.count, series: series, t: time[i], v: values[i]))
            }
        }
        return out
    }

    @ViewBuilder private func waveformChart(_ a: SINetAnalysis) -> some View {
        Chart(wavePoints(a)) { p in
            LineMark(x: .value("Time (s)", p.t), y: .value("Voltage (V)", p.v), series: .value("Signal", p.series))
                .foregroundStyle(Self.waveColors[p.series])
                .interpolationMethod(.linear)
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
                        Text(EngineeringFormat.string(v, unit: "V", digits: 2)).foregroundStyle(Theme.textMuted)
                    }
                }
            }
        }
        .frame(minHeight: 220)
        .accessibilityLabel(Text(verbatim: a.name))
        HStack(spacing: 14) {
            legend("Driver", Self.waveColors[0])
            legend("Receiver", Self.waveColors[1])
            if a.terminatedWaveform != nil { legend("With series termination", Self.waveColors[2]) }
        }
    }

    private func legend(_ title: LocalizedStringKey, _ color: Color) -> some View {
        HStack(spacing: 5) {
            Circle().fill(color).frame(width: 8, height: 8)
            Text(title).font(.caption).foregroundStyle(Theme.textSecondary)
        }
    }

    // MARK: Crosstalk and return path

    private var crosstalkPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 8) {
                Text("Crosstalk").font(.headline).foregroundStyle(Theme.textPrimary)
                ForEach(crosstalk.pairs) { p in
                    HStack(alignment: .firstTextBaseline, spacing: 8) {
                        Image(systemName: p.ok ? "checkmark.circle" : "exclamationmark.triangle.fill")
                            .foregroundStyle(p.ok ? Theme.textMuted : Theme.warning)
                        Text(verbatim: "\(p.aggressor) → \(p.victim)").font(.callout.monospaced())
                            .foregroundStyle(Theme.textPrimary)
                        if p.broadside == true {
                            Text("Broadside").font(.caption).foregroundStyle(Theme.warning)
                                .help("The victim runs on the adjacent layer, over or under the aggressor")
                        }
                        Spacer()
                        Text(verbatim: pairSummary(p))
                            .font(.caption.monospacedDigit())
                            .foregroundStyle(p.ok ? Theme.textSecondary : Theme.warning)
                    }
                }
                Text("Return path").font(.headline).foregroundStyle(Theme.textPrimary).padding(.top, 8)
                ForEach(crosstalk.returnPath) { issue in
                    Label(issue.message, systemImage: "arrow.uturn.backward.circle")
                        .font(.callout).foregroundStyle(Theme.warning)
                        .fixedSize(horizontal: false, vertical: true)
                }
                if crosstalk.pairs.allSatisfy(\.ok) && crosstalk.returnPath.isEmpty {
                    Text("No crosstalk or return-path problems found").font(.callout).foregroundStyle(Theme.textMuted)
                }
            }
            .padding(14)
            .frame(maxWidth: .infinity, alignment: .leading)
        }
        .bluePanel()
        .padding(10)
    }

    /// "Top · 60.0 mm @ 0.20 mm · NEXT 4.1 % · FEXT -0.9 % · 89 mV / 165 mV"
    private func pairSummary(_ p: SICrosstalkPair) -> String {
        String(format: "%@ · %.1f mm @ %.2f mm · NEXT %.1f %% · FEXT %.1f %% · ", p.layer, p.coupledLength, p.spacing,
               100 * p.next, 100 * p.fext)
            + EngineeringFormat.string(p.noise, unit: "V", digits: 2) + " / " + EngineeringFormat.string(p.limit, unit: "V", digits: 2)
    }

    // MARK: Power

    private var currentRail: PDNRail? { pdn.rails.first { $0.name == selectedRail } }

    private func loadRailFields() {
        guard let rail = currentRail else { return }
        rippleText = String(format: "%g", rail.ripplePercent)
        stepText = String(format: "%g", rail.transientCurrent)
    }

    private var powerPanel: some View {
        HStack(spacing: 0) {
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    if pdn.rails.isEmpty {
                        Text("No supply rails to analyse").font(.callout).foregroundStyle(Theme.textMuted).padding(8)
                    }
                    ForEach(pdn.rails) { rail in
                        Button {
                            selectedRail = rail.name
                            loadRailFields()
                        } label: {
                            HStack(spacing: 6) {
                                Image(systemName: rail.compliant ? "checkmark.circle" : "exclamationmark.triangle.fill")
                                    .foregroundStyle(rail.compliant ? Theme.textMuted : Theme.warning)
                                Text(verbatim: rail.name).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
                                Spacer(minLength: 4)
                                Text(verbatim: String(format: "%.2f V", rail.voltage))
                                    .font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
                            }
                            .padding(.horizontal, 8).padding(.vertical, 4)
                            .background(RoundedRectangle(cornerRadius: 5)
                                .fill(selectedRail == rail.name ? Theme.blue.opacity(0.3) : Color.clear))
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
                .padding(8)
            }
            .frame(width: 220)
            .bluePanel()
            .padding(10)
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    if let rail = currentRail { railDetail(rail) }
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .bluePanel()
            .padding(10)
        }
    }

    @ViewBuilder private func railDetail(_ rail: PDNRail) -> some View {
        HStack(spacing: 10) {
            Text(verbatim: rail.name).font(.headline).foregroundStyle(Theme.textPrimary)
            Text(verbatim: railSummary(rail)).font(.callout).foregroundStyle(Theme.textSecondary)
            Spacer()
            Text("Ripple").font(.caption).foregroundStyle(Theme.textMuted)
            TextField("", text: $rippleText).textFieldStyle(.blue).frame(width: 50).onSubmit { applyRail(rail) }
            Text("Load step").font(.caption).foregroundStyle(Theme.textMuted)
            TextField("", text: $stepText).textFieldStyle(.blue).frame(width: 60).onSubmit { applyRail(rail) }
        }
        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 4) {
            GridRow {
                metric("Target", EngineeringFormat.string(rail.target, unit: "Ω", digits: 3))
                metric("Impedance", EngineeringFormat.string(rail.worstZ, unit: "Ω", digits: 3) + " @ "
                       + EngineeringFormat.string(rail.worstF, unit: "Hz", digits: 2), warn: !rail.compliant)
                metric("Decoupling", "\(rail.decaps.count)", warn: rail.decaps.isEmpty)
                metric("IR drop", rail.irDrop.analyzed
                       ? EngineeringFormat.string(rail.irDrop.worst, unit: "V", digits: 3)
                           + String(format: " (%.1f %%)", rail.voltage > 0 ? 100 * rail.irDrop.worst / rail.voltage : 0)
                       : "—",
                       warn: rail.irDrop.analyzed && rail.voltage > 0
                           && rail.irDrop.worst > rail.irDrop.limitPercent / 100 * rail.voltage)
            }
        }
        impedanceChart(rail)
        ForEach(rail.recommendations, id: \.self) { text in
            Label(text, systemImage: "lightbulb").font(.callout).foregroundStyle(Theme.textPrimary)
                .fixedSize(horizontal: false, vertical: true)
        }
        ForEach(rail.decaps) { d in
            Text(verbatim: decapSummary(d))
                .font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
        }
        if !rail.irDrop.note.isEmpty {
            Text(verbatim: rail.irDrop.note).font(.caption).foregroundStyle(Theme.textMuted)
        }
        PowerPlanningSection(rail: rail)
    }

    /// "C1 100n C_0402 · ESR 30 mΩ · ESL 1 nH · SRF 16 MHz" (ESL includes the mounting inductance).
    private func decapSummary(_ d: PDNDecap) -> String {
        var text = "\(d.ref) \(d.value) \(d.footprint) · ESR " + EngineeringFormat.string(d.esr, unit: "Ω", digits: 2)
        text += " · ESL " + EngineeringFormat.string(d.esl + d.mounting, unit: "H", digits: 2)
        text += " · SRF " + EngineeringFormat.string(d.srf, unit: "Hz", digits: 2)
        return text
    }

    /// "3.30 V · 500 mA · J1 (connector)": voltage, DC load (≈ when estimated) and what feeds the rail.
    private func railSummary(_ rail: PDNRail) -> String {
        var text = String(format: "%.2f V · ", rail.voltage)
        text += EngineeringFormat.string(rail.dcCurrent, unit: "A", digits: 3)
        if rail.currentEstimated { text += " ≈" }
        if !rail.vrmRef.isEmpty { text += " · \(rail.vrmRef) (\(rail.vrmKind))" }
        return text
    }

    private func applyRail(_ rail: PDNRail) {
        let ripple = Double(rippleText.replacingOccurrences(of: ",", with: ".")) ?? 0
        let step = Double(stepText.replacingOccurrences(of: ",", with: ".")) ?? 0
        store.setPDNRail(rail.name, ripplePercent: max(0, ripple), transientAmps: max(0, step))
        reload()
    }

    private struct ImpedancePoint: Identifiable {
        let id: Int
        let logF: Double
        let logZ: Double
    }

    /// |Z| against frequency on log–log axes (plotted as decades), with the target impedance.
    @ViewBuilder private func impedanceChart(_ rail: PDNRail) -> some View {
        let points = zip(rail.curve.freq, rail.curve.z).enumerated().compactMap { index, pair -> ImpedancePoint? in
            pair.0 > 0 && pair.1 > 0 ? ImpedancePoint(id: index, logF: log10(pair.0), logZ: log10(pair.1)) : nil
        }
        let target = rail.target > 0 ? log10(rail.target) : nil
        let zValues = points.map(\.logZ) + (target.map { [$0] } ?? [])
        let low = floor(zValues.min() ?? -3), high = ceil(zValues.max() ?? 1)
        Chart {
            ForEach(points) { p in
                LineMark(x: .value("Frequency", p.logF), y: .value("Impedance", p.logZ))
                    .foregroundStyle(Theme.probe)
                    .interpolationMethod(.linear)
            }
            if let target {
                RuleMark(y: .value("Target", target))
                    .foregroundStyle(Theme.warning)
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [4, 3]))
            }
            RuleMark(x: .value("Band", log10(rail.fMax)))
                .foregroundStyle(Theme.textMuted.opacity(0.5))
                .lineStyle(StrokeStyle(lineWidth: 1, dash: [2, 3]))
        }
        .chartXScale(domain: 3.0...9.0)
        .chartYScale(domain: low...max(high, low + 1))
        .chartXAxis {
            AxisMarks(values: Array(stride(from: 3.0, through: 9.0, by: 1.0))) { value in
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
        .frame(minHeight: 240)
        .accessibilityLabel(Text(verbatim: rail.name))
    }
}
