// SiEDA Core — multi-sheet / hierarchical schematics, bus labels, annotation and the cross-sheet ERC rules.
//
// The schematic stays one flat list of components and wires (the flattened design that netlist, simulation and PCB
// read); every component carries the id of the sheet it is drawn on. Nets cross sheets only through global labels,
// ground symbols and hierarchical ports joined to sheet entries — wires never do.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
std::string trimmed(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool isDigits(const std::string& s) {
    return !s.empty() && s.size() <= 6 &&
           std::all_of(s.begin(), s.end(), [](char ch) { return std::isdigit(static_cast<unsigned char>(ch)) != 0; });
}

bool isGround(const Component& c) {
    if (c.kind == ComponentKind::Ground) return true;
    return c.kind == ComponentKind::NetLabel && (c.value == "GND" || c.value == "0" || c.value == "gnd" || c.value == "Gnd");
}
}  // namespace

const char* labelScopeName(LabelScope s) {
    switch (s) {
        case LabelScope::Local: return "local";
        case LabelScope::Port: return "port";
        case LabelScope::SheetEntry: return "entry";
        case LabelScope::Global: break;
    }
    return "global";
}

bool labelScopeFromName(const std::string& name, LabelScope* out) {
    static const std::pair<const char*, LabelScope> names[] = {
        {"global", LabelScope::Global}, {"local", LabelScope::Local}, {"port", LabelScope::Port}, {"entry", LabelScope::SheetEntry}};
    for (const auto& n : names)
        if (name == n.first) {
            if (out) *out = n.second;
            return true;
        }
    return false;
}

// ---------------------------------------------------------------- bus notation

std::vector<std::string> expandBus(const std::string& raw) {
    std::vector<std::string> parts;
    {
        std::string current;
        for (char ch : raw) {
            if (ch == ',') {
                parts.push_back(trimmed(current));
                current.clear();
            } else {
                current += ch;
            }
        }
        parts.push_back(trimmed(current));
    }
    std::vector<std::string> out;
    bool ranged = false;
    for (const auto& part : parts) {
        if (part.empty()) return {};
        const size_t open = part.find('[');
        if (open == std::string::npos) {
            if (part.find(']') != std::string::npos) return {};
            out.push_back(part);
            continue;
        }
        const size_t close = part.find(']', open);
        const size_t dots = part.find("..", open);
        if (open == 0 || close == std::string::npos || dots == std::string::npos || dots > close) return {};
        if (part.find('[', open + 1) != std::string::npos || part.find(']', close + 1) != std::string::npos) return {};
        const std::string from = part.substr(open + 1, dots - open - 1), to = part.substr(dots + 2, close - dots - 2);
        if (!isDigits(from) || !isDigits(to)) return {};
        const int a = std::stoi(from), b = std::stoi(to);
        if (std::abs(a - b) >= 1024) return {};
        const std::string prefix = part.substr(0, open), suffix = part.substr(close + 1);
        for (int i = a;; i += (a <= b ? 1 : -1)) {
            out.push_back(prefix + std::to_string(i) + suffix);
            if (i == b || out.size() > 1024) break;
        }
        ranged = true;
    }
    if ((!ranged && out.size() < 2) || out.size() > 1024) return {};
    return out;
}

// ---------------------------------------------------------------- sheets

const Sheet* Schematic::findSheet(int id) const {
    for (const auto& s : sheets_)
        if (s.id == id) return &s;
    return nullptr;
}

int Schematic::sheetIndex(int id) const {
    for (size_t i = 0; i < sheets_.size(); ++i)
        if (sheets_[i].id == id) return static_cast<int>(i);
    return -1;
}

int Schematic::sheetDepth(int id) const {
    int depth = 0;
    const Sheet* s = findSheet(id);
    while (s && s->parent != 0 && depth < static_cast<int>(sheets_.size())) {
        s = findSheet(s->parent);
        ++depth;
    }
    return depth;
}

int Schematic::addSheet(const std::string& rawName, int parent) {
    const std::string name = trimmed(rawName);
    if (name.empty() || (parent != 0 && !findSheet(parent))) return -1;
    if (parent != 0 && isRepeated(parent)) return -1;  // a repeated block has no child sheets
    for (const auto& s : sheets_)
        if (s.name == name) return -1;
    Sheet s;
    s.id = nextSheetId_++;
    s.name = name;
    s.parent = parent;
    sheets_.push_back(s);
    return s.id;
}

