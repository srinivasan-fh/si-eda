import Foundation

/// A finding of the footprint editor's land-pattern check (core `checkLandPattern`).
struct LandIssue: Decodable, Equatable, Identifiable {
    var severity: String  // "error", "warning"
    var code: String      // "LAND_OVERLAP", "LAND_GAP", "LAND_ANNULAR", "LAND_NO_PIN", "LAND_NO_PAD", "LAND_INVALID"
    var message: String
    var pads: [Int]       // 1-based pad numbers
    var id: String { code + message }
    var isError: Bool { severity == "error" }
}

/// The footprint editor's document: the pads of a land pattern (pad number = position + 1) and the body outline,
/// with the editing operations and an undo history. Pure value logic; the view draws it and the core checks it.
struct FootprintDraft: Equatable {
    struct Pad: Equatable, Identifiable {
        var id = UUID()
        var land: CustomPartSpec.Land

        static func == (a: Pad, b: Pad) -> Bool { a.id == b.id && a.land == b.land }
    }

    var pads: [Pad]
    var bodyW: Double
    var bodyD: Double
    /// "LGA" stays "LGA" (a catalog land pattern); anything edited from a generated footprint becomes "CUSTOM".
    var packageType: String

    /// Pads from a spec already on a land pattern (see `EDAEngine.landPattern`).
    init(spec: CustomPartSpec) {
        pads = (spec.package.lands ?? []).map { Pad(land: $0) }
        packageType = spec.package.usesLandPattern ? spec.package.type : "CUSTOM"
        let extentX = pads.map { abs($0.land.x) + $0.land.w / 2 }.max() ?? 1
        let extentY = pads.map { abs($0.land.y) + $0.land.h / 2 }.max() ?? 1
        bodyW = spec.package.bodySize ?? extentX * 2
        bodyD = spec.package.bodyDepth ?? spec.package.bodySize ?? extentY * 2
    }

    /// Writes the land pattern into the part: the package becomes the edited land pattern; the pins are kept.
    func apply(to spec: inout CustomPartSpec) {
        spec.package.type = packageType
        spec.package.lands = pads.map(\.land)
        spec.package.pinCount = pads.count
        spec.package.pitch = nil
        spec.package.bodySize = bodyW > 0.5 ? bodyW : nil
        spec.package.bodyDepth = bodyD > 0.5 ? bodyD : nil
    }

    /// The part with this footprint (for previews and the core's checks).
    func applied(to spec: CustomPartSpec) -> CustomPartSpec {
        var copy = spec
        apply(to: &copy)
        return copy
    }

    /// Pad number (1-based) of a pad.
    func number(of id: Pad.ID) -> Int? { pads.firstIndex { $0.id == id }.map { $0 + 1 } }

    /// The pin a pad connects to: its explicit pin, else its own number; nil for a mechanical pad.
    func pinNumber(of index: Int) -> String? {
        let pin = pads[index].land.pin
        if pin == "-" { return nil }
        return pin.isEmpty ? String(index + 1) : pin
    }

    static func snap(_ v: Double, _ grid: Double) -> Double {
        guard grid > 0 else { return v }
        return (v / grid).rounded() * grid
    }

    // MARK: - Editing

    /// Adds a pad like the last one (or a 1 × 0.6 mm SMD pad) at `point`, snapped. Returns its id.
    @discardableResult
    mutating func addPad(at point: CGPoint, grid: Double) -> Pad.ID {
        var land = pads.last?.land ?? CustomPartSpec.Land(x: 0, y: 0, w: 1.0, h: 0.6)
        land.x = Self.snap(point.x, grid)
        land.y = Self.snap(point.y, grid)
        land.pin = ""
        let pad = Pad(land: land)
        pads.append(pad)
        return pad.id
    }

    /// Copies pads (appended, numbered after the last), offset by `dx`, `dy`. Returns the copies' ids.
    @discardableResult
    mutating func duplicate(_ ids: Set<Pad.ID>, dx: Double, dy: Double) -> Set<Pad.ID> {
        var copies = Set<Pad.ID>()
        for pad in pads where ids.contains(pad.id) {
            var land = pad.land
            land.x += dx
            land.y += dy
            land.pin = land.pin == "-" ? "-" : ""
            let copy = Pad(land: land)
            pads.append(copy)
            copies.insert(copy.id)
        }
        return copies
    }

