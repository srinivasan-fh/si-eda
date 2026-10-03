import AppKit
import SceneKit
import SwiftUI

/// Cinematic holographic set pieces for the X-ray stack (the "workshop hologram" look): a hex-mesh holo-table with a
/// projector, rotating ring arrays and a light cone, drifting particle motes, targeting reticles and floating data
/// cards over the key parts, data pillars, and a 2D HUD frame with scanlines and live read-outs.
enum HoloFX {
    /// Amber accent used for call-outs (the cyan hologram's warm counterpoint).
    static let amber = NSColor(red: 1.00, green: 0.62, blue: 0.18, alpha: 1)
    static let cyan = NSColor(red: 0.36, green: 0.86, blue: 1.00, alpha: 1)

    // MARK: Textures

    /// Tileable hexagon mesh (pointy-top cells) with a soft glow on the edges.
    static func hexImage(colour: NSColor) -> NSImage {
        let r: CGFloat = 16                       // cell radius in pixels
        let w = sqrt(3) * r, h = 3 * r            // tile: one cell wide, two rows high
        let size = NSSize(width: w * 2, height: h)
        let image = NSImage(size: size)
        image.lockFocus()
        colour.withAlphaComponent(0.05).setFill()
        NSRect(origin: .zero, size: size).fill()
        func hexagon(cx: CGFloat, cy: CGFloat) -> NSBezierPath {
            let path = NSBezierPath()
            for k in 0..<6 {
                let a = CGFloat(k) * .pi / 3 + .pi / 6
                let p = NSPoint(x: cx + r * 0.92 * cos(a), y: cy + r * 0.92 * sin(a))
                if k == 0 { path.move(to: p) } else { path.line(to: p) }
            }
            path.close()
            return path
        }
        var centres: [NSPoint] = []
        for row in -1...2 {
            let offset = row % 2 == 0 ? 0 : w / 2
            for col in -1...3 { centres.append(NSPoint(x: CGFloat(col) * w + offset, y: CGFloat(row) * 1.5 * r)) }
        }
        for c in centres {
            let path = hexagon(cx: c.x, cy: c.y)
            colour.withAlphaComponent(0.10).setStroke()
            path.lineWidth = 3.5
            path.stroke()
            colour.withAlphaComponent(0.55).setStroke()
            path.lineWidth = 1
            path.stroke()
        }
        image.unlockFocus()
        return image
    }

    /// A ring of tick marks and arc segments, drawn flat (used on rotating projector rings).
    static func ringImage(colour: NSColor, ticks: Int, segments: Int) -> NSImage {
        let size: CGFloat = 512
        let image = NSImage(size: NSSize(width: size, height: size))
        image.lockFocus()
        let c = NSPoint(x: size / 2, y: size / 2)
        colour.withAlphaComponent(0.85).setStroke()
        for k in 0..<ticks {
            let a = CGFloat(k) / CGFloat(ticks) * 2 * .pi
            let long = k % 5 == 0
            let r0 = size * (long ? 0.40 : 0.43), r1 = size * 0.47
            let path = NSBezierPath()
            path.move(to: NSPoint(x: c.x + r0 * cos(a), y: c.y + r0 * sin(a)))
            path.line(to: NSPoint(x: c.x + r1 * cos(a), y: c.y + r1 * sin(a)))
            path.lineWidth = long ? 3 : 1.5
            path.stroke()
        }
        for k in 0..<segments where k % 3 != 2 {
            let start = CGFloat(k) / CGFloat(segments) * 360, end = start + 360 / CGFloat(segments) * 0.8
            let arc = NSBezierPath()
            arc.appendArc(withCenter: c, radius: size * 0.36, startAngle: start, endAngle: end)
            arc.lineWidth = 6
            colour.withAlphaComponent(0.6).setStroke()
            arc.stroke()
        }
        image.unlockFocus()
        return image
    }

