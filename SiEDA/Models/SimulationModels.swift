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
