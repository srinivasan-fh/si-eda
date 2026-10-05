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
