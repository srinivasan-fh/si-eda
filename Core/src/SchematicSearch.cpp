// SiEDA Core — find / replace across sheets and the net navigator (every place a net appears).
#include "sieda/SchematicSearch.hpp"

#include <algorithm>
#include <cctype>
#include <set>

namespace sieda {

namespace {
std::string lower(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return s;
}

bool matches(const std::string& field, const std::string& text, const SearchOptions& o) {
    if (text.empty()) return false;
    const std::string f = o.matchCase ? field : lower(field), t = o.matchCase ? text : lower(text);
    return o.wholeWord ? f == t : f.find(t) != std::string::npos;
}

/// `field` with every occurrence of `text` replaced (case-insensitively unless matchCase; the whole field when
/// wholeWord).
std::string replaced(const std::string& field, const std::string& text, const std::string& with, const SearchOptions& o) {
    if (o.wholeWord) return matches(field, text, o) ? with : field;
    const std::string f = o.matchCase ? field : lower(field), t = o.matchCase ? text : lower(text);
    std::string out;
    size_t from = 0;
    for (size_t at = f.find(t); at != std::string::npos; at = f.find(t, from)) {
        out += field.substr(from, at - from) + with;
        from = at + t.size();
    }
    return out + field.substr(from);
}

/// Components in reading order: sheet order, then top to bottom, left to right.
std::vector<const Component*> readingOrder(const Schematic& sch) {
    std::vector<const Component*> list;
    for (const auto& c : sch.components())
        if (c.kind != ComponentKind::Junction && !c.packageOnly) list.push_back(&c);
    std::stable_sort(list.begin(), list.end(), [&](const Component* a, const Component* b) {
        const int sa = sch.sheetIndex(a->sheet), sb = sch.sheetIndex(b->sheet);
        if (sa != sb) return sa < sb;
        if (a->position.y != b->position.y) return a->position.y < b->position.y;
        if (a->position.x != b->position.x) return a->position.x < b->position.x;
        return a->id < b->id;
    });
    return list;
}

bool isPart(const Component& c) { return !isNetSymbolKind(c.kind) || c.kind == ComponentKind::PartUnit; }
}  // namespace

std::vector<SearchHit> findInSchematic(const Schematic& sch, const std::string& text, const SearchOptions& o) {
    std::vector<SearchHit> hits;
    if (text.empty() || text.size() > 256) return hits;
    for (const Component* c : readingOrder(sch)) {
        auto hit = [&](const std::string& field, const std::string& value, int net, int pin) {
            SearchHit h;
            h.component = c->id;
            h.net = net;
            h.pin = pin;
            h.sheet = c->sheet;
            h.field = field;
            h.text = value;
            hits.push_back(h);
        };
        if (isPart(*c)) {
            const std::string ref = sch.displayRef(*c);
            if (o.refs && matches(ref, text, o)) hit("ref", ref, -1, -1);
            if (o.values && matches(c->value, text, o)) hit("value", c->value, -1, -1);
            if (o.pins) {
                const auto& pins = c->def().pins;
                for (size_t p = 0; p < pins.size(); ++p)
                    if (matches(pins[p].name, text, o))
                        hit("pin", ref + "." + pins[p].name, sch.netOf({c->id, static_cast<int>(p)}), static_cast<int>(p));
            }
        } else if (c->kind == ComponentKind::NetLabel && o.labels && matches(c->value, text, o)) {
            hit("label", c->value, sch.netOf({c->id, 0}), 0);
        }
    }
    if (o.nets)
        for (const auto& n : sch.nets())
            if (matches(n.name, text, o)) {
                SearchHit h;
                h.net = n.index;
                h.field = "net";
                h.text = n.name;
                const auto places = netPlaces(sch, n.index);
                if (!places.empty()) {
                    h.sheet = places.front().sheet;
                    h.component = places.front().component;
                }
                hits.push_back(h);
            }
    return hits;
}

int replaceInSchematic(Schematic& sch, const std::string& text, const std::string& with, const SearchOptions& o) {
    if (text.empty() || text.size() > 256 || with.size() > 256) return 0;
    std::vector<std::pair<int, std::string>> edits;  // (component to edit, new value)
    std::set<int> seen;
    for (const auto& c : sch.components()) {
        if (c.kind == ComponentKind::Junction) continue;
        const bool label = c.kind == ComponentKind::NetLabel;
        const bool part = isPart(c) && !c.packageOnly;
        if ((label && !o.labels) || (part && !o.values) || (!label && !part)) continue;
        if (!matches(c.value, text, o)) continue;
        int target = sch.masterOf(c.id);  // a block part is edited once, on its definition
        if (const int pkg = sch.unitPackage(target); pkg > 0) target = pkg;  // a multi-unit part once, on its package
        if (!seen.insert(target).second) continue;
        const std::string value = replaced(c.value, text, with, o);
        if (value == c.value || (label && value.empty())) continue;
        edits.push_back({target, value});
    }
    int changed = 0;
    for (const auto& [id, value] : edits) changed += sch.setValue(id, value) ? 1 : 0;
    return changed;
}

std::vector<NetPlace> netPlaces(const Schematic& sch, int net) {
    std::vector<NetPlace> out;
    if (net < 0 || net >= static_cast<int>(sch.nets().size())) return out;
    for (const Component* c : readingOrder(sch)) {
        const auto& pins = c->def().pins;
        for (size_t p = 0; p < pins.size(); ++p) {
            if (sch.netOf({c->id, static_cast<int>(p)}) != net) continue;
            NetPlace place;
            place.component = c->id;
            place.pin = static_cast<int>(p);
            place.sheet = c->sheet;
            place.position = sch.pinPosition({c->id, static_cast<int>(p)});
            place.ref = sch.displayRef(*c);
            if (c->kind == ComponentKind::Ground) {
                place.kind = "ground";
                place.name = c->value;
            } else if (c->kind == ComponentKind::NetLabel) {
                place.name = c->value;
                if (c->bus != 0) place.kind = "bus";
                else if (c->scope == LabelScope::Global) place.kind = "global";
                else if (c->scope == LabelScope::Port) place.kind = "port";
                else if (c->scope == LabelScope::SheetEntry) place.kind = "entry";
                else place.kind = "label";
            } else {
                place.kind = "pin";
                place.name = pins[p].name;
            }
            out.push_back(place);
        }
    }
    return out;
}

}  // namespace sieda
