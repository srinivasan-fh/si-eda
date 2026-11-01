# SiEDA — notes for Claude Code

- README.md is the landing page (gallery from `docs/images/`, rendered by `sieda-mcp`; feature cards link to the guides); the
  detailed reference is `docs/FEATURES.md`, the guide index `docs/README.md`; every guide opens with the same nav bar.
- Core (C++): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests`
- macOS app: `./run.sh` (builds the core, generates the Xcode project, runs the app).
- Footprint Editor: core in `Core/src/CustomParts.cpp` (`landPatternFromFootprint`, `checkLandPattern`), app in
  `SiEDA/Models/FootprintDraft.swift` and `SiEDA/Views/Library/FootprintEditorView.swift`; guide in
  `docs/FOOTPRINT_EDITOR.md`. Keep the Swift `CustomPartSpec.Land` encoding identical to the core's land JSON (pad options = a trailing
  object: corner, npth, mask, paste, pasteGrid, chamfer, outline (polygon pads: `Pad::outline`, Gerber G36 regions,
  3D prisms), inner / solder (pad stack: inner layers / the side opposite the part); carried by `PadDef` / `Pad` into the Gerbers, drills, IPC-2581 / ODB++). Footprint
  silk / fab lines: `PackageSpec::graphics` = Swift `CustomPartSpec.Graphic` (same JSON), `FootprintDef::lines`,
  `PcbLayout::footprintLines`, snapshot `courtyards[].lines`. Two supply pins of one name may share joined copper (`supplyNamesake` in `checkLandPattern`, as stacked symbol pins: TPS63070 VIN 12 + 13); more land checks in `landChecksMore` (LAND_PASTE /
  SILK_PAD / PIN1 / NUMBERING / COURTYARD, LAND_MASK_SLIVER in the pair loop) must stay quiet on every catalog part;
  the user's own footprints: `UserFootprintLibrary` (FootprintDraft.swift: `.history/<name>/` keeps the last 20 saves, `library.sharedFolder` = a team folder with Footprints / 3D Models). Pad copper is measured only through
  `TrackGeometry.hpp` (`padPointDistance` / `padSegmentDistance` / `padTrackDistance` / `padPadDistance`: a round pad
  with w ≠ h is an oval = stadium round `padSpine`, circles keep their exact arithmetic; `padOnLayer` = the pad stack's
  `inner` / `solder` circle on that layer; a polygon pad by its outline, even-odd, so a keyhole outline keeps its hole open: the
  INMP441's ground ring round its NPTH sound port, test `polygon_pads_are_measured_by_their_outline`) in DRC, connectivity, pours and routers; ovals export as Gerber `O`,
  IPC-2581 `Oval`, ODB++ `oval`. NPTH pads have no annular ring (DRC / DFM); via-to-edge DRC counts the via radius.
- Symbol Editor: core in `Core/src/CustomParts.cpp` (`autoArrangeSymbol`, `checkSymbol`, four-sided layout) and
  `Core/src/Schematic.cpp` (stacked pins join nets), app in `SiEDA/Models/SymbolDraft.swift` and
  `SiEDA/Views/Library/SymbolEditorView.swift`; guide in `docs/SYMBOL_EDITOR.md`.
- Memory (RAM) design segments: core in `Core/src/Memory.cpp` (`memoryChecks`, `memorySegments`, routed DDR layout
  rules `ddrLayoutChecks` / `ddrLayoutLimits` with `Project::memoryLimits` overrides from the controller's guide,
  DQS-to-lane `MEM_DDR_DQS_SKEW`, CK / DQS P-to-N `MEM_DDR_PAIR_SKEW`: MEM_DDR_* warnings on DDR, info on SDR), app via
  `EDAEngine.memorySegments` / `DesignStore.setMemoryDesign`; guide in `docs/MEMORY_DESIGN.md`. The memory reference design
  (STM32H743 + SDRAM) must keep passing verification on 6 layers.
- Schematic capture: sheets / hierarchy / bus labels / annotation in `Core/src/Sheets.cpp`, repeated sheets in
  `Core/src/Instances.cpp` (`syncInstances` keeps channel copies in line; every Schematic edit calls it; a sheet-numbered channel ref held by another part takes the next free number; suffixed blocks share one logical numbering in `annotateBlocks`), graphical
  buses in `Core/src/Buses.cpp`, multi-unit parts in `Core/src/PartUnits.cpp` (kind PartUnit + hidden `packageOnly`
  package; the snapshot reports units as kind Custom with `unitOf`), find / replace and the net navigator in
  `Core/src/SchematicSearch.cpp`, variant simulation via `Project::simulationSchematic`; live board run over MCP `sim_live` (`McpServer::liveSim`) + `sim_firmware` (Studio Simulation → Live); Update PCB (forward ECO,
  `pcbSync` baseline) in `Core/src/Eco.cpp` (its new parts placed by hand afterwards: `Core/src/InteractivePlacement.cpp`,
`SiEDA/App/DesignStore+Placement.swift`, `SiEDA/Views/PCB/PlaceNewPartsOverlay.swift`), PCB pin / gate swap in `Core/src/PcbSwap.cpp`, schematic PDF in
  `Core/src/SchematicPdf.cpp` with TrueType subsetting and optional HarfBuzz shaping (`TrueTypeFont::shape`, CMake `SIEDA_HARFBUZZ`) in `Core/src/PdfFont.cpp`; app in `SiEDA/Views/Schematic/`;
  guide in `docs/SCHEMATIC.md`. Several boards per project (one schematic): `Project::boardNames` / `activeBoard`,
  `pcb` = the active board, the others in `parkedBoards` with their parts' `parkedPlacements`; `PcbPlacement::board`,
  `PcbLayout::onBoard` (Auto Place and the unplaced checks skip other boards' parts); saved only with ≥ 2 boards; app
  `BoardMenu` in `PCBEditorView.swift`, MCP `pcb_boards`. Symbol graphics: `SymbolSpec::graphics` (core) = `CustomPartSpec.SymbolGraphic`
  (Swift); keep their JSON identical. Nested repetition and per-channel values
  (`channelOverrides`) are in `Instances.cpp` (`syncNestedSheets`); harnesses in `Harnesses.cpp`; net classes /
  directives in `Directives.cpp` (carried to the board by `Project::applySchematicRules`); align / copy / paste, mirror (`Component::mirror`, `orient`), touching pins joined
  (`connectTouching`, ERC_TOUCHING_UNCONNECTED) and `cleanupWires` in `SchematicEdit.cpp`, straight-wire drag
  `Schematic::dragWireSegment` and `moveWireEnd` in `Schematic.cpp` (app: right-click menu
  `CanvasMenuItem`, double-click `QuickEditSheet`, X / Y / ⌘D / ⌥-drag / '/' keys, schematic `locked`, net-label `power` look,
  sheet notes / drawings `SheetGraphic` (`sheetGraphics`), view options `SchematicGrid` (snap step, display, selection
  filters); the app lands on the schematic (`DesignStore.startWorkspace`), and a launch reopens the project last opened / saved and the workspace last used (`restoreLastSession`, user defaults "app.lastProject" / "app.lastWorkspace", off by "app.reopenLastProject", run by `AppDelegate.activateStore` after crash recovery and a Finder open; not in tests), its tools a top bar (`ToolStrip(horizontal: true)`), devices the left sidebar; `SidebarView` lists components and nets only, hidden by default (`columns` = `.detailOnly`): the toolbar picker switches workspaces; Schematic / PCB / Simulation / Library / 3D / BOM show drawn icons `Workspace.icon` (Assets.xcassets Workspace*.imageset: resistor, board with a trace, oscilloscope, IC, isometric board with a part, parts table; Checks keeps its SF Symbol seal by choice)); back-annotation ECO in `Eco.cpp`; sheet templates and the PDF in `SchematicPdf.cpp`; unit
  (gate) checks `checkUnits` in `CustomParts.cpp`, app `SiEDA/Models/UnitDraft.swift` + `UnitEditorView.swift`. AI
  plans keep this structure (`DesignPlanCompiler.plan(from:)` / `apply` / `preservingStructure`); new project fields
  are written only when used so older files load and save identically.
  Canvas colour schemes / grid styles / custom named-colour theme: `SiEDA/Views/Schematic/SchematicPalette.swift`
  (+ `SchematicCustomTheme.swift`, `SchematicAppearanceViews.swift`); the canvas draws only with the palette (no
  fixed `Theme` schematic colours), and Midnight Navy (`siedaDark`) must stay today's colours. Symbols are drawn in
  their device colour (`ComponentKind.deviceColour`) by default; "Colour Symbols by Device" off = the scheme's symbol colour.
  App-wide theme (Midnight Navy / Matrix Green / Graphite / Light): `SiEDA/Views/Common/AppTheme.swift`; `Theme` colours read
  `AppTheme.current` (saved in user defaults, so the next launch and its splash start in it; a plain value, never an observed one: colours are read
  thousands of times a frame and tracking each read halved UI speed); window roots `.id(AppTheme.Shown.shared.theme)` (`@Observable`) rebuild at once on a
  new theme; the system controls follow it, `AppTheme.colorScheme` (light for Light, dark otherwise; no separate Appearance setting); the Settings window is painted in it too (`ThemedWindowBackground`); `Theme` resolves each theme's `Color`s once (`Theme.Resolved`), layer / net colours are `static let`s; 3D / X-ray meshes are kept per design revision across view rebuilds (`MeshCache`, Board3DView.swift); the custom schematic palette is decoded once per change);
  dark backdrops outside `Theme` (splash, 3D / X-ray / model views) go through `AppTheme.backdrop(r, g, b)` (Midnight Navy = the given navy); Midnight Navy must stay today's blues; board colours
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
  `tools/circuit_lm/` into `SiEDA/Resources/Models/sieda-circuit-v1.{gguf,json}` — regenerate, don't hand-edit; values the request fixes computed by `Core/src/CircuitValues.cpp` (`circuitPlanValues`)); Studio's Window → Super Intelligence through MCP `project_assistant` (`McpOptions::modelDirs`: beside `sieda-studio`, which the release archives ship with the circuit model, root `Models/`, `--models-dir`); guide in `docs/AI.md`.
- Settings → General (`GeneralSettings`, SettingsView.swift): start-up (reopen last project, splash, welcome tour, beginner mode), projects folder for Open / Save panels (`DesignStore.projectFolder`, "app.projectFolder"), autosave and its delay ("app.autosaveSeconds", 3 s; `recoveryDelay` nil = this), undo depth ("app.undoLimit", 100 by default), undo history kept beside the saved file (off by default, "app.persistUndo": `.<file>.undo`, newest 30 steps ≤ 32 MB, restored only onto the same bytes by FNV-1a), new-project defaults (`applyNewProjectDefaults`: "newProject.layers" / "trackWidth" / "clearance", "newSheet.size" for added sheets; unset = the core's defaults), mouse wheel pans / reverse zoom (`Viewport.wheelPansKey` / `reverseZoomKey` in `handleScroll`), clear recents, updates, team library / plug-ins / hooks, settings export / import and Reset All Preferences (the app's defaults domain; the Keychain is kept). Appearance keeps themes, units, colours, GPU canvas and the editors' view options.
- Configurable menu shortcuts: `ShortcutStore` / `ShortcutSettings` in `SiEDA/Views/Settings/SettingsView.swift` (user
  defaults "app.shortcuts"; `SiEDACommands` reads `shortcuts.shortcut(id)`); canvas tool keys `ShortcutStore.toolKeys`
  ("sch." / "pcb."), the canvases switch on `toolKey(_:canvas:)` (the tool's standard key; a two-key sequence "p w"
  returns `sequenceStarted` for its first key). Beginner mode `BeginnerMode` (`BeginnerGuide.swift`: guide bar, no library
  workspace). Editor view state that must survive a theme switch's rebuild lives in `ViewMemory` (EditorChrome.swift, `.remembers`).
  In-app Git: `GitClient` / `GitView` (Views/Common/GitView.swift: /usr/bin/git, no shell, `validRef` before a name
  reaches git, `--` before a clone URL); Studio's Git tab through MCP `project_git` (McpTools.cpp, `cloud::git`: no hooks; clone URLs https / ssh / git@ only). Releases: `.github/workflows/release.yml` (signed + notarised DMG from secrets,
  never hard-coded; byte-identical Linux CLI rebuild; SBOM `tools/make_sbom.py`), update check `UpdateChecker`
  (UpdateChecker.swift; daily check off by default). Datasheet pins carry `page` / `confidence`
  (`DatasheetSchema.Citations` notes). User-entered board coordinates go through `nm()` (Geometry.hpp, 1 nm grid) in
  the C API; computed geometry (router, pours) stays double, and `fabricationPackage` snaps tracks / vias to the grid first
  (the board as saved: the same package before saving and after reopening, test
  `fabrication_package_is_the_same_before_saving_and_after_reopening`). Detached editors: `WindowGroup(id: "editor", for: String)`
  in SiEDAApp.swift (Window → Open in New Window, same store). Read-only browser viewer: `mcp::viewerHtml`
  (McpRender.cpp, export "viewer"). Customer templates: `TitleBlockEditor` (user defaults "titleBlock.customers").
  Netlist import `map` / `fidelity` in `importNetlist` (McpTools.cpp). GPU canvas: `gpuRendered` (EditorChrome.swift,
  user defaults "canvas.gpu", off = today's drawing). Approvals `Project::approvals` + `designHash()` (FNV-1a of
  the saved JSON without approvals / review; saved only when present), MCP `verify_approvals`, app `ApprovalsSection`
  (TeamViews.swift). Library quality `partQuality` (CustomParts.cpp). Plug-ins run under `sandbox-exec`
  (`ProjectPlugins.sandboxProfile`, setting "plugins.sandbox", on by default). AI audit log `AIAudit`
  (DesignStore+MCP.swift, ai-audit.log). Batch conversion `sieda-cli --convert` (CTest `sieda_cli_convert`).
  Library: `library_bulk_edit` (re-registers the spec, `sieda_custom_part_replace` moves placed parts),
  `library_compare_footprint` (`measureFootprint`, CustomParts.cpp). Blocks `schematic_block` (block file
  "sieda.block/1": clip + FNV fingerprint + approver). Traceability `Project::requirementItems` (`verify_requirements`),
  bench measurements `Project::measurements` (`verify_measurements`; both saved only when present, neither changes
  `designHash`). Controlled release `output_release` (fabrication/, project copy, verification.md, evidence.json,
  change_notice.md, test_report.xml, MANIFEST.sha256 via `sha256Hex` in Documents.cpp). JUnit XML test report `testReportJUnit` (Verification.cpp: suite per stage, requirements, measurements; export "junit", sieda-cli `out/test_report.xml`, docs/ci templates; test `junit_test_report_lists_stages_requirements_and_measurements`). Pin / gate swaps skip pairs across I/O banks on
  different VCCO nets (`crossesBankVoltage`, PcbSwap.cpp). Eagle canvas preset (two-letter command sequences).
  Route exchange `Core/src/RouteExchange.cpp`: `exportHyperlynx` (export "hyperlynx": inches, Y up, ARC clockwise) and
  `importRoutes` (PADS ASCII / HyperLynx (METRIC = cm) / IPC-2581 detected from the text; frame fitted by named pins, else
  by track ends landing in pads of their net; MCP `pcb_import_routes`, C `sieda_pcb_import_routes`, `idf` fuzz target);
  the XML reader is shared through `Core/src/XmlRead.hpp`.
- Launch splash: `SiEDA/App/SplashScreen.swift` (`SplashModel` preload steps, `SplashController` holds main windows from
  `applicationWillFinishLaunching`).
- Interface languages: `SiEDA/App/AppLanguage.swift`, translations in `SiEDA/Resources/<code>.lproj/Localizable.strings`
  (20 languages, English keys); guide in `docs/LOCALIZATION.md`. A new UI string needs a key in every table —
  `python3 tools/check_localization.py --missing` lists the gaps.
- Supplier data: core in `Core/src/Suppliers.cpp` (Nexar / DigiKey v4 / Mouser v2 readers into the `sieda.supplier/1`
  schema, price breaks, BOM roll-up, catalog match), app in `SiEDA/Suppliers/` (URLSession clients, Keychain keys,
  offline cache), `SupplierSearchView`, `BomLivePricingView`; guide in `docs/SUPPLIERS.md`. Tests never use the
  network: core fixtures in `Core/tests/fixtures/suppliers/`, app tests stub `URLProtocol`. Never hard-code a key.
- Fabrication package comparison: `Core/src/FabCompare.cpp` (`readZipArchive`: own bounds-checked inflate, 2000 entries /
  256 MB; `compareFabPackages`: layers paired by X2 FileFunction else name minus the common prefix, rasterised ≤ 16 M
  cells, changed patches; drills hole by hole; other files by bytes minus CreationDate lines), MCP `output_compare_packages`
  (folder / .zip / "current" = `fabricationPackage`; `layer` → `renderPackageDiffPng`), macOS File → Compare Packages…
  (`compareFabricationPackages`, DesignStore+Team.swift), Studio File menu, `idf` fuzz target; test
  `fab_package_compare_reads_zips_and_finds_changed_copper`.
- Gerber viewer / reference layers: `parseGerber` + `gerberImageJson` in `Documents.cpp` (Gerber X2 / RS-274X,
  Excellon (LZ / TZ, `;FILE_FORMAT=a:b`, nothing kept between files) and ODB++ layer `features` files (kind "odb"); Gerber `%SR` step-repeat expanded (copies capped), `%FSLI` incremental; `idf` fuzz target), `Project::referenceLayers` (saved only when present; snapshot always lists their names),
  C `sieda_gerber_parse` / `sieda_pcb_*reference_layer*`, MCP `pcb_reference_layer`, app `GerberViewerView.swift` and
  the PCB canvas overlay (`DesignStore.referenceArtwork`).
- Netlist / schematic import: `parseNetlist` in `LibraryImport.cpp` (KiCad .net, Protel / Tango, CSV, and through it
  `parseKicadSchematic` (.kicad_sch: counter-clockwise turns, mirror after the turn, PWR_FLAG names nothing) and
  `parseLtspiceSchematic` (.asc, built-in symbols' pin offsets) and `parseEagleSchematic` (.sch, nets by name, gate pins by connects); KiCad sub-sheets through a `KicadSheetLoader` (MCP `sheets` / files beside `path`: hierarchical labels joined to sheet pins, local labels scoped `/Sheet/NAME`, refs from `instances` paths, depth 16); in the `library` fuzz target), MCP `schematic_import_netlist` (built-in passives, catalog parts, else library parts from the netlist's pins,
  then `schematic_build`; its work is `importNetlist` in `McpTools.cpp`), app File → Import Netlist… through `DesignStore.runTool`.
  KiCad boards: `parseKicadPcb` (`KicadBoard`: pads at KiCad's board positions, bottom footprints' stored-flipped pads
  turned back to a top view, lands centred on the courtyard; `library` fuzz target, fixture `two_parts.kicad_pcb`), MCP
  `pcb_import_kicad` (circuit from `schematic` / `schematicText` or the pads, then placement fitted to the pads per
  quarter turn, copper nets matched through the pads, `fidelity` report), app File → Import KiCad Board…; the DRC
  measures round pads as circles (diagonal neighbours).
- Altium libraries: `Core/src/AltiumLibrary.cpp` (MS-CFB `CompoundFile`, SchLib / PcbLib records), converted in
  `LibraryImport.cpp` (`importAltium`); fixtures written by `tools/make_altium_fixtures.py` (don't edit the binaries).
- 3D models of parts: core in `Core/src/Model3D.cpp` (VRML 2.0 / STL / OBJ readers and the STEP AP203 / AP214 reader
  `parseStep`: Part 21 parser, B-rep faces triangulated in their surface's parameters with seams unwrapped so shells
  stay closed, assembly placements, units, colours; B-spline / unknown surfaces use their boundary and are counted in
  `warnings`; `Model3DRegistry` (256 MB budget: least recently used meshes dropped, never one a project holds —
  `Project::holdMeshes`, taken by `toJson` / load / set part model — a library part refers to, or the newest), project `models3d`, `appendModel3D` in `buildAssemblyMesh`); a part refers to its
  mesh by `CustomPartSpec::model3d`; app `SiEDA/Views/Library/Model3DEditorView.swift`. The 3D view's tools (camera presets, layers, hide / isolate =
  `MeshOptions::hidden`, explode, section cut, measure, click to cross-probe) are in `SiEDA/Views/ThreeD/Board3DView.swift`. Tests:
  `step_import_round_trip_of_export` (every exported solid comes back closed, bounds and volume within 1 %),
  `model3d_step_reader_solids_units_colours_assembly`; the STEP reader is in the `library` fuzz target. Generated bodies in
  `Core/src/Mesh.cpp` take the shape people know (`lookOf` / `drawLook`: USB-C / micro / A shells with a mouth, tact switch,
  HC-49 / SMD crystal, RF module shield + printed antenna, pin header posts, diode band, LED lens, JST; QFN flanks; plated
  mounting holes); test `assembly_mesh_draws_parts_by_their_shape`.
- Interactive routing: `Core/src/InteractiveRouter.cpp` (router, shove, arc corners, commands), arc tracks in
  `Core/include/sieda/TrackGeometry.hpp` (`Track::arc`: measure tracks only through these functions, never `a`–`b`;
  straight tracks must stay bit-identical), board commands (teardrops as `Track::teardrop` fans, via stitching /
  shielding, gloss, loop removal on commit, hug drag (segments, vias, corners), Stop mode, gloss `moveVias`, `sieda_pcb_minimize_vias`,
  pour island markers `ZoneFill::removedIslands`, pours held until refilled (`PcbLayout::holdPours`, `refillPours`,
  `sieda_pcb_hold_pours`, snapshot fill `stale`), `matchTrackLengths`, split / merge tracks and footprint drag with its tracks (`PcbLayout::splitTrack` / `mergeTracks` /
  `moveFootprintWithTracks`), drawn pours (`CopperZone::polygon`) and board cutouts (`BoardSettings::cutouts`,
  `forEachEdge` = outline + cutout edges), pair rules / interface presets (`PairRule` by `pairStem`,
  `interfacePresets()`) and delay length rules (`LengthRule::delayPs`) — opt-in router options, so default
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
  (`PairPlan::layerWidth`); a receptacle's doubled pair pads (USB-C D+ / D− in both rows) are fanned out to one via per
  member, either side of the row (`BgaFanout::hasKnee` / `sharedVia`), and the pair starts there; an end with no room
  under the fast nets' spacing retries with escape clearance (`escape`); REL_CROSSTALK skips a pair's own partner;
  via minimisation tries every net (adaptive batches + one shared grid, serial, `setViaMinimisationGrids` for tests);
  opt-in shape-based completion `AutorouteOptions::shapeCompletion` (`routequality::shapeComplete`: what the grid
  leaves unrouted, routed by the interactive router on the exact shapes); corridor routes get `routequality::fixAcuteJoins` (no DRC_ACUTE_ANGLE; classic copper untouched); opt-in
  `AutorouteOptions::lengthAwareSearch` (with `lengthAware`: longest first, then a `routeAll` stage after via
  minimisation re-routes target nets shorter with cheap vias or longer through a waypoint before the meanders);
  `classic_router_copper_fingerprint` pins the classic router's and greedy placer's result — update its constant
  only on purpose. `sieda_route_bench --options '{json}' --save file` measures strategy options and lets `cmp` compare runs.
  A wide piece a neck zone's edge leaves (the step into the zone was judged at the neck's width) is checked at its own
  width and class spacing at commit (`wideClear`) and necked when it does not clear (test `router_keeps_class_clearance_where_a_wide_track_necks`, fixture `routing/class_neck.siedaproj`); pad neck widths and neck zones keep the larger net class's clearance (`padNeckWidths(pads, netClearance)`, test `neck_widths_keep_net_class_clearance`).
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
  copper. `pcb_place_footprint` refuses a pose nearer than `standardGap` unless `force` (crystals count as passives); `pcb_drc`
  `unrouted[].cutOff` = the pads cut off with that end (`PcbLayout::padGroups`); hand repairs `pcb_add_via` / `pcb_add_track`
  (refused near other nets: `checkClearOfOtherNets`); `pcb_autoroute` presets apply to the call unless `keep`; undo history
  saved beside the project (`.<file>.undo`, `McpServer::saveUndoHistory` / `loadUndoHistory`, restored only onto the same
  saved file (its bytes)); antenna keep-outs widened by `antennaKeepout` (15 mm, at least the antenna's depth, either side), and a top-layer track keep-out "under REF" inside the module's pad ring, cut back (`carveUnder`) from its signal pads and, at route time, from pads whose net has no pour on that layer; the exact-shape checks (`trackClearExact` / `viaClearExact`: coupled pairs, shape completion) honour keep-outs as the grid does; open-ended connectors' mouths flush with the edge; library_search reads
  "4 pin" as pins:4; a schematic rotation carries the part's own labels round with its pins (grounds stay upright) and
  `schematic_move_component` warns of its pins left on a wire they do not join (ERC_TOUCHING_UNCONNECTED stays info:
  reference designs draw L-routes over pins); snapshot tracks / vias carry `netName`; `pcb_eco_preview`
  counts single-pin nets (`singlePinNets`); `pcb_autoplace` re-places the edge parts once when Auto Place grew the board;
  `stitchingVias` skips vias by a part (the router's pour fan-out); route results give `viasOnBoard`; MCP whole-board
  routes route unpoured power / ground nets with no width of their own 0.4 mm wide (`powerWidths`; tiers: every such net (necked down at a pad narrower than 0.4 mm + clearance), then those without
  such a pad, then none, until nothing is left open; only the one with the fewest open goes through `finishRouting`, once) and without a preset take the quality preset's options (gloss, fewer vias, coupled pairs, lengths; ≤ 300 parts); a later `schematic_build` anchors new parts by the pin
  of a part drawn before (`layoutBuilt` `anchor`), a grounded two-pin part above an upward pin stands upright with labels
  (`grounded`); McpRender keeps side pin names inside their half of the body, a lead-less pin's no-connect X outside it,
  and draws every pour under every track (bottom first, each in a light tint of its layer colour; no blend) with light spokes from each pad joined to a pour of its net; `pcb_summary` footprints carry `kicadFootprint` (the catalog's library name); a label splitting a wire is drawn as text along it, catalog D / SW / Y parts
  as a diode / switch / crystal glyph. `fitBoardToComponents` moves keep-outs, pours, graphics, dimensions and cutouts
  with the parts and keeps a part nearer an edge than the margin there; PLACE_KEEPOUT is an error (and DRC_PLACE_KEEPOUT)
  for a pour / barrier keep-out; a bulk cap goes by its rail's series entry (diode / fuse / ferrite), else a connector,
  or by a load on a regulated rail; `placeDecoupling` pairs caps with the pin they are drawn by (`drawnApart`); grounds
  always point down (out beyond the labels below); a crystal's load caps hang below its pins (laid out right after the crystal, which stands 300 out from its IC, on
  a 5 grid in line with its pins, wired down from them), a 4-pad crystal is mirrored to face its IC; a rail's first bulk cap links to its regulator's pin; `layoutBuilt`'s room counts a
  part's designator and value; a later build's part on an existing net is laid out by that net's regulator / largest
  part (layout-only link); the repair passes after an MCP route (`squareEscapes`, `viasOffPads`, `trimStubs`) try candidates on held pours
  (`PourTrials`, which sets `PcbLayout::pourTrials` so the DRC skips DRC_POUR_CLEARANCE on those stale pours) and judge one that passes again on refilled pours; after an MCP route `collapseJogs` folds sub-0.1 mm jogs (acute slivers), `fixAcuteJoins`
  runs, and `squareEscapes` straightens or nudges IC pad exits closer than the fab minimum (DRC_FINE_PITCH_PADS);
  VAL_DECOUPLING_PER_PIN lets any capacitor serve a regulator's input pin; `neckTombstones` (after MCP routes and pours) necks wide tracks at
  small two-pad parts unless that raises DRC_TRACK_CURRENT, else widens the light pad's lead-in (pour spokes) when the DRC
  stays as good, and names only the parts balanced; `finishRouting` last stitches a cut-off pour-net pad to the pour by a
  via; `pairsNotCoupled` names X_DP / X_DM nets (paired only by a directive: `differentialPairs` must not pair them, the
  POS reference design would no longer route); `schematic_build` `pins: false` keys pins `name(number)`; renders mark
  pins named NC with an X and keep ref / value off a centred top / bottom lead; after an MCP route `viasOffPads` moves a
  via whose ring overlaps a pad of its net (centre outside it) 0.1 mm clear, or drops a pour net's via with its stub;
  `pcb_fit_board` `compact` re-places everything (`autoplaceTool`) on a board shrunk in 1 mm steps while it still
  fits and keeps as many `decouplingPlaced` pairings (before routing), and `pcb_autoplace` says `boardRoomy` under 35 % use; the review's VAL_LED_OPEN flags an LED pin
  on no net. `PcbLayout::pruneStaleRouting` keeps a via reaching no pad when it stands in a pour (stitching), and a track reaching no pad whose ends both lie in a pour of its layer (it joins two islands: a board clean before saving stays clean on reopen);
  `placeDecoupling` also lays a two-pin R / D / LED / L / FB on a signal net with one other pad beside it (by an IC pin first, then the part it leads to: an LED after its resistor), puts crystal load caps on the IC's side (`nearest` `toward`) and a bulk cap by its rail's entry where that part is now (after small parts stepped aside); the under-module keep-out leaves 1 mm more by its pads on a board with a differential pair and the interactive router's hit checks honour keep-outs; after an MCP route `trimStubs` removes a dangling track or brings its end back onto its net's copper (`stubsTrimmed`, kept only when the DRC finds no new problem), DRC results name the tracks ending at a violation (`trackIds`), MCP routes search a budget-exhausted coupled pair once more 8 × further (`AutorouteOptions::widerPairSearch`, not saved; core routes unchanged), a re-route's result points at `project_undo`, a `schematic_build` wire chain naming GND grounds its pins, and the edge pass puts a part on the 0.25 grid inside the edge (a USB-C mouth flush); `placeAtEdges` (pcb_autoplace all: antenna parts top + keep-out, open-ended connectors bottom, by `openEnd`);
  undo is cleared by project_open / new / load_example; headers without an open end go to the left / right edges in turn, long side along it; crystals are placed before supply caps (small passives step aside, `stepAside`); edge parts largest first; `pcb_autoroute` re-lays stitching vias (`stitchingRestored`) and lists `pairsNotCoupled`; assembly spacing standard `standardGap` (passive 0.25 / passive-IC 0.5 / IC-IC and THT-SMD 1.0 / BGA 2.0 mm) kept by the MCP placement helpers and flagged as DRC_PART_SPACING; `placeGrouped` (pcb_autoplace grouped: rows of like passives); power-only connectors to the left edge; the main IC is centred before Auto Place; opt-in `AutorouteOptions::viasOffPads` (pour fan-out vias clear SMD pads; on for MCP `pcb_autoroute`); `schematic_build` expands a shared pin name to every unstacked pin; a package may be a bare size (`setPackage`: "0603" = R_0603); `noteBoardGrown` on Update PCB too; crystals within 6 mm of their pins first (stepAside); a supply cap with no spot by its pin (or still far) moves the crystal (and its load caps, small parts stepping aside) out of the way, and turns half round when that brings its net's pad nearer the pin; stitching (`stitchVias`) also joins a pour left with no copper (nothing of its net on that layer), and an MCP route stitches such a pour (`stitchingRestored`); a regulator pin takes a small cap after the loads; a `schematic_build` tag with no clear standing spot lies along its wire, tags are re-placed once every label is down, a part stepped off its pin keeps clear of the hub's pin labels (`hitAt`, label room by net-name length `pinNet`), check results' `clean` = no errors or warnings (`passes` = no errors), library_search lists a small part's `pinNames`, MCP routes with coupled pairs pair X_DP / X_DM nets by a directive (`pairedByName`), `decouplingPlaced` gives each cap's distance to its pin and a crystal crowding a far cap is placed again after it (compact keeps the summed distances), VAL_STRAPPING_PIN (info) flags ESP32-C3 GPIO2 / GPIO8 left open, a single-row HEADER symbol has every pin down one side (`pinOutward` then faces them away from the body), standing two-pin glyphs carry ref / value beside the body, the DRC holds a track leaving its own pad nearer a neighbour pad than the land pattern's own gap to the board clearance (not DRC_FINE_PITCH_PADS) and `squareEscapes` straightens such exits (stub from either pad end, direct or square, then nudges the segment; both to min(clearance, land gap)); it keeps a change only when the DRC finds no new problem (not one traded for another), trying the same shapes necked by 0.02 / 0.05 mm (never under `minTrackWidth`), `pcb_remove_copper` `net` rips up one net, `pcb_fit_board compact` says why it kept the size (`unchanged`) or stopped shrinking (`stopped`), clears routing done before it (`routingCleared`), `schematic_build` sizes a drawn-before part's label room by its nets' names and lists `labelOverlaps` it could not clear (a label-only small part steps back; a wire tag slides along its straight wire off a pin's label, 6 units clear), clears the no-connect mark of a pin it joins to a net (`noConnectCleared`), anchors a later part by its own signal link before a supply's layout-only link (and further out when its labels need room); library_search with a built-in kind word (led 0805) lists no partial matches but `builtin`, `pcb_fit_board compact` keeps every pairing Auto Place made, none more than 1 mm further, shrinking one side at a time past up to four refused sizes, a whole-board `pcb_autoroute` on a board with no pour pours the ground net first, on both layers of a 2-layer board (a module's ground pads join it without a via each; routed again with the bottom pour only when the top one leaves a differential pair uncoupled), else on the bottom (`groundPour`; `pour: false` skips) and lists `pairsCoupled`, `pcb_route_report` measures the copper when the route's report is gone (reopened), later-build parts stay on the sheet (no negative x / y; a first build's rows start below / right of each part's label room), a row runs as wide as 1.8 × its tallest group (small groups beside the IC's), a catalog crystal's value defaults to its frequency (Crystal_16MHz → 16MHz), grounds go last (a side pin's ground points outward when the pin below is used), and pins already on their named net get no second label; caps standing upright above an IC's upward supply pin share one rail wire (the row slid under the pin when clear) with the pin wired straight up to a junction on it, the supply's entry (a series D / F / FB / L with its feeding connector, wired in, or the rail's regulator with the two-pin parts by it) at the nearest clear spot left of the rail, and a catalog switch's value defaults to SW_Push; a pull-up (two-pin part to a +supply) on an IC's side pin stands up beside it on its own wire (laid out first, the higher pin's nearer: `standing`, 5 grid, ref / value drawn beside the body), its wire tagged along it (never moved off it; an untagged wire would split the net), every requested net checked as one net at the end (a pin left off it gets its label: `netsJoined`), `schematic_connect` clears a wired pin's no-connect mark (`noConnectCleared`), and other parts by those pins try in-line spots first; a ground / label on another net's wire steps out along its stub, sideways too (a wire's tag is left alone), a standing pull-up's tag slides along its wire to its clearest spot; the review's VAL_USB_ESD (info) flags USB data lines with no ESD / TVS part; a part-number-like library_search (a word with a digit or "-") lists only parts with every word in their name when some have (`alsoInDescriptions` counts the rest) and reads "header 2" as pins:2; board renders tint pours dark. A coupled pair whose nets reach more than two parts (an ESD part) is routed between the two parts carrying both members (farthest apart), the other pads joined to its copper afterwards (`pairSeeds` in `routeAll`; a receptacle's doubled pads fan out for any pad count); `tuneLengths` never meanders a segment another track or via branches from, `pcb_autoroute` says why it meandered (`lengthTunedWhy`); `pcb_update_from_schematic` on a routed board says how to place only the new parts (`next`); `pcb_fit_board compact` keeps only C / Y pairings and then pulls parts toward the main IC while the board shrinks; designators on silk / IPC-2581 / ODB++ / DFA / renders stand at `designatorBoxes` (Export.hpp: above, below, right, left of the courtyard, clear of other designators and parts); `schematic_build` puts caps on one rail per net and part (a misaligned cap at the row's end). `schematic_build` wires only straight links clear of every body (`wireCrossesBody`, exact: a thin cap counts), designators take the spot with the fewest clashes (other designators, pads, courtyards; slid in from the edge), library_search drops parts matching a word only by their package when others say it, and a 1N4xxx / 1N58xx search points to the built-in diode kind. A designator with no clear full-size spot is set 0.8 mm high (Gerber / IPC-2581 / ODB++ / renders draw the box's height); a tag counts crossing another wire as a clash (drawn, or another tag's link still to draw) and a standing pull-up's tag never slides back past the IC's pin; a pull-up stands past a part in line on a pin up to 110 above it (240 out), a second two-pin part on a pulled-up pin goes in line beyond the pull-up, wired from its lower pin (`rehub`; not when a pull-up on a pin just below would run under it; with no room there it hangs upright below the pull-up's wire, joined by a junction on it: a `taps` entry, whose junction follows the wire's drawn L), a part with more than four pins tries in-line spots up to 420 out before aside (a header facing the IC by straight wires), and an in-line spot is judged as wired (no label room) but clear of the group's grounds and labels; renders name a generic header pin (Pin_3) by its net; board renders dot each part's pin-1 courtyard corner (3+ pads); a second two-pin part on a pulled-up pin with a pull-up just below (an EN cap) stands above the pull-up's wire, joined by a junction on it (`taps`); the lowest pull-up on a side steps further out until clear (a crystal's load caps); a two-pin part with no clear spot, or a chain's first part (an LED's resistor) with none in line by an IC, starts a group of its own; board renders draw each track over a dark casing on darker pour tints; a designator up against another part's courtyard (0.3 mm) costs more than a clear spot; a part placed by a large IC's group member keeps clear of its body and its labelled pins' strips (not the box round them). Compact's pull-in keeps parts within 2.5 mm of an edge on it; renders clip keep-outs to the board and `render_pcb` `pours: false` leaves the pours out. Catalog rows may name `PCM_Espressif` footprints, and
  `ALL_PADS` in `tools/fetch_catalog_parts.py` keeps every pad of a footprint. Tool failures are `isError` results, never crashes; file
  access stays inside `allowedRoot` (`resolvePath` refuses any symbolic link on the way and dangling ones; output folders are checked by
  `checkLinksInside`; the undo file is never written through a link); read-only mode refuses tools with `mutates`. Activity log: `McpServer::callTool` writes every call through `appendActivity` (Mcp.cpp) to
  `McpOptions::activityLog/<UTC date>.log` (tab-separated: time, actor = `setActor` ("user": Studio's /rpc page) else
  "ai:<clientInfo name version>", tool, ok / changed / error, ms, project, args ≤ 2000, result ≤ 400; default folder
  `defaultActivityLogDir()` for `sieda-mcp` / `sieda-studio`, `--activity-log` / `--no-activity-log`,
  `SIEDA_ACTIVITY_LOG=off`; C API servers only when set); the macOS app's own edits go to the same folder through
  Swift `ActivityLog` (DesignStore+MCP.swift, `finishEdit`, setting "activityLog.enabled"); test
  `mcp_activity_log_records_who_did_what`. Lessons: `Core/src/Lessons.cpp` (MCP `project_lessons`, built-in lessons as
  JSON, "sieda.lesson/1" files from `McpOptions::lessonDirs` (`--lessons-dir`, SIEDA_LESSONS_DIRS) or a team folder's
  Lessons/; checks run read-only tools only, through `callToolUnlogged`; state `McpServer::lesson` / `lessonStep`); every
  built-in lesson must finish through its own `solution`s (test `mcp_lessons_run_built_in_and_team_lessons`); app
  `LessonsView` (Onboarding.swift), Studio `lessons()`; menu-use shortcut tips `ShortcutStore.noteUse` (SettingsView.swift),
  Studio `noteMenuUse` / quick keys; guide `docs/LESSONS.md`. Live app endpoint (off by default,
  127.0.0.1 only, bearer token in the Keychain, Origin / Host check): `SiEDA/App/MCPEndpoint.swift` (listener, HTTP
  parser, gate), `SiEDA/App/DesignStore+MCP.swift` (`MCPLiveServer`, `MCPStoreBridge`: mutating tools run in
  `DesignStore.performExternalEdit` = one undo step), `SiEDA/Views/Settings/MCPSettingsView.swift`; stdio clients
  reach it through `sieda-mcp --connect` (`Core/mcp/http_bridge.cpp`, CTest `sieda_mcp_connect`).
- Fabrication drawing PDF `exportBoardDrawingPdf` (declared in `Documents.hpp`, written in `SchematicPdf.cpp` with its
  `Canvas`: dimensions `BoardSettings::dimensions`, drill map + table, stack-up legend, notes) and the revision table
  `Project::revisions` (both saved only when present; title blocks of both PDFs); net-length CSV `exportNetLengthReport`.
- Documents in `Core/src/Documents.cpp`: DXF export / outline import (`exportDxf`, `importDxfOutline`,
  `applyDxfOutline`, MCP `pcb_import_dxf`, fuzzed in the `idf` target), test-point report, interactive HTML BOM and the
  bring-up checklist; all four are in the fabrication package. Placement files (`exportPickAndPlaceCsv` / `exportCplCsv`):
  lower-left origin (Y up), rotation counter-clockwise (`placementRotation`, as IPC-2581 / IDF), no DNP parts and `sieda_export` ("dxf", "testpoints", "html_bom",
  "bringup"). Product checklist with every item's status: `docs/EDA_CHECKLIST.md`.
- IPC-2581C / ODB++ v7: `Core/src/FabExchange.cpp` (one per-layer feature list feeds both; ODB++ is a .tgz written by
  `tgz` with stored deflate blocks; both are in the fabrication package, IPC-2581 also as export "ipc2581"). Keep the
  XML valid against the IPC-2581C schema (KiCad's `qa/data/pcbnew/ipc2581/IPC-2581C.xsd`).
- Manufacturer DFM / DFA packs: `Core/src/Dfm.cpp` (data table of fab / assembly limits; `dfmOverrides` = the fab's own values over a pack via `boardDfmPack`; `applyDfmPack` only tightens
  DRC minimums, `dfmChecks` adds DFM_* / DFA_* (incl. DFM_ASPECT_RATIO, DFM_COPPER_BALANCE via `copperCoverage`) to `runDRC`
  when `BoardSettings::dfmPack` is set; `dfmReportJson` / `sieda_dfm_report_json` is the per-rule sign-off; saved only when set;
  C API `sieda_dfm_packs_json` / `sieda_pcb_set_dfm_pack`, MCP `pcb_dfm_pack`), app Board Setup → Manufacturer Rules.
- Thermal analysis: `Core/src/Thermal.cpp` (`thermalAnalysis`: board as one sheet of laminate + per-layer copper coverage on a
  grid, convection from both faces, part power from the DC operating point or `ThermalOptions::power`, PCG solve (warm-started); each part's
  body (footprint `BodyDef` top + sides, `thetaBody`) and optional heatsink (`ThermalOptions::heatsink`, θ junction to ambient)
  take `toAirW` straight to the air, split by fixed-point iteration; junction = board + (power − toAirW) × θJB,
  `packageThetaJA` in Reliability.cpp), C `sieda_thermal_json`, MCP `pcb_thermal` (+ heat-map PNG
  `renderThermalPng`), app `ThermalView.swift` (Design → Thermal Analysis…), Studio Design menu; test
  `thermal_map_balances_power_and_spreads_through_copper` (energy balance, linearity, copper / airflow / heatsink cool it).
- Production example `examples/smps-230vac-5-48v/` (230 VAC → 5–48 V / 1–5 A: PFC, flyback, sync buck; 2 layers, routed,
  verified, fabrication outputs): its `.siedaproj` is embedded into the core by `tools/embed_examples.py` (checked-in `Core/src/ExampleProjects.cpp`,
  `smpsExampleJson`; CTest `sieda_examples_embedded` checks it is current — rerun the script after changing the project) as
  MCP example `smps_230vac` (`loadExample`), macOS File → New from Example → Production Boards (`loadBuiltInExample`);
  test `smps_example_is_routed_verified_and_its_fixes_hold` (DRC clean, verification without warnings). Fixes it brought:
  DIP / HEADER take a pitch above 2.54 (up to 15.24 mm, bigger pads / drills), placement refuses a pose nearer another net's
  pad than a net class's clearance (PLACE_NET_CLEARANCE in `collectIssues`; core `autoPlace` widens its keep-outs by the
  class extra), the router's `wideClear` counts only the other net's class extra beyond its own (`beyond`) and neck zones
  hold per layer (`neckLayers_`, classic grid too), VAL_LED_NO_RESISTOR takes a two-part net as the LED's own, a
  connector-fed board with no source and no ground symbol skips the DC stage, PGND / SGND pours are reference planes.
  Single-sided boards: package type WIRELINK (two holes bodySize apart; `Pad::wireLink`, joined in `copperClusters`)
  and MCP `pcb_add_jumpers` (`addJumpers`, McpTools.cpp: shortest straight link per split net, real JW part labelled with
  the net, stub tracks), run by `pcb_autoroute` on a 1-layer board (`jumpers`); a JSON package type TESTPOINT / NETTIE /
  WIRELINK is kept by `normalizePackage`. `pcb_add_jumpers` `fromBottom` (`bottomToLinks`): a 2-layer route's bottom copper
  becomes links between its vias (Prim per piece; a new hole joins the nearest hole with room, a lead hole gets a spare
  hole + stub), vias removed, 1 layer; links stand on a "Wire links" sheet (`addWireLink`, 4 to a row);
  `checkClearOfOtherNets` keeps net-class clearances; wire links are left out of PLACE_OVERLAP and `partClasses`
  (DRC_PART_SPACING). Single-sided production example `examples/smps-230vac-5-48v-1layer/` (MCP `smps_230vac_1layer`,
  `smps1LayerExampleJson`, test `smps_single_sided_example_is_routed_with_wire_links`). One click on a 1-layer board:
  `singleSidedAutoroute` (McpTools.cpp; `pcb_autoroute` whole-board, `links` default true): THT to the component side
  (turn by shortest pins), route as 2 layers with "link …" keep-outs, rip-up rounds round open pads, JSON round trip,
  `bottomToLinks`, `addJumpers`, ground pour; test `mcp_single_sided_autoroute_is_one_click`; guide docs/ROUTING.md. THT power parts are drawn as their hardware (`drawPowerLook`, `lookKey` in
  Mesh.cpp: transformer, electrolytic, toroid, TO-220 / TO-247, bridge, terminal, fuse holder, film, disc, axial, wire
  link). MCP: a pin name several pins share ("Q3.S") is wired together by `schematic_connect` / `schematic_net_label`
  (`namesakes`), `library_add_part` finds the project's own parts by name, `pcb_place_footprint` without x / y turns or
  flips a placed part in place.
- PI: lumped PDN / IR drop in `Core/src/PowerIntegrity.cpp`; cavity model, plane mesh on the real pour shape
  (`planeMeshImpedance`, RLGC grid from the IR map's cells, banded LU), droop, decap plan in `Core/src/PdnPlanning.cpp`.
- Field solver: `Core/src/FieldSolver.cpp` (2D Laplace, finite volumes on a graded grid, Jacobi-CG; C and C0 give Z0,
  εeff, L / C, odd / even, kb / kf; `lineLoss`: skin-effect R from the air solution's surface charge, Hammerstad
  roughness, G from Df with the filling factor; `trackGeometry` reads the stack-up and coats outer layers with 20 µm of solder mask (`FieldGeometry::mask`, coated microstrip); tests hold it within 1.5 % of exact stripline,
  Cohn coupled stripline and Hammerstad–Jensen), C API `sieda_field_solve` / goal seek `sieda_field_solve_width` (`fieldSolvedWidth`: Illinois regula falsi on ln Z, ~6 solves), MCP `si_field_solver` (`targetOhms`), app Board Setup →
  Stack-up → Check with Field Solver; opt-in `SiSettings::fieldSolverLines` / `LossOptions::fieldSolver` feeds the
  channel lines (`lineModel`, cached per geometry). The closed-form widths are unchanged.
- P2 features: rigid-flex folding `FlexRegion::bendAngle` (saved only when set) + `MeshOptions::fold` / `foldFlexRegions`
  (Mesh.cpp: triangles split at the bend's cuts, far side turned; render_3d and the drawing fold, the app's 3D view by
  "Fold Flex"); fabrication drawing sheet 2 (`exportBoardDrawingPdf(project, views3d)`: isometric / exploded / section
  A-A rasters by `rasterizeMesh` (McpRender.cpp) as ASCII85 + Flate images: the PDF stays ASCII, the C API hands it
  over as a string; verification's package leaves them out); price watch (`supplierBomRollup` with `baseline` →
  `alerts` + new `baseline`; app `PriceWatch` in SupplierModels.swift); fab quote / order (`fabQuote`, `fabServices` in
  Fabrication.cpp, C `sieda_fab_quote`, MCP `output_fab_quote`, app `FabOrderView.swift`: never built-in prices, only
  the user's price table; order pages verified, Indian fabs have no published pack); MCAD scripts `exportMcadScript`
  (Mechanical.cpp, export "freecad" / "fusion", package `mechanical/`: run again they write IDF placement for
  `importIdfPlacement`); rule scripting `Core/src/CustomRules.cpp` (`BoardSettings::customRules`, saved only when
  present, RULE_<NAME> / RULE_SYNTAX in `runDRC`, tokens capped at 500, C `sieda_pcb_custom_rules`, MCP
  `pcb_custom_rules`, app Board Setup → Custom Rules); plug-ins `ProjectPlugins` (ProjectHooks.swift: Application
  Support/SiEDA/Plugins only, run on a project copy, changes = one undo step via `performExternalEdit`); Studio's run by its host
  (`runPlugin` in Core/studio/main.cpp, routes `/plugins`, `/plugins/run`: XDG data / %APPDATA% SiEDA/Plugins only, bwrap sandbox,
  changes through MCP `project_load_json`).
- Board objects (guide in `docs/MCAD.md`): copper thieving `copperThieving` / `copperFills` in `Zones.cpp` (dots as
  extra fills with zone and net -1; output code reads `copperFills`, connectivity keeps `zoneFills`); artwork
  `BoardSettings::graphics` (`BoardGraphic`, raster logos, user layers `exportUserLayerGerber`, DRC_GRAPHIC_COPPER),
  MCP `pcb_graphics` / `pcb_copper_thieving`; `TESTPOINT` / `NETTIE` packages (net-tie DRC exemption `tied` in
  `runDRC`); flex regions (`FlexRegion`, FLEX_* in `mechanicalChecks`); barrier keep-outs (`RouteKeepout::barrier`);
  panel DRC `panelChecks`; grounded CPW `FieldGeometry::cpw`.
- Platform: file format `docs/FILE_FORMAT.md` (`kProjectFormatVersion`; `Project::loadNotes` = newer format / fields
  this version would drop, shown on open), project templates (`DesignStore.saveTemplate` / `newFromTemplate`, Application
  Support/SiEDA/Templates), settings export / import `SettingsTransfer`, colour-blind layer palette
  (`Theme.layerPaletteKey`), board units `Theme.LengthUnit` (mm / cm / mil / in / ft / pt from exact ratios, shown to ≥ 1 µm; `parse` reads a typed unit; one setting for the PCB read-outs, footprint editor and inspector: views showing lengths read `@AppStorage(Theme.displayUnitKey)` so a new unit redraws them), Altium canvas preset; migration guide `docs/MIGRATING.md`; `sieda-cli --upgrade` rewrites
  files in the current format (left alone when load notes say settings would be lost); check-out lock
  `<file>.lock` and event hooks (Application Support/SiEDA/Hooks: after-open / save / verify / export) in
  `SiEDA/App/ProjectHooks.swift` — hooks only from that folder, never from a project.
- Integration: pin maps `exportPinMap` / `parsePinAssignments` (`Documents.cpp`, MCP `schematic_pin_map`: header /
  XDC / QSF / CSV out, CubeMX .ioc / XDC / QSF / CSV in; `plan` = `pinPlanJson` for the app's `PinPlannerView`); Python client `python/sieda.py` over `sieda-mcp` (CTest
  `sieda_python_api`); C API semver `SIEDA_API_VERSION_*` / `sieda_api_version()` (bump MINOR when adding functions);
  CI templates `docs/ci/` (GitHub, GitLab); BOM with chosen columns / per board `exportBomColumns` (MCP `output_bom`).
- Library extras: CSV / TSV / CubeMX pin tables `parsePinTable` (`CustomParts.cpp`, MCP `library_create_part`
  `pinTable`, library fuzz target), LAND_ZERO_ORIENTATION (pad 1 on the right; quiet on every catalog part), library
  update notices `sieda_library_updates` (MCP `library_updates`), favourites / tags `LibraryMarks` (Standards.swift,
  search `fav:` / `tag:`).
- Production panels: `Core/src/Panel.cpp` (`BoardSettings::panel`, saved only when nx × ny > 1; `fitPanel` = most boards
  within a fab panel size, default the DFM pack's; panel Gerbers are the
  board's own Gerbers shifted and stepped with %SR (opened outside multi-line %…% blocks such as %AMRR; fiducial D-codes
  above every board code), drills repeated per board (NPTH from the board's -NPTH.drl: footprint `npth` pads too, which
  also count in the fab notes); C API `sieda_pcb_panel` /
  `_set_panel`, MCP `pcb_panel`), app Board Setup → Production Panel (`PanelPreview`).
- Mechanical CAD and team work: STEP AP214 / IDF 3.0 export and the IDF placement import in `Core/src/Mechanical.cpp`
  (faceted B-rep solids must stay closed: test `step_export_closed_named_solids`), 3D clearance DRC
  (`mechanicalChecks`: MECH_BODY_COLLISION always, MECH_HEIGHT / MECH_HEIGHT_ZONE only with `BoardSettings` limits; a part drawn by a 3D model collides by the model's
  bounds and z range (`Body::z0`),
  MECH_ENCLOSURE against `BoardSettings::enclosure` (a Model3DRegistry id in the MCAD frame, in the "mechanical" JSON,
  its mesh saved in `models3d`), MECH_CONNECTOR_ACCESS info via `openEnd`); glTF / VRML exports (`exportGltf`,
  `exportVrml`, `sieda_export_file`), MCP `render_3d` (`render3dPng`, a z-buffer rasteriser in `McpRender.cpp`); a placed
  part's own 3D model `PcbLayout::partModels` (`model3dRefToJson` / `FromJson`, `sieda_pcb_set_part_model`);
  IDX / EDMD v4.5 baseline (with keep-outs / height zones), change file, import of placement, outline, thickness and
  keep-outs (`exportIdx`, `exportIdxChanges`, `importIdx`, `importIdxPlacement`), accept / reject `idxResponse`,
  namespace-agnostic XML reader, fuzzed in the `idf` target);
  version diff, three-way merge (`mergeProjects`, `sieda-mcp --merge`), design review comments
  (`Project::reviewComments`, saved only when present), Git drivers (`--diff` / `--git-diff`) and the variant matrix
  in `Core/src/ProjectDiff.cpp`; app
  `SiEDA/App/DesignStore+Team.swift`, `SiEDA/Views/Common/TeamViews.swift`; live co-editing of a shared file
  (auto-save, 2 s watch, three-way merge as one undo step, presence folder) in `SiEDA/App/LiveCollaboration.swift`, Studio's through MCP `project_live` (`McpServer::coEdit`; same presence files);
  guides `docs/MCAD.md`, `docs/TEAM.md`.
  Scale guard: CTest `sieda_scale_budget` (`sieda_route_bench --budget`); `sieda_route_bench --clusters 32 --layers 8
  --seed 3 --fpga` is the 923-part board.
- Speed and safety rules:
  - **Per-edit path:** undo history and change detection use the compact `sieda_project_state_json` (no `models3d` meshes: `toJson(false)`, they stay in `Model3DRegistry`); files on disk
    use the indented `sieda_project_save_json`. The view refresh uses `sieda_project_snapshot_delta` (only changed
    top-level sections, merged in `DesignSnapshot.init(from:)` over `DesignStore.coreSnapshot`; the large arrays —
    components, wires, pads, tracks, vias, ratsnest, courtyards, bodies, zoneFills — send only their changed elements as
    `<key>Patch` when the length is unchanged, applied by `list(...)`); a new snapshot section must be decoded through
    `field(...)` there.
  - **Redraws:** every `@Published` write of `DesignStore` redraws the whole window, so write selection state only when
    it changes (`select(component:)`, `refresh()`); nothing in a view `body` / canvas `draw` calls the core or scans the
    catalog per redraw: segment reports, memory limits and PCB swap options go through `DesignStore.cached(_:_:)` (per
    `revision`), the palette's standard-part filter is memoised (`StandardFilterCache`), custom parts are looked up by
    `DesignSnapshot.customPartIndex`; canvas hover is written only when the snapped grid point (schematic) or the hovered
    net / 3 points of movement (PCB) changes. Panels show / hide without a per-frame editor rebuild: canvases hand their size to the editor once it settles
    (`settledSize`, EditorChrome.swift), the inspector and the components / nets sidebar toggle without animation (`ContentView.setInspector` / `setSidebar`, the system sidebar button removed), the picker's
    catalog filter is a static cache. A workspace switch is mostly AppKit laying out the editor's controls: `OptionsBar`, `ToolStrip` and the PCB `LayersPanel` build their controls once (`FillingScrollView`: a scroll view whose content is at least its length; the layers panel's scroll view is as tall as its content), never a `ViewThatFits` with a second copy of controls, `OptionsBar` and the PCB layers panel are built one run-loop pass after the editor appears (the canvas shows first; `built` / `panelsBuilt`), fixed core tables (autoroute presets, sheet templates) are read once, and the device picker lists the ~1,600 standard parts only when expanded or searched (`DevicePicker.showStandard`); `testSchematicAndPcbSwitchTimes` prints the switch times in the CI log, also with the 268-part SMPS board (switching does not grow with the design: the time is AppKit laying out controls). `Project::snapshot` walks the cached pours by reference (`copperFills`
    copies every cell grid).
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
    is kept between fills while its inputs are unchanged (`boardMask_`). The fill cache key (`zoneFills`) hashes every
    fill input, incl. the `spacingDomains` result (net voltages, altitude, coating), via spans and pad shape fields;
    `pourClearanceChecks` (Pcb.cpp) adds DRC_POUR_CLEARANCE (pour rects vs other nets' copper by the fill's own rule).
  - **MCP:** file reads are capped at 128 MB; undo history belongs to one project by its `SiedaProject::serial` (a reused
    address starts empty) and keeps the newest `kUndoHistory` (30) steps, also after loading a saved history.
  - **Previews / outputs:** `sieda_custom_part_preview` builds the part without keeping it (`registerPart(spec, false)`);
    verification runs the DRC once and builds the package without the STL (`fabricationPackage(…, false)`); the STL is
    written with `to_chars` (byte-identical to `%g`); `Schematic::find` binary-searches the id-sorted parts first; gloss
    skips the grid retrace of a line already at its octile length (copper unchanged).
  - **Live endpoint:** at most 16 connections, answered one request at a time (`MCPLiveServer.waitForTurn`); tool
    calls run the core in the background (`MCPStoreBridge.handleLive`, `DesignStore.performExternalEditInBackground`:
    one undo step as before).
  - **Main thread:** heavy core calls stay off it. `runDRC` runs in the background (one at a time plus one queued
    re-run, results tagged with the revision they checked; `drcSettled()` awaits it); verification and the DRC after
    Auto Route run inside `runBusy`; Auto Place (`autoPlaceInBackground`), Gloss and Minimise Vias go through
    `performInBackground` (one undo step); the placement ghost's check is coalesced like `moveRoute`
    (`movePlacementGhostNow` checks synchronously).
  - **`sieda-mcp --connect`:** only connects to 127.0.0.1 / localhost / ::1 and caps replies at 64 MB.
- Hardening: importers' union-find is iterative; the package compare's raster clamps cell indexes before the int cast (`clampCell`, FabCompare.cpp) and skips a non-finite extent; schematic wire spans join through `joinSpans` (LibraryImport.cpp: row /
  column index, capped diagonal scan); VRML `USE` walks stop after 1 000 000 nodes; the SPICE netlist writes ref / value /
  model through `spiceSafe` and part definitions through `withoutControlBlocks` (Export.hpp); `Json::asInt` saturates
  (layers / via spans clamped); `sieda-mcp --connect` dechunks without `at + size` overflow.
- Production extras: tracks / vias saved on the 1 nm grid (`nmVec`, Project.cpp); plug-in objects `Project::pluginObjects`
  (saved only when present, MCP `project_plugin_objects`); MCP `pcb_route_compare` (strategies routed on copies, before / after
  metrics, `recommended`); BOM roll-up lines carry `sources`, `risk` / `riskLevel`, `alternates` (`highRiskLines`); the
  test-point report gives `nearest_mm` / `probe` class (100 / 75 / 50 mil, same side).
- Linux / Windows app SiEDA Studio: `Core/studio/main.cpp` (HTTP on 127.0.0.1 only, random token on the page URL / `X-Sieda-Token` /
  bearer on `/mcp`, Host must be the loopback address; routes `/`, `/state`, `/rpc` = `/mcp`, `/quit`; one request at a time)
  and `Core/studio/ui.html` (embedded by `embed.cmake`; renders shown as `<img>` data URLs, never inline SVG; every action an
  MCP tool call; live routing / track and via drags through MCP `pcb_route_live` (steps work on the router's copy: only
  `commit` records undo and notifies a change, `McpServer::callTool`); board editing through MCP `pcb_edit_copper` (set width / layer / lock, split, merge, tune, gloss, minimize_vias), `pcb_align`, `pcb_cutouts` (pcb_summary lists `cutouts`), drawn pours / keep-outs / holes; library MCP `library_land_calculator` (core `ipc7351Land`, CustomParts.cpp), `library_footprints`, `library_part_spec` / `library_replace_part`, `library_import_model`, `library_company` (team folder: draft / released / obsolete, `librarians.json`, `.history/`); board MCP `pcb_layout_block` (captureLayout / applyLayout: channel replication, "sieda.layout/1" files), `pcb_partitions` (`BoardSettings::partitions`, saved only when present), `pcb_routing_policies` (`AutorouteOptions::classLayers` / `classMaxVias`, DRC ROUTE_POLICY_LAYER / _VIAS); `pcb_rooms` (`BoardSettings::rooms`, saved only when present, DRC_ROOM, place), `pcb_variant_check` (per variant: open nets among fitted pads, overlaps, alternates); macOS menus Design → Layout / File → Company Library (`DesignStore+Team.swift`, through `runTool(_:arguments:root:)`; rooms / partitions drawn on the PCB canvas from `BoardInfo.rooms` / `partitions`); MCP `render_3d` takes hide / isolate / explode / section / fold / pitch / yaw (`Render3dOptions`) and `picks` (each part's screen box, `Render3dPick`, for Studio's click-to-pick 3D view); team rule packs `boardRules` / `boardRulesHash` / `applyBoardRules` (Project.cpp: the rule keys of the board JSON, applied through the project reader), `BoardSettings::rulePackName` / `rulePackHash` (JSON "rulePack", saved only when set), VER_RULE_PACK in Verification.cpp, MCP `pcb_rule_pack` (save / apply / check / show, file "sieda.rules/1"), macOS Design → Team Rule Pack; Studio window laid out like the macOS app (menu row + toolbar: project name, workspace picker `WORKSPACES` drawn with the macOS icons as CSS masks `ICONS` / `iconize`, Undo / Redo / Check; Schematic tool strip on top + Devices / Standard Parts sidebar, PCB tool strip down the left, `.optbar` options bars; Cloud / Tools reached from the menus; last workspace `studio.workspace`, last project reopened `studio.lastProject` unless `studio.reopenLast` = 0; Settings General / Appearance / Language: projects folder, autosave + delay, new-project layers / track / clearance (`applyNewProjectDefaults` → `pcb_set_board`), wheel pans / reverse zoom, export / import (`sieda.studio-settings/1`) and reset of the `studio.*` keys; Midnight Navy default), Studio menus `MENUS` in ui.html (macOS-like; each item a form built from the tool's input schema, `form()`; Ctrl+K `palette()`; Settings = theme + 20 languages: `tools/make_studio_i18n.py` writes `Core/studio/i18n.json` from the macOS tables plus `Core/studio/i18n_extra.json`, served by GET /i18n, CTest `sieda_studio` checks it is current and complete), MCP `project_redo` (`McpServer::redo`; a call of an editing tool that leaves the design JSON unchanged — list / check / show actions — records no undo step and no change notice); macOS windows `CompanyLibraryView` / `BoardAreasView` (Views/Common/LibraryAndAreasViews.swift, `showCompanyLibrary` / `showBoardAreas`; library_company list rows carry description / manufacturer / package / pins / history); Studio polygon pads (land option `outline`, clicked out); the Library tab edits a part through `library_check_part`
  `preview` (drawn part + `customPackage` lands) and adds it with `library_create_part`; editors map clicks through the render's viewBox (design units) and hit-test `project_snapshot` components / pads /
  courtyards, an overlay `<svg>` draws selection / drag / DRC marks; Simulate plots are drawn client-side; `studio_test.py` runs
  `node --check` on the script), guide `docs/STUDIO.md`, CTest `sieda_studio`. Icons: `tools/make_icons.py` writes `packaging/linux/sieda-studio.png`, `packaging/windows/sieda-studio.ico` (in the .exe through `sieda-studio.rc.in`) and the page's favicon from the macOS app icon. Windows builds with MinGW (`_USE_MATH_DEFINES`, `NOMINMAX`,
  static `.exe`; CI job `windows`, release `sieda-windows-x86_64.zip`), `sieda-mcp --connect` works there (Winsock).
  GitHub cloud sync: `Core/src/CloudSync.cpp` (`cloud::git` runs git with no shell, no prompts, no repository hooks /
  fsmonitor; `connect` / `pull` (a `.siedaproj` both sides changed goes through `mergeProjects`: a failing merge driver
  stops git's text merge) / `push` (a rejected push pulls once) / `status`, `shareLinks`, `publiclyReadable`), MCP
  `project_cloud` (GitHub URLs only; push writes `<name>.viewer.html`), test `cloud_sync_pushes_pulls_and_merges_through_a_repository`.
- User KiCad library folders: `Core/src/ExternalLibrary.cpp` (`ExternalLibraries::shared(folders)`: lazy index of `.kicad_sym` /
  `.kicad_symdir` / `.pretty`, `search`, `load` = one symbol + its named footprint through `importLibraryFiles`), set only by
  the server's starter (`McpOptions::libraryDirs`: `--library-dir`, `SIEDA_LIBRARY_DIRS`); `library_search` adds `libraryParts`
  (its catalog part is `libraryCatalogSearch`), `library_add_part` places `"Library:Symbol"`; test
  `user_kicad_library_folders_search_and_place`. Simulator correlation against ngspice: CTest `sieda_sim_correlation`
  (`Core/tests/sim_correlation_test.py`, when ngspice is installed; CI Linux installs it).
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
