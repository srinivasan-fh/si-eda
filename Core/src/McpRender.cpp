// SiEDA Core — MCP render tools: the schematic (one sheet) or the board drawn as SVG or PNG, so an AI client can look
// at the design. A small display list feeds two back ends: SVG text and a palette PNG (own rasteriser and a
// fixed-Huffman deflate with run / previous-row matches; no image library). See docs/MCP.md.
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "SiedaProjectInternal.hpp"
#include "sieda/Export.hpp"
#include "sieda/Mcp.hpp"
#include "sieda/TrackGeometry.hpp"

namespace sieda::mcp {
namespace {

// ---- display list -----------------------------------------------------------------------------------------------

struct Prim {
    enum Kind { Line, Box, Disc, Polygon, Text } kind = Line;
    std::vector<Vec2> pts;  // Line: polyline; Box: two corners; Disc: centre; Polygon: outline; Text: origin
    double width = 0;       // stroke width (Line, outline Box / Polygon), radius (Disc), text height (Text)
    bool fill = false;
    std::string colour;
    std::string text;
    bool centred = false;   // Text: origin is the centre of the baseline
};

struct Scene {
    Rect bounds;
    std::string background = "#ffffff";
    std::vector<Prim> prims;

    void line(Vec2 a, Vec2 b, double w, const std::string& c) { prims.push_back({Prim::Line, {a, b}, w, false, c, {}, false}); }
    void polyline(std::vector<Vec2> p, double w, const std::string& c) {
        if (p.size() >= 2) prims.push_back({Prim::Line, std::move(p), w, false, c, {}, false});
    }
    void box(Rect r, bool fill, double w, const std::string& c) {
        prims.push_back({Prim::Box, {{r.x0, r.y0}, {r.x1, r.y1}}, w, fill, c, {}, false});
    }
    void disc(Vec2 p, double r, const std::string& c) { prims.push_back({Prim::Disc, {p}, r, true, c, {}, false}); }
    void polygon(std::vector<Vec2> p, bool fill, double w, const std::string& c) {
        if (p.size() >= 3) prims.push_back({Prim::Polygon, std::move(p), w, fill, c, {}, false});
    }
    void text(Vec2 at, const std::string& t, double h, const std::string& c, bool centred) {
        if (!t.empty()) prims.push_back({Prim::Text, {at}, h, true, c, t, centred});
    }
};

std::string fmt(double v) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%.3f", v);
    std::string s = buf;
    while (!s.empty() && s.back() == '0') s.pop_back();
    if (!s.empty() && s.back() == '.') s.pop_back();
    return s == "-0" ? "0" : s;
}

std::string xmlEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        switch (c) {
            case '<': out += "&lt;"; break;
            case '>': out += "&gt;"; break;
            case '&': out += "&amp;"; break;
            case '"': out += "&quot;"; break;
            default: out += c;
        }
    }
    return out;
}

