import SceneKit
import SwiftUI

/// 3D assembly viewer: the C++ core tessellates board, copper, vias, silkscreen and component bodies;
/// SceneKit renders them with orbit/zoom camera control.
struct Board3DWorkspace: View {
    @EnvironmentObject private var store: DesignStore
    @State private var showComponents = true
    @State private var resetCamera = 0
    @State private var stats = ""

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Image(systemName: "cube.transparent").foregroundStyle(Theme.blue)
                Text("3D Assembly").fontWeight(.semibold).foregroundStyle(Theme.textPrimary)
                Toggle("Components", isOn: $showComponents).toggleStyle(.switch).controlSize(.mini)
                Text(stats).foregroundStyle(Theme.textMuted).font(.caption)
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

            ZStack {
                if store.snapshot.pads.isEmpty {
                    BlueEmptyState(systemImage: "cube.transparent", title: "Nothing to show yet",
                                   message: "Place footprints in the PCB Layout workspace to build the 3D assembly.",
                                   actionTitle: "Auto-Place Footprints") { store.autoPlace(all: true) }
                } else {
                    BoardSceneView(engine: store.engine, revision: store.revision, includeComponents: showComponents,
                                   board: store.snapshot.board, resetToken: resetCamera, stats: $stats)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Theme.navy)
        }
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
        ambient.light?.color = NSColor(red: 0.75, green: 0.85, blue: 1.0, alpha: 1)
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
        rim.light?.color = NSColor(red: 0.45, green: 0.68, blue: 1.0, alpha: 1)
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
