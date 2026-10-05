// SiEDA Core — signal harnesses (Altium-style structured buses).
//
// A harness type names a bundle of signals (USB = DP, DN, VBUS, GND). A harness label stands for one bundle, named by
// its value ("USB1"); its entries — labels attached to it, each named by a member ("DP") — join the member nets
// "USB1.DP" … on their sheet: together they are the harness connector. Through the harness label's scope the whole
// bundle crosses sheets in one go: a harness port on a child sheet and the harness sheet entry of the same name on its
// parent carry every member; a global harness label carries them everywhere. Connectivity stays name based (see
// Schematic::rebuildNets), so the netlist, simulation, BOM and board see ordinary nets "USB1.DP".
#include <algorithm>
#include <cctype>
#include <map>
#include <set>

#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
bool validHarnessName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    return std::all_of(s.begin(), s.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-';
    });
}

bool validEntryName(const std::string& s) {
    if (s.empty() || s.size() > 32) return false;
    return std::none_of(s.begin(), s.end(), [](char ch) {
        return ch == '.' || std::isspace(static_cast<unsigned char>(ch)) != 0 || ch == '[' || ch == ']' || ch == ',';
    });
}

std::string trimmedText(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
}  // namespace

const HarnessType* Schematic::findHarnessType(const std::string& name) const {
    for (const auto& t : harnessTypes_)
        if (t.name == name) return &t;
    return nullptr;
}

bool Schematic::setHarnessType(const std::string& rawName, const std::vector<std::string>& rawEntries) {
    const std::string name = trimmedText(rawName);
    if (!validHarnessName(name) || rawEntries.empty() || rawEntries.size() > 256) return false;
    HarnessType t;
    t.name = name;
    std::set<std::string> seen;
    for (const auto& raw : rawEntries) {
        const std::string e = trimmedText(raw);
        if (!validEntryName(e) || !seen.insert(e).second) return false;
        t.entries.push_back(e);
    }
    for (auto& existing : harnessTypes_)
        if (existing.name == name) {
            existing = t;
            invalidate();
            return true;
        }
    if (harnessTypes_.size() >= 256) return false;
    harnessTypes_.push_back(t);
    invalidate();
    return true;
}

bool Schematic::removeHarnessType(const std::string& name) {
    const auto it = std::find_if(harnessTypes_.begin(), harnessTypes_.end(), [&](const HarnessType& t) { return t.name == name; });
    if (it == harnessTypes_.end()) return false;
    harnessTypes_.erase(it);
    invalidate();
    return true;
}

void Schematic::restoreHarnessTypes(const std::vector<HarnessType>& types) {
    harnessTypes_.clear();
    for (const auto& t : types)
        if (!findHarnessType(trimmedText(t.name))) setHarnessType(t.name, t.entries);
}

bool Schematic::setLabelHarness(int labelId, const std::string& rawType) {
    Component* c = find(masterOf(labelId));
    const std::string type = trimmedText(rawType);
    if (!c || c->kind != ComponentKind::NetLabel || c->harnessOf != 0) return false;
    if (!type.empty() && (!findHarnessType(type) || !validHarnessName(c->value))) return false;
    c->harnessType = type;
    if (type.empty())
        for (auto& e : components_)
            if (e.harnessOf == c->id) e.harnessOf = 0;  // its entries become ordinary labels
    invalidate();
    edited();
    return true;
}

int Schematic::addHarnessConnector(const std::string& rawType, const std::string& rawName, Vec2 position) {
    const std::string type = trimmedText(rawType), name = trimmedText(rawName);
    if (!findHarnessType(type) || !validHarnessName(name)) return -1;
    const int id = addComponent(ComponentKind::NetLabel, name, position);
    if (id < 0) return -1;
    const int master = masterOf(id);
    if (Component* c = find(master)) {
        c->scope = LabelScope::Local;
        c->harnessType = type;
    }
    placeHarnessEntries(master);
    invalidate();
    edited();
    return id;
}

int Schematic::placeHarnessEntries(int harnessLabel) {
    const int master = masterOf(harnessLabel);
    const Component* h = find(master);
    if (!h || !isHarnessLabel(*h)) return -1;
    const HarnessType* type = findHarnessType(h->harnessType);
    if (!type) return -1;
    std::set<std::string> present;
    double lowest = h->position.y;
    for (const auto& c : components_)
        if (c.harnessOf == master) {
            present.insert(c.value);
            lowest = std::max(lowest, c.position.y);
        }
    const Vec2 origin = h->position;
    const int sheet = h->sheet;
    const std::vector<std::string> entries = type->entries;
    int added = 0;
    const int shown = activeSheet_;
    activeSheet_ = sheet;
    for (const auto& e : entries) {
        if (present.count(e)) continue;
        lowest += 20;
        const int id = addComponent(ComponentKind::NetLabel, e, {origin.x - 40, lowest});
        if (Component* c = find(id)) {
            c->scope = LabelScope::Local;
            c->harnessOf = master;
            c->harnessType = find(master) ? find(master)->harnessType : std::string();
        }
        ++added;
    }
    activeSheet_ = shown;
    invalidate();
    if (added > 0) edited();
    return added;
}

