#include "sieda/Mechanical.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <map>
#include <optional>
#include <set>
#include <sstream>

#include "sieda/Embedded.hpp"
#include "sieda/Library.hpp"

namespace sieda {

namespace {

constexpr double kPi = 3.14159265358979323846;

/// Number with a decimal point and no trailing zeros ("1.", "0.25", "-3.2").
std::string num(double v) {
    if (std::fabs(v) < 5e-7) v = 0;
    char b[32];
    std::snprintf(b, sizeof b, "%.6f", v);
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    return s;
}

std::string quoted(std::string s) {
    for (auto& ch : s)
        if (ch == '\'' || ch == '"' || ch == '\n') ch = '_';
    return s;
}

double signedArea(const std::vector<Vec2>& p) {
    double a = 0;
    for (size_t i = 0; i < p.size(); ++i) a += p[i].x * p[(i + 1) % p.size()].y - p[(i + 1) % p.size()].x * p[i].y;
    return a / 2;
}

/// Polygon in MCAD XY (Y up), counter-clockwise when `ccw`.
std::vector<Vec2> mcad(const std::vector<Vec2>& board, bool ccw) {
    std::vector<Vec2> p;
    for (const auto& v : board) p.push_back({v.x, -v.y});
    if ((signedArea(p) > 0) != ccw) std::reverse(p.begin(), p.end());
    return p;
}

std::vector<Vec2> circle(Vec2 c, double r, int n) {
    std::vector<Vec2> p;
    for (int i = 0; i < n; ++i) p.push_back({c.x + r * std::cos(2 * kPi * i / n), c.y + r * std::sin(2 * kPi * i / n)});
    return p;
}

std::vector<Vec2> boardOutline(const BoardSettings& s) {
    if (s.hasCustomOutline()) return s.outline;
    return {{0, 0}, {s.width, 0}, {s.width, s.height}, {0, s.height}};
}

/// A placed part's package body in board coordinates: outline (rectangle or cylinder), height, colour.
struct Body {
    const Component* c;
    std::vector<Vec2> outline;
    double height;
    float r, g, b;
    bool round = false;
    Rect box() const {
        Rect r{outline[0].x, outline[0].y, outline[0].x, outline[0].y};
        for (const auto& v : outline) r = Rect(std::min(r.x0, v.x), std::min(r.y0, v.y), std::max(r.x1, v.x), std::max(r.y1, v.y));
        return r;
    }
};

std::vector<Body> partBodies(const Schematic& sch, const PcbLayout& pcb) {
    std::vector<Body> out;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || embeddedElement(c, pcb.settings)) continue;
        const FootprintDef* fp = Library::instance().footprint(c.footprintName());
        BodyDef b = fp ? fp->body : BodyDef{};
        double w = b.width, d = b.depth;
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1) std::swap(w, d);
        if (w <= 0 || d <= 0) {  // no body: the courtyard, 1 mm high
            const Rect r = pcb.courtyard(c);
            w = r.width();
            d = r.height();
            b.height = 1.0;
        }
        if (b.height <= 0) b.height = 1.0;
        const Vec2 p = c.pcb.position;
        Body body{&c, {}, b.height, b.r, b.g, b.b, b.cylinder};
        body.outline = b.cylinder ? circle(p, std::min(w, d) / 2, 24)
                                  : std::vector<Vec2>{{p.x - w / 2, p.y - d / 2}, {p.x + w / 2, p.y - d / 2},
                                                      {p.x + w / 2, p.y + d / 2}, {p.x - w / 2, p.y + d / 2}};
        out.push_back(std::move(body));
    }
    return out;
}

/// ISO 10303-21 writer for faceted B-rep prisms.
struct Step {
    std::ostringstream o;
    int next = 1;

    int add(const std::string& entity) {
        o << '#' << next << '=' << entity << ";\n";
        return next++;
    }
    std::string ref(int id) const { return "#" + std::to_string(id); }
    int point(double x, double y, double z) { return add("CARTESIAN_POINT('',(" + num(x) + "," + num(y) + "," + num(z) + "))"); }
    int dir(double x, double y, double z) { return add("DIRECTION('',(" + num(x) + "," + num(y) + "," + num(z) + "))"); }
    int placement(double x, double y, double z, double nx, double ny, double nz, double rx, double ry, double rz) {
        const int p = point(x, y, z), n = dir(nx, ny, nz), r = dir(rx, ry, rz);
        return add("AXIS2_PLACEMENT_3D(''," + ref(p) + "," + ref(n) + "," + ref(r) + ")");
    }
    std::string loop(const std::vector<int>& pts) {
        std::string s = "POLY_LOOP('',(";
        for (size_t i = 0; i < pts.size(); ++i) s += (i ? "," : "") + ref(pts[i]);
        return s + "))";
    }
    /// Planar face through `origin` with outward normal n (unit) and in-plane direction r.
    int face(const std::vector<std::vector<int>>& loops, Vec3 origin, Vec3 n, Vec3 r) {
        std::string bounds;
        for (size_t i = 0; i < loops.size(); ++i) {
            const int l = add(loop(loops[i]));
            bounds += (i ? "," : "") + ref(add(std::string(i ? "FACE_BOUND" : "FACE_OUTER_BOUND") + "(''," + ref(l) + ",.T.)"));
        }
        const int pl = add("PLANE(''," + ref(placement(origin.x, origin.y, origin.z, n.x, n.y, n.z, r.x, r.y, r.z)) + ")");
        return add("FACE_SURFACE('',(" + bounds + ")," + ref(pl) + ",.T.)");
    }
    /// Closed prism: CCW outline (MCAD XY) with CW holes, from z0 to z1. Returns the FACETED_BREP.
    int prism(const std::string& name, const std::vector<Vec2>& outline, const std::vector<std::vector<Vec2>>& holes,
              double z0, double z1) {
        std::vector<int> faces;
        std::vector<std::vector<int>> top, bottom;
        auto ring = [&](const std::vector<Vec2>& poly) {
            std::vector<int> lo, hi;
            for (const auto& v : poly) lo.push_back(point(v.x, v.y, z0));
            for (const auto& v : poly) hi.push_back(point(v.x, v.y, z1));
            top.push_back(hi);
            bottom.emplace_back(lo.rbegin(), lo.rend());
            for (size_t i = 0; i < poly.size(); ++i) {
                const size_t j = (i + 1) % poly.size();
                const Vec2 e = poly[j] - poly[i];
                const double len = std::hypot(e.x, e.y);
                if (len < 1e-9) continue;
                faces.push_back(face({{lo[i], lo[j], hi[j], hi[i]}}, {poly[i].x, poly[i].y, z0},
                                     {e.y / len, -e.x / len, 0}, {e.x / len, e.y / len, 0}));
            }
        };
        ring(outline);
        for (const auto& h : holes) ring(h);
        const Vec2 o = outline.front();
        faces.push_back(face(top, {o.x, o.y, z1}, {0, 0, 1}, {1, 0, 0}));
        faces.push_back(face(bottom, {o.x, o.y, z0}, {0, 0, -1}, {1, 0, 0}));
        std::string list;
        for (size_t i = 0; i < faces.size(); ++i) list += (i ? "," : "") + ref(faces[i]);
        const int shell = add("CLOSED_SHELL('',(" + list + "))");
        return add("FACETED_BREP('" + quoted(name) + "'," + ref(shell) + ")");
    }
    int styled(int item, float r, float g, float b) {
        const int col = add("COLOUR_RGB(''," + num(r) + "," + num(g) + "," + num(b) + ")");
        const int fill = add("FILL_AREA_STYLE('',(" + ref(add("FILL_AREA_STYLE_COLOUR(''," + ref(col) + ")")) + "))");
        const int side = add("SURFACE_SIDE_STYLE('',(" + ref(add("SURFACE_STYLE_FILL_AREA(" + ref(fill) + ")")) + "))");
        const int usage = add("SURFACE_STYLE_USAGE(.BOTH.," + ref(side) + ")");
        return add("STYLED_ITEM('',(" + ref(add("PRESENTATION_STYLE_ASSIGNMENT((" + ref(usage) + "))")) + ")," + ref(item) + ")");
    }
};

