import Foundation

// Channel analysis (lossy lines, S-parameters, Touchstone, eye) and power-integrity planning (plane cavity,
// decoupling plan, IR-drop map) decoded from the core (`sieda_si_channel_json`, `sieda_pi_*_json`).

/// One S-parameter magnitude curve ("S21", "SDD21", …) in dB on the report's frequency grid.
struct SIParameterCurve: Decodable, Equatable, Identifiable {
    var name: String
    var db: [Double]

    var id: String { name }
}

/// Loss of one stack-up layer (`sieda_si_line_loss_json`).
struct SILineLossLayer: Decodable, Equatable, Identifiable {
    var layer: Int
    var name: String
    var line: String
    var width: Double
    var z0: Double
    var epsEff: Double
    var dbPerInch: [Double]
    var conductorDbPerInch: [Double]
    var dielectricDbPerInch: [Double]

    var id: Int { layer }
}

struct SILineLossReport: Decodable, Equatable {
    var foil = ""
    var foilName = ""
    var roughness = ""
    var material = ""
    var er = 0.0
    var tanD = 0.0
    var freq: [Double] = []
    var layers: [SILineLossLayer] = []

    static let empty = SILineLossReport()
}

/// Copper foil profiles the core knows (`sieda_si_set_copper_foil`).
enum SICopperFoil: String, CaseIterable, Identifiable {
    case automatic = "", smooth, hvlp, vlp, rtf, std
    var id: String { rawValue }
}

struct SIChannelDriver: Decodable, Equatable {
    var ref: String
    var pin: String
    var model: String
    var rOut: Double
    var riseTime: Double
    var swing: Double
}

struct SIChannelReceiver: Decodable, Equatable {
    var ref: String
    var pin: String
    var model: String
}

/// A P / N section routed side by side, as coupled lines.
struct SICoupledSection: Decodable, Equatable, Identifiable {
    var layer: String
    var length: Double
    var gap: Double
    var zEven: Double
    var zOdd: Double
    var zDiff: Double
    var zComm: Double
    var epsEven: Double
    var epsOdd: Double

    var id: String { "\(layer)/\(gap)" }
}

struct SINyquist: Decodable, Equatable {
    var f: Double
    var il: Double
    var rl: Double
}

/// Step response at the receiver, with the lossless line for comparison.
struct SIStepResponse: Decodable, Equatable {
    var time: [Double]
    var lossy: [Double]
    var lossless: [Double]?
}

/// Eye diagram of a PRBS stream through the channel.
struct SIEyeReport: Decodable, Equatable {
    var error: String
    var bitRate: Double
    var ui: Double
    var prbs: Int
    var bits: Double
    var samplesPerUi: Int
    var vMid: Double
    var amplitude: Double
    var eyeHeight: Double
    var eyeWidth: Double
    var eyeWidthBer: Double
    var bestPhase: Double
    var pdaHeight: Double
    var djPeakToPeak: Double
    var jitterRms: Double
    var totalJitter: Double
    var open: Bool
    var maskMargin: Double
    var maskPass: Bool
    var ctleDcGainDb: Double
    var ffeTaps: [Double]
    var cursors: [Double]
    var mainCursor: Int
    var cols: Int
    var rows: Int
    var vMin: Double
    var vMax: Double
    var density: [Double]
    var upper: [Double]
    var lower: [Double]
    var pulseTime: [Double]
    var pulse: [Double]
    var notes: [String]

    /// Hit density (0…1) of a plot cell; 0 outside the grid.
    func hits(row: Int, col: Int) -> Double {
        guard row >= 0, col >= 0, row < rows, col < cols else { return 0 }
        let index = row * cols + col
        return density.indices.contains(index) ? density[index] : 0
    }
}

/// Channel of a routed net or differential pair (`sieda_si_channel_json`).
struct SIChannelReport: Decodable, Equatable {
    var net: String
    var partner: String
    var differential: Bool
    var estimated: Bool
    var ports: Int
    var refOhms: Double
    var driver: SIChannelDriver
    var receiver: SIChannelReceiver
    var length: Double
    var lengthN: Double
    var delay: Double
    var skew: Double
    var vias: Int
    var coupledLength: Double
    var coupled: [SICoupledSection]
    var freq: [Double]
    var curves: [SIParameterCurve]
    var nyquist: SINyquist?
    var step: SIStepResponse
    var eye: SIEyeReport?
    var notes: [String]
}

/// An imported Touchstone file, with its step response and eye when run as a channel.
struct SITouchstoneReport: Decodable, Equatable {
    var ports: Int
    var z0: Double
    var points: Int
    var fMin: Double
    var fMax: Double
    var format: String
    var parameter: String
    var version: String
    var freq: [Double]
    var curves: [SIParameterCurve]
    var comments: [String]
    var step: SIStepResponse?
    var eye: SIEyeReport?
    var error: String?
}

