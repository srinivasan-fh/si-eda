# SiEDA roadmap — checklist

Status of the recent work and the planned work, one line per item. Tick an item (`[x]`) when its PR is merged with
CI green. Token figures are rough estimates of the AI work per package (they help plan weekend sessions).

## Current ratings (after the MCAD / team / scale round)

| Area | Rating | Next step that moves it |
|---|---|---|
| Schematic capture | 9.8 | — |
| Interactive routing | 9.7 | — |
| Autorouting | 9.2 | — |
| Editor experience | 9.5 | — |
| Simulation | 9.0 | — |
| SI / PI | 9.0 | — |
| Library | 9.0 | — |
| AI / automation (MCP) | 9.3 | Real-client sessions on the live endpoint |
| Large-design scale | 9.0 | Faster per-net search (batch parallelism is at its deterministic limit) |
| 3D / mechanical | 9.0 | IDX (incremental exchange), STEP import of part models |
| Team, data management, variants | 9.0 | Shared library server, real-time co-editing |
| **Overall** | **~9.3** (features), ~9.0 (production readiness) | |

## Done (merged)

### Schematic and editor
- [x] Schematic canvas colour schemes (14 presets named by colour) and grid styles (Dots / Lines / None) — #77
- [x] Custom named-colour theme for the schematic canvas — #77
- [x] Device icons drawn from the real schematic symbols — #78
- [x] A standard colour per device family (icons, list, inspector) — #79
- [x] Symbols drawn in their device colour on the canvas by default ("Colour Symbols by Device") — #80
- [x] Focus mode: F11 / ⌃⌘F / Fn-F full screen with only the editor; options bar on hover — #81

### Interactive routing
- [x] Shove pushes arc tracks (re-filleted or concentric) — #81
- [x] Shove pushes S-curves / arc chains and arcs pinned on pads or vias — #81
- [x] Curved teardrops (TeardropStyle Straight / Curved; router, autorouter, C API, app) — #81
- [x] Tune lengths while routing: bus / matched-net targets, meanders on commit — #81
- [x] Live meanders in the preview while dragging — #81
- [x] Differential pairs tuned while routing (coupled meanders + skew) — #81

### Autorouting
- [x] Autorouting round (corridor router, coupled pairs, length-aware tuning, gloss, arcs, teardrops) — #76
- [x] Pin and gate swap before routing, back-annotated to the schematic — #81

### Update PCB
- [x] Interactive placement of new parts after Update PCB (ghost, R / F, Skip, Place All, undo) — #81
- [x] Parts still waiting in the queue no longer block placement — #81
- [x] Fix: a part straddling the board edge is now reported as outside the outline — #81

### Mechanical CAD, team work, scale
- [x] STEP AP214 export: board + one closed, coloured, named solid per part (validated with OpenCASCADE)
- [x] IDF 3.0 board / library export and MCAD placement import (one undo step)
- [x] Version diff (parts, nets by pins, copper per net, board, variants): app, MCP `project_diff`, `sieda-mcp --diff`
- [x] Git diff driver (`sieda-mcp --git-diff`)
- [x] Variants side by side (Compare Variants…, MCP `schematic_variant_matrix`)
- [x] Scale measured: 927 parts route 100 % in 45–50 s / 413 MB; 1756 parts in 175 s / 740 MB; CTest budget guard
- [x] Interactive routing on the 927-part board: median 6–8 ms, p99 ≈ 40 ms per update
- [x] Corridor batches analysed: 272 dependent batches (≈ 4 nets each) — speculative next-batch search kept the copper
      identical but was slower (most speculation invalidated), so it was not merged
- [x] 3D clearance DRC: body collisions, enclosure height per side, height zones
- [x] Three-way merge of project files (Git merge driver `sieda-mcp --merge`, MCP `project_merge`)
- [x] Design review comments (pinned to parts / places, replies, resolve, Markdown report; diff and merge aware)

## Weekend order (pick up from the top)

