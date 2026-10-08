import Foundation

// Signal- and power-integrity results decoded from the core (`sieda_si_*` / `sieda_pi_*`).

/// A driver / receiver model: a logic-family default or an imported IBIS buffer.
struct SIDriverModel: Decodable, Equatable, Identifiable {
    var id: String
    var name: String
    var source: String
    var type: String
    var vHigh: Double
    var riseTime: Double
    var fallTime: Double
    var rOut: Double
    var cComp: Double
    var cIn: Double
    var vih: Double
    var vil: Double
    var note: String
}

struct PDNRailSetting: Decodable, Equatable {
    var net: String
    var ripplePercent: Double
    var transientCurrent: Double
    var dcCurrent: Double
    var vrmR: Double?
    var vrmBandwidth: Double?
}

/// A serial channel checked by sign-off (SI_EYE_MASK).
struct SIChannelSpec: Decodable, Equatable {
    var net: String
    var bitRate: Double
    var maskHeight: Double
    var maskWidthUi: Double
}

/// SI / PI setup of the project (`sieda_si_settings_json`).
struct SISettings: Decodable, Equatable {
    var signOff = false
    var overshootLimit = 0.15
    var crosstalkLimit = 0.05
    var models: [SIDriverModel] = []
    var families: [SIDriverModel] = []
    var componentModels: [String: String] = [:]
    var pinModels: [String: String] = [:]
    var netModels: [String: String] = [:]
    var rails: [PDNRailSetting] = []
    /// Copper foil for loss ("" or nil = by laminate) and serial channels checked in sign-off.
    var copperFoil: String?
    var fieldSolverLines: Bool?
    var channels: [SIChannelSpec]?

    static let empty = SISettings()

    /// Imported models first, then the logic families.
    var allModels: [SIDriverModel] { models + families }
}

/// One row of the SI net picker (`sieda_si_net_list_json`).
struct SINetSummary: Decodable, Equatable, Identifiable {
    var net: Int
    var name: String
    var length: Double
    var delay: Double
    var critical: Bool
    var criticalLength: Double
    var driver: String
    var model: String
    var modelId: String
    var fast: Bool
    var routed: Bool
    var receivers: Int

    var id: Int { net }
}

struct SIEdgeMetrics: Decodable, Equatable {
    var vLow: Double
    var vHigh: Double
    var overshoot: Double
    var undershoot: Double
    var ringbackHigh: Double
    var ringbackLow: Double
    var settling: Double
    var settled: Bool
    var flightTime: Double
    var reachesHigh: Bool
    var reachesLow: Bool
    var overshootPercent: Double
    var undershootPercent: Double
}

struct SIReceiverResult: Decodable, Equatable, Identifiable {
    var component: Int
    var ref: String
    var pin: String
    var model: String
    var delay: Double
    var length: Double
    var connected: Bool
    var ok: Bool
    var metrics: SIEdgeMetrics

    var id: String { "\(ref).\(pin)" }
}

struct SILineSection: Decodable, Equatable, Identifiable {
    var layer: String
    var width: Double
    var length: Double
    var z0: Double
    var delay: Double

    var id: String { "\(layer)/\(width)" }
}

struct SIDriverInfo: Decodable, Equatable {
    var id: String
    var name: String
    var source: String
    var riseTime: Double
    var rOut: Double
    var vHigh: Double
    var ref: String
    var pin: String
    var assumed: Bool
}

/// Sampled waveforms: the ideal source, the driver pin and the worst receiver.
struct SIWaveform: Decodable, Equatable {
    var time: [Double]
    var source: [Double]?
    var driver: [Double]?
    var receiver: [Double]?
}

