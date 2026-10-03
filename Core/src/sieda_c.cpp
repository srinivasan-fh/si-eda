// SiEDA Core — C ABI implementation. Every entry point is exception-safe.
#include "sieda/sieda_c.h"

#include "sieda/Aerospace.hpp"
#include "sieda/Naval.hpp"
#include "sieda/Medical.hpp"
#include "sieda/Retail.hpp"
#include "sieda/Appliance.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Avr.hpp"
#include "sieda/Firmware.hpp"

#include <algorithm>
#include <cctype>
#include <map>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

#include "sieda/Bom.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Industry.hpp"
#include "sieda/LengthMatch.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Project.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/StandardParts.hpp"
#include "sieda/Units.hpp"
#include "sieda/Validation.hpp"
#include "sieda/Verification.hpp"

struct SiedaProject {
    sieda::Project project;
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
        auto* live = new SiedaLiveSim(project->project.schematic);
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
        Simulator sim(project->project.schematic);
        return dup(project->project.dcToJson(sim.dcOperatingPoint()).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_simulate_transient(const SiedaProject* project, double t_stop, double t_step) {
    if (!project) return nullptr;
    try {
        Simulator sim(project->project.schematic);
        return dup(project->project.transientToJson(sim.transient(t_stop, t_step)).dump());
    } catch (const std::exception& e) {
        return errorJson(e);
    }
}

char* sieda_spice_netlist(const SiedaProject* project) {
    if (!project) return nullptr;
    try {
        return dup(exportSpiceNetlist(project->project.schematic, project->project.name));
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
        if (f == "bom") return dup(exportBomCsv(p.schematic));
        if (f == "pnp") return dup(exportPickAndPlaceCsv(p.schematic));
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
        if (f == "bom_assembly") return dup(exportAssemblyBomCsv(p.schematic));
        if (f == "cpl") return dup(exportCplCsv(p.schematic, p.pcb));
        if (f == "assembly_top") return dup(exportAssemblySvg(p.schematic, p.pcb, false, p.name));
        if (f == "assembly_bottom") return dup(exportAssemblySvg(p.schematic, p.pcb, true, p.name));
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
        return dup(bomJson(project->project.schematic, project->project.buildQuantity).dump());
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
const uint32_t* sieda_mesh_indices(const SiedaMesh* m) { return m ? m->mesh.indices.data() : nullptr; }

}  // extern "C"
