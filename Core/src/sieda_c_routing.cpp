// SiEDA Core — C ABI: interactive routing additions (arcs, length tuning, multi-segment drags, teardrops, via
// stitching and shielding, glossing). Every entry point is exception-safe. See docs/INTERACTIVE_ROUTING.md.
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "SiedaProjectInternal.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/Json.hpp"
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

}  // extern "C"
