// SiEDA Core — IPC-2581C and ODB++ v7 writers (sieda/FabExchange.hpp), from one list of layer features.
#include "sieda/FabExchange.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <map>
#include <sstream>

#include "sieda/Export.hpp"
#include "sieda/Fabrication.hpp"
#include "sieda/Library.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda {
namespace {

struct Feature {
    enum Kind { Pad, Line, Arc, Box } kind = Pad;
    Vec2 a, b, c;            // pad / hole centre; line or arc ends; arc centre (Y up)
    double w = 0, h = 0;     // pad size or hole diameter; line width (w)
    bool round = false;      // round pad
    bool cw = false;         // clockwise arc
    bool plated = true;      // hole
    int net = -1;
    const Component* part = nullptr;  // pad of a part: its component and pad number
    int padNumber = 0;
};

struct Layer {
    std::string name;  // lower case (ODB++); IPC-2581 uses the upper-case form
    std::string odbType, ipcFunction;
    char side = 'T';   // T top, B bottom, I inner, A all (drills)
    std::string from, to;  // drill span (copper layer names)
    std::vector<Feature> features;
};

std::string num(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.4f", std::abs(v) < 5e-5 ? 0.0 : v);
    std::string s = b;
    while (s.back() == '0') s.pop_back();
    if (s.back() == '.') s.pop_back();
    return s;
}

std::string upper(std::string s) {
    for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    return s;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char ch : s) {
        switch (ch) {
            case '&': out += "&amp;"; break;
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '"': out += "&quot;"; break;
            default: out += ch;
        }
    }
    return out;
}

/// ODB++ names and fields: no spaces or semicolons.
std::string token(const std::string& s) {
    std::string out;
    for (char ch : s) out += (std::isspace(static_cast<unsigned char>(ch)) || ch == ';') ? '_' : ch;
    return out.empty() ? "_" : out;
}

std::string copperName(int layer, int count) {
    if (layer == 0) return "top";
    if (layer == count - 1) return "bottom";
    return "l" + std::to_string(layer + 1);
}

/// Every fabrication layer of the board as features in millimetres, Y up.
struct Board {
    const Project& project;
    const Schematic& sch;
    const PcbLayout& pcb;
    double height;
    int copper;
    std::vector<Pad> pads;
    std::vector<Layer> layers;

