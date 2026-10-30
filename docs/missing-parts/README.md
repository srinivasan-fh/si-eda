# Missing IC parts

These parts are in neither a KiCad library the catalog generator reads nor a datasheet that could be fetched from the cloud
build environment, so they are not in the SiEDA library yet: 14 from the production catalog and 89 from the
semiconductor IC reference (October 2026). Each file below holds everything known about the
part (package, pitch, body, datasheet link) and an empty pin table to fill in from the datasheet.

| File | Part number | Manufacturer | Package | Status |
|---|---|---|---|---|
| [TMS320F28379D](TMS320F28379D.md) | TMS320F28379DPTPS | Texas Instruments | HLQFP-176 (PTP) | verified (datasheet SPRS880P (revised February 2024), page 13) |
| [DRV8301](DRV8301.md) | DRV8301DCAR | Texas Instruments | HTSSOP-56 (DCA) | verified (datasheet SLOS719F (rev. F, January 2016), page 3–5) |
| [DRV8353](DRV8353.md) | DRV8353SRTAR | Texas Instruments | WQFN-40 6×6 (RTA) | verified (datasheet SLVSDY6A (DRV835x, revised June 2019), page 6) |
| [TPS65381](TPS65381.md) | TPS65381QDAPRQ1 | Texas Instruments | HTSSOP-32 (DAP) | verified (datasheet SLVSBC4G (TPS65381-Q1, revised June 2017), page 27) |
| [LM5116](LM5116.md) | LM5116MH/NOPB | Texas Instruments | HTSSOP-20 (PWP) | verified (datasheet SNVS499I (rev. I, November 2023), page 3–4) |
| [TJA1101](TJA1101.md) | TJA1101BHN | NXP Semiconductors | HVQFN-36 6×6 | verified (datasheet TJA1101B Rev. 1 (1 March 2021), page 4–6) |
| [MA702](MA702.md) | MA702GQ | Monolithic Power Systems | QFN-16 3×3 | verified (datasheet MA702 Rev. 1.2 (2022-08-08), page 8) |
| [BMX160](BMX160.md) | BMX160 | Bosch Sensortec | LGA-14 2.5×3.0 | verified (datasheet BST-BMX160-DS000-12 (revision 1.2, January 2019), page 101) |
| [ADS1256](ADS1256.md) | ADS1256IDBR | Texas Instruments | SSOP-28 (DB) | verified (datasheet SBAS288K (rev. K, September 2013), page 5) |
| [ATSAMD51P20A](ATSAMD51P20A.md) | ATSAMD51P20A-AUT | Microchip | TQFP-128 14×14 | verified (datasheet DS60001507N, page 26) |
| [INMP441](INMP441.md) | INMP441ACEZ-R0 | TDK InvenSense | LGA_CAV-9 4.72×3.76 | verified (datasheet DS-INMP441-00 revision 1.1, page 8) |
| [JQ6500](JQ6500.md) | JQ6500 | Jiaqi (JQ) | SOP-16 | TODO |
| [WT2003HP8](WT2003HP8.md) | WT2003HP8-32N | Waytronic | QFN-32 4×4 | verified (datasheet WT2003HX V1.15, page 7) |
| [SYN6288](SYN6288.md) | SYN6288 | Beijing Yutone (宇音天下) | SSOP-28 | TODO |

### From the semiconductor IC reference (October 2026)

89 more parts the reference names that are in none of the libraries the catalog generator reads (KiCad's, Espressif's,
Digi-Key's and SparkFun's KiCad libraries); the 57 that are in one of them are in the catalog already. The package values
in these files come from each vendor's ordering information and still need checking against the package drawing (step 2).

