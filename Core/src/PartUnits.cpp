// SiEDA Core — multi-unit parts: one symbol per gate (a quad op-amp's units A–D and its power unit), one package.
//
// Each placed unit is a component of kind PartUnit; its package is a Custom component that carries every pin and the
// footprint but is not drawn on the schematic (`packageOnly`). A unit's pins are joined to its package's pins when
// nets are built, and only the package's pins are net members, so the netlist, simulation, BOM and PCB see one
// ordinary part exactly as if it had been placed whole.
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "sieda/CustomParts.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
const CustomPart* partOf(const Component& c) { return CustomPartRegistry::instance().find(c.customPart); }

}  // namespace

int Schematic::unitPackage(int componentId) const {
    const Component* c = find(componentId);
    if (!c) return -1;
    if (c->kind == ComponentKind::PartUnit) {
        const Component* p = find(c->unitOf);
        return p && p->packageOnly ? p->id : -1;
    }
    return c->packageOnly ? c->id : -1;
}

std::vector<int> Schematic::placedUnits(int packageId) const {
    std::vector<std::pair<int, int>> units;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::PartUnit && c.unitOf == packageId) units.push_back({c.unit, c.id});
    std::sort(units.begin(), units.end());
    std::vector<int> out;
    for (const auto& [unit, id] : units) out.push_back(id);
    return out;
}

std::string Schematic::unitName(const Component& c) const {
    if (c.kind != ComponentKind::PartUnit) return {};
    const CustomPart* part = partOf(c);
    if (!part || c.unit < 1 || c.unit > static_cast<int>(part->units.size())) return {};
    return part->units[static_cast<size_t>(c.unit - 1)].name;
}

std::string Schematic::displayRef(const Component& c) const { return c.ref + unitName(c); }

int Schematic::addCustomUnits(const std::string& partId, const std::string& value, Vec2 position, int rotation,
                              const std::string& ref) {
    const CustomPart* part = CustomPartRegistry::instance().find(partId);
    if (!part || part->units.empty()) return -1;
    const int sheet = definitionSheet(activeSheet_);
    const int shown = activeSheet_;
    activeSheet_ = sheet;  // on a repeated sheet's instance: placed in the block
    Component pkg;
    pkg.id = nextComponentId_++;
    pkg.kind = ComponentKind::Custom;
    pkg.customPart = partId;
    pkg.value = value.empty() ? part->def.defaultValue : value;
    pkg.ref = ref.empty() ? nextRef(part->def.refPrefix) : ref;
    pkg.position = position;
    pkg.sheet = sheet;
    pkg.packageOnly = true;
    components_.push_back(pkg);
    const int id = addPartUnit(pkg.id, 1, position, rotation);
    activeSheet_ = shown;
    if (id < 0) return -1;
    return shown == sheet ? id : copyOn(id, shown);
}

int Schematic::addPartUnit(int componentId, int unit, Vec2 position, int rotation) {
    const int pkgId = unitPackage(masterOf(componentId));
    const Component* pkg = find(pkgId);
    if (!pkg) return -1;
    const CustomPart* part = partOf(*pkg);
    if (!part || unit < 1 || unit > static_cast<int>(part->units.size())) return -1;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::PartUnit && c.unitOf == pkgId && c.unit == unit) return -1;
    Component u;
    u.id = nextComponentId_++;
    u.kind = ComponentKind::PartUnit;
    u.customPart = pkg->customPart;
    u.unitOf = pkgId;
    u.unit = unit;
    u.ref = pkg->ref;
    u.value = pkg->value;
    u.position = position;
    u.rotation = ((rotation % 360) + 360) % 360;
    // A unit goes on the sheet shown (on a repeated sheet's channel: into the block); a unit cannot be apart from its
    // package across a repeated sheet's boundary, so then it goes on the package's sheet.
    u.sheet = definitionSheet(activeSheet_);
    if (u.sheet != pkg->sheet && (isRepeated(u.sheet) || isRepeated(pkg->sheet))) u.sheet = pkg->sheet;
    components_.push_back(u);
    const int shown = activeSheet_;
    invalidate();
    edited();
    if (u.sheet != shown)
        if (const int copy = copyOn(u.id, shown); copy > 0) return copy;
    return find(u.id) ? u.id : -1;
}

