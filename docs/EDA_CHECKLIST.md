<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# Best-in-class PCB EDA — SiEDA status against the checklist

The product checklist (26 sections, P0 essential → P3 enterprise) with SiEDA's status for every item. Work through
the **Pending** list at the end one item at a time; tick an item here when its PR merges with CI green.

| Mark | Meaning |
|---|---|
| ✅ | Done: in the core and the app, with tests |
| 🟡 | Partly there: what is missing is in the note |
| ⏳ | Pending: not built yet (see the pending list) |
| ⚪ | Not a code item (business, service, legal) — tracked outside the repository |

Where to look: the guides in `docs/` and the code map in `CLAUDE.md`.

---

## 1. Core platform and design database

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Unified database: schematic, PCB, footprints, parts, constraints, BOM, 3D | ✅ | `Project` (one JSON document) |
| P0 | Reliable undo / redo across editors | ✅ | compact state snapshots (`sieda_project_state_json`), one undo step per edit |
| P0 | Autosave, crash recovery, backups | ✅ | autosave, crash reporter / recovery |
| P0 | Accurate coordinates and units (mm / mil / inch) | ✅ | mm core; Settings → Board Units (mm / cm / mil / inch / ft / pt read-outs from exact ratios, ≥ 1 µm shown; Board Setup fields take a typed unit), the same unit in the footprint editor and inspector; user-entered coordinates on a 1 nm grid (`nm()`, exact in files and conversions) ✅; computed copper (tracks, vias) saved on the same grid, so a reopened board checks the same ✅; the fabrication package (Gerbers, drills, 3D model, drawing) is built from the board as saved, byte for byte the same before saving and after reopening ✅ |
| P0 | Cross-platform desktop app (Windows, macOS, Linux) | 🟡 | SiEDA Studio on Linux and Windows ([STUDIO.md](STUDIO.md)): open / save, schematic editing (place, move, rotate, wire, label, ground), board editing (move, rotate, flip, lock, align, live push-and-shove routing and pairs, track / via edits, pours, keep-outs, cutouts, holes, measure, net highlight, DRC markers), part editor, simulation plots, 3D, checks, BOM, Auto Place / Route, fabrication outputs, every MCP tool, GitHub sync, macOS-style menus with a form for every engine command, Ctrl+K command palette, Undo / Redo, company library and rooms / partitions windows, team rule packs, the four app themes and the macOS app's 20 interface languages; core tests pass on Linux, macOS and Windows CI ✅ |
| P0 | Fast canvas: pan, zoom, select, manipulate | ✅ | culling, level of detail, measured per-edit budgets |
| P0 | Save, reopen, copy, archive, recovery | ✅ | Save As, archive package planned in `ROADMAP.md` |
| P0 | Human-readable, documented project file | ✅ | indented JSON; `docs/FILE_FORMAT.md` |
| P0 | ➕ Text-based, diff-friendly file that merges | ✅ | sorted-key JSON, Git diff driver (`sieda-mcp --git-diff`), three-way merge |
| P0 | ➕ Forward / backward compatibility policy | ✅ | new fields written only when used; on open, a newer format or settings this version would drop are reported (`loadNotes`) |
| P0 | Bidirectional schematic ↔ PCB sync | ✅ | Update PCB (forward ECO), back-annotation ECO |
| P0 | Stable object IDs and references | ✅ | |
| P0 | ➕ Grids, snapping, origin per editor | ✅ | schematic grid / snap, PCB grid, footprint grid and origin |
| P0 | ➕ Object locking | ✅ | schematic `locked`, locked tracks kept by the router |
| P1 | Offline-first, optional cloud sync | ✅ | fully offline; optional GitHub cloud sync (MCP `project_cloud`, Studio → Cloud: connect / pull / push, project files merged field by field) ✅; shared-folder live co-editing |
| P1 | Project templates and reusable settings | ✅ | reference designs, rule presets, DFM packs; File → Save as Template… / New from Template |
| P1 | Versioned database and migration tools | ✅ | format version, load notes for newer files, `sieda-cli --upgrade` rewrites files in the current format |
| P1 | Custom shortcuts and command palette | ✅ | ⌘K palette, Settings → Shortcuts (menus and canvas tool keys) |
| P1 | Incremental updates | ✅ | delta snapshots, cached pours / board mask |
| P1 | ➕ Settings export / import (rules, colours, shortcuts) | ✅ | Settings → Appearance → Export / Import Settings… (rules travel in templates) |
| P1 | ➕ Multi-monitor, detachable editor windows | ✅ | Window → Open in New Window: schematic, board, 3D, simulation, checks or BOM in a window of its own on the same design |
| P2 | Multi-board system projects | ✅ | `Project::boardNames`, parked boards |
| P2 | Harness and connector system design | ✅ | `Harnesses.cpp` |
| P2 | Localisation and accessibility | ✅ | 20 languages, accessibility labels |
| P2 | Extensible design object model | ✅ | C API / MCP; plug-in objects kept with the design (`pluginObjects`, MCP `project_plugin_objects`) ✅ |
| P2 | ➕ Embedded components / cavities | ✅ | embedded passives |
| P3 | Company-wide database, managed templates, multi-site | ⏳ | |

