import SceneKit
import SwiftUI

/// 3D workspace with two modes:
/// - **Assembly** (default): the C++ core tessellates board, copper, vias, silkscreen and component bodies (realistic),
///   in the board's solder mask colour (green unless another is chosen).
/// - **X-Ray Stack**: every copper layer floats apart in a holographic, additive-glow exploded view with
///   through-vias as light pillars, wireframe component bodies and a scanning beam.
struct Board3DWorkspace: View {
    enum Mode: String, CaseIterable, Identifiable {
        case assembly = "Assembly"
        case xray = "X-Ray Stack"
        var id: String { rawValue }
    }

    @EnvironmentObject private var store: DesignStore
    @AppStorage("threeD.mode.v2") private var modeRaw = Mode.assembly.rawValue
    @State private var showComponents = true
    @State private var resetCamera = 0
    @State private var stats = ""
    @State private var xray = XRaySettings()

    private var mode: Mode { Mode(rawValue: modeRaw) ?? .assembly }

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Picker("Mode", selection: $modeRaw) {
                    Label("Assembly", systemImage: "cube.transparent").tag(Mode.assembly.rawValue)
                    Label("X-Ray Stack", systemImage: "square.3.layers.3d.top.filled").tag(Mode.xray.rawValue)
                }
                .pickerStyle(.segmented)
                .frame(width: 230)
                if mode == .xray {
                    xrayControls
                } else {
                    Toggle("Components", isOn: $showComponents).toggleStyle(.switch).controlSize(.mini)
                    maskPicker
                    let layers = store.snapshot.board.layerCount
                    Label(layers == 1 ? "Single-sided" : "\(layers)-layer", systemImage: "square.3.layers.3d.down.right")
                        .font(.caption)
                        .foregroundStyle(Theme.textSecondary)
                        .help("Stack-up from the Layers setting in PCB Layout: 1 layer leaves the underside bare FR-4; "
                              + "4 and 6 layers show their inner copper on the board edge")
                    Text(stats).foregroundStyle(Theme.textMuted).font(.caption)
                }
                Spacer()
                Button { resetCamera += 1 } label: { Label("Reset View", systemImage: "camera.metering.center.weighted") }
                Menu {
                    Button("STL (ASCII)") { store.export(.stl) }
                    Button("OBJ with vertex colours") { store.export(.obj) }
                } label: {
                    Label("Export 3D", systemImage: "square.and.arrow.up")
                }
                .fixedSize()
            }
            .buttonStyle(.borderless)

            ZStack(alignment: .bottomLeading) {
                if store.snapshot.pads.isEmpty {
                    BlueEmptyState(systemImage: "cube.transparent", title: "Nothing to show yet",
                                   message: "Place footprints in the PCB Layout workspace to build the 3D model.",
                                   actionTitle: "Auto-Place Footprints") { store.autoPlace(all: true) }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if mode == .xray {
                    XRayStackView(engine: store.engine, snapshot: store.snapshot, revision: store.revision,
                                  settings: xray, resetToken: resetCamera)
                    layerLegend
                        .padding(12)
                } else {
                    BoardSceneView(engine: store.engine, revision: store.revision, includeComponents: showComponents,
                                   board: store.snapshot.board, resetToken: resetCamera, stats: $stats)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Theme.navy)
            .overlay(alignment: .bottomTrailing) {
                if !store.snapshot.pads.isEmpty { navigationHint.padding(12) }
            }
        }
    }

    /// Solder mask colour swatches (green, black, blue, red, yellow, white, purple), like a fab's sample boards.
    private var maskPicker: some View {
        HStack(spacing: 5) {
            Text("Mask").font(.caption).foregroundStyle(Theme.textMuted)
            ForEach(SolderMaskColour.allCases) { mask in
                let selected = store.snapshot.board.mask == mask
                let c = mask.swatch
                Button { store.setSolderMask(mask) } label: {
                    Circle()
                        .fill(Color(red: c.red, green: c.green, blue: c.blue))
                        .overlay(Circle().strokeBorder(Color.white.opacity(0.35), lineWidth: 0.5))
                        .frame(width: 14, height: 14)
                        .padding(2)
                        .overlay(Circle().strokeBorder(selected ? Theme.skyBlue : .clear, lineWidth: 1.5))
                }
                .buttonStyle(.plain)
                .help("\(mask.title) solder mask")
                .accessibilityLabel("\(mask.title) solder mask")
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
    }

    /// SceneKit camera controls are invisible by default; spell them out.
    private var navigationHint: some View {
        VStack(alignment: .trailing, spacing: 3) {
            Label("Drag to orbit", systemImage: "rotate.3d")
            Label("Scroll or pinch to zoom", systemImage: "plus.magnifyingglass")
            Label("⌥-drag or two-finger drag to pan", systemImage: "hand.draw")
            Label("Reset View re-centres the board", systemImage: "camera.metering.center.weighted")
        }
        .labelStyle(.titleAndIcon)
        .font(.caption2)
        .foregroundStyle(Theme.textSecondary)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.7)))
        .allowsHitTesting(false)
    }

    private var xrayControls: some View {
        HStack(spacing: 12) {
            HStack(spacing: 6) {
                Image(systemName: "arrow.up.and.down.square").foregroundStyle(Theme.skyBlue)
                Slider(value: $xray.explode, in: 0.5...16).frame(width: 110)
                Text(String(format: "%.1f mm", xray.explode)).font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
            }
            .help("Layer separation (exploded view)")
            Toggle("Parts", isOn: $xray.showComponents).toggleStyle(.switch).controlSize(.mini)
            Toggle("Vias", isOn: $xray.showVias).toggleStyle(.switch).controlSize(.mini)
            Toggle("Scan", isOn: $xray.scan).toggleStyle(.switch).controlSize(.mini)
            Toggle("Spin", isOn: $xray.spin).toggleStyle(.switch).controlSize(.mini)
        }
    }

    /// Per-layer chips (click to hide a layer of the stack).
    private var layerLegend: some View {
        let board = store.snapshot.board
        return VStack(alignment: .leading, spacing: 4) {
            Text("STACK-UP · \(board.layerCount) LAYER\(board.layerCount == 1 ? "" : "S")")
                .font(.caption2.weight(.bold)).foregroundStyle(Theme.skyBlue)
            ForEach(0..<max(1, board.layerCount), id: \.self) { layer in
                let hidden = xray.hiddenLayers.contains(layer)
                Button {
                    if hidden { xray.hiddenLayers.remove(layer) } else { xray.hiddenLayers.insert(layer) }
                } label: {
                    HStack(spacing: 6) {
                        RoundedRectangle(cornerRadius: 2)
                            .fill(Theme.copperColor(layer, layerCount: board.layerCount).opacity(hidden ? 0.25 : 1))
                            .frame(width: 18, height: 6)
                            .shadow(color: Theme.copperColor(layer, layerCount: board.layerCount), radius: hidden ? 0 : 4)
                        Text("L\(layer + 1) · \(board.layerName(layer))")
                            .font(.caption.monospaced())
                            .foregroundStyle(hidden ? Theme.textMuted : Theme.textPrimary)
                    }
                }
                .buttonStyle(.plain)
            }
            Text("\(store.snapshot.tracks.count) tracks · \(store.snapshot.vias.count) vias")
                .font(.caption2).foregroundStyle(Theme.textMuted)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.navy.opacity(0.75)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.skyBlue.opacity(0.35)))
    }
}

