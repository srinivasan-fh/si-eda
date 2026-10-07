# Schematic sheets, hierarchy, variants, buses and annotation

This guide covers the large-design features of schematic capture: multi-sheet and hierarchical schematics, label
scopes, sheet symbols, repeated (multi-instance) sheets, the cross-sheet electrical rule checks, graphical buses,
multi-unit parts, designator annotation, assembly variants (BOM, assembly and simulation), find / replace, the net
navigator and the title block; nested repeated sheets and per-channel values, signal harnesses, schematic directives
(net classes and differential pairs as the source of the PCB rules), and the editing tools (arrange, smart paste,
cross-probing, back-annotation, messages, sheet templates and PDF). Core code: `Core/src/Sheets.cpp` (sheets, hierarchy, bus labels, annotation,
cross-sheet ERC), `Core/src/Instances.cpp` (repeated sheets), `Core/src/Buses.cpp` (graphical buses),
`Core/src/PartUnits.cpp` (multi-unit parts), `Core/src/SchematicSearch.cpp` (find / replace, net navigator),
`Core/src/Schematic.cpp` (connectivity and net naming), `Core/src/Variants.cpp` and `Project.cpp` (variants,
simulation of a variant, title block, persistence). App: the sheet bar and the inspector in
`SiEDA/Views/Schematic/SchematicEditorView.swift`, `SchematicCanvas.swift`, `SchematicFindView.swift` and
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

## Repeated sheets (multi-instance hierarchy)

A block drawn once can be used several times — four identical amplifier channels, eight relay drivers. Right-click
the block's tab → **Repeat Sheet…** and enter the number of channels (1 to 64; the sheet itself is the first).

- Each extra channel is a sheet of its own, named `<block> [B]`, `[C]` …, under the same parent. The tab of the block
  shows `×N`. Every channel without a sheet symbol gets one on the parent sheet, side by side; wire each channel's
  entries to its own signals.
- A channel holds copies of the block's parts and wires. Each copy is a real part with its **own designator, nets,
  footprint placement and variant settings**; local labels and ports are per sheet, so each channel has its own
  nets (named `Amp [B]/OUT` where a name repeats). Netlist, ERC, simulation, BOM, CPL and the board see every
  channel — the stored design is still flat.
- **Edit any channel**: moving, rotating, re-valuing, wiring, adding or deleting on a channel changes the block, and
  every channel follows at once. A banner over the canvas says which channel is shown. Footprint placement, BOM
  sourcing and variant fitting stay per channel.
- **Designators.** Each block part has a block designator (`R1`, shown as *Block designator* in the inspector) and
  one designator per channel. Right-click → **Channel Designators**: *By Sheet Number* (default; the n-th sheet
  numbers from n·100 + 1: R1 → R201, R301, R401 — n·1000 for blocks with 100 or more of a prefix) or *With Channel
  Suffix* (R1_A, R1_B …). **Rename Channel…** changes a channel label (letters, digits, `_`, `-`). **Annotate**
  numbers each block inside itself and numbers the rest of the design around the channels' designators.
- Repeating again with a smaller count removes the last channels (their parts, wires and sheet-symbol entries);
  **1** ends the repetition and gives the parts their block designators back. Deleting the block's sheet deletes
  its channels.
- **Nested repetition** (repeat inside repeat): a sheet whose child sheets are all repeated blocks can be repeated
  itself. Build the hierarchy first — *Amp* with its child *Stage* — repeat *Stage* (×2), then *Amp* (×4): every
  Amp channel gets its own two Stage channels (`Stage [B/A]`, `Stage [B/B]` …), with sheet entries that lead into
  its own Stage channels. Designators follow the channel path: *With Channel Suffix* gives `R1_B_A` (outer channel
  first); *By Sheet Number* numbers each channel's sheet. Changing the Stage count changes it in every Amp channel;
  renaming a Stage channel renames it in every Amp channel. Up to 1024 sheets in a design.