std::string toSvg(const Scene& s, int width) {
    const double w = std::max(1e-6, s.bounds.width()), h = std::max(1e-6, s.bounds.height());
    const int pxW = std::clamp(width, 64, 8192);
    const int pxH = std::max(1, static_cast<int>(std::lround(pxW * h / w)));
    std::string o;
    o += "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"" + std::to_string(pxW) + "\" height=\"" + std::to_string(pxH) +
         "\" viewBox=\"" + fmt(s.bounds.x0) + " " + fmt(s.bounds.y0) + " " + fmt(w) + " " + fmt(h) + "\">\n";
    o += "<rect x=\"" + fmt(s.bounds.x0) + "\" y=\"" + fmt(s.bounds.y0) + "\" width=\"" + fmt(w) + "\" height=\"" + fmt(h) +
         "\" fill=\"" + s.background + "\"/>\n";
    for (const auto& p : s.prims) {
        switch (p.kind) {
            case Prim::Line: {
                o += "<polyline fill=\"none\" stroke=\"" + p.colour + "\" stroke-width=\"" + fmt(p.width) +
                     "\" stroke-linecap=\"round\" stroke-linejoin=\"round\" points=\"";
                for (const auto& v : p.pts) o += fmt(v.x) + "," + fmt(v.y) + " ";
                o += "\"/>\n";
                break;
            }
            case Prim::Box: {
                Rect r(p.pts[0].x, p.pts[0].y, p.pts[1].x, p.pts[1].y);
                o += "<rect x=\"" + fmt(r.x0) + "\" y=\"" + fmt(r.y0) + "\" width=\"" + fmt(r.width()) + "\" height=\"" +
                     fmt(r.height()) + "\" ";
                o += p.fill ? "fill=\"" + p.colour + "\"/>\n"
                            : "fill=\"none\" stroke=\"" + p.colour + "\" stroke-width=\"" + fmt(p.width) + "\"/>\n";
                break;
            }
            case Prim::Disc:
                o += "<circle cx=\"" + fmt(p.pts[0].x) + "\" cy=\"" + fmt(p.pts[0].y) + "\" r=\"" + fmt(p.width) +
                     "\" fill=\"" + p.colour + "\"/>\n";
                break;
            case Prim::Polygon: {
                o += "<polygon points=\"";
                for (const auto& v : p.pts) o += fmt(v.x) + "," + fmt(v.y) + " ";
                o += p.fill ? "\" fill=\"" + p.colour + "\"/>\n"
                            : "\" fill=\"none\" stroke=\"" + p.colour + "\" stroke-width=\"" + fmt(p.width) + "\"/>\n";
                break;
            }
            case Prim::Text:
                o += "<text x=\"" + fmt(p.pts[0].x) + "\" y=\"" + fmt(p.pts[0].y) + "\" font-family=\"monospace\" font-size=\"" +
                     fmt(p.width * 1.2) + "\" fill=\"" + p.colour + "\"" + (p.centred ? " text-anchor=\"middle\"" : "") + ">" +
                     xmlEscape(p.text) + "</text>\n";
                break;
        }
    }
    o += "</svg>\n";
    return o;
}

// ---- PNG ----------------------------------------------------------------------------------------------------------