bool Schematic::renameSheet(int id, const std::string& rawName) {
    const std::string name = trimmed(rawName);
    if (name.empty() || !findSheet(id)) return false;
    for (auto& s : sheets_)
        if (s.name == name && s.id != id) return false;
    for (auto& s : sheets_)
        if (s.id == id) s.name = name;
    invalidate();  // qualified net names carry the sheet name
    return true;
}

bool Schematic::setSheetParent(int id, int parent) {
    if (!findSheet(id) || (parent != 0 && !findSheet(parent)) || parent == id) return false;
    if (parent != 0 && isRepeated(parent)) return false;  // a repeated block has no child sheets
    for (int up = parent, guard = 0; up != 0 && guard <= static_cast<int>(sheets_.size()); ++guard) {
        if (up == id) return false;  // would make a cycle
        const Sheet* s = findSheet(up);
        up = s ? s->parent : 0;
    }
    for (auto& s : sheets_)
        if (s.id == id) s.parent = parent;
    invalidate();
    return true;
}

bool Schematic::reorderSheet(int id, int index) {
    const int from = sheetIndex(id);
    if (from < 0 || index < 0 || index >= static_cast<int>(sheets_.size())) return false;
    Sheet s = sheets_[static_cast<size_t>(from)];
    sheets_.erase(sheets_.begin() + from);
    sheets_.insert(sheets_.begin() + index, s);
    edited();  // sheet-numbered designators of repeated sheets follow the order
    return true;
}

bool Schematic::removeSheet(int id, bool deleteContents) {
    const Sheet* sheet = findSheet(id);
    if (!sheet || sheets_.size() <= 1) return false;
    if (sheet->instanceOf == 0 && isRepeated(id)) {
        // The definition of a repeated sheet goes with all its instances.
        const std::vector<int> group = sheetInstances(id);
        if (sheets_.size() <= group.size()) return false;
        const bool any = std::any_of(components_.begin(), components_.end(), [&](const Component& c) {
            return std::find(group.begin(), group.end(), c.sheet) != group.end();
        });
        if (any && !deleteContents) return false;
        for (auto it = group.rbegin(); it != group.rend(); ++it)
            if (*it != id && !removeSheet(*it, true)) return false;
        sheet = findSheet(id);
        if (!sheet) return false;
    }
    const int parent = sheet->parent;
    const bool hasContents =
        std::any_of(components_.begin(), components_.end(), [&](const Component& c) { return c.sheet == id; });
    if (hasContents && !deleteContents) return false;
    std::set<int> gone;
    for (const auto& c : components_)
        if (c.sheet == id || (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::SheetEntry && c.targetSheet == id))
            gone.insert(c.id);
    components_.erase(std::remove_if(components_.begin(), components_.end(), [&](const Component& c) { return gone.count(c.id) > 0; }),
                      components_.end());
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [&](const Wire& w) { return gone.count(w.a.component) > 0 || gone.count(w.b.component) > 0; }),
                 wires_.end());
    for (auto& s : sheets_)
        if (s.parent == id) s.parent = parent;
    const int index = sheetIndex(id);
    sheets_.erase(sheets_.begin() + index);
    if (activeSheet_ == id) activeSheet_ = sheets_[static_cast<size_t>(std::max(0, index - 1))].id;
    invalidate();
    edited();
    return true;
}

bool Schematic::setActiveSheet(int id) {
    if (!findSheet(id)) return false;
    activeSheet_ = id;
    return true;
}

