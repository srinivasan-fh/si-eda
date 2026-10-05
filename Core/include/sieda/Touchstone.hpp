// SiEDA Core — Touchstone (.sNp) S-parameter files: version 1.x (option line "# <unit> <S|Y|Z> <MA|DB|RI> R <z0>")
// and the version 2.0 keywords ([Version], [Number of Ports], [Two-Port Data Order], [Number of Frequencies],
// [Reference], [Network Data], [End]). Y- and Z-parameter files are converted to S. The parser is hardened for
// untrusted input: bounded port count (≤ 64) and size, every number checked finite, frequencies strictly increasing,
// noise-parameter blocks skipped, and every malformed file rejected with a message (no exceptions escape other than
// std::runtime_error).
#pragma once

#include <string>
#include <vector>

#include "sieda/Channel.hpp"

namespace sieda {

struct TouchstoneData {
    SParams sp;            // always S, referenced to sp.z0
    std::string version;   // "1.1" or "2.0"
    std::string format;    // "MA", "DB", "RI" of the file
    std::string parameter; // "S", "Y", "Z" of the file
    std::vector<std::string> comments;
};

/// Parses Touchstone text. `portsHint` is the port count from the file extension (.s2p → 2); 0 infers it from a
/// version 2 header or from the first data line. Throws std::runtime_error with the reason.
TouchstoneData parseTouchstone(const std::string& text, int portsHint);
/// Port count from a file name ("x.s4p" → 4), 0 when not a Touchstone name.
int touchstonePortsFromName(const std::string& fileName);
/// Version 1.1 text: "# Hz S RI R <z0>", one frequency per block (2-port in the 1.x order S11 S21 S12 S22).
std::string writeTouchstone(const SParams& sp, const std::string& comment);
/// Reorders a 4-port whose thru paths are 1→2 / 3→4 ("12") into [1, 3, 2, 4] so it is [near…, far…]; "13" unchanged.
SParams reorderFourPort(const SParams& sp, const std::string& order);
/// Preview: {"ports","z0","points","fMin","fMax","format","parameter","version","freq":[…],"curves":[{name,db}],
/// "comments":[…]} with S21 / S11 (2-port) or SDD21 / SDD11 / SCD21 (4-port in `order`).
Json touchstoneJson(const TouchstoneData& t, const std::string& order);
/// An imported channel (2-port single-ended, or 4-port differential in `order`) driven by an ideal source and
/// termination of `drive`: {"curves","freq","step":{time,lossy},"eye":{…}} (eye when `eye` is set).
Json touchstoneChannelJson(const TouchstoneData& t, const std::string& order, const ChannelDrive& drive, const EyeOptions* eye);

}  // namespace sieda
