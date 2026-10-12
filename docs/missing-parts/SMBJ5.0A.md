# SMBJ5.0A — Littelfuse

**Status: TODO — pinout not yet taken from the datasheet.**

| Field | Value |
|---|---|
| SiEDA part name | `SMBJ5.0A` |
| Manufacturer | Littelfuse |
| Function | 600 W unidirectional TVS diode, 5 V standoff (SMBJ series) |
| Library category | Protection |
| Reference prefix | D |
| Catalog package | SMB (DO-214AA) |
| SiEDA package type | `CUSTOM` |
| Pin count | 2 |
| Pitch | _confirm_ |
| Body size | 3.6 mm |
| Exposed pad | No |
| Datasheet | https://www.littelfuse.com/products/tvs-diodes/ |
| Datasheet revision used | _fill in (e.g. SLVSxxx rev C, 2023-05)_ |
| Pin table page | _fill in_ |

## Notes

Requested in the semiconductor IC reference (October 2026). Not in KiCad's symbol library, Espressif's, Digi-Key's or
SparkFun's KiCad libraries, and the vendor's site could not be reached from the cloud build environment.

- Irregular pads: give the land pattern from the datasheet's recommended footprint (`package.lands`).
- Package values come from the vendor's ordering information and still need checking against the package drawing (step 2 of the README).

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| 1 |  |  |
| 2 |  |  |