uint32_t crc32(const uint8_t* data, size_t n, uint32_t crc = 0) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        ready = true;
    }
    crc = ~crc;
    for (size_t i = 0; i < n; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

struct BitWriter {
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    void put(uint32_t value, int n) {  // LSB first
        for (int i = 0; i < n; ++i) {
            acc |= ((value >> i) & 1u) << bits;
            if (++bits == 8) {
                out += static_cast<char>(acc & 0xFF);
                acc = 0;
                bits = 0;
            }
        }
    }
    void huff(uint32_t code, int n) {  // Huffman codes go most significant bit first
        for (int i = n - 1; i >= 0; --i) put((code >> i) & 1u, 1);
    }
    void flush() {
        if (bits > 0) out += static_cast<char>(acc & 0xFF);
        acc = 0;
        bits = 0;
    }
};

void literal(BitWriter& w, int v) {
    if (v < 144) w.huff(0x30 + v, 8);
    else if (v < 256) w.huff(0x190 + (v - 144), 9);
    else if (v < 280) w.huff(v - 256, 7);
    else w.huff(0xC0 + (v - 280), 8);
}

void match(BitWriter& w, int length, int distance) {
    static const int lenBase[] = {3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115,
                                  131, 163, 195, 227, 258};
    static const int lenExtra[] = {0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0};
    static const int distBase[] = {1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193, 257, 385, 513, 769, 1025, 1537,
                                   2049, 3073, 4097, 6145, 8193, 12289, 16385, 24577};
    static const int distExtra[] = {0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6, 7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13};
    int li = 28;  // the largest base not above the length (258 has a code of its own)
    while (li > 0 && lenBase[li] > length) --li;
    literal(w, 257 + li);
    w.put(static_cast<uint32_t>(length - lenBase[li]), lenExtra[li]);
    int di = 29;
    while (di > 0 && distBase[di] > distance) --di;
    w.huff(static_cast<uint32_t>(di), 5);
    w.put(static_cast<uint32_t>(distance - distBase[di]), distExtra[di]);
}

/// zlib stream (one fixed-Huffman block): runs (distance 1) and repeats of the row above (distance `stride`).
std::string zlibCompress(const std::string& data, size_t stride) {
    BitWriter w;
    w.out += static_cast<char>(0x78);
    w.out += static_cast<char>(0x01);
    w.put(1, 1);  // BFINAL
    w.put(1, 2);  // BTYPE = 01 fixed Huffman
    const size_t n = data.size();
    size_t i = 0;
    while (i < n) {
        size_t bestLen = 0, bestDist = 0;
        for (size_t d : {static_cast<size_t>(1), stride}) {
            if (d == 0 || d > 32768 || i < d) continue;
            size_t len = 0;
            while (len < 258 && i + len < n && data[i + len] == data[i + len - d]) ++len;
            if (len > bestLen) {
                bestLen = len;
                bestDist = d;
            }
        }
        if (bestLen >= 3) {
            match(w, static_cast<int>(bestLen), static_cast<int>(bestDist));
            i += bestLen;
        } else {
            literal(w, static_cast<uint8_t>(data[i]));
            ++i;
        }
    }
    literal(w, 256);
    w.flush();
    uint32_t a = 1, b = 0;
    for (unsigned char c : data) {
        a = (a + c) % 65521;
        b = (b + a) % 65521;
    }
    const uint32_t adler = (b << 16) | a;
    for (int k = 3; k >= 0; --k) w.out += static_cast<char>((adler >> (8 * k)) & 0xFF);
    return w.out;
}

void chunk(std::string& png, const char* type, const std::string& data) {
    const uint32_t len = static_cast<uint32_t>(data.size());
    for (int k = 3; k >= 0; --k) png += static_cast<char>((len >> (8 * k)) & 0xFF);
    std::string body = std::string(type, 4) + data;
    png += body;
    const uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(body.data()), body.size());
    for (int k = 3; k >= 0; --k) png += static_cast<char>((crc >> (8 * k)) & 0xFF);
}

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return 0;
}

struct Canvas {
    int w = 0, h = 0;
    std::vector<uint8_t> px;
    std::vector<std::string> palette;
    std::map<std::string, uint8_t> index;
    double scale = 1, ox = 0, oy = 0;