The agreed order for the weekend sessions; each line is one package (PR, CI green, merge). The sections below break
each package into its steps.

1. [x] Large-board benchmark (in CI as `sieda_scale_budget`); next: corridor-router thread scaling
2. [ ] PDF text shaping with HarfBuzz (~0.15M–0.25M)
3. [x] STEP export
4. [x] Variant manager (variants existed; comparison matrix added)
5. [x] Design diff and 3-way merge (app, MCP, Git diff and merge drivers)
6. [x] IDF 3.0 exchange (IDX still open)
7. [ ] Shared library server (~0.2M–0.3M)
8. [x] Design review
9. [x] 3D clearance checks
10. [ ] Windows build

## App themes — plan later

Midnight Navy, Matrix Green and Graphite colour the whole app (`AppTheme`). Still to do:

- **Light app theme** (~0.15M–0.25M tokens): light panels need dark text everywhere, so audit the views that assume a
  dark background (white text on accents, `.opacity` washes over `Theme.navy`, PCB / 3D canvases that stay dark),
  give `AppTheme.Colours` the missing roles (on-accent text, canvas versus panel background), pair it with a light
  schematic scheme (Paper White) and extend `testEveryThemeIsReadable` to 4.5:1 for body text.
- Optional: theme without the full-window redraw (an environment value read by the views), so open popovers and
  scroll positions survive a theme change.

## Quality and depth (planned, in suggested order)

Queued after the ⌘K / delta-snapshot / IPC-2581 / panel / onboarding / performance / fuzzing round; one PR each.

1. **DFM/DFA rule packs** (~0.2M tokens): manufacturer capability packs as data (JLCPCB, PCBWay, OSH Park,
   Eurocircuits, Class 3 / aerospace): minimum track / space, drill, annular ring, mask sliver, silk-to-pad; picking a
   pack sets the DRC limits. Assembly checks: part-to-part and part-to-edge spacing, fiducials, polarity marks, tall
   parts beside fine pitch.
2. **IDX (ProSTEP EDMD)** (~0.25M): baseline export (board, parts, holes, keep-outs), MCAD change proposals read back
   (moved / rotated parts, outline edits) with accept / reject, SiEDA changes sent as proposals.
3. **HarfBuzz text shaping in PDFs** (~0.2M): Indic, Arabic and Thai shaped correctly in the schematic PDF; an optional
   CMake dependency, so builds without it keep today's byte-identical output (the archival plan avoids dependencies).
4. **Field-solver SI / PI** (~0.5M): 2D cross-section solver (single-ended, differential, coupled: impedance,
   coupling, crosstalk, loss) replacing the closed-form formulas when enabled; plane solver on the real copper for PDN
   impedance and DC IR drop with a heat-map overlay.
5. **Real-time co-editing** (~0.7M): local-network session, edits broadcast as delta operations, conflicts through
   the three-way merge, presence (who is where on the board); an internet relay with accounts comes later.

## Next (planned, in suggested order)

### 1. Large boards (scale 8.0 → ~9.0) — ~0.35M–0.65M tokens
- [x] Benchmark board with 923 parts: 50 s, 413 MB (target < 60 s, < 500 MB); CI guard on a 350-part board
- [x] Profile routing, DRC, snapshot on it: corridor batches 38 s of 50 s; DRC 0.05 s; snapshot 0.12 s
- [ ] Live tuning: copy only the affected nets instead of the whole board
- [ ] Fix the hotspots the benchmark finds

### 2. Schematic PDF text shaping — ~0.15M–0.25M tokens (HarfBuzz)
- [ ] Decide: HarfBuzz dependency (recommended) or own shaper
- [ ] Arabic joining and right-to-left order
- [ ] Indic (Hindi, Tamil, …) conjuncts and reordering
- [ ] Tests with sample sheets in each script

### 3. 3D / mechanical (7.0 → ~9.0) — ~0.45M–0.7M tokens
- [x] STEP export (AP214) of board and parts
- [x] IDF 3.0 exchange with MCAD (outline, holes, placement, placement round trip); IDX still open
- [x] 3D clearance and enclosure checks
- [ ] Optional: STEP import of part models (needs OpenCASCADE) — ~0.25M–0.4M

