# SiEDA Architecture

```
┌─────────────────────────────── SwiftUI app (SiEDA/) ───────────────────────────────┐
│ Prompt Studio · Schematic · PCB · 3D · Simulation · Checks · Inspector · Settings  │
│                     ▲ @Published DesignSnapshot / results                          │
│ DesignStore (MainActor: undo stack, documents, analysis results, workspace)        │
│ AgentOrchestrator ── AIProvider { Claude* | OpenAI | Gemini | Ollama | Offline }   │
│                     │ DesignPlan (JSON schema-constrained)                         │
│ DesignPlanCompiler ─┘                                                              │
│ EDAEngine (thread-safe Swift façade, NSLock)                                       │
└───────────────────────────────┬────────────────────────────────────────────────────┘
                                │ C ABI: sieda_c.h (opaque handles, JSON strings, float arrays)
┌───────────────────────────────▼──────────── C++17 core (Core/) ────────────────────┐
│ Project ── Schematic (components, wires, nets, ERC)                                │
│        ├── Simulator (MNA, Newton–Raphson, DC + transient)                         │
│        ├── PcbLayout (pads, auto-place, A* autorouter, DRC, ratsnest)              │
│        ├── Mesh (3D assembly)       ├── Export (Gerber, Excellon, SPICE, BOM, STL) │
│        └── Json (persistence + UI snapshots)                                       │
└────────────────────────────────────────────────────────────────────────────────────┘
```

## Why a C ABI instead of direct Swift/C++ interop?

The core exposes `extern "C"` functions behind opaque handles. Results come back as UTF-8 JSON that Swift
decodes with `Codable`. Mesh data comes back as raw `float`/`uint32_t` arrays that go straight into SceneKit.
This gives us:

- **Stability.** The ABI is independent of the Swift compiler's C++ interop mode and of the libc++ version.
- **Testability.** The same ABI is exercised from C (`Core/tests/c_api_test.c`), from Swift (XCTest) and from Python (ctypes).
- **Safety.** Every entry point catches exceptions, and errors are returned as values.

## Core engine

### Schematic and connectivity
- Components reference library definitions (`Library.cpp`) by stable numeric kind. The Swift
  `ComponentKind` raw values mirror these, and `SiEDATests` checks that the two stay in sync.
- Nets are computed lazily with union-find. Wires join pins, identically named net labels join nets, and
  ground symbols (or labels named `GND`) join the ground net.
- ERC rules:
  - missing ground
  - unconnected pins and floating parts
  - shorted sources
  - duplicate designators
  - invalid values
  - LEDs driven without current limiting
  - parts that have no simulation model

### Simulator
- Modified Nodal Analysis with dense LU and partial pivoting.
- Non-linear devices use Newton–Raphson. Device Jacobians are central finite differences of the terminal
  current functions. Exponentials are linearised past a limit (`limexp`) and node-voltage steps are damped.
- If Newton fails to converge, the solver falls back to gmin stepping and then source stepping.
- Transient analysis uses backward-Euler companion models for C and L, and sub-steps on non-convergence.

### PCB
- **Auto-placement.** A greedy, connectivity-ordered placer. Each part goes where the Manhattan distance from
  its pads to the centroids of their nets is lowest. It tries 0° and 90° rotations and keeps courtyards
  apart with routing channels.
- **Autorouter.** A two-layer grid A* (0.25 mm pitch by default):
  - Moves can be orthogonal or 45°. Each layer has a preferred direction, turns cost extra, and vias are penalised.
  - Each net is built as a Steiner-like tree: every new connection may start from any copper already on that net.
  - Keep-out grids enforce clearance: track to track, track to pad, and via to everything.
  - Failed nets are promoted to the front and the board is re-routed (up to 4 passes); the best result is kept.
- **DRC.** Exact geometric checks on the routed copper (segment–segment, segment–rectangle and point–circle
  distances), edge clearance, courtyard overlap, and unrouted connections. Unrouted connections are found
  from copper connectivity, not from router bookkeeping.
- **Edits after routing.** After schematic edits, copper is re-associated with nets through its contact with
  pads. Copper that no longer touches any pad is pruned.

## AI agents

| Agent | Input | Output (JSON schema) |
|---|---|---|
| Requirements Analyst | prompt / PRD | `RequirementsSpec` |
| Circuit Architect | brief + spec (or current plan + change request) | `DesignPlan` |
| Plan Compiler (core) | `DesignPlan` | schematic + `PlanApplyReport` |
| Verification (core) | schematic | ERC findings + DC operating point |
| Design Reviewer | spec + plan + ERC + DC report | `DesignReview { approved, issues, plan }` |
| PCB Layout (core) | schematic | placement, routing, DRC |

Agents never edit the design directly. Every model response is a schema-constrained `DesignPlan`, and the
deterministic core validates it, simulates it and lays it out. The reviewer agent then sees the real
simulation numbers, not the model's own estimates.

### Claude integration (default)
The app sends `POST https://api.anthropic.com/v1/messages` with:
- `model`: `claude-opus-5-5` by default
- `output_config.format = {type: "json_schema", schema}` (structured outputs)
- `output_config.effort` (default `high`)
- `thinking: {type: "adaptive"}`

On models that support it, the request also sends `fallbacks: "default"` with the beta header
`server-side-fallback-2026-07-01`. The app checks `stop_reason` for `refusal` and `max_tokens` before it
reads any content. API keys are stored in the Keychain.

## UI composition
- **Photoshop:** the `ToolStrip`, the `OptionsBar` and the PCB `LayersPanel` (eye toggles, active layer).
- **Altium:** the Properties inspector, the `LayerTabs` along the bottom of the PCB editor, and the Checks
  panel with cross-probing to the affected parts.
- **Proteus:** the `DevicePicker` with symbol preview, the `SimulationTransport` controls and live DC probes
  drawn on schematic nets.
- **Theme:** `Theme.swift` defines the palette. The app is dark by default and blue throughout; warm colours
  are kept only for warnings and errors.
