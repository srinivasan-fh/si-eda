# ATSAMD51P20A-AUT — Microchip

**Status: verified (datasheet DS60001507N, page 26).**

| Field | Value |
|---|---|
| SiEDA part name | `ATSAMD51P20A-AUT` |
| Manufacturer | Microchip |
| Function | Arm Cortex-M4F MCU, 120 MHz, 1 MB flash, 256 KB SRAM |
| Library category | Microcontrollers · Microchip |
| Reference prefix | U |
| Catalog package | TQFP-128 14×14 |
| SiEDA package type | `LQFP` |
| Pin count | 128 |
| Pitch | 0.4 mm |
| Body size | 14 mm |
| Exposed pad | No |
| Datasheet | https://ww1.microchip.com/downloads/en/DeviceDoc/SAM-D5x-E5x-Family-Data-Sheet-DS60001507.pdf |
| Datasheet revision used | DS60001507N (SAM D5x/E5x Family Data Sheet, 2026) |
| Pin table page | 26 |

## Notes

Pin names from Figure 4-5 (128-pin TQFP, page 26); every GPIO checked against the TQFP 128 column of Table 6-1 (I/O Multiplexing). Package from section 59.3.10 (page 1964): 0.40 mm pitch, 14 × 14 mm body, no exposed pad. VDDCORE is the core regulator output (decoupling only) and VSW the switching regulator's inductor node; GPIO pins carry their port name (the peripheral functions are in Table 6-1). The part number follows the ordering code of Figure 2-1 (A = TQFP, U = −40 to 85 °C, T = tape and reel).

## Pin table

Take every row from the datasheet's pin-configuration table, in pin-number order. Types: `Input`, `Output`, `Bidirectional`, `PowerIn`, `PowerOut`, `OpenCollector`, `Passive`, `NoConnect`.
Use the datasheet's pin name exactly (no spaces; `/` is fine). Ground and supply pins are `PowerIn`; regulator outputs are `PowerOut`; open-drain outputs (nFAULT, nINT …) are `OpenCollector`; NC pins are `NoConnect`.

