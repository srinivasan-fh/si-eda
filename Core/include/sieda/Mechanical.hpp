// SiEDA Core — exchange with mechanical CAD: STEP AP214 solids of the assembled board and IDF 3.0 board / library
// files, with the placement read back from an IDF board file (MCAD moved a connector or a mounting part).
// Coordinates written are MCAD's: X = board X, Y = −board Y (up), Z = up with the board's underside at Z = 0.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// STEP AP214 (ISO 10303-21): the board (outline extruded to its thickness, mounting holes cut) and one closed,
/// coloured, named solid per placed part (its package body; designator as the solid's name). Faceted B-rep, so
/// every MCAD tool reads it (SolidWorks, Fusion, Creo, NX, Inventor, FreeCAD / OpenCASCADE).
std::string exportStep(const Schematic& sch, const PcbLayout& pcb, const std::string& name);

/// IDF 3.0 board file (.emn): outline with thickness, mounting holes, placement of every placed part.
std::string exportIdfBoard(const Schematic& sch, const PcbLayout& pcb, const std::string& name);
/// IDF 3.0 library file (.emp): one ELECTRICAL outline with height per package / part-number pair the board uses.
std::string exportIdfLibrary(const Schematic& sch, const PcbLayout& pcb);

/// Reads the PLACEMENT section of an IDF board file and moves the parts it names (by designator) to its position,
/// rotation and side. Returns the designators moved; unknown designators and unchanged parts are left out.
std::vector<std::string> importIdfPlacement(Schematic& sch, const std::string& emn);

/// ProSTEP iViP EDMD (IDX) v4.5 baseline (SendInformation): the board outline extruded to its thickness with its
/// mounting holes, and every placed part as a package item (body outline and height) plus an instance with its
/// designator (REFDES), side (AssembleToName TOP / BOTTOM) and 2D transformation. MCAD XY, Y up, millimetres.
std::string exportIdx(const Schematic& sch, const PcbLayout& pcb, const std::string& name);
/// IDX SendChanges with only the parts placed differently from `baselineIdx` (moved, rotated, flipped or new);
/// empty when nothing changed.
std::string exportIdxChanges(const Schematic& sch, const PcbLayout& pcb, const std::string& name,
                             const std::string& baselineIdx);
/// Reads an IDX baseline or change file (from MCAD or SiEDA) and moves, rotates and flips the parts it names (by
/// REFDES, else the instance name). Rotations snap to 90°. Returns the designators changed; malformed files change
/// nothing.
std::vector<std::string> importIdxPlacement(Schematic& sch, const std::string& idx);
/// What an IDX import changed.
struct IdxImport {
    std::vector<std::string> moved;  // designators
    bool outlineChanged = false, thicknessChanged = false;
    int keepouts = 0, heightZones = 0;  // added from MCAD
};
/// The full MCAD import: placements, the board outline and thickness (BOARD_OUTLINE item), routing / via keep-outs
/// (KEEPOUT_AREA_ROUTE / _VIA → RouteKeepout) and component keep-outs (KEEPOUT_AREA_COMPONENT / _PLACEMENT → a
/// HeightZone whose limit is the keep-out's height, 0 = no parts). Keep-outs from an earlier MCAD import ("MCAD …")
/// are replaced; SiEDA's own stay. Malformed files change nothing.
IdxImport importIdx(Schematic& sch, BoardSettings& s, const std::string& idx);
/// The IDX response to an MCAD change file: every change it proposes (computational:Change / NewItem) accepted or
/// rejected.
std::string idxResponse(const std::string& changesIdx, bool accept);

/// 3D clearance checks (part of the DRC): MECH_HEIGHT (a part taller than its side's enclosure limit),
/// MECH_HEIGHT_ZONE (taller than a height zone it stands in) and MECH_BODY_COLLISION (two fitted parts' bodies on
/// the same side overlap). Parts not fitted (DNP) are left out. No limits set: only collisions are checked.
std::vector<RuleViolation> mechanicalChecks(const Schematic& sch, const PcbLayout& pcb);

/// {"maxHeightTop","maxHeightBottom","zones":[{"name","x0","y0","x1","y1","bottom","maxHeight"}]}; null when unset.
Json mechanicalLimitsToJson(const BoardSettings& s);
/// Reads the limits into `s` (invalid zones are skipped; null leaves no limits).
void mechanicalLimitsFromJson(const Json& j, BoardSettings& s);

}  // namespace sieda
