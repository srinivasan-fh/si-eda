// SiEDA Core — Model Context Protocol engine: JSON-RPC 2.0 dispatch, sessions, file sandbox, resources, prompts and
// the built-in examples. The tools are in McpTools.cpp, the renderer in McpRender.cpp. See docs/MCP.md.
#include <algorithm>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include "McpInternal.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Units.hpp"

namespace fs = std::filesystem;

namespace sieda::mcp {

const char* const kProtocolVersion = "2025-06-18";

const std::vector<std::string>& supportedProtocolVersions() {
    static const std::vector<std::string> v = {"2025-06-18", "2025-03-26", "2024-11-05"};
    return v;
}

namespace {

/// A JSON-RPC protocol error (answered as "error", not as a tool result).
struct RpcError : std::runtime_error {
    RpcError(int c, const std::string& m) : std::runtime_error(m), code(c) {}
    int code;
};

constexpr int kParseError = -32700;
constexpr int kInvalidRequest = -32600;
constexpr int kMethodNotFound = -32601;
constexpr int kInvalidParams = -32602;
constexpr int kInternalError = -32603;
constexpr int kResourceNotFound = -32002;

Json errorResponse(const Json& id, int code, const std::string& message) {
    Json e = Json::object();
    e["code"] = code;
    e["message"] = message;
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["error"] = e;
    return r;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Largest file a tool reads (a project, a model, an IDF file): a client cannot make the server load anything bigger.
constexpr uintmax_t kMaxReadBytes = 128u * 1024u * 1024u;

std::string readTextFile(const fs::path& p) {
    std::error_code ec;
    if (fs::file_size(p, ec) > kMaxReadBytes && !ec)
        throw ToolError(p.string() + " is larger than 128 MB, the most a tool reads");
    std::ifstream in(p, std::ios::binary);
    if (!in) throw ToolError("Cannot read " + p.string());
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}

Json textContent(const std::string& text) {
    Json c = Json::object();
    c["type"] = "text";
    c["text"] = text;
    return c;
}

Json toolErrorResult(const std::string& message) {
    Json r = Json::object();
    Json content = Json::array();
    content.push(textContent(message));
    r["content"] = content;
    r["isError"] = true;
    return r;
}

const char* kQuickstart = R"(# SiEDA over MCP — quick start

SiEDA is an electronic design tool: schematic capture, circuit simulation, PCB layout with an autorouter, signal /
power integrity and fabrication outputs. Every tool works on one open project (start with `project_new`,
`project_open` or `project_load_example`; `project_summary` shows where you are).

Units: schematic coordinates are grid units (10 = one grid step, y down); PCB coordinates are millimetres (y down,
origin at the board's top-left corner). Parts are addressed by designator ("R1"), pins as REF.PIN ("R1.2", "D1.A",
"U1.VCC" or a pin number "U1.14").

A typical flow:
1. Schematic: `schematic_add_component` (built-in kinds: resistor, capacitor, inductor, diode, led, voltage_source,
   current_source, ground, npn, nmos, opamp, switch, connector, fuse, battery, ac_source) or `library_search` +
   `library_add_part` for ICs; then `schematic_connect`, `schematic_net_label`, `schematic_connect_to_ground`.
2. Check: `schematic_erc` (fix every error), `sim_dc_op` for voltages and currents, `sim_transient` / `sim_ac`.
3. Board: `pcb_set_board` (size, layers, rules), `pcb_update_from_schematic` (places new footprints),
   optionally `pcb_place_footprint`, then `pcb_autoroute` and `pcb_drc`.
4. Sign-off and outputs: `verify_design`, `output_fabrication_package` / `output_gerbers` / `output_bom`
   (files are written only inside the server's root folder), `render_schematic` / `render_pcb` to look at it.
)";

std::string docName(const fs::path& p) { return lower(p.stem().string()); }

}  // namespace

// ---- options ------------------------------------------------------------------------------------------------------

McpOptions McpOptions::fromJson(const Json& j) {
    McpOptions o;
    if (!j.isObject()) return o;
    o.readOnly = j.get("readOnly").asBool(false);
    o.allowedRoot = j.get("allowedRoot").asString("");
    o.docsDir = j.get("docsDir").asString("");
    o.serverName = j.get("serverName").asString("sieda");
    if (o.serverName.empty()) o.serverName = "sieda";
    if (j.get("toolFilter").isArray())
        for (const auto& t : j.get("toolFilter").items())
            if (t.isString() && !t.asString().empty()) o.toolFilter.push_back(t.asString());
    return o;
}

Json McpOptions::toJson() const {
    Json j = Json::object();
    j["readOnly"] = readOnly;
    j["allowedRoot"] = allowedRoot;
    Json f = Json::array();
    for (const auto& t : toolFilter) f.push(t);
    j["toolFilter"] = f;
    j["docsDir"] = docsDir;
    j["serverName"] = serverName;
    return j;
}

// ---- helpers shared with the tools --------------------------------------------------------------------------------

Json takeJson(char* s) {
    if (!s) return Json();
    std::string text(s);
    sieda_string_free(s);
    if (text.empty()) return Json();
    try {
        return Json::parse(text);
    } catch (const JsonError&) {
        return Json(text);
    }
}

std::string takeText(char* s) {
    if (!s) return std::string();
    std::string text(s);
    sieda_string_free(s);
    return text;
}

std::string argStr(const Json& a, const std::string& key, const std::string& def) {
    const Json& v = a.get(key);
    if (v.isString()) return v.asString();
    if (v.isNumber()) {
        const double d = v.asNumber();
        if (d == std::floor(d) && std::fabs(d) < 1e15) return std::to_string(static_cast<long long>(d));
        return formatEngineeringValue(d, "", 6);
    }
    return def;
}

double argNum(const Json& a, const std::string& key, double def) {
    const Json& v = a.get(key);
    if (v.isNumber()) return v.asNumber();
    if (v.isString()) {
        if (auto parsed = parseEngineeringValue(v.asString())) return *parsed;
        throw ToolError("\"" + key + "\" must be a number (got \"" + v.asString() + "\")");
    }
    return def;
}

int argInt(const Json& a, const std::string& key, int def) {
    const double d = argNum(a, key, def);
    if (!std::isfinite(d)) throw ToolError("\"" + key + "\" must be finite");
    return static_cast<int>(std::lround(d));
}

bool argBool(const Json& a, const std::string& key, bool def) {
    const Json& v = a.get(key);
    if (v.type() == Json::Type::Bool) return v.asBool();
    if (v.isNumber()) return v.asNumber() != 0;
    if (v.isString()) return v.asString() == "true" || v.asString() == "1" || v.asString() == "yes";
    return def;
}

std::string requireStr(const Json& a, const std::string& key) {
    std::string s = argStr(a, key, "");
    if (s.empty()) throw ToolError("\"" + key + "\" is required");
    return s;
}

double argValue(const Json& a, const std::string& key, double def) { return argNum(a, key, def); }

int componentId(const SiedaProject& p, const Json& refOrId) {
    const Schematic& s = p.project.schematic;
    if (refOrId.isNumber()) {
        const int id = refOrId.asInt();
        if (s.find(id)) return id;
        throw ToolError("No component with id " + std::to_string(id));
    }
    const std::string ref = refOrId.asString("");
    if (ref.empty()) throw ToolError("A component designator (\"ref\", e.g. \"R1\") is required");
    if (const Component* c = s.findByRef(ref)) return c->id;
    // Case-insensitive fallback ("r1").
    for (const auto& c : s.components())
        if (lower(c.ref) == lower(ref)) return c.id;
    std::string known;
    int n = 0;
    for (const auto& c : s.components()) {
        if (isNetSymbolKind(c.kind) || c.packageOnly) continue;
        if (n++ < 60) known += (known.empty() ? "" : ", ") + c.ref;
    }
    throw ToolError("No component \"" + ref + "\". Components: " + (known.empty() ? std::string("(none)") : known));
}

PinAddress pinAddress(const SiedaProject& p, const Json& spec) {
    std::string ref, pin;
    if (spec.isObject()) {
        ref = argStr(spec, "ref", "");
        pin = argStr(spec, "pin", "");
    } else if (spec.isString()) {
        const std::string s = spec.asString();
        const size_t dot = s.find('.');
        if (dot == std::string::npos || dot == 0 || dot + 1 >= s.size())
            throw ToolError("Pin \"" + s + "\" must be written REF.PIN, e.g. \"R1.1\" or \"U1.VCC\"");
        ref = s.substr(0, dot);
        pin = s.substr(dot + 1);
    } else {
        throw ToolError("A pin must be a string REF.PIN (e.g. \"R1.1\") or {\"ref\",\"pin\"}");
    }
    PinAddress a;
    a.component = componentId(p, Json(ref));
    a.pin = p.project.schematic.pinIndex(a.component, pin);
    if (a.pin < 0) {
        const Component* c = p.project.schematic.find(a.component);
        std::string names;
        if (c) {
            const auto& pins = c->def().pins;
            for (size_t i = 0; i < pins.size() && i < 80; ++i) {
                names += (names.empty() ? "" : ", ") + pins[i].name;
                if (!pins[i].number.empty() && pins[i].number != pins[i].name) names += " (" + pins[i].number + ")";
            }
        }
        throw ToolError("Component " + ref + " has no pin \"" + pin + "\". Pins: " + names);
    }
    return a;
}

std::string pinLabel(const SiedaProject& p, PinAddress a) {
    const Component* c = p.project.schematic.find(a.component);
    if (!c || a.pin < 0 || a.pin >= static_cast<int>(c->def().pins.size())) return "?";
    return c->ref + "." + c->def().pins[static_cast<size_t>(a.pin)].name;
}

std::string kindName(int kind) {
    static const char* names[] = {"resistor", "capacitor", "inductor", "diode", "led", "voltage_source", "current_source",
                                  "ground", "npn", "nmos", "opamp", "switch", "connector", "ic8", "fuse", "net_label",
                                  "custom", "battery", "ac_source", "junction", "part_unit"};
    return kind >= 0 && kind < kComponentKindCount ? names[kind] : "unknown";
}

int kindFromName(const Json& kind) {
    if (kind.isNumber()) {
        const int k = kind.asInt();
        return Library::isValidKind(k) ? k : -1;
    }
    std::string n;
    for (char c : kind.asString(""))
        if (std::isalnum(static_cast<unsigned char>(c))) n += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    static const std::map<std::string, int> names = {
        {"resistor", 0}, {"r", 0}, {"res", 0}, {"capacitor", 1}, {"c", 1}, {"cap", 1}, {"inductor", 2}, {"l", 2},
        {"diode", 3}, {"d", 3}, {"led", 4}, {"voltagesource", 5}, {"vsource", 5}, {"dcsource", 5}, {"supply", 5},
        {"v", 5}, {"powersupply", 5}, {"currentsource", 6}, {"isource", 6}, {"i", 6}, {"ground", 7}, {"gnd", 7},
        {"npn", 8}, {"bjt", 8}, {"transistor", 8}, {"nmos", 9}, {"mosfet", 9}, {"nmosfet", 9}, {"opamp", 10},
        {"operationalamplifier", 10}, {"switch", 11}, {"sw", 11}, {"connector", 12}, {"header", 12}, {"j", 12},
        {"ic8", 13}, {"ic", 13}, {"fuse", 14}, {"f", 14}, {"netlabel", 15}, {"label", 15}, {"battery", 17}, {"bt", 17},
        {"acsource", 18}, {"vac", 18}, {"ac", 18}, {"junction", 19}};
    auto it = names.find(n);
    return it == names.end() ? -1 : it->second;
}

Json componentJson(const SiedaProject& p, int id, bool withPins) {
    const Schematic& s = p.project.schematic;
    const Component* c = s.find(id);
    if (!c) return Json();
    Json j = Json::object();
    j["id"] = c->id;
    j["ref"] = c->ref;
    j["kind"] = kindName(static_cast<int>(c->kind));
    j["value"] = c->value;
    if (c->kind == ComponentKind::Custom || c->kind == ComponentKind::PartUnit) j["part"] = c->customPart;
    j["x"] = c->position.x;
    j["y"] = c->position.y;
    j["rotation"] = c->rotation;
    j["sheet"] = c->sheet;
    if (c->hasFootprint()) j["footprint"] = c->footprintName();
    if (withPins) {
        Json pins = Json::array();
        const auto& defs = c->def().pins;
        const auto& nets = s.nets();
        for (size_t i = 0; i < defs.size(); ++i) {
            Json pj = Json::object();
            pj["name"] = defs[i].name;
            if (!defs[i].number.empty()) pj["number"] = defs[i].number;
            const int n = s.netOf({c->id, static_cast<int>(i)});
            const bool connected = n >= 0 && s.isPinConnected({c->id, static_cast<int>(i)});
            pj["net"] = connected && n < static_cast<int>(nets.size()) ? Json(nets[static_cast<size_t>(n)].name) : Json();
            if (c->isNoConnect(static_cast<int>(i))) pj["noConnect"] = true;
            pins.push(pj);
        }
        j["pins"] = pins;
    }
    if (c->pcb.placed) {
        Json pc = Json::object();
        pc["x"] = c->pcb.position.x;
        pc["y"] = c->pcb.position.y;
        pc["rotation"] = c->pcb.rotation;
        pc["bottom"] = c->pcb.bottom;
        if (c->pcb.locked) pc["locked"] = true;
        j["pcb"] = pc;
    }
    return j;
}

Json summariseViolations(const SiedaProject& p, const Json& violations, size_t limit) {
    Json out = Json::object();
    if (violations.isObject() && violations.has("error")) {
        out["error"] = violations.get("error");
        return out;
    }
    int errors = 0, warnings = 0, infos = 0;
    Json list = Json::array();
    if (violations.isArray()) {
        // Errors first, then warnings, then infos.
        std::vector<const Json*> sorted;
        for (const auto& v : violations.items()) sorted.push_back(&v);
        auto rank = [](const Json* v) {
            const std::string s = v->get("severity").asString("");
            return s == "error" ? 0 : s == "warning" ? 1 : 2;
        };
        std::stable_sort(sorted.begin(), sorted.end(), [&](const Json* a, const Json* b) { return rank(a) < rank(b); });
        for (const Json* v : sorted) {
            const std::string sev = v->get("severity").asString("");
            if (sev == "error") ++errors;
            else if (sev == "warning") ++warnings;
            else ++infos;
            if (list.size() >= limit) continue;
            Json j = Json::object();
            j["severity"] = sev;
            j["code"] = v->get("code");
            j["message"] = v->get("message");
            Json refs = Json::array();
            if (v->get("components").isArray())
                for (const auto& id : v->get("components").items())
                    if (const Component* c = p.project.schematic.find(id.asInt())) refs.push(c->ref);
            if (refs.size()) j["refs"] = refs;
            if (v->get("hasLocation").asBool(false)) {
                j["x"] = v->get("x");
                j["y"] = v->get("y");
            }
            list.push(j);
        }
    }
    out["errors"] = errors;
    out["warnings"] = warnings;
    out["infos"] = infos;
    out["clean"] = errors == 0;
    out["violations"] = list;
    if (static_cast<size_t>(errors + warnings + infos) > limit) out["truncated"] = true;
    return out;
}

std::string validateArguments(const Json& schema, const Json& args) {
    if (!args.isObject()) return "arguments must be a JSON object";
    const Json& props = schema.get("properties");
    if (schema.get("required").isArray())
        for (const auto& r : schema.get("required").items()) {
            const Json& v = args.get(r.asString(""));
            if (v.isNull() || (v.isString() && v.asString().empty()))
                return "missing required argument \"" + r.asString("") + "\"";
        }
    if (!props.isObject()) return "";
    for (const auto& [key, value] : args.fields()) {
        const Json& p = props.get(key);
        if (!p.isObject() || value.isNull()) continue;
        const std::string type = p.get("type").asString("");
        bool ok = true;
        if (type == "string") ok = value.isString() || value.isNumber();
        else if (type == "number") ok = value.isNumber() || (value.isString() && parseEngineeringValue(value.asString()));
        else if (type == "integer")
            ok = (value.isNumber() && value.asNumber() == std::floor(value.asNumber())) ||
                 (value.isString() && parseEngineeringValue(value.asString()));
        else if (type == "boolean") ok = value.type() == Json::Type::Bool;
        else if (type == "array") ok = value.isArray();
        else if (type == "object") ok = value.isObject();
        if (!ok) return "argument \"" + key + "\" must be of type " + type;
        if (p.get("enum").isArray() && value.isString()) {
            bool found = false;
            std::string options;
            for (const auto& e : p.get("enum").items()) {
                if (e.asString("") == value.asString()) found = true;
                options += (options.empty() ? "" : ", ") + e.asString("");
            }
            if (!found) return "argument \"" + key + "\" must be one of: " + options;
        }
    }
    return "";
}

// ---- tool table ---------------------------------------------------------------------------------------------------

const std::vector<McpTool>& mcpTools() {
    static const std::vector<McpTool> tools = buildTools();
    return tools;
}

const std::vector<std::pair<std::string, std::string>>& mcpToolGroups() {
    static const std::vector<std::pair<std::string, std::string>> groups = {
        {"project", "Create, open, save and inspect projects; built-in examples."},
        {"schematic", "Schematic capture: parts, wires, labels, sheets, buses, annotation, search, ERC, variants."},
        {"library", "Standard-part catalog search, custom parts from pin lists, KiCad / Eagle library import."},
        {"pcb", "Board outline, stack-up, rules, Update PCB, placement, autorouter, interactive routes, pours, DRC."},
        {"sim", "Circuit simulation: DC, transient, AC, noise, sweeps, Monte Carlo, FFT, measurements, SPICE models."},
        {"si_pi", "Signal and power integrity: impedance, lengths, crosstalk, channels, PDN, memory design checks."},
        {"verify", "Design verification and domain (industry) checks."},
        {"output", "Fabrication outputs: Gerbers, drill, BOM, pick-and-place, schematic PDF, 3D models."},
        {"render", "Pictures of the schematic and the board (PNG or SVG)."},
    };
    return groups;
}

std::string mcpToolsMarkdown() {
    std::string md;
    for (const auto& [group, about] : mcpToolGroups()) {
        int n = 0;
        for (const auto& t : mcpTools()) n += t.group == group;
        md += "### " + group + " (" + std::to_string(n) + ")\n\n" + about + "\n\n| Tool | Kind | What it does |\n|---|---|---|\n";
        for (const auto& t : mcpTools()) {
            if (t.group != group) continue;
            std::string first = t.description;
            const size_t stop = first.find(". ");
            if (stop != std::string::npos) first = first.substr(0, stop + 1);
            for (auto& c : first)
                if (c == '|' || c == '\n') c = ' ';
            const char* kind = t.readOnly ? "read" : (t.mutates ? (t.destructive ? "edit (destructive)" : "edit") : "writes files");
            md += "| `" + t.name + "` | " + kind + " | " + first + " |\n";
        }
        md += "\n";
    }
    return md;
}

// ---- examples -----------------------------------------------------------------------------------------------------

const std::vector<McpExample>& mcpExamples() {
    static const std::vector<McpExample> examples = {
        {"led_indicator", "LED indicator", "5 V supply, 330 Ω resistor and a red LED to ground (4 parts, 30 × 20 mm board)."},
        {"transistor_switch", "Transistor LED driver",
         "A logic-level input switches an LED through an NPN transistor (BC847) from 5 V."},
        {"rc_lowpass", "RC low-pass filter", "1 kHz sine source into a 1 kΩ / 100 nF low-pass (f3dB ≈ 1.6 kHz); net OUT."},
        {"opamp_noninverting", "Non-inverting amplifier", "Ideal op-amp with gain 2 (10 kΩ / 10 kΩ) from a 0.5 V input; net OUT."},
        {"ne555_blinker", "NE555 LED blinker", "9 V battery, NE555 astable (10 kΩ, 47 kΩ, 10 µF ≈ 1.4 Hz) driving an LED."},
    };
    return examples;
}

bool loadExample(Project& p, const std::string& id) {
    Project fresh;
    auto& s = fresh.schematic;
    auto pin = [&](int c, const char* n) { return PinRef{c, s.pinIndex(c, n)}; };
    auto wire = [&](int a, const char* pa, int b, const char* pb) { s.connect(pin(a, pa), pin(b, pb)); };
    if (id == "led_indicator") {
        fresh.name = "LED indicator";
        int v = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "330", {80, -40});
        int d = s.addComponent(ComponentKind::LED, "Red", {160, -40});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        wire(v, "+", r, "1");
        wire(r, "2", d, "A");
        wire(d, "K", g, "GND");
        wire(v, "-", g, "GND");
        fresh.pcb.settings.width = 30;
        fresh.pcb.settings.height = 20;
    } else if (id == "transistor_switch") {
        fresh.name = "Transistor LED driver";
        int j = s.addComponent(ComponentKind::VoltageSource, "5", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int rb = s.addComponent(ComponentKind::Resistor, "10k", {100, 0});
        int rc = s.addComponent(ComponentKind::Resistor, "330", {200, -80});
        int d = s.addComponent(ComponentKind::LED, "Red", {280, -80});
        int q = s.addComponent(ComponentKind::NPN, "BC847", {200, 0});
        wire(j, "+", rb, "1");
        wire(rb, "2", q, "B");
        wire(j, "+", rc, "1");
        wire(rc, "2", d, "A");
        wire(d, "K", q, "C");
        wire(q, "E", g, "GND");
        wire(j, "-", g, "GND");
        fresh.pcb.settings.width = 30;
        fresh.pcb.settings.height = 22;
    } else if (id == "rc_lowpass") {
        fresh.name = "RC low-pass filter";
        int v = s.addComponent(ComponentKind::VoltageSource, "SIN(0 1 1k)", {0, 0});
        int r = s.addComponent(ComponentKind::Resistor, "1k", {80, -40});
        int c = s.addComponent(ComponentKind::Capacitor, "100n", {160, 0}, 90);
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int g2 = s.addComponent(ComponentKind::Ground, "", {160, 80});
        int out = s.addComponent(ComponentKind::NetLabel, "OUT", {200, -40});
        wire(v, "+", r, "1");
        wire(r, "2", c, "1");
        wire(c, "2", g2, "GND");
        wire(v, "-", g, "GND");
        wire(c, "1", out, "N");
        fresh.pcb.settings.width = 30;
        fresh.pcb.settings.height = 20;
    } else if (id == "opamp_noninverting") {
        fresh.name = "Non-inverting amplifier";
        int v = s.addComponent(ComponentKind::VoltageSource, "0.5", {0, 0});
        int u = s.addComponent(ComponentKind::OpAmp, "LM358", {120, 0});
        int rf = s.addComponent(ComponentKind::Resistor, "10k", {120, -60});
        int rg = s.addComponent(ComponentKind::Resistor, "10k", {40, -60});
        int rl = s.addComponent(ComponentKind::Resistor, "10k", {220, 40}, 90);
        int g = s.addComponent(ComponentKind::Ground, "", {0, 80});
        int g2 = s.addComponent(ComponentKind::Ground, "", {-20, -60});
        int g3 = s.addComponent(ComponentKind::Ground, "", {220, 100});
        int out = s.addComponent(ComponentKind::NetLabel, "OUT", {200, 0});
        wire(v, "+", u, "IN+");
        wire(v, "-", g, "GND");
        wire(u, "IN-", rg, "2");
        wire(rg, "1", g2, "GND");
        wire(u, "IN-", rf, "1");
        wire(rf, "2", u, "OUT");
        wire(u, "OUT", rl, "1");
        wire(rl, "2", g3, "GND");
        wire(u, "OUT", out, "N");
        fresh.pcb.settings.width = 35;
        fresh.pcb.settings.height = 25;
    } else if (id == "ne555_blinker") {
        const StandardPart* sp = findStandardPart("NE555");
        if (!sp) return false;
        fresh.name = "NE555 LED blinker";
        const std::string part = fresh.addCustomPart(sp->spec);
        int b = s.addComponent(ComponentKind::Battery, "9", {0, 0});
        int g = s.addComponent(ComponentKind::Ground, "", {0, 100});
        int u = s.addCustomComponent(part, "NE555", {160, 0});
        fresh.customLibrary.push_back(part);
        int r1 = s.addComponent(ComponentKind::Resistor, "10k", {60, -80}, 90);
        int r2 = s.addComponent(ComponentKind::Resistor, "47k", {60, -20}, 90);
        int c1 = s.addComponent(ComponentKind::Capacitor, "10u", {60, 60}, 90);
        int c2 = s.addComponent(ComponentKind::Capacitor, "10n", {260, 60}, 90);
        int r3 = s.addComponent(ComponentKind::Resistor, "470", {280, -20});
        int d = s.addComponent(ComponentKind::LED, "Red", {360, -20});
        int g2 = s.addComponent(ComponentKind::Ground, "", {260, 120});
        int g3 = s.addComponent(ComponentKind::Ground, "", {420, 40});
        int g4 = s.addComponent(ComponentKind::Ground, "", {60, 120});
        wire(b, "+", u, "VCC");
        wire(b, "-", g, "GND");
        wire(u, "GND", g, "GND");
        wire(u, "RESET", u, "VCC");
        wire(r1, "1", u, "VCC");
        wire(r1, "2", u, "DISCH");
        wire(r2, "1", u, "DISCH");
        wire(r2, "2", u, "THRES");
        wire(u, "THRES", u, "TRIG");
        wire(c1, "1", u, "TRIG");
        wire(c1, "2", g4, "GND");
        wire(u, "CONT", c2, "1");
        wire(c2, "2", g2, "GND");
        wire(u, "OUT", r3, "1");
        wire(r3, "2", d, "A");
        wire(d, "K", g3, "GND");
        fresh.pcb.settings.width = 40;
        fresh.pcb.settings.height = 30;
    } else {
        return false;
    }
    fresh.schematicChanged();
    p = std::move(fresh);
    return true;
}

// ---- server -------------------------------------------------------------------------------------------------------

McpServer::McpServer(McpOptions options) : options_(std::move(options)) {
    project_ = sieda_project_new("Untitled");
    ownsProject_ = true;
    protocol_ = kProtocolVersion;
}

McpServer::~McpServer() {
    if (ownsProject_ && project_) sieda_project_free(project_);
}

void McpServer::attachProject(SiedaProject* project) {
    if (project == project_) return;
    if (ownsProject_ && project_) sieda_project_free(project_);
    if (project) {
        project_ = project;
        ownsProject_ = false;
    } else {
        project_ = sieda_project_new("Untitled");
        ownsProject_ = true;
    }
    projectPath_.clear();
}

std::string McpServer::resolvePath(const std::string& path, bool write) const {
    if (options_.allowedRoot.empty())
        throw ToolError("File access is disabled: this server has no root folder (start sieda-mcp with --root <folder>). "
                        "Results are still returned inline.");
    if (write && options_.readOnly) throw ToolError("The server is read-only: writing files is refused.");
    if (path.empty()) throw ToolError("A path is required.");
    if (path.find('\0') != std::string::npos) throw ToolError("Invalid path.");
    const fs::path given(path);
    for (const auto& part : given)
        if (part == "..") throw ToolError("Paths may not contain \"..\": " + path);
    std::error_code ec;
    const fs::path root = fs::weakly_canonical(fs::absolute(fs::path(options_.allowedRoot), ec), ec);
    if (ec || !fs::is_directory(root, ec)) throw ToolError("The root folder " + options_.allowedRoot + " does not exist.");
    const fs::path full = given.is_absolute() ? given : root / given;
    const fs::path canon = fs::weakly_canonical(full, ec);  // resolves symbolic links of the existing part
    if (ec) throw ToolError("Invalid path: " + path);
    const fs::path rel = canon.lexically_relative(root);
    if (rel.empty() || *rel.begin() == "..")
        throw ToolError("Access outside the root folder is refused: " + path + " (root: " + root.string() + ")");
    return canon.string();
}

std::string McpServer::writeFile(const std::string& path, const std::string& content) const {
    const std::string full = resolvePath(path, true);
    std::error_code ec;
    const fs::path p(full);
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    if (fs::is_directory(p, ec)) throw ToolError(path + " is a folder");
    std::ofstream out(p, std::ios::binary | std::ios::trunc);
    if (!out) throw ToolError("Cannot write " + full);
    out << content;
    out.close();
    if (!out) throw ToolError("Writing " + full + " failed");
    return full;
}

void McpServer::openProject(const std::string& path) {
    const std::string full = resolvePath(path, false);
    std::error_code ec;
    if (!fs::is_regular_file(full, ec)) throw ToolError("No project file " + full);
    const std::string text = readTextFile(full);
    Project loaded;
    try {
        loaded = Project::fromJson(Json::parse(text));
    } catch (const std::exception& e) {
        throw ToolError(std::string("Not a SiEDA project: ") + e.what());
    }
    project_->router.reset();
    project_->project = std::move(loaded);
    projectPath_ = full;
}

bool McpServer::toolEnabled(const McpTool& tool) const {
    if (options_.toolFilter.empty()) return true;
    for (const auto& f : options_.toolFilter) {
        if (f == tool.name || f == tool.group) return true;
        if (!f.empty() && f.back() == '*' && tool.name.compare(0, f.size() - 1, f, 0, f.size() - 1) == 0) return true;
    }
    return false;
}

Json McpServer::toolsListJson() const {
    Json list = Json::array();
    for (const auto& t : mcpTools()) {
        if (!toolEnabled(t)) continue;
        Json j = Json::object();
        j["name"] = t.name;
        j["title"] = t.title;
        std::string desc = t.description;
        if (options_.readOnly && t.mutates) desc += " (Unavailable: this server is read-only.)";
        j["description"] = desc;
        j["inputSchema"] = t.inputSchema;
        Json a = Json::object();
        a["title"] = t.title;
        a["readOnlyHint"] = t.readOnly;
        a["destructiveHint"] = t.destructive;
        a["idempotentHint"] = t.idempotent;
        a["openWorldHint"] = false;
        j["annotations"] = a;
        list.push(j);
    }
    Json r = Json::object();
    r["tools"] = list;
    return r;
}

Json McpServer::callTool(const std::string& name, const Json& arguments) {
    const McpTool* tool = nullptr;
    for (const auto& t : mcpTools())
        if (t.name == name && toolEnabled(t)) tool = &t;
    if (!tool) throw std::out_of_range("Unknown tool: " + name);
    if (options_.readOnly && tool->mutates)
        return toolErrorResult("Refused: " + name + " changes the design, and this server is read-only.");
    const Json args = arguments.isNull() ? Json::object() : arguments;
    const std::string problem = validateArguments(tool->inputSchema, args);
    if (!problem.empty()) return toolErrorResult("Invalid arguments for " + name + ": " + problem);
    ToolOutput out;
    try {
        out = tool->handler(*this, args);
    } catch (const ToolError& e) {
        if (tool->mutates && onChange_) onChange_(name);
        return toolErrorResult(e.what());
    } catch (const JsonError& e) {
        return toolErrorResult(std::string("Invalid JSON: ") + e.what());
    } catch (const std::exception& e) {
        if (tool->mutates && onChange_) onChange_(name);
        return toolErrorResult(std::string("Internal error in ") + name + ": " + e.what());
    }
    if (tool->mutates && onChange_) onChange_(name);
    Json content = Json::array();
    content.push(textContent(out.data.isString() ? out.data.asString() : out.data.dump()));
    if (out.content.isArray())
        for (const auto& c : out.content.items()) content.push(c);
    Json r = Json::object();
    r["content"] = content;
    if (out.data.isObject() && protocol_ >= std::string("2025-06-18")) r["structuredContent"] = out.data;
    r["isError"] = false;
    return r;
}

// ---- resources ----------------------------------------------------------------------------------------------------

namespace {
Json resourceEntry(const std::string& uri, const std::string& name, const std::string& title, const std::string& desc,
                   const std::string& mime) {
    Json j = Json::object();
    j["uri"] = uri;
    j["name"] = name;
    j["title"] = title;
    j["description"] = desc;
    j["mimeType"] = mime;
    return j;
}

std::vector<fs::path> docFiles(const std::string& dir) {
    std::vector<fs::path> files;
    if (dir.empty()) return files;
    std::error_code ec;
    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().extension() == ".md") files.push_back(it->path());
    std::sort(files.begin(), files.end());
    return files;
}

std::string catalogDigest() {
    std::string md = "# SiEDA part catalog (digest)\n\n## Built-in kinds (schematic_add_component)\n\n"
                     "| kind | prefix | default value | pins | footprint |\n|---|---|---|---|---|\n";
    for (const auto& d : Library::instance().components()) {
        if (d.kind == ComponentKind::Custom || d.kind == ComponentKind::PartUnit || d.kind == ComponentKind::Junction) continue;
        std::string pins;
        for (const auto& p : d.pins) pins += (pins.empty() ? "" : ", ") + p.name;
        md += "| " + kindName(static_cast<int>(d.kind)) + " | " + d.refPrefix + " | " + d.defaultValue + " | " + pins +
              " | " + d.footprint + " |\n";
    }
    std::map<std::string, std::vector<std::string>> byCategory;
    for (const auto& p : standardParts()) byCategory[p.category].push_back(p.spec.name);
    md += "\n## Standard parts by category (" + std::to_string(standardParts().size()) +
          " parts; library_search finds them, library_part_details shows the pins, library_add_part places one)\n\n";
    for (auto& [cat, names] : byCategory) {
        md += "- **" + cat + "** (" + std::to_string(names.size()) + "): ";
        for (size_t i = 0; i < names.size() && i < 8; ++i) md += (i ? ", " : "") + names[i];
        if (names.size() > 8) md += ", …";
        md += "\n";
    }
    return md;
}
}  // namespace

Json McpServer::resourcesListJson() const {
    Json list = Json::array();
    list.push(resourceEntry("sieda://docs/mcp-quickstart", "mcp-quickstart", "SiEDA over MCP — quick start",
                            "Units, addressing and the typical design flow with the tools.", "text/markdown"));
    for (const auto& f : docFiles(options_.docsDir))
        list.push(resourceEntry("sieda://docs/" + docName(f), docName(f), f.stem().string() + " guide",
                                "SiEDA guide " + f.filename().string(), "text/markdown"));
    list.push(resourceEntry("sieda://catalog", "catalog", "Part catalog digest",
                            "Built-in component kinds with their pins and the standard-part categories.", "text/markdown"));
    list.push(resourceEntry("sieda://project/current", "project", "Current project",
                            "The open project as saved (.siedaproj JSON).", "application/json"));
    list.push(resourceEntry("sieda://rules/presets", "rule-presets", "Design-rule presets",
                            "Standard fabrication rule presets (pcb_apply_rule_preset).", "application/json"));
    list.push(resourceEntry("sieda://autoroute/presets", "autoroute-presets", "Autorouter strategy presets",
                            "Preset autorouter strategies (pcb_autoroute preset).", "application/json"));
    list.push(resourceEntry("sieda://tools", "tools", "Tool reference", "Every tool by group (Markdown).", "text/markdown"));
    Json r = Json::object();
    r["resources"] = list;
    return r;
}

Json McpServer::readResource(const std::string& uri) const {
    std::string text, mime = "text/markdown";
    if (uri == "sieda://docs/mcp-quickstart") {
        text = kQuickstart;
    } else if (uri.rfind("sieda://docs/", 0) == 0) {
        const std::string name = lower(uri.substr(13));
        bool found = false;
        for (const auto& f : docFiles(options_.docsDir))
            if (docName(f) == name) {
                text = readTextFile(f);
                found = true;
                break;
            }
        if (!found) throw RpcError(kResourceNotFound, "Resource not found: " + uri);
    } else if (uri == "sieda://catalog") {
        text = catalogDigest();
    } else if (uri == "sieda://project/current") {
        text = takeText(sieda_project_save_json(project_));
        mime = "application/json";
    } else if (uri == "sieda://rules/presets") {
        text = takeText(sieda_design_rule_presets_json());
        mime = "application/json";
    } else if (uri == "sieda://autoroute/presets") {
        text = takeText(sieda_autoroute_presets());
        mime = "application/json";
    } else if (uri == "sieda://tools") {
        text = "# SiEDA MCP tools\n\n" + mcpToolsMarkdown();
    } else {
        throw RpcError(kResourceNotFound, "Resource not found: " + uri);
    }
    Json c = Json::object();
    c["uri"] = uri;
    c["mimeType"] = mime;
    c["text"] = text;
    Json contents = Json::array();
    contents.push(c);
    Json r = Json::object();
    r["contents"] = contents;
    return r;
}

// ---- prompts ------------------------------------------------------------------------------------------------------

namespace {
struct PromptArg {
    const char* name;
    const char* description;
    bool required;
};
struct PromptDef {
    const char* name;
    const char* title;
    const char* description;
    std::vector<PromptArg> args;
};

const std::vector<PromptDef>& promptDefs() {
    static const std::vector<PromptDef> defs = {
        {"design_minimal_mcu_board", "Design a minimal MCU board",
         "Step-by-step: a microcontroller with supply, decoupling, reset, programming header and a status LED, "
         "simulated, laid out, routed and verified.",
         {{"mcu", "Microcontroller part number from the catalog (default ATmega328P)", false},
          {"supply", "Supply input, e.g. \"5V USB\" or \"9V battery with LDO\" (default 5 V header)", false},
          {"extras", "Anything else the board needs (sensors, connectors…)", false},
          {"layers", "Copper layers: 2 or 4 (default 2)", false}}},
        {"review_design", "Review the open design",
         "Runs every check (ERC, validation, DRC, verification, SI/PI) and writes a prioritised review without "
         "changing the design.",
         {{"focus", "Optional focus, e.g. \"power\", \"EMC\", \"manufacturability\"", false}}},
        {"route_and_verify", "Route and verify the board",
         "Update the PCB from the schematic, place, autoroute, fix DRC errors, verify and write the fabrication outputs.",
         {{"layers", "Copper layers (default: keep)", false},
          {"preset", "Autorouter preset: default, fast, quality … (default quality)", false},
          {"output_dir", "Folder (inside the server root) for the fabrication package (default \"fab\")", false}}},
        {"simulate_circuit", "Simulate and explain the circuit",
         "Operating point, then a transient or AC analysis of a net, with the numbers explained.",
         {{"net", "Net to look at (default: the most interesting output)", false},
          {"analysis", "\"transient\" or \"ac\" (default: chosen from the sources)", false}}},
    };
    return defs;
}

std::string argOr(const Json& a, const char* key, const std::string& def) {
    const std::string v = a.get(key).asString("");
    return v.empty() ? def : v;
}
}  // namespace

Json McpServer::promptsListJson() const {
    Json list = Json::array();
    for (const auto& d : promptDefs()) {
        Json j = Json::object();
        j["name"] = d.name;
        j["title"] = d.title;
        j["description"] = d.description;
        Json args = Json::array();
        for (const auto& a : d.args) {
            Json aj = Json::object();
            aj["name"] = a.name;
            aj["description"] = a.description;
            aj["required"] = a.required;
            args.push(aj);
        }
        j["arguments"] = args;
        list.push(j);
    }
    Json r = Json::object();
    r["prompts"] = list;
    return r;
}

Json McpServer::getPrompt(const std::string& name, const Json& arguments) const {
    const PromptDef* def = nullptr;
    for (const auto& d : promptDefs())
        if (name == d.name) def = &d;
    if (!def) throw RpcError(kInvalidParams, "Unknown prompt: " + name);
    const Json a = arguments.isObject() ? arguments : Json::object();
    for (const auto& pa : def->args)
        if (pa.required && a.get(pa.name).asString("").empty())
            throw RpcError(kInvalidParams, std::string("Missing required argument: ") + pa.name);
    std::string text;
    if (name == "design_minimal_mcu_board") {
        const std::string mcu = argOr(a, "mcu", "ATmega328P"), supply = argOr(a, "supply", "5 V from a 2-pin header"),
                          layers = argOr(a, "layers", "2"), extras = argOr(a, "extras", "nothing else");
        text = "Design a minimal, manufacturable microcontroller board in SiEDA using the sieda tools.\n\n"
               "Requirements: MCU " + mcu + "; supply: " + supply + "; extras: " + extras + "; " + layers +
               " copper layers.\n\nWork step by step and check each result before going on:\n"
               "1. project_new (name it), then read sieda://docs/mcp-quickstart if you have not.\n"
               "2. library_search for the MCU (and a regulator if the supply needs one); library_part_details to learn "
               "the pin names; library_add_part to place them.\n"
               "3. Add the support parts with schematic_add_component: 100 nF decoupling capacitor on every supply pin, "
               "a bulk capacitor, the reset pull-up (10 kΩ) where the MCU has a reset pin, a programming / power "
               "connector, and a status LED with its series resistor on a GPIO.\n"
               "4. Wire with schematic_connect, schematic_net_label (VCC, GND, named signals) and "
               "schematic_connect_to_ground. Mark unused pins with schematic_set_no_connect.\n"
               "5. schematic_erc until it has no errors; sim_dc_op to check supply voltages and LED current.\n"
               "6. pcb_set_board (a compact size, " + layers + " layers, rule preset) then pcb_update_from_schematic; "
               "move the decoupling capacitors next to their pins with pcb_place_footprint; pcb_fit_board.\n"
               "7. pcb_add_pour for GND, pcb_autoroute (preset \"quality\"), pcb_drc until clean.\n"
               "8. verify_design, render_pcb to look at the result, then summarise what you built and any open "
               "warnings.";
    } else if (name == "review_design") {
        const std::string focus = argOr(a, "focus", "");
        text = "Review the design that is open in SiEDA. Do not change it.\n\n"
               "Gather the facts with the read-only tools: project_summary, schematic_list_components, "
               "schematic_list_nets, schematic_erc, schematic_validate, sim_dc_op, pcb_drc, verify_design, "
               "si_checks and pi_rails (when there is a board), and render_schematic / render_pcb to look at it.\n\n"
               "Then write a review: the verdict first, then findings ordered by severity (errors, risks, "
               "improvements), each with the parts or nets involved and a concrete fix (which tool and arguments "
               "would make it).";
        if (!focus.empty()) text += " Pay particular attention to: " + focus + ".";
    } else if (name == "route_and_verify") {
        const std::string layers = argOr(a, "layers", ""), preset = argOr(a, "preset", "quality"),
                          dir = argOr(a, "output_dir", "fab");
        text = "Take the open SiEDA design from schematic to fabrication files.\n\n"
               "1. schematic_erc: stop and report if it has errors.\n"
               "2. " + std::string(layers.empty() ? "" : "pcb_set_board with layers " + layers + ", then ") +
               "pcb_update_from_schematic (it places new footprints), check pcb_summary; improve the placement with "
               "pcb_place_footprint where parts belong together (decoupling at their IC, connectors at the edge), "
               "pcb_fit_board.\n"
               "3. pcb_autoroute with preset \"" + preset + "\". If connections fail, enlarge the board, change the "
               "placement or the layer count, and route again.\n"
               "4. pcb_drc: fix every error (pcb_clear_routing and re-route, move parts, adjust rules) and repeat.\n"
               "5. verify_design; then output_fabrication_package into \"" + dir + "\".\n"
               "6. Report the routing statistics, the DRC / verification verdict and the files written.";
    } else {
        const std::string net = argOr(a, "net", ""), analysis = argOr(a, "analysis", "");
        text = "Simulate the open SiEDA circuit and explain it.\n\n1. schematic_list_nets and sim_dc_op: report the "
               "operating point (voltages, currents, dissipation) and anything out of range.\n2. ";
        text += analysis == "ac" ? "sim_ac" : analysis == "transient" ? "sim_transient" : "sim_transient or sim_ac (whichever fits the sources)";
        text += net.empty() ? " on the most interesting output net" : " on net " + net;
        text += "; use sim_measure for rise time, overshoot, frequency or f3dB as relevant.\n3. Explain the results in "
                "plain words and suggest component changes if the circuit misses its purpose.";
    }
    Json content = Json::object();
    content["type"] = "text";
    content["text"] = text;
    Json msg = Json::object();
    msg["role"] = "user";
    msg["content"] = content;
    Json messages = Json::array();
    messages.push(msg);
    Json r = Json::object();
    r["description"] = def->description;
    r["messages"] = messages;
    return r;
}

// ---- JSON-RPC -----------------------------------------------------------------------------------------------------

std::string McpServer::handle(const std::string& requestLine) {
    try {
        size_t first = requestLine.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) return std::string();
        Json msg;
        try {
            msg = Json::parse(requestLine);
        } catch (const std::exception& e) {
            return errorResponse(Json(), kParseError, std::string("Parse error: ") + e.what()).dump();
        }
        if (msg.isArray()) {
            if (msg.size() == 0) return errorResponse(Json(), kInvalidRequest, "Invalid Request: empty batch").dump();
            Json out = Json::array();
            for (const auto& item : msg.items()) {
                bool respond = false;
                Json r = handleMessage(item, &respond);
                if (respond) out.push(r);
            }
            return out.size() ? out.dump() : std::string();
        }
        bool respond = false;
        Json r = handleMessage(msg, &respond);
        return respond ? r.dump() : std::string();
    } catch (...) {
        return errorResponse(Json(), kInternalError, "Internal error").dump();
    }
}

