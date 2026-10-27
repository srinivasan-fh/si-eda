<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

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

**File → Git…** works on the folder holding the project with the Mac's own `git`: it lists the changes and the last
30 commits, **Save and Commit All** saves the project and commits everything with your message, and the branch picker,
**New Branch**, **Tag**, **Pull** (fast-forward only) and **Push** do the rest. **Create Repository Here** starts one,
and **Clone…** fetches a repository into a folder you pick and opens the project in it. Branch and tag names are
checked before they reach git, and git never waits on a password prompt (use a credential helper or SSH keys).

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
- **Theirs come in by themselves:** SiEDA watches the file (macOS reports a change the moment it lands; a 2 s check
  covers network shares that report none). When a teammate has saved, it three-way merges
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

## Check-out lock and event hooks

- **Lock:** while a project is open, SiEDA keeps `<name>.siedaproj.lock` beside it, saying who has it open, on which
  Mac and since when. Opening a file someone else has locked asks first: "This project is open elsewhere".
  - A lock is ignored when its app on this Mac has quit, or when it is older than 12 hours.
  - Live Collaboration does not lock, because co-editing is its point.
- **Event hooks:** Settings → Open Event Hooks Folder opens Application Support/SiEDA/Hooks.
  - An executable there named `after-open`, `after-save`, `after-verify` or `after-export` runs in the background
    after that event, with `SIEDA_EVENT`, `SIEDA_PROJECT` and `SIEDA_OUTPUT` (the fabrication folder) set.
  - Typical uses: commit to Git, copy the fabrication package to a share, post to a chat.
  - Output goes to `hooks.log`, and a hook is stopped after a minute.
  - Only that folder is used, so a project never brings scripts of its own.
- **Code:** `SiEDA/App/ProjectHooks.swift` (`ProjectLock`, `ProjectHooks`).

## Approvals

**Design Review → Approvals**: **Approve This Revision** records who approved the design (name, role, date) against
its fingerprint (16 hex digits of everything saved except approvals and review comments; MCP `verify_approvals`). Any
later change makes the approval stale, shown with a clock, so a release is approved only while every approval is
current. Approvals are saved in the project only when there are any.

## Controlled release

MCP `output_release` (`dir`, `revision`, optional `previous`) writes a release folder: the fabrication package, the
project as released, the verification report, `evidence.json` (design fingerprint, verdict, approvals of this revision,
requirements trace, waivers, bench measurements), a change notice against the previous revision and
`MANIFEST.sha256` (check it with `shasum -a 256 -c MANIFEST.sha256`). `ready` is true when the verification did not
fail, at least one approval is current and no requirement is missing its parts.

- **Requirements** (`verify_requirements`): each requirement names the parts that meet it; the trace says which are
  traced, which name a part that is gone, and which parts no requirement names.
- **Bench measurements** (`verify_measurements`): values measured on built boards with min / max limits, stamped with
  the design revision they were taken on.
- **Approved blocks** (`schematic_block`): save parts and their wiring as a block file signed off by an approver;
  placing it again says whether it is still the approved block (a hand-edited file is not).

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

## Layout reuse, partitions and the company library

- **Layout reuse** (MCP `pcb_layout_block`): `replicate` copies one channel's placement and the copper inside it onto
  other channels of the same parts (nets follow the pins); `save` writes an approved layout file and `apply` lays it
  again and says whether it is still the approved one. Copper that would touch another net is skipped and counted.
- **Partitions** (MCP `pcb_partitions`): give each engineer an area of the board. `check` with your name and the base
  file (the version you pulled) lists what changed inside areas owned by someone else, before you push.
- **Company library** (MCP `library_company`): a folder of parts the team shares. Parts are published as drafts,
  released by a librarian (the names in the folder's `librarians.json`), retired as obsolete with approved alternates,
  and every saved version stays in `.history/`. Placing a part says whether it is still the released one.

In the macOS app these are menu commands working on the selected footprints: **Design → Layout** (Replicate Layout,
Save / Apply Layout Block, Room from Selection, Place Parts in Their Rooms, Remove Rooms, Partition from Selection,
Check Partitions Against Base, Routing Policy, Check Variants) and **File → Company Library** (Show Parts, Publish
Selected Part, Place Part, Release, Mark Obsolete, Set Approved Alternates, Choose Library Folder). Rooms (teal) and
partitions (purple, with their owner) are drawn on the board; DRC_ROOM and ROUTE_POLICY_* results jump to the part.
**File → Company Library → Show Parts…** opens the library window: search the folder's parts, see each one's status,
version (and how many earlier versions are kept), manufacturer, package, who released it and its approved alternates,
and place, release, retire or set alternates for the chosen part. **Design → Layout → Rooms and Partitions…** lists
every room (its parts) and partition (its owner) with its corners in mm to rename, move, resize, add or remove.
Each command is one undo step.

- **Team rule packs** (MCP `pcb_rule_pack`, macOS **Design → Team Rule Pack**): the board's design rules — track and via
  sizes, clearances and fab minimums, net widths and clearances, pair and length rules, DFM pack and overrides, custom
  rules, routing policies — saved as one file (`sieda.rules/1`, with who approved it) and applied to any board; the
  board's size, outline, stack-up and copper stay as they are. The board remembers the pack it took, and Verify Design
  warns when its rules drift from it later (VER_RULE_PACK). Compare lists the rules that differ from a pack file.

