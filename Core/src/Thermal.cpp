#include "sieda/Thermal.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>

#include "sieda/Library.hpp"
#include "sieda/Reliability.hpp"
#include "sieda/Simulator.hpp"
#include "sieda/Stackup.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda {

namespace {
constexpr double kCopperK = 385;  // W/m·K

std::string fmt(const char* f, double v) {
    char b[64];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

bool insidePolygon(const std::vector<Vec2>& q, Vec2 p) {
    bool in = false;
    for (size_t i = 0, j = q.size() - 1; i < q.size(); j = i++)
        if ((q[i].y > p.y) != (q[j].y > p.y) && p.x < (q[j].x - q[i].x) * (p.y - q[i].y) / (q[j].y - q[i].y) + q[i].x)
            in = !in;
    return in;
}
}  // namespace

ThermalResult thermalAnalysis(const Project& project, const ThermalOptions& o) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const BoardSettings& s = pcb.settings;
    ThermalResult r;
    r.ambientC = r.maxC = o.ambientC;
    const std::vector<Vec2> outline = s.outlinePolygon();
    if (outline.size() < 3) {
        r.notes.push_back("The board has no outline.");
        return r;
    }
    double x0 = outline[0].x, y0 = outline[0].y, x1 = x0, y1 = y0;
    for (const Vec2& p : outline) x0 = std::min(x0, p.x), y0 = std::min(y0, p.y), x1 = std::max(x1, p.x), y1 = std::max(y1, p.y);
    const double cell = o.cell > 0 ? std::max(0.1, o.cell) : std::max(0.5, std::sqrt((x1 - x0) * (y1 - y0) / 20000));
    const int nx = std::max(1, static_cast<int>(std::ceil((x1 - x0) / cell))), ny = std::max(1, static_cast<int>(std::ceil((y1 - y0) / cell)));
    const size_t n = static_cast<size_t>(nx) * static_cast<size_t>(ny);
    r.cell = cell, r.x0 = x0, r.y0 = y0, r.nx = nx, r.ny = ny;
    auto cellOf = [&](Vec2 p) -> long {
        const int i = static_cast<int>(std::floor((p.x - x0) / cell)), j = static_cast<int>(std::floor((p.y - y0) / cell));
        if (i < 0 || j < 0 || i >= nx || j >= ny) return -1;
        return static_cast<long>(j) * nx + i;
    };
    std::vector<char> inside(n, 0);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const Vec2 c{x0 + (i + 0.5) * cell, y0 + (j + 0.5) * cell};
            bool in = insidePolygon(outline, c);
            for (const auto& cut : s.cutouts)
                if (in && cut.size() >= 3 && insidePolygon(cut, c)) in = false;
            inside[static_cast<size_t>(j) * nx + i] = in;
        }

    // Copper area per layer and cell (mm²): tracks sampled along their length, pads and pour rectangles by overlap.
    const int layers = std::max(1, s.layerCount);
    std::vector<std::vector<double>> cu(static_cast<size_t>(layers), std::vector<double>(n, 0));
    auto addRect = [&](int l, double ax, double ay, double bx, double by) {
        if (l < 0 || l >= layers) return;
        const int i0 = std::max(0, static_cast<int>(std::floor((ax - x0) / cell))), i1 = std::min(nx - 1, static_cast<int>(std::floor((bx - x0) / cell)));
        const int j0 = std::max(0, static_cast<int>(std::floor((ay - y0) / cell))), j1 = std::min(ny - 1, static_cast<int>(std::floor((by - y0) / cell)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                const double w = std::min(bx, x0 + (i + 1) * cell) - std::max(ax, x0 + i * cell);
                const double h = std::min(by, y0 + (j + 1) * cell) - std::max(ay, y0 + j * cell);
                if (w > 0 && h > 0) cu[static_cast<size_t>(l)][static_cast<size_t>(j) * nx + i] += w * h;
            }
    };
    for (const auto& t : pcb.tracks) {
        if (t.layer < 0 || t.layer >= layers) continue;
        const double len = trackLength(t);
        const int steps = std::max(1, static_cast<int>(std::ceil(len / (cell / 4))));
        const ArcGeom g = trackArc(t);
        for (int k = 0; k < steps; ++k) {
            const double f = (k + 0.5) / steps;
            const Vec2 p = g.valid ? g.point(f) : t.a + (t.b - t.a) * f;
            const long c = cellOf(p);
            if (c >= 0) cu[static_cast<size_t>(t.layer)][static_cast<size_t>(c)] += t.width * len / steps;
        }
    }
    const std::vector<Pad> pads = pcb.pads(sch);
    for (const auto& p : pads)
        if (!p.npth)
            for (int l = 0; l < layers; ++l)
                if (p.onLayer(l))
                    addRect(l, p.position.x - p.size.x / 2, p.position.y - p.size.y / 2, p.position.x + p.size.x / 2, p.position.y + p.size.y / 2);
    for (const auto& f : copperFills(sch, pcb))
        for (const Rect& q : f.rects) addRect(f.layer, std::min(q.x0, q.x1), std::min(q.y0, q.y1), std::max(q.x0, q.x1), std::max(q.y0, q.y1));

    // Sheet conductance per cell (W/K): laminate plus each layer's copper share.
    const double tcu = copperThickness(s), lam = boardLaminate(s).thermalConductivity;
    const double area = cell * cell;
    std::vector<double> kt(n, 0);
    for (size_t c = 0; c < n; ++c) {
        if (!inside[c]) continue;
        double v = lam * std::max(0.05, s.thickness - layers * tcu) * 1e-3;
        for (int l = 0; l < layers; ++l) v += kCopperK * tcu * 1e-3 * std::min(1.0, cu[static_cast<size_t>(l)][c] / area);
        kt[c] = v;
    }
    const double gAir = (std::max(0.0, o.hTop) + std::max(0.0, o.hBottom)) * area * 1e-6;  // W/K per cell

    // Heat sources: the options' powers, else the DC operating point's dissipation.
    std::map<int, double> dcPower;
    {
        bool hasSource = false;
        for (const auto& c : sch.components()) hasSource |= isSourceKind(c.kind);
        if (hasSource) {
            const DcResult dc = Simulator(sch).dcOperatingPoint();
            if (dc.converged) {
                for (const auto& d : dc.devices)
                    if (d.power > 0) dcPower[d.componentId] += d.power;
            } else {
                r.notes.push_back("The DC operating point did not converge: only the powers given are used.");
            }
        }
    }
    std::vector<std::vector<std::pair<long, double>>> feeds;  // per part: board cells and their share of its power
    int unplaced = 0;
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !pcb.onBoard(c) || isSourceKind(c.kind) || c.packageOnly) continue;
        if (!c.pcb.placed) {
            ++unplaced;
            continue;
        }
        ThermalPart part;
        part.componentId = c.id, part.ref = c.ref, part.position = c.pcb.position;
        if (auto it = o.power.find(c.ref); it != o.power.end())
            part.power = std::max(0.0, it->second), part.powerSource = "set";
        else if (auto d = dcPower.find(c.id); d != dcPower.end())
            part.power = d->second, part.powerSource = "dc";
        if (part.power <= 0) continue;
        // Into the board through its pads (by pad area), else at its centre.
        std::vector<std::pair<long, double>> into;
        double total = 0;
        for (const auto& p : pads)
            if (p.componentId == c.id) {
                const long k = cellOf(p.position);
                if (k >= 0 && inside[static_cast<size_t>(k)]) into.push_back({k, p.size.x * p.size.y}), total += p.size.x * p.size.y;
            }
        if (into.empty() || total <= 0) {
            const long k = cellOf(c.pcb.position);
            if (k < 0 || !inside[static_cast<size_t>(k)]) {
                r.notes.push_back(c.ref + " is off the board: its power is left out.");
                continue;
            }
            into = {{k, 1.0}}, total = 1;
        }
        for (auto& f : into) f.second /= total;
        feeds.push_back(std::move(into));
        r.totalPower += part.power;
        if (auto t = o.thetaJB.find(c.ref); t != o.thetaJB.end())
            part.thetaJB = std::max(0.0, t->second), part.thetaEstimated = false;
        else
            part.thetaJB = 0.35 * packageThetaJA(c);  // junction-to-board ≈ a third of θJA for leaded / QFN parts
        if (auto t = o.heatsink.find(c.ref); t != o.heatsink.end() && t->second > 0) part.thetaHeatsink = t->second;
        // Its body's top and sides also lose heat to the air (a transformer or an inductor sheds most of its own).
        if (const FootprintDef* fp = Library::instance().footprint(c.footprintName()); fp && fp->body.width > 0 && fp->body.depth > 0) {
            const BodyDef& b = fp->body;
            const double skin = b.width * b.depth + 2 * std::max(0.0, b.height) * (b.width + b.depth);  // mm²
            const double h = c.pcb.bottom ? o.hBottom : o.hTop;
            if (h > 0) part.thetaBody = 1e6 / (h * skin);
        }
        r.parts.push_back(part);
    }

    // Solve  Σ G_ij (T_j − T_i) + Q_i − G_air (T_i − T_a) = 0  for the rise over ambient: conjugate gradients with a
    // Jacobi preconditioner (the matrix is symmetric positive definite while every cell loses heat to the air).
    auto link = [&](size_t a, size_t b) { return kt[a] > 0 && kt[b] > 0 ? 2 * kt[a] * kt[b] / (kt[a] + kt[b]) : 0.0; };
    std::vector<double> diag(n, 0);
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const size_t c = static_cast<size_t>(j) * nx + i;
            if (!inside[c]) continue;
            double d = gAir > 0 ? gAir : 1e-9;
            if (i > 0) d += link(c, c - 1);
            if (i + 1 < nx) d += link(c, c + 1);
            if (j > 0) d += link(c, c - nx);
            if (j + 1 < ny) d += link(c, c + nx);
            diag[c] = d;
        }
    auto apply = [&](const std::vector<double>& x, std::vector<double>& y) {
        for (int j = 0; j < ny; ++j)
            for (int i = 0; i < nx; ++i) {
                const size_t c = static_cast<size_t>(j) * nx + i;
                if (!inside[c]) {
                    y[c] = 0;
                    continue;
                }
                double v = diag[c] * x[c];
                if (i > 0) v -= link(c, c - 1) * x[c - 1];
                if (i + 1 < nx) v -= link(c, c + 1) * x[c + 1];
                if (j > 0) v -= link(c, c - nx) * x[c - nx];
                if (j + 1 < ny) v -= link(c, c + nx) * x[c + nx];
                y[c] = v;
            }
    };
    std::vector<double> x(n, 0), q(n, 0);
    double norm0 = 0;
    auto solve = [&] {  // warm-started from the last x
        std::vector<double> res(n, 0), z(n, 0), dir(n, 0), ap(n, 0);
        apply(x, ap);
        double rz = 0, normQ = 0;
        for (size_t c = 0; c < n; ++c) {
            res[c] = q[c] - ap[c];
            z[c] = diag[c] > 0 ? res[c] / diag[c] : 0;
            rz += res[c] * z[c], normQ += q[c] * q[c];
        }
        dir = z;
        norm0 = std::sqrt(normQ);
        r.converged = norm0 == 0;
        for (int it = 0; it < 20000 && !r.converged; ++it) {
            apply(dir, ap);
            double pap = 0;
            for (size_t c = 0; c < n; ++c) pap += dir[c] * ap[c];
            if (pap <= 0) break;
            const double alpha = rz / pap;
            double norm = 0;
            for (size_t c = 0; c < n; ++c) x[c] += alpha * dir[c], res[c] -= alpha * ap[c], norm += res[c] * res[c];
            ++r.iterations;
            if (std::sqrt(norm) <= 1e-9 * norm0) {
                r.converged = true;
                break;
            }
            double rz2 = 0;
            for (size_t c = 0; c < n; ++c) z[c] = diag[c] > 0 ? res[c] / diag[c] : 0, rz2 += res[c] * z[c];
            const double beta = rz2 / rz;
            rz = rz2;
            for (size_t c = 0; c < n; ++c) dir[c] = z[c] + beta * dir[c];
        }
    };
    auto boardRise = [&](const std::vector<std::pair<long, double>>& into) {
        double t = 0;
        for (const auto& f : into) t = std::max(t, x[static_cast<size_t>(f.first)]);
        return t;
    };
    // A part splits its power: the junction drains to the board through θJB and straight to the air through its body
    // (and heatsink), so P_board = P − (T_j − T_a) / θair with T_j set by the board under it. Repeated until it settles.
    for (int pass = 0; pass < 30; ++pass) {
        std::fill(q.begin(), q.end(), 0.0);
        for (size_t p = 0; p < r.parts.size(); ++p)
            for (const auto& [k, w] : feeds[p]) q[static_cast<size_t>(k)] += (r.parts[p].power - r.parts[p].toAirW) * w;
        solve();
        double change = 0;
        for (size_t p = 0; p < r.parts.size(); ++p) {
            auto& part = r.parts[p];
            const double gH = (part.thetaHeatsink > 0 ? 1 / part.thetaHeatsink : 0) + (part.thetaBody > 0 ? 1 / part.thetaBody : 0);
            if (gH <= 0) continue;
            const double tb = boardRise(feeds[p]);  // over ambient
            const double gB = part.thetaJB > 0 ? 1 / part.thetaJB : 1e6;
            const double tj = (part.power + tb * gB) / (gB + gH);
            const double hs = std::clamp(tj * gH, 0.0, part.power);
            change = std::max(change, std::abs(hs - part.toAirW));
            part.toAirW = 0.5 * (part.toAirW + hs);  // damped: the board answers every part at once
        }
        if (change < 1e-4) break;
    }

    r.temperature.assign(n, std::numeric_limits<double>::quiet_NaN());
    for (size_t c = 0; c < n; ++c)
        if (inside[c]) {
            r.temperature[c] = o.ambientC + x[c];
            if (r.temperature[c] > r.maxC) {
                r.maxC = r.temperature[c];
                r.hottest = {x0 + (static_cast<double>(c % static_cast<size_t>(nx)) + 0.5) * cell,
                             y0 + (static_cast<double>(c / static_cast<size_t>(nx)) + 0.5) * cell};
            }
        }
    for (auto& part : r.parts) {
        part.boardC = o.ambientC;
        for (const auto& p : pads)
            if (p.componentId == part.componentId)
                if (const long k = cellOf(p.position); k >= 0 && inside[static_cast<size_t>(k)])
                    part.boardC = std::max(part.boardC, r.temperature[static_cast<size_t>(k)]);
        if (const long k = cellOf(part.position); k >= 0 && inside[static_cast<size_t>(k)])
            part.boardC = std::max(part.boardC, r.temperature[static_cast<size_t>(k)]);
        part.junctionC = part.boardC + (part.power - part.toAirW) * part.thetaJB;
    }
    std::sort(r.parts.begin(), r.parts.end(), [](const ThermalPart& a, const ThermalPart& b) {
        return a.junctionC != b.junctionC ? a.junctionC > b.junctionC : a.ref < b.ref;
    });

    const double tg = boardLaminate(s).tg;
    for (const auto& p : r.parts) {
        if (p.junctionC > o.junctionLimitC)
            r.findings.push_back({"THERMAL_JUNCTION", p.junctionC > 150 ? "error" : "warning",
                                  p.ref + " reaches about " + fmt("%.0f °C", p.junctionC) + " at the junction (" +
                                      fmt("%.2f W", p.power) + ", board " + fmt("%.0f °C", p.boardC) + " under it), above " +
                                      fmt("%.0f °C", o.junctionLimitC) + ": add copper under it (a pour of its ground / "
                                      "thermal-pad net), thermal vias to a plane, a larger package or airflow.",
                                  p.ref});
        if (p.boardC > tg - 25)
            r.findings.push_back({"THERMAL_BOARD", "warning",
                                  "The board under " + p.ref + " reaches about " + fmt("%.0f °C", p.boardC) +
                                      ", near the laminate's glass transition (Tg " + fmt("%.0f °C", tg) +
                                      "): spread the heat or choose a high-Tg laminate.",
                                  p.ref});
    }
    if (unplaced)
        r.notes.push_back(std::to_string(unplaced) + " part(s) not placed on the board are left out: place them first.");
    if (r.parts.empty())
        r.notes.push_back("No part dissipates power: give powers by designator (power) or add a supply so the DC "
                          "operating point finds them.");
    if (!r.converged && norm0 > 0) r.notes.push_back("The solver stopped before converging: the map is approximate.");
    r.notes.push_back("Board model: one isothermal sheet (laminate + copper coverage per layer), " +
                      fmt("%.0f", o.hTop) + " / " + fmt("%.0f W/m²·K", o.hBottom) +
                      " to the air from the top / bottom; each part's body (and heatsink) sheds part of its power to the air; junction = "
                      "board + power into the board × θJB.");
    return r;
}

