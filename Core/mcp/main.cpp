// sieda-mcp — SiEDA as a Model Context Protocol server over stdio (docs/MCP.md).
//
// Newline-delimited JSON-RPC 2.0 on stdin / stdout (one message per line); logs go to stderr only.
//
//   sieda-mcp [--root <dir>] [--project <file>] [--read-only] [--tools <list>] [--docs <dir>]
//   sieda-mcp --diff <old> <new> [--json] what changed between two project files (exit 1 when they differ)
//   sieda-mcp --git-diff <git's 7 args>  the same as Git's external diff driver (docs/TEAM.md)
//   sieda-mcp --merge <base> <ours> <theirs>  three-way merge into <ours> (Git's merge driver; exit 1 on conflicts)
//   sieda-mcp --list-tools-markdown      print the tool reference (docs/MCP.md) and exit
//   sieda-mcp --list-tools               print the tool table as JSON and exit
//   sieda-mcp --connect <url> [--token <t>] [--timeout <s>]
//                                        bridge stdio to the SiEDA app's live endpoint (token: --token or the
//                                        SIEDA_MCP_TOKEN environment variable; see http_bridge.hpp)
//
//   --root <dir>       folder the server may open projects from and write outputs into (default: the current folder)
//   --project <file>   project to open at start (inside the root)
//   --read-only        refuse every tool that changes the design, and every file write
//   --tools <list>     comma-separated groups / tool names / prefixes* to offer (default all), e.g. "project,schematic,pcb"
//   --docs <dir>       folder of the Markdown guides served as resources (default: the repository's docs/)
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#endif

#include "http_bridge.hpp"
#include "sieda/Mcp.hpp"
#include "sieda/sieda_c.h"

namespace fs = std::filesystem;

