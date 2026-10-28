# Bring-up checklist — SMPS 230 VAC to 5-48 V 5 A (CC/CV)

Work top to bottom; tick each line and write the measured value next to it.

## 1. Before power

- [ ] Inspect the board: solder bridges on fine-pitch parts, polarity of diodes, LEDs, electrolytics and connectors, pin 1 of every IC.
- [ ] Resistance VCC_P → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).
- [ ] Resistance FLY_VREF → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).
- [ ] Resistance VAUX → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).
- [ ] Resistance VBIAS_S → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).
- [ ] Resistance VCC_S → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).
- [ ] Resistance VREF5 → GND is not a short (expect more than ~100 Ω; less points at a solder bridge or a reversed part).

## 2. First power

- [ ] Bench supply at the lowest input voltage, current limit about 2× the expected idle current.
- [ ] Note the input current at switch-on and after 10 s; feel for hot parts.

## 3. Rails

- [ ] VCC_P — measure at the farthest load; ripple under 50 mV p-p (11 parts on the rail).
- [ ] FLY_VREF — measure at the farthest load; ripple under 50 mV p-p (6 parts on the rail).
- [ ] VAUX — measure at the farthest load; ripple under 50 mV p-p (7 parts on the rail).
- [ ] VBIAS_S — measure at the farthest load; ripple under 50 mV p-p (5 parts on the rail).
- [ ] VCC_S — measure at the farthest load; ripple under 50 mV p-p (13 parts on the rail).
- [ ] VREF5 — measure at the farthest load; ripple under 50 mV p-p (8 parts on the rail).

## 4. Clocks, resets, boot

- [ ] BUCK_EN sits at its run level after power-up (high unless active-high).
- [ ] BOOT sits at its run level after power-up (high unless active-high).

## 5. Programming and interfaces

No programming or bus nets found by name.

## 6. Sign-off

- [ ] Record the board's serial number, revision and every measured value above.
