# FM24CL64B-GATR — Infineon (Cypress)

**Status: verified (datasheet 001-84457 Rev. *F, page 3).**

| Field | Value |
|---|---|
| SiEDA part name | `FM24CL64B-GATR` |
| Manufacturer | Infineon (Cypress) |
| Function | 64 Kbit 3 V I2C FRAM, 10^14 write cycles |
| Library category | Memory |
| Reference prefix | U |
| Catalog package | SOIC-8 150 mil |
| SiEDA package type | `SOIC` |
| Pin count | 8 |
| Pitch | 1.27 mm |
| Body size | 3.9 mm |
| Exposed pad | No |
| Datasheet | https://www.infineon.com/cms/en/product/memories/f-ram-ferroelectric-ram/ |
| Datasheet revision used | 001-84457 Rev. *F |
| Pin table page | 3 |

## Notes

Figure 1 and Pin Definitions (page 3) of the automotive datasheet 001-84457, whose ordering codes are FM24CL64B-GA / -GATR (Automotive-E). The industrial FM24CL64B-G / -GTR is in a separate datasheet (001-84458) that was not available, so it is not listed here. SDA is open drain.
SparkFun's KiCad libraries, and the vendor's site could not be reached from the cloud build environment.

- Package values come from the vendor's ordering information and still need checking against the package drawing (step 2 of the README).

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| 1 | A0 | Input |
| 2 | A1 | Input |
| 3 | A2 | Input |
| 4 | VSS | PowerIn |
| 5 | SDA | Bidirectional |
| 6 | SCL | Input |
| 7 | WP | Input |
| 8 | VDD | PowerIn |
