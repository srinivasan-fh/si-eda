// SiEDA Core — the complete fabrication package: everything a board house and an assembly house need to build the
// design, laid out the way they expect, with an order sheet and a ready-to-upload Gerber zip.
#pragma once

#include <string>
#include <vector>

#include "sieda/Project.hpp"

namespace sieda {

struct FabFile {
    std::string path;         // relative to the package folder, e.g. "gerbers/board-F_Cu.gbr"
    std::string content;      // text, or binary for the zip
    std::string description;  // one line for the order notes / UI
};

/// Every manufacturing file, generated in memory:
///  - gerbers/: copper (every layer), solder mask, solder paste (stencil), silkscreen with reference designators,
///    board outline, plated + non-plated Excellon drills, Gerber X2 job file (stack-up) and IPC-D-356A test netlist;
///  - assembly/: BOM, assembly-house BOM and CPL, pick-and-place, assembly drawings (SVG) for each populated side;
///  - fab_notes.txt: the order sheet (size, layers, thickness, finish, mask/silk colours, minimum features, counts);
///  - <base>-gerbers.zip: the gerbers/ folder zipped for upload to the fab;
///  - netlist.cir and 3d/<base>.stl.
/// `base` names the files ("" → the project name). Unpopulated sides and absent features produce no file.
std::vector<FabFile> fabricationPackage(const Project& project, const std::string& base = "");

/// Writes the package into `dir` (created if needed). Returns false with `error` set on an I/O failure.
bool writeFabricationPackage(const Project& project, const std::string& dir, std::vector<std::string>* written,
                             std::string* error, const std::string& base = "");

/// The order sheet on its own (also part of the package as fab_notes.txt).
std::string fabricationNotes(const Project& project, const std::vector<FabFile>& files);

/// Gerber X2 job file (.gbrjob, JSON): board size, layer count, thickness, finish, design rules, stack-up with mask and
/// silkscreen colours, and the function of every Gerber file in `files`.
std::string exportGerberJob(const Project& project, const std::vector<FabFile>& files);

/// A .zip archive (stored, no compression) of the given files.
std::string makeZip(const std::vector<std::pair<std::string, std::string>>& files);

/// "ENIG" for fine-pitch parts (flat pads), otherwise "HASL lead-free".
std::string recommendedSurfaceFinish(const Project& project);

}  // namespace sieda
