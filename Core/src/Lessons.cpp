// SiEDA Core — in-app lessons (MCP project_lessons): step-by-step exercises whose steps are checked against the real
// tools' results. Built-in lessons below; a company ships its own as <name>.lesson.json files ("sieda.lesson/1") in its
// team folder's Lessons/ folder or a folder the server was started with (--lessons-dir, SIEDA_LESSONS_DIRS).
//
// A lesson: {"format":"sieda.lesson/1","id","title","summary","level","start":[call…],"steps":[step…]}
//   call  = {"tool":"pcb_drc","args":{…}}
//   step  = {"title","text","hint","check":[test…],"solution":[call…]}   (every test must pass)
//   test  = {"tool":<read-only tool>,"args":{…},"path":"board.unroutedConnections","equals"|"atLeast"|"atMost"|
//            "contains"|"excludes":value | "exists":bool, "message":"shown when it fails"}
//   path: keys and array indexes split by '.', "length" = the size of an array or object.
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

#include "McpInternal.hpp"

namespace fs = std::filesystem;

namespace sieda::mcp {
namespace {

const char* kBuiltInLessons = R"JSON([
{"format":"sieda.lesson/1","id":"first_schematic","title":"Your first circuit","level":"beginner",
 "summary":"Place a 5 V source, a resistor and an LED, wire them in a loop and ground it until the electrical rule check is clean.",
 "start":[{"tool":"project_new","args":{}}],
 "steps":[
  {"title":"Place the parts","text":"Place a voltage source (5 V), a resistor (330 Ω) and an LED on the schematic.",
   "hint":"Schematic → Place, or the palette: Voltage Source, Resistor, LED.",
   "check":[{"tool":"project_summary","path":"components","atLeast":3,"message":"Three parts are needed on the sheet."}],
   "solution":[{"tool":"schematic_build","args":{"parts":[{"ref":"V1","kind":"voltage_source","value":"5"},
     {"ref":"R1","kind":"resistor","value":"330"},{"ref":"D1","kind":"led","value":"Red"}]}}]},
  {"title":"Wire the loop","text":"Wire V1 + to the resistor, the resistor to the LED's anode (A) and the LED's cathode (K) back to V1 −.",
   "hint":"Press W (wire) and click pin to pin. Every pin needs a wire.",
   "check":[{"tool":"schematic_erc","path":"unconnectedPins.length","equals":0,"message":"Some pins are not wired yet."}],
   "solution":[{"tool":"schematic_connect","args":{"from":"V1.+","to":"R1.1"}},
     {"tool":"schematic_connect","args":{"from":"R1.2","to":"D1.A"}},
     {"tool":"schematic_connect","args":{"from":"D1.K","to":"V1.-"}}]},
  {"title":"Add a ground","text":"A circuit with a source needs a ground reference: put a ground symbol on V1 −, then run the electrical rule check.",
   "hint":"Place → Ground on the negative side of the source.",
   "check":[{"tool":"schematic_erc","path":"errors","equals":0,"message":"The electrical rule check still has errors."}],
   "solution":[{"tool":"schematic_connect_to_ground","args":{"pins":["V1.-"]}}]}]},
{"format":"sieda.lesson/1","id":"first_diff_pair","title":"Route your first differential pair","level":"intermediate",
 "summary":"Two connectors joined by DATA_P / DATA_N: put them on the board and route the two nets as one coupled pair.",
 "start":[{"tool":"project_new","args":{}},
  {"tool":"schematic_build","args":{"parts":[{"ref":"J1","kind":"header","pins":2},{"ref":"J2","kind":"header","pins":2}],
   "nets":{"DATA_P":["J1.1","J2.1"],"DATA_N":["J1.2","J2.2"]}}}],
 "steps":[
  {"title":"Put the parts on the board","text":"Update the board from the schematic and place both connectors.",
   "hint":"Design → Update PCB, then Auto Place (or drag the footprints onto the board).",
   "check":[{"tool":"project_summary","path":"board.footprintsPlaced","atLeast":2,"message":"Both connectors must be on the board."}],
   "solution":[{"tool":"pcb_update_from_schematic","args":{}},{"tool":"pcb_autoplace","args":{}}]},
  {"title":"Route the pair coupled","text":"Nets named …_P / …_N are a differential pair: route them side by side at the pair's gap so they stay coupled.",
   "hint":"Route → Differential Pair, or Auto Route (it routes named pairs coupled).",
   "check":[{"tool":"project_summary","path":"board.unroutedConnections","equals":0,"message":"The pair is not routed yet."},
    {"tool":"pcb_route_report","path":"pairs.0.coupled","equals":true,"message":"DATA_P / DATA_N are routed but not as a coupled pair."}],
   "solution":[{"tool":"pcb_autoroute","args":{}}]},
  {"title":"Check the rules","text":"Run the design rule check: no errors may be left.",
   "hint":"Checks → Design Rule Check.",
   "check":[{"tool":"pcb_drc","path":"errors","equals":0,"message":"The design rule check has errors."}],
   "solution":[{"tool":"pcb_drc","args":{}}]}]},
{"format":"sieda.lesson/1","id":"ready_for_fab","title":"From schematic to a board you can order","level":"beginner",
 "summary":"Take the LED indicator example from its schematic to a placed, routed, rule-checked and verified board.",
 "start":[{"tool":"project_load_example","args":{"id":"led_indicator"}}],
 "steps":[
  {"title":"Place the footprints","text":"Bring the parts onto the board and place them.",
   "hint":"Design → Update PCB, then Auto Place.",
   "check":[{"tool":"project_summary","path":"board.footprintsNotPlaced","equals":0},
    {"tool":"project_summary","path":"board.footprintsPlaced","atLeast":3,"message":"Every footprint must be on the board."}],
   "solution":[{"tool":"pcb_update_from_schematic","args":{}},{"tool":"pcb_autoplace","args":{}}]},
  {"title":"Route every connection","text":"Route the tracks until no connection is left open.",
   "hint":"Route → Auto Route, or route each ratsnest line by hand.",
   "check":[{"tool":"project_summary","path":"board.unroutedConnections","equals":0},
    {"tool":"project_summary","path":"board.tracks","atLeast":1,"message":"Connections are still open."}],
   "solution":[{"tool":"pcb_autoroute","args":{}}]},
  {"title":"Design rule check","text":"Run the DRC and fix anything it reports as an error.",
   "hint":"Checks → Design Rule Check; click a result to go to it.",
   "check":[{"tool":"pcb_drc","path":"errors","equals":0,"message":"The DRC reports errors."}],
   "solution":[{"tool":"pcb_drc","args":{}}]},
  {"title":"Verify before you order","text":"Run the full verification (ERC, simulation, DRC, fabrication outputs): no errors may be left before you order boards.",
   "hint":"Checks → Verify Design.",
   "check":[{"tool":"verify_design","path":"errors","equals":0,"message":"Verification reports errors."}],
   "solution":[{"tool":"verify_design","args":{}}]}]}
])JSON";

