# VL53L5CXV0GC/1 — STMicroelectronics

**Status: TODO — pinout not yet taken from the datasheet.**

| Field | Value |
|---|---|
| SiEDA part name | `VL53L5CXV0GC/1` |
| Manufacturer | STMicroelectronics |
| Function | 8×8 multizone 4 m time-of-flight sensor, I2C |
| Library category | Sensors |
| Reference prefix | U |
| Catalog package | Optical LGA-16 6.4×3 (land pattern needed) |
| SiEDA package type | `LGA` |
| Pin count | 16 |
| Pitch | 0.75 mm |
| Body size | 6.4 mm |
| Exposed pad | No |
| Datasheet | https://www.st.com/resource/en/datasheet/vl53l5cx.pdf |
| Datasheet revision used | _fill in (e.g. SLVSxxx rev C, 2023-05)_ |
| Pin table page | _fill in_ |

## Notes

Requested in the semiconductor IC reference (October 2026). Not in KiCad's symbol library, Espressif's, Digi-Key's or
SparkFun's KiCad libraries, and the vendor's site could not be reached from the cloud build environment.

- LGA package: SiEDA needs its land pattern too (`package.lands`, one pad per pin, from the datasheet's recommended footprint).
- Package values come from the vendor's ordering information and still need checking against the package drawing (step 2 of the README).

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| 1 |  |  |
| 2 |  |  |
| 3 |  |  |
| 4 |  |  |
| 5 |  |  |
| 6 |  |  |
| 7 |  |  |
| 8 |  |  |
| 9 |  |  |
| 10 |  |  |
| 11 |  |  |
| 12 |  |  |
| 13 |  |  |
| 14 |  |  |
| 15 |  |  |
| 16 |  |  |