## 2. Schematic editor

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Place, rotate, mirror, align | ✅ | |
| P0 | Wire draw / drag / reroute keeping connections | ✅ | straight-wire drag, wire-end moves, touching pins |
| P0 | Junctions, buses, labels, power symbols, net names | ✅ | |
| P0 | ➕ No-connect flags, off-page connectors, net ties | ✅ | no-connects, global / hierarchical labels, `NetTie-2` (the DRC accepts its own short) |
| P0 | ➕ Title blocks, revision tables, sheet numbers, text / graphics | ✅ | sheet templates, notes, drawings; revision table on the schematic PDF and the fabrication drawing |
| P0 | Annotation and renumbering | ✅ | `Sheets.cpp` |
| P0 | Multi-sheet, hierarchical and flat | ✅ | |
| P0 | Multi-unit parts and pin types | ✅ | `PartUnits.cpp` |
| P0 | Net highlight, connectivity tracing, search | ✅ | net navigator, find / replace |
| P0 | Copy, paste, duplicate, group, bulk edit | ✅ | |
| P0 | Print and searchable PDF | ✅ | `SchematicPdf.cpp` (TrueType subset, HarfBuzz) |
| P0 | ERC | ✅ | |
| P0 | ➕ Netlist and schematic-vs-PCB comparison | ✅ | ECO preview (Update PCB), version diff |
| P1 | Reusable circuit blocks, diff pairs and buses, variants / DNP | ✅ | |
| P1 | Cross-probing schematic ↔ PCB ↔ 3D | ✅ | |
| P1 | Parametric component search, net classes at schematic level | ✅ | directives, catalog search |
| P1 | Interactive schematic comparison | ✅ | project diff |
| P1 | ➕ Pin / gate swap with back-annotation | ✅ | `PcbSwap.cpp` |
| P1 | ➕ Operating-point annotations on the schematic | ✅ | DC operating point overlay |
| P1 | ➕ Bus rip / entries, `DATA[0..7]` | ✅ | `Buses.cpp` |
| P1 | ➕ Sheet instancing with per-instance designators | ✅ | `Instances.cpp` |
| P2 | Draft schematic from a written spec, functional grouping, review rules, NL editing, reference architectures | ✅ | AI plans, `designReview`, MCP |
| P3 | Approved blocks, review / approval workflow, requirements traceability | ✅ | review comments, approvals per design revision (Design Review → Approvals, MCP `verify_approvals`), approved blocks (`schematic_block`: block files with approver and fingerprint, tamper check on place), requirements traceability (`verify_requirements`) |

## 3. Component, symbol and footprint libraries

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Symbol editor; footprint editor (pads, holes, copper, mask, paste) | ✅ | |
| P0 | Custom pad shapes and numbering | ✅ | rounded, chamfered, outline pads; numbering check |
| P0 | ➕ Per-pad mask / paste overrides, via-in-pad, thermal pads | ✅ | |
| P0 | Courtyard, silk, fab, assembly layers | ✅ | |
| P0 | Symbol ↔ footprint mapping, verified starter library | ✅ | 1,700+ catalog parts from KiCad / vendor libraries |
| P0 | ➕ Pin-1 and IPC-7351 zero orientation | ✅ | pin-1 check and mark; LAND_ZERO_ORIENTATION (pad 1 on the right = mirrored / turned pattern) |
| P0 | Library search, filters, tags, favourites | ✅ | parametric search; favourites (star, `fav:`) and tags (`tag:x`) per user |
| P0 | Standard library import / export | ✅ | KiCad, Eagle, Altium libraries |
| P0 | Footprint and symbol validation | ✅ | `checkLandPattern`, `checkSymbol` |
| P0 | ➕ Wizards for high-pin-count parts from CSV pin tables | ✅ | generators; Import Pin Table… / `library_create_part` `pinTable` (CSV / TSV, CubeMX pin lists) |
| P1 | Datasheet links, datasheet-driven footprints, pin-to-pad checks | ✅ | |
| P1 | 3D model attach / align / offset | ✅ | |
| P1 | Lifecycle, MPN, supplier links | ✅ | `Suppliers.cpp` |
| P1 | IPC-7352-aware land patterns | ✅ | IPC-7351 calculator |
| P1 | Library versioning / history, alternative footprints | ✅ | user footprint folder, version history (last 20 saves per footprint), shared library / 3D model folder ✅ |
| P1 | ➕ Library update notices with "update instances" preview | ✅ | File → Check Library Updates…, MCP `library_updates` (changes and placed count, then apply) |
| P1 | ➕ Parametric search across the library | ✅ | |
| P1 | ➕ Shared 3D model library | ✅ | `Model3DRegistry` per project; the shared library folder's 3D Models |
| P2 | AI datasheet extraction, footprint-vs-datasheet comparison | ✅ | pin tables from datasheets with page and confidence; footprint vs the datasheet's land pattern (`measureFootprint`, MCP `library_compare_footprint`: pads, pitch, pad size, span) |
| P2 | Library quality score, bulk edit, supplier-neutral catalog | ✅ | catalog, quality score 0 – 100 (`partQuality`), bulk edit of library metadata (MCP `library_bulk_edit`, placed parts follow) |
| P2 | ➕ Least / Nominal / Most density variants | ✅ | IPC calculator density |
| P3 | Company library, librarian roles, approved alternates, releases | ✅ | MCP `library_company`: a team folder of parts with draft / released / obsolete states, versions with history, release only by the librarians listed in `librarians.json`, approved alternates; used parts report whether they are still the released ones |