/// Transmission-line analysis of one net (`sieda_si_net_json`).
struct SINetAnalysis: Decodable, Equatable {
    var net: Int
    var name: String
    var error: String
    var routed: Bool
    var estimated: Bool
    var driver: SIDriverInfo
    var seriesR: Double
    var seriesRef: String
    var terminations: [String]
    var length: Double
    var delay: Double
    var z0Min: Double
    var z0Max: Double
    var z0Trunk: Double
    var criticalLength: Double
    var critical: Bool
    var vias: Int
    var sections: [SILineSection]
    var receivers: [SIReceiverResult]
    var worstReceiver: Int
    var ok: Bool
    var recommendedSeriesR: Double
    var recommendation: String
    var terminated: SIEdgeMetrics
    var waveform: SIWaveform
    var terminatedWaveform: SIWaveform?
    var notes: [String]

    var worst: SIReceiverResult? {
        receivers.indices.contains(worstReceiver) ? receivers[worstReceiver] : receivers.first
    }
}

struct SICrosstalkPair: Decodable, Equatable, Identifiable {
    var aggressor: String
    var victim: String
    var layer: String
    var coupledLength: Double
    var spacing: Double
    var next: Double
    var fext: Double
    var noise: Double
    var limit: Double
    var ok: Bool
    var x: Double
    var y: Double
    var broadside: Bool?  // victim on the adjacent layer

    var id: String { "\(aggressor)>\(victim)" }
}

struct SIReturnPathIssue: Decodable, Equatable, Identifiable {
    var code: String
    var net: String
    var message: String
    var x: Double
    var y: Double

    var id: String { "\(code)@\(x),\(y)" }
}

/// Crosstalk pairs and return-path problems (`sieda_si_crosstalk_json`).
struct SICrosstalkReport: Decodable, Equatable {
    var pairs: [SICrosstalkPair] = []
    var returnPath: [SIReturnPathIssue] = []
    var limit = 0.05

    static let empty = SICrosstalkReport()
}

struct PDNDecap: Decodable, Equatable, Identifiable {
    var component: Int
    var ref: String
    var value: String
    var footprint: String
    var c: Double
    var esr: Double
    var esl: Double
    var mounting: Double
    var srf: Double
    var distance: Double

    var id: Int { component }
}

struct PDNPlane: Decodable, Equatable {
    var area: Double
    var gap: Double
    var capacitance: Double
    var inductance: Double
    var layers: String
    var cavityResonance: Double
}

struct PDNCurve: Decodable, Equatable {
    var freq: [Double]
    var z: [Double]
}

struct PDNPeak: Decodable, Equatable {
    var f: Double
    var z: Double
}

struct PDNLoad: Decodable, Equatable, Identifiable {
    var component: Int
    var ref: String
    var pin: String
    var current: Double
    var drop: Double
    var connected: Bool

    var id: String { "\(ref).\(pin)" }
}

struct PDNIRDrop: Decodable, Equatable {
    var analyzed: Bool
    var source: String
    var note: String
    var worst: Double
    var worstRef: String
    var limitPercent: Double
    var loads: [PDNLoad]
}

/// One supply rail: impedance profile against its target, decoupling, plane pair and IR drop.
struct PDNRail: Decodable, Equatable, Identifiable {
    var net: Int
    var name: String
    var voltage: Double
    var voltageEstimated: Bool
    var ripplePercent: Double
    var transientCurrent: Double
    var dcCurrent: Double
    var currentEstimated: Bool
    var target: Double
    var fMax: Double
    var vrmRef: String
    var vrmKind: String
    var vrmR: Double
    var vrmL: Double
    var vrmBandwidth: Double?
    var decaps: [PDNDecap]
    var plane: PDNPlane
    var curve: PDNCurve
    var peaks: [PDNPeak]
    var worstZ: Double
    var worstF: Double
    var compliant: Bool
    var irDrop: PDNIRDrop
    var recommendations: [String]

    var id: Int { net }
}

/// Power distribution of every rail (`sieda_pi_json`).
struct PDNReport: Decodable, Equatable {
    var rails: [PDNRail] = []

    static let empty = PDNReport()
}
