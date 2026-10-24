import SwiftUI

/// Find & Replace across every sheet (⌘F in the schematic): designators, values, net labels, nets and pin names.
/// Clicking a result shows it on its sheet; Replace All edits part values and net label names in one undo step.
struct SchematicFindPanel: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var replacement = ""
    @State private var matchCase = false
    @State private var wholeWord = false
    @State private var pins = false
    @State private var hits: [SchematicSearchHit] = []

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Find & Replace").font(.headline).foregroundStyle(Theme.textPrimary)
            HStack {
                TextField("Find", text: $text)
                    .textFieldStyle(.blue)
                    .onSubmit(search)
                Button("Find", action: search).keyboardShortcut(.defaultAction)
            }
            HStack {
                TextField("Replace with", text: $replacement)
                    .textFieldStyle(.blue)
                Button("Replace All") {
                    store.replaceAll(text, with: replacement, matchCase: matchCase, wholeWord: wholeWord)
                    search()
                }
                .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
                .help("Replaces the text in part values and net label names on every sheet (designators: use Annotate)")
            }
            HStack(spacing: 14) {
                Toggle("Match case", isOn: $matchCase)
                Toggle("Whole field", isOn: $wholeWord)
                Toggle("Pin names", isOn: $pins)
            }
            .toggleStyle(.checkbox)
            .font(.caption)
            .onChange(of: matchCase) { _, _ in search() }
            .onChange(of: wholeWord) { _, _ in search() }
            .onChange(of: pins) { _, _ in search() }
            Divider()
            if hits.isEmpty {
                Text(text.isEmpty ? "Type a designator, value, label or net name." : "No matches.")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
            } else {
                Text("\(hits.count) matches").font(.caption).foregroundStyle(Theme.textMuted)
            }
            ScrollView {
                LazyVStack(alignment: .leading, spacing: 2) {
                    ForEach(hits) { hit in
                        Button {
                            if hit.component >= 0 { store.crossProbe(component: hit.component, sheet: hit.sheet) }
                        } label: {
                            HStack {
                                Text(verbatim: hit.field).font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                                    .frame(width: 44, alignment: .leading)
                                Text(verbatim: hit.text).font(.callout.monospaced()).foregroundStyle(Theme.textPrimary)
                                Spacer()
                                Text(verbatim: store.snapshot.sheet(hit.sheet)?.name ?? "")
                                    .font(.caption)
                                    .foregroundStyle(Theme.lightBlue)
                            }
                            .padding(.vertical, 2)
                            .contentShape(Rectangle())
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
            .frame(minHeight: 160, maxHeight: 320)
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.cancelAction)
            }
        }
        .padding(16)
        .frame(width: 460)
        .background(Theme.deepBlue)
    }

    private func search() {
        hits = store.find(text, matchCase: matchCase, wholeWord: wholeWord, pins: pins)
    }
}

/// Net navigator: every place a net appears, sheet by sheet. Clicking a place shows it on its sheet.
struct NetNavigatorView: View {
    @EnvironmentObject private var store: DesignStore
    var net: Int

    var body: some View {
        if let report = store.netPlaces(net), !report.places.isEmpty {
            VStack(alignment: .leading, spacing: 4) {
                Text("Net Navigator").font(.caption.weight(.semibold)).foregroundStyle(Theme.lightBlue)
                ForEach(report.places) { place in
                    Button {
                        store.crossProbe(component: place.component, sheet: place.sheet)
                    } label: {
                        HStack {
                            Text(verbatim: place.kind == "pin" ? "\(place.ref).\(place.name)" : place.name)
                                .font(.caption.monospaced())
                                .foregroundStyle(Theme.textPrimary)
                            Text(verbatim: place.kind == "pin" ? "" : place.kind)
                                .font(.caption2)
                                .foregroundStyle(Theme.textMuted)
                            Spacer()
                            Text(verbatim: store.snapshot.sheet(place.sheet)?.name ?? "")
                                .font(.caption2)
                                .foregroundStyle(Theme.lightBlue)
                        }
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .help("Show this place on its sheet")
                }
            }
        }
    }
}

/// Title block fields printed on every schematic sheet and on the board's fabrication drawing.
struct TitleBlockEditor: View {
    @EnvironmentObject private var store: DesignStore
    @State private var block = TitleBlockInfo()
    /// Customer templates: the company / drawn-by fields kept by name (user defaults), applied to any project.
    @AppStorage("titleBlock.customers") private var customersData = Data()

