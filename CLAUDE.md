# SiEDA — notes for Claude Code

- Core (C++): `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests`
- macOS app: `./run.sh` (builds the core, generates the Xcode project, runs the app).
- Footprint Editor: core in `Core/src/CustomParts.cpp` (`landPatternFromFootprint`, `checkLandPattern`), app in
  `SiEDA/Models/FootprintDraft.swift` and `SiEDA/Views/Library/FootprintEditorView.swift`; guide in
  `docs/FOOTPRINT_EDITOR.md`. Keep the Swift `CustomPartSpec.Land` encoding identical to the core's land JSON.
- Symbol Editor: core in `Core/src/CustomParts.cpp` (`autoArrangeSymbol`, `checkSymbol`, four-sided layout) and
  `Core/src/Schematic.cpp` (stacked pins join nets), app in `SiEDA/Models/SymbolDraft.swift` and
  `SiEDA/Views/Library/SymbolEditorView.swift`; guide in `docs/SYMBOL_EDITOR.md`.
- Memory (RAM) design segments: core in `Core/src/Memory.cpp` (`memoryChecks`, `memorySegments`), app via
  `EDAEngine.memorySegments` / `DesignStore.setMemoryDesign`; guide in `docs/MEMORY_DESIGN.md`. The memory reference design
  (STM32H743 + SDRAM) must keep passing verification on 6 layers.
- Schematic capture: sheets / hierarchy / bus labels / annotation in `Core/src/Sheets.cpp`, repeated sheets in
  `Core/src/Instances.cpp` (`syncInstances` keeps channel copies in line; every Schematic edit calls it), graphical
  buses in `Core/src/Buses.cpp`, multi-unit parts in `Core/src/PartUnits.cpp` (kind PartUnit + hidden `packageOnly`
  package; the snapshot reports units as kind Custom with `unitOf`), find / replace and the net navigator in
  `Core/src/SchematicSearch.cpp`, variant simulation via `Project::simulationSchematic`; app in
  `SiEDA/Views/Schematic/`; guide in `docs/SCHEMATIC.md`.
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