| File | Part number | Manufacturer | Package | Status |
|---|---|---|---|---|
| [PIC16F18446](PIC16F18446.md) | PIC16F18446-I/SS | Microchip | SSOP-20 5.30 mm (SS) | verified (datasheet DS40001985B, page 7) |
| [LPC55S69](LPC55S69.md) | LPC55S69JBD100 | NXP Semiconductors | HLQFP-100 14×14 with exposed pad (SOT1570-3) | verified (datasheet LPC55S6x Rev. 2.5 (29 August 2024), page 8) |
| [RA6M5](RA6M5.md) | R7FA6M5BH3CFC | Renesas | LQFP-176 24×24 | TODO |
| [TC375](TC375.md) | SAK-TC375TP-96F300W AA | Infineon | LQFP-176 with exposed pad | TODO |
| [S32K344](S32K344.md) | FS32K344 (172 HDQFP) | NXP Semiconductors | 172-pin HDQFP (confirm) | TODO |
| [RH850](RH850.md) | RH850/F1KM-S1 (LQFP-100 variant) | Renesas | LQFP-100 14×14 (confirm) | TODO |
| [MAX10](MAX10.md) | 10M08SAE144C8G | Intel (Altera) | EQFP-144 with exposed pad | TODO |
| [MAX809](MAX809.md) | MAX809STRG | onsemi | SOT-23-3 | TODO |
| [MAX810](MAX810.md) | MAX810STRG | onsemi | SOT-23-3 | TODO |
| [STM809](STM809.md) | STM809TWX6F | STMicroelectronics | SOT-23-3 | TODO |
| [Si5341](Si5341.md) | Si5341A-D-GM | Skyworks (Silicon Labs) | QFN-64 9×9 | TODO |
| [LMK04828](LMK04828.md) | LMK04828BISQ/NOPB | Texas Instruments | WQFN-64 9×9 (NKD) | verified (datasheet SNAS605AS (revised May 2020), page 8) |
| [CDCE6214](CDCE6214.md) | CDCE6214RGET | Texas Instruments | VQFN-24 4×4 (RGE) | verified (datasheet SNAS811A (revised July 2025), page 5) |
| [W25Q64JV](W25Q64JV.md) | W25Q64JVSSIQ | Winbond | SOIC-8 208 mil | TODO |
| [W25Q256JV](W25Q256JV.md) | W25Q256JVFIQ | Winbond | SOIC-16 300 mil | TODO |
| [MX25L128](MX25L128.md) | MX25L12833FM2I-10G | Macronix | SOP-8 200 mil | TODO |
| [MX25L256](MX25L256.md) | MX25L25645GM2I-08G | Macronix | SOP-8 200 mil | TODO |
| [W25N01GV](W25N01GV.md) | W25N01GVZEIG | Winbond | WSON-8 8×6 | TODO |
| [24AA02E48](24AA02E48.md) | 24AA02E48T-I/OT | Microchip | SOT-23-5 (OT) | verified (datasheet DS20002124G, page 6) |
| [MB85RC256V](MB85RC256V.md) | MB85RC256VPNF-G-JNERE1 | Fujitsu (RAMXEED) | SOP-8 150 mil | TODO |
| [FM24CL64B](FM24CL64B.md) | FM24CL64B-GATR | Infineon (Cypress) | SOIC-8 150 mil | verified (datasheet 001-84457 Rev. *F, page 3) |
| [AS6C62256](AS6C62256.md) | AS6C62256-55SCN | Alliance Memory | SOP-28 330 mil | TODO |
| [INA826](INA826.md) | INA826AIDR | Texas Instruments | SOIC-8 (D) | verified (datasheet SBOS562G (revised June 2020), page 4) |
| [TLV1701](TLV1701.md) | TLV1701AIDBVR | Texas Instruments | SOT-23-5 (DBV) | verified (datasheet SBOS589D (revised June 2015), page 5) |
| [TLV3201](TLV3201.md) | TLV3201AIDBVR | Texas Instruments | SOT-23-5 (DBV) | verified (datasheet SBOS561C (revised May 2024), page 3) |
| [ADG704](ADG704.md) | ADG704BRMZ | Analog Devices | MSOP-10 | TODO |
| [PGA280](PGA280.md) | PGA280AIPW | Texas Instruments | TSSOP-24 (PW) | verified (datasheet SBOS487B (revised March 2020), page 3) |
| [AD8253](AD8253.md) | AD8253ARMZ | Analog Devices | MSOP-10 | TODO |
| [OPA380](OPA380.md) | OPA380AIDGKR | Texas Instruments | VSSOP-8 (DGK) | verified (datasheet SBOS291G (revised September 2007), page 2) |
| [TPA3116D2](TPA3116D2.md) | TPA3116D2DAD | Texas Instruments | HTSSOP-32 PowerPAD up (DAD) | verified (datasheet SLOS708G (revised December 2017), page 4) |
| [TPA3255](TPA3255.md) | TPA3255DDV | Texas Instruments | HTSSOP-44 PowerPAD up (DDV) | verified (datasheet SLASEA8A (revised October 2016), page 4) |
| [ADS124S08](ADS124S08.md) | ADS124S08IPBS | Texas Instruments | TQFP-32 5×5 (PBS) | verified (datasheet SBAS660C (revised June 2017), page 6) |
| [AD7124-4](AD7124-4.md) | AD7124-4BRUZ | Analog Devices | TSSOP-24 | TODO |
| [TLV320AIC3204](TLV320AIC3204.md) | TLV320AIC3204IRHBR | Texas Instruments | VQFN-32 5×5 (RHB) | verified (datasheet SLOS602E (revised September 2019), page 5) |
| [DAC80501](DAC80501.md) | DAC80501MDGSR | Texas Instruments | VSSOP-10 (DGS) | verified (datasheet SBAS794E (revised August 2023), page 3) |
| [AD5683R](AD5683R.md) | AD5683RBRMZ | Analog Devices | MSOP-10 | TODO |
| [ADS1292R](ADS1292R.md) | ADS1292RIPBS | Texas Instruments | TQFP-32 5×5 (PBS) | verified (datasheet SBAS502C (revised April 2020), page 4) |
| [CAP1298](CAP1298.md) | CAP1298-1-A4-TR | Microchip | QFN-16 3×3 (A4) | verified (datasheet DS00001571B, page 5) |
| [MP1584](MP1584.md) | MP1584EN-LF-Z | Monolithic Power Systems | SOIC-8 with exposed pad | TODO |
| [TPS61291](TPS61291.md) | TPS61291DRVR | Texas Instruments | WSON-6 2×2 (DRV) | verified (datasheet SLVSBX9A (September 2014), page 3) |
| [TPS63070](TPS63070.md) | TPS63070RNMR | Texas Instruments | VQFN-HR-15 2.5×3 (RNM0015A, TI land pattern) | verified (datasheet SLVSC58B (revised March 2019), page 3) |
| [LTC3115-1](LTC3115-1.md) | LTC3115EDHD-1#PBF | Analog Devices | DFN-16 5×4 (DHD) | TODO |
| [TPS65219](TPS65219.md) | TPS6521901RHBR | Texas Instruments | VQFN-32 5×5 (RHB) | verified (datasheet SLVSGA0D (revised April 2026), page 4) |
| [PCA9430](PCA9430.md) | PCA9430 (confirm the orderable code) | NXP Semiconductors | confirm from the datasheet | TODO |
| [MAX77650](MAX77650.md) | MAX77650EWV+ | Analog Devices | WLP-30 (ball grid) | TODO |
| [TPS25750](TPS25750.md) | TPS25750DRJKR (confirm) | Texas Instruments | WQFN-38 (confirm) | TODO (NRND; comb land pattern not fully dimensioned) |
| [LTC6811](LTC6811.md) | LTC6811HG-1#PBF | Analog Devices | SSOP-48 5.3 mm | TODO |
| [S-8261](S-8261.md) | S-8261ABJMD-G3JT2U | ABLIC | SOT-23-6 | TODO |
| [LTC4215](LTC4215.md) | LTC4215CGN#PBF | Analog Devices | SSOP-16 (GN) | TODO |
| [TPS25947](TPS25947.md) | TPS259470ARPWR | Texas Instruments | VQFN-HR-10 2×2 (RPW0010A, TI land pattern) | verified (datasheet SLVSFC9C (revised May 2026), page 5) |
| [TPS22918](TPS22918.md) | TPS22918DBVR | Texas Instruments | SOT-23-6 (DBV) | verified (datasheet SLVSD76C (revised July 2017), page 3) |
| [LP5024](LP5024.md) | LP5024RSMR | Texas Instruments | VQFN-32 4×4 (RSM) | verified (datasheet SLVSEB8C (revised July 2024), page 5) |
| [DRV8323](DRV8323.md) | DRV8323SRTAR | Texas Instruments | WQFN-40 6×6 (RTA) | verified (datasheet SLVSDJ3D (revised March 2022), page 8) |
| [6EDL7141](6EDL7141.md) | 6EDL7141XUMA1 | Infineon | VQFN-48 7×7 (PG-VQFN-48-78) | verified (datasheet Revision 1.20 (2024-03-22), page 8) |
| [UCC28180](UCC28180.md) | UCC28180D | Texas Instruments | SOIC-8 (D) | verified (datasheet SLUSBQ5D (revised July 2016), page 4) |
| [TOP247](TOP247.md) | TOP247YN | Power Integrations | TO-220-7C (Y) | TODO |
| [IKW40N120H3](IKW40N120H3.md) | IKW40N120H3FKSA1 | Infineon | TO-247-3 (PG-TO247-3) | verified (datasheet Revision 1.20 (2025-06-12), page 1) |
| [IMW120R030M1H](IMW120R030M1H.md) | IMW120R030M1HXKSA1 | Infineon | TO-247-3 (PG-TO247-3) | verified (datasheet Revision 2.2 (2020-12-11), page 1) |
| [BZX55C5V1](BZX55C5V1.md) | BZX55C5V1 | Vishay | DO-35 axial | TODO |
| [SMBJ5.0A](SMBJ5.0A.md) | SMBJ5.0A | Littelfuse | SMB (DO-214AA) | TODO |
| [MB10S](MB10S.md) | MB10S | onsemi | SOIC-4 (MBS / TO-269AA) | TODO |
| [BT151](BT151.md) | BT151-500R | WeEn Semiconductors | TO-220AB | TODO |
| [ST25R3916](ST25R3916.md) | ST25R3916-AQWT | STMicroelectronics | QFN-32 5×5 | TODO |
| [DW3000](DW3000.md) | DW3110 (DW3000 family, confirm) | Qorvo | QFN-40 5×5 (confirm) | TODO |
| [LMX2594](LMX2594.md) | LMX2594RHAR | Texas Instruments | VQFN-40 6×6 (RHA) | verified (datasheet SNAS696C (revised April 2019), page 7) |
| [PE4259](PE4259.md) | PE4259 | pSemi (Murata) | SC-70-6 | TODO |
| [DP83867](DP83867.md) | DP83867IRRGZR | Texas Instruments | VQFN-48 7×7 (RGZ) | verified (datasheet SNLS484J (revised June 2026), page 5) |
| [KSZ9897](KSZ9897.md) | KSZ9897RTXC | Microchip | TQFP-128 14×14 with exposed pad (TX) | verified (datasheet DS00002330E, page 10) |
| [SN65HVD3082E](SN65HVD3082E.md) | SN65HVD3082EDR | Texas Instruments | SOIC-8 (D) | verified (datasheet SLLS562O (revised July 2023), page 4) |
| [LIS2DW12](LIS2DW12.md) | LIS2DW12TR | STMicroelectronics | LGA-12 2×2 (land pattern needed) | TODO |
| [ICM-42688-P](ICM-42688-P.md) | ICM-42688-P | TDK InvenSense | LGA-14 2.5×3 (land pattern needed) | TODO |
| [BMI270](BMI270.md) | BMI270 | Bosch Sensortec | LGA-14 2.5×3 (land pattern needed) | TODO |
| [MMC5983MA](MMC5983MA.md) | MMC5983MA | MEMSIC | LGA-16 3×3 (land pattern needed) | TODO |
| [BMP390](BMP390.md) | BMP390 | Bosch Sensortec | LGA-10 2×2 (land pattern needed) | TODO |
| [AH49E](AH49E.md) | AH49ENTR-G1 | Diodes Inc. | SOT-23-3 (or SIP-3 / TO-92S) | TODO |
| [VL53L1X](VL53L1X.md) | VL53L1CXV0FY/1 | STMicroelectronics | Optical LGA-12 4.9×2.5 (land pattern needed) | TODO |
| [VL53L5CX](VL53L5CX.md) | VL53L5CXV0GC/1 | STMicroelectronics | Optical LGA-16 6.4×3 (land pattern needed) | TODO |
| [MAX3735](MAX3735.md) | MAX3735AETG+ | Analog Devices | TQFN-24 4×4 | TODO |
| [LMP91000](LMP91000.md) | LMP91000SDX/NOPB | Texas Instruments | WSON-14 4×4 (NHL) | verified (datasheet SNAS506I (revised December 2014), page 3) |
| [BQ79616](BQ79616.md) | BQ79616PAPR | Texas Instruments | HTQFP-64 10×10 with exposed pad (PAP) | verified (datasheet SLUSF21A (revised June 2026), page 3) |
| [AD2S1210](AD2S1210.md) | AD2S1210BSTZ | Analog Devices | LQFP-48 7×7 | TODO |
| [Si8233](Si8233.md) | Si8233BB-D-IS1 | Skyworks (Silicon Labs) | SOIC-16W | TODO |
| [ISO7741](ISO7741.md) | ISO7741DWR | Texas Instruments | SOIC-16W (DW) | verified (datasheet SLLSEP4K (revised August 2026), page 5) |
| [SLB9670](SLB9670.md) | SLB9670VQ20FW785XTMA1 | Infineon | VQFN-32 5×5 (PG-VQFN-32-13) | verified (datasheet Revision 1.5 (2024-08-07), page 7) |
| [DS28E15](DS28E15.md) | DS28E15P+ | Analog Devices | TSOC-6 | TODO |
| [SN65DSI86](SN65DSI86.md) | SN65DSI86ZXHR | Texas Instruments | nFBGA-64 5×5 0.5 mm (ZXH) | verified (datasheet SLLSEH2C (revised October 2020), page 5) |
| [LT8912B](LT8912B.md) | LT8912B | Lontium | QFN-64 (confirm) | TODO |
| [WM8960](WM8960.md) | WM8960CGEFL/V | Cirrus Logic | QFN-32 5×5 | TODO |
| [TDC7200](TDC7200.md) | TDC7200PWR | Texas Instruments | TSSOP-14 (PW) | verified (datasheet SNAS647D (revised March 2016), page 4) |

