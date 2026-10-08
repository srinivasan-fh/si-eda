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
        while (pos_ < s_.size() && (isdigit(static_cast<unsigned char>(s_[pos_])) || s_[pos_] == '.' ||
                                    s_[pos_] == 'e' || s_[pos_] == 'E' || s_[pos_] == '+' || s_[pos_] == '-'))
            ++pos_;
        std::string tok = s_.substr(start, pos_ - start);
        char* end = nullptr;
        double v = std::strtod(tok.c_str(), &end);
        if (end == tok.c_str() || *end != '\0') fail("invalid number '" + tok + "'");
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
            if (pos_ >= s_.size()) fail("unterminated string");
            char c = s_[pos_++];
            if (c == '"') break;
            if (c != '\\') {
                out += c;
                continue;
            }
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

void newline(std::string& out, bool pretty, int indent) {
    if (!pretty) return;
    out += '\n';
    out.append(static_cast<size_t>(indent) * 2, ' ');
}
}  // namespace

const std::string& Json::asString() const { return type_ == Type::String ? str_ : kEmptyString; }
const Json::Array& Json::items() const { return type_ == Type::Array && arr_ ? *arr_ : kEmptyArray; }
const Json::Object& Json::fields() const { return type_ == Type::Object && obj_ ? *obj_ : kEmptyObject; }

void Json::detach() {
    if (type_ == Type::Array && arr_ && arr_.use_count() > 1) arr_ = std::make_shared<Array>(*arr_);
    if (type_ == Type::Object && obj_ && obj_.use_count() > 1) obj_ = std::make_shared<Object>(*obj_);
}

void Json::push(Json v) {
    if (type_ == Type::Null) {
        type_ = Type::Array;
        arr_ = std::make_shared<Array>();
    }
    if (type_ != Type::Array) throw JsonError("push on non-array");
    detach();
    arr_->push_back(std::move(v));
}

size_t Json::size() const {
    if (type_ == Type::Array) return arr_ ? arr_->size() : 0;
    if (type_ == Type::Object) return obj_ ? obj_->size() : 0;
    return 0;
}

const Json& Json::operator[](size_t i) const {
    const Array& a = items();
    return i < a.size() ? a[i] : kNull;
}

Json& Json::operator[](const std::string& key) {
    if (type_ == Type::Null) {
        type_ = Type::Object;
        obj_ = std::make_shared<Object>();
    }
    if (type_ != Type::Object) throw JsonError("key access on non-object");
    detach();
    return (*obj_)[key];
}

const Json& Json::get(const std::string& key) const {
    const Object& o = fields();
    auto it = o.find(key);
    return it == o.end() ? kNull : it->second;
}

bool Json::has(const std::string& key) const { return fields().count(key) != 0; }

std::string Json::dump(bool pretty) const {
    std::string out;
    dumpTo(out, pretty, 0);
    return out;
}

void Json::dumpTo(std::string& out, bool pretty, int indent) const {
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += bool_ ? "true" : "false"; break;
        case Type::Number: {
            if (!std::isfinite(num_)) {
                out += "null";
            } else {
                // std::to_chars: the same text as printf ("%lld" / "%.10g") without locale or format parsing.
                char buf[40];
                const auto r = num_ == std::floor(num_) && std::fabs(num_) < 1e15
                                   ? std::to_chars(buf, buf + sizeof buf, static_cast<long long>(num_))
                                   : std::to_chars(buf, buf + sizeof buf, num_, std::chars_format::general, 10);
                out.append(buf, r.ptr);
            }
            break;
        }
        case Type::String: escapeString(out, str_); break;
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
