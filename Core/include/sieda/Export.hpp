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
std::string exportExcellonDrill(const Schematic& sch, const PcbLayout& pcb);
std::string exportStl(const Mesh& mesh, const std::string& name);
std::string exportObj(const Mesh& mesh, const std::string& name);

}  // namespace sieda