## 4. PCB placement and layout

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Outlines, cutouts, slots, mounting holes | ✅ | slot tool |
| P0 | Place, rotate, flip, align | ✅ | |
| P0 | ➕ Keep-outs and rule areas | ✅ | routing / pour keep-outs, height zones |
| P0 | Ratsnest, layer visibility, track / via / pad / zone editing | ✅ | |
| P0 | Pours (solid, priority, healing), thermal reliefs | ✅ | refill on demand |
| P0 | Stack-up, measurement, interactive DRC, highlight, undo, filters | ✅ | |
| P0 | ➕ Mechanical, documentation, user layers | ✅ | fab / courtyard / silk; `BoardSettings::graphics` on user-1 … user-4 (own Gerbers) |
| P0 | ➕ Logo / image import to silk or copper | ✅ | File → Import Logo… (raster → merged rectangles), MCP `pcb_graphics`; copper art DRC-checked |
| P0 | ➕ Test-point objects and net-tie footprints | ✅ | `TestPoint` (1.5 mm probe pad) and `NetTie-2` parts (`TESTPOINT` / `NETTIE` packages) |
| P1 | Stack-up materials, blind / buried / micro vias, back-drill | ✅ | |
| P1 | Diff pairs, rooms, channel replication, teardrops, cleanup | ✅ | rooms: MCP `pcb_rooms` (DRC_ROOM, place members into their room); channel replication: `pcb_layout_block` |
| P1 | Silk collision and board-edge rules | ✅ | |
| P1 | Panelisation with rails, fiducials, mouse bites, V-score | ✅ | `Panel.cpp` |
| P1 | ➕ Castellated edges | ✅ | |
| P1 | ➕ Copper balance / thieving, heavy copper | ✅ | `copperThieving` dots (Board Setup, MCP `pcb_copper_thieving`) in Gerbers, IPC-2581 / ODB++, 3D and coverage |
| P1 | ➕ Assembly-house height / side / orientation limits | ✅ | DFA packs |
| P2 | AI placement, decoupling suggestions, rigid-flex, HDI, multi-board, optimisation | ✅ | `placeDecoupling`, `improvePlacement` |
| P3 | Placement templates, approved layout reuse, multi-engineer partitioning | ✅ | MCP `pcb_layout_block` (replicate a channel's placement and copper onto other channels; save / apply an approved layout file); `pcb_partitions` (owner per board area; `check` lists edits inside other owners' areas against the base file) |

## 5. Interactive routing and autorouting

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Interactive routing, 45° and corner styles, any-angle and arcs | ✅ | |
| P0 | Walkaround, push-and-shove, width / clearance, vias, live completion | ✅ | |
| P0 | ➕ IPC-2152 / 2221 track-width calculator | ✅ | `PowerIntegrity` |
| P1 | Diff pairs, length tuning, net-class rules, BGA fan-out, multi-trace | ✅ | |
| P1 | Gloss, impedance widths, stitching / fencing, completion stats | ✅ | |
| P1 | ➕ Pin-swap-aware routing | ✅ | |
| P2 | Constraint-aware autorouter, return paths, HS constraints, phase tuning, escape | ✅ | |
| P2 | AI routing recommendations with before / after | ✅ | routing report; MCP `pcb_route_compare`: each strategy routed on a copy, before / after metrics and a recommendation ✅ |
| P2 | ➕ Coplanar waveguide geometries | ✅ | field solver `FieldGeometry::cpw` (grounded CPW, coplanar stripline), MCP `si_field_solver` `cpwGap` |
| P3 | Routing policies, batch routing over variants | ✅ | routing policies per net class (layers the autorouter keeps to, via limit; DRC ROUTE_POLICY_LAYER / _VIAS; MCP `pcb_routing_policies`); every variant checked on the one routing (MCP `pcb_variant_check`: open nets among fitted parts, overlaps of parts fitted together, alternates never fitted together) |