    /// Floating data card: a bracketed panel with a title, a value line and key / value rows.
    static func cardImage(title: String, subtitle: String, rows: [(String, String)], accent: NSColor) -> NSImage {
        let size = NSSize(width: 360, height: 120 + CGFloat(rows.count) * 26)
        let image = NSImage(size: size)
        image.lockFocus()
        let frame = NSRect(origin: .zero, size: size).insetBy(dx: 4, dy: 4)
        NSColor(red: 0.02, green: 0.10, blue: 0.20, alpha: 0.55).setFill()
        NSBezierPath(rect: frame).fill()
        cyan.withAlphaComponent(0.35).setStroke()
        let border = NSBezierPath(rect: frame)
        border.lineWidth = 1.5
        border.stroke()
        // Corner brackets.
        accent.setStroke()
        let b: CGFloat = 22
        for (x, y, dx, dy) in [(frame.minX, frame.minY, 1.0, 1.0), (frame.maxX, frame.minY, -1.0, 1.0),
                               (frame.minX, frame.maxY, 1.0, -1.0), (frame.maxX, frame.maxY, -1.0, -1.0)] {
            let p = NSBezierPath()
            p.move(to: NSPoint(x: x + dx * b, y: y))
            p.line(to: NSPoint(x: x, y: y))
            p.line(to: NSPoint(x: x, y: y + dy * b))
            p.lineWidth = 4
            p.stroke()
        }
        // Accent bar under the title.
        accent.withAlphaComponent(0.8).setFill()
        NSRect(x: frame.minX + 18, y: frame.maxY - 62, width: 120, height: 3).fill()
        let titleFont = NSFont.monospacedSystemFont(ofSize: 30, weight: .heavy)
        let bodyFont = NSFont.monospacedSystemFont(ofSize: 17, weight: .medium)
        (title as NSString).draw(at: NSPoint(x: frame.minX + 18, y: frame.maxY - 52),
                                 withAttributes: [.font: titleFont, .foregroundColor: NSColor.white])
        (subtitle as NSString).draw(at: NSPoint(x: frame.minX + 150, y: frame.maxY - 46),
                                    withAttributes: [.font: bodyFont, .foregroundColor: accent])
        var y = frame.maxY - 96
        for (key, value) in rows {
            (key.uppercased() as NSString).draw(at: NSPoint(x: frame.minX + 18, y: y),
                                                withAttributes: [.font: bodyFont, .foregroundColor: cyan.withAlphaComponent(0.75)])
            (value as NSString).draw(at: NSPoint(x: frame.minX + 150, y: y),
                                     withAttributes: [.font: bodyFont, .foregroundColor: NSColor.white])
            y -= 26
        }
        image.unlockFocus()
        return image
    }

    /// A glowing "particle" sprite: a soft round dot.
    static func dotImage() -> NSImage {
        let size: CGFloat = 32
        let image = NSImage(size: NSSize(width: size, height: size))
        image.lockFocus()
        let gradient = NSGradient(colors: [NSColor.white, NSColor.white.withAlphaComponent(0)])
        gradient?.draw(in: NSBezierPath(ovalIn: NSRect(x: 0, y: 0, width: size, height: size)), relativeCenterPosition: .zero)
        image.unlockFocus()
        return image
    }

    // MARK: Scene pieces

    /// Material that shows an image as additive light.
    static func glow(_ contents: Any, alpha: CGFloat) -> SCNMaterial {
        let m = XRayStackView.hologram(.white, alpha: alpha)
        m.emission.contents = contents
        return m
    }

