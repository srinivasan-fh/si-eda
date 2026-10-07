import CoreGraphics
import Foundation

/// "Place new parts" after Update PCB (as Altium's component placement after an ECO, KiCad's footprints on the
/// cursor): the footprints the update put on the board, one at a time. The part's ghost follows the cursor, R turns
/// it, F flips it to the other side, a click places it on the 0.25 mm grid (one undo step per part) and Esc skips it,
/// leaving it where Auto Place put it (or, when a placed part now covers that spot, at its nearest free spot).
/// Parts still waiting in the queue are no obstacles: they stand at their automatic spots only until their turn.
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
    /// The parts after the current one: ignored as obstacles while it is placed.
    var waiting: [Int] { queue.dropFirst().map(\.id) }
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

    /// The current part's ghost starts at the free spot nearest the parts it connects to: placed parts are
    /// obstacles, the parts still waiting are not (so a part whose automatic spot is now taken starts elsewhere).
    private func preparePlacement() {
        guard var session = placementSession else { return }
        while let next = session.current, snapshot.component(next.id) == nil { session.queue.removeFirst() }
        guard let next = session.current else {
            finishPlacement(session)
            return
        }
        let suggestion = engine.suggestPlacement(next.id, ignore: session.waiting)
        let part = snapshot.component(next.id)
        session.rotation = suggestion?.rotation ?? part?.pcb.rotation ?? 0
        session.bottom = suggestion?.bottom ?? part?.pcb.bottom ?? false
        session.ghost = suggestion
        placementSession = session
        selection = [next.id]
        statusMessage = "Placing \(next.ref) (\(session.number) of \(session.total))"
    }

    /// The ghost follows the cursor (snapped, checked against placed courtyards, the outline and keep-outs).
    func movePlacementGhost(to point: CGPoint) {
        guard var session = placementSession, let part = session.current else { return }
        let rotation = session.rotation
        let bottom = session.bottom
        session.ghost = engine.checkPlacement(part.id, at: point, rotation: rotation, bottom: bottom,
                                              ignore: session.waiting)
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
    /// violation is reported. Parts still waiting do not block it. Returns whether the part moved.
    @discardableResult
    func placeCurrentPart(at point: CGPoint? = nil, force: Bool = false) -> Bool {
        guard let session = placementSession, let part = session.current else { return false }
        guard let target = point ?? session.ghost?.position else { return false }
        let waiting = session.waiting
        var result: PlacementCheckInfo?
        let placed = performChecked("Placed \(part.ref)", invalidatesAnalysis: false) { engine in
            result = engine.placeFootprint(part.id, at: target, rotation: session.rotation,
                                           bottom: session.bottom, force: force, ignore: waiting)
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

    /// Esc / Skip: the part keeps its automatic position, unless a placed part now overlaps it: then it goes to
    /// its nearest free spot (one undo step).
    func skipPlacement() {
        guard let session = placementSession, let part = session.current else { return }
        let waiting = session.waiting
        if overlapsPlacedPart(part.id, waiting: waiting, in: engine) {
            performChecked("Moved \(part.ref) to a free spot", invalidatesAnalysis: false) { engine in
                moveToFreeSpot(part.id, waiting: waiting, in: engine)
            }
        }
        advancePlacement(placed: false)
    }

    /// Skip All: every remaining part keeps its automatic position; those a placed part now overlaps go to their
    /// nearest free spots (one undo step for all).
    func skipAllPlacements() {
        guard let session = placementSession else { return }
        let ids = session.queue.map(\.id)
        performChecked("Moved skipped parts to free spots", invalidatesAnalysis: false) { engine in
            var moved = 0
            for (index, id) in ids.enumerated() {
                let waiting = Array(ids.dropFirst(index + 1))
                guard overlapsPlacedPart(id, waiting: waiting, in: engine) else { continue }
                if moveToFreeSpot(id, waiting: waiting, in: engine) { moved += 1 }
            }
            return moved > 0
        }
        finishPlacement(session)
    }

    /// Place All Automatically: every remaining part goes to its suggested spot (next to its connections), as one
    /// undo step. Each part avoids the parts placed before it; the ones after it still wait and do not block it.
    func placeAllAutomatically() {
        guard var session = placementSession else { return }
        let ids = session.queue.map(\.id)
        var moved = 0
        performChecked("Placed \(ids.count) part(s) automatically", invalidatesAnalysis: false) { engine in
            for (index, id) in ids.enumerated() {
                let waiting = Array(ids.dropFirst(index + 1))
                if moveToFreeSpot(id, waiting: waiting, in: engine) { moved += 1 }
            }
            return moved > 0
        }
        session.placed += moved
        session.queue = []
        finishPlacement(session)
    }

    /// Whether the part, where it stands, overlaps the courtyard of a placed part (`waiting` ones do not count).
    private func overlapsPlacedPart(_ id: Int, waiting: [Int], in engine: EDAEngine) -> Bool {
        guard let pcb = snapshot.component(id)?.pcb, pcb.placed else { return false }
        let here = CGPoint(x: pcb.x, y: pcb.y)
        let check = engine.checkPlacement(id, at: here, rotation: pcb.rotation, bottom: pcb.bottom, ignore: waiting)
        let issues = check?.issues ?? []
        return issues.contains { $0.code == "PLACE_OVERLAP" }
    }

    /// Moves the part to its suggested free spot (placed parts are obstacles, `waiting` ones are not).
    private func moveToFreeSpot(_ id: Int, waiting: [Int], in engine: EDAEngine) -> Bool {
        guard let spot = engine.suggestPlacement(id, ignore: waiting), spot.legal else { return false }
        let done = engine.placeFootprint(id, at: spot.position, rotation: spot.rotation,
                                         bottom: spot.bottom, ignore: waiting)
        return done?.committed == true
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
