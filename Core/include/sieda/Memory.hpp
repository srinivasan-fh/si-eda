// SiEDA Core — memory (RAM) design architecture (JEDEC JESD79-3 / -4 / -5 DDR3 / DDR4 / DDR5, JESD209 LPDDR, JESD21-C
// SDR SDRAM, JESD21-C / JESD305 SPD, JESD301 DDR5 PMIC, IPC-2141 controlled impedance): memory design types (SDR
// SDRAM on an MCU memory controller, DDR3L / DDR4 memory-down, LPDDR point-to-point, DDR5 UDIMM / SO-DIMM and RDIMM
// modules) and the five segments a memory subsystem is built from — power & decoupling, clock / command / address,
// data integrity, configuration & management, and layout & fabrication — checked on the schematic and the layout.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {


struct MemoryDesignType {
    std::string id;  // "sdram", "ddr", "lpddr", "dimm", "rdimm"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<MemoryDesignType>& memoryDesignTypes();
const MemoryDesignType* findMemoryDesignType(const std::string& id);

/// True when the memory checks apply: a memory design type is set, or the industry is memory.
bool isMemoryProject(const Project& project);
std::vector<RobotSegment> memorySegments(const Project& project);
Json memorySegmentsJson(const Project& project);
/// Memory design rules. Empty for other projects; Info only while no memory design type is chosen.
std::vector<RuleViolation> memoryChecks(const Project& project);

/// {"laneSkewPs","dqsSkewPs","addrSkewPs","impedanceTolPercent","laneViaSpread","pairSkewPs"} of the overrides (only those set) and
/// back; values are clamped (skews 0 … 1000 ps, tolerance 0 … 50 %, via spread -1 … 8).
Json memoryLimitsJson(const Project::MemoryLayoutLimits& l);
Project::MemoryLayoutLimits memoryLimitsFromJson(const Json& j);
/// The limits in force for the project's memory type: {"type","effective":{…},"defaults":{…},"overrides":{…}}.
Json memoryLimitsReportJson(const Project& project);

}  // namespace sieda