std::string idfDate() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y/%m/%d.%H:%M:%S", &tm);
    return b;
}

/// IDF rotation (counter-clockwise, Y up) of a part's rotation (board view, Y down).
int idfRotation(int rotation) { return ((360 - rotation % 360) % 360 + 360) % 360; }

std::string idfPartNumber(const Component& c) {
    return quoted(!c.sourcing.mpn.empty() ? c.sourcing.mpn : !c.value.empty() ? c.value : c.footprintName());
}

void idfLoop(std::ostringstream& o, int label, std::vector<Vec2> p) {
    p.push_back(p.front());  // IDF loops are closed by repeating the first point
    for (const auto& v : p) o << label << ' ' << num(v.x) << ' ' << num(v.y) << " 0\n";
}

/// Whitespace-separated tokens; "quoted strings" stay one token.
std::vector<std::string> tokens(const std::string& line) {
    std::vector<std::string> t;
    for (size_t i = 0; i < line.size();) {
        if (std::isspace(static_cast<unsigned char>(line[i]))) {
            ++i;
        } else if (line[i] == '"') {
            const size_t e = line.find('"', i + 1);
            t.push_back(line.substr(i + 1, (e == std::string::npos ? line.size() : e) - i - 1));
            i = e == std::string::npos ? line.size() : e + 1;
        } else {
            size_t e = i;
            while (e < line.size() && !std::isspace(static_cast<unsigned char>(line[e]))) ++e;
            t.push_back(line.substr(i, e - i));
            i = e;
        }
    }
    return t;
}

/// Do two bodies' footprints overlap (more than touching)? Cylinders are circles, everything else rectangles.
bool bodiesOverlap(const Body& a, const Body& b) {
    const Rect ra = a.box(), rb = b.box();
    constexpr double e = 1e-3;
    if (ra.x1 <= rb.x0 + e || rb.x1 <= ra.x0 + e || ra.y1 <= rb.y0 + e || rb.y1 <= ra.y0 + e) return false;
    auto circleRect = [&](const Rect& c, const Rect& r) {
        const Vec2 m{(c.x0 + c.x1) / 2, (c.y0 + c.y1) / 2};
        const double dx = m.x - std::clamp(m.x, r.x0, r.x1), dy = m.y - std::clamp(m.y, r.y0, r.y1);
        return std::hypot(dx, dy) < c.width() / 2 - e;
    };
    if (a.round && b.round)
        return std::hypot((ra.x0 + ra.x1 - rb.x0 - rb.x1) / 2, (ra.y0 + ra.y1 - rb.y0 - rb.y1) / 2) <
               (ra.width() + rb.width()) / 2 - e;
    if (a.round) return circleRect(ra, rb);
    if (b.round) return circleRect(rb, ra);
    return true;
}

}  // namespace

std::vector<RuleViolation> mechanicalChecks(const Schematic& sch, const PcbLayout& pcb) {
    const BoardSettings& s = pcb.settings;
    std::vector<RuleViolation> out;
    auto add = [&](const char* code, std::string message, std::vector<int> ids, Vec2 at) {
        RuleViolation v;
        v.severity = Severity::Error;
        v.code = code;
        v.message = std::move(message);
        v.components = std::move(ids);
        v.location = at;
        v.hasLocation = true;
        out.push_back(std::move(v));
    };
    std::vector<Body> bodies;
    for (auto& b : partBodies(sch, pcb))
        if (!b.c->sourcing.dnp) bodies.push_back(std::move(b));
    auto mm = [](double v) { return num(std::round(v * 100) / 100) + " mm"; };
    for (const Body& b : bodies) {
        const Component& c = *b.c;
        const double limit = c.pcb.bottom ? s.maxHeightBottom : s.maxHeightTop;
        if (limit > 0 && b.height > limit + 1e-6)
            add("MECH_HEIGHT", c.ref + " is " + mm(b.height) + " tall; the enclosure allows " + mm(limit) + " on the " +
                    (c.pcb.bottom ? "bottom" : "top") + " side.", {c.id}, c.pcb.position);
        const Rect box = b.box();
        for (const HeightZone& z : s.heightZones)
            if (z.bottom == c.pcb.bottom && b.height > z.maxHeight + 1e-6 && box.x0 < z.area.x1 && z.area.x0 < box.x1 &&
                box.y0 < z.area.y1 && z.area.y0 < box.y1)
                add("MECH_HEIGHT_ZONE", c.ref + " (" + mm(b.height) + ") stands in height zone " +
                        (z.name.empty() ? std::string("(unnamed)") : z.name) + ", which allows " + mm(z.maxHeight) + ".",
                    {c.id}, c.pcb.position);
    }
    // Sweep along X: only bodies whose X ranges overlap are compared.
    std::vector<Rect> boxes;
    std::vector<size_t> order(bodies.size());
    for (size_t i = 0; i < bodies.size(); ++i) boxes.push_back(bodies[i].box()), order[i] = i;
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return boxes[a].x0 < boxes[b].x0; });
    std::vector<std::pair<size_t, size_t>> hits;
    for (size_t p = 0; p < order.size(); ++p)
        for (size_t q = p + 1; q < order.size() && boxes[order[q]].x0 < boxes[order[p]].x1; ++q) {
            const size_t i = std::min(order[p], order[q]), j = std::max(order[p], order[q]);
            if (bodies[i].c->pcb.bottom == bodies[j].c->pcb.bottom && bodiesOverlap(bodies[i], bodies[j])) hits.push_back({i, j});
        }
    std::sort(hits.begin(), hits.end());  // report in part order, independent of the sweep
    for (const auto& [i, j] : hits) {
        const Body &a = bodies[i], &b = bodies[j];
        const Rect &ra = boxes[i], &rb = boxes[j];
        add("MECH_BODY_COLLISION", "The bodies of " + a.c->ref + " and " + b.c->ref + " collide.", {a.c->id, b.c->id},
            {(std::max(ra.x0, rb.x0) + std::min(ra.x1, rb.x1)) / 2, (std::max(ra.y0, rb.y0) + std::min(ra.y1, rb.y1)) / 2});
    }
    return out;
}

Json mechanicalLimitsToJson(const BoardSettings& s) {
    if (s.maxHeightTop <= 0 && s.maxHeightBottom <= 0 && s.heightZones.empty()) return Json();
    Json j = Json::object(), zones = Json::array();
    j["maxHeightTop"] = s.maxHeightTop;
    j["maxHeightBottom"] = s.maxHeightBottom;
    for (const HeightZone& z : s.heightZones) {
        Json e = Json::object();
        e["name"] = z.name, e["x0"] = z.area.x0, e["y0"] = z.area.y0, e["x1"] = z.area.x1, e["y1"] = z.area.y1;
        e["bottom"] = z.bottom, e["maxHeight"] = z.maxHeight;
        zones.push(e);
    }
    j["zones"] = zones;
    return j;
}

