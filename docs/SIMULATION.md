# Circuit simulation

SiEDA's simulator is a Modified Nodal Analysis (MNA) solver in the C++ core (`Core/src/Simulator.cpp`,
`Core/src/Analysis.cpp`). It runs the analyses below on the schematic as drawn; nothing is exported to an external
SPICE. The **Simulation** workspace shows the DC operating point on the left and the selected analysis on the right
(**Analysis type**: Transient, AC Sweep, DC Sweep, Monte Carlo).

| Analysis | What it computes | App | C API |
|---|---|---|---|
| DC operating point | Node voltages, branch currents and dissipation | DC Operating Point | `sieda_simulate_dc` |
| Transient | Waveforms over time, microcontroller firmware co-simulation | Transient | `sieda_simulate_transient` |
| AC small-signal | Magnitude / phase of every node over a log frequency sweep; bandwidth, peak, unity gain, phase margin | AC Sweep (Bode plot) | `sieda_simulate_ac` |
| DC sweep | Operating point at each value of a swept source | DC Sweep | `sieda_simulate_dc_sweep` |
| Parameter sweep | DC, AC or transient once per value of one component | — | `sieda_simulate_param_sweep` |
| Monte Carlo / worst case | Spread of a node's DC voltage, low-frequency gain, −3 dB frequency or peak frequency over R/C/L tolerances | Monte Carlo | `sieda_simulate_monte_carlo` |
| FFT / THD | Spectrum and harmonic distortion of a node's transient waveform | — | `sieda_simulate_fft` |

## Sources

A voltage source, battery, AC source or current source takes its value as text:

| Value | Meaning |
|---|---|
| `5`, `5V`, `DC 5` | DC |
| `SIN(off amp freq)` | Sine: offset, peak amplitude, frequency |
| `PULSE(v1 v2 period [duty])` | Square wave from `v1` to `v2` (SPICE semantics: `v1` at t = 0) |
| `… AC mag [phase]` | Adds a small-signal stimulus for AC analysis, e.g. `0 AC 1`, `SIN(0 1 1k) AC 1`, `2.5 AC 1 90`. `AC 1` alone means DC 0, AC 1 |

The AC part does not change the DC operating point or the transient; it only sets which source drives the AC
analysis and with what phasor. The SPICE netlist export writes it as `AC mag phase`.

## DC operating point and transient

Newton–Raphson on the MNA equations, with Gmin stepping and then source stepping when the plain solve does not
converge. Device models: diode / LED (Shockley), NPN (Ebers–Moll transport model), N-MOSFET (square law with channel
length modulation), saturating op-amp (open-loop gain 10⁶, ±15 V output), behavioural regulators, chargers, IC supply
loads and the INA333 instrumentation amplifier. Part numbers in the value select device parameters (`1N4148`,
`BC847`, `2N7002`, `SI2302`, … see `Core/src/DeviceModels.cpp`). The transient uses backward-Euler companion models
for capacitors and inductors with a fixed step; microcontrollers with firmware run in lockstep with it.

## AC small-signal analysis

**Method.** The circuit is solved at its DC operating point first. The Newton Jacobian at that point is the
small-signal conductance matrix, so every non-linear device is linearised exactly as the DC solve sees it:

| Element | Small-signal model |
|---|---|
| R | conductance 1/R |
| C | admittance jωC |
| L | branch with impedance jωL |
| Independent sources | the stimulus source carries its AC phasor; every other voltage source is a short, every other current source an open |
| Diode / LED | dynamic conductance n·V_T / (I + I_s) at the operating current |
| NPN | g_m, g_π and the reverse-junction terms of the Ebers–Moll model at the bias point |
| N-MOSFET | g_m and g_ds of the square-law model (λ included) |
| Op-amp | single-pole gain–bandwidth model: A(f) = A′ / (1 + j·f·A′ / GBW), A′ the open-loop gain at the operating point (10⁶ in the linear region, falling as the output saturates) |
| Regulators, INA333, IC loads | their linearisation at the operating point (frequency independent) |

The complex MNA system is solved by LU decomposition with partial pivoting at each frequency of a logarithmic sweep
(SPICE `.AC DEC`): `start`, `stop` and `pointsPerDecade` (default 1 Hz – 1 MHz, 50 points per decade, at most
20 000 points).

