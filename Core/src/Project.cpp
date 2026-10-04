#include "sieda/Project.hpp"

#include "sieda/Aerospace.hpp"
#include "sieda/Naval.hpp"
#include "sieda/Medical.hpp"
#include "sieda/Retail.hpp"
#include "sieda/Appliance.hpp"
#include "sieda/Automotive.hpp"
#include "sieda/Avr.hpp"
#include "sieda/Embedded.hpp"
#include "sieda/Industry.hpp"
#include "sieda/Mesh.hpp"
#include "sieda/Robotics.hpp"
#include "sieda/Stackup.hpp"

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

Json sourcingJson(const Sourcing& s) {
    Json j = Json::object();
    if (!s.manufacturer.empty()) j["manufacturer"] = s.manufacturer;
    if (!s.mpn.empty()) j["mpn"] = s.mpn;
    if (!s.supplierPart.empty()) j["supplierPart"] = s.supplierPart;
    if (s.unitPrice > 0) j["unitPrice"] = s.unitPrice;
    if (s.dnp) j["dnp"] = true;
    return j;
}

Sourcing sourcingFrom(const Json& j) {
    Sourcing s;
    if (!j.isObject()) return s;
    s.manufacturer = j.get("manufacturer").asString("");
    s.mpn = j.get("mpn").asString("");
    s.supplierPart = j.get("supplierPart").asString("");
    s.unitPrice = std::max(0.0, j.get("unitPrice").asNumber(0));
    s.dnp = j.get("dnp").asBool(false);
    return s;
}

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
    b["rulePreset"] = s.rulePreset;
    b["minTrackWidth"] = s.minTrackWidth;
    b["minClearance"] = s.minClearance;
    b["minDrill"] = s.minDrill;
    b["minAnnularRing"] = s.minAnnularRing;
    b["minHoleToHole"] = s.minHoleToHole;
    b["copperWeightOz"] = s.copperWeightOz;
    b["maxTempRise"] = s.maxTempRise;
    b["highAltitude"] = s.highAltitude;
    b["coating"] = s.coating;
    b["underfill"] = s.underfill;
    b["isolationGap"] = s.isolationGap;
    b["material"] = s.material;
    b["construction"] = s.construction;
    b["singleEndedImpedance"] = s.singleEndedImpedance;
    b["differentialImpedance"] = s.differentialImpedance;
    b["backdrill"] = s.backdrill;
    b["lengthTuning"] = s.lengthTuning;
    b["hdi"] = s.hdi;
    b["microviaDrill"] = s.microviaDrill;
    b["microviaDiameter"] = s.microviaDiameter;
    b["viaInPad"] = s.viaInPad;
    b["pairSkewTolerance"] = s.pairSkewTolerance;
    b["busLengthTolerance"] = s.busLengthTolerance;
    b["solderMask"] = s.solderMask;
    Json widths = Json::object();
    for (const auto& [net, w] : s.netWidths) widths[net] = w;
    b["netWidths"] = widths;
    b["autoSizeNets"] = s.autoSizeNets;
    Json outline = Json::array();
    for (const auto& v : s.outline) outline.push(vec(v));
    b["outline"] = outline;
    Json holes = Json::array();
    for (const auto& h : s.holes) {
        Json j = vec(h.position);
        j["drill"] = h.drill;
        j["keepout"] = h.keepout;
        holes.push(j);
    }
    b["holes"] = holes;
    return b;
}

Json tamperMeshesJson(const std::vector<TamperMesh>& meshes) {
    Json arr = Json::array();
    for (const auto& m : meshes) {
        Json j = Json::object();
        j["component"] = m.componentRef;
        j["netA"] = m.netA;
        j["netB"] = m.netB;
        j["layerA"] = m.layerA;
        j["layerB"] = m.layerB;
        j["margin"] = m.margin;
        arr.push(j);
    }
    return arr;
}

Json zonesJson(const std::vector<CopperZone>& zones) {
    Json arr = Json::array();
    for (const auto& z : zones) {
        Json j = Json::object();
        j["net"] = z.net;
        j["layer"] = z.layer;
        j["plane"] = z.plane;
        j["clearance"] = z.clearance;
        arr.push(j);
    }
    return arr;
}
}  // namespace

bool Project::applyIndustry(const std::string& id) {
    const IndustryProfile* profile = findIndustry(id);
    if (!profile) return false;
    industry = profile->id;
    pcb.settings.applyPreset(profile->rulePreset);
    pcb.settings.highAltitude = profile->highAltitude;
    return true;
}

