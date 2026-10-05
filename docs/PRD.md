# SiEDA — Product Requirements (v1.0)

## Vision
Hardware teams should be able to go from a product idea to manufacturable PCB files in minutes. SiEDA does
this with AI agents whose work is grounded by a deterministic, verifiable EDA engine.

## Target users
- Hardware and embedded engineers who prototype small boards: sensors, indicators, drivers, filters, amplifiers.
- Product managers and makers who can describe a function but not draw a schematic.
- Educators who need simulation and visual feedback.

## Goals
1. Generate from a prompt or PRD: a schematic, an ERC-clean result, simulated operating values, a routed
   two-layer PCB, a 3D model and Gerber/drill files.
2. Let users choose the AI model per workspace. Claude is the default; OpenAI, Gemini, local Ollama and an
   offline mode are alternatives.
3. Give experienced users a professional manual workflow: schematic capture, footprint placement,
   autorouting, DRC and exports.
4. Combine familiar interface conventions from Altium, Photoshop and Proteus. Dark mode is the default and
   the palette is blue.

## Functional requirements
| ID | Requirement | Status |
|---|---|---|
| F1 | Prompt/PRD editor with templates and PRD file import | ✅ |
| F2 | Multi-agent pipeline: analyse → architect → compile → verify → review → layout, with live progress | ✅ |
| F3 | Conversational refinement of an existing design | ✅ |
| F4 | Provider selection (Claude default, OpenAI/compatible, Gemini, Ollama, Offline); keys stored in the Keychain | ✅ |
| F5 | Schematic editor: place, move, rotate, wire, delete, net labels, marquee, undo/redo | ✅ |
| F6 | ERC with cross-probing | ✅ |
| F7 | DC operating point and transient simulation, waveform charts, live probes; AC small-signal analysis with Bode plots, DC and parameter sweeps, Monte Carlo / worst-case tolerance analysis, FFT / THD, noise analysis, imported SPICE models (.model / .subckt, vendor op-amp macromodels) with device capacitances, multi-pole op-amp macromodel, waveform cursors and measurements, trapezoidal and adaptive time steps (docs/SIMULATION.md) | ✅ |
| F8 | PCB: auto-place, manual move/rotate/flip, autoroute, ratsnest, DRC, editable design rules | ✅ |
| F9 | 3D assembly viewer with STL/OBJ export | ✅ |
| F10 | Fabrication package: Gerber RS-274X, Excellon, BOM, pick-and-place, SPICE netlist | ✅ |
| F11 | Project save/open (`.siedaproj`, JSON) | ✅ |
| F12 | Component library: datasheet import, pin-table editor, standard parts catalog, symbol / footprint / 3D generation | ✅ |
| F13 | Footprint Editor: draw any part's land pattern pad by pad (position, size, shape, drill, pin), pad arrays, undo, live checks (overlap, gap, annular ring, pin ↔ pad coverage); converts generated footprints | ✅ |
| F14 | Symbol Editor: pins on any side and slot, gaps, stacked power pins (joined nets), Auto Arrange by name and type, live checks; library parts with 16+ pins arranged | ✅ |
| F15 | Large designs: multi-sheet hierarchical schematics (global / local / port labels, sheet symbols), repeated multi-instance sheets with per-channel designators, nets and footprints, cross-sheet ERC, graphical buses with bus entries and bus ERC, multi-unit symbols on one footprint, annotation with unit packing, assembly variants (DNP and value overrides) for BOM / CPL / assembly exports and simulation (docs/SCHEMATIC.md) | ✅ |
| F16 | Interactive routing: Route tool with walkaround, push-and-shove (recursive, DRC rules kept; pads and locked tracks fixed; post-shove optimiser so shoved copper adds no acute-angle warning) and highlight-collisions modes, 45° / 90° and rounded corners as true arcs (single tracks, concentric on differential pairs and buses; Convert
corners to arcs; arc tracks exact in DRC, connectivity, Gerber G02/G03, pours, lengths, SI and 3D; chords optional), through / blind / buried / micro vias with layer change, snap to pads, commit / cancel / undo; differential pairs at the pair gap, bus routing of a pad row, Teardrops (on demand and on every route), via stitching and shielding, glossing, loop removal on commit,
Select-tool track, corner, multi-track and via drag with shove, any-angle (free) routing, multi-route of picked nets
with bundle vias, Tune Length tool with live preview and gauge (accordion / trombone / sawtooth, square / mitered / round corners,
drag-along placement, coupled pair tuning, phase (skew) tuning; targets from length rules, match groups with xSignals
through series parts, pair / bus groups; DRC_LENGTH), fanout of selected parts; head updates off the main thread (cancellable, latest wins; 400-part benchmark) | ✅ |
| F17 | Schematic productivity: find / replace across sheets, net navigator, hierarchy cross-probing, title block | ✅ |
| F18 | Part library: KiCad, Eagle and Altium (.SchLib / .PcbLib) import with a footprint choice per symbol; VRML / STL / OBJ 3D models per part (3D view, STL / OBJ export); live distributor search (Octopart / Nexar, DigiKey, Mouser) with stock, price breaks and lifecycle, Place Part with sourcing; BOM live pricing and cost per build quantity (docs/LIBRARY_IMPORT.md, docs/SUPPLIERS.md) | ✅ |

