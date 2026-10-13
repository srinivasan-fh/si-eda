# Security policy

## Reporting a vulnerability

Please report security problems privately, not in a public issue: use GitHub's **Report a vulnerability**
(Security → Advisories) on this repository. Include what you found, how to reproduce it (a project file, an MCP
request, a library file) and what an attacker could do with it.

We answer within 5 working days, agree a fix date with you, and credit you in the release notes unless you prefer
otherwise.

## What is in scope

- File readers: projects, KiCad / Eagle / Altium libraries, STEP / VRML / STL / OBJ models, IDF / IDX, DXF, SPICE
  models, GGUF model files (fuzzed in CI — `Core/tests/fuzz/`).
- The MCP server (`sieda-mcp`): root-folder sandbox, read-only mode, the 128 MB read cap.
- The app's live MCP endpoint: 127.0.0.1 only, bearer token, Origin / Host checks, 16 connections.
- Supplier API keys (kept in the macOS Keychain, never in project files).

## Supported versions

Fixes go to `main`; there are no maintained release branches yet.
