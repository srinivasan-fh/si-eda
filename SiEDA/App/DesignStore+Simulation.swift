import AppKit
import Foundation
import UniformTypeIdentifiers

// Store actions of the simulation package: imported SPICE models (docs/SIMULATION.md).
extension DesignStore {
    /// Parts an imported SPICE model can replace the built-in model of (diodes, transistors, op-amps, ICs; not
    /// microcontrollers, which run firmware).
    static func acceptsSpiceModel(_ component: SnapComponent) -> Bool {
        switch component.componentKind {
        case .diode, .led, .npn, .nmos, .opAmp, .ic8: return true
        case .custom: return component.mcu == nil
        default: return false
        }
    }

    /// Attaches `model` of the vendor text to a part with its pin map ("" = default); an empty `text` removes it.
    /// Undoable; a model the core cannot use is refused with its reason.
    @discardableResult
    func setSpiceModel(_ id: Int, text: String, model: String, pins: String) -> Bool {
        guard let component = snapshot.component(id) else { return false }
        var failure: Error?
        let ok = performChecked(text.isEmpty ? "Removed the SPICE model of \(component.ref)" : "Attached SPICE model \(model) to \(component.ref)",
                                failureMessage: "\(component.ref): SPICE model not attached") { engine in
            do {
                try engine.setSpiceModel(id, text: text, model: model, pins: pins)
                return true
            } catch {
                failure = error
                return false
            }
        }
        if let failure { present(failure, title: "Could not attach the SPICE model") }
        return ok
    }

    // MARK: - Noise, parameter sweep, FFT

    func simulateNoise(output: String, start: String, stop: String, pointsPerDecade: Int, source: String) async {
        guard !isBusy else { return }  // one analysis at a time
        let engine = self.engine
        let result = await runBusy("Running noise analysis…", stoppable: true) {
            engine.simulateNoise(output: output, start: start, stop: stop, pointsPerDecade: pointsPerDecade, source: source)
        }
        noiseResult = result
        statusMessage = result.ok ? "Noise analysis: \(result.frequency.count) frequencies" : "Noise analysis failed: \(result.error)"
    }

    func simulateParamSweep(component: String, values: [String], analysis: String, net: String,
                            start: String, stop: String, step: String) async {
        guard !isBusy else { return }
        let engine = self.engine
        let result = await runBusy("Running parameter sweep…", stoppable: true) {
            engine.simulateParamSweep(component: component, values: values, analysis: analysis, net: net,
                                      start: start, stop: stop, step: step)
        }
        paramSweepResult = result
        statusMessage = result.ok ? "Parameter sweep: \(result.runs.count) values of \(component)" : "Parameter sweep failed: \(result.error)"
    }

    func simulateFFT(net: String, stop: String, step: String, fundamental: String, harmonics: Int) async {
        guard !isBusy else { return }
        let engine = self.engine
        let result = await runBusy("Running FFT…", stoppable: true) {
            engine.simulateFFT(net: net, stop: stop, step: step, fundamental: fundamental, harmonics: harmonics)
        }
        fftResult = result
        statusMessage = result.ok ? String(format: "FFT: THD %.3f %%", result.thdPercent) : "FFT failed: \(result.error)"
    }

    /// Asks for a vendor model file (.lib, .mod, .cir, .sub …) and returns its text; nil when cancelled.
    func chooseSpiceModelFile() -> String? {
        let panel = NSOpenPanel()
        let extensions = ["lib", "mod", "cir", "sub", "spi", "spice", "inc", "model", "txt", "ckt", "net"]
        panel.allowedContentTypes = extensions.compactMap { UTType(filenameExtension: $0) } + [.plainText, .data]
        panel.allowsMultipleSelection = false
        panel.message = "Choose a SPICE model file (.lib, .mod, .cir, .sub) from the part vendor."
        panel.prompt = "Open"
        guard panel.runModal() == .OK, let url = panel.url else { return nil }
        let scoped = url.startAccessingSecurityScopedResource()
        defer { if scoped { url.stopAccessingSecurityScopedResource() } }
        do {
            let data = try Data(contentsOf: url)
            guard data.count <= 8 << 20 else {
                present(EDAEngineError.operationFailed("\(url.lastPathComponent) is larger than 8 MB."),
                        title: "Could not read \(url.lastPathComponent)")
                return nil
            }
            // Vendor files are ASCII or Latin-1; invalid UTF-8 bytes become replacement characters.
            return EDAEngine.inlineSpiceIncludes(String(decoding: data, as: UTF8.self),
                                                 directory: url.deletingLastPathComponent().path)
        } catch {
            present(error, title: "Could not read \(url.lastPathComponent)")
            return nil
        }
    }
}