int Schematic::moveToSheet(const std::vector<int>& ids, int sheet) {
    if (!findSheet(sheet)) return 0;
    sheet = definitionSheet(sheet);  // parts moved onto a repeated sheet's instance join its definition
    std::set<int> moving;
    for (int id : ids)
        if (const Component* c = find(id); c && c->instanceOf == 0) moving.insert(id);  // copies stay with their block
    if (moving.empty()) return 0;
    // Junction clusters (junctions wired to each other) go along when every part they join is moving.
    std::map<int, std::vector<int>> neighbours;
    for (const auto& w : wires_) {
        neighbours[w.a.component].push_back(w.b.component);
        neighbours[w.b.component].push_back(w.a.component);
    }
    auto isJunction = [&](int id) {
        const Component* c = find(id);
        return c && c->kind == ComponentKind::Junction;
    };
    std::set<int> seen;
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::Junction || moving.count(c.id) || seen.count(c.id)) continue;
        std::vector<int> cluster, stack{c.id};
        std::set<int> parts;
        seen.insert(c.id);
        while (!stack.empty()) {
            int j = stack.back();
            stack.pop_back();
            cluster.push_back(j);
            for (int n : neighbours[j]) {
                if (isJunction(n)) {
                    if (seen.insert(n).second) stack.push_back(n);
                } else {
                    parts.insert(n);
                }
            }
        }
        if (!parts.empty() && std::all_of(parts.begin(), parts.end(), [&](int p) { return moving.count(p) > 0; }))
            moving.insert(cluster.begin(), cluster.end());
    }
    int moved = 0;
    for (auto& c : components_) {
        if (!moving.count(c.id) || c.sheet == sheet) continue;
        c.sheet = sheet;
        ++moved;
    }
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [&](const Wire& w) {
                                    const Component* a = find(w.a.component);
                                    const Component* b = find(w.b.component);
                                    return a && b && a->sheet != b->sheet;
                                }),
                 wires_.end());
    invalidate();
    edited();
    return moved;
}

std::vector<std::string> Schematic::sheetPorts(int sheet) const {
    std::set<std::string> names;
    for (const auto& c : components_)
        if (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::Port && c.sheet == sheet && !isGround(c))
            names.insert(c.value);
    return {names.begin(), names.end()};
}

int Schematic::placeSheetEntries(int child, Vec2 origin) {
    const Sheet* s = findSheet(child);
    if (!s || s->parent == 0 || !findSheet(s->parent)) return -1;
    const int parent = s->parent;
    std::set<std::string> present;
    bool any = false;
    double column = origin.x, lowest = origin.y;
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::NetLabel || c.scope != LabelScope::SheetEntry || c.targetSheet != child) continue;
        if (c.sheet != parent) continue;
        present.insert(c.value);
        if (!any) column = c.position.x;
        lowest = any ? std::max(lowest, c.position.y) : c.position.y;
        any = true;
    }
    Vec2 at = any ? Vec2{column, lowest + 20} : origin;
    int added = 0;
    const int saved = activeSheet_;
    activeSheet_ = parent;
    for (const auto& port : sheetPorts(child)) {
        if (present.count(port)) continue;
        int id = addComponent(ComponentKind::NetLabel, port, at);
        if (Component* c = find(id)) {
            c->scope = LabelScope::SheetEntry;
            c->targetSheet = child;
        }
        at.y += 20;
        ++added;
    }
    activeSheet_ = saved;
    invalidate();
    return added;
}

void Schematic::restoreSheets(const std::vector<Sheet>& sheets, int active) {
    std::vector<Sheet> valid;
    std::set<int> ids;
    std::set<std::string> names;
    for (Sheet s : sheets) {
        s.name = trimmed(s.name);
        if (s.id <= 0 || s.name.empty() || ids.count(s.id) || names.count(s.name)) continue;
        ids.insert(s.id);
        names.insert(s.name);
        valid.push_back(s);
    }
    if (valid.empty()) valid.push_back(Sheet{1, "Main", 0});
    for (auto& s : valid)
        if (s.parent == s.id || (s.parent != 0 && !ids.count(s.parent))) s.parent = 0;
    sheets_ = valid;
    // Break any parent cycle a hand-edited file may contain.
    for (auto& s : sheets_) {
        int up = s.parent;
        for (size_t guard = 0; up != 0 && guard <= sheets_.size(); ++guard) {
            if (up == s.id) {
                s.parent = 0;
                break;
            }
            const Sheet* p = findSheet(up);
            up = p ? p->parent : 0;
        }
    }
    // Repeated sheets: an instance points at an existing definition (not an instance itself), and a repeated block
    // has no child sheets (nested repetition is not supported): anything else is read as ordinary sheets.
    for (auto& s : sheets_) {
        if (s.instanceOf == 0) continue;
        const Sheet* d = findSheet(s.instanceOf);
        if (!d || d->instanceOf != 0 || s.instanceOf == s.id) s.instanceOf = 0;
    }
    for (auto& s : sheets_) {
        if (s.instanceOf == 0) continue;
        const int def = s.instanceOf;
        const bool nested = std::any_of(sheets_.begin(), sheets_.end(), [&](const Sheet& o) {
            if (o.parent == 0) return false;
            const Sheet* p = findSheet(o.parent);
            return p && (p->id == def || p->instanceOf == def);
        });
        if (nested)
            for (auto& o : sheets_)
                if (o.instanceOf == def) o.instanceOf = 0;
    }
    for (auto& s : sheets_) {
        s.channel = trimmed(s.channel);
        if (s.channel.size() > 16) s.channel.clear();
    }
    nextSheetId_ = 1;
    for (const auto& s : sheets_) nextSheetId_ = std::max(nextSheetId_, s.id + 1);
    activeSheet_ = findSheet(active) ? active : sheets_.front().id;
    for (auto& c : components_) {
        if (!findSheet(c.sheet)) c.sheet = sheets_.front().id;
        if (c.kind != ComponentKind::NetLabel) {
            c.scope = LabelScope::Global;
            c.targetSheet = 0;
        } else if (c.scope == LabelScope::SheetEntry && (!findSheet(c.targetSheet) || c.targetSheet == c.sheet)) {
            c.scope = LabelScope::Local;  // an entry into nothing keeps its net to its own sheet
            c.targetSheet = 0;
        } else if (c.scope != LabelScope::SheetEntry) {
            c.targetSheet = 0;
        }
    }
    invalidate();
}

