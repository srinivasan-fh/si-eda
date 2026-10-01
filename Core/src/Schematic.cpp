#include "sieda/Schematic.hpp"

#include <algorithm>
#include <numeric>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Units.hpp"

namespace sieda {

const char* severityName(Severity s) {
    switch (s) {
        case Severity::Info: return "info";
        case Severity::Warning: return "warning";
        case Severity::Error: return "error";
    }
    return "warning";
}

const ComponentDef& Component::def() const {
    if (kind == ComponentKind::Custom) {
        if (const CustomPart* part = CustomPartRegistry::instance().find(customPart)) return part->def;
    }
    return Library::instance().component(kind);
}

std::string Schematic::nextRef(const std::string& prefix) const {
    std::set<std::string> used;
    for (const auto& c : components_) used.insert(c.ref);
    for (int n = 1;; ++n) {
        std::string candidate = prefix + std::to_string(n);
        if (!used.count(candidate)) return candidate;
    }
}

std::string Schematic::nextRef(ComponentKind kind) const {
    const ComponentDef& def = Library::instance().component(kind);
    if (kind == ComponentKind::Ground || kind == ComponentKind::NetLabel) {
        int n = 1;
        for (const auto& c : components_)
            if (c.kind == kind) ++n;
        return "#" + def.refPrefix + std::to_string(n);
    }
    std::set<std::string> used;
    for (const auto& c : components_) used.insert(c.ref);
    for (int n = 1;; ++n) {
        std::string candidate = def.refPrefix + std::to_string(n);
        if (!used.count(candidate)) return candidate;
    }
}

int Schematic::addComponent(ComponentKind kind, const std::string& value, Vec2 position, int rotation,
                            const std::string& ref) {
    if (!Library::isValidKind(static_cast<int>(kind)) || kind == ComponentKind::Custom) return -1;
    Component c;
    c.id = nextComponentId_++;
    c.kind = kind;
    c.value = value.empty() ? c.def().defaultValue : value;
    c.ref = ref.empty() ? nextRef(kind) : ref;
    c.position = position;
    c.rotation = ((rotation % 360) + 360) % 360;
    components_.push_back(c);
    invalidate();
    return c.id;
}

int Schematic::addCustomComponent(const std::string& partId, const std::string& value, Vec2 position, int rotation,
                                  const std::string& ref) {
    const CustomPart* part = CustomPartRegistry::instance().find(partId);
    if (!part) return -1;
    Component c;
    c.id = nextComponentId_++;
    c.kind = ComponentKind::Custom;
    c.customPart = partId;
    c.value = value.empty() ? part->def.defaultValue : value;
    c.ref = ref.empty() ? nextRef(part->def.refPrefix) : ref;
    c.position = position;
    c.rotation = ((rotation % 360) + 360) % 360;
    components_.push_back(c);
    invalidate();
    return c.id;
}

int Schematic::replaceCustomPart(const std::string& oldId, const std::string& newId) {
    const CustomPart* oldPart = CustomPartRegistry::instance().find(oldId);
    const CustomPart* newPart = CustomPartRegistry::instance().find(newId);
    if (!oldPart || !newPart) return 0;
    auto mapPin = [&](int pin) -> int {
        if (pin < 0 || pin >= static_cast<int>(oldPart->def.pins.size())) return -1;
        const PinDef& old = oldPart->def.pins[static_cast<size_t>(pin)];
        for (size_t i = 0; i < newPart->def.pins.size(); ++i)
            if (newPart->def.pins[i].number == old.number) return static_cast<int>(i);
        for (size_t i = 0; i < newPart->def.pins.size(); ++i)
            if (newPart->def.pins[i].name == old.name) return static_cast<int>(i);
        return -1;
    };
    int count = 0;
    std::set<int> changed;
    for (auto& c : components_) {
        if (c.kind != ComponentKind::Custom || c.customPart != oldId) continue;
        c.customPart = newId;
        if (c.value == oldPart->def.defaultValue) c.value = newPart->def.defaultValue;
        changed.insert(c.id);
        ++count;
    }
    std::vector<Wire> kept;
    for (auto w : wires_) {
        bool ok = true;
        if (changed.count(w.a.component)) ok &= (w.a.pin = mapPin(w.a.pin)) >= 0;
        if (changed.count(w.b.component)) ok &= (w.b.pin = mapPin(w.b.pin)) >= 0;
        if (ok) kept.push_back(w);
    }
    wires_ = std::move(kept);
    invalidate();
    return count;
}

bool Schematic::removeComponent(int id) {
    auto it = std::find_if(components_.begin(), components_.end(), [&](const Component& c) { return c.id == id; });
    if (it == components_.end()) return false;
    components_.erase(it);
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [&](const Wire& w) { return w.a.component == id || w.b.component == id; }),
                 wires_.end());
    invalidate();
    return true;
}

