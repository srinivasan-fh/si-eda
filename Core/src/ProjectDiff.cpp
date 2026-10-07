#include "sieda/ProjectDiff.hpp"

#include <cmath>
#include <cstdio>
#include <map>
#include <set>

#include "sieda/TrackGeometry.hpp"
#include "sieda/Variants.hpp"

namespace sieda {

namespace {

using PinSet = std::set<std::string>;

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g", v);
    return b;
}

Json pair(Json a, Json b) {
    Json j = Json::array();
    j.push(std::move(a));
    j.push(std::move(b));
    return j;
}

Json list(const PinSet& s) {
    Json j = Json::array();
    for (const auto& v : s) j.push(v);
    return j;
}

/// Real parts (not net symbols) by designator.
std::map<std::string, const Component*> parts(const Schematic& s) {
    std::map<std::string, const Component*> m;
    for (const auto& c : s.components())
        if (!isNetSymbolKind(c.kind) && !c.ref.empty()) m[c.ref] = &c;
    return m;
}

/// Nets with ≥ 1 pin as name → pins ("R1.2").
std::map<std::string, PinSet> nets(const Schematic& s) {
    std::map<std::string, PinSet> m;
    for (const auto& n : s.nets()) {
        PinSet pins;
        for (const auto& p : n.pins)
            if (const Component* c = s.find(p.component); c && !isNetSymbolKind(c->kind))
                pins.insert(c->ref + "." + std::to_string(p.pin + 1));
        if (!pins.empty()) m[n.name.empty() ? "net" + std::to_string(n.index) : n.name] = pins;
    }
    return m;
}

struct Copper {
    int tracks = 0, vias = 0;
    double length = 0;
};

std::map<std::string, Copper> copper(const Project& p) {
    std::map<std::string, Copper> m;
    const auto& ns = p.schematic.nets();
    auto name = [&](int net) { return net >= 0 && net < static_cast<int>(ns.size()) ? ns[net].name : std::string("(no net)"); };
    for (const auto& t : p.pcb.tracks) {
        Copper& c = m[name(t.net)];
        ++c.tracks;
        c.length += trackLength(t);
    }
    for (const auto& v : p.pcb.vias) ++m[name(v.net)].vias;
    return m;
}

Json partJson(const Component& c) {
    Json j = Json::object();
    j["ref"] = c.ref;
    j["value"] = c.value;
    j["footprint"] = c.footprintName();
    return j;
}

}  // namespace

