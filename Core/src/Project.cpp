#include "sieda/Project.hpp"

#include <algorithm>
#include <map>

namespace sieda {

namespace {
Json vec(Vec2 p) {
    Json j = Json::object();
    j["x"] = p.x;
    j["y"] = p.y;
    return j;
}
Json pinRef(PinRef p) {
    Json j = Json::object();
    j["component"] = p.component;
    j["pin"] = p.pin;
    return j;
}
PinRef pinRefFrom(const Json& j) { return {j.get("component").asInt(-1), j.get("pin").asInt(-1)}; }

Json boardJson(const BoardSettings& s) {
    Json b = Json::object();
    b["width"] = s.width;
    b["height"] = s.height;
    b["thickness"] = s.thickness;
    b["trackWidth"] = s.trackWidth;
    b["clearance"] = s.clearance;
    b["viaDrill"] = s.viaDrill;
    b["viaDiameter"] = s.viaDiameter;
    b["edgeClearance"] = s.edgeClearance;
    b["routingGrid"] = s.routingGrid;
    b["layerCount"] = s.layerCount;
    return b;
}
}  // namespace

void Project::schematicChanged() { pcb.pruneStaleRouting(schematic); }

std::string Project::addCustomPart(const CustomPartSpec& spec) {
    auto part = CustomPartRegistry::instance().registerPart(spec);
    if (std::find(customLibrary.begin(), customLibrary.end(), part->id) == customLibrary.end())
        customLibrary.push_back(part->id);
    return part->id;
}

bool Project::removeCustomPart(const std::string& id) {
    for (const auto& c : schematic.components())
        if (c.kind == ComponentKind::Custom && c.customPart == id) return false;
    auto it = std::find(customLibrary.begin(), customLibrary.end(), id);
    if (it == customLibrary.end()) return false;
    customLibrary.erase(it);
    return true;
}

Json Project::toJson() const {
    Json root = Json::object();
    root["format"] = "sieda-project";
    root["version"] = 1;
    root["name"] = name;
    root["requirements"] = requirements;
    root["board"] = boardJson(pcb.settings);

    Json library = Json::array();
    for (const auto& id : customLibrary)
        if (const CustomPart* part = CustomPartRegistry::instance().find(id)) {
            Json pj = Json::object();
            pj["id"] = id;
            pj["spec"] = customPartSpecToJson(part->spec);
            library.push(pj);
        }
    root["customParts"] = library;

    Json comps = Json::array();
    for (const auto& c : schematic.components()) {
        Json j = Json::object();
        j["id"] = c.id;
        j["kind"] = static_cast<int>(c.kind);
        j["ref"] = c.ref;
        j["value"] = c.value;
        j["x"] = c.position.x;
        j["y"] = c.position.y;
        j["rotation"] = c.rotation;
        if (c.kind == ComponentKind::Custom) j["customPart"] = c.customPart;
        Json p = Json::object();
        p["x"] = c.pcb.position.x;
        p["y"] = c.pcb.position.y;
        p["rotation"] = c.pcb.rotation;
        p["bottom"] = c.pcb.bottom;
        p["placed"] = c.pcb.placed;
        j["pcb"] = p;
        comps.push(j);
    }
    root["components"] = comps;

    Json wires = Json::array();
    for (const auto& w : schematic.wires()) {
        Json j = Json::object();
        j["id"] = w.id;
        j["a"] = pinRef(w.a);
        j["b"] = pinRef(w.b);
        wires.push(j);
    }
    root["wires"] = wires;

    Json tracks = Json::array();
    for (const auto& t : pcb.tracks) {
        Json j = Json::object();
        j["layer"] = static_cast<int>(t.layer);
        j["width"] = t.width;
        j["a"] = vec(t.a);
        j["b"] = vec(t.b);
        tracks.push(j);
    }
    root["tracks"] = tracks;

    Json vias = Json::array();
    for (const auto& v : pcb.vias) {
        Json j = vec(v.position);
        j["drill"] = v.drill;
        j["diameter"] = v.diameter;
        vias.push(j);
    }
    root["vias"] = vias;
    return root;
}

Project Project::fromJson(const Json& root) {
    if (root.get("format").asString("") != "sieda-project") throw JsonError("Not a SiEDA project file");
    Project p;
    p.name = root.get("name").asString("Untitled");
    p.requirements = root.get("requirements").asString("");
    const Json& b = root.get("board");
    BoardSettings& s = p.pcb.settings;
    s.width = b.get("width").asNumber(s.width);
    s.height = b.get("height").asNumber(s.height);
    s.thickness = b.get("thickness").asNumber(s.thickness);
    s.trackWidth = b.get("trackWidth").asNumber(s.trackWidth);
    s.clearance = b.get("clearance").asNumber(s.clearance);
    s.viaDrill = b.get("viaDrill").asNumber(s.viaDrill);
    s.viaDiameter = b.get("viaDiameter").asNumber(s.viaDiameter);
    s.edgeClearance = b.get("edgeClearance").asNumber(s.edgeClearance);
    s.routingGrid = std::max(0.1, b.get("routingGrid").asNumber(s.routingGrid));
    s.layerCount = BoardSettings::normalizeLayerCount(b.get("layerCount").asInt(2));

    // Custom parts first so components can resolve them; ids are re-derived and remapped if they changed.
    std::map<std::string, std::string> idMap;
    for (const auto& j : root.get("customParts").items()) {
        std::string newId = p.addCustomPart(customPartSpecFromJson(j.get("spec")));
        idMap[j.get("id").asString(newId)] = newId;
    }

    for (const auto& j : root.get("components").items()) {
        int kind = j.get("kind").asInt(-1);
        if (!Library::isValidKind(kind)) throw JsonError("Unknown component kind " + std::to_string(kind));
        Component c;
        c.id = j.get("id").asInt(-1);
        if (c.id < 0) throw JsonError("Component without id");
        c.kind = static_cast<ComponentKind>(kind);
        c.ref = j.get("ref").asString("");
        c.value = j.get("value").asString(c.def().defaultValue);
        c.position = {j.get("x").asNumber(), j.get("y").asNumber()};
        c.rotation = j.get("rotation").asInt(0);
        if (c.kind == ComponentKind::Custom) {
            std::string id = j.get("customPart").asString("");
            auto it = idMap.find(id);
            c.customPart = it != idMap.end() ? it->second : id;
        }
        const Json& pc = j.get("pcb");
        c.pcb.position = {pc.get("x").asNumber(), pc.get("y").asNumber()};
        c.pcb.rotation = pc.get("rotation").asInt(0);
        c.pcb.bottom = pc.get("bottom").asBool(false);
        c.pcb.placed = pc.get("placed").asBool(false);
        p.schematic.restoreComponent(c);
    }
    for (const auto& j : root.get("wires").items()) {
        Wire w;
        w.id = j.get("id").asInt(-1);
        w.a = pinRefFrom(j.get("a"));
        w.b = pinRefFrom(j.get("b"));
        if (w.id < 0 || !p.schematic.find(w.a.component) || !p.schematic.find(w.b.component)) continue;
        p.schematic.restoreWire(w);
    }
    for (const auto& j : root.get("tracks").items()) {
        Track t;
        t.layer = std::clamp(j.get("layer").asInt(0), 0, s.bottomLayer());
        t.width = j.get("width").asNumber(s.trackWidth);
        t.a = {j.get("a").get("x").asNumber(), j.get("a").get("y").asNumber()};
        t.b = {j.get("b").get("x").asNumber(), j.get("b").get("y").asNumber()};
        p.pcb.addTrack(t);
    }
    for (const auto& j : root.get("vias").items()) {
        Via v;
        v.position = {j.get("x").asNumber(), j.get("y").asNumber()};
        v.drill = j.get("drill").asNumber(s.viaDrill);
        v.diameter = j.get("diameter").asNumber(s.viaDiameter);
        p.pcb.addVia(v);
    }
    p.schematicChanged();  // assigns nets to copper from pad contact
    return p;
}

Json Project::snapshot() const {
    Json root = Json::object();
    root["name"] = name;
    root["requirements"] = requirements;
    const auto& nets = schematic.nets();

    Json comps = Json::array();
    for (const auto& c : schematic.components()) {
        Json j = Json::object();
        j["id"] = c.id;
        j["kind"] = static_cast<int>(c.kind);
        j["ref"] = c.ref;
        j["value"] = c.value;
        j["x"] = c.position.x;
        j["y"] = c.position.y;
        j["rotation"] = c.rotation;
        j["footprint"] = c.def().footprint;
        if (c.kind == ComponentKind::Custom) j["customPart"] = c.customPart;
        Json pins = Json::array();
        for (size_t i = 0; i < c.def().pins.size(); ++i) {
            PinRef r{c.id, static_cast<int>(i)};
            Vec2 pos = schematic.pinPosition(r);
            int net = schematic.netOf(r);
            Json pj = Json::object();
            pj["name"] = c.def().pins[i].name;
            pj["x"] = pos.x;
            pj["y"] = pos.y;
            pj["net"] = net;
            pj["connected"] = net >= 0 && nets[static_cast<size_t>(net)].pins.size() > 1;
            pins.push(pj);
        }
        j["pins"] = pins;
        Json pc = Json::object();
        pc["x"] = c.pcb.position.x;
        pc["y"] = c.pcb.position.y;
        pc["rotation"] = c.pcb.rotation;
        pc["bottom"] = c.pcb.bottom;
        pc["placed"] = c.pcb.placed;
        j["pcb"] = pc;
        comps.push(j);
    }
    root["components"] = comps;

    Json wires = Json::array();
    for (const auto& w : schematic.wires()) {
        Json j = Json::object();
        j["id"] = w.id;
        j["a"] = pinRef(w.a);
        j["b"] = pinRef(w.b);
        Vec2 pa = schematic.pinPosition(w.a), pb = schematic.pinPosition(w.b);
        j["ax"] = pa.x;
        j["ay"] = pa.y;
        j["bx"] = pb.x;
        j["by"] = pb.y;
        j["net"] = schematic.netOf(w.a);
        wires.push(j);
    }
    root["wires"] = wires;

    Json netArr = Json::array();
    for (const auto& n : nets) {
        Json j = Json::object();
        j["index"] = n.index;
        j["name"] = n.name;
        j["pinCount"] = n.pins.size();
        j["ground"] = n.isGround;
        netArr.push(j);
    }
    root["nets"] = netArr;
    root["board"] = boardJson(pcb.settings);

    Json pads = Json::array();
    for (const auto& p : pcb.pads(schematic)) {
        Json j = Json::object();
        j["component"] = p.componentId;
        j["pin"] = p.pinIndex;
        j["number"] = p.padNumber;
        j["net"] = p.net;
        j["x"] = p.position.x;
        j["y"] = p.position.y;
        j["w"] = p.size.x;
        j["h"] = p.size.y;
        j["throughHole"] = p.throughHole;
        j["round"] = p.round;
        j["drill"] = p.drill;
        j["bottom"] = p.bottom;
        pads.push(j);
    }
    root["pads"] = pads;

    Json tracks = Json::array();
    for (const auto& t : pcb.tracks) {
        Json j = Json::object();
        j["id"] = t.id;
        j["net"] = t.net;
        j["layer"] = static_cast<int>(t.layer);
        j["width"] = t.width;
        j["ax"] = t.a.x;
        j["ay"] = t.a.y;
        j["bx"] = t.b.x;
        j["by"] = t.b.y;
        tracks.push(j);
    }
    root["tracks"] = tracks;

    Json vias = Json::array();
    for (const auto& v : pcb.vias) {
        Json j = Json::object();
        j["id"] = v.id;
        j["net"] = v.net;
        j["x"] = v.position.x;
        j["y"] = v.position.y;
        j["drill"] = v.drill;
        j["diameter"] = v.diameter;
        vias.push(j);
    }
    root["vias"] = vias;

    Json rats = Json::array();
    for (const auto& [a, b] : pcb.ratsnest(schematic)) {
        Json j = Json::object();
        j["ax"] = a.x;
        j["ay"] = a.y;
        j["bx"] = b.x;
        j["by"] = b.y;
        rats.push(j);
    }
    root["ratsnest"] = rats;

    Json courtyards = Json::array();
    for (const auto& c : schematic.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        Rect r = pcb.courtyard(c);
        Json j = Json::object();
        j["component"] = c.id;
        j["x0"] = r.x0;
        j["y0"] = r.y0;
        j["x1"] = r.x1;
        j["y1"] = r.y1;
        courtyards.push(j);
    }
    root["courtyards"] = courtyards;

    // Component bodies (millimetres, rotated) for 3D wireframe / X-ray rendering.
    Json bodies = Json::array();
    for (const auto& c : schematic.components()) {
        if (!c.hasFootprint() || !c.pcb.placed) continue;
        const FootprintDef* fp = Library::instance().footprint(c.def().footprint);
        if (!fp) continue;
        double w = fp->body.width, d = fp->body.depth;
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) std::swap(w, d);
        Json j = Json::object();
        j["component"] = c.id;
        j["x"] = c.pcb.position.x;
        j["y"] = c.pcb.position.y;
        j["w"] = w;
        j["d"] = d;
        j["h"] = fp->body.height;
        j["bottom"] = c.pcb.bottom;
        j["package"] = fp->label.empty() ? fp->name : fp->label;
        bodies.push(j);
    }
    root["bodies"] = bodies;

