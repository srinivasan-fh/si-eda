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

## Verifying a release

A release (tag `v<version>`, `.github/workflows/release.yml`) carries the app as a disk image signed with a
Developer ID and notarised by Apple when the signing secrets are set (ad-hoc signed otherwise, and the workflow says
so), the CLI / MCP server for Linux and macOS, a CycloneDX SBOM (`sieda-<version>.cdx.json`, `tools/make_sbom.py`) and
`SHA256SUMS`. Check a download with `shasum -a 256 -c SHA256SUMS` and the app with
`codesign --verify --strict --verbose=2 SiEDA.app` / `spctl -a -v SiEDA.app`. The Linux CLI is built twice in
different folders and the release stops unless both builds are byte-identical. **Help → Check for Updates…** (or the
daily check in Settings, off by default) asks GitHub for the latest release and opens its page; nothing is installed
without you.

## Supported versions

Fixes go to `main`; there are no maintained release branches yet.