## 6. Electrical and physical verification

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | ERC; DRC width / clearance / edge / drill / annular | ✅ | |
| P0 | ➕ Courtyard overlap, hole-to-hole, slivers | ✅ | |
| P0 | ➕ Reference-designator overlap | ✅ | `DFA_REFDES_OVERLAP` (with a manufacturer pack) |
| P0 | Unrouted nets, shorts, zones / thermals, silk-to-pad, clickable list | ✅ | |
| P0 | Manufacturer rule presets, export-time validation | ✅ | DFM packs, verification gate |
| P0 | ➕ Netlist mismatch before export | ✅ | verification |
| P1 | Constraint manager, pairs / lengths, via counts, current capacity | ✅ | |
| P1 | Rule queries, incremental DRC, explanations, waivers | ✅ | |
| P1 | ➕ Creepage / clearance (IPC-2221, IEC 62368 / 60664) | ✅ | industry profiles |
| P1 | ➕ Mask sliver, mask bridge, acid trap | ✅ | |
| P1 | ➕ DRC on the panel | ✅ | `panelChecks`: PANEL_VSCORE_COPPER / _OUTLINE, PANEL_BITE_COPPER, PANEL_TAB_PART, PANEL_OVERHANG |
| P2 | SI / return path / PI / thermal / assembly checks, rule scripting | ✅ | rule scripting: custom DRC rules (Board Setup → Custom Rules, MCP `pcb_custom_rules`) |
| P3 | Release gates, reviewer waivers, traceable reports, org rule packs | ✅ | verification report, waivers, approvals tied to the design fingerprint, release package with evidence (`output_release`); org rule packs: MCP `pcb_rule_pack` / macOS Design → Team Rule Pack (one shared rules file, VER_RULE_PACK on drift) |

## 7. Simulation and analysis

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Connectivity validation, model readiness | ✅ | |
| P1 | SPICE engine: DC, AC, transient | ✅ | own MNA engine (SPICE3 devices) |
| P1 | ➕ Sweeps, noise, FFT, waveform viewer with cursors / math | ✅ | |
| P1 | Probes, vendor model import, ➕ SPICE netlist export | ✅ | |
| P1 | Guided templates, stack-up impedance calculator | ✅ | |
| P2 | Monte Carlo, differential impedance, SI pre-analysis, IBIS | ✅ | |
| P2 | ➕ Touchstone S-parameters, mixed-signal / MCU co-simulation | ✅ | firmware on the MCU |
| P2 | IR drop, current density, thermal estimates, result comparison | ✅ | |
| P3 | Field solver and thermal integrations, evidence tied to releases | ✅ | 2D field solver, board thermal map (Design → Thermal Analysis…, MCP `pcb_thermal`), evidence in every release (`output_release` evidence.json) |

## 8. 3D viewer and ECAD–MCAD

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Real-time 3D, views, all layers, STEP in / out, ➕ VRML / glTF | ✅ | |
| P0 | Model placement, cross-probing, visibility | ✅ | selection boxed in 3D |
| P1 | Enclosure reference, heights, collisions, keep-outs, zones, export checks | ✅ | |
| P1 | ➕ Outline / keep-outs from MCAD (DXF / STEP) with reimport | ✅ | IDX / IDF and DXF outline import |
| P2 | IDX / IDF, section / exploded views, photoreal render | ✅ | |
| P2 | Rigid-flex folding | ✅ | flex bend angle folds the 3D view, render_3d and the drawing |
| P2 | Mechanical revision comparison, connector / fastener collisions | ✅ | |
| P2 | ➕ Native FreeCAD / Fusion / SolidWorks / Onshape connectors | 🟡 | FreeCAD macro and Fusion script with placement back (IDF) ✅; SolidWorks / Onshape through STEP / IDX |
| P3 | Mechanical change approval, PLM | ⏳ | |

