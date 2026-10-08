#include "sieda/Schematic.hpp"

#include <map>
#include <cctype>

#include <algorithm>
#include <climits>
#include <cmath>
#include <functional>
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
    if (kind == ComponentKind::PartUnit) {
        const CustomPart* part = CustomPartRegistry::instance().find(customPart);
        if (part && unit >= 1 && unit <= static_cast<int>(part->units.size()))
            return part->units[static_cast<size_t>(unit - 1)].def;
    }
    return Library::instance().component(kind);
}

const std::string& Component::footprintName() const {
    if (!package.empty() && Library::instance().footprint(package)) return package;
    return def().footprint;
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
    if (isNetSymbolKind(kind)) {
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
    if (!Library::isValidKind(static_cast<int>(kind)) || kind == ComponentKind::Custom || kind == ComponentKind::PartUnit)
        return -1;
    if (const Sheet* s = findSheet(activeSheet_); s && s->instanceOf != 0 && findSheet(s->instanceOf)) {
        // Placed on an instance of a repeated sheet: it goes into the definition, and so into every instance.
        const int shown = activeSheet_;
        activeSheet_ = s->instanceOf;
        const int id = addComponent(kind, value, position, rotation, ref);
        activeSheet_ = shown;
        return id < 0 ? id : copyOn(id, shown);
    }
    Component c;
    c.id = nextComponentId_++;
    c.kind = kind;
    c.value = value.empty() ? c.def().defaultValue : value;
    c.ref = ref.empty() ? nextRef(kind) : ref;
    c.position = position;
    c.rotation = ((rotation % 360) + 360) % 360;
    c.sheet = activeSheet_;
    components_.push_back(c);
    invalidate();
    edited();
    return c.id;
}

int Schematic::addCustomComponent(const std::string& partId, const std::string& value, Vec2 position, int rotation,
                                  const std::string& ref) {
    const CustomPart* part = CustomPartRegistry::instance().find(partId);
    if (!part) return -1;
    if (const Sheet* s = findSheet(activeSheet_); s && s->instanceOf != 0 && findSheet(s->instanceOf)) {
        const int shown = activeSheet_;
        activeSheet_ = s->instanceOf;
        const int id = addCustomComponent(partId, value, position, rotation, ref);
        activeSheet_ = shown;
        return id < 0 ? id : copyOn(id, shown);
    }
    Component c;
    c.id = nextComponentId_++;
    c.kind = ComponentKind::Custom;
    c.customPart = partId;
    c.value = value.empty() ? part->def.defaultValue : value;
    c.ref = ref.empty() ? nextRef(part->def.refPrefix) : ref;
    c.position = position;
    c.rotation = ((rotation % 360) + 360) % 360;
    c.sheet = activeSheet_;
    components_.push_back(c);
    invalidate();
    edited();
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
        c.noConnect.clear();  // pin indices change with the part
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
    edited();
    return count;
}

bool Component::isNoConnect(int pin) const {
    return std::find(noConnect.begin(), noConnect.end(), pin) != noConnect.end();
}

bool Schematic::setPinNoConnect(int componentId, int pin, bool nc) {
    Component* c = find(masterOf(componentId));
    if (!c || pin < 0 || pin >= static_cast<int>(c->def().pins.size())) return false;
    auto it = std::find(c->noConnect.begin(), c->noConnect.end(), pin);
    if (nc && it == c->noConnect.end()) c->noConnect.push_back(pin);
    if (!nc && it != c->noConnect.end()) c->noConnect.erase(it);
    edited();
    return true;
}

bool Schematic::removeComponent(int id) {
    id = masterOf(id);  // a part of a repeated sheet's instance goes from the definition, so from every instance
    auto it = std::find_if(components_.begin(), components_.end(), [&](const Component& c) { return c.id == id; });
    if (it == components_.end()) return false;
    // Deleting a bend point (a junction joining exactly two wires) keeps the connection: the wire is straightened.
    std::vector<PinRef> heal;
    if (it->kind == ComponentKind::Junction) {
        for (const auto& w : wires_) {
            if (w.a.component == id) heal.push_back(w.b);
            else if (w.b.component == id) heal.push_back(w.a);
        }
        if (heal.size() != 2) heal.clear();
    }
    components_.erase(it);
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [&](const Wire& w) { return w.a.component == id || w.b.component == id; }),
                 wires_.end());
    if (heal.size() == 2) connect(heal[0], heal[1]);
    invalidate();
    edited();
    return true;
}

