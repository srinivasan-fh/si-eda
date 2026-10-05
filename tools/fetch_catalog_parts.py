#!/usr/bin/env python3
"""Fetches the production parts catalog (robotics, automotive, industrial) from the KiCad symbol library and writes
Core/src/StandardCatalog.inc.

Pin numbers, names and electrical types come straight from the KiCad symbols (which follow the vendor datasheets);
the package is the orderable variant's (e.g. LM358DR = SOIC-8, LM358N = DIP-8). Parts KiCad does not carry are
written by hand in StandardParts.cpp. Run from the repository root:  python3 tools/fetch_catalog_parts.py
(needs network access to gitlab.com).
"""
import os, re, sys, urllib.parse, urllib.request

sys.path.insert(0, os.path.dirname(__file__))
import fetch_mcu_pinouts as kicad  # noqa: E402  (shares the symbol fetching and pin parsing)

# Footprints for symbols that name none (generic op-amps, logic): the orderable variant's package.
SOIC8 = "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"
DIP8 = "Package_DIP:DIP-8_W7.62mm"
SOIC14 = "Package_SO:SOIC-14_3.9x8.7mm_P1.27mm"
SOIC16 = "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"
SOT23 = "Package_TO_SOT_SMD:SOT-23"
SOT235 = "Package_TO_SOT_SMD:SOT-23-5"
TO220 = "Package_TO_SOT_THT:TO-220-3_Vertical"
# SOIC-8 body with an exposed thermal pad (TI DDA PowerPAD, Allegro LJ): drawn as an 8-lead package with its pad.
HSOP8 = "Package_SO:HSOP-8-1EP_3.9x4.9mm_P1.27mm_EP2.41x3.1mm"

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
    # ---------------------------------------------------------------- Robotics spares (rover, drone, quadruped,
    # humanoid, arm manipulator, 3D printer, CNC): production-grade parts on their orderable package.
    # Compute
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F4", "STM32F446RETx", "STM32F446RET6", "STMicroelectronics",
     "Cortex-M4F 180 MHz, 512 KB flash, dual CAN — 3D-printer mainboards (Klipper / Marlin), drone flight controllers",
     "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32G4", "STM32G474RETx", "STM32G474RET6", "STMicroelectronics",
     "Cortex-M4F 170 MHz, HRTIM, 5 ADCs, CORDIC / FMAC, FD-CAN — FOC joint controllers for legged robots and arms",
     "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32H7", "STM32H723VGTx", "STM32H723VGT6", "STMicroelectronics",
     "Cortex-M7 550 MHz, 1 MB flash, FD-CAN, Ethernet — CNC motion controllers, 3D-printer boards (Octopus Pro class)",
     "U", ""),
    ("Microcontrollers · Arm", "MCU_NXP_LPC", "LPC1769FBD100", "LPC1769FBD100", "NXP",
     "Cortex-M3 120 MHz, 512 KB flash, Ethernet, CAN — Smoothieboard / SKR-class CNC and 3D-printer controllers", "U", ""),
    # Motion: brushed DC
    ("Motor Control", "Driver_Motor", "DRV8833PWP", "DRV8833PWPR", "Texas Instruments",
     "Dual H-bridge 1.5 A (2 A peak), 2.7…10.8 V, small rovers and grippers, HTSSOP-16", "U", ""),
    ("Motor Control", "Driver_Motor", "DRV8871DDA", "DRV8871DDAR", "Texas Instruments",
     "Brushed DC H-bridge 3.6 A peak, 6.5…45 V, internal current regulation, HSOP-8", "U", ""),
    ("Motor Control", "Driver_Motor", "TB6612FNG", "TB6612FNG", "Toshiba",
     "Dual H-bridge 1.2 A (3.2 A peak), 15 V — rover drive wheels, SSOP-24", "U", ""),
    ("Motor Control", "Driver_Motor", "VNH5019A-E", "VNH5019A-E", "STMicroelectronics",
     "Automotive full H-bridge 30 A, 41 V, current-sense output — rover / tracked platform drive motors, MultiPowerSO-30",
     "U", ""),
    # Motion: BLDC / FOC
    ("Motor Control", "Driver_Motor", "STSPIN32F0A", "STSPIN32F0A", "STMicroelectronics",
     "BLDC controller: Cortex-M0 + 3-phase gate driver + op-amps — drone ESCs, joint motors, VFQFPN-48", "U", ""),
    ("Motor Control", "Driver_Motor", "DRV8308", "DRV8308RHAR", "Texas Instruments",
     "Brushless DC motor controller, sensored, closed-loop speed control, VQFN-40", "U", ""),
    ("Motor Control", "Driver_Motor", "DRV8311H", "DRV8311HRRWR", "Texas Instruments",
     "Three-phase PWM motor driver 5 A peak, 3…20 V — gimbals, small joint actuators, WQFN-24", "U", ""),
    # Motion: steppers (3D printers, CNC, arm joints)
    ("Motor Control", "Driver_Motor", "TMC2130-LA", "TMC2130-LA", "Trinamic (ADI)",
     "Stepper driver 1.2 A RMS, SPI, StallGuard2 sensorless homing, StealthChop, QFN-36", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC2208-LA", "TMC2208-LA", "Trinamic (ADI)",
     "Silent stepper driver 1.4 A RMS, UART, StealthChop2 — 3D-printer axes, QFN-28", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC2226-SA", "TMC2226-SA", "Trinamic (ADI)",
     "Stepper driver 2 A RMS, UART, StallGuard4, CoolStep, HTSSOP-28 (TMC2209 in a cooler package)", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC2660", "TMC2660-PA", "Trinamic (ADI)",
     "Stepper driver 2.8 A RMS, 30 V, SPI, StallGuard2 — CNC routers and large-format printers, TQFP-44", "U", ""),
    ("Motor Control", "Driver_Motor", "TMC5130A-TA", "TMC5130A-TA", "Trinamic (ADI)",
     "Stepper motion controller + driver 2 A, SixPoint ramps, encoder input, SPI, TQFP-48", "U", ""),
    ("Motor Control", "Driver_Motor", "DRV8434PWP", "DRV8434PWPR", "Texas Instruments",
     "Stepper driver 2.5 A full-scale, 48 V, integrated current sensing, stall detection, HTSSOP-28", "U", ""),
    ("Motor Control", "Driver_Motor", "STSPIN220", "STSPIN220", "STMicroelectronics",
     "Low-voltage stepper driver 1.3 A RMS, 1.8…10 V, 1/256 microstep — battery robots, VFQFPN-16", "U", ""),
    ("Motor Control", "Driver_Motor", "Pololu_Breakout_A4988", "A4988-STEPSTICK", "Allegro (Pololu carrier)",
     "A4988 StepStick carrier, 2 A, 1/16 microstep — the plug-in stepper driver of RAMPS / CNC shields", "U", ""),
    ("Motor Control", "Driver_Motor", "Pololu_Breakout_DRV8825", "DRV8825-STEPSTICK", "Texas Instruments (Pololu carrier)",
     "DRV8825 StepStick carrier, 2.2 A, 45 V, 1/32 microstep — CNC shields and 3D-printer boards", "U", ""),
    # Gate drivers and power stage
    ("Motor Control", "Driver_FET", "IR2184", "IR2184STRPBF", "Infineon",
     "Half-bridge gate driver 600 V, 1.9 / 2.3 A, single PWM input with dead time, SOIC-8", "U",
     "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("Motor Control", "Driver_FET", "UCC27524D", "UCC27524DR", "Texas Instruments",
     "Dual low-side gate driver 5 A, 4.5…18 V — brushless joint drives, SOIC-8", "U", ""),
    ("Motor Control", "Driver_FET", "LM5109BMA", "LM5109BMAX/NOPB", "Texas Instruments",
     "100 V 1 A high- and low-side gate driver — 48 V arm joints and quadruped legs, SOIC-8", "U", ""),
    ("Power Electronics", "Transistor_FET", "BSC028N06LS3", "BSC028N06LS3G", "Infineon",
     "N-MOSFET 60 V 100 A, 2.8 mΩ, logic level — ESC / joint bridge, TDSON-8 (SuperSO8)", "Q", ""),
    ("Power Electronics", "Transistor_FET", "BSC040N10NS5", "BSC040N10NS5", "Infineon",
     "N-MOSFET 100 V 100 A, 4 mΩ — 48 V servo drives, TDSON-8 (SuperSO8)", "Q", ""),
    ("Power Electronics", "Transistor_FET", "IRLZ44N", "IRLZ44NPBF", "Infineon",
     "N-MOSFET 55 V 47 A, logic level — heated bed, spindle and solenoid switches, TO-220", "Q", ""),
    ("Power Electronics", "Transistor_FET", "IRF3205", "IRF3205PBF", "Infineon",
     "N-MOSFET 55 V 110 A, 8 mΩ — high-current rover motor switching, TO-220", "Q", ""),
    ("Power Electronics", "Transistor_FET", "AO3400A", "AO3400A", "Alpha & Omega",
     "N-MOSFET 30 V 5.7 A, logic level — fans, LEDs, small loads, SOT-23", "Q", ""),
    ("Power Electronics", "Transistor_FET", "BSS138", "BSS138", "onsemi",
     "N-MOSFET 50 V 0.22 A — I²C level shifting, signal switching, SOT-23", "Q", ""),
    # Current and power sensing
    ("Sensors", "Sensor_Current", "ACS712xLCTR-20A", "ACS712ELCTR-20A-T", "Allegro",
     "Hall-effect current sensor ±20 A, 2.1 kV isolation — motor and battery current, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Current", "ACS723xLCTR-20AB", "ACS723LLCTR-20AB-T", "Allegro",
     "Hall-effect current sensor ±20 A, 80 kHz, 2.4 kV isolation, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Energy", "INA219AxD", "INA219AID", "Texas Instruments",
     "Current / voltage / power monitor 26 V, I²C — battery telemetry, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Energy", "INA226", "INA226AIDGSR", "Texas Instruments",
     "Current / power monitor 36 V, 16-bit, I²C, alert pin, VSSOP-10", "U", ""),
    ("Sensors", "Power_Management", "INA3221", "INA3221AIRGVR", "Texas Instruments",
     "Triple-channel current / voltage monitor 26 V, I²C — per-leg or per-rail telemetry, VQFN-16", "U", ""),
    ("Sensors", "Amplifier_Current", "INA181", "INA181A2IDBVR", "Texas Instruments",
     "Bidirectional current-sense amplifier, gain 50, −0.2…26 V — low-side shunts, SOT-23-6", "U", ""),
    # Inertial, pressure, magnetic
    ("Sensors", "Sensor_Motion", "ICM-20948", "ICM-20948", "TDK InvenSense",
     "9-axis IMU with AK09916 magnetometer, DMP, I²C / SPI — rover heading, humanoid balance, QFN-24 3 × 3 mm", "U", ""),
    ("Sensors", "Sensor_Motion", "ICM-20602", "ICM-20602", "TDK InvenSense",
     "6-axis IMU, low noise, 1 kHz ODR, SPI — flight-controller gyro, LGA-14 3 × 3 mm", "U", ""),
    ("Sensors", "Sensor_Motion", "BMI088", "BMI088", "Bosch Sensortec",
     "6-axis IMU built for drones: vibration-robust accelerometer ±24 g, gyro ±2000 °/s, LGA-16", "U", ""),
    ("Sensors", "Sensor_Motion", "ISM330DHCX", "ISM330DHCXTR", "STMicroelectronics",
     "Industrial 6-axis IMU, −40…105 °C, machine-learning core — arm and CNC vibration monitoring, LGA-14", "U", ""),
    ("Sensors", "Sensor_Motion", "IIM-42652", "IIM-42652", "TDK InvenSense",
     "Industrial 6-axis IMU, −40…105 °C, low drift — AGVs and legged robots, LGA-14", "U", ""),
    ("Sensors", "Sensor_Motion", "BNO055", "BNO055", "Bosch Sensortec",
     "9-axis absolute orientation sensor with on-chip fusion (quaternions out), I²C / UART, LGA-28", "U", ""),
    ("Sensors", "Sensor_Pressure", "BMP280", "BMP280", "Bosch Sensortec",
     "Barometric pressure sensor ±0.12 hPa (±1 m), I²C / SPI — drone altitude hold, LGA-8", "U", ""),
    ("Sensors", "Sensor_Pressure", "MS5611-01BA", "MS5611-01BA03", "TE Connectivity",
     "High-resolution barometric altimeter 10 cm, SPI / I²C — Pixhawk-class flight controllers, QFN-8 5 × 3 mm", "U", ""),
    ("Sensors", "Sensor_Pressure", "LPS22HB", "LPS22HBTR", "STMicroelectronics",
     "Barometric pressure sensor 260…1260 hPa, I²C / SPI, HLGA-10", "U", ""),
    ("Sensors", "Sensor_Magnetic", "LIS3MDL", "LIS3MDLTR", "STMicroelectronics",
     "3-axis magnetometer ±4…16 gauss, I²C / SPI — drone and rover compass, LGA-12", "U", ""),
    ("Sensors", "Sensor_Magnetic", "AS5048A", "AS5048A-HTSP", "ams OSRAM",
     "14-bit magnetic rotary encoder, SPI — joint absolute position (arms, quadrupeds, humanoids), TSSOP-14", "U", ""),
    ("Sensors", "Sensor_Magnetic", "MA730", "MA730GQ", "Monolithic Power Systems",
     "14-bit magnetic angle sensor, SPI / ABZ / UVW — off-axis joint encoders, QFN-16 3 × 3 mm", "U", ""),
    ("Sensors", "Sensor_Magnetic", "TLE5012B", "TLE5012BE1000", "Infineon",
     "15-bit GMR angle sensor, SSC / IIF / PWM, automotive grade — servo and wheel angle, DSO-8", "U", ""),
    ("Sensors", "Sensor_Magnetic", "MT6701CT", "MT6701CT-STD", "MagnTek",
     "14-bit magnetic angle sensor, SSI / I²C / ABZ / UVW — low-cost joint encoders, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Magnetic", "TMAG5273x1QDBV", "TMAG5273A1QDBVR", "Texas Instruments",
     "3-D linear Hall sensor with angle calculation, I²C — gripper and joystick position, SOT-23-6", "U", ""),
    # Distance, temperature, environment
    ("Sensors", "Sensor_Distance", "VL53L1CXV0FY1", "VL53L1CXV0FY/1", "STMicroelectronics",
     "Time-of-flight distance sensor to 4 m, I²C — rover obstacle detection, drone landing, optical LGA-12", "U", ""),
    ("Sensors", "Sensor_Distance", "VL53L0CXV0DH1", "VL53L0CXV0DH/1", "STMicroelectronics",
     "Time-of-flight distance sensor to 2 m, I²C — cliff and proximity sensing, optical LGA-12", "U", ""),
    ("Sensors", "Sensor_Temperature", "MAX31865xAP", "MAX31865AAP+T", "Analog Devices (Maxim)",
     "PT100 / PT1000 RTD-to-digital converter, SPI — hot-end and heated-bed temperature, SSOP-20", "U", ""),
    ("Sensors", "Sensor_Temperature", "MAX31856", "MAX31856MUD+T", "Analog Devices (Maxim)",
     "Thermocouple-to-digital converter, any type (K / J / N / T…), SPI — high-temperature hot ends, TSSOP-14", "U", ""),
    ("Sensors", "Sensor_Temperature", "AD8495", "AD8495ARMZ", "Analog Devices",
     "K-type thermocouple amplifier with cold-junction compensation, 5 mV/°C analog out, MSOP-8", "U", ""),
    ("Sensors", "Sensor_Humidity", "SHT31-DIS", "SHT31-DIS-B", "Sensirion",
     "Humidity and temperature sensor ±2 % RH, I²C — enclosure / filament-dryer monitoring, DFN-8", "U", ""),
    ("Sensors", "Sensor", "BME280", "BME280", "Bosch Sensortec",
     "Humidity, pressure and temperature sensor, I²C / SPI, LGA-8", "U", ""),
    # Data converters (load cells, joint potentiometers, spindle speed)
    ("Sensors", "Analog_ADC", "ADS1220xPW", "ADS1220IPWR", "Texas Instruments",
     "24-bit 4-channel delta-sigma ADC with PGA — load cells (grippers, bed levelling, force feet), TSSOP-16", "U", ""),
    ("Sensors", "Analog_ADC", "ADS131M04xPW", "ADS131M04IPWR", "Texas Instruments",
     "4-channel simultaneous-sampling 24-bit ADC — phase-current sensing for FOC drives, TSSOP-20", "U", ""),
    ("Sensors", "Analog_ADC", "MCP3208", "MCP3208-CI/SL", "Microchip",
     "12-bit 8-channel SAR ADC, SPI — potentiometer joints, analog sensors, SOIC-16", "U",
     "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"),
    ("Sensors", "Analog_DAC", "MCP4725xxx-xCH", "MCP4725A0T-E/CH", "Microchip",
     "12-bit DAC with EEPROM, I²C — CNC spindle 0–10 V speed reference (with an op-amp), SOT-23-6", "U", ""),
    # Field buses and networking
    ("Interface", "Interface_CAN_LIN", "MCP2515-xSO", "MCP2515-I/SO", "Microchip",
     "Stand-alone CAN 2.0B controller, SPI — adds CAN to any MCU, SOIC-18", "U", ""),
    ("Interface", "Interface_CAN_LIN", "TJA1051T-3", "TJA1051T/3", "NXP",
     "High-speed CAN transceiver 5 Mbit/s, 3.3 V / 5 V VIO — joint and leg buses, SOIC-8", "U", ""),
    ("Interface", "Interface_CAN_LIN", "SN65HVD230", "SN65HVD230DR", "Texas Instruments",
     "3.3 V CAN transceiver 1 Mbit/s, standby mode, SOIC-8", "U", ""),
    ("Interface", "Interface_CAN_LIN", "TCAN4550RGY", "TCAN4550RGYR", "Texas Instruments",
     "CAN FD controller + transceiver, SPI, 8 Mbit/s — CAN FD for MCUs without it, VQFN-20 3.5 × 4.5 mm", "U", ""),
    ("Interface", "Interface_CAN_LIN", "ISO1050DUB", "ISO1050DUBR", "Texas Instruments",
     "Isolated CAN transceiver, 2.5 kV — per-joint isolation in arms and humanoids, SOP-8", "U", ""),
    ("Interface", "Interface_Ethernet", "W5500", "W5500", "WIZnet",
     "Hardwired TCP/IP Ethernet controller with MAC + PHY, SPI — CNC and printer network control, LQFP-48", "U", ""),
    ("Interface", "Interface_Ethernet", "LAN8720A", "LAN8720A-CP", "Microchip",
     "10/100 Ethernet PHY, RMII — STM32 / ESP32 Ethernet, QFN-24", "U", ""),
    ("Interface", "Interface_Ethernet", "KSZ8081RNA", "KSZ8081RNACA", "Microchip",
     "10/100 Ethernet PHY, RMII, industrial temperature, QFN-24", "U", ""),
    ("Interface", "Interface_UART", "MAX3232", "MAX3232ESE+T", "Analog Devices (Maxim)",
     "Dual RS-232 transceiver 3…5.5 V — CNC pendants, legacy serial, SOIC-16", "U",
     "Package_SO:SOIC-16_3.9x9.9mm_P1.27mm"),
    ("Interface", "Interface_UART", "MAX3485", "MAX3485ESA+T", "Analog Devices (Maxim)",
     "3.3 V RS-485 / RS-422 transceiver 10 Mbit/s — Dynamixel-style servo buses, SOIC-8", "U",
     "Package_SO:SOIC-8_3.9x4.9mm_P1.27mm"),
    ("Interface", "Interface_UART", "THVD1450D", "THVD1450DR", "Texas Instruments",
     "RS-485 transceiver 50 Mbit/s, ±18 kV ESD, 3…5.5 V — encoder and servo links, SOIC-8", "U", ""),
    ("Interface", "Interface", "AM26LS31CD", "AM26LS31CDR", "Texas Instruments",
     "Quad differential line driver (RS-422) — CNC step / dir and encoder outputs, SOIC-16", "U", ""),
    ("Interface", "Interface", "AM26LV32xD", "AM26LV32CDR", "Texas Instruments",
     "Quad differential line receiver 3.3 V (RS-422) — incremental encoder inputs (A/B/Z), SOIC-16", "U", ""),
    ("Logic", "Interface_Expansion", "TCA9548APWR", "TCA9548APWR", "Texas Instruments",
     "8-channel I²C multiplexer — many identical sensors on one bus (fingers, feet, ToF arrays), TSSOP-24", "U", ""),
    ("Logic", "Logic_LevelTranslator", "TXS0108EPW", "TXS0108EPWR", "Texas Instruments",
     "8-bit bidirectional level translator, open-drain friendly (1.8 / 3.3 / 5 V), TSSOP-20", "U", ""),
    ("Logic", "Logic_LevelTranslator", "TXB0104PW", "TXB0104PWR", "Texas Instruments",
     "4-bit bidirectional push-pull level translator, auto-direction, TSSOP-14", "U", ""),
    ("Logic", "Logic_LevelTranslator", "SN74LVC1T45DBV", "SN74LVC1T45DBVR", "Texas Instruments",
     "Single-bit dual-supply bus transceiver — 3.3 V MCU to 5 V servo / LED signal, SOT-23-6", "U", ""),
    # Isolation
    ("Optocouplers", "Isolator", "ISO7721D", "ISO7721DR", "Texas Instruments",
     "2-channel digital isolator 100 Mbit/s, 3 kV RMS, 1 forward / 1 reverse — UART / CAN isolation, SOIC-8", "U", ""),
    ("Optocouplers", "Isolator", "6N137", "6N137", "Broadcom",
     "High-speed optocoupler 10 Mbit/s — CNC step / dir and limit-switch inputs, DIP-8", "U", ""),
    ("Optocouplers", "Isolator", "TLP291-4", "TLP291-4", "Toshiba",
     "Quad phototransistor optocoupler — limit switches, E-stop and probe inputs, SO-16", "U", ""),
    # Wireless and memory
    ("Memory", "Memory_Flash", "W25Q128JVS", "W25Q128JVSIQ", "Winbond",
     "128 Mbit SPI / QSPI NOR flash — flight logs (blackbox), G-code and firmware storage, SOIC-8 208 mil", "U", ""),
    ("Memory", "Memory_EEPROM", "AT24CS02-SSHM", "AT24CS02-SSHM-T", "Microchip",
     "2 Kbit I²C EEPROM with unique 128-bit serial number — joint calibration and board ID, SOIC-8", "U", ""),
    # DRAM: SDR SDRAM on an MCU's memory controller (FMC / EMC), DDR3L / DDR4 memory-down beside an SoC
    ("Memory · DRAM", "Memory_RAM", "MT48LC16M16A2TG", "MT48LC16M16A2TG-6A", "Micron",
     "256 Mb SDR SDRAM, 4 M × 16 × 4 banks, 167 MHz, 3.3 V, TSOP-II-54 — STM32 FMC / NXP SEMC frame buffers", "U", ""),
    ("Memory · DRAM", "Memory_RAM", "W9812G6KH-6", "W9812G6KH-6", "Winbond",
     "128 Mb SDR SDRAM, 2 M × 16 × 4 banks, 166 MHz, 3.3 V, TSOP-II-54", "U", ""),
    ("Memory · DRAM", "Memory_RAM", "IS42S16400J-xT", "IS42S16400J-7TL", "ISSI",
     "64 Mb SDR SDRAM, 1 M × 16 × 4 banks, 143 MHz, 3.3 V, TSOP-II-54", "U", ""),
    ("Memory · DRAM", "Memory_RAM", "MT41K256M16HA", "MT41K256M16HA-125", "Micron",
     "4 Gb DDR3L SDRAM × 16, 1.35 V, DDR3-1600, 96-ball FBGA — memory-down for FPGAs / SoCs", "U", ""),
    ("Memory · DRAM", "Memory_RAM", "AS4C256M16D3", "AS4C256M16D3-12BCN", "Alliance Memory",
     "4 Gb DDR3 SDRAM × 16, 1.5 V, DDR3-1600, 96-ball FBGA", "U", ""),
    ("Memory · DRAM", "Memory_RAM", "MT40A512M16LY", "MT40A512M16LY-062E", "Micron",
     "8 Gb DDR4 SDRAM × 16, 1.2 V (VPP 2.5 V), DDR4-3200, 96-ball FBGA", "U", ""),
    # Power: regulation, protection, batteries
    ("Regulators", "Regulator_Switching", "LM2576S-5", "LM2576S-5.0", "Texas Instruments",
     "5 V 3 A step-down regulator, 40 V input, 52 kHz — servo and logic rail, TO-263-5", "U", ""),
    ("Regulators", "Regulator_Switching", "LMR33630ADDA", "LMR33630ADDAR", "Texas Instruments",
     "36 V 3 A synchronous buck, 400 kHz, 25 µA quiescent — 24 V printer / CNC and 6S drone logic rails, HSOIC-8",
     "U", ""),
    ("Regulators", "Regulator_Switching", "LT8610", "LT8610EMSE#PBF", "Analog Devices",
     "42 V 2.5 A synchronous buck, 2.5 µA quiescent, Silent Switcher — battery robots, MSOP-16 with pad", "U", ""),
    ("Regulators", "Regulator_Linear", "LD1117S33TR_SOT223", "LD1117S33TR", "STMicroelectronics",
     "3.3 V 800 mA low-dropout regulator, SOT-223", "U", ""),
    ("Regulators", "Regulator_Linear", "LP2985-3.3", "LP2985-33DBVR", "Texas Instruments",
     "3.3 V 150 mA ultra-low-noise LDO — IMU and ADC references, SOT-23-5", "U", ""),
    ("Power", "Power_Management", "LM5050-1", "LM5050MK-1/NOPB", "Texas Instruments",
     "High-side ideal-diode OR-ing controller 5…75 V — dual battery / charger OR-ing, SOT-23-6", "U", ""),
    ("Power", "Power_Management", "LM5060", "LM5060MM/NOPB", "Texas Instruments",
     "High-side protection controller 5.5…65 V with timed overcurrent shutdown (eFuse with external FET), VSSOP-10",
     "U", ""),
    ("Power", "Power_Management", "TPS22919DCK", "TPS22919DCKR", "Texas Instruments",
     "5.5 V 1.5 A load switch with quick output discharge — sensor and radio power gating, SC-70-6", "U", ""),
    ("Power", "Battery_Management", "BQ76920PW", "BQ7692000PWR", "Texas Instruments",
     "3–5 cell Li-ion battery monitor and protection AFE with coulomb counter — drones, rovers, TSSOP-20", "U", ""),
    ("Power", "Battery_Management", "BQ76940DBT", "BQ7694000DBTR", "Texas Instruments",
     "9–15 cell Li-ion battery monitor and protection AFE — quadruped and humanoid packs (36–60 V), TSSOP-44", "U", ""),
    ("Power", "Battery_Management", "BQ24074RGT", "BQ24074RGTR", "Texas Instruments",
     "1-cell 1.5 A Li-ion charger with power-path (runs while charging), QFN-16", "U", ""),
    ("Power", "Battery_Management", "BQ25895RTW", "BQ25895RTWR", "Texas Instruments",
     "1-cell 5 A charger with power-path, I²C, USB input detection — handheld controllers, WQFN-24", "U", ""),
    # ---------------------------------------------------------------- General-purpose catalog: the MCUs, op-amps,
    # regulators, interfaces, logic, drivers, memories, sensors, transistors and protection parts most boards use,
    # each on its orderable package.
    # Microcontrollers · STMicroelectronics
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F0", "STM32F030C8Tx", "STM32F030C8T6", "STMicroelectronics",
     "Cortex-M0 48 MHz, 64 KB flash, LQFP-48", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F1", "STM32F103RBTx", "STM32F103RBT6", "STMicroelectronics",
     "Cortex-M3 72 MHz, 128 KB flash, USB, CAN, LQFP-64", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F3", "STM32F303CCTx", "STM32F303CCT6", "STMicroelectronics",
     "Cortex-M4F 72 MHz, 256 KB flash, analog-rich (op-amps, comparators, 12-bit ADCs), LQFP-48", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F4", "STM32F411RETx", "STM32F411RET6", "STMicroelectronics",
     "Cortex-M4F 100 MHz, 512 KB flash, USB OTG FS, LQFP-64", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32F4", "STM32F429ZITx", "STM32F429ZIT6", "STMicroelectronics",
     "Cortex-M4F 180 MHz, 2 MB flash, LCD-TFT controller, SDRAM interface, Ethernet, LQFP-144", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32G0", "STM32G031K8Tx", "STM32G031K8T6", "STMicroelectronics",
     "Cortex-M0+ 64 MHz, 64 KB flash, LQFP-32", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32L0", "STM32L072CZTx", "STM32L072CZT6", "STMicroelectronics",
     "Ultra-low-power Cortex-M0+ 32 MHz, 192 KB flash, USB, LQFP-48", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32L4", "STM32L476RGTx", "STM32L476RGT6", "STMicroelectronics",
     "Ultra-low-power Cortex-M4F 80 MHz, 1 MB flash, USB OTG, LCD driver, LQFP-64", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32WB", "STM32WB55CGUx", "STM32WB55CGU6", "STMicroelectronics",
     "Cortex-M4F + Cortex-M0+ Bluetooth LE 5 / 802.15.4 radio, 1 MB flash, UFQFPN-48", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32C0", "STM32C011F6Px", "STM32C011F6P6", "STMicroelectronics",
     "Cortex-M0+ 48 MHz, 32 KB flash, low-cost TSSOP-20", "U", ""),
    ("Microcontrollers · STMicroelectronics", "MCU_ST_STM32G0", "STM32G071KBTxN", "STM32G071KBT6N", "STMicroelectronics",
     "Cortex-M0+ 64 MHz, 128 KB flash, USB-C Power Delivery controller, LQFP-32", "U", ""),
    # Microcontrollers · Espressif
    ("Microcontrollers · Espressif", "MCU_Espressif", "ESP32-C3", "ESP32-C3", "Espressif",
     "RISC-V 160 MHz Wi-Fi 4 + Bluetooth LE 5 SoC, external flash, QFN-32", "U", ""),
    ("Microcontrollers · Espressif", "MCU_Espressif", "ESP32-S3", "ESP32-S3", "Espressif",
     "Dual Xtensa LX7 240 MHz Wi-Fi 4 + Bluetooth LE 5 SoC, USB OTG, vector instructions, QFN-56", "U", ""),
    ("Microcontrollers · Espressif", "MCU_Espressif", "ESP32-PICO-D4", "ESP32-PICO-D4", "Espressif",
     "ESP32 system-in-package with 4 MB flash and crystal, QFN-48", "U", ""),
    # Microcontrollers · Microchip
    ("Microcontrollers · Microchip", "MCU_Microchip_ATtiny", "ATtiny202-SS", "ATTINY202-SSN", "Microchip",
     "tinyAVR 0-series 20 MHz, 2 KB flash, UPDI, SOIC-8", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATtiny", "ATtiny404-SS", "ATTINY404-SSN", "Microchip",
     "tinyAVR 0-series 20 MHz, 4 KB flash, UPDI, SOIC-14", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATtiny", "ATtiny3216-S", "ATTINY3216-SN", "Microchip",
     "tinyAVR 1-series 20 MHz, 32 KB flash, UPDI, SOIC-20W", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATmega", "ATmega328PB-A", "ATMEGA328PB-AU", "Microchip",
     "8-bit AVR 20 MHz, 32 KB flash, two USARTs / SPIs / I2Cs, TQFP-32", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATmega", "ATmega1284P-A", "ATMEGA1284P-AU", "Microchip",
     "8-bit AVR 20 MHz, 128 KB flash, 16 KB SRAM, TQFP-44", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_SAMD", "ATSAMD21J18A-A", "ATSAMD21J18A-AU", "Microchip",
     "Cortex-M0+ 48 MHz, 256 KB flash, USB, 64-pin TQFP", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_SAMD", "ATSAMD11C14A-SS", "ATSAMD11C14A-SSUT", "Microchip",
     "Cortex-M0+ 48 MHz, 16 KB flash, crystal-less USB, SOIC-14", "U", ""),
    ("Microcontrollers · Microchip", "MCU_Microchip_ATtiny", "ATtiny84A-SS", "ATTINY84A-SSU", "Microchip",
     "8-bit AVR 20 MHz, 8 KB flash, 12 I/O, SOIC-14", "U", ""),
    # Microcontrollers · Arm
    ("Microcontrollers · Arm", "MCU_RaspberryPi", "RP2350B", "RP2350B", "Raspberry Pi",
     "Dual Cortex-M33 / Hazard3 RISC-V 150 MHz, 520 KB SRAM, 48 GPIO, QFN-80", "U", ""),
    # Op-Amps
    ("Op-Amps", "Amplifier_Operational", "LM324", "LM324DR", "Texas Instruments",
     "Quad general-purpose op-amp, SOIC-14", "U", SOIC14),
    ("Op-Amps", "Amplifier_Operational", "LM324", "LM324N", "Texas Instruments",
     "Quad general-purpose op-amp, DIP-14", "U", "Package_DIP:DIP-14_W7.62mm"),
    ("Op-Amps", "Amplifier_Operational", "TL072", "TL072CDR", "Texas Instruments",
     "Dual low-noise JFET-input op-amp, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "TL074", "TL074CDR", "Texas Instruments",
     "Quad low-noise JFET-input op-amp, SOIC-14", "U", SOIC14),
    ("Op-Amps", "Amplifier_Operational", "NE5532", "NE5532DR", "Texas Instruments",
     "Dual low-noise audio op-amp, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "MCP6004", "MCP6004T-I/SL", "Microchip",
     "Quad 1 MHz rail-to-rail I/O op-amp, 1.8-6 V, SOIC-14", "U", SOIC14),
    ("Op-Amps", "Amplifier_Operational", "OP07", "OP07CDR", "Texas Instruments",
     "Precision low-offset op-amp, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "TLV9062xD", "TLV9062IDR", "Texas Instruments",
     "Dual 10 MHz rail-to-rail I/O op-amp, 1.8-5.5 V, SOIC-8", "U", ""),
    ("Op-Amps", "Comparator", "LM339", "LM339DR", "Texas Instruments",
     "Quad differential comparator, open collector, SOIC-14", "U", SOIC14),
    ("Op-Amps", "Comparator", "LM311", "LM311DR", "Texas Instruments",
     "High-speed comparator with strobe, open collector, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Instrumentation", "AD620", "AD620ARZ", "Analog Devices",
     "Low-power instrumentation amplifier, gain 1-10000 by one resistor, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Instrumentation", "INA128", "INA128UA", "Texas Instruments",
     "Precision low-power instrumentation amplifier, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "MCP6002-xSN", "MCP6002T-I/SN", "Microchip",
     "Dual 1 MHz rail-to-rail I/O op-amp, 1.8-6 V, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "LMV321", "LMV321IDBVR", "Texas Instruments",
     "Single rail-to-rail output op-amp, 2.7-5.5 V, SOT-23-5", "U", SOT235),
    ("Op-Amps", "Amplifier_Operational", "LMV358", "LMV358IDR", "Texas Instruments",
     "Dual rail-to-rail output op-amp, 2.7-5.5 V, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "OPA2340", "OPA2340UA", "Texas Instruments",
     "Dual rail-to-rail CMOS op-amp, 5.5 MHz, single supply, SOIC-8", "U", SOIC8),
    ("Op-Amps", "Amplifier_Operational", "OPA2134", "OPA2134UA", "Texas Instruments",
     "Dual high-performance audio op-amp, SOIC-8", "U", SOIC8),
    # Audio
    ("Audio", "Amplifier_Audio", "LM386", "LM386MX-1", "Texas Instruments",
     "Low-voltage audio power amplifier, gain 20-200, SOIC-8", "U", SOIC8),
    ("Audio", "Audio", "PCM5102", "PCM5102APWR", "Texas Instruments",
     "112 dB stereo audio DAC, I2S, PLL, TSSOP-20", "U", ""),
    ("Audio", "Amplifier_Audio", "PAM8403D", "PAM8403DR", "Diodes Inc.",
     "3 W stereo class-D audio amplifier, 5 V, SOP-16", "U", ""),
    # Regulators
    ("Regulators", "Regulator_Linear", "L7905", "LM7905", "STMicroelectronics",
     "-5 V 1.5 A negative linear regulator, TO-220", "U", TO220),
    ("Regulators", "Regulator_Linear", "LM1117S-3.3", "LM1117S-3.3", "Texas Instruments",
     "3.3 V 800 mA low-dropout regulator, TO-263", "U", ""),
    ("Regulators", "Regulator_Linear", "MCP1700x-330xxTT", "MCP1700T-3302E/TT", "Microchip",
     "3.3 V 250 mA low-quiescent (1.6 uA) LDO, SOT-23", "U", ""),
    ("Regulators", "Regulator_Linear", "LP5907MFX-3.3", "LP5907MFX-3.3", "Texas Instruments",
     "3.3 V 250 mA ultra-low-noise LDO, SOT-23-5", "U", ""),
    ("Regulators", "Regulator_Linear", "MIC5219-3.3YM5", "MIC5219-3.3YM5", "Microchip",
     "3.3 V 500 mA LDO with enable, SOT-23-5", "U", ""),
    ("Regulators", "Regulator_Linear", "AP7361C-33E", "AP7361C-33E-13", "Diodes Inc.",
     "3.3 V 1 A low-dropout regulator, SOT-223", "U", ""),
    ("Regulators", "Regulator_Linear", "TLV75533PDBV", "TLV75533PDBVR", "Texas Instruments",
     "3.3 V 500 mA LDO with enable, SOT-23-5", "U", ""),
    ("Regulators", "Regulator_Switching", "TPS5430DDA", "TPS5430DDAR", "Texas Instruments",
     "3 A 500 kHz step-down converter, 5.5-36 V input, HSOP-8 PowerPAD", "U", HSOP8),
    ("Regulators", "Regulator_Switching", "MC34063AD", "MC34063ADR", "Texas Instruments",
     "1.5 A buck / boost / inverting DC-DC controller, SOIC-8", "U", ""),
    ("Regulators", "Regulator_Switching", "TPS563200", "TPS563200DDCR", "Texas Instruments",
     "3 A synchronous step-down converter, 4.5-17 V input, SOT-23-6", "U", ""),
    ("Regulators", "Regulator_Switching", "MT3608", "MT3608", "Aerosemi",
     "2 A 1.2 MHz step-up converter, 2-24 V, SOT-23-6", "U", ""),
    ("Regulators", "Regulator_Switching", "LM2675M-5", "LM2675M-5.0", "Texas Instruments",
     "5 V 1 A step-down converter, 6.5-40 V input, SOIC-8", "U", ""),
    ("Regulators", "Regulator_Switching", "AP63203WU", "AP63203WU-7", "Diodes Inc.",
     "3.3 V 2 A synchronous buck, 3.8-32 V input, TSOT-23-6", "U", ""),
    ("Regulators", "Regulator_Switching", "LM2596S-3.3", "LM2596S-3.3", "Texas Instruments",
     "3.3 V 3 A step-down (buck) regulator, 150 kHz, TO-263-5", "U", ""),
    ("Regulators", "Reference_Voltage", "TL431DBZ", "TL431AIDBZR", "Texas Instruments",
     "Adjustable shunt voltage reference 2.5-36 V, SOT-23", "U", ""),
    ("Regulators", "Reference_Voltage", "LM4040DBZ-2.5", "LM4040AIM3-2.5", "Texas Instruments",
     "2.5 V precision shunt voltage reference, SOT-23", "U", ""),
    ("Regulators", "Reference_Voltage", "REF3033", "REF3033AIDBZR", "Texas Instruments",
     "3.3 V 50 ppm/K series voltage reference, SOT-23", "U", ""),
    ("Regulators", "Regulator_Linear", "LM1117MP-3.3", "LM1117MPX-3.3", "Texas Instruments",
     "3.3 V 800 mA low-dropout regulator, SOT-223", "U", ""),
    ("Regulators", "Regulator_Linear", "AMS1117-5.0", "AMS1117-5.0", "Advanced Monolithic Systems",
     "5 V 1 A low-dropout regulator, SOT-223", "U", ""),
    ("Regulators", "Regulator_Linear", "L78L05_SO8", "L78L05ACD13TR", "STMicroelectronics",
     "5 V 100 mA positive linear regulator, SOIC-8", "U", ""),
    ("Regulators", "Regulator_Switching", "TPS54302", "TPS54302DDCR", "Texas Instruments",
     "3 A 400 kHz synchronous step-down converter, 4.5-28 V input, SOT-23-6", "U", ""),
    ("Regulators", "Regulator_Switching", "TPS54360DDA", "TPS54360DDAR", "Texas Instruments",
     "3.5 A 60 V step-down converter with Eco-mode, HSOP-8 PowerPAD", "U", HSOP8),
    # Power
    ("Power", "Battery_Management", "MCP73831-2-OT", "MCP73831T-2ACI/OT", "Microchip",
     "Single-cell Li-ion / Li-Po linear charger, 500 mA, SOT-23-5", "U", ""),
    ("Power", "Power_Supervisor", "TPS3839DBZ", "TPS3839G33DBZR", "Texas Instruments",
     "Ultra-low-power (150 nA) supply voltage supervisor, SOT-23", "U", ""),
    ("Power", "Power_Supervisor", "MCP130-xxxxTT", "MCP130T-315I/TT", "Microchip",
     "Microcontroller reset supervisor with internal pull-up, SOT-23", "U", ""),
    ("Power", "Battery_Management", "BQ21040DBV", "BQ21040DBVR", "Texas Instruments",
     "Single-cell Li-ion linear charger, 800 mA, SOT-23-6", "U", ""),
    ("Power", "Power_Protection", "USBLC6-4SC6", "USBLC6-4SC6", "STMicroelectronics",
     "4-line low-capacitance ESD protection for high-speed data, SOT-23-6", "D", ""),
    # Interface
    ("Interface", "Interface_USB", "FT232RL", "FT232RL", "FTDI",
     "USB to UART bridge with internal oscillator and EEPROM, SSOP-28", "U", ""),
    ("Interface", "Interface_USB", "FT231XS", "FT231XS", "FTDI",
     "USB to full-handshake UART bridge, SSOP-20", "U", ""),
    ("Interface", "Interface_USB", "CH340C", "CH340C", "WCH",
     "USB to UART bridge with internal oscillator, SOIC-16", "U", ""),
    ("Interface", "Interface_UART", "MAX232", "MAX232DR", "Texas Instruments",
     "Dual RS-232 driver / receiver with charge pump, 5 V, SOIC-16", "U", SOIC16),
    ("Interface", "Interface_CAN_LIN", "MCP2562-E-SN", "MCP2562-E/SN", "Microchip",
     "High-speed CAN transceiver with VIO pin, 8 Mbit/s CAN FD, SOIC-8", "U", ""),
    ("Interface", "Interface_CAN_LIN", "SN65HVD231", "SN65HVD231DR", "Texas Instruments",
     "3.3 V CAN transceiver, low-power standby, SOIC-8", "U", ""),
    ("Interface", "Interface_Expansion", "TCA9555PWR", "TCA9555PWR", "Texas Instruments",
     "16-bit I2C I/O expander, 1.65-5.5 V, TSSOP-24", "U", ""),
    ("Interface", "Interface_Ethernet", "ENC28J60x-SO", "ENC28J60-I/SO", "Microchip",
     "10BASE-T Ethernet controller with SPI, SOIC-28W", "U", ""),
    ("Interface", "Interface_UART", "THVD1400D", "THVD1400DR", "Texas Instruments",
     "RS-485 transceiver with integrated surge protection, 500 kbit/s, SOIC-8", "U", ""),
    ("Interface", "Interface_Expansion", "MCP23017x-x-SO", "MCP23017-E/SO", "Microchip",
     "16-bit I2C I/O expander with interrupts, SOIC-28W", "U", ""),
    ("Interface", "Interface_Expansion", "MCP23S17x-x-SO", "MCP23S17-E/SO", "Microchip",
     "16-bit SPI I/O expander with interrupts, SOIC-28W", "U", ""),
    ("Interface", "Interface_Expansion", "MCP23008-xSO", "MCP23008-E/SO", "Microchip",
     "8-bit I2C I/O expander with interrupt, SOIC-18W", "U", ""),
    ("Interface", "Interface_USB", "CH9102F", "CH9102F", "WCH",
     "USB to UART bridge, internal oscillator, QFN-24", "U", ""),
    ("Interface", "Interface_USB", "CP2104", "CP2104-F03-GMR", "Silicon Labs",
     "USB to UART bridge with modem signals and OTP, QFN-24", "U", ""),
    ("Interface", "Interface_USB", "STUSB4500QTR", "STUSB4500QTR", "STMicroelectronics",
     "Standalone USB-C Power Delivery sink controller, QFN-24", "U", ""),
    ("Interface", "Interface_CAN_LIN", "MCP2021A-xxxxSN", "MCP2021A-500E/SN", "Microchip",
     "LIN transceiver with 5 V voltage regulator, SOIC-8", "U", ""),
    ("Interface", "Interface_Ethernet", "W5100S-Q", "W5100S-Q", "WIZnet",
     "Hardwired TCP/IP Ethernet controller with PHY, SPI / parallel, QFN-48", "U", ""),
    ("Interface", "Interface_Ethernet", "LAN8742A", "LAN8742A-CZ", "Microchip",
     "10/100 Ethernet PHY, RMII, QFN-24", "U", ""),
    ("Interface", "Interface_Ethernet", "DP83848I", "DP83848IVV", "Texas Instruments",
     "Industrial 10/100 Ethernet PHY, MII / RMII, LQFP-48", "U", ""),
    # Isolation
    ("Isolation", "Isolator", "ISO7720D", "ISO7720DR", "Texas Instruments",
     "Dual-channel digital isolator, 3 kVrms, 100 Mbit/s, SOIC-8", "U", ""),
    ("Isolation", "Isolator", "ADuM1200AR", "ADUM1200ARZ", "Analog Devices",
     "Dual-channel digital isolator, 2.5 kVrms, SOIC-8", "U", ""),
    # Logic
    ("Logic", "74xx", "74HC14", "74HC14D", "Nexperia",
     "Hex Schmitt-trigger inverter, SOIC-14", "U", SOIC14),
    ("Logic", "74xx", "74HC04", "74HC04D", "Nexperia",
     "Hex inverter, SOIC-14", "U", SOIC14),
    ("Logic", "74xx", "74HC165", "74HC165D", "Nexperia",
     "8-bit parallel-in / serial-out shift register, SOIC-16", "U", SOIC16),
    ("Logic", "74xx", "74HC245", "74HC245DW", "Texas Instruments",
     "Octal bus transceiver, 3-state, SOIC-20W", "U", "Package_SO:SOIC-20W_7.5x12.8mm_P1.27mm"),
    ("Logic", "74xx", "74HC4051", "74HC4051D", "Nexperia",
     "8-channel analog multiplexer / demultiplexer, SOIC-16", "U", SOIC16),
    ("Logic", "74xx", "74HC138", "74HC138D", "Nexperia",
     "3-to-8 line decoder / demultiplexer, SOIC-16", "U", SOIC16),
    ("Logic", "4xxx", "4051", "CD4051BM96", "Texas Instruments",
     "8-channel analog multiplexer, 3-18 V, SOIC-16", "U", SOIC16),
    ("Logic", "74xx", "74HC164", "74HC164D", "Nexperia",
     "8-bit serial-in / parallel-out shift register, SOIC-14", "U", SOIC14),
    ("Logic", "74xx", "74HC02", "74HC02D", "Nexperia",
     "Quad 2-input NOR gate, SOIC-14", "U", SOIC14),
    ("Logic", "74xx", "74AHCT125", "74AHCT125D", "Nexperia",
     "Quad buffer, 3-state, 5 V-tolerant level shifting (3.3 V to 5 V for LED strips), SOIC-14", "U", SOIC14),
    # Drivers
    ("Drivers", "Driver_LED", "TLC5940PWP", "TLC5940PWP", "Texas Instruments",
     "16-channel LED driver, 12-bit grayscale PWM, dot correction, HTSSOP-28", "U", ""),
    ("Drivers", "Driver_FET", "TC4427xOA", "TC4427AEOA", "Microchip",
     "Dual 1.5 A non-inverting MOSFET driver, SOIC-8", "U", ""),
    ("Drivers", "Transistor_Array", "ULN2803A", "ULN2803ADWR", "Texas Instruments",
     "8-channel Darlington sink array, 500 mA, SOIC-18W", "U", "Package_SO:SOIC-18W_7.5x11.6mm_P1.27mm"),
    ("Drivers", "Driver_LED", "MAX7219", "MAX7219CWG+", "Analog Devices (Maxim)",
     "8-digit LED display driver, SPI, SOIC-24W", "U", "Package_SO:SOIC-24W_7.5x15.4mm_P1.27mm"),
    ("Drivers", "Driver_LED", "MAX7219", "MAX7219CNG+", "Analog Devices (Maxim)",
     "8-digit LED display driver, SPI, DIP-24", "U", "Package_DIP:DIP-24_W7.62mm"),
    ("Drivers", "Driver_Motor", "DRV8870DDA", "DRV8870DDAR", "Texas Instruments",
     "Brushed DC H-bridge 3.6 A peak, 6.5-45 V, HSOP-8", "U", ""),
    ("Drivers", "Driver_Motor", "A4950E", "A4950ELJTR-T", "Allegro",
     "Full-bridge DMOS PWM motor driver 3.5 A, 40 V, SOIC-8 with exposed pad", "U", HSOP8),
    # Memory
    ("Memory", "Memory_Flash", "W25Q32JVSS", "W25Q32JVSSIQ", "Winbond",
     "32 Mbit SPI / QSPI NOR flash, SOIC-8 208 mil", "U", ""),
    ("Memory", "Memory_EEPROM", "AT24CS01-SSHM", "AT24C02C-SSHM-T", "Microchip",
     "2 Kbit I2C serial EEPROM, SOIC-8", "U", ""),
    ("Memory", "Memory_EEPROM", "25LCxxx", "25LC256-I/SN", "Microchip",
     "256 Kbit SPI serial EEPROM, SOIC-8", "U", SOIC8),
    # Sensors
    ("Sensors", "Sensor_Temperature", "MCP9808_MSOP", "MCP9808-E/MS", "Microchip",
     "±0.25 °C digital temperature sensor, I2C, MSOP-8", "U", ""),
    ("Sensors", "Sensor_Temperature", "TMP36xS", "TMP36GSZ", "Analog Devices",
     "Analog temperature sensor, 10 mV/°C, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Motion", "LIS3DH", "LIS3DHTR", "STMicroelectronics",
     "3-axis accelerometer ±2/4/8/16 g, I2C / SPI, LGA-16", "U", ""),
    ("Sensors", "Sensor", "BME680", "BME680", "Bosch Sensortec",
     "Gas, pressure, humidity and temperature sensor, I2C / SPI, LGA-8", "U", ""),
    ("Sensors", "Analog_ADC", "MCP3008", "MCP3008-I/SL", "Microchip",
     "8-channel 10-bit SAR ADC, SPI, SOIC-16", "U", SOIC16),
    ("Sensors", "Analog_ADC", "ADS1015IDGS", "ADS1015IDGSR", "Texas Instruments",
     "12-bit 4-channel ADC, PGA, I2C, VSSOP-10", "U", ""),
    ("Sensors", "Analog_ADC", "HX711", "HX711", "Avia Semiconductor",
     "24-bit ADC for load cells / bridge sensors, SOP-16", "U", SOIC16),
    ("Sensors", "Analog_DAC", "MCP4921", "MCP4921-E/SN", "Microchip",
     "12-bit voltage-output DAC, SPI, SOIC-8", "U", SOIC8),
    ("Sensors", "Sensor_Temperature", "LM75B", "LM75BD", "NXP",
     "Digital temperature sensor and thermal watchdog, I2C, SOIC-8", "U", SOIC8),
    ("Sensors", "Sensor_Temperature", "DS18B20Z", "DS18B20Z+", "Analog Devices (Maxim)",
     "1-Wire digital temperature sensor ±0.5 °C, SOIC-8", "U", ""),
    ("Sensors", "Sensor_Motion", "ADXL343", "ADXL343BCCZ", "Analog Devices",
     "3-axis digital accelerometer ±16 g, I2C / SPI, LGA-14", "U", ""),
    ("Sensors", "Sensor_Motion", "LSM6DS3", "LSM6DS3TR-C", "STMicroelectronics",
     "6-axis IMU (accelerometer + gyroscope), I2C / SPI, LGA-14", "U", ""),
    ("Sensors", "Sensor_Motion", "LSM6DSL", "LSM6DSLTR", "STMicroelectronics",
     "6-axis IMU (accelerometer + gyroscope), I2C / SPI, LGA-14", "U", ""),
    ("Sensors", "Sensor_Motion", "MPU-6000", "MPU-6000", "TDK InvenSense",
     "6-axis IMU with SPI and I2C, QFN-24 4 x 4 mm", "U", ""),
    ("Sensors", "Sensor_Pressure", "LPS22HH", "LPS22HHTR", "STMicroelectronics",
     "Barometric pressure sensor 260-1260 hPa, I2C / SPI, HLGA-10", "U", ""),
    ("Sensors", "Sensor_Pressure", "LPS25HB", "LPS25HBTR", "STMicroelectronics",
     "Barometric pressure sensor 260-1260 hPa, I2C / SPI, HLGA-10", "U", ""),
    ("Sensors", "Sensor_Humidity", "SHTC3", "SHTC3", "Sensirion",
     "Humidity and temperature sensor ±2 %RH, I2C, DFN-4", "U", ""),
    ("Sensors", "Sensor_Magnetic", "DRV5033FAxDBZ", "DRV5033FAQDBZR", "Texas Instruments",
     "Digital omnipolar Hall switch, 2.5-38 V, SOT-23", "U", ""),
    ("Sensors", "Sensor_Current", "ACS712xLCTR-05B", "ACS712ELCTR-05B-T", "Allegro",
     "±5 A Hall-effect current sensor, 2.1 kVrms isolation, SOIC-8", "U", ""),
    ("Sensors", "Analog_ADC", "ADS1013IDGS", "ADS1013IDGSR", "Texas Instruments",
     "12-bit single-channel ADC, I2C, VSSOP-10", "U", ""),
    # Transistors
    ("Transistors", "Transistor_FET", "AO3401A", "AO3401A", "Alpha & Omega",
     "P-channel MOSFET -30 V -4 A, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "2N7002", "2N7002", "onsemi",
     "N-channel small-signal MOSFET 60 V 115 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "BSS84", "BSS84", "onsemi",
     "P-channel small-signal MOSFET -50 V -130 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "IRLML6402", "IRLML6402TRPBF", "Infineon",
     "P-channel MOSFET -20 V -3.7 A logic level, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "IRLB8721PBF", "IRLB8721PBF", "Infineon",
     "N-channel MOSFET 30 V 62 A logic level, TO-220", "Q", ""),
    ("Transistors", "Transistor_FET", "IRF9540N", "IRF9540NPBF", "Infineon",
     "P-channel power MOSFET -100 V -23 A, TO-220", "Q", ""),
    ("Transistors", "Transistor_BJT", "MMBT3904", "MMBT3904", "onsemi",
     "NPN small-signal transistor 40 V 200 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_BJT", "MMBT3906", "MMBT3906", "onsemi",
     "PNP small-signal transistor -40 V -200 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_BJT", "BC817", "BC817-40", "Nexperia",
     "NPN general-purpose transistor 45 V 500 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_BJT", "BC807", "BC807-40", "Nexperia",
     "PNP general-purpose transistor -45 V -500 mA, SOT-23", "Q", ""),
    ("Transistors", "Transistor_BJT", "TIP120", "TIP120", "onsemi",
     "NPN Darlington power transistor 60 V 5 A, TO-220", "Q", ""),
    ("Transistors", "Transistor_FET", "IRLML6244", "IRLML6244TRPBF", "Infineon",
     "N-channel MOSFET 20 V 6.3 A logic level, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "IRLML0030", "IRLML0030TRPBF", "Infineon",
     "N-channel MOSFET 30 V 5.3 A logic level, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "DMG3402L", "DMG3402L-7", "Diodes Inc.",
     "N-channel MOSFET 30 V 4 A, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "DMG2301L", "DMG2301L-7", "Diodes Inc.",
     "P-channel MOSFET -20 V -3 A, SOT-23", "Q", ""),
    ("Transistors", "Transistor_FET", "Si2319CDS", "SI2319CDS-T1-GE3", "Vishay",
     "P-channel MOSFET -40 V -4.4 A, SOT-23", "Q", ""),
    # Timing
    ("Timing", "Timer_RTC", "DS3231M", "DS3231MZ+", "Analog Devices (Maxim)",
     "±5 ppm I2C real-time clock with MEMS resonator, SOIC-8", "U", ""),
    ("Timing", "Timer_RTC", "DS1307Z+", "DS1307Z+", "Analog Devices (Maxim)",
     "I2C real-time clock with 56 bytes of RAM, SOIC-8", "U", ""),
    ("Timing", "Timer_RTC", "PCF8563T", "PCF8563T", "NXP",
     "Low-power I2C real-time clock / calendar, SOIC-8", "U", ""),
    ("Timing", "Timer", "LMC555xM", "LMC555CMX", "Texas Instruments",
     "CMOS 555 timer, 1.5-15 V, SOIC-8", "U", ""),
    # Protection
    ("Protection", "Power_Protection", "TPD2E2U06DCK", "TPD2E2U06DCKR", "Texas Instruments",
     "2-channel low-capacitance ESD protection for USB 2.0, SC-70", "D", ""),
]


