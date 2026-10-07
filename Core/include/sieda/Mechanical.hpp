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

/// 3D clearance checks (part of the DRC): MECH_HEIGHT (a part taller than its side's enclosure limit),
/// MECH_HEIGHT_ZONE (taller than a height zone it stands in) and MECH_BODY_COLLISION (two fitted parts' bodies on
/// the same side overlap). Parts not fitted (DNP) are left out. No limits set: only collisions are checked.
std::vector<RuleViolation> mechanicalChecks(const Schematic& sch, const PcbLayout& pcb);

/// {"maxHeightTop","maxHeightBottom","zones":[{"name","x0","y0","x1","y1","bottom","maxHeight"}]}; null when unset.
Json mechanicalLimitsToJson(const BoardSettings& s);
/// Reads the limits into `s` (invalid zones are skipped; null leaves no limits).
void mechanicalLimitsFromJson(const Json& j, BoardSettings& s);

}  // namespace sieda