int Schematic::placeNextUnit(int componentId, Vec2 position) {
    const int pkgId = unitPackage(masterOf(componentId));
    const Component* pkg = find(pkgId);
    if (!pkg) return -1;
    const CustomPart* part = partOf(*pkg);
    if (!part) return -1;
    std::set<int> placed;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::PartUnit && c.unitOf == pkgId) placed.insert(c.unit);
    for (int u = 1; u <= static_cast<int>(part->units.size()); ++u)
        if (!placed.count(u)) return addPartUnit(pkgId, u, position, 0);
    return -1;
}

bool Schematic::syncUnits() {
    bool any = false;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::PartUnit || c.packageOnly) {
            any = true;
            break;
        }
    if (!any) return false;
    std::unordered_map<int, size_t> at;
    for (size_t i = 0; i < components_.size(); ++i) at[components_[i].id] = i;
    auto comp = [&](int id) -> Component* {
        auto it = at.find(id);
        return it == at.end() ? nullptr : &components_[it->second];
    };
    std::set<int> gone;
    std::map<int, std::vector<int>> unitsOf;  // package → its units, in component order
    std::set<std::pair<int, int>> seen;       // (package, unit)
    for (auto& c : components_) {
        if (c.packageOnly && c.kind != ComponentKind::Custom) c.packageOnly = false;
        if (c.kind != ComponentKind::PartUnit) continue;
        const Component* pkg = comp(c.unitOf);
        const CustomPart* part = pkg ? partOf(*pkg) : nullptr;
        // A unit may sit on another sheet than its package, but not across a repeated sheet's boundary.
        const bool apart = pkg && pkg->sheet != c.sheet && (isRepeated(pkg->sheet) || isRepeated(c.sheet));
        if (!pkg || !pkg->packageOnly || pkg->kind != ComponentKind::Custom || !part || c.unit < 1 || apart ||
            c.unit > static_cast<int>(part->units.size()) || !seen.insert({c.unitOf, c.unit}).second) {
            gone.insert(c.id);
            continue;
        }
        unitsOf[c.unitOf].push_back(c.id);
    }
    for (const auto& c : components_)
        if (c.packageOnly && !unitsOf.count(c.id)) gone.insert(c.id);
    bool changed = false;
    if (!gone.empty()) {
        components_.erase(std::remove_if(components_.begin(), components_.end(), [&](const Component& c) { return gone.count(c.id) > 0; }),
                          components_.end());
        wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                    [&](const Wire& w) { return gone.count(w.a.component) || gone.count(w.b.component); }),
                     wires_.end());
        at.clear();
        for (size_t i = 0; i < components_.size(); ++i) at[components_[i].id] = i;
        changed = true;
    }
    for (const auto& [pkgId, units] : unitsOf) {
        Component* pkg = comp(pkgId);
        if (!pkg) continue;
        for (int id : units) {
            Component* u = comp(id);
            if (!u) continue;
            if (u->customPart != pkg->customPart || u->ref != pkg->ref || u->value != pkg->value) {
                u->customPart = pkg->customPart;
                u->ref = pkg->ref;
                u->value = pkg->value;
                changed = true;
            }
        }
        // The package sits with its first unit (its sheet; the position orders it for annotation).
        if (const Component* first = comp(units.front())) {
            if (!isRepeated(pkg->sheet) && !isRepeated(first->sheet) && pkg->sheet != first->sheet) {
                pkg->sheet = first->sheet;
                changed = true;
            }
            if (pkg->sheet == first->sheet) pkg->position = first->position;
        }
    }
    if (changed) invalidate();
    return changed;
}

