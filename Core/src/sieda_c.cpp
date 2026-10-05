// SiEDA Core — C ABI implementation. Every entry point is exception-safe.
#include "sieda/sieda_c.h"

#include "sieda/Aerospace.hpp"
#include "sieda/Analysis.hpp"
#include "sieda/Naval.hpp"
#include "sieda/Medical.hpp"
#include "sieda/Retail.hpp"
#include "sieda/Appliance.hpp"
#include "sieda/Memory.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Avr.hpp"
#include "sieda/Firmware.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <memory>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sieda/Bom.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Ibis.hpp"
#include "sieda/Industry.hpp"
#include "sieda/InteractiveRouter.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/LibraryImport.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/PowerIntegrity.hpp"
#include "sieda/Project.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/SchematicSearch.hpp"
#include "sieda/SignalIntegrity.hpp"
#include "sieda/Channel.hpp"
#include "sieda/Eye.hpp"
#include "sieda/LossyLine.hpp"
#include "sieda/Touchstone.hpp"
#include "sieda/PdnPlanning.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/Suppliers.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Units.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Verification.hpp"

struct SiedaProject {
    sieda::Project project;
    std::unique_ptr<sieda::InteractiveRouter> router;  // interactive route session (created on first use)
};

struct SiedaMesh {
    sieda::Mesh mesh;
};

struct SiedaLiveSim {
    explicit SiedaLiveSim(const sieda::Schematic& s) : schematic(s), sim(schematic) {}
    sieda::Schematic schematic;  // snapshot the simulator reads (declared first: constructed before `sim`)
    sieda::Simulator sim;
    std::map<int, bool> switches;  // component id → closed
    std::vector<double> traceTime;
    std::vector<std::vector<double>> traceNets;  // [net][sample]
};

using namespace sieda;

namespace {
char* dup(const std::string& s) {
    char* out = static_cast<char*>(std::malloc(s.size() + 1));
    if (!out) return nullptr;
    std::memcpy(out, s.c_str(), s.size() + 1);
    return out;
}

std::string str(const char* s) { return s ? std::string(s) : std::string(); }

char* errorJson(const std::exception& e) {
    Json j = Json::object();
    j["error"] = e.what();
    return dup(j.dump());
}

template <typename F>
int32_t guarded(F f) {
    try {
        return f();
    } catch (...) {
        return 0;
    }
}

/// Simulation results say which assembly was simulated: the active variant and the parts left out (not fitted).
void addAssemblyNote(const Project& p, Json& result) {
    if (!p.activeVariant.empty()) result["variant"] = p.activeVariant;
    const auto omitted = p.unfittedRefs();
    if (!omitted.empty()) {
        Json list = Json::array();
        for (const auto& ref : omitted) list.push(ref);
        result["omitted"] = list;
    }
}

/// Assembly outputs (they follow a design variant). False for any other format.
bool assemblyExport(const Project& p, const Schematic& sch, const std::string& f, std::string* out) {
    if (f == "bom") *out = exportBomCsv(sch);
    else if (f == "pnp") *out = exportPickAndPlaceCsv(sch);
    else if (f == "bom_assembly") *out = exportAssemblyBomCsv(sch);
    else if (f == "cpl") *out = exportCplCsv(sch, p.pcb);
    else if (f == "assembly_top") *out = exportAssemblySvg(sch, p.pcb, false, p.name);
    else if (f == "assembly_bottom") *out = exportAssemblySvg(sch, p.pcb, true, p.name);
    else if (f == "bom_json") *out = bomJson(sch, p.buildQuantity).dump();
    else return false;
    return true;
}
}  // namespace

