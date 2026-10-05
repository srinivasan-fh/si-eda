import SwiftUI

/// Status-bar progress of a running autoroute (`DesignStore.routeProgress`): the phase, the nets of the current pass,
/// what the best pass so far leaves unrouted, and Stop, which leaves the board as it was.
struct RouteProgressView: View {
    let progress: RouteProgressReport
    let onCancel: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            ProgressView(value: progress.fraction)
                .progressViewStyle(.linear)
                .frame(width: 120)
            caption
                .foregroundStyle(Theme.skyBlue)
                .monospacedDigit()
                .lineLimit(1)
                .truncationMode(.tail)
            Button("Stop", action: onCancel)
                .controlSize(.small)
                .help("Stop the autoroute; the board stays as it was")
        }
    }

    private var caption: Text {
        switch progress.phase {
        case 0:
            return Text("Preparing the autoroute…")
        case 1:
            return Text("Routing nets: \(progress.done) of \(progress.total)")
        case 2:
            return progress.unrouted >= 0
                ? Text("Rip-up pass \(progress.pass): \(progress.unrouted) unrouted")
                : Text("Rip-up pass \(progress.pass)")
        default:
            return Text("Finishing the autoroute…")
        }
    }
}
