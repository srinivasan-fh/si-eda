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