void Schematic::packUnits() {
    // Units of one part and value, grouped; only parts whose signal units are all interchangeable are packed.
    std::map<std::pair<std::string, std::string>, std::vector<int>> packagesOf;
    for (const auto& c : components_)
        if (c.packageOnly) packagesOf[{c.customPart, c.value}].push_back(c.id);
    auto order = [&](int a, int b) {
        const Component* x = find(a);
        const Component* y = find(b);
        const int sx = sheetIndex(x->sheet), sy = sheetIndex(y->sheet);
        if (sx != sy) return sx < sy;
        const double ry = std::round(x->position.y / 10), ryy = std::round(y->position.y / 10);
        if (ry != ryy) return ry < ryy;
        if (x->position.x != y->position.x) return x->position.x < y->position.x;
        return a < b;
    };
    for (auto& [key, packages] : packagesOf) {
        const CustomPart* part = CustomPartRegistry::instance().find(key.first);
        if (!part) continue;
        std::vector<int> signal;  // unit indices (1-based) of the signal units
        for (size_t i = 0; i < part->units.size(); ++i)
            if (!part->units[i].power) signal.push_back(static_cast<int>(i) + 1);
        // Only interchangeable gates (the same pins in the same places, or one swap group) are re-assigned.
        const bool interchangeable = std::all_of(signal.begin(), signal.end(), [&](int u) {
            return unitsInterchangeable(part->units[static_cast<size_t>(signal.front() - 1)], part->units[static_cast<size_t>(u - 1)]);
        });
        if (signal.size() < 2 || !interchangeable) continue;
        // Packages in a repeated sheet keep their units (each channel is its own copy).
        if (std::any_of(packages.begin(), packages.end(), [&](int id) { return isRepeated(find(id)->sheet); })) continue;
        std::vector<int> units;
        for (const auto& c : components_)
            if (c.kind == ComponentKind::PartUnit && std::find(packages.begin(), packages.end(), c.unitOf) != packages.end() &&
                std::find(signal.begin(), signal.end(), c.unit) != signal.end())
                units.push_back(c.id);
        if (units.empty()) continue;
        std::sort(units.begin(), units.end(), order);
        std::sort(packages.begin(), packages.end(), [&](int a, int b) {
            const auto ua = placedUnits(a), ub = placedUnits(b);
            return order(ua.empty() ? a : ua.front(), ub.empty() ? b : ub.front());
        });
        const size_t k = signal.size();
        const size_t needed = (units.size() + k - 1) / k;
        while (packages.size() < needed) {
            const Component* model = find(packages.front());
            Component pkg = *model;
            pkg.id = nextComponentId_++;
            pkg.ref = nextRef(part->def.refPrefix);
            pkg.pcb = PcbPlacement{};
            components_.push_back(pkg);
            packages.push_back(pkg.id);
        }
        for (size_t j = 0; j < units.size(); ++j)
            if (Component* u = find(units[j])) {
                u->unitOf = packages[j / k];
                u->unit = signal[j % k];
            }
        // Power units follow: one to each package in turn (any extra keeps its own package).
        std::vector<int> power;
        for (const auto& c : components_)
            if (c.kind == ComponentKind::PartUnit && std::find(packages.begin(), packages.end(), c.unitOf) != packages.end() &&
                std::find(signal.begin(), signal.end(), c.unit) == signal.end())
                power.push_back(c.id);
        std::sort(power.begin(), power.end(), order);
        for (size_t j = 0; j < power.size() && j < needed; ++j)
            if (Component* u = find(power[j])) u->unitOf = packages[j];
    }
    invalidate();
    syncUnits();
}

