// SiEDA Core — length targets: per-net length rules (net classes), match groups and xSignals (the pad-to-pad path of
// a signal through series parts such as termination resistors and AC-coupling capacitors). Used by the length tuning
// tool (targets and the live gauge), the router's length banner and the DRC (DRC_LENGTH). See
// docs/INTERACTIVE_ROUTING.md (Length tuning).
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"
#include "sieda/Schematic.hpp"

namespace sieda {

/// A signal from pad to pad, possibly through two-pin series parts: its nets (the first is the one asked for), the
/// series parts between them, and its end pads (indices into PcbLayout::pads()).
struct XSignal {
    std::vector<int> nets;
    std::vector<int> seriesParts;  // component ids
    std::vector<size_t> endPads;
};

/// The xSignal through `net`: nets joined by two-pin series parts (resistors, capacitors, inductors, fuses, ferrite
/// beads) whose both pins are on signal nets; at most 8 nets.
XSignal xSignalOf(const Schematic& sch, const std::vector<Pad>& pads, int net);

/// Routed length of an xSignal, pad to pad (mm): the longest of the shortest copper paths between its end pads, through
/// its series parts (a part itself adds nothing). -1 when the end pads are not joined by copper yet; a net with fewer
/// than two end pads measures its routed copper (sum of its track lengths).
double xSignalLength(const PcbLayout& pcb, const std::vector<Pad>& pads, const XSignal& x);

/// The length target that applies to `net`: its length rule (net class), else the longest xSignal of its match group.
/// `source` is "rule:<net>" or "group:<name>". False when none applies.
struct LengthTarget {
    double target = 0, tolerance = 0;
    std::string source;
    XSignal xsignal;  // the net's xSignal (what the target measures)
};
bool lengthTargetFor(const PcbLayout& pcb, const Schematic& sch, const std::vector<Pad>& pads, int net, LengthTarget& out);

/// Every length rule and match group with its members' current lengths: {"rules":[{"net","target","tolerance",
/// "length","ok","routed"}],"groups":[{"name","tolerance","target","members":[{"net","xsignal":[net names],"length",
/// "ok","routed"}]}]}.
Json lengthTargetsJson(const PcbLayout& pcb, const Schematic& sch);

/// DRC warnings (DRC_LENGTH) for routed nets outside their rule's or group's tolerance.
std::vector<RuleViolation> lengthRuleViolations(const PcbLayout& pcb, const Schematic& sch);

}  // namespace sieda
