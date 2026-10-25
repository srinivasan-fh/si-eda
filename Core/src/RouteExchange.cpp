// SiEDA Core — HyperLynx export and route import (PADS ASCII, HyperLynx, IPC-2581).
//
// Formats as their public descriptions give them: PADS Layout ASCII (Mentor's PADS 9.4 specification: corner lines
// "X Y LAYER WIDTH FLAGS [CW|CCW] [VIA]", a point's layer and width belonging to the segment that leaves it, layer 0 =
// unrouted, 65 = end at a pin, BASIC units = 1/38100 mil), HyperLynx BoardSim (ENGLISH = inches, METRIC = centimetres,
// Y up, ARC clockwise from point 1 to point 2) and IPC-2581 (LayerFeature sets of Line / Arc / Polyline, drill-layer
// holes as vias).
#include "sieda/RouteExchange.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <sstream>

#include "sieda/Stackup.hpp"
#include "XmlRead.hpp"

namespace sieda {

namespace {

// ------------------------------------------------------------------------------------------------ HyperLynx export

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.7f", v);
    return b;
}

std::string quoted(const std::string& s) {
    std::string o = "\"";
    for (char ch : s) o += ch == '"' ? '\'' : ch;
    return o + "\"";
}

// ------------------------------------------------------------------------------------------------ route import

/// One routed segment in the file's frame (mm, Y up) on file copper layer `layer` (0 = top … layers-1 = bottom).
struct Seg {
    std::string net;
    int layer = 0;
    double w = 0.25;
    Vec2 a, b;
    bool arc = false, cw = false;
    Vec2 c;
};

struct Hole {
    std::string net;
    Vec2 p;
    double drill = 0.3, dia = 0;
};

struct Routes {
    std::string format;
    int layers = 2;
    std::vector<Seg> segs;
    std::vector<Hole> vias;
    std::vector<std::pair<std::string, Vec2>> pins;  // "REF.PIN" and where the file has it
    std::string error;
};

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

bool same(Vec2 a, Vec2 b) { return std::abs(a.x - b.x) < 1e-6 && std::abs(a.y - b.y) < 1e-6; }

Routes readPads(const std::string& text) {
    Routes r;
    r.format = "pads";
    std::istringstream in(text);
    std::string line, section;
    std::getline(in, line);
    double scale = 0.0254;  // mils
    bool basic = false;
    {
        const std::string head = upper(line.substr(0, line.find('!', 1)));
        if (head.find("-BASIC") != std::string::npos) scale = 0.0254 / 38100, basic = true;
        else if (head.find("-MM") != std::string::npos || head.find("-METRIC") != std::string::npos) scale = 1;
        else if (head.find("-INCH") != std::string::npos) scale = 25.4;
    }
    struct ViaDef {
        double drill = 0, dia = 0;
    };
    std::map<std::string, ViaDef> viaDefs;
    std::string net, defaultVia;
    // The corner a segment leaves from: position, file layer (1-based), width.
    bool have = false;
    Vec2 prev;
    int prevLayer = 0;
    double prevWidth = 0;
    bool arcPending = false, arcCw = false;
    Vec2 centre;
    auto toLayer = [&](int l) { return l == r.layers ? r.layers - 1 : l - 1; };
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        std::istringstream ls(line);
        if (line[0] == '*') {
            std::string tag;
            ls >> tag;
            if (tag == "*SIGNAL*") {
                ls >> net;
                defaultVia.clear();
                for (std::string t; ls >> t;)
                    if (viaDefs.count(t)) defaultVia = t;
                have = arcPending = false;
            } else if (tag != "*REMARK*") {
                section = tag;
            }
            continue;
        }
        if (section == "*PCB*") {
            std::string key;
            ls >> key;
            if (key == "MAXIMUMLAYER") ls >> r.layers, r.layers = std::clamp(r.layers, 1, 64);
            if (key == "UNITS" && !basic) {
                int u = 0;
                ls >> u;
                scale = u == 1 ? 1 : u == 2 ? 25.4 : 0.0254;
            }
        } else if (section == "*VIA*") {
            std::string name;
            double drill = 0;
            int stack = 0;
            if (!(ls >> name >> drill >> stack) || std::isdigit(static_cast<unsigned char>(name[0])) || name[0] == '-') continue;
            ViaDef d{drill * scale, 0};
            for (int i = 0; i < stack && std::getline(in, line); ++i) {
                std::istringstream ss(line);
                int level = 0;
                double size = 0;
                std::string shape;
                if (ss >> level >> size >> shape && shape.back() != 'A') d.dia = std::max(d.dia, size * scale);
            }
            viaDefs[name] = d;
        } else if (section == "*ROUTE*" && !net.empty()) {
            if (!std::isdigit(static_cast<unsigned char>(line[0])) && line[0] != '-' && line[0] != '+') {
                have = arcPending = false;  // a pin pair: the next corners start a new run
                continue;
            }
            double x = 0, y = 0, w = 0;
            int layer = 0, flags = 0;
            if (!(ls >> x >> y >> layer >> w >> flags)) continue;
            const Vec2 p{x * scale, y * scale};
            std::string via, dir;
            for (std::string t; ls >> t;) {
                if (t == "CW" || t == "CCW") dir = t;
                else if (viaDefs.count(t)) via = t;
            }
            if (!dir.empty()) {  // the arc's centre: the arc runs from the previous corner to the next
                arcPending = true, arcCw = dir == "CW", centre = p;
                continue;
            }
            const bool copper = [&](int l) { return l >= 1 && l <= r.layers; }(prevLayer);
            if (have && copper && !same(prev, p)) {
                Seg s{net, toLayer(prevLayer), prevWidth * scale, prev, p, arcPending, arcCw, centre};
                r.segs.push_back(s);
            }
            if (via.empty() && have && same(prev, p) && copper && layer >= 1 && layer <= r.layers && layer != prevLayer)
                via = defaultVia.empty() && !viaDefs.empty() ? viaDefs.begin()->first : defaultVia;
            if (!via.empty()) {
                const ViaDef& d = viaDefs[via];
                r.vias.push_back({net, p, d.drill > 0 ? d.drill : 0.3, d.dia});
            }
            arcPending = false;
            have = layer != 65;
            prev = p, prevLayer = layer, prevWidth = w;
        }
    }
    if (r.segs.empty() && r.vias.empty()) r.error = "no routes found (a PADS ASCII file needs a *ROUTE* section)";
    return r;
}

