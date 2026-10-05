import SwiftUI

/// The schematic directive on a net (Altium's net class / differential pair / parameter set directives), edited from
/// a net label or a wire: the net's class, whether it is a differential pair member, and its own track width and
/// clearance. The schematic is the source of the board's net rules: every change goes to the PCB rules at once.
struct NetDirectiveEditor: View {
    @EnvironmentObject private var store: DesignStore
    /// The pin the directive sits on (a block part's pin for a channel copy).
    let anchor: PinAddress
    @State private var netClass: String
    @State private var diffPair: Bool
    @State private var width: String
    @State private var clearance: String

    init(anchor: PinAddress, directive: DirectiveInfo?) {
        self.anchor = anchor
        _netClass = State(initialValue: directive?.netClass ?? "")
        _diffPair = State(initialValue: directive?.isDiffPair ?? false)
        _width = State(initialValue: directive?.trackWidth.map { String(format: "%.2f", $0) } ?? "")
        _clearance = State(initialValue: directive?.clearance.map { String(format: "%.2f", $0) } ?? "")
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("NET DIRECTIVE").font(.caption.weight(.semibold)).foregroundStyle(Theme.lightBlue)
            Picker("Net class", selection: $netClass) {
                Text("None").tag("")
                ForEach(store.snapshot.netClassDefs) { Text(verbatim: $0.name).tag($0.name) }
            }
            .onChange(of: netClass) { _, _ in commit() }
            Toggle("Differential pair member", isOn: $diffPair)
                .onChange(of: diffPair) { _, _ in commit() }
                .help("Pairs this net with the net of the opposite suffix (X_P / X_N): routed, tuned and checked as a pair")
            LabeledContent("Track width (mm)") {
                TextField("Class / board", text: $width).textFieldStyle(.roundedBorder).frame(width: 90)
                    .onSubmit { commit() }
            }
            LabeledContent("Clearance (mm)") {
                TextField("Class / board", text: $clearance).textFieldStyle(.roundedBorder).frame(width: 90)
                    .onSubmit { commit() }
            }
            Text("Carried to the PCB rules: routing and DRC use them.").font(.caption).foregroundStyle(Theme.textMuted)
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .bluePanel()
    }

    private func commit() {
        let w = Double(width.trimmingCharacters(in: .whitespaces)) ?? 0
        let c = Double(clearance.trimmingCharacters(in: .whitespaces)) ?? 0
        let current = store.directive(on: anchor)
        guard current?.netClass ?? "" != netClass || current?.isDiffPair ?? false != diffPair
                || (current?.trackWidth ?? 0) != w || (current?.clearance ?? 0) != c else { return }
        store.setDirective(on: anchor, netClass: netClass, diffPair: diffPair, trackWidth: max(0, w), clearance: max(0, c))
    }
}

/// Net classes of the design (schematic directives): name, track width and clearance. Directives on nets name them.
struct NetClassesView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var width = ""
    @State private var clearance = ""
    @State private var selected: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("NET CLASSES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            Text("The schematic is the source of the board's net rules. Put a net class on a net from the inspector of a net label or wire; the board's routing and DRC take its width and clearance.")
                .font(.caption).foregroundStyle(Theme.textMuted)
                .fixedSize(horizontal: false, vertical: true)
            List(selection: $selected) {
                ForEach(store.snapshot.netClassDefs) { def in
                    HStack {
                        Text(verbatim: def.name).font(.body.weight(.semibold))
                        Spacer()
                        Text(verbatim: Self.describe(def)).font(.caption.monospaced()).foregroundStyle(Theme.textMuted)
                    }
                    .tag(def.name)
                }
            }
            .frame(minHeight: 120)
            .onChange(of: selected) { _, name in
                guard let name, let def = store.snapshot.netClassDefs.first(where: { $0.name == name }) else { return }
                self.name = def.name
                width = def.trackWidth.map { String(format: "%.2f", $0) } ?? ""
                clearance = def.clearance.map { String(format: "%.2f", $0) } ?? ""
            }
            LabeledContent("Name") { TextField("HighSpeed", text: $name).textFieldStyle(.roundedBorder) }
            LabeledContent("Track width (mm)") { TextField("Board default", text: $width).textFieldStyle(.roundedBorder) }
            LabeledContent("Clearance (mm)") { TextField("Board default", text: $clearance).textFieldStyle(.roundedBorder) }
            let used = Set(store.snapshot.directives.compactMap(\.netClass))
            HStack {
                Button("Save Class") {
                    let w = Double(width.trimmingCharacters(in: .whitespaces)) ?? 0
                    let c = Double(clearance.trimmingCharacters(in: .whitespaces)) ?? 0
                    if store.setNetClass(name, trackWidth: max(0, w), clearance: max(0, c)) {
                        selected = name.trimmingCharacters(in: .whitespaces)
                    }
                }
                .disabled(name.trimmingCharacters(in: .whitespaces).isEmpty)
                Button("Delete Class", role: .destructive) {
                    if let selected { store.removeNetClass(selected) }
                    selected = nil
                }
                .disabled(selected == nil || used.contains(selected ?? ""))
                .help("A class still used by a directive cannot be deleted")
                Spacer()
                Button("Done") { dismiss() }
                    .keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(width: 460, height: 440)
    }

    static func describe(_ def: NetClassDefInfo) -> String {
        let w = def.trackWidth.map { String(format: "w %.2f mm", $0) } ?? "w board"
        let c = def.clearance.map { String(format: "c %.2f mm", $0) } ?? "c board"
        return w + " · " + c
    }
}