// ---------------------------------------------------------------- annotation

std::vector<RefChange> Schematic::annotate(const AnnotateOptions& o) {
    // Repeated sheets are numbered inside the block (their logical designators); each channel's designators follow,
    // and the rest of the design is numbered around them.
    std::map<int, std::string> blockRefs;
    const bool blocks = hasInstances();
    if (blocks) {
        for (const auto& c : components_)
            if (!isNetSymbolKind(c.kind) && isRepeated(c.sheet)) blockRefs[c.id] = c.ref;
        annotateBlocks(o);
        syncInstances();
    }
    struct Item {
        size_t at;
        std::string prefix;
        int sheet;
        double major, minor;
    };
    std::vector<Item> items;
    for (size_t i = 0; i < components_.size(); ++i) {
        const Component& c = components_[i];
        if (isNetSymbolKind(c.kind) || (blocks && blockRefs.count(c.id))) continue;
        std::string prefix = c.def().refPrefix.empty() ? "U" : c.def().refPrefix;
        // Positions within one grid step count as one row (or column).
        const double x = std::round(c.position.x / 10.0), y = std::round(c.position.y / 10.0);
        items.push_back({i, prefix, std::max(0, sheetIndex(c.sheet)), o.byColumns ? x : y, o.byColumns ? y : x});
    }
    std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
        if (a.sheet != b.sheet) return a.sheet < b.sheet;
        if (a.major != b.major) return a.major < b.major;
        if (a.minor != b.minor) return a.minor < b.minor;
        return components_[a.at].id < components_[b.at].id;
    });
    // Sheet-based numbering: 100 per sheet, or 1000 when a sheet holds 100 or more parts of one prefix.
    std::map<std::string, int> step;
    {
        std::map<std::pair<std::string, int>, int> perSheet;
        for (const auto& it : items) {
            int& n = perSheet[{it.prefix, it.sheet}];
            ++n;
            int& s = step[it.prefix];
            s = std::max(s, n >= 100 ? 1000 : 100);
        }
    }
    auto firstNumber = [&](const Item& it) { return o.sheetNumbering ? (it.sheet + 1) * step[it.prefix] + 1 : 1; };

    std::vector<std::string> fresh(components_.size());
    std::set<std::string> used;
    for (const auto& [id, ref] : blockRefs)
        if (const Component* c = find(id)) used.insert(c->ref);  // the channels' designators are taken
    std::vector<const Item*> pending;
    if (o.keepExisting) {
        for (const auto& it : items) {
            const std::string& ref = components_[it.at].ref;
            if (!ref.empty() && ref.back() != '?' && used.insert(ref).second) fresh[it.at] = ref;
            else pending.push_back(&it);
        }
    } else {
        for (const auto& it : items) pending.push_back(&it);
    }
    std::map<std::pair<std::string, int>, int> next;  // (prefix, first number) → next candidate
    for (const Item* it : pending) {
        const int start = firstNumber(*it);
        int& n = next.emplace(std::make_pair(it->prefix, start), start).first->second;
        while (used.count(it->prefix + std::to_string(n))) ++n;
        fresh[it->at] = it->prefix + std::to_string(n);
        used.insert(fresh[it->at]);
        ++n;
    }
    std::vector<RefChange> changes;
    for (const auto& it : items) {
        Component& c = components_[it.at];
        if (fresh[it.at].empty() || fresh[it.at] == c.ref) continue;
        changes.push_back({c.id, c.ref, fresh[it.at]});
        c.ref = fresh[it.at];
    }
    for (const auto& [id, ref] : blockRefs)
        if (const Component* c = find(id); c && c->ref != ref) changes.push_back({id, ref, c->ref});
    return changes;
}

