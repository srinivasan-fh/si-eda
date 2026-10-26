// SiEDA Core — GitHub repository sync and share links (CloudSync.hpp).
#include "sieda/CloudSync.hpp"

#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>

#include "sieda/Project.hpp"
#include "sieda/ProjectDiff.hpp"

#ifdef _WIN32
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace fs = std::filesystem;

namespace sieda::cloud {

namespace {
void quietGit() {
    // Never wait for a password or a host-key answer nobody can type: fail and report instead.
#ifdef _WIN32
    _putenv_s("GIT_TERMINAL_PROMPT", "0");
    if (!std::getenv("GIT_SSH_COMMAND")) _putenv_s("GIT_SSH_COMMAND", "ssh -o BatchMode=yes");
#else
    setenv("GIT_TERMINAL_PROMPT", "0", 1);
    setenv("GIT_SSH_COMMAND", "ssh -o BatchMode=yes", 0);
#endif
}

#ifdef _WIN32
/// One argument as the C runtime parses it back (backslashes doubled before a quote).
std::string quoteArg(const std::string& a) {
    if (!a.empty() && a.find_first_of(" \t\"") == std::string::npos) return a;
    std::string q = "\"";
    size_t slashes = 0;
    for (char c : a) {
        if (c == '\\') { ++slashes; continue; }
        q.append(c == '"' ? slashes * 2 + 1 : slashes, '\\');
        slashes = 0;
        q += c;
    }
    return q.append(slashes * 2, '\\') + "\"";
}
#endif

std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
std::string trim(std::string s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}
GitRun must(const std::string& dir, const std::vector<std::string>& args) {
    GitRun r = git(dir, args);
    if (r.code == -1) throw std::runtime_error("git is not installed (install Git, then try again)");
    if (!r.ok()) throw std::runtime_error("git " + args.front() + " failed: " + trim(r.output));
    return r;
}
/// `-c` identity for commits when the user has none configured (git refuses to commit otherwise).
std::vector<std::string> withIdentity(const std::string& dir, std::vector<std::string> args) {
    if (trim(git(dir, {"config", "user.email"}).output).empty())
        args.insert(args.begin(), {"-c", "user.name=SiEDA", "-c", "user.email=sieda@localhost"});
    return args;
}
bool hasRef(const std::string& dir, const std::string& ref) { return git(dir, {"rev-parse", "-q", "--verify", ref}).ok(); }
}  // namespace

GitRun git(const std::string& dir, const std::vector<std::string>& args) {
    static const bool quiet = (quietGit(), true);
    (void)quiet;
    // A folder's own hooks / fsmonitor would run its code: SiEDA's git runs neither.
    static const std::string noHooks = "core.hooksPath=" + (fs::temp_directory_path() / "sieda-no-git-hooks").string();
    std::vector<std::string> all{"git", "-c", "core.fsmonitor=false", "-c", noHooks, "-C", dir};
    all.insert(all.end(), args.begin(), args.end());
    GitRun r;
#ifdef _WIN32
    std::string line;
    for (const auto& a : all) line += (line.empty() ? "" : " ") + quoteArg(a);
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE rd = nullptr, wr = nullptr;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return r;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi{};
    const BOOL started = CreateProcessA(nullptr, line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi);
    CloseHandle(wr);
    if (started) {
        char buf[4096];
        DWORD n = 0;
        while (ReadFile(rd, buf, sizeof buf, &n, nullptr) && n) r.output.append(buf, n);
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 1;
        GetExitCodeProcess(pi.hProcess, &code);
        r.code = static_cast<int>(code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
    CloseHandle(rd);
#else
    int fds[2];
    if (pipe(fds) != 0) return r;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    posix_spawn_file_actions_addclose(&fa, fds[1]);
    std::vector<char*> argv;
    for (auto& a : all) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int err = posix_spawnp(&pid, "git", &fa, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (err == 0) {
        char buf[4096];
        for (ssize_t n; (n = read(fds[0], buf, sizeof buf)) > 0;) r.output.append(buf, static_cast<size_t>(n));
        int status = 0;
        while (waitpid(pid, &status, 0) < 0 && errno == EINTR) {}
        r.code = WIFEXITED(status) ? WEXITSTATUS(status) : 128;
        if (r.code == 127) r.code = -1;  // posix_spawnp's child could not run git
    }
    close(fds[0]);
#endif
    return r;
}

std::optional<std::pair<std::string, std::string>> githubRepo(const std::string& url) {
    std::string rest;
    for (const char* prefix : {"https://github.com/", "http://github.com/", "git@github.com:", "ssh://git@github.com/"})
        if (url.rfind(prefix, 0) == 0) rest = url.substr(std::string(prefix).size());
    if (rest.empty() && url.find(':') == std::string::npos) rest = url;  // "owner/repo"
    while (!rest.empty() && rest.back() == '/') rest.pop_back();
    if (rest.size() > 4 && rest.compare(rest.size() - 4, 4, ".git") == 0) rest.resize(rest.size() - 4);
    const size_t slash = rest.find('/');
    if (slash == std::string::npos || rest.find('/', slash + 1) != std::string::npos) return std::nullopt;
    std::string owner = rest.substr(0, slash), repo = rest.substr(slash + 1);
    auto ok = [](const std::string& s) {
        if (s.empty() || s.size() > 100 || s[0] == '-' || s[0] == '.') return false;
        for (char c : s)
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '-' && c != '_' && c != '.') return false;
        return true;
    };
    if (!ok(owner) || !ok(repo)) return std::nullopt;
    return std::make_pair(owner, repo);
}

bool validBranch(const std::string& b) {
    if (b.empty() || b.size() > 200 || b[0] == '-' || b[0] == '/' || b.back() == '/' || b.back() == '.' ||
        b.find("..") != std::string::npos || b.find("//") != std::string::npos)
        return false;
    for (char c : b)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '.' && c != '_' && c != '/' && c != '-') return false;
    return true;
}

Json shareLinks(const std::string& owner, const std::string& repo, const std::string& branch, const std::string& path,
                const std::string& viewerPath) {
    const std::string base = "https://github.com/" + owner + "/" + repo;
    std::string pagesOwner = owner;
    for (char& c : pagesOwner) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    Json::Object o;
    o["repository"] = base;
    o["file"] = base + "/blob/" + branch + "/" + path;
    o["history"] = base + "/commits/" + branch + "/" + path;
    o["raw"] = "https://raw.githubusercontent.com/" + owner + "/" + repo + "/" + branch + "/" + path;
    if (!viewerPath.empty()) o["viewer"] = "https://" + pagesOwner + ".github.io/" + repo + "/" + viewerPath;
    return Json(std::move(o));
}

bool publiclyReadable(const std::string& owner, const std::string& repo) {
    // No credential helper: only a public repository answers.
    return git(".", {"-c", "credential.helper=", "-c", "core.askPass=", "ls-remote", "-q", "--heads",
                     "https://github.com/" + owner + "/" + repo + ".git"})
        .ok();
}

bool commit(const std::string& dir, const std::vector<std::string>& files, const std::string& message) {
    std::vector<std::string> add{"add", "--"};
    for (const auto& f : files)
        if (fs::exists(fs::path(dir) / f)) add.push_back(f);
    if (add.size() > 2) must(dir, add);
    if (git(dir, {"diff", "--cached", "--quiet"}).ok()) return false;
    must(dir, withIdentity(dir, {"commit", "-q", "-m", message}));
    return true;
}

Json connect(const std::string& dir, const std::string& remote, const std::string& branch,
             const std::vector<std::string>& files) {
    // --show-cdup ("../" up to the top, "" at it) rather than --show-toplevel: MSYS2's git prints POSIX paths.
    const GitRun top = git(dir, {"rev-parse", "--show-cdup"});
    if (top.code == -1) throw std::runtime_error("git is not installed (install Git, then try again)");
    if (top.ok() && !trim(top.output).empty())
        throw std::runtime_error("this folder is inside another Git repository: keep the project in a folder of its own");
    if (!top.ok()) {
        must(dir, {"init", "-q"});
        must(dir, {"symbolic-ref", "HEAD", "refs/heads/" + branch});
    }
    if (git(dir, {"remote", "get-url", "origin"}).ok()) must(dir, {"remote", "set-url", "origin", remote});
    else must(dir, {"remote", "add", "origin", remote});
    must(dir, {"fetch", "-q", "origin"});
    Json::Object o;
    o["remote"] = remote;
    o["branch"] = branch;
    const std::string theirs = "refs/remotes/origin/" + branch;
    if (hasRef(dir, theirs) && !hasRef(dir, "HEAD")) {
        // A new clone takes the repository's design; a local file it also has is kept beside it as <name>.local.<ext>.
        Json::Array backups;
        for (const auto& f : files) {
            const fs::path local = fs::path(dir) / f;
            if (!fs::exists(local) || !git(dir, {"cat-file", "-e", theirs + ":" + f}).ok()) continue;
            const fs::path backup = local.parent_path() / (local.stem().string() + ".local" + local.extension().string());
            fs::rename(local, backup);
            backups.push_back(fs::relative(backup, dir).generic_string());
        }
        must(dir, {"checkout", "-q", "-B", branch, theirs});
        o["tookRemote"] = true;
        o["backups"] = Json(std::move(backups));
    } else {
        o["committed"] = !files.empty() && commit(dir, files, "Add " + files.front() + " from SiEDA");
        if (hasRef(dir, theirs)) o["pulled"] = pull(dir, branch);
    }
    return Json(std::move(o));
}

Json pull(const std::string& dir, const std::string& branch) {
    must(dir, {"fetch", "-q", "origin"});
    Json::Object o;
    const std::string theirs = "refs/remotes/origin/" + branch;
    if (!hasRef(dir, theirs)) {
        o["pulled"] = false;
        o["reason"] = "the remote has no branch " + branch + " yet (push creates it)";
        return Json(std::move(o));
    }
    if (!hasRef(dir, "HEAD")) {  // nothing committed here yet: take the remote's history as it is
        must(dir, {"checkout", "-q", "-B", branch, theirs});
        o["pulled"] = true;
        return Json(std::move(o));
    }
    Json::Array merged, conflicts;
    // A project file both sides changed is never merged as text (that can break its JSON): a merge driver that always
    // fails leaves it to mergeProjects below.
    const fs::path attributes = fs::temp_directory_path() / "sieda-git-attributes";
    if (!fs::exists(attributes)) std::ofstream(attributes) << "*.siedaproj merge=siedaproj\n";
    if (!git(dir, withIdentity(dir, {"-c", "core.attributesFile=" + attributes.generic_string(), "-c",
                                     "merge.siedaproj.driver=false", "merge", "-q", "--no-edit", "--allow-unrelated-histories", theirs})).ok()) {
        const std::string unmerged = git(dir, {"diff", "--name-only", "--diff-filter=U"}).output;
        std::vector<std::string> files;
        std::istringstream lines(unmerged);
        for (std::string f; std::getline(lines, f);)
            if (!trim(f).empty()) files.push_back(trim(f));
        bool onlyProjects = !files.empty();
        for (const auto& f : files) onlyProjects = onlyProjects && fs::path(f).extension() == ".siedaproj";
        if (!onlyProjects) {
            git(dir, {"merge", "--abort"});
            throw std::runtime_error("the remote changed the same files as you (not project files); nothing was merged");
        }
        const std::string top = (fs::path(dir) / trim(must(dir, {"rev-parse", "--show-cdup"}).output)).string();
        for (const auto& f : files) {
            // Three stages as temporary files: "base ours theirs<TAB>path" ("." for a missing stage).
            const std::string line = trim(must(dir, {"-C", top, "checkout-index", "--stage=all", "--temp", "--", f}).output);
            std::istringstream parts(line.substr(0, line.find('\t')));
            std::string stage[3];
            parts >> stage[0] >> stage[1] >> stage[2];
            auto load = [&](const std::string& tmp) {
                const std::string text = tmp == "." || tmp.empty() ? "" : readFile(fs::path(top) / tmp);
                if (tmp != "." && !tmp.empty()) fs::remove(fs::path(top) / tmp);
                return text.find_first_not_of(" \t\r\n") == std::string::npos ? Project().toJson() : Json::parse(text);
            };
            const Json base = load(stage[0]), ours = load(stage[1]), theirsJson = load(stage[2]);
            const ProjectMerge m = mergeProjects(base, ours, theirsJson);
            if (!m.error.empty()) {
                git(dir, {"merge", "--abort"});
                throw std::runtime_error(f + " could not be merged: " + m.error);
            }
            std::ofstream(fs::path(top) / f, std::ios::binary | std::ios::trunc) << Project::fromJson(m.merged).toJson().dump(true);
            must(dir, {"-C", top, "add", "--", f});
            merged.push_back(f);
            for (const auto& c : m.conflicts) conflicts.push_back(f + ": " + c);
        }
        must(dir, withIdentity(dir, {"commit", "-q", "--no-edit"}));
    }
    o["pulled"] = true;
    o["merged"] = Json(std::move(merged));
    o["conflicts"] = Json(std::move(conflicts));
    return Json(std::move(o));
}

Json push(const std::string& dir, const std::string& branch, const std::vector<std::string>& files,
          const std::string& message) {
    Json::Object o;
    o["committed"] = commit(dir, files, message);
    const std::vector<std::string> pushArgs{"push", "-q", "-u", "origin", "HEAD:refs/heads/" + branch};
    GitRun r = git(dir, pushArgs);
    if (!r.ok() && (r.output.find("rejected") != std::string::npos || r.output.find("fetch first") != std::string::npos)) {
        o["pulledFirst"] = pull(dir, branch);  // someone pushed meanwhile: merge their work, then push again
        r = git(dir, pushArgs);
    }
    if (r.code == -1) throw std::runtime_error("git is not installed (install Git, then try again)");
    if (!r.ok()) throw std::runtime_error("git push failed: " + trim(r.output));
    o["pushed"] = true;
    o["commit"] = trim(git(dir, {"rev-parse", "--short", "HEAD"}).output);
    return Json(std::move(o));
}

Json status(const std::string& dir) {
    Json::Object o;
    const GitRun inside = git(dir, {"rev-parse", "--is-inside-work-tree"});
    if (inside.code == -1) throw std::runtime_error("git is not installed (install Git, then try again)");
    o["repository"] = inside.ok();
    if (!inside.ok()) return Json(std::move(o));
    o["remote"] = trim(git(dir, {"remote", "get-url", "origin"}).output);
    o["branch"] = trim(git(dir, {"rev-parse", "--abbrev-ref", "HEAD"}).output);
    Json::Array changed;
    std::istringstream lines(git(dir, {"status", "--porcelain=v1", "-b"}).output);
    for (std::string l; std::getline(lines, l);) {
        if (l.rfind("## ", 0) == 0) {
            auto num = [&](const char* key) {
                const size_t at = l.find(key);
                return at == std::string::npos ? 0 : std::atoi(l.c_str() + at + std::string(key).size());
            };
            o["ahead"] = num("ahead ");
            o["behind"] = num("behind ");
        } else if (l.size() > 3) {
            changed.push_back(l.substr(3));
        }
    }
    o["changed"] = Json(std::move(changed));
    return Json(std::move(o));
}

}  // namespace sieda::cloud
