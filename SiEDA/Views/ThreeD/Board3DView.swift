import SceneKit
import SwiftUI

/// 3D workspace with two modes:
/// - **Assembly** (default): the C++ core tessellates board, copper, vias, silkscreen and component bodies, tagging
///   each surface so it renders with its own physical material (glossy mask, metal finish, solder, plastic…) under
///   studio lighting, in the board's solder mask colour (green unless another is chosen).
/// - **X-Ray Stack**: every copper layer floats apart in a holographic, additive-glow exploded view with
///   through-vias as light pillars, wireframe component bodies and a scanning beam.
struct Board3DWorkspace: View {
    enum Mode: String, CaseIterable, Identifiable {
        case assembly = "Assembly"
        case xray = "X-Ray Stack"
        var id: String { rawValue }
    }

    @EnvironmentObject private var store: DesignStore
    @AppStorage("threeD.mode.v2") private var modeRaw = Mode.assembly.rawValue
    @AppStorage("threeD.finish") private var finishRaw = BoardFinish.enig.rawValue
    @State private var showComponents = true
    @State private var resetCamera = 0
    @State private var stats = ""
    @State private var xray = XRaySettings()
    @State private var look = AssemblyLook()
    @State private var camera: CameraCommand?
    @State private var measuring = false
    @State private var measured: [SCNVector3] = []
    @State private var picked: Int?
    @AppStorage("threeD.panels") private var panels = true
    @State private var savedViews: [String: [Double]] = [:]
    @State private var namingView = false
    @State private var viewName = ""
    /// Named viewpoints are kept per project name.
    private var viewsKey: String { "threeD.views." + store.snapshot.name }

    private var mode: Mode { Mode(rawValue: modeRaw) ?? .assembly }

    private static let surfaceToggles: [(title: String, surface: MeshSurface)] = [
        ("Solder Mask", .mask), ("Silkscreen", .silk), ("Copper Finish", .finish), ("Solder", .solder), ("Markings", .marking),
    ]

    /// A parts / layer / cut setting of the assembly view.
    struct AssemblyLook: Equatable {
        var hiddenSurfaces: Set<MeshSurface> = []
        var hiddenParts: Set<Int> = []
        var explode = 0.0       // 0…1: parts lifted up to 8 mm off the board
        var section: Double?    // cut plane across the board's width (0…1), nil = whole board
        var standoffs = false   // hex standoffs and screws at the mounting holes
        var enclosure = true    // the loaded enclosure, see-through
        var fold = true         // rigid-flex bend areas folded by their bend angle
        var clashes = true      // parts the 3D clearance DRC flags, boxed in red
    }

