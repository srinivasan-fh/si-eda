// SiEDA Core — schematic editing productivity: align / distribute, copy and paste (smart paste: a paste array with
// designators numbered on and net labels counted up), and pin connection swaps for back-annotation.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "sieda/CustomParts.hpp"
#include "sieda/Schematic.hpp"
#include "sieda/SchematicPdf.hpp"

namespace sieda {

namespace {
double snap10(double v) { return std::round(v / 10.0) * 10.0; }

/// "D7" counted up by `step` → "D8"; names without a trailing number (or bus notation) stay.
std::string countUp(const std::string& name, int step) {
    if (step == 0 || name.empty() || !expandBus(name).empty()) return name;
    size_t k = name.size();
    while (k > 0 && std::isdigit(static_cast<unsigned char>(name[k - 1]))) --k;
    if (k == name.size() || name.size() - k > 6) return name;
    const long long n = std::stoll(name.substr(k)) + step;
    if (n < 0) return name;
    std::string digits = std::to_string(n);
    const size_t width = name.size() - k;
    if (name[k] == '0' && digits.size() < width) digits.insert(0, width - digits.size(), '0');  // keep zero padding
    return name.substr(0, k) + digits;
}
}  // namespace

const char* alignModeName(AlignMode m) {
    switch (m) {
        case AlignMode::Left: return "left";
        case AlignMode::Right: return "right";
        case AlignMode::Top: return "top";
        case AlignMode::Bottom: return "bottom";
        case AlignMode::CenterX: return "centerX";
        case AlignMode::CenterY: return "centerY";
        case AlignMode::DistributeX: return "distributeX";
        case AlignMode::DistributeY: return "distributeY";
    }
    return "left";
}

bool alignModeFromName(const std::string& name, AlignMode* out) {
    for (AlignMode m : {AlignMode::Left, AlignMode::Right, AlignMode::Top, AlignMode::Bottom, AlignMode::CenterX,
                        AlignMode::CenterY, AlignMode::DistributeX, AlignMode::DistributeY})
        if (name == alignModeName(m)) {
            if (out) *out = m;
            return true;
        }
    return false;
}

int Schematic::alignComponents(const std::vector<int>& rawIds, AlignMode mode) {
    // Parts of a channel stand for their block's; each part once; packages (not drawn) are left out.
    std::vector<int> ids;
    for (int id : rawIds) {
        const int m = masterOf(id);
        const Component* c = find(m);
        if (c && !c->packageOnly && std::find(ids.begin(), ids.end(), m) == ids.end()) ids.push_back(m);
    }
    if (ids.size() < 2) return 0;
    std::vector<Vec2> at;
    for (int id : ids) at.push_back(find(id)->position);
    double minX = at[0].x, maxX = at[0].x, minY = at[0].y, maxY = at[0].y;
    for (const auto& p : at) {
        minX = std::min(minX, p.x);
        maxX = std::max(maxX, p.x);
        minY = std::min(minY, p.y);
        maxY = std::max(maxY, p.y);
    }
    std::vector<Vec2> to = at;
    switch (mode) {
        case AlignMode::Left:
            for (auto& p : to) p.x = minX;
            break;
        case AlignMode::Right:
            for (auto& p : to) p.x = maxX;
            break;
        case AlignMode::Top:
            for (auto& p : to) p.y = minY;
            break;
        case AlignMode::Bottom:
            for (auto& p : to) p.y = maxY;
            break;
        case AlignMode::CenterX:
            for (auto& p : to) p.x = snap10((minX + maxX) / 2);
            break;
        case AlignMode::CenterY:
            for (auto& p : to) p.y = snap10((minY + maxY) / 2);
            break;
        case AlignMode::DistributeX:
        case AlignMode::DistributeY: {
            // Equal steps between the outermost parts, in their current order.
            if (ids.size() < 3) return 0;
            const bool horizontal = mode == AlignMode::DistributeX;
            std::vector<size_t> order(ids.size());
            for (size_t i = 0; i < order.size(); ++i) order[i] = i;
            std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) {
                return horizontal ? at[a].x < at[b].x : at[a].y < at[b].y;
            });
            const double lo = horizontal ? minX : minY, hi = horizontal ? maxX : maxY;
            const double step = (hi - lo) / static_cast<double>(ids.size() - 1);
            for (size_t k = 0; k < order.size(); ++k) {
                const double v = k + 1 == order.size() ? hi : snap10(lo + step * static_cast<double>(k));
                (horizontal ? to[order[k]].x : to[order[k]].y) = v;
            }
            break;
        }
    }
    int moved = 0;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (to[i].x == at[i].x && to[i].y == at[i].y) continue;
        find(ids[i])->position = to[i];
        ++moved;
    }
    if (moved > 0) edited();
    return moved;
}

