#include "sieda/ProjectDiff.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
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

    // ---- review comments
    Json rAdded = Json::array(), rResolved = Json::array();
    for (const auto& c : after.reviewComments) {
        auto it = std::find_if(before.reviewComments.begin(), before.reviewComments.end(),
                               [&](const ReviewComment& o) { return o.id == c.id; });
        if (it == before.reviewComments.end()) rAdded.push(c.id);
        else if (c.resolved && !it->resolved) rResolved.push(c.id);
    }
    changes += static_cast<int>(rAdded.size() + rResolved.size());
    Json review = Json::object();
    review["added"] = rAdded, review["resolved"] = rResolved;
    d["review"] = review;

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
    const Json& rv = d.get("review");
    for (size_t i = 0; i < rv.get("added").size(); ++i) line("+ review comment #" + str(rv.get("added")[i]));
    for (size_t i = 0; i < rv.get("resolved").size(); ++i) line("~ review comment #" + str(rv.get("resolved")[i]) + " resolved");
    return (d.get("identical").asBool() ? std::string("No changes.\n") : d.get("summary").asString() + "\n") + o;
}

namespace {

bool same(const Json& a, const Json& b) { return a.type() == b.type() && a.dump() == b.dump(); }

std::string brief(const Json& j) {
    std::string s = j.isNull() ? std::string("(none)") : j.dump();
    return s.size() > 60 ? s.substr(0, 57) + "..." : s;
}

bool idList(const Json& a) {
    if (!a.isArray() || a.size() == 0) return false;
    for (const Json& e : a.items())
        if (!e.isObject() || !e.has("id")) return false;
    return true;
}

bool objectList(const Json& a) {
    if (!a.isArray()) return false;
    for (const Json& e : a.items())
        if (!e.isObject()) return false;
    return true;
}

Json merge3(const Json& b, const Json& o, const Json& t, const std::string& path, std::vector<std::string>& conflicts) {
    if (same(o, t)) return o;
    if (same(o, b)) return t;
    if (same(t, b)) return o;
    if (o.isObject() && t.isObject() && (b.isObject() || b.isNull())) {
        Json out = Json::object();
        std::set<std::string> keys;
        for (const auto& [k, v] : o.fields()) keys.insert(k);
        for (const auto& [k, v] : t.fields()) keys.insert(k);
        for (const std::string& k : keys) {
            Json m = merge3(b.isObject() ? b.get(k) : Json(), o.get(k), t.get(k), path.empty() ? k : path + "." + k, conflicts);
            if (!m.isNull()) out[k] = m;
        }
        return out;
    }
    const bool listsOfIds = (idList(o) || idList(t)) && objectList(o) && objectList(t) && (b.isNull() || objectList(b));
    if (listsOfIds) {
        // Item by item, keyed by id; ours' order first, then what only theirs added.
        auto index = [](const Json& a) {
            std::map<std::string, Json> m;
            if (a.isArray())
                for (const Json& e : a.items()) m[e.get("id").dump()] = e;
            return m;
        };
        const auto bi = index(b), oi = index(o), ti = index(t);
        std::vector<std::string> order;
        for (const Json& e : o.items()) order.push_back(e.get("id").dump());
        for (const Json& e : t.items())
            if (!oi.count(e.get("id").dump())) order.push_back(e.get("id").dump());
        Json out = Json::array();
        for (const std::string& id : order) {
            const Json be = bi.count(id) ? bi.at(id) : Json(), oe = oi.count(id) ? oi.at(id) : Json(),
                       te = ti.count(id) ? ti.at(id) : Json();
            const std::string at = path + "[id=" + id + "]";
            if (oe.isNull() || te.isNull()) {
                const Json& kept = oe.isNull() ? te : oe;  // the side that still has it
                if (be.isNull()) {                          // added by one side
                    out.push(kept);
                } else if (!same(kept, be)) {               // one side deleted it, the other changed it
                    conflicts.push_back(at + ": deleted on one side, changed on the other (kept)");
                    out.push(kept);
                }                                           // else deleted on one side, untouched on the other
                continue;
            }
            if (be.isNull() && !same(oe, te)) {
                conflicts.push_back(at + ": both sides added a different item with this id (ours kept)");
                out.push(oe);
                continue;
            }
            out.push(merge3(be, oe, te, at, conflicts));
        }
        return out;
    }
    if (objectList(o) && objectList(t) && (b.isNull() || objectList(b))) {
        // As multisets: base + what each side added − what each side removed.
        std::map<std::string, int> cb, co, ct;
        auto count = [&](const Json& a, std::map<std::string, int>& c) {
            if (a.isArray())
                for (const Json& e : a.items()) ++c[e.dump()];
        };
        count(b, cb), count(o, co), count(t, ct);
        Json out = Json::array();
        std::map<std::string, int> written;
        // The same addition (or removal) on both sides counts once.
        auto want = [&](const std::string& k) {
            const int d1 = co[k] - cb[k], d2 = ct[k] - cb[k];
            const int d = d1 > 0 && d2 > 0 ? std::max(d1, d2) : d1 < 0 && d2 < 0 ? std::min(d1, d2) : d1 + d2;
            return std::max(0, cb[k] + d);
        };
        for (const Json* side : {&o, &t})
            for (const Json& e : side->items()) {
                const std::string k = e.dump();
                if (written[k] < want(k)) {
                    out.push(e);
                    ++written[k];
                }
            }
        return out;
    }
    conflicts.push_back(path + ": ours " + brief(o) + ", theirs " + brief(t) + " (ours kept)");
    return o;
}

/// Both sides added an item under the same id (each editor took the next free id): theirs gets a fresh one, so both
/// survive the merge. Renumbered parts take references with them (wire ends, units) and a free designator if theirs
/// clashes with ours ("R2" → the next "R" number).
Json separateConcurrentAdds(const Json& base, const Json& ours, Json theirs) {
    if (!theirs.isObject()) return theirs;
    std::map<int, int> movedParts;
    const Json original = theirs;
    for (const auto& [key, list] : original.fields()) {
        if (!idList(list) || !idList(ours.get(key))) continue;
        std::set<int> inBase, inOurs, used;
        std::map<int, std::string> oursDump;
        std::set<std::string> refs;
        if (base.get(key).isArray())
            for (const Json& e : base.get(key).items()) used.insert(e.get("id").asInt(0)), inBase.insert(e.get("id").asInt(0));
        for (const Json& e : ours.get(key).items()) {
            const int id = e.get("id").asInt(0);
            inOurs.insert(id), used.insert(id), oursDump[id] = e.dump();
            if (e.get("ref").isString()) refs.insert(e.get("ref").asString());
        }
        for (const Json& e : list.items()) used.insert(e.get("id").asInt(0));
        int next = used.empty() ? 1 : *used.rbegin() + 1;
        std::map<int, int> moved;
        Json out = Json::array();
        for (Json e : list.items()) {
            const int id = e.get("id").asInt(0);
            if (e.get("id").isNumber() && !inBase.count(id) && inOurs.count(id) && oursDump[id] != e.dump()) {
                moved[id] = next;
                e["id"] = next++;
                if (e.get("ref").isString() && refs.count(e.get("ref").asString())) {
                    const std::string r = e.get("ref").asString(), prefix = r.substr(0, r.find_first_of("0123456789"));
                    for (int n = 1;; ++n)
                        if (!refs.count(prefix + std::to_string(n))) {
                            e["ref"] = prefix + std::to_string(n);
                            break;
                        }
                }
            }
            if (e.get("ref").isString()) refs.insert(e.get("ref").asString());
            out.push(e);
        }
        if (moved.empty()) continue;
        theirs[key] = out;
        if (key == "components") movedParts = moved;
    }
    if (movedParts.empty()) return theirs;
    // Everything in theirs that names a renumbered part follows it (wire ends, units of a package).
    std::function<Json(const Json&)> remap = [&](const Json& j) -> Json {
        if (j.isArray()) {
            Json a = Json::array();
            for (const Json& v : j.items()) a.push(remap(v));
            return a;
        }
        if (!j.isObject()) return j;
        Json o = Json::object();
        for (const auto& [k, v] : j.fields()) {
            const bool ref = (k == "component" || k == "componentId" || k == "unitOf") && v.isNumber() && movedParts.count(v.asInt(0));
            o[k] = ref ? Json(movedParts[v.asInt(0)]) : remap(v);
        }
        return o;
    };
    return remap(theirs);
}

}  // namespace

