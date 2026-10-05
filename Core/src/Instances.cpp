// SiEDA Core — repeated (multi-instance) hierarchical sheets.
//
// A block drawn once on a definition sheet is used several times: every instance is a sheet of its own (a channel)
// holding copies of the definition's components and wires. The copies are real components with their own ids,
// designators, nets (local labels and ports are sheet-scoped), footprint placements and variant settings, so the
// stored design stays one flat list that the netlist, simulation, BOM and PCB read exactly as before. Edits made to
// a copy go to the definition, and syncInstances() brings every instance back in line after each edit.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>
#include <unordered_map>

#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
constexpr int kMaxInstances = 64;

std::string trimmedText(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

/// "A" … "Z", then "AA", "AB", …
std::string channelLabel(int index) {
    std::string out;
    for (int n = index + 1; n > 0; n = (n - 1) / 26) out.insert(out.begin(), static_cast<char>('A' + (n - 1) % 26));
    return out;
}

bool validChannel(const std::string& c) {
    if (c.empty() || c.size() > 16) return false;
    return std::all_of(c.begin(), c.end(), [](char ch) {
        return std::isalnum(static_cast<unsigned char>(ch)) != 0 || ch == '_' || ch == '-';
    });
}

/// Splits "R12" into ("R", 12); false when the designator does not end in a number (or the number is too long).
bool splitDesignator(const std::string& ref, std::string& prefix, int& number) {
    size_t k = ref.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(ref[k - 1]))) --k;
    if (k == ref.size() || ref.size() - k > 6) return false;
    prefix = ref.substr(0, k);
    number = std::stoi(ref.substr(k));
    return true;
}

std::string refPrefixOf(const Component& c) {
    const std::string& p = c.def().refPrefix;
    return p.empty() ? std::string("U") : p;
}
}  // namespace

const char* instanceRefsName(InstanceRefs r) { return r == InstanceRefs::Suffix ? "suffix" : "sheet"; }

bool instanceRefsFromName(const std::string& name, InstanceRefs* out) {
    InstanceRefs r;
    if (name == "sheet") r = InstanceRefs::SheetNumber;
    else if (name == "suffix") r = InstanceRefs::Suffix;
    else return false;
    if (out) *out = r;
    return true;
}

bool Schematic::hasInstances() const {
    return std::any_of(sheets_.begin(), sheets_.end(), [](const Sheet& s) { return s.instanceOf != 0; });
}

int Schematic::definitionSheet(int sheet) const {
    const Sheet* s = findSheet(sheet);
    if (s && s->instanceOf != 0 && findSheet(s->instanceOf)) return s->instanceOf;
    return sheet;
}

std::vector<int> Schematic::sheetInstances(int sheet) const {
    const int def = definitionSheet(sheet);
    std::vector<int> out{def};
    for (const auto& s : sheets_)
        if (s.instanceOf == def && s.id != def) out.push_back(s.id);
    if (out.size() == 1) out[0] = sheet;
    return out;
}

bool Schematic::isRepeated(int sheet) const {
    if (!hasInstances()) return false;
    const Sheet* s = findSheet(sheet);
    if (!s) return false;
    if (s->instanceOf != 0) return true;
    return std::any_of(sheets_.begin(), sheets_.end(), [&](const Sheet& o) { return o.instanceOf == sheet; });
}

int Schematic::masterOf(int id) const {
    const Component* c = find(id);
    if (c && c->instanceOf > 0 && find(c->instanceOf)) return c->instanceOf;
    return id;
}

int Schematic::copyOn(int masterId, int sheet) const {
    const Component* m = find(masterId);
    if (!m) return -1;
    if (m->sheet == sheet) return masterId;
    for (const auto& c : components_)
        if (c.instanceOf == masterId && c.sheet == sheet) return c.id;
    return -1;
}

int Schematic::masterWireOf(int wireId) const {
    for (const auto& w : wires_)
        if (w.id == wireId) {
            if (w.instanceOf > 0)
                for (const auto& m : wires_)
                    if (m.id == w.instanceOf) return m.id;
            return wireId;
        }
    return wireId;
}

