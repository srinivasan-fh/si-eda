// SiEDA Core — C ABI: interactive routing additions (arcs, length tuning, multi-segment drags, teardrops, via
// stitching and shielding, glossing). Every entry point is exception-safe. See docs/INTERACTIVE_ROUTING.md.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "SiedaProjectInternal.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/Json.hpp"
#include "sieda/LengthRules.hpp"
#include "sieda/sieda_c.h"

using namespace sieda;

namespace {
char* dupString(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
char* errorString(const std::exception& e) {
    Json j = Json::object();
    j["ok"] = false;
    j["error"] = e.what();
    return dupString(j.dump());
}
Json parseOr(const char* text, Json fallback) { return text && *text ? Json::parse(text) : fallback; }
std::vector<int> idList(const Json& j) {
    std::vector<int> ids;
    if (j.isArray())
        for (const auto& v : j.items())
            if (v.isNumber()) ids.push_back(v.asInt());
    return ids;
}
}  // namespace

extern "C" {

char* sieda_pcb_arc_corners(SiedaProject* project, const char* track_ids_json, const char* options_json) {
    if (!project) return nullptr;
    try {
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        const Json o = parseOr(options_json, Json::object());
        ArcCornersOptions opt;
        opt.radius = o.get("radius").asNumber(0);
        opt.apply = o.get("apply").asBool(true);
        if (opt.apply && project->router) project->router->cancel();
        return dupString(arcCornersJson(convertCornersToArcs(project->project.pcb, project->project.schematic, ids, opt)).dump());
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

int32_t sieda_pcb_set_length_rule(SiedaProject* project, const char* net_name, double target_mm, double tolerance_mm) {
    if (!project || !net_name || !*net_name) return 0;
    auto& rules = project->project.pcb.settings.lengthRules;
    rules.erase(std::remove_if(rules.begin(), rules.end(), [&](const LengthRule& r) { return r.net == net_name; }), rules.end());
    if (!(target_mm > 0) || !std::isfinite(target_mm)) return 1;  // removed
    LengthRule r;
    r.net = net_name;
    r.target = target_mm;
    r.tolerance = std::isfinite(tolerance_mm) ? std::clamp(tolerance_mm, 0.0, 100.0) : 0.1;
    rules.push_back(r);
    return 1;
}

int32_t sieda_pcb_set_match_group(SiedaProject* project, const char* group_json) {
    if (!project) return 0;
    try {
        const Json j = parseOr(group_json, Json::object());
        const std::string name = j.get("name").asString("");
        if (name.empty()) return 0;
        auto& groups = project->project.pcb.settings.matchGroups;
        groups.erase(std::remove_if(groups.begin(), groups.end(), [&](const MatchGroup& g) { return g.name == name; }),
                     groups.end());
        MatchGroup g;
        g.name = name;
        g.tolerance = std::clamp(j.get("tolerance").asNumber(0.1), 0.0, 100.0);
        for (const auto& n : j.get("nets").items())
            if (n.isString() && !n.asString().empty()) g.nets.push_back(n.asString());
        if (g.nets.size() < 2) return 1;  // removed
        groups.push_back(g);
        return 1;
    } catch (...) {
        return 0;
    }
}

char* sieda_length_targets_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dupString(lengthTargetsJson(project->project.pcb, project->project.schematic).dump());
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

}  // extern "C"