### Large BGA, wafer-level and chip-on-glass parts

These have hundreds of balls, NDA datasheets or no PCB footprint of their own, so a hand-filled pin table is not the way
in. Import the vendor's ECAD model (**Library → Import library**, KiCad or Altium) or use the module that carries the chip.

| Part | Orderable example | Manufacturer | Package | Why |
|---|---|---|---|---|
| i.MX 8M Plus | MIMX8ML8DVNLZAB | NXP Semiconductors | FCBGA-548 15×15 | Application processor: hundreds of balls, power sequencing and DDR rules — use NXP's ECAD model |
| AM62x | AM6254 (ALW) | Texas Instruments | FCBGA-425 13×13 | Application processor — use TI's ECAD model |
| RK3588 | RK3588 | Rockchip | FCBGA-1088 23×23 | Application processor, datasheet under NDA |
| TMS320C6748 | TMS320C6748EZWTD4 | Texas Instruments | NFBGA-361 16×16 | DSP — use TI's ECAD model |
| ADSP-2156x | ADSP-21569KBCZ-10 | Analog Devices | CSP_BGA-400 | SHARC DSP — use ADI's ECAD model |
| Agilex | Agilex 7 family | Intel (Altera) | FBGA 1000+ balls | FPGA, pinout from Intel's pin-out files |
| CertusPro-NX | LFCPNX-100 | Lattice | BGA-256 / BGA-484 | FPGA, pinout from Lattice's package files |
| MachXO3 | LCMXO3LF-6900C-5BG256C | Lattice | caBGA-256 | FPGA, pinout from Lattice's package files |
| AD9234 | AD9234BCPZ-1000 | Analog Devices | LFCSP-64 9×9 | High-speed JESD204B ADC — use ADI's ECAD model |
| ADC12DJ3200 | ADC12DJ3200AAV | Texas Instruments | FCBGA-144 10×10 | RF-sampling ADC — use TI's ECAD model |
| DAC38RF82 | DAC38RF82IAAV | Texas Instruments | FCBGA-144 10×10 | RF-sampling DAC — use TI's ECAD model |
| AD9164 | AD9164BBCZ | Analog Devices | CSP_BGA-165 | RF DAC — use ADI's ECAD model |
| MAX86141 | MAX86141ENP+ | Analog Devices | WLP-20 | Optical AFE wafer-level package — use ADI's ECAD model |
| CYW43439 | CYW43439KUBG | Infineon | WLBGA-63 | Wi-Fi 4 + Bluetooth combo chip — usually used as a module (e.g. on Raspberry Pi Pico W) |
| CYW5557x | CYW55573 | Infineon | WLBGA | Wi-Fi 6E + Bluetooth combo chip — sold mainly as modules |
| AD9361 | AD9361BBCZ | Analog Devices | CSP_BGA-144 10×10 | RF transceiver — use ADI's ECAD model |
| ADRV9002 | ADRV9002BBCZ | Analog Devices | CSP_BGA-196 12×12 | RF transceiver — use ADI's ECAD model |
| AR0234 | AR0234CSSC00SUKA0 | onsemi | ODCSP / iBGA | Image sensor — use onsemi's ECAD model |
| BGT60TR13C | BGT60TR13C | Infineon | VQFN-32 / eWLB (confirm) | 60 GHz radar sensor with antennas in package — use Infineon's ECAD model and antenna keep-out |
| S32G | S32G274A | NXP Semiconductors | FCBGA-525 | Vehicle network processor — use NXP's ECAD model |
| TDA4VM | TDA4VM88 | Texas Instruments | FCBGA-827 | ADAS processor — use TI's ECAD model |
| AD5940 | AD5940BCBZ | Analog Devices | WLCSP-56 | Electrochemical / bioimpedance AFE — use ADI's ECAD model |
| ADuCM355 | ADuCM355BCCZ | Analog Devices | LGA-72 6×5 | Electrochemical sensor MCU — use ADI's ECAD model |
| SSD1306 | SSD1306 (COG die) | Solomon Systech | Chip-on-glass on the display panel | Display driver bonded to the OLED glass — use the OLED module (I2C / SPI header) instead |
| EPC23102 | EPC23102 | EPC | QFN-13 with 3 shaped power pads | GaN half-bridge whose power pads are custom polygons — import EPC's footprint |
| SSD1327 | SSD1327 (COG die) | Solomon Systech | Chip-on-glass on the display panel | Display driver bonded to the OLED glass — use the OLED module instead |

