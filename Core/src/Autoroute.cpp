// SiEDA Core — autorouter strategy options and the routing report as JSON (sieda/Autoroute.hpp).
#include "sieda/Autoroute.hpp"

#include <algorithm>
#include <cmath>

namespace sieda {

bool AutorouteOptions::operator==(const AutorouteOptions& o) const {
    return coupledPairs == o.coupledPairs && pairGap == o.pairGap && lengthAware == o.lengthAware;
}

namespace {
double clampMm(double v, double lo, double hi, double def) { return std::isfinite(v) ? std::clamp(v, lo, hi) : def; }
}  // namespace

Json autorouteOptionsToJson(const AutorouteOptions& o) {
    Json j = Json::object();
    j["coupledPairs"] = o.coupledPairs;
    j["pairGap"] = o.pairGap;
    j["lengthAware"] = o.lengthAware;
    return j;
}

AutorouteOptions autorouteOptionsFromJson(const Json& j, AutorouteOptions o) {
    if (!j.isObject()) return o;
    o.coupledPairs = j.get("coupledPairs").asBool(o.coupledPairs);
    if (j.has("pairGap")) o.pairGap = clampMm(j.get("pairGap").asNumber(o.pairGap), 0.0, 2.0, 0.0);
    o.lengthAware = j.get("lengthAware").asBool(o.lengthAware);
    return o;
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
    return j;
}

}  // namespace sieda