    explicit Board(const Project& p)
        : project(p), sch(p.schematic), pcb(p.pcb), height(p.pcb.settings.height),
          copper(std::max(1, p.pcb.settings.layerCount)), pads(p.pcb.pads(p.schematic)) {
        const auto& s = pcb.settings;
        auto padFeature = [&](const Pad& pad, double grow) {
            Feature f;
            f.a = up(pad.position);
            f.w = pad.size.x + 2 * grow;
            f.h = pad.size.y + 2 * grow;
            f.round = pad.round;
            f.net = pad.net;
            f.part = sch.find(pad.componentId);
            f.padNumber = pad.padNumber;
            return f;
        };
        auto silk = [&](bool bottomSide) {
            Layer l{bottomSide ? "ssb" : "sst", "SILK_SCREEN", "SILKSCREEN", bottomSide ? 'B' : 'T', "", "", {}};
            auto line = [&](Vec2 a, Vec2 b) {
                Feature f;
                f.kind = Feature::Line;
                f.a = up(a);
                f.b = up(b);
                f.w = 0.15;
                l.features.push_back(f);
            };
            for (const auto& c : sch.components()) {
                if (!c.hasFootprint() || !c.pcb.placed || c.pcb.bottom != bottomSide || c.pcb.embedded()) continue;
                Rect r = pcb.courtyard(c).inflated(-0.15);
                line({r.x0, r.y0}, {r.x1, r.y0});
                line({r.x1, r.y0}, {r.x1, r.y1});
                line({r.x1, r.y1}, {r.x0, r.y1});
                line({r.x0, r.y1}, {r.x0, r.y0});
                const double textH = 1.0, width = silkscreenTextWidth(c.ref, textH);
                double baseline = r.y0 - 0.45;
                if (baseline - textH < s.edgeClearance) baseline = r.y1 + 0.45 + textH;
                Vec2 origin{(r.x0 + r.x1) / 2 + (bottomSide ? width / 2 : -width / 2), baseline};
                for (const auto& stroke : silkscreenText(c.ref, origin, textH, bottomSide))
                    for (size_t i = 1; i < stroke.size(); ++i) line(stroke[i - 1], stroke[i]);
            }
            return l;
        };
        auto mask = [&](bool bottomSide) {
            Layer l{bottomSide ? "smb" : "smt", "SOLDER_MASK", "SOLDERMASK", bottomSide ? 'B' : 'T', "", "", {}};
            const int cl = bottomSide ? copper - 1 : 0;
            for (const auto& pad : pads)
                if (pad.onLayer(cl)) l.features.push_back(padFeature(pad, 0.05));
            return l;
        };

        layers.push_back(silk(false));
        layers.push_back(mask(false));
        for (int cl = 0; cl < copper; ++cl) {
            Layer l{copperName(cl, copper), "SIGNAL", "SIGNAL", cl == 0 ? 'T' : cl == copper - 1 ? 'B' : 'I', "", "", {}};
            for (const auto& fill : pcb.zoneFills(sch)) {
                if (fill.layer != cl) continue;
                for (const auto& r : fill.rects) {
                    Feature f;
                    f.kind = Feature::Box;
                    f.a = up({r.x0, r.y1});
                    f.b = up({r.x1, r.y0});
                    f.net = fill.net;
                    l.features.push_back(f);
                }
            }
            for (const auto& t : pcb.tracks) {
                if (t.layer != cl) continue;
                Feature f;
                f.kind = Feature::Line;
                f.a = up(t.a);
                f.b = up(t.b);
                f.w = t.width;
                f.net = t.net;
                if (const ArcGeom g = trackArc(t); g.valid) {
                    f.kind = Feature::Arc;
                    f.c = up(g.c);
                    f.cw = g.sweep > 0;  // counter-clockwise on the board (Y down) is clockwise with Y up
                }
                l.features.push_back(f);
            }
            for (const auto& pad : pads)
                if (pad.onLayer(cl)) l.features.push_back(padFeature(pad, 0));
            for (const auto& v : pcb.vias) {
                if (!v.spans(cl)) continue;
                Feature f;
                f.a = up(v.position);
                f.w = f.h = v.diameter;
                f.round = true;
                f.net = v.net;
                l.features.push_back(f);
            }
            layers.push_back(l);
        }
        if (copper > 1) layers.push_back(mask(true));
        if (std::any_of(sch.components().begin(), sch.components().end(),
                        [](const Component& c) { return c.pcb.placed && c.pcb.bottom; }))
            layers.push_back(silk(true));

        // Drills: plated through holes (pads and through vias), each blind / buried span, non-plated mounting holes.
        auto hole = [&](Vec2 at, double d, int net, bool plated) {
            Feature f;
            f.a = up(at);
            f.w = f.h = d;
            f.round = true;
            f.net = net;
            f.plated = plated;
            return f;
        };
        const std::string top = copperName(0, copper), bottom = copperName(copper - 1, copper);
        Layer through{"drill", "DRILL", "DRILL", 'A', top, bottom, {}};
        for (const auto& pad : pads)
            if (pad.throughHole && pad.drill > 0) through.features.push_back(hole(pad.position, pad.drill, pad.net, true));
        std::map<std::pair<int, int>, Layer> spans;
        for (const auto& v : pcb.vias) {
            if (v.isThrough()) {
                through.features.push_back(hole(v.position, v.drill, v.net, true));
                continue;
            }
            const int from = v.fromLayer, to = v.lastLayer(copper);
            auto& l = spans[{from, to}];
            if (l.name.empty())
                l = Layer{"drill_" + std::to_string(from + 1) + "-" + std::to_string(to + 1), "DRILL", "DRILL", 'A',
                          copperName(from, copper), copperName(to, copper), {}};
            l.features.push_back(hole(v.position, v.drill, v.net, true));
        }
        for (const auto& h : s.holes) through.features.push_back(hole(h.position, h.drill, -1, false));
        if (!through.features.empty()) layers.push_back(through);
        for (auto& entry : spans) layers.push_back(entry.second);
    }