    uint8_t colour(const std::string& c) {
        auto it = index.find(c);
        if (it != index.end()) return it->second;
        if (palette.size() >= 256) return 0;
        const uint8_t i = static_cast<uint8_t>(palette.size());
        palette.push_back(c);
        index[c] = i;
        return i;
    }
    double X(double x) const { return (x - ox) * scale; }
    double Y(double y) const { return (y - oy) * scale; }
    void set(int x, int y, uint8_t c) {
        if (x >= 0 && y >= 0 && x < w && y < h) px[static_cast<size_t>(y) * static_cast<size_t>(w) + static_cast<size_t>(x)] = c;
    }
    void fillRect(double x0, double y0, double x1, double y1, uint8_t c) {
        const int a = std::max(0, static_cast<int>(std::floor(std::min(x0, x1))));
        const int b = std::min(w - 1, static_cast<int>(std::ceil(std::max(x0, x1))) - 1);
        const int t = std::max(0, static_cast<int>(std::floor(std::min(y0, y1))));
        const int u = std::min(h - 1, static_cast<int>(std::ceil(std::max(y0, y1))) - 1);
        for (int y = t; y <= u; ++y)
            for (int x = a; x <= b; ++x) set(x, y, c);
        if (a > b || t > u) set(static_cast<int>(std::floor((x0 + x1) / 2)), static_cast<int>(std::floor((y0 + y1) / 2)), c);
    }
    void disc(double cx, double cy, double r, uint8_t c) {
        r = std::max(r, 0.5);
        const int x0 = std::max(0, static_cast<int>(std::floor(cx - r))), x1 = std::min(w - 1, static_cast<int>(std::ceil(cx + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(cy - r))), y1 = std::min(h - 1, static_cast<int>(std::ceil(cy + r)));
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double dx = x + 0.5 - cx, dy = y + 0.5 - cy;
                if (dx * dx + dy * dy <= r * r) set(x, y, c);
            }
    }
    void segment(double ax, double ay, double bx, double by, double width, uint8_t c) {
        const double r = std::max(width / 2, 0.5);
        const int x0 = std::max(0, static_cast<int>(std::floor(std::min(ax, bx) - r)));
        const int x1 = std::min(w - 1, static_cast<int>(std::ceil(std::max(ax, bx) + r)));
        const int y0 = std::max(0, static_cast<int>(std::floor(std::min(ay, by) - r)));
        const int y1 = std::min(h - 1, static_cast<int>(std::ceil(std::max(ay, by) + r)));
        const double dx = bx - ax, dy = by - ay, len2 = dx * dx + dy * dy;
        for (int y = y0; y <= y1; ++y)
            for (int x = x0; x <= x1; ++x) {
                const double px_ = x + 0.5, py = y + 0.5;
                double t = len2 > 0 ? ((px_ - ax) * dx + (py - ay) * dy) / len2 : 0;
                t = std::clamp(t, 0.0, 1.0);
                const double ex = ax + t * dx - px_, ey = ay + t * dy - py;
                if (ex * ex + ey * ey <= r * r) set(x, y, c);
            }
    }
    void fillPolygon(const std::vector<Vec2>& pts, uint8_t c) {
        if (pts.size() < 3) return;
        double minY = 1e300, maxY = -1e300;
        for (const auto& p : pts) {
            minY = std::min(minY, Y(p.y));
            maxY = std::max(maxY, Y(p.y));
        }
        const int y0 = std::max(0, static_cast<int>(std::floor(minY))), y1 = std::min(h - 1, static_cast<int>(std::ceil(maxY)));
        std::vector<double> xs;
        for (int y = y0; y <= y1; ++y) {
            const double sy = y + 0.5;
            xs.clear();
            for (size_t i = 0; i < pts.size(); ++i) {
                const Vec2 a{X(pts[i].x), Y(pts[i].y)};
                const Vec2 b{X(pts[(i + 1) % pts.size()].x), Y(pts[(i + 1) % pts.size()].y)};
                if ((a.y <= sy && b.y > sy) || (b.y <= sy && a.y > sy)) xs.push_back(a.x + (sy - a.y) / (b.y - a.y) * (b.x - a.x));
            }
            std::sort(xs.begin(), xs.end());
            for (size_t k = 0; k + 1 < xs.size(); k += 2) {
                const int a = std::max(0, static_cast<int>(std::lround(xs[k])));
                const int b = std::min(w - 1, static_cast<int>(std::lround(xs[k + 1])) - 1);
                for (int x = a; x <= b; ++x) set(x, y, c);
            }
        }
    }
};

