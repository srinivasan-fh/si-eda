// SiEDA Core — autorouter strategy options and the routing report as JSON (sieda/Autoroute.hpp).
#include "sieda/Autoroute.hpp"

#include <algorithm>
#include <cmath>

namespace sieda {

bool AutorouteOptions::operator==(const AutorouteOptions& o) const {
    return coupledPairs == o.coupledPairs && pairGap == o.pairGap && lengthAware == o.lengthAware &&
           minimizeVias == o.minimizeVias && gloss == o.gloss && arcCorners == o.arcCorners && arcRadius == o.arcRadius &&
           teardrops == o.teardrops && teardropStyle == o.teardropStyle && preset == o.preset && fast == o.fast && fanoutOnly == o.fanoutOnly && nets == o.nets &&
           netClass == o.netClass && hasArea == o.hasArea && (!hasArea || (area.x0 == o.area.x0 && area.y0 == o.area.y0 &&
           area.x1 == o.area.x1 && area.y1 == o.area.y1)) && protectLocked == o.protectLocked && classLayers == o.classLayers;
}

const std::vector<AutoroutePreset>& autoroutePresets() {
    static const std::vector<AutoroutePreset> presets = [] {
        std::vector<AutoroutePreset> v;
        auto add = [&](const char* name, const char* title, const char* description, AutorouteOptions o) {
            o.preset = name;
            v.push_back({name, title, description, o});
        };
        add("default", "Default", "Every net, as the router has always routed them.", AutorouteOptions{});
        AutorouteOptions fast;
        fast.fast = true;
        add("fast", "Fast", "Fewer rip-up passes: a quick first route of a dense board.", fast);
        AutorouteOptions quality;
        quality.coupledPairs = true;
        quality.lengthAware = true;
        quality.minimizeVias = true;
        quality.gloss = true;
        add("quality", "High quality",
            "Coupled differential pairs, length rules and match groups to target, fewer vias, glossed tracks.", quality);
        AutorouteOptions fanout;
        fanout.fanoutOnly = true;
        add("fanout", "Fan-out only", "BGA dog-bones and the vias of pour and plane pads; nothing else.", fanout);
        add("nets", "Selected nets", "Only the selected nets; everything else keeps its copper.", AutorouteOptions{});
        add("netclass", "Net class", "Only the nets of one net class; everything else keeps its copper.", AutorouteOptions{});
        add("area", "Area", "Only the nets whose pads all lie in an area, inside it.", AutorouteOptions{});
        return v;
    }();
    return presets;
}

const AutorouteOptions* autoroutePreset(const std::string& name) {
    for (const auto& p : autoroutePresets())
        if (p.name == name) return &p.options;
    return nullptr;
}

namespace {
double clampMm(double v, double lo, double hi, double def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }
}  // namespace

Json autorouteOptionsToJson(const AutorouteOptions& o) {
    Json j = Json::object();
    j["coupledPairs"] = o.coupledPairs;
    j["pairGap"] = o.pairGap;
    j["lengthAware"] = o.lengthAware;
    j["minimizeVias"] = o.minimizeVias;
    j["gloss"] = o.gloss;
    j["arcCorners"] = o.arcCorners;
    j["arcRadius"] = o.arcRadius;
    j["teardrops"] = o.teardrops;
    if (o.teardropStyle != TeardropStyle::Straight) j["teardropStyle"] = teardropStyleName(o.teardropStyle);  // only when changed
    j["preset"] = o.preset;
    j["fast"] = o.fast;
    j["fanoutOnly"] = o.fanoutOnly;
    Json nets = Json::array();
    for (const auto& n : o.nets) nets.push(n);
    j["nets"] = nets;
    j["netClass"] = o.netClass;
    j["hasArea"] = o.hasArea;
    if (o.hasArea) {
        Json a = Json::object();
        a["x0"] = o.area.x0;
        a["y0"] = o.area.y0;
        a["x1"] = o.area.x1;
        a["y1"] = o.area.y1;
        j["area"] = a;
    }
    j["protectLocked"] = o.protectLocked;
    Json cl = Json::object();
    for (const auto& [name, layers] : o.classLayers) {
        Json l = Json::array();
        for (int k : layers) l.push(k);
        cl[name] = l;
    }
    j["classLayers"] = cl;
    return j;
}

AutorouteOptions autorouteOptionsFromJson(const Json& j, AutorouteOptions o) {
    if (!j.isObject()) return o;
    o.coupledPairs = j.get("coupledPairs").asBool(o.coupledPairs);
    if (j.has("pairGap")) o.pairGap = clampMm(j.get("pairGap").asNumber(o.pairGap), 0.0, 2.0, 0.0);
    o.lengthAware = j.get("lengthAware").asBool(o.lengthAware);
    o.minimizeVias = j.get("minimizeVias").asBool(o.minimizeVias);
    o.gloss = j.get("gloss").asBool(o.gloss);
    o.arcCorners = j.get("arcCorners").asBool(o.arcCorners);
    if (j.has("arcRadius")) o.arcRadius = clampMm(j.get("arcRadius").asNumber(o.arcRadius), 0.0, 20.0, 0.0);
    o.teardrops = j.get("teardrops").asBool(o.teardrops);
    if (j.has("teardropStyle")) o.teardropStyle = teardropStyleFromName(j.get("teardropStyle").asString(""), o.teardropStyle);
    if (j.has("preset")) {
        const std::string p = j.get("preset").asString("default");
        o.preset = autoroutePreset(p) ? p : "default";
    }
    o.fast = j.get("fast").asBool(o.fast);
    o.fanoutOnly = j.get("fanoutOnly").asBool(o.fanoutOnly);
    if (j.has("nets")) {
        o.nets.clear();
        for (const auto& n : j.get("nets").items())
            if (n.isString() && !n.asString().empty()) o.nets.push_back(n.asString());
    }
    if (j.has("netClass")) o.netClass = j.get("netClass").asString("");
    o.hasArea = j.get("hasArea").asBool(o.hasArea);
    if (j.has("area")) {
        const Json& a = j.get("area");
        const double x0 = a.get("x0").asNumber(0), y0 = a.get("y0").asNumber(0), x1 = a.get("x1").asNumber(0), y1 = a.get("y1").asNumber(0);
        if (std::isfinite(x0) && std::isfinite(y0) && std::isfinite(x1) && std::isfinite(y1))
            o.area = Rect(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
    }
    if (o.hasArea && (o.area.width() <= 0 || o.area.height() <= 0)) o.hasArea = false;
    o.protectLocked = j.get("protectLocked").asBool(o.protectLocked);
    if (j.has("classLayers")) {
        o.classLayers.clear();
        const Json& cl = j.get("classLayers");
        if (cl.isObject())
            for (const auto& [name, layers] : cl.fields()) {
                std::vector<int> ls;
                for (const auto& l : layers.items()) {
                    const int k = l.asInt(-1);
                    if (k >= 0 && k < BoardSettings::kMaxLayers && std::find(ls.begin(), ls.end(), k) == ls.end()) ls.push_back(k);
                }
                std::sort(ls.begin(), ls.end());
                if (!name.empty() && !ls.empty()) o.classLayers[name] = ls;
            }
    }
    return o;
}

Json autoroutePresetsJson() {
    Json a = Json::array();
    for (const auto& p : autoroutePresets()) {
        Json j = Json::object();
        j["name"] = p.name;
        j["title"] = p.title;
        j["description"] = p.description;
        j["options"] = autorouteOptionsToJson(p.options);
        a.push(j);
    }
    return a;
}

Json keepoutsToJson(const std::vector<RouteKeepout>& ks) {
    Json a = Json::array();
    for (const RouteKeepout& k : ks) {
        Json j = Json::object();
        j["name"] = k.name;
        j["x0"] = k.area.x0;
        j["y0"] = k.area.y0;
        j["x1"] = k.area.x1;
        j["y1"] = k.area.y1;
        j["layer"] = k.layer;
        j["tracks"] = k.tracks;
        j["vias"] = k.vias;
        a.push(j);
    }
    return a;
}

std::vector<RouteKeepout> keepoutsFromJson(const Json& j) {
    std::vector<RouteKeepout> out;
    if (!j.isArray()) return out;
    for (const Json& e : j.items()) {
        RouteKeepout k;
        k.name = e.get("name").asString("");
        const double x0 = e.get("x0").asNumber(0), y0 = e.get("y0").asNumber(0), x1 = e.get("x1").asNumber(0), y1 = e.get("y1").asNumber(0);
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) continue;
        k.area = Rect(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
        if (k.area.width() <= 0 || k.area.height() <= 0) continue;
        k.layer = std::clamp(e.get("layer").asInt(-1), -1, BoardSettings::kMaxLayers - 1);
        k.tracks = e.get("tracks").asBool(true);
        k.vias = e.get("vias").asBool(true);
        if (k.tracks || k.vias) out.push_back(k);
    }
    return out;
}

Json routeReportJson(const RouteReport& r) {
    Json j = Json::object();
    Json pairs = Json::array();
    for (const PairRouteReport& p : r.pairs) {
        Json q = Json::object();
        q["positive"] = p.positive;
        q["negative"] = p.negative;
        q["coupled"] = p.coupled;
        q["reason"] = p.reason;
        q["width"] = p.width;
        q["gap"] = p.gap;
        q["coupledLength"] = p.coupledLength;
        q["uncoupledLength"] = p.uncoupledLength;
        q["skew"] = p.skew;
        q["viaPairs"] = p.viaPairs;
        pairs.push(q);
    }
    j["pairs"] = pairs;
    Json lengths = Json::array();
    for (const LengthRouteReport& l : r.lengths) {
        Json q = Json::object();
        q["net"] = l.net;
        q["source"] = l.source;
        q["target"] = l.target;
        q["tolerance"] = l.tolerance;
        q["routed"] = l.routed;
        q["achieved"] = l.achieved;
        q["ok"] = l.ok;
        q["tuned"] = l.tuned;
        lengths.push(q);
    }
    j["lengths"] = lengths;
    const RouteMetrics& m = r.metrics;
    Json q = Json::object();
    q["vias"] = m.vias;
    q["microvias"] = m.microvias;
    q["blindVias"] = m.blindVias;
    q["trackLength"] = m.trackLength;
    Json layers = Json::array();
    for (double l : m.layerLength) layers.push(l);
    q["layerLength"] = layers;
    q["segments"] = m.segments;
    q["arcs"] = m.arcs;
    q["teardrops"] = m.teardrops;
    q["unrouted"] = m.unrouted;
    q["viasRemoved"] = m.viasRemoved;
    q["netsRerouted"] = m.netsRerouted;
    q["glossed"] = m.glossed;
    q["arcsAdded"] = m.arcsAdded;
    q["teardropsAdded"] = m.teardropsAdded;
    j["metrics"] = q;
    return j;
}

}  // namespace sieda
