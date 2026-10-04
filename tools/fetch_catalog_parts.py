#!/usr/bin/env python3
"""Fetches the production parts catalog (robotics, automotive, industrial) from the KiCad symbol library and writes
Core/src/StandardCatalog.inc.

Pin numbers, names and electrical types come straight from the KiCad symbols (which follow the vendor datasheets);
the package is the orderable variant's (e.g. LM358DR = SOIC-8, LM358N = DIP-8). Parts KiCad does not carry are
written by hand in StandardParts.cpp. Run from the repository root:  python3 tools/fetch_catalog_parts.py
(needs network access to gitlab.com).
"""
import os, re, sys

sys.path.insert(0, os.path.dirname(__file__))
import fetch_mcu_pinouts as kicad  # noqa: E402  (shares the symbol fetching and pin parsing)

# (category, KiCad library, symbol, SiEDA name, manufacturer, description, ref prefix, footprint override)
PARTS = [
    # Main compute
    ("Microcontrollers · Microchip", "MCU_Microchip_ATmega", "ATmega328P-A", "ATMEGA328P-AU", "Microchip",
     "8-bit AVR 20 MHz, 32 KB flash — the Arduino Uno core in TQFP-32", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATmega", "ATmega328P-P", "ATMEGA328P-PU", "Microchip",
     "8-bit AVR 20 MHz, 32 KB flash — through-hole DIP-28", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F4", "STM32F405RGTx", "STM32F405RGT6", "STMicroelectronics",
     "Cortex-M4F 168 MHz, 1 MB flash, FPU — real-time kinematics and flight control", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32H7", "STM32H743IITx", "STM32H743IIT6", "STMicroelectronics",
     "Cortex-M7 480 MHz, double-precision FPU, 2 MB flash — rover / quadruped navigation", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F7", "STM32F765VITx", "STM32F765VIT6", "STMicroelectronics",
     "Cortex-M7 216 MHz, 2 MB flash — Pixhawk-class drone flight controller", "U", ""),
    ("FPGA", "FPGA_Xilinx_Artix7", "XC7A35T-CSG324", "XC7A35T-1CSG324I", "AMD (Xilinx)",
     "Artix-7 FPGA, 33 k logic cells, 324-ball CSBGA (0.8 mm) — encoder / lidar co-processing, safety interlocks", "U",
     "Package_BGA:BGA-324_15x15mm_P0.8mm"),
    # Motion and motor control
    ("Motor Control", "Driver_Motor", "L298HN", "L298HN", "STMicroelectronics",
     "Dual full-bridge driver, 46 V, 2 A per channel, Multiwatt-15", "U", ""),
    ("Motor Control", "Transistor_Array", "ULN2003A", "ULN2003ADR", "Texas Instruments",
     "7-channel Darlington sink array (steppers, relays), SOIC-16", "U", "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"),
    ("Motor Control", "Driver_LED", "PCA9685PW", "PCA9685PW", "NXP",
     "16-channel 12-bit PWM / servo driver, I²C, TSSOP-28", "U", ""),
    ("Motor Control", "Driver_FET", "IR2101", "IR2101S", "Infineon",
     "High- and low-side MOSFET gate driver, 600 V, SOIC-8", "U", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("Motor Control", "Driver_FET", "IR2110S", "IR2110S", "Infineon",
     "High- and low-side gate driver, 500 V, 2 A, SOIC-16W", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC2209-LA", "TMC2209-LA", "Trinamic (ADI)",
     "Silent stepper driver, 2 A, StealthChop2, UART, QFN-28", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC2160", "TMC2160-TA", "Trinamic (ADI)",
     "High-power stepper pre-driver for external MOSFETs, StallGuard2, SPI, TQFP-48", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC5160A-TA", "TMC5160A-TA", "Trinamic (ADI)",
     "Stepper motion controller + pre-driver, SixPoint ramps, SPI / step-dir, TQFP-48", "U", ""),
    # Sensors and signal conditioning
    ("Sensors", "Sensor_Motion", "MPU-9250", "MPU-9250", "TDK InvenSense",
     "9-axis IMU (accelerometer, gyroscope, magnetometer), I²C / SPI, QFN-24 3 × 3 mm", "U", ""),
    ("Sensors", "Analog_ADC", "ADS1115IDGS", "ADS1115IDGS", "Texas Instruments",
     "16-bit 4-channel delta-sigma ADC, PGA, I²C, VSSOP-10", "U", ""),
    ("Op-Amps", "Amplifier_Operational", "LM358", "LM358DR", "Texas Instruments",
     "Dual general-purpose op-amp, SOIC-8", "U", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("Op-Amps", "Amplifier_Operational", "LM358", "LM358N", "Texas Instruments",
     "Dual general-purpose op-amp, DIP-8", "U", "Package_DIP:DIP-8_W7.62mm"),
    ("Op-Amps", "Comparator", "LM393", "LM393DR", "Texas Instruments",
     "Dual differential comparator, open collector, SOIC-8", "U", "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("Op-Amps", "Comparator", "LM393", "LM393N", "Texas Instruments",
     "Dual differential comparator, open collector, DIP-8", "U", "Package_DIP:DIP-8_W7.62mm"),
    ("Sensors", "Sensor_Magnetic", "AS5047D", "AS5047D-ATSM", "ams OSRAM",
     "14-bit magnetic rotary position sensor (ABI / UVW / SPI), joint feedback, TSSOP-14", "U", ""),
    ("Sensors", "Amplifier_Current", "INA240A1D", "INA240A1EDRQ1", "Texas Instruments",
     "Bidirectional current-sense amplifier, −4…80 V common mode, enhanced PWM rejection, gain 20, SOIC-8", "U", ""),
    # Audio
    ("Audio", "Amplifier_Audio", "MAX9814", "MAX9814ETD+T", "Analog Devices (Maxim)",
     "Microphone amplifier with AGC and low-noise bias, TDFN-14", "U", ""),
    # Interface and networking
    ("Logic", "74xx", "74HC595", "74HC595D", "Nexperia",
     "8-bit serial-in shift register with output latches, SOIC-16", "U", "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"),
    ("Logic", "Interface_Expansion", "PCF8574TS", "PCF8574TS", "NXP",
     "8-bit I²C I/O expander (LCDs, keypads), SSOP-20", "U", ""),
    ("Logic", "Interface_Expansion", "PCF8574P", "PCF8574N", "NXP", "8-bit I²C I/O expander, DIP-16", "U", ""),
    ("Interface", "Interface_USB", "CH340G", "CH340G", "WCH",
     "USB to UART bridge (needs a 12 MHz crystal), SOIC-16", "U", ""),
    ("Interface", "Interface_USB", "CP2102N-Axx-xQFN28", "CP2102N-A02-GQFN28", "Silicon Labs",
     "USB to UART bridge, internal oscillator, QFN-28 (current CP2102 successor)", "U", ""),
    ("Interface", "Interface_UART", "MAX485E", "MAX485ESA+T", "Analog Devices (Maxim)",
     "RS-485 / RS-422 half-duplex transceiver, ±15 kV ESD, SOIC-8", "U", ""),
    ("Interface", "Interface_CAN_LIN", "MCP2551-I-SN", "MCP2551-I/SN", "Microchip",
     "High-speed CAN transceiver, 1 Mbit/s, SOIC-8", "U", ""),
    ("Interface", "Interface_UART", "ADM2587E", "ADM2587EBRWZ", "Analog Devices",
     "Isolated RS-485 transceiver with integrated isoPower DC-DC, 2.5 kV, SOIC-20W", "U", ""),
    # Power
    ("Regulators", "Regulator_Linear", "L7812", "LM7812", "STMicroelectronics",
     "12 V 1.5 A positive linear regulator, TO-220", "U", "Package_TO_SOT_THT:TO-220-3_Vertical"),
    ("Regulators", "Regulator_Linear", "AMS1117-3.3", "AMS1117-3.3", "Advanced Monolithic Systems",
     "3.3 V 1 A low-dropout regulator, SOT-223", "U", ""),
    ("Regulators", "Regulator_Switching", "LM2596S-5", "LM2596S-5.0", "Texas Instruments",
     "5 V 3 A step-down (buck) regulator, 150 kHz, TO-263-5", "U", ""),
    ("Regulators", "Regulator_Switching", "XL4015", "XL4015E1", "XLSEMI",
     "5 A 180 kHz step-down (buck) converter, 8…36 V input, TO-263-5", "U", ""),
    ("Power", "Power_Management", "LM74700", "LM74700QDBVRQ1", "Texas Instruments",
     "Automotive ideal-diode controller (reverse-battery protection), SOT-23-6", "U", ""),
    ("Power", "Power_Management", "LTC4359-MS8", "LTC4359IMS8#PBF", "Analog Devices",
     "Ideal-diode controller, 4…80 V (dual-battery OR-ing without diode drop), MSOP-8", "U", ""),
    # Closest parts KiCad carries for catalog entries it does not (BMX160, INMP441, ATSAMD51P20A).
    ("Sensors", "Sensor_Motion", "BMI160", "BMI160", "Bosch Sensortec",
     "6-axis IMU (accelerometer + gyroscope), I²C / SPI — the BMX160's IMU without the magnetometer, LGA-14", "U",
     "Package_DFN_QFN:DFN-14_2.5x3mm_P0.5mm"),
    ("Audio", "Sensor_Audio", "ICS-43434", "ICS-43434", "TDK InvenSense",
     "Digital I²S MEMS microphone (INMP441 successor), LGA-6", "MK", "Package_DFN_QFN:DFN-6_2.65x3.5mm_P1.27mm"),
    ("Microcontrollers · Microchip", "MCU_Microchip_SAMD", "ATSAMD51J20A-A", "ATSAMD51J20A-AU", "Microchip",
     "Cortex-M4F 120 MHz, 1 MB flash, FPU (64-pin SAM D51 for CNC / 3D-printer motion engines), TQFP-64", "U", ""),
]


def package(footprint):
    """Extends the MCU package mapping with BGA, TO-263, SOT-223, SOT-23-6, staggered TO-220 and DFN / SON / MSOP."""
    fp = footprint.split(":")[-1]
    u = fp.upper()
    pitch = re.search(r"_P([\d.]+)mm", fp)
    body = re.search(r"_([\d.]+)x([\d.]+)(?:x[\d.]+)?mm", fp)
    if "BGA" in u:
        n = int(re.search(r"BGA-(\d+)", u).group(1))
        return "BGA", n, float(pitch.group(1)) if pitch else 0.8, float(body.group(1)) if body else 0, fp
    if "TO-263" in u:
        return "TO263", int(re.search(r"TO-263-(\d+)", u).group(1)), 0, 0, fp
    if "SOT-223" in u:
        return "SOT223", 3, 0, 0, fp
    if "SOT-23" in u:
        return "SOT23", int(re.search(r"SOT-23-(\d+)", u).group(1)), 0, 0, fp
    if "TO-220-15" in u:
        return "MULTIWATT", 15, 1.27, 0, fp
    if "TO-220-3" in u:
        return "TO220", 3, 0, 0, fp
    if re.search(r"(^|_)(T|V|W)?DFN|TDSON|VSON", u):
        n = int(re.search(r"(?:DFN|SON)-(\d+)", u).group(1))
        return "SON", n, float(pitch.group(1)) if pitch else 1.27, float(body.group(1)) if body else 0, fp
    if "MSOP" in u:
        return "TSSOP", int(re.search(r"MSOP-(\d+)", u).group(1)), float(pitch.group(1)), float(body.group(1)), fp
    if "INVENSENSE_QFN" in u or "LGA" in u:
        n = int(re.search(r"(?:QFN|LGA)-(\d+)", u).group(1))
        return "QFN", n, float(pitch.group(1)) if pitch else 0.5, float(body.group(1)) if body else 0, fp
    if "SOIC-" in u and "W_" in u:  # wide-body SOIC (7.5 mm)
        n = int(re.search(r"SOIC-(\d+)W", u).group(1))
        return "SOIC", n, 1.27, float(body.group(1)), fp
    return kicad.package(footprint)


def cpp(s):
    return kicad.cpp_string(s)


def main():
    out = ["// Generated by tools/fetch_catalog_parts.py from the KiCad symbol library (pin numbers, names and",
           "// electrical types as published there, following the vendor datasheets). Do not edit by hand.", ""]
    summary = []
    for category, lib, sym, name, maker, desc, ref, override in PARTS:
        found, footprint = kicad.pins(lib, sym)
        footprint = override or footprint
        if not footprint: raise SystemExit(f"{sym}: no footprint")
        kind, n, pitch, body, fpname = package(footprint)
        if kind == "BGA":
            numbers = sorted(found, key=lambda b: (len(b.rstrip("0123456789")), b.rstrip("0123456789"),
                                                   int(b[len(b.rstrip("0123456789")):] or 0)))
            pin_list = [(k,) + found[k][:2] for k in numbers]
        else:
            for k in found:
                if not k.isdigit(): raise SystemExit(f"{sym}: non-numeric pin {k}")
            top = max(int(k) for k in found)
            pin_list = []
            for i in range(1, max(top, n) + 1):
                nm, t = (found[str(i)][0], found[str(i)][1]) if str(i) in found else ("NC", "NoConnect")
                pin_list.append(("EP" if i == n + 1 else str(i), nm, t))
        out.append(f"catalog(parts, {cpp(category)}, {cpp(name)}, {cpp(maker)}, {cpp(desc)}, {cpp(ref)},")
        out.append(f"    {cpp(kind)}, {n}, {pitch:g}, {body:g}, {cpp(fpname)},")
        out.append("    {" + ", ".join("{%s, %s, T::%s}" % (cpp(num), cpp(nm), t) for num, nm, t in pin_list) + "});")
        summary.append(f"{name:20} {kind:9} n={n:3} pins={len(pin_list):3} {fpname}")
    open("Core/src/StandardCatalog.inc", "w").write("\n".join(out) + "\n")
    print("\n".join(summary))


if __name__ == "__main__":
    main()
