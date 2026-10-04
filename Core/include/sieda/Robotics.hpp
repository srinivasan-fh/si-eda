// SiEDA Core — robotic system architecture: platforms (Mars rover, FPV drone, industrial arm, quadruped, humanoid,
// 3D printer, CNC machine) with their production parts kits
// and the seven modular design segments every production robot needs — power distribution, compute, motion control,
// sensors, communication, safety / UI and mechanical — each checked on the actual schematic and layout.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct RobotPlatform {
    std::string id;    // "rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"
    std::string name;  // "Mars / planetary rover"
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<RobotPlatform>& robotPlatforms();
const RobotPlatform* findRobotPlatform(const std::string& id);

/// The production parts kit of a platform: for each subsystem, the library part numbers (all in standardParts())
/// a production board of that robot is built from — compute, motion, sensing, field bus, power and safety — with
/// the spares that drop into the same footprint or role.
struct RobotKitGroup {
    std::string subsystem;  // "Motion — stepper drivers"
    std::vector<std::string> parts;
};
const std::vector<RobotKitGroup>& robotPartKit(const std::string& platform);  // empty for an unknown platform

struct RobotCheckItem {
    std::string label;
    bool ok = false;
    std::string detail;  // what was found, or what to add
};

struct RobotSegment {
    std::string id;    // "power", "compute", "motion", "sensors", "comms", "safety", "mechanical"
    std::string name;  // "Power Distribution Network"
    std::string status;  // "complete", "partial", "missing"
    std::vector<RobotCheckItem> items;
    std::vector<std::string> guidance;
};

/// True when the robotics checks apply: a robot platform is set, or the industry is robotics / UAV.
bool isRobotProject(const Project& project);
/// The seven segments with their checklist on the current design.
std::vector<RobotSegment> robotSegments(const Project& project);
Json robotSegmentsJson(const Project& project);

/// Robotics design rules (power protection, thermal vias, IMU placement, star ground, field-bus termination, RF
/// noise, coax placement, hardwired E-stop, MLCC strain relief, rigid-flex joints). Empty for non-robot projects.
std::vector<RuleViolation> roboticsChecks(const Project& project);

/// Stitches thermal vias under / next to the largest pad of a power part (its own net, keeping clearance), so heat
/// flows into the inner planes and the chassis. Returns the vias added.
int addThermalVias(Project& project, int componentId, int maxVias = 12);
/// Robot projects: thermal vias at every SMD power FET (Auto Route runs this after routing). Returns the vias added.
int autoThermalVias(Project& project);

}  // namespace sieda
