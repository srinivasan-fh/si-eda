# Signal & Power Integrity

SiEDA analyses the routed board for signal integrity (SI) and power integrity (PI) at board level, the job tools such
as HyperLynx LineSim / BoardSim or Sigrity PowerSI do in their screening modes. It solves every net as a set of
lossless transmission lines driven by IBIS or logic-family buffer models, estimates crosstalk between neighbouring
tracks, checks return paths, and compares each supply rail's power-distribution network (PDN) impedance with its
target. It also solves the DC voltage drop (IR drop) on supply copper.

It is a screening tool built on closed-form physics from the standard references. It is **not** a full-wave 3D field
solver. See [Limits](#limits) before you sign off a multi-gigabit or high-current design with it alone.

- [Where to find it](#where-to-find-it)
- [Driver and receiver models](#driver-and-receiver-models)
- [Transmission-line analysis](#transmission-line-analysis)
- [Crosstalk](#crosstalk)
- [Return path](#return-path)
- [Power distribution network](#power-distribution-network)
- [IR drop](#ir-drop)
- [Verification and check codes](#verification-and-check-codes)
- [C API](#c-api)
- [Validation](#validation)
- [Limits](#limits)
- [Code map](#code-map)
- [Tests](#tests)

## Where to find it

Open the **Simulation** workspace and switch the mode picker from **Circuit** to **SI / PI**. There are three tabs:

| Tab | What it shows |
|---|---|
| **Signals** | Every signal net, with critical and fast nets first. Select a net to see its driver and model, its line sections (layer, width, length, Z0, delay), the waveform at the driver pin and at the worst receiver, and the same receiver with the recommended series termination. Overshoot, undershoot, settling and flight time are listed for every receiver. The **Driver model** menu assigns a logic family or an imported IBIS model to the net. |
| **Crosstalk** | Every aggressor → victim pair of parallel tracks with coupled length, spacing, NEXT, FEXT and the noise voltage against its limit. Below it, the return-path problems. |
| **Power** | Every supply rail with its \|Z(f)\| curve on log–log axes, the target impedance (dashed line) and the 100 MHz band edge. It also lists decoupling capacitors (ESR, ESL with mounting, self-resonance), recommendations and the worst IR drop. **Ripple** (%) and **Load step** (A) set the target; 0 derives them from the design. |

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
- **PDN:** target impedance, |Z| = ESR at self-resonance, plane capacitance (100 cm², 0.1 mm, FR-4 = 3.9 nF), 1 oz
  sheet resistance 0.49 mΩ/□. On a two-load rail, the IR drop matches the hand calculation `ρL/(wt)` to 0.2 mV.

## Limits

- **Lossless lines.** No conductor (skin-effect) or dielectric loss, no dispersion and no frequency-dependent εr.
  Fine for edge rates down to about 100 ps on short board traces. Not a channel analysis for multi-gigabit SerDes:
  no eye diagrams, S-parameters, equalisation or jitter.
- **Linear buffers.** IBIS I-V and V-t curves are reduced to a resistance and a linear ramp. No clamp diodes, no
  non-linear drive and no simultaneous switching noise.
- **Single-ended.** Differential pairs are analysed one leg at a time. Mode conversion and differential impedance
  discontinuities are not computed; Board Setup sizes pair geometry.
- **Closed-form coupling** for edge-coupled lines on one layer only. Broadside coupling between layers, coupling to
  more than one neighbour at once and coupling through vias or connectors are not computed. Microstrip coupling is an
  approximation (thin-strip image theory); treat it as an estimate and confirm tight couplings with a field solver.
- **One driver per net.** Multi-master buses are analysed from the strongest output found.
- **PDN** is board level: no package or die capacitance, spreading inductance as a single term, only the first cavity
  resonance and no plane-mode analysis. The VRM is a two-element model. Load currents without a behavioural model are
  estimates.
- **IR drop** uses the supply copper only. The ground return's own drop and the temperature rise of copper are not
  included.
- **No full-wave 3D field solver.** Use a field solver for via transitions, connectors and anything above a few GHz.

## Code map

| Area | File |
|---|---|
| IBIS parser, buffer reduction, logic families | `Core/include/sieda/Ibis.hpp`, `Core/src/Ibis.cpp` |
| Physics, copper graph, line solver, net analysis, crosstalk, return path, checks | `Core/include/sieda/SignalIntegrity.hpp`, `Core/src/SignalIntegrity.cpp` |
| PDN impedance, decap parasitics, IR drop | `Core/include/sieda/PowerIntegrity.hpp`, `Core/src/PowerIntegrity.cpp` |
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

`Core/tests/c_api_test.c` covers the C functions, and `SiEDATests/SiEDATests.swift`
(`SignalIntegrityBridgeTests`) covers the Swift bridge.