    /// Holo-table projector under the stack: a hex-mesh disc, counter-rotating ring arrays and a faint light cone
    /// rising to the board.
    static func projector(radius: CGFloat, floorY: CGFloat, coneHeight: CGFloat) -> SCNNode {
        let node = SCNNode()
        node.position = SCNVector3(0, floorY, 0)

        let disc = SCNCylinder(radius: radius, height: 0.01)
        let hex = glow(hexImage(colour: cyan), alpha: 0.55)
        hex.emission.contentsTransform = SCNMatrix4MakeScale(radius / 6, radius / 6, 1)
        hex.emission.wrapS = .repeat
        hex.emission.wrapT = .repeat
        disc.materials = [hex, hex, hex]
        node.addChildNode(SCNNode(geometry: disc))

        for (k, scale, speed, colour) in [(0, 1.00, 60.0, cyan), (1, 0.78, -38.0, cyan), (2, 0.52, 24.0, amber)] {
            let plane = SCNPlane(width: radius * 2 * scale, height: radius * 2 * scale)
            plane.materials = [glow(ringImage(colour: colour, ticks: 72 - k * 18, segments: 9 + k * 3), alpha: k == 2 ? 0.7 : 0.9)]
            let ring = SCNNode(geometry: plane)
            ring.eulerAngles.x = -.pi / 2
            ring.position.y = 0.05 + CGFloat(k) * 0.04
            ring.runAction(.repeatForever(.rotateBy(x: 0, y: 0, z: .pi * 2 * (speed > 0 ? 1 : -1), duration: abs(speed))))
            node.addChildNode(ring)
        }

        // Light cone from the projector up through the stack.
        let cone = SCNCone(topRadius: radius * 0.95, bottomRadius: radius * 0.35, height: coneHeight)
        cone.radialSegmentCount = 64
        let beam = XRayStackView.hologram(cyan, alpha: 0.06)
        beam.isDoubleSided = true
        cone.materials = [beam]
        let coneNode = SCNNode(geometry: cone)
        coneNode.position.y = coneHeight / 2
        node.addChildNode(coneNode)
        // Breathing pulse on the beam.
        let pulse = SCNAction.sequence([.fadeOpacity(to: 0.45, duration: 1.6), .fadeOpacity(to: 1.0, duration: 1.6)])
        pulse.timingMode = .easeInEaseOut
        coneNode.runAction(.repeatForever(pulse))
        return node
    }

    /// Slowly rising particle motes filling the hologram volume.
    static func particles(width: CGFloat, depth: CGFloat, height: CGFloat) -> SCNNode {
        let system = SCNParticleSystem()
        system.birthRate = 60
        system.particleLifeSpan = 6
        system.particleLifeSpanVariation = 2
        system.particleSize = max(0.08, min(width, depth) / 400)
        system.particleSizeVariation = system.particleSize * 0.6
        system.particleColor = cyan.withAlphaComponent(0.8)
        system.particleColorVariation = SCNVector4(0.05, 0.1, 0.2, 0.3)
        system.particleImage = dotImage()
        system.blendMode = .additive
        system.isAffectedByGravity = false
        system.emitterShape = SCNBox(width: width, height: max(height, 1), length: depth, chamferRadius: 0)
        system.birthLocation = .volume
        system.particleVelocity = max(0.3, height / 12)
        system.particleVelocityVariation = system.particleVelocity * 0.5
        system.emittingDirection = SCNVector3(0, 1, 0)
        system.spreadingAngle = 25
        let node = SCNNode()
        node.addParticleSystem(system)
        return node
    }

    /// Targeting reticle over a part: a segmented ring that spins and a slower counter-rotating inner ring.
    static func reticle(size: CGFloat, colour: NSColor) -> SCNNode {
        let node = SCNNode()
        for (k, scale, duration) in [(0, 1.0, 8.0), (1, 0.7, -5.0)] {
            let plane = SCNPlane(width: size * scale, height: size * scale)
            plane.materials = [glow(ringImage(colour: colour, ticks: 36 + k * 12, segments: 6 + k * 2), alpha: 0.95)]
            let ring = SCNNode(geometry: plane)
            ring.eulerAngles.x = -.pi / 2
            ring.runAction(.repeatForever(.rotateBy(x: 0, y: 0, z: .pi * 2 * (duration > 0 ? 1 : -1), duration: abs(duration))))
            node.addChildNode(ring)
        }
        let pulse = SCNAction.sequence([.scale(to: 1.08, duration: 0.9), .scale(to: 1.0, duration: 0.9)])
        pulse.timingMode = .easeInEaseOut
        node.runAction(.repeatForever(pulse))
        return node
    }

