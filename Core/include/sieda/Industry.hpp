// SiEDA Core — industry design profiles (robotics, power electronics, automotive, RF, space, marine, industrial).
//
// A profile bundles the standards a domain designs to, the PCB design-rule preset, component derating factors used
// by circuit validation, the altitude class for voltage clearances, and design guidance for engineers and AI agents.
// Profiles apply common derating and spacing guidance; they are not a certification sign-off.
#pragma once

#include <string>
#include <vector>

#include "sieda/Validation.hpp"

namespace sieda {

struct IndustryProfile {
    std::string id;           // stable: "general", "robotics", "power", "automotive", "rf", "space", "marine", "industrial"
    std::string name;         // "Automotive"
    std::string description;  // one line
    std::string standards;    // "AEC-Q100/Q200, ISO 16750-2, ISO 7637-2, IPC-6012 Class 3/A"
    std::string rulePreset;   // design-rule preset applied with the profile
    double powerDerating = 1.0;    // fraction of rated power a part may dissipate
    double currentDerating = 1.0;  // fraction of rated current
    bool highAltitude = false;     // IPC-2221 B3 voltage clearances
    double minAmbientC = 0, maxAmbientC = 70;
    std::vector<std::string> guidance;  // design rules of thumb for the domain
};

const std::vector<IndustryProfile>& industryProfiles();
/// Profile by id (case-insensitive); nullptr if unknown.
const IndustryProfile* findIndustry(const std::string& id);
/// `base` ratings scaled by the profile's derating factors, with a note for validation messages.
PartRatings deratedRatings(const IndustryProfile& profile, const PartRatings& base = {});

}  // namespace sieda
