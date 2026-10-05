import AppKit
import SceneKit
import SwiftUI
import UniformTypeIdentifiers

/// 3D Model (Component Library → 3D Model…): attach a VRML (.wrl), STL or OBJ model to a part and align it on the
/// footprint — file unit, scale, rotation about x / y / z and offset — with a live preview of the part on a piece of
/// board. "Seat on Board" centres the model on the footprint and puts its lowest point on the board surface. The
/// model is saved with the project and replaces the generated body in the 3D view and the STL / OBJ exports.
struct Model3DEditorView: View {
    let onSave: (CustomPartSpec) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var spec: CustomPartSpec
    @State private var info: String = ""
    @State private var warnings: [String] = []
    @State private var error: String?
    @State private var mesh: MeshData?
    @State private var previewTask: Task<Void, Never>?

    init(spec: CustomPartSpec, onSave: @escaping (CustomPartSpec) -> Void) {
        self.onSave = onSave
        _spec = State(initialValue: spec)
    }

    /// Units a model file may be written in (millimetres per file unit).
    struct FileUnit: Hashable {
        var title: String
        var mm: Double
    }
    static let units = [FileUnit(title: "mm", mm: 1), FileUnit(title: "0.1 inch (KiCad VRML)", mm: 2.54),
                        FileUnit(title: "inch", mm: 25.4), FileUnit(title: "mil", mm: 0.0254), FileUnit(title: "m", mm: 1000)]

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            HStack {
                Text("3D Model").font(.headline).foregroundStyle(Theme.skyBlue)
                Text(verbatim: spec.name).foregroundStyle(Theme.textSecondary)
                Spacer()
                Button { chooseFile() } label: { Label("Choose File…", systemImage: "cube") }
                    .help("VRML (.wrl, as KiCad libraries ship), STL or OBJ. STEP files cannot be read: use the .wrl next to them.")
            }
            .padding(12)
            Divider()
            HStack(alignment: .top, spacing: 0) {
                Model3DPreview(mesh: mesh)
                    .frame(minWidth: 380, maxWidth: .infinity, minHeight: 360, maxHeight: .infinity)
                Divider()
                ScrollView { controls }
                    .frame(width: 300)
            }
            Divider()
            HStack {
                if spec.model3d != nil {
                    Button("Remove Model", role: .destructive) { spec.model3d = nil }
                        .help("Use the generated body again")
                }
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Apply") {
                    onSave(spec)
                    dismiss()
                }
                .keyboardShortcut(.defaultAction)
                .buttonStyle(.borderedProminent)
            }
            .padding(12)
        }
        .frame(minWidth: 760, minHeight: 520)
        .background(Theme.navy)
        .onAppear { refresh() }
        .onChange(of: spec) { _, _ in refresh() }
        .onDisappear { previewTask?.cancel() }
    }

    @ViewBuilder
    private var controls: some View {
        VStack(alignment: .leading, spacing: 10) {
            if let model = spec.model3d {
                Text(verbatim: model.name).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                if !info.isEmpty { Text(verbatim: info).font(.caption).foregroundStyle(Theme.textMuted) }
                Picker("File unit", selection: binding(\.unit)) {
                    ForEach(Self.units, id: \.self) { Text(verbatim: $0.title).tag($0.mm) }
                    if !Self.units.contains(where: { $0.mm == model.unit }) {
                        Text(verbatim: String(format: "%g mm", model.unit)).tag(model.unit)
                    }
                }
                axisRow("Offset (mm)", \.offset, step: 0.05)
                axisRow("Rotation (°)", \.rotate, step: 90)
                axisRow("Scale", \.scale, step: 0.1)
                HStack {
                    Button("Seat on Board") { seat() }
                        .help("Centre the model on the footprint and put its lowest point on the board surface")
                    Button("Reset") {
                        spec.model3d?.offset = [0, 0, 0]
                        spec.model3d?.rotate = [0, 0, 0]
                        spec.model3d?.scale = [1, 1, 1]
                    }
                }
                Text("x right, y towards the top of the PCB view, z up from the board. Rotations turn about x, then y, then z.")
                    .font(.caption2).foregroundStyle(Theme.textMuted)
            } else {
                Text("No 3D model: the part shows its generated body.").font(.callout).foregroundStyle(Theme.textMuted)
                Text("Choose a VRML (.wrl), STL or OBJ file. KiCad libraries ship a .wrl next to every .step model.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            if let error {
                Label { Text(verbatim: error) } icon: { Image(systemName: "exclamationmark.triangle.fill") }
                    .font(.caption).foregroundStyle(Theme.error)
            }
            ForEach(warnings, id: \.self) { warning in
                Text(verbatim: warning).font(.caption2).foregroundStyle(Theme.warning)
            }
        }
        .padding(12)
    }

    private func binding(_ key: WritableKeyPath<CustomPartSpec.Model3DRef, Double>) -> Binding<Double> {
        Binding(get: { spec.model3d?[keyPath: key] ?? 1 }, set: { spec.model3d?[keyPath: key] = $0 })
    }

    private func component(_ key: WritableKeyPath<CustomPartSpec.Model3DRef, [Double]>, _ i: Int) -> Binding<Double> {
        Binding(get: {
            guard let values = spec.model3d?[keyPath: key], i < values.count else { return 0 }
            return values[i]
        }, set: { value in
            guard var model = spec.model3d, i < model[keyPath: key].count, value.isFinite else { return }
            model[keyPath: key][i] = value
            spec.model3d = model
        })
    }

    private func axisRow(_ title: LocalizedStringKey, _ key: WritableKeyPath<CustomPartSpec.Model3DRef, [Double]>,
                         step: Double) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            Text(title).font(.caption).foregroundStyle(Theme.textMuted)
            HStack(spacing: 6) {
                ForEach(0..<3, id: \.self) { i in
                    HStack(spacing: 2) {
                        Text(verbatim: ["x", "y", "z"][i]).font(.caption2).foregroundStyle(Theme.textMuted)
                        TextField("", value: component(key, i), format: .number.precision(.fractionLength(0...4)))
                            .textFieldStyle(.roundedBorder)
                            .frame(width: 58)
                        Stepper("", value: component(key, i), step: step).labelsHidden()
                    }
                }
            }
        }
    }

    // MARK: - Actions

    private func chooseFile() {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        panel.canChooseDirectories = false
        panel.allowedContentTypes = EDAEngine.model3dExtensions.compactMap { UTType(filenameExtension: $0) }
        guard panel.runModal() == .OK, let url = panel.url else { return }
        guard let data = try? Data(contentsOf: url), data.count <= 32 << 20 else {
            error = "The file cannot be read or is larger than 32 MB."
            return
        }
        let result = EDAEngine.importModel3D(name: url.lastPathComponent, data: data)
        guard result.ok, let id = result.id else {
            error = result.error
            return
        }
        error = nil
        warnings = result.warnings ?? []
        var model = CustomPartSpec.Model3DRef(id: id, name: url.lastPathComponent, unit: result.unit ?? 1)
        if let old = spec.model3d {  // keep a rotation or scale the user already set for this part
            model.rotate = old.rotate
            model.scale = old.scale
        }
        spec.model3d = model
        seat()
    }

    private func seat() {
        let fit = EDAEngine.fitModel3D(spec)
        if fit.ok, let seated = fit.seated { spec.model3d = seated } else if !fit.error.isEmpty { error = fit.error }
    }

    private func refresh() {
        previewTask?.cancel()
        let current = spec
        previewTask = Task { @MainActor in
            try? await Task.sleep(nanoseconds: 150_000_000)
            guard !Task.isCancelled else { return }
            let built = await Task.detached(priority: .userInitiated) { EDAEngine.model3DPreviewMesh(current) }.value
            guard !Task.isCancelled else { return }
            mesh = built
            if current.model3d != nil {
                let fit = EDAEngine.fitModel3D(current)
                if fit.ok, let b = fit.bounds, b.count == 6 {
                    info = String(format: "%.2f × %.2f × %.2f mm", b[3] - b[0], b[4] - b[1], b[5] - b[2])
                    error = nil
                } else if !fit.error.isEmpty {
                    error = fit.error
                }
            } else {
                info = ""
            }
        }
    }
}