void mechanicalLimitsFromJson(const Json& j, BoardSettings& s) {
    auto len = [](const Json& v) {
        const double d = v.asNumber(0);
        return std::isfinite(d) && d > 0 ? d : 0.0;
    };
    s.maxHeightTop = len(j.get("maxHeightTop"));
    s.maxHeightBottom = len(j.get("maxHeightBottom"));
    s.heightZones.clear();
    if (!j.get("zones").isArray()) return;
    for (const Json& e : j.get("zones").items()) {
        HeightZone z;
        z.name = e.get("name").asString("");
        const double x0 = e.get("x0").asNumber(0), y0 = e.get("y0").asNumber(0), x1 = e.get("x1").asNumber(0),
                     y1 = e.get("y1").asNumber(0);
        if (!std::isfinite(x0) || !std::isfinite(y0) || !std::isfinite(x1) || !std::isfinite(y1)) continue;
        z.area = Rect(std::min(x0, x1), std::min(y0, y1), std::max(x0, x1), std::max(y0, y1));
        z.bottom = e.get("bottom").asBool(false);
        z.maxHeight = std::max(0.0, e.get("maxHeight").asNumber(0));
        if (z.area.width() > 0 && z.area.height() > 0 && std::isfinite(z.maxHeight)) s.heightZones.push_back(z);
    }
}

std::string exportStep(const Schematic& sch, const PcbLayout& pcb, const std::string& name) {
    const BoardSettings& s = pcb.settings;
    const double t = s.thickness;
    const std::string title = quoted(name.empty() ? "board" : name);
    Step w;
    const int app = w.add("APPLICATION_CONTEXT('core data for automotive mechanical design processes')");
    w.add("APPLICATION_PROTOCOL_DEFINITION('international standard','automotive_design',2000," + w.ref(app) + ")");
    const int pctx = w.add("PRODUCT_CONTEXT(''," + w.ref(app) + ",'mechanical')");
    const int prod = w.add("PRODUCT('" + title + "','" + title + "',''," + "(" + w.ref(pctx) + "))");
    const int form = w.add("PRODUCT_DEFINITION_FORMATION('',''," + w.ref(prod) + ")");
    const int dctx = w.add("PRODUCT_DEFINITION_CONTEXT('part definition'," + w.ref(app) + ",'design')");
    const int def = w.add("PRODUCT_DEFINITION('design',''," + w.ref(form) + "," + w.ref(dctx) + ")");
    const int shape = w.add("PRODUCT_DEFINITION_SHAPE(''," + w.ref(def) + ")");
    const int mm = w.add("(LENGTH_UNIT()NAMED_UNIT(*)SI_UNIT(.MILLI.,.METRE.))");
    const int rad = w.add("(NAMED_UNIT(*)PLANE_ANGLE_UNIT()SI_UNIT($,.RADIAN.))");
    const int sr = w.add("(NAMED_UNIT(*)SI_UNIT($,.STERADIAN.)SOLID_ANGLE_UNIT())");
    const int unc = w.add("UNCERTAINTY_MEASURE_WITH_UNIT(LENGTH_MEASURE(1.E-05)," + w.ref(mm) + ",'distance_accuracy_value','')");
    const int ctx = w.add("(GEOMETRIC_REPRESENTATION_CONTEXT(3)GLOBAL_UNCERTAINTY_ASSIGNED_CONTEXT((" + w.ref(unc) +
                          "))GLOBAL_UNIT_ASSIGNED_CONTEXT((" + w.ref(mm) + "," + w.ref(rad) + "," + w.ref(sr) +
                          "))REPRESENTATION_CONTEXT('',''))");
    std::vector<int> items{w.placement(0, 0, 0, 0, 0, 1, 1, 0, 0)}, styles;

    std::vector<std::vector<Vec2>> holes;
    for (const auto& h : s.holes) holes.push_back(mcad(circle(h.position, h.drill / 2, 24), false));
    const int board = w.prism("PCB", mcad(boardOutline(s), true), holes, 0, t);
    items.push_back(board);
    styles.push_back(w.styled(board, 0.10f, 0.42f, 0.20f));
    for (const auto& b : partBodies(sch, pcb)) {
        const double z0 = b.c->pcb.bottom ? -b.height : t, z1 = b.c->pcb.bottom ? 0 : t + b.height;
        const int solid = w.prism(b.c->ref.empty() ? b.c->footprintName() : b.c->ref, mcad(b.outline, true), {}, z0, z1);
        items.push_back(solid);
        styles.push_back(w.styled(solid, b.r, b.g, b.b));
    }
    std::string list, styleList;
    for (size_t i = 0; i < items.size(); ++i) list += (i ? "," : "") + w.ref(items[i]);
    for (size_t i = 0; i < styles.size(); ++i) styleList += (i ? "," : "") + w.ref(styles[i]);
    const int rep = w.add("FACETED_BREP_SHAPE_REPRESENTATION('" + title + "',(" + list + ")," + w.ref(ctx) + ")");
    w.add("SHAPE_DEFINITION_REPRESENTATION(" + w.ref(shape) + "," + w.ref(rep) + ")");
    w.add("MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION('',(" + styleList + ")," + w.ref(ctx) + ")");

    std::ostringstream o;
    o << "ISO-10303-21;\nHEADER;\nFILE_DESCRIPTION(('SiEDA board assembly'),'2;1');\n"
      << "FILE_NAME('" << title << ".step','" << idfDate() << "',(''),(''),'SiEDA','SiEDA','');\n"
      << "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 1 1 1 1 }'));\nENDSEC;\nDATA;\n"
      << w.o.str() << "ENDSEC;\nEND-ISO-10303-21;\n";
    return o.str();
}

std::string exportIdfBoard(const Schematic& sch, const PcbLayout& pcb, const std::string& name) {
    const BoardSettings& s = pcb.settings;
    std::ostringstream o;
    o << ".HEADER\nBOARD_FILE 3.0 \"SiEDA\" " << idfDate() << " 1\n\"" << quoted(name.empty() ? "board" : name)
      << "\" MM\n.END_HEADER\n.BOARD_OUTLINE ECAD\n" << num(s.thickness) << '\n';
    idfLoop(o, 0, mcad(boardOutline(s), true));
    o << ".END_BOARD_OUTLINE\n.DRILLED_HOLES\n";
    for (const auto& h : s.holes)
        o << num(h.drill) << ' ' << num(h.position.x) << ' ' << num(-h.position.y) << " NPTH BOARD MTG ECAD\n";
    o << ".END_DRILLED_HOLES\n.PLACEMENT\n";
    for (const auto& b : partBodies(sch, pcb)) {
        const Component& c = *b.c;
        o << '"' << quoted(c.footprintName()) << "\" \"" << idfPartNumber(c) << "\" " << quoted(c.ref) << '\n'
          << num(c.pcb.position.x) << ' ' << num(-c.pcb.position.y) << " 0 " << idfRotation(c.pcb.rotation) << ' '
          << (c.pcb.bottom ? "BOTTOM" : "TOP") << (c.pcb.locked ? " MCAD\n" : " PLACED\n");
    }
    o << ".END_PLACEMENT\n";
    return o.str();
}