Json Schematic::copyComponents(const std::vector<int>& rawIds) const {
    std::vector<int> ids;
    for (int id : rawIds) {
        const Component* c = find(id);
        if (!c || c->packageOnly || std::find(ids.begin(), ids.end(), id) != ids.end()) continue;
        ids.push_back(id);
    }
    std::map<int, int> index;  // component id → position in the clipboard
    Json comps = Json::array();
    for (int id : ids) {
        const Component& c = *find(id);
        Json j = Json::object();
        j["kind"] = static_cast<int>(c.kind);
        j["value"] = c.value;
        j["x"] = c.position.x;
        j["y"] = c.position.y;
        j["rotation"] = c.rotation;
        if (!c.customPart.empty()) j["customPart"] = c.customPart;
        if (!c.package.empty()) j["package"] = c.package;
        if (c.kind == ComponentKind::NetLabel) {
            // A sheet entry leads into one child sheet: pasted elsewhere it is a local label.
            j["scope"] = labelScopeName(c.scope == LabelScope::SheetEntry ? LabelScope::Local : c.scope);
            if (isHarnessLabel(c)) j["harnessType"] = c.harnessType;  // entries get theirs from their harness
        }
        if (c.kind == ComponentKind::PartUnit) j["unit"] = c.unit;
        Json nc = Json::array();
        for (int p : c.noConnect) nc.push(p);
        if (!c.noConnect.empty()) j["noConnect"] = nc;
        index[id] = static_cast<int>(comps.size());
        comps.push(j);
    }
    // Harness entries keep their harness when both are copied.
    Json links = Json::array();
    for (int id : ids) {
        const Component& c = *find(id);
        if (c.harnessOf != 0 && index.count(c.harnessOf)) {
            Json l = Json::array();
            l.push(index[id]);
            l.push(index[c.harnessOf]);
            links.push(l);
        }
    }
    Json wires = Json::array();
    for (const auto& w : wires_) {
        auto a = index.find(w.a.component), b = index.find(w.b.component);
        if (a == index.end() || b == index.end()) continue;
        Json wj = Json::array();
        wj.push(a->second);
        wj.push(w.a.pin);
        wj.push(b->second);
        wj.push(w.b.pin);
        wires.push(wj);
    }
    Json out = Json::object();
    out["format"] = "sieda.schematic-clip/1";
    out["components"] = comps;
    out["wires"] = wires;
    out["harnessLinks"] = links;
    return out;
}