/// SceneKit preview of the part on its piece of board (same materials as the 3D assembly view).
struct Model3DPreview: NSViewRepresentable {
    let mesh: MeshData?

    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        view.scene = SCNScene()
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = true
        view.antialiasingMode = .multisampling4X
        view.backgroundColor = NSColor(red: 0.03, green: 0.06, blue: 0.13, alpha: 1)
        view.scene?.lightingEnvironment.contents = BoardSceneView.studioEnvironment
        view.scene?.lightingEnvironment.intensity = 1.2
        return view
    }

    func updateNSView(_ view: SCNView, context: Context) {
        guard let scene = view.scene else { return }
        scene.rootNode.childNodes.filter { $0.name == "part" }.forEach { $0.removeFromParentNode() }
        guard let mesh, mesh.vertexCount > 0 else { return }
        let node = BoardSceneView.assemblyNode(from: mesh, finish: .enig)
        node.name = "part"
        // Centre the board piece on the origin and frame it.
        let (minB, maxB) = node.boundingBox
        let centre = SCNVector3((minB.x + maxB.x) / 2, (minB.y + maxB.y) / 2, (minB.z + maxB.z) / 2)
        node.position = SCNVector3(-centre.x, -centre.y, -centre.z)
        scene.rootNode.addChildNode(node)
        let span = max(maxB.x - minB.x, maxB.z - minB.z, 4)
        if scene.rootNode.childNode(withName: "camera", recursively: false) == nil {
            let camera = SCNNode()
            camera.name = "camera"
            camera.camera = SCNCamera()
            camera.camera?.zNear = 0.05
            camera.camera?.zFar = 1000
            camera.position = SCNVector3(span * 0.9, span * 1.1, span * 1.3)
            camera.look(at: SCNVector3(0, 0, 0))
            scene.rootNode.addChildNode(camera)
            view.pointOfView = camera
        }
    }
}