int Schematic::copyWireOn(int masterWire, int sheet) const {
    for (const auto& w : wires_) {
        const bool candidate = w.id == masterWire || w.instanceOf == masterWire;
        if (!candidate) continue;
        const Component* a = find(w.a.component);
        if (a && a->sheet == sheet) return w.id;
    }
    return -1;
}

int Schematic::repeatSheet(int sheet, int count) {
    const Sheet* def = findSheet(sheet);
    if (!def || def->instanceOf != 0 || count < 1 || count > kMaxInstances) return -1;
    // Nested hierarchy inside a repeated block is not supported: the block must be a leaf.
    for (const auto& s : sheets_)
        if (s.parent == sheet) return -1;
    for (const auto& c : components_)
        if (c.sheet == sheet && c.kind == ComponentKind::NetLabel && c.scope == LabelScope::SheetEntry) return -1;
    std::vector<int> group = sheetInstances(sheet);
    if (group.front() != sheet) return -1;
    // Instances beyond `count` go, last first (their components, wires and the sheet entries leading into them).
    while (static_cast<int>(group.size()) > count) {
        const int victim = group.back();
        group.pop_back();
        std::set<int> gone;
        for (const auto& c : components_)
            if (c.sheet == victim ||
                (c.kind == ComponentKind::NetLabel && c.scope == LabelScope::SheetEntry && c.targetSheet == victim))
                gone.insert(c.id);
        components_.erase(std::remove_if(components_.begin(), components_.end(),
                                         [&](const Component& c) { return gone.count(c.id) > 0; }),
                          components_.end());
        wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                    [&](const Wire& w) { return gone.count(w.a.component) || gone.count(w.b.component); }),
                     wires_.end());
        const int index = sheetIndex(victim);
        sheets_.erase(sheets_.begin() + index);
        if (activeSheet_ == victim) activeSheet_ = sheet;
        repairBusLinks();
    }
    if (count > static_cast<int>(group.size())) {
        std::set<std::string> channels, names;
        for (int id : group)
            if (const Sheet* s = findSheet(id)) channels.insert(s->channel);
        for (const auto& s : sheets_) names.insert(s.name);
        int next = 0;
        auto freshChannel = [&] {
            std::string label;
            do label = channelLabel(next++);
            while (channels.count(label));
            channels.insert(label);
            return label;
        };
        for (auto& s : sheets_)
            if (s.id == sheet && s.channel.empty()) s.channel = freshChannel();
        const Sheet base = *findSheet(sheet);
        int insertAt = sheetIndex(group.back()) + 1;
        while (static_cast<int>(group.size()) < count) {
            Sheet s;
            s.id = nextSheetId_++;
            s.parent = base.parent;
            s.instanceOf = sheet;
            s.channel = freshChannel();
            std::string name = base.name + " [" + s.channel + "]";
            for (int k = 2; names.count(name); ++k) name = base.name + " [" + s.channel + "] " + std::to_string(k);
            names.insert(name);
            s.name = name;
            sheets_.insert(sheets_.begin() + insertAt++, s);
            group.push_back(s.id);
        }
    }
    syncInstances();
    invalidate();
    return static_cast<int>(group.size());
}

bool Schematic::setInstanceRefs(int sheet, InstanceRefs refs) {
    if (!findSheet(sheet)) return false;
    const int def = definitionSheet(sheet);
    for (auto& s : sheets_)
        if (s.id == def) s.refs = refs;
    syncInstances();
    return true;
}

bool Schematic::setSheetChannel(int sheet, const std::string& raw) {
    const std::string channel = trimmedText(raw);
    if (!findSheet(sheet) || !validChannel(channel)) return false;
    for (int id : sheetInstances(sheet))
        if (id != sheet)
            if (const Sheet* s = findSheet(id); s && s->channel == channel) return false;
    for (auto& s : sheets_)
        if (s.id == sheet) s.channel = channel;
    syncInstances();
    return true;
}

