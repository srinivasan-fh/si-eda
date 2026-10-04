// SiEDA Core — a complete design: schematic + PCB layout, with persistence and UI snapshots.
#pragma once

#include <string>

#include "sieda/CustomParts.hpp"
#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Validation.hpp"

namespace sieda {

class Project {
public:
    std::string name = "Untitled";
    /// Boards per order (BOM cost totals).
    int buildQuantity = 5;
    std::string requirements;  // the prompt / PRD text the design was generated from
    std::string industry = "general";  // IndustryProfile id (derating, design rules, altitude class)
    /// Robot platform ("rover", "fpv", "arm", "quadruped", "humanoid", "printer3d", "cnc"; empty = not a robot): turns on the robotics
    /// segment checks and their platform guidance.
    std::string robotPlatform;
    /// Automotive ECU type ("bcm", "powertrain", "adas", "ev", "chassis", "gateway"; empty = none): turns on the
    /// six ECU segment checks with their guidance (the automotive industry profile turns them on too).
    std::string ecuType;
    /// Aerospace mission ("leo", "geo", "launcher", "military", "commercial"; empty = none): turns on the five
    /// aerospace segment checks with their guidance (the space industry profile turns them on too).
    std::string aerospaceMission;
    /// Naval platform ("combatant", "carrier", "submarine", "patrol", "commercial"; empty = none): turns on the five
    /// naval segment checks (the marine industry profile turns them on as advice).
    std::string navalPlatform;
    /// Medical device class ("bf", "cf", "life", "implant", "home"; empty = none): turns on the four medical segment
    /// checks (the medical industry profile turns them on as advice).
    std::string medicalClass;
    /// Retail device class ("countertop", "unattended", "mpos", "kiosk", "printer"; empty = none): turns on the four
    /// retail / POS segment checks (the retail industry profile turns them on as advice).
    std::string retailDevice;
    /// Home appliance type ("laundry", "kitchen", "refrigeration", "hvac", "small"; empty = none): turns on the
    /// four appliance segment checks (the appliance industry profile turns them on as advice).
    std::string applianceType;
    /// Memory design type ("sdram", "ddr", "lpddr", "dimm", "rdimm"; empty = none): turns on the five memory
    /// segment checks (the memory industry profile turns them on as advice).
    std::string memoryDesign;
    Schematic schematic;
    PcbLayout pcb;
    /// Ids of custom parts (CustomPartRegistry) available in this project's component library.
    std::vector<std::string> customLibrary;

    /// Registers `spec` and adds it to the project library; returns the part id.
    std::string addCustomPart(const CustomPartSpec& spec);  // throws JsonError
    bool removeCustomPart(const std::string& id);         // false if still used by a component

    /// Selects an industry profile: applies its design-rule preset and altitude class. False if the id is unknown.
    bool applyIndustry(const std::string& id);
    /// Part ratings derated for the project's industry profile.
    PartRatings partRatings() const;

    /// Call after any schematic edit that can change connectivity; keeps PCB copper consistent.
    void schematicChanged();

    Json toJson() const;
    static Project fromJson(const Json& j);  // throws JsonError on malformed input

    /// Read-only view model consumed by the UI (pins in world space, pads, ratsnest, …).
    Json snapshot() const;

    static Json violationsToJson(const std::vector<RuleViolation>& v);
    Json dcToJson(const DcResult& r) const;
    Json transientToJson(const TransientResult& r, size_t maxPoints = 2000) const;
    static Json libraryJson();
};

}  // namespace sieda
