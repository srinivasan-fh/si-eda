// SiEDA Core — schematic PDF: every sheet on its drawing template with frame, zones, title block and bookmarks.
//
// The PDF is written by hand (no library): uncompressed content streams in plain ASCII, the base-14 Helvetica fonts,
// one page per sheet and an outline tree that mirrors the sheet hierarchy. Symbols are simplified vector drawings
// (the app's canvas is richer): two-pin parts as boxes, capacitor plates or diode triangles between their pins,
// other parts as a body box with their pin stubs and names, ground symbols, label flags and sheet symbols.
#include "sieda/SchematicPdf.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <set>
#include <sstream>

#include "sieda/CustomParts.hpp"
#include "sieda/PdfFont.hpp"
#include "sieda/Project.hpp"

namespace sieda {

const std::vector<SheetTemplate>& sheetTemplates() {
    static const std::vector<SheetTemplate> list = {
        {"A4", 297, 210},          {"A3", 420, 297},          {"A2", 594, 420},          {"A1", 841, 594},
        {"A0", 1189, 841},         {"ANSI A", 279.4, 215.9},  {"ANSI B", 431.8, 279.4},  {"ANSI C", 558.8, 431.8},
        {"ANSI D", 863.6, 558.8},  {"ANSI E", 1117.6, 863.6},
    };
    return list;
}

const SheetTemplate* findSheetTemplate(const std::string& name) {
    for (const auto& t : sheetTemplates())
        if (t.name == name) return &t;
    return nullptr;
}

namespace {
constexpr double kPt = 72.0 / 25.4;  // points per millimetre
constexpr double kMargin = 10;       // mm, page edge to frame
constexpr double kZone = 5;          // mm, zone marker strip inside the frame
constexpr double kTitleW = 130, kTitleH = 32;

struct Box {
    double minX = 1e300, minY = 1e300, maxX = -1e300, maxY = -1e300;
    void add(Vec2 p) {
        minX = std::min(minX, p.x);
        minY = std::min(minY, p.y);
        maxX = std::max(maxX, p.x);
        maxY = std::max(maxY, p.y);
    }
    bool empty() const { return minX > maxX; }
};

/// Drawing extent of a sheet (schematic units): parts with their pins, and buses.
Box sheetExtent(const Schematic& s, int sheet) {
    Box b;
    for (const auto& c : s.components()) {
        if (c.sheet != sheet || c.packageOnly) continue;
        b.add(c.position + Vec2{-20, -20});
        b.add(c.position + Vec2{20, 20});
        for (size_t p = 0; p < c.def().pins.size(); ++p) b.add(s.pinPosition({c.id, static_cast<int>(p)}));
    }
    for (const auto& bus : s.buses())
        if (bus.sheet == sheet)
            for (const auto& p : bus.points) b.add(p);
    return b;
}

/// UTF-8 → code points (an invalid sequence gives U+FFFD).
std::vector<uint32_t> codepoints(const std::string& s) {
    std::vector<uint32_t> out;
    for (size_t i = 0; i < s.size();) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        int extra = c < 0x80 ? 0 : (c & 0xE0) == 0xC0 ? 1 : (c & 0xF0) == 0xE0 ? 2 : (c & 0xF8) == 0xF0 ? 3 : -1;
        uint32_t cp = extra == 0 ? c : extra == 1 ? (c & 0x1Fu) : extra == 2 ? (c & 0x0Fu) : (c & 0x07u);
        size_t k = 1;
        for (; extra > 0 && k <= static_cast<size_t>(extra); ++k) {
            if (i + k >= s.size() || (static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) {
                extra = -1;
                break;
            }
            cp = (cp << 6) | (static_cast<unsigned char>(s[i + k]) & 0x3Fu);
        }
        if (extra < 0) {
            out.push_back(0xFFFD);
            ++i;
            continue;
        }
        out.push_back(cp);
        i += k;
    }
    return out;
}

/// WinAnsiEncoding byte of a code point (0 = not in it).
unsigned char winAnsi(uint32_t cp) {
    if ((cp >= 0x20 && cp < 0x7F) || (cp >= 0xA0 && cp <= 0xFF)) return static_cast<unsigned char>(cp);
    static const std::map<uint32_t, unsigned char> extra = {
        {0x20AC, 0x80}, {0x201A, 0x82}, {0x0192, 0x83}, {0x201E, 0x84}, {0x2026, 0x85}, {0x2020, 0x86}, {0x2021, 0x87},
        {0x02C6, 0x88}, {0x2030, 0x89}, {0x0160, 0x8A}, {0x2039, 0x8B}, {0x0152, 0x8C}, {0x017D, 0x8E}, {0x2018, 0x91},
        {0x2019, 0x92}, {0x201C, 0x93}, {0x201D, 0x94}, {0x2022, 0x95}, {0x2013, 0x96}, {0x2014, 0x97}, {0x02DC, 0x98},
        {0x2122, 0x99}, {0x0161, 0x9A}, {0x203A, 0x9B}, {0x0153, 0x9C}, {0x017E, 0x9E}, {0x0178, 0x9F}};
    const auto it = extra.find(cp);
    return it == extra.end() ? 0 : it->second;
}

/// Symbol font byte of a Greek letter or math sign (0 = none): Ω, π, ≤, ∞ …
unsigned char symbolFont(uint32_t cp) {
    static const char upper[] = "ABGDEZHQIKLMNXOPR\0STUFCYW";  // Α … Ω (U+0391 …, U+03A2 unused)
    static const char lower[] = "abgdezhqiklmnxoprVstufcyw";   // α … ω (U+03B1 …, ς = V)
    if (cp >= 0x0391 && cp <= 0x03A9 && upper[cp - 0x0391]) return static_cast<unsigned char>(upper[cp - 0x0391]);
    if (cp >= 0x03B1 && cp <= 0x03C9) return static_cast<unsigned char>(lower[cp - 0x03B1]);
    static const std::map<uint32_t, unsigned char> signs = {
        {0x2264, 0xA3}, {0x2265, 0xB3}, {0x2260, 0xB9}, {0x221E, 0xA5}, {0x2248, 0xBB}, {0x2192, 0xAE}, {0x2190, 0xAC},
        {0x2191, 0xAD}, {0x2193, 0xAF}, {0x221A, 0xD6}, {0x2211, 0xE5}, {0x2206, 0x44}, {0x2126, 0x57}, {0x00B5, 0x6D},
        {0x2022, 0xB7}, {0x00D7, 0xB4}, {0x2212, 0x2D}, {0x2194, 0xAB}, {0x2202, 0xB6}, {0x222B, 0xF2}};
    const auto it = signs.find(cp);
    return it == signs.end() ? 0 : it->second;
}

/// A PDF literal string of raw bytes (bytes outside printable ASCII as octal escapes: the file stays ASCII).
std::string pdfBytes(const std::string& bytes) {
    std::string out = "(";
    for (char ch : bytes) {
        const unsigned char c = static_cast<unsigned char>(ch);
        if (ch == '(' || ch == ')' || ch == '\\') {
            out += '\\';
            out += ch;
        } else if (c < 0x20 || c >= 0x7F) {
            char esc[8];
            std::snprintf(esc, sizeof esc, "\\%03o", c);
            out += esc;
        } else {
            out += ch;
        }
    }
    return out + ")";
}

/// A PDF text string (bookmark titles): plain when ASCII, else UTF-16BE with a byte-order mark.
std::string pdfString(const std::string& s) {
    const auto cps = codepoints(s);
    if (std::all_of(cps.begin(), cps.end(), [](uint32_t c) { return c >= 0x20 && c < 0x7F; })) return pdfBytes(s);
    std::string hex = "<FEFF";
    char b[8];
    for (uint32_t c : cps) {
        if (c > 0x10FFFF || (c >= 0xD800 && c < 0xE000)) c = 0xFFFD;
        if (c >= 0x10000) {
            const uint32_t v = c - 0x10000;
            std::snprintf(b, sizeof b, "%04X", static_cast<unsigned>(0xD800 + (v >> 10)));
            hex += b;
            c = 0xDC00 + (v & 0x3FF);
        }
        std::snprintf(b, sizeof b, "%04X", static_cast<unsigned>(c));
        hex += b;
    }
    return hex + ">";
}

/// Fonts of the document: the embedded TrueType font (when given) and the glyphs pages use from it.
struct PdfFonts {
    const TrueTypeFont* font = nullptr;
    std::set<int> glyphs;
    std::map<int, uint32_t> unicodeOf;  // glyph → code point (ToUnicode, for copying text out of the PDF)
};

/// Content stream writer in millimetres (y up from the page bottom).
class Canvas {
public:
    explicit Canvas(double pageHeightMm) : h_(pageHeightMm) {}
    void width(double mm) { out_ << num(mm * kPt) << " w\n"; }
    void gray(double g) { out_ << num(g) << " G " << num(g) << " g\n"; }
    void line(double x1, double y1, double x2, double y2) {
        out_ << num(x1 * kPt) << ' ' << num((h_ - y1) * kPt) << " m " << num(x2 * kPt) << ' ' << num((h_ - y2) * kPt) << " l S\n";
    }
    void rect(double x, double y, double w, double hh, bool fill = false) {
        out_ << num(x * kPt) << ' ' << num((h_ - y - hh) * kPt) << ' ' << num(w * kPt) << ' ' << num(hh * kPt) << " re "
             << (fill ? "f" : "S") << '\n';
    }
    void polygon(const std::vector<Vec2>& pts, bool fill) {
        if (pts.size() < 2) return;
        out_ << num(pts[0].x * kPt) << ' ' << num((h_ - pts[0].y) * kPt) << " m";
        for (size_t i = 1; i < pts.size(); ++i) out_ << ' ' << num(pts[i].x * kPt) << ' ' << num((h_ - pts[i].y) * kPt) << " l";
        out_ << (fill ? " f\n" : " s\n");
    }
    /// An open polyline, or a closed outline (filled light, stroked) when `closed`.
    void path(const std::vector<Vec2>& pts, bool closed, bool fill) {
        if (pts.size() < 2) return;
        if (fill) out_ << "0.92 g\n";
        out_ << num(pts[0].x * kPt) << ' ' << num((h_ - pts[0].y) * kPt) << " m";
        for (size_t i = 1; i < pts.size(); ++i) out_ << ' ' << num(pts[i].x * kPt) << ' ' << num((h_ - pts[i].y) * kPt) << " l";
        out_ << (!closed ? " S\n" : fill ? " b\n0 g\n" : " s\n");
    }
    void dot(double x, double y, double r) {
        rect(x - r, y - r, 2 * r, 2 * r, true);
    }
    /// Text with its baseline at y; align 0 left, 1 centre, 2 right (approximate Helvetica width). Latin text in
    /// Helvetica, Greek and math signs in Symbol, anything else from the embedded font ('?' without one).
    void text(double x, double y, double sizeMm, const std::string& s, int align = 0, bool bold = false) {
        struct Run {
            int font;  // 1 Helvetica (2 bold), 3 Symbol, 4 embedded
            std::string bytes;
        };
        std::vector<Run> runs;
        double width = 0;
        auto add = [&](int font, const std::string& bytes, double w) {
            if (runs.empty() || runs.back().font != font) runs.push_back({font, std::string()});
            runs.back().bytes += bytes;
            width += w;
        };
        const int latin = bold ? 2 : 1;
        for (uint32_t cp : codepoints(s)) {
            if (cp < 0x20) continue;
            if (const unsigned char b = winAnsi(cp)) {
                add(latin, std::string(1, static_cast<char>(b)), 0.52 * sizeMm);
            } else if (const unsigned char sym = symbolFont(cp)) {
                add(3, std::string(1, static_cast<char>(sym)), 0.55 * sizeMm);
            } else if (const int g = fonts_ && fonts_->font ? fonts_->font->glyph(cp) : 0) {
                fonts_->glyphs.insert(g);
                fonts_->unicodeOf.emplace(g, cp);
                add(4, std::string{static_cast<char>(g >> 8), static_cast<char>(g & 0xFF)}, fonts_->font->advance(g) * sizeMm);
            } else {
                add(latin, "?", 0.52 * sizeMm);
            }
        }
        if (runs.empty()) return;
        const double left = align == 1 ? x - width / 2 : align == 2 ? x - width : x;
        out_ << "BT";
        for (size_t i = 0; i < runs.size(); ++i) {
            out_ << " /F" << runs[i].font << ' ' << num(sizeMm * kPt) << " Tf";
            if (i == 0) out_ << ' ' << num(left * kPt) << ' ' << num((h_ - y) * kPt) << " Td";
            if (runs[i].font == 4) {
                out_ << " <";
                char hex[4];
                for (char ch : runs[i].bytes) {
                    std::snprintf(hex, sizeof hex, "%02X", static_cast<unsigned char>(ch));
                    out_ << hex;
                }
                out_ << "> Tj";
            } else {
                out_ << ' ' << pdfBytes(runs[i].bytes) << " Tj";
            }
        }
        out_ << " ET\n";
    }
    void useFonts(PdfFonts* fonts) { fonts_ = fonts; }
    std::string str() const { return out_.str(); }

private:
    static std::string num(double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", std::fabs(v) < 0.005 ? 0.0 : v);
        return b;
    }
    double h_;
    std::ostringstream out_;
    PdfFonts* fonts_ = nullptr;
};

/// Places a schematic point on the page.
struct Mapping {
    double scale = kSchematicUnitMm;  // mm per unit
    double ox = 0, oy = 0;            // page mm of schematic (0, 0)
    Vec2 operator()(Vec2 p) const { return {ox + p.x * scale, oy + p.y * scale}; }
};

/// A part's free-form symbol drawings (Symbol Editor graphics), turned with the component and placed on the page.
void drawSymbolGraphics(Canvas& cv, const Mapping& m, const Component& c, const std::vector<SymbolGraphic>& graphics) {
    const double u = m.scale;
    auto at = [&](Vec2 p) { return m(c.position + rotate90(p, c.rotation)); };
    for (const auto& g : graphics) {
        if (g.points.empty()) continue;
        cv.width(std::max(0.1, 1.4 * u * std::max(0.25, g.lineWidth)));
        std::vector<Vec2> pts;
        bool closed = false;
        if (g.kind == "rect" && g.points.size() >= 2) {
            const Vec2 a = g.points[0], b = g.points[1];
            pts = {at(a), at({b.x, a.y}), at(b), at({a.x, b.y})};
            closed = true;
        } else if (g.kind == "circle" || g.kind == "arc") {
            const bool full = g.kind == "circle";
            const double a0 = full ? 0 : g.startAngle, a1 = full ? 360 : g.endAngle;
            const double sweep = std::clamp(a1 - a0, -360.0, 360.0);
            const int steps = std::max(4, static_cast<int>(std::ceil(std::fabs(sweep) / 6)));
            for (int i = 0; i <= steps; ++i) {
                const double rad = (a0 + sweep * i / steps) * 3.14159265358979323846 / 180;
                pts.push_back(at({g.points[0].x + g.radius * std::cos(rad), g.points[0].y + g.radius * std::sin(rad)}));
            }
            closed = full;
        } else if (g.kind == "text") {
            const Vec2 p = at(g.points[0]);
            cv.text(p.x, p.y + 0.35 * g.size * u, std::max(0.8, g.size * u), g.text);
            continue;
        } else {
            for (const auto& p : g.points) pts.push_back(at(p));
            closed = g.kind == "polygon";
        }
        cv.path(pts, closed, closed && g.fill);
    }
}

/// Draws in a component's symbol coordinates (grid units, y down), turned with the component and placed on the page.
struct SymbolPen {
    Canvas& cv;
    const Mapping& m;
    Vec2 pos;
    int rotation;
    Vec2 at(double x, double y) const { return m(pos + rotate90({x, y}, rotation)); }
    void line(double x1, double y1, double x2, double y2) {
        const Vec2 a = at(x1, y1), b = at(x2, y2);
        cv.line(a.x, a.y, b.x, b.y);
    }
    void shape(std::vector<Vec2> pts, bool closed, bool fill) {
        for (auto& p : pts) p = at(p.x, p.y);
        cv.path(pts, closed, fill);
    }
    void solid(std::vector<Vec2> pts) {
        for (auto& p : pts) p = at(p.x, p.y);
        cv.polygon(pts, true);
    }
    std::vector<Vec2> arcPoints(double cx, double cy, double r, double a0, double a1, int steps = 16) const {
        std::vector<Vec2> pts;
        for (int i = 0; i <= steps; ++i) {
            const double a = (a0 + (a1 - a0) * i / steps) * 3.14159265358979323846 / 180;
            pts.push_back({cx + r * std::cos(a), cy + r * std::sin(a)});
        }
        return pts;
    }
    void circle(double cx, double cy, double r, bool fill) { shape(arcPoints(cx, cy, r, 0, 360, 32), true, fill); }
    void rect(double x, double y, double w, double h, bool fill) { shape({{x, y}, {x + w, y}, {x + w, y + h}, {x, y + h}}, true, fill); }
    void arrow(double x1, double y1, double x2, double y2, double size) {
        line(x1, y1, x2, y2);
        const double a = std::atan2(y2 - y1, x2 - x1);
        solid({{x2, y2}, {x2 - size * std::cos(a - 0.45), y2 - size * std::sin(a - 0.45)},
               {x2 - size * std::cos(a + 0.45), y2 - size * std::sin(a + 0.45)}});
    }
    void sine(double x0, double x1, double amp) {
        std::vector<Vec2> pts;
        for (int i = 0; i <= 24; ++i) {
            const double t = static_cast<double>(i) / 24;
            pts.push_back({x0 + (x1 - x0) * t, -amp * std::sin(t * 2 * 3.14159265358979323846)});
        }
        shape(pts, false, false);
    }
};

/// The schematic symbol of a built-in part, as the canvas draws it (SchematicSymbols.shapes). False for kinds drawn
/// elsewhere (library parts, labels, junctions).
bool drawBuiltinSymbol(SymbolPen& p, ComponentKind kind, const std::string& value) {
    std::string upper = value;
    for (auto& ch : upper) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
    switch (kind) {
        case ComponentKind::Resistor: {
            p.line(-30, 0, -15, 0);
            p.line(15, 0, 30, 0);
            std::vector<Vec2> zig{{-15, 0}};
            const double xs[] = {-12.5, -7.5, -2.5, 2.5, 7.5, 12.5};
            for (int i = 0; i < 6; ++i) zig.push_back({xs[i], i % 2 == 0 ? -6.0 : 6.0});
            zig.push_back({15, 0});
            p.shape(zig, false, false);
            return true;
        }
        case ComponentKind::Capacitor:
            p.line(-30, 0, -3, 0);
            p.line(3, 0, 30, 0);
            p.line(-3, -11, -3, 11);
            p.line(3, -11, 3, 11);
            return true;
        case ComponentKind::Inductor:
            p.line(-30, 0, -16, 0);
            p.line(16, 0, 30, 0);
            for (int i = 0; i < 4; ++i) p.shape(p.arcPoints(-12 + 8 * i, 0, 4, 180, 360, 10), false, false);
            return true;
        case ComponentKind::Diode:
        case ComponentKind::LED:
            p.line(-30, 0, -8, 0);
            p.line(8, 0, 30, 0);
            p.shape({{-8, -8}, {-8, 8}, {8, 0}}, true, true);
            p.line(8, -8, 8, 8);
            if (kind == ComponentKind::LED) {
                p.arrow(0, -10, 7, -19, 4);
                p.arrow(6, -8, 13, -17, 4);
            }
            return true;
        case ComponentKind::VoltageSource:
        case ComponentKind::ACSource:
        case ComponentKind::CurrentSource:
            p.circle(0, 0, 15, true);
            p.line(0, -30, 0, -15);
            p.line(0, 15, 0, 30);
            if (kind == ComponentKind::CurrentSource) {
                p.arrow(0, 9, 0, -9, 5);
            } else if (kind == ComponentKind::ACSource || upper.rfind("SIN", 0) == 0) {
                p.sine(-8, 8, 6);
            } else if (upper.rfind("PULSE", 0) == 0) {
                p.shape({{-8, 5}, {-4, 5}, {-4, -5}, {4, -5}, {4, 5}, {8, 5}}, false, false);
            } else {
                p.line(-4, -8, 4, -8);
                p.line(0, -12, 0, -4);
                p.line(-4, 8, 4, 8);
            }
            return true;
        case ComponentKind::Battery:
            p.line(0, -30, 0, -10);
            p.line(0, 10, 0, 30);
            for (double y : {-10.0, 2.0}) {
                p.line(-12, y, 12, y);
                p.solid({{-6, y + 6}, {6, y + 6}, {6, y + 8.4}, {-6, y + 8.4}});
            }
            p.line(0, -2, 0, 2);
            p.line(9, -20, 15, -20);
            p.line(12, -23, 12, -17);
            return true;
        case ComponentKind::Ground:
            p.line(0, 0, 0, 8);
            p.line(-11, 8, 11, 8);
            p.line(-7, 12, 7, 12);
            p.line(-3, 16, 3, 16);
            return true;
        case ComponentKind::NPN:
            p.circle(5, 0, 21, true);
            p.line(-30, 0, -4, 0);
            p.line(-4, -12, -4, 12);
            p.line(-4, -5, 20, -20);
            p.line(20, -20, 20, -30);
            p.arrow(-4, 5, 17, 18, 5);
            p.line(17, 18, 20, 20);
            p.line(20, 20, 20, 30);
            return true;
        case ComponentKind::NMOS:
            p.line(-30, 0, -10, 0);
            p.line(-10, -12, -10, 12);
            p.line(-4, -14, -4, -6);
            p.line(-4, -4, -4, 4);
            p.line(-4, 6, -4, 14);
            p.line(-4, -10, 20, -10);
            p.line(20, -10, 20, -30);
            p.line(-4, 10, 20, 10);
            p.line(20, 10, 20, 30);
            p.line(20, 0, 20, 10);
            p.arrow(20, 0, -2, 0, 5);
            return true;
        case ComponentKind::OpAmp:
            p.shape({{-25, -26}, {-25, 26}, {30, 0}}, true, true);
            p.line(-40, -10, -25, -10);
            p.line(-40, 10, -25, 10);
            p.line(30, 0, 40, 0);
            p.line(-21, -10, -15, -10);
            p.line(-21, 10, -15, 10);
            p.line(-18, 7, -18, 13);
            return true;
        case ComponentKind::Switch: {
            p.line(-30, 0, -12, 0);
            p.line(12, 0, 30, 0);
            p.circle(-12, 0, 2, false);
            p.circle(12, 0, 2, false);
            std::string lower = value;
            for (auto& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            const bool closed = lower == "on" || lower == "closed" || lower == "1" || lower == "true";
            p.line(-12, 0, closed ? 12 : 7, closed ? 0 : -17);
            return true;
        }
        case ComponentKind::Connector:
            p.rect(-10, -20, 16, 40, true);
            p.line(-20, -10, -10, -10);
            p.line(-20, 10, -10, 10);
            p.solid({{-4, -12}, {0, -12}, {0, -8}, {-4, -8}});
            p.solid({{-4, 8}, {0, 8}, {0, 12}, {-4, 12}});
            return true;
        case ComponentKind::IC8:
            p.rect(-30, -40, 60, 80, true);
            p.shape(p.arcPoints(0, -40, 6, 180, 0, 10), false, false);
            for (double y : {-30.0, -10.0, 10.0, 30.0}) {
                p.line(-40, y, -30, y);
                p.line(30, y, 40, y);
            }
            return true;
        case ComponentKind::Fuse:
            p.line(-30, 0, -15, 0);
            p.line(15, 0, 30, 0);
            p.rect(-15, -5, 30, 10, true);
            p.line(-15, 0, 15, 0);
            return true;
        default:
            return false;
    }
}

void drawFrame(Canvas& cv, const SheetTemplate& t) {
    const double x0 = kMargin, y0 = kMargin, x1 = t.widthMm - kMargin, y1 = t.heightMm - kMargin;
    cv.gray(0);
    cv.width(0.5);
    cv.rect(x0, y0, x1 - x0, y1 - y0);
    cv.width(0.25);
    cv.rect(x0 + kZone, y0 + kZone, x1 - x0 - 2 * kZone, y1 - y0 - 2 * kZone);
    // Zones: numbers across, letters down, about 50 mm each.
    const int cols = std::max(2, static_cast<int>(std::round((x1 - x0) / 50)));
    const int rows = std::max(2, static_cast<int>(std::round((y1 - y0) / 50)));
    const double cw = (x1 - x0) / cols, rh = (y1 - y0) / rows;
    for (int i = 0; i < cols; ++i) {
        const double cx = x0 + cw * (i + 0.5);
        if (i > 0) {
            cv.line(x0 + cw * i, y0, x0 + cw * i, y0 + kZone);
            cv.line(x0 + cw * i, y1 - kZone, x0 + cw * i, y1);
        }
        cv.text(cx, y0 + kZone - 1.3, 2.5, std::to_string(i + 1), 1);
        cv.text(cx, y1 - 1.3, 2.5, std::to_string(i + 1), 1);
    }
    for (int j = 0; j < rows; ++j) {
        const double cy = y0 + rh * (j + 0.5);
        if (j > 0) {
            cv.line(x0, y0 + rh * j, x0 + kZone, y0 + rh * j);
            cv.line(x1 - kZone, y0 + rh * j, x1, y0 + rh * j);
        }
        const std::string letter(1, static_cast<char>('A' + std::min(j, 25)));
        cv.text(x0 + kZone / 2, cy + 1, 2.5, letter, 1);
        cv.text(x1 - kZone / 2, cy + 1, 2.5, letter, 1);
    }
}

void drawTitleBlock(Canvas& cv, const SheetTemplate& t, const Project& p, const Sheet& sheet, int index, int total) {
    const double x1 = t.widthMm - kMargin - kZone, y1 = t.heightMm - kMargin - kZone;
    const double x0 = x1 - kTitleW, y0 = y1 - kTitleH;
    cv.gray(0);
    cv.width(0.35);
    cv.rect(x0, y0, kTitleW, kTitleH);
    cv.width(0.2);
    cv.line(x0, y0 + 12, x1, y0 + 12);
    cv.line(x0, y0 + 22, x1, y0 + 22);
    cv.line(x0 + 85, y0 + 12, x0 + 85, y1);
    const TitleBlock& tb = p.titleBlock;
    cv.text(x0 + 2, y0 + 5, 2, "Title");
    cv.text(x0 + 2, y0 + 10, 4, tb.title.empty() ? p.name : tb.title, 0, true);
    cv.text(x0 + 2, y0 + 15.5, 2, "Sheet");
    cv.text(x0 + 2, y0 + 20, 3, sheet.name, 0, true);
    cv.text(x0 + 87, y0 + 15.5, 2, "Size");
    cv.text(x0 + 87, y0 + 20, 3, t.name);
    cv.text(x0 + 2, y0 + 25.5, 2, "Company");
    cv.text(x0 + 2, y0 + 30, 2.8, tb.company);
    cv.text(x0 + 87, y0 + 25.5, 2, "Rev / Date / Drawn");
    std::string rev = tb.revision.empty() ? std::string() : "Rev " + tb.revision;
    for (const auto& part : {tb.date, tb.drawnBy})
        if (!part.empty()) rev += (rev.empty() ? "" : " ") + part;
    cv.text(x0 + 87, y0 + 30, 2.4, rev);
    cv.text(x1 - 2, y0 + 5, 2.4, "Sheet " + std::to_string(index) + " of " + std::to_string(total), 2);
}

void drawSheet(Canvas& cv, const Project& p, const Sheet& sheet, const SheetTemplate& t) {
    const Schematic& s = p.schematic;
    const Box extent = sheetExtent(s, sheet.id);
    // The drawing area: inside the frame, above the title block's height on its right.
    const double ax0 = kMargin + kZone + 4, ay0 = kMargin + kZone + 4;
    const double ax1 = t.widthMm - kMargin - kZone - 4, ay1 = t.heightMm - kMargin - kZone - kTitleH - 4;
    Mapping m;
    // A fixed frame prints at full scale with the frame on the page, while the drawing fits inside the frame.
    const bool fixed = sheet.frameFixed && !extent.empty() &&
                       (extent.minX - sheet.frameOrigin.x) * kSchematicUnitMm >= ax0 &&
                       (extent.maxX - sheet.frameOrigin.x) * kSchematicUnitMm <= t.widthMm - kMargin - kZone &&
                       (extent.minY - sheet.frameOrigin.y) * kSchematicUnitMm >= ay0 &&
                       (extent.maxY - sheet.frameOrigin.y) * kSchematicUnitMm <= t.heightMm - kMargin - kZone;
    if (fixed) {
        m.ox = -sheet.frameOrigin.x * kSchematicUnitMm;
        m.oy = -sheet.frameOrigin.y * kSchematicUnitMm;
    } else if (!extent.empty()) {
        const double w = (extent.maxX - extent.minX) * kSchematicUnitMm, h = (extent.maxY - extent.minY) * kSchematicUnitMm;
        const double fit = std::min({1.0, w > 0 ? (ax1 - ax0) / w : 1.0, h > 0 ? (ay1 - ay0) / h : 1.0});
        m.scale = kSchematicUnitMm * fit;
        m.ox = (ax0 + ax1) / 2 - (extent.minX + extent.maxX) / 2 * m.scale;
        m.oy = (ay0 + ay1) / 2 - (extent.minY + extent.maxY) / 2 * m.scale;
    }
    const double u = m.scale;  // mm per unit
    const double text = std::max(1.2, 9 * u);
    cv.gray(0);
    // Wires (orthogonal L-routes, as on the canvas).
    cv.width(std::max(0.15, 1.2 * u));
    std::map<int, int> wireEnds;
    for (const auto& w : s.wires()) {
        const Component* a = s.find(w.a.component);
        if (!a || a->sheet != sheet.id) continue;
        const Vec2 pa = m(s.pinPosition(w.a)), pb = m(s.pinPosition(w.b));
        cv.line(pa.x, pa.y, pb.x, pa.y);
        cv.line(pb.x, pa.y, pb.x, pb.y);
        ++wireEnds[w.a.component];
        ++wireEnds[w.b.component];
    }
    // Buses.
    cv.width(std::max(0.4, 4 * u));
    for (const auto& b : s.buses()) {
        if (b.sheet != sheet.id) continue;
        for (size_t i = 1; i < b.points.size(); ++i) {
            const Vec2 a = m(b.points[i - 1]), c = m(b.points[i]);
            cv.line(a.x, a.y, c.x, c.y);
        }
        if (!b.points.empty()) {
            const Vec2 a = m(b.points[0]);
            cv.text(a.x + 1, a.y - 1, text, b.name, 0, true);
        }
    }
    // Sheet symbols: a box around each child's entries with the child's name.
    cv.width(std::max(0.2, 1.6 * u));
    for (const auto& child : s.sheets()) {
        if (child.parent != sheet.id) continue;
        Box box;
        for (const auto& c : s.components())
            if (c.sheet == sheet.id && c.kind == ComponentKind::NetLabel && c.scope == LabelScope::SheetEntry &&
                c.targetSheet == child.id) {
                box.add(c.position + Vec2{-4, -12});
                box.add(c.position + Vec2{std::max(60.0, 7.0 * static_cast<double>(c.value.size()) + 20), 12});
            }
        if (box.empty()) continue;
        // A drawn size (Sheet::symbolWidth / symbolHeight) enlarges the fitted box.
        const double right = std::max(box.maxX + 10, box.minX + child.symbolWidth);
        const double bottom = std::max(box.maxY + 4, box.minY - 10 + child.symbolHeight);
        const Vec2 a = m({box.minX, box.minY - 10}), c = m({right, bottom});
        cv.rect(a.x, a.y, c.x - a.x, c.y - a.y);
        cv.text(a.x, a.y - 1, text, child.name, 0, true);
    }
    // Harness connectors: a body around each harness label's entries, its notched side towards the harness.
    for (const auto& h : s.components()) {
        if (h.sheet != sheet.id || !s.isHarnessLabel(h)) continue;
        Box box;
        for (const auto& e : s.components())
            if (e.harnessOf == h.id && e.sheet == sheet.id) {
                box.add(e.position + Vec2{-6, -8});
                box.add(e.position + Vec2{std::max(40.0, 7.0 * static_cast<double>(e.value.size()) + 16), 8});
            }
        if (box.empty()) continue;
        const bool left = h.position.x < (box.minX + box.maxX) / 2;
        const double notch = 8, midY = (box.minY + box.maxY) / 2;
        std::vector<Vec2> outline;
        if (left)
            outline = {{box.minX, box.minY}, {box.maxX, box.minY}, {box.maxX, box.maxY}, {box.minX, box.maxY},
                       {box.minX - notch, midY}};
        else
            outline = {{box.minX, box.minY}, {box.maxX, box.minY}, {box.maxX + notch, midY}, {box.maxX, box.maxY},
                       {box.minX, box.maxY}};
        for (auto& p : outline) p = m(p);
        cv.polygon(outline, false);
    }
    // Parts and net symbols.
    for (const auto& c : s.components()) {
        if (c.sheet != sheet.id || c.packageOnly) continue;
        const auto& pins = c.def().pins;
        const Vec2 at = m(c.position);
        cv.width(std::max(0.15, 1.4 * u));
        switch (c.kind) {
            case ComponentKind::Junction:
                if (wireEnds[c.id] >= 3) cv.dot(at.x, at.y, std::max(0.35, 3 * u));
                continue;
            case ComponentKind::Ground: {
                SymbolPen pen{cv, m, c.position, c.rotation};
                drawBuiltinSymbol(pen, c.kind, c.value);
                continue;
            }
            case ComponentKind::NetLabel: {
                const Vec2 pin = pins.empty() ? at : m(s.pinPosition({c.id, 0}));
                std::string name = s.isHarnessLabel(c) ? c.value + " =" : c.value;
                const double w = (7.0 * static_cast<double>(name.size()) + 16) * u, hh = 9 * u;
                // A flag: ports and sheet entries pointed, global labels with a notch, local labels plain.
                if (c.scope == LabelScope::Port || c.scope == LabelScope::SheetEntry || c.scope == LabelScope::Global)
                    cv.polygon({{pin.x, pin.y}, {pin.x + hh, pin.y - hh}, {pin.x + w, pin.y - hh}, {pin.x + w, pin.y + hh},
                                {pin.x + hh, pin.y + hh}}, false);
                else
                    cv.line(pin.x, pin.y, pin.x + w, pin.y);
                cv.text(pin.x + hh + 1 * u, pin.y + 0.35 * text, text, name, 0, c.scope != LabelScope::Local);
                continue;
            }
            default:
                break;
        }
        const std::string ref = s.displayRef(c);
        {
            // Built-in parts: their real symbols, designator above and value below the outline.
            SymbolPen pen{cv, m, c.position, c.rotation};
            if (drawBuiltinSymbol(pen, c.kind, c.value)) {
                const auto o = s.symbolOutline(c);
                const Vec2 lo = m({o[0], o[1]}), hi = m({o[2], o[3]});
                cv.text(lo.x, lo.y - 0.4 * text, text, ref, 0, true);
                cv.text(lo.x, hi.y + text * 1.1, text, c.value);
                continue;
            }
        }
        std::vector<Vec2> ends;
        for (size_t p = 0; p < pins.size(); ++p) ends.push_back(m(s.pinPosition({c.id, static_cast<int>(p)})));
        // Other parts: a body box inside the pins (a library part's own body and drawings), pin stubs and names.
        const CustomPart* part = c.kind == ComponentKind::Custom || c.kind == ComponentKind::PartUnit
                                     ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
        const bool unit = part && c.kind == ComponentKind::PartUnit;
        if (unit && (c.unit < 1 || c.unit > static_cast<int>(part->units.size()))) part = nullptr;
        Vec2 a, b;
        if (part) {
            const double hw = unit ? part->units[static_cast<size_t>(c.unit - 1)].halfWidth : part->symbolHalfWidth;
            const double hh = unit ? part->units[static_cast<size_t>(c.unit - 1)].halfHeight : part->symbolHalfHeight;
            const Vec2 p0 = m(c.position + rotate90({-hw, -hh}, c.rotation));
            const Vec2 p1 = m(c.position + rotate90({hw, hh}, c.rotation));
            a = {std::min(p0.x, p1.x), std::min(p0.y, p1.y)};
            b = {std::max(p0.x, p1.x), std::max(p0.y, p1.y)};
        } else {
            Box body;
            for (const auto& pd : pins) body.add(c.position + rotate90(pd.offset, c.rotation));
            if (body.empty()) body.add(c.position);
            const double inset = 10;
            a = m({body.minX + inset, body.minY - (body.maxY - body.minY < 1 ? 15 : -inset + 10)});
            b = m({body.maxX - inset, body.maxY + (body.maxY - body.minY < 1 ? 15 : -inset + 10)});
            if (b.x - a.x < 4 * u) {
                a.x -= 15 * u;
                b.x += 15 * u;
            }
        }
        if (!part || unit || part->spec.symbol.body)
            cv.rect(std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y));
        if (part && !unit) drawSymbolGraphics(cv, m, c, part->spec.symbol.graphics);
        cv.width(std::max(0.15, 1.4 * u));
        for (size_t p = 0; p < ends.size(); ++p) {
            const Vec2 e = ends[p];
            const Vec2 inner{std::clamp(e.x, std::min(a.x, b.x), std::max(a.x, b.x)), std::clamp(e.y, std::min(a.y, b.y), std::max(a.y, b.y))};
            cv.line(e.x, e.y, inner.x, inner.y);
            if (pins.size() > 2 && text >= 1.2) {
                const int align = inner.x <= std::min(a.x, b.x) + 1e-6 ? 0 : inner.x >= std::max(a.x, b.x) - 1e-6 ? 2 : 1;
                const double ix = align == 0 ? inner.x + 1 * u : align == 2 ? inner.x - 1 * u : inner.x;
                cv.text(ix, inner.y + 0.35 * text * 0.8, text * 0.8, pins[p].name, align);
            }
        }
        cv.text(std::min(a.x, b.x), std::min(a.y, b.y) - 1, text, ref, 0, true);
        cv.text(std::min(a.x, b.x), std::max(a.y, b.y) + text * 1.1, text, c.value);
    }
}
}  // namespace

const SheetTemplate& sheetTemplateFor(const Project& project, int sheet) {
    const Sheet* s = project.schematic.findSheet(sheet);
    if (s && !s->size.empty())
        if (const SheetTemplate* t = findSheetTemplate(s->size)) return *t;
    const Box e = sheetExtent(project.schematic, sheet);
    const auto& list = sheetTemplates();
    if (e.empty()) return list.front();
    const double w = (e.maxX - e.minX) * kSchematicUnitMm, h = (e.maxY - e.minY) * kSchematicUnitMm;
    for (size_t i = 0; i < 5; ++i) {  // A4 … A0
        const SheetTemplate& t = list[i];
        const double aw = t.widthMm - 2 * (kMargin + kZone + 4), ah = t.heightMm - 2 * (kMargin + kZone + 4) - kTitleH;
        if (w <= aw && h <= ah) return t;
    }
    return list[4];
}

std::string exportSchematicPdf(const Project& project) { return exportSchematicPdf(project, SchematicPdfOptions{}); }

std::string exportSchematicPdf(const Project& project, const SchematicPdfOptions& options) {
    const Schematic& s = project.schematic;
    const auto& sheets = s.sheets();
    const int total = static_cast<int>(sheets.size());
    TrueTypeFont font;
    PdfFonts fonts;
    if (!options.fontData.empty() && font.load(options.fontData)) fonts.font = &font;
    // Font objects after the pages: Symbol, then (with an embedded font) Type0, CIDFont, descriptor, file, ToUnicode.
    const int symbolObj = 5 + 3 * total + 1;
    const int type0Obj = symbolObj + 1;
    std::string fontResources = "/F1 3 0 R /F2 4 0 R /F3 " + std::to_string(symbolObj) + " 0 R";
    if (fonts.font) fontResources += " /F4 " + std::to_string(type0Obj) + " 0 R";
    // Objects: 1 catalog, 2 pages, 3 / 4 fonts, 5 outline root, then per sheet: page, content, outline item.
    std::vector<std::string> objects(5);
    std::vector<int> pageObj, itemObj;
    for (int i = 0; i < total; ++i) {
        pageObj.push_back(6 + 3 * i);
        itemObj.push_back(8 + 3 * i);
    }
    std::vector<std::string> sheetObjects;
    for (int i = 0; i < total; ++i) {
        const Sheet& sh = sheets[static_cast<size_t>(i)];
        const SheetTemplate& t = sheetTemplateFor(project, sh.id);
        Canvas cv(t.heightMm);
        cv.useFonts(&fonts);
        drawFrame(cv, t);
        drawSheet(cv, project, sh, t);
        drawTitleBlock(cv, t, project, sh, i + 1, total);
        const std::string content = cv.str();
        char mediabox[96];
        std::snprintf(mediabox, sizeof mediabox, "[0 0 %.2f %.2f]", t.widthMm * kPt, t.heightMm * kPt);
        sheetObjects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox " + std::string(mediabox) +
                               " /Resources << /Font << " + fontResources + " >> >> /Contents " +
                               std::to_string(pageObj[static_cast<size_t>(i)] + 1) + " 0 R >>");
        sheetObjects.push_back("<< /Length " + std::to_string(content.size()) + " >>\nstream\n" + content + "endstream");
        sheetObjects.push_back("");  // outline item, filled below
    }
    // Outline tree: each sheet under its parent's item (top-level sheets under the root), in sheet order.
    std::map<int, int> indexOf;
    for (int i = 0; i < total; ++i) indexOf[sheets[static_cast<size_t>(i)].id] = i;
    std::map<int, std::vector<int>> children;  // parent item (-1 = root) → child sheet indices
    for (int i = 0; i < total; ++i) {
        const int parent = sheets[static_cast<size_t>(i)].parent;
        children[parent != 0 && indexOf.count(parent) ? indexOf[parent] : -1].push_back(i);
    }
    // Descendant counts (open outline: every item shown).
    std::map<int, int> descendants;
    std::function<int(int)> count = [&](int item) {
        int n = 0;
        for (int c : children[item]) n += 1 + count(c);
        return descendants[item] = n;
    };
    count(-1);
    auto ref = [](int obj) { return std::to_string(obj) + " 0 R"; };
    for (int i = 0; i < total; ++i) {
        const Sheet& sh = sheets[static_cast<size_t>(i)];
        const int parent = sh.parent != 0 && indexOf.count(sh.parent) ? indexOf[sh.parent] : -1;
        const auto& siblings = children[parent];
        const auto at = std::find(siblings.begin(), siblings.end(), i) - siblings.begin();
        std::string item = "<< /Title " + pdfString(sh.name) + " /Parent " + (parent < 0 ? std::string("5 0 R") : ref(itemObj[static_cast<size_t>(parent)])) +
                           " /Dest [" + ref(pageObj[static_cast<size_t>(i)]) + " /Fit]";
        if (at > 0) item += " /Prev " + ref(itemObj[static_cast<size_t>(siblings[static_cast<size_t>(at - 1)])]);
        if (static_cast<size_t>(at) + 1 < siblings.size()) item += " /Next " + ref(itemObj[static_cast<size_t>(siblings[static_cast<size_t>(at + 1)])]);
        const auto& kids = children[i];
        if (!kids.empty())
            item += " /First " + ref(itemObj[static_cast<size_t>(kids.front())]) + " /Last " + ref(itemObj[static_cast<size_t>(kids.back())]) +
                    " /Count " + std::to_string(descendants[i]);
        item += " >>";
        sheetObjects[static_cast<size_t>(3 * i + 2)] = item;
    }
    std::string kids;
    for (int i = 0; i < total; ++i) kids += (i ? " " : "") + ref(pageObj[static_cast<size_t>(i)]);
    objects[0] = "<< /Type /Catalog /Pages 2 0 R /Outlines 5 0 R /PageMode /UseOutlines >>";
    objects[1] = "<< /Type /Pages /Kids [" + kids + "] /Count " + std::to_string(total) + " >>";
    objects[2] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding /WinAnsiEncoding >>";
    objects[3] = "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica-Bold /Encoding /WinAnsiEncoding >>";
    const auto& top = children[-1];
    objects[4] = top.empty() ? std::string("<< /Type /Outlines /Count 0 >>")
                             : "<< /Type /Outlines /First " + ref(itemObj[static_cast<size_t>(top.front())]) + " /Last " +
                                   ref(itemObj[static_cast<size_t>(top.back())]) + " /Count " + std::to_string(descendants[-1]) + " >>";
    objects.insert(objects.end(), sheetObjects.begin(), sheetObjects.end());
    objects.push_back("<< /Type /Font /Subtype /Type1 /BaseFont /Symbol >>");
    if (fonts.font) {
        // The embedded font: a subset of the glyphs used, addressed by glyph number (Identity-H, CIDToGIDMap /Identity).
        const double em = font.unitsPerEm();
        auto units = [&](double v) { return std::to_string(static_cast<long>(std::lround(v * 1000 / em))); };
        std::string widths = "[";
        for (int g : fonts.glyphs) widths += " " + std::to_string(g) + " [" + std::to_string(std::lround(font.advance(g) * 1000)) + "]";
        widths += " ]";
        const std::string name = "/SIEDAA+SiEDAEmbedded";
        objects.push_back("<< /Type /Font /Subtype /Type0 /BaseFont " + name + " /Encoding /Identity-H /DescendantFonts [" +
                          ref(type0Obj + 1) + "] /ToUnicode " + ref(type0Obj + 4) + " >>");
        objects.push_back("<< /Type /Font /Subtype /CIDFontType2 /BaseFont " + name +
                          " /CIDSystemInfo << /Registry (Adobe) /Ordering (Identity) /Supplement 0 >> /FontDescriptor " +
                          ref(type0Obj + 2) + " /CIDToGIDMap /Identity /DW 1000 /W " + widths + " >>");
        const int* bb = font.bbox();
        objects.push_back("<< /Type /FontDescriptor /FontName " + name + " /Flags 32 /FontBBox [" + units(bb[0]) + " " +
                          units(bb[1]) + " " + units(bb[2]) + " " + units(bb[3]) + "] /ItalicAngle 0 /Ascent " +
                          units(font.ascent()) + " /Descent " + units(font.descent()) + " /CapHeight " + units(font.ascent()) +
                          " /StemV 80 /FontFile2 " + ref(type0Obj + 3) + " >>");
        // The font file as hex (the PDF stays plain ASCII text).
        const std::string sub = font.subset(fonts.glyphs);
        std::string hex;
        hex.reserve(sub.size() * 2 + sub.size() / 32 + 2);
        static const char digits[] = "0123456789ABCDEF";
        for (size_t i = 0; i < sub.size(); ++i) {
            const unsigned char c = static_cast<unsigned char>(sub[i]);
            hex += digits[c >> 4];
            hex += digits[c & 15];
            if (i % 32 == 31) hex += '\n';
        }
        hex += ">";
        objects.push_back("<< /Length " + std::to_string(hex.size()) + " /Length1 " + std::to_string(sub.size()) +
                          " /Filter /ASCIIHexDecode >>\nstream\n" + hex + "\nendstream");
        std::string cmap = "/CIDInit /ProcSet findresource begin 12 dict begin begincmap /CIDSystemInfo << /Registry (Adobe) "
                           "/Ordering (UCS) /Supplement 0 >> def /CMapName /Adobe-Identity-UCS def /CMapType 2 def 1 "
                           "begincodespacerange <0000> <FFFF> endcodespacerange\n";
        std::vector<std::pair<int, uint32_t>> pairs(fonts.unicodeOf.begin(), fonts.unicodeOf.end());
        for (size_t i = 0; i < pairs.size(); i += 100) {
            const size_t n = std::min<size_t>(100, pairs.size() - i);
            cmap += std::to_string(n) + " beginbfchar\n";
            for (size_t k = i; k < i + n; ++k) {
                char line[64];
                const uint32_t cp = pairs[k].second;
                if (cp >= 0x10000) {
                    const uint32_t v = cp - 0x10000;
                    std::snprintf(line, sizeof line, "<%04X> <%04X%04X>\n", static_cast<unsigned>(pairs[k].first),
                                  static_cast<unsigned>(0xD800 + (v >> 10)), static_cast<unsigned>(0xDC00 + (v & 0x3FF)));
                } else {
                    std::snprintf(line, sizeof line, "<%04X> <%04X>\n", static_cast<unsigned>(pairs[k].first), static_cast<unsigned>(cp));
                }
                cmap += line;
            }
            cmap += "endbfchar\n";
        }
        cmap += "endcmap CMapName currentdict /CMap defineresource pop end end";
        objects.push_back("<< /Length " + std::to_string(cmap.size()) + " >>\nstream\n" + cmap + "\nendstream");
    }

    std::string pdf = "%PDF-1.4\n%SiEDA schematic\n";
    std::vector<size_t> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.size());
        pdf += std::to_string(i + 1) + " 0 obj\n" + objects[i] + "\nendobj\n";
    }
    const size_t xref = pdf.size();
    pdf += "xref\n0 " + std::to_string(objects.size() + 1) + "\n0000000000 65535 f \n";
    for (size_t off : offsets) {
        char line[24];
        std::snprintf(line, sizeof line, "%010zu 00000 n \n", off);
        pdf += line;
    }
    std::string title = project.titleBlock.title.empty() ? project.name : project.titleBlock.title;
    pdf += "trailer\n<< /Size " + std::to_string(objects.size() + 1) + " /Root 1 0 R >>\nstartxref\n" + std::to_string(xref) + "\n%%EOF\n";
    (void)title;
    return pdf;
}

}  // namespace sieda
