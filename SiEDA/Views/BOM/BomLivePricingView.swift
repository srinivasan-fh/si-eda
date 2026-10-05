import SwiftUI

/// BOM → Live Pricing: looks up every line's manufacturer part number at the configured distributors and shows stock,
/// the best offer and price at the build quantity, lifecycle warnings (NRND, EOL, obsolete) and the BOM cost at
/// 1, 10, 100 and 1000 boards. "Apply Prices" writes the supplier part numbers and unit prices into the BOM (one undo
/// step).
struct BomLivePricingView: View {
    let report: BomReport
    @EnvironmentObject private var store: DesignStore
    @ObservedObject private var settings = SupplierSettings.shared
    @Environment(\.dismiss) private var dismiss
    @State private var rollup: SupplierRollup?
    @State private var progress: (done: Int, total: Int) = (0, 0)
    @State private var problems: [String] = []
    @State private var task: Task<Void, Never>?
    @State private var applied: Int?

    static let boardCounts = [1, 10, 100, 1000]

    private var buildQuantity: Int { report.buildQuantity }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("Live Pricing").font(.headline).foregroundStyle(Theme.skyBlue)
                Spacer()
                if task != nil {
                    ProgressView(value: Double(progress.done), total: Double(max(progress.total, 1)))
                        .frame(width: 160)
                    Text(verbatim: "\(progress.done) / \(progress.total)").font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
                }
                Button("Refresh") { run() }.disabled(task != nil || !settings.hasAnySource)
            }
            .padding(12)
            Divider()
            if !settings.hasAnySource {
                VStack(spacing: 10) {
                    Image(systemName: "key.slash").font(.system(size: 40, weight: .light)).foregroundStyle(Theme.textMuted)
                    Text("No distributor API keys yet").font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                    Text("Add an Octopart (Nexar), DigiKey or Mouser key in Settings → Suppliers to see stock, prices and lifecycle for every BOM line.")
                        .font(.callout).foregroundStyle(Theme.textSecondary).multilineTextAlignment(.center).frame(maxWidth: 460)
                    SettingsLink { Text("Open Settings…") }
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if let rollup {
                content(rollup)
            } else {
                VStack(spacing: 8) {
                    ProgressView()
                    Text("Asking the distributors…").font(.callout).foregroundStyle(Theme.textMuted)
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
            Divider()
            HStack {
                if let applied { Text(verbatim: "\(applied) lines updated").font(.caption).foregroundStyle(Theme.textMuted) }
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Apply Prices to BOM") { apply() }
                    .buttonStyle(.borderedProminent)
                    .disabled(rollup == nil || task != nil)
                    .help("Fill the supplier part number and unit price of the best offer at the build quantity into each line")
            }
            .padding(12)
        }
        .frame(minWidth: 900, minHeight: 560)
        .background(Theme.navy)
        .onAppear { if settings.hasAnySource && rollup == nil { run() } }
        .onDisappear { task?.cancel() }
    }

    private func content(_ rollup: SupplierRollup) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                ForEach(rollup.totals) { total in
                    VStack(alignment: .leading, spacing: 2) {
                        Text(verbatim: SupplierSearchView.money(total.perBoard, rollup.currency))
                            .font(.title3.weight(.semibold).monospacedDigit())
                            .foregroundStyle(total.complete ? Theme.textPrimary : Theme.warning)
                        Text(verbatim: "per board at \(total.boards) · total \(SupplierSearchView.money(total.cost, rollup.currency))")
                            .font(.caption2).foregroundStyle(Theme.textMuted)
                        if total.unpriced > 0 || total.shortages > 0 {
                            Text(verbatim: "\(total.unpriced) unpriced · \(total.shortages) short")
                                .font(.caption2).foregroundStyle(Theme.warning)
                        }
                    }
                    .padding(8)
                    .background(RoundedRectangle(cornerRadius: 8).fill(total.boards == buildQuantity ? Theme.darkBlue : Theme.deepBlue))
                }
            }
            if rollup.mixedCurrency {
                Label("Some lines are priced in another currency and are added as they are.", systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(Theme.warning)
            }
            ForEach(Array((rollup.warnings + problems).prefix(12).enumerated()), id: \.offset) { _, warning in
                Label { Text(verbatim: warning) } icon: { Image(systemName: "exclamationmark.triangle.fill") }
                    .font(.caption).foregroundStyle(Theme.warning)
            }
            ScrollView {
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 6) {
                    GridRow {
                        Text("Designators")
                        Text("Manufacturer part #")
                        Text("Lifecycle")
                        Text("Stock")
                        Text("Supplier")
                        Text("Unit price")
                        Text("Line total")
                    }
                    .font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                    ForEach(rollup.lines) { line in
                        let offer = line.offer(boards: buildQuantity)
                        GridRow {
                            Text(verbatim: line.refs.prefix(4).joined(separator: ", ") + (line.refs.count > 4 ? "…" : ""))
                                .foregroundStyle(Theme.textSecondary)
                            Text(verbatim: line.mpn.isEmpty ? "—" : line.mpn).foregroundStyle(Theme.textPrimary)
                            LifecycleBadge(lifecycle: line.lifecycle, text: line.lifecycleText)
                            Text(verbatim: line.stock >= 0 ? line.stock.formatted() : "—")
                                .foregroundStyle(offer.map { $0.inStock } ?? true ? Theme.textSecondary : Theme.warning)
                            Text(verbatim: offer.map { "\($0.supplier) \($0.sku)" } ?? Self.statusText(line.status))
                                .foregroundStyle(offer == nil ? Theme.textMuted : Theme.textSecondary)
                            Text(verbatim: offer.map { SupplierSearchView.money($0.unitPrice, $0.currency) } ?? "—")
                            Text(verbatim: offer.map { SupplierSearchView.money($0.extended, $0.currency) + " (\($0.orderQuantity))" } ?? "—")
                        }
                        .font(.caption.monospacedDigit())
                        .help(line.warning)
                    }
                }
                .padding(.vertical, 4)
            }
        }
        .padding(12)
    }

    /// Why a line has no offer (kept in English like other BOM notes).
    static func statusText(_ status: String) -> String {
        switch status {
        case "no_mpn": return "no part number"
        case "not_found": return "not found"
        case "dnp": return "not fitted"
        case "unpriced": return "no price"
        default: return "—"
        }
    }

    // MARK: - Work

    private func run() {
        task?.cancel()
        applied = nil
        let engine = settings.engine()
        let currency = settings.currency
        let lines = report.lines
        let build = buildQuantity
        // Each part number once, at most 200 (one search per part number and distributor, 4 at a time).
        var seen = Set<String>()
        let mpns = lines.filter { !$0.dnp && $0.embedded != true }.map(\.mpn)
            .filter { !$0.isEmpty && seen.insert(SupplierPlacement.normalized($0)).inserted }
        let queries = Array(mpns.prefix(200))
        progress = (0, queries.count)
        problems = mpns.count > queries.count ? ["Only the first 200 part numbers were looked up."] : []
        task = Task { @MainActor in
            var parts: [SupplierPartInfo] = []
            var failures = Set<String>()
            await withTaskGroup(of: SupplierSearchOutcome.self) { group in
                var pending = queries.makeIterator()
                for _ in 0..<min(4, queries.count) {
                    if let q = pending.next() { group.addTask { await engine.search(q, limit: 5) } }
                }
                for await outcome in group {
                    parts += outcome.parts
                    for (source, status) in outcome.statuses {
                        if case .failed(let message) = status { failures.insert("\(source.displayName): \(message)") }
                    }
                    progress.done += 1
                    if let q = pending.next() { group.addTask { await engine.search(q, limit: 5) } }
                }
            }
            guard !Task.isCancelled else { return }
            let request = SupplierRollupRequest(
                currency: currency, quantities: Self.boardCounts, buildQuantity: build,
                lines: lines.map { .init(item: $0.item, refs: $0.refs, quantity: $0.quantity, mpn: $0.mpn,
                                         manufacturer: $0.manufacturer, dnp: $0.dnp, embedded: $0.embedded ?? false) },
                parts: parts)
            rollup = EDAEngine.supplierBomRollup(request)
            problems += failures.sorted().prefix(4)
            task = nil
        }
    }

    private func apply() {
        guard let rollup else { return }
        var updates: [(line: BomLineInfo, update: SourcingUpdate)] = []
        for line in report.lines {
            guard let priced = rollup.lines.first(where: { $0.item == line.item }), priced.status == "priced",
                  let offer = priced.offer(boards: buildQuantity), offer.currency == rollup.currency || offer.currency.isEmpty
            else { continue }
            var update = SourcingUpdate()
            if !offer.sku.isEmpty && offer.sku != line.supplierPart { update.supplierPart = offer.sku }
            if abs(offer.unitPrice - line.unitPrice) > 1e-9 { update.unitPrice = offer.unitPrice }
            updates.append((line, update))
        }
        applied = store.applySupplierPricing(updates)
    }
}
