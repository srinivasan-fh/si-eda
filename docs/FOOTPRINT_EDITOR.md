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
      [0, 0, 2, 2, 0, 0, "8"]
    ]
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
