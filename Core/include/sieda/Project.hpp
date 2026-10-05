// SiEDA Core — a complete design: schematic + PCB layout, with persistence and UI snapshots.
#pragma once

#include <array>
#include <map>
#include <string>

#include "sieda/CustomParts.hpp"
#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Variants.hpp"

namespace sieda {

/// Title block printed on every schematic sheet (the sheet name and "n of N" are added per sheet).
struct TitleBlock {
    std::string title, company, revision, date, drawnBy;
    bool empty() const { return title.empty() && company.empty() && revision.empty() && date.empty() && drawnBy.empty(); }
};

/// A change the board proposes to the schematic (back-annotation, Altium's ECO): a designator rename, a pin swap
/// (two pins of a part exchange their connections) or a gate swap (two units exchange gates).
struct EcoChange {
    std::string kind;          // "rename", "pinSwap", "gateSwap"
    int component = -1;        // the part (rename, pin swap; a multi-unit part's package) or the first unit
    std::string from, to;      // rename: designators; pin / gate swap: what changes places
    int pinA = -1, pinB = -1;  // pin swap: pins of the part
    int other = -1;            // gate swap: the second unit
    bool applicable = true;
    std::string note;          // why it cannot be applied
};

/// A change the schematic makes to the board (Altium's Update PCB ECO): parts added, removed or changed since the
/// last update, nets whose pins changed, copper zones on nets that are gone, and net rules from the schematic.
struct PcbEcoChange {
    std::string section;  // "component", "net", "zone", "rule"
    std::string action;   // "add", "remove", "change"
    std::string object;   // "R5", "VCC", "HS"
    std::string detail;   // what the update does
    std::string key;      // stable identity, for executing a chosen subset
    bool applicable = true;
    std::string note;     // why it cannot be executed
};

/// What the board was last updated from: each part (designator, footprint, value) and each net's pins.
struct PcbSyncBaseline {
    std::map<int, std::array<std::string, 3>> parts;  // component id → ref, footprint, value
    std::map<std::string, std::string> nets;          // net name → "R1.1 R2.2 …"
    bool operator==(const PcbSyncBaseline& o) const { return parts == o.parts && nets == o.nets; }
};

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
    /// Signal / power integrity: imported IBIS models, model assignments, PDN rail inputs and SI sign-off.
    SiSettings si;
    /// Ids of custom parts (CustomPartRegistry) available in this project's component library.
    std::vector<std::string> customLibrary;
    /// Assembly variants and the one the BOM, CPL and assembly exports follow ("" = the base design).
    std::vector<DesignVariant> variants;
    std::string activeVariant;
    /// Schematic title block (empty = the title is the project name).
    TitleBlock titleBlock;

    const DesignVariant* findVariant(const std::string& name) const;
    /// Adds a variant (name trimmed, unique, non-empty), optionally a copy of `copyFrom`'s settings.
    bool addVariant(const std::string& name, const std::string& copyFrom = "");
    bool renameVariant(const std::string& name, const std::string& newName);
    bool removeVariant(const std::string& name);
    bool setVariantDescription(const std::string& name, const std::string& description);
    /// Sets one component's fitting in a variant: fitted -1 (as the base design), 0 (not fitted) or 1 (fitted);
    /// `value` overrides the value when non-null ("" = the base value). False for an unknown variant or a part
    /// that is not a real component (net symbols).
    bool setVariantPart(const std::string& name, int componentId, int fitted, const std::string* value);
    /// "" selects the base design. False for an unknown name.
    bool setActiveVariant(const std::string& name);
    /// The schematic as `name` is assembled (the base design for "" or an unknown name).
    Schematic variantSchematic(const std::string& name) const;
    /// The circuit the simulator runs: the active variant as assembled — its value overrides applied and parts not
    /// fitted (in the variant or marked DNP) left out. Connectivity and net indices are the design's.
    Schematic simulationSchematic() const;
    /// Designators of the parts simulationSchematic() leaves out.
    std::vector<std::string> unfittedRefs() const;
    /// Re-numbers designators (see Schematic::annotate) and keeps tamper meshes on their parts.
    std::vector<RefChange> annotate(const AnnotateOptions& options);

    /// Registers `spec` and adds it to the project library; returns the part id.
    std::string addCustomPart(const CustomPartSpec& spec);  // throws JsonError
    bool removeCustomPart(const std::string& id);         // false if still used by a component

    /// Selects an industry profile: applies its design-rule preset and altitude class. False if the id is unknown.
    bool applyIndustry(const std::string& id);
    /// Part ratings derated for the project's industry profile.
    PartRatings partRatings() const;

    /// Call after any schematic edit that can change connectivity; keeps PCB copper consistent.
    void schematicChanged();
    /// The schematic is the source of the board's net rules: the widths and clearances its directives give nets
    /// replace those of the previous application in the board settings (nets the schematic never set keep theirs).
    /// Returns true when the board rules changed. Called by schematicChanged().
    bool applySchematicRules();

    // ---- forward annotation (schematic → board, "Update PCB") ----
    /// The board's baseline: what the last update put on it (a project read from a file without one is in sync).
    PcbSyncBaseline pcbSync;
    /// The schematic as the board would take it now.
    PcbSyncBaseline currentSync() const;
    /// The changes an update would make, by section; nothing is changed.
    std::vector<PcbEcoChange> pcbEcoPreview() const;
    /// Executes the changes with these keys (all when `keys` is empty): places added footprints, removes copper zones
    /// on nets that are gone, carries the schematic's net rules to the board and records the new baseline for what
    /// was executed. `report` gets one line per executed change. Returns the number executed.
    int applyPcbEco(const std::vector<std::string>& keys, std::vector<std::string>* report = nullptr);

    // ---- back-annotation (board → schematic ECO) ----
    /// Designators re-numbered from the board: per prefix, the placed parts in board order (rows top to bottom then
    /// left to right; or columns). Parts of repeated sheets keep theirs (their designators are generated). Only
    /// changes are listed; nothing is applied.
    std::vector<EcoChange> reannotateFromBoard(bool byColumns = false) const;
    /// Changes from a WAS / IS text: "OLD NEW" renames, "PINSWAP REF PIN PIN" and "GATESWAP U1A U2B" lines ('#'
    /// comments). Changes that cannot be applied are listed with a note.
    std::vector<EcoChange> ecoFromWasIs(const std::string& text) const;
    /// Applies the applicable changes (renames through temporary designators, so R1 ↔ R2 swaps work). Returns the
    /// number applied.
    int applyEco(const std::vector<EcoChange>& changes);

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
