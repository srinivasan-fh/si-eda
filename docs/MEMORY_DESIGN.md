# Memory (RAM) Design

SiEDA checks a memory subsystem the way it checks a robot, an ECU or an appliance: you pick what you are building,
and the **Memory (RAM) Segments** panel checks the design in five segments. Each segment is marked complete, partial
or missing, and every gap is also a Design Check.

- [Memory design types](#memory-design-types)
- [The five segments](#the-five-segments)
- [Check codes](#check-codes)
- [DRAM parts in the library](#dram-parts-in-the-library)
- [Reference design](#reference-design)
- [Limits](#limits)
- [Code map](#code-map)
- [Tests](#tests)

## Memory design types

Pick the type in **Properties → Memory (RAM) Segments**, or with **Design → Memory Design**. The **Memory & DRAM
Design** industry profile alone reports the checks as advice (Info). It also selects the *HDI / Fine-Pitch BGA
(IPC-2226)* design rules, which suit 0.8 mm BGA DRAM and 0.5 mm LQFP controllers.

| Type | Id | What it is |
|---|---|---|
| SDR SDRAM on an MCU | `sdram` | 16 / 32-bit SDR SDRAM on an MCU memory controller (STM32 FMC, NXP SEMC / EMC): frame buffers, heaps |
| DDR3L / DDR4 memory-down | `ddr` | DDR3L / DDR4 DRAM soldered beside an SoC, FPGA or processor |
| LPDDR4 / LPDDR5 point-to-point | `lpddr` | Mobile DRAM next to an application processor or SoM |
| DDR5 UDIMM / SO-DIMM module | `dimm` | Unbuffered desktop / laptop memory modules |
| DDR5 RDIMM / LRDIMM server module | `rdimm` | Registered server memory modules |

The type is saved with the project (`memoryDesign`). It is undoable, and design plans and AI agents can set it.

## The five segments

| Segment | Items |
|---|---|
| **1 Memory Power & Decoupling** | 100 nF per VDD / VDDQ pin pair; ≥ 4.7 µF bulk at the DRAM; on DDR, a VTT rail and a filtered VREF = VDDQ / 2 |
| **2 Clock, Command & Address** | SDR: a 22–33 Ω series resistor on SDCLK. DDR: CK / CK# with ~100 Ω termination. Every address / bank line connected. DDR: fly-by command / address terminated to VTT |
| **3 Data Integrity** | Data nets named as a bus (`DQ0…`, `SD_DQ0…`), so Auto Route tunes them to the bus tolerance. DDR: DQS strobes named as differential pairs. DDR: 240 Ω 1 % on every ZQ pin |
| **4 Configuration & Management** | The memory controller found on the data lines. DDR: RESET_n pulled low. SDR: CKE driven. Modules: an SPD EEPROM / hub with SDA / SCL pull-ups |
| **5 Layout & Fabrication** | DRAM within 25 mm (SDR) or 15 mm (DDR) of its controller; ≥ 4 layers with a GND pour / plane; DDR impedance target 34–50 Ω; modules on a 1.1–1.4 mm board |

The checks read the DRAM by its pins (DQ0…, BA0…, A0…, VDD / VDDQ, VREFCA / VREFDQ, ZQ, RESET_n, CK / CK#, DQS),
whatever the symbol's markup (`V_{DDQ}` and `VDDQ` match). A part counts as DRAM when its name is a known DRAM family
(MT48LC, W9812, IS42S, MT41K, MT40A, AS4C, K4A / K4B, H5AN, …) or when it has at least four DQ pins and a BA pin.
DDR rules apply to DDR parts (ZQ / VREF / ODT pins, or a DDR family name), and to every DRAM of a `ddr`, `lpddr`,
`dimm` or `rdimm` design.

## Check codes

With a memory design type set, the checks run at full severity. With only the industry set, they are Info.

| Code | Severity | When |
|---|---|---|
| `REL_DRAM_NONE` | info | No DRAM part in the design |
| `REL_DRAM_DECOUPLING` | warning | Fewer than one decoupling capacitor per two VDD / VDDQ pins |
| `REL_DRAM_BULK` | info | No ≥ 4.7 µF capacitor on the DRAM supply |
| `REL_DDR_VREF` | warning | VREFCA / VREFDQ is not a filtered divider or regulator output |
| `REL_DDR_ZQ` | warning | ZQ has no resistor to ground, or it is not 240 Ω |
| `REL_DDR_RESET` | warning | RESET_n has no pull-down |
| `REL_DDR_VTT` | warning | No address / command line terminated to a `VTT` net (not on LPDDR) |
| `REL_DDR_CK_TERM` | warning | CK / CK# not terminated, or only half connected |
| `REL_DQS_PAIR` | info | DQS strobes not named as differential pairs |
| `REL_DRAM_CLOCK_SERIES` | info | SDR clock without a series resistor |
| `REL_DRAM_LENGTH_MATCH` | warning (DDR) / info (SDR) | The wired data lines are not one length-matched bus |
| `REL_DRAM_CONTROLLER` | warning | The data lines reach no memory controller |
| `REL_DRAM_CKE` | warning | SDR CKE not connected |
| `REL_DRAM_DISTANCE` | warning | DRAM too far from its controller on the board |
| `REL_DRAM_LAYERS` | warning | Fewer than 4 copper layers |
| `REL_DRAM_REFERENCE_PLANE` | info | No GND pour or plane |
| `REL_DRAM_IMPEDANCE` | info | DDR single-ended impedance target outside 34–50 Ω |
| `REL_SPD` / `REL_SPD_PULLUPS` | warning | Module without an SPD EEPROM, or its SDA / SCL without pull-ups |
| `REL_DDR5_PMIC` / `REL_RCD` | info | DDR5 modules need a JESD301 PMIC; RDIMMs an RCD |
| `REL_DIMM_THICKNESS` | warning | Module board thickness outside 1.1–1.4 mm |

## DRAM parts in the library

All DRAM pinouts come from the KiCad symbol library (`tools/fetch_catalog_parts.py`). They are listed under
**Memory · DRAM** in the device picker.

| Part | Type | Package |
|---|---|---|
| MT48LC16M16A2TG-6A (Micron) | 256 Mb SDR SDRAM × 16, 167 MHz, 3.3 V | TSOP-II-54 |
| W9812G6KH-6 (Winbond) | 128 Mb SDR SDRAM × 16, 166 MHz | TSOP-II-54 |
| IS42S16400J-7TL (ISSI) | 64 Mb SDR SDRAM × 16, 143 MHz | TSOP-II-54 |
| MT41K256M16HA-125 (Micron) | 4 Gb DDR3L × 16, 1.35 V | FBGA-96 9 × 14 mm |
| AS4C256M16D3-12BCN (Alliance) | 4 Gb DDR3 × 16, 1.5 V | FBGA-96 9 × 13 mm |
| MT40A512M16LY-062E (Micron) | 8 Gb DDR4 × 16, 1.2 V | FBGA-96 7.5 × 13.5 mm |

The AT24CS02 (2 Kbit I²C EEPROM, already in the library) serves as a DDR3-class SPD EEPROM.

**Ball layout.** DDR3 / DDR4 DRAM sit on a 9 × 16 ball grid with columns 4–6 empty. Their footprints place every
ball from its name (row letter, column number) on the 0.8 mm pitch, on the rectangular body. A1 is top-left, and T9
is bottom-right at (+3.2, +6.0) mm. Parts with a full square grid (the Artix-7 FPGA) keep their existing layout.

## Reference design

**File → New from Example → Memory & RAM → STM32H743 + 32 MB SDRAM Frame Buffer** (or ask the offline designer for
"an STM32H743 frame buffer with external SDRAM"). It is an STM32H743IIT6 with an MT48LC16M16A2TG-6A on FMC bank 1:

1. **Power:** 5 V → AP2112K 3.3 V; 100 nF per MCU and SDRAM pin pair; bulk; VCAP and VDDA capacitors.
2. **Clock & address:** SDCLK (PG8) through 22 Ω; A0–A12, BA0–BA1, CKE (PH2), CS (PH3), RAS (PF11), CAS (PG15), WE (PH5).
3. **Data:** SD_DQ0–SD_DQ15, length-matched as one bus by Auto Route; DQML / DQMH on PE0 / PE1.
4. **Configuration:** BOOT0 pulled low, SWD header, 8 MHz HSE crystal; the notes give the FMC timings.
5. **Layout:** 6 layers (signal / GND / signal / signal / 3.3 V / signal); unused MCU pins marked no-connect.

It places, routes 143 / 143 connections with 16 length-tuned nets, and passes verification with no errors or
warnings. All five segments are complete.

## Limits

- **BGA routing.** Auto Route fans out every BGA ball (dogbone: a short stub to a via between four balls, or a
  via in the pad with VIPPO) and routes BGA boards on a finer grid. An Artix-7 FPGA with a DDR3L x16 memory-down
  (50 lines) routes completely on 8 layers with GND and supply planes; on 6 layers a few lines may stay for hand
  routing. Give BGA boards planes for their supplies: dozens of power balls routed as tracks crowd the escape.
- **Not in the library yet:** DDR VTT regulators (TPS51200 class), DDR5 SPD hubs (SPD5118), DDR5 PMICs and RCDs.
  Add them as custom parts from their datasheets.

## Code map

| Layer | File | What |
|---|---|---|
| Core | `Core/include/sieda/Memory.hpp`, `Core/src/Memory.cpp` | types, DRAM analysis, checks, segments, JSON |
| Core | `Core/src/CustomParts.cpp` | name-based ball placement for depopulated BGA grids |
| Core | `Core/src/Industry.cpp` | the Memory & DRAM Design industry profile |
| Core | `Core/src/Reliability.cpp` | memory checks in Design Checks and verification |
| C ABI | `sieda_set_memory_design`, `sieda_memory_segments_json` | |
| App | `EDAEngine.setMemoryDesign` / `memorySegments`, `DesignStore.setMemoryDesign`, `MemorySystemProperties` (Inspector), **Design → Memory Design**, `DesignPlan.memoryDesign` | |
| Parts | `tools/fetch_catalog_parts.py` (TSOP-II mapping, rectangular BGA bodies) → `Core/src/StandardCatalog.inc` | |

## Tests

- **Core:** `memory_design_segments_and_checks`
  - types, the industry profile and the DRAM footprints (ball positions);
  - SDR checks from advice to full severity, and clearing them;
  - DDR3L ZQ / VREF / RESET_n / VTT / CK checks;
  - DIMM SPD and board thickness;
  - JSON persistence and the C API.
- **Core:** the DRAM parts are in `production_parts_catalog`.
- **App:** `MemoryDesignTests`
  - types, parts and template selection;
  - the reference design's segments, undo, save / reopen and plan schema;
  - DDR checks on a fresh project.
- **App:** the reference design is in `verifyReferenceDesigns` (placed, routed, verified).