// The array itself, or an empty one (a missing or mistyped field in a lesson file).
const Json& arr(const Json& j) {
    static const Json empty = Json::array();
    return j.isArray() ? j : empty;
}

const Json& builtInLessons() {
    static const Json lessons = Json::parse(kBuiltInLessons);
    return lessons;
}

// The lesson folders: the server's own (set by whoever started it) and, when given, a folder inside the root (a
// team folder: its Lessons/ subfolder when it has one).
std::vector<std::pair<std::string, fs::path>> lessonFolders(const McpServer& s, const Json& args) {
    std::vector<std::pair<std::string, fs::path>> dirs;
    for (const auto& d : s.options().lessonDirs) dirs.push_back({"organisation", d});
    const std::string folder = args.get("folder").asString("");
    if (!folder.empty()) {
        fs::path p = s.resolvePath(folder, false);
        std::error_code ec;
        if (fs::is_directory(p / "Lessons", ec)) p /= "Lessons";
        dirs.push_back({"team", p});
    }
    return dirs;
}

Json readLessonFile(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    if (ss.str().size() > (4u << 20)) throw ToolError(file.filename().string() + ": larger than 4 MB");
    return Json::parse(ss.str());
}

// Every lesson known: built-in, then each folder's *.lesson.json (sorted by name), each tagged with its source.
std::vector<Json> allLessons(const McpServer& s, const Json& args, Json* problems) {
    std::vector<Json> list;
    for (const auto& l : builtInLessons().items()) {
        Json c = l;
        c["source"] = "built-in";
        list.push_back(c);
    }
    for (const auto& [source, dir] : lessonFolders(s, args)) {
        std::error_code ec;
        std::vector<fs::path> files;
        for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
            const std::string name = it->path().filename().string();
            if (name.size() > 12 && name.compare(name.size() - 12, 12, ".lesson.json") == 0 && !it->is_symlink(ec))
                files.push_back(it->path());
        }
        std::sort(files.begin(), files.end());
        for (const auto& f : files) {
            try {
                Json l = readLessonFile(f);
                if (l.get("format").asString("") != "sieda.lesson/1" || l.get("id").asString("").empty()) {
                    if (problems) problems->push(f.filename().string() + ": not a sieda.lesson/1 file with an id");
                    continue;
                }
                l["source"] = source;
                l["file"] = f.filename().string();
                list.push_back(l);
            } catch (const std::exception& e) {
                if (problems) problems->push(f.filename().string() + ": " + e.what());
            }
        }
    }
    return list;
}

