// SiEDA Core — automotive Electronic Control Unit (ECU) architecture: ECU types (body, powertrain, ADAS / domain,
// EV, chassis, gateway) and the six design segments a production ECU is built from — the transient & power
// protection front-end, voltage regulation & management, the safety microcontroller, vehicle networks, power
// actuation and sensor conditioning — each checked on the actual schematic and layout (ISO 7637-2, ISO 16750-2,
// CISPR 25, ISO 10605, ISO 26262 practice).
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct EcuType {
    std::string id;  // "bcm", "powertrain", "adas", "ev", "chassis", "gateway"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<EcuType>& ecuTypes();
const EcuType* findEcuType(const std::string& id);

/// True when the ECU checks apply: an ECU type is set, or the industry is automotive.
bool isEcuProject(const Project& project);
/// The six ECU segments with their checklists (same shape as the robot segments).
std::vector<RobotSegment> ecuSegments(const Project& project);
Json ecuSegmentsJson(const Project& project);
/// Automotive ECU design rules. Empty for non-automotive projects.
std::vector<RuleViolation> automotiveChecks(const Project& project);

}  // namespace sieda