## Non-functional requirements
- **Correctness.** All AI output is schema-validated and checked by the core (ERC, simulation, DRC) before
  it is presented.
- **Performance.** Interactive editing of designs with 1000+ parts (O(1) part lookup, culled and batched
  drawing). Autorouting typical small boards takes under one second; a 400-part, 8-layer board with BGAs routes in
  about 20 s, and a 923-part, 8-layer board with an FPGA and four BGAs routes completely in under two minutes in
  under 1 GB (corridor router: global routing, parallel nets, targeted rip-up), with progress and Stop in the app
  (benchmark and limits in [ROUTING.md](ROUTING.md)).
- **Privacy.** Keys live in the Keychain. Only the prompt and design context are sent to the selected provider.
- **Portability.** The core is dependency-free C++17 and is tested on Linux and macOS in CI.

## Out of scope for v1
- Custom symbol graphics (the Footprint and Symbol Editors shipped; see F13, F14; library import shipped, F18).
  Multi-unit symbols shipped (see docs/SCHEMATIC.md).
- STEP 3D models (no CAD kernel; VRML / STL / OBJ are read), Altium integrated (.IntLib), ASCII and database
  libraries, distributor parametric filtering and contract pricing.

## Roadmap
(Shipped since v1: up to 24 layers, copper pours and planes with thermal reliefs, differential pairs, length tuning,
HDI vias and BGA fan-out, autorouting / DRC at the scale of 1000-part boards (corridor router with global routing,
parallel nets and targeted rip-up; negotiated-congestion recovery), and
board-level signal / power integrity: IBIS import, reflections, crosstalk, return path, PDN impedance and IR drop;
then channel analysis: frequency-dependent lossy lines, S-parameters and Touchstone, coupled differential pairs, PRBS
eye diagrams with CTLE / FFE, broadside crosstalk, plane-cavity PDN, decoupling plans and the IR-drop heat map —
see [SIGNAL_POWER_INTEGRITY.md](SIGNAL_POWER_INTEGRITY.md).)

1. ~~KiCad footprint import~~ shipped: Library Import reads KiCad `.kicad_mod` / `.kicad_sym`, Eagle `.lbr` and Altium `.SchLib` / `.PcbLib` libraries into project parts, with a footprint choice per symbol in the import sheet and VRML / STL / OBJ 3D models ([LIBRARY_IMPORT.md](LIBRARY_IMPORT.md)); live supplier search and BOM pricing ([SUPPLIERS.md](SUPPLIERS.md)). Still open: `.IntLib`, STEP models, distributor parametric search.
2. AC small-signal analysis and Bode plots — shipped (F7, docs/SIMULATION.md).
3. ~~Push-and-shove interactive routing.~~ **Shipped** (F16): walkaround, push-and-shove and highlight modes,
   differential pairs, buses, track and via drag, interactive length tuning, HDI vias, rounded corners and fanout.
   Remaining: true arc tracks, bus vias, pair-aware length tuning with coupled meanders. See
   [INTERACTIVE_ROUTING.md](INTERACTIVE_ROUTING.md).
4. Team collaboration through a shared project repository.
