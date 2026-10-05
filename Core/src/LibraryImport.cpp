#include "sieda/LibraryImport.hpp"
#include "sieda/AltiumLibrary.hpp"
#include "sieda/Model3D.hpp"

#include <algorithm>
#include <tuple>
#include <memory>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <functional>
#include <set>

namespace sieda {

namespace {

// Limits that keep hostile or broken files from exhausting memory or the stack.
constexpr size_t kMaxFileBytes = 32u << 20;  // 32 MB per file
constexpr int kMaxDepth = 200;               // nesting of s-expressions / XML elements
constexpr size_t kMaxNodes = 4000000;        // parsed nodes per file
constexpr size_t kMaxParts = 2000;           // parts per import
constexpr size_t kMaxLands = 512;            // = the land-pattern limit in CustomParts.cpp
constexpr double kGrid = 2.54;  // schematic pin pitch of KiCad and Eagle symbols (0.1 in)
constexpr double kPi = 3.14159265358979323846;

// ------------------------------------------------------------------ text helpers

std::string upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string lower(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

/// Valid UTF-8 without control characters, trimmed and at most `maxLen` bytes (cut at a character boundary).
std::string clean(const std::string& in, size_t maxLen = 200) {
    std::string out;
    out.reserve(std::min(in.size(), maxLen));
    size_t i = 0;
    while (i < in.size()) {
        const unsigned char c = static_cast<unsigned char>(in[i]);
        size_t len = c < 0x80 ? 1 : (c >> 5) == 0x6 ? 2 : (c >> 4) == 0xE ? 3 : (c >> 3) == 0x1E ? 4 : 0;
        bool ok = len > 0 && i + len <= in.size();
        for (size_t k = 1; ok && k < len; ++k) ok = (static_cast<unsigned char>(in[i + k]) & 0xC0) == 0x80;
        if (!ok) {
            out += '?';
            ++i;
            continue;
        }
        if (len == 1) {
            if (c == '\t' || c == '\n' || c == '\r') out += ' ';
            else if (c >= 0x20 && c != 0x7F) out += static_cast<char>(c);
        } else {
            if (out.size() + len > maxLen) break;
            out.append(in, i, len);
        }
        i += len;
        if (out.size() >= maxLen) break;
    }
    const size_t a = out.find_first_not_of(' '), b = out.find_last_not_of(' ');
    return a == std::string::npos ? std::string() : out.substr(a, b - a + 1);
}

/// Locale-independent decimal number ("-1.27", "2.5e-1", "+3"). False for anything else or a non-finite value.
bool parseNumber(const std::string& s, double& out) {
    size_t i = 0;
    const size_t n = s.size();
    if (n == 0 || n > 40) return false;
    bool neg = false;
    if (s[i] == '+' || s[i] == '-') neg = s[i++] == '-';
    double v = 0;
    int digits = 0;
    while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
        v = v * 10 + (s[i++] - '0');
        ++digits;
    }
    if (i < n && s[i] == '.') {
        ++i;
        double scale = 0.1;
        while (i < n && std::isdigit(static_cast<unsigned char>(s[i]))) {
            v += (s[i++] - '0') * scale;
            scale *= 0.1;
            ++digits;
        }
    }
    if (digits == 0) return false;
    if (i < n && (s[i] == 'e' || s[i] == 'E')) {
        ++i;
        bool eneg = false;
        if (i < n && (s[i] == '+' || s[i] == '-')) eneg = s[i++] == '-';
        int e = 0, edigits = 0;
        while (i < n && std::isdigit(static_cast<unsigned char>(s[i])) && e < 400) {
            e = e * 10 + (s[i++] - '0');
            ++edigits;
        }
        if (edigits == 0) return false;
        v *= std::pow(10.0, eneg ? -e : e);
    }
    if (i != n || !std::isfinite(v)) return false;
    out = neg ? -v : v;
    return true;
}

/// Rounds to 0.1 µm so decimal file values stay decimal in the land JSON.
double tidy(double v) {
    const double r = std::round(v * 10000.0) / 10000.0;
    return std::fabs(r) < 1e-9 ? 0.0 : r;
}

std::string fmt(double v) {
    char b[32];
    std::snprintf(b, sizeof b, "%.3g", v);
    return b;
}

/// Pin / pad numbers in natural order: 1, 2, 10, A1, A2, A10, B1, MP.
bool naturalLess(const std::string& a, const std::string& b) {
    auto split = [](const std::string& s, std::string& prefix, long& num, std::string& rest) {
        size_t i = 0;
        while (i < s.size() && !std::isdigit(static_cast<unsigned char>(s[i]))) ++i;
        prefix = upper(s.substr(0, i));
        size_t j = i;
        num = 0;
        while (j < s.size() && std::isdigit(static_cast<unsigned char>(s[j])) && j - i < 9) num = num * 10 + (s[j++] - '0');
        if (j == i) num = -1;
        rest = s.substr(j);
    };
    std::string pa, pb, ra, rb;
    long na = 0, nb = 0;
    split(a, pa, na, ra);
    split(b, pb, nb, rb);
    if (pa.empty() != pb.empty()) return pa.empty();  // plain numbers first
    if (pa != pb) return pa < pb;
    if (na != nb) return na < nb;
    return ra < rb;
}

struct Box {
    double x0 = 1e300, y0 = 1e300, x1 = -1e300, y1 = -1e300;
    bool empty() const { return x0 > x1; }
    void add(double x, double y) {
        if (!std::isfinite(x) || !std::isfinite(y) || std::fabs(x) > 1e4 || std::fabs(y) > 1e4) return;
        x0 = std::min(x0, x);
        y0 = std::min(y0, y);
        x1 = std::max(x1, x);
        y1 = std::max(y1, y);
    }
    void add(const Box& b) {
        if (b.empty()) return;
        add(b.x0, b.y0);
        add(b.x1, b.y1);
    }
    double w() const { return empty() ? 0 : x1 - x0; }
    double h() const { return empty() ? 0 : y1 - y0; }
};

// ------------------------------------------------------------------ s-expressions (KiCad)

struct SNode {
    std::string text;  // atom text (unquoted)
    bool list = false;
    int line = 0;
    std::vector<SNode> kids;

    std::string head() const { return list && !kids.empty() && !kids[0].list ? kids[0].text : std::string(); }
    /// Atom i (0 = the head), or "" if missing or a list.
    std::string atom(size_t i) const { return list && i < kids.size() && !kids[i].list ? kids[i].text : std::string(); }
    const SNode* child(const char* name) const {
        for (const auto& k : kids)
            if (k.list && k.head() == name) return &k;
        return nullptr;
    }
    bool hasAtom(const char* name) const {
        for (size_t i = 1; i < kids.size(); ++i)
            if (!kids[i].list && kids[i].text == name) return true;
        return false;
    }
};

class SexprParser {
public:
    explicit SexprParser(const std::string& text) : s_(text) {}

    SNode parse() {
        skip();
        if (i_ >= s_.size() || s_[i_] != '(') fail("expected '(' at the start of the file");
        SNode root = list(0);
        skip();
        if (i_ < s_.size()) fail("unexpected text after the closing ')'");
        return root;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1;
    size_t nodes_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        throw ImportError("line " + std::to_string(line_) + ": " + what);
    }

    void skip() {
        while (i_ < s_.size()) {
            const char c = s_[i_];
            if (c == '\n') {
                ++line_;
                ++i_;
            } else if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
                ++i_;
            } else {
                break;
            }
        }
    }

    SNode list(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        SNode n;
        n.list = true;
        n.line = line_;
        ++i_;  // '('
        for (;;) {
            skip();
            if (i_ >= s_.size()) fail("missing ')' (the file ends inside a list opened on line " + std::to_string(n.line) + ")");
            if (++nodes_ > kMaxNodes) fail("too many elements");
            const char c = s_[i_];
            if (c == ')') {
                ++i_;
                return n;
            }
            if (c == '(') {
                n.kids.push_back(list(depth + 1));
            } else if (c == '"') {
                n.kids.push_back(quoted());
            } else {
                SNode a;
                a.line = line_;
                const size_t start = i_;
                while (i_ < s_.size() && !std::isspace(static_cast<unsigned char>(s_[i_])) && s_[i_] != '(' &&
                       s_[i_] != ')' && s_[i_] != '"')
                    ++i_;
                a.text = s_.substr(start, i_ - start);
                n.kids.push_back(std::move(a));
            }
        }
    }

    SNode quoted() {
        SNode a;
        a.line = line_;
        ++i_;  // opening quote
        for (;;) {
            if (i_ >= s_.size()) fail("unterminated string (opened on line " + std::to_string(a.line) + ")");
            const char c = s_[i_++];
            if (c == '"') return a;
            if (c == '\n') ++line_;
            if (c == '\\' && i_ < s_.size()) {
                const char e = s_[i_++];
                a.text += e == 'n' ? '\n' : e == 't' ? '\t' : e;
            } else {
                a.text += c;
            }
            if (a.text.size() > 65536) fail("string too long");
        }
    }
};

double num(const SNode* n, size_t i, double def = 0) {
    double v = def;
    if (n && parseNumber(n->atom(i), v)) return v;
    return def;
}

// ------------------------------------------------------------------ XML (Eagle)

struct XNode {
    std::string tag;
    std::vector<std::pair<std::string, std::string>> attrs;
    std::vector<XNode> kids;
    std::string text;
    int line = 0;

    std::string attr(const char* name, const std::string& def = {}) const {
        for (const auto& a : attrs)
            if (a.first == name) return a.second;
        return def;
    }
    double number(const char* name, double def = 0) const {
        double v = def;
        return parseNumber(attr(name), v) ? v : def;
    }
    const XNode* child(const char* name) const {
        for (const auto& k : kids)
            if (k.tag == name) return &k;
        return nullptr;
    }
};

class XmlParser {
public:
    explicit XmlParser(const std::string& text) : s_(text) {}

