// SiEDA Core — C ABI: autorouter strategy options, presets, the routing report and routing keep-outs. Every entry
// point is exception-safe. See docs/ROUTING.md (Strategies).
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>

#include "SiedaProjectInternal.hpp"
#include "sieda/Autoroute.hpp"
#include "sieda/Json.hpp"
#include "sieda/sieda_c.h"
#include "sieda/Mechanical.hpp"
#include "sieda/Panel.hpp"
#include "sieda/Dfm.hpp"
#include "sieda/CustomRules.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/FieldSolver.hpp"
#include "sieda/CustomParts.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Thermal.hpp"

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

char* sieda_thermal_json(const SiedaProject* project, const char* options_json, int32_t with_map) {
    if (!project) return nullptr;
    try {
        const ThermalResult r = thermalAnalysis(project->project, thermalOptionsFromJson(Json::parse(options_json ? options_json : "{}")));
        return dupText(thermalJson(r, with_map != 0).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_fab_quote(const SiedaProject* project, const char* request_json) {
    if (!project) return nullptr;
    try {
        return dupText(fabQuote(project->project, Json::parse(request_json ? request_json : "{}")).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_pcb_custom_rules(SiedaProject* project, const char* rules_json) {
    if (!project) return nullptr;
    try {
        PcbLayout& pcb = project->project.pcb;
        if (rules_json) {
            const Json j = Json::parse(rules_json);
            if (!j.isArray()) return nullptr;
            pcb.settings.customRules = customRulesFromJson(j);
        }
        Json out = Json::object(), errors = Json::array();
        out["rules"] = customRulesToJson(pcb.settings.customRules);
        for (const CustomRule& r : pcb.settings.customRules)
            if (const std::string e = customRuleError(r); !e.empty()) {
                Json x = Json::object();
                x["name"] = r.name, x["error"] = e;
                errors.push(x);
            }
        out["errors"] = errors;
        out["violations"] = Project::violationsToJson(customRuleChecks(project->project.schematic, pcb));
        return dupText(out.dump());
    } catch (...) {
        return nullptr;
    }
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

char* sieda_field_solve(const SiedaProject* project, int32_t layer, double width, double gap, double roughness_um,
                        double cpw) {
    if (!project) return nullptr;
    const auto& s = project->project.pcb.settings;
    if (layer < 0 || layer >= std::max(1, s.layerCount) || !(width > 0 && width < 20) || !(gap >= 0 && gap < 20) ||
        !(roughness_um >= 0 && roughness_um < 20) || !(cpw >= 0 && cpw < 20))
        return nullptr;
    FieldGeometry g = trackGeometry(s, layer, width, gap);
    g.roughness = roughness_um, g.cpw = cpw;
    return dupText(fieldResultJson(g, solveField(g)).dump());
}

double sieda_field_solve_width(const SiedaProject* project, int32_t layer, double ohms, double gap, double cpw) {
    if (!project) return 0;
    const auto& s = project->project.pcb.settings;
    if (layer < 0 || layer >= std::max(1, s.layerCount) || !(ohms > 5 && ohms < 500) || !(gap >= 0 && gap < 20) ||
        !(cpw >= 0 && cpw < 20))
        return 0;
    return fieldSolvedWidth(s, layer, ohms, gap, cpw);
}

char* sieda_pcb_panel(const SiedaProject* project) {
    if (!project) return nullptr;
    return dupText(panelLayoutJson(project->project.pcb.settings).dump());
}

char* sieda_pcb_thieving(SiedaProject* project, const char* settings_json) {
    if (!project) return nullptr;
    auto& pr = project->project;
    CopperThieving& th = pr.pcb.settings.thieving;
    try {
        if (settings_json) {
            const Json j = Json::parse(settings_json);
            const CopperThieving t{j.get("pitch").asNumber(th.pitch), j.get("size").asNumber(th.size), j.get("clearance").asNumber(th.clearance)};
            if (!(t.pitch >= 0 && t.pitch <= 50 && t.size >= 0.1 && t.size <= 10 && t.clearance >= 0 && t.clearance <= 10) ||
                (t.pitch > 0 && t.pitch < t.size + 0.1))
                return nullptr;
            th = t;
        }
        Json out = Json::object(), layers = Json::array();
        out["pitch"] = th.pitch, out["size"] = th.size, out["clearance"] = th.clearance;
        const auto dots = copperThieving(pr.schematic, pr.pcb);
        const auto cover = copperCoverage(pr.schematic, pr.pcb);
        for (size_t l = 0; l < cover.size(); ++l) {
            Json e = Json::object();
            e["layer"] = static_cast<int>(l), e["dots"] = 0, e["coverage"] = cover[l];
            for (const auto& f : dots)
                if (f.layer == static_cast<int>(l)) e["dots"] = static_cast<int>(f.rects.size());
            layers.push(e);
        }
        out["layers"] = layers;
        return dupText(out.dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_pcb_graphics(SiedaProject* project, const char* graphics_json) {
    if (!project) return nullptr;
    auto& gs = project->project.pcb.settings.graphics;
    try {
        if (graphics_json) {
            const Json j = Json::parse(graphics_json);
            if (!j.isArray()) return nullptr;
            auto next = boardGraphicsFromJson(j);
            if (next.size() != j.size()) return nullptr;  // an item without artwork or on an unknown layer
            gs = std::move(next);
        }
        return dupText(boardGraphicsToJson(gs).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_parse_pin_table(const char* text) {
    Json out = Json::array();
    for (const auto& p : parsePinTable(text ? text : "")) {
        Json j = Json::object();
        j["number"] = p.number, j["name"] = p.name, j["type"] = pinTypeName(p.type), j["description"] = p.description;
        out.push(j);
    }
    return dupText(out.dump());
}

char* sieda_project_load_notes(const SiedaProject* project) {
    Json out = Json::array();
    if (project)
        for (const auto& n : project->project.loadNotes) out.push(n);
    return dupText(out.dump());
}

char* sieda_library_updates(SiedaProject* project, const char* apply_json) {
    if (!project) return nullptr;
    try {
        std::set<std::string> apply;
        if (apply_json) {
            const Json names = Json::parse(apply_json);
            for (const Json& n : names.items()) apply.insert(n.asString(""));
        }
        auto& pr = project->project;
        auto& reg = CustomPartRegistry::instance();
        Json updates = Json::array();
        int updated = 0;
        const std::vector<std::string> ids = pr.customLibrary;  // updated in place below
        for (const std::string& id : ids) {
            const auto old = reg.find(id);
            const StandardPart* sp = old ? findStandardPart(old->spec.name) : nullptr;
            if (!sp || sp->spec.name != old->spec.name) continue;
            const auto now = reg.registerPart(sp->spec);
            if (now->id == id) continue;
            const CustomPartSpec &a = old->spec, &b = now->spec;
            Json changes = Json::array();
            if (a.pins.size() != b.pins.size())
                changes.push("pins " + std::to_string(a.pins.size()) + " → " + std::to_string(b.pins.size()));
            int renamed = 0;
            for (size_t i = 0; i < std::min(a.pins.size(), b.pins.size()); ++i)
                renamed += a.pins[i].name != b.pins[i].name || a.pins[i].number != b.pins[i].number || a.pins[i].type != b.pins[i].type;
            if (renamed) changes.push(std::to_string(renamed) + " pin(s) renamed, renumbered or retyped");
            if (a.package.type != b.package.type || a.package.pinCount != b.package.pinCount)
                changes.push("package " + a.package.type + "-" + std::to_string(a.package.pinCount) + " → " + b.package.type + "-" +
                             std::to_string(b.package.pinCount));
            if (old->footprint.pads.size() != now->footprint.pads.size() || std::fabs(a.package.pitch - b.package.pitch) > 1e-9 ||
                std::fabs(a.package.bodySize - b.package.bodySize) > 1e-9)
                changes.push("footprint " + std::to_string(old->footprint.pads.size()) + " → " +
                             std::to_string(now->footprint.pads.size()) + " pads");
            if (!changes.size()) changes.push("description, values or model");
            int instances = 0;
            for (const auto& c : pr.schematic.components()) instances += c.customPart == id;
            if (apply.count("*") || apply.count(a.name)) {
                pr.schematic.replaceCustomPart(id, now->id);
                std::replace(pr.customLibrary.begin(), pr.customLibrary.end(), id, now->id);
                pr.schematicChanged();
                ++updated;
                continue;
            }
            Json u = Json::object();
            u["name"] = a.name, u["oldId"] = id, u["newId"] = now->id, u["instances"] = instances, u["changes"] = changes;
            updates.push(u);
        }
        Json out = Json::object();
        out["updates"] = updates, out["updated"] = updated;
        return dupText(out.dump());
    } catch (...) {
        return nullptr;
    }
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
