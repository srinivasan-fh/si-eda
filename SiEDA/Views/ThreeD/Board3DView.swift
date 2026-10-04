import SceneKit
import SwiftUI

/// 3D workspace with two modes:
/// - **Assembly** (default): the C++ core tessellates board, copper, vias, silkscreen and component bodies, tagging
///   each surface so it renders with its own physical material (glossy mask, metal finish, solder, plastic…) under
///   studio lighting, in the board's solder mask colour (green unless another is chosen).
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
    @AppStorage("threeD.finish") private var finishRaw = BoardFinish.enig.rawValue
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
                    Picker("Finish", selection: $finishRaw) {
                        ForEach(BoardFinish.allCases) { Text($0.title).tag($0.rawValue) }
                    }
                    .fixedSize()
                    .help("Surface finish of the exposed copper: pads, via lands and plated holes")
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
                    if xray.hud {
                        HoloHUDOverlay(title: store.snapshot.name, readouts: hudReadouts)
                    }
                    layerLegend
                        .padding(12)
                } else {
                    BoardSceneView(engine: store.engine, revision: store.revision, includeComponents: showComponents,
                                   board: store.snapshot.board, resetToken: resetCamera,
                                   finish: BoardFinish(rawValue: finishRaw) ?? .enig, stats: $stats)
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Theme.navy)
            .overlay(alignment: .bottomTrailing) {
                if !store.snapshot.pads.isEmpty { navigationHint.padding(12) }
            }
        }
    }

    /// Live read-outs for the holographic HUD.
    private var hudReadouts: [(String, String)] {
        let b = store.snapshot.board
        let parts = store.snapshot.components.filter { !$0.componentKind.isVirtual }.count
        return [("Board", String(format: "%.1f × %.1f mm", b.width, b.height)),
                ("Stack", "\(b.layerCount) layers · \(b.material.uppercased())"),
                ("Parts", "\(parts)"),
                ("Nets", "\(store.snapshot.nets.filter { $0.pinCount > 1 }.count)"),
                ("Tracks", "\(store.snapshot.tracks.count)"),
                ("Vias", "\(store.snapshot.vias.count)")]
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
            Toggle("HUD", isOn: $xray.hud).toggleStyle(.switch).controlSize(.mini)
                .help("Cinematic heads-up display: projector, particles, part call-outs, data pillars and read-outs")
            Toggle("Panels", isOn: $xray.panels).toggleStyle(.switch).controlSize(.mini)
                .help("Holographic data panels around the stack: copper per layer, net fan-out, parts, routing and a board map")
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

/// Surface finish of the exposed copper (pads, via lands, plated rings), as ordered from the fab.
enum BoardFinish: String, CaseIterable, Identifiable {
    case enig, hasl, osp
    var id: String { rawValue }
    var title: String {
        switch self {
        case .enig: return "ENIG (gold)"
        case .hasl: return "HASL (tin)"
        case .osp: return "OSP (bare copper)"
        }
    }
    var colour: NSColor {
        switch self {
        case .enig: return NSColor(red: 0.96, green: 0.78, blue: 0.42, alpha: 1)
        case .hasl: return NSColor(red: 0.83, green: 0.84, blue: 0.86, alpha: 1)
        case .osp: return NSColor(red: 0.93, green: 0.56, blue: 0.36, alpha: 1)
        }
    }
    var roughness: CGFloat { self == .hasl ? 0.32 : 0.22 }
}

/// Realistic assembly view: each surface (mask, laminate, copper finish, solder, tin and gold, silkscreen, plastic,
/// ceramic, glass…) gets its own physically based material, lit by a studio environment (soft boxes reflected in
/// the metals) and a key light casting soft shadows onto an invisible floor, with ambient occlusion in the gaps.
struct BoardSceneView: NSViewRepresentable {
    let engine: EDAEngine
    let revision: Int
    let includeComponents: Bool
    let board: BoardInfo
    let resetToken: Int
    var finish: BoardFinish = .enig
    @Binding var stats: String

    final class Coordinator {
        var lastRevision = -1
        var lastComponents = true
        var lastFinish: BoardFinish?
        var lastReset = 0
        let boardNode = SCNNode()
        let cameraNode = SCNNode()
        let floorNode = SCNNode()
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

        // Dark studio backdrop; the reflections come from a bright studio environment instead.
        scene.background.contents = Self.backdrop
        scene.lightingEnvironment.contents = Self.studioEnvironment
        scene.lightingEnvironment.intensity = 1.25

        let camera = SCNCamera()
        camera.zNear = 0.1
        camera.zFar = 2000
        camera.fieldOfView = 35
        camera.wantsHDR = true
        camera.wantsExposureAdaptation = false
        camera.exposureOffset = 0.15
        camera.screenSpaceAmbientOcclusionIntensity = 0.9
        camera.screenSpaceAmbientOcclusionRadius = 1.2   // millimetres: the gaps under and between parts
        camera.screenSpaceAmbientOcclusionBias = 0.03
        camera.bloomIntensity = 0.25                     // a little glint on solder and gold
        camera.bloomThreshold = 0.95
        camera.bloomBlurRadius = 6
        camera.vignettingIntensity = 0.35
        camera.vignettingPower = 0.6
        context.coordinator.cameraNode.camera = camera
        scene.rootNode.addChildNode(context.coordinator.cameraNode)
        view.pointOfView = context.coordinator.cameraNode

        let ambient = SCNNode()
        ambient.light = SCNLight()
        ambient.light?.type = .ambient
        ambient.light?.intensity = 120
        ambient.light?.color = NSColor(red: 0.95, green: 0.96, blue: 1.0, alpha: 1)  // near-neutral: true mask colours
        scene.rootNode.addChildNode(ambient)

        // Key light: soft shadows of the parts on the board and of the board on the floor.
        let key = SCNNode()
        key.light = SCNLight()
        key.light?.type = .directional
        key.light?.intensity = 1100
        key.light?.color = NSColor(red: 1.0, green: 0.97, blue: 0.92, alpha: 1)
        key.light?.castsShadow = true
        key.light?.shadowMode = .deferred
        key.light?.shadowSampleCount = 16
        key.light?.shadowRadius = 6
        key.light?.shadowMapSize = CGSize(width: 4096, height: 4096)
        key.light?.automaticallyAdjustsShadowProjection = true
        key.light?.shadowColor = NSColor(white: 0, alpha: 0.55)
        key.eulerAngles = SCNVector3(-1.05, 0.55, 0)
        scene.rootNode.addChildNode(key)

        let rim = SCNNode()
        rim.light = SCNLight()
        rim.light?.type = .directional
        rim.light?.intensity = 450
        rim.light?.color = NSColor(red: 0.80, green: 0.86, blue: 1.0, alpha: 1)
        rim.eulerAngles = SCNVector3(-0.4, -2.4, 0)
        scene.rootNode.addChildNode(rim)

        // Invisible floor that only receives the board's shadow.
        let floor = SCNFloor()
        floor.reflectivity = 0
        let shadowOnly = SCNMaterial()
        shadowOnly.lightingModel = .shadowOnly
        floor.materials = [shadowOnly]
        context.coordinator.floorNode.geometry = floor
        scene.rootNode.addChildNode(context.coordinator.floorNode)

        scene.rootNode.addChildNode(context.coordinator.boardNode)
        return view
    }

    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        if c.lastRevision != revision || c.lastComponents != includeComponents || c.lastFinish != finish {
            c.lastRevision = revision
            c.lastComponents = includeComponents
            c.lastFinish = finish
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
        // Below the board and anything hanging under it (bottom-side parts, header tails).
        c.floorNode.position = SCNVector3(0, -CGFloat(board.thickness) - 9, 0)
        guard let mesh = engine.buildMesh(includeComponents: includeComponents) else { return }
        let node = Self.assemblyNode(from: mesh, finish: finish)
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

    // MARK: - Materials

    /// One child geometry per surface, all sharing the mesh's vertex buffers. O(triangles) to split.
    static func assemblyNode(from mesh: MeshData, finish: BoardFinish) -> SCNNode {
        let root = SCNNode()
        guard mesh.surfaces.count == mesh.vertexCount, mesh.vertexCount > 0 else {
            root.addChildNode(SCNNode(geometry: geometry(from: mesh)))  // untagged mesh: one material
            return root
        }
        var buckets = [[UInt32]](repeating: [], count: MeshSurface.allCases.count)
        var t = 0
        while t + 2 < mesh.indices.count {
            let surface = Int(mesh.surfaces[Int(mesh.indices[t])])
            if surface < buckets.count { buckets[surface].append(contentsOf: mesh.indices[t...(t + 2)]) }
            t += 3
        }
        let (vertices, normals, colours) = sources(from: mesh)
        for surface in MeshSurface.allCases where !buckets[Int(surface.rawValue)].isEmpty {
            let indices = buckets[Int(surface.rawValue)]
            let element = SCNGeometryElement(data: indices.withUnsafeBufferPointer { Data(buffer: $0) },
                                             primitiveType: .triangles, primitiveCount: indices.count / 3,
                                             bytesPerIndex: MemoryLayout<UInt32>.size)
            // The finish takes its colour from the chosen plating, not from the mesh.
            let sources = surface == .finish ? [vertices, normals] : [vertices, normals, colours]
            let geometry = SCNGeometry(sources: sources, elements: [element])
            geometry.materials = [material(for: surface, finish: finish)]
            let node = SCNNode(geometry: geometry)
            node.name = "surface.\(surface)"
            if surface == .glass { node.renderingOrder = 10 }  // after the opaque board
            root.addChildNode(node)
        }
        return root
    }

    /// Physically based look of each surface (base colours come from the mesh except for the finish).
    static func material(for surface: MeshSurface, finish: BoardFinish) -> SCNMaterial {
        let m = SCNMaterial()
        m.lightingModel = .physicallyBased
        m.diffuse.contents = NSColor.white
        m.isDoubleSided = false
        func pbr(metal: CGFloat, rough: CGFloat) {
            m.metalness.contents = metal
            m.roughness.contents = rough
        }
        switch surface {
        case .mask:
            pbr(metal: 0, rough: 0.38)
            m.clearCoat.contents = 0.55          // the lacquer's gloss over the colour
            m.clearCoatRoughness.contents = 0.18
        case .laminate: pbr(metal: 0, rough: 0.78)
        case .finish:
            m.diffuse.contents = finish.colour
            pbr(metal: 1, rough: finish.roughness)
        case .gold: pbr(metal: 1, rough: 0.2)
        case .tin: pbr(metal: 1, rough: 0.3)
        case .solder: pbr(metal: 1, rough: 0.16)
        case .silk: pbr(metal: 0, rough: 0.88)
        case .plastic: pbr(metal: 0, rough: 0.6)
        case .ceramic: pbr(metal: 0, rough: 0.5)
        case .glass:
            pbr(metal: 0, rough: 0.06)
            m.transparency = 0.82
            m.blendMode = .alpha
            m.isDoubleSided = true
        case .hole: pbr(metal: 0, rough: 0.95)
        case .marking: pbr(metal: 0, rough: 0.8)
        }
        return m
    }

    private static func sources(from mesh: MeshData) -> (SCNGeometrySource, SCNGeometrySource, SCNGeometrySource) {
        let vertexCount = mesh.vertexCount
        let floatSize = MemoryLayout<Float>.size
        let vertices = SCNGeometrySource(data: mesh.positions.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .vertex,
                                         vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 3,
                                         bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 3)
        let normals = SCNGeometrySource(data: mesh.normals.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .normal,
                                        vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 3,
                                        bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 3)
        let colours = SCNGeometrySource(data: mesh.colors.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .color,
                                        vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 4,
                                        bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 4)
        return (vertices, normals, colours)
    }

    // MARK: - Studio

    /// Equirectangular studio: a dim floor, a bright overhead and two soft boxes that metals and the glossy mask
    /// reflect. Drawn once (1024 × 512, 2 MB).
    static let studioEnvironment: NSImage = HoloFX.render(NSSize(width: 1024, height: 512)) {
        let full = NSRect(x: 0, y: 0, width: 1024, height: 512)
        NSGradient(colorsAndLocations:
                    (NSColor(white: 0.06, alpha: 1), 0.0),
                   (NSColor(red: 0.20, green: 0.22, blue: 0.26, alpha: 1), 0.48),
                   (NSColor(red: 0.55, green: 0.58, blue: 0.64, alpha: 1), 0.56),
                   (NSColor(white: 0.85, alpha: 1), 1.0))?.draw(in: full, angle: 90)
        func softbox(_ rect: NSRect, _ brightness: CGFloat) {
            for k in 0..<6 {  // feathered edge
                let inset = CGFloat(5 - k) * 6
                NSColor(white: 1, alpha: brightness * CGFloat(k + 1) / 6).setFill()
                NSBezierPath(roundedRect: rect.insetBy(dx: -inset, dy: -inset), xRadius: 18, yRadius: 18).fill()
            }
        }
        softbox(NSRect(x: 140, y: 380, width: 220, height: 70), 0.95)  // key side
        softbox(NSRect(x: 620, y: 360, width: 160, height: 90), 0.7)   // fill side
        softbox(NSRect(x: 420, y: 470, width: 200, height: 30), 0.9)   // overhead strip
    }

    /// Vertical navy gradient behind the board.
    static let backdrop: NSImage = HoloFX.render(NSSize(width: 4, height: 256)) {
        NSGradient(colors: [NSColor(red: 0.01, green: 0.02, blue: 0.05, alpha: 1),
                            NSColor(red: 0.05, green: 0.09, blue: 0.17, alpha: 1)])?
            .draw(in: NSRect(x: 0, y: 0, width: 4, height: 256), angle: 90)
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