    Vec2 up(Vec2 v) const { return {v.x, height - v.y}; }

    std::vector<Vec2> profile() const {
        std::vector<Vec2> poly;
        for (const auto& p : pcb.settings.outlinePolygon()) poly.push_back(up(p));
        return poly;
    }

    std::string netName(int net) const {
        const auto& nets = sch.nets();
        return net >= 0 && net < static_cast<int>(nets.size()) ? nets[static_cast<size_t>(net)].name : "";
    }

    /// Parts on the board, top side first.
    std::vector<const Component*> placed(bool bottomSide) const {
        std::vector<const Component*> out;
        for (const auto& c : sch.components())
            if (c.hasFootprint() && c.pcb.placed && c.pcb.bottom == bottomSide && !c.pcb.embedded()) out.push_back(&c);
        return out;
    }

    /// The pads of one part (board space).
    std::vector<const Pad*> padsOf(const Component& c) const {
        std::vector<const Pad*> out;
        for (const auto& p : pads)
            if (p.componentId == c.id) out.push_back(&p);
        return out;
    }
};

// MARK: - IPC-2581

std::string exportIpc(const Board& board) {
    const Project& project = board.project;
    std::ostringstream o;
    std::map<std::string, std::string> shapes;  // id → primitive
    auto padShape = [&](const Feature& f) {
        std::string id = f.round ? "CIRCLE_" + num(f.w) : "RECT_" + num(f.w) + "X" + num(f.h);
        shapes.emplace(id, f.round ? "<Circle diameter=\"" + num(f.w) + "\"/>"
                                   : "<RectCenter width=\"" + num(f.w) + "\" height=\"" + num(f.h) + "\"/>");
        return id;
    };
    std::map<std::string, double> lineDescs;
    auto lineDesc = [&](double w) {
        std::string id = "LINE_" + num(w);
        lineDescs.emplace(id, w);
        return id;
    };
    auto xy = [](const char* px, const char* py, Vec2 v) {
        return std::string(" ") + px + "=\"" + num(v.x) + "\" " + py + "=\"" + num(v.y) + "\"";
    };
    auto polygon = [&](const std::vector<Vec2>& poly) {
        std::string s = "<Polygon><PolyBegin" + xy("x", "y", poly[0]) + "/>";
        for (size_t i = 1; i <= poly.size(); ++i) s += "<PolyStepSegment" + xy("x", "y", poly[i % poly.size()]) + "/>";
        return s + "</Polygon>";
    };

    // Step body first: it fills the dictionaries the Content section lists.
    std::ostringstream step;
    step << "<Step name=\"pcb\"><Datum x=\"0\" y=\"0\"/><Profile>" << polygon(board.profile()) << "</Profile>\n";
    std::map<std::string, const Component*> packages;
    for (bool bottomSide : {false, true})
        for (const Component* c : board.placed(bottomSide)) packages.emplace(c->footprintName(), c);
    for (const auto& [name, c] : packages) {
        const FootprintDef* fp = Library::instance().footprint(name);
        step << "<Package name=\"" << xmlEscape(name) << "\" type=\"OTHER\" pinOne=\"1\" pinOneOrientation=\"OTHER\"";
        if (fp && fp->body.height > 0) step << " height=\"" << num(fp->body.height) << "\"";
        step << ">";
        const double w = fp && fp->courtyardW > 0 ? fp->courtyardW / 2 : 0.5, h = fp && fp->courtyardH > 0 ? fp->courtyardH / 2 : 0.5;
        step << "<Outline>" << polygon({{-w, -h}, {w, -h}, {w, h}, {-w, h}}) << "<LineDescRef id=\"" << lineDesc(0.1)
             << "\"/></Outline>";
        if (fp)
            for (size_t i = 0; i < fp->pads.size(); ++i) {
                const auto& pd = fp->pads[i];
                Feature f;
                f.w = pd.size.x;
                f.h = pd.size.y;
                f.round = pd.round;
                step << "<Pin number=\"" << i + 1 << "\" type=\"" << (pd.throughHole ? "THRU" : "SURFACE")
                     << "\" electricalType=\"" << (pd.pinIndex >= 0 ? "ELECTRICAL" : "MECHANICAL") << "\"><Location"
                     << xy("x", "y", {pd.offset.x, -pd.offset.y}) << "/><StandardPrimitiveRef id=\"" << padShape(f)
                     << "\"/></Pin>";
            }
        step << "</Package>\n";
    }
    for (bool bottomSide : {false, true})
        for (const Component* c : board.placed(bottomSide)) {
            const FootprintDef* fp = Library::instance().footprint(c->footprintName());
            const bool th = fp && std::any_of(fp->pads.begin(), fp->pads.end(), [](const PadDef& p) { return p.throughHole; });
            step << "<Component refDes=\"" << xmlEscape(c->ref) << "\" packageRef=\"" << xmlEscape(c->footprintName())
                 << "\" layerRef=\"" << (bottomSide ? upper(copperName(board.copper - 1, board.copper)) : "TOP")
                 << "\" part=\"" << xmlEscape(c->value) << "\" mountType=\"" << (th ? "THMT" : "SMT") << "\"><Xform rotation=\""
                 << (360 - c->pcb.rotation % 360) % 360 << "\"" << (bottomSide ? " mirror=\"true\"" : "") << "/><Location"
                 << xy("x", "y", board.up(c->pcb.position)) << "/></Component>\n";
        }
    std::map<int, std::vector<const Pad*>> netPads;
    for (const auto& p : board.pads)
        if (p.net >= 0 && board.sch.find(p.componentId)) netPads[p.net].push_back(&p);
    for (const auto& [net, list] : netPads) {
        step << "<LogicalNet name=\"" << xmlEscape(board.netName(net)) << "\">";
        for (const Pad* p : list)
            step << "<PinRef componentRef=\"" << xmlEscape(board.sch.find(p->componentId)->ref) << "\" pin=\""
                 << p->padNumber << "\"/>";
        step << "</LogicalNet>\n";
    }
    for (const auto& layer : board.layers) {
        if (layer.features.empty()) continue;
        step << "<LayerFeature layerRef=\"" << upper(layer.name) << "\">\n";
        std::map<int, std::vector<const Feature*>> byNet;
        for (const auto& f : layer.features) byNet[f.net].push_back(&f);
        for (const auto& [net, list] : byNet) {
            step << "<Set" << (net >= 0 ? " net=\"" + xmlEscape(board.netName(net)) + "\"" : std::string());
            const bool drill = layer.odbType == "DRILL";
            step << ">";
            for (const Feature* f : list) {
                if (drill) {
                    step << "<Hole name=\"H" << num(f->w) << "\" diameter=\"" << num(f->w) << "\" platingStatus=\""
                         << (f->plated ? "PLATED" : "NONPLATED") << "\" plusTol=\"0\" minusTol=\"0\"" << xy("x", "y", f->a)
                         << "/>";
                } else if (f->kind == Feature::Pad) {
                    step << "<Pad><Location" << xy("x", "y", f->a) << "/><StandardPrimitiveRef id=\"" << padShape(*f) << "\"/>";
                    if (f->part) step << "<PinRef componentRef=\"" << xmlEscape(f->part->ref) << "\" pin=\"" << f->padNumber << "\"/>";
                    step << "</Pad>";
                } else if (f->kind == Feature::Box) {
                    step << "<Features><Contour>"
                         << polygon({f->a, {f->b.x, f->a.y}, f->b, {f->a.x, f->b.y}}) << "</Contour></Features>";
                } else {
                    step << "<Features><" << (f->kind == Feature::Arc ? "Arc" : "Line") << xy("startX", "startY", f->a)
                         << xy("endX", "endY", f->b);
                    if (f->kind == Feature::Arc)
                        step << xy("centerX", "centerY", f->c) << " clockwise=\"" << (f->cw ? "true" : "false") << "\"";
                    step << "><LineDescRef id=\"" << lineDesc(f->w) << "\"/></" << (f->kind == Feature::Arc ? "Arc" : "Line")
                         << "></Features>";
                }
            }
            step << "</Set>\n";
        }
        step << "</LayerFeature>\n";
    }
    step << "</Step>\n";

    // BOM rows: one per value + package.
    std::map<std::pair<std::string, std::string>, std::vector<const Component*>> bom;
    for (bool bottomSide : {false, true})
        for (const Component* c : board.placed(bottomSide)) bom[{c->value, c->footprintName()}].push_back(c);

    const std::string job = xmlEscape(project.name.empty() ? "board" : project.name);
    o << "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
      << "<IPC-2581 revision=\"C\" xmlns=\"http://webstds.ipc.org/2581\">\n"
      << "<Content roleRef=\"Owner\"><FunctionMode mode=\"FABRICATION\"/><StepRef name=\"pcb\"/>";
    for (const auto& layer : board.layers) o << "<LayerRef name=\"" << upper(layer.name) << "\"/>";
    o << "<BomRef name=\"" << job << "_bom\"/><DictionaryLineDesc units=\"MILLIMETER\">";
    for (const auto& [id, w] : lineDescs)
        o << "<EntryLineDesc id=\"" << id << "\"><LineDesc lineWidth=\"" << num(w) << "\" lineEnd=\"ROUND\"/></EntryLineDesc>";
    o << "</DictionaryLineDesc><DictionaryStandard units=\"MILLIMETER\">";
    for (const auto& [id, shape] : shapes) o << "<EntryStandard id=\"" << id << "\">" << shape << "</EntryStandard>";
    o << "</DictionaryStandard></Content>\n"
      << "<LogisticHeader><Role id=\"Owner\" roleFunction=\"SENDER\"/><Enterprise id=\"SiEDA\" code=\"NONE\"/>"
      << "<Person name=\"SiEDA\" enterpriseRef=\"SiEDA\" roleRef=\"Owner\"/></LogisticHeader>\n"
      << "<HistoryRecord number=\"1\" origination=\"2000-01-01T00:00:00\" software=\"SiEDA\" lastChange=\"2000-01-01T00:00:00\">"
      << "<FileRevision fileRevisionId=\"1\" comment=\"\"><SoftwarePackage name=\"SiEDA\" revision=\"1\" vendor=\"SiEDA\">"
      << "<Certification certificationStatus=\"SELFTEST\"/></SoftwarePackage></FileRevision></HistoryRecord>\n"
      << "<Bom name=\"" << job << "_bom\"><BomHeader assembly=\"" << job << "\" revision=\"1\"><StepRef name=\"pcb\"/></BomHeader>\n";
    for (const auto& [key, parts] : bom) {
        o << "<BomItem OEMDesignNumberRef=\"" << xmlEscape(key.first + " " + key.second) << "\" quantity=\"" << parts.size()
          << "\" category=\"ELECTRICAL\">";
        for (const Component* c : parts)
            o << "<RefDes name=\"" << xmlEscape(c->ref) << "\" packageRef=\"" << xmlEscape(key.second) << "\" populate=\""
              << (c->sourcing.dnp ? "false" : "true") << "\" layerRef=\""
              << (c->pcb.bottom ? upper(copperName(board.copper - 1, board.copper)) : "TOP") << "\"/>";
        o << "<Characteristics category=\"ELECTRICAL\"/></BomItem>\n";
    }
    o << "</Bom>\n<Ecad name=\"" << job << "\"><CadHeader units=\"MILLIMETER\"/><CadData>\n";
    for (const auto& layer : board.layers) {
        const char* side = layer.side == 'T' ? "TOP" : layer.side == 'B' ? "BOTTOM" : layer.side == 'I' ? "INTERNAL" : "ALL";
        o << "<Layer name=\"" << upper(layer.name) << "\" layerFunction=\"" << layer.ipcFunction << "\" side=\"" << side
          << "\" polarity=\"POSITIVE\"";
        if (layer.odbType == "DRILL")
            o << "><Span fromLayer=\"" << upper(layer.from) << "\" toLayer=\"" << upper(layer.to) << "\"/></Layer>\n";
        else
            o << "/>\n";
    }
    o << step.str() << "</CadData></Ecad>\n</IPC-2581>\n";
    return o.str();
}