int Schematic::wireCount(int componentId) const {
    int n = 0;
    for (const auto& w : wires_)
        n += (w.a.component == componentId) + (w.b.component == componentId);
    return n;
}

int Schematic::splitWire(int wireId, Vec2 position) {
    if (const int master = masterWireOf(wireId); master != wireId) {
        // A wire of a repeated sheet's instance: split the definition's wire; return this instance's junction.
        auto copy = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == wireId; });
        const Component* end = copy == wires_.end() ? nullptr : find(copy->a.component);
        if (!end) return -1;
        const int sheet = end->sheet;
        const int j = splitWire(master, position);
        return j < 0 ? j : copyOn(j, sheet);
    }
    auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == wireId; });
    if (it == wires_.end()) return -1;
    const Wire old = *it;
    for (PinRef end : {old.a, old.b})  // splitting on a junction end reuses that junction
        if (const Component* c = find(end.component); c && c->kind == ComponentKind::Junction &&
                                                      c->position.x == position.x && c->position.y == position.y)
            return c->id;
    const int shown = activeSheet_;
    if (const Component* end = find(old.a.component)) activeSheet_ = end->sheet;  // on the wire's sheet
    int j = addComponent(ComponentKind::Junction, "", position);
    activeSheet_ = shown;
    wires_.erase(std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == wireId; }));
    connect(old.a, {j, 0});
    connect({j, 0}, old.b);
    invalidate();
    edited();
    return j;
}

int Schematic::removeDanglingJunctions(int junctionId) {
    int removed = 0;
    for (int id = masterOf(junctionId); id >= 0;) {
        const Component* c = find(id);
        if (!c || c->kind != ComponentKind::Junction || wireCount(id) > 1) break;
        int next = -1;
        for (const auto& w : wires_) {
            if (w.a.component == id) next = w.b.component;
            else if (w.b.component == id) next = w.a.component;
        }
        removeComponent(id);
        ++removed;
        id = next;
    }
    return removed;
}

bool Schematic::moveComponent(int id, Vec2 position) {
    Component* c = find(masterOf(id));
    if (!c) return false;
    c->position = position;
    edited();
    return true;  // geometry only; connectivity unchanged
}

bool Schematic::rotateComponent(int id, int deltaDeg) {
    Component* c = find(masterOf(id));
    if (!c) return false;
    c->rotation = (((c->rotation + deltaDeg) % 360) + 360) % 360;
    edited();
    return true;
}

bool Schematic::setValue(int id, const std::string& value) {
    if (const int pkg = unitPackage(masterOf(id)); pkg > 0) id = pkg;  // a unit's value is its package's
    Component* c = find(masterOf(id));
    if (!c) return false;
    c->value = value;
    if (c->kind == ComponentKind::NetLabel || c->kind == ComponentKind::Ground) invalidate();
    edited();
    return true;
}

bool Schematic::setLabelScope(int id, LabelScope scope, int targetSheet) {
    Component* c = find(masterOf(id));
    if (!c || c->kind != ComponentKind::NetLabel) return false;
    if (scope == LabelScope::SheetEntry) {
        if (!findSheet(targetSheet) || targetSheet == c->sheet) return false;
    } else {
        targetSheet = 0;
    }
    c->scope = scope;
    c->targetSheet = targetSheet;
    invalidate();
    edited();
    return true;
}

bool Schematic::setPackage(int id, const std::string& package) {
    Component* c = find(masterOf(id));
    if (!c) return false;
    const auto variants = Library::packageVariants(c->kind);
    if (!package.empty() && std::find(variants.begin(), variants.end(), package) == variants.end()) return false;
    c->package = package == c->def().footprint ? std::string() : package;
    edited();
    return true;
}

