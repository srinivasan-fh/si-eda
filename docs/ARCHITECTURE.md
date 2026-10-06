# SiEDA Architecture

```
┌─────────────────────────────── SwiftUI app (SiEDA/) ───────────────────────────────┐
│ Prompt Studio · Schematic · PCB · 3D · Simulation · Checks · Inspector · Settings  │
│                     ▲ @Published DesignSnapshot / results                          │
│ DesignStore (MainActor: undo stack, documents, analysis results, workspace)        │
│ AgentOrchestrator ── AIProvider { Claude* | OpenAI | Gemini | Ollama | Offline }   │
│                     │ DesignPlan (JSON schema-constrained)                         │
│ DesignPlanCompiler ─┘                                                              │
│ EDAEngine (thread-safe Swift façade, NSLock)                                       │
└───────────────────────────────┬────────────────────────────────────────────────────┘
                                │ C ABI: sieda_c.h (opaque handles, JSON strings, float arrays)
┌───────────────────────────────▼──────────── C++17 core (Core/) ────────────────────┐
│ Project ── Schematic (components, wires, nets, ERC)                                │
│        ├── Simulator (MNA, Newton–Raphson, DC + transient)                         │
│        ├── PcbLayout (pads, auto-place, A* autorouter, DRC, ratsnest)              │
│        │     └── GlobalRouter (tiles, corridors, parallel batches)                 │
│        ├── Mesh (3D assembly)       ├── Export (Gerber, Excellon, SPICE, BOM, STL) │
│        └── Json (persistence + UI snapshots)                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
```

## Why a C ABI instead of direct Swift/C++ interop?

The core exposes `extern "C"` functions behind opaque handles. Results come back as UTF-8 JSON that Swift
decodes with `Codable`. Mesh data comes back as raw `float`/`uint32_t` arrays that go straight into SceneKit.
This gives us:

- **Stability.** The ABI is independent of the Swift compiler's C++ interop mode and of the libc++ version.
- **Testability.** The same ABI is exercised from C (`Core/tests/c_api_test.c`), from Swift (XCTest) and from Python (ctypes).
- **Safety.** Every entry point catches exceptions, and errors are returned as values.

## Core engine

### Schematic and connectivity
- Components reference library definitions (`Library.cpp`) by stable numeric kind. The Swift
  `ComponentKind` raw values mirror these, and `SiEDATests` checks that the two stay in sync.
- Nets are computed lazily with union-find. Wires join pins, identically named net labels join nets, and
  ground symbols (or labels named `GND`) join the ground net.
- ERC rules:
  - missing ground
  - unconnected pins and floating parts
  - shorted sources
  - duplicate designators
  - invalid values
  - LEDs driven without current limiting
  - parts that have no simulation model

### Simulator
- Modified Nodal Analysis with dense LU and partial pivoting.
- Non-linear devices use Newton–Raphson. Device Jacobians are central finite differences of the terminal
  current functions. Exponentials are linearised past a limit (`limexp`) and node-voltage steps are damped.
- If Newton fails to converge, the solver falls back to gmin stepping and then source stepping.
- Transient analysis uses backward-Euler companion models for C and L, and sub-steps on non-convergence.
- AC analysis reuses the Newton Jacobian at the operating point as the small-signal matrix, adds jωC, jωL and the
  op-amp pole, and solves the complex system per frequency. DC / parameter sweeps, Monte Carlo / worst case and
  FFT / THD build on these analyses in `Analysis.cpp` (see [SIMULATION.md](SIMULATION.md)).

### PCB
- **Auto-placement.** A greedy, connectivity-ordered placer. Each part goes where the Manhattan distance from
  its pads to the centroids of their nets is lowest. It tries 0° and 90° rotations and keeps courtyards
  apart with routing channels.