Json diffProjects(const Project& before, const Project& after) {
    Json d = Json::object();
    int changes = 0;

    // ---- components
    Json added = Json::array(), removed = Json::array(), changed = Json::array();
    const auto pa = parts(before.schematic), pb = parts(after.schematic);
    for (const auto& [ref, c] : pa)
        if (!pb.count(ref)) removed.push(partJson(*c));
    for (const auto& [ref, c] : pb) {
        auto it = pa.find(ref);
        if (it == pa.end()) {
            added.push(partJson(*c));
            continue;
        }
        const Component& o = *it->second;
        Json f = Json::object();
        auto field = [&](const char* key, const std::string& x, const std::string& y) {
            if (x != y) f[key] = pair(x, y);
        };
        field("value", o.value, c->value);
        field("footprint", o.footprintName(), c->footprintName());
        field("mpn", o.sourcing.mpn, c->sourcing.mpn);
        if (o.sourcing.dnp != c->sourcing.dnp) f["dnp"] = pair(o.sourcing.dnp, c->sourcing.dnp);
        if (o.pcb.placed != c->pcb.placed) f["placed"] = pair(o.pcb.placed, c->pcb.placed);
        if (o.pcb.placed && c->pcb.placed) {
            if ((o.pcb.position - c->pcb.position).length() > 1e-4) {
                Json a = Json::array(), b = Json::array();
                a.push(o.pcb.position.x), a.push(o.pcb.position.y), b.push(c->pcb.position.x), b.push(c->pcb.position.y);
                f["position"] = pair(a, b);
            }
            if (o.pcb.rotation != c->pcb.rotation) f["rotation"] = pair(o.pcb.rotation, c->pcb.rotation);
            if (o.pcb.bottom != c->pcb.bottom) f["side"] = pair(o.pcb.bottom ? "bottom" : "top", c->pcb.bottom ? "bottom" : "top");
        }
        if (f.size()) {
            Json j = Json::object();
            j["ref"] = ref;
            j["changes"] = f;
            changed.push(j);
        }
    }
    changes += static_cast<int>(added.size() + removed.size() + changed.size());
    Json comps = Json::object();
    comps["added"] = added, comps["removed"] = removed, comps["changed"] = changed;
    d["components"] = comps;

    // ---- connectivity (nets matched by their pins)
    const auto na = nets(before.schematic), nb = nets(after.schematic);
    std::map<PinSet, std::string> byPins;
    for (const auto& [n, pins] : na) byPins[pins] = n;
    std::set<std::string> matched;
    Json nAdded = Json::array(), nRemoved = Json::array(), nChanged = Json::array();
    std::vector<std::pair<std::string, const PinSet*>> unmatched;
    for (const auto& [n, pins] : nb) {
        auto it = byPins.find(pins);
        if (it == byPins.end()) {
            unmatched.push_back({n, &pins});
            continue;
        }
        matched.insert(it->second);
        if (it->second != n) {
            Json j = Json::object();
            j["name"] = n, j["oldName"] = it->second;
            nChanged.push(j);
        }
    }
    for (const auto& [n, pins] : unmatched) {
        // The old net sharing most pins (same name first) is the one this net grew or shrank from.
        std::string best;
        size_t bestShared = 0;
        for (const auto& [m, old] : na) {
            if (matched.count(m)) continue;
            size_t shared = 0;
            for (const auto& p : *pins) shared += old.count(p);
            if (shared > bestShared || (shared == bestShared && shared > 0 && m == n)) best = m, bestShared = shared;
        }
        if (best.empty()) {
            Json j = Json::object();
            j["name"] = n, j["pins"] = list(*pins);
            nAdded.push(j);
            continue;
        }
        matched.insert(best);
        PinSet plus, minus;
        for (const auto& p : *pins)
            if (!na.at(best).count(p)) plus.insert(p);
        for (const auto& p : na.at(best))
            if (!pins->count(p)) minus.insert(p);
        Json j = Json::object();
        j["name"] = n;
        if (best != n) j["oldName"] = best;
        j["added"] = list(plus), j["removed"] = list(minus);
        nChanged.push(j);
    }
    for (const auto& [n, pins] : na)
        if (!matched.count(n)) {
            Json j = Json::object();
            j["name"] = n, j["pins"] = list(pins);
            nRemoved.push(j);
        }
    changes += static_cast<int>(nAdded.size() + nRemoved.size() + nChanged.size());
    Json netsJ = Json::object();
    netsJ["added"] = nAdded, netsJ["removed"] = nRemoved, netsJ["changed"] = nChanged;
    d["nets"] = netsJ;

    // ---- copper per net
    Json cu = Json::array();
    auto ca = copper(before), cb = copper(after);
    std::set<std::string> cuNets;
    for (const auto& [n, c] : ca) cuNets.insert(n);
    for (const auto& [n, c] : cb) cuNets.insert(n);
    for (const auto& n : cuNets) {
        const Copper x = ca.count(n) ? ca[n] : Copper{}, y = cb.count(n) ? cb[n] : Copper{};
        if (x.tracks == y.tracks && x.vias == y.vias && std::fabs(x.length - y.length) < 1e-3) continue;
        Json j = Json::object();
        j["net"] = n, j["tracks"] = pair(x.tracks, y.tracks), j["vias"] = pair(x.vias, y.vias);
        j["length"] = pair(std::round(x.length * 1000) / 1000, std::round(y.length * 1000) / 1000);
        cu.push(j);
    }
    changes += static_cast<int>(cu.size());
    d["copper"] = cu;

    // ---- board
    Json board = Json::object();
    const BoardSettings &sa = before.pcb.settings, &sb = after.pcb.settings;
    auto num = [&](const char* k, double x, double y) {
        if (std::fabs(x - y) > 1e-6) board[k] = pair(x, y);
    };
    num("layers", sa.layerCount, sb.layerCount);
    num("width", sa.width, sb.width);
    num("height", sa.height, sb.height);
    num("thickness", sa.thickness, sb.thickness);
    num("outlinePoints", static_cast<double>(sa.outline.size()), static_cast<double>(sb.outline.size()));
    num("mountingHoles", static_cast<double>(sa.holes.size()), static_cast<double>(sb.holes.size()));
    num("zones", static_cast<double>(before.pcb.zones.size()), static_cast<double>(after.pcb.zones.size()));
    if (sa.solderMask != sb.solderMask) board["solderMask"] = pair(sa.solderMask, sb.solderMask);
    changes += static_cast<int>(board.size());
    d["board"] = board;

    // ---- variants
    Json vAdded = Json::array(), vRemoved = Json::array(), vChanged = Json::array();
    for (const auto& v : after.variants) {
        const DesignVariant* o = before.findVariant(v.name);
        if (!o) vAdded.push(v.name);
        else if (variantToJson(*o, before.schematic).dump() != variantToJson(v, after.schematic).dump()) vChanged.push(v.name);
    }
    for (const auto& v : before.variants)
        if (!after.findVariant(v.name)) vRemoved.push(v.name);
    changes += static_cast<int>(vAdded.size() + vRemoved.size() + vChanged.size());
    Json vars = Json::object();
    vars["added"] = vAdded, vars["removed"] = vRemoved, vars["changed"] = vChanged;
    d["variants"] = vars;

    d["identical"] = changes == 0;
    d["summary"] = std::to_string(added.size()) + " parts added, " + std::to_string(removed.size()) + " removed, " +
                   std::to_string(changed.size()) + " changed; " +
                   std::to_string(nAdded.size() + nRemoved.size() + nChanged.size()) + " nets changed; " +
                   std::to_string(cu.size()) + " nets re-routed";
    return d;
}