## How to complete a part (for Claude Code working locally)

1. **Download the datasheet** from the link in the part's file. Use the vendor's current revision.
2. **Fill in the pin table** in the part's file from the datasheet's pin-configuration table: every pin, in number order,
   with its name and type. Record the datasheet revision and the page of the pin table. Check the package fields
   (pin count, pitch, body size, exposed pad) against the package drawing and correct them if they differ.
3. **Add the part to the library** in `Core/src/StandardParts.cpp`, after the comment
   `// Catalog parts the KiCad library does not carry, from their datasheets.`, with the `catalog(...)` helper:

   ```cpp
   catalog(parts, "Motor Control", "DRV8301DCAR", "Texas Instruments",
           "Three-phase gate driver with dual current-sense amplifiers and 1.5 A buck regulator",
           "U", "TSSOP", 56, 0.5, 6.1, "HTSSOP-56 (DCA)",
           {{"1", "RT_CLK", T::Input},
            /* ... every pin from the table ... */
            {"EP", "PGND", T::PowerIn}});
   ```

   Arguments: category, part name, manufacturer, description, reference prefix, SiEDA package type, pin count, pitch (mm),
   body size (mm), footprint label, then `{number, name, type}` for each pin. Add the exposed pad as pin `"EP"` (supported on
   `QFN`, `TSSOP`, `LQFP`, `SON` and `MODULE`); it is not counted in the pin count.
4. **Add the part number to the tests**:
   - `Core/tests/core_tests.cpp` → `TEST(production_parts_catalog)`, the `catalog[]` list.
   - `SiEDATests/SiEDATests.swift` → `testCatalogPartsAreInTheLibraryAndPlace`, the `names` list.
   - For microcontrollers (TMS320F28379D, ATSAMD51P20A), bump the vendor count in `TEST(microcontroller_library_by_vendor)`
     and `testTenMicrocontrollersPerVendorWithRealPackages`.
5. **Mark the part done**: set `Status: verified (datasheet <revision>, page <n>)` in its file and in the table above, and
   remove the part number from the "Not in the library yet" paragraph in the top-level `README.md`.
6. **Build and test**:

   ```sh
   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ./build/sieda_core_tests
   ./run.sh
   ```

   Then commit and open a pull request. One part per commit keeps review easy.

Never guess a pin: if the datasheet is ambiguous (several package variants, a pin shared between functions), use the name
the datasheet's pin table gives for this exact orderable part number and note the choice in the part's file.
