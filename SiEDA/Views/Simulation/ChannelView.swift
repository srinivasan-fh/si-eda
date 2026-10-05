import Charts
import SwiftUI

/// Channel analysis of the routed board: per-layer line loss, S-parameters of a net or differential pair (with an
/// imported Touchstone block cascaded at the receiver if wanted), the step response lossy against lossless, and the
/// PRBS eye with optional CTLE / FFE and a mask.
struct ChannelPanel: View {
    @EnvironmentObject private var store: DesignStore
    let nets: [SINetSummary]
    let settings: SISettings

    @State private var selectedNet: String?
    @State private var singleEnded = false
    @State private var options = SIChannelSettings()
    @State private var bitRateText = "5"
    @State private var maskText = "0"
    @State private var maskWidthText = "0"
    @State private var rjText = "0"
    @State private var report: SIChannelReport?
    @State private var errorText = ""
    @State private var lineLoss: SILineLossReport = .empty
    @State private var imported: (name: String, text: String, ports: Int)?
    @State private var importedReport: SITouchstoneReport?
    @State private var cascade = false

    var body: some View {
        HStack(spacing: 0) {
            netList
                .frame(width: 240)
                .bluePanel()
                .padding(10)
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    controls
                    if !errorText.isEmpty {
                        Label(errorText, systemImage: "exclamationmark.triangle").foregroundStyle(Theme.warning)
                    }
                    if let report {
                        channelDetail(report)
                    } else if errorText.isEmpty {
                        Text("Select a net to analyse its channel").foregroundStyle(Theme.textMuted)
                    }
                    if let importedReport { importedSection(importedReport) }
                    lineLossSection
                }
                .padding(14)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .bluePanel()
            .padding(10)
        }
        .task(id: store.revision) { lineLoss = store.siLineLoss(roughness: options.roughness) }
    }

    // MARK: Net list and controls

    private var netList: some View {
        ScrollView {
            LazyVStack(alignment: .leading, spacing: 2) {
                if nets.isEmpty {
                    Text("No signal nets to analyse").font(.callout).foregroundStyle(Theme.textMuted).padding(8)
                }
                ForEach(nets) { net in
                    Button {
                        selectedNet = net.name
                        Task { await analyse() }
                    } label: {
                        HStack(spacing: 6) {
                            Image(systemName: net.fast ? "bolt.horizontal.circle" : "circle")
                                .foregroundStyle(net.fast ? Theme.probe : Theme.textMuted)
                            Text(verbatim: net.name).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary).lineLimit(1)
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
    }

    private var controls: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 10) {
                Text("Bit rate (Gb/s)").font(.caption).foregroundStyle(Theme.textMuted)
                TextField("", text: $bitRateText).textFieldStyle(.blue).frame(width: 60)
                Picker("PRBS", selection: $options.prbs) {
                    ForEach([7, 9, 15, 23, 31], id: \.self) { order in Text(verbatim: "PRBS\(order)").tag(order) }
                }
                .frame(width: 150)
                Picker("Driver", selection: $options.idealDriver) {
                    Text("Ideal 50 Ω").tag(true)
                    Text("Net model").tag(false)
                }
                .frame(width: 190)
                Toggle("CTLE", isOn: $options.ctle).help("Receive equaliser, peaking chosen for the widest eye")
                Toggle("FFE", isOn: $options.ffe).help("Transmit FFE, zero-forcing taps (1 pre, 2 post)")
                Toggle("Single-ended", isOn: $singleEnded).help("Analyse one leg of a differential pair alone")
                Spacer()
                Button {
                    Task { await analyse() }
                } label: { Label("Analyse", systemImage: "play.fill") }
                    .disabled(selectedNet == nil || store.isBusy)
            }
            HStack(spacing: 10) {
                Text("Mask (mV)").font(.caption).foregroundStyle(Theme.textMuted)
                TextField("", text: $maskText).textFieldStyle(.blue).frame(width: 50)
                Text("Mask width (UI)").font(.caption).foregroundStyle(Theme.textMuted)
                TextField("", text: $maskWidthText).textFieldStyle(.blue).frame(width: 50)
                Text("RJ (ps)").font(.caption).foregroundStyle(Theme.textMuted)
                TextField("", text: $rjText).textFieldStyle(.blue).frame(width: 50)
                Picker("Roughness", selection: $options.roughness) {
                    Text(verbatim: "Huray").tag("huray")
                    Text(verbatim: "Hammerstad").tag("hammerstad")
                    Text("Smooth").tag("none")
                }
                .frame(width: 200)
                Picker("Copper foil", selection: Binding(get: { settings.copperFoil ?? "" }, set: { store.setCopperFoil($0) })) {
                    Text("By laminate").tag("")
                    Text(verbatim: "HVLP").tag("hvlp")
                    Text(verbatim: "VLP").tag("vlp")
                    Text(verbatim: "RTF").tag("rtf")
                    Text("Standard ED").tag("std")
                    Text("Smooth").tag("smooth")
                }
                .frame(width: 220)
                Spacer()
            }
            HStack(spacing: 12) {
                Button {
                    guard let net = selectedNet else { return }
                    readFields()
                    store.exportChannelTouchstone(net: net, partner: partner, differential: report?.differential ?? false,
                                                  settings: options)
                } label: { Label("Export Touchstone…", systemImage: "square.and.arrow.up") }
                    .disabled(selectedNet == nil)
                Button {
                    importTouchstone()
                } label: { Label("Import Touchstone…", systemImage: "square.and.arrow.down") }
                if let imported {
                    Text(verbatim: imported.name).font(.caption.monospaced()).foregroundStyle(Theme.textSecondary)
                    Toggle("Cascade at receiver", isOn: $cascade)
                        .help("Appends the imported block (connector, cable, backplane) after the routed channel")
                    Button("Clear") {
                        self.imported = nil
                        importedReport = nil
                        cascade = false
                    }
                }
                Spacer()
                Button {
                    guard let net = report?.net else { return }
                    readFields()
                    store.setSIChannel(net, bitRate: options.bitRate, maskHeight: options.maskHeight, maskWidthUi: options.maskWidthUi)
                } label: { Label("Check in sign-off", systemImage: "checkmark.shield") }
                    .disabled(report == nil)
                    .help("Design Checks verify this eye against the mask (needs sign-off on)")
            }
        }
        .buttonStyle(.borderless)
    }

    private var partner: String { singleEnded ? "none" : "" }

    private func number(_ text: String) -> Double {
        Double(text.trimmingCharacters(in: .whitespaces).replacingOccurrences(of: ",", with: ".")) ?? 0
    }

    private func readFields() {
        options.bitRate = min(200, max(0.001, number(bitRateText))) * 1e9
        options.maskHeight = max(0, number(maskText)) / 1000
        options.maskWidthUi = min(0.99, max(0, number(maskWidthText)))
        options.rjRms = max(0, number(rjText)) * 1e-12
    }

    @MainActor private func analyse() async {
        guard let net = selectedNet, !store.isBusy else { return }
        readFields()
        let block = cascade ? imported : nil
        let result = await store.analyzeChannel(net: net, partner: partner, settings: options, touchstone: block?.text,
                                                touchstonePorts: block?.ports ?? 0)
        switch result {
        case .success(let value):
            report = value
            errorText = ""
        case .failure(let error):
            report = nil
            errorText = error.localizedDescription
        }
        lineLoss = store.siLineLoss(roughness: options.roughness)
        if let imported { runImported(imported) }
    }

    @MainActor private func importTouchstone() {
        guard let file = store.chooseTouchstoneFile() else { return }
        imported = file
        readFields()
        runImported(file)
    }

    @MainActor private func runImported(_ file: (name: String, text: String, ports: Int)) {
        do {
            importedReport = try EDAEngine.touchstoneChannel(file.text, ports: file.ports, settings: options)
        } catch {
            importedReport = nil
            imported = nil
            store.present(error, title: "Could not import \(file.name)")
        }
    }

    // MARK: Results

    @ViewBuilder private func channelDetail(_ r: SIChannelReport) -> some View {
        HStack {
            Image(systemName: (r.eye?.open ?? true) && (r.eye?.maskPass ?? true) ? "checkmark.seal.fill" : "exclamationmark.triangle.fill")
                .foregroundStyle((r.eye?.open ?? true) && (r.eye?.maskPass ?? true) ? Theme.success : Theme.warning)
            Text(verbatim: r.differential ? "\(r.net) / \(r.partner)" : r.net).font(.headline).foregroundStyle(Theme.textPrimary)
            Text(verbatim: "\(r.driver.ref) \(r.driver.pin) → \(r.receiver.ref) \(r.receiver.pin) · \(r.driver.model)")
                .font(.callout).foregroundStyle(Theme.textSecondary)
            Spacer()
        }
        Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 4) {
            GridRow {
                metric("Length", String(format: "%.1f mm", r.length) + (r.estimated ? " ≈" : ""))
                metric("Skew", r.differential ? EngineeringFormat.string(r.skew, unit: "s", digits: 2) : "—", warn: abs(r.skew) > 0.1 * (r.eye?.ui ?? 1))
                metric("Insertion loss", r.nyquist.map { String(format: "%.1f dB", $0.il) } ?? "—")
                metric("Return loss", r.nyquist.map { String(format: "%.1f dB", $0.rl) } ?? "—")
            }
            if let e = r.eye {
                GridRow {
                    metric("Eye height", EngineeringFormat.string(e.eyeHeight, unit: "V", digits: 3), warn: !e.open)
                    metric("Eye width", EngineeringFormat.string(e.eyeWidth, unit: "s", digits: 3), warn: !e.open)
                    metric("Total jitter", EngineeringFormat.string(e.totalJitter, unit: "s", digits: 3))
                    metric("Mask margin", options.maskHeight > 0 && options.maskWidthUi > 0
                           ? EngineeringFormat.string(e.maskMargin, unit: "V", digits: 3) : "—", warn: !e.maskPass)
                }
                GridRow {
                    metric("Worst-case eye", EngineeringFormat.string(e.pdaHeight, unit: "V", digits: 3))
                    metric("CTLE", e.ctleDcGainDb < 0 ? String(format: "%.0f dB", e.ctleDcGainDb) : "—")
                    metric("FFE", e.ffeTaps.isEmpty ? "—" : e.ffeTaps.map { String(format: "%.2f", $0) }.joined(separator: " "))
                    metric("PRBS", "PRBS\(e.prbs) · \(Int(e.bits))")
                }
            }
        }
        Text("S-parameters").font(.headline).foregroundStyle(Theme.textPrimary)
        SParameterChart(freq: r.freq, curves: r.curves, nyquist: r.nyquist?.f)
        if let e = r.eye {
            Text("Eye diagram").font(.headline).foregroundStyle(Theme.textPrimary)
            if e.error.isEmpty {
                EyeDiagramView(eye: e, maskHeight: options.maskHeight, maskWidthUi: options.maskWidthUi).frame(height: 260)
            } else {
                Text(verbatim: e.error).foregroundStyle(Theme.warning)
            }
            ForEach(e.notes, id: \.self) { Text(verbatim: $0).font(.caption).foregroundStyle(Theme.textMuted) }
        }
        Text("Step response").font(.headline).foregroundStyle(Theme.textPrimary)
        StepResponseChart(step: r.step)
        if !r.coupled.isEmpty {
            Text("Coupled sections").font(.headline).foregroundStyle(Theme.textPrimary)
            ForEach(r.coupled) { c in
                Text(verbatim: String(format: "%@ · %.1f mm @ %.2f mm · Zdiff %.1f Ω · Zcomm %.1f Ω · Zodd %.1f / Zeven %.1f Ω",
                                      c.layer, c.length, c.gap, c.zDiff, c.zComm, c.zOdd, c.zEven))
                    .font(.caption.monospaced()).foregroundStyle(Theme.textSecondary)
            }
        }
        ForEach(r.notes, id: \.self) { note in
            Text(verbatim: note).font(.caption).foregroundStyle(Theme.textMuted).fixedSize(horizontal: false, vertical: true)
        }
    }

    @ViewBuilder private func importedSection(_ t: SITouchstoneReport) -> some View {
        Divider()
        Text("Imported channel").font(.headline).foregroundStyle(Theme.textPrimary)
        Text(verbatim: "\(imported?.name ?? "") · \(t.ports)-port · \(t.points) points · "
             + EngineeringFormat.string(t.fMin, unit: "Hz", digits: 3) + " – " + EngineeringFormat.string(t.fMax, unit: "Hz", digits: 3)
             + " · " + String(format: "%.0f Ω", t.z0))
            .font(.callout).foregroundStyle(Theme.textSecondary)
        if let error = t.error { Text(verbatim: error).foregroundStyle(Theme.warning) }
        SParameterChart(freq: t.freq, curves: t.curves, nyquist: options.bitRate / 2)
        if let e = t.eye, e.error.isEmpty {
            Text(verbatim: "Eye height " + EngineeringFormat.string(e.eyeHeight, unit: "V", digits: 3) + " · width "
                 + EngineeringFormat.string(e.eyeWidth, unit: "s", digits: 3))
                .font(.callout.monospacedDigit()).foregroundStyle(e.open ? Theme.probe : Theme.warning)
            EyeDiagramView(eye: e, maskHeight: options.maskHeight, maskWidthUi: options.maskWidthUi).frame(height: 220)
        }
    }

    private var lineLossSection: some View {
        VStack(alignment: .leading, spacing: 6) {
            Divider()
            Text("Line loss per layer").font(.headline).foregroundStyle(Theme.textPrimary)
            Text(verbatim: "\(lineLoss.material) · εr \(String(format: "%.2f", lineLoss.er)) · tan δ \(String(format: "%.4f", lineLoss.tanD)) · \(lineLoss.foilName)")
                .font(.callout).foregroundStyle(Theme.textSecondary)
            LineLossChart(report: lineLoss)
        }
    }

    private func metric(_ title: LocalizedStringKey, _ value: String, warn: Bool = false) -> some View {
        VStack(alignment: .leading, spacing: 1) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            Text(verbatim: value).font(.callout.monospacedDigit()).foregroundStyle(warn ? Theme.warning : Theme.probe)
        }
    }
}