std::string Schematic::labelNetName(const Component& c) const {
    if (c.kind != ComponentKind::NetLabel || c.harnessOf == 0) return c.value;
    const Component* h = find(c.harnessOf);
    if (!h || !isHarnessLabel(*h) || h->sheet != c.sheet) return c.value;
    return h->value + "." + c.value;
}

bool Schematic::repairHarnessLinks() {
    bool changed = false;
    for (auto& c : components_) {
        if (c.kind != ComponentKind::NetLabel) {
            if (!c.harnessType.empty() || c.harnessOf != 0) {
                c.harnessType.clear();
                c.harnessOf = 0;
                changed = true;
            }
            continue;
        }
        if (c.harnessOf == 0) continue;
        const Component* h = find(c.harnessOf);
        if (!h || h->id == c.id || !isHarnessLabel(*h) || h->sheet != c.sheet) {
            c.harnessOf = 0;
            changed = true;
        } else if (c.harnessType != h->harnessType) {
            c.harnessType = h->harnessType;  // an entry carries its harness's type (for display)
            changed = true;
        }
    }
    if (changed) invalidate();
    return changed;
}

void Schematic::harnessERC(std::vector<RuleViolation>& out) const {
    auto add = [&](Severity s, const char* code, const std::string& msg, const Component& at) {
        RuleViolation v;
        v.severity = s;
        v.code = code;
        v.message = msg;
        v.components = {at.id};
        v.location = at.position;
        v.hasLocation = true;
        v.sheet = at.sheet;
        out.push_back(std::move(v));
    };
    std::map<std::pair<int, std::string>, std::string> portTypes;  // (sheet, port name) → harness type ("" = plain)
    for (const auto& c : components_)
        if (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::Port && c.harnessOf == 0)
            portTypes.emplace(std::make_pair(c.sheet, c.value), c.harnessType);
    std::set<std::string> reportedType;
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::NetLabel || c.instanceOf != 0) continue;  // a channel copy is checked on its block
        if (isHarnessLabel(c)) {
            const HarnessType* type = findHarnessType(c.harnessType);
            if (!type) {
                add(Severity::Error, "ERC_HARNESS_UNKNOWN_TYPE",
                    "Harness " + c.value + " is of type " + c.harnessType + ", which is not defined (Harness Types).", c);
                continue;
            }
            if (c.scope == LabelScope::SheetEntry) {
                auto it = portTypes.find({c.targetSheet, c.value});
                if (it != portTypes.end() && it->second != c.harnessType)
                    add(Severity::Error, "ERC_HARNESS_TYPE_MISMATCH",
                        "Sheet entry " + c.value + " carries harness " + c.harnessType + " but the port on the child sheet " +
                            (it->second.empty() ? std::string("is a single signal") : "carries " + it->second) + ".", c);
            }
            continue;
        }
        if (c.scope == LabelScope::SheetEntry && c.harnessOf == 0) {
            auto it = portTypes.find({c.targetSheet, c.value});
            if (it != portTypes.end() && !it->second.empty())
                add(Severity::Error, "ERC_HARNESS_TYPE_MISMATCH",
                    "Sheet entry " + c.value + " is a single signal but the port on the child sheet carries harness " + it->second + ".", c);
        }
        if (c.harnessOf == 0) continue;
        const Component* h = find(c.harnessOf);
        if (!h) continue;
        const HarnessType* type = findHarnessType(h->harnessType);
        if (!type) continue;
        if (std::find(type->entries.begin(), type->entries.end(), c.value) == type->entries.end()) {
            add(Severity::Error, "ERC_HARNESS_ENTRY_NOT_MEMBER",
                "Harness entry " + c.value + " of " + h->value + " is not a member of harness type " + type->name + ".", c);
            continue;
        }
        // A member that reaches no pin: the entry is left open, or nothing carries the bundle onwards.
        const int net = netOf({c.id, 0});
        bool reachesPart = false;
        if (net >= 0)
            for (const auto& p : nets()[static_cast<size_t>(net)].pins) {
                const Component* other = find(p.component);
                if (other && !isNetSymbolKind(other->kind)) reachesPart = true;
            }
        if (!reachesPart)
            add(Severity::Warning, "ERC_HARNESS_MEMBER_UNCONNECTED",
                "Member " + h->value + "." + c.value + " of the harness reaches no part pin.", c);
    }
    for (const auto& t : harnessTypes_) {
        const bool used = std::any_of(components_.begin(), components_.end(), [&](const Component& c) { return c.harnessType == t.name; });
        if (!used && reportedType.insert(t.name).second) {
            RuleViolation v;
            v.severity = Severity::Info;
            v.code = "ERC_HARNESS_TYPE_UNUSED";
            v.message = "Harness type " + t.name + " is defined but not used.";
            out.push_back(std::move(v));
        }
    }
}

}  // namespace sieda
