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
    /// Locked by the designer (connectors, fixed mechanical parts, matched-length bus parts): Auto Place keeps it.
    bool locked = false;
    /// Embedded passive: the inner copper layer it is formed on (1 … layers-2); 0 = an ordinary surface part.
    int embeddedLayer = 0;
    bool embedded() const { return embeddedLayer > 0; }
};

/// How a part is bought and fitted (BOM): who makes it, the orderable part number, the distributor / assembly-house
/// part number, the unit price, and "do not populate" (on the board but not fitted).
struct Sourcing {
    std::string manufacturer;
    std::string mpn;           // manufacturer part number
    std::string supplierPart;  // e.g. LCSC "C17414", Digi-Key, Mouser
    double unitPrice = 0;      // per part, in the project currency (0 = not priced)
    bool dnp = false;
    bool empty() const { return manufacturer.empty() && mpn.empty() && supplierPart.empty() && unitPrice == 0 && !dnp; }
};

struct Component {
    int id = -1;
    ComponentKind kind = ComponentKind::Resistor;
    std::string ref;
    std::string value;
    Vec2 position;  // schematic units
    int rotation = 0;
    PcbPlacement pcb;
    std::string customPart;  // CustomPartRegistry id when kind == ComponentKind::Custom
    std::vector<int> noConnect;  // pin indices deliberately left open (no-connect flag): ERC does not flag them
    // Microcontroller firmware (Intel HEX) run by the simulator; empty for other parts.
    std::string firmware;
    std::string firmwareName;  // file or example name shown in the editor
    double clockHz = 0;        // CPU clock; 0 = the model's default (16 MHz ATmega328P, 8 MHz ATtiny85)
    Sourcing sourcing;         // BOM: manufacturer, part numbers, price, do-not-populate
    /// Package variant this part is fitted in (Library::packageVariants: "R_0603", "CP_Tant_B", "D_DO41_THT"…);
    /// empty = the kind's default footprint.
    std::string package;

    bool isNoConnect(int pin) const;

    const ComponentDef& def() const;
    bool hasFootprint() const { return !def().footprint.empty(); }
    /// The footprint the part is placed with: its package variant, else the kind's default.
    const std::string& footprintName() const;
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

/// What a net carries, for colouring copper the way designers expect (power red, ground blue) and for net classes.
enum class NetRole { Signal = 0, Power, Ground, NegativeSupply };
const char* netRoleName(NetRole r);  // "signal", "power", "ground", "negative"

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
    /// Adds an instance of a registered custom part. Returns -1 if the part id is unknown.
    int addCustomComponent(const std::string& partId, const std::string& value, Vec2 position, int rotation = 0,
                           const std::string& ref = "");
    /// Switches every instance of `oldId` to `newId`, re-mapping wires by pin number then name. Returns count.
    int replaceCustomPart(const std::string& oldId, const std::string& newId);
    /// Removes a component and its wires. Removing a junction that joins exactly two wires keeps them connected.
    bool removeComponent(int id);
    bool moveComponent(int id, Vec2 position);
    bool rotateComponent(int id, int deltaDeg = 90);
    bool setValue(int id, const std::string& value);
    /// Fits the part in another package variant ("" = default); false if the kind has no such variant.
    bool setPackage(int id, const std::string& package);
    bool setRef(int id, const std::string& ref);
    /// Marks a pin as intentionally unconnected (KiCad/Altium "no-connect" flag) or clears the mark.
    bool setPinNoConnect(int componentId, int pin, bool noConnect);
    /// Attaches firmware to a microcontroller (empty `hex` removes it). Returns false for an unknown id.
    bool setFirmware(int id, const std::string& hex, const std::string& name, double clockHz);

    int connect(PinRef a, PinRef b);  // returns wire id, or -1 if invalid / duplicate
    /// Removes a wire; a junction left without wires is removed with it.
    bool removeWire(int id);
    /// Splits a wire at `position` with a new junction (T-junction or bend point) and returns the junction's id:
    /// the two halves keep the connection and further wires can end on the junction. -1 for an unknown wire.
    int splitWire(int wireId, Vec2 position);
    /// Removes a chain of dangling junctions starting at `junctionId` (a wire abandoned half-way). Returns count.
    int removeDanglingJunctions(int junctionId);
    /// Number of wire ends on the component's pins.
    int wireCount(int componentId) const;
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
    /// True when the pin's net reaches anything besides the pin itself and the pins stacked with it on its symbol
    /// (a stack of repeated GND pins with no wire is still open).
    bool isPinConnected(PinRef pin) const;
    int groundNet() const;        // -1 if no ground net
    /// Ground nets; supply rails (a source's + terminal, a regulator's power output, an IC's power input, or a name
    /// such as VCC, VDD, VBAT, +5V, 3V3); negative rails (a source's − terminal above a grounded +, or -12V, VEE);
    /// everything else is a signal.
    NetRole netRole(int net) const;

    std::vector<RuleViolation> runERC() const;

    std::string nextRef(ComponentKind kind) const;
    std::string nextRef(const std::string& prefix) const;

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
