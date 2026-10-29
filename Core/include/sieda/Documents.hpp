// SiEDA Core — documents for mechanical exchange, test and bring-up: DXF (board outline, cutouts, holes, drills,
// silk / fab lines) both ways, the test-point report for fixture builders, a self-contained interactive HTML BOM and
// board viewer for hand assembly, and a bring-up checklist drawn from the design's rails, clocks, resets and buses.
#pragma once

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Json.hpp"

namespace sieda {

class Project;
class Schematic;
class PcbLayout;

/// AutoCAD R12 ASCII DXF of the board in millimetres, Y up (MCAD convention, origin at the board's lower-left):
/// layers OUTLINE (outline and cutouts), HOLES (mounting holes), DRILLS (plated pad holes and vias), SILK_TOP /
/// SILK_BOTTOM and FAB (the footprints' drawn lines) and COURTYARD.
std::string exportDxf(const Schematic& sch, const PcbLayout& pcb);

/// Closed loops read from a DXF: LINE / ARC chains, LWPOLYLINE and POLYLINE (closed or end-to-start), CIRCLE.
/// Millimetres (inches by $INSUNITS 1), Y flipped into board coordinates with the lower-left corner at the origin.
/// The largest loop is the outline, every other loop inside it a cutout.
struct DxfOutline {
    std::vector<Vec2> outline;
    std::vector<std::vector<Vec2>> cutouts;
    double width = 0, height = 0;
    std::string error;  // empty on success
};
DxfOutline importDxfOutline(const std::string& text);
/// Applies a read outline to the board: size, outline polygon and cutouts. False (board unchanged) on an error.
bool applyDxfOutline(PcbLayout& pcb, const DxfOutline& dxf);

/// Test-point report (CSV) for bed-of-nails / flying-probe fixtures: one row per probe point — test-point parts
/// (TP…) first, then for each other net its best accessible pad (through-hole: both sides; else an SMD pad of at
/// least 0.6 mm on the outer side) — and a row per net no probe can reach.
std::string exportTestPointReport(const Schematic& sch, const PcbLayout& pcb);

/// One self-contained HTML file (no network): the board drawn per side with every part's pads and courtyard, and the
/// BOM grouped by value and footprint; clicking a BOM line highlights its parts, a checkbox per line tracks hand
/// assembly.
std::string exportHtmlBom(const Project& project);
/// SHA-256 of `data` as 64 lower-case hex digits (release manifests, `shasum -a 256` compatible).
std::string sha256Hex(const std::string& data);

/// Bring-up checklist (Markdown): safety first (shorts on every rail before power), then rails with their expected
/// voltage and source, clocks, reset / enable / boot nets, programming and bus interfaces, each with what to measure.
std::string bringUpChecklist(const Project& project);

/// Fabrication drawing (A3 PDF, written in SchematicPdf.cpp beside the schematic PDF): the board outline and
/// cutouts with overall and user dimensions (BoardSettings::dimensions), a drill map with one symbol per finished
/// hole size and plating, the drill table, the stack-up as the layer legend, fabrication notes, the title block and
/// the revision table (Project::revisions); with `views3d` a second sheet: the assembly isometric (rigid-flex folded),
/// exploded and section A-A (through the board's middle, marked on sheet 1) views.
std::string exportBoardDrawingPdf(const Project& project, bool views3d = true);

/// A Gerber (RS-274X / X2), Excellon or ODB++ layer features file (kind "odb") read for viewing: strokes, flashes and
/// regions in mm, Y up as in the file.
/// Apertures C / R / O / P draw as such; macro apertures as a circle of their first size (counted in `warnings`).
/// Clear polarity (%LPC) shapes are kept with `clear`. Excellon holes are flashes of their tool's diameter.
struct GerberShape {
    enum Kind { Stroke, Flash, Region } kind = Stroke;
    Vec2 a, b;                // stroke ends; flash centre in a
    double width = 0, height = 0;  // stroke width; flash size (w × h)
    char aperture = 'C';      // flash shape: C circle, R rectangle, O obround, P polygon
    std::vector<Vec2> points; // region outline
    bool clear = false;
};
struct GerberImage {
    std::string kind;  // "gerber", "drill" or "odb"
    std::vector<GerberShape> shapes;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds (mm)
    std::vector<std::string> warnings;
    std::string error;  // empty on success
};
GerberImage parseGerber(const std::string& text);
/// {"kind","bounds":[x0,y0,x1,y1],"strokes":[[x0,y0,x1,y1,w,clear]],"flashes":[[x,y,w,h,"C",clear]],
///  "regions":[[clear,x,y,x,y,…]],"warnings":[…],"error"} (clear 1 = clear polarity) — the C API's and the app's form.
std::string gerberImageJson(const GerberImage& image);

/// The files of a .zip archive (stored or deflated entries), path → bytes; folders are skipped. Throws
/// std::runtime_error on a malformed or ZIP64 archive, more than 2000 entries or more than 256 MB unpacked.
std::map<std::string, std::string> readZipArchive(const std::string& bytes);
/// `image` painted on a w × h grid of `res` mm cells from (x0, y0) (row 0 at the bottom): 1 copper, 0 none; clear
/// polarity shapes erase, in file order.
std::vector<uint8_t> rasterizeGerber(const GerberImage& image, double x0, double y0, double res, int w, int h);
/// Two fabrication packages (path → bytes, e.g. a folder's files or readZipArchive) compared: each Gerber layer paired
/// by its X2 file function (else by its file name without the board name every file starts with) and compared on a
/// `resolution` mm grid — status same / changed / added / removed, copper area added and removed and where (the
/// changed patches, largest first); each drill file hole by hole (added, removed, resized); the other files by their
/// bytes. {"identical", "layers": [...], "otherFiles": [...]}.
Json compareFabPackages(const std::map<std::string, std::string>& a, const std::map<std::string, std::string>& b,
                        double resolution = 0.05);

/// Net-length report (CSV): per routed net its track length (arcs measured as arcs), segments, vias and, with a
/// length rule, the target, tolerance and whether it is met.
std::string exportNetLengthReport(const Schematic& sch, const PcbLayout& pcb);

/// A part's pin map for firmware and FPGA tools, from the schematic: "header" (C #defines naming each signal net's pin;
/// STM32-style PA5 pins also get CubeMX's NAME_Pin / NAME_GPIO_Port), "xdc" (Vivado PACKAGE_PIN), "qsf" (Quartus
/// set_location_assignment) or "csv" (pin, name, net). Power, ground and unnamed nets are left out. Empty when `ref`
/// is not on the schematic or the format is unknown.
std::string exportPinMap(const Schematic& sch, const std::string& ref, const std::string& format);
/// The part's package for a graphical pin planner: {ref, package, width, height (courtyard, mm), pads:[{x, y, w, h, round
/// (footprint frame, mm), number, name, type, net, role ("signal" / "power" / "ground" / "negative" / "none")}]}; null
/// for an unknown part or one without a footprint. A pad with no pin (mechanical) has no number.
Json pinPlanJson(const Schematic& sch, const std::string& ref);
/// Pin → net assignments from an STM32CubeMX project (.ioc: PA5.GPIO_Label / .Signal), a Vivado .xdc, a Quartus .qsf or
/// a two-column CSV (pin, net); pins as the file names them (a pin name such as PA5, or a package pin such as E3).
std::vector<std::pair<std::string, std::string>> parsePinAssignments(const std::string& text);

/// BOM as CSV with the columns asked for, in that order: item, designators, quantity, value, type, footprint,
/// description, rating, manufacturer, mpn, supplier_part, unit_price, line_cost, dnp, notes, boards (unknown names are
/// skipped; none = the standard set). `board` >= 0 keeps the parts on that board of a multi-board project; -1 = one
/// consolidated BOM across every board (the boards column names where each line's parts go).
std::string exportBomColumns(const Project& project, const std::vector<std::string>& columns, int board = -1);

}  // namespace sieda
