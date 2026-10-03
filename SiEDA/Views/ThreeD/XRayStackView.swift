import AppKit
import SceneKit
import SwiftUI

/// Options for the holographic layer-stack view.
struct XRaySettings: Equatable {
    var explode: Double = 6        // mm between copper layers
    var showComponents = true
    var showVias = true
    var scan = true
    var spin = false
    var hiddenLayers: Set<Int> = []

    /// The settings that change the scene's geometry (spinning only animates the existing scene).
    var geometryKey: XRaySettings {
        var key = self
        key.spin = false
        return key
    }
}

/// "Iron Man" X-ray view: each copper layer is rendered as a translucent, additively blended hologram plane,
/// separated vertically so the stack-up can be inspected; through vias become light pillars joining the
/// layers, component bodies are wireframes, and a scan beam sweeps through the stack. HDR bloom makes the
/// copper glow.
struct XRayStackView: NSViewRepresentable {
    let engine: EDAEngine
    let snapshot: DesignSnapshot
    let revision: Int
    let settings: XRaySettings
    let resetToken: Int

    final class Coordinator {
        var builtRevision = -1
        var builtSettings: XRaySettings?
        var lastReset = -1
        let root = SCNNode()     // rotates when "spin" is on
        let stack = SCNNode()    // rebuilt content
        let scanBeam = SCNNode()
        let cameraNode = SCNNode()
        var layerMeshes: [Int: MeshData] = [:]
        var meshRevision = -1
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        let scene = SCNScene()
        view.scene = scene
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = false
        view.antialiasingMode = .multisampling4X
        view.backgroundColor = NSColor(red: 0.01, green: 0.03, blue: 0.08, alpha: 1)
        view.defaultCameraController.interactionMode = .orbitTurntable
        view.defaultCameraController.inertiaEnabled = true
        scene.background.contents = Self.backgroundGradient()

        let camera = SCNCamera()
        camera.zNear = 0.1
        camera.zFar = 4000
        camera.fieldOfView = 38
        camera.wantsHDR = true
        camera.bloomIntensity = 1.6
        camera.bloomThreshold = 0.25
        camera.bloomBlurRadius = 14
        camera.vignettingIntensity = 0.7
        camera.vignettingPower = 0.9
        camera.wantsExposureAdaptation = false
        let c = context.coordinator
        c.cameraNode.camera = camera
        scene.rootNode.addChildNode(c.cameraNode)
        view.pointOfView = c.cameraNode