std::string toPng(const Scene& s, int width) {
    Canvas cv;
    const double bw = std::max(1e-6, s.bounds.width()), bh = std::max(1e-6, s.bounds.height());
    cv.w = std::clamp(width, 64, 2048);
    cv.scale = cv.w / bw;
    cv.h = std::clamp(static_cast<int>(std::lround(bh * cv.scale)), 1, 4096);
    cv.ox = s.bounds.x0;
    cv.oy = s.bounds.y0;
    cv.px.assign(static_cast<size_t>(cv.w) * static_cast<size_t>(cv.h), cv.colour(s.background));
    for (const auto& p : s.prims) {
        const uint8_t c = cv.colour(p.colour);
        switch (p.kind) {
            case Prim::Line:
                for (size_t i = 0; i + 1 < p.pts.size(); ++i)
                    cv.segment(cv.X(p.pts[i].x), cv.Y(p.pts[i].y), cv.X(p.pts[i + 1].x), cv.Y(p.pts[i + 1].y),
                               p.width * cv.scale, c);
                break;
            case Prim::Box: {
                const double x0 = cv.X(p.pts[0].x), y0 = cv.Y(p.pts[0].y), x1 = cv.X(p.pts[1].x), y1 = cv.Y(p.pts[1].y);
                if (p.fill) {
                    cv.fillRect(x0, y0, x1, y1, c);
                } else {
                    const double lw = p.width * cv.scale;
                    cv.segment(x0, y0, x1, y0, lw, c);
                    cv.segment(x1, y0, x1, y1, lw, c);
                    cv.segment(x1, y1, x0, y1, lw, c);
                    cv.segment(x0, y1, x0, y0, lw, c);
                }
                break;
            }
            case Prim::Disc: cv.disc(cv.X(p.pts[0].x), cv.Y(p.pts[0].y), p.width * cv.scale, c); break;
            case Prim::Polygon:
                if (p.fill) {
                    cv.fillPolygon(p.pts, c);
                } else {
                    for (size_t i = 0; i < p.pts.size(); ++i) {
                        const Vec2& a = p.pts[i];
                        const Vec2& b = p.pts[(i + 1) % p.pts.size()];
                        cv.segment(cv.X(a.x), cv.Y(a.y), cv.X(b.x), cv.Y(b.y), p.width * cv.scale, c);
                    }
                }
                break;
            case Prim::Text: {
                Vec2 at = p.pts[0];
                if (p.centred) at.x -= silkscreenTextWidth(p.text, p.width) / 2;
                const double stroke = std::max(1.0, p.width * cv.scale / 8);
                for (const auto& st : silkscreenText(p.text, at, p.width, false))
                    for (size_t i = 0; i + 1 < st.size(); ++i)
                        cv.segment(cv.X(st[i].x), cv.Y(st[i].y), cv.X(st[i + 1].x), cv.Y(st[i + 1].y), stroke, c);
                break;
            }
        }
    }
    std::string raw;
    raw.reserve(static_cast<size_t>(cv.h) * static_cast<size_t>(cv.w + 1));
    for (int y = 0; y < cv.h; ++y) {
        raw += '\0';  // filter: none
        raw.append(reinterpret_cast<const char*>(&cv.px[static_cast<size_t>(y) * static_cast<size_t>(cv.w)]),
                   static_cast<size_t>(cv.w));
    }
    std::string png = "\x89PNG\r\n\x1a\n";
    std::string ihdr;
    for (uint32_t v : {static_cast<uint32_t>(cv.w), static_cast<uint32_t>(cv.h)})
        for (int k = 3; k >= 0; --k) ihdr += static_cast<char>((v >> (8 * k)) & 0xFF);
    ihdr += static_cast<char>(8);  // bit depth
    ihdr += static_cast<char>(3);  // indexed colour
    ihdr += std::string(3, '\0');  // compression, filter, interlace
    chunk(png, "IHDR", ihdr);
    std::string plte;
    for (const auto& c : cv.palette) {
        const std::string h = c.size() == 7 ? c.substr(1) : std::string("000000");
        for (int k = 0; k < 3; ++k) plte += static_cast<char>(hexDigit(h[2 * k]) * 16 + hexDigit(h[2 * k + 1]));
    }
    chunk(png, "PLTE", plte);
    chunk(png, "IDAT", zlibCompress(raw, static_cast<size_t>(cv.w) + 1));
    chunk(png, "IEND", "");
    return png;
}

