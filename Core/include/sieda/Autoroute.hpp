// SiEDA Core — autorouter strategy options (AutorouteOptions in Pcb.hpp) as JSON, and the routing report. See
// docs/ROUTING.md (Strategies, Routing report).
#pragma once

#include <string>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

/// {"coupledPairs","pairGap","lengthAware","minimizeVias","gloss","arcCorners","arcRadius","teardrops","preset","fast",
/// "fanoutOnly","nets":[name],"netClass","hasArea","area":{"x0","y0","x1","y1"},"protectLocked",
/// "classLayers":{class:[layer]}} — every field, as saved with the board — plus "teardropStyle":"curved" only when the
/// teardrops are curved (absent = straight, so older files stay identical).
Json autorouteOptionsToJson(const AutorouteOptions& o);
/// Options from JSON; missing fields keep their value in `base`, out-of-range values are clamped.
AutorouteOptions autorouteOptionsFromJson(const Json& j, AutorouteOptions base = {});
/// {"pairs":[{"positive","negative","coupled","reason","width","gap","coupledLength","uncoupledLength","skew",
///  "viaPairs"}],"lengths":[{"net","source","target","tolerance","routed","achieved","ok","tuned"}],
///  "metrics":{"vias","microvias","blindVias","trackLength","layerLength":[mm per layer],"segments","arcs","teardrops",
///  "unrouted","viasRemoved","netsRerouted","glossed","arcsAdded","teardropsAdded"}}
Json routeReportJson(const RouteReport& r);
/// [{"name","title","description","options":{…}}] (autoroutePresets()).
Json autoroutePresetsJson();
/// Routing keep-outs: [{"name","x0","y0","x1","y1","layer","tracks","vias"}] (layer -1 = every layer).
Json keepoutsToJson(const std::vector<RouteKeepout>& keepouts);
/// Keep-outs from JSON (empty or inverted areas and keep-outs that keep nothing out are dropped).
std::vector<RouteKeepout> keepoutsFromJson(const Json& j);

}  // namespace sieda