PartRatings Project::partRatings() const {
    const IndustryProfile* profile = findIndustry(industry);
    return profile ? deratedRatings(*profile) : PartRatings{};
}

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
    root["industry"] = industry;
    root["robotPlatform"] = robotPlatform;
    root["ecuType"] = ecuType;
    root["aerospaceMission"] = aerospaceMission;
    root["navalPlatform"] = navalPlatform;
    root["medicalClass"] = medicalClass;
    root["retailDevice"] = retailDevice;
    root["applianceType"] = applianceType;
    root["buildQuantity"] = buildQuantity;
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
        if (!c.firmware.empty()) {
            j["firmware"] = c.firmware;
            j["firmwareName"] = c.firmwareName;
            if (c.clockHz > 0) j["clockHz"] = c.clockHz;
        }
        if (!c.noConnect.empty()) {
            Json nc = Json::array();
            for (int pin : c.noConnect) nc.push(pin);
            j["noConnect"] = nc;
        }
        if (!c.sourcing.empty()) j["sourcing"] = sourcingJson(c.sourcing);
        if (!c.package.empty()) j["package"] = c.package;
        Json p = Json::object();
        p["x"] = c.pcb.position.x;
        p["y"] = c.pcb.position.y;
        p["rotation"] = c.pcb.rotation;
        p["bottom"] = c.pcb.bottom;
        p["placed"] = c.pcb.placed;
        if (c.pcb.locked) p["locked"] = true;
        if (c.pcb.embedded()) p["embeddedLayer"] = c.pcb.embeddedLayer;
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
        if (!v.isThrough()) {
            j["fromLayer"] = v.fromLayer;
            j["toLayer"] = v.toLayer;
        }
        vias.push(j);
    }
    root["vias"] = vias;
    root["zones"] = zonesJson(pcb.zones);
    root["tamperMeshes"] = tamperMeshesJson(pcb.tamperMeshes);
    return root;
}