Json McpServer::handleMessage(const Json& msg, bool* respond) {
    *respond = true;
    if (!msg.isObject()) return errorResponse(Json(), kInvalidRequest, "Invalid Request: not an object");
    const bool hasId = msg.has("id");
    Json id = msg.get("id");
    if (hasId && !id.isString() && !id.isNumber()) {
        if (id.isNull() && !msg.has("method")) {  // a response to nothing we sent
            *respond = false;
            return Json();
        }
        return errorResponse(Json(), kInvalidRequest, "Invalid Request: id must be a string or a number");
    }
    if (!msg.get("method").isString()) {
        if (msg.has("result") || msg.has("error")) {  // a response from the client (we send no requests)
            *respond = false;
            return Json();
        }
        return errorResponse(hasId ? id : Json(), kInvalidRequest, "Invalid Request: method missing");
    }
    if (msg.get("jsonrpc").asString("") != "2.0")
        return errorResponse(hasId ? id : Json(), kInvalidRequest, "Invalid Request: jsonrpc must be \"2.0\"");
    const std::string method = msg.get("method").asString();
    const Json& params = msg.get("params");
    if (!params.isNull() && !params.isObject())
        return errorResponse(hasId ? id : Json(), kInvalidParams, "Invalid params: params must be an object");

    if (!hasId) {  // notification: never answered
        *respond = false;
        if (method == "notifications/initialized") initialized_ = true;
        return Json();
    }

    Json result;
    try {
        if (method == "initialize") {
            const std::string requested = params.get("protocolVersion").asString("");
            const auto& versions = supportedProtocolVersions();
            protocol_ = std::find(versions.begin(), versions.end(), requested) != versions.end() ? requested
                                                                                                 : std::string(kProtocolVersion);
            result = Json::object();
            result["protocolVersion"] = protocol_;
            Json caps = Json::object();
            Json tools = Json::object();
            tools["listChanged"] = false;
            caps["tools"] = tools;
            Json resources = Json::object();
            resources["subscribe"] = false;
            resources["listChanged"] = false;
            caps["resources"] = resources;
            Json prompts = Json::object();
            prompts["listChanged"] = false;
            caps["prompts"] = prompts;
            result["capabilities"] = caps;
            Json info = Json::object();
            info["name"] = options_.serverName;
            info["title"] = "SiEDA";
            info["version"] = sieda_version();
            result["serverInfo"] = info;
            std::string instructions =
                "SiEDA electronic design tools. Read sieda://docs/mcp-quickstart first. One project is open at a time "
                "(project_summary). Parts are addressed by designator (R1), pins as REF.PIN (R1.2). Schematic units: "
                "10 = one grid step; PCB units: millimetres. Check with schematic_erc and pcb_drc after changes.";
            if (options_.readOnly) instructions += " This server is read-only: tools that change the design are refused.";
            if (options_.allowedRoot.empty()) instructions += " File access is disabled.";
            else instructions += " Files are read and written only inside " + options_.allowedRoot + ".";
            result["instructions"] = instructions;
        } else if (method == "ping") {
            result = Json::object();
        } else if (method == "tools/list") {
            result = toolsListJson();
        } else if (method == "tools/call") {
            const Json& name = params.get("name");
            if (!name.isString()) throw RpcError(kInvalidParams, "Invalid params: tools/call needs a tool name");
            const Json& args = params.get("arguments");
            if (!args.isNull() && !args.isObject()) throw RpcError(kInvalidParams, "Invalid params: arguments must be an object");
            try {
                result = callTool(name.asString(), args);
            } catch (const std::out_of_range& e) {
                throw RpcError(kInvalidParams, e.what());
            }
        } else if (method == "resources/list") {
            result = resourcesListJson();
        } else if (method == "resources/templates/list") {
            Json t = Json::object();
            t["uriTemplate"] = "sieda://docs/{name}";
            t["name"] = "docs";
            t["title"] = "SiEDA guides";
            t["description"] = "A SiEDA guide by name (schematic, routing, simulation, …; see resources/list).";
            t["mimeType"] = "text/markdown";
            Json list = Json::array();
            list.push(t);
            result = Json::object();
            result["resourceTemplates"] = list;
        } else if (method == "resources/read") {
            const Json& uri = params.get("uri");
            if (!uri.isString()) throw RpcError(kInvalidParams, "Invalid params: resources/read needs a uri");
            result = readResource(uri.asString());
        } else if (method == "prompts/list") {
            result = promptsListJson();
        } else if (method == "prompts/get") {
            const Json& name = params.get("name");
            if (!name.isString()) throw RpcError(kInvalidParams, "Invalid params: prompts/get needs a prompt name");
            result = getPrompt(name.asString(), params.get("arguments"));
        } else {
            throw RpcError(kMethodNotFound, "Method not found: " + method);
        }
    } catch (const RpcError& e) {
        return errorResponse(id, e.code, e.what());
    } catch (const ToolError& e) {
        return errorResponse(id, kInvalidParams, e.what());
    } catch (const std::exception& e) {
        return errorResponse(id, kInternalError, std::string("Internal error: ") + e.what());
    }
    Json r = Json::object();
    r["jsonrpc"] = "2.0";
    r["id"] = id;
    r["result"] = result;
    return r;
}

}  // namespace sieda::mcp
