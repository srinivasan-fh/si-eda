// SiEDA Core — rule scripting (CustomRules.hpp): a small expression language evaluated per board object.
#include "sieda/CustomRules.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <map>

#include "sieda/TrackGeometry.hpp"

namespace sieda {
namespace {

struct Value {
    bool text = false;
    double n = 0;
    std::string s;
    bool truthy() const { return text ? !s.empty() : n != 0; }
};
using Props = std::map<std::string, Value>;

Value num(double v) { return {false, v, {}}; }
Value str(std::string v) { return {true, 0, std::move(v)}; }

bool glob(const std::string& text, const std::string& pattern) {
    size_t t = 0, p = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::toupper(static_cast<unsigned char>(pattern[p])) ==
                                                          std::toupper(static_cast<unsigned char>(text[t])))) ++t, ++p;
        else if (p < pattern.size() && pattern[p] == '*') star = p++, mark = t;
        else if (star != std::string::npos) p = star + 1, t = ++mark;
        else return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

/// Tokens: numbers (unit folded in, mm), strings (kind 's'), names ('n'), operators ('o').
struct Token {
    char kind;
    std::string text;
    double value = 0;
};

bool tokenize(const std::string& src, std::vector<Token>& out, std::string& error) {
    for (size_t i = 0; i < src.size();) {
        const char c = src[i];
        if (std::isspace(static_cast<unsigned char>(c))) {
            ++i;
        } else if (std::isdigit(static_cast<unsigned char>(c)) || (c == '.' && i + 1 < src.size() && std::isdigit(static_cast<unsigned char>(src[i + 1])))) {
            char* end = nullptr;
            double v = std::strtod(src.c_str() + i, &end);
            i = static_cast<size_t>(end - src.c_str());
            size_t j = i;
            while (j < src.size() && std::isalpha(static_cast<unsigned char>(src[j]))) ++j;
            std::string unit = src.substr(i, j - i);
            if (unit.empty()) {  // "400 mil": a unit word after a space
                size_t k = i;
                while (k < src.size() && src[k] == ' ') ++k;
                size_t e = k;
                while (e < src.size() && std::isalpha(static_cast<unsigned char>(src[e]))) ++e;
                const std::string w = src.substr(k, e - k);
                if (w == "mm" || w == "mil" || w == "in") unit = w, j = e;
            }
            if (unit == "mil") v *= 0.0254;
            else if (unit == "in") v *= 25.4;
            else if (!unit.empty() && unit != "mm") return error = "unknown unit \"" + unit + "\"", false;
            i = j;
            out.push_back({'#', "", v});
        } else if (c == '"' || c == '\'') {
            const size_t e = src.find(c, i + 1);
            if (e == std::string::npos) return error = "unclosed string", false;
            out.push_back({'s', src.substr(i + 1, e - i - 1)});
            i = e + 1;
        } else if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') {
            size_t j = i;
            while (j < src.size() && (std::isalnum(static_cast<unsigned char>(src[j])) || src[j] == '_')) ++j;
            std::string w = src.substr(i, j - i);
            i = j;
            if (w == "and") out.push_back({'o', "&&"});
            else if (w == "or") out.push_back({'o', "||"});
            else if (w == "not") out.push_back({'o', "!"});
            else out.push_back({'n', w});
        } else {
            static const char* ops[] = {"&&", "||", "==", "!=", "<=", ">=", "<", ">", "!", "~", "+", "-", "*", "/", "(", ")"};
            bool found = false;
            for (const char* op : ops)
                if (src.compare(i, std::char_traits<char>::length(op), op) == 0) {
                    out.push_back({'o', op});
                    i += std::char_traits<char>::length(op);
                    found = true;
                    break;
                }
            if (!found) return error = std::string("unexpected '") + c + "'", false;
        }
        if (out.size() > 500) return error = "expression too long", false;
    }
    return true;
}

/// Recursive-descent evaluation of a token list against one object's properties.
struct Eval {
    const std::vector<Token>& t;
    const Props& props;
    size_t i = 0;
    int depth = 0;
    std::string error;

