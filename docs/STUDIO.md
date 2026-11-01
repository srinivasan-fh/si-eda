<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# SiEDA Studio on Linux and Windows, and GitHub cloud sync

SiEDA Studio is SiEDA on Linux and Windows (it runs on macOS too). The design engine serves the app's window to
your web browser, on this computer only. Nothing goes over the network unless you sync with GitHub.

## Install and start

| System | Download | Start |
|---|---|---|
| Linux (x86-64) | `sieda-linux-x86_64.tar.gz` from the release | Unpack it, then run `./sieda-studio`. To get a menu entry, copy `sieda-studio` to `~/.local/bin` `sieda-studio.desktop` to `~/.local/share/applications` and `sieda-studio.png` to `~/.local/share/icons/hicolor/256x256/apps`. |
| Windows 10 / 11 (x64) | `sieda-windows-x86_64.zip` from the release | Unpack it and run `sieda-studio.exe` (it carries the SiEDA icon and version details). The files are not signed yet, so Windows SmartScreen asks first: choose **More info → Run anyway**. |

Each archive also contains `sieda-cli` and `sieda-mcp`. Every program is one self-contained file: there are no
runtimes or libraries to install.

You can also build it from source on any of the three systems:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target sieda-studio
```

On Windows, build in an MSYS2 MinGW-64 shell (`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake`).

```
sieda-studio [--root <dir>] [--project <file>] [--port <n>] [--no-browser] [--library-dir <dir> …]
```

| Option | Meaning |
|---|---|
| `--root` | Folder for opening projects and writing outputs (default: your home folder). |
| `--project` | Project to open at start. |
| `--port` | Port to listen on (default: any free port). |
| `--no-browser` | Print the address instead of opening the browser. |
| `--library-dir` | A KiCad library folder (symbols / footprints) to search and place parts from; repeatable, read-only ([LIBRARY_IMPORT.md](LIBRARY_IMPORT.md)). |

## What it does

| Area | What you can do |
|---|---|
| **Menus** | A menu bar like the macOS app's: **File** (new, open, save, examples, every import — netlist, KiCad board, DXF outline, IDF / IDX, routes, Gerber reference, logo, library — and export — formats, fabrication package, Gerbers, placement, schematic PDF, 3D model, controlled release, board order), **Edit** (Undo, Redo, find / replace, annotate, align, rename net), **Schematic** (ERC, validation, sheets, buses, net classes and rules, directives, no-connects, pin maps, blocks, variants), **Design** (Auto Place, Fit Board, Auto Route, DRC, thermal vias, thermal analysis (board temperature map), Update PCB, routing strategy comparison, Layout: replicate, layout blocks, rooms and partitions, routing policy, variants; Team Rule Pack: save, apply, compare; industry profile, design rules, Verify), **Board Setup** (board, outline, stack-up, HDI, manufacturer rules, custom rules, net widths, length and pair rules, keep-outs, mechanical limits, production panel, copper thieving, boards, signal integrity, field solver), **Team** (Company Library window, compare and merge versions, design review, approvals, requirements, measurements, waivers, cloud sync) and **Help** (command palette, shortcuts, settings). Each command opens a form made from its settings; Run shows its result. **Ctrl+K** opens the command palette: every menu item and every engine command by name. |
| **File** | New, Open (lists the projects under the root folder), Save, Save As, built-in examples, Undo (Ctrl+Z), Redo (Ctrl+Shift+Z / Ctrl+Y), Ctrl+S. |
| **Team** | **Company Library…**: the team folder's parts with status, version, earlier versions, package, who released them and their alternates; place, release, retire, set alternates, publish a project part. **Rooms and Partitions…**: every room (its parts) and partition (its owner) with its corners in mm, to edit, add or remove. |
| **Help** | **Lessons…**: step-by-step lessons checked against the design (built-in, the organisation's `--lessons-dir`, a team folder's `Lessons/`), with Check My Work and Show Me ([LESSONS.md](LESSONS.md)). A menu item you choose four times is offered a quick key Alt+1…9. |
| **Layout** | The macOS app's window: a menu bar, then a toolbar with the project name, one workspace picker drawn with the macOS icons (Schematic, Library, PCB Layout, 3D Viewer, Simulation, Design Checks, BOM) and Undo / Redo / Check. Each editor has its tool strip (icons; the tooltip names the tool and its key), an options bar and a side panel, as on the Mac. Studio starts in the workspace last used and reopens the project last opened or saved. |
| **Settings** | **General**: reopen the last project and workspace at start, projects folder (Open lists it first, Save As proposes it), save projects automatically after a 3–60 s pause, a new project's copper layers / track width / clearance, mouse wheel pans (Ctrl + wheel zooms) and reverse zoom, export / import settings (a `sieda.studio-settings/1` file) and Reset All Preferences. **Appearance**: Midnight Navy (the default), Matrix Green, Graphite or Light. **Language**: the macOS app's 20 languages (menus, tabs, buttons, dialogs and hints; engineering terms, part numbers, units and check messages stay in English). All are remembered in the browser. |
| **Schematic** | Tool strip along the top, the Devices list (with a search field) and Standard Parts (catalog search) down the left, the selected part on the right. Every sheet (wheel zoom, drag to pan, double-click fits). Edit by hand: **Select** (drag a part to move it, R rotates, Delete removes, value / rotate / mirror in the side panel), **Wire** (click a pin, then another), **Label** and **Ground** (click a pin), **Place** (a built-in part or a catalog search hit, then click the sheet; P). ERC. |
| **Board** | Tool strip down the left of the board, as on the Mac. All layers or one (inner layers too). Auto Place (with decoupling), Auto Route, Fit Board, board shape (Outline…), Teardrops, Gloss, Fewer vias, Stitch GND, Place rooms, Check variants (each assembly variant on the one routing). **Select**: drag a part (R rotates, F flips side, K locks, any angle in the panel; a move closer than the assembly spacing is refused with the reason, Alt-drag places it anyway); shift-click or shift-drag selects several parts to align (left, right, top, bottom, centre), spread evenly, replicate their layout onto other channels or give them a room (an area they belong in, drawn dashed; the DRC flags a member outside it); click a track to set its width, layer and lock, split it, tune its net's length, gloss or delete it (Delete); click a via to delete it; H highlights the selected net; drag a track or via to push it and its neighbours. **Route** (live: the track follows the mouse and pushes other nets aside; click fixes a corner, V drops a via, double-click or Enter finishes, Esc cancels; or walk around / highlight collisions), **Pair** (both members of a differential pair together), **Via**, **Rip net**, **Measure**, **Pour** (click its corners, double-click closes, then name the net), **Keep-out** (drag a rectangle), **Cutout**, **Hole** (M3). Fan out a selected part. DRC marks each located problem on the board. |
| **Library** | Make or edit a part for the project library. Open part loads one to change; Save changes updates every placed instance. Pins: type them or paste a datasheet's pin table. Pads: start from the package, from a library footprint (From footprint…) or from the IPC-7351B calculator (lead dimensions L / S / W, density); Edit pads drags them on a 0.05 mm grid, sets size, drill, shape (rectangle, rounded, chamfered, oval, or a polygon you click out), mask margin, paste, non-plated holes, pin, and adds rows. Silk / fab lines on the footprint, lines / rectangles / circles on the symbol, symbol pins dragged to another side. A 3D model (STEP / VRML / STL / OBJ) seated on the footprint. Manufacturer, description and datasheet raise the library quality score, shown with the checks as you type. |
| **3D** | The assembly as in the macOS 3D view: drag to orbit (Shift-drag pans, the wheel zooms), camera presets (isometric, top, bottom, front, back, left, right), the parts list (untick to hide a part, Only isolates it), Explode, Section (cut at a board y), Fold Flex, and a click on a part picks it (dashed box): its properties, Show in PCB / Show in Schematic (cross-probe), Hide, Only (MCP `render_3d` with `picks`). |
| **Git** | Team → Git…, as the macOS Git window, for the folder holding the project: create a repository, the changed files, Save and Commit All, Show Changes (the diff since the last commit, coloured), switch branch, New Branch, Tag, Pull (fast-forward), Push (with tags), history, and Clone (an https / ssh URL into a folder inside the root; its project opens). Your own git and its sign-in; no repository hooks run (MCP `project_git`). |
| **Simulate** | DC operating point (net voltages, part currents and power), transient (waveform plot plus min / max / RMS / frequency per net) AC sweep (gain plot, −3 dB, peak, phase margin) and **Live (run the board)**, as the macOS app's Run: simulated time follows the clock times the speed (0.01× – 10×, 10 – 500 µs resolution), LEDs glow with their current, switches and push-buttons are clicked while it runs, each microcontroller has a serial monitor (text sent to its USART), a scope plots the nets ticked and a table gives every net's voltage; edits restart it. Before starting, pick a microcontroller's firmware: a built-in example or a .hex file in the root folder (MCP `sim_live`, `sim_firmware`). |
| **Checks** | ERC, DRC and the full design verification, as a table. |
| **BOM** | The bill of materials with suggested part numbers, plus the fabrication package (Gerbers, drills, IPC-2581, ODB++, placement, PDF drawings). |
| **Cloud** | Team › Cloud Sync: GitHub sync and share links (see below). |
| **Tools** | Help › All Engine Commands: every one of SiEDA's MCP tools ([MCP.md](MCP.md)), with its description and arguments. Run any of them to edit the design: add parts, connect pins, set rules, simulate, export. |

What only the macOS app has: the 3D view's interactive tools (Studio renders the 3D views; `render_3d` hides, isolates, explodes and cuts), the live simulation scopes and firmware run, the AI assistant window, plug-ins (sandboxed by macOS), live co-editing of a shared file and the in-app Git window (Studio syncs through GitHub on the Cloud tab).

## Let an AI client edit the open design

The address printed at start also serves MCP at `/mcp`, with the same token as a bearer token:

```sh
sieda-mcp --connect http://127.0.0.1:<port>/mcp --token <token>
```

Use that command as the MCP server in Claude Desktop, Claude Code, Cursor or VS Code. Their edits appear in the window
within two seconds.

## GitHub as the cloud: sync and share links

A GitHub repository holds the project. You choose whether it is **public**, so anyone can open the links, or
**private**, so only the repository's collaborators can. SiEDA uses your own `git` and its sign-in: Git Credential
Manager, which comes with Git for Windows, `gh auth login`, or SSH keys. SiEDA never stores a token.

1. Install Git and sign in to GitHub once (for example with `gh auth login`).
2. Create the repository on github.com, public or private.
3. Save the project, open **Cloud**, enter `owner/name`, and choose **Connect**. The project's folder becomes a clone
   of the repository and takes whatever is already there.
4. **Push** saves the project and writes `<name>.viewer.html`, a read-only page with every sheet and the board. It
   commits both and pushes them. If someone pushed in the meantime, SiEDA merges their work first.
5. **Pull** fetches and merges what others pushed, then reopens the project. A project file changed on both sides is
   merged field by field, not as text. Where both sides changed the same field, your version is kept and the field is
   listed under `conflicts`.
6. **Share links** gives:
   - **file:** the project on GitHub;
   - **history:** every change to the project;
   - **raw:** the project file itself, for downloads;
   - **viewer:** the read-only page, served by GitHub Pages.

   It also says who can open them: anyone for a public repository, only the collaborators for a private one. To turn
   the viewer link on, enable Pages in the repository's **Settings → Pages**. Pages for a private repository needs a
   paid GitHub plan.

To give people access to a private repository, add them under the repository's **Settings → Collaborators**. They open
the links signed in to GitHub, or connect their own SiEDA to the same repository.

AI clients use the same feature through the MCP tool `project_cloud` (`connect`, `pull`, `push`, `share`,
`status`). On macOS, the app's Git panel works with the same repository.

## Security

- SiEDA Studio listens on `127.0.0.1` only.
- Every request must carry the random token from the printed address. The page sends it back as a header, which other
  web sites cannot add, and the server refuses any `Host` that isn't the loopback address.
- File access stays inside the root folder: symbolic links out of it are refused.
- When SiEDA runs `git`, it never runs a repository's hooks or fsmonitor, and it never waits for a password prompt.
  Signing in is up to your git setup.
