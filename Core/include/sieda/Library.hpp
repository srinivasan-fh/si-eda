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
};
constexpr int kComponentKindCount = 17;

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
    const std::vector<ComponentDef>& components() const { return components_; }
    const std::vector<FootprintDef>& footprints() const { return footprints_; }
    static bool isValidKind(int kind) { return kind >= 0 && kind < kComponentKindCount; }

private:
    Library();
    std::vector<ComponentDef> components_;
    std::vector<FootprintDef> footprints_;
};

}  // namespace sieda
