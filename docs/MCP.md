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
| Tool table | `Core/src/McpTools.cpp` | 132 tools in 9 groups, each with a description, a JSON Schema and MCP annotations; handlers call the C API (`sieda_c.h`). Data-driven: `mcpTools()`. |
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

## Embedding the engine (C API)

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
  Gerbers in the root).
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

### project (15)

Create, open, save and inspect projects; built-in examples.

| Tool | Kind | What it does |
|---|---|---|
| `project_new` | edit (destructive) | Starts a new, empty project (replacing the open one; save first if needed). |
| `project_open` | edit (destructive) | Opens a .siedaproj file from the server's root folder (relative paths resolve against it), replacing the open project. |
| `project_save` | writes files | Saves the project to the file it was opened from / last saved to (or to "path"), inside the root folder. |
| `project_save_as` | writes files | Saves the project to a new .siedaproj file inside the root folder; later saves go there. |
| `project_export_json` | read | The whole project as saved (.siedaproj JSON): schematic, library, board, rules, variants. |
| `project_list_examples` | read | The built-in example designs (ids for project_load_example). |
| `project_load_example` | edit (destructive) | Replaces the open project with a built-in example design (schematic and board size; route it with pcb_update_from_schematic and pcb_autoroute). |
| `project_summary` | read | Where the design stands: name, file, part / net / sheet counts, board size and layers, placement and routing progress, and the session (read-only, root folder). |
| `project_set_info` | edit | Sets the project name, the requirements text, the industry profile and title-block fields (any subset). |
| `project_industry_profiles` | read | Industry profiles (rule preset, derating, standards, guidance) for project_new / project_set_info. |
| `project_snapshot` | read | Raw view-model sections of the design (as the app draws it): components, wires, nets, sheets, buses, board, pads, tracks, vias, zones, zoneFills, ratsnest, courtyards, variants, … Default: components, wires, nets, board. |
| `project_merge` | writes files | Three-way merge of two edited copies of a project against their common ancestor (all files inside the root), written to path. |
| `project_review_comments` | read | Design review comments (pinned to a part and / or a place) with their replies and status, and the review as Markdown. |
| `project_review_comment` | edit | Review commands: add (text, author, ref and / or x, y with view pcb schematic), reply (id, text, author), resolve, reopen or delete (id). |
| `project_diff` | read | What changed between two versions of a project: parts added / removed / changed (value, footprint, placement), nets (pins joined or left, renames), copper per net, board settings and variants. |

### schematic (34)

Schematic capture: parts, wires, labels, sheets, buses, annotation, search, ERC, variants.

