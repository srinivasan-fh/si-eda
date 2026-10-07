// SiEDA Core — C ABI: interactive routing additions (arcs, length tuning, multi-segment drags, teardrops, via
// stitching and shielding, glossing). Every entry point is exception-safe. See docs/INTERACTIVE_ROUTING.md.
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <memory>
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
InteractiveRouter& routerFor(SiedaProject* project) {
    if (!project->router) {
        project->router = std::make_unique<InteractiveRouter>(project->project.pcb, project->project.schematic);
        project->router->setAbortSource(&project->routerAbort);
    }
    return *project->router;
}
void setRouterOptions(SiedaProject* project, const char* options_json) {
    if (!options_json || !*options_json) return;
    InteractiveRouter& r = routerFor(project);
    r.setOptions(routerOptionsFromJson(Json::parse(options_json), r.options()));
}
char* previewOf(SiedaProject* project, bool ok) {
    InteractiveRouter& r = routerFor(project);
    Json j = routePreviewJson(r.preview());
    if (!ok) j["error"] = r.error().empty() ? std::string("Not possible here") : r.error();
    return dupString(j.dump());
}
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

char* sieda_router_begin_corner_drag(SiedaProject* project, const char* options_json, int32_t track_id, double x,
                                     double y) {
    if (!project) return nullptr;
    try {
        setRouterOptions(project, options_json);
        return previewOf(project, routerFor(project).beginCornerDrag(track_id, {x, y}));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

char* sieda_router_begin_multi_drag(SiedaProject* project, const char* options_json, const char* track_ids_json, double x,
                                    double y) {
    if (!project) return nullptr;
    try {
        setRouterOptions(project, options_json);
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        return previewOf(project, routerFor(project).beginMultiDrag(ids, {x, y}));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

char* sieda_router_begin_multi(SiedaProject* project, const char* options_json, const char* points_json, int32_t layer) {
    if (!project) return nullptr;
    try {
        setRouterOptions(project, options_json);
        std::vector<Vec2> starts;
        const Json pts = parseOr(points_json, Json::array());
        if (pts.isArray())
            for (const auto& p : pts.items())
                if (p.get("x").isNumber() && p.get("y").isNumber())
                    starts.push_back({p.get("x").asNumber(), p.get("y").asNumber()});
        return previewOf(project, routerFor(project).beginMultiRoute(starts, layer));
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

namespace {
char* boardEditString(SiedaProject* project, const BoardEditResult& r) {
    return dupString(boardEditJson(r, project->project.pcb.settings.layerCount).dump());
}
}  // namespace

extern "C" {

char* sieda_pcb_teardrops(SiedaProject* project, const char* track_ids_json, const char* options_json) {
    if (!project) return nullptr;
    try {
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        const Json o = parseOr(options_json, Json::object());
        if (project->router) project->router->cancel();
        if (o.get("remove").asBool(false)) return boardEditString(project, removeTeardrops(project->project.pcb, ids));
        TeardropOptions opt;
        opt.trackIds = ids;
        opt.pads = o.get("pads").asBool(true);
        opt.vias = o.get("vias").asBool(true);
        opt.length = o.get("length").asNumber(1.0);
        opt.style = teardropStyleFromName(o.get("style").asString("straight"));
        opt.apply = o.get("apply").asBool(true);
        return boardEditString(project, addTeardrops(project->project.pcb, project->project.schematic, opt));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

}  // extern "C"

namespace {
ViaPatternOptions viaPatternFrom(const Json& o) {
    ViaPatternOptions opt;
    opt.net = o.get("net").asString(std::string());
    opt.pitch = std::max(0.0, o.get("pitch").asNumber(0));
    opt.offset = std::max(0.0, o.get("offset").asNumber(0));
    if (o.has("x0") && o.has("y0") && o.has("x1") && o.has("y1")) {
        opt.area = Rect(o.get("x0").asNumber(), o.get("y0").asNumber(), o.get("x1").asNumber(), o.get("y1").asNumber());
        opt.hasArea = opt.area.width() > 0 && opt.area.height() > 0;
    }
    opt.apply = o.get("apply").asBool(true);
    return opt;
}
}  // namespace

extern "C" {

char* sieda_pcb_stitch_vias(SiedaProject* project, const char* options_json) {
    if (!project) return nullptr;
    try {
        const ViaPatternOptions opt = viaPatternFrom(parseOr(options_json, Json::object()));
        if (project->router) project->router->cancel();
        return boardEditString(project, stitchVias(project->project.pcb, project->project.schematic, opt));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

char* sieda_pcb_shield_tracks(SiedaProject* project, const char* track_ids_json, const char* options_json) {
    if (!project) return nullptr;
    try {
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        const ViaPatternOptions opt = viaPatternFrom(parseOr(options_json, Json::object()));
        if (project->router) project->router->cancel();
        return boardEditString(project, shieldTracks(project->project.pcb, project->project.schematic, ids, opt));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

char* sieda_pcb_gloss(SiedaProject* project, const char* track_ids_json, const char* options_json) {
    if (!project) return nullptr;
    try {
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        const Json o = parseOr(options_json, Json::object());
        GlossOptions opt;
        opt.retrace = o.get("retrace").asBool(true);
        opt.apply = o.get("apply").asBool(true);
        if (project->router) project->router->cancel();
        return boardEditString(project, glossTracks(project->project.pcb, project->project.schematic, ids, opt));
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

}  // extern "C"

extern "C" {

char* sieda_pcb_match_lengths(SiedaProject* project, const char* track_ids_json, const char* options_json) {
    if (!project) return nullptr;
    try {
        const std::vector<int> ids = idList(parseOr(track_ids_json, Json::array()));
        const Json o = parseOr(options_json, Json::object());
        const LengthTuneOptions opt = lengthTuneOptionsFromJson(o);
        const double tolerance = o.get("tolerance").asNumber(0.1);
        if (project->router) project->router->cancel();
        return dupString(matchLengthsJson(matchTrackLengths(project->project.pcb, project->project.schematic, ids, opt,
                                                            std::isfinite(tolerance) ? tolerance : 0.1))
                             .dump());
    } catch (const std::exception& e) {
        return errorString(e);
    }
}

}  // extern "C"