// ---------------------------------------------------------------- bus labels

int Schematic::addBusLabels(int componentId, const std::vector<int>& pins, const std::string& bus, LabelScope scope) {
    componentId = masterOf(componentId);  // on a repeated sheet's instance: labelled on the definition
    const Component* part = find(componentId);
    if (!part || scope == LabelScope::SheetEntry || isNetSymbolKind(part->kind)) return -1;
    const auto members = expandBus(bus);
    if (members.empty() || members.size() != pins.size()) return -1;
    const int pinCount = static_cast<int>(part->def().pins.size());
    for (int p : pins)
        if (p < 0 || p >= pinCount) return -1;
    const int sheet = part->sheet;
    const Vec2 centre = part->position;
    const int saved = activeSheet_;
    activeSheet_ = sheet;
    int added = 0;
    for (size_t i = 0; i < pins.size(); ++i) {
        const Vec2 at = pinPosition({componentId, pins[i]});
        const Vec2 d = at - centre;
        // Just outside the pin, the label's text pointing away from the part.
        int rotation = 0;
        Vec2 offset{30, 0};
        if (std::abs(d.x) >= std::abs(d.y)) {
            if (d.x < 0) {
                rotation = 180;
                offset = {-30, 0};
            }
        } else if (d.y < 0) {
            rotation = 270;
            offset = {0, -30};
        } else {
            rotation = 90;
            offset = {0, 30};
        }
        const int id = addComponent(ComponentKind::NetLabel, members[i], at + offset, rotation);
        if (Component* label = find(id)) label->scope = scope;
        connect({id, 0}, {componentId, pins[i]});
        ++added;
    }
    activeSheet_ = saved;
    invalidate();
    return added;
}

// ---------------------------------------------------------------- cross-sheet ERC