const McpTool* findTool(const std::string& name) {
    for (const auto& t : mcpTools())
        if (t.name == name) return &t;
    return nullptr;
}

// A lesson's problems: unknown tools, checks through tools that change the design, steps without a check.
Json lessonProblems(const Json& l) {
    Json problems = Json::array();
    auto callOk = [&](const Json& c, const std::string& where) {
        if (!findTool(c.get("tool").asString(""))) problems.push(where + ": unknown tool \"" + c.get("tool").asString("") + "\"");
    };
    if (l.get("title").asString("").empty()) problems.push("no title");
    for (const auto& c : arr(l.get("start")).items()) callOk(c, "start");
    if (!l.get("steps").isArray() || l.get("steps").size() == 0) problems.push("no steps");
    int n = 0;
    for (const auto& st : arr(l.get("steps")).items()) {
        const std::string where = "step " + std::to_string(++n);
        if (st.get("text").asString("").empty()) problems.push(where + ": no text");
        if (!st.get("check").isArray() || st.get("check").size() == 0) problems.push(where + ": no check");
        for (const auto& t : arr(st.get("check")).items()) {
            const McpTool* tool = findTool(t.get("tool").asString(""));
            if (!tool) problems.push(where + ": unknown tool \"" + t.get("tool").asString("") + "\"");
            else if (tool->mutates || !tool->readOnly) problems.push(where + ": " + tool->name + " changes the design; a check may only read");
            if (t.get("path").asString("").empty()) problems.push(where + ": a check without a path");
        }
        for (const auto& c : arr(st.get("solution")).items()) callOk(c, where + " solution");
    }
    return problems;
}

const Json* atPath(const Json& root, const std::string& path) {
    const Json* cur = &root;
    std::stringstream ss(path);
    for (std::string key; std::getline(ss, key, '.');) {
        if (cur->isArray()) {
            if (key == "length") {
                static thread_local Json size;
                size = Json(static_cast<double>(cur->size()));
                cur = &size;
                continue;
            }
            char* end = nullptr;
            const long i = std::strtol(key.c_str(), &end, 10);
            if (key.empty() || *end || i < 0 || static_cast<size_t>(i) >= cur->size()) return nullptr;
            cur = &(*cur)[static_cast<size_t>(i)];
        } else if (cur->isObject()) {
            if (key == "length" && !cur->has("length")) {
                static thread_local Json size;
                size = Json(static_cast<double>(cur->fields().size()));
                cur = &size;
                continue;
            }
            if (!cur->has(key)) return nullptr;
            cur = &cur->get(key);
        } else {
            return nullptr;
        }
    }
    return cur;
}

