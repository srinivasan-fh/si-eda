# Symbol Editor

The Symbol Editor arranges where a part's pins sit on its schematic symbol: which side of the body, in what order,
with gaps between groups. It can also stack repeated power pins onto one spot. The symbol is what people read on the
schematic. A good arrangement keeps supplies at the top, grounds at the bottom, inputs on the left and outputs on the
right, so wires stay short and the signal flow is obvious.

- [What changes on the schematic](#what-changes-on-the-schematic)
- [Using the editor](#using-the-editor)
- [Auto Arrange](#auto-arrange)
- [Stacked pins](#stacked-pins)
- [Checks](#checks)
- [Library symbols](#library-symbols)
- [Data format](#data-format)
- [Code map](#code-map)
- [Tests](#tests)

## What changes on the schematic

- Pins can leave the body from all four sides: left, right, top and bottom.
- Pin names are drawn inside the body. Names on vertical leads read from bottom to top, at any component rotation.
- Pin numbers are drawn beside the leads. A stack shows all its numbers, e.g. `19,32,48,64`.
- Wires are attached to pins, not to positions. Rearranging a symbol moves the pins, and every wire follows its pin.
  The board, nets and footprint do not change, except that stacked pins join one net (see below).

## Using the editor

1. Open the **Component Library** workspace and select a part (a project part, or a standard part as a draft).
2. Click **Edit Symbol…** (next to **Edit Footprint…**). The editor opens as a sheet, showing the part's current
   layout. A part that was never arranged starts from the plain datasheet-order box.
3. Arrange the pins, then click **Apply Symbol**.
4. Save the part (**Add to Library**, **Save Changes** or **Save & Place**). Components already placed with it
   switch to the new symbol. **Edit → Undo** brings the old one back.

### Canvas

| Action | Result |
|---|---|
| Click a pin (its lead or name) | Select it (with its stack) |
| ⇧-click or ⌘-click a pin | Add it to, or remove it from, the selection |
| Click empty space | Clear the selection |
| Drag selected pins | Move them to the side and slot under the pointer; pins already there shift along to make room |
| ⌥-drop onto a pin | Stack the dragged pins on that pin |

While you drag, a marker shows the target side and slot. Pin names are colour-coded: power pins are cyan, pins named
in a check finding are red and selected pins are white.

### Toolbar

| Control | Does |
|---|---|
| **Stack repeated power pins** | Used by Auto Arrange: put supply / ground pins of the same name on one spot |
| **Auto Arrange** | Lays the symbol out from the pins' names and types (below) |
| **Datasheet Order** | The plain box: pins in number order down the left side, then up the right |
| **Mirror** | Swaps the left and right sides |
| **Close Gaps** | Removes empty slots on every side |
| **Undo / Redo** | Every edit is one step; the editor keeps the last 200 |

### Side panel

- **Selected pins:** their numbers, names and types, and these actions:
  - **Send to** Left / Right / Top / Bottom (appends to that side);
  - **↑ / ↓** move one slot along the side, swapping with the neighbour or moving into a gap;
  - **Gap Before** inserts an empty slot to separate a group;
  - **Stack** and **Unstack**.
- **Body width:** Auto (from the pin names), or a fixed width in steps of 20 units.
- **Checks:** the live findings. Click one to select its pins.
- **Pin list** per side, slot by slot, with gaps shown. Click a row to select it.

The footer shows how many spots the pins use, and the number of errors. **Apply Symbol** stays disabled while there
are errors.

## Units (gates) of a multi-unit part

The **Symbol / Units** switch in the toolbar opens the unit editor (Altium's *Part A / Part B*, KiCad's units). It
writes the part's `units` — no JSON editing.

- **Units list** (left): **Add Unit** (the selected pins go on it), remove, rename (1–8 characters, unique), and the
  unit's **Swap** setting: *Identical gates* (default: gates with the same symbol are interchangeable), *Swap group
  1–4* (interchangeable with the gates of that group only) or *Never swap*. **Detect Gates** splits the pins into
  gates from their names (`1A, 1B, 1Y, 2A …` or `OUT1, IN1-, IN1+, OUT2 …`); supplies stay on the power unit.
  **One Symbol** removes the units.
- **Pin matrix** (centre): one row per pin, one checkbox column per unit. A pin on no unit goes to the power unit
  **P** (⚡), one on several units is **shared** (🔗: one pin, drawn on each gate). With pins selected (⌘/⇧-click):
  **Only on A**, **Shared by All**, **Power Unit**.
- **Right panel**: the selected unit's generated symbol, its **pin-swap groups** (select two or more of its pins →
  **Make Swap Group**, e.g. the inputs of a NAND gate) and the unit checks.
- Checks (core `checkUnits`): unknown pins, a pin twice in one unit, empty or duplicate units, a pin-swap group
  naming a pin that is not on the unit (errors — **Apply** is disabled); signal pins shared by several units,
  signal pins left to the power unit, swap groups whose gates differ, pin-swap groups across pin types (warnings);
  shared supplies and the power unit's pins (info).
- On the schematic: selecting two units of one part shows **Swap Gates** (they exchange gates — package and unit —
  while symbols and wires stay); a unit with pin-swap groups shows **Swap Pins** (the two pins exchange wires).
  **Pack Units into Packages** packs only interchangeable gates.

## Auto Arrange

Auto Arrange (core `autoArrangeSymbol`) sorts the pins by name and electrical type:

| Pins | Placed |
|---|---|
| Grounds and negative supplies (`GND…`, `AGND`, `PGND`, `VSS…`, `VEE`, `V-`, `0V`) | bottom |
| Other supply inputs (`power_in`: `VCC`, `VDD`, `AVDD`, `VBAT`, …) | top |
| Reset, clock, boot and debug (`NRST`, `RESET`, `MCLR`, `XTAL…`, `OSC…`, `BOOT…`, `SWDIO`, `SWCLK`, `TCK`, `TMS`, `TDI`, `TDO`, `UPDI`, `EN`, …) | left, first |
| Inputs | left |
| Outputs, open-drain outputs, regulator outputs | right |
| MCU port pins (`PA0…`, `P1.3`, `P1_3`, `GPIO12`, `IO5`, `RB7`) | grouped by port, in bit order, in groups of 8; each group goes to the shorter side |
| Other bidirectional / passive pins | split across both sides |
| No-connect pins | last on the right |

Each group is separated from the next by an empty slot. With **Stack repeated power pins**, repeated supply and
ground pins of the same name share one spot: an STM32F405's four `VDD` pins become one `VDD` pin numbered
`19,32,48,64`.

## Stacked pins

Pins of one part on the same side and slot are **stacked**. They are drawn as one pin and are electrically one node.
A wire to the stack connects every pin in it, and on the PCB all their pads belong to that net, so the ratsnest and
the autorouter connect them. Stack only pins that must be connected together: repeated supplies and grounds, or an
exposed pad and its ground pins.

A stack that no wire reaches is still **open**. ERC reports it like any unconnected pin, and its pins don't show as
connected.

## Checks

The core checks the layout on every edit (`checkSymbol`):

| Code | Severity | When |
|---|---|---|
| `SYM_MISSING` | error | A pin of the part is not on the symbol |
| `SYM_UNKNOWN` | error | The symbol places a pin number the part does not have |
| `SYM_DUPLICATE` | error | A pin is placed more than once |
| `SYM_OVERLAP` | error | Pins with different names are on the same spot |
| `SYM_STACK_SIGNAL` | warning | Signal (non-power) pins of the same name are stacked; they will be joined |
| `SYM_STACK` | info | Supply / ground pins are stacked and join one net |
| `SYM_INVALID` | error | The layout cannot be read (a side other than L / R / T / B, a negative slot, a missing pin number) |

The core also refuses to register a part whose layout has errors, so a broken symbol can never be saved.

If you later add or remove pins in the pin table, an arranged symbol follows: removed pins leave it and new pins go
to the end of the shorter side.

## Library symbols

Standard-library parts with 16 or more pins are auto-arranged with stacking: MCUs, the FPGA, the RF modules, motor
drivers, interfaces and so on. Smaller parts (NE555, LM358, regulators, transistors) keep the familiar datasheet-order
box. To change a library part's symbol, add it to the project library and edit it there.

## Data format

The layout is part of the custom-part spec, under `symbolLayout`:

```json
{
  "name": "SYM-TEST",
  "pins": [{"number": "1", "name": "VCC", "type": "power_in"}, …],
  "symbolLayout": {
    "width": 120,
    "pins": [
      {"number": "1", "side": "T", "slot": 0},
      {"number": "2", "side": "L", "slot": 0},
      {"number": "6", "side": "L", "slot": 2},
      {"number": "3", "side": "R", "slot": 0},
      {"number": "4", "side": "B", "slot": 0},
      {"number": "5", "side": "B", "slot": 0}
    ]
  }
}
```

- **`side`** is `L`, `R`, `T` or `B`.
- **`slot`** is the position along the side:
  - 0 is the topmost pin on L / R and the leftmost on T / B;
  - slots are 2 grid units (20) apart;
  - a skipped slot is a gap.
- **`width`** is the body width in schematic units; leave it out for automatic.
- A spec without `symbolLayout` uses the generated box, so its part id is unchanged.

**Geometry.** All pin ends land on the 10-unit grid:
- left / right pins are at x = ∓(half width + 20);
- top / bottom pins are at y = ∓(half height + 20), centred along the side.

The body grows to fit the longest pin names and the rows of top and bottom pins. Names on top and bottom pins get
room above and below the side pins.

## Code map

| Layer | File | What |
|---|---|---|
| Core | `Core/include/sieda/CustomParts.hpp` | `SymbolPin`, `SymbolSpec`, `autoArrangeSymbol`, `SymbolIssue`, `checkSymbol` |
| Core | `Core/src/CustomParts.cpp` | layout JSON and validation, four-sided pin placement, auto-arrange, checks |
| Core | `Core/src/Schematic.cpp` | stacked pins joined in the netlist; `isPinConnected` (an unwired stack stays open) |
| Core | `Core/src/StandardParts.cpp` | auto-arranged symbols for library parts with 16+ pins |
| C ABI | `Core/include/sieda/sieda_c.h` | `sieda_symbol_auto_arrange`, `sieda_check_symbol` |
| Bridge | `SiEDA/Bridge/EDAEngine.swift` | `EDAEngine.autoArrangeSymbol(_:stack:)`, `EDAEngine.checkSymbol(_:)` |
| Model | `SiEDA/Models/CustomParts.swift` | `CustomPartSpec.SymbolLayout`; symbol pins carry their side |
| Model | `SiEDA/Models/SymbolDraft.swift` | `SymbolDraft` (move, stack, gaps, swap, nudge, mirror, reconcile), `SymbolIssue` |
| Model | `SiEDA/Models/FootprintDraft.swift` | `EditHistory` (undo / redo shared with the Footprint Editor) |
| View | `SiEDA/Views/Library/SymbolEditorView.swift` | the editor sheet |
| View | `SiEDA/Views/Schematic/SchematicSymbols.swift` | four-sided symbol drawing, `drawPinLabels` (used by the schematic, previews and the editor) |

## Tests

- **Core** (`Core/tests/core_tests.cpp`), `symbol_editor_layout_auto_arrange_and_checks`:
  - the generated box;
  - every arranged library symbol (clean checks, pins on the grid);
  - STM32 stacking and ordering;
  - four-sided geometry and body sizing;
  - each check and JSON validation;
  - stacked pins joining nets, and an unwired stack staying open in ERC;
  - the C API.
- **Core**, `symbol_editor_auto_arrange_rules_geometry_and_board`:
  - every Auto Arrange rule on one part (supplies, grounds, control, inputs, outputs, ports in bit order, NC);
  - ports split into groups of 8, and arranging without stacking;
  - body sizing and four-sided pin positions;
  - stacked pins on a rotated component, placed and routed on the board;
  - project save / load and replacing a placed part's symbol with wires kept.
- **App** (`SiEDATests/SiEDATests.swift`):
  - `SymbolEditorTests` (10 tests): the document operations, history and reconcile; the layout encoding and
    four-sided drawing; checks and auto-arrange; editing a placed part's symbol through the store, with wires kept
    and undo; the editor in a live window.
  - `SymbolEditorDetailTests` (12 tests): the generated box, stack splitting and nudging, no-op edits, gaps,
    reconcile, width and apply, issue decoding, deterministic Auto Arrange, every arranged library part drawn with
    its pins on their sides, project save and reopen, and four-sided symbols in the schematic editor.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests
xcodebuild -project SiEDA.xcodeproj -scheme SiEDA test   # macOS
```