- **Helper sheets** (ordinary child sheets inside a block): right-click a repeated block's tab (or any channel's) ▸
  **Add Helper Sheet** adds a child sheet that every channel gets its own copy of (`Bias [B]`, with its own
  designators and nets, and the channel's sheet entries leading into its own copy) — a nested block of one channel.
  Before repeating, a child sheet is marked with right-click ▸ **Repeat With Its Block**; a plain child sheet (not
  marked) still keeps its parent from being repeated, and **Add Child Sheet** is not offered on a repeated block.
  The mark is saved as the sheet's `"helper": true`; it cannot be removed while the block is repeated. C API
  `sieda_add_helper_sheet`, `sieda_set_helper_sheet`.
- A nested block's channels cannot be
  deleted or moved on their own (change the repeat count). Parts moved onto a channel join the block; parts of a
  channel cannot be moved out of it (move them on the block's own sheet).
- **Per-channel parameters**: select a part on any channel; the inspector's **Channel value** sets the value of that
  channel only (R1 = 10k in A, 12k in B), and **Use Block Value** goes back to the block's. The **Value** field
  still sets the block's value: every channel that does not set its own follows. Setting a channel value on the
  block's own sheet (channel A) keeps the other channels' current values. The core also keeps a per-channel package
  (`sieda_set_channel_package`). The netlist, ERC, BOM, simulation and board all see each channel's own value —
  the copies are real parts.
- **More per-channel parameters**: **Fitted in this channel** (inspector) leaves the part off (DNP) in that channel
  only — BOM sourcing (manufacturer, MPN, supplier part, price, DNP) was always per channel. The SPICE model sheet's
  **This channel only** attaches an imported model to one channel; per-channel firmware is in the C API
  (`sieda_set_channel_firmware`). The inspector shows which parameters a channel has of its own, each with
  **Use Block's**. Saved in the copy's `channelOverride` bits (1 value, 2 package, 4 SPICE model, 8 firmware);
  an older SiEDA reads bits 4 and 8 as unknown and drops them (the channel then takes the block's model). C API
  `sieda_set_channel_spice_model`, `sieda_clear_channel_override`, `sieda_set_channel_fitted`.

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
| `ERC_GLOBAL_LABEL_IN_REPEAT` | warning | A signal's global label inside a repeated sheet joins every channel into one net (supply nets are exempt) |
| `ERC_OUTPUT_CONFLICT` | warning | Existing rule; the message now names the sheets when the drivers are on different sheets |

The multi-sheet rules never fire on a single-sheet design except `ERC_BUS_LABEL`.

## Buses

Bus notation names a group of nets: `D[0..7]` is D0 … D7, `A[15..12]` counts down, a suffix is kept
(`D[0..1]_N` → D0_N, D1_N) and comma lists combine (`D[0..3],WR,RD`). Up to 1024 members.

### Graphical buses

- **Draw**: the **Bus** tool (**B**) in the tool strip. Click the corners; click the last point again (or press
  Return) and name the bus in bus notation. Esc cancels. A bus is drawn as a thick line with its name.
- **Select** a bus by clicking it; drag it to move it with its entries; ⌫ deletes it with its entries.
- **Bus entries** are net labels attached to the bus, drawn with a short diagonal stub from the bus. They join nets
  by name like any label — **local to the sheet** by default — so a bus member connects to every entry and label of
  the same name. The bus line itself carries no connection.
- In the inspector of a selected bus: rename it, **Rip Out Entries** (an entry for every member that has none,
  spaced along the bus; wire each entry to its pin), or **Connect to Part** — every member that names a pin of the
  chosen part (D0 → pin `D0`, or one function of `PB0/D0`) gets an entry near that pin, wired to it; when no names
  match, the members go to the part's unconnected pins in order. Pins already wired are left alone.
- Buses on a repeated sheet are copied into every channel with their entries.

| Code | Severity | Meaning |
|---|---|---|
| `ERC_BUS_ENTRY_NOT_MEMBER` | error | An entry's name is not a member of its bus |
| `ERC_BUS_MEMBER_UNCONNECTED` | warning | A member leaves the bus to one pin only — nothing else on the bus connects to it |
| `ERC_BUS_NO_ENTRIES` | warning | A bus has no entries |

### Bus labels without a bus line

`sieda_add_bus_labels` (core: `Schematic::addBusLabels`) puts one label per member on a list of pins of a part — in
order, each just outside its pin, facing away from the part, wired to it — with the scope you choose.

## Signal harnesses (structured buses)

A harness is a named bundle of signals — USB (DP, DN, VBUS, GND), SPI, a debug header — carried between sheets as
one, like Altium's signal harnesses. Code: `Core/src/Harnesses.cpp`.

- **Harness types**: sheet bar ▸ **Harnesses ▸ Harness Types…** — a name (letters, digits, `_`, `-`) and its
  members (comma-separated, unique, no dots). Types are part of the project.
- **Harness connector**: **Harnesses ▸ Place USB Connector…** places a *harness label* named e.g. `USB1` (drawn in
  the harness colour with `≡`) and one *harness entry* per member, joined to it by thin stubs. Wire each entry to its
  signal: entry `DP` of harness `USB1` joins the member net **`USB1.DP`** on its sheet. A local label named
  `USB1.DP` joins it too.
- **Across the hierarchy**: set the harness label's scope to **Port** on the child sheet; **Place Sheet Symbol**
  gives the parent a harness sheet entry `USB1` of the same type, and every member crosses through that one entry
  — the parent's own `USB1` connector meets them. A **Global** harness label carries its members to every global
  harness label (and global label `USB1.DP`) of the name.
- In the inspector a net label's **Harness** picker makes it a harness label of a type (or a single signal again);
  **Add Missing Entries** completes a connector after the type grew.
- The harness label itself is a bundle, not a net: it is not in the netlist, and ERC does not report it as dangling.
  Members are ordinary nets everywhere (netlist, simulation, BOM, board). Harness connectors on a repeated sheet are
  copied into every channel (each channel has its own members, as with local labels). A member named like a ground
  (`GND`) joins the ground net, as every ground label does.

| Code | Severity | Meaning |
|---|---|---|
| `ERC_HARNESS_UNKNOWN_TYPE` | error | A harness label's type is not defined |
| `ERC_HARNESS_ENTRY_NOT_MEMBER` | error | A harness entry's name is not a member of its harness's type |
| `ERC_HARNESS_TYPE_MISMATCH` | error | A sheet entry and the port behind it carry different harness types (or one is a single signal) |
| `ERC_HARNESS_MEMBER_UNCONNECTED` | warning | A member reaches no part pin |
| `ERC_HARNESS_TYPE_UNUSED` | info | A type is defined but not used |

## Schematic directives: net classes, differential pairs, parameter sets

The schematic is the source of the board's net rules (as in Altium). Code: `Core/src/Directives.cpp`,
`Project::applySchematicRules`.

- **Net classes**: sheet bar ▸ **Net Classes** — a name and a track width and / or clearance (mm; empty = the
  board's default).
- **Directive on a net**: select a net label or a wire; the inspector's **Net directive** sets the net's **class**,
  marks it a **differential pair member** (paired with the net of the opposite suffix: `X_P`/`X_N`, `X+`/`X-`,
  `X_DP`/`X_DN`, `X.DP`/`X.DN`, `CANH`/`CANL`, `X_H`/`X_L`, `XP`/`XN`), and gives it its own **track width** and
  **clearance** (a parameter set: it wins over the class). The directive sits on the label's (or the wire end's)
  pin and is drawn as a small `◆` flag with its class, `⇄` for a pair and its sizes. On a repeated sheet it applies
  in every channel.
- **Carried to the PCB** at every change: the widths become the board's net widths, the clearances its per-net
  clearances — the autorouter and the interactive router (route, pair, bus, drag, shove and walkaround; a pair's gap
  is at least its nets' class clearance) keep other nets' copper that far away, and DRC reports copper closer than a
  net's class clearance (`DRC_NET_CLASS_CLEARANCE`, warning). Pairs marked by directives join the name-based differential
  pairs used by the autorouter (impedance width), the interactive differential router, length tuning and the
  signal-integrity checks. Removing a directive gives the board its own rules back; widths the designer set on the
  board for other nets stay.

| Code | Severity | Meaning |
|---|---|---|
| `ERC_DIRECTIVE_UNKNOWN_CLASS` | error | A directive names a net class that is not defined |
| `ERC_DIRECTIVE_NO_NET` | warning | A directive sits on an unconnected pin |
| `ERC_DIFF_PAIR_UNPAIRED` | warning | A net marked as a pair member has no partner net of the opposite suffix |
| `ERC_DIRECTIVE_CONFLICT` | warning | A net gets several net classes (the widest width and clearance apply) |

## Multi-unit parts

A part with several identical gates — a quad op-amp, a hex inverter — can be drawn one gate per symbol.

- Units are defined in the Symbol Editor's **Units** mode (docs/SYMBOL_EDITOR.md): gates, pin assignment, shared
  and power pins, gate swap groups and pin-swap groups. In the spec they are written as **units**: `"units":[{"name":"A","pins":["1","2","3"]},{"name":"B","pins":["7","6","5"]},…]`.
  Pins in no unit form an extra **power unit** `P` (the shared supply pins). A pin listed in several units is shared:
  one pin, drawn on each. Each unit gets its own symbol (from the part's symbol layout where it places every pin of
  the unit, else arranged by pin type). The standard library's **LM324** is a quad op-amp with units A–D and P.
- Placing a multi-unit part from the device picker places **unit A**. Select a unit and use **Place Next Unit** or
  **Place Unit** in the inspector for the others; units may sit anywhere, on any sheet. A unit shows its designator
  with the unit letter (`U1A`), and its value, designator and variant fitting are the part's.
- One footprint: the units belong to a hidden package that carries every pin and the footprint. The netlist, BOM, CPL,
  simulation and PCB see that one part (`U1`, 14 pads); a unit's pins are its package's pins. Deleting the last unit
  deletes the part. Selecting the part on the board highlights its units.
- **Gate swap / pin swap**: select two units of one part → **Swap Gates** in the inspector (interchangeable gates:
  identical symbols, or one swap group); a unit's **Swap Pins** menu exchanges the wires of two pins of a pin-swap
  group. C API `sieda_swap_units`, `sieda_swap_pins`; unit checks `sieda_check_units`.
- **Annotate ▸ Pack Units into Packages** re-assigns interchangeable gates (same pins in the same places, or one swap
  group) to packages
  in placement order — A, B, C, D of the first package, then the next — and gives the power units to the packages in
  turn, before numbering. Gates on repeated sheets keep their packages (one per channel).
- ERC: a unit's open pins are reported with the unit designator (`U1A.IN1+`); a unit that is not placed is reported
  when its pins are open (`ERC_UNIT_NOT_PLACED`, warning), and the power pins of an unplaced power unit as
  `ERC_POWER_PIN_UNCONNECTED` (error).
- Placed through the C API with `sieda_add_custom_component`, such a part is still one whole symbol, exactly as
  before; `sieda_add_custom_units`, `sieda_add_part_unit` and `sieda_place_next_unit` place it by units. An AI design
  plan carries the part with its `units` (where each gate sits): refining with the agents keeps it gate by gate.

## Annotation

**Annotate** in the sheet bar re-numbers reference designators. Net symbols (ground, labels, junctions) keep theirs.

| Option | Result |
|---|---|
| Number by Rows | sheet by sheet (sheet-bar order), top to bottom, then left to right: R1, R2, … |
| Number by Columns | sheet by sheet, left to right, then top to bottom |
| Number by Sheet (R101, R201…) | parts on the n-th sheet are numbered from n·100 + 1 (n·1000 + 1 when a sheet holds 100 or more parts of one prefix) |
| Pack Units into Packages | first re-assigns the gates of multi-unit parts to packages in placement order (see above), then numbers by rows |
| Fix Duplicates Only | keeps every unique designator; the second and later uses of a designator and unnumbered ones (`R?`) get the next free number |

Positions within one grid step count as the same row or column. Tamper meshes follow their part when its designator
changes. Annotation is one undo step.

## Design variants

A variant is a named assembly option of the same schematic and board: some parts not fitted (DNP), some fitted with
another value. The base design is what the schematic says (with each part's own DNP flag from the BOM workspace).

- The **variant menu** in the sheet bar picks the active variant (**Base Design** or a named one), creates a variant
  (**New Variant…**, a copy of the active one) and deletes the active one.
- With a variant active, the inspector shows its **Fitted** switch and **Value** for the selected part. Parts not
  fitted are **crossed out** on the canvas with `DNP` beside their designator (also parts marked DNP in the BOM
  workspace); a variant value is shown in amber instead of the design value.
- The active variant drives the BOM workspace, the BOM / assembly BOM / CPL / pick-and-place / assembly drawing
  exports and the assembly files of the fabrication package (whose order notes name the variant). Gerbers, drills,
  the IPC-D-356 netlist and the board are the same for every variant.
- **Simulation follows the active variant**: DC, transient, AC, sweeps, Monte Carlo, FFT, the live board and the
  SPICE netlist export run the circuit as assembled — variant values applied, parts not fitted (in the variant, or
  marked DNP) left out of the circuit while their nets stay. The results name the variant and the parts left out
  (shown next to the DC result in the simulation transport). With the base design and nothing marked DNP the
  simulated circuit is exactly the design.
- Variants never change connectivity or ERC: a not-fitted part keeps its footprint and copper. Verification, circuit
  validation and the industry checks analyse the design as drawn.
- Variants are keyed by component id, so re-annotating designators does not disturb them. Settings for deleted parts
  are dropped when the project is saved.

## Find & replace, net navigator, cross-probing, title block

- **Find & Replace** (⌘F, or the magnifier in the schematic options bar) searches designators, values, net labels
  and net names — and pin names when asked — on every sheet, in sheet order. Click a result to show it on its sheet,
  selected and zoomed. **Replace All** replaces the text in part values and net label names everywhere as one undo
  step (designators are re-numbered with Annotate). *Whole field* matches a complete value only. A repeated block's
  part and a multi-unit part are edited once.
- **Net navigator**: the inspector of a wire or a net label lists every place its net appears — pins, global and
  local labels, ports, sheet entries, bus entries, ground — sheet by sheet; click one to go there.
- **Cross-probing through the hierarchy**: a sheet entry's inspector has **Go to Port** (opens the child sheet on
  the port), a port's has **Go to Sheet Entry**. Picking a part on the PCB, in the BOM or in a check opens its sheet.
- **Title block**: with nothing selected, the inspector's *Title Block* group sets title, company, revision, date and
  drawn-by. Each sheet shows it at the bottom right of its drawing with the sheet name and "Sheet n of N". It is saved
  with the project (only when set) and the title defaults to the project name.

## Editing, back-annotation, messages, templates and PDF

Code: `Core/src/SchematicEdit.cpp`, `Core/src/Eco.cpp`, `Core/src/SchematicPdf.cpp`; app
`SiEDA/App/DesignStore+SchematicTools.swift`, `SiEDA/Views/Schematic/SchematicToolsViews.swift`.

- **Rubber-banding**: wires follow parts while they are dragged (wires join pins, so they always did).
- **Arrange** menu (options bar): **Align** left / right / top / bottom / centres and **Distribute** horizontally /
  vertically (three or more parts) — by the symbols' outlines with **Align by Symbol Outline** on (the default:
  left edges line up, distributing leaves equal gaps between outlines; positions stay on the grid), or by the parts'
  reference points with it off. One undo step; a channel copy moves its block's part. C API `sieda_align_outlines`.
- **Copy / Cut / Paste** (⌘C / ⌘X / ⌘V on the canvas, or the Arrange menu): parts and the wires between them,
  label scopes, packages, no-connect marks, harness connectors with their entries, the **buses** the copied bus
  entries belong to (and a selected bus), **net directives** on the copied pins, BOM **sourcing** and each part's
  **variant settings** (applied to the variants of the same name where it is pasted). Pasted parts get the **next free
  designators**. **Paste Array…** places *n* copies, each one step further, counting net label numbers up by the
  increment (`D0` → `D1`, `D2` …; zero padding kept: `A07` → `A08`). A pasted sheet entry becomes a local label; a
  pasted gate becomes a part of its own showing the same gate.
- **Cross-probing both ways**: the inspector of a part has **Show on PCB** (switches to the board, selects and
  zooms it; a unit shows its package) and, on the board, **Show in Schematic** (opens its sheet, selects and zooms
  the part or its first unit).
- **Back Annotate** (options bar): *From Board Positions (Rows / Columns)* re-numbers designators from the board,
  per prefix, in board order (parts of repeated sheets keep theirs); *From WAS / IS File…* reads `OLD NEW` lines
  (renames), `PINSWAP U1 2 3` (two pins exchange their connections) and `GATESWAP U1A U2B` (two gates exchange
  places). The **ECO review** lists every change with a check box; changes that cannot be applied say why (unknown
  part, target designator taken, pins on different units). **Apply Changes** is one undo step; renames go through
  temporary designators so swaps (R1 ↔ R2) work, and the chosen set must keep designators unique.
- **Pin / gate swap on the board** (Altium's PCB pin / gate swapping): select a multi-unit part's footprint (or one of
  its gates) in the PCB editor; the inspector's **Pin / Gate Swap** lists the swaps its package allows — two pins of
  one swap group of a gate, or two interchangeable gates (of this package or another of the same part and value) —
  each with the ratsnest it saves (nearest-pad estimate). **Swap** makes one; **Optimize Swaps** makes the best ones
  for the part, one after another, while they shorten the connections. Every swap is back-annotated: the schematic's
  wires (pin swap) or gates (gate swap) change, the board's nets follow, routing that no longer fits is removed, and
  Update PCB has nothing to bring over. One undo step. Units of a repeated sheet's channels are swapped on the
  block's own sheet. C API `sieda_pcb_swap_options`, `sieda_apply_pcb_swap`, `sieda_optimize_pcb_swaps` (all
  packages with -1).
- **Messages**: the options bar's **Messages** panel lists every ERC finding of every sheet with its sheet name;
  **Compile** re-runs ERC; click a message to show it on its sheet, selected and zoomed.
- **Error reporting** (Altium's project options ▸ Error Reporting): right-click a message ▸ *Report as Error /
  Warning / Info*, *Do Not Report*, or *Rule's Own Severity* — for that rule everywhere in the project (saved with
  it as `ercSeverities`; one undo step). C API `sieda_set_erc_severity`.
- **Drawn sheet symbols and harness connectors**: right-click a child sheet's tab ▸ **Sheet Symbol Size** — *Fitted to
  Entries* or a fixed size (120 × 80 … 360 × 280 units); the box never shrinks below what its entries need (saved as
  the sheet's `symbolSize`; C API `sieda_set_sheet_symbol_size`). A harness connector is drawn as a body around its
  entries, notched on the side its harness label leaves from, on the canvas and in the PDF.
- **Sheet templates**: right-click a sheet tab ▸ **Sheet Size** — A4 … A0, ANSI A … E, or *Auto* (the smallest A
  size that holds the drawing at full scale). Choosing a template **fixes the frame** where it is drawn (centred on
  the drawing; 10 units = 2.54 mm): it stays put while parts move, like a real sheet border, and the PDF prints the
  sheet at full scale with the drawing where it sits in the frame. **Centre Frame on Drawing** moves it again. Saved as
  the sheet's `frame` (`[x, y]`, top-left); files without it keep the frame centred on the drawing. C API
  `sieda_set_sheet_frame`.
- **PDF** (options bar): every sheet in sheet order, one page per sheet on its template, with a frame and zone markers
  (1, 2, 3 … / A, B, C …), the title block (title, company, revision, date, drawn by, sheet name, size, "Sheet n of
  N") and the drawing (at full scale in a fixed frame; scaled down when it is larger than the sheet or sticks out of
  its frame). The PDF's bookmarks follow the sheet hierarchy (Unicode titles). Symbols are the canvas's: resistor
  zigzags, capacitor plates, inductor loops, diode / LED, sources, battery, transistors, op-amp, switch, connector,
  IC, fuse and ground turned with the part; library parts with their own body box and Symbol Editor drawings, units
  with their unit box; label flags, sheet symbols and harness connector bodies. **Text**: Latin (WinAnsi) in
  Helvetica, Greek letters and math signs (Ω, µ, ≤, ∞ …) from the Symbol font, and every other script from a
  subset of a system TrueType font embedded in the file (macOS: *Arial Unicode*; glyphs one per character — scripts
  that need shaping, such as Devanagari conjuncts or Arabic joining, print unshaped). Without such a font those
  characters print as `?`. The file stays plain ASCII (the font as hex). C API
  `sieda_export_schematic_pdf_with_font`.
- **Update PCB** (options bar ▸ Back Annotate menu ▸ Update PCB; Altium's *Design ▸ Update PCB* engineering change order): lists every change from the
  schematic to the board since the last update, grouped as **Components** (new parts to place, removed parts,
  changed designator / footprint / value), **Nets** (new, removed, changed pin lists), **Copper Pours** (pours on
  nets that no longer exist, to remove) and **Net Rules** (widths / clearances from directives that the board lacks
  or holds differently). Each change has a check box; **Validate** reads the changes again; **Execute Changes**
  carries out the chosen ones as one undo step (new parts are placed next to the board; removed parts and pours
  leave it; rules are written from the schematic) and lists what was done. Unchosen changes stay pending. The
  baseline (what the board was last updated from) is saved as `pcbSync` only while an update is pending.
- **Place new parts** (after Update PCB; Altium's component placement after an ECO, KiCad's footprints on the
  cursor): when the update adds footprints, the PCB editor opens and places them one at a time, by designator. The
  current part's ghost (courtyard, pads and ratsnest lines to its nets) follows the cursor, snapped to the 0.25 mm
  grid, green where it may go, red where it may not (overlapping another part's courtyard on the same side, past the
  board outline, in a mounting-hole keep-out) and amber with a warning (its pads inside a routing keep-out). It starts
  at the free spot nearest the parts it connects to. **R** (or a Space tap) turns it a quarter turn, **F** flips it
  to the other side, a **click** places it — one undo step per part — and **Esc** skips it. A drag pans. An illegal
  spot is refused with the reason in the status bar and nothing moves; **⌥-click** places it anyway (the DRC then
  reports the overlap). The HUD at the bottom shows *Placing U3 (2 of 5)* with **Skip**, **Skip All** and **Place All
  Automatically** (every remaining part to its suggested spot, one undo step). New parts already stand where Auto
  Place put them, so a skipped part keeps that position. **Settings → Appearance → PCB Editor → Place new parts
  interactively after Update PCB** (on by default) turns the mode off: new parts then stay where Auto Place puts them,
  as before. Nothing about the mode is saved in the project. Core: `Project::applyPcbEco(keys, report,
  placementQueue)` lists the added footprints; `checkPlacement` / `placeComponent` (an illegal pose is *not*
  committed unless `allowIllegal`, which commits it with `legal` false) / `suggestPlacement` in
  `Core/src/InteractivePlacement.cpp`; C API `sieda_apply_pcb_eco` (`placementQueue`), `sieda_pcb_check_placement`,
  `sieda_pcb_place_footprint`, `sieda_pcb_suggest_placement`.

## Canvas colour schemes and grid

The schematic canvas can be drawn in any of fourteen ready-made colour schemes or in your own. Pick them from the
palette button (🎨) at the right of the schematic options bar, from **View → Schematic Canvas**, or in **Settings →
Appearance → Schematic Canvas**. The choice is an app-wide preference and the canvas follows it at once (wires,
symbols, pins, labels, selection, ERC and DNP markers, the sheet frame and title block, live probes and switch pills).
The Symbol Editor preview uses the same scheme. The schematic **PDF** keeps its print colours.

Every scheme is named after its colours. Some follow the familiar look of another tool; the tools are named here only
to describe the inspiration, never in the app.

| Scheme | Background | Wires | Symbols | Inspired by |
|---|---|---|---|---|
| **Midnight Navy** (default) | navy | sky blue | light blue on dark blue | SiEDA's own palette, unchanged |
| **Classic Cream** | cream | navy | dark red on pale yellow, blue net labels | Altium |
| **Paper White** | white | dark blue | dark red, black pins and text | Cadence OrCAD |
| **Blueprint Light** | white | blue | near-black outlines | Cadence Allegro System Capture |
| **Night Forest** | black | green | yellow, cyan pins, white text | Siemens Xpedition |
| **Amber Night** | black | yellow | light grey, green buses, white text | Siemens PADS Logic |
| **Slate Cyan** | charcoal | cyan | light grey | Zuken CR-8000 |
| **Meadow Cream** | cream (#F5F4EF) | green (#009600) | dark red (#840000) on pale yellow (#FFFFC2), blue buses | KiCad |
| **Mint Paper** | white | green | dark grey, maroon text | Autodesk Eagle / Fusion |
| **Ivory Garden** | ivory | forest green | dark blue, maroon text | Labcenter Proteus |
| **Ink Blue** | white | navy | dark red | EasyEDA |
| **Silver Mist** | light grey | dark blue | dark red | DipTrace |
| **Print Mono** | white | black | black (selection stays blue) | monochrome print |
| **High Contrast** | black | white | yellow, cyan buses and labels | accessibility |

The colour menus group the schemes into **Light** and **Dark**. Every foreground of every scheme (wires, junctions,
buses, labels, symbol outlines, pins, pin names and numbers, designators, values, unconnected-pin and no-connect
markers, selection, harnesses, ERC errors, previews, directives) reaches a WCAG contrast of at least 3:1 against its
background, and live-probe text at least 3:1 against its pill; a test checks every preset.

**Grid:** **Dots** (the default, as before), **Lines** or **None**. The pitch still adapts to the zoom (10, 50, 100,
500 … units, never denser than 8 points). In Lines mode a stronger major line is drawn every 4, 5, 8 or 10 minor lines
(**Major Grid Line Every**, default 10).

### Custom colours

Choose **Customise…** in any of the colour menus. The sheet starts from a scheme (**Start from**; the first time, the
scheme in use) and lists each role: Background, Grid, Wire, Junction, Bus, Net label, Power label, Symbol outline,
Symbol fill, Pin, Text, Unconnected pin, Selection, Harness and Error marker. Each role takes one of 41 named colours
(White, Ivory, Cream … Brown, Tan, each shown with a swatch) or **Other…**, which opens a colour well for an exact
colour. The canvas switches to **Custom** and updates while you pick. A role drawn on the background whose contrast
falls below 3:1 shows a warning (it is not blocked). **Reset** returns every role to the starting scheme.

The custom theme is saved as JSON in the app preferences (`schematic.customTheme`): the seed scheme and, per role, the
stable id of a named colour (`"green"`) or a hex value (`"#12AB34"`, `"#RRGGBBAA"` for a translucent fill). An unknown
id or an unreadable value falls back to the seed scheme's colour. Preferences: `schematic.colorScheme` (the scheme's
raw value, e.g. `kicad`, independent of its display name), `schematic.gridStyle` (`dots` / `lines` / `none`) and
`schematic.gridMajorEvery`.

Code: `SiEDA/Views/Schematic/SchematicPalette.swift` (palettes, schemes, grid styles, the `schematicStyle` environment
value the canvas draws with), `SchematicCustomTheme.swift` (named colours, roles, custom theme) and
`SchematicAppearanceViews.swift` (menus, swatches, the Customise sheet).

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
- AI design plans keep the design's structure (no flattening). A plan made from a design carries `"sheets"` (with
  `"channels"`, `"channel"`, `"refs"` on a repeated block and `"instanceOf"` on its channel sheets), each
  component's `"sheet"`, `"scope"`, `"targetSheet"`, `"blockRef"` (designator inside a block), `"channelValues"`
  (per-channel values by channel path, `{"B/A": "12k"}`), `"units"` (a multi-unit part gate by gate) and `"bus"`
  (index of the bus a label is an entry of), and top-level `"buses"`. A repeated block is drawn once in the plan;
  applying the plan repeats it again (inner blocks first), names and labels the channels as planned, points sheet
  entries into the channels and restores block designators and channel values. When the agents' answer leaves any
  of this out, it is merged back from the plan the refinement started from (by sheet name and designator); new
  parts without a sheet go on the sheet of a part they connect to.

Further optional fields (written only when used, so other designs' files are unchanged): sheets `instanceOf`,
`channel`, `refs` (a nested block's channels have a channel sheet as `parent`); components `instanceOf`,
`logicalRef`, `channelOverride` (1 value, 2 package, 4 SPICE model, 8 firmware: the copy keeps its own), `bus`, `unitOf` / `unit` (kind 20, a placed unit) and
`packageOnly`, `harnessType` / `harnessOf` (net labels); wires `instanceOf`; top-level `buses`, `harnessTypes`
(`[{"name","entries"}]`), `netClassDefs` (`[{"name","trackWidth"?,"clearance"?}]`), `directives`
(`[{"id","component","pin","netClass"?,"diffPair"?,"trackWidth"?,"clearance"?}]`) and `titleBlock`; board
`netClearances` and `schematicRuleNets` (the nets whose rules came from the schematic); sheets `size`; top-level
`ercSeverities` (`{"ERC_…": "error" | "warning" | "info" | "off"}`); sheets `symbolSize` (`[width, height]`), `helper`, `frame` (`[x, y]`). `pcbSync` (`{"parts":[{"id","ref","footprint","value"}],"nets":{name:[pins]}}`,
the Update PCB baseline; written only while the board is behind the schematic, and a file without it is in step). A file is repaired on load: copies whose
block part is gone, units without a valid package, packages without units, entries of missing buses and buses on
missing sheets are dropped; an instance of a missing or nested definition becomes an ordinary sheet. Older versions of
SiEDA open a file with repeated sheets as ordinary sheets (every channel's parts are real parts); a file with placed
units needs this version.

## C API

Repeated sheets, buses, multi-unit parts, find / replace and the title block:

```c
int32_t sieda_repeat_sheet(SiedaProject*, int32_t sheet, int32_t count);       /* channels, or -1 */
int32_t sieda_set_instance_refs(SiedaProject*, int32_t sheet, const char* scheme); /* "sheet" | "suffix" */
int32_t sieda_set_sheet_channel(SiedaProject*, int32_t sheet, const char* channel);
int32_t sieda_add_bus(SiedaProject*, const char* name, const char* points_json);
int32_t sieda_remove_bus(SiedaProject*, int32_t bus);
int32_t sieda_rename_bus(SiedaProject*, int32_t bus, const char* name);
int32_t sieda_move_bus(SiedaProject*, int32_t bus, double dx, double dy);
int32_t sieda_rip_bus_entries(SiedaProject*, int32_t bus, const char* members_json, const char* scope);
int32_t sieda_connect_bus_to_part(SiedaProject*, int32_t bus, int32_t component, const char* scope);
int32_t sieda_add_custom_units(SiedaProject*, const char* part_id, const char* value, double x, double y,
                               int32_t rotation, const char* ref);
int32_t sieda_add_part_unit(SiedaProject*, int32_t component, int32_t unit, double x, double y, int32_t rotation);
int32_t sieda_place_next_unit(SiedaProject*, int32_t component, double x, double y);
char*   sieda_schematic_find(const SiedaProject*, const char* request_json);
int32_t sieda_schematic_replace(SiedaProject*, const char* request_json);
char*   sieda_net_places(const SiedaProject*, int32_t net);
int32_t sieda_set_title_block(SiedaProject*, const char* json);
int32_t sieda_set_channel_value(SiedaProject*, int32_t component, const char* value);   /* "" = block value */
int32_t sieda_set_channel_package(SiedaProject*, int32_t component, const char* package);
int32_t sieda_clear_channel_overrides(SiedaProject*, int32_t component);
char*   sieda_check_units(const char* spec_json);
int32_t sieda_swap_units(SiedaProject*, int32_t unit_a, int32_t unit_b);
int32_t sieda_swap_pins(SiedaProject*, int32_t component, int32_t pin_a, int32_t pin_b);
int32_t sieda_set_label_bus(SiedaProject*, int32_t label, int32_t bus);
char*   sieda_harness_types_json(const SiedaProject*);
int32_t sieda_set_harness_type(SiedaProject*, const char* name, const char* entries_json); /* [] removes */
int32_t sieda_set_label_harness(SiedaProject*, int32_t label, const char* type);
int32_t sieda_add_harness_connector(SiedaProject*, const char* type, const char* name, double x, double y);
int32_t sieda_place_harness_entries(SiedaProject*, int32_t label);
int32_t sieda_set_net_class(SiedaProject*, const char* json);      /* {"name","trackWidth","clearance"} */
int32_t sieda_remove_net_class(SiedaProject*, const char* name);
int32_t sieda_add_directive(SiedaProject*, const char* json);      /* {"component","pin","netClass","diffPair",…} */
int32_t sieda_update_directive(SiedaProject*, int32_t id, const char* json);
int32_t sieda_remove_directive(SiedaProject*, int32_t id);
char*   sieda_net_rules_json(const SiedaProject*);
int32_t sieda_align_components(SiedaProject*, const char* ids_json, const char* mode);
char*   sieda_copy_components(const SiedaProject*, const char* ids_json);
char*   sieda_paste_components(SiedaProject*, const char* clip_json, const char* options_json);
int32_t sieda_swap_pin_connections(SiedaProject*, int32_t component, int32_t pin_a, int32_t pin_b);
char*   sieda_reannotate_from_board(const SiedaProject*, int32_t by_columns);
char*   sieda_eco_from_was_is(const SiedaProject*, const char* text);
int32_t sieda_apply_eco(SiedaProject*, const char* eco_json);
int32_t sieda_set_sheet_size(SiedaProject*, int32_t sheet, const char* size);
char*   sieda_sheet_templates_json(void);
char*   sieda_export_schematic_pdf(const SiedaProject*);
char*   sieda_export_schematic_pdf_with_font(const SiedaProject*, const char* font_path);
int32_t sieda_align_outlines(SiedaProject*, const char* ids_json, const char* mode);
int32_t sieda_set_sheet_frame(SiedaProject*, int32_t sheet, int32_t fixed, double x, double y);
char*   sieda_pcb_swap_options(const SiedaProject*, int32_t component);
int32_t sieda_apply_pcb_swap(SiedaProject*, const char* option_json);
int32_t sieda_optimize_pcb_swaps(SiedaProject*, int32_t component, int32_t max_swaps);
int32_t sieda_set_erc_severity(SiedaProject*, const char* code, const char* level); /* "error"…"off", "default" */
char*   sieda_pcb_eco_preview(const SiedaProject*);               /* [{section,action,object,detail,key,applicable,note}] */
char*   sieda_apply_pcb_eco(SiedaProject*, const char* keys_json); /* NULL = all; {"executed","report"} */
int32_t sieda_set_sheet_symbol_size(SiedaProject*, int32_t sheet, double width, double height); /* 0, 0 = fitted */
int32_t sieda_set_channel_spice_model(SiedaProject*, int32_t component, const char* text, const char* model,
                                      const char* pins, char** error_out);
int32_t sieda_set_channel_firmware(SiedaProject*, int32_t component, const char* hex, const char* name, double clock_hz);
int32_t sieda_clear_channel_override(SiedaProject*, int32_t component, const char* what); /* value|package|spice|firmware */
int32_t sieda_set_channel_fitted(SiedaProject*, int32_t component, int32_t fitted);
int32_t sieda_add_helper_sheet(SiedaProject*, const char* name, int32_t parent);
int32_t sieda_set_helper_sheet(SiedaProject*, int32_t sheet, int32_t helper);
```

Sheets, hierarchy, bus labels, annotation and variants:

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

- A plain child sheet inside a block must be marked as a helper (or repeated) before the block is repeated; a
  design holds at most 1024 sheets. Per-channel parameters cover value, package, SPICE model, firmware and BOM
  sourcing / DNP; the symbol, pins, wiring and footprint geometry are the block's. Per-channel firmware has no
  inspector control yet (C API only).
- A bus line has no electrical meaning of its own: members connect through their entries' names. Buses are not
  shown in the PCB editor (their members are ordinary nets there).
- Variants affect the assembly outputs and simulation, not ERC, verification or the board.
- A sheet symbol is a rectangle (fitted to its entries or a drawn size); it has no free-form graphics of its own, and
  its entries stay where they were placed (they are not snapped to the box's edge when the box grows).
- Wires never cross sheets; parts moved to another sheet lose their wires to parts left behind.
- A channel's own value is a per-channel parameter; per-channel fitting (DNP) is still made with a design variant.
  A unit of a multi-unit part inside a repeated sheet stays on that sheet with its package.
- Multi-unit parts: unit symbols are generated from the part's symbol layout (or arranged by pin type); a unit has no
  hand-drawn layout of its own. Gate swap works between units on one sheet. Unit packing re-assigns only interchangeable gates.
  A new multi-unit part an agent adds without `units` is drawn as one symbol. Board-side swaps judge by the nearest pad
  of each net (not routed length), swap gates only between units on one sheet, and are offered for a selected part
  (the whole-board optimisation is in the C API).
- Find & Replace edits values and net label names only; designators are changed by annotation.
- Harnesses are name based: a harness connector is a harness label plus entry labels (no drawn connector body or
  harness wire); harness types are flat (no nested harnesses inside harnesses). Harness labels are not shown on the
  PCB (their members are ordinary nets).
- Directives sit on pins (labels, part pins). A net class has a width and a clearance, no pair gap of its own: a
  differential pair's gap comes from the impedance target (or the router's pair-gap option), at least the class
  clearance. There is no "No ERC" marker (use ERC error reporting per rule, or no-connect flags on pins).
- Paste places parts on the shown sheet; a pasted gate becomes a part of its own. Variant settings go only to
  variants of the same name in the target design.
- Back-annotation reads designator renames, pin swaps and gate swaps (board re-annotation, a WAS / IS text or the
  board's pin / gate swap tool); repeated-sheet parts are not renamed from the board.
- The PDF's text is unshaped (one glyph per character) and needs a glyf-outline TrueType font for scripts beyond
  Latin / Greek; CFF (`.otf`) fonts are not embedded. A frame fixed so that the drawing sticks out of it prints
  scaled to fit, as before.
- Update PCB first places new parts with the automatic placer; placing them by hand afterwards moves them one at a
  time (no block placement of a group, no push-aside of other parts; parts still waiting in the queue count as
  obstacles at their automatic spots). The board reads parts and nets from
  the schematic live (one design), so removed parts, new designators, values and footprints are already on it:
  executing those changes records them (and removes routing that no longer fits) rather than moving copper.

## Device icons

The device palette, the component list and the inspector show each device by its real schematic symbol (a resistor
is its zigzag, a capacitor two plates, ground the ground bars), drawn from the same paths as the canvas
(`SchematicSymbols.shapes`) by `SiEDA/Views/Schematic/ComponentSymbolIcon.swift`; a symbol change on the canvas
changes its icon too.

Each device has its own colour (`ComponentKind.deviceColour`), grouped by family the way schematic part palettes
colour-code their groups:

| Family | Devices | Colour |
| --- | --- | --- |
| Power sources | voltage, battery, AC, current source | reds (power ports and flags are red) |
| Ground and nets | ground, net label, junction | greens (earth green, label and wire green) |
| Passives | resistor, capacitor, inductor, fuse | amber body, blue sleeve, copper winding, pale gold |
| Discrete semiconductors | diode, LED, NPN, N-MOSFET | violets; an LED shows the colour it lights (its value) |
| ICs | op-amp, IC, custom parts | the yellow of an IC body |
| Electromechanical | switch, connector | metal greys |

The canvas draws every symbol in its device colour too, in every scheme (Midnight Navy included), darkened or
lightened until it reads at 3:1 on the scheme's background. Turn off **Colour Symbols by Device** (in the colour-scheme
menu) to draw all symbols in the scheme's one symbol colour instead.
