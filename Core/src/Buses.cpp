// SiEDA Core — graphical buses: named polylines on a sheet with bus entries ripped out of them.
//
// A bus is drawing: its members ("D[0..7]" → D0 … D7) leave it through bus entries, which are net labels attached
// to it (Component::bus). The entries join nets by name like any label (local to the sheet by default), so the
// netlist, simulation and PCB need nothing new. ERC checks entries against the bus's members and reports members
// that lead nowhere.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <map>
#include <set>

#include "sieda/Schematic.hpp"

namespace sieda {

namespace {
constexpr size_t kMaxBusPoints = 256;

std::string upperText(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string trimmedName(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}

bool validPoints(const std::vector<Vec2>& points) {
    if (points.size() < 2 || points.size() > kMaxBusPoints) return false;
    return std::all_of(points.begin(), points.end(),
                       [](Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::fabs(p.x) < 1e7 && std::fabs(p.y) < 1e7; });
}

Vec2 nearestOnSegment(Vec2 a, Vec2 b, Vec2 p) {
    const double dx = b.x - a.x, dy = b.y - a.y, len2 = dx * dx + dy * dy;
    double t = len2 > 0 ? ((p.x - a.x) * dx + (p.y - a.y) * dy) / len2 : 0;
    t = std::clamp(t, 0.0, 1.0);
    return {a.x + t * dx, a.y + t * dy};
}

Vec2 snapped(Vec2 p) { return {std::round(p.x / 10.0) * 10.0, std::round(p.y / 10.0) * 10.0}; }
}  // namespace

const Bus* Schematic::findBus(int id) const {
    for (const auto& b : buses_)
        if (b.id == id) return &b;
    return nullptr;
}

void Schematic::restoreBus(const Bus& in) {
    Bus b = in;
    b.name = trimmedName(b.name);
    if (b.id <= 0 || findBus(b.id) || !findSheet(b.sheet) || b.name.empty() || b.name.size() > 256 || !validPoints(b.points))
        return;
    b.instanceOf = std::max(0, b.instanceOf);
    buses_.push_back(b);
    nextBusId_ = std::max(nextBusId_, b.id + 1);
}

void Schematic::repairBusLinks() {
    buses_.erase(std::remove_if(buses_.begin(), buses_.end(), [&](const Bus& b) { return !findSheet(b.sheet); }),
                 buses_.end());
    for (auto& c : components_) {
        if (c.bus == 0) continue;
        const Bus* b = findBus(c.bus);
        if (c.kind != ComponentKind::NetLabel || !b || b->sheet != c.sheet || c.scope == LabelScope::SheetEntry) c.bus = 0;
    }
}

int Schematic::masterBusOf(int id) const {
    const Bus* b = findBus(id);
    if (b && b->instanceOf > 0 && findBus(b->instanceOf)) return b->instanceOf;
    return id;
}

int Schematic::addBus(const std::string& rawName, const std::vector<Vec2>& points) {
    const std::string name = trimmedName(rawName);
    if (expandBus(name).empty() || !validPoints(points)) return -1;
    Bus b;
    b.id = nextBusId_++;
    b.sheet = definitionSheet(activeSheet_);
    b.name = name;
    for (Vec2 p : points) b.points.push_back(snapped(p));
    buses_.push_back(b);
    edited();
    if (b.sheet != activeSheet_)  // drawn on an instance: report the bus shown there
        for (const auto& c : buses_)
            if (c.instanceOf == b.id && c.sheet == activeSheet_) return c.id;
    return b.id;
}

bool Schematic::removeBus(int id) {
    id = masterBusOf(id);
    if (!findBus(id)) return false;
    std::set<int> gone;
    for (const auto& c : components_)
        if (c.bus == id) gone.insert(c.id);
    components_.erase(std::remove_if(components_.begin(), components_.end(), [&](const Component& c) { return gone.count(c.id) > 0; }),
                      components_.end());
    wires_.erase(std::remove_if(wires_.begin(), wires_.end(),
                                [&](const Wire& w) { return gone.count(w.a.component) || gone.count(w.b.component); }),
                 wires_.end());
    buses_.erase(std::remove_if(buses_.begin(), buses_.end(), [&](const Bus& b) { return b.id == id; }), buses_.end());
    invalidate();
    edited();
    return true;
}

bool Schematic::renameBus(int id, const std::string& rawName) {
    id = masterBusOf(id);
    const std::string name = trimmedName(rawName);
    if (expandBus(name).empty()) return false;
    for (auto& b : buses_)
        if (b.id == id) {
            b.name = name;
            edited();
            return true;
        }
    return false;
}

bool Schematic::moveBus(int id, Vec2 delta) {
    id = masterBusOf(id);
    if (!std::isfinite(delta.x) || !std::isfinite(delta.y)) return false;
    for (auto& b : buses_) {
        if (b.id != id) continue;
        for (auto& p : b.points) p = p + delta;
        for (auto& c : components_)
            if (c.bus == id) c.position = c.position + delta;
        edited();
        return true;
    }
    return false;
}

bool Schematic::setBusPoints(int id, const std::vector<Vec2>& points) {
    id = masterBusOf(id);
    if (!validPoints(points)) return false;
    for (auto& b : buses_)
        if (b.id == id) {
            b.points.clear();
            for (Vec2 p : points) b.points.push_back(snapped(p));
            edited();
            return true;
        }
    return false;
}

std::vector<std::string> Schematic::busMembers(int id) const {
    const Bus* b = findBus(id);
    return b ? expandBus(b->name) : std::vector<std::string>{};
}

Vec2 Schematic::nearestBusPoint(int id, Vec2 p) const {
    const Bus* b = findBus(id);
    if (!b || b->points.empty()) return p;
    Vec2 best = b->points.front();
    double bestD = 1e300;
    for (size_t i = 0; i + 1 < b->points.size(); ++i) {
        const Vec2 q = nearestOnSegment(b->points[i], b->points[i + 1], p);
        const double d = std::hypot(q.x - p.x, q.y - p.y);
        if (d < bestD) {
            bestD = d;
            best = q;
        }
    }
    return best;
}

int Schematic::ripBusEntries(int id, const std::vector<std::string>& wanted, LabelScope scope) {
    id = masterBusOf(id);
    const Bus* bus = findBus(id);
    if (!bus || scope == LabelScope::SheetEntry) return -1;
    const auto members = expandBus(bus->name);
    std::vector<std::string> list = wanted.empty() ? members : wanted;
    for (const auto& m : list)
        if (std::find(members.begin(), members.end(), m) == members.end()) return -1;
    std::set<std::string> present;
    for (const auto& c : components_)
        if (c.bus == id) present.insert(c.value);
    const int sheet = bus->sheet;
    const std::vector<Vec2> points = bus->points;
    // Entries every 20 units along the bus, from its first point.
    double length = 0;
    for (size_t i = 0; i + 1 < points.size(); ++i) length += std::hypot(points[i + 1].x - points[i].x, points[i + 1].y - points[i].y);
    auto along = [&](double d) {
        for (size_t i = 0; i + 1 < points.size(); ++i) {
            const double seg = std::hypot(points[i + 1].x - points[i].x, points[i + 1].y - points[i].y);
            if (d <= seg || i + 2 == points.size()) {
                const double t = seg > 0 ? std::min(1.0, d / seg) : 0;
                return Vec2{points[i].x + t * (points[i + 1].x - points[i].x), points[i].y + t * (points[i + 1].y - points[i].y)};
            }
            d -= seg;
        }
        return points.back();
    };
    std::set<std::pair<double, double>> taken;
    for (const auto& c : components_)
        if (c.bus == id) taken.insert({c.position.x, c.position.y});
    const int shown = activeSheet_;
    activeSheet_ = sheet;
    int added = 0;
    double d = 20;
    for (const auto& m : list) {
        if (present.count(m)) continue;
        Vec2 at;
        do {
            at = snapped(along(std::min(d, length))) + Vec2{10, -10};
            d += 20;
        } while (taken.count({at.x, at.y}) && d < length + 20 * static_cast<double>(list.size()) + 40);
        taken.insert({at.x, at.y});
        const int label = addComponent(ComponentKind::NetLabel, m, at);
        if (Component* c = find(label)) {
            c->scope = scope;
            c->bus = id;
        }
        present.insert(m);
        ++added;
    }
    activeSheet_ = shown;
    invalidate();
    edited();
    return added;
}

int Schematic::connectBusToPart(int id, int componentId, LabelScope scope) {
    id = masterBusOf(id);
    componentId = masterOf(componentId);
    const Bus* bus = findBus(id);
    const Component* part = find(componentId);
    if (!bus || !part || isNetSymbolKind(part->kind) || part->sheet != bus->sheet || scope == LabelScope::SheetEntry)
        return -1;
    const auto members = expandBus(bus->name);
    const auto& pins = part->def().pins;
    // Members that name a pin of the part (the whole name or one function of "PB0/D0"), else the open pins in order.
    std::vector<std::pair<std::string, int>> pairs;
    std::set<int> used;
    bool named = false;
    for (const auto& m : members) {
        const std::string key = upperText(m);
        for (size_t i = 0; i < pins.size(); ++i) {
            const std::string name = upperText(pins[i].name);
            bool match = name == key;
            for (size_t start = 0; !match && start <= name.size();) {
                const size_t slash = name.find('/', start);
                match = name.substr(start, slash == std::string::npos ? std::string::npos : slash - start) == key;
                if (slash == std::string::npos) break;
                start = slash + 1;
            }
            if (match) named = true;
            if (match && used.insert(static_cast<int>(i)).second) {
                if (isPinConnected({componentId, static_cast<int>(i)})) break;  // already wired: left as it is
                pairs.push_back({m, static_cast<int>(i)});
                break;
            }
        }
    }
    if (!named) {
        size_t next = 0;
        for (size_t i = 0; i < pins.size() && next < members.size(); ++i)
            if (!isPinConnected({componentId, static_cast<int>(i)}) && !part->isNoConnect(static_cast<int>(i)))
                pairs.push_back({members[next++], static_cast<int>(i)});
    }
    const int sheet = bus->sheet;
    const int shown = activeSheet_;
    activeSheet_ = sheet;
    int made = 0;
    for (const auto& [member, pin] : pairs) {
        const Vec2 pp = pinPosition({componentId, pin});
        const Vec2 q = snapped(nearestBusPoint(id, pp));
        const Vec2 at = q + Vec2{pp.x >= q.x ? 10.0 : -10.0, pp.y >= q.y ? 10.0 : -10.0};
        const int label = addComponent(ComponentKind::NetLabel, member, at, pp.x >= q.x ? 0 : 180);
        if (Component* c = find(label)) {
            c->scope = scope;
            c->bus = id;
        }
        if (connect({label, 0}, {componentId, pin}) >= 0) ++made;
    }
    activeSheet_ = shown;
    invalidate();
    edited();
    return made;
}

void Schematic::busERC(std::vector<RuleViolation>& out) const {
    for (const auto& b : buses_) {
        const auto members = expandBus(b.name);
        std::vector<const Component*> entries;
        for (const auto& c : components_)
            if (c.bus == b.id) entries.push_back(&c);
        if (entries.empty()) {
            if (b.instanceOf != 0) continue;  // reported once, on the repeated sheet's definition
            RuleViolation v;
            v.severity = Severity::Warning;
            v.code = "ERC_BUS_NO_ENTRIES";
            v.message = "Bus " + b.name + " has no entries: rip its members out to the pins they connect to.";
            v.location = b.points.front();
            v.hasLocation = true;
            v.sheet = b.sheet;
            out.push_back(v);
            continue;
        }
        std::set<std::string> reported;
        for (const Component* e : entries) {
            auto add = [&](Severity s, const std::string& code, const std::string& msg) {
                RuleViolation v;
                v.severity = s;
                v.code = code;
                v.message = msg;
                v.components = {e->id};
                v.location = e->position;
                v.hasLocation = true;
                v.sheet = e->sheet;
                out.push_back(v);
            };
            if (std::find(members.begin(), members.end(), e->value) == members.end()) {
                add(Severity::Error, "ERC_BUS_ENTRY_NOT_MEMBER",
                    "Bus entry " + e->value + " is not a member of bus " + b.name + ".");
                continue;
            }
            if (reported.count(e->value)) continue;
            const int net = netOf({e->id, 0});
            if (net < 0) continue;
            std::vector<std::string> reached;
            for (const auto& p : nets()[static_cast<size_t>(net)].pins) {
                const Component* c = find(p.component);
                if (c && !isNetSymbolKind(c->kind) && p.pin >= 0 && static_cast<size_t>(p.pin) < c->def().pins.size())
                    reached.push_back(c->ref + "." + c->def().pins[static_cast<size_t>(p.pin)].name);
            }
            if (reached.size() == 1) {
                reported.insert(e->value);
                add(Severity::Warning, "ERC_BUS_MEMBER_UNCONNECTED",
                    "Bus " + b.name + " member " + e->value + " reaches only " + reached.front() +
                        ": nothing else on the bus connects to it.");
            }
        }
    }
}

}  // namespace sieda