    /// A straight glowing line between two points.
    static func line(from a: SCNVector3, to b: SCNVector3, colour: NSColor, alpha: CGFloat = 0.9) -> SCNNode {
        let source = SCNGeometrySource(vertices: [a, b])
        let element = SCNGeometryElement(indices: [UInt32(0), UInt32(1)], primitiveType: .line)
        let geometry = SCNGeometry(sources: [source], elements: [element])
        geometry.materials = [XRayStackView.hologram(colour, alpha: alpha)]
        return SCNNode(geometry: geometry)
    }

    /// Data pillar (bar chart in space): a translucent column with a bright cap, coloured cyan → amber → red by `level`.
    static func pillar(height: CGFloat, radius: CGFloat, level: CGFloat) -> SCNNode {
        let colour = level < 0.5 ? cyan.blended(withFraction: level * 2, of: amber) ?? amber
                                 : amber.blended(withFraction: (level - 0.5) * 2, of: NSColor.systemRed) ?? amber
        let box = SCNBox(width: radius * 2, height: height, length: radius * 2, chamferRadius: 0)
        box.materials = [XRayStackView.hologram(colour, alpha: 0.45)]
        let column = SCNNode(geometry: box)
        column.position.y = height / 2
        let cap = SCNBox(width: radius * 2.2, height: radius * 0.4, length: radius * 2.2, chamferRadius: 0)
        cap.materials = [XRayStackView.hologram(NSColor.white, alpha: 0.95)]
        let capNode = SCNNode(geometry: cap)
        capNode.position.y = height / 2
        column.addChildNode(capNode)
        // Base at the node's origin, so it grows up from the board when the hologram materialises.
        let node = SCNNode()
        node.addChildNode(column)
        node.scale = SCNVector3(1, 0.01, 1)
        let grow = SCNAction.scale(to: 1, duration: 1.2)
        grow.timingMode = .easeOut
        node.runAction(.sequence([.wait(duration: 0.8), grow]))
        return node
    }

    /// Materialise: fade a node in after `delay`, with a brief over-bright flicker as it "locks on".
    static func materialise(_ node: SCNNode, delay: TimeInterval) {
        node.opacity = 0
        node.runAction(.sequence([.wait(duration: delay), .fadeOpacity(to: 1.0, duration: 0.08),
                                  .fadeOpacity(to: 0.35, duration: 0.06), .fadeOpacity(to: 1.0, duration: 0.25)]))
    }

    /// Occasional holographic flicker on an otherwise steady node.
    static func flicker(_ node: SCNNode) {
        let blink = SCNAction.sequence([.fadeOpacity(to: 0.82, duration: 0.05), .fadeOpacity(to: 1.0, duration: 0.07),
                                        .fadeOpacity(to: 0.9, duration: 0.04), .fadeOpacity(to: 1.0, duration: 0.05)])
        node.runAction(.repeatForever(.sequence([.wait(duration: 4.5, withRange: 3.0), blink])), forKey: "flicker")
    }
}

/// 2D HUD over the X-ray stack: corner brackets, drifting scanlines, a rotating targeting arc and live read-outs.
struct HoloHUDOverlay: View {
    let title: String
    let readouts: [(String, String)]

    private let cyan = Color(red: 0.36, green: 0.86, blue: 1.00)
    private let amber = Color(red: 1.00, green: 0.62, blue: 0.18)