std::string exportIdfLibrary(const Schematic& sch, const PcbLayout& pcb) {
    std::ostringstream o;
    o << ".HEADER\nLIBRARY_FILE 3.0 \"SiEDA\" " << idfDate() << " 1\n.END_HEADER\n";
    std::set<std::pair<std::string, std::string>> done;
    for (const auto& b : partBodies(sch, pcb)) {
        const Component& c = *b.c;
        if (!done.insert({c.footprintName(), idfPartNumber(c)}).second) continue;
        // The outline in the package's own frame (unrotated, centred on its origin).
        std::vector<Vec2> local;
        for (const auto& v : b.outline) local.push_back(v - c.pcb.position);
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1 && local.size() == 4)
            for (auto& v : local) v = {v.y, v.x};
        o << ".ELECTRICAL\n\"" << quoted(c.footprintName()) << "\" \"" << idfPartNumber(c) << "\" MM "
          << num(b.height) << '\n';
        idfLoop(o, 0, mcad(local, true));
        o << ".END_ELECTRICAL\n";
    }
    return o.str();
}

std::vector<std::string> importIdfPlacement(Schematic& sch, const std::string& emn) {
    std::vector<std::string> moved, lines;
    std::istringstream in(emn);
    for (std::string l; std::getline(in, l);) {
        if (!l.empty() && l.back() == '\r') l.pop_back();
        if (l.find_first_not_of(" \t") != std::string::npos && l[l.find_first_not_of(" \t")] != '#') lines.push_back(l);
    }
    bool inPlacement = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        const auto t = tokens(lines[i]);
        if (t.empty()) continue;
        if (t[0] == ".PLACEMENT") inPlacement = true;
        else if (t[0] == ".END_PLACEMENT") inPlacement = false;
        else if (inPlacement && t.size() >= 3 && i + 1 < lines.size()) {
            const auto p = tokens(lines[++i]);
            Component* c = nullptr;
            if (const Component* found = sch.findByRef(t[2])) c = sch.find(found->id);
            if (!c || p.size() < 5) continue;
            const Vec2 at{std::atof(p[0].c_str()), -std::atof(p[1].c_str())};
            double deg = std::fmod(std::atof(p[3].c_str()), 360.0);  // hostile files: huge or NaN angles
            if (!std::isfinite(deg)) deg = 0;
            const int rot = idfRotation(static_cast<int>(std::lround(deg / 90.0)) * 90);
            const bool bottom = p[4] == "BOTTOM";
            if (std::hypot(at.x - c->pcb.position.x, at.y - c->pcb.position.y) < 1e-4 && rot == c->pcb.rotation % 360 &&
                bottom == c->pcb.bottom)
                continue;
            c->pcb.position = at;
            c->pcb.rotation = rot;
            c->pcb.bottom = bottom;
            c->pcb.placed = true;
            moved.push_back(c->ref);
        }
    }
    return moved;
}

// ------------------------------------------------------------------------------------------------ IDX (EDMD)
namespace {

/// Minimal XML reader for IDX: elements by local name (namespace prefixes dropped), attributes, text. Comments,
/// declarations and processing instructions are skipped; CDATA is text. Nesting is capped (hostile files).
struct XmlNode {
    std::string name, text;
    std::map<std::string, std::string> attrs;
    std::vector<XmlNode> kids;
    const XmlNode* child(const std::string& n) const {
        for (const auto& k : kids)
            if (k.name == n) return &k;
        return nullptr;
    }
    /// The element's number: its text, or its Value child's (IDX writes <tx><Value>1.5</Value></tx>).
    double number(double fallback = 0) const {
        const std::string& t = text.find_first_not_of(" \t\r\n") != std::string::npos ? text : (child("Value") ? child("Value")->text : text);
        char* end = nullptr;
        const double v = std::strtod(t.c_str(), &end);
        return end != t.c_str() && std::isfinite(v) ? v : fallback;
    }
};

std::string localName(const std::string& n) {
    const size_t c = n.rfind(':');
    return c == std::string::npos ? n : n.substr(c + 1);
}

std::string xmlDecode(const std::string& s) {
    std::string o;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] != '&') {
            o += s[i];
            continue;
        }
        static const std::pair<const char*, char> ents[] = {{"&lt;", '<'}, {"&gt;", '>'}, {"&amp;", '&'}, {"&quot;", '"'}, {"&apos;", '\''}};
        bool hit = false;
        for (const auto& [e, ch] : ents)
            if (s.compare(i, std::strlen(e), e) == 0) {
                o += ch;
                i += std::strlen(e) - 1;
                hit = true;
                break;
            }
        if (!hit) o += '&';
    }
    return o;
}

bool parseXml(const std::string& s, XmlNode& root) {
    std::vector<XmlNode*> stack{&root};
    size_t i = 0;
    while (i < s.size()) {
        if (s[i] != '<') {
            const size_t e = s.find('<', i);
            stack.back()->text += xmlDecode(s.substr(i, e == std::string::npos ? std::string::npos : e - i));
            if (e == std::string::npos) break;
            i = e;
            continue;
        }
        if (s.compare(i, 4, "<!--") == 0) {
            const size_t e = s.find("-->", i + 4);
            if (e == std::string::npos) return false;
            i = e + 3;
        } else if (s.compare(i, 9, "<![CDATA[") == 0) {
            const size_t e = s.find("]]>", i + 9);
            if (e == std::string::npos) return false;
            stack.back()->text += s.substr(i + 9, e - i - 9);
            i = e + 3;
        } else if (s.compare(i, 2, "<?") == 0 || s.compare(i, 2, "<!") == 0) {
            const size_t e = s.find('>', i);
            if (e == std::string::npos) return false;
            i = e + 1;
        } else if (s.compare(i, 2, "</") == 0) {
            const size_t e = s.find('>', i);
            if (e == std::string::npos || stack.size() < 2) return false;
            if (localName(std::string(s, i + 2, e - i - 2).substr(0, s.find_first_of(" \t\r\n", i + 2) - i - 2)) != stack.back()->name)
                return false;
            stack.pop_back();
            i = e + 1;
        } else {
            size_t j = i + 1;
            while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j])) && s[j] != '>' && s[j] != '/') ++j;
            XmlNode node;
            node.name = localName(s.substr(i + 1, j - i - 1));
            // Attributes: name="value" or name='value'.
            while (j < s.size() && s[j] != '>' && s[j] != '/') {
                if (std::isspace(static_cast<unsigned char>(s[j]))) {
                    ++j;
                    continue;
                }
                const size_t eq = s.find('=', j);
                if (eq == std::string::npos || eq + 1 >= s.size()) return false;
                const char q = s[eq + 1];
                if (q != '"' && q != '\'') return false;
                const size_t end = s.find(q, eq + 2);
                if (end == std::string::npos) return false;
                std::string key = s.substr(j, eq - j);
                key.erase(key.find_last_not_of(" \t\r\n") + 1);
                node.attrs[localName(key)] = xmlDecode(s.substr(eq + 2, end - eq - 2));
                j = end + 1;
            }
            if (j >= s.size()) return false;
            const bool selfClosing = s[j] == '/';
            const size_t close = s.find('>', j);
            if (close == std::string::npos) return false;
            if (stack.size() > 256) return false;
            stack.back()->kids.push_back(std::move(node));
            if (!selfClosing) stack.push_back(&stack.back()->kids.back());
            i = close + 1;
        }
    }
    return stack.size() == 1;
}

/// A part's placement as IDX writes it: MCAD position (Y up), counter-clockwise rotation, side.
struct IdxPlacement {
    double x = 0, y = 0;
    int rotation = 0;
    bool bottom = false;
};

