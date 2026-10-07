// SiEDA Core — what changed between two versions of a project (parts, connectivity, placement, copper, board,
// variants), for reviews and Git (`sieda-mcp --diff old.siedaproj new.siedaproj`), and the variants side by side.
#pragma once

#include <string>
#include <vector>

#include "sieda/Json.hpp"
#include "sieda/Project.hpp"

namespace sieda {

/// {"identical", "summary", "components":{"added","removed","changed"}, "nets":{"added","removed","changed"},
///  "copper":[{"net","tracks","vias","length"}], "board":{field:[old,new]}, "variants":{"added","removed","changed"}}.
/// Parts are matched by designator, nets by their pins (a renamed net with the same pins is a rename only).
Json diffProjects(const Project& before, const Project& after);
/// The diff as review text: one line per change ("+ R5 10k R_0603", "~ U1 moved …", "~ net SDA +U2.5 −U3.7").
std::string diffText(const Json& diff);

/// Three-way merge of two edited copies of a project against their common ancestor (Git's merge driver:
/// `sieda-mcp --merge base ours theirs`). Fields merge one by one; lists of objects with an "id" (parts, wires,
/// sheets, custom parts) merge item by item; lists of other objects (tracks, vias, pours) merge as sets, so both
/// sides' additions and removals apply; other lists merge whole. A field both sides changed differently is a conflict:
/// "ours" is kept and the path is reported. `merged` is a loadable project unless `error` is set.
struct ProjectMerge {
    Json merged;
    std::vector<std::string> conflicts;  // e.g. "components[id=12].value: ours \"10k\", theirs \"4k7\""
    std::string error;                   // a side is not a project, or the result does not load
};
ProjectMerge mergeProjects(const Json& base, const Json& ours, const Json& theirs);

/// Review comments as JSON: [{"id","author","text","ref","view","x","y","resolved","replies":[{"author","text"}]}].
Json reviewToJson(const std::vector<ReviewComment>& comments);
std::vector<ReviewComment> reviewFromJson(const Json& j);
/// Review commands: {"action":"add","author","text","ref"?,"view"?,"x"?,"y"?} → the new id; "reply" (id, author,
/// text), "resolve" / "reopen" / "delete" (id). Throws JsonError for an unknown id or action, or an empty text.
int reviewCommand(Project& project, const Json& request);
/// The review as Markdown: open comments first, each with its part, place and replies.
std::string reviewMarkdown(const Project& project);

/// Every part a variant changes, with its fitting and value in each variant, and per-variant totals:
/// {"variants":[{"name","fitted","notFitted","valueChanges"}], "parts":[{"ref","value","cells":[{"fitted","value"}]}]}.
Json variantMatrix(const Project& project);

}  // namespace sieda
