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

/// Result of a board command (teardrops, via stitching / shielding, glossing): what was added or removed.
struct BoardEditResult: Decodable, Equatable {
    var ok: Bool
    var message: String
    var added: Int
    var skipped: Int
    var applied: Bool
    var addedTracks: [SnapTrack]
    var addedVias: [SnapVia]
    var removedTracks: [Int]
    var removedVias: [Int]
}

/// Result of matching several nets' lengths (`sieda_pcb_match_lengths`).
struct MatchLengthsResult: Decodable, Equatable {
    var ok: Bool
    var message: String
    var target: Double
    var tuned: Int
    var matched: Int
    var short: Int
}

/// Meander pattern of the length tuning tool (`sieda_router_tune` "style").
enum MeanderStyleChoice: String, CaseIterable, Identifiable {
    case accordion, trombone, sawtooth
    var id: String { rawValue }
}

/// Meander corner shape ("corner"): square, 45° mitered or round (true arcs).
enum MeanderCornerChoice: String, CaseIterable, Identifiable {
    case square, mitered, round
    var id: String { rawValue }
}

/// Length rules, match groups and their members' current lengths (`sieda_length_targets_json`).
struct LengthTargets: Decodable, Equatable {
    struct Rule: Decodable, Equatable, Identifiable {
        var net: String
        var target: Double
        var tolerance: Double
        var length: Double
        var ok: Bool
        var routed: Bool
        var id: String { net }
    }
    struct Member: Decodable, Equatable, Identifiable {
        var net: String
        var xsignal: [String]
        var length: Double
        var ok: Bool
        var routed: Bool
        var id: String { net }
    }
    struct Group: Decodable, Equatable, Identifiable {
        var name: String
        var tolerance: Double
        var target: Double
        var members: [Member]
        var id: String { name }
    }
    var rules: [Rule]
    var groups: [Group]
    static let empty = LengthTargets(rules: [], groups: [])
}

/// Interactive-routing additions of the core (docs/INTERACTIVE_ROUTING.md): router options with true arcs,
/// corner-to-arc conversion and the other routing commands.
extension EDAEngine {
    /// Router options JSON (`sieda_router_*`). `rounded`: corners get the automatic radius; `arcs`: as true arcs
    /// (pairs and buses on concentric arcs) instead of short chords. `tune`: tune lengths while routing (a bus's or a
    /// matched net's short members get meanders on commit; the preview reports `memberLengths`).
    static func routingOptions(mode: RouterModeChoice, diagonal: Bool, via: RouterViaChoice = .through,
                               rounded: Bool = false, arcs: Bool = true, anyAngle: Bool = false,
                               removeLoops: Bool = false, teardrops: Bool = false, hug: Bool = false,
                               tune: Bool = false) -> String {
        let posture = anyAngle ? "free" : (diagonal ? "45" : "90")
        return "{\"mode\":\"\(mode.rawValue)\",\"posture\":\"\(posture)\",\"viaType\":\"\(via.rawValue)\","
            + "\"cornerRadius\":\(rounded ? -1 : 0),\"arcCorners\":\(rounded && arcs),"
            + "\"removeLoops\":\(removeLoops),\"teardrops\":\(teardrops),\"hug\":\(hug),"
            + "\"tuneWhileRouting\":\(tune)}"
    }