/// Every component instance in an IDX file by designator (REFDES property, else the instance name).
std::map<std::string, IdxPlacement> idxPlacements(const std::string& idx) {
    std::map<std::string, IdxPlacement> out;
    XmlNode root;
    if (idx.size() > (64u << 20) || !parseXml(idx, root)) return out;
    std::vector<const XmlNode*> todo{&root};
    while (!todo.empty()) {
        const XmlNode* n = todo.back();
        todo.pop_back();
        for (const auto& k : n->kids) todo.push_back(&k);
        if (n->name != "Item") continue;
        const XmlNode* side = n->child("AssembleToName");
        for (const auto& inst : n->kids) {
            if (inst.name != "ItemInstance") continue;
            const XmlNode* t = inst.child("Transformation");
            if (!t) continue;
            std::string ref;
            for (const auto& up : inst.kids)
                if (up.name == "UserProperty" && up.child("Key") && up.child("Key")->child("ObjectName") &&
                    up.child("Key")->child("ObjectName")->text == "REFDES" && up.child("Value"))
                    ref = up.child("Value")->text;
            if (ref.empty() && inst.child("InstanceName") && inst.child("InstanceName")->child("ObjectName"))
                ref = inst.child("InstanceName")->child("ObjectName")->text;
            if (ref.empty()) continue;
            IdxPlacement p;
            p.x = t->child("tx") ? t->child("tx")->number() : 0;
            p.y = t->child("ty") ? t->child("ty")->number() : 0;
            const double xx = t->child("xx") ? t->child("xx")->number(1) : 1, yx = t->child("yx") ? t->child("yx")->number() : 0;
            const double deg = std::atan2(yx, xx) * 180 / kPi;
            p.rotation = ((static_cast<int>(std::lround(deg / 90.0)) * 90) % 360 + 360) % 360;
            const XmlNode* s = inst.child("AssembleToName") ? inst.child("AssembleToName") : side;
            p.bottom = s && s->text.find("BOTTOM") != std::string::npos;
            out[ref] = p;
        }
    }
    return out;
}

/// Items of an IDX file with their geometry: geometryType, name, side, outlines (MCAD XY) with z-range and inversion.
struct IdxShape {
    std::vector<Vec2> points;
    double z0 = 0, z1 = 0;
    bool inverted = false;
};
struct IdxItem {
    std::string type, name, side;
    std::vector<IdxShape> shapes;
};

std::vector<IdxItem> idxItems(const XmlNode& root) {
    std::map<std::string, const XmlNode*> byId;
    std::vector<const XmlNode*> items, todo{&root};
    while (!todo.empty()) {
        const XmlNode* n = todo.back();
        todo.pop_back();
        for (const auto& k : n->kids) todo.push_back(&k);
        const auto id = n->attrs.find("id");
        if (id != n->attrs.end()) byId[id->second] = n;
        if (n->name == "Item" && !n->child("ItemInstance")) items.push_back(n);
    }
    auto ref = [&](std::string id) -> const XmlNode* {  // element by id (references may carry whitespace)
        id.erase(0, id.find_first_not_of(" \t\r\n"));
        id.erase(id.find_last_not_of(" \t\r\n") + 1);
        const auto it = byId.find(id);
        return it == byId.end() ? nullptr : it->second;
    };
    std::vector<IdxItem> out;
    for (const XmlNode* it : items) {
        IdxItem item;
        const auto gt = it->attrs.find("geometryType");
        item.type = gt == it->attrs.end() ? "" : gt->second;
        if (it->child("Name")) item.name = it->child("Name")->text;
        if (it->child("AssembleToName")) item.side = it->child("AssembleToName")->text;
        // Shape → (Stratum | KeepOut | ShapeElement) → CurveSet2d → PolyLine → CartesianPoints, depth-limited.
        std::vector<std::pair<const XmlNode*, bool>> stack;
        for (const auto& k : it->kids)
            if (k.name == "Shape")
                if (const XmlNode* n = ref(k.text)) stack.push_back({n, false});
        for (int guard = 0; !stack.empty() && guard < 10000; ++guard) {
            auto [n, inverted] = stack.back();
            stack.pop_back();
            if (n->child("Inverted")) inverted = inverted || n->child("Inverted")->text.find("true") != std::string::npos;
            if (n->name == "CurveSet2d") {
                IdxShape sh;
                sh.inverted = inverted;
                sh.z0 = n->child("LowerBound") ? n->child("LowerBound")->number() : 0;
                sh.z1 = n->child("UpperBound") ? n->child("UpperBound")->number() : 0;
                auto point = [&](const XmlNode* k) -> std::optional<Vec2> {
                    const XmlNode* p = k ? ref(k->text) : nullptr;
                    if (!p) return std::nullopt;
                    return Vec2{p->child("X") ? p->child("X")->number() : 0, p->child("Y") ? p->child("Y")->number() : 0};
                };
                auto sweep = [&](Vec2 c, Vec2 from, double deg) {  // arc from `from` about `c`, ≤ 10° per segment
                    const int n = std::clamp(static_cast<int>(std::ceil(std::fabs(deg) / 10)), 1, 36);
                    for (int i = 1; i <= n; ++i) {
                        const double a = deg * kPi / 180 * i / n;
                        const Vec2 d = from - c;
                        sh.points.push_back({c.x + d.x * std::cos(a) - d.y * std::sin(a), c.y + d.x * std::sin(a) + d.y * std::cos(a)});
                    }
                };
                for (const auto& k : n->kids) {
                    const XmlNode* line = k.name == "DetailedGeometricModelElement" ? ref(k.text) : nullptr;
                    if (!line) continue;
                    if (const auto c = point(line->child("CenterPoint")); c && line->child("Diameter")) {  // CircleCenter
                        const Vec2 from{c->x + line->child("Diameter")->number() / 2, c->y};
                        sh.points.push_back(from), sweep(*c, from, 360), sh.points.pop_back();
                    } else if (const auto a = point(line->child("StartPoint")), b = point(line->child("EndPoint"));
                               a && b && line->child("Angle")) {  // Arc: start, end and the swept angle (degrees, CCW > 0)
                        const double deg = std::clamp(line->child("Angle")->number(), -360.0, 360.0), t = std::tan(deg * kPi / 360);
                        const Vec2 m = (*a + *b) * 0.5, h = (*b - *a) * 0.5;  // centre on the chord's bisector
                        if (sh.points.empty() || (sh.points.back() - *a).length() > 1e-9) sh.points.push_back(*a);
                        if (std::fabs(t) > 1e-9) sweep({m.x - h.y / t, m.y + h.x / t}, *a, deg); else sh.points.push_back(*b);
                    } else {
                        for (const auto& pt : line->kids)
                            if (pt.name == "Point")
                                if (const auto v = point(&pt); v && (sh.points.empty() || (sh.points.back() - *v).length() > 1e-9))
                                    sh.points.push_back(*v);
                    }
                }
                if (sh.points.size() > 1 && (sh.points.front() - sh.points.back()).length() < 1e-9) sh.points.pop_back();
                if (sh.points.size() >= 3) item.shapes.push_back(std::move(sh));
                continue;
            }
            for (const auto& k : n->kids)
                if (k.name == "DefiningShape" || k.name == "ShapeElement" || k.name == "Shape")
                    if (const XmlNode* m = ref(k.text); m && m != n) stack.push_back({m, inverted});
        }
        out.push_back(std::move(item));
    }
    return out;
}

std::string xmlEscape(const std::string& s) {
    std::string o;
    for (char ch : s) o += ch == '<' ? "&lt;" : ch == '>' ? "&gt;" : ch == '&' ? "&amp;" : ch == '"' ? "&quot;" : std::string(1, ch);
    return o;
}

