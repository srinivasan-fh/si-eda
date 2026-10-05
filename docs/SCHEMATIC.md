# Schematic sheets, hierarchy, variants, buses and annotation

This guide covers the large-design features of schematic capture: multi-sheet and hierarchical schematics, label
scopes, sheet symbols, the cross-sheet electrical rule checks, bus labels, designator annotation and assembly
variants. Core code: `Core/src/Sheets.cpp` (sheets, hierarchy, buses, annotation, cross-sheet ERC),
`Core/src/Schematic.cpp` (connectivity and net naming), `Core/src/Variants.cpp` and `Project.cpp` (variants and
persistence). App: the sheet bar and the inspector in `SiEDA/Views/Schematic/SchematicEditorView.swift` and
`SiEDA/Views/Inspector/InspectorView.swift`.

## The model in one paragraph

A design is still one flat list of components and wires. Every component carries the sheet it is drawn on, and a
wire always joins two pins on the same sheet. Nets reach other sheets only through labels: global net labels and
ground symbols join everywhere, local labels join within their sheet, and hierarchical ports join the matching entry
on their parent sheet's sheet symbol. Because the stored design is already flat, the netlist, ERC, SPICE simulation,
BOM and PCB layout always see the whole design — there is no separate flattening step that could go stale.

## Sheets

- The **sheet bar** sits under the schematic options bar. Click a tab to show that sheet; new parts are placed on the
  sheet shown. **+** adds a sheet ("Sheet 2", "Sheet 3", …).
- Right-click a tab for **Rename Sheet…**, **Add Child Sheet**, **Place Sheet Symbol** (child sheets),
  **Move Selection Here** and **Delete Sheet**. Child sheets are indented under their parent (`› Filter`).
- Deleting a sheet that holds parts asks first; its parts and wires go with it, as do the sheet entries that lead into
  it. Its child sheets move up one level. The last sheet cannot be deleted. Every sheet operation is undoable.
- To move parts between sheets, select them and use **Move Selection Here** on the target tab, or the **Sheet** picker
  in the inspector. Junctions that only join moved parts move with them; wires to parts that stay behind are removed
  (connect them again with labels).
- Selecting parts from a list — a Design Checks finding, the BOM, simulation results — brings up the sheet they are on.
- Every sheet has its own drawing area: parts on different sheets may share coordinates.

## Label scopes

Select a net label and set its **Scope** in the inspector.

| Scope | Joins | Typical use |
|---|---|---|
| **Global** (default) | every global label of the same name on every sheet | supply rails, buses shared by the whole design |
| **Local (this sheet)** | labels of the same name on the same sheet | signals private to one sheet |
| **Port (to parent sheet)** | like Local, plus the entry of the same name on the sheet symbol in the parent sheet | the interface of a hierarchical block |
| Sheet entry | the ports of its name on the child sheet it points into | created by **Place Sheet Symbol** |

Ground symbols and labels named `GND`, `gnd`, `Gnd` or `0` always join the ground net, whatever their scope. A global
label and a local label of the same name are different nets, as in KiCad and Altium.

Every label of an older design is global, so older designs connect exactly as before.

### Net names

A net is named by its smallest global label name; otherwise by its sheet-scoped label from the sheet nearest the top
of the hierarchy; otherwise `N$n`. When a sheet-scoped name is also used by another net, the sheet's name is put in
front of it — `Sensor/EN` — so every net name in the netlist, the PCB and the exports stays unique.

## Hierarchical sheets and sheet symbols