// MARK: - Charts

private let channelColors: [Color] = [Theme.probe, Theme.warning, Theme.liveOn, Theme.lightBlue, Theme.error]

/// |S| in dB against frequency (GHz) for every curve, with the Nyquist frequency marked.
struct SParameterChart: View {
    let freq: [Double]
    let curves: [SIParameterCurve]
    let nyquist: Double?

    private struct Point: Identifiable {
        let id: Int
        let series: String
        let f: Double
        let db: Double
    }

    private var points: [Point] {
        var out: [Point] = []
        for curve in curves {
            for i in 0..<min(freq.count, curve.db.count) where curve.db[i].isFinite {
                out.append(Point(id: out.count, series: curve.name, f: freq[i] / 1e9, db: max(-80, curve.db[i])))
            }
        }
        return out
    }

    var body: some View {
        Chart {
            ForEach(points) { p in
                LineMark(x: .value("GHz", p.f), y: .value("dB", p.db), series: .value("Parameter", p.series))
                    .foregroundStyle(by: .value("Parameter", p.series))
                    .interpolationMethod(.linear)
            }
            if let nyquist, nyquist > 0 {
                RuleMark(x: .value("Nyquist", nyquist / 1e9))
                    .foregroundStyle(Theme.textMuted.opacity(0.6))
                    .lineStyle(StrokeStyle(lineWidth: 1, dash: [3, 3]))
            }
        }
        .chartForegroundStyleScale(domain: curves.map(\.name), range: Array(channelColors.prefix(max(1, curves.count))))
        .chartXAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let f = value.as(Double.self) { Text(verbatim: String(format: "%g GHz", f)).foregroundStyle(Theme.textMuted) }
                }
            }
        }
        .chartYAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let d = value.as(Double.self) { Text(verbatim: String(format: "%g dB", d)).foregroundStyle(Theme.textMuted) }
                }
            }
        }
        .frame(minHeight: 220)
    }
}

