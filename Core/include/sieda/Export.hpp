// SiEDA Core — manufacturing & interchange exports.
#pragma once

#include <string>
#include <vector>

#include "sieda/Mesh.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class GerberLayer { TopCopper, BottomCopper, TopMask, BottomMask, TopSilk, EdgeCuts, TopPaste, BottomPaste, BottomSilk };

std::string exportSpiceNetlist(const Schematic& sch, const std::string& title);
std::string exportBomCsv(const Schematic& sch);
std::string exportPickAndPlaceCsv(const Schematic& sch);
std::string exportGerber(const Schematic& sch, const PcbLayout& pcb, GerberLayer layer);
/// Copper Gerber for any layer of the stack-up (0 = top … layerCount-1 = bottom).
std::string exportCopperGerber(const Schematic& sch, const PcbLayout& pcb, int copperLayer);
/// Excellon drill file: plated holes (pads, vias) or, with `plated` false, the non-plated mounting holes.
std::string exportExcellonDrill(const Schematic& sch, const PcbLayout& pcb, bool plated = true);
/// IPC-D-356A bare-board netlist (every pad and via with its net): fabs use it for the electrical (flying-probe) test.
std::string exportIpcD356(const Schematic& sch, const PcbLayout& pcb, const std::string& jobName);
/// Assembly BOM in the column layout assembly houses (JLCPCB, PCBWay) import: Comment, Designator, Footprint, part #.
std::string exportAssemblyBomCsv(const Schematic& sch);
/// Component placement list (CPL) for assembly: Designator, Mid X, Mid Y, Layer, Rotation.
std::string exportCplCsv(const Schematic& sch, const PcbLayout& pcb);
/// Assembly drawing (SVG) of one side: board outline, part outlines, designators and pin-1 marks (bottom mirrored).
std::string exportAssemblySvg(const Schematic& sch, const PcbLayout& pcb, bool bottom, const std::string& title);
/// Strokes of `text` in the vector font used on the silkscreen (board mm, y down), `height` mm tall, starting at the
/// baseline point `origin`; `mirror` for bottom-side text (reads correctly from below).
std::vector<std::vector<Vec2>> silkscreenText(const std::string& text, Vec2 origin, double height, bool mirror);
double silkscreenTextWidth(const std::string& text, double height);
std::string exportStl(const Mesh& mesh, const std::string& name);
std::string exportObj(const Mesh& mesh, const std::string& name);

}  // namespace sieda