    bool is(const char* op) const { return i < t.size() && t[i].kind == 'o' && t[i].text == op; }
    Value fail(const std::string& why) {
        if (error.empty()) error = why;
        i = t.size();
        return num(0);
    }
    Value orExpr() {
        if (++depth > 64) return fail("expression nested too deep");
        Value v = andExpr();
        while (is("||")) ++i, v = num(andExpr().truthy() || v.truthy());
        --depth;
        return v;
    }
    Value andExpr() {
        Value v = notExpr();
        while (is("&&")) ++i, v = num(notExpr().truthy() && v.truthy());
        return v;
    }
    Value notExpr() {
        if (is("!")) return ++i, num(!notExpr().truthy());
        return compare();
    }
    Value compare() {
        Value a = sum();
        for (const char* op : {"==", "!=", "<=", ">=", "<", ">", "~"}) {
            if (!is(op)) continue;
            ++i;
            const Value b = sum();
            const std::string o = op;
            if (o == "~") return num(glob(a.text ? a.s : std::to_string(a.n), b.s));
            if (a.text != b.text) {
                if (o == "==" || o == "!=") return num(o == "!=");  // a string never equals a number
                return fail("comparing text with a number");
            }
            const int c = a.text ? a.s.compare(b.s) : (a.n < b.n - 1e-9 ? -1 : a.n > b.n + 1e-9 ? 1 : 0);
            return num(o == "==" ? c == 0 : o == "!=" ? c != 0 : o == "<" ? c < 0 : o == ">" ? c > 0 : o == "<=" ? c <= 0 : c >= 0);
        }
        return a;
    }
    Value sum() {
        Value v = product();
        while (is("+") || is("-")) {
            const bool plus = t[i++].text == "+";
            const Value b = product();
            if (v.text || b.text) return fail("arithmetic on text");
            v.n += plus ? b.n : -b.n;
        }
        return v;
    }
    Value product() {
        Value v = unary();
        while (is("*") || is("/")) {
            const bool times = t[i++].text == "*";
            const Value b = unary();
            if (v.text || b.text) return fail("arithmetic on text");
            v.n = times ? v.n * b.n : (b.n == 0 ? 0 : v.n / b.n);
        }
        return v;
    }
    Value unary() {
        if (is("-")) {
            ++i;
            Value v = unary();
            v.n = -v.n;
            return v;
        }
        if (i >= t.size()) return fail("expression ends early");
        const Token& k = t[i++];
        if (k.kind == '#') return num(k.value);
        if (k.kind == 's') return str(k.text);
        if (k.kind == 'n') {
            const auto it = props.find(k.text);
            return it == props.end() ? fail("unknown property \"" + k.text + "\"") : it->second;
        }
        if (k.text == "(") {
            Value v = orExpr();
            if (!is(")")) return fail("missing )");
            ++i;
            return v;
        }
        return fail("unexpected \"" + k.text + "\"");
    }
};

/// The expression's value for `props` ("" = always true); `error` set when it does not read.
bool holds(const std::vector<Token>& tokens, const Props& props, std::string& error) {
    if (tokens.empty()) return true;
    Eval e{tokens, props, 0, 0, {}};
    const Value v = e.orExpr();
    if (e.error.empty() && e.i < tokens.size()) e.error = "unexpected \"" + tokens[e.i].text + "\"";
    error = e.error;
    return v.truthy();
}

/// Every object of `kind` with its properties, a description, a location and its part (or -1).
struct Object {
    Props props;
    std::string what;
    Vec2 at;
    int component = -1;
};

