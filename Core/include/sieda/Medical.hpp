// SiEDA Core — medical electronics architecture (IEC 60601-1 / -1-2, ISO 14971, ISO 13485): device classes (type BF,
// type CF, life-support / therapy, active implant, home healthcare) and the four design segments a medical board is
// built from — patient isolation & defibrillator protection, biosignal acquisition, safety compute & power, and
// coexistence & clinical wireless — checked on the schematic, the galvanic isolation domains and the layout.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

class Project;

struct MedicalClass {
    std::string id;  // "bf", "cf", "life", "implant", "home"
    std::string name;
    std::string description;
    std::vector<std::string> guidance;
};
const std::vector<MedicalClass>& medicalClasses();
const MedicalClass* findMedicalClass(const std::string& id);

/// True when the medical checks apply: a device class is set, or the industry is medical.
bool isMedicalProject(const Project& project);
std::vector<RobotSegment> medicalSegments(const Project& project);
Json medicalSegmentsJson(const Project& project);
/// Medical design rules. Empty for other projects; Info only while no class is chosen.
std::vector<RuleViolation> medicalChecks(const Project& project);

}  // namespace sieda
