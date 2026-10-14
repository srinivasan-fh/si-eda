// SiEDA Core — the MCP tool table: every tool's name, group, description, JSON Schema, annotations and handler.
// Handlers drive the C API (sieda_c.h) on the server's project, so the stdio server and the app's live endpoint run
// exactly what the app runs. Data-driven: mcpTools() is this table (docs/MCP.md lists it).
#include <algorithm>
#include <array>
#include <regex>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
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

/// Lays out parts schematic_build placed: each wire chain's parts side by side with the wired pins facing each other
/// (two-pin parts turned to suit), so the wires run straight; the groups then go in rows with room for net labels.
void layoutBuilt(SiedaProject* p, const std::vector<int>& ids, const Json& wires, const std::set<std::pair<int, int>>& grounded = {},
                 const std::map<std::pair<int, int>, std::string>& pinNet = {}) {
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
    std::stable_partition(links.begin(), links.end(), [&](const auto& l) { return isCrystal(l.first.component) || isCrystal(l.second.component); });
    std::set<int> mine(ids.begin(), ids.end()), done;
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
            const double side = grounded.count({id, i}) ? 140.0
                                : net != pinNet.end() ? std::max(60.0, 10.0 * static_cast<double>(net->second.size()) + 30) : 60.0;
            if (q.x <= body[0] + 0.5) o[0] = std::min(o[0], body[0] - side);
            if (q.x >= body[2] - 0.5) o[2] = std::max(o[2], body[2] + side);
            if (q.y <= body[1] + 0.5) o[1] = std::min(o[1], body[1] - 60);
            if (q.y >= body[3] - 0.5) o[3] = std::max(o[3], body[3] + 60);
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
                double along = 0;
                for (int step = 0; step < 40; ++step) {
                    bool hit = false;
                    for (int id : group)
                        for (const auto& c : sch.components()) {
                            if (in.count(c.id) || c.packageOnly || isNetSymbolKind(c.kind) || c.sheet != sch.find(id)->sheet) continue;
                            const auto m = room(id), q = room(c.id);
                            hit |= m[0] < q[2] && q[0] < m[2] && m[1] < q[3] && q[1] < m[3];
                        }
                    if (!hit) return true;
                    const double next = (step / 2 + 1) * 40.0 * (step % 2 ? -1 : 1);  // either side, further each time
                    for (int id : group) moveBy(id, side * (next - along));
                    along = next;
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
                    if (sch.find(b.component)->def().pins.size() == 2)
                        for (int turn = 0; turn < 3; ++turn) {
                            const Vec2 e = pinOutward(p->project, b);
                            if (upright ? e.y < -0.5 : std::fabs(e.x + d.x) < 0.5 && std::fabs(e.y + d.y) < 0.5) break;
                            sieda_rotate_component(p, b.component, 90);
                        }
                    const Vec2 from = sch.pinPosition({a.component, a.pin}), to = sch.pinPosition({b.component, b.pin});
                    // Clear of the labels on an IC's other pins; close up between two-pin parts.
                    // A crystal stands further out from its IC, so its load capacitors hang clear of the IC's labels.
                    const std::string& ref = sch.find(b.component)->ref;
                    const bool crystal = ref.size() > 1 && ref[0] == 'Y' && std::isdigit(static_cast<unsigned char>(ref[1]));
                    const double run = upright ? 0 : loadCap ? 100 : sch.find(a.component)->def().pins.size() == 2 ? 40 : crystal ? 300 : 120;
                    moveBy(b.component, upright ? Vec2{from.x + 80 - to.x, from.y - 140 - to.y}
                                                : Vec2{from.x + d.x * run - to.x, from.y + d.y * run - to.y},
                           loadCap ? 5 : 10);  // in line with the crystal's pin (5 off the 10 grid)
                    // Two parts on one pin (or a chain folding back): the nearest clear spot, along the pin's side either
                    // way or further out (its pin then gets a label), clear of the group (a crystal's load capacitor
                    // either side of its pin, so it stays under the crystal).
                    auto hitAt = [&](bool labelled) {  // labelled: joined to its pin by labels (their room counts too)
                        const auto o = sch.symbolOutline(*sch.find(b.component)), ro = room(b.component);
                        bool hit = false;
                        for (int other : group) {  // the part it is wired to: bodies apart; any other: label room too
                            const auto q = other == a.component ? sch.symbolOutline(*sch.find(other)) : room(other);
                            const auto& m = other == a.component ? o : ro;
                            const double g = 20;
                            hit |= m[0] < q[2] + g && q[0] < m[2] + g && m[1] < q[3] + g && q[1] < m[3] + g;
                        }
                        // Nor over the labels on the hub's other pins.
                        const Component& h = *sch.find(a.component);
                        for (int j = 0; j < static_cast<int>(h.def().pins.size()) && !hit; ++j) {
                            const auto net = pinNet.find({h.id, j});
                            if ((j == a.pin && !labelled) || net == pinNet.end()) continue;
                            const Vec2 q = sch.pinPosition({h.id, j}), e = pinOutward(p->project, {h.id, j});
                            const Vec2 r = q + e * (std::fabs(e.x) > 0.5 ? 12.0 * static_cast<double>(net->second.size()) + 40 : 80);
                            const std::array<double, 4> m = labelled ? ro : std::array<double, 4>{o[0], o[1] - 14, o[2], o[3] + 14};
                            hit = m[0] < std::max(q.x, r.x) + 12 && std::min(q.x, r.x) - 12 < m[2] &&
                                  m[1] < std::max(q.y, r.y) + 12 && std::min(q.y, r.y) - 12 < m[3];
                        }
                        return hit;
                    };
                    if (hitAt(upright)) {
                        const Vec2 home = sch.find(b.component)->position;
                        const auto o = sch.symbolOutline(*sch.find(b.component));
                        // A step clears the designator above and the value below as well.
                        const double w = std::fabs(d.x) > 0.5 ? o[3] - o[1] + 60 : o[2] - o[0] + 40;
                        const Vec2 side = std::fabs(d.x) > 0.5 ? Vec2{0, 1} : Vec2{1, 0};
                        std::vector<std::pair<double, Vec2>> spots;
                        for (double out : loadCap ? std::vector<double>{0} : std::vector<double>{0, 80, 200, 340})
                            for (int k = -10; k <= 10; ++k)
                                if (k || out > 0) spots.push_back({std::abs(k) * w + out * 1.5, d * out + side * (k * w)});
                        std::stable_sort(spots.begin(), spots.end(), [](const auto& l, const auto& r) { return l.first < r.first; });
                        for (const auto& [cost, off] : spots) {
                            moveBy(b.component, home + off - sch.find(b.component)->position, loadCap ? 5 : 10);
                            if (!hitAt(true)) break;
                        }
                    }
                    done.insert(b.component);
                    group.push_back(b.component);
                    if (crystal) expand(b.component);
                }
        };
        for (size_t g = 0; g < group.size(); ++g) expand(group[g]);
        if (anchor(group)) continue;
        double x0 = 1e18, y0 = 1e18, x1 = -1e18, y1 = -1e18;
        for (int id : group) {
            const auto o = sch.symbolOutline(*sch.find(id));
            x0 = std::min(x0, o[0]), y0 = std::min(y0, o[1]), x1 = std::max(x1, o[2]), y1 = std::max(y1, o[3]);
        }
        rowWidth = std::max(rowWidth, x1 - x0 + 100);  // rows as wide as the widest group: small groups sit side by side
        if (x > 0 && x + (x1 - x0) > rowWidth) x = 0, y += rowHeight + 120, rowHeight = 0;
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
int squareEscapes(Project& pr) {
    auto problems = [&] {
        int n = 0;
        for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info;
        return n;
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
    int was = -1, done = 0;
    for (const Pad& pad : pads) {
        if (byPart[pad.componentId].size() < 8 || pad.round || pad.throughHole) continue;
        const Component* c = pr.schematic.find(pad.componentId);
        const bool tall = pad.size.y > pad.size.x;  // the long axis, pointing away from the part's centre
        const double side = tall ? (pad.position.y >= c->pcb.position.y ? 1 : -1) : (pad.position.x >= c->pcb.position.x ? 1 : -1);
        const Vec2 axis = tall ? Vec2{0, side} : Vec2{side, 0};
        const Vec2 out = pad.position + axis * ((tall ? pad.size.y : pad.size.x) / 2 + 0.25);
        for (size_t i = 0; i < pr.pcb.tracks.size(); ++i) {
            const Track t = pr.pcb.tracks[i];
            if (t.net != pad.net || t.layer != pad.smdLayer || t.arc || t.teardrop || gapTo(t, pad.componentId) >= clr - 1e-6) continue;
            const bool aIn = pad.bounds().contains(t.a), bIn = pad.bounds().contains(t.b);
            if (aIn == bIn) continue;
            const Vec2 far = aIn ? t.b : t.a;
            if ((far - out).dot(axis) < 0) continue;  // turning back past the pad's end: left as routed
            if (was < 0) was = problems();
            const auto before = pr.pcb.tracks;
            Track in = t, on = t;
            in.a = pad.position, in.b = out;
            on.a = out, on.b = far, on.id = -1;
            pr.pcb.tracks[i] = in;
            pr.pcb.addTrack(on);
            const int now = problems();
            if (now <= was && gapTo(in, pad.componentId) >= clr - 1e-6 && gapTo(on, pad.componentId) >= clr - 1e-6) was = now, ++done;
            else pr.pcb.tracks = before;
        }
    }
    // A segment still too close to a pin of the part moves away from it in 0.05 mm steps, its free ends (not on a pad
    // or via) taking the tracks joined there along.
    auto anchored = [&](Vec2 v, int net) {
        for (const auto& pad : pads)
            if (pad.net == net && pad.bounds().contains(v)) return true;
        return std::any_of(pr.pcb.vias.begin(), pr.pcb.vias.end(), [&](const Via& via) { return (via.position - v).length() < 1e-6; });
    };
    for (size_t i = 0; i < pr.pcb.tracks.size(); ++i)
        for (const auto& [part, list] : byPart) {
            const Track t = pr.pcb.tracks[i];
            if (list.size() < 8 || t.arc || t.teardrop || gapTo(t, part) >= clr - 1e-6) continue;
            Vec2 away{0, 0};  // from the nearest other-net pad toward the segment's middle
            double near = 1e9;
            const Vec2 mid = (t.a + t.b) * 0.5;
            for (const Pad* o : list)
                if (o->net != t.net && (o->position - mid).length() < near) near = (o->position - mid).length(), away = mid - o->position;
            if (away.length() < 1e-9) continue;
            away = away * (1 / away.length());
            const bool moveA = !anchored(t.a, t.net), moveB = !anchored(t.b, t.net);
            if (!moveA && !moveB) continue;
            if (was < 0) was = problems();
            const auto before = pr.pcb.tracks;
            bool kept = false;
            for (double d = 0.05; d <= 0.25 + 1e-9 && !kept; d += 0.05) {
                pr.pcb.tracks = before;
                for (auto& o : pr.pcb.tracks) {
                    if (o.net != t.net || o.layer != t.layer) continue;
                    for (Vec2* e : {&o.a, &o.b})
                        if ((moveA && (*e - t.a).length() < 1e-6) || (moveB && (*e - t.b).length() < 1e-6)) *e = *e + away * d;
                }
                if (gapTo(pr.pcb.tracks[i], part) < clr - 1e-6) continue;
                const int now = problems();
                if (now <= was) was = now, kept = true, ++done;
            }
            if (!kept) pr.pcb.tracks = before;
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
    int was = -1, moved = 0;
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
        bool kept = false;
        for (size_t k = 0; k < dirs.size() * 12 && !kept; ++k) {
            const Vec2 dir = dirs[k / 12];
            const double d = 0.05 * static_cast<double>(k % 12 + 1);
            pr.pcb.tracks = tracks;
            pr.pcb.vias[i].position = from + dir * d;
            for (auto& t : pr.pcb.tracks)
                if (t.net == pr.pcb.vias[i].net && !t.arc && !t.teardrop)
                    for (Vec2* e : {&t.a, &t.b})
                        if ((*e - from).length() < 1e-6) *e = pr.pcb.vias[i].position;
            if (worst(pr.pcb.vias[i]).first < 0.1 - 1e-9) continue;
            const int now = problems();
            if (now <= was && pr.pcb.ratsnest(pr.schematic).size() <= open) was = now, kept = true, ++moved;
        }
        if (!kept) pr.pcb.tracks = tracks, pr.pcb.vias[i].position = from;
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
        if (const int now = problems(); now <= was && pr.pcb.ratsnest(pr.schematic).size() <= open) {
            was = now, ++moved, --i;
            continue;
        }
        pr.pcb.tracks = tracks, pr.pcb.vias = vias;
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
    auto drcCount = [&](bool currentOnly) {
        int n = 0;
        for (const auto& v : pr.pcb.runDRC(pr.schematic))
            n += currentOnly ? v.code == "DRC_TRACK_CURRENT" : v.severity != Severity::Info;
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
    const int warned = drcCount(true), errors = drcCount(false);
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
    if (drcCount(true) > warned) pr.pcb.tracks = before;  // a neck too thin for the current: leave the copper as routed
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
        if (drcCount(false) > errors) pr.pcb.tracks = kept;
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

/// The pour keep-out round an antenna end (openEnd's `area` on `side`): its own width and its depth again on either
/// side (a rule of thumb: copper beside the antenna detunes it too), clipped to the board.
Rect antennaKeepout(Rect area, int side, const BoardSettings& st) {
    const double depth = side < 2 ? area.height() : area.width();
    if (side < 2) area.x0 -= depth, area.x1 += depth;
    else area.y0 -= depth, area.y1 += depth;
    area.x0 = std::max(0.0, area.x0), area.y0 = std::max(0.0, area.y0);
    area.x1 = std::min(st.width, area.x1), area.y1 = std::min(st.height, area.y1);
    return area;
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
                         y = !side ? y0 : attempt < 2 ? st.height / 2 + step : st.edgeClearance + cy.height() / 2 + 1 + 0.5 * k;
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
        const double d = pad.round ? trackPointDistance(seg, pad.position) - std::min(pad.size.x, pad.size.y) / 2
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
    auto nearest = [&](int id, Vec2 target, double from, double to) {
        for (double r = from; r <= to; r += 0.5)
            for (int k = 0; k < 16; ++k)
                for (int rot : {0, 90}) {
                    const Vec2 at = target + Vec2{std::cos(k * kPi / 8), std::sin(k * kPi / 8)} * r;
                    if (spaced(p->project, classes, id, at, rot, false) &&
                        takeJson(sieda_pcb_place_footprint(p, id, at.x, at.y, rot, 0, 0.25, 0)).get("committed").asBool(false))
                        return true;
                }
        return false;
    };
    // How far the capacitor's pad on the pin's net ended from the pin (pad centres), for the result.
    auto gap = [&](int cap, int comp, int pinIndex) {
        const auto pads = p->project.pcb.pads(sch);
        const auto pin = std::find_if(pads.begin(), pads.end(), [&](const Pad& q) { return q.componentId == comp && q.pinIndex == pinIndex; });
        double best = 1e9;
        for (const auto& q : pads)
            if (pin != pads.end() && q.componentId == cap && q.net == pin->net) best = std::min(best, (q.position - pin->position).length());
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
                    if ((sch.find(c.id)->pcb.position - pad.position).length() > 3 && nearest(c.id, pad.position, 1.5, 6))
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
                            takeJson(sieda_pcb_place_footprint(p, pr.cap, at.x, at.y, rot, 0, 0.25, 0)).get("committed").asBool(false))
                            return true;
                    }
            return false;
        };
        // Close by the pin, if need be with the small parts there stepping aside; else the nearest free spot.
        if (!within(1.5, 4) && !stepAside(pr.pad->position, 7, [&] { return within(1.5, 4); }) && !within(4.5, 9)) continue;
        // Still far, with a crystal taking the room by the pin: the capacitor first, then the crystal (and its load
        // capacitors) again further out; kept only when all of them find a spot.
        for (const auto& xt : crystals) {
            const auto pads2 = p->project.pcb.pads(sch);
            const auto mine = std::find_if(pads2.begin(), pads2.end(), [&](const Pad& q) { return q.componentId == pr.cap && q.net == pr.pad->net; });
            if (mine == pads2.end() || (mine->position - pr.pad->position).length() <= 3.5 ||
                (sch.find(xt.id)->pcb.position - pr.pad->position).length() > 10)
                continue;
            const Project before = p->project;
            p->project.schematic.find(xt.id)->pcb.placed = false;
            for (const auto& [load, net] : xt.loads) p->project.schematic.find(load)->pcb.placed = false;
            bool ok = within(1.5, 3.5) && nearest(xt.id, xt.pins, 2.5, 9);
            for (const auto& [load, net] : xt.loads)
                for (const auto& q : p->project.pcb.pads(sch))
                    if (ok && q.componentId == xt.id && q.net == net) {
                        ok = nearest(load, q.position, 1.5, 6);
                        break;
                    }
            if (!ok) p->project = before, p->router.reset();
            break;
        }
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
        for (bool series : {true, false}) {  // a series part (an input diode, a fuse) before a connector on the rail
            for (const auto& pad : pads) {
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
        if (!best || (best->position - at).length() <= 6) continue;
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

/// schematic_import_netlist's work on a read netlist (also the start of pcb_import_kicad): the schematic_build result
/// with "imported" (what each designator became).
Json importNetlist(McpServer& s, const ParsedNetlist& nl) {
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
        if (np.part.rfind("kind:", 0) == 0) {  // a schematic import that knows the kind (LTspice)
            part["kind"] = np.part.substr(5);
            static const std::regex plain(R"(^[0-9.]+[a-zA-Z\xC2\xB5]*[0-9]*$)");  // "1k", "2k2", "10µ" — not "{res}"
            if (std::regex_match(np.value, plain)) part["value"] = np.value;
        } else if (kind && (np.package.lands.empty() || !size.empty())) {  // a board's THT passive keeps its exact pads
            part["kind"] = kind;
            if (!np.value.empty() && np.value != np.part) part["value"] = np.value;
            if (!size.empty()) part["package"] = size;
        } else if (const StandardPart* sp = !np.package.lands.empty() ? nullptr : findStandardPart(np.part) ? findStandardPart(np.part)
                                                                                                         : findStandardPart(np.value)) {
            part["part"] = sp->spec.name;
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
          "are REF.PIN (\"R1.2\", \"D1.A\", \"Q1.B\", \"U1.VCC\", \"U1.14\"). Pins on different sheets are joined "
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
              Json j = obj();
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
              .str("format", "Export: header, xdc, qsf or csv", false, {"header", "xdc", "qsf", "csv"})
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
              if (a.has("format")) j["text"] = exportPinMap(proj(s).schematic, ref, argStr(a, "format"));
              check(a.has("format") || a.has("assign") || a.has("path"), "Give format to export, or assign / path to import");
              return out(j);
          });
    t.add("schematic", "schematic_net_label", "Net label", Kind::Edit, false,
          "Names a net: puts a net label beside each given pin and wires it, so every pin with the same label joins "
          "net \"net\" (also across sheets for scope global). Without pins the label is placed at x, y.",
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
                  for (const auto& pin : a.get("pins").items()) {
                      const PinAddress pa = pinAddress(*p, pin);
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
              .array("wires", "Pins wired one after the other: [[\"R1.2\", \"D1.A\"], …]", "array")
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
              for (const Link& l : links) {
                  Json chain = Json::array();
                  chain.push(l.from);
                  chain.push(l.to);
                  layoutWires.push(chain);
              }
              layoutBuilt(P(s), unplaced, layoutWires, grounded, pinNet);
              auto straight = [&](const Link& l) {  // facing pins in line, nothing in between
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 p0 = sch.pinPosition({l.a.component, l.a.pin}), p1 = sch.pinPosition({l.b.component, l.b.pin});
                  const Vec2 d0 = pinOutward(proj(s), l.a), d1 = pinOutward(proj(s), l.b), run = p1 - p0;
                  // A crystal's load capacitor hangs straight down from the crystal's pin (a junction with the IC's wire).
                  const std::string& hub = sch.find(l.a.component)->ref;
                  const bool fromCrystal = hub.size() > 1 && hub[0] == 'Y' && std::isdigit(static_cast<unsigned char>(hub[1]));
                  const double out0 = run.x * d0.x + run.y * d0.y;
                  if ((std::fabs(run.x) > 0.5 && std::fabs(run.y) > 0.5) || run.length() < 1 ||
                      (fromCrystal ? out0 < -0.5 : out0 <= 0) || run.x * d1.x + run.y * d1.y >= 0)
                      return false;
                  for (const auto& c : sch.components()) {  // nothing in the way: a body, a label drawn before or another pin
                      if (c.kind == ComponentKind::Junction || (isNetSymbolKind(c.kind) && c.kind != ComponentKind::NetLabel)) continue;
                      for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
                          const Vec2 q = sch.pinPosition({c.id, i});
                          if ((q - p0).length() > 1 && (q - p1).length() > 1 && pointSegmentDistance(q, p0, p1) < 1) return false;
                      }
                      const auto o = sch.symbolOutline(c);
                      for (int k = 1; k < 20; ++k) {
                          const Vec2 q = p0 + run * (k / 20.0);
                          if (q.x > o[0] + 1 && q.x < o[2] - 1 && q.y > o[1] + 1 && q.y < o[3] - 1) return false;
                      }
                  }
                  return true;
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
                      const bool wired = wiredPins.count(net + "|" + pin.dump()) > 0;  // joined by its wire
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
                  Json args = obj();
                  args["chain"] = chain;
                  run("schematic_connect", args, "wire " + chain.dump());
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
              auto hitsOf = [&](int id) {  // symbols and labels a placed tag overlaps
                  const Schematic& sch = proj(s).schematic;
                  const auto o = textBox(*sch.find(id));
                  int hits = 0;
                  for (const auto& c : sch.components()) {
                      if (c.id == id || c.kind == ComponentKind::Junction) continue;
                      const auto q = textBox(c);
                      hits += o[0] < q[2] && q[0] < o[2] && o[1] < q[3] && q[1] < o[3];
                  }
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
                      const int hits = hitsOf(id);
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
              std::vector<std::tuple<int, std::string, const Link*>> placedTags;
              for (const auto& [net, l] : tags) {
                  std::vector<std::pair<Vec2, int>> poses;
                  const auto [best, fewest] = clearest(net, l, poses);
                  const Schematic& sch = proj(s).schematic;
                  const Vec2 p0 = sch.pinPosition({l->a.component, l->a.pin}), p1 = sch.pinPosition({l->b.component, l->b.pin});
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
                  if (tag < 0 || !hitsOf(tag)) continue;
                  sieda_remove_component(P(s), tag);  // its two wires go with it
                  std::vector<std::pair<Vec2, int>> poses;
                  tag = placeTag(net, l, poses, clearest(net, l, poses).first);
              }
              Json j = obj();
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
          "labels, power symbols; sub-sheets listed in warnings), an LTspice schematic (.asc: R, C, L, sources, diodes, "
          "LEDs), a KiCad netlist (.net), Protel / Tango (Altium, OrCAD, EasyEDA, PADS) or CSV (Net, Designator, Pin). Two-pin R / C / L / D parts become built-in parts (package from the footprint's "
          "size), parts the catalog knows by part name or value come from it, the rest become library parts with the "
          "netlist's pins (package from the footprint name, else a header). Then works as schematic_build; imported "
          "lists what each designator became.",
          Schema().str("text", "The netlist text").str("path", "Or the netlist file inside the root"),
          [](McpServer& s, const Json& a) {
              const std::string text = a.has("text") ? argStr(a, "text") : readFileArg(s, requireStr(a, "path")).asString();
              ParsedNetlist nl;
              try {
                  nl = parseNetlist(text);
              } catch (const std::exception& e) {
                  throw ToolError(std::string("Netlist not read: ") + e.what());
              }
              return out(importNetlist(s, nl));
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
          [](McpServer&, const Json& a) {
              std::vector<std::string> words, cats, pkgs, mfrs;
              int lo = 0, hi = 1 << 30;
              bool range = false;
              // "4 pin", "4-pin", "8 way", "6 position" are a pin count: pins:4.
              static const std::regex count(R"((^|\s)(\d{1,3})\s*-?\s*(pins?|ways?|positions?|pos)\b)");
              std::istringstream in(std::regex_replace(lowerStr(argStr(a, "query", "")), count, "$1pins:$2"));
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
                  return out(j);
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
              std::vector<std::pair<size_t, const StandardPart*>> partial, ranked;  // some of the words / all of them
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
                  size_t named = 0;  // words in its name rank it first ("usb-c": the receptacle before MCUs with USB)
                  for (const auto& w : words) named += wordMatches(lowerStr(p.spec.name), w);
                  ranked.push_back({named, &p});
              }
              std::stable_sort(ranked.begin(), ranked.end(), [](const auto& l, const auto& r) { return l.first > r.first; });
              for (const auto& [n, part] : ranked)
                  if (static_cast<int>(list.size()) < limit) list.push(entry(*part));
              if (!total && !partial.empty()) {  // nothing has every word: the parts with the most of them
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
              return out(j);
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
          "Places a standard catalog part (an IC, regulator, connector …) on the schematic. units: true places a "
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
              const StandardPart* sp = &catalogPart(requireStr(a, "name"));
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
              const std::string value = argStr(a, "value", sp->spec.defaultValue.empty() ? sp->spec.name : sp->spec.defaultValue);
              const std::string ref = argStr(a, "ref", "");
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
    t.add("library", "library_check_part", "Check part spec", Kind::Read, true,
          "Checks a part spec (as library_create_part takes) without adding it: symbol, land pattern and unit checks.",
          Schema().object("spec", "Part spec", true).num("minGap", "Copper gap limit for pads (mm, default 0.15)"),
          [](McpServer&, const Json& a) {
              const std::string spec = a.get("spec").dump();
              // A parametric package (SOIC, QFN …) is checked on the footprint it generates.
              std::string lands = spec;
              try {
                  lands = customPartSpecToJson(landPatternFromFootprint(customPartSpecFromJson(a.get("spec")))).dump();
              } catch (const std::exception&) {}  // an invalid spec: the checks below say why
              Json j = obj();
              j["symbol"] = takeJson(sieda_check_symbol(spec.c_str()));
              j["landPattern"] = takeJson(sieda_check_land_pattern(lands.c_str(), argNum(a, "minGap", 0.15)));
              j["units"] = takeJson(sieda_check_units(spec.c_str()));
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
    if (const auto& st = proj(s).pcb.settings; st.outline.empty() && used < 0.25 * st.width * st.height && used > 0)
        j["boardRoomy"] = "Parts cover " + std::to_string(static_cast<int>(100 * used / (st.width * st.height))) +
                          " % of the board: pcb_fit_board {\"compact\": true} re-places them on the smallest board that fits.";
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
          "before routing, re-places every unlocked part (as pcb_autoplace all) on a board made smaller in 1 mm steps "
          "while every part still fits with no new placement problems and every capacitor / crystal stays by its pins — "
          "for a board whose edge parts leave empty space.",
          Schema().num("margin", "Margin around the parts (mm, default 3)")
              .boolean("compact", "Re-place everything on the smallest board that still fits it (before routing; default false)"),
          [](McpServer& s, const Json& a) {
              Project& pr = P(s)->project;
              if (argBool(a, "compact", false)) {
                  check(pr.pcb.tracks.empty() && pr.pcb.vias.empty(), "compact re-places every part: run it before routing (pcb_clear_routing)");
                  check(pr.pcb.settings.outline.empty(), "compact needs a rectangular board (this one has a drawn outline)");
                  auto problems = [&] {
                      int n = 0;
                      for (const auto& v : pr.pcb.runDRC(pr.schematic)) n += v.severity != Severity::Info && v.code != "DRC_UNROUTED";
                      for (const auto& c : pr.schematic.components()) n += c.hasFootprint() && pr.pcb.onBoard(c) && !c.pcb.placed;
                      return n;
                  };
                  Json place = obj();
                  place["all"] = true;
                  // Each smaller board keeps every capacitor / crystal Auto Place set by its pins at the start.
                  Json placed, bestPlaced;  // the capacitors / crystals by their pins, as Auto Place reports them
                  auto paired = [&] { return (placed = autoplaceTool(s, place).data.get("decouplingPlaced")).size(); };
                  const size_t pairs = paired();
                  auto spread = [&] {  // the capacitors' distances to their pins, summed ("C2 → U1.VCC (2.7 mm)")
                      double mm = 0;
                      for (const auto& m : placed.items())
                          if (const auto at = m.asString("").rfind(" ("); at != std::string::npos) mm += std::atof(m.asString("").c_str() + at + 2);
                      return mm;
                  };
                  const double reach = spread() + 1;
                  bestPlaced = placed;
                  const int allowed = problems();
                  Project best = pr;
                  for (int tries = 0, shrunk = 1; shrunk && tries < 80;) {  // the longer side first, 1 mm at a time
                      shrunk = 0;
                      const bool wide = pr.pcb.settings.width >= pr.pcb.settings.height;
                      for (const bool cutWidth : {wide, !wide}) {
                          const double w = pr.pcb.settings.width - (cutWidth ? 1 : 0), h = pr.pcb.settings.height - (cutWidth ? 0 : 1);
                          if (w < 5 || h < 5 || ++tries > 80) continue;
                          sieda_pcb_set_board(P(s), w, h, pr.pcb.settings.trackWidth, pr.pcb.settings.clearance);
                          if (paired() >= pairs && spread() <= reach && pr.pcb.settings.width == w && pr.pcb.settings.height == h && problems() <= allowed) {
                              best = pr, bestPlaced = placed, shrunk = 1;
                              break;
                          }
                          pr = best;
                      }
                  }
                  pr = best;
                  P(s)->router.reset();
                  Json board = projectSummary(s).get("board");
                  if (bestPlaced.size()) board["decouplingPlaced"] = bestPlaced;
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
              return out(r);
          });
    t.add("pcb", "pcb_autoplace", "Auto place", Kind::Edit, false,
          "Automatic placement: all = true re-places every unlocked footprint (antenna parts and USB / connectors with "
          "an open end first, at the top / bottom edge facing out; edges: false skips that); false places only those "
          "not on the board yet. decoupling (default on with all) gives each IC supply pin the nearest small capacitor (≤ 1 µF; a regulator's "
          "input / output also a bulk one) between its net and ground, one per pin, moves one further than 3 mm to the "
          "nearest free spot by the pin, and puts a crystal (Y…) by the pins it drives with its load capacitors beside "
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
          "Moves / turns / flips footprints (centre x, y in mm): one (ref, x, y …) or many at once (placements: "
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
                  if (!e.has("ref") || !e.has("x") || !e.has("y")) throw ToolError("Each placement needs ref, x and y");
                  const int id = componentId(*p, e.get("ref"));
                  const Component* c = p->project.schematic.find(id);
                  if (!c->hasFootprint()) throw ToolError(c->ref + " has no footprint");
                  const bool force = argBool(e, "force", argBool(a, "force", false)), locked = c->pcb.locked;
                  std::vector<Pad> oldPads;  // its copper before the move: the tracks on them are ripped up
                  if (c->pcb.placed)
                      for (const auto& pad : p->project.pcb.pads(p->project.schematic))
                          if (pad.componentId == id) oldPads.push_back(pad);
                  const PcbPlacement was = c->pcb;
                  if (force && locked) sieda_pcb_lock_footprint(p, id, 0);
                  Json r = takeJson(sieda_pcb_place_footprint(p, id, argNum(e, "x", 0), argNum(e, "y", 0),
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
              .boolean("powerWidths", "Route power / ground nets without a width of their own 0.4 mm wide (default true)"),
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
              if (argBool(a, "clearFirst", false)) sieda_pcb_clear_routing(p);
              sieda_pcb_autoplace(p, 0);
              // Over MCP a pour pad's fan-out via clears its SMD pad (no via in pad) unless options say otherwise;
              // the board's saved strategy keeps its own setting.
              const bool offPads = p->project.pcb.settings.autorouter.viasOffPads;
              p->project.pcb.settings.autorouter.viasOffPads = a.get("options").get("viasOffPads").asBool(true);
              // Unpoured power / ground nets with no width of their own route 0.4 mm wide (a connector-fed board gives
              // the current sizing no source to measure); again at the board's width if that leaves anything open.
              std::vector<std::string> widened;
              if (!ar.scoped() && !ar.fanoutOnly && p->project.pcb.settings.trackWidth < 0.4 && argBool(a, "powerWidths", true))
                  for (const auto& n : p->project.schematic.nets())
                      if (p->project.schematic.netRole(n.index) != NetRole::Signal && !n.name.empty() &&
                          !p->project.pcb.settings.netWidths.count(n.name) && !p->project.pcb.isZoneNet(p->project.schematic, n.index))
                          widened.push_back(n.name);
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
              for (const auto& n : widened) p->project.pcb.settings.netWidths[n] = 0.4;
              Json stats = takeJson(sieda_pcb_autoroute(p));
              Json j = obj();
              Json repairs = finishRouting(p, &stats);
              if (!widened.empty() && !p->project.pcb.ratsnest(p->project.schematic).empty()) {
                  p->project = unrouted;
                  p->router.reset();
                  stats = takeJson(sieda_pcb_autoroute(p));
                  repairs = finishRouting(p, &stats);
                  widened.clear();
              }
              if (!widened.empty()) {
                  std::string list;
                  for (const auto& n : widened) list += (list.empty() ? "" : ", ") + n;
                  j["powerWidths"] = list + " routed 0.4 mm wide (power / ground; pcb_set_net_width changes it, powerWidths: false skips it)";
              }
              if (!a.get("options").has("viasOffPads")) p->project.pcb.settings.autorouter.viasOffPads = offPads;
              if (repairs.size()) j["repairs"] = repairs;
              // No acute joins left by gloss / via minimisation (a branch meeting its net's track at 45° joins square).
              if (!p->project.pcb.settings.autorouter.scoped()) {
                  collapseJogs(p->project);
                  routequality::fixAcuteJoins(p->project.pcb, p->project.schematic);
                  squareEscapes(p->project);
                  if (const int off = viasOffPads(p->project)) j["viasMovedOffPads"] = off;
              }
              if (const Json necked = neckTombstones(p->project); necked.size()) j["tombstoningPrevented"] = necked;
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
                              double largest = 0;  // an exposed pad is its part's largest pad
                              for (const auto& q : pads)
                                  if (q.componentId == pad.componentId) largest = std::max(largest, q.size.x * q.size.y);
                              const Component* c = p->project.schematic.find(pad.componentId);
                              if (c && pad.size.x * pad.size.y >= largest && largest > 4 && said.insert(c->ref).second)
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
              if (restitched) j["stitchingRestored"] = restitched;
              // stats.vias counts the router's own; the board also has fan-out vias into pours and stitching
              if (static_cast<int>(p->project.pcb.vias.size()) != stats.get("vias").asInt(0))
                  j["viasOnBoard"] = static_cast<int>(p->project.pcb.vias.size());
              Json uncoupled = Json::array();  // a differential pair routed as two single nets, and why
              for (const auto& pr : report.get("pairs").items())
                  if (!pr.get("coupled").asBool(false))
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
              if (pairedByName.size()) j["pairedByName"] = pairedByName;  // schematic_net_rules lists the directives
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
          "Deletes tracks and vias by id (ids from project_snapshot sections tracks / vias).",
          Schema().array("tracks", "Track ids", "integer").array("vias", "Via ids", "integer"), [](McpServer& s, const Json& a) {
              int t = 0, v = 0;
              if (a.get("tracks").isArray())
                  for (const auto& id : a.get("tracks").items()) t += sieda_pcb_remove_track(P(s), id.asInt(-1)) == 1;
              if (a.get("vias").isArray())
                  for (const auto& id : a.get("vias").items()) v += sieda_pcb_remove_via(P(s), id.asInt(-1)) == 1;
              Json j = obj();
              j["tracksRemoved"] = t;
              j["viasRemoved"] = v;
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
              const std::string base = argStr(a, "base", "");
              Json r = takeJson(sieda_write_fabrication_package(P(s), dir.c_str(), base.empty() ? nullptr : base.c_str()));
              if (!r.get("ok").asBool(false)) throw ToolError("Writing the package failed: " + r.get("error").asString("?"));
              r["dir"] = dir;
              return out(r);
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
          "drill_npth, ipc356, ipc2581 (IPC-2581C XML), fab_notes, stl, obj, dxf (outline, holes, silk, courtyards), "
          "testpoints (probe point per net, CSV), html_bom (interactive BOM / board viewer), bringup (bring-up "
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
                           "(copper, default 2), dynamic}]"),
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
              .str("schematicText", "Or that schematic's text"),
          [](McpServer& s, const Json& a) {
              const std::string text = a.has("text") ? argStr(a, "text") : readFileArg(s, requireStr(a, "path")).asString();
              KicadBoard kb;
              ParsedNetlist nl;
              try {
                  kb = parseKicadPcb(text);
                  nl = a.has("schematicText") ? parseNetlist(argStr(a, "schematicText"))
                     : a.has("schematic")     ? parseNetlist(readFileArg(s, argStr(a, "schematic")).asString())
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
              .integer("width", "Image width in pixels (default 1000, at most 2048)"),
          [](McpServer& s, const Json& a) {
              std::vector<int> layers;
              if (a.get("layers").isArray())
                  for (const auto& l : a.get("layers").items()) layers.push_back(l.asInt(0));
              const std::string f = argStr(a, "format", "png");
              const int w = argInt(a, "width", 1000);
              return imageOutput(f, f == "svg" ? renderPcbSvg(*P(s), layers, w) : renderPcbPng(*P(s), layers, w), "pcb");
          });
    t.add("render", "render_3d", "Render 3D assembly", Kind::Read, true,
          "A shaded 3D picture of the assembled board (PNG): board, copper finish, solder mask, silkscreen and every "
          "part's body, seen from the given side.",
          Schema()
              .str("view", "iso (default), top, bottom, front, back, left or right", false,
                   {"iso", "top", "bottom", "front", "back", "left", "right"})
              .integer("width", "Image width in pixels (default 1000, at most 2048)"),
          [](McpServer& s, const Json& a) {
              return imageOutput("png", render3dPng(*P(s), argStr(a, "view", "iso"), argInt(a, "width", 1000)), "board3d");
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