ProjectMerge mergeProjects(const Json& base, const Json& ours, const Json& theirs) {
    ProjectMerge r;
    try {
        for (const Json* j : {&base, &ours, &theirs})
            if (!j->isObject() || j->get("format").asString("") != "sieda-project") throw JsonError("Not a SiEDA project file");
        r.merged = merge3(base, ours, separateConcurrentAdds(base, ours, theirs), "", r.conflicts);
        Project::fromJson(r.merged);  // the merged file must load
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

Json reviewToJson(const std::vector<ReviewComment>& comments) {
    Json a = Json::array();
    for (const ReviewComment& c : comments) {
        Json j = Json::object(), replies = Json::array();
        j["id"] = c.id, j["author"] = c.author, j["text"] = c.text, j["view"] = c.view, j["resolved"] = c.resolved;
        if (!c.ref.empty()) j["ref"] = c.ref;
        if (c.hasAt) j["x"] = c.at.x, j["y"] = c.at.y;
        for (const auto& [who, text] : c.replies) {
            Json r = Json::object();
            r["author"] = who, r["text"] = text;
            replies.push(r);
        }
        if (replies.size()) j["replies"] = replies;
        a.push(j);
    }
    return a;
}

std::vector<ReviewComment> reviewFromJson(const Json& j) {
    std::vector<ReviewComment> out;
    if (!j.isArray()) return out;
    std::set<int> ids;
    for (const Json& e : j.items()) {
        ReviewComment c;
        c.id = e.get("id").asInt(0);
        c.text = e.get("text").asString("");
        if (c.id <= 0 || c.text.empty() || !ids.insert(c.id).second) continue;
        c.author = e.get("author").asString("");
        c.ref = e.get("ref").asString("");
        c.view = e.get("view").asString("pcb") == "schematic" ? "schematic" : "pcb";
        c.resolved = e.get("resolved").asBool(false);
        if (e.get("x").isNumber() && e.get("y").isNumber()) {
            c.at = {e.get("x").asNumber(), e.get("y").asNumber()};
            c.hasAt = std::isfinite(c.at.x) && std::isfinite(c.at.y);
        }
        for (const Json& r : e.get("replies").isArray() ? e.get("replies").items() : Json::Array{})
            if (!r.get("text").asString("").empty()) c.replies.push_back({r.get("author").asString(""), r.get("text").asString("")});
        out.push_back(std::move(c));
    }
    return out;
}

int reviewCommand(Project& p, const Json& q) {
    const std::string action = q.get("action").asString("");
    auto& cs = p.reviewComments;
    if (action == "add") {
        ReviewComment c;
        c.text = q.get("text").asString("");
        if (c.text.empty()) throw JsonError("A comment needs text");
        for (const auto& o : cs) c.id = std::max(c.id, o.id);
        ++c.id;
        c.author = q.get("author").asString("");
        c.ref = q.get("ref").asString("");
        if (!c.ref.empty() && !p.schematic.findByRef(c.ref)) throw JsonError("No part " + c.ref);
        c.view = q.get("view").asString("pcb") == "schematic" ? "schematic" : "pcb";
        if (q.get("x").isNumber() && q.get("y").isNumber()) c.at = {q.get("x").asNumber(), q.get("y").asNumber()}, c.hasAt = true;
        cs.push_back(c);
        return c.id;
    }
    const int id = q.get("id").asInt(0);
    auto it = std::find_if(cs.begin(), cs.end(), [&](const ReviewComment& c) { return c.id == id; });
    if (it == cs.end()) throw JsonError("No review comment " + std::to_string(id));
    if (action == "reply") {
        const std::string text = q.get("text").asString("");
        if (text.empty()) throw JsonError("A reply needs text");
        it->replies.push_back({q.get("author").asString(""), text});
    } else if (action == "resolve" || action == "reopen") {
        it->resolved = action == "resolve";
    } else if (action == "delete") {
        cs.erase(it);
    } else {
        throw JsonError("Unknown review action " + action);
    }
    return id;
}

std::string reviewMarkdown(const Project& p) {
    int open = 0;
    for (const auto& c : p.reviewComments) open += !c.resolved;
    std::string o = "# Design review — " + p.name + "\n\n" + std::to_string(open) + " open, " +
                    std::to_string(p.reviewComments.size() - static_cast<size_t>(open)) + " resolved\n";
    for (bool resolved : {false, true}) {
        bool heading = false;
        for (const auto& c : p.reviewComments) {
            if (c.resolved != resolved) continue;
            if (!heading) o += std::string("\n## ") + (resolved ? "Resolved" : "Open") + "\n", heading = true;
            o += "\n- **#" + std::to_string(c.id) + "**" + (c.ref.empty() ? "" : " " + c.ref) +
                 (c.hasAt ? " (" + c.view + " " + fmt(c.at.x) + ", " + fmt(c.at.y) + ")" : "") + " — " +
                 (c.author.empty() ? "" : c.author + ": ") + c.text + "\n";
            for (const auto& [who, text] : c.replies) o += "  - " + (who.empty() ? "" : who + ": ") + text + "\n";
        }
    }
    return o;
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