    /// Drags the corner of `trackId` nearest to `point`.
    func routerBeginCornerDrag(track trackId: Int, at point: CGPoint, options: String) -> RoutePreview? {
        Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_corner_drag($0, options, Int32(trackId), Double(point.x), Double(point.y)))
        })
    }

    /// Drags several tracks together by the cursor's movement.
    func routerBeginMultiDrag(tracks: [Int], at point: CGPoint, options: String) -> RoutePreview? {
        let ids = Self.idList(tracks)
        return Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_multi_drag($0, options, ids, Double(point.x), Double(point.y)))
        })
    }

    /// Routes the nets at `starts` together as one bundle (multi-route).
    func routerBeginMulti(starts: [CGPoint], layer: Int, options: String) -> RoutePreview? {
        let points = "[" + starts.map { String(format: "{\"x\":%.6f,\"y\":%.6f}", Double($0.x), Double($0.y)) }
            .joined(separator: ",") + "]"
        return Self.decode(RoutePreview.self, from: withHandle {
            Self.take(sieda_router_begin_multi($0, options, points, Int32(layer)))
        })
    }

    static func idList(_ ids: [Int]) -> String { "[" + ids.map(String.init).joined(separator: ",") + "]" }

    /// Everything the Tune Length tool asks for: target (0 = rule / group / pair), meander height and spacing (0 =
    /// defaults), the click point (meanders go near it), the drag-along end, pattern, corners, coupled, phase.
    struct TuneRequest: Equatable {
        var target: Double
        var amplitude: Double
        var spacing: Double
        var near: CGPoint?
        var spanEnd: CGPoint?
        var style: MeanderStyleChoice
        var corner: MeanderCornerChoice
        var coupled: Bool
        var phase: Bool
        var apply: Bool

        var json: String {
            var fields = [String(format: "\"target\":%.6f", max(0, target)),
                          String(format: "\"maxAmplitude\":%.6f", max(0, amplitude)),
                          String(format: "\"spacing\":%.6f", max(0, spacing)),
                          "\"apply\":\(apply)", "\"style\":\"\(style.rawValue)\"", "\"corner\":\"\(corner.rawValue)\"",
                          "\"coupled\":\(coupled)", "\"phase\":\(phase)"]
            if let near, near.x.isFinite, near.y.isFinite {
                fields.append(String(format: "\"x\":%.6f,\"y\":%.6f", Double(near.x), Double(near.y)))
                if let end = spanEnd, end.x.isFinite, end.y.isFinite, hypot(end.x - near.x, end.y - near.y) > 0.05 {
                    fields.append(String(format: "\"fromX\":%.6f,\"fromY\":%.6f,\"toX\":%.6f,\"toY\":%.6f",
                                         Double(near.x), Double(near.y), Double(end.x), Double(end.y)))
                }
            }
            return "{" + fields.joined(separator: ",") + "}"
        }
    }

    /// Length tuning of the net of `trackId` with every option: a preview (`apply` false) or the change itself.
    func routerTune(track trackId: Int, request: TuneRequest) -> TunePreview? {
        let options = request.json
        return Self.decode(TunePreview.self, from: withHandle { Self.take(sieda_router_tune($0, Int32(trackId), options)) })
    }

    /// Length rule of a net (target ± tolerance mm, pad to pad through series parts); target 0 removes it.
    @discardableResult
    func setLengthRule(net: String, target: Double, tolerance: Double) -> Bool {
        withHandle { sieda_pcb_set_length_rule($0, net, target, tolerance) } == 1
    }

    /// Match group: these nets' xSignal lengths match the longest within `tolerance`; fewer than two nets removes it.
    @discardableResult
    func setMatchGroup(name: String, nets: [String], tolerance: Double) -> Bool {
        guard let body = try? JSONSerialization.data(withJSONObject: ["name": name, "nets": nets, "tolerance": tolerance])
        else { return false }
        return withHandle { sieda_pcb_set_match_group($0, String(decoding: body, as: UTF8.self)) } == 1
    }

    func lengthTargets() -> LengthTargets {
        Self.decode(LengthTargets.self, from: withHandle { Self.take(sieda_length_targets_json($0)) }) ?? .empty
    }

    /// Converts the corners between the given tracks to true arcs (radius 0 = automatic); `apply` false previews.
    func arcCorners(tracks: [Int], radius: Double = 0, apply: Bool = true) -> ArcCornersResult? {
        let options = "{\"radius\":\(max(0, radius)),\"apply\":\(apply)}"
        return Self.decode(ArcCornersResult.self, from: withHandle {
            Self.take(sieda_pcb_arc_corners($0, Self.idList(tracks), options))
        })
    }

    /// Adds teardrops where the tracks (empty: all) meet pads and vias, or (`remove`) removes theirs.
    func teardrops(tracks: [Int], remove: Bool = false, apply: Bool = true) -> BoardEditResult? {
        let options = "{\"remove\":\(remove),\"apply\":\(apply)}"
        return Self.decode(BoardEditResult.self, from: withHandle {
            Self.take(sieda_pcb_teardrops($0, Self.idList(tracks), options))
        })
    }

    /// Via stitching of `net` (empty: ground) where its pours overlap on two or more layers.
    func stitchVias(net: String = "", pitch: Double = 0) -> BoardEditResult? {
        let options = Self.viaPatternOptions(net: net, pitch: pitch)
        return Self.decode(BoardEditResult.self, from: withHandle { Self.take(sieda_pcb_stitch_vias($0, options)) })
    }

    /// Via shielding: rows of `net` (empty: ground) vias on both sides of the tracks.
    func shieldTracks(_ tracks: [Int], net: String = "", pitch: Double = 0) -> BoardEditResult? {
        let options = Self.viaPatternOptions(net: net, pitch: pitch)
        return Self.decode(BoardEditResult.self, from: withHandle {
            Self.take(sieda_pcb_shield_tracks($0, Self.idList(tracks), options))
        })
    }

    /// Glossing: pulls the lines through the tracks tight (and re-searches them with `retrace`).
    func gloss(tracks: [Int], retrace: Bool = true) -> BoardEditResult? {
        let options = "{\"retrace\":\(retrace)}"
        return Self.decode(BoardEditResult.self, from: withHandle {
            Self.take(sieda_pcb_gloss($0, Self.idList(tracks), options))
        })
    }

    static func viaPatternOptions(net: String, pitch: Double) -> String {
        let object: [String: Any] = ["net": net, "pitch": pitch.isFinite ? max(0, pitch) : 0]
        guard let data = try? JSONSerialization.data(withJSONObject: object),
              let text = String(data: data, encoding: .utf8) else { return "{}" }
        return text
    }

    /// Tunes the nets of `tracks` to the longest of them (within `tolerance` mm) with the meander style and corners.
    func matchLengths(tracks: [Int], style: MeanderStyleChoice = .accordion, corner: MeanderCornerChoice = .square,
                      tolerance: Double = 0.1) -> MatchLengthsResult? {
        let tol = tolerance.isFinite ? max(0.01, tolerance) : 0.1
        let options = "{\"style\":\"\(style.rawValue)\",\"corner\":\"\(corner.rawValue)\",\"tolerance\":\(tol)}"
        return Self.decode(MatchLengthsResult.self, from: withHandle {
            Self.take(sieda_pcb_match_lengths($0, Self.idList(tracks), options))
        })
    }
}