// ---- scenes -------------------------------------------------------------------------------------------------------

void grow(Rect& r, bool& any, Vec2 p) {
    if (!any) {
        r = Rect(p.x, p.y, p.x, p.y);
        any = true;
        return;
    }
    r.x0 = std::min(r.x0, p.x);
    r.y0 = std::min(r.y0, p.y);
    r.x1 = std::max(r.x1, p.x);
    r.y1 = std::max(r.y1, p.y);
}

Scene schematicScene(const Project& project, int sheet) {
    const Schematic& sch = project.schematic;
    if (sheet <= 0 || !sch.findSheet(sheet)) sheet = sch.activeSheet();
    Scene s;
    s.background = "#ffffff";
    Rect b;
    bool any = false;
    const std::string body = "#8b1a1a", wireC = "#1f6f3f", textC = "#202020", labelC = "#1a3d8b", pinC = "#555555";
    auto onSheet = [&](int id) {
        const Component* c = sch.find(id);
        return c && c->sheet == sheet && !c->packageOnly;
    };
    for (const auto& w : sch.wires()) {
        if (!onSheet(w.a.component) || !onSheet(w.b.component)) continue;
        const Vec2 a = sch.pinPosition(w.a), c = sch.pinPosition(w.b);
        s.line(a, c, 1.5, wireC);
        grow(b, any, a);
        grow(b, any, c);
    }
    for (const auto& bus : sch.buses()) {
        if (bus.sheet != sheet) continue;
        s.polyline(bus.points, 4, "#2a5caa");
        for (const auto& p : bus.points) grow(b, any, p);
        if (!bus.points.empty()) s.text(bus.points.front() + Vec2{0, -6}, bus.name, 8, labelC, false);
    }
    for (const auto& c : sch.components()) {
        if (c.sheet != sheet || c.packageOnly) continue;
        const auto o = sch.symbolOutline(c);
        grow(b, any, {o[0], o[1]});
        grow(b, any, {o[2], o[3]});
        const auto& pins = c.def().pins;
        switch (c.kind) {
            case ComponentKind::Junction: s.disc(c.position, 3, wireC); break;
            case ComponentKind::Ground: {
                const Vec2 p = c.position;
                s.line(p, p + Vec2{0, 10}, 1.5, body);
                s.line(p + Vec2{-10, 10}, p + Vec2{10, 10}, 1.5, body);
                s.line(p + Vec2{-6, 14}, p + Vec2{6, 14}, 1.5, body);
                s.line(p + Vec2{-2, 18}, p + Vec2{2, 18}, 1.5, body);
                break;
            }
            case ComponentKind::NetLabel: {
                const std::string name = sch.labelNetName(c).empty() ? c.value : sch.labelNetName(c);
                s.disc(c.position, 2, labelC);
                s.text(c.position + Vec2{4, -4}, name, 9, labelC, false);
                grow(b, any, c.position + Vec2{4 + 7.2 * static_cast<double>(name.size()), -14});
                break;
            }
            default: {
                // Body: the symbol outline without its pin leads (10 units each side where pins leave).
                double x0 = o[0], y0 = o[1], x1 = o[2], y1 = o[3];
                if (pins.size() >= 2) {
                    if (x1 - x0 > 30) x0 += 10, x1 -= 10;
                    if (y1 - y0 > 30) y0 += 10, y1 -= 10;
                }
                s.box(Rect(x0, y0, x1, y1), true, 0, "#fff7e6");
                s.box(Rect(x0, y0, x1, y1), false, 1.5, body);
                for (size_t i = 0; i < pins.size(); ++i) {
                    const Vec2 p = sch.pinPosition({c.id, static_cast<int>(i)});
                    // Lead from the pin to the nearest body edge.
                    Vec2 q = p;
                    const double dl = std::fabs(p.x - x0), dr = std::fabs(p.x - x1), dt = std::fabs(p.y - y0), db = std::fabs(p.y - y1);
                    const double m = std::min({dl, dr, dt, db});
                    if (m == dl) q.x = x0;
                    else if (m == dr) q.x = x1;
                    else if (m == dt) q.y = y0;
                    else q.y = y1;
                    if (p.x < x0 || p.x > x1 || p.y < y0 || p.y > y1) s.line(p, q, 1.2, pinC);
                    s.disc(p, 1.6, pinC);
                    if (pins.size() > 3 || c.kind == ComponentKind::Custom || c.kind == ComponentKind::PartUnit) {
                        const std::string& label = pins[i].name;
                        const bool left = m == dl, right = m == dr;
                        if (left) s.text(q + Vec2{2, 2.5}, label, 6, pinC, false);
                        else if (right) s.text(q + Vec2{-2 - 4.8 * static_cast<double>(label.size()), 2.5}, label, 6, pinC, false);
                        else s.text(q + Vec2{0, m == dt ? 8.0 : -3.0}, label, 6, pinC, true);
                    }
                }
                const std::string ref = sch.displayRef(c);
                s.text({(x0 + x1) / 2, y0 - 4}, ref, 8, textC, true);
                if (!c.value.empty()) s.text({(x0 + x1) / 2, y1 + 10}, c.value, 7, textC, true);
                grow(b, any, {(x0 + x1) / 2, y0 - 14});
                grow(b, any, {(x0 + x1) / 2, y1 + 12});
                break;
            }
        }
    }
    if (!any) b = Rect(0, 0, 200, 150);
    s.bounds = b.inflated(20);
    return s;
}