/// HyperLynx tokens: punctuation { } ( ) = , as their own tokens, quoted strings without their quotes.
std::vector<std::string> hypTokens(const std::string& s) {
    std::vector<std::string> t;
    for (size_t i = 0; i < s.size();) {
        const char ch = s[i];
        if (std::isspace(static_cast<unsigned char>(ch))) {
            ++i;
        } else if (std::string("{}()=,").find(ch) != std::string::npos) {
            t.emplace_back(1, ch), ++i;
        } else if (ch == '"') {
            const size_t e = s.find('"', i + 1);
            t.push_back(s.substr(i + 1, (e == std::string::npos ? s.size() : e) - i - 1));
            i = e == std::string::npos ? s.size() : e + 1;
        } else {
            size_t e = i;
            while (e < s.size() && !std::isspace(static_cast<unsigned char>(s[e])) && std::string("{}()=,\"").find(s[e]) == std::string::npos) ++e;
            t.push_back(s.substr(i, e - i)), i = e;
        }
    }
    return t;
}

Routes readHyperlynx(const std::string& text) {
    Routes r;
    r.format = "hyperlynx";
    const auto t = hypTokens(text);
    double scale = 25.4;  // ENGLISH: inches
    std::vector<std::string> layerNames;
    struct Stack {
        double drill = 0, dia = 0;
    };
    std::map<std::string, Stack> stacks;
    std::string net, block;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == "{" && i + 1 < t.size()) {
            block = upper(t[i + 1]);
            if (block == "UNITS" && i + 3 < t.size()) scale = upper(t[i + 3]) == "METRIC" ? 10 : 25.4;
            if (block == "NET" && i + 3 < t.size()) net = t[i + 3];
            if (block == "PADSTACK" && i + 3 < t.size()) {
                Stack& st = stacks[t[i + 3]];
                size_t k = i + 4;
                if (k + 1 < t.size() && t[k] == ",") st.drill = std::atof(t[k + 1].c_str()) * scale;
                // Entries ( layer , shape , sx , sy , angle [, M | A | thermal …] ): metal pads give the diameter.
                for (; k < t.size() && t[k] != "}"; ++k) {
                    if (t[k] != "(") continue;
                    std::vector<std::string> f;
                    for (++k; k < t.size() && t[k] != ")" && t[k] != "}"; ++k)
                        if (t[k] != ",") f.push_back(t[k]);
                    if (f.size() >= 5 && (f.size() == 5 || upper(f[5]) == "M"))
                        st.dia = std::max(st.dia, std::atof(f[2].c_str()) * scale);
                    if (k < t.size() && t[k] == "}") break;
                }
            }
            continue;
        }
        if (t[i] != "(" || i + 1 >= t.size()) continue;
        const std::string kind = upper(t[i + 1]);
        std::map<std::string, std::string> kv;
        size_t k = i + 2;
        for (; k + 2 < t.size() && t[k] != ")"; ++k)
            if (t[k + 1] == "=") kv[upper(t[k])] = t[k + 2], k += 2;
        auto v = [&](const char* key) { return std::atof(kv[key].c_str()) * scale; };
        if (block == "STACKUP" && (kind == "SIGNAL" || kind == "PLANE")) layerNames.push_back(kv["L"]);
        if (block != "NET") continue;
        auto layerOf = [&](const std::string& name) {
            const auto it = std::find(layerNames.begin(), layerNames.end(), name);
            return it == layerNames.end() ? -1 : static_cast<int>(it - layerNames.begin());
        };
        if (kind == "SEG" || kind == "ARC") {
            Seg s{net, layerOf(kv["L"]), v("W"), {v("X1"), v("Y1")}, {v("X2"), v("Y2")}, kind == "ARC", true, {v("XC"), v("YC")}};
            if (s.layer >= 0) r.segs.push_back(s);
        } else if (kind == "PIN") {
            r.pins.push_back({kv["R"], {v("X"), v("Y")}});
        } else if (kind == "VIA") {
            const Stack& st = stacks[kv["P"]];
            r.vias.push_back({net, {v("X"), v("Y")}, st.drill > 0 ? st.drill : 0.3, st.dia});
        }
    }
    r.layers = std::max<int>(1, static_cast<int>(layerNames.size()));
    if (r.segs.empty() && r.vias.empty()) r.error = "no routes found (a HyperLynx file needs NET records with SEG / ARC / VIA)";
    return r;
}