// MARK: - ODB++

std::string odbFeatures(const Layer& layer) {
    std::map<std::string, int> symbols;
    auto symbol = [&](const std::string& name) { return symbols.emplace(name, static_cast<int>(symbols.size())).first->second; };
    auto microns = [](double mm) { return num(mm * 1000); };
    std::ostringstream body;
    for (const auto& f : layer.features) {
        switch (f.kind) {
            case Feature::Pad:
                body << "P " << num(f.a.x) << " " << num(f.a.y) << " "
                     << symbol(f.round ? "r" + microns(f.w) : "rect" + microns(f.w) + "x" + microns(f.h)) << " P 0 0\n";
                break;
            case Feature::Line:
                body << "L " << num(f.a.x) << " " << num(f.a.y) << " " << num(f.b.x) << " " << num(f.b.y) << " "
                     << symbol("r" + microns(f.w)) << " P 0\n";
                break;
            case Feature::Arc:
                body << "A " << num(f.a.x) << " " << num(f.a.y) << " " << num(f.b.x) << " " << num(f.b.y) << " "
                     << num(f.c.x) << " " << num(f.c.y) << " " << symbol("r" + microns(f.w)) << " P 0 " << (f.cw ? "Y" : "N")
                     << "\n";
                break;
            case Feature::Box:
                body << "S P 0\nOB " << num(f.a.x) << " " << num(f.a.y) << " I\nOS " << num(f.b.x) << " " << num(f.a.y)
                     << "\nOS " << num(f.b.x) << " " << num(f.b.y) << "\nOS " << num(f.a.x) << " " << num(f.b.y) << "\nOS "
                     << num(f.a.x) << " " << num(f.a.y) << "\nOE\nSE\n";
                break;
        }
    }
    std::vector<std::string> names(symbols.size());
    for (const auto& [name, i] : symbols) names[static_cast<size_t>(i)] = name;
    std::ostringstream o;
    o << "UNITS=MM\n#\n#Feature symbol names\n#\n";
    for (size_t i = 0; i < names.size(); ++i) o << "$" << i << " " << names[i] << "\n";
    o << "#\n#Layer features\n#\n" << body.str();
    return o.str();
}