FOOTPRINTS = "https://gitlab.com/api/v4/projects/kicad%2Flibraries%2Fkicad-footprints/repository/files/{}/raw?ref=master"


def lands(footprint):
    """Exact land pattern of a KiCad footprint: [(number, x, y, w, h)] of its numbered SMD pads (the largest pad of
    each number; paste-only and thermal-via pads skipped), and the body (w, h) from the name when it carries one."""
    lib, name = footprint.split(":")
    path = os.path.join(kicad.CACHE, f"fp__{name}.kicad_mod")
    if not os.path.exists(path):
        os.makedirs(kicad.CACHE, exist_ok=True)
        url = FOOTPRINTS.format(urllib.parse.quote(f"{lib}.pretty/{name}.kicad_mod", safe=""))
        with urllib.request.urlopen(url, timeout=60) as r, open(path, "wb") as f:
            f.write(r.read())
    text = open(path, encoding="utf-8").read()
    best = {}
    for m in re.finditer(r'\(pad "([^"]+)" smd \w+\s*\(at ([-\d.]+) ([-\d.]+)(?: ([-\d.]+))?\)\s*\(size ([\d.]+) ([\d.]+)\)',
                         text):
        num, x, y, rot, w, h = m.group(1), float(m.group(2)), float(m.group(3)), float(m.group(4) or 0), \
            float(m.group(5)), float(m.group(6))
        if round(rot) % 180 == 90: w, h = h, w
        if not num.isdigit(): continue
        if num not in best or w * h > best[num][2] * best[num][3]: best[num] = (x, y, w, h)
    body = re.search(r"_([\d.]+)x([\d.]+)mm", name)
    return best, (float(body.group(1)), float(body.group(2))) if body else (0, 0)


