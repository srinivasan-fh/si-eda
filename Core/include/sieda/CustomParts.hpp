// SiEDA Core — user-defined components (e.g. extracted from a datasheet).
//
// A custom part is described by a CustomPartSpec (name, pins with electrical types, package). From it the
// core generates a schematic symbol (DIP-style box), a parametric IPC-like footprint and a 3D body, and
// registers the result so components of kind `ComponentKind::Custom` behave like built-in parts.
#pragma once

#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Library.hpp"

namespace sieda {

enum class PinType { Passive = 0, Input, Output, Bidirectional, PowerIn, PowerOut, OpenCollector, NoConnect };

const char* pinTypeName(PinType t);
PinType pinTypeFromName(const std::string& s);  // lenient ("pwr", "vcc", "in", "i/o", "nc"…)

struct CustomPin {
    std::string number;  // as printed in the datasheet: "1", "14", "EP"
    std::string name;    // "VCC", "TRIG", "PB0"
    PinType type = PinType::Passive;
    std::string description;
};

struct PackageSpec {
    std::string type = "SOIC";  // SOIC, TSSOP, DIP, QFN, LQFP, SOT23, HEADER, TO220
    int pinCount = 0;           // 0 = derived from the pin list
};

struct CustomPartSpec {
    std::string name;
    std::string manufacturer;
    std::string description;
    std::string refPrefix = "U";
    std::string defaultValue;
    std::string datasheet;  // file name or URL the part was extracted from
    PackageSpec package;
    std::vector<CustomPin> pins;
};

struct CustomPart {
    std::string id;  // "<NAME>-<hash>"; stable for identical specs
    CustomPartSpec spec;
    ComponentDef def;
    FootprintDef footprint;
    double symbolHalfWidth = 40, symbolHalfHeight = 40;  // schematic body half extents (grid units)
};

Json customPartSpecToJson(const CustomPartSpec& spec);
CustomPartSpec customPartSpecFromJson(const Json& j);  // throws JsonError on invalid input
/// Full description including the generated symbol and footprint geometry (for previews and the UI).
Json customPartToJson(const CustomPart& part);
std::vector<std::string> supportedPackages();

class CustomPartRegistry {
public:
    static CustomPartRegistry& instance();
    /// Validates the spec, generates symbol/footprint and registers it (idempotent for identical specs).
    std::shared_ptr<const CustomPart> registerPart(const CustomPartSpec& spec);  // throws JsonError
    std::shared_ptr<const CustomPart> get(const std::string& id) const;
    const CustomPart* find(const std::string& id) const;  // pointer stays valid for the process lifetime

private:
    CustomPartRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const CustomPart>> parts_;
};

}  // namespace sieda