// One test: the tool's result (read-only tools only) at `path` against the expectation. "" = passed, else why not.
std::string runTest(McpServer& s, const Json& t) {
    const std::string name = t.get("tool").asString("");
    const McpTool* tool = findTool(name);
    if (!tool || tool->mutates || !tool->readOnly) return "a check may only use a read-only tool (" + name + ")";
    const Json r = s.callToolUnlogged(name, t.get("args").isObject() ? t.get("args") : Json::object());
    std::string text;
    if (r.get("content").isArray() && r.get("content").size() > 0) text = r.get("content")[0].get("text").asString("");
    if (r.get("isError").asBool(false)) return name + " failed: " + text;
    Json data;
    try {
        data = Json::parse(text);
    } catch (const std::exception&) {
        data = Json(text);
    }
    const std::string path = t.get("path").asString("");
    const Json* v = atPath(data, path);
    const std::string why = t.get("message").asString("");
    auto fail = [&](const std::string& detail) { return why.empty() ? name + " " + path + ": " + detail : why; };
    if (t.has("exists")) return (v != nullptr) == t.get("exists").asBool(true) ? "" : fail(v ? "is present" : "is missing");
    if (!v) return fail("is missing");
    const std::string got = v->isString() ? v->asString() : v->dump();
    if (t.has("equals")) {
        const Json& e = t.get("equals");
        const bool same = e.isNumber() && v->isNumber() ? e.asNumber() == v->asNumber() : e.dump() == v->dump();
        return same ? "" : fail("is " + got + ", expected " + (e.isString() ? e.asString() : e.dump()));
    }
    if (t.has("atLeast")) return v->isNumber() && v->asNumber() >= t.get("atLeast").asNumber() ? "" : fail("is " + got + ", needs at least " + t.get("atLeast").dump());
    if (t.has("atMost")) return v->isNumber() && v->asNumber() <= t.get("atMost").asNumber() ? "" : fail("is " + got + ", needs at most " + t.get("atMost").dump());
    if (t.has("contains")) return got.find(t.get("contains").asString("")) != std::string::npos ? "" : fail("does not mention " + t.get("contains").asString(""));
    if (t.has("excludes")) return got.find(t.get("excludes").asString("")) == std::string::npos ? "" : fail("still mentions " + t.get("excludes").asString(""));
    return "";
}

Json stepJson(const Json& lesson, int index) {
    const Json& steps = lesson.get("steps");
    Json j = Json::object();
    j["lesson"] = lesson.get("id").asString("");
    j["title"] = lesson.get("title").asString("");
    j["steps"] = static_cast<int>(steps.size());
    if (index >= static_cast<int>(steps.size())) {
        j["done"] = true;
        j["message"] = "Lesson complete: every step checked.";
        return j;
    }
    const Json& st = steps[static_cast<size_t>(index)];
    j["done"] = false;
    j["step"] = index + 1;
    j["stepTitle"] = st.get("title").asString("");
    j["text"] = st.get("text").asString("");
    if (st.has("hint")) j["hint"] = st.get("hint").asString("");
    j["canShow"] = st.get("solution").isArray() && st.get("solution").size() > 0;
    return j;
}

void runCalls(McpServer& s, const Json& calls, const std::string& what) {
    if (!calls.isArray()) return;
    for (const auto& c : calls.items()) {
        const Json r = s.callTool(c.get("tool").asString(""), c.get("args").isObject() ? c.get("args") : Json::object());
        if (r.get("isError").asBool(false))
            throw ToolError(what + ": " + c.get("tool").asString("") + " failed: " + r.get("content")[0].get("text").asString(""));
    }
}

}  // namespace

std::vector<std::string> lessonDirsFromEnvironment() {
    std::vector<std::string> out;
    const char* env = std::getenv("SIEDA_LESSONS_DIRS");
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    std::stringstream ss(env ? env : "");
    for (std::string d; std::getline(ss, d, sep);)
        if (!d.empty()) out.push_back(d);
    return out;
}

