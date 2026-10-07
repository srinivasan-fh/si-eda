# SiEDA roadmap — checklist

Status of the recent work and the planned work, one line per item. Tick an item (`[x]`) when its PR is merged with
CI green. Token figures are rough estimates of the AI work per package (they help plan weekend sessions).

## Current ratings (after PR #81)

| Area | Rating | Next step that moves it |
|---|---|---|
| Schematic capture | 9.8 | — |
| Interactive routing | 9.7 | — |
| Autorouting | 9.2 | — |
| Editor experience | 9.5 | — |
| Simulation | 9.0 | — |
| SI / PI | 9.0 | — |
| Library | 9.0 | — |
| Large-design scale | 8.0 | Large-board benchmark and fixes |
| 3D / mechanical | 7.0 | STEP export, IDF / IDX |
| Team, data management, variants | 6.0 | Variant manager, diff / merge, library server |
| **Overall** | **~9.0** (features), ~7.5 (production readiness) | |

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

## Next (planned, in suggested order)

### 1. Large boards (scale 8.0 → ~9.0) — ~0.35M–0.65M tokens
- [ ] Benchmark board with 923 parts in CI (time and memory; target < 60 s, < 500 MB)
- [ ] Profile routing, DRC, snapshot and canvas on it
- [ ] Live tuning: copy only the affected nets instead of the whole board
- [ ] Fix the hotspots the benchmark finds

### 2. Schematic PDF text shaping — ~0.15M–0.25M tokens (HarfBuzz)
- [ ] Decide: HarfBuzz dependency (recommended) or own shaper
- [ ] Arabic joining and right-to-left order
- [ ] Indic (Hindi, Tamil, …) conjuncts and reordering
- [ ] Tests with sample sheets in each script

### 3. 3D / mechanical (7.0 → ~9.0) — ~0.45M–0.7M tokens
- [ ] STEP export (AP214) of board and parts — ~0.2M–0.3M
- [ ] IDF 3.0 / IDX exchange with MCAD (outline, keep-outs, placement, round trip) — ~0.15M–0.25M
- [ ] 3D clearance and enclosure checks — ~0.1M–0.15M
- [ ] Optional: STEP import of part models (needs OpenCASCADE) — ~0.25M–0.4M

### 4. Team, data management, variants (6.0 → ~8.5) — ~0.75M–1.1M tokens
- [ ] Variant manager (table of variants, per-variant outputs, comparison) — ~0.15M–0.2M
- [ ] Visual design diff and 3-way merge of project files (Git-friendly) — ~0.25M–0.4M
- [ ] Shared library server (versioned parts, lifecycle states, where used) — ~0.2M–0.3M
- [ ] Design review (pinned comments, status, review PDF) — ~0.15M–0.2M
- [ ] Optional: real-time multi-user editing (needs a sync server) — ~0.8M–1.5M+

### 5. Platforms (on hold — plan later)
- [ ] Windows: build the C++ engine with MSVC in CI (Ubuntu, macOS, Windows)
- [ ] Choose the Windows / Linux UI framework (Qt 6 recommended; or web / .NET)
- [ ] App skeleton: open / save, schematic and PCB canvases, zoom, selection
- [ ] Port the editors one area per PR

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