    private var customers: [String: [String: String]] {
        (try? JSONDecoder().decode([String: [String: String]].self, from: customersData)) ?? [:]
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            Menu("Customer Template") {
                ForEach(customers.keys.sorted(), id: \.self) { name in
                    Button(name) {
                        block.company = customers[name]?["company"] ?? block.company
                        block.drawnBy = customers[name]?["drawnBy"] ?? block.drawnBy
                        commit()
                    }
                }
                Divider()
                Button("Save as Customer Template") {
                    var all = customers
                    all[block.company.isEmpty ? "Customer" : block.company] = ["company": block.company, "drawnBy": block.drawnBy]
                    customersData = (try? JSONEncoder().encode(all)) ?? customersData
                }
                .disabled(block.company.isEmpty && block.drawnBy.isEmpty)
            }
            .controlSize(.small)
            TextField("Title", text: $block.title).textFieldStyle(.blue).onSubmit(commit)
            TextField("Company", text: $block.company).textFieldStyle(.blue).onSubmit(commit)
            HStack {
                TextField("Revision", text: $block.revision).textFieldStyle(.blue).onSubmit(commit)
                TextField("Date", text: $block.date).textFieldStyle(.blue).onSubmit(commit)
            }
            TextField("Drawn by", text: $block.drawnBy).textFieldStyle(.blue).onSubmit(commit)
            Text("Revisions").font(.caption.weight(.semibold)).foregroundStyle(Theme.textSecondary)
            ForEach(Array((block.revisions ?? []).indices), id: \.self) { i in
                HStack(spacing: 4) {
                    TextField("Rev", text: revision(i, \.rev)).textFieldStyle(.blue).frame(width: 36)
                    TextField("Date", text: revision(i, \.date)).textFieldStyle(.blue).frame(width: 78)
                    TextField("Description", text: revision(i, \.description)).textFieldStyle(.blue)
                    TextField("By", text: revision(i, \.by)).textFieldStyle(.blue).frame(width: 36)
                    Button { block.revisions?.remove(at: i) } label: { Image(systemName: "minus.circle") }
                        .buttonStyle(.plain)
                        .help("Remove this revision")
                }
            }
            Button("Add Revision") {
                let next = (block.revisions ?? []).count
                block.revisions = (block.revisions ?? []) + [RevisionInfo(rev: String(UnicodeScalar(UInt8(65 + next % 26))),
                                                                          date: ISO8601DateFormatter.dateOnly.string(from: Date()))]
            }
            .buttonStyle(.borderless)
            .controlSize(.small)
            Button("Apply", action: commit)
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(block == store.snapshot.titleBlock)
        }
        .onAppear { block = store.snapshot.titleBlock }
        .onChange(of: store.snapshot.titleBlock) { _, new in block = new }
    }

    private func commit() { store.setTitleBlock(block) }

    private func revision(_ i: Int, _ key: WritableKeyPath<RevisionInfo, String>) -> Binding<String> {
        Binding(get: { block.revisions?.indices.contains(i) == true ? block.revisions![i][keyPath: key] : "" },
                set: { if block.revisions?.indices.contains(i) == true { block.revisions![i][keyPath: key] = $0 } })
    }
}

extension ISO8601DateFormatter {
    /// yyyy-MM-dd, for revision dates.
    static let dateOnly: ISO8601DateFormatter = {
        let f = ISO8601DateFormatter()
        f.formatOptions = [.withFullDate]
        return f
    }()
}