void Schematic::hierarchyERC(std::vector<RuleViolation>& out) const {
    auto add = [&](Severity s, const std::string& code, const std::string& msg, const Component& at) {
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
    auto sheetName = [&](int id) {
        const Sheet* s = findSheet(id);
        return s ? s->name : "sheet " + std::to_string(id);
    };

    // Wires between sheets (only a hand-edited file can hold one): the connection is not drawn anywhere.
    for (const auto& w : wires_) {
        const Component* a = find(w.a.component);
        const Component* b = find(w.b.component);
        if (a && b && a->sheet != b->sheet)
            add(Severity::Error, "ERC_CROSS_SHEET_WIRE",
                "A wire joins " + a->ref + " on " + sheetName(a->sheet) + " to " + b->ref + " on " + sheetName(b->sheet) +
                    ": use a global label or a hierarchical port instead.", *a);
    }

    std::map<std::string, std::set<int>> globalSheets, localSheets;
    std::map<std::string, const Component*> firstGlobal, firstLocal;
    std::set<std::pair<int, std::string>> entries;  // (child sheet, name) with an entry on the child's parent
    std::set<std::pair<int, std::string>> ports;    // (sheet, name) of every hierarchical port
    for (const auto& c : components_)
        if (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::Port) ports.insert({c.sheet, c.value});
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::NetLabel || isGround(c)) continue;
        if (!expandBus(c.value).empty())
            add(Severity::Warning, "ERC_BUS_LABEL", "Label " + c.value + " names a bus but joins a single net — place " +
                    "bus labels on the member pins (one label per member) instead.", c);
        switch (c.scope) {
            case LabelScope::Global:
                globalSheets[c.value].insert(c.sheet);
                firstGlobal.emplace(c.value, &c);
                break;
            case LabelScope::Local:
                localSheets[c.value].insert(c.sheet);
                firstLocal.emplace(c.value, &c);
                break;
            case LabelScope::SheetEntry: {
                const Sheet* target = findSheet(c.targetSheet);
                if (!target) {
                    add(Severity::Error, "ERC_SHEET_ENTRY_NO_SHEET", "Sheet entry " + c.value + " points to a sheet that does not exist.", c);
                    break;
                }
                if (target->parent != c.sheet)
                    add(Severity::Warning, "ERC_SHEET_ENTRY_PLACEMENT",
                        "Sheet entry " + c.value + " for " + target->name + " is on " + sheetName(c.sheet) +
                            ", not on its parent sheet " + (target->parent ? sheetName(target->parent) : std::string("(none: top level)")) + ".", c);
                if (!ports.count({c.targetSheet, c.value}))
                    add(Severity::Error, "ERC_SHEET_ENTRY_NO_PORT",
                        "Sheet entry " + c.value + " has no matching port on sheet " + target->name + ".", c);
                if (target->parent == c.sheet) entries.insert({c.targetSheet, c.value});
                break;
            }
            case LabelScope::Port: break;
        }
    }
    std::set<std::pair<int, std::string>> reported;
    for (const auto& c : components_) {
        if (c.kind != ComponentKind::NetLabel || c.scope != LabelScope::Port || isGround(c)) continue;
        if (!reported.insert({c.sheet, c.value}).second) continue;
        const Sheet* s = findSheet(c.sheet);
        if (!s || s->parent == 0)
            add(Severity::Warning, "ERC_PORT_NO_PARENT",
                "Port " + c.value + " is on top-level sheet " + sheetName(c.sheet) +
                    ": nothing connects to it from above. Make it a global or local label, or give the sheet a parent.", c);
        else if (!entries.count({c.sheet, c.value}))
            add(Severity::Warning, "ERC_PORT_UNUSED",
                "Port " + c.value + " on " + s->name + " has no sheet entry on " + sheetName(s->parent) +
                    " — place the sheet symbol's entries and wire them.", c);
    }
    for (const auto& [name, sheets] : localSheets) {
        if (sheets.size() < 2) continue;
        // The channels of one repeated sheet are meant to keep their local nets apart.
        std::set<int> blocks;
        for (int id : sheets) blocks.insert(definitionSheet(id));
        if (blocks.size() < 2) continue;
        std::string list;
        for (int id : sheets) list += (list.empty() ? "" : ", ") + sheetName(id);
        const Component* at = firstLocal[name];
        add(Severity::Warning, "ERC_LOCAL_LABEL_SPLIT",
            "Local label " + name + " is used on sheets " + list +
                "; these are separate nets. Use a global label if they should connect.", *at);
    }
    // A signal's global label inside a repeated sheet joins every channel into one net — almost always a mistake.
    {
        std::set<std::string> reportedBlock;
        for (const auto& c : components_) {
            if (c.kind != ComponentKind::NetLabel || c.scope != LabelScope::Global || isGround(c) || c.instanceOf != 0) continue;
            if (!isRepeated(c.sheet) || !reportedBlock.insert(c.value).second) continue;
            const int net = netOf({c.id, 0});
            if (net < 0 || netRole(net) != NetRole::Signal) continue;  // supply rails are shared on purpose
            const auto channels = sheetInstances(c.sheet);
            add(Severity::Warning, "ERC_GLOBAL_LABEL_IN_REPEAT",
                "Global label " + c.value + " is inside repeated sheet " + sheetName(c.sheet) + ": it joins all " +
                    std::to_string(channels.size()) + " channels into one net. Make it a port or a local label to keep " +
                    "each channel's own net.", c);
        }
    }
    if (sheets_.size() > 1) {
        for (const auto& [name, sheets] : globalSheets) {
            if (sheets.size() != 1) continue;
            const Component* at = firstGlobal[name];
            const int net = netOf({at->id, 0});
            if (net < 0 || netRole(net) != NetRole::Signal) continue;  // supply rails are global by nature
            add(Severity::Info, "ERC_GLOBAL_LABEL_ONE_SHEET",
                "Global label " + name + " is used only on " + sheetName(*sheets.begin()) +
                    "; a local label keeps the net private to that sheet.", *at);
        }
    }
}

}  // namespace sieda
