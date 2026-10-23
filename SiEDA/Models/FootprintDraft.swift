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
    /// Courtyard margin around pads and body (mm).
    var courtyardMargin = 0.25
    /// Drawn silkscreen / fab lines.
    var graphics: [CustomPartSpec.Graphic] = []
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
        courtyardMargin = spec.package.courtyard ?? 0.25
        graphics = spec.package.graphics ?? []
    }

    /// Writes the land pattern into the part: the package becomes the edited land pattern; the pins are kept.
    func apply(to spec: inout CustomPartSpec) {
        spec.package.type = packageType
        spec.package.lands = pads.map(\.land)
        spec.package.pinCount = pads.count
        spec.package.pitch = nil
        spec.package.bodySize = bodyW > 0.5 ? bodyW : nil
        spec.package.bodyDepth = bodyD > 0.5 ? bodyD : nil
        spec.package.courtyard = abs(courtyardMargin - 0.25) > 1e-9 ? courtyardMargin : nil
        spec.package.graphics = graphics.isEmpty ? nil : graphics
    }

    /// The body's outline as four lines on the silkscreen (0.1 mm outside it, clear of the pads' mask) or the fab layer.
    mutating func outlineBody(fab: Bool) {
        let gap = fab ? 0 : 0.1, x = bodyW / 2 + gap, y = bodyD / 2 + gap
        let corners = [(-x, -y), (x, -y), (x, y), (-x, y)]
        for i in 0..<4 {
            let (a, b) = (corners[i], corners[(i + 1) % 4])
            graphics.append(.init(x0: a.0, y0: a.1, x1: b.0, y1: b.1, width: fab ? 0.1 : 0.15, fab: fab))
        }
    }

    /// A short silkscreen bar beside pad 1 on its inner side (towards the centre: inside the courtyard, 0.3 mm clear
    /// of the pad, so the silk-on-pad check stays quiet).
    mutating func addPin1Mark() {
        guard let p = pads.first?.land else { return }
        let alongX = abs(p.x) >= abs(p.y)
        let sx: Double = p.x > 0 ? 1 : -1, sy: Double = p.y > 0 ? 1 : -1
        let c = alongX ? CGPoint(x: p.x - sx * (p.w / 2 + 0.4), y: p.y) : CGPoint(x: p.x, y: p.y - sy * (p.h / 2 + 0.4))
        let half = 0.15  // across the line to the centre
        graphics.append(alongX ? .init(x0: c.x, y0: c.y - half, x1: c.x, y1: c.y + half, width: 0.2)
                               : .init(x0: c.x - half, y0: c.y, x1: c.x + half, y1: c.y, width: 0.2))
    }

    /// Pads a selection filter lets through: "smd", "tht" (drilled), "mechanical" (no pin or a plain hole), else all.
    func passes(_ pad: Pad, filter: String) -> Bool {
        let l = pad.land
        switch filter {
        case "smd": return l.drill == 0
        case "tht": return l.drill > 0 && l.options.npth != true
        case "mechanical": return l.pin == "-" || l.options.npth == true
        default: return true
        }
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

    /// Turns pads 90° clockwise about the centre of the selection (positions turn, width and height swap).
    mutating func rotate(_ ids: Set<Pad.ID>) {
        let chosen = pads.filter { ids.contains($0.id) }
        guard !chosen.isEmpty else { return }
        let cx = (chosen.map(\.land.x).min()! + chosen.map(\.land.x).max()!) / 2
        let cy = (chosen.map(\.land.y).min()! + chosen.map(\.land.y).max()!) / 2
        for i in pads.indices where ids.contains(pads[i].id) {
            let l = pads[i].land
            pads[i].land.x = cx - (l.y - cy)
            pads[i].land.y = cy + (l.x - cx)
            pads[i].land.w = l.h
            pads[i].land.h = l.w
            pads[i].land.options.outline = l.options.outline?.map { [-$0[1], $0[0]] }  // a quarter turn, like the pad
        }
    }

    /// Pad outline presets: a trapezoid (top edge `top` × the width, y down) or the pad's rectangle to edit.
    static func trapezoid(_ l: CustomPartSpec.Land, top: Double = 0.6) -> [[Double]] {
        [[-l.w / 2, l.h / 2], [l.w / 2, l.h / 2], [l.w * top / 2, -l.h / 2], [-l.w * top / 2, -l.h / 2]]
    }

    /// Thermal vias in a pad (an exposed / thermal pad): plated holes of `drill` on a `pitch` grid inside it, on the
    /// pad's pin. Returns the new pads' ids (none when the pad is too small for one).
    @discardableResult
    mutating func addThermalVias(to id: Pad.ID, pitch: Double = 1.0, drill: Double = 0.3) -> Set<Pad.ID> {
        guard let index = pads.firstIndex(where: { $0.id == id }), let pin = pinNumber(of: index), pitch > 0 else { return [] }
        let l = pads[index].land, ring = drill + 0.3
        let nx = Int(((l.w - ring) / pitch).rounded(.down)) + 1, ny = Int(((l.h - ring) / pitch).rounded(.down)) + 1
        guard l.w >= ring, l.h >= ring, nx * ny <= 100 else { return [] }
        var made = Set<Pad.ID>()
        for i in 0..<nx {
            for j in 0..<ny {
                let via = CustomPartSpec.Land(x: l.x + (Double(i) - Double(nx - 1) / 2) * pitch,
                                              y: l.y + (Double(j) - Double(ny - 1) / 2) * pitch,
                                              w: ring, h: ring, drill: drill, round: true, pin: pin)
                let pad = Pad(land: via)
                pads.append(pad)
                made.insert(pad.id)
            }
        }
        return made
    }

    /// Moves the footprint origin to `point` (pads shift by −point).
    mutating func setOrigin(_ point: CGPoint) {
        for i in pads.indices {
            pads[i].land.x -= point.x
            pads[i].land.y -= point.y
        }
    }

    enum Alignment { case left, right, top, bottom, centreX, centreY, distributeX, distributeY }

    /// Lines pads up on the outermost edge (or the centre line), or spaces their centres evenly between the outermost.
    mutating func align(_ ids: Set<Pad.ID>, _ how: Alignment) {
        let chosen = pads.indices.filter { ids.contains(pads[$0].id) }
        guard chosen.count >= 2 else { return }
        let l = chosen.map { pads[$0].land }
        let left = l.map { $0.x - $0.w / 2 }.min()!, right = l.map { $0.x + $0.w / 2 }.max()!
        let top = l.map { $0.y - $0.h / 2 }.min()!, bottom = l.map { $0.y + $0.h / 2 }.max()!
        func spread(_ key: WritableKeyPath<CustomPartSpec.Land, Double>) {
            let order = chosen.sorted { pads[$0].land[keyPath: key] < pads[$1].land[keyPath: key] }
            let first = pads[order.first!].land[keyPath: key], last = pads[order.last!].land[keyPath: key]
            for (k, i) in order.enumerated() { pads[i].land[keyPath: key] = first + (last - first) * Double(k) / Double(order.count - 1) }
        }
        switch how {
        case .left: chosen.forEach { pads[$0].land.x = left + pads[$0].land.w / 2 }
        case .right: chosen.forEach { pads[$0].land.x = right - pads[$0].land.w / 2 }
        case .top: chosen.forEach { pads[$0].land.y = top + pads[$0].land.h / 2 }
        case .bottom: chosen.forEach { pads[$0].land.y = bottom - pads[$0].land.h / 2 }
        case .centreX: chosen.forEach { pads[$0].land.x = (left + right) / 2 }
        case .centreY: chosen.forEach { pads[$0].land.y = (top + bottom) / 2 }
        case .distributeX: if chosen.count >= 3 { spread(\.x) }
        case .distributeY: if chosen.count >= 3 { spread(\.y) }
        }
    }

    /// A columns × rows grid of copies of a pad at the pitches (the pad itself is the top-left one), numbered row by
    /// row (or column by column) after the last pad. Returns the new ids.
    @discardableResult
    mutating func grid(from id: Pad.ID, columns: Int, rows: Int, pitchX: Double, pitchY: Double, byColumn: Bool = false) -> Set<Pad.ID> {
        guard let start = pads.first(where: { $0.id == id })?.land, columns >= 1, rows >= 1, columns * rows <= 1024 else { return [] }
        var made = Set<Pad.ID>()
        let cells = byColumn ? (0..<columns).flatMap { c in (0..<rows).map { (c, $0) } } : (0..<rows).flatMap { r in (0..<columns).map { ($0, r) } }
        for (c, r) in cells where c != 0 || r != 0 {
            var land = start
            land.pin = ""
            land.x += Double(c) * pitchX
            land.y += Double(r) * pitchY
            let pad = Pad(land: land)
            pads.append(pad)
            made.insert(pad.id)
        }
        return made
    }

    /// IPC-7351B land for a two-sided gull-wing or chip part from its lead dimensions (mm): toe-to-toe span L,
    /// heel-to-heel span S and lead width W (min / max each), fillet goals (toe, heel, side) for the density level,
    /// fabrication and placement tolerances F and P. Returns pad length × width and the pad-centre distance.
    static func ipc7351(lMin: Double, lMax: Double, sMin: Double, sMax: Double, wMin: Double, wMax: Double,
                        toe: Double, heel: Double, side: Double, f: Double = 0.05, p: Double = 0.025)
        -> (length: Double, width: Double, centres: Double)? {
        guard lMax >= lMin, sMax >= sMin, wMax >= wMin, lMin > sMax, wMin > 0 else { return nil }
        let z = lMin + 2 * toe + (pow(lMax - lMin, 2) + f * f + p * p).squareRoot()
        let g = sMax - 2 * heel - (pow(sMax - sMin, 2) + f * f + p * p).squareRoot()
        let x = wMin + 2 * side + (pow(wMax - wMin, 2) + f * f + p * p).squareRoot()
        guard z > g, x > 0 else { return nil }
        return ((z - g) / 2, x, (z + g) / 2)
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
        let m = courtyardMargin
        return CGRect(x: minX - m, y: minY - m, width: maxX - minX + 2 * m, height: maxY - minY + 2 * m)
    }

    /// What changed from `original`: pads added, removed, moved and resized (matched by pad identity).
    func changes(from original: FootprintDraft) -> (added: Int, removed: Int, moved: Int, resized: Int) {
        let before = Dictionary(original.pads.map { ($0.id, $0.land) }, uniquingKeysWith: { a, _ in a })
        let now = Set(pads.map(\.id))
        var moved = 0, resized = 0
        for p in pads {
            guard let old = before[p.id] else { continue }
            if old.x != p.land.x || old.y != p.land.y { moved += 1 }
            if old.w != p.land.w || old.h != p.land.h || old.drill != p.land.drill || old.round != p.land.round
                || old.options != p.land.options { resized += 1 }
        }
        return (pads.filter { before[$0.id] == nil }.count, before.keys.filter { !now.contains($0) }.count, moved, resized)
    }

    /// Topmost pad under a point (mm), if any.
    func pad(at point: CGPoint) -> Pad.ID? {
        pads.last { p in
            let l = p.land
            return abs(point.x - l.x) <= l.w / 2 && abs(point.y - l.y) <= l.h / 2
        }?.id
    }
}