bool Schematic::moveComponent(int id, Vec2 position) {
    Component* c = find(id);
    if (!c) return false;
    c->position = position;
    return true;  // geometry only; connectivity unchanged
}

bool Schematic::rotateComponent(int id, int deltaDeg) {
    Component* c = find(id);
    if (!c) return false;
    c->rotation = (((c->rotation + deltaDeg) % 360) + 360) % 360;
    return true;
}

bool Schematic::setValue(int id, const std::string& value) {
    Component* c = find(id);
    if (!c) return false;
    c->value = value;
    if (c->kind == ComponentKind::NetLabel || c->kind == ComponentKind::Ground) invalidate();
    return true;
}

bool Schematic::setRef(int id, const std::string& ref) {
    Component* c = find(id);
    if (!c || ref.empty()) return false;
    c->ref = ref;
    return true;
}

int Schematic::connect(PinRef a, PinRef b) {
    const Component* ca = find(a.component);
    const Component* cb = find(b.component);
    if (!ca || !cb || a == b) return -1;
    if (a.pin < 0 || a.pin >= static_cast<int>(ca->def().pins.size())) return -1;
    if (b.pin < 0 || b.pin >= static_cast<int>(cb->def().pins.size())) return -1;
    for (const auto& w : wires_)
        if ((w.a == a && w.b == b) || (w.a == b && w.b == a)) return -1;
    Wire w;
    w.id = nextWireId_++;
    w.a = a;
    w.b = b;
    wires_.push_back(w);
    invalidate();
    return w.id;
}

bool Schematic::removeWire(int id) {
    auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; });
    if (it == wires_.end()) return false;
    wires_.erase(it);
    invalidate();
    return true;
}

void Schematic::clear() {
    components_.clear();
    wires_.clear();
    nextComponentId_ = 1;
    nextWireId_ = 1;
    invalidate();
}

void Schematic::restoreComponent(const Component& c) {
    components_.push_back(c);
    nextComponentId_ = std::max(nextComponentId_, c.id + 1);
    invalidate();
}

void Schematic::restoreWire(const Wire& w) {
    wires_.push_back(w);
    nextWireId_ = std::max(nextWireId_, w.id + 1);
    invalidate();
}

const Component* Schematic::find(int id) const {
    for (const auto& c : components_)
        if (c.id == id) return &c;
    return nullptr;
}

Component* Schematic::find(int id) {
    for (auto& c : components_)
        if (c.id == id) return &c;
    return nullptr;
}

const Component* Schematic::findByRef(const std::string& ref) const {
    for (const auto& c : components_)
        if (c.ref == ref) return &c;
    return nullptr;
}

int Schematic::pinIndex(int componentId, const std::string& pinName) const {
    const Component* c = find(componentId);
    if (!c) return -1;
    const auto& pins = c->def().pins;
    if (c->kind == ComponentKind::Custom) {  // datasheet pin numbers take precedence ("U1.4", "U1.EP")
        for (size_t i = 0; i < pins.size(); ++i)
            if (pins[i].number == pinName) return static_cast<int>(i);
    }
    for (size_t i = 0; i < pins.size(); ++i)
        if (pins[i].name == pinName) return static_cast<int>(i);
    // Accept 1-based numeric aliases ("1", "2", ...) for any part.
    try {
        size_t used = 0;
        int n = std::stoi(pinName, &used);
        if (used == pinName.size() && n >= 1 && n <= static_cast<int>(pins.size())) return n - 1;
    } catch (...) {
    }
    return -1;
}