### 4. Team, data management, variants (6.0 → ~8.5) — ~0.75M–1.1M tokens
- [x] Variant manager (table of variants, per-variant outputs, comparison)
- [x] Design diff of project files (app, MCP, Git diff driver)
- [x] 3-way merge of project files
- [ ] Shared library server (versioned parts, lifecycle states, where used) — ~0.2M–0.3M
- [x] Design review (pinned comments, status, Markdown report)
- [ ] Optional: real-time multi-user editing (needs a sync server) — ~0.8M–1.5M+

### 5. Platforms (on hold — plan later)
- [ ] Windows: build the C++ engine with MSVC in CI (Ubuntu, macOS, Windows)
- [ ] Choose the Windows / Linux UI framework (Qt 6 recommended; or web / .NET)
- [ ] App skeleton: open / save, schematic and PCB canvases, zoom, selection
- [ ] Port the editors one area per PR

## Long-term archival ("1,000-year" designs) — plan later

Principle: the data must outlive the software. Formats first, executables second.

### Already true
- [x] Works fully offline (AI and supplier prices are optional; design work never needs the network)
- [x] Plain-text project file (`.siedaproj`, UTF-8 JSON)
- [x] Open, documented, text-based outputs: Gerber X2, Excellon, IPC-D-356, SVG, PDF, STEP AP214, IDF 3.0
- [x] Core has no third-party libraries and renders PNG / SVG with its own code

### Archive Package — File → Export Archive… (~0.1M–0.15M tokens)
- [ ] `README.txt`: plain English — what the folder is and how to read every file
- [ ] `FORMAT.txt`: `.siedaproj` specification (units, axes, every field)
- [ ] `"readme"` field at the top of the project JSON explaining the syntax (written only in the archive copy)
- [ ] `PHYSICS.txt`: the formulas the engine uses (IPC-2221 current, impedance, decoupling)
- [ ] `netlist.txt`: `ref.pin → net`, one per line
- [ ] `schematic.pdf` and 1:1 `layers/*.svg` with scale bar and coordinates (for printing / etching onto archival media)
- [ ] `fab/` (Gerber, drill, IPC-356, BOM, CPL) and `mechanical/` (STEP, IDF)
- [ ] `checksums.txt`: SHA-256 of every file (detects bit rot)
- [ ] Core test: the archive is complete and every checksum matches

### WebAssembly core (~0.1M tokens)
- [ ] Emscripten target for the dependency-free C++17 core (`sieda-core.wasm`), included in the archive
- [ ] CI job that keeps the WebAssembly build green

### Deliberately not planned
- Self-compiling ("quine") toolchain: years of work; documented formats + clean C source serve a future engineer better
- Electronics textbook inside the source: a short `PHYSICS.txt` in the archive instead
- "Biological / atomic" manufacturing outputs: exact vector geometry + netlist + rules + stack-up already adapt to any process
- A minimal "stone-age" UI: the UI is the most replaceable layer; keep the data and the core durable instead

## Arm-focused production-ready PCB (plan for review) — ~2.1M–3.2M tokens

Goal: pick an Arm processor and its peripherals; SiEDA produces a minimal, low-cost, production-ready board
(schematic → pin assignment → 2- / 4-layer PCB → checks → Gerbers, drill, BOM, pick-and-place).

### Open questions (answer before Phase 1)
- [ ] Fab house for the cost rules: JLCPCB, PCBWay or both
- [ ] Phase 1 boards: LPC1769, STM32F103, RP2350 (or swap one)
- [ ] Firmware starter code (CMSIS startup, pin-mux, clock config): yes / no
- [ ] Cortex-M firmware simulation: skip for now (recommended) / plan it
- [ ] Cortex-A only as a system-on-module carrier (no bare DDR boards): yes / no