## 9. Manufacturing, DFM, DFA, test readiness

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Gerber, Excellon (PTH / NPTH), all layers, ➕ stencil | ✅ | |
| P0 | BOM, pick-and-place, assembly drawings, fab notes | ✅ | |
| P0 | One-click output job, package validation | ✅ | fabrication package |
| P0 | Built-in Gerber viewer | ✅ | File → Gerber Viewer… (Gerber X2 / RS-274X and Excellon, layered, any file kept on the board) |
| P0 | ➕ Versioned bundle with a README (layer order, stack-up, finish) | ✅ | `fab_notes.txt` lists every file, the stack-up, finish and drill chart |
| P1 | Gerber X2, IPC-2581, ODB++, ➕ IPC-D-356 | ✅ | |
| P1 | DFM / DFA, drill-to-copper, mask bridge, polarity, fab profiles | ✅ | |
| P1 | Panels, fiducials, release checklist, ➕ stack-up docs, per-variant outputs | ✅ | |
| P2 | Quote comparison, assembly cost, DFT, yield risk, package compare | 🟡 | cost roll-up ✅; fab order form + estimate from your price table ✅; Gerber-level package comparison (File → Compare Packages…, MCP `output_compare_packages`) ✅; live quote APIs ⏳ (partner keys) |
| P2 | ➕ Direct order / quote APIs (incl. Indian vendors) | 🟡 | File → Order Boards: order form, published-limit check, order page (JLCPCB, PCBWay, OSH Park, Eurocircuits, Lion Circuits, PCB Power) ✅; vendors' partner APIs ⏳ |
| P3 | Release signatures, change notices, MES / QMS | 🟡 | release manifest of SHA-256 hashes and a change notice against the previous revision (`output_release`) ✅; QMS evidence as a JUnit test report in every release ✅; MES ⏳ |

## 10. Documentation and drawings

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Fabrication drawing with drill table, dimensions, stack-up | ✅ | `<name>-fab_drawing.pdf`: drill map with a symbol per size, drill table, dimensions, stack-up, notes |
| P0 | Assembly drawing with designators, polarity, variants | ✅ | assembly SVG |
| P0 | Dimension tools | ✅ | ruler tool in the PCB editor; on the drawing, the DXF and the board view |
| P0 | Title blocks, revision tables, templates; PDF of drawings | ✅ | schematic PDF and the board's fabrication drawing PDF, both with the revision table |
| P1 | Customer / fab templates, layer legend, net-length and test-point reports, change report | ✅ | change report, layer legend, net-length and test-point reports, customer templates (title block → Customer Template) ✅ |
| P2 | Interactive HTML BOM / board viewer | ✅ | `exportHtmlBom` (one offline file, in the package) |
| P2 | 3D / exploded / section views in drawings | ✅ | fabrication drawing sheet 2 |
| P3 | Controlled drawing release, PLM numbering | 🟡 | drawings and pin maps released with the package, the revision and the manifest (`output_release`) ✅; PLM numbering ⏳ |

## 11. BOM, sourcing and cost

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Live BOM, MPNs, quantities, CSV, DNP / variants | ✅ | |
| P0 | ➕ Custom columns, grouping, templates | ✅ | grouping; MCP `output_bom` columns in any order (`exportBomColumns`); BOM layout travels in project templates |
| P1 | Supplier catalogs, price breaks, INR / USD, stock / lead time, EOL, alternates | ✅ | Nexar, DigiKey, Mouser |
| P1 | ➕ MOQ, packaging, attrition | ✅ | |
| P2 | Build cost, layer / material cost, second sources, risk scores, alerts | ✅ | cost roll-up and price / stock / lifecycle alerts ✅; per-line sources, risk score / level and catalog alternates (`highRiskLines`) ✅ |
| P2 | Indian suppliers, ➕ duty / GST / landed cost | ✅ | |
| P2 | ➕ Consolidated BOM across boards | ✅ | `exportBomColumns`: one BOM across every board with a Boards column, or one board's |
| P3 | AVL, procurement rules, RoHS / REACH, origin, ERP | 🟡 | compliance fields ✅; ERP ⏳ |

## 12. Pin planning and firmware co-design

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P1 | MCU / FPGA pin-planning view | ✅ | pin map export / import (MCP `schematic_pin_map`), graphical planner (right-click a part → Plan Pins…) |
| P1 | Pin swap within groups with back-annotation | ✅ | |
| P1 | Import CubeMX / XDC / QSF pin assignments | ✅ | `parsePinAssignments`: .ioc labels / signals, XDC, QSF, CSV → net labels on the part's pins |
| P2 | Export firmware headers / constraint files | ✅ | `exportPinMap`: C header (CubeMX-style for STM32), XDC, QSF, CSV |
| P2 | Bank-voltage checks while swapping, mating connector pin-outs | ✅ | harness pin-outs; pin / gate swaps never move a signal to an I/O bank on another VCCO supply (`crossesBankVoltage`) |
| P3 | Controlled pin-map releases | ✅ | the pin map ships with each controlled release (`output_release`, hashed in its manifest) |

