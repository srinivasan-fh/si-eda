# Design Verification Report

- **Project:** SMPS 230 VAC to 5-48 V 5 A (CC/CV)
- **Industry profile:** Power Electronics (IEC 62368-1, IEC 61204, IPC-2221 B2 spacing, IPC-2152)
- **Design rules:** IPC-2221 Class 3, 2 copper layers
- **Verdict:** PASS (0 errors, 0 warnings, 67 notes)

| Stage | Result | Summary |
|---|---|---|
| Electrical Rule Check | PASS | 0 errors, 0 warnings, 66 notes |
| DC Simulation | SKIPPED | No source or ground symbol to simulate |
| Circuit Validation | PASS | Values standard, parts within derated ratings |
| Footprint Placement | PASS | 112 of 112 footprints placed |
| Routing Completion | PASS | All connections routed |
| Design Rule Check | PASS | Clean against IPC-2221 Class 3 |
| Design for Reliability | PASS | No reliability risks found |
| Manufacturing Outputs | PASS | 16 files generated and checked |

## Electrical Rule Check — PASS

0 errors, 0 warnings, 66 notes

- 112 components, 73 nets

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
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label AC_L is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label AC_N is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label AUX_W is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label BOOT is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label BUCK_EN is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label BUCK_PWM is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label CC_OUT is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label CV_OUT is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FB_K is used only on Isolated 52 V bus & feedback; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FB_REF is used only on Isolated 52 V bus & feedback; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FLY_ISNS is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FLY_OUT is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label FLY_SW is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label HS_G is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label INA_OUT is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label ISET is used only on CC / CV control; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label I_RTN is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label LS_G is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label L_F is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label L_FUSED is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label N_F is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label OPTO_A is used only on Isolated 52 V bus & feedback; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PE is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_GATE is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_INV is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_ISNS is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_SW is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label PFC_ZCD is used only on PFC boost (L6563); a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label RECT_RAW is used only on AC input & EMI filter; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label SNUB is used only on Flyback primary & bias; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label SW is used only on Synchronous buck; a local label keeps the net private to that sheet. |
| info | `ERC_GLOBAL_LABEL_ONE_SHEET` | Global label VSET is used only on CC / CV control; a local label keeps the net private to that sheet. |

## DC Simulation — SKIPPED

No source or ground symbol to simulate

| Severity | Code | Message |
|---|---|---|
| info | `VER_NO_SOURCE` | No voltage source and no ground symbol: nothing to simulate (add a source and a ground to simulate the circuit). |

## Circuit Validation — PASS

Values standard, parts within derated ratings

- Power Electronics derating: power 70 %, current 80 % of rating

## Footprint Placement — PASS

112 of 112 footprints placed

- Board 200.0 × 130.0 mm, 2 copper layers

## Routing Completion — PASS

All connections routed

- 699 track segments, 140 vias, 2758.5 mm of track

## Design Rule Check — PASS

Clean against IPC-2221 Class 3

- Rules: track 0.40 mm, clearance 0.30 mm, via 0.35/0.75 mm (fab min 0.20/0.20 mm)

## Design for Reliability — PASS

No reliability risks found

- Material: High-Tg FR-4, Tg ≥ 170 °C, low Z-axis CTE (IPC-4101/126)
- Solder: SAC305 lead-free, IPC Class 3
- Conformal coating: none

## Manufacturing Outputs — PASS

16 files generated and checked

- Copper Top (Gerber) — 38.0 KB
- Copper Bottom (Gerber) — 668.5 KB
- Solder mask top (Gerber) — 8.2 KB
- Solder mask bottom (Gerber) — 3.8 KB
- Silkscreen top (Gerber) — 73.6 KB
- Board outline (Gerber) — 0.3 KB
- Drill (Excellon) — 4.7 KB, 262 holes
- Mounting holes (Excellon NPTH) — 0.2 KB, 5 mounting holes
- Bill of materials (CSV) — 5.1 KB
- Pick and place (CSV) — 4.7 KB
- SPICE netlist — 3.3 KB
- Solder paste top (Gerber) — 4.6 KB
- Silkscreen designators — 73.6 KB
- IPC-D-356A test netlist — 31.8 KB
- Gerber job file (X2) — 2.3 KB
- Gerber zip for upload — 837.8 KB

---
Generated by SiEDA design verification.