extern "C" {

const char* sieda_version(void) { return "1.0.0"; }

SiedaProject* sieda_project_new(const char* name) {
    try {
        auto* p = new SiedaProject();
        if (name && *name) p->project.name = name;
        return p;
    } catch (...) {
        return nullptr;
    }
}

void sieda_project_free(SiedaProject* project) { delete project; }

SiedaProject* sieda_project_load_json(const char* json, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        auto* p = new SiedaProject();
        p->project = Project::fromJson(Json::parse(str(json)));
        return p;
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_project_save_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(project->project.toJson().dump(true));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_project_set_name(SiedaProject* project, const char* name) {
    if (project && name) project->project.name = name;
}

void sieda_project_set_requirements(SiedaProject* project, const char* text) {
    if (project) project->project.requirements = str(text);
}

int32_t sieda_set_pin_no_connect(SiedaProject* project, int32_t component_id, int32_t pin, int32_t no_connect) {
    if (!project) return 0;
    return project->project.schematic.setPinNoConnect(component_id, pin, no_connect != 0) ? 1 : 0;
}

int32_t sieda_project_set_industry(SiedaProject* project, const char* industry_id) {
    if (!project || !industry_id) return 0;
    return project->project.applyIndustry(industry_id) ? 1 : 0;
}

char* sieda_industry_profiles_json(void) {
    Json arr = Json::array();
    for (const auto& p : industryProfiles()) {
        Json j = Json::object();
        j["id"] = p.id;
        j["name"] = p.name;
        j["description"] = p.description;
        j["standards"] = p.standards;
        j["rulePreset"] = p.rulePreset;
        j["powerDerating"] = p.powerDerating;
        j["currentDerating"] = p.currentDerating;
        j["highAltitude"] = p.highAltitude;
        j["minAmbientC"] = p.minAmbientC;
        j["maxAmbientC"] = p.maxAmbientC;
        Json g = Json::array();
        for (const auto& line : p.guidance) g.push(line);
        j["guidance"] = g;
        arr.push(j);
    }
    return dup(arr.dump());
}

void sieda_project_clear(SiedaProject* project) {
    if (!project) return;
    project->project.schematic.clear();
    project->project.pcb.clearRouting();
}

void sieda_project_reset(SiedaProject* project) {
    if (!project) return;
    project->project = Project{};
}

char* sieda_project_snapshot(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(project->project.snapshot().dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_library_json(void) {
    try {
        return dup(Project::libraryJson().dump());
    } catch (...) {
        return dup("[]");
    }
}

void sieda_string_free(char* s) { std::free(s); }

int32_t sieda_add_component(SiedaProject* project, int32_t kind, const char* value, double x, double y,
                            int32_t rotation, const char* ref) {
    if (!project || !Library::isValidKind(kind)) return -1;
    try {
        int id = project->project.schematic.addComponent(static_cast<ComponentKind>(kind), str(value), {x, y}, rotation,
                                                         str(ref));
        project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_remove_component(SiedaProject* project, int32_t id) {
    if (!project) return 0;
    return guarded([&] {
        bool ok = project->project.schematic.removeComponent(id);
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_move_component(SiedaProject* project, int32_t id, double x, double y) {
    if (!project) return 0;
    return project->project.schematic.moveComponent(id, {x, y}) ? 1 : 0;
}

int32_t sieda_rotate_component(SiedaProject* project, int32_t id, int32_t delta) {
    if (!project) return 0;
    return project->project.schematic.rotateComponent(id, delta) ? 1 : 0;
}

int32_t sieda_set_component_value(SiedaProject* project, int32_t id, const char* value) {
    if (!project) return 0;
    return guarded([&] {
        bool ok = project->project.schematic.setValue(id, str(value));
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_set_component_package(SiedaProject* project, int32_t id, const char* package) {
    if (!project) return 0;
    return guarded([&] {
        bool ok = project->project.schematic.setPackage(id, str(package));
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_set_component_ref(SiedaProject* project, int32_t id, const char* ref) {
    if (!project) return 0;
    return project->project.schematic.setRef(id, str(ref)) ? 1 : 0;
}

int32_t sieda_find_component(const SiedaProject* project, const char* ref) {
    if (!project || !ref) return -1;
    const Component* c = project->project.schematic.findByRef(ref);
    return c ? c->id : -1;
}

int32_t sieda_find_pin(const SiedaProject* project, int32_t id, const char* name) {
    if (!project || !name) return -1;
    return project->project.schematic.pinIndex(id, name);
}

int32_t sieda_connect(SiedaProject* project, int32_t ca, int32_t pa, int32_t cb, int32_t pb) {
    if (!project) return -1;
    try {
        int id = project->project.schematic.connect({ca, pa}, {cb, pb});
        if (id >= 0) project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_split_wire(SiedaProject* project, int32_t wire_id, double x, double y) {
    if (!project) return -1;
    try {
        int id = project->project.schematic.splitWire(wire_id, {x, y});
        if (id >= 0) project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_remove_dangling_junctions(SiedaProject* project, int32_t junction_id) {
    if (!project) return 0;
    return guarded([&] {
        int n = project->project.schematic.removeDanglingJunctions(junction_id);
        if (n > 0) project->project.schematicChanged();
        return n;
    });
}

int32_t sieda_remove_wire(SiedaProject* project, int32_t id) {
    if (!project) return 0;
    return guarded([&] {
        bool ok = project->project.schematic.removeWire(id);
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

namespace {
bool isMomentary(const std::string& value) {
    std::string v;
    for (char c : value) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v.find("push") != std::string::npos || v.find("button") != std::string::npos ||
           v.find("momentary") != std::string::npos || v.find("tact") != std::string::npos;
}
bool isClosedValue(const std::string& value) {
    std::string v;
    for (char c : value) v += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return v == "on" || v == "closed" || v == "1" || v == "true";
}
}  // namespace

SiedaLiveSim* sieda_live_start(const SiedaProject* project, char** error_out) {
    if (error_out) *error_out = nullptr;
    if (!project) return nullptr;
    try {
        auto* live = new SiedaLiveSim(project->project.simulationSchematic());
        std::string error;
        if (!live->sim.begin(error)) {
            if (error_out) *error_out = dup(error);
            delete live;
            return nullptr;
        }
        for (const auto& c : live->schematic.components())
            if (c.kind == ComponentKind::Switch) live->switches[c.id] = isClosedValue(c.value);
        return live;
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

int32_t sieda_live_run(SiedaLiveSim* live, double duration, double step, int32_t trace_points, char** error_out) {
    if (error_out) *error_out = nullptr;
    if (!live) return 0;
    try {
        if (!(duration > 0) || !(step > 0)) return 1;
        int steps = std::max(1, static_cast<int>(std::ceil(duration / step - 1e-9)));
        steps = std::min(steps, 200000);
        double h = duration / steps;
        int keep = std::max(2, static_cast<int>(trace_points));
        int stride = std::max(1, steps / keep);
        const size_t netCount = live->schematic.nets().size();
        live->traceTime.clear();
        live->traceNets.assign(netCount, {});
        auto sample = [&]() {
            live->traceTime.push_back(live->sim.time());
            auto v = live->sim.netVoltages();
            for (size_t n = 0; n < netCount && n < v.size(); ++n) live->traceNets[n].push_back(v[n]);
        };
        for (int i = 0; i < steps; ++i) {
            std::string error;
            if (!live->sim.advance(h, error)) {
                if (error_out) *error_out = dup(error);
                return 0;
            }
            if (i % stride == stride - 1 || i == steps - 1) sample();
        }
        return 1;
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return 0;
    }
}

char* sieda_live_state(const SiedaLiveSim* live) {
    if (!live) return nullptr;
    try {
        const Schematic& s = live->schematic;
        Json root = Json::object();
        root["time"] = live->sim.time();
        auto volts = live->sim.netVoltages();
        Json nets = Json::array();
        for (const auto& n : s.nets()) {
            if (n.isGround || n.pins.size() < 2) continue;
            Json j = Json::object();
            j["index"] = n.index;
            j["name"] = n.name;
            j["voltage"] = static_cast<size_t>(n.index) < volts.size() ? volts[static_cast<size_t>(n.index)] : 0.0;
            nets.push(j);
        }
        root["nets"] = nets;
        Json devices = Json::array(), leds = Json::array();
        for (const auto& r : live->sim.deviceReadings()) {
            if (r.subIndex > 0) continue;
            const Component* c = s.find(r.componentId);
            if (!c) continue;
            Json j = Json::object();
            j["component"] = r.componentId;
            j["ref"] = c->ref;
            j["current"] = r.current;
            j["power"] = r.power;
            devices.push(j);
            if (c->kind == ComponentKind::LED) {
                Json l = Json::object();
                l["component"] = r.componentId;
                l["ref"] = c->ref;
                l["current"] = r.current;
                l["brightness"] = std::clamp(r.current / 0.015, 0.0, 1.0);  // full glow at 15 mA
                leds.push(l);
            }
        }
        root["devices"] = devices;
        root["leds"] = leds;
        Json switches = Json::array();
        for (const auto& [id, closed] : live->switches) {
            const Component* c = s.find(id);
            Json j = Json::object();
            j["component"] = id;
            j["ref"] = c ? c->ref : std::string();
            j["closed"] = closed;
            j["momentary"] = c ? isMomentary(c->value) : false;
            switches.push(j);
        }
        root["switches"] = switches;
        Json mcus = Json::array();
        for (const auto& m : live->sim.mcuReports()) {
            const Component* c = s.find(m.componentId);
            Json j = Json::object();
            j["component"] = m.componentId;
            j["ref"] = c ? c->ref : std::string();
            j["model"] = m.model;
            j["status"] = m.status;
            j["running"] = m.running;
            j["serial"] = m.serial;
            j["cycles"] = static_cast<double>(m.cycles);
            j["clockHz"] = m.clockHz;
            mcus.push(j);
        }
        root["mcus"] = mcus;
        Json trace = Json::object();
        Json times = Json::array();
        for (double t : live->traceTime) times.push(t);
        trace["time"] = times;
        Json tnets = Json::array();
        for (const auto& n : s.nets()) {
            if (n.isGround || n.pins.size() < 2 || static_cast<size_t>(n.index) >= live->traceNets.size()) continue;
            Json j = Json::object();
            j["index"] = n.index;
            j["name"] = n.name;
            Json vals = Json::array();
            for (double v : live->traceNets[static_cast<size_t>(n.index)]) vals.push(v);
            j["values"] = vals;
            tnets.push(j);
        }
        trace["nets"] = tnets;
        root["trace"] = trace;
        return dup(root.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_live_set_switch(SiedaLiveSim* live, int32_t component_id, int32_t closed) {
    if (!live || !live->switches.count(component_id)) return;
    live->switches[component_id] = closed != 0;
    live->sim.setSwitch(component_id, closed != 0);
}

void sieda_live_serial_input(SiedaLiveSim* live, int32_t component_id, const char* text) {
    if (live) live->sim.feedSerial(component_id, str(text));
}

void sieda_live_free(SiedaLiveSim* live) { delete live; }

int32_t sieda_set_firmware(SiedaProject* project, int32_t component_id, const char* hex, const char* name,
                           double clock_hz, char** error_out) {
    if (error_out) *error_out = nullptr;
    auto fail = [&](const std::string& message) {
        if (error_out) *error_out = dup(message);
        return 0;
    };
    if (!project) return fail("No project.");
    try {
        Component* c = project->project.schematic.find(component_id);
        if (!c) return fail("Unknown component.");
        const CustomPart* part = c->kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c->customPart)
                                                                  : nullptr;
        auto model = part ? mcuModelForPart(part->spec.name) : std::nullopt;
        if (!model)
            return fail(c->ref + " is not a microcontroller the simulator can run (ATmega328P or ATtiny85).");
        std::string text = str(hex);
        if (!text.empty()) {
            HexImage img = parseIntelHex(text);
            if (!img.ok()) return fail(img.error);
            AvrMcu probe(*model);
            std::string err;
            if (!probe.loadFirmware(img.bytes, err)) return fail(err);
        }
        project->project.schematic.setFirmware(component_id, text, str(name), clock_hz);
        return 1;
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}

char* sieda_component_firmware(const SiedaProject* project, int32_t component_id) {
    if (!project) return dup("");
    const Component* c = project->project.schematic.find(component_id);
    return dup(c ? c->firmware : std::string());
}

char* sieda_firmware_examples_json(void) {
    Json arr = Json::array();
    for (const auto& e : firmwareExamples()) {
        Json j = Json::object();
        j["id"] = e.id;
        j["name"] = e.name;
        j["model"] = e.model;
        j["description"] = e.description;
        arr.push(j);
    }
    return dup(arr.dump());
}

char* sieda_firmware_example_hex(const char* id) {
    const FirmwareExample* e = findFirmwareExample(str(id));
    return e ? dup(e->hex) : nullptr;
}

char* sieda_custom_part_register(SiedaProject* project, const char* spec_json, char** error_out) {
    if (error_out) *error_out = nullptr;
    if (!project) return nullptr;
    try {
        std::string id = project->project.addCustomPart(customPartSpecFromJson(Json::parse(str(spec_json))));
        return dup(customPartToJson(*CustomPartRegistry::instance().find(id)).dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_custom_part_preview(const char* spec_json, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        auto part = CustomPartRegistry::instance().registerPart(customPartSpecFromJson(Json::parse(str(spec_json))));
        return dup(customPartToJson(*part).dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_custom_part_land_pattern(const char* spec_json, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        return dup(customPartSpecToJson(landPatternFromFootprint(customPartSpecFromJson(Json::parse(str(spec_json))))).dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_check_land_pattern(const char* spec_json, double min_gap) {
    try {
        Json arr = Json::array();
        const CustomPartSpec spec = customPartSpecFromJson(Json::parse(str(spec_json)));
        for (const auto& issue : checkLandPattern(spec, min_gap > 0 ? min_gap : 0.1)) {
            Json j = Json::object();
            j["severity"] = issue.severity;
            j["code"] = issue.code;
            j["message"] = issue.message;
            Json pads = Json::array();
            for (int p : issue.pads) pads.push(p);
            j["pads"] = pads;
            arr.push(j);
        }
        return dup(arr.dump());
    } catch (const std::exception& e) {
        Json arr = Json::array();
        Json j = Json::object();
        j["severity"] = "error";
        j["code"] = "LAND_INVALID";
        j["message"] = e.what();
        j["pads"] = Json::array();
        arr.push(j);
        return dup(arr.dump());
    }
}

char* sieda_symbol_auto_arrange(const char* spec_json, int32_t stack, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        CustomPartSpec spec = customPartSpecFromJson(Json::parse(str(spec_json)));
        if (spec.pins.empty()) throw JsonError("The part has no pins to arrange.");
        spec.symbol = autoArrangeSymbol(spec, stack != 0);
        return dup(customPartSpecToJson(spec).dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_check_symbol(const char* spec_json) {
    Json arr = Json::array();
    auto add = [&](const std::string& severity, const std::string& code, const std::string& message,
                   const std::vector<std::string>& pins) {
        Json j = Json::object();
        j["severity"] = severity;
        j["code"] = code;
        j["message"] = message;
        Json pj = Json::array();
        for (const auto& p : pins) pj.push(p);
        j["pins"] = pj;
        arr.push(j);
    };
    try {
        for (const auto& i : checkSymbol(customPartSpecFromJson(Json::parse(str(spec_json)))))
            add(i.severity, i.code, i.message, i.pins);
    } catch (const std::exception& e) {
        add("error", "SYM_INVALID", e.what(), {});
    }
    return dup(arr.dump());
}

int32_t sieda_custom_part_remove(SiedaProject* project, const char* part_id) {
    if (!project) return 0;
    return guarded([&] { return project->project.removeCustomPart(str(part_id)) ? 1 : 0; });
}

int32_t sieda_custom_part_replace(SiedaProject* project, const char* old_id, const char* new_id) {
    if (!project) return 0;
    return guarded([&] {
        int n = project->project.schematic.replaceCustomPart(str(old_id), str(new_id));
        auto& lib = project->project.customLibrary;
        std::string newId = str(new_id);
        if (CustomPartRegistry::instance().find(newId) && std::find(lib.begin(), lib.end(), newId) == lib.end())
            lib.push_back(newId);
        project->project.schematicChanged();
        return n;
    });
}

int32_t sieda_add_custom_component(SiedaProject* project, const char* part_id, const char* value, double x, double y,
                                   int32_t rotation, const char* ref) {
    if (!project) return -1;
    try {
        int id = project->project.schematic.addCustomComponent(str(part_id), str(value), {x, y}, rotation, str(ref));
        if (id >= 0) {
            auto& lib = project->project.customLibrary;
            if (std::find(lib.begin(), lib.end(), str(part_id)) == lib.end()) lib.push_back(str(part_id));
            project->project.schematicChanged();
        }
        return id;
    } catch (...) {
        return -1;
    }
}

char* sieda_packages_json(void) {
    Json arr = Json::array();
    for (const auto& p : supportedPackages()) arr.push(p);
    return dup(arr.dump());
}

char* sieda_standard_parts_json(void) {
    try {
        Json arr = Json::array();
        for (const auto& p : standardParts()) {
            Json j = Json::object();
            j["category"] = p.category;
            j["spec"] = customPartSpecToJson(p.spec);
            arr.push(j);
        }
        return dup(arr.dump());
    } catch (...) {
        return dup("[]");
    }
}

char* sieda_supplier_parse(const char* source, const char* body, const char* currency) {
    try {
        return dup(supplierParseRequest(str(source), str(body), str(currency)).dump());
    } catch (...) {
        return dup("{\"schema\":\"sieda.supplier/1\",\"parts\":[],\"error\":\"out of memory\",\"errorKind\":\"parse\"}");
    }
}

char* sieda_supplier_merge(const char* request_json) {
    try {
        return dup(supplierMergeRequest(Json::parse(str(request_json))).dump());
    } catch (const std::exception& e) {
        Json j = Json::object();
        j["parts"] = Json::array();
        j["errors"] = Json::array();
        j["error"] = std::string("Invalid merge request: ") + e.what();
        return dup(j.dump());
    }
}

char* sieda_supplier_bom_rollup(const char* request_json) {
    try {
        return dup(supplierBomRollup(Json::parse(str(request_json))).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_supplier_catalog_match(const char* mpn) {
    try {
        return dup(catalogMatchForMpn(str(mpn)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_library_import(const char* request_json) {
    try {
        return dup(importLibraryRequest(Json::parse(str(request_json))).dump());
    } catch (const std::exception& e) {
        Json j = Json::object();
        j["parts"] = Json::array();
        Json files = Json::array();
        Json f = Json::object();
        f["name"] = "";
        f["format"] = "unknown";
        f["symbols"] = 0;
        f["footprints"] = 0;
        f["error"] = std::string("Invalid import request: ") + e.what();
        files.push(f);
        j["files"] = files;
        j["symbols"] = 0;
        j["footprints"] = 0;
        return dup(j.dump());
    }
}

static ESeries seriesFrom(int32_t s) { return s <= 12 ? ESeries::E12 : (s <= 24 ? ESeries::E24 : ESeries::E96); }
double sieda_nearest_standard_value(double value, int32_t series) { return nearestStandardValue(value, seriesFrom(series)); }
int32_t sieda_is_standard_value(double value, int32_t series) { return isStandardValue(value, seriesFrom(series)) ? 1 : 0; }
int32_t sieda_parse_value(const char* text, double* out) {
    if (!text) return 0;
    auto v = parseEngineeringValue(primaryValue(text));
    if (!v) return 0;
    if (out) *out = *v;
    return 1;
}

char* sieda_run_erc(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(Project::violationsToJson(project->project.schematic.runERC()).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_run_circuit_validation(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(Project::violationsToJson(validateCircuit(project->project.schematic, project->project.partRatings())).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_run_verification(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(verifyDesign(project->project).toJson().dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_simulate_dc(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        const Schematic circuit = project->project.simulationSchematic();  // the active variant as assembled
        Simulator sim(circuit);
        Json result = project->project.dcToJson(sim.dcOperatingPoint());
        addAssemblyNote(project->project, result);
        return dup(result.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_simulate_transient(const SiedaProject* project, double t_stop, double t_step) {
    if (!project) return nullptr;
    try {
        const Schematic circuit = project->project.simulationSchematic();
        Simulator sim(circuit);
        Json result = project->project.transientToJson(sim.transient(t_stop, t_step));
        addAssemblyNote(project->project, result);
        return dup(result.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

}  // extern "C"

namespace {
// Advanced analyses: options JSON in, result JSON out (sieda/Analysis.hpp).
char* runAnalysis(const SiedaProject* project, const char* options_json,
                  Json (*analysis)(const Schematic&, const Json&)) {
    if (!project) return nullptr;
    try {
        std::string text = str(options_json);
        Json options = text.find_first_not_of(" \t\r\n") == std::string::npos ? Json::object() : Json::parse(text);
        Json result = analysis(project->project.simulationSchematic(), options);
        if (result.isObject()) addAssemblyNote(project->project, result);
        return dup(result.dump());
    } catch (const std::exception& e) {
        Json j = Json::object();
        j["ok"] = false;
        j["error"] = std::string("Invalid analysis options: ") + e.what();
        return dup(j.dump());
    }
}
}  // namespace

extern "C" {

char* sieda_simulate_ac(const SiedaProject* project, const char* options_json) {
    return runAnalysis(project, options_json, simulateAcJson);
}

char* sieda_simulate_dc_sweep(const SiedaProject* project, const char* options_json) {
    return runAnalysis(project, options_json, simulateDcSweepJson);
}

char* sieda_simulate_param_sweep(const SiedaProject* project, const char* options_json) {
    return runAnalysis(project, options_json, simulateParamSweepJson);
}

char* sieda_simulate_monte_carlo(const SiedaProject* project, const char* options_json) {
    return runAnalysis(project, options_json, simulateToleranceJson);
}

char* sieda_simulate_fft(const SiedaProject* project, const char* options_json) {
    return runAnalysis(project, options_json, simulateFftJson);
}

char* sieda_spice_netlist(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(exportSpiceNetlist(project->project.simulationSchematic(), project->project.name));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_pcb_set_board(SiedaProject* project, double width, double height, double track_width, double clearance) {
    if (!project) return;
    auto& s = project->project.pcb.settings;
    if (width > 5) s.width = width;
    if (height > 5) s.height = height;
    if (track_width > 0.05) s.trackWidth = track_width;
    if (clearance > 0.05) s.clearance = clearance;
}

void sieda_pcb_autoplace(SiedaProject* project, int32_t all) {
    if (!project) return;
    try {
        project->project.pcb.autoPlace(project->project.schematic, all != 0);
        project->project.schematicChanged();
    } catch (...) {
    }
}

int32_t sieda_pcb_move_footprint(SiedaProject* project, int32_t id, double x, double y) {
    if (!project) return 0;
    Component* c = project->project.schematic.find(id);
    if (!c || !c->hasFootprint()) return 0;
    c->pcb.position = {x, y};
    c->pcb.placed = true;
    project->project.schematicChanged();
    return 1;
}

int32_t sieda_set_component_embedded(SiedaProject* project, int32_t id, int32_t layer) {
    if (!project || layer < 0) return 0;
    Component* c = project->project.schematic.find(id);
    if (!c || !canEmbed(*c)) return 0;
    c->pcb.embeddedLayer = layer;
    if (layer > 0) c->pcb.bottom = false;
    project->project.schematicChanged();
    return 1;
}

int32_t sieda_set_robot_platform(SiedaProject* project, const char* platform) {
    if (!project || !platform) return 0;
    std::string id = platform;
    if (!id.empty() && !findRobotPlatform(id)) return 0;
    project->project.robotPlatform = id;
    return 1;
}

char* sieda_robot_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(robotSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_ecu_type(SiedaProject* project, const char* type) {
    if (!project || !type) return 0;
    std::string id = type;
    if (!id.empty() && !findEcuType(id)) return 0;
    project->project.ecuType = id;
    return 1;
}

char* sieda_ecu_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(ecuSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_pcb_set_mechanical(SiedaProject* project, double thickness, int32_t underfill) {
    if (!project) return 0;
    auto& s = project->project.pcb.settings;
    if (thickness > 0) {
        if (thickness < 0.4 || thickness > 6.4) return 0;
        s.thickness = thickness;
    }
    s.underfill = underfill != 0;
    return 1;
}

int32_t sieda_pcb_set_isolation_gap(SiedaProject* project, double gap) {
    if (!project || gap < 0 || gap > 25) return 0;
    project->project.pcb.settings.isolationGap = gap;
    return 1;
}

int32_t sieda_set_medical_class(SiedaProject* project, const char* cls) {
    if (!project || !cls) return 0;
    std::string id = cls;
    if (!id.empty() && !findMedicalClass(id)) return 0;
    project->project.medicalClass = id;
    return 1;
}

char* sieda_medical_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(medicalSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_retail_device(SiedaProject* project, const char* device) {
    if (!project || !device) return 0;
    std::string id = device;
    if (!id.empty() && !findRetailDevice(id)) return 0;
    project->project.retailDevice = id;
    return 1;
}

int32_t sieda_set_appliance_type(SiedaProject* project, const char* type) {
    if (!project || !type) return 0;
    std::string id = type;
    if (!id.empty() && !findApplianceType(id)) return 0;
    project->project.applianceType = id;
    return 1;
}

char* sieda_appliance_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(applianceSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_memory_design(SiedaProject* project, const char* type) {
    if (!project || !type) return 0;
    std::string id = type;
    if (!id.empty() && !findMemoryDesignType(id)) return 0;
    project->project.memoryDesign = id;
    return 1;
}

char* sieda_memory_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(memorySegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_retail_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(retailSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_naval_platform(SiedaProject* project, const char* platform) {
    if (!project || !platform) return 0;
    std::string id = platform;
    if (!id.empty() && !findNavalPlatform(id)) return 0;
    project->project.navalPlatform = id;
    return 1;
}

char* sieda_naval_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(navalSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_aerospace_mission(SiedaProject* project, const char* mission) {
    if (!project || !mission) return 0;
    std::string id = mission;
    if (!id.empty() && !findAerospaceMission(id)) return 0;
    project->project.aerospaceMission = id;
    return 1;
}

char* sieda_aerospace_segments_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(aerospaceSegmentsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_pcb_add_thermal_vias(SiedaProject* project, int32_t id) {
    if (!project) return 0;
    return addThermalVias(project->project, id);
}

int32_t sieda_pcb_lock_footprint(SiedaProject* project, int32_t id, int32_t locked) {
    if (!project) return 0;
    Component* c = project->project.schematic.find(id);
    if (!c || !c->hasFootprint() || (locked && !c->pcb.placed)) return 0;
    c->pcb.locked = locked != 0;
    return 1;
}

int32_t sieda_pcb_rotate_footprint(SiedaProject* project, int32_t id, int32_t delta) {
    if (!project) return 0;
    Component* c = project->project.schematic.find(id);
    if (!c || !c->hasFootprint()) return 0;
    c->pcb.rotation = (((c->pcb.rotation + delta) % 360) + 360) % 360;
    project->project.schematicChanged();
    return 1;
}

int32_t sieda_pcb_flip_footprint(SiedaProject* project, int32_t id) {
    if (!project) return 0;
    Component* c = project->project.schematic.find(id);
    if (!c || !c->hasFootprint()) return 0;
    c->pcb.bottom = !c->pcb.bottom;
    project->project.schematicChanged();
    return 1;
}

int32_t sieda_pcb_fit_board(SiedaProject* project, double margin_mm) {
    if (!project) return 0;
    return guarded([&] {
        return project->project.pcb.fitBoardToComponents(project->project.schematic, margin_mm) ? 1 : 0;
    });
}

void sieda_pcb_set_layer_count(SiedaProject* project, int32_t layers) {
    if (!project) return;
    auto& pcb = project->project.pcb;
    pcb.settings.layerCount = BoardSettings::normalizeLayerCount(layers);
    int bottom = pcb.settings.bottomLayer();
    pcb.tracks.erase(std::remove_if(pcb.tracks.begin(), pcb.tracks.end(), [&](const Track& t) { return t.layer > bottom; }),
                     pcb.tracks.end());
    if (pcb.settings.layerCount == 1) pcb.vias.clear();
    for (auto& v : pcb.vias) {  // blind / buried spans belong to the old stack-up
        v.fromLayer = 0;
        v.toLayer = -1;
    }
    pcb.zones.erase(std::remove_if(pcb.zones.begin(), pcb.zones.end(), [&](const CopperZone& z) { return z.layer > bottom; }),
                    pcb.zones.end());
}

char* sieda_pcb_autoroute(SiedaProject* project) {
    if (!project) return nullptr;
    try {
        project->project.pcb.autoPlace(project->project.schematic, false);
        RouteStats s = project->project.pcb.autoRoute(project->project.schematic);
        // Robots: stitched thermal vias under the power FETs (motion-control segment).
        const int thermal = autoThermalVias(project->project);
        Json j = Json::object();
        j["thermalVias"] = thermal;
        j["connections"] = s.connections;
        j["routed"] = s.routed;
        j["failed"] = s.failed;
        j["vias"] = s.vias;
        j["trackLength"] = s.trackLength;
        j["lengthTuned"] = s.lengthTuned;
        Json failed = Json::array();
        for (const auto& n : s.failedNets) failed.push(n);
        j["failedNets"] = failed;
        return dup(j.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_pcb_clear_routing(SiedaProject* project) {
    if (project) project->project.pcb.clearRouting();
}

char* sieda_pcb_run_drc(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        // Layout rules, then the design-for-reliability rules (leakage, thermal, SI/EMI, assembly, industry risks).
        auto found = project->project.pcb.runDRC(project->project.schematic);
        for (auto& v : reliabilityChecks(project->project)) found.push_back(std::move(v));
        return dup(Project::violationsToJson(found).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_design_rule_presets_json(void) {
    Json arr = Json::array();
    for (const auto& p : designRulePresets()) {
        Json j = Json::object();
        j["name"] = p.name;
        j["description"] = p.description;
        j["trackWidth"] = p.trackWidth;
        j["clearance"] = p.clearance;
        j["viaDrill"] = p.viaDrill;
        j["viaDiameter"] = p.viaDiameter;
        j["edgeClearance"] = p.edgeClearance;
        j["minTrackWidth"] = p.minTrackWidth;
        j["minClearance"] = p.minClearance;
        j["minDrill"] = p.minDrill;
        j["minAnnularRing"] = p.minAnnularRing;
        j["minHoleToHole"] = p.minHoleToHole;
        arr.push(j);
    }
    return dup(arr.dump());
}

int32_t sieda_pcb_set_net_width(SiedaProject* project, const char* net_name, double width_mm) {
    if (!project || !net_name || !*net_name) return 0;
    auto& widths = project->project.pcb.settings.netWidths;
    if (width_mm > 0) widths[net_name] = width_mm;
    else widths.erase(net_name);
    return 1;
}

char* sieda_pcb_auto_net_widths(SiedaProject* project) {
    if (!project) return nullptr;
    try {
        Json out = Json::object();
        for (const auto& [net, w] : project->project.pcb.autoNetWidths(project->project.schematic)) out[net] = w;
        return dup(out.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_pcb_set_auto_size_nets(SiedaProject* project, int32_t enabled) {
    if (project) project->project.pcb.settings.autoSizeNets = enabled != 0;
}

int32_t sieda_pcb_set_solder_mask(SiedaProject* project, const char* colour) {
    if (!project || !colour || !findSolderMask(colour)) return 0;
    project->project.pcb.settings.solderMask = colour;
    return 1;
}

int32_t sieda_pcb_set_stackup(SiedaProject* project, const char* material, const char* construction, double se, double diff,
                              int32_t backdrill) {
    if (!project || !material || !construction || !findLaminate(material)) return 0;
    std::string c = construction;
    if (c != "rigid" && c != "rigid-flex" && c != "metal-core") return 0;
    auto& s = project->project.pcb.settings;
    s.material = material;
    s.construction = c;
    s.singleEndedImpedance = std::clamp(se, 20.0, 150.0);
    s.differentialImpedance = std::clamp(diff, 50.0, 200.0);
    s.backdrill = backdrill != 0;
    return 1;
}

int32_t sieda_pcb_set_length_matching(SiedaProject* project, int32_t enabled, double pair_skew_mm, double bus_mm) {
    if (!project || !(pair_skew_mm > 0) || !(bus_mm > 0)) return 0;
    auto& s = project->project.pcb.settings;
    s.lengthTuning = enabled != 0;
    s.pairSkewTolerance = std::clamp(pair_skew_mm, 0.02, 5.0);
    s.busLengthTolerance = std::clamp(bus_mm, 0.02, 20.0);
    return 1;
}

int32_t sieda_pcb_set_hdi(SiedaProject* project, int32_t hdi, double microvia_drill, double microvia_diameter,
                          int32_t via_in_pad) {
    if (!project || !(microvia_drill > 0) || !(microvia_diameter > microvia_drill)) return 0;
    auto& s = project->project.pcb.settings;
    s.hdi = hdi != 0;
    s.microviaDrill = std::clamp(microvia_drill, 0.05, 0.15);
    s.microviaDiameter = std::clamp(microvia_diameter, 0.15, 0.5);
    s.viaInPad = via_in_pad != 0;
    return 1;
}

int32_t sieda_pcb_apply_hdi(SiedaProject* project) {
    if (!project || !project->project.pcb.settings.hdi) return 0;
    return project->project.pcb.applyHdiVias(project->project.schematic);
}

int32_t sieda_pcb_tune_lengths(SiedaProject* project) {
    if (!project) return 0;
    return tuneLengths(project->project.pcb, project->project.schematic);
}

char* sieda_length_report_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(lengthReportJson(project->project.pcb, project->project.schematic).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_stackup_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        Json j = stackupJson(project->project.pcb.settings);
        Json mats = Json::array();
        for (const auto& m : laminateMaterials()) {
            Json x = Json::object();
            x["id"] = m.id;
            x["name"] = m.name;
            x["er"] = m.er;
            x["lossTangent"] = m.lossTangent;
            x["tg"] = m.tg;
            x["note"] = m.note;
            mats.push(x);
        }
        j["materials"] = mats;
        return dup(j.dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_pcb_set_coating(SiedaProject* project, const char* coating) {
    if (!project || !coating) return 0;
    const auto& all = conformalCoatings();
    if (std::find(all.begin(), all.end(), coating) == all.end()) return 0;
    project->project.pcb.settings.coating = coating;
    return 1;
}

int32_t sieda_pcb_set_outline(SiedaProject* project, const char* points_json) {
    if (!project || !points_json) return 0;
    try {
        std::vector<Vec2> pts;
        Json parsed = Json::parse(points_json);
        for (const auto& j : parsed.items()) pts.push_back({j.get("x").asNumber(), j.get("y").asNumber()});
        if (!pts.empty() && pts.size() < 3) return 0;
        project->project.pcb.settings.setOutline(pts);
        return 1;
    } catch (...) {
        return 0;
    }
}

int32_t sieda_pcb_outline_preset(SiedaProject* project, const char* kind, double w, double h, double param) {
    if (!project || !kind) return 0;
    std::string k = kind;
    auto poly = boardOutlinePreset(k, w, h, param);
    if (poly.size() < 3) return 0;
    auto& s = project->project.pcb.settings;
    if (k == "rectangle") {
        s.outline.clear();
        s.width = w;
        s.height = h;
    } else {
        s.setOutline(poly);
    }
    return 1;
}

int32_t sieda_pcb_add_mounting_hole(SiedaProject* project, double x, double y, double drill_mm, double keepout_mm) {
    if (!project || !(drill_mm > 0)) return 0;
    MountingHole h;
    h.position = {x, y};
    h.drill = drill_mm;
    h.keepout = keepout_mm > 0 ? std::max(keepout_mm, drill_mm) : 2 * drill_mm;
    auto& holes = project->project.pcb.settings.holes;
    holes.push_back(h);
    return static_cast<int32_t>(holes.size());
}

void sieda_pcb_clear_mounting_holes(SiedaProject* project) {
    if (project) project->project.pcb.settings.holes.clear();
}

int32_t sieda_pcb_add_zone(SiedaProject* project, const char* net_name, int32_t layer, int32_t plane, double clearance_mm) {
    if (!project || !net_name || !*net_name) return -1;
    auto& pcb = project->project.pcb;
    if (layer < 0 || layer >= pcb.settings.layerCount) return -1;
    CopperZone z;
    z.net = net_name;
    z.layer = layer;
    z.plane = plane != 0;
    z.clearance = std::max(0.0, clearance_mm);
    pcb.zones.push_back(z);
    return static_cast<int32_t>(pcb.zones.size()) - 1;
}

int32_t sieda_pcb_remove_zone(SiedaProject* project, int32_t index) {
    if (!project || index < 0 || index >= static_cast<int32_t>(project->project.pcb.zones.size())) return 0;
    auto& zones = project->project.pcb.zones;
    zones.erase(zones.begin() + index);
    return 1;
}

void sieda_pcb_clear_zones(SiedaProject* project) {
    if (project) project->project.pcb.zones.clear();
}

int32_t sieda_pcb_add_tamper_mesh(SiedaProject* project, const char* ref, const char* net_a, const char* net_b,
                                  int32_t layer_a, int32_t layer_b, double margin_mm) {
    if (!project || !ref || !*ref || !net_a || !*net_a || !net_b || !*net_b) return -1;
    auto& pcb = project->project.pcb;
    TamperMesh m;
    m.componentRef = ref;
    m.netA = net_a;
    m.netB = net_b;
    m.layerA = layer_a;
    m.layerB = layer_b;
    m.margin = std::clamp(margin_mm, 0.0, 20.0);
    pcb.tamperMeshes.push_back(m);
    return static_cast<int32_t>(pcb.tamperMeshes.size()) - 1;
}

void sieda_pcb_clear_tamper_meshes(SiedaProject* project) {
    if (project) project->project.pcb.tamperMeshes.clear();
}

int32_t sieda_pcb_apply_rule_preset(SiedaProject* project, const char* name) {
    if (!project || !name) return 0;
    return project->project.pcb.settings.applyPreset(name) ? 1 : 0;
}

char* sieda_export(const SiedaProject* project, const char* format) {
    if (!project || !format) return nullptr;
    try {
        const auto& p = project->project;
        std::string f = format;
        if (f == "spice") return dup(exportSpiceNetlist(p.schematic, p.name));
        if (f != "bom_json") {
            std::string text;
            if (p.activeVariant.empty() ? assemblyExport(p, p.schematic, f, &text)
                                        : assemblyExport(p, p.variantSchematic(p.activeVariant), f, &text))
                return dup(text);
        }
        if (f == "gerber_top") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::TopCopper));
        if (f == "gerber_bottom") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::BottomCopper));
        if (f == "gerber_mask_top") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::TopMask));
        if (f == "gerber_mask_bottom") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::BottomMask));
        if (f == "gerber_silk_top") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::TopSilk));
        if (f == "gerber_edge") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::EdgeCuts));
        if (f == "gerber_paste_top") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::TopPaste));
        if (f == "gerber_paste_bottom") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::BottomPaste));
        if (f == "gerber_silk_bottom") return dup(exportGerber(p.schematic, p.pcb, GerberLayer::BottomSilk));
        if (f == "ipc356") return dup(exportIpcD356(p.schematic, p.pcb, p.name));
        if (f == "gerber_job") return dup(exportGerberJob(p, fabricationPackage(p)));
        if (f == "fab_notes") {
            for (const auto& file : fabricationPackage(p))
                if (file.path == "fab_notes.txt") return dup(file.content);
        }
        if (f == "drill") return dup(exportExcellonDrill(p.schematic, p.pcb));
        if (f == "drill_npth") return dup(exportExcellonDrill(p.schematic, p.pcb, false));
        if (f.rfind("gerber_l", 0) == 0) {
            int layer = std::atoi(f.c_str() + 8) - 1;
            if (layer < 0 || layer >= p.pcb.settings.layerCount) return nullptr;
            return dup(exportCopperGerber(p.schematic, p.pcb, layer));
        }
        if (f == "stl") return dup(exportStl(buildAssemblyMesh(p.schematic, p.pcb), p.name));
        if (f == "obj") return dup(exportObj(buildAssemblyMesh(p.schematic, p.pcb), p.name));
        return nullptr;
    } catch (...) {
        return nullptr;
    }
}

char* sieda_bom_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        const Project& p = project->project;
        if (p.activeVariant.empty()) return dup(bomJson(p.schematic, p.buildQuantity).dump());
        return dup(bomJson(p.variantSchematic(p.activeVariant), p.buildQuantity).dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_component_sourcing(SiedaProject* project, int32_t component_id, const char* json) {
    if (!project || !json) return 0;
    try {
        Component* c = project->project.schematic.find(component_id);
        if (!c) return 0;
        Json j = Json::parse(json);
        Sourcing& s = c->sourcing;
        auto text = [&](const char* key, std::string& field) {
            if (j.has(key)) field = j.get(key).asString("");
        };
        text("manufacturer", s.manufacturer);
        text("mpn", s.mpn);
        text("supplierPart", s.supplierPart);
        if (j.has("unitPrice")) s.unitPrice = std::max(0.0, j.get("unitPrice").asNumber(0));
        if (j.has("dnp")) s.dnp = j.get("dnp").asBool(false);
        return 1;
    } catch (...) {
        return 0;
    }
}

void sieda_set_build_quantity(SiedaProject* project, int32_t quantity) {
    if (project) project->project.buildQuantity = std::max(1, static_cast<int>(quantity));
}

char* sieda_write_fabrication_package(const SiedaProject* project, const char* dir, const char* base) {
    Json out = Json::object();
    std::vector<std::string> written;
    std::string error;
    bool ok = project && dir &&
              writeFabricationPackage(project->project, dir, &written, &error, base ? std::string(base) : std::string());
    if (!project || !dir) error = "No project or folder";
    Json files = Json::array();
    for (const auto& f : written) files.push(f);
    out["ok"] = ok;
    out["files"] = files;
    out["error"] = error;
    return dup(out.dump());
}

// ---- signal & power integrity ----------------------------------------------------------------------------------------

namespace {
int netByName(const Schematic& sch, const std::string& name) {
    for (const auto& n : sch.nets())
        if (n.name == name) return n.index;
    return -1;
}
}  // namespace

char* sieda_ibis_parse(const char* text, const char* corner, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        return dup(ibisFileJson(parseIbis(str(text)), corner ? corner : "typ").dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

int32_t sieda_si_import_ibis(SiedaProject* project, const char* text, const char* corner, const char* ref, char** error_out) {
    if (error_out) *error_out = nullptr;
    if (!project) return 0;
    try {
        const IbisFile f = parseIbis(str(text));
        const std::string c = corner && *corner ? corner : "typ";
        const IbisComponent* comp = f.components.empty() ? nullptr : &f.components.front();
        SiSettings& si = project->project.si;
        for (const auto& m : f.models) {
            DriverModel d = driverFromIbis(m, c, comp);
            auto it = std::find_if(si.models.begin(), si.models.end(), [&](const DriverModel& x) { return x.id == d.id; });
            if (it != si.models.end()) *it = d;
            else si.models.push_back(d);
        }
        const std::string r = str(ref);
        if (!r.empty() && comp) {
            const Component* part = project->project.schematic.findByRef(r);
            if (!part) throw std::runtime_error("No component " + r);
            for (const auto& pin : part->def().pins) {
                const std::string key = pin.number.empty() ? pin.name : pin.number;
                for (const auto& ip : comp->pins)
                    if (ip.pin == key)
                        if (const IbisModel* m = f.findModel(ip.model)) si.pinModels[r + "." + key] = "ibis:" + m->name;
            }
        }
        return static_cast<int32_t>(f.models.size());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return 0;
    }
}

int32_t sieda_si_assign_model(SiedaProject* project, const char* kind, const char* target, const char* model_id) {
    if (!project || !kind || !target || !*target) return 0;
    SiSettings& si = project->project.si;
    const std::string k = kind, t = target, id = str(model_id);
    std::map<std::string, std::string>* map = k == "net" ? &si.netModels : k == "component" ? &si.componentModels
                                              : k == "pin"                                ? &si.pinModels
                                                                                          : nullptr;
    if (!map) return 0;
    if (id.empty()) {
        map->erase(t);
        return 1;
    }
    if (!si.findModel(id)) return 0;
    (*map)[t] = id;
    return 1;
}

int32_t sieda_si_set_options(SiedaProject* project, int32_t sign_off, double overshoot_limit, double crosstalk_limit) {
    if (!project || !(overshoot_limit > 0) || !(crosstalk_limit > 0)) return 0;
    SiSettings& si = project->project.si;
    si.signOff = sign_off != 0;
    si.overshootLimit = std::clamp(overshoot_limit, 0.01, 1.0);
    si.crosstalkLimit = std::clamp(crosstalk_limit, 0.005, 0.5);
    return 1;
}

int32_t sieda_pi_set_rail(SiedaProject* project, const char* net_name, double ripple_percent, double transient_amps,
                          double dc_amps) {
    if (!project || !net_name || !*net_name || ripple_percent < 0 || transient_amps < 0 || dc_amps < 0) return 0;
    auto& rails = project->project.si.rails;
    auto it = std::find_if(rails.begin(), rails.end(), [&](const PdnRailSettings& r) { return r.net == net_name; });
    if (ripple_percent == 0 && transient_amps == 0 && dc_amps == 0) {
        if (it != rails.end() && (it->vrmR > 0 || it->vrmBandwidth > 0)) it->ripplePercent = it->transientCurrent = it->dcCurrent = 0;
        else if (it != rails.end()) rails.erase(it);
        return 1;
    }
    PdnRailSettings r;
    if (it != rails.end()) r = *it;  // keeps the regulator override
    r.net = net_name;
    r.ripplePercent = std::min(ripple_percent, 50.0);
    r.transientCurrent = std::min(transient_amps, 1000.0);
    r.dcCurrent = std::min(dc_amps, 1000.0);
    if (it != rails.end()) *it = r;
    else rails.push_back(r);
    return 1;
}

char* sieda_si_settings_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        Json j = project->project.si.toJson();
        Json fam = Json::array();
        for (const auto& f : logicFamilies()) fam.push(driverModelToJson(f));
        j["families"] = fam;
        return dup(j.dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_si_net_list_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(siNetsJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_si_net_json(const SiedaProject* project, const char* net_name, double series_ohms) {
    if (!project) return nullptr;
    try {
        const int net = netByName(project->project.schematic, str(net_name));
        if (net < 0) {
            Json j = Json::object();
            j["error"] = "Unknown net " + str(net_name);
            return dup(j.dump());
        }
        return dup(siNetJson(analyzeNet(project->project, net, series_ohms)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_si_crosstalk_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(crosstalkJson(project->project).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_pi_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(pdnJson(analyzePdn(project->project)).dump());
    } catch (...) {
        return nullptr;
    }
}

char* sieda_si_checks_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(Project::violationsToJson(signalPowerIntegrityChecks(project->project)).dump());
    } catch (...) {
        return nullptr;
    }
}

SiedaMesh* sieda_mesh_build(const SiedaProject* project, int32_t include_components) {
    if (!project) return nullptr;
    try {
        auto* m = new SiedaMesh();
        MeshOptions opt;
        opt.components = include_components != 0;
        m->mesh = buildAssemblyMesh(project->project.schematic, project->project.pcb, opt);
        return m;
    } catch (...) {
        return nullptr;
    }
}

SiedaMesh* sieda_mesh_build_layer(const SiedaProject* project, int32_t layer) {
    if (!project || layer < 0 || layer >= project->project.pcb.settings.layerCount) return nullptr;
    try {
        auto* m = new SiedaMesh();
        m->mesh = buildCopperLayerMesh(project->project.schematic, project->project.pcb, layer);
        return m;
    } catch (...) {
        return nullptr;
    }
}

void sieda_mesh_free(SiedaMesh* mesh) { delete mesh; }
int32_t sieda_mesh_vertex_count(const SiedaMesh* m) { return m ? static_cast<int32_t>(m->mesh.vertexCount()) : 0; }
int32_t sieda_mesh_index_count(const SiedaMesh* m) { return m ? static_cast<int32_t>(m->mesh.indices.size()) : 0; }
const float* sieda_mesh_positions(const SiedaMesh* m) { return m ? m->mesh.positions.data() : nullptr; }
const float* sieda_mesh_normals(const SiedaMesh* m) { return m ? m->mesh.normals.data() : nullptr; }
const float* sieda_mesh_colors(const SiedaMesh* m) { return m ? m->mesh.colors.data() : nullptr; }
const uint8_t* sieda_mesh_surfaces(const SiedaMesh* m) { return m ? m->mesh.surfaces.data() : nullptr; }
const uint32_t* sieda_mesh_indices(const SiedaMesh* m) { return m ? m->mesh.indices.data() : nullptr; }

/* ---- sheets, hierarchy, buses, annotation ---- */

char* sieda_sheets_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        const Schematic& sch = project->project.schematic;
        std::map<int, int> counts;
        for (const auto& c : sch.components()) ++counts[c.sheet];
        Json out = Json::object();
        out["active"] = sch.activeSheet();
        Json arr = Json::array();
        for (const auto& s : sch.sheets()) {
            Json j = Json::object();
            j["id"] = s.id;
            j["name"] = s.name;
            j["parent"] = s.parent;
            j["depth"] = sch.sheetDepth(s.id);
            j["components"] = counts[s.id];
            Json ports = Json::array();
            for (const auto& port : sch.sheetPorts(s.id)) ports.push(port);
            j["ports"] = ports;
            if (sch.isRepeated(s.id)) {
                j["instanceOf"] = s.instanceOf;
                j["channel"] = s.channel;
                j["refs"] = instanceRefsName(sch.findSheet(sch.definitionSheet(s.id))->refs);
                j["instances"] = static_cast<int>(sch.sheetInstances(s.id).size());
            }
            arr.push(j);
        }
        out["sheets"] = arr;
        return dup(out.dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_add_sheet(SiedaProject* project, const char* name, int32_t parent) {
    if (!project) return -1;
    try {
        return project->project.schematic.addSheet(str(name), parent);
    } catch (...) {
        return -1;
    }
}

int32_t sieda_rename_sheet(SiedaProject* project, int32_t sheet, const char* name) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.renameSheet(sheet, str(name)) ? 1 : 0; });
}

int32_t sieda_set_sheet_parent(SiedaProject* project, int32_t sheet, int32_t parent) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.setSheetParent(sheet, parent) ? 1 : 0; });
}

int32_t sieda_reorder_sheet(SiedaProject* project, int32_t sheet, int32_t index) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.reorderSheet(sheet, index) ? 1 : 0; });
}

int32_t sieda_remove_sheet(SiedaProject* project, int32_t sheet, int32_t delete_contents) {
    if (!project) return 0;
    return guarded([&] {
        bool ok = project->project.schematic.removeSheet(sheet, delete_contents != 0);
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_set_active_sheet(SiedaProject* project, int32_t sheet) {
    if (!project) return 0;
    return project->project.schematic.setActiveSheet(sheet) ? 1 : 0;
}

int32_t sieda_move_to_sheet(SiedaProject* project, const int32_t* component_ids, int32_t count, int32_t sheet) {
    if (!project || !component_ids || count <= 0) return 0;
    return guarded([&] {
        std::vector<int> ids(component_ids, component_ids + count);
        int moved = project->project.schematic.moveToSheet(ids, sheet);
        if (moved > 0) project->project.schematicChanged();
        return moved;
    });
}

int32_t sieda_set_label_scope(SiedaProject* project, int32_t component_id, const char* scope, int32_t target_sheet) {
    if (!project || !scope) return 0;
    return guarded([&] {
        LabelScope s;
        if (!labelScopeFromName(scope, &s)) return 0;
        bool ok = project->project.schematic.setLabelScope(component_id, s, target_sheet);
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_place_sheet_entries(SiedaProject* project, int32_t child, double x, double y) {
    if (!project) return -1;
    try {
        int added = project->project.schematic.placeSheetEntries(child, {x, y});
        if (added > 0) project->project.schematicChanged();
        return added;
    } catch (...) {
        return -1;
    }
}

char* sieda_expand_bus(const char* bus) {
    try {
        Json arr = Json::array();
        for (const auto& m : expandBus(str(bus))) arr.push(m);
        return dup(arr.dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_add_bus_labels(SiedaProject* project, int32_t component_id, const int32_t* pins, int32_t count,
                             const char* bus, const char* scope) {
    if (!project || !pins || count <= 0) return -1;
    try {
        LabelScope s = LabelScope::Global;
        if (scope && *scope && !labelScopeFromName(scope, &s)) return -1;
        std::vector<int> list(pins, pins + count);
        int added = project->project.schematic.addBusLabels(component_id, list, str(bus), s);
        if (added > 0) project->project.schematicChanged();
        return added;
    } catch (...) {
        return -1;
    }
}

char* sieda_annotate(SiedaProject* project, const char* options_json) {
    if (!project) return nullptr;
    try {
        AnnotateOptions o;
        if (options_json && *options_json) {
            Json j = Json::parse(options_json);
            o.byColumns = j.get("order").asString("rows") == "columns";
            o.keepExisting = j.get("keepExisting").asBool(false);
            o.sheetNumbering = j.get("sheetNumbering").asBool(false);
            o.packUnits = j.get("packUnits").asBool(false);
        }
        Json changed = Json::array();
        for (const auto& ch : project->project.annotate(o)) {
            Json c = Json::object();
            c["component"] = ch.component;
            c["from"] = ch.from;
            c["to"] = ch.to;
            changed.push(c);
        }
        Json out = Json::object();
        out["changed"] = changed;
        return dup(out.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

/* ---- design variants ---- */

char* sieda_variants_json(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        const Project& p = project->project;
        Json out = Json::object();
        out["active"] = p.activeVariant;
        Json arr = Json::array();
        for (const auto& v : p.variants) arr.push(variantToJson(v, p.schematic));
        out["variants"] = arr;
        return dup(out.dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_add_variant(SiedaProject* project, const char* name, const char* copy_from) {
    if (!project) return 0;
    return guarded([&] { return project->project.addVariant(str(name), str(copy_from)) ? 1 : 0; });
}

int32_t sieda_rename_variant(SiedaProject* project, const char* name, const char* new_name) {
    if (!project) return 0;
    return guarded([&] { return project->project.renameVariant(str(name), str(new_name)) ? 1 : 0; });
}

int32_t sieda_remove_variant(SiedaProject* project, const char* name) {
    if (!project) return 0;
    return guarded([&] { return project->project.removeVariant(str(name)) ? 1 : 0; });
}

int32_t sieda_set_variant_description(SiedaProject* project, const char* name, const char* description) {
    if (!project) return 0;
    return guarded([&] { return project->project.setVariantDescription(str(name), str(description)) ? 1 : 0; });
}

int32_t sieda_set_variant_part(SiedaProject* project, const char* name, int32_t component_id, int32_t fitted,
                               const char* value) {
    if (!project) return 0;
    return guarded([&] {
        const std::string text = str(value);
        return project->project.setVariantPart(str(name), component_id, fitted, value ? &text : nullptr) ? 1 : 0;
    });
}

int32_t sieda_set_active_variant(SiedaProject* project, const char* name) {
    if (!project) return 0;
    return guarded([&] { return project->project.setActiveVariant(str(name)) ? 1 : 0; });
}

char* sieda_export_variant(const SiedaProject* project, const char* format, const char* variant) {
    if (!project || !format) return nullptr;
    try {
        const Project& p = project->project;
        const std::string name = str(variant);
        if (!name.empty() && !p.findVariant(name)) return nullptr;
        std::string text;
        if (!assemblyExport(p, p.variantSchematic(name), format, &text)) return nullptr;
        return dup(text);
    } catch (...) {
        return nullptr;
    }
}

}  // extern "C"

// ---- interactive routing

namespace {
InteractiveRouter& routerOf(SiedaProject* project) {
    if (!project->router) project->router = std::make_unique<InteractiveRouter>(project->project.pcb, project->project.schematic);
    return *project->router;
}

void applyRouterOptions(SiedaProject* project, const char* options_json) {
    if (!options_json || !*options_json) return;
    InteractiveRouter& r = routerOf(project);
    r.setOptions(routerOptionsFromJson(Json::parse(options_json), r.options()));
}

char* routerPreview(SiedaProject* project, bool ok) {
    InteractiveRouter& r = routerOf(project);
    Json j = routePreviewJson(r.preview());
    if (!ok) j["error"] = r.error().empty() ? std::string("Not possible here") : r.error();
    return dup(j.dump());
}
}  // namespace

extern "C" {

char* sieda_router_begin(SiedaProject* project, const char* options_json, double x, double y, int32_t layer) {
    if (!project) return nullptr;
    try {
        applyRouterOptions(project, options_json);
        return routerPreview(project, routerOf(project).beginRoute({x, y}, layer));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_begin_pair(SiedaProject* project, const char* options_json, double x, double y, int32_t layer) {
    if (!project) return nullptr;
    try {
        applyRouterOptions(project, options_json);
        return routerPreview(project, routerOf(project).beginPair({x, y}, layer));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_begin_drag(SiedaProject* project, const char* options_json, int32_t track_id, double x, double y) {
    if (!project) return nullptr;
    try {
        applyRouterOptions(project, options_json);
        return routerPreview(project, routerOf(project).beginDrag(track_id, {x, y}));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_move(SiedaProject* project, double x, double y) {
    if (!project) return nullptr;
    try {
        routerOf(project).moveTo({x, y});
        return routerPreview(project, true);
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_fix(SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return routerPreview(project, routerOf(project).fixHead());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_add_via(SiedaProject* project, int32_t to_layer) {
    if (!project) return nullptr;
    try {
        return routerPreview(project, routerOf(project).addVia(to_layer));
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_set_options(SiedaProject* project, const char* options_json) {
    if (!project) return nullptr;
    try {
        applyRouterOptions(project, options_json);  // re-aims an active head at the last cursor position
        return routerPreview(project, true);
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_router_commit(SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(routeChangesJson(routerOf(project).commit()).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

void sieda_router_cancel(SiedaProject* project) {
    if (project && project->router) project->router->cancel();
}

int32_t sieda_router_active(const SiedaProject* project) {
    return project && project->router && project->router->active() ? 1 : 0;
}

char* sieda_router_tune_length(SiedaProject* project, int32_t track_id, double target_mm, double max_amplitude_mm) {
    if (!project) return nullptr;
    try {
        if (project->router) project->router->cancel();
        const LengthTuneResult r =
            tuneTrackLength(project->project.pcb, project->project.schematic, track_id, target_mm, max_amplitude_mm);
        Json j = Json::object();
        j["ok"] = r.ok;
        j["message"] = r.message;
        j["net"] = r.net;
        j["before"] = r.before;
        j["after"] = r.after;
        j["target"] = r.target;
        j["changes"] = routeChangesJson(r.changes);
        return dup(j.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

int32_t sieda_pcb_lock_track(SiedaProject* project, int32_t track_id, int32_t locked) {
    if (!project) return 0;
    for (auto& t : project->project.pcb.tracks)
        if (t.id == track_id) {
            t.locked = locked != 0;
            return 1;
        }
    return 0;
}

int32_t sieda_pcb_remove_track(SiedaProject* project, int32_t track_id) {
    if (!project) return 0;
    auto& tracks = project->project.pcb.tracks;
    const auto n = tracks.size();
    tracks.erase(std::remove_if(tracks.begin(), tracks.end(), [&](const Track& t) { return t.id == track_id; }), tracks.end());
    return tracks.size() < n ? 1 : 0;
}

int32_t sieda_pcb_remove_via(SiedaProject* project, int32_t via_id) {
    if (!project) return 0;
    auto& vias = project->project.pcb.vias;
    const auto n = vias.size();
    vias.erase(std::remove_if(vias.begin(), vias.end(), [&](const Via& v) { return v.id == via_id; }), vias.end());
    return vias.size() < n ? 1 : 0;
}

}  // extern "C"

// ---- schematic capture: repeated sheets, graphical buses, multi-unit parts, search and navigation

extern "C" {

int32_t sieda_repeat_sheet(SiedaProject* project, int32_t sheet, int32_t count) {
    if (!project) return -1;
    try {
        const int n = project->project.schematic.repeatSheet(sheet, count);
        if (n > 0) project->project.schematicChanged();
        return n;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_set_instance_refs(SiedaProject* project, int32_t sheet, const char* scheme) {
    if (!project || !scheme) return 0;
    return guarded([&] {
        InstanceRefs refs;
        if (!instanceRefsFromName(scheme, &refs)) return 0;
        return project->project.schematic.setInstanceRefs(sheet, refs) ? 1 : 0;
    });
}

int32_t sieda_set_sheet_channel(SiedaProject* project, int32_t sheet, const char* channel) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.setSheetChannel(sheet, str(channel)) ? 1 : 0; });
}

}  // extern "C"

// ---- graphical buses

namespace {
bool busScope(const char* scope, LabelScope* out) {
    if (!scope || !*scope) {
        *out = LabelScope::Local;
        return true;
    }
    return labelScopeFromName(scope, out) && *out != LabelScope::SheetEntry;
}
}  // namespace

extern "C" {

int32_t sieda_add_bus(SiedaProject* project, const char* name, const char* points_json) {
    if (!project || !points_json) return -1;
    try {
        std::vector<Vec2> points;
        const Json list = Json::parse(points_json);
        for (const auto& p : list.items()) points.push_back({p.get("x").asNumber(), p.get("y").asNumber()});
        return project->project.schematic.addBus(str(name), points);
    } catch (...) {
        return -1;
    }
}

int32_t sieda_remove_bus(SiedaProject* project, int32_t bus) {
    if (!project) return 0;
    return guarded([&] {
        const bool ok = project->project.schematic.removeBus(bus);
        if (ok) project->project.schematicChanged();
        return ok ? 1 : 0;
    });
}

int32_t sieda_rename_bus(SiedaProject* project, int32_t bus, const char* name) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.renameBus(bus, str(name)) ? 1 : 0; });
}

int32_t sieda_move_bus(SiedaProject* project, int32_t bus, double dx, double dy) {
    if (!project) return 0;
    return guarded([&] { return project->project.schematic.moveBus(bus, {dx, dy}) ? 1 : 0; });
}

int32_t sieda_rip_bus_entries(SiedaProject* project, int32_t bus, const char* members_json, const char* scope) {
    if (!project) return -1;
    try {
        LabelScope s;
        if (!busScope(scope, &s)) return -1;
        std::vector<std::string> members;
        if (members_json && *members_json)
        {
            const Json list = Json::parse(members_json);
            for (const auto& m : list.items()) members.push_back(m.asString(""));
        }
        const int n = project->project.schematic.ripBusEntries(bus, members, s);
        if (n > 0) project->project.schematicChanged();
        return n;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_connect_bus_to_part(SiedaProject* project, int32_t bus, int32_t component_id, const char* scope) {
    if (!project) return -1;
    try {
        LabelScope s;
        if (!busScope(scope, &s)) return -1;
        const int n = project->project.schematic.connectBusToPart(bus, component_id, s);
        if (n > 0) project->project.schematicChanged();
        return n;
    } catch (...) {
        return -1;
    }
}

}  // extern "C"

// ---- multi-unit parts

extern "C" {

int32_t sieda_add_custom_units(SiedaProject* project, const char* part_id, const char* value, double x, double y,
                               int32_t rotation, const char* ref) {
    if (!project || !part_id) return -1;
    try {
        const int id = project->project.schematic.addCustomUnits(part_id, str(value), {x, y}, rotation, str(ref));
        if (id >= 0) project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_add_part_unit(SiedaProject* project, int32_t component_id, int32_t unit, double x, double y, int32_t rotation) {
    if (!project) return -1;
    try {
        const int id = project->project.schematic.addPartUnit(component_id, unit, {x, y}, rotation);
        if (id >= 0) project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

int32_t sieda_place_next_unit(SiedaProject* project, int32_t component_id, double x, double y) {
    if (!project) return -1;
    try {
        const int id = project->project.schematic.placeNextUnit(component_id, {x, y});
        if (id >= 0) project->project.schematicChanged();
        return id;
    } catch (...) {
        return -1;
    }
}

}  // extern "C"

// ---- find / replace, net navigator, title block

namespace {
SearchOptions searchOptions(const Json& j) {
    SearchOptions o;
    o.matchCase = j.get("matchCase").asBool(false);
    o.wholeWord = j.get("wholeWord").asBool(false);
    if (j.has("fields")) {
        o.refs = o.values = o.labels = o.nets = o.pins = false;
        for (const auto& f : j.get("fields").items()) {
            const std::string name = f.asString("");
            if (name == "ref") o.refs = true;
            else if (name == "value") o.values = true;
            else if (name == "label") o.labels = true;
            else if (name == "net") o.nets = true;
            else if (name == "pin") o.pins = true;
        }
    }
    return o;
}
}  // namespace

extern "C" {

char* sieda_schematic_find(const SiedaProject* project, const char* request_json) {
    if (!project || !request_json) return nullptr;
    try {
        const Json req = Json::parse(request_json);
        Json hits = Json::array();
        for (const auto& h : findInSchematic(project->project.schematic, req.get("text").asString(""), searchOptions(req))) {
            Json j = Json::object();
            j["component"] = h.component;
            j["net"] = h.net;
            j["pin"] = h.pin;
            j["sheet"] = h.sheet;
            j["field"] = h.field;
            j["text"] = h.text;
            hits.push(j);
        }
        Json out = Json::object();
        out["hits"] = hits;
        return dup(out.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

int32_t sieda_schematic_replace(SiedaProject* project, const char* request_json) {
    if (!project || !request_json) return 0;
    try {
        const Json req = Json::parse(request_json);
        SearchOptions o = searchOptions(req);
        if (!req.has("fields")) o.refs = o.nets = o.pins = false;  // values and labels by default
        const int n = replaceInSchematic(project->project.schematic, req.get("text").asString(""),
                                         req.get("replacement").asString(""), o);
        if (n > 0) project->project.schematicChanged();
        return n;
    } catch (...) {
        return 0;
    }
}

char* sieda_net_places(const SiedaProject* project, int32_t net) {
    if (!project) return nullptr;
    try {
        const Schematic& sch = project->project.schematic;
        Json out = Json::object();
        out["net"] = net;
        out["name"] = net >= 0 && net < static_cast<int>(sch.nets().size()) ? sch.nets()[static_cast<size_t>(net)].name : std::string();
        Json places = Json::array();
        for (const auto& p : netPlaces(sch, net)) {
            Json j = Json::object();
            j["component"] = p.component;
            j["pin"] = p.pin;
            j["sheet"] = p.sheet;
            j["x"] = p.position.x;
            j["y"] = p.position.y;
            j["kind"] = p.kind;
            j["ref"] = p.ref;
            j["name"] = p.name;
            places.push(j);
        }
        out["places"] = places;
        return dup(out.dump());
    } catch (...) {
        return nullptr;
    }
}

int32_t sieda_set_title_block(SiedaProject* project, const char* json) {
    if (!project || !json) return 0;
    try {
        const Json j = Json::parse(json);
        if (!j.isObject()) return 0;
        TitleBlock& tb = project->project.titleBlock;
        auto set = [&](const char* key, std::string& field) {
            if (j.has(key)) field = j.get(key).asString("").substr(0, 256);
        };
        set("title", tb.title);
        set("company", tb.company);
        set("revision", tb.revision);
        set("date", tb.date);
        set("drawnBy", tb.drawnBy);
        return 1;
    } catch (...) {
        return 0;
    }
}

}  // extern "C"

// ---- channel analysis: lossy lines, S-parameters, Touchstone, eye ------------------------------------------------------

namespace {
Json parseOptions(const char* json) {
    if (!json || !*json) return Json::object();
    try {
        Json j = Json::parse(json);
        return j.isObject() ? j : Json::object();
    } catch (...) {
        return Json::object();
    }
}

LossOptions lossOptions(const Json& j, const SiSettings& si) {
    LossOptions o;
    o.foil = j.get("foil").asString(si.copperFoil);
    o.roughness = roughnessFromString(j.get("roughness").asString("huray"));
    o.lossless = j.get("lossless").asBool(false);
    return o;
}
}  // namespace

extern "C" {

char* sieda_si_line_loss_json(const SiedaProject* project, const char* options_json) {
    if (!project) return nullptr;
    try {
        const Json o = parseOptions(options_json);
        return dup(lineLossJson(project->project.pcb.settings, lossOptions(o, project->project.si),
                                std::clamp(o.get("width").asNumber(0), 0.0, 20.0), o.get("fMax").asNumber(20e9))
                       .dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

int32_t sieda_si_set_copper_foil(SiedaProject* project, const char* foil) {
    if (!project) return 0;
    const std::string id = str(foil);
    if (!id.empty() && std::none_of(copperFoils().begin(), copperFoils().end(), [&](const CopperFoil& f) { return f.id == id; }))
        return 0;
    project->project.si.copperFoil = id;
    return 1;
}

int32_t sieda_si_set_channel(SiedaProject* project, const char* net_name, double bit_rate, double mask_height,
                             double mask_width_ui) {
    if (!project || !net_name || !*net_name || !(bit_rate >= 0) || !(mask_height >= 0) || !(mask_width_ui >= 0)) return 0;
    auto& cs = project->project.si.channels;
    auto it = std::find_if(cs.begin(), cs.end(), [&](const SiSettings::ChannelSpec& c) { return c.net == net_name; });
    if (bit_rate == 0) {
        if (it != cs.end()) cs.erase(it);
        return 1;
    }
    SiSettings::ChannelSpec c;
    c.net = net_name;
    c.bitRate = std::min(bit_rate, 200e9);
    c.maskHeight = std::min(mask_height, 100.0);
    c.maskWidthUi = std::min(mask_width_ui, 0.99);
    if (it != cs.end()) *it = c;
    else cs.push_back(c);
    return 1;
}

char* sieda_si_channel_json(const SiedaProject* project, const char* options_json) {
    if (!project) return nullptr;
    try {
        const Json o = parseOptions(options_json);
        ChannelOptions co = channelOptionsFromJson(o);
        co.loss = lossOptions(o, project->project.si);
        const ChannelDrive drive = channelDriveFromJson(o);
        const bool wantEye = o.get("eye").isObject();
        const EyeOptions eye = wantEye ? eyeOptionsFromJson(o.get("eye")) : EyeOptions();
        std::unique_ptr<TouchstoneData> cascade;
        const std::string ts = o.get("touchstone").asString("");
        Json extraNote;
        if (!ts.empty()) {
            try {
                TouchstoneData t = parseTouchstone(ts, std::clamp(o.get("touchstonePorts").asInt(0), 0, 64));
                t.sp = reorderFourPort(t.sp, o.get("portOrder").asString("13"));
                cascade = std::make_unique<TouchstoneData>(std::move(t));
            } catch (const std::exception& e) {
                extraNote = Json(std::string(e.what()));
            }
        }
        Json j = channelJson(project->project, co, drive, wantEye ? &eye : nullptr, cascade.get());
        if (extraNote.isString() && j.get("notes").isArray()) j["notes"].push(extraNote);
        return dup(j.dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_si_channel_touchstone(const SiedaProject* project, const char* options_json, char** error_out) {
    if (error_out) *error_out = nullptr;
    if (!project) return nullptr;
    try {
        const Json o = parseOptions(options_json);
        ChannelOptions co = channelOptionsFromJson(o);
        co.loss = lossOptions(o, project->project.si);
        std::string err;
        const std::string text = channelTouchstone(project->project, co, &err);
        if (text.empty()) {
            if (error_out) *error_out = dup(err.empty() ? std::string("No channel") : err);
            return nullptr;
        }
        return dup(text);
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_touchstone_parse(const char* text, int32_t ports_hint, const char* port_order, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        const TouchstoneData t = parseTouchstone(str(text), std::clamp(ports_hint, 0, 64));
        return dup(touchstoneJson(t, port_order && *port_order ? port_order : "13").dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

char* sieda_touchstone_channel_json(const char* text, int32_t ports_hint, const char* options_json, char** error_out) {
    if (error_out) *error_out = nullptr;
    try {
        const Json o = parseOptions(options_json);
        const TouchstoneData t = parseTouchstone(str(text), std::clamp(ports_hint, 0, 64));
        ChannelDrive drive = channelDriveFromJson(o);
        drive.idealDriver = true;
        const bool wantEye = o.get("eye").isObject();
        const EyeOptions eye = wantEye ? eyeOptionsFromJson(o.get("eye")) : EyeOptions();
        return dup(touchstoneChannelJson(t, o.get("portOrder").asString("13"), drive, wantEye ? &eye : nullptr).dump());
    } catch (const std::exception& e) {
        if (error_out) *error_out = dup(e.what());
        return nullptr;
    }
}

}  // extern "C"

// ---- power-integrity planning: regulator, plane cavity, decoupling plan, IR-drop map --------------------------------------

extern "C" {

int32_t sieda_pi_set_vrm(SiedaProject* project, const char* net_name, double r_out, double loop_bandwidth) {
    if (!project || !net_name || !*net_name || !(r_out >= 0) || !(loop_bandwidth >= 0) || r_out > 10 || loop_bandwidth > 100e6)
        return 0;
    auto& rails = project->project.si.rails;
    auto it = std::find_if(rails.begin(), rails.end(), [&](const PdnRailSettings& r) { return r.net == net_name; });
    if (it == rails.end()) {
        if (r_out == 0 && loop_bandwidth == 0) return 1;
        PdnRailSettings r;
        r.net = net_name;
        rails.push_back(r);
        it = rails.end() - 1;
    }
    it->vrmR = r_out;
    it->vrmBandwidth = loop_bandwidth;
    if (it->ripplePercent == 0 && it->transientCurrent == 0 && it->dcCurrent == 0 && r_out == 0 && loop_bandwidth == 0) rails.erase(it);
    return 1;
}

char* sieda_pi_cavity_json(const SiedaProject* project, const char* net_name) {
    if (!project) return nullptr;
    try {
        return dup(pdnCavityJson(project->project, str(net_name)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_pi_decap_plan_json(const SiedaProject* project, const char* net_name) {
    if (!project) return nullptr;
    try {
        return dup(pdnDecapPlanJson(project->project, str(net_name)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_pi_ir_map_json(const SiedaProject* project, const char* net_name) {
    if (!project) return nullptr;
    try {
        return dup(pdnIrMapJson(project->project, str(net_name)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

}  // extern "C"
