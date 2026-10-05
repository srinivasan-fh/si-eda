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

/// Title block fields printed on every schematic sheet.
struct TitleBlockEditor: View {
    @EnvironmentObject private var store: DesignStore
    @State private var block = TitleBlockInfo()

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            TextField("Title", text: $block.title).textFieldStyle(.blue).onSubmit(commit)
            TextField("Company", text: $block.company).textFieldStyle(.blue).onSubmit(commit)
            HStack {
                TextField("Revision", text: $block.revision).textFieldStyle(.blue).onSubmit(commit)
                TextField("Date", text: $block.date).textFieldStyle(.blue).onSubmit(commit)
            }
            TextField("Drawn by", text: $block.drawnBy).textFieldStyle(.blue).onSubmit(commit)
            Button("Apply", action: commit)
                .buttonStyle(.bordered)
                .controlSize(.small)
                .disabled(block == store.snapshot.titleBlock)
        }
        .onAppear { block = store.snapshot.titleBlock }
        .onChange(of: store.snapshot.titleBlock) { _, new in block = new }
    }

    private func commit() { store.setTitleBlock(block) }
}
