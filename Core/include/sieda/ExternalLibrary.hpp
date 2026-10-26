// SiEDA Core — the user's own KiCad libraries (e.g. a KiCad install's symbols and footprints) as a searchable,
// read-only part source. Folders are indexed once; a part is imported (symbol + the footprint it names) only when it
// is placed, so projects stay small. Only whoever starts the server names the folders (sieda-mcp / sieda-studio
// --library-dir, SIEDA_LIBRARY_DIRS); tools can search and place from them but never add one.
#pragma once

#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "sieda/LibraryImport.hpp"

namespace sieda {

struct ExternalPart {
    std::string id;           // "Library:Symbol"
    std::string library, name, description, keywords, footprint, datasheet;
    std::string file;         // the .kicad_sym file holding it
    std::string extends;      // a derived symbol's base
    int pins = 0;
};

class ExternalLibraries {
public:
    /// Symbol folders hold .kicad_sym files and .kicad_symdir folders, footprint folders .pretty folders; one folder
    /// may hold both (searched 4 levels deep). Changing the folders drops the index.
    void setFolders(std::vector<std::string> folders);
    std::vector<std::string> folders() const;
    /// Every symbol, indexed on first use (files larger than 64 MB or unreadable are skipped and listed in warnings).
    const std::vector<ExternalPart>& parts();
    size_t footprintCount();
    std::vector<std::string> warnings();
    /// Parts whose name, library, description or keywords contain every query word (case-insensitive), names that
    /// start with the query first.
    std::vector<const ExternalPart*> search(const std::string& query, size_t limit);
    /// The part for "Library:Symbol" built from its symbol and the footprint it names (when that footprint is in a
    /// folder; else SiEDA's package of that name). Throws std::runtime_error for an unknown id.
    ImportedPart load(const std::string& id);

    /// SIEDA_LIBRARY_DIRS split on the path separator (":" on Linux / macOS, ";" on Windows); empty when unset.
    static std::vector<std::string> foldersFromEnvironment();
    /// The process-wide index for a list of folders (one per distinct list).
    static ExternalLibraries& shared(const std::vector<std::string>& folders);

private:
    void index();
    std::mutex mutex_;
    std::vector<std::string> folders_;
    bool indexed_ = false;
    std::vector<ExternalPart> parts_;
    std::map<std::string, std::string> footprintDirs_;  // library name → its .pretty folder
    size_t footprints_ = 0;
    std::vector<std::string> warnings_;
};

}  // namespace sieda