std::string layerColour(int layer, int layerCount) {
    if (layer == 0) return "#c83737";
    if (layer == layerCount - 1) return "#3c64c8";
    static const char* inner[] = {"#c8a03c", "#3ca050", "#a050c8", "#3cb4b4"};
    return inner[(layer - 1) % 4];
}

Scene pcbScene(const Project& project, const std::vector<int>& layersIn) {
    const Schematic& sch = project.schematic;
    const PcbLayout& pcb = project.pcb;
    const int n = std::max(1, pcb.settings.layerCount);
    std::vector<int> layers = layersIn;
    if (layers.empty())
        for (int l = 0; l < n; ++l) layers.push_back(l);
    auto shown = [&](int l) { return std::find(layers.begin(), layers.end(), l) != layers.end(); };
    Scene s;
    s.background = "#1a1a1a";
    const std::vector<Vec2> outline = pcb.settings.outlinePolygon();
    Rect b(0, 0, pcb.settings.width, pcb.settings.height);
    s.polygon(outline, true, 0, "#0d3b1e");
    // Bottom layer first, the top layer last.
    std::vector<int> order = layers;
    std::sort(order.begin(), order.end(), [](int a, int c) { return a > c; });
    const auto& fills = pcb.zoneFills(sch);
    for (int l : order) {
        const std::string col = layerColour(l, n);
        const std::string dim = l == 0 ? "#5a2020" : (l == n - 1 ? "#20305a" : "#4a4020");
        for (const auto& f : fills)
            if (f.layer == l)
                for (const auto& r : f.rects) s.box(r, true, 0, dim);
        for (const auto& t : pcb.tracks) {
            if (t.layer != l) continue;
            if (t.arc) {
                const ArcGeom g = trackArc(t);
                if (g.valid) {
                    std::vector<Vec2> pts;
                    const int steps = std::max(4, static_cast<int>(std::fabs(g.sweep) / (kPi / 24)));
                    for (int i = 0; i <= steps; ++i) pts.push_back(g.point(static_cast<double>(i) / steps));
                    s.polyline(pts, t.width, col);
                    continue;
                }
            }
            s.line(t.a, t.b, t.width, col);
        }
    }
    const std::string padC = "#d4af37";
    for (const auto& p : pcb.pads(sch)) {
        if (!p.throughHole && !shown(p.smdLayer)) continue;
        const Rect r = p.bounds();
        if (p.round && std::fabs(p.size.x - p.size.y) < 1e-6) {
            s.disc(p.position, p.size.x / 2, padC);
        } else if (p.round) {  // oval
            const bool wide = p.size.x > p.size.y;
            const double rad = std::min(p.size.x, p.size.y) / 2;
            const Vec2 a = wide ? Vec2{r.x0 + rad, p.position.y} : Vec2{p.position.x, r.y0 + rad};
            const Vec2 c = wide ? Vec2{r.x1 - rad, p.position.y} : Vec2{p.position.x, r.y1 - rad};
            s.line(a, c, 2 * rad, padC);
        } else {
            s.box(r, true, 0, padC);
        }
        if (p.throughHole && p.drill > 0) s.disc(p.position, p.drill / 2, "#101010");
    }
    for (const auto& v : pcb.vias) {
        s.disc(v.position, v.diameter / 2, "#b0b0b0");
        s.disc(v.position, v.drill / 2, "#101010");
    }
    for (const auto& h : pcb.settings.holes) {
        s.disc(h.position, h.keepout / 2, "#2a2a2a");
        s.disc(h.position, h.drill / 2, "#000000");
    }
    for (const auto& [a, c] : pcb.ratsnest(sch)) s.line(a, c, 0.08, "#ffff66");
    for (const auto& c : sch.components()) {
        if (!c.hasFootprint() || !c.pcb.placed || c.packageOnly) continue;
        const Rect cy = pcb.courtyard(c);
        s.box(cy, false, 0.08, "#e0e0e0");
        s.text({cy.center().x, cy.y0 - 0.3}, c.ref, 1.0, "#f0f0f0", true);
        b.x0 = std::min(b.x0, cy.x0);
        b.y0 = std::min(b.y0, cy.y0 - 1.5);
        b.x1 = std::max(b.x1, cy.x1);
        b.y1 = std::max(b.y1, cy.y1);
    }
    for (const auto& p : outline) {
        b.x0 = std::min(b.x0, p.x);
        b.y0 = std::min(b.y0, p.y);
        b.x1 = std::max(b.x1, p.x);
        b.y1 = std::max(b.y1, p.y);
    }
    s.polygon(outline, false, 0.15, "#e8e8e8");
    s.bounds = b.inflated(2);
    return s;
}

}  // namespace

