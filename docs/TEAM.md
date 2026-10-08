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

## Merge two versions (three-way)

When two people edit copies of the same design, SiEDA merges them against their common ancestor:

```bash
echo '*.siedaproj merge=sieda' >> .gitattributes
git config merge.sieda.driver "/path/to/sieda-mcp --merge %O %A %B"
```

How each kind of data merges:
- **Fields** merge one by one.
- **Lists with ids** (parts, wires, sheets, custom parts, review comments) merge item by item, so a part one side
  added and a value the other side changed both land.
- **Copper** (tracks, vias, pours) merges as a set. Both sides' additions and removals apply, and the same track added
  on both sides appears once.
- **Conflicts:** a field both sides changed differently keeps **ours**. The driver lists it on stderr and exits 1, so
  Git marks the file as conflicted.
- **Edited and deleted items:** an item one side deleted and the other changed is kept, and listed as a conflict.

The merged file always loads, and after a merge with copper changes the DRC catches any shorts between the two sides'
tracks. The same merge is available as MCP `project_merge` (base, ours, theirs, path) and C
`sieda_merge_projects` / `sieda_merge_project_files`.

## Live collaboration (co-editing a shared file)

**File → Live Collaboration** turns a shared project file into a live session — no server, just a folder every
teammate can reach: iCloud Drive, Dropbox, OneDrive, a network share or a Git working copy.

- **Your edits go out within a second or two:** each edit is saved shortly after you make it (edits in quick
  succession share one save).
- **Theirs come in by themselves:** SiEDA checks the file every 2 s. When a teammate has saved, it three-way merges
  their version into your open design (the same merge as above, base = the file as you last read or wrote it) as one
  Undo step. Parts, wires, tracks, vias and settings from both sides are kept (when both of you added a part at
  the same moment, theirs gets a fresh id — and the next free designator if yours already uses it — and its wires
  follow it); a field you both changed differently
  keeps your value, and the status-bar badge turns amber and lists it (hover).
- **Who is here:** each window announces itself in a small presence folder beside the file
  (`.<name>.siedaproj.presence/`), refreshed every 15 s; the status bar shows the others' names while they have the
  file open (gone after a minute without a heartbeat).

Turn it off to go back to saving by hand. Code: `SiEDA/App/LiveCollaboration.swift` (`LiveCollaboration`,
`LiveCollaborationIndicator`), merge `EDAEngine.mergeProjects` → `sieda_merge_projects`. Test:
`LiveCollaborationTests.testTwoWindowsMergeEachOthersSaves`.

## Design review

**File → Design Review…** lists comments pinned to a part (or to a place on the board or a sheet). Open comments are
listed first. Each comment can be replied to, resolved, reopened or deleted, and every action is one Undo step.
**Copy as Markdown** gives the review for a ticket or an e-mail.

Comments are saved in the project file only when there are any. They merge like parts, and the diff lists the ones
added or resolved. They are also available as:
- **MCP:** `project_review_comments` and `project_review_comment` (add / reply / resolve / reopen / delete);
- **C API:** `sieda_review_json` and `sieda_review_command`;
- **export:** `sieda_export(p, "review")` (Markdown).

## Compare variants

The schematic's variant menu has **Compare Variants…**. It shows one row per part that any variant changes, with
its value or **DNP** in each variant:
- **Red** means the part is not fitted.
- **Orange** means its value differs from the base design.

The last row counts the parts fitted in each variant. Over MCP this is `schematic_variant_matrix`; in C it is
`sieda_variant_matrix_json`.

Code: `Core/src/ProjectDiff.cpp` (`diffProjects`, `diffText`, `mergeProjects`, review comments, `variantMatrix`), `Core/mcp/main.cpp` (`--diff`,
`--git-diff`, `--merge`), app `SiEDA/App/DesignStore+Team.swift`, `SiEDA/Views/Common/TeamViews.swift`.
