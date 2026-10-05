import SwiftUI

/// Find Parts Online (Component Library): searches Octopart (Nexar), DigiKey and Mouser with the user's own keys and
/// shows each manufacturer part with its lifecycle, stock, price breaks and datasheet. "Place Part" and "Add to
/// Library" hand the chosen part to `onChoose` (place: true / false). Without keys the sheet says how to add one.
struct SupplierSearchView: View {
    let onChoose: (SupplierPartInfo, Bool) -> Void
    @ObservedObject private var settings = SupplierSettings.shared
    @StateObject private var model = SupplierSearchModel()
    @State private var query: String
    @State private var selectedId: String?
    @Environment(\.dismiss) private var dismiss
    @Environment(\.openURL) private var openURL

    init(initialQuery: String = "", onChoose: @escaping (SupplierPartInfo, Bool) -> Void) {
        self.onChoose = onChoose
        _query = State(initialValue: initialQuery)
    }

    private var selected: SupplierPartInfo? { model.parts.first { $0.id == selectedId } ?? model.parts.first }

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack(spacing: 8) {
                Text("Find Parts Online").font(.headline).foregroundStyle(Theme.skyBlue)
                Spacer()
                ForEach(SupplierSource.allCases) { source in sourceChip(source) }
            }
            .padding(12)
            HStack(spacing: 8) {
                TextField("Manufacturer part number or keywords", text: $query)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit { runSearch() }
                    .disabled(!settings.hasAnySource)
                Button("Search") { runSearch() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(!settings.hasAnySource || query.trimmingCharacters(in: .whitespaces).isEmpty)
                if model.isSearching {
                    ProgressView().controlSize(.small)
                    Button("Stop") { model.cancel() }
                }
            }
            .padding(.horizontal, 12)
            .padding(.bottom, 10)
            Divider()
            if !settings.hasAnySource {
                noKeys
            } else if model.parts.isEmpty {
                VStack(spacing: 8) {
                    Image(systemName: "magnifyingglass").font(.largeTitle).foregroundStyle(Theme.textMuted)
                    Group {
                        if model.lastQuery.isEmpty {
                            Text("Search by part number (LM358DR) or keywords (CAN transceiver SOIC-8).")
                        } else if model.isSearching {
                            Text("Searching…")
                        } else {
                            Text("No parts found.")
                        }
                    }
                    .font(.callout).foregroundStyle(Theme.textMuted)
                    statusLines
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else {
                HStack(spacing: 0) {
                    List(selection: $selectedId) {
                        ForEach(model.parts) { part in row(part).tag(part.id) }
                    }
                    .listStyle(.inset)
                    .scrollContentBackground(.hidden)
                    .frame(width: 330)
                    Divider()
                    ScrollView {
                        if let part = selected { detail(part) }
                    }
                    .frame(maxWidth: .infinity)
                }
            }
            Divider()
            HStack {
                statusLines
                Spacer()
                Button("Close") { dismiss() }.keyboardShortcut(.cancelAction)
            }
            .padding(12)
        }
        .frame(minWidth: 860, minHeight: 540)
        .background(Theme.navy)
        .onAppear { if !query.isEmpty && settings.hasAnySource { runSearch() } }
        .onDisappear { model.cancel() }
    }

    private func runSearch() {
        selectedId = nil
        model.search(query, engine: settings.engine())
    }

    // MARK: - Pieces

    private var noKeys: some View {
        VStack(spacing: 10) {
            Image(systemName: "key.slash").font(.system(size: 40, weight: .light)).foregroundStyle(Theme.textMuted)
            Text("No distributor API keys yet").font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary)
            Text("Live stock, prices and lifecycle come from Octopart (Nexar), DigiKey or Mouser with your own free API keys. Add at least one in Settings → Suppliers. Keys stay in the macOS Keychain.")
                .font(.callout).foregroundStyle(Theme.textSecondary).multilineTextAlignment(.center).frame(maxWidth: 480)
            SettingsLink { Text("Open Settings…") }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }

    private func sourceChip(_ source: SupplierSource) -> some View {
        let configured = settings.configuredSources.contains(source)
        return Text(verbatim: source.displayName)
            .font(.caption2.weight(.semibold))
            .padding(.horizontal, 6).padding(.vertical, 2)
            .background(Capsule().fill(configured ? Theme.blue.opacity(0.35) : Theme.deepBlue))
            .foregroundStyle(configured ? Theme.textPrimary : Theme.textMuted)
            .help(configured ? Text("API key configured") : Text("No API key: add one in Settings → Suppliers"))
    }

    @ViewBuilder
    private var statusLines: some View {
        VStack(alignment: .leading, spacing: 2) {
            ForEach(SupplierSource.allCases) { source in
                if let status = model.statuses[source], let line = Self.statusText(source, status) {
                    Text(verbatim: line.text).font(.caption2).foregroundStyle(line.warning ? Theme.warning : Theme.textMuted)
                        .lineLimit(2)
                }
            }
        }
    }

    /// One line per source: how many parts, from the cache or live, or why it failed.
    static func statusText(_ source: SupplierSource, _ status: SupplierSourceStatus) -> (text: String, warning: Bool)? {
        let date = { (d: Date) in d.formatted(date: .abbreviated, time: .shortened) }
        switch status {
        case .notConfigured: return nil
        case .live(let n): return ("\(source.displayName): \(n) parts", false)
        case .cached(let n, let at): return ("\(source.displayName): \(n) parts (cached \(date(at)))", false)
        case .offline(let n, let at, let reason):
            return ("\(source.displayName): offline, showing \(n) parts saved \(date(at)) — \(reason)", true)
        case .failed(let message): return (message, true)
        }
    }

    private func row(_ part: SupplierPartInfo) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack(spacing: 6) {
                Text(verbatim: part.mpn).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                LifecycleBadge(lifecycle: part.lifecycle, text: part.lifecycleText)
            }
            Text(verbatim: part.manufacturer).font(.caption).foregroundStyle(Theme.textSecondary)
            Text(verbatim: part.description).font(.caption2).foregroundStyle(Theme.textMuted).lineLimit(2)
            HStack(spacing: 10) {
                Text(verbatim: part.stock >= 0 ? "\(part.stock.formatted()) in stock" : "stock not reported")
                if let p = part.price(at: 1) { Text(verbatim: "1+ " + Self.money(p.unitPrice, p.currency)) }
                if let p = part.price(at: 100) { Text(verbatim: "100+ " + Self.money(p.unitPrice, p.currency)) }
            }
            .font(.caption2.monospacedDigit())
            .foregroundStyle(part.stock > 0 ? Theme.textSecondary : Theme.warning)
        }
        .padding(.vertical, 2)
    }

    private func detail(_ part: SupplierPartInfo) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(verbatim: part.mpn).font(.title3.weight(.semibold)).foregroundStyle(Theme.textPrimary).textSelection(.enabled)
                LifecycleBadge(lifecycle: part.lifecycle, text: part.lifecycleText)
                Spacer()
                Button { onChoose(part, false); dismiss() } label: { Label("Add to Library", systemImage: "books.vertical") }
                    .help("Add the part to the project library (linked to a catalog symbol and footprint when one matches)")
                Button { onChoose(part, true); dismiss() } label: { Label("Place Part", systemImage: "square.and.arrow.down.on.square") }
                    .buttonStyle(.borderedProminent)
                    .help("Place the part in the schematic with its manufacturer, part number, supplier number and price")
            }
            Text(verbatim: [part.manufacturer, part.category, part.package].filter { !$0.isEmpty }.joined(separator: " · "))
                .font(.caption).foregroundStyle(Theme.textSecondary)
            Text(verbatim: part.description).font(.callout).foregroundStyle(Theme.textPrimary).textSelection(.enabled)
            if part.lifecycle.needsAttention {
                Label {
                    Text(verbatim: part.lifecycleText.isEmpty ? part.lifecycle.badge : part.lifecycleText)
                } icon: { Image(systemName: "exclamationmark.triangle.fill") }
                    .font(.caption).foregroundStyle(Theme.warning)
            }
            HStack(spacing: 12) {
                if let url = URL(string: part.datasheet), !part.datasheet.isEmpty {
                    Button { openURL(url) } label: { Label("Datasheet", systemImage: "doc.text") }.buttonStyle(.link)
                }
                if let url = URL(string: part.productUrl), !part.productUrl.isEmpty {
                    Button { openURL(url) } label: { Label("Product Page", systemImage: "safari") }.buttonStyle(.link)
                }
            }
            Text("OFFERS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            if part.offers.isEmpty {
                Text("No distributor offers.").font(.caption).foregroundStyle(Theme.textMuted)
            }
            ForEach(part.offers) { offer in offerRow(offer) }
            if !part.parameters.isEmpty {
                Text("PARAMETERS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Grid(alignment: .leading, horizontalSpacing: 10, verticalSpacing: 3) {
                    ForEach(Array(part.parameters.prefix(24).enumerated()), id: \.offset) { _, p in
                        GridRow {
                            Text(verbatim: p.name).foregroundStyle(Theme.textMuted)
                            Text(verbatim: p.value).foregroundStyle(Theme.textPrimary)
                        }
                    }
                }
                .font(.caption)
            }
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private func offerRow(_ offer: SupplierOfferInfo) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            HStack {
                Text(verbatim: offer.supplier).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                Text(verbatim: offer.sku).font(.caption.monospaced()).foregroundStyle(Theme.textSecondary).textSelection(.enabled)
                Spacer()
                if let url = URL(string: offer.url), !offer.url.isEmpty {
                    Button { openURL(url) } label: { Image(systemName: "arrow.up.right.square") }
                        .buttonStyle(.borderless)
                        .help("Open the offer on the distributor's site")
                }
            }
            Text(verbatim: Self.offerFacts(offer)).font(.caption2).foregroundStyle(offer.stock > 0 ? Theme.textMuted : Theme.warning)
            if !offer.prices.isEmpty {
                Text(verbatim: offer.prices.map { "\($0.quantity)+ \(Self.money($0.price, offer.currency))" }.joined(separator: "   "))
                    .font(.caption2.monospacedDigit()).foregroundStyle(Theme.textSecondary)
            }
        }
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.deepBlue.opacity(0.7)))
    }

    static func offerFacts(_ offer: SupplierOfferInfo) -> String {
        var facts = [offer.stock >= 0 ? "\(offer.stock.formatted()) in stock" : "stock not reported"]
        if offer.moq > 1 { facts.append("MOQ \(offer.moq)") }
        if offer.multiple > 1 { facts.append("multiples of \(offer.multiple)") }
        if !offer.packaging.isEmpty { facts.append(offer.packaging) }
        if offer.leadTimeDays >= 0 { facts.append("lead time \(offer.leadTimeDays) days") }
        return facts.joined(separator: " · ")
    }

    /// "USD 0.184" style prices (four significant digits, the distributor's currency).
    static func money(_ value: Double, _ currency: String) -> String {
        let code = currency.isEmpty ? "" : currency + " "
        return code + (value < 1 ? String(format: "%.4g", value) : String(format: "%.2f", value))
    }
}

/// NRND / EOL / OBSOLETE badge (warning colour); active and new parts get a quiet one, unknown none.
struct LifecycleBadge: View {
    var lifecycle: SupplierLifecycle
    var text: String = ""

    var body: some View {
        if lifecycle != .unknown {
            Text(verbatim: lifecycle.badge)
                .font(.system(size: 9, weight: .bold))
                .padding(.horizontal, 5).padding(.vertical, 1)
                .background(Capsule().fill(lifecycle.needsAttention ? Theme.warning.opacity(0.25) : Theme.blue.opacity(0.25)))
                .foregroundStyle(lifecycle.needsAttention ? Theme.warning : Theme.skyBlue)
                .help(text.isEmpty ? lifecycle.badge : text)
        }
    }
}
