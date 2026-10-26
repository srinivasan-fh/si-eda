// SiEDA Core — a GitHub repository as the project's cloud: sync (connect / pull / push) and share links, through the
// user's own `git` (its credentials: Git Credential Manager, `gh auth`, SSH keys; SiEDA never stores a token).
// Public repositories are readable by anyone; private ones by the repository's collaborators.
#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "sieda/Json.hpp"

namespace sieda::cloud {

struct GitRun {
    int code = -1;       // git's exit status; -1 when git could not be started
    std::string output;  // stdout and stderr together
    bool ok() const { return code == 0; }
};
/// Runs `git -C dir args…` without a shell (never prompts: GIT_TERMINAL_PROMPT=0, SSH in batch mode).
GitRun git(const std::string& dir, const std::vector<std::string>& args);

/// {owner, repo} of "https://github.com/o/r(.git)", "git@github.com:o/r(.git)" or "o/r"; nullopt otherwise.
std::optional<std::pair<std::string, std::string>> githubRepo(const std::string& url);
/// A branch name git accepts and that cannot be read as an option ([A-Za-z0-9._/-], no "..", no leading '-' / '/').
bool validBranch(const std::string& branch);
/// {"file","history","raw","viewer"} links for a file in the repository (viewer: GitHub Pages, when it is on).
Json shareLinks(const std::string& owner, const std::string& repo, const std::string& branch, const std::string& path,
                const std::string& viewerPath);
/// True when the repository can be read without credentials (a public repository).
bool publiclyReadable(const std::string& owner, const std::string& repo);

/// Makes `dir` (a folder of its own, not inside another repository) a clone of `remote`. When the remote already has
/// the branch and nothing is committed here, its design is taken and local copies of its files are kept beside them
/// ("backups": <name>.local.siedaproj); otherwise `files` are committed and the remote's work merged (pull).
Json connect(const std::string& dir, const std::string& remote, const std::string& branch,
             const std::vector<std::string>& files);
/// Commits the existing `files` when they changed. True when a commit was made.
bool commit(const std::string& dir, const std::vector<std::string>& files, const std::string& message);
/// Fetches and merges origin/<branch>; conflicting project files are merged field by field (mergeProjects, ours
/// kept where both changed: listed in "conflicts"); any other conflict aborts the merge. Throws std::runtime_error.
Json pull(const std::string& dir, const std::string& branch);
/// Commits `files` (paths relative to dir) when they changed and pushes; a rejected push pulls once and retries.
Json push(const std::string& dir, const std::string& branch, const std::vector<std::string>& files,
          const std::string& message);
/// {"repository","remote","branch","ahead","behind","changed":[…]} ("repository": false outside a clone).
Json status(const std::string& dir);

}  // namespace sieda::cloud
