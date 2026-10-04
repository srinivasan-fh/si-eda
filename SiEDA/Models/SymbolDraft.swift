import Foundation

/// A finding of the symbol editor's check (core `checkSymbol`).
struct SymbolIssue: Decodable, Equatable, Identifiable {
    var severity: String  // "error", "warning", "info"
    var code: String      // "SYM_MISSING", "SYM_UNKNOWN", "SYM_DUPLICATE", "SYM_OVERLAP", "SYM_STACK", "SYM_STACK_SIGNAL", "SYM_INVALID"
    var message: String
    var pins: [String]    // pin numbers
    var id: String { code + message }
    var isError: Bool { severity == "error" }
}

/// The symbol editor's document: where each pin of a part sits on its schematic symbol (a side and a slot along
/// it), and the body width. Pins on the same spot are stacked (drawn once, joined into one net). Pure value logic;
/// the core lays the symbol out and checks it.
struct SymbolDraft: Equatable {
    enum Side: String, CaseIterable, Identifiable {
        case left = "L", right = "R", top = "T", bottom = "B"
        var id: String { rawValue }
        var title: String {
            switch self {
            case .left: return "Left"
            case .right: return "Right"
            case .top: return "Top"
            case .bottom: return "Bottom"
            }
        }
    }

    struct Placement: Equatable {
        var number: String
        var side: Side
        var slot: Int
    }

    /// One placement per pin of the part.
    var placements: [Placement]
    /// Body width in schematic units (0 = from the pin names).
    var width: Double

    /// The part's layout, or the generated one (pins in number order down the left, then up the right).
    init(spec: CustomPartSpec) {
        width = spec.symbolLayout?.width ?? 0
        if let layout = spec.symbolLayout, !layout.pins.isEmpty {
            placements = layout.pins.map { Placement(number: $0.number, side: Side(rawValue: $0.side) ?? .left, slot: $0.slot) }
        } else {
            placements = Self.generated(spec.pins)
        }
    }

    /// The core's generated layout: number order, the first half down the left, the rest up the right.
    static func generated(_ pins: [CustomPartSpec.Pin]) -> [Placement] {
        let order = pins.indices.sorted { a, b in
            let na = Int(pins[a].number) ?? Int.max, nb = Int(pins[b].number) ?? Int.max
            return na == nb ? a < b : na < nb
        }
        let leftCount = (pins.count + 1) / 2
        var out = [Placement](repeating: Placement(number: "", side: .left, slot: 0), count: pins.count)
        for (k, index) in order.enumerated() {
            let left = k < leftCount
            out[index] = Placement(number: pins[index].number, side: left ? .left : .right, slot: left ? k : pins.count - 1 - k)
        }
        return out
    }

    var layout: CustomPartSpec.SymbolLayout {
        CustomPartSpec.SymbolLayout(width: width > 0 ? width : nil,
                                    pins: placements.map { .init(number: $0.number, side: $0.side.rawValue, slot: $0.slot) })
    }

    /// Writes the layout into the part (the pins themselves are untouched).
    func apply(to spec: inout CustomPartSpec) { spec.symbolLayout = layout }

    func applied(to spec: CustomPartSpec) -> CustomPartSpec {
        var copy = spec
        apply(to: &copy)
        return copy
    }

    // MARK: - Queries

    func placement(of number: String) -> Placement? { placements.first { $0.number == number } }

    /// Number of slots on a side (highest slot + 1).
    func slotCount(_ side: Side) -> Int { (placements.filter { $0.side == side }.map(\.slot).max() ?? -1) + 1 }

    /// The pins on each slot of a side, top to bottom (L / R) or left to right (T / B); empty slots are gaps.
    func slots(_ side: Side) -> [[String]] {
        var out = [[String]](repeating: [], count: slotCount(side))
        for p in placements where p.side == side { out[p.slot].append(p.number) }
        return out
    }

    /// Pins sharing a spot with this one (itself included).
    func stack(of number: String) -> [String] {
        guard let p = placement(of: number) else { return [] }
        return placements.filter { $0.side == p.side && $0.slot == p.slot }.map(\.number)
    }

    // MARK: - Editing