    XNode parse() {
        if (s_.compare(0, 3, "\xEF\xBB\xBF") == 0) i_ = 3;
        prolog();
        if (i_ >= s_.size() || s_[i_] != '<') fail("expected the root element");
        XNode root = element(0);
        prolog();
        if (i_ < s_.size()) fail("unexpected content after the root element");
        return root;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int line_ = 1;
    size_t nodes_ = 0;

    [[noreturn]] void fail(const std::string& what) const {
        throw ImportError("line " + std::to_string(line_) + ": " + what);
    }
    bool at(const char* lit) const { return s_.compare(i_, std::strlen(lit), lit) == 0; }
    void advance(size_t n) {
        for (size_t k = 0; k < n && i_ < s_.size(); ++k)
            if (s_[i_++] == '\n') ++line_;
    }
    void skipTo(const char* end) {
        const size_t p = s_.find(end, i_);
        if (p == std::string::npos) fail(std::string("missing '") + end + "'");
        advance(p - i_ + std::strlen(end));
    }
    void space() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) advance(1);
    }
    /// Whitespace, the XML declaration, processing instructions, comments and the DOCTYPE.
    void prolog() {
        for (;;) {
            space();
            if (at("<?")) skipTo("?>");
            else if (at("<!--")) skipTo("-->");
            else if (at("<!DOCTYPE") || at("<!doctype")) doctype();
            else return;
        }
    }
    void doctype() {
        int bracket = 0;
        while (i_ < s_.size()) {
            const char c = s_[i_];
            advance(1);
            if (c == '[') ++bracket;
            else if (c == ']') --bracket;
            else if (c == '>' && bracket <= 0) return;
        }
        fail("unterminated DOCTYPE");
    }
    std::string name() {
        const size_t start = i_;
        while (i_ < s_.size()) {
            const unsigned char c = static_cast<unsigned char>(s_[i_]);
            if (std::isalnum(c) || c == '_' || c == '-' || c == '.' || c == ':' || c >= 0x80) ++i_;
            else break;
        }
        if (i_ == start) fail("expected a name");
        return s_.substr(start, i_ - start);
    }
    std::string decode(const std::string& raw) {
        std::string out;
        for (size_t k = 0; k < raw.size(); ++k) {
            if (raw[k] != '&') {
                out += raw[k];
                continue;
            }
            const size_t semi = raw.find(';', k);
            if (semi == std::string::npos || semi - k > 10) {
                out += '&';
                continue;
            }
            const std::string ent = raw.substr(k + 1, semi - k - 1);
            unsigned long cp = 0;
            bool known = true;
            if (ent == "lt") out += '<';
            else if (ent == "gt") out += '>';
            else if (ent == "amp") out += '&';
            else if (ent == "quot") out += '"';
            else if (ent == "apos") out += '\'';
            else if (ent.size() > 1 && ent[0] == '#') {
                const bool hex = ent[1] == 'x' || ent[1] == 'X';
                const std::string digits = ent.substr(hex ? 2 : 1);
                known = !digits.empty() && digits.size() <= 6;
                for (char c : digits) {
                    const int d = std::isdigit(static_cast<unsigned char>(c)) ? c - '0'
                                  : hex && std::isxdigit(static_cast<unsigned char>(c))
                                      ? std::tolower(static_cast<unsigned char>(c)) - 'a' + 10
                                      : -1;
                    if (d < 0) known = false;
                    else cp = cp * (hex ? 16 : 10) + static_cast<unsigned long>(d);
                }
                if (known && cp > 0 && cp < 0x110000) {
                    if (cp < 0x80) out += static_cast<char>(cp);
                    else if (cp < 0x800) {
                        out += static_cast<char>(0xC0 | (cp >> 6));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else if (cp < 0x10000) {
                        out += static_cast<char>(0xE0 | (cp >> 12));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    } else {
                        out += static_cast<char>(0xF0 | (cp >> 18));
                        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
                        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
                        out += static_cast<char>(0x80 | (cp & 0x3F));
                    }
                } else {
                    known = false;
                }
            } else {
                known = false;
            }
            if (!known) {
                out += '&';
                continue;
            }
            k = semi;
        }
        return out;
    }
    XNode element(int depth) {
        if (depth > kMaxDepth) fail("nesting too deep");
        if (++nodes_ > kMaxNodes) fail("too many elements");
        XNode n;
        n.line = line_;
        advance(1);  // '<'
        n.tag = name();
        for (;;) {
            space();
            if (i_ >= s_.size()) fail("unterminated <" + n.tag + ">");
            if (at("/>")) {
                advance(2);
                return n;
            }
            if (s_[i_] == '>') {
                advance(1);
                break;
            }
            std::string key = name();
            space();
            if (i_ >= s_.size() || s_[i_] != '=') fail("attribute '" + key + "' of <" + n.tag + "> has no value");
            advance(1);
            space();
            if (i_ >= s_.size() || (s_[i_] != '"' && s_[i_] != '\'')) fail("attribute '" + key + "' is not quoted");
            const char q = s_[i_];
            advance(1);
            const size_t end = s_.find(q, i_);
            if (end == std::string::npos) fail("unterminated attribute '" + key + "'");
            std::string raw = s_.substr(i_, end - i_);
            advance(end - i_ + 1);
            if (n.attrs.size() < 64) n.attrs.emplace_back(std::move(key), decode(raw));
        }
        // Content.
        for (;;) {
            if (i_ >= s_.size()) fail("<" + n.tag + "> (line " + std::to_string(n.line) + ") is never closed");
            if (at("</")) {
                advance(2);
                const std::string closing = name();
                space();
                if (i_ >= s_.size() || s_[i_] != '>') fail("malformed closing tag </" + closing + ">");
                advance(1);
                if (closing != n.tag)
                    fail("</" + closing + "> closes <" + n.tag + "> opened on line " + std::to_string(n.line));
                return n;
            }
            if (at("<!--")) {
                skipTo("-->");
            } else if (at("<![CDATA[")) {
                advance(9);
                const size_t end = s_.find("]]>", i_);
                if (end == std::string::npos) fail("unterminated CDATA");
                if (n.text.size() < 65536) n.text += s_.substr(i_, std::min(end - i_, size_t(65536)));
                advance(end - i_ + 3);
            } else if (at("<?")) {
                skipTo("?>");
            } else if (s_[i_] == '<') {
                n.kids.push_back(element(depth + 1));
            } else {
                const size_t end = s_.find('<', i_);
                const size_t stop = end == std::string::npos ? s_.size() : end;
                if (n.text.size() < 65536) n.text += decode(s_.substr(i_, std::min(stop - i_, size_t(65536))));
                advance(stop - i_);
            }
        }
    }
};

/// Every element named `tag` under `n`, depth first.
void collect(const XNode& n, const char* tag, std::vector<const XNode*>& out, int depth = 0) {
    if (depth > kMaxDepth) return;
    for (const auto& k : n.kids) {
        if (k.tag == tag) out.push_back(&k);
        else collect(k, tag, out, depth + 1);
    }
}

/// Eagle descriptions are HTML: tags removed, whitespace collapsed.
std::string stripHtml(const std::string& html) {
    std::string out;
    bool inTag = false;
    for (char c : html) {
        if (c == '<') inTag = true;
        else if (c == '>') {
            inTag = false;
            out += ' ';
        } else if (!inTag) {
            out += c;
        }
    }
    std::string collapsed;
    for (char c : out) {
        const bool sp = std::isspace(static_cast<unsigned char>(c)) != 0;
        if (sp && (collapsed.empty() || collapsed.back() == ' ')) continue;
        collapsed += sp ? ' ' : c;
    }
    return clean(collapsed, 300);
}

// ------------------------------------------------------------------ land patterns

/// One copper pad before it becomes a land: centre, size, drill, shape and number (as in the file).
struct RawPad {
    std::string number;
    double x = 0, y = 0, w = 0, h = 0, drill = 0;
    bool round = false;
};

/// Builds the land pattern: recentred on `centreBox` (courtyard, else pads + body), body from `bodyBox`, with the
/// limits of the land JSON enforced (throws ImportError).
void finishLands(ImportedFootprint& fp, const std::vector<RawPad>& pads, const Box& courtyard, const Box& body) {
    if (pads.empty()) throw ImportError("the footprint has no copper pads");
    if (pads.size() > kMaxLands)
        throw ImportError("the footprint has " + std::to_string(pads.size()) + " pads; land patterns hold up to " +
                          std::to_string(kMaxLands));
    Box padBox;
    for (const auto& p : pads) {
        padBox.add(p.x - p.w / 2, p.y - p.h / 2);
        padBox.add(p.x + p.w / 2, p.y + p.h / 2);
    }
    Box centreBox = courtyard;
    if (centreBox.empty()) {
        centreBox = padBox;
        centreBox.add(body);
    }
    const double cx = tidy((centreBox.x0 + centreBox.x1) / 2), cy = tidy((centreBox.y0 + centreBox.y1) / 2);
    fp.centreX = cx;
    fp.centreY = cy;
    if (std::fabs(cx) > 0.01 || std::fabs(cy) > 0.01)
        fp.warnings.push_back("Origin moved to the centre of the footprint (by " + fmt(tidy(-cx)) + ", " + fmt(tidy(-cy)) + " mm).");
    fp.package = PackageSpec{};
    fp.package.type = "CUSTOM";
    fp.padNumbers.clear();
    for (const auto& p : pads) {
        PackageSpec::Land l{tidy(p.x - cx), tidy(p.y - cy), tidy(p.w), tidy(p.h), tidy(p.drill), p.round, {}};
        if (std::fabs(l.x) > 60 || std::fabs(l.y) > 60)
            throw ImportError("pad " + p.number + " lies more than 60 mm from the footprint centre");
        if (l.w > 30 || l.h > 30) throw ImportError("pad " + p.number + " is larger than 30 mm");
        fp.package.lands.push_back(l);
        fp.padNumbers.push_back(p.number);
    }
    fp.package.pinCount = static_cast<int>(fp.package.lands.size());
    // Body: the fab outline, else the courtyard less its 0.25 mm margin, else none (the pads' extent).
    double bw = body.w(), bd = body.h();
    if ((bw <= 0.5 || bd <= 0.5) && !courtyard.empty()) {
        bw = courtyard.w() - 0.5;
        bd = courtyard.h() - 0.5;
    }
    if (bw > 0.5 && bw <= 60 && bd > 0.5 && bd <= 60) {
        fp.package.bodySize = tidy(bw);
        fp.package.bodyDepth = tidy(bd);
    }
}

/// A rectangle w × h rotated by `deg`: right angles swap the sides; other angles take the bounding box.
void rotateSize(double deg, double& w, double& h, bool& oblique) {
    double r = std::fmod(deg, 180.0);
    if (r < 0) r += 180;
    if (std::fabs(r) < 0.01 || std::fabs(r - 180) < 0.01) return;
    if (std::fabs(r - 90) < 0.01) {
        std::swap(w, h);
        return;
    }
    const double a = r * kPi / 180, c = std::fabs(std::cos(a)), s = std::fabs(std::sin(a));
    const double nw = w * c + h * s, nh = w * s + h * c;
    w = nw;
    h = nh;
    oblique = true;
}

std::string joinLimited(const std::vector<std::string>& items, size_t max = 8, const char* sep = ", ") {
    std::string out;
    for (size_t i = 0; i < items.size() && i < max; ++i) out += (i ? sep : "") + items[i];
    if (items.size() > max) out += " … (" + std::to_string(items.size()) + " in all)";
    return out;
}

// ------------------------------------------------------------------ symbol layout from pin positions

struct PlacedPin {
    std::string number, name;
    char side = 'L';
    double x = 0, y = 0;  // connection point, y up
    int unit = 0;         // layout group (KiCad unit, Eagle gate)
    bool hidden = false;
    bool ground = false, power = false;
};

/// Sides and slots from the pins' positions: per unit, pins keep their order and spacing along each side
/// (one slot per 2.54 mm); units follow one another on each side, separated by an empty slot. Hidden pins go to
/// the top (supplies), bottom (grounds) or right. Duplicate numbers keep their first placement.
SymbolSpec layoutFromPositions(const std::vector<PlacedPin>& pins) {
    SymbolSpec out;
    std::set<std::string> placed;
    int next[4] = {0, 0, 0, 0};  // next free slot per side L R T B
    auto sideIndex = [](char s) { return s == 'L' ? 0 : s == 'R' ? 1 : s == 'T' ? 2 : 3; };
    std::set<int> units;
    for (const auto& p : pins)
        if (!p.hidden) units.insert(p.unit);
    for (int unit : units) {
        int used[4] = {0, 0, 0, 0};
        for (char side : {'L', 'R', 'T', 'B'}) {
            std::vector<const PlacedPin*> on;
            for (const auto& p : pins)
                if (!p.hidden && p.unit == unit && p.side == side && !placed.count(upper(p.number))) on.push_back(&p);
            if (on.empty()) continue;
            const bool vertical = side == 'L' || side == 'R';
            auto key = [vertical](const PlacedPin* p) { return vertical ? -p->y : p->x; };
            std::stable_sort(on.begin(), on.end(), [&](const PlacedPin* a, const PlacedPin* b) { return key(a) < key(b) - 1e-6; });
            const double k0 = key(on.front());
            // Slots from the spacing; pins less than a slot apart (1.27 mm pitch) fall back to their rank.
            std::vector<int> slot(on.size());
            bool grid = true;
            for (size_t i = 0; i < on.size(); ++i) {
                const double q = (key(on[i]) - k0) / kGrid;
                slot[i] = static_cast<int>(std::lround(q));
                if (slot[i] > 400) grid = false;
                if (i > 0 && slot[i] == slot[i - 1] && std::fabs(key(on[i]) - key(on[i - 1])) > 1e-3) grid = false;
                if (i > 0 && slot[i] == slot[i - 1] && on[i]->name != on[i - 1]->name) grid = false;
            }
            if (!grid) {
                int rank = 0;
                for (size_t i = 0; i < on.size(); ++i) {
                    if (i > 0 && (std::fabs(key(on[i]) - key(on[i - 1])) > 1e-3 || on[i]->name != on[i - 1]->name)) ++rank;
                    slot[i] = rank;
                }
            }
            const int k = sideIndex(side);
            const int base = next[k] > 0 ? next[k] + 1 : 0;  // a gap between units
            for (size_t i = 0; i < on.size(); ++i) {
                if (!placed.insert(upper(on[i]->number)).second) continue;
                const int s = base + slot[i];
                if (s > 511) continue;
                out.pins.push_back({on[i]->number, side, s});
                used[k] = std::max(used[k], s + 1);
            }
        }
        for (int k = 0; k < 4; ++k) next[k] = std::max(next[k], used[k]);
    }
    std::map<std::pair<char, std::string>, int> hiddenSlot;  // hidden pins of one name share a slot (stacked supplies)
    for (const auto& p : pins) {
        if (!p.hidden || placed.count(upper(p.number))) continue;
        const char side = p.ground ? 'B' : p.power ? 'T' : 'R';
        const int k = sideIndex(side);
        auto it = hiddenSlot.find({side, p.name});
        int slot = 0;
        if (it != hiddenSlot.end()) {
            slot = it->second;
        } else {
            if (next[k] > 511) continue;
            slot = next[k]++;
            hiddenSlot[{side, p.name}] = slot;
        }
        placed.insert(upper(p.number));
        out.pins.push_back({p.number, side, slot});
    }
    return out;
}

bool groundName(const std::string& name) {
    const std::string u = upper(name);
    return u.rfind("GND", 0) == 0 || u == "AGND" || u == "DGND" || u == "PGND" || u.rfind("VSS", 0) == 0 || u == "VEE" ||
           u == "V-" || u == "0V" || u == "EP" || u == "PAD";
}

/// "~{RESET}" (KiCad) / "!RESET!" (Eagle) overbars become an "n" prefix; "~" alone is no name.
std::string pinNameText(std::string name) {
    if (name == "~") return {};
    std::string out;
    for (size_t i = 0; i < name.size(); ++i) {
        if (name.compare(i, 2, "~{") == 0) {
            const size_t end = name.find('}', i);
            if (end != std::string::npos) {
                out += "n" + name.substr(i + 2, end - i - 2);
                i = end;
                continue;
            }
        }
        if (name[i] != '~') out += name[i];
    }
    return clean(out, 40);
}

PinType kicadPinType(const std::string& t) {
    if (t == "input") return PinType::Input;
    if (t == "output") return PinType::Output;
    if (t == "bidirectional" || t == "tri_state") return PinType::Bidirectional;
    if (t == "power_in") return PinType::PowerIn;
    if (t == "power_out") return PinType::PowerOut;
    if (t == "open_collector" || t == "open_emitter") return PinType::OpenCollector;
    if (t == "no_connect") return PinType::NoConnect;
    return PinType::Passive;  // passive, free, unspecified
}

/// Supply pins repeated as "passive" (KiCad's stacked duplicates) take the supply's type.
void promoteRepeatedSupplies(std::vector<CustomPin>& pins) {
    std::set<std::string> power;
    for (const auto& p : pins)
        if (p.type == PinType::PowerIn || p.type == PinType::PowerOut) power.insert(p.name);
    for (auto& p : pins)
        if (p.type == PinType::Passive && power.count(p.name)) p.type = PinType::PowerIn;
}

std::string refPrefixOf(const std::string& ref) {
    std::string out;
    for (char c : ref)
        if (std::isalpha(static_cast<unsigned char>(c)) || c == '_') out += c;
        else break;
    return out.empty() ? "U" : clean(out, 8);
}

// ------------------------------------------------------------------ KiCad footprint

bool copperLayer(const std::string& l, bool& front, bool& back) {
    if (l == "*.Cu" || l == "F&B.Cu") {
        front = back = true;
        return true;
    }
    if (l == "F.Cu") front = true;
    else if (l == "B.Cu") back = true;
    else if (l.size() > 3 && l.compare(l.size() - 3, 3, ".Cu") == 0) back = true;  // inner layers
    else return false;
    return true;
}

/// Bounding box of a KiCad graphic (line, rect, circle, arc, polygon, text-less shapes).
void graphicBox(const SNode& g, Box& box) {
    for (const char* key : {"start", "end", "mid", "center"}) {
        if (const SNode* p = g.child(key)) box.add(num(p, 1), num(p, 2));
    }
    if (g.head() == "fp_circle" || g.head() == "gr_circle") {
        const SNode* c = g.child("center");
        const SNode* e = g.child("end");
        if (c && e) {
            const double r = std::hypot(num(e, 1) - num(c, 1), num(e, 2) - num(c, 2));
            box.add(num(c, 1) - r, num(c, 2) - r);
            box.add(num(c, 1) + r, num(c, 2) + r);
        }
    }
    if (const SNode* pts = g.child("pts"))
        for (const auto& xy : pts->kids)
            if (xy.list && xy.head() == "xy") box.add(num(&xy, 1), num(&xy, 2));
}

}  // namespace