### Arm portfolio coverage
- [ ] Cortex-M (M0+ … M85): STM32, LPC, SAM, RP2040 / RP2350, MSPM0, TM4C — main focus
- [ ] Cortex-R (R5, R52): TMS570, AM243x, RZ/T2 — Phase 3
- [ ] Cortex-A + Mali + Ethos-U: i.MX 93, AM62x, CM4 / CM5 as SoM carriers — Phase 3
- [ ] Ethos-U in MCUs: Alif Ensemble E3 / E7 — Phase 3
- [ ] System / Security / Subsystem IP: as board checks (secure boot pins, debug lock, tamper / VBAT)
- Out of scope: Cortex-X, Neoverse, CSS, C2 cluster, AGI CPU (not sold for small-run boards)

### Phase 1 — foundation and three boards (~0.9M–1.3M tokens)
- [ ] Pin-mux planner (Pin Connect Block): legal pins per peripheral, conflicts, routing-friendly choice (core, C API, app)
- [ ] Power block: USB 5 V → 3.3 V LDO, decoupling per VDD pin, VDDA ferrite, reverse protection
- [ ] Clocking & power control, PLL: main and 32.768 kHz crystals with load caps calculated, PLL limits checked
- [ ] System & reset control: reset RC / supervisor, boot / ISP strap, reset button
- [ ] Boot loader: ISP strap + USB-UART with auto-reset (DTR / RTS)
- [ ] Debug: 10-pin Cortex SWD connector (+ TC2030 option)
- [ ] UARTs 0–3, I2C 0–2 (pull-ups from bus capacitance), SPI / SSP 0–1 (series termination)
- [ ] CAN 0–1: transceiver, split termination, ESD
- [ ] A/D converter (anti-alias RC, reference decoupling), D/A converter (output filter)
- [ ] Real Time Clock: VBAT coin cell / supercap with diode OR
- [ ] GPIO ports and GPIO interrupts: headers, LED, button, protection
- [ ] Firmware-only features noted (no circuitry): Flash IAP, memory mapping, MPU, NVIC, SysTick, RIT, fault reports, WDT
- [ ] Low-cost design-rule preset (JLCPCB / PCBWay standard 2- and 4-layer)
- [ ] Arm bring-up checklist (SWD reachable, boot pins, every VDD decoupled, VDDA filtered, crystal caps, reset)
- [ ] JLC-format BOM and pick-and-place output
- [ ] Reference board: LPC1769 dev board (4-layer) — full LPC17xx peripheral set
- [ ] Reference board: STM32F103 minimal (2-layer)
- [ ] Reference board: RP2350 minimal (2-layer)

### Phase 2 — remaining blocks and three boards (~0.6M–0.9M tokens)
- [ ] Ethernet controller: RMII PHY (LAN8720A), 50 MHz clock, magjack, 100 Ω pairs
- [ ] USB device / host / OTG: ESD, 90 Ω pair, VBUS switch, ID pin, Type-C CC resistors
- [ ] General-purpose DMA (firmware note), I2S interface with codec block
- [ ] Motor control PWM, PWM (safe pull-downs, gate-driver interface), QEI (encoder filter, 5 V level)
- [ ] Timers 0–3 capture / match breakout
- [ ] BOM cost with basic-part preference
- [ ] Peripheral checkboxes in the app
- [ ] Reference boards: SAMD21 (2-layer), STM32F411 (2-layer), MSPM0G3507 or TM4C123 (2-layer)

### Phase 3 — Cortex-R, Cortex-A carrier, Ethos-U (~0.6M–1.0M tokens)
- [ ] Cortex-R safety board (TMS570LC43 or AM2434)
- [ ] Edge-AI board with Ethos-U (Alif E3)
- [ ] Cortex-A SoM carrier (CM5 or i.MX 93 SoM)

### Optional
- [ ] Firmware starter code generated from the board (~0.2M–0.3M)
- [ ] Cortex-M firmware simulation in the circuit simulator (~1.0M–1.5M)
