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
/// A pin or gate swap a placed multi-unit part allows on the board (Project::pcbSwapOptions).
struct PcbSwapOption {
    std::string kind;       // "pin" (two pins of one swap group of a gate) or "gate" (two interchangeable gates)
    int component = -1;     // the placed unit (gate)
    int other = -1;         // gate swap: the other unit
    int pinA = -1, pinB = -1;  // pin swap: unit pin indices
    std::string label;      // "U1A: pins 2 ↔ 3", "U1A ↔ U2B"
    double gain = 0;        // ratsnest saved (mm, nearest-pad estimate; negative = longer)
};

/// What the automatic pin / gate swap before routing did (Project::autoSwapForRouting).
struct AutoSwapResult {
    int pinSwaps = 0, gateSwaps = 0;
    double ratsnestBefore = 0, ratsnestAfter = 0;  // mm, every net's pad-to-pad spanning tree (pour nets left out)
    int crossingsBefore = 0, crossingsAfter = 0;   // airwires of different nets that cross
    std::vector<std::string> report;               // one line per swap (applyPcbSwap)
};

/// Interactive placement (Update PCB's "place new parts", or any single footprint): a footprint at a candidate pose
/// (grid-snapped centre, rotation, side) and what is wrong there. Errors make the pose illegal: the courtyard overlaps
/// another part's on the same side (PLACE_OVERLAP), reaches past the board outline (PLACE_OUTSIDE) or into a
/// mounting-hole keep-out (PLACE_HOLE), or the part is locked (PLACE_LOCKED). A routing keep-out over the part's pads
/// (PLACE_KEEPOUT) is a warning: the pose is legal, but tracks cannot reach those pads there.
struct PlacementIssue {
    std::string code;     // PLACE_OVERLAP, PLACE_OUTSIDE, PLACE_HOLE, PLACE_LOCKED, PLACE_KEEPOUT
    std::string message;  // English, with the designators
    int other = -1;       // PLACE_OVERLAP: the part overlapped
    bool error = true;
};

struct PlacementCheck {
    int component = -1;  // -1: no such footprint
    Vec2 position;       // snapped
    int rotation = 0;    // 0, 90, 180, 270
    bool bottom = false;
    Rect courtyard;
    /// Copper pads at this pose (board space, as PcbLayout::pads gives them once placed; nets from the schematic).
    std::vector<Pad> pads;
    std::vector<PlacementIssue> issues;
    bool legal = false;      // no error among the issues
    bool committed = false;  // placeComponent: the part now stands there
};

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

    /// Clipboard of components (Schematic::copyComponents) with their BOM sourcing and their settings in every
    /// assembly variant.
    Json copyComponents(const std::vector<int>& ids, const std::vector<int>& busIds = {}) const;
    /// Pastes a clipboard (Schematic::pasteComponents): sourcing comes along, and variant settings go to the
    /// variants of the same name in this project. Returns the new components' ids.
    std::vector<int> pasteComponents(const Json& clip, const PasteOptions& options);

    /// Pin / gate swaps for the package of `componentId` (the footprint or any of its units), best first. Units on
    /// a repeated sheet's channel copies are left out (their block's are listed).
    std::vector<PcbSwapOption> pcbSwapOptions(int componentId) const;
    /// Carries a swap out in the schematic (back-annotation): the nets of the pads change places, routing that no
    /// longer fits is removed, and the Update PCB baseline follows when the board was in step. False when refused.
    bool applyPcbSwap(const PcbSwapOption& option, std::vector<std::string>* report = nullptr);
    /// Automatic pin / gate swap: makes the swap that saves most ratsnest, again and again (at most `maxSwaps`), for
    /// one package (`componentId`) or every placed package (-1). Returns the number made.
    int optimizePcbSwaps(int componentId, int maxSwaps = 100, std::vector<std::string>* report = nullptr);
    /// The autorouter's pin / gate swap (AutorouteOptions::pinSwap): over every placed, unlocked multi-unit package,
    /// makes the legal swap (pcbSwapOptions) that most shortens the pad-to-pad ratsnest without adding crossings, or
    /// removes crossings without lengthening it, again and again (at most `maxSwaps`). Each swap is carried out with
    /// applyPcbSwap, so it is back-annotated like a manual one. Nets with locked copper or a pour, and units of
    /// repeated sheets, are never swapped; a scoped or fan-out-only strategy swaps nothing. Deterministic.
    AutoSwapResult autoSwapForRouting(int maxSwaps = 500);
    /// The board's autoroute (PcbLayout::autoRoute) with the strategy's pin / gate swap first when it is on; the swaps
    /// are reported in the route report (RouteMetrics::swapRun, RouteReport::swaps). A cancelled route undoes them.
    RouteStats autoRoute(const RouteControl& control = RouteControl{});
    /// The schematic as the board would take it now.
    PcbSyncBaseline currentSync() const;
    /// The changes an update would make, by section; nothing is changed.
    std::vector<PcbEcoChange> pcbEcoPreview() const;
    /// Executes the changes with these keys (all when `keys` is empty): places added footprints, removes copper zones
    /// on nets that are gone, carries the schematic's net rules to the board and records the new baseline for what
    /// was executed. `report` gets one line per executed change. Returns the number executed.
    /// `placementQueue` (optional) gets the components the update put on the board, in designator order: the new
    /// footprints the designer may now place interactively (placeComponent). They already stand where Auto Place put
    /// them, so a part never placed by hand keeps that position. Parts already on the board are never listed.
    int applyPcbEco(const std::vector<std::string>& keys, std::vector<std::string>* report = nullptr,
                    std::vector<int>* placementQueue = nullptr);

    // ---- interactive placement (Core/src/InteractivePlacement.cpp) ----
    /// The footprint of `componentId` with its centre at `at` snapped to `grid` mm (≤ 0: no snap), turned to
    /// `rotation` (rounded to a quarter turn), on the bottom side when `bottom` (embedded parts stay inside). Nothing
    /// changes. Other parts' courtyards, the outline, mounting holes and keep-outs are checked (see PlacementCheck).
    PlacementCheck checkPlacement(int componentId, Vec2 at, int rotation, bool bottom, double grid) const;
    /// Moves, turns and flips the part to that pose when it is legal and returns the check with `committed` set. An
    /// illegal pose is not committed (the part stays where it was) unless `allowIllegal`: then it is committed and
    /// `legal` stays false, so the caller can report the violation (the DRC reports it as well).
    PlacementCheck placeComponent(int componentId, Vec2 at, int rotation, bool bottom, double grid,
                                  bool allowIllegal = false);
    /// Where to start placing the part: next to the parts it connects to (the centroid of their pads on its nets,
    /// each net weighted equally so a ground rail does not pull it to the middle), at the nearest grid spot where the
    /// pose is legal with a small gap to other courtyards. Its current rotation and side are kept. Without
    /// connections the search starts at the board centre; `legal` is false when the board has no free spot.
    PlacementCheck suggestPlacement(int componentId, double grid) const;

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