    Json parts = Json::array();
    for (const auto& id : customLibrary)
        if (const CustomPart* part = CustomPartRegistry::instance().find(id)) parts.push(customPartToJson(*part));
    root["customParts"] = parts;
    return root;
}

Json Project::violationsToJson(const std::vector<RuleViolation>& list) {
    Json arr = Json::array();
    for (const auto& v : list) {
        Json j = Json::object();
        j["severity"] = severityName(v.severity);
        j["code"] = v.code;
        j["message"] = v.message;
        Json comps = Json::array();
        for (int id : v.components) comps.push(id);
        j["components"] = comps;
        j["hasLocation"] = v.hasLocation;
        j["x"] = v.location.x;
        j["y"] = v.location.y;
        arr.push(j);
    }
    return arr;
}

Json Project::dcToJson(const DcResult& r) const {
    Json root = Json::object();
    root["converged"] = r.converged;
    root["error"] = r.error;
    root["iterations"] = r.iterations;
    Json nets = Json::array();
    const auto& sn = schematic.nets();
    for (size_t i = 0; i < r.netVoltages.size() && i < sn.size(); ++i) {
        Json j = Json::object();
        j["index"] = static_cast<int>(i);
        j["name"] = sn[i].name;
        j["voltage"] = r.netVoltages[i];
        nets.push(j);
    }
    root["nets"] = nets;
    Json devs = Json::array();
    for (const auto& d : r.devices) {
        const Component* c = schematic.find(d.componentId);
        Json j = Json::object();
        j["component"] = d.componentId;
        j["ref"] = c ? c->ref : std::string();
        j["current"] = d.current;
        j["power"] = d.power;
        j["voltage"] = d.voltage;
        devs.push(j);
    }
    root["devices"] = devs;
    return root;
}