- **Autorouter.** A two-layer grid A* (0.25 mm pitch by default):
  - Moves can be orthogonal or 45°. Each layer has a preferred direction, turns cost extra, and vias are penalised.
  - Each net is built as a Steiner-like tree: every new connection may start from any copper already on that net.
  - Keep-out grids enforce clearance: track to track, track to pad, and via to everything.
  - Failed nets are promoted to the front and the board is re-routed (up to 8 passes); the best result is kept. If
    connections remain unrouted, recovery passes route with negotiated congestion (history costs on the corridors the
    failed connections need) and are kept only when they route more. See [ROUTING.md](ROUTING.md).
  - Large boards (2 M grid nodes or more) use the corridor router instead: `GlobalRouter` (`Core/src/GlobalRouter.cpp`)
    routes every connection over ~2 mm tiles with negotiated congestion, the same A* then searches only that
    corridor (search state paged per tile), nets with disjoint corridors run on several `std::thread`s against the
    same board state and commit in routing order (identical copper for any thread count), and failures are repaired
    by ripping up only the nets in their way. Smaller boards keep the classic router bit for bit. On BGA boards the
    corridor router's first pass searches a multi-resolution grid (a ~0.25 mm lattice away from pads and BGA fields);
    if that leaves connections open, the board is routed again on the full grid and the better result is kept.
  - The pass loop is a function (`runPass`): routing passes, rip-up / recovery passes and the via-minimisation stage
    (batches of nets that lie apart, each re-routed alone with costly layer changes) all run through it.
  - Strategy options (`AutorouteOptions`, `BoardSettings::autorouter`; every default is the old behaviour): coupled
    differential pairs (`CoupledPairRouter.inc`: a centre-line A* whose state carries direction, polarity side and
    run length, members offset at the pair spacing, coupled via pairs), length-aware routing, via minimisation, gloss /
    arcs / teardrops (`RouteQuality.cpp`, through the interactive router's engines, each undone if it opens a
    connection), keep-outs, scope (nets, net class, area; other copper fixed), locked copper, per-class layers.
    `RouteStats::report` (pairs, lengths, metrics) is kept as `PcbLayout::lastRouteReport`.
  - `RouteControl` reports progress from the routing thread and cancels (leaving the board unchanged); the app shows it
    in the status bar with Stop.
- **Tracks** are straight segments or true circular arcs (`Track::arc`, 3-point form). Every consumer measures them
  through `TrackGeometry.hpp` (exact arc distances, lengths, bounds, tangents); for a straight track each function is
  the segment formula used before arcs, so boards without arcs give bit-identical results. Gerber writes arcs with
  `G75`/`G02`/`G03`. The autorouter writes straight tracks unless its arc-corners option is on, and rips up the routing first unless a strategy scope or locked copper keeps it.
- **DRC.** Exact geometric checks on the routed copper (segment–segment, segment–rectangle, point–circle and the
  arc cases), edge clearance, courtyard overlap, and unrouted connections. Pairs are found through a uniform-grid
  spatial index, in the order of a full scan, so large boards check in O(n log n) with identical reports. Unrouted connections are found
  from copper connectivity, not from router bookkeeping.
- **Edits after routing.** After schematic edits, copper is re-associated with nets through its contact with
  pads. Copper that no longer touches any pad is pruned.

### Components and footprints
- **Custom parts** (`CustomParts.cpp`) are specs (name, pins with electrical types, package). From a spec the
  core generates a schematic symbol, a footprint and a 3D body, and registers the part under an id hashed from
  the spec. The standard library, the datasheet importer and the Footprint Editor all produce such specs.
- **Parametric packages** (SOIC, TSSOP, DIP, QFN, LQFP, SOT-23, SON, BGA, TO-220 / TO-263, SOT-223, modules,
  headers, crystals) place pads from a pin count, pitch and body size. QFN, TSSOP, LQFP, SON and module packages
  can add an exposed pad; TO-263 and SOT-223 add a tab.
- **Land patterns** (`LGA`, `CUSTOM`) carry the exact pads instead: one land per pad with position, size, drill,
  shape and, optionally, the pin it belongs to (a tab, `EP`, a BGA ball, or `-` for a mechanical pad). The
  catalog's irregular parts use `LGA`, generated pad for pad from the KiCad footprints; the Footprint Editor
  writes `CUSTOM`.