    /// Removes pads; the pads after them renumber down.
    mutating func delete(_ ids: Set<Pad.ID>) { pads.removeAll { ids.contains($0.id) } }

    /// Moves pads by `dx`, `dy`, then snaps each pad's centre to the grid.
    mutating func move(_ ids: Set<Pad.ID>, dx: Double, dy: Double, grid: Double) {
        for i in pads.indices where ids.contains(pads[i].id) {
            pads[i].land.x = Self.snap(pads[i].land.x + dx, grid)
            pads[i].land.y = Self.snap(pads[i].land.y + dy, grid)
        }
    }

    /// A row or column of `count` pads at `pitch` starting after the pad (numbered after the last pad), like a
    /// connector or an SOIC side. Returns the new ids.
    @discardableResult
    mutating func array(from id: Pad.ID, count: Int, pitch: Double, horizontal: Bool) -> Set<Pad.ID> {
        guard let start = pads.first(where: { $0.id == id })?.land, count > 0 else { return [] }
        var made = Set<Pad.ID>()
        for k in 1...count {
            var land = start
            land.pin = ""
            if horizontal { land.x += Double(k) * pitch } else { land.y += Double(k) * pitch }
            let pad = Pad(land: land)
            pads.append(pad)
            made.insert(pad.id)
        }
        return made
    }

    /// Mirrors pads left ↔ right about the footprint origin.
    mutating func mirrorX(_ ids: Set<Pad.ID>) {
        for i in pads.indices where ids.contains(pads[i].id) { pads[i].land.x = -pads[i].land.x }
    }

    /// Gives a pad a new number (1…N) by moving it in the order; the pads in between shift by one. Pads that follow
    /// their own number keep following it, so their pins move with the number.
    mutating func setNumber(of id: Pad.ID, to number: Int) {
        guard let from = pads.firstIndex(where: { $0.id == id }) else { return }
        let to = min(max(number, 1), pads.count) - 1
        guard to != from else { return }
        let pad = pads.remove(at: from)
        pads.insert(pad, at: to)
    }

    /// Re-centres the pads on the origin (the centre of their extent).
    mutating func centre() {
        guard !pads.isEmpty else { return }
        let minX = pads.map { $0.land.x - $0.land.w / 2 }.min()!, maxX = pads.map { $0.land.x + $0.land.w / 2 }.max()!
        let minY = pads.map { $0.land.y - $0.land.h / 2 }.min()!, maxY = pads.map { $0.land.y + $0.land.h / 2 }.max()!
        let cx = (minX + maxX) / 2, cy = (minY + maxY) / 2
        for i in pads.indices {
            pads[i].land.x -= cx
            pads[i].land.y -= cy
        }
    }

    /// Courtyard (pads and body plus 0.25 mm), mm.
    var courtyard: CGRect {
        var minX = -bodyW / 2, maxX = bodyW / 2, minY = -bodyD / 2, maxY = bodyD / 2
        for p in pads {
            minX = min(minX, p.land.x - p.land.w / 2)
            maxX = max(maxX, p.land.x + p.land.w / 2)
            minY = min(minY, p.land.y - p.land.h / 2)
            maxY = max(maxY, p.land.y + p.land.h / 2)
        }
        return CGRect(x: minX - 0.25, y: minY - 0.25, width: maxX - minX + 0.5, height: maxY - minY + 0.5)
    }

    /// Topmost pad under a point (mm), if any.
    func pad(at point: CGPoint) -> Pad.ID? {
        pads.last { p in
            let l = p.land
            return abs(point.x - l.x) <= l.w / 2 && abs(point.y - l.y) <= l.h / 2
        }?.id
    }
}

/// Undo / redo of footprint edits (snapshots of the draft).
struct FootprintHistory {
    private(set) var undoStack: [FootprintDraft] = []
    private(set) var redoStack: [FootprintDraft] = []
    static let limit = 200

    mutating func record(_ draft: FootprintDraft) {
        undoStack.append(draft)
        if undoStack.count > Self.limit { undoStack.removeFirst(undoStack.count - Self.limit) }
        redoStack.removeAll()
    }

    mutating func undo(_ current: inout FootprintDraft) -> Bool {
        guard let previous = undoStack.popLast() else { return false }
        redoStack.append(current)
        current = previous
        return true
    }

    mutating func redo(_ current: inout FootprintDraft) -> Bool {
        guard let next = redoStack.popLast() else { return false }
        undoStack.append(current)
        current = next
        return true
    }

    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }
}