Project Project::fromJson(const Json& root) {
    if (root.get("format").asString("") != "sieda-project") throw JsonError("Not a SiEDA project file");
    Project p;
    p.name = root.get("name").asString("Untitled");
    p.requirements = root.get("requirements").asString("");
    p.industry = root.get("industry").asString("general");
    p.robotPlatform = root.get("robotPlatform").asString("");
    if (!p.robotPlatform.empty() && !findRobotPlatform(p.robotPlatform)) p.robotPlatform.clear();
    p.ecuType = root.get("ecuType").asString("");
    if (!p.ecuType.empty() && !findEcuType(p.ecuType)) p.ecuType.clear();
    p.aerospaceMission = root.get("aerospaceMission").asString("");
    if (!p.aerospaceMission.empty() && !findAerospaceMission(p.aerospaceMission)) p.aerospaceMission.clear();
    p.navalPlatform = root.get("navalPlatform").asString("");
    if (!p.navalPlatform.empty() && !findNavalPlatform(p.navalPlatform)) p.navalPlatform.clear();
    p.medicalClass = root.get("medicalClass").asString("");
    if (!p.medicalClass.empty() && !findMedicalClass(p.medicalClass)) p.medicalClass.clear();
    p.retailDevice = root.get("retailDevice").asString("");
    if (!p.retailDevice.empty() && !findRetailDevice(p.retailDevice)) p.retailDevice.clear();
    p.applianceType = root.get("applianceType").asString("");
    if (!p.applianceType.empty() && !findApplianceType(p.applianceType)) p.applianceType.clear();
    p.buildQuantity = std::max(1, root.get("buildQuantity").asInt(5));
    if (!findIndustry(p.industry)) p.industry = "general";
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
    s.rulePreset = b.get("rulePreset").asString(s.rulePreset);
    s.minTrackWidth = b.get("minTrackWidth").asNumber(s.minTrackWidth);
    s.minClearance = b.get("minClearance").asNumber(s.minClearance);
    s.minDrill = b.get("minDrill").asNumber(s.minDrill);
    s.minAnnularRing = b.get("minAnnularRing").asNumber(s.minAnnularRing);
    s.minHoleToHole = b.get("minHoleToHole").asNumber(s.minHoleToHole);
    s.copperWeightOz = std::max(0.5, b.get("copperWeightOz").asNumber(s.copperWeightOz));
    s.highAltitude = b.get("highAltitude").asBool(false);
    s.coating = b.get("coating").asString("none");
    s.underfill = b.get("underfill").asBool(false);
    s.isolationGap = std::clamp(b.get("isolationGap").asNumber(0), 0.0, 25.0);
    if (std::find(conformalCoatings().begin(), conformalCoatings().end(), s.coating) == conformalCoatings().end())
        s.coating = "none";
    s.material = b.get("material").asString("fr4");
    if (!findLaminate(s.material)) s.material = "fr4";
    s.construction = b.get("construction").asString("rigid");
    if (s.construction != "rigid-flex" && s.construction != "metal-core") s.construction = "rigid";
    s.singleEndedImpedance = std::clamp(b.get("singleEndedImpedance").asNumber(50), 20.0, 150.0);
    s.differentialImpedance = std::clamp(b.get("differentialImpedance").asNumber(100), 50.0, 200.0);
    s.backdrill = b.get("backdrill").asBool(false);
    s.lengthTuning = b.get("lengthTuning").asBool(true);
    s.hdi = b.get("hdi").asBool(false);
    s.microviaDrill = std::clamp(b.get("microviaDrill").asNumber(0.1), 0.05, 0.15);
    s.microviaDiameter = std::clamp(b.get("microviaDiameter").asNumber(0.25), 0.15, 0.5);
    s.viaInPad = b.get("viaInPad").asBool(false);
    s.pairSkewTolerance = std::clamp(b.get("pairSkewTolerance").asNumber(0.13), 0.02, 5.0);
    s.busLengthTolerance = std::clamp(b.get("busLengthTolerance").asNumber(0.5), 0.02, 20.0);
    s.solderMask = b.get("solderMask").asString("green");
    if (!findSolderMask(s.solderMask)) s.solderMask = "green";
    const Json& widths = b.get("netWidths");
    if (widths.isObject())
        for (const auto& [net, w] : widths.fields())
            if (w.asNumber(0) > 0) s.netWidths[net] = w.asNumber(0);
    s.maxTempRise = std::max(1.0, b.get("maxTempRise").asNumber(s.maxTempRise));
    s.autoSizeNets = b.get("autoSizeNets").asBool(true);
    {
        std::vector<Vec2> outline;
        for (const auto& v : b.get("outline").items()) outline.push_back({v.get("x").asNumber(), v.get("y").asNumber()});
        if (outline.size() >= 3) {
            double w = s.width, h = s.height;
            s.setOutline(outline);
            s.width = std::max(s.width, w);  // keep the stored size (the outline may not touch every bound)
            s.height = std::max(s.height, h);
        }
        for (const auto& j : b.get("holes").items()) {
            MountingHole m;
            m.position = {j.get("x").asNumber(), j.get("y").asNumber()};
            m.drill = std::max(0.1, j.get("drill").asNumber(m.drill));
            m.keepout = std::max(m.drill, j.get("keepout").asNumber(m.keepout));
            s.holes.push_back(m);
        }
    }
    for (const auto& j : root.get("zones").items()) {
        CopperZone z;
        z.net = j.get("net").asString("");
        z.layer = std::clamp(j.get("layer").asInt(0), 0, s.bottomLayer());
        z.plane = j.get("plane").asBool(false);
        z.clearance = std::max(0.0, j.get("clearance").asNumber(0));
        if (!z.net.empty()) p.pcb.zones.push_back(z);
    }
    for (const auto& j : root.get("tamperMeshes").items()) {
        TamperMesh m;
        m.componentRef = j.get("component").asString("");
        m.netA = j.get("netA").asString("");
        m.netB = j.get("netB").asString("");
        m.layerA = j.get("layerA").asInt(1);
        m.layerB = j.get("layerB").asInt(2);
        m.margin = std::clamp(j.get("margin").asNumber(2.0), 0.0, 20.0);
        if (!m.componentRef.empty()) p.pcb.tamperMeshes.push_back(m);
    }

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
        c.firmware = j.get("firmware").asString("");
        c.firmwareName = j.get("firmwareName").asString("");
        c.clockHz = j.get("clockHz").asNumber(0);
        c.sourcing = sourcingFrom(j.get("sourcing"));
        {
            // Only a variant this kind offers (an unknown one from a newer file falls back to the default).
            const std::string package = j.get("package").asString("");
            const auto variants = Library::packageVariants(c.kind);
            if (std::find(variants.begin(), variants.end(), package) != variants.end()) c.package = package;
        }
        for (const auto& nc : j.get("noConnect").items()) {
            int pin = nc.asInt(-1);
            if (pin >= 0 && pin < static_cast<int>(c.def().pins.size()) && !c.isNoConnect(pin)) c.noConnect.push_back(pin);
        }
        const Json& pc = j.get("pcb");
        c.pcb.position = {pc.get("x").asNumber(), pc.get("y").asNumber()};
        c.pcb.rotation = pc.get("rotation").asInt(0);
        c.pcb.bottom = pc.get("bottom").asBool(false);
        c.pcb.placed = pc.get("placed").asBool(false);
        c.pcb.locked = c.pcb.placed && pc.get("locked").asBool(false);
        c.pcb.embeddedLayer = canEmbed(c) ? std::max(0, pc.get("embeddedLayer").asInt(0)) : 0;
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
        const int last = std::max(0, s.layerCount - 1);
        v.fromLayer = std::clamp(static_cast<int>(j.get("fromLayer").asNumber(0)), 0, last);
        v.toLayer = static_cast<int>(j.get("toLayer").asNumber(-1));
        if (v.toLayer >= last || v.toLayer < v.fromLayer) v.toLayer = -1;
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
        j["footprint"] = c.footprintName();
        if (!c.package.empty()) j["package"] = c.package;
        {
            const auto variants = Library::packageVariants(c.kind);
            if (!variants.empty()) {
                Json options = Json::array();
                for (const auto& v : variants) {
                    Json o = Json::object();
                    o["id"] = v;
                    o["label"] = Library::packageLabel(v);
                    options.push(o);
                }
                j["packageOptions"] = options;
            }
        }
        if (c.kind == ComponentKind::Custom) j["customPart"] = c.customPart;
        if (c.kind == ComponentKind::Custom) {
            if (const CustomPart* part = CustomPartRegistry::instance().find(c.customPart)) {
                if (auto model = mcuModelForPart(part->spec.name)) {
                    Json m = Json::object();
                    m["model"] = mcuModelName(*model);
                    m["clockHz"] = c.clockHz > 0 ? c.clockHz : defaultMcuClock(*model);
                    m["firmwareName"] = c.firmwareName;
                    HexImage img = c.firmware.empty() ? HexImage{} : parseIntelHex(c.firmware);
                    m["firmwareBytes"] = static_cast<int>(img.ok() ? img.bytes.size() : 0);
                    m["firmwareError"] = c.firmware.empty() ? std::string() : img.error;
                    j["mcu"] = m;
                }
            }
        }
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
            pj["noConnect"] = c.isNoConnect(static_cast<int>(i));
            pins.push(pj);
        }
        j["pins"] = pins;
        Json pc = Json::object();
        pc["x"] = c.pcb.position.x;
        pc["y"] = c.pcb.position.y;
        pc["rotation"] = c.pcb.rotation;
        pc["bottom"] = c.pcb.bottom;
        pc["placed"] = c.pcb.placed;
        pc["locked"] = c.pcb.locked;
        pc["embeddedLayer"] = c.pcb.embeddedLayer;
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
        j["role"] = netRoleName(schematic.netRole(n.index));
        netArr.push(j);
    }
    root["nets"] = netArr;
    root["industry"] = industry;
    root["robotPlatform"] = robotPlatform;
    root["ecuType"] = ecuType;
    root["aerospaceMission"] = aerospaceMission;
    root["navalPlatform"] = navalPlatform;
    root["medicalClass"] = medicalClass;
    root["retailDevice"] = retailDevice;
    root["applianceType"] = applianceType;
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
        j["layer"] = p.throughHole ? -1 : p.smdLayer;
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
        j["fromLayer"] = v.fromLayer;
        j["toLayer"] = v.lastLayer(pcb.settings.layerCount);
        j["kind"] = viaKind(v, pcb.settings.layerCount);
        vias.push(j);
    }
    root["vias"] = vias;

    root["zones"] = zonesJson(pcb.zones);
    root["tamperMeshes"] = tamperMeshesJson(pcb.tamperMeshes);
    Json fills = Json::array();
    for (const auto& f : pcb.zoneFills(schematic)) {
        Json j = Json::object();
        j["zone"] = f.zone;
        j["net"] = f.net;
        j["layer"] = f.layer;
        j["islands"] = f.islands;
        j["area"] = f.area();
        Json rects = Json::array();  // flat x0, y0, x1, y1 quadruples
        for (const auto& r : f.rects) {
            rects.push(r.x0);
            rects.push(r.y0);
            rects.push(r.x1);
            rects.push(r.y1);
        }
        j["rects"] = rects;
        fills.push(j);
    }
    root["zoneFills"] = fills;

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
        const FootprintDef* fp = Library::instance().footprint(c.footprintName());
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
    // One entry per component: custom parts with a regulator and supply loads report the first element's
    // current/voltage and their total dissipation.
    std::vector<DeviceReading> merged;
    std::map<int, size_t> at;
    for (const auto& d : r.devices) {
        auto it = at.find(d.componentId);
        if (it == at.end()) {
            at[d.componentId] = merged.size();
            merged.push_back(d);
        } else {
            merged[it->second].power += d.power;
        }
    }
    Json devs = Json::array();
    for (const auto& d : merged) {
        const Component* c = schematic.find(d.componentId);
        Json j = Json::object();
        j["component"] = d.componentId;
        j["ref"] = c ? c->ref : std::string();
        j["current"] = d.current;
        j["power"] = d.power;
        j["voltage"] = d.voltage;
        // Operating state only for parts with a regulator/charger model.
        if (c && c->kind == ComponentKind::Custom)
            if (const CustomPart* part = CustomPartRegistry::instance().find(c->customPart))
                if (part->spec.model.hasRegulator) j["state"] = d.state;
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
    Json mcus = Json::array();
    for (const auto& m : r.mcus) {
        const Component* c = schematic.find(m.componentId);
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
