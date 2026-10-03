# SiEDA — AI-native Electronic Design Automation for macOS

SiEDA is a professional EDA suite: a **SwiftUI** macOS app on top of a **C++17** engine.
You describe a product in plain language or paste a PRD. A team of AI agents (Claude by default, or any
model you choose) then turns it into a verified schematic, a simulated circuit, an autorouted two-layer
PCB, a 3D assembly and fabrication-ready Gerber files.

The interface combines conventions from three tools:

- **Altium Designer:** docked Properties and Projects panels, coloured layer tabs, a Messages-style check panel with cross-probing.
- **Photoshop:** a vertical tool strip, a tool-options bar and a layers panel with visibility toggles.
- **Proteus:** a device picker with symbol preview, simulation transport controls and live voltage probes on the schematic.

The app defaults to dark mode with a blue palette.

## Use it with or without AI

AI is optional. A single switch (**Settings → AI Models → Enable AI assistance**, the toolbar AI menu, or
**Design → AI Assistance**, ⌥⌘A) changes how SiEDA works:

| | AI assistance **on** | AI assistance **off** (manual mode) |
|---|---|---|
| Start | Super Intelligence: describe the product, agents design it | Schematic editor; **File → New from Example** for reference designs |
| Schematic, simulation, PCB, autorouting, DRC, 3D, exports | ✓ | ✓ (all run locally in the C++ core) |
| Datasheet import | AI reads the PDF or image | Offline pin-table parser plus OCR; review the result in the pin editor |
| Network traffic | Only to the AI provider you select | None |

Every AI result lands in the same editable schematic and PCB, so you can switch modes at any time.

## Features

