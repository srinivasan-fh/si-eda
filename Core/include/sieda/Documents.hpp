// SiEDA Core — documents for mechanical exchange, test and bring-up: DXF (board outline, cutouts, holes, drills,
// silk / fab lines) both ways, the test-point report for fixture builders, a self-contained interactive HTML BOM and
// board viewer for hand assembly, and a bring-up checklist drawn from the design's rails, clocks, resets and buses.
#pragma once

#include <string>
#include <vector>

#include "sieda/Geometry.hpp"

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

/// Bring-up checklist (Markdown): safety first (shorts on every rail before power), then rails with their expected
/// voltage and source, clocks, reset / enable / boot nets, programming and bus interfaces, each with what to measure.
std::string bringUpChecklist(const Project& project);

/// Fabrication drawing (one A3 PDF page, written in SchematicPdf.cpp beside the schematic PDF): the board outline and
/// cutouts with overall and user dimensions (BoardSettings::dimensions), a drill map with one symbol per finished
/// hole size and plating, the drill table, the stack-up as the layer legend, fabrication notes, the title block and
/// the revision table (Project::revisions).
std::string exportBoardDrawingPdf(const Project& project);

/// A Gerber (RS-274X / X2) or Excellon file read for viewing: strokes, flashes and regions in mm, Y up as in the file.
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
    std::string kind;  // "gerber" or "drill"
    std::vector<GerberShape> shapes;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;  // bounds (mm)
    std::vector<std::string> warnings;
    std::string error;  // empty on success
};
GerberImage parseGerber(const std::string& text);
/// {"kind","bounds":[x0,y0,x1,y1],"strokes":[[x0,y0,x1,y1,w,clear]],"flashes":[[x,y,w,h,"C",clear]],
///  "regions":[[clear,x,y,x,y,…]],"warnings":[…],"error"} (clear 1 = clear polarity) — the C API's and the app's form.
std::string gerberImageJson(const GerberImage& image);

/// Net-length report (CSV): per routed net its track length (arcs measured as arcs), segments, vias and, with a
/// length rule, the target, tolerance and whether it is met.
std::string exportNetLengthReport(const Schematic& sch, const PcbLayout& pcb);

}  // namespace sieda
