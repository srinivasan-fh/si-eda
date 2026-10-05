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

std::array<double, 4> Schematic::symbolOutline(const Component& c) const {
    // Local boxes as the canvas draws them (SchematicSymbols.bounds), before rotation.
    double x0 = -40, y0 = -40, x1 = 40, y1 = 40;
    auto box = [&](double x, double y, double w, double h) {
        x0 = x;
        y0 = y;
        x1 = x + w;
        y1 = y + h;
    };
    switch (c.kind) {
        case ComponentKind::Resistor: case ComponentKind::Capacitor: case ComponentKind::Inductor: case ComponentKind::Diode:
        case ComponentKind::LED: case ComponentKind::Switch: case ComponentKind::Fuse:
            box(-30, -14, 60, 28);
            break;
        case ComponentKind::VoltageSource: case ComponentKind::CurrentSource: case ComponentKind::ACSource: box(-18, -30, 36, 60); break;
        case ComponentKind::Battery: box(-16, -30, 32, 60); break;
        case ComponentKind::Ground: box(-12, -2, 24, 22); break;
        case ComponentKind::NPN: case ComponentKind::NMOS: box(-30, -30, 56, 60); break;
        case ComponentKind::OpAmp: box(-40, -28, 80, 56); break;
        case ComponentKind::Connector: box(-20, -22, 28, 44); break;
        case ComponentKind::IC8: box(-40, -42, 80, 84); break;
        case ComponentKind::Junction: box(-5, -5, 10, 10); break;
        case ComponentKind::NetLabel:
            box(-2, -9, std::max(60.0, std::max(36.0, 7.0 * static_cast<double>(c.value.size()) + 16) + 9), 18);
            break;
        case ComponentKind::Custom:
        case ComponentKind::PartUnit:
            if (const CustomPart* part = CustomPartRegistry::instance().find(c.customPart)) {
                double hw = part->symbolHalfWidth, hh = part->symbolHalfHeight;
                const std::vector<PinDef>* pins = &part->def.pins;
                if (c.kind == ComponentKind::PartUnit && c.unit >= 1 && c.unit <= static_cast<int>(part->units.size())) {
                    const auto& u = part->units[static_cast<size_t>(c.unit - 1)];
                    hw = u.halfWidth;
                    hh = u.halfHeight;
                    pins = &u.def.pins;
                }
                double h = hh;
                for (const auto& p : *pins) h = std::max(h, std::fabs(p.offset.y));
                box(-(hw + 20), -h, 2 * (hw + 20), 2 * h);
                if (c.kind == ComponentKind::Custom && !part->spec.symbol.graphics.empty()) {
                    const auto g = symbolGraphicsBounds(part->spec.symbol.graphics);
                    x0 = std::min(x0, g[0]);
                    y0 = std::min(y0, g[1]);
                    x1 = std::max(x1, g[2]);
                    y1 = std::max(y1, g[3]);
                }
            }
            break;
        default:
            break;
    }
    double mx = 1e300, my = 1e300, Mx = -1e300, My = -1e300;
    for (const Vec2 corner : {Vec2{x0, y0}, Vec2{x1, y0}, Vec2{x1, y1}, Vec2{x0, y1}}) {
        const Vec2 p = c.position + rotate90(corner, c.rotation);
        mx = std::min(mx, p.x);
        my = std::min(my, p.y);
        Mx = std::max(Mx, p.x);
        My = std::max(My, p.y);
    }
    return {mx, my, Mx, My};
}