std::string Schematic::channelRef(const std::string& logical, int sheet, int step) const {
    const Sheet* s = findSheet(sheet);
    const Sheet* def = findSheet(definitionSheet(sheet));
    const std::string channel = s && !s->channel.empty() ? s->channel : std::to_string(sheet);
    std::string prefix;
    int number = 0;
    if (def && def->refs == InstanceRefs::SheetNumber && step > 0 && splitDesignator(logical, prefix, number) &&
        number < step) {
        const int block = std::max(0, sheetIndex(sheet)) + 1;
        return prefix + std::to_string(static_cast<long long>(block) * step + number);
    }
    return logical + "_" + channel;
}

void Schematic::syncInstances() {
    repairBusLinks();
    // Sheets: an instance points at a definition that exists and is not an instance itself.
    std::map<int, std::vector<int>> groups;  // definition → its instance sheets, in sheet order
    std::map<int, int> defOf;                // instance sheet → definition
    for (auto& s : sheets_) {
        if (s.instanceOf == 0) continue;
        const Sheet* d = findSheet(s.instanceOf);
        if (!d || d->instanceOf != 0 || s.instanceOf == s.id) {
            s.instanceOf = 0;
            continue;
        }
        groups[s.instanceOf].push_back(s.id);
        defOf[s.id] = s.instanceOf;
    }
    // Channel labels: present, valid and unique within each block.
    for (const auto& [def, instances] : groups) {
        std::vector<int> members{def};
        members.insert(members.end(), instances.begin(), instances.end());
        std::set<std::string> seen;
        std::vector<int> fix;
        for (int id : members) {
            const Sheet* s = findSheet(id);
            if (!validChannel(s->channel) || !seen.insert(s->channel).second) fix.push_back(id);
        }
        int next = 0;
        for (int id : fix)
            for (auto& s : sheets_)
                if (s.id == id) {
                    std::string label;
                    do label = channelLabel(next++);
                    while (seen.count(label));
                    seen.insert(label);
                    s.channel = label;
                }
    }

    bool changed = false;
    // A part that is no longer on a repeated definition gets its block designator back (when it is free).
    {
        std::set<std::string> refs;
        for (const auto& c : components_) refs.insert(c.ref);
        for (auto& c : components_) {
            if (c.logicalRef.empty() || groups.count(c.sheet)) continue;
            if (!refs.count(c.logicalRef)) {
                refs.erase(c.ref);
                c.ref = c.logicalRef;
                refs.insert(c.ref);
            }
            c.logicalRef.clear();
            changed = true;
        }
    }
    for (auto& c : components_)
        if (c.instanceOf != 0 && !defOf.count(c.sheet)) {
            c.instanceOf = 0;
            changed = true;
        }
    if (groups.empty()) {
        for (auto& w : wires_) w.instanceOf = 0;
        for (auto& b : buses_) b.instanceOf = 0;
        if (changed) invalidate();
        return;
    }

    std::unordered_map<int, size_t> at;
    auto reindex = [&] {
        at.clear();
        for (size_t i = 0; i < components_.size(); ++i) at[components_[i].id] = i;
    };
    reindex();
    auto comp = [&](int id) -> Component* {
        auto it = at.find(id);
        return it == at.end() ? nullptr : &components_[it->second];
    };

    // Parts drawn directly on an instance sheet (a hand-edited file) join the definition, so every channel gets them.
    for (auto& c : components_) {
        auto d = defOf.find(c.sheet);
        if (d == defOf.end() || c.instanceOf != 0) continue;
        c.sheet = d->second;
        changed = true;
    }
    // Copies whose original is gone or no longer on the definition, and second copies of one original, go.
    {
        std::set<std::pair<int, int>> seen;  // (original, sheet)
        std::set<int> gone;
        for (const auto& c : components_) {
            if (c.instanceOf == 0) continue;
            const Component* m = comp(c.instanceOf);
            const auto d = defOf.find(c.sheet);
            if (!m || m->instanceOf != 0 || d == defOf.end() || m->sheet != d->second || m->kind != c.kind ||
                !seen.insert({c.instanceOf, c.sheet}).second)
                gone.insert(c.id);
        }
        if (!gone.empty()) {
            components_.erase(std::remove_if(components_.begin(), components_.end(),
                                             [&](const Component& c) { return gone.count(c.id) > 0; }),
                              components_.end());
            wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                        [&](const Wire& w) { return gone.count(w.a.component) || gone.count(w.b.component); }),
                         wires_.end());
            reindex();
            changed = true;
        }
    }
    // Wires: a wire of the definition joins two parts on it; any other wire touching an instance either copies a
    // definition wire (kept, re-targeted below) or was drawn between copies — it becomes a definition wire.
    {
        std::set<int> masterWires;
        for (const auto& w : wires_) {
            const Component* a = comp(w.a.component);
            const Component* b = comp(w.b.component);
            if (w.instanceOf == 0 && a && b && a->sheet == b->sheet && groups.count(a->sheet)) masterWires.insert(w.id);
        }
        std::set<std::pair<std::pair<int, int>, std::pair<int, int>>> existing;
        for (const auto& w : wires_)
            if (masterWires.count(w.id))
                existing.insert({{w.a.component, w.a.pin}, {w.b.component, w.b.pin}});
        std::vector<Wire> kept;
        kept.reserve(wires_.size());
        std::set<std::pair<int, int>> copied;  // (original wire, instance sheet)
        for (Wire w : wires_) {
            const Component* a = comp(w.a.component);
            const Component* b = comp(w.b.component);
            if (!a || !b) continue;
            const bool onInstance = defOf.count(a->sheet) || defOf.count(b->sheet);
            if (!onInstance) {
                if (w.instanceOf != 0) {
                    w.instanceOf = 0;
                    changed = true;
                }
                kept.push_back(w);
                continue;
            }
            if (w.instanceOf != 0) {
                if (masterWires.count(w.instanceOf) && a->sheet == b->sheet && copied.insert({w.instanceOf, a->sheet}).second)
                    kept.push_back(w);
                else
                    changed = true;
                continue;
            }
            // Drawn between copies: the same wire on the definition.
            changed = true;
            Wire m = w;
            m.a.component = a->instanceOf ? a->instanceOf : a->id;
            m.b.component = b->instanceOf ? b->instanceOf : b->id;
            const Component* ma = comp(m.a.component);
            const Component* mb = comp(m.b.component);
            if (!ma || !mb || ma->sheet != mb->sheet || !groups.count(ma->sheet) || m.a == m.b) continue;
            const auto key = std::make_pair(std::make_pair(m.a.component, m.a.pin), std::make_pair(m.b.component, m.b.pin));
            const auto rev = std::make_pair(key.second, key.first);
            if (existing.count(key) || existing.count(rev)) continue;
            existing.insert(key);
            kept.push_back(m);
        }
        wires_ = std::move(kept);
    }

    // Buses: those drawn on an instance join the definition; copies of buses that are gone go.
    {
        std::set<std::pair<int, int>> seen;
        std::vector<Bus> kept;
        for (Bus b : buses_) {
            const auto d = defOf.find(b.sheet);
            if (d == defOf.end()) {
                b.instanceOf = 0;
                kept.push_back(b);
                continue;
            }
            if (b.instanceOf == 0) {
                b.sheet = d->second;  // drawn on a channel: becomes the block's
                for (auto& c : components_)
                    if (c.bus == b.id) c.bus = 0;  // its entries there were copies-to-be; re-ripped on the block
                kept.push_back(b);
                changed = true;
                continue;
            }
            const Bus* m = findBus(b.instanceOf);
            if (!m || m->instanceOf != 0 || m->sheet != d->second || !seen.insert({b.instanceOf, b.sheet}).second) {
                changed = true;
                continue;
            }
            kept.push_back(b);
        }
        buses_ = std::move(kept);
        for (auto& [def, instances] : groups)
            for (int sheet : instances) {
                std::vector<Bus> originals;
                for (const auto& b : buses_)
                    if (b.sheet == def && b.instanceOf == 0) originals.push_back(b);
                for (const auto& m : originals) {
                    auto it = std::find_if(buses_.begin(), buses_.end(),
                                           [&](const Bus& b) { return b.instanceOf == m.id && b.sheet == sheet; });
                    if (it == buses_.end()) {
                        Bus c = m;
                        c.id = nextBusId_++;
                        c.sheet = sheet;
                        c.instanceOf = m.id;
                        buses_.push_back(c);
                        changed = true;
                    } else if (it->name != m.name || it->points.size() != m.points.size() ||
                               !std::equal(it->points.begin(), it->points.end(), m.points.begin(),
                                           [](Vec2 a, Vec2 b) { return a.x == b.x && a.y == b.y; })) {
                        it->name = m.name;
                        it->points = m.points;
                        changed = true;
                    }
                }
            }
    }

    for (auto& [def, instances] : groups) {
        // Block designators: each part of the definition keeps a unique one; new parts take their own designator
        // when it is free in the block, else the next free number of their prefix.
        std::vector<size_t> masters;
        for (size_t i = 0; i < components_.size(); ++i)
            if (components_[i].sheet == def) masters.push_back(i);
        {
            std::set<std::string> used;
            std::vector<size_t> pending;
            for (size_t i : masters) {
                Component& m = components_[i];
                if (isNetSymbolKind(m.kind)) {
                    if (!m.logicalRef.empty()) {
                        m.logicalRef.clear();
                        changed = true;
                    }
                    continue;
                }
                if (!m.logicalRef.empty() && used.insert(m.logicalRef).second) continue;
                pending.push_back(i);
            }
            for (size_t i : pending) {
                Component& m = components_[i];
                std::string logical = m.ref;
                if (logical.empty() || logical.back() == '?' || used.count(logical)) {
                    const std::string prefix = refPrefixOf(m);
                    for (int n = 1;; ++n)
                        if (!used.count(logical = prefix + std::to_string(n))) break;
                }
                m.logicalRef = logical;
                used.insert(logical);
                changed = true;
            }
        }
        int step = 100;
        for (size_t i : masters) {
            std::string prefix;
            int number = 0;
            if (splitDesignator(components_[i].logicalRef, prefix, number))
                while (number >= step && step < 1000000) step *= 10;
        }

        // Copies: one per original on every instance, carrying everything but identity, placement and sourcing.
        std::map<std::pair<int, int>, int> copyOf;  // (original, instance sheet) → copy id
        for (const auto& c : components_)
            if (c.instanceOf != 0) copyOf[{c.instanceOf, c.sheet}] = c.id;
        std::vector<int> masterIds;
        for (size_t i : masters) masterIds.push_back(components_[i].id);
        for (int sheet : instances) {
            for (int mid : masterIds) {
                auto it = copyOf.find({mid, sheet});
                if (it == copyOf.end()) {
                    Component c = *comp(mid);
                    c.id = nextComponentId_++;
                    c.sheet = sheet;
                    c.instanceOf = mid;
                    c.logicalRef.clear();
                    c.pcb = PcbPlacement{};
                    components_.push_back(c);
                    at[c.id] = components_.size() - 1;
                    copyOf[{mid, sheet}] = c.id;
                    changed = true;
                    continue;
                }
                const Component& m = *comp(mid);
                Component& c = *comp(it->second);
                c.kind = m.kind;
                c.customPart = m.customPart;
                c.value = m.value;
                c.position = m.position;
                c.rotation = m.rotation;
                c.noConnect = m.noConnect;
                c.firmware = m.firmware;
                c.firmwareName = m.firmwareName;
                c.clockHz = m.clockHz;
                c.package = m.package;
                c.scope = m.scope;
                c.targetSheet = m.targetSheet;
            }
        }
        // Bus entries of the copies belong to the copies of their bus.
        for (int sheet : instances)
            for (int mid : masterIds) {
                const int masterBus = comp(mid)->bus;
                Component& c = *comp(copyOf[{mid, sheet}]);
                int bus = 0;
                if (masterBus != 0)
                    for (const auto& b : buses_)
                        if (b.instanceOf == masterBus && b.sheet == sheet) bus = b.id;
                if (c.bus != bus) {
                    c.bus = bus;
                    changed = true;
                }
            }
        // Designators for every channel, the definition's own included.
        for (int mid : masterIds) {
            Component& m = *comp(mid);
            const bool part = !isNetSymbolKind(m.kind);
            const std::string logical = m.logicalRef;
            const std::string netSymbolRef = m.ref;
            if (part) {
                const std::string ref = channelRef(logical, def, step);
                if (m.ref != ref) {
                    m.ref = ref;
                    changed = true;
                }
            }
            for (int sheet : instances) {
                Component& c = *comp(copyOf[{mid, sheet}]);
                const std::string ref = part ? channelRef(logical, sheet, step) : netSymbolRef;
                if (c.ref != ref) {
                    c.ref = ref;
                    changed = true;
                }
            }
        }
        // Wires: the definition's wires on every instance, between the copies of their ends.
        std::map<std::pair<int, int>, size_t> wireCopy;  // (original wire, instance sheet) → index in wires_
        for (size_t i = 0; i < wires_.size(); ++i)
            if (wires_[i].instanceOf != 0)
                if (const Component* a = comp(wires_[i].a.component)) wireCopy[{wires_[i].instanceOf, a->sheet}] = i;
        std::vector<Wire> originals;
        for (const auto& w : wires_) {
            const Component* a = comp(w.a.component);
            if (w.instanceOf == 0 && a && a->sheet == def) originals.push_back(w);
        }
        for (int sheet : instances)
            for (const auto& w : originals) {
                const PinRef a{copyOf[{w.a.component, sheet}], w.a.pin}, b{copyOf[{w.b.component, sheet}], w.b.pin};
                auto it = wireCopy.find({w.id, sheet});
                if (it == wireCopy.end()) {
                    Wire c;
                    c.id = nextWireId_++;
                    c.a = a;
                    c.b = b;
                    c.instanceOf = w.id;
                    wires_.push_back(c);
                    changed = true;
                } else {
                    Wire& c = wires_[it->second];
                    if (!(c.a == a) || !(c.b == b)) {
                        c.a = a;
                        c.b = b;
                        changed = true;
                    }
                }
            }
    }
    if (changed) invalidate();
}

