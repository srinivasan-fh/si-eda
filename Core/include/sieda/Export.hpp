// SiEDA Core — manufacturing & interchange exports.
#pragma once

#include <string>

#include "sieda/Mesh.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

enum class GerberLayer { TopCopper, BottomCopper, TopMask, BottomMask, TopSilk, EdgeCuts };

std::string exportSpiceNetlist(const Schematic& sch, const std::string& title);
std::string exportBomCsv(const Schematic& sch);
std::string exportPickAndPlaceCsv(const Schematic& sch);
std::string exportGerber(const Schematic& sch, const PcbLayout& pcb, GerberLayer layer);
/// Copper Gerber for any layer of the stack-up (0 = top … layerCount-1 = bottom).
std::string exportCopperGerber(const Schematic& sch, const PcbLayout& pcb, int copperLayer);
/// Excellon drill file: plated holes (pads, vias) or, with `plated` false, the non-plated mounting holes.
std::string exportExcellonDrill(const Schematic& sch, const PcbLayout& pcb, bool plated = true);
std::string exportStl(const Mesh& mesh, const std::string& name);
std::string exportObj(const Mesh& mesh, const std::string& name);

}  // namespace sieda
