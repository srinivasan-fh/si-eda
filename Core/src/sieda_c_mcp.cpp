// SiEDA Core — C ABI of the MCP engine (docs/MCP.md): the app's live endpoint and other hosts drive McpServer through
// these calls. Every entry point is exception-safe.
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>

#include "SiedaProjectInternal.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mcp.hpp"
#include "sieda/sieda_c.h"

struct SiedaMcpServer {
    explicit SiedaMcpServer(sieda::mcp::McpOptions o) : server(std::move(o)) {}
    sieda::mcp::McpServer server;
};

namespace {
char* dupMcp(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

bool parseOptions(const char* options_json, sieda::mcp::McpOptions* out) {
    if (!options_json || !*options_json) return true;
    try {
        const sieda::Json j = sieda::Json::parse(options_json);
        if (!j.isObject()) return false;
        *out = sieda::mcp::McpOptions::fromJson(j);
        return true;
    } catch (...) {
        return false;
    }
}
}  // namespace

extern "C" {

SiedaMcpServer* sieda_mcp_new(const char* options_json) {
    try {
        sieda::mcp::McpOptions o;
        if (!parseOptions(options_json, &o)) return nullptr;
        return new SiedaMcpServer(std::move(o));
    } catch (...) {
        return nullptr;
    }
}

void sieda_mcp_free(SiedaMcpServer* server) { delete server; }

char* sieda_mcp_handle(SiedaMcpServer* server, const char* request_json) {
    if (!server) return nullptr;
    try {
        return dupMcp(server->server.handle(request_json ? request_json : ""));
    } catch (...) {
        return dupMcp("{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":-32603,\"message\":\"Internal error\"}}");
    }
}

int32_t sieda_mcp_attach_project(SiedaMcpServer* server, SiedaProject* project) {
    if (!server) return 0;
    try {
        server->server.attachProject(project);
        return 1;
    } catch (...) {
        return 0;
    }
}

int32_t sieda_mcp_set_options(SiedaMcpServer* server, const char* options_json) {
    if (!server) return 0;
    sieda::mcp::McpOptions o;
    if (!parseOptions(options_json, &o)) return 0;
    try {
        server->server.setOptions(std::move(o));
        return 1;
    } catch (...) {
        return 0;
    }
}

void sieda_mcp_set_change_callback(SiedaMcpServer* server, SiedaMcpChangeCallback callback, void* user) {
    if (!server) return;
    try {
        if (callback) server->server.setChangeHandler([callback, user](const std::string& tool) { callback(user, tool.c_str()); });
        else server->server.setChangeHandler(nullptr);
    } catch (...) {
    }
}

int32_t sieda_mcp_set_project_path(SiedaMcpServer* server, const char* path) {
    if (!server) return 0;
    try {
        server->server.setProjectPath(path ? path : "");
        return 1;
    } catch (...) {
        return 0;
    }
}

char* sieda_mcp_tools_json(void) {
    try {
        sieda::Json list = sieda::Json::array();
        for (const auto& t : sieda::mcp::mcpTools()) {
            sieda::Json j = sieda::Json::object();
            j["name"] = t.name;
            j["group"] = t.group;
            j["title"] = t.title;
            j["description"] = t.description;
            j["inputSchema"] = t.inputSchema;
            j["readOnly"] = t.readOnly;
            j["destructive"] = t.destructive;
            j["idempotent"] = t.idempotent;
            j["mutates"] = t.mutates;
            list.push(j);
        }
        return dupMcp(list.dump());
    } catch (...) {
        return dupMcp("[]");
    }
}

}  // extern "C"
