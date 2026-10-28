// SiEDA Core — the MCP tool table: every tool's name, group, description, JSON Schema, annotations and handler.
// Handlers drive the C API (sieda_c.h) on the server's project, so the stdio server and the app's live endpoint run
// exactly what the app runs. Data-driven: mcpTools() is this table (docs/MCP.md lists it).
#include <algorithm>
#include <array>
#include <regex>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <numeric>
#include <iterator>
#include <set>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "RouteQuality.hpp"
#include "McpInternal.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/LibraryImport.hpp"
#include "sieda/Documents.hpp"
#include "sieda/Units.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Mechanical.hpp"
#include "sieda/Export.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Thermal.hpp"
#include "sieda/Autoroute.hpp"
#include "sieda/CloudSync.hpp"
#include "sieda/ExternalLibrary.hpp"

namespace fs = std::filesystem;

namespace sieda::mcp {
namespace {

using Handler = std::function<ToolOutput(McpServer&, const Json&)>;

enum class Kind {
    Read,         // changes nothing
    Edit,         // changes the design
    Destructive,  // removes or replaces work
    Files,        // writes files inside the root (no design change)
};

struct Table {
    std::vector<McpTool> tools;
    void add(const char* group, const char* name, const char* title, Kind kind, bool idempotent, const std::string& desc,
             Json schema, Handler handler) {
        McpTool t;
        t.name = name;
        t.group = group;
        t.title = title;
        t.description = desc;
        t.inputSchema = schema.isObject() ? schema : Schema().build();
        t.readOnly = kind == Kind::Read;
        t.destructive = kind == Kind::Destructive;
        t.idempotent = idempotent || kind == Kind::Read;
        t.mutates = kind == Kind::Edit || kind == Kind::Destructive;
        t.handler = std::move(handler);
        tools.push_back(std::move(t));
    }
};

ToolOutput out(Json data) {
    ToolOutput o;
    o.data = std::move(data);
    return o;
}

Json obj() { return Json::object(); }

SiedaProject* P(McpServer& s) { return s.project(); }
const Project& proj(McpServer& s) { return s.project()->project; }

/// Throws a ToolError when a C API call reports failure.
void check(bool ok, const std::string& message) {
    if (!ok) throw ToolError(message);
}

/// The C API options object: the arguments minus the keys the tool handles itself.
std::string optionsFrom(const Json& a, std::initializer_list<const char*> drop = {}) {
    Json o = Json::object();
    if (a.isObject())
        for (const auto& [k, v] : a.fields()) {
            bool skip = false;
            for (const char* d : drop) skip = skip || k == d;
            if (!skip) o[k] = v;
        }
    return o.dump();
}

/// A free spot on the active sheet for a new part (row by row on a 120-unit grid).
Vec2 freeSpot(const Project& p) {
    const Schematic& s = p.schematic;
    const int sheet = s.activeSheet();
    for (int i = 0; i < 400; ++i) {
        const Vec2 c{static_cast<double>((i % 8) * 120), static_cast<double>((i / 8) * 120)};
        bool taken = false;
        for (const auto& comp : s.components()) {  // clear of every symbol's outline, not only its origin
            if (comp.sheet != sheet || comp.packageOnly) continue;
            const auto o = s.symbolOutline(comp);
            if (c.x + 70 > o[0] && c.x - 70 < o[2] && c.y + 70 > o[1] && c.y - 70 < o[3]) {
                taken = true;
                break;
            }
        }
        if (!taken) return c;
    }
    return {0, static_cast<double>(s.components().size() * 40)};
}

/// Where a label / ground symbol goes next to a pin: away from the part's centre, 20 units out (on the grid).
/// Rotation (clockwise degrees, y down) that points a net label's tag along `d`.
int outwardRotation(Vec2 d) { return d.x > 0.5 ? 0 : d.x < -0.5 ? 180 : d.y > 0.5 ? 90 : 270; }

Vec2 outsidePin(const Project& p, PinAddress a, double distance) {
    const Vec2 pin = p.schematic.pinPosition({a.component, a.pin});
    const Vec2 d = pinOutward(p, a);
    return {std::round((pin.x + d.x * distance) / 10) * 10, std::round((pin.y + d.y * distance) / 10) * 10};
}

/// Runs `f` with the component's sheet active (a label or ground symbol for one of its pins belongs on that sheet),
/// then makes the sheet that was active before active again.
template <typename F>
auto onPinSheet(SiedaProject* p, int component, F f) {
    const int was = p->project.schematic.activeSheet();
    const Component* c = p->project.schematic.find(component);
    if (c && c->sheet != was) sieda_set_active_sheet(p, c->sheet);
    struct Restore {
        SiedaProject* p;
        int was;
        ~Restore() {
            if (p->project.schematic.activeSheet() != was) sieda_set_active_sheet(p, was);
        }
    } restore{p, was};
    return f();
}

int netIndex(const Project& p, const std::string& name) {
    const auto& nets = p.schematic.nets();
    for (size_t i = 0; i < nets.size(); ++i)
        if (nets[i].name == name) return static_cast<int>(i);
    for (size_t i = 0; i < nets.size(); ++i) {
        std::string a = nets[i].name, b = name;
        std::transform(a.begin(), a.end(), a.begin(), ::tolower);
        std::transform(b.begin(), b.end(), b.begin(), ::tolower);
        if (a == b) return static_cast<int>(i);
    }
    std::string known;
    for (size_t i = 0; i < nets.size() && i < 60; ++i) known += (i ? ", " : "") + nets[i].name;
    throw ToolError("No net \"" + name + "\". Nets: " + (known.empty() ? std::string("(none)") : known));
}

std::string netName(const Project& p, int net) {
    const auto& nets = p.schematic.nets();
    return net >= 0 && net < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(net)].name : std::string();
}

/// Decimates an array to at most n values (keeping the last).
Json decimate(const Json& arr, size_t n) {
    if (!arr.isArray() || n == 0 || arr.size() <= n) return arr;
    Json out = Json::array();
    const size_t stride = (arr.size() + n - 1) / n;
    for (size_t i = 0; i < arr.size(); i += stride) out.push(arr[i]);
    if ((arr.size() - 1) % stride != 0) out.push(arr[arr.size() - 1]);
    return out;
}

std::set<std::string> nameSet(const Json& a, const std::string& key) {
    std::set<std::string> s;
    if (a.get(key).isArray())
        for (const auto& v : a.get(key).items())
            if (v.isString()) s.insert(v.asString());
    return s;
}

Json pinsOfSpec(const CustomPartSpec& spec) {
    Json pins = Json::array();
    for (const auto& p : spec.pins) {
        Json j = obj();
        j["number"] = p.number;
        j["name"] = p.name;
        j["type"] = pinTypeName(p.type);
        if (!p.description.empty()) j["description"] = p.description;
        pins.push(j);
    }
    return pins;
}

std::string lowerStr(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string compact(const std::string& s) {
    std::string o;
    for (char c : s)
        if (std::isalnum(static_cast<unsigned char>(c))) o += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return o;
}

/// A catalog part by name, ignoring case and punctuation ("AMS1117 3.3" = "AMS1117-3.3"). Unknown names throw with the
/// closest catalog names: an orderable number ("PIC16F877A-I/P") suggests its base part ("PIC16F877A").
const StandardPart& catalogPart(const std::string& name) {
    if (const StandardPart* sp = findStandardPart(name)) return *sp;
    const std::string key = compact(name);
    std::vector<std::pair<size_t, const StandardPart*>> near;
    for (const auto& p : standardParts()) {
        const std::string n = compact(p.spec.name);
        if (n == key) return p;
        size_t common = 0;
        while (common < n.size() && common < key.size() && n[common] == key[common]) ++common;
        // A whole catalog name at the start (the orderable number's base part) ranks first.
        if (common >= std::min<size_t>(4, key.size())) near.push_back({common + (common == n.size() ? 1000 : 0), &p});
    }
    std::stable_sort(near.begin(), near.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::string list;
    for (size_t i = 0; i < near.size() && i < 8; ++i) {
        const auto& sp = near[i].second->spec;
        list += (i ? ", " : "") + sp.name + " (" + sp.package.type + "-" + std::to_string(sp.package.pinCount) + ")";
    }
    throw ToolError("No catalog part \"" + name + "\"" + (list.empty() ? "" : ". Closest: " + list) +
                    " (library_search finds names)");
}

/// A search word against a part's text (as the app's PartQuery): a short word (≤ 3 letters) starts a word of the text
/// ("ble" finds "BLE-MODULE", not "adjustable"; "esp" finds "ESP32"), a longer one appears in it, separators ignored
/// ("wifi" finds "Wi-Fi"); "ble" / "bt" also find Bluetooth and "wlan" Wi-Fi.
bool wordMatches(const std::string& hay, const std::string& word) {
    static const std::map<std::string, std::string> alias = {{"ble", "bluetooth"}, {"bt", "bluetooth"}, {"wlan", "wifi"}};
    if (auto it = alias.find(word); it != alias.end() && wordMatches(hay, it->second)) return true;
    const bool letters = !word.empty() && std::all_of(word.begin(), word.end(), [](unsigned char c) { return std::isalpha(c); });
    if (letters && word.size() <= 3) {
        for (size_t at = hay.find(word); at != std::string::npos; at = hay.find(word, at + 1))
            if (at == 0 || !std::isalnum(static_cast<unsigned char>(hay[at - 1]))) return true;
        return false;
    }
    return hay.find(word) != std::string::npos || (!compact(word).empty() && compact(hay).find(compact(word)) != std::string::npos);
}

/// "8" → 8…8, "6-10", ">40", "<8", ">=40", "<=8" (as the app's PartQuery).
bool pinRange(const std::string& v, int* lo, int* hi) {
    try {
        if (v.rfind(">=", 0) == 0) { *lo = std::stoi(v.substr(2)); *hi = 1 << 30; return true; }
        if (v.rfind("<=", 0) == 0) { *lo = 0; *hi = std::stoi(v.substr(2)); return true; }
        if (v.rfind(">", 0) == 0) { *lo = std::stoi(v.substr(1)) + 1; *hi = 1 << 30; return true; }
        if (v.rfind("<", 0) == 0) { *lo = 0; *hi = std::stoi(v.substr(1)) - 1; return true; }
        const size_t dash = v.find('-');
        if (dash != std::string::npos && dash > 0) { *lo = std::stoi(v.substr(0, dash)); *hi = std::stoi(v.substr(dash + 1)); return *lo <= *hi; }
        *lo = *hi = std::stoi(v);
        return true;
    } catch (...) {
        return false;
    }
}

Json projectSummary(McpServer& s) {
    const Project& p = proj(s);
    const Schematic& sch = p.schematic;
    Json j = obj();
    j["name"] = p.name;
    j["path"] = s.projectPath();
    if (!p.requirements.empty()) j["requirements"] = p.requirements.substr(0, 600);
    j["industry"] = p.industry;
    int parts = 0, labels = 0, placed = 0, unplaced = 0;
    for (const auto& c : sch.components()) {
        if (c.packageOnly) continue;
        if (isNetSymbolKind(c.kind)) {
            ++labels;
            continue;
        }
        ++parts;
        if (c.hasFootprint() && p.pcb.onBoard(c)) (c.pcb.placed ? placed : unplaced)++;
    }
    j["components"] = parts;
    j["netSymbols"] = labels;
    j["wires"] = sch.wires().size();
    int nets = 0;
    for (const auto& n : sch.nets()) nets += n.pins.size() > 1;
    j["nets"] = nets;
    j["sheets"] = sch.sheets().size();
    j["activeSheet"] = sch.activeSheet();
    if (!p.activeVariant.empty()) j["activeVariant"] = p.activeVariant;
    Json b = obj();
    b["width"] = p.pcb.settings.width;
    b["height"] = p.pcb.settings.height;
    b["layers"] = p.pcb.settings.layerCount;
    b["rulePreset"] = p.pcb.settings.rulePreset;
    b["trackWidth"] = p.pcb.settings.trackWidth;
    b["clearance"] = p.pcb.settings.clearance;
    b["customOutline"] = p.pcb.settings.hasCustomOutline();
    b["footprintsPlaced"] = placed;
    b["footprintsNotPlaced"] = unplaced;
    b["tracks"] = p.pcb.tracks.size();
    b["vias"] = p.pcb.vias.size();
    b["unroutedConnections"] = p.pcb.ratsnest(sch).size();
    b["pours"] = p.pcb.zones.size();
    j["board"] = b;
    Json sess = obj();
    sess["readOnly"] = s.options().readOnly;
    sess["root"] = s.options().allowedRoot;
    sess["attached"] = s.attached();
    j["session"] = sess;
    return j;
}

/// A straight wire p0–p1 running inside a part's body or a net label (exactly, however thin the body).
bool wireCrossesBody(const Schematic& sch, Vec2 p0, Vec2 p1) {
    for (const auto& c : sch.components()) {
        if (c.packageOnly || (isNetSymbolKind(c.kind) && c.kind != ComponentKind::NetLabel)) continue;
        const auto o = sch.symbolOutline(c);
        if (std::max(p0.x, p1.x) > o[0] + 1 && std::min(p0.x, p1.x) < o[2] - 1 && std::max(p0.y, p1.y) > o[1] + 1 && std::min(p0.y, p1.y) < o[3] - 1)
            return true;
    }
    return false;
}

/// Lays out parts schematic_build placed: each wire chain's parts side by side with the wired pins facing each other
/// (two-pin parts turned to suit), so the wires run straight; the groups then go in rows with room for net labels.
/// A bare pin name several pins share ("Q3.S" on a power MOSFET's three source pins), written without its number,
/// means every one of them not on the same spot of the symbol: the first pin's namesakes.
std::vector<int> namesakes(const SiedaProject& p, PinAddress a, const Json& spec) {
    std::vector<int> more;
    const Component* c = p.project.schematic.find(a.component);
    if (!c || !spec.isString() || spec.asString().find('(') != std::string::npos) return more;
    const auto& pins = c->def().pins;
    const std::string name = pins[static_cast<size_t>(a.pin)].name;
    std::vector<Vec2> spots{p.project.schematic.pinPosition({a.component, a.pin})};
    for (int i = 0; i < static_cast<int>(pins.size()); ++i) {
        if (i == a.pin || pins[static_cast<size_t>(i)].name != name) continue;
        const Vec2 at = p.project.schematic.pinPosition({a.component, i});
        if (std::any_of(spots.begin(), spots.end(), [&](Vec2 q) { return (q - at).length() < 1e-6; })) continue;
        spots.push_back(at);
        more.push_back(i);
    }
    return more;
}

void layoutBuilt(SiedaProject* p, const std::vector<int>& ids, const Json& wires, const std::set<std::pair<int, int>>& grounded = {},
                 const std::map<std::pair<int, int>, std::string>& pinNet = {}, std::map<int, PinAddress>* rehub = nullptr,
                 std::map<int, std::pair<int, PinAddress>>* taps = nullptr) {
    const Schematic& sch = p->project.schematic;
    std::vector<std::pair<PinAddress, PinAddress>> links;
    for (const auto& chain : wires.items())
        for (size_t i = 1; i < chain.size(); ++i) try {
                links.push_back({pinAddress(*p, chain.items()[i - 1]), pinAddress(*p, chain.items()[i])});
            } catch (const std::exception&) {}
    // Crystals (and their load capacitors) first: they stand by the IC's pins before its other parts take the room.
    auto isCrystal = [&](int id) {
        const std::string& r = sch.find(id)->ref;
        return r.size() > 1 && r[0] == 'Y' && std::isdigit(static_cast<unsigned char>(r[1]));
    };
    std::set<int> mine(ids.begin(), ids.end()), done, standing;
    auto supplyName = [&](int id, int pin) {  // the pin's net is a positive supply (+3V3, VCC, 5V)
        const auto n = pinNet.find({id, pin});
        if (n == pinNet.end() || n->second.empty()) return false;
        std::string up = n->second;
        for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return up[0] == '+' || (std::isdigit(static_cast<unsigned char>(up[0])) && up.find('V') != std::string::npos) ||
               up.rfind("VCC", 0) == 0 || up.rfind("VDD", 0) == 0;
    };
    // A pulled-up side pin whose second two-pin part (an EN cap) taps the pull-up's wire from above: the pin below has a
    // pull-up of its own (its wire would run under a part beyond), and nothing is linked or labelled up to 100 above.
    auto tapPin = [&](int hub, int pin, double side) {
        const double y = sch.pinPosition({hub, pin}).y;
        bool pulled = false, extra = false, below = false, above = false;
        for (const auto& [c0, e0] : links)
            for (const auto& [c, e] : {std::pair{c0, e0}, std::pair{e0, c0}}) {
                if (c.component != hub || !mine.count(e.component) || pinOutward(p->project, c).x * side < 0.5) continue;
                const bool two = sch.find(e.component)->def().pins.size() == 2, pull = two && supplyName(e.component, 1 - e.pin);
                const double dy = sch.pinPosition({hub, c.pin}).y - y;
                if (c.pin == pin && pull && !pulled) pulled = true;
                else if (c.pin == pin) extra = extra || two;
                else if (pull && dy > 0.5 && dy < 70) below = true;
            }
        for (int j = 0; j < static_cast<int>(sch.find(hub)->def().pins.size()); ++j) {
            const double dy = sch.pinPosition({hub, j}).y - y;
            above = above || (j != pin && pinNet.count({hub, j}) && pinOutward(p->project, {hub, j}).x * side > 0.5 && dy < -0.5 && dy > -100);
        }
        return pulled && extra && below && !above;
    };
    // Then pull-ups (two-pin parts to a supply) on an IC's pins, standing up beside them before other parts take the room.
    std::stable_partition(links.begin(), links.end(), [&](const auto& l) {
        for (const auto& [a, b] : {std::pair{l.first, l.second}, std::pair{l.second, l.first}})
            if (mine.count(b.component) && sch.find(b.component)->def().pins.size() == 2 && sch.find(a.component)->def().pins.size() > 4 &&
                supplyName(b.component, 1 - b.pin))
                return true;
        return false;
    });
    std::stable_partition(links.begin(), links.end(), [&](const auto& l) { return isCrystal(l.first.component) || isCrystal(l.second.component); });
    auto moveBy = [&](int id, Vec2 d, double grid = 10) {
        const Vec2 at = sch.find(id)->position;
        sieda_move_component(p, id, std::round((at.x + d.x) / grid) * grid, std::round((at.y + d.y) / grid) * grid);
    };
    auto room = [&](int id) {  // the outline with room for a net label beside each side that has pins
        const Component& c = *sch.find(id);
        auto o = sch.symbolOutline(c);
        const auto body = o;
        for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
            const Vec2 q = sch.pinPosition({id, i});
            // A side pin's label as long as its net's name; a grounded side pin's symbol stands beyond the labels.
            const auto net = pinNet.find({id, i});
            const int drawn = mine.count(id) ? -1 : sch.netOf({id, i});  // a part drawn before: its net's name as the schematic has it
            const size_t chars = net != pinNet.end() ? net->second.size() : drawn >= 0 ? sch.nets()[static_cast<size_t>(drawn)].name.size() : 0;
            const double side = grounded.count({id, i}) ? (c.def().pins.size() == 2 ? 50.0 : 140.0)  // (a two-pin part's ground: no label between)
                                                        : std::max(60.0, 10.0 * static_cast<double>(chars) + 30);
            if (q.x <= body[0] + 0.5) o[0] = std::min(o[0], body[0] - side);
            if (q.x >= body[2] - 0.5) o[2] = std::max(o[2], body[2] + side);
            if (q.y <= body[1] + 0.5) o[1] = std::min(o[1], body[1] - 60);
            if (q.y >= body[3] - 0.5 && !standing.count(id)) o[3] = std::max(o[3], body[3] + 60);  // (a pull-up's wire comes in from the side)
        }
        o[1] = std::min(o[1], body[1] - 14), o[3] = std::max(o[3], body[3] + 14);  // its designator above, value below
        return o;
    };
    double x = 0, y = 0, rowHeight = 0, rowWidth = 1200;
    auto drawnBefore = [&](const Component& c) { return !mine.count(c.id) && !c.packageOnly && !isNetSymbolKind(c.kind); };
    for (const auto& c : sch.components())  // a later build: rows start below what is drawn already
        if (drawnBefore(c)) y = std::max(y, room(c.id)[3] + 120);
    // A group wired to a part drawn before goes beside that part's pin, stepped along it until clear of everything.
    auto anchor = [&](const std::vector<int>& group) {
        const std::set<int> in(group.begin(), group.end());
        for (const auto& [a0, b0] : links)
            for (const auto& [a, b] : {std::pair{a0, b0}, std::pair{b0, a0}}) {
                const Component* old = sch.find(a.component);
                if (!in.count(b.component) || !old || !drawnBefore(*old)) continue;
                const Vec2 d = pinOutward(p->project, a);
                if (group.size() == 1 && sch.find(b.component)->def().pins.size() == 2)
                    for (int turn = 0; turn < 3; ++turn) {
                        const Vec2 e = pinOutward(p->project, b);
                        if (std::fabs(e.x + d.x) < 0.5 && std::fabs(e.y + d.y) < 0.5) break;
                        sieda_rotate_component(p, b.component, 90);
                    }
                bool labelled = false;  // its pin's label from before: room for the new part's own label facing it
                for (const auto& w : sch.wires())
                    for (const auto& [x, y] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                        labelled = labelled || (x.component == a.component && x.pin == a.pin && sch.find(y.component) &&
                                                sch.find(y.component)->kind == ComponentKind::NetLabel);
                const Vec2 shift = sch.pinPosition({a.component, a.pin}) + d * (labelled ? 220 : 120) - sch.pinPosition({b.component, b.pin});
                for (int id : group) moveBy(id, shift);
                const Vec2 side = std::fabs(d.x) > 0.5 ? Vec2{0, 1} : Vec2{1, 0};
                for (double out = 0; out <= 300; out += 100) {  // further out when its labels need the room
                    double along = 0;
                    for (int step = 0; step < 40; ++step) {
                        bool hit = false;
                        for (int id : group)
                            for (const auto& c : sch.components()) {
                                if (in.count(c.id) || c.packageOnly || isNetSymbolKind(c.kind) || c.sheet != sch.find(id)->sheet) continue;
                                const auto m = room(id), q = room(c.id);
                                hit |= m[0] < q[2] && q[0] < m[2] && m[1] < q[3] && q[1] < m[3];
                            }
                        for (int id : group) hit |= room(id)[0] < 0 || room(id)[1] < 0;  // on the sheet, not above or left of it
                        if (!hit) return true;
                        const double next = (step / 2 + 1) * 40.0 * (step % 2 ? -1 : 1);  // either side, further each time
                        for (int id : group) moveBy(id, side * (next - along));
                        along = next;
                    }
                    for (int id : group) moveBy(id, d * 100 - side * along);
                }
                return false;
            }
        return false;
    };
    std::vector<int> roots = ids;  // a group grows from its biggest part (the IC), so its signals read left to right
    std::stable_sort(roots.begin(), roots.end(), [&](int l, int r) {
        return sch.find(l)->def().pins.size() > sch.find(r)->def().pins.size();
    });
    for (int root : roots) {
        if (done.count(root)) continue;
        std::vector<int> group{root};
        done.insert(root);
        // Lays out the parts linked to `hub` beside its pins; a crystal's load capacitors go right after the crystal,
        // before the IC's other parts take the room below it.
        std::function<void(int)> expand = [&](int hub) {
            for (const auto& [a0, b0] : links)
                for (const auto& [a, b] : {std::pair{a0, b0}, std::pair{b0, a0}}) {
                    if (a.component != hub || !mine.count(b.component) || done.count(b.component)) continue;
                    // A crystal's load capacitor hangs below its pin (ground at the bottom), clear of the IC beside it.
                    const std::string& hubRef = sch.find(a.component)->ref;
                    const bool loadCap = hubRef.size() > 1 && hubRef[0] == 'Y' && std::isdigit(static_cast<unsigned char>(hubRef[1])) &&
                                         sch.find(b.component)->def().pins.size() == 2;
                    const Vec2 d = loadCap ? Vec2{0, 1} : pinOutward(p->project, a);
                    // A small part (a 4-pad crystal) whose pin faces the same way as this one is mirrored to face it.
                    if (const Vec2 e = pinOutward(p->project, b); sch.find(b.component)->def().pins.size() <= 4 &&
                                                                  sch.find(b.component)->def().pins.size() != 2 && e.x * d.x + e.y * d.y > 0.5)
                        sieda_mirror_component(p, b.component, std::fabs(d.y) > 0.5 ? 1 : 0);
                    if (const Vec2 e = pinOutward(p->project, b); sch.find(b.component)->def().pins.size() != 2 && e.x * d.x + e.y * d.y > -0.5)
                        continue;  // its pin does not face this one: placed by it, the part would sit on the other
                    // A part grounded at its other end above an upward pin stands beside it the right way up (its
                    // pin facing up too, joined by labels), not upside down over the pin with its ground on top.
                    const bool upright = d.y < -0.5 && sch.find(b.component)->def().pins.size() == 2 && grounded.count({b.component, 1 - b.pin});
                    // A pull-up on a side pin stands up beside it on its own wire, the first on each pin: pull-ups on
                    // pins higher up nearer the IC, so no wire runs under another's body.
                    int rank = -1;
                    double pullRun = 120;
                    bool lastPull = false;  // the lowest pull-up on its side: free to step further out, clear of the group
                    if (std::fabs(d.x) > 0.5 && sch.find(b.component)->def().pins.size() == 2 && sch.find(a.component)->def().pins.size() > 4 &&
                        supplyName(b.component, 1 - b.pin)) {
                        std::map<int, int> pulled;  // the hub's pins on this side with a pull-up: pin → its first pull-up
                        for (const auto& [c0, e0] : links)
                            for (const auto& [c, e] : {std::pair{c0, e0}, std::pair{e0, c0}})
                                if (c.component == a.component && mine.count(e.component) && sch.find(e.component)->def().pins.size() == 2 &&
                                    supplyName(e.component, 1 - e.pin) && pinOutward(p->project, c).x * d.x > 0.5)
                                    pulled.emplace(c.pin, e.component);
                        if (pulled[a.pin] == b.component) {
                            rank = 0;
                            standing.insert(b.component);
                            for (const auto& [pin, part] : pulled)
                                rank += sch.pinPosition({a.component, pin}).y < sch.pinPosition({a.component, a.pin}).y - 0.5;
                            // Its distance out: past the pull-ups above (80 apart) and past a part in line on a pin up to
                            // 110 above it (an AREF cap), so its body never stands across that part's wire.
                            std::vector<std::pair<double, int>> order;
                            for (const auto& [pin, part] : pulled) order.push_back({sch.pinPosition({a.component, pin}).y, pin});
                            std::sort(order.begin(), order.end());
                            double prev = 40;
                            for (const auto& [y, pin] : order) {
                                double r = prev + 80;
                                for (const auto& [c0, e0] : links)
                                    for (const auto& [c, e] : {std::pair{c0, e0}, std::pair{e0, c0}})
                                        if (c.component == a.component && !pulled.count(c.pin) && mine.count(e.component) &&
                                            sch.find(e.component)->def().pins.size() == 2 && pinOutward(p->project, c).x * d.x > 0.5 &&
                                            sch.pinPosition({c.component, c.pin}).y < y - 0.5 && sch.pinPosition({c.component, c.pin}).y > y - 110)
                                            r = std::max(r, 290.0);
                                if (taps && tapPin(a.component, pin, d.x)) r = std::max(r, 180.0);  // (room for the tap above)
                                if (pin == a.pin) pullRun = r, lastPull = pin == order.back().second;
                                prev = r;
                            }
                        }
                    }
                    if (sch.find(b.component)->def().pins.size() == 2)
                        for (int turn = 0; turn < 3; ++turn) {
                            const Vec2 e = pinOutward(p->project, b);
                            if (upright || rank >= 0 ? e.y * (rank >= 0 ? -1 : 1) < -0.5 : std::fabs(e.x + d.x) < 0.5 && std::fabs(e.y + d.y) < 0.5) break;
                            sieda_rotate_component(p, b.component, 90);
                        }
                    const Vec2 from = sch.pinPosition({a.component, a.pin}), to = sch.pinPosition({b.component, b.pin});
                    // Clear of the labels on an IC's other pins; close up between two-pin parts.
                    // A crystal stands further out from its IC, so its load capacitors hang clear of the IC's labels.
                    const std::string& ref = sch.find(b.component)->ref;
                    const bool crystal = ref.size() > 1 && ref[0] == 'Y' && std::isdigit(static_cast<unsigned char>(ref[1]));
                    double run = rank >= 0 ? pullRun : upright ? 0 : loadCap ? 100 : sch.find(a.component)->def().pins.size() == 2 ? 40 : crystal ? 300 : 120;
                    if (rank < 0 && run == 120 && std::fabs(d.x) > 0.5)  // past the labels on the hub's neighbouring pins (a USB-C's USB_DP)
                        for (int j = 0; j < static_cast<int>(sch.find(a.component)->def().pins.size()); ++j) {
                            const auto net = pinNet.find({a.component, j});
                            if (j == a.pin || net == pinNet.end() || pinOutward(p->project, {a.component, j}).x * d.x < 0.5 ||
                                std::fabs(sch.pinPosition({a.component, j}).y - sch.pinPosition({a.component, a.pin}).y) > 45)
                                continue;
                            run = std::max(run, 12.0 * static_cast<double>(net->second.size()) + 70);
                        }
                    moveBy(b.component, upright ? Vec2{from.x + 80 - to.x, from.y - 140 - to.y}
                                                : Vec2{from.x + d.x * run - to.x, from.y + d.y * run - to.y},
                           loadCap || rank >= 0 ? 5 : 10);  // in line with the crystal's pin (5 off the 10 grid)
                    // Two parts on one pin (or a chain folding back): the nearest clear spot, along the pin's side either
                    // way or further out (its pin then gets a label), clear of the group (a crystal's load capacitor
                    // either side of its pin, so it stays under the crystal).
                    auto hitAt = [&, hub = a, part = b](bool labelled) {  // labelled: joined to its pin by labels (their room counts too)
                        const auto o = sch.symbolOutline(*sch.find(part.component)), ro = room(part.component);
                        bool hit = false;
                        for (int other : group) {  // the part it is wired to: bodies apart; any other: label room too
                            const auto& m = other == hub.component ? o : ro;
                            const double g = 20;
                            auto near = [&](const std::array<double, 4>& q) { return m[0] < q[2] + g && q[0] < m[2] + g && m[1] < q[3] + g && q[1] < m[3] + g; };
                            if (other == hub.component || sch.find(other)->def().pins.size() <= 4) {  // (a small part: its whole room)
                                hit |= near(other == hub.component ? sch.symbolOutline(*sch.find(other)) : room(other));
                                continue;
                            }
                            // A larger part: its body and the label strip at each pin with a net, not the box round them all.
                            auto body = sch.symbolOutline(*sch.find(other));
                            hit |= near({body[0], body[1] - 14, body[2], body[3] + 14});
                            for (int j = 0; j < static_cast<int>(sch.find(other)->def().pins.size()) && !hit; ++j) {
                                const auto net = pinNet.find({other, j});
                                if (net == pinNet.end() && !grounded.count({other, j})) continue;
                                const Vec2 q = sch.pinPosition({other, j}), e = pinOutward(p->project, {other, j});
                                const double len = grounded.count({other, j}) ? 140.0 : std::max(60.0, 10.0 * static_cast<double>(net->second.size()) + 30);
                                const Vec2 r = q + e * len;
                                hit |= near({std::min(q.x, r.x) - 12, std::min(q.y, r.y) - 12, std::max(q.x, r.x) + 12, std::max(q.y, r.y) + 12});
                            }
                        }
                        // Nor over the labels on the hub's other pins.
                        const Component& h = *sch.find(hub.component);
                        for (int j = 0; j < static_cast<int>(h.def().pins.size()) && !hit; ++j) {
                            const auto net = pinNet.find({h.id, j});
                            if ((j == hub.pin && !labelled) || net == pinNet.end()) continue;
                            const Vec2 q = sch.pinPosition({h.id, j}), e = pinOutward(p->project, {h.id, j});
                            const Vec2 r = q + e * (std::fabs(e.x) > 0.5 ? 12.0 * static_cast<double>(net->second.size()) + 40 : 80);
                            const std::array<double, 4> m = labelled ? ro : std::array<double, 4>{o[0], o[1] - 14, o[2], o[3] + 14};
                            hit = m[0] < std::max(q.x, r.x) + 12 && std::min(q.x, r.x) - 12 < m[2] &&
                                  m[1] < std::max(q.y, r.y) + 12 && std::min(q.y, r.y) - 12 < m[3];
                        }
                        return hit;
                    };
                    // A second part on a pulled-up pin (an auto-reset cap, a BOOT button) in line beyond its pull-up, wired from
                    // the pull-up's lower pin, unless a pull-up on a pin just below would run its wire under it.
                    if (const Vec2 home = sch.find(b.component)->position;
                        rank < 0 && rehub && std::fabs(d.x) > 0.5 && sch.find(b.component)->def().pins.size() == 2 && sch.find(a.component)->def().pins.size() > 4) {
                        PinAddress up{-1, 0};
                        bool below = false;
                        for (const auto& [c0, e0] : links)
                            for (const auto& [c, e] : {std::pair{c0, e0}, std::pair{e0, c0}}) {
                                if (c.component != a.component || !standing.count(e.component) || !done.count(e.component)) continue;
                                if (c.pin == a.pin) up = e;
                                const double dy = sch.pinPosition({c.component, c.pin}).y - from.y;
                                below = below || (pinOutward(p->project, c).x * d.x > 0.5 && dy > 0.5 && dy < 70);
                            }
                        if (up.component >= 0 && below && taps && tapPin(a.component, a.pin, d.x)) {  // above the wire, by a junction on it
                            moveBy(b.component, from + d * 30 + Vec2{0, -60} - sch.pinPosition({b.component, b.pin}));
                            const Vec2 q = sch.pinPosition({b.component, 1 - b.pin});
                            auto ob = sch.symbolOutline(*sch.find(b.component));
                            const std::array<double, 4> m{std::min(ob[0], q.x - 30), ob[1] - 14, std::max(ob[2], q.x + 30), std::max(ob[3], q.y + 40)};
                            bool clear = true;
                            for (int other : group) {
                                const auto o = sch.symbolOutline(*sch.find(other));
                                clear = clear && !(m[0] < o[2] + 10 && o[0] < m[2] + 10 && m[1] < o[3] + 10 && o[1] < m[3] + 10);
                            }
                            if (clear) {
                                (*taps)[b.component] = {b.pin, PinAddress{a.component, a.pin}};
                                done.insert(b.component);
                                group.push_back(b.component);
                                continue;
                            }
                            moveBy(b.component, home - sch.find(b.component)->position);
                        }
                        if (up.component >= 0 && !below) {
                            const Vec2 rp = sch.pinPosition({up.component, up.pin});
                            moveBy(b.component, rp + d * 80 - sch.pinPosition({b.component, b.pin}));
                            const Vec2 q = sch.pinPosition({b.component, 1 - b.pin}), e = pinOutward(p->project, {b.component, 1 - b.pin});
                            const auto net = pinNet.find({b.component, 1 - b.pin});
                            const double len = grounded.count({b.component, 1 - b.pin}) ? 60.0 : net == pinNet.end() ? 30.0 : 10.0 * static_cast<double>(net->second.size()) + 40;
                            auto ob = sch.symbolOutline(*sch.find(b.component));
                            const Vec2 r = q + e * len;
                            const std::array<double, 4> m{std::min({ob[0], r.x}), std::min({ob[1] - 14, r.y}), std::max({ob[2], r.x}), std::max({ob[3] + 14, r.y})};
                            bool clear = !wireCrossesBody(sch, rp, sch.pinPosition({b.component, b.pin}));
                            for (int other : group) {
                                if (other == up.component) continue;
                                const auto o = sch.symbolOutline(*sch.find(other));
                                clear = clear && !(m[0] < o[2] + 10 && o[0] < m[2] + 10 && m[1] < o[3] + 10 && o[1] < m[3] + 10);
                            }
                            if (clear) {
                                (*rehub)[b.component] = up;
                                done.insert(b.component);
                                group.push_back(b.component);
                                continue;
                            }
                            moveBy(b.component, home - sch.find(b.component)->position);
                            // No room in line (another part's labels there): hanging below the pull-up's wire, upright,
                            // joined by a junction on it, at the first clear spot along it.
                            if (taps) {
                                int turns = 0;
                                for (; turns < 3 && pinOutward(p->project, b).y > -0.5; ++turns) sieda_rotate_component(p, b.component, 90);
                                bool hung = false;
                                for (double k = 60; !hung && k + 30 < std::fabs(rp.x - from.x); k += 20) {
                                    moveBy(b.component, Vec2{from.x + d.x * k, from.y + 40} - sch.pinPosition({b.component, b.pin}));
                                    hung = !hitAt(true) && !wireCrossesBody(sch, rp, from);
                                }
                                if (hung) {
                                    (*taps)[b.component] = {b.pin, PinAddress{a.component, a.pin}};
                                    done.insert(b.component);
                                    group.push_back(b.component);
                                    continue;
                                }
                                for (; turns > 0 && turns < 4; ++turns) sieda_rotate_component(p, b.component, 90);  // back round
                                moveBy(b.component, home - sch.find(b.component)->position);
                            }
                        }
                    }
                    for (int i = 0; lastPull && i < 30 && hitAt(false); ++i) moveBy(b.component, d * 10, 5);
                    bool placed = true, chain = false;  // chain: linked on beyond this part
                    for (const auto& [c, e] : links)
                        chain = chain || (sch.find(a.component)->def().pins.size() > 4 && ((c.component == b.component && e.component != a.component) ||
                                                                                          (e.component == b.component && c.component != a.component)));
                    if (hitAt(upright)) {
                        placed = false;
                        const Vec2 home = sch.find(b.component)->position;
                        const auto o = sch.symbolOutline(*sch.find(b.component));
                        // A step clears the designator above and the value below as well.
                        const double w = std::fabs(d.x) > 0.5 ? o[3] - o[1] + 60 : o[2] - o[0] + 40;
                        const Vec2 side = std::fabs(d.x) > 0.5 ? Vec2{0, 1} : Vec2{1, 0};
                        std::vector<std::pair<double, Vec2>> spots;
                        bool beside = false;  // pull-ups stand on the hub's pins: in line further out (still wired) before aside
                        for (int id : standing)  // (on this side of the hub)
                            beside = beside || (std::fabs(d.x) > 0.5 && std::find(group.begin(), group.end(), id) != group.end() &&
                                                (sch.find(id)->position.x - from.x) * d.x > 0);
                        // A connector (more than four pins) goes further out in line before aside: its wire stays straight.
                        const bool big = sch.find(b.component)->def().pins.size() > 4;
                        for (double out : loadCap ? std::vector<double>{0} : big ? std::vector<double>{0, 80, 200, 340, 420} : std::vector<double>{0, 80, 200, 340})
                            for (int k = -10; k <= 10; ++k)
                                if ((k || out > 0) && (!k || out < 500))  // along a chain of two-pin parts: further out in line before aside
                                    spots.push_back({std::abs(k) * w + out * (sch.find(a.component)->def().pins.size() == 2 ? 0.5 : beside || (big && !k) ? 0.2 : 1.5), d * out + side * (k * w)});
                        std::stable_sort(spots.begin(), spots.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
                        for (const auto& [cost, off] : spots) {
                            moveBy(b.component, home + off - sch.find(b.component)->position, loadCap ? 5 : 10);
                            const Vec2 q0 = sch.pinPosition({a.component, a.pin}), q1 = sch.pinPosition({b.component, b.pin});
                            const bool inLine = std::fabs(q0.x - q1.x) < 0.5 || std::fabs(q0.y - q1.y) < 0.5;  // (wired: clear of bodies
                            bool across = inLine && wireCrossesBody(sch, q0, q1);                                // and of the group's
                            for (int other : group)                                                              // grounds and labels)
                                if (const auto r = room(other); inLine && other != a.component && other != b.component)
                                    across = across || (std::max(q0.x, q1.x) > r[0] + 1 && std::min(q0.x, q1.x) < r[2] - 1 &&
                                                        std::max(q0.y, q1.y) > r[1] + 1 && std::min(q0.y, q1.y) < r[3] - 1);
                            if (inLine ? !hitAt(false) && !across : !hitAt(true)) {  // (in line: a wire, no label to make room for)
                                placed = inLine || !chain;
                                break;
                            }
                        }
                    }
                    // A two-pin part with no clear spot (or a chain's first part, an LED's resistor, with none in line by an
                    // IC) starts a group of its own, its chain wired, joined to the rest by labels.
                    if (!placed && !loadCap && sch.find(b.component)->def().pins.size() == 2) continue;
                    done.insert(b.component);
                    group.push_back(b.component);
                    if (crystal) expand(b.component);
                }
        };
        for (size_t g = 0; g < group.size(); ++g) expand(group[g]);
        if (anchor(group)) continue;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (int id : group) {  // the bodies, but the top-left with each part's labels and designator: all on the sheet
            const auto o = sch.symbolOutline(*sch.find(id)), r = room(id);
            x0 = std::min(x0, r[0] + 100), y0 = std::min(y0, r[1]), x1 = std::max(x1, o[2]), y1 = std::max(y1, o[3]);
        }
        rowWidth = std::max(rowWidth, x1 - x0 + 100);  // rows as wide as the widest group: small groups sit side by side
        // A row may run as wide as a landscape sheet beside its tallest group (a small group beside the IC's).
        if (x > 0 && x + (x1 - x0) > std::max(rowWidth, rowHeight * 1.8)) x = 0, y += rowHeight + 120, rowHeight = 0;
        for (int id : group) moveBy(id, {x + 100 - x0, y - y0});
        x += x1 - x0 + 200;
        rowHeight = std::max(rowHeight, y1 - y0);
    }
}

/// Net labels and grounds wired to this part alone (they belong to it: they move and go with it).
std::vector<int> ownNetSymbols(const Schematic& sch, int id) {
    std::map<int, bool> alone;
    for (const auto& w : sch.wires())
        for (const auto& [mine, other] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}}) {
            const Component* o = sch.find(other.component);
            if (!o || !isNetSymbolKind(o->kind) || o->kind == ComponentKind::Junction) continue;
            if (mine.component == id) alone.emplace(o->id, true);
            else alone[o->id] = false;
        }
    std::vector<int> out;
    for (const auto& [sym, only] : alone)
        if (only) out.push_back(sym);
    return out;
}

/// Pours held while a repair pass tries its candidates (no refill per trial); `fresh` judges a candidate that passed
/// on the held pours again on refilled ones, so a change is kept only as the DRC sees it afterwards, and `reverted`
/// (after putting the copper back) refills them for that state after a failed `fresh`. Pours a board already holds stay held.
struct PourTrials {
    Project& pr;
    const bool held = pr.pcb.holdPours;
    bool dirty = false;
    explicit PourTrials(Project& p) : pr(p) {
        pr.pcb.zoneFills(pr.schematic);
        pr.pcb.holdPours = pr.pcb.pourTrials = true;  // (held pours are stale: their clearance is judged on refill)
    }
    ~PourTrials() { pr.pcb.holdPours = held, pr.pcb.pourTrials = false; }
    PourTrials(const PourTrials&) = delete;
    PourTrials& operator=(const PourTrials&) = delete;
    template <class F> bool fresh(F&& test) {
        pr.pcb.holdPours = held, pr.pcb.pourTrials = false;
        const bool ok = test();
        pr.pcb.holdPours = pr.pcb.pourTrials = true;
        dirty = !ok;
        return ok;
    }
    void reverted() {
        if (dirty) fresh([&] { pr.pcb.zoneFills(pr.schematic); return true; });
    }
};

/// A jog shorter than 0.1 mm between two tracks (left by gloss / via minimisation) folds into one corner: the tracks
/// meeting its one end move to the other, so no acute sliver remains. Kept only when the DRC finds no more problems.
int collapseJogs(Project& pr) {
    auto problems = [&] {
        int n = 0;
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info;
        return n;
    };
    const auto before = pr.pcb.tracks;
    const int was = problems();
    int folded = 0;
    for (bool again = true; again;) {
        again = false;
        for (size_t i = 0; i < pr.pcb.tracks.size(); ++i) {
            const Track t = pr.pcb.tracks[i];
            if (t.arc || t.teardrop || (t.b - t.a).length() >= 0.1) continue;
            const bool viaAtA = std::any_of(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& v) { return (v.position - t.a).length() < 1e-6; });
            const Vec2 from = viaAtA ? t.b : t.a, to = viaAtA ? t.a : t.b;  // a via end stays where it is
            for (auto& o : pr.pcb.tracks) {
                if (o.id == t.id || o.net != t.net || o.layer != t.layer || o.arc || o.teardrop) continue;
                if ((o.a - from).length() < 1e-6) o.a = to;
                if ((o.b - from).length() < 1e-6) o.b = to;
            }
            pr.pcb.tracks.erase(pr.pcb.tracks.begin() + static_cast<long>(i));
            ++folded;
            again = true;
            break;
        }
    }
    if (folded && problems() > was) {
        pr.pcb.tracks = before;
        return 0;
    }
    return folded;
}

/// A track leaving an IC pad at an angle can bulge past the pad's edge toward the next pin (DRC_FINE_PITCH_PADS): it
/// leaves straight along the pad's long axis first, then turns. Each change is kept only when both new pieces keep the
/// clearance to the part's other pads and the DRC finds no more errors or warnings. Returns the tracks straightened.
/// A track end the router left touching nothing (DRC_DANGLING_TRACK, e.g. 0.25 mm past the track it tees into): the
/// track goes when that adds no DRC problem, else its end comes back along it to the first point on copper of its net.
/// Returns the stubs fixed.
int trimStubs(Project& pr) {
    auto problems = [&] {
        std::multiset<std::string> n;
        for (const auto& v : pr.pcb.runDRC(pr.schematic))
            if (v.severity != Severity::Info) n.insert(v.code + " " + v.message);
        return n;
    };
    int fixed = 0;
    PourTrials trials(pr);
    for (int pass = 0; pass < 20; ++pass) {
        std::multiset<std::string> was;
        Vec2 at;
        bool found = false;
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) {
            if (v.code == "DRC_DANGLING_TRACK" && !found) at = v.location, found = true;
            if (v.severity != Severity::Info) was.insert(v.code + " " + v.message);
        }
        if (!found) break;
        size_t t = pr.pcb.tracks.size();
        for (size_t i = 0; i < pr.pcb.tracks.size(); ++i) {
            const Track& k = pr.pcb.tracks[i];
            if (!k.arc && !k.teardrop && ((k.a - at).length() < 1e-6 || (k.b - at).length() < 1e-6)) t = i;
        }
        if (t == pr.pcb.tracks.size()) break;
        const Track old = pr.pcb.tracks[t];
        const bool atB = (old.b - at).length() < 1e-6;
        const Vec2 other = atB ? old.a : old.b;
        auto improves = [&] {
            const auto now = problems();
            return now.size() < was.size() && std::includes(was.begin(), was.end(), now.begin(), now.end());
        };
        auto better = [&] { return improves() && trials.fresh(improves); };
        pr.pcb.tracks.erase(pr.pcb.tracks.begin() + static_cast<long>(t));
        if (better()) { ++fixed; continue; }
        pr.pcb.tracks.insert(pr.pcb.tracks.begin() + static_cast<long>(t), old);
        trials.reverted();
        bool done = false;
        const double len = (other - at).length();
        for (double d = 0.02; d < len - 0.02 && !done; d += 0.02) {
            const Vec2 end = at + (other - at) * (d / len);
            bool onCopper = false;  // the new end on another track of its net, on its layer
            for (size_t k = 0; k < pr.pcb.tracks.size() && !onCopper; ++k)
                onCopper = k != t && pr.pcb.tracks[k].net == old.net && pr.pcb.tracks[k].layer == old.layer &&
                           trackPointDistance(pr.pcb.tracks[k], end) <= (pr.pcb.tracks[k].width + old.width) / 4;
            if (!onCopper) continue;
            (atB ? pr.pcb.tracks[t].b : pr.pcb.tracks[t].a) = end;
            if ((done = better())) ++fixed;
            else pr.pcb.tracks[t] = old, trials.reverted();
            break;
        }
        if (!done) break;  // this stub stays: the DRC lists it
    }
    return fixed;
}

int squareEscapes(Project& pr) {
    // The problems the DRC finds; a change is kept only when it adds none (it may only take some away): a fix that
    // trades a pin's fabrication-minimum gap for a design-rule gap to another part's pad is not one.
    auto problems = [&] {
        std::multiset<std::string> n;
        for (const auto& v : pr.pcb.runDRC(pr.schematic))
            if (v.severity != Severity::Info) n.insert(v.code + " " + v.message);
        return n;
    };
    auto noWorse = [](const std::multiset<std::string>& now, const std::multiset<std::string>& was) {
        return std::includes(was.begin(), was.end(), now.begin(), now.end());
    };
    const auto pads = pr.pcb.pads(pr.schematic);
    std::map<int, std::vector<const Pad*>> byPart;
    for (const auto& pad : pads) byPart[pad.componentId].push_back(&pad);
    // Gap from a track's edge to the other-net pads of a part (sampled along it).
    auto gapTo = [&](const Track& t, int part) {
        double best = 1e9;
        for (int k = 0; k <= 40; ++k) {
            const Vec2 q = t.a + (t.b - t.a) * (k / 40.0);
            for (const Pad* o : byPart[part]) {
                if (o->net == t.net || !o->onLayer(t.layer)) continue;
                const Rect r = o->bounds();
                const double dx = std::max({r.x0 - q.x, 0.0, q.x - r.x1}), dy = std::max({r.y0 - q.y, 0.0, q.y - r.y1});
                best = std::min(best, std::hypot(dx, dy) - t.width / 2);
            }
        }
        return best;
    };
    // The fabrication minimum DRC_FINE_PITCH_PADS measures against inside a footprint.
    const double clr = std::min(pr.pcb.settings.minClearance, pr.pcb.settings.clearance);
    std::multiset<std::string> was, now;
    bool counted = false;
    int done = 0;
    PourTrials trials(pr);
    auto noNewProblem = [&] { return noWorse(now = problems(), was); };  // on the held pours, then on refilled ones
    auto keep = [&] { return noNewProblem() && trials.fresh(noNewProblem); };
    for (const Pad& pad : pads) {
        if (byPart[pad.componentId].size() < 8 || pad.round || pad.throughHole) continue;
        const Component* c = pr.schematic.find(pad.componentId);
        const bool tall = pad.size.y > pad.size.x;  // the long axis, pointing away from the part's centre
        const double side = tall ? (pad.position.y >= c->pcb.position.y ? 1 : -1) : (pad.position.x >= c->pcb.position.x ? 1 : -1);
        const Vec2 axis = tall ? Vec2{0, side} : Vec2{side, 0};
        // A track leaving the pad is held to the board's clearance, or to the land pattern's own gap where that is less.
        double need = pr.pcb.settings.clearance;
        for (const Pad* o : byPart[pad.componentId])
            if (o->net != pad.net) need = std::min(need, rectRectDistance(pad.bounds(), o->bounds()) - 0.01);
        need = std::max(need, clr);
        for (size_t i = 0; i < pr.pcb.tracks.size(); ++i) {
            const Track t = pr.pcb.tracks[i];
            if (t.net != pad.net || t.layer != pad.smdLayer || t.arc || t.teardrop || gapTo(t, pad.componentId) >= need - 1e-6) continue;
            const bool aIn = pad.bounds().contains(t.a), bIn = pad.bounds().contains(t.b);
            if (aIn == bIn) continue;
            const Vec2 far = aIn ? t.b : t.a;
            // Out of the pad's end the track heads for: its outer end, or its inner one (under the part's body).
            const Vec2 ax = (far - pad.position).dot(axis) < 0 ? axis * -1 : axis;
            if (!counted) was = problems(), counted = true;
            const auto before = pr.pcb.tracks;
            // Straight out of the pad's end, then on to the far end directly or square (along the pad's axis, then
            // across), the stub longer each time until one is clear.
            bool kept = false;
            // Then the same shapes necked (a narrower lead out of a fine-pitch pin), never below the minimum width.
            for (const double neck : {0.0, 0.02, 0.05})
            for (double ext = 0; ext <= 1.5 + 1e-9 && !kept; ext += 0.25)
                for (int square = 0; square < 2 && !kept; ++square) {
                    if (neck > 0 && t.width - neck < pr.pcb.settings.minTrackWidth - 1e-9) continue;
                    pr.pcb.tracks = before;
                    trials.reverted();
                    const Vec2 o2 = pad.position + ax * ((tall ? pad.size.y : pad.size.x) / 2 + ext);
                    if ((far - o2).dot(ax) < 0) continue;
                    std::vector<Vec2> path{pad.position, o2};
                    if (square) path.push_back(o2 + ax * (far - o2).dot(ax));
                    path.push_back(far);
                    std::vector<Track> segs;
                    for (size_t k = 1; k < path.size(); ++k)
                        if ((path[k] - path[k - 1]).length() > 1e-6) {
                            Track seg = t;
                            seg.a = path[k - 1], seg.b = path[k], seg.id = segs.empty() ? t.id : -1, seg.width = t.width - neck;
                            segs.push_back(seg);
                        }
                    if (segs.size() < 2) continue;
                    pr.pcb.tracks[i] = segs[0];
                    for (size_t k = 1; k < segs.size(); ++k) pr.pcb.addTrack(segs[k]);
                    kept = std::all_of(segs.begin(), segs.end(), [&](const Track& seg) { return gapTo(seg, pad.componentId) >= need - 1e-6; }) && keep();
                    if (kept) was = now, ++done;
                }
            if (!kept) pr.pcb.tracks = before, trials.reverted();
        }
    }
    // A segment still too close to a pin of the part moves away from it in 0.05 mm steps, its free ends (not on a pad
    // or via) taking the tracks joined there along.
    auto anchored = [&](Vec2 v, int net) {
        for (const auto& pad : pads)
            if (pad.net == net && pad.bounds().contains(v)) return true;
        return std::any_of(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& via) { return (via.position - v).length() < 1e-6; });
    };
    std::map<int, double> partNeed;  // the board clearance, or the part's own tightest land gap where that is less
    for (const auto& [part, list] : byPart) {
        double need = pr.pcb.settings.clearance;
        for (const Pad* x : list)
            for (const Pad* y : list)
                if (x != y && x->net != y->net) need = std::min(need, rectRectDistance(x->bounds(), y->bounds()) - 0.01);
        partNeed[part] = std::max(need, clr);
    }
    for (size_t i = 0; i < pr.pcb.tracks.size(); ++i)
        for (const auto& [part, list] : byPart) {
            const Track t = pr.pcb.tracks[i];
            if (list.size() < 8 || t.arc || t.teardrop || gapTo(t, part) >= partNeed[part] - 1e-6) continue;
            Vec2 away{0, 0};  // from the nearest other-net pad toward the segment's middle
            double near = 1e9;
            const Vec2 mid = (t.a + t.b) * 0.5;
            for (const Pad* o : list)
                if (o->net != t.net && (o->position - mid).length() < near) near = (o->position - mid).length(), away = mid - o->position;
            if (away.length() < 1e-9) continue;
            away = away * (1 / away.length());
            const bool moveA = !anchored(t.a, t.net), moveB = !anchored(t.b, t.net);
            if (!moveA && !moveB) continue;
            if (!counted) was = problems(), counted = true;
            const auto before = pr.pcb.tracks;
            bool kept = false;
            for (double d = 0.05; d <= 0.25 + 1e-9 && !kept; d += 0.05) {
                pr.pcb.tracks = before;
                trials.reverted();
                for (auto& o : pr.pcb.tracks) {
                    if (o.net != t.net || o.layer != t.layer) continue;
                    for (Vec2* e : {&o.a, &o.b})
                        if ((moveA && (*e - t.a).length() < 1e-6) || (moveB && (*e - t.b).length() < 1e-6)) *e = *e + away * d;
                }
                if (gapTo(pr.pcb.tracks[i], part) < partNeed[part] - 1e-6) continue;
                if (keep()) was = now, kept = true, ++done;
            }
            if (!kept) pr.pcb.tracks = before, trials.reverted();
        }
    return done;
}

/// A via whose ring overlaps a pad of its net (its centre outside the pad: not a deliberate via-in-pad) draws solder
/// down the barrel: it moves straight out from the pad, its tracks' ends with it, until 0.1 mm of mask separates them.
/// A pour net's via with no room to move goes, with its stub, when the pours still join everything. Each change is kept
/// only when the DRC finds no more problems and nothing comes unrouted. Returns the vias moved or dropped.
int viasOffPads(Project& pr) {
    if (pr.pcb.settings.viaInPad) return 0;
    auto problems = [&] {
        int n = 0;
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info;
        return n;
    };
    const auto pads = pr.pcb.pads(pr.schematic);
    auto gap = [&](const Via& v, const Pad& p) {
        const Rect r = p.bounds();
        const double dx = std::max({r.x0 - v.position.x, 0.0, v.position.x - r.x1}), dy = std::max({r.y0 - v.position.y, 0.0, v.position.y - r.y1});
        return std::hypot(dx, dy) - v.diameter / 2;
    };
    auto worst = [&](const Via& v) {  // the nearest SMD pad of its net, and how far its ring is from it
        std::pair<double, const Pad*> w{1e9, nullptr};
        for (const auto& p : pads)
            if (!p.throughHole && p.net == v.net && !p.bounds().contains(v.position) && gap(v, p) < w.first) w = {gap(v, p), &p};
        return w;
    };
    // A move bringing the via nearer than the clearance to a track or via of another net is a problem the DRC need not
    // be run to find (with the DRC's 1 µm tolerance, and as much again).
    const double clr = pr.pcb.settings.clearance - 2e-3;
    auto clearOfOthers = [&](const Via& v) {
        for (const auto& t : pr.pcb.tracks)
            if (t.net != v.net && v.spans(t.layer) && trackPointDistance(t, v.position) - (t.width + v.diameter) / 2 < clr - 1e-6) return false;
        return std::none_of(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& o) {
            return o.net != v.net && (o.position - v.position).length() - (o.diameter + v.diameter) / 2 < clr - 1e-6;
        });
    };
    int was = -1, moved = 0;
    PourTrials trials(pr);
    for (size_t i = 0; i < pr.pcb.vias.size(); ++i) {
        const auto [g, pad] = worst(pr.pcb.vias[i]);
        if (!pad || g >= 0.1 - 1e-9) continue;
        Vec2 out = pr.pcb.vias[i].position - pad->position;
        if (out.length() < 1e-9) continue;
        out = out * (1 / out.length());
        std::vector<Vec2> dirs{out};  // straight out first, then each compass direction leading away from the pad
        for (int k = 0; k < 8; ++k)
            if (const Vec2 c{std::cos(k * M_PI / 4), std::sin(k * M_PI / 4)}; c.dot(out) > 0.1) dirs.push_back(c);
        if (was < 0) was = problems();
        const auto tracks = pr.pcb.tracks;
        const Vec2 from = pr.pcb.vias[i].position;
        const size_t open = pr.pcb.ratsnest(pr.schematic).size();
        const bool wasClear = clearOfOthers(pr.pcb.vias[i]);
        int now = 0;
        auto noWorse = [&] { return (now = problems()) <= was && pr.pcb.ratsnest(pr.schematic).size() <= open; };
        bool kept = false;
        for (size_t k = 0; k < dirs.size() * 12 && !kept; ++k) {
            const Vec2 dir = dirs[k / 12];
            const double d = 0.05 * static_cast<double>(k % 12 + 1);
            pr.pcb.tracks = tracks;
            pr.pcb.vias[i].position = from;
            trials.reverted();
            pr.pcb.vias[i].position = from + dir * d;
            for (auto& t : pr.pcb.tracks)
                if (t.net == pr.pcb.vias[i].net && !t.arc && !t.teardrop)
                    for (Vec2* e : {&t.a, &t.b})
                        if ((*e - from).length() < 1e-6) *e = pr.pcb.vias[i].position;
            if (worst(pr.pcb.vias[i]).first < 0.1 - 1e-9 || (wasClear && !clearOfOthers(pr.pcb.vias[i]))) continue;
            if (noWorse() && trials.fresh(noWorse)) was = now, kept = true, ++moved;
        }
        if (!kept) pr.pcb.tracks = tracks, pr.pcb.vias[i].position = from, trials.reverted();
        if (kept || !pr.pcb.isZoneNet(pr.schematic, pr.pcb.vias[i].net)) continue;
        // No room to move a pour net's via: dropped with its stub to the pad when the pours still join everything.
        const auto vias = pr.pcb.vias;
        const int net = pr.pcb.vias[i].net;
        auto& ts = pr.pcb.tracks;
        ts.erase(std::remove_if(ts.begin(), ts.end(), [&](const Track& t) {
                     return t.net == net && ((t.a - from).length() < 1e-6 || (t.b - from).length() < 1e-6) &&
                            std::any_of(pads.begin(), pads.end(), [&](const Pad& p) {
                                return p.net == net && (p.bounds().contains(t.a) || p.bounds().contains(t.b));
                            });
                 }),
                 ts.end());
        pr.pcb.vias.erase(pr.pcb.vias.begin() + static_cast<long>(i));
        if (noWorse() && trials.fresh(noWorse)) {
            was = now, ++moved, --i;
            continue;
        }
        pr.pcb.tracks = tracks, pr.pcb.vias = vias, trials.reverted();
    }
    return moved;
}

/// A wide track running into one pad of a small two-pad chip, with little copper on the other pad, is necked down over
/// its first 0.5 mm out of the pad, so both pads heat alike (REL_TOMBSTONE); when the heavy pad's pour spokes alone
/// outweigh the other pad, the light pad's lead-in is widened instead (kept only when the DRC gets no worse). Necks are
/// kept only when no track then falls below its IPC-2221 current width (DRC_TRACK_CURRENT). Returns the parts balanced.
Json neckTombstones(Project& pr) {
    Json done = Json::array();
    const auto pads = pr.pcb.pads(pr.schematic);
    std::set<int> poured;  // nets with a pour: their chip pads count relief spokes, as the reliability check does
    for (const auto& z : pr.pcb.zones)
        for (const auto& n : pr.schematic.nets())
            if (n.name == z.net) poured.insert(n.index);
    auto drcCounts = [&] {  // DRC_TRACK_CURRENT warnings, all errors and warnings
        std::pair<int, int> n{0, 0};
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) n.first += v.code == "DRC_TRACK_CURRENT", n.second += v.severity != Severity::Info;
        return n;
    };
    auto leaving = [&](const Pad& pad) {  // the tracks crossing the pad's edge, as the reliability check counts them
        std::vector<Track*> out;
        for (auto& t : pr.pcb.tracks)
            if (t.net == pad.net && t.layer == pad.smdLayer && pad.bounds().contains(t.a) != pad.bounds().contains(t.b)) out.push_back(&t);
        return out;
    };
    auto mass = [&](const Pad& pad) {
        double m = poured.count(pad.net) ? 2 * pr.pcb.settings.trackWidth : 0;
        for (const Track* t : leaving(pad)) m += t->width;
        return m;
    };
    // The first 0.5 mm of each track leaving the pad (split off) is narrowed (or widened) to w.
    auto setLeadIn = [&](const Pad& pad, double w, bool widen) {
        std::vector<int> ids;
        for (const Track* t : leaving(pad))
            if (!t->arc && !t->teardrop && (widen ? t->width < w : t->width > w + 1e-9)) ids.push_back(t->id);
        for (int tid : ids) {
            const auto it = std::find_if(pr.pcb.tracks.begin(), pr.pcb.tracks.end(), [&](const Track& t) { return t.id == tid; });
            const Vec2 in = pad.bounds().contains(it->a) ? it->a : it->b, far = pad.bounds().contains(it->a) ? it->b : it->a;
            const double len = (far - in).length();
            const Vec2 dir = (far - in) * (1 / len);
            double out = 0;  // where it leaves the pad, then 0.5 mm on
            while (out < len && pad.bounds().contains(in + dir * out)) out += 0.05;
            if (out + 0.5 < len - 0.05) pr.pcb.splitTrack(tid, in + dir * (out + 0.5));
        }
        for (Track* t : leaving(pad))
            if (!t->arc && !t->teardrop && (widen ? t->width < w : t->width > w + 1e-9)) t->width = w;
    };
    auto unbalanced = [&](const Pad& a, const Pad& b) {
        const double hi = std::max(mass(a), mass(b)), lo = std::min(mass(a), mass(b));
        return hi >= 0.6 && hi > 3 * std::max(lo, 0.05);
    };
    const auto before = pr.pcb.tracks;
    const auto [warned, errors] = drcCounts();
    std::map<int, std::vector<const Pad*>> byPart;
    for (const auto& pad : pads) byPart[pad.componentId].push_back(&pad);
    std::vector<std::pair<int, std::array<const Pad*, 2>>> parts;
    for (const auto& [id, two] : byPart)
        if (two.size() == 2 && !two[0]->throughHole && !two[1]->throughHole && two[0]->size.x * two[0]->size.y <= 4.0 &&
            unbalanced(*two[0], *two[1]))
            parts.push_back({id, {two[0], two[1]}});
    for (const auto& [id, two] : parts) {  // neck the heavy pad's tracks down to three times the light pad's copper
        const int heavy = mass(*two[0]) >= mass(*two[1]) ? 0 : 1;
        const Pad& h = *two[heavy];
        const auto wide = leaving(h);
        const double spokes = poured.count(h.net) ? 2 * pr.pcb.settings.trackWidth : 0;
        if (!wide.empty())
            setLeadIn(h, std::max(pr.pcb.settings.minTrackWidth,
                                  (3 * std::max(mass(*two[1 - heavy]), 0.05) - spokes) / static_cast<double>(wide.size()) - 0.01), false);
    }
    if (!parts.empty() && drcCounts().first > warned) pr.pcb.tracks = before;  // a neck too thin for the current: leave the copper as routed
    for (const auto& [id, two] : parts) {  // still unbalanced (spokes, the minimum width): widen the light pad's lead-in
        if (!unbalanced(*two[0], *two[1])) continue;
        const int light = mass(*two[0]) < mass(*two[1]) ? 0 : 1;
        const Pad& l = *two[light];
        const auto thin = leaving(l);
        const double spokes = poured.count(l.net) ? 2 * pr.pcb.settings.trackWidth : 0;
        const double w = (std::max(mass(*two[0]), mass(*two[1])) / 3 + 0.02 - spokes) / static_cast<double>(thin.size());
        if (thin.empty() || w > std::min(l.size.x, l.size.y)) continue;
        const auto kept = pr.pcb.tracks;
        setLeadIn(l, w, true);
        if (drcCounts().second > errors) pr.pcb.tracks = kept;
    }
    for (const auto& [id, two] : parts)
        if (!unbalanced(*two[0], *two[1])) done.push(pr.schematic.find(id)->ref);
    return done;
}

/// What a whole-board autoroute leaves unrouted on a small board gets a second try, each kept only when fewer
/// connections stay open: the shape-based engine, then each two-pin part on an open connection turned round (its far
/// pad faced the other end, so its own other pad stood in the way). Returns what was changed.
Json finishRouting(SiedaProject* p, Json* stats) {
    Json done = Json::array();
    const AutorouteOptions chosen = p->project.pcb.settings.autorouter;
    if (chosen.scoped() || chosen.fanoutOnly || p->project.schematic.components().size() > 300)
        return done;  // a partial route keeps the rest of the board as it is
    size_t open = p->project.pcb.ratsnest(p->project.schematic).size();
    auto attempt = [&](const std::string& what, const std::function<void()>& change, bool clear = true) {
        if (!open) return;
        Project before = p->project;
        change();
        if (clear) sieda_pcb_clear_routing(p);
        Json st = takeJson(sieda_pcb_autoroute(p));
        const size_t now = p->project.pcb.ratsnest(p->project.schematic).size();
        if (now < open) {
            open = now;
            *stats = st;
            done.push(what);
        } else {
            p->project = std::move(before);
        }
    };
    if (!chosen.shapeCompletion)
        attempt("shape-based completion", [&] { p->project.pcb.settings.autorouter.shapeCompletion = true; });
    std::set<int> tried;
    const auto pads = p->project.pcb.pads(p->project.schematic);
    for (const auto& [a, b] : p->project.pcb.ratsnest(p->project.schematic))
        for (Vec2 end : {a, b})
            for (const auto& pad : pads) {
                const Component* c = p->project.schematic.find(pad.componentId);
                if (std::hypot(pad.position.x - end.x, pad.position.y - end.y) > 1e-3 || !c || c->pcb.locked ||
                    c->def().pins.size() != 2 || !tried.insert(c->id).second || tried.size() > 8)
                    continue;
                const int id = c->id;
                attempt(c->ref + " turned 180°", [&] {
                    Component* m = p->project.schematic.find(id);
                    m->pcb.rotation = (m->pcb.rotation + 180) % 360;
                });
            }
    // A fine-pitch part (pad pitch under 0.65 mm) on an open connection: its pins fanned out first (escape track and
    // via, held while the rest routes), so later nets cannot wall them in.
    std::set<int> fanned;
    for (const auto& [a, b] : p->project.pcb.ratsnest(p->project.schematic))
        for (Vec2 end : {a, b})
            for (const auto& pad : pads) {
                if (std::hypot(pad.position.x - end.x, pad.position.y - end.y) > 1e-3 || !fanned.insert(pad.componentId).second)
                    continue;
                const Component* c = p->project.schematic.find(pad.componentId);
                const FootprintDef* fp = c ? Library::instance().footprint(c->footprintName()) : nullptr;
                double pitch = 1e9;
                for (size_t i = 0; fp && i < fp->pads.size(); ++i)
                    for (size_t k = i + 1; k < fp->pads.size(); ++k)
                        pitch = std::min(pitch, (fp->pads[i].offset - fp->pads[k].offset).length());
                if (pitch >= 0.65) continue;
                const int id = c->id;
                for (const bool shove : {false, true})  // walk around at the pad's edge; shoving, 1.4 mm out
                    attempt(c->ref + " fanned out first" + (shove ? " (shoving)" : ""), [&] {
                        sieda_pcb_clear_routing(p);
                        FanoutOptions fo;
                        fo.onlyUnrouted = false;
                        fo.shove = shove;
                        fo.distance = shove ? 1.4 : 0;
                        fanoutComponent(p->project.pcb, p->project.schematic, id, fo);
                        for (auto& t : p->project.pcb.tracks) t.locked = true;
                        p->project.pcb.settings.autorouter.protectLocked = true;
                    }, false);
                for (auto& t : p->project.pcb.tracks) t.locked = false;  // the stubs are ordinary copper again
                p->project.pcb.settings.autorouter.protectLocked = chosen.protectLocked;
            }
    // A pour-net pad still cut off: a short track to a via beside it, into the net's pour on the other layer (kept when
    // the connection closes and the DRC gets no worse).
    Project& pr = p->project;
    auto problems = [&] {
        int n = 0;
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info && v.code != "DRC_UNROUTED";
        return n;
    };
    auto rats = pr.pcb.ratsnest(pr.schematic);
    for (const auto& pad : rats.empty() ? std::vector<Pad>{} : pr.pcb.pads(pr.schematic)) {
        if (pad.throughHole || !pr.pcb.isZoneNet(pr.schematic, pad.net) ||
            std::none_of(rats.begin(), rats.end(), [&](const auto& r) { return (r.first - pad.position).length() < 1e-3 || (r.second - pad.position).length() < 1e-3; }))
            continue;
        const int worst = problems();
        const double r0 = std::max(pad.size.x, pad.size.y) / 2 + pr.pcb.settings.clearance + pr.pcb.settings.viaDiameter / 2;
        bool joined = false;
        for (double r : {r0, r0 + 0.5})
            for (int k = 0; k < 8 && !joined; ++k) {
                const Project before = pr;
                Via v;
                v.net = pad.net, v.diameter = pr.pcb.settings.viaDiameter, v.drill = pr.pcb.settings.viaDrill;
                v.position = pad.position + Vec2{std::cos(k * M_PI / 4), std::sin(k * M_PI / 4)} * r;
                Track t;
                t.net = pad.net, t.layer = pad.smdLayer, t.width = pr.pcb.settings.trackWidth, t.a = pad.position, t.b = v.position;
                if (!pr.pcb.settings.contains(v.position)) continue;
                pr.pcb.addVia(v), pr.pcb.addTrack(t);
                joined = pr.pcb.ratsnest(pr.schematic).size() < rats.size() && problems() <= worst;
                if (!joined) pr = before;
            }
        if (joined) done.push(pr.schematic.find(pad.componentId)->ref + " stitched to the pour by a via"), rats = pr.pcb.ratsnest(pr.schematic);
    }
    p->project.pcb.settings.autorouter.shapeCompletion = chosen.shapeCompletion;  // the board's strategy as chosen
    p->router.reset();
    return done;
}

/// "Packages: R_0402, R_0603 …" for an error that names a package the part does not come in.
/// Sets a part's package variant; a bare size ("0603") picks the one variant ending in it (R_0603 for a resistor).
bool setPackage(SiedaProject* p, int id, const std::string& name) {
    if (sieda_set_component_package(p, id, name.c_str()) == 1) return true;
    std::string only;
    for (const auto& v : Library::packageVariants(p->project.schematic.find(id)->kind))
        if (!name.empty() && v.size() > name.size() && v[v.size() - name.size() - 1] == '_' &&
            compact(v.substr(v.size() - name.size())) == compact(name))
            only = only.empty() ? v : "~";
    return !only.empty() && only != "~" && sieda_set_component_package(p, id, only.c_str()) == 1;
}

std::string packageChoices(ComponentKind kind) {
    std::string list;
    for (const auto& v : Library::packageVariants(kind)) list += (list.empty() ? "" : ", ") + v;
    return list.empty() ? "This kind has one package." : "Packages: " + list + ".";
}

/// Decoupling capacitors (one pin on an IC's supply net, the other on ground) moved next to the IC supply pin they
/// serve, one capacitor per pin in pin order: one further than 5 mm goes to the nearest legal spot 2.5–6 mm from the
/// pin (room left for the pins' escape routes). Locked parts stay. Returns "C3 → U1.VDD" for each one moved.
bool hasAntenna(const Component& c) {
    const CustomPart* cp = c.kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
    std::string text = cp ? cp->spec.description : "";
    for (auto& ch : text) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return text.find("antenna") != std::string::npos;
}

/// Assembly spacing between parts on one side, courtyard to courtyard (mm), on top of the IPC-7351B courtyards
/// (land pattern + 0.25 mm): 0.25 between passives (chip R / C / L, SOD, SOT-23, SMD crystals), 0.5 between a passive and an IC,
/// 1.0 between ICs and between a through-hole part and an SMD one, 0.5 between through-hole parts, 2.0 round a BGA
/// (rework nozzle). Typical assembly-house practice; the courtyards themselves never overlap.
enum class PartClass { Passive, Ic, ThroughHole, Bga };
const char* const kPartClassName[] = {"passive", "IC", "through-hole part", "BGA"};

std::map<int, PartClass> partClasses(const Project& pr) {
    std::map<int, PartClass> out;
    for (const auto& c : pr.schematic.components()) {
        if (!c.hasFootprint() || isNetSymbolKind(c.kind) || !pr.pcb.onBoard(c)) continue;
        std::string fp = c.footprintName();
        for (auto& ch : fp) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        const auto pads = pr.checkPlacement(c.id, c.pcb.position, c.pcb.rotation, c.pcb.bottom, 0).pads;  // placed or not
        const size_t n = pads.size();
        const bool tht = std::any_of(pads.begin(), pads.end(), [](const Pad& pad) { return pad.throughHole; });
        const bool crystal = fp.find("CRYSTAL") != std::string::npos || (!c.ref.empty() && c.ref[0] == 'Y');  // sits by its pins
        out[c.id] = fp.find("BGA") != std::string::npos ? PartClass::Bga : tht ? PartClass::ThroughHole
                    : n <= 3 || crystal ? PartClass::Passive : PartClass::Ic;
    }
    return out;
}

double standardGap(PartClass a, PartClass b) {
    if (a == PartClass::Bga || b == PartClass::Bga) return 2.0;
    if (a == PartClass::ThroughHole || b == PartClass::ThroughHole) return a == b ? 0.5 : 1.0;
    if (a == b) return a == PartClass::Ic ? 1.0 : 0.25;
    return 0.5;
}

double rectGap(const Rect& a, const Rect& b) {
    return std::hypot(std::max(0.0, std::max(a.x0 - b.x1, b.x0 - a.x1)), std::max(0.0, std::max(a.y0 - b.y1, b.y0 - a.y1)));
}

/// The placed parts (same side) part `id` would stand closer to than the standard with courtyard `cy`.
std::vector<std::pair<const Component*, double>> crowding(const Project& pr, const std::map<int, PartClass>& classes, int id,
                                                           const Rect& cy, bool bottom) {
    std::vector<std::pair<const Component*, double>> out;
    const auto mine = classes.find(id);
    if (mine == classes.end()) return out;
    for (const auto& o : pr.schematic.components()) {
        const auto theirs = classes.find(o.id);
        if (o.id == id || theirs == classes.end() || !o.pcb.placed || o.pcb.bottom != bottom || !pr.pcb.onBoard(o)) continue;
        if (rectGap(cy, pr.pcb.courtyard(o)) < standardGap(mine->second, theirs->second) - 1e-6) out.push_back({&o, rectGap(cy, pr.pcb.courtyard(o))});
    }
    return out;
}

/// A pose keeping the standard spacing (the placement check still decides whether it is legal at all).
bool spaced(const Project& pr, const std::map<int, PartClass>& classes, int id, Vec2 at, int rotation, bool bottom) {
    return crowding(pr, classes, id, pr.checkPlacement(id, at, rotation, bottom, 0.25).courtyard, bottom).empty();
}

/// The pour keep-out round an antenna end (openEnd's `area` on `side`): its own width and 15 mm (at least its depth)
/// on either side, as module makers draw it (copper beside the antenna detunes it too), clipped to the board.
Rect antennaKeepout(Rect area, int side, const BoardSettings& st) {
    const double depth = std::max(15.0, side < 2 ? area.height() : area.width());
    if (side < 2) area.x0 -= depth, area.x1 += depth;
    else area.y0 -= depth, area.y1 += depth;
    area.x0 = std::max(0.0, area.x0), area.y0 = std::max(0.0, area.y0);
    area.x1 = std::min(st.width, area.x1), area.y1 = std::min(st.height, area.y1);
    return area;
}

/// The under-module keep-out "under REF" cut back past each of the module's pads a track must reach inside it: one on
/// a signal net, and (once the pours are known, `zonesKnown`) one whose net has no pour on the keep-out's layer; 1 mm more
/// on a board with a differential pair, so the coupled pair fits.
void carveUnder(const Project& pr, RouteKeepout& k, bool zonesKnown) {
    const BoardSettings& st = pr.pcb.settings;
    double reach = 0;
    std::vector<Pad> pads;
    for (const auto& pad : pr.pcb.pads(pr.schematic))
        if (const Component* c = pr.schematic.find(pad.componentId); c && "under " + c->ref == k.name)
            pads.push_back(pad), reach = std::max(reach, std::max(pad.size.x, pad.size.y) / 2);
    const auto paired = differentialPairs(pr.schematic);
    const double m = reach + st.trackWidth + st.clearance + (paired.empty() ? 0.0 : 1.0);
    for (const auto& pad : pads) {
        if (pad.net < 0) continue;
        bool poured = false;
        for (const auto& z : pr.pcb.zones)
            poured = poured || (z.layer == k.layer && z.net == pr.schematic.nets()[static_cast<size_t>(pad.net)].name);
        if (pr.schematic.netRole(pad.net) != NetRole::Signal && (!zonesKnown || poured)) continue;
        Rect& a = k.area;
        const Vec2 q = pad.position;
        if (q.x < a.x0 - m || q.x > a.x1 + m || q.y < a.y0 - m || q.y > a.y1 + m) continue;
        const double cut[4] = {q.x + m - a.x0, a.x1 - (q.x - m), q.y + m - a.y0, a.y1 - (q.y - m)};
        const int side = static_cast<int>(std::min_element(cut, cut + 4) - cut);
        (side == 0 ? a.x0 : side == 1 ? a.x1 : side == 2 ? a.y0 : a.y1) = side == 0 ? q.x + m : side == 1 ? q.x - m : side == 2 ? q.y + m : q.y - m;
    }
}

/// Auto Place's edge pass (before the others are placed): a part with an antenna goes to the top edge with its
/// antenna end outwards and a keep-out (tracks, vias, pours) over that end; a USB / connector part (J…, P…, CN…,
/// USB…) with an open end (its mouth) goes to the bottom edge facing out, one without (a pin header) to the left edge
/// when it carries power only (an input) and to the right edge otherwise (the other one when it has no room; filled
/// from the top when neither has room from the middle), its long side along the edge. Each slides along its edge to the first free spot from the middle. Returns what moved
/// and the ids placed (held while the rest is placed).
Json placeAtEdges(SiedaProject* p, std::vector<int>* held) {
    Json moved = Json::array();
    Project& pr = p->project;
    const BoardSettings& st = pr.pcb.settings;
    if (st.hasCustomOutline()) return moved;  // a rectangle only
    const auto classes = partClasses(pr);
    std::vector<std::pair<double, int>> order;  // the largest first: a long header takes an edge's middle, short ones fit beside
    for (const auto& c : pr.schematic.components())
        if (c.hasFootprint() && !c.pcb.locked && !isNetSymbolKind(c.kind) && pr.pcb.onBoard(c)) {
            const Rect cy = pr.checkPlacement(c.id, {st.width / 2, st.height / 2}, 0, c.pcb.bottom, 0).courtyard;
            order.push_back({-std::max(cy.width(), cy.height()), c.id});
        }
    std::stable_sort(order.begin(), order.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
    for (const auto& [size, id] : order) {
        const Component& c = *pr.schematic.find(id);
        const std::string prefix = c.ref.substr(0, c.ref.find_first_of("0123456789"));
        const bool antenna = hasAntenna(c);
        if (!antenna && prefix != "J" && prefix != "P" && prefix != "CN" && prefix != "USB") continue;
        const int edge = antenna ? 0 : 1;  // top / bottom
        int rotation = -1;
        Rect cy;
        for (int rot : {0, 90, 180, 270}) {  // the turn that faces its open end out over that edge
            const PlacementCheck k = pr.checkPlacement(c.id, {st.width / 2, st.height / 2}, rot, c.pcb.bottom, 0);
            double gap = 0;
            Rect area;
            if (openEnd(k.courtyard, k.pads, &gap, &area) == edge && gap >= 0.5) {
                rotation = rot;
                cy = k.courtyard;
                break;
            }
        }
        const bool side = rotation < 0;  // no open end (a pin header): along the left / right edge, in turn
        if (side && antenna) continue;
        bool power = side;  // pins on supplies / ground only: a power input, at the left edge; signal headers right
        for (int i = 0; i < static_cast<int>(c.def().pins.size()) && power; ++i)
            if (const int n = pr.schematic.netOf({c.id, i}); n >= 0 && pr.schematic.netRole(n) == NetRole::Signal) power = false;
        bool right = side && !power;
        if (side)
            for (int rot : {c.pcb.rotation, (c.pcb.rotation + 90) % 360}) {  // its long side along the edge
                rotation = rot;
                cy = pr.checkPlacement(c.id, {st.width / 2, st.height / 2}, rot, c.pcb.bottom, 0).courtyard;
                if (cy.height() >= cy.width()) break;
            }
        // The antenna end at the edge; a connector's mouth flush with the bottom edge (its pads are behind it), else
        // as close as the placement check allows.
        bool done = false;
        double inset = 0;
        for (double tryInset : {0.0, st.edgeClearance, st.edgeClearance + 1})
        // A header with no room from the middle out: the other side, then each side filled from the top.
        for (int attempt = 0; attempt < (side ? 4 : 1) && !done && (!side || tryInset == 0); ++attempt)
        for (int k = 0; k < 4 * static_cast<int>(std::max(st.width, st.height)) && !done; ++k) {
            inset = tryInset;
            const double y0 = edge == 0 ? st.edgeClearance + cy.height() / 2 : st.height - inset - cy.height() / 2;
            if (attempt && !k) right = !right;
            const double x0 = right ? st.width - st.edgeClearance - cy.width() / 2 - 1 : st.edgeClearance + cy.width() / 2 + 1;
            const double step = (k % 2 ? 1 : -1) * 0.5 * ((k + 1) / 2);
            const double x = side ? x0 : st.width / 2 + step,
                         y = !side ? (edge == 1 ? std::floor(y0 / 0.25) : std::ceil(y0 / 0.25)) * 0.25  // on the 0.25 grid, inside
                                   : attempt < 2 ? st.height / 2 + step : st.edgeClearance + cy.height() / 2 + 1 + 0.5 * k;
            if (!spaced(pr, classes, c.id, {x, y}, rotation, c.pcb.bottom) ||
                !takeJson(sieda_pcb_place_footprint(p, c.id, x, y, rotation, c.pcb.bottom ? 1 : 0, 0.25, 0)).get("committed").asBool(false))
                continue;
            held->push_back(c.id);
            moved.push(c.ref + " → " + (right ? "right" : side ? "left" : edge == 0 ? "top" : "bottom") + " edge" +
                      (edge == 1 && !side ? (inset > 0 ? " (" + Json(inset).dump() + " mm in)" : std::string(" (mouth at the edge)")) : ""));
            if (antenna) {  // copper kept out from under the antenna
                const Component& m = *pr.schematic.find(c.id);
                std::vector<Pad> pads;
                for (const auto& pad : pr.pcb.pads(pr.schematic))
                    if (pad.componentId == m.id) pads.push_back(pad);
                double gap = 0;
                Rect area;
                const int end = openEnd(pr.pcb.courtyard(m), pads, &gap, &area);
                RouteKeepout kp;
                kp.name = "antenna " + m.ref;
                kp.area = antennaKeepout(area, end, st);
                kp.pours = true;
                auto& kos = pr.pcb.settings.keepouts;
                kos.erase(std::remove_if(kos.begin(), kos.end(), [&](const RouteKeepout& o) { return o.name == kp.name; }), kos.end());
                kos.push_back(kp);
                // No top-layer tracks under the module between its pad ring (its ground pads join through the pour).
                Rect ring;  // the pads' centres' extent (the constructor would order a reversed start)
                ring.x0 = ring.y0 = 1e9, ring.x1 = ring.y1 = -1e9;
                double reach = 0;
                for (const auto& pad : pads) {
                    ring.x0 = std::min(ring.x0, pad.position.x), ring.y0 = std::min(ring.y0, pad.position.y);
                    ring.x1 = std::max(ring.x1, pad.position.x), ring.y1 = std::max(ring.y1, pad.position.y);
                    reach = std::max(reach, std::max(pad.size.x, pad.size.y) / 2);
                }
                reach += st.trackWidth;  // room for a track leaving a pad of the ring along it
                RouteKeepout under;
                under.name = "under " + m.ref;
                under.layer = kTopLayer;
                under.vias = false;
                under.area = {ring.x0 + reach + st.clearance, ring.y0 + reach + st.clearance, ring.x1 - reach - st.clearance,
                              ring.y1 - reach - st.clearance};
                carveUnder(pr, under, false);
                kos.erase(std::remove_if(kos.begin(), kos.end(), [&](const RouteKeepout& o) { return o.name == under.name; }), kos.end());
                if (under.area.width() > 1 && under.area.height() > 1) kos.push_back(under);
            }
            done = true;
        }
    }
    return moved;
}

/// Via stitching (pcb_stitch_vias): the core's grid of vias, less those under a part's courtyard.
/// Refuses (ToolError) a hand-drawn segment a–b of `width` on `layer` (-1 = a via, every layer) that would come nearer
/// than the board clearance to copper of another net: pads, tracks, vias.
void checkClearOfOtherNets(const Project& pr, int net, Vec2 a, Vec2 b, double width, int layer) {
    const double clr = pr.pcb.settings.clearance;
    Track seg;
    seg.a = a, seg.b = b, seg.width = width, seg.layer = std::max(0, layer);
    auto fail = [&](const std::string& what) { throw ToolError("Too close to " + what + " (board clearance " + Json(clr).dump() + " mm)"); };
    for (const auto& pad : pr.pcb.pads(pr.schematic)) {
        if (pad.net == net || (layer >= 0 && !pad.onLayer(layer))) continue;
        const double d = pad.round ? roundPadTrackDistance(pad, seg) - padRadius(pad)
                                   : trackRectDistance(seg, pad.bounds());
        if (d - width / 2 < clr - 1e-9) {
            const Component* c = pr.schematic.find(pad.componentId);
            fail("pad " + (c ? c->ref : std::string("?")) + "." + std::to_string(pad.padNumber));
        }
    }
    for (const auto& t : pr.pcb.tracks)
        if (t.net != net && (layer < 0 || t.layer == layer) && trackTrackDistance(seg, t) - width / 2 - t.width / 2 < clr - 1e-9)
            fail("a track of another net (id " + std::to_string(t.id) + ")");
    for (const auto& v : pr.pcb.vias)
        if (v.net != net && (layer < 0 || v.spans(layer)) && trackPointDistance(seg, v.position) - width / 2 - v.diameter / 2 < clr - 1e-9)
            fail("a via of another net (id " + std::to_string(v.id) + ")");
}

// A wire link (WIRELINK part, refPrefix JW) from a to b on the schematic, both pins labelled with `net`; an unnamed
// net (empty or N$…) is first named JW_NET<serial> by labelling `anchorPin`, and `net` comes back with that name. The
// part is placed on the board by placeWireLink after an Update PCB.
int addWireLink(McpServer& s, Vec2 a, Vec2 b, std::string& net, const std::string& anchorPin, int serial,
                bool insulated = false) {
    SiedaProject* p = P(s);
    Project& pr = p->project;
    // On the bottom the part is mirrored: a link falling to the right (both offsets one sign) is drawn with its
    // offsets swapped and placed a quarter turn round.
    const bool swapped = (b.x - a.x) * (b.y - a.y) > 0;
    const double dx = std::fabs(swapped ? b.y - a.y : b.x - a.x), dy = std::fabs(swapped ? b.x - a.x : b.y - a.y);
    auto mm = [](double v) {
        const long um = std::lround(v * 100);
        return std::to_string(um / 100) + "." + (um % 100 < 10 ? "0" : "") + std::to_string(um % 100);
    };
    const bool diagonal = dx > 0.01 && dy > 0.01;
    const std::string size = diagonal ? mm(dx) + "x" + mm(dy) : mm(std::max(dx, dy));
    const std::string partName = (insulated ? "WIRE-JUMPER-" : "WIRE-LINK-") + size;
    std::string partId;
    for (const auto& id : pr.customLibrary)
        if (const CustomPart* cp = CustomPartRegistry::instance().find(id); cp && cp->spec.name == partName) partId = id;
    if (partId.empty()) {
        Json spec = obj(), pkg = obj(), pins = Json::array();
        for (int k = 1; k <= 2; ++k) {
            Json pin = obj();
            pin["number"] = std::to_string(k), pin["name"] = std::to_string(k), pin["type"] = "passive";
            pins.push(pin);
        }
        pkg["type"] = "WIRELINK", pkg["pinCount"] = 2, pkg["bodySize"] = diagonal ? dx : std::max(dx, dy);
        if (diagonal) pkg["bodyDepth"] = dy;
        spec["name"] = partName, spec["refPrefix"] = "JW", spec["defaultValue"] = "Wire link",
        spec["description"] = (insulated ? "Insulated jumper wire (0.6 mm, formed round the parts below it), "
                                         : "Wire link (jumper) of 0.6 mm tinned copper wire, ") + size +
                              " mm: joins copper a single-sided board cannot";
        spec["package"] = pkg, spec["pins"] = pins;
        char* err = nullptr;
        char* res = sieda_custom_part_register(p, spec.dump().c_str(), &err);
        if (!res) throw ToolError("Could not make the wire link part: " + takeText(err));
        partId = takeJson(res).get("id").asString("");
    }
    auto label = [&](const std::string& pin) {
        Json args = obj(), list = Json::array();
        list.push(pin);
        args["net"] = net, args["pins"] = list;
        for (const auto& t : mcpTools())
            if (t.name == "schematic_net_label") t.handler(s, args);
    };
    if (net.empty() || net.rfind("N$", 0) == 0) {
        auto taken = [&](const std::string& n) {
            return std::any_of(pr.schematic.nets().begin(), pr.schematic.nets().end(), [&](const Net& x) { return x.name == n; });
        };
        net = "JW_NET" + std::to_string(serial);
        for (int k = 2; taken(net); ++k) net = "JW_NET" + std::to_string(serial) + "_" + std::to_string(k);
        label(anchorPin);
    }
    const Vec2 at = freeSpot(pr);
    const int id = sieda_add_custom_component(p, partId.c_str(), "Wire link", at.x, at.y, 0, nullptr);
    check(id >= 0, "Could not add a wire link");
    const std::string ref = pr.schematic.find(id)->ref;
    label(ref + ".1"), label(ref + ".2");
    return id;
}

// Places a wire link on the bottom (component side) with its holes on a and b: the quarter turn that puts them there.
// Refused (ToolError) when none does.
void placeWireLink(SiedaProject* p, int id, Vec2 a, Vec2 b) {
    const Vec2 mid = (a + b) * 0.5;
    for (int rot : {0, 90, 180, 270}) {
        takeJson(sieda_pcb_place_footprint(p, id, mid.x, mid.y, rot, 1, 0, 1));
        bool fits = true;
        for (const Pad& pad : p->project.pcb.pads(p->project.schematic))
            if (pad.componentId == id) fits = fits && std::min((pad.position - a).length(), (pad.position - b).length()) < 0.01;
        if (fits) return;
    }
    throw ToolError("Could not place wire link " + p->project.schematic.find(id)->ref + " on its holes");
}

// Wire links (pcb_add_jumpers, single-sided boards): a net the copper layer leaves in islands is joined by a wire link
// soldered from the component side, a real part (JW1 …: schematic, BOM, both pins labelled with the net). For each
// split net the shortest link along x or y (7.62–50.8 mm in 2.54 mm steps) from one island's copper to another's,
// with a short stub track at either end, clear of other nets' copper by their class clearance, of holes, keep-outs,
// the edge and every courtyard (its body of the bottom side's). Returns the refs added.
Json addJumpers(McpServer& s, int maxLinks) {
    SiedaProject* p = P(s);
    Project& pr = p->project;
    const BoardSettings& st = pr.pcb.settings;
    Json added = Json::array();
    std::set<std::pair<long, long>> refused;  // jumper ends (0.01 mm) that did not join
    auto key = [](Vec2 v) { return std::make_pair(std::lround(v.x * 100), std::lround(v.y * 100)); };
    for (int round = 0; round < maxLinks; ++round) {
        const std::vector<Pad> pads = pr.pcb.pads(pr.schematic);
        const std::vector<size_t> groups = pr.pcb.padGroups(pr.schematic);
        std::map<int, std::string> names;
        for (const Net& n : pr.schematic.nets()) names[n.index] = n.name;
        auto nameOf = [&](int n) { return names.count(n) ? names[n] : std::string(); };
        // Tracks join the island of a pad they touch (through other tracks too).
        std::vector<int> owner(pr.pcb.tracks.size(), -1);  // pad index
        for (bool grew = true; grew;) {
            grew = false;
            for (size_t t = 0; t < pr.pcb.tracks.size(); ++t) {
                if (owner[t] >= 0) continue;
                const Track& tr = pr.pcb.tracks[t];
                for (size_t i = 0; i < pads.size() && owner[t] < 0; ++i)
                    if (pads[i].net >= 0 && pads[i].onLayer(tr.layer) && padTrackDistance(pads[i], tr) <= tr.width / 2 + 1e-6)
                        owner[t] = static_cast<int>(i);
                for (size_t u = 0; u < pr.pcb.tracks.size() && owner[t] < 0; ++u)
                    if (owner[u] >= 0 && pr.pcb.tracks[u].layer == tr.layer &&
                        trackTrackDistance(tr, pr.pcb.tracks[u]) <= (tr.width + pr.pcb.tracks[u].width) / 2 + 1e-6)
                        owner[t] = owner[u];
                grew = grew || owner[t] >= 0;
            }
        }
        struct Spot {
            Vec2 at;
            int pad;     // pad index of its island
            Vec2 stub;   // the copper the stub runs to (= at: none)
        };
        std::map<int, std::vector<Spot>> spots;  // net → attach points
        std::map<int, std::set<size_t>> islands;
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].net >= 0 && !pads[i].npth) islands[pads[i].net].insert(groups[i]);
        for (size_t i = 0; i < pads.size(); ++i) {
            if (pads[i].net < 0 || pads[i].npth || islands[pads[i].net].size() < 2) continue;
            for (double ring : {2.5, 4.0, 5.5, 7.0}) {  // the link's hole out past the courtyard, a stub back to the pad
                const double r = std::max(pads[i].size.x, pads[i].size.y) / 2 + ring;
                for (int k = 0; k < 16; ++k)
                    spots[pads[i].net].push_back({pads[i].position + Vec2{std::cos(k * M_PI / 8), std::sin(k * M_PI / 8)} * r,
                                                  static_cast<int>(i), pads[i].position});
            }
        }
        for (size_t t = 0; t < pr.pcb.tracks.size(); ++t) {
            const Track& tr = pr.pcb.tracks[t];
            if (owner[t] < 0 || tr.arc || islands[pads[owner[t]].net].size() < 2) continue;
            const int n = static_cast<int>(std::ceil((tr.b - tr.a).length())) + 1;
            for (int k = 0; k <= n; ++k) {
                const Vec2 q = tr.a + (tr.b - tr.a) * (static_cast<double>(k) / n);
                spots[pads[owner[t]].net].push_back({q, owner[t], q});
            }
        }
        // Clear of other nets' copper, holes, keep-outs and the edge: a segment a–b of `width` (a pad: a == b).
        auto clear = [&](int net, Vec2 a, Vec2 b, double width, bool hole) {
            const double own = st.clearanceFor(nameOf(net));
            for (const Pad& pad : pads)
                if (pad.net != net && padSegmentDistance(pad, a, b) - width / 2 <
                                          std::max(own, pad.net >= 0 ? st.clearanceFor(nameOf(pad.net)) : st.clearance))
                    return false;
            for (size_t t = 0; t < pr.pcb.tracks.size(); ++t) {
                const Track& u = pr.pcb.tracks[t];
                const int other = owner[t] >= 0 ? pads[owner[t]].net : u.net;
                if (other != net && (hole || u.layer == kTopLayer) &&
                    trackSegmentDistance(u, a, b) - width / 2 - u.width / 2 < std::max(own, st.clearanceFor(nameOf(other))))
                    return false;
            }
            for (const Via& v : pr.pcb.vias)
                if (v.net != net && pointSegmentDistance(v.position, a, b) - width / 2 - v.diameter / 2 < own) return false;
            for (const MountingHole& h : st.holes)
                if (pointSegmentDistance(h.position, a, b) - width / 2 < std::max(h.keepout, h.drill) / 2) return false;
            for (const RouteKeepout& k : st.keepouts)
                if ((k.tracks || k.barrier) && k.area.inflated(width / 2).intersects(Rect{std::min(a.x, b.x), std::min(a.y, b.y),
                                                                                         std::max(a.x, b.x), std::max(a.y, b.y)}))
                    return false;
            return st.segmentEdgeDistance(a, b) - width / 2 >= st.edgeClearance;
        };
        // The wire lies on the bottom (component) side: clear of the bodies there; its holes clear of those bodies and of
        // the top side's courtyards (SMD parts on the copper side).
        std::vector<Rect> bodies, noHoles;
        for (const auto& c : pr.schematic.components())
            if (const FootprintDef* fp = c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c) ? Library::instance().footprint(c.footprintName()) : nullptr) {
                if (!c.pcb.bottom) {
                    noHoles.push_back(pr.pcb.courtyard(c));
                    continue;
                }
                const bool turned = c.pcb.rotation % 180 != 0;
                const Rect body = Rect::centered(c.pcb.position, turned ? fp->body.depth : fp->body.width,
                                                 turned ? fp->body.width : fp->body.depth).inflated(0.3);
                bodies.push_back(body), noHoles.push_back(body);
            }
        constexpr double padD = 2.0;
        struct Link {
            double cost;
            int net;
            Spot from, to;
            Vec2 a, b;
        };
        std::optional<Link> best;
        for (const auto& [net, list] : spots) {
            const double w = st.widthFor(nameOf(net));
            std::vector<Link> cands;
            for (const Spot& f : list)
                for (int d = 0; d < 4; ++d) {
                    const Vec2 dir = d == 0 ? Vec2{1, 0} : d == 1 ? Vec2{-1, 0} : d == 2 ? Vec2{0, 1} : Vec2{0, -1};
                    for (int steps = 3; steps <= 20; ++steps) {
                        const Vec2 b = f.at + dir * (steps * 2.54);
                        const Spot* near = nullptr;
                        double nd = 4.0;
                        for (const Spot& g : list)
                            if (groups[g.pad] != groups[f.pad] && (g.at - b).length() < nd) nd = (g.at - b).length(), near = &g;
                        if (near) cands.push_back({steps * 2.54 + nd + (f.stub - f.at).length() + (near->stub - near->at).length(),
                                                   net, f, *near, f.at, b});
                    }
                }
            std::sort(cands.begin(), cands.end(), [](const Link& x, const Link& y) { return x.cost < y.cost; });
            for (const Link& c : cands) {
                if (best && c.cost >= best->cost) break;
                if (refused.count(key(c.a)) && refused.count(key(c.b))) continue;
                const Rect body = Rect{std::min(c.a.x, c.b.x), std::min(c.a.y, c.b.y), std::max(c.a.x, c.b.x),
                                       std::max(c.a.y, c.b.y)}.inflated(0.3);
                const Rect ra = Rect::centered(c.a, padD + 0.5, padD + 0.5), rb = Rect::centered(c.b, padD + 0.5, padD + 0.5);
                bool ok = std::none_of(bodies.begin(), bodies.end(), [&](const Rect& r) { return r.intersects(body); }) &&
                          std::none_of(noHoles.begin(), noHoles.end(), [&](const Rect& r) { return r.intersects(ra) || r.intersects(rb); });
                ok = ok && clear(net, c.a, c.a, padD, true) && clear(net, c.b, c.b, padD, true);
                ok = ok && (c.from.stub == c.a || clear(net, c.a, c.from.stub, w, false));
                ok = ok && (c.to.at == c.b && c.to.stub == c.b ? true
                                                               : clear(net, c.b, c.to.at, w, false) && clear(net, c.to.at, c.to.stub, w, false));
                if (ok) {
                    best = c;
                    break;
                }
            }
        }
        if (!best) break;
        std::string net = nameOf(best->net);
        const Pad& anchor = pads[best->from.pad];
        const int id = addWireLink(s, best->a, best->b, net, pr.schematic.find(anchor.componentId)->ref + "." +
                                                             std::to_string(anchor.padNumber), round + 1);
        const std::string ref = pr.schematic.find(id)->ref;
        takeJson(sieda_apply_pcb_eco(p, nullptr));
        placeWireLink(p, id, best->a, best->b);
        int jnet = -1;
        for (const Pad& pad : pr.pcb.pads(pr.schematic))
            if (pad.componentId == id) jnet = pad.net;
        auto stub = [&](Vec2 a, Vec2 b) {
            if ((a - b).length() < 1e-6) return;
            Track t;
            t.a = a, t.b = b, t.net = jnet, t.layer = kTopLayer, t.width = st.widthFor(net);
            pr.pcb.addTrack(t);
        };
        stub(best->a, best->from.stub);
        stub(best->b, best->to.at), stub(best->to.at, best->to.stub);
        refused.insert(key(best->a)), refused.insert(key(best->b));
        added.push(ref);
    }
    return added;
}

// A two-layer route whose bottom copper stands for wire links (single-sided boards, pcb_add_jumpers fromBottom): each
// run of bottom tracks becomes one link between the holes at its ends (a bare straight wire when that line keeps clear
// of the bodies on the bottom, else an insulated jumper formed along the run); the vias go and the board becomes
// single-sided.
Json bottomToLinks(McpServer& s) {
    SiedaProject* p = P(s);
    Project& pr = p->project;
    PcbLayout& pcb = pr.pcb;
    const int bottom = pcb.settings.bottomLayer();
    std::map<int, std::string> names;
    for (const Net& n : pr.schematic.nets()) names[n.index] = n.name;
    std::map<int, std::string> anchor;  // net → a pin on it ("R1.2")
    for (const Pad& pad : pcb.pads(pr.schematic))
        if (pad.net >= 0 && !anchor.count(pad.net))
            anchor[pad.net] = pr.schematic.find(pad.componentId)->ref + "." + std::to_string(pad.padNumber);
    std::vector<Rect> bodies;
    for (const auto& c : pr.schematic.components())
        if (const FootprintDef* fp = c.hasFootprint() && c.pcb.placed && c.pcb.bottom ? Library::instance().footprint(c.footprintName()) : nullptr) {
            const bool turned = c.pcb.rotation % 180 != 0;
            bodies.push_back(Rect::centered(c.pcb.position, turned ? fp->body.depth : fp->body.width, turned ? fp->body.width : fp->body.depth));
        }
    std::vector<Track> under, top;
    for (const Track& t : pcb.tracks) (t.layer == bottom && !t.arc ? under : top).push_back(t);
    // Each piece of bottom copper joins the vias standing on it: they are the link holes, joined by the shortest
    // links that keep them all together (Prim).
    auto key = [](Vec2 v) { return std::make_pair(std::lround(v.x * 1000), std::lround(v.y * 1000)); };
    std::vector<size_t> parent(under.size());
    std::iota(parent.begin(), parent.end(), 0);
    std::function<size_t(size_t)> find = [&](size_t x) { return parent[x] == x ? x : parent[x] = find(parent[x]); };
    std::map<std::pair<long, long>, size_t> at;
    for (size_t i = 0; i < under.size(); ++i)
        for (Vec2 e : {under[i].a, under[i].b}) {
            auto [it, fresh] = at.emplace(key(e), i);
            if (!fresh) parent[find(i)] = find(it->second);
        }
    for (size_t i = 0; i < under.size(); ++i)  // a track ending on another's middle
        for (size_t k = 0; k < under.size(); ++k)
            if (i != k && find(i) != find(k) &&
                std::min(trackPointDistance(under[k], under[i].a), trackPointDistance(under[k], under[i].b)) < 1e-3)
                parent[find(i)] = find(k);
    std::map<size_t, std::vector<Vec2>> holes;
    std::map<size_t, int> pieceNet;
    for (const Via& v : pcb.vias)
        for (size_t i = 0; i < under.size(); ++i)
            if (trackPointDistance(under[i], v.position) < 1e-3) {
                holes[find(i)].push_back(v.position), pieceNet[find(i)] = under[i].net;
                break;
            }
    std::vector<Vec2> viaAt;  // the vias and bottom copper stay until the links stand on the holes: Update PCB drops
    for (const Via& v : pcb.vias) viaAt.push_back(v.position);  // top copper that reaches no pad
    auto clearLine = [&](Vec2 a, Vec2 b) {
        return std::all_of(bodies.begin(), bodies.end(), [&](const Rect& r) { return segmentRectDistance(a, b, r) > 0.3; });
    };
    // One wire per hole: a via that takes a second link gets a hole of its own 2.54 mm away, joined to it on top by a
    // stub clear of other nets (the first clear direction, toward the far end first).
    std::map<std::pair<long, long>, int> taken;
    const double width = pcb.settings.trackWidth;
    std::vector<Track> stubs;  // added once the links stand on their holes (Update PCB drops copper reaching no pad)
    auto ends = [&](Vec2 v, Vec2 toward, int net) {
        if (taken[key(v)]++ == 0) return v;
        const double base = std::atan2(toward.y - v.y, toward.x - v.x);
        for (int k = 0; k < 16; ++k) {
            const double ang = base + (k % 2 ? 1 : -1) * ((k + 1) / 2) * M_PI / 8;
            const Vec2 q = v + Vec2{std::cos(ang), std::sin(ang)} * 2.54;
            if (taken.count(key(q)) || std::any_of(viaAt.begin(), viaAt.end(), [&](Vec2 h) {
                    return (h - v).length() > 1e-6 && (h - q).length() < 2.0 + pcb.settings.clearance;
                }))
                continue;
            try {  // the new hole clear of every pad and of other nets' tracks; its stub clear of other nets
                checkClearOfOtherNets(pr, net, q, q, 2.0, -1);
                checkClearOfOtherNets(pr, net, v, q, width, kTopLayer);
                for (const Pad& pad : pcb.pads(pr.schematic))
                    if (padPointDistance(pad, q) < 1.0 + pcb.settings.clearance) throw ToolError("pad");
            } catch (const ToolError&) {
                continue;
            }
            Track t;
            t.a = v, t.b = q, t.net = net, t.layer = kTopLayer, t.width = width;
            stubs.push_back(t);
            taken[key(q)] = 1;
            return q;
        }
        return v;  // no room: the wires share the hole
    };
    struct Pair { int net; Vec2 a, b; };
    std::vector<Pair> pairs;
    std::vector<std::tuple<int, Vec2, Vec2>> links;
    std::map<int, std::string> netNames;
    int serial = 0;
    for (const auto& [piece, pts] : holes) {
        const int net = pieceNet[piece];
        std::vector<char> in(pts.size(), 0);
        in[0] = 1;
        for (size_t n = 1; n < pts.size(); ++n) {
            double best = 1e18;
            size_t bi = 0, bj = 0;
            for (size_t i = 0; i < pts.size(); ++i)
                for (size_t j = 0; in[i] && j < pts.size(); ++j)
                    if (!in[j] && (pts[i] - pts[j]).length() < best) best = (pts[i] - pts[j]).length(), bi = i, bj = j;
            in[bj] = 1;
            pairs.push_back({net, ends(pts[bi], pts[bj], net), ends(pts[bj], pts[bi], net)});
        }
    }
    for (const Pair& q : pairs) {
        if (!netNames.count(q.net)) netNames[q.net] = names.count(q.net) ? names[q.net] : std::string();
        links.emplace_back(addWireLink(s, q.a, q.b, netNames[q.net], anchor.count(q.net) ? anchor[q.net] : std::string(),
                                       ++serial, !clearLine(q.a, q.b)),
                           q.a, q.b);
    }
    takeJson(sieda_apply_pcb_eco(p, nullptr));
    Json added = Json::array();
    for (const auto& [id, a, b] : links) {
        placeWireLink(p, id, a, b);
        added.push(pr.schematic.find(id)->ref);
    }
    pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return t.layer == bottom; }),
                     pcb.tracks.end());
    pcb.vias.clear();
    for (const Track& t : stubs) pcb.addTrack(t);
    pcb.settings.layerCount = 1;
    return added;
}

// Layout reuse (pcb_layout_block): a group's placement relative to its first part and the copper inside the group,
// nets named by the group pin they reach ("part index:pin index") so the layout lands on another group of the same
// parts.
Json captureLayout(const Project& pr, const std::vector<int>& ids) {
    const Component* anchor = pr.schematic.find(ids[0]);
    const Vec2 o = anchor->pcb.position;
    std::map<int, std::string> netKey;  // net → "i:pin"
    std::map<int, int> padCount;
    for (const Pad& pad : pr.pcb.pads(pr.schematic))
        for (size_t i = 0; i < ids.size(); ++i)
            if (pad.componentId == ids[i]) {
                ++padCount[ids[i]];
                if (pad.net >= 0 && !netKey.count(pad.net)) netKey[pad.net] = std::to_string(i) + ":" + std::to_string(pad.pinIndex);
            }
    Rect box;
    bool first = true;
    Json parts = Json::array();
    for (int id : ids) {
        const Component* c = pr.schematic.find(id);
        const Rect r = pr.pcb.courtyard(*c);
        box = first ? r : Rect{std::min(box.x0, r.x0), std::min(box.y0, r.y0), std::max(box.x1, r.x1), std::max(box.y1, r.y1)};
        first = false;
        Json p = obj();
        p["ref"] = c->ref, p["pads"] = padCount[id], p["dx"] = c->pcb.position.x - o.x, p["dy"] = c->pcb.position.y - o.y;
        p["rotation"] = c->pcb.rotation, p["bottom"] = c->pcb.bottom;
        parts.push(p);
    }
    auto inside = [&](Vec2 q) { return q.x >= box.x0 - 0.5 && q.x <= box.x1 + 0.5 && q.y >= box.y0 - 0.5 && q.y <= box.y1 + 0.5; };
    auto pt = [&](Vec2 q) {
        Json j = obj();
        j["x"] = q.x - o.x, j["y"] = q.y - o.y;
        return j;
    };
    Json tracks = Json::array(), vias = Json::array();
    for (const Track& t : pr.pcb.tracks)
        if (netKey.count(t.net) && !t.arc && inside(t.a) && inside(t.b)) {
            Json j = obj();
            j["a"] = pt(t.a), j["b"] = pt(t.b), j["layer"] = t.layer, j["width"] = t.width, j["net"] = netKey[t.net];
            tracks.push(j);
        }
    for (const Via& v : pr.pcb.vias)
        if (netKey.count(v.net) && inside(v.position)) {
            Json j = pt(v.position);
            j["drill"] = v.drill, j["diameter"] = v.diameter, j["from"] = v.fromLayer, j["to"] = v.toLayer, j["net"] = netKey[v.net];
            vias.push(j);
        }
    Json layout = obj();
    layout["parts"] = parts, layout["tracks"] = tracks, layout["vias"] = vias;
    return layout;
}

/// Places `ids` (same parts, same order as the layout's) with the layout's arrangement round `at` and copies its copper
/// onto their nets; copper that would touch another net is skipped and counted.
Json applyLayout(SiedaProject* p, const Json& layout, const std::vector<int>& ids, Vec2 at) {
    Project& pr = p->project;
    const auto& parts = layout.get("parts").items();
    check(parts.size() == ids.size(), "The layout has " + std::to_string(parts.size()) + " parts; give as many target parts, in the same order");
    std::map<int, int> padCount;
    std::map<std::string, int> netOf;  // "i:pin" → target net
    for (const Pad& pad : pr.pcb.pads(pr.schematic))
        for (size_t i = 0; i < ids.size(); ++i)
            if (pad.componentId == ids[i]) {
                ++padCount[ids[i]];
                netOf[std::to_string(i) + ":" + std::to_string(pad.pinIndex)] = pad.net;
            }
    for (size_t i = 0; i < ids.size(); ++i) {
        const Component* c = pr.schematic.find(ids[i]);
        check(c && c->hasFootprint(), "Target part " + std::to_string(i + 1) + " has no footprint");
        check(padCount[ids[i]] == parts[i].get("pads").asInt(-1),
              c->ref + " has " + std::to_string(padCount[ids[i]]) + " pads, the layout's " + parts[i].get("ref").asString("?") + " " +
                  std::to_string(parts[i].get("pads").asInt(0)));
    }
    Json j = obj(), notMoved = Json::array();
    for (size_t i = 0; i < ids.size(); ++i) {
        const Json& q = parts[i];
        const Json r = takeJson(sieda_pcb_place_footprint(p, ids[i], at.x + q.get("dx").asNumber(0), at.y + q.get("dy").asNumber(0),
                                                           q.get("rotation").asInt(0), q.get("bottom").asBool(false) ? 1 : 0, 0, 1));
        if (!r.get("committed").asBool(false)) notMoved.push(pr.schematic.find(ids[i])->ref);
    }
    int tracks = 0, vias = 0, skipped = 0;
    auto netFor = [&](const Json& e) {
        const auto it = netOf.find(e.get("net").asString(""));
        return it == netOf.end() ? -1 : it->second;
    };
    for (const auto& e : layout.get("tracks").items()) {
        Track t;
        t.net = netFor(e), t.layer = e.get("layer").asInt(0), t.width = e.get("width").asNumber(0.25);
        t.a = {at.x + e.get("a").get("x").asNumber(0), at.y + e.get("a").get("y").asNumber(0)};
        t.b = {at.x + e.get("b").get("x").asNumber(0), at.y + e.get("b").get("y").asNumber(0)};
        try {
            check(t.net >= 0 && t.layer < pr.pcb.settings.layerCount, "");
            const auto same = [&](Vec2 u, Vec2 w) { return std::fabs(u.x - w.x) < 1e-6 && std::fabs(u.y - w.y) < 1e-6; };
            if (std::any_of(pr.pcb.tracks.begin(), pr.pcb.tracks.end(), [&](const Track& o) {
                    return o.net == t.net && o.layer == t.layer && ((same(o.a, t.a) && same(o.b, t.b)) || (same(o.a, t.b) && same(o.b, t.a)));
                }))
                continue;  // already there (the layout applied before)
            checkClearOfOtherNets(pr, t.net, t.a, t.b, t.width, t.layer);
            pr.pcb.addTrack(t);
            ++tracks;
        } catch (const ToolError&) {
            ++skipped;
        }
    }
    for (const auto& e : layout.get("vias").items()) {
        Via v;
        v.net = netFor(e), v.position = {at.x + e.get("x").asNumber(0), at.y + e.get("y").asNumber(0)};
        v.drill = e.get("drill").asNumber(0.3), v.diameter = e.get("diameter").asNumber(0.6);
        v.fromLayer = e.get("from").asInt(0), v.toLayer = e.get("to").asInt(-1);
        try {
            check(v.net >= 0, "");
            if (std::any_of(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& o) {
                    return o.net == v.net && std::fabs(o.position.x - v.position.x) < 1e-6 && std::fabs(o.position.y - v.position.y) < 1e-6;
                }))
                continue;
            checkClearOfOtherNets(pr, v.net, v.position, v.position, v.diameter, -1);
            pr.pcb.addVia(v);
            ++vias;
        } catch (const ToolError&) {
            ++skipped;
        }
    }
    p->router.reset();
    j["tracks"] = tracks, j["vias"] = vias, j["skipped"] = skipped;
    if (notMoved.size()) j["notMoved"] = notMoved;
    return j;
}

Json stitchVias(SiedaProject* p, Json o) {
    if (!o.has("pitch")) o["pitch"] = 5.0;
    Project& pr = p->project;
    const size_t before = pr.pcb.vias.size();
    Json r = takeJson(sieda_pcb_stitch_vias(p, o.dump().c_str()));
    int under = 0;  // a stitch under a part (a module, a switch, a connector shell) goes again
    for (size_t i = pr.pcb.vias.size(); i-- > before;)
        for (const auto& c : pr.schematic.components())
            if (c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c) &&
                pr.pcb.courtyard(c).inflated(pr.pcb.vias[i].diameter / 2).contains(pr.pcb.vias[i].position)) {
                pr.pcb.vias.erase(pr.pcb.vias.begin() + static_cast<long>(i));
                ++under;
                break;
            }
    Json j = obj();
    for (const char* k : {"ok", "message", "added", "skipped", "error"})
        if (r.has(k)) j[k] = r.get(k);
    if (under) {
        j["added"] = r.get("added").asInt(0) - under;
        j["underParts"] = under;
        j["message"] = std::to_string(r.get("added").asInt(0) - under) + " stitching vias (" + std::to_string(under) +
                       " more under parts removed)";
    }
    if (j.get("added").asInt(0) == 0 && !j.has("error")) {  // say why none
        const std::string net = o.has("net") ? o.get("net").asString("") : "the ground net";
        std::set<int> layers;
        for (const auto& z : pr.pcb.zones)
            if (!o.has("net") || z.net == o.get("net").asString("")) layers.insert(z.layer);
        j["reason"] = layers.size() < 2 ? net + " has pours on fewer than two layers: add one on another layer (pcb_add_pour)"
                                        : "every spot on the pitch grid is blocked by copper of other nets, a part or a keep-out: "
                                          "try a smaller pitch, or place one with pcb_add_via";
    }
    return j;
}

/// The stitching vias of each pour net: vias of a net with a pour that no track reaches, away from the parts (one by a
/// part is the router's fan-out into the pour; pcb_stitch_vias puts none there).
std::map<int, std::vector<Vec2>> stitchingVias(const Project& pr) {
    std::set<std::string> poured;
    for (const auto& z : pr.pcb.zones) poured.insert(z.net);
    std::map<int, std::vector<Vec2>> out;
    const auto& nets = pr.schematic.nets();
    for (const auto& v : pr.pcb.vias) {
        if (v.net < 0 || v.net >= static_cast<int>(nets.size()) || !poured.count(nets[static_cast<size_t>(v.net)].name)) continue;
        bool reached = false;
        for (const auto& t : pr.pcb.tracks)
            reached = reached || (t.net == v.net && std::min((t.a - v.position).length(), (t.b - v.position).length()) < v.diameter / 2);
        for (const auto& c : pr.schematic.components())  // by a part: the router's fan-out into the pour
            reached = reached || (c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c) &&
                                  pr.pcb.courtyard(c).inflated(v.diameter / 2 + 1).contains(v.position));
        if (!reached) out[v.net].push_back(v.position);
    }
    return out;
}

Json placeDecoupling(SiedaProject* p, std::set<int>* bound = nullptr) {
    const auto classes = partClasses(p->project);  // the assembly spacing every move keeps (standardGap)
    // Each IC supply pin gets the nearest small capacitor (≤ 1 µF) between its net and ground, one capacitor per pin
    // (a regulator's input and output pins take bulk ones too; other bulk capacitors stay); one further than 3 mm
    // goes to the nearest free spot 1.5 – 9 mm from the pin, on the side away from the IC.
    Json moved = Json::array();
    const Schematic& sch = p->project.schematic;
    const auto pads = p->project.pcb.pads(sch);
    std::vector<const Pad*> supply;
    for (const auto& pad : pads) {
        const Component* c = sch.find(pad.componentId);
        if (!c || pad.net < 0 || pad.pinIndex < 0 || c->def().pins.size() < 3 || sch.netRole(pad.net) != NetRole::Power) continue;
        const auto type = static_cast<PinType>(c->def().pins[static_cast<size_t>(pad.pinIndex)].type);
        if (type == PinType::PowerIn || type == PinType::PowerOut) supply.push_back(&pad);
    }
    auto regulator = [&](int id) {  // a part with a supply output: its input and output take bulk capacitors too
        for (const auto& pin : sch.find(id)->def().pins)
            if (static_cast<PinType>(pin.type) == PinType::PowerOut) return true;
        return false;
    };
    struct Cap { int id, net; bool bulk; };
    std::vector<Cap> caps;
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Capacitor || c.pcb.locked || !c.pcb.placed || c.def().pins.size() != 2) continue;
        const auto v = parseEngineeringValue(c.value);
        const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        if (n0 < 0 || n1 < 0) continue;
        const int net = sch.netRole(n0) == NetRole::Ground ? n1 : sch.netRole(n1) == NetRole::Ground ? n0 : -1;
        if (net >= 0) caps.push_back({c.id, net, v && *v > 1.01e-6});
    }
    // A crystal (Y…) goes beside the IC pins it drives, and each load capacitor (its net to ground) beside it; first,
    // being the biggest, so the supply capacitors take the room around it.
    // The free spot nearest `target` (rings of 0.5 mm); with `toward`, the spots ranked by their distance to `target`
    // plus half that to `target + toward` (a load capacitor between its crystal pad and the IC).
    auto nearest = [&](int id, Vec2 target, double from, double to, Vec2 toward = Vec2{0, 0}) {
        std::vector<std::pair<double, Vec2>> spots;
        for (double r = from; r <= to; r += 0.5)
            for (int k = 0; k < 16; ++k) {
                const Vec2 d = Vec2{std::cos(k * kPi / 8), std::sin(k * kPi / 8)} * r;
                spots.push_back({toward.length() > 1e-9 ? r + 0.5 * (d - toward).length() : 0.0, target + d});
            }
        std::stable_sort(spots.begin(), spots.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
        for (const auto& spot : spots)
            for (int rot : {0, 90})
                if (const Vec2 at = spot.second; spaced(p->project, classes, id, at, rot, false) &&
                    takeJson(sieda_pcb_place_footprint(p, id, at.x, at.y, rot, 0, 0.25, 0)).get("committed").asBool(false))
                    return true;
        return false;
    };
    // How far the capacitor's pad on the pin's net ended from the pin (pad centres), for the result.
    auto gap = [&](int cap, int comp, int pinIndex) {
        const auto pads = p->project.pcb.pads(sch);
        const auto pin = std::find_if(pads.begin(), pads.end(), [&](const Pad& q) { return q.componentId == comp && q.pinIndex == pinIndex; });
        double best = 1e9;  // to the nearest pad of that net on the part (a regulator's tab is its output too)
        for (const auto& q : pads)
            for (const auto& r : pads)
                if (pin != pads.end() && q.componentId == cap && q.net == pin->net && r.componentId == comp && r.net == pin->net)
                    best = std::min(best, (q.position - r.position).length());
        char text[32] = "";
        if (best < 1e8) std::snprintf(text, sizeof text, " (%.1f mm)", best);
        return std::string(text);
    };
    std::set<int> settled;  // placed here: they do not step aside for a later part
    auto stepAside = [&](Vec2 at, double radius, const std::function<bool()>& place) {  // small parts by `at` make room
        std::vector<std::pair<int, Vec2>> aside;
        for (const auto& o : sch.components()) {  // small parts only (chip passives, SOD / SMA diodes, SOT-23): connectors stay at their edge
            const std::string prefix = o.ref.substr(0, o.ref.find_first_of("0123456789"));
            const auto cls = classes.find(o.id);
            if (!settled.count(o.id) && !o.pcb.locked && o.pcb.placed && (o.pcb.position - at).length() < radius &&
                cls != classes.end() && cls->second == PartClass::Passive && o.def().pins.size() <= 3 &&
                prefix != "J" && prefix != "P" && prefix != "CN" && prefix != "USB")
                aside.push_back({o.id, o.pcb.position});
        }
        for (const auto& [id, was] : aside) p->project.schematic.find(id)->pcb.placed = false;
        const bool ok = place();
        for (const auto& [id, was] : aside)
            if (!nearest(id, was, 0, 15)) p->project.schematic.find(id)->pcb.placed = true;  // back where it was
        return ok;
    };
    struct Crystal { int id; Vec2 pins; std::vector<std::pair<int, int>> loads; };  // load capacitors with their nets
    std::vector<Crystal> crystals;
    for (const auto& y : sch.components()) {
        if (y.ref.size() < 2 || y.ref[0] != 'Y' || !std::isdigit(static_cast<unsigned char>(y.ref[1])) || y.pcb.locked || !y.pcb.placed)
            continue;
        std::set<int> nets;
        for (int i = 0; i < static_cast<int>(y.def().pins.size()); ++i)
            if (const int n = sch.netOf({y.id, i}); n >= 0 && sch.netRole(n) == NetRole::Signal) nets.insert(n);
        Vec2 sum{0, 0};
        int count = 0, icId = -1;
        for (const auto& pad : p->project.pcb.pads(sch)) {
            const Component* c = sch.find(pad.componentId);
            if (c && c->id != y.id && c->def().pins.size() >= 3 && nets.count(pad.net)) sum = sum + pad.position, ++count, icId = c->id;
        }
        if (!count) continue;
        const Vec2 pins = sum * (1.0 / count);
        const bool close = (y.pcb.position - pins).length() <= 4;
        settled.insert(y.id);
        // Close by the pins, if need be with the small parts there stepping aside; else the nearest free spot.
        const bool near = close || nearest(y.id, pins, 2.5, 6) || stepAside(pins, 10, [&] { return nearest(y.id, pins, 2.5, 6); }) ||
                          nearest(y.id, pins, 6.5, 9);
        if (near && !close)
            moved.push(y.ref + " → " + sch.find(icId)->ref + " " + std::to_string(count) + " pins");
        crystals.push_back({y.id, pins, {}});
        for (const auto& c : sch.components()) {
            if (c.kind != ComponentKind::Capacitor || c.pcb.locked || !c.pcb.placed || c.def().pins.size() != 2) continue;
            const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
            const int net = nets.count(n0) && n1 >= 0 && sch.netRole(n1) == NetRole::Ground   ? n0
                            : nets.count(n1) && n0 >= 0 && sch.netRole(n0) == NetRole::Ground ? n1 : -1;
            if (net < 0) continue;
            for (const auto& pad : p->project.pcb.pads(sch))
                if (pad.componentId == y.id && pad.net == net) {
                    crystals.back().loads.push_back({c.id, net});
                    // On the IC's side of the crystal: a short loop from the pins through the caps.
                    if ((sch.find(c.id)->pcb.position - pad.position).length() > 3 && nearest(c.id, pad.position, 1.5, 6, pins - pad.position))
                        moved.push(c.ref + " → " + y.ref);
                    settled.insert(c.id);
                    break;
                }
        }
    }
    struct Pair { double d; const Pad* pad; int cap; bool spare; double drawn; };
    std::vector<Pair> pairs;
    auto drawnApart = [&](const Pad* pad, int cap) {  // on the schematic: a capacitor drawn by a pin is meant for it
        double best = 1e9;
        for (int i = 0; i < 2; ++i)
            if (sch.netOf({cap, i}) == pad->net)
                best = std::min(best, (sch.pinPosition({cap, i}) - sch.pinPosition({pad->componentId, pad->pinIndex})).length());
        return std::round(best / 100);  // in steps of 100 units: nearer than that, the board decides
    };
    for (const Pad* pad : supply)
        for (const auto& c : caps)
            if (c.net == pad->net && (!c.bulk || regulator(pad->componentId)))
                pairs.push_back({(sch.find(c.id)->pcb.position - pad->position).length(), pad, c.id,
                                 !c.bulk && regulator(pad->componentId), drawnApart(pad, c.id)});
    // A regulator pin takes a small capacitor only after the loads' pins have theirs (it has its bulk ones); then the
    // capacitor drawn by the pin, then the nearest on the board.
    std::stable_sort(pairs.begin(), pairs.end(), [](const Pair& l, const Pair& r) {
        return l.spare != r.spare ? r.spare : l.drawn != r.drawn ? l.drawn < r.drawn : l.d < r.d;
    });
    std::set<const Pad*> served;
    std::set<int> used;
    for (const Pair& pr : pairs) {
        if (served.count(pr.pad) || used.count(pr.cap)) continue;
        served.insert(pr.pad);
        used.insert(pr.cap);
        settled.insert(pr.cap);
        if (pr.d <= 3.0) continue;  // already close: leave it
        const Component* ic = sch.find(pr.pad->componentId);
        Vec2 out = pr.pad->position - ic->pcb.position;
        out = out.length() > 1e-9 ? out * (1 / out.length()) : Vec2{1, 0};
        std::vector<Vec2> dirs;
        for (int k = 0; k < 16; ++k) dirs.push_back({std::cos(k * kPi / 8), std::sin(k * kPi / 8)});
        std::stable_sort(dirs.begin(), dirs.end(), [&](Vec2 l, Vec2 r) { return l.x * out.x + l.y * out.y > r.x * out.x + r.y * out.y; });
        auto within = [&](double from, double to) {
            for (double r = from; r <= to; r += 0.5)
                for (size_t k = 0; k < dirs.size(); ++k)
                    for (int rot : {0, 90}) {
                        const Vec2 at = pr.pad->position + dirs[k] * r;
                        if (spaced(p->project, classes, pr.cap, at, rot, false) &&
                            takeJson(sieda_pcb_place_footprint(p, pr.cap, at.x, at.y, rot, 0, 0.25, 0)).get("committed").asBool(false)) {
                            // Turned half round when that brings its pad on the pin's net nearer the pin (no acute joins).
                            auto mm = [&] {
                                const std::string t = gap(pr.cap, pr.pad->componentId, pr.pad->pinIndex);
                                return t.size() > 2 ? std::atof(t.c_str() + 2) : 1e9;
                            };
                            const double was = mm();
                            if (!takeJson(sieda_pcb_place_footprint(p, pr.cap, at.x, at.y, rot + 180, 0, 0.25, 0)).get("committed").asBool(false) ||
                                mm() > was)
                                takeJson(sieda_pcb_place_footprint(p, pr.cap, at.x, at.y, rot, 0, 0.25, 0));
                            return true;
                        }
                    }
            return false;
        };
        // Close by the pin, if need be with the small parts there stepping aside; else the nearest free spot.
        const bool put = within(1.5, 4) || stepAside(pr.pad->position, 7, [&] { return within(1.5, 4); }) || within(4.5, 9);
        // Still far (or nowhere), with a crystal taking the room by the pin: the capacitor first, then the crystal (and its load
        // capacitors) again further out; kept only when all of them find a spot.
        bool swapped = false;
        for (const auto& xt : crystals) {
            const auto pads2 = p->project.pcb.pads(sch);
            const auto mine = std::find_if(pads2.begin(), pads2.end(), [&](const Pad& q) { return q.componentId == pr.cap && q.net == pr.pad->net; });
            if ((put && (mine == pads2.end() || (mine->position - pr.pad->position).length() <= 3.5)) ||
                (sch.find(xt.id)->pcb.position - pr.pad->position).length() > 10)
                continue;
            const Project before = p->project;
            p->project.schematic.find(xt.id)->pcb.placed = false;
            for (const auto& [load, net] : xt.loads) p->project.schematic.find(load)->pcb.placed = false;
            settled.insert(pr.cap);  // the crystal again: small parts by its pins step aside, as at the start
            const bool capOk = within(1.5, 3.5);
            bool ok = capOk && (nearest(xt.id, xt.pins, 2.5, 9) || stepAside(xt.pins, 10, [&] { return nearest(xt.id, xt.pins, 2.5, 9); }));
            for (const auto& [load, net] : xt.loads)
                for (const auto& q : p->project.pcb.pads(sch))
                    if (ok && q.componentId == xt.id && q.net == net) {
                        ok = nearest(load, q.position, 1.5, 6, xt.pins - q.position) ||
                             stepAside(q.position, 7, [&, id = load] { return nearest(id, q.position, 1.5, 6, xt.pins - q.position); });
                        break;
                    }
            if (!ok) p->project = before, p->router.reset();
            swapped = ok;
            break;
        }
        if (put || swapped)
            moved.push(sch.find(pr.cap)->ref + " → " + pinLabel(*p, {ic->id, pr.pad->pinIndex}) + gap(pr.cap, ic->id, pr.pad->pinIndex));
    }
    // A bulk capacitor no regulator pin took: on a rail a regulator drives, by the nearest load supply pin; on a rail
    // fed from outside, where the supply comes in (a connector, input diode, fuse, ferrite or inductor on its net);
    // after the small ones.
    for (const auto& c : caps) {
        if (!c.bulk || used.count(c.id)) continue;
        const Pad* best = nullptr;
        const Vec2 at = sch.find(c.id)->pcb.position;
        auto closer = [&](const Pad* pad) { return !best || (pad->position - at).length() < (best->position - at).length(); };
        auto output = [&](const Pad* pad) {
            return static_cast<PinType>(sch.find(pad->componentId)->def().pins[static_cast<size_t>(pad->pinIndex)].type) == PinType::PowerOut;
        };
        const bool regulated = std::any_of(supply.begin(), supply.end(), [&](const Pad* pad) { return pad->net == c.net && output(pad); });
        const auto now = p->project.pcb.pads(sch);  // where the parts are now (small ones may have stepped aside)
        for (bool series : {true, false}) {  // a series part (an input diode, a fuse) before a connector on the rail
            for (const auto& pad : now) {
                const Component* o = sch.find(pad.componentId);
                const std::string prefix = o ? o->ref.substr(0, o->ref.find_first_of("0123456789")) : "";
                const bool entry = series ? prefix == "D" || prefix == "F" || prefix == "FB" || prefix == "L"
                                          : prefix == "J" || prefix == "P" || prefix == "CN" || prefix == "USB";
                if (!regulated && pad.net == c.net && entry && closer(&pad)) best = &pad;
            }
            if (best) break;
        }
        if (!best)
            for (const Pad* pad : supply)
                if (pad->net == c.net && !output(pad) && closer(pad)) best = pad;
        if (!best || (best->position - at).length() <= 3.5) continue;
        settled.insert(c.id);
        if (nearest(c.id, best->position, 2, 9))
            moved.push(sch.find(c.id)->ref + " → " + pinLabel(*p, {best->componentId, best->pinIndex}) + gap(c.id, best->componentId, best->pinIndex));
    }
    // A capacitor from ground to a signal net with one IC pin (AREF, EN, a reference or filter pin) goes beside it.
    for (const auto& c : sch.components()) {
        if (c.kind != ComponentKind::Capacitor || c.pcb.locked || !c.pcb.placed || c.def().pins.size() != 2) continue;
        const int n0 = sch.netOf({c.id, 0}), n1 = sch.netOf({c.id, 1});
        const int net = n1 >= 0 && sch.netRole(n1) == NetRole::Ground && n0 >= 0 && sch.netRole(n0) == NetRole::Signal ? n0
                        : n0 >= 0 && sch.netRole(n0) == NetRole::Ground && n1 >= 0 && sch.netRole(n1) == NetRole::Signal ? n1 : -1;
        if (net < 0) continue;
        Pad pin;
        int ics = 0;
        for (const auto& pad : p->project.pcb.pads(sch)) {
            const Component* o = sch.find(pad.componentId);
            if (pad.net != net || !o || o->id == c.id) continue;
            if (o->ref.size() > 1 && o->ref[0] == 'Y' && std::isdigit(static_cast<unsigned char>(o->ref[1]))) ics = 99;  // a load cap
            if (o->def().pins.size() >= 3 && ++ics == 1) pin = pad;
        }
        if (ics == 1) settled.insert(c.id);
        if (ics != 1 || (sch.find(c.id)->pcb.position - pin.position).length() <= 3) continue;
        const std::string label = pinLabel(*p, {pin.componentId, pin.pinIndex});
        if (nearest(c.id, pin.position, 1.5, 9)) moved.push(c.ref + " → " + label + gap(c.id, pin.componentId, pin.pinIndex));
    }
    // A two-pin part (a pull-up, an LED's resistor, the LED, a ferrite) on a signal net with one other pad goes beside
    // that pad: first those on an IC pin, then the ones they lead to (an LED after its resistor).
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& c : sch.components()) {
            const std::string prefix = c.ref.substr(0, c.ref.find_first_of("0123456789"));
            if (settled.count(c.id) || c.pcb.locked || !c.pcb.placed || c.def().pins.size() != 2 ||
                (prefix != "R" && prefix != "D" && prefix != "LED" && prefix != "L" && prefix != "FB"))
                continue;
            for (int i = 0; i < 2; ++i) {
                const int net = sch.netOf({c.id, i});
                if (net < 0 || sch.netRole(net) != NetRole::Signal) continue;
                const Pad* other = nullptr;
                int others = 0;
                for (const auto& pad : pads)
                    if (pad.net == net && pad.componentId != c.id) other = &pad, ++others;
                if (others != 1) continue;
                const Component* o = sch.find(other->componentId);
                if (!o || (o->def().pins.size() >= 3) != (pass == 0) || (pass == 1 && !settled.count(o->id))) continue;
                settled.insert(c.id);
                const auto now = p->project.pcb.pads(sch);
                const Pad* to = nullptr;  // where that pad is now (its part may have moved in this pass)
                for (const auto& q : now)
                    if (q.componentId == other->componentId && q.pinIndex == other->pinIndex) to = &q;
                if (to && (c.pcb.position - to->position).length() > 4 && nearest(c.id, to->position, 1.5, 8))
                    moved.push(c.ref + " → " + pinLabel(*p, {to->componentId, to->pinIndex}) + gap(c.id, to->componentId, to->pinIndex));
                break;
            }
        }
    if (bound) bound->insert(settled.begin(), settled.end());  // placed by their pins: grouping leaves them there
    return moved;
}

/// Grouped layout (pcb_autoplace grouped): the free passives of one kind (C…, R…, D…, L…) in an aligned row in
/// designator order (C1 C2 C3), kGroupGap apart (room for a track between pads and a readable designator), nearest
/// where Auto Place put them (their connections);
/// parts placed by their IC pins (`bound`) stay. A row that does not fit is split into two or three. Returns the rows.
constexpr double kGroupGap = 1.0;  // mm between courtyards inside a grouped row and between its lines

Json placeGrouped(SiedaProject* p, const std::set<int>& bound) {
    Json rows = Json::array();
    Project& pr = p->project;
    const auto classes = partClasses(pr);
    std::map<std::string, std::vector<const Component*>> groups;
    for (const auto& c : pr.schematic.components()) {
        const auto cls = classes.find(c.id);
        if (cls == classes.end() || cls->second != PartClass::Passive || c.pcb.locked || !c.pcb.placed || bound.count(c.id)) continue;
        groups[c.ref.substr(0, c.ref.find_first_of("0123456789"))].push_back(&c);
    }
    for (auto& [prefix, parts] : groups) {
        if (parts.size() < 2) continue;
        auto number = [](const Component* c) { return std::atoi(c->ref.c_str() + c->ref.find_first_of("0123456789")); };
        std::stable_sort(parts.begin(), parts.end(), [&](const Component* l, const Component* r) { return number(l) < number(r); });
        Vec2 centre{0, 0};
        std::vector<std::pair<int, PcbPlacement>> was;
        for (const Component* c : parts) centre = centre + c->pcb.position * (1.0 / static_cast<double>(parts.size())), was.push_back({c->id, c->pcb});
        for (const auto& [id, pl] : was) pr.schematic.find(id)->pcb.placed = false;
        // Each part turned so it is narrower than tall; the row's pitch is the widest courtyard plus the spacing.
        std::vector<int> turn;
        double pitch = 0, height = 0;
        for (const Component* c : parts) {
            Rect cy = pr.checkPlacement(c->id, centre, 0, false, 0).courtyard;
            turn.push_back(cy.width() > cy.height() ? 90 : 0);
            if (turn.back()) cy = pr.checkPlacement(c->id, centre, 90, false, 0).courtyard;
            pitch = std::max(pitch, cy.width() + kGroupGap);
            height = std::max(height, cy.height() + kGroupGap);
        }
        bool done = false;
        for (int lines = 1; lines <= 3 && !done; ++lines) {
            const int across = static_cast<int>((parts.size() + static_cast<size_t>(lines) - 1) / static_cast<size_t>(lines));
            auto slot = [&](size_t i, Vec2 o) {
                return o + Vec2{(static_cast<double>(i % static_cast<size_t>(across)) - (across - 1) / 2.0) * pitch,
                                static_cast<double>(i / static_cast<size_t>(across)) * height};
            };
            for (double r = 0; r <= std::max(pr.pcb.settings.width, pr.pcb.settings.height) && !done; r += 0.5)
                for (int k = 0; k < (r > 0 ? 16 : 1) && !done; ++k) {
                    const Vec2 o = centre + Vec2{std::cos(k * kPi / 8), std::sin(k * kPi / 8)} * r;
                    bool fits = true;
                    for (size_t i = 0; i < parts.size() && fits; ++i)  // the whole row is free (the others still lifted)
                        fits = pr.checkPlacement(parts[i]->id, slot(i, o), turn[i], false, 0).legal && spaced(pr, classes, parts[i]->id, slot(i, o), turn[i], false);
                    if (!fits) continue;
                    size_t placed = 0;
                    for (size_t i = 0; i < parts.size(); ++i)
                        placed += takeJson(sieda_pcb_place_footprint(p, parts[i]->id, slot(i, o).x, slot(i, o).y, turn[i], 0, 0, 0)).get("committed").asBool(false);
                    done = placed == parts.size();
                    if (!done)  // all or nothing: lifted again for the next try
                        for (const auto& [id, pl] : was) pr.schematic.find(id)->pcb.placed = false;
                }
        }
        if (!done) {
            for (const auto& [id, pl] : was) pr.schematic.find(id)->pcb = pl;
            continue;
        }
        std::string row;
        for (const Component* c : parts) row += (row.empty() ? "" : " ") + c->ref;
        rows.push(row);
    }
    return rows;
}

/// After a part moved: the tracks that ended on its old pads, then every track of those nets left with a free end
/// and every via left alone, so no stubs stay behind. Returns how many tracks and vias went.
int removeStranded(Project& pr, const std::vector<Pad>& old) {
    std::set<int> nets;
    for (const auto& pad : old)
        if (pad.net >= 0) nets.insert(pad.net);
    auto onPad = [](const std::vector<Pad>& pads, Vec2 v, int net) {
        for (const auto& pad : pads)
            if (pad.net == net && std::fabs(v.x - pad.position.x) <= pad.size.x / 2 + 0.01 &&
                std::fabs(v.y - pad.position.y) <= pad.size.y / 2 + 0.01)
                return true;
        return false;
    };
    auto& tracks = pr.pcb.tracks;
    auto& vias = pr.pcb.vias;
    const size_t before = tracks.size() + vias.size();
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const Track& t) {
                     return nets.count(t.net) && !t.locked && (onPad(old, t.a, t.net) || onPad(old, t.b, t.net));
                 }), tracks.end());
    const auto pads = pr.pcb.pads(pr.schematic);
    for (bool again = true; again;) {
        auto held = [&](Vec2 v, int net, const Track* self) {  // another track, a via or a pad holds this end
            if (onPad(pads, v, net)) return true;
            for (const auto& w : vias)
                if (w.net == net && (w.position - v).length() <= w.diameter / 2 + 0.01) return true;
            for (const auto& t : tracks)
                if (&t != self && t.net == net && ((t.a - v).length() < 0.01 || (t.b - v).length() < 0.01)) return true;
            return false;
        };
        const size_t n = tracks.size();
        std::vector<char> drop(n, 0);
        for (size_t i = 0; i < n; ++i)
            drop[i] = nets.count(tracks[i].net) && !tracks[i].locked && (!held(tracks[i].a, tracks[i].net, &tracks[i]) ||
                                                                         !held(tracks[i].b, tracks[i].net, &tracks[i]));
        size_t k = 0;
        for (size_t i = 0; i < n; ++i)
            if (!drop[i]) tracks[k++] = tracks[i];
        tracks.resize(k);
        again = k < n;
    }
    vias.erase(std::remove_if(vias.begin(), vias.end(), [&](const Via& w) {
                   if (!nets.count(w.net)) return false;
                   for (const auto& t : tracks)
                       if (t.net == w.net && ((t.a - w.position).length() <= w.diameter / 2 + 0.01 || (t.b - w.position).length() <= w.diameter / 2 + 0.01))
                           return false;
                   return !onPad(pads, w.position, w.net);
               }), vias.end());
    return static_cast<int>(before - tracks.size() - vias.size());
}

Json violationsOut(McpServer& s, char* raw) { return summariseViolations(*P(s), takeJson(raw)); }

/// Pad centre and layer of a pin on the board.
bool padOf(const Project& p, PinAddress a, Vec2* at, int* layer) {
    for (const auto& pad : p.pcb.pads(p.schematic))
        if (pad.componentId == a.component && pad.pinIndex == a.pin) {
            *at = pad.position;
            *layer = pad.throughHole ? 0 : pad.smdLayer;
            return true;
        }
    return false;
}

Json readFileArg(McpServer& s, const std::string& path) {
    const std::string full = s.resolvePath(path, false);
    std::error_code ec;
    if (fs::file_size(full, ec) > 128u * 1024u * 1024u && !ec) throw ToolError(path + " is larger than 128 MB");
    std::ifstream in(full, std::ios::binary);
    if (!in) throw ToolError("Cannot read " + full);
    std::stringstream ss;
    ss << in.rdbuf();
    return Json(ss.str());
}

/// Reads a KiCad schematic's sub-sheets: from `sheets` ({file: text}) first, else the file beside `from` (inside the
/// root); "" when neither has it.
KicadSheetLoader sheetLoader(McpServer& s, const Json& sheets, const std::string& from) {
    return [&s, sheets, dir = fs::path(from).parent_path()](const std::string& file) -> std::string {
        if (sheets.get(file).isString()) return sheets.get(file).asString();
        if (const std::string base = fs::path(file).filename().string(); sheets.get(base).isString()) return sheets.get(base).asString();
        try {
            return readFileArg(s, (dir / file).string()).asString();
        } catch (...) {
            return {};
        }
    };
}

/// schematic_import_netlist's work on a read netlist (also the start of pcb_import_kicad): the schematic_build result
/// with "imported" (what each designator became and a fidelity count). `map` = library mapping: the other tool's part
/// name, value or footprint → a catalog / library part or a built-in kind (resistor, capacitor, inductor, diode, led).
Json importNetlist(McpServer& s, const ParsedNetlist& nl, const Json& map = Json()) {
    auto upper = [](std::string v) {
        for (auto& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return v;
    };
    std::map<std::string, std::vector<std::string>> pinsOf;  // designator → its pin numbers, in netlist order
    for (const auto& [net, members] : nl.nets)
        for (const auto& [ref, pin] : members)
            if (std::find(pinsOf[ref].begin(), pinsOf[ref].end(), pin) == pinsOf[ref].end()) pinsOf[ref].push_back(pin);
    auto tool = [&](const char* name, const Json& args) {
        for (const auto& t : mcpTools())
            if (t.name == name) return t.handler(s, args).data;
        throw ToolError(std::string("missing tool ") + name);
    };
    Json parts = Json::array(), mapped = obj(), created = Json::array(), skipped = Json::array();
    std::set<std::string> made;
    std::map<std::string, int> how;  // fidelity: built-in / catalog / mapped / library parts
    auto mapping = [&](const NetlistPart& np) -> std::string {
        for (const std::string& key : {np.part, np.value, np.footprint})
            if (!key.empty() && map.get(key).isString()) return map.get(key).asString();
        return {};
    };
    for (const auto& np : nl.parts) {
        const std::string ref = np.ref, prefix = upper(ref.substr(0, ref.find_first_of("0123456789")));
        const auto& pins = pinsOf[ref];
        const std::string fp = np.footprint.substr(np.footprint.find(':') == std::string::npos ? 0 : np.footprint.find(':') + 1);
        Json part = obj();
        part["ref"] = ref;
        std::string size;
        for (const char* sz : {"0201", "0402", "0603", "0805", "1206", "1210", "1812", "2010", "2512"})
            if (size.empty() && fp.find(sz) != std::string::npos) size = sz;
        const bool led = upper(np.value + fp + prefix).find("LED") != std::string::npos;
        const char* kind = pins.size() > 2 ? nullptr : prefix == "R" ? "resistor" : prefix == "C" ? "capacitor"
                         : prefix == "L" ? "inductor" : prefix == "D" || prefix == "LED" ? (led ? "led" : "diode") : nullptr;
        const std::string to = mapping(np);
        static const std::set<std::string> kinds = {"resistor", "capacitor", "inductor", "diode", "led"};
        const bool inLibrary = std::any_of(proj(s).customLibrary.begin(), proj(s).customLibrary.end(), [&](const std::string& id) {
            const CustomPart* cp = CustomPartRegistry::instance().find(id);
            return id == to || (cp && cp->spec.name == to);
        });
        if (!to.empty() && (kinds.count(to) || findStandardPart(to) || inLibrary)) {
            part[kinds.count(to) ? "kind" : "part"] = to;
            if (!np.value.empty() && np.value != np.part && kinds.count(to)) part["value"] = np.value;
            if (kinds.count(to) && !size.empty()) part["package"] = size;
            ++how["mapped"];
        } else if (np.part.rfind("kind:", 0) == 0) {  // a schematic import that knows the kind (LTspice)
            ++how["builtIn"];
            part["kind"] = np.part.substr(5);
            static const std::regex plain(R"(^[0-9.]+[a-zA-Z\xC2\xB5]*[0-9]*$)");  // "1k", "2k2", "10µ" — not "{res}"
            if (std::regex_match(np.value, plain)) part["value"] = np.value;
        } else if (kind && (np.package.lands.empty() || !size.empty())) {  // a board's THT passive keeps its exact pads
            ++how["builtIn"];
            part["kind"] = kind;
            if (!np.value.empty() && np.value != np.part) part["value"] = np.value;
            if (!size.empty()) part["package"] = size;
        } else if (const StandardPart* sp = !np.package.lands.empty() ? nullptr : findStandardPart(np.part) ? findStandardPart(np.part)
                                                                                                         : findStandardPart(np.value)) {
            part["part"] = sp->spec.name;
            ++how["catalog"];
        } else if (pins.empty()) {  // a drawing, logo or unconnected mounting hole: nothing to wire
            skipped.push(ref);
            continue;
        } else {  // a library part with the netlist's pins
            std::string name = !np.part.empty() && np.part != "~" ? np.part : !np.value.empty() ? np.value : prefix + "_PART";
            int named = 0;  // the package and pin count the footprint names, its pitch and body sizes left out
            static const std::regex sizes(R"(_P\d+(\.\d+)?mm|_?\d+(\.\d+)?x\d+(\.\d+)?(x\d+(\.\d+)?)?mm)");
            std::string pkg = packageTypeFromName(std::regex_replace(fp, sizes, ""), named);
            bool numeric = !pins.empty();
            int top = 0;
            for (const auto& pin : pins) {
                numeric = numeric && pin.size() <= 3 && std::all_of(pin.begin(), pin.end(), ::isdigit);
                if (numeric) top = std::max(top, std::stoi(pin));
            }
            std::vector<std::string> numbers = pins;
            if (named < top || named > 4 * top + 8 || top <= 1) named = 0;  // not a pin count ("M3_DIN965", "45-Degree")
            const int count = numeric ? std::max(top, named) : static_cast<int>(pins.size());
            if (numeric) {
                numbers.clear();
                for (int i = 1; i <= count; ++i) numbers.push_back(std::to_string(i));
            }
            if (pkg.empty() || (named && named != count)) pkg = "HEADER";
            const bool exact = !np.package.lands.empty();  // a board import: the footprint's own pads
            if (exact) {
                numbers.clear();
                for (const auto& l : np.package.lands)
                    if (l.pin != "-" && std::find(numbers.begin(), numbers.end(), l.pin) == numbers.end()) numbers.push_back(l.pin);
            }
            name += "-" + std::to_string(exact ? static_cast<int>(numbers.size()) : count);
            if (!made.count(name)) {
                Json spec = obj(), package = obj(), pinList = Json::array();
                if (exact) {
                    CustomPartSpec cs;
                    cs.package = np.package;
                    package = customPartSpecToJson(cs).get("package");
                }
                package["type"] = exact ? "CUSTOM" : pkg, package["pinCount"] = exact ? static_cast<int>(numbers.size()) : count;
                for (const auto& n : numbers) {
                    Json pin = obj();
                    pin["number"] = n;
                    const auto it = np.pinNames.find(n);
                    pin["name"] = it == np.pinNames.end() ? n : it->second;
                    pin["type"] = "passive";
                    pinList.push(pin);
                }
                spec["name"] = name, spec["package"] = package, spec["pins"] = pinList;
                spec["refPrefix"] = prefix.empty() ? "U" : prefix;
                if (!np.value.empty()) spec["defaultValue"] = np.value;
                if (!np.footprint.empty()) spec["description"] = "Imported from a netlist: " + np.footprint;
                try {
                    tool("library_create_part", spec);
                } catch (const std::exception&) {  // a package that will not draw: a header of the pins used
                    Json header = obj();
                    header["type"] = "HEADER", header["pinCount"] = static_cast<int>(pinList.size());
                    spec["package"] = header;
                    tool("library_create_part", spec);
                }
                made.insert(name);
                created.push(name);
            }
            part["part"] = name;
            if (!np.value.empty()) part["value"] = np.value;
            ++how["library"];
        }
        mapped[ref] = part.has("part") ? part.get("part") : part.get("kind");
        parts.push(part);
    }
    Json nets = obj(), open = Json::array(), wires = Json::array();
    int unnamed = 0;
    for (const auto& [name, members] : nl.nets) {
        if (members.size() == 1 && name.rfind("unconnected-", 0) == 0)  // KiCad's pins left open on purpose
            open.push(members[0].first + "." + members[0].second);
        if (members.size() < 2) continue;  // one pin: nothing to join
        if (members.size() == 2 && (name.empty() || name.rfind("Net-(", 0) == 0)) {  // unnamed: a plain wire
            Json chain = Json::array();
            for (const auto& [ref, pin] : members) chain.push(ref + "." + pin);
            wires.push(chain);
            continue;
        }
        std::string n = name.size() > 1 && name[0] == '/' ? name.substr(1) : name;
        if (n.rfind("Net-(", 0) == 0 && n.back() == ')') n = n.substr(5, n.size() - 6);  // KiCad's unnamed nets
        if (n.empty()) n = "N$" + std::to_string(++unnamed);
        Json list = Json::array();
        for (const auto& [ref, pin] : members) list.push(ref + "." + pin);
        nets[n] = list;
    }
    Json build = obj();
    build["parts"] = parts, build["nets"] = nets, build["pins"] = false;
    if (open.size()) build["noConnect"] = open;
    if (wires.size()) build["wires"] = wires;
    Json j = tool("schematic_build", build);
    Json info = obj();
    info["format"] = nl.format;
    info["parts"] = static_cast<int>(nl.parts.size());
    info["nets"] = static_cast<int>(nets.fields().size());
    info["as"] = mapped;
    info["created"] = created;
    if (skipped.size()) info["skipped"] = skipped;
    // Fidelity: how many parts came through as a known part (built-in, catalog or mapped) rather than a generic one
    // made from the netlist's pins, and what the reader could not carry over (warnings).
    Json fidelity = obj();
    for (const char* k : {"builtIn", "catalog", "mapped", "library"}) fidelity[k] = how[k];
    fidelity["skipped"] = static_cast<int>(skipped.size());
    fidelity["warnings"] = static_cast<int>(nl.warnings.size());
    const int placedParts = how["builtIn"] + how["catalog"] + how["mapped"] + how["library"];
    fidelity["knownPercent"] = placedParts ? 100 * (placedParts - how["library"]) / placedParts : 100;
    if (how["library"]) fidelity["next"] = "Give \"map\" ({\"their part or footprint\": \"catalog part or kind\"}) to use known parts for the generic ones";
    info["fidelity"] = fidelity;
    if (!nl.warnings.empty()) {
        Json w = Json::array();
        for (const auto& x : nl.warnings) w.push(x);
        info["warnings"] = w;
    }
    j["imported"] = info;
    return j;
}

Json writeOrInline(McpServer& s, const Json& a, const std::string& content, const std::string& what) {
    Json j = obj();
    const std::string path = argStr(a, "path", "");
    if (!path.empty()) {
        j["written"] = s.writeFile(path, content);
        j["bytes"] = content.size();
    } else {
        const size_t limit = 400000;
        if (content.size() > limit) {
            j["content"] = content.substr(0, limit);
            j["truncated"] = true;
            j["note"] = what + " is " + std::to_string(content.size()) + " bytes: give \"path\" to write it whole.";
        } else {
            j["content"] = content;
        }
    }
    return j;
}

// ================================================================================================= project tools

void projectTools(Table& t) {
    t.add("project", "project_new", "New project", Kind::Destructive, false,
          "Starts a new, empty project (replacing the open one; save first if needed). Optional industry profile "
          "(general, robotics, power, automotive, rf, space, marine, industrial, medical, defence, networking, vlsi) "
          "applies its design rules and derating.",
          Schema().str("name", "Project name").str("industry", "Industry profile id (default general)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              p->router.reset();
              p->project = Project();
              p->project.name = argStr(a, "name", "Untitled");
              s.setProjectPath("");
              const std::string ind = argStr(a, "industry", "");
              if (!ind.empty()) check(sieda_project_set_industry(p, ind.c_str()) == 1, "Unknown industry profile " + ind);
              return out(projectSummary(s));
          });
    t.add("project", "project_open", "Open project", Kind::Destructive, false,
          "Opens a .siedaproj file from the server's root folder (relative paths resolve against it), replacing the "
          "open project.",
          Schema().str("path", "Project file, e.g. \"designs/blinker.siedaproj\"", true),
          [](McpServer& s, const Json& a) {
              s.openProject(requireStr(a, "path"));
              return out(projectSummary(s));
          });
    t.add("project", "project_save", "Save project", Kind::Files, true,
          "Saves the project to the file it was opened from / last saved to (or to \"path\"), inside the root folder.",
          Schema().str("path", "File to save to (default: the project's own file)"),
          [](McpServer& s, const Json& a) {
              std::string path = argStr(a, "path", s.projectPath());
              if (path.empty()) throw ToolError("The project has no file yet: give \"path\" (e.g. \"my_board.siedaproj\").");
              const std::string full = s.writeFile(path, takeText(sieda_project_save_json(P(s))));
              s.setProjectPath(full);
              Json j = obj();
              j["saved"] = full;
              return out(j);
          });
    t.add("project", "project_save_as", "Save project as", Kind::Files, false,
          "Saves the project to a new .siedaproj file inside the root folder; later saves go there.",
          Schema().str("path", "New project file, e.g. \"designs/led.siedaproj\"", true),
          [](McpServer& s, const Json& a) {
              const std::string full = s.writeFile(requireStr(a, "path"), takeText(sieda_project_save_json(P(s))));
              s.setProjectPath(full);
              Json j = obj();
              j["saved"] = full;
              return out(j);
          });
    t.add("project", "project_cloud", "GitHub cloud sync", Kind::Destructive, false,
          "A GitHub repository as the project's cloud, through your own git and its sign-in (Git Credential Manager, gh "
          "auth login or SSH keys; SiEDA keeps no token). connect {repo: \"owner/name\" or its URL, branch?}: the "
          "project's folder becomes a clone and takes what is there; push {message?}: saves, writes a read-only viewer "
          "(<name>.viewer.html) and pushes both (a rejected push merges first); pull: merges the remote's changes "
          "(project files field by field, yours kept where both changed: \"conflicts\") and reopens the project; share: "
          "links to the file, its history and the viewer (GitHub Pages) and whether the repository is public (anyone can "
          "open them) or private (its collaborators only); status: changed files, ahead / behind. The project must be "
          "saved in a file first.",
          Schema().str("action", "What to do", true, {"connect", "pull", "push", "share", "status"})
              .str("repo", "GitHub repository for connect: owner/name, https://github.com/owner/name or git@github.com:owner/name")
              .str("branch", "Branch (default main)").str("message", "Commit message for push")
              .boolean("viewer", "push: also publish the read-only viewer page (default true)"),
          [](McpServer& s, const Json& a) {
              const std::string action = requireStr(a, "action"), branch = argStr(a, "branch", "main");
              if (!cloud::validBranch(branch)) throw ToolError("Not a branch name: \"" + branch + "\"");
              if (s.projectPath().empty()) throw ToolError("Save the project in a file first (project_save_as).");
              const fs::path file(s.projectPath());
              const std::string dir = file.parent_path().string(), name = file.filename().string(),
                                viewer = file.stem().string() + ".viewer.html";
              if (action != "status" && action != "share" && s.options().readOnly) throw ToolError("Read-only server.");
              Json j = obj();
              try {
                  if (action == "connect") {
                      const auto repo = cloud::githubRepo(requireStr(a, "repo"));
                      if (!repo) throw ToolError("Not a GitHub repository: give owner/name or its github.com URL.");
                      s.writeFile(s.projectPath(), takeText(sieda_project_save_json(P(s))));
                      j = cloud::connect(dir, "https://github.com/" + repo->first + "/" + repo->second + ".git", branch, {name});
                      if (fs::exists(file)) s.openProject(s.projectPath());  // the repository's design when it had one
                  } else if (action == "pull") {
                      s.writeFile(s.projectPath(), takeText(sieda_project_save_json(P(s))));
                      cloud::commit(dir, {name}, "Save " + name + " before pulling");
                      j = cloud::pull(dir, branch);
                      s.openProject(s.projectPath());
                  } else if (action == "push") {
                      s.writeFile(s.projectPath(), takeText(sieda_project_save_json(P(s))));
                      std::vector<std::string> files{name};
                      if (argBool(a, "viewer", true)) {
                          s.writeFile((file.parent_path() / viewer).string(), mcp::viewerHtml(*P(s)));
                          files.push_back(viewer);
                      }
                      j = cloud::push(dir, branch, files, argStr(a, "message", "Update " + P(s)->project.name + " from SiEDA"));
                  } else if (action == "status") {
                      j = cloud::status(dir);
                  }
                  if (action != "status") {
                      auto line = [&](std::vector<std::string> args) {
                          const std::string o = cloud::git(dir, args).output;
                          return o.substr(0, o.find_last_not_of(" \r\n") + 1);
                      };
                      const auto repo = cloud::githubRepo(line({"remote", "get-url", "origin"}));
                      if (!repo) {
                          if (action == "share") throw ToolError("Not connected to GitHub yet: project_cloud connect first.");
                      } else {
                          const std::string at = line({"rev-parse", "--show-prefix"});
                          j["links"] = cloud::shareLinks(repo->first, repo->second, branch, at + name, at + viewer);
                          if (action == "share") {
                              const bool pub = cloud::publiclyReadable(repo->first, repo->second);
                              j["public"] = pub;
                              j["access"] = pub ? "anyone with the link" : "the repository's collaborators (private; add people in "
                                                                          "the repository's Settings → Collaborators)";
                              j["viewerNote"] = "The viewer link works once GitHub Pages serves the branch (Settings → Pages); "
                                                "private-repository Pages need a paid GitHub plan.";
                          }
                      }
                  }
              } catch (const ToolError&) {
                  throw;
              } catch (const std::exception& e) {
                  throw ToolError(e.what());
              }
              return out(j);
          });
    t.add("project", "project_export_json", "Project JSON", Kind::Read, true,
          "The whole project as saved (.siedaproj JSON): schematic, library, board, rules, variants.", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_project_save_json(P(s)))); });
    t.add("project", "project_list_examples", "List examples", Kind::Read, true,
          "The built-in example designs (ids for project_load_example).", Schema(), [](McpServer&, const Json&) {
              Json list = Json::array();
              for (const auto& e : mcpExamples()) {
                  Json j = obj();
                  j["id"] = e.id;
                  j["title"] = e.title;
                  j["description"] = e.description;
                  list.push(j);
              }
              Json j = obj();
              j["examples"] = list;
              return out(j);
          });
    t.add("project", "project_load_example", "Load example", Kind::Destructive, false,
          "Replaces the open project with a built-in example design (schematic and board size; route it with "
          "pcb_update_from_schematic and pcb_autoroute).",
          Schema().str("id", "Example id from project_list_examples, e.g. \"led_indicator\"", true),
          [](McpServer& s, const Json& a) {
              const std::string id = requireStr(a, "id");
              Project p;
              if (!loadExample(p, id)) throw ToolError("Unknown example \"" + id + "\" (see project_list_examples)");
              P(s)->router.reset();
              P(s)->project = std::move(p);
              s.setProjectPath("");
              return out(projectSummary(s));
          });
    t.add("project", "project_summary", "Project summary", Kind::Read, true,
          "Where the design stands: name, file, part / net / sheet counts, board size and layers, placement and "
          "routing progress, and the session (read-only, root folder).",
          Schema(), [](McpServer& s, const Json&) { return out(projectSummary(s)); });
    t.add("project", "project_set_info", "Set project info", Kind::Edit, true,
          "Sets the project name, the requirements text, the industry profile and title-block fields (any subset).",
          Schema()
              .str("name", "Project name")
              .str("requirements", "Requirements / brief the design follows")
              .str("industry", "Industry profile id (project_industry_profiles)")
              .str("title", "Title block: title")
              .str("company", "Title block: company")
              .str("revision", "Title block: revision")
              .str("date", "Title block: date")
              .str("drawnBy", "Title block: drawn by")
              .array("revisions", "Revision table, oldest first (replaces it; [] clears): [{rev, date, description, by}]", "object"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              if (a.has("name")) sieda_project_set_name(p, argStr(a, "name").c_str());
              if (a.has("requirements")) sieda_project_set_requirements(p, argStr(a, "requirements").c_str());
              if (a.has("industry"))
                  check(sieda_project_set_industry(p, argStr(a, "industry").c_str()) == 1, "Unknown industry profile");
              Json tb = obj();
              for (const char* k : {"title", "company", "revision", "date", "drawnBy"})
                  if (a.has(k)) tb[k] = argStr(a, k);
              if (a.get("revisions").isArray()) tb["revisions"] = a.get("revisions");
              if (tb.fields().size()) sieda_set_title_block(p, tb.dump().c_str());
              return out(projectSummary(s));
          });
    t.add("project", "project_industry_profiles", "Industry profiles", Kind::Read, true,
          "Industry profiles (rule preset, derating, standards, guidance) for project_new / project_set_info.", Schema(),
          [](McpServer&, const Json&) {
              Json list = takeJson(sieda_industry_profiles_json());
              Json j = obj();
              j["profiles"] = list;
              return out(j);
          });
    t.add("project", "project_snapshot", "Project snapshot", Kind::Read, true,
          "Raw view-model sections of the design (as the app draws it): components, wires, nets, sheets, buses, "
          "board, pads, tracks, vias, zones, zoneFills, ratsnest, courtyards, variants, … Default: components, "
          "wires, nets, board. (One part's pads by pin: pcb_summary pads.)",
          Schema().array("sections", "Top-level sections to return", "string"),
          [](McpServer& s, const Json& a) {
              Json snap = takeJson(sieda_project_snapshot(P(s)));
              std::set<std::string> want = nameSet(a, "sections");
              if (want.empty()) want = {"components", "wires", "nets", "board"};
              Json j = obj();
              Json available = Json::array();
              if (snap.isObject())
                  for (const auto& [k, v] : snap.fields()) {
                      available.push(k);
                      if (want.count(k)) j[k] = v;
                  }
              if (j.has("pads")) {  // each pad names its part by designator too (component is the internal id)
                  Json pads = Json::array();
                  for (Json pad : j.get("pads").items()) {
                      if (const Component* c = proj(s).schematic.find(pad.get("component").asInt(-1))) pad["ref"] = c->ref;
                      pads.push(pad);
                  }
                  j["pads"] = pads;
              }
              const auto& nets = proj(s).schematic.nets();
              for (const char* key : {"tracks", "vias"})  // copper names its net too (net is the index into nets)
                  if (j.has(key)) {
                      Json list = Json::array();
                      for (Json item : j.get(key).items()) {
                          const int n = item.get("net").asInt(-1);
                          if (n >= 0 && n < static_cast<int>(nets.size())) item["netName"] = nets[static_cast<size_t>(n)].name;
                          list.push(item);
                      }
                      j[key] = list;
                  }
              j["availableSections"] = available;
              return out(j);
          });
}

// ================================================================================================= schematic tools

void schematicTools(Table& t) {
    t.add("schematic", "schematic_add_component", "Add component", Kind::Edit, false,
          "Places a built-in component: resistor, capacitor, inductor, diode, led, voltage_source (value \"5\" or "
          "\"SIN(0 1 1k)\" / \"PULSE(0 5 1m)\"), current_source, ground, npn, nmos, opamp (pins IN+, IN-, OUT; no supply pins: it swings ±15 V unless its "
          "value sets the output limits, e.g. \"LM358 VOH=3.5 VOL=0\" on a 5 V supply, with SR=, AOL=, ROUT= too), switch, "
          "connector, ic8, fuse, net_label, battery, ac_source. Values are engineering text (\"4k7\", \"100n\"). "
          "Without x/y it goes to a free spot. Returns the part with its designator and pins. ICs from the catalog: "
          "library_add_part.",
          Schema()
              .str("kind", "Component kind (see description)", true)
              .str("value", "Value, e.g. \"10k\", \"100n\", \"Red\", \"5\"")
              .str("ref", "Designator (default: next free, e.g. R3)")
              .num("x", "Schematic x (grid units, 10 = one step)")
              .num("y", "Schematic y (grid units, y down)")
              .integer("rotation", "Rotation in degrees (0, 90, 180, 270)")
              .str("package", "Package variant (e.g. \"R_0603\", \"C_1206\", \"D_DO41_THT\"; default by kind)")
              .integer("sheet", "Sheet id to place it on (default: the active sheet)"),
          [](McpServer& s, const Json& a) {
              const int kind = kindFromName(a.get("kind"));
              if (kind < 0 || kind == static_cast<int>(ComponentKind::Custom) || kind == static_cast<int>(ComponentKind::PartUnit))
                  throw ToolError("Unknown kind \"" + argStr(a, "kind") +
                                  "\". Use resistor, capacitor, inductor, diode, led, voltage_source, current_source, "
                                  "ground, npn, nmos, opamp, switch, connector, ic8, fuse, net_label, battery, ac_source; "
                                  "catalog ICs go through library_add_part.");
              SiedaProject* p = P(s);
              if (a.has("sheet")) check(sieda_set_active_sheet(p, argInt(a, "sheet", 1)) == 1, "Unknown sheet");
              Vec2 at = freeSpot(p->project);
              if (a.has("x")) at.x = argNum(a, "x", at.x);
              if (a.has("y")) at.y = argNum(a, "y", at.y);
              const std::string value = argStr(a, "value", "");
              const std::string ref = argStr(a, "ref", "");
              if (!ref.empty() && p->project.schematic.findByRef(ref)) throw ToolError("Designator " + ref + " is already used");
              const int id = sieda_add_component(p, kind, value.empty() ? nullptr : value.c_str(), at.x, at.y,
                                                 argInt(a, "rotation", 0), ref.empty() ? nullptr : ref.c_str());
              check(id >= 0, "Could not add the component");
              if (a.has("package"))
                  check(setPackage(p, id, argStr(a, "package")),
                        "Unknown package \"" + argStr(a, "package") + "\" for this part (the part was added). " +
                            packageChoices(static_cast<ComponentKind>(kind)));
              return out(componentJson(*p, id));
          });
    t.add("schematic", "schematic_remove_component", "Remove component", Kind::Destructive, false,
          "Deletes a component by designator, with its wires and the net labels / grounds wired to it alone. "
          "disconnected lists other parts' pins left with no connection (a label that was their only link).",
          Schema().str("ref", "Designator, e.g. \"R3\"", true), [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              const std::vector<int> own = ownNetSymbols(proj(s).schematic, id);
              auto joined = [&] {  // the other parts' pins wired to at least one more pin
                  const Schematic& sch = proj(s).schematic;
                  std::map<int, std::vector<PinRef>> nets;
                  for (const auto& c : sch.components())
                      if (!isNetSymbolKind(c.kind))
                          for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
                              if (const int n = sch.netOf({c.id, i}); n >= 0) nets[n].push_back({c.id, i});
                  std::set<std::string> pins;
                  for (const auto& [n, list] : nets)
                      if (list.size() > 1)
                          for (const PinRef& pin : list) pins.insert(pinLabel(*P(s), {pin.component, pin.pin}));
                  return pins;
              };
              const std::set<std::string> before = joined();
              check(sieda_remove_component(P(s), id) == 1, "Could not remove " + argStr(a, "ref"));
              int symbols = 0;
              for (int sym : own)
                  if (proj(s).schematic.find(sym)) symbols += sieda_remove_component(P(s), sym) == 1;
              Json j = obj();
              j["removed"] = argStr(a, "ref");
              if (symbols) j["netSymbolsRemoved"] = symbols;
              Json lost = Json::array();  // a label was their only link: say so now, not at the next ERC
              const std::set<std::string> after = joined();
              for (const auto& pin : before)
                  if (!after.count(pin) && pin.rfind(argStr(a, "ref") + ".", 0) != 0) lost.push(pin);
              if (lost.size()) j["disconnected"] = lost;
              return out(j);
          });
    t.add("schematic", "schematic_set_value", "Set value", Kind::Edit, true,
          "Sets a part's value (\"4k7\", \"100n\", \"Blue\", a source's \"3.3\" or \"SIN(0 1 50)\").",
          Schema().str("ref", "Designator", true).str("value", "New value", true), [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              check(sieda_set_component_value(P(s), id, requireStr(a, "value").c_str()) == 1, "Could not set the value");
              return out(componentJson(*P(s), id, false));
          });
    t.add("schematic", "schematic_set_package", "Set package", Kind::Edit, true,
          "Sets the package variant of a passive / diode (\"R_0603\", \"C_1206\", \"CP_Tant_B\", \"D_DO41_THT\"; \"\" "
          "= default). The part's packageOptions are in project_snapshot components.",
          Schema().str("ref", "Designator", true).str("package", "Package variant (\"\" = default)"),
          [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              check(setPackage(P(s), id, argStr(a, "package")),
                    "This part has no package \"" + argStr(a, "package") + "\". " +
                        packageChoices(proj(s).schematic.find(id)->kind));
              return out(componentJson(*P(s), id, false));
          });
    t.add("schematic", "schematic_rename", "Rename designator", Kind::Edit, true, "Changes a part's designator.",
          Schema().str("ref", "Current designator", true).str("newRef", "New designator", true),
          [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              check(sieda_set_component_ref(P(s), id, requireStr(a, "newRef").c_str()) == 1,
                    "Could not rename (is " + argStr(a, "newRef") + " taken?)");
              return out(componentJson(*P(s), id, false));
          });
    t.add("schematic", "schematic_move_component", "Move component", Kind::Edit, true,
          "Moves a part on the schematic (x, y in grid units; the net labels and grounds wired to it alone move with it, "
          "a tag naming a wire from it goes back to the wire's middle) and/or turns it (rotation: absolute degrees; "
          "rotate: relative degrees; its own labels turn with it, its grounds stay upright). overlaps lists symbols it now "
          "sits on, warnings its pins left on a wire or pin they do not join. Net "
          "labels / grounds (#NL…, #GND…) move by "
          "their own ref (schematic_list_components netSymbols: true lists them).",
          Schema()
              .str("ref", "Designator", true)
              .num("x", "New x")
              .num("y", "New y")
              .integer("rotation", "Absolute rotation (0, 90, 180, 270)")
              .integer("rotate", "Relative rotation in degrees (multiples of 90)")
              .str("mirror", "\"x\" mirrors left-right, \"y\" top-bottom (toggles)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const int id = componentId(*p, a.get("ref"));
              if (a.has("mirror")) {
                  const std::string axis = argStr(a, "mirror");
                  check(axis == "x" || axis == "y", "mirror is \"x\" or \"y\"");
                  sieda_mirror_component(p, id, axis == "y" ? 1 : 0);
              }
              const Component* c = p->project.schematic.find(id);
              if (a.has("x") || a.has("y")) {
                  const Vec2 from = c->position;
                  check(sieda_move_component(p, id, argNum(a, "x", c->position.x), argNum(a, "y", c->position.y)) == 1,
                        "Could not move");
                  const Vec2 by = p->project.schematic.find(id)->position - from;  // its own labels / grounds come along
                  for (int sym : ownNetSymbols(p->project.schematic, id)) {
                      const Vec2 at = p->project.schematic.find(sym)->position;
                      sieda_move_component(p, sym, at.x + by.x, at.y + by.y);
                  }
              }
              c = p->project.schematic.find(id);
              int delta = argInt(a, "rotate", 0);
              if (a.has("rotation")) delta = argInt(a, "rotation", 0) - c->rotation;
              delta = ((delta % 360) + 360) % 360;
              if (delta) {
                  // Its own labels / grounds turn with it about their pins, so their wires do not cross the body.
                  struct Held { int sym; PinRef pin; Vec2 offset; };
                  std::vector<Held> held;
                  const Schematic& before = p->project.schematic;
                  const auto own = ownNetSymbols(before, id);
                  for (const auto& w : before.wires())
                      for (const auto& [mine, other] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                          if (mine.component == id && std::find(own.begin(), own.end(), other.component) != own.end())
                              held.push_back({other.component, mine, before.find(other.component)->position - before.pinPosition(mine)});
                  const Vec2 centre = c->position;
                  std::map<int, Vec2> pinWas;
                  for (const auto& h : held) pinWas[h.pin.pin] = before.pinPosition(h.pin) - centre;
                  check(sieda_rotate_component(p, id, delta) == 1, "Could not rotate");
                  const Schematic& after = p->project.schematic;
                  for (const auto& h : held) {
                      const Vec2 was = pinWas[h.pin.pin], now = after.pinPosition(h.pin) - after.find(id)->position;
                      int turn = delta;  // the quarter turn that took the pin where it is now (y down: either sense)
                      for (int t : {delta, (360 - delta) % 360})
                          if ((rotate90(was, t) - now).length() < 1e-6) turn = t;
                      if (after.find(h.sym)->kind == ComponentKind::Ground) {  // a ground stays upright, out from the pin
                          const Vec2 d = pinOutward(p->project, {id, h.pin.pin});
                          const Vec2 at = after.pinPosition(h.pin) + d * 30;
                          sieda_move_component(p, h.sym, std::round(at.x / 10) * 10, std::round(at.y / 10) * 10);
                          const int want = d.y < -0.5 ? 180 : 0, by = ((want - after.find(h.sym)->rotation) % 360 + 360) % 360;
                          if (by) sieda_rotate_component(p, h.sym, by);
                          continue;
                      }
                      const Vec2 at = after.pinPosition(h.pin) + rotate90(h.offset, turn);
                      sieda_move_component(p, h.sym, std::round(at.x / 10) * 10, std::round(at.y / 10) * 10);
                      sieda_rotate_component(p, h.sym, turn);
                  }
              }
              // A tag naming a wire from this part to another (schematic_build) goes back to the wire's middle.
              const Schematic& sch = p->project.schematic;
              std::map<int, std::vector<PinRef>> tagEnds;
              for (const auto& w : sch.wires())
                  for (const auto& [end, other] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                      if (const Component* t = sch.find(other.component); t && t->kind == ComponentKind::NetLabel)
                          tagEnds[t->id].push_back(end);
              for (const auto& [tag, ends] : tagEnds)
                  if (ends.size() == 2 && (ends[0].component == id || ends[1].component == id) && ends[0].component != ends[1].component) {
                      const Vec2 m = (sch.pinPosition(ends[0]) + sch.pinPosition(ends[1])) * 0.5;
                      sieda_move_component(p, tag, std::round(m.x / 10) * 10, std::round(m.y / 10) * 10);
                  }
              Json j = componentJson(*p, id);
              const auto o = sch.symbolOutline(*sch.find(id));
              Json overlaps = Json::array();  // another symbol under it: move again
              for (const auto& other : sch.components())
                  if (other.id != id && other.sheet == sch.find(id)->sheet && !other.packageOnly && !isNetSymbolKind(other.kind)) {
                      const auto q = sch.symbolOutline(other);
                      if (o[0] < q[2] && q[0] < o[2] && o[1] < q[3] && q[1] < o[3]) overlaps.push(other.ref);
                  }
              if (overlaps.size()) j["overlaps"] = overlaps;
              // Its pins (or its labels' / grounds') now lying on a wire or pin they do not join: they read as joined.
              Json touching = Json::array();
              const auto own = ownNetSymbols(sch, id);
              for (const auto& v : sch.runERC())
                  if (v.code == "ERC_TOUCHING_UNCONNECTED" && !v.components.empty() &&
                      (v.components.front() == id || std::find(own.begin(), own.end(), v.components.front()) != own.end()))
                      touching.push(v.message);
              if (touching.size()) j["warnings"] = touching;
              return out(j);
          });
    t.add("schematic", "schematic_connect", "Connect pins", Kind::Edit, false,
          "Draws wires between pins. Give from + to, a chain (each pin wired to the next: all of a chain's pins end up "
          "on ONE net, a chain does not pass through a part), or a list of pairs. Pins "
          "are REF.PIN (\"R1.2\", \"D1.A\", \"Q1.B\", \"U1.VCC\", \"U1.14\"); a name several pins share (\"Q3.S\") wires "
          "them all (alsoWired), \"Q3.S(2)\" just that one. Pins on different sheets are joined "
          "through a pair of global net labels (the net's name, or REF_PIN). Returns the net of each connection "
          "(N$… names are automatic and change as the circuit changes: name a net with schematic_net_label to refer "
          "to it). Two pins already on different named nets (VCC and GND) are not joined unless merge is true; "
          "warnings name any part whose two pins end up on one net (a short).",
          Schema()
              .pin("from", "First pin.")
              .pin("to", "Second pin.")
              .array("chain", "Pins wired one after the other: [\"V1.+\",\"R1.1\"] …", "string")
              .array("connections", "Pairs [[\"R1.2\",\"D1.A\"], …]", "array")
              .boolean("merge", "Join two differently named nets into one (default false)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              std::vector<std::pair<Json, Json>> pairs;
              if (a.has("from") || a.has("to")) pairs.push_back({a.get("from"), a.get("to")});
              if (a.get("chain").isArray())
                  for (size_t i = 0; i + 1 < a.get("chain").size(); ++i) pairs.push_back({a.get("chain")[i], a.get("chain")[i + 1]});
              if (a.get("connections").isArray())
                  for (const auto& c : a.get("connections").items()) {
                      if (c.isArray() && c.size() == 2) pairs.push_back({c[0], c[1]});
                      else if (c.isObject()) pairs.push_back({c.get("from"), c.get("to")});
                      else throw ToolError("Each connection is a pair [\"R1.2\", \"D1.A\"]");
                  }
              if (pairs.empty()) throw ToolError("Give from and to, a chain, or connections");
              Json results = Json::array(), warnings = Json::array();
              int made = 0;
              const bool merge = argBool(a, "merge", false);
              Schematic& sch = p->project.schematic;
              // The named net a pin is on ("" when it is unwired or only on an automatic N$ net).
              auto named = [&](PinAddress q) {
                  const std::string n = sch.isPinConnected({q.component, q.pin}) ? netName(p->project, sch.netOf({q.component, q.pin})) : "";
                  return n.rfind("N$", 0) == 0 ? std::string() : n;
              };
              for (const auto& [x, y] : pairs) {
                  const PinAddress pa = pinAddress(*p, x), pb = pinAddress(*p, y);
                  const Component* ca = sch.find(pa.component);
                  const Component* cb = sch.find(pb.component);
                  Json r = obj();
                  r["from"] = pinLabel(*p, pa);
                  r["to"] = pinLabel(*p, pb);
                  const std::string na = named(pa), nb = named(pb);
                  if (!merge && !na.empty() && !nb.empty() && na != nb) {
                      r["net"] = Json();
                      r["note"] = "not connected: " + r["from"].asString() + " is on net " + na + " and " + r["to"].asString() +
                                  " on net " + nb + " (pass merge: true to join them)";
                      warnings.push(r["note"]);
                      results.push(r);
                      continue;
                  }
                  if (ca && cb && ca->sheet != cb->sheet) {
                      // Across sheets: a global label of one name at each pin, on each pin's sheet.
                      std::string name;
                      for (const PinAddress& q : {pa, pb}) {
                          const std::string n = netName(p->project, p->project.schematic.netOf({q.component, q.pin}));
                          if (name.empty() && !n.empty() && n.rfind("N$", 0) != 0) name = n;
                      }
                      if (name.empty()) name = ca->ref + "_" + std::to_string(pa.pin + 1);
                      for (const PinAddress& q : {pa, pb})
                          onPinSheet(p, q.component, [&] {
                              const Vec2 at = outsidePin(p->project, q, 20);
                              const int l = sieda_add_component(p, static_cast<int>(ComponentKind::NetLabel), name.c_str(), at.x, at.y,
                                                                outwardRotation(pinOutward(p->project, q)), nullptr);
                              check(l >= 0 && sieda_connect(p, q.component, q.pin, l, 0) >= 0, "Could not label " + pinLabel(*p, q));
                              return l;
                          });
                      r["label"] = name;
                      ++made;
                  } else {
                      const int w = sieda_connect(p, pa.component, pa.pin, pb.component, pb.pin);
                      r["wire"] = w;
                      if (w < 0) r["note"] = "not connected (same pin, or already joined)";
                      else ++made;
                  }
                  // "Q3.S" names all of Q3's source pins: they join the first one.
                  Json alsoWired = Json::array();
                  for (const auto& [q, spec] : {std::pair{pa, x}, std::pair{pb, y}})
                      for (int other : namesakes(*p, q, spec))
                          if (sieda_connect(p, q.component, q.pin, q.component, other) >= 0) {
                              alsoWired.push(pinLabel(*p, PinAddress{q.component, other}));
                              ++made;
                          }
                  if (alsoWired.size()) r["alsoWired"] = alsoWired;
                  const int net = sch.netOf({pa.component, pa.pin});
                  r["net"] = netName(p->project, net);
                  results.push(r);
              }
              // Two-pin parts shorted by these wires (both pins on one net).
              std::set<int> parts;
              for (const auto& r : results.items())
                  for (const char* k : {"from", "to"}) parts.insert(pinAddress(*p, r.get(k)).component);
              for (int id : parts) {
                  const Component* c = sch.find(id);
                  if (!c || isNetSymbolKind(c->kind) || c->def().pins.size() != 2) continue;
                  const int n = sch.netOf({id, 0});
                  if (n >= 0 && n == sch.netOf({id, 1}) && sch.isPinConnected({id, 0}))
                      warnings.push(c->ref + " is shorted: both its pins are on net " + netName(p->project, n));
              }
              Json noConnectCleared = Json::array();  // a pin marked unused, now wired: no longer unused
              for (const auto& r : results.items())
                  for (const char* k : {"from", "to"}) {
                      const PinAddress pa = pinAddress(*p, r.get(k));
                      const Component* c = sch.find(pa.component);
                      if (c && c->isNoConnect(pa.pin) && sieda_set_pin_no_connect(p, pa.component, pa.pin, 0))
                          noConnectCleared.push(pinLabel(*p, pa));
                  }
              Json j = obj();
              if (noConnectCleared.size()) j["noConnectCleared"] = noConnectCleared;
              j["wiresAdded"] = made;
              j["connections"] = results;
              if (warnings.size()) j["warnings"] = warnings;
              return out(j);
          });
    t.add("schematic", "schematic_disconnect", "Remove wires", Kind::Destructive, false,
          "Removes the wires between two pins (from, to), every wire at one pin (from only), or a wire by id.",
          Schema().pin("from", "A pin.").pin("to", "The other pin (optional).").integer("wire", "Wire id"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              std::vector<int> ids;
              if (a.has("wire")) ids.push_back(argInt(a, "wire", -1));
              if (a.has("from")) {
                  const PinAddress pa = pinAddress(*p, a.get("from"));
                  PinAddress pb;
                  const bool two = a.has("to");
                  if (two) pb = pinAddress(*p, a.get("to"));
                  for (const auto& w : p->project.schematic.wires()) {
                      const bool atA = (w.a.component == pa.component && w.a.pin == pa.pin) ||
                                       (w.b.component == pa.component && w.b.pin == pa.pin);
                      if (!atA) continue;
                      if (two) {
                          const bool atB = (w.a.component == pb.component && w.a.pin == pb.pin) ||
                                           (w.b.component == pb.component && w.b.pin == pb.pin);
                          if (!atB) continue;
                      }
                      ids.push_back(w.id);
                  }
              }
              int tags = 0;
              const size_t wiresBefore = p->project.schematic.wires().size();
              if (ids.empty() && a.has("from") && a.has("to")) {  // through a net tag between them (schematic_build)
                  const PinAddress pa = pinAddress(*p, a.get("from")), pb = pinAddress(*p, a.get("to"));
                  std::map<int, int> touches;  // net symbol → which of the two pins it is wired to (bit 1 / 2)
                  for (const auto& w : p->project.schematic.wires())
                      for (const auto& [end, other] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}}) {
                          const Component* c = p->project.schematic.find(other.component);
                          if (!c || c->kind != ComponentKind::NetLabel) continue;
                          if (end.component == pa.component && end.pin == pa.pin) touches[c->id] |= 1;
                          if (end.component == pb.component && end.pin == pb.pin) touches[c->id] |= 2;
                      }
                  for (const auto& [id, bits] : touches)
                      if (bits == 3) tags += sieda_remove_component(p, id) == 1;  // its wires go with it
              }
              if (ids.empty() && !tags) throw ToolError("No such wire");
              for (int id : ids) sieda_remove_wire(p, id);
              Json j = obj();
              j["wiresRemoved"] = static_cast<int>(wiresBefore - p->project.schematic.wires().size());  // with a tag's wires
              if (tags) j["tagsRemoved"] = tags;
              return out(j);
          });
    t.add("schematic", "schematic_pin_map", "MCU / FPGA pin map", Kind::Edit, false,
          "A part's pin map for firmware and FPGA tools. format: header (C #defines of each signal net's pin, with "
          "CubeMX-style NAME_Pin / NAME_GPIO_Port for STM32 pins), xdc (Vivado), qsf (Quartus) or csv. assign (text) "
          "or path: an STM32CubeMX .ioc, .xdc, .qsf or two-column CSV (pin, net) whose nets are put on the part's "
          "pins with net labels (pins by name or number).",
          Schema().str("ref", "The MCU / FPGA designator", true)
              .str("format", "Export: header, xdc, qsf or csv; plan = the package's pads with pin, net and role (pin planner)",
                   false, {"header", "xdc", "qsf", "csv", "plan"})
              .str("assign", "Pin assignments to apply (file text)").str("path", "Pin assignment file in the root folder"),
          [](McpServer& s, const Json& a) {
              const std::string ref = requireStr(a, "ref");
              const Component* c = proj(s).schematic.findByRef(ref);
              check(c != nullptr, "No part " + ref);
              Json j = obj();
              if (a.has("assign") || a.has("path")) {
                  const std::string text = a.has("assign") ? argStr(a, "assign") : readFileArg(s, argStr(a, "path")).asString();
                  Json unknown = Json::array();
                  int assigned = 0;
                  const auto pinsOf = c->def().pins;
                  for (const auto& [pin, net] : parsePinAssignments(text)) {
                      std::string number;
                      auto up = [](std::string v) {
                          for (auto& ch : v) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                          return v;
                      };
                      for (const auto& pd : pinsOf)
                          if (up(pd.number) == up(pin) || (number.empty() && up(pd.name) == up(pin))) number = pd.number;
                      if (number.empty()) {
                          unknown.push(pin);
                          continue;
                      }
                      Json args = obj(), list = Json::array();
                      list.push(ref + "." + number);
                      args["net"] = net, args["pins"] = list;
                      for (const auto& t : mcpTools())
                          if (t.name == "schematic_net_label") t.handler(s, args);
                      ++assigned;
                  }
                  j["assigned"] = assigned;
                  if (unknown.size()) j["unknownPins"] = unknown;
              }
              if (argStr(a, "format") == "plan") j["plan"] = pinPlanJson(proj(s).schematic, ref);
              else if (a.has("format")) j["text"] = exportPinMap(proj(s).schematic, ref, argStr(a, "format"));
              check(a.has("format") || a.has("assign") || a.has("path"), "Give format to export, or assign / path to import");
              return out(j);
          });
    t.add("schematic", "schematic_net_label", "Net label", Kind::Edit, false,
          "Names a net: puts a net label beside each given pin and wires it, so every pin with the same label joins "
          "net \"net\" (also across sheets for scope global). A name several pins share (\"Q3.S\") gets one label with "
          "the others wired to it; \"Q3.S(2)\" names just that one. Without pins the label is placed at x, y.",
          Schema()
              .str("net", "Net name, e.g. \"VCC\", \"SDA\", \"LED\"", true)
              .array("pins", "Pins to label: [\"U1.VCC\", \"C1.1\"]", "string")
              .num("x", "Label x when no pins are given")
              .num("y", "Label y when no pins are given")
              .str("scope", "global (default), local (this sheet), port (hierarchical port), entry (sheet entry)",
                   false, {"global", "local", "port", "entry"})
              .integer("targetSheet", "Child sheet of an entry"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string net = requireStr(a, "net");
              const std::string scope = argStr(a, "scope", "");
              Json labels = Json::array();
              auto place = [&](Vec2 at, int rotation = 0) {
                  const int id = sieda_add_component(p, static_cast<int>(ComponentKind::NetLabel), net.c_str(), at.x, at.y, rotation, nullptr);
                  check(id >= 0, "Could not add the label");
                  if (!scope.empty() && scope != "global")
                      check(sieda_set_label_scope(p, id, scope.c_str(), argInt(a, "targetSheet", 0)) == 1, "Invalid scope");
                  return id;
              };
              if (a.get("pins").isArray() && a.get("pins").size()) {
                  std::vector<PinAddress> targets;
                  for (const auto& pin : a.get("pins").items()) {
                      const PinAddress first = pinAddress(*p, pin);
                      targets.push_back(first);
                      // "Q3.S": one label, the other source pins wired to the first.
                      for (int other : namesakes(*p, first, pin)) sieda_connect(p, first.component, first.pin, first.component, other);
                  }
                  for (const PinAddress& pa : targets) {
                      const int id = onPinSheet(p, pa.component, [&] {
                          const int l = place(outsidePin(p->project, pa, 20), outwardRotation(pinOutward(p->project, pa)));
                          check(sieda_connect(p, pa.component, pa.pin, l, 0) >= 0, "Could not wire the label to " + pinLabel(*p, pa));
                          return l;
                      });
                      Json l = obj();
                      l["label"] = p->project.schematic.find(id)->ref;
                      l["pin"] = pinLabel(*p, pa);
                      labels.push(l);
                  }
              } else {
                  const Vec2 at = a.has("x") ? Vec2{argNum(a, "x", 0), argNum(a, "y", 0)} : freeSpot(p->project);
                  const int id = place(at);
                  Json l = obj();
                  l["label"] = p->project.schematic.find(id)->ref;
                  labels.push(l);
              }
              Json j = obj();
              j["net"] = net;
              j["labels"] = labels;
              return out(j);
          });
    t.add("schematic", "schematic_connect_to_ground", "Connect to ground", Kind::Edit, false,
          "Wires each pin to its own ground symbol (net GND, the simulation reference).",
          Schema().array("pins", "Pins to ground: [\"V1.-\", \"C1.2\"]", "string", true),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              Json done = Json::array();
              for (const auto& pin : a.get("pins").items()) {
                  const PinAddress pa = pinAddress(*p, pin);
                  onPinSheet(p, pa.component, [&] {
                      // Clear of the part: out from a side pin, below a bottom pin, above (upside down) a top pin.
                      const Vec2 d = pinOutward(p->project, pa);
                      const Vec2 at = outsidePin(p->project, pa, 30);
                      // A side pin with a connected pin right below it: the symbol still points down, out beyond the
                      // labels drawn on the pins below.
                      const Schematic& sch = p->project.schematic;
                      const Vec2 pos = sch.pinPosition({pa.component, pa.pin});
                      bool below = false;
                      for (int i = 0; std::fabs(d.x) > 0.5 && i < static_cast<int>(sch.find(pa.component)->def().pins.size()); ++i) {
                          const Vec2 q = sch.pinPosition({pa.component, i});
                          below = below || (i != pa.pin && std::fabs(q.x - pos.x) < 0.1 && q.y > pos.y && q.y - pos.y < 25 &&
                                            sch.isPinConnected({pa.component, i}));
                      }
                      Vec2 spot = at;
                      if (below)
                          for (const auto& c : sch.components()) {
                              if (!isNetSymbolKind(c.kind) || c.sheet != sch.find(pa.component)->sheet) continue;
                              const auto o = sch.symbolOutline(c);  // a symbol in the rows just below, on this side
                              const double reach = d.x > 0 ? o[2] - pos.x : pos.x - o[0];
                              if (o[3] > pos.y + 3 && o[1] < pos.y + 30 && reach > 0 && reach < 200)
                                  spot.x = d.x > 0 ? std::max(spot.x, std::ceil((o[2] + 15) / 10) * 10)
                                                   : std::min(spot.x, std::floor((o[0] - 15) / 10) * 10);
                          }
                      const int g = sieda_add_component(p, static_cast<int>(ComponentKind::Ground), nullptr, spot.x, spot.y,
                                                        d.y < -0.5 ? 180 : 0, nullptr);
                      check(g >= 0, "Could not add a ground symbol");
                      check(sieda_connect(p, pa.component, pa.pin, g, 0) >= 0, "Could not wire " + pinLabel(*p, pa) + " to ground");
                      return g;
                  });
                  done.push(pinLabel(*p, pa));
              }
              Json j = obj();
              j["grounded"] = done;
              return out(j);
          });
    t.add("schematic", "schematic_set_no_connect", "No-connect flag", Kind::Edit, true,
          "Marks pins as deliberately unconnected (ERC stops reporting them), or clears the mark. pin, or pins for "
          "several at once.",
          Schema().pin("pin", "The pin.")
              .array("pins", "Several pins: [\"U1.RB1\", \"U1.RB2\"]", "string")
              .boolean("noConnect", "true to mark (default), false to clear"),
          [](McpServer& s, const Json& a) {
              std::vector<Json> specs;
              if (a.has("pin")) specs.push_back(a.get("pin"));
              if (a.get("pins").isArray())
                  for (const auto& q : a.get("pins").items()) specs.push_back(q);
              if (specs.empty()) throw ToolError("Give pin or pins");
              Json done = Json::array();
              for (const auto& spec : specs) {
                  const PinAddress pa = pinAddress(*P(s), spec);
                  check(sieda_set_pin_no_connect(P(s), pa.component, pa.pin, argBool(a, "noConnect", true) ? 1 : 0) == 1,
                        "Could not set the flag on " + pinLabel(*P(s), pa));
                  done.push(pinLabel(*P(s), pa));
              }
              Json j = obj();
              if (done.size() == 1) j["pin"] = std::as_const(done)[size_t{0}];
              else j["pins"] = done;
              j["noConnect"] = argBool(a, "noConnect", true);
              return out(j);
          });
    t.add("schematic", "schematic_list_components", "List components", Kind::Read, true,
          "Every part with designator, kind, value, position, footprint, board placement and the net of each pin.",
          Schema()
              .integer("sheet", "Only this sheet")
              .boolean("pins", "Include the pins (default true)")
              .boolean("netSymbols", "Include net labels and ground symbols (default false)"),
          [](McpServer& s, const Json& a) {
              const Project& p = proj(s);
              const int sheet = argInt(a, "sheet", 0);
              const bool pins = argBool(a, "pins", true), symbols = argBool(a, "netSymbols", false);
              Json list = Json::array();
              for (const auto& c : p.schematic.components()) {
                  if (c.packageOnly || c.kind == ComponentKind::Junction) continue;
                  if (!symbols && isNetSymbolKind(c.kind) && c.kind != ComponentKind::PartUnit) continue;
                  if (sheet > 0 && c.sheet != sheet) continue;
                  list.push(componentJson(*P(s), c.id, pins));
              }
              Json j = obj();
              j["count"] = list.size();
              j["components"] = list;
              return out(j);
          });
    t.add("schematic", "schematic_list_nets", "List nets", Kind::Read, true,
          "Every net with its name, role (signal / power / ground) and the part pins on it.",
          Schema().boolean("includeSingle", "Include nets with a single pin (default false)"),
          [](McpServer& s, const Json& a) {
              const Project& p = proj(s);
              const Schematic& sch = p.schematic;
              const bool single = argBool(a, "includeSingle", false);
              Json list = Json::array();
              for (const auto& n : sch.nets()) {
                  Json pins = Json::array();
                  for (const auto& r : n.pins) {
                      const Component* c = sch.find(r.component);
                      if (!c || isNetSymbolKind(c->kind)) continue;
                      pins.push(pinLabel(*P(s), {r.component, r.pin}));
                  }
                  if (!single && pins.size() < 2 && !n.isGround) continue;
                  Json j = obj();
                  j["name"] = n.name;
                  j["role"] = netRoleName(sch.netRole(n.index));
                  j["pins"] = pins;
                  list.push(j);
              }
              Json j = obj();
              j["count"] = list.size();
              j["nets"] = list;
              return out(j);
          });
    t.add("schematic", "schematic_net_places", "Net navigator", Kind::Read, true,
          "Every place a net appears (pins, labels, ports, entries, buses, grounds) across the sheets.",
          Schema().str("net", "Net name", true), [](McpServer& s, const Json& a) {
              return out(takeJson(sieda_net_places(P(s), netIndex(proj(s), requireStr(a, "net")))));
          });
    t.add("schematic", "schematic_erc", "Electrical rule check", Kind::Read, true,
          "Runs the ERC: unconnected pins, driver conflicts, missing ground, power pins without a source, hierarchy, "
          "bus and directive errors. Returns counts, the violations (errors first) and unconnectedPins: each part's "
          "pins with no wire and no no-connect mark (wire them, or schematic_set_no_connect the unused ones).",
          Schema(), [](McpServer& s, const Json&) {
              Json j = violationsOut(s, sieda_run_erc(P(s)));
              const Schematic& sch = proj(s).schematic;
              Json open = obj();
              size_t openCount = 0;
              for (const auto& c : sch.components()) {
                  if (c.packageOnly || isNetSymbolKind(c.kind)) continue;
                  Json pins = Json::array();
                  for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
                      if (!sch.isPinConnected({c.id, i}) && !c.isNoConnect(i) &&
                          static_cast<PinType>(c.def().pins[static_cast<size_t>(i)].type) != PinType::NoConnect)  // an NC pin
                          pins.push(pinLabel(*P(s), {c.id, i}));
                  openCount += pins.size();
                  if (pins.size()) open[c.ref] = pins;
              }
              j["unconnectedPins"] = open;
              // One line for the unconnected pins (unconnectedPins lists them), not one violation each.
              Json kept = Json::array();
              int unconnected = 0;
              for (const auto& v : j.get("violations").items())
                  if (v.get("code").asString("") == "ERC_UNCONNECTED_PIN") ++unconnected;
                  else kept.push(v);
              if (unconnected) {
                  j["warnings"] = j.get("warnings").asInt(0) - unconnected + 1;  // counts match the list
                  Json v = obj();
                  v["severity"] = "warning";
                  v["code"] = "ERC_UNCONNECTED_PIN";
                  v["message"] = std::to_string(openCount) + " unconnected pins: see unconnectedPins";
                  kept.push(v);
              }
              j["violations"] = kept;
              return out(j);
          });
    t.add("schematic", "schematic_validate", "Circuit validation", Kind::Read, true,
          "Checks values and ratings (E-series values, decoupling, part ratings from the DC operating point) and "
          "reviews the design: an LED with no series resistor, a reset / enable input left floating.", Schema(),
          [](McpServer& s, const Json&) {
              Json all = takeJson(sieda_run_circuit_validation(P(s)));
              const Json review = Project::violationsToJson(designReview(proj(s).schematic));
              if (all.isArray())
                  for (const auto& v : review.items()) all.push(v);
              return out(summariseViolations(*P(s), all));
          });
    t.add("project", "project_undo", "Undo", Kind::Edit, false,
          "Takes back the last tool call that changed the design (up to 30 steps; steps: n takes back several).",
          Schema().integer("steps", "How many calls to take back (default 1)"), [](McpServer& s, const Json& a) {
              int undone = 0;
              for (int i = 0; i < std::max(1, argInt(a, "steps", 1)) && s.undo(); ++i) ++undone;
              if (!undone) throw ToolError("Nothing to undo in this session");
              Json j = obj();
              j["undone"] = undone;
              j["left"] = s.undoSteps();
              return out(j);
          });
    t.add("project", "project_redo", "Redo", Kind::Edit, false,
          "Puts back what project_undo took back (steps: n puts back several); any other change to the design clears it.",
          Schema().integer("steps", "How many undone calls to put back (default 1)"), [](McpServer& s, const Json& a) {
              int redone = 0;
              for (int i = 0; i < std::max(1, argInt(a, "steps", 1)) && s.redo(); ++i) ++redone;
              if (!redone) throw ToolError("Nothing to redo");
              Json j = obj();
              j["redone"] = redone;
              j["undoSteps"] = s.undoSteps();
              return out(j);
          });
    t.add("project", "project_quickstart", "Quick start", Kind::Read, true,
          "The quick start: units, pin addressing, what results carry, the usual flow (the resource "
          "sieda://docs/mcp-quickstart, for clients that cannot read resources).",
          Schema(), [](McpServer& s, const Json&) {
              Json j = obj();
              j["text"] = s.readResource("sieda://docs/mcp-quickstart").get("contents")[size_t{0}].get("text");
              return out(j);
          });
    t.add("schematic", "schematic_build", "Build from a netlist", Kind::Edit, false,
          "Builds a circuit in one call: places the parts, joins each named net (net labels on its pins; GND gets "
          "ground symbols; a 2 – 3 pin signal net is drawn as wires with one label; a net may also name one pin of a "
          "wired chain), wires the wires chains (straight pin-to-pin "
          "lines: wired parts are placed in line), flags the noConnect pins, then runs the ERC and the design review. "
          "Returns checks (erc, review), the placed parts with their pins and any step that failed (the others still "
          "run). Adds to the open project.",
          Schema()
              .array("parts", "[{\"ref\":\"U1\",\"part\":\"NE555\"}, {\"ref\":\"R1\",\"kind\":\"resistor\","
                              "\"value\":\"10k\"}]: part = catalog or project-library name, kind = built-in kind; also value, package, "
                              "x, y, rotation", "object", true)
              .object("nets", "{\"VCC\": [\"U1.VCC\", \"R1.1\"], \"GND\": [\"U1.GND\"]}")
              .array("wires", "Pins wired one after the other: [[\"R1.2\", \"D1.A\"], …]; a chain naming GND grounds its pins", "array")
              .array("noConnect", "Pins left open on purpose", "string")
              .boolean("pins", "Each part's full pin detail (default true); false gives pin number → net only"),
          [](McpServer& s, const Json& a) {
              Json placed = Json::array(), failed = Json::array();
              std::vector<int> ids, unplaced;  // unplaced: parts without x / y, laid out here
              std::set<int> before;
              for (const auto& c : proj(s).schematic.components()) before.insert(c.id);
              auto run = [&](const std::string& tool, const Json& args, const std::string& what) {
                  for (const auto& t : mcpTools())
                      if (t.name == tool) try {
                              const std::string problem = validateArguments(t.inputSchema, args);
                              if (!problem.empty()) throw ToolError(problem);
                              return t.handler(s, args).data;
                          } catch (const std::exception& e) {
                              failed.push(what + ": " + e.what());
                          }
                  return Json();
              };
              for (const auto& part : a.get("parts").items()) {
                  Json args = part;
                  const bool catalog = part.has("part");
                  std::string tool = catalog ? "library_add_part" : "schematic_add_component";
                  if (catalog) args["name"] = part.get("part");
                  for (const auto& id : proj(s).customLibrary)  // a part made with library_create_part
                      if (const CustomPart* cp = CustomPartRegistry::instance().find(id);
                          catalog && cp && (id == part.get("part").asString("") || cp->spec.name == part.get("part").asString(""))) {
                          tool = "library_place_custom_part";
                          args["partId"] = id;
                      }
                  const Json r = run(tool, args,
                                     part.get("ref").asString(catalog ? part.get("part").asString("") : "part"));
                  int id = r.get("id").isNumber() ? r.get("id").asInt() : -1;
                  if (id < 0)  // added, then a step failed (an unknown package): lay it out all the same
                      if (const Component* c = proj(s).schematic.findByRef(part.get("ref").asString("")); c && !before.count(c->id))
                          id = c->id;
                  if (id < 0) continue;
                  ids.push_back(id);
                  if (!part.has("x")) unplaced.push_back(ids.back());
              }
              // Each named net (not ground) offers wire links from its biggest part's pin: to every other pin of a
              // 2 – 3 pin signal net, to the two-pin parts of a bigger signal net, and to the capacitors of a supply.
              // Layout puts the linked parts beside that pin; a link that ends up as a straight, clear run facing
              // both pins is drawn as a wire, the rest of the net gets labels.
              // A name several pins share, given without its number ("J1.GND" on a USB-C's A1 / A12 / B1 / B12),
              // joins every one of them; pins stacked on one spot (a module's GND pads) need no more.
              Json nets = Json::object();
              for (const auto& [net, pins] : a.get("nets").fields()) {
                  Json list = Json::array();
                  for (const auto& pin : pins.items()) {
                      list.push(pin);
                      const std::string spec = pin.asString("");
                      const size_t dot = spec.find('.');
                      const Component* c = dot == std::string::npos || spec.find('(') != std::string::npos
                                               ? nullptr : proj(s).schematic.findByRef(spec.substr(0, dot));
                      if (!c) continue;
                      const std::string name = spec.substr(dot + 1);
                      std::vector<Vec2> seen;
                      for (int i = 0; i < static_cast<int>(c->def().pins.size()); ++i) {
                          const PinDef& d = c->def().pins[static_cast<size_t>(i)];
                          if (d.name != name || d.number.empty()) continue;
                          const Vec2 at = proj(s).schematic.pinPosition({c->id, i});
                          if (std::any_of(seen.begin(), seen.end(), [&](Vec2 v) { return (v - at).length() < 1e-6; })) continue;
                          if (!seen.empty()) list.push(c->ref + "." + name + "(" + d.number + ")");
                          seen.push_back(at);
                      }
                  }
                  nets[net] = list;
              }
              Json wires = a.get("wires").isArray() ? a.get("wires") : Json::array();
              struct Link { std::string net; Json from, to; PinAddress a, b; };
              std::vector<Link> links;
              std::set<std::pair<int, int>> grounded;  // pins on a ground net: their parts stand ground side down
              std::map<std::pair<int, int>, std::string> pinNet;  // each pin's net (its label's length)
              for (const auto& [net, pins] : nets.fields()) {
                  for (const auto& pin : pins.items()) try {
                          const PinAddress pa = pinAddress(*P(s), pin);
                          pinNet[{pa.component, pa.pin}] = net;
                      } catch (const std::exception&) {}
                  std::string up = net;
                  for (auto& ch : up) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                  if (up == "GND" || up == "0" || up == "AGND" || up == "DGND" || up == "PGND")
                      for (const auto& pin : pins.items()) try {
                              const PinAddress g = pinAddress(*P(s), pin);
                              grounded.insert({g.component, g.pin});
                          } catch (const std::exception&) {}
                  if (up.empty() || up == "GND" || up == "0") continue;
                  bool supply = up[0] == '+' || up[0] == '-' ||
                                (std::isdigit(static_cast<unsigned char>(up[0])) && up.find('V') != std::string::npos);
                  for (const char* w : {"GND", "VCC", "VDD", "VSS", "VEE", "VBAT", "VIN", "VBUS", "AGND", "DGND", "PGND"})
                      supply = supply || up.rfind(w, 0) == 0;
                  std::vector<std::pair<Json, PinAddress>> at;
                  for (const auto& pin : pins.items()) try {
                          at.push_back({pin, pinAddress(*P(s), pin)});
                          const PinType t = static_cast<PinType>(
                              proj(s).schematic.find(at.back().second.component)->def().pins[static_cast<size_t>(at.back().second.pin)].type);
                          supply = supply || t == PinType::PowerIn || t == PinType::PowerOut;
                      } catch (const std::exception&) {}
                  auto pinCount = [&](const PinAddress& x) { return proj(s).schematic.find(x.component)->def().pins.size(); };
                  std::stable_sort(at.begin(), at.end(), [&](const auto& l, const auto& r) { return pinCount(l.second) > pinCount(r.second); });
                  // A crystal's load capacitor hangs from the crystal's pin rather than the IC's.
                  const auto crystal = std::find_if(at.begin(), at.end(), [&](const auto& x) {
                      const std::string& r = proj(s).schematic.find(x.second.component)->ref;
                      return r.size() > 1 && r[0] == 'Y' && std::isdigit(static_cast<unsigned char>(r[1]));
                  });
                  // On a rail with a regulator, its first bulk capacitor (≥ 1 µF) stands by the regulator's pin.
                  const auto regulatorPin = std::find_if(at.begin(), at.end(), [&](const auto& x) {
                      for (const auto& pin : proj(s).schematic.find(x.second.component)->def().pins)
                          if (static_cast<PinType>(pin.type) == PinType::PowerOut) return true;
                      return false;
                  });
                  bool bulkTaken = regulatorPin == at.end() || regulatorPin == at.begin();
                  for (size_t i = 1; i < at.size(); ++i) {
                      const Component* c = proj(s).schematic.find(at[i].second.component);
                      // A net of two pins always joins them (a connector to its input diode on VIN).
                      if (c->id == at[0].second.component || (at.size() > 2 && (supply ? c->kind != ComponentKind::Capacitor || pinCount(at[i].second) != 2
                                                                                       : at.size() > 3 && pinCount(at[i].second) != 2)))
                          continue;
                      const auto v = parseEngineeringValue(c->value);
                      const bool bulk = !bulkTaken && c->kind == ComponentKind::Capacitor && v && *v >= 0.99e-6;
                      bulkTaken = bulkTaken || bulk;
                      const auto& hub = crystal != at.end() && c->kind == ComponentKind::Capacitor ? *crystal
                                        : bulk                                                     ? *regulatorPin : at[0];
                      links.push_back({net, hub.first, at[i].first, hub.second, at[i].second});
                  }
              }
              Json layoutWires = wires;
              // The build's own links first: a part anchors by its signal (a pull-up by its pin) before its supply.
              for (const Link& l : links) {
                  Json chain = Json::array();
                  chain.push(l.from);
                  chain.push(l.to);
                  layoutWires.push(chain);
              }
              // A net this build names only on new parts, already on parts drawn before (a later build adding a cap
              // on VBUS): the new part is laid out by a pin of the net from before (a regulator's first), not wired.
              {
                  const Schematic& sch = proj(s).schematic;
                  const std::set<int> fresh(unplaced.begin(), unplaced.end());
                  for (const auto& [net, pins] : nets.fields()) {
                      Json first;
                      bool old = false;
                      for (const auto& pin : pins.items()) try {
                              const PinAddress pa = pinAddress(*P(s), pin);
                              if (!fresh.count(pa.component)) old = true;
                              else if (first.isNull()) first = pin;
                          } catch (const std::exception&) {}
                      if (old || first.isNull()) continue;
                      const Component* best = nullptr;
                      int bestPin = -1, bestScore = -1;
                      for (const auto& c : sch.components()) {
                          if (fresh.count(c.id) || isNetSymbolKind(c.kind) || c.packageOnly) continue;
                          bool regulator = false;
                          for (const auto& d : c.def().pins) regulator |= static_cast<PinType>(d.type) == PinType::PowerOut;
                          for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
                              const int n = sch.netOf({c.id, i});
                              const int score = (regulator ? 10000 : 0) + static_cast<int>(c.def().pins.size());
                              if (n >= 0 && sch.nets()[static_cast<size_t>(n)].name == net && score > bestScore)
                                  best = &c, bestPin = i, bestScore = score;
                          }
                      }
                      if (!best) continue;
                      const PinDef& d = best->def().pins[static_cast<size_t>(bestPin)];
                      Json chain = Json::array();
                      chain.push(best->ref + "." + (d.number.empty() ? d.name : d.number));
                      chain.push(first);
                      layoutWires.push(chain);
                  }
              }
              std::map<int, PinAddress> rehub;  // parts laid out beyond a pull-up: wired from its lower pin instead
              std::map<int, std::pair<int, PinAddress>> taps;  // parts above a pull-up's wire: joined by a junction on it
              layoutBuilt(P(s), unplaced, layoutWires, grounded, pinNet, &rehub, &taps);
              links.erase(std::remove_if(links.begin(), links.end(), [&](const Link& l) { return taps.count(l.b.component) > 0; }), links.end());
              for (Link& l : links)
                  if (const auto it = rehub.find(l.b.component); it != rehub.end() && l.a.component != it->second.component) {
                      const Component* r = proj(s).schematic.find(it->second.component);
                      const PinDef& d = r->def().pins[static_cast<size_t>(it->second.pin)];
                      l.a = it->second;
                      l.from = Json(r->ref + "." + (d.number.empty() ? d.name : d.number));
                  }
              auto straight = [&](const Link& l) {  // facing pins in line, nothing in between
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 p0 = sch.pinPosition({l.a.component, l.a.pin}), p1 = sch.pinPosition({l.b.component, l.b.pin});
                  const Vec2 d0 = pinOutward(proj(s), l.a), d1 = pinOutward(proj(s), l.b), run = p1 - p0;
                  // A crystal's load capacitor hangs straight down from the crystal's pin (a junction with the IC's wire).
                  const std::string& hub = sch.find(l.a.component)->ref;
                  const bool fromCrystal = hub.size() > 1 && hub[0] == 'Y' && std::isdigit(static_cast<unsigned char>(hub[1]));
                  const double out0 = run.x * d0.x + run.y * d0.y;
                  if ((std::fabs(run.x) > 0.5 && std::fabs(run.y) > 0.5) || run.length() < 1 ||
                      (fromCrystal ? out0 < -0.5 : out0 <= 0 && !(d0.y > 0.5 && std::fabs(run.y) < 0.5)) ||  // (from a pull-up's lower pin, side-on)
                      run.x * d1.x + run.y * d1.y >= (std::fabs(d0.x) > 0.5 && d1.y > 0.5 ? 0.5 : 0))  // (a pull-up standing on its wire)
                      return false;
                  for (const auto& c : sch.components()) {  // nothing in the way: a body, a label drawn before or another pin
                      if (c.kind == ComponentKind::Junction || (isNetSymbolKind(c.kind) && c.kind != ComponentKind::NetLabel)) continue;
                      for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
                          const Vec2 q = sch.pinPosition({c.id, i});
                          if ((q - p0).length() > 1 && (q - p1).length() > 1 && pointSegmentDistance(q, p0, p1) < 1) return false;
                      }
                  }
                  return !wireCrossesBody(sch, p0, p1);
              };
              std::map<std::string, std::vector<const Link*>> drawn;  // net → its links drawn as wires
              std::vector<std::pair<std::string, const Link*>> tags;  // net, the wire its tag names
              std::set<std::string> wiredPins;                       // net + pin of each drawn link's ends
              for (const Link& l : links)
                  if (straight(l)) {
                      drawn[l.net].push_back(&l);
                      wiredPins.insert(l.net + "|" + l.from.dump());
                      wiredPins.insert(l.net + "|" + l.to.dump());
                  }
              auto valid = [&](const Json& pins, const std::string& what) {  // a wrong pin drops only itself
                  Json ok = Json::array();
                  for (const auto& pin : pins.items()) try {
                          pinAddress(*P(s), pin);
                          ok.push(pin);
                      } catch (const std::exception& e) {
                          failed.push(what + ": " + e.what());
                      }
                  return ok;
              };
              auto isGround = [](std::string n) {
                  for (auto& ch : n) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                  return n == "GND" || n == "0";
              };
              // Capacitors standing upright in a row above an IC's upward supply pin (decoupling): one rail wire joins
              // their tops (the row shifted, when clear, to have the pin under it) and the pin runs straight up to it; the
              // net's label stays on the rail's first capacitor. The supply's entry stands at the rail's left end: a series
              // diode / fuse / ferrite with the connector feeding it, wired in, or the regulator with the parts by it.
              {
                  const Schematic& sch = proj(s).schematic;
                  auto prefix = [&](int id) { const std::string& r = sch.find(id)->ref; return r.substr(0, r.find_first_of("0123456789")); };
                  auto clearBy = [&](const std::vector<int>& group, Vec2 d, const std::vector<int>& skip = {}) {  // the group moved by d keeps off every other part
                      for (int g : group)
                          for (const auto& c : sch.components()) {
                              if (std::find(group.begin(), group.end(), c.id) != group.end() || std::find(skip.begin(), skip.end(), c.id) != skip.end() ||
                                  isNetSymbolKind(c.kind) || c.kind == ComponentKind::Junction) continue;
                              const auto o = sch.symbolOutline(c), m = sch.symbolOutline(*sch.find(g));
                              if (!(m[2] + d.x + 80 < o[0] || m[0] + d.x - 80 > o[2] || m[3] + d.y + 40 < o[1] || m[1] + d.y - 40 > o[3])) return false;
                          }
                      return true;
                  };
                  auto moveAll = [&](const std::vector<int>& group, Vec2 d) {
                      for (int g : group) sieda_move_component(P(s), g, sch.find(g)->position.x + d.x, sch.find(g)->position.y + d.y);
                  };
                  std::map<std::pair<std::string, std::pair<int, int>>, std::vector<const Link*>> rails;
                  for (const Link& l : links) {
                      const Component* c = sch.find(l.b.component);
                      if (isGround(l.net) || c->kind != ComponentKind::Capacitor || c->def().pins.size() != 2 ||
                          wiredPins.count(l.net + "|" + l.to.dump()) || pinOutward(proj(s), l.a).y > -0.5 || pinOutward(proj(s), l.b).y > -0.5 ||
                          sch.pinPosition({l.b.component, l.b.pin}).y > sch.pinPosition({l.a.component, l.a.pin}).y - 1)
                          continue;
                      // One rail per IC and net (its VCC and AVCC caps in one row), dropped from the first pin.
                      auto same = std::find_if(rails.begin(), rails.end(), [&](const auto& r) { return r.first.first == l.net && r.first.second.first == l.a.component; });
                      (same != rails.end() ? same->second : rails[{l.net, {l.a.component, l.a.pin}}]).push_back(&l);
                  }
                  for (auto& [key, caps] : rails) {
                      auto at = [&](const Link* l) { return sch.pinPosition({l->b.component, l->b.pin}); };
                      const Vec2 hub = sch.pinPosition({key.second.first, key.second.second});
                      const double y = at(caps.front()).y;
                      // A cap standing at another height joins the row when it can move there clear of everything.
                      double end = -1e18, pitch = 1e18;  // the row's last cap (its pin x) and its spacing
                      std::vector<int> inRow;
                      for (const Link* l : caps)
                          if (std::fabs(at(l).y - y) <= 0.5) {
                              if (end > -1e17) pitch = std::min(pitch, std::fabs(at(l).x - end));
                              end = std::max(end, at(l).x), inRow.push_back(l->b.component);
                          }
                      if (pitch > 1e17) pitch = 90;
                      caps.erase(std::remove_if(caps.begin(), caps.end(), [&](const Link* l) {
                                     const double dy = y - at(l).y;
                                     if (std::fabs(dy) <= 0.5) return false;
                                     for (const Vec2 d : {Vec2{std::round((end + pitch - at(l).x) / 10) * 10, dy}})  // at the row's end
                                         if (clearBy({l->b.component}, d, inRow)) {
                                             moveAll({l->b.component}, d);
                                             end = std::max(end, at(l).x), inRow.push_back(l->b.component);
                                             return false;
                                         }
                                     return true;
                                 }),
                                 caps.end());
                      std::sort(caps.begin(), caps.end(), [&](const Link* l, const Link* r) { return at(l).x < at(r).x; });
                      if (caps.size() < 2) continue;
                      std::vector<int> row;
                      for (const Link* l : caps) row.push_back(l->b.component);
                      if (hub.x <= at(caps.front()).x || hub.x >= at(caps.back()).x) {  // the row slid to have the pin after its first cap
                          const Vec2 d{std::round((hub.x - 30 - at(caps.front()).x) / 10) * 10, 0};
                          if (!clearBy(row, d)) continue;
                          moveAll(row, d);
                      }
                      bool clear = true;  // the drop from the pin to the rail crosses no body
                      for (const auto& c : sch.components()) {
                          if (isNetSymbolKind(c.kind) || c.kind == ComponentKind::Junction) continue;
                          const auto o = sch.symbolOutline(c);
                          for (int k = 1; k < 20 && clear; ++k) {
                              const Vec2 q{hub.x, hub.y + (y - hub.y) * k / 20.0};
                              clear = !(q.x > o[0] - 1 && q.x < o[2] + 1 && q.y > o[1] - 1 && q.y < o[3] + 1);
                          }
                      }
                      if (!clear) continue;
                      Json chain = Json::array(), args = obj();
                      for (const Link* l : caps) chain.push(l->to);
                      args["chain"] = chain;
                      run("schematic_connect", args, key.first);
                      int over = -1;  // the rail wire over the pin (split after the scan: splitting adds wires)
                      for (const auto& w : sch.wires()) {
                          const Vec2 a = sch.pinPosition(w.a), b = sch.pinPosition(w.b);
                          if (over < 0 && std::fabs(a.y - y) < 0.5 && std::fabs(b.y - y) < 0.5 && std::min(a.x, b.x) < hub.x && std::max(a.x, b.x) > hub.x)
                              over = w.id;
                      }
                      const int junction = over < 0 ? -1 : sieda_split_wire(P(s), over, hub.x, y);
                      for (const Link* l : caps)
                          if (l != caps.front()) wiredPins.insert(key.first + "|" + l->to.dump());
                      if (junction >= 0 && sieda_connect(P(s), key.second.first, key.second.second, junction, 0) >= 0)
                          wiredPins.insert(key.first + "|" + caps.front()->from.dump());
                      // The entry: a series part on the rail fed from a connector, else the rail's regulator.
                      const Vec2 first = at(caps.front());
                      for (const auto& pin : nets.get(key.first).items()) try {
                              const PinAddress e = pinAddress(*P(s), pin);
                              const Component& ec = *sch.find(e.component);
                              const std::string pre = prefix(e.component);
                              const bool series = ec.def().pins.size() == 2 && (pre == "D" || pre == "F" || pre == "FB" || pre == "L");
                              const bool regulator = static_cast<PinType>(ec.def().pins[static_cast<size_t>(e.pin)].type) == PinType::PowerOut;
                              if (!series && !regulator) continue;
                              std::vector<int> group{e.component};
                              for (int i = 0; i < static_cast<int>(ec.def().pins.size()); ++i) {
                                  if (series && i == e.pin) continue;
                                  for (const auto& q : nets.get(pinNet[{e.component, i}]).items()) try {
                                          const int c = pinAddress(*P(s), q).component;
                                          const std::string cp = prefix(c);
                                          const bool near = (sch.find(c)->position - ec.position).length() < 250 && sch.find(c)->def().pins.size() == 2 &&
                                                            std::find(row.begin(), row.end(), c) == row.end();
                                          if (c != e.component && std::find(group.begin(), group.end(), c) == group.end() &&
                                              (series ? cp == "J" || cp == "P" || cp == "CN" || cp == "USB" : near && !isGround(pinNet[{e.component, i}]) ))
                                              group.push_back(c);
                                      } catch (const std::exception&) {}
                              }
                              if (series && group.size() < 2) break;
                              double right = -1e9, top = 1e9;  // a regulator's group: its right / top edge
                              for (int g : group) right = std::max(right, sch.symbolOutline(*sch.find(g))[2]), top = std::min(top, sch.symbolOutline(*sch.find(g))[1]);
                              const Vec2 from = series ? sch.pinPosition({e.component, e.pin}) : Vec2{right, top};
                              // The nearest clear spot left of the rail's start (a series part's pin on the rail's line).
                              std::vector<std::pair<double, Vec2>> spots;
                              for (int k = 0; k < 30; ++k)
                                  for (int j = series ? 0 : -6; j <= (series ? 0 : 6); ++j)
                                      spots.push_back({std::hypot(20.0 * k, 30.0 * j), Vec2{first.x - (series ? 80 : 60) - 20.0 * k, (series ? y : y + 20) + 30.0 * j}});
                              std::stable_sort(spots.begin(), spots.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
                              bool placed = false;
                              for (const auto& spot : spots) {
                                  const Vec2 d = spot.second - from, g{std::round(d.x / 10) * 10, std::round(d.y / 10) * 10};
                                  if ((placed = clearBy(group, g))) {
                                      moveAll(group, g);
                                      break;
                                  }
                              }
                              if (placed && series && sieda_connect(P(s), e.component, e.pin, caps.front()->b.component, caps.front()->b.pin) >= 0)
                                  wiredPins.insert(key.first + "|" + pin.dump());
                              break;
                          } catch (const std::exception&) {}
                  }
              }
              // Grounds last: a ground symbol turns off the labels already on the pins next to its own.
              auto ordered = nets.fields();
              std::stable_partition(ordered.begin(), ordered.end(), [&](const auto& f) { return !isGround(f.first); });
              for (const auto& [net, pins] : ordered) {
                  // Neighbouring pins of one part on one side (an IC's VDD pins) share one label: a short wire
                  // joins their ends, so their labels do not stack on top of each other.
                  Json args = obj(), labelled = Json::array();
                  struct Seen { Json pin; PinAddress at; Vec2 pos, out; };
                  std::vector<Seen> seen;
                  const Json checked = valid(pins, net);
                  for (const Link* l : drawn[net]) {
                      if (l == drawn[net].front()) {  // named by a tag placed last, where it is clear
                          tags.push_back({net, l});
                          continue;
                      }
                      Json w = obj();
                      w["from"] = l->from;
                      w["to"] = l->to;
                      run("schematic_connect", w, net);
                  }
                  for (const auto& pin : checked.items()) {
                      const PinAddress pa = pinAddress(*P(s), pin);
                      const Vec2 pos = proj(s).schematic.pinPosition({pa.component, pa.pin}), d = pinOutward(proj(s), pa);
                      bool joined = false;
                      // Same part, same side, in line, and no other pin of the part between the two.
                      auto neighbours = [&](const Seen& o) {
                          if (o.at.component != pa.component || (o.out - d).length() > 0.1) return false;
                          const bool vertical = std::fabs(d.x) > 0.5;  // pins on the left / right: a column
                          if (std::fabs(vertical ? o.pos.x - pos.x : o.pos.y - pos.y) > 0.1) return false;
                          const double lo = std::min(vertical ? o.pos.y : o.pos.x, vertical ? pos.y : pos.x);
                          const double hi = std::max(vertical ? o.pos.y : o.pos.x, vertical ? pos.y : pos.x);
                          const Component* c = proj(s).schematic.find(pa.component);
                          for (int i = 0; i < static_cast<int>(c->def().pins.size()); ++i) {
                              const Vec2 q = proj(s).schematic.pinPosition({c->id, i});
                              const double along = vertical ? q.y : q.x;
                              if (std::fabs(vertical ? q.x - pos.x : q.y - pos.y) < 0.1 && along > lo + 0.1 && along < hi - 0.1)
                                  return false;
                          }
                          return true;
                      };
                      for (const Seen& o : seen)
                          if (!joined && !wiredPins.count(net + "|" + pin.dump()) && neighbours(o)) {
                              Json w = obj();
                              w["from"] = o.pin;
                              w["to"] = pin;
                              run("schematic_connect", w, net);
                              joined = true;
                          }
                      const auto tap = taps.find(pa.component);  // (or a junction on the wire it stands over)
                      const bool wired = wiredPins.count(net + "|" + pin.dump()) > 0 || (tap != taps.end() && tap->second.first == pa.pin);
                      seen.push_back({pin, pa, pos, d});
                      const int on = proj(s).schematic.netOf({pa.component, pa.pin});  // labelled by an earlier build
                      const bool already = on >= 0 && proj(s).schematic.nets()[static_cast<size_t>(on)].name == net &&
                                           proj(s).schematic.isPinConnected({pa.component, pa.pin});
                      if (!joined && !wired && !already) labelled.push(pin);
                  }
                  args["pins"] = labelled;
                  if (!args.get("pins").size()) continue;
                  if (isGround(net)) {
                      run("schematic_connect_to_ground", args, net);
                  } else {
                      args["net"] = net;
                      run("schematic_net_label", args, net);
                  }
              }
              for (const auto& chain : wires.items()) {
                  Json args = obj(), pins = Json::array();
                  bool toGround = false;  // ["R6.2", "GND"]: the other pins to ground
                  for (const auto& pin : chain.items())
                      if (isGround(pin.asString(""))) toGround = true;
                      else pins.push(pin);
                  args[toGround ? "pins" : "chain"] = toGround ? pins : chain;
                  run(toGround ? "schematic_connect_to_ground" : "schematic_connect", args, "wire " + chain.dump());
              }
              Json noConnectCleared = Json::array();  // a pin marked unused before that this build joined to a net
              for (const auto& c : proj(s).schematic.components())
                  for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i)
                      if (const int n = proj(s).schematic.netOf({c.id, i});
                          c.isNoConnect(i) && n >= 0 && proj(s).schematic.nets()[static_cast<size_t>(n)].pins.size() > 1)
                          noConnectCleared.push(pinLabel(*P(s), {c.id, i}));
              for (const auto& pin : noConnectCleared.items()) {
                  const PinAddress pa = pinAddress(*P(s), pin);
                  sieda_set_pin_no_connect(P(s), pa.component, pa.pin, 0);
              }
              Json args = obj();
              args["pins"] = valid(a.get("noConnect"), "noConnect");
              if (args.get("pins").size()) {
                  run("schematic_set_no_connect", args, "noConnect");
              }
              // Each wired group's name: a tag on its wire, at the first spot along it (middle first, either side)
              // clear of every part and label.
              auto textBox = [&](const Component& c) {  // an upright label's name reads across it
                  auto o = proj(s).schematic.symbolOutline(c);
                  if (c.kind == ComponentKind::NetLabel && c.rotation % 180 != 0)
                      o[0] -= 5.0 * c.value.size(), o[2] += 5.0 * c.value.size();
                  return o;
              };
              auto hitsOf = [&](int id, const Link* own = nullptr) {  // symbols and labels a placed tag overlaps
                  const Schematic& sch = proj(s).schematic;
                  const auto o = textBox(*sch.find(id));
                  int hits = 0;
                  for (const auto& c : sch.components()) {
                      if (c.id == id || c.kind == ComponentKind::Junction) continue;
                      const auto q = textBox(c);
                      hits += o[0] < q[2] && q[0] < o[2] && o[1] < q[3] && q[1] < o[3];
                  }
                  auto across = [&](Vec2 a0, Vec2 b0) {  // a straight wire through the tag's box
                      return std::max(a0.x, b0.x) > o[0] + 1 && std::min(a0.x, b0.x) < o[2] - 1 && std::max(a0.y, b0.y) > o[1] + 1 &&
                             std::min(a0.y, b0.y) < o[3] - 1 && (std::fabs(a0.x - b0.x) < 0.5 || std::fabs(a0.y - b0.y) < 0.5);
                  };
                  // Nor across another wire (a tag standing off its own into the next), drawn or still to come with its tag.
                  for (const auto& w : sch.wires())
                      if (w.a.component != id && w.b.component != id) hits += across(sch.pinPosition(w.a), sch.pinPosition(w.b));
                  if (own)
                      for (const auto& [n, l] : tags)
                          if (l != own) hits += across(sch.pinPosition({l->a.component, l->a.pin}), sch.pinPosition({l->b.component, l->b.pin}));
                  return hits;
              };
              // The clearest spot for a net's tag on wire l: {pose index, overlaps}, poses filled in.
              auto clearest = [&](const std::string& net, const Link* l, std::vector<std::pair<Vec2, int>>& poses) {
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 p0 = sch.pinPosition({l->a.component, l->a.pin}), p1 = sch.pinPosition({l->b.component, l->b.pin});
                  const bool across = std::fabs(p1.y - p0.y) < 0.5;  // a horizontal wire: the tag stands up or down
                  poses.clear();
                  for (double f : {0.5, 0.35, 0.65, 0.25, 0.75})
                      for (const Vec2 side : across ? std::array<Vec2, 2>{Vec2{0, -1}, Vec2{0, 1}} : std::array<Vec2, 2>{Vec2{-1, 0}, Vec2{1, 0}}) {
                          const Vec2 at{std::round((p0.x + (p1.x - p0.x) * f) / 10) * 10, std::round((p0.y + (p1.y - p0.y) * f) / 10) * 10};
                          poses.push_back({{across ? at.x : p0.x, across ? p0.y : at.y}, outwardRotation(side)});
                      }
                  // Between labels on the neighbouring pins: the tag lies along its wire, centred on it.
                  const Vec2 dir = (p1 - p0) * (1 / std::max(1e-9, (p1 - p0).length()));
                  const Vec2 mid = (p0 + p1) * 0.5 - dir * ((std::max(60.0, 7.0 * net.size() + 16) + 9) / 2);
                  poses.push_back({{std::round(mid.x / 10) * 10, std::round(mid.y / 10) * 10}, outwardRotation(dir)});
                  int best = -1, fewest = 1 << 30;
                  for (size_t k = 0; k < poses.size() && fewest; ++k) {
                      const int id = sieda_add_component(P(s), static_cast<int>(ComponentKind::NetLabel), net.c_str(),
                                                         poses[k].first.x, poses[k].first.y, poses[k].second, nullptr);
                      if (id < 0) continue;
                      const int hits = hitsOf(id, l);
                      sieda_remove_component(P(s), id);
                      if (hits < fewest) fewest = hits, best = static_cast<int>(k);
                  }
                  return std::make_pair(best, fewest);
              };
              auto placeTag = [&](const std::string& net, const Link* l, const std::vector<std::pair<Vec2, int>>& poses, int best) {
                  const int tag = best < 0 ? -1
                                           : sieda_add_component(P(s), static_cast<int>(ComponentKind::NetLabel), net.c_str(),
                                                                 poses[best].first.x, poses[best].first.y, poses[best].second, nullptr);
                  if (tag < 0 || sieda_connect(P(s), l->a.component, l->a.pin, tag, 0) < 0 ||
                      sieda_connect(P(s), tag, 0, l->b.component, l->b.pin) < 0)
                      failed.push(net + ": could not name the wire");
                  return tag;
              };
              auto isStanding = [&](const Link* l) {  // a pull-up standing on its wire: the wire meets its pin side-on
                  const Vec2 run = proj(s).schematic.pinPosition({l->b.component, l->b.pin}) - proj(s).schematic.pinPosition({l->a.component, l->a.pin});
                  return std::fabs(run.x * pinOutward(proj(s), l->b).x + run.y * pinOutward(proj(s), l->b).y) < 0.5;
              };
              std::vector<std::tuple<int, std::string, const Link*>> placedTags;
              for (const auto& [net, l] : tags) {
                  std::vector<std::pair<Vec2, int>> poses;
                  const auto [best, fewest] = clearest(net, l, poses);
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 p0 = sch.pinPosition({l->a.component, l->a.pin}), p1 = sch.pinPosition({l->b.component, l->b.pin});
                  if (isStanding(l)) {  // a pull-up standing on its wire: its tag lies along the wire, never off it,
                      int pick = fewest ? static_cast<int>(poses.size()) - 1 : best;  // slid to its clearest spot there
                      if (fewest) {
                          const auto along = poses.back();
                          const Vec2 dir = (p1 - p0) * (1 / std::max(1e-9, (p1 - p0).length()));
                          int least = 1 << 30;
                          for (int k = -6; k <= 6; ++k) {
                              const Vec2 at = along.first + dir * (10.0 * k);
                              const double t = (at.x - p0.x) * dir.x + (at.y - p0.y) * dir.y;  // never back past the IC's pin
                              if (t < 20 || (at - p1).length() < 20) continue;
                              const int id = sieda_add_component(P(s), static_cast<int>(ComponentKind::NetLabel), net.c_str(), at.x, at.y, along.second, nullptr);
                              if (id < 0) continue;
                              const int hits = hitsOf(id, l);
                              sieda_remove_component(P(s), id);
                              if (hits < least) least = hits, poses.push_back({at, along.second}), pick = static_cast<int>(poses.size()) - 1;
                          }
                      }
                      placedTags.push_back({placeTag(net, l, poses, pick), net, l});
                      continue;
                  }
                  // No clear spot on its only wire, and room on it for two labels: a label at each end instead.
                  if (fewest && drawn[net].size() == 1 && (p1 - p0).length() >= 2 * (15.0 * net.size() + 30)) {
                      Json args = obj(), ends = Json::array();
                      ends.push(l->from);
                      ends.push(l->to);
                      args["net"] = net;
                      args["pins"] = ends;
                      run("schematic_net_label", args, net);
                      continue;
                  }
                  placedTags.push_back({placeTag(net, l, poses, best), net, l});
              }
              // Labels placed after a tag (another net's end labels) may sit on it: such a tag moves to its clearest
              // spot now that every label is down.
              for (auto& [tag, net, l] : placedTags) {
                  if (tag < 0 || !hitsOf(tag, l) || isStanding(l)) continue;
                  sieda_remove_component(P(s), tag);  // its two wires go with it
                  std::vector<std::pair<Vec2, int>> poses;
                  tag = placeTag(net, l, poses, clearest(net, l, poses).first);
              }
              // Two pins' labels on one another (a part drawn before, facing a pin labelled now): the smaller part and
              // its labels step back from its pin until both read; a pair still overlapping is listed.
              Json labelOverlaps = Json::array();
              {
                  const Schematic& sch = proj(s).schematic;
                  auto pinOf = [&](int sym) {  // the one part pin a net symbol is wired to (a wire's tag has two: none)
                      PinAddress at{-1, 0};
                      int n = 0;
                      for (const auto& w : sch.wires())
                          for (const auto& [x, y] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                              if (x.component == sym && sch.find(y.component) && !isNetSymbolKind(sch.find(y.component)->kind))
                                  at = PinAddress{y.component, y.pin}, ++n;
                      return n == 1 ? at : PinAddress{-1, 0};
                  };
                  auto overlap = [&](int a0, int b0, double gap = 0) {  // gap: how far apart they must stand to read
                      const auto o = textBox(*sch.find(a0)), q = textBox(*sch.find(b0));
                      return o[0] < q[2] + gap && q[0] < o[2] + gap && o[1] < q[3] + gap && q[1] < o[3] + gap;
                  };
                  std::vector<int> symbols;
                  for (const auto& c : sch.components())
                      if (c.kind == ComponentKind::NetLabel) symbols.push_back(c.id);
                  auto wiredToParts = [&](int id) {  // joined to another part by a wire: moving it would stretch that wire
                      for (const auto& w : sch.wires())
                          for (const auto& [x, y] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                              if (x.component == id && y.component != id && sch.find(y.component) && !isNetSymbolKind(sch.find(y.component)->kind))
                                  return true;
                      return false;
                  };
                  for (size_t i = 0; i < symbols.size(); ++i)
                      for (size_t k = i + 1; k < symbols.size(); ++k) {
                          const int l = symbols[i], o = symbols[k];
                          const PinAddress pl = pinOf(l), po = pinOf(o);
                          if (!overlap(l, o, (pl.component < 0) != (po.component < 0) ? 6 : 0)) continue;
                          if ((pl.component < 0) != (po.component < 0)) {  // a wire's tag on a pin's label: the tag slides along its wire
                              const int tag = pl.component < 0 ? l : o, label = tag == l ? o : l;
                              std::vector<Vec2> ends;
                              for (const auto& w : sch.wires())
                                  for (const auto& [x, y] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                                      if (x.component == tag && sch.find(y.component)) ends.push_back(sch.pinPosition(y));
                              const Vec2 was = sch.find(tag)->position;
                              bool clear = false;
                              if (ends.size() == 2 && (std::fabs(ends[0].x - ends[1].x) < 0.5 || std::fabs(ends[0].y - ends[1].y) < 0.5)) {
                                  const Vec2 lo{std::min(ends[0].x, ends[1].x), std::min(ends[0].y, ends[1].y)},
                                             hi{std::max(ends[0].x, ends[1].x), std::max(ends[0].y, ends[1].y)};
                                  const Vec2 along = std::fabs(ends[0].x - ends[1].x) < 0.5 ? Vec2{0, 10} : Vec2{10, 0};
                                  for (int step = 1; step < 40 && !clear; ++step)  // either way, nearest first, 20 clear of the ends
                                      for (const int sign : {1, -1}) {
                                          const Vec2 at = was + along * (sign * ((step + 1) / 2));
                                          if (at.x < lo.x + (along.x ? 20 : 0) || at.x > hi.x - (along.x ? 20 : 0) ||
                                              at.y < lo.y + (along.y ? 20 : 0) || at.y > hi.y - (along.y ? 20 : 0))
                                              continue;
                                          sieda_move_component(P(s), tag, at.x, at.y);
                                          if ((clear = !overlap(tag, label, 6))) break;
                                      }
                              }
                              if (!clear) {
                                  sieda_move_component(P(s), tag, was.x, was.y);
                                  labelOverlaps.push(sch.find(tag)->value + " (wire tag) overlaps " + sch.find(label)->value + " at " +
                                                     pinLabel(*P(s), tag == l ? po : pl));
                              }
                              continue;
                          }
                          if (pl.component < 0 || po.component < 0 || pl.component == po.component) continue;
                          const bool lSmall = sch.find(pl.component)->def().pins.size() < sch.find(po.component)->def().pins.size();
                          const PinAddress small = lSmall ? pl : po;
                          bool clear = false;
                          if (sch.find(small.component)->def().pins.size() <= 10 && !wiredToParts(small.component)) {
                              const Vec2 back = pinOutward(proj(s), small) * -20;
                              std::vector<int> group = ownNetSymbols(sch, small.component);
                              group.push_back(small.component);
                              for (int step = 0; step < 12 && !clear; ++step) {
                                  for (int id : group) {
                                      const Vec2 at = sch.find(id)->position + back;
                                      sieda_move_component(P(s), id, at.x, at.y);
                                  }
                                  clear = !overlap(l, o);
                              }
                          }
                          if (!clear)
                              labelOverlaps.push(sch.find(l)->value + " at " + pinLabel(*P(s), pl) + " overlaps " + sch.find(o)->value + " at " + pinLabel(*P(s), po));
                      }
              }
              // A ground or label standing on another net's wire (a part laid out across a link drawn before): it steps out
              // along its stub, away from its pin, until clear of every other net's wire; else it is listed.
              {
                  const Schematic& sch = proj(s).schematic;
                  auto route = [&](const Wire& w) {  // as drawn: out of the part's pin, one corner
                      const bool partA = !isNetSymbolKind(sch.find(w.a.component)->kind);
                      const PinRef from = partA ? w.a : w.b, to = partA ? w.b : w.a;
                      const Vec2 p0 = sch.pinPosition(from), p1 = sch.pinPosition(to);
                      const bool across = std::fabs(pinOutward(proj(s), {from.component, from.pin}).x) > 0.5;
                      return std::array<Vec2, 3>{p0, across ? Vec2{p1.x, p0.y} : Vec2{p0.x, p1.y}, p1};
                  };
                  auto onWire = [&](int sym) {  // its outline crosses a wire of another net
                      const auto o = sch.symbolOutline(*sch.find(sym));
                      const int net = sch.netOf({sym, 0});
                      for (const auto& w : sch.wires()) {
                          if (sch.netOf(w.a) == net || w.a.component == sym || w.b.component == sym) continue;
                          const auto r = route(w);
                          for (int seg = 0; seg < 2; ++seg)
                              for (int k = 0; k <= 20; ++k) {
                                  const Vec2 q = r[seg] + (r[seg + 1] - r[seg]) * (k / 20.0);
                                  if (q.x > o[0] - 1 && q.x < o[2] + 1 && q.y > o[1] - 1 && q.y < o[3] + 1) return true;
                              }
                      }
                      return false;
                  };
                  std::vector<int> grounds;
                  for (const auto& c : sch.components())
                      if (c.kind == ComponentKind::Ground || c.kind == ComponentKind::NetLabel) grounds.push_back(c.id);
                  for (int g : grounds) {
                      PinAddress pin{-1, 0};  // the one part pin it hangs from (a wire's tag has two: left alone)
                      int pins = 0;
                      for (const auto& w : sch.wires())
                          for (const auto& [x, y] : {std::pair{w.a, w.b}, std::pair{w.b, w.a}})
                              if (x.component == g && sch.find(y.component) && !isNetSymbolKind(sch.find(y.component)->kind)) pin = {y.component, y.pin}, ++pins;
                      if (pins > 1 || !onWire(g)) continue;
                      const Vec2 was = sch.find(g)->position;
                      bool clear = false;
                      if (pin.component >= 0) {
                          const Vec2 out = pinOutward(proj(s), pin) * 10, aside{-out.y, out.x};  // out, then aside too
                          for (int side : {0, 2, -2, 4, -4})
                              for (int step = 1; step <= 8 && !clear; ++step) {
                                  sieda_move_component(P(s), g, was.x + out.x * step + aside.x * side, was.y + out.y * step + aside.y * side);
                                  clear = !onWire(g);
                              }
                      }
                      if (!clear) {
                          sieda_move_component(P(s), g, was.x, was.y);
                          labelOverlaps.push((sch.find(g)->kind == ComponentKind::Ground ? std::string("ground") : sch.find(g)->value) +
                                             (pin.component >= 0 ? " at " + pinLabel(*P(s), pin) : std::string()) + " stands on another net's wire");
                      }
                  }
              }
              for (const auto& [part, tap] : taps) {  // a junction on the wire by a tapped part (above or below), wired to its pin
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 hub{sch.pinPosition({part, tap.first}).x, sch.pinPosition({tap.second.component, tap.second.pin}).y};
                  Vec2 at = hub;
                  int over = -1;
                  double best = 60;  // the run nearest the hub pin's height
                  for (const auto& w : sch.wires()) {
                      const Vec2 a0 = sch.pinPosition(w.a), b0 = sch.pinPosition(w.b), k{b0.x, a0.y};
                      for (const auto& [u, v] : {std::pair{a0, k}, std::pair{k, b0}}) {  // the wire as drawn (an L)
                          const Vec2 on{at.x, u.y};
                          if (std::fabs(u.y - v.y) < 0.5 && std::fabs(u.y - hub.y) <= best && pointSegmentDistance(on, u, v) < 0.5 &&
                              (on - a0).length() > 1 && (on - b0).length() > 1)
                              over = w.id, at = on, best = std::fabs(u.y - hub.y);
                      }
                  }
                  const int junction = over < 0 ? -1 : sieda_split_wire(P(s), over, at.x, at.y);
                  if (junction >= 0) sieda_connect(P(s), junction, 0, part, tap.first);  // (else its pin is labelled below)
              }
              // Every net asked for is one net in the end: a pin left off it (a wire nobody named) gets its label.
              Json netsJoined = Json::array();
              for (const auto& [net, pins] : nets.fields()) {
                  Json loose = Json::array();
                  for (const auto& pin : pins.items()) try {
                          const PinAddress pa = pinAddress(*P(s), pin);
                          const int n = proj(s).schematic.netOf({pa.component, pa.pin});
                          if (n < 0 || proj(s).schematic.nets()[static_cast<size_t>(n)].name != net) loose.push(pin);
                      } catch (const std::exception&) {}
                  if (!loose.size() || isGround(net)) continue;
                  Json args = obj();
                  args["net"] = net;
                  args["pins"] = loose;
                  run("schematic_net_label", args, net);
                  for (const auto& pin : loose.items()) netsJoined.push(net + ": " + pin.asString(""));
              }
              Json j = obj();
              if (netsJoined.size()) j["netsJoined"] = netsJoined;  // pins labelled onto their net after the layout
              if (labelOverlaps.size()) j["labelOverlaps"] = labelOverlaps;
              if (noConnectCleared.size()) j["noConnectCleared"] = noConnectCleared;  // schematic_move_component clears them
              for (int id : ids) {  // with the nets they joined; pins: false keeps the reply short
                  Json part = componentJson(*P(s), id);
                  if (!a.get("pins").asBool(true)) {
                      Json nets = obj();
                      for (const auto& pin : part.get("pins").items()) {  // "K(1)" when the name says more than the number
                          const std::string num = pin.get("number").asString(""), name = pin.get("name").asString("");
                          nets[num.empty() || name.empty() || name == num ? (num.empty() ? name : num) : name + "(" + num + ")"] = pin.get("net");
                      }
                      part["pins"] = nets;
                  }
                  placed.push(part);
              }
              j["parts"] = placed;
              Json checks = obj();  // ahead of the parts in the reply
              checks["erc"] = run("schematic_erc", obj(), "erc");
              checks["review"] = run("schematic_validate", obj(), "review");
              j["checks"] = checks;
              if (failed.size()) j["failed"] = failed;
              return out(j);
          });
    t.add("schematic", "schematic_import_netlist", "Import a netlist", Kind::Edit, false,
          "Builds the schematic from another tool's schematic or netlist: a KiCad sheet (.kicad_sch: symbols, wires, "
          "labels, power symbols; sub-sheets read from sheets or beside path, hierarchical labels joined to their sheet "
          "pins, local labels named /Sheet/NAME), an Eagle schematic (.sch: every sheet, nets by name, gate pins on their "
          "pads), an LTspice schematic (.asc: R, C, L, sources, diodes, "
          "LEDs), a KiCad netlist (.net), Protel / Tango (Altium, OrCAD, EasyEDA, PADS) or CSV (Net, Designator, Pin). Two-pin R / C / L / D parts become built-in parts (package from the footprint's "
          "size), parts the catalog knows by part name or value come from it, the rest become library parts with the "
          "netlist's pins (package from the footprint name, else a header). Then works as schematic_build; imported "
          "lists what each designator became.",
          Schema().str("text", "The netlist text").str("path", "Or the netlist file inside the root")
              .object("sheets", "KiCad sub-sheets by file name: {\"power.kicad_sch\": \"(kicad_sch …)\"} (else read beside path)")
              .object("map", "Library mapping: their part name, value or footprint → a catalog / library part or a kind (resistor, capacitor, inductor, diode, led)"),
          [](McpServer& s, const Json& a) {
              const std::string text = a.has("text") ? argStr(a, "text") : readFileArg(s, requireStr(a, "path")).asString();
              ParsedNetlist nl;
              try {
                  nl = parseNetlist(text, sheetLoader(s, a.get("sheets"), argStr(a, "path")));
              } catch (const std::exception& e) {
                  throw ToolError(std::string("Netlist not read: ") + e.what());
              }
              return out(importNetlist(s, nl, a.get("map")));
          });
    t.add("schematic", "schematic_set_erc_severity", "ERC severity", Kind::Edit, true,
          "Reports an ERC rule as error, warning, info or off (\"default\" restores it).",
          Schema().str("code", "Rule code, e.g. ERC_UNCONNECTED_PIN", true)
              .str("level", "error, warning, info, off or default", true, {"error", "warning", "info", "off", "default"}),
          [](McpServer& s, const Json& a) {
              check(sieda_set_erc_severity(P(s), requireStr(a, "code").c_str(), requireStr(a, "level").c_str()) == 1,
                    "Unknown rule code or level");
              Json j = obj();
              j["ok"] = true;
              return out(j);
          });
    t.add("schematic", "schematic_annotate", "Annotate", Kind::Edit, false,
          "Re-numbers designators in reading order (rows or columns), optionally keeping existing ones or numbering "
          "by sheet (R101, R201…).",
          Schema()
              .str("order", "rows (default) or columns", false, {"rows", "columns"})
              .boolean("keepExisting", "Only number parts without a proper designator")
              .boolean("sheetNumbering", "Number by sheet (R101 …)")
              .boolean("packUnits", "Re-assign interchangeable units of multi-unit parts to packages"),
          [](McpServer& s, const Json& a) { return out(takeJson(sieda_annotate(P(s), optionsFrom(a).c_str()))); });
    t.add("schematic", "schematic_find", "Find", Kind::Read, true,
          "Finds text in designators, values, net labels, net names (and pins) across every sheet.",
          Schema()
              .str("text", "Text to find", true)
              .boolean("matchCase", "Case-sensitive")
              .boolean("wholeWord", "Whole words only")
              .array("fields", "Fields: ref, value, label, net, pin", "string"),
          [](McpServer& s, const Json& a) { return out(takeJson(sieda_schematic_find(P(s), optionsFrom(a).c_str()))); });
    t.add("schematic", "schematic_replace", "Replace", Kind::Edit, false,
          "Replaces text in part values and net label names. Returns the number of fields changed.",
          Schema()
              .str("text", "Text to find", true)
              .str("replacement", "Replacement text", true)
              .boolean("matchCase", "Case-sensitive")
              .boolean("wholeWord", "Whole words only")
              .array("fields", "value and/or label", "string"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              j["changed"] = sieda_schematic_replace(P(s), optionsFrom(a).c_str());
              return out(j);
          });
    t.add("schematic", "schematic_align", "Align components", Kind::Edit, false,
          "Aligns or distributes parts: left, right, top, bottom, centerX, centerY, distributeX, distributeY.",
          Schema()
              .array("refs", "Designators", "string", true)
              .str("mode", "Alignment", true, {"left", "right", "top", "bottom", "centerX", "centerY", "distributeX", "distributeY"})
              .boolean("byOutline", "Align the symbol outlines instead of the origins"),
          [](McpServer& s, const Json& a) {
              Json ids = Json::array();
              for (const auto& r : a.get("refs").items()) ids.push(componentId(*P(s), r));
              const std::string mode = requireStr(a, "mode");
              const int n = argBool(a, "byOutline", false) ? sieda_align_outlines(P(s), ids.dump().c_str(), mode.c_str())
                                                           : sieda_align_components(P(s), ids.dump().c_str(), mode.c_str());
              check(n >= 0, "Could not align");
              Json j = obj();
              j["moved"] = n;
              return out(j);
          });
    // ---- sheets & hierarchy
    t.add("schematic", "schematic_sheets", "Sheets", Kind::Read, true,
          "The sheets with hierarchy (parent, depth), component counts, ports and the active sheet.", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_sheets_json(P(s)))); });
    t.add("schematic", "schematic_block", "Approved design blocks", Kind::Edit, false,
          "Reusable design blocks. save writes the parts refs (with the wires between them, labels, directives) to a "
          "block file at path inside the root, approved by approvedBy (optional) with a fingerprint of its content. "
          "place puts a block file's parts on the active sheet, offset by x / y (new designators), and says whether "
          "it is still the approved block (approved false when unapproved or changed since).",
          Schema().str("action", "save or place", true).str("path", "Block file inside the root", true)
              .array("refs", "save: the parts' designators", "string").str("approvedBy", "save: who approved the block")
              .num("x", "place: offset x").num("y", "place: offset y"),
          [](McpServer& s, const Json& a) {
              auto fingerprint = [](const Json& clip) {
                  uint64_t h = 1469598103934665603ull;  // FNV-1a
                  for (const unsigned char ch : clip.dump()) h = (h ^ ch) * 1099511628211ull;
                  char text[17];
                  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
                  return std::string(text);
              };
              Schematic& sch = P(s)->project.schematic;
              const std::string action = requireStr(a, "action"), path = requireStr(a, "path");
              Json j = obj();
              if (action == "save") {
                  std::vector<int> ids;
                  for (const auto& r : a.get("refs").items()) {
                      const Component* c = sch.findByRef(r.asString(""));
                      check(c != nullptr, "No part " + r.asString(""));
                      ids.push_back(c->id);
                  }
                  check(!ids.empty(), "save needs refs");
                  const Json clip = sch.copyComponents(ids);
                  Json block = obj();
                  block["format"] = "sieda.block/1", block["clip"] = clip, block["hash"] = fingerprint(clip);
                  const std::string by = argStr(a, "approvedBy", "");
                  if (!by.empty()) {
                      const std::time_t now = std::time(nullptr);
                      char date[11];
                      std::strftime(date, sizeof date, "%Y-%m-%d", std::gmtime(&now));
                      block["approvedBy"] = by, block["approvedDate"] = date;
                  }
                  j["written"] = s.writeFile(path, block.dump(true));
                  j["parts"] = static_cast<int>(ids.size()), j["hash"] = fingerprint(clip), j["approved"] = !by.empty();
              } else if (action == "place") {
                  const Json block = Json::parse(readFileArg(s, path).asString());
                  check(block.get("format").asString("") == "sieda.block/1", path + " is not a SiEDA block file");
                  const Json& clip = block.get("clip");
                  const bool intact = fingerprint(clip) == block.get("hash").asString("");
                  PasteOptions options;
                  options.offset = {argNum(a, "x", 0), argNum(a, "y", 0)};
                  Json refs = Json::array();
                  for (int id : sch.pasteComponents(clip, options))
                      if (const Component* c = sch.find(id)) refs.push(c->ref);
                  P(s)->project.schematicChanged();
                  j["placed"] = refs, j["intact"] = intact;
                  j["approved"] = intact && block.has("approvedBy");
                  if (block.has("approvedBy")) j["approvedBy"] = block.get("approvedBy"), j["approvedDate"] = block.get("approvedDate");
              } else {
                  throw ToolError("action is save or place");
              }
              return out(j);
          });
    t.add("schematic", "schematic_sheet_edit", "Edit sheets", Kind::Edit, false,
          "Sheet commands (multi-page schematics): add (name, parent; activate: true makes the new page the one new "
          "parts go to), rename (sheet, name), activate (sheet: new parts go there), remove "
          "(sheet, deleteContents), set_parent (sheet, parent), repeat (sheet, count: a multi-channel block), "
          "place_entries (sheet = child, x, y: its sheet entries on the parent), size (sheet, size \"A4\"… or \"\").",
          Schema()
              .str("action", "What to do", true, {"add", "rename", "activate", "remove", "set_parent", "repeat", "place_entries", "size"})
              .integer("sheet", "Sheet id")
              .str("name", "Sheet name")
              .integer("parent", "Parent sheet id (0 = top level)")
              .integer("count", "Repeat count")
              .boolean("deleteContents", "Remove a sheet with its components")
              .num("x", "Entries x")
              .num("y", "Entries y")
              .str("size", "Template size")
              .boolean("activate", "add: make the new sheet active"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string act = requireStr(a, "action");
              const int sheet = argInt(a, "sheet", 0);
              Json j = obj();
              if (act == "add") {
                  const int id = sieda_add_sheet(p, requireStr(a, "name").c_str(), argInt(a, "parent", 0));
                  check(id >= 0, "Could not add the sheet (is the name unique?)");
                  if (argBool(a, "activate", false)) check(sieda_set_active_sheet(p, id) == 1, "Could not activate the sheet");
                  j["sheet"] = id;
              } else if (act == "rename") {
                  check(sieda_rename_sheet(p, sheet, requireStr(a, "name").c_str()) == 1, "Could not rename");
              } else if (act == "activate") {
                  check(sieda_set_active_sheet(p, sheet) == 1, "Unknown sheet");
              } else if (act == "remove") {
                  check(sieda_remove_sheet(p, sheet, argBool(a, "deleteContents", false) ? 1 : 0) == 1,
                        "Could not remove (the last sheet, or it holds components: deleteContents)");
              } else if (act == "set_parent") {
                  check(sieda_set_sheet_parent(p, sheet, argInt(a, "parent", 0)) == 1, "Could not re-parent (a cycle?)");
              } else if (act == "repeat") {
                  const int n = sieda_repeat_sheet(p, sheet, argInt(a, "count", 1));
                  check(n >= 0, "Could not repeat the sheet");
                  j["channels"] = n;
              } else if (act == "place_entries") {
                  const int n = sieda_place_sheet_entries(p, sheet, argNum(a, "x", 0), argNum(a, "y", 0));
                  check(n >= 0, "Unknown or top-level sheet");
                  j["entriesAdded"] = n;
              } else if (act == "size") {
                  check(sieda_set_sheet_size(p, sheet, argStr(a, "size", "").c_str()) == 1, "Unknown sheet or size");
              }
              j["sheets"] = takeJson(sieda_sheets_json(p));
              return out(j);
          });
    t.add("pcb", "pcb_boards", "Boards", Kind::Edit, false,
          "Several boards in one project (one schematic): list, add (name, activate), activate (board: the pcb_* tools "
          "work on it), assign (refs, board: the parts move there unplaced; place them with pcb_update_from_schematic "
          "or pcb_autoplace), rename (board, name), remove (board). Boards are numbered from 0.",
          Schema()
              .str("action", "What to do", true, {"list", "add", "activate", "assign", "rename", "remove"})
              .integer("board", "Board index")
              .str("name", "Board name")
              .array("refs", "Designators to assign", "string")
              .boolean("activate", "add: make the new board active"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string act = requireStr(a, "action");
              const int board = argInt(a, "board", 0);
              Json j = obj();
              if (act == "add") {
                  const int i = sieda_add_board(p, argStr(a, "name", "").c_str());
                  check(i >= 0, "Could not add a board");
                  if (argBool(a, "activate", false)) sieda_set_active_board(p, i);
                  j["board"] = i;
              } else if (act == "activate") {
                  check(sieda_set_active_board(p, board) == 1, "Unknown board");
              } else if (act == "assign") {
                  std::vector<int32_t> ids;
                  for (const auto& r : a.get("refs").items()) ids.push_back(componentId(*p, r));
                  check(!ids.empty(), "Give refs");
                  check(board >= 0 && board < p->project.boardCount(), "Unknown board");
                  j["moved"] = sieda_assign_to_board(p, ids.data(), static_cast<int32_t>(ids.size()), board);
              } else if (act == "rename") {
                  check(sieda_rename_board(p, board, requireStr(a, "name").c_str()) == 1, "Unknown board or empty name");
              } else if (act == "remove") {
                  check(sieda_remove_board(p, board) == 1, "Could not remove (the last board, or an unknown index)");
              }
              j["boards"] = takeJson(sieda_boards_json(p));
              return out(j);
          });
    t.add("schematic", "schematic_move_to_sheet", "Move to sheet", Kind::Edit, false,
          "Moves parts to another sheet (wires that would cross sheets are removed; join them with labels).",
          Schema().array("refs", "Designators", "string", true).integer("sheet", "Target sheet id", true),
          [](McpServer& s, const Json& a) {
              std::vector<int32_t> ids;
              for (const auto& r : a.get("refs").items()) ids.push_back(componentId(*P(s), r));
              Json j = obj();
              j["moved"] = sieda_move_to_sheet(P(s), ids.data(), static_cast<int32_t>(ids.size()), argInt(a, "sheet", 1));
              return out(j);
          });
    t.add("schematic", "schematic_set_label_scope", "Label scope", Kind::Edit, true,
          "Sets a net label's scope: global, local (its sheet), port (hierarchical port) or entry (sheet entry into "
          "targetSheet).",
          Schema()
              .str("label", "The label's designator (from schematic_list_components with netSymbols)", true)
              .str("scope", "Scope", true, {"global", "local", "port", "entry"})
              .integer("targetSheet", "Child sheet for an entry"),
          [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("label"));
              check(sieda_set_label_scope(P(s), id, requireStr(a, "scope").c_str(), argInt(a, "targetSheet", 0)) == 1,
                    "Not a net label, or an invalid scope");
              return out(componentJson(*P(s), id, false));
          });
    // ---- buses
    t.add("schematic", "schematic_add_bus", "Add bus", Kind::Edit, false,
          "Draws a named bus (\"D[0..7]\", \"A[15..0]\", \"D[0..3],WR\") on the active sheet; then "
          "schematic_bus_connect wires its members to a part.",
          Schema()
              .str("name", "Bus name with members", true)
              .array("points", "Polyline [{\"x\":0,\"y\":0},{\"x\":200,\"y\":0}] (grid units)", "object", true),
          [](McpServer& s, const Json& a) {
              const int id = sieda_add_bus(P(s), requireStr(a, "name").c_str(), a.get("points").dump().c_str());
              check(id >= 0, "Could not add the bus (2–256 points)");
              Json j = obj();
              j["bus"] = id;
              j["members"] = takeJson(sieda_expand_bus(argStr(a, "name").c_str()));
              return out(j);
          });
    t.add("schematic", "schematic_bus_connect", "Connect bus", Kind::Edit, false,
          "Wires bus members to a part: members named like pins join those pins (D0 → pin D0), else the part's open "
          "pins in order, each through a bus entry. Or rips entries out for given members.",
          Schema()
              .integer("bus", "Bus id", true)
              .str("ref", "Part to connect")
              .array("members", "Rip entries for these members only (no ref)", "string")
              .str("scope", "Entry scope: local (default), global, port", false, {"local", "global", "port"}),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              const std::string scope = argStr(a, "scope", "local");
              if (a.has("ref")) {
                  const int n = sieda_connect_bus_to_part(P(s), argInt(a, "bus", 0), componentId(*P(s), a.get("ref")), scope.c_str());
                  check(n >= 0, "Unknown bus or part");
                  j["connections"] = n;
              } else {
                  const std::string members = a.get("members").isArray() ? a.get("members").dump() : std::string("[]");
                  const int n = sieda_rip_bus_entries(P(s), argInt(a, "bus", 0), members.c_str(), scope.c_str());
                  check(n >= 0, "Unknown bus");
                  j["entries"] = n;
              }
              return out(j);
          });
    t.add("schematic", "schematic_bus_labels", "Bus labels on pins", Kind::Edit, false,
          "Puts one net label per bus member on the given pins of a part, in order (D[0..7] on 8 pins).",
          Schema()
              .str("ref", "Part", true)
              .array("pins", "Pin names / numbers in member order", "string", true)
              .str("bus", "Bus notation, e.g. \"D[0..7]\"", true)
              .str("scope", "global (default), local or port", false, {"global", "local", "port"}),
          [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              std::vector<int32_t> pins;
              for (const auto& pn : a.get("pins").items()) {
                  const std::string name = pn.isString() ? pn.asString() : pn.dump();
                  const int idx = sieda_find_pin(P(s), id, name.c_str());
                  if (idx < 0) throw ToolError("No pin " + pn.dump() + " on " + argStr(a, "ref"));
                  pins.push_back(idx);
              }
              const int n = sieda_add_bus_labels(P(s), id, pins.data(), static_cast<int32_t>(pins.size()),
                                                 requireStr(a, "bus").c_str(), argStr(a, "scope", "global").c_str());
              check(n >= 0, "Could not add the labels (pin count must match the members)");
              Json j = obj();
              j["labels"] = n;
              return out(j);
          });
    // ---- net classes & directives
    t.add("schematic", "schematic_net_class", "Net class", Kind::Edit, true,
          "Defines (or removes) a net class with a track width and clearance (mm, 0 = board default); assign it to "
          "nets with schematic_add_directive. The rules carry to the board.",
          Schema()
              .str("name", "Class name, e.g. \"POWER\"", true)
              .num("trackWidth", "Track width (mm)")
              .num("clearance", "Clearance (mm)")
              .boolean("remove", "Remove the class"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              if (argBool(a, "remove", false)) {
                  check(sieda_remove_net_class(P(s), requireStr(a, "name").c_str()) == 1, "Unknown class");
              } else {
                  Json c = obj();
                  c["name"] = requireStr(a, "name");
                  c["trackWidth"] = argNum(a, "trackWidth", 0);
                  c["clearance"] = argNum(a, "clearance", 0);
                  check(sieda_set_net_class(P(s), c.dump().c_str()) == 1, "Could not set the class");
              }
              j["netRules"] = takeJson(sieda_net_rules_json(P(s)));
              return out(j);
          });
    t.add("schematic", "schematic_add_directive", "Net directive", Kind::Edit, false,
          "Attaches a rule to the net of a pin: a net class, a differential pair (with its X_P / X_N, X_DP / X_DN or X_DM partner), or "
          "its own track width / clearance (mm).",
          Schema()
              .pin("pin", "A pin on the net.", true)
              .str("netClass", "Net class name")
              .boolean("diffPair", "Pair this net with its _P/_N (or _DP/_DM) partner")
              .num("trackWidth", "Track width (mm)")
              .num("clearance", "Clearance (mm)"),
          [](McpServer& s, const Json& a) {
              const PinAddress pa = pinAddress(*P(s), a.get("pin"));
              Json d = obj();
              d["component"] = pa.component;
              d["pin"] = pa.pin;
              if (a.has("netClass")) d["netClass"] = argStr(a, "netClass");
              d["diffPair"] = argBool(a, "diffPair", false);
              d["trackWidth"] = argNum(a, "trackWidth", 0);
              d["clearance"] = argNum(a, "clearance", 0);
              const int id = sieda_add_directive(P(s), d.dump().c_str());
              check(id >= 0, "Could not add the directive (unknown class?)");
              Json j = obj();
              j["directive"] = id;
              j["netRules"] = takeJson(sieda_net_rules_json(P(s)));
              return out(j);
          });
    t.add("schematic", "schematic_net_rules", "Net rules", Kind::Read, true,
          "The net classes, differential pairs and per-net widths / clearances the schematic gives the board.", Schema(),
          [](McpServer& s, const Json&) {
              Json j = obj();
              j["netRules"] = takeJson(sieda_net_rules_json(P(s)));
              return out(j);
          });
    // ---- variants
    t.add("schematic", "schematic_variants", "Variants", Kind::Read, true,
          "Assembly variants (fitted / DNP parts, value overrides) and the active one.", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_variants_json(P(s)))); });
    t.add("schematic", "schematic_variant_matrix", "Compare variants", Kind::Read, true,
          "Variants side by side: every part a variant changes with its fitting and value in each variant, and "
          "per-variant totals (fitted, not fitted, value changes).", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_variant_matrix_json(P(s)))); });
    t.add("schematic", "schematic_variant_edit", "Edit variants", Kind::Edit, false,
          "Variant commands: add (name, copyFrom), remove (name), rename (name, newName), describe (name, "
          "description), activate (name; \"\" = base design), set_part (name, ref, fitted true/false/null, value).",
          Schema()
              .str("action", "What to do", true, {"add", "remove", "rename", "describe", "activate", "set_part"})
              .str("name", "Variant name")
              .str("newName", "New name")
              .str("copyFrom", "Variant to copy")
              .str("description", "Description")
              .str("ref", "Part designator")
              .any("fitted", "true fitted, false not fitted (DNP), null as the base design")
              .str("value", "Value override (\"\" clears)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string act = requireStr(a, "action"), name = argStr(a, "name", "");
              bool ok = false;
              if (act == "add") ok = sieda_add_variant(p, name.c_str(), argStr(a, "copyFrom", "").c_str()) == 1;
              else if (act == "remove") ok = sieda_remove_variant(p, name.c_str()) == 1;
              else if (act == "rename") ok = sieda_rename_variant(p, name.c_str(), requireStr(a, "newName").c_str()) == 1;
              else if (act == "describe") ok = sieda_set_variant_description(p, name.c_str(), argStr(a, "description").c_str()) == 1;
              else if (act == "activate") ok = sieda_set_active_variant(p, name.c_str()) == 1;
              else if (act == "set_part") {
                  const Json& f = a.get("fitted");
                  const int fitted = f.type() == Json::Type::Bool ? (f.asBool() ? 1 : 0) : -1;
                  const std::string value = argStr(a, "value", "");
                  ok = sieda_set_variant_part(p, name.c_str(), componentId(*p, a.get("ref")), fitted,
                                              a.has("value") ? value.c_str() : nullptr) == 1;
              }
              check(ok, "Variant " + act + " failed (unknown or taken name?)");
              return out(takeJson(sieda_variants_json(p)));
          });
}

// ================================================================================================= library tools

/// library_search over the standard catalog (its arguments): parts, total, notes.
Json libraryCatalogSearch(const Json& a) {
    std::vector<std::string> words, cats, pkgs, mfrs;
    int lo = 0, hi = 1 << 30;
    bool range = false;
    // "4 pin", "4-pin", "8 way", "6 position" are a pin count: pins:4.
    static const std::regex count(R"((^|\s)(\d{1,3})\s*-?\s*(pins?|ways?|positions?|pos)\b)");
    // "header 2", "connector 4": the count after a connector word too.
    static const std::regex after(R"(\b(headers?|connectors?|conn|terminals?|sockets?)\s+(\d{1,3})\b)");
    std::istringstream in(std::regex_replace(std::regex_replace(lowerStr(argStr(a, "query", "")), count, "$1pins:$2"), after, "$1 pins:$2"));
    for (std::string w; in >> w;) {
        const size_t colon = w.find(':');
        if (colon != std::string::npos && colon > 0 && colon + 1 < w.size()) {
            const std::string k = w.substr(0, colon), v = w.substr(colon + 1);
            if (k == "cat" || k == "category") { cats.push_back(v); continue; }
            if (k == "pkg" || k == "package") { pkgs.push_back(compact(v)); continue; }
            if (k == "mfr" || k == "maker" || k == "manufacturer") { mfrs.push_back(v); continue; }
            if ((k == "pins" || k == "pin") && pinRange(v, &lo, &hi)) { range = true; continue; }
        }
        words.push_back(w);
    }
    if (a.has("category")) cats.push_back(lowerStr(argStr(a, "category")));
    if (a.has("package")) pkgs.push_back(compact(argStr(a, "package")));
    if (a.has("manufacturer")) mfrs.push_back(lowerStr(argStr(a, "manufacturer")));
    if (a.has("pins")) range = pinRange(argStr(a, "pins"), &lo, &hi) || range;
    const int limit = std::clamp(argInt(a, "limit", 25), 1, 200);
    Json j = obj();
    if (words.empty() && cats.empty() && pkgs.empty() && mfrs.empty() && !range) {
        std::map<std::string, int> counts;
        for (const auto& p : standardParts()) ++counts[p.category];
        Json list = Json::array();
        for (const auto& [c, n] : counts) {
            Json e = obj();
            e["category"] = c;
            e["parts"] = n;
            list.push(e);
        }
        j["categories"] = list;
        j["total"] = standardParts().size();
        return j;
    }
    Json list = Json::array();
    int total = 0;
    auto entry = [](const StandardPart& p) {
        const int pins = static_cast<int>(p.spec.pins.size());
        Json e = obj();
        e["name"] = p.spec.name;
        e["category"] = p.category;
        e["manufacturer"] = p.spec.manufacturer;
        e["description"] = p.spec.description;
        e["package"] = !p.footprint.empty() ? p.footprint  // the footprint its pads follow
                                            : p.spec.package.type + "-" + std::to_string(p.spec.package.pinCount ? p.spec.package.pinCount : pins);
        e["pins"] = pins;
        if (pins <= 8) {  // a small part's pins by name (number), as schematic_build takes them
            Json names = Json::array();
            for (const auto& pin : p.spec.pins) names.push(pin.name + "(" + pin.number + ")");
            e["pinNames"] = names;
        }
        return e;
    };
    std::vector<std::pair<size_t, const StandardPart*>> partial, ranked, byPackage;  // some of the words / all of them
    for (const auto& p : standardParts()) {
        const std::string cat = lowerStr(p.category), mfr = lowerStr(p.spec.manufacturer);
        const std::string pkg = compact(p.spec.package.type + std::to_string(p.spec.package.pinCount) + p.footprint);
        const int pins = static_cast<int>(p.spec.pins.size());
        bool ok = true;
        for (const auto& c : cats) ok = ok && cat.find(c) != std::string::npos;
        for (const auto& m : mfrs) ok = ok && mfr.find(m) != std::string::npos;
        for (const auto& k : pkgs) ok = ok && pkg.find(k) != std::string::npos;
        if (range) ok = ok && pins >= lo && pins <= hi;
        if (!ok) continue;
        const std::string hay = lowerStr(p.spec.name + " " + p.spec.description + " " + p.category + " " +
                                         p.spec.manufacturer + " " + p.spec.package.type + "-" +
                                         std::to_string(p.spec.package.pinCount) + " " + p.footprint);
        size_t matched = 0;
        for (const auto& w : words) matched += wordMatches(hay, w);
        if (matched < words.size()) {
            if (matched) partial.push_back({matched, &p});
            continue;
        }
        ++total;
        size_t named = 0, said = 0;  // words in its name rank it first ("usb-c": the receptacle before MCUs with USB)
        for (const auto& w : words) named += wordMatches(lowerStr(p.spec.name), w);
        for (const auto& w : words) said += wordMatches(lowerStr(p.spec.name + " " + p.spec.description + " " + p.category + " " + p.spec.manufacturer), w);
        (said == words.size() ? ranked : byPackage).push_back({named, &p});
    }
    // A word only a package names ("header": a module in a HEADER-4 package) lists those parts only when
    // nothing says it in its name or description.
    if (ranked.empty()) ranked.swap(byPackage);
    total = static_cast<int>(ranked.size());
    std::stable_sort(ranked.begin(), ranked.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
    // A part-number-like query ("usb-c", "esp32"): the parts with every word in their name alone (an MCU whose
    // description says USB-C is noise there); a feature word ("ble") still lists every part that has it.
    const bool numberLike = std::any_of(words.begin(), words.end(), [](const std::string& w) {
        return std::any_of(w.begin(), w.end(), [](unsigned char c) { return std::isdigit(c) || c == '-'; });
    });
    if (numberLike && !ranked.empty() && ranked.front().first == words.size()) {
        const size_t all = ranked.size();
        ranked.erase(std::find_if(ranked.begin(), ranked.end(), [&](const auto& r) { return r.first < words.size(); }), ranked.end());
        if (all > ranked.size()) j["alsoInDescriptions"] = static_cast<int>(all - ranked.size());
        total = static_cast<int>(ranked.size());
    }
    for (const auto& [n, part] : ranked)
        if (static_cast<int>(list.size()) < limit) list.push(entry(*part));
    std::string kindWord;  // a built-in kind ("led 0805"): partial matches would only be noise
    for (const auto& w : words)
        for (const char* k : {"led", "leds", "resistor", "resistors", "capacitor", "capacitors", "inductor"})
            if (w == k) kindWord = w;
    if (!total && !kindWord.empty())
        j["builtin"] = "\"" + kindWord + "\" is a built-in kind, not a catalog part: schematic_build parts [{\"ref\": \"D1\", \"kind\": "
                       "\"led\", \"value\": \"Green\", \"package\": \"0805\"}] (kinds: resistor, capacitor, inductor, led; "
                       "library_builtin_kinds lists them with packages).";
    static const std::regex genericDiode(R"(^1n(4\d{3}|5[48]\d{2})$)");  // 1N4007, 1N4148, 1N5819: plain two-pin diodes
    for (const auto& w : words)
        if (!total && std::regex_match(w, genericDiode))
            j["builtin"] = std::string("A plain two-pin diode: schematic_build parts [{\"ref\": \"D1\", \"kind\": \"diode\", \"value\": \"1N") +
                           w.substr(2) + "\"}] (pins A / K; library_builtin_kinds lists its packages), or the catalog's SS14 Schottky.";
    if (!total && !partial.empty() && kindWord.empty()) {  // nothing has every word: the parts with the most of them
        size_t best = 0;
        for (const auto& [n, p] : partial) best = std::max(best, n);
        for (const auto& [n, p] : partial)
            if (n == best && static_cast<int>(list.size()) < limit) list.push(entry(*p));
        j["closest"] = std::to_string(best) + " of " + std::to_string(words.size()) + " words match";
    }
    j["total"] = total;
    j["parts"] = list;
    for (const auto& w : words)  // programming / debug headers are plain pin headers in the catalog
        if (w == "isp" || w == "icsp" || w == "ftdi" || w == "swd" || w == "jtag" || w == "programming")
            j["hint"] = "Programming headers are generic pin headers: AVR ISP = PinHeader_2x03_P2.54mm, FTDI serial = "
                        "PinHeader_1x06_P2.54mm, ARM SWD = PinHeader_2x05_P2.54mm (or a 1.27 mm Cortex header from library_create_part), JTAG = "
                        "PinHeader_2x10_P2.54mm; name the pins with net labels.";
    if (total > limit) j["note"] = "More parts match: refine the query or raise limit.";
    if (!total) {  // say which word narrowed it to nothing
        std::string per;
        for (const auto& w : words) {
            int n = 0;
            for (const auto& p : standardParts())
                n += wordMatches(lowerStr(p.spec.name + " " + p.spec.description + " " + p.category), w);
            per += (per.empty() ? "" : ", ") + w + " " + std::to_string(n);
        }
        j["note"] = "Nothing matches every word" + (per.empty() ? std::string() : " (parts per word: " + per + ")") +
                    (list.size() ? "; parts lists those matching the most words" : "") +
                    ". Use fewer words or a cat: filter (an empty query lists the categories); resistors, "
                    "capacitors, LEDs, switches and generic connectors are schematic_add_component kinds.";
    }
    return j;
}

void libraryTools(Table& t) {
    t.add("library", "library_builtin_kinds", "Built-in kinds", Kind::Read, true,
          "The built-in component kinds (schematic_add_component) with designator prefix, default value, footprint "
          "and pin names.",
          Schema(), [](McpServer&, const Json&) {
              Json list = Json::array();
              for (const auto& d : Library::instance().components()) {
                  if (d.kind == ComponentKind::Custom || d.kind == ComponentKind::PartUnit || d.kind == ComponentKind::Junction) continue;
                  Json j = obj();
                  j["kind"] = kindName(static_cast<int>(d.kind));
                  j["name"] = d.name;
                  j["refPrefix"] = d.refPrefix;
                  j["defaultValue"] = d.defaultValue;
                  j["footprint"] = d.footprint;
                  j["simulated"] = d.simulated;
                  Json pins = Json::array();
                  for (const auto& p : d.pins) pins.push(p.name);
                  j["pins"] = pins;
                  list.push(j);
              }
              Json j = obj();
              j["kinds"] = list;
              return out(j);
          });
    t.add("library", "library_search", "Search catalog", Kind::Read, true,
          "Searches the standard-part catalog (regulators, MCUs, op-amps, logic, interfaces, sensors, connectors …). "
          "query words must all match name / description / category / manufacturer / package (a word of 3 letters or "
          "fewer starts a word there, longer ones ignore separators: \"wifi\" finds Wi-Fi, \"ble\" Bluetooth LE); the query also takes "
          "filters as in the app: \"cat:regulators pkg:sot-23 pins:3-5 mfr:ti\" (\"4 pin\" / \"8-way\" = pins:4 / pins:8). "
          "Separate filter arguments do the same. Empty query: the categories with their part counts.",
          Schema()
              .str("query", "Search words and filters, e.g. \"ldo 3.3 pkg:sot23\"")
              .str("category", "Category contains")
              .str("package", "Package contains (SOT-23 = sot23)")
              .str("manufacturer", "Manufacturer contains")
              .str("pins", "Pin count: \"8\", \"6-10\", \">40\", \"<8\"")
              .integer("limit", "Most results (default 25, at most 200)"),
          [](McpServer& s, const Json& a) {
              Json found = libraryCatalogSearch(a);
              // The user's own KiCad libraries (when the server was given some): "Library:Symbol" names library_add_part places.
              if (!s.options().libraryDirs.empty() && !argStr(a, "query", "").empty()) {
                  ExternalLibraries& ext = ExternalLibraries::shared(s.options().libraryDirs);
                  Json list = Json::array();
                  for (const ExternalPart* p : ext.search(argStr(a, "query"), static_cast<size_t>(std::clamp(argInt(a, "limit", 25), 1, 200)))) {
                      Json e = obj();
                      e["name"] = p->id;
                      e["description"] = p->description;
                      e["footprint"] = p->footprint;
                      e["pins"] = p->pins;
                      list.push(e);
                  }
                  found["libraryParts"] = list;
                  found["librarySize"] = static_cast<double>(ext.parts().size());
              }
              return out(found);
          });
    t.add("library", "library_part_details", "Part details", Kind::Read, true,
          "A catalog part's full description: manufacturer, package, units and every pin (number, name, type).",
          Schema().str("name", "Part name from library_search, e.g. \"NE555\", \"AMS1117-3.3\"", true),
          [](McpServer&, const Json& a) {
              const StandardPart* sp = &catalogPart(requireStr(a, "name"));
              Json j = obj();
              j["name"] = sp->spec.name;
              j["category"] = sp->category;
              j["manufacturer"] = sp->spec.manufacturer;
              j["description"] = sp->spec.description;
              j["refPrefix"] = sp->spec.refPrefix;
              j["defaultValue"] = sp->spec.defaultValue;
              j["datasheet"] = sp->spec.datasheet;
              j["package"] = !sp->footprint.empty() ? sp->footprint : sp->spec.package.type + "-" + std::to_string(sp->spec.package.pinCount);
              Json units = Json::array();
              for (const auto& u : sp->spec.units) {
                  Json uj = obj();
                  uj["name"] = u.name;
                  Json pins = Json::array();
                  for (const auto& pn : u.pins) pins.push(pn);
                  uj["pins"] = pins;
                  units.push(uj);
              }
              if (units.size()) j["units"] = units;
              j["pins"] = pinsOfSpec(sp->spec);
              return out(j);
          });
    t.add("library", "library_add_part", "Place catalog part", Kind::Edit, false,
          "Places a standard catalog part (an IC, regulator, connector …) or a part of the project's own library "
          "(library_create_part, by name) on the schematic, or a \"Library:Symbol\" "
          "from the user's own KiCad libraries (library_search libraryParts; its footprint comes with it). units: true places a "
          "multi-unit part by gates (unit A first; the hidden package carries every pin). Returns the part with its "
          "pins.",
          Schema()
              .str("name", "Catalog part name", true)
              .str("value", "Value (default the part name)")
              .str("ref", "Designator (default next free, e.g. U2)")
              .num("x", "Schematic x")
              .num("y", "Schematic y")
              .integer("rotation", "Rotation (degrees)")
              .boolean("units", "Place by units (multi-unit parts)")
              .integer("sheet", "Sheet id to place it on (default: the active sheet)"),
          [](McpServer& s, const Json& a) {
              const std::string name = requireStr(a, "name");
              // "Library:Symbol" from the user's own KiCad libraries: imported (symbol + its footprint) when placed.
              std::unique_ptr<StandardPart> external;
              if (name.find(':') != std::string::npos && !s.options().libraryDirs.empty()) {
                  ImportedPart ip;
                  try {
                      ip = ExternalLibraries::shared(s.options().libraryDirs).load(name);
                  } catch (const std::exception& e) {
                      throw ToolError(e.what());
                  }
                  if (!ip.ok) throw ToolError(name + " cannot be used as it is: " + ip.error);
                  external = std::make_unique<StandardPart>();
                  external->spec = ip.spec;
              }
              // A part of the project's own library (library_create_part) by its name, as schematic_build finds it.
              if (!external && !findStandardPart(name))
                  for (const auto& id : P(s)->project.customLibrary)
                      if (const CustomPart* cp = CustomPartRegistry::instance().find(id); cp && (id == name || lowerStr(cp->spec.name) == lowerStr(name))) {
                          external = std::make_unique<StandardPart>();
                          external->spec = cp->spec;
                          break;
                      }
              const StandardPart* sp = external ? external.get() : &catalogPart(name);
              SiedaProject* p = P(s);
              if (a.has("sheet")) check(sieda_set_active_sheet(p, argInt(a, "sheet", 1)) == 1, "Unknown sheet");
              std::string partId;
              try {
                  partId = p->project.addCustomPart(sp->spec);
              } catch (const std::exception& e) {
                  throw ToolError(std::string("The part could not be generated: ") + e.what());
              }
              Vec2 at = freeSpot(p->project);
              if (a.has("x")) at.x = argNum(a, "x", at.x);
              if (a.has("y")) at.y = argNum(a, "y", at.y);
              std::string fallback = sp->spec.defaultValue.empty() ? sp->spec.name : sp->spec.defaultValue;
              if (fallback == sp->spec.name)  // a crystal / oscillator reads as its frequency ("Crystal_16MHz" → 16MHz)
                  for (const char* unit : {"MHz", "kHz"})
                      if (const size_t u = sp->spec.name.find(unit); u != std::string::npos) {
                          size_t b0 = u;
                          while (b0 > 0 && (std::isdigit(static_cast<unsigned char>(sp->spec.name[b0 - 1])) || sp->spec.name[b0 - 1] == '.')) --b0;
                          if (b0 < u) fallback = sp->spec.name.substr(b0, u - b0) + unit;
                          break;
                      }
              const std::string ref = argStr(a, "ref", "");
              if (fallback == sp->spec.name && ref.rfind("SW", 0) == 0) fallback = "SW_Push";  // a switch, not its catalog name
              const std::string value = argStr(a, "value", fallback);
              const int id = argBool(a, "units", false) && !sp->spec.units.empty()
                                 ? sieda_add_custom_units(p, partId.c_str(), value.c_str(), at.x, at.y, argInt(a, "rotation", 0),
                                                          ref.empty() ? nullptr : ref.c_str())
                                 : sieda_add_custom_component(p, partId.c_str(), value.c_str(), at.x, at.y,
                                                              argInt(a, "rotation", 0), ref.empty() ? nullptr : ref.c_str());
              check(id >= 0, "Could not place the part");
              return out(componentJson(*p, id));
          });
    t.add("library", "library_create_part", "Create custom part", Kind::Edit, false,
          "Creates a part from a datasheet pin list: symbol and footprint are generated from the package. pins: "
          "[{number, name, type}] with type power_in, input, output, bidirectional, passive, power_out, "
          "open_collector or no_connect. package: {type: SOIC|TSSOP|DIP|QFN|LQFP|SOT23|HEADER|TO220|…, pinCount, "
          "pitch?, bodySize?}. place: true also puts it on the schematic. pinTable: the pins as CSV / TSV text instead "
          "(a header names the columns, e.g. an STM32CubeMX pin list; else number, name, type, description).",
          Schema()
              .str("name", "Part name / number", true)
              .str("manufacturer", "Manufacturer")
              .str("description", "Description")
              .str("refPrefix", "Designator prefix (default U)")
              .str("defaultValue", "Default value")
              .str("datasheet", "Datasheet URL or file name")
              .object("package", "Package {type, pinCount, pitch, bodySize}", true)
              .array("pins", "Pins [{number, name, type, description}]", "object")
              .str("pinTable", "Pins as a CSV / TSV pin table, instead of pins")
              .array("units", "Optional gates [{name, pins:[numbers]}]", "object")
              .boolean("place", "Also place it on the schematic")
              .str("ref", "Designator when placing")
              .num("x", "x when placing")
              .num("y", "y when placing"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              Json args = a;
              if (a.has("pinTable")) {
                  args["pins"] = takeJson(sieda_parse_pin_table(argStr(a, "pinTable").c_str()));
                  check(args.get("pins").size() > 0, "The pin table has no rows with a pin number");
              }
              const std::string spec = optionsFrom(args, {"place", "ref", "x", "y", "pinTable"});
              char* err = nullptr;
              char* res = sieda_custom_part_register(p, spec.c_str(), &err);
              if (!res) throw ToolError("Invalid part: " + takeText(err));
              const Json part = takeJson(res);
              Json j = obj();
              j["partId"] = part.get("id");
              j["name"] = argStr(a, "name");
              if (argBool(a, "place", false)) {
                  Vec2 at = freeSpot(p->project);
                  if (a.has("x")) at.x = argNum(a, "x", at.x);
                  if (a.has("y")) at.y = argNum(a, "y", at.y);
                  const std::string ref = argStr(a, "ref", "");
                  const std::string id = part.get("id").asString("");
                  const int c = sieda_add_custom_component(p, id.c_str(), argStr(a, "defaultValue", argStr(a, "name")).c_str(),
                                                           at.x, at.y, 0, ref.empty() ? nullptr : ref.c_str());
                  check(c >= 0, "The part was created but could not be placed");
                  j["component"] = componentJson(*p, c);
              }
              Json checks = Json::array();
              const Json symbolChecks = takeJson(sieda_check_symbol(spec.c_str()));
              for (const auto& c : symbolChecks.items()) checks.push(c);
              if (checks.size()) j["symbolChecks"] = checks;
              return out(j);
          });
    t.add("library", "library_place_custom_part", "Place project part", Kind::Edit, false,
          "Places another instance of a part already in the project library (library_project_parts lists ids).",
          Schema()
              .str("partId", "Part id", true)
              .str("value", "Value")
              .str("ref", "Designator")
              .num("x", "x")
              .num("y", "y")
              .integer("rotation", "Rotation"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              Vec2 at = freeSpot(p->project);
              if (a.has("x")) at.x = argNum(a, "x", at.x);
              if (a.has("y")) at.y = argNum(a, "y", at.y);
              const std::string ref = argStr(a, "ref", "");
              const std::string value = argStr(a, "value", "");
              const int id = sieda_add_custom_component(p, requireStr(a, "partId").c_str(), value.c_str(), at.x, at.y,
                                                        argInt(a, "rotation", 0), ref.empty() ? nullptr : ref.c_str());
              check(id >= 0, "Unknown part id");
              return out(componentJson(*p, id));
          });
    t.add("library", "library_updates", "Library update notices", Kind::Edit, false,
          "Project parts whose catalog part has changed since they were added (pins, package, footprint), with how "
          "many placed instances use each. apply: names to update (\"*\" = all) — their instances switch to the "
          "catalog's current part, pins kept by number.",
          Schema().array("apply", "Part names to update, or [\"*\"]", "string"),
          [](McpServer& s, const Json& a) {
              const std::string apply = a.has("apply") ? a.get("apply").dump() : std::string();
              char* r = sieda_library_updates(P(s), apply.empty() ? nullptr : apply.c_str());
              check(r != nullptr, "apply must be a list of part names");
              return out(takeJson(r));
          });
    t.add("library", "library_project_parts", "Project parts", Kind::Read, true,
          "The custom and catalog parts in the project's library (ids, names, pin counts).", Schema(),
          [](McpServer& s, const Json&) {
              Json list = Json::array();
              for (const auto& id : proj(s).customLibrary) {
                  const CustomPart* cp = CustomPartRegistry::instance().find(id);
                  if (!cp) continue;
                  Json j = obj();
                  j["partId"] = id;
                  j["name"] = cp->spec.name;
                  j["manufacturer"] = cp->spec.manufacturer;
                  j["package"] = cp->spec.package.type + "-" + std::to_string(cp->spec.package.pinCount);
                  j["pins"] = cp->spec.pins.size();
                  list.push(j);
              }
              Json j = obj();
              j["parts"] = list;
              return out(j);
          });
    t.add("library", "library_compare_footprint", "Footprint vs datasheet", Kind::Read, true,
          "Holds a part's footprint against the datasheet's recommended land pattern: give the datasheet's pads, pitch, "
          "padWidth, padLength and span (outer pad edge to outer pad edge, mm), any of them. Returns what the footprint "
          "measures and each value off by more than tolerance (mm, default 0.05; pads must match exactly).",
          Schema().object("spec", "Part spec (as library_create_part takes)").str("partId", "Or a project library part id")
              .object("datasheet", "{pads, pitch, padWidth, padLength, span}", true).num("tolerance", "mm (default 0.05)"),
          [](McpServer& s, const Json& a) {
              CustomPartSpec spec;
              if (a.has("spec")) {
                  spec = customPartSpecFromJson(a.get("spec"));
              } else {
                  const CustomPart* cp = CustomPartRegistry::instance().find(requireStr(a, "partId"));
                  check(cp != nullptr, "No library part with that id");
                  spec = cp->spec;
              }
              (void)s;
              const FootprintDims d = measureFootprint(spec);
              const double tol = argNum(a, "tolerance", 0.05);
              Json measured = obj(), off = Json::array();
              const std::pair<const char*, double> values[] = {{"pads", d.pads}, {"pitch", d.pitch}, {"padWidth", d.padWidth},
                                                               {"padLength", d.padLength}, {"span", d.span}};
              for (const auto& [key, value] : values) {
                  measured[key] = value;
                  const Json& want = a.get("datasheet").get(key);
                  if (!want.isNumber()) continue;
                  const double diff = value - want.asNumber();
                  if (std::abs(diff) > (std::string(key) == "pads" ? 0.5 : tol)) {
                      Json m = obj();
                      m["field"] = key, m["footprint"] = value, m["datasheet"] = want.asNumber(), m["diff"] = diff;
                      off.push(m);
                  }
              }
              Json j = obj();
              j["measured"] = measured, j["mismatches"] = off, j["matches"] = off.size() == 0;
              return out(j);
          });
    t.add("library", "library_bulk_edit", "Bulk edit library parts", Kind::Edit, false,
          "Sets the same fields on every project library part whose name holds match (case-insensitive; \"\" = all): "
          "manufacturer, description, datasheet, refPrefix, defaultValue. Placed parts move to the edited part (wires "
          "kept). Returns each part changed with its old and new id and quality score.",
          Schema().str("match", "Part name filter (\"\" = every part)").object("set", "Fields to set, e.g. {\"manufacturer\": \"TI\"}", true),
          [](McpServer& s, const Json& a) {
              static const std::set<std::string> fields = {"manufacturer", "description", "datasheet", "refPrefix", "defaultValue"};
              const Json& set = a.get("set");
              check(set.isObject() && set.fields().size() > 0, "set needs at least one field");
              for (const auto& [key, value] : set.fields())
                  check(fields.count(key) && value.isString(), "set takes string fields: manufacturer, description, datasheet, refPrefix, defaultValue");
              auto lower = [](std::string v) {
                  for (auto& ch : v) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
                  return v;
              };
              const std::string match = lower(argStr(a, "match", ""));
              Json changed = Json::array();
              const std::vector<std::string> ids = proj(s).customLibrary;  // the list changes below
              for (const auto& id : ids) {
                  const CustomPart* cp = CustomPartRegistry::instance().find(id);
                  if (!cp || lower(cp->spec.name).find(match) == std::string::npos) continue;
                  Json spec = customPartSpecToJson(cp->spec);
                  for (const auto& [key, value] : set.fields()) spec[key] = value;
                  char* error = nullptr;
                  const Json made = takeJson(sieda_custom_part_register(P(s), spec.dump().c_str(), &error));
                  if (error) {
                      const std::string why = error;
                      sieda_string_free(error);
                      throw ToolError(cp->spec.name + ": " + why);
                  }
                  const std::string to = made.get("id").asString("");
                  if (to.empty() || to == id) continue;
                  Json j = obj();
                  j["name"] = cp->spec.name, j["from"] = id, j["to"] = to;
                  j["replaced"] = sieda_custom_part_replace(P(s), id.c_str(), to.c_str());
                  sieda_custom_part_remove(P(s), id.c_str());
                  if (const CustomPart* now = CustomPartRegistry::instance().find(to)) j["quality"] = partQuality(now->spec).score;
                  changed.push(j);
              }
              Json j = obj();
              j["changed"] = changed;
              return out(j);
          });
    t.add("library", "library_check_part", "Check part spec", Kind::Read, true,
          "Checks a part spec (as library_create_part takes) without adding it: symbol, land pattern and unit checks, "
          "and its library quality score (0 – 100, with what is missing). preview: true adds the part as it would be "
          "drawn (part: symbol pins and footprint pads) and its footprint as editable lands (customPackage: package type CUSTOM).",
          Schema().object("spec", "Part spec", true).num("minGap", "Copper gap limit for pads (mm, default 0.15)")
              .boolean("preview", "Also return the drawn part and its lands"),
          [](McpServer&, const Json& a) {
              const std::string spec = a.get("spec").dump();
              // A parametric package (SOIC, QFN …) is checked on the footprint it generates.
              std::string lands = spec;
              try {
                  lands = customPartSpecToJson(landPatternFromFootprint(customPartSpecFromJson(a.get("spec")))).dump();
              } catch (const std::exception&) {}  // an invalid spec: the checks below say why
              Json j = obj();
              if (argBool(a, "preview", false)) {
                  char* err = nullptr;
                  char* part = sieda_custom_part_preview(spec.c_str(), &err);
                  if (!part) throw ToolError("Invalid part: " + takeText(err));
                  j["part"] = takeJson(part);
                  j["customPackage"] = Json::parse(lands).get("package");
              }
              j["symbol"] = takeJson(sieda_check_symbol(spec.c_str()));
              j["landPattern"] = takeJson(sieda_check_land_pattern(lands.c_str(), argNum(a, "minGap", 0.15)));
              j["units"] = takeJson(sieda_check_units(spec.c_str()));
              try {
                  const PartQuality q = partQuality(customPartSpecFromJson(a.get("spec")));
                  Json quality = obj(), missing = Json::array();
                  for (const auto& m : q.missing) missing.push(m);
                  quality["score"] = q.score, quality["missing"] = missing;
                  j["quality"] = quality;
              } catch (const std::exception&) {}  // an invalid spec: the checks above say why
              return out(j);
          });
    t.add("library", "library_import", "Import library files", Kind::Edit, false,
          "Imports KiCad (.kicad_sym / .kicad_mod) or Eagle (.lbr) libraries, from files in the root folder (paths) "
          "or inline text (files). Symbols are paired with footprints and checked; with register (default true) the "
          "good parts join the project library.",
          Schema()
              .array("paths", "Library files inside the root folder", "string")
              .array("files", "Inline files [{name, content}]", "object")
              .object("pairs", "Symbol → footprint names, {\"LM358\":\"SOIC-8_3.9x4.9mm\"}")
              .boolean("register", "Add the parts that pass the checks (default true)"),
          [](McpServer& s, const Json& a) {
              Json files = Json::array();
              if (a.get("files").isArray())
                  for (const auto& f : a.get("files").items()) files.push(f);
              if (a.get("paths").isArray())
                  for (const auto& pth : a.get("paths").items()) {
                      Json f = obj();
                      f["name"] = fs::path(pth.asString("")).filename().string();
                      f["content"] = readFileArg(s, pth.asString(""));
                      files.push(f);
                  }
              if (!files.size()) throw ToolError("Give paths or files");
              Json req = obj();
              req["files"] = files;
              if (a.get("pairs").isObject()) req["pairs"] = a.get("pairs");
              Json res = takeJson(sieda_library_import(req.dump().c_str()));
              Json parts = Json::array();
              const bool reg = argBool(a, "register", true);
              for (const auto& part : res.get("parts").items()) {
                  Json pj = obj();
                  pj["name"] = part.get("name");
                  pj["symbol"] = part.get("symbol");
                  pj["footprint"] = part.get("footprint");
                  pj["ok"] = part.get("ok");
                  if (!part.get("error").asString("").empty()) pj["error"] = part.get("error");
                  if (part.get("warnings").size()) pj["warnings"] = part.get("warnings");
                  if (reg && part.get("ok").asBool(false)) {
                      char* err = nullptr;
                      const Json r = takeJson(sieda_custom_part_register(P(s), part.get("spec").dump().c_str(), &err));
                      if (r.isObject()) {
                          pj["partId"] = r.get("id");
                          // Keep it in the project library even before it is placed.
                          auto& lib = P(s)->project.customLibrary;
                          const std::string id = r.get("id").asString("");
                          if (!id.empty() && std::find(lib.begin(), lib.end(), id) == lib.end()) lib.push_back(id);
                      } else {
                          pj["error"] = takeText(err);
                      }
                  }
                  parts.push(pj);
              }
              Json j = obj();
              j["parts"] = parts;
              j["files"] = res.get("files");
              return out(j);
          });
    t.add("library", "library_land_calculator", "IPC-7351 land calculator", Kind::Read, true,
          "IPC-7351B pads for a two-sided part from the datasheet's lead dimensions (mm): toe-to-toe span L, heel-to-heel "
          "span S and lead width W, min and max each; family gullwing (SOIC, SOP, QFP), chip (0603 and up) or qfn; "
          "density most, nominal or least. With pins (even) and pitch it also lays out the two rows as lands for "
          "library_create_part (package type CUSTOM): pad 1 top left, down the left side, then up the right.",
          Schema().str("family", "Lead family", true, {"gullwing", "chip", "qfn"})
              .str("density", "most, nominal (default) or least", false, {"most", "nominal", "least"})
              .num("lMin", "L min", true).num("lMax", "L max", true).num("sMin", "S min", true).num("sMax", "S max", true)
              .num("wMin", "W min", true).num("wMax", "W max", true)
              .integer("pins", "Pin count (two rows)").num("pitch", "Pin pitch (mm)"),
          [](McpServer&, const Json& a) {
              const std::string d = argStr(a, "density", "nominal");
              IpcLand l;
              try {
                  l = ipc7351Land(argStr(a, "family", ""), d == "most" ? 0 : d == "least" ? 2 : 1, argNum(a, "lMin", 0),
                                  argNum(a, "lMax", 0), argNum(a, "sMin", 0), argNum(a, "sMax", 0), argNum(a, "wMin", 0),
                                  argNum(a, "wMax", 0));
              } catch (const std::invalid_argument& e) {
                  throw ToolError(e.what());
              }
              auto r3 = [](double v) { return std::round(v * 1000) / 1000; };
              Json j = obj();
              j["padLength"] = r3(l.length), j["padWidth"] = r3(l.width), j["centres"] = r3(l.centres);
              j["z"] = r3(l.z), j["g"] = r3(l.g), j["x"] = r3(l.x);
              const int pins = argInt(a, "pins", 0);
              if (pins > 0) {
                  check(pins % 2 == 0 && pins <= 1024, "Two rows need an even pin count");
                  const double pitch = argNum(a, "pitch", 0);
                  check(pins == 2 || pitch > l.width, "The pitch must be wider than the pad");
                  Json lands = Json::array();
                  const int half = pins / 2;
                  for (int i = 0; i < pins; ++i) {
                      const bool left = i < half;
                      const int k = left ? i : pins - 1 - i;
                      Json land = Json::array();
                      land.push(r3(left ? -l.centres / 2 : l.centres / 2));
                      land.push(r3((k - (half - 1) / 2.0) * pitch));
                      land.push(r3(l.length));
                      land.push(r3(l.width));
                      lands.push(land);
                  }
                  j["lands"] = lands;
              }
              return out(j);
          });
    t.add("library", "library_footprints", "Footprint library", Kind::Read, true,
          "The built-in footprints and the project's own: search by words (name or label; \"\" = all, at most 200) for "
          "[{name, label, pads, width, height}], or name one to get its pads as lands {lands, bodySize, bodyDepth} "
          "(pad k on pin k + 1, mechanical pads \"-\") to start a part from (library_create_part, package type CUSTOM).",
          Schema().str("query", "Search words").str("name", "A footprint name from a search"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              if (a.has("name")) {
                  char* lands = sieda_footprint_lands_json(argStr(a, "name").c_str());
                  check(lands != nullptr, "No footprint " + argStr(a, "name"));
                  j = takeJson(lands);
                  j["name"] = argStr(a, "name");
              } else {
                  j["footprints"] = takeJson(sieda_footprint_library_json(P(s), argStr(a, "query", "").c_str()));
              }
              return out(j);
          });
    t.add("library", "library_part_spec", "Part spec", Kind::Read, true,
          "A project library part's full spec (library_project_parts ids), as library_create_part takes it: to edit "
          "it and save it back with library_replace_part.",
          Schema().str("id", "Part id", true), [](McpServer&, const Json& a) {
              const CustomPart* cp = CustomPartRegistry::instance().find(argStr(a, "id"));
              check(cp != nullptr, "No library part " + argStr(a, "id"));
              Json j = obj();
              j["id"] = cp->id;
              j["spec"] = customPartSpecToJson(cp->spec);
              return out(j);
          });
    t.add("library", "library_replace_part", "Replace part", Kind::Edit, false,
          "Saves an edited spec over a project library part: every placed instance takes the new symbol and footprint "
          "(same designators and nets by pin number), the old part leaves the library.",
          Schema().str("id", "Part id to replace", true).object("spec", "The new spec", true), [](McpServer& s, const Json& a) {
              const std::string id = argStr(a, "id");
              check(CustomPartRegistry::instance().find(id) != nullptr, "No library part " + id);
              char* error = nullptr;
              char* made = sieda_custom_part_register(P(s), a.get("spec").dump().c_str(), &error);
              if (!made) throw ToolError("Invalid part: " + takeText(error));
              const std::string to = takeJson(made).get("id").asString("");
              Json j = obj();
              j["id"] = to;
              j["replaced"] = to == id ? 0 : sieda_custom_part_replace(P(s), id.c_str(), to.c_str());
              if (to != id) sieda_custom_part_remove(P(s), id.c_str());
              return out(j);
          });
    t.add("library", "library_import_model", "Import 3D model", Kind::Edit, false,
          "Imports a part's 3D model (STEP / VRML / STL / OBJ file in the root folder). Returns its id and bounds; with "
          "spec (a part spec) also the model3d alignment that centres it on that footprint and seats it on the board "
          "(put it in the spec's model3d), and with ref it becomes that placed part's own model.",
          Schema().str("path", "Model file", true).num("unit", "mm per file unit (default from the file; KiCad VRML 2.54)")
              .object("spec", "Part spec to align the model to").str("ref", "Placed part to give this model"),
          [](McpServer& s, const Json& a) {
              const std::string path = argStr(a, "path"), text = readFileArg(s, path).asString("");
              Json req = obj();
              req["name"] = fs::path(path).filename().string();
              const std::string ext = fs::path(path).extension().string();
              if (ext == ".stl" || ext == ".STL") req["contentBase64"] = base64Encode(text);
              else req["content"] = text;
              Json r = takeJson(sieda_model3d_import(req.dump().c_str()));
              check(r.get("ok").asBool(false), "Cannot read the model: " + r.get("error").asString("?"));
              const double unit = argNum(a, "unit", r.get("unit").asNumber(1));
              if (a.get("spec").isObject()) {
                  Json spec = a.get("spec");
                  Json m = obj();
                  m["id"] = r.get("id"), m["name"] = r.get("name"), m["unit"] = unit;
                  spec["model3d"] = m;
                  const Json fit = takeJson(sieda_model3d_fit(spec.dump().c_str()));
                  check(fit.get("ok").asBool(false), "Cannot align the model: " + fit.get("error").asString("?"));
                  r["model3d"] = fit.get("seated");
              }
              if (a.has("ref")) {
                  const int id = componentId(*P(s), a.get("ref"));
                  check(sieda_pcb_set_part_model(P(s), id, r.get("id").asString("").c_str(), r.get("name").asString("").c_str(), unit) == 1,
                        "Cannot give " + argStr(a, "ref") + " that model");
                  r["placedOn"] = argStr(a, "ref");
              }
              return out(r);
          });
    t.add("library", "library_company", "Company library", Kind::Edit, false,
          "A team library in a folder inside the root (one <name>.part.json per part, each saved version kept in "
          ".history/<name>/). publish (id: a project part) adds or updates a part as a draft, version + 1; release "
          "marks it released by approvedBy (when the folder's librarians.json lists names, only they may); obsolete "
          "retires it; alternates sets its approved alternates (names); list shows every part with status, version, "
          "alternates, approved (unchanged since release), description, manufacturer, package, pins and the number of "
          "earlier versions kept (history); use (name) adds it to the project and places it "
          "(drafts only with allowDraft; never an obsolete part).",
          Schema().str("action", "publish, release, obsolete, alternates, list or use", true,
                       {"publish", "release", "obsolete", "alternates", "list", "use"})
              .str("folder", "Library folder in the root", true).str("id", "publish: project part id").str("name", "Part name")
              .str("approvedBy", "release: librarian").array("alternates", "alternates: part names", "string")
              .boolean("allowDraft", "use: allow a draft").num("x", "use: x").num("y", "use: y"),
          [](McpServer& s, const Json& a) {
              auto fingerprint = [](const Json& spec) {
                  uint64_t h = 1469598103934665603ull;  // FNV-1a
                  for (const unsigned char ch : spec.dump()) h = (h ^ ch) * 1099511628211ull;
                  char text[17];
                  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
                  return std::string(text);
              };
              const std::string folder = requireStr(a, "folder"), action = argStr(a, "action", "");
              auto safe = [](const std::string& name) {
                  check(!name.empty() && name.size() <= 120 && name.find_first_of("/\\:*?\"<>|") == std::string::npos && name[0] != '.',
                        "Part names for the library are plain file names");
                  return name;
              };
              auto fileOf = [&](const std::string& name) { return folder + "/" + safe(name) + ".part.json"; };
              auto load = [&](const std::string& name) { return Json::parse(readFileArg(s, fileOf(name)).asString("")); };
              auto approved = [&](const Json& e) {
                  return e.get("status").asString("") == "released" && e.get("approvedHash").asString("") == fingerprint(e.get("spec"));
              };
              Json j = obj();
              if (action == "list") {
                  Json parts = Json::array();
                  const std::string dir = s.resolvePath(folder, false);
                  std::error_code ec;
                  std::vector<std::string> names;
                  for (const auto& f : fs::directory_iterator(dir, ec)) {
                      const std::string fn = f.path().filename().string();
                      if (fn.size() > 10 && fn.compare(fn.size() - 10, 10, ".part.json") == 0) names.push_back(fn.substr(0, fn.size() - 10));
                  }
                  std::sort(names.begin(), names.end());
                  for (const auto& n : names) {
                      const Json e = load(n);
                      Json row = obj();
                      row["name"] = n, row["status"] = e.get("status"), row["version"] = e.get("version");
                      row["approved"] = approved(e), row["alternates"] = e.get("alternates").isArray() ? e.get("alternates") : Json::array();
                      if (e.has("approvedBy")) row["approvedBy"] = e.get("approvedBy");
                      const Json& spec = e.get("spec");
                      row["description"] = spec.get("description").asString(""), row["manufacturer"] = spec.get("manufacturer").asString("");
                      row["package"] = spec.get("package").get("type").asString(""), row["pins"] = spec.get("package").get("pinCount").asInt(0);
                      int history = 0;
                      for (const auto& f : fs::directory_iterator(dir + "/.history/" + n, ec)) history += f.is_regular_file();
                      row["history"] = history;
                      parts.push(row);
                  }
                  j["parts"] = parts;
                  return out(j);
              }
              if (action == "publish") {
                  const CustomPart* cp = CustomPartRegistry::instance().find(requireStr(a, "id"));
                  check(cp != nullptr, "No project part " + argStr(a, "id"));
                  const std::string name = a.has("name") ? argStr(a, "name") : cp->spec.name;
                  Json e = obj();
                  int version = 0;
                  try {
                      e = load(name);
                      version = e.get("version").asInt(0);
                      s.writeFile(folder + "/.history/" + safe(name) + "/v" + std::to_string(version) + ".json", e.dump(2) + "\n");
                  } catch (const ToolError&) {  // a new part
                  }
                  e["format"] = "sieda.part/1", e["spec"] = customPartSpecToJson(cp->spec), e["version"] = version + 1, e["status"] = "draft";
                  e["hash"] = fingerprint(e.get("spec"));
                  s.writeFile(fileOf(name), e.dump(2) + "\n");
                  j["name"] = name, j["version"] = version + 1, j["status"] = "draft";
                  return out(j);
              }
              const std::string name = requireStr(a, "name");
              Json e = load(name);
              check(e.get("format").asString("") == "sieda.part/1", fileOf(name) + " is not a SiEDA library part");
              if (action == "use") {
                  const std::string status = e.get("status").asString("draft");
                  check(status != "obsolete", name + " is obsolete" +
                                                  (e.get("alternates").size() ? "; approved alternates: " + e.get("alternates").dump() : std::string()));
                  check(status == "released" || argBool(a, "allowDraft", false), name + " is a draft (not released); use allowDraft to try it");
                  char* error = nullptr;
                  char* made = sieda_custom_part_register(P(s), e.get("spec").dump().c_str(), &error);
                  if (!made) throw ToolError("Invalid part: " + takeText(error));
                  const std::string id = takeJson(made).get("id").asString("");
                  const Vec2 at = freeSpot(P(s)->project);
                  const int c = sieda_add_custom_component(P(s), id.c_str(), e.get("spec").get("defaultValue").asString(name).c_str(),
                                                           argNum(a, "x", at.x), argNum(a, "y", at.y), 0, nullptr);
                  check(c >= 0, "The part was added but could not be placed");
                  j["partId"] = id, j["component"] = componentJson(*P(s), c), j["approved"] = approved(e), j["version"] = e.get("version");
                  return out(j);
              }
              if (action == "release") {
                  const std::string by = requireStr(a, "approvedBy");
                  std::error_code ec;
                  if (fs::exists(s.resolvePath(folder + "/librarians.json", false), ec)) {  // only the listed librarians release
                      const Json lib = Json::parse(readFileArg(s, folder + "/librarians.json").asString(""));
                      bool listed = false;
                      for (const auto& l : lib.get("librarians").items()) listed = listed || l.asString("") == by;
                      check(listed, by + " is not a librarian of this library (librarians.json)");
                  }
                  e["status"] = "released", e["approvedBy"] = by, e["approvedHash"] = fingerprint(e.get("spec"));
              } else if (action == "obsolete") {
                  e["status"] = "obsolete";
              } else {
                  Json alts = Json::array();
                  for (const auto& n : a.get("alternates").items()) alts.push(safe(n.asString("")));
                  e["alternates"] = alts;
              }
              s.writeFile(fileOf(name), e.dump(2) + "\n");
              j["name"] = name, j["status"] = e.get("status"), j["approved"] = approved(e);
              return out(j);
          });
    t.add("library", "library_packages", "Packages", Kind::Read, true,
          "Package types a custom part can use (SOIC, TSSOP, QFN, LQFP, BGA …).", Schema(), [](McpServer&, const Json&) {
              Json j = obj();
              j["packages"] = takeJson(sieda_packages_json());
              return out(j);
          });
}

// ================================================================================================= PCB tools

Json pcbSummary(McpServer& s) {
    const Project& p = proj(s);
    Json j = obj();
    j["board"] = projectSummary(s).get("board");
    Json outline = Json::array();
    for (const auto& v : p.pcb.settings.outlinePolygon()) {
        Json pt = obj();
        pt["x"] = v.x;
        pt["y"] = v.y;
        outline.push(pt);
    }
    j["outline"] = outline;
    Json fps = Json::array();
    for (const auto& c : p.schematic.components()) {
        if (!c.hasFootprint() || isNetSymbolKind(c.kind)) continue;
        Json f = obj();
        f["ref"] = c.ref;
        f["footprint"] = c.footprintName();
        if (const StandardPart* sp = findStandardPart(c.def().name); sp && !sp->footprint.empty())
            f["kicadFootprint"] = sp->footprint;  // the library footprint its pads follow (the id above is the project's copy)
        f["placed"] = c.pcb.placed;
        {  // its courtyard (w, h mm as it is turned now): what a hand placement must leave room for
            const Rect cy = p.checkPlacement(c.id, c.pcb.position, c.pcb.rotation, c.pcb.bottom, 0).courtyard;
            Json size = Json::array();
            size.push(std::round(cy.width() * 100) / 100), size.push(std::round(cy.height() * 100) / 100);
            f["courtyard"] = size;
        }
        if (c.pcb.placed) {
            f["x"] = c.pcb.position.x;
            f["y"] = c.pcb.position.y;
            f["rotation"] = c.pcb.rotation;
            f["bottom"] = c.pcb.bottom;
            if (c.pcb.locked) f["locked"] = true;
        }
        fps.push(f);
    }
    j["footprints"] = fps;
    double length = 0;
    for (const auto& t : p.pcb.tracks) length += trackLength(t);
    j["trackLengthApprox"] = length;
    j["zones"] = takeJson(sieda_project_snapshot(P(s))).get("zones");
    j["keepouts"] = takeJson(sieda_pcb_keepouts(P(s)));
    Json cutouts = Json::array();
    for (const auto& c : P(s)->project.pcb.settings.cutouts) {
        Json pts = Json::array();
        for (const Vec2& q : c) {
            Json pt = obj();
            pt["x"] = q.x, pt["y"] = q.y;
            pts.push(pt);
        }
        cutouts.push(pts);
    }
    j["cutouts"] = cutouts;
    Json holes = Json::array();
    for (const auto& h : p.pcb.settings.holes) {
        Json hj = obj();
        hj["x"] = h.position.x;
        hj["y"] = h.position.y;
        hj["drill"] = h.drill;
        holes.push(hj);
    }
    j["mountingHoles"] = holes;
    return j;
}

/// Auto Place grows the board when the parts do not fit: say so (boardGrown), so the size set is not lost unseen.
void noteBoardGrown(McpServer& s, double w0, double h0, Json& j) {
    const auto& st = proj(s).pcb.settings;
    if (st.width == w0 && st.height == h0) return;
    char note[400];
    std::snprintf(note, sizeof note,
                  "The parts did not fit: the board grew from %g x %g mm to %g x %g mm. Keep-outs, pours and holes kept "
                  "their coordinates. pcb_set_board sets the size back (pcb_drc then flags parts left outside).",
                  w0, h0, st.width, st.height);
    j["boardGrown"] = std::string(note);
}

/// pcb_autoplace (also run by pcb_fit_board compact).
ToolOutput autoplaceTool(McpServer& s, const Json& a) {
    const double w0 = proj(s).pcb.settings.width, h0 = proj(s).pcb.settings.height;
    std::vector<int> held;  // placed at an edge first, held while the rest is placed
    Json edges;
    // Once more when Auto Place grew the board: the edge parts go to the grown board's edges.
    for (int pass = 0; pass < 2; ++pass) {
        const double wPass = proj(s).pcb.settings.width, hPass = proj(s).pcb.settings.height;
        held.clear();
        if (argBool(a, "all", false) && argBool(a, "edges", true)) {
            std::vector<int> lift;  // everything unlocked is placed again: the edge parts first
            for (const auto& c : proj(s).schematic.components())
                if (!c.pcb.locked && c.hasFootprint() && proj(s).pcb.onBoard(c)) lift.push_back(c.id);
            for (int id : lift) P(s)->project.schematic.find(id)->pcb.placed = false;
            edges = placeAtEdges(P(s), &held);
            // The main IC (most pins, at least 8) at the board's middle, so its capacitors and crystal find
            // room on every side and the rest spreads round it.
            const Component* main = nullptr;
            for (const auto& c : proj(s).schematic.components())
                if (!c.pcb.locked && !c.pcb.placed && c.hasFootprint() && proj(s).pcb.onBoard(c) && c.def().pins.size() >= 8 &&
                    (!main || c.def().pins.size() > main->def().pins.size()))
                    main = &c;
            if (main) {
                const auto& st = proj(s).pcb.settings;
                const Vec2 mid{st.width / 2, st.height / 2};
                bool put = false;
                for (double r = 0; r <= std::min(st.width, st.height) / 2 && !put; r += 0.5)
                    for (int k = 0; k < (r > 0 ? 16 : 1) && !put; ++k) {
                        const Vec2 at = mid + Vec2{std::cos(k * kPi / 8), std::sin(k * kPi / 8)} * r;
                        put = takeJson(sieda_pcb_place_footprint(P(s), main->id, at.x, at.y, main->pcb.rotation, 0, 0.25, 0))
                                  .get("committed").asBool(false);
                    }
                if (put) held.push_back(main->id);
            }
            for (int id : held) P(s)->project.schematic.find(id)->pcb.locked = true;
        }
        sieda_pcb_autoplace(P(s), argBool(a, "all", false) ? 1 : 0);
        for (int id : held) P(s)->project.schematic.find(id)->pcb.locked = false;
        if (!edges.size() || (proj(s).pcb.settings.width == wPass && proj(s).pcb.settings.height == hPass)) break;
    }
    std::set<int> bound;  // placed by their IC pins: grouping leaves them
    const Json moved = argBool(a, "decoupling", argBool(a, "all", false)) ? placeDecoupling(P(s), &bound) : Json();
    const Json rows = argBool(a, "grouped", false) ? placeGrouped(P(s), bound) : Json();
    Json j = pcbSummary(s);
    if (moved.size()) j["decouplingPlaced"] = moved;
    if (rows.size()) j["groupedRows"] = rows;
    if (edges.size()) j["edgesPlaced"] = edges;
    noteBoardGrown(s, w0, h0, j);
    double used = 0;  // courtyards' share of the board: a roomy board can be made smaller before routing
    for (const auto& c : proj(s).schematic.components())
        if (c.hasFootprint() && c.pcb.placed && proj(s).pcb.onBoard(c)) used += [&] { const Rect r = proj(s).pcb.courtyard(c); return (r.x1 - r.x0) * (r.y1 - r.y0); }();
    if (const auto& st = proj(s).pcb.settings; st.outline.empty() && used < 0.35 * st.width * st.height && used > 0)
        j["boardRoomy"] = "Parts cover " + std::to_string(static_cast<int>(100 * used / (st.width * st.height))) +
                          " % of the board: pcb_fit_board {\"compact\": true} tries smaller boards (it keeps this size, and says why, "
                          "when a smaller one would move a capacitor / crystal further from its pins).";
    return out(j);
}

void pcbTools(Table& t) {
    t.add("pcb", "pcb_summary", "Board summary", Kind::Read, true,
          "The board: size, outline, layers, rules, every footprint with position / rotation / side and courtyard "
          "size [w, h] (mm, as turned), routing "
          "progress (tracks, vias, unrouted connections), pours, keep-outs and mounting holes. pads: [\"U1\"] adds "
          "those parts' pads (pin, net, centre x / y, size, layer).",
          Schema().array("pads", "Designators whose pads to list", "string"), [](McpServer& s, const Json& a) {
              Json j = pcbSummary(s);
              if (!a.get("pads").size()) return out(j);
              const Project& pr = proj(s);
              Json pads = obj();
              const auto all = pr.pcb.pads(pr.schematic);
              for (const auto& r : a.get("pads").items()) {
                  const int id = componentId(*P(s), r);
                  Json list = Json::array();
                  for (const auto& pad : all) {
                      if (pad.componentId != id) continue;
                      Json q = obj();
                      if (pad.pinIndex >= 0) q["pin"] = pinLabel(*P(s), {id, pad.pinIndex});
                      if (pad.net >= 0) q["net"] = pr.schematic.nets()[static_cast<size_t>(pad.net)].name;
                      q["x"] = pad.position.x;
                      q["y"] = pad.position.y;
                      q["w"] = pad.size.x;
                      q["h"] = pad.size.y;
                      q["layer"] = pad.throughHole ? "through" : pad.bottom ? "bottom" : "top";
                      list.push(q);
                  }
                  pads[pr.schematic.find(id)->ref] = list;
              }
              j["pads"] = pads;
              return out(j);
          });
    t.add("pcb", "pcb_set_board", "Board setup", Kind::Edit, true,
          "Sets the board size (mm), copper layers (1, 2, 4, 6), default track width and clearance, a rule preset "
          "(pcb_rule_presets), thickness, solder mask colour and conformal coating. Give any subset.",
          Schema()
              .num("width", "Board width (mm)")
              .num("height", "Board height (mm)")
              .integer("layers", "Copper layers: 1, 2, 4 or 6")
              .num("trackWidth", "Default track width (mm)")
              .num("clearance", "Default clearance (mm)")
              .str("rulePreset", "Design-rule preset name")
              .num("thickness", "Board thickness (mm, 0.4–6.4)")
              .str("solderMask", "green, black, blue, red, yellow, white, purple")
              .str("coating", "none, acrylic, silicone, urethane, epoxy, parylene"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              if (a.has("rulePreset"))
                  if (sieda_pcb_apply_rule_preset(p, argStr(a, "rulePreset").c_str()) != 1) {
                      std::string names;  // nothing else is changed: the agent picks a name and sends the call again
                      const Json presets = takeJson(sieda_design_rule_presets_json());
                      for (const auto& pr : presets.items())
                          names += (names.empty() ? "" : ", ") + pr.get("name").asString("");
                      throw ToolError("Unknown rule preset \"" + argStr(a, "rulePreset") + "\" (nothing was changed). Presets: " + names + ".");
                  }
              sieda_pcb_set_board(p, argNum(a, "width", 0), argNum(a, "height", 0), argNum(a, "trackWidth", 0), argNum(a, "clearance", 0));
              if (a.has("layers")) {
                  const int l = argInt(a, "layers", 2);
                  if (l != 1 && l != 2 && l != 4 && l != 6) throw ToolError("layers must be 1, 2, 4 or 6");
                  sieda_pcb_set_layer_count(p, l);
              }
              if (a.has("thickness"))
                  check(sieda_pcb_set_mechanical(p, argNum(a, "thickness", 1.6), p->project.pcb.settings.underfill ? 1 : 0) == 1,
                        "Thickness must be 0.4–6.4 mm");
              if (a.has("solderMask")) check(sieda_pcb_set_solder_mask(p, argStr(a, "solderMask").c_str()) == 1, "Unknown mask colour");
              if (a.has("coating")) check(sieda_pcb_set_coating(p, argStr(a, "coating").c_str()) == 1, "Unknown coating");
              return out(projectSummary(s).get("board"));
          });
    t.add("pcb", "pcb_set_outline", "Board outline", Kind::Edit, true,
          "Board shape: a polygon (points in mm, ≥ 3; [] = the plain rectangle) or a preset: rectangle (w × h), "
          "rounded (corner radius param), circle (diameter w), quad-x (quadcopter frame: span w, body h, arm param).",
          Schema()
              .array("points", "Polygon [{\"x\":0,\"y\":0}, …]", "object")
              .str("preset", "Outline preset", false, {"rectangle", "rounded", "circle", "quad-x"})
              .num("width", "Preset width / diameter / span (mm)")
              .num("height", "Preset height (mm)")
              .num("param", "Corner radius / arm width (mm)")
              .any("cutouts", "Board cutouts: [[{x, y}, …], …] (closed shapes inside the board); [] removes them")
              .array("dimensions", "Drawing dimensions [{a:{x,y}, b:{x,y}, offset}] (mm; offset along (−dy, dx), default 3) for "
                     "the fabrication drawing and DXF; replaces them, [] removes", "object"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              if (a.get("dimensions").isArray()) {
                  check(sieda_pcb_set_dimensions(p, a.get("dimensions").dump().c_str()) == 1, "A dimension needs two distinct points a and b");
                  if (!a.has("preset") && !a.has("points") && !a.has("cutouts")) return out(pcbSummary(s).get("outline"));
              }
              if (a.get("cutouts").isArray()) {
                  p->project.pcb.settings.cutouts.clear();
                  for (const auto& c : a.get("cutouts").items())
                      check(sieda_pcb_add_cutout(p, c.dump().c_str()) == 1, "A cutout needs 3 or more corners inside the board");
                  if (!a.has("preset") && !a.has("points")) return out(pcbSummary(s).get("outline"));
              }
              if (a.has("preset")) {
                  check(sieda_pcb_outline_preset(p, argStr(a, "preset").c_str(), argNum(a, "width", p->project.pcb.settings.width),
                                                 argNum(a, "height", p->project.pcb.settings.height), argNum(a, "param", 2)) == 1,
                        "Unknown outline preset");
              } else if (a.get("points").isArray()) {
                  check(sieda_pcb_set_outline(p, a.get("points").dump().c_str()) == 1, "Invalid outline");
              } else {
                  throw ToolError("Give points or a preset");
              }
              return out(pcbSummary(s).get("outline"));
          });
    t.add("pcb", "pcb_fit_board", "Fit board", Kind::Edit, true,
          "Shrinks / grows the board to the placed footprints plus a margin (parts and copper move together). compact: "
          "re-places every unlocked part (as pcb_autoplace all) on a board made smaller in 1 mm steps "
          "while every part still fits with no new placement problems and every capacitor / crystal stays by its pins — "
          "for a board whose edge parts leave empty space.",
          Schema().num("margin", "Margin around the parts (mm, default 3)")
              .boolean("compact", "Re-place everything on the smallest board that still fits it (clears any routing; default false)"),
          [](McpServer& s, const Json& a) {
              Project& pr = P(s)->project;
              if (argBool(a, "compact", false)) {
                  // Compact re-places every part: routing done before it is cleared (and said so), to route again after.
                  const size_t clearedTracks = pr.pcb.tracks.size(), clearedVias = pr.pcb.vias.size();
                  pr.pcb.tracks.clear(), pr.pcb.vias.clear();
                  check(pr.pcb.settings.outline.empty(), "compact needs a rectangular board (this one has a drawn outline)");
                  auto problems = [&] {
                      int n = 0;
                      for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info && v.code != "DRC_UNROUTED";
                      for (const auto& c : pr.schematic.components()) n += c.hasFootprint() && pr.pcb.onBoard(c) && !c.pcb.placed;
                      return n;
                  };
                  const double widthWas = pr.pcb.settings.width, heightWas = pr.pcb.settings.height;
                  Json place = obj();
                  place["all"] = true;
                  // Each smaller board keeps every capacitor / crystal Auto Place set by its pins at the start.
                  Json placed, bestPlaced;  // the capacitors / crystals by their pins, as Auto Place reports them
                  auto paired = [&] {  // the capacitors / crystals Auto Place set by their pins
                      placed = autoplaceTool(s, place).data.get("decouplingPlaced");
                      size_t n = 0;
                      for (const auto& m : placed.items()) n += m.asString("x")[0] == 'C' || m.asString("x")[0] == 'Y';
                      return n;
                  };
                  const size_t pairs = paired();
                  auto spread = [&] {  // the capacitors' distances to their pins, summed ("C2 → U1.VCC (2.7 mm)")
                      double mm = 0;
                      for (const auto& m : placed.items())
                          if (const auto at = m.asString("").rfind(" ("); at != std::string::npos) mm += std::atof(m.asString("").c_str() + at + 2);
                      return mm;
                  };
                  const double reach = spread() * 1.15 + 1;
                  auto pairings = [&] {  // "C1 → D1.K (2.7 mm)" → C1's distance (mm; 0 when not given): by its part, so two
                      std::map<std::string, double> keys;  // caps that trade like pins (VCC / AVCC) still count as kept
                      for (const auto& m : placed.items()) {
                          const std::string line = m.asString("");
                          const size_t at = line.rfind(" (");
                          keys[line.substr(0, line.find(' '))] = at == std::string::npos ? 0 : std::atof(line.c_str() + at + 2);
                      }
                      return keys;
                  };
                  const std::map<std::string, double> served = pairings();
                  bestPlaced = placed;
                  const int allowed = problems();
                  Project best = pr;
                  std::string held, last;  // why the first / the last size tried was refused
                  // One side at a time, the longer first, 1 mm at a time; a refused size does not end that side's search
                  // (Auto Place may do better on a smaller board still) until four in a row are refused. Twice over.
                  int tries = 0;
                  auto shrink = [&](bool width) {
                      double cut = 0;
                      for (int refused = 0; refused < 4 && tries < 80; ++tries) {
                          cut += 1;
                          const double w = best.pcb.settings.width - (width ? cut : 0), h = best.pcb.settings.height - (width ? 0 : cut);
                          if (w < 5 || h < 5) break;
                          sieda_pcb_set_board(P(s), w, h, pr.pcb.settings.trackWidth, pr.pcb.settings.clearance);
                          bool fewer = paired() < pairs;
                          const auto now = pairings();  // and each one Auto Place made at the start, no more than 1 mm further
                          for (const auto& [key, mm] : served)  // capacitors and crystals (a resistor / LED may move)
                              fewer = fewer || ((key[0] == 'C' || key[0] == 'Y') && (!now.count(key) || now.at(key) > mm + 1.0));
                          const bool further = !fewer && spread() > reach,
                                     resized = pr.pcb.settings.width == w && pr.pcb.settings.height == h, worse = resized && problems() > allowed;
                          if (!fewer && !further && resized && !worse) {
                              best = pr, bestPlaced = placed, refused = 0, cut = 0;
                              continue;
                          }
                          last = fewer ? "fewer capacitors / crystals ended by their pins" : further ? "capacitors ended further from their pins"
                                 : !resized ? "the parts no longer fitted" : "the DRC found more problems";
                          if (held.empty()) held = last;
                          ++refused;
                          pr = best;
                      }
                  };
                  const bool wide = pr.pcb.settings.width >= pr.pcb.settings.height;
                  for (int pass = 0; pass < 2; ++pass) shrink(wide), shrink(!wide);
                  pr = best;
                  // Pull in: each part steps toward the main IC while it stays legal and spaced (an edge part only away
                  // from its edge, and only while it stays the outermost part there); then the board fits them.
                  if (const auto classes = partClasses(pr); true) {
                      const Component* hub = nullptr;
                      for (const auto& c : pr.schematic.components())
                          if (c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c) && (!hub || c.def().pins.size() > hub->def().pins.size())) hub = &c;
                      const Vec2 c0 = hub ? hub->pcb.position : Vec2{};
                      const double W = pr.pcb.settings.width, H = pr.pcb.settings.height;
                      std::vector<int> ids;
                      for (const auto& c : pr.schematic.components())
                          if (hub && c.id != hub->id && c.hasFootprint() && c.pcb.placed && !c.pcb.locked && pr.pcb.onBoard(c)) ids.push_back(c.id);
                      auto outermost = [&](int id, const Rect& r, int edge) {  // no other part further out on that side
                          for (const auto& o : pr.schematic.components()) {
                              if (o.id == id || !o.hasFootprint() || !o.pcb.placed || !pr.pcb.onBoard(o)) continue;
                              const Rect q = pr.pcb.courtyard(o);
                              if ((edge == 1 && q.x0 < r.x0) || (edge == 2 && q.x1 > r.x1) || (edge == 4 && q.y0 < r.y0) || (edge == 8 && q.y1 > r.y1)) return false;
                          }
                          return true;
                      };
                      // No step takes a capacitor / crystal more than 1 mm further from what it serves (the nearest pads on
                      // a shared net of "C2 → U1.VCC (2.7 mm)" / "C6 → Y1"); the reply's distances follow the moves.
                      auto pairGaps = [&] {
                          const auto pads = pr.pcb.pads(pr.schematic);
                          std::map<std::string, double> gaps;
                          const std::string arrow = " → ";
                          for (const auto& m : bestPlaced.items()) {
                              const std::string line = m.asString("");
                              if (line.find(arrow) == std::string::npos) continue;
                              const std::string rest = line.substr(line.find(arrow) + arrow.size());
                              const Component *ca = pr.schematic.findByRef(line.substr(0, line.find(' '))),
                                              *cb = pr.schematic.findByRef(rest.substr(0, rest.find_first_of(". ")));
                              double best = 1e9;
                              for (const auto& q : pads)
                                  for (const auto& r : pads)
                                      if (ca && cb && q.componentId == ca->id && r.componentId == cb->id && q.net >= 0 && q.net == r.net)
                                          best = std::min(best, (q.position - r.position).length());
                              if (best < 1e8) gaps[line] = best;
                          }
                          return gaps;
                      };
                      const auto gaps0 = pairGaps();
                      auto pairsKept = [&] {
                          const auto now = pairGaps();
                          for (const auto& [line, mm] : gaps0)
                              if (!now.count(line) || now.at(line) > mm + 1.0) return false;
                          return true;
                      };
                      for (int round = 0; round < 8 && hub; ++round) {
                          std::sort(ids.begin(), ids.end(), [&](int l, int r) {
                              return (pr.schematic.find(l)->pcb.position - c0).length() > (pr.schematic.find(r)->pcb.position - c0).length();
                          });
                          for (int id : ids) {
                              const Component& c = *pr.schematic.find(id);
                              const Rect cy = pr.pcb.courtyard(c);
                              const int edge = cy.x0 < 2.5 ? 1 : cy.x1 > W - 2.5 ? 2 : cy.y0 < 2.5 ? 4 : cy.y1 > H - 2.5 ? 8 : 0;  // (Auto Place's edge parts)
                              Vec2 dir = c0 - c.pcb.position;
                              if (edge) dir = edge < 4 ? Vec2{dir.x, 0} : Vec2{0, dir.y};
                              for (double t : {0.5, 0.35, 0.2, 0.1, 0.05}) {
                                  const Vec2 to = c.pcb.position + dir * t;
                                  const PlacementCheck chk = pr.checkPlacement(id, to, c.pcb.rotation, c.pcb.bottom, 0.25);
                                  if ((chk.position - c.pcb.position).length() < 0.2 || !chk.legal ||
                                      !crowding(pr, classes, id, chk.courtyard, c.pcb.bottom).empty() || (edge && !outermost(id, chk.courtyard, edge)))
                                      continue;
                                  const Vec2 was = c.pcb.position;
                                  pr.placeComponent(id, to, c.pcb.rotation, c.pcb.bottom, 0.25);
                                  if (pairsKept()) break;
                                  pr.schematic.find(id)->pcb.position = was;
                                  pr.schematicChanged();
                              }
                          }
                      }
                      if (hub && sieda_pcb_fit_board(P(s), argNum(a, "margin", 3)) == 1 && problems() <= allowed &&
                          pr.pcb.settings.width * pr.pcb.settings.height < best.pcb.settings.width * best.pcb.settings.height - 1) {
                          best = pr;
                          const auto now = pairGaps();
                          Json moved = Json::array();
                          for (const auto& m : bestPlaced.items()) {
                              std::string line = m.asString("");
                              const size_t at = line.rfind(" (");
                              if (at != std::string::npos && gaps0.count(line) && now.count(line)) {
                                  char mm[32];
                                  std::snprintf(mm, sizeof mm, " (%.1f mm)", std::max(0.0, std::atof(line.c_str() + at + 2) + now.at(line) - gaps0.at(line)));
                                  line = line.substr(0, at) + mm;
                              }
                              moved.push(line);
                          }
                          bestPlaced = moved;
                      }
                      pr = best;
                  }
                  P(s)->router.reset();
                  Json board = projectSummary(s).get("board");
                  if (bestPlaced.size()) board["decouplingPlaced"] = bestPlaced;
                  if (clearedTracks || clearedVias)
                      board["routingCleared"] = std::to_string(clearedTracks) + " tracks and " + std::to_string(clearedVias) +
                                                " vias removed (compact re-places every part): run pcb_autoroute again.";
                  if (best.pcb.settings.width != widthWas || best.pcb.settings.height != heightWas) {
                      if (!last.empty()) board["stopped"] = "1 mm smaller on either side: " + last + "; pcb_set_board with a size and pcb_autoplace all try one anyway.";
                  } else
                      board["unchanged"] = "No smaller board kept the placement as good (" + held + " 1 mm smaller); pcb_set_board with "
                                           "a size and pcb_autoplace all try one anyway.";
                  return out(board);
              }
              check(sieda_pcb_fit_board(P(s), argNum(a, "margin", 3)) == 1, "Nothing placed to fit to");
              return out(projectSummary(s).get("board"));
          });
    t.add("pcb", "pcb_set_stackup", "Stack-up", Kind::Edit, true,
          "Laminate (fr4, fr4-hightg, isola-370hr, rogers-4350b, megtron-6, polyimide, ims-aluminium), construction "
          "(rigid, rigid-flex, metal-core), impedance targets (Ω) and backdrilling. Returns the stack-up with the "
          "impedance-controlled widths.",
          Schema()
              .str("material", "Laminate id")
              .str("construction", "rigid, rigid-flex or metal-core")
              .num("singleEndedOhms", "Single-ended target (Ω)")
              .num("differentialOhms", "Differential target (Ω)")
              .boolean("backdrill", "Backdrill high-speed via stubs"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const auto& st = p->project.pcb.settings;
              check(sieda_pcb_set_stackup(p, argStr(a, "material", st.material).c_str(), argStr(a, "construction", st.construction).c_str(),
                                          argNum(a, "singleEndedOhms", st.singleEndedImpedance),
                                          argNum(a, "differentialOhms", st.differentialImpedance),
                                          argBool(a, "backdrill", st.backdrill) ? 1 : 0) == 1,
                    "Unknown material or construction");
              Json j = takeJson(sieda_stackup_json(p));
              double widest = 0;  // over 1 mm the router keeps the board's own widths (no plane close enough below)
              for (const auto& l : j.get("layers").items())
                  if (l.get("type").asString("") == "copper")
                      widest = std::max({widest, l.get("diffWidth").asNumber(0), l.get("seWidth").asNumber(0)});
              if (widest > 1.0)
                  j["warning"] = "The impedance targets need tracks up to " + Json(std::round(widest * 100) / 100).dump() +
                                 " mm wide on this stack-up: impractical, so the router keeps the board's track widths. "
                                 "Controlled impedance needs a plane close below the tracks: 4 layers (pcb_set_board layers 4).";
              return out(j);
          });
    t.add("pcb", "pcb_rule_presets", "Rule presets", Kind::Read, true,
          "Standard design-rule presets (track, clearance, via, annular ring, hole limits) for pcb_set_board.", Schema(),
          [](McpServer&, const Json&) {
              Json j = obj();
              j["presets"] = takeJson(sieda_design_rule_presets_json());
              return out(j);
          });
    t.add("pcb", "pcb_set_net_width", "Net width", Kind::Edit, true,
          "Routes one net with its own track width (mm; 0 removes it). auto: size every power net from the DC "
          "operating point (IPC-2221) instead.",
          Schema().str("net", "Net name").num("width", "Track width (mm)").boolean("auto", "Size the nets automatically"),
          [](McpServer& s, const Json& a) {
              if (argBool(a, "auto", false)) return out(takeJson(sieda_pcb_auto_net_widths(P(s))));
              check(sieda_pcb_set_net_width(P(s), requireStr(a, "net").c_str(), argNum(a, "width", 0)) == 1, "Could not set the width");
              Json j = obj();
              j["net"] = argStr(a, "net");
              j["width"] = argNum(a, "width", 0);
              return out(j);
          });
    t.add("pcb", "pcb_eco_preview", "Update PCB preview", Kind::Read, true,
          "What \"Update PCB from schematic\" would change: footprints to add / remove, nets, pours, rules (nets of a "
          "single pin are only counted: singlePinNets).", Schema(),
          [](McpServer& s, const Json&) {
              Json j = obj(), kept = Json::array();
              int single = 0;  // a net of one pin (an unconnected or no-connect pin) connects nothing
              const Json changes = takeJson(sieda_pcb_eco_preview(P(s)));
              for (const auto& c : changes.items()) {
                  const bool lone = c.get("section").asString("") == "net" && c.get("action").asString("") == "add" &&
                                    c.get("detail").asString("").find(' ') == std::string::npos;
                  if (lone) ++single;
                  else kept.push(c);
              }
              j["changes"] = kept;
              if (single) j["singlePinNets"] = single;
              return out(j);
          });
    t.add("pcb", "pcb_update_from_schematic", "Update PCB", Kind::Edit, false,
          "Forward annotation: places the footprints of new parts (Auto Place spots), removes parts that are gone, "
          "carries net rules, records the baseline. keys picks changes from pcb_eco_preview (default all). Then "
          "improve placement with pcb_place_footprint or pcb_autoplace.",
          Schema().array("keys", "Change keys to apply (default all)", "string"), [](McpServer& s, const Json& a) {
              const std::string keys = a.get("keys").isArray() ? a.get("keys").dump() : std::string();
              const double w0 = proj(s).pcb.settings.width, h0 = proj(s).pcb.settings.height;
              Json r = takeJson(sieda_apply_pcb_eco(P(s), keys.empty() ? nullptr : keys.c_str()));
              if (r.isObject() && r.get("report").isArray()) {  // a net of one pin (a no-connect pin) connects nothing
                  Json kept = Json::array();
                  int single = 0;
                  for (const auto& line : r.get("report").items()) {
                      const std::string t = line.asString("");
                      const size_t colon = t.find(": ");
                      const bool lone = t.rfind("add net ", 0) == 0 && colon != std::string::npos && t.find(' ', colon + 2) == std::string::npos;
                      if (lone) ++single;
                      else kept.push(line);
                  }
                  if (single) kept.push(std::to_string(single) + " single-pin nets (unconnected or no-connect pins) not listed");
                  r["report"] = kept;
              }
              if (r.isObject()) r["board"] = projectSummary(s).get("board"), noteBoardGrown(s, w0, h0, r);
              if (r.isObject() && r.get("placementQueue").size() && !proj(s).pcb.tracks.empty())
                  r["next"] = "The rest stays as placed and routed: the new parts sit at Auto Place spots; pcb_place_footprint moves "
                              "one by the pads it joins (pcb_summary gives their x / y), then pcb_autoroute routes them; "
                              "pcb_autoplace all: true would re-place every part.";
              return out(r);
          });
    t.add("pcb", "pcb_autoplace", "Auto place", Kind::Edit, false,
          "Automatic placement: all = true re-places every unlocked footprint (antenna parts and USB / connectors with "
          "an open end first, at the top / bottom edge facing out; edges: false skips that); false places only those "
          "not on the board yet. decoupling (default on with all) gives each IC supply pin the nearest small capacitor (≤ 1 µF; a regulator's "
          "input / output also a bulk one) between its net and ground, one per pin, moves one further than 3 mm to the "
          "nearest free spot by the pin (as near as the room there allows: decouplingPlaced gives each distance), and puts a crystal (Y…) by the pins it drives with its load capacitors beside "
          "it (shorter loops; on a dense 2-layer board it leaves the router less room around the IC).",
          Schema().boolean("all", "Re-place everything (default false)")
              .boolean("edges", "With all: antenna parts to the top edge (with a keep-out), USB / connectors with an open "
                                "end to the bottom edge, facing out (default true)")
              .boolean("decoupling", "Then place a small capacitor next to each IC supply pin and crystals by their pins "
                                     "(default: on with all)")
              .boolean("grouped", "Then line up the other passives of one kind in rows in designator order (C1 C2 C3, "
                                  "R1 R2 R3 …) near their connections (default false)"),
          autoplaceTool);
    t.add("pcb", "pcb_place_footprint", "Place footprint", Kind::Edit, true,
          "Moves / turns / flips footprints (centre x, y in mm; without them a placed part turns or flips where it stands): "
          "one (ref, x, y …) or many at once (placements: "
          "[{ref, x, y, rotation, bottom, force}]). A pose that overlaps another part, leaves the board, hits a "
          "keep-out or stands nearer a neighbour than the assembly spacing standard (tooClose; see pcb_drc) is refused "
          "(the reasons are returned) unless force is true; force also moves a locked part (it stays locked). A batch "
          "tries each refused pose again once the others have moved. Tracks left on a moved part's old pads (and stubs "
          "they leave) are removed (copperRemoved): route again afterwards.",
          Schema()
              .str("ref", "Designator")
              .num("x", "Centre x (mm)")
              .num("y", "Centre y (mm)")
              .integer("rotation", "Rotation (0, 90, 180, 270; default unchanged)")
              .boolean("bottom", "Bottom side (default unchanged)")
              .num("grid", "Snap grid (mm; 0 = none)")
              .boolean("force", "Place even if illegal or locked")
              .array("placements", "Several parts: [{\"ref\":\"C1\",\"x\":10,\"y\":5,\"rotation\":90}, …]", "object"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              auto placeOne = [&](const Json& e) {
                  if (!e.has("ref")) throw ToolError("Each placement needs ref");
                  const int id = componentId(*p, e.get("ref"));
                  const Component* c = p->project.schematic.find(id);
                  if (!c->hasFootprint()) throw ToolError(c->ref + " has no footprint");
                  // Without x / y it turns or flips where it stands (a placed part).
                  if ((!e.has("x") || !e.has("y")) && !c->pcb.placed) throw ToolError(c->ref + " is not placed yet: give x and y");
                  const bool force = argBool(e, "force", argBool(a, "force", false)), locked = c->pcb.locked;
                  std::vector<Pad> oldPads;  // its copper before the move: the tracks on them are ripped up
                  if (c->pcb.placed)
                      for (const auto& pad : p->project.pcb.pads(p->project.schematic))
                          if (pad.componentId == id) oldPads.push_back(pad);
                  const PcbPlacement was = c->pcb;
                  if (force && locked) sieda_pcb_lock_footprint(p, id, 0);
                  Json r = takeJson(sieda_pcb_place_footprint(p, id, argNum(e, "x", c->pcb.position.x), argNum(e, "y", c->pcb.position.y),
                                                              argInt(e, "rotation", c->pcb.rotation),
                                                              argBool(e, "bottom", c->pcb.bottom) ? 1 : 0,
                                                              argNum(e, "grid", argNum(a, "grid", 0)), force ? 1 : 0));
                  if (force && locked) sieda_pcb_lock_footprint(p, id, 1);
                  if (!r.isObject()) throw ToolError("Could not place " + c->ref);
                  Json j = obj();
                  j["ref"] = c->ref;
                  for (const char* k : {"x", "y", "rotation", "bottom", "legal", "committed", "issues"}) j[k] = r.get(k);
                  if (Component* m = p->project.schematic.find(id); r.get("committed").asBool(false)) {
                      // Nearer than the assembly spacing standard: refused like an overlap unless force.
                      const auto classes = partClasses(p->project);
                      Json close = Json::array();
                      for (const auto& [o, g] : crowding(p->project, classes, id, p->project.pcb.courtyard(*m), m->pcb.bottom))
                          close.push(o->ref + " " + Json(std::round(g * 100) / 100).dump() + " mm (standard " +
                                     Json(standardGap(classes.at(id), classes.at(o->id))).dump() + " mm)");
                      if (close.size()) j["tooClose"] = close;
                      if (close.size() && !force) {
                          m->pcb = was;
                          j["committed"] = false, j["legal"] = false;
                      } else if (!oldPads.empty() && ((m->pcb.position - was.position).length() > 1e-6 ||
                                                      m->pcb.rotation != was.rotation || m->pcb.bottom != was.bottom)) {
                          if (const int gone = removeStranded(p->project, oldPads)) {
                              j["copperRemoved"] = gone;
                              j["note"] = "Its old tracks were removed: run pcb_autoroute (or pcb_route) to reconnect it.";
                          }
                      }
                  }
                  if (!j.get("committed").asBool(false))
                      j["note"] = j.has("tooClose") ? "Not placed: closer than the assembly spacing standard (tooClose). Move "
                                                      "further, or force to place it anyway."
                                                    : "Not placed: the pose is illegal (see issues). Try pcb_suggest_placement or force.";
                  return j;
              };
              if (!a.get("placements").isArray()) return out(placeOne(a));
              // The batch's own parts are lifted first, so they do not stand in each other's way; a pose refused
              // twice (a pose refused first may be free once the others have moved) leaves its part where it was.
              std::vector<std::pair<Component*, PcbPlacement>> lifted;
              for (const auto& e : a.get("placements").items())
                  if (const Component* f = p->project.schematic.findByRef(e.get("ref").asString("")); f && f->pcb.placed && !f->pcb.locked) {
                      Component* c = p->project.schematic.find(f->id);
                      lifted.push_back({c, c->pcb});
                      c->pcb.placed = false;
                  }
              std::set<int> stayed;  // refused twice: back at the old pose
              auto restore = [&] {
                  for (auto& [c, was] : lifted)
                      if (!c->pcb.placed) c->pcb = was, stayed.insert(c->id);
              };
              std::vector<Json> list;
              int placed = 0;
              try {
                  for (int pass = 0; pass < 2; ++pass)
                      for (size_t i = 0; i < a.get("placements").size(); ++i)
                          if (!pass || !list[i].get("committed").asBool(false)) {
                              Json r = placeOne(a.get("placements").items()[i]);
                              placed += r.get("committed").asBool(false);
                              if (pass) list[i] = r; else list.push_back(r);
                          }
              } catch (...) {
                  restore();
                  throw;
              }
              restore();
              for (Json& r : list)  // one left at its old pose may now lie under a part this batch moved there
                  if (const Component* c = p->project.schematic.findByRef(r.get("ref").asString("")); c && stayed.count(c->id)) {
                      Json under = Json::array();
                      for (const auto& is : p->project.checkPlacement(c->id, c->pcb.position, c->pcb.rotation, c->pcb.bottom, 0).issues)
                          if (is.code == "PLACE_OVERLAP" && is.other >= 0) under.push(p->project.schematic.find(is.other)->ref);
                      if (!under.size()) continue;
                      const PlacementCheck free = p->project.suggestPlacement(c->id, 0.5);
                      r["overlaps"] = under;
                      r["note"] = "Not placed, so it stayed at its old pose, which a part of this batch now overlaps: move it "
                                  "(a free spot: x " + Json(free.position.x).dump() + ", y " + Json(free.position.y).dump() + ").";
                  }
              Json j = obj();
              j["placed"] = placed;
              j["results"] = Json::array();
              for (auto& r : list) j["results"].push(std::move(r));
              return out(j);
          });
    t.add("pcb", "pcb_suggest_placement", "Suggest placement", Kind::Read, true,
          "The nearest free legal spot for a footprint that keeps the assembly spacing, close to the pads it connects "
          "to (ground counts only for a part on ground alone). Nothing changes.",
          Schema().str("ref", "Designator", true).num("grid", "Snap grid (mm, default 0.5)"), [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              Json r = takeJson(sieda_pcb_suggest_placement(P(s), id, argNum(a, "grid", 0.5)));
              if (!r.isObject()) throw ToolError("No footprint for " + argStr(a, "ref"));
              // The nearest spot from there that also keeps the assembly spacing (pcb_place_footprint refuses less).
              const Project& pr = proj(s);
              const auto classes = partClasses(pr);
              const Vec2 at{r.get("x").asNumber(0), r.get("y").asNumber(0)};
              const int rot = r.get("rotation").asInt(0);
              const bool bottom = r.get("bottom").asBool(false);
              bool found = !r.get("legal").asBool(false) || spaced(pr, classes, id, at, rot, bottom);
              for (double rad = 0.5; rad <= 10 && !found; rad += 0.5)
                  for (int k = 0; k < 16 && !found; ++k) {
                      const Vec2 q = at + Vec2{std::cos(k * kPi / 8), std::sin(k * kPi / 8)} * rad;
                      if ((found = spaced(pr, classes, id, q, rot, bottom) && pr.checkPlacement(id, q, rot, bottom, 0.25).legal))
                          r["x"] = std::round(q.x * 100) / 100, r["y"] = std::round(q.y * 100) / 100;
                  }
              Json j = obj();
              for (const char* k : {"x", "y", "rotation", "bottom", "legal", "issues"}) j[k] = r.get(k);
              return out(j);
          });
    t.add("pcb", "pcb_align", "Align footprints", Kind::Edit, false,
          "Lines footprints' courtyards up on the outermost one's edge or centre line (left, right, top, bottom, "
          "centreX, centreY), or spaces them evenly between the outermost two (distributeX, distributeY: three or "
          "more). Their tracks come along.",
          Schema().array("refs", "Designators (two or more)", "string", true)
              .str("mode", "How", true, {"left", "right", "top", "bottom", "centreX", "centreY", "distributeX", "distributeY"}),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const Project& pr = p->project;
              const std::string mode = argStr(a, "mode", "");
              const bool spread = mode.rfind("distribute", 0) == 0;
              struct Box { int id; Rect r; Vec2 at; };
              std::vector<Box> boxes;
              for (const auto& ref : a.get("refs").items()) {
                  const int id = componentId(*p, ref);
                  const Component* c = pr.schematic.find(id);
                  check(c && c->hasFootprint() && c->pcb.placed, ref.asString("?") + " is not on the board");
                  boxes.push_back({id, pr.pcb.courtyard(*c), c->pcb.position});
              }
              check(boxes.size() >= (spread ? 3u : 2u), spread ? "Distribute needs three or more footprints" : "Align needs two or more footprints");
              Rect all = boxes[0].r;
              for (const auto& b : boxes)
                  all = {std::min(all.x0, b.r.x0), std::min(all.y0, b.r.y0), std::max(all.x1, b.r.x1), std::max(all.y1, b.r.y1)};
              std::vector<std::pair<int, Vec2>> shift;
              const bool alongX = mode == "distributeX";
              if (spread) {
                  std::sort(boxes.begin(), boxes.end(), [&](const Box& l, const Box& r) { return alongX ? l.r.x0 < r.r.x0 : l.r.y0 < r.r.y0; });
                  double used = 0;
                  for (const auto& b : boxes) used += alongX ? b.r.width() : b.r.height();
                  const double gap = ((alongX ? all.width() : all.height()) - used) / static_cast<double>(boxes.size() - 1);
                  double at = alongX ? all.x0 : all.y0;
                  for (const auto& b : boxes) {
                      const double d = at - (alongX ? b.r.x0 : b.r.y0);
                      shift.push_back({b.id, alongX ? Vec2{d, 0} : Vec2{0, d}});
                      at += (alongX ? b.r.width() : b.r.height()) + gap;
                  }
              } else {
                  for (const auto& b : boxes) {
                      const Vec2 d = mode == "left"    ? Vec2{all.x0 - b.r.x0, 0}
                                     : mode == "right" ? Vec2{all.x1 - b.r.x1, 0}
                                     : mode == "top"   ? Vec2{0, all.y0 - b.r.y0}
                                     : mode == "bottom" ? Vec2{0, all.y1 - b.r.y1}
                                     : mode == "centreX" ? Vec2{all.center().x - b.r.center().x, 0}
                                                         : Vec2{0, all.center().y - b.r.center().y};
                      shift.push_back({b.id, d});
                  }
              }
              int moved = 0;
              for (const auto& [id, d] : shift) {
                  if (std::fabs(d.x) < 1e-9 && std::fabs(d.y) < 1e-9) continue;
                  const Vec2 at = pr.schematic.find(id)->pcb.position;
                  moved += sieda_pcb_move_footprint_with_tracks(p, id, at.x + d.x, at.y + d.y) == 1;
              }
              Json j = obj();
              j["moved"] = moved;
              return out(j);
          });
    t.add("pcb", "pcb_layout_block", "Layout reuse", Kind::Edit, false,
          "Reuses a laid-out group (an approved channel, a regulator block). replicate copies the refs group's placement "
          "and the copper inside it onto each target group (same parts, same order; each lands round its first part, "
          "or at x / y). save writes the group's layout to a file in the root (approvedBy, fingerprint); apply lays a "
          "saved layout onto the refs group and says whether it is still the approved one. Copper that would touch "
          "another net is skipped (skipped).",
          Schema().str("action", "replicate, save or apply", true, {"replicate", "save", "apply"})
              .array("refs", "The group (source for replicate / save, target for apply), first part = anchor", "string", true)
              .array("targets", "replicate: target groups [[refs…], …]", "array").str("path", "save / apply: layout file")
              .str("approvedBy", "save: who approved it").num("x", "Anchor position x (mm)").num("y", "Anchor position y (mm)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              auto group = [&](const Json& refs) {
                  std::vector<int> ids;
                  for (const auto& r : refs.items()) {
                      const int id = componentId(*p, r);
                      const Component* c = p->project.schematic.find(id);
                      check(c && c->hasFootprint() && c->pcb.placed, r.asString("?") + " is not on the board");
                      ids.push_back(id);
                  }
                  check(!ids.empty(), "Give the group's parts");
                  return ids;
              };
              auto fingerprint = [](const Json& layout) {
                  uint64_t h = 1469598103934665603ull;  // FNV-1a
                  for (const unsigned char ch : layout.dump()) h = (h ^ ch) * 1099511628211ull;
                  char text[17];
                  std::snprintf(text, sizeof text, "%016llx", static_cast<unsigned long long>(h));
                  return std::string(text);
              };
              const std::string action = argStr(a, "action", "");
              const std::vector<int> ids = group(a.get("refs"));
              auto anchorAt = [&](const std::vector<int>& g) {
                  const Vec2 here = p->project.schematic.find(g[0])->pcb.position;
                  return Vec2{argNum(a, "x", here.x), argNum(a, "y", here.y)};
              };
              Json j = obj();
              if (action == "replicate") {
                  const Json layout = captureLayout(p->project, ids);
                  Json done = Json::array();
                  for (const auto& tg : a.get("targets").items()) {
                      const std::vector<int> target = group(tg);
                      Json r = applyLayout(p, layout, target, anchorAt(target));
                      r["anchor"] = p->project.schematic.find(target[0])->ref;
                      done.push(r);
                  }
                  check(done.size() > 0, "Give the target groups");
                  j["groups"] = done;
              } else if (action == "save") {
                  Json file = obj(), layout = captureLayout(p->project, ids);
                  file["format"] = "sieda.layout/1", file["layout"] = layout, file["hash"] = fingerprint(layout);
                  const std::string by = argStr(a, "approvedBy", "");
                  if (!by.empty()) file["approvedBy"] = by, file["approvedHash"] = fingerprint(layout);
                  j["path"] = s.writeFile(requireStr(a, "path"), file.dump(2) + "\n");
                  j["parts"] = static_cast<int>(layout.get("parts").size()), j["tracks"] = static_cast<int>(layout.get("tracks").size());
                  j["hash"] = file.get("hash");
              } else {
                  const Json file = Json::parse(readFileArg(s, requireStr(a, "path")).asString(""));
                  check(file.get("format").asString("") == "sieda.layout/1", "Not a SiEDA layout file");
                  const Json& layout = file.get("layout");
                  j = applyLayout(p, layout, ids, anchorAt(ids));
                  j["approved"] = !file.get("approvedBy").asString("").empty() && file.get("approvedHash").asString("") == fingerprint(layout);
                  if (j.get("approved").asBool(false)) j["approvedBy"] = file.get("approvedBy");
              }
              return out(j);
          });
    t.add("pcb", "pcb_partitions", "Board partitions", Kind::Edit, false,
          "Several engineers on one board: set the partitions [{name, owner, x0, y0, x1, y1}] (mm; [] removes them) or "
          "list them; check (owner, against = the base project file, e.g. the last pulled version) lists what changed "
          "inside partitions owned by someone else: parts moved, added or removed there, tracks and vias changed there. "
          "clean = nothing.",
          Schema().str("action", "set, list or check", true, {"set", "list", "check"})
              .array("partitions", "set: [{name, owner, x0, y0, x1, y1}]", "object")
              .str("owner", "check: you").str("against", "check: base project file"),
          [](McpServer& s, const Json& a) {
              auto& parts = P(s)->project.pcb.settings.partitions;
              const std::string action = argStr(a, "action", "");
              Json j = obj();
              if (action == "set") {
                  std::vector<BoardSettings::Partition> list;
                  for (const auto& q : a.get("partitions").items()) {
                      const Rect area{q.get("x0").asNumber(0), q.get("y0").asNumber(0), q.get("x1").asNumber(0), q.get("y1").asNumber(0)};
                      check(area.width() > 0 && area.height() > 0, "Each partition needs an area (x0 < x1, y0 < y1)");
                      check(!q.get("owner").asString("").empty(), "Each partition needs an owner");
                      list.push_back({q.get("name").asString("Partition " + std::to_string(list.size() + 1)), q.get("owner").asString(""), area});
                  }
                  parts = list;
              }
              Json listed = Json::array();
              for (const auto& q : parts) {
                  Json pj = obj();
                  pj["name"] = q.name, pj["owner"] = q.owner, pj["x0"] = q.area.x0, pj["y0"] = q.area.y0, pj["x1"] = q.area.x1, pj["y1"] = q.area.y1;
                  listed.push(pj);
              }
              j["partitions"] = listed;
              if (action != "check") return out(j);
              const std::string me = requireStr(a, "owner");
              const Project base = Project::fromJson(Json::parse(readFileArg(s, requireStr(a, "against")).asString("")));
              const Project& now = P(s)->project;
              auto in = [](const Rect& r, Vec2 q) { return q.x >= r.x0 && q.x <= r.x1 && q.y >= r.y0 && q.y <= r.y1; };
              // Copper as text keys (net name, layer, ends to 1 µm) so the two versions compare whatever their ids.
              auto netName = [](const Project& p, int net) {
                  for (const auto& n : p.schematic.nets())
                      if (n.index == net) return n.name;
                  return std::string("?");
              };
              auto copper = [&](const Project& p, const Rect& r) {
                  std::multiset<std::string> keys;
                  char buf[160];
                  for (const auto& t : p.pcb.tracks)
                      if (in(r, t.a) || in(r, t.b)) {
                          Vec2 u = t.a, w = t.b;
                          if (std::make_pair(w.x, w.y) < std::make_pair(u.x, u.y)) std::swap(u, w);
                          std::snprintf(buf, sizeof buf, "T %d %.3f %.3f %.3f %.3f %.3f ", t.layer, u.x, u.y, w.x, w.y, t.width);
                          keys.insert(buf + netName(p, t.net));
                      }
                  for (const auto& v : p.pcb.vias)
                      if (in(r, v.position)) {
                          std::snprintf(buf, sizeof buf, "V %.3f %.3f %.3f ", v.position.x, v.position.y, v.diameter);
                          keys.insert(buf + netName(p, v.net));
                      }
                  return keys;
              };
              Json found = Json::array();
              for (const auto& q : parts) {
                  if (q.owner == me) continue;
                  Json moved = Json::array();
                  for (const auto& c : now.schematic.components()) {
                      if (!c.hasFootprint()) continue;
                      const Component* was = base.schematic.findByRef(c.ref);
                      const bool hereNow = c.pcb.placed && in(q.area, c.pcb.position);
                      const bool hereThen = was && was->pcb.placed && in(q.area, was->pcb.position);
                      if (!hereNow && !hereThen) continue;
                      if (!was || hereNow != hereThen || std::fabs(was->pcb.position.x - c.pcb.position.x) > 1e-6 ||
                          std::fabs(was->pcb.position.y - c.pcb.position.y) > 1e-6 || was->pcb.rotation != c.pcb.rotation ||
                          was->pcb.bottom != c.pcb.bottom)
                          moved.push(c.ref);
                  }
                  for (const auto& c : base.schematic.components())
                      if (c.hasFootprint() && c.pcb.placed && in(q.area, c.pcb.position) && !now.schematic.findByRef(c.ref)) moved.push(c.ref);
                  const auto before = copper(base, q.area), after = copper(now, q.area);
                  std::vector<std::string> diff;
                  std::set_symmetric_difference(before.begin(), before.end(), after.begin(), after.end(), std::back_inserter(diff));
                  if (moved.size() || diff.size()) {
                      Json f = obj();
                      f["partition"] = q.name, f["owner"] = q.owner, f["parts"] = moved, f["copperChanges"] = static_cast<int>(diff.size());
                      found.push(f);
                  }
              }
              j["outsideYours"] = found;
              j["clean"] = found.size() == 0;
              return out(j);
          });
    t.add("pcb", "pcb_rule_pack", "Team rule packs", Kind::Edit, false,
          "A team's design rules as one file shared by every board: track / via sizes, clearances and fab minimums, "
          "net widths and clearances, pair and length rules, DFM pack and overrides, custom rules and routing policies "
          "(not the board's size, outline, stack-up or copper). save writes the board's rules to a file in the root "
          "(name, approvedBy); apply sets them on the board and remembers the pack, so verify_design warns when the "
          "rules drift from it later (VER_RULE_PACK); check lists the rules that differ from the file (or from the "
          "applied pack's fingerprint); show gives the board's rules.",
          Schema().str("action", "save, apply, check or show", true, {"save", "apply", "check", "show"})
              .str("path", "save / apply / check: rule file").str("name", "save: the pack's name (default: the file name)")
              .str("approvedBy", "save: who approved it"),
          [](McpServer& s, const Json& a) {
              BoardSettings& st = P(s)->project.pcb.settings;
              const std::string action = argStr(a, "action", "");
              Json j = obj();
              const Json rules = boardRules(st);
              if (action == "show") {
                  j["rules"] = rules, j["hash"] = boardRulesHash(st);
                  if (!st.rulePackName.empty()) j["pack"] = st.rulePackName, j["matchesPack"] = st.rulePackHash == boardRulesHash(st);
                  return out(j);
              }
              if (action == "save") {
                  std::string name = argStr(a, "name", "");
                  const std::string path = requireStr(a, "path");
                  if (name.empty()) name = std::filesystem::path(path).stem().string();
                  Json file = obj();
                  file["format"] = "sieda.rules/1", file["name"] = name, file["rules"] = rules, file["hash"] = boardRulesHash(st);
                  const std::string by = argStr(a, "approvedBy", "");
                  if (!by.empty()) file["approvedBy"] = by;
                  j["path"] = s.writeFile(path, file.dump(2) + "\n");
                  j["name"] = name, j["hash"] = file.get("hash"), j["rules"] = static_cast<int>(rules.fields().size());
                  return out(j);
              }
              const Json file = Json::parse(readFileArg(s, requireStr(a, "path")).asString(""));
              check(file.get("format").asString("") == "sieda.rules/1" && file.get("rules").isObject(), "Not a SiEDA rule pack");
              const Json& pack = file.get("rules");
              Json differ = Json::array();
              std::set<std::string> keys;
              for (const auto& [k, v] : rules.fields()) keys.insert(k);
              for (const auto& [k, v] : pack.fields()) keys.insert(k);
              for (const auto& k : keys)
                  if (rules.get(k).dump() != pack.get(k).dump()) differ.push(k);
              j["name"] = file.get("name"), j["differences"] = differ;
              if (file.has("approvedBy")) j["approvedBy"] = file.get("approvedBy");
              if (action == "apply") {
                  applyBoardRules(P(s)->project.pcb, pack);
                  st.rulePackName = file.get("name").asString("rules"), st.rulePackHash = boardRulesHash(st);
                  j["applied"] = true, j["hash"] = st.rulePackHash;
              } else {
                  j["clean"] = differ.size() == 0;
              }
              return out(j);
          });
    t.add("pcb", "pcb_routing_policies", "Routing policies", Kind::Edit, false,
          "Routing policies per schematic net class: the copper layers its nets may run on (the autorouter keeps them "
          "there; other layers only to leave their pads) and the most vias each may use. pcb_drc flags tracks off "
          "their layers (ROUTE_POLICY_LAYER) and nets with too many vias (ROUTE_POLICY_VIAS). set replaces them "
          "([] removes them); list shows them.",
          Schema().str("action", "set or list", true, {"set", "list"})
              .array("policies", "set: [{netClass, layers: [layer…], maxVias}] (either may be left out)", "object"),
          [](McpServer& s, const Json& a) {
              AutorouteOptions& o = P(s)->project.pcb.settings.autorouter;
              if (argStr(a, "action", "") == "set") {
                  std::map<std::string, std::vector<int>> layers;
                  std::map<std::string, int> maxVias;
                  const int n = P(s)->project.pcb.settings.layerCount;
                  for (const auto& q : a.get("policies").items()) {
                      const std::string cls = q.get("netClass").asString("");
                      check(!cls.empty(), "Each policy needs its netClass");
                      for (const auto& l : q.get("layers").items()) {
                          check(l.asInt(-1) >= 0 && l.asInt(-1) < n, "No copper layer " + l.dump() + " on this board");
                          layers[cls].push_back(l.asInt(0));
                      }
                      if (q.has("maxVias")) {
                          check(q.get("maxVias").asInt(-1) >= 0, "maxVias is 0 or more");
                          maxVias[cls] = q.get("maxVias").asInt(0);
                      }
                  }
                  o.classLayers = layers, o.classMaxVias = maxVias;
              }
              Json list = Json::array();
              std::set<std::string> classes;
              for (const auto& [c, l] : o.classLayers) classes.insert(c);
              for (const auto& [c, m] : o.classMaxVias) classes.insert(c);
              for (const auto& c : classes) {
                  Json q = obj();
                  q["netClass"] = c;
                  if (o.classLayers.count(c)) {
                      Json ls = Json::array();
                      for (int l : o.classLayers.at(c)) ls.push(l);
                      q["layers"] = ls;
                  }
                  if (o.classMaxVias.count(c)) q["maxVias"] = o.classMaxVias.at(c);
                  list.push(q);
              }
              Json j = obj();
              j["policies"] = list;
              return out(j);
          });
    t.add("pcb", "pcb_rooms", "Rooms", Kind::Edit, false,
          "Rooms: board areas a group of parts belongs in (a channel, a power stage). set replaces them "
          "[{name, refs, x0, y0, x1, y1}] ([] removes them); list shows them with the members outside; place moves "
          "every member outside its room to the free legal spot in it nearest the room's centre (assembly spacing kept). "
          "pcb_drc flags a member outside its room (DRC_ROOM).",
          Schema().str("action", "set, list or place", true, {"set", "list", "place"})
              .array("rooms", "set: [{name, refs: [designator…], x0, y0, x1, y1}] (mm)", "object"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              Project& pr = p->project;
              auto& rooms = pr.pcb.settings.rooms;
              const std::string action = argStr(a, "action", "");
              if (action == "set") {
                  std::vector<BoardSettings::Room> list;
                  for (const auto& q : a.get("rooms").items()) {
                      BoardSettings::Room r{q.get("name").asString("Room " + std::to_string(list.size() + 1)), {},
                                            {q.get("x0").asNumber(0), q.get("y0").asNumber(0), q.get("x1").asNumber(0), q.get("y1").asNumber(0)}};
                      check(r.area.width() > 0 && r.area.height() > 0, "Each room needs an area (x0 < x1, y0 < y1)");
                      for (const auto& ref : q.get("refs").items()) {
                          check(pr.schematic.findByRef(ref.asString("")) != nullptr, "No part " + ref.asString("?"));
                          r.refs.push_back(ref.asString(""));
                      }
                      list.push_back(std::move(r));
                  }
                  rooms = list;
              }
              auto inside = [&](const Component& c, const Rect& area) {
                  const Rect k = pr.pcb.courtyard(c);
                  return k.x0 >= area.x0 - 1e-6 && k.y0 >= area.y0 - 1e-6 && k.x1 <= area.x1 + 1e-6 && k.y1 <= area.y1 + 1e-6;
              };
              Json moved = Json::array(), stuck = Json::array();
              if (action == "place") {
                  const auto classes = partClasses(pr);
                  for (const auto& room : rooms)
                      for (const auto& ref : room.refs) {
                          const Component* c = pr.schematic.findByRef(ref);
                          if (!c || !c->hasFootprint() || (c->pcb.placed && inside(*c, room.area)) || c->pcb.locked) continue;
                          const Rect k = pr.pcb.courtyard(*c);  // the courtyard round the part's position
                          const Vec2 lo = Vec2{k.x0, k.y0} - c->pcb.position, hi = Vec2{k.x1, k.y1} - c->pcb.position;
                          const Vec2 mid = room.area.center();
                          std::vector<Vec2> spots;
                          // On the 0.25 mm grid the spacing check snaps to, so the pose checked is the pose placed.
                          for (double y = std::ceil((room.area.y0 - lo.y) * 4 - 1e-6) / 4; y <= room.area.y1 - hi.y + 1e-9; y += 0.25)
                              for (double x = std::ceil((room.area.x0 - lo.x) * 4 - 1e-6) / 4; x <= room.area.x1 - hi.x + 1e-9; x += 0.25)
                                  spots.push_back({x, y});
                          std::sort(spots.begin(), spots.end(), [&](Vec2 u, Vec2 v) { return (u - mid).length() < (v - mid).length(); });
                          bool placed = false;
                          for (const Vec2 q : spots) {
                              if (!spaced(pr, classes, c->id, q, c->pcb.rotation, c->pcb.bottom) ||
                                  !pr.checkPlacement(c->id, q, c->pcb.rotation, c->pcb.bottom, 0).legal)
                                  continue;
                              placed = takeJson(sieda_pcb_place_footprint(p, c->id, q.x, q.y, c->pcb.rotation, c->pcb.bottom ? 1 : 0, 0, 0))
                                           .get("committed").asBool(false);
                              if (placed) break;
                          }
                          (placed ? moved : stuck).push(ref);
                      }
              }
              Json list = Json::array();
              for (const auto& room : rooms) {
                  Json rj = obj(), refs = Json::array(), outside = Json::array();
                  for (const auto& ref : room.refs) {
                      refs.push(ref);
                      const Component* c = pr.schematic.findByRef(ref);
                      if (c && c->hasFootprint() && c->pcb.placed && !inside(*c, room.area)) outside.push(ref);
                  }
                  rj["name"] = room.name, rj["refs"] = refs, rj["outside"] = outside;
                  rj["x0"] = room.area.x0, rj["y0"] = room.area.y0, rj["x1"] = room.area.x1, rj["y1"] = room.area.y1;
                  list.push(rj);
              }
              Json j = obj();
              j["rooms"] = list;
              if (action == "place") j["moved"] = moved, j["noRoom"] = stuck;
              return out(j);
          });
    t.add("pcb", "pcb_variant_check", "Check variants", Kind::Read, true,
          "Checks the board as each assembly variant is built (the base design and every variant): its unfitted parts, "
          "connections between fitted parts still unrouted, and fitted parts whose courtyards overlap. Parts that "
          "overlap but are never fitted together in any variant are listed as alternates (allowed: one footprint "
          "area shared by two options). Variants change which parts are fitted, not the copper, so one routing serves "
          "all of them; ok = no unrouted connection and no real overlap in any variant.",
          Schema(), [](McpServer& s, const Json&) {
              const Project& pr = proj(s);
              std::vector<std::string> names{""};
              for (const auto& v : pr.variants) names.push_back(v.name);
              std::map<std::string, std::set<int>> fittedIn;  // variant → fitted part ids
              for (const auto& n : names) {
                  const Schematic sch = pr.variantSchematic(n);
                  for (const auto& c : sch.components())
                      if (c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c) && !c.sourcing.dnp) fittedIn[n].insert(c.id);
              }
              // Overlapping courtyards (same side), and whether any variant fits both parts.
              std::vector<const Component*> placed;
              for (const auto& c : pr.schematic.components())
                  if (c.hasFootprint() && c.pcb.placed && pr.pcb.onBoard(c)) placed.push_back(&c);
              std::vector<std::pair<int, int>> overlaps;
              for (size_t i = 0; i < placed.size(); ++i)
                  for (size_t k = i + 1; k < placed.size(); ++k) {
                      if (placed[i]->pcb.bottom != placed[k]->pcb.bottom) continue;
                      const Rect a1 = pr.pcb.courtyard(*placed[i]), a2 = pr.pcb.courtyard(*placed[k]);
                      if (a1.x0 < a2.x1 - 1e-6 && a2.x0 < a1.x1 - 1e-6 && a1.y0 < a2.y1 - 1e-6 && a2.y0 < a1.y1 - 1e-6)
                          overlaps.push_back({placed[i]->id, placed[k]->id});
                  }
              const std::vector<Pad> pads = pr.pcb.pads(pr.schematic);
              const std::vector<size_t> groups = pr.pcb.padGroups(pr.schematic);
              auto refOf = [&](int id) { return pr.schematic.find(id)->ref; };
              Json variants = Json::array(), alternates = Json::array();
              bool ok = true;
              for (const auto& [a, b] : overlaps) {
                  bool together = false;
                  for (const auto& n : names) together = together || (fittedIn[n].count(a) && fittedIn[n].count(b));
                  if (!together) {
                      Json pair = Json::array();
                      pair.push(refOf(a)), pair.push(refOf(b));
                      alternates.push(pair);
                  }
              }
              for (const auto& n : names) {
                  const std::set<int>& fit = fittedIn[n];
                  // A net is open in this variant when its fitted pads sit in more than one copper group.
                  std::map<int, std::set<size_t>> netGroups;
                  for (size_t i = 0; i < pads.size(); ++i)
                      if (pads[i].net >= 0 && fit.count(pads[i].componentId)) netGroups[pads[i].net].insert(groups[i]);
                  Json open = Json::array(), clash = Json::array(), unfitted = Json::array();
                  for (const auto& [net, g] : netGroups)
                      if (g.size() > 1)
                          for (const auto& nn : pr.schematic.nets())
                              if (nn.index == net) open.push(nn.name);
                  for (const auto& [a, b] : overlaps)
                      if (fit.count(a) && fit.count(b)) {
                          Json pair = Json::array();
                          pair.push(refOf(a)), pair.push(refOf(b));
                          clash.push(pair);
                      }
                  for (const Component* c : placed)
                      if (!fit.count(c->id)) unfitted.push(c->ref);
                  Json v = obj();
                  v["variant"] = n.empty() ? "(base)" : n, v["unfitted"] = unfitted, v["openNets"] = open, v["overlaps"] = clash;
                  v["ok"] = open.size() == 0 && clash.size() == 0;
                  ok = ok && v.get("ok").asBool(false);
                  variants.push(v);
              }
              Json j = obj();
              j["variants"] = variants, j["alternates"] = alternates, j["ok"] = ok;
              return out(j);
          });
    t.add("pcb", "pcb_lock_footprint", "Lock footprint", Kind::Edit, true,
          "Locks footprints where they are (Auto Place and the autorouter's placement keep them) or unlocks them: ref or "
          "refs.",
          Schema().str("ref", "Designator").array("refs", "Several designators", "string")
              .boolean("locked", "true locks (default), false unlocks"),
          [](McpServer& s, const Json& a) {
              std::vector<Json> refs;
              if (a.has("ref")) refs.push_back(a.get("ref"));
              for (const auto& r : a.get("refs").items()) refs.push_back(r);
              if (refs.empty()) throw ToolError("Give ref or refs");
              Json done = Json::array();
              for (const auto& r : refs) {
                  check(sieda_pcb_lock_footprint(P(s), componentId(*P(s), r), argBool(a, "locked", true) ? 1 : 0) == 1,
                        r.asString("") + " is not placed on the board");
                  done.push(r);
              }
              Json j = obj();
              j["refs"] = done;
              j["locked"] = argBool(a, "locked", true);
              return out(j);
          });
    t.add("pcb", "pcb_autoroute", "Autoroute", Kind::Edit, false,
          "Routes the board. Unplaced footprints are placed first. preset: default, fast, quality (coupled pairs, "
          "length rules, fewer vias, gloss), fanout, nets, netclass, area. options (merged over the preset): "
          "{coupledPairs, lengthAware, minimizeVias, gloss, arcCorners, arcRadius, teardrops, teardropStyle, fast, "
          "fanoutOnly, nets:[names], netClass, area:{x0,y0,x1,y1}, protectLocked, classLayers, pinSwap, "
          "improvePlacement (Auto Place's improvement pass), lengthAwareSearch (with lengthAware: nets with a length "
          "target are routed again toward it, shorter or through a waypoint, before meandering), shapeCompletion "
          "(connections the grid leaves unrouted are routed by the gridless shape-based engine), viasOffPads (a pour pad's "
          "fan-out via clears its SMD pad; on by default here)}. Without a preset, the quality preset's options are on (boards up to "
          "300 parts; not on a partial route). A whole-board route "
          "on a small board that leaves connections open tries again (shape-based completion, two-pin parts turned "
          "round) and lists the repairs kept. A whole-board route re-routes every net, hand routing included: to keep "
          "hand-routed nets, route the others with preset nets. A preset or options apply to this call only; keep: true "
          "saves a whole-board one as the board's strategy (the app's Auto Route uses it). Returns "
          "routed / failed connections, vias and track length; small two-pad parts get even copper at both pads "
          "(tombstoningPrevented lists them) so they do not stand on end.",
          Schema()
              .str("preset", "Strategy preset (pcb_autoroute_presets)")
              .object("options", "Strategy options (see description)")
              .boolean("keep", "Save the preset / options as the board's strategy (default false)")
              .boolean("clearFirst", "Remove the existing routing first")
              .boolean("powerWidths", "Route power / ground nets without a width of their own 0.4 mm wide (default true)")
              .boolean("pour", "A whole-board route on a board with no pour pours the ground net first: both layers of a 2-layer board, else the bottom (default true)")
              .boolean("jumpers", "A 1-layer board: join what is left open by wire links (pcb_add_jumpers; default true)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const auto stitched = stitchingVias(p->project);  // re-laid after a whole-board route clears them
              const AutorouteOptions previous = p->project.pcb.settings.autorouter;
              if (a.has("preset") || a.get("options").isObject()) {
                  Json o = obj();
                  const std::string preset = argStr(a, "preset", "");
                  if (!preset.empty()) {
                      bool found = false;
                      const Json presets = takeJson(sieda_autoroute_presets());
                      for (const auto& pr : presets.items())
                          if (pr.get("name").asString("") == preset) {
                              o = pr.get("options");
                              found = true;
                          }
                      if (!found) throw ToolError("Unknown preset \"" + preset + "\" (pcb_autoroute_presets)");
                  }
                  if (a.get("options").isObject())
                      for (const auto& [k, v] : a.get("options").fields()) o[k] = v;
                  check(sieda_pcb_set_autoroute_options(p, o.dump().c_str()) == 1, "Invalid autoroute options");
              }
              // Over MCP a route on a small board is the quality preset (gloss, fewer vias, coupled pairs, lengths) unless a
              // preset or options choose.
              auto& ar = p->project.pcb.settings.autorouter;
              if (!a.has("preset") && !ar.scoped() && !ar.fanoutOnly && p->project.schematic.components().size() <= 300)
                  for (auto [key, on] : {std::pair{"gloss", &ar.gloss}, {"minimizeVias", &ar.minimizeVias},
                                         {"coupledPairs", &ar.coupledPairs}, {"lengthAware", &ar.lengthAware}})
                      if (!a.get("options").has(key)) *on = true;
              const bool reroute = !p->project.pcb.tracks.empty();  // a route over one before: project_undo brings it back
              if (argBool(a, "clearFirst", false)) sieda_pcb_clear_routing(p);
              sieda_pcb_autoplace(p, 0);
              // Over MCP a pour pad's fan-out via clears its SMD pad (no via in pad) unless options say otherwise;
              // the board's saved strategy keeps its own setting.
              const bool offPads = p->project.pcb.settings.autorouter.viasOffPads;
              p->project.pcb.settings.autorouter.viasOffPads = a.get("options").get("viasOffPads").asBool(true);
              p->project.pcb.settings.autorouter.widerPairSearch = true;  // a USB pair is worth a longer search
              // A whole-board route with no pour yet pours the ground net on the bottom layer first: a plane, not tracks.
              std::string poured;
              int topPour = -1;
              if (p->project.pcb.zones.empty() && !ar.scoped() && !ar.fanoutOnly && argBool(a, "pour", true))
                  for (const auto& n : p->project.schematic.nets())
                      if (p->project.schematic.netRole(n.index) == NetRole::Ground && !n.name.empty()) {
                          // On two layers the top too: a module's ground pads join it directly, not each by a via.
                          const int layers = p->project.pcb.settings.layerCount;
                          if (sieda_pcb_add_zone(p, n.name.c_str(), layers - 1, 0, 0) >= 0)
                              poured = n.name + " poured on the bottom layer before routing (pour: false skips; pcb_remove_pour removes it)";
                          if (layers == 2 && (topPour = sieda_pcb_add_zone(p, n.name.c_str(), 0, 0, 0)) >= 0)
                              poured = n.name + " poured on both layers before routing (pour: false skips; pcb_remove_pour removes them)";
                          break;
                      }
              // A track keep-out under a module stays clear of the pads a track must still reach (no pour of their net there).
              {
                  auto& kos = p->project.pcb.settings.keepouts;
                  for (auto& k : kos)
                      if (k.name.rfind("under ", 0) == 0) carveUnder(p->project, k, true);
                  kos.erase(std::remove_if(kos.begin(), kos.end(), [](const RouteKeepout& k) {
                                return k.name.rfind("under ", 0) == 0 && (k.area.width() <= 1 || k.area.height() <= 1);
                            }),
                            kos.end());
              }
              // Unpoured power / ground nets with no width of their own route 0.4 mm wide (a connector-fed board gives
              // the current sizing no source to measure); again at the board's width if that leaves anything open. A net
              // with a pad narrower than 0.4 mm + clearance (a fine-pitch pin) is widened too, necked down at that pad, unless
              // that leaves more open than widening only the others.
              std::vector<std::string> widened, fineNets;
              if (!ar.scoped() && !ar.fanoutOnly && p->project.pcb.settings.trackWidth < 0.4 && argBool(a, "powerWidths", true)) {
                  std::set<int> fine;
                  for (const auto& pad : p->project.pcb.pads(p->project.schematic))
                      if (std::min(pad.size.x, pad.size.y) < 0.4 + p->project.pcb.settings.clearance - 1e-9) fine.insert(pad.net);
                  for (const auto& n : p->project.schematic.nets())
                      if (p->project.schematic.netRole(n.index) != NetRole::Signal && !n.name.empty() &&
                          !p->project.pcb.settings.netWidths.count(n.name) && !p->project.pcb.isZoneNet(p->project.schematic, n.index))
                          (fine.count(n.index) ? fineNets : widened).push_back(n.name);
              }
              // A route with coupled pairs pairs X_DP / X_DM (USB) nets by a directive, as a designer would.
              Json pairedByName = Json::array();
              if (ar.coupledPairs && !ar.scoped() && !ar.fanoutOnly) {
                  std::set<int> paired;
                  for (const auto& [pn, nn] : differentialPairs(p->project.schematic)) paired.insert(pn), paired.insert(nn);
                  std::map<std::string, const Net*> byUpper;
                  for (const auto& n : p->project.schematic.nets()) {
                      std::string u = n.name;
                      for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                      byUpper[u] = &n;
                  }
                  std::vector<std::pair<PinRef, std::string>> add;
                  for (const auto& [u, n] : byUpper)
                      if (u.size() > 3 && u.compare(u.size() - 3, 3, "_DP") == 0 && byUpper.count(u.substr(0, u.size() - 3) + "_DM") && !paired.count(n->index))
                          for (const auto& pin : n->pins)
                              if (const Component* c = p->project.schematic.find(pin.component); c && !isNetSymbolKind(c->kind)) {
                                  add.push_back({pin, n->name + " / " + byUpper[u.substr(0, u.size() - 3) + "_DM"]->name});
                                  break;
                              }
                  for (const auto& [pin, name] : add) {
                      Json d = obj();
                      d["component"] = pin.component;
                      d["pin"] = pin.pin;
                      d["diffPair"] = true;
                      if (sieda_add_directive(p, d.dump().c_str()) >= 0) pairedByName.push(name);
                  }
              }
              const Project unrouted = p->project;
              // Tried widest first (every power net, then those without a fine pin, then none); the first with the fewest
              // connections open goes on to the (costly) completion, once.
              std::vector<std::vector<std::string>> tiers;
              if (!fineNets.empty()) tiers.push_back(widened), tiers.back().insert(tiers.back().end(), fineNets.begin(), fineNets.end());
              if (!widened.empty()) tiers.push_back(widened);
              tiers.push_back({});
              Json stats;
              size_t bestOpen = SIZE_MAX;
              Project best;
              for (const auto& tier : tiers) {
                  p->project = unrouted;
                  p->router.reset();
                  for (const auto& n : tier) p->project.pcb.settings.netWidths[n] = 0.4;
                  Json s = takeJson(sieda_pcb_autoroute(p));
                  if (const size_t open = p->project.pcb.ratsnest(p->project.schematic).size(); open < bestOpen)
                      bestOpen = open, best = std::move(p->project), stats = s, widened = tier;
                  if (bestOpen == 0) break;
              }
              p->project = std::move(best);
              p->router.reset();
              Json j = obj();
              Json repairs = finishRouting(p, &stats);
              // A pair the top pour left uncoupled: routed again with the bottom pour only, kept if more pairs couple.
              auto uncoupledPairs = [&] {
                  int n = 0;
                  const Json report = takeJson(sieda_pcb_route_report(p));
                  for (const auto& pr : report.get("pairs").items()) n += !pr.get("coupled").asBool(false);
                  return n;
              };
              if (const int was = topPour >= 0 ? uncoupledPairs() : 0; was > 0) {
                  const Project withTop = p->project;
                  const Json statsTop = stats, repairsTop = repairs;
                  p->project = unrouted;
                  p->project.pcb.zones.erase(p->project.pcb.zones.begin() + topPour);
                  for (const auto& n : widened) p->project.pcb.settings.netWidths[n] = 0.4;
                  p->router.reset();
                  stats = takeJson(sieda_pcb_autoroute(p));
                  repairs = finishRouting(p, &stats);
                  if (uncoupledPairs() < was && p->project.pcb.ratsnest(p->project.schematic).size() <= withTop.pcb.ratsnest(withTop.schematic).size())
                      poured = poured.substr(0, poured.find(" poured")) + " poured on the bottom layer before routing (a top pour left a "
                               "differential pair uncoupled; pour: false skips; pcb_remove_pour removes it)";
                  else
                      p->project = withTop, p->router.reset(), stats = statsTop, repairs = repairsTop;
              }
              if (!widened.empty()) {
                  std::string list;
                  for (const auto& n : widened) list += (list.empty() ? "" : ", ") + n;
                  j["powerWidths"] = list + " routed 0.4 mm wide (power / ground; pcb_set_net_width changes it, powerWidths: false skips it)";
              }
              if (!a.get("options").has("viasOffPads")) p->project.pcb.settings.autorouter.viasOffPads = offPads;
              p->project.pcb.settings.autorouter.widerPairSearch = false;
              if (repairs.size()) j["repairs"] = repairs;
              // No acute joins left by gloss / via minimisation (a branch meeting its net's track at 45° joins square).
              if (!p->project.pcb.settings.autorouter.scoped()) {
                  collapseJogs(p->project);
                  routequality::fixAcuteJoins(p->project.pcb, p->project.schematic);
                  squareEscapes(p->project);
                  if (const int off = viasOffPads(p->project)) j["viasMovedOffPads"] = off;
                  if (const int stubs = trimStubs(p->project)) j["stubsTrimmed"] = stubs;
              }
              if (const Json necked = neckTombstones(p->project); necked.size()) j["tombstoningPrevented"] = necked;
              if (reroute) j["undo"] = "project_undo puts back the routing from before this call (compare and keep the better)";
              // A partial route (some nets, an area, fanout only) is for this call: the board keeps its strategy.
              j["options"] = takeJson(sieda_pcb_autoroute_options(p));  // as routed
              const AutorouteOptions used = p->project.pcb.settings.autorouter;
              if (!argBool(a, "keep", false) || used.scoped() || used.fanoutOnly) p->project.pcb.settings.autorouter = previous;
              const auto open = p->project.pcb.ratsnest(p->project.schematic);
              {  // a net the grid left open but a pour or a later repair joined is not failed
                  std::set<std::string> openNets;
                  const auto pads = p->project.pcb.pads(p->project.schematic);
                  for (const auto& [ea, eb] : open)
                      for (const auto& pad : pads)
                          if (pad.net >= 0 && std::hypot(pad.position.x - ea.x, pad.position.y - ea.y) < 1e-3)
                              openNets.insert(p->project.schematic.nets()[static_cast<size_t>(pad.net)].name);
                  Json still = Json::array(), joined = Json::array();
                  for (const auto& n : stats.get("failedNets").items()) (openNets.count(n.asString("")) ? still : joined).push(n);
                  if (joined.size()) {
                      stats["failedNets"] = still;
                      stats["joinedLater"] = joined;
                      if (stats.get("failed").asInt(0) > static_cast<int>(open.size())) stats["failed"] = open.size();
                  }
              }
              j["stats"] = stats;
              j["unroutedConnections"] = open.size();
              if (!open.empty()) {  // what to do about the connections left open
                  Json hints = Json::array();
                  std::set<std::string> said;
                  const auto pads = p->project.pcb.pads(p->project.schematic);
                  for (const auto& [ea, eb] : open)
                      for (Vec2 end : {ea, eb})
                          for (const auto& pad : pads) {
                              if (std::hypot(pad.position.x - end.x, pad.position.y - end.y) > 1e-3) continue;
                              // An exposed pad: an SMD pad larger than every other pad of its part (not one of a
                              // through-hole part's equal pads).
                              double others = 0;
                              for (const auto& q : pads)
                                  if (q.componentId == pad.componentId && &q != &pad) others = std::max(others, q.size.x * q.size.y);
                              const Component* c = p->project.schematic.find(pad.componentId);
                              const double area = pad.size.x * pad.size.y;
                              if (c && !pad.throughHole && area > others * 1.5 && area > 4 && said.insert(c->ref).second)
                                  hints.push(c->ref + "'s exposed pad: pcb_add_thermal_vias {\"ref\":\"" + c->ref +
                                             "\"} and a pour of its net on the bottom (pcb_add_pour) connect it.");
                          }
                  hints.push("A finer rule preset (pcb_set_board rulePreset \"Fab House Advanced (4/4 mil)\"), more "
                             "layers or a larger board gives the router room; pcb_drc lists each open connection.");
                  j["hints"] = hints;
              }
              const Json report = takeJson(sieda_pcb_route_report(p));
              if (report.isObject()) j["metrics"] = report.get("metrics");
              const auto now = stitchingVias(p->project);
              int restitched = 0;
              for (const auto& [net, at] : stitched) {  // the pitch they had: their nearest neighbour's distance
                  if (now.count(net) && now.at(net).size() >= at.size()) continue;
                  double pitch = 1e9;
                  for (Vec2 a1 : at)
                      for (Vec2 b1 : at)
                          if ((a1 - b1).length() > 1e-6) pitch = std::min(pitch, (a1 - b1).length());
                  Json o = obj();
                  o["net"] = p->project.schematic.nets()[static_cast<size_t>(net)].name;
                  o["pitch"] = pitch < 1e8 ? std::round(pitch * 10) / 10 : 5.0;
                  restitched += stitchVias(p, o).get("added").asInt(0);
              }
              {  // a pour left with no copper (nothing of its net on that layer) joins the net through stitching vias
                  std::set<int> floating;
                  for (const auto& f : p->project.pcb.zoneFills(p->project.schematic))
                      if (f.net >= 0 && f.islands == 0 && !f.removedIslands.empty()) floating.insert(f.net);
                  for (int net : floating) {
                      Json o = obj();
                      o["net"] = p->project.schematic.nets()[static_cast<size_t>(net)].name;
                      o["pitch"] = 5.0;
                      restitched += stitchVias(p, o).get("added").asInt(0);
                  }
              }
              if (restitched) j["stitchingRestored"] = restitched;
              // stats.vias counts the router's own; the board also has fan-out vias into pours and stitching
              if (static_cast<int>(p->project.pcb.vias.size()) != stats.get("vias").asInt(0))
                  j["viasOnBoard"] = static_cast<int>(p->project.pcb.vias.size());
              Json uncoupled = Json::array(), coupled = Json::array();  // a differential pair routed as two single nets, and why
              for (const auto& pr : report.get("pairs").items())
                  if (pr.get("coupled").asBool(false)) coupled.push(pr.get("positive").asString("") + " / " + pr.get("negative").asString(""));
                  else
                      uncoupled.push(pr.get("positive").asString("") + " / " + pr.get("negative").asString("") + ": " +
                                     pr.get("reason").asString("not coupled") + " (routed as two single nets" +
                                     (pr.get("reason").asString("").find("two pads") != std::string::npos
                                          ? ", e.g. a USB-C receptacle's doubled D+ / D− pads: keep the two nets short and side by side)" : ")"));
              std::set<int> paired;  // X_DP / X_DM (USB) pairs only by a directive: say how
              for (const auto& [pn, nn] : differentialPairs(p->project.schematic)) paired.insert(pn), paired.insert(nn);
              std::map<std::string, const Net*> byUpper;
              for (const auto& n : p->project.schematic.nets()) {
                  std::string u = n.name;
                  for (auto& ch : u) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
                  byUpper[u] = &n;
              }
              for (const auto& [u, n] : byUpper)
                  if (u.size() > 3 && u.compare(u.size() - 3, 3, "_DP") == 0 && byUpper.count(u.substr(0, u.size() - 3) + "_DM") && !paired.count(n->index))
                      uncoupled.push(n->name + " / " + byUpper[u.substr(0, u.size() - 3) + "_DM"]->name +
                                     ": not paired by name; schematic_add_directive {\"pin\": a pin on " + n->name +
                                     ", \"diffPair\": true} pairs them, then route again");
              if (uncoupled.size()) j["pairsNotCoupled"] = uncoupled;
              if (coupled.size()) j["pairsCoupled"] = coupled;
              if (stats.get("lengthTuned").asInt(0) > 0)  // the meanders' reason
                  j["lengthTunedWhy"] = coupled.size() ? "serpentines even out a coupled pair's P / N lengths (skew), e.g. " + coupled.items().front().asString("")
                                                       : std::string("serpentines bring nets to their length rules or match groups");
              if (pairedByName.size()) j["pairedByName"] = pairedByName;  // schematic_net_rules lists the directives
              if (!poured.empty()) j["groundPour"] = poured;
              if (p->project.pcb.settings.layerCount == 1 && argBool(a, "jumpers", true) &&
                  !p->project.pcb.ratsnest(p->project.schematic).empty()) {  // single-sided: wire links over what is left
                  const Json links = addJumpers(s, 200);
                  if (links.size()) j["jumpersAdded"] = links;
                  j["openAfterJumpers"] = static_cast<int>(p->project.pcb.ratsnest(p->project.schematic).size());
              }
              return out(j);
          });
    t.add("pcb", "pcb_add_jumpers", "Wire links", Kind::Edit, false,
          "Single-sided boards: joins each net the copper leaves in islands by a wire link (JW1 …, a real part in the "
          "schematic and BOM, both pins labelled with the net): the shortest straight link along x or y, 7.62–50.8 mm "
          "in 2.54 mm steps, from one island's copper to another's with short stub tracks, clear of other nets by their "
          "class clearance, of holes, keep-outs, the edge and courtyards; its body on the bottom (component) side. "
          "pcb_autoroute adds them by itself on a 1-layer board (jumpers: false skips).",
          Schema().integer("max", "Most links to add (default 200)")
              .boolean("fromBottom", "A 2-layer route instead: its bottom copper becomes wire links (one per run of "
                                     "tracks between its end holes; an insulated jumper when the straight line meets a bottom body), the vias go and the board "
                                     "becomes single-sided"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              if (argBool(a, "fromBottom", false)) {
                  check(proj(s).pcb.settings.layerCount == 2, "fromBottom needs a 2-layer board");
                  j["linksFromBottom"] = bottomToLinks(s);
              }
              j["jumpersAdded"] = addJumpers(s, argInt(a, "max", 200));
              j["open"] = static_cast<int>(proj(s).pcb.ratsnest(proj(s).schematic).size());
              return out(j);
          });
    t.add("pcb", "pcb_autoroute_presets", "Autoroute presets", Kind::Read, true,
          "The autorouter strategy presets with their options, and the board's current strategy.", Schema(),
          [](McpServer& s, const Json&) {
              Json j = obj();
              j["presets"] = takeJson(sieda_autoroute_presets());
              j["current"] = takeJson(sieda_pcb_autoroute_options(P(s)));
              return out(j);
          });
    t.add("pcb", "pcb_route_report", "Route report", Kind::Read, true,
          "The last autoroute's report: differential pairs, lengths against their rules, quality metrics.", Schema(),
          [](McpServer& s, const Json&) {
              const PcbLayout& pcb = P(s)->project.pcb;
              if (pcb.lastRouteReport.metrics.segments == 0 && !pcb.tracks.empty()) {  // reopened: measure the copper now
                  RouteReport report = pcb.lastRouteReport;
                  routequality::measure(pcb, P(s)->project.schematic, report.metrics);
                  Json r = routeReportJson(report);
                  r["note"] = "Measured from the board as it is (the route's own pair / length report is not saved with the "
                              "project: pcb_drc and si_length_report check them).";
                  return out(r);
              }
              Json r = takeJson(sieda_pcb_route_report(P(s)));
              if (r.isNull()) throw ToolError("No autoroute has run yet");
              return out(r);
          });
    t.add("pcb", "pcb_clear_routing", "Clear routing", Kind::Destructive, false, "Removes every track and via.", Schema(),
          [](McpServer& s, const Json&) {
              sieda_pcb_clear_routing(P(s));
              return out(projectSummary(s).get("board"));
          });
    t.add("pcb", "pcb_route", "Route connection", Kind::Edit, false,
          "Interactive router in one call: a track from one pin's pad to another's, through optional waypoints "
          "(mm; \"via\": true drops a via there and continues on the other side), pushing other nets aside (mode "
          "shove) or walking around them. Commits only when the target pad is reached.",
          Schema()
              .pin("from", "Start pin (its pad).", true)
              .pin("to", "Target pin (its pad).", true)
              .integer("layer", "Start copper layer (default the start pad's)")
              .num("width", "Track width (mm, 0 = net class)")
              .str("mode", "shove (default), walkaround, highlight, stop", false, {"shove", "walkaround", "highlight", "stop"})
              .str("posture", "45 (default), 90 or free", false, {"45", "90", "free"})
              .array("waypoints", "[{\"x\":10,\"y\":5,\"via\":false}] corners on the way", "object"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const PinAddress from = pinAddress(*p, a.get("from")), to = pinAddress(*p, a.get("to"));
              Vec2 start, end;
              int layer = 0, endLayer = 0;
              if (!padOf(p->project, from, &start, &layer)) throw ToolError(pinLabel(*p, from) + " has no pad on the board (place it first)");
              if (!padOf(p->project, to, &end, &endLayer)) throw ToolError(pinLabel(*p, to) + " has no pad on the board (place it first)");
              if (a.has("layer")) layer = argInt(a, "layer", layer);
              Json o = obj();
              o["mode"] = argStr(a, "mode", "shove");
              o["posture"] = argStr(a, "posture", "45");
              if (a.has("width")) o["width"] = argNum(a, "width", 0);
              sieda_router_cancel(p);
              Json pv = takeJson(sieda_router_begin(p, o.dump().c_str(), start.x, start.y, layer));
              auto fail = [&](const std::string& why) {
                  sieda_router_cancel(p);
                  throw ToolError(why);
              };
              if (!pv.isObject() || pv.has("error")) fail("Cannot start a route at " + pinLabel(*p, from) + ": " + pv.get("error").asString("?"));
              if (a.get("waypoints").isArray())
                  for (const auto& w : a.get("waypoints").items()) {
                      pv = takeJson(sieda_router_move(p, argNum(w, "x", 0), argNum(w, "y", 0)));
                      pv = takeJson(argBool(w, "via", false) ? sieda_router_add_via(p, w.has("layer") ? argInt(w, "layer", -1) : -1)
                                                             : sieda_router_fix(p));
                      if (pv.has("error")) fail("Waypoint failed: " + pv.get("error").asString("?"));
                  }
              pv = takeJson(sieda_router_move(p, end.x, end.y));
              if (!pv.get("reachedTarget").asBool(false) && pv.get("layer").asInt(endLayer) != endLayer) {
                  // The target is an SMD pad on another layer: drop a via towards it and try again.
                  pv = takeJson(sieda_router_add_via(p, endLayer));
                  pv = takeJson(sieda_router_move(p, end.x, end.y));
              }
              if (!pv.get("reachedTarget").asBool(false))
                  fail("The route could not reach " + pinLabel(*p, to) + " (" + pv.get("status").asString("blocked") +
                       "). Add waypoints, change mode, or use pcb_autoroute.");
              const Json done = takeJson(sieda_router_commit(p));
              if (!done.get("ok").asBool(false)) throw ToolError("Commit failed: " + done.get("error").asString("?"));
              Json j = obj();
              j["from"] = pinLabel(*p, from);
              j["to"] = pinLabel(*p, to);
              j["addedTracks"] = done.get("addedTracks");
              j["addedVias"] = done.get("addedVias");
              j["length"] = pv.get("netLength").isNumber() ? pv.get("netLength") : pv.get("length");
              return out(j);
          });
    t.add("pcb", "pcb_route_live", "Route live (push and shove)", Kind::Edit, false,
          "Drag routing step by step, as in an editor: begin at a pad or track end (or drag / pair / via_drag), then "
          "move the head to the cursor (other nets are pushed aside live), fix a corner, via to change layer, and "
          "commit (one undo step) or cancel. Every step returns the preview: head / placed tracks, vias, the tracks and "
          "vias shoved, and the ids hidden while they are. Only commit changes the board.",
          Schema()
              .str("action", "begin, pair, drag (a track), via_drag, move, fix, via, options, commit, cancel", true,
                   {"begin", "pair", "drag", "via_drag", "move", "fix", "via", "options", "commit", "cancel"})
              .num("x", "Point (mm): begin / drag at, move to").num("y", "Point (mm)")
              .integer("layer", "begin: copper layer; via: layer to continue on (default the other side)")
              .integer("id", "drag: track id; via_drag: via id")
              .str("mode", "shove (default), walkaround, highlight", false, {"shove", "walkaround", "highlight"})
              .str("posture", "45 (default), 90 or free", false, {"45", "90", "free"})
              .num("width", "Track width (mm, 0 = net class)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string act = argStr(a, "action", "");
              const double x = argNum(a, "x", 0), y = argNum(a, "y", 0);
              Json o = obj();
              o["mode"] = argStr(a, "mode", "shove");
              o["posture"] = argStr(a, "posture", "45");
              if (a.has("width")) o["width"] = argNum(a, "width", 0);
              const std::string opts = o.dump();
              if (act == "cancel") {
                  sieda_router_cancel(p);
                  return out(obj());
              }
              const bool starts = act == "begin" || act == "pair" || act == "drag" || act == "via_drag";
              if (starts) sieda_router_cancel(p);
              else if (!sieda_router_active(p)) throw ToolError("No route in progress: begin one first");
              Json r = takeJson(act == "begin"      ? sieda_router_begin(p, opts.c_str(), x, y, argInt(a, "layer", 0))
                                : act == "pair"     ? sieda_router_begin_pair(p, opts.c_str(), x, y, argInt(a, "layer", 0))
                                : act == "drag"     ? sieda_router_begin_drag(p, opts.c_str(), argInt(a, "id", -1), x, y)
                                : act == "via_drag" ? sieda_router_begin_via_drag(p, opts.c_str(), argInt(a, "id", -1), x, y)
                                : act == "move"     ? sieda_router_move(p, x, y)
                                : act == "fix"      ? sieda_router_fix(p)
                                : act == "via"      ? sieda_router_add_via(p, argInt(a, "layer", -1))
                                : act == "options"  ? sieda_router_set_options(p, opts.c_str())
                                                    : sieda_router_commit(p));
              if (starts && (!r.isObject() || r.has("error") || !r.get("active").asBool(false))) {
                  sieda_router_cancel(p);
                  throw ToolError("Cannot start there: " + r.get("error").asString("no pad or track of a net at that point"));
              }
              if (act == "commit" && !r.get("ok").asBool(false)) throw ToolError("Not committed: " + r.get("error").asString("?"));
              return out(r);
          });
    t.add("pcb", "pcb_teardrops", "Teardrops", Kind::Edit, false,
          "Adds (or removes) teardrops where tracks meet pads / vias, on the given tracks or all of them.",
          Schema()
              .array("trackIds", "Track ids (default every track)", "integer")
              .boolean("pads", "At pads (default true)")
              .boolean("vias", "At vias (default true)")
              .num("length", "Length as a fraction of the pad size (0.3–3, default 1)")
              .str("style", "straight (default) or curved", false, {"straight", "curved"})
              .boolean("remove", "Remove teardrops instead"),
          [](McpServer& s, const Json& a) {
              const std::string ids = a.get("trackIds").isArray() ? a.get("trackIds").dump() : std::string("[]");
              Json r = takeJson(sieda_pcb_teardrops(P(s), ids.c_str(), optionsFrom(a, {"trackIds"}).c_str()));
              Json j = obj();
              for (const char* k : {"ok", "message", "added", "skipped", "error"})
                  if (r.has(k)) j[k] = r.get(k);
              return out(j);
          });
    t.add("pcb", "pcb_add_pour", "Copper pour", Kind::Edit, false,
          "Adds a copper pour of a net (usually GND) on a layer (0 = top, layers-1 = bottom); plane: true reserves "
          "the layer for the net. Pours fill around other copper at the clearance. Returns the filled area, its islands, "
          "the net's pads it joins and islands that reach no pad, with their centres (stitch them with vias, or keep the "
          "pour out of there, or they float). Tracks into small two-pad parts the pour now outweighs are necked down at "
          "the pad (neckedDown: tombstoning).",
          Schema()
              .str("net", "Net name, e.g. \"GND\"", true)
              .integer("layer", "Copper layer (default 0 = top)")
              .boolean("plane", "Reserve the layer as a plane")
              .num("clearance", "Clearance (mm, default board clearance)")
              .any("polygon", "Optional pour shape: [{x, y}] (mm, 3 or more corners); default the whole board"),
          [](McpServer& s, const Json& a) {
              netIndex(proj(s), requireStr(a, "net"));
              const size_t openBefore = proj(s).pcb.ratsnest(proj(s).schematic).size();
              const int z = a.has("polygon")
                  ? sieda_pcb_add_zone_polygon(P(s), argStr(a, "net").c_str(), argInt(a, "layer", 0), a.get("polygon").dump().c_str())
                  : sieda_pcb_add_zone(P(s), argStr(a, "net").c_str(), argInt(a, "layer", 0), argBool(a, "plane", false) ? 1 : 0,
                                       argNum(a, "clearance", 0));
              if (z >= 0 && a.has("polygon")) {
                  P(s)->project.pcb.zones[static_cast<size_t>(z)].plane = argBool(a, "plane", false);
                  P(s)->project.pcb.zones[static_cast<size_t>(z)].clearance = std::max(0.0, argNum(a, "clearance", 0));
              }
              check(z >= 0, "Could not add the pour (layer out of range?)");
              Json j = obj();
              j["zone"] = z;
              // The pour joins its net's pads itself: those connections no longer need a track.
              if (const Json necked = neckTombstones(P(s)->project); necked.size()) j["tombstoningPrevented"] = necked;  // spokes added copper
              if (const size_t now = proj(s).pcb.ratsnest(proj(s).schematic).size(); now != openBefore)
                  j["unroutedConnections"] = std::to_string(openBefore) + " → " + std::to_string(now) + " (the pour joins " +
                                             requireStr(a, "net") + " pads, so those connections need no track)";
              const Project& pr = proj(s);
              for (const auto& f : pr.pcb.zoneFills(pr.schematic)) {  // what it filled and joined
                  if (f.zone != z) continue;
                  std::set<int> fed;
                  int pads = 0, joined = 0;
                  for (const auto& pad : pr.pcb.pads(pr.schematic))
                      if (pad.net == f.net && pad.onLayer(f.layer)) {
                          ++pads;
                          if (const int i = f.islandNear(pad.position, std::min(pad.size.x, pad.size.y) / 2); i >= 0) ++joined, fed.insert(i);
                      }
                  j["areaMm2"] = std::round(f.area() * 10) / 10;
                  j["islands"] = f.islands;
                  j["padsJoined"] = std::to_string(joined) + " of " + std::to_string(pads) + " " + requireStr(a, "net") +
                                    " pads on this layer";
                  j["islandsWithoutPads"] = f.islands - static_cast<int>(fed.size());  // 0: every island reaches a pad
                  if (f.islands > static_cast<int>(fed.size())) {  // stitch them or they float: where each one is
                      std::map<int, std::pair<Vec2, int>> sums;
                      for (int r = 0; r < f.rows; ++r)
                          for (int q = 0; q < f.cols; ++q)
                              if (const int i = f.island[static_cast<size_t>(r * f.cols + q)]; i >= 0 && !fed.count(i)) {
                                  auto& [sum, n] = sums[i];
                                  sum = sum + Vec2{(q + 0.5) * f.cell, (r + 0.5) * f.cell}, ++n;
                              }
                      Json at = Json::array();
                      for (const auto& [i, sn] : sums) {
                          Json xy = Json::array();
                          xy.push(std::round(sn.first.x / sn.second * 10) / 10);
                          xy.push(std::round(sn.first.y / sn.second * 10) / 10);
                          at.push(xy);
                      }
                      j["islandsWithoutPadsAt"] = at;
                  }
              }
              return out(j);
          });
    t.add("pcb", "pcb_remove_pour", "Remove pours", Kind::Destructive, false,
          "Removes one copper pour by index (pcb_summary zones) or all of them.",
          Schema().integer("index", "Pour index").boolean("all", "Remove every pour"), [](McpServer& s, const Json& a) {
              if (argBool(a, "all", false)) sieda_pcb_clear_zones(P(s));
              else check(sieda_pcb_remove_zone(P(s), argInt(a, "index", -1)) == 1, "Unknown pour index");
              Json j = obj();
              j["ok"] = true;
              return out(j);
          });
    t.add("pcb", "pcb_cutouts", "Board cutouts", Kind::Edit, false,
          "Board cutouts (closed shapes routed out of the board; copper, parts and pours keep the edge clearance from "
          "them): add one through points [{x, y}, …] (mm, 3 or more) or remove one by index (pcb_summary cutouts).",
          Schema().str("action", "add or remove", true, {"add", "remove"})
              .array("points", "add: [{x, y}, …] (mm)", "object").integer("index", "remove: cutout index"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              if (argStr(a, "action", "") == "add") {
                  check(a.get("points").size() >= 3 && sieda_pcb_add_cutout(P(s), a.get("points").dump().c_str()) == 1,
                        "A cutout needs 3 or more points inside the board");
                  j["index"] = static_cast<int>(P(s)->project.pcb.settings.cutouts.size()) - 1;
              } else {
                  check(sieda_pcb_remove_cutout(P(s), argInt(a, "index", -1)) == 1, "No cutout " + std::to_string(argInt(a, "index", -1)));
                  j["removed"] = true;
              }
              j["cutouts"] = static_cast<int>(P(s)->project.pcb.settings.cutouts.size());
              return out(j);
          });
    t.add("pcb", "pcb_add_mounting_hole", "Mounting hole", Kind::Edit, false,
          "Adds a non-plated mounting hole with a keep-out (default M3: 3.2 mm drill). clear: true removes all.",
          Schema()
              .num("x", "Centre x (mm)")
              .num("y", "Centre y (mm)")
              .num("drill", "Drill (mm, default 3.2)")
              .num("keepout", "Keep-out diameter (mm, default 2 × drill)")
              .boolean("clear", "Remove every mounting hole instead"),
          [](McpServer& s, const Json& a) {
              Json j = obj();
              if (argBool(a, "clear", false)) {
                  sieda_pcb_clear_mounting_holes(P(s));
                  j["holes"] = 0;
              } else {
                  if (!a.has("x") || !a.has("y")) throw ToolError("x and y are required");
                  j["holes"] = sieda_pcb_add_mounting_hole(P(s), argNum(a, "x", 0), argNum(a, "y", 0), argNum(a, "drill", 3.2),
                                                           argNum(a, "keepout", 0));
              }
              return out(j);
          });
    t.add("pcb", "pcb_set_keepouts", "Routing keep-outs", Kind::Edit, true,
          "Replaces the routing keep-outs: [{name, x0, y0, x1, y1, layer (-1 = all), tracks, vias, pours}] (mm). "
          "pours: true keeps copper pours out as well (the area under an antenna).",
          Schema().array("keepouts", "Keep-out rectangles (mm)", "object", true), [](McpServer& s, const Json& a) {
              Json j = obj();
              j["kept"] = sieda_pcb_set_keepouts(P(s), a.get("keepouts").dump().c_str());
              return out(j);
          });
    t.add("pcb", "pcb_drc", "Design rule check", Kind::Read, true,
          "Runs the DRC (clearances, widths, drills, annular rings, unrouted nets, board edge) and the "
          "design-for-reliability checks, and flags a part whose courtyard leaves the board or overlaps another part, or "
          "whose pads stand in an antenna / barrier keep-out (DRC_PLACE_*, a forced pose) or stands closer to one than the assembly spacing standard (DRC_PART_SPACING: "
          "courtyard gaps of 0.25 mm passive-passive, 0.5 passive-IC, 1.0 IC-IC and through-hole-SMD, 0.5 between "
          "through-hole parts, 2.0 round a BGA). Returns counts and the violations (errors first, mm coordinates); "
          "unrouted lists up to 20 open connections, each with cutOff = the pads in the piece of copper cut off from "
          "the rest of its net (the end to fix).",
          Schema(), [](McpServer& s, const Json&) {
              Json all = takeJson(sieda_pcb_run_drc(P(s)));
              const Project& pr = proj(s);
              // A part whose courtyard leaves the board, overlaps another part's or sits on a hole or keep-out
              // (a forced pose), as the placement check sees it.
              for (const auto& c : pr.schematic.components()) {
                  if (!c.hasFootprint() || !c.pcb.placed || isNetSymbolKind(c.kind) || !pr.pcb.onBoard(c)) continue;
                  for (const auto& issue : pr.checkPlacement(c.id, c.pcb.position, c.pcb.rotation, c.pcb.bottom, 0).issues) {
                      if (!issue.error || (issue.code != "PLACE_OUTSIDE" && issue.code != "PLACE_OVERLAP" && issue.code != "PLACE_HOLE" &&
                                          issue.code != "PLACE_KEEPOUT") ||
                          (issue.code == "PLACE_OVERLAP" && issue.other < c.id))  // each pair once
                          continue;
                      Json v = obj(), refs = Json::array();
                      v["code"] = "DRC_" + issue.code;
                      std::string msg = issue.message;  // it stands there already
                      for (const auto& [from, to] : {std::pair<std::string, std::string>{"would extend", "extends"},
                                                     {"would overlap", "overlaps"}})
                          if (const size_t at = msg.find(from); at != std::string::npos) msg.replace(at, from.size(), to);
                      if (const size_t at = msg.find("would lie"); at != std::string::npos) msg.replace(at, 9, "lie");
                      v["message"] = msg;
                      v["severity"] = "warning";
                      refs.push(c.ref);
                      v["refs"] = refs;
                      v["x"] = c.pcb.position.x;
                      v["y"] = c.pcb.position.y;
                      if (all.isArray()) all.push(v);
                  }
              }
              // Parts closer than the assembly spacing standard (standardGap), each pair once; overlaps are above.
              const auto classes = partClasses(pr);
              for (const auto& [id, cls] : classes) {
                  const Component* c = pr.schematic.find(id);
                  if (!c->pcb.placed || !pr.pcb.onBoard(*c)) continue;
                  const Rect cy = pr.pcb.courtyard(*c);
                  for (const auto& [o, g] : crowding(pr, classes, id, cy, c->pcb.bottom)) {
                      const Rect oc = pr.pcb.courtyard(*o);  // touching courtyards count; overlapping ones are DRC_PLACE_OVERLAP
                      if (o->id < id || (std::min(cy.x1, oc.x1) - std::max(cy.x0, oc.x0) > 1e-6 && std::min(cy.y1, oc.y1) - std::max(cy.y0, oc.y0) > 1e-6))
                          continue;
                      Json v = obj(), refs = Json::array();
                      v["code"] = "DRC_PART_SPACING";
                      v["message"] = c->ref + " (" + kPartClassName[static_cast<int>(cls)] + ") and " + o->ref + " (" +
                                     kPartClassName[static_cast<int>(classes.at(o->id))] + ") are " + Json(std::round(g * 100) / 100).dump() +
                                     " mm apart; the assembly standard keeps " +
                                     Json(standardGap(cls, classes.at(o->id))).dump() + " mm between their courtyards";
                      v["severity"] = "warning";
                      refs.push(c->ref);
                      refs.push(o->ref);
                      v["refs"] = refs;
                      v["x"] = (cy.center().x + pr.pcb.courtyard(*o).center().x) / 2;
                      v["y"] = (cy.center().y + pr.pcb.courtyard(*o).center().y) / 2;
                      if (all.isArray()) all.push(v);
                  }
              }
              // A part with an antenna and no pour keep-out over it: copper under the antenna detunes it.
              for (const auto& c : pr.schematic.components()) {
                  if (!c.pcb.placed || !hasAntenna(c) || !pr.pcb.onBoard(c)) continue;
                  bool kept = false;
                  for (const auto& k : pr.pcb.settings.keepouts) kept |= k.pours && k.area.intersects(pr.pcb.courtyard(c));
                  if (kept) continue;
                  std::vector<Pad> pads;
                  for (const auto& pad : pr.pcb.pads(pr.schematic))
                      if (pad.componentId == c.id) pads.push_back(pad);
                  double gap = 0;
                  Rect area;
                  const int end = openEnd(pr.pcb.courtyard(c), pads, &gap, &area);
                  area = antennaKeepout(area, end, pr.pcb.settings);
                  char box[200];
                  std::snprintf(box, sizeof box, "{\"x0\":%.2f,\"y0\":%.2f,\"x1\":%.2f,\"y1\":%.2f}", area.x0, area.y0, area.x1, area.y1);
                  Json v = obj(), refs = Json::array();
                  v["code"] = "DRC_ANTENNA_KEEPOUT";
                  v["message"] = c.ref + " has an antenna: keep copper out from under it (pcb_set_keepouts with tracks, "
                                 "vias and pours true over its antenna end, " + std::string(box) +
                                 " where it stands now; pcb_autoplace all puts it at the top edge with that keep-out).";

                  v["severity"] = "info";
                  refs.push(c.ref);
                  v["refs"] = refs;
                  v["x"] = c.pcb.position.x;
                  v["y"] = c.pcb.position.y;
                  if (all.isArray()) all.push(v);
              }
              Json j = summariseViolations(*P(s), all);
              // Each unrouted connection by its pins and net, so it can be routed or looked at.
              const auto pads = pr.pcb.pads(pr.schematic);
              auto pinAt = [&](Vec2 v) -> const Pad* {
                  for (const auto& pad : pads)
                      if (std::hypot(pad.position.x - v.x, pad.position.y - v.y) < 1e-3) return &pad;
                  return nullptr;
              };
              Json open = Json::array();
              const auto ratsnest = pr.pcb.ratsnest(pr.schematic);
              const auto groups = ratsnest.empty() ? std::vector<size_t>{} : pr.pcb.padGroups(pr.schematic);
              for (const auto& [a, b] : ratsnest) {
                  const Pad *pa = pinAt(a), *pb = pinAt(b);
                  if (!pa || !pb || open.size() >= 20) continue;
                  Json u = obj();
                  u["from"] = pinLabel(*P(s), {pa->componentId, pa->pinIndex});
                  u["to"] = pinLabel(*P(s), {pb->componentId, pb->pinIndex});
                  // The pads cut off with `to` (its piece of copper): the end to look at.
                  Json cut = Json::array();
                  const size_t g = groups[static_cast<size_t>(pb - pads.data())];
                  for (size_t i = 0; i < pads.size() && cut.size() < 8; ++i)
                      if (groups[i] == g && pads[i].net == pb->net) cut.push(pinLabel(*P(s), {pads[i].componentId, pads[i].pinIndex}));
                  u["cutOff"] = cut;
                  const int net = pr.schematic.netOf({pa->componentId, pa->pinIndex});
                  if (net >= 0) u["net"] = pr.schematic.nets()[static_cast<size_t>(net)].name;
                  Json at = Json::array();  // the two ends (mm): one pin may have two pads (a switch)
                  for (Vec2 v : {a, b}) {
                      Json xy = Json::array();
                      xy.push(v.x);
                      xy.push(v.y);
                      at.push(xy);
                  }
                  u["at"] = at;
                  open.push(u);
              }
              if (open.size()) j["unrouted"] = open;
              if (ratsnest.size() > open.size()) j["unroutedNotListed"] = static_cast<int>(ratsnest.size() - open.size());
              return out(j);
          });
    t.add("pcb", "pcb_fanout", "Fanout", Kind::Edit, false,
          "Fans out a component: an escape track and a via on every SMD pad whose net has other pins.",
          Schema()
              .str("ref", "Designator", true)
              .boolean("shove", "Push other copper aside")
              .boolean("onlyUnrouted", "Only pads without copper")
              .num("distance", "Escape length (mm)")
              .str("viaType", "through, blind, micro or auto", false, {"through", "blind", "micro", "auto"}),
          [](McpServer& s, const Json& a) {
              return out(takeJson(sieda_pcb_fanout(P(s), componentId(*P(s), a.get("ref")), optionsFrom(a, {"ref"}).c_str())));
          });
    t.add("pcb", "pcb_stitch_vias", "Via stitching", Kind::Edit, false,
          "Stitches vias where a net's pours overlap on two or more layers (default the ground net, 5 mm pitch), "
          "optionally inside an area; none under a part's courtyard.",
          Schema().str("net", "Net (default ground)").num("pitch", "Pitch (mm, default 5)").num("x0", "Area x0").num("y0", "Area y0").num("x1", "Area x1").num("y1", "Area y1"),
          [](McpServer& s, const Json& a) { return out(stitchVias(P(s), a.isObject() ? a : obj())); });
    t.add("pcb", "pcb_add_via", "Add via", Kind::Edit, false,
          "Drops a via of a net at a point (mm), e.g. to tie a cut-off pad's piece of copper to a pour on another layer. "
          "Refused when it would touch copper of another net or stand off the board. Returns its id; connect it with "
          "pcb_add_track or pcb_route.",
          Schema().str("net", "Net name", true).num("x", "x (mm)", true).num("y", "y (mm)", true)
              .num("diameter", "Pad diameter (mm, default the board's via)").num("drill", "Drill (mm, default the board's via)"),
          [](McpServer& s, const Json& a) {
              Project& pr = P(s)->project;
              Via v;
              v.net = netIndex(pr, requireStr(a, "net"));
              v.position = {argNum(a, "x", 0), argNum(a, "y", 0)};
              v.diameter = argNum(a, "diameter", pr.pcb.settings.viaDiameter);
              v.drill = argNum(a, "drill", pr.pcb.settings.viaDrill);
              if (!(v.drill > 0 && v.diameter > v.drill)) throw ToolError("The via needs diameter > drill > 0");
              if (!pr.pcb.settings.contains(v.position)) throw ToolError("That point is off the board");
              checkClearOfOtherNets(pr, v.net, v.position, v.position, v.diameter, -1);
              Json j = obj();
              j["id"] = pr.pcb.addVia(v);
              return out(j);
          });
    t.add("pcb", "pcb_add_track", "Add track", Kind::Edit, false,
          "Draws a track of a net through points (mm) on one copper layer, ends anywhere (a pad, a via, a pour, another "
          "track) — for repairs the router's pad-to-pad pcb_route cannot make. Refused when a segment would touch copper "
          "of another net. Returns the new track ids.",
          Schema().str("net", "Net name", true).integer("layer", "Copper layer (0 = top)", true)
              .array("points", "[[x, y], [x, y], …] at least two", "", true,
                     Json::parse(R"({"type":"array","items":{"type":"number"},"minItems":2,"maxItems":2})")).num("width", "Width (mm, default the net's)"),
          [](McpServer& s, const Json& a) {
              Project& pr = P(s)->project;
              const int net = netIndex(pr, requireStr(a, "net")), layer = argInt(a, "layer", 0);
              if (layer < 0 || layer >= pr.pcb.settings.layerCount) throw ToolError("No copper layer " + std::to_string(layer));
              std::vector<Vec2> pts;
              for (const auto& q : a.get("points").items()) {
                  double xy[2];
                  for (size_t k = 0; k < 2; ++k) {  // numbers (or numeric strings); anything else is refused, never read as 0
                      const Json& v = q.isArray() && q.size() == 2 ? q[k] : Json();
                      char* end = nullptr;
                      const std::string text = v.isString() ? v.asString() : "";
                      xy[k] = v.isNumber() ? v.asNumber(0) : std::strtod(text.c_str(), &end);
                      if (!v.isNumber() && (text.empty() || *end)) throw ToolError("Each point is [x, y] in mm, e.g. [20.8, 24]");
                  }
                  pts.push_back({xy[0], xy[1]});
              }
              if (pts.size() < 2) throw ToolError("points needs at least two [x, y]");
              const double width = argNum(a, "width", pr.pcb.settings.widthFor(pr.schematic.nets()[static_cast<size_t>(net)].name));
              for (size_t i = 1; i < pts.size(); ++i) {
                  if (!pr.pcb.settings.contains(pts[i - 1]) || !pr.pcb.settings.contains(pts[i])) throw ToolError("A point is off the board");
                  checkClearOfOtherNets(pr, net, pts[i - 1], pts[i], width, layer);
              }
              Json ids = Json::array();
              for (size_t i = 1; i < pts.size(); ++i) {
                  Track t;
                  t.net = net, t.layer = layer, t.width = width, t.a = pts[i - 1], t.b = pts[i];
                  ids.push(pr.pcb.addTrack(t));
              }
              Json j = obj();
              j["ids"] = ids;
              return out(j);
          });
    t.add("pcb", "pcb_length_rules", "Length rules", Kind::Edit, false,
          "Length / phase matching: enable serpentine tuning and tolerances, set per-net length rules "
          "[{net, target, tolerance}] (target 0 removes), match groups [{name, nets:[…], tolerance}], and tune now.",
          Schema()
              .boolean("enabled", "Tune lengths after Auto Route")
              .num("pairSkew", "Intra-pair skew tolerance (mm)")
              .num("busTolerance", "Bus length tolerance (mm)")
              .array("rules", "[{net, target, tolerance}] mm, or [{net, delayPs, tolerancePs}] as a delay", "object")
              .array("groups", "[{name, nets, tolerance}]", "object")
              .boolean("tune", "Add serpentines now"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const auto& st = p->project.pcb.settings;
              if (a.has("enabled") || a.has("pairSkew") || a.has("busTolerance"))
                  sieda_pcb_set_length_matching(p, argBool(a, "enabled", st.lengthTuning) ? 1 : 0, argNum(a, "pairSkew", st.pairSkewTolerance),
                                                argNum(a, "busTolerance", st.busLengthTolerance));
              if (a.get("rules").isArray())
                  for (const auto& r : a.get("rules").items())
                      check(r.has("delayPs") ? sieda_pcb_set_delay_rule(p, requireStr(r, "net").c_str(), argNum(r, "delayPs", 0),
                                                                         argNum(r, "tolerancePs", 5)) == 1
                                             : sieda_pcb_set_length_rule(p, requireStr(r, "net").c_str(), argNum(r, "target", 0),
                                                                         argNum(r, "tolerance", 0.5)) == 1,
                            "Invalid length rule");
              if (a.get("groups").isArray())
                  for (const auto& g : a.get("groups").items()) check(sieda_pcb_set_match_group(p, g.dump().c_str()) == 1, "Invalid match group");
              Json j = obj();
              if (argBool(a, "tune", false)) j["netsTuned"] = sieda_pcb_tune_lengths(p);
              j["targets"] = takeJson(sieda_length_targets_json(p));
              return out(j);
          });
    t.add("pcb", "pcb_pair_rules", "Pair rules", Kind::Edit, false,
          "Differential pair rules by the pair's stem (USB_D for USB_D_P / USB_D_N): an interface preset (usb2, usb3, "
          "pcie, ethernet, hdmi, lvds, ddr, can: typical impedance and skew) and / or impedance (Ω), width, gap (mm) "
          "and skew tolerance (mm); a rule with no values removes it. The routers use the pair's impedance, width and "
          "gap; length tuning and DRC its skew. Returns the board's pairs, their rules and the presets.",
          Schema().array("rules", "[{pair, preset, impedance, width, gap, skew}]", "object"), [](McpServer& s, const Json& a) {
              std::vector<std::string> stems;  // the board's pairs; a rule names one (or the start of just one)
              for (auto [pn, nn] : differentialPairs(proj(s).schematic))
                  stems.push_back(pairStem(proj(s).schematic.nets()[static_cast<size_t>(pn)].name, proj(s).schematic.nets()[static_cast<size_t>(nn)].name));
              for (Json r : a.get("rules").items()) {
                  const std::string want = r.get("pair").asString("");
                  if (std::find(stems.begin(), stems.end(), want) == stems.end()) {
                      std::vector<std::string> starts;
                      for (const auto& st : stems)
                          if (st.rfind(want, 0) == 0) starts.push_back(st);
                      std::string list;
                      for (const auto& st : stems) list += (list.empty() ? "" : ", ") + st;
                      if (starts.size() != 1)
                          throw ToolError("No differential pair \"" + want + "\" on this board (its pairs: " + (list.empty() ? "none" : list) +
                                          "; pair nets X_P / X_N, or schematic_add_directive diffPair)");
                      r["pair"] = starts[0];
                  }
                  check(sieda_pcb_set_pair_rule(P(s), r.dump().c_str()) == 1, "Invalid pair rule");
              }
              const Project& pr = proj(s);
              Json j = obj(), pairs = Json::array();
              for (auto [pn, nn] : differentialPairs(pr.schematic)) {
                  const auto& nets = pr.schematic.nets();
                  Json e = obj();
                  e["pair"] = pairStem(nets[static_cast<size_t>(pn)].name, nets[static_cast<size_t>(nn)].name);
                  if (const PairRule* rule = pr.pcb.settings.pairRule(e.get("pair").asString(""))) {
                      for (auto [k, v] : {std::pair{"impedance", rule->impedance}, {"width", rule->width}, {"gap", rule->gap}, {"skew", rule->skew}})
                          if (v > 0) e[k] = v;
                      if (!rule->preset.empty()) e["preset"] = rule->preset;
                  }
                  pairs.push(e);
              }
              j["pairs"] = pairs;
              j["presets"] = takeJson(sieda_interface_presets_json());
              return out(j);
          });
    t.add("pcb", "pcb_set_hdi", "HDI", Kind::Edit, true,
          "HDI (IPC-2226): blind / buried vias and laser microvias (drill / pad mm), via-in-pad. apply: re-span the "
          "routed board's vias now.",
          Schema()
              .boolean("hdi", "Enable HDI", true)
              .num("microviaDrill", "Microvia drill (mm, default 0.1)")
              .num("microviaDiameter", "Microvia pad (mm, default 0.25)")
              .boolean("viaInPad", "Allow filled vias in pads")
              .boolean("apply", "Apply to the routed board now"),
          [](McpServer& s, const Json& a) {
              check(sieda_pcb_set_hdi(P(s), argBool(a, "hdi", true) ? 1 : 0, argNum(a, "microviaDrill", 0.1), argNum(a, "microviaDiameter", 0.25),
                                      argBool(a, "viaInPad", false) ? 1 : 0) == 1,
                    "Invalid microvia sizes");
              Json j = obj();
              if (argBool(a, "apply", false)) j["viasChanged"] = sieda_pcb_apply_hdi(P(s));
              j["ok"] = true;
              return out(j);
          });
    t.add("pcb", "pcb_remove_copper", "Remove tracks / vias", Kind::Destructive, false,
          "Deletes tracks and vias by id (ids from project_snapshot sections tracks / vias), or every track and via "
          "of a net (net: rip it up, then pcb_autoroute nets [it] routes it again).",
          Schema().array("tracks", "Track ids", "integer").array("vias", "Via ids", "integer").str("net", "Rip up this net"),
          [](McpServer& s, const Json& a) {
              int t = 0, v = 0;
              if (a.has("net")) {
                  const std::string net = argStr(a, "net");
                  Project& pr = P(s)->project;
                  int index = -1;
                  for (const auto& n : pr.schematic.nets())
                      if (n.name == net) index = n.index;
                  check(index >= 0, "No net " + net);
                  const size_t tracks = pr.pcb.tracks.size(), vias = pr.pcb.vias.size();
                  pr.pcb.tracks.erase(std::remove_if(pr.pcb.tracks.begin(), pr.pcb.tracks.end(), [&](const Track& x) { return x.net == index; }),
                                      pr.pcb.tracks.end());
                  pr.pcb.vias.erase(std::remove_if(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& x) { return x.net == index; }),
                                    pr.pcb.vias.end());
                  t += static_cast<int>(tracks - pr.pcb.tracks.size()), v += static_cast<int>(vias - pr.pcb.vias.size());
                  P(s)->router.reset();
              }
              if (a.get("tracks").isArray())
                  for (const auto& id : a.get("tracks").items()) t += sieda_pcb_remove_track(P(s), id.asInt(-1)) == 1;
              if (a.get("vias").isArray())
                  for (const auto& id : a.get("vias").items()) v += sieda_pcb_remove_via(P(s), id.asInt(-1)) == 1;
              Json j = obj();
              j["tracksRemoved"] = t;
              j["viasRemoved"] = v;
              return out(j);
          });
    t.add("pcb", "pcb_edit_copper", "Edit tracks", Kind::Edit, false,
          "Edits routed copper by id (project_snapshot tracks / vias): set width (mm), layer or lock on tracks (each "
          "refused when it would come too close to another net), split a track at a point, tune its net's length with "
          "meanders, merge collinear tracks, "
          "gloss (pull the given tracks, or all, tight) or minimise vias (route the named nets again with fewer vias, "
          "kept only when better).",
          Schema().str("action", "set, split, merge, tune, gloss, minimize_vias", true, {"set", "split", "merge", "tune", "gloss", "minimize_vias"})
              .num("target", "tune: net length (mm; 0 = the longest of its pair / bus group)")
              .array("tracks", "Track ids", "integer").array("nets", "minimize_vias: net names (default every routed net)", "string")
              .num("width", "set: track width (mm)").integer("layer", "set: copper layer").boolean("locked", "set: lock / unlock")
              .num("x", "split: point (mm)").num("y", "split: point (mm)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              Project& pr = p->project;
              const std::string act = argStr(a, "action", "");
              Json ids = a.get("tracks").isArray() ? a.get("tracks") : Json::array(), j = obj();
              auto track = [&](int id) -> Track& {
                  for (auto& t : pr.pcb.tracks)
                      if (t.id == id) return t;
                  throw ToolError("No track " + std::to_string(id));
              };
              if (act == "set") {
                  check(ids.size() > 0, "Give the tracks to change");
                  const double width = argNum(a, "width", 0);
                  const int layer = argInt(a, "layer", -1);
                  check(!a.has("width") || (width >= pr.pcb.settings.minTrackWidth - 1e-9 && width <= 20),
                        "Width must be from the board's minimum track width up to 20 mm");
                  check(!a.has("layer") || (layer >= 0 && layer < pr.pcb.settings.layerCount), "No copper layer " + std::to_string(layer));
                  std::vector<Track> changed;
                  for (const auto& id : ids.items()) {
                      Track t = track(id.asInt(-1));
                      if (a.has("width")) t.width = width;
                      if (a.has("layer")) t.layer = layer;
                      if (a.has("locked")) t.locked = argBool(a, "locked", false);
                      if ((a.has("width") || a.has("layer")) && !t.arc) checkClearOfOtherNets(pr, t.net, t.a, t.b, t.width, t.layer);
                      changed.push_back(t);
                  }
                  for (const Track& t : changed) track(t.id) = t;
                  p->router.reset();
                  j["changed"] = static_cast<int>(changed.size());
              } else if (act == "split") {
                  check(ids.size() == 1, "Give one track to split");
                  const int id = sieda_pcb_split_track(p, ids.items()[0].asInt(-1), argNum(a, "x", 0), argNum(a, "y", 0));
                  check(id >= 0, "The point is not on that track (or too near an end)");
                  j["newTrack"] = id;
              } else if (act == "merge") {
                  j["joins"] = sieda_pcb_merge_tracks(p, ids.size() ? ids.dump().c_str() : nullptr);  // none given: all
              } else if (act == "tune") {
                  check(ids.size() == 1, "Give one track of the net to tune");
                  Json o = obj();
                  o["target"] = argNum(a, "target", 0), o["apply"] = true;
                  j = takeJson(sieda_router_tune(p, ids.items()[0].asInt(-1), o.dump().c_str()));
                  check(j.get("ok").asBool(false), "Not tuned: " + j.get("message").asString("?"));
              } else if (act == "gloss") {
                  if (ids.size() == 0)
                      for (const auto& t : pr.pcb.tracks) ids.push(t.id);
                  j = takeJson(sieda_pcb_gloss(p, ids.dump().c_str(), R"({"apply":true})"));
              } else {
                  Json nets = a.get("nets").isArray() ? a.get("nets") : Json::array();
                  if (nets.size() == 0) {
                      std::set<int> routed;
                      for (const auto& t : pr.pcb.tracks) routed.insert(t.net);
                      for (const auto& n : pr.schematic.nets())
                          if (routed.count(n.index)) nets.push(n.name);
                  }
                  j = takeJson(sieda_pcb_minimize_vias(p, nets.dump().c_str()));
              }
              return out(j);
          });
    t.add("pcb", "pcb_optimize_swaps", "Pin / gate swap", Kind::Edit, false,
          "Swaps interchangeable pins and gates to shorten the ratsnest (back-annotated to the schematic), for one "
          "part or all.",
          Schema().str("ref", "Designator (default every part)").integer("maxSwaps", "Most swaps (default 50)"),
          [](McpServer& s, const Json& a) {
              const int id = a.has("ref") ? componentId(*P(s), a.get("ref")) : -1;
              const int n = sieda_optimize_pcb_swaps(P(s), id, argInt(a, "maxSwaps", 50));
              check(n >= 0, "Swap optimisation failed");
              Json j = obj();
              j["swaps"] = n;
              return out(j);
          });
    t.add("pcb", "pcb_add_thermal_vias", "Thermal vias", Kind::Edit, false,
          "Stitches thermal vias into a power part's largest pad (its own net, clearance kept).",
          Schema().str("ref", "Designator", true), [](McpServer& s, const Json& a) {
              Json j = obj();
              j["viasAdded"] = sieda_pcb_add_thermal_vias(P(s), componentId(*P(s), a.get("ref")));
              return out(j);
          });
}

// ================================================================================================= simulation tools

Json pickNets(const Json& nets, const std::set<std::string>& want, size_t points, const std::vector<std::string>& arrays) {
    Json list = Json::array();
    if (!nets.isArray()) return list;
    for (const auto& n : nets.items()) {
        if (!want.empty() && !want.count(n.get("name").asString(""))) continue;
        Json m = n;
        for (const auto& k : arrays)
            if (m.get(k).isArray()) m[k] = decimate(m.get(k), points);
        list.push(m);
    }
    return list;
}

/// A DC / temperature sweep result trimmed to the asked nets and points (its error becomes a tool error).
Json sweepOut(Json r, const Json& a) {
    if (r.has("error") && !r.get("error").asString("").empty()) throw ToolError(r.get("error").asString(""));
    const size_t points = static_cast<size_t>(std::max(1, argInt(a, "maxPoints", 200)));
    r["values"] = decimate(r.get("values"), points);
    r["nets"] = pickNets(r.get("nets"), nameSet(a, "nets"), points, {"values"});
    Json cur = Json::array();
    for (const auto& c : r.get("currents").items()) {
        Json m = c;
        m["values"] = decimate(c.get("values"), points);
        cur.push(m);
    }
    r["currents"] = cur;
    return r;
}

void simTools(Table& t) {
    t.add("sim", "sim_dc_op", "DC operating point", Kind::Read, true,
          "DC operating point of the schematic (the active variant as assembled): every net voltage and each "
          "part's current, voltage and dissipation.",
          Schema(), [](McpServer& s, const Json&) {
              Json r = takeJson(sieda_simulate_dc(P(s)));
              if (!r.isObject()) throw ToolError("Simulation failed");
              if (r.has("error") && !r.get("converged").asBool(false) && !r.get("error").asString("").empty())
                  throw ToolError("DC analysis failed: " + r.get("error").asString(""));
              Json volts = obj();
              for (const auto& n : r.get("nets").items()) volts[n.get("name").asString("?")] = n.get("voltage");
              Json j = obj();
              j["converged"] = r.get("converged");
              j["netVoltages"] = volts;
              j["devices"] = r.get("devices");
              for (const char* k : {"variant", "omitted"})
                  if (r.has(k)) j[k] = r.get(k);
              return out(j);
          });
    t.add("sim", "sim_transient", "Transient analysis", Kind::Read, true,
          "Time-domain simulation to stop (s or \"10m\") with step. Returns the chosen nets' waveforms (decimated to "
          "maxPoints) and per-net measurements (min, max, average, rms, final, rise / fall time, overshoot, "
          "frequency, duty cycle).",
          Schema()
              .str("stop", "Stop time, e.g. \"10m\"", true)
              .str("step", "Time step, e.g. \"10u\" (default stop/1000)")
              .str("method", "be (default) or trap", false, {"be", "trap"})
              .boolean("adaptive", "Adaptive step (LTE control)")
              .num("temperature", "°C (default 27; imported SPICE models follow it)")
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Samples per waveform (default 200; 0 = measurements only)"),
          [](McpServer& s, const Json& a) {
              const double stop = argNum(a, "stop", 0);
              if (!(stop > 0)) throw ToolError("stop must be a positive time");
              Json o = obj();
              o["stop"] = stop;
              o["step"] = argNum(a, "step", stop / 1000);
              if (a.has("method")) o["method"] = argStr(a, "method");
              if (a.has("adaptive")) o["adaptive"] = argBool(a, "adaptive", false);
              if (a.has("temperature")) o["temperature"] = argNum(a, "temperature", 27);
              Json r = takeJson(sieda_simulate_transient_ex(P(s), o.dump().c_str()));
              if (!r.get("ok").asBool(false)) throw ToolError("Transient failed: " + r.get("error").asString("?"));
              const std::set<std::string> want = nameSet(a, "nets");
              const int points = std::max(0, argInt(a, "maxPoints", 200));
              Json measures = obj();
              Json nets = Json::array();
              for (const auto& n : r.get("nets").items()) {
                  const std::string name = n.get("name").asString("");
                  if (!want.empty() && !want.count(name)) continue;
                  Json req = obj();
                  req["time"] = r.get("time");
                  req["values"] = n.get("values");
                  Json m = takeJson(sieda_measure_waveform(req.dump().c_str()));
                  Json mm = obj();
                  for (const char* k : {"min", "max", "average", "rms", "final", "riseTime", "fallTime", "overshootPercent",
                                        "settlingTime", "frequency", "dutyCycle"})
                      if (m.has(k) && !m.get(k).isNull()) mm[k] = m.get(k);
                  measures[name] = mm;
                  if (points > 0) {
                      Json nj = obj();
                      nj["name"] = name;
                      nj["values"] = decimate(n.get("values"), static_cast<size_t>(points));
                      nets.push(nj);
                  }
              }
              Json j = obj();
              if (points > 0) j["time"] = decimate(r.get("time"), static_cast<size_t>(points));
              j["nets"] = nets;
              j["measurements"] = measures;
              if (r.get("mcus").size()) j["mcus"] = r.get("mcus");
              return out(j);
          });
    t.add("sim", "sim_ac", "AC analysis", Kind::Read, true,
          "Small-signal frequency sweep: magnitude (dB) and phase per net with metrics (low-frequency gain, peak, "
          "f3dB, bandwidth, unity gain, phase margin). at: the gain (dB and V/V) and phase of each net at given "
          "frequencies.",
          Schema()
              .str("start", "Start frequency (default 1)")
              .str("stop", "Stop frequency (default 1MEG)")
              .integer("pointsPerDecade", "Points per decade (default 50)")
              .str("source", "Stimulus source designator (default the AC source rule)")
              .num("temperature", "°C (default 27; imported SPICE models follow it)")
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Points per curve (default 100; 0 = metrics only)")
              .array("at", "Frequencies to read the gain at, e.g. [\"1k\", \"20k\"] (the sweep covers them)", "string"),
          [](McpServer& s, const Json& a) {
              Json sweep = a;
              std::vector<double> at;
              for (const auto& f : a.get("at").items()) {
                  const auto v = f.isNumber() ? std::optional<double>(f.asNumber()) : parseEngineeringValue(f.asString(""));
                  if (!v || !(*v > 0)) throw ToolError("\"at\" frequencies must be positive, e.g. \"1k\"");
                  at.push_back(*v);
              }
              if (!at.empty()) {  // widen the sweep to take in every asked frequency
                  const double lo = *std::min_element(at.begin(), at.end()), hi = *std::max_element(at.begin(), at.end());
                  if (lo < argNum(a, "start", 1)) sweep["start"] = lo / 2;
                  if (hi > argNum(a, "stop", 1e6)) sweep["stop"] = hi * 2;
              }
              Json r = takeJson(sieda_simulate_ac(P(s), optionsFrom(sweep, {"nets", "maxPoints", "at"}).c_str()));
              if (!r.get("ok").asBool(true) || r.has("error")) {
                  if (!r.get("error").asString("").empty()) throw ToolError("AC analysis failed: " + r.get("error").asString(""));
              }
              const int points = std::max(0, argInt(a, "maxPoints", 100));
              Json j = obj();
              j["stimulus"] = r.get("stimulus");
              if (points > 0) j["frequency"] = decimate(r.get("frequency"), static_cast<size_t>(points));
              Json nets = pickNets(r.get("nets"), nameSet(a, "nets"), static_cast<size_t>(std::max(points, 1)), {"magnitudeDb", "phaseDeg"});
              if (points == 0) {
                  Json slim = Json::array();
                  for (const auto& n : nets.items()) {
                      Json m = obj();
                      m["name"] = n.get("name");
                      m["metrics"] = n.get("metrics");
                      slim.push(m);
                  }
                  nets = slim;
              }
              j["nets"] = nets;
              if (!at.empty()) {  // each net's gain and phase at the asked frequencies, interpolated on log f
                  const Json& freq = r.get("frequency");
                  const std::set<std::string> want = nameSet(a, "nets");
                  Json points = Json::array();
                  for (double f : at) {
                      Json pt = obj(), per = obj();
                      pt["frequency"] = f;
                      for (const auto& n : r.get("nets").items()) {
                          const std::string name = n.get("name").asString("");
                          if (!want.empty() && !want.count(name)) continue;
                          const Json &db = n.get("magnitudeDb"), &ph = n.get("phaseDeg");
                          for (size_t i = 0; i + 1 < freq.size() && i + 1 < db.size(); ++i) {
                              const double f0 = freq[i].asNumber(), f1 = freq[i + 1].asNumber();
                              if (f < f0 || f > f1) continue;
                              const double t = f1 > f0 ? std::log(f / f0) / std::log(f1 / f0) : 0;
                              Json g = obj();
                              g["gainDb"] = db[i].asNumber() + t * (db[i + 1].asNumber() - db[i].asNumber());
                              g["gain"] = std::pow(10.0, g.get("gainDb").asNumber() / 20);
                              if (ph.size() > i + 1) g["phaseDeg"] = ph[i].asNumber() + t * (ph[i + 1].asNumber() - ph[i].asNumber());
                              per[name] = g;
                              break;
                          }
                      }
                      pt["nets"] = per;
                      points.push(pt);
                  }
                  j["at"] = points;
              }
              return out(j);
          });
    t.add("sim", "sim_dc_sweep", "DC sweep", Kind::Read, true,
          "Sweeps a source from start to stop in step and returns net voltages and part currents along the sweep.",
          Schema()
              .str("source", "Source designator, e.g. \"V1\"", true)
              .str("start", "Start value", true)
              .str("stop", "Stop value", true)
              .str("step", "Step", true)
              .num("temperature", "°C (default 27; imported SPICE models follow it)")
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Points per curve (default 200)"),
          [](McpServer& s, const Json& a) {
              return out(sweepOut(takeJson(sieda_simulate_dc_sweep(P(s), optionsFrom(a, {"nets", "maxPoints"}).c_str())), a));
          });
    t.add("sim", "sim_temperature_sweep", "Temperature sweep", Kind::Read, true,
          "DC operating point at each temperature (°C): net voltages and part currents per temperature. Imported SPICE "
          "models follow their temperature equations (IS, BF, VTO, TC1 / TC2 …); built-in parts do not move.",
          Schema()
              .array("temperatures", "Temperatures in °C, e.g. [-40, 27, 85, 125]", "number")
              .num("start", "First temperature when no list is given (default -40)")
              .num("stop", "Last temperature (default 125)")
              .num("step", "Temperature step (default 5)")
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Points per curve (default 200)"),
          [](McpServer& s, const Json& a) {
              return out(sweepOut(takeJson(sieda_simulate_temperature_sweep(P(s), optionsFrom(a, {"nets", "maxPoints"}).c_str())), a));
          });
    t.add("sim", "sim_param_sweep", "Parameter sweep", Kind::Read, true,
          "Re-runs an analysis (dc, ac or transient) for each value of one part: {component, values:[…], analysis, "
          "net, AC options or stop / step}.",
          Schema()
              .str("component", "Part designator", true)
              .array("values", "Values, e.g. [\"1k\",\"2k2\",\"4k7\"]", "string", true)
              .str("analysis", "dc, ac or transient", true, {"dc", "ac", "transient"})
              .str("net", "Net to report")
              .str("start", "AC start")
              .str("stop", "AC stop / transient stop")
              .str("step", "Transient step"),
          [](McpServer& s, const Json& a) { return out(takeJson(sieda_simulate_param_sweep(P(s), optionsFrom(a).c_str()))); });
    t.add("sim", "sim_monte_carlo", "Monte Carlo", Kind::Read, true,
          "Monte Carlo and worst case over R / C / L tolerances for a net measure (dc, gain, f3db, peak).",
          Schema()
              .str("net", "Net", true)
              .str("measure", "dc, gain, f3db or peak", false, {"dc", "gain", "f3db", "peak"})
              .integer("runs", "Runs (default 100)")
              .integer("seed", "Random seed")
              .str("distribution", "uniform or gaussian", false, {"uniform", "gaussian"})
              .boolean("worstCase", "Also the worst-case corners")
              .object("tolerances", "{resistor, capacitor, inductor} fractions"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_simulate_monte_carlo(P(s), optionsFrom(a).c_str()));
              if (r.isObject() && r.has("samples")) r["samples"] = decimate(r.get("samples"), 50);
              return out(r);
          });
    t.add("sim", "sim_fft", "FFT / THD", Kind::Read, true,
          "Spectrum and total harmonic distortion of a net over a transient run.",
          Schema()
              .str("net", "Net", true)
              .str("stop", "Transient stop")
              .str("step", "Transient step")
              .num("fundamental", "Fundamental (Hz, default the first SIN source)")
              .integer("harmonics", "Harmonics to report")
              .str("from", "Window start"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_simulate_fft(P(s), optionsFrom(a).c_str()));
              if (r.isObject()) {
                  r["frequency"] = decimate(r.get("frequency"), 200);
                  r["magnitudeDb"] = decimate(r.get("magnitudeDb"), 200);
              }
              return out(r);
          });
    t.add("sim", "sim_noise", "Noise analysis", Kind::Read, true,
          "Output noise density and RMS of a net over a sweep, input-referred noise and the largest contributors.",
          Schema()
              .str("output", "Output net", true)
              .str("reference", "Reference net (default ground)")
              .str("source", "Input source for input-referred noise")
              .str("start", "Start (Hz, default 10)")
              .str("stop", "Stop (Hz, default 100k)")
              .integer("pointsPerDecade", "Points per decade")
              .num("temperature", "°C (default 27)"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_simulate_noise(P(s), optionsFrom(a).c_str()));
              if (r.isObject())
                  for (const char* k : {"frequency", "outputDensity", "inputDensity", "gain"})
                      if (r.get(k).isArray()) r[k] = decimate(r.get(k), 100);
              return out(r);
          });
    t.add("sim", "sim_measure", "Measure waveform", Kind::Read, true,
          "\".meas\"-like measurements of a net over a transient run (or of given samples): min, max, peak-to-peak, "
          "average, rms, rise / fall time, overshoot, settling time, period, frequency, duty cycle, in a window.",
          Schema()
              .str("net", "Net to simulate and measure")
              .str("stop", "Transient stop (with net)")
              .str("step", "Transient step")
              .array("time", "Sample times (instead of net)", "number")
              .array("values", "Sample values (instead of net)", "number")
              .str("from", "Window start (s)")
              .str("to", "Window end (s)"),
          [](McpServer& s, const Json& a) {
              Json req = obj();
              if (a.has("net")) {
                  const double stop = argNum(a, "stop", 0);
                  if (!(stop > 0)) throw ToolError("stop is required with net");
                  Json o = obj();
                  o["stop"] = stop;
                  o["step"] = argNum(a, "step", stop / 2000);
                  Json r = takeJson(sieda_simulate_transient_ex(P(s), o.dump().c_str()));
                  if (!r.get("ok").asBool(false)) throw ToolError("Transient failed: " + r.get("error").asString("?"));
                  bool found = false;
                  for (const auto& n : r.get("nets").items())
                      if (n.get("name").asString("") == argStr(a, "net")) {
                          req["values"] = n.get("values");
                          found = true;
                      }
                  if (!found) throw ToolError("No waveform for net " + argStr(a, "net"));
                  req["time"] = r.get("time");
              } else {
                  req["time"] = a.get("time");
                  req["values"] = a.get("values");
              }
              if (a.has("from")) req["from"] = argNum(a, "from", 0);
              if (a.has("to")) req["to"] = argNum(a, "to", 0);
              return out(takeJson(sieda_measure_waveform(req.dump().c_str())));
          });
    t.add("sim", "sim_spice_netlist", "SPICE netlist", Kind::Read, true, "The schematic as a SPICE netlist.", Schema(),
          [](McpServer& s, const Json&) {
              Json j = obj();
              j["netlist"] = takeText(sieda_spice_netlist(P(s)));
              return out(j);
          });
    t.add("sim", "sim_spice_parse", "Parse SPICE models", Kind::Read, true,
          "Parses vendor model text (.model / .subckt / .lib) and lists the models, their ports and diagnostics.",
          Schema().str("text", "Model text").str("path", "Model file inside the root folder"), [](McpServer& s, const Json& a) {
              const std::string text = a.has("path") ? readFileArg(s, argStr(a, "path")).asString() : requireStr(a, "text");
              return out(takeJson(sieda_spice_parse(text.c_str())));
          });
    t.add("sim", "sim_set_spice_model", "Attach SPICE model", Kind::Edit, false,
          "Attaches a vendor SPICE model (.model or .subckt from text or a file in the root folder) to a part, with "
          "an optional pin map (one entry per port: pin name / number, \"0\", \"net:NAME\", \"dc:15\", \"nc\"). Empty "
          "text removes it. builtin: use a ready-made model by name (sim_builtin_models).",
          Schema()
              .str("ref", "Part designator", true)
              .str("model", "Model / subcircuit name in the text")
              .str("text", "Model text")
              .str("path", "Model file inside the root folder")
              .str("builtin", "Built-in model name")
              .str("pins", "Pin map (default by position)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const int id = componentId(*p, a.get("ref"));
              std::string text = argStr(a, "text", ""), model = argStr(a, "model", "");
              if (a.has("path")) text = readFileArg(s, argStr(a, "path")).asString();
              if (a.has("builtin")) {
                  bool found = false;
                  const Json models = takeJson(sieda_spice_builtin_models());
                  for (const auto& m : models.items())
                      if (m.get("name").asString("") == argStr(a, "builtin")) {
                          text = m.get("text").asString("");
                          found = true;
                      }
                  if (!found) throw ToolError("Unknown built-in model (sim_builtin_models)");
                  if (model.empty()) {
                      const Json parsed = takeJson(sieda_spice_parse(text.c_str()));
                      if (parsed.get("entries").size()) model = parsed.get("entries")[0].get("name").asString("");
                  }
              }
              char* err = nullptr;
              if (sieda_set_spice_model(p, id, text.c_str(), model.c_str(), argStr(a, "pins", "").c_str(), &err) != 1)
                  throw ToolError("Model not attached: " + takeText(err));
              Json j = componentJson(*p, id, false);
              const Json m = takeJson(sieda_component_spice_model(p, id));
              j["spiceModel"] = m.get("model");
              j["spicePins"] = m.get("pins");
              return out(j);
          });
    t.add("sim", "sim_builtin_models", "Built-in models", Kind::Read, true,
          "Ready-made SPICE models of common parts (names and descriptions; give name for the model text).",
          Schema().str("name", "Model to return in full"), [](McpServer&, const Json& a) {
              Json list = Json::array();
              const Json models = takeJson(sieda_spice_builtin_models());
              for (const auto& m : models.items()) {
                  Json e = obj();
                  e["name"] = m.get("name");
                  e["description"] = m.get("description");
                  if (argStr(a, "name", "") == m.get("name").asString("")) e["text"] = m.get("text");
                  list.push(e);
              }
              Json j = obj();
              j["models"] = list;
              return out(j);
          });
}

// ================================================================================================= SI / PI tools

void siTools(Table& t) {
    t.add("si_pi", "si_impedance", "Stack-up impedance", Kind::Read, true,
          "The stack-up: laminate, εr, loss tangent, every layer with the single-ended and differential track width "
          "and gap for the impedance targets.",
          Schema(), [](McpServer& s, const Json&) { return out(takeJson(sieda_stackup_json(P(s)))); });
    t.add("si_pi", "si_net_list", "SI nets", Kind::Read, true,
          "Signal nets for SI, critical and fast first: length, delay, critical length, driver model, receivers.",
          Schema(), [](McpServer& s, const Json&) {
              Json j = obj();
              j["nets"] = takeJson(sieda_si_net_list_json(P(s)));
              return out(j);
          });
    t.add("si_pi", "si_net_analysis", "Net SI analysis", Kind::Read, true,
          "Transmission-line analysis of a routed net: line sections (Z0, delay), overshoot / undershoot / ringback "
          "at each receiver, termination advice; seriesOhms tries a series resistor at the driver.",
          Schema()
              .str("net", "Net name", true)
              .num("seriesOhms", "What-if series resistor (Ω; omit = as designed)")
              .boolean("waveforms", "Include the waveforms (default false)"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_si_net_json(P(s), requireStr(a, "net").c_str(), argNum(a, "seriesOhms", -1)));
              if (r.has("error")) throw ToolError(r.get("error").asString("?"));
              if (!argBool(a, "waveforms", false) && r.isObject()) {
                  Json slim = obj();
                  for (const auto& [k, v] : r.fields())
                      if (k != "waveform" && k != "terminatedWaveform") slim[k] = v;
                  r = slim;
              }
              return out(r);
          });
    t.add("si_pi", "si_crosstalk", "Crosstalk", Kind::Read, true,
          "Coupled track pairs with NEXT / FEXT noise against the limit, and return-path problems.", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_si_crosstalk_json(P(s)))); });
    t.add("si_pi", "si_checks", "SI / PI checks", Kind::Read, true,
          "Every signal- and power-integrity finding (SI_* / PI_* codes) with counts.", Schema(),
          [](McpServer& s, const Json&) { return out(violationsOut(s, sieda_si_checks_json(P(s)))); });
    t.add("si_pi", "si_length_report", "Length / skew report", Kind::Read, true,
          "Lengths of differential pairs and buses with their skew against the tolerances, and net length rules / "
          "match groups with their status.",
          Schema(), [](McpServer& s, const Json&) {
              Json j = obj();
              j["matching"] = takeJson(sieda_length_report_json(P(s)));
              j["targets"] = takeJson(sieda_length_targets_json(P(s)));
              return out(j);
          });
    t.add("si_pi", "si_line_loss", "Line loss", Kind::Read, true,
          "Insertion loss per stack-up layer (dB/inch over frequency, conductor and dielectric parts, RLGC at 1 GHz).",
          Schema()
              .str("foil", "smooth, hvlp, vlp, rtf, std")
              .str("roughness", "huray, hammerstad or none")
              .num("width", "Track width (mm, 0 = impedance width)")
              .str("fMax", "Highest frequency"),
          [](McpServer& s, const Json& a) { return out(takeJson(sieda_si_line_loss_json(P(s), optionsFrom(a).c_str()))); });
    t.add("si_pi", "si_channel", "Channel / eye", Kind::Read, true,
          "Channel of a routed net (a differential pair as a 4-port): S-parameters, step response and an eye "
          "diagram at a bit rate with optional CTLE / FFE. Options as docs/SIGNAL_POWER_INTEGRITY.md.",
          Schema()
              .str("net", "Net (one member of a pair)", true)
              .str("partner", "Pair partner (\"\" = by name, \"none\" = single-ended)")
              .object("eye", "Eye options {bitRate, prbs, ctle, ffe, …}")
              .str("fMax", "Highest frequency"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_si_channel_json(P(s), optionsFrom(a).c_str()));
              if (r.has("error")) throw ToolError(r.get("error").asString("?"));
              if (r.isObject()) {
                  r["freq"] = decimate(r.get("freq"), 100);
                  Json curves = Json::array();
                  for (const auto& c : r.get("curves").items()) {
                      Json m = c;
                      m["db"] = decimate(c.get("db"), 100);
                      curves.push(m);
                  }
                  r["curves"] = curves;
                  r["step"] = Json();
              }
              return out(r);
          });
    t.add("si_pi", "si_assign_model", "Assign driver model", Kind::Edit, true,
          "Assigns a driver / receiver model (logic family like \"lvcmos33\", or \"ibis:<model>\") to a net, a part or "
          "a pin (\"U1.12\"). Empty model clears it.",
          Schema()
              .str("kind", "net, component or pin", true, {"net", "component", "pin"})
              .str("target", "Net name, designator or REF.PIN", true)
              .str("model", "Model id"),
          [](McpServer& s, const Json& a) {
              check(sieda_si_assign_model(P(s), requireStr(a, "kind").c_str(), requireStr(a, "target").c_str(),
                                          argStr(a, "model", "").c_str()) == 1,
                    "Unknown target or model");
              Json j = obj();
              j["ok"] = true;
              return out(j);
          });
    t.add("si_pi", "si_set_options", "SI sign-off", Kind::Edit, true,
          "Adds (signOff) the Signal & Power Integrity stage to verification, with overshoot and crosstalk limits "
          "(fractions of the swing).",
          Schema().boolean("signOff", "Include SI/PI in verification", true).num("overshootLimit", "e.g. 0.15").num("crosstalkLimit", "e.g. 0.05"),
          [](McpServer& s, const Json& a) {
              check(sieda_si_set_options(P(s), argBool(a, "signOff", true) ? 1 : 0, argNum(a, "overshootLimit", 0.15),
                                         argNum(a, "crosstalkLimit", 0.05)) == 1,
                    "Invalid limits");
              Json j = obj();
              j["ok"] = true;
              return out(j);
          });
    t.add("si_pi", "pi_rails", "Power rails", Kind::Read, true,
          "Power distribution of every rail: target impedance, decoupling, plane, worst impedance, IR drop, "
          "compliance and recommendations.",
          Schema().boolean("curves", "Include the impedance curves (default false)"), [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_pi_json(P(s)));
              if (!argBool(a, "curves", false) && r.isObject()) {
                  Json rails = Json::array();
                  for (const auto& rail : r.get("rails").items()) {
                      Json m = obj();
                      for (const auto& [k, v] : rail.fields())
                          if (k != "curve") m[k] = v;
                      rails.push(m);
                  }
                  r["rails"] = rails;
              }
              return out(r);
          });
    t.add("si_pi", "pi_set_rail", "Rail inputs", Kind::Edit, true,
          "Power-integrity inputs of a rail: allowed ripple (%), load step and DC load (A), regulator output "
          "resistance (Ω) and loop bandwidth (Hz); 0 derives a value.",
          Schema()
              .str("net", "Rail net", true)
              .num("ripplePercent", "Allowed ripple (%)")
              .num("transientAmps", "Load step (A)")
              .num("dcAmps", "DC load (A)")
              .num("vrmROut", "Regulator output resistance (Ω)")
              .num("vrmBandwidth", "Regulator loop bandwidth (Hz)")
              .num("copperTempC", "Copper temperature for IR drop, whole board (°C, default 20)"),
          [](McpServer& s, const Json& a) {
              const std::string net = requireStr(a, "net");
              check(sieda_pi_set_rail(P(s), net.c_str(), argNum(a, "ripplePercent", 0), argNum(a, "transientAmps", 0), argNum(a, "dcAmps", 0)) == 1,
                    "Unknown rail");
              if (a.has("vrmROut") || a.has("vrmBandwidth"))
                  check(sieda_pi_set_vrm(P(s), net.c_str(), argNum(a, "vrmROut", 0), argNum(a, "vrmBandwidth", 0)) == 1, "Invalid regulator model");
              if (a.has("copperTempC"))
                  check(sieda_pi_set_copper_temperature(P(s), argNum(a, "copperTempC", 20)) == 1, "Copper temperature must be -55 to 200 °C");
              Json j = obj();
              j["ok"] = true;
              return out(j);
          });
    t.add("si_pi", "pi_decap_plan", "Decoupling plan", Kind::Read, true,
          "Decoupling capacitors to add so a rail meets its target impedance (values, footprints, counts).",
          Schema().str("net", "Rail net", true), [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_pi_decap_plan_json(P(s), requireStr(a, "net").c_str()));
              if (r.isObject())
                  for (const char* k : {"freq", "zBefore", "zAfter"}) r[k] = decimate(r.get(k), 60);
              return out(r);
          });
    t.add("si_pi", "pi_ir_drop", "IR drop", Kind::Read, true,
          "DC IR drop of a rail on the board: worst drop against the limit, current density, hot spots.",
          Schema().str("net", "Rail net", true).boolean("map", "Include the cell map (default false)"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_pi_ir_map_json(P(s), requireStr(a, "net").c_str()));
              if (!argBool(a, "map", false) && r.isObject()) {
                  r["cells"] = Json();
                  r["segments"] = Json();
              }
              return out(r);
          });
    t.add("si_pi", "si_memory_checks", "Memory design checks", Kind::Read, true,
          "The memory (SDRAM / DDR / LPDDR / DIMM) design segments checked on the design (set the type with "
          "design_set_domain domain memory).",
          Schema(), [](McpServer& s, const Json&) { return out(takeJson(sieda_memory_segments_json(P(s)))); });
}

// ================================================================================================= verification

void verifyTools(Table& t) {
    t.add("verify", "verify_design", "Verify design", Kind::Read, true,
          "Full design verification: ERC, DC operating point, validation, placement, routing, DRC, manufacturing "
          "outputs (and SI/PI when signed off). Verdict pass / warning / fail with every stage.",
          Schema().boolean("markdown", "Include the Markdown report"), [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_run_verification(P(s)));
              if (r.isObject() && !argBool(a, "markdown", false)) r["markdown"] = Json();
              return out(r);
          });
    t.add("verify", "design_segments", "Domain checks", Kind::Read, true,
          "The design segments of a domain checked on the design: robot, ecu (automotive), aerospace, naval, "
          "medical, retail, appliance or memory.",
          Schema().str("domain", "Domain", true, {"robot", "ecu", "aerospace", "naval", "medical", "retail", "appliance", "memory"}),
          [](McpServer& s, const Json& a) {
              const std::string d = requireStr(a, "domain");
              char* r = d == "robot"       ? sieda_robot_segments_json(P(s))
                        : d == "ecu"       ? sieda_ecu_segments_json(P(s))
                        : d == "aerospace" ? sieda_aerospace_segments_json(P(s))
                        : d == "naval"     ? sieda_naval_segments_json(P(s))
                        : d == "medical"   ? sieda_medical_segments_json(P(s))
                        : d == "retail"    ? sieda_retail_segments_json(P(s))
                        : d == "appliance" ? sieda_appliance_segments_json(P(s))
                                           : sieda_memory_segments_json(P(s));
              return out(takeJson(r));
          });
    t.add("verify", "verify_waivers", "Sign-off waivers", Kind::Edit, true,
          "Findings accepted at sign-off: each waiver names a check code (e.g. DRC_CLEARANCE, DFM_ASPECT_RATIO), "
          "optionally the part it applies to, and the reason. A matching finding stays in verify_design as Info "
          "\"(waived: reason)\" and no longer fails the design. With waivers: replaces the list ([] = none). Returns it.",
          Schema().array("waivers", "[{code, ref (optional), reason}]", "object"),
          [](McpServer& s, const Json& a) {
              if (a.has("waivers")) check(sieda_set_waivers(P(s), a.get("waivers").dump().c_str()) == 1, "Each waiver needs a code");
              return out(takeJson(sieda_waivers_json(P(s))));
          });
    t.add("verify", "verify_requirements", "Requirements traceability", Kind::Edit, false,
          "Requirements traceability: each requirement (id, text) names the parts that meet it (refs). With "
          "requirements: replaces the list. Returns each requirement's status (traced: every ref is in the design; "
          "missing: a ref is not; untraced: no refs) and the parts no requirement names.",
          Schema().array("requirements", "[{id, text, refs: [\"U1\", …]}]", "object"),
          [](McpServer& s, const Json& a) {
              Project& p = P(s)->project;
              if (a.get("requirements").isArray()) {
                  std::vector<Project::RequirementItem> items;
                  for (const auto& r : a.get("requirements").items()) {
                      check(r.get("id").isString() && !r.get("id").asString().empty(), "Each requirement needs an id");
                      Project::RequirementItem item{r.get("id").asString(), r.get("text").asString(""), {}};
                      for (const auto& ref : r.get("refs").items())
                          if (ref.isString()) item.refs.push_back(ref.asString());
                      items.push_back(item);
                  }
                  p.requirementItems = items;
              }
              std::set<std::string> named;
              Json list = Json::array();
              for (const auto& r : p.requirementItems) {
                  Json j = obj(), missing = Json::array(), refs = Json::array();
                  for (const auto& ref : r.refs) {
                      refs.push(ref);
                      named.insert(ref);
                      if (!p.schematic.findByRef(ref)) missing.push(ref);
                  }
                  j["id"] = r.id, j["text"] = r.text, j["refs"] = refs;
                  j["status"] = r.refs.empty() ? "untraced" : missing.size() ? "missing" : "traced";
                  if (missing.size()) j["missing"] = missing;
                  list.push(j);
              }
              Json unnamed = Json::array();
              for (const auto& c : p.schematic.components())
                  if (!c.ref.empty() && !c.packageOnly && c.hasFootprint() && !named.count(c.ref)) unnamed.push(c.ref);
              Json j = obj();
              j["requirements"] = list, j["partsNotTraced"] = unnamed;
              return out(j);
          });
    t.add("verify", "verify_measurements", "Bench measurements", Kind::Edit, false,
          "Measured results on built boards, linked to the design: add records what was measured (a net, a rail, a "
          "test point), its value and unit, optional min / max limits and a note, with today's date and the design "
          "fingerprint. Returns every measurement with pass (inside its limits) and current (taken on this revision).",
          Schema().object("add", "{what, value, unit, min, max, note}").integer("remove", "Position of a measurement to remove"),
          [](McpServer& s, const Json& a) {
              Project& p = P(s)->project;
              if (a.get("add").isObject()) {
                  const Json& x = a.get("add");
                  check(x.get("what").isString() && x.get("value").isNumber(), "add needs what and a numeric value");
                  Project::Measurement m;
                  m.what = x.get("what").asString(), m.value = x.get("value").asNumber(), m.unit = x.get("unit").asString("");
                  m.note = x.get("note").asString("");
                  m.hasMin = x.get("min").isNumber(), m.min = x.get("min").asNumber(0);
                  m.hasMax = x.get("max").isNumber(), m.max = x.get("max").asNumber(0);
                  const std::time_t now = std::time(nullptr);
                  char date[11];
                  std::strftime(date, sizeof date, "%Y-%m-%d", std::gmtime(&now));
                  m.date = date, m.revision = p.designHash();
                  p.measurements.push_back(m);
              }
              if (a.has("remove")) {
                  const int i = a.get("remove").asInt(-1);
                  check(i >= 0 && i < static_cast<int>(p.measurements.size()), "No measurement at that position");
                  p.measurements.erase(p.measurements.begin() + i);
              }
              const std::string revision = p.designHash();
              Json list = Json::array();
              int failed = 0;
              for (const auto& m : p.measurements) {
                  Json j = obj();
                  const bool pass = (!m.hasMin || m.value >= m.min) && (!m.hasMax || m.value <= m.max);
                  failed += !pass;
                  j["what"] = m.what, j["value"] = m.value, j["unit"] = m.unit, j["date"] = m.date, j["note"] = m.note;
                  if (m.hasMin) j["min"] = m.min;
                  if (m.hasMax) j["max"] = m.max;
                  j["pass"] = pass, j["current"] = m.revision == revision;
                  list.push(j);
              }
              Json j = obj();
              j["measurements"] = list, j["failed"] = failed;
              return out(j);
          });
    t.add("project", "project_plugin_objects", "Plug-in objects", Kind::Edit, false,
          "Objects of a plug-in's own type kept in the design (saved, diffed, merged and undone with it; SiEDA does not "
          "interpret them): add {type, plugin, id?, layer?, points: [[x, y], …] in mm, props: {…}}, update {id, layer?, "
          "points?, props?} (props merged), remove id. Lists them (type filters).",
          Schema().object("add", "{type, plugin, id, layer, points, props}").object("update", "{id, layer, points, props}")
              .str("remove", "Id of an object to remove").str("type", "List only this type"),
          [](McpServer& s, const Json& a) {
              Project& p = P(s)->project;
              auto find = [&](const std::string& id) {
                  for (auto& o : p.pluginObjects)
                      if (o.id == id) return &o;
                  return static_cast<Project::PluginObject*>(nullptr);
              };
              auto setFields = [](Project::PluginObject& o, const Json& x) {
                  if (x.get("layer").isString()) o.layer = x.get("layer").asString();
                  if (x.get("points").isArray()) {
                      o.points.clear();
                      for (const auto& pt : x.get("points").items()) {
                          check(pt.isArray() && pt.size() == 2 && pt[0].isNumber() && pt[1].isNumber(), "points are [x, y] pairs in mm");
                          o.points.push_back({pt[0].asNumber(), pt[1].asNumber()});
                      }
                  }
                  if (x.get("props").isObject())
                      for (const auto& [k, v] : x.get("props").fields()) o.props[k] = v;
              };
              if (a.get("add").isObject()) {
                  const Json& x = a.get("add");
                  check(x.get("type").isString() && !x.get("type").asString().empty(), "add needs a type");
                  check(p.pluginObjects.size() < 10000, "At most 10000 plug-in objects");
                  Project::PluginObject o;
                  o.type = x.get("type").asString(), o.plugin = x.get("plugin").asString("");
                  o.id = x.get("id").asString("");
                  for (int n = 1; o.id.empty() || (find(o.id) && !x.get("id").isString()); ++n) o.id = "PO" + std::to_string(n);
                  check(!find(o.id), "An object " + o.id + " exists already");
                  setFields(o, x);
                  p.pluginObjects.push_back(std::move(o));
              }
              if (a.get("update").isObject()) {
                  Project::PluginObject* o = find(a.get("update").get("id").asString(""));
                  check(o != nullptr, "No plug-in object with that id");
                  setFields(*o, a.get("update"));
              }
              if (a.has("remove")) {
                  const std::string id = argStr(a, "remove");
                  check(find(id) != nullptr, "No plug-in object " + id);
                  p.pluginObjects.erase(std::remove_if(p.pluginObjects.begin(), p.pluginObjects.end(), [&](const auto& o) { return o.id == id; }),
                                        p.pluginObjects.end());
              }
              Json list = Json::array();
              for (const auto& o : p.pluginObjects) {
                  if (a.has("type") && o.type != argStr(a, "type")) continue;
                  Json j = obj(), pts = Json::array();
                  for (const auto& v : o.points) {
                      Json pt = Json::array();
                      pt.push(v.x), pt.push(v.y);
                      pts.push(pt);
                  }
                  j["id"] = o.id, j["type"] = o.type, j["plugin"] = o.plugin, j["layer"] = o.layer, j["points"] = pts, j["props"] = o.props;
                  list.push(j);
              }
              Json j = obj();
              j["objects"] = list;
              return out(j);
          });
    t.add("verify", "verify_approvals", "Sign-off approvals", Kind::Edit, false,
          "Approvals of this design revision (release sign-off). approve records who approved it (by, role, note) "
          "against the design's fingerprint; any later change to the design makes it stale (current false). revoke "
          "removes approval number index. Without action: lists them with the current revision.",
          Schema().str("action", "approve, revoke or none (list)").str("by", "Who approves").str("role", "Their role (e.g. design, test, quality)")
              .str("note", "Optional note").integer("index", "revoke: the approval's position (0 = first)"),
          [](McpServer& s, const Json& a) {
              Project& p = P(s)->project;
              const std::string action = argStr(a, "action", "");
              if (action == "approve") {
                  const std::string by = argStr(a, "by", "");
                  check(!by.empty(), "approve needs by (who approves)");
                  const std::time_t now = std::time(nullptr);
                  char date[11];
                  std::strftime(date, sizeof date, "%Y-%m-%d", std::gmtime(&now));
                  p.approvals.push_back({by, argStr(a, "role", ""), date, p.designHash(), argStr(a, "note", "")});
              } else if (action == "revoke") {
                  const int i = a.get("index").asInt(-1);
                  check(i >= 0 && i < static_cast<int>(p.approvals.size()), "No approval at that index");
                  p.approvals.erase(p.approvals.begin() + i);
              } else {
                  check(action.empty() || action == "none", "action is approve, revoke or none");
              }
              const std::string revision = p.designHash();
              Json list = Json::array();
              int current = 0;
              for (const auto& x : p.approvals) {
                  Json j = obj();
                  j["by"] = x.by, j["role"] = x.role, j["date"] = x.date, j["revision"] = x.revision, j["note"] = x.note;
                  j["current"] = x.revision == revision;
                  current += x.revision == revision;
                  list.push(j);
              }
              Json j = obj();
              j["revision"] = revision, j["approvals"] = list, j["approvedNow"] = current;
              return out(j);
          });
    t.add("verify", "memory_layout_limits", "DDR layout limits", Kind::Edit, true,
          "DDR layout limits checked on the routed memory bus (byte-lane skew, DQ to DQS, command / address to clock, "
          "data impedance tolerance, via spread). With arguments: sets the controller's own values (0 or omitted = the memory "
          "type's typical value). Returns the limits in force.",
          Schema().num("laneSkewPs", "Byte-lane DQ skew, ps").num("dqsSkewPs", "DQ to its strobe, ps")
              .num("addrSkewPs", "Command / address to clock, ps").num("impedanceTolPercent", "Data impedance tolerance, %")
              .integer("laneViaSpread", "Via-count spread within a lane (-1 = typical)")
              .num("pairSkewPs", "P to N of the clock and strobe pairs, ps"),
          [](McpServer& s, const Json& a) {
              if (a.isObject() && a.size() > 0) check(sieda_set_memory_limits(P(s), a.dump().c_str()) == 1, "Invalid limits");
              return out(takeJson(sieda_memory_limits_json(P(s))));
          });
    t.add("verify", "design_set_domain", "Set domain", Kind::Edit, true,
          "Sets the design's domain type, which turns on its checks: robot (rover, fpv, arm, quadruped, humanoid, "
          "printer3d, cnc), ecu (bcm, powertrain, adas, ev, chassis, gateway), aerospace (leo, geo, launcher, "
          "military, commercial), naval (combatant, carrier, submarine, patrol, commercial), medical (bf, cf, life, "
          "implant, home), retail (countertop, unattended, mpos, kiosk, printer), appliance (laundry, kitchen, "
          "refrigeration, hvac, small), memory (sdram, ddr, lpddr, dimm, rdimm). \"\" clears it.",
          Schema()
              .str("domain", "Domain", true, {"robot", "ecu", "aerospace", "naval", "medical", "retail", "appliance", "memory"})
              .str("value", "Type id (\"\" = none)"),
          [](McpServer& s, const Json& a) {
              const std::string d = requireStr(a, "domain"), v = argStr(a, "value", "");
              const int ok = d == "robot"       ? sieda_set_robot_platform(P(s), v.c_str())
                             : d == "ecu"       ? sieda_set_ecu_type(P(s), v.c_str())
                             : d == "aerospace" ? sieda_set_aerospace_mission(P(s), v.c_str())
                             : d == "naval"     ? sieda_set_naval_platform(P(s), v.c_str())
                             : d == "medical"   ? sieda_set_medical_class(P(s), v.c_str())
                             : d == "retail"    ? sieda_set_retail_device(P(s), v.c_str())
                             : d == "appliance" ? sieda_set_appliance_type(P(s), v.c_str())
                                                : sieda_set_memory_design(P(s), v.c_str());
              check(ok == 1, "Unknown " + d + " type \"" + v + "\"");
              Json j = obj();
              j[d] = v;
              return out(j);
          });
}

// ================================================================================================= outputs

void outputTools(Table& t) {
    t.add("output", "output_fabrication_package", "Fabrication package", Kind::Files, false,
          "Writes the complete fabrication package into a folder inside the root: gerbers/ (every layer, drills, "
          "job file, IPC-D-356A), assembly/ (BOMs, CPL, pick-and-place, drawings), fab notes, a gerber zip, netlist "
          "and 3D STL.",
          Schema().str("dir", "Output folder (inside the root), e.g. \"fab\"", true).str("base", "File base name (default the project name)"),
          [](McpServer& s, const Json& a) {
              const std::string dir = s.resolvePath(requireStr(a, "dir"), true);
              std::error_code ec;
              fs::create_directories(dir, ec);
              s.checkLinksInside(dir);
              const std::string base = argStr(a, "base", "");
              Json r = takeJson(sieda_write_fabrication_package(P(s), dir.c_str(), base.empty() ? nullptr : base.c_str()));
              if (!r.get("ok").asBool(false)) throw ToolError("Writing the package failed: " + r.get("error").asString("?"));
              r["dir"] = dir;
              return out(r);
          });
    t.add("output", "output_release", "Controlled release", Kind::Files, false,
          "A controlled release in a folder inside the root: the fabrication package (fabrication/), the project as "
          "released (<name>-rev<revision>.siedaproj), the verification report, the evidence (design fingerprint, "
          "approvals of this revision, requirements trace, waivers, bench measurements), a change notice against previous (an older "
          ".siedaproj) and MANIFEST.sha256 with every file's SHA-256 (check with shasum -a 256 -c). ready = "
          "verification not failed, at least one current approval and no requirement missing its parts.",
          Schema().str("dir", "Release folder inside the root, e.g. \"release/B\"", true).str("revision", "Revision, e.g. B", true)
              .str("previous", "Older .siedaproj inside the root for the change notice"),
          [](McpServer& s, const Json& a) {
              auto tool = [&](const char* name, const Json& args) {
                  for (const auto& t : mcpTools())
                      if (t.name == name) return t.handler(s, args).data;
                  throw ToolError(std::string("missing tool ") + name);
              };
              const std::string dir = s.resolvePath(requireStr(a, "dir"), true), revision = requireStr(a, "revision");
              std::error_code ec;
              fs::create_directories(fs::path(dir) / "fabrication", ec);
              s.checkLinksInside(dir);
              Json fab = takeJson(sieda_write_fabrication_package(P(s), (fs::path(dir) / "fabrication").string().c_str(), nullptr));
              if (!fab.get("ok").asBool(false)) throw ToolError("Writing the package failed: " + fab.get("error").asString("?"));
              std::string base = proj(s).name.empty() ? "board" : proj(s).name;
              for (auto& c : base)
                  if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
              const std::string saved = takeText(sieda_project_save_json(P(s)));
              auto write = [&](const std::string& name, const std::string& text) { s.writeFile((fs::path(dir) / name).string(), text); };
              write(base + "-rev" + revision + ".siedaproj", saved);
              const Json verification = takeJson(sieda_run_verification(P(s)));
              write("verification.md", verification.get("markdown").asString(""));
              Json evidence = obj();
              evidence["revision"] = revision, evidence["designHash"] = proj(s).designHash();
              evidence["verdict"] = verification.get("verdict");
              evidence["approvals"] = tool("verify_approvals", obj());
              evidence["requirements"] = tool("verify_requirements", obj());
              evidence["waivers"] = takeJson(sieda_waivers_json(P(s)));
              evidence["measurements"] = tool("verify_measurements", obj());
              write("evidence.json", evidence.dump(true));
              if (a.has("previous")) {
                  const std::string before = readFileArg(s, requireStr(a, "previous")).asString();
                  write("change_notice.md", "# Change notice: revision " + revision + "\n\n" +
                                                takeText(sieda_diff_projects(before.c_str(), saved.c_str(), 1)));
              }
              // The manifest last: every file written above, sorted, as shasum -a 256 prints them.
              std::vector<std::string> files;
              for (const auto& e : fs::recursive_directory_iterator(dir, ec))
                  if (e.is_regular_file() && e.path().filename() != "MANIFEST.sha256")
                      files.push_back(fs::relative(e.path(), dir).generic_string());
              std::sort(files.begin(), files.end());
              std::string manifest;
              for (const auto& f : files) {
                  std::ifstream in(fs::path(dir) / f, std::ios::binary);
                  std::stringstream ss;
                  ss << in.rdbuf();
                  manifest += sha256Hex(ss.str()) + "  " + f + "\n";
              }
              write("MANIFEST.sha256", manifest);
              bool missing = false;
              for (const auto& r : evidence.get("requirements").get("requirements").items()) missing |= r.get("status").asString("") == "missing";
              Json j = obj();
              j["dir"] = dir, j["files"] = static_cast<int>(files.size()) + 1, j["designHash"] = proj(s).designHash();
              j["verdict"] = verification.get("verdict");
              j["approvedNow"] = evidence.get("approvals").get("approvedNow");
              j["ready"] = verification.get("verdict").asString("") != "fail" && evidence.get("approvals").get("approvedNow").asInt(0) > 0 && !missing;
              return out(j);
          });
    t.add("output", "output_gerbers", "Gerbers and drill", Kind::Files, false,
          "Writes RS-274X Gerbers (every copper layer, masks, silkscreens, paste, outline) and Excellon drill files "
          "into a folder inside the root.",
          Schema().str("dir", "Output folder (inside the root)", true).str("base", "File name prefix (default the project name)"),
          [](McpServer& s, const Json& a) {
              const std::string dir = requireStr(a, "dir");
              std::string base = argStr(a, "base", proj(s).name);
              for (auto& c : base)
                  if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_') c = '_';
              if (base.empty()) base = "board";
              std::vector<std::pair<std::string, std::string>> files;  // format, file suffix
              const int layers = proj(s).pcb.settings.layerCount;
              for (int l = 1; l <= layers; ++l)
                  files.push_back({"gerber_l" + std::to_string(l), l == 1 ? "-F_Cu.gtl" : l == layers ? "-B_Cu.gbl" : "-In" + std::to_string(l - 1) + "_Cu.g" + std::to_string(l)});
              for (const auto& f : std::vector<std::pair<std::string, std::string>>{
                       {"gerber_mask_top", "-F_Mask.gts"}, {"gerber_mask_bottom", "-B_Mask.gbs"},
                       {"gerber_silk_top", "-F_Silk.gto"}, {"gerber_silk_bottom", "-B_Silk.gbo"},
                       {"gerber_paste_top", "-F_Paste.gtp"}, {"gerber_paste_bottom", "-B_Paste.gbp"},
                       {"gerber_edge", "-Edge_Cuts.gm1"}, {"drill", "-PTH.drl"}, {"drill_npth", "-NPTH.drl"}})
                  files.push_back(f);
              Json written = Json::array();
              for (const auto& [format, suffix] : files) {
                  const std::string content = takeText(sieda_export(P(s), format.c_str()));
                  if (content.empty()) continue;
                  written.push(s.writeFile((fs::path(dir) / (base + suffix)).string(), content));
              }
              Json j = obj();
              j["files"] = written;
              return out(j);
          });
    t.add("output", "output_export", "Export", Kind::Files, false,
          "One output as text, returned inline or written to path (inside the root): spice, bom, bom_assembly, cpl, "
          "pnp, assembly_top / assembly_bottom (SVG), gerber_top, gerber_bottom, gerber_l<N>, gerber_mask_top / "
          "_bottom, gerber_silk_top / _bottom, gerber_paste_top / _bottom, gerber_edge, gerber_job, drill, "
          "drill_npth, ipc356, ipc2581 (IPC-2581C XML), hyperlynx (HyperLynx .hyp for signal-integrity tools), fab_notes, stl, obj, dxf (outline, holes, silk, courtyards), "
          "freecad / fusion (FreeCAD macro / Autodesk Fusion script: board and parts by designator; run again it writes the "
          "moved placement as IDF for pcb_import_idf_placement), testpoints (probe point per net, CSV), html_bom (interactive BOM / board viewer), bringup (bring-up "
          "checklist, Markdown), fab_drawing (fabrication drawing PDF: dimensions, drill map / table, stack-up, "
          "revisions), net_lengths (routed length per net, CSV). variant: an assembly variant for the assembly outputs.",
          Schema().str("format", "Output format", true).str("path", "File to write (default: return the text)").str("variant", "Assembly variant"),
          [](McpServer& s, const Json& a) {
              const std::string f = requireStr(a, "format");
              const std::string content = a.has("variant") ? takeText(sieda_export_variant(P(s), f.c_str(), argStr(a, "variant").c_str()))
                                                           : takeText(sieda_export(P(s), f.c_str()));
              if (content.empty()) throw ToolError("Unknown format \"" + f + "\" (or nothing to export)");
              Json j = writeOrInline(s, a, content, f);
              j["format"] = f;
              return out(j);
          });
    t.add("output", "output_bom", "Bill of materials", Kind::Files, false,
          "The bill of materials (grouped lines, quantities, MPNs, cost) as JSON, or CSV written to path. columns "
          "or board give a CSV with the columns you choose, in order: item, designators, quantity, value, type, "
          "footprint, description, rating, manufacturer, mpn, supplier_part, unit_price, line_cost, dnp, notes, "
          "boards; board picks one board (index) of a multi-board project, else one consolidated BOM.",
          Schema().str("format", "json (default) or csv", false, {"json", "csv"}).str("path", "File to write")
              .array("columns", "CSV column names in order", "string")
              .integer("board", "Board index (multi-board projects; omit for the consolidated BOM)"),
          [](McpServer& s, const Json& a) {
              if (a.has("columns") || a.has("board")) {
                  std::vector<std::string> cols;
                  for (const auto& c : a.get("columns").items()) cols.push_back(c.asString(""));
                  return out(writeOrInline(s, a, exportBomColumns(proj(s), cols, a.has("board") ? argInt(a, "board", -1) : -1), "BOM"));
              }
              if (argStr(a, "format", "json") == "csv") return out(writeOrInline(s, a, takeText(sieda_export(P(s), "bom")), "BOM"));
              Json bom = takeJson(sieda_bom_json(P(s)));
              if (a.has("path")) {
                  Json j = obj();
                  j["written"] = s.writeFile(argStr(a, "path"), bom.dump(true));
                  j["summary"] = bom.get("summary");
                  return out(j);
              }
              return out(bom);
          });
    t.add("output", "output_pick_and_place", "Pick and place", Kind::Files, false,
          "Pick-and-place / centroid file (designator, x, y, rotation, side) as CSV, inline or written to path.",
          Schema().str("path", "File to write").str("format", "pnp (default) or cpl (assembly house)", false, {"pnp", "cpl"}),
          [](McpServer& s, const Json& a) {
              const std::string f = argStr(a, "format", "pnp");
              return out(writeOrInline(s, a, takeText(sieda_export(P(s), f.c_str())), "pick and place"));
          });
    t.add("output", "output_schematic_pdf", "Schematic PDF", Kind::Files, false,
          "The schematic as a PDF (one page per sheet with frame and title block, bookmarks by hierarchy), written "
          "to path inside the root, or returned as an embedded PDF resource.",
          Schema().str("path", "PDF file to write, e.g. \"out/schematic.pdf\""), [](McpServer& s, const Json& a) {
              const std::string pdf = takeText(sieda_export_schematic_pdf(P(s)));
              if (pdf.empty()) throw ToolError("Could not produce the PDF");
              ToolOutput o;
              Json j = obj();
              if (a.has("path")) {
                  j["written"] = s.writeFile(argStr(a, "path"), pdf);
                  j["bytes"] = pdf.size();
              } else {
                  j["bytes"] = pdf.size();
                  Json res = obj();
                  res["uri"] = "sieda://output/schematic.pdf";
                  res["mimeType"] = "application/pdf";
                  res["blob"] = base64Encode(pdf);
                  Json item = obj();
                  item["type"] = "resource";
                  item["resource"] = res;
                  o.content.push(item);
              }
              o.data = j;
              return o;
          });
    t.add("output", "output_3d_model", "3D / MCAD model", Kind::Files, false,
          "The assembled board for mechanical CAD written to path inside the root: STEP AP214 solids (board + one "
          "named body per part), IDF 3.0 board (.emn) or library (.emp), an IDX (ProSTEP EDMD v4.5) baseline, or a "
          "mesh (STL, OBJ, VRML 2.0, glTF 2.0 binary .glb).",
          Schema().str("path", "File to write, e.g. \"out/board.step\"", true)
              .str("format", "step, idf_board, idf_library, idx, stl (default), obj, vrml or glb", false,
                   {"step", "idf_board", "idf_library", "idx", "stl", "obj", "vrml", "glb"}),
          [](McpServer& s, const Json& a) {
              const std::string format = argStr(a, "format", "stl");
              const Project& pr = P(s)->project;
              const std::string content = format == "glb" ? exportGltf(buildAssemblyMesh(pr.schematic, pr.pcb), pr.name)
                                                          : takeText(sieda_export(P(s), format.c_str()));
              if (content.empty()) throw ToolError("Could not build the 3D model");
              Json j = obj();
              j["written"] = s.writeFile(requireStr(a, "path"), content);
              j["bytes"] = content.size();
              return out(j);
          });
}

void teamTools(Table& t) {
    t.add("project", "project_merge", "Merge versions", Kind::Files, false,
          "Three-way merge of two edited copies of a project against their common ancestor (all files inside the "
          "root), written to path. Parts, wires and sheets merge item by item, copper as sets; a field both sides "
          "changed differently keeps 'ours' and is listed in conflicts.",
          Schema().str("base", "Common ancestor .siedaproj", true).str("ours", "Our edited copy", true)
              .str("theirs", "Their edited copy", true).str("path", "Where to write the merged project", true),
          [](McpServer& s, const Json& a) {
              const std::string b = readFileArg(s, requireStr(a, "base")).asString(),
                                o = readFileArg(s, requireStr(a, "ours")).asString(),
                                th = readFileArg(s, requireStr(a, "theirs")).asString();
              Json r = takeJson(sieda_merge_projects(b.c_str(), o.c_str(), th.c_str()));
              if (r.has("error")) throw ToolError(r.get("error").asString());
              Json j = obj();
              j["written"] = s.writeFile(requireStr(a, "path"), r.get("merged").dump(true));
              j["conflicts"] = r.get("conflicts");
              return out(j);
          });
    t.add("project", "project_review_comments", "Review comments", Kind::Read, true,
          "Design review comments (pinned to a part and / or a place) with their replies and status, and the review "
          "as Markdown.", Schema(),
          [](McpServer& s, const Json&) { return out(takeJson(sieda_review_json(P(s)))); });
    t.add("project", "project_review_comment", "Comment / resolve", Kind::Edit, false,
          "Review commands: add (text, author, ref and / or x, y with view pcb|schematic), reply (id, text, author), "
          "resolve, reopen or delete (id).",
          Schema().str("action", "What to do", true, {"add", "reply", "resolve", "reopen", "delete"})
              .num("id", "Comment id (reply, resolve, reopen, delete)").str("text", "Comment or reply text")
              .str("author", "Who writes it").str("ref", "Part designator the comment is about")
              .str("view", "pcb (default) or schematic", false, {"pcb", "schematic"})
              .num("x", "Place (mm on the board, grid units on the schematic)").num("y", "Place"),
          [](McpServer& s, const Json& a) {
              Json q = a;
              Json r = takeJson(sieda_review_command(P(s), q.dump().c_str()));
              if (r.has("error")) throw ToolError(r.get("error").asString());
              Json all = takeJson(sieda_review_json(P(s)));
              all["id"] = r.get("id");
              return out(all);
          });
    t.add("pcb", "pcb_mechanical_limits", "Enclosure limits", Kind::Edit, true,
          "3D clearance limits checked by the DRC: tallest part per side (mm, 0 = none) and height zones "
          "(rectangles with their own maximum height), and rigid-flex bend areas checked against IPC-2223 (bend radius ≥ "
          "6 / 12 / 24 × the flex thickness for 1 / 2 / more copper layers, 100 × when dynamic; no vias, parts or sharp track "
          "corners in the bend). Without arguments returns the current limits.",
          Schema().num("maxHeightTop", "Tallest part on the top side (mm, 0 = no limit)")
              .num("maxHeightBottom", "Tallest part on the bottom side (mm, 0 = no limit)")
              .any("zones", "[{name, x0, y0, x1, y1, bottom, maxHeight}] (mm, board coordinates)")
              .any("flex", "Rigid-flex bend areas [{name, x0, y0, x1, y1, bendRadius, thickness (mm, default 0.11), layers "
                           "(copper, default 2), dynamic, bendAngle (degrees the board past the area folds in render_3d, the 3D view and "
                           "the drawing: + up, − down; it bends across the area's shorter side)}]"),
          [](McpServer& s, const Json& a) {
              if (a.has("maxHeightTop") || a.has("maxHeightBottom") || a.has("zones") || a.has("flex")) {
                  Json cur = takeJson(sieda_pcb_mechanical_limits(P(s)));
                  if (!cur.isObject()) cur = obj();
                  for (const char* k : {"maxHeightTop", "maxHeightBottom", "zones", "flex"})
                      if (a.has(k)) cur[k] = a.get(k);
                  check(sieda_pcb_set_mechanical_limits(P(s), cur.dump().c_str()) == 1, "Invalid limits");
              }
              return out(takeJson(sieda_pcb_mechanical_limits(P(s))));
          });
    t.add("pcb", "pcb_dfm_pack", "Manufacturer rule pack", Kind::Edit, true,
          "Manufacturer DFM / DFA rule packs (JLCPCB, PCBWay, OSH Park, Eurocircuits, IPC Class 3). With pack: selects it "
          "(\"\" = none), tightening the DRC minimums and adding the DFM_* / DFA_* checks to run_drc. Returns the packs, "
          "the board's pack and its sign-off report (measured value, limit, pass / fail per rule).",
          Schema().str("pack", "Pack id, e.g. jlcpcb-standard (\"\" = none)")
              .object("overrides", "Your fab's own values by field, e.g. {\"minTrack\":0.1,\"maxAspectRatio\":12} (0 removes one)"),
          [](McpServer& s, const Json& a) {
              if (a.has("pack")) check(sieda_pcb_set_dfm_pack(P(s), argStr(a, "pack").c_str()) == 1, "Unknown pack");
              if (a.get("overrides").isObject())
                  for (const auto& [k, v] : a.get("overrides").fields())
                      check(sieda_pcb_set_dfm_override(P(s), k.c_str(), v.asNumber(-1)) == 1, "Unknown pack field " + k);
              Json j = obj();
              j["packs"] = takeJson(sieda_dfm_packs_json());
              j["selected"] = takeJson(sieda_project_snapshot(P(s))).get("board").get("dfmPack");
              j["report"] = takeJson(sieda_dfm_report_json(P(s)));
              return out(j);
          });
    t.add("pcb", "pcb_custom_rules", "Custom design rules", Kind::Edit, true,
          "Rule scripting: your own DRC rules as expressions over each track, via, pad, part or net. rules replaces "
          "them ([] removes all): {name, object, when (filter, \"\" = every object), require, severity (error / warning / "
          "info), message}. Expressions: numbers with mm / mil / in, \"text\", ( ), + - * /, == != < <= > >=, ~ (glob: "
          "net ~ \"USB_*\"), and / && , or / ||, not / !. Properties — track: net, width, length, layer (\"Top\", \"Bottom\", "
          "\"Inner 1\"), "
          "layerIndex, arc, locked; via: net, drill, diameter, ring, through; pad: net, ref, pin, width, height, drill, "
          "through, npth; part: ref, value, footprint, mpn, x, y, rotation, bottom, dnp; net: net, length, tracks, vias, "
          "pads, minWidth. pcb_drc reports a broken rule as RULE_<NAME> (RULE_SYNTAX when it cannot be read). Returns the "
          "rules, read errors and what they flag now.",
          Schema().array("rules", "[{name, object, when, require, severity, message}]", "object"),
          [](McpServer& s, const Json& a) {
              const Json rules = a.get("rules");
              Json j = takeJson(sieda_pcb_custom_rules(P(s), rules.isArray() ? rules.dump().c_str() : nullptr));
              check(!j.isNull(), "rules must be an array");
              return out(j);
          });
    t.add("output", "output_fab_quote", "Fab quote and order", Kind::Read, false,
          "What a board house's order form asks, read from the board (layers, size, area, thickness, material, copper, "
          "finish, mask / silk colours, minimum track / space / drill / ring, hole and via counts, HDI, via-in-pad, "
          "impedance control, panel), checked against the fab's published limits (JLCPCB, PCBWay, OSH Park, "
          "Eurocircuits; Lion Circuits and PCB Power (India) have no published pack: check their capability page), its "
          "order page and steps. A price estimate only from priceTable (your fab's prices, sieda.fabprice/1: currency, "
          "setup, perCm2, minimum, layers {\"4\": 2.2}, quantityFactors [[qty, factor]], options {ENIG, impedance, hdi, "
          "viaInPad, colour}, leadDays); SiEDA has no built-in prices. Without fab: the list of fabs.",
          Schema().str("fab", "jlcpcb, pcbway, oshpark, eurocircuits, lioncircuits or pcbpower")
              .integer("quantity", "Boards to order (default 5)")
              .object("priceTable", "Your fab's prices (sieda.fabprice/1) for an estimate"),
          [](McpServer& s, const Json& a) {
              Json j = takeJson(sieda_fab_quote(P(s), a.dump().c_str()));
              if (j.has("error") && a.has("fab")) throw ToolError(j.get("error").asString() + " (fabs: jlcpcb, pcbway, oshpark, eurocircuits, lioncircuits, pcbpower)");
              return out(j);
          });
    t.add("pcb", "pcb_thermal", "Thermal analysis", Kind::Read, true,
          "Board temperature map: the steady-state temperature over the board from each part's dissipation (the DC "
          "operating point's, or `power` by designator), spread by the laminate and each layer's copper (tracks, pads, "
          "pours) and lost to the air from both faces. Gives the hottest spot, every powered part's board and junction "
          "temperature (board + power × θJB, estimated from the package unless `thetaJB` gives it; `heatsink` takes part of a part's power off the board), THERMAL_JUNCTION / "
          "THERMAL_BOARD findings and a heat-map picture.",
          Schema()
              .num("ambient", "Ambient air, °C (default 25)")
              .object("power", "Dissipation by designator, W, e.g. {\"U1\": 1.2} (replaces the DC operating point's)")
              .object("thetaJB", "Junction-to-board thermal resistance by designator, °C/W (from the datasheet)")
              .object("heatsink", "Parts on a heatsink: junction-to-ambient through it by designator, °C/W (θJC + θCS + θSA); "
                                  "the part's power splits between the heatsink and the board")
              .num("hTop", "Heat transfer to the air from the top face, W/m²·K (default 10 = still air; 25–50 with a fan)")
              .num("hBottom", "The same from the bottom face (default 10)")
              .num("junctionLimit", "THERMAL_JUNCTION above this junction temperature, °C (default 125)")
              .num("cell", "Grid cell, mm (default from the board size)")
              .boolean("map", "Include the temperature grid (default false)")
              .boolean("image", "Include the heat-map PNG (default true)")
              .integer("width", "Image width in pixels (default 900, at most 2048)"),
          [](McpServer& s, const Json& a) {
              const ThermalResult r = thermalAnalysis(proj(s), thermalOptionsFromJson(a));
              ToolOutput o = out(thermalJson(r, argBool(a, "map", false)));
              if (argBool(a, "image", true) && r.nx > 0) {
                  Json item = obj();
                  item["type"] = "image";
                  item["data"] = base64Encode(renderThermalPng(*P(s), r, argInt(a, "width", 900)));
                  item["mimeType"] = "image/png";
                  o.content.push(item);
              }
              return o;
          });
    t.add("pcb", "si_field_solver", "Field-solver impedance", Kind::Read, false,
          "2D field solver on a stack-up layer's cross-section (Laplace by finite volumes, with and without the "
          "dielectric): Z0, εeff, delay, L and C per mm for a track; with gap also odd / even / differential impedance "
          "and the backward (kb) and forward (kf) crosstalk coefficients; loss in dB/inch from 0.1 to 25 GHz (skin effect "
          "with copper roughness, laminate loss tangent). Within ~1 % of exact stripline results. cpwGap adds coplanar "
          "ground copper beside the track (grounded coplanar waveguide).",
          Schema().integer("layer", "Copper layer (0 = top)").num("width", "Track width, mm").num("gap", "Pair gap, mm (0 = single)")
              .num("cpwGap", "Coplanar waveguide: gap from the track edges to ground copper on the same layer, mm (0 = none)")
              .num("roughness", "Copper RMS roughness, µm (default 1; HVLP ≈ 0.4, standard ED ≈ 1–2)")
              .num("targetOhms", "Goal seek: solve the width for this Z0 (with gap: Zdiff) instead of taking width"),
          [](McpServer& s, const Json& a) {
              double width = argNum(a, "width", 0.2);
              if (a.has("targetOhms")) {
                  width = sieda_field_solve_width(P(s), argInt(a, "layer", 0), argNum(a, "targetOhms", 50), argNum(a, "gap", 0),
                                                  argNum(a, "cpwGap", 0));
                  check(width > 0, "No width from 0.02 to 10 mm reaches that impedance");
              }
              char* r = sieda_field_solve(P(s), argInt(a, "layer", 0), width, argNum(a, "gap", 0), argNum(a, "roughness", 1.0),
                                          argNum(a, "cpwGap", 0));
              check(r != nullptr, "Bad layer, width or gap");
              return out(takeJson(r));
          });
    t.add("pcb", "pcb_copper_thieving", "Copper thieving", Kind::Edit, true,
          "Copper thieving: a staggered grid of square dots in the free area of every copper layer, clear of other "
          "copper, the edge, holes, keep-outs and outer-layer part courtyards; it evens out plating and the mirror "
          "layers' copper (DFM_COPPER_BALANCE). The dots go into the copper Gerbers, IPC-2581 / ODB++ and the 3D view. "
          "Without arguments returns the settings and the dots and copper coverage per layer; pitch 0 turns it off.",
          Schema().num("pitch", "Dot pitch, mm (0 = off; typical 1.5–2.5)").num("size", "Dot size, mm (default 1)")
              .num("clearance", "Clearance to other copper, holes and the courtyards, mm (default 1)"),
          [](McpServer& s, const Json& a) {
              Json set = obj();
              for (const char* k : {"pitch", "size", "clearance"})
                  if (a.has(k)) set[k] = a.get(k);
              char* r = sieda_pcb_thieving(P(s), set.size() == 0 ? nullptr : set.dump().c_str());
              check(r != nullptr, "Invalid thieving settings (pitch 0 or ≥ size + 0.1 mm, size 0.1–10, clearance 0–10)");
              return out(takeJson(r));
          });
    t.add("pcb", "pcb_graphics", "Board artwork and user layers", Kind::Edit, false,
          "Logos and free drawings on the board: filled rectangles and lines on silk-top / silk-bottom, copper-<n> "
          "(netless copper; the DRC keeps it clear of other copper) or user-1 … user-4 (their own Gerbers in the "
          "fabrication package). add: {name, layer, width, rects: [x0,y0,x1,y1,…], lines: [ax,ay,bx,by,…]} or a logo "
          "{name, layer, raster: {rows: [\"0110…\", …] top row first, '1' = ink, x, y (top-left, mm), width (mm)}}; "
          "remove: index. Returns the graphics.",
          Schema().any("add", "Graphic to add").integer("remove", "Index of the graphic to remove"),
          [](McpServer& s, const Json& a) {
              Json cur = takeJson(sieda_pcb_graphics(P(s), nullptr));
              if (a.has("add") || a.has("remove")) {
                  Json next = Json::array();
                  const int drop = a.has("remove") ? argInt(a, "remove", -1) : -1;
                  check(drop < static_cast<int>(cur.size()), "No graphic with that index");
                  for (size_t i = 0; i < cur.size(); ++i)
                      if (static_cast<int>(i) != drop) next.push(cur.items()[i]);
                  if (a.has("add")) next.push(a.get("add"));
                  char* r = sieda_pcb_graphics(P(s), next.dump().c_str());
                  check(r != nullptr, "The graphic needs rects, lines or a raster on silk-top, silk-bottom, copper-<n> or user-1 … user-4");
                  cur = takeJson(r);
              }
              Json o = obj();
              o["graphics"] = cur;
              return out(o);
          });
    t.add("pcb", "pcb_panel", "Production panel", Kind::Edit, true,
          "Panel for the fabrication package: nx × ny boards between rails with fiducials and tooling holes, "
          "separated by V-score lines or routed gaps with breakaway tabs and mouse bites. Without arguments returns "
          "the layout (mm, Y up); 1 × 1 removes the panel.",
          Schema().integer("nx", "Boards across (1 to 20)").integer("ny", "Boards up (1 to 20)")
              .num("gap", "Routed gap between boards and to the rails, mm (tab panels)")
              .num("rail", "Rail width along the top and bottom, mm (0 = none)")
              .boolean("vscore", "V-score instead of tabs and mouse bites")
              .boolean("fit", "Pick nx × ny: the most boards within maxWidth × maxHeight (default: the DFM pack's size limit)")
              .num("maxWidth", "Largest panel width for fit, mm").num("maxHeight", "Largest panel height for fit, mm"),
          [](McpServer& s, const Json& a) {
              if (a.has("nx") || a.has("ny") || a.has("gap") || a.has("rail") || a.has("vscore") || a.get("fit").asBool(false)) {
                  Json cur = takeJson(sieda_pcb_panel(P(s))).get("settings");
                  for (const char* k : {"nx", "ny", "gap", "rail", "vscore"})
                      if (a.has(k)) cur[k] = a.get(k);
                  if (a.get("fit").asBool(false)) {
                      Json fit = obj();
                      if (a.has("maxWidth")) fit["width"] = a.get("maxWidth");
                      if (a.has("maxHeight")) fit["height"] = a.get("maxHeight");
                      cur["fit"] = fit;
                  }
                  check(sieda_pcb_set_panel(P(s), cur.dump().c_str()) == 1, "Invalid panel settings");
              }
              return out(takeJson(sieda_pcb_panel(P(s))));
          });
    t.add("pcb", "pcb_import_idf_placement", "Import MCAD placement", Kind::Edit, false,
          "Moves parts to the placement in an IDF 3.0 board file (.emn) written back by mechanical CAD (position, "
          "rotation, side by designator). Returns the designators moved.",
          Schema().str("path", "The .emn file inside the root", true),
          [](McpServer& s, const Json& a) {
              const std::string emn = readFileArg(s, requireStr(a, "path")).asString();
              return out(takeJson(sieda_import_idf_placement(P(s), emn.c_str())));
          });
    t.add("pcb", "pcb_import_idx", "Import MCAD changes (IDX)", Kind::Edit, false,
          "Applies an IDX (ProSTEP EDMD) baseline or change file written by mechanical CAD: parts moved, rotated and "
          "flipped by designator, the board outline and thickness, routing / via keep-outs and component keep-outs "
          "(height zones). Returns what changed.",
          Schema().str("path", "The .idx file inside the root", true),
          [](McpServer& s, const Json& a) {
              const std::string idx = readFileArg(s, requireStr(a, "path")).asString();
              return out(takeJson(sieda_import_idx(P(s), idx.c_str())));
          });
    t.add("pcb", "pcb_route_compare", "Compare routing strategies", Kind::Edit, false,
          "Routes copies of the board with each strategy (pcb_autoroute presets; default: default, fast and quality) and "
          "compares them with the board as it is now (before): connections left open, DRC errors and warnings, vias and "
          "track length. recommended = fewest open, then fewest DRC errors, warnings, vias, shortest copper. apply: a "
          "strategy name keeps that result on the board (project_undo goes back); otherwise the board is unchanged.",
          Schema().array("strategies", "Preset names to compare", "string").str("apply", "Keep this strategy's result"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              auto tool = [&](const char* name, const Json& args) {
                  for (const auto& t : mcpTools())
                      if (t.name == name) return t.handler(s, args).data;
                  throw ToolError(std::string("missing tool ") + name);
              };
              auto metrics = [&](const char* name) {
                  Json m = obj();
                  int errors = 0, warnings = 0;
                  const Json drc = takeJson(sieda_pcb_run_drc(p));
                  for (const auto& v : drc.items()) {
                      errors += v.get("severity").asString() == "error";
                      warnings += v.get("severity").asString() == "warning";
                  }
                  double length = 0;
                  for (const auto& t : p->project.pcb.tracks) length += trackLength(t);
                  m["strategy"] = name;
                  m["open"] = static_cast<int>(p->project.pcb.ratsnest(p->project.schematic).size());
                  m["drcErrors"] = errors, m["drcWarnings"] = warnings;
                  m["vias"] = static_cast<int>(p->project.pcb.vias.size());
                  m["trackLength"] = std::round(length * 100) / 100;
                  return m;
              };
              std::vector<std::string> names;
              for (const auto& n : a.get("strategies").items()) names.push_back(n.asString());
              if (names.empty()) names = {"default", "fast", "quality"};
              check(names.size() <= 6, "Compare at most 6 strategies");
              const Project before = p->project;
              Json results = Json::array();
              std::map<std::string, Project> routed;
              std::string best;
              std::tuple<int, int, int, int, double> bestKey{0, 0, 0, 0, 0.0};
              for (const auto& n : names) {
                  p->project = before;
                  p->router.reset();
                  Json args = obj();
                  args["preset"] = n;
                  tool("pcb_autoroute", args);
                  Json m = metrics(n.c_str());
                  const std::tuple<int, int, int, int, double> key{m.get("open").asInt(), m.get("drcErrors").asInt(), m.get("drcWarnings").asInt(),
                                                                   m.get("vias").asInt(), m.get("trackLength").asNumber()};
                  if (best.empty() || key < bestKey) best = n, bestKey = key;
                  routed[n] = p->project;
                  results.push(m);
              }
              p->project = before;
              p->router.reset();
              Json j = obj();
              j["before"] = metrics("before");
              j["results"] = results;
              j["recommended"] = best;
              if (a.has("apply")) {
                  const std::string keep = argStr(a, "apply");
                  check(routed.count(keep), "apply must name one of the compared strategies");
                  p->project = routed[keep];
                  p->router.reset();
                  j["applied"] = keep;
              }
              return out(j);
          });
    t.add("pcb", "pcb_import_dxf", "Import board outline (DXF)", Kind::Edit, false,
          "Sets the board outline and cutouts from a DXF drawing: closed loops of lines, arcs, polylines (with bulges) "
          "and circles, on outline-like layers when the file names them (OUTLINE, Edge.Cuts, BOARD …); millimetres, or "
          "inches by $INSUNITS. The largest loop is the outline, loops inside it are cutouts.",
          Schema().str("path", "The .dxf file inside the root", true),
          [](McpServer& s, const Json& a) {
              const std::string dxf = readFileArg(s, requireStr(a, "path")).asString();
              Json j = takeJson(sieda_pcb_import_dxf(P(s), dxf.c_str()));
              if (!j.get("ok").asBool(false)) throw ToolError("DXF not imported: " + j.get("error").asString("unreadable"));
              return out(j);
          });
    t.add("pcb", "pcb_import_routes", "Import routes (PADS / HyperLynx / IPC-2581)", Kind::Edit, false,
          "Takes the routed copper (tracks, arcs, vias) of another tool's board onto this one: PADS ASCII (.asc), "
          "HyperLynx (.hyp) or IPC-2581 (.xml / .cvg, as Allegro and OrCAD PCB Editor export it), detected from the text. "
          "The parts must already be placed as in that design: nets match by name, and the file's frame is fitted to "
          "this board by its named pins (HyperLynx, IPC-2581), else by the track ends that land on pads of their net. Replaces the board's routing unless replace is "
          "false. padHits / ends says how many track ends reach a pad of their net; run pcb_drc after.",
          Schema().str("path", "The route file inside the root").str("text", "The file's text (instead of path)")
              .boolean("replace", "Replace the board's tracks and vias (default true)"),
          [](McpServer& s, const Json& a) {
              const std::string text = a.has("text") ? argStr(a, "text") : readFileArg(s, requireStr(a, "path")).asString();
              Json j = takeJson(sieda_pcb_import_routes(P(s), text.c_str(), argBool(a, "replace", true)));
              if (j.has("error")) throw ToolError("Routes not imported: " + j.get("error").asString());
              return out(j);
          });
    t.add("pcb", "pcb_import_kicad", "Import a KiCad board", Kind::Edit, false,
          "Brings a KiCad board (.kicad_pcb, KiCad 6 – 10) into the project. On an empty schematic it first builds the "
          "circuit, from the project's .kicad_sch when schematic names it, else from the pads' nets (as "
          "schematic_import_netlist, but every part on a board footprint other than an SMD chip passive becomes a "
          "library part with that footprint's exact pads). "
          "Then every part whose designator the schematic has is placed where KiCad has it (side and quarter turn "
          "fitted to its pads), and the board takes KiCad's copper layers, Edge.Cuts outline, tracks, arcs, vias and "
          "copper zones (replacing its own routing). fidelity lists parts whose pads stand more than 0.05 mm from "
          "KiCad's (offBy, mm), designators the schematic lacks and copper whose net could not be matched.",
          Schema().str("path", "The .kicad_pcb file inside the root").str("text", "Or the board's text")
              .str("schematic", "The project's .kicad_sch (inside the root): the circuit comes from it, each part with its board footprint")
              .str("schematicText", "Or that schematic's text")
              .object("sheets", "Its KiCad sub-sheets by file name (else read beside schematic)"),
          [](McpServer& s, const Json& a) {
              const std::string text = a.has("text") ? argStr(a, "text") : readFileArg(s, requireStr(a, "path")).asString();
              KicadBoard kb;
              ParsedNetlist nl;
              try {
                  kb = parseKicadPcb(text);
                  const KicadSheetLoader load = sheetLoader(s, a.get("sheets"), argStr(a, "schematic"));
                  nl = a.has("schematicText") ? parseNetlist(argStr(a, "schematicText"), load)
                     : a.has("schematic")     ? parseNetlist(readFileArg(s, argStr(a, "schematic")).asString(), load)
                                              : kb.netlist;
              } catch (const std::exception& e) {
                  throw ToolError(std::string("Not read: ") + e.what());
              }
              for (auto& np : nl.parts)  // the schematic's parts take their footprint's exact pads from the board
                  for (const auto& bp : kb.netlist.parts)
                      if (bp.ref == np.ref) np.package = bp.package, np.footprint = np.footprint.empty() ? bp.footprint : np.footprint;
              Json j = obj();
              if (proj(s).schematic.components().empty()) j["built"] = importNetlist(s, nl);
              Project& pr = P(s)->project;
              Schematic& sch = pr.schematic;
              PcbLayout& pcb = pr.pcb;
              sieda_pcb_set_layer_count(P(s), kb.layerCount);
              const int bottom = pcb.settings.bottomLayer();
              auto layer = [&](int k) { return k == kb.layerCount - 1 ? bottom : k < bottom ? k : -1; };
              Vec2 origin = kb.edgeMin;
              const bool outline = !kb.outlineDxf.empty() && applyDxfOutline(pcb, importDxfOutline(kb.outlineDxf));
              if (!outline) {  // the parts' extent and 5 mm
                  double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
                  for (const auto& f : kb.footprints)
                      for (const auto& p : f.pads)
                          x0 = std::min(x0, p.at.x), y0 = std::min(y0, p.at.y), x1 = std::max(x1, p.at.x), y1 = std::max(y1, p.at.y);
                  origin = {x0 - 5, y0 - 5};
                  pcb.settings.outline.clear();
                  pcb.settings.width = std::max(10.0, x1 - x0 + 10);
                  pcb.settings.height = std::max(10.0, y1 - y0 + 10);
              }
              pcb.settings.thickness = kb.thickness;
              Json misfit = Json::array(), missing = Json::array();
              int placed = 0;
              for (const auto& f : kb.footprints) {
                  const Component* found = sch.findByRef(f.ref);
                  const FootprintDef* def = found && found->hasFootprint() ? Library::instance().footprint(found->footprintName()) : nullptr;
                  if (!def || def->pads.empty() || f.pads.empty()) {
                      missing.push(f.ref);
                      continue;
                  }
                  Component& c = *sch.find(found->id);
                  auto numberOf = [&](const PadDef& pd) {
                      if (pd.pinIndex < 0 || pd.pinIndex >= static_cast<int>(c.def().pins.size())) return std::string("-");
                      const std::string& n = c.def().pins[static_cast<size_t>(pd.pinIndex)].number;
                      return n.empty() ? std::to_string(pd.pinIndex + 1) : n;
                  };
                  // The quarter turn (and the offset of SiEDA's centred land pattern) that puts its pads on KiCad's.
                  const int pref = (360 - 90 * ((static_cast<int>(std::lround(f.angle / 90.0)) % 4 + 4) % 4)) % 360;
                  double bestErr = 1e300;
                  for (int i = 0; i < 4; ++i) {
                      PcbPlacement pl = c.pcb;
                      pl.rotation = (pref + 90 * i) % 360;
                      pl.bottom = f.bottom;
                      auto at = [&](const PadDef& pd) {
                          Vec2 q = pd.offset;
                          if (pl.bottom) q.x = -q.x;
                          return pl.position + rotate90(q, pl.rotation);
                      };
                      pl.position = {};
                      Vec2 kc, sc;
                      for (const auto& p : f.pads) kc = kc + (p.at - origin) * (1.0 / f.pads.size());
                      for (const auto& pd : def->pads) sc = sc + at(pd) * (1.0 / def->pads.size());
                      pl.position = kc - sc;
                      double err = 0;
                      for (int pass = 0; pass < 2; ++pass) {  // match each KiCad pad to its own number's nearest pad
                          Vec2 shift;
                          err = 0;
                          for (const auto& p : f.pads) {
                              const Vec2 k = p.at - origin;
                              double dBest = 1e300;
                              Vec2 dv;
                              for (bool any : {false, true}) {
                                  for (const auto& pd : def->pads)
                                      if (any || numberOf(pd) == p.number) {
                                          const Vec2 d = k - at(pd);
                                          if (d.length() < dBest) dBest = d.length(), dv = d;
                                      }
                                  if (dBest < 1e299) break;
                              }
                              shift = shift + dv * (1.0 / f.pads.size());
                              err = std::max(err, dBest);
                          }
                          if (pass == 0) pl.position = pl.position + shift;
                      }
                      if (err < bestErr - 1e-6) {
                          bestErr = err;
                          c.pcb.position = pl.position, c.pcb.rotation = pl.rotation, c.pcb.bottom = pl.bottom;
                      }
                  }
                  c.pcb.placed = true;
                  c.pcb.board = pcb.board;
                  ++placed;
                  if (bestErr > 0.05) {
                      Json m = obj();
                      m["ref"] = f.ref, m["offBy"] = std::round(bestErr * 1000) / 1000;
                      misfit.push(m);
                  }
              }
              // KiCad nets → the schematic's, by the pads they reach (else by name).
              std::map<std::string, int> netOf;
              const auto pads = pcb.pads(sch);
              for (const auto& f : kb.footprints) {
                  const Component* c = sch.findByRef(f.ref);
                  for (const auto& p : f.pads) {
                      if (!c || p.net.empty() || netOf.count(p.net)) continue;
                      for (const Pad& sp : pads)
                          if (sp.componentId == c->id && sp.net >= 0 && (sp.position - (p.at - origin)).length() < 0.3) netOf[p.net] = sp.net;
                  }
              }
              const auto& nets = sch.nets();
              auto net = [&](const std::string& name) {
                  if (auto it = netOf.find(name); it != netOf.end()) return it->second;
                  const std::string bare = name.size() > 1 && name[0] == '/' ? name.substr(1) : name;
                  for (size_t i = 0; i < nets.size(); ++i)
                      if (nets[i].name == bare) return static_cast<int>(i);
                  return -1;
              };
              pcb.clearRouting();
              pcb.zones.clear();
              int dropped = 0;
              for (const auto& t : kb.tracks) {
                  Track tr;
                  tr.net = net(t.net), tr.layer = layer(t.layer), tr.width = t.width;
                  tr.a = t.a - origin, tr.b = t.b - origin, tr.arc = t.arc, tr.mid = t.mid - origin;
                  if (tr.net < 0 || tr.layer < 0) ++dropped;
                  else pcb.addTrack(tr);
              }
              for (const auto& v : kb.vias) {
                  Via via;
                  via.net = net(v.net), via.position = v.at - origin, via.diameter = v.diameter, via.drill = v.drill;
                  via.fromLayer = layer(v.from), via.toLayer = v.to < 0 ? -1 : layer(v.to);
                  if (via.net < 0 || via.fromLayer < 0 || (v.to >= 0 && via.toLayer < 0)) ++dropped;
                  else pcb.addVia(via);
              }
              for (const auto& z : kb.zones) {
                  const int n = net(z.net);
                  if (n < 0 || layer(z.layer) < 0) {
                      ++dropped;
                      continue;
                  }
                  CopperZone cz;
                  cz.net = nets[static_cast<size_t>(n)].name, cz.layer = layer(z.layer);
                  for (const Vec2& q : z.polygon) cz.polygon.push_back(q - origin);
                  pcb.zones.push_back(cz);
              }
              j["parts"] = placed, j["tracks"] = static_cast<int>(pcb.tracks.size()), j["vias"] = static_cast<int>(pcb.vias.size());
              j["zones"] = static_cast<int>(pcb.zones.size()), j["layers"] = pcb.settings.layerCount, j["outline"] = outline;
              Json size = Json::array();
              size.push(pcb.settings.width), size.push(pcb.settings.height);
              j["board"] = size;
              Json fidelity = obj(), warnings = Json::array();
              fidelity["misfit"] = misfit, fidelity["missing"] = missing, fidelity["copperDropped"] = dropped;
              j["fidelity"] = fidelity;
              for (const auto& w : kb.netlist.warnings) warnings.push(w);
              if (warnings.size()) j["warnings"] = warnings;
              return out(j);
          });
    t.add("pcb", "pcb_reference_layer", "Reference layer (Gerber)", Kind::Edit, false,
          "Keeps a Gerber or Excellon drill file over the board as a reference layer (origin at the board's lower-left "
          "corner, never fabricated): path adds one; index with visible shows / hides it, with remove deletes it. "
          "Returns the layers with each one's shape counts and bounds (mm).",
          Schema().str("path", "Gerber / drill file inside the root to add").str("name", "Layer name (default: the file name)")
              .integer("index", "Layer to change").boolean("visible", "Show or hide it").boolean("remove", "Delete it"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              if (a.has("path")) {
                  const std::string path = requireStr(a, "path");
                  const std::string text = readFileArg(s, path).asString();
                  const std::string name = a.has("name") ? argStr(a, "name") : path.substr(path.find_last_of('/') + 1);
                  check(sieda_pcb_add_reference_layer(p, name.c_str(), text.c_str()) >= 0,
                        "Not a Gerber or drill file with a drawing (or 32 reference layers already)");
              }
              if (a.has("index"))
                  check(sieda_pcb_set_reference_layer(p, a.get("index").asInt(-1), argBool(a, "visible", true) ? 1 : 0,
                                                      argBool(a, "remove", false) ? 1 : 0) == 1, "No reference layer with that index");
              Json layers = Json::array();
              for (const auto& r : p->project.referenceLayers) {
                  const GerberImage img = parseGerber(r.text);
                  Json j = obj();
                  j["name"] = r.name, j["visible"] = r.visible, j["kind"] = img.kind, j["shapes"] = static_cast<int>(img.shapes.size());
                  Json b = Json::array();
                  for (double v : {img.x0, img.y0, img.x1, img.y1}) b.push(v);
                  j["bounds"] = b;
                  layers.push(j);
              }
              Json j = obj();
              j["referenceLayers"] = layers;
              return out(j);
          });
    t.add("output", "output_idx_response", "IDX response for MCAD", Kind::Files, false,
          "Writes the IDX response to a change file from MCAD: every change it proposes accepted or rejected.",
          Schema().str("changes", "The MCAD change .idx inside the root", true).str("path", "File to write", true)
              .boolean("accept", "Accept (default) or reject the changes"),
          [](McpServer& s, const Json& a) {
              const std::string changes = readFileArg(s, requireStr(a, "changes")).asString();
              const std::string content = takeText(sieda_idx_response(changes.c_str(), a.get("accept").asBool(true) ? 1 : 0));
              Json j = obj();
              j["written"] = s.writeFile(requireStr(a, "path"), content);
              return out(j);
          });
    t.add("output", "output_idx_changes", "IDX changes for MCAD", Kind::Files, false,
          "Writes an IDX SendChanges file with only the parts placed differently from a baseline IDX (the one MCAD "
          "already has). Nothing is written when nothing changed.",
          Schema().str("baseline", "The baseline .idx inside the root", true).str("path", "File to write", true),
          [](McpServer& s, const Json& a) {
              const std::string base = readFileArg(s, requireStr(a, "baseline")).asString();
              const std::string content = takeText(sieda_export_idx_changes(P(s), base.c_str()));
              Json j = obj();
              j["changed"] = !content.empty();
              if (!content.empty()) j["written"] = s.writeFile(requireStr(a, "path"), content);
              return out(j);
          });
    t.add("project", "project_diff", "Compare versions", Kind::Read, true,
          "What changed between two versions of a project: parts added / removed / changed (value, footprint, "
          "placement), nets (pins joined or left, renames), copper per net, board settings and variants. 'after' "
          "defaults to the open project.",
          Schema().str("before", "Older .siedaproj inside the root", true).str("after", "Newer .siedaproj (default: the open project)"),
          [](McpServer& s, const Json& a) {
              const std::string before = readFileArg(s, requireStr(a, "before")).asString();
              const std::string after = a.has("after") ? readFileArg(s, requireStr(a, "after")).asString()
                                                       : takeText(sieda_project_save_json(P(s)));
              Json d = takeJson(sieda_diff_projects(before.c_str(), after.c_str(), 0));
              if (d.has("error")) throw ToolError(d["error"].asString());
              d["text"] = takeText(sieda_diff_projects(before.c_str(), after.c_str(), 1));
              return out(d);
          });
}

// ================================================================================================= render

ToolOutput imageOutput(const std::string& format, const std::string& bytes, const std::string& name) {
    ToolOutput o;
    Json j = obj();
    j["format"] = format;
    j["bytes"] = bytes.size();
    Json item = obj();
    if (format == "png") {
        item["type"] = "image";
        item["data"] = base64Encode(bytes);
        item["mimeType"] = "image/png";
    } else {
        Json res = obj();
        res["uri"] = "sieda://render/" + name + ".svg";
        res["mimeType"] = "image/svg+xml";
        res["text"] = bytes;
        item["type"] = "resource";
        item["resource"] = res;
    }
    o.content.push(item);
    o.data = j;
    return o;
}

void renderTools(Table& t) {
    t.add("render", "render_schematic", "Render schematic", Kind::Read, true,
          "A picture of one schematic sheet: PNG image (default) or SVG. Parts as boxes with designators, values and "
          "pin names, wires, labels, grounds and buses.",
          Schema()
              .str("format", "png (default) or svg", false, {"png", "svg"})
              .integer("sheet", "Sheet id (default the active sheet)")
              .integer("width", "Image width in pixels (default 1000, at most 2048)"),
          [](McpServer& s, const Json& a) {
              const std::string f = argStr(a, "format", "png");
              const int w = argInt(a, "width", 1000);
              const int sheet = argInt(a, "sheet", 0);
              return imageOutput(f, f == "svg" ? renderSchematicSvg(*P(s), sheet, w) : renderSchematicPng(*P(s), sheet, w), "schematic");
          });
    t.add("render", "render_pcb", "Render board", Kind::Read, true,
          "A picture of the board: PNG image (default) or SVG. Outline, pours, tracks by layer (top red, bottom "
          "blue), pads, vias, holes, courtyards with designators and the unrouted ratsnest (yellow).",
          Schema()
              .str("format", "png (default) or svg", false, {"png", "svg"})
              .array("layers", "Copper layers to show (default all; 0 = top)", "integer")
              .integer("width", "Image width in pixels (default 1000, at most 2048)")
              .boolean("pours", "Draw the copper pours (default true; false shows the tracks and pads alone)"),
          [](McpServer& s, const Json& a) {
              std::vector<int> layers;
              if (a.get("layers").isArray())
                  for (const auto& l : a.get("layers").items()) layers.push_back(l.asInt(0));
              const std::string f = argStr(a, "format", "png");
              const int w = argInt(a, "width", 1000);
              const bool pours = argBool(a, "pours", true);
              return imageOutput(f, f == "svg" ? renderPcbSvg(*P(s), layers, w, pours) : renderPcbPng(*P(s), layers, w, pours), "pcb");
          });
    t.add("render", "render_3d", "Render 3D assembly", Kind::Read, true,
          "A shaded 3D picture of the assembled board (PNG): board, copper finish, solder mask, silkscreen and every "
          "part's body, seen from the given side or any angle; parts can be hidden or isolated, lifted off (explode) "
          "or the board cut through (section).",
          Schema()
              .str("view", "iso (default), top, bottom, front, back, left or right", false,
                   {"iso", "top", "bottom", "front", "back", "left", "right"})
              .integer("width", "Image width in pixels (default 1000, at most 2048)")
              .array("hide", "Designators left out (their pads stay bare)", "string")
              .array("isolate", "Show only these designators' bodies", "string")
              .num("explode", "Lift the parts off both sides (0 – 1)")
              .num("section", "Cut through the board at y (mm): everything nearer the eye is removed")
              .boolean("fold", "Fold rigid-flex bends (default true)")
              .num("pitch", "Camera elevation (degrees; overrides the view)").num("yaw", "Camera turn (degrees; overrides the view)"),
          [](McpServer& s, const Json& a) {
              Render3dOptions o;
              const Schematic& sch = P(s)->project.schematic;
              for (const auto& r : a.get("hide").items()) o.hidden.insert(componentId(*P(s), r));
              if (a.get("isolate").isArray() && a.get("isolate").size()) {
                  std::set<int> keep;
                  for (const auto& r : a.get("isolate").items()) keep.insert(componentId(*P(s), r));
                  for (const auto& c : sch.components())
                      if (!keep.count(c.id)) o.hidden.insert(c.id);
              }
              o.explode = argNum(a, "explode", 0), o.fold = argBool(a, "fold", true);
              if (a.has("section")) o.section = argNum(a, "section", 0);
              if (a.has("pitch")) o.pitchDeg = argNum(a, "pitch", 0);
              if (a.has("yaw")) o.yawDeg = argNum(a, "yaw", 0);
              return imageOutput("png", render3dPng(*P(s), argStr(a, "view", "iso"), argInt(a, "width", 1000), o), "board3d");
          });
}

}  // namespace

std::vector<McpTool> buildTools() {
    Table t;
    projectTools(t);
    schematicTools(t);
    libraryTools(t);
    pcbTools(t);
    simTools(t);
    siTools(t);
    verifyTools(t);
    outputTools(t);
    teamTools(t);
    renderTools(t);
    return std::move(t.tools);
}

}  // namespace sieda::mcp





