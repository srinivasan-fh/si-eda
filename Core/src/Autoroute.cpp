// SiEDA Core — autorouter strategy options and the routing report as JSON (sieda/Autoroute.hpp).
#include "sieda/Autoroute.hpp"

#include <algorithm>
#include <cmath>

namespace sieda {

bool AutorouteOptions::operator==(const AutorouteOptions& o) const {
    return coupledPairs == o.coupledPairs && pairGap == o.pairGap && lengthAware == o.lengthAware &&
           minimizeVias == o.minimizeVias && gloss == o.gloss && arcCorners == o.arcCorners && arcRadius == o.arcRadius &&
           teardrops == o.teardrops;
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
    return o;
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
