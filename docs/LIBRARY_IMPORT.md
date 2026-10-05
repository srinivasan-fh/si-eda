# Library Import

Library Import turns third-party part libraries into project-library parts. It reads KiCad footprints, KiCad symbol
libraries, Eagle libraries and Altium schematic / PCB libraries, pairs each symbol with its footprint, and checks every part with the same rules as the
Symbol and Footprint Editors. Imported parts behave like any other library part: you can place them, edit them in
both editors and save them with the project.

- [Supported formats](#supported-formats)
- [Using it](#using-it)
- [How symbols pair with footprints](#how-symbols-pair-with-footprints)
- [What is imported, and how](#what-is-imported-and-how)
- [Checks and messages](#checks-and-messages)
- [Altium libraries](#altium-libraries)
- [3D models](#3d-models)
- [Not supported](#not-supported)
- [Parametric search](#parametric-search)
- [C API and data format](#c-api-and-data-format)
- [Code map](#code-map)
- [Tests](#tests)

## Supported formats

| File | Contents | Becomes |
|---|---|---|
| `.kicad_mod` (KiCad 6–9, and KiCad 5 `(module …)` files) | One footprint: pads, drills, layers, rotation, courtyard, fab and silkscreen outlines | A land pattern (package type `CUSTOM`) |
| `.kicad_sym` (KiCad 6–9) | One or many symbols: pins with number, name, electrical type, position and orientation; multi-unit symbols; derived symbols (`extends`) | A part's pins and its symbol layout |
| `.lbr` (Eagle 6–9, Fusion Electronics libraries exported as `.lbr`) | Packages, symbols, device sets with their devices, technologies and pin–pad connects | Complete parts, one per device and technology |
| `.SchLib` (Altium Designer binary schematic library) | Components: pins with designator, name, electrical type, position and orientation; multi-part components; designator prefix, parameters (manufacturer, datasheet) and the PCB footprint models they link | A part's pins and symbol layout, paired with the linked footprint |
| `.PcbLib` (Altium Designer binary PCB library) | Footprints: pads with position, size, shape, drill, layer and rotation; top overlay outline | A land pattern |
| `.wrl` (VRML 2.0 / 97), `.stl` (ASCII or binary), `.obj` | A 3D model | The 3D body of the footprint that names it (see [3D models](#3d-models)) |

You can pick several files at once, or folders. A folder is searched for these file types, so a KiCad `.pretty`
folder or `.kicad_symdir` folder imports as a whole (up to 2000 files, 32 MB each). Choosing a `X.pretty` folder also
brings the models of the `X.3dshapes` folder next to it.

## Using it

1. Open the **Component Library** workspace.
2. Click **Import Library…** and choose files or folders.
3. Review the parts in the sheet. Parts that can be imported are selected. Each row shows the footprint, the pin
   count, the source file and any notes. Parts that cannot be imported say why, in red. The selected row's symbol
   and footprint are previewed on the right.
   - **Choosing a footprint.** For a KiCad or Altium symbol, the **Footprint** menu offers *Automatic* (the pairing
     below), then **Fits every pin**: the imported footprints with a pad for every pin, best first (the footprint the
     symbol names, then one pad per pin, then those its footprint filters accept, then the fewest extra pads), then
     the **Other footprints**. Choosing one reads the files again with that pair; a footprint that does not fit (for
     example overlapping pads of different pins) shows why on the row. Your selection of rows is kept.
4. Click **Add Selected Parts**. The parts join the project library in one step; **Edit → Undo** removes them again.

To fix a part after the import, select it in the library and use **Edit Symbol…** or **Edit Footprint…**.

## How symbols pair with footprints

A KiCad symbol is paired with an imported footprint by the first rule that applies:

1. **Explicit pair.** The footprint you choose for the symbol in the import sheet (the core API's `pairs`, symbol
   name → footprint name).
2. **The footprint the symbol names.** For example, `Package_SO:SOIC-8_3.9x4.9mm_P1.27mm` matches an imported
   footprint named `SOIC-8_3.9x4.9mm_P1.27mm`. The library prefix and case are ignored.
3. **The symbol's footprint filters** (`ki_fp_filters`, such as `SOIC*3.9x4.9mm*P1.27mm*`). The first imported
   footprint that matches a filter and has a pad for every pin is used. When several match, a note names the one
   chosen.
4. **A single footprint.** When the import has exactly one KiCad footprint and it has a pad for every pin of the
   symbol, the two are paired. This is the usual vendor or SnapEDA download of one symbol and one footprint.

When none applies, the package is **generated** from the package name in the symbol's footprint field if SiEDA
knows it (`SOIC-16_3.9x9.9mm_P1.27mm` → SOIC-16 at 1.27 mm, `LQFP-48_7x7mm_P0.5mm` → LQFP-48, `SOT-223-3_TabPin2`
→ SOT-223). A note asks you to check it against the datasheet. Otherwise the part is listed as not importable, with
the footprint to add.

Eagle device sets already say which package each device uses and which pad each pin connects to, so Eagle parts
need no pairing.

**Derived symbols.** A KiCad symbol that `extends` another one takes the base symbol's pins, layout and missing
properties. The base can be in another imported file. KiCad 8 and later keep every symbol of a `.kicad_symdir`
in its own file, so import the base symbol's file too (or the whole folder).

Footprints that no symbol uses become parts of their own, with one passive pin per pad number (connectors get the
reference prefix `J`).

## What is imported, and how

**Footprints**

- **Copper pads.** SMD, through-hole and `connect` pads on any copper layer.
- **Pad shapes.** Rectangles and rounded rectangles are imported as rectangles; trapezoids too. Circles and ovals
  are imported round. Custom pads are imported as the bounding box of their primitives.
- **Rotation.** Pads at 90° or 270° swap width and height; any other angle uses the bounding box.
- **Drills.** The drill size is kept. A slotted drill becomes a round hole of the slot's width. If the drill is as
  large as the pad, the pad is enlarged to leave a 0.1 mm ring.
- **Pad numbers.**
  - Each pad keeps its number.
  - Pads that share a number, such as an exposed pad and its thermal vias, all belong to that pin.
  - Unnumbered pads, and pads whose number is not a symbol pin, become mechanical pads.
  - Two pads of different numbers on the same copper (USB-C A1 / B12, both GND) are joined into one pin when the
    pins have the same name. A footprint imported on its own always joins them. A note lists the joined pins.
- **Origin.** The land pattern is centred on the courtyard (`F.CrtYd`). Without a courtyard, it is centred on the
  pads and body. This is because SiEDA places parts by their centre, while connector footprints often have pin 1
  at the origin.
- **Body.** The body is the `F.Fab` outline; without one, the courtyard less its 0.25 mm margin, else the silkscreen.
  For Eagle, the body comes from `tDocu` (layer 51), else `tPlace` (21).
- **Coordinates.** Eagle's y axis points up; it is flipped so that the import matches what you see in Eagle.

**Symbols**

- **Pin numbers.** Each pin number appears once. A number repeated in another unit or in a stack keeps its first
  appearance.
- **Electrical types.** `input`, `output`, `bidirectional`, `power_in`, `power_out`, `open_collector`, `passive` and
  `no_connect` map directly. `tri_state` becomes bidirectional, `open_emitter` open collector, and `free` /
  `unspecified` passive. Eagle `in`, `out`, `io`, `hiz`, `oc`, `pwr`, `sup`, `pas` and `nc` map the same way.
- **Repeated supplies.** A supply pin repeated as `passive` (KiCad's stacked duplicates) takes the supply's type.
- **Names.** Overbars become an `n` prefix: KiCad `~{RESET}` and Eagle `!RESET` are `nRESET`. Eagle's `GND@2`
  suffixes are removed.
- **Layout.** Each pin goes on the side it points from: left, right, top or bottom. Pins keep their order and
  spacing along each side, one slot per 2.54 mm. Pins at the same position with the same name are stacked.
- **Units.** Multi-unit symbols (op-amps, logic gates) and Eagle gates are drawn as **one symbol**: the units are
  placed one after another on each side, separated by a gap. SiEDA has no multi-unit symbols. The De Morgan
  alternative body style is skipped.
- **Hidden pins.** Hidden pins go on the top (supplies), on the bottom (grounds) or on the right.
- **Properties.** The reference prefix, value, datasheet, description and manufacturer are kept. For Eagle, the
  prefix and the device set's description are kept.

**Eagle names**

Eagle builds a device's name from the device set name, the technology and the variant. In the set name, `*` is
replaced by the technology and `?` by the variant; when there is no `?`, the variant name is appended. For example:

- set `BC*`, technologies `547B` and `548C` → **BC547B** and **BC548C**;
- set `LM358`, device `D` → **LM358D**.

## Checks and messages

Every part is checked before it is offered:

- **Symbol layout.** The layout must pass `checkSymbol`. If the library's layout cannot be kept (for example, two
  different pins at one spot), the pins are auto-arranged and a note says why. Stacked signal pins are reported.
- **Land pattern.** The land pattern must pass `checkLandPattern` with no errors: no overlapping pads of different
  pins, and every pin has a pad. Gaps below 0.1 mm and thin annular rings are reported as notes.
- **Registration.** The part must register, with the same limits as any custom part (512 pins, 512 pads, pads
  within 60 mm and 0.05–30 mm in size).

| Note | Meaning |
|---|---|
| Origin moved to the centre of the footprint (by x, y mm) | The pads were shifted to centre the part |
| N non-plated hole(s) not imported | Mounting / locating holes. Add them on the board as mounting holes |
| N paste-only aperture(s) without copper skipped | Paste windows on an exposed pad |
| N bottom-side copy(ies) of a top pad (heat spreader) not imported | A pad repeated on `B.Cu` under an exposed pad |
| N bottom-side pad(s) imported on the top side | SiEDA places parts on the top side |
| N pad(s) at an angle imported as their bounding box | Pads at angles other than multiples of 90° |
| N slotted drill(s) imported as round holes of the slot width | Oval drills |
| N custom-shaped pad(s) imported as their bounding box | KiCad custom pads |
| Pads … are on no symbol pin: imported as mechanical pads | Shield tabs, unused pads |
| Pins sharing one pad were joined (B12 into A1, …) | Overlapping pads of same-named pins |
| The symbol's N units are drawn as one symbol | Multi-unit symbol |
| Footprint X generated from its package name | No footprint file; check the generated footprint |

**Files that cannot be read** are listed at the top of the sheet. The message names the line where reading
stopped, for example `line 412: missing ')'` or `</drawing> closes <library> opened on line 3`. The other files
still import. Malformed or hostile files cannot crash the app:

- nesting is limited to 200 levels;
- files are limited to 32 MB;
- every number is range-checked;
- text is cleaned to valid UTF-8.

## Altium libraries

Altium `.SchLib` and `.PcbLib` files are OLE compound files (the container of old Office documents). SiEDA reads the
container itself and then the Altium records inside:

- **Symbols** (`.SchLib`, one storage per component): the component record (name, description, part count), pins in
  both the text and the binary record forms (designator, name, electrical type, location, length, orientation,
  hidden flag, owner part), the designator (`U?` → prefix `U`), parameters (`Manufacturer`, `Datasheet`, …) and the
  PCB footprint models (`MODELNAME`, the current one first). Overbars (`R\E\S\E\T\`) become `nRESET`; pins of
  alternate display modes (De Morgan) are skipped; multi-part components are drawn as one symbol like KiCad units.
  Electrical types: input, I/O, output, open collector, passive, hi-Z (bidirectional), open emitter (open
  collector), power (power in).
- **Footprints** (`.PcbLib`, one storage per footprint): pads on the top, bottom or all layers, with position, top
  size, round / rectangular / octagonal / rounded shape, drill, plating and rotation (right angles swap the sides,
  other angles take the bounding box). Non-plated holes are skipped with a note; tracks on the top overlay give the
  body outline. Arcs, texts, fills, regions, vias and 3D bodies are skipped; an unknown primitive stops the reading of
  that footprint with a note.
- **Pairing.** A symbol pairs with the footprint its current model names, like a KiCad symbol with its footprint
  field; import the `.SchLib` and the `.PcbLib` together. Without the footprint, the package is generated from the
  model name when SiEDA knows it (`DIP8`, `SOIC8`, `SOT223`…).
- **Not read:** integrated libraries (`.IntLib`, which keep compressed copies of their libraries: use Altium's
  *Extract Sources*, or KiCad 8, and import the `.SchLib` / `.PcbLib`), ASCII-format libraries, database libraries,
  symbol graphics other than pins, pad stacks with different sizes per layer (the top size is used), and footprint
  3D bodies (attach a model with **3D Model…**).
- **Robustness.** The compound file's header, DIFAT, FAT, mini FAT, mini stream and directory tree are checked:
  sector numbers in range, chains without loops, sizes within the file, at most 65 536 directory entries. A damaged
  file is reported ("Altium library: the compound file is damaged (a sector chain is broken)") and the other files
  still import.

**Provenance of the tests.** No Altium-made library can be redistributed with the tests, so
`Core/tests/fixtures/altium/Test.SchLib`, `Test.PcbLib` and `Test.IntLib` are written by
`tools/make_altium_fixtures.py`: an MS-CFB version 3 writer (512-byte sectors, mini stream, FAT / mini FAT, directory
tree) and the Altium record layouts as published by the reverse-engineering in KiCad's open-source Altium importer.
The containers are cross-checked with the independent `olefile` reader when it is installed. The records follow that
published layout; libraries saved by every Altium version could not be tested here.

## 3D models

A part can carry an imported 3D model instead of SiEDA's generated body: VRML 2.0 / 97 (`.wrl`, what KiCad's
libraries ship next to every STEP model), STL (ASCII or binary) or Wavefront OBJ. The model is drawn in the 3D view,
included in the STL / OBJ exports and the fabrication package's `3d/` STL, and saved with the project.

**With a KiCad import.** A KiCad footprint names its model, for example
`${KICAD8_3DMODEL_DIR}/Package_SO.3dshapes/SOIC-8_3.9x4.9mm_P1.27mm.wrl`, with an offset, scale and rotation. When a
model file of that name (`.wrl`, `.stl` or `.obj`; a `.step` reference takes the file of the same name) is in the
import, the part gets it, with the footprint's offset, scale and rotation, moved with the pads when the land pattern
is centred, and a note "3D model … attached". When it is not, a note says to add the library's `.3dshapes` folder.
KiCad VRML files are in 0.1 inch units; STL and OBJ are taken as millimetres.

**By hand.** Select a part in the Component Library and click **3D Model…**:

1. **Choose File…** reads a `.wrl`, `.stl` or `.obj` file and seats it on the footprint.
2. Align it: **File unit** (mm, 0.1 inch, inch, mil, m), **Offset** (mm), **Rotation** (degrees about x, then y, then
   z) and **Scale**. Axes: x right, y towards the top of the PCB view, z up from the board. The preview shows the
   part on a piece of board with its pads; **Seat on Board** centres the model on the footprint and puts its lowest
   point on the board surface.
3. **Apply**, then save the part. **Remove Model** goes back to the generated body.

**What is read.** VRML: `Transform` (translation, rotation, scale, centre, scale orientation), `Group`, `Switch`,
`LOD`, `Shape` with `IndexedFaceSet` (polygons are fanned into triangles, `ccw FALSE` and mirroring transforms are
handled) and `Box`, `Material` diffuse colour and transparency, `DEF` / `USE`. Spheres, cones, cylinders, `Inline`,
`PROTO`s and textures are skipped with a note. STL: identical vertices are shared. OBJ: polygons, `v/vt/vn` and
negative indices; colours are guessed from `usemtl` names (`.mtl` files are not read). Each colour becomes a
material in the 3D view: bright greys render as tinned metal, gold as gold, transparent parts as glass, the rest as
moulded plastic.

**Limits.** 32 MB per file, 200 000 triangles and 600 000 vertices per model, coordinates within ±10⁶ units, VRML
nesting to 64 levels. A file beyond them is refused with the reason and the line where reading stopped.

**STEP is not read.** STEP (`.step` / `.stp`) files describe exact surfaces (B-rep, NURBS) that need a CAD geometry
kernel to tessellate, which SiEDA does not include. They are recognised and refused with that reason; use the `.wrl`
KiCad ships beside each `.step`, or export VRML / STL / OBJ from the CAD tool.

## Not supported

- **Altium integrated libraries** (`.IntLib`) and ASCII Altium libraries are refused with a message (see
  [Altium libraries](#altium-libraries)); binary `.SchLib` and `.PcbLib` are read.
- **KiCad 5 `.lib` symbol libraries** (`EESchema-LIBRARY`) are refused; open them in KiCad 6 or later and save them
  as `.kicad_sym`. KiCad 5 footprints (`.kicad_mod` with `(module …)`) are supported.
- **STEP 3D models** (see [3D models](#3d-models)); VRML, STL and OBJ models are imported.
- **Other graphics.** Silkscreen and fab graphics are used only for the body outline; text and other drawings are not
  imported.
- **Paste and mask.** Paste and solder-mask expansions are SiEDA's own; per-pad overrides are ignored.
- **Pad shapes.** Round-rectangle corner radii and chamfers are imported as plain rectangles.
- **Units and body styles.** Multi-unit symbols become one symbol, and De Morgan alternatives are dropped.

## Parametric search

The schematic device picker (**DEVICES**) and the Component Library search field accept filters as well as words:

| Term | Matches |
|---|---|
| `cat:sensors` | category contains "sensors" |
| `pkg:soic` / `pkg:sot-23` | package type and pin count contain it (separators ignored: `sot-23` = `SOT23`) |
| `pins:8`, `pins:6-10`, `pins:>40`, `pins:<8` | pin count exactly, in a range, above or below |
| `mfr:ti` | manufacturer contains "ti" |
| any other word | appears in the name, description, category, manufacturer or package |

All terms must match. Examples:

- `cat:transistors pkg:sot23 mosfet` lists the SOT-23 MOSFETs;
- `mfr:espressif pins:>40` lists the ESP32-S3 and ESP32-PICO-D4;
- `cat:regulators pkg:sot223` lists the SOT-223 regulators (AMS1117, LD1117, LM1117, AP7361C).

## C API and data format

```c
/* request: {"files":[{"name":"LM358.kicad_sym","content":"<file text>"}, ...],
 *           "pairs":{"<symbol name>":"<footprint name>"}}   (pairs optional) */
char* sieda_library_import(const char* request_json);   /* caller frees with sieda_string_free */
```

The result:

```json
{
  "parts": [
    {"name": "LM358", "symbol": "LM358", "footprint": "SOIC-8_3.9x4.9mm_P1.27mm",
     "source": "LM358.kicad_sym, SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod",
     "ok": true, "error": "", "warnings": ["The symbol's 3 units are drawn as one symbol, …"],
     "pairable": true, "candidates": ["SOIC-8_3.9x4.9mm_P1.27mm"],
     "spec": { "name": "LM358", "pins": [...], "package": {"type": "CUSTOM", "lands": [...]},
               "symbolLayout": {"pins": [...]} }}
  ],
  "files": [{"name": "LM358.kicad_sym", "format": "kicad_sym", "symbols": 1, "footprints": 0, "error": ""}],
  "footprintList": [{"name": "SOIC-8_3.9x4.9mm_P1.27mm", "source": "SOIC-8_3.9x4.9mm_P1.27mm.kicad_mod", "pads": 8}],
  "symbols": 1, "footprints": 1
}
```

Files may carry `"contentBase64"` instead of `"content"` (binary STL, Altium libraries). A part with a 3D model has
`spec.model3d = {"id","name","unit","scale":[x,y,z],"rotate":[x,y,z],"offset":[x,y,z]}`.

```c
char* sieda_model3d_import(const char* request_json);  /* {"name","content"|"contentBase64"} → {"ok","id",…} */
char* sieda_model3d_fit(const char* spec_json);        /* aligned bounds and the seated alignment */
SiedaMesh* sieda_model3d_preview(const char* spec_json); /* the part alone on a piece of board */
```

`spec` is a normal custom-part spec (see [FOOTPRINT_EDITOR.md](FOOTPRINT_EDITOR.md#data-format) and
[SYMBOL_EDITOR.md](SYMBOL_EDITOR.md#data-format)). Register an `ok` part with `sieda_custom_part_register`. The call
never fails on file content: unreadable files are reported in `files[].error`, and an invalid request returns an
empty `parts` list with the reason.

## Code map

| Layer | File | What |
|---|---|---|
| Core | `Core/include/sieda/LibraryImport.hpp` | `ImportedFootprint`, `ImportedSymbol`, `ImportedPart`, `LibraryImport`, parsers, `makeImportedPart`, `importLibraryFiles` |
| Core | `Core/src/LibraryImport.cpp` | s-expression and XML readers, KiCad / Eagle mapping, symbol layout from pin positions, pairing, pin merging, validation |
| Core | `Core/src/CustomParts.cpp` | `packageTypeFromName` (generated package from a footprint name) |
| C ABI | `Core/include/sieda/sieda_c.h` | `sieda_library_import` |
| Bridge | `SiEDA/Bridge/EDAEngine.swift` | `EDAEngine.importLibrary(files:pairs:)` |
| Model | `SiEDA/Models/CustomParts.swift` | `LibraryImportFile`, `LibraryImportResult` |
| Model | `SiEDA/Models/Standards.swift` | `PartQuery` (parametric search) |
| Store | `SiEDA/App/DesignStore.swift` | `importLibraryParts(_:)` (one undo step) |
| View | `SiEDA/Views/Library/ComponentLibraryView.swift` | **Import Library…**, file and folder selection (`libraryFiles(at:)`) |
| View | `SiEDA/Views/Library/LibraryImportView.swift` | the review sheet |
| Core | `Core/include/sieda/AltiumLibrary.hpp`, `Core/src/AltiumLibrary.cpp` | `CompoundFile` (MS-CFB reader), `readAltiumSchLib`, `readAltiumPcbLib`; the conversion to symbols and land patterns is `importAltium` in `LibraryImport.cpp` |
| Tool | `tools/make_altium_fixtures.py` | writes the synthetic Altium test libraries |
| Core | `Core/include/sieda/Model3D.hpp`, `Core/src/Model3D.cpp` | VRML / STL / OBJ readers, mesh registry, alignment, project `models3d`, `appendModel3D` (used by `buildAssemblyMesh`) |
| Bridge | `SiEDA/Bridge/EDAEngine+Model3D.swift` | `importModel3D`, `fitModel3D`, `model3DPreviewMesh` |
| View | `SiEDA/Views/Library/Model3DEditorView.swift` | **3D Model…** sheet with the alignment preview |

## Tests

- **Core** (`Core/tests/core_tests.cpp`), with fixture files in `Core/tests/fixtures/library/`:
  - `library_import_kicad_footprints`:
    - KiCad 8 SOIC-8;
    - QFN-16 with rotated pads, exposed pad, thermal vias and paste apertures;
    - KiCad 5 pin header with pin 1 at the origin and a mounting hole;
    - slots, oblique, custom and bottom pads;
    - error messages with line numbers, and the limits.
  - `library_import_kicad_symbols_and_pairing`:
    - a multi-unit op-amp, a derived symbol, a regulator with a generated package, and an MCU with hidden and
      stacked pins on the QFN;
    - pairing by name, by footprint filters, by explicit pairs and by the single-footprint rule;
    - base symbols in other files;
    - USB-C pin joining;
    - layout fallback;
    - JSON round trip.
  - `library_import_eagle_libraries`: device sets with gates, technologies, multi-pad connects, long pads, y
    flipping, description HTML, and malformed XML.
  - `library_import_rejects_unsupported_formats_and_survives_fuzzing`:
    - ASCII / damaged Altium files and KiCad 5 libraries are refused;
    - deep nesting;
    - content sniffing;
    - 800 deterministic mutations of the fixtures (truncation, byte flips, deleted, duplicated and inserted
      ranges): the import never throws, and every part it offers registers.
  - `library_import_footprint_candidates_for_the_sheet`: candidate ranking, re-pairing, a choice that does not fit,
    no choice for Eagle devices.
  - `library_import_c_api` (`Core/tests/c_api_test.c`): the C entry point, bad files and bad requests.
  - `altium_compound_file_reader`, `library_import_reads_altium_libraries`, `library_import_altium_survives_fuzzing`
    (800 mutations of the container and records): the CFB reader (mini stream and regular sectors, case-insensitive
    names, truncation and looping chains), the SchLib / PcbLib records, pairing, explicit pairs, `.IntLib` refusal and
    base64 transport.
  - `model3d_readers_vrml_stl_obj`, `model3d_alignment_assembly_and_project_file`,
    `library_import_attaches_kicad_3d_models`, `model3d_readers_survive_fuzzing` (1200 mutations): the readers and
    their limits, alignment, the model in the assembly on both sides, project save / load, KiCad model attachment,
    base64 transport, STEP refusal. The VRML fixture `Core/tests/fixtures/models/SOIC-8_3.9x4.9mm_P1.27mm.wrl` is
    synthetic, written in the layout of KiCad's models; STL / OBJ cubes are generated by the tests.
- **App** (`SiEDATests/SiEDATests.swift`):
  - `LibraryImportTests`: import through the engine, adding to the library, and finding files in folders.
  - `PartQueryTests`: filter parsing and the search in the standard library.

The importer was also run against current KiCad library files: SOIC, QFN with thermal vias, LQFP, BGA-256, SOT-23,
SOT-223, TO-220, JST, ESP32-WROOM and two USB-C receptacles, and the LM358, STM32F103, AMS1117, CH340G, 74HC00,
ESP32-WROOM-32, AO3400A and USB-C symbols. All of them import.