## 13. High-voltage, power, RF, specialty

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P1 | HV classes with creepage / slots, heavy copper, thermal vias | ✅ | |
| P2 | RF impedance, stitching, antenna keep-outs, matching templates | ✅ | |
| P2 | Flex / rigid-flex stack-ups, bend radius, coverlay | ✅ | flex regions (IPC-2223 bend radius, no vias / parts / sharp corners in the bend) |
| P2 | Metal-core / ceramic | ✅ | |
| P2 | Isolation barriers as checkable objects | ✅ | barrier keep-outs (DRC_BARRIER_WIDTH / _PAD, no copper on any layer) |
| P3 | Certification evidence packs | ✅ | `output_release` evidence.json: design fingerprint, verification verdict, approvals, requirements trace, waivers, bench measurements |

## 14. AI-assisted engineering

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Optional AI, reviewable proposals, full undo, facts vs suggestions | ✅ | built-in model, MCP `project_undo` |
| P0 | ➕ Every extracted value cites its source and confidence | ✅ | each datasheet pin carries its page and a 0 – 1 confidence; the import notes list the pins below 70 % |
| P1 | Explain violations, suggested fixes, datasheet drafts, alternates, NL edits, review lists, change summaries | ✅ | |
| P2 | Draft schematics, placement, routing hints, protection, BOM optimisation, review, agent interface | ✅ | MCP |
| P3 | Org policies, audit of AI use, private models, knowledge bases | 🟡 | private on-device model, AI audit log of every live-endpoint tool call ✅; org policies, knowledge bases ⏳ |
| — | Safety: no silent net changes, diffs, re-run checks, fail safe offline | ✅ | `schematic_connect` refuses merges without `merge` |

## 15. Collaboration, versioning, review

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Local history / VCS, backup, diffable files | ✅ | |
| P0 | ➕ In-app Git: clone, commit, branch, tag | ✅ | File → Git… (status, history, commit, branch, tag, pull, push, clone) and the Git drivers |
| P1 | Named revisions, visual diff, comments, review summaries | ✅ | |
| P1 | Browser read-only viewer, cloud sync, share links | ✅ | read-only viewer: export "viewer" (one offline HTML file, every sheet and the board, pan / zoom) ✅; GitHub sync and share links (file, history, raw, Pages viewer; public repository = anyone, private = collaborators) ✅ |
| P1 | ➕ File locking / check-out | ✅ | `<file>.lock` beside an open project: who has it open, where, since when (Live Collaboration does not lock) |
| P2 | Approvals, branch compare / merge, integrations, shared libraries, co-editing | ✅ | approvals, in-app Git branches, three-way merge, shared library folder, co-editing |
| P3 | Permissions, sign-off, audit, identity | 🟡 | sign-off (approvals), AI audit log ✅; permissions and identity ⏳ (need a server) |

## 16. Usability and learning

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | First-project tour, palette, shortcuts, searchable libraries, snapping | ✅ | |
| P0 | Dark / light themes | ✅ | three app themes, 14 canvas schemes |
| P0 | High-contrast, colour-blind-friendly layer colours | ✅ | Settings → Appearance → Layer Colours: Okabe–Ito palette |
| P0 | Context help, offline docs, actionable errors, ➕ hover details | ✅ | |
| P1 | Beginner / pro modes | ✅ | Settings → Beginner mode: four-step guide bar, library workspace hidden |
| P1 | Keyboard presets for KiCad / Altium / Eagle users | ✅ | KiCad, Altium and Eagle canvas presets (Eagle: the first two letters of its commands — n e NET, a d ADD, r o ROUTE, r a RATSNEST), two-key sequences |
| P1 | Tutorials, pre-manufacturing checklist, examples, layouts, accessibility | ✅ | |
| P1 | ➕ Migration guides, ➕ pen / trackpad gestures | ✅ | trackpad; `docs/MIGRATING.md` |
| P2–P3 | Personalised shortcuts, lessons, org training | ⏳ | |

