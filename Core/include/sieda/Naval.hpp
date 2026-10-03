// SiEDA Core — naval / marine electronics architecture: platforms (surface combatant, aircraft carrier, submarine,
// patrol vessel, commercial marine) and the five design segments a shipboard board is built from — heavy-duty power
// filtration & galvanic isolation, corrosion-resistant hermetic compute, high-shock mechanical stabilisation, rugged
// marine data links and radar / sonar front ends — each checked on the schematic, the board build and the layout
// (MIL-STD-1399, MIL-STD-461, MIL-STD-810, MIL-STD-901E, IEC 60945).
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct NavalPlatform {
    std::string id;  // "combatant", "carrier", "submarine", "patrol", "commercial"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<NavalPlatform>& navalPlatforms();
const NavalPlatform* findNavalPlatform(const std::string& id);

/// True when the naval checks apply: a platform is set, or the industry is marine.
bool isNavalProject(const Project& project);
std::vector<RobotSegment> navalSegments(const Project& project);
Json navalSegmentsJson(const Project& project);
/// Naval design rules. Empty for other projects; Info only while no platform is chosen.
std::vector<RuleViolation> navalChecks(const Project& project);

}  // namespace sieda
