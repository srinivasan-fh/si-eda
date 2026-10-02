#!/usr/bin/env python3
"""Fetches microcontroller pinouts and footprints from the KiCad symbol library and writes
Core/src/StandardMcus.inc (the "Microcontrollers" parts of the standard library).

The pin numbers, names and electrical types come straight from the KiCad symbols (which follow the vendor
datasheets); the package (type, pins, pitch, body) is parsed from each symbol's Footprint property.
Run from the repository root:  python3 tools/fetch_mcu_pinouts.py   (needs network access to gitlab.com).
"""
import json, os, re, sys, urllib.parse, urllib.request

API = "https://gitlab.com/api/v4/projects/kicad%2Flibraries%2Fkicad-symbols/repository/files/{}/raw?ref=master"
CACHE = os.environ.get("KICAD_CACHE", "/tmp/kicad-symbol-cache")

# (vendor group, KiCad library, symbol, SiEDA name, manufacturer, description)
PARTS = [
    # ARM Cortex-M from other vendors
    ("Arm", "MCU_RaspberryPi", "RP2040", "RP2040", "Raspberry Pi", "Dual Cortex-M0+ 133 MHz, 264 KB SRAM, PIO, USB 1.1, external QSPI flash"),
    ("Arm", "MCU_RaspberryPi", "RP2350A", "RP2350A", "Raspberry Pi", "Dual Cortex-M33 / Hazard3 RISC-V 150 MHz, 520 KB SRAM, secure boot, PIO, USB"),
    ("Arm", "MCU_Nordic", "nRF52832-QFxx", "nRF52832", "Nordic Semiconductor", "Cortex-M4F 64 MHz, Bluetooth LE / 2.4 GHz radio, 512 KB flash, NFC"),
    ("Arm", "MCU_Nordic", "nRF52810-QFxx", "nRF52810", "Nordic Semiconductor", "Cortex-M4 64 MHz, Bluetooth LE radio, 192 KB flash"),
    ("Arm", "MCU_Nordic", "nRF51x22-QFxx", "nRF51822", "Nordic Semiconductor", "Cortex-M0 16 MHz, Bluetooth LE / 2.4 GHz radio, 256 KB flash"),
    ("Arm", "MCU_NXP_LPC", "LPC1768FBD100", "LPC1768", "NXP", "Cortex-M3 100 MHz, 512 KB flash, Ethernet MAC, USB, CAN (mbed LPC1768)"),
    ("Arm", "MCU_NXP_LPC", "LPC1114FBD48-302", "LPC1114FBD48", "NXP", "Cortex-M0 50 MHz, 32 KB flash, UART/SPI/I2C"),
    ("Arm", "MCU_NXP_Kinetis", "MKL25Z128VLK4", "MKL25Z128VLK4", "NXP", "Kinetis L Cortex-M0+ 48 MHz, 128 KB flash, USB OTG, capacitive touch (FRDM-KL25Z)"),
    ("Arm", "MCU_Cypress", "CY8C4245AXI-M445", "CY8C4245AXI", "Infineon (Cypress)", "PSoC 4 Cortex-M0 48 MHz, 32 KB flash, programmable analog/digital blocks, CapSense, 64-TQFP"),
    ("Arm", "MCU_SiliconLabs", "EFM32HG308F64G-C-QFN24", "EFM32HG308F64", "Silicon Labs", "Happy Gecko Cortex-M0+ 25 MHz, 64 KB flash, crystal-less USB, ultra low power"),
    # Microchip (ATmega328P and ATtiny85 are already in the library)
    ("Microchip", "MCU_Microchip_ATmega", "ATmega2560-16A", "ATmega2560", "Microchip", "8-bit AVR 16 MHz, 256 KB flash, 86 I/O, 4 USARTs (Arduino Mega)"),
    ("Microchip", "MCU_Microchip_ATmega", "ATmega32U4-A", "ATmega32U4", "Microchip", "8-bit AVR 16 MHz, 32 KB flash, native USB (Arduino Leonardo / Micro)"),
    ("Microchip", "MCU_Microchip_ATmega", "ATmega4809-A", "ATmega4809", "Microchip", "8-bit AVR 20 MHz, 48 KB flash, UPDI, core-independent peripherals (Arduino Nano Every)"),
    ("Microchip", "MCU_Microchip_ATtiny", "ATtiny1614-SS", "ATtiny1614", "Microchip", "tinyAVR 1-series 20 MHz, 16 KB flash, UPDI, event system"),
    ("Microchip", "MCU_Microchip_SAMD", "ATSAMD21G18A-A", "ATSAMD21G18A", "Microchip", "Cortex-M0+ 48 MHz, 256 KB flash, USB device/host (Arduino Zero, Feather M0)"),
    ("Microchip", "MCU_Microchip_SAMD", "ATSAMD21E18A-A", "ATSAMD21E18A", "Microchip", "Cortex-M0+ 48 MHz, 256 KB flash, USB, 32-pin (Trinket M0, QT Py)"),
    ("Microchip", "MCU_Microchip_PIC16", "PIC16F877A-IP", "PIC16F877A", "Microchip", "8-bit PIC 20 MHz, 14 KB flash, 33 I/O, 10-bit ADC"),
    ("Microchip", "MCU_Microchip_PIC18", "PIC18F4550-IP", "PIC18F4550", "Microchip", "8-bit PIC18 48 MHz, 32 KB flash, full-speed USB 2.0"),
    ("Microchip", "MCU_Microchip_PIC12", "PIC12F675-xP", "PIC12F675", "Microchip", "8-bit PIC 20 MHz, 1 K-word flash, 6 I/O, 10-bit ADC"),
    ("Microchip", "MCU_Microchip_PIC32", "PIC32MX250F128D-IPT", "PIC32MX250F128D", "Microchip", "32-bit MIPS M4K 50 MHz, 128 KB flash, USB OTG"),
    # STMicroelectronics
    ("STMicroelectronics", "MCU_ST_STM32F1", "STM32F103C8Tx", "STM32F103C8T6", "STMicroelectronics", "Cortex-M3 72 MHz, 64 KB flash, USB, CAN (Blue Pill)"),
    ("STMicroelectronics", "MCU_ST_STM32F0", "STM32F030F4Px", "STM32F030F4P6", "STMicroelectronics", "Cortex-M0 48 MHz, 16 KB flash, low-cost 20-pin"),
    ("STMicroelectronics", "MCU_ST_STM32F0", "STM32F042K6Tx", "STM32F042K6T6", "STMicroelectronics", "Cortex-M0 48 MHz, 32 KB flash, crystal-less USB, CAN"),
    ("STMicroelectronics", "MCU_ST_STM32F4", "STM32F401CCUx", "STM32F401CCU6", "STMicroelectronics", "Cortex-M4F 84 MHz, 256 KB flash, USB OTG FS (Black Pill)"),
    ("STMicroelectronics", "MCU_ST_STM32F4", "STM32F411CEUx", "STM32F411CEU6", "STMicroelectronics", "Cortex-M4F 100 MHz, 512 KB flash, USB OTG FS (Black Pill)"),
    ("STMicroelectronics", "MCU_ST_STM32F4", "STM32F407VGTx", "STM32F407VGT6", "STMicroelectronics", "Cortex-M4F 168 MHz, 1 MB flash, Ethernet, USB OTG HS/FS, camera interface"),
    ("STMicroelectronics", "MCU_ST_STM32G0", "STM32G030F6Px", "STM32G030F6P6", "STMicroelectronics", "Cortex-M0+ 64 MHz, 32 KB flash, 20-pin"),
    ("STMicroelectronics", "MCU_ST_STM32G4", "STM32G431CBUx", "STM32G431CBU6", "STMicroelectronics", "Cortex-M4F 170 MHz, 128 KB flash, math accelerators, motor-control timers, op-amps"),
    ("STMicroelectronics", "MCU_ST_STM32L4", "STM32L432KCUx", "STM32L432KCU6", "STMicroelectronics", "Ultra-low-power Cortex-M4F 80 MHz, 256 KB flash, USB (Nucleo-L432KC)"),
    ("STMicroelectronics", "MCU_ST_STM32H7", "STM32H743VITx", "STM32H743VIT6", "STMicroelectronics", "Cortex-M7 480 MHz, 2 MB flash, 1 MB RAM, Ethernet, USB HS"),
    # Texas Instruments
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430G2553IN20", "MSP430G2553", "Texas Instruments", "16-bit MSP430 16 MHz, 16 KB flash, ultra-low power (LaunchPad)"),
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430G2452IN20", "MSP430G2452", "Texas Instruments", "16-bit MSP430 16 MHz, 8 KB flash, comparator, 10-bit ADC"),
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430F2013IN", "MSP430F2013", "Texas Instruments", "16-bit MSP430 16 MHz, 2 KB flash, 16-bit sigma-delta ADC"),
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430F2274IDA", "MSP430F2274", "Texas Instruments", "16-bit MSP430 16 MHz, 32 KB flash, op-amps, 10-bit ADC"),
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430FR5738IRGE", "MSP430FR5738", "Texas Instruments", "16-bit MSP430 24 MHz, 16 KB FRAM, ultra-low power"),
    ("Texas Instruments", "MCU_Texas_MSP430", "MSP430F5510IRGZ", "MSP430F5510", "Texas Instruments", "16-bit MSP430 25 MHz, 32 KB flash, full-speed USB"),
    ("Texas Instruments", "MCU_Texas", "TM4C1231H6PM", "TM4C1231H6PM", "Texas Instruments", "Tiva C Cortex-M4F 80 MHz, 256 KB flash, CAN, 12-bit ADC"),
    ("Texas Instruments", "MCU_Texas", "MSP432E401Y", "MSP432E401Y", "Texas Instruments", "SimpleLink Cortex-M4F 120 MHz, 1 MB flash, Ethernet MAC+PHY, USB, CAN"),
    ("Texas Instruments", "MCU_Texas_SimpleLink", "CC1312R1F3RGZ", "CC1312R1", "Texas Instruments", "SimpleLink Sub-1 GHz wireless Cortex-M4F 48 MHz, 352 KB flash"),
    ("Texas Instruments", "MCU_Texas_MSP430", "CC430F5137xRGZ", "CC430F5137", "Texas Instruments", "MSP430 + Sub-1 GHz CC1101 radio, 32 KB flash"),
]