Routes readIpc2581(const std::string& text) {
    Routes r;
    r.format = "ipc2581";
    XmlNode root;
    if (!parseXml(text, root)) return r.error = "not well-formed XML", r;
    auto unitScale = [](const std::string& u) {
        const std::string s = upper(u);
        return s == "INCH" ? 25.4 : s == "MICRON" ? 0.001 : s == "MILS" || s == "MIL" ? 0.0254 : 1.0;
    };
    auto num = [](const XmlNode& n, const char* key) {
        const auto it = n.attrs.find(key);
        return it == n.attrs.end() ? 0.0 : std::atof(it->second.c_str());
    };
    auto attr = [](const XmlNode& n, const char* key) {
        const auto it = n.attrs.find(key);
        return it == n.attrs.end() ? std::string() : it->second;
    };
    double scale = 1, lineScale = 1, stdScale = 1;
    std::map<std::string, double> lineWidths, circles;
    std::vector<std::string> copper;
    std::set<std::string> drills;
    std::vector<const XmlNode*> todo{&root}, features;
    while (!todo.empty()) {
        const XmlNode* n = todo.back();
        todo.pop_back();
        for (auto it = n->kids.rbegin(); it != n->kids.rend(); ++it) todo.push_back(&*it);
        if (n->name == "CadHeader") scale = unitScale(attr(*n, "units"));
        else if (n->name == "DictionaryLineDesc") lineScale = unitScale(attr(*n, "units"));
        else if (n->name == "DictionaryStandard") stdScale = unitScale(attr(*n, "units"));
        else if (n->name == "EntryLineDesc" && n->child("LineDesc")) lineWidths[attr(*n, "id")] = num(*n->child("LineDesc"), "lineWidth") * lineScale;
        else if (n->name == "EntryStandard" && n->child("Circle")) circles[attr(*n, "id")] = num(*n->child("Circle"), "diameter") * stdScale;
        else if (n->name == "Layer") {
            const std::string fn = upper(attr(*n, "layerFunction"));
            if (fn == "CONDUCTOR" || fn == "SIGNAL" || fn == "PLANE" || fn == "MIXED" || fn == "POWER_GROUND") copper.push_back(attr(*n, "name"));
            if (fn == "DRILL") drills.insert(attr(*n, "name"));
        } else if (n->name == "LayerFeature") {
            features.push_back(n);
        }
    }
    r.layers = std::max<int>(1, static_cast<int>(copper.size()));
    std::vector<std::pair<Vec2, double>> viaPads;  // conductor pads with no pin: via lands
    std::vector<Vec2> pins;
    for (const XmlNode* lf : features) {
        const std::string layerName = attr(*lf, "layerRef");
        const int layer = static_cast<int>(std::find(copper.begin(), copper.end(), layerName) - copper.begin());
        const bool drill = drills.count(layerName) > 0;
        if (layer >= r.layers && !drill) continue;
        for (const auto& set : lf->kids) {
            if (set.name != "Set") continue;
            const std::string net = attr(set, "net");
            std::vector<const XmlNode*> items{&set};
            while (!items.empty()) {
                const XmlNode* n = items.back();
                items.pop_back();
                for (const auto& k : n->kids) items.push_back(&k);
                auto width = [&] {
                    if (const XmlNode* d = n->child("LineDesc")) return num(*d, "lineWidth") * scale;
                    if (const XmlNode* d = n->child("LineDescRef")) return lineWidths[attr(*d, "id")];
                    return 0.0;
                };
                if (drill && n->name == "Hole" && upper(attr(*n, "platingStatus")) != "NONPLATED")
                    r.vias.push_back({net, {num(*n, "x") * scale, num(*n, "y") * scale}, num(*n, "diameter") * scale, 0});
                if (drill || net.empty()) continue;
                if (n->name == "Line" || n->name == "Arc") {
                    Seg s{net, layer, width(), {num(*n, "startX") * scale, num(*n, "startY") * scale}, {num(*n, "endX") * scale, num(*n, "endY") * scale},
                          n->name == "Arc", attr(*n, "clockwise") == "true", {num(*n, "centerX") * scale, num(*n, "centerY") * scale}};
                    r.segs.push_back(s);
                } else if (n->name == "Polyline" && n->child("PolyBegin")) {
                    Vec2 at{num(*n->child("PolyBegin"), "x") * scale, num(*n->child("PolyBegin"), "y") * scale};
                    for (const auto& step : n->kids) {
                        if (step.name != "PolyStepSegment" && step.name != "PolyStepCurve") continue;
                        const Vec2 to{num(step, "x") * scale, num(step, "y") * scale};
                        r.segs.push_back({net, layer, width(), at, to, step.name == "PolyStepCurve", attr(step, "clockwise") == "true",
                                          {num(step, "centerX") * scale, num(step, "centerY") * scale}});
                        at = to;
                    }
                } else if (n->name == "Pad" && n->child("Location")) {
                    const Vec2 at{num(*n->child("Location"), "x") * scale, num(*n->child("Location"), "y") * scale};
                    if (const XmlNode* pin = n->child("PinRef")) pins.push_back(at), r.pins.push_back({attr(*pin, "componentRef") + "." + attr(*pin, "pin"), at});
                    else if (const XmlNode* ref = n->child("StandardPrimitiveRef"); ref && circles.count(attr(*ref, "id")))
                        viaPads.push_back({at, circles[attr(*ref, "id")]});
                }
            }
        }
    }
    // A plated hole at a pin is the pin's own; any other is a via, its land the via pad found there.
    std::vector<Hole> vias;
    for (auto& h : r.vias) {
        if (std::any_of(pins.begin(), pins.end(), [&](Vec2 p) { return same(p, h.p); })) continue;
        for (const auto& [at, d] : viaPads)
            if (same(at, h.p)) h.dia = std::max(h.dia, d);
        vias.push_back(h);
    }
    r.vias = std::move(vias);
    if (r.segs.empty() && r.vias.empty()) r.error = "no routes found (IPC-2581 LayerFeature sets with Line / Arc / Polyline)";
    return r;
}