1. Add a child sheet (right-click the parent's tab → **Add Child Sheet**).
2. On the child sheet, set the scope of each interface label to **Port**.
3. Right-click the child's tab → **Place Sheet Symbol**. The parent sheet gets one sheet entry per port, stacked in a
   column to the right of its parts, and is shown. Running it again adds entries only for new ports.
4. Wire the entries on the parent sheet like any other pin.

The canvas draws the sheet symbol as a box around the entries of each child sheet with the child's name above it;
drag the entries (select them together) to move or reshape the symbol.

Each child sheet is used once (single-instance hierarchy): there is no repeated instantiation of one sheet with
per-instance designators.

## Electrical rule checks across sheets

ERC findings carry the sheet they are on (`"sheet"` in the JSON); selecting one shows that sheet.

| Code | Severity | Meaning |
|---|---|---|
| `ERC_SHEET_ENTRY_NO_PORT` | error | A sheet entry has no port of its name on the child sheet |
| `ERC_SHEET_ENTRY_NO_SHEET` | error | A sheet entry points to a sheet that does not exist |
| `ERC_CROSS_SHEET_WIRE` | error | A wire joins two sheets (only a hand-edited file can hold one) |
| `ERC_PORT_UNUSED` | warning | A port has no entry on its parent sheet's symbol |
| `ERC_PORT_NO_PARENT` | warning | A port is on a top-level sheet: nothing connects to it from above |
| `ERC_SHEET_ENTRY_PLACEMENT` | warning | A sheet entry is not on its child sheet's parent |
| `ERC_LOCAL_LABEL_SPLIT` | warning | A local label name is used on several sheets; those nets are separate |
| `ERC_BUS_LABEL` | warning | A single label is named like a bus (`D[0..7]`); it joins one net, not eight |
| `ERC_GLOBAL_LABEL_ONE_SHEET` | info | In a multi-sheet design, a signal's global label is used on one sheet only (supply and ground nets are exempt) |
| `ERC_OUTPUT_CONFLICT` | warning | Existing rule; the message now names the sheets when the drivers are on different sheets |

The multi-sheet rules never fire on a single-sheet design except `ERC_BUS_LABEL`.

## Bus labels

Bus notation names a group of nets: `D[0..7]` is D0 … D7, `A[15..12]` counts down, a suffix is kept
(`D[0..1]_N` → D0_N, D1_N) and comma lists combine (`D[0..3],WR,RD`). Up to 1024 members.

`sieda_add_bus_labels` (core: `Schematic::addBusLabels`) puts one label per member on a list of pins of a part — in
order, each just outside its pin, facing away from the part, wired to it — with the scope you choose. Buses are a
labelling aid: SiEDA has no graphical bus wires or bus entries, so a bus connects through its member labels. Bus labels
are available through the C API (and so to scripts and agents); the app does not have a bus-label tool yet.

## Annotation

**Annotate** in the sheet bar re-numbers reference designators. Net symbols (ground, labels, junctions) keep theirs.

| Option | Result |
|---|---|
| Number by Rows | sheet by sheet (sheet-bar order), top to bottom, then left to right: R1, R2, … |
| Number by Columns | sheet by sheet, left to right, then top to bottom |
| Number by Sheet (R101, R201…) | parts on the n-th sheet are numbered from n·100 + 1 (n·1000 + 1 when a sheet holds 100 or more parts of one prefix) |
| Fix Duplicates Only | keeps every unique designator; the second and later uses of a designator and unnumbered ones (`R?`) get the next free number |

Positions within one grid step count as the same row or column. Tamper meshes follow their part when its designator
changes. Annotation is one undo step.

## Design variants

A variant is a named assembly option of the same schematic and board: some parts not fitted (DNP), some fitted with
another value. The base design is what the schematic says (with each part's own DNP flag from the BOM workspace).

- The **variant menu** in the sheet bar picks the active variant (**Base Design** or a named one), creates a variant
  (**New Variant…**, a copy of the active one) and deletes the active one.
- With a variant active, the inspector shows its **Fitted** switch and **Value** for the selected part. Parts not
  fitted show `DNP` beside their designator on the canvas; a variant value is shown in amber instead of the design
  value.
- The active variant drives the BOM workspace, the BOM / assembly BOM / CPL / pick-and-place / assembly drawing
  exports and the assembly files of the fabrication package (whose order notes name the variant). Gerbers, drills,
  the IPC-D-356 netlist and the board are the same for every variant.
- Variants never change connectivity, simulation or ERC: a not-fitted part keeps its footprint and copper, and the
  simulator uses the design values.
- Variants are keyed by component id, so re-annotating designators does not disturb them. Settings for deleted parts
  are dropped when the project is saved.

## Files and compatibility

New project fields (all optional when reading):

```json
"sheets": [{"id": 1, "name": "Main", "parent": 0}, {"id": 2, "name": "Power", "parent": 1}],
"activeSheet": 2,
"components": [{"id": 7, "sheet": 2, "scope": "port", "…": "…"},
               {"id": 9, "scope": "entry", "targetSheet": 2, "…": "…"}],
"variants": [{"name": "Lite", "description": "", "parts": [{"component": 4, "ref": "D1", "fitted": false},
                                                            {"component": 3, "ref": "R1", "value": "470"}]}],
"activeVariant": "Lite"
```

- A file without `"sheets"` loads as one sheet named "Main" with every component on it; a component's `"sheet"` is
  written only when it is not sheet 1, and `"scope"` only for non-global labels, so a single-sheet design's components
  are written exactly as before.
- Invalid entries in hand-edited files are repaired on load: unknown sheets move components to the first sheet, a
  parent cycle is broken, an entry into a missing sheet becomes a local label, an unknown scope from a newer version is
  read as local.
- AI design plans carry `"sheets"` and each component's `"sheet"`, `"scope"` and `"targetSheet"`, so asking the
  agents to change a multi-sheet design keeps its sheets and label scopes.

## C API

```c
char*   sieda_sheets_json(const SiedaProject*);                 /* {"active", "sheets":[{id,name,parent,depth,components,ports}]} */
int32_t sieda_add_sheet(SiedaProject*, const char* name, int32_t parent);
int32_t sieda_rename_sheet(SiedaProject*, int32_t sheet, const char* name);
int32_t sieda_set_sheet_parent(SiedaProject*, int32_t sheet, int32_t parent);
int32_t sieda_reorder_sheet(SiedaProject*, int32_t sheet, int32_t index);
int32_t sieda_remove_sheet(SiedaProject*, int32_t sheet, int32_t delete_contents);
int32_t sieda_set_active_sheet(SiedaProject*, int32_t sheet);
int32_t sieda_move_to_sheet(SiedaProject*, const int32_t* ids, int32_t count, int32_t sheet);
int32_t sieda_set_label_scope(SiedaProject*, int32_t label, const char* scope, int32_t target_sheet);
int32_t sieda_place_sheet_entries(SiedaProject*, int32_t child, double x, double y);
char*   sieda_expand_bus(const char* bus);
int32_t sieda_add_bus_labels(SiedaProject*, int32_t component, const int32_t* pins, int32_t count,
                             const char* bus, const char* scope);
char*   sieda_annotate(SiedaProject*, const char* options_json); /* {"order":"rows"|"columns","keepExisting","sheetNumbering"} */
char*   sieda_variants_json(const SiedaProject*);
int32_t sieda_add_variant(SiedaProject*, const char* name, const char* copy_from);
int32_t sieda_rename_variant(SiedaProject*, const char* name, const char* new_name);
int32_t sieda_remove_variant(SiedaProject*, const char* name);
int32_t sieda_set_variant_description(SiedaProject*, const char* name, const char* description);
int32_t sieda_set_variant_part(SiedaProject*, const char* name, int32_t component, int32_t fitted, const char* value);
int32_t sieda_set_active_variant(SiedaProject*, const char* name);
char*   sieda_export_variant(const SiedaProject*, const char* format, const char* variant);
```

The snapshot (`sieda_project_snapshot`) adds `sheets`, `activeSheet`, `variants`, `activeVariant` and, per component,
`sheet`, `scope` / `targetSheet` (labels), `fitted` (false when not fitted in the active variant or marked DNP) and
`variantValue`.

## Limits

- Single-instance hierarchy only (a child sheet stands for one block, not several copies).
- No graphical bus wires or bus entries; buses connect through member labels, placed through the C API.
- Variant values affect the assembly outputs only, not simulation or design checks.
- The sheet symbol is drawn from its entries; it has no separate size or graphics of its own.
- Wires never cross sheets; parts moved to another sheet lose their wires to parts left behind.
