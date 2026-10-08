// SiEDA Core — C ABI: autorouter strategy options, presets, the routing report and routing keep-outs. Every entry
// point is exception-safe. See docs/ROUTING.md (Strategies).
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>

#include "SiedaProjectInternal.hpp"
#include "sieda/Autoroute.hpp"
#include "sieda/Json.hpp"
#include "sieda/sieda_c.h"
#include "sieda/Mechanical.hpp"
#include "sieda/Panel.hpp"
#include "sieda/Dfm.hpp"
#include "sieda/FieldSolver.hpp"

using namespace sieda;

namespace {
char* dupText(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}
}  // namespace

char* sieda_pcb_autoroute_options(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dupText(autorouteOptionsToJson(project->project.pcb.settings.autorouter).dump());
    } catch (const std::exception&) {
        return nullptr;
    }
}

int32_t sieda_pcb_set_autoroute_options(SiedaProject* project, const char* options_json) {
    if (!project || !options_json) return 0;
    try {
        const Json j = Json::parse(options_json);
        if (!j.isObject()) return 0;
        AutorouteOptions& o = project->project.pcb.settings.autorouter;
        o = autorouteOptionsFromJson(j, o);
        return 1;
    } catch (const std::exception&) {
        return 0;
    }
}

char* sieda_autoroute_presets(void) {
    try {
        return dupText(autoroutePresetsJson().dump());
    } catch (const std::exception&) {
        return nullptr;
    }
}

char* sieda_pcb_route_report(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dupText(routeReportJson(project->project.pcb.lastRouteReport).dump());
    } catch (const std::exception&) {
        return nullptr;
    }
}

char* sieda_pcb_keepouts(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dupText(keepoutsToJson(project->project.pcb.settings.keepouts).dump());
    } catch (const std::exception&) {
        return nullptr;
    }
}

char* sieda_pcb_mechanical_limits(const SiedaProject* project) {
    if (!project) return nullptr;
    const Json j = mechanicalLimitsToJson(project->project.pcb.settings);
    return dupText(j.isNull() ? std::string("{}") : j.dump());
}

int32_t sieda_pcb_set_mechanical_limits(SiedaProject* project, const char* limits_json) {
    if (!project || !limits_json) return 0;
    try {
        mechanicalLimitsFromJson(Json::parse(limits_json), project->project.pcb.settings);
        return 1;
    } catch (const std::exception&) {
        return 0;
    }
}

char* sieda_dfm_packs_json(void) { return dupText(dfmPacksJson().dump()); }

char* sieda_dfm_report_json(const SiedaProject* project) {
    if (!project) return nullptr;
    return dupText(dfmReportJson(project->project.schematic, project->project.pcb).dump());
}

int32_t sieda_pcb_set_dfm_pack(SiedaProject* project, const char* pack_id) {
    return project && pack_id && applyDfmPack(project->project.pcb.settings, pack_id) ? 1 : 0;
}

int32_t sieda_pcb_set_dfm_override(SiedaProject* project, const char* field, double value) {
    const auto& names = dfmFieldNames();
    if (!project || !field || std::find(names.begin(), names.end(), field) == names.end() || !(value < 1e4)) return 0;
    auto& s = project->project.pcb.settings;
    if (value > 0) s.dfmOverrides[field] = value;
    else s.dfmOverrides.erase(field);
    if (!s.dfmPack.empty()) applyDfmPack(s, s.dfmPack);
    return 1;
}

char* sieda_field_solve(const SiedaProject* project, int32_t layer, double width, double gap, double roughness_um) {
    if (!project) return nullptr;
    const auto& s = project->project.pcb.settings;
    if (layer < 0 || layer >= std::max(1, s.layerCount) || !(width > 0 && width < 20) || !(gap >= 0 && gap < 20) ||
        !(roughness_um >= 0 && roughness_um < 20))
        return nullptr;
    FieldGeometry g = trackGeometry(s, layer, width, gap);
    g.roughness = roughness_um;
    return dupText(fieldResultJson(g, solveField(g)).dump());
}

double sieda_field_solve_width(const SiedaProject* project, int32_t layer, double ohms, double gap) {
    if (!project) return 0;
    const auto& s = project->project.pcb.settings;
    if (layer < 0 || layer >= std::max(1, s.layerCount) || !(ohms > 5 && ohms < 500) || !(gap >= 0 && gap < 20)) return 0;
    return fieldSolvedWidth(s, layer, ohms, gap);
}

char* sieda_pcb_panel(const SiedaProject* project) {
    if (!project) return nullptr;
    return dupText(panelLayoutJson(project->project.pcb.settings).dump());
}

int32_t sieda_pcb_set_panel(SiedaProject* project, const char* settings_json) {
    if (!project || !settings_json) return 0;
    try {
        const Json j = Json::parse(settings_json);
        auto& s = project->project.pcb.settings;
        panelFromJson(j, s.panel);
        if (j.get("fit").isObject()) {  // the most boards within a maximum panel (default: the DFM pack's size limit)
            const auto pack = boardDfmPack(s);
            const double w = j.get("fit").get("width").asNumber(pack ? pack->maxWidth : 250);
            const double h = j.get("fit").get("height").asNumber(pack ? pack->maxHeight : 250);
            if (!(w > 0 && h > 0 && w < 1e4 && h < 1e4)) return 0;
            s.panel = fitPanel(s, w, h);
        }
        return 1;
    } catch (const std::exception&) {
        return 0;
    }
}

int32_t sieda_pcb_set_keepouts(SiedaProject* project, const char* keepouts_json) {
    if (!project || !keepouts_json) return 0;
    try {
        project->project.pcb.settings.keepouts = keepoutsFromJson(Json::parse(keepouts_json));
        return static_cast<int32_t>(project->project.pcb.settings.keepouts.size());
    } catch (const std::exception&) {
        return 0;
    }
}