struct BoardSceneView: NSViewRepresentable {
    let engine: EDAEngine
    let revision: Int
    let includeComponents: Bool
    let board: BoardInfo
    let resetToken: Int
    @Binding var stats: String

    final class Coordinator {
        var lastRevision = -1
        var lastComponents = true
        var lastReset = 0
        let boardNode = SCNNode()
        let cameraNode = SCNNode()
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        let scene = SCNScene()
        view.scene = scene
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = false
        view.antialiasingMode = .multisampling4X
        view.backgroundColor = NSColor(red: 0.03, green: 0.06, blue: 0.13, alpha: 1)
        view.defaultCameraController.interactionMode = .orbitTurntable
        view.defaultCameraController.inertiaEnabled = true

        // Soft blue environment gradient
        scene.background.contents = NSColor(red: 0.03, green: 0.06, blue: 0.13, alpha: 1)

        let camera = SCNCamera()
        camera.zNear = 0.1
        camera.zFar = 2000
        camera.fieldOfView = 35
        context.coordinator.cameraNode.camera = camera
        scene.rootNode.addChildNode(context.coordinator.cameraNode)
        view.pointOfView = context.coordinator.cameraNode

        let ambient = SCNNode()
        ambient.light = SCNLight()
        ambient.light?.type = .ambient
        ambient.light?.intensity = 350
        ambient.light?.color = NSColor(red: 0.95, green: 0.96, blue: 1.0, alpha: 1)  // near-neutral: true mask colours
        scene.rootNode.addChildNode(ambient)

