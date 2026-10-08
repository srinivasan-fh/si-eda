// SiEDA Core — the MCP tool table: every tool's name, group, description, JSON Schema, annotations and handler.
// Handlers drive the C API (sieda_c.h) on the server's project, so the stdio server and the app's live endpoint run
// exactly what the app runs. Data-driven: mcpTools() is this table (docs/MCP.md lists it).
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "McpInternal.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Units.hpp"

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
        for (const auto& comp : s.components()) {
            if (comp.sheet != sheet || comp.packageOnly) continue;
            if (std::fabs(comp.position.x - c.x) < 70 && std::fabs(comp.position.y - c.y) < 70) {
                taken = true;
                break;
            }
        }
        if (!taken) return c;
    }
    return {0, static_cast<double>(s.components().size() * 40)};
}

/// Where a label / ground symbol goes next to a pin: away from the part's centre, 20 units out (on the grid).
Vec2 outsidePin(const Project& p, PinAddress a, double distance) {
    const Schematic& s = p.schematic;
    const Component* c = s.find(a.component);
    const Vec2 pin = s.pinPosition({a.component, a.pin});
    Vec2 d = c ? pin - c->position : Vec2{1, 0};
    if (std::fabs(d.x) >= std::fabs(d.y)) d = {d.x >= 0 ? 1.0 : -1.0, 0};
    else d = {0, d.y >= 0 ? 1.0 : -1.0};
    return {std::round((pin.x + d.x * distance) / 10) * 10, std::round((pin.y + d.y * distance) / 10) * 10};
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
        if (c.hasFootprint()) (c.pcb.placed ? placed : unplaced)++;
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
              .str("drawnBy", "Title block: drawn by"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              if (a.has("name")) sieda_project_set_name(p, argStr(a, "name").c_str());
              if (a.has("requirements")) sieda_project_set_requirements(p, argStr(a, "requirements").c_str());
              if (a.has("industry"))
                  check(sieda_project_set_industry(p, argStr(a, "industry").c_str()) == 1, "Unknown industry profile");
              Json tb = obj();
              for (const char* k : {"title", "company", "revision", "date", "drawnBy"})
                  if (a.has(k)) tb[k] = argStr(a, k);
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
          "wires, nets, board.",
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
              j["availableSections"] = available;
              return out(j);
          });
}

// ================================================================================================= schematic tools

