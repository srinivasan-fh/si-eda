import AppKit
import SwiftUI

/// What changed from another version of the project to the open design, one line per change.
struct DesignDiffView: View {
    let diff: DesignDiff
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Changes since \(diff.title)").font(.headline)
            ScrollView {
                VStack(alignment: .leading, spacing: 2) {
                    ForEach(Array(diff.text.split(separator: "\n").enumerated()), id: \.offset) { _, line in
                        Text(String(line))
                            .foregroundStyle(line.hasPrefix("+") ? Color.green : line.hasPrefix("-") ? Color.red
                                             : line.hasPrefix("~") ? Color.orange : Theme.textPrimary)
                    }
                }
                .font(.system(.body, design: .monospaced))
                .textSelection(.enabled)
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            HStack {
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(diff.text, forType: .string)
                }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(minWidth: 560, minHeight: 380)
    }
}

/// The variants side by side: every part a variant changes, fitted value or "DNP" per variant, and totals.
struct VariantMatrixView: View {
    let matrix: VariantMatrix
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Compare Variants").font(.headline)
            ScrollView([.horizontal, .vertical]) {
                Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 6) {
                    GridRow {
                        Text("Part").bold()
                        Text("Base").bold()
                        ForEach(matrix.variants) { Text($0.name).bold() }
                    }
                    Divider()
                    ForEach(matrix.parts) { row in
                        GridRow {
                            Text(row.ref)
                            Text(row.value).foregroundStyle(Theme.textSecondary)
                            ForEach(Array(row.cells.enumerated()), id: \.offset) { _, cell in
                                Text(cell.fitted ? cell.value : "DNP")
                                    .foregroundStyle(!cell.fitted ? Color.red : cell.value != row.value ? Color.orange : Theme.textPrimary)
                            }
                        }
                    }
                    Divider()
                    GridRow {
                        Text("Fitted").bold()
                        Text("")
                        ForEach(matrix.variants) { Text("\($0.fitted) (\($0.notFitted) DNP)") }
                    }
                }
                .font(.system(.body, design: .monospaced))
                .padding(.vertical, 4)
            }
            if matrix.parts.isEmpty { Text("No variant changes a part yet.").foregroundStyle(Theme.textSecondary) }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(minWidth: 480, minHeight: 320)
    }
}

/// Design review: comments pinned to parts, with replies; resolve, reopen, delete, copy the review as Markdown.
struct DesignReviewView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var text = ""
    @State private var ref = ""
    @State private var replies: [Int: String] = [:]
    @AppStorage("review.author") private var author = NSFullUserName()

    var body: some View {
        let review = store.engine.review()
        let comments = (review?.comments ?? []).sorted { ($0.resolved ? 1 : 0, $0.id) < ($1.resolved ? 1 : 0, $1.id) }
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text("Design Review").font(.headline)
                Spacer()
                Text("\(review?.open ?? 0) open").foregroundStyle(Theme.textSecondary)
            }
            HStack {
                TextField("Comment", text: $text).textFieldStyle(.roundedBorder)
                TextField("Part (optional)", text: $ref).textFieldStyle(.roundedBorder).frame(width: 120)
                Button("Add") {
                    var q: [String: Any] = ["action": "add", "text": text, "author": author]
                    if !ref.isEmpty { q["ref"] = ref }
                    store.review(q, "Review comment")
                    text = ""
                }
                .disabled(text.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            List(comments) { c in
                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text("#\(c.id)").monospacedDigit().foregroundStyle(Theme.textSecondary)
                        if let r = c.ref { Text(r).bold() }
                        Text(c.text).strikethrough(c.resolved)
                        Spacer()
                        Button(c.resolved ? LocalizedStringKey("Reopen") : LocalizedStringKey("Resolve")) {
                            store.review(["action": c.resolved ? "reopen" : "resolve", "id": c.id], "Review status")
                        }
                        Button(role: .destructive) { store.review(["action": "delete", "id": c.id], "Delete comment") } label: {
                            Image(systemName: "trash")
                        }
                        .buttonStyle(.borderless)
                        .help("Delete comment")
                    }
                    if !c.author.isEmpty { Text(c.author).font(.caption).foregroundStyle(Theme.textSecondary) }
                    ForEach(c.replies ?? [], id: \.self) { r in
                        Text((r.author.isEmpty ? "" : r.author + ": ") + r.text).font(.callout).padding(.leading, 16)
                    }
                    if !c.resolved {
                        TextField("Reply", text: Binding(get: { replies[c.id] ?? "" }, set: { replies[c.id] = $0 }))
                            .textFieldStyle(.roundedBorder)
                            .padding(.leading, 16)
                            .onSubmit {
                                guard let t = replies[c.id], !t.isEmpty else { return }
                                store.review(["action": "reply", "id": c.id, "text": t, "author": author], "Reply")
                                replies[c.id] = nil
                            }
                    }
                }
                .padding(.vertical, 2)
            }
            HStack {
                Button("Copy as Markdown") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(review?.markdown ?? "", forType: .string)
                }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(20)
        .frame(minWidth: 620, minHeight: 420)
    }
}
