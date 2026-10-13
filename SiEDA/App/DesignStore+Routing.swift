import CoreGraphics
import Foundation

/// Track selection and the track commands of the PCB editor (docs/INTERACTIVE_ROUTING.md): convert corners to arcs
/// and the other routing commands. Each command is one undo step.
extension DesignStore {
    /// Select tool click on a track: selects it (⇧ adds or removes it); nil clears the track selection.
    func selectTrack(_ id: Int?, extend: Bool = false) {
        guard let id else {
            if !extend { selectedTracks = [] }
            return
        }
        if extend {
            if selectedTracks.contains(id) { selectedTracks.remove(id) } else { selectedTracks.insert(id) }
        } else {
            selectedTracks = [id]
        }
        selection = []
    }

    /// Deletes the selected tracks, or the given `vias` instead. One undo step.
    func deleteSelectedCopper(vias: [Int] = []) {
        let tracks = vias.isEmpty ? Array(selectedTracks) : []
        guard !tracks.isEmpty || !vias.isEmpty else { return }
        if routePreview != nil { cancelRoute() }
        perform(vias.isEmpty ? "Deleted \(tracks.count) track(s)" : "Deleted copper", invalidatesAnalysis: false) { engine in
            tracks.forEach { engine.removeTrack($0) }
            vias.forEach { engine.removeVia($0) }
        }
        if vias.isEmpty { selectedTracks = [] }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Splits a straight track in two at the point nearest `point` (a new corner to drag).
    func splitTrack(_ id: Int, at point: CGPoint) {
        var new: Int?
        performChecked("Split track", invalidatesAnalysis: false,
                       failureMessage: "Split: only a straight track can be split, away from its ends") {
            new = $0.splitTrack(id, at: point)
            return new != nil
        }
        if let new { selectedTracks = [id, new] }
    }

    /// Joins the selected tracks (all when none is selected) that continue each other in a straight line.
    func mergeTracks() {
        let ids = selectedTracks.isEmpty ? nil : Array(selectedTracks).sorted()
        var joins = 0
        performChecked("Merged tracks", invalidatesAnalysis: false,
                       failureMessage: "Merge: no straight joints between tracks of one net, layer and width") {
            joins = $0.mergeTracks(ids)
            return joins > 0
        }
        if joins > 0 {
            selectedTracks = []
            statusMessage = "Merged \(joins) joint(s)"
        }
    }

    /// Goes to the next (`delta` 1) or previous (-1) located design rule error: rings it and zooms the board to it.
    func stepDRC(_ delta: Int) {
        let located = drcResults.filter { $0.severity == .error && $0.hasLocation }
        guard !located.isEmpty else { return }
        let at = located.firstIndex { $0.id == focusedViolation }.map { ($0 + delta + located.count) % located.count }
            ?? (delta > 0 ? 0 : located.count - 1)
        let v = located[at]
        focusedViolation = v.id
        if v.components.count == 1 { selection = Set(v.components) }
        requestView(.focus(CGPoint(x: v.x, y: v.y)))
        statusMessage = "\(at + 1) of \(located.count): \(v.message)"
    }

    /// Selects every track of the selected tracks' nets (the whole route), so a command covers it.
    func selectTrackNets() {
        let nets = Set(snapshot.tracks.filter { selectedTracks.contains($0.id) }.map(\.net))
        guard !nets.isEmpty else { return }
        selectedTracks = Set(snapshot.tracks.filter { nets.contains($0.net) }.map(\.id))
    }

    /// The selected tracks, or with none selected every track of the board.
    var commandTracks: [Int] {
        selectedTracks.isEmpty ? snapshot.tracks.map(\.id) : Array(selectedTracks).sorted()
    }

    /// Sets (target > 0) or removes a net's length rule (target ± tolerance mm, pad to pad). Undoable.
    func setLengthRule(net: String, target: Double, tolerance: Double) {
        guard !net.isEmpty, target.isFinite, tolerance.isFinite else { return }
        perform(target > 0 ? "Set length rule" : "Removed length rule", invalidatesAnalysis: false) {
            _ = $0.setLengthRule(net: net, target: max(0, target), tolerance: max(0, tolerance))
        }
        if tuneSession != nil { setTuneTarget(tuneSession?.target ?? 0) }
    }

    /// A length rule as a delay: delayPs ± tolerancePs, converted to mm on the outer layer (0 removes it). Undoable.
    func setDelayRule(net: String, delayPs: Double, tolerancePs: Double) {
        guard !net.isEmpty, delayPs.isFinite, tolerancePs.isFinite else { return }
        perform(delayPs > 0 ? "Set delay rule" : "Removed length rule", invalidatesAnalysis: false) {
            _ = $0.setDelayRule(net: net, delayPs: max(0, delayPs), tolerancePs: max(0, tolerancePs))
        }
    }

    /// Sets a differential pair's rule (an interface preset and / or impedance); no values removes it. Undoable.
    func setPairRule(_ rule: PairRuleInfo) {
        perform("Pair rule for \(rule.pair)", invalidatesAnalysis: false) { _ = $0.setPairRule(rule) }
        if !drcResults.isEmpty { runDRC() }
    }

    /// Sets or (fewer than two nets) removes a match group. Undoable.
    func setMatchGroup(name: String, nets: [String], tolerance: Double) {
        let name = name.trimmingCharacters(in: .whitespaces)
        guard !name.isEmpty, tolerance.isFinite else { return }
        perform(nets.count >= 2 ? "Set match group" : "Removed match group", invalidatesAnalysis: false) {
            _ = $0.setMatchGroup(name: name, nets: nets, tolerance: max(0, tolerance))
        }
        if tuneSession != nil { setTuneTarget(tuneSession?.target ?? 0) }
    }

    /// Converts the corners between the selected tracks (all tracks when none is selected) to true arcs.
    func convertCornersToArcs() {
        let tracks = commandTracks
        guard !tracks.isEmpty else {
            statusMessage = "Convert corners to arcs: there are no tracks"
            return
        }
        if routePreview != nil { cancelRoute() }
        var result: ArcCornersResult?
        let done = performChecked("Convert corners to arcs", invalidatesAnalysis: false,
                                  failureMessage: "Convert corners to arcs: no corner had room for an arc") {
            result = $0.arcCorners(tracks: tracks)
            return result?.applied == true
        }
        if let result { statusMessage = result.message }
        if done {
            selectedTracks = []
            if !drcResults.isEmpty { runDRC() }
        }
    }

    /// Teardrops on the selected tracks (all tracks when none is selected); when they already have teardrops,
    /// removes them instead.
    func toggleTeardrops() {
        let tracks = commandTracks
        guard !tracks.isEmpty else {
            statusMessage = "Teardrops: there are no tracks"
            return
        }
        let ids = selectedTracks.isEmpty ? [] : tracks
        let remove = ids.isEmpty ? snapshot.tracks.contains { $0.teardrop == true } : hasTeardrops(on: Set(ids))
        runBoardCommand(remove ? "Removed teardrops" : "Added teardrops",
                        failure: remove ? "Teardrops: none to remove" : "Teardrops: no pad or via end had room") {
            $0.teardrops(tracks: ids, remove: remove, style: teardropStyle)
        }
    }

    /// Via stitching of the ground net wherever its pours or planes overlap on two or more layers.
    func stitchVias() {
        runBoardCommand("Via stitching", failure: "Via stitching: needs ground pours or planes on two layers") {
            $0.stitchVias()
        }
    }

    /// Via shielding: ground vias on both sides of the selected tracks.
    func shieldSelectedTracks() {
        guard !selectedTracks.isEmpty else {
            statusMessage = "Via shielding: select the tracks to shield first"
            return
        }
        let tracks = Array(selectedTracks).sorted()
        runBoardCommand("Via shielding", failure: "Via shielding: no room for vias beside the tracks") {
            $0.shieldTracks(tracks)
        }
    }

    /// Glossing: pulls the selected routes (all tracks when none is selected) tight.
    func glossTracks() {
        let tracks = commandTracks
        guard !tracks.isEmpty else {
            statusMessage = "Gloss: there are no tracks"
            return
        }
        runBoardCommand("Gloss", failure: "Gloss: the routes are already as tight as the board allows") {
            $0.gloss(tracks: tracks)
        }
    }

    /// Teardrops whose track end lies on one of `tracks` (a teardrop ends on the track it widens).
    private func hasTeardrops(on tracks: Set<Int>) -> Bool {
        let owners = snapshot.tracks.filter { tracks.contains($0.id) && $0.teardrop != true }
        return snapshot.tracks.contains { drop in
            guard drop.teardrop == true else { return false }
            return owners.contains { $0.net == drop.net && $0.layer == drop.layer
                && $0.distance(to: CGPoint(x: drop.bx, y: drop.by)) <= 1e-4 }
        }
    }

    /// Runs one board command as one undo step and reports its message.
    private func runBoardCommand(_ action: String, failure: String, _ body: (EDAEngine) -> BoardEditResult?) {
        if routePreview != nil { cancelRoute() }
        var result: BoardEditResult?
        let done = performChecked(action, invalidatesAnalysis: false, failureMessage: failure) {
            result = body($0)
            return result?.applied == true
        }
        if let result, !result.message.isEmpty { statusMessage = result.message }
        if done, !drcResults.isEmpty { runDRC() }
    }

    /// Matches the lengths of the selected tracks' nets (a routed bus) to the longest of them. One undo step.
    func matchSelectedLengths() {
        let tracks = Array(selectedTracks).sorted()
        guard !tracks.isEmpty else {
            statusMessage = "Match lengths: select a track of each net (⇧-click)"
            return
        }
        if routePreview != nil { cancelRoute() }
        let style = tuneStyle, corner = tuneCorner
        var result: MatchLengthsResult?
        _ = performChecked("Match lengths", invalidatesAnalysis: false,
                           failureMessage: "Match lengths: no net needed or had room for meanders") {
            result = $0.matchLengths(tracks: tracks, style: style, corner: corner)
            return result?.ok == true
        }
        if let result { statusMessage = result.message }
        if !drcResults.isEmpty { runDRC() }
    }
}