namespace {
void usage() {
    std::fprintf(stderr,
                 "usage: sieda-mcp [--root <dir>] [--project <file>] [--read-only] [--tools <list>] [--docs <dir>]\n"
                 "       sieda-mcp --connect http://127.0.0.1:39717/mcp [--token <t>] [--timeout <s>]\n"
                 "       sieda-mcp --diff <old.siedaproj> <new.siedaproj> [--json]\n"
                 "       sieda-mcp --merge <base> <ours> <theirs>   (writes the result into <ours>)\n"
                 "       sieda-mcp --list-tools-markdown | --list-tools | --version\n"
                 "Speaks the Model Context Protocol (JSON-RPC 2.0) on stdin/stdout. See docs/MCP.md.\n");
}

std::string defaultDocsDir(const char* argv0) {
    std::error_code ec;
#ifdef SIEDA_DOCS_DIR
    if (fs::is_directory(SIEDA_DOCS_DIR, ec)) return SIEDA_DOCS_DIR;
#endif
    const fs::path exe = fs::weakly_canonical(fs::absolute(argv0, ec), ec);
    for (const fs::path& candidate : {exe.parent_path() / "docs", exe.parent_path().parent_path() / "docs",
                                      exe.parent_path().parent_path() / "share" / "sieda" / "docs"})
        if (fs::is_directory(candidate, ec)) return candidate.string();
    return std::string();
}
/// A project file's JSON; a missing or empty file (Git's /dev/null for an added file) is an empty project.
std::string projectText(const char* path) {
    std::ifstream in(path, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str().find_first_not_of(" \t\r\n") == std::string::npos ? "{\"format\":\"sieda-project\"}" : ss.str();
}

int diff(const char* before, const char* after, bool json, const char* title) {
    const std::string a = projectText(before), b = projectText(after);
    char* d = sieda_diff_projects(a.c_str(), b.c_str(), json ? 0 : 1);
    const std::string text = d ? d : "";
    sieda_string_free(d);
    if (title) std::printf("SiEDA diff %s\n", title);
    std::fputs(text.c_str(), stdout);
    if (json) std::fputc('\n', stdout);
    return text.rfind("No changes.", 0) == 0 || text.find("\"identical\":true") != std::string::npos ? 0 : 1;
}
/// Git merge driver: merges into `ours`; conflicts (ours kept) are listed on stderr and exit 1 so Git marks the file.
int merge(const char* base, const char* ours, const char* theirs) {
    char* report = nullptr;
    const int conflicts = sieda_merge_project_files(base, ours, theirs, &report);
    if (report) std::fputs(report, stderr);
    sieda_string_free(report);
    return conflicts == 0 ? 0 : conflicts > 0 ? 1 : 2;
}
}  // namespace

int main(int argc, char** argv) {
#ifdef _WIN32
    _setmode(_fileno(stdin), _O_BINARY);
    _setmode(_fileno(stdout), _O_BINARY);
#endif
    sieda::mcp::McpOptions options;
    std::string project;
    std::string connectUrl;
    std::string token;
    int timeoutSeconds = 600;  // long tools (autoroute, Monte Carlo) run inside one request
    std::error_code ec;
    options.allowedRoot = fs::current_path(ec).string();
    options.docsDir = defaultDocsDir(argv[0]);
    if (argc >= 4 && std::strcmp(argv[1], "--diff") == 0)
        return diff(argv[2], argv[3], argc >= 5 && std::strcmp(argv[4], "--json") == 0, nullptr);
    if (argc >= 5 && std::strcmp(argv[1], "--merge") == 0) return merge(argv[2], argv[3], argv[4]);
    if (argc >= 7 && std::strcmp(argv[1], "--git-diff") == 0) {  // path old-file old-hex old-mode new-file …
        diff(argv[3], argv[6], false, argv[2]);
        return 0;  // Git stops a multi-file diff on a non-zero exit
    }
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sieda-mcp: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--root") {
            options.allowedRoot = fs::absolute(value(), ec).string();
        } else if (a == "--project") {
            project = value();
        } else if (a == "--read-only") {
            options.readOnly = true;
        } else if (a == "--tools") {
            std::stringstream ss(value());
            for (std::string t; std::getline(ss, t, ',');)
                if (!t.empty()) options.toolFilter.push_back(t);
        } else if (a == "--docs") {
            options.docsDir = value();
        } else if (a == "--connect") {
            connectUrl = value();
        } else if (a == "--token") {
            token = value();
        } else if (a == "--timeout") {
            timeoutSeconds = std::atoi(value().c_str());
            if (timeoutSeconds <= 0) timeoutSeconds = 600;
        } else if (a == "--list-tools-markdown") {
            std::fputs(sieda::mcp::mcpToolsMarkdown().c_str(), stdout);
            return 0;
        } else if (a == "--list-tools") {
            char* json = sieda_mcp_tools_json();
            std::puts(json ? json : "[]");
            sieda_string_free(json);
            return 0;
        } else if (a == "--version") {
            std::printf("sieda-mcp %s (MCP %s)\n", sieda_version(), sieda::mcp::kProtocolVersion);
            return 0;
        } else if (a == "--help" || a == "-h") {
            usage();
            return 0;
        } else {
            std::fprintf(stderr, "sieda-mcp: unknown argument %s\n", a.c_str());
            usage();
            return 2;
        }
    }
    if (!connectUrl.empty()) {
        if (token.empty()) {
            const char* env = std::getenv("SIEDA_MCP_TOKEN");
            if (env) token = env;
        }
        return sieda::mcpbridge::runBridge(connectUrl, token, timeoutSeconds);
    }
    if (!fs::is_directory(options.allowedRoot, ec)) {
        std::fprintf(stderr, "sieda-mcp: root folder %s does not exist\n", options.allowedRoot.c_str());
        return 2;
    }

    sieda::mcp::McpServer server(options);
    if (!project.empty()) {
        try {
            server.openProject(project);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sieda-mcp: cannot open %s: %s\n", project.c_str(), e.what());
            return 2;
        }
    }
    std::fprintf(stderr, "sieda-mcp %s ready (root %s%s, %zu tools)\n", sieda_version(), options.allowedRoot.c_str(),
                 options.readOnly ? ", read-only" : "", sieda::mcp::mcpTools().size());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const std::string response = server.handle(line);
        if (response.empty()) continue;
        std::fwrite(response.data(), 1, response.size(), stdout);
        std::fputc('\n', stdout);
        std::fflush(stdout);
    }
    return 0;
}
