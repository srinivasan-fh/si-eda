import SwiftUI

/// Strategy settings of the autorouter (docs/ROUTING.md, Strategies): the preset, the quality options with their
/// values, and the copper layers each schematic net class may route on. Applied as one undo step.
struct RoutingStrategySheet: View {
    @EnvironmentObject var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var options = AutorouteOptions()
    @State private var loaded = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Routing Strategy").font(.headline)
            Form {
                presetPicker
                qualitySection
                classLayersSection
            }
            .formStyle(.grouped)
            buttons
        }
        .padding(16)
        .frame(width: 460, height: 520)
        .onAppear(perform: load)
    }

    private func load() {
        guard !loaded else { return }
        options = store.autorouteOptions
        loaded = true
    }

    private var presetPicker: some View {
        Picker("Preset", selection: Binding(get: { options.preset }, set: applyPreset)) {
            ForEach(EDAEngine.autoroutePresets().filter { !["nets", "netclass", "area"].contains($0.name) }) { preset in
                Text(LocalizedStringKey(preset.title)).tag(preset.name)
            }
        }
    }

    private func applyPreset(_ name: String) {
        guard let preset = EDAEngine.autoroutePresets().first(where: { $0.name == name }) else { return }
        var next = preset.options
        next.classLayers = options.classLayers
        next.protectLocked = options.protectLocked
        next.pinSwap = options.pinSwap
        next.pairGap = options.pairGap
        next.arcRadius = options.arcRadius
        options = next
    }

    private var qualitySection: some View {
        Section("Options") {
            Toggle("Coupled differential pairs", isOn: $options.coupledPairs)
            numberField("Pair gap (0 = stack-up)", $options.pairGap)
            Toggle("Length-aware routing", isOn: $options.lengthAware)
            Toggle("Minimize vias", isOn: $options.minimizeVias)
            Toggle("Gloss tracks", isOn: $options.gloss)
            Toggle("Arc corners", isOn: $options.arcCorners)
            numberField("Arc radius (0 = automatic)", $options.arcRadius)
            Toggle("Teardrops", isOn: $options.teardrops)
            Toggle("Fast (fewer rip-up passes)", isOn: $options.fast)
            Toggle("Protect locked copper", isOn: $options.protectLocked)
            Toggle("Swap Pins & Gates", isOn: $options.pinSwap)
                .help("Before routing, swap equivalent pins and gates where the ratsnest gets shorter (back-annotated to the schematic)")
        }
    }

    private func numberField(_ title: LocalizedStringKey, _ value: Binding<Double>) -> some View {
        LabeledContent(title) {
            TextField("", value: value, format: .number.precision(.fractionLength(0...3)))
                .frame(width: 70)
                .multilineTextAlignment(.trailing)
        }
    }

    @ViewBuilder private var classLayersSection: some View {
        Section("Net Class Layers") {
            if store.snapshot.netClassDefs.isEmpty {
                Text("No net classes on the schematic")
                    .foregroundStyle(.secondary)
            } else {
                ForEach(store.snapshot.netClassDefs) { cls in classRow(cls.name) }
            }
        }
    }

    private func classRow(_ name: String) -> some View {
        HStack {
            Text(verbatim: name).frame(width: 90, alignment: .leading)
            ScrollView(.horizontal, showsIndicators: false) {
                HStack(spacing: 6) {
                    ForEach(0..<max(1, store.snapshot.board.layerCount), id: \.self) { layer in
                        layerToggle(name, layer)
                    }
                }
            }
        }
        .help("Layers this class's nets may route on; none checked = every layer")
    }

    private func layerToggle(_ cls: String, _ layer: Int) -> some View {
        let on = options.classLayers[cls]?.contains(layer) ?? false
        return Toggle(isOn: Binding(get: { on }, set: { setLayer(cls, layer, $0) })) {
            Text(verbatim: "L\(layer + 1)")
        }
        .toggleStyle(.checkbox)
    }

    private func setLayer(_ cls: String, _ layer: Int, _ on: Bool) {
        var layers = Set(options.classLayers[cls] ?? [])
        if on { layers.insert(layer) } else { layers.remove(layer) }
        options.classLayers[cls] = layers.isEmpty ? nil : layers.sorted()
    }

    private var buttons: some View {
        HStack {
            Spacer()
            Button("Cancel") { dismiss() }
                .keyboardShortcut(.cancelAction)
            Button("Apply") {
                store.setAutorouteOptions(options)
                dismiss()
            }
            .keyboardShortcut(.defaultAction)
        }
    }
}