        scene.rootNode.addChildNode(c.root)
        c.root.addChildNode(c.stack)
        c.root.addChildNode(c.scanBeam)
        return view
    }

    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        if c.meshRevision != revision {
            c.layerMeshes = [:]
            for layer in 0..<max(1, snapshot.board.layerCount) {
                if let mesh = engine.buildLayerMesh(layer: layer) { c.layerMeshes[layer] = mesh }
            }
            c.meshRevision = revision
        }
        if c.builtRevision != revision || c.builtSettings != settings.geometryKey {
            let firstBuild = c.builtRevision < 0
            build(c)
            c.builtRevision = revision
            c.builtSettings = settings.geometryKey
            if firstBuild { placeCamera(c, view: view) }
        }
        if c.lastReset != resetToken {
            if c.lastReset >= 0 { placeCamera(c, view: view) }
            c.lastReset = resetToken
        }
        if settings.spin {
            if c.root.action(forKey: "spin") == nil {
                c.root.runAction(.repeatForever(.rotateBy(x: 0, y: .pi * 2, z: 0, duration: 40)), forKey: "spin")
            }
        } else {
            c.root.removeAction(forKey: "spin")
        }
    }

    // MARK: - Scene construction

    private var layerCount: Int { max(1, snapshot.board.layerCount) }
    private var spacing: CGFloat { CGFloat(settings.explode) }
    /// Height of copper layer `layer` (top layer highest).
    private func height(of layer: Int) -> CGFloat { CGFloat(layerCount - 1 - layer) * spacing }
    private var stackHeight: CGFloat { CGFloat(layerCount - 1) * spacing }

    private func build(_ c: Coordinator) {
        c.stack.childNodes.forEach { $0.removeFromParentNode() }
        let w = CGFloat(snapshot.board.width), h = CGFloat(snapshot.board.height)
        let origin = SCNVector3(-w / 2, 0, -h / 2)  // core meshes use board millimetres with the origin at a corner
        // Custom outline in the sheet's local XY plane (rotated −90° about X onto the board: local y = −z).
        var customShape: NSBezierPath?
        if snapshot.board.hasCustomOutline {
            let path = NSBezierPath()
            for (i, p) in snapshot.board.outline.enumerated() {
                let q = NSPoint(x: p.point.x - w / 2, y: -(p.point.y - h / 2))
                if i == 0 { path.move(to: q) } else { path.line(to: q) }
            }
            path.close()
            customShape = path
        }

        for layer in 0..<layerCount where !settings.hiddenLayers.contains(layer) {
            let colour = NSColor(Theme.copperColor(layer, layerCount: layerCount))
            let plane = SCNNode()
            plane.position = SCNVector3(0, height(of: layer), 0)

            // Dielectric sheet with a holographic grid, cut to the board shape (rounded, circle, quad-X…).
            let sheet: SCNGeometry
            if let shape = customShape {
                let geometry = SCNShape(path: shape, extrusionDepth: 0)
                sheet = geometry
            } else {
                sheet = SCNPlane(width: w, height: h)
            }
            sheet.materials = [Self.gridMaterial(colour: colour, width: w, height: h)]
            let sheetNode = SCNNode(geometry: sheet)
            sheetNode.eulerAngles.x = -.pi / 2
            sheetNode.position.y = -0.02
            plane.addChildNode(sheetNode)

            // Glowing board outline.
            if snapshot.board.hasCustomOutline {
                plane.addChildNode(Self.polygon(snapshot.board.outline.map(\.point), width: w, height: h,
                                                colour: colour.withAlphaComponent(0.9)))
            } else {
                plane.addChildNode(Self.outline(width: w, height: h, colour: colour.withAlphaComponent(0.9)))
            }

            // Copper of this layer: solid glow + bright wire edges.
            if let mesh = c.layerMeshes[layer] {
                let geometry = BoardSceneView.geometry(from: mesh)
                geometry.materials = [Self.hologram(colour, alpha: 0.9)]
                let copper = SCNNode(geometry: geometry)
                copper.position = origin
                plane.addChildNode(copper)

                if let edgeGeometry = geometry.copy() as? SCNGeometry {
                    let edges = Self.hologram(NSColor(Theme.iceBlue), alpha: 0.35)
                    edges.fillMode = .lines
                    edgeGeometry.materials = [edges]
                    let edgeNode = SCNNode(geometry: edgeGeometry)
                    edgeNode.position = SCNVector3(origin.x, origin.y + 0.01, origin.z)
                    plane.addChildNode(edgeNode)
                }
            }

            // Floating label ("L1 · TOP").
            let label = SCNText(string: "L\(layer + 1) · \(snapshot.board.layerName(layer).uppercased())", extrusionDepth: 0)
            label.font = NSFont.monospacedSystemFont(ofSize: 10, weight: .bold)
            label.flatness = 0.2
            label.materials = [Self.hologram(colour, alpha: 1)]
            let labelNode = SCNNode(geometry: label)
            let scale = max(0.06, min(w, h) / 260)
            labelNode.scale = SCNVector3(scale, scale, scale)
            labelNode.position = SCNVector3(-w / 2, 0.4, -h / 2 - 1.5)
            labelNode.constraints = [SCNBillboardConstraint()]
            plane.addChildNode(labelNode)

            c.stack.addChildNode(plane)
        }

        // Through vias and through-hole pins: light pillars spanning the whole stack.
        if settings.showVias && layerCount > 1 {
            let pillarHeight = max(stackHeight, 0.2)
            let viaColour = NSColor(Theme.iceBlue)
            for via in snapshot.vias {
                // A blind / buried / micro via spans only its own layers.
                let top = height(of: via.fromLayer ?? 0), bottom = height(of: via.toLayer ?? (layerCount - 1))
                c.stack.addChildNode(Self.pillar(x: CGFloat(via.x) - w / 2, z: CGFloat(via.y) - h / 2,
                                                 radius: CGFloat(via.diameter) / 2,
                                                 height: via.isThrough ? pillarHeight : max(top - bottom, 0.2),
                                                 colour: via.isThrough ? viaColour : NSColor(Theme.lightBlue),
                                                 base: via.isThrough ? 0 : bottom))
            }
            for pad in snapshot.pads where pad.throughHole {
                c.stack.addChildNode(Self.pillar(x: CGFloat(pad.x) - w / 2, z: CGFloat(pad.y) - h / 2,
                                                 radius: CGFloat(max(pad.drill, 0.3)) / 2, height: pillarHeight,
                                                 colour: NSColor(Theme.skyBlue)))
            }
        }

        // Wireframe component bodies above the top layer / below the bottom layer.
        if settings.showComponents {
            for body in snapshot.bodies {
                let box = SCNBox(width: CGFloat(body.w), height: CGFloat(body.h), length: CGFloat(body.d), chamferRadius: 0)
                let wire = Self.hologram(NSColor(Theme.lightBlue), alpha: 0.85)
                wire.fillMode = .lines
                let fill = Self.hologram(NSColor(Theme.blue), alpha: 0.10)
                box.materials = [wire]
                let node = SCNNode(geometry: box)
                let y = body.bottom ? -CGFloat(body.h) / 2 - 0.4 : stackHeight + CGFloat(body.h) / 2 + 0.4
                node.position = SCNVector3(CGFloat(body.x) - w / 2, y, CGFloat(body.y) - h / 2)
                let ghost = SCNNode(geometry: SCNBox(width: CGFloat(body.w), height: CGFloat(body.h), length: CGFloat(body.d),
                                                     chamferRadius: 0))
                ghost.geometry?.materials = [fill]
                node.addChildNode(ghost)
                c.stack.addChildNode(node)
            }
        }

        // Faint reference grid floor.
        let floorSize = max(w, h) * 3
        let floor = SCNPlane(width: floorSize, height: floorSize)
        floor.materials = [Self.gridMaterial(colour: NSColor(Theme.darkBlue), width: floorSize, height: floorSize, alpha: 0.25)]
        let floorNode = SCNNode(geometry: floor)
        floorNode.eulerAngles.x = -.pi / 2
        floorNode.position.y = -max(spacing, 3) - 2
        c.stack.addChildNode(floorNode)

        // Scan beam sweeping through the stack.
        c.scanBeam.removeAllActions()
        c.scanBeam.childNodes.forEach { $0.removeFromParentNode() }
        c.scanBeam.geometry = nil
        if settings.scan {
            let beam = SCNPlane(width: w * 1.15, height: h * 1.15)
            beam.materials = [Self.hologram(NSColor(Theme.probe), alpha: 0.22)]
            c.scanBeam.geometry = beam
            c.scanBeam.eulerAngles.x = -.pi / 2
            let bottom = -1.0 as CGFloat, top = stackHeight + 1.5
            c.scanBeam.position = SCNVector3(0, bottom, 0)
            let up = SCNAction.moveBy(x: 0, y: top - bottom, z: 0, duration: 2.6)
            up.timingMode = .easeInEaseOut
            c.scanBeam.runAction(.repeatForever(.sequence([up, up.reversed(), .wait(duration: 0.4)])))
            // A bright edge ring travelling with the beam.
            c.scanBeam.addChildNode(Self.outline(width: w * 1.15, height: h * 1.15, colour: NSColor(Theme.probe), flat: true))
        }
    }

    private func placeCamera(_ c: Coordinator, view: SCNView) {
        let span = CGFloat(max(snapshot.board.width, snapshot.board.height))
        let mid = stackHeight / 2
        c.cameraNode.position = SCNVector3(span * 0.85, mid + span * 0.75, span * 1.15)
        c.cameraNode.look(at: SCNVector3(0, mid, 0))
        view.pointOfView = c.cameraNode
        // Orbit about the middle of the stack, not SceneKit's default origin.
        view.defaultCameraController.pointOfView = c.cameraNode
        view.defaultCameraController.target = SCNVector3(0, mid, 0)
    }

    // MARK: - Materials & helpers

    static func hologram(_ colour: NSColor, alpha: CGFloat) -> SCNMaterial {
        let m = SCNMaterial()
        m.lightingModel = .constant
        m.diffuse.contents = NSColor.black
        m.emission.contents = colour
        m.transparency = alpha
        m.blendMode = .add
        m.isDoubleSided = true
        m.writesToDepthBuffer = false
        return m
    }

    static func gridMaterial(colour: NSColor, width: CGFloat, height: CGFloat, alpha: CGFloat = 0.55) -> SCNMaterial {
        let m = hologram(colour, alpha: alpha)
        m.emission.contents = gridImage(colour: colour)
        // One texture tile per 5 mm.
        m.emission.contentsTransform = SCNMatrix4MakeScale(width / 5, height / 5, 1)
        m.emission.wrapS = .repeat
        m.emission.wrapT = .repeat
        return m
    }

    /// Tileable grid texture: faint fill, thin lines and a brighter major line.
    static func gridImage(colour: NSColor) -> NSImage {
        let size = 128
        let image = NSImage(size: NSSize(width: size, height: size))
        image.lockFocus()
        colour.withAlphaComponent(0.06).setFill()
        NSRect(x: 0, y: 0, width: size, height: size).fill()
        colour.withAlphaComponent(0.35).setFill()
        NSRect(x: 0, y: 0, width: size, height: 2).fill()
        NSRect(x: 0, y: 0, width: 2, height: size).fill()
        colour.withAlphaComponent(0.15).setFill()
        NSRect(x: 0, y: size / 2, width: size, height: 1).fill()
        NSRect(x: size / 2, y: 0, width: 1, height: size).fill()
        image.unlockFocus()
        return image
    }

    static func backgroundGradient() -> NSImage {
        let size = NSSize(width: 4, height: 512)
        let image = NSImage(size: size)
        image.lockFocus()
        let gradient = NSGradient(colors: [NSColor(red: 0.00, green: 0.02, blue: 0.06, alpha: 1),
                                           NSColor(red: 0.02, green: 0.08, blue: 0.20, alpha: 1),
                                           NSColor(red: 0.00, green: 0.02, blue: 0.06, alpha: 1)])
        gradient?.draw(in: NSRect(origin: .zero, size: size), angle: 90)
        image.unlockFocus()
        return image
    }

    /// Closed polygon outline (board millimetres, origin at a corner) as line primitives in the y = 0 plane.
    static func polygon(_ points: [CGPoint], width w: CGFloat, height h: CGFloat, colour: NSColor) -> SCNNode {
        let vertices = points.map { SCNVector3($0.x - w / 2, 0, $0.y - h / 2) }
        let source = SCNGeometrySource(vertices: vertices)
        var indices: [UInt32] = []
        for i in 0..<vertices.count {
            indices.append(UInt32(i))
            indices.append(UInt32((i + 1) % vertices.count))
        }
        let element = SCNGeometryElement(indices: indices, primitiveType: .line)
        let geometry = SCNGeometry(sources: [source], elements: [element])
        geometry.materials = [hologram(colour, alpha: 1)]
        return SCNNode(geometry: geometry)
    }

    /// Rectangle outline as line primitives (y = 0 plane, or the local XY plane when `flat`).
    static func outline(width w: CGFloat, height h: CGFloat, colour: NSColor, flat: Bool = false) -> SCNNode {
        let corners: [SCNVector3] = flat
            ? [SCNVector3(-w / 2, -h / 2, 0), SCNVector3(w / 2, -h / 2, 0), SCNVector3(w / 2, h / 2, 0), SCNVector3(-w / 2, h / 2, 0)]
            : [SCNVector3(-w / 2, 0, -h / 2), SCNVector3(w / 2, 0, -h / 2), SCNVector3(w / 2, 0, h / 2), SCNVector3(-w / 2, 0, h / 2)]
        let source = SCNGeometrySource(vertices: corners)
        let indices: [UInt32] = [0, 1, 1, 2, 2, 3, 3, 0]
        let element = SCNGeometryElement(indices: indices, primitiveType: .line)
        let geometry = SCNGeometry(sources: [source], elements: [element])
        geometry.materials = [hologram(colour, alpha: 1)]
        return SCNNode(geometry: geometry)
    }

    static func pillar(x: CGFloat, z: CGFloat, radius: CGFloat, height: CGFloat, colour: NSColor,
                       base: CGFloat = 0) -> SCNNode {
        let cylinder = SCNCylinder(radius: max(radius, 0.12), height: height)
        cylinder.radialSegmentCount = 14
        cylinder.materials = [hologram(colour, alpha: 0.75)]
        let node = SCNNode(geometry: cylinder)
        node.position = SCNVector3(x, base + height / 2, z)
        // Bright core for the "energy beam" look.
        let core = SCNCylinder(radius: max(radius, 0.12) * 0.35, height: height)
        core.materials = [hologram(NSColor.white, alpha: 0.9)]
        node.addChildNode(SCNNode(geometry: core))
        return node
    }
}
