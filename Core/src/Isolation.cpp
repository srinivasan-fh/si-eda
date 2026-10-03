#include "sieda/Isolation.hpp"

#include <map>
#include <numeric>

#include "SystemParts.hpp"

namespace sieda {

bool isIsolationBarrier(const Component& c) {
    using namespace sysparts;
    if (c.kind != ComponentKind::Custom) return false;
    const std::string n = partName(c);
    if (containsAny(n, {"ADUM", "ISO77", "ISO78", "ISO15", "ISO72", "SI86", "PC817", "6N137", "TLP", "OPTO", "HCPL", "ISO-DCDC",
                        "NME", "MHF", "SVR28", "XFMR", "TRANSFORMER", "ISOLAT"}))
        return true;
    const CustomPart* cp = CustomPartRegistry::instance().find(c.customPart);
    return cp && cp->spec.model.hasRegulator && cp->spec.model.regulator.isolated();
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

}  // namespace sieda
