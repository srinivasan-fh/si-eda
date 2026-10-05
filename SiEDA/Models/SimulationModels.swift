import Foundation

// Simulation types added with SPICE model import, noise analysis, parameter sweep / FFT panels and waveform
// measurements (docs/SIMULATION.md). They decode the C API's JSON; every field the core may omit is optional.

/// An imported SPICE model on a part, as the snapshot summarises it (the text: EDAEngine.spiceModel(of:)).
struct SpiceAttachment: Decodable, Equatable {
    var model: String
    var pins: String
    var bytes: Int
}

/// A parser or flattening message, with its 1-based line in the model text (0: not tied to a line).
struct SpiceDiagnostic: Decodable, Equatable, Identifiable, Hashable {
    var level: String
    var line: Int
    var message: String

    var id: String { "\(line):\(level):\(message)" }
    var isError: Bool { level == "error" }
    var isWarning: Bool { level == "warning" }
}

/// A .model or .subckt a library offers.
struct SpiceLibraryEntry: Decodable, Equatable, Identifiable, Hashable {
    var name: String
    var kind: String
    var type: String
    var ports: [String]
    var line: Int

    var id: String { name }
    /// "NPN · C B E", "SUBCKT · IN+ IN- V+ V- OUT".
    var summary: String { "\(type) · \(ports.joined(separator: " "))" }
}

struct SpiceParseResult: Decodable, Equatable {
    var ok: Bool = false
    var entries: [SpiceLibraryEntry] = []
    var diagnostics: [SpiceDiagnostic] = []
}

/// A model tried on a part without changing the design.
struct SpiceCheckResult: Decodable, Equatable {
    var ok: Bool = false
    var error: String = ""
    var kind: String?
    var type: String?
    var ports: [String]?
    var pins: String?
    var defaultPins: String?
    var diagnostics: [SpiceDiagnostic]?
}

struct SpiceModelText: Decodable, Equatable {
    var text: String
    var model: String
    var pins: String
}

/// A ready-made model of a common part.
struct SpiceBuiltinModel: Decodable, Equatable, Identifiable {
    var name: String
    var description: String
    var text: String

    var id: String { name }
}

// MARK: - Noise analysis

struct NoiseContributionInfo: Decodable, Equatable, Identifiable {
    var ref: String
    var kind: String
    var rms: Double

    var id: String { "\(ref)·\(kind)" }
}

/// Output noise density of a node, the input-referred density and the integrated RMS noise (sieda_simulate_noise).
struct NoiseAnalysisResult: Decodable, Equatable {
    var ok: Bool = false
    var error: String = ""
    var output: String = ""
    var reference: String = ""
    var frequency: [Double] = []
    var outputDensity: [Double?] = []
    var outputRms: Double?
    var inputSource: String = ""
    var inputUnit: String = "V"
    var inputDensity: [Double?]?
    var gain: [Double?]?
    var inputRms: Double?
    var contributions: [NoiseContributionInfo] = []

    init(ok: Bool = false, error: String = "") {
        self.ok = ok
        self.error = error
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        output = try c.decodeIfPresent(String.self, forKey: .output) ?? ""
        reference = try c.decodeIfPresent(String.self, forKey: .reference) ?? ""
        frequency = try c.decodeIfPresent([Double].self, forKey: .frequency) ?? []
        outputDensity = try c.decodeIfPresent([Double?].self, forKey: .outputDensity) ?? []
        outputRms = try c.decodeIfPresent(Double.self, forKey: .outputRms)
        inputSource = try c.decodeIfPresent(String.self, forKey: .inputSource) ?? ""
        inputUnit = try c.decodeIfPresent(String.self, forKey: .inputUnit) ?? "V"
        inputDensity = try c.decodeIfPresent([Double?].self, forKey: .inputDensity)
        gain = try c.decodeIfPresent([Double?].self, forKey: .gain)
        inputRms = try c.decodeIfPresent(Double.self, forKey: .inputRms)
        contributions = try c.decodeIfPresent([NoiseContributionInfo].self, forKey: .contributions) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case ok, error, output, reference, frequency, outputDensity, outputRms, inputSource, inputUnit, inputDensity, gain,
             inputRms, contributions
    }
}

// MARK: - Parameter sweep

struct ParamSweepNet: Decodable, Equatable, Identifiable {
    var index: Int
    var name: String
    var voltage: Double?          // DC
    var values: [Double?]?        // transient (decimated)
    var magnitudeDb: [Double?]?   // AC
    var phaseDeg: [Double?]?
    var metrics: ACMetrics?

    var id: Int { index }
}

struct ParamSweepRunResult: Decodable, Equatable, Identifiable {
    var value: String
    var ok: Bool
    var error: String
    var x: [Double]?
    var nets: [ParamSweepNet]

