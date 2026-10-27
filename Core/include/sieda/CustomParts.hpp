// SiEDA Core — user-defined components (e.g. extracted from a datasheet).
//
// A custom part is described by a CustomPartSpec (name, pins with electrical types, package). From it the
// core generates a schematic symbol (DIP-style box), a parametric IPC-like footprint and a 3D body, and
// registers the result so components of kind `ComponentKind::Custom` behave like built-in parts.
#pragma once

#include <array>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "sieda/Geometry.hpp"
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
        /// Pad options (JSON: a trailing object, written only when one is set): corner radius as a fraction of the
        /// short side (rounded rectangle, 0…0.5), a non-plated hole (no copper), the solder mask margin (mm, < 0 =
        /// the board's 0.05) and the paste size as a fraction of the pad (< 0 = automatic, 0 = no paste) split into
        /// pasteGrid × pasteGrid windows (thermal pads).
        double corner = 0;
        bool npth = false;
        double mask = -1, paste = -1;
        int pasteGrid = 1;
        /// More shapes: corners cut at 45° (`chamfer`, a fraction of the short side, 0…0.5) or any outline (`outline`,
        /// mm round the pad centre, y down: trapezoids, custom pads); `inner` = a through-hole pad's diameter on the
        /// inner copper layers (pad stack; 0 = the pad's own size); `solder` = its diameter on the solder side (the
        /// layer opposite the part, copper and mask; 0 = the pad's own size).
        double chamfer = 0;
        std::vector<Vec2> outline{};
        double inner = 0, solder = 0;
        bool hasOptions() const {
            return corner > 0 || npth || mask >= 0 || paste >= 0 || pasteGrid > 1 || chamfer > 0 || !outline.empty() || inner > 0 ||
                   solder > 0;
        }
        Land() = default;
        Land(double x_, double y_, double w_, double h_, double drill_ = 0, bool round_ = false, std::string pin_ = {})
            : x(x_), y(y_), w(w_), h(h_), drill(drill_), round(round_), pin(std::move(pin_)) {}
    };
    std::vector<Land> lands;
    double bodyDepth = 0;
    /// Land patterns: courtyard margin around the pads and body (mm, 0 = 0.25). JSON "courtyard", written when set.
    double courtyard = 0;
    /// Drawn silkscreen / fabrication lines (JSON "graphics": [[x0, y0, x1, y1, width, "silk" | "fab"]]), written
    /// when there are any.
    std::vector<FootprintLine> graphics;
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

/// A free-form drawing on a part's symbol (Altium's symbol graphics), in symbol coordinates: schematic units (grid =
/// 10) from the symbol's centre, y down, before the component's rotation.
///   line    — a polyline through `points` (two or more)
///   rect    — `points[0]` and `points[1]` are opposite corners
///   circle  — centre `points[0]`, `radius`
///   arc     — centre `points[0]`, `radius`, from `startAngle` to `endAngle` (degrees, clockwise on screen from +x)
///   polygon — closed through `points` (three or more)
///   text    — `text` with its left end at `points[0]`, `size` high
struct SymbolGraphic {
    std::string kind = "line";
    std::vector<Vec2> points;
    double radius = 0;
    double startAngle = 0, endAngle = 360;
    std::string text;
    double size = 8;
    double lineWidth = 1;  // multiples of the normal symbol line
    bool fill = false;     // filled with the symbol fill colour (rect, circle, polygon)
    bool operator==(const SymbolGraphic& o) const {
        return kind == o.kind && points == o.points && radius == o.radius && startAngle == o.startAngle &&
               endAngle == o.endAngle && text == o.text && size == o.size && lineWidth == o.lineWidth && fill == o.fill;
    }
};

/// The symbol coordinates' bounding box of drawings (text estimated at 0.6 × size per character). Empty → {0,0,0,0}.
std::array<double, 4> symbolGraphicsBounds(const std::vector<SymbolGraphic>& graphics);

