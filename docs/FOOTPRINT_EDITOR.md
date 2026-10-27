<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# Footprint Editor

The Footprint Editor draws a part's land pattern pad by pad: the copper pads, their drills and which pin each one
connects to. It works on every part in the library. Built-in footprints (SOIC, QFN, DIP, TO-263, BGA, …) are first
converted to an editable land pattern with the same pads on the same pins.

- [Using the editor](#using-the-editor)
- [How pads map to pins](#how-pads-map-to-pins)
- [Checks](#checks)
- [Converting a generated footprint](#converting-a-generated-footprint)
- [Data format](#data-format)
- [Code map](#code-map)
- [Tests](#tests)

## Using the editor

1. Open the **Component Library** workspace.
2. Select a part: one in the project library, or a standard part (it opens as a draft).
3. Click **Edit Footprint…** (next to the package picker). The editor opens as a sheet.
4. Edit the pads, then click **Apply Footprint**. The part's package becomes the edited land pattern.
5. Click **Add to Library** (or **Save Changes** for a part already in the library; **Save & Place** also places
   it) to save the part. Components already placed with it switch to the new
   footprint, keeping their position on the board. **Edit → Undo** restores the previous footprint.

**Cancel** (Esc) closes the editor without changing the part.

### Canvas

| Action | Result |
|---|---|
| Click a pad | Select it |
| ⇧-click or ⌘-click a pad | Add it to, or remove it from, the selection |
| Click empty space | Clear the selection |
| Drag a selected pad | Move the selection; each pad snaps to the grid on release |
| Double-click empty space | Add a pad there (a copy of the last pad's shape) |
| ⌫ | Delete the selected pads |

The canvas shows the grid (it hides lines closer than 6 points and shows every tenth line instead), the origin
axes, the body outline, the courtyard (dashed, pads and body plus 0.25 mm) and the pin-1 dot. Pads are drawn in pad
gold, mechanical pads in grey, drills as holes. A pad that belongs to another pin is labelled `5→EP`. Pads named in
a check finding are outlined in red; selected pads in white.

### Toolbar

| Control | Does |
|---|---|
| **Add Pad** | Adds a pad below the last one, shaped like it, numbered next |
| **Duplicate** | Copies the selected pads ten grid steps to the right, numbered after the last pad |
| **Delete** | Removes the selected pads; later pads renumber down |
| **Mirror** | Flips the selected pads left ↔ right about the origin |
| **Centre** | Moves all pads so the centre of their extent is the origin |
| **Undo / Redo** | Every edit is one step; the editor keeps the last 200 |
| **Grid** | 0.01, 0.025, 0.05, 0.1, 0.25, 0.5, 0.635, 1, 1.27 or 2.54 mm |
| Zoom − / fit / + | 0.25× to 16× of the fitted view |

### Side panel

- **Pad properties** for a single selected pad:
  - X and Y, width and height in mm. Steppers move by one grid step.
  - **Drill:** 0 is an SMD pad; anything above 0 makes a plated through-hole. The drill must stay smaller than the
    pad.
  - **Shape:** rectangle, or circle / oval.
  - **Pin:** its own number, any other pin of the part, or **None (mechanical)**.
  - **Number:** moves the pad in the numbering; the pads in between shift by one.
- **Pad array:** adds 1–128 copies of the selected pad at a pitch, down or to the right. Use it for a connector
  row or one side of an SOIC.
- **Body:** width (x) and length (y), 0.5–60 mm.
- **Checks:** the live findings. Click one to select its pads.
- **Pads:** every pad with its number, pin name, position and size. Click a row to select the pad.

The footer shows the pad and pin counts and the number of errors. **Apply Footprint** is disabled while there are
errors or no pads.

## Pad options

The pad panel also sets, per pad:

- **Shape**: Rectangle, **Rounded** (corner radius as a share of the short side, 5–50 %; a rounded-rectangle aperture
  macro in the Gerbers), Circle / Oval, **Chamfered** (45° corners, 5–50 % of the short side) or **Custom Outline**.
  A custom outline starts as a trapezoid; type its corners round the pad centre (`x, y; x, y; …`, mm, y down).
  Chamfered and custom pads go to the Gerbers as filled regions (copper, mask grown by its margin, paste), and to the
  3D view as their own shape.
- **Inner Ø** (a pad with a drill): the pad stack — its diameter on the inner copper layers (0 = its own size).
- **Solder-side Ø** (a pad with a drill): its diameter on the side opposite the part (copper and mask opening; 0 = its
  own size; land option `solder`).
- **Non-plated hole** (a pad with a drill): no copper and no net; the hole goes in the NPTH drill file.
- **Mask**: the solder mask opening margin (board default 0.05 mm), in the Gerbers, IPC-2581 and ODB++.
- **Paste**: automatic (80 % per side on pads of 2.5 mm and more), none, or 50–100 % of the pad, split into up to
  4 × 4 windows for a large thermal pad.
- **Add Thermal Vias**: 0.3 mm plated vias on a 1 mm grid inside the pad, on its pin.
- **Rotate** (toolbar): the selected pads turn 90° about their centre.

## Silkscreen and fabrication graphics

The **Graphics** section of the side panel draws lines into the footprint:

- **Body on Silk** / **Body on Fab**: the body's outline as four lines, on the silkscreen (0.1 mm outside the body)
  or on the fabrication (assembly) drawing.
- **Add Line**, then type its ends (x0, y0, x1, y1 in mm); **Fab** moves it to the fabrication drawing.
- **Add Pin-1 Mark**: a short silk bar beside pad 1 on its inner side (0.3 mm clear of the pad).

Silk lines go to the silkscreen Gerber, the 3D view and the board's Top / Bottom Overlay. Fab lines go to the assembly
drawings and the board's Fabrication layer.

## Layout tools

- **Units**: the board display unit (mm, cm, mil, in, ft or pt; the toolbar menu and Settings → Board Units set the same
  one, also used by the PCB editor and the inspector); the cursor position shows at the top left of the canvas.
- **Arrange**: origin at pad 1 or at the selected pad; align left / right / top / bottom / centres; distribute.
- **Pad array**: a row (copies and pitch) or a grid (rows and row pitch), numbered along the chosen direction.
- **Body → Courtyard**: the courtyard margin around pads and body (IPC-7351 nominal 0.25 mm; package JSON
  `courtyard`, written only when it differs).
- **Generate**: a standard package's pads (SOIC, TSSOP, SON, QFN, LQFP, BGA, SOT-23, SOT-223, DIP, headers, TO-220)
  from pins, pitch and body size; it replaces the pads (undo brings them back).
- **IPC-7351 calculator** (two-sided gull-wing parts): from the datasheet's toe-to-toe span L, heel-to-heel span S and
  lead width W (min / max), the density level's toe / heel / side fillets (most 0.55 / 0.45 / 0.05, nominal
  0.35 / 0.35 / 0.03, least 0.15 / 0.25 / 0.01 mm) and tolerances F 0.05, P 0.025 mm: Z = Lmin + 2·toe + RMS,
  G = Smax − 2·heel − RMS, X = Wmin + 2·side + RMS; the pad is (Z − G) / 2 × X with rows (Z + G) / 2 apart.
  Apply to Selected Pads sizes and places them.

- **Layers** (toolbar): show or hide copper, the solder mask and paste openings (dashed / thin outlines, hidden at
  first), silkscreen, fabrication, courtyard and body.
- **Select** (toolbar): the selection filter — all pads, SMD, through-hole or mechanical (no pin or a plain hole); a
  click picks only those, and **Select All of These** takes them all.
- **Library → Save to My Footprints**: the pads, body, courtyard and graphics as one JSON file in
  `~/Library/Application Support/SiEDA/Footprints` (`UserFootprintLibrary`); **My Footprints** at the top of the
  Library popover loads one (undoable) or removes it.

### IPC-7351 calculator and library footprints

The core's IPC-7351B calculator (MCP `library_land_calculator`, Studio's IPC calculator) gives pad length, width and
centre distance from the datasheet's lead dimensions: toe-to-toe span L, heel-to-heel span S and lead width W, min and
max, for gull-wing, chip and flat no-lead parts at most / nominal / least density, and lays out the two rows as lands.
MCP `library_footprints` searches the built-in and project footprints and returns one's pads to start from.

## Reviewing

- ⌘Z / ⇧⌘Z undo and redo footprint edits; **Datasheet** opens the part's datasheet link.
- Several selected pads share one width / height / drill field.
- The pad list filters by pad number, pin number or pin name, and its header counts the pads added (+), removed (−),
  moved (↔) and resized or reshaped (⤢) since the editor opened.

## How pads map to pins

Pads are numbered 1…N in order. A pad connects to:

- the pin whose number is the pad's own number (the default);
- otherwise the pin named in its **Pin** field. Use this for:
  - a TO-263 / SOT-223 tab on the middle lead's pin;
  - an exposed pad on pin `EP`;
  - several pads on one ground pin;
  - BGA balls (`A1`, `B12`, …);
- or no pin, when **Pin** is `-` (mounting holes, fiducial-like copper, mechanical tabs).

Pin references ignore case (`ep` is pin `EP`). Every pin of the part needs at least one pad.

## Checks

The core checks the land pattern on every edit (`checkLandPattern`), with a minimum gap of 0.1 mm.

| Code | Severity | When |
|---|---|---|
| `LAND_OVERLAP` | error | Copper of two pads overlaps and they belong to different pins (or both are mechanical). Pads of one pin may touch or overlap: a split exposed pad, a tab. |
| `LAND_GAP` | warning | Copper of two pads of different pins is closer than 0.1 mm. Round pads use the circle-to-circle distance; others use the rectangle distance (corner to corner when diagonal). |
| `LAND_ANNULAR` | warning | A plated hole's annular ring, (min(w, h) − drill) / 2, is below 0.1 mm. |
| `LAND_NO_PIN` | warning | A pad's pin is not in the part's pin list. Mark it `-` if it is mechanical. |
| `LAND_NO_PAD` | error | A pin of the part has no pad. |
| `LAND_INVALID` | error | The land pattern cannot be read, e.g. a drill as large as its pad. |
| `LAND_PASTE` | warning / info | A paste opening under 50 % of the pad size starves the joint (warning); a pad of 9 mm² or more under one full window floats the part (note: split it into windows). |
| `LAND_SILK_PAD` | warning | A silkscreen line comes within 0.15 mm of a pad. |
| `LAND_PIN1` | info | The footprint has its own graphics but nothing tells pad 1 apart: no other shape and no silk mark within 1 mm of it. |
| `LAND_NUMBERING` | warning | Pads without ball names are numbered across the footprint (more than 4, or one in 8, steps of over three times the usual pitch). |
| `LAND_COURTYARD` | info | A silk or fab line runs outside the courtyard. |
| `LAND_MASK_SLIVER` | info | The mask openings of two pads overlap (their gap is less than the two margins). |
| `LAND_ZERO_ORIENTATION` | warning | Pad 1 is on the right half. IPC-7351 zero orientation puts pin 1 at the top left, so the pattern was probably drawn from the bottom view or turned around, and pick-and-place rotations come out wrong. SiEDA's SOT-23 / SOT-223 / D2PAK lands use the datasheet view with the leads at the bottom and pad 1 at the bottom left, so the check looks only at left and right. |

The added checks are quiet on every catalog footprint.

Errors block **Apply Footprint**. The core also refuses to register a land-pattern part with a pin that has no pad
or an invalid land, so a broken footprint can never be saved.

## Converting a generated footprint

`landPatternFromFootprint` registers the part, takes its generated footprint and writes one land per pad: position,
size, drill, shape and pin. The result has package type `CUSTOM`, the same body and the same pins, and draws exactly
the same pads on the same pins. Tests cover DIP, SOIC, TO-220, SOT-223, TO-263, LQFP, QFN with an exposed pad,
HTSSOP, SOT-23, RF modules, BGA, crystals, SuperSO8 and LGA parts.

- Pads whose pin number is not their own pad number keep that pin in their **Pin** field: tabs, `EP`, BGA balls.
- Unconnected pads become mechanical (`-`).
- Parts already on a land pattern (`LGA` from the catalog, or `CUSTOM`) open unchanged. A catalog `LGA` part stays
  `LGA` when edited.

## Data format

A land-pattern part is a normal custom-part spec. Its package carries the lands:

```json
{
  "name": "NE555-CUSTOM",
  "pins": [{"number": "1", "name": "GND", "type": "power_in"}, …],
  "package": {
    "type": "CUSTOM",
    "pinCount": 10,
    "bodySize": 7.0,
    "bodyDepth": 10.0,
    "lands": [
      [-3.81, -3.81, 1.6, 1.6, 0.8, 0],
      [-3.81, -1.27, 1.6, 1.6, 0.8, 1],
      …,
      [0, 9, 3, 3, 2.2, 1, "-"],
      [0, 0, 2, 2, 0, 0, "8"],
      [0, 4, 3, 3, 0, 0, "", {"corner": 0.25, "mask": 0.1, "paste": 0.6, "pasteGrid": 2}],
      [3, 4, 2, 2, 0, 0, "", {"chamfer": 0.25}],
      [6, 4, 2, 2, 0, 0, "", {"outline": [[-1, 1], [1, 1], [0.6, -1], [-0.6, -1]]}],
      [6, 0, 1.8, 1.8, 1.0, 1, "", {"inner": 1.0}]
    ],
    "graphics": [[-4, -6, 4, -6, 0.15, "silk"], [-3.5, -5, 3.5, -5, 0.1, "fab"]]
  }
}
```

Each land is one of three forms (mm, y down, pad 1 usually top-left):

| Form | Meaning |
|---|---|
| `[x, y, w, h]` | rectangular SMD pad on its own pin |
| `[x, y, w, h, drill, round]` | `drill` > 0 is a plated through-hole; `round` 1 is a circle / oval |
| `[x, y, w, h, drill, round, "pin"]` | the pad belongs to pin `pin`; `"-"` is mechanical |

Limits:
- Up to 512 lands, each within 60 mm of the origin and 0.05–30 mm in size.
- The drill must be smaller than the pad.
- `bodySize` is the body width (x) and `bodyDepth` its length (y). Without them the body is the pads' extent.
- A pad outline has 3–64 points within 30 mm of the pad centre.
- `graphics`: up to 500 lines `[x0, y0, x1, y1, width, "silk" | "fab"]`, width 0.01–2 mm, written only when there are
  any.

The project file (`.siedaproj`) stores the spec as it is, so the footprint is saved with the project.

## Code map

| Layer | File | What |
|---|---|---|
| Core | `Core/include/sieda/CustomParts.hpp` | `PackageSpec::Land`, `usesLandPattern`, `landPatternFromFootprint`, `LandIssue`, `checkLandPattern` |
| Core | `Core/src/CustomParts.cpp` | land JSON, validation, pad placement for `LGA` / `CUSTOM`, pin mapping, conversion, checks |
| C ABI | `Core/include/sieda/sieda_c.h` | `sieda_custom_part_land_pattern`, `sieda_check_land_pattern` |
| Bridge | `SiEDA/Bridge/EDAEngine.swift` | `EDAEngine.landPattern(_:)`, `EDAEngine.checkLandPattern(_:minGap:)` |
| Model | `SiEDA/Models/CustomParts.swift` | `CustomPartSpec.Land` (encodes like the core) |
| Model | `SiEDA/Models/FootprintDraft.swift` | `FootprintDraft` (pads, numbering, editing operations), `FootprintHistory` (undo), `LandIssue` |
| View | `SiEDA/Views/Library/FootprintEditorView.swift` | the editor sheet: canvas, toolbar, side panel, checks |
| View | `SiEDA/Views/Library/ComponentLibraryView.swift` | **Edit Footprint…**, conversion, applying the result to the draft |

The catalog's exact `LGA` land patterns (BMI088, BMP280, MS5611, VL53L1X, SuperSO8 MOSFETs, …) use the same lands.
`tools/fetch_catalog_parts.py` generates them from the KiCad footprints.

## Tests

- **Core** (`Core/tests/core_tests.cpp`):
  - `footprint_editor_land_patterns`: conversion of 14 parts across every package family, checks, mechanical and
    split pads, JSON round trip, validation.
  - `footprint_editor_checks_geometry_and_c_api`: gap geometry, the overlap rules, pin references, BGA conversion
    and the C API.
  - `exact_land_patterns_for_irregular_packages`: the catalog's `LGA` geometry against KiCad.
- **App** (`SiEDATests/SiEDATests.swift`):
  - `FootprintEditorTests` (12 tests): document editing, undo, land encoding, the checks, conversions, editing a
    placed part through the store, and the editor and library in a live window.
  - `testFootprintEditorConvertsEditsAndPlacesACustomFootprint`: the end-to-end flow.

Run them with:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests
xcodebuild -project SiEDA.xcodeproj -scheme SiEDA test   # macOS
```