bool Schematic::setFirmware(int id, const std::string& hex, const std::string& name, double clockHz) {
    Component* c = find(masterOf(id));
    if (!c) return false;
    c->firmware = hex;
    c->firmwareName = hex.empty() ? std::string() : name;
    c->clockHz = clockHz > 0 ? clockHz : 0;
    edited();
    return true;
}

bool Schematic::setSpiceModel(int id, const SpiceModelRef& model) {
    Component* c = find(id);
    if (!c) return false;
    c->spice = model.text.empty() ? SpiceModelRef{} : model;
    return true;
}

bool Schematic::setRef(int id, const std::string& ref) {
    if (const int pkg = unitPackage(masterOf(id)); pkg > 0) id = pkg;  // a unit's designator is its package's
    Component* c = find(masterOf(id));
    if (!c || ref.empty()) return false;
    if (hasInstances() && isRepeated(c->sheet) && !isNetSymbolKind(c->kind)) {
        // A part of a repeated sheet: `ref` is its designator inside the block; each channel's follows from it.
        for (const auto& o : components_)
            if (o.sheet == c->sheet && o.id != c->id && o.logicalRef == ref) return false;
        c->logicalRef = ref;
        syncInstances();
        return true;
    }
    c->ref = ref;
    edited();
    return true;
}

int Schematic::connect(PinRef a, PinRef b) {
    const Component* ca = find(a.component);
    const Component* cb = find(b.component);
    if (!ca || !cb || a == b) return -1;
    if (ca->instanceOf != 0 && cb->instanceOf != 0 && ca->sheet == cb->sheet) {
        // Between parts of a repeated sheet's instance: the wire is drawn on the definition, so in every instance.
        const int sheet = ca->sheet;
        const int w = connect({masterOf(a.component), a.pin}, {masterOf(b.component), b.pin});
        return w < 0 ? w : copyWireOn(w, sheet);
    }
    if (ca->sheet != cb->sheet) return -1;  // wires stay on one sheet; labels and ports join sheets
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
    edited();
    return w.id;
}

bool Schematic::removeWire(int id) {
    id = masterWireOf(id);
    auto it = std::find_if(wires_.begin(), wires_.end(), [&](const Wire& w) { return w.id == id; });
    if (it == wires_.end()) return false;
    const Wire old = *it;
    wires_.erase(it);
    // A junction left with no wires is gone with them.
    for (int end : {old.a.component, old.b.component}) {
        const Component* c = find(end);
        if (c && c->kind == ComponentKind::Junction && wireCount(end) == 0) removeComponent(end);
    }
    invalidate();
    edited();
    return true;
}

void Schematic::clear() {
    components_.clear();
    wires_.clear();
    buses_.clear();
    nextBusId_ = 1;
    sheets_ = {Sheet{1, "Main", 0}};
    activeSheet_ = 1;
    nextSheetId_ = 2;
    nextComponentId_ = 1;
    nextWireId_ = 1;
    // Directives sit on components; the design's definitions (harness types, net classes, ERC levels) stay.
    directives_.clear();
    nextDirectiveId_ = 1;
    invalidate();
}

void Schematic::restoreComponent(const Component& c) {
    components_.push_back(c);
    nextComponentId_ = std::max(nextComponentId_, nextIdAfter(c.id));
    invalidate();
}

void Schematic::restoreWire(const Wire& w) {
    wires_.push_back(w);
    nextWireId_ = std::max(nextWireId_, nextIdAfter(w.id));
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
    // The part itself, not one of its placed units (a multi-unit part's units share its designator).
    for (const auto& c : components_)
        if (c.ref == ref && c.kind != ComponentKind::PartUnit) return &c;
    for (const auto& c : components_)
        if (c.ref == ref) return &c;
    return nullptr;
}