/// A part's schematic symbol layout (the Symbol Editor's document). Empty = the generated layout: pins in number
/// order down the left side, then up the right. Graphics are drawn over the body; with `body` false they replace it
/// (pin leads then start where the generated body's edge would be).
struct SymbolSpec {
    std::vector<SymbolPin> pins;
    double width = 0;  // body width in schematic units (grid = 10); 0 = from the pin names
    std::vector<SymbolGraphic> graphics;
    bool body = true;
    bool empty() const { return pins.empty(); }
};

/// One unit (gate) of a multi-unit part, drawn as its own symbol: a quad op-amp has units A–D and a power unit. A pin
/// listed in several units is shared (one pin, drawn on each); pins in no unit form an extra power unit "P".
struct UnitSpec {
    UnitSpec() = default;
    UnitSpec(std::string name_, std::vector<std::string> pins_) : name(std::move(name_)), pins(std::move(pins_)) {}
    std::string name;              // "A", "B", "P"
    std::vector<std::string> pins;  // pin numbers
    /// Gate swapping: 0 = automatic (units with identical symbols are interchangeable), > 0 = interchangeable with the
    /// units of the same group only, -1 = never swapped.
    int swapGroup = 0;
    /// Pin swapping inside the unit: groups of interchangeable pins (the inputs of a NAND gate), by pin number.
    std::vector<std::vector<std::string>> pinSwap;
};

/// A part's imported 3D model (Model3D.hpp): the registered mesh and how it sits on the footprint. File
/// coordinates are scaled by unit × scale, rotated about x, y, z (degrees) and moved by offset (mm) into the
/// footprint's model frame (x right, y towards the top of the PCB view, z up from the board).
struct Model3DRef {
    std::string id;    // Model3DRegistry id; empty = the generated body
    std::string name;  // file name, shown in the UI
    double unit = 1;   // millimetres per file unit (KiCad VRML: 2.54)
    std::array<double, 3> scale{{1, 1, 1}};
    std::array<double, 3> rotate{{0, 0, 0}};
    std::array<double, 3> offset{{0, 0, 0}};
    bool empty() const { return id.empty(); }
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
    Model3DRef model3d;  // imported 3D model (empty = generated body)
    /// Multi-unit part (empty = one symbol for the whole part). The part can still be placed whole.
    std::vector<UnitSpec> units;

    /// Index of the pin with this number (preferred) or name; -1 if none.
    int pinIndex(const std::string& numberOrName) const;
};

/// The symbol of one unit of a multi-unit part.
struct PartUnitDef {
    std::string name;
    ComponentDef def;            // the unit's pins (kind PartUnit, no footprint)
    std::vector<int> pins;       // unit pin index → the part's pin index
    double halfWidth = 40, halfHeight = 40;
    bool power = false;          // only supply pins (the power unit)
    int swapGroup = 0;           // UnitSpec::swapGroup (0 for the generated power unit)
    std::vector<std::vector<int>> pinSwap;  // interchangeable pins, as unit pin indices
};


struct CustomPart {
    std::string id;  // "<NAME>-<hash>"; stable for identical specs
    CustomPartSpec spec;
    ComponentDef def;
    FootprintDef footprint;
    double symbolHalfWidth = 40, symbolHalfHeight = 40;  // schematic body half extents (grid units)
    /// Units of a multi-unit part, in order (the spec's units, then "P" for pins in no unit); empty otherwise.
    std::vector<PartUnitDef> units;
};

/// True for package types drawn from an explicit land pattern ("LGA", "CUSTOM").
bool usesLandPattern(const std::string& packageType);

