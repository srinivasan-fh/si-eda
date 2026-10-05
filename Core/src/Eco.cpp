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

// ---------------------------------------------------------------- forward annotation (Update PCB)

PcbSyncBaseline Project::currentSync() const {
    PcbSyncBaseline b;
    for (const auto& c : schematic.components())
        if (c.hasFootprint() && !isNetSymbolKind(c.kind)) b.parts[c.id] = {c.ref, c.footprintName(), c.value};
    for (const auto& n : schematic.nets()) {
        std::vector<std::string> pins;
        for (const auto& pr : n.pins) {
            const Component* c = schematic.find(pr.component);
            if (!c || !c->hasFootprint() || isNetSymbolKind(c->kind)) continue;
            const auto& def = c->def().pins[static_cast<size_t>(pr.pin)];
            pins.push_back(c->ref + "." + (def.number.empty() ? std::to_string(pr.pin + 1) : def.number));
        }
        if (pins.empty()) continue;
        std::sort(pins.begin(), pins.end());
        std::string joined;
        for (const auto& p : pins) joined += (joined.empty() ? "" : " ") + p;
        b.nets[n.name] = joined;
    }
    return b;
}

namespace {
std::vector<std::string> words(const std::string& s) {
    std::vector<std::string> out;
    std::istringstream in(s);
    for (std::string w; in >> w;) out.push_back(w);
    return out;
}

std::string mm(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.2f mm", v);
    return b;
}
}  // namespace

std::vector<PcbEcoChange> Project::pcbEcoPreview() const {
    std::vector<PcbEcoChange> out;
    const PcbSyncBaseline now = currentSync();
    auto add = [&](const char* section, const char* action, const std::string& object, const std::string& detail,
                   const std::string& key) {
        PcbEcoChange e;
        e.section = section;
        e.action = action;
        e.object = object;
        e.detail = detail;
        e.key = key;
        out.push_back(e);
    };
    for (const auto& [id, part] : now.parts) {
        const Component* c = schematic.find(id);
        auto was = pcbSync.parts.find(id);
        if (was == pcbSync.parts.end()) {
            add("component", "add", part[0], "Footprint " + part[1] + (c && c->pcb.placed ? "" : ", placed on the board"),
                "component:" + std::to_string(id));
        } else if (was->second != part) {
            std::string what;
            if (was->second[0] != part[0]) what += "designator " + was->second[0] + " → " + part[0];
            if (was->second[1] != part[1]) what += std::string(what.empty() ? "" : "; ") + "footprint " + was->second[1] + " → " + part[1];
            if (was->second[2] != part[2]) what += std::string(what.empty() ? "" : "; ") + "value " + was->second[2] + " → " + part[2];
            add("component", "change", part[0], what, "component:" + std::to_string(id));
        }
    }
    for (const auto& [id, part] : pcbSync.parts)
        if (!now.parts.count(id))
            add("component", "remove", part[0], "Footprint " + part[1] + " and its copper leave the board",
                "component:" + std::to_string(id));
    for (const auto& [name, pins] : now.nets) {
        auto was = pcbSync.nets.find(name);
        if (was == pcbSync.nets.end()) {
            add("net", "add", name, pins, "net:" + name);
        } else if (was->second != pins) {
            const auto a = words(was->second), b = words(pins);
            std::string what;
            for (const auto& w : b)
                if (std::find(a.begin(), a.end(), w) == a.end()) what += (what.empty() ? "+" : " +") + w;
            for (const auto& w : a)
                if (std::find(b.begin(), b.end(), w) == b.end()) what += (what.empty() ? "-" : " -") + w;
            add("net", "change", name, what, "net:" + name);
        }
    }
    for (const auto& [name, pins] : pcbSync.nets)
        if (!now.nets.count(name)) add("net", "remove", name, pins, "net:" + name);
    // Copper pours on nets the schematic no longer has.
    for (size_t z = 0; z < pcb.zones.size(); ++z) {
        const std::string& net = pcb.zones[z].net;
        const bool known = std::any_of(schematic.nets().begin(), schematic.nets().end(), [&](const Net& n) { return n.name == net; });
        if (!known) add("zone", "remove", net, "Copper pour on a net the schematic no longer has", "zone:" + std::to_string(z));
    }
    // Net rules the schematic's directives give that the board does not have (or no longer should).
    if (!schematic.directives().empty() || !pcb.settings.schematicRuleNets.empty()) {
        Project probe = *this;
        probe.applySchematicRules();
        const auto& want = probe.pcb.settings;
        const auto& have = pcb.settings;
        std::set<std::string> names;
        for (const auto* m : {&want.netWidths, &have.netWidths, &want.netClearances, &have.netClearances})
            for (const auto& [n, v] : *m) names.insert(n);
        for (const auto& n : names) {
            auto value = [&](const std::map<std::string, double>& m) {
                auto it = m.find(n);
                return it == m.end() ? -1.0 : it->second;
            };
            const double ww = value(want.netWidths), hw = value(have.netWidths);
            const double wc = value(want.netClearances), hc = value(have.netClearances);
            if (ww == hw && wc == hc) continue;
            const std::string detail = "width " + (ww < 0 ? std::string("board default") : mm(ww)) + ", clearance " +
                                       (wc < 0 ? std::string("board default") : mm(wc));
            const char* action = ww < 0 && wc < 0 ? "remove" : (hw < 0 && hc < 0 ? "add" : "change");
            add("rule", action, n, detail, "rule:" + n);
        }
    }
    return out;
}

