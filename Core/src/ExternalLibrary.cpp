// SiEDA Core — the user's own KiCad libraries as a part source (ExternalLibrary.hpp).
#include "sieda/ExternalLibrary.hpp"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <stdexcept>

namespace fs = std::filesystem;

namespace sieda {

namespace {
constexpr uintmax_t kMaxFile = 64u * 1024 * 1024;
constexpr size_t kMaxFiles = 20000;

std::string lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string readFile(const fs::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::stringstream ss;
    ss << in.rdbuf();
    return ss.str();
}
}  // namespace

void ExternalLibraries::setFolders(std::vector<std::string> folders) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (folders == folders_) return;
    folders_ = std::move(folders);
    indexed_ = false;
}

std::vector<std::string> ExternalLibraries::folders() const { return folders_; }

void ExternalLibraries::index() {
    if (indexed_) return;
    indexed_ = true;
    parts_.clear(), footprintDirs_.clear(), warnings_.clear(), footprints_ = 0;
    std::vector<std::pair<std::string, fs::path>> symbolFiles;  // library, file
    size_t seen = 0;
    for (const std::string& root : folders_) {
        std::error_code ec;
        fs::recursive_directory_iterator it(root, fs::directory_options::skip_permission_denied, ec), end;
        if (ec) {
            warnings_.push_back("Cannot read " + root);
            continue;
        }
        for (; !ec && it != end && ++seen < kMaxFiles; it.increment(ec)) {
            const fs::path& p = it->path();
            const std::string ext = p.extension().string();
            if (it->is_directory(ec)) {
                if (ext == ".pretty") {
                    footprintDirs_.emplace(p.stem().string(), p.string());
                    for (const auto& f : fs::directory_iterator(p, ec))
                        footprints_ += f.path().extension() == ".kicad_mod";
                    it.disable_recursion_pending();
                } else if (it.depth() >= 3) {
                    it.disable_recursion_pending();
                }
                continue;
            }
            if (ext != ".kicad_sym") continue;
            const fs::path parent = p.parent_path();
            // A .kicad_symdir keeps one symbol per file: the library is the folder.
            symbolFiles.emplace_back(parent.extension() == ".kicad_symdir" ? parent.stem().string() : p.stem().string(), p);
        }
    }
    for (const auto& [library, file] : symbolFiles) {
        std::error_code ec;
        if (fs::file_size(file, ec) > kMaxFile || ec) {
            warnings_.push_back("Skipped " + file.string() + " (larger than 64 MB or unreadable)");
            continue;
        }
        std::vector<ImportedSymbol> symbols;
        try {
            symbols = parseKicadSymbols(readFile(file), file.filename().string());
        } catch (const std::exception& e) {
            warnings_.push_back(file.string() + ": " + e.what());
            continue;
        }
        std::map<std::string, int> pinCount;
        for (const auto& s : symbols) pinCount[s.name] = static_cast<int>(s.pins.size());
        for (const auto& s : symbols) {
            ExternalPart p;
            p.library = library, p.name = s.name, p.id = library + ":" + s.name;
            p.description = s.description, p.footprint = s.footprint, p.datasheet = s.datasheet;
            p.file = file.string(), p.extends = s.extends;
            p.pins = s.pins.empty() && pinCount.count(s.extends) ? pinCount[s.extends] : static_cast<int>(s.pins.size());
            for (const auto& f : s.footprintFilters) p.keywords += f + " ";
            parts_.push_back(std::move(p));
        }
    }
    std::sort(parts_.begin(), parts_.end(), [](const ExternalPart& a, const ExternalPart& b) { return a.id < b.id; });
}

const std::vector<ExternalPart>& ExternalLibraries::parts() {
    std::lock_guard<std::mutex> lock(mutex_);
    index();
    return parts_;
}

size_t ExternalLibraries::footprintCount() {
    std::lock_guard<std::mutex> lock(mutex_);
    index();
    return footprints_;
}

std::vector<std::string> ExternalLibraries::warnings() {
    std::lock_guard<std::mutex> lock(mutex_);
    index();
    return warnings_;
}

std::vector<const ExternalPart*> ExternalLibraries::search(const std::string& query, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex_);
    index();
    std::vector<std::string> words;
    std::istringstream in(lower(query));
    for (std::string w; in >> w;) words.push_back(w);
    std::vector<std::pair<int, const ExternalPart*>> hits;
    for (const ExternalPart& p : parts_) {
        const std::string name = lower(p.name), hay = name + " " + lower(p.library + " " + p.description + " " + p.keywords + " " + p.footprint);
        if (!std::all_of(words.begin(), words.end(), [&](const std::string& w) { return hay.find(w) != std::string::npos; })) continue;
        const int rank = words.empty() ? 2 : name == words[0] ? 0 : name.rfind(words[0], 0) == 0 ? 1 : 2;
        hits.emplace_back(rank, &p);
    }
    std::stable_sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    std::vector<const ExternalPart*> out;
    for (size_t i = 0; i < hits.size() && out.size() < limit; ++i) out.push_back(hits[i].second);
    return out;
}

ImportedPart ExternalLibraries::load(const std::string& id) {
    std::unique_lock<std::mutex> lock(mutex_);
    index();
    const auto it = std::lower_bound(parts_.begin(), parts_.end(), id,
                                     [](const ExternalPart& p, const std::string& key) { return p.id < key; });
    if (it == parts_.end() || it->id != id) throw std::runtime_error("No library part " + id);
    const ExternalPart part = *it;
    std::string footprintName;
    std::vector<ImportFile> files{{fs::path(part.file).filename().string(), readFile(part.file)}};
    if (!part.extends.empty()) {  // a .kicad_symdir keeps the base symbol in a file of its own
        const fs::path base = fs::path(part.file).parent_path() / (part.extends + ".kicad_sym");
        std::error_code ec;
        if (base != fs::path(part.file) && fs::exists(base, ec)) files.push_back({base.filename().string(), readFile(base)});
    }
    if (const size_t colon = part.footprint.find(':'); colon != std::string::npos) {
        const auto dir = footprintDirs_.find(part.footprint.substr(0, colon));
        footprintName = part.footprint.substr(colon + 1);
        std::error_code ec;
        const fs::path fp = dir == footprintDirs_.end() ? fs::path() : fs::path(dir->second) / (footprintName + ".kicad_mod");
        if (!fp.empty() && fs::exists(fp, ec) && fs::file_size(fp, ec) <= kMaxFile) files.push_back({fp.filename().string(), readFile(fp)});
        else footprintName.clear();
    }
    lock.unlock();
    std::map<std::string, std::string> pairs;
    if (!footprintName.empty()) pairs[part.name] = footprintName;
    LibraryImport result = importLibraryFiles(files, pairs);
    for (ImportedPart& p : result.parts)
        if (p.symbolName == part.name) {
            p.source = part.id;
            return p;
        }
    throw std::runtime_error("Library part " + id + " could not be read");
}

std::vector<std::string> ExternalLibraries::foldersFromEnvironment() {
    std::vector<std::string> out;
    const char* env = std::getenv("SIEDA_LIBRARY_DIRS");
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

ExternalLibraries& ExternalLibraries::shared(const std::vector<std::string>& folders) {
    static std::mutex m;
    static std::map<std::vector<std::string>, std::unique_ptr<ExternalLibraries>> all;
    std::lock_guard<std::mutex> lock(m);
    auto& slot = all[folders];
    if (!slot) {
        slot = std::make_unique<ExternalLibraries>();
        slot->setFolders(folders);
    }
    return *slot;
}

}  // namespace sieda
