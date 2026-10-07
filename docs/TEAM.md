# Team work: compare versions, Git, variants

## Compare two versions

**File → Compare with Another Version…** picks an earlier `.siedaproj` and lists every change from it to the open
design:

```
1 parts added, 0 removed, 2 changed; 1 nets changed; 3 nets re-routed
+ C12 100n C_0402
~ R5 value 10k → 4k7
~ U1 position [12,8] → [15,8]; rotation 0 → 90
~ net SDA +U3.5 −U2.7
~ copper SDA: 41.2 → 47.9 mm, vias 2 → 3
~ board layers 4 → 6
+ variant Lite
```

The diff covers these areas:
- **Parts** are matched by designator. A change can be to the value, footprint, MPN, DNP flag, or placement
  (position, rotation, side).
- **Nets** are matched by their pins. A net that only changed its name is reported as a rename. Otherwise the
  diff lists the pins that joined (`+`) or left (`−`) the net.
- **Copper** is reported per net: track count, routed length and via count.
- **Board settings** cover layers, size, thickness, outline, mounting holes, pours and solder mask.
- **Variants** are listed when added, removed or changed.

The same diff is available in three other places:
- **MCP:** `project_diff` (`before`, and optionally `after`; by default `after` is the open project) returns the
  JSON and the text.
- **C API:** `sieda_diff_projects(before_json, after_json, as_text)`.
- **Command line:**

```bash
sieda-mcp --diff old.siedaproj new.siedaproj          # the text; exit status 1 when they differ
sieda-mcp --diff old.siedaproj new.siedaproj --json   # the JSON
```

## Git

Project files are JSON, so Git stores and merges them as text. To make `git diff` and `git log -p` show the design
changes instead of JSON lines, register SiEDA as the diff driver once:

```bash
echo '*.siedaproj diff=sieda' >> .gitattributes
git config diff.sieda.command "/path/to/sieda-mcp --git-diff"
```

A file Git reports as added or deleted (`/dev/null`) is compared with an empty project.

## Compare variants

The schematic's variant menu has **Compare Variants…**. It shows one row per part that any variant changes, with
its value or **DNP** in each variant:
- **Red** means the part is not fitted.
- **Orange** means its value differs from the base design.

The last row counts the parts fitted in each variant. Over MCP this is `schematic_variant_matrix`; in C it is
`sieda_variant_matrix_json`.

Code: `Core/src/ProjectDiff.cpp` (`diffProjects`, `diffText`, `variantMatrix`), `Core/mcp/main.cpp` (`--diff`,
`--git-diff`), app `SiEDA/App/DesignStore+Team.swift`, `SiEDA/Views/Common/TeamViews.swift`.