**Op-amp gain–bandwidth.** Taken from the value: `GBW=10MEG` anywhere in it sets it explicitly; otherwise a known part
number gives its typical datasheet figure (LM358 / LM324 / LM2904 / LM741 / LMV321 / LMV358 / MCP6001-4 / TLV900x
1 MHz, TL07x / TL08x 3 MHz, NE5532 / NE5534 / MCP6021 / AD8605 10 MHz, OPA134 / OPA2134 / OPA4134 8 MHz, LM833
15 MHz, OPA1612 40 MHz, OPA350 38 MHz, AD8628 2.5 MHz, OPA333 350 kHz, OP07 600 kHz); anything else is 1 MHz. Vendor
figures differ (an LM358 is specified anywhere from 0.7 to 1.2 MHz); put `GBW=` in the value when it matters.

**Stimulus.** In order: the source chosen as the input (**Input** in the app, `"source"` in the C API), which uses
its `AC` phasor or 1 V / 1 A at 0°; otherwise every source whose value has an `AC` magnitude; otherwise the first
SIN or AC source with AC 1. Without any of these the analysis stops with "No AC stimulus".

**Readouts** (per node, in the readout table under the Bode plot):

| Readout | Definition |
|---|---|
| Gain | Gain at the sweep start (the pass-band reference of a low-pass response) |
| −3 dB | First frequency above the start where the gain is 3.01 dB (half power) below the start gain |
| Peak | Maximum gain and its frequency, when the maximum lies inside the sweep (a resonance or band-pass) |
| Bandwidth (C API `bwLowHz`, `bwHighHz`) | Half-power points either side of an interior peak |
| Unity gain | Where the gain falls through 0 dB |
| Phase margin | 180° + phase at the unity-gain frequency, wrapped to (−180°, 180°]; meaningful when the node is a loop gain |

The sweep brackets each crossing; the core then re-solves the circuit at intermediate frequencies (regula falsi for
the crossings, golden-section search for the peak) so the readouts are accurate to about 10⁻⁷ relative instead of
being interpolated between sweep points. On very large circuits a solve budget caps this refinement and the remaining
nodes are interpolated (dB linear in log f, typically within 0.05 % at 50 points per decade). The −3 dB point is
referred to the gain at the sweep *start*: start the sweep at least a decade below the corner (an RC low-pass swept
from fc / 100 reads 0.01 % high, from fc / 10 about 1 % high).

**Limits.** No junction or diffusion capacitances (diodes, BJTs and MOSFETs are purely resistive in AC, so their
high-frequency roll-off is not modelled), op-amps have one pole (no second pole, slew rate or output impedance), and
regulators and the INA333 are frequency independent. There is no noise analysis.

## DC sweep

Steps the value of one voltage or current source (`"source"`: its reference) from `start` to `stop` in `step` and
solves the operating point at every value, continuing from the previous solution (with the full Gmin / source
stepping fallback when a point does not converge). Reports node voltages and component currents per point: transfer
curves, diode I–V, transistor characteristics, regulator dropout. Up to 100 000 points.

## Parameter sweep (C API)

`sieda_simulate_param_sweep` runs a DC operating point, an AC sweep or a transient once per value of one component
(`"values"`: up to 100 value strings such as `"1k"`, `"2k2"`, `"TL072"`), on a copy of the schematic; the design is
not modified. With `"net"` only that node is returned (transient waveforms are decimated to 1000 points).

## Monte Carlo and worst case

**Tolerances.** Every resistor, capacitor and inductor varies within its tolerance: a percentage in its value
(`10k 1%`, `100n ±5%`) or the default (R 1 %, C 10 %, L 20 %; the C API's `"tolerances"` overrides the defaults).
Sources, semiconductors and fuses stay at their nominal values.

**Monte Carlo.** Each run draws every part's value independently: uniform within ±tolerance (default, as LTspice's
`mc()`), or Gaussian with σ = tolerance / 3 clipped at ±tolerance. The random numbers come from a seeded SplitMix64
generator with portable uniform and Box–Muller normal deviates, so the same seed reproduces the same runs. Reported:
nominal, mean, sample standard deviation, minimum and maximum, a 20-bin histogram, and the samples. Up to 10 000 runs;
runs that fail to converge are counted and left out.

**Worst case.** Each part is moved alone to its +tolerance end to find the sign of its effect (its sensitivity), then
all parts are set to the end that raises the measurement (maximum) and to the end that lowers it (minimum). This
extreme-value analysis is exact for responses that are monotonic in each value, which component values almost always
give; a response with an interior optimum (for example a tuned circuit measured off resonance) can exceed it.

**Measurements**: `"dc"` (node voltage), `"gain"` (AC gain at the sweep start), `"f3db"` (AC −3 dB frequency) and
`"peak"` (AC peak frequency). The app offers DC voltage and bandwidth; the bandwidth sweep runs from 1 Hz to 100 MHz.