    var body: some View {
        VStack(spacing: 0) {
            OptionsBar {
                Picker("Mode", selection: $modeRaw) {
                    Label("Assembly", systemImage: "cube.transparent").tag(Mode.assembly.rawValue)
                    Label("X-Ray Stack", systemImage: "square.3.layers.3d.top.filled").tag(Mode.xray.rawValue)
                }
                .pickerStyle(.segmented)
                .frame(width: 230)
                if mode == .xray {
                    xrayControls
                } else {
                    Toggle("Components", isOn: $showComponents).toggleStyle(.switch).controlSize(.mini)
                    maskPicker
                    Picker("Finish", selection: $finishRaw) {
                        ForEach(BoardFinish.allCases) { Text($0.title).tag($0.rawValue) }
                    }
                    .fixedSize()
                    .help("Surface finish of the exposed copper: pads, via lands and plated holes")
                    let layers = store.snapshot.board.layerCount
                    Label(layers == 1 ? "Single-sided" : "\(layers)-layer", systemImage: "square.3.layers.3d.down.right")
                        .font(.caption)
                        .foregroundStyle(Theme.textSecondary)
                        .help("Stack-up from the Layers setting in PCB Layout: 1 layer leaves the underside bare FR-4; "
                              + "4 and 6 layers show their inner copper on the board edge")
                    Text(stats).foregroundStyle(Theme.textMuted).font(.caption)
                    assemblyTools
                    Toggle("Panels", isOn: $panels).toggleStyle(.switch).controlSize(.mini)
                        .help("Layers and parts on the left, the picked part's properties on the right, 3D checks below")
                }
                Spacer()
                Button { resetCamera += 1 } label: { Label("Reset View", systemImage: "camera.metering.center.weighted") }
                Menu {
                    Button("STL (ASCII)") { Task { await store.export(.stl) } }
                    Button("OBJ with vertex colours") { Task { await store.export(.obj) } }
                } label: {
                    Label("Export 3D", systemImage: "square.and.arrow.up")
                }
                .fixedSize()
            }
            .buttonStyle(.borderless)

            ZStack(alignment: .bottomLeading) {
                if store.snapshot.pads.isEmpty {
                    BlueEmptyState(systemImage: "cube.transparent", title: "Nothing to show yet",
                                   message: "Place footprints in the PCB Layout workspace to build the 3D model.",
                                   actionTitle: "Auto-Place Footprints") { Task { await store.autoPlaceInBackground(all: true) } }
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if mode == .xray {
                    XRayStackView(engine: store.engine, snapshot: store.snapshot, revision: store.revision,
                                  settings: xray, resetToken: resetCamera)
                    if xray.hud {
                        HoloHUDOverlay(title: store.snapshot.name, readouts: hudReadouts)
                    }
                    layerLegend
                        .padding(12)
                } else {
                    HStack(spacing: 0) {
                        if panels {
                            layersPanel.frame(width: 200)
                            Divider()
                        }
                        VStack(spacing: 0) {
                            ZStack(alignment: .bottomLeading) {
                                BoardSceneView(engine: store.engine, revision: store.revision, includeComponents: showComponents,
                                               board: store.snapshot.board, resetToken: resetCamera,
                                               finish: BoardFinish(rawValue: finishRaw) ?? .enig, stats: $stats,
                                               look: look, camera: camera, clashes: clashBodies,
                                               selected: store.snapshot.bodies.filter { store.selection.contains($0.component) },
                                               onClick: clicked, onSaveView: saveView)
                                if !panels || measuring { pickInfo.padding(12) }
                            }
                            if panels {
                                Divider()
                                checksPanel.frame(height: 130)
                            }
                        }
                        if panels {
                            Divider()
                            propertiesPanel.frame(width: 230)
                        }
                    }
                }
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity)
            .background(Theme.navy)
            .overlay(alignment: .bottomTrailing) {
                if !store.snapshot.pads.isEmpty, !panels || mode == .xray { navigationHint.padding(12) }
            }
        }
        .onAppear { savedViews = UserDefaults.standard.dictionary(forKey: viewsKey) as? [String: [Double]] ?? [:] }
        .onChange(of: viewsKey) { _, key in savedViews = UserDefaults.standard.dictionary(forKey: key) as? [String: [Double]] ?? [:] }
        .alert("Save View", isPresented: $namingView) {
            TextField("Name", text: $viewName)
            Button("Save") {
                let name = viewName.trimmingCharacters(in: .whitespaces)
                if !name.isEmpty { camera = CameraCommand(.saveAs(name)) }
            }
            Button("Cancel", role: .cancel) {}
        }
    }

    private func saveView(_ name: String, _ values: [Double]) {
        savedViews[name] = values
        UserDefaults.standard.set(savedViews, forKey: viewsKey)
    }

    /// Left panel: the board's layers and the parts (click selects, the eye hides).
    private var layersPanel: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 6) {
                Text("LAYERS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                ForEach(Self.surfaceToggles, id: \.surface) { item in
                    Toggle(LocalizedStringKey(item.title), isOn: Binding(get: { !look.hiddenSurfaces.contains(item.surface) }, set: { on in
                        if on { look.hiddenSurfaces.remove(item.surface) } else { look.hiddenSurfaces.insert(item.surface) }
                    }))
                }
                Toggle("Components", isOn: $showComponents)
                Toggle("Standoffs and Screws", isOn: $look.standoffs)
                Toggle("Enclosure", isOn: $look.enclosure)
                Toggle("Fold Flex", isOn: $look.fold)
                Toggle("Show Clashes", isOn: $look.clashes)
                Divider()
                Text("PARTS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                ForEach(store.snapshot.bodies, id: \.component) { body in
                    if let c = store.snapshot.component(body.component) {
                        let hidden = look.hiddenParts.contains(c.id)
                        HStack(spacing: 6) {
                            Button {
                                if hidden { look.hiddenParts.remove(c.id) } else { look.hiddenParts.insert(c.id) }
                            } label: { Image(systemName: hidden ? "eye.slash" : "eye") }
                                .buttonStyle(.borderless)
                                .accessibilityLabel(hidden ? "Show Part" : "Hide Part")
                            Button {
                                picked = c.id
                                store.select(component: c.id)
                            } label: {
                                Text(verbatim: "\(c.ref)  \(c.value)").lineLimit(1)
                                    .foregroundStyle(store.selection.contains(c.id) ? Theme.skyBlue : hidden ? Theme.textMuted : Theme.textPrimary)
                            }
                            .buttonStyle(.plain)
                        }
                    }
                }
            }
            .toggleStyle(.checkbox)
            .font(.caption)
            .padding(10)
        }
        .background(Theme.deepBlue.opacity(0.45))
    }

    /// Right panel: the picked (or selected) part's properties and its 3D model.
    private var propertiesPanel: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text("PROPERTIES").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
            if let id = picked ?? store.selection.first, let c = store.snapshot.component(id) {
                let body = store.snapshot.bodies.first { $0.component == id }
                Group {
                    LabeledContent("Part", value: c.ref)
                    LabeledContent("Value", value: c.value)
                    if let body { LabeledContent("Package", value: body.package) }
                    LabeledContent("Side", value: c.pcb.bottom ? String(localized: "Bottom") : String(localized: "Top"))
                    LabeledContent("Position", value: String(format: "%.2f, %.2f mm", c.pcb.x, c.pcb.y))
                    if let body {
                        LabeledContent("Body", value: String(format: "%.2f × %.2f × %.2f mm", body.w, body.d, body.h))
                    }
                }
                .font(.caption)
                Button("3D Model…") { store.assignPartModel(id) }
                    .help("Give this part its own 3D model (STEP, STL, OBJ or VRML), seated on its footprint")
                Button("Generated Body") { store.removePartModel(id) }
                    .help("Back to the generated body (or its library part's model)")
                Button("Focus") { camera = CameraCommand(.focus(CGPoint(x: c.pcb.x, y: c.pcb.y))) }
            } else {
                Text("Click a part in the model or the list.").font(.caption).foregroundStyle(Theme.textMuted)
            }
            Spacer()
        }
        .controlSize(.small)
        .padding(10)
        .frame(maxHeight: .infinity, alignment: .top)
        .background(Theme.deepBlue.opacity(0.45))
    }

    /// Bottom panel: the 3D clearance findings of the last check (click picks and focuses the part).
    private var checksPanel: some View {
        let checks = store.drcResults.filter { $0.code.hasPrefix("MECH_") }
        return VStack(alignment: .leading, spacing: 4) {
            HStack {
                Text("3D CHECKS").font(.caption.weight(.bold)).foregroundStyle(Theme.skyBlue)
                Spacer()
                Button("Check 3D Clearances") { store.runDRC() }.controlSize(.small)
            }
            if checks.isEmpty {
                Text("No 3D findings. Check 3D Clearances lists collisions, heights, the enclosure and connector access.")
                    .font(.caption).foregroundStyle(Theme.textMuted)
            }
            ScrollView {
                VStack(alignment: .leading, spacing: 3) {
                    ForEach(checks) { v in
                        Button {
                            if let id = v.components.first, let c = store.snapshot.component(id) {
                                picked = id
                                store.select(component: id)
                                camera = CameraCommand(.focus(CGPoint(x: c.pcb.x, y: c.pcb.y)))
                            }
                        } label: {
                            Label(v.message, systemImage: v.severity == .error ? "xmark.octagon.fill"
                                  : v.severity == .warning ? "exclamationmark.triangle.fill" : "info.circle")
                                .font(.caption)
                                .foregroundStyle(v.severity == .error ? Theme.error : v.severity == .warning ? Theme.warning : Theme.textSecondary)
                                .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .buttonStyle(.plain)
                    }
                }
            }
        }
        .padding(10)
        .background(Theme.deepBlue.opacity(0.45))
    }

    /// Live read-outs for the holographic HUD.
    private var hudReadouts: [(String, String)] {
        let b = store.snapshot.board
        let parts = store.snapshot.components.filter { !$0.componentKind.isVirtual && $0.unitOf == nil }.count
        return [("Board", String(format: "%.1f × %.1f mm", b.width, b.height)),
                ("Stack", "\(b.layerCount) layers · \(b.material.uppercased())"),
                ("Parts", "\(parts)"),
                ("Nets", "\(store.snapshot.nets.filter { $0.pinCount > 1 }.count)"),
                ("Tracks", "\(store.snapshot.tracks.count)"),
                ("Vias", "\(store.snapshot.vias.count)")]
    }

    /// Solder mask colour swatches (green, black, blue, red, yellow, white, purple), like a fab's sample boards.
    private var maskPicker: some View {
        HStack(spacing: 5) {
            Text("Mask").font(.caption).foregroundStyle(Theme.textMuted)
            ForEach(SolderMaskColour.allCases) { mask in
                let selected = store.snapshot.board.mask == mask
                let c = mask.swatch
                Button { store.setSolderMask(mask) } label: {
                    Circle()
                        .fill(Color(red: c.red, green: c.green, blue: c.blue))
                        .overlay(Circle().strokeBorder(Color.white.opacity(0.35), lineWidth: 0.5))
                        .frame(width: 14, height: 14)
                        .padding(2)
                        .overlay(Circle().strokeBorder(selected ? Theme.skyBlue : .clear, lineWidth: 1.5))
                }
                .buttonStyle(.plain)
                .help("\(mask.title) solder mask")
                .accessibilityLabel("\(mask.title) solder mask")
                .accessibilityAddTraits(selected ? .isSelected : [])
            }
        }
    }

    /// Views, layers, explode, cut, measure and part visibility of the assembly view.
    private var assemblyTools: some View {
        HStack(spacing: 8) {
            Menu {
                ForEach(ViewPreset.allCases) { preset in
                    Button(LocalizedStringKey(preset.title)) { camera = CameraCommand(.preset(preset)) }
                }
                Divider()
                Button("Focus Selection") {
                    if let id = store.selection.first, let c = store.snapshot.component(id) {
                        camera = CameraCommand(.focus(CGPoint(x: c.pcb.x, y: c.pcb.y)))
                    }
                }
                .disabled(store.selection.isEmpty)
                Button("Save View As…") {
                    viewName = ""
                    namingView = true
                }
                if !savedViews.isEmpty {
                    Divider()
                    ForEach(savedViews.keys.sorted(), id: \.self) { name in
                        Button(name) { camera = CameraCommand(.go(savedViews[name] ?? [])) }
                    }
                    Button("Forget Saved Views") {
                        savedViews = [:]
                        UserDefaults.standard.removeObject(forKey: viewsKey)
                    }
                }
            } label: { Label("View", systemImage: "camera") }
                .fixedSize()
                .help("Top, bottom (flip), front, back, side and isometric views; focus the selected part; save a view")
            Menu {
                ForEach(Self.surfaceToggles, id: \.surface) { item in
                    Toggle(LocalizedStringKey(item.title), isOn: Binding(get: { !look.hiddenSurfaces.contains(item.surface) }, set: { on in
                        if on { look.hiddenSurfaces.remove(item.surface) } else { look.hiddenSurfaces.insert(item.surface) }
                    }))
                }
                Divider()
                Button("Hide Selected Parts") { look.hiddenParts.formUnion(store.selection) }
                    .disabled(store.selection.isEmpty)
                Button("Isolate Selected Parts") {
                    look.hiddenParts = Set(store.snapshot.components.map(\.id)).subtracting(store.selection)
                }
                .disabled(store.selection.isEmpty)
                Button("Show All Parts") { look.hiddenParts = [] }
                    .disabled(look.hiddenParts.isEmpty)
                Divider()
                Toggle("Standoffs and Screws", isOn: $look.standoffs)
                Toggle("Enclosure", isOn: $look.enclosure)
                Toggle("Fold Flex", isOn: $look.fold)
                Toggle("Show Clashes", isOn: $look.clashes)
            } label: { Label("Layers", systemImage: "square.3.layers.3d") }
                .fixedSize()
            Menu {
                Button("Check 3D Clearances") { store.runDRC() }
                Button("Load Enclosure…") { store.loadEnclosure() }
                Button("Remove Enclosure") { store.removeEnclosure() }
                Divider()
                Button("Save Image…") { if let url = Self.saveURL("board.png") { camera = CameraCommand(.saveImage(url)) } }
                Button("Export USDZ…") { if let url = Self.saveURL("board.usdz") { camera = CameraCommand(.saveUSDZ(url)) } }
                Button("Export glTF (.glb)…") { Task { await store.exportFile("glb", name: "assembly.glb", title: "glTF 2.0") } }
                Button("Export VRML…") { Task { await store.export(.vrml) } }
            } label: { Label("Mechanical", systemImage: "shippingbox") }
                .fixedSize()
                .help("Clearance check (collisions, heights, enclosure, connector access), enclosure model and 3D exports")
            HStack(spacing: 4) {
                Image(systemName: "arrow.up.square").foregroundStyle(Theme.skyBlue)
                Slider(value: $look.explode, in: 0...1).frame(width: 70)
            }
            .help("Explode: lift the parts off the board")
            Toggle("Cut", isOn: Binding(get: { look.section != nil }, set: { look.section = $0 ? 0.5 : nil }))
                .toggleStyle(.switch).controlSize(.mini)
                .help("Section view: cut the board across its width")
            if let section = look.section {
                Slider(value: Binding(get: { section }, set: { look.section = $0 }), in: 0...1).frame(width: 70)
            }
            Toggle("Measure", isOn: $measuring).toggleStyle(.switch).controlSize(.mini)
                .help("Click two points on the model to measure the distance between them")
                .onChange(of: measuring) { _, _ in measured = [] }
        }
    }

    /// Picked part (click) or the measured distance.
    @ViewBuilder private var pickInfo: some View {
        if measuring {
            let text = measured.count == 2
                ? String(format: "%.2f mm  (dx %.2f · dy %.2f · dz %.2f)", Self.distance(measured[0], measured[1]),
                         abs(measured[1].x - measured[0].x), abs(measured[1].z - measured[0].z), abs(measured[1].y - measured[0].y))
                : String(localized: "Click two points to measure")
            Label(text, systemImage: "ruler").font(.caption.monospacedDigit()).padding(8)
                .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.8)))
        } else if let id = picked, let c = store.snapshot.component(id) {
            let height = store.snapshot.bodies.first { $0.component == id }.map { String(format: " · %.2f mm", $0.h) } ?? ""
            HStack(spacing: 8) {
                Label("\(c.ref) · \(c.value) · \(c.pcb.bottom ? "bottom" : "top")\(height)", systemImage: "cpu").font(.caption)
                Button("3D Model…") { store.assignPartModel(id) }
                    .help("Give this part its own 3D model (STEP, STL, OBJ or VRML), seated on its footprint")
                Button("Generated Body") { store.removePartModel(id) }
                    .help("Back to the generated body (or its library part's model)")
            }
            .controlSize(.small)
            .padding(8)
            .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.8)))
        }
    }

    /// Bodies of the parts the last DRC flagged in 3D (collision, height, enclosure).
    private var clashBodies: [SnapBody] {
        guard look.clashes else { return [] }
        let ids = Set(store.drcResults.filter { $0.code.hasPrefix("MECH_") && $0.severity != .info }.flatMap(\.components))
        return store.snapshot.bodies.filter { ids.contains($0.component) }
    }

    static func saveURL(_ name: String) -> URL? {
        let panel = NSSavePanel()
        panel.nameFieldStringValue = name
        return panel.runModal() == .OK ? panel.url : nil
    }

    static func distance(_ a: SCNVector3, _ b: SCNVector3) -> Double {
        let dx = Double(a.x - b.x), dy = Double(a.y - b.y), dz = Double(a.z - b.z)
        return (dx * dx + dy * dy + dz * dz).squareRoot()
    }

    /// A click on the model: a measure point, or the part under it (selected, so the editors cross-probe to it).
    private func clicked(_ world: SCNVector3, _ boardPoint: CGPoint) {
        if measuring {
            measured = measured.count >= 2 ? [world] : measured + [world]
            return
        }
        picked = store.snapshot.courtyards.last { $0.rect.contains(boardPoint) && !look.hiddenParts.contains($0.component) }?.component
        store.select(component: picked)
    }

    /// SceneKit camera controls are invisible by default; spell them out.
    private var navigationHint: some View {
        VStack(alignment: .trailing, spacing: 3) {
            Label("Drag to orbit", systemImage: "rotate.3d")
            Label("Scroll or pinch to zoom", systemImage: "plus.magnifyingglass")
            Label("⌥-drag or two-finger drag to pan", systemImage: "hand.draw")
            Label("Reset View re-centres the board", systemImage: "camera.metering.center.weighted")
        }
        .labelStyle(.titleAndIcon)
        .font(.caption2)
        .foregroundStyle(Theme.textSecondary)
        .padding(8)
        .background(RoundedRectangle(cornerRadius: 8).fill(Theme.navy.opacity(0.7)))
        .allowsHitTesting(false)
    }

    private var xrayControls: some View {
        HStack(spacing: 12) {
            HStack(spacing: 6) {
                Image(systemName: "arrow.up.and.down.square").foregroundStyle(Theme.skyBlue)
                Slider(value: $xray.explode, in: 0.5...16).frame(width: 110)
                Text(String(format: "%.1f mm", xray.explode)).font(.caption.monospacedDigit()).foregroundStyle(Theme.textMuted)
            }
            .help("Layer separation (exploded view)")
            Toggle("Parts", isOn: $xray.showComponents).toggleStyle(.switch).controlSize(.mini)
            Toggle("Vias", isOn: $xray.showVias).toggleStyle(.switch).controlSize(.mini)
            Toggle("Scan", isOn: $xray.scan).toggleStyle(.switch).controlSize(.mini)
            Toggle("Spin", isOn: $xray.spin).toggleStyle(.switch).controlSize(.mini)
            Toggle("HUD", isOn: $xray.hud).toggleStyle(.switch).controlSize(.mini)
                .help("Cinematic heads-up display: projector, particles, part call-outs, data pillars and read-outs")
            Toggle("Panels", isOn: $xray.panels).toggleStyle(.switch).controlSize(.mini)
                .help("Holographic data panels around the stack: copper per layer, net fan-out, parts, routing and a board map")
        }
    }

    /// Per-layer chips (click to hide a layer of the stack).
    private var layerLegend: some View {
        let board = store.snapshot.board
        return VStack(alignment: .leading, spacing: 4) {
            Text("STACK-UP · \(board.layerCount) LAYER\(board.layerCount == 1 ? "" : "S")")
                .font(.caption2.weight(.bold)).foregroundStyle(Theme.skyBlue)
            ForEach(0..<max(1, board.layerCount), id: \.self) { layer in
                let hidden = xray.hiddenLayers.contains(layer)
                Button {
                    if hidden { xray.hiddenLayers.remove(layer) } else { xray.hiddenLayers.insert(layer) }
                } label: {
                    HStack(spacing: 6) {
                        RoundedRectangle(cornerRadius: 2)
                            .fill(Theme.copperColor(layer, layerCount: board.layerCount).opacity(hidden ? 0.25 : 1))
                            .frame(width: 18, height: 6)
                            .shadow(color: Theme.copperColor(layer, layerCount: board.layerCount), radius: hidden ? 0 : 4)
                        Text("L\(layer + 1) · \(board.layerName(layer))")
                            .font(.caption.monospaced())
                            .foregroundStyle(hidden ? Theme.textMuted : Theme.textPrimary)
                    }
                }
                .buttonStyle(.plain)
            }
            Text("\(store.snapshot.tracks.count) tracks · \(store.snapshot.vias.count) vias")
                .font(.caption2).foregroundStyle(Theme.textMuted)
        }
        .padding(10)
        .background(RoundedRectangle(cornerRadius: 10).fill(Theme.navy.opacity(0.75)))
        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(Theme.skyBlue.opacity(0.35)))
    }
}

