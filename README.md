# SiEDA — AI-native Electronic Design Automation for macOS

SiEDA is a professional EDA suite: a **SwiftUI** macOS app on top of a **C++17** engine.
You describe a product in plain language or paste a PRD. A team of AI agents (Claude by default, or any
model you choose) then turns it into a verified schematic, a simulated circuit, an autorouted two-layer
PCB, a 3D assembly and fabrication-ready Gerber files.

The interface combines conventions from three tools:

- **Altium Designer:** docked Properties and Projects panels, coloured layer tabs, a Messages-style check panel with cross-probing.
- **Photoshop:** a vertical tool strip, a tool-options bar and a layers panel with visibility toggles.
- **Proteus:** a device picker with symbol preview, simulation transport controls and live voltage probes on the schematic.

The app defaults to dark mode with a blue palette.

## Use it with or without AI

AI is optional. A single switch (**Settings → AI Models → Enable AI assistance**, the toolbar AI menu, or
**Design → AI Assistance**, ⌥⌘A) changes how SiEDA works:

| | AI assistance **on** | AI assistance **off** (manual mode) |
|---|---|---|
| Start | AI Prompt Studio: describe the product, agents design it | Schematic editor; **File → New from Example** for reference designs |
| Schematic, simulation, PCB, autorouting, DRC, 3D, exports | ✓ | ✓ (all run locally in the C++ core) |
| Datasheet import | AI reads the PDF or image | Offline pin-table parser plus OCR; review the result in the pin editor |
| Network traffic | Only to the AI provider you select | None |

Every AI result lands in the same editable schematic and PCB, so you can switch modes at any time.

## Features

| Area | What it does |
|---|---|
| **AI Prompt Studio** | Prompt/PRD editor, template briefs, PRD import, live agent pipeline, conversational refinement ("make the LED green and run it from 3.3 V") |
| **Agent team** | Requirements Analyst → Circuit Architect → Plan Compiler → Verification (ERC + SPICE) → Design Reviewer (feedback loop) → PCB Layout |
| **Standard components** | Built-in standard library (LM7805, LM317, NE555, LM358, ATtiny85, ATmega328P, 74HC595, 74HC00, ULN2003A, L293D, pin headers) with correct pinouts, pin types and footprints, in the device picker and Component Library. AI agents can use them too. IEC 60063 E12/E24/E96 value checks with a one-click "use nearest standard value" in the Properties panel |
| **Basic circuit library** | 13 verified reference designs in 7 groups (**File → New from Example**): LED indicator, voltage divider, 5 V LM7805 regulator, half-wave rectifier, non-inverting/inverting amplifier, voltage follower, RC low-/high-pass filters, NPN and MOSFET drivers, 555 astable blinker, Wheatstone bridge. Every one passes full verification on 1, 2, 4 and 6 layers |
| **Circuit validation** | Beyond ERC: non-standard values, missing IC decoupling, and DC-derived part ratings (resistor power, LED/diode current, reverse-biased LEDs, transistor current/power, supply over-current, op-amp saturation, fuse overload). Exceeding a rating is a warning; 2× the rating is an error |
| **PCB design rules** | Presets: Prototype (Conservative), IPC-2221 Class 2 (default), IPC-2221 Class 3, Fab House Standard (6/6 mil) and Advanced (4/4 mil). DRC checks clearance against both the design rule and the fab minimum, plus track width, drill size, annular ring, hole-to-hole spacing, via-in-pad, dangling tracks, acute angles and IPC-2221 current capacity from the simulated net currents |
| **Verification process** | **Design → Verify Design** (⇧⌘V) runs a 7-stage sign-off: ERC → DC simulation → circuit validation → footprint placement → routing completion → DRC/fab rules → manufacturing outputs (every Gerber layer, drill, BOM, pick-and-place and netlist are generated and checked). The verdict is Pass, Pass with warnings or Fail; findings cross-probe to the editors, and the report exports as Markdown. **Export Fabrication Package** always verifies first, asks before exporting a failing design, and includes `verification_report.md` |
| **Model choice** | Anthropic **Claude** (default: `claude-opus-5-5`, structured outputs, adaptive thinking, effort control, refusal fallbacks), OpenAI or any OpenAI-compatible endpoint, Google Gemini, local Ollama, and an Offline Designer that needs no network |
| **Schematic capture** | 16 built-in device types plus your own library parts, orthogonal wiring, net labels, junctions, rotate/move/marquee, undo/redo, ERC with pin-type rules |
| **Datasheet → component** | Drop a datasheet (PDF, pinout screenshot or text). The Datasheet Analyst agent extracts part number, package and every pin with its electrical type: Claude and Gemini read the PDF natively, OCR (Vision) handles images, and an offline pin-table parser works without a key. You review it in a pin-table editor with live symbol and footprint previews, then save it to the project library and place it like any other part. AI design agents can use library parts too |
| **Simulation** | Modified Nodal Analysis with Newton–Raphson: DC operating point and transient. Diode/LED (Shockley), BJT (Ebers–Moll), MOSFET (square-law with λ), saturating op-amp, R/L/C, DC/SIN/PULSE sources. Waveforms are drawn with Swift Charts |
| **PCB layout** | Footprint library (0805, SOD-123, SOT-23, SOIC, TSSOP, DIP, QFN with exposed pad, LQFP, headers, TO-220), connectivity-driven auto-placement, board fit, ratsnest, geometric and manufacturability DRC (see design rules) |
| **Autorouting** | Single-layer (single-sided, no vias), 2-, 4- and 6-layer A* autorouter: through vias, alternating layer directions, turn penalties, rip-up and retry passes |
| **3D** | **X-Ray Stack:** a holographic exploded view of every copper layer with an adjustable gap. Copper glows with additive blending and HDR bloom, through vias become light pillars, part bodies are wireframes, and a scan beam sweeps the stack; turntable spin and per-layer toggles. **Assembly:** a realistic board with blue solder mask. Both export to STL/OBJ |
| **Manufacturing** | Gerber RS-274X for every copper layer including inner layers, plus solder mask, silkscreen and outline; Excellon drill, BOM, pick-and-place, SPICE netlist and the verification report; all in one "Export Fabrication Package" step |