/// Undo / redo of an editor's document (snapshots), shared by the footprint and symbol editors.
struct EditHistory<Document: Equatable> {
    private(set) var undoStack: [Document] = []
    private(set) var redoStack: [Document] = []
    static var limit: Int { 200 }

    mutating func record(_ document: Document) {
        undoStack.append(document)
        if undoStack.count > Self.limit { undoStack.removeFirst(undoStack.count - Self.limit) }
        redoStack.removeAll()
    }

    mutating func undo(_ current: inout Document) -> Bool {
        guard let previous = undoStack.popLast() else { return false }
        redoStack.append(current)
        current = previous
        return true
    }

    mutating func redo(_ current: inout Document) -> Bool {
        guard let next = redoStack.popLast() else { return false }
        undoStack.append(current)
        current = next
        return true
    }

    var canUndo: Bool { !undoStack.isEmpty }
    var canRedo: Bool { !redoStack.isEmpty }
}

/// Undo / redo of footprint edits.
typealias FootprintHistory = EditHistory<FootprintDraft>

/// The user's own footprint library: one JSON file per footprint (pads, body, courtyard margin, silk / fab lines) in
/// Application Support/SiEDA/Footprints, listed in the footprint editor's Library popover.
enum UserFootprintLibrary {
    struct Saved: Codable {
        var lands: [CustomPartSpec.Land]
        var bodyW: Double
        var bodyD: Double
        var courtyard: Double
        var graphics: [CustomPartSpec.Graphic]
    }

