#include <algorithm>
#include <charconv>
#include "sieda/Json.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace sieda {

namespace {
const Json kNull;
const std::string kEmptyString;
const Json::Array kEmptyArray;
const Json::Object kEmptyObject;

class Parser {
public:
    explicit Parser(const std::string& s) : s_(s) {}

    Json parseDocument() {
        Json v = parseValue();
        skipWs();
        if (pos_ != s_.size()) fail("trailing characters");
        return v;
    }

private:
    [[noreturn]] void fail(const std::string& msg) const {
        throw JsonError("JSON parse error at offset " + std::to_string(pos_) + ": " + msg);
    }

    void skipWs() {
        while (pos_ < s_.size() && (s_[pos_] == ' ' || s_[pos_] == '\n' || s_[pos_] == '\r' || s_[pos_] == '\t'))
            ++pos_;
    }

    bool consume(char c) {
        skipWs();
        if (pos_ < s_.size() && s_[pos_] == c) {
            ++pos_;
            return true;
        }
        return false;
    }

    void expect(char c) {
        if (!consume(c)) fail(std::string("expected '") + c + "'");
    }

    Json parseValue() {
        skipWs();
        if (pos_ >= s_.size()) fail("unexpected end of input");
        char c = s_[pos_];
        if (c == '{' || c == '[') {
            // Nesting is bounded so a hostile document (a project file, an MCP or live-endpoint request) cannot
            // exhaust the stack; real SiEDA documents nest about ten levels.
            if (++depth_ > kMaxDepth) fail("nesting deeper than " + std::to_string(kMaxDepth) + " levels");
            Json v = c == '{' ? parseObject() : parseArray();
            --depth_;
            return v;
        }
        if (c == '"') return Json(parseString());
        if (c == 't') { literal("true"); return Json(true); }
        if (c == 'f') { literal("false"); return Json(false); }
        if (c == 'n') { literal("null"); return Json(); }
        if (c == '-' || (c >= '0' && c <= '9')) return parseNumber();
        fail(std::string("unexpected character '") + c + "'");
    }

    void literal(const char* word) {
        size_t n = std::char_traits<char>::length(word);
        if (s_.compare(pos_, n, word) != 0) fail(std::string("expected ") + word);
        pos_ += n;
    }

    Json parseNumber() {
        size_t start = pos_;
        if (s_[pos_] == '-') ++pos_;
        // Plain decimals with at most 15 significant digits and 22 decimals (every coordinate and value SiEDA writes)
        // are exact in a double, so one correctly rounded division gives strtod's result without its cost.
        {
            uint64_t m = 0;
            int digits = 0, frac = 0;
            bool dot = false;
            size_t q = pos_;
            for (; q < s_.size(); ++q) {
                const char c = s_[q];
                if (c >= '0' && c <= '9') {
                    if (++digits <= 15) m = m * 10 + static_cast<uint64_t>(c - '0');
                    if (dot) ++frac;
                } else if (c == '.' && !dot) {
                    dot = true;
                } else {
                    break;
                }
            }
            const bool ends =
                q >= s_.size() || (s_[q] != 'e' && s_[q] != 'E' && s_[q] != '.' && s_[q] != '+' && s_[q] != '-');
            if (digits >= 1 && digits <= 15 && frac <= 22 && (!dot || frac >= 1) && ends) {
                static const double pow10[] = {1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
                                               1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22};
                const double v = static_cast<double>(m) / pow10[frac];
                pos_ = q;
                return Json(s_[start] == '-' ? -v : v);
            }
        }
        while (pos_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        char* end = nullptr;
        double v = std::strtod(s_.c_str() + start, &end);  // the text is NUL-terminated; must stop where the scan did
        if (end != s_.c_str() + pos_) fail("invalid number '" + s_.substr(start, pos_ - start) + "'");
        return Json(v);
    }

    static void appendUtf8(std::string& out, uint32_t cp) {
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
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
    }

    uint32_t parseHex4() {
        if (pos_ + 4 > s_.size()) fail("truncated \\u escape");
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = s_[pos_++];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= static_cast<uint32_t>(c - '0');
            else if (c >= 'a' && c <= 'f') v |= static_cast<uint32_t>(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= static_cast<uint32_t>(c - 'A' + 10);
            else fail("invalid hex digit");
        }
        return v;
    }

    std::string parseString() {
        skipWs();
        if (s_[pos_] != '"') fail("expected string");
        ++pos_;
        std::string out;
        while (true) {
            size_t run = pos_;  // plain characters up to the next quote or escape go in one append
            while (run < s_.size() && s_[run] != '"' && s_[run] != '\\') ++run;
            out.append(s_, pos_, run - pos_);
            pos_ = run;
            if (pos_ >= s_.size()) fail("unterminated string");
            if (s_[pos_++] == '"') break;
            if (pos_ >= s_.size()) fail("unterminated escape");
            char e = s_[pos_++];
            switch (e) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    uint32_t cp = parseHex4();
                    if (cp >= 0xD800 && cp <= 0xDBFF && pos_ + 6 <= s_.size() && s_[pos_] == '\\' && s_[pos_ + 1] == 'u') {
                        pos_ += 2;
                        uint32_t lo = parseHex4();
                        cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(out, cp);
                    break;
                }
                default: fail("invalid escape");
            }
        }
        return out;
    }