## Repository layout

```
Core/                C++17 engine (no dependencies)
  include/sieda/     public headers; sieda_c.h is the stable C ABI used by Swift
  src/               schematic, simulator, PCB/autorouter/DRC, mesh, exports, JSON
  tests/             unit tests (C++ and a C-compiled ABI smoke test)
  cli/               sieda-cli: headless ERC → simulation → place & route → DRC → verification → fabrication files
SiEDA/               macOS SwiftUI app
  App/               app entry point, menus, DesignStore (state, undo, documents)
  Bridge/            bridging header + EDAEngine (thread-safe Swift façade over the C ABI)
  Models/            snapshot models, ComponentKind, DesignPlan (the agents' structured output)
  AI/                providers (Claude, OpenAI, Gemini, Ollama, Offline), prompts, orchestrator, settings
  Views/             Prompt Studio, Schematic, PCB, 3D, Simulation, Checks, Inspector, Settings
SiEDATests/          XCTest suite (bridge, plan compiler, offline templates)
SiEDA.xcodeproj      generated by tools/generate_xcodeproj.py
docs/                PRD and architecture
```

## Getting started

### macOS app

Requirements: macOS 14 Sonoma or later and Xcode 16 or later.

```bash
open SiEDA.xcodeproj        # then Run (⌘R)
```

1. Open **Settings → AI Models**, keep **Anthropic Claude** selected and paste an API key. It is stored in
   the macOS Keychain. You can switch to OpenAI, Gemini, Ollama or the Offline Designer at any time from
   the same screen or the toolbar menu.
2. In **AI Prompt Studio**, describe the product or pick a template, then press **Generate Design** (⌘↩).
3. Inspect the result in **Schematic** (⌘2), **PCB Layout** (⌘4), **3D Viewer** (⌘5) and **Simulation** (⌘6).
   Import datasheets in **Component Library** (⌘3). Choose 1, 2, 4 or 6 copper layers in the PCB options bar.
4. Choose **Design → Verify Design** (⇧⌘V) and fix anything the report flags, then **File → Export Fabrication Package…** (⇧⌘E).

Command-line build and test:

```bash
xcodebuild -project SiEDA.xcodeproj -scheme SiEDA -destination 'platform=macOS' test
```

### C++ core (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sieda-cli --demo out/     # full pipeline on a demo design, writes Gerbers/drill/BOM/STL and
                                  # verification_report.md to out/; exits non-zero if verification fails
```

### Regenerating the Xcode project

After adding or removing source files, run:

```bash
python3 tools/generate_xcodeproj.py
```

## Canvas navigation and keyboard shortcuts

The schematic and PCB canvases are built for large designs such as CPU/GPU/NPU motherboards, MCU development boards and daughterboards:

- **Zoom range:** fits a 600 mm backplane in a small window and goes down to 0.4 mm-pitch BGA detail on the PCB (1/50 to 15× on the schematic).
- **Rendering:** only what is on screen is drawn. Pins, labels and designators fade out when zoomed far out, and the grid pitch adapts to the zoom level.
- **Navigator:** a thumbnail of the whole design (toggle with N, the toolbar map button or **View → Show Navigator**). Click or drag in it to move the view.

| Mouse / trackpad | Action |
|---|---|
| Two-finger scroll | Pan |
| Pinch, mouse wheel, or ⌘/⌥/⌃ + scroll | Zoom at the cursor |
| ⇧ + mouse wheel | Pan horizontally |
| Middle- or right-button drag | Pan (any tool) |
| Space + left-button drag | Pan (any tool) |
| Drag on empty canvas | Pan (select tool) |
| ⇧-drag | Marquee select (schematic) |

| Key | Action |
|---|---|
| Arrow keys (⇧ for half a screen) | Pan |
| + / − (or ⌘= / ⌘−) | Zoom in / out at the cursor |
| Home, 0 or ⌘0 | Zoom to fit (home view) |
| ⇧Z or ⌥⌘0 | Zoom to selection |
| Z | Zoom to area: drag a rectangle (a click zooms 2×) |
| N | Show/hide the navigator |
| **View → Zoom Level** / click the zoom percentage | Preset levels from 10 % to 1600 % |
| V / H / W | Select, pan (hand) and wire tools (schematic) |
| G / L | Place ground or net label |
| Space (tap) or R | Rotate the selected component(s) 90° (schematic and PCB; nothing happens if nothing is selected). While placing a part, rotate it before clicking. Holding Space and dragging pans instead |
| F | Flip a footprint to the other side (PCB) |
| ⌫ / Esc | Delete selection / cancel |
| ⇧⌘K / ⇧⌘D / ⇧⌘R | Run ERC, DC operating point, autoroute |
| ⇧⌘L / ⇧⌘V | Validate circuit, verify design |
| ⌘1 … ⌘7 | Switch workspace (⌘1 … ⌘6 when AI assistance is off) |
| ⌥⌘A | Turn AI assistance on/off |

## License

Proprietary. All rights reserved.