/// Receiver step response, lossy against lossless.
struct StepResponseChart: View {
    let step: SIStepResponse

    private struct Point: Identifiable {
        let id: Int
        let lossy: Bool
        let t: Double
        let v: Double
    }

    private var points: [Point] {
        var out: [Point] = []
        for i in 0..<min(step.time.count, step.lossy.count) {
            out.append(Point(id: out.count, lossy: true, t: step.time[i], v: step.lossy[i]))
        }
        if let lossless = step.lossless {
            for i in 0..<min(step.time.count, lossless.count) {
                out.append(Point(id: out.count, lossy: false, t: step.time[i], v: lossless[i]))
            }
        }
        return out
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Chart(points) { p in
                LineMark(x: .value("Time", p.t), y: .value("Voltage", p.v), series: .value("Line", p.lossy ? 1 : 0))
                    .foregroundStyle(p.lossy ? Theme.probe : Theme.textMuted)
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
            .frame(minHeight: 200)
            HStack(spacing: 14) {
                legendDot("Lossy", Theme.probe)
                if step.lossless != nil { legendDot("Lossless", Theme.textMuted) }
            }
        }
    }
}

/// Attenuation (dB per inch) of every stack-up layer against frequency.
struct LineLossChart: View {
    let report: SILineLossReport