ThermalOptions thermalOptionsFromJson(const Json& j) {
    ThermalOptions o;
    o.ambientC = j.get("ambient").asNumber(o.ambientC);
    o.hTop = j.get("hTop").asNumber(o.hTop);
    o.hBottom = j.get("hBottom").asNumber(o.hBottom);
    o.cell = j.get("cell").asNumber(0);
    o.junctionLimitC = j.get("junctionLimit").asNumber(o.junctionLimitC);
    for (auto [key, map] : {std::pair{"power", &o.power}, {"thetaJB", &o.thetaJB}, {"heatsink", &o.heatsink}})
        if (j.get(key).isObject())
            for (const auto& [ref, v] : j.get(key).fields())
                if (v.isNumber()) (*map)[ref] = v.asNumber();
    return o;
}

Json thermalJson(const ThermalResult& r, bool withMap) {
    auto round1 = [](double v) { return std::round(v * 10) / 10; };
    Json j = Json::object();
    j["ambientC"] = r.ambientC;
    j["maxC"] = round1(r.maxC);
    Json h = Json::object();
    h["x"] = round1(r.hottest.x), h["y"] = round1(r.hottest.y);
    j["hottest"] = h;
    j["totalPowerW"] = std::round(r.totalPower * 1000) / 1000;
    j["converged"] = r.converged;
    j["iterations"] = r.iterations;
    Json parts = Json::array();
    for (const auto& p : r.parts) {
        Json e = Json::object();
        e["ref"] = p.ref;
        e["powerW"] = std::round(p.power * 1000) / 1000;
        e["powerSource"] = p.powerSource;
        e["boardC"] = round1(p.boardC);
        e["junctionC"] = round1(p.junctionC);
        e["thetaJB"] = round1(p.thetaJB);
        e["thetaEstimated"] = p.thetaEstimated;
        e["toAirW"] = std::round(p.toAirW * 1000) / 1000;  // straight from its body / heatsink to the air
        if (p.thetaHeatsink > 0) e["thetaHeatsink"] = round1(p.thetaHeatsink);
        e["x"] = round1(p.position.x), e["y"] = round1(p.position.y);
        parts.push(e);
    }
    j["parts"] = parts;
    Json f = Json::array();
    for (const auto& x : r.findings) {
        Json e = Json::object();
        e["code"] = x.code, e["severity"] = x.severity, e["message"] = x.message, e["ref"] = x.ref;
        f.push(e);
    }
    j["findings"] = f;
    Json notes = Json::array();
    for (const auto& s : r.notes) notes.push(s);
    j["notes"] = notes;
    if (withMap) {
        Json m = Json::object();
        m["x0"] = r.x0, m["y0"] = r.y0, m["cell"] = r.cell, m["nx"] = r.nx, m["ny"] = r.ny;
        Json t = Json::array();
        t.reserve(r.temperature.size());
        for (double v : r.temperature) t.push(std::isnan(v) ? Json() : Json(round1(v)));
        m["t"] = t;
        j["map"] = m;
    }
    return j;
}

}  // namespace sieda
