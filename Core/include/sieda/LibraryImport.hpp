// SiEDA Core — importing third-party part libraries into custom parts.
//
// Reads KiCad footprints (.kicad_mod), KiCad symbol libraries (.kicad_sym, one or many symbols, multi-unit) and
// Eagle libraries (.lbr: packages, symbols, device sets), pairs each symbol with its footprint and turns the pair
// into a CustomPartSpec: the symbol's pins and pin layout, and the footprint as an exact land pattern (package type
// "CUSTOM"). Every part is validated with the same checks the Symbol and Footprint editors use (checkSymbol,
// checkLandPattern) and by registering it. Malformed input never crashes: a file that cannot be read is reported
// with its line number and the other files still import.
#pragma once

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

#include "sieda/CustomParts.hpp"
#include "sieda/Json.hpp"

namespace sieda {

/// A file that cannot be read (syntax error, unsupported format, limits exceeded). The message names the line.
struct ImportError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct ImportFile {
    std::string name;     // file name, used to detect the format by extension and in messages
    std::string content;  // the file's text (or bytes: binary STL, Altium libraries)
};

/// A footprint as a land pattern. Lands are in file order, y down, centred on the courtyard (or the pads and body
/// when there is none); `padNumbers[i]` is land i's pad number as written in the file ("" = unnumbered).
struct ImportedFootprint {
    std::string name;
    std::string description;
    std::string source;  // file name
    PackageSpec package;  // type "CUSTOM", lands, body size
    std::vector<std::string> padNumbers;
    std::vector<std::string> warnings;
    /// The 3D model the footprint names (KiCad `(model …)`, the first one not hidden), as written, and its offset
    /// (mm), scale and rotation already in SiEDA's sense (Model3DRef; id and unit are filled when the model file is
    /// imported too).
    std::string modelPath;
    Model3DRef modelAlign;
    /// How far the pads were moved to centre the land pattern (mm, PCB axes, y down): the model moves with them.
    double centreX = 0, centreY = 0;
};

/// A schematic symbol: its pins (one per pin number, all units merged) and their layout.
struct ImportedSymbol {
    std::string name;
    std::string refPrefix = "U";
    std::string value;
    std::string footprint;  // the footprint the library names for it ("Package_SO:SOIC-8_3.9x4.9mm_P1.27mm")
    std::vector<std::string> footprintFilters;  // KiCad ki_fp_filters: footprint name patterns ("SOIC*3.9x4.9mm*")
    std::string datasheet;
    std::string description;
    std::string manufacturer;
    std::string source;
    std::vector<CustomPin> pins;
    SymbolSpec symbol;
    int units = 1;
    /// A derived symbol whose base symbol is not in its file (KiCad keeps each symbol of a .kicad_symdir in its own
    /// file): importLibraryFiles fills the pins from the base when that file is imported too.
    std::string extends;
    std::vector<std::string> warnings;
};

/// One importable part. `ok` parts register as they are; otherwise `error` says what is missing or wrong.
struct ImportedPart {
    CustomPartSpec spec;
    std::string symbolName;     // the symbol it was built from ("" = footprint only)
    std::string footprintName;  // the footprint it uses ("" = a generated package)
    std::string source;
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
};

struct LibraryImport {
    struct FileResult {
        std::string name;
        std::string format;  // "kicad_mod", "kicad_sym", "eagle_lbr", "unknown"
        int symbols = 0, footprints = 0;
        std::string error;
    };
    std::vector<FileResult> files;
    std::vector<ImportedSymbol> symbols;
    std::vector<ImportedFootprint> footprints;
    std::vector<ImportedPart> parts;
};

/// Parsers. Each throws ImportError on input it cannot read.
ImportedFootprint parseKicadFootprint(const std::string& text, const std::string& source = {});
std::vector<ImportedSymbol> parseKicadSymbols(const std::string& text, const std::string& source = {});
/// Eagle library: its packages are added to `into.footprints`, each device (deviceset × package variant ×
/// technology) to `into.parts`, already paired by the device's connects.
void parseEagleLibrary(const std::string& text, const std::string& source, LibraryImport& into);

/// Builds and validates a part from a symbol and / or a footprint (at least one). Without a footprint the package
/// is generated from the package name in the symbol's footprint field when SiEDA knows it.
ImportedPart makeImportedPart(const ImportedSymbol* symbol, const ImportedFootprint* footprint);

/// Imports a set of files. Symbols pair with an imported footprint by `pairs` (symbol name → footprint name), else by
/// the footprint the symbol names, else by its footprint filters (the first match with a pad for every pin), else —
/// when exactly one footprint was imported — with that one if it has a pad for every pin. Derived symbols take their
/// base symbol's pins from any imported file. Footprints no symbol uses become parts of their own (one passive pin
/// per pad number).
LibraryImport importLibraryFiles(const std::vector<ImportFile>& files, const std::map<std::string, std::string>& pairs = {});

/// {"parts":[{name,symbol,footprint,source,ok,error,warnings,spec}],"files":[{name,format,symbols,footprints,error}],
///  "symbols":n,"footprints":n}
Json libraryImportToJson(const LibraryImport& result);

/// The C API's entry point: request {"files":[{"name","content"}],"pairs":{"symbol":"footprint"}}.
Json importLibraryRequest(const Json& request);

}  // namespace sieda
