# Signal & Power Integrity

SiEDA analyses the routed board for signal integrity (SI) and power integrity (PI) at board level, the job tools such
as HyperLynx LineSim / BoardSim, Sigrity or Altium's SI / PDN Analyzer do. It solves every net as a set of transmission
lines driven by IBIS or logic-family buffer models, estimates crosstalk between neighbouring tracks (same layer and
adjacent layer), and checks return paths. For serial links it builds the **channel** of a net or differential pair
from frequency-dependent lossy lines, gives its S-parameters (Touchstone in and out) and runs a PRBS **eye diagram**
with optional equalisation. For power it compares each rail's power-distribution network (PDN) impedance with its
target, models the plane pair as a resonant cavity, proposes the decoupling that meets the target and maps the DC
voltage drop (IR drop) and current density on the board.

It is a screening tool built on closed-form physics from the standard references. It is **not** a full-wave 3D field
solver. See [Limits](#limits) before you sign off a multi-gigabit or high-current design with it alone.

- [Where to find it](#where-to-find-it)
- [Driver and receiver models](#driver-and-receiver-models)
- [Transmission-line analysis](#transmission-line-analysis)
- [Lossy lines](#lossy-lines)
- [Channel analysis](#channel-analysis)
- [Eye diagram](#eye-diagram)
- [Crosstalk](#crosstalk)
- [Return path](#return-path)
- [Power distribution network](#power-distribution-network)
- [IR drop](#ir-drop)
- [PI planning](#pi-planning)
- [Verification and check codes](#verification-and-check-codes)
- [C API](#c-api)
- [Validation](#validation)
- [Limits](#limits)
- [Code map](#code-map)
- [Tests](#tests)

## Where to find it

Open the **Simulation** workspace and switch the mode picker from **Circuit** to **SI / PI**. There are four tabs:

| Tab | What it shows |
|---|---|
| **Signals** | Every signal net, with critical and fast nets first. Select a net to see its driver and model, its line sections (layer, width, length, Z0, delay), the waveform at the driver pin and at the worst receiver, and the same receiver with the recommended series termination. Overshoot, undershoot, settling and flight time are listed for every receiver. The **Driver model** menu assigns a logic family or an imported IBIS model to the net. |
| **Crosstalk** | Every aggressor → victim pair of parallel tracks with coupled length, spacing, NEXT, FEXT and the noise voltage against its limit. Below it, the return-path problems. |
| **Power** | Every supply rail with its \|Z(f)\| curve on log–log axes, the target impedance (dashed line) and the 100 MHz band edge. It also lists decoupling capacitors (ESR, ESL with mounting, self-resonance), recommendations and the worst IR drop. **Ripple** (%) and **Load step** (A) set the target; 0 derives them from the design. Below: the **Regulator** model (output resistance, loop bandwidth), the **Plane cavity** curve against the lumped one, the **Decoupling plan** and the **IR-drop map** of the board (drop or current density). |
| **Channel** | Pick a net (a differential pair is found by name and analysed as a pair; **Single-ended** analyses one leg). **Analyse** shows the insertion and return loss (S21 / S11, or SDD21 / SDD11 / SCD21 / SCC21), the Nyquist loss, skew and coupled sections, the step response lossy against lossless and the eye (bit rate, PRBS, **Ideal 50 Ω** or the net's driver model, **CTLE**, **FFE**, mask, random jitter). **Export Touchstone…** writes .s2p / .s4p; **Import Touchstone…** shows a vendor file's loss and eye and can **Cascade at receiver**. **Check in sign-off** adds the eye-mask check. **Roughness** and **Copper foil** set the conductor model; **Line loss per layer** plots dB / inch of every layer. |

**Import IBIS…** reads a vendor `.ibs` file and adds every buffer model to the project. **Sign-off in verification**
adds a **Signal & Power Integrity** stage to Design Checks (see [Verification](#verification-and-check-codes)).
Every setting is undoable and saved with the project (`signalIntegrity` in the project file).

## Driver and receiver models

### IBIS import

`parseIbis` reads IBIS 4.x and 5.x files (ANSI/EIA-656-B). Keywords are case-insensitive, `_` and space are
equivalent, `|` starts a comment (or the character set by `[Comment Char]`), and numbers take the IBIS scale suffixes
(T G M k m u n p f; trailing units are ignored, `NA` marks a missing column). The parser reads:

| Keyword | Used for |
|---|---|
| `[IBIS Ver]`, `[File Name]`, `[Component]`, `[Manufacturer]` | Identification |
| `[Package]` (`R_pkg`, `L_pkg`, `C_pkg`), `[Pin]` (with `R_pin L_pin C_pin`) | Package parasitics per pin |
| `[Model Selector]` | A selector name resolves to its first model |
| `[Model]`: `Model_type`, `C_comp`, `Vinl`, `Vinh` | Buffer type, die capacitance, receiver thresholds |
| `[Voltage Range]`, `[Pullup Reference]` | Supply level |
| `[Pulldown]`, `[Pullup]`, `[GND Clamp]`, `[POWER Clamp]` | I-V tables |
| `[Ramp]` (`dV/dt_r`, `dV/dt_f`, `R_load`) | Edge rate |
| `[Rising Waveform]`, `[Falling Waveform]` (`R_fixture`, `V_fixture`) | Edge rate when `[Ramp]` is absent |
| `[Model Spec]`, `[Receiver Thresholds]` (`Vinh`, `Vinl`, `_dc` forms) | Thresholds when `[Model]` has none |

Other keywords, such as `[Composite Current]`, `[ISSO PU]` or `[Driver Schedule]`, are skipped. A file without a
`[Model]` is rejected.

Each model is reduced at a corner to the linear buffer the line solve uses. The corners are **typ**, **min**
(weak / slow) and **max** (strong / fast):

- **Edge:** `[Ramp]` gives the 20–80 % swing time `dt`. For a linear ramp the 10–90 % rise time is `dt × 0.8/0.6`.
  Without `[Ramp]`, the 20 % and 80 % crossings of the first rising / falling waveform table are used.
- **Output resistance:** the least-squares slope `R = Σv² / Σ(v·i)` of the `[Pulldown]` table between 0 V and 30 %
  of the supply, averaged with the `[Pullup]` table over the same range. The pullup voltage column is `Vcc − Vpin`
  and its current flows out of the pin (IBIS convention).
- **C_comp**, **Vinl / Vinh** (default 30 % / 70 % of the supply) and the **package**: `R_pin / L_pin / C_pin` of the
  pin, else the component's `[Package]` typ values.

The model's id is `ibis:<model name>`. With a reference designator (C API `ref`), the component's pins are mapped by
pin number to the models of the file's `[Pin]` list.

### Logic-family defaults

When nothing is assigned, a net uses a logic family. DDR nets (named `DDR…`, or `DQ`, `DQS`, `DM`, `CK`, `ADDR`, `BA`,
`RAS`, `CAS`, `WE`, `ODT`, `CKE`, `CS` on a `ddr` / `lpddr` / `dimm` / `rdimm` memory design) use SSTL / POD by the
supply. Other nets use LVCMOS / CMOS by the highest supply rail on the driving part's power pins.

| Id | Family | Swing | 10–90 % edge | R_out | C_in | VIH / VIL | Termination |
|---|---|---|---|---|---|---|---|
| `cmos5` | 5 V CMOS (74HC / 74AC) | 5 V | 2.5 ns | 40 Ω | 5 pF | 3.5 / 1.5 V | — |
| `lvcmos33` | LVCMOS 3.3 V (JESD8C) | 3.3 V | 1.0 ns | 30 Ω | 4 pF | 2.0 / 0.8 V | — |
| `lvcmos25` | LVCMOS 2.5 V (JESD8-5) | 2.5 V | 0.8 ns | 35 Ω | 4 pF | 1.7 / 0.7 V | — |
| `lvcmos18` | LVCMOS 1.8 V (JESD8-7) | 1.8 V | 0.7 ns | 40 Ω | 3 pF | 1.17 / 0.63 V | — |
| `lvcmos12` | LVCMOS 1.2 V (JESD8-12) | 1.2 V | 0.6 ns | 45 Ω | 3 pF | 0.78 / 0.42 V | — |
| `sstl15` | SSTL-15 (DDR3, JESD79-3) | 1.5 V | 0.3 ns | 34 Ω | 2 pF | 0.85 / 0.65 V | 60 Ω ODT to 0.75 V |
| `sstl135` | SSTL-135 (DDR3L) | 1.35 V | 0.3 ns | 34 Ω | 2 pF | 0.765 / 0.585 V | 60 Ω ODT to 0.675 V |
| `pod12` | POD-12 (DDR4, JESD79-4) | 1.2 V | 0.25 ns | 34 Ω | 1.1 pF | 0.915 / 0.765 V | 48 Ω ODT to VDDQ |
| `usb2hs` | USB 2.0 high-speed, per leg | 0.8 V open circuit | 0.5 ns | 45 Ω | 1 pF | 0.3 / 0.1 V | 45 Ω to GND |

These are typical datasheet values. Import the vendor's IBIS file for sign-off.

### Assignment

A pin's model is looked up in this order: the pin (`U1.12`, by number or name), the component (`U1`), the net, and then
the default family. The **Driver model** menu in the Signals tab sets the net assignment. The C API also assigns by
component and pin.

## Transmission-line analysis

### Topology

The net's routed copper becomes a graph (`buildNetCopperGraph`). Tracks are split where another track of the net
ends on them (T-junctions). Every via has a landing on each copper layer its barrel spans, joined by barrel sections.
Pads join the track ends and via lands inside them. If the driver's copper reaches no receiver (unrouted), the net is
estimated on straight Manhattan lengths on the top layer, and the result says so.

**Driver:** an assigned output or I/O model first. Otherwise the pin with the strongest output type: Output, then
Bidirectional, then open collector. A PULSE / SIN test source or a connector pin can also drive. When the net has no
output but a resistor of ≤ 200 Ω connects it to a net that has one, that resistor is a **series termination**: the
driver is the part behind it, and the resistor adds to the source resistance.

**Loads:** logic inputs on ICs (measured receivers, with their model's C_in, package and on-die termination); resistors
to ground or a supply rail (parallel / Thévenin terminations, at the rail voltage); capacitors to a rail; 2 pF on
discrete semiconductor pins; 1 pF for a connector, whose off-board load is not modelled. Series resistors and AC
coupling capacitors that continue the signal to another net are reported in the notes.

### Line parameters

| Element | Model |
|---|---|
| Track impedance | IPC-2141 microstrip / stripline of the stack-up (`trackImpedance`, same as Board Setup) |
| Effective permittivity | Microstrip: Hammerstad & Jensen, `εeff = (εr+1)/2 + (εr−1)/2 · (1 + 12h/w)^−½` (+ 0.04 (εr−1)(1−w/h)² for w < h). Stripline: εr |
| Delay | `√εeff / c` per mm (FR-4 stripline ≈ 7.0 ps/mm, 178 ps/in) |
| Via barrel | Johnson & Graham, *High-Speed Digital Design* §7.3: `C = 1.41 εr T D1 / (D2 − D1)` pF and `L = 5.08 h [ln(4h/d) + 1]` nH (inches), anti-pad = pad + 2 × clearance. Each barrel section is a line with `Z = √(L/C)`. Unused barrel beyond the last connected layer is an open stub. With backdrilling on (fast / RF nets), the stub is removed |
| Pads | SMD: parallel plate `ε0 εr A / h` to the nearest plane; through-hole: as a via barrel |
| Package | A line of `Z = √(L_pkg/C_pkg)`, delay `√(L_pkg·C_pkg)` between pad and die; otherwise C_pkg at the pad |

### Solver

The network is solved in the time domain with **Bergeron's method of characteristics** (Dommel 1969). Each lossless
line is exact for its delay rounded to the time step, and capacitors use the trapezoidal companion model. Because the
lines decouple the nodes, each step is a scalar solve per node. Lines shorter than half a step are electrically short
and are lumped (their capacitance `delay / Z0` moves to the node). The run starts from the DC state, including the DC
current through terminations to VTT. It drives one rising edge, then one falling edge. The step is RT / 40, and each
half lasts long enough to settle: 20 round trips, 6 RC time constants or 4 rise times, whichever is longest.

Measured at each receiver:

| Figure | Definition |
|---|---|
| Overshoot | Highest voltage after the rising edge minus the settled high level |
| Undershoot | Settled low level minus the lowest voltage after the falling edge |
| Ringback | After first crossing VIH (VIL), how far the signal comes back. Below the threshold means a false edge |
| Settling | Time from the edge until the signal stays within ±5 % of the swing |
| Flight time | Receiver 50 % crossing minus the ideal source's 50 % crossing |

A receiver passes when overshoot and undershoot are within the limit (default **15 %** of the swing), there is no
ringback across a threshold, and both thresholds are reached.

### Critical length and termination

A line is **critical** when its longest one-way delay from the driver exceeds **RT / 6**: unterminated, it rings. This
is Bogatin's rule of thumb (*Signal and Power Integrity — Simplified*), about 1 inch per ns of rise time on FR-4. When
a net is critical or fails, the recommended series resistor is `Z0(trunk) − R_out − R_pkg`, rounded to E24. The net is
simulated again with it, and the Signals tab shows that waveform beside the original. An existing series resistor
that does not match is flagged with the right value. With more than two receivers, the advice is a daisy chain with a
far-end termination. On-die terminated receivers (DDR) get the ODT advice instead.


## Lossy lines

`Core/src/LossyLine.cpp` gives every track a per-unit-length model R(f), L(f), G(f), C(f). Its lossless limit is the
line the reflection analysis uses (same Z0 and delay), so the two agree when losses are off.

| Part | Model |
|---|---|
| L, C | From the stack-up line: `L = Z0·√εeff / c`, `C = √εeff / (Z0·c)` (IPC-2141 Z0, Hammerstad–Jensen εeff) |
| Dielectric | **Djordjevic–Sarkar** wideband Debye model, `ε(ω) = ε∞ + Δε/(m2 − m1)·log10((ω2 + jω)/(ω1 + jω))` with ω1 = 10⁴ and ω2 = 10¹² rad/s, fitted so εr and tan δ equal the laminate's at 1 GHz. Causal (Kramers–Kronig consistent). A microstrip fills `q = (εeff − 1)/(εr − 1)` of its field with it: `Y = jω·C_air·(1 + q(ε(ω) − 1))` |
| Conductor | `Z_int = R_dc·√(1 + jω/ωc)`: R_dc = ρ/(w·t) at low frequency, the skin-effect `Rs·K` with equal internal reactance at high frequency (causal). `K = R_ac / Rs` by **Wheeler's incremental-inductance rule**: stripline, Pozar's closed form (eq. 3.198, both branches); microstrip, the numerical Wheeler derivative of the Hammerstad–Jensen inductance with their thickness correction |
| Roughness | **Huray** snowball model in its causal form (complex skin depth δ(1 − j)/√2), or **Hammerstad** `1 + (2/π)·atan(1.4 (Δ/δ)²)`, applied to the skin-effect part. Foils: smooth, HVLP, VLP, RTF, standard ED (typical profile parameters; fit your fabricator's data for sign-off). By default low-loss laminates (tan δ < 0.005) use HVLP and others standard ED; **Copper foil** overrides it per project |

Time domain uses the frequency response and an inverse FFT (window ≥ 2× the span, raised-cosine taper above the
evaluated band). The models are causal, so nothing arrives before the time of flight.

## Channel analysis

`Core/src/Channel.cpp` turns a routed net into a linear network:

- **Elements.** Every track section is a lossy line; via barrel sections (and unused stubs, unless backdrilled) are
  short lines with Johnson's L and C, as in the reflection analysis; pads, terminations, connectors and the other
  receivers (C_in, C_pkg, ODT) are shunts. The driver pad and the receiver pad are the ports (the farthest receiver,
  or the one named). A net with no logic input (it ends at a connector or a test point) ends at its farthest pad,
  with no load model beyond it: cascade the connector or cable as Touchstone.
- **Differential pairs** (found by name: `_P / _N`, `+ / −`, …) form one 4-port, ordered P near, N near, P far, N far.
  Where P and N tracks run side by side on a layer, the sections are split so they pair up and become **coupled
  lines**: from the coupling coefficients of the crosstalk model, `L_e,o = L(1 ± kl)`, `C_e,o = C11(1 ∓ kc)`,
  `C11 = C/(1 − kl·kc)`. In a homogeneous stripline (kl = kc) both modes keep c/√εr; on a microstrip the odd mode runs
  faster (more of it in air). The report lists Z_odd, Z_even, Z_diff = 2·Z_odd and Z_comm = Z_even/2 per layer and gap.
- **Solution.** Each line stamps `Y11 = coth(γl)/Zc`, `Y12 = −csch(γl)/Zc` (a coupled pair: `(Ye ± Yo)/2`); every
  non-port node is eliminated (Kron reduction in minimum-degree order) and `S = (I + Z0·Y)⁻¹(I − Z0·Y)`, Z0 = 50 Ω by
  default. Mixed mode: `Smm = M·S·Mᵀ` (SDD, SDC / SCD, SCC; differential reference 2·Z0).
- **Touchstone.** Export writes version 1.1 (`# Hz S RI R 50`; 2-port in the S11 S21 S12 S22 order, 4-ports as rows).
  Import reads versions 1.x and 2.0: S, Y or Z data (Y / Z converted to S, normalised in 1.x), MA / DB / RI, any
  frequency unit, `[Two-Port Data Order]`, `[Matrix Format]`, `[Number of Frequencies]`, a single `[Reference]`;
  noise blocks are skipped. Malformed files are rejected with a reason (port count ≤ 64, 64 MB, finite numbers,
  increasing frequencies); the parser is fuzz-tested. A 4-port can be ordered "13" (1→3 thru, the default) or "12".
  An imported block **cascades** after the routed channel (Redheffer star product, renormalised to Z0); outside its
  band the magnitude is held and the phase extrapolated (constant group delay).
- **Drive.** The step response and the eye drive the channel from the driver model (R_out + R_pkg with C_comp, the
  schematic's series resistor, C_pkg) into the receiver (C_in + C_pkg, ODT), or from an ideal 50 Ω source into 50 Ω
  per leg. The port voltages come from the wave equations `(I − S·Γ)·b = S·c`.

## Eye diagram

`Core/src/Eye.cpp`. The pulse response of one unit interval with the driver's edge, `p(t) = IFFT[H(f)·P(f)]`, is
superposed for every bit of a PRBS (7, 9, 15: a full period, circular; 23, 31: the first 32 767 bits): exact for a
linear channel. The time window covers ≥ 128 UI and 80 channel delays so reflections settle.

| Result | Definition |
|---|---|
| Eye height | Inner opening (lowest "1" minus highest "0") at the best sampling phase |
| Eye width | UI minus the data-dependent jitter at the threshold |
| Jitter | Every threshold crossing against its nominal boundary: peak-to-peak (DJ) and RMS. Total jitter `TJ = DJ + 2·Q(BER)·RJ` (dual Dirac, Q(10⁻¹²) = 7.03) with the random jitter you enter; width at BER = UI − TJ |
| Worst-case eye | Peak distortion analysis: `2A·(c0 − Σ|ck|)` from the pulse cursors |
| Mask | A hexagon (width in UI, height in V) centred on the eye; the margin is the smallest clearance |
| CTLE | `H(s) = A(1 + s/ωz)/((1 + s/ωp1)(1 + s/ωp2))`, ωz = A·ωp1, ωp2 = 3ωp1, peak at Nyquist, followed by a gain stage restoring the unequalised main cursor; the DC gain is swept 0 … −20 dB for the largest worst-case eye |
| FFE | Transmit taps (1 pre, 2 post) by zero forcing on the cursors, Σ\|c\| = 1 |

## Crosstalk

Every pair of parallel tracks (|cos θ| ≥ 0.97, ≥ 0.5 mm overlap) on one layer is a candidate. One net of the pair must
be an aggressor: a fast or RF net, a net with an assigned model, or a net driven by an output pin. The pair must lie
within 6 dielectric heights of each other. Differential-pair partners are excluded, because their coupling is
intended.

| Line | Coupling |
|---|---|
| Stripline (homogeneous) | `Lm/L = Cm/C = (Z0e − Z0o)/(Z0e + Z0o)`. Even and odd impedances come from Cohn's exact zero-thickness coupled stripline (*IRE Trans. MTT-3*, 1955): `Z = 30π/√εr · K(k')/K(k)`, `k_e = tanh(πw/2b)·tanh(π(w+s)/2b)`, `k_o = tanh(πw/2b)·coth(π(w+s)/2b)` |
| Microstrip | Image theory for conductors at height `H = h + t/2` with the thin-strip equivalent radius `r = (w+t)/4` (C. R. Paul, *Introduction to EMC*): `Lm/L = ln(1 + (2H/D)²) / (2 ln(2H/r))`. Because part of the mutual field runs in air, `Cm/C = Lm/L · ((εr+1)/2) / εeff` |

From these, with `TD` = coupled length × delay per mm and `RT` the aggressor's rise time:

- **NEXT** = `(Cm/C + Lm/L)/4 · min(1, 2·TD/RT)`. Near-end noise saturates once the coupled region is longer than half
  the rise time.
- **FEXT** = `½ (Cm/C − Lm/L) · TD/RT`. It is negative on a microstrip, where inductive coupling dominates, and zero on
  a stripline.

Coupling along several segments of the same pair is summed. The noise voltage is the larger of |NEXT| and |FEXT|
times the aggressor's launched step `V·Z0/(Z0 + R_out)`. The limit is **5 %** of the victim's swing by default.

**Broadside crosstalk.** A victim on the adjacent copper layer (no plane can lie between two adjacent layers that
both carry tracks there), within three layer spacings laterally, is a broadside pair (shown as *Broadside*, layer
"Top / Inner 1"). Its coupling uses the same image theory with the conductors at heights h1 and h2 over the nearest
plane outside the pair: `Lm/L = ln((x² + (h1+h2)²)/(x² + (h1−h2)²)) / (2·√(ln(2h1/r)·ln(2h2/r)))`. Both layers buried:
homogeneous, `Cm/C = Lm/L` and no FEXT; otherwise the air-filled share lowers Cm/C. The second plane on the far side
is ignored, so the estimate errs high. NEXT and FEXT follow the formulas above.

## Return path

Fast / RF nets and nets with an assigned model are checked against the planes next to their layer. Planes are zones
marked *plane*, or pours covering at least a quarter of the board.

| Code | Finding |
|---|---|
| `SI_PLANE_GAP` | The track crosses a gap in its reference plane at least 1.5 mm long (or anti-pad + 0.5 mm). Small anti-pad holes around vias are ignored |
| `SI_PLANE_SPLIT` | The copper under the track changes from one plane net or island to another (a split plane) |
| `SI_REFERENCE_CHANGE` | A via moves the signal between layers whose reference planes differ. If the two planes are the same net on different layers, a stitching via of that net is needed within 2 mm. If they are different nets, a capacitor between them is needed within 6 mm |

## Power distribution network

Every power or negative-supply net with two or more pads is a rail.

**Inputs:**

- **Voltage:** from the name (`3V3`, `+1V8`, `VCC_5V`, `1.2V`), else a regulator model's output, else a DC source.
- **DC load:** each IC's behavioural supply load on that rail, split over its power pins. ICs without a load model
  count as 10 mA (marked estimated). A regulator's input carries its output rail's load (switching converters:
  `P_out / (η·V_in)`).
- **Load step:** the transient current, half the DC load unless set.
- **Ripple:** 5 % (3 % for rails ≤ 1.5 V) unless set.
- **Target impedance:** `Z_target = V · ripple / I_step` (Smith & Swaminathan; Novak).

**Elements**, in parallel:

| Element | Model |
|---|---|
| VRM / source | Output resistance R with the inductance `L = R / (2π f_bw)` that models its loop roll-off: switching regulator 5 mΩ / 50 kHz, LDO or load switch 20 mΩ / 100 kHz, bench supply 10 mΩ / 10 kHz, connector feed 20 mΩ / 10 kHz |
| Decoupling capacitor | `ESR + jω(ESL + L_mount) + 1/(jωC)`. ESL by case (0201 0.3, 0402 0.4, 0603 0.55, 0805 0.7, 1206 0.9 nH; tantalum 1.2–1.5 nH; radial electrolytic 5 nH). MLCC ESR `≈ 30 mΩ · (C / 100 nF)^−0.3` |
| Mounting inductance | Per side: pad in the pour → 0.05 nH. Plane on another layer → trace to the nearest via (`Z0·t_pd` per mm) plus the via barrel to the plane (Johnson). No plane → trace to the nearest load pin. Plus 0.1 nH for pads and escape |
| Plane pair | Rail pour facing a ground pour: `C = ε0 εr A / d` over the overlapping copper, spreading inductance `½ μ0 d`. The first cavity resonance `c / (2 · span · √εr)` is reported |

`|Z(f)|` is computed from 1 kHz to 1 GHz at 40 points per decade. The target applies up to **100 MHz**; above that,
package and die capacitance decouple, and they are not modelled. Every local maximum at least 1.3× above its
surroundings is listed as an **anti-resonance**.

**Recommendations:**

- Too high below 1 MHz: bulk capacitance `C ≥ 1/(2π f Z_target)`.
- Anti-resonance above the target: a capacitor whose self-resonance sits at that frequency,
  `C = 1/((2πf)² L_mount)`, rounded to E12.
- Inductive above 1 MHz: the allowed inductance `Z_target / (2πf)` and how many more 0402 capacitors reach it.
- 4+ layer board without a rail plane: pour the rail next to ground.

## IR drop

The rail's tracks, via barrels and pours form a resistive network solved by conjugate gradients from the source pads
(regulator output, supply, or connector pin) to every load pin, each sinking its DC current.

| Element | Resistance |
|---|---|
| Track | `ρ·L / (w·t)`, ρ = 1.72 × 10⁻⁸ Ω·m (annealed copper, 20 °C), t from the copper weight |
| Via | `ρ·h / (π·t_p·(d + t_p))`, plating t_p = 25 µm |
| Pour | Coarse mesh of the zone fill (cells ≥ 0.5 mm), one sheet resistance `ρ/t` per square between neighbouring cells (1 oz ≈ 0.49 mΩ/□); pads, track ends and via lands on the pour join it |

The drop limit is half the ripple budget (2.5 % on a 5 % rail).

## PI planning

`Core/src/PdnPlanning.cpp` (Power tab, below the rail's curve):

- **Regulator.** Output resistance R and loop bandwidth f_bw per rail (0 = by regulator type, as above); the
  inductance `R/(2π f_bw)` models the loop's roll-off.
- **Plane mesh (real pour shape).** When the rail has an IR-drop map, its pour cells (merged to at most 40 per side)
  become a 2D RLGC grid: each cell has `C = ε0εr·A/d` (with tan δ) to the ground plane, neighbours are joined by
  `L = μ0·d` per square and the skin-effect resistance of both plates, and every decoupling capacitor and the regulator
  attach at the cell under them. A banded complex LU gives |Z| at the loads per frequency ("Plane mesh" curve). An
  L-shaped or notched pour resonates where its real shape says, not its bounding box; the worst |Z| / target and the
  **load-step droop** (transient current × worst |Z| up to 1 GHz, against V × ripple) use this curve when present.
- **Plane cavity.** The rail pour and the ground pour as a rectangular cavity (the pour's bounding box, plate spacing
  d). The impedance between ports is the modal sum of Lei, Mittra & Wang / Novak,
  `Z_ij = jωμd/(ab)·Σ χm²χn²·cos(kx·xi)cos(ky·yi)cos(kx·xj)cos(ky·yj)·sinc(kx·w/2)·sinc(ky·w/2) / (k_mn² − k²)`,
  `k² = ω²με(1 − j(tan δ + 2Rs/(ωμd)))` (conductor loss on the propagating modes; the (0,0) term is the plate
  capacitor). Modes up to twice 3 GHz are summed dynamically, the higher ones as their static (inductive) sum. Every
  decoupling capacitor (with its mounting inductance) and the regulator connect at their own positions; the curve is
  the impedance at the loads' centre, beside the lumped one. The first resonances `f_mn = c/(2√εr)·√((m/a)² + (n/b)²)`
  are listed.
- **Decoupling plan.** Greedy: at each step the capacitor from 1 nF 0201, 10 nF / 100 nF / 1 µF 0402, 4.7 µF 0603,
  10 / 22 µF 0805 and 47 µF 1206 that lowers the worst |Z| / target below 100 MHz most, at the rail's median mounting
  inductance (0.5 nH without capacitors), until the rail complies (≤ 40 parts).
- **IR-drop map.** The IR-drop solution per pour cell and track section: drop, and current density `|∇V|/ρ` (cells)
  or `I/(w·t)` (tracks) in A/mm², drawn on the board outline with the source and the load pins. The five highest
  densities are listed (above 30 A/mm² in amber; check IPC-2152 for the temperature rise).

## Verification and check codes

SI / PI sign-off is **opt-in per project**, with the **Sign-off in verification** toggle or `sieda_si_set_options`.
With it on, **Design → Verify Design** adds a **Signal & Power Integrity** stage after Design for Reliability. Its
findings are warnings, so the verdict becomes *warning*, never *fail*. Projects without sign-off keep their 8-stage
report, unchanged. The stage analyses up to 120 nets: those with a real driver, fast / RF nets, and nets with an
assigned model, fast and long first.

| Code | Severity | When |
|---|---|---|
| `SI_OVERSHOOT` | warning | Overshoot or undershoot at the worst receiver above the limit (with the termination advice) |
| `SI_RINGBACK` | warning | Ringing re-crosses VIH or VIL |
| `SI_THRESHOLD` | warning | The receiver never reaches VIH or VIL (a heavy termination or a divider) |
| `SI_LONG_LINE` | info | Longer than the critical length but within limits; a series resistor is advised |
| `SI_CROSSTALK` | warning | NEXT or FEXT noise above the limit |
| `SI_PLANE_GAP`, `SI_PLANE_SPLIT`, `SI_REFERENCE_CHANGE` | warning | See [Return path](#return-path) |
| `PI_NO_DECOUPLING` | warning | A rail feeds load pins but has no capacitor to ground |
| `PI_TARGET_IMPEDANCE` | warning | \|Z\| above the target within 100 MHz (worst point, target and the first recommendation) |
| `PI_IR_DROP` | warning | The worst load pin drops more than the limit |
| `SI_EYE_MASK` | warning | A net given a bit rate (**Check in sign-off**) has a closed eye or violates its mask: PRBS7 with the net's IBIS driver, else an ideal 50 Ω source and termination with a 1 V swing |

Broadside pairs report under `SI_CROSSTALK`. Projects without channel specs get no `SI_EYE_MASK` finding.

## C API

| Function | Returns |
|---|---|
| `sieda_ibis_parse(text, corner, &err)` | Preview: components, pins, models with their derived buffers, warnings |
| `sieda_si_import_ibis(project, text, corner, ref, &err)` | Imports every model; with `ref`, maps that part's pins. Returns the model count |
| `sieda_si_assign_model(project, kind, target, model_id)` | `kind` = `net`, `component` or `pin`; an empty id clears |
| `sieda_si_set_options(project, sign_off, overshoot, crosstalk)` | Sign-off and limits (fractions of the swing) |
| `sieda_pi_set_rail(project, net, ripple_percent, transient_A, dc_A)` | Rail inputs; 0 derives them, all 0 removes the override |
| `sieda_si_settings_json(project)` | Settings, imported models and the logic families |
| `sieda_si_net_list_json(project)` | Net picker rows: length, delay, critical, driver, model |
| `sieda_si_net_json(project, net, series_ohms)` | Full analysis with waveforms (`series_ohms ≥ 0` adds a what-if resistor) |
| `sieda_si_crosstalk_json(project)` | Crosstalk pairs and return-path issues |
| `sieda_pi_json(project)` | Every rail: inputs, elements, \|Z(f)\| curve, peaks, compliance, IR drop, recommendations |
| `sieda_si_checks_json(project)` | All SI_* / PI_* findings |
| `sieda_si_line_loss_json(project, options)` | Loss per layer (dB / inch, conductor and dielectric parts, RLGC at 1 GHz) |
| `sieda_si_set_copper_foil(project, foil)` | Foil for loss: `smooth`, `hvlp`, `vlp`, `rtf`, `std`, "" = by laminate |
| `sieda_si_channel_json(project, options)` | Channel of a net or pair: S-parameter curves, coupled sections, step response, eye (options in `sieda_c.h`) |
| `sieda_si_channel_touchstone(project, options, &err)` | The channel as Touchstone .s2p / .s4p text |
| `sieda_touchstone_parse(text, ports, order, &err)` | Touchstone preview (ports, band, curves) |
| `sieda_touchstone_channel_json(text, ports, options, &err)` | An imported channel's step response and eye |
| `sieda_si_set_channel(project, net, bit_rate, mask_v, mask_ui)` | Eye-mask check in sign-off (bit rate 0 removes) |
| `sieda_pi_set_vrm(project, net, r_out, loop_bw)` | Regulator model of a rail (0 = by type) |
| `sieda_pi_cavity_json(project, net)` | Plane-cavity model and modes |
| `sieda_pi_decap_plan_json(project, net)` | Decoupling plan |
| `sieda_pi_ir_map_json(project, net)` | IR-drop / current-density map |

## Validation

The core tests check the physics against reference values:

- **Lattice diagram:** 25 Ω source, 50 Ω / 1 ns line, open end. The far end reads 4/3, 8/9 and 28/27 V at 1, 3 and
  5 TD; overshoot is 1/3; flight time is 1 ns. A matched source steps cleanly. A far-end parallel termination to
  0.5 V starts at the DC divider and settles at 0.75 V. An RC load reaches 63 % at τ.
- **Johnson's worked via example:** 0.50 pF and 1.0 nH.
- **FR-4 stripline delay:** 178 ps/in. The Hammerstad–Jensen εeff at w/h = 2.
- **Elliptic integral:** K(1/√2) = 1.854075. Cohn's stripline matches Pozar's closed form (65.4 Ω at w/b = 1 in air)
  within 2 %, and the even and odd modes merge far apart.
- **Crosstalk:** NEXT falls with spacing, saturates with length, and FEXT is negative on a microstrip and zero on a
  stripline.
- **IBIS:** a 25 Ω / 30 Ω I-V pair gives R_out 27.5 Ω, a 0.6 ns `[Ramp]` gives a 0.8 ns edge, and a rising waveform
  table gives its 20–80 % time × 4/3. Corners, package, selector, `NA`, `[Comment Char]` and error handling are
  covered.
- **Lossy lines:** Djordjevic–Sarkar reproduces εr and tan δ at 1 GHz and stays causal; skin depth 2.09 µm and Rs
  8.24 mΩ/□ at 1 GHz; R → R_dc at low frequency and ∝ √f above; Hammerstad–Jensen air microstrip 126.5 Ω at w = h;
  Wheeler's rule → 2/w for a wide microstrip; **Pozar Example 3.5** (50 Ω stripline, 10 GHz): α_c = 0.122 Np/m and
  α_d = 0.155 Np/m (exact TEM `π f √εr tan δ / c` to 0.2 %); Bogatin's 2.3·f·tan δ·√εr dB/inch; roughness limits
  (Hammerstad → 2, Huray → 1 + SR). The lossless model equals the stack-up Z0 and delay.
- **Frequency-domain time response:** the lossless lattice through the FFT path gives 4/3, 8/9 and 28/27 V at 1, 3 and
  5 TD (as Bergeron's method); a 200 mm FR-4 stripline is causal (< 0.2 % before the time of flight), settles at the
  DC divider including R_dc and has a slower edge.
- **S-parameters:** a 70 Ω line against the closed form `S11 = Γ(1 − e^{−2jθ})/(1 − Γ²e^{−2jθ})` (10⁻⁵); two halves
  cascaded equal the whole; routed nets are reciprocal and passive, and the lossless twin conserves power. Mixed mode:
  uncoupled lines give SDD21 = S21 and no conversion; a coupled stripline referenced to Z_odd is matched
  differentially; a routed pair finds its coupled run with Z_odd < Z0 < Z_even and ε_odd < ε_even on a microstrip.
  Cascading the pair's own Touchstone doubles its loss.
- **Touchstone:** formats, units, Y / Z conversion, orders, version 2 keywords, noise blocks, ten malformed files, a
  4-port round trip, and 3 000 fuzzed inputs (mutations and random soup: success or a clean rejection, nothing else).
- **Eye:** PRBS7 / 15 maximal length; an ideal channel opens fully (height 2A, width 1 UI, no DJ); an RC channel
  (τ = T/2) matches the analytic worst-case eye `2A(1 − 2e^{−T/τ})` and PRBS7 reaches it; RJ adds 2·7.03·RJ to TJ;
  masks pass and fail; CTLE and FFE open a −14 dB channel.
- **Broadside:** strongest directly over, falls with offset, no FEXT when buried, negative FEXT with a microstrip side;
  found on a board (Top over Inner 1).
- **PI planning:** the cavity's low-frequency |Z| is the plate capacitance (2 %), its first peak at a corner port is
  f10 = c/(2a√εr) (2 %), the centre port does not excite (1,0); the regulator override sets R and L = R/(2π f_bw);
  the decoupling plan makes a bare rail compliant; the IR map's track density matches I/(w·t) (28.6 A/mm² for 0.5 A
  in 0.5 mm × 35 µm). The plane mesh of a 60 × 40 mm pair has the plate capacitance at 10 MHz (2 %) and its first
  peak at f10 (3 %); an L-shaped pour resonates away from its bounding box; a capacitor at the port lowers |Z|.
- **PDN:** target impedance, |Z| = ESR at self-resonance, plane capacitance (100 cm², 0.1 mm, FR-4 = 3.9 nF), 1 oz
  sheet resistance 0.49 mΩ/□. On a two-load rail, the IR drop matches the hand calculation `ρL/(wt)` to 0.2 mV.

## Limits

- **Reflection analysis is lossless.** The Signals tab keeps Bergeron's lossless solver (its results are unchanged);
  losses, dispersion and frequency-dependent εr are in the Channel tab. The lossy models are closed form: no proximity
  effect between close conductors, no microstrip dispersion of εeff (Kirschning–Jansen), no glass-weave skew, the
  return plane's DC resistance is ignored, and the roughness parameters are typical, not your fabricator's.
- **Channel / eye.** Linear and noise-free: no crosstalk aggressors in the eye, no DFE, no receiver noise or
  non-linearity, no IBIS-AMI. The driver is the linear IBIS reduction (R_out, edge) or an ideal source. Vias are lumped
  L–C lines (Johnson), not a 3D via model; connectors need a vendor Touchstone file. Differential coupling uses the
  same closed-form kl / kc as crosstalk (exact for a homogeneous stripline, an approximation on a microstrip).
- **Linear buffers.** IBIS I-V and V-t curves are reduced to a resistance and a linear ramp. No clamp diodes, no
  non-linear drive and no simultaneous switching noise.
- **Reflection analysis is single-ended.** The Signals tab analyses a differential pair one leg at a time; the Channel
  tab treats it as a coupled 4-port with mode conversion (SCD21) and differential impedance.
- **Closed-form coupling** for edge-coupled lines and broadside pairs on adjacent layers (image theory, errs high).
  Coupling to more than one neighbour at once and coupling through vias or connectors are not computed. Microstrip coupling is an
  approximation (thin-strip image theory); treat it as an estimate and confirm tight couplings with a field solver.
- **One driver per net.** Multi-master buses are analysed from the strongest output found.
- **PDN** is board level: no package or die capacitance. The compliance check uses the lumped model; the cavity model
  (rectangular, the pour's bounding box, one plane pair, square ports) and the plane mesh (the real pour shape, one
  plane pair, needs the IR-drop map) show the plane behaviour beside it. The VRM is an
  R–L model with a loop bandwidth, not a switching or control-loop simulation. Load currents without a behavioural
  model are estimates.
- **IR drop** uses the supply copper only. The ground return's own drop and the temperature rise of copper are not
  included.
- **No full-wave 3D field solver.** Use a field solver for via transitions, connectors and anything above a few GHz.

## Code map

| Area | File |
|---|---|
| IBIS parser, buffer reduction, logic families | `Core/include/sieda/Ibis.hpp`, `Core/src/Ibis.cpp` |
| Physics, copper graph, line solver, net analysis, crosstalk, return path, checks | `Core/include/sieda/SignalIntegrity.hpp`, `Core/src/SignalIntegrity.cpp` |
| PDN impedance, decap parasitics, IR drop | `Core/include/sieda/PowerIntegrity.hpp`, `Core/src/PowerIntegrity.cpp` |
| Lossy lines (RLGC, roughness, Djordjevic–Sarkar) | `Core/include/sieda/LossyLine.hpp`, `Core/src/LossyLine.cpp` |
| Channel network, S-parameters, mixed mode, cascading | `Core/include/sieda/Channel.hpp`, `Core/src/Channel.cpp` |
| Touchstone parser / writer | `Core/include/sieda/Touchstone.hpp`, `Core/src/Touchstone.cpp` |
| PRBS eye, CTLE / FFE, jitter, mask | `Core/include/sieda/Eye.hpp`, `Core/src/Eye.cpp` |
| Plane cavity, decoupling plan, IR map | `Core/include/sieda/PdnPlanning.hpp`, `Core/src/PdnPlanning.cpp` |
| Channel tab, Power-tab planning | `SiEDA/Views/Simulation/ChannelView.swift`, `SiEDA/Views/Simulation/PowerPlanningView.swift`, `SiEDA/Models/ChannelAnalysis.swift` |
| Settings in the project (`Project::si`, `signalIntegrity` JSON) | `Core/include/sieda/Project.hpp`, `Core/src/Project.cpp` |
| Verification stage | `Core/src/Verification.cpp` |
| C API | `Core/include/sieda/sieda_c.h`, `Core/src/sieda_c.cpp` |
| Swift bridge and models | `SiEDA/Bridge/EDAEngine.swift`, `SiEDA/Models/SignalIntegrity.swift`, `SiEDA/App/DesignStore.swift` |
| SI / PI panel | `SiEDA/Views/Simulation/SignalIntegrityView.swift` (mode picker in `SimulationView.swift`) |

## Tests

`Core/tests/core_tests.cpp`:

- `si_ibis_import`
- `si_physics_reference_values`
- `si_crosstalk_coupling`
- `si_transmission_line_lattice`
- `si_net_reflections_and_termination`
- `si_ibis_model_drives_the_net`
- `si_crosstalk_and_return_path`
- `pi_pdn_impedance_and_ir_drop`
- `si_verification_sign_off`
- `si_lossy_line_physics`
- `si_channel_sparameters`
- `si_touchstone_parser`
- `si_touchstone_fuzz`
- `si_differential_pair_coupled_lines`
- `si_eye_diagram`
- `si_channel_sign_off_and_c_api`
- `si_broadside_crosstalk`
- `pi_cavity_decap_plan_and_ir_map`
- `plane_mesh_matches_cavity_resonance_and_plate_capacitance`
- `si_channel_ends_at_connector`

`Core/tests/c_api_test.c` covers the C functions (`sieda_c_api_channel_test`, `sieda_c_api_pi_test`), and
`SiEDATests/SiEDATests.swift` (`SignalIntegrityBridgeTests`, `ChannelAnalysisBridgeTests`) covers the Swift bridge.
