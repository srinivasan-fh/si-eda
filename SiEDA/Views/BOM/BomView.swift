import SwiftUI

/// BOM workspace: the bill of materials grouped into order lines, with editable manufacturer, part numbers, prices and
/// do-not-populate, suggested standard part numbers, and cost per board and per order. Everything is saved with the
/// project and flows into the BOM / assembly CSVs and the fabrication package.
struct BomView: View {
    @EnvironmentObject private var store: DesignStore
    @State private var report = BomReport.empty
    @State private var filter = Filter.all
    @State private var showLivePricing = false

    enum Filter: String, CaseIterable, Identifiable {
        case all = "All", missing = "Needs Attention", dnp = "DNP"
        var id: String { rawValue }
    }

    private var shown: [BomLineInfo] {
        switch filter {
        case .all: return report.lines
        case .missing: return report.lines.filter { !$0.notes.isEmpty }
        case .dnp: return report.lines.filter(\.dnp)
        }
    }

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Image(systemName: "list.bullet.rectangle.portrait").foregroundStyle(Theme.blue)
                Text("Bill of Materials").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Picker("Show", selection: $filter) {
                    ForEach(Filter.allCases) { Text($0.rawValue).tag($0) }
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                .frame(width: 260)
                Divider().frame(height: 18)
                Button { store.applySuggestedPartNumbers() } label: {
                    Label("Fill Suggested Part Numbers", systemImage: "wand.and.stars")
                }
                .disabled(!report.lines.contains { $0.mpn.isEmpty && !$0.suggestedMpn.isEmpty })
                .help("Fills standard part numbers where they follow from the value and package (e.g. Yageo RC0805 "
                      + "resistors, semiconductor part numbers). Undo reverts it.")
                Button { showLivePricing = true } label: { Label("Live Pricing…", systemImage: "shippingbox") }
                    .disabled(report.lines.isEmpty)
                    .help("Stock, prices, lifecycle (EOL / NRND) and cost per build quantity from Octopart, DigiKey and Mouser")
                Spacer()
                Stepper(value: Binding(get: { report.buildQuantity }, set: { store.setBuildQuantity($0) }), in: 1...100_000) {
                    Text("Boards: \(report.buildQuantity)").monospacedDigit()
                }
                .fixedSize()
                .help("Boards per order, for the order cost")
                Menu {
                    Button("Bill of Materials (CSV)…") { store.export(.bom) }
                    Button("BOM for Assembly (JLCPCB / PCBWay)…") { store.export(.bomAssembly) }
                    Button("Component Placement List (CPL)…") { store.export(.cpl) }
                } label: {
                    Label("Export", systemImage: "square.and.arrow.up")
                }
                .fixedSize()
            }
            .buttonStyle(.borderless)

            if report.lines.isEmpty {
                BlueEmptyState(systemImage: "list.bullet.rectangle.portrait", title: "No parts yet",
                               message: "Parts placed in the schematic appear here grouped into BOM lines. Ground, net "
                                   + "labels and wire junctions are not parts and are never listed.",
                               actionTitle: "Open Schematic") { store.workspace = .schematic }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                ScrollView(.horizontal, showsIndicators: false) { summary }
                ScrollView([.horizontal, .vertical]) {
                    VStack(alignment: .leading, spacing: 0) {
                        BomHeaderRow()
                        ForEach(shown) { line in
                            BomRow(line: line)
                                .id("\(line.id)|\(line.manufacturer)|\(line.mpn)|\(line.supplierPart)|\(line.unitPrice)|\(line.dnp)")
                            Divider().overlay(Theme.blue.opacity(0.15))
                        }
                    }
                    .padding(12)
                }
            }
        }
        .background(Theme.navy)
        .task(id: store.revision) { report = store.bomReport }
        .sheet(isPresented: $showLivePricing) { BomLivePricingView(report: report).environmentObject(store) }
    }

    private var summary: some View {
        let s = report.summary
        let currency = Locale.current.currencySymbol ?? "$"
        return HStack(spacing: 10) {
            BomStat(title: "Lines", value: "\(s.lines)")
            BomStat(title: "Parts fitted", value: "\(s.placements)")
            BomStat(title: "Not fitted (DNP)", value: "\(s.dnp)")
            BomStat(title: "Without part number", value: "\(s.missingMpn)", warning: s.missingMpn > 0)
            BomStat(title: "Unpriced lines", value: "\(s.unpriced)", warning: s.unpriced > 0)
            BomStat(title: "Cost per board", value: currency + String(format: "%.2f", s.costPerBoard))
            BomStat(title: "Order (\(report.buildQuantity) boards)", value: currency + String(format: "%.2f", report.orderCost))
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 12)
        .padding(.top, 10)
    }
}

private struct BomStat: View {
    var title: String
    var value: String
    var warning = false

    var body: some View {
        VStack(alignment: .leading, spacing: 2) {
            Text(value).font(.title3.weight(.semibold).monospacedDigit())
                .foregroundStyle(warning ? Theme.warning : Theme.textPrimary)
            Text(title).font(.caption2).foregroundStyle(Theme.textMuted)
        }
        .padding(.horizontal, 10)
        .padding(.vertical, 6)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.deepBlue))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Theme.blue.opacity(0.3)))
        .accessibilityElement(children: .combine)
    }
}

/// Column widths shared by the header and the rows.
private enum BomColumn {
    static let item: CGFloat = 30, qty: CGFloat = 34, refs: CGFloat = 130, part: CGFloat = 250, footprint: CGFloat = 110
    static let maker: CGFloat = 110, mpn: CGFloat = 170, supplier: CGFloat = 96, price: CGFloat = 70, total: CGFloat = 70
    static let dnp: CGFloat = 40, status: CGFloat = 26
}

