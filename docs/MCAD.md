# Mechanical CAD exchange (STEP, IDF)

SiEDA sends the board to mechanical CAD (MCAD) as STEP solids or IDF 3.0 files. It reads MCAD's placement changes
back from IDF.

| Output | Where | What it holds |
|---|---|---|
| STEP AP214 (`board.step`) | **File → Export → STEP AP214 — Board and Parts**, MCP `output_3d_model` `format: "step"`, C `sieda_export(p, "step")` | The board outline extruded to its thickness, with the mounting holes cut. One closed, coloured solid per placed part (its package body), named by designator (`U1`, `R5`). |
| IDF 3.0 board (`board.emn`) | **File → Export → IDF 3.0 Board**, `format: "idf_board"` | Outline and thickness, mounting holes (`NPTH … MTG`), and the placement of every part: package, part number, designator, X / Y, rotation, side. Locked parts are marked `MCAD`. |
| IDF 3.0 library (`board.emp`) | **File → Export → IDF 3.0 Library**, `format: "idf_library"` | One `ELECTRICAL` outline with its height for each package and part-number pair the board uses. |

Coordinates are in MCAD's frame:
- **Units:** millimetres.
- **Axes:** X is the board's X, Y points up (the board's −Y), and Z points up.
- **Origin:** the board's underside is at Z = 0.
- **Parts:** top-side parts sit on Z = thickness; bottom-side parts hang below Z = 0.

## 3D clearance (DRC)

The design rule check also checks the parts in 3D (Errors):

| Code | When |
|---|---|
| `MECH_BODY_COLLISION` | Two fitted parts' bodies overlap on the same side. Always checked; parts marked DNP are left out. |
| `MECH_HEIGHT` | A part is taller than the enclosure allows on its side (**Board Setup → Mechanical → Tallest part, top / bottom**). |
| `MECH_HEIGHT_ZONE` | A part stands in a height zone (a rectangle under a display, a rib or a battery) and is taller than the zone allows. |

How to set the limits:
- **App:** the per-side limits are in **Board Setup → Mechanical**.
- **MCP:** `pcb_mechanical_limits` sets the per-side limits and the zones.
- **C API:** `sieda_pcb_mechanical_limits` / `sieda_pcb_set_mechanical_limits`.

Limits are saved with the board only when set. Heights come from each package body, the same as the STEP solids.

## STEP

The solids are a faceted B-rep: planar faces bounded by polygons, so every MCAD tool reads them without a CAD kernel.
- **Round bodies:** electrolytic cans and other cylinders have 24 sides.
- **Colours:** each solid carries its package colour, the same as in the 3D view.
- **Test:** the core test `step_export_closed_named_solids` checks that every solid is closed and consistently
  oriented. That is, every edge is shared by exactly two faces running in opposite directions.
- **OpenCASCADE check:** the exported files were also read with OpenCASCADE, which reported every solid valid and
  of positive volume.

STEP is export only. Part models are read as VRML / STL / OBJ ([LIBRARY_IMPORT.md](LIBRARY_IMPORT.md#3d-models)).

## Round trip with IDF

1. Export the IDF board and library, and open them in MCAD (SolidWorks CircuitWorks, Creo ECAD-MCAD, NX, Fusion
   via an add-in, FreeCAD's IDF importer).
2. The mechanical engineer moves connectors, switches or mounting parts, and saves the `.emn` file.
3. **File → Import MCAD Placement (IDF)…** moves each part the file names (by designator) to its position, rotation
   and side.

The import changes the design as one Undo step. The status line lists the parts moved, and parts already where the
file puts them are left alone. Over MCP the same import is `pcb_import_idf_placement` (`path`); in C it is
`sieda_import_idf_placement`. Rotation is converted between SiEDA's clockwise board view and IDF's counter-clockwise
frame, so a round trip keeps every angle.

## Incremental exchange with IDX

IDX (ProSTEP iViP EDMD, schema v4.5) is the XML successor of IDF that MCAD tools use for incremental ECAD–MCAD
collaboration (SolidWorks PCB / CircuitWorks, Creo, NX, Altium, Cadence and others).

1. **File → Export → IDX / EDMD Baseline for MCAD (.idx)** writes a baseline (`SendInformation`): the board outline
   extruded to its thickness with the mounting holes cut, and for every placed part a package item (body outline and
   height) and an instance with its designator (`REFDES` property), side (`AssembleToName` TOP / BOTTOM) and 2D
   transformation. Millimetres, MCAD XY (Y up).
2. MCAD moves, rotates or flips parts and sends back a baseline or a change file (`SendChanges`).
3. **File → Import MCAD Changes (IDX)…** applies it by designator (one Undo step). Namespace prefixes may differ
   between tools; the reader matches elements by their local names and accepts numbers written plainly or in a
   `Value` element. Rotations snap to 90°.
4. To send SiEDA's own moves back, MCP `output_idx_changes` (`baseline`, `path`) or C `sieda_export_idx_changes`
   writes a `SendChanges` file with only the parts placed differently from the baseline MCAD already has.

Limits: placement (position, rotation, side) is exchanged both ways; board-outline edits and keep-outs from MCAD are
not read yet, and there is no accept / reject response file. The files are checked for well-formed XML and a
SiEDA round trip in the tests; they have not yet been checked against a commercial MCAD tool.

Code: `Core/src/Mechanical.cpp` (`exportStep`, `exportIdfBoard`, `exportIdfLibrary`, `importIdfPlacement`,
`exportIdx`, `exportIdxChanges`, `importIdxPlacement` with a small namespace-agnostic XML reader).
