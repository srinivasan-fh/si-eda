// SiEDA Core — back-annotation: changes made for the board (designators re-numbered by board position, pin swaps,
// gate swaps) proposed to the schematic as an ECO the designer reviews, then applied in one step.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <sstream>

#include "sieda/Project.hpp"

namespace sieda {

namespace {
std::string prefixOf(const std::string& ref) {
    size_t k = ref.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(ref[k - 1]))) --k;
    return ref.substr(0, k);
}

/// The component a designator names: a multi-unit part's package, or the part.
const Component* byRef(const Schematic& s, const std::string& ref) {
    for (const auto& c : s.components())
        if (c.ref == ref && !isNetSymbolKind(c.kind) && c.kind != ComponentKind::PartUnit) return &c;
    return nullptr;
}
}  // namespace

std::vector<EcoChange> Project::reannotateFromBoard(bool byColumns) const {
    std::vector<EcoChange> out;
    struct Item {
        const Component* c;
        double major, minor;
    };
    std::map<std::string, std::vector<Item>> byPrefix;
    std::set<std::string> fixed;  // designators that stay (parts not re-numbered)
    for (const auto& c : schematic.components()) {
        if (isNetSymbolKind(c.kind) || c.kind == ComponentKind::PartUnit) continue;
        const bool candidate = c.hasFootprint() && c.pcb.placed && c.instanceOf == 0 && c.logicalRef.empty() &&
                               !schematic.isRepeated(c.sheet);
        if (!candidate) {
            fixed.insert(c.ref);
            continue;
        }
        // Within 1 mm counts as one row (or column).
        const double x = std::round(c.pcb.position.x), y = std::round(c.pcb.position.y);
        byPrefix[prefixOf(c.ref).empty() ? std::string("U") : prefixOf(c.ref)].push_back(
            {&c, byColumns ? x : y, byColumns ? y : x});
    }
    for (auto& [prefix, items] : byPrefix) {
        std::stable_sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
            if (a.major != b.major) return a.major < b.major;
            if (a.minor != b.minor) return a.minor < b.minor;
            return a.c->id < b.c->id;
        });
        int n = 1;
        for (const auto& it : items) {
            std::string to;
            do to = prefix + std::to_string(n++);
            while (fixed.count(to));
            if (to == it.c->ref) continue;
            EcoChange e;
            e.kind = "rename";
            e.component = it.c->id;
            e.from = it.c->ref;
            e.to = to;
            out.push_back(e);
        }
    }
    return out;
}

std::vector<EcoChange> Project::ecoFromWasIs(const std::string& text) const {
    std::vector<EcoChange> out;
    std::istringstream lines(text);
    std::string line;
    int count = 0;
    while (std::getline(lines, line) && count < 100000) {
        ++count;
        if (const auto hash = line.find('#'); hash != std::string::npos) line = line.substr(0, hash);
        std::istringstream words(line);
        std::vector<std::string> w;
        for (std::string t; words >> t && w.size() < 6;) w.push_back(t);
        if (w.empty()) continue;
        std::string head = w[0];
        for (auto& ch : head) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        EcoChange e;
        if (head == "PINSWAP" && w.size() == 4) {
            e.kind = "pinSwap";
            e.from = w[1] + "." + w[2];
            e.to = w[1] + "." + w[3];
            const Component* c = byRef(schematic, w[1]);
            if (!c) {
                e.applicable = false;
                e.note = "No part " + w[1] + ".";
            } else {
                e.component = c->id;
                e.pinA = schematic.pinIndex(c->id, w[2]);
                e.pinB = schematic.pinIndex(c->id, w[3]);
                if (e.pinA < 0 || e.pinB < 0 || e.pinA == e.pinB) {
                    e.applicable = false;
                    e.note = w[1] + " has no pins " + w[2] + " and " + w[3] + ".";
                } else if (c->packageOnly) {
                    // Both pins must be drawn by one unit.
                    int unitA = -1, unitB = -1;
                    for (int u : schematic.placedUnits(c->id)) {
                        const Component* unit = schematic.find(u);
                        const CustomPart* part = unit ? CustomPartRegistry::instance().find(unit->customPart) : nullptr;
                        if (!part || unit->unit < 1 || unit->unit > static_cast<int>(part->units.size())) continue;
                        const auto& pins = part->units[static_cast<size_t>(unit->unit - 1)].pins;
                        if (std::find(pins.begin(), pins.end(), e.pinA) != pins.end() && unitA < 0) unitA = u;
                        if (std::find(pins.begin(), pins.end(), e.pinB) != pins.end() && unitB < 0) unitB = u;
                    }
                    if (unitA < 0 || unitA != unitB) {
                        e.applicable = false;
                        e.note = "Pins " + w[2] + " and " + w[3] + " of " + w[1] + " are not on one placed unit.";
                    }
                }
            }
        } else if (head == "GATESWAP" && w.size() == 3) {
            e.kind = "gateSwap";
            e.from = w[1];
            e.to = w[2];
            for (const auto& c : schematic.components())
                if (c.kind == ComponentKind::PartUnit) {
                    if (schematic.displayRef(c) == w[1]) e.component = c.id;
                    if (schematic.displayRef(c) == w[2]) e.other = c.id;
                }
            if (e.component < 0 || e.other < 0) {
                e.applicable = false;
                e.note = "Units " + w[1] + " / " + w[2] + " are not placed.";
            }
        } else if (w.size() == 2) {
            e.kind = "rename";
            e.from = w[0];
            e.to = w[1];
            const Component* c = byRef(schematic, w[0]);
            if (!c) {
                e.applicable = false;
                e.note = "No part " + w[0] + ".";
            } else {
                e.component = c->id;
                if (c->instanceOf != 0 || !c->logicalRef.empty()) {
                    e.applicable = false;
                    e.note = w[0] + " is a part of a repeated sheet: its designator follows its block designator.";
                }
            }
            if (e.from == e.to) continue;
        } else {
            e.kind = "invalid";
            e.from = line;
            e.applicable = false;
            e.note = "Not a WAS / IS line.";
        }
        out.push_back(e);
    }
    // Renames must end unique: a target that another part keeps (or that several parts get) is refused. Refusing one
    // can make another's target taken, so this repeats until nothing changes.
    for (bool again = true; again;) {
        again = false;
        std::set<std::string> renamedFrom;
        std::map<std::string, int> targets;
        for (const auto& e : out)
            if (e.kind == "rename" && e.applicable) {
                renamedFrom.insert(e.from);
                ++targets[e.to];
            }
        for (auto& e : out) {
            if (e.kind != "rename" || !e.applicable) continue;
            if (targets[e.to] > 1) {
                e.applicable = false;
                e.note = e.to + " is the new designator of several parts.";
                again = true;
            } else if (byRef(schematic, e.to) && !renamedFrom.count(e.to)) {
                e.applicable = false;
                e.note = e.to + " is used by another part.";
                again = true;
            }
        }
    }
    return out;
}

