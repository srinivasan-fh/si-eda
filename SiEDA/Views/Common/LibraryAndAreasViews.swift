import SwiftUI

/// File → Company Library → Show Parts…: the team library folder's parts (search, status, versions, alternates) and
/// what to do with the chosen one (place, release, retire, alternates). Every action goes through MCP library_company.
struct CompanyLibraryView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var parts: [Part] = []
    @State private var search = ""
    @State private var selected: String?
    @State private var folder = ""

    struct Part: Identifiable {
        var name, status, description, manufacturer, package, approvedBy: String
        var version, pins, history: Int
        var approved: Bool
        var alternates: [String]
        var id: String { name }

        init(_ row: [String: Any]) {
            name = row["name"] as? String ?? ""
            status = row["status"] as? String ?? "draft"
            description = row["description"] as? String ?? ""
            manufacturer = row["manufacturer"] as? String ?? ""
            package = row["package"] as? String ?? ""
            approvedBy = row["approvedBy"] as? String ?? ""
            version = row["version"] as? Int ?? 0
            pins = row["pins"] as? Int ?? 0
            history = row["history"] as? Int ?? 0
            approved = row["approved"] as? Bool ?? false
            alternates = row["alternates"] as? [String] ?? []
        }
    }

    private var shown: [Part] {
        search.isEmpty ? parts : parts.filter {
            "\($0.name) \($0.description) \($0.manufacturer) \($0.package)".localizedCaseInsensitiveContains(search)
        }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Company Library").font(.headline).foregroundStyle(Theme.skyBlue)
                Text(verbatim: folder).font(.caption).foregroundStyle(Theme.textSecondary).lineLimit(1).truncationMode(.middle)
                Spacer()
                Button("Choose Library Folder…") { if store.companyLibraryFolder(choose: true) != nil { reload() } }
                Button("Publish Selected Part") { store.companyLibraryPublish(); reload() }
            }
            TextField("Search parts", text: $search).textFieldStyle(.roundedBorder)
            HSplitView {
                List(shown, selection: $selected) { part in
                    HStack {
                        Text(verbatim: part.name)
                        Spacer()
                        Text(verbatim: "v\(part.version)").foregroundStyle(Theme.textSecondary).monospacedDigit()
                        badge(part.status)
                    }
                    .tag(part.name)
                }
                .frame(minWidth: 260)
                detail.frame(minWidth: 320, maxWidth: .infinity, maxHeight: .infinity, alignment: .topLeading).padding(8)
            }
            HStack {
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(minWidth: 760, minHeight: 480)
        .onAppear(perform: reload)
    }

    @ViewBuilder private var detail: some View {
        if let part = parts.first(where: { $0.name == selected }) {
            VStack(alignment: .leading, spacing: 8) {
                Text(verbatim: part.name).font(.title3.bold())
                if !part.description.isEmpty { Text(verbatim: part.description).foregroundStyle(Theme.textSecondary) }
                Grid(alignment: .leading, horizontalSpacing: 12, verticalSpacing: 4) {
                    GridRow { label("Status"); badge(part.status) }
                    GridRow { label("Version"); Text(verbatim: "\(part.version)") + Text(verbatim: part.history > 0 ? " (\(part.history) ↺)" : "") }
                    if !part.manufacturer.isEmpty { GridRow { label("Manufacturer"); Text(verbatim: part.manufacturer) } }
                    if !part.package.isEmpty { GridRow { label("Package"); Text(verbatim: "\(part.package.uppercased()), \(part.pins)") } }
                    if !part.approvedBy.isEmpty { GridRow { label("Released by"); Text(verbatim: part.approvedBy + (part.approved ? " ✓" : " (changed since)")) } }
                    if !part.alternates.isEmpty { GridRow { label("Approved Alternates"); Text(verbatim: part.alternates.joined(separator: ", ")) } }
                }
                .font(.callout)
                HStack {
                    Button("Place Part…") { act("use", part.name) }.disabled(part.status == "obsolete")
                    Button("Release Part…") { act("release", part.name) }
                    Button("Mark Part Obsolete…") { act("obsolete", part.name) }.disabled(part.status == "obsolete")
                    Button("Set Approved Alternates…") { act("alternates", part.name) }
                }
            }
        } else {
            Text(parts.isEmpty ? LocalizedStringKey("No parts yet: publish one from the board or the schematic.")
                 : LocalizedStringKey("Choose a part to see its details."))
                .foregroundStyle(Theme.textSecondary)
        }
    }

    private func label(_ key: LocalizedStringKey) -> some View { Text(key).foregroundStyle(Theme.textSecondary) }

    private func badge(_ status: String) -> some View {
        let (key, colour): (LocalizedStringKey, Color) = status == "released" ? ("Released", .green)
            : status == "obsolete" ? ("Obsolete", .red) : ("Draft", .orange)
        return Text(key).font(.caption.bold()).padding(.horizontal, 6).padding(.vertical, 1)
            .background(colour.opacity(0.18), in: Capsule()).foregroundStyle(colour)
    }

    private func act(_ action: String, _ name: String) {
        store.companyLibraryAction(action, name: name)
        reload()
    }

    private func reload() {
        let rows = (store.companyLibrary(["action": "list"])?["parts"] as? [[String: Any]]) ?? []
        parts = rows.map(Part.init)
        folder = UserDefaults.standard.string(forKey: "library.companyFolder") ?? ""
        if selected == nil || !parts.contains(where: { $0.name == selected }) { selected = parts.first?.name }
    }
}

