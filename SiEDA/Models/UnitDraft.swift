import Foundation

/// The unit (gate) editor's document: the gates of a multi-unit part (a quad op-amp's A–D), the pins each gate
/// draws, which gates may be swapped for each other and which pins of a gate are interchangeable. Pins on no unit
/// form the power unit "P" the core adds; a pin on several units is shared (one pin, drawn on each). Pure value
/// logic; the core checks the result (`checkUnits`) and generates the unit symbols. Writes `CustomPartSpec.units`,
/// encoded exactly like the core's spec JSON ("name", "pins", "swap", "pinSwap").
struct UnitDraft: Equatable {
    var units: [CustomPartSpec.Unit]

    init(spec: CustomPartSpec) { units = spec.units ?? [] }

    init(units: [CustomPartSpec.Unit]) { self.units = units }

    var isMultiUnit: Bool { !units.isEmpty }

    // MARK: - Queries

    /// Indices of the units that draw a pin.
    func units(of number: String) -> [Int] { units.indices.filter { units[$0].pins.contains(number) } }

    /// Pins on no unit: the power unit the core generates.
    func powerPins(_ pins: [CustomPartSpec.Pin]) -> [String] {
        pins.map(\.number).filter { units(of: $0).isEmpty }
    }

    /// The next free unit name: A, B, … Z, then U27, U28 …
    var nextName: String {
        let used = Set(units.map(\.name))
        for scalar in UnicodeScalar("A").value...UnicodeScalar("Z").value {
            if let s = UnicodeScalar(scalar), !used.contains(String(Character(s))) { return String(Character(s)) }
        }
        var n = units.count + 1
        while used.contains("U\(n)") { n += 1 }
        return "U\(n)"
    }

    /// The pin-swap group of a unit that holds a pin, if any.
    func pinSwapGroup(of number: String, unit: Int) -> Int? {
        guard units.indices.contains(unit) else { return nil }
        return (units[unit].pinSwap ?? []).firstIndex { $0.contains(number) }
    }

    // MARK: - Editing

    mutating func addUnit(pins: [String] = []) {
        guard units.count < 32 else { return }
        units.append(CustomPartSpec.Unit(name: nextName, pins: pins))
    }

    mutating func removeUnit(at index: Int) {
        guard units.indices.contains(index) else { return }
        units.remove(at: index)
    }

    /// Renames a unit (1…8 characters, unique); false when the name cannot be used.
    @discardableResult
    mutating func rename(_ index: Int, to raw: String) -> Bool {
        let name = raw.trimmingCharacters(in: .whitespacesAndNewlines)
        guard units.indices.contains(index), !name.isEmpty, name.count <= 8,
              !units.enumerated().contains(where: { $0.offset != index && $0.element.name == name }) else { return false }
        units[index].name = name
        return true
    }

    /// Adds the pin to the unit, or takes it off (and out of the unit's pin-swap groups).
    mutating func toggle(_ number: String, unit: Int) {
        guard units.indices.contains(unit) else { return }
        if units[unit].pins.contains(number) {
            remove([number], from: unit)
        } else {
            units[unit].pins.append(number)
        }
    }

    /// Puts the pins on one unit only.
    mutating func assign(_ numbers: [String], to unit: Int) {
        guard units.indices.contains(unit) else { return }
        for i in units.indices where i != unit { remove(numbers, from: i) }
        for n in numbers where !units[unit].pins.contains(n) { units[unit].pins.append(n) }
    }

    /// Takes the pins off every unit: they go to the power unit P.
    mutating func makePower(_ numbers: [String]) {
        for i in units.indices { remove(numbers, from: i) }
    }

    /// Draws the pins on every unit (shared pins: one pin, drawn on each gate).
    mutating func share(_ numbers: [String]) {
        for i in units.indices {
            for n in numbers where !units[i].pins.contains(n) { units[i].pins.append(n) }
        }
    }

    /// Gate swapping: nil / 0 automatic, n > 0 a swap group, -1 never.
    mutating func setSwap(_ value: Int?, unit: Int) {
        guard units.indices.contains(unit) else { return }
        let v = value.map { max(-1, min(999, $0)) }
        units[unit].swap = v == 0 ? nil : v
    }

    /// Makes the given pins of a unit one pin-swap group (taken out of any other group of the unit).
    mutating func makePinSwapGroup(_ numbers: [String], unit: Int) {
        guard units.indices.contains(unit) else { return }
        let members = numbers.filter { units[unit].pins.contains($0) }
        guard members.count >= 2 else { return }
        var groups = (units[unit].pinSwap ?? []).map { $0.filter { !members.contains($0) } }.filter { $0.count >= 2 }
        groups.append(members)
        units[unit].pinSwap = groups
    }

    mutating func clearPinSwap(unit: Int) {
        guard units.indices.contains(unit) else { return }
        units[unit].pinSwap = nil
    }

    /// Splits pins into gates from their names: "1A, 1B, 1Y, 2A …" (gate number first) or "OUT1, IN1-, IN1+, OUT2 …"
    /// (gate number last). Supply and no-connect pins stay on the power unit. Nil when the names show no gates.
    static func detectGates(_ pins: [CustomPartSpec.Pin]) -> [CustomPartSpec.Unit]? {
        func gateNumber(_ name: String) -> Int? {
            let upper = name.uppercased()
            let leading = upper.prefix { $0.isNumber }
            if !leading.isEmpty, leading.count < upper.count, upper.dropFirst(leading.count).first?.isLetter == true {
                return Int(leading)
            }
            let core = upper.trimmingCharacters(in: CharacterSet(charactersIn: "+-_"))
            let trailing = core.reversed().prefix { $0.isNumber }
            if !trailing.isEmpty, trailing.count < core.count { return Int(String(trailing.reversed())) }
            return nil
        }
        var gates: [Int: [String]] = [:]
        for pin in pins where pin.type != .powerIn && pin.type != .powerOut && pin.type != .noConnect {
            guard let g = gateNumber(pin.name) else { continue }
            gates[g, default: []].append(pin.number)
        }
        let ordered = gates.keys.sorted()
        guard ordered.count >= 2, ordered.count <= 26, ordered.allSatisfy({ (gates[$0]?.count ?? 0) >= 2 }) else { return nil }
        var draft = UnitDraft(units: [])
        for g in ordered { draft.addUnit(pins: gates[g] ?? []) }
        return draft.units
    }

    /// Writes the units into the part (nil when there are none: a one-symbol part).
    func apply(to spec: inout CustomPartSpec) {
        let cleaned = units.map { unit -> CustomPartSpec.Unit in
            var u = unit
            if u.swap == 0 { u.swap = nil }
            if let groups = u.pinSwap {
                let kept = groups.map { $0.filter { u.pins.contains($0) } }.filter { $0.count >= 2 }
                u.pinSwap = kept.isEmpty ? nil : kept
            }
            return u
        }
        spec.units = cleaned.isEmpty ? nil : cleaned
    }

    func applied(to spec: CustomPartSpec) -> CustomPartSpec {
        var copy = spec
        apply(to: &copy)
        return copy
    }

    private mutating func remove(_ numbers: [String], from unit: Int) {
        units[unit].pins.removeAll { numbers.contains($0) }
        if let groups = units[unit].pinSwap {
            let kept = groups.map { $0.filter { !numbers.contains($0) } }.filter { $0.count >= 2 }
            units[unit].pinSwap = kept.isEmpty ? nil : kept
        }
    }
}
