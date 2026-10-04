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
#include <utility>
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
    std::string type = "SOIC";  // SOIC, TSSOP, DIP, QFN, LQFP, SOT23, HEADER, TO220, LGA, … (supportedPackages())
    int pinCount = 0;           // 0 = derived from the pin list
    /// Lead pitch and body size in millimetres (0 = the package type's default). The body size is the square body of
    /// a QFN/QFP (7 for a 7 × 7 mm LQFP-48), the moulded body width of a SOIC/TSSOP, or the row spacing of a DIP.
    double pitch = 0;
    double bodySize = 0;
    /// "LGA" / "CUSTOM": the exact land pattern, one pad per pin number 1…N in order (centre x, y and size w, h in mm,
    /// y down, pad 1 usually top-left) — land-grid and other irregular packages taken pad-for-pad from the
    /// manufacturer's recommended footprint ("LGA"), or a footprint drawn in the footprint editor ("CUSTOM").
    /// bodySize is the body width (x) and bodyDepth its length (y; 0 = square). A land with a drill is a plated
    /// through-hole pad; `round` makes it a circle / oval.
    /// `pin` names the pin number the pad belongs to when that is not its own number (a tab or a second pad on the
    /// same pin, an exposed pad "EP", BGA ball names); "-" marks a mechanical pad with no pin.
    struct Land {
        double x = 0, y = 0, w = 0, h = 0;
        double drill = 0;
        bool round = false;
        std::string pin;
        Land() = default;
        Land(double x_, double y_, double w_, double h_, double drill_ = 0, bool round_ = false, std::string pin_ = {})
            : x(x_), y(y_), w(w_), h(h_), drill(drill_), round(round_), pin(std::move(pin_)) {}
    };
    std::vector<Land> lands;
    double bodyDepth = 0;
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
    // Isolated DC-DC module: the input draws P_out / efficiency between `in` and `inReturn` (the primary side) and
    // the output is referenced to `ref` on the galvanically separate secondary. Empty = non-isolated regulator.
    std::string inReturn;
    double efficiency = 0.8;
    bool isolated() const { return !inReturn.empty(); }
    /// A converter with its own input return that is a different pin from the output reference: a galvanic
    /// barrier. inReturn == ref models a non-isolated switching converter (buck) by its efficiency.
    bool galvanic() const { return isolated() && inReturn != ref; }
    // Current-limited load switch: the output follows the input (minus `dropout`), so it is never "in dropout".
    bool loadSwitch = false;
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

/// Where one pin sits on the schematic symbol: a side of the body ('L' left, 'R' right, 'T' top, 'B' bottom) and a
/// slot along it (0 = topmost on L / R, leftmost on T / B; slots are 2 grid units apart and may leave gaps between
/// groups). Pins of one part on the same side and slot are stacked: drawn as one pin and joined electrically, the
/// way repeated VDD / GND pins are shown on professional symbols.
struct SymbolPin {
    std::string number;  // the pin's number in the part's pin list
    char side = 'L';
    int slot = 0;
};

/// A part's schematic symbol layout (the Symbol Editor's document). Empty = the generated layout: pins in number
/// order down the left side, then up the right.
struct SymbolSpec {
    std::vector<SymbolPin> pins;
    double width = 0;  // body width in schematic units (grid = 10); 0 = from the pin names
    bool empty() const { return pins.empty(); }
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
    SymbolSpec symbol;

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

/// True for package types drawn from an explicit land pattern ("LGA", "CUSTOM").
bool usesLandPattern(const std::string& packageType);

/// The footprint editor's starting point: the part's generated footprint as an editable land pattern (type
/// "CUSTOM", one land per pad, body kept). Pins numbered "EP" / "TAB" take their pad's number. Parts already on a
/// land pattern are returned unchanged. Throws JsonError for an invalid part or a BGA (ball names, not numbers).
CustomPartSpec landPatternFromFootprint(const CustomPartSpec& spec);

/// Manufacturability of a land pattern: pads that overlap, copper gaps below `minGap` (mm), annular rings below
/// 0.1 mm, pads with no pin and pins with no pad. `pads` are 1-based pad numbers.
struct LandIssue {
    std::string severity;  // "error", "warning"
    std::string code;      // "LAND_OVERLAP", "LAND_GAP", "LAND_ANNULAR", "LAND_NO_PIN", "LAND_NO_PAD"
    std::string message;
    std::vector<int> pads;
};
std::vector<LandIssue> checkLandPattern(const CustomPartSpec& spec, double minGap = 0.1);

/// Symbol Editor's starting point: a readable layout from the pins' names and electrical types. Supplies go on top
/// and grounds at the bottom, inputs on the left and outputs on the right. MCU port pins (PA0…, P1.3, GPIO5) are
/// grouped by port and kept in bit order; reset, clock, boot and debug pins sit together at the top left; no-connect
/// pins go last on the right. Groups are separated by an empty slot. With `stackDuplicates`, repeated supply and
/// ground pins of the same name share one slot (stacked, joined).
SymbolSpec autoArrangeSymbol(const CustomPartSpec& spec, bool stackDuplicates = true);

/// Problems with a symbol layout: pins missing from it or placed twice, unknown pins, pins of different names on
/// one spot (errors); stacked pins (info, they are joined) and stacked signal pins (warning). `pins` are pin numbers.
struct SymbolIssue {
    std::string severity;  // "error", "warning", "info"
    std::string code;      // "SYM_MISSING", "SYM_UNKNOWN", "SYM_DUPLICATE", "SYM_OVERLAP", "SYM_STACK", "SYM_STACK_SIGNAL",
                           // "SYM_INVALID"
    std::string message;
    std::vector<std::string> pins;
};
std::vector<SymbolIssue> checkSymbol(const CustomPartSpec& spec);

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
