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

// MARK: - Holographic data panels

extension HoloFX {
    /// Glass data panel texture: frosted gradient, hex grain, scanlines, glowing frame with amber corner brackets, a
    /// header band (title + code) and a footer; `content` draws the panel's chart into the body rectangle.
    static func panelImage(title: String, code: String, accent: NSColor, content: (NSRect) -> Void) -> NSImage {
        let size = NSSize(width: 640, height: 420)
        let image = NSImage(size: size)
        image.lockFocus()
        let frame = NSRect(origin: .zero, size: size).insetBy(dx: 10, dy: 10)
        NSGradient(colors: [NSColor(red: 0.03, green: 0.16, blue: 0.30, alpha: 0.55),
                            NSColor(red: 0.01, green: 0.06, blue: 0.14, alpha: 0.30)])?.draw(in: frame, angle: 90)
        // Hex grain and scanlines.
        if let hex = hexImage(colour: cyan).cgImage(forProposedRect: nil, context: nil, hints: nil) {
            NSGraphicsContext.current?.cgContext.saveGState()
            NSBezierPath(rect: frame).addClip()
            NSGraphicsContext.current?.cgContext.setAlpha(0.18)
            NSGraphicsContext.current?.cgContext.draw(hex, in: CGRect(x: 0, y: 0, width: 64, height: 48), byTiling: true)
            NSGraphicsContext.current?.cgContext.restoreGState()
        }
        cyan.withAlphaComponent(0.05).setFill()
        var y = frame.minY
        while y < frame.maxY {
            NSRect(x: frame.minX, y: y, width: frame.width, height: 1.5).fill()
            y += 5
        }
        // Frame: soft outer glow, crisp border.
        for (width, alpha) in [(10.0, 0.08), (5.0, 0.18), (2.0, 0.85)] as [(CGFloat, CGFloat)] {
            cyan.withAlphaComponent(alpha).setStroke()
            let border = NSBezierPath(rect: frame)
            border.lineWidth = width
            border.stroke()
        }
        // Header band.
        let header = NSRect(x: frame.minX, y: frame.maxY - 58, width: frame.width, height: 58)
        cyan.withAlphaComponent(0.14).setFill()
        header.fill()
        accent.setFill()
        NSRect(x: frame.minX, y: header.minY - 3, width: 150, height: 3).fill()
        cyan.withAlphaComponent(0.45).setFill()
        NSRect(x: frame.minX + 156, y: header.minY - 2, width: frame.width - 156, height: 1).fill()
        (title.uppercased() as NSString).draw(at: NSPoint(x: frame.minX + 22, y: header.minY + 14),
                                              withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 26, weight: .heavy),
                                                               .foregroundColor: NSColor.white])
        let codeAttrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 17, weight: .bold),
                                                         .foregroundColor: accent]
        let codeSize = (code as NSString).size(withAttributes: codeAttrs)
        (code as NSString).draw(at: NSPoint(x: frame.maxX - codeSize.width - 20, y: header.minY + 19), withAttributes: codeAttrs)
        // Left ruler ticks.
        cyan.withAlphaComponent(0.6).setFill()
        for k in 0..<14 {
            let ty = frame.minY + 34 + CGFloat(k) * 22
            NSRect(x: frame.minX + 4, y: ty, width: k % 3 == 0 ? 12 : 6, height: 1.5).fill()
        }
        // Amber corner brackets.
        accent.setStroke()
        let b: CGFloat = 30
        let corners: [(CGFloat, CGFloat, CGFloat, CGFloat)] = [(frame.minX, frame.minY, 1.0, 1.0), (frame.maxX, frame.minY, -1.0, 1.0),
                                (frame.minX, frame.maxY, 1.0, -1.0), (frame.maxX, frame.maxY, -1.0, -1.0)]
        for (x, yy, dx, dy) in corners {
            let path = NSBezierPath()
            path.move(to: NSPoint(x: x + dx * b, y: yy))
            path.line(to: NSPoint(x: x, y: yy))
            path.line(to: NSPoint(x: x, y: yy + dy * b))
            path.lineWidth = 5
            path.stroke()
        }
        // Footer.
        ("SIEDA // HOLO-ANALYSIS" as NSString).draw(at: NSPoint(x: frame.minX + 22, y: frame.minY + 10),
                                                    withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: 12, weight: .medium),
                                                                     .foregroundColor: cyan.withAlphaComponent(0.55)])
        content(NSRect(x: frame.minX + 26, y: frame.minY + 36, width: frame.width - 52, height: header.minY - frame.minY - 50))
        image.unlockFocus()
        return image
    }

    private static func label(_ text: String, at p: NSPoint, size: CGFloat = 15, colour: NSColor = .white,
                              weight: NSFont.Weight = .medium) {
        (text as NSString).draw(at: p, withAttributes: [.font: NSFont.monospacedSystemFont(ofSize: size, weight: weight),
                                                        .foregroundColor: colour])
    }

    /// Glowing bar colour by level: cyan → amber → red.
    private static func levelColour(_ level: CGFloat) -> NSColor {
        level < 0.6 ? cyan.blended(withFraction: level / 0.6, of: amber) ?? cyan
                    : amber.blended(withFraction: (level - 0.6) / 0.4, of: NSColor.systemRed) ?? amber
    }

    private static func glowBar(_ r: NSRect, colour: NSColor) {
        colour.withAlphaComponent(0.18).setFill()
        r.insetBy(dx: -3, dy: -3).fill()
        colour.withAlphaComponent(0.75).setFill()
        r.fill()
        NSColor.white.withAlphaComponent(0.9).setFill()
        (r.width > r.height ? NSRect(x: r.maxX - 3, y: r.minY, width: 3, height: r.height)
                            : NSRect(x: r.minX, y: r.maxY - 3, width: r.width, height: 3)).fill()
    }

    /// Horizontal bars with a label on the left and the value on the right.
    static func drawHBars(_ rows: [(String, Double, String)], in r: NSRect) {
        guard !rows.isEmpty else { return }
        let maxV = max(rows.map { $0.1 }.max() ?? 1, 1e-9)
        let rowH = min(34, r.height / CGFloat(rows.count))
        for (i, row) in rows.enumerated() {
            let y = r.maxY - CGFloat(i + 1) * rowH + 6
            label(row.0, at: NSPoint(x: r.minX, y: y + 2), size: 14, colour: cyan)
            let barX = r.minX + 120, barW = r.width - 230
            cyan.withAlphaComponent(0.12).setFill()
            NSRect(x: barX, y: y, width: barW, height: rowH - 14).fill()
            let level = CGFloat(row.1 / maxV)
            glowBar(NSRect(x: barX, y: y, width: max(2, barW * level), height: rowH - 14), colour: levelColour(level))
            label(row.2, at: NSPoint(x: barX + barW + 12, y: y + 2), size: 14)
        }
    }

    /// Vertical bars (histogram) with labels underneath.
    static func drawVBars(_ bars: [(String, Double)], in r: NSRect) {
        guard !bars.isEmpty else { return }
        let maxV = max(bars.map { $0.1 }.max() ?? 1, 1e-9)
        let slot = r.width / CGFloat(bars.count)
        let base = r.minY + 26, height = r.height - 50
        cyan.withAlphaComponent(0.4).setFill()
        NSRect(x: r.minX, y: base - 2, width: r.width, height: 1).fill()
        for (i, bar) in bars.enumerated() {
            let level = CGFloat(bar.1 / maxV)
            let x = r.minX + CGFloat(i) * slot + slot * 0.2
            glowBar(NSRect(x: x, y: base, width: slot * 0.6, height: max(2, height * level)), colour: levelColour(level))
            label(String(format: "%.0f", bar.1), at: NSPoint(x: x, y: base + max(2, height * level) + 4), size: 13)
            label(bar.0, at: NSPoint(x: x, y: r.minY), size: 13, colour: cyan)
        }
    }

    /// Circular gauge with a big percentage in the middle and key / value rows to its right.
    static func drawGauge(fraction: Double, caption: String, rows: [(String, String)], in r: NSRect) {
        let d = min(r.height, r.width * 0.5) - 10
        let c = NSPoint(x: r.minX + d / 2 + 6, y: r.midY)
        let track = NSBezierPath()
        track.appendArc(withCenter: c, radius: d / 2 - 8, startAngle: 0, endAngle: 360)
        track.lineWidth = 14
        cyan.withAlphaComponent(0.15).setStroke()
        track.stroke()
        let arc = NSBezierPath()
        arc.appendArc(withCenter: c, radius: d / 2 - 8, startAngle: 90, endAngle: 90 - 360 * CGFloat(min(1, max(0, fraction))),
                      clockwise: true)
        arc.lineWidth = 14
        (fraction >= 0.999 ? cyan : amber).setStroke()
        arc.stroke()
        for k in 0..<48 {  // tick ring
            let a = CGFloat(k) / 48 * 2 * .pi, r0 = d / 2 + 2, r1 = d / 2 + (k % 4 == 0 ? 12 : 6)
            let t = NSBezierPath()
            t.move(to: NSPoint(x: c.x + r0 * cos(a), y: c.y + r0 * sin(a)))
            t.line(to: NSPoint(x: c.x + r1 * cos(a), y: c.y + r1 * sin(a)))
            cyan.withAlphaComponent(0.5).setStroke()
            t.lineWidth = 1.5
            t.stroke()
        }
        let pct = String(format: "%.0f%%", fraction * 100) as NSString
        let attrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 40, weight: .heavy),
                                                    .foregroundColor: NSColor.white]
        let sz = pct.size(withAttributes: attrs)
        pct.draw(at: NSPoint(x: c.x - sz.width / 2, y: c.y - sz.height / 2 + 6), withAttributes: attrs)
        let capAttrs: [NSAttributedString.Key: Any] = [.font: NSFont.monospacedSystemFont(ofSize: 13, weight: .bold),
                                                       .foregroundColor: cyan]
        let csz = (caption as NSString).size(withAttributes: capAttrs)
        (caption as NSString).draw(at: NSPoint(x: c.x - csz.width / 2, y: c.y - sz.height / 2 - 14), withAttributes: capAttrs)
        var y = r.maxY - 30
        for (key, value) in rows {
            label(key.uppercased(), at: NSPoint(x: r.minX + d + 40, y: y), size: 14, colour: cyan.withAlphaComponent(0.8))
            label(value, at: NSPoint(x: r.minX + d + 180, y: y), size: 16, weight: .bold)
            y -= 34
        }
    }

    /// Top-down mini-map of the board: outline, tracks and pads scaled into `r`.
    static func drawMiniMap(width: Double, height: Double, tracks: [(CGPoint, CGPoint)], pads: [CGPoint], in r: NSRect) {
        guard width > 0, height > 0 else { return }
        let k = min(r.width / CGFloat(width), r.height / CGFloat(height))
        let ox = r.midX - CGFloat(width) * k / 2, oy = r.midY + CGFloat(height) * k / 2
        func map(_ p: CGPoint) -> NSPoint { NSPoint(x: ox + p.x * k, y: oy - p.y * k) }  // board y runs down
        let outline = NSRect(x: ox, y: oy - CGFloat(height) * k, width: CGFloat(width) * k, height: CGFloat(height) * k)
        cyan.withAlphaComponent(0.08).setFill()
        outline.fill()
        cyan.withAlphaComponent(0.9).setStroke()
        let o = NSBezierPath(rect: outline)
        o.lineWidth = 2
        o.stroke()
        let path = NSBezierPath()
        for (a, b) in tracks {
            path.move(to: map(a))
            path.line(to: map(b))
        }
        path.lineWidth = 1
        cyan.withAlphaComponent(0.55).setStroke()
        path.stroke()
        amber.withAlphaComponent(0.9).setFill()
        for p in pads {
            let q = map(p)
            NSRect(x: q.x - 1, y: q.y - 1, width: 2, height: 2).fill()
        }
    }

    /// A panel in the scene: the glass face, a fainter depth frame behind it, a light post down to the holo-table, an
    /// unfold-in after `delay` and a slow float.
    static func panelNode(image: NSImage, width: CGFloat, delay: TimeInterval, postDepth: CGFloat) -> SCNNode {
        let height = width * image.size.height / image.size.width
        let root = SCNNode()
        let face = SCNNode(geometry: SCNPlane(width: width, height: height))
        face.geometry?.materials = [glow(image, alpha: 0.92)]
        root.addChildNode(face)
        let depth = SCNNode(geometry: SCNPlane(width: width * 1.04, height: height * 1.06))
        depth.geometry?.materials = [glow(ringFrameImage(), alpha: 0.35)]
        depth.position.z = -width * 0.04
        root.addChildNode(depth)
        root.addChildNode(line(from: SCNVector3(0, -height / 2, 0), to: SCNVector3(0, -height / 2 - postDepth, 0),
                               colour: cyan, alpha: 0.45))
        let base = SCNNode(geometry: SCNTorus(ringRadius: width * 0.06, pipeRadius: width * 0.004))
        base.geometry?.materials = [XRayStackView.hologram(cyan, alpha: 0.8)]
        base.position.y = -height / 2 - postDepth
        root.addChildNode(base)
        // Upright, turning to face the camera as it orbits.
        let billboard = SCNBillboardConstraint()
        billboard.freeAxes = .Y
        root.constraints = [billboard]
        // Unfold in, then float.
        root.scale = SCNVector3(0.05, 0.05, 0.05)
        root.opacity = 0
        let grow = SCNAction.scale(to: 1, duration: 0.55)
        grow.timingMode = .easeOut
        let bob = SCNAction.sequence([.moveBy(x: 0, y: height * 0.05, z: 0, duration: 2.4),
                                      .moveBy(x: 0, y: -height * 0.05, z: 0, duration: 2.4)])
        bob.timingMode = .easeInEaseOut
        root.runAction(.sequence([.wait(duration: delay),
                                  .group([grow, .sequence([.fadeOpacity(to: 1, duration: 0.1), .fadeOpacity(to: 0.4, duration: 0.06),
                                                           .fadeOpacity(to: 1, duration: 0.3)])]),
                                  .repeatForever(bob)]))
        return root
    }

    /// Faint double frame for a panel's depth layer.
    static func ringFrameImage() -> NSImage {
        let size = NSSize(width: 320, height: 210)
        let image = NSImage(size: size)
        image.lockFocus()
        for (inset, alpha) in [(4.0, 0.7), (12.0, 0.3)] as [(CGFloat, CGFloat)] {
            cyan.withAlphaComponent(alpha).setStroke()
            let p = NSBezierPath(rect: NSRect(origin: .zero, size: size).insetBy(dx: inset, dy: inset))
            p.lineWidth = 2
            p.stroke()
        }
        image.unlockFocus()
        return image
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
