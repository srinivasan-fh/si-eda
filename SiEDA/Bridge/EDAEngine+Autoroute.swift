import CoreGraphics
import Foundation

/// Autorouter strategy of the board (core `AutorouteOptions`, saved with the project; docs/ROUTING.md, Strategies).
/// Every option defaults to the router's classic behaviour.
struct AutorouteOptions: Codable, Equatable {
    var coupledPairs = false
    var pairGap = 0.0
    var lengthAware = false
    var minimizeVias = false
    var gloss = false
    var arcCorners = false
    var arcRadius = 0.0
    var teardrops = false
    /// Teardrop outline (core `TeardropStyle`, JSON "teardropStyle").
    var teardropStyle = TeardropStyleChoice.straight
    var preset = "default"
    var fast = false
    var fanoutOnly = false
    var nets: [String] = []
    var netClass = ""
    var hasArea = false
    var area: AutorouteArea?
    var protectLocked = false
    /// Per schematic net class: the copper layers its nets may route on.
    var classLayers: [String: [Int]] = [:]

    init() {}

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        coupledPairs = try c.decodeIfPresent(Bool.self, forKey: .coupledPairs) ?? false
        pairGap = try c.decodeIfPresent(Double.self, forKey: .pairGap) ?? 0
        lengthAware = try c.decodeIfPresent(Bool.self, forKey: .lengthAware) ?? false
        minimizeVias = try c.decodeIfPresent(Bool.self, forKey: .minimizeVias) ?? false
        gloss = try c.decodeIfPresent(Bool.self, forKey: .gloss) ?? false
        arcCorners = try c.decodeIfPresent(Bool.self, forKey: .arcCorners) ?? false
        arcRadius = try c.decodeIfPresent(Double.self, forKey: .arcRadius) ?? 0
        teardrops = try c.decodeIfPresent(Bool.self, forKey: .teardrops) ?? false
        let style = try c.decodeIfPresent(String.self, forKey: .teardropStyle) ?? ""
        teardropStyle = TeardropStyleChoice(rawValue: style) ?? .straight
        preset = try c.decodeIfPresent(String.self, forKey: .preset) ?? "default"
        fast = try c.decodeIfPresent(Bool.self, forKey: .fast) ?? false
        fanoutOnly = try c.decodeIfPresent(Bool.self, forKey: .fanoutOnly) ?? false
        nets = try c.decodeIfPresent([String].self, forKey: .nets) ?? []
        netClass = try c.decodeIfPresent(String.self, forKey: .netClass) ?? ""
        hasArea = try c.decodeIfPresent(Bool.self, forKey: .hasArea) ?? false
        area = try c.decodeIfPresent(AutorouteArea.self, forKey: .area)
        protectLocked = try c.decodeIfPresent(Bool.self, forKey: .protectLocked) ?? false
        classLayers = try c.decodeIfPresent([String: [Int]].self, forKey: .classLayers) ?? [:]
    }

    /// Routes only part of the board (selected nets, a net class or an area).
    var isScoped: Bool { !nets.isEmpty || !netClass.isEmpty || hasArea }
}

/// The area of the "Area" strategy (mm, board coordinates).
struct AutorouteArea: Codable, Equatable {
    var x0: Double
    var y0: Double
    var x1: Double
    var y1: Double
}

/// A preset strategy (`sieda_autoroute_presets`).
struct AutoroutePreset: Decodable, Equatable, Identifiable {
    var name: String
    var title: String
    var description: String
    var options: AutorouteOptions
    var id: String { name }
}

/// The last autoroute's report (`sieda_pcb_route_report`): differential pairs, length targets and board metrics.
struct RouteReport: Decodable, Equatable {
    struct Pair: Decodable, Equatable, Identifiable {
        var positive: String
        var negative: String
        var coupled: Bool
        var reason: String
        var width: Double
        var gap: Double
        var coupledLength: Double
        var uncoupledLength: Double
        var skew: Double
        var viaPairs: Int
        var id: String { positive + "/" + negative }
    }
    struct Length: Decodable, Equatable, Identifiable {
        var net: String
        var source: String
        var target: Double
        var tolerance: Double
        var routed: Double
        var achieved: Double
        var ok: Bool
        var tuned: Bool
        var id: String { net }
    }
    struct Metrics: Decodable, Equatable {
        var vias = 0
        var microvias = 0
        var blindVias = 0
        var trackLength = 0.0
        var layerLength: [Double] = []
        var segments = 0
        var arcs = 0
        var teardrops = 0
        var unrouted = 0
        var viasRemoved = 0
        var netsRerouted = 0
        var glossed = 0
        var arcsAdded = 0
        var teardropsAdded = 0
    }
    var pairs: [Pair] = []
    var lengths: [Length] = []
    var metrics = Metrics()
}

/// A routing keep-out (`sieda_pcb_keepouts`): no track and / or via inside the area on a layer (-1 = all layers).
struct RouteKeepout: Codable, Equatable, Identifiable {
    var name: String
    var x0: Double
    var y0: Double
    var x1: Double
    var y1: Double
    var layer: Int
    var tracks: Bool
    var vias: Bool
    var id: String { "\(name)|\(x0)|\(y0)|\(x1)|\(y1)|\(layer)" }
}

extension EDAEngine {
    /// The board's autorouter strategy.
    func autorouteOptions() -> AutorouteOptions {
        Self.decode(AutorouteOptions.self, from: withHandle { Self.take(sieda_pcb_autoroute_options($0)) }) ?? AutorouteOptions()
    }

    /// Replaces the board's autorouter strategy; false when the core rejected it.
    @discardableResult
    func setAutorouteOptions(_ options: AutorouteOptions) -> Bool {
        guard let data = try? JSONEncoder().encode(options), let json = String(data: data, encoding: .utf8) else { return false }
        return withHandle { sieda_pcb_set_autoroute_options($0, json) } == 1
    }

    /// The preset strategies (default, fast, high quality, fan-out only, selected nets, net class, area).
    static func autoroutePresets() -> [AutoroutePreset] {
        decode([AutoroutePreset].self, from: take(sieda_autoroute_presets())) ?? []
    }

    /// The last autoroute's report (empty before the first route).
    func routeReport() -> RouteReport {
        Self.decode(RouteReport.self, from: withHandle { Self.take(sieda_pcb_route_report($0)) }) ?? RouteReport()
    }

    func keepouts() -> [RouteKeepout] {
        Self.decode([RouteKeepout].self, from: withHandle { Self.take(sieda_pcb_keepouts($0)) }) ?? []
    }

    /// Replaces the routing keep-outs; returns how many the core kept.
    @discardableResult
    func setKeepouts(_ keepouts: [RouteKeepout]) -> Int {
        guard let data = try? JSONEncoder().encode(keepouts), let json = String(data: data, encoding: .utf8) else { return 0 }
        return Int(withHandle { sieda_pcb_set_keepouts($0, json) })
    }
}