| Number | Name | Type |
|---|---|---|
| 1 | PA00 | Bidirectional |
| 2 | PA01 | Bidirectional |
| 3 | PC00 | Bidirectional |
| 4 | PC01 | Bidirectional |
| 5 | GND | PowerIn |
| 6 | VDDANA | PowerIn |
| 7 | PC02 | Bidirectional |
| 8 | PC03 | Bidirectional |
| 9 | PA02 | Bidirectional |
| 10 | PA03 | Bidirectional |
| 11 | PB04 | Bidirectional |
| 12 | PB05 | Bidirectional |
| 13 | PD00 | Bidirectional |
| 14 | GNDANA | PowerIn |
| 15 | VDDANA | PowerIn |
| 16 | PD01 | Bidirectional |
| 17 | PB06 | Bidirectional |
| 18 | PB07 | Bidirectional |
| 19 | PB08 | Bidirectional |
| 20 | PB09 | Bidirectional |
| 21 | PA04 | Bidirectional |
| 22 | PA05 | Bidirectional |
| 23 | PA06 | Bidirectional |
| 24 | PA07 | Bidirectional |
| 25 | GNDANA | PowerIn |
| 26 | VDDANA | PowerIn |
| 27 | PC04 | Bidirectional |
| 28 | PC05 | Bidirectional |
| 29 | PC06 | Bidirectional |
| 30 | PC07 | Bidirectional |
| 31 | GND | PowerIn |
| 32 | VDDIOB | PowerIn |
| 33 | PA08 | Bidirectional |
| 34 | PA09 | Bidirectional |
| 35 | PA10 | Bidirectional |
| 36 | PA11 | Bidirectional |
| 37 | VDDIOB | PowerIn |
| 38 | GND | PowerIn |
| 39 | PB10 | Bidirectional |
| 40 | PB11 | Bidirectional |
| 41 | PB12 | Bidirectional |
| 42 | PB13 | Bidirectional |
| 43 | PB14 | Bidirectional |
| 44 | PB15 | Bidirectional |
| 45 | GND | PowerIn |
| 46 | VDDIO | PowerIn |
| 47 | PD08 | Bidirectional |
| 48 | PD09 | Bidirectional |
| 49 | PD10 | Bidirectional |
| 50 | PD11 | Bidirectional |
| 51 | PD12 | Bidirectional |
| 52 | PC10 | Bidirectional |
| 53 | GND | PowerIn |
| 54 | VDDIO | PowerIn |
| 55 | PC11 | Bidirectional |
| 56 | PC12 | Bidirectional |
| 57 | PC13 | Bidirectional |
| 58 | PC14 | Bidirectional |
| 59 | PC15 | Bidirectional |
| 60 | PA12 | Bidirectional |
| 61 | PA13 | Bidirectional |
| 62 | PA14 | Bidirectional |
| 63 | PA15 | Bidirectional |
| 64 | GND | PowerIn |
| 65 | VDDIO | PowerIn |
| 66 | PA16 | Bidirectional |
| 67 | PA17 | Bidirectional |
| 68 | PA18 | Bidirectional |
| 69 | PA19 | Bidirectional |
| 70 | PC16 | Bidirectional |
| 71 | PC17 | Bidirectional |
| 72 | PC18 | Bidirectional |
| 73 | PC19 | Bidirectional |
| 74 | PC20 | Bidirectional |
| 75 | PC21 | Bidirectional |
| 76 | PC22 | Bidirectional |
| 77 | PC23 | Bidirectional |
| 78 | GND | PowerIn |
| 79 | VDDIO | PowerIn |
| 80 | PD20 | Bidirectional |
| 81 | PD21 | Bidirectional |
| 82 | PB16 | Bidirectional |
| 83 | PB17 | Bidirectional |
| 84 | PB18 | Bidirectional |
| 85 | PB19 | Bidirectional |
| 86 | PB20 | Bidirectional |
| 87 | PB21 | Bidirectional |
| 88 | PA20 | Bidirectional |
| 89 | PA21 | Bidirectional |
| 90 | GND | PowerIn |
| 91 | VDDIO | PowerIn |
| 92 | PA22 | Bidirectional |
| 93 | PA23 | Bidirectional |
| 94 | PA24 | Bidirectional |
| 95 | PA25 | Bidirectional |
| 96 | GND | PowerIn |
| 97 | VDDIO | PowerIn |
| 98 | PB22 | Bidirectional |
| 99 | PB23 | Bidirectional |
| 100 | PB24 | Bidirectional |
| 101 | PB25 | Bidirectional |
| 102 | PB26 | Bidirectional |
| 103 | PB27 | Bidirectional |
| 104 | PB28 | Bidirectional |
| 105 | PB29 | Bidirectional |
| 106 | GND | PowerIn |
| 107 | VDDIO | PowerIn |
| 108 | PC24 | Bidirectional |
| 109 | PC25 | Bidirectional |
| 110 | PC26 | Bidirectional |
| 111 | PC27 | Bidirectional |
| 112 | PC28 | Bidirectional |
| 113 | PA27 | Bidirectional |
| 114 | RESETN | Input |
| 115 | VDDCORE | PowerOut |
| 116 | GNDIO | PowerIn |
| 117 | VSW | Passive |
| 118 | VDDIO | PowerIn |
| 119 | PA30 | Bidirectional |
| 120 | PA31 | Bidirectional |
| 121 | PB30 | Bidirectional |
| 122 | PB31 | Bidirectional |
| 123 | PC30 | Bidirectional |
| 124 | PC31 | Bidirectional |
| 125 | PB00 | Bidirectional |
| 126 | PB01 | Bidirectional |
| 127 | PB02 | Bidirectional |
| 128 | PB03 | Bidirectional |