    /// The team's shared library folder (Settings → Library; a network drive or synced folder) or nil: it holds
    /// Footprints/ and 3D Models/.
    static var sharedFolder: URL? {
        UserDefaults.standard.string(forKey: "library.sharedFolder").flatMap { $0.isEmpty ? nil : URL(fileURLWithPath: $0, isDirectory: true) }
    }

    /// My Footprints: in the shared library folder when one is set, else Application Support/SiEDA/Footprints (a
    /// folder set here, e.g. by a test, wins).
    static var folder: URL {
        get {
            chosenFolder ?? sharedFolder?.appendingPathComponent("Footprints", isDirectory: true)
                ?? FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
                .appendingPathComponent("SiEDA/Footprints", isDirectory: true)
        }
        set { chosenFolder = newValue }
    }
    private static var chosenFolder: URL?

    private static func safe(_ name: String) -> String { String(name.map { "/:\\".contains($0) ? "-" : $0 }) }

    private static func file(_ name: String) -> URL {
        folder.appendingPathComponent(safe(name)).appendingPathExtension("json")
    }

    /// Earlier versions of a footprint: each save that changed it keeps the one before (newest 20).
    private static func historyFolder(_ name: String) -> URL {
        folder.appendingPathComponent(".history", isDirectory: true).appendingPathComponent(safe(name), isDirectory: true)
    }