void Schematic::annotateBlocks(const AnnotateOptions& o) {
    std::set<int> defs;
    for (const auto& s : sheets_)
        if (s.instanceOf != 0) defs.insert(s.instanceOf);
    for (int def : defs) {
        struct Item {
            size_t at;
            std::string prefix;
            double major, minor;
        };
        std::vector<Item> items;
        for (size_t i = 0; i < components_.size(); ++i) {
            const Component& c = components_[i];
            if (c.sheet != def || isNetSymbolKind(c.kind)) continue;
            const double x = std::round(c.position.x / 10.0), y = std::round(c.position.y / 10.0);
            items.push_back({i, refPrefixOf(c), o.byColumns ? x : y, o.byColumns ? y : x});
        }
        std::stable_sort(items.begin(), items.end(), [&](const Item& a, const Item& b) {
            if (a.major != b.major) return a.major < b.major;
            if (a.minor != b.minor) return a.minor < b.minor;
            return components_[a.at].id < components_[b.at].id;
        });
        std::set<std::string> used;
        std::vector<const Item*> pending;
        for (const auto& it : items) {
            const std::string& logical = components_[it.at].logicalRef;
            if (o.keepExisting && !logical.empty() && logical.back() != '?' && used.insert(logical).second) continue;
            pending.push_back(&it);
        }
        std::map<std::string, int> next;
        for (const Item* it : pending) {
            int& n = next.emplace(it->prefix, 1).first->second;
            while (used.count(it->prefix + std::to_string(n))) ++n;
            components_[it->at].logicalRef = it->prefix + std::to_string(n);
            used.insert(components_[it->at].logicalRef);
            ++n;
        }
    }
}

}  // namespace sieda