    var id: String { value }
}

/// One analysis per value of a component (sieda_simulate_param_sweep).
struct ParamSweepResult: Decodable, Equatable {
    var ok: Bool = false
    var error: String = ""
    var component: String = ""
    var analysis: String = "dc"
    var runs: [ParamSweepRunResult] = []

    init(ok: Bool = false, error: String = "") {
        self.ok = ok
        self.error = error
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        component = try c.decodeIfPresent(String.self, forKey: .component) ?? ""
        analysis = try c.decodeIfPresent(String.self, forKey: .analysis) ?? "dc"
        runs = try c.decodeIfPresent([ParamSweepRunResult].self, forKey: .runs) ?? []
    }

    private enum CodingKeys: String, CodingKey { case ok, error, component, analysis, runs }
}

// MARK: - FFT / THD

struct HarmonicInfo: Decodable, Equatable, Identifiable {
    var order: Int
    var frequency: Double
    var amplitude: Double
    var dbc: Double?

    var id: Int { order }
}

/// Spectrum and harmonic distortion of a node's transient waveform (sieda_simulate_fft).
struct FFTResult: Decodable, Equatable {
    var ok: Bool = false
    var error: String = ""
    var net: String = ""
    var fundamentalHz: Double = 0
    var thdPercent: Double = 0
    var dc: Double = 0
    var cycles: Int = 0
    var windowStart: Double = 0
    var windowStop: Double = 0
    var frequency: [Double] = []
    var magnitudeDb: [Double?] = []
    var harmonics: [HarmonicInfo] = []

    init(ok: Bool = false, error: String = "") {
        self.ok = ok
        self.error = error
    }

    init(from decoder: Decoder) throws {
        let c = try decoder.container(keyedBy: CodingKeys.self)
        ok = try c.decodeIfPresent(Bool.self, forKey: .ok) ?? false
        error = try c.decodeIfPresent(String.self, forKey: .error) ?? ""
        net = try c.decodeIfPresent(String.self, forKey: .net) ?? ""
        fundamentalHz = try c.decodeIfPresent(Double.self, forKey: .fundamentalHz) ?? 0
        thdPercent = try c.decodeIfPresent(Double.self, forKey: .thdPercent) ?? 0
        dc = try c.decodeIfPresent(Double.self, forKey: .dc) ?? 0
        cycles = try c.decodeIfPresent(Int.self, forKey: .cycles) ?? 0
        windowStart = try c.decodeIfPresent(Double.self, forKey: .windowStart) ?? 0
        windowStop = try c.decodeIfPresent(Double.self, forKey: .windowStop) ?? 0
        frequency = try c.decodeIfPresent([Double].self, forKey: .frequency) ?? []
        magnitudeDb = try c.decodeIfPresent([Double?].self, forKey: .magnitudeDb) ?? []
        harmonics = try c.decodeIfPresent([HarmonicInfo].self, forKey: .harmonics) ?? []
    }

    private enum CodingKeys: String, CodingKey {
        case ok, error, net, fundamentalHz, thdPercent, dc, cycles, windowStart, windowStop, frequency, magnitudeDb, harmonics
    }
}

// MARK: - Waveform measurements

/// ".meas"-like measurements of one waveform over a window (sieda_measure_waveform); undefined quantities are nil.
struct WaveformMeasurementsInfo: Decodable, Equatable {
    var ok: Bool = false
    var error: String = ""
    var from: Double?
    var to: Double?
    var samples: Int?
    var min: Double?
    var max: Double?
    var peakToPeak: Double?
    var average: Double?
    var rms: Double?
    var acRms: Double?
    var stepLike: Bool?
    var riseTime: Double?
    var fallTime: Double?
    var overshootPercent: Double?
    var settlingTime: Double?
    var period: Double?
    var frequency: Double?
    var dutyCycle: Double?
    var cycles: Int?
}

/// Linear interpolation of a sampled waveform at `t` (clamped to its ends); nil when empty.
enum WaveformMath {
    static func value(at t: Double, time: [Double], values: [Double]) -> Double? {
        let n = min(time.count, values.count)
        guard n > 0 else { return nil }
        if t <= time[0] { return values[0] }
        if t >= time[n - 1] { return values[n - 1] }
        var lo = 0, hi = n - 1
        while hi - lo > 1 {
            let mid = (lo + hi) / 2
            if time[mid] <= t { lo = mid } else { hi = mid }
        }
        let span = time[hi] - time[lo]
        guard span > 0 else { return values[lo] }
        return values[lo] + (values[hi] - values[lo]) * (t - time[lo]) / span
    }
}
