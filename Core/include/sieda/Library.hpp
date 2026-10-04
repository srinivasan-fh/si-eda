// SiEDA Core — built-in component library: schematic pin maps, footprints and 3D bodies.
#pragma once

#include <string>
#include <utility>
#include <vector>

#include "sieda/Geometry.hpp"

namespace sieda {

/// Stable numeric identifiers — mirrored by `ComponentKind` in the Swift app (raw values must match).
enum class ComponentKind : int {
    Resistor = 0,
    Capacitor = 1,
    Inductor = 2,
    Diode = 3,
    LED = 4,
    VoltageSource = 5,
    CurrentSource = 6,
    Ground = 7,
    NPN = 8,
    NMOS = 9,
    OpAmp = 10,
    Switch = 11,
    Connector = 12,
    IC8 = 13,
    Fuse = 14,
    NetLabel = 15,
    Custom = 16,  // user-defined part (see CustomParts.hpp); Component::customPart holds its id
    Battery = 17,    // DC cell / pack: a voltage source with a battery symbol (value = volts)
    ACSource = 18,   // AC mains / transformer secondary: a voltage source, value SIN(offset peak freq)
    Junction = 19,   // wire node: a T-junction or bend point; joins the wires that end on it (no part, no footprint)
};
constexpr int kComponentKindCount = 20;

/// Kinds that are ideal voltage sources in the simulator (DC supply, battery, AC source).
inline bool isVoltageSourceKind(ComponentKind k) {
    return k == ComponentKind::VoltageSource || k == ComponentKind::Battery || k == ComponentKind::ACSource;
}
/// Schematic-only symbols that name or join nets rather than being parts (no reference designator in use).
inline bool isNetSymbolKind(ComponentKind k) {
    return k == ComponentKind::Ground || k == ComponentKind::NetLabel || k == ComponentKind::Junction;
}
/// Any independent source (voltage or current).
inline bool isSourceKind(ComponentKind k) { return isVoltageSourceKind(k) || k == ComponentKind::CurrentSource; }

struct PinDef {
    PinDef() = default;
    PinDef(std::string n, Vec2 o) : name(std::move(n)), offset(o) {}
    std::string name;
    Vec2 offset;         // schematic units (grid = 10), relative to the symbol origin, y down
    std::string number;  // package pin number (custom parts); empty for built-ins
    int type = 0;        // sieda::PinType (custom parts); 0 = passive
};

struct PadDef {
    int pinIndex = -1;  // -1 = mechanical / not connected
    Vec2 offset;        // millimetres, relative to footprint origin
    Vec2 size;          // millimetres
    bool throughHole = false;
    double drill = 0.0;
    bool round = false;
};

struct BodyDef {
    double width = 0, depth = 0, height = 0;  // millimetres
    bool cylinder = false;
    float r = 0.2f, g = 0.2f, b = 0.2f;
};

struct FootprintDef {
    std::string name;
    std::string label;  // human readable package name (defaults to name)
    std::vector<PadDef> pads;
    double courtyardW = 0, courtyardH = 0;
    BodyDef body;
};

struct ComponentDef {
    ComponentKind kind;
    std::string name;          // human readable, e.g. "Resistor"
    std::string refPrefix;     // e.g. "R"
    std::string defaultValue;  // e.g. "10k"
    std::string unit;          // e.g. "Ω"
    std::vector<PinDef> pins;
    std::string footprint;  // empty for virtual parts (ground, net labels)
    bool simulated = true;
};

class Library {
public:
    static const Library& instance();
    const ComponentDef& component(ComponentKind kind) const;
    const FootprintDef* footprint(const std::string& name) const;  // nullptr if unknown
    /// Footprints a part of this kind can be fitted in (chip sizes, through-hole, tantalum…); empty = fixed.
    static std::vector<std::string> packageVariants(ComponentKind kind);
    /// Short name of a package variant for menus and the BOM ("0603", "SMA (DO-214AC)").
    static std::string packageLabel(const std::string& footprint);
    const std::vector<ComponentDef>& components() const { return components_; }
    const std::vector<FootprintDef>& footprints() const { return footprints_; }
    static bool isValidKind(int kind) { return kind >= 0 && kind < kComponentKindCount; }

private:
    Library();
    std::vector<ComponentDef> components_;
    std::vector<FootprintDef> footprints_;
};

}  // namespace sieda
