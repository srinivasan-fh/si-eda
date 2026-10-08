import CoreGraphics
import Foundation

/// Autorouter strategies (docs/ROUTING.md, Strategies): the board's options, the preset strategies, the one-shot
/// scoped routes (selected nets, a net class, an area) and the routing report.
extension DesignStore {
    /// The board's autorouter strategy (saved with the project).
    var autorouteOptions: AutorouteOptions { engine.autorouteOptions() }

    /// Changes the strategy as one undo step.
    func setAutorouteOptions(_ options: AutorouteOptions) {
        guard options != engine.autorouteOptions() else { return }
        perform("Routing strategy", invalidatesAnalysis: false) { $0.setAutorouteOptions(options) }
    }

    /// Switches to a preset strategy; the per-class layers, locked-copper protection and pin / gate swap are kept.
    func applyRoutingPreset(_ name: String) {
        guard let preset = EDAEngine.autoroutePresets().first(where: { $0.name == name }) else { return }
        let current = engine.autorouteOptions()
        var options = preset.options
        options.classLayers = current.classLayers
        options.protectLocked = current.protectLocked
        options.teardropStyle = current.teardropStyle
        options.pinSwap = current.pinSwap
        setAutorouteOptions(options)
    }

    /// ", swapped 2 pin pair(s) and 1 gate(s)" after a route that swapped pins or gates ("" otherwise).
    func swapSummary(_ stats: RouteStats) -> String {
        let pins = stats.pinSwaps ?? 0
        let gates = stats.gateSwaps ?? 0
        guard pins + gates > 0 else { return "" }
        return ", swapped \(pins) pin pair(s) and \(gates) gate(s)"
    }

    /// Flips one quality / strategy option.
    func toggleRoutingOption(_ keyPath: WritableKeyPath<AutorouteOptions, Bool>) {
        var options = engine.autorouteOptions()
        options[keyPath: keyPath].toggle()
        setAutorouteOptions(options)
    }

    /// Sets the outline of the autorouter's teardrops.
    func setRoutingTeardropStyle(_ style: TeardropStyleChoice) {
        var options = engine.autorouteOptions()
        options.teardropStyle = style
        setAutorouteOptions(options)
    }

    /// Nets of the selected tracks and of the pads of the selected parts (signal nets with two or more pins).
    var selectedRoutingNets: [String] {
        var indices = Set(snapshot.tracks.filter { selectedTracks.contains($0.id) }.map(\.net))
        for pad in snapshot.pads where selection.contains(pad.component) { indices.insert(pad.net) }
        let names = snapshot.nets.filter { indices.contains($0.index) && $0.pinCount >= 2 }.map(\.name)
        return names.sorted()
    }

    /// Routes only the selected nets; everything else keeps its copper. One undo step.
    func routeSelectedNets() async {
        let nets = selectedRoutingNets
        guard !nets.isEmpty else {
            statusMessage = "Route selected nets: select tracks or parts first"
            return
        }
        await routeScoped { $0.nets = nets }
    }

    /// Routes only the nets of one schematic net class.
    func routeNetClass(_ name: String) async {
        await routeScoped { $0.netClass = name }
    }

    /// Routes only the nets whose pads all lie in `rect` (board mm), inside it.
    func routeArea(_ rect: CGRect) async {
        await routeScoped {
            $0.hasArea = true
            $0.area = AutorouteArea(x0: rect.minX, y0: rect.minY, x1: rect.maxX, y1: rect.maxY)
        }
    }

    /// A one-shot scoped route: the scope applies to this route only, the board keeps its strategy.
    private func routeScoped(_ scope: (inout AutorouteOptions) -> Void) async {
        guard !isBusy else { return }
        let saved = engine.autorouteOptions()
        let undoState = engine.stateJSON()  // undo returns to the board's own strategy, not the one-shot scope
        var options = saved
        scope(&options)
        engine.setAutorouteOptions(options)
        await autoRoute(clearFirst: false, placeMissing: false, undoState: undoState)
        engine.setAutorouteOptions(saved)
    }

    /// The area around the selected parts' pads (3 mm margin), for the Area strategy; nil without a selection.
    var selectionRoutingArea: CGRect? {
        let rects = snapshot.pads.filter { selection.contains($0.component) }.map(\.rect)
        guard let first = rects.first else { return nil }
        return rects.dropFirst().reduce(first) { $0.union($1) }.insetBy(dx: -3, dy: -3)
    }

    /// The last autoroute's report.
    var routeReport: RouteReport { engine.routeReport() }
}
