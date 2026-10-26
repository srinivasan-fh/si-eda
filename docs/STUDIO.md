<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# SiEDA Studio on Linux and Windows, and GitHub cloud sync

SiEDA Studio is SiEDA on Linux and Windows (it runs on macOS too). The design engine serves the app's window to
your web browser, on this computer only. Nothing goes over the network unless you sync with GitHub.

## Install and start

| System | Download | Start |
|---|---|---|
| Linux (x86-64) | `sieda-linux-x86_64.tar.gz` from the release | Unpack it, then run `./sieda-studio`. To get a menu entry, copy `sieda-studio` to `~/.local/bin` and `sieda-studio.desktop` to `~/.local/share/applications`. |
| Windows 10 / 11 (x64) | `sieda-windows-x86_64.zip` from the release | Unpack it and run `sieda-studio.exe`. The files are not signed yet, so Windows SmartScreen asks first: choose **More info → Run anyway**. |

Each archive also contains `sieda-cli` and `sieda-mcp`. Every program is one self-contained file: there are no
runtimes or libraries to install.

You can also build it from source on any of the three systems:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build --target sieda-studio
```

On Windows, build in an MSYS2 MinGW-64 shell (`pacman -S mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake`).

```
sieda-studio [--root <dir>] [--project <file>] [--port <n>] [--no-browser]
```

| Option | Meaning |
|---|---|
| `--root` | Folder for opening projects and writing outputs (default: your home folder). |
| `--project` | Project to open at start. |
| `--port` | Port to listen on (default: any free port). |
| `--no-browser` | Print the address instead of opening the browser. |

## What it does

| Area | What you can do |
|---|---|
| **File** | New, Open (lists the projects under the root folder), Save, Save As, built-in examples, Undo (Ctrl+Z), Ctrl+S. |
| **Schematic** | Every sheet, with wheel zoom and drag to pan; double-click fits the sheet. ERC. |
| **Board** | All layers, or top / bottom copper. Auto Place (with decoupling), Auto Route, Fit Board, DRC. |
| **3D** | The assembly from seven views. |
| **Checks** | ERC, DRC and the full design verification, as a table. |
| **BOM** | The bill of materials with suggested part numbers, plus the fabrication package (Gerbers, drills, IPC-2581, ODB++, placement, PDF drawings). |
| **Cloud** | GitHub sync and share links (see below). |
| **Tools** | Every one of SiEDA's MCP tools ([MCP.md](MCP.md)), with its description and arguments. Run any of them to edit the design: add parts, connect pins, set rules, simulate, export. |

The macOS app adds interactive drawing editors on top of the same engine: schematic capture, the interactive router,
and the footprint and symbol editors. On Linux and Windows, design edits go through the tools, or through an AI client
connected to the open design.

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