ImportedFootprint parseKicadFootprint(const std::string& text, const std::string& source) {
    if (text.size() > kMaxFileBytes) throw ImportError("the file is larger than 32 MB");
    const SNode root = SexprParser(text).parse();
    const std::string head = root.head();
    if (head != "footprint" && head != "module")
        throw ImportError("not a KiCad footprint: the file starts with (" + clean(head, 40) + ") instead of (footprint");
    ImportedFootprint fp;
    fp.source = source;
    fp.name = clean(root.atom(1), 80);
    if (fp.name.empty()) throw ImportError("the footprint has no name");
    if (const SNode* d = root.child("descr")) fp.description = clean(d->atom(1), 300);

    std::vector<RawPad> pads, bottomPads;
    Box courtyard, fab, silk;
    int skippedHoles = 0, pasteOnly = 0, oblique = 0, slots = 0, backOnly = 0, customShapes = 0, grown = 0, tiny = 0;
    for (const auto& k : root.kids) {
        if (!k.list) continue;
        const std::string h = k.head();
        if (h == "pad") {
            RawPad p;
            p.number = clean(k.atom(1), 16);
            const std::string type = k.atom(2), shape = k.atom(3);
            const SNode* at = k.child("at");
            const SNode* size = k.child("size");
            if (!at || !size) throw ImportError("line " + std::to_string(k.line) + ": pad " + p.number + " has no position or size");
            p.x = num(at, 1);
            p.y = num(at, 2);
            const double rot = num(at, 3);
            p.w = num(size, 1);
            p.h = num(size, 2, p.w);
            bool front = false, back = false, copper = false;
            if (const SNode* layers = k.child("layers"))
                for (size_t i = 1; i < layers->kids.size(); ++i) copper |= copperLayer(layers->atom(i), front, back);
            if (type == "np_thru_hole") {
                ++skippedHoles;
                continue;
            }
            if (!copper) {
                ++pasteOnly;
                continue;
            }
            if (type == "thru_hole") {
                if (const SNode* drill = k.child("drill")) {
                    std::vector<double> d;
                    for (size_t i = 1; i < drill->kids.size(); ++i) {
                        double v = 0;
                        if (parseNumber(drill->atom(i), v)) d.push_back(v);
                    }
                    if (!d.empty()) p.drill = d.size() > 1 ? std::min(d[0], d[1]) : d[0];
                    if (d.size() > 1 && std::fabs(d[0] - d[1]) > 1e-6) ++slots;
                }
            }
            const bool bottomOnly = type != "thru_hole" && !front && back;
            if (shape == "custom") {  // anchor plus primitives: their bounding box
                ++customShapes;
                Box b;
                b.add(-p.w / 2, -p.h / 2);
                b.add(p.w / 2, p.h / 2);
                if (const SNode* prim = k.child("primitives"))
                    for (const auto& g : prim->kids)
                        if (g.list) graphicBox(g, b);
                p.w = std::max(std::fabs(b.x0), std::fabs(b.x1)) * 2;
                p.h = std::max(std::fabs(b.y0), std::fabs(b.y1)) * 2;
            }
            p.round = shape == "circle" || shape == "oval";
            if (shape == "circle") p.h = p.w;
            bool obl = false;
            rotateSize(rot, p.w, p.h, obl);
            if (obl) {
                ++oblique;
                p.round = false;
            }
            if (!std::isfinite(p.w) || !std::isfinite(p.h) || p.w <= 0.05 || p.h <= 0.05) {
                ++tiny;
                continue;
            }
            if (p.drill < 0 || !std::isfinite(p.drill)) p.drill = 0;
            if (p.drill > 0 && p.drill >= std::min(p.w, p.h) - 0.01) {
                const double grow = p.drill + 0.2;
                p.w = std::max(p.w, grow);
                p.h = std::max(p.h, grow);
                ++grown;
            }
            (bottomOnly ? bottomPads : pads).push_back(p);
        } else if (h == "model" && fp.modelPath.empty()) {
            // (model "path" (hide yes) (offset (xyz …)) (scale (xyz …)) (rotate (xyz …))); KiCad 5 writes the offset
            // as (at (xyz …)) in inches. KiCad turns models by the negative of the written angles.
            const SNode* hide = k.child("hide");
            if (k.hasAtom("hide") || (hide && hide->atom(1) != "no")) continue;
            fp.modelPath = clean(k.atom(1), 400);
            auto xyz = [&](const char* name, std::array<double, 3> def, double factor) {
                const SNode* n = k.child(name);
                const SNode* v = n ? n->child("xyz") : nullptr;
                if (!v) return def;
                std::array<double, 3> out{};
                for (size_t i = 0; i < 3; ++i) {
                    out[i] = num(v, i + 1, def[i]) * factor;
                    if (!std::isfinite(out[i])) out[i] = def[i];
                }
                return out;
            };
            Model3DRef& a = fp.modelAlign;
            a.offset = k.child("offset") ? xyz("offset", {0, 0, 0}, 1.0) : xyz("at", {0, 0, 0}, 25.4);
            a.scale = xyz("scale", {1, 1, 1}, 1.0);
            const auto r = xyz("rotate", {0, 0, 0}, 1.0);
            a.rotate = {-r[0], -r[1], -r[2]};
            for (auto& s : a.scale)
                if (!(s > 1e-6 && s <= 1000)) s = 1;
            for (auto& o : a.offset) o = std::clamp(o, -200.0, 200.0);
            for (auto& d : a.rotate) d = std::clamp(d, -3600.0, 3600.0);
        } else if (h == "fp_line" || h == "fp_rect" || h == "fp_circle" || h == "fp_arc" || h == "fp_poly") {
            const SNode* layer = k.child("layer");
            const std::string l = layer ? layer->atom(1) : std::string();
            if (l == "F.CrtYd") graphicBox(k, courtyard);
            else if (l == "F.Fab") graphicBox(k, fab);
            else if (l == "F.SilkS" || l == "F.Silkscreen") graphicBox(k, silk);
        }
    }
    // Bottom-side pads: a copy of a top pad (the heat-spreading pad under an exposed pad) is left out; any other is
    // imported on the top side.
    int bottomCopies = 0;
    for (const auto& b : bottomPads) {
        bool copy = false;
        for (const auto& t : pads) copy = copy || (!b.number.empty() && t.number == b.number);
        if (copy) {
            ++bottomCopies;
        } else {
            ++backOnly;
            pads.push_back(b);
        }
    }
    Box body = fab.empty() ? silk : fab;
    finishLands(fp, pads, courtyard, body);
    if (skippedHoles)
        fp.warnings.push_back(std::to_string(skippedHoles) +
                              " non-plated hole(s) not imported (mounting / locating holes: add them on the board).");
    if (pasteOnly) fp.warnings.push_back(std::to_string(pasteOnly) + " paste-only aperture(s) without copper skipped.");
    if (oblique) fp.warnings.push_back(std::to_string(oblique) + " pad(s) at an angle imported as their bounding box.");
    if (slots) fp.warnings.push_back(std::to_string(slots) + " slotted drill(s) imported as round holes of the slot width.");
    if (backOnly) fp.warnings.push_back(std::to_string(backOnly) + " bottom-side pad(s) imported on the top side.");
    if (bottomCopies)
        fp.warnings.push_back(std::to_string(bottomCopies) + " bottom-side copy(ies) of a top pad (heat spreader) not imported.");
    if (customShapes) fp.warnings.push_back(std::to_string(customShapes) + " custom-shaped pad(s) imported as their bounding box.");
    if (grown) fp.warnings.push_back(std::to_string(grown) + " pad(s) enlarged to leave a 0.1 mm ring around the drill.");
    if (tiny) fp.warnings.push_back(std::to_string(tiny) + " pad(s) smaller than 0.05 mm skipped.");
    return fp;
}