std::vector<int> Schematic::pasteComponents(const Json& clip, const PasteOptions& o) {
    std::vector<int> created;
    if (clip.get("format").asString("") != "sieda.schematic-clip/1") return created;
    const auto& comps = clip.get("components").items();
    if (comps.empty() || comps.size() > 5000) return created;
    const int count = std::clamp(o.count, 1, 256);
    for (int copy = 0; copy < count; ++copy) {
        const Vec2 shift{o.offset.x + o.step.x * copy, o.offset.y + o.step.y * copy};
        std::vector<int> ids(comps.size(), -1);
        for (size_t i = 0; i < comps.size(); ++i) {
            const Json& j = comps[i];
            const int kindValue = j.get("kind").asInt(-1);
            if (!Library::isValidKind(kindValue)) continue;
            const auto kind = static_cast<ComponentKind>(kindValue);
            const Vec2 at{snap10(j.get("x").asNumber(0) + shift.x), snap10(j.get("y").asNumber(0) + shift.y)};
            const int rotation = j.get("rotation").asInt(0);
            std::string value = j.get("value").asString("");
            if (kind == ComponentKind::NetLabel && o.labelIncrement != 0)
                value = countUp(value, o.labelIncrement * (copy + 1));
            int id = -1;
            if (kind == ComponentKind::Custom) {
                id = addCustomComponent(j.get("customPart").asString(""), value, at, rotation);
            } else if (kind == ComponentKind::PartUnit) {
                // A gate pasted on its own becomes a part of its own, showing the same gate.
                const int unit = j.get("unit").asInt(1);
                const int first = addCustomUnits(j.get("customPart").asString(""), value, at, rotation);
                id = first;
                if (first > 0 && unit > 1) {
                    id = addPartUnit(first, unit, at, rotation);
                    if (id > 0) removeComponent(first);
                    else id = first;
                }
            } else {
                id = addComponent(kind, value, at, rotation);
            }
            if (id < 0) continue;
            ids[i] = id;
            created.push_back(id);
            const std::string package = j.get("package").asString("");
            if (!package.empty()) setPackage(id, package);
            if (kind == ComponentKind::NetLabel) {
                LabelScope scope = LabelScope::Global;
                if (labelScopeFromName(j.get("scope").asString("global"), &scope) && scope != LabelScope::Global &&
                    scope != LabelScope::SheetEntry)
                    setLabelScope(id, scope);
                const std::string harness = j.get("harnessType").asString("");
                if (!harness.empty()) setLabelHarness(id, harness);
            }
            for (const auto& p : j.get("noConnect").items()) setPinNoConnect(id, p.asInt(-1), true);
        }
        for (const auto& l : clip.get("harnessLinks").items()) {
            const auto& pair = l.items();
            if (pair.size() != 2) continue;
            const int a = pair[0].asInt(-1), b = pair[1].asInt(-1);
            if (a < 0 || b < 0 || a >= static_cast<int>(ids.size()) || b >= static_cast<int>(ids.size())) continue;
            if (ids[static_cast<size_t>(a)] > 0 && ids[static_cast<size_t>(b)] > 0)
                setHarnessOf(ids[static_cast<size_t>(a)], ids[static_cast<size_t>(b)]);
        }
        for (const auto& w : clip.get("wires").items()) {
            const auto& f = w.items();
            if (f.size() != 4) continue;
            const int a = f[0].asInt(-1), b = f[2].asInt(-1);
            if (a < 0 || b < 0 || a >= static_cast<int>(ids.size()) || b >= static_cast<int>(ids.size())) continue;
            if (ids[static_cast<size_t>(a)] < 0 || ids[static_cast<size_t>(b)] < 0) continue;
            connect({ids[static_cast<size_t>(a)], f[1].asInt(-1)}, {ids[static_cast<size_t>(b)], f[3].asInt(-1)});
        }
    }
    return created;
}

bool Schematic::setHarnessOf(int labelId, int harnessLabel) {
    Component* c = find(masterOf(labelId));
    const Component* h = find(masterOf(harnessLabel));
    if (!c || c->kind != ComponentKind::NetLabel || isHarnessLabel(*c)) return false;
    if (harnessLabel != 0 && (!h || !isHarnessLabel(*h) || h->sheet != c->sheet || h->id == c->id)) return false;
    c->harnessOf = harnessLabel == 0 ? 0 : h->id;
    c->harnessType = harnessLabel == 0 ? std::string() : h->harnessType;
    c->scope = LabelScope::Local;
    invalidate();
    edited();
    return true;
}

bool Schematic::swapPinConnections(int componentId, int pinA, int pinB) {
    const int id = masterOf(componentId);
    Component* c = find(id);
    if (!c || pinA == pinB || c->packageOnly || c->kind == ComponentKind::Ground || c->kind == ComponentKind::NetLabel ||
        c->kind == ComponentKind::Junction)
        return false;
    const int n = static_cast<int>(c->def().pins.size());
    if (pinA < 0 || pinB < 0 || pinA >= n || pinB >= n) return false;
    for (auto& w : wires_)
        for (PinRef* end : {&w.a, &w.b}) {
            if (end->component != id) continue;
            if (end->pin == pinA) end->pin = pinB;
            else if (end->pin == pinB) end->pin = pinA;
        }
    for (auto& nc : c->noConnect) {
        if (nc == pinA) nc = pinB;
        else if (nc == pinB) nc = pinA;
    }
    invalidate();
    edited();
    return true;
}

bool Schematic::setSheetSize(int id, const std::string& size) {
    if (!findSheet(id) || (!size.empty() && !findSheetTemplate(size))) return false;
    for (auto& s : sheets_)
        if (s.id == id) s.size = size;
    return true;
}

bool Schematic::setSheetSymbolSize(int id, double width, double height) {
    if (!findSheet(id) || !std::isfinite(width) || !std::isfinite(height)) return false;
    for (auto& s : sheets_)
        if (s.id == id) {
            s.symbolWidth = std::clamp(width, 0.0, 4000.0);
            s.symbolHeight = std::clamp(height, 0.0, 4000.0);
        }
    return true;
}

}  // namespace sieda