def irregular(footprint):
    """Footprints SiEDA's parametric packages cannot draw pad-for-pad: land grids, rectangular QFN / DFN bodies,
    clockwise numbering, uneven pad rows, multi-slug power packages, power SON with a fused drain pad, 0.65 mm SC-70."""
    lib, name = footprint.split(":")
    u = name.upper()
    if lib in ("Package_LGA", "Sensor_Distance") or any(k in u for k in ("MULTIPOWERSO", "R-PVQFN", "TDSON", "SC-70")):
        return True
    body = re.search(r"(?:QFN|DFN)-\d+.*?_([\d.]+)x([\d.]+)mm", name)
    return bool(body and body.group(1) != body.group(2))


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
    if "SOT-23" in u or "SC-70" in u:
        m = re.search(r"(?:SOT-23|SC-70)-(\d+)", u)
        return "SOT23", int(m.group(1)) if m else 3, 0, 0, fp
    if re.search(r"H(T)?SOP-8-1EP", u):  # SOIC-8 body with a thermal pad (HSOP / HTSOP / DDA)
        return "TSSOP", 8, 1.27, 3.9, fp
    if "SOP-8_6.62X9.15MM_P2.54MM" in u:  # wide-pitch SOP-8 (TI DUB)
        return "SOIC", 8, 2.54, 6.62, fp
    if "POLOLU_BREAKOUT-16" in u:  # StepStick carrier: two 1 × 8 headers 12.7 mm apart, numbered like a DIP
        return "DIP", 16, 2.54, 12.7, fp
    if "TO-220-15" in u:
        return "MULTIWATT", 15, 1.27, 0, fp
    if "TO-220-3" in u:
        return "TO220", 3, 0, 0, fp
    if re.search(r"(^|_)(T|V|W)?DFN|TDSON|VSON", u):
        n = int(re.search(r"(?:DFN|SON)-(\d+)", u).group(1))
        return "SON", n, float(pitch.group(1)) if pitch else 1.27, float(body.group(1)) if body else 0, fp
    if "TSOP-II" in u:  # SDRAM TSOP-II: gull-wing leads on the long sides, body width across the rows
        tsop = re.search(r"TSOP-II-(\d+)_([\d.]+)x([\d.]+)mm", fp)
        return "TSSOP", int(tsop.group(1)), float(pitch.group(1)), float(tsop.group(3)), fp
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
    summary, errors = [], []
    for category, lib, sym, name, maker, desc, ref, override in PARTS:
        try:
            out.extend(entry(category, lib, sym, name, maker, desc, ref, override, summary))
        except (SystemExit, Exception) as e:  # report every bad part at once
            errors.append(f"{name}: {e}")
    if errors: raise SystemExit("\n".join(errors))
    open("Core/src/StandardCatalog.inc", "w").write("\n".join(out) + "\n")
    print("\n".join(summary))


