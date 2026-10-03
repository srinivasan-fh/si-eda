#include "sieda/Isolation.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <numeric>

#include "SystemParts.hpp"
#include "sieda/Pcb.hpp"

namespace sieda {

bool isIsolationBarrier(const Component& c) {
    using namespace sysparts;
    if (c.kind != ComponentKind::Custom) return false;
    const std::string n = partName(c);
    if (containsAny(n, {"ADUM", "ISO77", "ISO78", "ISO15", "ISO72", "SI86", "PC817", "6N137", "TLP", "OPTO", "HCPL", "ISO-DCDC",
                        "NME", "MHF", "SVR28", "XFMR", "TRANSFORMER", "ISOLAT"}))
        return true;
    const CustomPart* cp = CustomPartRegistry::instance().find(c.customPart);
    return cp && cp->spec.model.hasRegulator && cp->spec.model.regulator.galvanic();
}

GalvanicDomains galvanicDomains(const Schematic& sch) {
    GalvanicDomains d;
    const auto& nets = sch.nets();
    std::vector<int> parent(nets.size());
    std::iota(parent.begin(), parent.end(), 0);
    auto find = [&](int x) {
        while (parent[static_cast<size_t>(x)] != x) x = parent[static_cast<size_t>(x)] = parent[static_cast<size_t>(parent[static_cast<size_t>(x)])];
        return x;
    };
    std::vector<char> used(nets.size(), 0);
    for (const auto& c : sch.components()) {
        if (isNetSymbolKind(c.kind)) continue;
        const bool barrier = isIsolationBarrier(c);
        int first = -1;
        for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
            const int n = sch.netOf({c.id, i});
            if (n < 0) continue;
            used[static_cast<size_t>(n)] = 1;
            if (barrier) continue;
            if (first < 0) first = n;
            else parent[static_cast<size_t>(find(n))] = find(first);
        }
    }
    std::map<int, int> ids;
    d.netDomain.assign(nets.size(), -1);
    for (size_t i = 0; i < nets.size(); ++i) {
        if (!used[i] || nets[i].pins.size() < 2) continue;  // an unconnected pin is no domain
        const int root = find(static_cast<int>(i));
        auto it = ids.find(root);
        if (it == ids.end()) it = ids.emplace(root, static_cast<int>(ids.size())).first;
        d.netDomain[i] = it->second;
    }
    d.count = static_cast<int>(ids.size());
    return d;
}

int GalvanicDomains::domainOfComponent(const Schematic& sch, const Component& c) const {
    if (isIsolationBarrier(c)) return -1;
    int dom = -2;
    for (int i = 0; i < static_cast<int>(c.def().pins.size()); ++i) {
        const int x = domainOfNet(sch.netOf({c.id, i}));
        if (x < 0) continue;
        if (dom == -2) dom = x;
        else if (dom != x) return -1;
    }
    return dom;
}

SpacingDomains spacingDomains(const Schematic& sch, const BoardSettings& s) {
    SpacingDomains out;
    const GalvanicDomains galv = galvanicDomains(sch);
    const size_t nn = sch.nets().size();
    out.highVoltage.assign(nn, 0);
    // Voltage classes: nets more than 60 V (the SELV limit) from ground, spaced by IPC-2221 for their worst pair.
    const auto ranges = netVoltageRanges(sch);
    double hvGap = 0;
    for (const auto& [a, ra] : ranges) {
        if (a < 0 || static_cast<size_t>(a) >= nn || std::max(std::fabs(ra.first), std::fabs(ra.second)) <= 60) continue;
        if (sch.nets()[static_cast<size_t>(a)].pins.size() < 2) continue;  // an open pin's node floats
        out.highVoltage[static_cast<size_t>(a)] = 1;
        for (const auto& [b, rb] : ranges) {
            if (b == a || b < 0 || static_cast<size_t>(b) >= nn || sch.nets()[static_cast<size_t>(b)].pins.size() < 2) continue;
            const double dv = std::max(std::fabs(ra.second - rb.first), std::fabs(rb.second - ra.first));
            hvGap = std::max(hvGap, ipc2221Clearance(dv, s.highAltitude, s.coated()));
        }
    }
    // Up to 0.6 mm (≤ 150 V uncoated) the router simply widens its clearance everywhere (see autoRoute).
    out.hvGap = hvGap > 0.6 + 1e-9 ? hvGap : 0;
    out.gap = std::max(s.isolationGap > 0 ? s.isolationGap : 0.0, out.hvGap);
    if (out.gap <= 0) return out;
    std::map<std::pair<int, int>, int> ids;
    out.domains.netDomain.assign(nn, -1);
    for (size_t i = 0; i < nn; ++i) {
        const int g = galv.netDomain[i];
        if (g < 0) continue;
        const int base = s.isolationGap > 0 ? g : 0;
        const int cls = out.hvGap > 0 && out.highVoltage[i] ? static_cast<int>(i) + 1 : 0;
        auto it = ids.find({base, cls});
        if (it == ids.end()) it = ids.emplace(std::make_pair(base, cls), static_cast<int>(ids.size())).first;
        out.domains.netDomain[i] = it->second;
    }
    out.domains.count = static_cast<int>(ids.size());
    return out;
}

}  // namespace sieda