/// Camera views of the 3D assembly.
enum ViewPreset: String, CaseIterable, Identifiable {
    case top, bottom, front, back, left, right, iso
    var id: String { rawValue }
    var title: String {
        switch self {
        case .top: return "Top"
        case .bottom: return "Bottom (Flip)"
        case .front: return "Front"
        case .back: return "Back"
        case .left: return "Left"
        case .right: return "Right"
        case .iso: return "Isometric"
        }
    }
}

/// A one-shot camera request to the 3D view (a new id runs it once).
struct CameraCommand: Equatable {
    enum Action: Equatable {
        case preset(ViewPreset)
        case focus(CGPoint)  // board millimetres
        case save, restore
        case saveImage(URL), saveUSDZ(URL)
        case saveAs(String)  // a named viewpoint: the camera's position and orientation go to `onSaveView`
        case go([Double])    // back to a named viewpoint (x, y, z, then the orientation quaternion)
    }
    let id = UUID()
    let action: Action
    init(_ action: Action) { self.action = action }
}

/// Surface finish of the exposed copper (pads, via lands, plated rings), as ordered from the fab.
enum BoardFinish: String, CaseIterable, Identifiable {
    case enig, hasl, osp
    var id: String { rawValue }
    var title: String {
        switch self {
        case .enig: return "ENIG (gold)"
        case .hasl: return "HASL (tin)"
        case .osp: return "OSP (bare copper)"
        }
    }
    var colour: NSColor {
        switch self {
        case .enig: return NSColor(red: 0.96, green: 0.78, blue: 0.42, alpha: 1)
        case .hasl: return NSColor(red: 0.83, green: 0.84, blue: 0.86, alpha: 1)
        case .osp: return NSColor(red: 0.93, green: 0.56, blue: 0.36, alpha: 1)
        }
    }
    var roughness: CGFloat { self == .hasl ? 0.32 : 0.22 }
}