Example: a 10 k / 10 k divider from 10 V with 1 % resistors measures 5 V nominal, 4.95 V – 5.05 V worst case, and
σ ≈ 20 mV over uniform Monte Carlo runs (5 · √(2/3) · 1 % / 2).

## FFT and THD (C API)

`sieda_simulate_fft` runs a transient analysis and analyses one node over the last whole number of fundamental
periods after `"from"` (default: half the stop time, so start-up transients have settled). The window is resampled to
a power-of-two number of points (256 – 32 768) and transformed without a window function: with a whole number of
periods every harmonic falls exactly on a bin. The fundamental defaults to the first SIN source's frequency, or is
detected as the strongest component. THD = √(A₂² + … + A_n²) / A₁ over `"harmonics"` orders (default 10). The
spectrum is returned as peak amplitudes in dB (dBV for a voltage), up to 4096 bins.

Distortion components below about −100 dBc are limited by the transient's fixed-step backward-Euler integration and
the linear resampling; use a step of 1/500 of the fundamental period or finer.

## Imported SPICE models

Any diode, LED, NPN, N-MOSFET, op-amp, 8-pin IC or catalog part (except the simulated microcontrollers) can take a
vendor SPICE model instead of its built-in model: **Properties → SPICE Model → Attach SPICE Model…** opens the model
editor. Paste the text or **Open File…** a `.lib` / `.mod` / `.cir` / `.sub`, or pick one from **Library** (generic
1N4148, 1N4007, 1N5819, BZX84C5V1, 2N3904, 2N3906, BC847B, 2N7002, BSS84 and the µA741 Boyle macromodel). Pick the
`.model` or `.subckt`, check the pin map and **Attach**. The editor re-checks the model on the part as you type and lists
every diagnostic with its line. Only the definition and what it uses (`.param`, `.func`, referenced `.model` and
`.subckt` blocks) is stored in the project (`Component::spice`, `"spice": {"text", "model", "pins"}` in the file); the
rest of the vendor file is not kept. Undo removes an attachment, the trash button removes it. Parts without a model are
simulated exactly as before (see *Verification*).

**What is read.** `.model` cards of type D, NPN, PNP, NMOS, PMOS (LEVEL 1–3), NJF, PJF, SW / CSW / VSWITCH / ISWITCH, with
`AKO:` inheritance and PSpice `DEV` / `LOT` tolerances skipped; `.subckt` with `PARAMS:` defaults and instance
overrides, nested and local definitions; `.param`, `.func`; continuation lines (`+`), comments (`*`, `;`, ` $`),
`{expressions}` and `'expressions'` with SPICE scale factors (`MEG`, `MIL`, `k`, `u`, …, case-insensitive); elements
R C L K V I E F G H B D Q M J S W X. Controlled sources accept the linear form, `POLY(n)` (SPICE coefficient order),
`VALUE = {…}` and `TABLE {…} = (x, y) …`; B sources take `V=` or `I=` expressions of `V(a)`, `V(a,b)`, `I(Vx)` and
`time` with + − * / `**` `^` comparisons, `&& || !`, `?:` and abs sqrt exp ln log log10 sin cos tan asin acos atan
atan2 sinh cosh tanh min max pow pwr pwrs sgn sign u uramp limit if floor ceil round int table. Simulation commands
(`.tran`, `.options`, …) and test-circuit elements outside a `.subckt` are ignored with an info line; `.include` /
`.lib file` are reported (paste the included text). Anything the simulator cannot honour is an error with its line:
T / O / U / A / Z elements, LAPLACE / FREQ sources, `ddt` / `idt`, MOSFET LEVEL ≥ 4 (BSIM), BJT LEVEL > 1 (VBIC). Model
parameters it does not use are listed as a warning (or an info line for temperature coefficients, ratings and vendor
metadata — the simulation is at 27 °C).

**Device equations.** Diode: Shockley with emission coefficient, series resistance, reverse breakdown (BV, IBV, NBV),
depletion charge (CJO, VJ, M, FC) and diffusion charge (TT). BJT: Gummel–Poon level 1 — IS, BF, BR, NF, NR, Early
voltages VAF / VAR, high injection IKF / IKR, leakage ISE / NE, ISC / NC, RB / RC / RE, junction charges CJE / VJE /
MJE, CJC / VJC / MJC and transit times TF / TR (XTF, RBM, CJS ignored). MOSFET: Shichman–Hodges level 1 with body effect
(GAMMA, PHI), λ, RD / RS, KP or UO·Cox, LD, overlap capacitances CGSO / CGDO / CGBO, bulk junctions (IS, CBD / CBS or
CJ·AD / AS + CJSW·PD / PS, PB, MJ, MJSW) and, when TOX is given, an intrinsic gate charge of ⅔·Cox·W·L; LEVEL 3 adds
THETA mobility reduction (ETA, VMAX, KAPPA ignored). JFET: Shichman–Hodges with gate diodes and CGS / CGD. Switches are
smooth log-interpolated conductances between ROFF and RON across the threshold (no hysteresis). Model temperature is
27 °C (V_T = 25.865 mV); built-in models keep their 300 K value.

