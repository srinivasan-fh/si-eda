<p align="center"><a href="../README.md">Home</a> · <a href="FEATURES.md">Features</a> · <a href="README.md">Documentation</a> · <a href="MCP.md">MCP server</a> · <a href="ARCHITECTURE.md">Architecture</a></p>

# Releases and long-term support

This page explains how SiEDA is versioned, which releases get fixes, and for how long. It covers the macOS app,
SiEDA Studio (Linux, Windows), `sieda-cli`, `sieda-mcp` and the C API.

## Version numbers

A release is tagged `v<MAJOR>.<MINOR>.<PATCH>`. The tag must match `project(SiEDA VERSION …)` in `CMakeLists.txt`,
and the release workflow (`.github/workflows/release.yml`) stops if it doesn't.

| Part | Changes when | Promise |
|---|---|---|
| MAJOR | A project file, C API or MCP tool changes in a way older users would notice | Announced one minor release ahead in the release notes, with a migration note in [MIGRATING.md](MIGRATING.md) |
| MINOR | New features, new MCP tools, new C API functions (`SIEDA_API_VERSION_MINOR`) | Projects saved by an older release of the same major open and save identically ([FILE_FORMAT.md](FILE_FORMAT.md)); JSON results only gain fields |
| PATCH | Fixes only | No new features, no file-format change, no change to the copper of an existing route |

A file from a **newer** release still opens. Fields this version doesn't know are listed when the file opens
(`Project::loadNotes`), so nothing is lost silently. `sieda-cli --upgrade` rewrites older files in the current format.

## Release lines

| Line | What it is | Supported for |
|---|---|---|
| **Current** | The latest minor release (`main`) | Until the next minor release |
| **LTS** | One minor release a year, chosen when it ships, kept on branch `release/<MAJOR>.<MINOR>` | **24 months** from its first release |

**v1.0 is the first LTS line** (branch `release/1.0`). Two LTS lines overlap for 12 months, so a team can move from
one to the next on its own schedule.

## What an LTS line gets

Fixes are made on `main` first, then cherry-picked to every supported `release/*` branch and released as a patch
(`v1.0.1`, `v1.0.2`, …). An LTS line gets:

1. **Security fixes**: everything in scope in [SECURITY.md](../SECURITY.md).
2. **Data-safety fixes**: anything that could lose, corrupt or silently change a design.
3. **Manufacturing-output fixes**: wrong Gerber, drill, IPC-2581, ODB++, pick-and-place or BOM output, or a DRC /
   DFM check that passes a board it should fail.
4. **Crash fixes**, and fixes needed for a new macOS, Windows or Linux release to run it.
5. **Catalog pinout corrections**: a pin found wrong against its datasheet.

An LTS line gets **no new features**, no new parts, no UI changes and no changes to routing or placement results.
A board routed on an LTS line routes the same on every patch of that line.

## How a patch release is made

An LTS branch starts at its first release: `git branch release/1.0 v1.0.0 && git push origin release/1.0`.

1. Fix on `main` with a regression test; CI must be green.
2. `git cherry-pick -x <commit>` onto each supported `release/*` branch. CI runs on pushes to `main` and `release/**`.
3. Raise the PATCH number in `CMakeLists.txt` on that branch, commit, and tag `v<MAJOR>.<MINOR>.<PATCH>`.
4. The release workflow builds the signed DMG, Studio for Linux and Windows, the CLI / MCP server, the SBOM and
   `SHA256SUMS`, and checks that the Linux build is byte-identical.

## Supported lines

| Line | Branch | First release | Supported until |
|---|---|---|---|
| 1.0 (LTS) | `release/1.0` (made from the v1.0.0 tag) | v1.0.0 | 24 months after v1.0.0 |
| Current | `main` | — | the next minor release |

When a line reaches its end date, its row moves to the table below and it gets no more fixes.

| Ended line | Last release |
|---|---|
| — | — |
