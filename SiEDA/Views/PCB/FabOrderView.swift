// SiEDA — File → Order Boards…: what a board house's order form asks, read from the board by the core (fabQuote),
// checked against the fab's published limits, an estimate only from the user's own price table, then the
// fabrication package and the fab's order page.
import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct FabOrderView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @AppStorage("fabOrder.fab") private var fab = "jlcpcb"
    @AppStorage("fabOrder.quantity") private var quantity = 5
    @State private var quote: FabQuote?
    @State private var priceTables: [String: String] = (UserDefaults.standard.dictionary(forKey: "fabOrder.priceTables") as? [String: String]) ?? [:]

    static let fabs = [("jlcpcb", "JLCPCB"), ("pcbway", "PCBWay"), ("oshpark", "OSH Park"), ("eurocircuits", "Eurocircuits"),
                       ("lioncircuits", "Lion Circuits (India)"), ("pcbpower", "PCB Power (India)")]
    /// Order-form rows: core key, label.
    static let rows = [("layers", "Layers"), ("widthMm", "Width (mm)"), ("heightMm", "Height (mm)"), ("thicknessMm", "Thickness (mm)"),
                       ("material", "Material"), ("copperOz", "Copper (oz)"), ("finish", "Surface finish"),
                       ("solderMask", "Solder mask"), ("silkscreen", "Silkscreen"), ("minTrackMm", "Min track (mm)"),
                       ("minSpaceMm", "Min spacing (mm)"), ("minDrillMm", "Min drill (mm)"), ("vias", "Vias"),
                       ("platedHoles", "Plated holes"), ("drillSizes", "Drill sizes"), ("impedanceControl", "Impedance control"),
                       ("hdi", "Blind / buried vias"), ("viaInPad", "Via-in-pad"), ("panel", "Panel")]

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Order Boards").font(.headline).foregroundStyle(Theme.skyBlue)
                Spacer()
                Picker("Fab", selection: $fab) { ForEach(Self.fabs, id: \.0) { Text(verbatim: $0.1).tag($0.0) } }
                    .frame(width: 240)
                Stepper(value: $quantity, in: 1...10000) { (Text("Quantity") + Text(verbatim: ": \(quantity)")).monospacedDigit() }
            }
            if let quote {
                ScrollView {
                    Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 4) {
                        ForEach(Self.rows, id: \.0) { key, label in
                            if let value = quote.order[key] {
                                GridRow {
                                    Text(LocalizedStringKey(label)).foregroundStyle(Theme.textSecondary)
                                    Text(verbatim: value.text).foregroundStyle(Theme.textPrimary).textSelection(.enabled)
                                }
                            }
                        }
                    }
                    .font(.callout.monospacedDigit())
                    if quote.checks.isEmpty {
                        Label("No published limits for this fab: check the board against its capability page.", systemImage: "info.circle")
                            .font(.caption).foregroundStyle(Theme.textMuted).padding(.top, 6)
                    }
                    ForEach(quote.checks.filter { !$0.ok }, id: \.rule) { check in
                        Label { Text(verbatim: "\(check.rule): \(check.actual) (limit \(check.limit))") } icon: {
                            Image(systemName: "exclamationmark.triangle.fill")
                        }
                        .font(.caption).foregroundStyle(Theme.warning)
                    }
                    if !quote.checks.isEmpty && quote.checks.allSatisfy(\.ok) {
                        Label("Within the fab's published limits", systemImage: "checkmark.seal").font(.caption).foregroundStyle(Theme.success)
                    }
                    if let estimate = quote.estimate {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(estimate.lines, id: \.item) { line in
                                Text(verbatim: "\(line.item): \(SupplierSearchView.money(line.amount, estimate.currency))")
                            }
                            Text(verbatim: "\(SupplierSearchView.money(estimate.total, estimate.currency)) · "
                                 + "\(SupplierSearchView.money(estimate.perBoard, estimate.currency)) / board")
                                .font(.headline.monospacedDigit())
                            Text("Estimate from your price table, not the fab's quote.").font(.caption2).foregroundStyle(Theme.textMuted)
                        }
                        .font(.caption.monospacedDigit()).padding(.top, 8)
                    }
                }
            }
            Divider()
            HStack {
                Button("Price Table…", action: importPriceTable)
                    .help("Your fab's prices (sieda.fabprice/1 JSON) for an estimate; SiEDA has no built-in prices")
                if priceTables[fab] != nil { Button("Remove Price Table") { setPriceTable(nil) } }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Export Package & Open Order Page") {
                    let url = quote.flatMap { URL(string: $0.orderUrl) }
                    Task {
                        await store.exportFabricationPackage()
                        if let url { NSWorkspace.shared.open(url) }
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(quote == nil)
            }
        }
        .padding(16)
        .frame(minWidth: 620, minHeight: 560)
        .background(Theme.navy)
        .onAppear(perform: refresh)
        .onChange(of: fab) { refresh() }
        .onChange(of: quantity) { refresh() }
    }

    private func refresh() {
        var request = "{\"fab\":\"\(fab)\",\"quantity\":\(quantity)"
        if let table = priceTables[fab] { request += ",\"priceTable\":\(table)" }
        quote = store.engine.fabQuote(request + "}")
    }

    private func importPriceTable() {
        let panel = NSOpenPanel()
        panel.allowedContentTypes = [.json]
        guard panel.runModal() == .OK, let url = panel.url, let data = try? Data(contentsOf: url),
              (try? JSONSerialization.jsonObject(with: data)) is [String: Any] else { return }
        setPriceTable(String(decoding: data, as: UTF8.self))
    }

    private func setPriceTable(_ json: String?) {
        priceTables[fab] = json
        UserDefaults.standard.set(priceTables, forKey: "fabOrder.priceTables")
        refresh()
    }
}

/// The core's fab quote (Fabrication.hpp fabQuote).
struct FabQuote: Decodable {
    enum Value: Decodable {
        case number(Double), flag(Bool), text(String)
        init(from decoder: Decoder) throws {
            let c = try decoder.singleValueContainer()
            if let b = try? c.decode(Bool.self) { self = .flag(b) }
            else if let d = try? c.decode(Double.self) { self = .number(d) }
            else { self = .text(try c.decode(String.self)) }
        }
        var text: String {
            switch self {
            case .number(let d): return d == d.rounded() ? String(Int(d)) : String(format: "%.3g", d)
            case .flag(let b): return b ? "yes" : "no"
            case .text(let s): return s
            }
        }
    }
    struct Check: Decodable { var rule, actual, limit: String; var ok: Bool }
    struct Estimate: Decodable {
        struct Line: Decodable { var item: String; var amount: Double }
        var currency: String
        var lines: [Line]
        var total, perBoard: Double
    }
    var orderUrl: String
    var order: [String: Value]
    var checks: [Check]
    var estimate: Estimate?
}

extension EDAEngine {
    func fabQuote(_ requestJSON: String) -> FabQuote? {
        withHandle { Self.take(sieda_fab_quote($0, requestJSON)) }
            .flatMap { try? JSONDecoder().decode(FabQuote.self, from: Data($0.utf8)) }
    }
}
