// SiEDA Core — MCP engine internals shared by Mcp.cpp (protocol, resources, prompts) and McpTools.cpp (the tools).
#pragma once

#include <cstdlib>
#include <initializer_list>
#include <string>
#include <vector>

#include "SiedaProjectInternal.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mcp.hpp"
#include "sieda/sieda_c.h"

namespace sieda::mcp {

/// Builds the tool table (McpTools.cpp).
std::vector<McpTool> buildTools();
/// Loads a built-in example into `project` (Mcp.cpp). False for an unknown id.
bool loadExample(Project& project, const std::string& id);

/// JSON Schema builder for tool arguments.
class Schema {
public:
    Schema& str(const std::string& name, const std::string& desc, bool required = false,
                std::initializer_list<const char*> values = {}) {
        Json p = Json::object();
        p["type"] = "string";
        p["description"] = desc;
        if (values.size()) {
            Json e = Json::array();
            for (const char* v : values) e.push(v);
            p["enum"] = e;
        }
        return add(name, p, required);
    }
    Schema& num(const std::string& name, const std::string& desc, bool required = false) {
        return add(name, typed("number", desc), required);
    }
    Schema& integer(const std::string& name, const std::string& desc, bool required = false) {
        return add(name, typed("integer", desc), required);
    }
    Schema& boolean(const std::string& name, const std::string& desc, bool required = false) {
        return add(name, typed("boolean", desc), required);
    }
    Schema& object(const std::string& name, const std::string& desc, bool required = false, Json properties = Json()) {
        Json p = typed("object", desc);
        if (properties.isObject()) p["properties"] = properties;
        return add(name, p, required);
    }
    /// Array of `itemType` ("string", "number", "integer", "object", "array"; "" = anything).
    Schema& array(const std::string& name, const std::string& desc, const std::string& itemType = "", bool required = false,
                  Json itemSchema = Json()) {
        Json p = typed("array", desc);
        if (itemSchema.isObject()) {
            p["items"] = itemSchema;
        } else if (!itemType.empty()) {
            Json it = Json::object();
            it["type"] = itemType;
            if (itemType == "array") {  // nested arrays (pairs) hold strings; strict clients want "items" everywhere
                Json inner = Json::object();
                inner["type"] = "string";
                it["items"] = inner;
            }
            p["items"] = it;
        }
        return add(name, p, required);
    }
    /// A pin: "R1.2" / "U1.PA0" (designator, a dot, pin name or number).
    Schema& pin(const std::string& name, const std::string& desc, bool required = false) {
        return str(name, desc + " Written REF.PIN, e.g. \"R1.1\", \"D1.A\", \"U1.VCC\" or \"U1.14\".", required);
    }
    /// Any JSON value (passed through to the core).
    Schema& any(const std::string& name, const std::string& desc, bool required = false) {
        Json p = Json::object();
        p["description"] = desc;
        return add(name, p, required);
    }
    Json build() const {
        Json s = Json::object();
        s["type"] = "object";
        s["properties"] = props_;
        if (req_.size()) s["required"] = req_;
        return s;
    }
    operator Json() const { return build(); }

private:
    static Json typed(const char* type, const std::string& desc) {
        Json p = Json::object();
        p["type"] = type;
        p["description"] = desc;
        return p;
    }
    Schema& add(const std::string& name, Json p, bool required) {
        props_[name] = std::move(p);
        if (required) req_.push(name);
        return *this;
    }
    Json props_ = Json::object();
    Json req_ = Json::array();
};

/// Checks arguments against a tool schema (required members, types, enums). Returns "" or the first problem.
std::string validateArguments(const Json& schema, const Json& args);

/// Takes a malloc'ed C API string (may be NULL), frees it and parses it. NULL gives null.
Json takeJson(char* s);
/// Takes a malloc'ed C API string, frees it and returns its text ("" for NULL).
std::string takeText(char* s);

// ---- argument helpers (throw ToolError with a helpful message) ----
std::string argStr(const Json& a, const std::string& key, const std::string& def = "");
double argNum(const Json& a, const std::string& key, double def);
int argInt(const Json& a, const std::string& key, int def);
bool argBool(const Json& a, const std::string& key, bool def);
std::string requireStr(const Json& a, const std::string& key);
/// Engineering text ("10k", "1MEG") or a number.
double argValue(const Json& a, const std::string& key, double def);

/// The component with designator `ref` (or a numeric id). Throws ToolError listing the designators in use.
int componentId(const SiedaProject& p, const Json& refOrId);
struct PinAddress {
    int component = -1;
    int pin = -1;
};
/// "R1.2" / {"ref":"R1","pin":"2"}: the component and its pin index. Throws ToolError listing the part's pins.
PinAddress pinAddress(const SiedaProject& p, const Json& spec);
std::string pinLabel(const SiedaProject& p, PinAddress a);
/// Component description for results: {id, ref, kind, value, sheet, pins:[{name, number, net}]}.
Json componentJson(const SiedaProject& p, int id, bool withPins = true);
/// Built-in component kind by name ("resistor", "LED", "R", "voltage_source", …) or number. -1 if unknown.
int kindFromName(const Json& kind);
std::string kindName(int kind);
/// Violations JSON array summarised: {errors, warnings, infos, violations:[…]} (at most `limit` listed).
Json summariseViolations(const SiedaProject& p, const Json& violations, size_t limit = 100);

}  // namespace sieda::mcp