// ------------------------------------------------------------------ KiCad symbols

namespace {

struct RawSymbol {
    ImportedSymbol sym;
    std::string extends;
    std::vector<PlacedPin> placed;
    std::vector<CustomPin> pins;
    bool resolved = false;
};

/// "NAME_2_1" → unit 2, style 1 (−1 when the name has no such suffix).
void unitOf(const std::string& unitName, int& unit, int& style) {
    unit = 0;
    style = 1;
    const size_t b = unitName.rfind('_');
    if (b == std::string::npos || b == 0) return;
    const size_t a = unitName.rfind('_', b - 1);
    if (a == std::string::npos) return;
    double u = 0, s = 0;
    if (parseNumber(unitName.substr(a + 1, b - a - 1), u) && parseNumber(unitName.substr(b + 1), s) && u >= 0 && u < 1000 &&
        s >= 0 && s < 10) {
        unit = static_cast<int>(u);
        style = static_cast<int>(s);
    }
}

void readPins(const SNode& unitNode, int unit, RawSymbol& raw, std::set<std::string>& seen, int& skipped) {
    for (const auto& k : unitNode.kids) {
        if (!k.list || k.head() != "pin") continue;
        const SNode* at = k.child("at");
        const SNode* nameNode = k.child("name");
        const SNode* numberNode = k.child("number");
        const std::string number = clean(numberNode ? numberNode->atom(1) : std::string(), 16);
        if (number.empty()) {
            ++skipped;
            continue;
        }
        const std::string key = upper(number);
        if (seen.count(key)) continue;  // repeated in another unit or stacked
        seen.insert(key);
        bool hidden = k.hasAtom("hide");
        if (const SNode* hide = k.child("hide")) hidden = hide->atom(1) != "no";
        CustomPin pin;
        pin.number = number;
        pin.name = pinNameText(nameNode ? nameNode->atom(1) : std::string());
        if (pin.name.empty()) pin.name = number;
        pin.type = kicadPinType(k.atom(1));
        raw.pins.push_back(pin);
        PlacedPin pp;
        pp.number = number;
        pp.name = pin.name;
        pp.x = num(at, 1);
        pp.y = num(at, 2);
        // The pin points from its connection point towards the body: 0° → body to the right → pin on the left.
        double rot = std::fmod(num(at, 3), 360.0);
        if (rot < 0) rot += 360;
        pp.side = rot < 45 || rot >= 315 ? 'L' : rot < 135 ? 'B' : rot < 225 ? 'R' : 'T';
        pp.unit = unit;
        pp.hidden = hidden;
        pp.ground = groundName(pin.name);
        pp.power = pin.type == PinType::PowerIn || pin.type == PinType::PowerOut;
        raw.placed.push_back(pp);
    }
}

}  // namespace