- **Library import** (`LibraryImport.cpp`) reads KiCad `.kicad_mod` / `.kicad_sym` (s-expressions) and Eagle `.lbr`
  (XML) with its own bounded readers, maps footprints to `CUSTOM` land patterns and symbols to pins and a symbol
  layout, pairs them, and validates each part with `checkSymbol`, `checkLandPattern` and registration. See
  [LIBRARY_IMPORT.md](LIBRARY_IMPORT.md).
- **Footprint editing.**
  - `landPatternFromFootprint` converts any generated footprint into an editable land pattern, with the same
    pads on the same pins.
  - `checkLandPattern` reports overlapping pads, copper gaps below 0.1 mm, annular rings, pads without pins and
    pins without pads.
  - Registration refuses a land pattern that leaves a pin without a pad.
  
  The editor UI (`FootprintEditorView`, `FootprintDraft`) is pure Swift over these two calls. See
  [FOOTPRINT_EDITOR.md](FOOTPRINT_EDITOR.md).
- **Symbols.**
  - A part's symbol is generated: pins in number order down the left side, then up the right.
  - Alternatively it comes from a `symbolLayout`: each pin's side (L / R / T / B) and slot.
  - `autoArrangeSymbol` builds a layout from pin names and types, and library parts with 16+ pins get one.
  - Pins on the same spot are stacked. `rebuildNets` joins them into one node, and `isPinConnected` still treats an
    unwired stack as open.
  - `checkSymbol` reports unplaced, unknown, duplicated and colliding pins. Registration refuses those errors.
  
  The Symbol Editor (`SymbolEditorView`, `SymbolDraft`) works over these calls. See [SYMBOL_EDITOR.md](SYMBOL_EDITOR.md).

## AI agents

| Agent | Input | Output (JSON schema) |
|---|---|---|
| Requirements Analyst | prompt / PRD | `RequirementsSpec` |
| Circuit Architect | brief + spec (or current plan + change request) | `DesignPlan` |
| Plan Compiler (core) | `DesignPlan` | schematic + `PlanApplyReport` |
| Verification (core) | schematic | ERC findings + DC operating point |
| Design Reviewer | spec + plan + ERC + DC report | `DesignReview { approved, issues, plan }` |
| PCB Layout (core) | schematic | placement, routing, DRC |

Agents never edit the design directly. Every model response is a schema-constrained `DesignPlan`, and the
deterministic core validates it, simulates it and lays it out. The reviewer agent then sees the real
simulation numbers, not the model's own estimates.

### Claude integration (default)
The app sends `POST https://api.anthropic.com/v1/messages` with:
- `model`: `claude-opus-5-5` by default
- `output_config.format = {type: "json_schema", schema}` (structured outputs)
- `output_config.effort` (default `high`)
- `thinking: {type: "adaptive"}`

On models that support it, the request also sends `fallbacks: "default"` with the beta header
`server-side-fallback-2026-07-01`. The app checks `stop_reason` for `refusal` and `max_tokens` before it
reads any content. API keys are stored in the Keychain.

## UI composition
- **Photoshop:** the `ToolStrip`, the `OptionsBar` and the PCB `LayersPanel` (eye toggles, active layer).
- **Altium:** the Properties inspector, the `LayerTabs` along the bottom of the PCB editor, and the Checks
  panel with cross-probing to the affected parts.
- **Altium / KiCad library editors:** the Component Library (pin table, symbol and footprint previews) and the
  Footprint Editor sheet (pad canvas on a grid, pad properties, pad arrays, live land-pattern checks) and the Symbol
  Editor sheet (pins dragged to any side and slot, stacks, Auto Arrange, live symbol checks).
- **Proteus:** the `DevicePicker` with symbol preview, the `SimulationTransport` controls and live DC probes
  drawn on schematic nets.
- **Theme:** `Theme.swift` defines the palette. The app is dark by default and blue throughout; warm colours
  are kept only for warnings and errors.
