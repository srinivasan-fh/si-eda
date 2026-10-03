// SiEDA Core — galvanic isolation domains. Nets joined by any part other than an isolation barrier (digital
// isolators, optocouplers, isolated DC-DC converters, isolation transformers) form one domain; with
// BoardSettings::isolationGap set, placement, routing, pours and DRC keep different domains that far apart
// (creepage / clearance across a patient barrier: 8 mm for 2 × MOPP, 4 mm for 1 × MOPP).
#pragma once

#include <vector>

#include "sieda/Schematic.hpp"

namespace sieda {

/// True for parts that bridge two domains by design (digital isolators, optocouplers, isolated converters…).
bool isIsolationBarrier(const Component& c);

struct GalvanicDomains {
    std::vector<int> netDomain;  // net index → domain id (-1 for nets on no part)
    int count = 0;               // number of domains
    int domainOfNet(int net) const {
        return net >= 0 && net < static_cast<int>(netDomain.size()) ? netDomain[static_cast<size_t>(net)] : -1;
    }
    /// A component's domain: the one domain all its connected pins sit in, -1 for barrier parts and parts spanning
    /// several domains, -2 for parts with no connected pin.
    int domainOfComponent(const Schematic& sch, const Component& c) const;
};

GalvanicDomains galvanicDomains(const Schematic& sch);

}  // namespace sieda
