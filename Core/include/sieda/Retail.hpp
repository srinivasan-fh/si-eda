// SiEDA Core — retail / point-of-sale architecture (PCI PTS POI, EMVCo Level 1, IEC 62368-1, IEC 61000-4-2): device
// classes (countertop payment terminal, unattended terminal, mobile POS, self-service kiosk, receipt printer) and the
// four design segments a POS board is built from — payment security & anti-tamper, printer mechanism drivers, HMI &
// peripheral expansion, and ESD & environmental robustness — checked on the schematic and the layout.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct RetailDevice {
    std::string id;  // "countertop", "unattended", "mpos", "kiosk", "printer"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<RetailDevice>& retailDevices();
const RetailDevice* findRetailDevice(const std::string& id);

/// True when the retail checks apply: a device class is set, or the industry is retail.
bool isRetailProject(const Project& project);
std::vector<RobotSegment> retailSegments(const Project& project);
Json retailSegmentsJson(const Project& project);
/// Retail / POS design rules. Empty for other projects; Info only while no device class is chosen.
std::vector<RuleViolation> retailChecks(const Project& project);

}  // namespace sieda