std::vector<ImportedSymbol> parseKicadSymbols(const std::string& text, const std::string& source) {
    if (text.size() > kMaxFileBytes) throw ImportError("the file is larger than 32 MB");
    if (text.find("EESchema-LIBRARY") != std::string::npos)
        throw ImportError("KiCad 5 .lib symbol libraries are not supported: open the library in KiCad 6 or later and "
                          "save it as .kicad_sym");
    const SNode root = SexprParser(text).parse();
    if (root.head() != "kicad_symbol_lib")
        throw ImportError("not a KiCad symbol library: the file starts with (" + clean(root.head(), 40) +
                          ") instead of (kicad_symbol_lib");
    std::vector<RawSymbol> raws;
    for (const auto& s : root.kids) {
        if (!s.list || s.head() != "symbol") continue;
        if (raws.size() >= kMaxParts) throw ImportError("the library has more than 2000 symbols");
        RawSymbol raw;
        ImportedSymbol& sym = raw.sym;
        sym.source = source;
        sym.name = clean(s.atom(1), 80);
        if (sym.name.empty()) continue;
        if (const SNode* e = s.child("extends")) raw.extends = clean(e->atom(1), 80);
        for (const auto& k : s.kids) {
            if (!k.list || k.head() != "property") continue;
            const std::string key = k.atom(1), value = k.atom(2);
            if (key == "Reference") sym.refPrefix = refPrefixOf(value);
            else if (key == "Value") sym.value = clean(value, 80);
            else if (key == "Footprint") sym.footprint = clean(value, 160);
            else if (key == "Datasheet") sym.datasheet = value == "~" ? std::string() : clean(value, 300);
            else if (key == "Description" || key == "ki_description") sym.description = clean(value, 300);
            else if (key == "Manufacturer" || key == "MANUFACTURER" || key == "Mfr" || key == "MF")
                sym.manufacturer = clean(value, 80);
            else if (key == "ki_fp_filters") {
                size_t pos = 0;
                while (pos < value.size() && sym.footprintFilters.size() < 32) {
                    while (pos < value.size() && value[pos] == ' ') ++pos;
                    size_t end = value.find(' ', pos);
                    if (end == std::string::npos) end = value.size();
                    if (end > pos) sym.footprintFilters.push_back(clean(value.substr(pos, end - pos), 80));
                    pos = end;
                }
            }
        }
        // Units: "NAME_U_S" (unit U, 0 = common to all units; body style S, 2 = De Morgan alternative).
        std::set<std::string> seen;
        int skipped = 0, maxUnit = 0;
        std::vector<std::pair<int, const SNode*>> units;
        for (const auto& k : s.kids) {
            if (!k.list || k.head() != "symbol") continue;
            int unit = 0, style = 1;
            unitOf(k.atom(1), unit, style);
            if (style > 1) continue;
            units.emplace_back(unit, &k);
            maxUnit = std::max(maxUnit, unit);
        }
        std::stable_sort(units.begin(), units.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        readPins(s, 0, raw, seen, skipped);  // pins written directly in the symbol (old files)
        for (const auto& u : units) readPins(*u.second, u.first, raw, seen, skipped);
        sym.units = std::max(1, maxUnit);
        if (skipped) sym.warnings.push_back(std::to_string(skipped) + " pin(s) without a number skipped.");
        raws.push_back(std::move(raw));
    }
    // Derived symbols ("extends") take the parent's pins and keep their own properties.
    std::function<void(RawSymbol&, int)> resolve = [&](RawSymbol& r, int depth) {
        if (r.resolved || r.extends.empty()) {
            r.resolved = true;
            return;
        }
        r.resolved = true;
        if (depth > 8) return;
        for (auto& parent : raws) {
            if (parent.sym.name != r.extends || &parent == &r) continue;
            resolve(parent, depth + 1);
            if (r.pins.empty()) {
                r.pins = parent.pins;
                r.placed = parent.placed;
                r.sym.units = parent.sym.units;
            }
            if (r.sym.footprint.empty()) r.sym.footprint = parent.sym.footprint;
            if (r.sym.datasheet.empty()) r.sym.datasheet = parent.sym.datasheet;
            if (r.sym.description.empty()) r.sym.description = parent.sym.description;
            if (r.sym.manufacturer.empty()) r.sym.manufacturer = parent.sym.manufacturer;
            if (r.sym.footprintFilters.empty()) r.sym.footprintFilters = parent.sym.footprintFilters;
            return;
        }
        r.sym.extends = r.extends;  // in another file: importLibraryFiles resolves it
    };
    for (auto& r : raws) resolve(r, 0);
    std::vector<ImportedSymbol> out;
    for (auto& r : raws) {
        r.sym.pins = r.pins;
        promoteRepeatedSupplies(r.sym.pins);
        for (auto& pp : r.placed)
            for (const auto& pin : r.sym.pins)
                if (pin.number == pp.number) pp.power = pin.type == PinType::PowerIn || pin.type == PinType::PowerOut;
        r.sym.symbol = layoutFromPositions(r.placed);
        if (r.sym.value.empty()) r.sym.value = r.sym.name;
        out.push_back(std::move(r.sym));
    }
    if (out.empty()) throw ImportError("the library contains no symbols");
    return out;
}

// ------------------------------------------------------------------ Eagle

namespace {

/// Eagle rotation "R90", "MR180", "SR45": angle in degrees and whether it is mirrored (bottom side).
double eagleRot(const std::string& rot, bool& mirrored) {
    mirrored = false;
    size_t i = 0;
    while (i < rot.size() && (rot[i] == 'M' || rot[i] == 'S')) mirrored |= rot[i++] == 'M';
    if (i < rot.size() && rot[i] == 'R') ++i;
    double a = 0;
    return parseNumber(rot.substr(i), a) ? a : 0;
}

ImportedFootprint eaglePackage(const XNode& pkg, const std::string& source) {
    ImportedFootprint fp;
    fp.source = source;
    fp.name = clean(pkg.attr("name"), 80);
    if (const XNode* d = pkg.child("description")) fp.description = stripHtml(d->text);
    std::vector<RawPad> pads;
    Box docu, place;
    int holes = 0, oblique = 0, bottom = 0, grown = 0, tiny = 0;
    for (const auto& k : pkg.kids) {
        if (k.tag == "smd" || k.tag == "pad") {
            RawPad p;
            p.number = clean(k.attr("name"), 16);
            p.x = k.number("x");
            p.y = -k.number("y");  // Eagle's y points up
            bool mirrored = false;
            const double rot = eagleRot(k.attr("rot"), mirrored);
            if (k.tag == "smd") {
                p.w = k.number("dx");
                p.h = k.number("dy");
                p.round = k.number("roundness") >= 99;
                if (mirrored || k.attr("layer") == "16") ++bottom;
            } else {
                p.drill = k.number("drill");
                double d = k.number("diameter");
                // Automatic diameter: Eagle's default restring, 25 % of the drill within 0.254…0.508 mm.
                if (d <= 0) d = p.drill + 2 * std::clamp(0.25 * p.drill, 0.254, 0.508);
                const std::string shape = k.attr("shape", "round");
                p.w = p.h = d;
                p.round = shape != "square";
                if (shape == "long" || shape == "offset") {
                    p.w = 2 * d;
                    if (shape == "offset") {
                        const double a = rot * kPi / 180;
                        p.x += d / 2 * std::cos(a);
                        p.y -= d / 2 * std::sin(a);
                    }
                }
            }
            bool obl = false;
            rotateSize(rot, p.w, p.h, obl);
            if (obl) {
                ++oblique;
                p.round = false;
            }
            if (!std::isfinite(p.w) || !std::isfinite(p.h) || p.w <= 0.05 || p.h <= 0.05) {
                ++tiny;
                continue;
            }
            if (p.drill > 0 && p.drill >= std::min(p.w, p.h) - 0.01) {
                p.w = std::max(p.w, p.drill + 0.2);
                p.h = std::max(p.h, p.drill + 0.2);
                ++grown;
            }
            pads.push_back(p);
        } else if (k.tag == "hole") {
            ++holes;
        } else if (k.tag == "wire" || k.tag == "rectangle" || k.tag == "circle" || k.tag == "polygon") {
            const std::string layer = k.attr("layer");
            Box* box = layer == "51" ? &docu : layer == "21" ? &place : nullptr;
            if (!box) continue;
            if (k.tag == "circle") {
                const double r = k.number("radius");
                box->add(k.number("x") - r, -k.number("y") - r);
                box->add(k.number("x") + r, -k.number("y") + r);
            } else if (k.tag == "polygon") {
                for (const auto& v : k.kids)
                    if (v.tag == "vertex") box->add(v.number("x"), -v.number("y"));
            } else {
                box->add(k.number("x1"), -k.number("y1"));
                box->add(k.number("x2"), -k.number("y2"));
            }
        }
    }
    if (fp.name.empty()) throw ImportError("line " + std::to_string(pkg.line) + ": a package has no name");
    try {
        finishLands(fp, pads, Box{}, docu.empty() ? place : docu);
    } catch (const ImportError& e) {
        throw ImportError("package " + fp.name + ": " + e.what());
    }
    if (holes) fp.warnings.push_back(std::to_string(holes) + " non-plated hole(s) not imported (mounting / locating holes: add them on the board).");
    if (oblique) fp.warnings.push_back(std::to_string(oblique) + " pad(s) at an angle imported as their bounding box.");
    if (bottom) fp.warnings.push_back(std::to_string(bottom) + " bottom-side pad(s) imported on the top side.");
    if (grown) fp.warnings.push_back(std::to_string(grown) + " pad(s) enlarged to leave a 0.1 mm ring around the drill.");
    if (tiny) fp.warnings.push_back(std::to_string(tiny) + " pad(s) smaller than 0.05 mm skipped.");
    return fp;
}

PinType eaglePinType(const std::string& d) {
    if (d == "in") return PinType::Input;
    if (d == "out") return PinType::Output;
    if (d == "pwr") return PinType::PowerIn;
    if (d == "sup") return PinType::PowerOut;
    if (d == "oc") return PinType::OpenCollector;
    if (d == "nc") return PinType::NoConnect;
    if (d == "pas") return PinType::Passive;
    return PinType::Bidirectional;  // io (the default), hiz
}

/// "GND@2" → "GND"; "!RESET" → "nRESET".
std::string eaglePinName(const std::string& raw) {
    std::string n = raw.substr(0, raw.find('@'));
    if (!n.empty() && n[0] == '!') {
        n.erase(std::remove(n.begin(), n.end(), '!'), n.end());
        n = "n" + n;
    } else {
        n.erase(std::remove(n.begin(), n.end(), '!'), n.end());
    }
    return clean(n, 40);
}

}  // namespace

void parseEagleLibrary(const std::string& text, const std::string& source, LibraryImport& into) {
    if (text.size() > kMaxFileBytes) throw ImportError("the file is larger than 32 MB");
    const XNode root = XmlParser(text).parse();
    if (root.tag != "eagle") throw ImportError("not an Eagle library: the root element is <" + clean(root.tag, 40) + ">");
    if (into.files.empty()) into.files.push_back({source, "eagle_lbr", 0, 0, {}});
    std::vector<const XNode*> libraries;
    collect(root, "library", libraries);
    if (libraries.empty()) throw ImportError("the file contains no <library>");
    for (const XNode* lib : libraries) {
        std::map<std::string, size_t> packageIndex;  // name → index in into.footprints
        std::map<std::string, std::string> packageErrors;
        std::map<std::string, const XNode*> symbols;
        std::vector<const XNode*> packages, syms, devicesets;
        collect(*lib, "package", packages);
        collect(*lib, "symbol", syms);
        collect(*lib, "deviceset", devicesets);
        for (const XNode* s : syms) symbols[s->attr("name")] = s;
        for (const XNode* p : packages) {
            if (into.footprints.size() >= kMaxParts) throw ImportError("the library has more than 2000 packages");
            const std::string name = p->attr("name");
            try {
                into.footprints.push_back(eaglePackage(*p, source));
                packageIndex[name] = into.footprints.size() - 1;
            } catch (const ImportError& e) {
                packageErrors[name] = e.what();
                into.files.back().error += std::string(into.files.back().error.empty() ? "" : "; ") + e.what();
            }
        }
        for (const XNode* ds : devicesets) {
            const std::string dsName = ds->attr("name");
            std::string prefix = clean(ds->attr("prefix"), 8);
            std::string description;
            if (const XNode* d = ds->child("description")) description = stripHtml(d->text);
            std::vector<const XNode*> gates, devices;
            collect(*ds, "gate", gates);
            collect(*ds, "device", devices);
            for (const XNode* dev : devices) {
                const std::string packageName = dev->attr("package");
                if (packageName.empty()) continue;  // a symbol-only device (supply symbol, frame)
                std::vector<std::string> technologies;
                std::vector<const XNode*> techs;
                collect(*dev, "technology", techs);
                for (const XNode* t : techs) technologies.push_back(t->attr("name"));
                if (technologies.empty()) technologies.push_back("");
                for (const std::string& tech : technologies) {
                    if (into.parts.size() >= kMaxParts) throw ImportError("the library has more than 2000 devices");
                    // Eagle names: '*' stands for the technology, '?' for the package variant (else appended).
                    std::string name = dsName, variant = dev->attr("name");
                    const size_t star = name.find('*');
                    if (star != std::string::npos) name.replace(star, 1, tech);
                    else name += tech;
                    const size_t q = name.find('?');
                    if (q != std::string::npos) name.replace(q, 1, variant);
                    else name += variant;
                    ImportedPart part;
                    part.source = source;
                    part.footprintName = clean(packageName, 80);
                    part.symbolName = clean(name, 80);
                    ImportedSymbol sym;
                    sym.name = clean(name, 80);
                    sym.refPrefix = prefix.empty() ? "U" : prefix;
                    sym.value = sym.name;
                    sym.description = description;
                    sym.source = source;
                    sym.units = static_cast<int>(gates.size());
                    // Pins: one per pad of each connect (a pin on several pads becomes stacked pins of one name).
                    std::vector<PlacedPin> placed;
                    std::set<std::string> seen;
                    int unit = 0;
                    for (const XNode* gate : gates) {
                        ++unit;
                        const std::string gateName = gate->attr("name");
                        auto symIt = symbols.find(gate->attr("symbol"));
                        std::vector<const XNode*> connects;
                        collect(*dev, "connect", connects);
                        for (const XNode* c : connects) {
                            if (c->attr("gate") != gateName) continue;
                            const std::string pinName = c->attr("pin");
                            const XNode* symPin = nullptr;
                            if (symIt != symbols.end())
                                for (const auto& sp : symIt->second->kids)
                                    if (sp.tag == "pin" && sp.attr("name") == pinName) symPin = &sp;
                            // "pad" holds one pad, or several separated by spaces (Eagle 6+ "route any").
                            std::string pads = c->attr("pad");
                            size_t pos = 0;
                            while (pos < pads.size()) {
                                while (pos < pads.size() && pads[pos] == ' ') ++pos;
                                size_t end = pads.find(' ', pos);
                                if (end == std::string::npos) end = pads.size();
                                const std::string pad = clean(pads.substr(pos, end - pos), 16);
                                pos = end;
                                if (pad.empty() || !seen.insert(upper(pad)).second) continue;
                                CustomPin pin;
                                pin.number = pad;
                                pin.name = eaglePinName(pinName);
                                if (pin.name.empty()) pin.name = pad;
                                pin.type = eaglePinType(symPin ? symPin->attr("direction", "io") : std::string("io"));
                                sym.pins.push_back(pin);
                                PlacedPin pp;
                                pp.number = pad;
                                pp.name = pin.name;
                                pp.unit = unit;
                                if (symPin) {
                                    bool mirrored = false;
                                    double rot = std::fmod(eagleRot(symPin->attr("rot"), mirrored), 360.0);
                                    if (rot < 0) rot += 360;
                                    pp.x = symPin->number("x");
                                    pp.y = symPin->number("y");
                                    pp.side = rot < 45 || rot >= 315 ? 'L' : rot < 135 ? 'B' : rot < 225 ? 'R' : 'T';
                                } else {
                                    pp.hidden = true;
                                }
                                pp.ground = groundName(pin.name);
                                pp.power = pin.type == PinType::PowerIn || pin.type == PinType::PowerOut;
                                placed.push_back(pp);
                            }
                        }
                    }
                    sym.symbol = layoutFromPositions(placed);
                    auto fpIt = packageIndex.find(packageName);
                    if (fpIt == packageIndex.end()) {
                        part.spec.name = sym.name;
                        part.error = packageErrors.count(packageName)
                                         ? "Its package could not be imported (" + packageErrors[packageName] + ")."
                                         : "Its package " + clean(packageName, 80) + " is not in the library.";
                        into.parts.push_back(part);
                        continue;
                    }
                    ImportedPart made = makeImportedPart(&sym, &into.footprints[fpIt->second]);
                    made.source = source;
                    into.parts.push_back(std::move(made));
                }
            }
        }
        into.files.back().footprints += static_cast<int>(packages.size());
        into.files.back().symbols += static_cast<int>(devicesets.size());
    }
}

// ------------------------------------------------------------------ Altium (.SchLib, .PcbLib)

namespace {

PinType altiumPinType(int electrical) {
    switch (electrical) {
        case 0: return PinType::Input;
        case 1: return PinType::Bidirectional;
        case 2: return PinType::Output;
        case 3: return PinType::OpenCollector;
        case 5: return PinType::Bidirectional;   // hi-Z (tri-state)
        case 6: return PinType::OpenCollector;   // open emitter
        case 7: return PinType::PowerIn;
        default: return PinType::Passive;
    }
}

ImportedSymbol altiumSymbol(const AltiumSymbol& a, const std::string& source) {
    ImportedSymbol sym;
    sym.name = clean(a.name, 80);
    sym.value = sym.name;
    sym.refPrefix = refPrefixOf(a.designatorPrefix);
    sym.description = clean(a.description, 300);
    sym.footprint = clean(a.footprint, 120);
    sym.source = source;
    sym.units = a.partCount;
    auto param = [&](std::initializer_list<const char*> names) {
        for (const char* n : names)
            for (const auto& kv : a.parameters)
                if (upper(kv.first) == upper(n)) return clean(kv.second, 300);
        return std::string();
    };
    sym.manufacturer = param({"Manufacturer", "Manufacturer 1", "MFR"});
    sym.datasheet = param({"Datasheet", "DatasheetURL", "HelpURL", "ComponentLink1URL"});
    std::set<std::string> seen;
    std::vector<PlacedPin> placed;
    for (const auto& p : a.pins) {
        CustomPin pin;
        pin.number = clean(p.designator, 16);
        if (pin.number.empty() || !seen.insert(upper(pin.number)).second) continue;
        pin.name = clean(p.name, 40);
        if (pin.name.empty()) pin.name = pin.number;
        pin.type = altiumPinType(p.electrical);
        sym.pins.push_back(pin);
        PlacedPin pp;
        pp.number = pin.number;
        pp.name = pin.name;
        pp.side = p.side;
        pp.x = p.x;
        pp.y = p.y;
        pp.unit = p.part;
        pp.hidden = p.hidden;
        pp.ground = groundName(pin.name);
        pp.power = pin.type == PinType::PowerIn;
        placed.push_back(pp);
    }
    promoteRepeatedSupplies(sym.pins);
    sym.symbol = layoutFromPositions(placed);
    for (const auto& w : a.warnings) sym.warnings.push_back(clean(w, 200));
    return sym;
}

ImportedFootprint altiumFootprint(const AltiumFootprint& a, const std::string& source) {
    ImportedFootprint fp;
    fp.name = clean(a.name, 80);
    if (fp.name.empty()) throw ImportError("a footprint has no name");
    fp.description = clean(a.description, 300);
    fp.source = source;
    std::vector<RawPad> pads;
    int holes = 0, bottom = 0, oblique = 0, grown = 0;
    for (const auto& p : a.pads) {
        if (p.drill > 0 && !p.plated) {
            ++holes;
            continue;
        }
        RawPad r;
        r.number = clean(p.name, 16);
        r.x = tidy(p.x);
        r.y = tidy(p.y);
        r.w = p.w;
        r.h = p.h;
        bool angled = false;
        rotateSize(p.rotation, r.w, r.h, angled);
        oblique += angled;
        r.drill = p.drill > 0 ? tidy(p.drill) : 0;
        if (r.drill > 0 && r.drill > std::min(r.w, r.h) - 0.2) {
            r.w = std::max(r.w, r.drill + 0.2);
            r.h = std::max(r.h, r.drill + 0.2);
            ++grown;
        }
        r.w = tidy(r.w);
        r.h = tidy(r.h);
        r.round = p.shape == 1;
        if (p.layer == 32 && r.drill == 0) ++bottom;
        pads.push_back(r);
    }
    Box body;
    if (a.hasOutline) {
        body.add(a.outlineX0, a.outlineY0);
        body.add(a.outlineX1, a.outlineY1);
    }
    finishLands(fp, pads, Box{}, body);
    if (holes)
        fp.warnings.push_back(std::to_string(holes) +
                              " non-plated hole(s) not imported (mounting / locating holes: add them on the board).");
    if (oblique) fp.warnings.push_back(std::to_string(oblique) + " pad(s) at an angle imported as their bounding box.");
    if (bottom) fp.warnings.push_back(std::to_string(bottom) + " bottom-side pad(s) imported on the top side.");
    if (grown) fp.warnings.push_back(std::to_string(grown) + " pad(s) enlarged to leave a 0.1 mm ring around the drill.");
    for (const auto& w : a.warnings) fp.warnings.push_back(clean(w, 200));
    return fp;
}

/// An Altium binary library: symbols (.SchLib) or footprints (.PcbLib).
void importAltium(const ImportFile& f, const std::string& name, LibraryImport& out, LibraryImport::FileResult& res) {
    if (!CompoundFile::looksLikeCompoundFile(f.content))
        throw ImportError("this Altium library is not a binary (OLE compound) file: ASCII Altium libraries are not "
                          "supported; save it as a binary .SchLib / .PcbLib in Altium");
    std::unique_ptr<CompoundFile> cfb;
    try {
        cfb = std::make_unique<CompoundFile>(f.content);
    } catch (const CfbError& e) {
        throw ImportError(std::string("Altium library: the compound file is damaged (") + e.what() + ")");
    }
    const std::string kind = altiumLibraryKind(*cfb, f.name);
    if (kind == "intlib")
        throw ImportError("Altium integrated libraries (.IntLib) are not read: extract the .SchLib and .PcbLib inside it "
                          "(Altium: Extract Sources; or KiCad 8) and import those");
    try {
        if (kind == "schlib") {
            for (const auto& a : readAltiumSchLib(*cfb)) {
                if (out.symbols.size() >= kMaxParts) throw ImportError("too many symbols in one import");
                out.symbols.push_back(altiumSymbol(a, name));
                ++res.symbols;
            }
        } else if (kind == "pcblib") {
            for (const auto& a : readAltiumPcbLib(*cfb)) {
                if (out.footprints.size() >= kMaxParts) throw ImportError("too many footprints in one import");
                try {
                    out.footprints.push_back(altiumFootprint(a, name));
                    ++res.footprints;
                } catch (const ImportError& e) {
                    res.error += (res.error.empty() ? "" : "; ") + clean(a.name, 80) + ": " + e.what();
                }
            }
        } else {
            throw ImportError("Altium compound file of an unknown kind: import .SchLib or .PcbLib files");
        }
    } catch (const CfbError& e) {
        throw ImportError(std::string("Altium library: ") + e.what());
    }
}

}  // namespace

// ------------------------------------------------------------------ pairing and validation

namespace {

/// Pads of different pins whose copper overlaps join the first pin's pad and the second pin is dropped (USB-C
/// receptacles put A1 and B12, both GND, on one pad): when both pins have the same name, or (for pins made up from
/// pad numbers) always. Returns "B12 into A1" notes.
std::vector<std::string> mergeOverlappingPins(CustomPartSpec& spec, bool anyName) {
    std::vector<std::string> notes;
    auto& lands = spec.package.lands;
    auto pinOf = [&lands](size_t i) { return lands[i].pin.empty() ? std::to_string(i + 1) : upper(lands[i].pin); };
    auto indexOf = [&spec](const std::string& number) {
        for (size_t i = 0; i < spec.pins.size(); ++i)
            if (upper(spec.pins[i].number) == number) return static_cast<int>(i);
        return -1;
    };
    for (int merges = 0; merges < 64; ++merges) {
        int keep = -1, drop = -1;
        for (size_t i = 0; i < lands.size() && drop < 0; ++i) {
            const std::string a = pinOf(i);
            if (a == "-") continue;
            for (size_t j = i + 1; j < lands.size() && drop < 0; ++j) {
                const std::string b = pinOf(j);
                if (b == "-" || a == b) continue;
                const auto& x = lands[i];
                const auto& y = lands[j];
                if (std::fabs(x.x - y.x) >= (x.w + y.w) / 2 - 1e-6 || std::fabs(x.y - y.y) >= (x.h + y.h) / 2 - 1e-6) continue;
                const int ia = indexOf(a), ib = indexOf(b);
                if (ia < 0 || ib < 0) continue;
                if (!anyName && upper(spec.pins[static_cast<size_t>(ia)].name) != upper(spec.pins[static_cast<size_t>(ib)].name))
                    continue;
                keep = ia;
                drop = ib;
            }
        }
        if (drop < 0) break;
        const std::string a = spec.pins[static_cast<size_t>(keep)].number, b = spec.pins[static_cast<size_t>(drop)].number;
        for (size_t k = 0; k < lands.size(); ++k)
            if (pinOf(k) == upper(b)) lands[k].pin = a == std::to_string(k + 1) ? std::string() : a;
        spec.pins.erase(spec.pins.begin() + drop);
        auto& sp = spec.symbol.pins;
        sp.erase(std::remove_if(sp.begin(), sp.end(), [&b](const SymbolPin& p) { return upper(p.number) == upper(b); }), sp.end());
        notes.push_back(b + " into " + a);
    }
    return notes;
}

}  // namespace

ImportedPart makeImportedPart(const ImportedSymbol* symbol, const ImportedFootprint* footprint) {
    ImportedPart r;
    CustomPartSpec& spec = r.spec;
    if (symbol) {
        r.symbolName = symbol->name;
        r.source = symbol->source;
        spec.name = symbol->name;
        spec.refPrefix = symbol->refPrefix.empty() ? "U" : symbol->refPrefix;
        spec.defaultValue = symbol->value.empty() ? symbol->name : symbol->value;
        spec.description = symbol->description;
        spec.datasheet = symbol->datasheet;
        spec.manufacturer = symbol->manufacturer;
        spec.pins = symbol->pins;
        std::stable_sort(spec.pins.begin(), spec.pins.end(),
                         [](const CustomPin& a, const CustomPin& b) { return naturalLess(a.number, b.number); });
        spec.symbol = symbol->symbol;
        r.warnings = symbol->warnings;
        if (symbol->units > 1)
            r.warnings.push_back("The symbol's " + std::to_string(symbol->units) +
                                 " units are drawn as one symbol, side by side in unit order.");
    }
    if (footprint) {
        r.footprintName = footprint->name;
        if (r.source.empty()) r.source = footprint->source;
        else if (footprint->source != r.source && !footprint->source.empty()) r.source += ", " + footprint->source;
        r.warnings.insert(r.warnings.end(), footprint->warnings.begin(), footprint->warnings.end());
    }
    if (!symbol && footprint) {
        // A footprint on its own: one passive pin per pad number.
        spec.name = footprint->name;
        spec.defaultValue = footprint->name;
        spec.description = footprint->description;
        const std::string u = upper(footprint->name);
        for (const char* k : {"CONN", "HEADER", "USB", "JST", "MOLEX", "TERMINAL", "SOCKET", "RJ45", "BARREL", "SMA"})
            if (u.find(k) != std::string::npos) spec.refPrefix = "J";
        std::set<std::string> seen;
        std::vector<std::string> numbers;
        for (const auto& n : footprint->padNumbers)
            if (!n.empty() && seen.insert(upper(n)).second) numbers.push_back(n);
        std::stable_sort(numbers.begin(), numbers.end(), naturalLess);
        for (const auto& n : numbers) spec.pins.push_back({n, n, PinType::Passive, {}});
        if (spec.pins.empty()) {
            r.error = "The footprint has no numbered pads, so it has no pins.";
            return r;
        }
    }
    if (spec.name.empty()) {
        r.error = "Nothing to import.";
        return r;
    }
    if (spec.pins.empty()) {
        r.error = symbol && !symbol->extends.empty()
                      ? "Its base symbol " + symbol->extends + " is not imported: add the file that defines it (" +
                            symbol->extends + ".kicad_sym in the same .kicad_symdir)."
                      : "The symbol has no pins.";
        return r;
    }
    if (footprint) {
        spec.package = footprint->package;
        std::map<std::string, std::string> pinByNumber;  // upper → as written
        for (const auto& p : spec.pins) pinByNumber[upper(p.number)] = p.number;
        std::vector<std::string> unused;
        for (size_t i = 0; i < spec.package.lands.size() && i < footprint->padNumbers.size(); ++i) {
            const std::string& n = footprint->padNumbers[i];
            auto it = pinByNumber.find(upper(n));
            auto& land = spec.package.lands[i];
            if (n.empty() || it == pinByNumber.end()) {
                land.pin = "-";
                if (!n.empty() && std::find(unused.begin(), unused.end(), n) == unused.end()) unused.push_back(n);
            } else {
                land.pin = it->second == std::to_string(i + 1) ? std::string() : it->second;
            }
        }
        if (!unused.empty())
            r.warnings.push_back("Pads " + joinLimited(unused) + " are on no symbol pin: imported as mechanical pads.");
        const auto merged = mergeOverlappingPins(spec, symbol == nullptr);
        if (!merged.empty())
            r.warnings.push_back("Pins sharing one pad were joined (" + joinLimited(merged) + "): they are one connection.");
    } else {
        // No footprint: a generated package from the name in the symbol's footprint field ("Lib:SOIC-8_3.9x4.9mm…").
        const std::string full = symbol ? symbol->footprint : std::string();
        std::string fpName = full.substr(full.find(':') == std::string::npos ? 0 : full.find(':') + 1);
        std::string pkgName = fpName.substr(0, fpName.find('_'));
        const size_t ep = upper(pkgName).find("-1EP");
        if (ep != std::string::npos) pkgName = pkgName.substr(0, ep);
        int pinCount = 0;
        const std::string type = pkgName.empty() ? std::string() : packageTypeFromName(pkgName, pinCount);
        if (type.empty() || usesLandPattern(type) || type == "BGA") {
            r.error = full.empty() ? "No footprint: import its .kicad_mod together with the symbol, or choose a package in the library."
                                   : "Footprint " + full + " was not imported: add its .kicad_mod file, or choose a package in the library.";
            spec.package.type = "SOIC";
            return r;
        }
        spec.package.type = type;
        spec.package.pinCount = pinCount;
        auto dim = [&](const char* tag) {  // "_P1.27mm", "_W7.62mm"
            const size_t at = fpName.find(tag);
            if (at == std::string::npos) return 0.0;
            const size_t start = at + std::strlen(tag), end = fpName.find("mm", start);
            double v = 0;
            return end != std::string::npos && parseNumber(fpName.substr(start, end - start), v) ? v : 0.0;
        };
        const double pitch = dim("_P");
        if (pitch > 0.2 && pitch <= 5.08) spec.package.pitch = pitch;
        if (type == "DIP") {
            const double w = dim("_W");
            if (w > 0.5 && w <= 60) spec.package.bodySize = w;
        } else if (type == "SOIC" || type == "TSSOP" || type == "QFN" || type == "LQFP") {
            const size_t x = fpName.find('x');
            const size_t us = fpName.rfind('_', x);
            double w = 0;
            if (x != std::string::npos && us != std::string::npos && parseNumber(fpName.substr(us + 1, x - us - 1), w) &&
                w > 0.5 && w <= 60)
                spec.package.bodySize = w;
        }
        r.warnings.push_back("Footprint " + fpName + " generated from its package name (" + type +
                             "): check it against the datasheet, or import the .kicad_mod.");
    }
    // Symbol: keep the library's layout when it is valid, else auto-arrange.
    if (!spec.symbol.empty()) {
        std::vector<std::string> stacked;
        for (const auto& issue : checkSymbol(spec)) {
            if (issue.severity == "warning") stacked.push_back(issue.message);
            if (issue.severity != "error") continue;
            spec.symbol = autoArrangeSymbol(spec);
            r.warnings.push_back("The symbol's pin layout could not be kept (" + issue.message +
                                 "); the pins were auto-arranged.");
            stacked.clear();
            break;
        }
        if (!stacked.empty()) r.warnings.push_back("Symbol check: " + joinLimited(stacked, 4, " "));
    }
    if (usesLandPattern(spec.package.type)) {
        std::vector<std::string> errors, warnings;
        for (const auto& issue : checkLandPattern(spec)) (issue.severity == "error" ? errors : warnings).push_back(issue.message);
        if (!warnings.empty()) r.warnings.push_back("Footprint check: " + joinLimited(warnings, 4, " "));
        if (!errors.empty()) {
            r.error = "Footprint check: " + joinLimited(errors, 4, " ");
            return r;
        }
    }
    try {
        spec = customPartSpecFromJson(customPartSpecToJson(spec));  // exactly what the app will send back
        CustomPartRegistry::instance().registerPart(spec);
        r.ok = true;
    } catch (const std::exception& e) {
        r.error = e.what();
    }
    return r;
}

namespace {

std::string extensionOf(const std::string& name) {
    const size_t dot = name.rfind('.');
    return dot == std::string::npos ? std::string() : lower(name.substr(dot + 1));
}

/// Format by extension, else by the first bytes.
std::string detectFormat(const ImportFile& f) {
    const std::string ext = extensionOf(f.name);
    if (ext == "kicad_mod") return "kicad_mod";
    if (ext == "kicad_sym") return "kicad_sym";
    if (ext == "lbr") return "eagle_lbr";
    if (f.content.compare(0, 4, "\xD0\xCF\x11\xE0") == 0 || ext == "schlib" || ext == "pcblib" || ext == "intlib")
        return "altium";
    if (isModel3DFile(f.name)) return "model3d";
    if (ext == "lib" || f.content.find("EESchema-LIBRARY") != std::string::npos) return "kicad5_lib";
    size_t i = 0;
    while (i < f.content.size() && i < 4096 && std::isspace(static_cast<unsigned char>(f.content[i]))) ++i;
    const std::string start = f.content.substr(i, 64);
    if (start.rfind("(footprint", 0) == 0 || start.rfind("(module", 0) == 0) return "kicad_mod";
    if (start.rfind("(kicad_symbol_lib", 0) == 0) return "kicad_sym";
    if (start.rfind("<?xml", 0) == 0 || start.rfind("<eagle", 0) == 0 || start.rfind("\xEF\xBB\xBF<", 0) == 0)
        return "eagle_lbr";
    return "unknown";
}

/// KiCad footprint filter ("SOIC*3.9x4.9mm*P1.27mm*", "SOT?23*"): '*' any run, '?' any one character, no case.
bool globMatch(const std::string& pattern, const std::string& text) {
    size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || std::tolower(static_cast<unsigned char>(pattern[p])) ==
                                                             std::tolower(static_cast<unsigned char>(text[t])))) {
            ++p;
            ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool coversPins(const ImportedFootprint& fp, const ImportedSymbol& sym) {
    std::set<std::string> pads;
    for (const auto& n : fp.padNumbers) pads.insert(upper(n));
    for (const auto& p : sym.pins)
        if (!pads.count(upper(p.number))) return false;
    return !sym.pins.empty();
}

std::string baseName(const std::string& footprintRef) {
    const size_t colon = footprintRef.find(':');
    return colon == std::string::npos ? footprintRef : footprintRef.substr(colon + 1);
}

}  // namespace

namespace {
/// File name without directories and extension, lower case ("${KICAD8_3DMODEL_DIR}/Package_SO.3dshapes/SOIC-8.wrl"
/// → "soic-8").
std::string modelStem(const std::string& path) {
    const size_t slash = path.find_last_of("/\\");
    std::string name = slash == std::string::npos ? path : path.substr(slash + 1);
    const size_t dot = name.rfind('.');
    if (dot != std::string::npos) name.resize(dot);
    return lower(name);
}

struct ImportedModel {
    std::string id, name, format;
};

/// Attaches the imported 3D model each footprint names (by file name; a .step reference takes the .wrl / .stl /
/// .obj of the same name) to the parts that use the footprint.
void attachModels(LibraryImport& out, const std::map<std::string, ImportedModel>& models) {
    for (auto& part : out.parts) {
        if (part.footprintName.empty()) continue;
        const ImportedFootprint* fp = nullptr;
        for (const auto& f : out.footprints)
            if (f.name == part.footprintName && part.source.find(f.source) != std::string::npos) fp = &f;
        if (!fp || fp->modelPath.empty()) continue;
        auto it = models.find(modelStem(fp->modelPath));
        if (it == models.end()) {
            const size_t slash = fp->modelPath.find_last_of("/\\");
            const std::string file = slash == std::string::npos ? fp->modelPath : fp->modelPath.substr(slash + 1);
            part.warnings.push_back("3D model " + clean(file, 120) + " is not in the import: add the library's .3dshapes "
                                    "folder (its .wrl files) to attach it");
            continue;
        }
        Model3DRef ref = fp->modelAlign;
        ref.id = it->second.id;
        ref.name = it->second.name;
        ref.unit = defaultModelUnit(it->second.format);
        // The model moves with the pads when the land pattern is centred (model y points up the footprint).
        ref.offset[0] = std::clamp(tidy(ref.offset[0] - fp->centreX), -200.0, 200.0);
        ref.offset[1] = std::clamp(tidy(ref.offset[1] + fp->centreY), -200.0, 200.0);
        part.spec.model3d = ref;
        part.warnings.push_back("3D model " + ref.name + " attached");
    }
}

}  // namespace

LibraryImport importLibraryFiles(const std::vector<ImportFile>& files, const std::map<std::string, std::string>& pairs) {
    LibraryImport out;
    std::map<std::string, ImportedModel> models;  // file stem → registered mesh
    for (const auto& f : files) {
        LibraryImport::FileResult fr;
        fr.name = clean(f.name, 200);
        fr.format = detectFormat(f);
        out.files.push_back(fr);
        auto& res = out.files.back();
        try {
            if (fr.format == "kicad_mod") {
                out.footprints.push_back(parseKicadFootprint(f.content, fr.name));
                res.footprints = 1;
            } else if (fr.format == "kicad_sym") {
                auto syms = parseKicadSymbols(f.content, fr.name);
                if (out.symbols.size() + syms.size() > kMaxParts) throw ImportError("too many symbols in one import");
                res.symbols = static_cast<int>(syms.size());
                for (auto& s : syms) out.symbols.push_back(std::move(s));
            } else if (fr.format == "eagle_lbr") {
                parseEagleLibrary(f.content, fr.name, out);
            } else if (fr.format == "altium") {
                importAltium(f, fr.name, out, res);
            } else if (fr.format == "model3d") {
                if (models.size() >= 2000) throw ImportError("too many 3D models in one import");
                Model3DMesh mesh = parseModel3D(f.content, fr.name);
                const std::string format = mesh.format;
                const std::string id = Model3DRegistry::instance().add(std::move(mesh));
                models[modelStem(fr.name)] = {id, fr.name, format};
            } else if (fr.format == "kicad5_lib") {
                throw ImportError("KiCad 5 .lib symbol libraries are not supported: open the library in KiCad 6 or "
                                  "later and save it as .kicad_sym");
            } else {
                throw ImportError("unknown file type: import KiCad .kicad_mod / .kicad_sym or Eagle .lbr files");
            }
        } catch (const std::exception& e) {
            res.error = res.error.empty() ? e.what() : res.error + "; " + e.what();
        }
    }
    // Derived symbols whose base symbol is in another imported file (one file per symbol in a .kicad_symdir).
    for (int pass = 0; pass < 8; ++pass) {
        bool changed = false;
        for (auto& sym : out.symbols) {
            if (sym.extends.empty() || !sym.pins.empty()) continue;
            for (const auto& base : out.symbols) {
                if (base.name != sym.extends || base.pins.empty()) continue;
                sym.pins = base.pins;
                sym.symbol = base.symbol;
                sym.units = base.units;
                if (sym.footprint.empty()) sym.footprint = base.footprint;
                if (sym.datasheet.empty()) sym.datasheet = base.datasheet;
                if (sym.description.empty()) sym.description = base.description;
                if (sym.manufacturer.empty()) sym.manufacturer = base.manufacturer;
                if (sym.footprintFilters.empty()) sym.footprintFilters = base.footprintFilters;
                sym.extends.clear();
                changed = true;
                break;
            }
        }
        if (!changed) break;
    }
    // Pair the KiCad symbols with footprints.
    std::map<std::string, size_t> byName;  // upper-case footprint name → index (KiCad footprints only)
    std::vector<size_t> kicadFootprints;
    std::set<size_t> eagleFootprints;
    for (const auto& p : out.parts)
        for (size_t i = 0; i < out.footprints.size(); ++i)
            if (out.footprints[i].name == p.footprintName && out.footprints[i].source == p.source) eagleFootprints.insert(i);
    for (size_t i = 0; i < out.footprints.size(); ++i) {
        const auto& srcFile = out.footprints[i].source;
        bool fromEagle = false;
        for (const auto& fr : out.files)
            if (fr.name == srcFile && fr.format == "eagle_lbr") fromEagle = true;
        if (fromEagle) continue;
        kicadFootprints.push_back(i);
        byName.emplace(upper(out.footprints[i].name), i);
    }
    std::set<size_t> used;
    for (const auto& sym : out.symbols) {
        if (out.parts.size() >= kMaxParts) break;
        const ImportedFootprint* fp = nullptr;
        std::string pairError;
        auto explicitPair = pairs.find(sym.name);
        if (explicitPair != pairs.end() && !explicitPair->second.empty()) {
            auto it = byName.find(upper(baseName(explicitPair->second)));
            if (it != byName.end()) fp = &out.footprints[it->second];
            else pairError = "Footprint " + clean(explicitPair->second, 80) + " chosen for it was not imported.";
        }
        if (!fp && pairError.empty() && !sym.footprint.empty()) {
            auto it = byName.find(upper(baseName(sym.footprint)));
            if (it != byName.end()) fp = &out.footprints[it->second];
        }
        std::string filterNote;
        if (!fp && pairError.empty() && !sym.footprintFilters.empty()) {
            // The symbol's footprint filters (ki_fp_filters): the first imported footprint that matches and has a pad
            // for every pin.
            std::vector<const ImportedFootprint*> matches;
            for (size_t i : kicadFootprints) {
                const auto& cand = out.footprints[i];
                bool match = false;
                for (const auto& f : sym.footprintFilters) match = match || globMatch(f, cand.name);
                if (match && coversPins(cand, sym)) matches.push_back(&cand);
            }
            if (!matches.empty()) fp = matches.front();
            if (matches.size() > 1)
                filterNote = "Paired with " + fp->name + ", the first of " + std::to_string(matches.size()) +
                             " imported footprints its footprint filter accepts.";
        }
        if (!fp && pairError.empty() && kicadFootprints.size() == 1 && coversPins(out.footprints[kicadFootprints[0]], sym)) {
            // A single symbol + footprint download (SnapEDA, Ultra Librarian, vendor sites): pair them when every pin
            // has a pad.
            fp = &out.footprints[kicadFootprints[0]];
        }
        if (fp) used.insert(static_cast<size_t>(fp - out.footprints.data()));
        ImportedPart part = pairError.empty() ? makeImportedPart(&sym, fp) : ImportedPart{};
        if (!filterNote.empty()) part.warnings.push_back(filterNote);
        if (!pairError.empty()) {
            part.spec.name = sym.name;
            part.symbolName = sym.name;
            part.source = sym.source;
            part.error = pairError;
        }
        out.parts.push_back(std::move(part));
    }
    // Footprints no symbol uses are parts of their own.
    for (size_t i : kicadFootprints) {
        if (used.count(i) || out.parts.size() >= kMaxParts) continue;
        out.parts.push_back(makeImportedPart(nullptr, &out.footprints[i]));
    }
    for (size_t i = 0; i < out.footprints.size(); ++i) {
        if (!eagleFootprints.count(i) && std::find(kicadFootprints.begin(), kicadFootprints.end(), i) == kicadFootprints.end() &&
            out.parts.size() < kMaxParts)
            out.parts.push_back(makeImportedPart(nullptr, &out.footprints[i]));  // an Eagle package no device uses
    }
    attachModels(out, models);
    return out;
}

std::vector<std::string> footprintCandidates(const LibraryImport& result, const ImportedSymbol& sym) {
    struct Scored {
        int exactCount, named, filtered;
        size_t padGap;
        std::string name;
    };
    std::vector<Scored> found;
    std::set<std::string> eagleFiles;
    for (const auto& f : result.files)
        if (f.format == "eagle_lbr") eagleFiles.insert(f.name);
    for (const auto& fp : result.footprints) {
        if (eagleFiles.count(fp.source) || !coversPins(fp, sym)) continue;
        std::set<std::string> pads;
        for (const auto& n : fp.padNumbers) pads.insert(upper(n));
        bool filtered = false;
        for (const auto& f : sym.footprintFilters) filtered = filtered || globMatch(f, fp.name);
        const size_t pins = sym.pins.size();
        found.push_back({pads.size() == pins ? 0 : 1, upper(baseName(sym.footprint)) == upper(fp.name) ? 0 : 1,
                         filtered ? 0 : 1, pads.size() > pins ? pads.size() - pins : pins - pads.size(), fp.name});
    }
    std::sort(found.begin(), found.end(), [](const Scored& a, const Scored& b) {
        return std::tie(a.named, a.exactCount, a.filtered, a.padGap, a.name) <
               std::tie(b.named, b.exactCount, b.filtered, b.padGap, b.name);
    });
    std::vector<std::string> out;
    for (const auto& s : found) {
        if (out.size() >= 30) break;
        if (std::find(out.begin(), out.end(), s.name) == out.end()) out.push_back(s.name);
    }
    return out;
}

Json libraryImportToJson(const LibraryImport& result) {
    Json j = Json::object();
    Json parts = Json::array();
    std::map<std::string, const ImportedSymbol*> symbolsByName;  // KiCad / Altium symbols: their footprint can be chosen
    for (const auto& s : result.symbols) symbolsByName.emplace(s.name, &s);
    for (const auto& p : result.parts) {
        Json pj = Json::object();
        pj["name"] = p.spec.name;
        pj["symbol"] = p.symbolName;
        pj["footprint"] = p.footprintName;
        pj["source"] = p.source;
        pj["ok"] = p.ok;
        pj["error"] = p.error;
        Json w = Json::array();
        for (const auto& s : p.warnings) w.push(s);
        pj["warnings"] = w;
        pj["spec"] = customPartSpecToJson(p.spec);
        auto sym = p.symbolName.empty() ? symbolsByName.end() : symbolsByName.find(p.symbolName);
        if (sym != symbolsByName.end()) {
            pj["pairable"] = true;
            Json c = Json::array();
            for (const auto& name : footprintCandidates(result, *sym->second)) c.push(name);
            pj["candidates"] = c;
        }
        parts.push(pj);
    }
    j["parts"] = parts;
    Json list = Json::array();
    {
        std::set<std::string> eagleFiles;
        for (const auto& f : result.files)
            if (f.format == "eagle_lbr") eagleFiles.insert(f.name);
        for (const auto& fp : result.footprints) {
            if (eagleFiles.count(fp.source) || list.size() >= 2000) continue;
            Json fj = Json::object();
            fj["name"] = fp.name;
            fj["source"] = fp.source;
            fj["pads"] = static_cast<int>(fp.package.lands.size());
            list.push(fj);
        }
    }
    j["footprintList"] = list;
    Json files = Json::array();
    for (const auto& f : result.files) {
        Json fj = Json::object();
        fj["name"] = f.name;
        fj["format"] = f.format;
        fj["symbols"] = f.symbols;
        fj["footprints"] = f.footprints;
        fj["error"] = f.error;
        files.push(fj);
    }
    j["files"] = files;
    j["symbols"] = static_cast<int>(result.symbols.size());
    j["footprints"] = static_cast<int>(result.footprints.size());
    return j;
}

Json importLibraryRequest(const Json& request) {
    std::vector<ImportFile> files;
    std::map<std::string, std::string> pairs;
    for (const auto& f : request.get("files").items()) {
        if (!f.isObject()) continue;
        std::string content = f.get("content").asString("");
        if (f.get("contentBase64").isString()) {  // binary files (STL, Altium libraries)
            try {
                content = decodeBase64(f.get("contentBase64").asString());
            } catch (const std::exception&) {
                content.clear();
            }
        }
        files.push_back({f.get("name").asString(""), std::move(content)});
    }
    const Json& pj = request.get("pairs");
    if (pj.isObject())
        for (const auto& kv : pj.fields())
            if (kv.second.isString()) pairs[kv.first] = kv.second.asString();
    return libraryImportToJson(importLibraryFiles(files, pairs));
}

}  // namespace sieda