    private struct Point: Identifiable {
        let id: Int
        let layer: String
        let f: Double
        let db: Double
    }

    private var points: [Point] {
        var out: [Point] = []
        for layer in report.layers {
            for i in 0..<min(report.freq.count, layer.dbPerInch.count) {
                out.append(Point(id: out.count, layer: "\(layer.name) (\(layer.line), \(String(format: "%.3f", layer.width)) mm)",
                                 f: report.freq[i] / 1e9, db: layer.dbPerInch[i]))
            }
        }
        return out
    }

    var body: some View {
        Chart(points) { p in
            LineMark(x: .value("GHz", p.f), y: .value("dB/in", p.db), series: .value("Layer", p.layer))
                .foregroundStyle(by: .value("Layer", p.layer))
        }
        .chartXAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let f = value.as(Double.self) { Text(verbatim: String(format: "%g GHz", f)).foregroundStyle(Theme.textMuted) }
                }
            }
        }
        .chartYAxis {
            AxisMarks { value in
                AxisGridLine().foregroundStyle(Theme.blue.opacity(0.18))
                AxisValueLabel {
                    if let d = value.as(Double.self) { Text(verbatim: String(format: "%.2g dB/in", d)).foregroundStyle(Theme.textMuted) }
                }
            }
        }
        .frame(minHeight: 200)
    }
}