    static func names() -> [String] {
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "json" }.map { $0.deletingPathExtension().lastPathComponent }
            .sorted { $0.localizedStandardCompare($1) == .orderedAscending }
    }

    static func save(_ draft: FootprintDraft, as name: String) throws {
        try FileManager.default.createDirectory(at: folder, withIntermediateDirectories: true)
        let saved = Saved(lands: draft.pads.map(\.land), bodyW: draft.bodyW, bodyD: draft.bodyD, courtyard: draft.courtyardMargin,
                          graphics: draft.graphics)
        let data = try JSONEncoder().encode(saved)
        if let old = try? Data(contentsOf: file(name)), old != data {  // keep the version this one replaces
            let history = historyFolder(name)
            try FileManager.default.createDirectory(at: history, withIntermediateDirectories: true)
            let format = DateFormatter()
            format.dateFormat = "yyyy-MM-dd HH.mm.ss"
            let stamp = format.string(from: Date())
            try old.write(to: history.appendingPathComponent(stamp).appendingPathExtension("json"), options: .atomic)
            for stale in versions(name).dropFirst(20) { try? FileManager.default.removeItem(at: stale) }
        }
        try data.write(to: file(name), options: .atomic)
    }

    /// A footprint's earlier versions, newest first (file names are their save times).
    static func versions(_ name: String) -> [URL] {
        let files = (try? FileManager.default.contentsOfDirectory(at: historyFolder(name), includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "json" }.sorted { $0.lastPathComponent > $1.lastPathComponent }
    }

    static func load(_ name: String) -> Saved? { load(file(name)) }

    static func load(_ url: URL) -> Saved? {
        (try? Data(contentsOf: url)).flatMap { try? JSONDecoder().decode(Saved.self, from: $0) }
    }

    static func remove(_ name: String) {
        try? FileManager.default.removeItem(at: file(name))
        try? FileManager.default.removeItem(at: historyFolder(name))
    }
}
