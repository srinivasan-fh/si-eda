// sieda-studio — SiEDA's desktop app on Linux and Windows (and macOS): the core serves its interface on this computer's
// loopback interface and opens it in the system browser. Every action is an MCP tool call (docs/STUDIO.md).
//
//   sieda-studio [--root <dir>] [--project <file>] [--port <n>] [--no-browser] [--library-dir <dir> …]
//
//   --root <dir>      folder projects are opened from and outputs written to (default: your home folder)
//   --project <file>  project to open at start
//   --port <n>        port on 127.0.0.1 (default: any free port)
//   --no-browser      print the address without opening a browser
//   --library-dir     a KiCad library folder to search and place parts from (read-only; also SIEDA_LIBRARY_DIRS)
//   --activity-log <dir> / --no-activity-log   where the dated activity log goes (default the per-user log folder):
//                     the page's actions are logged as "user", an AI client's on /mcp by its name
//   --lessons-dir <dir> an organisation's lesson folder (<name>.lesson.json); also SIEDA_LESSONS_DIRS
//
// Security: it listens on 127.0.0.1 only and every request needs the random token in the printed address (the page
// sends it back as a header, which other web pages cannot add without a CORS preflight this server never answers);
// the Host header must name the loopback address (DNS rebinding). /mcp takes the same token as a bearer token, so
// `sieda-mcp --connect http://127.0.0.1:<port>/mcp --token <token>` lets an AI client edit the open design live.
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <random>
#include <sstream>
#include <string>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
using Socket = SOCKET;
#define closeSocket closesocket
#else
#include <arpa/inet.h>
#include <poll.h>
#include <signal.h>
#include <netinet/in.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
using Socket = int;
#define closeSocket close
#define INVALID_SOCKET (-1)
#endif

#include "sieda/ExternalLibrary.hpp"
#include "sieda/Json.hpp"
#include "sieda/Mcp.hpp"

namespace fs = std::filesystem;
using sieda::Json;

extern const char* const kStudioHtml;  // ui.html, embedded at build time (Core/studio/embed.cmake)
extern const char* const kStudioI18n;  // i18n.json: the interface translations (tools/make_studio_i18n.py)

