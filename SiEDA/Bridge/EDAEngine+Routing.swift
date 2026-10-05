import CoreGraphics
import Foundation

/// "Convert corners to arcs" (`sieda_pcb_arc_corners`): corners made arcs, corners kept sharp (no room), the arc
/// tracks added and the tracks they replace.
struct ArcCornersResult: Decodable, Equatable {
    var ok: Bool
    var message: String
    var converted: Int
    var kept: Int
    var applied: Bool
    var addedTracks: [SnapTrack]
    var removedTracks: [Int]
}

/// Interactive-routing additions of the core (docs/INTERACTIVE_ROUTING.md): router options with true arcs,
/// corner-to-arc conversion and the other routing commands.
extension EDAEngine {
    /// Router options JSON (`sieda_router_*`). `rounded`: corners get the automatic radius; `arcs`: as true arcs
    /// (pairs and buses on concentric arcs) instead of short chords.
    static func routingOptions(mode: RouterModeChoice, diagonal: Bool, via: RouterViaChoice = .through,
                               rounded: Bool = false, arcs: Bool = true) -> String {
        "{\"mode\":\"\(mode.rawValue)\",\"posture\":\"\(diagonal ? "45" : "90")\",\"viaType\":\"\(via.rawValue)\","
            + "\"cornerRadius\":\(rounded ? -1 : 0),\"arcCorners\":\(rounded && arcs)}"
    }

    static func idList(_ ids: [Int]) -> String { "[" + ids.map(String.init).joined(separator: ",") + "]" }

    /// Converts the corners between the given tracks to true arcs (radius 0 = automatic); `apply` false previews.
    func arcCorners(tracks: [Int], radius: Double = 0, apply: Bool = true) -> ArcCornersResult? {
        let options = "{\"radius\":\(max(0, radius)),\"apply\":\(apply)}"
        return Self.decode(ArcCornersResult.self, from: withHandle {
            Self.take(sieda_pcb_arc_corners($0, Self.idList(tracks), options))
        })
    }
}