/// Design → Layout → Rooms and Partitions…: every room (its parts) and partition (its owner) of the board as rows to
/// rename, move, resize, add or remove; Apply sets them through MCP pcb_rooms / pcb_partitions.
struct BoardAreasView: View {
    @EnvironmentObject private var store: DesignStore
    @Environment(\.dismiss) private var dismiss
    @State private var rooms: [Area] = []
    @State private var partitions: [Area] = []
    @State private var problem = ""

    struct Area: Identifiable {
        var id = UUID()
        var name: String
        var detail: String  // a room's designators (space-separated), a partition's owner
        var x0, y0, x1, y1: Double

        var json: [String: Any] { ["name": name, "x0": min(x0, x1), "y0": min(y0, y1), "x1": max(x0, x1), "y1": max(y0, y1)] }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Rooms and Partitions").font(.headline).foregroundStyle(Theme.skyBlue)
            ScrollView {
                VStack(alignment: .leading, spacing: 14) {
                    section("Rooms", detail: "Parts", areas: $rooms)
                    section("Partitions", detail: "Owner", areas: $partitions)
                }
            }
            if !problem.isEmpty { Text(verbatim: problem).foregroundStyle(Theme.error).font(.callout) }
            HStack {
                Spacer()
                Button("Cancel") { dismiss() }.keyboardShortcut(.cancelAction)
                Button("Apply") { apply() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(16)
        .frame(minWidth: 780, minHeight: 420)
        .onAppear(perform: load)
    }

    private func section(_ title: LocalizedStringKey, detail: LocalizedStringKey, areas: Binding<[Area]>) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text(title).font(.subheadline.bold())
                Spacer()
                Button("Add") {
                    let b = store.snapshot.board
                    areas.wrappedValue.append(Area(name: "\(areas.wrappedValue.count + 1)", detail: "", x0: 0, y0: 0, x1: b.width / 2, y1: b.height / 2))
                }
            }
            Grid(alignment: .leading, horizontalSpacing: 8, verticalSpacing: 4) {
                GridRow {
                    Text("Name"); Text(detail); Text(verbatim: "x0 (mm)"); Text(verbatim: "y0 (mm)"); Text(verbatim: "x1 (mm)"); Text(verbatim: "y1 (mm)"); Text(verbatim: "")
                }
                .font(.caption).foregroundStyle(Theme.textSecondary)
                ForEach(areas) { $area in
                    GridRow {
                        TextField("", text: $area.name).frame(width: 130)
                        TextField("", text: $area.detail).frame(width: 220)
                        number($area.x0)
                        number($area.y0)
                        number($area.x1)
                        number($area.y1)
                        Button {
                            let id = area.id
                            areas.wrappedValue.removeAll { $0.id == id }
                        } label: { Image(systemName: "trash") }
                            .buttonStyle(.borderless).help("Remove").accessibilityLabel(Text("Remove"))
                    }
                }
            }
            .textFieldStyle(.roundedBorder)
        }
    }

    private func number(_ value: Binding<Double>) -> some View {
        TextField("", value: value, format: .number.precision(.fractionLength(0...3))).frame(width: 70).monospacedDigit()
    }

    private func load() {
        let b = store.snapshot.board
        rooms = b.rooms.map { Area(name: $0.name, detail: ($0.refs ?? []).joined(separator: " "), x0: $0.x0, y0: $0.y0, x1: $0.x1, y1: $0.y1) }
        partitions = b.partitions.map { Area(name: $0.name, detail: $0.owner ?? "", x0: $0.x0, y0: $0.y0, x1: $0.x1, y1: $0.y1) }
    }

    private func apply() {
        let roomRows = rooms.map { a in
            a.json.merging(["refs": a.detail.split(whereSeparator: { $0 == " " || $0 == "," }).map(String.init)] as [String: Any]) { x, _ in x }
        }
        let partitionRows = partitions.map { a in a.json.merging(["owner": a.detail] as [String: Any]) { x, _ in x } }
        for (tool, key, rows) in [("pcb_rooms", "rooms", roomRows), ("pcb_partitions", "partitions", partitionRows)] {
            let r = store.runTool(tool, arguments: ["action": "set", key: rows])
            if r.isError { problem = r.text; return }
        }
        dismiss()
    }
}