def entry(category, lib, sym, name, maker, desc, ref, override, summary):
    out = []
    if True:
        found, footprint = kicad.pins(lib, sym)
        for k in [k for k in found if k.startswith("[")]:
            for num in k.strip("[]").split(","):
                found[num.strip()] = found[k]
            del found[k]
        footprint = override or footprint
        if not footprint: raise SystemExit(f"{sym}: no footprint")
        if not override and irregular(footprint):
            pads, (bw, bd) = lands(footprint)
            if not pads: raise SystemExit(f"{footprint}: no SMD pads")
            top = max(int(k) for k in pads)
            missing = [i for i in range(1, top + 1) if str(i) not in pads]
            if missing: raise SystemExit(f"{footprint}: land pattern has no pad {missing}")
            for k in found:
                if not k.isdigit() or int(k) > top: raise SystemExit(f"{sym}: pin {k} has no pad")
            pin_list = [(str(i),) + ((found[str(i)][0], found[str(i)][1]) if str(i) in found else ("NC", "NoConnect"))
                        for i in range(1, top + 1)]
            fpname = footprint.split(":")[-1]
            out.append(f"catalog(parts, {cpp(category)}, {cpp(name)}, {cpp(maker)}, {cpp(desc)}, {cpp(ref)},")
            out.append(f"    \"LGA\", {top}, 0, {bw:g}, {cpp(fpname)},")
            out.append("    {" + ", ".join("{%s, %s, T::%s}" % (cpp(num), cpp(nm), t) for num, nm, t in pin_list) + "});")
            out.append(f"parts.back().spec.package.bodyDepth = {bd:g};")
            out.append("parts.back().spec.package.lands = {" + ", ".join(
                "{%g, %g, %g, %g}" % pads[str(i)] for i in range(1, top + 1)) + "};")
            summary.append(f"{name:20} {'LGA':9} n={top:3} pins={len(pin_list):3} {fpname}")
            return out
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
        rect = re.search(r"_([\d.]+)x([\d.]+)mm", fpname)
        if kind == "BGA" and rect and float(rect.group(1)) != float(rect.group(2)):  # rectangular body (DRAM FBGA)
            out.append(f"parts.back().spec.package.bodyDepth = {float(rect.group(2)):g};")
        summary.append(f"{name:20} {kind:9} n={n:3} pins={len(pin_list):3} {fpname}")
    return out


if __name__ == "__main__":
    main()