std::string diffText(const Json& d) {
    std::string o;
    auto line = [&](const std::string& s) { o += s + "\n"; };
    auto str = [](const Json& j) { return j.isString() ? j.asString() : j.dump(); };
    auto pins = [](const Json& a, const char* sign) {
        std::string s;
        for (size_t i = 0; i < a.size(); ++i) s += std::string(" ") + sign + a[i].asString();
        return s;
    };
    const Json& c = d.get("components");
    for (size_t i = 0; i < c.get("added").size(); ++i)
        line("+ " + c.get("added")[i].get("ref").asString() + " " + c.get("added")[i].get("value").asString() + " " + c.get("added")[i].get("footprint").asString());
    for (size_t i = 0; i < c.get("removed").size(); ++i)
        line("- " + c.get("removed")[i].get("ref").asString() + " " + c.get("removed")[i].get("value").asString());
    for (size_t i = 0; i < c.get("changed").size(); ++i) {
        std::string s = "~ " + c.get("changed")[i].get("ref").asString();
        for (const auto& [k, v] : c.get("changed")[i].get("changes").fields()) s += " " + k + " " + str(v[0]) + " → " + str(v[1]) + ";";
        s.pop_back();
        line(s);
    }
    const Json& n = d.get("nets");
    for (size_t i = 0; i < n.get("added").size(); ++i) line("+ net " + n.get("added")[i].get("name").asString() + pins(n.get("added")[i].get("pins"), ""));
    for (size_t i = 0; i < n.get("removed").size(); ++i) line("- net " + n.get("removed")[i].get("name").asString());
    for (size_t i = 0; i < n.get("changed").size(); ++i) {
        const Json& x = n.get("changed")[i];
        std::string s = "~ net " + x.get("name").asString();
        if (x.has("oldName")) s += " (was " + x.get("oldName").asString() + ")";
        if (x.has("added")) s += pins(x.get("added"), "+") + pins(x.get("removed"), "−");
        line(s);
    }
    const Json& cu = d.get("copper");
    for (size_t i = 0; i < cu.size(); ++i)
        line("~ copper " + cu[i].get("net").asString() + ": " + fmt(cu[i].get("length")[0].asNumber()) + " → " +
             fmt(cu[i].get("length")[1].asNumber()) + " mm, vias " + str(cu[i].get("vias")[0]) + " → " + str(cu[i].get("vias")[1]));
    for (const auto& [k, v] : d.get("board").fields()) line("~ board " + k + " " + str(v[0]) + " → " + str(v[1]));
    const Json& v = d.get("variants");
    for (size_t i = 0; i < v.get("added").size(); ++i) line("+ variant " + v.get("added")[i].asString());
    for (size_t i = 0; i < v.get("removed").size(); ++i) line("- variant " + v.get("removed")[i].asString());
    for (size_t i = 0; i < v.get("changed").size(); ++i) line("~ variant " + v.get("changed")[i].asString());
    return (d.get("identical").asBool() ? std::string("No changes.\n") : d.get("summary").asString() + "\n") + o;
}

Json variantMatrix(const Project& p) {
    Json out = Json::object(), vs = Json::array(), rows = Json::array();
    std::set<int> touched;
    for (const auto& v : p.variants)
        for (const auto& [id, part] : v.parts)
            if (!part.empty()) touched.insert(id);
    for (const auto& v : p.variants) {
        int fitted = 0, notFitted = 0, values = 0;
        const Schematic s = applyVariant(p.schematic, v);
        for (const auto& c : s.components()) {
            if (isNetSymbolKind(c.kind) || !c.hasFootprint()) continue;
            (c.sourcing.dnp ? notFitted : fitted)++;
        }
        for (const auto& [id, part] : v.parts) values += !part.value.empty();
        Json j = Json::object();
        j["name"] = v.name, j["fitted"] = fitted, j["notFitted"] = notFitted, j["valueChanges"] = values;
        vs.push(j);
    }
    for (int id : touched) {
        const Component* c = p.schematic.find(id);
        if (!c) continue;
        Json row = Json::object(), cells = Json::array();
        row["ref"] = c->ref, row["value"] = c->value;
        for (const auto& v : p.variants) {
            auto it = v.parts.find(id);
            const VariantPart part = it == v.parts.end() ? VariantPart{} : it->second;
            Json cell = Json::object();
            cell["fitted"] = part.fitted < 0 ? !c->sourcing.dnp : part.fitted == 1;
            cell["value"] = part.value.empty() ? c->value : part.value;
            cells.push(cell);
        }
        row["cells"] = cells;
        rows.push(row);
    }
    out["variants"] = vs, out["parts"] = rows;
    return out;
}

}  // namespace sieda
