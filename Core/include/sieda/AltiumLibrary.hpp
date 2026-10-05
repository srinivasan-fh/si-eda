// SiEDA Core — Altium Designer libraries: the OLE compound file (MS-CFB) container and the schematic (.SchLib) and
// PCB (.PcbLib) records inside it.
//
// CompoundFile reads the container: header, DIFAT, FAT, mini FAT, mini stream and the directory tree, with every
// sector chain, size and index checked (a damaged or hostile file throws CfbError, never reads out of bounds or
// loops). The Altium readers turn a SchLib's components (pins with designator, name, electrical type, position,
// orientation, part number; designator prefix, parameters and footprint model links) and a PcbLib's footprints
// (pads with position, size, shape, drill, layer and rotation; overlay outline) into plain structs that
// LibraryImport converts to symbols, land patterns and parts.
//
// The record layouts follow the published reverse-engineered format (the same one KiCad's Altium importer reads):
// schematic records are "|KEY=VALUE" property lists or binary pin records; PCB primitives are a type byte followed by
// length-prefixed subrecords. Integrated libraries (.IntLib) keep zlib-compressed copies of the libraries and are not
// read (Altium's "Extract Sources" writes the .SchLib / .PcbLib).
#pragma once

#include <array>
#include <cstdint>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace sieda {

struct CfbError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

class CompoundFile {
public:
    enum class EntryType { Empty = 0, Storage = 1, Stream = 2, Root = 5 };
    struct Entry {
        std::string name;  // UTF-8
        EntryType type = EntryType::Empty;
        uint32_t start = 0;
        uint64_t size = 0;
        std::vector<int> children;  // directory indices, for storages and the root
    };

    /// Parses the container (throws CfbError). `bytes` must outlive the object.
    explicit CompoundFile(const std::string& bytes);

    const std::vector<Entry>& entries() const { return entries_; }
    static constexpr int root() { return 0; }
    /// Child of `storage` named `name` (case-insensitive), or -1.
    int child(int storage, const std::string& name) const;
    /// A stream's contents (throws CfbError for a broken chain); at most `limit` bytes are allowed.
    std::string read(int entry, size_t limit = 32u << 20) const;

    static bool looksLikeCompoundFile(const std::string& bytes);

private:
    std::vector<uint32_t> chain(uint32_t start, const std::vector<uint32_t>& table, size_t maxLength) const;
    const std::string& b_;
    uint32_t sectorSize_ = 512, miniSectorSize_ = 64, miniCutoff_ = 4096;
    size_t sectorCount_ = 0;
    std::vector<uint32_t> fat_, miniFat_;
    std::string miniStream_;
    std::vector<Entry> entries_;
    std::vector<std::array<uint32_t, 3>> links_;  // left sibling, right sibling, child of each entry
};

/// One schematic pin (coordinates in mm, y up; the connection point is at the pin's outer end).
struct AltiumPin {
    std::string designator, name;
    int electrical = 4;  // 0 input, 1 I/O, 2 output, 3 open collector, 4 passive, 5 hi-Z, 6 open emitter, 7 power
    double x = 0, y = 0;
    char side = 'L';     // side of the body the pin leaves from
    int part = 1;        // owner part (multi-part components)
    bool hidden = false;
};

struct AltiumSymbol {
    std::string name, description, designatorPrefix = "U";
    std::string footprint;  // the current PCB footprint model (MODELNAME)
    std::vector<std::string> footprints;  // every PCB footprint model linked
    std::map<std::string, std::string> parameters;  // "Manufacturer", "Datasheet", …
    int partCount = 1;
    std::vector<AltiumPin> pins;
    std::vector<std::string> warnings;
};

struct AltiumPad {
    std::string name;
    double x = 0, y = 0;  // mm, y down (PCB view)
    double w = 0, h = 0;  // mm, before rotation
    double drill = 0;     // mm (0 = SMD)
    double rotation = 0;  // degrees
    int shape = 1;        // 1 round, 2 rectangle, 3 octagonal, 9 rounded rectangle
    int layer = 1;        // 1 top, 32 bottom, 74 multi-layer (through-hole)
    bool plated = true;
};

struct AltiumFootprint {
    std::string name, description;
    std::vector<AltiumPad> pads;
    /// Top overlay outline extent (mm, y down); empty when there is none.
    double outlineX0 = 0, outlineY0 = 0, outlineX1 = 0, outlineY1 = 0;
    bool hasOutline = false;
    int skippedPrimitives = 0;  // arcs, texts, fills, regions, bodies: not needed for the land pattern
    std::vector<std::string> warnings;
};

/// "schlib", "pcblib", "intlib" or "" from the container's FileHeader stream (and the file name as a fallback).
std::string altiumLibraryKind(const CompoundFile& cfb, const std::string& fileName);
std::vector<AltiumSymbol> readAltiumSchLib(const CompoundFile& cfb);
std::vector<AltiumFootprint> readAltiumPcbLib(const CompoundFile& cfb);

/// "|KEY=VALUE|KEY=VALUE" → map with upper-case keys; %UTF8%KEY values win over KEY (Windows-1252 otherwise).
std::map<std::string, std::string> altiumProperties(const std::string& record);

}  // namespace sieda
