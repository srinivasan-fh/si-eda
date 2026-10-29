# SN65DSI86ZXHR — Texas Instruments

**Status: verified (datasheet SLLSEH2C (revised October 2020), page 5).**

| Field | Value |
|---|---|
| SiEDA part name | `SN65DSI86ZXHR` |
| Manufacturer | Texas Instruments |
| Function | MIPI DSI to embedded DisplayPort (eDP) bridge |
| Library category | Interface |
| Reference prefix | U |
| Catalog package | nFBGA-64 5×5 0.5 mm (ZXH) |
| SiEDA package type | `BGA` |
| Pin count | 64 |
| Pitch | 0.5 mm |
| Body size | 5 mm |
| Exposed pad | No |
| Datasheet | https://www.ti.com/product/SN65DSI86 |
| Datasheet revision used | SLLSEH2C (revised October 2020) |
| Pin table page | 5 |

## Notes

Table 6-1 and Figure 6-1 (ZXH, 9 × 9 grid with 17 balls unpopulated). This datasheet covers only the ZXH ball-grid package; the HTQFP-64 SN65DSI86IPAPRQ1 is the automotive -Q1 device with its own datasheet. SCL / SDA are open drain; TEST1 to ground, TEST3 to ground through 0.1 µF.
SparkFun's KiCad libraries, and the vendor's site could not be reached from the cloud build environment.

- Package values come from the vendor's ordering information and still need checking against the package drawing (step 2 of the README).

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| A1 | ADDR | Bidirectional |
| H8 | AUXP | Bidirectional |
| H9 | AUXN | Bidirectional |
| H3 | DA0P | Bidirectional |
| J3 | DA0N | Bidirectional |
| H4 | DA1P | Input |
| J4 | DA1N | Input |
| H6 | DA2P | Input |
| J6 | DA2N | Input |
| H7 | DA3P | Input |
| J7 | DA3N | Input |
| H5 | DACP | Input |
| J5 | DACN | Input |
| C2 | DB0P | Input |
| C1 | DB0N | Input |
| D2 | DB1P | Input |
| D1 | DB1N | Input |
| F2 | DB2P | Input |
| F1 | DB2N | Input |
| G2 | DB3P | Input |
| G1 | DB3N | Input |
| E2 | DBCP | Input |
| E1 | DBCN | Input |
| F8 | ML0P | Output |
| F9 | ML0N | Output |
| E8 | ML1P | Output |
| E9 | ML1N | Output |
| C8 | ML2P | Output |
| C9 | ML2N | Output |
| B8 | ML3P | Output |
| B9 | ML3N | Output |
| B1 | EN | Input |
| J8 | HPD | Input |
| A3 | IRQ | Output |
| A7 | REFCLK | Input |
| H1 | SCL | Bidirectional |
| J1 | SDA | Bidirectional |
| B3 | TEST1 | Input |
| B5 | TEST2 | Bidirectional |
| B7 | TEST3 | Passive |
| A4 | GPIO1 | Bidirectional |
| A5 | GPIO2 | Bidirectional |
| A6 | GPIO3 | Bidirectional |
| B4 | GPIO4 | Bidirectional |
| D9 | VPLL | PowerIn |
| B6 | VCCIO | PowerIn |
| A2 | VCCIO | PowerIn |
| A8 | GND | PowerIn |
| D8 | GND | PowerIn |
| E4 | GND | PowerIn |
| E5 | GND | PowerIn |
| F4 | GND | PowerIn |
| F5 | GND | PowerIn |
| F6 | GND | PowerIn |
| G8 | GND | PowerIn |
| D6 | VCC | PowerIn |
| D5 | VCC | PowerIn |
| J2 | VCC | PowerIn |
| J9 | VCC | PowerIn |
| A9 | VCCA | PowerIn |
| G9 | VCCA | PowerIn |
| E6 | VCCA | PowerIn |
| B2 | VCCA | PowerIn |
| H2 | VCCA | PowerIn |