std::string isoNow() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char b[32];
    std::strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    return b;
}

/// IDX writer. `only` limits the parts written (change files); `changes` writes SendChanges instead of
/// SendInformation, listing the changed parts.
std::string writeIdx(const Schematic& sch, const PcbLayout& pcb, const std::string& name, const std::set<std::string>* only) {
    const BoardSettings& s = pcb.settings;
    std::ostringstream body;
    int next = 1;
    auto id = [&](const char* prefix) { return std::string(prefix) + std::to_string(next++); };
    auto value = [&](const char* tag, double v) {
        return std::string("<") + tag + "><property:Value>" + num(v) + "</property:Value></" + tag + ">";
    };
    // An extruded outline (MCAD XY, Y up) from z0 to z1: points, a closed polyline, its curve set, shape element.
    auto shape = [&](const std::vector<Vec2>& outline, double z0, double z1, bool inverted) {
        std::vector<std::string> pts;
        for (const auto& v : outline) {
            const std::string pid = id("P");
            body << "  <foundation:CartesianPoint id=\"" << pid << "\" xsi:type=\"d2:EDMDCartesianPoint\">" << value("d2:X", v.x)
                 << value("d2:Y", v.y) << "</foundation:CartesianPoint>\n";
            pts.push_back(pid);
        }
        const std::string line = id("L"), curve = id("CS"), element = id("SE");
        body << "  <foundation:PolyLine id=\"" << line << "\" xsi:type=\"d2:EDMDPolyLine\">" << value("d2:Thickness", 0);
        for (const auto& pid : pts) body << "<d2:Point>" << pid << "</d2:Point>";
        body << "<d2:Point>" << pts.front() << "</d2:Point></foundation:PolyLine>\n";
        body << "  <foundation:CurveSet2d id=\"" << curve << "\" xsi:type=\"d2:EDMDCurveSet2d\"><pdm:ShapeDescriptionType>"
                "GeometricModel</pdm:ShapeDescriptionType>" << value("d2:LowerBound", z0) << value("d2:UpperBound", z1)
             << "<d2:DetailedGeometricModelElement>" << line << "</d2:DetailedGeometricModelElement></foundation:CurveSet2d>\n";
        body << "  <foundation:ShapeElement id=\"" << element << "\" xsi:type=\"pdm:EDMDShapeElement\"><pdm:ShapeElementType>"
                "FeatureShapeElement</pdm:ShapeElementType><pdm:Inverted>" << (inverted ? "true" : "false")
             << "</pdm:Inverted><pdm:DefiningShape>" << curve << "</pdm:DefiningShape></foundation:ShapeElement>\n";
        return element;
    };
    auto instanceName = [&](const std::string& n) {
        return "<pdm:InstanceName><foundation:SystemScope>SiEDA</foundation:SystemScope><foundation:ObjectName>" +
               xmlEscape(n) + "</foundation:ObjectName></pdm:InstanceName>";
    };
    std::vector<std::string> changed;
    if (!only) {
        // The board: outline extruded to its thickness, mounting holes cut through it.
        std::vector<std::string> elements{shape(mcad(boardOutline(s), true), 0, s.thickness, false)};
        for (const auto& h : s.holes)
            elements.push_back(shape(mcad(circle(h.position, h.drill / 2, 16), true), 0, s.thickness, true));
        const std::string def = id("ITEM"), inst = id("ITEM");
        body << "  <foundation:Item id=\"" << def << "\" geometryType=\"BOARD_OUTLINE\" xsi:type=\"pdm:EDMDItem\"><foundation:Name>"
             << xmlEscape(name) << "</foundation:Name><pdm:ItemType>single</pdm:ItemType>";
        for (const auto& e : elements) body << "<pdm:Shape>" << e << "</pdm:Shape>";
        body << "</foundation:Item>\n  <foundation:Item id=\"" << inst << "\" xsi:type=\"pdm:EDMDItem\"><foundation:Name>"
             << xmlEscape(name) << "</foundation:Name><pdm:ItemType>assembly</pdm:ItemType><pdm:ItemInstance id=\"" << id("II")
             << "\"><pdm:Item>" << def << "</pdm:Item>" << instanceName(name) << "</pdm:ItemInstance></foundation:Item>\n";
        // Keep-outs: routing / via keep-outs, and height zones as component keep-outs with their height limit.
        auto keepout = [&](const char* type, const std::string& nm, const Rect& r, double z1, const char* side) {
            const std::vector<Vec2> box{{r.x0, r.y0}, {r.x1, r.y0}, {r.x1, r.y1}, {r.x0, r.y1}};
            const std::string e = shape(mcad(box, true), 0, z1, false);
            body << "  <foundation:Item id=\"" << id("ITEM") << "\" geometryType=\"" << type
                 << "\" xsi:type=\"pdm:EDMDItem\"><foundation:Name>" << xmlEscape(nm) << "</foundation:Name><pdm:ItemType>"
                 << "single</pdm:ItemType><pdm:Shape>" << e << "</pdm:Shape>"
                 << (side ? std::string("<pdm:AssembleToName>") + side + "</pdm:AssembleToName>" : std::string())
                 << "</foundation:Item>\n";
        };
        for (const auto& k : s.keepouts)
            keepout(k.tracks ? "KEEPOUT_AREA_ROUTE" : "KEEPOUT_AREA_VIA", k.name, k.area, 0, nullptr);
        for (const auto& z : s.heightZones)
            keepout("KEEPOUT_AREA_COMPONENT", z.name, z.area, z.maxHeight, z.bottom ? "BOTTOM" : "TOP");
    }
    // Parts: a package item (body in its own frame) and an assembly instance placed by a 2D transformation.
    for (const auto& b : partBodies(sch, pcb)) {
        const Component& c = *b.c;
        if (only && !only->count(c.ref)) continue;
        std::vector<Vec2> local;
        for (const auto& v : b.outline) local.push_back(v - c.pcb.position);
        if (((c.pcb.rotation / 90) % 2 + 2) % 2 == 1 && local.size() == 4)
            for (auto& v : local) v = {v.y, v.x};
        const std::string element = shape(mcad(local, true), 0, b.height, false);
        const std::string def = id("ITEM"), inst = id("ITEM"), ii = id("II");
        const double a = idfRotation(c.pcb.rotation) * kPi / 180;
        const char* side = c.pcb.bottom ? "BOTTOM" : "TOP";
        body << "  <foundation:Item id=\"" << def << "\" geometryType=\"COMPONENT\" xsi:type=\"pdm:EDMDItem\"><foundation:Name>"
             << xmlEscape(c.footprintName()) << "</foundation:Name><pdm:ItemType>single</pdm:ItemType><pdm:PackageName>"
             << "<foundation:SystemScope>SiEDA</foundation:SystemScope><foundation:ObjectName>" << xmlEscape(c.footprintName())
             << "</foundation:ObjectName></pdm:PackageName><pdm:Shape>" << element << "</pdm:Shape></foundation:Item>\n";
        body << "  <foundation:Item id=\"" << inst << "\" geometryType=\"COMPONENT\" xsi:type=\"pdm:EDMDItem\"><foundation:Name>"
             << xmlEscape(c.ref) << "</foundation:Name><pdm:ItemType>assembly</pdm:ItemType><pdm:ItemInstance id=\"" << ii
             << "\"><pdm:Item>" << def << "</pdm:Item>" << instanceName(c.ref)
             << "<pdm:UserProperty xsi:type=\"property:EDMDUserSimpleProperty\"><property:Key><foundation:SystemScope>SiEDA"
                "</foundation:SystemScope><foundation:ObjectName>REFDES</foundation:ObjectName></property:Key><property:Value>"
             << xmlEscape(c.ref) << "</property:Value></pdm:UserProperty><pdm:Transformation><pdm:TransformationType>d2"
             << "</pdm:TransformationType><pdm:xx>" << num(std::cos(a)) << "</pdm:xx><pdm:xy>" << num(-std::sin(a))
             << "</pdm:xy><pdm:yx>" << num(std::sin(a)) << "</pdm:yx><pdm:yy>" << num(std::cos(a)) << "</pdm:yy>"
             << value("pdm:tx", c.pcb.position.x) << value("pdm:ty", -c.pcb.position.y)
             << value("pdm:zOffset", c.pcb.bottom ? 0 : s.thickness) << "</pdm:Transformation></pdm:ItemInstance>"
             << "<pdm:AssembleToName>" << side << "</pdm:AssembleToName></foundation:Item>\n";
        changed.push_back(inst);
    }
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
         "<foundation:EDMDDataSet xmlns:foundation=\"http://www.prostep.org/EDMD/Foundation\" "
         "xmlns:pdm=\"http://www.prostep.org/EDMD/PDM\" xmlns:d2=\"http://www.prostep.org/EDMD/2D\" "
         "xmlns:property=\"http://www.prostep.org/EDMD/Property\" "
         "xmlns:computational=\"http://www.prostep.org/EDMD/Computational\" "
         "xmlns:administration=\"http://www.prostep.org/EDMD/Administration\" "
         "xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
         " <foundation:Header xsi:type=\"foundation:EDMDHeader\"><foundation:Description>"
      << (only ? "SiEDA placement changes" : "SiEDA board baseline") << "</foundation:Description><foundation:CreatorName>"
      << "SiEDA</foundation:CreatorName><foundation:CreatorSystem>SiEDA</foundation:CreatorSystem>"
         "<foundation:GlobalUnitLength>UNIT_MM</foundation:GlobalUnitLength><foundation:CreationDateTime>"
      << isoNow() << "</foundation:CreationDateTime></foundation:Header>\n <foundation:Body xsi:type=\"foundation:EDMDDataSetBody\">\n"
      << body.str() << " </foundation:Body>\n";
    if (only) {
        o << " <foundation:ProcessInstruction xsi:type=\"computational:EDMDProcessInstructionSendChanges\">\n";
        for (const auto& c : changed)
            o << "  <computational:Changes><computational:Change xsi:type=\"computational:EDMDChange\"><computational:NewItem>"
              << c << "</computational:NewItem></computational:Change></computational:Changes>\n";
        o << " </foundation:ProcessInstruction>\n";
    } else {
        o << " <foundation:ProcessInstruction xsi:type=\"computational:EDMDProcessInstructionSendInformation\"/>\n";
    }
    o << "</foundation:EDMDDataSet>\n";
    return o.str();
}

}  // namespace

