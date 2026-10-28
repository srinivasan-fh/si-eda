import SwiftUI

/// Design → Thermal Analysis…: the board's steady-state temperature map from each part's dissipation (MCP
/// `pcb_thermal`, core `thermalAnalysis`): ambient, airflow and powers by designator in, a heat map with every powered
/// part's board / junction temperature and the THERMAL_* findings out.
struct ThermalView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var ambient = 25.0
    @State private var airflow = 10.0
    @State private var powers: [PowerRow] = []
    @State private var result: ThermalResult?
    @State private var problem = ""

    struct PowerRow: Identifiable {
        var id = UUID()
        var ref: String
        var watts: Double
    }

    struct ThermalResult: Decodable {
        struct Part: Decodable, Identifiable {
            var id: String { ref }
            let ref: String
            let powerW: Double
            let powerSource: String
            let boardC: Double
            let junctionC: Double
            let x: Double
            let y: Double
        }
        struct Finding: Decodable, Identifiable {
            var id: String { code + ref }
            let code, severity, message, ref: String
        }
        struct Grid: Decodable {
            let x0, y0, cell: Double
            let nx, ny: Int
            let t: [Double?]
        }
        let ambientC, maxC, totalPowerW: Double
        let parts: [Part]
        let findings: [Finding]
        let notes: [String]
        let map: Grid?
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Thermal Analysis").font(.headline).foregroundStyle(Theme.skyBlue)
            HStack(spacing: 16) {
                LabeledContent("Ambient (°C)") {
                    TextField("", value: $ambient, format: .number.precision(.fractionLength(0...1))).frame(width: 60)
                }
                Picker("Airflow", selection: $airflow) {
                    Text("Still air").tag(10.0)
                    Text("Light airflow").tag(25.0)
                    Text("Fan").tag(50.0)
                }
                .frame(width: 220)
                Spacer()
                Button("Run") { run() }.keyboardShortcut(.defaultAction)
            }
            .textFieldStyle(.roundedBorder)
            HStack(alignment: .top, spacing: 14) {
                heatMap.frame(minWidth: 420, minHeight: 300)
                VStack(alignment: .leading, spacing: 8) {
                    powerTable
                    if let r = result { partsTable(r) }
                }
                .frame(width: 330)
            }
            if let r = result {
                ForEach(r.findings) { f in
                    Label(f.message, systemImage: f.severity == "error" ? "xmark.octagon.fill" : "exclamationmark.triangle.fill")
                        .foregroundStyle(f.severity == "error" ? Theme.error : Theme.warning).font(.callout)
                }
                ForEach(r.notes, id: \.self) { Text(verbatim: $0).font(.caption).foregroundStyle(Theme.textSecondary) }
            }
            if !problem.isEmpty { Text(verbatim: problem).foregroundStyle(Theme.error).font(.callout) }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(16)
        .frame(minWidth: 820, minHeight: 520)
        .onAppear(perform: run)
    }

    private var powerTable: some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("Power by part").font(.subheadline.bold())
                Spacer()
                Button("Add") { powers.append(PowerRow(ref: "", watts: 0.5)) }
            }
            Text("Parts not listed take their power from the DC operating point.")
                .font(.caption).foregroundStyle(Theme.textSecondary)
            ForEach($powers) { $row in
                HStack {
                    TextField("Designator", text: $row.ref).frame(width: 110)
                    TextField("", value: $row.watts, format: .number.precision(.fractionLength(0...3))).frame(width: 70).monospacedDigit()
                    Text(verbatim: "W").foregroundStyle(Theme.textSecondary)
                    Button {
                        let id = row.id
                        powers.removeAll { $0.id == id }
                    } label: { Image(systemName: "trash") }
                        .buttonStyle(.borderless).help("Remove").accessibilityLabel(Text("Remove"))
                }
            }
            .textFieldStyle(.roundedBorder)
        }
    }

    private func partsTable(_ r: ThermalResult) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack(spacing: 4) {
                Text("Hottest")
                Text(verbatim: String(format: "%.0f °C · %.2f W", r.maxC, r.totalPowerW))
            }
            .font(.subheadline.bold())
            Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 3) {
                GridRow {
                    Text("Part"); Text(verbatim: "W"); Text("Board (°C)"); Text("Junction (°C)")
                }
                .font(.caption).foregroundStyle(Theme.textSecondary)
                ForEach(r.parts.prefix(12)) { p in
                    GridRow {
                        Text(verbatim: p.ref)
                        Text(verbatim: String(format: "%.3f", p.powerW))
                        Text(verbatim: String(format: "%.0f", p.boardC))
                        Text(verbatim: String(format: "%.0f", p.junctionC))
                            .foregroundStyle(p.junctionC > 125 ? Theme.error : Theme.textPrimary)
                    }
                    .monospacedDigit()
                }
            }
        }
    }

    /// Cells coloured from ambient (blue) to the hottest (red); powered parts marked with their junction temperature.
    private var heatMap: some View {
        Canvas { ctx, size in
            guard let r = result, let g = r.map, g.nx > 0, g.ny > 0 else { return }
            let scale = min(size.width / (Double(g.nx) * g.cell), size.height / (Double(g.ny) * g.cell))
            let span = max(1e-6, r.maxC - r.ambientC)
            for j in 0..<g.ny {
                for i in 0..<g.nx {
                    guard let t = g.t[j * g.nx + i] else { continue }
                    let u = min(1, max(0, (t - r.ambientC) / span))
                    let rect = CGRect(x: Double(i) * g.cell * scale, y: Double(j) * g.cell * scale,
                                      width: g.cell * scale + 0.5, height: g.cell * scale + 0.5)
                    ctx.fill(Path(rect), with: .color(Color(hue: 0.66 * (1 - u), saturation: 0.85, brightness: 0.9)))
                }
            }
            for p in r.parts {
                let at = CGPoint(x: (p.x - g.x0) * scale, y: (p.y - g.y0) * scale)
                ctx.draw(Text(verbatim: "\(p.ref) \(Int(p.junctionC.rounded()))°").font(.caption2.bold()).foregroundColor(.white), at: at)
            }
        }
        .background(Theme.navy)
        .accessibilityLabel(Text("Board temperature map"))
    }

    private func run() {
        var power: [String: Double] = [:]
        for row in powers where !row.ref.trimmingCharacters(in: .whitespaces).isEmpty {
            power[row.ref.trimmingCharacters(in: .whitespaces).uppercased()] = row.watts
        }
        let (text, failed) = store.runTool("pcb_thermal", arguments: ["ambient": ambient, "hTop": airflow, "hBottom": airflow,
                                                                      "power": power, "map": true, "image": false])
        guard !failed, let decoded = try? JSONDecoder().decode(ThermalResult.self, from: Data(text.utf8)) else {
            problem = text
            return
        }
        problem = ""
        result = decoded
    }
}