private struct BomHeaderRow: View {
    var body: some View {
        HStack(spacing: 8) {
            cell("#", BomColumn.item)
            cell("Qty", BomColumn.qty)
            cell("Designators", BomColumn.refs)
            cell("Part", BomColumn.part)
            cell("Footprint", BomColumn.footprint)
            cell("Manufacturer", BomColumn.maker)
            cell("Manufacturer part #", BomColumn.mpn)
            cell("Supplier #", BomColumn.supplier)
            cell("Unit price", BomColumn.price)
            cell("Line total", BomColumn.total)
            cell("DNP", BomColumn.dnp)
            cell("", BomColumn.status)
        }
        .font(.caption.weight(.bold))
        .foregroundStyle(Theme.skyBlue)
        .padding(.vertical, 6)
    }

    private func cell(_ text: String, _ width: CGFloat) -> some View {
        Text(text).frame(width: width, alignment: .leading)
    }
}

private struct BomRow: View {
    @EnvironmentObject private var store: DesignStore
    let line: BomLineInfo
    @State private var manufacturer: String
    @State private var mpn: String
    @State private var supplier: String
    @State private var price: String
    @FocusState private var focused: Bool

    init(line: BomLineInfo) {
        self.line = line
        _manufacturer = State(initialValue: line.manufacturer)
        _mpn = State(initialValue: line.mpn)
        _supplier = State(initialValue: line.supplierPart)
        _price = State(initialValue: line.unitPrice > 0 ? String(format: "%g", line.unitPrice) : "")
    }

    var body: some View {
        HStack(spacing: 8) {
            Text("\(line.item)").frame(width: BomColumn.item, alignment: .leading).foregroundStyle(Theme.textMuted)
            Text("\(line.quantity)").monospacedDigit().frame(width: BomColumn.qty, alignment: .leading)
            Button { show() } label: {
                Text(line.designators).lineLimit(2).multilineTextAlignment(.leading)
                    .frame(width: BomColumn.refs, alignment: .leading)
            }
            .buttonStyle(.plain)
            .foregroundStyle(Theme.skyBlue)
            .help("Show these parts in the schematic")
            VStack(alignment: .leading, spacing: 1) {
                Text(line.description).lineLimit(2).foregroundStyle(line.dnp ? Theme.textMuted : Theme.textPrimary)
                if !line.rating.isEmpty {
                    Text("Rating: \(line.rating)").font(.caption2).foregroundStyle(Theme.textMuted)
                }
            }
            .frame(width: BomColumn.part, alignment: .leading)
            Text(line.footprint).lineLimit(1).foregroundStyle(Theme.textSecondary).frame(width: BomColumn.footprint, alignment: .leading)
            field("Manufacturer", text: $manufacturer, prompt: line.suggestedManufacturer, width: BomColumn.maker)
            field("Part number", text: $mpn, prompt: line.suggestedMpn, width: BomColumn.mpn)
            field("e.g. C17414", text: $supplier, prompt: "", width: BomColumn.supplier)
            field("0.00", text: $price, prompt: "", width: BomColumn.price)
            Text(line.lineCost > 0 ? String(format: "%.2f", line.lineCost) : "—")
                .monospacedDigit().foregroundStyle(Theme.textSecondary)
                .frame(width: BomColumn.total, alignment: .leading)
            Toggle("DNP", isOn: Binding(get: { line.dnp }, set: { store.updateBomLine(line, SourcingUpdate(dnp: $0)) }))
                .toggleStyle(.checkbox)
                .labelsHidden()
                .help("Do not populate: on the board but not fitted; left out of the assembly BOM and CPL")
                .frame(width: BomColumn.dnp, alignment: .leading)
            Group {
                if line.notes.isEmpty {
                    Image(systemName: "checkmark.circle.fill").foregroundStyle(Theme.success)
                } else {
                    Image(systemName: "exclamationmark.circle").foregroundStyle(Theme.warning)
                        .help(line.notes.joined(separator: "\n"))
                }
            }
            .frame(width: BomColumn.status)
        }
        .font(.callout)
        .padding(.vertical, 6)
        .opacity(line.dnp ? 0.7 : 1)
        .onChange(of: focused) { _, isFocused in if !isFocused { commit() } }
    }

    private func field(_ placeholder: String, text: Binding<String>, prompt: String, width: CGFloat) -> some View {
        TextField(placeholder, text: text, prompt: Text(prompt.isEmpty ? placeholder : prompt))
            .textFieldStyle(.blue)
            .focused($focused)
            .onSubmit { commit() }
            .frame(width: width)
    }

    /// Writes the edited fields (only those that changed) to every part on the line.
    private func commit() {
        var update = SourcingUpdate()
        let trim = { (s: String) in s.trimmingCharacters(in: .whitespacesAndNewlines) }
        if trim(manufacturer) != line.manufacturer { update.manufacturer = trim(manufacturer) }
        if trim(mpn) != line.mpn { update.mpn = trim(mpn) }
        if trim(supplier) != line.supplierPart { update.supplierPart = trim(supplier) }
        let priceText = trim(price).replacingOccurrences(of: ",", with: ".")
            .trimmingCharacters(in: CharacterSet(charactersIn: "$€£¥₹ "))
        let newPrice = Double(priceText) ?? 0
        if abs(newPrice - line.unitPrice) > 1e-12 { update.unitPrice = max(0, newPrice) }
        store.updateBomLine(line, update)
    }

    private func show() {
        store.workspace = .schematic
        store.selection = Set(line.componentIds)
        store.selectedWire = nil
        store.requestView(.fitSelection)
    }
}
