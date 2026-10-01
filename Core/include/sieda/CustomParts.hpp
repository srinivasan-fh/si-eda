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

/// Behavioural simulation model of a custom part (optional). Pins are referenced by number or name.
struct RegulatorModel {
    std::string in, out, ref;  // input, output and reference pins (GND for fixed regulators, ADJ for LM317-style)
    double vout = 0;           // output relative to `ref` (V)
    double dropout = 0.3;      // minimum in − out headroom (V)
    double iq = 0;             // quiescent / ground-pin current (A), returned through `ref`
    double ilimit = 1.0;       // output current limit (A) — a charger's charge current
    double maxPower = 0.5;     // package dissipation limit (W) for validation
    bool charger = false;      // constant-current operation is normal (Li-ion CC/CV charger)
};

struct SupplyLoad {
    std::string supply, ret;  // supply pin and return (ground) pin
    double current = 0;       // typical operating current (A)
};

struct BehaviorModel {
    bool hasRegulator = false;
    RegulatorModel regulator;
    std::vector<SupplyLoad> loads;
    bool empty() const { return !hasRegulator && loads.empty(); }
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
    BehaviorModel model;

    /// Index of the pin with this number (preferred) or name; -1 if none.
    int pinIndex(const std::string& numberOrName) const;
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
