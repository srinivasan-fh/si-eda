<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <b>Documentation</b> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# SiEDA documentation

Every guide in one place, grouped by what you are doing. Each one starts with its own short overview.

## Start here

| Guide | Read it to |
|---|---|
| [Features in detail](FEATURES.md) | See everything SiEDA does, with menus, shortcuts and set-up steps |
| [Moving from KiCad, Altium or Eagle](MIGRATING.md) | Bring your libraries, netlists and boards across |
| [Product requirements](PRD.md) | Understand what SiEDA is built to do |
| [Architecture](ARCHITECTURE.md) | Learn how the SwiftUI app, the C ABI and the C++ engine fit together |

## Design

| Guide | Covers |
|---|---|
| [Schematic](SCHEMATIC.md) | Sheets, hierarchy, repeated channels, buses, variants, annotation, colour schemes |
| [Simulation](SIMULATION.md) | DC / AC / transient / noise, SPICE model import, temperature sweeps, measurements |
| [Memory design](MEMORY_DESIGN.md) | SDRAM / DDR segments, layout rules, skew checks |
| [Signal & power integrity](SIGNAL_POWER_INTEGRITY.md) | Stack-up, impedance, field solver, PDN and IR drop |

## Board layout

| Guide | Covers |
|---|---|
| [Interactive routing](INTERACTIVE_ROUTING.md) | Push-and-shove, arcs, differential pairs, length tuning, teardrops |
| [Auto place, auto route and DRC](ROUTING.md) | Strategies, performance, limits, the routing report |
| [Mechanical CAD](MCAD.md) | STEP, IDF, IDX exchange, 3D clearance, board objects |

## Libraries and parts

| Guide | Covers |
|---|---|
| [Footprint Editor](FOOTPRINT_EDITOR.md) | Land patterns, pad stacks, checks, the footprint library |
| [Symbol Editor](SYMBOL_EDITOR.md) | Four-sided symbols, auto-arrange, multi-unit parts |
| [Library import](LIBRARY_IMPORT.md) | KiCad, Eagle and Altium libraries |
| [Suppliers](SUPPLIERS.md) | Live part search and BOM pricing |
| [Missing parts](missing-parts/README.md) | Catalog parts still waiting for vendor pinouts |

## AI and automation

| Guide | Covers |
|---|---|
| [AI back-ends](AI.md) | Cloud models, the built-in on-device model, SiEDA's own circuit model |
| [MCP server](MCP.md) | Drive SiEDA from Claude, Cursor, VS Code or any MCP client; full tool reference |
| [CI templates](ci/) | Verify designs in GitHub Actions or GitLab CI |

## Team and platform

| Guide | Covers |
|---|---|
| [Team work](TEAM.md) | Version compare, Git diff / merge drivers, variants, review comments, co-editing |
| [File format](FILE_FORMAT.md) | The `.siedaproj` project file |
| [Interface languages](LOCALIZATION.md) | The 20 interface languages and how to add strings |
| [EDA checklist](EDA_CHECKLIST.md) | SiEDA against a best-in-class PCB EDA checklist |
| [Roadmap](ROADMAP.md) | What is done and what comes next |

---

<p align="center"><a href="../README.md">← Back to the SiEDA home page</a></p>