int Schematic::pinIndex(int componentId, const std::string& pinName) const {
    const Component* c = find(componentId);
    if (!c) return -1;
    const auto& pins = c->def().pins;
    if (c->kind == ComponentKind::Custom || c->kind == ComponentKind::PartUnit) {
        // Datasheet pin numbers take precedence ("U1.4", "U1.EP"); a unit answers to its package's pin numbers.
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
    // Multi-function names answer to each function, case-insensitively: "PB5/SCK" is "PB5" or "SCK".
    auto upper = [](std::string v) {
        for (auto& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return v;
    };
    const std::string key = upper(pinName);
    for (size_t i = 0; i < pins.size(); ++i) {
        std::string name = upper(pins[i].name);
        if (name == key) return static_cast<int>(i);
        size_t start = 0;
        while (start <= name.size()) {
            size_t slash = name.find('/', start);
            if (name.substr(start, slash == std::string::npos ? std::string::npos : slash - start) == key && !key.empty())
                return static_cast<int>(i);
            if (slash == std::string::npos) break;
            start = slash + 1;
        }
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
    // Signal harnesses: each harness sheet entry joins its members on the child sheet to the same members on its own
    // sheet, each global harness label its sheet's members to the global ones. The member keys get nodes of their
    // own (after the pins) so a bundle can join names that no pin carries yet. (sheet 0 = the global namespace.)
    std::map<std::pair<int, std::string>, int> memberNode;
    std::vector<std::pair<int, int>> bridges;
    for (const auto& c : components_) {
        if (!isHarnessLabel(c) || (c.scope != LabelScope::SheetEntry && c.scope != LabelScope::Global)) continue;
        const HarnessType* type = findHarnessType(c.harnessType);
        if (!type) continue;
        for (const auto& e : type->entries) {
            const std::string member = c.value + "." + e;
            const std::pair<int, std::string> here{c.sheet, member};
            const std::pair<int, std::string> there{c.scope == LabelScope::SheetEntry ? c.targetSheet : 0, member};
            const int a = memberNode.emplace(here, static_cast<int>(pins.size() + memberNode.size())).first->second;
            const int b = memberNode.emplace(there, static_cast<int>(pins.size() + memberNode.size())).first->second;
            bridges.push_back({a, b});
        }
    }
    UnionFind uf(pins.size() + memberNode.size());
    for (const auto& [a, b] : bridges) uf.unite(a, b);
    // Stacked symbol pins (several pins of one part drawn on the same spot, e.g. repeated VDD / GND) are one node.
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::Custom) continue;
        const auto& defPins = c.def().pins;
        std::map<std::pair<double, double>, int> first;  // pin offset → first pin there
        for (size_t p = 0; p < defPins.size(); ++p) {
            auto [it, fresh] = first.emplace(std::make_pair(defPins[p].offset.x, defPins[p].offset.y), static_cast<int>(p));
            if (!fresh) uf.unite(index[{c.id, it->second}], index[{c.id, static_cast<int>(p)}]);
        }
    }
    // A placed unit's pins are its package's pins.
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::PartUnit) continue;
        const CustomPart* part = CustomPartRegistry::instance().find(c.customPart);
        if (!part || c.unit < 1 || c.unit > static_cast<int>(part->units.size())) continue;
        const auto& unitPins = part->units[static_cast<size_t>(c.unit - 1)].pins;
        for (size_t p = 0; p < unitPins.size(); ++p) {
            auto iu = index.find({c.id, static_cast<int>(p)}), ip = index.find({c.unitOf, unitPins[p]});
            if (iu != index.end() && ip != index.end()) uf.unite(iu->second, ip->second);
        }
    }
    for (const auto& w : wires_) {
        auto ia = index.find(w.a), ib = index.find(w.b);
        if (ia != index.end() && ib != index.end()) uf.unite(ia->second, ib->second);
    }
    int groundRoot = -1;
    std::map<std::string, int> labelRoot;                       // global labels
    std::map<std::pair<int, std::string>, int> sheetLabelRoot;  // (sheet, name): local labels, ports, sheet entries
    for (const auto& c : components_) {
        bool ground = c.kind == ComponentKind::Ground || (c.kind == ComponentKind::NetLabel && isGroundName(c.value));
        if (ground) {
            int i = index[{c.id, 0}];
            if (groundRoot < 0) groundRoot = i;
            else uf.unite(groundRoot, i);
        } else if (c.kind == ComponentKind::NetLabel && isHarnessLabel(c)) {
            continue;  // a bundle: its members are joined above, its own pin joins nothing
        } else if (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::Global && c.harnessOf == 0) {
            int i = index[{c.id, 0}];
            auto it = labelRoot.find(c.value);
            if (it == labelRoot.end()) labelRoot[c.value] = i;
            else uf.unite(it->second, i);
            if (!memberNode.empty())
                if (auto m = memberNode.find({0, c.value}); m != memberNode.end()) uf.unite(m->second, i);
        } else if (c.kind == ComponentKind::NetLabel) {
            // A sheet entry joins the ports (and local labels) of its name on the sheet it stands for; a harness entry
            // joins its member ("USB1.DP") on its own sheet.
            int i = index[{c.id, 0}];
            const bool member = c.harnessOf != 0;
            auto key = std::make_pair(c.scope == LabelScope::SheetEntry && !member ? c.targetSheet : c.sheet,
                                      member ? labelNetName(c) : c.value);
            auto it = sheetLabelRoot.find(key);
            if (it == sheetLabelRoot.end()) sheetLabelRoot[key] = i;
            else uf.unite(it->second, i);
            if (!memberNode.empty())
                if (auto m = memberNode.find(key); m != memberNode.end()) uf.unite(m->second, i);
        }
    }

    // Junctions only join wires: they are not net members, and a net exists only where a real pin is.
    std::vector<bool> isJunction(pins.size());
    // Units' pins are not members either: their package's pins are.
    for (size_t i = 0; i < pins.size(); ++i) {
        const Component* owner = find(pins[i].component);
        const ComponentKind k = owner->kind;
        isJunction[i] = k == ComponentKind::Junction || k == ComponentKind::PartUnit || isHarnessLabel(*owner);
    }
    std::map<int, int> rootToNet;
    for (size_t i = 0; i < pins.size(); ++i) {
        if (isJunction[i]) continue;
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
    for (size_t i = 0; i < pins.size(); ++i) {
        if (!isJunction[i]) continue;
        auto it = rootToNet.find(uf.find(static_cast<int>(i)));
        if (it != rootToNet.end()) pinToNet_[pins[i]] = it->second;
    }

    int autoCounter = 1;
    std::vector<std::pair<size_t, int>> sheetNamed;  // nets named by a sheet-scoped label → that label's sheet
    for (auto& n : nets_) {
        std::string label;           // global labels name the net first (the smallest name)
        std::string local;           // then sheet-scoped labels, from the sheet nearest the top of the hierarchy
        int localDepth = INT_MAX, localSheet = 0;
        for (const auto& p : n.pins) {
            const Component* c = find(p.component);
            if (c->kind == ComponentKind::Ground || (c->kind == ComponentKind::NetLabel && isGroundName(c->value))) {
                n.isGround = true;
            } else if (c->kind == ComponentKind::NetLabel && c->scope == LabelScope::Global && c->harnessOf == 0) {
                if (label.empty() || c->value < label) label = c->value;
            } else if (c->kind == ComponentKind::NetLabel) {
                int depth = sheetDepth(c->sheet);
                const std::string name = labelNetName(*c);
                if (depth < localDepth || (depth == localDepth && name < local)) {
                    local = name;
                    localDepth = depth;
                    localSheet = c->sheet;
                }
            }
        }
        if (n.isGround) {
            n.name = "GND";
        } else if (!label.empty()) {
            n.name = label;
        } else if (!local.empty()) {
            n.name = local;
            sheetNamed.push_back({static_cast<size_t>(n.index), localSheet});
        } else {
            n.name = "N$" + std::to_string(autoCounter++);
        }
    }
    if (!sheetNamed.empty()) {
        // A sheet-local name used by another net too gets its sheet's name in front ("Power/EN"), kept unique.
        std::map<std::string, int> uses;
        for (const auto& n : nets_) ++uses[n.name];
        std::set<std::string> taken;
        for (const auto& n : nets_) taken.insert(n.name);
        for (const auto& [net, sheet] : sheetNamed) {
            Net& n = nets_[net];
            if (uses[n.name] < 2) continue;
            const Sheet* s = findSheet(sheet);
            std::string base = (s ? s->name : "Sheet" + std::to_string(sheet)) + "/" + n.name;
            std::string name = base;
            for (int k = 2; taken.count(name); ++k) name = base + "_" + std::to_string(k);
            taken.insert(name);
            n.name = name;
        }
    }
    netsDirty_ = false;
}

