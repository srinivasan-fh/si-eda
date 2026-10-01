// SiEDA Core — design verification: one pass/fail sign-off over every check the engine can run.
//
// Stages, in order: ERC → DC simulation → circuit validation → footprint placement → routing completion →
// DRC and fabrication rules → manufacturing outputs. Each stage reports Pass / Warning / Fail; the overall
// verdict is the worst stage. The project is never modified.
#pragma once

#include <optional>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"
#include "sieda/Validation.hpp"

namespace sieda {

enum class StageStatus { Pass = 0, Warning = 1, Fail = 2, Skipped = 3 };
const char* stageStatusName(StageStatus s);

struct VerificationStage {
    std::string id;     // stable: "erc", "simulation", "validation", "placement", "routing", "drc", "manufacturing"
    std::string title;  // "Electrical Rule Check", …
    StageStatus status = StageStatus::Pass;
    std::string summary;               // one line
    std::vector<std::string> details;  // facts measured by the stage (e.g. generated file sizes)
    std::vector<RuleViolation> findings;
};

struct VerificationReport {
    std::string projectName;
    std::string industry;           // profile id
    std::string industryName;       // "Automotive"
    std::string industryStandards;  // standards the profile follows
    std::string rulePreset;
    int layerCount = 2;
    StageStatus verdict = StageStatus::Pass;  // never Skipped
    int errors = 0, warnings = 0, infos = 0;
    std::vector<VerificationStage> stages;

    bool passed() const { return verdict != StageStatus::Fail; }
    Json toJson() const;
    std::string toMarkdown() const;
};

struct VerificationOptions {
    /// Part ratings for circuit validation; defaults to the project's industry-derated ratings.
    std::optional<PartRatings> ratings;
    bool includeManufacturing = true;
};

VerificationReport verifyDesign(const Project& project, const VerificationOptions& options = {});

}  // namespace sieda