    Json parseArray() {
        expect('[');
        Json::Array arr;
        if (consume(']')) return Json(std::move(arr));
        do {
            arr.push_back(parseValue());
        } while (consume(','));
        expect(']');
        return Json(std::move(arr));
    }

    Json parseObject() {
        expect('{');
        Json::Object obj;
        if (consume('}')) return Json(std::move(obj));
        do {
            skipWs();
            std::string key = parseString();
            expect(':');
            obj[key] = parseValue();
        } while (consume(','));
        expect('}');
        return Json(std::move(obj));
    }

    static constexpr int kMaxDepth = 256;
    int depth_ = 0;
    const std::string& s_;
    size_t pos_ = 0;
};

void escapeString(std::string& out, const std::string& s) {
    out += '"';
    // Fast path: most strings (names, designators, layer names) need no escaping.
    bool plain = true;
    for (unsigned char c : s)
        if (c < 0x20 || c == '"' || c == '\\') {
            plain = false;
            break;
        }
    if (plain) {
        out += s;
        out += '"';
        return;
    }
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof buf, "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

/// Writes x when it is a short decimal (coordinates, widths: at most 4, or 6, decimals and at most 10 significant
/// digits) as "%.10g" would, without the general-format conversion: 117.25 → "117.25". Any other value → false.
bool shortDecimal(std::string& out, double x) {
    const double a = std::fabs(x);
    if (!(a >= 1e-4 && a < 1e6)) return false;  // "%.10g" would use exponent form, or more than 10 digits
    for (const auto& [scale, limit] : {std::pair{1e4, 1e6}, std::pair{1e6, 1e4}}) {
        if (a >= limit) continue;
        const double k = std::nearbyint(a * scale);
        if (k / scale != a) continue;  // not exactly this decimal
        const long long n = static_cast<long long>(k), s = static_cast<long long>(scale);
        char buf[24];
        if (x < 0) out += '-';
        out.append(buf, std::to_chars(buf, buf + sizeof buf, n / s).ptr);
        out += '.';
        char frac[8];
        const auto r = std::to_chars(frac, frac + sizeof frac, n % s);
        const size_t digits = static_cast<size_t>(r.ptr - frac), width = scale == 1e4 ? 4 : 6;
        out.append(width - digits, '0');
        size_t end = digits;
        while (end > 0 && frac[end - 1] == '0') --end;
        out.append(frac, end);
        return true;
    }
    return false;
}

void newline(std::string& out, bool pretty, int indent) {
    if (!pretty) return;
    out += '\n';
    out.append(static_cast<size_t>(indent) * 2, ' ');
}
}  // namespace

Json::Object::const_iterator Json::Object::find(std::string_view key) const {
    auto it = std::lower_bound(items_.begin(), items_.end(), key,
                               [](const value_type& a, std::string_view k) { return a.first < k; });
    return it != items_.end() && it->first == key ? it : items_.end();
}

Json::Object::iterator Json::Object::find(std::string_view key) {
    auto it = std::lower_bound(items_.begin(), items_.end(), key,
                               [](const value_type& a, std::string_view k) { return a.first < k; });
    return it != items_.end() && it->first == key ? it : items_.end();
}

Json& Json::Object::operator[](std::string_view key) {
    // Keys usually arrive in order (our own files and builders), so the common case is an append; most objects
    // have a handful of keys, so one allocation holds them.
    if (items_.empty()) items_.reserve(8);
    if (items_.empty() || items_.back().first < key) {
        items_.emplace_back(std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
        return items_.back().second;
    }
    auto it = std::lower_bound(items_.begin(), items_.end(), key,
                               [](const value_type& a, std::string_view k) { return a.first < k; });
    if (it == items_.end() || it->first != key)
        it = items_.emplace(it, std::piecewise_construct, std::forward_as_tuple(key), std::forward_as_tuple());
    return it->second;
}

const std::string& Json::asString() const {
    const std::string* p = std::get_if<std::string>(&v_);
    return p ? *p : kEmptyString;
}

const Json::Array& Json::items() const {
    const auto* p = std::get_if<std::shared_ptr<Array>>(&v_);
    return p && *p ? **p : kEmptyArray;
}

const Json::Object& Json::fields() const {
    const auto* p = std::get_if<std::shared_ptr<Object>>(&v_);
    return p && *p ? **p : kEmptyObject;
}

void Json::detach() {
    if (auto* a = std::get_if<std::shared_ptr<Array>>(&v_); a && *a && a->use_count() > 1)
        *a = std::make_shared<Array>(**a);
    if (auto* o = std::get_if<std::shared_ptr<Object>>(&v_); o && *o && o->use_count() > 1)
        *o = std::make_shared<Object>(**o);
}

void Json::push(Json v) {
    if (isNull()) v_.emplace<std::shared_ptr<Array>>(std::make_shared<Array>());
    if (!isArray()) throw JsonError("push on non-array");
    detach();
    std::get<std::shared_ptr<Array>>(v_)->push_back(std::move(v));
}

void Json::reserve(size_t n) {
    if (isNull()) v_.emplace<std::shared_ptr<Array>>(std::make_shared<Array>());
    if (!isArray()) throw JsonError("reserve on non-array");
    detach();
    std::get<std::shared_ptr<Array>>(v_)->reserve(n);
}

size_t Json::size() const {
    if (isArray()) return items().size();
    if (isObject()) return fields().size();
    return 0;
}

const Json& Json::operator[](size_t i) const {
    const Array& a = items();
    return i < a.size() ? a[i] : kNull;
}

Json& Json::operator[](std::string_view key) {
    if (isNull()) v_.emplace<std::shared_ptr<Object>>(std::make_shared<Object>());
    if (!isObject()) throw JsonError("key access on non-object");
    detach();
    return (*std::get<std::shared_ptr<Object>>(v_))[key];
}

const Json& Json::get(std::string_view key) const {
    const Object& o = fields();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

bool Json::has(std::string_view key) const { return fields().count(key) != 0; }

std::string Json::dump(bool pretty) const {
    std::string out;
    dumpTo(out, pretty, 0);
    return out;
}

void Json::dumpTo(std::string& out, bool pretty, int indent) const {
    switch (type()) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(v_) ? "true" : "false"; break;
        case Type::Number: {
            const double num_ = std::get<double>(v_);
            if (!std::isfinite(num_)) {
                out += "null";
            } else {
                // std::to_chars: the same text as printf ("%lld" / "%.10g") without locale or format parsing.
                char buf[40];
                if (num_ == std::floor(num_) && std::fabs(num_) < 1e15) {
                    out.append(buf, std::to_chars(buf, buf + sizeof buf, static_cast<long long>(num_)).ptr);
                } else if (!shortDecimal(out, num_)) {
                    out.append(buf, std::to_chars(buf, buf + sizeof buf, num_, std::chars_format::general, 10).ptr);
                }
            }
            break;
        }
        case Type::String: escapeString(out, std::get<std::string>(v_)); break;
        case Type::Array: {
            const Array& a = items();
            out += '[';
            for (size_t i = 0; i < a.size(); ++i) {
                if (i) out += ',';
                newline(out, pretty, indent + 1);
                a[i].dumpTo(out, pretty, indent + 1);
            }
            if (!a.empty()) newline(out, pretty, indent);
            out += ']';
            break;
        }
        case Type::Object: {
            const Object& o = fields();
            out += '{';
            bool first = true;
            for (const auto& [k, v] : o) {
                if (!first) out += ',';
                first = false;
                newline(out, pretty, indent + 1);
                escapeString(out, k);
                out += pretty ? ": " : ":";
                v.dumpTo(out, pretty, indent + 1);
            }
            if (!o.empty()) newline(out, pretty, indent);
            out += '}';
            break;
        }
    }
}

Json Json::parse(const std::string& text) { return Parser(text).parseDocument(); }

}  // namespace sieda
