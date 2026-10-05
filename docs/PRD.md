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
| F7 | DC operating point and transient simulation, waveform charts, live probes | ✅ |
| F8 | PCB: auto-place, manual move/rotate/flip, autoroute, ratsnest, DRC, editable design rules | ✅ |
| F9 | 3D assembly viewer with STL/OBJ export | ✅ |
| F10 | Fabrication package: Gerber RS-274X, Excellon, BOM, pick-and-place, SPICE netlist | ✅ |
| F11 | Project save/open (`.siedaproj`, JSON) | ✅ |
| F12 | Component library: datasheet import, pin-table editor, standard parts catalog, symbol / footprint / 3D generation | ✅ |
| F13 | Footprint Editor: draw any part's land pattern pad by pad (position, size, shape, drill, pin), pad arrays, undo, live checks (overlap, gap, annular ring, pin ↔ pad coverage); converts generated footprints | ✅ |
| F14 | Symbol Editor: pins on any side and slot, gaps, stacked power pins (joined nets), Auto Arrange by name and type, live checks; library parts with 16+ pins arranged | ✅ |

## Non-functional requirements
- **Correctness.** All AI output is schema-validated and checked by the core (ERC, simulation, DRC) before
  it is presented.
- **Performance.** Interactive editing of designs with up to 200 parts. Autorouting typical small boards
  takes under one second.
- **Privacy.** Keys live in the Keychain. Only the prompt and design context are sent to the selected provider.
- **Portability.** The core is dependency-free C++17 and is tested on Linux and macOS in CI.

## Out of scope for v1
- Importing KiCad or Altium library files, multi-unit symbols (one gate per unit) and custom symbol graphics (the
  Footprint and Symbol Editors shipped; see F13, F14).
- AC and noise analysis.

## Roadmap
(Shipped since v1: up to 24 layers, copper pours and planes with thermal reliefs, differential pairs, length tuning,
HDI vias and BGA fan-out.)

1. KiCad `.kicad_mod` / Altium footprint import into the Footprint Editor (user component libraries shipped).
2. AC small-signal analysis and Bode plots.
3. Push-and-shove interactive routing.
4. Team collaboration through a shared project repository.