Vec2 Schematic::pinPosition(PinRef pin) const {
    const Component* c = find(pin.component);
    if (!c) return {};
    const auto& pins = c->def().pins;
    if (pin.pin < 0 || pin.pin >= static_cast<int>(pins.size())) return c->position;
    // Schematic y axis points down; rotation is clockwise degrees as seen on screen, which is exactly
    // rotate90() applied in y-down coordinates.
    return c->position + rotate90(pins[static_cast<size_t>(pin.pin)].offset, c->rotation);
}

// ---------------------------------------------------------------- connectivity

namespace {
struct UnionFind {
    std::vector<int> parent;
    explicit UnionFind(size_t n) : parent(n) { std::iota(parent.begin(), parent.end(), 0); }
    int find(int x) {
        while (parent[static_cast<size_t>(x)] != x) {
            parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
            x = parent[static_cast<size_t>(x)];
        }
        return x;
    }
    void unite(int a, int b) {
        a = find(a);
        b = find(b);
        if (a != b) parent[static_cast<size_t>(std::max(a, b))] = std::min(a, b);
    }
};

bool isGroundName(const std::string& n) { return n == "GND" || n == "0" || n == "gnd" || n == "Gnd"; }
}  // namespace

void Schematic::rebuildNets() const {
    nets_.clear();
    pinToNet_.clear();

    std::vector<PinRef> pins;
    std::map<PinRef, int> index;
    for (const auto& c : components_) {
        for (size_t p = 0; p < c.def().pins.size(); ++p) {
            PinRef r{c.id, static_cast<int>(p)};
            index[r] = static_cast<int>(pins.size());
            pins.push_back(r);
        }
    }
    UnionFind uf(pins.size());
    for (const auto& w : wires_) {
        auto ia = index.find(w.a), ib = index.find(w.b);
        if (ia != index.end() && ib != index.end()) uf.unite(ia->second, ib->second);
    }
    int groundRoot = -1;
    std::map<std::string, int> labelRoot;
    for (const auto& c : components_) {
        bool ground = c.kind == ComponentKind::Ground || (c.kind == ComponentKind::NetLabel && isGroundName(c.value));
        if (ground) {
            int i = index[{c.id, 0}];
            if (groundRoot < 0) groundRoot = i;
            else uf.unite(groundRoot, i);
        } else if (c.kind == ComponentKind::NetLabel) {
            int i = index[{c.id, 0}];
            auto it = labelRoot.find(c.value);
            if (it == labelRoot.end()) labelRoot[c.value] = i;
            else uf.unite(it->second, i);
        }
    }

    std::map<int, int> rootToNet;
    for (size_t i = 0; i < pins.size(); ++i) {
        int root = uf.find(static_cast<int>(i));
        auto it = rootToNet.find(root);
        int net;
        if (it == rootToNet.end()) {
            net = static_cast<int>(nets_.size());
            rootToNet[root] = net;
            Net n;
            n.index = net;
            nets_.push_back(n);
        } else {
            net = it->second;
        }
        nets_[static_cast<size_t>(net)].pins.push_back(pins[i]);
        pinToNet_[pins[i]] = net;
    }

    int autoCounter = 1;
    for (auto& n : nets_) {
        std::string label;
        for (const auto& p : n.pins) {
            const Component* c = find(p.component);
            if (c->kind == ComponentKind::Ground || (c->kind == ComponentKind::NetLabel && isGroundName(c->value))) {
                n.isGround = true;
            } else if (c->kind == ComponentKind::NetLabel && (label.empty() || c->value < label)) {
                label = c->value;
            }
        }
        if (n.isGround) n.name = "GND";
        else if (!label.empty()) n.name = label;
        else n.name = "N$" + std::to_string(autoCounter++);
    }
    netsDirty_ = false;
}