# Symbols whose library entry names no footprint (PDIP parts): the package from the datasheet.
FOOTPRINT_OVERRIDE = {
    "PIC16F877A-IP": "Package_DIP:DIP-40_W15.24mm",
    "PIC18F4550-IP": "Package_DIP:DIP-40_W15.24mm",
    "PIC12F675-xP": "Package_DIP:DIP-8_W7.62mm",
    "MSP430G2553IN20": "Package_DIP:DIP-20_W7.62mm",
    "MSP430G2452IN20": "Package_DIP:DIP-20_W7.62mm",
    "MSP430F2013IN": "Package_DIP:DIP-14_W7.62mm",
}

TYPE = {"input": "Input", "output": "Output", "bidirectional": "Bidirectional", "tri_state": "Bidirectional",
        "passive": "Passive", "free": "Passive", "unspecified": "Passive", "power_in": "PowerIn",
        "power_out": "PowerOut", "open_collector": "OpenCollector", "open_emitter": "OpenCollector",
        "no_connect": "NoConnect"}


def fetch(lib, sym):
    os.makedirs(CACHE, exist_ok=True)
    path = os.path.join(CACHE, f"{lib}__{sym}.kicad_sym")
    if not os.path.exists(path):
        url = API.format(urllib.parse.quote(f"{lib}.kicad_symdir/{sym}.kicad_sym", safe=""))
        with urllib.request.urlopen(url, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    return open(path, encoding="utf-8").read()


def prop(text, key):
    m = re.search(r'\(property "%s" "((?:[^"\\]|\\.)*)"' % re.escape(key), text)
    return m.group(1) if m else ""


def pins(lib, sym):
    text = fetch(lib, sym)
    parent = re.search(r'\(extends "([^"]+)"\)', text)
    footprint = prop(text, "Footprint")
    if parent:  # derived symbol: pins come from the parent
        base, base_fp = pins(lib, parent.group(1))
        return base, footprint or base_fp
    found = {}
    for m in re.finditer(r'\(pin (\w+) \w+\s*\(at [^)]*\)\s*\(length [^)]*\)(.*?)\(name "((?:[^"\\]|\\.)*)".*?\(number "([^"]+)"',
                         text, re.S):
        etype, extra, name, number = m.group(1), m.group(2), m.group(3), m.group(4)
        hidden = "hide" in extra
        name = re.sub(r"~\{([^}]*)\}", r"n\1", name).replace("~", "").strip() or f"P{number}"
        if number not in found or (found[number][2] and not hidden):
            found[number] = (name, TYPE.get(etype, "Passive"), hidden)
    # Stacked duplicates of a supply pin are "passive" in KiCad: they are the same supply.
    power = {v[0] for v in found.values() if v[1] in ("PowerIn", "PowerOut")}
    for k, (nm, t, h) in list(found.items()):
        if nm in power and t == "Passive":
            found[k] = (nm, "PowerIn", h)
    return found, footprint


def package(footprint):
    """KiCad footprint name → (type, pins, pitch mm, body mm)."""
    fp = footprint.split(":")[-1]
    pitch = re.search(r"_P([\d.]+)mm", fp)
    body = re.search(r"_([\d.]+)x([\d.]+)(?:x[\d.]+)?mm", fp)
    count = re.search(r"-(\d+)(?:-\d+EP|_|$)", fp)
    u = fp.upper()
    if "QFN" in u or "DFN" in u: kind = "QFN"
    elif "QFP" in u: kind = "LQFP"
    elif "TSSOP" in u or "SSOP" in u: kind = "TSSOP"
    elif "SOIC" in u or "SO-" in u: kind = "SOIC"
    elif "DIP" in u: kind = "DIP"
    else: raise SystemExit(f"unknown package {footprint}")
    n = int(count.group(1))
    p = float(pitch.group(1)) if pitch else (2.54 if kind == "DIP" else 0)
    b = 0.0
    if kind in ("QFN", "LQFP") and body: b = float(body.group(1))
    if kind == "DIP":
        w = re.search(r"W([\d.]+)mm", fp)
        b = float(w.group(1)) if w else 7.62
    if kind in ("TSSOP", "SOIC") and body: b = float(body.group(1))
    return kind, n, p, b, fp


def cpp_string(s):
    return '"' + s.replace("\\", "\\\\").replace('"', '\\"') + '"'


def main():
    out = ["// Generated by tools/fetch_mcu_pinouts.py from the KiCad symbol library (pin numbers, names and electrical",
           "// types as published there, following the vendor datasheets). Do not edit by hand.", ""]
    summary = []
    for group, lib, sym, name, maker, desc in PARTS:
        found, footprint = pins(lib, sym)
        if not footprint: footprint = FOOTPRINT_OVERRIDE.get(sym, "")
        if not footprint: raise SystemExit(f"{sym}: no footprint")
        kind, n, pitch, body, fpname = package(footprint)
        numbers = sorted(found, key=lambda x: (not x.isdigit(), int(x) if x.isdigit() else 0, x))
        for k in numbers:
            if not k.isdigit():
                raise SystemExit(f"{sym}: non-numeric pin {k}")
        maxnum = max(int(k) for k in numbers)
        exposed = maxnum == n + 1
        missing = [i for i in range(1, n + 1) if str(i) not in found]
        pin_list = []
        for i in range(1, maxnum + 1):
            key = str(i)
            if key in found:
                nm, t, _ = found[key]
                pin_list.append((nm, t))
            else:
                pin_list.append(("NC", "NoConnect"))
        body_txt = f"{body:g}" if body else "0"
        out.append(f"mcu(parts, {cpp_string(group)}, {cpp_string(name)}, {cpp_string(maker)}, {cpp_string(desc)},")
        out.append(f"    {cpp_string(kind)}, {n}, {pitch:g}, {body_txt}, {cpp_string(fpname)},")
        items = ", ".join("{%s, T::%s}" % (cpp_string(nm), t) for nm, t in pin_list)
        out.append("    {" + items + "});")
        summary.append(f"{group[:9]:9} {name:18} {fpname:45} pins={len(pin_list)} ep={exposed} missing={missing}")
    open("Core/src/StandardMcus.inc", "w").write("\n".join(out) + "\n")
    print("\n".join(summary))


if __name__ == "__main__":
    main()
