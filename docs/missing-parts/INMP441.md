# INMP441ACEZ-R0 — TDK InvenSense

**Status: verified (datasheet DS-INMP441-00 revision 1.1, page 8).**

| Field | Value |
|---|---|
| SiEDA part name | `INMP441ACEZ-R0` |
| Manufacturer | TDK InvenSense |
| Function | Omnidirectional MEMS microphone, I²S digital output |
| Library category | Audio |
| Reference prefix | MK |
| Catalog package | LGA_CAV-9 4.72×3.76 |
| SiEDA package type | `LGA` |
| Pin count | 9 (+ the sound-port hole) |
| Pitch | 1.05 mm (lands listed) |
| Body size | 3.76 × 4.72 mm |
| Exposed pad | No — pin 5 is the ground ring round the sound port |
| Datasheet | https://invensense.tdk.com/wp-content/uploads/2015/02/INMP441.pdf |
| Datasheet revision used | DS-INMP441-00 revision 1.1 |
| Pin table page | 8 |

## Notes

INMP441ACEZ-R7 (7" reel) is marked "to be discontinued" in the ordering guide (page 20), so the 13" reel part INMP441ACEZ-R0 is added (same device). Lands from Figure 14 (page 17) and Figure 16 (page 19), turned a quarter so pin 1 is top-left (top view): pins 1–4 and 6–9 are 0.60 × 0.40 mm at 1.05 mm pitch, rows 2.66 mm apart; pin 5 is a ground ring Ø1.56 / Ø0.96 mm (a keyhole outline, so the centre stays free of copper and paste) round the 0.25 mm sound-port hole, a non-plated land "-" (the datasheet recommends 0.5–1 mm for the board hole; any size ≥ 0.25 mm works). Body 4.72 × 3.76 mm.

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| 1 | SCK | Input |
| 2 | SD | Output |
| 3 | WS | Input |
| 4 | L/R | Input |
| 5 | GND | PowerIn |
| 6 | GND | PowerIn |
| 7 | VDD | PowerIn |
| 8 | CHIPEN | Input |
| 9 | GND | PowerIn |
| - | (sound port, NPTH 0.25 mm) | — |
