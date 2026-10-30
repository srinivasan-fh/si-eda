<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# Lessons, team training and personal shortcuts

## Lessons

**Help → Lessons…** (macOS app and SiEDA Studio) lists step-by-step exercises. Start one and SiEDA sets up its
project; each step says what to do and gives a hint. **Check My Work** checks the step against your design (the
real ERC, DRC, route report…) and moves on when it passes, or says what is still missing. **Show Me** does the step
for you (one undo step, so you can undo it and try yourself).

Built in:

| Lesson | Steps |
|---|---|
| Your first circuit | Place a source, resistor and LED; wire the loop; ground it until the ERC is clean |
| Route your first differential pair | Put two connectors on the board; route DATA_P / DATA_N as one coupled pair; DRC |
| From schematic to a board you can order | Place, route, DRC and verify the LED indicator example |

AI clients use the same lessons through the MCP tool `project_lessons` (`list`, `start`, `check`, `show`, `status`, `stop`,
`validate`).

## Your company's own lessons

A company ships its own lessons — its design rules, its review steps, a walkthrough of its team rule pack — as
`<name>.lesson.json` files:

- in the team folder's `Lessons/` folder (macOS: the shared library folder in Settings → Library; Studio: Help →
  Lessons… → Team folder), or
- in a folder the server is started with: `sieda-mcp --lessons-dir <dir>`, `sieda-studio --lessons-dir <dir>`, or
  `SIEDA_LESSONS_DIRS` (several folders separated by `:`, `;` on Windows).

They appear beside the built-in lessons, marked *team* or *organisation*.

### The file

```json
{
  "format": "sieda.lesson/1",
  "id": "acme_rules",
  "title": "ACME board rules",
  "summary": "Our three release rules on the LED example.",
  "level": "beginner",
  "start": [{"tool": "project_load_example", "args": {"id": "led_indicator"}},
            {"tool": "pcb_rule_pack", "args": {"action": "apply", "path": "team.rules.json"}}],
  "steps": [
    {"title": "Route it",
     "text": "Place and route the board. ACME ships no board with an open connection.",
     "hint": "Design → Auto Place, then Auto Route.",
     "check": [{"tool": "project_summary", "path": "board.unroutedConnections", "equals": 0},
               {"tool": "project_summary", "path": "board.tracks", "atLeast": 1, "message": "Nothing is routed yet."}],
     "solution": [{"tool": "pcb_update_from_schematic"}, {"tool": "pcb_autoplace"}, {"tool": "pcb_autoroute"}]},
    {"title": "Rules clean",
     "text": "Run the DRC: no errors under the ACME rule pack.",
     "check": [{"tool": "pcb_drc", "path": "errors", "equals": 0},
               {"tool": "verify_design", "path": "stages", "excludes": "VER_RULE_PACK"}]}
  ]
}
```

- `start` and `solution` are lists of MCP tool calls ([MCP.md](MCP.md#tool-reference)); `start` runs when the lesson
  starts, a step's `solution` runs on **Show Me** (leave it out to have none).
- Every entry of `check` must pass. It runs a **read-only** tool (`project_summary`, `schematic_erc`, `pcb_drc`,
  `pcb_route_report`, `verify_design`, …) and looks at its result at `path`: keys and array indexes split by `.`,
  `length` = the size of a list or object. Then one of: `equals` (a number, text, true / false), `atLeast`, `atMost`,
  `contains` / `excludes` (text found in the value as JSON), `exists` (true / false). `message` is what the learner
  sees when it fails.
- `validate` checks a file before you share it: `project_lessons {"action": "validate", "path": "team/Lessons/acme_rules.lesson.json"}`
  (or without `path`: every lesson the server sees). It reports unknown tools, checks through tools that change the
  design, steps without text or a check.

## Personal shortcuts

SiEDA notices commands you keep choosing from the menus:

- **macOS app:** a command that has a shortcut shows it in the status line every third time you pick it from the
  menu (Tip: ⇧⌘R — Auto Route Board). One without a shortcut (Auto-Place Footprints, Run Design Rule Check) is offered
  a free ⌃⌥1…9 once, after four menu picks; Settings → Shortcuts changes it at any time.
- **SiEDA Studio:** the same tips for Ctrl+S / Z / Shift+Z / K; any other menu item you choose four times is offered a
  quick key Alt+1…9 (kept in the browser), listed under Help → Keyboard Shortcuts.