const std::vector<Net>& Schematic::nets() const {
    if (netsDirty_) rebuildNets();
    return nets_;
}

int Schematic::netOf(PinRef pin) const {
    if (netsDirty_) rebuildNets();
    auto it = pinToNet_.find(pin);
    return it == pinToNet_.end() ? -1 : it->second;
}

int Schematic::groundNet() const {
    for (const auto& n : nets())
        if (n.isGround) return n.index;
    return -1;
}

// ---------------------------------------------------------------- ERC

std::vector<RuleViolation> Schematic::runERC() const {
    std::vector<RuleViolation> out;
    const auto& allNets = nets();
    auto add = [&](Severity s, const std::string& code, const std::string& msg, std::vector<int> comps, Vec2 loc) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        v.components = std::move(comps);
        v.location = loc;
        v.hasLocation = true;
        out.push_back(std::move(v));
    };

    if (components_.empty()) {
        RuleViolation v;
        v.severity = Severity::Info;
        v.code = "ERC_EMPTY";
        v.message = "Schematic is empty.";
        out.push_back(v);
        return out;
    }

    // Duplicate reference designators.
    std::map<std::string, std::vector<int>> refs;
    for (const auto& c : components_)
        if (c.kind != ComponentKind::Ground && c.kind != ComponentKind::NetLabel) refs[c.ref].push_back(c.id);
    for (const auto& [ref, ids] : refs)
        if (ids.size() > 1)
            add(Severity::Error, "ERC_DUPLICATE_REF", "Reference designator " + ref + " is used " +
                std::to_string(ids.size()) + " times.", ids, find(ids[0])->position);

    bool hasSources = false;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::VoltageSource || c.kind == ComponentKind::CurrentSource) hasSources = true;
    int gnd = groundNet();
    if (hasSources && gnd < 0)
        add(Severity::Error, "ERC_NO_GROUND", "Circuit has sources but no ground reference. Add a Ground symbol.", {},
            components_.front().position);

    for (const auto& c : components_) {
        const auto& pins = c.def().pins;
        int unconnected = 0;
        std::vector<std::string> openPins;
        for (size_t p = 0; p < pins.size(); ++p) {
            int net = netOf({c.id, static_cast<int>(p)});
            if (net < 0 || allNets[static_cast<size_t>(net)].pins.size() < 2) {
                ++unconnected;
                openPins.push_back(pins[p].name);
            }
        }
        if (c.kind == ComponentKind::NetLabel || c.kind == ComponentKind::Ground) {
            if (unconnected)
                add(Severity::Warning, "ERC_DANGLING_LABEL", c.ref + " (" + c.value + ") is not connected to anything.",
                    {c.id}, c.position);
            continue;
        }
        if (unconnected == static_cast<int>(pins.size()) && pins.size() > 1) {
            add(Severity::Warning, "ERC_FLOATING_COMPONENT", c.ref + " is not connected to the circuit.", {c.id},
                c.position);
        } else {
            for (const auto& name : openPins) {
                if (c.kind == ComponentKind::Custom) {
                    PinType type = PinType::Passive;
                    for (const auto& pd : pins)
                        if (pd.name == name) type = static_cast<PinType>(pd.type);
                    if (type == PinType::NoConnect) continue;
                    if (type == PinType::PowerIn) {
                        add(Severity::Error, "ERC_POWER_PIN_UNCONNECTED",
                            "Power pin " + c.ref + "." + name + " is not connected.", {c.id}, c.position);
                        continue;
                    }
                }
                Severity s = c.kind == ComponentKind::IC8 ? Severity::Info : Severity::Warning;
                add(s, "ERC_UNCONNECTED_PIN", "Pin " + c.ref + "." + name + " is unconnected.", {c.id}, c.position);
            }
        }

        // Value validation.
        switch (c.kind) {
            case ComponentKind::Resistor:
            case ComponentKind::Capacitor:
            case ComponentKind::Inductor:
            case ComponentKind::Fuse: {
                auto v = parseEngineeringValue(primaryValue(c.value));
                if (!v || *v <= 0)
                    add(Severity::Error, "ERC_INVALID_VALUE", c.ref + " has invalid value '" + c.value + "'.", {c.id},
                        c.position);
                break;
            }
            case ComponentKind::VoltageSource:
            case ComponentKind::CurrentSource:
                if (!SourceSpec::parse(c.value))
                    add(Severity::Error, "ERC_INVALID_VALUE",
                        c.ref + " has invalid source value '" + c.value + "' (use e.g. 5, SIN(0 1 1k), PULSE(0 5 1m)).",
                        {c.id}, c.position);
                break;
            case ComponentKind::Connector:
            case ComponentKind::IC8:
            case ComponentKind::Custom:
                if (c.kind == ComponentKind::Custom && !CustomPartRegistry::instance().find(c.customPart)) {
                    add(Severity::Error, "ERC_MISSING_PART", c.ref + " references a component that is not in the library.",
                        {c.id}, c.position);
                    break;
                }
                add(Severity::Info, "ERC_NOT_SIMULATED", c.ref + " (" + c.def().name + ") has no simulation model.",
                    {c.id}, c.position);
                break;
            default: break;
        }

        if (c.kind == ComponentKind::VoltageSource) {
            int a = netOf({c.id, 0}), b = netOf({c.id, 1});
            if (a >= 0 && a == b)
                add(Severity::Error, "ERC_SHORTED_SOURCE", c.ref + " is short-circuited (both terminals on net " +
                    allNets[static_cast<size_t>(a)].name + ").", {c.id}, c.position);
        }

        if (c.kind == ComponentKind::LED || c.kind == ComponentKind::Diode) {
            auto netHasSource = [&](int net) {
                if (net < 0) return false;
                const Net& n = allNets[static_cast<size_t>(net)];
                if (n.isGround) return true;
                return std::any_of(n.pins.begin(), n.pins.end(), [&](const PinRef& p) {
                    return find(p.component)->kind == ComponentKind::VoltageSource;
                });
            };
            int a = netOf({c.id, 0}), k = netOf({c.id, 1});
            bool aSrc = a >= 0 && !allNets[static_cast<size_t>(a)].isGround && netHasSource(a);
            if (aSrc && netHasSource(k) && allNets[static_cast<size_t>(a)].pins.size() == 2)
                add(Severity::Warning, "ERC_NO_CURRENT_LIMIT",
                    c.ref + " is driven directly by a voltage source without a current-limiting resistor.", {c.id},
                    c.position);
        }
    }
    // Pin-type rules for custom parts (datasheet-derived electrical types).
    for (const auto& net : allNets) {
        std::vector<std::string> drivers;
        std::vector<int> ids;
        for (const auto& pr : net.pins) {
            const Component* c = find(pr.component);
            if (c->kind != ComponentKind::Custom) continue;
            const auto& pins = c->def().pins;
            if (pr.pin < 0 || pr.pin >= static_cast<int>(pins.size())) continue;
            auto type = static_cast<PinType>(pins[static_cast<size_t>(pr.pin)].type);
            if (type == PinType::Output || type == PinType::PowerOut) {
                drivers.push_back(c->ref + "." + pins[static_cast<size_t>(pr.pin)].name);
                ids.push_back(c->id);
            }
            if (type == PinType::NoConnect && net.pins.size() > 1)
                add(Severity::Warning, "ERC_NC_CONNECTED",
                    "Pin " + c->ref + "." + pins[static_cast<size_t>(pr.pin)].name + " is marked no-connect but is wired to net " +
                        net.name + ".", {c->id}, c->position);
        }
        if (drivers.size() > 1) {
            std::string list;
            for (size_t i = 0; i < drivers.size(); ++i) list += (i ? ", " : "") + drivers[i];
            add(Severity::Warning, "ERC_OUTPUT_CONFLICT", "Net " + net.name + " is driven by several outputs: " + list + ".",
                ids, find(ids[0])->position);
        }
    }
    return out;
}

}  // namespace sieda