Json Project::transientToJson(const TransientResult& r, size_t maxPoints) const {
    Json root = Json::object();
    root["ok"] = r.ok;
    root["error"] = r.error;
    size_t n = r.time.size();
    size_t stride = maxPoints > 0 && n > maxPoints ? (n + maxPoints - 1) / maxPoints : 1;
    auto decimate = [&](const std::vector<double>& v) {
        Json a = Json::array();
        for (size_t i = 0; i < v.size(); i += stride) a.push(v[i]);
        if (!v.empty() && (v.size() - 1) % stride != 0) a.push(v.back());
        return a;
    };
    root["time"] = decimate(r.time);
    Json nets = Json::array();
    const auto& sn = schematic.nets();
    for (size_t i = 0; i < r.netVoltages.size() && i < sn.size(); ++i) {
        if (sn[i].isGround || sn[i].pins.size() < 2) continue;
        Json j = Json::object();
        j["index"] = static_cast<int>(i);
        j["name"] = sn[i].name;
        j["values"] = decimate(r.netVoltages[i]);
        nets.push(j);
    }
    root["nets"] = nets;
    Json currents = Json::array();
    for (const auto& [id, values] : r.currents) {
        const Component* c = schematic.find(id);
        Json j = Json::object();
        j["component"] = id;
        j["ref"] = c ? c->ref : std::string();
        j["values"] = decimate(values);
        currents.push(j);
    }
    root["currents"] = currents;
    return root;
}

Json Project::libraryJson() {
    Json arr = Json::array();
    for (const auto& d : Library::instance().components()) {
        Json j = Json::object();
        j["kind"] = static_cast<int>(d.kind);
        j["name"] = d.name;
        j["refPrefix"] = d.refPrefix;
        j["defaultValue"] = d.defaultValue;
        j["unit"] = d.unit;
        j["footprint"] = d.footprint;
        j["simulated"] = d.simulated;
        Json pins = Json::array();
        for (const auto& p : d.pins) {
            Json pj = Json::object();
            pj["name"] = p.name;
            pj["x"] = p.offset.x;
            pj["y"] = p.offset.y;
            pins.push(pj);
        }
        j["pins"] = pins;
        arr.push(j);
    }
    return arr;
}

}  // namespace sieda
