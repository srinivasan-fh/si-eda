import CoreGraphics
import Foundation

/// What Update PCB did (`sieda_apply_pcb_eco`): one report line per change and the footprints it put on the board,
/// by designator, for the designer to place by hand.
struct PcbEcoResult: Decodable, Equatable {
    struct Queued: Decodable, Equatable {
        var id: Int
        var ref: String
    }

    var executed: Int
    var report: [String]
    var placementQueue: [Queued]

    init(executed: Int, report: [String], placementQueue: [Queued]) {
        self.executed = executed
        self.report = report
        self.placementQueue = placementQueue
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        executed = try c.decode(Int.self, forKey: .executed)
        report = try c.decode([String].self, forKey: .report)
        placementQueue = try c.decodeIfPresent([Queued].self, forKey: .placementQueue) ?? []
    }

    private enum CodingKeys: String, CodingKey { case executed, report, placementQueue }
}

/// A footprint at a candidate pose and what is wrong there (core `PlacementCheck`, `sieda_pcb_check_placement`).
struct PlacementCheckInfo: Decodable, Equatable {
    struct Box: Decodable, Equatable {
        var x0: Double
        var y0: Double
        var x1: Double
        var y1: Double
        var rect: CGRect { CGRect(x: x0, y: y0, width: x1 - x0, height: y1 - y0) }
    }

    struct GhostPad: Decodable, Equatable {
        var x: Double
        var y: Double
        var w: Double
        var h: Double
        var round: Bool
        var net: Int
        var rect: CGRect { CGRect(x: x - w / 2, y: y - h / 2, width: w, height: h) }
    }

    struct Issue: Decodable, Equatable {
        var code: String
        var message: String
        var other: Int
        var error: Bool
    }

    var component: Int
    var x: Double
    var y: Double
    var rotation: Int
    var bottom: Bool
    var legal: Bool
    var committed: Bool
    var courtyard: Box
    var pads: [GhostPad]
    var issues: [Issue]

    var position: CGPoint { CGPoint(x: x, y: y) }
    /// The first error (or else warning) to show beside the ghost.
    var firstMessage: String? { (issues.first { $0.error } ?? issues.first)?.message }
}

extension EDAEngine {
    /// The 0.25 mm grid footprints snap to (as when they are dragged).
    static let placementGrid = 0.25

    /// The footprint at that pose; nothing changes.
    func checkPlacement(_ id: Int, at point: CGPoint, rotation: Int, bottom: Bool) -> PlacementCheckInfo? {
        let reply = withHandle {
            sieda_pcb_check_placement($0, Int32(id), Double(point.x), Double(point.y), Int32(rotation),
                                      bottom ? 1 : 0, Self.placementGrid)
        }
        return Self.decode(PlacementCheckInfo.self, from: Self.take(reply))
    }

    /// Moves / turns / flips the footprint there when legal (or `force`); `committed` says whether it moved.
    func placeFootprint(_ id: Int, at point: CGPoint, rotation: Int, bottom: Bool, force: Bool = false) -> PlacementCheckInfo? {
        let reply = withHandle {
            sieda_pcb_place_footprint($0, Int32(id), Double(point.x), Double(point.y), Int32(rotation),
                                      bottom ? 1 : 0, Self.placementGrid, force ? 1 : 0)
        }
        return Self.decode(PlacementCheckInfo.self, from: Self.take(reply))
    }

    /// The nearest free spot to the parts it connects to; nothing changes.
    func suggestPlacement(_ id: Int) -> PlacementCheckInfo? {
        let reply = withHandle { sieda_pcb_suggest_placement($0, Int32(id), Self.placementGrid) }
        return Self.decode(PlacementCheckInfo.self, from: Self.take(reply))
    }
}
