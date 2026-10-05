// SiEDA Core — autorouter strategy options (AutorouteOptions in Pcb.hpp) as JSON, and the routing report. See
// docs/ROUTING.md (Strategies, Routing report).
#pragma once

#include <string>

#include "sieda/Json.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

/// {"coupledPairs","pairGap","lengthAware"} — every field, as saved with the board.
Json autorouteOptionsToJson(const AutorouteOptions& o);
/// Options from JSON; missing fields keep their value in `base`, out-of-range values are clamped.
AutorouteOptions autorouteOptionsFromJson(const Json& j, AutorouteOptions base = {});
/// {"pairs":[{"positive","negative","coupled","reason","width","gap","coupledLength","uncoupledLength","skew",
///  "viaPairs"}],"lengths":[{"net","source","target","tolerance","routed","achieved","ok","tuned"}]}
Json routeReportJson(const RouteReport& r);

}  // namespace sieda
