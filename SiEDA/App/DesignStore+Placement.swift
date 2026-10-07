import CoreGraphics
import Foundation

/// "Place new parts" after Update PCB (as Altium's component placement after an ECO, KiCad's footprints on the
/// cursor): the footprints the update put on the board, one at a time. The part's ghost follows the cursor, R turns
/// it, F flips it to the other side, a click places it on the 0.25 mm grid (one undo step per part) and Esc skips it,
/// leaving it where Auto Place put it.
struct PlacementSession: Equatable {
    /// Parts still to place, the current one first.
    var queue: [PcbEcoResult.Queued]
    var total: Int
    var placed = 0
    var rotation = 0
    var bottom = false
    /// The current part at the cursor (or its suggested spot), with what is wrong there.
    var ghost: PlacementCheckInfo?

    var current: PcbEcoResult.Queued? { queue.first }
    /// 1-based position of the current part in the whole queue.
    var number: Int { total - queue.count + 1 }
}

extension DesignStore {
    /// Preferences → PCB: "Place new parts interactively after Update PCB" (default on). Off: new parts keep the
    /// positions Auto Place gives them, as before.
    static let placeInteractivelyKey = "pcb.placeNewPartsInteractively"

    static var placesNewPartsInteractively: Bool {
        UserDefaults.standard.object(forKey: placeInteractivelyKey) as? Bool ?? true
    }

    /// Starts placing these parts by hand in the PCB editor (the ones still in the design).
    func beginPlacement(_ queue: [PcbEcoResult.Queued]) {
        let parts = queue.filter { snapshot.component($0.id) != nil }
        guard !parts.isEmpty else {
            placementSession = nil
            return
        }
        placementSession = PlacementSession(queue: parts, total: parts.count)
        workspace = .pcb
        preparePlacement()
    }

    /// The current part's ghost starts at the free spot nearest the parts it connects to.
    private func preparePlacement() {
        guard var session = placementSession else { return }
        while let next = session.current, snapshot.component(next.id) == nil { session.queue.removeFirst() }
        guard let next = session.current else {
            finishPlacement(session)
            return
        }
        let suggestion = engine.suggestPlacement(next.id)
        let part = snapshot.component(next.id)
        session.rotation = suggestion?.rotation ?? part?.pcb.rotation ?? 0
        session.bottom = suggestion?.bottom ?? part?.pcb.bottom ?? false
        session.ghost = suggestion
        placementSession = session
        selection = [next.id]
        statusMessage = "Placing \(next.ref) (\(session.number) of \(session.total))"
    }

    /// The ghost follows the cursor (snapped, checked against courtyards, the outline and keep-outs).
    func movePlacementGhost(to point: CGPoint) {
        guard var session = placementSession, let part = session.current else { return }
        session.ghost = engine.checkPlacement(part.id, at: point, rotation: session.rotation, bottom: session.bottom)
        placementSession = session
    }

    /// R: a quarter turn.
    func rotatePlacement() {
        guard var session = placementSession else { return }
        session.rotation = (session.rotation + 90) % 360
        placementSession = session
        recheckGhost()
    }

    /// F: to the other side of the board.
    func flipPlacement() {
        guard var session = placementSession else { return }
        session.bottom.toggle()
        placementSession = session
        recheckGhost()
    }

    private func recheckGhost() {
        guard let ghost = placementSession?.ghost else { return }
        movePlacementGhost(to: ghost.position)
    }

    /// Click: places the current part at `point` (or the ghost's spot), snapped, as one undo step, and goes on to
    /// the next. An illegal spot is refused with the reason, unless `force` (⌥-click) — then it is placed and the
    /// violation is reported. Returns whether the part moved.
    @discardableResult
    func placeCurrentPart(at point: CGPoint? = nil, force: Bool = false) -> Bool {
        guard let session = placementSession, let part = session.current else { return false }
        guard let target = point ?? session.ghost?.position else { return false }
        var result: PlacementCheckInfo?
        let placed = performChecked("Placed \(part.ref)", invalidatesAnalysis: false) { engine in
            result = engine.placeFootprint(part.id, at: target, rotation: session.rotation,
                                           bottom: session.bottom, force: force)
            return result?.committed == true
        }
        guard placed else {
            let reason = result?.firstMessage ?? "not possible"
            statusMessage = "\(part.ref) cannot be placed there: \(reason) (⌥-click places it anyway)"
            if let result { placementSession?.ghost = result }
            return false
        }
        advancePlacement(placed: true)
        if let result, !result.legal, let reason = result.firstMessage {
            statusMessage = "Placed \(part.ref) with a violation: \(reason)"  // ⌥-click
        }
        return true
    }

    /// Esc / Skip: the part keeps its automatic position.
    func skipPlacement() {
        guard placementSession != nil else { return }
        advancePlacement(placed: false)
    }

    /// Skip All: every remaining part keeps its automatic position.
    func skipAllPlacements() {
        guard let session = placementSession else { return }
        finishPlacement(session)
    }

    /// Place All Automatically: every remaining part goes to its suggested spot (next to its connections), as one
    /// undo step.
    func placeAllAutomatically() {
        guard var session = placementSession else { return }
        let parts = session.queue
        var moved = 0
        performChecked("Placed \(parts.count) part(s) automatically", invalidatesAnalysis: false) { engine in
            for part in parts {
                guard let spot = engine.suggestPlacement(part.id), spot.legal else { continue }
                let done = engine.placeFootprint(part.id, at: spot.position, rotation: spot.rotation, bottom: spot.bottom)
                if done?.committed == true { moved += 1 }
            }
            return moved > 0
        }
        session.placed += moved
        session.queue = []
        finishPlacement(session)
    }

    private func advancePlacement(placed: Bool) {
        guard var session = placementSession, !session.queue.isEmpty else { return }
        session.queue.removeFirst()
        if placed { session.placed += 1 }
        session.ghost = nil
        placementSession = session
        preparePlacement()
    }

    private func finishPlacement(_ session: PlacementSession) {
        placementSession = nil
        statusMessage = "Placed \(session.placed) of \(session.total) new part(s)"
    }
}