int Project::applyEco(const std::vector<EcoChange>& changes) {
    int applied = 0;
    // Renames in two steps (through temporary designators) so swaps and chains work.
    std::vector<const EcoChange*> renames;
    for (const auto& e : changes)
        if (e.kind == "rename" && e.applicable && !e.to.empty() && schematic.find(e.component)) renames.push_back(&e);
    // The chosen renames must leave every designator unique (a review may have dropped half of a swap).
    for (bool again = true; again;) {
        again = false;
        std::map<int, std::string> finalRef;
        for (const EcoChange* e : renames) finalRef[schematic.unitPackage(e->component) > 0 ? schematic.unitPackage(e->component) : e->component] = e->to;
        std::map<std::string, int> uses;
        for (const auto& c : schematic.components()) {
            if (isNetSymbolKind(c.kind)) continue;
            auto it = finalRef.find(c.id);
            ++uses[it == finalRef.end() ? c.ref : it->second];
        }
        for (auto it = renames.begin(); it != renames.end(); ++it)
            if (uses[(*it)->to] > 1) {
                renames.erase(it);
                again = true;
                break;
            }
    }
    for (size_t i = 0; i < renames.size(); ++i) schematic.setRef(renames[i]->component, "~ECO" + std::to_string(i));
    for (const EcoChange* e : renames)
        if (schematic.setRef(e->component, e->to)) ++applied;
    for (const auto& e : changes) {
        if (!e.applicable) continue;
        if (e.kind == "pinSwap") {
            const Component* c = schematic.find(e.component);
            if (!c) continue;
            if (!c->packageOnly) {
                if (schematic.swapPinConnections(e.component, e.pinA, e.pinB)) ++applied;
                continue;
            }
            for (int u : schematic.placedUnits(c->id)) {
                const Component* unit = schematic.find(u);
                const CustomPart* part = unit ? CustomPartRegistry::instance().find(unit->customPart) : nullptr;
                if (!part || unit->unit < 1 || unit->unit > static_cast<int>(part->units.size())) continue;
                const auto& pins = part->units[static_cast<size_t>(unit->unit - 1)].pins;
                const auto a = std::find(pins.begin(), pins.end(), e.pinA), b = std::find(pins.begin(), pins.end(), e.pinB);
                if (a == pins.end() || b == pins.end()) continue;
                if (schematic.swapPinConnections(u, static_cast<int>(a - pins.begin()), static_cast<int>(b - pins.begin())))
                    ++applied;
                break;
            }
        } else if (e.kind == "gateSwap") {
            if (schematic.swapUnits(e.component, e.other)) ++applied;
        }
    }
    if (applied > 0) schematicChanged();
    return applied;
}

}  // namespace sieda