| Area | What it does |
|---|---|
| **Super Intelligence** | Prompt/PRD editor, template briefs, PRD import, live agent pipeline, conversational refinement ("make the LED green and run it from 3.3 V") |
| **Agent team** | Requirements Analyst → Circuit Architect → Plan Compiler → Verification (ERC + SPICE) → Design Reviewer (feedback loop) → PCB Layout |
| **Standard components** | Built-in standard library (LM7805, LM317, NE555, LM358, ATtiny85, ATmega328P, 74HC595, 74HC00, ULN2003A, L293D, pin headers) with correct pinouts, pin types and footprints, in the device picker and Library. Plus 40 popular microcontrollers, see [Microcontroller library](#microcontroller-library). AI agents can use them too. IEC 60063 E12/E24/E96 value checks with a one-click "use nearest standard value" in the Properties panel |
| **Basic circuit library** | 13 verified basic reference designs in 7 groups (**File → New from Example**): LED indicator, voltage divider, 5 V LM7805 regulator, half-wave rectifier, non-inverting/inverting amplifier, voltage follower, RC low-/high-pass filters, NPN and MOSFET drivers, 555 astable blinker, Wheatstone bridge. Every one passes full verification on 1, 2, 4 and 6 layers |
| **Circuit validation** | Beyond ERC: non-standard values, missing IC decoupling, and DC-derived part ratings (resistor power, LED/diode current, reverse-biased LEDs, transistor current/power, supply over-current, op-amp saturation, fuse overload). Exceeding a rating is a warning; 2× the rating is an error |
| **PCB design rules** | Presets: Prototype (Conservative), IPC-2221 Class 2 (default), IPC-2221 Class 3, Fab House Standard (6/6 mil) and Advanced (4/4 mil). DRC checks clearance against both the design rule and the fab minimum, plus track width, drill size, annular ring, hole-to-hole spacing, via-in-pad, dangling tracks, acute angles and IPC-2221 current capacity from the simulated net currents |
| **Industry design kits** | Each project has an industry profile: General, Robotics & Motor Control, Drones & UAV, Power Electronics, Automotive, RF & Wireless, Space, Marine & Ship, Industrial Automation, Medical Devices, Defence & Military, Networking & Telecom, or VLSI / ASIC & FPGA. The profile sets the design rules, derates part ratings in validation (Space 50 %, Automotive 60/70 %, …) and selects IPC-2221 B2 or B3 (altitude) voltage spacing, and its standards and design guidance are shown in the Properties panel. The kits add domain parts (IR2104, IRF540N, UC3843, ACS712, TJA1050, LM2940, MAX485, PC817, INA333, ADuM1201) and 13 verified reference designs, and AI agents choose and follow the profile. See [Industry design kits](#industry-design-kits) |
| **Live board** | **Run** (▶, ⌘R) runs the board in real time: LEDs glow with their current, you click switches and push-buttons, probes, a scope and the serial monitor update as firmware runs. See [Live board simulation](#live-board-simulation) |
| **Microcontroller firmware** | Upload an Arduino/avr-gcc `.hex` to an ATmega328P or ATtiny85 and run it in the transient simulation. Pins drive the circuit, inputs and the ADC read it, and the serial monitor shows its output. See [Microcontroller simulation](#microcontroller-simulation-firmware) |
| **Verification process** | **Design → Verify Design** (⌥⌘V) runs a 7-stage sign-off: ERC → DC simulation → circuit validation → footprint placement → routing completion → DRC/fab rules → manufacturing outputs (every Gerber layer, drill, BOM, pick-and-place and netlist are generated and checked). The verdict is Pass, Pass with warnings or Fail; findings cross-probe to the editors, and the report exports as Markdown. **Export Fabrication Package** always verifies first, asks before exporting a failing design, and includes `verification_report.md` |
| **Model choice** | Anthropic **Claude** (default: `claude-opus-5-5`, structured outputs, adaptive thinking, effort control, refusal fallbacks), OpenAI or any OpenAI-compatible endpoint, Google Gemini, local Ollama, and an Offline Designer that needs no network |
| **Schematic capture** | 16 built-in device types plus your own library parts, orthogonal wiring with T-junctions (end a wire on any wire to join it, e.g. a part in parallel), corners (wire tool: click empty space), bendable wires (drag a wire or its bend points; deleting a bend straightens it), net labels, rotate/move/marquee, undo/redo, ERC with pin-type rules, no-connect flags (Q) for pins left open on purpose |
| **Datasheet → component** | Drop a datasheet (PDF, pinout screenshot or text). The Datasheet Analyst agent extracts part number, package and every pin with its electrical type: Claude and Gemini read the PDF natively, OCR (Vision) handles images, and an offline pin-table parser works without a key. You review it in a pin-table editor with live symbol and footprint previews, then save it to the project library and place it like any other part. AI design agents can use library parts too |
| **Simulation** | Modified Nodal Analysis with Newton–Raphson: DC operating point and transient. Diode/LED (Shockley), BJT (Ebers–Moll), MOSFET (square-law with λ), saturating op-amp, R/L/C, DC/SIN/PULSE sources, batteries and AC (mains / transformer) sources. Part-number device models (1N4148, SS14, BC847, 2N7002, AO3400, SI2302, IRF540N, …) and behavioural chip models: regulators and chargers (LM7805, LM317, XC6206, AP2112K, TP4056 with constant-current charging) and IC supply current (ATmega328P, MPU-6050, nRF24L01+, …). Switching designs are checked for RMS/peak stress in steady state. Waveforms are drawn with Swift Charts |
| **PCB layout** | Footprint library (0805, SOD-123, SOT-23, SOIC, TSSOP, DIP, QFN with exposed pad, LQFP, 1×N and 2×N headers, TO-220), connectivity-driven auto-placement with escape space around fine-pitch parts, board fit, ratsnest, geometric and manufacturability DRC (see design rules). **Board Setup** (PCB options bar): board outlines (rectangle, rounded, circle, quadcopter X frame), M2/M3 mounting-hole patterns with keep-outs, copper pours and reserved plane layers (clearance-aware fill, thermal reliefs, island removal), and net classes with IPC-2221 auto-sizing from the simulated currents |
| **Autorouting** | A final clean-up pass merges collinear pieces and chamfers right-angle corners to 45° wherever clearance allows, so tracks look hand-routed. In the PCB view, copper is coloured **By Net** (power red, ground blue, negative rails purple, signals yellow on top, green on the bottom, and other colours on inner layers) or **By Layer** (the CAD convention: top red, bottom blue, inner yellow / green / orange / magenta), with a legend in the Layers panel. Part designators show at normal zoom, values inside parts and net names along tracks as you zoom in, and pad numbers close up. One **Auto Route** button (PCB options bar, or ⇧⌘R): places any footprints not on the board yet inside the board shape (outline, mounting holes, edge clearance), then routes every connection cleanly and runs DRC, as one undo step. A banner offers it whenever connections are still unrouted. Single-layer (single-sided, no vias), 2-, 4- and 6-layer A* autorouter: through vias, alternating layer directions, turn penalties, rip-up and retry passes, escape routing and neck-down for 0.5 mm-pitch QFN pins, fan-out vias to ground pours/planes, exact clearance for wide power tracks |
| **3D** | **Assembly** (default): a realistic board in your solder mask colour: green like most boards, or black, blue, red, yellow, white or purple (saved with the project; black legend on yellow and white). The board follows the Layers setting in PCB Layout: single-sided boards have a bare FR-4 underside, 2-layer boards have solder mask on both sides, and 4/6-layer boards show their inner copper on the board edge. **X-Ray Stack:** a holographic exploded view of every copper layer with an adjustable gap. Copper glows with additive blending and HDR bloom, through vias become light pillars, part bodies are wireframes, and a scan beam sweeps the stack; turntable spin and per-layer toggles. Both export to STL/OBJ |
| **Design for reliability** | Design Checks and the verification report (new **Design for Reliability** stage) explain each failure mechanism and its fix. **Leakage:** IPC-2221B Table 6-1 spacing (all columns B1–A7; A5 when the board is conformal coated, set in Board Setup → Protection & Reliability: acrylic, silicone, urethane, epoxy, parylene per IPC-CC-830). High-impedance inputs are found automatically and get a guard-ring recommendation (driven by the other amplifier input, or by the output for a follower), 0.5 mm leakage spacing (0.25 mm coated) and air-wiring advice for pA work, plus moisture baking (IPC-1601). **Thermal:** hot spots from the DC operating point and package θJA against 150 °C junctions and the laminate's Tg; thermal reliefs on two-pad chips in pours. **CTE:** via aspect ratio. **Signal integrity / EMI:** 3W crosstalk, missing ground reference plane, RF 50 Ω microstrip width (IPC-2141) checked only when the line is longer than λ/10, plus RF vias and width steps. **Assembly:** tombstoning, solder bridges and acid traps. **Motor control:** missing flyback diodes, half-bridge shoot-through without a gate driver, RC snubbers, ground bounce. **Industry:** automotive load dump (ISO 16750-2 / ISO 7637-2 TVS) and thermal cycling; vibration staking; space tin whiskers (SnPb), outgassing (polyimide, ASTM E595) and radiation; flex fatigue (IPC-2223). **Auto Route** follows the same rules: high-impedance and fast nets route first with leakage / 3W spacing, and RF nets get their 50 Ω width. **Fab notes and job file** carry the profile's material, finish, solder alloy, coating and IPC class |
| **BOM** | The **BOM** workspace lists parts grouped into order lines (type, value, footprint), with designators in natural order (R2 before R10), descriptions ("Resistor 10kΩ ±1% 0805") and ratings (resistor power from the package, capacitor voltage at twice the highest supply). Edit the manufacturer, manufacturer part number, supplier / LCSC number and unit price, or mark a line DNP (do not populate). **Fill Suggested Part Numbers** adds standard ones (Yageo RC-series resistors, semiconductor part numbers). It shows cost per board and per order (boards per order is saved with the project), flags lines without a part number or price, and a designator click shows the parts in the schematic. Everything is saved with the project and goes into `bom.csv`, the assembly-house BOM (LCSC and MPN filled, DNP left out), the CPL and the fab notes |
| **Manufacturing** | **File → Export Fabrication Package** (⇧⌘E) verifies the design, then writes everything a board house and an assembly house need. **gerbers/**: Gerber X2 (RS-274X) copper for every layer including inner layers and pours, solder mask, solder paste (stencil), silkscreen with reference designators and pin-1 marks (top and bottom), board outline, plated and non-plated Excellon drills, a Gerber X2 job file (stack-up, finish, mask and silkscreen colours) and an IPC-D-356A netlist for the electrical test. **assembly/**: BOM, assembly-house BOM and CPL (JLCPCB / PCBWay columns), pick-and-place, and assembly drawings (SVG) for each populated side. **fab_notes.txt**: the order sheet (size, layers, thickness, copper weight, recommended finish (ENIG for fine pitch, else HASL lead-free), mask and silkscreen colours, IPC class, minimum track, clearance, drill and annular ring, hole and part counts). Also **\<name\>-gerbers.zip** (ready to upload to the fab), the SPICE netlist, a 3D STL, the project and `verification_report.md`. The same package comes from `sieda-cli`, and every file is also in **File → Export** on its own |

## Industry design kits

Pick the industry in **Properties → Industry Profile** or **Design → Industry Profile**. AI-generated designs and the examples set it automatically.

| Profile | Standards it follows | Design rules | Derating (power / current) | Reference design |
|---|---|---|---|---|
| General Electronics | IPC-2221, IPC-A-610 Class 2 | IPC-2221 Class 2 | none | 13 basic circuits |
| Robotics & Motor Control | IEC 61800-5-1, IEC 60034 | IPC-2221 Class 3 | 80 % / 80 % | DC motor PWM drive with flyback diode and current shunt |
| Drones & UAV | IPC-2221/IPC-A-610 Class 3, RTCA DO-160G, ASTM F3002, EN 4709-001 | IPC-2221 Class 3 | 80 % / 80 % | Quadcopter flight controller on a 100 mm quad-X frame: 1S LiPo + TP4056 charger, 3.3 V LDO, ATmega328P, MPU-6050 IMU, nRF24L01+ radio, four brushed-motor drivers, ground pours, M3 30.5 mm mounting |
| Power Electronics | IEC 62368-1, IEC 61204, IPC-2152 | IPC-2221 Class 3 (High Voltage above 50 V) | 70 % / 80 % | Boost converter 5 V → 12 V; IR2104 half-bridge gate driver |
| Automotive | AEC-Q100/Q101/Q200, ISO 16750-2, ISO 7637-2, ISO 11898 | Automotive (IPC-6012 Class 3/A) | 60 % / 70 % | 12 V battery input protection + LM2940 + TJA1050 CAN node |
| RF & Wireless | IPC-2141, ETSI EN 300 220 / FCC Part 15 | RF (Controlled Impedance) | 80 % / 80 % | 433 MHz 50 Ω LC harmonic filter |
| Space | ECSS-Q-ST-30-11C, ECSS-Q-ST-70-12C, NASA EEE-INST-002 | Space (IPC-6012 Class 3/A, ECSS) + IPC-2221 B3 spacing | 50 % / 50 % | Redundant 28 V bus OR-ing + LM317 5 V |
| Marine & Ship | IEC 60945, IEC 61162 (NMEA), DNV-CG-0339 | IPC-2221 Class 3 | 70 % / 75 % | MAX485 RS-485 / NMEA interface |
| Industrial Automation | IEC 61131-2, IEC 61000-6-2/4, IEC 60664-1 | IPC-2221 Class 3 | 75 % / 80 % | 24 V PLC opto-isolated digital input |
| Medical Devices | IEC 60601-1 (MOOP/MOPP), IEC 60601-1-2, ISO 14971, IEC 62304, ISO 13485 | Medical (IEC 60601-1, IPC Class 3) | 60 % / 70 % | ECG biopotential front end: protected electrodes, INA333 (G = 101), mid-supply bias, anti-alias filter |
| Defence & Military | MIL-STD-810H, MIL-STD-461G, MIL-STD-704F/1275E, MIL-PRF-31032, MIL-HDBK-1547 | Defence (IPC-6012 Class 3/A, MIL) + IPC-2221 B3 spacing | 50 % / 60 % | 28 V MIL-STD-704 input: reverse + TVS protection, LC EMI filter, LM317 5 V |
| Networking & Telecom | IEEE 802.3 (Ethernet, PoE af/at/bt), IEC 62368-1, ITU-T K.21, IPC-2141 | High-Speed Digital (100 Ω diff) | 80 % / 80 % | 802.3af PoE powered device: polarity bridge, 58 V TVS, 24.9 kΩ signature, 12 V rail |
| VLSI / ASIC & FPGA | IPC-2226 (HDI), IPC-7351, JEDEC JESD8, IEEE 1149.1 | HDI / Fine-Pitch BGA (IPC-2226) | 80 % / 80 % | FPGA/ASIC bring-up: 1.25 V core, 1.8 V aux, 3.3 V I/O rails, decoupling, JTAG, 4 layers with a ground plane |

- **High-voltage clearance:** DRC computes each net's DC voltage, widened to the peaks of SIN/PULSE sources. Copper of nets with a large potential difference must keep the IPC-2221 Table 6-1 spacing: B2 at sea level, B3 above 3050 m. For example, 230 V needs 1.25 mm, or 6.4 mm at altitude. The autorouter routes with that spacing when the board's largest potential difference needs more than the design-rule clearance (above 30 V, such as 48 V PoE).
- **Not a certification:** profiles apply common derating and spacing guidance, not a compliance sign-off. Full-wave EM, thermal and radiation analysis are outside SiEDA's scope.

## Microcontroller simulation (firmware)

Microcontrollers run real firmware inside the circuit simulation, like on a physical board.

- **Supported chips:** ATmega328P (Arduino Uno/Nano, 16 MHz by default) and ATtiny85 (8 MHz).
- **Upload:** select the chip and use **Inspector → Firmware → Upload .hex…**. In the Arduino IDE, **Sketch ▸ Export Compiled Binary** produces the `.hex`; with avr-gcc, use `avr-objcopy -O ihex`. You can also pick a built-in example (Arduino Blink, AnalogReadSerial + Fade, UART, timer interrupt, ADC → PWM, button interrupt, CPU self-test, ATtiny85 blink). The firmware is saved with the project.
- **Run:** start a **Transient** analysis; the timer menu has firmware presets such as 1 s at 100 µs. Each analog step runs the chip for that step's clock cycles. Then:
  - Pins drive the circuit: 25 Ω push-pull outputs, 35 kΩ pull-ups. A PWM pin averages its duty over a step longer than the PWM period.
  - Inputs, interrupts and the 10-bit ADC read the solved node voltages.
  - Everything the USART transmits appears in the **serial monitor** under the waveforms.
  - Below 1.8 V supply the chip is held in reset.
- **What's emulated:** the full AVR instruction set with cycle counts; GPIO; Timer0/1/2 (normal, CTC, fast and phase-correct PWM); USART0 with the real bit waveform on TXD; the ADC; EEPROM; INT0/INT1 and pin-change interrupts; and sleep. The CPU is checked instruction-for-instruction against simavr, and the Arduino core's `millis()`, `delay()`, `Serial` and `analogRead()`/`analogWrite()` behave as on a board.
- **Example design:** **New from Example ▸ Microcontrollers ▸ Arduino Uno Core: LED + Button**.
- **Test firmware:** sources and the build script are in `Core/tests/firmware`.

## Signing in to AI models

Each cloud provider has a **Sign In…** button in **Settings → AI Models**. SiEDA opens the provider's own login page in your default browser. You sign in there, and SiEDA keeps the session token in the macOS Keychain (or leaves it with the vendor's CLI) and sends it with every request. Each provider uses its own documented login:

| Provider | Sign-in | What happens |
|---|---|---|
| Anthropic Claude | **Claude Console** | The official Anthropic CLI (`ant auth login`) opens the Claude Console login, where you pick the organisation and workspace. SiEDA asks the CLI for short-lived access tokens (`ant auth print-credentials`) and sends them as bearer tokens. Usage is billed to that workspace. Claude.ai Pro/Max chat subscriptions cannot be used by other apps. |
| Google Gemini | **Google (Vertex AI)** | The official Google Cloud CLI (`gcloud auth application-default login`) signs you in with your Google account. SiEDA calls Gemini on Vertex AI in your project and region. |
| OpenRouter | **OpenRouter** | OAuth with PKCE in the browser. OpenRouter issues SiEDA a key (kept in the Keychain), giving one login for Claude, GPT, Gemini and other models. |
| OpenAI / compatible | **Organisation SSO** | OpenID Connect + PKCE with your company identity provider (Okta, Microsoft Entra ID, Google Workspace, Keycloak…) for an OpenAI-compatible AI gateway that accepts its access tokens. Tokens are refreshed automatically. OpenAI itself has no third-party sign-in, so the OpenAI API uses a key. |

Every provider still accepts an API key. Browser redirects are received on `127.0.0.1`, local connections only. The Claude Console and Google sign-ins start the vendors' command-line tools, which the macOS App Sandbox does not allow. SiEDA therefore runs without the App Sandbox, like other developer tools distributed outside the Mac App Store. A sandboxed build disables those two sign-ins and explains why.

## Microcontroller library

Ten popular microcontrollers from each vendor group are in the standard library. Each is on its real package: lead count, pitch, body size and exposed pad. Pin numbers, names and electrical types are taken from the KiCad symbol library, which follows the vendor datasheets (`tools/fetch_mcu_pinouts.py` regenerates `Core/src/StandardMcus.inc`). In the device picker they are grouped under **Microcontrollers · <vendor>**.

| Group | Parts (package) |
|---|---|
| Arm (other vendors) | RP2040 (QFN-56 0.4), RP2350A (QFN-60 0.4), nRF52832 / nRF52810 / nRF51822 (QFN-48 0.4), LPC1768 (LQFP-100), LPC1114FBD48 (LQFP-48), MKL25Z128VLK4 (LQFP-80), CY8C4245AXI PSoC 4 (TQFP-64 0.8), EFM32HG308F64 (QFN-24 0.65) |
| Microchip | ATmega2560 (TQFP-100), ATmega32U4 (TQFP-44 0.8), ATmega4809 (TQFP-48), ATtiny1614 (SOIC-14), ATSAMD21G18A (TQFP-48), ATSAMD21E18A (TQFP-32 0.8), PIC16F877A / PIC18F4550 (DIP-40), PIC12F675 (DIP-8), PIC32MX250F128D (TQFP-44 0.8); plus ATmega328P and ATtiny85 |
| STMicroelectronics | STM32F103C8T6 (LQFP-48), STM32F030F4P6 / STM32G030F6P6 (TSSOP-20), STM32F042K6T6 (LQFP-32 0.8), STM32F401CCU6 / STM32F411CEU6 / STM32G431CBU6 (UFQFPN-48), STM32F407VGT6 / STM32H743VIT6 (LQFP-100), STM32L432KCU6 (UFQFPN-32) |
| Texas Instruments | MSP430G2553 / MSP430G2452 (DIP-20), MSP430F2013 (DIP-14), MSP430F2274 (TSSOP-38), MSP430FR5738 (VQFN-24), MSP430F5510 (VQFN-48), TM4C1231H6PM (LQFP-64), MSP432E401Y (LQFP-128 0.4), CC1312R1 (VQFN-48), CC430F5137 (VQFN-48) |

Pitches are 0.5 mm unless noted. Firmware can only be run in the simulator on the ATmega328P and ATtiny85. The other parts are for schematic and PCB design.

## Live board simulation

**Run** (▶ on the schematic's transport, ⌘R, or **Run Live** in Simulation) runs the circuit in real time, like a powered board on the bench. **DC** next to it solves the operating point once.

- **Firmware runs:** microcontrollers execute their firmware continuously.
- **LEDs glow:** in their own colour, with brightness from the simulated current. A dark LED keeps a faint tint of its colour, and a DC result lights LEDs too.
- **Live probes:** wires show voltages that update continuously.
- **Switches and buttons:** click them on the schematic while the board runs. A switch whose value contains "push", "button" or "tact" is a momentary push-button: it is closed while you hold the mouse. Other switches toggle. When the board isn't running, clicking a switch's lever flips its value (on/off, undoable) and re-solves a DC result on screen.
- **Instruments:** **Simulation ▸ Live board** has:
  - The speed (0.01× slow motion to 10×) and the analog step.
  - Hold/toggle controls for every switch, plus LED currents.
  - Probes you can add to a rolling **scope** (20 ms – 10 s window).
  - A **serial monitor** per microcontroller, with an input line that feeds its USART receiver.
- **Pause, resume and edit while running.** A circuit edit (value, wiring, parts, firmware) restarts the board with the change straight away; moving or renaming parts keeps it running.
- **Delete / Backspace** removes the selected parts or wire wherever the keyboard focus is, except while you're typing in a text field.
- **Real-time factor:** shown in the header. Heavy circuits or fine steps can run slower than real time.

## Repository layout

```
Core/                C++17 engine (no dependencies)
  include/sieda/     public headers; sieda_c.h is the stable C ABI used by Swift
  src/               schematic, simulator, PCB/autorouter/DRC, mesh, exports, JSON
  tests/             unit tests (C++ and a C-compiled ABI smoke test)
  cli/               sieda-cli: headless ERC → simulation → place & route → DRC → verification → fabrication files
SiEDA/               macOS SwiftUI app
  App/               app entry point, menus, DesignStore (state, undo, documents)
  Bridge/            bridging header + EDAEngine (thread-safe Swift façade over the C ABI)
  Models/            snapshot models, ComponentKind, DesignPlan (the agents' structured output)
  AI/                providers (Claude, OpenAI, Gemini, Ollama, Offline), prompts, orchestrator, settings
  Views/             Super Intelligence (PromptStudio/), Schematic, PCB, 3D, Simulation, Checks, Inspector, Settings
SiEDATests/          XCTest suite (bridge, plan compiler, offline templates)
SiEDA.xcodeproj      generated by tools/generate_xcodeproj.py
docs/                PRD and architecture
```

## Getting started

### Quick start: `run.sh`

One script builds, tests and launches everything from a terminal:

```bash
git clone https://github.com/srinivasan-fh/si-eda.git
cd si-eda
./run.sh            # macOS: builds SiEDA.app and launches it · Linux: builds the core and runs the demo
```

| Command | What it does |
|---|---|
| `./run.sh app` | Build the macOS app (Debug, into `build-app/`) and launch it |
| `./run.sh xcode` | Open the project in Xcode (then press ⌘R) |
| `./run.sh test` | Core unit tests, plus the Xcode tests on macOS |
| `./run.sh core` | Build the C++ core, `sieda-cli` and the unit tests (any platform) |
| `./run.sh demo [dir]` | Headless pipeline on the demo design; fabrication files and `verification_report.md` go to `dir` (default `out/`) |
| `./run.sh cli design.siedaproj [dir]` | Verify a saved project and write its complete fabrication package (Gerbers, drills, job file, IPC netlist, assembly files, fab notes and the Gerber zip) |
| `./run.sh doctor` | Show which required tools are installed |
| `./run.sh clean` | Remove `build/`, `build-app/` and `out/` |

Requirements: the app needs macOS 14+ with Xcode 16+ (`sudo xcode-select -s /Applications/Xcode.app` if
`xcodebuild` only finds the Command Line Tools). The core needs CMake 3.20+ and a C++17 compiler
(`brew install cmake` · `sudo apt install cmake g++`). Python 3 keeps the Xcode project in sync.

### macOS app

Requirements: macOS 14 Sonoma or later and Xcode 16 or later.

```bash
open SiEDA.xcodeproj        # then Run (⌘R)
```

1. Open **Settings → AI Models**, pick a provider and **Sign In** (or paste an API key). See
   [Signing in to AI models](#signing-in-to-ai-models). You can switch between Claude, OpenAI, Gemini,
   OpenRouter, Ollama and the Offline Designer at any time from the same screen or the toolbar menu.
2. In **Super Intelligence**, describe the product or pick a template, then press **Generate Design** (⌘↩).
3. Inspect the result in **Schematic** (⌘2), **PCB Layout** (⌘3), **3D Viewer** (⌘4) and **Simulation** (⌘5).
   Import datasheets in **Library** (⌘7). Choose 1, 2, 4 or 6 copper layers in the PCB options bar.
4. Choose **Design → Verify Design** (⌥⌘V) and fix anything the report flags, then **File → Export Fabrication Package…** (⇧⌘E).

Command-line build and test:

```bash
xcodebuild -project SiEDA.xcodeproj -scheme SiEDA -destination 'platform=macOS' test
```

### C++ core (any platform)

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/sieda-cli --demo out/     # full pipeline on a demo design, writes Gerbers/drill/BOM/STL and
                                  # verification_report.md to out/; exits non-zero if verification fails
```

### Regenerating the Xcode project

After adding or removing source files, run:

```bash
python3 tools/generate_xcodeproj.py
```

## Canvas navigation and keyboard shortcuts

The schematic and PCB canvases are built for large designs such as CPU/GPU/NPU motherboards, MCU development boards and daughterboards:

- **Zoom range:** fits a 600 mm backplane in a small window and goes down to 0.4 mm-pitch BGA detail on the PCB (1/50 to 15× on the schematic).
- **Rendering:** only what is on screen is drawn. Pins, labels and designators fade out when zoomed far out, and the grid pitch adapts to the zoom level.
- **Navigator:** a thumbnail of the whole design (toggle with N, the toolbar map button or **View → Show Navigator**). Click or drag in it to move the view.

| Mouse / trackpad | Action |
|---|---|
| Two-finger scroll | Pan |
| Pinch, mouse wheel, or ⌘/⌥/⌃ + scroll | Zoom at the cursor |
| ⇧ + mouse wheel | Pan horizontally |
| Middle- or right-button drag | Pan (any tool) |
| Space + left-button drag | Pan (any tool) |
| Drag on empty canvas | Pan (select tool) |
| ⇧-drag | Marquee select (schematic) |

| Key | Action |
|---|---|
| Arrow keys (⇧ for half a screen) | Pan |
| + / − (or ⌘= / ⌘−) | Zoom in / out at the cursor |
| Home, 0 or ⌘0 | Zoom to fit (home view) |
| ⇧Z or ⌥⌘0 | Zoom to selection |
| Z | Zoom to area: drag a rectangle (a click zooms 2×) |
| N | Show/hide the navigator |
| **View → Zoom Level** / click the zoom percentage | Preset levels from 10 % to 1600 % |
| V / H / W | Select, pan (hand) and wire tools (schematic) |
| Q | No-connect tool: click a pin to mark it intentionally open (schematic) |
| G / L | Place ground or net label |
| Space (tap) or R | Rotate the selected component(s) 90° (schematic and PCB; nothing happens if nothing is selected). While placing a part, rotate it before clicking. Holding Space and dragging pans instead |
| F | Flip a footprint to the other side (PCB) |
| ⌫ / Esc | Delete selection / cancel |
| ⇧⌘K / ⇧⌘D / ⇧⌘R | Run ERC, DC operating point, Auto Route |
| ⇧⌘L / ⌥⌘V | Validate circuit, verify design |
| ⌘1 … ⌘7 | Switch workspace (⌘1 … ⌘6 when AI assistance is off) |
| ⌥⌘A | Turn AI assistance on/off |

## License

Proprietary. All rights reserved.