**Pin map.** One entry per model port, in port order: a pin name or number of the part, `0` (ground), `net:NAME` (a
schematic net), `dc:15` (an ideal supply to ground) or `nc` (left open); `;` separates instances (`3 2 8 4 1; 5 6 8 4 7`
puts a single-op-amp model into both halves of an LM358). Empty means the default: model terminals map to pins of the
same name (`A K`, `C B E`, `D G S`, MOSFET bulk to `B` / `BULK` or the source), subcircuit ports to pins of the same
name, a five-port op-amp macromodel on the op-amp symbol to IN+ IN− V+ V− OUT with ±15 V rails, otherwise ports in pin
order when the counts agree. A pin that is not connected floats.

**Readings.** The part's DC reading and transient current trace are taken at its pins: the collector / drain current
and V_CE / V_DS for transistors (pins named C / E or D / S on catalog parts), the output voltage and delivered current
for op-amps, otherwise the current into the first pin; power is the total the model absorbs, rails included.

**Robustness.** The parser never throws. It refuses text over 8 MB or 200 000 lines, cards over 1 MB, expressions
nested deeper than 200, subcircuits nested deeper than 40 (recursive definitions) and models that flatten to more than
100 000 elements; every refusal is a diagnostic. With an imported model in the circuit the Newton iteration also
measures its step tolerance from the damped step (a stricter test than the built-in models keep for compatibility).

C API: `sieda_spice_parse`, `sieda_spice_check`, `sieda_set_spice_model`, `sieda_component_spice_model`,
`sieda_spice_builtin_models` (see `sieda_c.h`).

## C API

All five functions take a JSON options object (a `NULL` or empty string means defaults) and return JSON
`{"ok": bool, "error": "…", …}`; free the result with `sieda_string_free`. Numbers may be given as engineering
strings (`"10k"`, `"1MEG"`, `"50m"`). Nets are named as in the schematic (or given by index), parts by reference.
The exact fields are documented at each declaration in `Core/include/sieda/sieda_c.h`. Invalid options give
`"ok": false` with a message; they never throw across the C boundary. Readouts that do not exist in the sweep are
`null`.

```json
// sieda_simulate_ac(project, "{\"start\":10,\"stop\":\"1MEG\",\"pointsPerDecade\":50,\"source\":\"V1\"}")
{"ok":true,"error":"","stimulus":["V1"],"frequency":[10, …],
 "nets":[{"index":2,"name":"OUT","dc":0,"magnitudeDb":[…],"phaseDeg":[…],
          "metrics":{"lowFreqDb":-0.0002,"peakDb":…,"peakHz":…,"f3dbHz":1591.6,"bwLowHz":null,"bwHighHz":null,
                     "unityHz":null,"phaseMarginDeg":null}}]}
```

## Verification

The core tests (`Core/tests/core_tests.cpp`) check the numerics against closed-form results:

- RC low-pass: every sweep point equals 1 / (1 + jωRC) to 10⁻⁸; the −3 dB point to 10⁻⁷ of the analytic value
  (within 0.5 % of 1/(2πRC) even interpolated at 20 points per decade); phase → −90°.
- Series RLC: resonance at 1/(2π√LC) to 10⁻⁵, bandwidth R/(2πL) to 10⁻⁴; the capacitor node's peak at
  f₀√(1 − 1/2Q²) with Q/√(1 − 1/4Q²).
- Op-amp: non-inverting gain 2 reads 6.02 dB with its closed-loop pole at GBW·β; open loop crosses unity at GBW with
  90° phase margin; `GBW=` overrides the part table.
- Diode r_d = n·V_T / I and common-emitter gain −β·R_C from the linearised junction models.
- DC sweep against separate operating points; parameter sweep of an RC corner over R; divider worst case
  (4.95 V / 5.05 V exactly), Monte Carlo σ against the analytic value, seed reproducibility; RC corner worst case
  fc / (1.01·1.1) … fc / (0.99·0.9).
- THD of a synthetic 10 % third harmonic, of a square wave (√(1/9 + 1/25 + 1/49 + 1/81)) and of a clean sine.
