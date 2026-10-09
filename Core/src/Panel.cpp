// SiEDA Core — production panels (sieda/Panel.hpp).
#include "sieda/Panel.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

namespace sieda {
namespace {

constexpr double kTab = 5.0;        // breakaway tab width, mm
constexpr double kBite = 0.5;       // mouse-bite hole, mm
constexpr double kBitePitch = 0.75;
constexpr double kTooling = 2.0;    // tooling hole, mm

std::string fmt(const char* f, double v) {
    char b[32];
    std::snprintf(b, sizeof b, f, v);
    return b;
}

std::string gerberXY(Vec2 p) {
    return "X" + std::to_string(std::llround(p.x * 1e6)) + "Y" + std::to_string(std::llround(p.y * 1e6));
}

std::string gerberHeader(const std::string& function) {
    return "G04 SiEDA panel*\n%TF.GenerationSoftware,SiEDA,SiEDA,1.0*%\n%TF.FileFunction," + function +
           "*%\n%TF.FilePolarity,Positive*%\n%FSLAX46Y46*%\n%MOMM*%\n%LPD*%\n%ADD10C,0.100000*%\nD10*\n";
}

/// Removes the tab spans from an axis-aligned segment (a routed edge stops where a tab bridges the gap).
std::vector<std::pair<Vec2, Vec2>> cutTabs(Vec2 a, Vec2 b, const std::vector<Rect>& tabs) {
    std::vector<std::pair<Vec2, Vec2>> out{{a, b}};
    const bool horizontal = std::abs(a.y - b.y) < 1e-9, vertical = std::abs(a.x - b.x) < 1e-9;
    if (!horizontal && !vertical) return out;
    for (const Rect& t : tabs) {
        std::vector<std::pair<Vec2, Vec2>> next;
        for (auto [p, q] : out) {
            const double line = horizontal ? p.y : p.x;
            const bool touches = horizontal ? (std::abs(line - t.y0) < 1e-6 || std::abs(line - t.y1) < 1e-6)
                                            : (std::abs(line - t.x0) < 1e-6 || std::abs(line - t.x1) < 1e-6);
            double lo = horizontal ? std::min(p.x, q.x) : std::min(p.y, q.y);
            double hi = horizontal ? std::max(p.x, q.x) : std::max(p.y, q.y);
            const double c0 = horizontal ? t.x0 : t.y0, c1 = horizontal ? t.x1 : t.y1;
            if (!touches || c1 <= lo || c0 >= hi) {
                next.push_back({p, q});
                continue;
            }
            auto at = [&](double v) { return horizontal ? Vec2{v, line} : Vec2{line, v}; };
            if (c0 > lo) next.push_back({at(lo), at(c0)});
            if (c1 < hi) next.push_back({at(c1), at(hi)});
        }
        out = std::move(next);
    }
    return out;
}

/// Board Gerber → panel Gerber: shifted onto the first board, stepped nx × ny, plus `flashes` (aperture, points).
std::string stepGerber(const std::string& board, const PanelLayout& l, const PanelSettings& p,
                       const std::vector<std::pair<double, std::vector<Vec2>>>& flashes) {
    std::istringstream in(board);
    std::ostringstream out;
    std::string line;
    bool body = false;
    int maxCode = 9;
    const Vec2 shift = l.boards.front();
    const double gap = p.vscore ? 0 : p.gap;
    auto open = [&] {
        body = true;
        int code = maxCode;
        for (const auto& [diameter, points] : flashes)
            if (!points.empty()) out << "%ADD" << ++code << "C," << fmt("%.6f", diameter) << "*%\n";
        out << "%SRX" << std::max(1, p.nx) << "Y" << std::max(1, p.ny) << "I" << fmt("%.6f", l.boardWidth + gap) << "J"
            << fmt("%.6f", l.boardHeight + gap) << "*%\n";
    };
    while (std::getline(in, line)) {
        if (line.rfind("%ADD", 0) == 0) maxCode = std::max(maxCode, std::atoi(line.c_str() + 4));
        if (line == "M02*") {
            if (!body) open();  // a layer with no artwork: the block is empty, the panel extras still go in
            out << "%SR*%\n";
            for (const auto& [diameter, points] : flashes) {
                if (points.empty()) continue;
                out << "D" << ++maxCode << "*\n";
                for (Vec2 v : points) out << gerberXY(v) << "D03*\n";
            }
            out << "M02*\n";
            break;
        }
        if (!body && line[0] != '%' && line.rfind("G04", 0) != 0) open();
        if (body && line[0] == 'X') {
            // Coordinates lead the line (X…Y…); I / J offsets after them are relative and stay.
            char* end = nullptr;
            const long long x = std::strtoll(line.c_str() + 1, &end, 10);
            if (end && *end == 'Y') {
                const long long y = std::strtoll(end + 1, &end, 10);
                line = gerberXY({x / 1e6 + shift.x, y / 1e6 + shift.y}) + end;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

/// Board Excellon → panel Excellon: every hole once per board.
std::string stepDrill(const std::string& board, const PanelLayout& l) {
    std::istringstream in(board);
    std::ostringstream out;
    std::string line;
    while (std::getline(in, line)) {
        double x = 0, y = 0;
        char rest = 0;
        if (line[0] == 'X' && std::sscanf(line.c_str(), "X%lfY%lf%c", &x, &y, &rest) == 2) {
            for (Vec2 o : l.boards) out << "X" << fmt("%.3f", x + o.x) << "Y" << fmt("%.3f", y + o.y) << "\n";
            continue;
        }
        out << line << "\n";
    }
    return out.str();
}

std::string excellon(const std::map<double, std::vector<Vec2>>& holes) {
    std::ostringstream o;
    o << "M48\n; SiEDA panel drill file (non-plated: holes, tooling holes, mouse bites)\n"
      << "; #@! TF.FileFunction,NonPlated,1,2,NPTH\nFMAT,2\nMETRIC,TZ\n";
    int tool = 1;
    for (const auto& [d, _] : holes) o << "T" << tool++ << "C" << fmt("%.3f", d) << "\n";
    o << "%\nG90\nG05\n";
    tool = 1;
    for (const auto& [d, points] : holes) {
        o << "T" << tool++ << "\n";
        for (Vec2 v : points) o << "X" << fmt("%.3f", v.x) << "Y" << fmt("%.3f", v.y) << "\n";
    }
    o << "T0\nM30\n";
    return o.str();
}

}  // namespace

PanelLayout panelLayout(const BoardSettings& s) {
    const PanelSettings& p = s.panel;
    PanelLayout l;
    const int nx = std::max(1, p.nx), ny = std::max(1, p.ny);
    const double g = p.vscore ? 0 : p.gap, W = s.width, H = s.height;
    const double y0 = p.rail > 0 ? p.rail + g : 0;
    l.boardWidth = W;
    l.boardHeight = H;
    l.width = nx * W + (nx - 1) * g;
    l.height = ny * H + (ny - 1) * g + 2 * y0;
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) l.boards.push_back({i * (W + g), y0 + j * (H + g)});
    if (p.rail >= 4) {
        const double top = l.height - p.rail / 2, bottom = p.rail / 2;
        l.fiducials = {{10, bottom}, {l.width - 10, bottom}, {10, top}};
        l.toolingHoles = {{3.5, bottom}, {l.width - 3.5, bottom}, {3.5, top}, {l.width - 3.5, top}};
    }
    if (p.vscore) {
        for (int j = 0; j <= ny; ++j)
            if (p.rail > 0 || (j > 0 && j < ny)) l.vscores.push_back({{0, y0 + j * H}, {l.width, y0 + j * H}});
        for (int i = 1; i < nx; ++i) l.vscores.push_back({{i * W, 0}, {i * W, l.height}});
        return l;
    }
    if (g <= 0) return l;
    // Tabs across every gap a board faces (a neighbour or a rail): one per side, two on sides over 60 mm.
    auto centres = [](double from, double len) {
        std::vector<double> c;
        const int n = len > 60 ? 2 : 1;
        for (int k = 1; k <= n; ++k) c.push_back(from + len * k / (n + 1));
        return c;
    };
    auto bites = [&](Vec2 c, bool alongX) {
        for (int k = 0; k < 6; ++k) {
            const double d = (k - 2.5) * kBitePitch;
            l.mouseBites.push_back(alongX ? Vec2{c.x + d, c.y} : Vec2{c.x, c.y + d});
        }
    };
    for (int j = 0; j < ny; ++j)
        for (int i = 0; i < nx; ++i) {
            const Vec2 o = l.boards[static_cast<size_t>(j * nx + i)];
            const bool below = j > 0 || p.rail > 0, above = j == ny - 1 && p.rail > 0;
            for (double cx : centres(o.x, W)) {
                if (below) {
                    l.tabs.push_back({cx - kTab / 2, o.y - g, cx + kTab / 2, o.y});
                    bites({cx, o.y}, true);
                    if (j > 0) bites({cx, o.y - g}, true);
                }
                if (above) {
                    l.tabs.push_back({cx - kTab / 2, o.y + H, cx + kTab / 2, o.y + H + g});
                    bites({cx, o.y + H}, true);
                }
            }
            if (i > 0)
                for (double cy : centres(o.y, H)) {
                    l.tabs.push_back({o.x - g, cy - kTab / 2, o.x, cy + kTab / 2});
                    bites({o.x, cy}, false);
                    bites({o.x - g, cy}, false);
                }
        }
    return l;
}

namespace {
Json settingsJson(const PanelSettings& p) {
    Json j = Json::object();
    j["nx"] = p.nx;
    j["ny"] = p.ny;
    j["gap"] = p.gap;
    j["rail"] = p.rail;
    j["vscore"] = p.vscore;
    return j;
}
}  // namespace

Json panelToJson(const PanelSettings& p) { return p.enabled() ? settingsJson(p) : Json(); }

void panelFromJson(const Json& j, PanelSettings& p) {
    p = PanelSettings();
    if (!j.isObject()) return;
    auto count = [&](const char* k) { return static_cast<int>(std::clamp(j.get(k).asNumber(1), 1.0, 20.0)); };
    p.nx = count("nx");
    p.ny = count("ny");
    p.gap = std::isfinite(j.get("gap").asNumber(2.0)) ? std::clamp(j.get("gap").asNumber(2.0), 0.0, 20.0) : 2.0;
    p.rail = std::isfinite(j.get("rail").asNumber(5.0)) ? std::clamp(j.get("rail").asNumber(5.0), 0.0, 20.0) : 5.0;
    p.vscore = j.get("vscore").asBool(false);
}

PanelSettings fitPanel(const BoardSettings& s, double maxWidth, double maxHeight) {
    PanelSettings best = s.panel, p = s.panel;
    best.nx = best.ny = 1;
    double bestArea = 0;
    BoardSettings t = s;
    for (p.nx = 1; p.nx <= 20; ++p.nx)
        for (p.ny = 1; p.ny <= 20; ++p.ny) {
            if (p.nx * p.ny < 2 || p.nx * p.ny < best.nx * best.ny) continue;
            t.panel = p;
            const PanelLayout l = panelLayout(t);
            if (l.width > maxWidth + 1e-9 || l.height > maxHeight + 1e-9) continue;
            const double area = l.width * l.height + std::fabs(l.width - l.height);  // smaller, then squarer
            if (p.nx * p.ny > best.nx * best.ny || area < bestArea) best = p, bestArea = area;
        }
    return best;
}

Json panelLayoutJson(const BoardSettings& s) {
    const PanelLayout l = panelLayout(s);
    auto points = [](const std::vector<Vec2>& v) {
        Json a = Json::array();
        for (Vec2 p : v) {
            Json q = Json::array();
            q.push(p.x);
            q.push(p.y);
            a.push(q);
        }
        return a;
    };
    Json j = Json::object();
    j["settings"] = settingsJson(s.panel);
    j["enabled"] = s.panel.enabled();
    j["width"] = l.width;
    j["height"] = l.height;
    j["boardWidth"] = l.boardWidth;
    j["boardHeight"] = l.boardHeight;
    j["utilisation"] = l.width * l.height > 0 ? l.boardWidth * l.boardHeight * static_cast<double>(l.boards.size()) / (l.width * l.height) : 0.0;
    j["boards"] = points(l.boards);
    j["fiducials"] = points(l.fiducials);
    j["toolingHoles"] = points(l.toolingHoles);
    j["mouseBites"] = points(l.mouseBites);
    Json tabs = Json::array();
    for (const Rect& r : l.tabs) tabs.push(points({{r.x0, r.y0}, {r.x1, r.y1}}));
    j["tabs"] = tabs;
    Json vs = Json::array();
    for (const auto& [a, b] : l.vscores) vs.push(points({a, b}));
    j["vscores"] = vs;
    return j;
}

std::vector<FabFile> panelFiles(const PcbLayout& pcb, const std::string& base, const std::vector<FabFile>& boardFiles) {
    const BoardSettings& s = pcb.settings;
    if (!s.panel.enabled()) return {};
    const PanelLayout l = panelLayout(s);
    const std::string dir = "panel/" + base + "-panel";
    std::vector<FabFile> files;
    const std::string kind = s.panel.vscore ? "V-score" : "tabs and mouse bites";
    for (const auto& f : boardFiles) {
        if (f.path.rfind("gerbers/", 0) != 0) continue;
        const std::string name = f.path.substr(f.path.find('-', 8));  // "-F_Cu.gbr", "-PTH.drl", …
        const bool gbr = name.size() > 4 && name.compare(name.size() - 4, 4, ".gbr") == 0;
        const bool drl = name.size() > 4 && name.compare(name.size() - 4, 4, ".drl") == 0;
        if (name == "-Edge_Cuts.gbr" || name == "-NPTH.drl" || (!gbr && !drl)) continue;
        if (drl) {
            files.push_back({dir + name, stepDrill(f.content, l), "Panel: " + f.description});
            continue;
        }
        // Fiducials: copper dots on the outer layers, larger openings in their solder mask.
        double dot = 0;
        const bool copper = f.content.find("%TF.FileFunction,Copper") != std::string::npos;
        if (copper && (f.content.find(",Top*%") != std::string::npos || f.content.find(",Bot*%") != std::string::npos))
            dot = 1.0;
        if (f.content.find("%TF.FileFunction,Soldermask") != std::string::npos) dot = 2.0;
        files.push_back({dir + name, stepGerber(f.content, l, s.panel, {{dot, dot > 0 ? l.fiducials : std::vector<Vec2>{}}}),
                         "Panel: " + f.description});
    }

    // Outline: the panel frame; tab panels also route every board outline and the rails' inner edges, stopping at
    // the tabs.
    std::ostringstream edge;
    edge << gerberHeader("Profile,NP");
    auto segment = [&](Vec2 a, Vec2 b) {
        for (const auto& [p, q] : cutTabs(a, b, l.tabs)) edge << gerberXY(p) << "D02*\n" << gerberXY(q) << "D01*\n";
    };
    const Vec2 c0{0, 0}, c1{l.width, 0}, c2{l.width, l.height}, c3{0, l.height};
    edge << gerberXY(c0) << "D02*\n" << gerberXY(c1) << "D01*\n" << gerberXY(c2) << "D01*\n" << gerberXY(c3) << "D01*\n"
         << gerberXY(c0) << "D01*\n";
    if (!s.panel.vscore && s.panel.gap > 0) {
        const auto outline = s.outlinePolygon();
        for (Vec2 o : l.boards)
            for (size_t i = 0; i < outline.size(); ++i) {
                const Vec2 a = outline[i], b = outline[(i + 1) % outline.size()];
                segment({o.x + a.x, o.y + s.height - a.y}, {o.x + b.x, o.y + s.height - b.y});
            }
        if (s.panel.rail > 0) {
            segment({0, s.panel.rail}, {l.width, s.panel.rail});
            segment({0, l.height - s.panel.rail}, {l.width, l.height - s.panel.rail});
        }
    }
    edge << "M02*\n";
    files.push_back({dir + "-Edge_Cuts.gbr", edge.str(),
                     "Panel outline " + fmt("%.1f", l.width) + " × " + fmt("%.1f", l.height) + " mm, " +
                         std::to_string(s.panel.nx) + " × " + std::to_string(s.panel.ny) + " boards (" + kind + ")"});
    if (!l.vscores.empty()) {
        std::ostringstream v;
        v << gerberHeader("Other,V-score");
        for (const auto& [a, b] : l.vscores) v << gerberXY(a) << "D02*\n" << gerberXY(b) << "D01*\n";
        v << "M02*\n";
        files.push_back({dir + "-VScore.gbr", v.str(), "Panel V-score lines (score both sides, 1/3 depth each)"});
    }
    std::map<double, std::vector<Vec2>> npth;
    for (Vec2 o : l.boards)
        for (const auto& h : s.holes) npth[h.drill].push_back({o.x + h.position.x, o.y + s.height - h.position.y});
    for (Vec2 v : l.toolingHoles) npth[kTooling].push_back(v);
    for (Vec2 v : l.mouseBites) npth[kBite].push_back(v);
    if (!npth.empty())
        files.push_back({dir + "-NPTH.drl", excellon(npth), "Panel drill, non-plated (board holes, tooling holes, mouse bites)"});

    std::vector<std::pair<std::string, std::string>> zipped;
    for (const auto& f : files) zipped.push_back({f.path.substr(6), f.content});  // strip "panel/"
    files.push_back({"panel/" + base + "-panel.zip", makeZip(zipped), "Panel Gerbers + drills: upload this for a panel order"});
    return files;
}

}  // namespace sieda
