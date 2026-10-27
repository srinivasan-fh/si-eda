<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# Using SiEDA from any AI: the MCP server

SiEDA speaks the [Model Context Protocol](https://modelcontextprotocol.io) (MCP). Any MCP client — Claude Desktop,
Claude Code, Cursor, VS Code, ChatGPT desktop, or a local LLM host — can open SiEDA projects, capture schematics,
search the part catalog, simulate, lay out and autoroute boards, run ERC / DRC / verification, look at the design as a
picture and write fabrication files. The AI works with the same engine the app uses; nothing runs in the cloud except
the AI client itself.

- [What it is](#what-it-is)
- [Two modes: headless or live](#two-modes-headless-or-live)
- [Build and run](#build-and-run)
- [Live app mode](#live-app-mode)
- [Client setup](#client-setup)
- [Safety](#safety)
- [A first session](#a-first-session)
- [Resources and prompts](#resources-and-prompts)
- [Embedding the engine (C API)](#embedding-the-engine-c-api)
- [Tests](#tests)
- [Code map](#code-map)
- [Tool reference](#tool-reference)

## What it is

| Part | Where | What it does |
|---|---|---|
| MCP engine | `Core/src/Mcp.cpp`, `Core/include/sieda/Mcp.hpp` | Transport-independent JSON-RPC 2.0 handler: `initialize`, `ping`, `tools/list`, `tools/call`, `resources/list`, `resources/read`, `resources/templates/list`, `prompts/list`, `prompts/get`; file sandbox; session (the open project). |
| Tool table | `Core/src/McpTools.cpp` | 185 tools in 9 groups, each with a description, a JSON Schema and MCP annotations; handlers call the C API (`sieda_c.h`). Data-driven: `mcpTools()`. |
| Renderer | `Core/src/McpRender.cpp` | The schematic and the board as PNG (own rasteriser and PNG writer) or SVG, for clients that can look at images. |
| C API | `Core/src/sieda_c_mcp.cpp`, `sieda_mcp_*` in `sieda_c.h` | For hosts that embed the engine (the app's live endpoint). |
| Stdio server | `Core/mcp/main.cpp` → `sieda-mcp` | Newline-delimited JSON-RPC on stdin / stdout; logs on stderr only. |
| Live app endpoint | `SiEDA/App/MCPEndpoint.swift`, `SiEDA/App/DesignStore+MCP.swift` | MCP over Streamable HTTP on `127.0.0.1` inside the macOS app: the AI works on the design open in the window, with undo. Off by default. |
| Stdio bridge | `Core/mcp/http_bridge.cpp` → `sieda-mcp --connect` | For clients that only launch stdio servers (Claude Desktop): forwards stdin to the live app endpoint. |

Protocol revision **2025-06-18**. Clients that ask for 2025-03-26 or 2024-11-05 are answered in their revision. Tool
failures (an unknown designator, a pin that does not exist, a refused path) come back as tool results with
`isError: true` and a message the AI can act on; protocol errors use the JSON-RPC codes -32700 (parse error), -32600
(invalid request), -32601 (unknown method), -32602 (invalid params, unknown tool or prompt), -32603 (internal) and
-32002 (unknown resource).

## Two modes: headless or live

| | Headless (`sieda-mcp`) | Live app (Settings → AI Access (MCP)) |
|---|---|---|
| What the AI works on | Its own project, opened from / saved to a root folder | The design open in the SiEDA window — you watch the canvas change |
| Undo | — | Every tool call that changes the design is one undo step (**Edit → Undo**), shown as "AI: <tool>" in the status bar |
| Transport | stdio (the client starts the process) | Streamable HTTP on `http://127.0.0.1:39717/mcp`, bearer token; stdio clients use `sieda-mcp --connect` |
| Needs | A build of the core (any OS) | The macOS app running, with the live server switched on |
| Good for | Batch jobs, CI, scripted designs, Linux / Windows | Pair-designing: you and the AI on the same board |

The tools, resources and prompts are the same in both modes (one engine, `Core/src/Mcp.cpp`).

On Linux and Windows, SiEDA Studio serves the live mode too: `sieda-mcp --connect http://127.0.0.1:<port>/mcp --token
<token>` with the address and token it prints at start ([STUDIO.md](STUDIO.md)).

## Build and run

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j --target sieda-mcp
./build/sieda-mcp --root ~/Designs           # waits for JSON-RPC on stdin
```

| Option | Meaning |
|---|---|
| `--root <dir>` | The only folder the server reads projects from and writes outputs into. Relative paths in tool arguments resolve against it. Default: the current folder. |
| `--project <file>` | Open this project at start (it must be inside the root). |
| `--read-only` | Refuse every tool that changes the design, and every file write. Reading, checking, simulating and rendering still work. |
| `--tools <list>` | Offer only these groups / tools / prefixes, e.g. `--tools project,schematic,sim_*,pcb_drc`. |
| `--docs <dir>` | Folder of the guides served as `sieda://docs/<name>` (default: the repository's `docs/`). |
| `--library-dir <dir>` | A KiCad library folder that `library_search` / `library_add_part` use (`"Library:Symbol"`); repeatable, read-only, also `SIEDA_LIBRARY_DIRS` ([LIBRARY_IMPORT.md](LIBRARY_IMPORT.md)). |
| `--connect <url>` | Bridge mode: no local engine, forward stdin / stdout to the app's live endpoint (see [Live app mode](#live-app-mode)); with `--token <t>` (or `SIEDA_MCP_TOKEN`) and `--timeout <s>`. |
| `--list-tools-markdown`, `--list-tools` | Print the tool reference (Markdown, as below) or the tool table (JSON) and exit. |

Use absolute paths in client configuration files: most clients do not expand `~` there.

## Live app mode

The SiEDA app can serve MCP itself, so an AI client drives **the design that is open in the window**.

1. **SiEDA → Settings… → AI Access (MCP)** and switch on **Enable live MCP server** (it is off by default and stays
   off until you turn it on).
2. Optional: change the **port** (default 39717), turn on **Read-only**, or pick the **Allowed folder** (default: the
   folder of the open document; an unsaved design has no file access until you save it or pick a folder).
3. Copy the **access token** (or one of the ready-made snippets under **Client setup**, which contain it) into your
   client — see [Client setup](#client-setup). The token is created on first use and kept in the macOS Keychain;
   **Regenerate** replaces it (every client must then be given the new one).
4. The status line shows the endpoint, open connections, MCP sessions and the last tool called. While the server is on,
   an antenna icon sits in the window's status bar; it lights up for half a minute after an AI client's call.

What happens to a request:

- Every request is handled on the app's main thread, against the window's design. A tool that changes the design runs
  as one undoable edit labelled "AI: <tool name>" (status bar); **Edit → Undo** takes it back like any edit. Reading
  tools (lists, ERC, DRC, simulation, renders) leave no undo step. A refused or failed tool that changed nothing
  leaves no undo step either.
- `project_new` and `project_load_example` replace the window's design in place (one undo step; the window keeps its
  file, so **File → Save** would write the new design there). `project_open` uses the app's normal open path: the
  window then shows that file and its undo history starts afresh; it is refused while the open design has unsaved
  changes. `project_save` without a path saves the window's document (like **File → Save**); with a path it writes a
  copy inside the allowed folder.
- While SiEDA is busy (autorouting, simulation started from the app) tool calls answer "SiEDA is busy" — try again.
  Long tools called by the AI (e.g. `pcb_autoroute` on a big board) run on the main thread, so the window waits for
  them.

Protocol details (for client authors): MCP **Streamable HTTP**, revision 2025-06-18 (also 2025-03-26, 2024-11-05).

| Request | Answer |
|---|---|
| `POST /mcp`, JSON-RPC request (or batch) | `200`, `Content-Type: application/json`, the JSON-RPC response |
| `POST /mcp`, only notifications / responses | `202 Accepted`, no body |
| `POST /mcp` `initialize` | The response carries `Mcp-Session-Id`; send it on later requests (an unknown id → `404`, initialize again; a missing id is accepted) |
| `DELETE /mcp` with `Mcp-Session-Id` | `204`, the session ends |
| `GET /mcp` | `405` (no server-initiated stream: every answer comes back on its POST) |
| No / wrong `Authorization: Bearer <token>` | `401` with `WWW-Authenticate: Bearer` |
| `Origin` or `Host` that is not `localhost` / `127.0.0.1` / `[::1]` | `403` (blocks DNS rebinding from web pages) |
| `MCP-Protocol-Version` the server does not speak | `400` |
| Body over 16 MB / headers over 32 KB | `413` / `431` |

`sieda-mcp --connect <url>` turns this endpoint back into stdio for clients that only start local processes: it reads
one JSON-RPC message per stdin line, POSTs it with the token (`--token <t>` or the `SIEDA_MCP_TOKEN` environment
variable; prefer the variable — command lines are visible to other processes), keeps the session id, and prints each
response (JSON or Server-Sent Events) as one stdout line. If the app is not running or refuses the token, every request
is answered with a JSON-RPC error that says why, so the client never hangs. `--timeout <s>` (default 600) bounds one
request. Plain HTTP/1.1 over POSIX sockets, macOS and Linux.

## Client setup

Every client below in both modes. Replace `/path/to/si-eda` with your checkout, `/Users/me/Designs` with the folder
the AI may work in, and `<token>` with the token from **Settings → AI Access (MCP)** (the snippets there already have
it filled in, with the port you chose). Use absolute paths: most clients do not expand `~` in their configuration.
Restart a client (or reload its MCP servers) after changing its configuration.

### Claude Code

Headless:

```bash
claude mcp add sieda -- /path/to/si-eda/build/sieda-mcp --root ~/Designs
# shared with a repository's team: claude mcp add --scope project sieda -- …   (writes .mcp.json)
```

Live app (Claude Code speaks HTTP itself):

```bash
claude mcp add --transport http sieda-app http://127.0.0.1:39717/mcp --header "Authorization: Bearer <token>"
```

Check with `claude mcp list` (or `/mcp` inside a session).

### Claude Desktop

`claude_desktop_config.json` (macOS: `~/Library/Application Support/Claude/`, Windows: `%APPDATA%\Claude\`; or
**Settings → Developer → Edit Config**), then quit and restart Claude Desktop. Both modes can be configured at once.

```json
{
  "mcpServers": {
    "sieda": {
      "command": "/path/to/si-eda/build/sieda-mcp",
      "args": ["--root", "/Users/me/Designs"]
    },
    "sieda-app": {
      "command": "/path/to/si-eda/build/sieda-mcp",
      "args": ["--connect", "http://127.0.0.1:39717/mcp"],
      "env": { "SIEDA_MCP_TOKEN": "<token>" }
    }
  }
}
```

`sieda-app` is the live mode: Claude Desktop starts `sieda-mcp --connect`, which forwards to the running app. (Claude
Desktop's remote "custom connectors" are reached from Anthropic's servers and cannot see a server on your Mac, so
the bridge is the way in.)

### Cursor

`.cursor/mcp.json` in a project (or `~/.cursor/mcp.json` for all projects):

```json
{
  "mcpServers": {
    "sieda": {
      "command": "/path/to/si-eda/build/sieda-mcp",
      "args": ["--root", "/Users/me/Designs", "--tools", "project,schematic,library,pcb,verify,output,render"]
    },
    "sieda-app": {
      "url": "http://127.0.0.1:39717/mcp",
      "headers": { "Authorization": "Bearer <token>" }
    }
  }
}
```

Cursor (and some other clients) only pass a limited number of tools to the model. SiEDA has 132, so in headless mode
pick the groups a task needs with `--tools`; in live mode switch off the tools you do not need in Cursor's MCP
settings.

### VS Code (Copilot agent mode)

`.vscode/mcp.json` in the workspace (or **MCP: Open User Configuration** for all workspaces):

```json
{
  "inputs": [
    { "type": "promptString", "id": "sieda-token", "description": "SiEDA MCP token", "password": true }
  ],
  "servers": {
    "sieda": {
      "type": "stdio",
      "command": "/path/to/si-eda/build/sieda-mcp",
      "args": ["--root", "${workspaceFolder}"]
    },
    "sieda-app": {
      "type": "http",
      "url": "http://127.0.0.1:39717/mcp",
      "headers": { "Authorization": "Bearer ${input:sieda-token}" }
    }
  }
}
```

The `inputs` entry makes VS Code ask for the token once and keep it out of the file (the snippet in SiEDA's settings
has it inline instead — fine for a user configuration, not for a file you commit).

### ChatGPT

ChatGPT's connectors (developer mode) are called from OpenAI's servers and need a public HTTPS URL. SiEDA never
listens beyond this Mac — neither mode can be reached by ChatGPT directly, and exposing the endpoint through a tunnel
would put your design and token on the internet; that is not supported. Use one of the local clients above (or a
local agent host below) instead.

### Other clients

- **Anything that launches stdio servers** (LM Studio, Ollama-based agents such as `mcphost`, Zed, Continue, your own
  scripts): headless — the command `sieda-mcp --root <folder>`; live — the command
  `sieda-mcp --connect http://127.0.0.1:39717/mcp` with `SIEDA_MCP_TOKEN=<token>` in its environment.
- **Anything that speaks Streamable HTTP** (live mode only): URL `http://127.0.0.1:39717/mcp` and the header
  `Authorization: Bearer <token>`.
- **By hand**, headless:

  ```bash
  printf '%s\n' \
    '{"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18"}}' \
    '{"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"project_load_example","arguments":{"id":"led_indicator"}}}' \
    '{"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"schematic_erc"}}' \
    | ./build/sieda-mcp --root /tmp
  ```

  and live (the part appears in the window; **Edit → Undo** removes it):

  ```bash
  curl -s http://127.0.0.1:39717/mcp -H "Authorization: Bearer $SIEDA_MCP_TOKEN" \
    -H 'Content-Type: application/json' -H 'Accept: application/json, text/event-stream' \
    -d '{"jsonrpc":"2.0","id":1,"method":"tools/call","params":{"name":"schematic_add_component","arguments":{"kind":"resistor","value":"10k"}}}'
  ```

## Safety

- **One folder.** Files are opened and written only inside the root folder. `..` in a path is refused, as are absolute
  paths elsewhere and symbolic links that lead out of the root. A server without a root (the C API's default) has no
  file access at all; results still come back inline.
- **Read-only mode.** `--read-only` (or `"readOnly": true`) refuses every tool marked as changing the design (and the
  tools that replace the project: `project_new`, `project_open`, `project_load_example`) and every file write. The
  tool list says which tools are unavailable.
- **Annotations.** Every tool carries MCP hints: `readOnlyHint` (changes nothing), `destructiveHint` (removes or
  replaces work: removing parts or pours, clearing routing, replacing the project), `idempotentHint`. Clients use them
  to decide what to confirm with you. Output tools are not read-only (they write files) but do not change the design.
- **No network, no secrets.** The server makes no network requests and reads no keys. Supplier lookups stay in the app.
- **Live app mode** adds:
  - *Off by default*; nothing listens until you switch it on, and switching it off closes every connection.
  - *This Mac only*: the listener is bound to `127.0.0.1` (never `0.0.0.0`) and accepts local connections only.
  - *Token*: every request needs `Authorization: Bearer <token>` (compared in constant time); the token is random
    (256 bits), lives in the Keychain and can be regenerated at any time.
  - *Origin / Host check*: a web page that tricks your browser into calling `localhost` (DNS rebinding) is refused with
    403 — requests must name `localhost`, `127.0.0.1` or `[::1]`.
  - *Undo*: every change the AI makes is one undo step, and `project_open` never discards unsaved work.
  - *Same sandbox*: files only inside the allowed folder (the open document's folder unless you pick one); read-only
    mode as above.
- **Bounded answers.** Waveforms, sweeps and curves are decimated (`maxPoints`), long outputs are truncated inline
  (give `path` to write them whole), violation lists are capped at 100 (errors first).

## A first session

Ask the AI something like *"Design a 5 V LED indicator board in SiEDA, route it and write the Gerbers to `fab/`"*. A
typical tool sequence:

1. `project_new` → `schematic_add_component` (voltage_source 5, resistor 330, led) → `schematic_connect`
   (`{"chain":["V1.+","R1.1"],"connections":[["R1.2","D1.A"]]}`) → `schematic_connect_to_ground`
   (`{"pins":["V1.-","D1.K"]}`).
2. `schematic_erc` (no errors) → `sim_dc_op` (LED current ≈ 9 mA).
3. `pcb_set_board` (`{"width":30,"height":20,"layers":2}`) → `pcb_update_from_schematic` → `pcb_autoroute` → `pcb_drc`.
4. `render_pcb` to look at it → `output_gerbers` (`{"dir":"fab"}`) → `project_save_as`.

Addressing: parts by designator (`R1`), pins as `REF.PIN` (`R1.2`, `D1.A`, `U1.VCC` or the pin number `U1.14`).
Schematic coordinates are grid units (10 = one grid step, y down); board coordinates are millimetres (y down, origin
at the board's top-left corner). Without coordinates, new parts go to a free spot.

What the agent gets back, so it can wire correctly without guessing:

- Every placed part lists each pin's `name`, `number`, `type` (power_in, input, output, bidirectional, passive …),
  `net` (null = unwired), `x` / `y` of the pin end and `side` (left / right / top / bottom; a corner pin belongs to
  the side with more pins, so an SOIC's pins 1 and 24 are left and right). Net labels and grounds go out on that side,
  the label's flag pointing at the pin, in the app, the schematic PDF and `render_schematic` alike. A multi-function pin
  answers to each function (`U1.RB0` = `INT/RB0`); a name several pins share is written and accepted with its number,
  `U1.VDD(32)`.
- `schematic_connect` refuses to join two differently named nets (`VCC` and `GND`) unless `merge` is true, and its
  `warnings` name any part whose two pins end up on one net. A chain puts all its pins on one net; it does not pass
  through a part. `N$…` names are automatic and change; `schematic_net_label` names a net.
- Each part also gives its `bounds` (`x0`, `y0`, `x1`, `y1`), so other parts and labels can be kept outside it.
  Built-in parts have pin types too: a source's `+` is power_out, an op-amp's `IN±` are inputs and `OUT` an output.
- `schematic_erc` gives a `status` (clean / warnings / errors; `clean` alone means no errors) and `unconnectedPins` per part, with one summary line
  for them instead of one violation per pin; `schematic_set_no_connect` takes `pins` to mark the unused ones.
- `schematic_validate` also reviews the design: no decoupling capacitor on an IC supply pin, an LED with nothing that
  sets its current (`VAL_LED_NO_RESISTOR`), a reset / enable input left floating (`VAL_FLOATING_CONTROL`). The review
  checks run only here, not in the app's verification.
- `schematic_build` builds a circuit in one call:
  `{"parts":[{"ref":"U1","part":"NE555"},{"ref":"R1","kind":"resistor","value":"1k"}],
  "nets":{"VCC":["U1.VCC"],"GND":["U1.GND"]},"wires":[["U1.OUT","R1.1"]],"noConnect":["U1.CONT"]}`.
  Named nets get net labels (`GND` gets ground symbols). Wired parts are placed side by side so the wires run
  straight, and the rest go in rows. It returns `checks` (the ERC and the review), the parts with their nets
  (`"pins": false` gives only pin number → net) and every step that `failed` (a wrong pin drops only itself). A `part` may also be one made with `library_create_part`.
- `project_quickstart` returns the quick start for clients that cannot read MCP resources.
- `pcb_drc` lists each `unrouted` connection by its pins and net (`{"from":"J1.CC1","to":"R1.1","net":"N$1"}`).
  `schematic_remove_component` also removes the net labels and grounds that were wired to that part alone, and
  `schematic_move_component` moves them with it.
- `library_search` that matches nothing says how many parts each word matches. `library_check_part` checks a
  parametric package (SOIC, QFN …) on the footprint it generates.
- `render_schematic` draws op-amps as triangles with + / − inputs, and capacitors, diodes / LEDs and switches with
  their usual symbols (other parts are boxes with pin names). Wires bend at right angles, two nets never share a wire
  leg, no-connect pins carry an X, and lower-case letters are drawn as small capitals (100n ≠ 100N).
- `pcb_autoroute`: a whole-board route on a small board that leaves connections open tries again, keeping what
  helps: shape-based completion, then each two-pin part on an open connection turned round. The result lists the
  `repairs` kept. A partial preset (`nets`, `area`, `fanout`) applies to its call only.
- `pcb_set_keepouts` takes `pours: true` to keep copper pours out of an area (the area under an antenna); Auto Place
  keeps parts out of a keep-out on every layer.
- In `schematic_build`, neighbouring pins of one part on one side and net (an IC's supply pins) share one label,
  joined by a short wire along their ends. A part whose package is wrong is still placed; the error lists the
  packages it comes in.
- The built-in op-amp has no supply pins and swings ±15 V unless its value sets the output limits
  (`"LM358 VOH=3.5 VOL=0"`); `schematic_validate` warns (`VAL_OPAMP_RAILS`) when the supply is lower.
- `schematic_validate` also checks one decoupling capacitor per supply pin of a rail (`VAL_DECOUPLING_PER_PIN`),
  a load capacitor on each side of a crystal (`VAL_CRYSTAL_LOAD`), and BOOT0 / BOOT / MODE pins like reset and enable.
- `sim_ac` with `at: ["1k"]` gives each net's gain (dB and V/V) and phase at those frequencies.
- An unknown catalog name (an orderable number such as `PIC16F877A-I/P`) answers with the closest catalog parts and
  their packages.
- `project_undo` (`steps`, default 1) steps back over this session's edits to the open project, one tool call per
  step (the last 30 are kept).
- `schematic_build` draws a 2 – 3 pin signal net as wires from its biggest part's pin, with one label naming it;
  supplies (a power pin on the net, or a name like `VCC`, `3V3`, `+5V`) keep their labels.
- `pcb_place_footprint` takes `placements` to place many parts at once (a refused pose is tried again once the
  others have moved); `pcb_lock_footprint` takes `refs`. `pcb_autoplace` with `decoupling: true` then moves each
  decoupling capacitor further than 5 mm from its IC supply pin to a free spot 2.5 – 6 mm from it.
- `pcb_autoroute` also fans out each fine-pitch part (pitch < 0.65 mm) on an open connection first, and with
  connections still open returns `hints` (an exposed pad: thermal vias and a pour; a finer rule preset).
- Catalog: the ESP32-C3-MINI-1 (Espressif's KiCad library) and ESP32-C3-WROOM-02 modules, 12 / 25 / 40 MHz
  3.2 × 2.5 mm crystals and a 2.45 GHz chip antenna; the USB-C receptacle and the 6 mm switch have all their pads.
- `schematic_build` lays each two-pin part on a named net (a capacitor on a supply, a pull-up on RESET) beside the
  pin it serves and draws a wire where the run is straight and clear, named by a tag halfway along it; the rest
  keeps labels. Parts keep room for their labels.
- `pcb_autoplace` `decoupling` gives each IC supply pin its own small capacitor (≤ 1 µF; a regulator's input and
  output also take bulk ones, and a small one only after the loads' pins have theirs), puts a crystal (`Y…`) within
  6 mm of the pins it drives (small parts there step aside) with its load capacitors beside it, and reports
  `boardGrown` when the parts did not fit (`pcb_update_from_schematic` too). A package may be named by its size
  alone ("0603" = `R_0603` on a resistor); `project_snapshot` pads carry their part's `ref`. A batch
  `pcb_place_footprint` lifts its own parts first; moving
  a part removes the tracks left on its old pads and the stubs they leave (`copperRemoved`).
- A via that joins pours only (stitching the GND pours of two layers) is kept through edits and save / open.
- `pcb_autoplace` with `all` first puts an antenna part at the top edge, antenna end out, with a keep-out (tracks,
  vias, pours) over it, and a USB / connector part with an open end (its mouth) at the bottom edge facing out
  (`edgesPlaced`; `edges: false` skips it); `decoupling` is on by default with `all`.
- `project_undo` never steps back past `project_open` / `project_new` / `project_load_example`.
- `pcb_autoplace` `all` also puts connectors without an open end (pin headers) at the left and right edges in turn,
  long side along the edge, and a capacitor from ground to a single IC pin (AREF, EN) beside that pin. A crystal is
  placed before the supply capacitors; where small parts fill its spot they step aside to the nearest free one.
- A batch `pcb_place_footprint` part refused and left at its old pose under a part of the batch gets `overlaps` and a
  free spot in its `note`; `schematic_remove_component` lists the pins it left unwired (`disconnected`);
  `pcb_set_board` with an unknown `rulePreset` changes nothing and lists the presets; `pcb_add_pour` returns the
  area, islands, pads joined and islands without pads; the ERC's "no simulation model" notes are one line; the BOM
  names a catalog part's maker and KiCad footprint.
- Auto Place puts the edge parts largest first (a long header takes an edge's middle; a header with no room there tries
  the other side, then each side filled from the top); small passives step aside so a supply capacitor lands within
  3.5 mm of its pin. `pcb_autoroute` lays a re-routed net's stitching vias again (`stitchingRestored`) and names
  differential pairs it routed as two single nets (`pairsNotCoupled`, e.g. a USB-C's doubled D+ / D- pads);
  a schematic diff-pair directive pairs `USB_DP` / `USB_DM`; `pcb_set_stackup` warns when the impedance targets need tracks over 1 mm wide.
  `render_schematic` staggers top / bottom pin names that would touch and keeps the ref and value clear of the pins.
- Assembly spacing standard, courtyard to courtyard on top of the IPC-7351B courtyards (land pattern + 0.25 mm):

  | Neighbours | Gap |
  |---|---|
  | passive – passive (chip R / C / L, SOD, SOT-23) | 0.25 mm |
  | passive – IC (SOIC, QFP, QFN, modules) | 0.5 mm |
  | IC – IC, through-hole – SMD | 1.0 mm |
  | through-hole – through-hole | 0.5 mm |
  | BGA – anything (rework nozzle) | 2.0 mm |

  Auto Place's edge, decoupling and crystal moves keep it; `pcb_drc` flags a pair closer than it (`DRC_PART_SPACING`,
  touching courtyards included) and `pcb_place_footprint` lists such neighbours in `tooClose`.
- `pcb_autoplace` `grouped` lines up the other passives of one kind in rows in designator order (C1 C2 C3, R1 R2 R3,
  D1 D2 D3), 1 mm between courtyards, near their connections (`groupedRows`); parts placed by their IC pins stay. A
  connector carrying power only goes to the left edge (the input side), signal connectors to the right.
- `pcb_autoplace` `all` puts the main IC (most pins) at the board's middle before the rest, so its decoupling caps and
  crystal find room on every side. `pcb_autoroute` keeps a pour pad's fan-out via clear of its SMD pad by a 0.1 mm
  mask web (option `viasOffPads`, on by default over MCP; the board's saved strategy is untouched).
- `schematic_build`: a pin name several pins share, given without a number (`J1.GND` on a USB-C), joins all of them
  (pins stacked on one spot already are). `pcb_add_pour` gives the centres of islands that reach no pad
  (`islandsWithoutPadsAt`).
- `schematic_move_component` puts a tag naming a wire back at the wire's middle and lists `overlaps`; the ERC's
  `unconnectedPins` leaves out pins typed no-connect; `library_search` ranks parts whose name has the words first;
  each `pcb_drc` `unrouted` entry has both ends' coordinates (`at`).
- `pcb_stitch_vias` defaults to a 5 mm pitch and keeps vias out from under parts (`underParts`); `render_pcb`
  draws keep-outs; `schematic_disconnect` between two pins joined through a `schematic_build` tag removes the tag.
- `pcb_drc` flags a part whose courtyard leaves the board or overlaps another (`DRC_PLACE_*`, a forced pose) and an
  antenna part with no pour keep-out over it (`DRC_ANTENNA_KEEPOUT`, info, with the antenna end's area).
- `pcb_summary` with `pads: ["U1"]` lists those parts' pads (pin, net, x / y, size, layer).
- `pcb_autoroute` lists in `failedNets` only nets still open (a pour joined the rest: `joinedLater`).
- `verify_design` counts parts and nets as `project_summary` does; a board fed through a connector gets
  `VER_NO_SOURCE` as info.
- `library_search` gives a catalog part's KiCad footprint as its `package`; when no part has every word it lists
  the parts matching the most words (`closest`). `pcb_update_from_schematic` sums up single-pin nets in one line.

## Resources and prompts

| Resource | Content |
|---|---|
| `sieda://docs/mcp-quickstart` | Units, addressing and the typical flow (always available). |
| `sieda://docs/<name>` | The guides in `docs/` (`schematic`, `routing`, `simulation`, `interactive_routing`, `signal_power_integrity`, `memory_design`, …). Template `sieda://docs/{name}`. |
| `sieda://catalog` | Catalog digest: the built-in kinds with their pins, the standard-part categories with sizes and examples. |
| `sieda://project/current` | The open project as saved (JSON). |
| `sieda://rules/presets`, `sieda://autoroute/presets` | Design-rule presets, autorouter strategy presets. |
| `sieda://tools` | The tool reference below. |

| Prompt | Arguments | What it asks the AI to do |
|---|---|---|
| `design_minimal_mcu_board` | `mcu`, `supply`, `extras`, `layers` | A microcontroller board with supply, decoupling, reset, header and a status LED, step by step to a routed, verified board. |
| `review_design` | `focus` | Run every check and write a prioritised review without changing the design. |
| `route_and_verify` | `layers`, `preset`, `output_dir` | Update the PCB, place, route, fix DRC, verify and write the fabrication package. |
| `simulate_circuit` | `net`, `analysis` | Operating point, then a transient or AC analysis, explained. |

Built-in examples (`project_list_examples`): `led_indicator`, `transistor_switch`, `rc_lowpass`,
`opamp_noninverting`, `ne555_blinker`.

## Python

`python/sieda.py` (standard library only) starts `sieda-mcp` and turns every tool into a method:

```python
from sieda import Sieda
with Sieda("build/sieda-mcp", root="designs") as s:
    s.project_load_example(id="led_indicator")
    s.pcb_update_from_schematic()
    s.pcb_autoroute()
    print(s.pcb_drc())
    s.output_fabrication_package(dir="fab")
```

A tool that fails raises `SiedaError` with the tool's message. `tools()` lists the tools with their schemas. CTest
`sieda_python_api` runs it end to end.

## Embedding the engine (C API)

The C API is versioned semantically: `SIEDA_API_VERSION_MAJOR` / `_MINOR` / `_PATCH` in `sieda_c.h`, and
`sieda_api_version()` at run time. MAJOR changes break callers, MINOR adds functions or JSON fields, PATCH fixes.

The app's live endpoint reuses this engine through the C API in `sieda_c.h`:

```c
SiedaMcpServer* sieda_mcp_new(const char* options_json);          /* NULL for invalid options */
void            sieda_mcp_free(SiedaMcpServer* server);
char*           sieda_mcp_handle(SiedaMcpServer* server, const char* request_json); /* "" = no response; free it */
int32_t         sieda_mcp_attach_project(SiedaMcpServer* server, SiedaProject* project); /* NULL detaches */
int32_t         sieda_mcp_set_options(SiedaMcpServer* server, const char* options_json);
void            sieda_mcp_set_change_callback(SiedaMcpServer* server, SiedaMcpChangeCallback cb, void* user);
int32_t         sieda_mcp_set_project_path(SiedaMcpServer* server, const char* path);
char*           sieda_mcp_tools_json(void);
```

Options: `{"readOnly", "allowedRoot", "toolFilter":[…], "docsDir", "serverName"}`. With a project attached, tools
edit the host's document in place and the change callback (tool name) fires after every tool that changed it, inside
`sieda_mcp_handle`. **Threading:** a server is not thread-safe; call it from one thread at a time, and with an
attached project only from the thread that owns that project (the rule for every `SiedaProject` call), e.g. the
app's main actor. The app attaches the window's project before every request (Undo and Open swap the engine's
project), runs mutating tools inside one undo step and refreshes its views when the change callback fired. In C++ the same engine is `sieda::mcp::McpServer` (`handle`, `attachProject`, `setChangeHandler`)
and the tool table is `sieda::mcp::mcpTools()`.

## Tests

- `Core/tests/core_tests.cpp`, block `// ---- MCP`: handshake and capabilities, tool schemas for every group,
  protocol errors, tool failures as `isError` results, read-only mode, the root sandbox (`..`, absolute paths,
  symbolic links), resources and prompts, examples, tool filter, attach + change callback, PNG / SVG rendering, and
  an end-to-end board built only through `tools/call` (schematic → ERC → Update PCB → autoroute → clean DRC →
  Gerbers in the root). `mcp_agent_gets_full_pin_detail_and_guards`: pin detail, shared pin names, the VCC–GND
  merge guard, short warnings, `unconnectedPins` and catalog suggestions.
- `Core/tests/c_api_test.c`, `sieda_c_api_mcp_test`: the `sieda_mcp_*` round trip with an attached project.
- CTest `sieda_mcp_stdio` (`Core/tests/mcp_stdio_test.py`, transcript `Core/tests/fixtures/mcp/transcript.jsonl`): runs
  the `sieda-mcp` binary and checks every stdout line is a JSON-RPC response in order (and that the PNG inflates).
- CTest `sieda_mcp_connect` (`Core/tests/mcp_connect_test.py`): `sieda-mcp --connect` against a small HTTP server on an
  ephemeral loopback port — token and session headers, 202 for notifications, JSON, Server-Sent-Events and chunked
  replies, and JSON-RPC errors (no hang) for a wrong token or an app that is not running.
- App: `MCPLiveEndpointTests` in `SiEDATests/SiEDATests.swift` — HTTP parsing across arbitrary chunk boundaries
  (Content-Length and chunked bodies, limits), the gate (401 without / with a wrong token, 403 for a foreign Origin or
  Host), routing (405, 404, 202, sessions), `initialize`, `schematic_add_component` through the store (the part is in
  the window's design, one undo step, Undo removes it), read-only refusals, project replacement and `project_open`
  with unsaved work, off by default, the listener's loopback-only parameters, and a real URLSession round trip on an
  ephemeral port.

## Code map

| File | Contents |
|---|---|
| `Core/include/sieda/Mcp.hpp` | `McpServer`, `McpOptions`, `McpTool`, `mcpTools()`, render functions |
| `Core/src/Mcp.cpp` | JSON-RPC dispatch, sandbox, resources, prompts, examples, argument helpers |
| `Core/src/McpInternal.hpp` | `Schema` builder and helpers shared with the tools |
| `Core/src/McpTools.cpp` | The tool table |
| `Core/src/McpRender.cpp` | PNG / SVG renderer, base64 |
| `Core/src/sieda_c_mcp.cpp` | C API |
| `Core/mcp/main.cpp` | `sieda-mcp` stdio server |
| `Core/mcp/http_bridge.cpp` | `sieda-mcp --connect`: stdio ⇄ HTTP bridge to the live app |
| `SiEDA/App/MCPEndpoint.swift` | Live endpoint transport: `NWListener` on 127.0.0.1, HTTP/1.1 parser, request gate (token, Origin / Host) |
| `SiEDA/App/DesignStore+MCP.swift` | `MCPLiveServer` (settings, token, status, Streamable HTTP routing) and `MCPStoreBridge` (tool calls on the window's design with undo) |
| `SiEDA/Views/Settings/MCPSettingsView.swift` | Settings → AI Access (MCP) and the status-bar indicator |

To add a tool, add a `t.add(group, name, title, kind, idempotent, description, Schema()…, handler)` entry in
`McpTools.cpp` and regenerate the reference below with `./build/sieda-mcp --list-tools-markdown`.

## Tool reference

Kind: **read** changes nothing; **edit** changes the design (refused in read-only mode); **edit (destructive)**
removes or replaces work; **writes files** creates files inside the root without changing the design. Every tool's
full description and argument schema is in `tools/list` (or `sieda-mcp --list-tools`).

### project (19)

Create, open, save and inspect projects; built-in examples.

| Tool | Kind | What it does |
|---|---|---|
| `project_new` | edit (destructive) | Starts a new, empty project (replacing the open one; save first if needed). |
| `project_open` | edit (destructive) | Opens a .siedaproj file from the server's root folder (relative paths resolve against it), replacing the open project. |
| `project_save` | writes files | Saves the project to the file it was opened from / last saved to (or to "path"), inside the root folder. |
| `project_save_as` | writes files | Saves the project to a new .siedaproj file inside the root folder; later saves go there. |
| `project_cloud` | edit (destructive) | A GitHub repository as the project's cloud, through your own git and its sign-in (Git Credential Manager, gh auth login or SSH keys; SiEDA keeps no token). |
| `project_export_json` | read | The whole project as saved (.siedaproj JSON): schematic, library, board, rules, variants. |
| `project_list_examples` | read | The built-in example designs (ids for project_load_example). |
| `project_load_example` | edit (destructive) | Replaces the open project with a built-in example design (schematic and board size; route it with pcb_update_from_schematic and pcb_autoroute). |
| `project_summary` | read | Where the design stands: name, file, part / net / sheet counts, board size and layers, placement and routing progress, and the session (read-only, root folder). |
| `project_set_info` | edit | Sets the project name, the requirements text, the industry profile and title-block fields (any subset). |
| `project_industry_profiles` | read | Industry profiles (rule preset, derating, standards, guidance) for project_new / project_set_info. |
| `project_snapshot` | read | Raw view-model sections of the design (as the app draws it): components, wires, nets, sheets, buses, board, pads, tracks, vias, zones, zoneFills, ratsnest, courtyards, variants, … Default: components, wires, nets, board. |
| `project_undo` | edit | Takes back the last tool call that changed the design (up to 30 steps; steps: n takes back several). |
| `project_quickstart` | read | The quick start: units, pin addressing, what results carry, the usual flow (the resource sieda://docs/mcp-quickstart, for clients that cannot read resources). |
| `project_plugin_objects` | edit | Objects of a plug-in's own type kept in the design (saved, diffed, merged and undone with it; SiEDA does not interpret them): add {type, plugin, id?, layer?, points: [[x, y], …] in mm, props: {…}}, update {id, layer?, points?, props?} (props merged), remove id. |
| `project_merge` | writes files | Three-way merge of two edited copies of a project against their common ancestor (all files inside the root), written to path. |
| `project_review_comments` | read | Design review comments (pinned to a part and / or a place) with their replies and status, and the review as Markdown. |
| `project_review_comment` | edit | Review commands: add (text, author, ref and / or x, y with view pcb schematic), reply (id, text, author), resolve, reopen or delete (id). |
| `project_diff` | read | What changed between two versions of a project: parts added / removed / changed (value, footprint, placement), nets (pins joined or left, renames), copper per net, board settings and variants. |

### schematic (38)

Schematic capture: parts, wires, labels, sheets, buses, annotation, search, ERC, variants.

| Tool | Kind | What it does |
|---|---|---|
| `schematic_add_component` | edit | Places a built-in component: resistor, capacitor, inductor, diode, led, voltage_source (value "5" or "SIN(0 1 1k)" / "PULSE(0 5 1m)"), current_source, ground, npn, nmos, opamp (pins IN+, IN-, OUT; no supply pins: it swings ±15 V unless its value sets the output limits, e.g. |
| `schematic_remove_component` | edit (destructive) | Deletes a component by designator, with its wires and the net labels / grounds wired to it alone. |
| `schematic_set_value` | edit | Sets a part's value ("4k7", "100n", "Blue", a source's "3.3" or "SIN(0 1 50)"). |
| `schematic_set_package` | edit | Sets the package variant of a passive / diode ("R_0603", "C_1206", "CP_Tant_B", "D_DO41_THT"; "" = default). |
| `schematic_rename` | edit | Changes a part's designator. |
| `schematic_move_component` | edit | Moves a part on the schematic (x, y in grid units; the net labels and grounds wired to it alone move with it, a tag naming a wire from it goes back to the wire's middle) and/or turns it (rotation: absolute degrees; rotate: relative degrees; its own labels turn with it, its grounds stay upright). |
| `schematic_connect` | edit | Draws wires between pins. |
| `schematic_disconnect` | edit (destructive) | Removes the wires between two pins (from, to), every wire at one pin (from only), or a wire by id. |
| `schematic_pin_map` | edit | A part's pin map for firmware and FPGA tools. |
| `schematic_net_label` | edit | Names a net: puts a net label beside each given pin and wires it, so every pin with the same label joins net "net" (also across sheets for scope global). |
| `schematic_connect_to_ground` | edit | Wires each pin to its own ground symbol (net GND, the simulation reference). |
| `schematic_set_no_connect` | edit | Marks pins as deliberately unconnected (ERC stops reporting them), or clears the mark. |
| `schematic_list_components` | read | Every part with designator, kind, value, position, footprint, board placement and the net of each pin. |
| `schematic_list_nets` | read | Every net with its name, role (signal / power / ground) and the part pins on it. |
| `schematic_net_places` | read | Every place a net appears (pins, labels, ports, entries, buses, grounds) across the sheets. |
| `schematic_erc` | read | Runs the ERC: unconnected pins, driver conflicts, missing ground, power pins without a source, hierarchy, bus and directive errors. |
| `schematic_validate` | read | Checks values and ratings (E-series values, decoupling, part ratings from the DC operating point) and reviews the design: an LED with no series resistor, a reset / enable input left floating. |
| `schematic_build` | edit | Builds a circuit in one call: places the parts, joins each named net (net labels on its pins; GND gets ground symbols; a 2 – 3 pin signal net is drawn as wires with one label; a net may also name one pin of a wired chain), wires the wires chains (straight pin-to-pin lines: wired parts are placed in line), flags the noConnect pins, then runs the ERC and the design review. |
| `schematic_import_netlist` | edit | Builds the schematic from another tool's schematic or netlist: a KiCad sheet (.kicad_sch: symbols, wires, labels, power symbols; sub-sheets read from sheets or beside path, hierarchical labels joined to their sheet pins, local labels named /Sheet/NAME), an Eagle schematic (.sch: every sheet, nets by name, gate pins on their pads), an LTspice schematic (.asc: R, C, L, sources, diodes, LEDs), a KiCad netlist (.net), Protel / Tango (Altium, OrCAD, EasyEDA, PADS) or CSV (Net, Designator, Pin). |
| `schematic_set_erc_severity` | edit | Reports an ERC rule as error, warning, info or off ("default" restores it). |
| `schematic_annotate` | edit | Re-numbers designators in reading order (rows or columns), optionally keeping existing ones or numbering by sheet (R101, R201…). |
| `schematic_find` | read | Finds text in designators, values, net labels, net names (and pins) across every sheet. |
| `schematic_replace` | edit | Replaces text in part values and net label names. |
| `schematic_align` | edit | Aligns or distributes parts: left, right, top, bottom, centerX, centerY, distributeX, distributeY. |
| `schematic_sheets` | read | The sheets with hierarchy (parent, depth), component counts, ports and the active sheet. |
| `schematic_block` | writes files | Reusable design blocks. |
| `schematic_sheet_edit` | edit | Sheet commands (multi-page schematics): add (name, parent; activate: true makes the new page the one new parts go to), rename (sheet, name), activate (sheet: new parts go there), remove (sheet, deleteContents), set_parent (sheet, parent), repeat (sheet, count: a multi-channel block), place_entries (sheet = child, x, y: its sheet entries on the parent), size (sheet, size "A4"… or ""). |
| `schematic_move_to_sheet` | edit | Moves parts to another sheet (wires that would cross sheets are removed; join them with labels). |
| `schematic_set_label_scope` | edit | Sets a net label's scope: global, local (its sheet), port (hierarchical port) or entry (sheet entry into targetSheet). |
| `schematic_add_bus` | edit | Draws a named bus ("D[0..7]", "A[15..0]", "D[0..3],WR") on the active sheet; then schematic_bus_connect wires its members to a part. |
| `schematic_bus_connect` | edit | Wires bus members to a part: members named like pins join those pins (D0 → pin D0), else the part's open pins in order, each through a bus entry. |
| `schematic_bus_labels` | edit | Puts one net label per bus member on the given pins of a part, in order (D[0..7] on 8 pins). |
| `schematic_net_class` | edit | Defines (or removes) a net class with a track width and clearance (mm, 0 = board default); assign it to nets with schematic_add_directive. |
| `schematic_add_directive` | edit | Attaches a rule to the net of a pin: a net class, a differential pair (with its X_P / X_N, X_DP / X_DN or X_DM partner), or its own track width / clearance (mm). |
| `schematic_net_rules` | read | The net classes, differential pairs and per-net widths / clearances the schematic gives the board. |
| `schematic_variants` | read | Assembly variants (fitted / DNP parts, value overrides) and the active one. |
| `schematic_variant_matrix` | read | Variants side by side: every part a variant changes with its fitting and value in each variant, and per-variant totals (fitted, not fitted, value changes). |
| `schematic_variant_edit` | edit | Variant commands: add (name, copyFrom), remove (name), rename (name, newName), describe (name, description), activate (name; "" = base design), set_part (name, ref, fitted true/false/null, value). |

### library (19)

Standard-part catalog search, custom parts from pin lists, KiCad / Eagle library import.

| Tool | Kind | What it does |
|---|---|---|
| `library_builtin_kinds` | read | The built-in component kinds (schematic_add_component) with designator prefix, default value, footprint and pin names. |
| `library_search` | read | Searches the standard-part catalog (regulators, MCUs, op-amps, logic, interfaces, sensors, connectors …). |
| `library_part_details` | read | A catalog part's full description: manufacturer, package, units and every pin (number, name, type). |
| `library_add_part` | edit | Places a standard catalog part (an IC, regulator, connector …) on the schematic, or a "Library:Symbol" from the user's own KiCad libraries (library_search libraryParts; its footprint comes with it). |
| `library_create_part` | edit | Creates a part from a datasheet pin list: symbol and footprint are generated from the package. |
| `library_place_custom_part` | edit | Places another instance of a part already in the project library (library_project_parts lists ids). |
| `library_updates` | edit | Project parts whose catalog part has changed since they were added (pins, package, footprint), with how many placed instances use each. |
| `library_project_parts` | read | The custom and catalog parts in the project's library (ids, names, pin counts). |
| `library_compare_footprint` | read | Holds a part's footprint against the datasheet's recommended land pattern: give the datasheet's pads, pitch, padWidth, padLength and span (outer pad edge to outer pad edge, mm), any of them. |
| `library_bulk_edit` | edit | Sets the same fields on every project library part whose name holds match (case-insensitive; "" = all): manufacturer, description, datasheet, refPrefix, defaultValue. |
| `library_check_part` | read | Checks a part spec (as library_create_part takes) without adding it: symbol, land pattern and unit checks, and its library quality score (0 – 100, with what is missing). |
| `library_import` | edit | Imports KiCad (.kicad_sym / .kicad_mod) or Eagle (.lbr) libraries, from files in the root folder (paths) or inline text (files). |
| `library_land_calculator` | read | IPC-7351B pads for a two-sided part from the datasheet's lead dimensions (mm): toe-to-toe span L, heel-to-heel span S and lead width W, min and max each; family gullwing (SOIC, SOP, QFP), chip (0603 and up) or qfn; density most, nominal or least. |
| `library_footprints` | read | The built-in footprints and the project's own: search by words (name or label; "" = all, at most 200) for [{name, label, pads, width, height}], or name one to get its pads as lands {lands, bodySize, bodyDepth} (pad k on pin k + 1, mechanical pads "-") to start a part from (library_create_part, package type CUSTOM). |
| `library_part_spec` | read | A project library part's full spec (library_project_parts ids), as library_create_part takes it: to edit it and save it back with library_replace_part. |
| `library_replace_part` | edit | Saves an edited spec over a project library part: every placed instance takes the new symbol and footprint (same designators and nets by pin number), the old part leaves the library. |
| `library_import_model` | edit | Imports a part's 3D model (STEP / VRML / STL / OBJ file in the root folder). |
| `library_company` | writes files | A team library in a folder inside the root (one <name>.part.json per part, each saved version kept in .history/<name>/). |
| `library_packages` | read | Package types a custom part can use (SOIC, TSSOP, QFN, LQFP, BGA …). |

### pcb (58)

Board outline, stack-up, rules, Update PCB, placement, autorouter, interactive routes, pours, DRC.

| Tool | Kind | What it does |
|---|---|---|
| `pcb_boards` | edit | Several boards in one project (one schematic): list, add (name, activate), activate (board: the pcb_* tools work on it), assign (refs, board: the parts move there unplaced; place them with pcb_update_from_schematic or pcb_autoplace), rename (board, name), remove (board). |
| `pcb_summary` | read | The board: size, outline, layers, rules, every footprint with position / rotation / side and courtyard size [w, h] (mm, as turned), routing progress (tracks, vias, unrouted connections), pours, keep-outs and mounting holes. |
| `pcb_set_board` | edit | Sets the board size (mm), copper layers (1, 2, 4, 6), default track width and clearance, a rule preset (pcb_rule_presets), thickness, solder mask colour and conformal coating. |
| `pcb_set_outline` | edit | Board shape: a polygon (points in mm, ≥ 3; [] = the plain rectangle) or a preset: rectangle (w × h), rounded (corner radius param), circle (diameter w), quad-x (quadcopter frame: span w, body h, arm param). |
| `pcb_fit_board` | edit | Shrinks / grows the board to the placed footprints plus a margin (parts and copper move together). |
| `pcb_set_stackup` | edit | Laminate (fr4, fr4-hightg, isola-370hr, rogers-4350b, megtron-6, polyimide, ims-aluminium), construction (rigid, rigid-flex, metal-core), impedance targets (Ω) and backdrilling. |
| `pcb_rule_presets` | read | Standard design-rule presets (track, clearance, via, annular ring, hole limits) for pcb_set_board. |
| `pcb_set_net_width` | edit | Routes one net with its own track width (mm; 0 removes it). |
| `pcb_eco_preview` | read | What "Update PCB from schematic" would change: footprints to add / remove, nets, pours, rules (nets of a single pin are only counted: singlePinNets). |
| `pcb_update_from_schematic` | edit | Forward annotation: places the footprints of new parts (Auto Place spots), removes parts that are gone, carries net rules, records the baseline. |
| `pcb_autoplace` | edit | Automatic placement: all = true re-places every unlocked footprint (antenna parts and USB / connectors with an open end first, at the top / bottom edge facing out; edges: false skips that); false places only those not on the board yet. |
| `pcb_place_footprint` | edit | Moves / turns / flips footprints (centre x, y in mm): one (ref, x, y …) or many at once (placements: [{ref, x, y, rotation, bottom, force}]). |
| `pcb_suggest_placement` | read | The nearest free legal spot for a footprint that keeps the assembly spacing, close to the pads it connects to (ground counts only for a part on ground alone). |
| `pcb_align` | edit | Lines footprints' courtyards up on the outermost one's edge or centre line (left, right, top, bottom, centreX, centreY), or spaces them evenly between the outermost two (distributeX, distributeY: three or more). |
| `pcb_layout_block` | writes files | Reuses a laid-out group (an approved channel, a regulator block). |
| `pcb_partitions` | edit | Several engineers on one board: set the partitions [{name, owner, x0, y0, x1, y1}] (mm; [] removes them) or list them; check (owner, against = the base project file, e.g. |
| `pcb_routing_policies` | edit | Routing policies per schematic net class: the copper layers its nets may run on (the autorouter keeps them there; other layers only to leave their pads) and the most vias each may use. |
| `pcb_rooms` | edit | Rooms: board areas a group of parts belongs in (a channel, a power stage). |
| `pcb_variant_check` | read | Checks the board as each assembly variant is built (the base design and every variant): its unfitted parts, connections between fitted parts still unrouted, and fitted parts whose courtyards overlap. |
| `pcb_lock_footprint` | edit | Locks footprints where they are (Auto Place and the autorouter's placement keep them) or unlocks them: ref or refs. |
| `pcb_autoroute` | edit | Routes the board. |
| `pcb_autoroute_presets` | read | The autorouter strategy presets with their options, and the board's current strategy. |
| `pcb_route_report` | read | The last autoroute's report: differential pairs, lengths against their rules, quality metrics. |
| `pcb_clear_routing` | edit (destructive) | Removes every track and via. |
| `pcb_route` | edit | Interactive router in one call: a track from one pin's pad to another's, through optional waypoints (mm; "via": true drops a via there and continues on the other side), pushing other nets aside (mode shove) or walking around them. |
| `pcb_route_live` | edit | Drag routing step by step, as in an editor: begin at a pad or track end (or drag / pair / via_drag), then move the head to the cursor (other nets are pushed aside live), fix a corner, via to change layer, and commit (one undo step) or cancel. |
| `pcb_teardrops` | edit | Adds (or removes) teardrops where tracks meet pads / vias, on the given tracks or all of them. |
| `pcb_add_pour` | edit | Adds a copper pour of a net (usually GND) on a layer (0 = top, layers-1 = bottom); plane: true reserves the layer for the net. |
| `pcb_remove_pour` | edit (destructive) | Removes one copper pour by index (pcb_summary zones) or all of them. |
| `pcb_cutouts` | edit | Board cutouts (closed shapes routed out of the board; copper, parts and pours keep the edge clearance from them): add one through points [{x, y}, …] (mm, 3 or more) or remove one by index (pcb_summary cutouts). |
| `pcb_add_mounting_hole` | edit | Adds a non-plated mounting hole with a keep-out (default M3: 3.2 mm drill). |
| `pcb_set_keepouts` | edit | Replaces the routing keep-outs: [{name, x0, y0, x1, y1, layer (-1 = all), tracks, vias, pours}] (mm). |
| `pcb_drc` | read | Runs the DRC (clearances, widths, drills, annular rings, unrouted nets, board edge) and the design-for-reliability checks, and flags a part whose courtyard leaves the board or overlaps another part, or whose pads stand in an antenna / barrier keep-out (DRC_PLACE_*, a forced pose) or stands closer to one than the assembly spacing standard (DRC_PART_SPACING: courtyard gaps of 0.25 mm passive-passive, 0.5 passive-IC, 1.0 IC-IC and through-hole-SMD, 0.5 between through-hole parts, 2.0 round a BGA). |
| `pcb_fanout` | edit | Fans out a component: an escape track and a via on every SMD pad whose net has other pins. |
| `pcb_stitch_vias` | edit | Stitches vias where a net's pours overlap on two or more layers (default the ground net, 5 mm pitch), optionally inside an area; none under a part's courtyard. |
| `pcb_add_via` | edit | Drops a via of a net at a point (mm), e.g. |
| `pcb_add_track` | edit | Draws a track of a net through points (mm) on one copper layer, ends anywhere (a pad, a via, a pour, another track) — for repairs the router's pad-to-pad pcb_route cannot make. |
| `pcb_length_rules` | edit | Length / phase matching: enable serpentine tuning and tolerances, set per-net length rules [{net, target, tolerance}] (target 0 removes), match groups [{name, nets:[…], tolerance}], and tune now. |
| `pcb_pair_rules` | edit | Differential pair rules by the pair's stem (USB_D for USB_D_P / USB_D_N): an interface preset (usb2, usb3, pcie, ethernet, hdmi, lvds, ddr, can: typical impedance and skew) and / or impedance (Ω), width, gap (mm) and skew tolerance (mm); a rule with no values removes it. |
| `pcb_set_hdi` | edit | HDI (IPC-2226): blind / buried vias and laser microvias (drill / pad mm), via-in-pad. |
| `pcb_remove_copper` | edit (destructive) | Deletes tracks and vias by id (ids from project_snapshot sections tracks / vias), or every track and via of a net (net: rip it up, then pcb_autoroute nets [it] routes it again). |
| `pcb_edit_copper` | edit | Edits routed copper by id (project_snapshot tracks / vias): set width (mm), layer or lock on tracks (each refused when it would come too close to another net), split a track at a point, tune its net's length with meanders, merge collinear tracks, gloss (pull the given tracks, or all, tight) or minimise vias (route the named nets again with fewer vias, kept only when better). |
| `pcb_optimize_swaps` | edit | Swaps interchangeable pins and gates to shorten the ratsnest (back-annotated to the schematic), for one part or all. |
| `pcb_add_thermal_vias` | edit | Stitches thermal vias into a power part's largest pad (its own net, clearance kept). |
| `pcb_mechanical_limits` | edit | 3D clearance limits checked by the DRC: tallest part per side (mm, 0 = none) and height zones (rectangles with their own maximum height), and rigid-flex bend areas checked against IPC-2223 (bend radius ≥ 6 / 12 / 24 × the flex thickness for 1 / 2 / more copper layers, 100 × when dynamic; no vias, parts or sharp track corners in the bend). |
| `pcb_dfm_pack` | edit | Manufacturer DFM / DFA rule packs (JLCPCB, PCBWay, OSH Park, Eurocircuits, IPC Class 3). |
| `pcb_custom_rules` | edit | Rule scripting: your own DRC rules as expressions over each track, via, pad, part or net. |
| `si_field_solver` | read | 2D field solver on a stack-up layer's cross-section (Laplace by finite volumes, with and without the dielectric): Z0, εeff, delay, L and C per mm for a track; with gap also odd / even / differential impedance and the backward (kb) and forward (kf) crosstalk coefficients; loss in dB/inch from 0.1 to 25 GHz (skin effect with copper roughness, laminate loss tangent). |
| `pcb_copper_thieving` | edit | Copper thieving: a staggered grid of square dots in the free area of every copper layer, clear of other copper, the edge, holes, keep-outs and outer-layer part courtyards; it evens out plating and the mirror layers' copper (DFM_COPPER_BALANCE). |
| `pcb_graphics` | edit | Logos and free drawings on the board: filled rectangles and lines on silk-top / silk-bottom, copper-<n> (netless copper; the DRC keeps it clear of other copper) or user-1 … user-4 (their own Gerbers in the fabrication package). |
| `pcb_panel` | edit | Panel for the fabrication package: nx × ny boards between rails with fiducials and tooling holes, separated by V-score lines or routed gaps with breakaway tabs and mouse bites. |
| `pcb_import_idf_placement` | edit | Moves parts to the placement in an IDF 3.0 board file (.emn) written back by mechanical CAD (position, rotation, side by designator). |
| `pcb_import_idx` | edit | Applies an IDX (ProSTEP EDMD) baseline or change file written by mechanical CAD: parts moved, rotated and flipped by designator, the board outline and thickness, routing / via keep-outs and component keep-outs (height zones). |
| `pcb_route_compare` | edit | Routes copies of the board with each strategy (pcb_autoroute presets; default: default, fast and quality) and compares them with the board as it is now (before): connections left open, DRC errors and warnings, vias and track length. |
| `pcb_import_dxf` | edit | Sets the board outline and cutouts from a DXF drawing: closed loops of lines, arcs, polylines (with bulges) and circles, on outline-like layers when the file names them (OUTLINE, Edge.Cuts, BOARD …); millimetres, or inches by $INSUNITS. |
| `pcb_import_routes` | edit | Takes the routed copper (tracks, arcs, vias) of another tool's board onto this one: PADS ASCII (.asc), HyperLynx (.hyp) or IPC-2581 (.xml / .cvg, as Allegro and OrCAD PCB Editor export it), detected from the text. |
| `pcb_import_kicad` | edit | Brings a KiCad board (.kicad_pcb, KiCad 6 – 10) into the project. |
| `pcb_reference_layer` | edit | Keeps a Gerber or Excellon drill file over the board as a reference layer (origin at the board's lower-left corner, never fabricated): path adds one; index with visible shows / hides it, with remove deletes it. |

### sim (14)

Circuit simulation: DC, transient, AC, noise, sweeps, Monte Carlo, FFT, measurements, SPICE models.

| Tool | Kind | What it does |
|---|---|---|
| `sim_dc_op` | read | DC operating point of the schematic (the active variant as assembled): every net voltage and each part's current, voltage and dissipation. |
| `sim_transient` | read | Time-domain simulation to stop (s or "10m") with step. |
| `sim_ac` | read | Small-signal frequency sweep: magnitude (dB) and phase per net with metrics (low-frequency gain, peak, f3dB, bandwidth, unity gain, phase margin). |
| `sim_dc_sweep` | read | Sweeps a source from start to stop in step and returns net voltages and part currents along the sweep. |
| `sim_temperature_sweep` | read | DC operating point at each temperature (°C): net voltages and part currents per temperature. |
| `sim_param_sweep` | read | Re-runs an analysis (dc, ac or transient) for each value of one part: {component, values:[…], analysis, net, AC options or stop / step}. |
| `sim_monte_carlo` | read | Monte Carlo and worst case over R / C / L tolerances for a net measure (dc, gain, f3db, peak). |
| `sim_fft` | read | Spectrum and total harmonic distortion of a net over a transient run. |
| `sim_noise` | read | Output noise density and RMS of a net over a sweep, input-referred noise and the largest contributors. |
| `sim_measure` | read | ".meas"-like measurements of a net over a transient run (or of given samples): min, max, peak-to-peak, average, rms, rise / fall time, overshoot, settling time, period, frequency, duty cycle, in a window. |
| `sim_spice_netlist` | read | The schematic as a SPICE netlist. |
| `sim_spice_parse` | read | Parses vendor model text (.model / .subckt / .lib) and lists the models, their ports and diagnostics. |
| `sim_set_spice_model` | edit | Attaches a vendor SPICE model (.model or .subckt from text or a file in the root folder) to a part, with an optional pin map (one entry per port: pin name / number, "0", "net:NAME", "dc:15", "nc"). |
| `sim_builtin_models` | read | Ready-made SPICE models of common parts (names and descriptions; give name for the model text). |

### si_pi (15)

Signal and power integrity: impedance, lengths, crosstalk, channels, PDN, memory design checks.

| Tool | Kind | What it does |
|---|---|---|
| `si_impedance` | read | The stack-up: laminate, εr, loss tangent, every layer with the single-ended and differential track width and gap for the impedance targets. |
| `si_net_list` | read | Signal nets for SI, critical and fast first: length, delay, critical length, driver model, receivers. |
| `si_net_analysis` | read | Transmission-line analysis of a routed net: line sections (Z0, delay), overshoot / undershoot / ringback at each receiver, termination advice; seriesOhms tries a series resistor at the driver. |
| `si_crosstalk` | read | Coupled track pairs with NEXT / FEXT noise against the limit, and return-path problems. |
| `si_checks` | read | Every signal- and power-integrity finding (SI_* / PI_* codes) with counts. |
| `si_length_report` | read | Lengths of differential pairs and buses with their skew against the tolerances, and net length rules / match groups with their status. |
| `si_line_loss` | read | Insertion loss per stack-up layer (dB/inch over frequency, conductor and dielectric parts, RLGC at 1 GHz). |
| `si_channel` | read | Channel of a routed net (a differential pair as a 4-port): S-parameters, step response and an eye diagram at a bit rate with optional CTLE / FFE. |
| `si_assign_model` | edit | Assigns a driver / receiver model (logic family like "lvcmos33", or "ibis:<model>") to a net, a part or a pin ("U1.12"). |
| `si_set_options` | edit | Adds (signOff) the Signal & Power Integrity stage to verification, with overshoot and crosstalk limits (fractions of the swing). |
| `pi_rails` | read | Power distribution of every rail: target impedance, decoupling, plane, worst impedance, IR drop, compliance and recommendations. |
| `pi_set_rail` | edit | Power-integrity inputs of a rail: allowed ripple (%), load step and DC load (A), regulator output resistance (Ω) and loop bandwidth (Hz); 0 derives a value. |
| `pi_decap_plan` | read | Decoupling capacitors to add so a rail meets its target impedance (values, footprints, counts). |
| `pi_ir_drop` | read | DC IR drop of a rail on the board: worst drop against the limit, current density, hot spots. |
| `si_memory_checks` | read | The memory (SDRAM / DDR / LPDDR / DIMM) design segments checked on the design (set the type with design_set_domain domain memory). |

### verify (8)

Design verification and domain (industry) checks.

| Tool | Kind | What it does |
|---|---|---|
| `verify_design` | read | Full design verification: ERC, DC operating point, validation, placement, routing, DRC, manufacturing outputs (and SI/PI when signed off). |
| `design_segments` | read | The design segments of a domain checked on the design: robot, ecu (automotive), aerospace, naval, medical, retail, appliance or memory. |
| `verify_waivers` | edit | Findings accepted at sign-off: each waiver names a check code (e.g. |
| `verify_requirements` | edit | Requirements traceability: each requirement (id, text) names the parts that meet it (refs). |
| `verify_measurements` | edit | Measured results on built boards, linked to the design: add records what was measured (a net, a rail, a test point), its value and unit, optional min / max limits and a note, with today's date and the design fingerprint. |
| `verify_approvals` | edit | Approvals of this design revision (release sign-off). |
| `memory_layout_limits` | edit | DDR layout limits checked on the routed memory bus (byte-lane skew, DQ to DQS, command / address to clock, data impedance tolerance, via spread). |
| `design_set_domain` | edit | Sets the design's domain type, which turns on its checks: robot (rover, fpv, arm, quadruped, humanoid, printer3d, cnc), ecu (bcm, powertrain, adas, ev, chassis, gateway), aerospace (leo, geo, launcher, military, commercial), naval (combatant, carrier, submarine, patrol, commercial), medical (bf, cf, life, implant, home), retail (countertop, unattended, mpos, kiosk, printer), appliance (laundry, kitchen, refrigeration, hvac, small), memory (sdram, ddr, lpddr, dimm, rdimm). |

### output (11)

Fabrication outputs: Gerbers, drill, BOM, pick-and-place, schematic PDF, 3D models.

| Tool | Kind | What it does |
|---|---|---|
| `output_fabrication_package` | writes files | Writes the complete fabrication package into a folder inside the root: gerbers/ (every layer, drills, job file, IPC-D-356A), assembly/ (BOMs, CPL, pick-and-place, drawings), fab notes, a gerber zip, netlist and 3D STL. |
| `output_release` | writes files | A controlled release in a folder inside the root: the fabrication package (fabrication/), the project as released (<name>-rev<revision>.siedaproj), the verification report, the evidence (design fingerprint, approvals of this revision, requirements trace, waivers, bench measurements), a change notice against previous (an older .siedaproj) and MANIFEST.sha256 with every file's SHA-256 (check with shasum -a 256 -c). |
| `output_gerbers` | writes files | Writes RS-274X Gerbers (every copper layer, masks, silkscreens, paste, outline) and Excellon drill files into a folder inside the root. |
| `output_export` | writes files | One output as text, returned inline or written to path (inside the root): spice, bom, bom_assembly, cpl, pnp, assembly_top / assembly_bottom (SVG), gerber_top, gerber_bottom, gerber_l<N>, gerber_mask_top / _bottom, gerber_silk_top / _bottom, gerber_paste_top / _bottom, gerber_edge, gerber_job, drill, drill_npth, ipc356, ipc2581 (IPC-2581C XML), hyperlynx (HyperLynx .hyp for signal-integrity tools), fab_notes, stl, obj, dxf (outline, holes, silk, courtyards), freecad / fusion (FreeCAD macro / Autodesk Fusion script: board and parts by designator; run again it writes the moved placement as IDF for pcb_import_idf_placement), testpoints (probe point per net, CSV), html_bom (interactive BOM / board viewer), bringup (bring-up checklist, Markdown), fab_drawing (fabrication drawing PDF: dimensions, drill map / table, stack-up, revisions), net_lengths (routed length per net, CSV). |
| `output_bom` | writes files | The bill of materials (grouped lines, quantities, MPNs, cost) as JSON, or CSV written to path. |
| `output_pick_and_place` | writes files | Pick-and-place / centroid file (designator, x, y, rotation, side) as CSV, inline or written to path. |
| `output_schematic_pdf` | writes files | The schematic as a PDF (one page per sheet with frame and title block, bookmarks by hierarchy), written to path inside the root, or returned as an embedded PDF resource. |
| `output_3d_model` | writes files | The assembled board for mechanical CAD written to path inside the root: STEP AP214 solids (board + one named body per part), IDF 3.0 board (.emn) or library (.emp), an IDX (ProSTEP EDMD v4.5) baseline, or a mesh (STL, OBJ, VRML 2.0, glTF 2.0 binary .glb). |
| `output_fab_quote` | read | What a board house's order form asks, read from the board (layers, size, area, thickness, material, copper, finish, mask / silk colours, minimum track / space / drill / ring, hole and via counts, HDI, via-in-pad, impedance control, panel), checked against the fab's published limits (JLCPCB, PCBWay, OSH Park, Eurocircuits; Lion Circuits and PCB Power (India) have no published pack: check their capability page), its order page and steps. |
| `output_idx_response` | writes files | Writes the IDX response to a change file from MCAD: every change it proposes accepted or rejected. |
| `output_idx_changes` | writes files | Writes an IDX SendChanges file with only the parts placed differently from a baseline IDX (the one MCAD already has). |

### render (3)

Pictures of the schematic and the board (PNG or SVG).

| Tool | Kind | What it does |
|---|---|---|
| `render_schematic` | read | A picture of one schematic sheet: PNG image (default) or SVG. |
| `render_pcb` | read | A picture of the board: PNG image (default) or SVG. |
| `render_3d` | read | A shaded 3D picture of the assembled board (PNG): board, copper finish, solder mask, silkscreen and every part's body, seen from the given side or any angle; parts can be hidden or isolated, lifted off (explode) or the board cut through (section). |