    /// Moves pins to a side, one per slot from `slot` on; pins already there from `slot` down shift to make room (a
    /// stack moves as one when all its pins are moved). The slots the pins leave become gaps (see `closeGaps`).
    mutating func move(_ numbers: [String], to side: Side, slot: Int) {
        let moving = numbers.filter { n in placements.contains { $0.number == n } }
        guard !moving.isEmpty else { return }
        // Keep stacks together: pins that shared a spot land on one slot.
        var groups: [[String]] = []
        var seen = Set<String>()
        for n in moving where !seen.contains(n) {
            let together = stack(of: n).filter { moving.contains($0) }
            together.forEach { seen.insert($0) }
            groups.append(together)
        }
        let start = max(0, slot)
        let movingSet = Set(moving)
        for i in placements.indices where !movingSet.contains(placements[i].number) && placements[i].side == side
            && placements[i].slot >= start {
            placements[i].slot += groups.count
        }
        for (k, group) in groups.enumerated() {
            for i in placements.indices where group.contains(placements[i].number) {
                placements[i].side = side
                placements[i].slot = start + k
            }
        }
    }

    /// Puts pins on the first one's spot: drawn once and joined into one net (for repeated VDD / GND pins).
    mutating func stack(_ numbers: [String]) {
        guard let first = numbers.first, let target = placement(of: first) else { return }
        for i in placements.indices where numbers.contains(placements[i].number) {
            placements[i].side = target.side
            placements[i].slot = target.slot
        }
    }

    /// Takes a pin out of its stack onto its own slot right after the stack.
    mutating func unstack(_ number: String) {
        guard let p = placement(of: number), stack(of: number).count > 1 else { return }
        move([number], to: p.side, slot: p.slot + 1)
    }

    /// Inserts an empty slot (a gap between groups) on a side before `slot`.
    mutating func insertGap(_ side: Side, at slot: Int) {
        for i in placements.indices where placements[i].side == side && placements[i].slot >= slot { placements[i].slot += 1 }
    }

    /// Removes empty slots on a side (all sides when nil), keeping the order.
    mutating func closeGaps(_ side: Side? = nil) {
        for s in Side.allCases where side == nil || side == s {
            let used = Set(placements.filter { $0.side == s }.map(\.slot)).sorted()
            let remap = Dictionary(uniqueKeysWithValues: used.enumerated().map { ($0.element, $0.offset) })
            for i in placements.indices where placements[i].side == s { placements[i].slot = remap[placements[i].slot] ?? 0 }
        }
    }

    /// Swaps the spots of two pins (and of their stacks).
    mutating func swap(_ a: String, _ b: String) {
        guard let pa = placement(of: a), let pb = placement(of: b) else { return }
        let stackA = Set(stack(of: a)), stackB = Set(stack(of: b))
        for i in placements.indices {
            if stackA.contains(placements[i].number) {
                placements[i].side = pb.side
                placements[i].slot = pb.slot
            } else if stackB.contains(placements[i].number) {
                placements[i].side = pa.side
                placements[i].slot = pa.slot
            }
        }
    }

    /// Moves a pin (with its stack) one slot along its side: into an empty slot, or swapping with the pin there.
    /// Slot 0 is the first; moving past the last slot appends.
    mutating func nudge(_ number: String, by delta: Int) {
        guard let p = placement(of: number), delta != 0 else { return }
        let target = p.slot + delta
        guard target >= 0 else { return }
        let occupants = placements.filter { $0.side == p.side && $0.slot == target }.map(\.number)
        if let other = occupants.first {
            swap(number, other)
        } else {
            let stackNumbers = Set(stack(of: number))
            for i in placements.indices where stackNumbers.contains(placements[i].number) { placements[i].slot = target }
        }
    }

    /// Mirrors left ↔ right (and keeps top / bottom), e.g. to make signals flow the other way.
    mutating func mirror() {
        for i in placements.indices {
            switch placements[i].side {
            case .left: placements[i].side = .right
            case .right: placements[i].side = .left
            default: break
            }
        }
    }

    /// Keeps the layout in step with the part's pins: drops pins the part no longer has and puts new pins at the end
    /// of the shorter side. Returns true when something changed.
    @discardableResult
    mutating func reconcile(with pins: [CustomPartSpec.Pin]) -> Bool {
        let numbers = Set(pins.map(\.number))
        let before = placements
        placements.removeAll { !numbers.contains($0.number) }
        var seen = Set<String>()
        placements.removeAll { !seen.insert($0.number).inserted }
        for pin in pins where !placements.contains(where: { $0.number == pin.number }) {
            let side: Side = slotCount(.left) <= slotCount(.right) ? .left : .right
            placements.append(Placement(number: pin.number, side: side, slot: slotCount(side)))
        }
        return placements != before
    }
}
