// SiEDA Core — home appliance architecture (IEC / UL 60335-1, IEC 60730-1 Class B, IEC 60664-1, IEC 61000-4-4 / -4-5,
// CISPR 14-1): appliance types (laundry, kitchen & cooking, refrigeration, HVAC, small appliances) and the four design
// segments a mains-powered appliance controller is built from — AC mains entry & power conversion, high-voltage
// actuation & motor control, HMI & sensing, and IoT connectivity — checked on the schematic and the layout.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct ApplianceType {
    std::string id;  // "laundry", "kitchen", "refrigeration", "hvac", "small"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<ApplianceType>& applianceTypes();
const ApplianceType* findApplianceType(const std::string& id);

/// True when the appliance checks apply: an appliance type is set, or the industry is appliance.
bool isApplianceProject(const Project& project);
std::vector<RobotSegment> applianceSegments(const Project& project);
Json applianceSegmentsJson(const Project& project);
/// Home appliance design rules. Empty for other projects; Info only while no appliance type is chosen.
std::vector<RuleViolation> applianceChecks(const Project& project);

}  // namespace sieda
