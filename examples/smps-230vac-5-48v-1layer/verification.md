# Design Verification Report

- **Project:** SMPS 230 VAC to 5-48 V 5 A (CC/CV)
- **Industry profile:** Power Electronics (IEC 62368-1, IEC 61204, IPC-2221 B2 spacing, IPC-2152)
- **Design rules:** IPC-2221 Class 3, 1 copper layer
- **Verdict:** PASS (0 errors, 0 warnings, 134 notes)

| Stage | Result | Summary |
|---|---|---|
| Electrical Rule Check | PASS | 0 errors, 0 warnings, 132 notes |
| DC Simulation | SKIPPED | No source or ground symbol to simulate |
| Circuit Validation | PASS | Values standard, parts within derated ratings |
| Footprint Placement | PASS | 200 of 200 footprints placed |
| Routing Completion | PASS | All connections routed |
| Design Rule Check | PASS | Clean against IPC-2221 Class 3 |
| Design for Reliability | PASS | 0 errors, 0 warnings, 1 note |
| Manufacturing Outputs | PASS | 15 files generated and checked |

## Electrical Rule Check — PASS

0 errors, 0 warnings, 132 notes

- 200 components, 73 nets

| Severity | Code | Message |
|---|---|---|
| info | `ERC_TOUCHING_UNCONNECTED` | BR1.+ touches a wire but is not connected to it: wire it, or move it clear. |
| info | `ERC_TOUCHING_UNCONNECTED` | BR1.- touches a wire but is not connected to it: wire it, or move it clear. |
| info | `ERC_TOUCHING_UNCONNECTED` | #NL82.N touches a wire but is not connected to it: wire it, or move it clear. |
| info | `ERC_NOT_SIMULATED` | J1 (TERMINAL-3P-5.08) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | F1 (FUSE-5X20-HOLDER) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | RV1 (S10K275) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | CX1 (X2-100N-275VAC) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | LF1 (CMC-2X10MH-6A) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | CX2 (X2-100N-275VAC) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | BR1 (GBU4M) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | U1 (L6563) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | LPFC (L-PFC-400U-ZCD) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | Q1 (C3M0065100K) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | DPFC (DIODE-TO220-2) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | CB1 (CAP-SNAPIN-30MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | Q2 (C3M0065100K) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | T1 (T-FLYBACK-ETD44) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | DOUT (DIODE-TO220-2) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | C11 (CAP-RADIAL-12.5MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | C12 (CAP-RADIAL-12.5MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | U3 (PC817) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | U4 (TL431AIDBZR) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | CY1 (CAP-Y1-DISC-10MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | Q3 (BSC040N10NS5) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | Q4 (BSC040N10NS5) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | LB (L-POWER-33U-10A) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | C18 (CAP-RADIAL-12.5MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | C19 (CAP-RADIAL-12.5MM) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | J2 (TERMINAL-2P-5.08) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | NT1 (NetTie-2) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | NT2 (NetTie-2) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | U10 (INA181A2IDBVR) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | J3 (PinHeader_1x04_P2.54mm) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | MH1 (MH-M3-PE) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW1 (WIRE-LINK-42.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW2 (WIRE-LINK-11.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW3 (WIRE-JUMPER-13.31x9.98) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW4 (WIRE-LINK-34.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW5 (WIRE-LINK-8.50x6.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW6 (WIRE-JUMPER-16.26x55.45) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW7 (WIRE-LINK-9.75x9.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW8 (WIRE-LINK-26.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW9 (WIRE-LINK-34.75x1.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW10 (WIRE-LINK-1.50x3.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW11 (WIRE-LINK-2.50x2.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW12 (WIRE-LINK-3.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW13 (WIRE-LINK-8.07x16.97) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW14 (WIRE-LINK-4.00x4.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW15 (WIRE-LINK-25.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW16 (WIRE-LINK-3.75x5.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW17 (WIRE-LINK-14.75x21.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW18 (WIRE-JUMPER-20.77x18.82) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW19 (WIRE-LINK-7.00x4.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW20 (WIRE-LINK-0.75x18.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW21 (WIRE-JUMPER-16.25x40.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW22 (WIRE-LINK-6.00x6.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW23 (WIRE-LINK-16.25x2.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW24 (WIRE-LINK-1.75x2.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW25 (WIRE-LINK-20.75x15.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW26 (WIRE-LINK-9.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW27 (WIRE-LINK-5.75x5.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW28 (WIRE-LINK-1.62x10.03) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW29 (WIRE-LINK-6.50x16.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW30 (WIRE-LINK-1.75x18.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW31 (WIRE-LINK-25.75x25.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW32 (WIRE-LINK-11.54x2.35) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW33 (WIRE-LINK-8.00x13.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW34 (WIRE-LINK-11.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW35 (WIRE-LINK-1.00x4.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW36 (WIRE-LINK-11.25x11.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW37 (WIRE-LINK-6.75x2.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW38 (WIRE-LINK-5.00x5.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW39 (WIRE-LINK-5.25x5.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW40 (WIRE-LINK-4.75x6.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW41 (WIRE-LINK-5.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW42 (WIRE-LINK-13.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW43 (WIRE-LINK-27.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW44 (WIRE-LINK-22.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW45 (WIRE-LINK-2.25x13.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW46 (WIRE-LINK-4.00x8.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW47 (WIRE-LINK-3.00x0.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW48 (WIRE-LINK-4.25x0.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW49 (WIRE-LINK-2.75x8.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW50 (WIRE-LINK-11.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW51 (WIRE-LINK-8.25x16.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW52 (WIRE-LINK-1.00x16.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW53 (WIRE-JUMPER-10.74x18.28) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW54 (WIRE-LINK-3.75x3.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW55 (WIRE-LINK-1.25x36.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW56 (WIRE-LINK-11.84x3.66) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW57 (WIRE-LINK-4.75x34.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW58 (WIRE-LINK-2.25x5.05) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW59 (WIRE-LINK-3.25x3.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW60 (WIRE-LINK-12.25x12.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW61 (WIRE-LINK-8.50x8.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW62 (WIRE-LINK-21.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW63 (WIRE-LINK-4.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW64 (WIRE-LINK-4.75x4.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW65 (WIRE-LINK-2.25x2.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW66 (WIRE-LINK-7.00x2.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW67 (WIRE-LINK-7.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW68 (WIRE-LINK-9.25x9.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW69 (WIRE-LINK-8.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW70 (WIRE-LINK-0.75x25.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW71 (WIRE-LINK-8.25x20.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW72 (WIRE-LINK-12.75x0.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW73 (WIRE-LINK-7.56x12.17) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW74 (WIRE-LINK-1.75x8.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW75 (WIRE-LINK-21.66x5.03) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW76 (WIRE-LINK-4.25x6.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW77 (WIRE-LINK-0.76x8.02) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW78 (WIRE-LINK-16.75x31.50) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW79 (WIRE-LINK-1.00x9.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW80 (WIRE-LINK-31.00) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW81 (WIRE-LINK-8.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW82 (WIRE-LINK-2.09x13.77) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW83 (WIRE-LINK-19.02x35.55) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW84 (WIRE-LINK-20.75x1.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW85 (WIRE-LINK-10.73x6.49) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW86 (WIRE-LINK-19.25x0.25) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW87 (WIRE-LINK-18.75x11.75) has no simulation model. |
| info | `ERC_NOT_SIMULATED` | JW88 (WIRE-LINK-20.00x8.50) has no simulation model. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label AC_L is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label CC_OUT is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label CV_OUT is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FLY_ISNS is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label I_RTN is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label L_F is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PE is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_GATE is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label RECT_RAW is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label SNUB is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |

## DC Simulation — SKIPPED

No source or ground symbol to simulate

| Severity | Code | Message |
|---|---|---|
| info | `VER_NO_SOURCE` | No voltage source and no ground symbol: nothing to simulate (add a source and a ground to simulate the circuit). |

## Circuit Validation — PASS

Values standard, parts within derated ratings

- Power Electronics derating: power 70 %, current 80 % of rating

## Footprint Placement — PASS

200 of 200 footprints placed

- Board 270.0 × 176.0 mm, 1 copper layer

## Routing Completion — PASS

All connections routed

- 780 track segments, 0 vias, 4067.7 mm of track

## Design Rule Check — PASS

Clean against IPC-2221 Class 3

- Rules: track 0.40 mm, clearance 0.30 mm, via 0.80/1.60 mm (fab min 0.20/0.20 mm)

## Design for Reliability — PASS

0 errors, 0 warnings, 1 note

- Material: High-Tg FR-4, Tg ≥ 170 °C, low Z-axis CTE (IPC-4101/126)
- Solder: SAC305 lead-free, IPC Class 3
- Conformal coating: none

| Severity | Code | Message |
|---|---|---|
| info | `REL_LENGTH_MISMATCH` | Differential pair ISNS: intra-pair skew 37.10 mm (tolerance 0.13 mm); ISNS_P is shortest. Mismatched lengths arrive out of phase (skew), closing the timing eye and turning a differential signal into common-mode noise. Auto Route adds serpentines when there is room; leave space beside the short member or tune it by hand. |

## Manufacturing Outputs — PASS

15 files generated and checked

- Copper Top (Gerber) — 1271.6 KB
- Solder mask top (Gerber) — 12.5 KB
- Solder mask bottom (Gerber) — 12.5 KB
- Silkscreen top (Gerber) — 48.0 KB
- Board outline (Gerber) — 0.3 KB
- Drill (Excellon) — 5.5 KB, 298 holes
- Mounting holes (Excellon NPTH) — 0.2 KB, 5 mounting holes
- Bill of materials (CSV) — 13.6 KB
- Pick and place (CSV) — 9.4 KB
- SPICE netlist — 8.5 KB
- Solder paste top (Gerber) — 4.7 KB
- Silkscreen designators — 48.0 KB
- IPC-D-356A test netlist — 34.5 KB
- Gerber job file (X2) — 2.0 KB
- Gerber zip for upload — 1475.2 KB

---
Generated by SiEDA design verification.