int Project::applyPcbEco(const std::vector<std::string>& keys, std::vector<std::string>* report) {
    const auto changes = pcbEcoPreview();
    const std::set<std::string> chosen(keys.begin(), keys.end());
    auto picked = [&](const PcbEcoChange& e) { return chosen.empty() || chosen.count(e.key) > 0; };
    const PcbSyncBaseline now = currentSync();
    int done = 0;
    // Added footprints that are not on the board yet are placed (only the chosen ones).
    std::vector<int> toPlace;
    for (const auto& e : changes)
        if (picked(e) && e.section == "component" && e.action == "add") {
            const int id = std::stoi(e.key.substr(10));
            if (const Component* c = schematic.find(id); c && !c->pcb.placed) toPlace.push_back(id);
        }
    if (!toPlace.empty()) {
        std::vector<int> held;
        for (auto& c : schematic.mutableComponents())
            if (c.hasFootprint() && !c.pcb.placed && std::find(toPlace.begin(), toPlace.end(), c.id) == toPlace.end()) {
                c.pcb.placed = true;  // not chosen: kept out of this placement run
                held.push_back(c.id);
            }
        pcb.autoPlace(schematic, false);
        for (int id : held)
            if (Component* c = schematic.find(id)) c->pcb.placed = false;
    }
    std::vector<size_t> zonesGone;
    bool rules = false;
    for (const auto& e : changes) {
        if (!picked(e)) continue;
        if (e.section == "component") {
            const int id = std::stoi(e.key.substr(10));
            auto it = now.parts.find(id);
            if (it == now.parts.end()) pcbSync.parts.erase(id);
            else pcbSync.parts[id] = it->second;
        } else if (e.section == "net") {
            auto it = now.nets.find(e.object);
            if (it == now.nets.end()) pcbSync.nets.erase(e.object);
            else pcbSync.nets[e.object] = it->second;
        } else if (e.section == "zone") {
            zonesGone.push_back(static_cast<size_t>(std::stoul(e.key.substr(5))));
        } else if (e.section == "rule") {
            rules = true;
        }
        ++done;
        if (report) report->push_back(e.action + " " + e.section + " " + e.object + (e.detail.empty() ? "" : ": " + e.detail));
    }
    std::sort(zonesGone.rbegin(), zonesGone.rend());
    for (size_t z : zonesGone)
        if (z < pcb.zones.size()) pcb.zones.erase(pcb.zones.begin() + static_cast<std::ptrdiff_t>(z));
    if (rules) applySchematicRules();
    if (done > 0) pcb.pruneStaleRouting(schematic);
    return done;
}

}  // namespace sieda
