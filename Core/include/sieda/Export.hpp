// SiEDA Core — manufacturing & interchange exports.
#pragma once

#include <map>
#include <string>
#include <utility>
#include <vector>

#include "sieda/Mesh.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class GerberLayer { TopCopper, BottomCopper, TopMask, BottomMask, TopSilk, EdgeCuts, TopPaste, BottomPaste, BottomSilk };

std::string exportSpiceNetlist(const Schematic& sch, const std::string& title);
std::string exportBomCsv(const Schematic& sch);
/// Pick and place, every column: lower-left origin (Y up), rotation counter-clockwise from the top; no DNP parts.
std::string exportPickAndPlaceCsv(const Schematic& sch, const PcbLayout& pcb);
std::string exportGerber(const Schematic& sch, const PcbLayout& pcb, GerberLayer layer);
/// Copper Gerber for any layer of the stack-up (0 = top … layerCount-1 = bottom).
std::string exportCopperGerber(const Schematic& sch, const PcbLayout& pcb, int copperLayer);
/// User drawing layer `user` (1–4) as a Gerber (FileFunction Other,User<n>); empty when it has no graphics.
std::string exportUserLayerGerber(const PcbLayout& pcb, int user);
/// Excellon drill file: plated holes (pads, vias) or, with `plated` false, the non-plated mounting holes.
std::string exportExcellonDrill(const Schematic& sch, const PcbLayout& pcb, bool plated = true);
/// Distinct (first, last) copper-layer spans of the board's blind / buried / micro vias.
std::vector<std::pair<int, int>> viaSpans(const PcbLayout& pcb);
/// Excellon file for the vias spanning exactly `fromLayer`…`toLayer` (one per span, as HDI fabs drill them).
std::string exportExcellonSpan(const PcbLayout& pcb, int fromLayer, int toLayer);
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
/// Each placed part's silkscreen designator box (`height` mm text; y1 = its baseline): above the part, else below,
/// right or left of it, the first spot clear of the designators placed before it, of other parts' outlines and of the
/// board edge (above / below at the edge as before when none is). The silkscreen Gerber, IPC-2581 / ODB++, the DFA
/// check and the renders all place designators so.
std::map<int, Rect> designatorBoxes(const Schematic& sch, const PcbLayout& pcb, double height = 1.0);
std::string exportStl(const Mesh& mesh, const std::string& name);
std::string exportObj(const Mesh& mesh, const std::string& name);
/// glTF 2.0 binary (.glb): positions, normals and vertex colours, in metres. Empty for an empty mesh.
std::string exportGltf(const Mesh& mesh, const std::string& name);
/// VRML 2.0 (.wrl) with vertex colours, in metres.
std::string exportVrml(const Mesh& mesh, const std::string& name);

}  // namespace sieda
