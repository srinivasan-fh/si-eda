# SiEDA — AI-native Electronic Design Automation for macOS

SiEDA is a professional EDA suite: a **SwiftUI** macOS app on top of a **C++17** engine.
You describe a product in plain language or paste a PRD. A team of AI agents (Claude by default, or any
model you choose) then turns it into a verified schematic, a simulated circuit, an autorouted two-layer
PCB, a 3D assembly and fabrication-ready Gerber files.

The interface combines conventions from three tools:

- **Altium Designer:** docked Properties and Projects panels, coloured layer tabs, a Messages-style check panel with cross-probing.
- **Photoshop:** a vertical tool strip, a tool-options bar and a layers panel with visibility toggles.
- **Proteus:** a device picker with symbol preview, simulation transport controls and live voltage probes on the schematic.

The app defaults to dark mode with a blue palette. At launch, a five-second splash screen preloads the design engine, the component library, the industry profiles, the memory & system design kits and the reference designs, then opens the main window. Once loading is done, a click or Esc skips the rest. **Settings → Appearance → Show splash screen at launch** turns it off (or launch with `-showSplashScreen NO`).

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
| **Industry design kits** | Each project has an industry profile: General, Robotics & Motor Control, Drones & UAV, Power Electronics, Automotive, RF & Wireless, Space, Marine & Ship, Industrial Automation, Medical Devices, Defence & Military, Networking & Telecom, VLSI / ASIC & FPGA, or one of the computing segments: Motherboards & PCs (Intel / AMD / ARM), Servers & Mainframes, Supercomputers & AI Accelerators, ARM SoC & Compute Modules, Daughterboards & Add-in Cards, Retail & POS, Home Appliances. The profile sets the design rules, derates part ratings in validation (Space 50 %, Automotive 60/70 %, …) and selects IPC-2221 B2 or B3 (altitude) voltage spacing, and its standards and design guidance are shown in the Properties panel. The kits add domain parts (IR2104, IRF540N, UC3843, ACS712, TJA1050, LM2940, MAX485, PC817, INA333, ADuM1201) and verified reference designs for every profile, and AI agents choose and follow the profile. See [Industry design kits](#industry-design-kits) |
| **Computing segments (Intel, AMD, ARM, NVIDIA, Microsoft, Google platforms)** | Five profiles cover microprocessor, mainframe and supercomputer boards. Each is built on the public standards those platforms use, not on vendor reference designs: vendor platform design guides still govern a real product. **Motherboards & PCs:** ATX 3.1 / ATX12VO, PCIe CEM 5/6, DDR5, USB4, Intel VR / AMD SVI3 VRMs; mid-loss FR-4, 6–10 layers. **Servers & Mainframes:** Xeon / EPYC / Neoverse, OCP DC-MHS, Open Rack v3 48 V, PMBus / Redfish BMC, N+1 mainframe RAS; IPC Class 3, Megtron 6/7, 12–16 layers. **Supercomputers & AI Accelerators:** GPU / accelerator baseboards (HGX-class, Instinct, TPU-style), OCP OAM / UBB, 48 V vertical power, 112G SerDes; Megtron 7 / Tachyon 100G, 16–26 layers, heavy copper. **ARM SoC & Compute Modules:** SMARC / COM-HPC / Qseven, LPDDR, MIPI, SoM + carrier. **Daughterboards & Add-in Cards:** PCIe CEM cards with hard-gold bevelled fingers, OCP NIC 3.0, M.2, risers and mezzanines. Each profile sets the material, finish, IPC class and controlled-impedance notes for fabrication. Each has a reference design that verifies clean, with every differential pair length-matched by Auto Route: CPU VRM phase + PCIe x1 link (8 layers, 85 Ω); redundant 48 V rack power + PMBus (12 layers); 48 V tray + four 112G SerDes pairs (12 layers, Megtron 7); RP2040 Cortex-M0+ module with a symmetric USB pair; PCIe x1 add-in card. Boards now go up to **24 layers** (even counts). New laminates: Megtron 7 and Isola Tachyon 100G. 4-layer boards use the standard 0.2 mm outer prepreg in impedance calculations. |
| **Robot system segments (Mars rover, FPV drone, industrial arm, quadruped, humanoid, 3D printer, CNC)** | Pick the robot platform in **Properties → Robot System Segments** (or **Design → Robot Platform**). Each platform comes with its own guidance and production parts kit ([Robotics parts kits](#robotics-parts-kits)). The panel checks the seven modular segments on the live design, each marked complete, partial or missing, and lists what is still needed. **1 Power distribution:** battery input, reverse-polarity protection (ideal-diode MOSFET), fuse / eFuse, TVS, motor / logic isolation, 2–4 oz copper for high-current rails. **2 Compute:** processor / SoM, debug access, length-matched DDR / PCIe / MIPI, HDI when a fine-pitch processor needs it. **3 Motion control:** gate drivers, MOSFET / GaN stages, Kelvin current-sense shunts, thermal vias (**Design → Add Thermal Vias**; Auto Route stitches them at SMD power FETs automatically), flyback / shoot-through. **4 Sensors:** IMU kept ≥ 10 mm from the power stage, AGND tied to GND at exactly one star point, CAN / RS-485 with 120 Ω termination (CANH / CANL and RS485_A / _B route as differential pairs). **5 Communication:** radio kept ≥ 10 mm from switching power with a shield can, coax at the board edge. **6 Safety & UI:** a hardwired ESTOP net reaching every gate-driver enable, status LEDs. **7 Mechanical:** mounting holes, MLCC strain relief, rigid-flex through joints on legged and humanoid robots. Each gap is a Design Check (REL_REVERSE_POLARITY, REL_NO_FUSE, REL_THERMAL_VIAS, REL_IMU_PLACEMENT, REL_STAR_GROUND, REL_BUS_TERMINATION, REL_RF_NOISE, REL_COAX_EDGE, REL_ESTOP, REL_MLCC_STRAIN, …). Auto Place keeps radios and IMUs away from switching parts and MLCCs out of mounting-hole flex zones. The **Robot Joint Controller** example implements all seven segments: 24 V input with ideal diode / fuse / TVS, ATmega328P, ADuM1201, IR2104 + IRF540N half-bridge with a 10 mΩ shunt amplifier, MPU-6050, TJA1050 CAN, nRF24L01+, fail-safe E-stop, M3 holes. It routes 142/142 and verifies clean. |
| **Automotive ECU segments (body, powertrain, ADAS / domain, EV, chassis, gateway)** | Pick the ECU type in **Properties → Automotive ECU Segments** (or **Design → ECU Type**); the Automotive industry profile turns the checks on too. The panel checks the six ECU design blocks on the live design. **1 Transient & power protection front-end:** battery input, reverse-battery protection (low-side MOSFET / P-MOSFET / LM74700-Q1 ideal diode), SM8S-class load-dump TVS (ISO 16750-2), fuse or TPS1213-Q1 eFuse, C–L–C pi filter / CM choke (CISPR 25). **2 Voltage regulation & management:** regulated rails, cold-crank headroom from a DC simulation (a battery-fed regulator that needs more than 6 V resets on every engine start; a buck-boost pre-regulator fixes it), a windowed watchdog / PMIC whose RESET drives the MCU and whose WDI the MCU kicks, a separate low-noise LDO for analog. **3 Safety MCU:** lockstep ASIL-D parts (AURIX TC3xx, Stellar, RH850, S32K3, TMS570), external EEPROM / flash for DTCs, an AEC-Q200 crystal (required with CAN-FD). **4 Vehicle networks:** CAN-FD transceiver with 120 Ω termination and bus ESD diodes, a LIN transceiver with master pull-up on body ECUs, 100/1000BASE-T1 PHYs on ADAS and gateway ECUs. **5 Actuation:** PROFET high-side switches whose IS diagnostic reaches the MCU, gate drivers and MOSFETs with clamped inductive loads. **6 Sensor conditioning:** instrumentation amplifiers, and series R + RC + ESD on every harness input (ISO 10605). Each gap is a Design Check (REL_REVERSE_POLARITY, REL_NO_FUSE, REL_LOAD_DUMP_RATING, REL_EMI_FILTER, REL_COLD_CRANK, REL_WATCHDOG, REL_SAFETY_MCU, REL_NVM, REL_CLOCK, REL_BUS_ESD, REL_BUS_TERMINATION, REL_LIN_NODE, REL_HIGHSIDE_DIAG, REL_INPUT_PROTECTION). New parts: TJA1044GT, TJA1021, MCP2518FD, TPS3823-33/-50-Q1, 24LC256, 8 / 20 MHz crystals (HC-49). The **Automotive Body ECU** example implements all six segments: 13.5 V input with IRLZ44N reverse-battery MOSFET, fuse, SM8S33A and pi filter; LM2940 + XC6206 + TPS3823 watchdog; ATmega328P with crystal and 24LC256; MCP2518FD + TJA1044GT CAN-FD with termination and ESD, TJA1021 LIN master; IR2104 + IRF540N half-bridge with a current shunt; INA333 protected differential sensor input. It routes 175/175 on 4 layers and verifies clean; only the lockstep item stays open, because the ATmega is not an ASIL-D part. |
| **Aerospace segments (LEO satellite / CubeSat, GEO & deep space, launch vehicle, military aircraft, commercial avionics)** | Pick the mission in **Properties → Aerospace Segments** (or **Design → Aerospace Mission**). The Space industry profile alone reports the checks as advice; a mission makes them full Design Checks. The panel checks five segments on the live design. **1 Rad-hardened & redundant compute:** rad-hard / rad-tolerant processors (ATmegaS128, SAMRH71, GR712 / GR740, RTG4, XQR), three identical lanes with hardware majority voters (TMR), MRAM, and single-event latch-up protection: each processor fed through a current-limited switch that an independent watchdog power-cycles. **2 Power conditioning & isolation:** a 28 V bus or solar-array input on its own return, a galvanically isolated DC-DC (the simulator models isolated converters: primary power = output power / efficiency, separate secondary ground), MIL-PRF-19500 / DO-160 lightning TVS, SSPC / eFuse. **3 Sensor interface:** high-CMRR instrumentation amplifiers, ADCs on an isolated supply behind digital isolators, thermocouple cold-junction compensation, protected harness pins. **4 Avionics communications:** transformer-coupled dual-redundant MIL-STD-1553 (military, launchers), SpaceWire LVDS with 100 Ω receiver termination (spacecraft), ARINC 429 with lightning protection (commercial). **5 RF telemetry:** 50 Ω coax at the board edge, a low-loss laminate (Rogers / PTFE), a ground plane for shield cans and via fences. Each gap is a Design Check (REL_RAD_HARD, REL_TMR_VOTER, REL_MRAM, REL_SEL_PROTECTION, REL_SEL_POWER_CYCLE, REL_SEL_WATCHDOG, REL_ISOLATED_POWER, REL_LIGHTNING_TVS, REL_SSPC, REL_THERMOCOUPLE_CJC, REL_ISOLATED_ADC, REL_HARNESS_INPUT, REL_1553_COUPLING, REL_1553_REDUNDANCY, REL_AVIONICS_BUS_PROTECTION, REL_LVDS_TERMINATION, REL_RF_LAMINATE, REL_RF_IMPEDANCE, REL_RF_GROUND, REL_COAX_EDGE). New parts: ATmegaS128, MR25H40, TPS2553, 74HC10, UT54LVDS031 / 032, MCP3201, MAX31855K, CC1101, 26 MHz crystal, and generic isolated DC-DC function blocks (28 V → 5 V, 5 V → 5 V). Fine-pitch land patterns below a preset's minimum spacing are now reported as Info for the fab to confirm, instead of as an error. The **Satellite On-Board Computer** example implements all five segments: three ATmegaS128 lanes with MRAM, TPS2553 latch-up limiters cycled by TPS3823 watchdogs and a 74HC00 + 74HC10 majority voter; a 28 V bus with fuse, MPLAD15KP36CA TVS, pi filter and isolated DC-DC; an isolated INA333 + MCP3201 sensor channel behind ADuM1201s and a MAX31855K thermocouple input; a SpaceWire port on UT54LVDS031 / 032; a CC1101 UHF transmitter with balun, 50 Ω low-pass and an edge SMA, on a 4-layer Rogers RO4350B board. |
| **Naval segments (surface combatant, carrier, submarine, patrol, commercial marine)** | Pick the platform in **Properties → Naval Segments** (or **Design → Naval Platform**); the Marine industry alone reports the checks as advice. **1 Power filtration & galvanic isolation:** ship's power on its own return through an isolated forward / flyback DC-DC, a MIL-STD-1399 surge front end (varistor + gas discharge tube), PFC on AC supplies, an EMI pi filter. **2 Corrosion-resistant hermetic compute:** a military-grade Parylene-C / silicone coating, a 2 mm copper-free coating border, hermetic ceramic ICs, anti-dendrite spacing beyond Class 3. **3 Shock & vibration:** underfill / corner bonding, mounting holes ≤ 75 mm apart, 8–14 layer ≥ 2 mm polyimide / high-Tg boards (MIL-STD-901E). **4 Marine data links:** fibre links on warships, 2.5 kV-isolated CAN / RS-485, NTDS for legacy systems. **5 Radar & sonar:** PIN-diode limiters ahead of LNAs, hydrophone inputs on their own analog return with a single star point. Design Checks: REL_SHIP_ISOLATION, REL_SURGE_FRONT_END, REL_PFC, REL_EMI_FILTER, REL_SALT_FOG_COATING, REL_ECM_SPACING, REL_HERMETIC, REL_COATING_BORDER, REL_SHOCK_UNDERFILL, REL_SHOCK_MOUNTING, REL_SHOCK_SUBSTRATE, REL_ISOLATED_BUS, REL_FIBER_LINK, REL_LNA_LIMITER, REL_SONAR_GROUND. Board Setup gains board thickness and an underfill / corner-bond switch (plans set "coating", "thickness" and "underfill"). New parts: S14K35 varistor, GDT-90V, fibre TX / RX blocks, LNA MMIC block, and the radial-disc (DISC) package. The **Naval Shipboard Interface** example covers all five segments on an 8-layer 2.4 mm Parylene-coated polyimide board and verifies clean. |
| **Medical segments & isolation barriers (type BF, type CF, life-support, implant, home healthcare)** | Pick the class in **Properties → Medical Segments** (or **Design → Medical Device Class**); the Medical industry alone reports the checks as advice. **Galvanic isolation domains:** nets joined by anything but an isolator / optocoupler / isolated converter form one domain. With **Board Setup → Isolation barrier** (2.5 / 4 mm 1 × MOPP / 8 mm 2 × MOPP), Auto Place keeps parts of different domains that far apart, Auto Route fences each domain's copper, pours stop that short of foreign copper and DRC reports any copper closer (DRC_ISOLATION_GAP); a barrier part's own land pattern is rated by its datasheet. **1 Patient isolation & defibrillator protection:** the applied part on its own domain (a Y-capacitor or resistor across the barrier is caught), 2 × MOPP-rated isolators and converter, the 8 mm barrier, a GDT / TVS clamp and ≥ 1 kΩ pulse-rated series resistor on every lead. **2 Biosignal acquisition:** instrumentation amplifier, driven right leg, patient ground split with data through isolators. **3 Safety compute & power:** lockstep MCU and a windowed watchdog forcing the safe state for life-support / implants, battery protector and NTC cut-off on Li-ion cells. **4 Coexistence & wireless:** ESD on every connector pin, a 50 Ω BLE / UWB feed over a ground plane, shield cans. Design Checks: REL_PATIENT_ISOLATION, REL_MOPP_BARRIER, REL_MOPP_CREEPAGE, REL_DEFIB_PROTECTION, REL_BIOSIGNAL_AMP, REL_DRL, REL_SAFETY_MCU, REL_SAFE_STATE_WATCHDOG, REL_BMS, REL_BMS_THERMAL, REL_CONNECTOR_ESD, REL_TELEMETRY_IMPEDANCE, REL_TELEMETRY_GROUND. New parts: ADuM4401 (5 kV wide body), ISO-DCDC-MED (2 × MOPP converter block), DW01A, BLE module block. The **Defibrillator-Proof ECG Monitor** example (type CF) keeps its patient side 8 mm from everything else, routes 144/144 and verifies clean. |
| **Retail / POS segments & active tamper mesh (countertop terminal, unattended terminal, mobile POS, kiosk, receipt printer)** | Pick the device in **Properties → Retail / POS Segments** (or **Design → Retail Device**); the Retail & POS industry alone reports the checks as advice and selects the 4/4 mil fab rules fine-pitch QFNs need. **Active tamper mesh:** **Board Setup → Protection → Tamper mesh** (or a plan's "tamperMeshes") puts two serpentine mesh nets over a secure element — horizontal stripes on Inner 1, vertical stripes on Inner 2, one track + clearance pitch, 2 mm beyond its courtyard. Auto Place keeps other parts clear, and Auto Route lays the mesh with a via at each end, joins each drive / sense pin to its end and keeps every other track and every via out of the secure area. DRC reports a missing stripe (DRC_TAMPER_MESH) and any via or track that breaches it (DRC_TAMPER_MESH_BREACH). **1 Payment security & anti-tamper:** secure element, the mesh, case-open switches on its tamper inputs, keys on a backup cell, EMV / magstripe front ends. **2 Printer drivers:** ≥ 470 µF at the print-head supply, a stepper H-bridge with its PowerPAD on a pour, flyback on cutter / drawer coils. **3 HMI & peripherals:** a USB hub for several ports, an opto-isolated 24 V cash-drawer kick, 100 Ω display pairs. **4 ESD & environment:** a TVS on every exterior port pin, acrylic conformal coating. Design Checks: REL_SECURE_ELEMENT, REL_TAMPER_MESH, REL_TAMPER_SWITCHES, REL_KEY_BATTERY, REL_TPH_BULK, REL_PRINTER_MOTOR, REL_DRIVER_THERMAL, REL_SOLENOID_FLYBACK, REL_USB_HUB, REL_CASH_DRAWER, REL_DISPLAY_PAIRS, REL_PORT_ESD, REL_RETAIL_COATING. New parts: SECURE-MCU block, NCN8025, USB2514B, DRV8833 (HTSSOP PowerPAD), 80 mm print head, LM2596-5.0 (a buck simulated by its efficiency), USBLC6-2SC6, TPD4E1U06, 24 MHz crystal. The **Countertop Payment Terminal + Receipt Printer** example covers all four segments on a 4-layer acrylic-coated board, routes 245/245 and verifies clean. |
| **Home appliance segments & mains spacing (laundry, kitchen & cooking, refrigeration, HVAC, small appliances)** | Pick the type in **Properties → Home Appliance Segments** (or **Design → Appliance Type**); the Home Appliances industry alone reports the checks as advice. **Mains spacing:** with an AC mains source the simulation runs a few cycles so L, N, the rectified bus and the off-line switch node carry their real swing. Every net above 60 V then becomes its own spacing domain. Auto Place, Auto Route (per copper layer) and the pours keep its IPC-2221 distance (0.8 mm for 325 V under conformal coat, 2.5 mm bare) and radio modules get a 6 mm mains-free keep-out, while the low-voltage logic keeps its normal design rules; DRC checks every pair. **1 AC mains entry & power:** slow-blow fuse and varistor, X2 capacitor and common-mode choke, an off-line (LinkSwitch-TN) buck. **2 Actuation & motor control:** triacs with RC snubbers, an opto-triac driver, zero-cross detection, an IPM with shunt sensing for inverter motors. **3 HMI & sensing:** capacitive touch with series resistors and a guard, NTC dividers with RC filters, hall / tacho speed sensing. **4 IoT:** a Wi-Fi / BLE module at the board edge, ≥ 6 mm from mains copper. Design Checks: REL_MAINS_FUSE, REL_MAINS_MOV, REL_X_CAP, REL_CM_CHOKE, REL_OFFLINE_SUPPLY, REL_TRIAC_SNUBBER, REL_ZERO_CROSS, REL_IPM, REL_IPM_SHUNT, REL_TOUCH_SERIES, REL_NTC_FILTER, REL_ANTENNA_EDGE, REL_ANTENNA_MAINS. New parts: S10K275, X2 film capacitor, LNK306, BT136-600E (formed leads), MOC3021, H11AA1, 600 V IPM block, AT42QT1070, A3144, ESP32-WROOM-32E with a castellated MODULE package. The **Washing Machine Controller** example (230 VAC, triac pump, inverter drum IPM, touch keys, NTC, hall, ESP32) routes 157/157 and verifies clean. |
| **Memory (RAM) segments (SDR SDRAM, DDR3L / DDR4 memory-down, LPDDR, DDR5 UDIMM / SO-DIMM, RDIMM)** | Pick the type in **Properties → Memory (RAM) Segments** (or **Design → Memory Design**); the Memory & DRAM industry alone reports the checks as advice. **1 Power & decoupling:** 100 nF per VDD / VDDQ pin pair and bulk at the DRAM; on DDR a VTT rail and a filtered VREF = VDDQ / 2. **2 Clock, command & address:** a series resistor on SDR SDCLK, a 100 Ω-terminated CK pair and fly-by command / address terminated to VTT on DDR. **3 Data integrity:** the data bus named (DQ0… / SD_DQ0…) so Auto Route length-matches it, DQS as differential pairs, 240 Ω 1 % on ZQ. **4 Configuration:** the memory controller found on the data lines, RESET_n pulled low, an SPD EEPROM with pull-ups on modules. **5 Layout:** the DRAM near its controller, ≥ 4 layers with a GND plane, a 34–50 Ω DDR impedance target, a 1.2–1.27 mm module board. New parts (KiCad pinouts): MT48LC16M16A2TG-6A, W9812G6KH-6, IS42S16400J-7TL (TSOP-II-54 SDR SDRAM), MT41K256M16HA-125, AS4C256M16D3-12BCN (DDR3 / DDR3L) and MT40A512M16LY-062E (DDR4) on 96-ball FBGAs whose depopulated 9 × 16 ball grid is laid out from the ball names. The **STM32H743 + 32 MB SDRAM Frame Buffer** example routes 143 / 143 on 6 layers and verifies clean. See [docs/MEMORY_DESIGN.md](docs/MEMORY_DESIGN.md) |
| **Live board** | **Run** (▶, ⌘R) runs the board in real time: LEDs glow with their current, you click switches and push-buttons, probes, a scope and the serial monitor update as firmware runs. See [Live board simulation](#live-board-simulation) |
| **Microcontroller firmware** | Upload an Arduino/avr-gcc `.hex` to an ATmega328P or ATtiny85 and run it in the transient simulation. Pins drive the circuit, inputs and the ADC read it, and the serial monitor shows its output. See [Microcontroller simulation](#microcontroller-simulation-firmware) |
| **Verification process** | **Design → Verify Design** (⌥⌘V) runs a 7-stage sign-off: ERC → DC simulation → circuit validation → footprint placement → routing completion → DRC/fab rules → manufacturing outputs (every Gerber layer, drill, BOM, pick-and-place and netlist are generated and checked). The verdict is Pass, Pass with warnings or Fail; findings cross-probe to the editors, and the report exports as Markdown. **Export Fabrication Package** always verifies first, asks before exporting a failing design, and includes `verification_report.md` |
| **Model choice** | Anthropic **Claude** (default: `claude-opus-5-5`, structured outputs, adaptive thinking, effort control, refusal fallbacks), OpenAI or any OpenAI-compatible endpoint, Google Gemini, local Ollama, and an Offline Designer that needs no network |
| **Schematic capture** | 16 built-in device types plus your own library parts, orthogonal wiring with T-junctions (end a wire on any wire to join it, e.g. a part in parallel), corners (wire tool: click empty space), bendable wires (drag a wire or its bend points; deleting a bend straightens it), net labels, rotate/move/marquee, undo/redo, ERC with pin-type rules, no-connect flags (Q) for pins left open on purpose |
| **Datasheet → component** | Drop a datasheet (PDF, pinout screenshot or text). The Datasheet Analyst agent extracts part number, package and every pin with its electrical type: Claude and Gemini read the PDF natively, OCR (Vision) handles images, and an offline pin-table parser works without a key. You review it in a pin-table editor with live symbol and footprint previews, then save it to the project library and place it like any other part. Any part's footprint can be redrawn pad by pad in the [Footprint Editor](#footprint-editor), and its symbol arranged in the [Symbol Editor](#symbol-editor). AI design agents can use library parts too |
| **Footprint Editor** | Draw any part's land pattern pad by pad (**Component Library → Edit Footprint…**). You set each pad's position, size, rectangle or circle shape, drill (SMD or plated through-hole), number, and pin (its own, another pin such as a tab or exposed pad, or none for a mounting hole). The canvas has a grid, ⇧-click selection, drag to move and double-click to add. Tools: duplicate, delete, mirror, centre, pad arrays, body size and undo. Live checks flag overlaps, gaps under 0.1 mm, thin annular rings, pads without pins and pins without pads; errors block Apply. Generated footprints (DIP, SOIC, QFN, TO-263, BGA, …) convert with the same pads on the same pins. See [Footprint Editor](#footprint-editor) and [docs/FOOTPRINT_EDITOR.md](docs/FOOTPRINT_EDITOR.md) |
| **Symbol Editor** | Arrange any part's schematic symbol (**Component Library → Edit Symbol…**). Drag pins to any of the four sides and any slot, separate groups with gaps, and stack repeated VDD / GND pins onto one spot (drawn once, joined into one net). **Auto Arrange** puts supplies on top, grounds at the bottom, inputs left, outputs right, and groups MCU ports in bit order. Live checks catch unplaced, duplicated or colliding pins. Library parts with 16+ pins (MCUs, FPGA, modules, drivers) come arranged. See [Symbol Editor](#symbol-editor) and [docs/SYMBOL_EDITOR.md](docs/SYMBOL_EDITOR.md) |
| **Simulation** | Modified Nodal Analysis with Newton–Raphson: DC operating point and transient. Diode/LED (Shockley), BJT (Ebers–Moll), MOSFET (square-law with λ), saturating op-amp, R/L/C, DC/SIN/PULSE sources, batteries and AC (mains / transformer) sources. Part-number device models (1N4148, SS14, BC847, 2N7002, AO3400, SI2302, IRF540N, …) and behavioural chip models: regulators and chargers (LM7805, LM317, XC6206, AP2112K, TP4056 with constant-current charging) and IC supply current (ATmega328P, MPU-6050, nRF24L01+, …). Switching designs are checked for RMS/peak stress in steady state. Waveforms are drawn with Swift Charts |
| **PCB layout** | Footprint library (0805, SOD-123, SOT-23, SOIC, TSSOP, DIP, QFN with exposed pad, LQFP, 1×N and 2×N headers, TO-220), connectivity-driven auto-placement with escape space around fine-pitch parts, board fit, ratsnest, geometric and manufacturability DRC (see design rules). **Board Setup** (PCB options bar): board outlines (rectangle, rounded, circle, quadcopter X frame), M2/M3 mounting-hole patterns with keep-outs, copper pours and reserved plane layers (clearance-aware fill, thermal reliefs, island removal), and net classes with IPC-2221 auto-sizing from the simulated currents |
| **Autorouting** | A final clean-up pass merges collinear pieces and chamfers right-angle corners to 45° wherever clearance allows, so tracks look hand-routed. In the PCB view, copper is coloured **By Net** (power red, ground blue, negative rails purple, signals yellow on top, green on the bottom, and other colours on inner layers) or **By Layer** (the CAD convention: top red, bottom blue, inner yellow / green / orange / magenta), with a legend in the Layers panel. Part designators show at normal zoom, values inside parts and net names along tracks as you zoom in, and pad numbers close up. One **Auto Route** button (PCB options bar, or ⇧⌘R): places any footprints not on the board yet inside the board shape (outline, mounting holes, edge clearance), then routes every connection cleanly and runs DRC, as one undo step. A banner offers it whenever connections are still unrouted. Single-layer (single-sided, no vias), 2-, 4- and 6-layer A* autorouter: through vias, alternating layer directions, turn penalties, rip-up and retry passes, escape routing and neck-down for 0.5 mm-pitch QFN pins, fan-out vias to ground pours/planes, exact clearance for wide power tracks **Every IC package auto-routes:** DIP, SOIC, TSSOP, QFN, LQFP, SON, SOT-23, SOT-223, TO-220, TO-263, Multiwatt, LGA, BGA, modules, headers and crystals (a test routes one library part of every package type with no DRC error). **BGA / FPGA / VLSI fan-out:** each ball gets a dogbone (a short stub to a via between four balls, pointing away from the package centre) or, with via-in-pad (VIPPO), a via in its pad; BGA boards route on a grid of 1/8 ball pitch. An Artix-7 FPGA with a DDR3L memory-down routes completely on 8 layers with GND and supply planes. Exposed pads and QFN corner pads keep ≥ 0.15 mm from every pin pad. |
| **3D** | **Assembly** (default): a realistic board in your solder mask colour: green like most boards, or black, blue, red, yellow, white or purple (saved with the project; black legend on yellow and white). The board follows the Layers setting in PCB Layout: single-sided boards have a bare FR-4 underside, 2-layer boards have solder mask on both sides, and 4/6-layer boards show their inner copper on the board edge. Each surface has its own physically based material: glossy lacquered mask, metallic pad finish (choose **ENIG** gold, **HASL** tin or **OSP** bare copper), shiny solder joints, tinned gull-wing leads and chip terminations, gold header pins, matte silkscreen and moulded plastic. Studio lighting reflects soft boxes in the metals, soft shadows fall on the board and an invisible floor, and ambient occlusion darkens the gaps under parts. The silkscreen carries every reference designator and a pin-1 dot; packages show a laser-etched part number and pin-1 dimple; drilled vias and holes are dark; electrolytic cans have a vented aluminium top and LEDs a clear lens. **X-Ray Stack:** a holographic exploded view of every copper layer with an adjustable gap. Copper glows with additive blending and HDR bloom, through vias become light pillars, part bodies are wireframes, and a scan beam sweeps the stack; turntable spin and per-layer toggles. The **HUD** switch adds a cinematic holo-table: a hex-mesh floor, counter-rotating projector rings and a light cone, drifting particles, layers that materialise one after another, amber targeting reticles over the largest parts with leader lines to floating data cards (reference, value, package, pins), data pillars sized by pin count, and a 2D heads-up frame with scanlines, a rotating reticle and live board read-outs (size, stack, parts, nets, tracks, vias). The **Panels** switch (off by default) unfolds five holographic glass panels on light posts in an arc behind the stack: copper length per layer, a net fan-out histogram with power and ground counts, parts by type, a routing-completion gauge and a board mini-map; they float gently and turn to face the camera as you orbit. Both export to STL/OBJ |
| **Design for reliability** | Design Checks and the verification report (new **Design for Reliability** stage) explain each failure mechanism and its fix. **Leakage:** IPC-2221B Table 6-1 spacing (all columns B1–A7; A5 when the board is conformal coated, set in Board Setup → Protection & Reliability: acrylic, silicone, urethane, epoxy, parylene per IPC-CC-830). High-impedance inputs are found automatically and get a guard-ring recommendation (driven by the other amplifier input, or by the output for a follower), 0.5 mm leakage spacing (0.25 mm coated) and air-wiring advice for pA work, plus moisture baking (IPC-1601). **Thermal:** hot spots from the DC operating point and package θJA against 150 °C junctions and the laminate's Tg; thermal reliefs on two-pad chips in pours. **CTE:** via aspect ratio. **Signal integrity / EMI:** 3W crosstalk, missing ground reference plane, RF 50 Ω microstrip width (IPC-2141) checked only when the line is longer than λ/10, plus RF vias and width steps. **Assembly:** tombstoning, solder bridges and acid traps. **Motor control:** missing flyback diodes, half-bridge shoot-through without a gate driver, RC snubbers, ground bounce. **Industry:** automotive load dump (ISO 16750-2 / ISO 7637-2 TVS) and thermal cycling; vibration staking; space tin whiskers (SnPb), outgassing (polyimide, ASTM E595) and radiation; flex fatigue (IPC-2223). **Auto Route** follows the same rules: high-impedance and fast nets route first with leakage / 3W spacing, and RF nets get their 50 Ω width. **Fab notes and job file** carry the profile's material, finish, solder alloy, coating and IPC class |
| **Stack-up, impedance & length matching** | Board Setup → **Stack-up & Impedance**: laminate (FR-4, high-Tg FR-4, Isola 370HR, Rogers RO4350B, Panasonic Megtron 6, polyimide, aluminium IMS) with εr / Df / Tg, construction (rigid, rigid-flex, metal-core) and single-ended / differential impedance targets. Each copper layer shows its line type (surface microstrip or buried stripline, IPC-2141) and the width / pair geometry that hits the target; Auto Route sizes RF nets and differential pairs (…_P/_N, …+/-) to them. **Backdrilling:** via stubs on fast nets of ≥ 4-layer boards are found (REL_VIA_STUB) and, when enabled, a back-drill Excellon file joins the fabrication package. **Length & phase matching:** differential pairs and buses (DQ0…, DATA[0..7], ADDR…, RXD…) are matched after Auto Route with serpentine (accordion) tuning that keeps clearance; Board Setup → Length & Phase Matching lists every group with its lengths, sets the skew / bus tolerances and has **Tune Lengths**; mismatches show in Design Checks (REL_LENGTH_MISMATCH). Metal-core boards warn above two layers. **HDI (IPC-2226):** with HDI on (≥ 4 layers), Auto Route cuts every via to the layers it actually joins: blind, buried, or laser microvias (0.1 mm default) across thin 0.075 mm build-up dielectrics. Each span gets its own Excellon file (`*-L1-L2.drl`), Gerbers and the 3D / X-ray views show the via only on its layers, and Design Checks cover microvia aspect ratio (≤ 1:1) and stacked microvias. **Via-in-pad plated over (VIPPO, IPC-4761 Type VII)** allows vias in SMD pads and goes in the fab notes. **Embedded passives:** in the Inspector, set a resistor or capacitor's **Mounting** to an inner layer to build it inside the board. A resistor becomes thin-film foil (Ohmega-Ply / TICER, 25–250 Ω/sq; the sheet is chosen for about 3 squares, so 330 Ω = 3.3 sq of 100 Ω/sq, 0.5 × 1.65 mm). A capacitor becomes buried-capacitance plates on two neighbouring inner layers (1.6 nF/cm²). Embedded parts are routed and DRC-checked on their layer, but don't collide with surface parts. They stay off the pick-and-place / CPL and assembly BOM, appear as "embedded" BOM lines, and get a resistor-element Gerber (`*-In<n>_Resistor.gbr`) plus an EMBEDDED PASSIVES section in the fab notes. Design Checks cover stack-up, achievable value / plate size, tolerance (±10 % untrimmed, laser trim to ±2 %) and foil power. **Locked footprints:** Inspector → Locked keeps a part where it is through Auto Place. Reference designs use this to fix connectors and terminators; see the length-matched **High-Speed Link** example. |
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
| Retail & POS | PCI PTS POI, EMVCo Level 1, IEC 62368-1, IEC 61000-4-2 level 4 | Fab House Advanced (4/4 mil) | 80 % / 80 % | Countertop payment terminal + receipt printer: secure MCU under a tamper mesh, EMV front end, print head, stepper, cutter, USB hub, drawer kick |
| Home Appliances | IEC / UL 60335-1, IEC 60730-1, IEC 60664-1, IEC 61000-4-4/5, CISPR 14-1 | IPC-2221 Class 2 + per-net mains spacing | 70 % / 80 % | Washing machine controller: mains entry, LinkSwitch buck, triac pump, inverter IPM, touch, NTC, hall, ESP32 |
| Memory & DRAM Design | JEDEC JESD79-3/-4/-5 (DDR3/4/5), JESD209 (LPDDR), JESD21-C (SDRAM, SPD), JESD301 (PMIC), IPC-2141, IPC-2226 | HDI / Fine-Pitch BGA (IPC-2226) | 80 % / 80 % | STM32H743 + 32 MB SDR SDRAM frame buffer on FMC: decoupling per pin pair, 22 Ω SDCLK, length-matched SD_DQ bus, 6 layers with GND / 3.3 V planes |

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

## Production parts catalog

The standard library covers the usual robotics, automotive and industrial production catalog. Every part listed here has its orderable part number, its real package, and datasheet pin names. Where KiCad's symbol library has the part, the pinouts come from it (`tools/fetch_catalog_parts.py` and `tools/fetch_mcu_pinouts.py` write them).

- **Compute:**
  - ATMEGA328P-AU / -PU, STM32F103C8T6, STM32F405RGT6, STM32H743IIT6, STM32F765VIT6, LPC1768;
  - ESP32-WROOM-32E, RP2040, ATSAMD51J20A;
  - XC7A35T-1CSG324I FPGA (324-ball BGA).
- **Motion control:** L293D / L293DD, L298HN (Multiwatt-15), ULN2003A / ULN2003ADR, PCA9685PW, IR2101S, IR2110S, TMC2209-LA, TMC2160-TA, TMC5160A-TA, CSD18540Q5B.
- **Sensors and conditioning:** MPU-6050, MPU-9250, BMI160, ADS1115IDGS, AS5047D-ATSM, INA240A1EDRQ1, LM358DR / LM358N, LM393DR / LM393N.
- **Audio:** MAX9814ETD+T, MAX4466EXK+T, ICS-43434 (I²S microphone).
- **Interfaces:** 74HC595 / 74HC595D, PCF8574TS / PCF8574N, CH340G, CP2102N, MAX485ESA+T, SP485EEN-L, TJA1050, MCP2551-I/SN, TCAN1042VDRQ1, ADM2587EBRWZ.
- **Power:** LM7805, LM7812, AMS1117-3.3 (SOT-223), LM2596S-5.0 and XL4015E1 (TO-263-5), TP4056, LM74700QDBVRQ1, LTC4359IMS8.
- **Isolation:** PC817X3NSZ0F, TLP281-4.
- **Glue parts:**
  - 16 / 8 MHz crystals (HC-49/S or 3225 SMD) and 32.768 kHz crystals (3215 SMD or cylinder);
  - 1N5819 / 1N4007 diodes with simulation models.

New footprint types: BGA (JEDEC ball names), TO-263 / D²PAK and SOT-223 with the tab tied to the datasheet's pin, Multiwatt-15 (staggered), SON / DFN with a drain tab, exposed pads on TQFP, and 3225 / 3215 / cylinder crystals.

**Package choice for passives.** Pick a resistor, capacitor, inductor, diode or LED and choose its package in the Inspector. The choice is undoable, saved with the project, and the PCB, 3D view and BOM follow it.
- Resistors: 0402, 0603, 0805, 1206, axial through-hole.
- Capacitors: chip sizes, ceramic disc, tantalum A/B, radial electrolytic.
- Diodes: SOD-123, SMA, DO-41.

**Not in the library yet.** These parts are in neither the KiCad library nor a datasheet I could verify here: TMS320F28379D, DRV8301, DRV8353, TPS65381, LM5116, TJA1101, MA702, BMX160, ADS1256, ATSAMD51P20A, INMP441, JQ6500, WT2003HP8 and SYN6288. Each has a file in [`docs/missing-parts/`](docs/missing-parts/README.md) with its package, datasheet link and a pin table to fill in; until then, add them from their datasheet's pin table with **Library → Import from datasheet**. The nearest stocked alternatives are BMI160, ICS-43434 and ATSAMD51J20A.

## Robotics parts kits

Seven robot platforms each carry a production parts kit: Mars / ground rover, FPV drone, industrial arm, quadruped, humanoid, 3D printer and CNC machine. Pick the platform in the Inspector (**Robot System Segments**). **Parts Kit** lists the kit by subsystem, and **Add Kit to Library** puts every part into the project library in one step. In the Component Library, search the standard library, or filter it to one platform's kit.

95 robotics parts were added, on their orderable packages, with pinouts from the KiCad symbol library. Kits also reuse the catalog parts above.
- **Compute:** STM32F446RET6, STM32G474RET6 (FOC joints), STM32H723VGT6, LPC1769FBD100.
- **Brushed and BLDC drives:** DRV8833PWPR, DRV8871DDAR, TB6612FNG, VNH5019A-E, STSPIN32F0A, DRV8308RHAR, DRV8311HRRWR.
- **Stepper drives:** TMC2130 / 2208 / 2226 / 2660 / 5130, DRV8434PWPR, STSPIN220, and A4988 / DRV8825 StepStick carriers.
- **Gate drivers and MOSFETs:** IR2184S, UCC27524, LM5109B, BSC028N06LS3G and BSC040N10NS5 (SuperSO8), IRLZ44N, IRF3205, AO3400A, BSS138.
- **Current sensing:** ACS712 / ACS723, INA219, INA226, INA3221, INA181, ADS131M04.
- **IMUs:** ICM-20948, ICM-20602, BMI088, ISM330DHCX, IIM-42652, BNO055.
- **Pressure and heading:** BMP280, BME280, MS5611, LPS22HB, LIS3MDL.
- **Joint encoders:** AS5048A, MA730, TLE5012B, MT6701, TMAG5273.
- **Distance:** VL53L1X / VL53L0X time-of-flight sensors.
- **Temperature and humidity:** MAX31865, MAX31856, AD8495, SHT31.
- **Analog I/O:** ADS1220, MCP3208, MCP4725 (0–10 V spindle reference).
- **CAN:** MCP2515, TJA1051T/3, SN65HVD230, TCAN4550 (CAN FD), ISO1050 (isolated).
- **Ethernet:** W5500, LAN8720A, KSZ8081.
- **Serial and encoder lines:** MAX3232, MAX3485, THVD1450, AM26LS31 / AM26LV32.
- **I²C and level shifting:** TCA9548A, TXS0108E, TXB0104, SN74LVC1T45.
- **Isolation:** ISO7721, 6N137, TLP291-4.
- **Memory:** W25Q128JV, AT24CS02.
- **Regulators:** LM2576S-5.0, LMR33630, LT8610, LD1117S33, LP2985-33.
- **Protection and load switching:** LM5050-1, LM5060, TPS22919.
- **Batteries and charging:** BQ76920 / BQ76940 BMS, BQ24074 / BQ25895 chargers.

**Exact land patterns.** Some packages can't be drawn from a pin count and pitch. For these parts the pads come one by one from the KiCad footprint (new package type `LGA`, saved with the part):
- rectangular and clockwise-numbered LGA sensors (BMI088, BMP280, BNO055, MS5611, VL53L1X);
- rectangular QFNs (TMC2130, TCAN4550);
- SuperSO8 MOSFETs with one large drain pad;
- 0.65 mm SC-70 parts;
- the three-slug MultiPowerSO-30.

`tools/fetch_catalog_parts.py` regenerates the catalog (it needs network access to gitlab.com).

## Footprint Editor

In the Component Library, select a part and click **Edit Footprint…** to draw its land pattern pad by pad. This works on any part: a generated footprint (SOIC, QFN, DIP, TO-263, BGA, …) first converts to an editable one, with the same pads on the same pins.
- **Canvas.** Pads sit on a grid you choose (0.01 to 2.54 mm), with the body, the courtyard and the pin-1 marker drawn around them.
  - Click a pad to select it, and ⇧-click to add pads to the selection.
  - Drag to move the selection; it snaps to the grid.
  - Double-click an empty spot to add a pad there, and press ⌫ to delete.
- **Pad properties:**
  - X, Y, width and height;
  - rectangle or circle / oval;
  - drill (0 is an SMD pad; anything above 0 makes a plated through-hole);
  - number: change it to renumber the pads;
  - pin: by default a pad connects to the pin with its own number. You can point it at another pin (a tab, an exposed pad, a second pad on a ground pin) or mark it mechanical (no pin).
- **Tools.** Add Pad, Duplicate, Delete, Mirror left ↔ right, Centre on the origin, Pad Array (a row or column of N pads at a pitch), body width and length, and Undo / Redo for every edit.
- **Live checks** (from the core). Errors block **Apply Footprint** until you fix them:
  - overlapping pads (error);
  - copper gaps below 0.1 mm (warning);
  - annular rings below 0.1 mm (warning);
  - pads with no pin (warning);
  - pins with no pad (error).
  
  Click a finding to select its pads.

The edited part is saved with the project as a `CUSTOM` land pattern. It places, routes and appears in the 3D view like any other part.

**Typical workflow.**
1. Select the part and click **Edit Footprint…**.
2. Edit the pads, then click **Apply Footprint**.
3. Click **Add to Library** / **Save Changes**.

Parts already on the board switch to the new footprint and keep their position. **Edit → Undo** brings the old footprint back.

| In the editor | Action |
|---|---|
| Click / ⇧-click / ⌘-click a pad | Select / add to or remove from the selection |
| Click empty space | Clear the selection |
| Drag a selected pad | Move the selection (snaps to the grid on release) |
| Double-click empty space | Add a pad there |
| ⌫ | Delete the selected pads |
| Esc / ↩ | Cancel / Apply Footprint |

The full guide covers how pads map to pins, every check code, conversion details, the land JSON format, the code map and the tests: [docs/FOOTPRINT_EDITOR.md](docs/FOOTPRINT_EDITOR.md).

## Symbol Editor

The symbol is how a part appears on the schematic. In the Component Library, select a part and click **Edit Symbol…** to arrange where its pins sit. Readable symbols keep wires short and the signal flow obvious: supplies at the top, grounds at the bottom, inputs on the left and outputs on the right.

- **Canvas.** Click a pin to select it, and ⇧-click to add more. Drag to any side and slot: a marker shows where the pins will land, and pins already there shift along. ⌥-drop onto a pin to stack onto it.
- **Selected pins:** send them to Left / Right / Top / Bottom, move them one slot with ↑ / ↓, insert a gap before them, and Stack / Unstack.
- **Tools:**
  - **Auto Arrange** (optionally stacking repeated power pins);
  - **Datasheet Order** (the plain box);
  - **Mirror**, **Close Gaps**, body width;
  - **Undo / Redo**.
- **Stacked pins** (several pins on one spot, e.g. the STM32's four `VDD` pins numbered `19,32,48,64`) are one node: a wire to the stack connects them all, and on the PCB their pads are routed together. A stack with no wire is still reported by ERC.
- **Live checks** (from the core):
  - errors, which block **Apply Symbol**: pins not on the symbol, unknown or duplicated pins, pins with different names on one spot;
  - a warning for stacked signal pins;
  - info for stacked power pins.
- **Wires follow their pins.** Rearranging a placed part's symbol moves its pins, and the wires stay attached. **Edit → Undo** restores the old symbol.
- **Drawing.** Symbols now have pins on all four sides. Pin names along vertical leads read from bottom to top at any rotation.

Library parts with 16 or more pins come auto-arranged; smaller parts keep the datasheet-order box. To change a library part's symbol, add it to the project library and edit it there.

**Typical workflow.**
1. Select the part and click **Edit Symbol…**. A part that was never arranged opens as the datasheet-order box.
2. Click **Auto Arrange**, then fine-tune: drag pins, add gaps between groups, stack or unstack power pins.
3. Click **Apply Symbol**, then **Add to Library** / **Save Changes**.

Components already on the schematic switch to the new symbol, with their wires still attached. The board, nets and footprint don't change, except that stacked pins join one net.

| In the editor | Action |
|---|---|
| Click / ⇧-click / ⌘-click a pin | Select (with its stack) / add to or remove from the selection |
| Click empty space | Clear the selection |
| Drag selected pins | Move them to the side and slot under the pointer; pins already there shift along |
| ⌥-drop onto a pin | Stack the dragged pins on that pin |
| Esc / ↩ | Cancel / Apply Symbol |

The layout is saved with the part as `symbolLayout` (a side and slot for each pin, plus an optional body width), so it travels with the project file. Tests cover every Auto Arrange rule, the checks, four-sided geometry, stacked pins on a placed and routed board, project save and reopen, and the editor and schematic in a live window.

Full guide (Auto Arrange rules, check codes, layout JSON, code map, tests): [docs/SYMBOL_EDITOR.md](docs/SYMBOL_EDITOR.md).

## Speed on large designs

Auto-place works out the other parts' keep-outs and each net's position once per part, not once per candidate position. The C++ core is also optimised in Debug builds, which is what `run.sh` uses. All 40 reference designs place, route, verify and build their 3D model in about 60 s in total, against about 163 s before. A 300-connection satellite computer takes about 21 s, against several minutes in an unoptimised Debug build. Tests hold each step of a 201-part board, and the three largest reference designs, to time budgets.

## Crash reports and recovery

SiEDA keeps its diagnostics on your Mac and never uploads them (`~/Library/Application Support/SiEDA/Diagnostics`).

- **Crash reports.** A crash (a Swift runtime trap, an invalid memory access, an abort or an uncaught exception) is written with a backtrace. Recent activity is recorded too: edits, opens, saves, workspace switches, hangs and memory warnings. If the app ended abnormally, the next launch turns this into a readable report and tells you. The newest 20 reports are kept; use **Help → Show Crash Reports** to see them.
- **Recovery.** While a design has unsaved changes, it is autosaved a few seconds after you stop editing. The file is written in the background. After a crash, SiEDA offers to restore that work. Saving, or quitting normally, removes the autosave.
- **Bounded memory.**
  - Undo and redo keep at most 100 steps and 96 MB in total. On low memory, redo and the older half of undo are released.
  - The X-ray textures are fixed-size 1× bitmaps, held in a shared cache that has a size limit.
  - The panel textures are reused until the design changes.
  - The board mini-map draws at most 8,000 tracks and 6,000 pads.

## Repository layout

```
Core/                C++17 engine (no dependencies)
  include/sieda/     public headers; sieda_c.h is the stable C ABI used by Swift
  src/               schematic, simulator, PCB/autorouter/DRC, mesh, exports, JSON
  tests/             unit tests (C++ and a C-compiled ABI smoke test)
  cli/               sieda-cli: headless ERC → simulation → place & route → DRC → verification → fabrication files
SiEDA/               macOS SwiftUI app
  App/               app entry point, launch splash, menus, DesignStore (state, undo, documents)
  Bridge/            bridging header + EDAEngine (thread-safe Swift façade over the C ABI)
  Models/            snapshot models, ComponentKind, DesignPlan (the agents' structured output), FootprintDraft and SymbolDraft (editor documents)
  AI/                providers (Claude, OpenAI, Gemini, Ollama, Offline), prompts, orchestrator, settings
  Views/             Super Intelligence (PromptStudio/), Schematic, PCB, 3D, Simulation, Checks, Inspector, Settings,
                     Library (Component Library, Footprint Editor, Symbol Editor), BOM
SiEDATests/          XCTest suite (bridge, plan compiler, offline templates, editors, live windows)
SiEDA.xcodeproj      generated by tools/generate_xcodeproj.py
docs/                PRD, architecture, footprint and symbol editor guides, memory design guide, missing-parts worksheets
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
