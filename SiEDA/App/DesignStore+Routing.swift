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
}
