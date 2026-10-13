# Moving to SiEDA from KiCad, Altium or Eagle

## Bringing designs and libraries across

| From | Schematic / netlist | Libraries | Board |
|---|---|---|---|
| KiCad | `.kicad_sch` sheets or a KiCad netlist: File → Import Netlist… | `.kicad_sym`, `.kicad_mod` (Component Library → Import Library…) | Gerbers as a reference layer (File → Import Gerber as Reference Layer…); `.kicad_pcb` import is not built yet |
| Altium | a Protel netlist (Design → Netlist For Project → Protel) | `.SchLib`, `.PcbLib`, `.IntLib` | Gerbers as a reference layer |
| Eagle | a netlist exported as CSV | `.lbr` | Gerbers as a reference layer |
| LTspice | `.asc` schematics | — | — |
| Anything else | a CSV netlist (ref, value, footprint, net, pin) | a CSV pin table (Import Pin Table…) | Gerbers as a reference layer |

After an import, run File → Check Library Updates… so parts that match the catalog use its current version. Then run
Update PCB.

## Habits that carry over

- **Keys:**
  - Settings → Shortcuts → Canvas Key Preset sets the canvas tool keys to the KiCad or Altium layout.
  - Altium's two-key Place sequences become their second key: W wire, N net label, O power port, T track, L flip.
  - Menu shortcuts can be set one by one.
- **Panning and zooming:** trackpad pinch and two-finger scroll work on every canvas. The space bar or the middle
  button drags.
- **Rules:**
  - Board Setup holds the design rules, net classes, pair rules and length rules.
  - Manufacturer Rules applies a fab's minimums and adds DFM / DFA checks (JLCPCB, PCBWay, OSH Park, Eurocircuits,
    IPC Class 3).
- **Output:** the fabrication package bundles a lot in one export:
  - Gerber X2 and the job file, drills, IPC-356 netlist;
  - IPC-2581, ODB++;
  - BOM, pick and place, the fabrication drawing PDF;
  - STEP and the panel.

  Upload `<name>-gerbers.zip` to the fab.

## Things that work differently

- **One file per project:** schematic, board, library parts and rules together, as JSON (`docs/FILE_FORMAT.md`).
  Library parts are copied into the project, so a project opens the same everywhere.
- **The DRC runs electrical and manufacturing checks together** (clearance, voltage spacing, DFM, 3D clearance,
  panel). Verification adds the simulation-based checks before fabrication export.
- **Any AI client can drive the whole flow** through MCP (`docs/MCP.md`). The app's live endpoint makes each AI edit
  one undo step.
