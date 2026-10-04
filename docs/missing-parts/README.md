# Missing IC parts

These 14 parts from the production catalog are in neither the KiCad symbol library nor a datasheet that could be fetched
from the cloud build environment, so they are not in the SiEDA library yet. Each file below holds everything known about the
part (package, pitch, body, datasheet link) and an empty pin table to fill in from the datasheet.

| File | Part number | Manufacturer | Package | Status |
|---|---|---|---|---|
| [TMS320F28379D](TMS320F28379D.md) | TMS320F28379DPTPS | Texas Instruments | HLQFP-176 (PTP) | TODO |
| [DRV8301](DRV8301.md) | DRV8301DCAR | Texas Instruments | HTSSOP-56 (DCA) | TODO |
| [DRV8353](DRV8353.md) | DRV8353RTAWRQ1 | Texas Instruments | WQFN-40 6×6 (RTA) | TODO |
| [TPS65381](TPS65381.md) | TPS65381AQDAPRQ1 | Texas Instruments | HTSSOP-32 (DAP) | TODO |
| [LM5116](LM5116.md) | LM5116MH/NOPB | Texas Instruments | HTSSOP-20 (PWP) | TODO |
| [TJA1101](TJA1101.md) | TJA1101HN | NXP Semiconductors | HVQFN-36 6×6 | TODO |
| [MA702](MA702.md) | MA702GQ | Monolithic Power Systems | QFN-16 3×3 | TODO |
| [BMX160](BMX160.md) | BMX160 | Bosch Sensortec | LGA-14 2.5×3.0 | TODO |
| [ADS1256](ADS1256.md) | ADS1256IDBR | Texas Instruments | SSOP-28 (DB) | TODO |
| [ATSAMD51P20A](ATSAMD51P20A.md) | ATSAMD51P20A-AUT | Microchip | TQFP-128 14×14 | TODO |
| [INMP441](INMP441.md) | INMP441ACEZ-R7 | TDK InvenSense | LGA 4.72×3.76 | TODO |
| [JQ6500](JQ6500.md) | JQ6500 | Jiaqi (JQ) | SOP-16 | TODO |
| [WT2003HP8](WT2003HP8.md) | WT2003HP8-16S | Waytronic | SOP-16 | TODO |
| [SYN6288](SYN6288.md) | SYN6288 | Beijing Yutone (宇音天下) | SSOP-28 | TODO |

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
