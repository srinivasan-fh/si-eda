// SiEDA Core — schematic data model: components, wires, nets and electrical rule check.
#pragma once

#include <map>
#include <string>
#include <vector>

#include "sieda/Geometry.hpp"
#include "sieda/Library.hpp"

namespace sieda {

struct PinRef {
    int component = -1;
    int pin = -1;
    bool operator==(const PinRef& o) const { return component == o.component && pin == o.pin; }
    bool operator<(const PinRef& o) const { return component < o.component || (component == o.component && pin < o.pin); }
};

struct PcbPlacement {
    Vec2 position;      // mm, board coordinates (y down)
    int rotation = 0;   // degrees, multiple of 90
    bool bottom = false;
    bool placed = false;
};

struct Component {
    int id = -1;
    ComponentKind kind = ComponentKind::Resistor;
    std::string ref;
    std::string value;
    Vec2 position;  // schematic units
    int rotation = 0;
    PcbPlacement pcb;

    const ComponentDef& def() const { return Library::instance().component(kind); }
    bool hasFootprint() const { return !def().footprint.empty(); }
};

struct Wire {
    int id = -1;
    PinRef a, b;
};

struct Net {
    int index = -1;
    std::string name;
    std::vector<PinRef> pins;
    bool isGround = false;
};

enum class Severity { Info = 0, Warning = 1, Error = 2 };

struct RuleViolation {
    Severity severity = Severity::Warning;
    std::string code;     // stable machine code, e.g. "ERC_UNCONNECTED_PIN"
    std::string message;  // human readable
    std::vector<int> components;
    Vec2 location;  // schematic units (ERC) or millimetres (DRC)
    bool hasLocation = false;
};

class Schematic {
public:
    int addComponent(ComponentKind kind, const std::string& value, Vec2 position, int rotation = 0,
                     const std::string& ref = "");
    bool removeComponent(int id);
    bool moveComponent(int id, Vec2 position);
    bool rotateComponent(int id, int deltaDeg = 90);
    bool setValue(int id, const std::string& value);
    bool setRef(int id, const std::string& ref);

    int connect(PinRef a, PinRef b);  // returns wire id, or -1 if invalid / duplicate
    bool removeWire(int id);
    void clear();

    const std::vector<Component>& components() const { return components_; }
    std::vector<Component>& mutableComponents() { invalidate(); return components_; }
    const std::vector<Wire>& wires() const { return wires_; }
    const Component* find(int id) const;
    Component* find(int id);
    const Component* findByRef(const std::string& ref) const;
    int pinIndex(int componentId, const std::string& pinName) const;

    Vec2 pinPosition(PinRef pin) const;  // schematic units, world space

    /// Connectivity (cached). Nets are merged across wires and identically named net labels.
    const std::vector<Net>& nets() const;
    int netOf(PinRef pin) const;  // -1 if pin has no net (unconnected)
    int groundNet() const;        // -1 if no ground net

    std::vector<RuleViolation> runERC() const;

    std::string nextRef(ComponentKind kind) const;

    // Persistence helpers used by Project
    void restoreComponent(const Component& c);
    void restoreWire(const Wire& w);

private:
    void invalidate() { netsDirty_ = true; }
    void rebuildNets() const;

    std::vector<Component> components_;
    std::vector<Wire> wires_;
    int nextComponentId_ = 1;
    int nextWireId_ = 1;

    mutable bool netsDirty_ = true;
    mutable std::vector<Net> nets_;
    mutable std::map<PinRef, int> pinToNet_;
};

const char* severityName(Severity s);

}  // namespace sieda