| Tool | Kind | What it does |
|---|---|---|
| `schematic_add_component` | edit | Places a built-in component: resistor, capacitor, inductor, diode, led, voltage_source (value "5" or "SIN(0 1 1k)" / "PULSE(0 5 1m)"), current_source, ground, npn, nmos, opamp (pins IN+, IN-, OUT), switch, connector, ic8, fuse, net_label, battery, ac_source. |
| `schematic_remove_component` | edit (destructive) | Deletes a component (and its wires) by designator. |
| `schematic_set_value` | edit | Sets a part's value ("4k7", "100n", "Blue", a source's "3.3" or "SIN(0 1 50)"). |
| `schematic_set_package` | edit | Sets the package variant of a passive / diode ("R_0603", "C_1206", "CP_Tant_B", "D_DO41_THT"; "" = default). |
| `schematic_rename` | edit | Changes a part's designator. |
| `schematic_move_component` | edit | Moves a part on the schematic (x, y in grid units) and/or turns it (rotation: absolute degrees; rotate: relative degrees). |
| `schematic_connect` | edit | Draws wires between pins. |
| `schematic_disconnect` | edit (destructive) | Removes the wires between two pins (from, to), every wire at one pin (from only), or a wire by id. |
| `schematic_net_label` | edit | Names a net: puts a net label beside each given pin and wires it, so every pin with the same label joins net "net" (also across sheets for scope global). |
| `schematic_connect_to_ground` | edit | Wires each pin to its own ground symbol (net GND, the simulation reference). |
| `schematic_set_no_connect` | edit | Marks a pin as deliberately unconnected (ERC stops reporting it), or clears the mark. |
| `schematic_list_components` | read | Every part with designator, kind, value, position, footprint, board placement and the net of each pin. |
| `schematic_list_nets` | read | Every net with its name, role (signal / power / ground) and the part pins on it. |
| `schematic_net_places` | read | Every place a net appears (pins, labels, ports, entries, buses, grounds) across the sheets. |
| `schematic_erc` | read | Runs the ERC: unconnected pins, driver conflicts, missing ground, power pins without a source, hierarchy, bus and directive errors. |
| `schematic_validate` | read | Checks values and ratings: E-series values, decoupling, part ratings from the DC operating point. |
| `schematic_set_erc_severity` | edit | Reports an ERC rule as error, warning, info or off ("default" restores it). |
| `schematic_annotate` | edit | Re-numbers designators in reading order (rows or columns), optionally keeping existing ones or numbering by sheet (R101, R201…). |
| `schematic_find` | read | Finds text in designators, values, net labels, net names (and pins) across every sheet. |
| `schematic_replace` | edit | Replaces text in part values and net label names. |
| `schematic_align` | edit | Aligns or distributes parts: left, right, top, bottom, centerX, centerY, distributeX, distributeY. |
| `schematic_sheets` | read | The sheets with hierarchy (parent, depth), component counts, ports and the active sheet. |
| `schematic_sheet_edit` | edit | Sheet commands: add (name, parent), rename (sheet, name), activate (sheet: new parts go there), remove (sheet, deleteContents), set_parent (sheet, parent), repeat (sheet, count: a multi-channel block), place_entries (sheet = child, x, y: its sheet entries on the parent), size (sheet, size "A4"… or ""). |
| `schematic_move_to_sheet` | edit | Moves parts to another sheet (wires that would cross sheets are removed; join them with labels). |
| `schematic_set_label_scope` | edit | Sets a net label's scope: global, local (its sheet), port (hierarchical port) or entry (sheet entry into targetSheet). |
| `schematic_add_bus` | edit | Draws a named bus ("D[0..7]", "A[15..0]", "D[0..3],WR") on the active sheet; then schematic_bus_connect wires its members to a part. |
| `schematic_bus_connect` | edit | Wires bus members to a part: members named like pins join those pins (D0 → pin D0), else the part's open pins in order, each through a bus entry. |
| `schematic_bus_labels` | edit | Puts one net label per bus member on the given pins of a part, in order (D[0..7] on 8 pins). |
| `schematic_net_class` | edit | Defines (or removes) a net class with a track width and clearance (mm, 0 = board default); assign it to nets with schematic_add_directive. |
| `schematic_add_directive` | edit | Attaches a rule to the net of a pin: a net class, a differential pair (with its X_P / X_N partner), or its own track width / clearance (mm). |
| `schematic_net_rules` | read | The net classes, differential pairs and per-net widths / clearances the schematic gives the board. |
| `schematic_variants` | read | Assembly variants (fitted / DNP parts, value overrides) and the active one. |
| `schematic_variant_matrix` | read | Variants side by side: every part a variant changes with its fitting and value in each variant, and per-variant totals (fitted, not fitted, value changes). |
| `schematic_variant_edit` | edit | Variant commands: add (name, copyFrom), remove (name), rename (name, newName), describe (name, description), activate (name; "" = base design), set_part (name, ref, fitted true/false/null, value). |

### library (10)

Standard-part catalog search, custom parts from pin lists, KiCad / Eagle library import.

| Tool | Kind | What it does |
|---|---|---|
| `library_builtin_kinds` | read | The built-in component kinds (schematic_add_component) with designator prefix, default value, footprint and pin names. |
| `library_search` | read | Searches the standard-part catalog (regulators, MCUs, op-amps, logic, interfaces, sensors, connectors …). |
| `library_part_details` | read | A catalog part's full description: manufacturer, package, units and every pin (number, name, type). |
| `library_add_part` | edit | Places a standard catalog part (an IC, regulator, connector …) on the schematic. |
| `library_create_part` | edit | Creates a part from a datasheet pin list: symbol and footprint are generated from the package. |
| `library_place_custom_part` | edit | Places another instance of a part already in the project library (library_project_parts lists ids). |
| `library_project_parts` | read | The custom and catalog parts in the project's library (ids, names, pin counts). |
| `library_check_part` | read | Checks a part spec (as library_create_part takes) without adding it: symbol, land pattern and unit checks. |
| `library_import` | edit | Imports KiCad (.kicad_sym / .kicad_mod) or Eagle (.lbr) libraries, from files in the root folder (paths) or inline text (files). |
| `library_packages` | read | Package types a custom part can use (SOIC, TSSOP, QFN, LQFP, BGA …). |