        let key = SCNNode()
        key.light = SCNLight()
        key.light?.type = .directional
        key.light?.intensity = 900
        key.light?.castsShadow = true
        key.light?.shadowRadius = 4
        key.light?.shadowColor = NSColor(white: 0, alpha: 0.45)
        key.eulerAngles = SCNVector3(-1.0, 0.6, 0)
        scene.rootNode.addChildNode(key)

        let rim = SCNNode()
        rim.light = SCNLight()
        rim.light?.type = .directional
        rim.light?.intensity = 400
        rim.light?.color = NSColor(red: 0.80, green: 0.86, blue: 1.0, alpha: 1)
        rim.eulerAngles = SCNVector3(-0.4, -2.4, 0)
        scene.rootNode.addChildNode(rim)

        scene.rootNode.addChildNode(context.coordinator.boardNode)
        return view
    }

    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        if c.lastRevision != revision || c.lastComponents != includeComponents {
            c.lastRevision = revision
            c.lastComponents = includeComponents
            rebuild(c)
            if c.lastReset == 0 { placeCamera(c, view: view) }
        }
        if c.lastReset != resetToken {
            c.lastReset = resetToken
            placeCamera(c, view: view)
        }
    }

    private func rebuild(_ c: Coordinator) {
        c.boardNode.childNodes.forEach { $0.removeFromParentNode() }
        guard let mesh = engine.buildMesh(includeComponents: includeComponents) else { return }
        let geometry = Self.geometry(from: mesh)
        let node = SCNNode(geometry: geometry)
        // Centre the board on the origin (mesh is in board millimetres).
        node.position = SCNVector3(-board.width / 2, 0, -board.height / 2)
        c.boardNode.addChildNode(node)
        let triangles = mesh.indices.count / 3
        DispatchQueue.main.async {
            stats = "\(mesh.vertexCount) vertices · \(triangles) triangles"
        }
    }

    private func placeCamera(_ c: Coordinator, view: SCNView) {
        let span = max(board.width, board.height)
        c.cameraNode.position = SCNVector3(0, span * 1.1, span * 1.25)
        c.cameraNode.look(at: SCNVector3(0, 0, 0))
        view.pointOfView = c.cameraNode
        // Reset also re-centres the orbit (a pan moves the controller's target away from the board).
        view.defaultCameraController.pointOfView = c.cameraNode
        view.defaultCameraController.target = SCNVector3(0, 0, 0)
    }

    static func geometry(from mesh: MeshData) -> SCNGeometry {
        let vertexCount = mesh.vertexCount
        let positions = mesh.positions.withUnsafeBufferPointer { Data(buffer: $0) }
        let normals = mesh.normals.withUnsafeBufferPointer { Data(buffer: $0) }
        let colors = mesh.colors.withUnsafeBufferPointer { Data(buffer: $0) }
        let indices = mesh.indices.withUnsafeBufferPointer { Data(buffer: $0) }
        let floatSize = MemoryLayout<Float>.size

        let vertexSource = SCNGeometrySource(data: positions, semantic: .vertex, vectorCount: vertexCount,
                                             usesFloatComponents: true, componentsPerVector: 3, bytesPerComponent: floatSize,
                                             dataOffset: 0, dataStride: floatSize * 3)
        let normalSource = SCNGeometrySource(data: normals, semantic: .normal, vectorCount: vertexCount,
                                             usesFloatComponents: true, componentsPerVector: 3, bytesPerComponent: floatSize,
                                             dataOffset: 0, dataStride: floatSize * 3)
        let colorSource = SCNGeometrySource(data: colors, semantic: .color, vectorCount: vertexCount,
                                            usesFloatComponents: true, componentsPerVector: 4, bytesPerComponent: floatSize,
                                            dataOffset: 0, dataStride: floatSize * 4)
        let element = SCNGeometryElement(data: indices, primitiveType: .triangles,
                                         primitiveCount: mesh.indices.count / 3, bytesPerIndex: MemoryLayout<UInt32>.size)
        let geometry = SCNGeometry(sources: [vertexSource, normalSource, colorSource], elements: [element])
        let material = SCNMaterial()
        material.lightingModel = .physicallyBased
        material.diffuse.contents = NSColor.white
        material.metalness.contents = 0.15
        material.roughness.contents = 0.45
        material.isDoubleSided = false
        geometry.materials = [material]
        return geometry
    }
}