std::vector<Object> objects(const std::string& kind, const Schematic& sch, const PcbLayout& pcb) {
    std::vector<Object> out;
    const auto& nets = sch.nets();
    auto netName = [&](int n) { return n >= 0 && n < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(n)].name : std::string(); };
    const BoardSettings& s = pcb.settings;
    if (kind == "track") {
        for (const Track& t : pcb.tracks) {
            const std::string layer = copperLayerName(t.layer, s.layerCount);
            out.push_back({{{"net", str(netName(t.net))}, {"width", num(t.width)}, {"length", num(trackLength(t))}, {"layer", str(layer)},
                            {"layerIndex", num(t.layer)}, {"arc", num(t.arc)}, {"locked", num(t.locked)}},
                           "track of " + netName(t.net) + " on " + layer, (t.a + t.b) * 0.5});
        }
    } else if (kind == "via") {
        for (const Via& v : pcb.vias)
            out.push_back({{{"net", str(netName(v.net))}, {"drill", num(v.drill)}, {"diameter", num(v.diameter)},
                            {"ring", num((v.diameter - v.drill) / 2)}, {"through", num(v.isThrough())}},
                           "via of " + netName(v.net), v.position});
    } else if (kind == "pad") {
        for (const Pad& p : pcb.pads(sch)) {
            const Component* c = sch.find(p.componentId);
            const std::string ref = c ? c->ref : "";
            out.push_back({{{"net", str(netName(p.net))}, {"ref", str(ref)}, {"pin", num(p.padNumber)}, {"width", num(p.size.x)},
                            {"height", num(p.size.y)}, {"drill", num(p.drill)}, {"through", num(p.throughHole)}, {"npth", num(p.npth)}},
                           "pad " + ref + "." + std::to_string(p.padNumber), p.position, p.componentId});
        }
    } else if (kind == "part") {
        for (const auto& c : sch.components()) {
            if (!c.hasFootprint() || !c.pcb.placed || !pcb.onBoard(c)) continue;
            out.push_back({{{"ref", str(c.ref)}, {"value", str(c.value)}, {"footprint", str(c.footprintName())}, {"mpn", str(c.sourcing.mpn)},
                            {"x", num(c.pcb.position.x)}, {"y", num(c.pcb.position.y)}, {"rotation", num(c.pcb.rotation)},
                            {"bottom", num(c.pcb.bottom)}, {"dnp", num(c.sourcing.dnp)}},
                           c.ref, c.pcb.position, c.id});
        }
    } else if (kind == "net") {
        struct Sum { double length = 0, minWidth = 0; int tracks = 0, vias = 0, pads = 0; Vec2 at; bool placed = false; };
        std::vector<Sum> sums(nets.size());
        auto in = [&](int n) { return n >= 0 && n < static_cast<int>(sums.size()); };
        for (const Track& t : pcb.tracks)
            if (in(t.net)) {
                Sum& m = sums[static_cast<size_t>(t.net)];
                m.length += trackLength(t), m.minWidth = m.tracks++ ? std::min(m.minWidth, t.width) : t.width;
            }
        for (const Via& v : pcb.vias)
            if (in(v.net)) ++sums[static_cast<size_t>(v.net)].vias;
        for (const Pad& p : pcb.pads(sch))
            if (in(p.net)) {
                Sum& m = sums[static_cast<size_t>(p.net)];
                if (!m.pads++) m.at = p.position;
            }
        for (size_t n = 0; n < nets.size(); ++n) {
            const Sum& m = sums[n];
            out.push_back({{{"net", str(nets[n].name)}, {"length", num(m.length)}, {"tracks", num(m.tracks)}, {"vias", num(m.vias)},
                            {"pads", num(m.pads)}, {"minWidth", num(m.minWidth)}},
                           "net " + nets[n].name, m.at});
        }
    }
    return out;
}

const std::vector<std::string>& objectKinds() {
    static const std::vector<std::string> k = {"track", "via", "pad", "part", "net"};
    return k;
}

std::string code(const std::string& name) {
    std::string c = "RULE_";
    for (char ch : name) c += std::isalnum(static_cast<unsigned char>(ch)) ? static_cast<char>(std::toupper(static_cast<unsigned char>(ch))) : '_';
    return c.substr(0, 64);
}

