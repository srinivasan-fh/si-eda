import SwiftUI

/// The part being placed after Update PCB, drawn at the cursor: its courtyard and pads (green where it may go, red
/// where it may not, amber with a warning) and ratsnest lines to the nearest pad of each of its nets.
struct PlacementGhostLayer: View {
    let ghost: PlacementCheckInfo
    let viewport: Viewport
    let pads: [SnapPad]

    var body: some View {
        Canvas(rendersAsynchronously: false) { ctx, _ in draw(&ctx) }
            .allowsHitTesting(false)
            .accessibilityHidden(true)
    }

    private var colour: Color {
        if !ghost.legal { return Theme.error }
        return ghost.issues.isEmpty ? Theme.liveOn : Theme.warning
    }

    private func screenRect(_ r: CGRect) -> CGRect {
        let a = viewport.toScreen(r.origin)
        return CGRect(x: a.x, y: a.y, width: r.width * viewport.scale, height: r.height * viewport.scale)
    }

    private func draw(_ ctx: inout GraphicsContext) {
        let tint = colour
        let box = screenRect(ghost.courtyard.rect)
        ctx.fill(Path(box), with: .color(tint.opacity(0.12)))
        ctx.stroke(Path(box), with: .color(tint), style: StrokeStyle(lineWidth: 1.5, dash: [5, 3]))
        for pad in ghost.pads {
            let r = screenRect(pad.rect)
            let shape = pad.round ? Path(ellipseIn: r) : Path(r)
            ctx.fill(shape, with: .color(tint.opacity(0.55)))
        }
        drawRatsnest(&ctx)
        if let message = ghost.firstMessage {
            let label = Text(verbatim: message).font(.caption).foregroundColor(tint)
            ctx.draw(label, at: CGPoint(x: box.midX, y: box.maxY + 10), anchor: .top)
        }
    }

    /// One line per ghost pad on a net, to the nearest pad of another part on that net.
    private func drawRatsnest(_ ctx: inout GraphicsContext) {
        var lines = Path()
        for pad in ghost.pads where pad.net >= 0 {
            let from = CGPoint(x: pad.x, y: pad.y)
            let others = pads.filter { $0.net == pad.net && $0.component != ghost.component }
            let nearest = others.min { distance($0, from) < distance($1, from) }
            guard let nearest else { continue }
            lines.move(to: viewport.toScreen(from))
            lines.addLine(to: viewport.toScreen(CGPoint(x: nearest.x, y: nearest.y)))
        }
        ctx.stroke(lines, with: .color(Theme.iceBlue.opacity(0.8)), lineWidth: 1)
    }

    private func distance(_ pad: SnapPad, _ p: CGPoint) -> Double {
        hypot(pad.x - Double(p.x), pad.y - Double(p.y))
    }
}

/// "Placing U3 (2 of 5)" with the keys and Skip / Skip All / Place All Automatically.
struct PlaceNewPartsHUD: View {
    @EnvironmentObject private var store: DesignStore
    let session: PlacementSession

    var body: some View {
        HStack(spacing: 10) {
            Image(systemName: "cursorarrow.click.badge.clock").foregroundStyle(Theme.skyBlue)
            VStack(alignment: .leading, spacing: 2) {
                Text(title).font(.callout.weight(.semibold)).foregroundStyle(Theme.textPrimary)
                Text("R rotates · F flips · click places · ⌥-click forces · Esc skips")
                    .font(.caption)
                    .foregroundStyle(Theme.textMuted)
            }
            Button("Skip") { store.skipPlacement() }
                .help("Leave this part where Auto Place put it")
            Button("Skip All") { store.skipAllPlacements() }
                .help("Leave every remaining part where Auto Place put it")
            Button("Place All Automatically") { store.placeAllAutomatically() }
                .buttonStyle(.borderedProminent)
                .help("Put every remaining part at the free spot next to its connections")
        }
        .controlSize(.small)
        .padding(.horizontal, 12)
        .padding(.vertical, 6)
        .background(Capsule().fill(Theme.deepBlue.opacity(0.95)))
        .overlay(Capsule().strokeBorder(Theme.blue.opacity(0.5)))
    }

    private var title: LocalizedStringKey {
        let ref = session.current?.ref ?? ""
        return "Placing \(ref) (\(session.number) of \(session.total))"
    }
}
