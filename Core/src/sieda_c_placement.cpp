// SiEDA Core — C API of interactive placement (Update PCB's "place new parts"; Core/src/InteractivePlacement.cpp).
#include <cstdlib>
#include <cstring>
#include <string>

#include "SiedaProjectInternal.hpp"
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

char* placementJson(const PlacementCheck& c) {
    if (c.component < 0) return nullptr;
    Json j = Json::object();
    j["component"] = c.component;
    j["x"] = c.position.x;
    j["y"] = c.position.y;
    j["rotation"] = c.rotation;
    j["bottom"] = c.bottom;
    j["legal"] = c.legal;
    j["committed"] = c.committed;
    Json box = Json::object();
    box["x0"] = c.courtyard.x0;
    box["y0"] = c.courtyard.y0;
    box["x1"] = c.courtyard.x1;
    box["y1"] = c.courtyard.y1;
    j["courtyard"] = box;
    Json pads = Json::array();
    for (const Pad& p : c.pads) {
        Json pj = Json::object();
        pj["x"] = p.position.x;
        pj["y"] = p.position.y;
        pj["w"] = p.size.x;
        pj["h"] = p.size.y;
        pj["round"] = p.round;
        pj["net"] = p.net;
        pads.push(pj);
    }
    j["pads"] = pads;
    Json issues = Json::array();
    for (const auto& i : c.issues) {
        Json ij = Json::object();
        ij["code"] = i.code;
        ij["message"] = i.message;
        ij["other"] = i.other;
        ij["error"] = i.error;
        issues.push(ij);
    }
    j["issues"] = issues;
    return dupString(j.dump());
}
}  // namespace

extern "C" {

char* sieda_pcb_check_placement(const SiedaProject* project, int32_t component_id, double x, double y,
                                int32_t rotation, int32_t bottom, double grid) {
    if (!project) return nullptr;
    try {
        return placementJson(project->project.checkPlacement(component_id, {x, y}, rotation, bottom != 0, grid));
    } catch (...) {
        return nullptr;
    }
}

char* sieda_pcb_place_footprint(SiedaProject* project, int32_t component_id, double x, double y, int32_t rotation,
                                int32_t bottom, double grid, int32_t force) {
    if (!project) return nullptr;
    try {
        return placementJson(
            project->project.placeComponent(component_id, {x, y}, rotation, bottom != 0, grid, force != 0));
    } catch (...) {
        return nullptr;
    }
}

char* sieda_pcb_suggest_placement(const SiedaProject* project, int32_t component_id, double grid) {
    if (!project) return nullptr;
    try {
        return placementJson(project->project.suggestPlacement(component_id, grid));
    } catch (...) {
        return nullptr;
    }
}

}  // extern "C"