/// Reads both expressions and checks them against a sample of the object's properties (names exist, types fit).
std::string check(const CustomRule& r, std::vector<Token>& when, std::vector<Token>& require) {
    if (std::find(objectKinds().begin(), objectKinds().end(), r.object) == objectKinds().end())
        return "unknown object \"" + r.object + "\" (track, via, pad, part or net)";
    std::string error;
    if (!tokenize(r.when, when, error)) return "when: " + error;
    if (!tokenize(r.require, require, error)) return "require: " + error;
    if (require.empty()) return "require: empty";
    // Every property of the object with a value of its type: unknown names and type mistakes show at once.
    static const std::map<std::string, std::vector<std::pair<const char*, bool>>> names = {  // (name, text)
        {"track", {{"net", true}, {"width", false}, {"length", false}, {"layer", true}, {"layerIndex", false}, {"arc", false}, {"locked", false}}},
        {"via", {{"net", true}, {"drill", false}, {"diameter", false}, {"ring", false}, {"through", false}}},
        {"pad", {{"net", true}, {"ref", true}, {"pin", false}, {"width", false}, {"height", false}, {"drill", false}, {"through", false}, {"npth", false}}},
        {"part", {{"ref", true}, {"value", true}, {"footprint", true}, {"mpn", true}, {"x", false}, {"y", false}, {"rotation", false}, {"bottom", false}, {"dnp", false}}},
        {"net", {{"net", true}, {"length", false}, {"tracks", false}, {"vias", false}, {"pads", false}, {"minWidth", false}}}};
    Props sample;
    for (const auto& [name, text] : names.at(r.object)) sample[name] = text ? str("") : num(0);
    holds(when, sample, error);
    if (!error.empty()) return "when: " + error;
    holds(require, sample, error);
    return error.empty() ? "" : "require: " + error;
}

}  // namespace

std::string customRuleError(const CustomRule& rule) {
    std::vector<Token> a, b;
    return check(rule, a, b);
}

std::vector<RuleViolation> customRuleChecks(const Schematic& sch, const PcbLayout& pcb) {
    std::vector<RuleViolation> out;
    std::map<std::string, std::vector<Object>> cache;
    for (const CustomRule& r : pcb.settings.customRules) {
        std::vector<Token> when, require;
        const std::string error = check(r, when, require);
        RuleViolation v;
        v.code = code(r.name);
        v.severity = r.severity == "warning" ? Severity::Warning : r.severity == "info" ? Severity::Info : Severity::Error;
        if (!error.empty()) {
            v.code = "RULE_SYNTAX";
            v.severity = Severity::Error;
            v.message = "Rule \"" + r.name + "\" cannot be read: " + error;
            out.push_back(v);
            continue;
        }
        auto& objs = cache.try_emplace(r.object, objects(r.object, sch, pcb)).first->second;
        int count = 0;
        for (const Object& o : objs) {
            std::string e;
            if (!holds(when, o.props, e) || !e.empty() || holds(require, o.props, e) || !e.empty()) continue;
            if (++count > 200) break;
            RuleViolation x = v;
            x.message = (r.message.empty() ? r.name + ": " + r.require + " fails" : r.message) + " (" + o.what + ")";
            x.location = o.at, x.hasLocation = true;
            if (o.component >= 0) x.components = {o.component};
            out.push_back(std::move(x));
        }
    }
    return out;
}

Json customRulesToJson(const std::vector<CustomRule>& rules) {
    Json a = Json::array();
    for (const CustomRule& r : rules) {
        Json j = Json::object();
        j["name"] = r.name, j["object"] = r.object, j["when"] = r.when, j["require"] = r.require, j["severity"] = r.severity;
        if (!r.message.empty()) j["message"] = r.message;
        a.push(j);
    }
    return a;
}

std::vector<CustomRule> customRulesFromJson(const Json& j) {
    std::vector<CustomRule> out;
    for (const Json& e : j.items()) {
        if (!e.isObject() || out.size() >= 500) continue;
        CustomRule r;
        auto text = [&](const char* key, const std::string& fallback) { return e.get(key).asString(fallback).substr(0, 2000); };
        r.name = text("name", "rule"), r.object = text("object", "track"), r.when = text("when", "");
        r.require = text("require", ""), r.severity = text("severity", "error"), r.message = text("message", "");
        out.push_back(std::move(r));
    }
    return out;
}

}  // namespace sieda