std::string renderSchematicSvg(const SiedaProject& project, int sheet, int width) {
    return toSvg(schematicScene(project.project, sheet), width);
}
std::string renderSchematicPng(const SiedaProject& project, int sheet, int width) {
    return toPng(schematicScene(project.project, sheet), width);
}
std::string renderPcbSvg(const SiedaProject& project, const std::vector<int>& layers, int width) {
    return toSvg(pcbScene(project.project, layers), width);
}
std::string renderPcbPng(const SiedaProject& project, const std::vector<int>& layers, int width) {
    return toPng(pcbScene(project.project, layers), width);
}

std::string base64Encode(const std::string& bytes) {
    static const char* tbl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < bytes.size(); i += 3) {
        const uint32_t v = (static_cast<uint8_t>(bytes[i]) << 16) | (static_cast<uint8_t>(bytes[i + 1]) << 8) |
                           static_cast<uint8_t>(bytes[i + 2]);
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i < bytes.size()) {
        uint32_t v = static_cast<uint8_t>(bytes[i]) << 16;
        if (i + 1 < bytes.size()) v |= static_cast<uint8_t>(bytes[i + 1]) << 8;
        out += tbl[(v >> 18) & 63];
        out += tbl[(v >> 12) & 63];
        out += i + 1 < bytes.size() ? tbl[(v >> 6) & 63] : '=';
        out += '=';
    }
    return out;
}

}  // namespace sieda::mcp