/// A point halfway along the arc from a to b round c, clockwise or not (Y up).
Vec2 arcMid(Vec2 a, Vec2 b, Vec2 c, bool cw) {
    const double a0 = std::atan2(a.y - c.y, a.x - c.x), a1 = std::atan2(b.y - c.y, b.x - c.x);
    double sweep = a1 - a0;
    if (cw) while (sweep >= 0) sweep -= 2 * M_PI;
    else while (sweep <= 0) sweep += 2 * M_PI;
    const double r = std::hypot(a.x - c.x, a.y - c.y), m = a0 + sweep / 2;
    return {c.x + r * std::cos(m), c.y + r * std::sin(m)};
}

}  // namespace

std::string exportHyperlynx(const Project& project) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    const int n = std::max(1, s.layerCount);
    const double H = s.height;
    auto in = [](double mm) { return num(mm / 25.4); };
    auto xy = [&](const char* x, const char* y, Vec2 v) { return std::string(" ") + x + "=" + in(v.x) + " " + y + "=" + in(H - v.y); };
    auto layerName = [&](int l) { return quoted(copperLayerName(l, n)); };
    std::ostringstream o;
    o << "{BOARD_FILE}\n{VERSION=2.14}\n{UNITS=ENGLISH LENGTH}\n{PLANE_SEP=" << in(s.clearance) << "}\n\n{BOARD\n";
    s.forEachEdge([&](Vec2 a, Vec2 b) { o << "  (PERIMETER_SEGMENT" << xy("X1", "Y1", a) << xy("X2", "Y2", b) << ")\n"; });
    o << "}\n\n{STACKUP\n";
    const LaminateMaterial& lam = boardLaminate(s);
    for (int l = 0; l < n; ++l) {
        const bool plane = std::any_of(pcb.zones.begin(), pcb.zones.end(), [&](const CopperZone& z) { return z.plane && z.layer == l; });
        o << "  (" << (plane ? "PLANE" : "SIGNAL") << " T=" << in(copperThickness(s)) << " L=" << layerName(l) << " M=COPPER)\n";
        if (l + 1 < n)
            o << "  (DIELECTRIC T=" << in(dielectricBelow(s, l)) << " C=" << lam.er << " LT=" << lam.lossTangent << " L=\"DE" << l + 1
              << "\" M=" << quoted(lam.name) << ")\n";
    }
    o << "}\n\n{DEVICES\n";
    for (const auto& c : sch.components())
        if (c.hasFootprint() && c.pcb.placed)
            o << "  (? REF=" << quoted(c.ref) << (c.value.empty() ? "" : " VAL=" + quoted(c.value)) << " L="
              << layerName(c.pcb.bottom ? n - 1 : 0) << ")\n";
    o << "}\n\n";
    // Pad stacks: one per distinct land (shape, size, drill, layer); vias by drill and diameter.
    std::map<std::string, int> stackIds;
    std::ostringstream stacks;
    auto stack = [&](const std::string& body, double drill) {
        const std::string key = num(drill) + body;
        if (auto it = stackIds.find(key); it != stackIds.end()) return it->second;
        const int id = static_cast<int>(stackIds.size()) + 1;
        stackIds[key] = id;
        stacks << "{PADSTACK=" << id << (drill > 0 ? ", " + in(drill) : std::string()) << "\n" << body << "}\n\n";
        return id;
    };
    auto land = [&](const std::string& layer, int shape, double w, double h) {
        return "  (" + layer + ", " + std::to_string(shape) + ", " + in(w) + ", " + in(h) + ", 0, M)\n";
    };
    const auto pads = pcb.pads(sch);
    std::vector<int> padStack(pads.size());
    for (size_t i = 0; i < pads.size(); ++i) {
        const Pad& p = pads[i];
        const int shape = p.round ? 0 : p.corner > 0 ? 2 : 1;
        padStack[i] = stack(land(p.throughHole ? "MDEF" : layerName(p.smdLayer), shape, p.size.x, p.size.y), p.throughHole ? p.drill : 0);
    }
    std::map<int, int> viaStack;
    for (const auto& v : pcb.vias) {
        std::string body;
        if (v.isThrough()) body = land("MDEF", 0, v.diameter, v.diameter);
        else
            for (int l = std::max(0, v.fromLayer); l <= v.lastLayer(n); ++l) body += land(layerName(l), 0, v.diameter, v.diameter);
        viaStack[v.id] = stack(body, v.drill);
    }
    o << stacks.str();
    // Nets: pins, tracks, arcs, vias and pours of each.
    int polygonId = 0;
    for (const auto& net : sch.nets()) {
        std::ostringstream b;
        for (size_t i = 0; i < pads.size(); ++i)
            if (pads[i].net == net.index)
                if (const Component* c = sch.find(pads[i].componentId))
                    b << "  (PIN" << xy("X", "Y", pads[i].position) << " R=" << quoted(c->ref + "." + std::to_string(pads[i].padNumber))
                      << " P=" << padStack[i] << ")\n";
        for (const auto& t : pcb.tracks) {
            if (t.net != net.index) continue;
            if (!t.arc) {
                b << "  (SEG" << xy("X1", "Y1", t.a) << xy("X2", "Y2", t.b) << " W=" << in(t.width) << " L=" << layerName(t.layer) << ")\n";
                continue;
            }
            // Centre of the circle through a, mid, b; HyperLynx draws ARC clockwise (Y up) from point 1 to point 2.
            const Vec2 a{t.a.x, H - t.a.y}, m{t.mid.x, H - t.mid.y}, e{t.b.x, H - t.b.y};
            const double d = 2 * (a.x * (m.y - e.y) + m.x * (e.y - a.y) + e.x * (a.y - m.y));
            if (std::abs(d) < 1e-12) {
                b << "  (SEG" << xy("X1", "Y1", t.a) << xy("X2", "Y2", t.b) << " W=" << in(t.width) << " L=" << layerName(t.layer) << ")\n";
                continue;
            }
            auto sq = [](Vec2 p) { return p.x * p.x + p.y * p.y; };
            const Vec2 c{(sq(a) * (m.y - e.y) + sq(m) * (e.y - a.y) + sq(e) * (a.y - m.y)) / d,
                         (sq(a) * (e.x - m.x) + sq(m) * (a.x - e.x) + sq(e) * (m.x - a.x)) / d};
            const bool ccw = (m.x - a.x) * (e.y - m.y) - (m.y - a.y) * (e.x - m.x) > 0;
            const Vec2 p1 = ccw ? t.b : t.a, p2 = ccw ? t.a : t.b;
            b << "  (ARC" << xy("X1", "Y1", p1) << xy("X2", "Y2", p2) << xy("XC", "YC", {c.x, H - c.y}) << " R="
              << in(std::hypot(a.x - c.x, a.y - c.y)) << " W=" << in(t.width) << " L=" << layerName(t.layer) << ")\n";
        }
        for (const auto& v : pcb.vias)
            if (v.net == net.index) b << "  (VIA" << xy("X", "Y", v.position) << " P=" << viaStack[v.id] << ")\n";
        for (const auto& z : pcb.zones) {
            if (z.net != net.name) continue;
            const auto poly = z.polygon.size() >= 3 ? z.polygon : s.outlinePolygon();
            b << "  {POLYGON T=POUR L=" << layerName(z.layer) << " ID=" << polygonId++ << xy("X", "Y", poly[0]) << "\n";
            for (size_t i = 1; i <= poly.size(); ++i) b << "    (LINE" << xy("X", "Y", poly[i % poly.size()]) << ")\n";
            b << "  }\n";
        }
        if (!b.str().empty()) o << "{NET=" << quoted(net.name.empty() ? "N" + std::to_string(net.index) : net.name) << "\n" << b.str() << "}\n\n";
    }
    o << "{END}\n";
    return o.str();
}