/// Options of a channel run, encoded for the core.
struct SIChannelSettings: Equatable {
    var bitRate = 5e9
    var prbs = 7
    var idealDriver = true
    var swing = 1.0
    var ctle = false
    var ffe = false
    var rjRms = 0.0
    var maskHeight = 0.0
    var maskWidthUi = 0.0
    var fMax = 20e9
    var roughness = "huray"
    var portOrder = "13"

    /// The `eye` object and drive fields of `sieda_si_channel_json` / `sieda_touchstone_channel_json`.
    func options(net: String, partner: String, touchstone: String?, touchstonePorts: Int) -> [String: Any] {
        var eye: [String: Any] = ["bitRate": bitRate, "prbs": prbs, "rjRms": rjRms, "maskHeight": maskHeight,
                                  "maskWidthUi": maskWidthUi]
        if ctle { eye["ctle"] = true; eye["ctleAuto"] = true }
        if ffe { eye["ffe"] = true; eye["ffeAuto"] = true }
        var o: [String: Any] = ["net": net, "partner": partner, "fMax": fMax, "points": 401, "roughness": roughness,
                                "driver": idealDriver ? "ideal" : "model", "swing": swing, "eye": eye,
                                "portOrder": portOrder]
        if let touchstone, !touchstone.isEmpty {
            o["touchstone"] = touchstone
            o["touchstonePorts"] = touchstonePorts
        }
        return o
    }
}

// MARK: - Power-integrity planning

struct PDNPoint: Decodable, Equatable {
    var x: Double
    var y: Double
}

struct PDNCavityMode: Decodable, Equatable, Identifiable {
    var m: Int
    var n: Int
    var f: Double

    var id: String { "\(m),\(n)" }
}

/// Plane-pair cavity model of a rail (`sieda_pi_cavity_json`).
struct PDNCavityReport: Decodable, Equatable {
    var rail: String
    var available: Bool
    var note: String
    var a: Double
    var b: Double
    var d: Double
    var er: Double
    var ports: Int
    var observe: PDNPoint
    var modes: [PDNCavityMode]
    var freq: [Double]
    var zCavity: [Double]
    var zLumped: [Double]
    var zPlane: [Double]?
    var droop: Double?
    var droopLimit: Double?
    var target: Double
    var worstRatio: Double
    var worstF: Double
    var recommendations: [String]
}

struct PDNDecapAddition: Decodable, Equatable, Identifiable {
    var value: String
    var footprint: String
    var c: Double
    var count: Int

    var id: String { "\(value)/\(footprint)" }
}

/// Decoupling capacitors that bring a rail under its target (`sieda_pi_decap_plan_json`).
struct PDNDecapPlanReport: Decodable, Equatable {
    var rail: String
    var needed: Bool
    var compliant: Bool
    var worstBefore: Double
    var worstAfter: Double
    var mounting: Double
    var target: Double
    var additions: [PDNDecapAddition]
    var freq: [Double]
    var zBefore: [Double]
    var zAfter: [Double]
}

struct PDNIRBoard: Decodable, Equatable {
    var width: Double
    var height: Double
    var outline: [PDNPoint]
}

struct PDNIRCell: Decodable, Equatable {
    var x: Double
    var y: Double
    var size: Double
    var layer: Int
    var drop: Double
    var density: Double
}

struct PDNIRSegment: Decodable, Equatable {
    var ax: Double
    var ay: Double
    var bx: Double
    var by: Double
    var layer: Int
    var width: Double
    var current: Double
    var density: Double
    var drop: Double
}

struct PDNIRLoadPoint: Decodable, Equatable, Identifiable {
    var ref: String
    var pin: String
    var x: Double
    var y: Double
    var drop: Double
    var connected: Bool

    var id: String { "\(ref).\(pin)" }
}

struct PDNIRHotspot: Decodable, Equatable {
    var x: Double
    var y: Double
    var density: Double
    var layer: Int
}

/// DC voltage drop and current density over a rail's copper (`sieda_pi_ir_map_json`).
struct PDNIRMapReport: Decodable, Equatable {
    var rail: String
    var analyzed: Bool
    var note: String
    var voltage: Double
    var limit: Double
    var worst: Double
    var maxDensity: Double
    var board: PDNIRBoard
    var cells: [PDNIRCell]
    var segments: [PDNIRSegment]
    var loads: [PDNIRLoadPoint]
    var source: PDNPoint?
    var hotspots: [PDNIRHotspot]
}
