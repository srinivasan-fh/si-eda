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

## DXF

**Export → DXF** (and `mechanical/<name>.dxf` in the fabrication package) writes an AutoCAD R12 drawing in millimetres,
Y up, origin at the board's lower-left: layers OUTLINE (outline and cutouts), HOLES, DRILLS, SILK_TOP / SILK_BOTTOM,
FAB, COURTYARD and DIMENSIONS (the board's drawing dimensions: extension lines, dimension line and value; drawn with
the PCB editor's ruler tool, MCP `pcb_set_outline` `dimensions`). **File → Import Board Outline (DXF)…** (MCP `pcb_import_dxf`, C `sieda_pcb_import_dxf`) reads
closed loops of lines, arcs, polylines with bulges and circles — only from outline-like layers (OUTLINE, Edge.Cuts,
BOARD, PROFILE, CUT…) when the file names them; millimetres, or inches by `$INSUNITS`. The largest loop becomes the
outline and the loops inside it cutouts; one undo step.

## 3D clearance (DRC)

The design rule check also checks the parts in 3D (Errors):

| Code | When |
|---|---|
| `MECH_BODY_COLLISION` | Two fitted parts' bodies overlap on the same side, in plan and in height. A part drawn by a 3D model uses the model's bounds and height range, so a body raised on its own leads or standoffs passes over the parts below it. Always checked; parts marked DNP are left out. |
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

**Import of part models.** STEP files of parts (AP203 / AP214, `.step` / `.stp`, as KiCad's libraries ship) are read
by SiEDA's own B-rep tessellator in `Core/src/Model3D.cpp` (`parseStep`) — no CAD kernel: the Part 21 structure is
parsed, assembly placements (`MAPPED_ITEM`, `REPRESENTATION_RELATIONSHIP_WITH_TRANSFORMATION`) are applied, edges are
sampled along lines, circles, ellipses, polylines and B-spline curves (de Boor), and each face's boundary is
triangulated in the parameters of its surface (plane, cylinder, cone, sphere, torus, surface of revolution; B-spline
and other surfaces in their best-fit plane), so neighbouring faces share their boundary samples and closed solids
come back closed. Units follow the file's `LENGTH_UNIT` (mm, inch, m…), colours its `STYLED_ITEM`s. See
[LIBRARY_IMPORT.md](LIBRARY_IMPORT.md#3d-models) for what is approximated. The core test
`step_import_round_trip_of_export` reads the exported board back: every solid closed, with its bounds and volume.

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
   extruded to its thickness with the mounting holes cut, for every placed part a package item (body outline and
   height) and an instance with its designator (`REFDES` property), side (`AssembleToName` TOP / BOTTOM) and 2D
   transformation, routing keep-outs (`KEEPOUT_AREA_ROUTE` / `_VIA`) and height zones (`KEEPOUT_AREA_COMPONENT` with
   the height limit as the extrusion). Millimetres, MCAD XY (Y up).
2. MCAD moves, rotates or flips parts and sends back a baseline or a change file (`SendChanges`).
3. **File → Import MCAD Changes (IDX)…** applies it (one Undo step): parts by designator, a reshaped board
   outline and its thickness, routing / via keep-outs and component keep-outs (as height zones; height 0 = no parts).
   Keep-outs from an earlier MCAD import ("MCAD …") are replaced, SiEDA's own stay. The status line lists what changed. Namespace prefixes may differ
   between tools; the reader matches elements by their local names and accepts numbers written plainly or in a
   `Value` element. Rotations snap to 90°.
4. To send SiEDA's own moves back, MCP `output_idx_changes` (`baseline`, `path`) or C `sieda_export_idx_changes`
   writes a `SendChanges` file with only the parts placed differently from the baseline MCAD already has.

5. MCAD proposals are answered with MCP `output_idx_response` (`changes`, `path`, `accept`) or C `sieda_idx_response`:
   every proposed change accepted or rejected.

Outlines and keep-outs may use polylines, arcs (`Arc`: start, end, swept angle) and circles (`CircleCenter`); arcs
are read in steps of at most 10°. Round cut-outs in the board outline come back as its drilled holes (moved, added,
removed or resized in MCAD). A keep-out of any shape becomes one rectangle per run in each horizontal band between
its corners — exact for L / U / notched shapes, never smaller than the shape (more than 32 bands: its bounding box);
the pieces are named "MCAD name", "MCAD name #2", …. The files are checked for well-formed XML and a
SiEDA round trip in the tests; they have not yet been checked against a commercial MCAD tool.

Code: `Core/src/Mechanical.cpp` (`exportStep`, `exportIdfBoard`, `exportIdfLibrary`, `importIdfPlacement`,
`exportIdx`, `exportIdxChanges`, `importIdxPlacement` with a small namespace-agnostic XML reader).

## The 3D view

The 3D workspace (**Assembly**) has these tools in its options bar:

- **View:** top, bottom (flip), front, back, left, right and isometric cameras; **Focus Selection** flies to the
  selected part; **Save View As…** names the current camera (kept per project name); the named views follow, and
  **Forget Saved Views** clears them.
- **Layers:** show or hide the solder mask, silkscreen, copper finish, solder and part markings; **Hide Selected
  Parts**, **Isolate Selected Parts**, **Show All Parts**.
- **Explode** (slider): lifts the parts up to 8 mm off the board.
- **Cut:** a section plane across the board's width; its slider moves the plane.
- **Measure:** click two points on the model for the distance and its X / Y / Z parts.

Clicking a part selects it in the schematic and the board (cross-probe) and shows its designator, value, side and body
height. Parts selected in the schematic or the PCB layout are boxed in cyan in 3D.

**Panels** (on by default) lays the workspace out like a mechanical CAD tool: layers and the parts list (eye = hide,
click = select) on the left, the picked part's properties and 3D model on the right, and the 3D clearance findings
below (click one to pick and fly to its part).

The **Mechanical** menu:

- **Check 3D Clearances** runs the DRC; with **Layers → Show Clashes** on, the parts it flags (collision, height,
  enclosure) are boxed in red.
- **Load Enclosure…** reads a STEP / STL / OBJ / VRML model in the board's MCAD frame (millimetres, underside at Z = 0,
  as the STEP export writes it). It is saved with the project and drawn see-through (**Layers → Enclosure**). The DRC
  reports `MECH_ENCLOSURE` for each part body that cuts into it. **Remove Enclosure** drops it.
- **Save Image…** (PNG of the view), **Export USDZ…**, **Export glTF (.glb)…** and **Export VRML…** (metres, vertex
  colours). **File → Export** has glTF and VRML too.

A clicked part's **3D Model…** gives it its own model (STEP / STL / OBJ / VRML, any part, catalog ones too), centred
on its footprint and standing on the board (`PcbLayout::partModels`, saved as `partModels` with the mesh in `models3d`;
C `sieda_pcb_set_part_model`); **Generated Body** removes it. The cube button in PCB Layout shows the board in 3D beside
the layout, following every edit.

**Layers → Standoffs and Screws** draws a 6 mm brass hex standoff under each mounting hole and a pan-head screw on top.

`MECH_CONNECTOR_ACCESS` (a note) names a part standing in the 3 mm in front of a connector's open end, where the plug
goes. AI clients get the same picture from MCP `render_3d` (`view` iso / top / bottom / front / back / left / right).
