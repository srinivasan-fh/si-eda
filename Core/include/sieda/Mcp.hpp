// SiEDA Core — Model Context Protocol (MCP) engine (docs/MCP.md).
//
// A transport-independent JSON-RPC 2.0 handler that lets any MCP client (Claude Desktop, Claude Code, Cursor,
// VS Code, ChatGPT desktop, local LLM hosts) drive SiEDA: one request line in, one response line out. The headless
// stdio server (Core/mcp/main.cpp, executable `sieda-mcp`) and the app's live endpoint both sit on top of it.
//
// Protocol revision "2025-06-18" (also answers "2025-03-26" and "2024-11-05" clients in their own revision).
// Methods: initialize, notifications/initialized, ping, tools/list, tools/call, resources/list, resources/read,
// resources/templates/list, prompts/list, prompts/get. Tool failures come back as results with isError: true;
// protocol errors use the JSON-RPC codes -32700, -32600, -32601, -32602, -32603 (and -32002 for an unknown resource).
//
// The tool table is data (mcpTools()): name, group, description, JSON Schema, annotations and a handler, so the app
// can list and document the same tools. A server holds one session: the open project (its own, or one attached by
// the app), the project's file path and the options below. Not thread-safe: call handle() from one thread at a time,
// the thread that owns an attached project.
#pragma once

#include <functional>
#include <string>
#include <vector>

#include "sieda/Json.hpp"

struct SiedaProject;  // sieda_c.h

namespace sieda::mcp {

/// The MCP protocol revision this server speaks by default, and every revision it accepts.
extern const char* const kProtocolVersion;          // "2025-06-18"
const std::vector<std::string>& supportedProtocolVersions();

struct McpOptions {
    /// Tools that change the design or the session (and every file write) are refused.
    bool readOnly = false;
    /// Folder the server may read projects from and write outputs into (relative paths resolve against it). Empty:
    /// no file access at all (opening, saving and writing outputs are refused; results still come back inline).
    std::string allowedRoot;
    /// Tools offered: names ("pcb_autoroute"), groups ("pcb") or prefixes ending in '*' ("sim_*"). Empty = all.
    std::vector<std::string> toolFilter;
    /// Folder of the Markdown guides served as sieda://docs/<name> (docs/*.md). Empty: only the built-in guide.
    std::string docsDir;
    /// Server name reported by initialize (the app may say "sieda-app").
    std::string serverName = "sieda";

    static McpOptions fromJson(const Json& j);  // {"readOnly","allowedRoot","toolFilter":[…],"docsDir","serverName"}
    Json toJson() const;
};

/// A tool's failure (bad arguments, unknown part, refused path …): reported as an isError result, never as a crash.
struct ToolError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class McpServer;

/// What a tool returns: `data` is sent as JSON text (and as structuredContent when it is an object); `content` adds
/// further MCP content items (images, embedded resources) after it.
struct ToolOutput {
    Json data;
    Json content = Json::array();
};

struct McpTool {
    std::string name;         // "schematic_add_component"
    std::string group;        // "project", "schematic", "library", "pcb", "sim", "si_pi", "verify", "output", "render"
    std::string title;
    std::string description;
    Json inputSchema;         // JSON Schema of the arguments (type "object")
    bool readOnly = true;     // annotations.readOnlyHint: changes neither the design nor any file
    bool destructive = false; // annotations.destructiveHint: may remove or replace work (or overwrite files)
    bool idempotent = false;  // annotations.idempotentHint
    bool mutates = false;     // changes the design or the session: refused in read-only mode
    std::function<ToolOutput(McpServer&, const Json&)> handler;
};

/// The full tool table, in group order.
const std::vector<McpTool>& mcpTools();
/// The groups, in order, with a one-line description each.
const std::vector<std::pair<std::string, std::string>>& mcpToolGroups();
/// Markdown table of every tool (docs/MCP.md is generated from it: `sieda-mcp --list-tools-markdown`).
std::string mcpToolsMarkdown();

class McpServer {
public:
    explicit McpServer(McpOptions options = {});
    ~McpServer();
    McpServer(const McpServer&) = delete;
    McpServer& operator=(const McpServer&) = delete;

    /// One JSON-RPC message (or a batch array) in; the response line out, "" when nothing is to be answered
    /// (notifications, responses). Never throws.
    std::string handle(const std::string& requestLine);
    /// The same for a parsed message; `respond` is false when no response is due.
    Json handleMessage(const Json& message, bool* respond);

    /// Binds the server to a project owned by someone else (the app's open document). nullptr returns to the
    /// server's own project. The server never frees an attached project.
    void attachProject(SiedaProject* project);
    SiedaProject* project() { return project_; }
    const SiedaProject* project() const { return project_; }
    bool attached() const { return !ownsProject_; }

    /// File the current project was opened from / saved to ("" when none), absolute.
    const std::string& projectPath() const { return projectPath_; }
    void setProjectPath(std::string path) { projectPath_ = std::move(path); }
    /// Opens a project file (absolute, or relative to the allowed root). Throws ToolError.
    void openProject(const std::string& path);

    const McpOptions& options() const { return options_; }
    void setOptions(McpOptions options) { options_ = std::move(options); }
    /// Called after every tool that changed the design or the session (tool name), e.g. to refresh the app's views.
    void setChangeHandler(std::function<void(const std::string&)> handler) { onChange_ = std::move(handler); }

    /// A path inside the allowed root (relative paths resolve against it; "../" escapes, absolute paths elsewhere and
    /// symbolic links out of the root are refused). `write` also requires the server not to be read-only. Throws
    /// ToolError.
    std::string resolvePath(const std::string& path, bool write) const;
    /// Writes a file inside the root (parent folders created). Returns the absolute path. Throws ToolError.
    std::string writeFile(const std::string& path, const std::string& content) const;

    bool toolEnabled(const McpTool& tool) const;
    const std::string& protocolVersion() const { return protocol_; }
    /// Notes for this session (e.g. the last tool's warnings): appended to tool results by handlers.
    std::vector<std::string>& sessionLog() { return log_; }

    Json toolsListJson() const;
    Json resourcesListJson() const;
    Json readResource(const std::string& uri) const;  // throws ToolError for an unknown resource
    Json promptsListJson() const;
    Json getPrompt(const std::string& name, const Json& arguments) const;  // throws ToolError
    /// Runs a tool and wraps the result as an MCP CallToolResult. Unknown tools throw std::out_of_range.
    Json callTool(const std::string& name, const Json& arguments);

private:
    McpOptions options_;
    SiedaProject* project_ = nullptr;
    bool ownsProject_ = true;
    std::string projectPath_;
    std::string protocol_;
    bool initialized_ = false;
    std::function<void(const std::string&)> onChange_;
    std::vector<std::string> log_;
};

/// The built-in example designs (project_list_examples / project_load_example): {id, title, description}.
struct McpExample {
    std::string id, title, description;
};
const std::vector<McpExample>& mcpExamples();

/// Render the schematic (one sheet; 0 = the active sheet) or the board as SVG text, or as PNG bytes.
/// layers: copper layers to draw on the board (empty = all). width: image width in pixels (PNG) / user units (SVG).
std::string renderSchematicSvg(const SiedaProject& project, int sheet, int width);
std::string renderSchematicPng(const SiedaProject& project, int sheet, int width);
std::string renderPcbSvg(const SiedaProject& project, const std::vector<int>& layers, int width);
std::string renderPcbPng(const SiedaProject& project, const std::vector<int>& layers, int width);

std::string base64Encode(const std::string& bytes);

}  // namespace sieda::mcp