void Schematic::unitERC(std::vector<RuleViolation>& out) const {
    for (const auto& pkg : components_) {
        if (!pkg.packageOnly) continue;
        const CustomPart* part = partOf(pkg);
        if (!part) continue;
        const auto units = placedUnits(pkg.id);
        if (units.empty()) continue;
        const Component* first = find(units.front());
        std::set<int> placed;
        for (int id : units)
            if (const Component* u = find(id)) placed.insert(u->unit);
        for (size_t i = 0; i < part->units.size(); ++i) {
            if (placed.count(static_cast<int>(i) + 1)) continue;
            const PartUnitDef& unit = part->units[i];
            std::vector<std::string> open, power;
            for (int pin : unit.pins) {
                const PinDef& pd = part->def.pins[static_cast<size_t>(pin)];
                if (pkg.isNoConnect(pin) || static_cast<PinType>(pd.type) == PinType::NoConnect || isPinConnected({pkg.id, pin}))
                    continue;
                (static_cast<PinType>(pd.type) == PinType::PowerIn ? power : open).push_back(pd.name);
            }
            RuleViolation v;
            v.components = {first->id};
            v.location = first->position;
            v.hasLocation = true;
            v.sheet = first->sheet;
            if (!power.empty()) {
                v.severity = Severity::Error;
                v.code = "ERC_POWER_PIN_UNCONNECTED";
                std::string list;
                for (const auto& n : power) list += (list.empty() ? "" : ", ") + pkg.ref + "." + n;
                v.message = "Power pin " + list + " is not connected: place unit " + unit.name + " of " + pkg.ref + " and wire it.";
                out.push_back(v);
            } else if (!open.empty()) {
                v.severity = Severity::Warning;
                v.code = "ERC_UNIT_NOT_PLACED";
                std::string list;
                for (const auto& n : open) list += (list.empty() ? "" : ", ") + n;
                v.message = "Unit " + unit.name + " of " + pkg.ref + " (" + part->spec.name + ") is not placed: its pins " + list +
                            " are open. Place it and tie off an unused gate's inputs.";
                out.push_back(v);
            }
        }
    }
}

bool Schematic::swapUnits(int a, int b) {
    a = masterOf(a);
    b = masterOf(b);
    Component* ua = find(a);
    Component* ub = find(b);
    if (!ua || !ub || a == b || ua->kind != ComponentKind::PartUnit || ub->kind != ComponentKind::PartUnit) return false;
    if (ua->customPart != ub->customPart || ua->sheet != ub->sheet) return false;
    const CustomPart* part = partOf(*ua);
    if (!part) return false;
    const int n = static_cast<int>(part->units.size());
    if (ua->unit < 1 || ua->unit > n || ub->unit < 1 || ub->unit > n) return false;
    if (!unitsInterchangeable(part->units[static_cast<size_t>(ua->unit - 1)], part->units[static_cast<size_t>(ub->unit - 1)]))
        return false;
    const Component* pa = find(ua->unitOf);
    const Component* pb = find(ub->unitOf);
    if (!pa || !pb || pa->value != pb->value) return false;
    // The symbols stay where they are; the gates (package and unit index) change places.
    std::swap(ua->unitOf, ub->unitOf);
    std::swap(ua->unit, ub->unit);
    invalidate();
    edited();
    return true;
}

bool Schematic::swapPins(int componentId, int pinA, int pinB) {
    const int id = masterOf(componentId);
    Component* c = find(id);
    if (!c || c->kind != ComponentKind::PartUnit || pinA == pinB) return false;
    const CustomPart* part = partOf(*c);
    if (!part || c->unit < 1 || c->unit > static_cast<int>(part->units.size())) return false;
    const PartUnitDef& unit = part->units[static_cast<size_t>(c->unit - 1)];
    const bool allowed = std::any_of(unit.pinSwap.begin(), unit.pinSwap.end(), [&](const std::vector<int>& g) {
        return std::find(g.begin(), g.end(), pinA) != g.end() && std::find(g.begin(), g.end(), pinB) != g.end();
    });
    if (!allowed) return false;
    // The wires (and no-connect flags) of the two pins change places: the same nets now reach the other pin.
    for (auto& w : wires_) {
        for (PinRef* end : {&w.a, &w.b}) {
            if (end->component != id) continue;
            if (end->pin == pinA) end->pin = pinB;
            else if (end->pin == pinB) end->pin = pinA;
        }
    }
    for (auto& nc : c->noConnect) {
        if (nc == pinA) nc = pinB;
        else if (nc == pinB) nc = pinA;
    }
    invalidate();
    edited();
    return true;
}

}  // namespace sieda