int Schematic::alignComponents(const std::vector<int>& rawIds, AlignMode mode, bool byOutline) {
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
    if (byOutline) return alignOutlines(ids, at, mode);
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

int Schematic::alignOutlines(const std::vector<int>& ids, const std::vector<Vec2>& at, AlignMode mode) {
    std::vector<std::array<double, 4>> box;
    for (int id : ids) box.push_back(symbolOutline(*find(id)));
    double minX = box[0][0], minY = box[0][1], maxX = box[0][2], maxY = box[0][3];
    for (const auto& b : box) {
        minX = std::min(minX, b[0]);
        minY = std::min(minY, b[1]);
        maxX = std::max(maxX, b[2]);
        maxY = std::max(maxY, b[3]);
    }
    std::vector<Vec2> to = at;
    for (size_t i = 0; i < ids.size(); ++i) {
        const auto& b = box[i];
        switch (mode) {
            case AlignMode::Left: to[i].x = snap10(at[i].x + minX - b[0]); break;
            case AlignMode::Right: to[i].x = snap10(at[i].x + maxX - b[2]); break;
            case AlignMode::Top: to[i].y = snap10(at[i].y + minY - b[1]); break;
            case AlignMode::Bottom: to[i].y = snap10(at[i].y + maxY - b[3]); break;
            case AlignMode::CenterX: to[i].x = snap10(at[i].x + (minX + maxX) / 2 - (b[0] + b[2]) / 2); break;
            case AlignMode::CenterY: to[i].y = snap10(at[i].y + (minY + maxY) / 2 - (b[1] + b[3]) / 2); break;
            default: break;
        }
    }
    if (mode == AlignMode::DistributeX || mode == AlignMode::DistributeY) {
        // Equal gaps between the outlines, the outermost two staying where they are.
        if (ids.size() < 3) return 0;
        const bool horizontal = mode == AlignMode::DistributeX;
        const size_t lo = horizontal ? 0 : 1, hi = horizontal ? 2 : 3;
        std::vector<size_t> order(ids.size());
        for (size_t i = 0; i < order.size(); ++i) order[i] = i;
        std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return box[a][lo] < box[b][lo]; });
        double total = 0;
        for (const auto& b : box) total += b[hi] - b[lo];
        const double span = box[order.back()][hi] - box[order.front()][lo];
        const double gap = (span - total) / static_cast<double>(ids.size() - 1);
        double edge = box[order.front()][hi] + gap;
        for (size_t k = 1; k + 1 < order.size(); ++k) {
            const size_t i = order[k];
            const double shift = edge - box[i][lo];
            (horizontal ? to[i].x : to[i].y) = snap10((horizontal ? at[i].x : at[i].y) + shift);
            edge += box[i][hi] - box[i][lo] + gap;
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

Json Schematic::copyComponents(const std::vector<int>& rawIds, const std::vector<int>& busIds) const {
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
        j["source"] = id;
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
    // Buses: those the copied bus entries belong to, and the ones asked for; each entry keeps its bus.
    std::vector<int> busList;
    auto addBusId = [&](int b) {
        const Bus* bus = findBus(b);
        if (bus && std::find(busList.begin(), busList.end(), bus->id) == busList.end()) busList.push_back(bus->id);
    };
    for (int b : busIds) addBusId(b);
    for (int id : ids) addBusId(find(id)->bus);
    Json buses = Json::array();
    for (int b : busList) {
        const Bus* bus = findBus(b);
        Json bj = Json::object();
        bj["name"] = bus->name;
        Json pts = Json::array();
        for (const auto& p : bus->points) {
            Json pj = Json::array();
            pj.push(p.x);
            pj.push(p.y);
            pts.push(pj);
        }
        bj["points"] = pts;
        buses.push(bj);
    }
    Json busLinks = Json::array();
    for (int id : ids) {
        const Component& c = *find(id);
        const auto it = std::find(busList.begin(), busList.end(), c.bus);
        if (c.bus == 0 || it == busList.end()) continue;
        Json l = Json::array();
        l.push(index[id]);
        l.push(static_cast<int>(it - busList.begin()));
        busLinks.push(l);
    }
    // Net directives on the copied parts' pins (a channel copy carries its block part's).
    Json directives = Json::array();
    for (const auto& d : directives_)
        for (int id : ids)
            if (d.component == id || d.component == masterOf(id)) {
                Json dj = Json::object();
                dj["component"] = index[id];
                dj["pin"] = d.pin;
                if (!d.netClass.empty()) dj["netClass"] = d.netClass;
                if (d.diffPair) dj["diffPair"] = true;
                if (d.trackWidth > 0) dj["trackWidth"] = d.trackWidth;
                if (d.clearance > 0) dj["clearance"] = d.clearance;
                directives.push(dj);
                break;
            }
    Json out = Json::object();
    out["format"] = "sieda.schematic-clip/1";
    out["components"] = comps;
    out["wires"] = wires;
    out["harnessLinks"] = links;
    if (!buses.items().empty()) {
        out["buses"] = buses;
        out["busLinks"] = busLinks;
    }
    if (!directives.items().empty()) out["directives"] = directives;
    return out;
}

std::vector<int> Schematic::pasteComponents(const Json& clip, const PasteOptions& o, std::vector<std::vector<int>>* perCopy) {
    std::vector<int> created;
    if (perCopy) perCopy->clear();
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
        // Buses (moved with the copy) and their entries.
        std::vector<int> busIds;
        for (const auto& bj : clip.get("buses").items()) {
            std::vector<Vec2> pts;
            for (const auto& pj : bj.get("points").items()) {
                const double x = pj[size_t{0}].asNumber(NAN), y = pj[size_t{1}].asNumber(NAN);
                if (std::isfinite(x) && std::isfinite(y)) pts.push_back({snap10(x + shift.x), snap10(y + shift.y)});
            }
            busIds.push_back(pts.size() >= 2 && pts.size() <= 256 ? addBus(bj.get("name").asString(""), pts) : -1);
        }
        for (const auto& l : clip.get("busLinks").items()) {
            const int a = l[size_t{0}].asInt(-1), b = l[size_t{1}].asInt(-1);
            if (a < 0 || b < 0 || a >= static_cast<int>(ids.size()) || b >= static_cast<int>(busIds.size())) continue;
            if (ids[static_cast<size_t>(a)] > 0 && busIds[static_cast<size_t>(b)] > 0)
                setLabelBus(ids[static_cast<size_t>(a)], busIds[static_cast<size_t>(b)]);
        }
        // Net directives on the pasted parts.
        for (const auto& dj : clip.get("directives").items()) {
            const int a = dj.get("component").asInt(-1);
            if (a < 0 || a >= static_cast<int>(ids.size()) || ids[static_cast<size_t>(a)] < 0) continue;
            NetDirective d;
            d.component = ids[static_cast<size_t>(a)];
            d.pin = dj.get("pin").asInt(0);
            d.netClass = dj.get("netClass").asString("");
            d.diffPair = dj.get("diffPair").asBool(false);
            d.trackWidth = dj.get("trackWidth").asNumber(0);
            d.clearance = dj.get("clearance").asNumber(0);
            addDirective(d);
        }
        if (perCopy) perCopy->push_back(ids);
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
    const SheetTemplate* t = size.empty() ? nullptr : findSheetTemplate(size);
    // The frame is fixed where it is drawn now: centred on the drawing (on the old frame's centre when it had one).
    Vec2 centre{0, 0};
    bool any = false;
    double x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    for (const auto& c : components_) {
        if (c.sheet != id || c.packageOnly) continue;
        const auto b = symbolOutline(c);
        if (!any) {
            x0 = b[0];
            y0 = b[1];
            x1 = b[2];
            y1 = b[3];
            any = true;
        }
        x0 = std::min(x0, b[0]);
        y0 = std::min(y0, b[1]);
        x1 = std::max(x1, b[2]);
        y1 = std::max(y1, b[3]);
    }
    if (any) centre = {(x0 + x1) / 2, (y0 + y1) / 2};
    for (auto& s : sheets_) {
        if (s.id != id) continue;
        if (s.frameFixed && !s.size.empty())
            if (const SheetTemplate* old = findSheetTemplate(s.size))
                centre = {s.frameOrigin.x + old->widthMm / kSchematicUnitMm / 2, s.frameOrigin.y + old->heightMm / kSchematicUnitMm / 2};
        s.size = size;
        s.frameFixed = t != nullptr;
        if (t) s.frameOrigin = {snap10(centre.x - t->widthMm / kSchematicUnitMm / 2), snap10(centre.y - t->heightMm / kSchematicUnitMm / 2)};
        else s.frameOrigin = {0, 0};
    }
    return true;
}

bool Schematic::setSheetFrame(int id, bool fixed, Vec2 origin) {
    const Sheet* sh = findSheet(id);
    if (!sh || sh->size.empty() || !std::isfinite(origin.x) || !std::isfinite(origin.y)) return false;
    for (auto& s : sheets_)
        if (s.id == id) {
            s.frameFixed = fixed;
            s.frameOrigin = fixed ? Vec2{snap10(std::clamp(origin.x, -1e6, 1e6)), snap10(std::clamp(origin.y, -1e6, 1e6))} : Vec2{0, 0};
        }
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