/// Realistic assembly view: each surface (mask, laminate, copper finish, solder, tin and gold, silkscreen, plastic,
/// ceramic, glass…) gets its own physically based material, lit by a studio environment (soft boxes reflected in
/// the metals) and a key light casting soft shadows onto an invisible floor, with ambient occlusion in the gaps.
struct BoardSceneView: NSViewRepresentable {
    let engine: EDAEngine
    let revision: Int
    let includeComponents: Bool
    let board: BoardInfo
    let resetToken: Int
    var finish: BoardFinish = .enig
    @Binding var stats: String
    var look = Board3DWorkspace.AssemblyLook()
    var camera: CameraCommand?
    var clashes: [SnapBody] = []
    /// Parts selected in the editors, boxed in the selection colour.
    var selected: [SnapBody] = []
    var onClick: (SCNVector3, CGPoint) -> Void = { _, _ in }
    var onSaveView: (String, [Double]) -> Void = { _, _ in }

    final class Coordinator: NSObject {
        var lastRevision = -1
        /// An edit's mesh rebuild, waiting for edits to pause (each edit bumps the revision).
        var pendingRebuild: DispatchWorkItem?
        var lastComponents = true
        var lastFinish: BoardFinish?
        var lastReset = 0
        var lastMesh: [String] = []
        var lastClashes: [SnapBody] = []
        let clashNode = SCNNode()
        var lastSelected: [SnapBody] = []
        let selectedNode = SCNNode()
        var onSaveView: (String, [Double]) -> Void = { _, _ in }
        var lastCamera: UUID?
        var saved: (SCNVector3, SCNQuaternion)?
        let boardNode = SCNNode()
        let cameraNode = SCNNode()
        let floorNode = SCNNode()
        weak var view: SCNView?
        var board = BoardInfo()
        var onClick: (SCNVector3, CGPoint) -> Void = { _, _ in }

