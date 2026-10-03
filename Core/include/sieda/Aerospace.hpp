// SiEDA Core — aerospace electronics architecture: mission types (LEO satellite / CubeSat, GEO & deep space, launch
// vehicle, military aircraft, commercial avionics) and the five design segments an aerospace board is built from —
// rad-hardened & redundant compute, ruggedised power conditioning & isolation, flight sensor interfaces, avionics
// communications (MIL-STD-1553, SpaceWire, ARINC 429) and RF telemetry — each checked on the actual schematic and
// layout (IPC-6012DS / Class 3, MIL-STD-704, DO-160, ECSS-E-ST-50-12C).
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct AerospaceMission {
    std::string id;  // "leo", "geo", "launcher", "military", "commercial"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<AerospaceMission>& aerospaceMissions();
const AerospaceMission* findAerospaceMission(const std::string& id);

/// True when the aerospace checks apply: a mission is set, or the industry is space.
bool isAerospaceProject(const Project& project);
/// The five aerospace segments with their checklists (same shape as the robot segments).
std::vector<RobotSegment> aerospaceSegments(const Project& project);
Json aerospaceSegmentsJson(const Project& project);
/// Aerospace design rules. Empty for other projects; Info only while no mission is chosen.
std::vector<RuleViolation> aerospaceChecks(const Project& project);

}  // namespace sieda
