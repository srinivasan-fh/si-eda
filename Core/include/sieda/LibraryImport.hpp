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

/// Imported KiCad / Altium footprints that have a pad for every pin of `symbol`, best first: the footprint the symbol
/// names, then those with exactly one pad per pin, those its footprint filters accept, the fewest extra pads, by name.
std::vector<std::string> footprintCandidates(const LibraryImport& result, const ImportedSymbol& symbol);

/// {"parts":[{name,symbol,footprint,source,ok,error,warnings,spec, pairable?, candidates?}],
///  "footprintList":[{name,source,pads}],"files":[{name,format,symbols,footprints,error}],
///  "symbols":n,"footprints":n}
Json libraryImportToJson(const LibraryImport& result);

/// The C API's entry point: request {"files":[{"name","content"}],"pairs":{"symbol":"footprint"}}.
Json importLibraryRequest(const Json& request);

/// One part of an imported netlist: designator, value, footprint and library part name as the file gives them, and
/// pin names by number where the file has them (KiCad's pinfunction).
struct NetlistPart {
    std::string ref, value, footprint, part;
    std::map<std::string, std::string> pinNames;
    /// A board import's exact land pattern (type CUSTOM, top view, centred like an imported footprint; land `pin` =
    /// pad number, "-" = none) and body; no lands otherwise.
    PackageSpec package;
};
/// A netlist read from another tool: its parts and its nets, each a list of (designator, pin number).
struct ParsedNetlist {
    std::string format;  // "kicad", "protel", "csv", "kicad_sch", "ltspice"
    std::vector<NetlistPart> parts;
    std::vector<std::pair<std::string, std::vector<std::pair<std::string, std::string>>>> nets;
    std::vector<std::string> warnings;  // what a schematic import could not carry over
};
/// Reads a KiCad netlist (.net, "(export …)"), a Protel / Tango netlist ([ part ] and ( net ) blocks, also written by
/// Altium, OrCAD, EasyEDA and PADS) or a CSV with Net, Designator (Ref / Reference) and Pin columns. Parts the nets
/// name but the file does not list are added with only their designator. Throws ImportError for anything else.
ParsedNetlist parseNetlist(const std::string& text);
/// A KiCad schematic sheet (.kicad_sch, KiCad 6 – 10) read for its connectivity: each symbol's pins placed by its
/// position, rotation and mirror, joined by wires (an end on another wire joins it), junctions, labels (local, global,
/// hierarchical: by name) and power symbols (by value). Multi-unit parts become one part. Sub-sheets are not followed
/// (listed in warnings). parseNetlist reads these too.
ParsedNetlist parseKicadSchematic(const std::string& text);
/// An LTspice schematic (.asc): resistors, capacitors, inductors, voltage and current sources, diodes and LEDs (the
/// built-in symbols, any rotation / mirror) joined by wires and flags ("0" = GND). Parts carry their kind in `part`
/// as "kind:<resistor|capacitor|inductor|voltage_source|current_source|diode|led>"; other symbols are listed in
/// warnings. parseNetlist reads these too.
ParsedNetlist parseLtspiceSchematic(const std::string& text);

/// A KiCad board (.kicad_pcb, KiCad 6 – 10) read for placement and copper, in KiCad's page coordinates (mm, y down):
/// each footprint's designator, position, angle (degrees, counter-clockwise as drawn) and side with its pads where
/// they stand on the board; tracks (straight and arcs), vias, copper zones and the Edge.Cuts outline as a DXF that
/// importDxfOutline reads (`edgeMin` = the top-left corner of its extent, where the board's origin goes).
/// `netlist` holds the parts (value, footprint, exact land pattern) and the nets their pads name. Copper layers:
/// 0 = F.Cu, n = In<n>.Cu, layerCount - 1 = B.Cu.
struct KicadBoard {
    struct Pad { std::string number, net; Vec2 at; };
    struct Footprint { std::string ref; Vec2 at; double angle = 0; bool bottom = false; std::vector<Pad> pads; };
    struct Copper { std::string net; int layer = 0; double width = 0; Vec2 a, b, mid; bool arc = false; };
    struct Hole { std::string net; Vec2 at; double diameter = 0, drill = 0; int from = 0, to = -1; };
    struct Zone { std::string net; int layer = 0; std::vector<Vec2> polygon; };
    ParsedNetlist netlist;
    std::vector<Footprint> footprints;
    std::vector<Copper> tracks;
    std::vector<Hole> vias;
    std::vector<Zone> zones;
    std::string outlineDxf;
    Vec2 edgeMin;
    int layerCount = 2;
    double thickness = 1.6;
};
KicadBoard parseKicadPcb(const std::string& text);

}  // namespace sieda