### pcb (33)

Board outline, stack-up, rules, Update PCB, placement, autorouter, interactive routes, pours, DRC.

| Tool | Kind | What it does |
|---|---|---|
| `pcb_summary` | read | The board: size, outline, layers, rules, every footprint with position / rotation / side, routing progress (tracks, vias, unrouted connections), pours, keep-outs and mounting holes. |
| `pcb_set_board` | edit | Sets the board size (mm), copper layers (1, 2, 4, 6), default track width and clearance, a rule preset (pcb_rule_presets), thickness, solder mask colour and conformal coating. |
| `pcb_set_outline` | edit | Board shape: a polygon (points in mm, ≥ 3; [] = the plain rectangle) or a preset: rectangle (w × h), rounded (corner radius param), circle (diameter w), quad-x (quadcopter frame: span w, body h, arm param). |
| `pcb_fit_board` | edit | Shrinks / grows the board to the placed footprints plus a margin (parts and copper move together). |
| `pcb_set_stackup` | edit | Laminate (fr4, fr4-hightg, isola-370hr, rogers-4350b, megtron-6, polyimide, ims-aluminium), construction (rigid, rigid-flex, metal-core), impedance targets (Ω) and backdrilling. |
| `pcb_rule_presets` | read | Standard design-rule presets (track, clearance, via, annular ring, hole limits) for pcb_set_board. |
| `pcb_set_net_width` | edit | Routes one net with its own track width (mm; 0 removes it). |
| `pcb_eco_preview` | read | What "Update PCB from schematic" would change: footprints to add / remove, nets, pours, rules. |
| `pcb_update_from_schematic` | edit | Forward annotation: places the footprints of new parts (Auto Place spots), removes parts that are gone, carries net rules, records the baseline. |
| `pcb_autoplace` | edit | Automatic placement: all = true re-places every unlocked footprint; false places only those not on the board yet. |
| `pcb_place_footprint` | edit | Moves / turns / flips one footprint (centre x, y in mm). |
| `pcb_suggest_placement` | read | The nearest free legal spot for a footprint, close to the pads it connects to. |
| `pcb_lock_footprint` | edit | Locks a footprint where it is (Auto Place and the autorouter's placement keep it) or unlocks it. |
| `pcb_autoroute` | edit | Routes the board. |
| `pcb_autoroute_presets` | read | The autorouter strategy presets with their options, and the board's current strategy. |
| `pcb_route_report` | read | The last autoroute's report: differential pairs, lengths against their rules, quality metrics. |
| `pcb_clear_routing` | edit (destructive) | Removes every track and via. |
| `pcb_route` | edit | Interactive router in one call: a track from one pin's pad to another's, through optional waypoints (mm; "via": true drops a via there and continues on the other side), pushing other nets aside (mode shove) or walking around them. |
| `pcb_teardrops` | edit | Adds (or removes) teardrops where tracks meet pads / vias, on the given tracks or all of them. |
| `pcb_add_pour` | edit | Adds a copper pour of a net (usually GND) on a layer (0 = top, layers-1 = bottom); plane: true reserves the layer for the net. |
| `pcb_remove_pour` | edit (destructive) | Removes one copper pour by index (pcb_summary zones) or all of them. |
| `pcb_add_mounting_hole` | edit | Adds a non-plated mounting hole with a keep-out (default M3: 3.2 mm drill). |
| `pcb_set_keepouts` | edit | Replaces the routing keep-outs: [{name, x0, y0, x1, y1, layer (-1 = all), tracks, vias}]. |
| `pcb_drc` | read | Runs the DRC (clearances, widths, drills, annular rings, unrouted nets, board edge) and the design-for-reliability checks. |
| `pcb_fanout` | edit | Fans out a component: an escape track and a via on every SMD pad whose net has other pins. |
| `pcb_stitch_vias` | edit | Stitches vias where a net's pours overlap on two or more layers (default the ground net, 2 mm pitch), optionally inside an area. |
| `pcb_length_rules` | edit | Length / phase matching: enable serpentine tuning and tolerances, set per-net length rules [{net, target, tolerance}] (target 0 removes), match groups [{name, nets:[…], tolerance}], and tune now. |
| `pcb_set_hdi` | edit | HDI (IPC-2226): blind / buried vias and laser microvias (drill / pad mm), via-in-pad. |
| `pcb_remove_copper` | edit (destructive) | Deletes tracks and vias by id (ids from project_snapshot sections tracks / vias). |
| `pcb_optimize_swaps` | edit | Swaps interchangeable pins and gates to shorten the ratsnest (back-annotated to the schematic), for one part or all. |
| `pcb_add_thermal_vias` | edit | Stitches thermal vias into a power part's largest pad (its own net, clearance kept). |
| `pcb_mechanical_limits` | edit | 3D clearance limits checked by the DRC: tallest part per side (mm, 0 = none) and height zones (rectangles with their own maximum height). |
| `pcb_import_idf_placement` | edit | Moves parts to the placement in an IDF 3.0 board file (.emn) written back by mechanical CAD (position, rotation, side by designator). |

### sim (13)

Circuit simulation: DC, transient, AC, noise, sweeps, Monte Carlo, FFT, measurements, SPICE models.

| Tool | Kind | What it does |
|---|---|---|
| `sim_dc_op` | read | DC operating point of the schematic (the active variant as assembled): every net voltage and each part's current, voltage and dissipation. |
| `sim_transient` | read | Time-domain simulation to stop (s or "10m") with step. |
| `sim_ac` | read | Small-signal frequency sweep: magnitude (dB) and phase per net with metrics (low-frequency gain, peak, f3dB, bandwidth, unity gain, phase margin). |
| `sim_dc_sweep` | read | Sweeps a source from start to stop in step and returns net voltages and part currents along the sweep. |
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

### verify (3)

Design verification and domain (industry) checks.

| Tool | Kind | What it does |
|---|---|---|
| `verify_design` | read | Full design verification: ERC, DC operating point, validation, placement, routing, DRC, manufacturing outputs (and SI/PI when signed off). |
| `design_segments` | read | The design segments of a domain checked on the design: robot, ecu (automotive), aerospace, naval, medical, retail, appliance or memory. |
| `design_set_domain` | edit | Sets the design's domain type, which turns on its checks: robot (rover, fpv, arm, quadruped, humanoid, printer3d, cnc), ecu (bcm, powertrain, adas, ev, chassis, gateway), aerospace (leo, geo, launcher, military, commercial), naval (combatant, carrier, submarine, patrol, commercial), medical (bf, cf, life, implant, home), retail (countertop, unattended, mpos, kiosk, printer), appliance (laundry, kitchen, refrigeration, hvac, small), memory (sdram, ddr, lpddr, dimm, rdimm). |

### output (7)

Fabrication outputs: Gerbers, drill, BOM, pick-and-place, schematic PDF, 3D models.

| Tool | Kind | What it does |
|---|---|---|
| `output_fabrication_package` | writes files | Writes the complete fabrication package into a folder inside the root: gerbers/ (every layer, drills, job file, IPC-D-356A), assembly/ (BOMs, CPL, pick-and-place, drawings), fab notes, a gerber zip, netlist and 3D STL. |
| `output_gerbers` | writes files | Writes RS-274X Gerbers (every copper layer, masks, silkscreens, paste, outline) and Excellon drill files into a folder inside the root. |
| `output_export` | writes files | One output as text, returned inline or written to path (inside the root): spice, bom, bom_assembly, cpl, pnp, assembly_top / assembly_bottom (SVG), gerber_top, gerber_bottom, gerber_l<N>, gerber_mask_top / _bottom, gerber_silk_top / _bottom, gerber_paste_top / _bottom, gerber_edge, gerber_job, drill, drill_npth, ipc356, fab_notes, stl, obj. |
| `output_bom` | writes files | The bill of materials (grouped lines, quantities, MPNs, cost) as JSON, or CSV written to path. |
| `output_pick_and_place` | writes files | Pick-and-place / centroid file (designator, x, y, rotation, side) as CSV, inline or written to path. |
| `output_schematic_pdf` | writes files | The schematic as a PDF (one page per sheet with frame and title block, bookmarks by hierarchy), written to path inside the root, or returned as an embedded PDF resource. |
| `output_3d_model` | writes files | The assembled board for mechanical CAD written to path inside the root: STEP AP214 solids (board + one named body per part), IDF 3.0 board (.emn) or library (.emp), or a mesh (STL, OBJ). |

### render (2)

Pictures of the schematic and the board (PNG or SVG).

| Tool | Kind | What it does |
|---|---|---|
| `render_schematic` | read | A picture of one schematic sheet: PNG image (default) or SVG. |
| `render_pcb` | read | A picture of the board: PNG image (default) or SVG. |