ToolOutput lessonsTool(McpServer& s, const Json& args) {
    const std::string action = args.get("action").asString("list");
    ToolOutput o;
    if (action == "list") {
        Json problems = Json::array(), list = Json::array();
        for (const auto& l : allLessons(s, args, &problems)) {
            Json j = Json::object();
            for (const char* k : {"id", "title", "summary", "level", "source", "file"})
                if (l.has(k)) j[k] = l.get(k);
            j["steps"] = static_cast<int>(l.get("steps").size());
            list.push(j);
        }
        Json j = Json::object();
        j["lessons"] = list;
        if (problems.size()) j["problems"] = problems;
        if (!s.lesson.isNull()) j["current"] = stepJson(s.lesson, s.lessonStep);
        o.data = j;
        return o;
    }
    if (action == "validate") {  // for lesson authors: one file, or every lesson the server can see
        Json report = Json::array(), problems = Json::array();
        std::vector<Json> lessons;
        if (!args.get("path").asString("").empty()) lessons.push_back(readLessonFile(s.resolvePath(args.get("path").asString(""), false)));
        else lessons = allLessons(s, args, &problems);
        for (const auto& l : lessons) {
            Json r = Json::object();
            r["id"] = l.get("id").asString("");
            if (l.get("format").asString("") != "sieda.lesson/1") problems.push(r["id"].asString("") + ": format must be sieda.lesson/1");
            r["problems"] = lessonProblems(l);
            report.push(r);
        }
        Json j = Json::object();
        j["lessons"] = report;
        if (problems.size()) j["problems"] = problems;
        bool ok = problems.size() == 0;
        for (const auto& r : report.items()) ok = ok && r.get("problems").size() == 0;
        j["valid"] = ok;
        o.data = j;
        return o;
    }
    if (action == "start") {
        const std::string id = args.get("id").asString("");
        for (const auto& l : allLessons(s, args, nullptr)) {
            if (l.get("id").asString("") != id) continue;
            const Json problems = lessonProblems(l);
            if (problems.size()) throw ToolError("Lesson " + id + " has problems: " + problems.dump());
            runCalls(s, l.get("start"), "start");
            s.lesson = l;
            s.lessonStep = 0;
            o.data = stepJson(l, 0);
            return o;
        }
        throw ToolError("No lesson \"" + id + "\" (action list shows them)");
    }
    if (s.lesson.isNull()) throw ToolError("No lesson is running: start one (action list shows them)");
    if (action == "stop") {
        s.lesson = Json();
        Json j = Json::object();
        j["stopped"] = true;
        o.data = j;
        return o;
    }
    if (action == "status") {
        o.data = stepJson(s.lesson, s.lessonStep);
        return o;
    }
    if (action == "show") {  // do the current step for the learner, then check it as usual
        if (s.lessonStep < static_cast<int>(s.lesson.get("steps").size()))
            runCalls(s, s.lesson.get("steps")[static_cast<size_t>(s.lessonStep)].get("solution"), "show");
    } else if (action != "check") {
        throw ToolError("Unknown action " + action + ": list, start, check, show, status, stop or validate");
    }
    const Json& steps = s.lesson.get("steps");
    if (s.lessonStep >= static_cast<int>(steps.size())) {
        o.data = stepJson(s.lesson, s.lessonStep);
        return o;
    }
    Json failed = Json::array();
    for (const auto& t : steps[static_cast<size_t>(s.lessonStep)].get("check").items()) {
        const std::string why = runTest(s, t);
        if (!why.empty()) failed.push(why);
    }
    const bool passed = failed.size() == 0;
    if (passed) ++s.lessonStep;
    Json j = stepJson(s.lesson, s.lessonStep);
    j["passed"] = passed;
    if (!passed) j["notYet"] = failed;
    o.data = j;
    return o;
}

}  // namespace sieda::mcp
