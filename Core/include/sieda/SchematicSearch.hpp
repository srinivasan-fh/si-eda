// SiEDA Core — schematic productivity: find / replace across sheets and the net navigator.
#pragma once

#include <string>
#include <vector>

#include "sieda/Schematic.hpp"

namespace sieda {

struct SearchOptions {
    bool matchCase = false;
    bool wholeWord = false;  // the text must match a whole field, not part of it
    /// Fields searched: designators, part values, net label names, net names, pin names.
    bool refs = true, values = true, labels = true, nets = true, pins = false;
};

struct SearchHit {
    int component = -1;  // -1 for a net hit
    int net = -1;        // net of a label / net / pin hit
    int pin = -1;        // pin hits
    int sheet = 0;
    std::string field;   // "ref", "value", "label", "net", "pin"
    std::string text;    // the matching text
};

/// Every place `text` appears, sheet by sheet (sheet order, then top to bottom). Copies on repeated sheets and the
/// units of multi-unit parts are found where they are drawn; hidden packages are not.
std::vector<SearchHit> findInSchematic(const Schematic& sch, const std::string& text, const SearchOptions& options);

/// Replaces `text` with `replacement` in part values and / or net label names (options.values / options.labels;
/// designators are re-numbered by annotation instead). A block part of a repeated sheet and a multi-unit part are
/// edited once. Returns the number of fields changed. Labels never get an empty name.
int replaceInSchematic(Schematic& sch, const std::string& text, const std::string& replacement, const SearchOptions& options);

/// One place a net appears: a part pin, a label, a port, a sheet entry or a bus entry.
struct NetPlace {
    int component = -1;
    int pin = -1;
    int sheet = 0;
    Vec2 position;
    std::string kind;  // "pin", "label", "global", "port", "entry", "bus", "ground"
    std::string ref;   // "R1" / "U1A"
    std::string name;  // pin name, or the label text
};

/// The net navigator: every place `net` appears, in sheet order (then top to bottom, left to right). Junctions and
/// hidden packages are left out (a multi-unit part appears through its units).
std::vector<NetPlace> netPlaces(const Schematic& sch, int net);

}  // namespace sieda