std::string exportIdx(const Schematic& sch, const PcbLayout& pcb, const std::string& name) {
    return writeIdx(sch, pcb, name.empty() ? "board" : name, nullptr);
}

std::string exportIdxChanges(const Schematic& sch, const PcbLayout& pcb, const std::string& name, const std::string& baseline) {
    const auto before = idxPlacements(baseline);
    std::set<std::string> refs;
    for (const auto& b : partBodies(sch, pcb)) {
        const Component& c = *b.c;
        const auto it = before.find(c.ref);
        if (it == before.end() || std::hypot(it->second.x - c.pcb.position.x, it->second.y + c.pcb.position.y) > 1e-4 ||
            it->second.rotation != idfRotation(c.pcb.rotation) || it->second.bottom != c.pcb.bottom)
            refs.insert(c.ref);
    }
    return refs.empty() ? std::string() : writeIdx(sch, pcb, name.empty() ? "board" : name, &refs);
}

namespace {

/// A keep-out polygon (MCAD XY) as SiEDA rectangles: one per run inside each horizontal slab between vertex
/// heights, each as wide as the polygon anywhere in its slab — exact for rectilinear shapes, never smaller than the
/// polygon. More than 32 slabs or 64 rectangles: the bounding box.
std::vector<Rect> stripsOf(const std::vector<Vec2>& mcadPoly) {
    std::vector<Vec2> p;
    for (const auto& v : mcadPoly) p.push_back({v.x, -v.y});
    Rect box(p[0].x, p[0].y, p[0].x, p[0].y);
    for (const auto& v : p) box = Rect(std::min(box.x0, v.x), std::min(box.y0, v.y), std::max(box.x1, v.x), std::max(box.y1, v.y));
    std::vector<double> ys;
    for (const auto& v : p) ys.push_back(v.y);
    std::sort(ys.begin(), ys.end());
    ys.erase(std::unique(ys.begin(), ys.end(), [](double a, double b) { return b - a < 1e-6; }), ys.end());
    if (ys.size() > 33) return {box};
    std::vector<Rect> out;
    for (size_t i = 0; i + 1 < ys.size(); ++i) {
        const double y0 = ys[i], y1 = ys[i + 1], ym = (y0 + y1) / 2;
        struct Cross { double mid, lo, hi; };  // an edge through the slab: x at mid-height and its extent in the slab
        std::vector<Cross> xs;
        for (size_t k = 0; k < p.size(); ++k) {
            const Vec2 a = p[k], b = p[(k + 1) % p.size()];
            if ((a.y <= ym) == (b.y <= ym)) continue;
            auto at = [&](double y) { return a.x + (b.x - a.x) * std::clamp((y - a.y) / (b.y - a.y), 0.0, 1.0); };
            xs.push_back({at(ym), std::min(at(y0), at(y1)), std::max(at(y0), at(y1))});
        }
        std::sort(xs.begin(), xs.end(), [](const Cross& a, const Cross& b) { return a.mid < b.mid; });
        for (size_t k = 0; k + 1 < xs.size(); k += 2) out.push_back(Rect(xs[k].lo, y0, xs[k + 1].hi, y1));
        if (out.size() > 64) return {box};
    }
    return out.empty() ? std::vector<Rect>{box} : out;
}

}  // namespace

