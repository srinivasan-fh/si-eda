# SiEDA — notes for Claude Code

- Core (C++): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests`
- macOS app: `./run.sh` (builds the core, generates the Xcode project, runs the app).
- Footprint Editor: core in `Core/src/CustomParts.cpp` (`landPatternFromFootprint`, `checkLandPattern`), app in
  `SiEDA/Models/FootprintDraft.swift` and `SiEDA/Views/Library/FootprintEditorView.swift`; guide in
  `docs/FOOTPRINT_EDITOR.md`. Keep the Swift `CustomPartSpec.Land` encoding identical to the core's land JSON.
- Symbol Editor: core in `Core/src/CustomParts.cpp` (`autoArrangeSymbol`, `checkSymbol`, four-sided layout) and
  `Core/src/Schematic.cpp` (stacked pins join nets), app in `SiEDA/Models/SymbolDraft.swift` and
  `SiEDA/Views/Library/SymbolEditorView.swift`; guide in `docs/SYMBOL_EDITOR.md`.
- Memory (RAM) design segments: core in `Core/src/Memory.cpp` (`memoryChecks`, `memorySegments`, routed DDR layout
  rules `ddrLayoutChecks` / `ddrLayoutLimits` with `Project::memoryLimits` overrides from the controller's guide,
  DQS-to-lane `MEM_DDR_DQS_SKEW`, CK / DQS P-to-N `MEM_DDR_PAIR_SKEW`: MEM_DDR_* warnings on DDR, info on SDR), app via
  `EDAEngine.memorySegments` / `DesignStore.setMemoryDesign`; guide in `docs/MEMORY_DESIGN.md`. The memory reference design
  (STM32H743 + SDRAM) must keep passing verification on 6 layers.
- Schematic capture: sheets / hierarchy / bus labels / annotation in `Core/src/Sheets.cpp`, repeated sheets in
  `Core/src/Instances.cpp` (`syncInstances` keeps channel copies in line; every Schematic edit calls it), graphical
  buses in `Core/src/Buses.cpp`, multi-unit parts in `Core/src/PartUnits.cpp` (kind PartUnit + hidden `packageOnly`
  package; the snapshot reports units as kind Custom with `unitOf`), find / replace and the net navigator in
  `Core/src/SchematicSearch.cpp`, variant simulation via `Project::simulationSchematic`; Update PCB (forward ECO,
  `pcbSync` baseline) in `Core/src/Eco.cpp` (its new parts placed by hand afterwards: `Core/src/InteractivePlacement.cpp`,
`SiEDA/App/DesignStore+Placement.swift`, `SiEDA/Views/PCB/PlaceNewPartsOverlay.swift`), PCB pin / gate swap in `Core/src/PcbSwap.cpp`, schematic PDF in
  `Core/src/SchematicPdf.cpp` with TrueType subsetting in `Core/src/PdfFont.cpp`; app in `SiEDA/Views/Schematic/`;
  guide in `docs/SCHEMATIC.md`. Symbol graphics: `SymbolSpec::graphics` (core) = `CustomPartSpec.SymbolGraphic`
  (Swift); keep their JSON identical. Nested repetition and per-channel values
  (`channelOverrides`) are in `Instances.cpp` (`syncNestedSheets`); harnesses in `Harnesses.cpp`; net classes /
  directives in `Directives.cpp` (carried to the board by `Project::applySchematicRules`); align / copy / paste in
  `SchematicEdit.cpp`; back-annotation ECO in `Eco.cpp`; sheet templates and the PDF in `SchematicPdf.cpp`; unit
  (gate) checks `checkUnits` in `CustomParts.cpp`, app `SiEDA/Models/UnitDraft.swift` + `UnitEditorView.swift`. AI
  plans keep this structure (`DesignPlanCompiler.plan(from:)` / `apply` / `preservingStructure`); new project fields
  are written only when used so older files load and save identically.
  Canvas colour schemes / grid styles / custom named-colour theme: `SiEDA/Views/Schematic/SchematicPalette.swift`
  (+ `SchematicCustomTheme.swift`, `SchematicAppearanceViews.swift`); the canvas draws only with the palette (no
  fixed `Theme` schematic colours), and Midnight Navy (`siedaDark`) must stay today's colours. Symbols are drawn in
  their device colour (`ComponentKind.deviceColour`) by default; "Colour Symbols by Device" off = the scheme's symbol colour.
  App-wide theme (Midnight Navy / Matrix Green / Graphite): `SiEDA/Views/Common/AppTheme.swift`; `Theme` colours read
  `AppTheme.current`, the app redraws through `.id(appTheme)`; Midnight Navy must stay today's blues; board colours
  (copper, pads, errors) stay standard in every theme.
- Simulation: MNA core in `Core/src/Simulator.cpp` (elements in `SimulatorInternal.hpp`), SPICE model import in
  `Core/src/SpiceModels.cpp` (parser / flattening, fuzz-hardened) + `SpiceDevices.cpp` (device equations) +
  `SpiceBuild.cpp` (models and op-amp macromodel in the simulator), noise in `Noise.cpp`, adaptive / trapezoidal
  transient and convergence aids in `Convergence.cpp`, `.meas`-like measurements in `Waveforms.cpp`; app in
  `SiEDA/Views/Simulation/` and `SiEDA/App/DesignStore+Simulation.swift`; guide in `docs/SIMULATION.md`. Parts without
  an imported model must keep their exact results: new behaviour is opt-in (a model, macromodel parameters, transient
  options, `setConvergenceAids`).
- Launch splash: `SiEDA/App/SplashScreen.swift` (`SplashModel` preload steps, `SplashController` holds main windows from
  `applicationWillFinishLaunching`).
- Interface languages: `SiEDA/App/AppLanguage.swift`, translations in `SiEDA/Resources/<code>.lproj/Localizable.strings`
  (20 languages, English keys); guide in `docs/LOCALIZATION.md`. A new UI string needs a key in every table —
  `python3 tools/check_localization.py --missing` lists the gaps.
- Supplier data: core in `Core/src/Suppliers.cpp` (Nexar / DigiKey v4 / Mouser v2 readers into the `sieda.supplier/1`
  schema, price breaks, BOM roll-up, catalog match), app in `SiEDA/Suppliers/` (URLSession clients, Keychain keys,
  offline cache), `SupplierSearchView`, `BomLivePricingView`; guide in `docs/SUPPLIERS.md`. Tests never use the
  network: core fixtures in `Core/tests/fixtures/suppliers/`, app tests stub `URLProtocol`. Never hard-code a key.
- Altium libraries: `Core/src/AltiumLibrary.cpp` (MS-CFB `CompoundFile`, SchLib / PcbLib records), converted in
  `LibraryImport.cpp` (`importAltium`); fixtures written by `tools/make_altium_fixtures.py` (don't edit the binaries).
- 3D models of parts: core in `Core/src/Model3D.cpp` (VRML 2.0 / STL / OBJ readers, `Model3DRegistry`, project
  `models3d`, `appendModel3D` in `buildAssemblyMesh`); a part refers to its mesh by `CustomPartSpec::model3d`; app
  `SiEDA/Views/Library/Model3DEditorView.swift`. STEP is deliberately not read (no CAD kernel).
- Interactive routing: `Core/src/InteractiveRouter.cpp` (router, shove, arc corners, commands), arc tracks in
  `Core/include/sieda/TrackGeometry.hpp` (`Track::arc`: measure tracks only through these functions, never `a`–`b`;
  straight tracks must stay bit-identical), board commands (teardrops as `Track::teardrop` fans, via stitching /
  shielding, gloss, loop removal on commit, hug drag, Stop mode, `matchTrackLengths` — opt-in router options, so default
  routing is unchanged; head searches may run on several threads but must give the sequential result), C API additions in `Core/src/sieda_c_routing.cpp`; app in
  `SiEDA/Views/PCB/`, `SiEDA/App/DesignStore+Routing.swift`, `SiEDA/Bridge/EDAEngine+Routing.swift`; guide in
  `docs/INTERACTIVE_ROUTING.md`.
- Autorouter: `Core/src/Pcb.cpp` (`PcbLayout::routeAll`, `runPass`; classic router below 2 M grid nodes — keep its
  copper and DRC bit-identical, check with the reference designs), corridor router + `GlobalRouter.cpp`, coupled pairs
  in `Core/src/CoupledPairRouter.inc`, post-route passes (length-aware tuning, gloss, arcs, teardrops, metrics) in
  `Core/src/RouteQuality.cpp`; strategy options `AutorouteOptions` (`BoardSettings::autorouter`, every default = the old
  behaviour; JSON in `Core/src/Autoroute.cpp`), keep-outs, C API in `Core/src/sieda_c_autoroute.cpp`; app
  `SiEDA/Views/PCB/RoutingStrategy*.swift`, `RoutingReportSheet.swift`, `SiEDA/App/DesignStore+Autoroute.swift`; guide in
  `docs/ROUTING.md`. `SIEDA_ROUTE_PROFILE=1` prints phase timings; the corridor router must give the same copper for
  any thread count.
- MCP (any AI client drives SiEDA): engine in `Core/src/Mcp.cpp` (JSON-RPC dispatch, root-folder sandbox, resources,
  prompts, examples; `Core/include/sieda/Mcp.hpp`), the data-driven tool table in `Core/src/McpTools.cpp` (handlers
  call the C API), PNG / SVG renders in `Core/src/McpRender.cpp`, C API `sieda_mcp_*` in `Core/src/sieda_c_mcp.cpp`
  (the app's live endpoint reuses it), stdio server `sieda-mcp` in `Core/mcp/main.cpp`; guide in `docs/MCP.md` (its
  tool reference is `sieda-mcp --list-tools-markdown`). Tool failures are `isError` results, never crashes; file
  access stays inside `allowedRoot`; read-only mode refuses tools with `mutates`. Live app endpoint (off by default,
  127.0.0.1 only, bearer token in the Keychain, Origin / Host check): `SiEDA/App/MCPEndpoint.swift` (listener, HTTP
  parser, gate), `SiEDA/App/DesignStore+MCP.swift` (`MCPLiveServer`, `MCPStoreBridge`: mutating tools run in
  `DesignStore.performExternalEdit` = one undo step), `SiEDA/Views/Settings/MCPSettingsView.swift`; stdio clients
  reach it through `sieda-mcp --connect` (`Core/mcp/http_bridge.cpp`, CTest `sieda_mcp_connect`).
- IPC-2581C / ODB++ v7: `Core/src/FabExchange.cpp` (one per-layer feature list feeds both; ODB++ is a .tgz written by
  `tgz` with stored deflate blocks; both are in the fabrication package, IPC-2581 also as export "ipc2581"). Keep the
  XML valid against the IPC-2581C schema (KiCad's `qa/data/pcbnew/ipc2581/IPC-2581C.xsd`).
- Manufacturer DFM / DFA packs: `Core/src/Dfm.cpp` (data table of fab / assembly limits; `dfmOverrides` = the fab's own values over a pack via `boardDfmPack`; `applyDfmPack` only tightens
  DRC minimums, `dfmChecks` adds DFM_* / DFA_* (incl. DFM_ASPECT_RATIO, DFM_COPPER_BALANCE via `copperCoverage`) to `runDRC`
  when `BoardSettings::dfmPack` is set; `dfmReportJson` / `sieda_dfm_report_json` is the per-rule sign-off; saved only when set;
  C API `sieda_dfm_packs_json` / `sieda_pcb_set_dfm_pack`, MCP `pcb_dfm_pack`), app Board Setup → Manufacturer Rules.
- PI: lumped PDN / IR drop in `Core/src/PowerIntegrity.cpp`; cavity model, plane mesh on the real pour shape
  (`planeMeshImpedance`, RLGC grid from the IR map's cells, banded LU), droop, decap plan in `Core/src/PdnPlanning.cpp`.
- Field solver: `Core/src/FieldSolver.cpp` (2D Laplace, finite volumes on a graded grid, Jacobi-CG; C and C0 give Z0,
  εeff, L / C, odd / even, kb / kf; `lineLoss`: skin-effect R from the air solution's surface charge, Hammerstad
  roughness, G from Df with the filling factor; `trackGeometry` reads the stack-up and coats outer layers with 20 µm of solder mask (`FieldGeometry::mask`, coated microstrip); tests hold it within 1.5 % of exact stripline,
  Cohn coupled stripline and Hammerstad–Jensen), C API `sieda_field_solve`, MCP `si_field_solver`, app Board Setup →
  Stack-up → Check with Field Solver; opt-in `SiSettings::fieldSolverLines` / `LossOptions::fieldSolver` feeds the
  channel lines (`lineModel`, cached per geometry). The closed-form widths are unchanged.
- Production panels: `Core/src/Panel.cpp` (`BoardSettings::panel`, saved only when nx × ny > 1; `fitPanel` = most boards
  within a fab panel size, default the DFM pack's; panel Gerbers are the
  board's own Gerbers shifted and stepped with %SR, drills repeated per board; C API `sieda_pcb_panel` /
  `_set_panel`, MCP `pcb_panel`), app Board Setup → Production Panel (`PanelPreview`).
- Mechanical CAD and team work: STEP AP214 / IDF 3.0 export and the IDF placement import in `Core/src/Mechanical.cpp`
  (faceted B-rep solids must stay closed: test `step_export_closed_named_solids`), 3D clearance DRC
  (`mechanicalChecks`: MECH_BODY_COLLISION always, MECH_HEIGHT / MECH_HEIGHT_ZONE only with `BoardSettings` limits);
  IDX / EDMD v4.5 baseline (with keep-outs / height zones), change file, import of placement, outline, thickness and
  keep-outs (`exportIdx`, `exportIdxChanges`, `importIdx`, `importIdxPlacement`), accept / reject `idxResponse`,
  namespace-agnostic XML reader, fuzzed in the `idf` target);
  version diff, three-way merge (`mergeProjects`, `sieda-mcp --merge`), design review comments
  (`Project::reviewComments`, saved only when present), Git drivers (`--diff` / `--git-diff`) and the variant matrix
  in `Core/src/ProjectDiff.cpp`; app
  `SiEDA/App/DesignStore+Team.swift`, `SiEDA/Views/Common/TeamViews.swift`; live co-editing of a shared file
  (auto-save, 2 s watch, three-way merge as one undo step, presence folder) in `SiEDA/App/LiveCollaboration.swift`;
  guides `docs/MCAD.md`, `docs/TEAM.md`.
  Scale guard: CTest `sieda_scale_budget` (`sieda_route_bench --budget`); `sieda_route_bench --clusters 32 --layers 8
  --seed 3 --fpga` is the 923-part board.
- Speed and safety rules:
  - **Per-edit path:** undo history and change detection use the compact `sieda_project_state_json`; files on disk
    use the indented `sieda_project_save_json`. The view refresh uses `sieda_project_snapshot_delta` (only changed
    top-level sections, merged in `DesignSnapshot.init(from:)` over `DesignStore.coreSnapshot`); a new snapshot section
    must be decoded through `field(...)` there.
  - **JSON writer:** `Json::dump` uses `std::to_chars` and a no-escape fast path, and must stay byte-identical to
    `%.10g` / `%lld`.
  - **JSON parser:** limits nesting to 256 levels (hostile files and requests).
  - **Measured:** every Schematic / board edit records its undo-state, engine and refresh time (`DesignStore.editTimings`,
    Help → Performance…, os_signpost "edit" intervals for Instruments); `sieda_route_bench --edit-budget <ms>` fails when
    the app's per-edit core path (move + undo state + delta snapshot) is slower at p95 (in CTest `sieda_scale_budget`).
  - **Copper pours:** fills (`Zones.cpp`) use running-count filters, union-find island labels and one thread per
    layer, and must stay identical to the sequential fill.
  - **MCP:** file reads are capped at 128 MB.
  - **Live endpoint:** at most 16 connections.
  - **`sieda-mcp --connect`:** only connects to 127.0.0.1 / localhost / ::1 and caps replies at 64 MB.
- Library parts live in `Core/src/StandardParts.cpp`; KiCad-derived pinouts are generated into
  `Core/src/StandardCatalog.inc` by `tools/fetch_catalog_parts.py` — don't edit that file by hand. More parts from
  whole KiCad libraries: `--discover` freezes `tools/catalog_extra_parts.tsv`, `--extra` writes
  `Core/src/StandardCatalogExtra.inc` (don't edit either by hand; `EXCLUDE` in the script lists symbols the core
  cannot draw). AI prompts get a digest of the catalog (`SiEDA/AI/CatalogDigest.swift`), never the whole list.

## Missing IC parts

Fourteen catalog parts still need their pinouts from the vendor datasheet. The list, one file per part, and the
step-by-step procedure (fill the pin table, add a `catalog(...)` entry, update the tests) are in
[`docs/missing-parts/README.md`](docs/missing-parts/README.md). When asked to add or update missing parts, follow that
file and keep each part's Status line current.
