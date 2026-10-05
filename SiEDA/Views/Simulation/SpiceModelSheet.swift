import SwiftUI

/// Imports vendor SPICE model text (.lib / .mod / .cir) and attaches one .model or .subckt to a part with its pin map.
/// The model is checked on the part as it is edited: ports, the default pin map and every diagnostic with its line.
struct SpiceModelSheet: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    let component: SnapComponent

    @State private var text = ""
    @State private var parsed = SpiceParseResult()
    @State private var selection = ""
    @State private var pins = ""
    @State private var check = SpiceCheckResult()

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("SPICE Model for \(component.ref)").font(.headline).foregroundStyle(Theme.textPrimary)
            Text("Paste or open vendor model text (.lib, .mod, .cir): .model and .subckt definitions.")
                .font(.caption)
                .foregroundStyle(Theme.textMuted)
            HStack {
                Button {
                    if let loaded = store.chooseSpiceModelFile() { text = loaded }
                } label: {
                    Label("Open File…", systemImage: "doc")
                }
                Menu {
                    ForEach(EDAEngine.builtinSpiceModels) { model in
                        Button { text = model.text } label: { Text(verbatim: "\(model.name) — \(model.description)") }
                    }
                } label: {
                    Label("Library", systemImage: "books.vertical")
                }
                .fixedSize()
                Spacer()
            }
            .controlSize(.small)
            TextEditor(text: $text)
                .font(.system(.caption, design: .monospaced))
                .frame(minHeight: 180)
                .overlay(RoundedRectangle(cornerRadius: 4).stroke(Theme.darkBlue))
            if parsed.entries.isEmpty {
                Text("No models or subcircuits found.").font(.caption).foregroundStyle(Theme.textMuted)
            } else {
                Picker("Model", selection: $selection) {
                    ForEach(parsed.entries) { entry in
                        Text(verbatim: "\(entry.name)  (\(entry.summary))").tag(entry.name)
                    }
                }
            }
            if let ports = check.ports, !ports.isEmpty {
                Text("Ports: \(ports.joined(separator: " "))").font(.caption).foregroundStyle(Theme.textSecondary)
            }
            TextField("Pin map", text: $pins, prompt: Text(verbatim: check.defaultPins ?? ""))
                .textFieldStyle(.roundedBorder)
                .font(.system(.callout, design: .monospaced))
            Text("One entry per model port, in order: a pin name or number, 0 (ground), net:NAME, dc:15 (ideal supply) or nc; separate instances with ;.")
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
            diagnosticsList
            if !selection.isEmpty, !check.ok, !check.error.isEmpty {
                Text(verbatim: check.error).font(.caption).foregroundStyle(Theme.error).fixedSize(horizontal: false, vertical: true)
            }
            Text("The imported model replaces the built-in one in every analysis.")
                .font(.caption2)
                .foregroundStyle(Theme.textMuted)
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Attach") {
                    if store.setSpiceModel(component.id, text: text, model: selection, pins: trimmedPins) { dismiss() }
                }
                .keyboardShortcut(.defaultAction)
                .disabled(!check.ok)
            }
        }
        .padding(16)
        .frame(minWidth: 560, minHeight: 540)
        .background(Theme.deepBlue)
        .onAppear {
            if let existing = store.engine.spiceModel(of: component.id) {
                text = existing.text
                selection = existing.model
                pins = existing.pins
            }
            reparse()
        }
        .onChange(of: text) { _, _ in reparse() }
        .onChange(of: selection) { _, _ in recheck() }
        .onChange(of: pins) { _, _ in recheck() }
    }

    private var trimmedPins: String { pins.trimmingCharacters(in: .whitespacesAndNewlines) }

    @ViewBuilder private var diagnosticsList: some View {
        let diagnostics = Array((check.diagnostics ?? parsed.diagnostics).prefix(60))
        if !diagnostics.isEmpty {
            ScrollView {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(diagnostics) { d in
                        HStack(alignment: .firstTextBaseline, spacing: 5) {
                            Image(systemName: d.isError ? "xmark.octagon.fill" : (d.isWarning ? "exclamationmark.triangle.fill" : "info.circle"))
                                .foregroundStyle(d.isError ? Theme.error : (d.isWarning ? Theme.warning : Theme.textMuted))
                            Text(verbatim: d.line > 0 ? "\(d.line): \(d.message)" : d.message)
                                .foregroundStyle(Theme.textPrimary)
                                .fixedSize(horizontal: false, vertical: true)
                        }
                        .font(.caption)
                    }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            }
            .frame(maxHeight: 120)
        }
    }

    private func reparse() {
        parsed = EDAEngine.parseSpice(text)
        if !parsed.entries.contains(where: { $0.name == selection }) { selection = parsed.entries.first?.name ?? "" }
        recheck()
    }

    private func recheck() {
        guard !selection.isEmpty else {
            check = SpiceCheckResult()
            return
        }
        check = store.engine.checkSpiceModel(component.id, text: text, model: selection, pins: trimmedPins)
    }
}