/// ustar archive of `files`, gzip-wrapped with stored (uncompressed) deflate blocks.
std::string tgz(const std::vector<std::pair<std::string, std::string>>& files) {
    std::string tar;
    for (const auto& [path, data] : files) {
        std::string h(512, '\0');
        auto field = [&](size_t at, size_t len, const std::string& v) { h.replace(at, std::min(len, v.size()), v.substr(0, len)); };
        auto octal = [](unsigned long long v, int width) {
            char b[24];
            std::snprintf(b, sizeof b, "%0*llo", width, v);
            return std::string(b);
        };
        field(0, 100, path);
        field(100, 8, "0000644");
        field(108, 8, "0000000");
        field(116, 8, "0000000");
        field(124, 12, octal(data.size(), 11));
        field(136, 12, octal(0, 11));
        h[156] = '0';
        field(257, 6, "ustar");
        field(263, 2, "00");
        h.replace(148, 8, 8, ' ');
        unsigned sum = 0;
        for (unsigned char ch : h) sum += ch;
        field(148, 8, octal(sum, 6));
        h[154] = '\0';
        tar += h;
        tar += data;
        tar.append((512 - data.size() % 512) % 512, '\0');
    }
    tar.append(1024, '\0');

    std::string gz = {'\x1f', '\x8b', '\x08', '\0', '\0', '\0', '\0', '\0', '\0', '\xff'};
    for (size_t at = 0; at < tar.size(); at += 65535) {
        const size_t n = std::min<size_t>(65535, tar.size() - at);
        gz += static_cast<char>(at + n >= tar.size() ? 1 : 0);
        for (unsigned v : {static_cast<unsigned>(n), static_cast<unsigned>(~n & 0xFFFF)}) {
            gz += static_cast<char>(v & 0xFF);
            gz += static_cast<char>((v >> 8) & 0xFF);
        }
        gz.append(tar, at, n);
    }
    for (uint32_t v : {crc32(tar), static_cast<uint32_t>(tar.size())})
        for (int i = 0; i < 4; ++i) gz += static_cast<char>((v >> (8 * i)) & 0xFF);
    return gz;
}