## 17. Import, export, interoperability

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Netlist import / export | ✅ | export; import of KiCad, Protel / Tango and CSV netlists (File → Import Netlist…) |
| P0 | PDF / SVG / PNG documentation | ✅ | |
| P0 | DXF import of outlines, ➕ DXF export | ✅ | `Documents.cpp`; File → Import Board Outline (DXF)…, Export → DXF |
| P0 | STEP in / out, BOM and fab files | ✅ | |
| P1 | KiCad project import (schematic + board) | ✅ | libraries, schematic sheets and boards (.kicad_pcb: placement fitted to the pads, tracks, arcs, vias, zones, outline, fidelity report), hierarchical sub-sheets ✅ |
| P1 | EasyEDA / Eagle projects | ✅ | Eagle libraries and schematics ✅; EasyEDA through its Protel netlist export |
| P1 | Import-fidelity report, library mapping | ✅ | library import notes; netlist / schematic import `fidelity` (built-in / catalog / mapped / generic parts, `knownPercent`) and `map` (their part → catalog part or kind) |
| P1 | ➕ LTspice / SPICE netlist import | ✅ | SPICE models; LTspice schematics (.asc) into a schematic |
| P1 | ➕ Gerber / ODB++ import as a reference layer | ✅ | Gerber, drill and ODB++ layer features files (saved with the project) |
| P2 | Altium libraries, IDF / IDX | ✅ | |
| P2 | PADS / OrCAD routes, batch conversion, ➕ HyperLynx export | ✅ | batch conversion `sieda-cli --convert` (KiCad, Eagle, LTspice, Protel / PADS / OrCAD netlists, KiCad boards); routes (tracks, arcs, vias) from PADS ASCII, HyperLynx and IPC-2581 (Allegro / OrCAD PCB Editor) fitted onto the placed board (`importRoutes`, MCP `pcb_import_routes`, File → Import Routes…); HyperLynx `.hyp` export (outline, stack-up, pad stacks, pins, tracks, arcs, vias, pours) |
| P3 | Enterprise migration, PLM / ERP, archival | ⏳ | |

## 18. Scripting, automation, extensibility

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Stable APIs, headless validation and export | ✅ | C API, `sieda-cli`, `sieda-mcp` |
| P0 | ➕ API semantic versioning | ✅ | MCP protocol version; C API `SIEDA_API_VERSION_*` and `sieda_api_version()` with a written semver policy |
| P1 | Python API | ✅ | `python/sieda.py`: every MCP tool as a method (standard library only; CTest `sieda_python_api`) |
| P1 | CLI ERC / DRC, automated outputs | ✅ | |
| P1 | CI templates (GitHub Actions / GitLab) | ✅ | `docs/ci/sieda-check.yml` (GitHub Actions), `docs/ci/sieda-check.gitlab-ci.yml` (GitLab) |
| P1 | ➕ Event hooks | ✅ | after-open / save / verify / export scripts in Application Support/SiEDA/Hooks |
| P2 | Plug-ins, custom rules / reports, agent interface, webhooks, sandboxing | ✅ | MCP agent interface, plug-ins (Design → Plug-ins), custom rules, plug-in sandbox (no network, writes only to the project copy) ✅ |
| P3 | REST, PLM / ERP, plug-in governance | ⏳ | |

## 19. Enterprise, security, compliance

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | Secure storage, privacy, backups, dependency security | ✅ | Keychain keys, sandboxed MCP, local only |
| P0 | ➕ Signed installers with auto-update | ✅ | release workflow: Developer ID signed, notarised DMG (secrets), Help → Check for Updates… / daily check |
| P0 | ➕ Vulnerability-disclosure process | ✅ | `SECURITY.md` |
| P0 | ➕ Telemetry opt-in | ✅ | no telemetry leaves the machine |
| P1–P3 | Teams, RBAC, audit, licensing, SBOM, SSO, self-hosting, ITAR | ⏳ ⚪ | |

## 20. Performance, reliability, QA

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P0 | No data loss, deterministic connectivity, geometry, ERC / DRC regression tests | ✅ | 1.24 M checks |
| P0 | Golden tests for Gerber / drill / assembly, integrity checks, crash diagnostics | ✅ | |
| P0 | ➕ Byte-identical outputs | ✅ | deterministic routers and fills |
| P1 | Incremental DRC / rendering, large-board tests, fuzzing, profiling, OS matrix | ✅ | CTest budgets, libFuzzer, Ubuntu / macOS CI |
| P1 | ➕ GPU rendering, ➕ multithreaded fill / DRC / 3D | ✅ | threaded fills and routing; Settings → GPU canvas rendering (Metal) |
| P2 | Large-design optimisation, stress tests, ➕ public benchmarks | ✅ | `sieda_route_bench` |
| P3 | Signed / reproducible builds, change control, LTS | 🟡 | signed release, SBOM, byte-identical CLI rebuild ✅; LTS ⏳ |

## 21. Hardware bring-up and test

| P | Item | Status | Where / what is missing |
|---|---|---|---|
| P1 | Test-point report for fixture builders | ✅ | `exportTestPointReport` (CSV, in the package) |
| P1 | Interactive BOM / board viewer for hand assembly | ✅ | `exportHtmlBom` |
| P1 | Bring-up checklist from rails, clocks, interfaces | ✅ | `bringUpChecklist` (Markdown, in the package) |
| P2 | Flying-probe / bed-of-nails export | ✅ | IPC-D-356 ✅; test-point report with the nearest probe on the same side and its probe class (100 / 75 / 50 mil) ✅ |
| P2 | Measured results and field failures linked to the design | ✅ | bench measurements with limits on the design revision (MCP `verify_measurements`), in the release evidence |
| P3 | Test-management / QMS integration | 🟡 | verification, requirements trace and bench measurements as a JUnit XML test report (File → Export, MCP `output_export` `junit`, `sieda-cli`, every controlled release, CI templates) that test-management and QMS tools import ✅; direct links to a named QMS ⏳ (its API) |