    var body: some View {
        TimelineView(.animation(minimumInterval: 1 / 30)) { context in
            let t = context.date.timeIntervalSinceReferenceDate
            GeometryReader { geo in
                ZStack(alignment: .topLeading) {
                    // Scanlines with a slow bright band rolling down.
                    Canvas { ctx, size in
                        var y: CGFloat = 0
                        while y < size.height {
                            ctx.fill(Path(CGRect(x: 0, y: y, width: size.width, height: 1)), with: .color(cyan.opacity(0.035)))
                            y += 3
                        }
                        let band = CGFloat((t * 0.12).truncatingRemainder(dividingBy: 1)) * (size.height + 160) - 80
                        ctx.fill(Path(CGRect(x: 0, y: band, width: size.width, height: 80)),
                                 with: .linearGradient(Gradient(colors: [.clear, cyan.opacity(0.06), .clear]),
                                                       startPoint: CGPoint(x: 0, y: band), endPoint: CGPoint(x: 0, y: band + 80)))
                    }
                    brackets(size: geo.size)
                    // Header.
                    VStack(alignment: .leading, spacing: 4) {
                        HStack(spacing: 6) {
                            Circle().fill(amber).frame(width: 6, height: 6)
                                .opacity(0.5 + 0.5 * sin(t * 3))
                            Text("HOLOGRAPHIC LAYER ANALYSIS").font(.system(size: 10, weight: .heavy, design: .monospaced))
                                .foregroundStyle(cyan)
                        }
                        Text(title.uppercased()).font(.system(size: 15, weight: .bold, design: .monospaced))
                            .foregroundStyle(.white)
                            .lineLimit(1)
                        Rectangle().fill(amber).frame(width: 90, height: 2)
                    }
                    .padding(.leading, 34)
                    .padding(.top, 26)
                    // Read-outs.
                    VStack(alignment: .trailing, spacing: 3) {
                        ForEach(readouts.indices, id: \.self) { i in
                            HStack(spacing: 8) {
                                Text(readouts[i].0.uppercased()).foregroundStyle(cyan.opacity(0.7))
                                Text(readouts[i].1).foregroundStyle(.white)
                            }
                        }
                    }
                    .font(.system(size: 10, weight: .medium, design: .monospaced))
                    .padding(.trailing, 34)
                    .padding(.top, 26)
                    .frame(maxWidth: .infinity, alignment: .trailing)
                    // Rotating targeting arc, lower right.
                    reticle(t: t)
                        .frame(width: 84, height: 84)
                        .position(x: geo.size.width - 80, y: geo.size.height - 150)
                }
            }
        }
        .allowsHitTesting(false)
        .accessibilityHidden(true)
    }

    private func brackets(size: CGSize) -> some View {
        Canvas { ctx, _ in
            let inset: CGFloat = 14, len: CGFloat = 36
            var path = Path()
            for (x, y, dx, dy) in [(inset, inset, 1.0, 1.0), (size.width - inset, inset, -1.0, 1.0),
                                   (inset, size.height - inset, 1.0, -1.0), (size.width - inset, size.height - inset, -1.0, -1.0)] {
                path.move(to: CGPoint(x: x + dx * len, y: y))
                path.addLine(to: CGPoint(x: x, y: y))
                path.addLine(to: CGPoint(x: x, y: y + dy * len))
            }
            ctx.stroke(path, with: .color(cyan.opacity(0.8)), lineWidth: 2)
            // Thin frame and centre ticks.
            ctx.stroke(Path(CGRect(x: inset + 6, y: inset + 6, width: size.width - 2 * inset - 12,
                                   height: size.height - 2 * inset - 12)),
                       with: .color(cyan.opacity(0.12)), lineWidth: 1)
            var ticks = Path()
            ticks.move(to: CGPoint(x: size.width / 2 - 40, y: inset))
            ticks.addLine(to: CGPoint(x: size.width / 2 + 40, y: inset))
            ticks.move(to: CGPoint(x: size.width / 2 - 40, y: size.height - inset))
            ticks.addLine(to: CGPoint(x: size.width / 2 + 40, y: size.height - inset))
            ctx.stroke(ticks, with: .color(amber.opacity(0.8)), lineWidth: 2)
        }
    }

    private func reticle(t: Double) -> some View {
        ZStack {
            Circle().trim(from: 0, to: 0.7).stroke(cyan.opacity(0.8), style: StrokeStyle(lineWidth: 2, dash: [6, 4]))
                .rotationEffect(.degrees(t * 40))
            Circle().trim(from: 0, to: 0.35).stroke(amber, lineWidth: 3)
                .padding(12)
                .rotationEffect(.degrees(-t * 70))
            Circle().stroke(cyan.opacity(0.3), lineWidth: 1).padding(24)
            Text(String(format: "%03.0f", (t * 37).truncatingRemainder(dividingBy: 360)))
                .font(.system(size: 9, weight: .bold, design: .monospaced))
                .foregroundStyle(cyan)
        }
    }
}
