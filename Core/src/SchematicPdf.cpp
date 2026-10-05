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

/// Plain ASCII for the base-14 fonts: other characters become '?'.
std::string ascii(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x80) {
            if (ch >= 0x20) out += static_cast<char>(ch);
        } else if ((ch & 0xC0) != 0x80) {
            out += '?';  // first byte of a multi-byte character
        }
    }
    return out;
}

std::string pdfString(const std::string& s) {
    std::string out = "(";
    for (char ch : ascii(s)) {
        if (ch == '(' || ch == ')' || ch == '\\') out += '\\';
        out += ch;
    }
    return out + ")";
}

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
    /// Text with its baseline at y; align 0 left, 1 centre, 2 right (approximate Helvetica width).
    void text(double x, double y, double sizeMm, const std::string& s, int align = 0, bool bold = false) {
        const std::string a = ascii(s);
        if (a.empty()) return;
        const double width = 0.52 * sizeMm * static_cast<double>(a.size());
        const double left = align == 1 ? x - width / 2 : align == 2 ? x - width : x;
        out_ << "BT /" << (bold ? "F2 " : "F1 ") << num(sizeMm * kPt) << " Tf " << num(left * kPt) << ' '
             << num((h_ - y) * kPt) << " Td " << pdfString(a) << " Tj ET\n";
    }
    std::string str() const { return out_.str(); }

private:
    static std::string num(double v) {
        char b[32];
        std::snprintf(b, sizeof b, "%.2f", std::fabs(v) < 0.005 ? 0.0 : v);
        return b;
    }
    double h_;
    std::ostringstream out_;
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
    if (!extent.empty()) {
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
                const Vec2 pin = pins.empty() ? at : m(s.pinPosition({c.id, 0}));
                const double d = 8 * u;
                cv.line(pin.x, pin.y, pin.x, pin.y + d);
                cv.line(pin.x - 2 * d, pin.y + d, pin.x + 2 * d, pin.y + d);
                cv.line(pin.x - 1.3 * d, pin.y + 1.6 * d, pin.x + 1.3 * d, pin.y + 1.6 * d);
                cv.line(pin.x - 0.6 * d, pin.y + 2.2 * d, pin.x + 0.6 * d, pin.y + 2.2 * d);
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
        std::vector<Vec2> ends;
        for (size_t p = 0; p < pins.size(); ++p) ends.push_back(m(s.pinPosition({c.id, static_cast<int>(p)})));
        const std::string ref = s.displayRef(c);
        if (ends.size() == 2) {
            // Two-pin parts: leads to a body in the middle third.
            const Vec2 a = ends[0], b = ends[1];
            const Vec2 d{(b.x - a.x) / 3, (b.y - a.y) / 3};
            const Vec2 q1{a.x + d.x, a.y + d.y}, q2{a.x + 2 * d.x, a.y + 2 * d.y};
            cv.line(a.x, a.y, q1.x, q1.y);
            cv.line(q2.x, q2.y, b.x, b.y);
            const double len = std::hypot(q2.x - q1.x, q2.y - q1.y);
            const Vec2 n = len > 0 ? Vec2{-(q2.y - q1.y) / len, (q2.x - q1.x) / len} : Vec2{0, 1};
            const double half = std::max(0.6, 8 * u);
            if (c.kind == ComponentKind::Capacitor) {
                const Vec2 mid{(q1.x + q2.x) / 2, (q1.y + q2.y) / 2};
                const Vec2 t{(q2.x - q1.x) / std::max(len, 1e-9), (q2.y - q1.y) / std::max(len, 1e-9)};
                const double gap = std::max(0.3, 2.5 * u);
                for (double side : {-gap, gap}) {
                    const Vec2 c0{mid.x + t.x * side, mid.y + t.y * side};
                    cv.line(c0.x - n.x * half, c0.y - n.y * half, c0.x + n.x * half, c0.y + n.y * half);
                }
                cv.line(q1.x, q1.y, mid.x - t.x * gap, mid.y - t.y * gap);
                cv.line(mid.x + t.x * gap, mid.y + t.y * gap, q2.x, q2.y);
            } else if (c.kind == ComponentKind::Diode || c.kind == ComponentKind::LED) {
                cv.polygon({{q1.x + n.x * half, q1.y + n.y * half}, {q1.x - n.x * half, q1.y - n.y * half}, {q2.x, q2.y}}, false);
                cv.line(q2.x - n.x * half, q2.y - n.y * half, q2.x + n.x * half, q2.y + n.y * half);
            } else {
                cv.polygon({{q1.x + n.x * half * 0.5, q1.y + n.y * half * 0.5}, {q2.x + n.x * half * 0.5, q2.y + n.y * half * 0.5},
                            {q2.x - n.x * half * 0.5, q2.y - n.y * half * 0.5}, {q1.x - n.x * half * 0.5, q1.y - n.y * half * 0.5}},
                           false);
            }
            const double tx = std::max(a.x, b.x) + 2 * u, ty = std::min(a.y, b.y) + (std::fabs(a.y - b.y) > 1 ? std::fabs(a.y - b.y) / 2 : -6 * u);
            cv.text(tx, ty, text, ref, 0, true);
            cv.text(tx, ty + text * 1.2, text, c.value);
            continue;
        }
        // Other parts: a body box inside the pins (a library part's own body and drawings), pin stubs and names.
        const CustomPart* part = c.kind == ComponentKind::Custom ? CustomPartRegistry::instance().find(c.customPart) : nullptr;
        Vec2 a, b;
        if (part) {
            const Vec2 p0 = m(c.position + rotate90({-part->symbolHalfWidth, -part->symbolHalfHeight}, c.rotation));
            const Vec2 p1 = m(c.position + rotate90({part->symbolHalfWidth, part->symbolHalfHeight}, c.rotation));
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
        if (!part || part->spec.symbol.body)
            cv.rect(std::min(a.x, b.x), std::min(a.y, b.y), std::fabs(b.x - a.x), std::fabs(b.y - a.y));
        if (part) drawSymbolGraphics(cv, m, c, part->spec.symbol.graphics);
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

std::string exportSchematicPdf(const Project& project) {
    const Schematic& s = project.schematic;
    const auto& sheets = s.sheets();
    const int total = static_cast<int>(sheets.size());
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
        drawFrame(cv, t);
        drawSheet(cv, project, sh, t);
        drawTitleBlock(cv, t, project, sh, i + 1, total);
        const std::string content = cv.str();
        char mediabox[96];
        std::snprintf(mediabox, sizeof mediabox, "[0 0 %.2f %.2f]", t.widthMm * kPt, t.heightMm * kPt);
        sheetObjects.push_back("<< /Type /Page /Parent 2 0 R /MediaBox " + std::string(mediabox) +
                               " /Resources << /Font << /F1 3 0 R /F2 4 0 R >> >> /Contents " +
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