void schematicTools(Table& t) {
    t.add("schematic", "schematic_add_component", "Add component", Kind::Edit, false,
          "Places a built-in component: resistor, capacitor, inductor, diode, led, voltage_source (value \"5\" or "
          "\"SIN(0 1 1k)\" / \"PULSE(0 5 1m)\"), current_source, ground, npn, nmos, opamp (pins IN+, IN-, OUT), switch, "
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
                  check(sieda_set_component_package(p, id, argStr(a, "package").c_str()) == 1,
                        "Unknown package \"" + argStr(a, "package") + "\" for this part (the part was added)");
              return out(componentJson(*p, id));
          });
    t.add("schematic", "schematic_remove_component", "Remove component", Kind::Destructive, false,
          "Deletes a component (and its wires) by designator.",
          Schema().str("ref", "Designator, e.g. \"R3\"", true), [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              check(sieda_remove_component(P(s), id) == 1, "Could not remove " + argStr(a, "ref"));
              Json j = obj();
              j["removed"] = argStr(a, "ref");
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
              check(sieda_set_component_package(P(s), id, argStr(a, "package").c_str()) == 1,
                    "This part has no package \"" + argStr(a, "package") + "\"");
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
          "Moves a part on the schematic (x, y in grid units) and/or turns it (rotation: absolute degrees; rotate: "
          "relative degrees).",
          Schema()
              .str("ref", "Designator", true)
              .num("x", "New x")
              .num("y", "New y")
              .integer("rotation", "Absolute rotation (0, 90, 180, 270)")
              .integer("rotate", "Relative rotation in degrees (multiples of 90)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const int id = componentId(*p, a.get("ref"));
              const Component* c = p->project.schematic.find(id);
              if (a.has("x") || a.has("y"))
                  check(sieda_move_component(p, id, argNum(a, "x", c->position.x), argNum(a, "y", c->position.y)) == 1,
                        "Could not move");
              c = p->project.schematic.find(id);
              int delta = argInt(a, "rotate", 0);
              if (a.has("rotation")) delta = argInt(a, "rotation", 0) - c->rotation;
              delta = ((delta % 360) + 360) % 360;
              if (delta) check(sieda_rotate_component(p, id, delta) == 1, "Could not rotate");
              return out(componentJson(*p, id));
          });
    t.add("schematic", "schematic_connect", "Connect pins", Kind::Edit, false,
          "Draws wires between pins. Give from + to, a chain (each pin wired to the next), or a list of pairs. Pins "
          "are REF.PIN (\"R1.2\", \"D1.A\", \"Q1.B\", \"U1.VCC\", \"U1.14\"). Returns the nets that result.",
          Schema()
              .pin("from", "First pin.")
              .pin("to", "Second pin.")
              .array("chain", "Pins wired one after the other: [\"V1.+\",\"R1.1\"] …", "string")
              .array("connections", "Pairs [[\"R1.2\",\"D1.A\"], …]", "array"),
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
              Json results = Json::array();
              int made = 0;
              for (const auto& [x, y] : pairs) {
                  const PinAddress pa = pinAddress(*p, x), pb = pinAddress(*p, y);
                  const int w = sieda_connect(p, pa.component, pa.pin, pb.component, pb.pin);
                  Json r = obj();
                  r["from"] = pinLabel(*p, pa);
                  r["to"] = pinLabel(*p, pb);
                  r["wire"] = w;
                  if (w < 0) r["note"] = "not connected (same pin, or already joined)";
                  else ++made;
                  const int net = p->project.schematic.netOf({pa.component, pa.pin});
                  r["net"] = netName(p->project, net);
                  results.push(r);
              }
              Json j = obj();
              j["wiresAdded"] = made;
              j["connections"] = results;
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
              if (ids.empty()) throw ToolError("No such wire");
              int removed = 0;
              for (int id : ids) removed += sieda_remove_wire(p, id) == 1;
              Json j = obj();
              j["wiresRemoved"] = removed;
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
              auto place = [&](Vec2 at) {
                  const int id = sieda_add_component(p, static_cast<int>(ComponentKind::NetLabel), net.c_str(), at.x, at.y, 0, nullptr);
                  check(id >= 0, "Could not add the label");
                  if (!scope.empty() && scope != "global")
                      check(sieda_set_label_scope(p, id, scope.c_str(), argInt(a, "targetSheet", 0)) == 1, "Invalid scope");
                  return id;
              };
              if (a.get("pins").isArray() && a.get("pins").size()) {
                  for (const auto& pin : a.get("pins").items()) {
                      const PinAddress pa = pinAddress(*p, pin);
                      const int id = place(outsidePin(p->project, pa, 20));
                      check(sieda_connect(p, pa.component, pa.pin, id, 0) >= 0, "Could not wire the label to " + pinLabel(*p, pa));
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
                  const Vec2 pos = p->project.schematic.pinPosition({pa.component, pa.pin});
                  const Vec2 at{std::round(pos.x / 10) * 10, std::round(pos.y / 10) * 10 + 30};
                  const int g = sieda_add_component(p, static_cast<int>(ComponentKind::Ground), nullptr, at.x, at.y, 0, nullptr);
                  check(g >= 0, "Could not add a ground symbol");
                  check(sieda_connect(p, pa.component, pa.pin, g, 0) >= 0, "Could not wire " + pinLabel(*p, pa) + " to ground");
                  done.push(pinLabel(*p, pa));
              }
              Json j = obj();
              j["grounded"] = done;
              return out(j);
          });
    t.add("schematic", "schematic_set_no_connect", "No-connect flag", Kind::Edit, true,
          "Marks a pin as deliberately unconnected (ERC stops reporting it), or clears the mark.",
          Schema().pin("pin", "The pin.", true).boolean("noConnect", "true to mark (default), false to clear"),
          [](McpServer& s, const Json& a) {
              const PinAddress pa = pinAddress(*P(s), a.get("pin"));
              check(sieda_set_pin_no_connect(P(s), pa.component, pa.pin, argBool(a, "noConnect", true) ? 1 : 0) == 1,
                    "Could not set the flag");
              Json j = obj();
              j["pin"] = pinLabel(*P(s), pa);
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
          "bus and directive errors. Returns counts and the violations (errors first).",
          Schema(), [](McpServer& s, const Json&) { return out(violationsOut(s, sieda_run_erc(P(s)))); });
    t.add("schematic", "schematic_validate", "Circuit validation", Kind::Read, true,
          "Checks values and ratings: E-series values, decoupling, part ratings from the DC operating point.", Schema(),
          [](McpServer& s, const Json&) { return out(violationsOut(s, sieda_run_circuit_validation(P(s)))); });
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
          "Sheet commands: add (name, parent), rename (sheet, name), activate (sheet: new parts go there), remove "
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
              .str("size", "Template size"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string act = requireStr(a, "action");
              const int sheet = argInt(a, "sheet", 0);
              Json j = obj();
              if (act == "add") {
                  const int id = sieda_add_sheet(p, requireStr(a, "name").c_str(), argInt(a, "parent", 0));
                  check(id >= 0, "Could not add the sheet (is the name unique?)");
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
          "Attaches a rule to the net of a pin: a net class, a differential pair (with its X_P / X_N partner), or "
          "its own track width / clearance (mm).",
          Schema()
              .pin("pin", "A pin on the net.", true)
              .str("netClass", "Net class name")
              .boolean("diffPair", "Pair this net with its _P/_N partner")
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
          "query words must all match name / description / category / manufacturer / package; the query also takes "
          "filters as in the app: \"cat:regulators pkg:sot-23 pins:3-5 mfr:ti\". Separate filter arguments do the "
          "same. Empty query: the categories with their part counts.",
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
              std::istringstream in(lowerStr(argStr(a, "query", "")));
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
              for (const auto& p : standardParts()) {
                  const std::string cat = lowerStr(p.category), mfr = lowerStr(p.spec.manufacturer);
                  const std::string pkg = compact(p.spec.package.type + std::to_string(p.spec.package.pinCount));
                  const int pins = static_cast<int>(p.spec.pins.size());
                  bool ok = true;
                  for (const auto& c : cats) ok = ok && cat.find(c) != std::string::npos;
                  for (const auto& m : mfrs) ok = ok && mfr.find(m) != std::string::npos;
                  for (const auto& k : pkgs) ok = ok && pkg.find(k) != std::string::npos;
                  if (range) ok = ok && pins >= lo && pins <= hi;
                  if (!ok) continue;
                  const std::string hay = lowerStr(p.spec.name + " " + p.spec.description + " " + p.category + " " +
                                                   p.spec.manufacturer + " " + p.spec.package.type + "-" +
                                                   std::to_string(p.spec.package.pinCount));
                  for (const auto& w : words) ok = ok && hay.find(w) != std::string::npos;
                  if (!ok) continue;
                  ++total;
                  if (static_cast<int>(list.size()) >= limit) continue;
                  Json e = obj();
                  e["name"] = p.spec.name;
                  e["category"] = p.category;
                  e["manufacturer"] = p.spec.manufacturer;
                  e["description"] = p.spec.description;
                  e["package"] = p.spec.package.type + "-" + std::to_string(p.spec.package.pinCount ? p.spec.package.pinCount : pins);
                  e["pins"] = pins;
                  list.push(e);
              }
              j["total"] = total;
              j["parts"] = list;
              if (total > limit) j["note"] = "More parts match: refine the query or raise limit.";
              return out(j);
          });
    t.add("library", "library_part_details", "Part details", Kind::Read, true,
          "A catalog part's full description: manufacturer, package, units and every pin (number, name, type).",
          Schema().str("name", "Part name from library_search, e.g. \"NE555\", \"AMS1117-3.3\"", true),
          [](McpServer&, const Json& a) {
              const StandardPart* sp = findStandardPart(requireStr(a, "name"));
              if (!sp) throw ToolError("No catalog part \"" + argStr(a, "name") + "\" (library_search finds names)");
              Json j = obj();
              j["name"] = sp->spec.name;
              j["category"] = sp->category;
              j["manufacturer"] = sp->spec.manufacturer;
              j["description"] = sp->spec.description;
              j["refPrefix"] = sp->spec.refPrefix;
              j["defaultValue"] = sp->spec.defaultValue;
              j["datasheet"] = sp->spec.datasheet;
              j["package"] = sp->spec.package.type + "-" + std::to_string(sp->spec.package.pinCount);
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
              .boolean("units", "Place by units (multi-unit parts)"),
          [](McpServer& s, const Json& a) {
              const StandardPart* sp = findStandardPart(requireStr(a, "name"));
              if (!sp) throw ToolError("No catalog part \"" + argStr(a, "name") + "\" (library_search finds names)");
              SiedaProject* p = P(s);
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
          "pitch?, bodySize?}. place: true also puts it on the schematic.",
          Schema()
              .str("name", "Part name / number", true)
              .str("manufacturer", "Manufacturer")
              .str("description", "Description")
              .str("refPrefix", "Designator prefix (default U)")
              .str("defaultValue", "Default value")
              .str("datasheet", "Datasheet URL or file name")
              .object("package", "Package {type, pinCount, pitch, bodySize}", true)
              .array("pins", "Pins [{number, name, type, description}]", "object", true)
              .array("units", "Optional gates [{name, pins:[numbers]}]", "object")
              .boolean("place", "Also place it on the schematic")
              .str("ref", "Designator when placing")
              .num("x", "x when placing")
              .num("y", "y when placing"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const std::string spec = optionsFrom(a, {"place", "ref", "x", "y"});
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
              Json j = obj();
              j["symbol"] = takeJson(sieda_check_symbol(spec.c_str()));
              j["landPattern"] = takeJson(sieda_check_land_pattern(spec.c_str(), argNum(a, "minGap", 0.15)));
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
    for (const auto& t : p.pcb.tracks) length += (t.b - t.a).length();
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

void pcbTools(Table& t) {
    t.add("pcb", "pcb_summary", "Board summary", Kind::Read, true,
          "The board: size, outline, layers, rules, every footprint with position / rotation / side, routing "
          "progress (tracks, vias, unrouted connections), pours, keep-outs and mounting holes.",
          Schema(), [](McpServer& s, const Json&) { return out(pcbSummary(s)); });
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
                  check(sieda_pcb_apply_rule_preset(p, argStr(a, "rulePreset").c_str()) == 1, "Unknown rule preset (pcb_rule_presets)");
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
              .num("param", "Corner radius / arm width (mm)"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
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
          "Shrinks / grows the board to the placed footprints plus a margin (parts and copper move together).",
          Schema().num("margin", "Margin around the parts (mm, default 3)"), [](McpServer& s, const Json& a) {
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
              return out(takeJson(sieda_stackup_json(p)));
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
          "What \"Update PCB from schematic\" would change: footprints to add / remove, nets, pours, rules.", Schema(),
          [](McpServer& s, const Json&) {
              Json j = obj();
              j["changes"] = takeJson(sieda_pcb_eco_preview(P(s)));
              return out(j);
          });
    t.add("pcb", "pcb_update_from_schematic", "Update PCB", Kind::Edit, false,
          "Forward annotation: places the footprints of new parts (Auto Place spots), removes parts that are gone, "
          "carries net rules, records the baseline. keys picks changes from pcb_eco_preview (default all). Then "
          "improve placement with pcb_place_footprint or pcb_autoplace.",
          Schema().array("keys", "Change keys to apply (default all)", "string"), [](McpServer& s, const Json& a) {
              const std::string keys = a.get("keys").isArray() ? a.get("keys").dump() : std::string();
              Json r = takeJson(sieda_apply_pcb_eco(P(s), keys.empty() ? nullptr : keys.c_str()));
              if (r.isObject()) r["board"] = projectSummary(s).get("board");
              return out(r);
          });
    t.add("pcb", "pcb_autoplace", "Auto place", Kind::Edit, false,
          "Automatic placement: all = true re-places every unlocked footprint; false places only those not on the "
          "board yet.",
          Schema().boolean("all", "Re-place everything (default false)"), [](McpServer& s, const Json& a) {
              sieda_pcb_autoplace(P(s), argBool(a, "all", false) ? 1 : 0);
              return out(pcbSummary(s));
          });
    t.add("pcb", "pcb_place_footprint", "Place footprint", Kind::Edit, true,
          "Moves / turns / flips one footprint (centre x, y in mm). Refused when the pose overlaps another part, "
          "leaves the board or hits a hole keep-out (the issues are returned) unless force is true.",
          Schema()
              .str("ref", "Designator", true)
              .num("x", "Centre x (mm)", true)
              .num("y", "Centre y (mm)", true)
              .integer("rotation", "Rotation (0, 90, 180, 270; default unchanged)")
              .boolean("bottom", "Bottom side (default unchanged)")
              .num("grid", "Snap grid (mm; 0 = none)")
              .boolean("force", "Place even if illegal"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
              const int id = componentId(*p, a.get("ref"));
              const Component* c = p->project.schematic.find(id);
              if (!c->hasFootprint()) throw ToolError(c->ref + " has no footprint");
              Json r = takeJson(sieda_pcb_place_footprint(p, id, argNum(a, "x", 0), argNum(a, "y", 0),
                                                          argInt(a, "rotation", c->pcb.rotation), argBool(a, "bottom", c->pcb.bottom) ? 1 : 0,
                                                          argNum(a, "grid", 0), argBool(a, "force", false) ? 1 : 0));
              if (!r.isObject()) throw ToolError("Could not place " + c->ref);
              Json j = obj();
              j["ref"] = c->ref;
              for (const char* k : {"x", "y", "rotation", "bottom", "legal", "committed", "issues"}) j[k] = r.get(k);
              if (!r.get("committed").asBool(false))
                  j["note"] = "Not placed: the pose is illegal (see issues). Try pcb_suggest_placement or force.";
              return out(j);
          });
    t.add("pcb", "pcb_suggest_placement", "Suggest placement", Kind::Read, true,
          "The nearest free legal spot for a footprint, close to the pads it connects to. Nothing changes.",
          Schema().str("ref", "Designator", true).num("grid", "Snap grid (mm, default 0.5)"), [](McpServer& s, const Json& a) {
              const int id = componentId(*P(s), a.get("ref"));
              Json r = takeJson(sieda_pcb_suggest_placement(P(s), id, argNum(a, "grid", 0.5)));
              if (!r.isObject()) throw ToolError("No footprint for " + argStr(a, "ref"));
              Json j = obj();
              for (const char* k : {"x", "y", "rotation", "bottom", "legal", "issues"}) j[k] = r.get(k);
              return out(j);
          });
    t.add("pcb", "pcb_lock_footprint", "Lock footprint", Kind::Edit, true,
          "Locks a footprint where it is (Auto Place and the autorouter's placement keep it) or unlocks it.",
          Schema().str("ref", "Designator", true).boolean("locked", "true locks (default), false unlocks"),
          [](McpServer& s, const Json& a) {
              check(sieda_pcb_lock_footprint(P(s), componentId(*P(s), a.get("ref")), argBool(a, "locked", true) ? 1 : 0) == 1,
                    "Not placed on the board");
              Json j = obj();
              j["ref"] = argStr(a, "ref");
              j["locked"] = argBool(a, "locked", true);
              return out(j);
          });
    t.add("pcb", "pcb_autoroute", "Autoroute", Kind::Edit, false,
          "Routes the board. Unplaced footprints are placed first. preset: default, fast, quality (coupled pairs, "
          "length rules, fewer vias, gloss), fanout, nets, netclass, area. options (merged over the preset): "
          "{coupledPairs, lengthAware, minimizeVias, gloss, arcCorners, arcRadius, teardrops, teardropStyle, fast, "
          "fanoutOnly, nets:[names], netClass, area:{x0,y0,x1,y1}, protectLocked, classLayers, pinSwap}. Returns "
          "routed / failed connections, vias and track length.",
          Schema()
              .str("preset", "Strategy preset (pcb_autoroute_presets)")
              .object("options", "Strategy options (see description)")
              .boolean("clearFirst", "Remove the existing routing first"),
          [](McpServer& s, const Json& a) {
              SiedaProject* p = P(s);
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
              if (argBool(a, "clearFirst", false)) sieda_pcb_clear_routing(p);
              sieda_pcb_autoplace(p, 0);
              Json stats = takeJson(sieda_pcb_autoroute(p));
              Json j = obj();
              j["stats"] = stats;
              j["unroutedConnections"] = p->project.pcb.ratsnest(p->project.schematic).size();
              j["options"] = takeJson(sieda_pcb_autoroute_options(p));
              const Json report = takeJson(sieda_pcb_route_report(p));
              if (report.isObject()) j["metrics"] = report.get("metrics");
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
          "the layer for the net. Pours fill around other copper at the clearance.",
          Schema()
              .str("net", "Net name, e.g. \"GND\"", true)
              .integer("layer", "Copper layer (default 0 = top)")
              .boolean("plane", "Reserve the layer as a plane")
              .num("clearance", "Clearance (mm, default board clearance)"),
          [](McpServer& s, const Json& a) {
              netIndex(proj(s), requireStr(a, "net"));
              const int z = sieda_pcb_add_zone(P(s), argStr(a, "net").c_str(), argInt(a, "layer", 0), argBool(a, "plane", false) ? 1 : 0,
                                               argNum(a, "clearance", 0));
              check(z >= 0, "Could not add the pour (layer out of range?)");
              Json j = obj();
              j["zone"] = z;
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
          "Replaces the routing keep-outs: [{name, x0, y0, x1, y1, layer (-1 = all), tracks, vias}].",
          Schema().array("keepouts", "Keep-out rectangles (mm)", "object", true), [](McpServer& s, const Json& a) {
              Json j = obj();
              j["kept"] = sieda_pcb_set_keepouts(P(s), a.get("keepouts").dump().c_str());
              return out(j);
          });
    t.add("pcb", "pcb_drc", "Design rule check", Kind::Read, true,
          "Runs the DRC (clearances, widths, drills, annular rings, unrouted nets, board edge) and the "
          "design-for-reliability checks. Returns counts and the violations (errors first, mm coordinates).",
          Schema(), [](McpServer& s, const Json&) { return out(violationsOut(s, sieda_pcb_run_drc(P(s)))); });
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
          "Stitches vias where a net's pours overlap on two or more layers (default the ground net, 2 mm pitch), "
          "optionally inside an area.",
          Schema().str("net", "Net (default ground)").num("pitch", "Pitch (mm)").num("x0", "Area x0").num("y0", "Area y0").num("x1", "Area x1").num("y1", "Area y1"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_pcb_stitch_vias(P(s), optionsFrom(a).c_str()));
              Json j = obj();
              for (const char* k : {"ok", "message", "added", "skipped", "error"})
                  if (r.has(k)) j[k] = r.get(k);
              return out(j);
          });
    t.add("pcb", "pcb_length_rules", "Length rules", Kind::Edit, false,
          "Length / phase matching: enable serpentine tuning and tolerances, set per-net length rules "
          "[{net, target, tolerance}] (target 0 removes), match groups [{name, nets:[…], tolerance}], and tune now.",
          Schema()
              .boolean("enabled", "Tune lengths after Auto Route")
              .num("pairSkew", "Intra-pair skew tolerance (mm)")
              .num("busTolerance", "Bus length tolerance (mm)")
              .array("rules", "[{net, target, tolerance}]", "object")
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
                      check(sieda_pcb_set_length_rule(p, requireStr(r, "net").c_str(), argNum(r, "target", 0), argNum(r, "tolerance", 0.5)) == 1,
                            "Invalid length rule");
              if (a.get("groups").isArray())
                  for (const auto& g : a.get("groups").items()) check(sieda_pcb_set_match_group(p, g.dump().c_str()) == 1, "Invalid match group");
              Json j = obj();
              if (argBool(a, "tune", false)) j["netsTuned"] = sieda_pcb_tune_lengths(p);
              j["targets"] = takeJson(sieda_length_targets_json(p));
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
          "f3dB, bandwidth, unity gain, phase margin).",
          Schema()
              .str("start", "Start frequency (default 1)")
              .str("stop", "Stop frequency (default 1MEG)")
              .integer("pointsPerDecade", "Points per decade (default 50)")
              .str("source", "Stimulus source designator (default the AC source rule)")
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Points per curve (default 100; 0 = metrics only)"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_simulate_ac(P(s), optionsFrom(a, {"nets", "maxPoints"}).c_str()));
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
              return out(j);
          });
    t.add("sim", "sim_dc_sweep", "DC sweep", Kind::Read, true,
          "Sweeps a source from start to stop in step and returns net voltages and part currents along the sweep.",
          Schema()
              .str("source", "Source designator, e.g. \"V1\"", true)
              .str("start", "Start value", true)
              .str("stop", "Stop value", true)
              .str("step", "Step", true)
              .array("nets", "Nets to return (default all)", "string")
              .integer("maxPoints", "Points per curve (default 200)"),
          [](McpServer& s, const Json& a) {
              Json r = takeJson(sieda_simulate_dc_sweep(P(s), optionsFrom(a, {"nets", "maxPoints"}).c_str()));
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
              return out(r);
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
              .num("vrmBandwidth", "Regulator loop bandwidth (Hz)"),
          [](McpServer& s, const Json& a) {
              const std::string net = requireStr(a, "net");
              check(sieda_pi_set_rail(P(s), net.c_str(), argNum(a, "ripplePercent", 0), argNum(a, "transientAmps", 0), argNum(a, "dcAmps", 0)) == 1,
                    "Unknown rail");
              if (a.has("vrmROut") || a.has("vrmBandwidth"))
                  check(sieda_pi_set_vrm(P(s), net.c_str(), argNum(a, "vrmROut", 0), argNum(a, "vrmBandwidth", 0)) == 1, "Invalid regulator model");
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
          "drill_npth, ipc356, ipc2581 (IPC-2581C XML), fab_notes, stl, obj. variant: an assembly variant for the assembly outputs.",
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
          "The bill of materials (grouped lines, quantities, MPNs, cost) as JSON, or CSV written to path.",
          Schema().str("format", "json (default) or csv", false, {"json", "csv"}).str("path", "File to write"),
          [](McpServer& s, const Json& a) {
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
          "named body per part), IDF 3.0 board (.emn) or library (.emp), or a mesh (STL, OBJ).",
          Schema().str("path", "File to write, e.g. \"out/board.step\"", true)
              .str("format", "step, idf_board, idf_library, stl (default) or obj", false,
                   {"step", "idf_board", "idf_library", "stl", "obj"}),
          [](McpServer& s, const Json& a) {
              const std::string content = takeText(sieda_export(P(s), argStr(a, "format", "stl").c_str()));
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
          "(rectangles with their own maximum height). Without arguments returns the current limits.",
          Schema().num("maxHeightTop", "Tallest part on the top side (mm, 0 = no limit)")
              .num("maxHeightBottom", "Tallest part on the bottom side (mm, 0 = no limit)")
              .any("zones", "[{name, x0, y0, x1, y1, bottom, maxHeight}] (mm, board coordinates)"),
          [](McpServer& s, const Json& a) {
              if (a.has("maxHeightTop") || a.has("maxHeightBottom") || a.has("zones")) {
                  Json cur = takeJson(sieda_pcb_mechanical_limits(P(s)));
                  if (!cur.isObject()) cur = obj();
                  for (const char* k : {"maxHeightTop", "maxHeightBottom", "zones"})
                      if (a.has(k)) cur[k] = a.get(k);
                  check(sieda_pcb_set_mechanical_limits(P(s), cur.dump().c_str()) == 1, "Invalid limits");
              }
              return out(takeJson(sieda_pcb_mechanical_limits(P(s))));
          });
    t.add("pcb", "pcb_dfm_pack", "Manufacturer rule pack", Kind::Edit, true,
          "Manufacturer DFM / DFA rule packs (JLCPCB, PCBWay, OSH Park, Eurocircuits, IPC Class 3). With pack: selects it "
          "(\"\" = none), tightening the DRC minimums and adding the DFM_* / DFA_* checks to run_drc. Returns the packs and "
          "the board's pack.",
          Schema().str("pack", "Pack id, e.g. jlcpcb-standard (\"\" = none)"),
          [](McpServer& s, const Json& a) {
              if (a.has("pack")) check(sieda_pcb_set_dfm_pack(P(s), argStr(a, "pack").c_str()) == 1, "Unknown pack");
              Json j = obj();
              j["packs"] = takeJson(sieda_dfm_packs_json());
              j["selected"] = takeJson(sieda_project_snapshot(P(s))).get("board").get("dfmPack");
              return out(j);
          });
    t.add("pcb", "si_field_solver", "Field-solver impedance", Kind::Read, false,
          "2D field solver on a stack-up layer's cross-section (Laplace by finite volumes, with and without the "
          "dielectric): Z0, εeff, delay, L and C per mm for a track; with gap also odd / even / differential impedance "
          "and the backward (kb) and forward (kf) crosstalk coefficients; loss in dB/inch from 0.1 to 25 GHz (skin effect "
          "with copper roughness, laminate loss tangent). Within ~1 % of exact stripline results.",
          Schema().integer("layer", "Copper layer (0 = top)").num("width", "Track width, mm").num("gap", "Pair gap, mm (0 = single)")
              .num("roughness", "Copper RMS roughness, µm (default 1; HVLP ≈ 0.4, standard ED ≈ 1–2)"),
          [](McpServer& s, const Json& a) {
              char* r = sieda_field_solve(P(s), argInt(a, "layer", 0), argNum(a, "width", 0.2), argNum(a, "gap", 0),
                                          argNum(a, "roughness", 1.0));
              check(r != nullptr, "Bad layer, width or gap");
              return out(takeJson(r));
          });
    t.add("pcb", "pcb_panel", "Production panel", Kind::Edit, true,
          "Panel for the fabrication package: nx × ny boards between rails with fiducials and tooling holes, "
          "separated by V-score lines or routed gaps with breakaway tabs and mouse bites. Without arguments returns "
          "the layout (mm, Y up); 1 × 1 removes the panel.",
          Schema().integer("nx", "Boards across (1 to 20)").integer("ny", "Boards up (1 to 20)")
              .num("gap", "Routed gap between boards and to the rails, mm (tab panels)")
              .num("rail", "Rail width along the top and bottom, mm (0 = none)")
              .boolean("vscore", "V-score instead of tabs and mouse bites"),
          [](McpServer& s, const Json& a) {
              if (a.has("nx") || a.has("ny") || a.has("gap") || a.has("rail") || a.has("vscore")) {
                  Json cur = takeJson(sieda_pcb_panel(P(s))).get("settings");
                  for (const char* k : {"nx", "ny", "gap", "rail", "vscore"})
                      if (a.has(k)) cur[k] = a.get(k);
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