        @objc func clicked(_ recognizer: NSClickGestureRecognizer) {
            guard let view else { return }
            let options: [SCNHitTestOption: Any] = [.searchMode: SCNHitTestSearchMode.closest.rawValue]
            guard let hit = view.hitTest(recognizer.location(in: view), options: options).first else { return }
            let w = hit.worldCoordinates
            // The board node is centred on the origin: board mm = world + half the board.
            onClick(w, CGPoint(x: Double(w.x) + board.width / 2, y: Double(w.z) + board.height / 2))
        }
    }

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> SCNView {
        let view = SCNView()
        let scene = SCNScene()
        view.scene = scene
        view.allowsCameraControl = true
        view.autoenablesDefaultLighting = false
        view.antialiasingMode = .multisampling4X
        view.backgroundColor = AppTheme.backdrop(0.03, 0.06, 0.13).nsColor
        view.defaultCameraController.interactionMode = .orbitTurntable
        view.defaultCameraController.inertiaEnabled = true

        // Dark studio backdrop; the reflections come from a bright studio environment instead.
        scene.background.contents = Self.backdrop
        scene.lightingEnvironment.contents = Self.studioEnvironment
        scene.lightingEnvironment.intensity = 1.25

        let camera = SCNCamera()
        camera.zNear = 0.1
        camera.zFar = 2000
        camera.fieldOfView = 35
        camera.wantsHDR = true
        camera.wantsExposureAdaptation = false
        camera.exposureOffset = 0.15
        camera.screenSpaceAmbientOcclusionIntensity = 0.9
        camera.screenSpaceAmbientOcclusionRadius = 1.2   // millimetres: the gaps under and between parts
        camera.screenSpaceAmbientOcclusionBias = 0.03
        camera.bloomIntensity = 0.25                     // a little glint on solder and gold
        camera.bloomThreshold = 0.95
        camera.bloomBlurRadius = 6
        camera.vignettingIntensity = 0.35
        camera.vignettingPower = 0.6
        context.coordinator.cameraNode.camera = camera
        scene.rootNode.addChildNode(context.coordinator.cameraNode)
        view.pointOfView = context.coordinator.cameraNode

        let ambient = SCNNode()
        ambient.light = SCNLight()
        ambient.light?.type = .ambient
        ambient.light?.intensity = 120
        ambient.light?.color = NSColor(red: 0.95, green: 0.96, blue: 1.0, alpha: 1)  // near-neutral: true mask colours
        scene.rootNode.addChildNode(ambient)

        // Key light: soft shadows of the parts on the board and of the board on the floor.
        let key = SCNNode()
        key.light = SCNLight()
        key.light?.type = .directional
        key.light?.intensity = 1100
        key.light?.color = NSColor(red: 1.0, green: 0.97, blue: 0.92, alpha: 1)
        key.light?.castsShadow = true
        key.light?.shadowMode = .deferred
        key.light?.shadowSampleCount = 16
        key.light?.shadowRadius = 6
        key.light?.shadowMapSize = CGSize(width: 4096, height: 4096)
        key.light?.automaticallyAdjustsShadowProjection = true
        key.light?.shadowColor = NSColor(white: 0, alpha: 0.55)
        key.eulerAngles = SCNVector3(-1.05, 0.55, 0)
        scene.rootNode.addChildNode(key)

        let rim = SCNNode()
        rim.light = SCNLight()
        rim.light?.type = .directional
        rim.light?.intensity = 450
        rim.light?.color = NSColor(red: 0.80, green: 0.86, blue: 1.0, alpha: 1)
        rim.eulerAngles = SCNVector3(-0.4, -2.4, 0)
        scene.rootNode.addChildNode(rim)

        // Invisible floor that only receives the board's shadow.
        let floor = SCNFloor()
        floor.reflectivity = 0
        let shadowOnly = SCNMaterial()
        shadowOnly.lightingModel = .shadowOnly
        floor.materials = [shadowOnly]
        context.coordinator.floorNode.geometry = floor
        scene.rootNode.addChildNode(context.coordinator.floorNode)

        scene.rootNode.addChildNode(context.coordinator.boardNode)
        scene.rootNode.addChildNode(context.coordinator.clashNode)
        scene.rootNode.addChildNode(context.coordinator.selectedNode)
        context.coordinator.view = view
        view.addGestureRecognizer(NSClickGestureRecognizer(target: context.coordinator, action: #selector(Coordinator.clicked(_:))))
        return view
    }

    func updateNSView(_ view: SCNView, context: Context) {
        let c = context.coordinator
        c.board = board
        c.onClick = onClick
        c.onSaveView = onSaveView
        let meshKey = ["\(look.hiddenParts.sorted())", "\(look.standoffs)", "\(look.enclosure)", "\(look.fold)"]
        if c.lastRevision != revision || c.lastComponents != includeComponents || c.lastFinish != finish || c.lastMesh != meshKey {
            // Only an edit (side-by-side with an editor): rebuild once the edits pause, not on every one.
            let onlyEdit = c.lastRevision >= 0 && c.lastComponents == includeComponents && c.lastFinish == finish && c.lastMesh == meshKey
            c.lastRevision = revision
            c.lastComponents = includeComponents
            c.lastFinish = finish
            c.lastMesh = meshKey
            c.pendingRebuild?.cancel()
            if onlyEdit {
                let work = DispatchWorkItem { [c] in
                    MainActor.assumeIsolated {
                        rebuild(c)
                        applyLook(c)
                    }
                }
                c.pendingRebuild = work
                DispatchQueue.main.asyncAfter(deadline: .now() + 0.25, execute: work)
            } else {
                rebuild(c)
                if c.lastReset == 0 { placeCamera(c, view: view) }
            }
        }
        if c.lastReset != resetToken {
            c.lastReset = resetToken
            placeCamera(c, view: view)
        }
        applyLook(c)
        if c.lastClashes != clashes {
            c.lastClashes = clashes
            showBoxes(c.clashNode, clashes, colour: .systemRed)
        }
        if c.lastSelected != selected {
            c.lastSelected = selected
            showBoxes(c.selectedNode, selected, colour: .systemCyan)
        }
        if let camera, camera.id != c.lastCamera {
            c.lastCamera = camera.id
            run(camera.action, c, view: view)
        }
    }

    /// Surfaces hidden or shown, parts lifted (explode) and the section cut, without rebuilding the mesh.
    private func applyLook(_ c: Coordinator) {
        let partSurfaces: Set<String> = ["plastic", "ceramic", "glass", "marking", "tin", "gold"]
        let cut = look.section.map { CGFloat(($0 - 0.5) * board.width) }
        c.boardNode.enumerateChildNodes { node, _ in
            guard let name = node.name, name.hasPrefix("surface.") else { return }
            let surface = String(name.dropFirst("surface.".count))
            node.isHidden = look.hiddenSurfaces.contains { "\($0)" == surface }
            node.position.y = partSurfaces.contains(surface) ? CGFloat(look.explode * 8) : 0
            for m in node.geometry?.materials ?? [] {
                if let cut {
                    if m.shaderModifiers == nil { m.shaderModifiers = [.fragment: Self.sectionShader] }
                    m.setValue(NSNumber(value: Double(cut)), forKey: "sectionX")
                } else if m.shaderModifiers != nil {
                    m.shaderModifiers = nil
                }
            }
        }
    }

    /// See-through boxes round bodies: red for the parts the 3D clearance DRC flagged, cyan for the selection.
    private func showBoxes(_ parent: SCNNode, _ bodies: [SnapBody], colour: NSColor) {
        parent.childNodes.forEach { $0.removeFromParentNode() }
        let material = SCNMaterial()
        material.diffuse.contents = colour.withAlphaComponent(0.4)
        material.emission.contents = colour.withAlphaComponent(0.3)
        material.isDoubleSided = true
        for b in bodies {
            let box = SCNBox(width: CGFloat(b.w) + 0.2, height: CGFloat(b.h) + 0.2, length: CGFloat(b.d) + 0.2, chamferRadius: 0)
            box.materials = [material]
            let node = SCNNode(geometry: box)
            let y = b.bottom ? -board.thickness - b.h / 2 : b.h / 2
            node.position = SCNVector3(CGFloat(b.x - board.width / 2), CGFloat(y), CGFloat(b.y - board.height / 2))
            parent.addChildNode(node)
        }
    }

    /// Section cut: fragments beyond world x = sectionX are not drawn.
    static let sectionShader = """
    #pragma arguments
    float sectionX;
    #pragma body
    float4 world = scn_frame.inverseViewTransform * float4(_surface.position, 1.0);
    if (world.x > sectionX) { discard_fragment(); }
    """

    private func run(_ action: CameraCommand.Action, _ c: Coordinator, view: SCNView) {
        let span = CGFloat(max(board.width, board.height))
        var target = SCNVector3(0, 0, 0)
        switch action {
        case .preset(let preset):
            let d = span * 1.6
            var position = SCNVector3(d * 0.6, d * 0.6, d * 0.6), up = SCNVector3(0, 1, 0)
            switch preset {
            case .top: position = SCNVector3(0, d, 0.001); up = SCNVector3(0, 0, -1)
            case .bottom: position = SCNVector3(0, -d, 0.001); up = SCNVector3(0, 0, 1)
            case .front: position = SCNVector3(0, 0.15 * d, d)
            case .back: position = SCNVector3(0, 0.15 * d, -d)
            case .left: position = SCNVector3(-d, 0.15 * d, 0)
            case .right: position = SCNVector3(d, 0.15 * d, 0)
            case .iso: break
            }
            c.cameraNode.position = position
            c.cameraNode.look(at: target, up: up, localFront: SCNVector3(0, 0, -1))
        case .focus(let p):
            target = SCNVector3(CGFloat(p.x - board.width / 2), 0, CGFloat(p.y - board.height / 2))
            c.cameraNode.position = SCNVector3(target.x, span * 0.35, target.z + span * 0.35)
            c.cameraNode.look(at: target)
        case .save:
            c.saved = (c.cameraNode.position, c.cameraNode.orientation)
            return
        case .saveImage(let url):
            if let tiff = view.snapshot().tiffRepresentation,
               let png = NSBitmapImageRep(data: tiff)?.representation(using: .png, properties: [:]) {
                try? png.write(to: url)
            }
            return
        case .saveUSDZ(let url):
            let scene = SCNScene()
            scene.rootNode.addChildNode(c.boardNode.clone())
            scene.write(to: url, options: nil, delegate: nil, progressHandler: nil)
            return
        case .saveAs(let name):
            let p = c.cameraNode.position, o = c.cameraNode.orientation
            c.onSaveView(name, [p.x, p.y, p.z, o.x, o.y, o.z, o.w].map { Double($0) })
            return
        case .go(let v):
            guard v.count == 7 else { return }
            c.cameraNode.position = SCNVector3(CGFloat(v[0]), CGFloat(v[1]), CGFloat(v[2]))
            c.cameraNode.orientation = SCNQuaternion(CGFloat(v[3]), CGFloat(v[4]), CGFloat(v[5]), CGFloat(v[6]))
        case .restore:
            guard let saved = c.saved else { return }
            c.cameraNode.position = saved.0
            c.cameraNode.orientation = saved.1
        }
        view.pointOfView = c.cameraNode
        view.defaultCameraController.pointOfView = c.cameraNode
        view.defaultCameraController.target = target
    }

    private func rebuild(_ c: Coordinator) {
        c.boardNode.childNodes.forEach { $0.removeFromParentNode() }
        // Below the board and anything hanging under it (bottom-side parts, header tails).
        c.floorNode.position = SCNVector3(0, -CGFloat(board.thickness) - 9, 0)
        let options = "\(includeComponents) \(look.hiddenParts.sorted()) \(look.standoffs) \(look.enclosure) \(look.fold)"
        guard let mesh = MeshCache.mesh(engine, revision, options, {
            engine.buildMesh(includeComponents: includeComponents, hidden: look.hiddenParts,
                             standoffs: look.standoffs ? 6 : 0, enclosure: look.enclosure, fold: look.fold)
        }) else { return }
        let node = Self.assemblyNode(from: mesh, finish: finish)
        // Centre the board on the origin (mesh is in board millimetres).
        node.position = SCNVector3(-board.width / 2, 0, -board.height / 2)
        c.boardNode.addChildNode(node)
        let triangles = mesh.indices.count / 3
        DispatchQueue.main.async {
            stats = "\(mesh.vertexCount) vertices · \(triangles) triangles"
        }
    }

    private func placeCamera(_ c: Coordinator, view: SCNView) {
        let span = max(board.width, board.height)
        c.cameraNode.position = SCNVector3(0, span * 1.1, span * 1.25)
        c.cameraNode.look(at: SCNVector3(0, 0, 0))
        view.pointOfView = c.cameraNode
        // Reset also re-centres the orbit (a pan moves the controller's target away from the board).
        view.defaultCameraController.pointOfView = c.cameraNode
        view.defaultCameraController.target = SCNVector3(0, 0, 0)
    }

    // MARK: - Materials

    /// One child geometry per surface, all sharing the mesh's vertex buffers. O(triangles) to split.
    static func assemblyNode(from mesh: MeshData, finish: BoardFinish) -> SCNNode {
        let root = SCNNode()
        guard mesh.surfaces.count == mesh.vertexCount, mesh.vertexCount > 0 else {
            root.addChildNode(SCNNode(geometry: geometry(from: mesh)))  // untagged mesh: one material
            return root
        }
        var buckets = [[UInt32]](repeating: [], count: MeshSurface.allCases.count)
        var t = 0
        while t + 2 < mesh.indices.count {
            let surface = Int(mesh.surfaces[Int(mesh.indices[t])])
            if surface < buckets.count { buckets[surface].append(contentsOf: mesh.indices[t...(t + 2)]) }
            t += 3
        }
        let (vertices, normals, colours) = sources(from: mesh)
        for surface in MeshSurface.allCases where !buckets[Int(surface.rawValue)].isEmpty {
            let indices = buckets[Int(surface.rawValue)]
            let element = SCNGeometryElement(data: indices.withUnsafeBufferPointer { Data(buffer: $0) },
                                             primitiveType: .triangles, primitiveCount: indices.count / 3,
                                             bytesPerIndex: MemoryLayout<UInt32>.size)
            // The finish takes its colour from the chosen plating, not from the mesh.
            let sources = surface == .finish ? [vertices, normals] : [vertices, normals, colours]
            let geometry = SCNGeometry(sources: sources, elements: [element])
            geometry.materials = [material(for: surface, finish: finish)]
            let node = SCNNode(geometry: geometry)
            node.name = "surface.\(surface)"
            if surface == .glass { node.renderingOrder = 10 }  // after the opaque board
            root.addChildNode(node)
        }
        return root
    }

    /// Physically based look of each surface (base colours come from the mesh except for the finish).
    static func material(for surface: MeshSurface, finish: BoardFinish) -> SCNMaterial {
        let m = SCNMaterial()
        m.lightingModel = .physicallyBased
        m.diffuse.contents = NSColor.white
        m.isDoubleSided = false
        func pbr(metal: CGFloat, rough: CGFloat) {
            m.metalness.contents = metal
            m.roughness.contents = rough
        }
        switch surface {
        case .mask:
            pbr(metal: 0, rough: 0.38)
            m.clearCoat.contents = 0.55          // the lacquer's gloss over the colour
            m.clearCoatRoughness.contents = 0.18
        case .laminate: pbr(metal: 0, rough: 0.78)
        case .finish:
            m.diffuse.contents = finish.colour
            pbr(metal: 1, rough: finish.roughness)
        case .gold: pbr(metal: 1, rough: 0.2)
        case .tin: pbr(metal: 1, rough: 0.3)
        case .solder: pbr(metal: 1, rough: 0.16)
        case .silk: pbr(metal: 0, rough: 0.88)
        case .plastic: pbr(metal: 0, rough: 0.6)
        case .ceramic: pbr(metal: 0, rough: 0.5)
        case .glass:
            pbr(metal: 0, rough: 0.06)
            m.transparency = 0.82
            m.blendMode = .alpha
            m.isDoubleSided = true
        case .hole: pbr(metal: 0, rough: 0.95)
        case .marking: pbr(metal: 0, rough: 0.8)
        }
        return m
    }

    private static func sources(from mesh: MeshData) -> (SCNGeometrySource, SCNGeometrySource, SCNGeometrySource) {
        let vertexCount = mesh.vertexCount
        let floatSize = MemoryLayout<Float>.size
        let vertices = SCNGeometrySource(data: mesh.positions.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .vertex,
                                         vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 3,
                                         bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 3)
        let normals = SCNGeometrySource(data: mesh.normals.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .normal,
                                        vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 3,
                                        bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 3)
        let colours = SCNGeometrySource(data: mesh.colors.withUnsafeBufferPointer { Data(buffer: $0) }, semantic: .color,
                                        vectorCount: vertexCount, usesFloatComponents: true, componentsPerVector: 4,
                                        bytesPerComponent: floatSize, dataOffset: 0, dataStride: floatSize * 4)
        return (vertices, normals, colours)
    }

    // MARK: - Studio

    /// Equirectangular studio: a dim floor, a bright overhead and two soft boxes that metals and the glossy mask
    /// reflect. Drawn once (1024 × 512, 2 MB).
    static let studioEnvironment: NSImage = HoloFX.render(NSSize(width: 1024, height: 512)) {
        let full = NSRect(x: 0, y: 0, width: 1024, height: 512)
        NSGradient(colorsAndLocations:
                    (NSColor(white: 0.06, alpha: 1), 0.0),
                   (NSColor(red: 0.20, green: 0.22, blue: 0.26, alpha: 1), 0.48),
                   (NSColor(red: 0.55, green: 0.58, blue: 0.64, alpha: 1), 0.56),
                   (NSColor(white: 0.85, alpha: 1), 1.0))?.draw(in: full, angle: 90)
        func softbox(_ rect: NSRect, _ brightness: CGFloat) {
            for k in 0..<6 {  // feathered edge
                let inset = CGFloat(5 - k) * 6
                NSColor(white: 1, alpha: brightness * CGFloat(k + 1) / 6).setFill()
                NSBezierPath(roundedRect: rect.insetBy(dx: -inset, dy: -inset), xRadius: 18, yRadius: 18).fill()
            }
        }
        softbox(NSRect(x: 140, y: 380, width: 220, height: 70), 0.95)  // key side
        softbox(NSRect(x: 620, y: 360, width: 160, height: 90), 0.7)   // fill side
        softbox(NSRect(x: 420, y: 470, width: 200, height: 30), 0.9)   // overhead strip
    }

    /// Vertical gradient behind the board in the app theme (Midnight Navy: navy); drawn per view, so a new theme shows.
    static var backdrop: NSImage {
        HoloFX.render(NSSize(width: 4, height: 256)) {
            NSGradient(colors: [AppTheme.backdrop(0.01, 0.02, 0.05).nsColor, AppTheme.backdrop(0.05, 0.09, 0.17).nsColor])?
                .draw(in: NSRect(x: 0, y: 0, width: 4, height: 256), angle: 90)
        }
    }

    static func geometry(from mesh: MeshData) -> SCNGeometry {
        let vertexCount = mesh.vertexCount
        let positions = mesh.positions.withUnsafeBufferPointer { Data(buffer: $0) }
        let normals = mesh.normals.withUnsafeBufferPointer { Data(buffer: $0) }
        let colors = mesh.colors.withUnsafeBufferPointer { Data(buffer: $0) }
        let indices = mesh.indices.withUnsafeBufferPointer { Data(buffer: $0) }
        let floatSize = MemoryLayout<Float>.size

        let vertexSource = SCNGeometrySource(data: positions, semantic: .vertex, vectorCount: vertexCount,
                                             usesFloatComponents: true, componentsPerVector: 3, bytesPerComponent: floatSize,
                                             dataOffset: 0, dataStride: floatSize * 3)
        let normalSource = SCNGeometrySource(data: normals, semantic: .normal, vectorCount: vertexCount,
                                             usesFloatComponents: true, componentsPerVector: 3, bytesPerComponent: floatSize,
                                             dataOffset: 0, dataStride: floatSize * 3)
        let colorSource = SCNGeometrySource(data: colors, semantic: .color, vectorCount: vertexCount,
                                            usesFloatComponents: true, componentsPerVector: 4, bytesPerComponent: floatSize,
                                            dataOffset: 0, dataStride: floatSize * 4)
        let element = SCNGeometryElement(data: indices, primitiveType: .triangles,
                                         primitiveCount: mesh.indices.count / 3, bytesPerIndex: MemoryLayout<UInt32>.size)
        let geometry = SCNGeometry(sources: [vertexSource, normalSource, colorSource], elements: [element])
        let material = SCNMaterial()
        material.lightingModel = .physicallyBased
        material.diffuse.contents = NSColor.white
        material.metalness.contents = 0.15
        material.roughness.contents = 0.45
        material.isDoubleSided = false
        geometry.materials = [material]
        return geometry
    }
}

/// The core meshes of the current design revision, kept when a 3D view is made again for the same board (a theme
/// change or a workspace switch remakes the views): building them is the slow part. A new revision drops them all.
enum MeshCache {
    private static weak var engine: EDAEngine?  // weak: a later engine at the same address never reads its meshes
    private static var revision = -1
    private static var meshes: [String: MeshData] = [:]

    static func mesh(_ engine: EDAEngine, _ revision: Int, _ key: String, _ build: () -> MeshData?) -> MeshData? {
        if self.engine !== engine || self.revision != revision {
            self.engine = engine
            self.revision = revision
            meshes = [:]
        }
        if let hit = meshes[key] { return hit }
        let mesh = build()
        meshes[key] = mesh
        return mesh
    }
}
