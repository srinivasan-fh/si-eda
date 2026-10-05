#include "sieda/Variants.hpp"

namespace sieda {

Schematic applyVariant(const Schematic& base, const DesignVariant& v) {
    Schematic out = base;
    if (v.parts.empty()) return out;
    for (auto& c : out.mutableComponents()) {
        if (isNetSymbolKind(c.kind)) continue;
        auto it = v.parts.find(c.id);
        if (it == v.parts.end()) continue;
        if (it->second.fitted >= 0) c.sourcing.dnp = it->second.fitted == 0;
        if (!it->second.value.empty()) c.value = it->second.value;
    }
    return out;
}

Json variantToJson(const DesignVariant& v, const Schematic& sch) {
    Json j = Json::object();
    j["name"] = v.name;
    j["description"] = v.description;
    Json parts = Json::array();
    for (const auto& [id, part] : v.parts) {
        const Component* c = sch.find(id);
        if (!c || part.empty()) continue;
        Json p = Json::object();
        p["component"] = id;
        p["ref"] = c->ref;
        if (part.fitted >= 0) p["fitted"] = part.fitted == 1;
        if (!part.value.empty()) p["value"] = part.value;
        parts.push(p);
    }
    j["parts"] = parts;
    return j;
}

DesignVariant variantFromJson(const Json& j) {
    DesignVariant v;
    v.name = j.get("name").asString("");
    if (v.name.empty()) throw JsonError("Variant without a name");
    v.description = j.get("description").asString("");
    for (const auto& p : j.get("parts").items()) {
        const int id = p.get("component").asInt(-1);
        if (id < 0) continue;
        VariantPart part;
        if (p.has("fitted")) part.fitted = p.get("fitted").asBool(true) ? 1 : 0;
        part.value = p.get("value").asString("");
        if (!part.empty()) v.parts[id] = part;
    }
    return v;
}

}  // namespace sieda