Json importRoutes(Project& project, const std::string& text, bool replace) {
    const size_t start = text.find_first_not_of(" \t\r\n\xEF\xBB\xBF");
    Routes r;
    if (start != std::string::npos && text.compare(start, 6, "!PADS-") == 0) r = readPads(text);
    else if (text.find("<IPC-2581") != std::string::npos) r = readIpc2581(text);
    else if (text.find("{NET") != std::string::npos || text.find("{VERSION") != std::string::npos) r = readHyperlynx(text);
    else r.error = "unknown format: expected PADS ASCII (!PADS-…), HyperLynx (.hyp) or IPC-2581 XML";
    Json out = Json::object();
    if (!r.error.empty()) {
        out["error"] = r.error;
        return out;
    }
    const Schematic& sch = project.schematic;
    PcbLayout& pcb = project.pcb;
    const int bottom = pcb.settings.bottomLayer();
    std::map<std::string, int> byName, byUpper;
    for (const auto& n : sch.nets())
        if (!n.name.empty()) byName[n.name] = n.index, byUpper[upper(n.name)] = n.index;
    auto netOf = [&](const std::string& name) {
        if (auto it = byName.find(name); it != byName.end()) return it->second;
        auto it = byUpper.find(upper(name));
        return it == byUpper.end() ? -1 : it->second;
    };
    // File layer → board layer: top and bottom by position, inner ones by number while the board has them.
    auto layerOf = [&](int l) { return l == 0 ? 0 : l == r.layers - 1 ? bottom : l < bottom ? l : -1; };
    const auto pads = pcb.pads(sch);
    std::map<int, std::vector<const Pad*>> netPads;
    for (const auto& p : pads)
        if (p.net >= 0) netPads[p.net].push_back(&p);
    // Fit by named pins when the file has them (HyperLynx PIN records, IPC-2581 PinRef pads): the offset most of
    // them agree on (0.01 mm bins).
    std::map<std::string, Vec2> padAt;
    for (const auto& p : pads)
        if (const Component* c = sch.find(p.componentId)) padAt[c->ref + "." + std::to_string(p.padNumber)] = p.position;
    std::map<std::pair<long, long>, std::pair<int, Vec2>> pinVotes;
    int matchedPins = 0;
    for (const auto& [name, at] : r.pins)
        if (auto it = padAt.find(name); it != padAt.end()) {
            const Vec2 d{it->second.x - at.x, it->second.y + at.y};
            auto& v = pinVotes[{std::lround(d.x * 100), std::lround(d.y * 100)}];
            ++v.first, v.second.x += d.x, v.second.y += d.y, ++matchedPins;
        }
    // Fit: offset votes from every segment end (Y flipped) to every pad of its net on its layer.
    std::map<std::pair<long, long>, std::pair<int, Vec2>> votes;
    size_t pairs = 0;
    for (const auto& sg : r.segs) {
        const int net = netOf(sg.net), layer = layerOf(sg.layer);
        if (net < 0 || layer < 0 || !netPads.count(net)) continue;
        for (Vec2 e : {sg.a, sg.b})
            for (const Pad* p : netPads[net]) {
                if (!p->onLayer(layer) || ++pairs > 4000000) continue;
                const Vec2 d{p->position.x - e.x, p->position.y + e.y};
                auto& v = votes[{std::lround(d.x * 100), std::lround(d.y * 100)}];
                ++v.first, v.second.x += d.x, v.second.y += d.y;
            }
    }
    // Otherwise candidates (this board's own export frame first, then the 200 most voted) scored by the ends that land
    // inside a pad of their net, then by those exactly on a pad centre (PADS runs start and end on pin centres): pads on
    // a regular grid give chance votes, landing on the right pads does not happen by chance. Every voted offset within
    // 1 mm of the winner is scored too (ends inside large pads leave a plateau of near offsets).
    std::vector<std::pair<int, Vec2>> ranked;
    for (const auto& [key, v] : votes) ranked.push_back({v.first, {v.second.x / v.first, v.second.y / v.first}});
    std::sort(ranked.begin(), ranked.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    std::vector<Vec2> candidates{{0, pcb.settings.height}};
    for (size_t i = 0; i < ranked.size() && i < 200; ++i) candidates.push_back(ranked[i].second);
    auto score = [&](Vec2 o) {
        std::pair<int, int> n{0, 0};
        for (const auto& sg : r.segs) {
            const int net = netOf(sg.net), layer = layerOf(sg.layer);
            if (net < 0 || layer < 0 || !netPads.count(net)) continue;
            for (Vec2 e : {sg.a, sg.b}) {
                const Vec2 q{e.x + o.x, o.y - e.y};
                for (const Pad* pd : netPads[net])
                    if (pd->onLayer(layer) && std::abs(q.x - pd->position.x) <= pd->size.x / 2 + 1e-6 &&
                        std::abs(q.y - pd->position.y) <= pd->size.y / 2 + 1e-6) {
                        ++n.first, n.second += std::hypot(q.x - pd->position.x, q.y - pd->position.y) < 1e-3;
                        break;
                    }
            }
        }
        return n;
    };
    Vec2 off = candidates[0];
    auto best = score(off);
    for (size_t i = 1; i < candidates.size(); ++i)
        if (const auto n = score(candidates[i]); n > best) best = n, off = candidates[i];
    const Vec2 coarse = off;
    for (const auto& [count, at] : ranked)
        if (std::abs(at.x - coarse.x) <= 1 && std::abs(at.y - coarse.y) <= 1)
            if (const auto n = score(at); n > best) best = n, off = at;
    for (const auto& [key, v] : pinVotes)
        if (v.first * 2 > matchedPins) off = {v.second.x / v.first, v.second.y / v.first};
    auto place = [&](Vec2 v) { return Vec2{v.x + off.x, off.y - v.y}; };
    if (replace) pcb.clearRouting();
    std::set<std::string> unknown, nets;
    int tracks = 0, vias = 0, ends = 0, hits = 0, offLayer = 0;
    for (const auto& sg : r.segs) {
        const int net = netOf(sg.net), layer = layerOf(sg.layer);
        if (net < 0) {
            unknown.insert(sg.net);
            continue;
        }
        if (layer < 0) {
            ++offLayer;
            continue;
        }
        Track t;
        t.net = net, t.layer = layer, t.width = sg.w > 0 ? sg.w : pcb.settings.trackWidth;
        t.a = place(sg.a), t.b = place(sg.b);
        if (sg.arc) t.arc = true, t.mid = place(arcMid(sg.a, sg.b, sg.c, sg.cw));
        for (Vec2 e : {t.a, t.b}) {
            ++ends;
            for (const Pad* p : netPads[net])
                if (p->onLayer(layer) && std::abs(e.x - p->position.x) <= p->size.x / 2 && std::abs(e.y - p->position.y) <= p->size.y / 2) {
                    ++hits;
                    break;
                }
        }
        pcb.addTrack(t);
        ++tracks, nets.insert(sch.nets()[static_cast<size_t>(net)].name);
    }
    for (const auto& h : r.vias) {
        const int net = netOf(h.net);
        if (net < 0) {
            if (!h.net.empty()) unknown.insert(h.net);
            continue;
        }
        Via v;
        v.net = net, v.position = place(h.p), v.drill = h.drill;
        v.diameter = h.dia > h.drill ? h.dia : h.drill + 2 * std::max(0.1, pcb.settings.viaDiameter / 2 - pcb.settings.viaDrill / 2);
        pcb.addVia(v);
        ++vias;
    }
    out["format"] = r.format;
    out["tracks"] = tracks;
    out["vias"] = vias;
    out["nets"] = static_cast<int>(nets.size());
    Json u = Json::array();
    for (const auto& n : unknown) u.push(n);
    out["unknownNets"] = u;
    Json o = Json::object();
    o["x"] = off.x, o["y"] = off.y;
    out["offset"] = o;
    out["ends"] = ends;
    out["padHits"] = hits;
    if (offLayer) out["offLayer"] = offLayer;
    return out;
}

}  // namespace sieda
