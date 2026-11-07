// si-llm's tool vocabulary on SiEDA's MCP server (group "si_llm", offered with McpOptions::siLlmTools / sieda-mcp --si-llm).
//
// si-llm (github.com/kernelsrinivas/si-llm) is a small PCB / CAD language model whose CLI carries its own MCP client: it
// turns a sentence into calls of its own tool names (pcb_add, pcb_net, sch_wire, pcb_flow …) and sends each to whichever
// connected server lists that name. These tools take those names and arguments and do the work with SiEDA's own tools,
// so si-llm designs real parts on real nets: its generic packages become catalog parts with their datasheet pinouts, a net
// between two references lands on the pins its name points at. Results are one plain sentence (si-llm shows the text of the
// first content item). When the server has a project file open, every change is saved to it, so the app or Studio editing
// that file live (co-editing) shows si-llm's work as it happens.

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "McpInternal.hpp"

namespace sieda::mcp {
namespace {

ToolOutput say(const std::string& text) {
    ToolOutput o;
    o.data = Json(text);
    return o;
}

/// Runs one of SiEDA's tools inside an si-llm call: one undo step for the whole si-llm call, not one per inner tool.
Json run(McpServer& s, const std::string& name, const Json& args = Json::object()) {
    for (const auto& t : mcpTools())
        if (t.name == name) return t.handler(s, args).data;
    throw ToolError("SiEDA has no tool " + name);
}

std::string upper(std::string v) {
    for (auto& c : v) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return v;
}

std::string trim(std::string v) {
    while (!v.empty() && std::isspace(static_cast<unsigned char>(v.back()))) v.pop_back();
    size_t i = 0;
    while (i < v.size() && std::isspace(static_cast<unsigned char>(v[i]))) ++i;
    return v.substr(i);
}

/// si-llm's references: "u1" → "U1".
std::string canonRef(const std::string& r) { return upper(trim(r)); }

/// What one of si-llm's generic packages becomes in SiEDA: a built-in kind (two-pin parts, transistors) or a catalog part
/// whose pinout comes from its datasheet. Anything else is tried as a catalog name ("LM358", "ESP32-S3-WROOM-1").
struct PartChoice {
    std::string kind;     // built-in kind, or "" for a catalog part
    std::string catalog;  // catalog part name
};

PartChoice choosePart(const std::string& pkgIn) {
    const std::string p = upper(trim(pkgIn));
    struct Row { const char* words; const char* kind; const char* catalog; };
    static const Row rows[] = {
        {"RES RESISTOR R", "resistor", ""},
        {"CAP CAPACITOR C", "capacitor", ""},
        {"IND INDUCTOR COIL L", "inductor", ""},
        {"DIODE D", "diode", ""},
        {"LED", "led", ""},
        {"BJT NPN TRANSISTOR Q", "npn", ""},
        {"FET MOSFET NMOS", "nmos", ""},
        {"SWITCH SW BUTTON", "switch", ""},
        {"FUSE F", "fuse", ""},
        {"CONN CONNECTOR HDR HEADER J", "", "Header_1x04"},
        {"ESP32", "", "ESP32-WROOM-32E"},
        {"USB-C USBC USB", "", "USB-C-RECEPTACLE-USB2"},
        {"LDO", "", "AP2112K-3.3"},
        {"IMU", "", "LSM6DS3TR-C"},
        {"MCU", "", "ATMEGA328P-AU"},
        {"BUCK", "", "TPS62130RGT"},
        {"ISO ISOLATOR", "", "ISO7741DWR"},
        {"ANT ANTENNA", "", "ANTENNA-2450AT18A100"},
    };
    for (const auto& row : rows) {
        std::string words = std::string(" ") + row.words + " ";
        if (words.find(" " + p + " ") != std::string::npos) return {row.kind, row.catalog};
    }
    return {"", trim(pkgIn)};
}

/// The schematic part with this reference (from schematic_list_components), or null.
Json findPart(McpServer& s, const std::string& ref) {
    const Json list = run(s, "schematic_list_components");
    for (const auto& c : list.get("components").items())
        if (c.get("ref").asString("") == ref) return c;
    return Json();
}

/// Adds the part to the schematic unless that reference is there already. Returns a sentence saying what it did.
std::string ensurePart(McpServer& s, const std::string& ref, const std::string& pkg) {
    if (!findPart(s, ref).isNull()) return "";
    const PartChoice choice = choosePart(pkg);
    Json a = Json::object();
    a["ref"] = ref;
    Json made;
    if (!choice.kind.empty()) {
        a["kind"] = choice.kind;
        made = run(s, "schematic_add_component", a);
    } else {
        a["name"] = choice.catalog;
        try {
            made = run(s, "library_add_part", a);
        } catch (const ToolError& e) {
            throw ToolError("No SiEDA part for \"" + pkg + "\" (" + e.what() +
                            "). Use RES, CAP, IND, DIODE, LED, BJT, FET, CONN, ESP32, USB-C, LDO, IMU, MCU, BUCK, ISO, ANT or a "
                            "catalog part name.");
        }
    }
    const std::string got = made.get("ref").asString(ref);
    if (got != ref) throw ToolError("SiEDA named the part " + got + ", not " + ref + ": that reference is taken.");
    const std::string what = choice.kind.empty() ? choice.catalog : choice.kind;
    return "Added " + ref + " (" + what + ") to the schematic.";
}

/// Pin names that go with a supply or ground net's name, in order of preference (a 3V3 net takes an LDO's VOUT, not its
/// VIN; a 5V net its VIN).
std::vector<std::string> pinNamesFor(const std::string& net) {
    const std::string n = upper(net);
    static const std::vector<std::vector<std::string>> groups = {
        {"GND", "VSS", "AGND", "DGND", "PGND", "0V", "GROUND", "EP"},
        {"3V3", "+3V3", "3.3V", "+3.3V", "VCC", "VDD", "VOUT", "VBAT", "VDDIO"},
        {"5V", "+5V", "VBUS", "VIN", "VCC", "VDD"},
        {"VIN", "VBUS", "VIN+", "IN"},
        {"VOUT", "OUT", "SW"},
    };
    for (const auto& g : groups)
        if (std::find(g.begin(), g.end(), n) != g.end()) return g;
    return {};
}

/// One end of si-llm's net: "U1.GND" / "U1.3" as given, or a bare reference whose pin is chosen by the net's name (a pin
/// of that name, then of a supply / ground name that goes with it), else a two-pin part's free pin. An IC with no pin of
/// that name is refused with its pin names, so the next call can name one.
std::string pinFor(McpServer& s, const std::string& endIn, const std::string& net) {
    const std::string end = trim(endIn);
    if (end.find('.') != std::string::npos) {
        const auto dot = end.find('.');
        return canonRef(end.substr(0, dot)) + end.substr(dot);
    }
    const std::string ref = canonRef(end);
    const Json part = findPart(s, ref);
    if (part.isNull()) throw ToolError(ref + " is not in the design: add it first (sch_add or pcb_add).");
    const auto& pins = part.get("pins").items();
    auto label = [](const Json& p) { return p.get("name").asString(p.get("number").asString("")); };
    for (const auto& p : pins)
        if (upper(label(p)) == upper(net) || upper(p.get("net").asString("")) == upper(net)) return ref + "." + label(p);
    for (const auto& want : pinNamesFor(net))
        for (const auto& p : pins)
            if (upper(label(p)) == want) return ref + "." + label(p);
    if (pins.size() <= 2) {
        for (const auto& p : pins)
            if (p.get("net").isNull()) return ref + "." + label(p);
        return ref + "." + label(pins.back());
    }
    std::string names;
    for (const auto& p : pins) {
        if (names.size() > 300) {
            names += ", …";
            break;
        }
        names += (names.empty() ? "" : ", ") + label(p);
    }
    throw ToolError(ref + " has no pin for net " + net + ": name one as " + ref + ".<pin> (" + names + ").");
}

/// Ties two ends onto a named net (a net label on each pin, so the net keeps si-llm's name).
std::string tie(McpServer& s, const Json& a) {
    const std::string net = upper(trim(argStr(a, "name")));
    if (net.empty()) throw ToolError("Name the net.");
    const std::string pa = pinFor(s, argStr(a, "a"), net), pb = pinFor(s, argStr(a, "b"), net);
    Json args = Json::object();
    args["net"] = net;
    Json pins = Json::array();
    pins.push(pa), pins.push(pb);
    args["pins"] = pins;
    run(s, "schematic_net_label", args);
    return "Net " + net + " ties " + pa + " to " + pb + ".";
}

/// After a change: the design goes back to the file the server has open (if any), so an app editing it live sees it.
std::string saved(McpServer& s, const std::string& text) {
    if (s.projectPath().empty()) return text;
    try {
        run(s, "project_save");
        return text;
    } catch (const ToolError& e) {
        return text + " (Not saved: " + e.what() + ")";
    }
}

/// The PCB in step with the schematic (new parts get footprints) before a placement.
void syncBoard(McpServer& s) { run(s, "pcb_update_from_schematic"); }

std::string place(McpServer& s, const std::string& ref, double x, double y, const Json* rotation = nullptr) {
    Json a = Json::object();
    a["ref"] = ref, a["x"] = x, a["y"] = y, a["force"] = true;
    if (rotation) a["rotation"] = *rotation;
    const Json r = run(s, "pcb_place_footprint", a);
    if (!r.get("committed").asBool(true)) throw ToolError("SiEDA could not place " + ref + ": " + r.get("note").asString(""));
    char buf[96];
    std::snprintf(buf, sizeof buf, " at (%g, %g) mm", r.get("x").asNumber(x), r.get("y").asNumber(y));
    return ref + buf;
}

std::string drcLine(const Json& d) {
    const int errors = static_cast<int>(d.get("errors").asNumber(0)), warnings = static_cast<int>(d.get("warnings").asNumber(0));
    std::string text = "DRC: " + std::to_string(errors) + " errors, " + std::to_string(warnings) + " warnings";
    int shown = 0;
    for (const auto& v : d.get("violations").items()) {
        if (v.get("severity").asString("") != "error" || shown == 3) continue;
        text += shown++ ? "; " : " (";
        text += v.get("message").asString("");
    }
    return text + (shown ? ")." : ".");
}

}  // namespace

std::vector<McpTool> siLlmTools() {
    std::vector<McpTool> tools;
    auto add = [&](const char* name, const char* title, bool mutates, const std::string& desc, Json schema,
                   std::function<ToolOutput(McpServer&, const Json&)> handler) {
        McpTool t;
        t.name = name, t.group = "si_llm", t.title = title, t.description = desc;
        t.inputSchema = schema.isObject() ? schema : Schema().build();
        t.readOnly = !mutates, t.mutates = mutates, t.idempotent = !mutates;
        t.handler = std::move(handler);
        tools.push_back(std::move(t));
    };
    const std::string pkgs = "RES, CAP, IND, DIODE, LED, BJT, FET, CONN, ESP32, USB-C, LDO, IMU, MCU, BUCK, ISO, ANT or a catalog part name";

    add("sch_add", "si-llm: add a symbol", true, "Adds a part to the schematic. pkg: " + pkgs + ".",
        Schema().str("ref", "Reference, e.g. U1", true).str("pkg", "Package / part", true).build(),
        [](McpServer& s, const Json& a) {
            const std::string ref = canonRef(argStr(a, "ref"));
            const std::string did = ensurePart(s, ref, argStr(a, "pkg"));
            return say(saved(s, did.empty() ? ref + " is in the schematic already." : did));
        });
    add("sch_wire", "si-llm: wire a net", true,
        "Ties two parts onto a named net. a / b: a reference (its pin is chosen by the net's name) or REF.PIN.",
        Schema().str("name", "Net name", true).str("a", "First end", true).str("b", "Second end", true).build(),
        [](McpServer& s, const Json& a) { return say(saved(s, tie(s, a))); });
    add("pcb_net", "si-llm: tie a net", true,
        "Ties two parts onto a named net (as sch_wire; the board follows the schematic).",
        Schema().str("name", "Net name", true).str("a", "First end", true).str("b", "Second end", true).build(),
        [](McpServer& s, const Json& a) {
            const std::string text = tie(s, a);
            syncBoard(s);
            return say(saved(s, text));
        });
    add("pcb_add", "si-llm: place a part", true,
        "Places a part on the board at x, y mm (added to the schematic first when new). pkg: " + pkgs + ".",
        Schema().str("ref", "Reference", true).str("pkg", "Package / part", true).num("x", "mm", true).num("y", "mm", true).build(),
        [](McpServer& s, const Json& a) {
            const std::string ref = canonRef(argStr(a, "ref"));
            std::string did = ensurePart(s, ref, argStr(a, "pkg"));
            syncBoard(s);
            const std::string at = place(s, ref, argNum(a, "x", 0), argNum(a, "y", 0));
            return say(saved(s, (did.empty() ? "" : did + " ") + "Placed " + at + "."));
        });
    add("pcb_move", "si-llm: move a part", true, "Moves a placed part to x, y mm.",
        Schema().str("ref", "Reference", true).num("x", "mm", true).num("y", "mm", true).build(),
        [](McpServer& s, const Json& a) {
            return say(saved(s, "Moved " + place(s, canonRef(argStr(a, "ref")), argNum(a, "x", 0), argNum(a, "y", 0)) + "."));
        });
    add("pcb_rotate", "si-llm: rotate a part", true, "Turns a placed part to an angle in degrees (multiples of 90).",
        Schema().str("ref", "Reference", true).num("angle", "Degrees", true).build(),
        [](McpServer& s, const Json& a) {
            const std::string ref = canonRef(argStr(a, "ref"));
            const Json part = findPart(s, ref);
            if (part.isNull()) throw ToolError(ref + " is not in the design.");
            Json placed = run(s, "pcb_summary");
            double x = 0, y = 0;
            bool found = false;
            for (const auto& f : placed.get("footprints").items())
                if (f.get("ref").asString("") == ref) x = f.get("x").asNumber(0), y = f.get("y").asNumber(0), found = true;
            if (!found) throw ToolError(ref + " is not on the board: place it first (pcb_add).");
            const int angle = ((static_cast<int>(std::lround(argNum(a, "angle", 0) / 90.0)) * 90) % 360 + 360) % 360;
            const Json rot(angle);
            return say(saved(s, "Turned " + place(s, ref, x, y, &rot) + " to " + std::to_string(angle) + "°."));
        });
    add("pcb_delete", "si-llm: remove a part", true, "Removes a part from the schematic and the board.",
        Schema().str("ref", "Reference", true).build(),
        [](McpServer& s, const Json& a) {
            const std::string ref = canonRef(argStr(a, "ref"));
            Json args = Json::object();
            args["ref"] = ref;
            run(s, "schematic_remove_component", args);
            syncBoard(s);
            return say(saved(s, "Removed " + ref + "."));
        });
    add("pcb_status", "si-llm: parts and nets", false, "Lists the parts (placed or not) and the named nets.", Json(),
        [](McpServer& s, const Json&) {
            const Json list = run(s, "schematic_list_components");
            std::string parts, nets;
            std::vector<std::string> seen;
            for (const auto& c : list.get("components").items()) {
                parts += (parts.empty() ? "" : ", ") + c.get("ref").asString("") + " " +
                         c.get("value").asString(c.get("kind").asString(""));
                for (const auto& p : c.get("pins").items()) {
                    const std::string n = p.get("net").asString("");
                    if (!n.empty() && n.rfind("N$", 0) != 0 && std::find(seen.begin(), seen.end(), n) == seen.end())
                        seen.push_back(n);
                }
            }
            for (const auto& n : seen) nets += (nets.empty() ? "" : ", ") + n;
            const Json b = run(s, "project_summary").get("board");
            return say("Parts: " + (parts.empty() ? std::string("none") : parts) + ". Nets: " +
                       (nets.empty() ? std::string("none") : nets) + ". Board " +
                       std::to_string(static_cast<int>(b.get("footprintsPlaced").asNumber(0))) + " placed, " +
                       std::to_string(static_cast<int>(b.get("unroutedConnections").asNumber(0))) + " connections unrouted.");
        });
    add("pcb_reset", "si-llm: clear the design", true, "Starts a new, empty project.", Json(),
        [](McpServer& s, const Json&) {
            run(s, "project_new");
            return say("Cleared: a new, empty design.");
        });
    add("sch_erc", "si-llm: electrical rule check", false, "Electrical rule check of the schematic.", Json(),
        [](McpServer& s, const Json&) {
            const Json r = run(s, "schematic_erc");
            std::string text = "ERC: " + r.get("status").asString("done");
            const std::string open = r.get("unconnectedPins").asString("");
            if (!open.empty()) text += ". " + open;
            return say(text + ".");
        });
    add("set_outline", "si-llm: board size", true, "Sets the board to length × width mm.",
        Schema().num("length", "mm", true).num("width", "mm", true).build(),
        [](McpServer& s, const Json& a) {
            Json args = Json::object();
            args["width"] = argNum(a, "length", 50), args["height"] = argNum(a, "width", 40);
            run(s, "pcb_set_board", args);
            char buf[96];
            std::snprintf(buf, sizeof buf, "Board outline %g × %g mm.", argNum(a, "length", 50), argNum(a, "width", 40));
            return say(saved(s, buf));
        });
    add("place_sheet", "si-llm: place the schematic", true,
        "Puts every schematic part that is not on the board yet onto it (footprints from the parts, Auto Place).", Json(),
        [](McpServer& s, const Json&) {
            syncBoard(s);
            Json args = Json::object();
            args["decoupling"] = true;
            const Json r = run(s, "pcb_autoplace", args);
            return say(saved(s, "Placed the schematic's parts on the board" +
                                    std::string(r.get("boardGrown").asBool(false) ? " (the board grew to fit)." : ".")));
        });
    add("autoroute", "si-llm: route the board", true, "Routes every open connection (which: \"rip\" removes the routing).",
        Schema().str("which", "\"rip\" to remove the routing").build(),
        [](McpServer& s, const Json& a) {
            const std::string which = upper(argStr(a, "which"));
            if (which == "RIP" || which == "CLEAR" || which == "REMOVE") {
                const Json nets = run(s, "project_snapshot", [] {
                    Json j = Json::object();
                    Json sec = Json::array();
                    sec.push("tracks");
                    j["sections"] = sec;
                    return j;
                }());
                std::vector<std::string> seen;
                for (const auto& t : nets.get("tracks").items()) {
                    const std::string n = t.get("netName").asString("");
                    if (!n.empty() && std::find(seen.begin(), seen.end(), n) == seen.end()) seen.push_back(n);
                }
                for (const auto& n : seen) {
                    Json args = Json::object();
                    args["net"] = n;
                    run(s, "pcb_remove_copper", args);
                }
                return say(saved(s, "Removed the routing of " + std::to_string(seen.size()) + " nets."));
            }
            const Json r = run(s, "pcb_autoroute");
            const int open = static_cast<int>(r.get("unrouted").asNumber(r.get("unroutedConnections").asNumber(0)));
            return say(saved(s, open ? "Routed; " + std::to_string(open) + " connections are still open." : "Routed every connection."));
        });
    add("pcb_flow", "si-llm: check and release", true,
        "Runs the DRC, then writes the fabrication package (Gerbers, drills, BOM, placement) into fabrication/ when it passes.",
        Json(), [](McpServer& s, const Json&) {
            const Json d = run(s, "pcb_drc");
            if (!d.get("passes").asBool(false)) return say(drcLine(d) + " Not released: fix the errors (autoroute routes open nets).");
            Json args = Json::object();
            args["dir"] = "fabrication";
            run(s, "output_fabrication_package", args);
            return say(saved(s, drcLine(d) + " Released: the fabrication package is in fabrication/."));
        });
    add("fab_gerber", "si-llm: Gerbers", false, "Writes the Gerbers and drill files into gerbers/.", Json(),
        [](McpServer& s, const Json&) {
            Json args = Json::object();
            args["dir"] = "gerbers";
            run(s, "output_gerbers", args);
            return say("Gerbers and drills are in gerbers/.");
        });
    add("asm_bom", "si-llm: bill of materials", false, "The bill of materials, one line per value and footprint.", Json(),
        [](McpServer& s, const Json&) {
            Json args = Json::object();
            args["format"] = "csv";
            const Json r = run(s, "output_bom", args);
            return say(r.isString() ? r.asString() : r.get("csv").asString(r.dump()));
        });
    add("asm_pos", "si-llm: pick and place", false, "The pick-and-place (centroid) file.", Json(),
        [](McpServer& s, const Json&) {
            const Json r = run(s, "output_pick_and_place");
            return say(r.isString() ? r.asString() : r.get("text").asString(r.dump()));
        });
    add("sim_dc", "si-llm: DC operating point", false, "DC operating point of the schematic.", Json(),
        [](McpServer& s, const Json&) {
            const Json r = run(s, "sim_dc_op");
            std::string text;
            for (const auto& [net, v] : r.get("nodes").fields()) {
                if (text.size() > 400) break;
                char buf[64];
                std::snprintf(buf, sizeof buf, "%g V", v.asNumber(0));
                text += (text.empty() ? "" : ", ") + net + " " + buf;
            }
            return say(text.empty() ? r.dump() : "Operating point: " + text + ".");
        });
    return tools;
}

}  // namespace sieda::mcp