/// The footprint editor's starting point: the part's generated footprint as an editable land pattern (type
/// "CUSTOM", one land per pad, body kept). Pins numbered "EP" / "TAB" take their pad's number. Parts already on a
/// land pattern are returned unchanged. Throws JsonError for an invalid part or a BGA (ball names, not numbers).
CustomPartSpec landPatternFromFootprint(const CustomPartSpec& spec);
/// A part's 3D model reference as JSON {"id","name","unit","scale","rotate","offset"} and back (empty for no id;
/// throws JsonError on bad values).
Json model3dRefToJson(const Model3DRef& ref);
Model3DRef model3dRefFromJson(const Json& j);

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
/// Unit (gate) editor checks of a multi-unit spec: unknown or repeated pins, empty or duplicate units (errors);
/// signal pins shared by several units, swap groups whose gates differ, pin-swap groups across pin types
/// (warnings); the power unit the part will get (info). Codes "UNIT_*"; `pins` are pin numbers.
std::vector<SymbolIssue> checkUnits(const CustomPartSpec& spec);
/// Library quality score of a part, 0 – 100: clean symbol (25) and land pattern (25 for no errors, 10 more for no
/// warnings), a datasheet (10), manufacturer and description (10), pin types beyond passive (10), a 3D model (10).
/// `missing` names what took points off.
/// What a footprint measures, to hold against the datasheet's recommended land pattern: pads with a pin, the smallest
/// pad-to-pad pitch, the first pad's width (short side) and length (long side), and the span (outer pad edge to outer
/// pad edge across x). mm.
struct FootprintDims {
    int pads = 0;
    double pitch = 0, padWidth = 0, padLength = 0, span = 0;
};
FootprintDims measureFootprint(const CustomPartSpec& spec);

struct PartQuality {
    int score = 0;
    std::vector<std::string> missing;
};
PartQuality partQuality(const CustomPartSpec& spec);

/// IPC-7351B land for a two-sided part from its lead dimensions (mm): toe-to-toe span L, heel-to-heel span S and lead
/// width W (min / max each). `family` "gullwing" (SOIC, SOP, QFP), "chip" (rectangular chips, 0603 and up) or "qfn"
/// (flat no-lead) picks the fillet goals; `density` 0 most, 1 nominal, 2 least. Fabrication / placement tolerances
/// F = 0.05, P = 0.025. Throws std::invalid_argument for impossible dimensions.
struct IpcLand {
    double length = 0, width = 0, centres = 0;  // pad length (along the lead) × width, pad-centre distance
    double z = 0, g = 0, x = 0;                  // outer span, inner gap, pad width as IPC names them
};
IpcLand ipc7351Land(const std::string& family, int density, double lMin, double lMax, double sMin, double sMax,
                    double wMin, double wMax);
/// True when two units of a part may be swapped (gate swap): see UnitSpec::swapGroup.
bool unitsInterchangeable(const PartUnitDef& a, const PartUnitDef& b);

Json customPartSpecToJson(const CustomPartSpec& spec);
CustomPartSpec customPartSpecFromJson(const Json& j);  // throws JsonError on invalid input
/// Full description including the generated symbol and footprint geometry (for previews and the UI).
Json customPartToJson(const CustomPart& part);
std::vector<std::string> supportedPackages();
/// Pins from a pasted or exported pin table: CSV, TSV or semicolon-separated, quoted cells allowed. With a header the
/// columns are found by name (number / pin / position / ball, name / pin name / signal, type / direction, description /
/// function); STM32CubeMX pin lists (Position, Name, Type, Signal) work as they are. Without one the columns are
/// number, name, type, description. At most 4096 rows; rows without a number (or with a space in it) are skipped.
std::vector<CustomPin> parsePinTable(const std::string& text);
/// Package type for a package name ("SOIC-8", "QFN-24", "TO-220", "PinHeader 1x04"…) and the pin count it names
/// (0 if none); "" when the name is not a package SiEDA can generate.
std::string packageTypeFromName(const std::string& name, int& pinCount);

class CustomPartRegistry {
public:
    static CustomPartRegistry& instance();
    /// Validates the spec, generates symbol/footprint and registers it (idempotent for identical specs). `keep`
    /// false only builds it (an editor's preview of each draft: nothing is kept for the process lifetime).
    std::shared_ptr<const CustomPart> registerPart(const CustomPartSpec& spec, bool keep = true);  // throws JsonError
    std::shared_ptr<const CustomPart> get(const std::string& id) const;
    const CustomPart* find(const std::string& id) const;  // pointer stays valid for the process lifetime
    /// The 3D model ids the registered parts refer to (`CustomPartSpec::model3d`): Model3DRegistry keeps them.
    std::set<std::string> model3dIds() const;

private:
    CustomPartRegistry() = default;
    mutable std::mutex mutex_;
    std::map<std::string, std::shared_ptr<const CustomPart>> parts_;
};

}  // namespace sieda
