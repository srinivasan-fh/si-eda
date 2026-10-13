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
  `Core/src/SchematicPdf.cpp` with TrueType subsetting and optional HarfBuzz shaping (`TrueTypeFont::shape`, CMake `SIEDA_HARFBUZZ`) in `Core/src/PdfFont.cpp`; app in `SiEDA/Views/Schematic/`;
  guide in `docs/SCHEMATIC.md`. Several boards per project (one schematic): `Project::boardNames` / `activeBoard`,
  `pcb` = the active board, the others in `parkedBoards` with their parts' `parkedPlacements`; `PcbPlacement::board`,
  `PcbLayout::onBoard` (Auto Place and the unplaced checks skip other boards' parts); saved only with ≥ 2 boards; app
  `BoardMenu` in `PCBEditorView.swift`, MCP `pcb_boards`. Symbol graphics: `SymbolSpec::graphics` (core) = `CustomPartSpec.SymbolGraphic`
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
  `Core/src/SpiceModels.cpp` (parser / flattening, fuzz-hardened, `inlineSpiceIncludes` for files beside the model) + `SpiceDevices.cpp` (device equations: SPICE3
  temperature updates per model TNOM, MOS3 level 3 `channel3`, switch hysteresis `$SWH` state in `SpiceExpr::state`) +
  `SpiceBuild.cpp` (models and op-amp macromodel in the simulator), noise in `Noise.cpp`, adaptive / trapezoidal
  transient and convergence aids in `Convergence.cpp`, `.meas`-like measurements in `Waveforms.cpp`; simulation
  temperature `Simulator::setTemperature` (27 °C default = every result unchanged; JSON `"temperature"` on every analysis,
  `temperatureSweep` / `sieda_simulate_temperature_sweep` / MCP `sim_temperature_sweep`); app in
  `SiEDA/Views/Simulation/` and `SiEDA/App/DesignStore+Simulation.swift`; guide in `docs/SIMULATION.md`. Parts without
  an imported model must keep their exact results: new behaviour is opt-in (a model, macromodel parameters, transient
  options, `setConvergenceAids`, `setTemperature`).
- Built-in AI model (no outside app): engine in `Core/src/LocalModel.cpp` (GGUF reader, F16…Q6_K blocks, Llama / Mistral /
  Qwen transformer, BPE + SentencePiece tokenizers; tokens equal and logits within 0.5 % of exact — tests against
  llama.cpp fixtures from `tools/make_llm_fixtures.py`), C API `sieda_llm_*` in `Core/src/sieda_c_llm.cpp`, CLI
  `sieda-cli --chat`; app `SiEDA/AI/BuiltInModel.swift` (`BuiltInProvider`, Metal GPU matmul `MetalMatmul.swift` via `sieda_llm_set_accelerator`, `LocalModelStore` downloads / imports /
  deletes models, `BuiltInModelsView`); Window → Super Intelligence (`SiEDA/AI/IntelligenceWindow.swift`: sidebar of four use cases — Cloud LLM sign-in, Local LLM on our engine
  (one tab per set-up step), MCP + cloud LLM agent clients, Own LLM = SiEDA's circuit model `SiEDA/AI/CircuitModel.swift`, trained in the app
  (`CircuitTrainer.swift`, `OwnModelViews.swift`: dataset / training / results) or by
  `tools/circuit_lm/` into `SiEDA/Resources/Models/sieda-circuit-v1.{gguf,json}` — regenerate, don't hand-edit; values the request fixes computed by `Core/src/CircuitValues.cpp` (`circuitPlanValues`)); guide in `docs/AI.md`.
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
- 3D models of parts: core in `Core/src/Model3D.cpp` (VRML 2.0 / STL / OBJ readers and the STEP AP203 / AP214 reader
  `parseStep`: Part 21 parser, B-rep faces triangulated in their surface's parameters with seams unwrapped so shells
  stay closed, assembly placements, units, colours; B-spline / unknown surfaces use their boundary and are counted in
  `warnings`; `Model3DRegistry`, project `models3d`, `appendModel3D` in `buildAssemblyMesh`); a part refers to its
  mesh by `CustomPartSpec::model3d`; app `SiEDA/Views/Library/Model3DEditorView.swift`. Tests:
  `step_import_round_trip_of_export` (every exported solid comes back closed, bounds and volume within 1 %),
  `model3d_step_reader_solids_units_colours_assembly`; the STEP reader is in the `library` fuzz target.
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
  any thread count. Placement improvement after the greedy `autoPlace` (`PcbLayout::improvePlacement`, opt-in
  `AutorouteOptions::improvePlacement`); coupled pairs' P/N crossover (`Project::autoPairCrossovers` in
  `Core/src/PcbSwap.cpp`, a pin swap of a unit's pin-swap group, run by `Project::autoRoute` with `coupledPairs`);
  coupled pairs start from BGA balls' dog-bone vias (`PairPlan::viaEnd`) and use each layer's stack-up width / gap
  (`PairPlan::layerWidth`);
  via minimisation tries every net (adaptive batches + one shared grid, serial, `setViaMinimisationGrids` for tests);
  opt-in shape-based completion `AutorouteOptions::shapeCompletion` (`routequality::shapeComplete`: what the grid
  leaves unrouted, routed by the interactive router on the exact shapes); corridor routes get `routequality::fixAcuteJoins` (no DRC_ACUTE_ANGLE; classic copper untouched); opt-in
  `AutorouteOptions::lengthAwareSearch` (with `lengthAware`: longest first, then a `routeAll` stage after via
  minimisation re-routes target nets shorter with cheap vias or longer through a waypoint before the meanders);
  `classic_router_copper_fingerprint` pins the classic router's and greedy placer's result — update its constant
  only on purpose. `sieda_route_bench --options '{json}' --save file` measures strategy options and lets `cmp` compare runs.
- MCP (any AI client drives SiEDA): engine in `Core/src/Mcp.cpp` (JSON-RPC dispatch, root-folder sandbox, resources,
  prompts, examples; `Core/include/sieda/Mcp.hpp`), the data-driven tool table in `Core/src/McpTools.cpp` (handlers
  call the C API), PNG / SVG renders in `Core/src/McpRender.cpp`, C API `sieda_mcp_*` in `Core/src/sieda_c_mcp.cpp`
  (the app's live endpoint reuses it), stdio server `sieda-mcp` in `Core/mcp/main.cpp`; guide in `docs/MCP.md` (its
  tool reference is `sieda-mcp --list-tools-markdown`). Agent-facing results: every placed part's pins carry number, type, net, x / y and
  side; a shared pin name is `REF.NAME(number)`; `schematic_connect` never merges two named nets without `merge`
  and warns on shorted parts; `schematic_erc` gives `unconnectedPins` (one summary line) and `status`;
  `schematic_build` = one-call netlist build (`layoutBuilt` lines wired pins up); `schematic_validate` adds
  `designReview` (`Validation.cpp`: LED without resistor, floating reset / enable / BOOT0, op-amp without output limits,
  decoupling per supply pin, crystal load caps), which the app's verification does not run; `pcb_autoroute` runs
  `finishRouting` (shape completion, turned two-pin parts, kept only when fewer stay open; partial presets per call);
  `RouteKeepout::pours` keeps pours out (saved only when on);
  `finishRouting` also fans fine-pitch parts out first; `project_undo` = `McpServer::undo` (state before each mutating
  call, 30 kept, cleared when the project changes); `placeDecoupling` (opt-in `pcb_autoplace` `decoupling`);
  `schematic_build` lays two-pin parts beside the IC pin of their named net (`layoutBuilt` keeps label room) and
  draws a straight, clear link as a wire named by a mid-wire tag, else labels; `pcb_drc` adds DRC_PLACE_* (courtyard
  off board / overlapping) and DRC_ANTENNA_KEEPOUT; Verification counts parts / nets as `project_summary`, and
  VER_NO_SOURCE is info on a connector-fed board; `StandardPart::footprint` = the catalog row's KiCad footprint;
  `placeDecoupling` also places crystals (Y…) and regulator bulk caps; `removeStranded` rips a moved part's old
  copper. `PcbLayout::pruneStaleRouting` keeps a via reaching no pad when it stands in a pour (stitching);
  `placeAtEdges` (pcb_autoplace all: antenna parts top + keep-out, open-ended connectors bottom, by `openEnd`);
  undo is cleared by project_open / new / load_example; headers without an open end go to the left / right edges in turn, long side along it; crystals are placed before supply caps (small passives step aside, `stepAside`); edge parts largest first; `pcb_autoroute` re-lays stitching vias (`stitchingRestored`) and lists `pairsNotCoupled`; assembly spacing standard `standardGap` (passive 0.25 / passive-IC 0.5 / IC-IC and THT-SMD 1.0 / BGA 2.0 mm) kept by the MCP placement helpers and flagged as DRC_PART_SPACING; `placeGrouped` (pcb_autoplace grouped: rows of like passives); power-only connectors to the left edge; the main IC is centred before Auto Place; opt-in `AutorouteOptions::viasOffPads` (pour fan-out vias clear SMD pads; on for MCP `pcb_autoroute`); `schematic_build` expands a shared pin name to every unstacked pin. Catalog rows may name `PCM_Espressif` footprints, and
  `ALL_PADS` in `tools/fetch_catalog_parts.py` keeps every pad of a footprint. Tool failures are `isError` results, never crashes; file
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
  Cohn coupled stripline and Hammerstad–Jensen), C API `sieda_field_solve` / goal seek `sieda_field_solve_width` (`fieldSolvedWidth`: Illinois regula falsi on ln Z, ~6 solves), MCP `si_field_solver` (`targetOhms`), app Board Setup →
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
  - **JSON values:** 40-byte `std::variant`s; objects are flat vectors in `std::map` key order (`Json::Object`),
    arrays `reserve`d where the count is known. `Json::dump` uses `std::to_chars`, a no-escape fast path and
    `shortDecimal` (≤ 6 decimals by integer arithmetic), and must stay byte-identical to `%.10g` / `%lld`
    (test `json_short_decimals_match_printf`).
  - **JSON parser:** limits nesting to 256 levels (hostile files and requests).
  - **Measured:** every Schematic / board edit records its undo-state, engine and refresh time (`DesignStore.editTimings`,
    Help → Performance…, os_signpost "edit" intervals for Instruments); `sieda_route_bench --edit-budget <ms>` fails when
    the app's per-edit core path (move + undo state + delta snapshot) is slower at p95 (in CTest `sieda_scale_budget`).
  - **Copper pours:** fills (`Zones.cpp`) use running-count filters, union-find island labels and one thread per
    layer, and must stay identical to the sequential fill; the board mask (cells inside the outline, clear of holes)
    is kept between fills while its inputs are unchanged (`boardMask_`).
  - **MCP:** file reads are capped at 128 MB.
  - **Live endpoint:** at most 16 connections.
  - **`sieda-mcp --connect`:** only connects to 127.0.0.1 / localhost / ::1 and caps replies at 64 MB.
- Library parts live in `Core/src/StandardParts.cpp`; KiCad-derived pinouts are generated into
  `Core/src/StandardCatalog.inc` by `tools/fetch_catalog_parts.py` — don't edit that file by hand. More parts from
  whole KiCad libraries: `--discover` freezes `tools/catalog_extra_parts.tsv`, `--extra` writes
  `Core/src/StandardCatalogExtra.inc` (don't edit either by hand; `EXCLUDE` in the script lists symbols the core
  cannot draw). RF modules (`RF_Module`) and Nordic aQFN parts use KiCad's exact land pattern (aQFN lands name
  their balls); chips KiCad lacks (ESP32-C6, ESP32-H2) come from Espressif's own KiCad library (`ESPRESSIF` in
  `tools/fetch_mcu_pinouts.py`), then Digi-Key's and SparkFun's (`COMMUNITY`); a footprint no
  parametric package draws falls back to its exact KiCad pads (through-hole ones with drills, centred). Catalog search words: `wordMatches` (core MCP) = `PartQuery.wordMatches` (app). AI prompts get a digest of the catalog (`SiEDA/AI/CatalogDigest.swift`), never the whole list.

## Missing IC parts

Fourteen catalog parts, and 89 from the semiconductor IC reference (October 2026), still need their pinouts from the
vendor datasheet. The list, one file per part, and the
step-by-step procedure (fill the pin table, add a `catalog(...)` entry, update the tests) are in
[`docs/missing-parts/README.md`](docs/missing-parts/README.md). When asked to add or update missing parts, follow that
file and keep each part's Status line current.