namespace {

constexpr size_t kMaxHeader = 64 * 1024, kMaxBody = 64u * 1024 * 1024;

struct Request {
    std::string method, path, query, body;
    std::vector<std::pair<std::string, std::string>> headers;  // names lower case
    std::string header(const std::string& name) const {
        for (const auto& [k, v] : headers)
            if (k == name) return v;
        return {};
    }
};

bool sendAll(Socket s, const std::string& data) {
    size_t at = 0;
    while (at < data.size()) {
        const int n = static_cast<int>(send(s, data.data() + at, static_cast<int>(std::min<size_t>(data.size() - at, 1 << 20)), 0));
        if (n <= 0) return false;
        at += static_cast<size_t>(n);
    }
    return true;
}

void reply(Socket s, int status, const std::string& type, const std::string& body) {
    const char* text = status == 200 ? "OK" : status == 403 ? "Forbidden" : status == 404 ? "Not Found"
                     : status == 413 ? "Payload Too Large" : "Bad Request";
    sendAll(s, "HTTP/1.1 " + std::to_string(status) + " " + text + "\r\nContent-Type: " + type +
                   "\r\nContent-Length: " + std::to_string(body.size()) +
                   "\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\nX-Frame-Options: DENY\r\n"
                   "Referrer-Policy: no-referrer\r\nContent-Security-Policy: default-src 'none'; img-src data:; "
                   "style-src 'unsafe-inline'; script-src 'unsafe-inline'; connect-src 'self'; frame-ancestors 'none'\r\n"
                   "Connection: close\r\n\r\n" + body);
}

/// Reads one request (headers capped at 64 KB, body at 64 MB). False on a malformed or oversized one.
bool readRequest(Socket s, Request& r, int& error) {
    std::string data;
    char buf[65536];
    size_t end = std::string::npos;
    while ((end = data.find("\r\n\r\n")) == std::string::npos) {
        if (data.size() > kMaxHeader) return error = 413, false;
        const int n = static_cast<int>(recv(s, buf, sizeof buf, 0));
        if (n <= 0) return error = 400, false;
        data.append(buf, static_cast<size_t>(n));
    }
    const std::string head = data.substr(0, end);
    size_t lineEnd = head.find("\r\n");
    const std::string first = head.substr(0, lineEnd);
    const size_t sp1 = first.find(' '), sp2 = first.find(' ', sp1 + 1);
    if (sp1 == std::string::npos || sp2 == std::string::npos) return error = 400, false;
    r.method = first.substr(0, sp1);
    const std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    const size_t q = target.find('?');
    r.path = target.substr(0, q);
    r.query = q == std::string::npos ? "" : target.substr(q + 1);
    while (lineEnd != std::string::npos && lineEnd < head.size()) {
        const size_t next = head.find("\r\n", lineEnd + 2);
        const std::string line = head.substr(lineEnd + 2, next == std::string::npos ? std::string::npos : next - lineEnd - 2);
        const size_t colon = line.find(':');
        if (colon != std::string::npos) {
            std::string name = line.substr(0, colon), value = line.substr(colon + 1);
            for (char& c : name) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            value.erase(0, value.find_first_not_of(" \t"));
            r.headers.emplace_back(name, value);
        }
        lineEnd = next;
    }
    if (!r.header("transfer-encoding").empty()) return error = 400, false;  // the page always sends a length
    const std::string lengthText = r.header("content-length");
    const unsigned long long length = lengthText.empty() ? 0 : std::strtoull(lengthText.c_str(), nullptr, 10);
    if (length > kMaxBody) return error = 413, false;
    r.body = data.substr(end + 4);
    while (r.body.size() < length) {
        const int n = static_cast<int>(recv(s, buf, sizeof buf, 0));
        if (n <= 0) return error = 400, false;
        r.body.append(buf, static_cast<size_t>(n));
    }
    r.body.resize(static_cast<size_t>(length));
    return true;
}

std::string randomToken() {
    std::random_device rd;
    static const char* hex = "0123456789abcdef";
    std::string t;
    for (int i = 0; i < 32; ++i) t += hex[rd() & 15];
    return t;
}

/// Same length and bytes, without stopping at the first difference.
bool sameToken(const std::string& a, const std::string& b) {
    if (a.size() != b.size() || a.empty()) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < a.size(); ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}

std::string queryValue(const std::string& query, const std::string& key) {
    for (size_t at = 0; at <= query.size();) {
        const size_t amp = query.find('&', at);
        const std::string pair = query.substr(at, amp == std::string::npos ? std::string::npos : amp - at);
        if (pair.rfind(key + "=", 0) == 0) return pair.substr(key.size() + 1);
        if (amp == std::string::npos) break;
        at = amp + 1;
    }
    return {};
}

void openBrowser(const std::string& url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char* opener = "open";
#else
    const char* opener = "xdg-open";
#endif
    char* argv[] = {const_cast<char*>(opener), const_cast<char*>(url.c_str()), nullptr};
    pid_t pid = 0;
    if (posix_spawnp(&pid, opener, nullptr, nullptr, argv, environ) != 0)
        std::fprintf(stderr, "sieda-studio: could not start %s; open the address above in a browser\n", opener);
#endif
}

/// Plug-ins (as the macOS app's ProjectPlugins): executables in the user's own plug-in folder only — never from a
/// project or a path a request names. One runs on a copy of the design in a fresh temporary folder (SIEDA_PROJECT names
/// the file, SIEDA_PLUGIN the plug-in); what it prints (first 64 KB) is shown and, when it changed the copy, the copy
/// replaces the design as one undo step. Sandboxed (the default): no network, writes only in that folder — bubblewrap
/// on Linux; sandboxing is not available on Windows, where a plug-in runs only with the sandbox turned off.
fs::path pluginFolder() {
#ifdef _WIN32
    const char* base = std::getenv("APPDATA");
    return fs::path(base ? base : ".") / "SiEDA" / "Plugins";
#else
    if (const char* xdg = std::getenv("XDG_DATA_HOME"); xdg && *xdg) return fs::path(xdg) / "SiEDA" / "Plugins";
    const char* home = std::getenv("HOME");
    return fs::path(home ? home : ".") / ".local" / "share" / "SiEDA" / "Plugins";
#endif
}

Json pluginList() {
    Json list = Json::array();
    std::error_code ec;
    std::vector<std::string> names;
    for (const auto& e : fs::directory_iterator(pluginFolder(), ec)) {
        if (!e.is_regular_file(ec) || e.is_symlink(ec)) continue;
        const std::string name = e.path().filename().string();
        if (name.empty() || name[0] == '.') continue;
#ifdef _WIN32
        const std::string ext = e.path().extension().string();
        if (ext != ".exe" && ext != ".bat" && ext != ".cmd") continue;
#else
        if ((e.status(ec).permissions() & fs::perms::owner_exec) == fs::perms::none) continue;
#endif
        names.push_back(name);
    }
    std::sort(names.begin(), names.end());
    for (const auto& n : names) list.push(n);
    return list;
}

#ifndef _WIN32
std::string findInPath(const char* tool) {
    const char* path = std::getenv("PATH");
    std::stringstream ss(path ? path : "/usr/bin:/bin");
    for (std::string d; std::getline(ss, d, ':');)
        if (!d.empty() && access((fs::path(d) / tool).c_str(), X_OK) == 0) return (fs::path(d) / tool).string();
    return {};
}
#endif

/// {"output", "changed", "json"?} or {"error"}.
Json runPlugin(const std::string& name, const std::string& design, bool sandbox) {
    Json r = Json::object();
    const Json known = pluginList();
    bool listed = false;
    for (const auto& n : known.items()) listed = listed || n.asString() == name;
    if (!listed) return r["error"] = "No plug-in " + name + " in " + pluginFolder().string(), r;
    const fs::path plugin = pluginFolder() / name;
    std::error_code ec;
    std::random_device rd;
    const fs::path dir = fs::temp_directory_path(ec) / ("sieda-plugin-" + std::to_string(rd()) + std::to_string(rd()));
    fs::create_directories(dir, ec);
    const fs::path file = dir / "project.siedaproj";
    { std::ofstream(file, std::ios::binary) << design; }
    std::string output;
    bool timedOut = false;
#ifdef _WIN32
    if (sandbox) {
        fs::remove_all(dir, ec);
        return r["error"] = "Sandboxing plug-ins is not available on Windows: turn off Settings → General → Sandbox plug-ins "
                            "to run them with your own rights.", r;
    }
    SetEnvironmentVariableA("SIEDA_PROJECT", file.string().c_str());
    SetEnvironmentVariableA("SIEDA_PLUGIN", name.c_str());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE readEnd = nullptr, writeEnd = nullptr;
    CreatePipe(&readEnd, &writeEnd, &sa, 0);
    SetHandleInformation(readEnd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = writeEnd;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    std::string cmd = "\"" + plugin.string() + "\"";
    const bool started = CreateProcessA(nullptr, cmd.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                                        dir.string().c_str(), &si, &pi);
    CloseHandle(writeEnd);
    if (!started) {
        CloseHandle(readEnd);
        fs::remove_all(dir, ec);
        return r["error"] = "Could not start " + name, r;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
        DWORD avail = 0;
        if (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail) {
            char buf[4096];
            DWORD got = 0;
            if (ReadFile(readEnd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) && got && output.size() < 65536)
                output.append(buf, got);
            continue;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) {
            DWORD got = 0;
            char buf[4096];
            while (PeekNamedPipe(readEnd, nullptr, 0, nullptr, &avail, nullptr) && avail &&
                   ReadFile(readEnd, buf, std::min<DWORD>(avail, sizeof buf), &got, nullptr) && got)
                if (output.size() < 65536) output.append(buf, got);
            break;
        }
        if (std::chrono::steady_clock::now() > deadline) {
            TerminateProcess(pi.hProcess, 1);
            timedOut = true;
            break;
        }
    }
    CloseHandle(readEnd);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
#else
    std::vector<std::string> args;
    if (sandbox) {
#ifdef __APPLE__
        const std::string sb = "/usr/bin/sandbox-exec";
        args = {sb, "-p",
                "(version 1)(allow default)(deny network*)(deny file-write*)(allow file-write* (subpath \"" + dir.string() +
                    "\") (subpath \"/private" + dir.string() + "\") (literal \"/dev/null\") (literal \"/dev/tty\"))",
                plugin.string()};
#else
        const std::string bwrap = findInPath("bwrap");
        if (bwrap.empty()) {
            fs::remove_all(dir, ec);
            return r["error"] = "Sandboxed plug-ins need bubblewrap (bwrap): install it (e.g. apt install bubblewrap), or "
                                "turn off Settings → General → Sandbox plug-ins to run them with your own rights.", r;
        }
        args = {bwrap, "--ro-bind", "/", "/", "--dev", "/dev", "--proc", "/proc", "--tmpfs", "/tmp", "--bind", dir.string(),
                dir.string(), "--unshare-net", "--die-with-parent", "--chdir", dir.string(), "--", plugin.string()};
#endif
    } else {
        args = {plugin.string()};
    }
    int fds[2];
    if (pipe(fds) != 0) return fs::remove_all(dir, ec), r["error"] = "Could not start " + name, r;
    const pid_t pid = fork();
    if (pid == 0) {
        dup2(fds[1], 1), dup2(fds[1], 2);
        close(fds[0]), close(fds[1]);
        if (chdir(dir.c_str()) != 0) _exit(126);
        setenv("SIEDA_PROJECT", file.c_str(), 1);
        setenv("SIEDA_PLUGIN", name.c_str(), 1);
        std::vector<char*> argv;
        for (auto& a : args) argv.push_back(a.data());
        argv.push_back(nullptr);
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(fds[1]);
    if (pid < 0) {
        close(fds[0]);
        fs::remove_all(dir, ec);
        return r["error"] = "Could not start " + name, r;
    }
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(120);
    for (;;) {
        pollfd p{fds[0], POLLIN, 0};
        const int left = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count());
        if (left <= 0) {
            kill(pid, SIGKILL);
            timedOut = true;
            break;
        }
        if (poll(&p, 1, std::min(left, 500)) <= 0) continue;
        char buf[4096];
        const ssize_t got = read(fds[0], buf, sizeof buf);
        if (got <= 0) break;
        if (output.size() < 65536) output.append(buf, static_cast<size_t>(got));
    }
    close(fds[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    if (!timedOut && WIFEXITED(status) && WEXITSTATUS(status) == 127 && output.empty()) output = "Could not start " + name;
#endif
    if (output.size() > 65536) output.resize(65536);
    if (timedOut) output += "\n(stopped after 120 s)";
    std::ifstream in(file, std::ios::binary);
    std::stringstream after;
    after << in.rdbuf();
    fs::remove_all(dir, ec);
    r["output"] = output;
    r["changed"] = after.str() != design && !timedOut;
    if (after.str() != design && !timedOut) r["json"] = after.str();
    return r;
}

/// Project files under the root (4 folders deep, at most 500), relative paths.
Json projectFiles(const std::string& root) {
    Json list = Json::array();
    std::error_code ec;
    fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
    for (int count = 0; !ec && it != end && count < 500; it.increment(ec)) {
        const std::string name = it->path().filename().string();
        if (it->is_directory(ec) && (it.depth() >= 3 || (!name.empty() && name[0] == '.'))) {
            it.disable_recursion_pending();
            continue;
        }
        if (it->path().extension() == ".siedaproj" && it->is_regular_file(ec)) {
            list.push(fs::relative(it->path(), root, ec).generic_string());
            ++count;
        }
    }
    return list;
}

}  // namespace

int main(int argc, char** argv) {
    sieda::mcp::McpOptions options;
    options.serverName = "sieda-studio";
    std::string project;
    int port = 0;
    bool browser = true;
    std::error_code ec;
    const char* home = std::getenv("HOME");
#ifdef _WIN32
    if (!home) home = std::getenv("USERPROFILE");
#endif
    options.allowedRoot = home ? std::string(home) : fs::current_path(ec).string();
    options.libraryDirs = sieda::ExternalLibraries::foldersFromEnvironment();
    options.lessonDirs = sieda::mcp::lessonDirsFromEnvironment();
    {   // Built-in AI models: beside the executable (release archives carry sieda-circuit-v1.gguf), share/sieda/models,
        // a build folder inside the source tree, then SIEDA_MODELS_DIRS.
        const fs::path exe = fs::weakly_canonical(fs::absolute(argv[0], ec), ec).parent_path();
        for (const fs::path& d : {exe, exe / ".." / "share" / "sieda" / "models", exe / ".." / "SiEDA" / "Resources" / "Models"})
            if (fs::is_directory(d, ec)) options.modelDirs.push_back(fs::weakly_canonical(d, ec).string());
        if (const char* env = std::getenv("SIEDA_MODELS_DIRS")) {
#ifdef _WIN32
            const char sep = ';';
#else
            const char sep = ':';
#endif
            std::stringstream ss(env);
            for (std::string d; std::getline(ss, d, sep);)
                if (!d.empty()) options.modelDirs.push_back(d);
        }
    }
    options.activityLog = sieda::mcp::defaultActivityLogDir();
    for (int i = 1; i < argc; ++i) {
        const std::string a = argv[i];
        auto value = [&]() -> std::string {
            if (i + 1 >= argc) {
                std::fprintf(stderr, "sieda-studio: %s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--root") options.allowedRoot = fs::absolute(value(), ec).string();
        else if (a == "--project") project = value();
        else if (a == "--port") port = std::atoi(value().c_str());
        else if (a == "--no-browser") browser = false;
        else if (a == "--library-dir") options.libraryDirs.push_back(fs::absolute(value(), ec).string());
        else if (a == "--lessons-dir") options.lessonDirs.push_back(fs::absolute(value(), ec).string());
        else if (a == "--models-dir") options.modelDirs.push_back(fs::absolute(value(), ec).string());
        else if (a == "--activity-log") options.activityLog = fs::absolute(value(), ec).string();
        else if (a == "--no-activity-log") options.activityLog.clear();
        else if (a == "--version") return std::puts("sieda-studio " SIEDA_STUDIO_VERSION), 0;
        else {
            std::fprintf(stderr, "usage: sieda-studio [--root <dir>] [--project <file>] [--port <n>] [--no-browser] [--library-dir <dir>] [--activity-log <dir> | --no-activity-log] [--lessons-dir <dir>] [--models-dir <dir>]\n");
            return 2;
        }
    }
    options.allowedRoot = fs::weakly_canonical(options.allowedRoot, ec).string();
    sieda::mcp::McpServer server(options);
    std::atomic<long> revision{0};
    server.setChangeHandler([&](const std::string&) { ++revision; });
    if (!project.empty()) {
        try {
            server.openProject(project);
        } catch (const std::exception& e) {
            std::fprintf(stderr, "sieda-studio: %s\n", e.what());
        }
    }
#ifdef _WIN32
    WSADATA wsa;
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) return 1;
#endif
    const Socket listener = socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // never another interface
    addr.sin_port = htons(static_cast<uint16_t>(port));
    socklen_t len = sizeof addr;
    if (listener == INVALID_SOCKET || bind(listener, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
        listen(listener, 16) != 0 || getsockname(listener, reinterpret_cast<sockaddr*>(&addr), &len) != 0) {
        std::fprintf(stderr, "sieda-studio: cannot listen on 127.0.0.1:%d\n", port);
        return 1;
    }
    port = ntohs(addr.sin_port);
    const std::string token = randomToken(), hostA = "127.0.0.1:" + std::to_string(port),
                      hostB = "localhost:" + std::to_string(port);
    const std::string url = "http://" + hostA + "/?t=" + token;
    std::printf("SiEDA Studio: %s\nMCP: http://%s/mcp (bearer token %s)\n", url.c_str(), hostA.c_str(), token.c_str());
    std::fflush(stdout);
    if (browser) openBrowser(url);
    std::string page = kStudioHtml;
    for (size_t at; (at = page.find("__SIEDA_TOKEN__")) != std::string::npos;) page.replace(at, 15, token);

    for (bool running = true; running;) {
        const Socket client = accept(listener, nullptr, nullptr);
        if (client == INVALID_SOCKET) continue;
        Request r;
        int error = 400;
        if (!readRequest(client, r, error)) {
            reply(client, error, "text/plain", "Bad request\n");
            closeSocket(client);
            continue;
        }
        const std::string host = r.header("host");
        std::string bearer = r.header("authorization");
        bearer = bearer.rfind("Bearer ", 0) == 0 ? bearer.substr(7) : std::string();
        const std::string given = r.path == "/" ? queryValue(r.query, "t") : !bearer.empty() ? bearer : r.header("x-sieda-token");
        if ((host != hostA && host != hostB) || !sameToken(given, token)) {
            reply(client, 403, "text/plain", "Open SiEDA Studio from the address it printed when it started.\n");
        } else if (r.method == "GET" && r.path == "/") {
            reply(client, 200, "text/html; charset=utf-8", page);
        } else if (r.method == "GET" && r.path == "/state") {
            Json j = Json::object();
            j["revision"] = static_cast<double>(revision.load());
            j["root"] = options.allowedRoot;
            j["project"] = server.projectPath();
            j["files"] = projectFiles(options.allowedRoot);
            reply(client, 200, "application/json", j.dump());
        } else if (r.method == "GET" && r.path == "/i18n") {
            static const Json all = Json::parse(kStudioI18n);
            const Json& words = all.get(queryValue(r.query, "lang"));
            reply(client, 200, "application/json; charset=utf-8", words.isObject() ? words.dump() : "{}");
        } else if (r.method == "POST" && (r.path == "/rpc" || r.path == "/mcp")) {
            server.setActor(r.path == "/rpc" ? "user" : "");  // the page is the user; /mcp is an AI client
            const std::string out = server.handle(r.body);
            reply(client, 200, "application/json", out.empty() ? "{}" : out);
        } else if (r.method == "GET" && r.path == "/plugins") {
            Json j = Json::object();
            j["folder"] = pluginFolder().string();
            j["plugins"] = pluginList();
            reply(client, 200, "application/json", j.dump());
        } else if (r.method == "POST" && r.path == "/plugins/run") {
            // The page asks; the design goes to the plug-in as saved JSON and its changes come back as one undo step.
            Json ask;
            try {
                ask = Json::parse(r.body);
            } catch (const std::exception&) {
            }
            server.setActor("user");
            const Json exported = server.callTool("project_export_json", Json::object());
            const std::string design = exported.get("content").items()[0].get("text").asString();
            Json result = runPlugin(ask.get("name").asString(), design, ask.get("sandbox").asBool(true));
            if (result.has("json")) {
                Json args = Json::object();
                args["json"] = result.get("json");
                const Json applied = server.callTool("project_load_json", args);
                if (applied.get("isError").asBool(false))
                    result["output"] = result.get("output").asString() + "\n" +
                                       applied.get("content").items()[0].get("text").asString();
                else
                    result["applied"] = true;
                result["json"] = Json();
            }
            reply(client, 200, "application/json", result.dump());
        } else if (r.method == "POST" && r.path == "/quit") {
            reply(client, 200, "application/json", "{}");
            running = false;
        } else {
            reply(client, 404, "text/plain", "Not found\n");
        }
        closeSocket(client);
    }
    closeSocket(listener);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