/// Eye diagram: hit density over two unit intervals, the inner contour of the eye and the mask.
struct EyeDiagramView: View {
    let eye: SIEyeReport
    let maskHeight: Double
    let maskWidthUi: Double

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Canvas { context, size in
                let cols = max(1, eye.cols), rows = max(1, eye.rows)
                let cw = size.width / CGFloat(cols), rh = size.height / CGFloat(rows)
                context.fill(Path(CGRect(origin: .zero, size: size)), with: .color(Theme.navy))
                for r in 0..<rows {
                    for c in 0..<cols {
                        let h = eye.hits(row: r, col: c)
                        guard h > 0 else { continue }
                        let rect = CGRect(x: CGFloat(c) * cw, y: CGFloat(r) * rh, width: cw + 0.5, height: rh + 0.5)
                        context.fill(Path(rect), with: .color(Theme.probe.opacity(0.15 + 0.85 * h)))
                    }
                }
                let span = max(1e-12, eye.vMax - eye.vMin)
                func y(_ v: Double) -> CGFloat { CGFloat((eye.vMax - v) / span) * size.height }
                func x(_ col: Double) -> CGFloat { CGFloat((col + 0.5) / Double(cols)) * size.width }
                // Inner contour where the eye is open.
                var upper = Path(), lower = Path()
                var started = false
                for c in 0..<min(cols, min(eye.upper.count, eye.lower.count)) {
                    guard eye.upper[c] > eye.lower[c] else {
                        started = false
                        continue
                    }
                    let pu = CGPoint(x: x(Double(c)), y: y(eye.upper[c])), pl = CGPoint(x: x(Double(c)), y: y(eye.lower[c]))
                    if started {
                        upper.addLine(to: pu)
                        lower.addLine(to: pl)
                    } else {
                        upper.move(to: pu)
                        lower.move(to: pl)
                        started = true
                    }
                }
                context.stroke(upper, with: .color(Theme.liveOn), lineWidth: 1.5)
                context.stroke(lower, with: .color(Theme.liveOn), lineWidth: 1.5)
                // Mask: a hexagon centred on the eye.
                if maskHeight > 0, maskWidthUi > 0, eye.samplesPerUi > 0 {
                    let centre = Double(cols) / 2 - 0.5
                    let halfW = maskWidthUi * Double(eye.samplesPerUi) / 2
                    let mid = eye.vMid
                    var mask = Path()
                    mask.move(to: CGPoint(x: x(centre - halfW), y: y(mid)))
                    mask.addLine(to: CGPoint(x: x(centre - halfW / 2), y: y(mid + maskHeight / 2)))
                    mask.addLine(to: CGPoint(x: x(centre + halfW / 2), y: y(mid + maskHeight / 2)))
                    mask.addLine(to: CGPoint(x: x(centre + halfW), y: y(mid)))
                    mask.addLine(to: CGPoint(x: x(centre + halfW / 2), y: y(mid - maskHeight / 2)))
                    mask.addLine(to: CGPoint(x: x(centre - halfW / 2), y: y(mid - maskHeight / 2)))
                    mask.closeSubpath()
                    context.fill(mask, with: .color((eye.maskPass ? Theme.blue : Theme.error).opacity(0.25)))
                    context.stroke(mask, with: .color(eye.maskPass ? Theme.lightBlue : Theme.error), lineWidth: 1)
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 6))
            .accessibilityLabel(Text("Eye diagram"))
            HStack {
                Text(verbatim: "−1 UI").font(.caption2).foregroundStyle(Theme.textMuted)
                Spacer()
                Text(verbatim: EngineeringFormat.string(eye.vMin, unit: "V", digits: 2) + " … "
                     + EngineeringFormat.string(eye.vMax, unit: "V", digits: 2) + " · UI "
                     + EngineeringFormat.string(eye.ui, unit: "s", digits: 3))
                    .font(.caption2.monospacedDigit()).foregroundStyle(Theme.textMuted)
                Spacer()
                Text(verbatim: "+1 UI").font(.caption2).foregroundStyle(Theme.textMuted)
            }
        }
    }
}

func legendDot(_ title: LocalizedStringKey, _ color: Color) -> some View {
    HStack(spacing: 5) {
        Circle().fill(color).frame(width: 8, height: 8)
        Text(title).font(.caption).foregroundStyle(Theme.textSecondary)
    }
}