std::string exportOdb(const Board& board) {
    const std::string job = token(board.project.name.empty() ? "board" : board.project.name).substr(0, 40);
    const std::string step = job + "/steps/pcb/";
    std::vector<std::pair<std::string, std::string>> files;

    // Matrix: component and copper rows in stack order, drills last.
    std::vector<std::string> rows;
    std::ostringstream matrix;
    matrix << "STEP {\n   COL=1\n   NAME=PCB\n}\n\n";
    int row = 0;
    auto matrixRow = [&](const std::string& name, const std::string& type, const std::string& from, const std::string& to) {
        matrix << "LAYER {\n   ROW=" << ++row << "\n   CONTEXT=BOARD\n   TYPE=" << type << "\n   NAME=" << name
               << "\n   POLARITY=POSITIVE\n   START_NAME=" << from << "\n   END_NAME=" << to << "\n   OLD_NAME=\n}\n\n";
    };
    const bool anyBottom = !board.placed(true).empty();
    matrixRow("comp_+_top", "COMPONENT", "", "");
    for (const auto& l : board.layers)
        if (l.odbType != "DRILL") matrixRow(l.name, l.odbType, "", "");
    if (anyBottom) matrixRow("comp_+_bot", "COMPONENT", "", "");
    for (const auto& l : board.layers)
        if (l.odbType == "DRILL") matrixRow(l.name, "DRILL", l.from, l.to);
    files.push_back({job + "/matrix/matrix", matrix.str()});
    files.push_back({job + "/misc/info", "JOB_NAME=" + job + "\nODB_VERSION_MAJOR=7\nODB_VERSION_MINOR=0\nODB_SOURCE=SiEDA\n"
                                          "CREATION_DATE=20000101.000000\nSAVE_DATE=20000101.000000\nSAVE_APP=SiEDA\n"
                                          "SAVE_USER=SiEDA\nUNITS=MM\n"});
    files.push_back({step + "stephdr", "UNITS=MM\nX_DATUM=0\nY_DATUM=0\nX_ORIGIN=0\nY_ORIGIN=0\n"});

    std::ostringstream profile;
    const auto poly = board.profile();
    profile << "UNITS=MM\n#\n#Num Features\n#\nS P 0\nOB " << num(poly[0].x) << " " << num(poly[0].y) << " I\n";
    for (size_t i = 1; i <= poly.size(); ++i) profile << "OS " << num(poly[i % poly.size()].x) << " " << num(poly[i % poly.size()].y) << "\n";
    profile << "OE\nSE\n";
    files.push_back({step + "profile", profile.str()});
    for (const auto& l : board.layers) files.push_back({step + "layers/" + l.name + "/features", odbFeatures(l)});

    // eda/data: nets with their pins (subnets), then packages; component layers refer to both by index.
    const auto& nets = board.sch.nets();
    std::vector<std::vector<std::string>> subnets(nets.size() + 1);  // last: pads without a net
    std::map<std::pair<int, int>, std::pair<int, int>> pinNet;      // (component id, pad number) → (net, subnet)
    std::map<std::string, int> packageIndex;
    std::ostringstream packages;
    for (bool bottomSide : {false, true}) {
        const auto parts = board.placed(bottomSide);
        for (size_t ci = 0; ci < parts.size(); ++ci) {
            const Component* c = parts[ci];
            auto pads = board.padsOf(*c);
            for (size_t ti = 0; ti < pads.size(); ++ti) {
                const int net = pads[ti]->net >= 0 && pads[ti]->net < static_cast<int>(nets.size()) ? pads[ti]->net
                                                                                                       : static_cast<int>(nets.size());
                auto& list = subnets[static_cast<size_t>(net)];
                pinNet[{c->id, pads[ti]->padNumber}] = {net, static_cast<int>(list.size())};
                list.push_back(std::string("SNT TOP ") + (bottomSide ? "B " : "T ") + std::to_string(ci) + " " + std::to_string(ti));
            }
            const std::string& name = c->footprintName();
            if (packageIndex.count(name)) continue;
            packageIndex[name] = static_cast<int>(packageIndex.size());
            const FootprintDef* fp = Library::instance().footprint(name);
            const double w = fp ? fp->courtyardW / 2 : 1, h = fp ? fp->courtyardH / 2 : 1;
            packages << "PKG " << token(name) << " 1 " << num(-w) << " " << num(-h) << " " << num(w) << " " << num(h) << "\n"
                     << "RC " << num(-w) << " " << num(-h) << " " << num(2 * w) << " " << num(2 * h) << "\n";
            if (fp)
                for (size_t i = 0; i < fp->pads.size(); ++i) {
                    const auto& pd = fp->pads[i];
                    packages << "PIN " << i + 1 << " " << (pd.throughHole ? "T" : "S") << " " << num(pd.offset.x) << " "
                             << num(-pd.offset.y) << " 0 " << (pd.pinIndex >= 0 ? "E" : "M") << " "
                             << (pd.throughHole ? "T" : "S") << "\n";
                    if (pd.round)
                        packages << "CR " << num(pd.offset.x) << " " << num(-pd.offset.y) << " " << num(pd.size.x / 2) << "\n";
                    else
                        packages << "RC " << num(pd.offset.x - pd.size.x / 2) << " " << num(-pd.offset.y - pd.size.y / 2) << " "
                                 << num(pd.size.x) << " " << num(pd.size.y) << "\n";
                }
            packages << "#\n";
        }
    }
    std::ostringstream eda;
    eda << "HDR SiEDA\nUNITS=MM\nLYR";
    for (int cl = 0; cl < board.copper; ++cl) eda << " " << copperName(cl, board.copper);
    eda << "\n#\n";
    for (size_t n = 0; n < subnets.size(); ++n) {
        eda << "NET " << (n < nets.size() ? token(nets[n].name) : std::string("$NONE$")) << "\n";
        for (const auto& s : subnets[n]) eda << s << "\n";
        eda << "#\n";
    }
    eda << packages.str();
    files.push_back({step + "eda/data", eda.str()});

    for (bool bottomSide : {false, true}) {
        const auto parts = board.placed(bottomSide);
        if (bottomSide && parts.empty()) continue;
        std::ostringstream comp;
        comp << "UNITS=MM\n";
        for (size_t ci = 0; ci < parts.size(); ++ci) {
            const Component* c = parts[ci];
            const Vec2 at = board.up(c->pcb.position);
            comp << "# CMP " << ci << "\nCMP " << packageIndex[c->footprintName()] << " " << num(at.x) << " " << num(at.y) << " "
                 << c->pcb.rotation % 360 << " " << (bottomSide ? "M" : "N") << " " << token(c->ref) << " " << token(c->value)
                 << "\n";
            auto pads = board.padsOf(*c);
            for (size_t ti = 0; ti < pads.size(); ++ti) {
                const auto [net, sub] = pinNet[{c->id, pads[ti]->padNumber}];
                const Vec2 p = board.up(pads[ti]->position);
                comp << "TOP " << ti << " " << num(p.x) << " " << num(p.y) << " " << c->pcb.rotation % 360 << " "
                     << (bottomSide ? "M" : "N") << " " << net << " " << sub << " " << pads[ti]->padNumber << "\n";
            }
        }
        files.push_back({step + "layers/" + (bottomSide ? "comp_+_bot" : "comp_+_top") + "/components", comp.str()});
    }
    return tgz(files);
}

}  // namespace

std::string exportIpc2581(const Project& project) { return exportIpc(Board(project)); }

std::string exportOdbArchive(const Project& project) { return exportOdb(Board(project)); }

}  // namespace sieda