IdxImport importIdx(Schematic& sch, BoardSettings& s, const std::string& idx) {
    IdxImport r;
    r.moved = importIdxPlacement(sch, idx);
    XmlNode root;
    if (idx.size() > (64u << 20) || !parseXml(idx, root)) return r;
    std::vector<RouteKeepout> newKeepouts;
    std::vector<HeightZone> newZones;
    for (const auto& item : idxItems(root)) {
        auto box = [](const IdxShape& sh) {
            Rect b(sh.points[0].x, -sh.points[0].y, sh.points[0].x, -sh.points[0].y);
            for (const auto& p : sh.points) b = Rect(std::min(b.x0, p.x), std::min(b.y0, -p.y), std::max(b.x1, p.x), std::max(b.y1, -p.y));
            return b;
        };
        if (item.type == "BOARD_OUTLINE") {
            std::vector<MountingHole> holes;  // inverted round cut-outs: drilled holes, centre and diameter
            for (const auto& sh : item.shapes)
                if (sh.inverted && sh.points.size() >= 8) {
                    Vec2 c{0, 0};
                    for (const auto& p : sh.points) c = c + Vec2{p.x, -p.y} * (1.0 / sh.points.size());
                    double r = 0;
                    for (const auto& p : sh.points) r += (Vec2{p.x, -p.y} - c).length() / sh.points.size();
                    MountingHole h;
                    h.position = c;
                    h.drill = 2 * r;
                    const auto old = std::find_if(s.holes.begin(), s.holes.end(), [&](const MountingHole& o) {
                        return (o.position - c).length() < 0.05;
                    });
                    if (old != s.holes.end()) h.keepout = old->keepout + (h.drill - old->drill);
                    holes.push_back(h);
                }
            const bool holesDiffer = holes.size() != s.holes.size() ||
                                     !std::all_of(holes.begin(), holes.end(), [&](const MountingHole& h) {
                                         return std::any_of(s.holes.begin(), s.holes.end(), [&](const MountingHole& o) {
                                             return (o.position - h.position).length() < 0.05 && std::fabs(o.drill - h.drill) < 0.05;
                                         });
                                     });
            if (holesDiffer && item.shapes.size() > holes.size()) s.holes = holes, r.holesChanged = true;
            for (const auto& sh : item.shapes) {
                if (sh.inverted) continue;
                std::vector<Vec2> poly;
                for (const auto& p : sh.points) poly.push_back({p.x, -p.y});
                const Rect b = box(sh);
                if (b.x0 < -1e-6 || b.y0 < -1e-6 || b.width() <= 0 || b.height() <= 0 || poly.size() > 100000) continue;
                bool same = poly.size() == s.outlinePolygon().size();
                if (same) {
                    const auto cur = s.outlinePolygon();
                    // Same polygon, possibly from a different starting point or winding.
                    for (const auto& p : poly)
                        same = same && std::any_of(cur.begin(), cur.end(), [&](const Vec2& q) { return (p - q).length() < 1e-4; });
                }
                if (std::fabs(sh.z1 - sh.z0) > 0.05 && std::fabs(sh.z1 - sh.z0 - s.thickness) > 1e-4) {
                    s.thickness = std::fabs(sh.z1 - sh.z0);
                    r.thicknessChanged = true;
                }
                if (same) break;
                const bool rect = poly.size() == 4 && std::fabs(b.x0) < 1e-6 && std::fabs(b.y0) < 1e-6 &&
                                  std::all_of(poly.begin(), poly.end(), [&](const Vec2& p) {
                                      return (std::fabs(p.x - b.x0) < 1e-6 || std::fabs(p.x - b.x1) < 1e-6) &&
                                             (std::fabs(p.y - b.y0) < 1e-6 || std::fabs(p.y - b.y1) < 1e-6);
                                  });
                s.outline = rect ? std::vector<Vec2>{} : poly;
                s.width = b.x1;
                s.height = b.y1;
                r.outlineChanged = true;
                break;
            }
        } else if (item.type.rfind("KEEPOUT_AREA", 0) == 0 && !item.shapes.empty()) {
            const std::string name = "MCAD " + (item.name.empty() ? item.type.substr(13) : item.name);
            int part = 0;
            for (const Rect& b : stripsOf(item.shapes.front().points)) {
                const std::string nm = part++ ? name + " #" + std::to_string(part) : name;
                if (item.type.find("COMPONENT") != std::string::npos || item.type.find("PLACEMENT") != std::string::npos) {
                    HeightZone z;
                    z.name = nm;
                    z.area = b;
                    z.bottom = item.side.find("BOTTOM") != std::string::npos;
                    z.maxHeight = std::max(0.0, item.shapes.front().z1 - item.shapes.front().z0);
                    newZones.push_back(z);
                } else {
                    RouteKeepout k;
                    k.name = nm;
                    k.area = b;
                    k.tracks = item.type.find("VIA") == std::string::npos;
                    k.vias = true;
                    newKeepouts.push_back(k);
                }
            }
        }
    }
    // MCAD's keep-outs replace the ones it sent before (named "MCAD …"); SiEDA's own stay.
    auto fromMcad = [](const std::string& n) { return n.rfind("MCAD ", 0) == 0; };
    auto mergeIn = [&](auto& list, auto& added, int& count) {
        list.erase(std::remove_if(list.begin(), list.end(), [&](const auto& x) { return fromMcad(x.name); }), list.end());
        for (auto& x : added) {
            const bool known = std::any_of(list.begin(), list.end(), [&](const auto& y) {
                return std::fabs(y.area.x0 - x.area.x0) < 1e-4 && std::fabs(y.area.y0 - x.area.y0) < 1e-4 &&
                       std::fabs(y.area.x1 - x.area.x1) < 1e-4 && std::fabs(y.area.y1 - x.area.y1) < 1e-4;
            });
            if (!known) list.push_back(x), ++count;
        }
    };
    mergeIn(s.keepouts, newKeepouts, r.keepouts);
    mergeIn(s.heightZones, newZones, r.heightZones);
    return r;
}

std::string idxResponse(const std::string& changesIdx, bool accept) {
    XmlNode root;
    std::vector<std::string> ids;
    if (changesIdx.size() <= (64u << 20) && parseXml(changesIdx, root)) {
        std::vector<const XmlNode*> todo{&root};
        while (!todo.empty()) {
            const XmlNode* n = todo.back();
            todo.pop_back();
            for (const auto& k : n->kids) todo.push_back(&k);
            if (n->name == "Change" && n->child("NewItem")) ids.push_back(n->child("NewItem")->text);
        }
    }
    std::ostringstream o;
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<foundation:EDMDDataSet xmlns:foundation=\"http://www.prostep.org/EDMD/Foundation\" "
         "xmlns:computational=\"http://www.prostep.org/EDMD/Computational\" xmlns:xsi=\"http://www.w3.org/2001/XMLSchema-instance\">\n"
         " <foundation:Header xsi:type=\"foundation:EDMDHeader\"><foundation:Description>SiEDA response</foundation:Description>"
         "<foundation:CreatorSystem>SiEDA</foundation:CreatorSystem><foundation:GlobalUnitLength>UNIT_MM</foundation:GlobalUnitLength>"
         "<foundation:CreationDateTime>" << isoNow() << "</foundation:CreationDateTime></foundation:Header>\n"
         " <foundation:Body xsi:type=\"foundation:EDMDDataSetBody\"/>\n"
         " <foundation:ProcessInstruction xsi:type=\"computational:EDMDProcessInstructionSendChanges\">\n";
    for (const auto& id : ids)
        o << "  <computational:Changes><computational:Change xsi:type=\"computational:EDMDChange\"><computational:NewItem>"
          << xmlEscape(id) << "</computational:NewItem><computational:Accept>" << (accept ? "true" : "false")
          << "</computational:Accept></computational:Change></computational:Changes>\n";
    o << " </foundation:ProcessInstruction>\n</foundation:EDMDDataSet>\n";
    return o.str();
}

std::vector<std::string> importIdxPlacement(Schematic& sch, const std::string& idx) {
    std::vector<std::string> moved;
    for (const auto& [ref, p] : idxPlacements(idx)) {
        const Component* found = sch.findByRef(ref);
        Component* c = found ? sch.find(found->id) : nullptr;
        if (!c || !c->hasFootprint()) continue;
        const Vec2 at{p.x, -p.y};
        const int rot = idfRotation(p.rotation);
        if (std::hypot(at.x - c->pcb.position.x, at.y - c->pcb.position.y) < 1e-4 && rot == ((c->pcb.rotation % 360) + 360) % 360 &&
            p.bottom == c->pcb.bottom)
            continue;
        c->pcb.position = at;
        c->pcb.rotation = rot;
        c->pcb.bottom = p.bottom;
        c->pcb.placed = true;
        moved.push_back(c->ref);
    }
    return moved;
}

}  // namespace sieda