## 22–23. Editions, monetisation, community

All items are business, licensing or service work (⚪): free tier without limits (SiEDA has no board-size or layer
limits today), regional pricing, education programmes, issue tracker, forum, partner programmes.

## 24–26. Phases, selling points, non-negotiables

| Non-negotiable | Status |
|---|---|
| Never lose or silently corrupt a design (atomic saves, recovery) | ✅ |
| Deterministic, validated manufacturing outputs | ✅ |
| Connectivity is the source of truth (no silent net changes) | ✅ |
| Works fully offline | ✅ |
| Open, documented file format | ✅ (`docs/FILE_FORMAT.md`) |
| Free tier usable for real boards | ⚪ |

---

## Done in this round

- [x] DXF export (outline, cutouts, holes, drills, silk / fab lines, courtyards) and DXF outline import — §17 P0, §8 P1
- [x] Test-point report (CSV: designator, net, x, y, side, pad size, probe access; nets without access) — §21 P1
- [x] Interactive HTML BOM and board viewer (one offline file) — §21 P1, §10 P2
- [x] Bring-up checklist (shorts, rails with expected voltage, clocks, resets, programming and buses) — §21 P1
- [x] All four in the fabrication package — §9 P0
- [x] KiCad preset for the canvas tool keys — §16 P1
- [x] GitHub Actions template for headless checks and outputs — §18 P1
- [x] Reference-designator overlap check `DFA_REFDES_OVERLAP` — §6 P0
- [x] `SECURITY.md` vulnerability-disclosure process — §19 P0
- [x] Already there (now recorded): drill chart and README-style notes in `fab_notes.txt` — §9, §10

## Pending (bigger — one by one)

1. ~~Drawn drill map with drill symbols on the fabrication drawing (§10 P0)~~ ✅
2. ~~Built-in Gerber / drill viewer and Gerber import as a reference layer (§9 P0, §17 P1)~~ ✅
3. ~~KiCad project import, sub-sheets, EasyEDA / Eagle projects, LTspice schematics (§17 P1)~~ ✅
4. ~~Netlist import (§17 P0)~~ ✅
5. ~~Dimension tools and a board drawing PDF with title block and revision table (§10 P0)~~ ✅
6. ~~Schematic revision table and net-tie symbol / footprint (§2 P0)~~ ✅
7. ~~Board test-point object and logo / image import to silk or copper (§4 P0)~~ ✅
8. ~~User-defined documentation layers (§4 P0)~~ ✅
9. ~~Panel DRC (§6 P1)~~ ✅
10. ~~Python API~~ ✅ (over MCP), ~~event hooks~~ ✅ (§18 P1)
11. ~~Settings export / import, user project templates (§1 P1)~~ ✅
12. ~~High-contrast / colour-blind layer palette, beginner / pro modes (§16)~~ ✅
13. ~~MCU / FPGA pin planner (graphical) with CubeMX / XDC / QSF import and firmware header export (§12)~~ ✅
14. ~~Copper thieving generation, CPWG geometry, flex bend-radius rule, isolation-barrier object (§4, §5, §13)~~ ✅
15. ~~Library update notices, library versions, tags / favourites, CSV pin-table wizard, IPC zero-orientation check (§3)~~ ✅
16. ~~Consolidated multi-board BOM, custom BOM columns, price alerts~~ ✅ (§11)
17. ~~In-app Git~~ ✅, ~~browser viewer~~ ✅, ~~file locking~~ ✅, approvals (§15)
18. Fixed-point (nm) geometry database (~~user coordinates on a 1 nm grid~~ ✅, ~~saved copper and the fabrication package on the grid~~ ✅; router / pour arithmetic stays floating point by design), ~~inch units, file-format reference~~ ✅ and ~~migrator~~ ✅ (§1)
19. ~~Rigid-flex folding in 3D~~ ✅, native MCAD connectors (~~FreeCAD, Fusion~~ ✅; SolidWorks, Onshape) (§8)
20. Fab quote / order (~~order form, price-table estimate, order page~~ ✅; partner APIs), ~~Gerber-level package comparison~~ ✅ (§9 P2)
21. Windows / Linux app (§1 P0): ~~SiEDA Studio (views, checks, outputs, every tool, GitHub sync)~~ ✅; interactive drawing editors
22. ~~Signed installers and auto-update, SBOM, reproducible builds (§19, §20)~~ ✅
23. Enterprise: teams, RBAC, SSO, audit, self-hosting, PLM / ERP (§15, §19 P3)