bool Schematic::isPinConnected(PinRef pin) const {
    const int net = netOf(pin);
    if (net < 0) return false;
    const Component* self = find(pin.component);
    if (self && self->kind == ComponentKind::PartUnit) {  // a unit's pin is its package's pin
        const CustomPart* part = CustomPartRegistry::instance().find(self->customPart);
        if (!part || self->unit < 1 || self->unit > static_cast<int>(part->units.size())) return false;
        const auto& unitPins = part->units[static_cast<size_t>(self->unit - 1)].pins;
        if (pin.pin < 0 || static_cast<size_t>(pin.pin) >= unitPins.size()) return false;
        return isPinConnected({self->unitOf, unitPins[static_cast<size_t>(pin.pin)]});
    }
    if (!self || pin.pin < 0 || static_cast<size_t>(pin.pin) >= self->def().pins.size()) return false;
    const Vec2 spot = self->def().pins[static_cast<size_t>(pin.pin)].offset;
    for (const auto& other : nets()[static_cast<size_t>(net)].pins) {
        if (other == pin) continue;
        if (other.component != pin.component) return true;
        const Vec2 o = self->def().pins[static_cast<size_t>(other.pin)].offset;
        if (o.x != spot.x || o.y != spot.y) return true;  // wired to another pin of the same part
    }
    return false;
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

const char* netRoleName(NetRole r) {
    switch (r) {
        case NetRole::Power: return "power";
        case NetRole::Ground: return "ground";
        case NetRole::NegativeSupply: return "negative";
        case NetRole::Signal: break;
    }
    return "signal";
}

NetRole Schematic::netRole(int net) const {
    const auto& all = nets();
    if (net < 0 || net >= static_cast<int>(all.size())) return NetRole::Signal;
    const Net& n = all[static_cast<size_t>(net)];
    if (n.isGround) return NetRole::Ground;
    std::string name;
    for (char ch : n.name) name += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    auto has = [&](const char* s) { return name.find(s) != std::string::npos; };
    auto startsWith = [&](const char* s) { return name.rfind(s, 0) == 0; };
    // Negative rails by name: -5V, -12V, VEE, VNEG, V-.
    if ((startsWith("-") && name.size() > 1 && std::isdigit(static_cast<unsigned char>(name[1]))) || startsWith("VEE") ||
        has("VNEG") || name == "V-")
        return NetRole::NegativeSupply;
    // Supply rails by name: VCC, VDD, VIN, VBAT, VBUS, +5V, 3V3, 12V, 1.8V, V+ …
    for (const char* rail : {"VCC", "VDD", "VIN", "VBAT", "VBUS", "VSYS", "VREG", "VMOT", "VSUP", "VPP", "PWR", "POWER",
                             "VMAIN", "VLOGIC", "VSERVO", "VLED", "VCORE", "VTT"})
        if (has(rail)) return NetRole::Power;
    if (startsWith("+") || name == "V+") return NetRole::Power;
    {
        // 5V, 12V, 3V3, 1V8, 3.3V, 5V0
        size_t i = 0;
        while (i < name.size() && (std::isdigit(static_cast<unsigned char>(name[i])) || name[i] == '.')) ++i;
        if (i > 0 && i < name.size() && name[i] == 'V') {
            size_t j = i + 1;
            while (j < name.size() && std::isdigit(static_cast<unsigned char>(name[j]))) ++j;
            if (j == name.size()) return NetRole::Power;
        }
    }
    // By what the net connects to.
    const int gnd = groundNet();
    bool power = false, negative = false;
    for (const auto& pin : n.pins) {
        const Component* c = find(pin.component);
        if (!c) continue;
        if (isVoltageSourceKind(c->kind)) {
            // A DC supply, battery or mains source powers its + net; a SIN/PULSE test source drives a signal.
            auto spec = SourceSpec::parse(c->value);
            bool supply = c->kind != ComponentKind::VoltageSource || (spec && spec->kind == SourceSpec::Kind::DC);
            if (!supply) continue;
            int other = netOf({c->id, pin.pin == 0 ? 1 : 0});
            if (pin.pin == 0) power = true;                             // the + terminal
            else if (gnd >= 0 && other == gnd) negative = true;         // − terminal below a grounded +
            continue;
        }
        if (c->kind == ComponentKind::Custom) {
            const auto& pins = c->def().pins;
            if (pin.pin < 0 || pin.pin >= static_cast<int>(pins.size())) continue;
            PinType type = static_cast<PinType>(pins[static_cast<size_t>(pin.pin)].type);
            std::string pinName;
            for (char ch : pins[static_cast<size_t>(pin.pin)].name)
                pinName += static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
            if (type == PinType::PowerIn || type == PinType::PowerOut) {
                if (pinName.find("GND") != std::string::npos || pinName == "VSS" || pinName == "AGND" || pinName == "PGND")
                    return NetRole::Ground;
                if (pinName.find("VEE") != std::string::npos || pinName == "V-") negative = true;
                else power = true;
            }
        }
    }
    if (negative) return NetRole::NegativeSupply;
    if (power) return NetRole::Power;
    return NetRole::Signal;
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
        if (!comps.empty())
            if (const Component* first = find(comps.front())) v.sheet = first->sheet;
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
        if (!isNetSymbolKind(c.kind)) refs[c.ref].push_back(c.id);
    for (const auto& [ref, ids] : refs)
        if (ids.size() > 1)
            add(Severity::Error, "ERC_DUPLICATE_REF", "Reference designator " + ref + " is used " +
                std::to_string(ids.size()) + " times.", ids, find(ids[0])->position);

    bool hasSources = false;
    for (const auto& c : components_)
        if (isSourceKind(c.kind)) hasSources = true;
    int gnd = groundNet();
    if (hasSources && gnd < 0)
        add(Severity::Error, "ERC_NO_GROUND", "Circuit has sources but no ground reference. Add a Ground symbol.", {},
            components_.front().position);

    for (const auto& c : components_) {
        if (c.kind == ComponentKind::Junction) {
            if (wireCount(c.id) < 2)
                add(Severity::Warning, "ERC_DANGLING_WIRE", "A wire ends in empty space (not on a pin or another wire).",
                    {c.id}, c.position);
            continue;
        }
        if (c.packageOnly) continue;  // a multi-unit part is checked through its units (and unitERC)
        if (c.kind == ComponentKind::PartUnit) {
            const auto& unitPins = c.def().pins;
            const std::string name = displayRef(c);
            std::vector<size_t> open;
            for (size_t p = 0; p < unitPins.size(); ++p)
                if (!isPinConnected({c.id, static_cast<int>(p)})) open.push_back(p);
            if (open.size() == unitPins.size() && unitPins.size() > 1) {
                add(Severity::Warning, "ERC_FLOATING_COMPONENT", name + " is not connected to the circuit.", {c.id}, c.position);
                continue;
            }
            for (size_t p : open) {
                const auto type = static_cast<PinType>(unitPins[p].type);
                if (c.isNoConnect(static_cast<int>(p)) || type == PinType::NoConnect) continue;
                if (type == PinType::PowerIn)
                    add(Severity::Error, "ERC_POWER_PIN_UNCONNECTED", "Power pin " + name + "." + unitPins[p].name + " is not connected.",
                        {c.id}, c.position);
                else
                    add(Severity::Warning, "ERC_UNCONNECTED_PIN", "Pin " + name + "." + unitPins[p].name +
                            " is unconnected — wire it or mark it no-connect if it is unused.", {c.id}, c.position);
            }
            continue;
        }
        const auto& pins = c.def().pins;
        int unconnected = 0;
        std::vector<std::string> openPins;
        std::vector<int> openIdx;
        for (size_t p = 0; p < pins.size(); ++p) {
            if (!isPinConnected({c.id, static_cast<int>(p)})) {
                ++unconnected;
                openPins.push_back(pins[p].name);
                openIdx.push_back(static_cast<int>(p));
            }
        }
        if (isHarnessLabel(c)) continue;  // a bundle: checked by harnessERC
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
            for (size_t o = 0; o < openPins.size(); ++o) {
                const std::string& name = openPins[o];
                if (c.isNoConnect(openIdx[o])) continue;  // flagged as intentionally open
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
                add(s, "ERC_UNCONNECTED_PIN",
                    "Pin " + c.ref + "." + name + " is unconnected — wire it or mark it no-connect if it is unused.",
                    {c.id}, c.position);
            }
        }

        for (int pin : c.noConnect) {
            int net = netOf({c.id, pin});
            if (net >= 0 && allNets[static_cast<size_t>(net)].pins.size() > 1 && pin < static_cast<int>(pins.size()))
                add(Severity::Warning, "ERC_NC_CONNECTED",
                    "Pin " + c.ref + "." + pins[static_cast<size_t>(pin)].name +
                        " has a no-connect flag but is wired to net " + allNets[static_cast<size_t>(net)].name + ".",
                    {c.id}, c.position);
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
            case ComponentKind::Battery:
            case ComponentKind::ACSource:
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
                // Connectors are plain interconnect; parts with a behavioural model (regulators, IC supply
                // current) are simulated.
                if (c.kind == ComponentKind::Connector) break;
                if (c.kind == ComponentKind::Custom) {
                    const CustomPart* part = CustomPartRegistry::instance().find(c.customPart);
                    if (part && !part->spec.model.empty()) break;
                }
                add(Severity::Info, "ERC_NOT_SIMULATED", c.ref + " (" + c.def().name + ") has no simulation model.",
                    {c.id}, c.position);
                break;
            default: break;
        }

        if (isVoltageSourceKind(c.kind)) {
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
                    return isVoltageSourceKind(find(p.component)->kind);
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
            // Drivers on different sheets are easy to miss: name the sheets.
            std::vector<std::string> sheetNames;
            for (int id : ids) {
                const Sheet* s = findSheet(find(id)->sheet);
                if (s && std::find(sheetNames.begin(), sheetNames.end(), s->name) == sheetNames.end())
                    sheetNames.push_back(s->name);
            }
            std::string across;
            if (sheetNames.size() > 1) {
                across = " (across sheets ";
                for (size_t i = 0; i < sheetNames.size(); ++i) across += (i ? ", " : "") + sheetNames[i];
                across += ")";
            }
            add(Severity::Warning, "ERC_OUTPUT_CONFLICT", "Net " + net.name + " is driven by several outputs: " + list +
                    across + ".", ids, find(ids[0])->position);
        }
    }
    hierarchyERC(out);
    busERC(out);
    unitERC(out);
    if (!harnessTypes_.empty() || std::any_of(components_.begin(), components_.end(), [](const Component& c) {
            return !c.harnessType.empty();
        }))
        harnessERC(out);
    if (!directives_.empty()) directiveERC(out);
    if (!ercSeverity_.empty()) {
        // Error reporting: rules reported at another severity, or left out.
        std::vector<RuleViolation> kept;
        for (auto& v : out) {
            auto it = ercSeverity_.find(v.code);
            if (it == ercSeverity_.end()) {
                kept.push_back(std::move(v));
            } else if (it->second >= 0) {
                v.severity = static_cast<Severity>(it->second);
                kept.push_back(std::move(v));
            }
        }
        out = std::move(kept);
    }
    return out;
}

bool Schematic::setErcSeverity(const std::string& code, int level) {
    if (code.empty() || code.size() > 64 || level < -1 || level > 2) return false;
    ercSeverity_[code] = level;
    return true;
}

bool Schematic::clearErcSeverity(const std::string& code) { return ercSeverity_.erase(code) > 0; }

}  // namespace sieda
