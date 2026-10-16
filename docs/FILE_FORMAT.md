<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# SiEDA project file format

A `.siedaproj` file is UTF-8 JSON written by `sieda_project_save_json`. It is indented, with object keys in sorted
order, so files diff cleanly in Git (`docs/TEAM.md` covers the merge drivers).

## Versions and compatibility

- `"format": "sieda-project"` marks the file, and `"version"` is the format version, `kProjectFormatVersion` (now 1).
  The version is raised only when an older SiEDA would misread a newer file.
- New features add keys and write them **only when used**. A project that does not use a feature loads and saves
  byte-identically in versions before and after it.
- Readers are tolerant: unknown keys are ignored, missing keys take their defaults, and values are clamped to sane
  ranges. Hostile files are fuzzed: the JSON parser stops at 256 levels of nesting.
- **Newer files.** On open, the core compares the file with what this version would write back
  (`Project::loadNotes`, C `sieda_project_load_notes`). The app warns before anything is lost in two cases:
  - the format version is higher than this build reads;
  - the file has non-empty settings, at the top level or one level down, that this build would drop on save.

  Save a copy instead of overwriting the file.
- **Upgrading files.** `sieda-cli --upgrade a.siedaproj b.siedaproj …` rewrites files in this version's format,
  for example a folder of older projects before they go into Git. It reports `already current` or `upgraded`.
  A file that load notes say would lose settings is left alone, and the exit status is 1.

## Top level

| Key | What |
|---|---|
| `format`, `version`, `name`, `requirements` | header, format version, project name, the design brief |
| `components`, `wires`, `sheets`, `activeSheet` | the schematic: parts with their pins and placement, wires, hierarchical sheets |
| `customParts` | the project library: every custom / catalog part the design uses (`CustomPartSpec` JSON) |
| `board` | board settings: size, outline, stack-up, rules (see below) |
| `tracks`, `vias`, `zones`, `tamperMeshes` | copper: tracks (straight or arc), vias, pour rules, tamper meshes |
| `pcbSync` | the Update PCB baseline (forward ECO) |
| `industry`, `robotPlatform`, `ecuType`, `aerospaceMission`, `navalPlatform`, `medicalClass`, `retailDevice`, `applianceType`, `memoryDesign` | design segment profiles |
| `buildQuantity` | quantity for BOM costing |

Written only when used: `titleBlock`, `revisions`, `variants`, `reviewComments`, `referenceLayers`, `models3d`,
`partModels`, `memoryLimits`, `signalIntegrity`, the extra boards of a multi-board project, and others.

## `board`

Always written:
- size, thickness and layer count;
- track, clearance and via defaults, and the fabrication minimums;
- `outline` and `holes`;
- stack-up material, construction, copper weight, finish and coating;
- impedance targets;
- HDI / backdrill / via-in-pad options;
- length tuning.

Written only when used:

| Key | What |
|---|---|
| `cutouts` | board cutouts |
| `dimensions` | drawing dimensions |
| `keepouts` | routing keep-outs (`pours`, `barrier` flags) |
| `pairRules`, `lengthRules`, `matchGroups` | differential-pair, length and match-group rules |
| `netWidths`, `netClearances` | per-net width and clearance overrides |
| `autorouter` | autorouter strategy options |
| `panel` | production panel |
| `dfmPack`, `dfmOverrides` | manufacturer rule pack and the fab's own values |
| `mechanical` | height limits, height zones, enclosure, rigid-flex `flex` areas |
| `thieving` | copper thieving settings |
| `graphics` | logos and user-layer artwork |

## Units and coordinates

- **Board:** millimetres, origin top-left, Y down. Gerbers flip Y to the usual bottom-left origin.
- **Schematic:** grid units (10 per 0.1 inch pin pitch).
- **Angles:** degrees, counter-clockwise.

## Reading and writing it yourself

Prefer the C API (`Core/include/sieda/sieda_c.h`) or the MCP server (`docs/MCP.md`). They validate every change.
If you edit the JSON directly, keep the keys you do not understand: the app warns when it would drop them.
