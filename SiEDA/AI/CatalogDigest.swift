import Foundation

/// The slice of the standard catalog an AI prompt carries. The catalog holds over a thousand parts (many MCUs with
/// 100+ pins), far too many to list with their pins in every request: a prompt gets the parts the brief names or
/// matches best (with pins), a few staples every design may need, the parts the current plan already uses, and an
/// index of every category with its size. The same selection bounds the plan schema's list of part kinds.
enum CatalogDigest {
    /// Parts every design may reach for (regulators, timer, op-amp, logic, a common MCU), detailed in every prompt.
    static let staples = ["NE555", "LM7805", "AMS1117-3.3", "LM317", "LM358DR", "LM393DR", "74HC595D", "ATMEGA328P-AU",
                          "STM32F103C8T6", "ESP32-C3", "CH340C", "MCP2551-I/SN", "TL431AIDBZR", "2N7002", "AO3401A"]
    /// At most this many standard parts with their pins, and this many pins in all.
    static let maxDetailed = 40
    static let maxPins = 1800

    /// Lower-case words of a text (letters and digits, two characters or more).
    static func words(_ text: String) -> [String] {
        text.lowercased().split { !($0.isLetter || $0.isNumber || $0 == "-" || $0 == "." || $0 == "/" || $0 == "+") }
            .map(String.init).filter { $0.count >= 2 }
    }

    /// Upper-case letters and digits ("AMS1117-3.3" → "AMS111733"), as part numbers are matched.
    static func compact(_ s: String) -> String { s.uppercased().filter { $0.isLetter || $0.isNumber } }

    /// Relevance of a part to the brief: a part named in it ranks first, then matches in its description, category
    /// and package; staples get a small head start so short briefs still see them.
    static func score(_ part: StandardPart, briefWords: Set<String>, briefCompact: String) -> Int {
        var s = 0
        let name = compact(part.spec.name)
        if name.count >= 4, briefCompact.contains(name) { s += 100 }
        let text = Set(words(part.spec.description + " " + part.category + " " + part.spec.package.type))
        s += 3 * briefWords.intersection(text).count
        if staples.contains(part.spec.name) { s += 2 }
        return s
    }

    /// The standard parts to detail for `brief`: named or best matching first, staples, and `always` (parts a plan
    /// already uses), within the part and pin budgets.
    static func selection(_ parts: [StandardPart], brief: String, always: Set<String> = []) -> [StandardPart] {
        let briefWords = Set(words(brief))
        let briefCompact = compact(brief)
        let pinned = parts.filter { always.contains($0.spec.name.lowercased()) }
        let scored = parts.map { (part: $0, score: score($0, briefWords: briefWords, briefCompact: briefCompact)) }
            .filter { $0.score > 0 && !always.contains($0.part.spec.name.lowercased()) }
            .sorted { $0.score != $1.score ? $0.score > $1.score : $0.part.spec.name < $1.part.spec.name }
        var out = pinned
        var pins = pinned.reduce(0) { $0 + $1.spec.pins.count }
        for item in scored where out.count < maxDetailed + pinned.count {
            let n = item.part.spec.pins.count
            if pins + n > maxPins && item.score < 100 { continue }  // a part the brief names always fits
            out.append(item.part)
            pins += n
        }
        return out
    }

    /// "Regulators (112): AMS1117-3.3, LM317, …" for every category, a few names each.
    static func index(_ parts: [StandardPart], examples: Int = 6) -> String {
        var byCategory: [String: [String]] = [:]
        for p in parts { byCategory[p.category, default: []].append(p.spec.name) }
        return byCategory.keys.sorted().map { category in
            let names = byCategory[category] ?? []
            return "- \(category) (\(names.count)): \(names.prefix(examples).joined(separator: ", "))\(names.count > examples ? ", …" : "")"
        }.joined(separator: "\n")
    }
}
