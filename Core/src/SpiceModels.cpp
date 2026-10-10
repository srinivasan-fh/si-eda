#include "sieda/SpiceModels.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <sstream>
#include <mutex>
#include <set>

namespace sieda {

namespace {
constexpr const char* kPortTag = kSpicePortTag;
constexpr size_t kMaxText = 8u << 20;   // 8 MB of model text
constexpr size_t kMaxLines = 200000;
constexpr int kMaxDepth = 40;           // subcircuit nesting
constexpr size_t kMaxPrims = 100000;    // flattened primitives
constexpr size_t kMaxInstances = 10000; // subcircuit instances (a self-calling subcircuit doubles per level)
constexpr int kMaxExprDepth = 200;      // expression nesting

std::string upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}
std::string trim(const std::string& s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace(static_cast<unsigned char>(s[a]))) ++a;
    while (b > a && std::isspace(static_cast<unsigned char>(s[b - 1]))) --b;
    return s.substr(a, b - a);
}
bool startsWith(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
bool isIdentStart(char c) { return std::isalpha(static_cast<unsigned char>(c)) || c == '_'; }
bool isIdentChar(char c) { return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '.' || c == '$'; }
}  // namespace

const char* spiceLevelName(SpiceDiagnostic::Level level) {
    switch (level) {
        case SpiceDiagnostic::Level::Info: return "info";
        case SpiceDiagnostic::Level::Warning: return "warning";
        case SpiceDiagnostic::Level::Error: return "error";
    }
    return "warning";
}

bool parseSpiceNumber(const std::string& text, double& value) {
    const std::string t = trim(text);
    size_t i = 0;
    if (i < t.size() && (t[i] == '+' || t[i] == '-')) ++i;
    const size_t digitsStart = i;
    bool digits = false;
    while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
        ++i;
        digits = true;
    }
    if (i < t.size() && t[i] == '.') {
        ++i;
        while (i < t.size() && std::isdigit(static_cast<unsigned char>(t[i]))) {
            ++i;
            digits = true;
        }
    }
    if (!digits || i == digitsStart) return false;
    if (i < t.size() && (t[i] == 'e' || t[i] == 'E')) {  // exponent only when digits follow
        size_t j = i + 1;
        if (j < t.size() && (t[j] == '+' || t[j] == '-')) ++j;
        if (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) {
            while (j < t.size() && std::isdigit(static_cast<unsigned char>(t[j]))) ++j;
            i = j;
        }
    }
    const std::string number = t.substr(0, i);
    char* end = nullptr;
    double v = std::strtod(number.c_str(), &end);
    if (!end || *end != '\0') return false;
    std::string suffix = upper(t.substr(i));
    double scale = 1;
    size_t used = 0;
    if (startsWith(suffix, "MEG")) {
        scale = 1e6;
        used = 3;
    } else if (startsWith(suffix, "MIL")) {
        scale = 25.4e-6;
        used = 3;
    } else if (startsWith(suffix, "\xC2\xB5")) {  // µ
        scale = 1e-6;
        used = 2;
    } else if (!suffix.empty()) {
        used = 1;
        switch (suffix[0]) {
            case 'T': scale = 1e12; break;
            case 'G': scale = 1e9; break;
            case 'K': scale = 1e3; break;
            case 'M': scale = 1e-3; break;
            case 'U': scale = 1e-6; break;
            case 'N': scale = 1e-9; break;
            case 'P': scale = 1e-12; break;
            case 'F': scale = 1e-15; break;
            case 'A': scale = 1e-18; break;
            default: used = 0; break;
        }
    }
    for (size_t k = used; k < suffix.size(); ++k) {  // unit letters ("pF", "kohm", "V", "Ω")
        unsigned char c = static_cast<unsigned char>(suffix[k]);
        if (!(std::isalpha(c) || c >= 0x80)) return false;
    }
    v *= scale;
    if (!std::isfinite(v)) return false;
    value = v;
    return true;
}

// ------------------------------------------------------------------ expressions

namespace {
SpiceExprPtr node(SpiceExpr::Op op, std::vector<SpiceExprPtr> args = {}) {
    auto e = std::make_shared<SpiceExpr>();
    e->op = op;
    e->args = std::move(args);
    return e;
}
SpiceExprPtr number(double v) {
    auto e = node(SpiceExpr::Op::Number);
    e->value = v;
    return e;
}

class ExprParser {
public:
    explicit ExprParser(const std::string& text) : s_(text) {}
    SpiceExprPtr parse(std::string& error) {
        SpiceExprPtr e = ternary();
        skip();
        if (e && i_ < s_.size()) fail("unexpected '" + s_.substr(i_, 12) + "'");
        if (!error_.empty()) {
            error = error_;
            return nullptr;
        }
        return e;
    }

private:
    const std::string& s_;
    size_t i_ = 0;
    int depth_ = 0;
    std::string error_;

    void fail(const std::string& m) {
        if (error_.empty()) error_ = m;
    }
    void skip() {
        while (i_ < s_.size() && std::isspace(static_cast<unsigned char>(s_[i_]))) ++i_;
    }
    bool eat(const char* tok) {
        skip();
        size_t n = std::char_traits<char>::length(tok);
        if (s_.compare(i_, n, tok) == 0) {
            i_ += n;
            return true;
        }
        return false;
    }
    bool peek(char c) {
        skip();
        return i_ < s_.size() && s_[i_] == c;
    }
    struct Guard {
        ExprParser& p;
        explicit Guard(ExprParser& q) : p(q) {
            if (++p.depth_ > kMaxExprDepth) p.fail("expression nested too deeply");
        }
        ~Guard() { --p.depth_; }
    };

    SpiceExprPtr ternary() {
        Guard g(*this);
        if (!error_.empty()) return nullptr;
        SpiceExprPtr c = logicalOr();
        if (!c) return nullptr;
        if (eat("?")) {
            SpiceExprPtr a = ternary();
            if (!eat(":")) {
                fail("expected ':' in a ? b : c");
                return nullptr;
            }
            SpiceExprPtr b = ternary();
            if (!a || !b) return nullptr;
            return node(SpiceExpr::Op::Cond, {c, a, b});
        }
        return c;
    }
    SpiceExprPtr logicalOr() {
        SpiceExprPtr a = logicalAnd();
        while (a && eat("||")) {
            SpiceExprPtr b = logicalAnd();
            if (!b) return nullptr;
            a = node(SpiceExpr::Op::Or, {a, b});
        }
        return a;
    }
    SpiceExprPtr logicalAnd() {
        SpiceExprPtr a = comparison();
        while (a && eat("&&")) {
            SpiceExprPtr b = comparison();
            if (!b) return nullptr;
            a = node(SpiceExpr::Op::And, {a, b});
        }
        return a;
    }
    SpiceExprPtr comparison() {
        SpiceExprPtr a = additive();
        while (a) {
            SpiceExpr::Op op;
            if (eat("<=")) op = SpiceExpr::Op::Le;
            else if (eat(">=")) op = SpiceExpr::Op::Ge;
            else if (eat("==")) op = SpiceExpr::Op::Eq;
            else if (eat("!=")) op = SpiceExpr::Op::Ne;
            else if (eat("<")) op = SpiceExpr::Op::Lt;
            else if (eat(">")) op = SpiceExpr::Op::Gt;
            else break;
            SpiceExprPtr b = additive();
            if (!b) return nullptr;
            a = node(op, {a, b});
        }
        return a;
    }
    SpiceExprPtr additive() {
        SpiceExprPtr a = multiplicative();
        while (a) {
            SpiceExpr::Op op;
            if (eat("+")) op = SpiceExpr::Op::Add;
            else if (eat("-")) op = SpiceExpr::Op::Sub;
            else break;
            SpiceExprPtr b = multiplicative();
            if (!b) return nullptr;
            a = node(op, {a, b});
        }
        return a;
    }
    SpiceExprPtr multiplicative() {
        SpiceExprPtr a = unary();
        while (a) {
            skip();
            SpiceExpr::Op op;
            if (i_ < s_.size() && s_[i_] == '*' && !(i_ + 1 < s_.size() && s_[i_ + 1] == '*')) {
                ++i_;
                op = SpiceExpr::Op::Mul;
            } else if (eat("/")) {
                op = SpiceExpr::Op::Div;
            } else {
                break;
            }
            SpiceExprPtr b = unary();
            if (!b) return nullptr;
            a = node(op, {a, b});
        }
        return a;
    }
    SpiceExprPtr unary() {
        Guard g(*this);
        if (!error_.empty()) return nullptr;
        if (eat("-")) {
            SpiceExprPtr a = unary();
            return a ? node(SpiceExpr::Op::Neg, {a}) : nullptr;
        }
        if (eat("+")) return unary();
        if (peek('!') && !(i_ + 1 < s_.size() && s_[i_ + 1] == '=')) {
            ++i_;
            SpiceExprPtr a = unary();
            return a ? node(SpiceExpr::Op::Not, {a}) : nullptr;
        }
        return power();
    }
    SpiceExprPtr power() {
        SpiceExprPtr a = primary();
        if (a && (eat("**") || eat("^"))) {
            SpiceExprPtr b = unary();  // right associative
            if (!b) return nullptr;
            return node(SpiceExpr::Op::Pow, {a, b});
        }
        return a;
    }
    SpiceExprPtr primary() {
        skip();
        if (i_ >= s_.size()) {
            fail("unexpected end of expression");
            return nullptr;
        }
        char c = s_[i_];
        if (c == '(' || c == '{') {
            const char close = c == '(' ? ')' : '}';
            ++i_;
            SpiceExprPtr e = ternary();
            skip();
            if (i_ >= s_.size() || s_[i_] != close) {
                fail(std::string("expected '") + close + "'");
                return nullptr;
            }
            ++i_;
            return e;
        }
        if (std::isdigit(static_cast<unsigned char>(c)) ||
            (c == '.' && i_ + 1 < s_.size() && std::isdigit(static_cast<unsigned char>(s_[i_ + 1])))) {
            size_t j = i_;
            while (j < s_.size() && (std::isdigit(static_cast<unsigned char>(s_[j])) || s_[j] == '.')) ++j;
            if (j < s_.size() && (s_[j] == 'e' || s_[j] == 'E')) {
                size_t k = j + 1;
                if (k < s_.size() && (s_[k] == '+' || s_[k] == '-')) ++k;
                if (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) {
                    while (k < s_.size() && std::isdigit(static_cast<unsigned char>(s_[k]))) ++k;
                    j = k;
                }
            }
            while (j < s_.size() && (std::isalpha(static_cast<unsigned char>(s_[j])) || static_cast<unsigned char>(s_[j]) >= 0x80))
                ++j;
            double v = 0;
            if (!parseSpiceNumber(s_.substr(i_, j - i_), v)) {
                fail("invalid number '" + s_.substr(i_, j - i_) + "'");
                return nullptr;
            }
            i_ = j;
            return number(v);
        }
        if (isIdentStart(c)) {
            size_t j = i_;
            while (j < s_.size() && isIdentChar(s_[j])) ++j;
            std::string name = upper(s_.substr(i_, j - i_));
            i_ = j;
            if (peek('(')) {
                if (name == "V" || name == "I") {  // V(a), V(a,b), I(Vx): raw node / source names
                    ++i_;
                    size_t close = s_.find(')', i_);
                    if (close == std::string::npos) {
                        fail("missing ')' after " + name + "(");
                        return nullptr;
                    }
                    std::string inside = s_.substr(i_, close - i_);
                    i_ = close + 1;
                    std::vector<std::string> parts;
                    size_t start = 0;
                    for (size_t k = 0; k <= inside.size(); ++k)
                        if (k == inside.size() || inside[k] == ',') {
                            parts.push_back(upper(trim(inside.substr(start, k - start))));
                            start = k + 1;
                        }
                    for (const auto& p : parts)
                        if (p.empty()) {
                            fail(name + "() needs a name");
                            return nullptr;
                        }
                    if (name == "V") {
                        if (parts.size() > 2) {
                            fail("V() takes one or two nodes");
                            return nullptr;
                        }
                        auto e = node(SpiceExpr::Op::Voltage);
                        e->name = parts[0];
                        e->name2 = parts.size() > 1 ? parts[1] : std::string("0");
                        return e;
                    }
                    if (parts.size() != 1) {
                        fail("I() takes one voltage source");
                        return nullptr;
                    }
                    auto e = node(SpiceExpr::Op::Current);
                    e->name = parts[0];
                    return e;
                }
                ++i_;
                auto call = node(SpiceExpr::Op::Call);
                call->name = name;
                if (!eat(")")) {
                    while (true) {
                        SpiceExprPtr a = ternary();
                        if (!a) return nullptr;
                        call->args.push_back(a);
                        if (call->args.size() > 512) {
                            fail("too many arguments");
                            return nullptr;
                        }
                        if (eat(",")) continue;
                        if (eat(")")) break;
                        fail("expected ',' or ')' in " + name + "()");
                        return nullptr;
                    }
                }
                return call;
            }
            auto e = node(SpiceExpr::Op::Param);
            e->name = name;
            return e;
        }
        fail(std::string("unexpected '") + c + "'");
        return nullptr;
    }
};

double limitedExp(double x) { return x < 80 ? std::exp(x) : std::exp(80.0) * (1.0 + x - 80.0); }

/// Built-in functions: name → (min args, max args).
const std::map<std::string, std::pair<int, int>>& builtinFunctions() {
    static const std::map<std::string, std::pair<int, int>> f = {
        {"ABS", {1, 1}},   {"SQRT", {1, 1}},  {"EXP", {1, 1}},   {"LN", {1, 1}},    {"LOG", {1, 1}},
        {"LOG10", {1, 1}}, {"SIN", {1, 1}},   {"COS", {1, 1}},   {"TAN", {1, 1}},   {"ASIN", {1, 1}},
        {"ACOS", {1, 1}},  {"ATAN", {1, 1}},  {"ATAN2", {2, 2}}, {"SINH", {1, 1}},  {"COSH", {1, 1}},
        {"TANH", {1, 1}},  {"MIN", {2, 64}},  {"MAX", {2, 64}},  {"POW", {2, 2}},   {"PWR", {2, 2}},
        {"PWRS", {2, 2}},  {"SGN", {1, 1}},   {"SIGN", {1, 2}},  {"U", {1, 1}},     {"URAMP", {1, 1}},
        {"LIMIT", {3, 3}}, {"IF", {3, 3}},    {"FLOOR", {1, 1}}, {"CEIL", {1, 1}},  {"ROUND", {1, 1}},
        {"INT", {1, 1}},   {"TABLE", {3, 513}}, {"$SWG", {5, 5}},
    };
    return f;
}

double callBuiltin(const std::string& n, const std::vector<double>& a) {
    auto at = [&](size_t i) { return i < a.size() ? a[i] : 0.0; };
    const double x = at(0);
    switch (n[0]) {
        case 'A':
            if (n == "ABS") return std::fabs(x);
            if (n == "ASIN") return std::asin(std::clamp(x, -1.0, 1.0));
            if (n == "ACOS") return std::acos(std::clamp(x, -1.0, 1.0));
            if (n == "ATAN") return std::atan(x);
            if (n == "ATAN2") return std::atan2(x, at(1));
            break;
        case 'C':
            if (n == "COS") return std::cos(x);
            if (n == "COSH") return std::cosh(std::clamp(x, -700.0, 700.0));
            if (n == "CEIL") return std::ceil(x);
            break;
        case 'E':
            if (n == "EXP") return limitedExp(x);
            break;
        case 'F':
            if (n == "FLOOR") return std::floor(x);
            break;
        case 'I':
            if (n == "IF") return x != 0 ? at(1) : at(2);
            if (n == "INT") return std::trunc(x);
            break;
        case 'L':
            if (n == "LN" || n == "LOG") return std::log(std::max(x, 1e-300));
            if (n == "LOG10") return std::log10(std::max(x, 1e-300));
            if (n == "LIMIT") {
                double lo = std::min(at(1), at(2)), hi = std::max(at(1), at(2));
                return std::clamp(x, lo, hi);
            }
            break;
        case 'M':
            if (n == "MIN") return *std::min_element(a.begin(), a.end());
            if (n == "MAX") return *std::max_element(a.begin(), a.end());
            break;
        case 'P':
            if (n == "POW") {
                if (x < 0 && std::floor(at(1)) != at(1)) return 0.0;
                return std::pow(x, at(1));
            }
            if (n == "PWR") return std::pow(std::fabs(x), at(1));
            if (n == "PWRS") return (x < 0 ? -1.0 : 1.0) * std::pow(std::fabs(x), at(1));
            break;
        case 'R':
            if (n == "ROUND") return std::round(x);
            break;
        case 'S':
            if (n == "SQRT") return x > 0 ? std::sqrt(x) : 0.0;
            if (n == "SIN") return std::sin(x);
            if (n == "SINH") return std::sinh(std::clamp(x, -700.0, 700.0));
            if (n == "SGN") return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
            if (n == "SIGN") {
                if (a.size() == 1) return x > 0 ? 1.0 : x < 0 ? -1.0 : 0.0;
                return at(1) >= 0 ? std::fabs(x) : -std::fabs(x);  // FORTRAN SIGN(a, b)
            }
            break;
        case 'T':
            if (n == "TAN") return std::tan(x);
            if (n == "TANH") return std::tanh(x);
            if (n == "TABLE") {  // TABLE(x, x1, y1, x2, y2, …): piecewise linear, clamped at the ends
                size_t pairs = (a.size() - 1) / 2;
                if (pairs == 0) return 0.0;
                if (x <= a[1]) return a[2];
                for (size_t k = 1; k < pairs; ++k) {
                    double x0 = a[2 * k - 1], y0 = a[2 * k], x1 = a[2 * k + 1], y1 = a[2 * k + 2];
                    if (x <= x1) return x1 > x0 ? y0 + (y1 - y0) * (x - x0) / (x1 - x0) : y1;
                }
                return a[2 * pairs];
            }
            break;
        case 'U':
            if (n == "U") return x > 0 ? 1.0 : 0.0;
            if (n == "URAMP") return x > 0 ? x : 0.0;
            break;
        case '$':
            if (n == "$SWG") {  // switch conductance: log-interpolated between goff and gon over [v0, v1], smooth
                double v0 = at(1), v1 = at(2), gon = at(3), goff = at(4);
                double s = v1 != v0 ? std::clamp((x - v0) / (v1 - v0), 0.0, 1.0) : (x >= v0 ? 1.0 : 0.0);
                s = s * s * (3 - 2 * s);
                return std::exp(std::log(goff) + (std::log(gon) - std::log(goff)) * s);
            }
            break;
        default: break;
    }
    return 0.0;
}
}  // namespace

SpiceExprPtr parseSpiceExpression(const std::string& text, std::string& error) {
    error.clear();
    if (text.size() > 100000) {
        error = "expression too long";
        return nullptr;
    }
    ExprParser p(text);
    return p.parse(error);
}

double evalSpiceExpression(const SpiceExpr& e, const double* c, double t) {
    using Op = SpiceExpr::Op;
    auto arg = [&](size_t i) { return evalSpiceExpression(*e.args[i], c, t); };
    switch (e.op) {
        case Op::Number: return e.value;
        case Op::Param: return 0.0;
        case Op::Voltage: return (e.slot >= 0 ? c[e.slot] : 0.0) - (e.slot2 >= 0 ? c[e.slot2] : 0.0);
        case Op::Current: return e.slot >= 0 ? c[e.slot] : 0.0;
        case Op::Time: return t;
        case Op::Neg: return -arg(0);
        case Op::Not: return arg(0) == 0 ? 1.0 : 0.0;
        case Op::Add: return arg(0) + arg(1);
        case Op::Sub: return arg(0) - arg(1);
        case Op::Mul: return arg(0) * arg(1);
        case Op::Div: {
            double b = arg(1);
            if (std::fabs(b) < 1e-300) b = b < 0 ? -1e-300 : 1e-300;
            return arg(0) / b;
        }
        case Op::Pow: {
            double a = arg(0), b = arg(1);
            if (a < 0 && std::floor(b) != b) return 0.0;
            return std::pow(a, b);
        }
        case Op::Lt: return arg(0) < arg(1) ? 1.0 : 0.0;
        case Op::Gt: return arg(0) > arg(1) ? 1.0 : 0.0;
        case Op::Le: return arg(0) <= arg(1) ? 1.0 : 0.0;
        case Op::Ge: return arg(0) >= arg(1) ? 1.0 : 0.0;
        case Op::Eq: return arg(0) == arg(1) ? 1.0 : 0.0;
        case Op::Ne: return arg(0) != arg(1) ? 1.0 : 0.0;
        case Op::And: return arg(0) != 0 && arg(1) != 0 ? 1.0 : 0.0;
        case Op::Or: return arg(0) != 0 || arg(1) != 0 ? 1.0 : 0.0;
        case Op::Cond: return arg(0) != 0 ? arg(1) : arg(2);
        case Op::Call: {
            if (e.name == "IF") return arg(0) != 0 ? arg(1) : arg(2);
            std::vector<double> a(e.args.size());
            for (size_t i = 0; i < a.size(); ++i) a[i] = arg(i);
            return callBuiltin(e.name, a);
        }
    }
    return 0.0;
}

std::vector<std::vector<int>> spicePolyTerms(int dims, size_t count) {
    std::vector<std::vector<int>> terms;
    if (dims < 1) return terms;
    for (int degree = 0; terms.size() < count && degree <= 64; ++degree) {
        std::vector<int> idx(static_cast<size_t>(degree), 0);
        while (true) {
            terms.push_back(idx);
            if (terms.size() >= count) break;
            // Next nondecreasing tuple in lexicographic order.
            int k = degree - 1;
            while (k >= 0 && idx[static_cast<size_t>(k)] == dims - 1) --k;
            if (k < 0) break;
            int v = idx[static_cast<size_t>(k)] + 1;
            for (int m = k; m < degree; ++m) idx[static_cast<size_t>(m)] = v;
        }
    }
    return terms;
}

// ------------------------------------------------------------------ library

namespace {
/// Splits a card into tokens: whitespace (and, with `parens`, "( ) ,") separate; "{…}" and '…' stay whole (quotes
/// become braces); "=" is its own token and is merged into "KEY=VALUE".
std::vector<std::string> splitCard(const std::string& text, bool parens) {
    std::vector<std::string> raw;
    std::string cur;
    auto flush = [&] {
        if (!cur.empty()) raw.push_back(cur);
        cur.clear();
    };
    for (size_t i = 0; i < text.size(); ++i) {
        char c = text[i];
        if (c == '{' || c == '\'') {
            const char close = c == '{' ? '}' : '\'';
            int depth = 0;
            std::string group = "{";
            size_t j = i + 1;
            for (; j < text.size(); ++j) {
                if (c == '{' && text[j] == '{') ++depth;
                if (text[j] == close) {
                    if (depth == 0) break;
                    --depth;
                }
                group += text[j];
            }
            group += "}";
            cur += group;
            i = j;
            continue;
        }
        if (std::isspace(static_cast<unsigned char>(c)) || (parens && (c == '(' || c == ')' || c == ','))) {
            flush();
            continue;
        }
        if (c == '=') {
            flush();
            raw.push_back("=");
            continue;
        }
        cur += c;
    }
    flush();
    std::vector<std::string> out;
    for (size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == "=" && !out.empty() && i + 1 < raw.size() && raw[i + 1] != "=") {
            out.back() += "=" + raw[i + 1];
            ++i;
        } else {
            out.push_back(raw[i]);
        }
    }
    return out;
}

bool splitAssign(const std::string& tok, std::string& key, std::string& val) {
    if (!tok.empty() && tok[0] == '{') return false;
    size_t eq = tok.find('=');
    if (eq == std::string::npos || eq == 0) return false;
    key = upper(tok.substr(0, eq));
    val = tok.substr(eq + 1);
    return true;
}

std::string stripBraces(std::string s) {
    s = trim(s);
    while (s.size() >= 2 && s.front() == '{' && s.back() == '}') s = trim(s.substr(1, s.size() - 2));
    return s;
}

const std::set<std::string>& ignoredDotCommands() {
    static const std::set<std::string> s = {
        ".OPTIONS", ".OPTION", ".OPT", ".TEMP", ".TRAN", ".AC", ".DC", ".OP", ".PROBE", ".SAVE", ".PRINT", ".PLOT",
        ".IC", ".NODESET", ".MEAS", ".MEASURE", ".STEP", ".TITLE", ".WIDTH", ".BACKANNO", ".FOUR", ".NOISE", ".TF",
        ".SENS", ".ALIASES", ".ENDALIASES", ".ALIAS", ".DISTRIBUTION", ".ENDL", ".GLOBAL", ".CONNECT", ".WAVE",
        ".STIMULUS", ".LOADBIAS", ".SAVEBIAS", ".WATCH", ".EXTERNAL", ".PARAMS", ".FAULTS", ".PZ", ".DISTO",
    };
    return s;
}
}  // namespace

const SpiceModelCard* SpiceLibrary::findModel(const std::string& rawName, int scope) const {
    const std::string name = upper(rawName);
    for (int s = scope;; s = subckts[static_cast<size_t>(s)].scope) {
        for (const auto& m : models)
            if (m.scope == s && m.name == name) return &m;
        if (s < 0) break;
        if (static_cast<size_t>(s) >= subckts.size()) break;
    }
    return nullptr;
}

int SpiceLibrary::findSubckt(const std::string& rawName, int scope) const {
    const std::string name = upper(rawName);
    for (int s = scope;; s = subckts[static_cast<size_t>(s)].scope) {
        for (size_t i = 0; i < subckts.size(); ++i)
            if (subckts[i].scope == s && subckts[i].name == name) return static_cast<int>(i);
        if (s < 0) break;
        if (static_cast<size_t>(s) >= subckts.size()) break;
    }
    return -1;
}

std::string inlineSpiceIncludes(const std::string& text, const std::string& dir) {
    size_t budget = kMaxText;
    std::set<std::string> seen;
    std::function<std::string(const std::string&, int)> expand = [&](const std::string& src, int depth) {
        std::istringstream in(src);
        std::string out, line;
        while (std::getline(in, line)) {
            std::istringstream ls(line);
            std::string kw, file, section;
            ls >> kw >> file >> section;
            kw = upper(kw);
            if (file.size() > 1 && (file.front() == '"' || file.front() == '\'')) file = file.substr(1, file.size() - 2);
            const bool inc = kw == ".INCLUDE" || kw == ".INC" || kw == ".LIB" || kw == ".LIBRARY";
            std::string body;
            if (inc && depth < 8 && !file.empty() && file[0] != '/' && file[0] != '\\' && file.find("..") == std::string::npos &&
                file.find(':') == std::string::npos && seen.insert(file + "|" + upper(section)).second) {
                std::ifstream f(dir + "/" + file, std::ios::binary);
                std::ostringstream buf;
                if (f && buf << f.rdbuf()) body = buf.str();
                if (!body.empty() && !section.empty()) {  // .lib file section: only that block
                    std::istringstream bs(body);
                    std::string l, block;
                    bool on = false;
                    while (std::getline(bs, l)) {
                        std::istringstream t(l);
                        std::string k, name;
                        t >> k >> name;
                        k = upper(k);
                        if (on && (k == ".ENDL" || k == ".LIB")) break;
                        if (on) block += l + "\n";
                        on = on || (k == ".LIB" && upper(name) == upper(section));
                    }
                    body = block;
                }
            }
            if (body.empty() || body.size() > budget) {
                out += line + "\n";
                continue;
            }
            budget -= body.size();
            out += "* " + line + " (inlined)\n" + expand(body, depth + 1) + "\n";
        }
        return out;
    };
    return expand(text, 0);
}

SpiceLibrary parseSpiceLibrary(const std::string& text) {
    SpiceLibrary lib;
    auto diag = [&](SpiceDiagnostic::Level level, int line, const std::string& m) {
        if (lib.diagnostics.size() < 500) lib.diagnostics.push_back({level, line, m});
    };
    using L = SpiceDiagnostic::Level;
    if (text.size() > kMaxText) {
        diag(L::Error, 0, "The model text is larger than 8 MB.");
        return lib;
    }
    // Physical lines → logical cards (comments removed, "+" continuations joined).
    struct Logical {
        int line;
        std::string text;
    };
    std::vector<Logical> cards;
    size_t lineCount = 0;
    size_t pos = 0;
    int lineNo = 0;
    bool inControl = false;
    while (pos <= text.size()) {
        size_t nl = text.find('\n', pos);
        std::string line = text.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        pos = nl == std::string::npos ? text.size() + 1 : nl + 1;
        ++lineNo;
        if (++lineCount > kMaxLines) {
            diag(L::Error, lineNo, "The model text has more than 200 000 lines.");
            return lib;
        }
        for (char& c : line)
            if (static_cast<unsigned char>(c) < 0x20 && c != '\t') c = ' ';  // \r, NUL and other control characters
        std::string t = trim(line);
        if (t.empty() || t[0] == '*') continue;
        if (inControl) {  // ngspice .control … .endc: simulator commands
            if (startsWith(upper(t), ".ENDC")) inControl = false;
            continue;
        }
        if (startsWith(upper(t), ".CONTROL")) {
            inControl = true;
            diag(L::Info, lineNo, ".control block ignored.");
            continue;
        }
        // Inline comments: ";" anywhere, "$" after whitespace (outside braces).
        int brace = 0;
        for (size_t i = 0; i < t.size(); ++i) {
            if (t[i] == '{') ++brace;
            else if (t[i] == '}') brace = std::max(0, brace - 1);
            else if (brace == 0 && (t[i] == ';' || (t[i] == '$' && i > 0 && std::isspace(static_cast<unsigned char>(t[i - 1]))))) {
                t = trim(t.substr(0, i));
                break;
            }
        }
        if (t.empty()) continue;
        if (t[0] == '+') {
            if (cards.empty()) {
                diag(L::Warning, lineNo, "Continuation line with nothing to continue.");
                continue;
            }
            cards.back().text += " " + t.substr(1);
            if (cards.back().text.size() > 1000000) {
                diag(L::Error, lineNo, "A card is longer than 1 MB.");
                return lib;
            }
            continue;
        }
        cards.push_back({lineNo, t});
    }

    std::vector<int> stack;  // open subckts
    int topLevelElements = 0, firstTopLine = 0;
    auto addBlock = [&](const std::string& t) {
        for (int s : stack) lib.subckts[static_cast<size_t>(s)].blockText.push_back(t);
    };
    for (const auto& card : cards) {
        const std::string& t = card.text;
        if (t[0] == '.') {
            auto tokens = splitCard(t, true);
            std::string kw = upper(tokens[0]);
            if (kw == ".SUBCKT" || kw == ".MACRO") {
                if (tokens.size() < 2) {
                    diag(L::Error, card.line, ".subckt without a name.");
                    continue;
                }
                SpiceSubckt s;
                s.displayName = tokens[1];
                s.name = upper(tokens[1]);
                s.line = card.line;
                s.scope = stack.empty() ? -1 : stack.back();
                bool params = false;
                for (size_t i = 2; i < tokens.size(); ++i) {
                    std::string tok = tokens[i], up = upper(tok);
                    if (startsWith(up, "PARAMS:")) {
                        params = true;
                        tok = tok.substr(7);
                        if (tok.empty()) continue;
                    }
                    std::string k, v;
                    if (splitAssign(tok, k, v)) {
                        s.params.push_back({k, v});
                        params = true;
                    } else if (params) {
                        diag(L::Warning, card.line, "Parameter '" + tok + "' of " + s.displayName + " has no value.");
                    } else {
                        s.ports.push_back(upper(tok));
                    }
                }
                std::set<std::string> seen;
                for (const auto& p : s.ports)
                    if (!seen.insert(p).second) diag(L::Warning, card.line, s.displayName + ": port " + p + " is listed twice.");
                if (lib.findSubckt(s.name, s.scope) >= 0 &&
                    lib.subckts[static_cast<size_t>(lib.findSubckt(s.name, s.scope))].scope == s.scope)
                    diag(L::Warning, card.line, "Subcircuit " + s.displayName + " is defined again; the first definition is used.");
                addBlock(t);
                s.blockText.push_back(t);
                lib.subckts.push_back(std::move(s));
                stack.push_back(static_cast<int>(lib.subckts.size() - 1));
            } else if (kw == ".ENDS" || kw == ".EOM") {
                if (stack.empty()) {
                    diag(L::Warning, card.line, ".ends without a .subckt.");
                    continue;
                }
                addBlock(t);
                SpiceSubckt& s = lib.subckts[static_cast<size_t>(stack.back())];
                if (tokens.size() > 1 && upper(tokens[1]) != s.name)
                    diag(L::Warning, card.line, ".ends " + tokens[1] + " closes subcircuit " + s.displayName + ".");
                s.closed = true;
                stack.pop_back();
            } else if (kw == ".MODEL") {
                auto mt = splitCard(t, true);
                if (mt.size() < 3) {
                    diag(L::Error, card.line, ".model needs a name and a type.");
                    continue;
                }
                SpiceModelCard m;
                m.displayName = mt[1];
                m.name = upper(mt[1]);
                m.line = card.line;
                m.scope = stack.empty() ? -1 : stack.back();
                m.text = t;
                size_t i = 2;
                std::string type = upper(mt[2]);
                if (startsWith(type, "AKO:")) {
                    m.ako = type.substr(4);
                    if (m.ako.empty() && mt.size() > 3) m.ako = upper(mt[++i]);
                    ++i;
                    type = i < mt.size() ? upper(mt[i]) : std::string();
                    if (type.find('=') != std::string::npos) type.clear();  // type comes from the base model
                    else ++i;
                } else {
                    ++i;
                }
                m.type = type;
                for (; i < mt.size(); ++i) {
                    std::string k, v;
                    if (splitAssign(mt[i], k, v)) {
                        if (startsWith(k, "DEV") || startsWith(k, "LOT")) continue;  // PSpice tolerances
                        m.params[k] = v;
                    } else if (upper(mt[i]) == "DEV" || upper(mt[i]) == "LOT" || startsWith(upper(mt[i]), "DEV/") ||
                               startsWith(upper(mt[i]), "LOT/") || mt[i].find('%') != std::string::npos) {
                        continue;
                    } else {
                        diag(L::Warning, card.line, "Model " + m.displayName + ": '" + mt[i] + "' is not a parameter.");
                    }
                }
                addBlock(t);
                lib.models.push_back(std::move(m));
            } else if (kw == ".PARAM") {
                if (!stack.empty()) {
                    lib.subckts[static_cast<size_t>(stack.back())].body.push_back({card.line, t});
                    addBlock(t);
                    continue;
                }
                bool any = false;
                for (size_t i = 1; i < tokens.size(); ++i) {
                    std::string k, v;
                    if (splitAssign(tokens[i], k, v)) {
                        lib.params.push_back({k, v});
                        any = true;
                    } else {
                        diag(L::Warning, card.line, ".param: '" + tokens[i] + "' has no value.");
                    }
                }
                if (any) lib.paramText.push_back(t);
            } else if (kw == ".FUNC") {
                // .func name(a, b) {body}   or   .func name(a,b) = {body}
                std::string rest = trim(t.substr(5));
                size_t open = rest.find('('), close = rest.find(')');
                if (open == std::string::npos || close == std::string::npos || close < open) {
                    diag(L::Error, card.line, ".func needs name(arguments) {expression}.");
                    continue;
                }
                SpiceFunc f;
                f.name = upper(trim(rest.substr(0, open)));
                std::string args = rest.substr(open + 1, close - open - 1);
                size_t start = 0;
                for (size_t k = 0; k <= args.size(); ++k)
                    if (k == args.size() || args[k] == ',') {
                        std::string a = upper(trim(args.substr(start, k - start)));
                        if (!a.empty()) f.args.push_back(a);
                        start = k + 1;
                    }
                std::string body = trim(rest.substr(close + 1));
                if (!body.empty() && body[0] == '=') body = trim(body.substr(1));
                f.body = stripBraces(body);
                f.line = card.line;
                f.text = t;
                if (f.name.empty() || f.body.empty() || !isIdentStart(f.name[0])) {
                    diag(L::Error, card.line, ".func needs name(arguments) {expression}.");
                    continue;
                }
                if (builtinFunctions().count(f.name)) {
                    diag(L::Warning, card.line, ".func " + f.name + " redefines a built-in function; ignored.");
                    continue;
                }
                lib.funcs.push_back(std::move(f));
            } else if (kw == ".END") {
                break;
            } else if (kw == ".INCLUDE" || kw == ".INC" || kw == ".LIB" || kw == ".LIBRARY") {
                if (tokens.size() >= 2 && (kw == ".INCLUDE" || kw == ".INC" || tokens.size() >= 3 ||
                                           tokens[1].find('.') != std::string::npos || tokens[1].find('"') != std::string::npos))
                    diag(L::Warning, card.line, kw + " " + tokens[1] +
                                                    " is not followed: paste the contents of that file into the model text.");
            } else if (ignoredDotCommands().count(kw)) {
                diag(L::Info, card.line, kw + " is a simulation command and is ignored.");
            } else {
                diag(L::Warning, card.line, "Unknown command " + tokens[0] + " ignored.");
            }
            continue;
        }
        if (!stack.empty()) {
            lib.subckts[static_cast<size_t>(stack.back())].body.push_back({card.line, t});
            addBlock(t);
        } else {
            if (topLevelElements++ == 0) firstTopLine = card.line;
        }
    }
    for (int s : stack)
        diag(L::Error, lib.subckts[static_cast<size_t>(s)].line,
             "Subcircuit " + lib.subckts[static_cast<size_t>(s)].displayName + " has no .ends.");
    if (topLevelElements > 0)
        diag(L::Info, firstTopLine,
             std::to_string(topLevelElements) + " element line(s) outside any .subckt are a test circuit, not a model, "
                                                "and are ignored.");
    // AKO models: the base model's type and parameters, overridden by the card's own.
    for (auto& m : lib.models) {
        if (m.ako.empty()) continue;
        std::set<std::string> visited{m.name};
        const SpiceModelCard* base = lib.findModel(m.ako, m.scope);
        std::map<std::string, std::string> merged;
        std::vector<const SpiceModelCard*> chain;
        while (base && visited.insert(base->name).second) {
            chain.push_back(base);
            base = base->ako.empty() ? nullptr : lib.findModel(base->ako, base->scope);
        }
        if (chain.empty()) {
            diag(L::Error, m.line, "Model " + m.displayName + ": AKO base model " + m.ako + " is not defined.");
            continue;
        }
        for (auto it = chain.rbegin(); it != chain.rend(); ++it)
            for (const auto& [k, v] : (*it)->params) merged[k] = v;
        for (const auto& [k, v] : m.params) merged[k] = v;
        if (m.type.empty()) m.type = chain.back()->type.empty() ? chain.front()->type : chain.back()->type;
        m.params = merged;
    }
    static const std::set<std::string> kTypes = {"D", "NPN", "PNP", "NMOS", "PMOS", "NJF", "PJF", "SW", "CSW",
                                                 "VSWITCH", "ISWITCH", "R", "C", "L", "RES", "CAP", "IND"};
    for (const auto& m : lib.models)
        if (!m.type.empty() && !kTypes.count(m.type))
            diag(L::Warning, m.line, "Model " + m.displayName + " has type " + m.type + ", which the simulator does not support.");
    return lib;
}

// ------------------------------------------------------------------ flattening

namespace {
struct Ctx {
    std::map<std::string, double> params;
    std::string prefix;
    std::map<std::string, std::string> ports;
    int scope = -1;
    int depth = 0;
};

// Model parameters the simulator uses, and ones it knowingly ignores without consequence at 27 °C (temperature
// coefficients, ratings, vendor metadata). Anything else gets a warning.
struct ParamSets {
    std::set<std::string> used, info;
};
const ParamSets& paramSets(const std::string& type) {
    static const std::set<std::string> commonInfo = {"TNOM", "EG", "XTI", "XTB", "MFG", "TYPE", "IAVE", "VPK", "IPK",
                                                     "IREV", "VREV", "VCEO", "ICRATING", "VDS", "RON", "QG", "BVJ",
                                                     "TEMP", "KIND", "IMAX", "VMAX_RATING", "DESC", "PART", "VOLTAGE",
                                                     "CURRENT", "POWER"};
    static const std::map<std::string, ParamSets> sets = {
        {"D", {{"IS", "N", "RS", "CJO", "CJ0", "CJ", "VJ", "PB", "M", "MJ", "TT", "FC", "BV", "VB", "IBV", "NBV", "KF",
                "AF", "LEVEL", "AREA"},
               commonInfo}},
        {"BJT", {{"IS", "BF", "BR", "NF", "NR", "VAF", "VA", "VAR", "VB", "IKF", "IK", "IKR", "ISE", "NE", "ISC", "NC",
                  "RB", "RC", "RE", "CJE", "VJE", "PE", "MJE", "ME", "CJC", "VJC", "PC", "MJC", "MC", "TF", "TR", "FC",
                  "KF", "AF", "LEVEL"},
                 commonInfo}},
        {"MOS", {{"LEVEL", "VTO", "VT0", "KP", "GAMMA", "PHI", "LAMBDA", "THETA", "RD", "RS", "CBD", "CBS", "IS", "PB",
                  "CGSO", "CGDO", "CGBO", "CJ", "MJ", "CJSW", "MJSW", "FC", "TOX", "UO", "U0", "LD", "KF", "AF", "JS",
                  "L", "W"},
                 commonInfo}},
        {"JFET", {{"VTO", "VT0", "BETA", "LAMBDA", "IS", "N", "RD", "RS", "CGS", "CGD", "PB", "M", "FC", "KF", "AF",
                   "LEVEL"},
                  commonInfo}},
        {"SW", {{"VT", "VH", "RON", "ROFF", "VON", "VOFF", "IT", "IH", "ION", "IOFF", "LEVEL"}, commonInfo}},
    };
    static const ParamSets none;
    std::string key = type == "D" ? "D"
                      : (type == "NPN" || type == "PNP") ? "BJT"
                      : (type == "NMOS" || type == "PMOS") ? "MOS"
                      : (type == "NJF" || type == "PJF") ? "JFET"
                      : (type == "SW" || type == "CSW" || type == "VSWITCH" || type == "ISWITCH") ? "SW"
                                                                                                 : "";
    auto it = sets.find(key);
    return it == sets.end() ? none : it->second;
}

bool isTemperatureParam(const std::string& k) {
    if (startsWith(k, "T_") || startsWith(k, "TC") || startsWith(k, "TRS") || startsWith(k, "TBV") || startsWith(k, "TIKF") ||
        startsWith(k, "TRB") || startsWith(k, "TRC") || startsWith(k, "TRE") || startsWith(k, "TRD") || startsWith(k, "TCV"))
        return true;
    return k.size() >= 2 && k[0] == 'T' && std::isdigit(static_cast<unsigned char>(k.back()));
}

class Flattener {
public:
    Flattener(const SpiceLibrary& lib, SpiceFlatCircuit& out) : lib_(lib), out_(out) {
        for (const auto& f : lib.funcs)
            if (!funcs_.count(f.name)) funcs_[f.name] = &f;
        for (const auto& [k, v] : lib.params) {
            double val = 0;
            if (evalValue(global_, v, val, 0, "parameter " + k)) global_.params[k] = val;
        }
    }

    std::set<int> usedSubckts;
    std::set<const SpiceModelCard*> usedModels;
    size_t instances_ = 0;  // subcircuit instances so far (at most kMaxInstances)

    bool run(const std::string& rawName) {
        const std::string name = upper(trim(rawName));
        if (const SpiceModelCard* m = lib_.findModel(name, -1)) {
            useModel(m);
            out_.kind = "model";
            out_.type = m->type;
            out_.name = m->displayName;
            SpicePrimitive p;
            p.name = m->displayName;
            std::vector<std::string> roles;
            if (m->type == "D") {
                p.type = 'D';
                roles = {"A", "K"};
            } else if (m->type == "NPN" || m->type == "PNP") {
                p.type = 'Q';
                roles = {"C", "B", "E"};
            } else if (m->type == "NMOS" || m->type == "PMOS") {
                p.type = 'M';
                roles = {"D", "G", "S", "B"};
                p.w = p.l = 100e-6;  // SPICE default device size: KP·W/L = KP
            } else if (m->type == "NJF" || m->type == "PJF") {
                p.type = 'J';
                roles = {"D", "G", "S"};
            } else {
                error(m->line, "Model " + m->displayName + " (" + m->type +
                                   ") is not a device a part can use: choose a diode, transistor or subcircuit.");
                return false;
            }
            for (size_t i = 0; i < roles.size(); ++i) p.nodes.push_back(std::string(kPortTag) + std::to_string(i));
            out_.ports = roles;
            if (!loadModel(p, *m, global_, m->line)) return false;
            if (p.type == 'M') {  // W / L given on the model card (some vendors do) size the device
                if (auto it = p.model.find("L"); it != p.model.end() && it->second > 0) p.l = it->second;
                if (auto it = p.model.find("W"); it != p.model.end() && it->second > 0) p.w = it->second;
            }
            out_.prims.push_back(std::move(p));
            return finish();
        }
        int si = lib_.findSubckt(name, -1);
        if (si < 0) {
            error(0, "No .model or .subckt named " + trim(rawName) + " in the model text.");
            return false;
        }
        const SpiceSubckt& s = lib_.subckts[static_cast<size_t>(si)];
        out_.kind = "subckt";
        out_.type = "SUBCKT";
        out_.name = s.displayName;
        out_.ports = s.ports;
        std::vector<std::string> nodes;
        for (size_t i = 0; i < s.ports.size(); ++i) nodes.push_back(std::string(kPortTag) + std::to_string(i));
        instantiate(si, global_, "", nodes, {}, s.line);
        return finish();
    }

private:
    const SpiceLibrary& lib_;
    SpiceFlatCircuit& out_;
    Ctx global_;
    std::map<std::string, const SpiceFunc*> funcs_;
    std::set<std::string> warnedModels_;
    bool tooMany_ = false;

    void diag(SpiceDiagnostic::Level l, int line, const std::string& m) {
        for (const auto& d : out_.diagnostics)
            if (d.message == m) return;
        if (out_.diagnostics.size() < 300) out_.diagnostics.push_back({l, line, m});
    }
    void error(int line, const std::string& m) { diag(SpiceDiagnostic::Level::Error, line, m); }
    void warn(int line, const std::string& m) { diag(SpiceDiagnostic::Level::Warning, line, m); }
    void info(int line, const std::string& m) { diag(SpiceDiagnostic::Level::Info, line, m); }

    bool finish() {
        // Every controlling source and coupled inductor must exist.
        std::set<std::string> vs, ls;
        for (const auto& p : out_.prims) {
            if (p.type == 'V') vs.insert(p.name);
            if (p.type == 'L') ls.insert(p.name);
        }
        std::function<void(const SpiceExpr&, const std::string&)> checkExpr = [&](const SpiceExpr& e, const std::string& who) {
            if (e.op == SpiceExpr::Op::Current && !vs.count(e.name))
                error(0, who + ": I(" + e.name + ") needs a voltage source named " + e.name + ".");
            for (const auto& a : e.args) checkExpr(*a, who);
        };
        for (const auto& p : out_.prims) {
            if (p.type == 'K') {
                for (const auto& l : p.controlSources)
                    if (!ls.count(l)) error(0, p.name + ": coupled inductor " + l + " does not exist.");
            } else {
                for (const auto& v : p.controlSources)
                    if (!vs.count(v)) error(0, p.name + ": controlling source " + v + " does not exist.");
            }
            if (p.expr) checkExpr(*p.expr, p.name);
        }
        out_.ok = true;
        for (const auto& d : out_.diagnostics)
            if (d.level == SpiceDiagnostic::Level::Error) out_.ok = false;
        if (out_.ok && out_.prims.empty()) {
            error(0, out_.name + " contains no elements.");
            out_.ok = false;
        }
        return out_.ok;
    }

    void useModel(const SpiceModelCard* m) {
        usedModels.insert(m);
        std::set<std::string> seen;
        while (m && !m->ako.empty() && seen.insert(m->name).second) {
            m = lib_.findModel(m->ako, m->scope);
            if (m) usedModels.insert(m);
        }
    }

    std::string nodeName(const Ctx& c, const std::string& raw) const {
        std::string n = upper(raw);
        if (n == "0" || n == "GND" || n == "GND!") return "0";
        auto it = c.ports.find(n);
        if (it != c.ports.end()) return it->second;
        return c.prefix + n;
    }

    /// Replaces parameters by their values, renames nodes and sources into the flattened namespace and expands
    /// .func calls. `runtime` allows V(), I() and time (behavioural sources).
    SpiceExprPtr substitute(const SpiceExpr& e, const Ctx& c, bool runtime, const std::map<std::string, SpiceExprPtr>* bind,
                            int depth, std::string& err) {
        if (depth > kMaxExprDepth) {
            err = "expression nested too deeply (recursive .func?)";
            return nullptr;
        }
        using Op = SpiceExpr::Op;
        switch (e.op) {
            case Op::Number: return number(e.value);
            case Op::Param: {
                if (bind) {
                    auto it = bind->find(e.name);
                    if (it != bind->end()) return it->second;
                }
                auto it = c.params.find(e.name);
                if (it != c.params.end()) return number(it->second);
                if (e.name == "PI") return number(3.14159265358979323846);
                if (e.name == "TEMP") return number(27.0);
                if (e.name == "TIME" || e.name == "T") {
                    if (!runtime) {
                        err = "time is only defined in behavioural sources";
                        return nullptr;
                    }
                    return node(Op::Time);
                }
                err = "unknown parameter " + e.name;
                return nullptr;
            }
            case Op::Voltage:
            case Op::Current: {
                if (!runtime) {
                    err = "a value cannot depend on node voltages or currents";
                    return nullptr;
                }
                auto r = std::make_shared<SpiceExpr>(e);
                if (e.op == Op::Voltage) {
                    r->name = nodeName(c, e.name);
                    r->name2 = nodeName(c, e.name2);
                } else {
                    r->name = c.prefix + e.name;
                }
                return r;
            }
            case Op::Time:
                if (!runtime) {
                    err = "time is only defined in behavioural sources";
                    return nullptr;
                }
                return node(Op::Time);
            default: break;
        }
        std::vector<SpiceExprPtr> args;
        for (const auto& a : e.args) {
            SpiceExprPtr s = substitute(*a, c, runtime, bind, depth + 1, err);
            if (!s) return nullptr;
            args.push_back(s);
        }
        if (e.op == Op::Call) {
            auto fit = funcs_.find(e.name);
            if (fit != funcs_.end() && !builtinFunctions().count(e.name)) {
                const SpiceFunc& f = *fit->second;
                if (f.args.size() != args.size()) {
                    err = "function " + f.name + " takes " + std::to_string(f.args.size()) + " argument(s)";
                    return nullptr;
                }
                std::string perr;
                SpiceExprPtr body = parseSpiceExpression(f.body, perr);
                if (!body) {
                    err = ".func " + f.name + ": " + perr;
                    return nullptr;
                }
                std::map<std::string, SpiceExprPtr> b;
                for (size_t i = 0; i < args.size(); ++i) b[f.args[i]] = args[i];
                return substitute(*body, global_, runtime, &b, depth + 1, err);
            }
            auto bit = builtinFunctions().find(e.name);
            if (bit == builtinFunctions().end()) {
                if (e.name == "DDT" || e.name == "IDT" || e.name == "SDT" || e.name == "DELAY")
                    err = "function " + e.name + "() (time derivative / integral / delay) is not supported";
                else
                    err = "unknown function " + e.name + "()";
                return nullptr;
            }
            if (static_cast<int>(args.size()) < bit->second.first || static_cast<int>(args.size()) > bit->second.second) {
                err = "wrong number of arguments to " + e.name + "()";
                return nullptr;
            }
        }
        auto r = std::make_shared<SpiceExpr>();
        r->op = e.op;
        r->name = e.name;
        r->args = std::move(args);
        // Constant folding.
        bool constant = true;
        for (const auto& a : r->args) constant = constant && a->op == Op::Number;
        if (constant) return number(evalSpiceExpression(*r, nullptr, 0));
        return r;
    }

    bool evalExpr(const Ctx& c, const std::string& text, double& v, std::string& err) {
        SpiceExprPtr e = parseSpiceExpression(text, err);
        if (!e) return false;
        SpiceExprPtr s = substitute(*e, c, false, nullptr, 0, err);
        if (!s) return false;
        if (s->op != SpiceExpr::Op::Number) {
            err = "not a constant";
            return false;
        }
        v = s->value;
        if (!std::isfinite(v)) {
            err = "the value is not finite";
            return false;
        }
        return true;
    }

    /// A value token: a number with SPICE scale factor, a {expression} or a parameter name.
    bool evalValue(const Ctx& c, const std::string& tok, double& v, int line, const std::string& what) {
        std::string t = trim(tok);
        if (!t.empty() && t[0] != '{' && parseSpiceNumber(t, v)) return true;
        std::string err;
        if (evalExpr(c, stripBraces(t), v, err)) return true;
        error(line, what + ": invalid value '" + t + "' (" + err + ").");
        return false;
    }

    bool loadModel(SpicePrimitive& p, const SpiceModelCard& m, const Ctx& c, int line) {
        p.modelType = m.type;
        p.modelName = m.displayName;
        const ParamSets& sets = paramSets(m.type);
        std::vector<std::string> ignored, unsupported;
        for (const auto& [k, v] : m.params) {
            double val = 0;
            if (!evalValue(c, v, val, m.line, "Model " + m.displayName + " " + k)) return false;
            p.model[k] = val;
            if (sets.used.count(k)) continue;
            if (sets.info.count(k) || isTemperatureParam(k)) ignored.push_back(k);
            else unsupported.push_back(k);
        }
        auto lv = p.model.find("LEVEL");
        p.level = lv == p.model.end() ? 1 : std::fabs(lv->second) < 1e6 ? static_cast<int>(lv->second) : 0;  // no overflow on a huge LEVEL
        if (warnedModels_.insert(m.name).second) {
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
                return s;
            };
            if (!unsupported.empty())
                warn(m.line, "Model " + m.displayName + ": " + join(unsupported) + " not modelled (ignored).");
            if (!ignored.empty())
                info(m.line, "Model " + m.displayName + ": " + join(ignored) +
                                 " (temperature, rating or vendor data) ignored; the simulation is at 27 °C.");
            if ((m.type == "NMOS" || m.type == "PMOS")) {
                if (p.level == 3)
                    info(m.line, "Model " + m.displayName +
                                     ": LEVEL=3 is simulated with the level-1 equations plus THETA mobility reduction "
                                     "(ETA, VMAX, KAPPA, NFS ignored).");
                else if (p.level == 2)
                    warn(m.line, "Model " + m.displayName + ": LEVEL=2 is simulated with the level-1 equations.");
            }
        }
        if ((m.type == "NMOS" || m.type == "PMOS") && (p.level < 1 || p.level > 3)) {
            error(line, "Model " + m.displayName + ": MOSFET LEVEL=" + std::to_string(p.level) +
                            " (BSIM / EKV) is not supported; levels 1–3 are.");
            return false;
        }
        if ((m.type == "NPN" || m.type == "PNP") && p.level != 1) {
            error(line, "Model " + m.displayName + ": BJT LEVEL=" + std::to_string(p.level) +
                            " (VBIC / HICUM / MEXTRAM) is not supported; Gummel–Poon (level 1) is.");
            return false;
        }
        return true;
    }

    const SpiceModelCard* model(const Ctx& c, const std::string& name) {
        const SpiceModelCard* m = lib_.findModel(name, c.scope);
        if (m) useModel(m);
        return m;
    }

    bool addPrim(SpicePrimitive p) {
        if (out_.prims.size() >= kMaxPrims) {
            if (!tooMany_) error(0, "The model flattens to more than 100 000 elements.");
            tooMany_ = true;
            return false;
        }
        out_.prims.push_back(std::move(p));
        return true;
    }

    void instantiate(int si, const Ctx& parent, const std::string& instName, const std::vector<std::string>& nodes,
                     const std::vector<std::pair<std::string, std::string>>& instParams, int line) {
        const SpiceSubckt& s = lib_.subckts[static_cast<size_t>(si)];
        if (parent.depth >= kMaxDepth) {
            error(line, "Subcircuits nested more than 40 deep (is " + s.displayName + " recursive?).");
            return;
        }
        if (++instances_ > kMaxInstances) {
            if (instances_ == kMaxInstances + 1)
                error(line, "More than 10000 subcircuit instances (is " + s.displayName + " recursive?).");
            return;
        }
        if (!s.closed) {
            error(s.line, "Subcircuit " + s.displayName + " has no .ends.");
            return;
        }
        if (s.scope < 0) usedSubckts.insert(si);
        if (nodes.size() != s.ports.size()) {
            error(line, instName + ": " + s.displayName + " has " + std::to_string(s.ports.size()) + " ports, " +
                            std::to_string(nodes.size()) + " nodes given.");
            return;
        }
        Ctx c;
        c.params = global_.params;
        c.prefix = parent.prefix + (instName.empty() ? std::string() : upper(instName) + ".");
        c.scope = si;
        c.depth = parent.depth + 1;
        for (size_t i = 0; i < nodes.size(); ++i) c.ports[s.ports[i]] = nodes[i];
        std::set<std::string> overridden;
        for (const auto& [k, v] : instParams) {
            double val = 0;
            if (!evalValue(parent, v, val, line, instName + " parameter " + k)) return;
            c.params[k] = val;
            overridden.insert(k);
            bool known = false;
            for (const auto& d : s.params) known = known || d.first == k;
            if (!known) warn(line, instName + ": " + s.displayName + " has no parameter " + k + ".");
        }
        for (const auto& [k, v] : s.params) {
            if (overridden.count(k)) continue;
            double val = 0;
            if (!evalValue(c, v, val, s.line, s.displayName + " parameter " + k)) return;
            c.params[k] = val;
        }
        for (const auto& card : s.body)
            if (startsWith(upper(card.text), ".PARAM")) {
                for (const auto& tok : splitCard(card.text, true)) {
                    std::string k, v;
                    if (!splitAssign(tok, k, v)) continue;
                    double val = 0;
                    if (evalValue(c, v, val, card.line, "parameter " + k)) c.params[k] = val;
                }
            }
        for (const auto& card : s.body) {
            if (card.text[0] == '.') continue;
            if (tooMany_) return;
            element(card, c);
        }
    }

    void element(const SpiceCard& card, Ctx& c) {
        const int line = card.line;
        std::vector<std::string> tok = splitCard(card.text, true);
        if (tok.empty()) return;
        const std::string name = upper(tok[0]);
        const char type = name[0];
        SpicePrimitive p;
        p.type = type;
        p.name = c.prefix + name;
        auto need = [&](size_t n) {
            if (tok.size() < n) {
                error(line, name + ": too few fields.");
                return false;
            }
            return true;
        };
        auto nd = [&](size_t i) { return nodeName(c, tok[i]); };
        switch (type) {
            case 'R':
            case 'C':
            case 'L': {
                if (!need(4)) return;
                p.nodes = {nd(1), nd(2)};
                bool have = false;
                double mult = 1;
                for (size_t i = 3; i < tok.size(); ++i) {
                    std::string k, v;
                    if (splitAssign(tok[i], k, v)) {
                        if (k == std::string(1, type) || k == "VALUE") {
                            if (!evalValue(c, v, p.value, line, name)) return;
                            have = true;
                        } else if (k == "M") {
                            if (!evalValue(c, v, mult, line, name)) return;
                        } else if (k == "RSER" || k == "RPAR" || k == "CPAR" || k == "LSER") {
                            warn(line, name + ": " + k + " is not modelled (ignored).");
                        }
                        continue;
                    }
                    if (!have) {
                        if (upper(tok[i]) == "R" || upper(tok[i]) == "C" || upper(tok[i]) == "L") continue;
                        if (!evalValue(c, tok[i], p.value, line, name)) return;
                        have = true;
                    }
                }
                if (!have) {
                    error(line, name + ": no value.");
                    return;
                }
                if (!(mult > 0)) mult = 1;
                if (type == 'R') {
                    if (p.value == 0) {
                        error(line, name + ": zero resistance (use a 0 V source for a short).");
                        return;
                    }
                    p.value /= mult;
                } else {
                    p.value *= type == 'C' ? mult : 1.0 / mult;
                    if (p.value == 0) {
                        if (type == 'C') return;  // C = 0: nothing
                        error(line, name + ": zero inductance.");
                        return;
                    }
                }
                addPrim(std::move(p));
                return;
            }
            case 'K': {
                if (!need(4)) return;
                if (!evalValue(c, tok.back(), p.value, line, name)) return;
                if (!(std::fabs(p.value) <= 1.0)) {
                    error(line, name + ": coupling must be between -1 and 1.");
                    return;
                }
                std::vector<std::string> inductors;
                for (size_t i = 1; i + 1 < tok.size(); ++i) inductors.push_back(c.prefix + upper(tok[i]));
                if (inductors.size() < 2) {
                    error(line, name + ": needs two inductors.");
                    return;
                }
                for (size_t a = 0; a < inductors.size(); ++a)
                    for (size_t b = a + 1; b < inductors.size(); ++b) {
                        SpicePrimitive k = p;
                        k.controlSources = {inductors[a], inductors[b]};
                        if (!addPrim(std::move(k))) return;
                    }
                return;
            }
            case 'V':
            case 'I': {
                if (!need(3)) return;
                p.nodes = {nd(1), nd(2)};
                bool have = false, waveform = false;
                for (size_t i = 3; i < tok.size(); ++i) {
                    std::string u = upper(tok[i]);
                    if (u == "DC") {
                        if (i + 1 < tok.size()) {
                            if (!evalValue(c, tok[i + 1], p.value, line, name)) return;
                            have = true;
                            ++i;
                        }
                        continue;
                    }
                    if (u == "AC") {  // AC mag [phase]: not a DC value
                        double tmp = 0;
                        if (i + 1 < tok.size() && parseSpiceNumber(tok[i + 1], tmp)) ++i;
                        if (i + 1 < tok.size() && parseSpiceNumber(tok[i + 1], tmp)) ++i;
                        continue;
                    }
                    if (u == "SIN" || u == "PULSE" || u == "PWL" || u == "EXP" || u == "SFFM" || u == "AM") {
                        // The waveform's numbers (until AC / DC or the end), evaluated here.
                        std::vector<double> args;
                        size_t j = i + 1;
                        for (; j < tok.size(); ++j) {
                            const std::string uj = upper(tok[j]);
                            std::string k, v;
                            if (uj == "AC" || uj == "DC" || splitAssign(tok[j], k, v)) break;
                            double x = 0;
                            if (!evalValue(c, tok[j], x, line, name + " " + u)) return;
                            args.push_back(x);
                        }
                        auto num = [](double x) {
                            char b[40];
                            std::snprintf(b, sizeof b, "%.17g", x);
                            return std::string(b);
                        };
                        std::string w;
                        if (u == "PULSE" && args.size() >= 2 && args.size() <= 7) {
                            // SPICE defaults: no delay, ideal edges, one pulse that lasts to the end.
                            const double defaults[7] = {0, 0, 0, 0, 0, 1e300, 0};
                            while (args.size() < 7) args.push_back(defaults[args.size()]);
                        }
                        const bool usable = (u == "PULSE" && args.size() == 7) || (u == "SIN" && args.size() >= 3 && args.size() <= 6) ||
                                            (u == "PWL" && args.size() >= 2 && args.size() % 2 == 0) ||
                                            (u == "EXP" && (args.size() == 4 || args.size() == 6));
                        if (usable) {
                            w = u + "(";
                            for (size_t k = 0; k < args.size(); ++k) w += (k ? " " : "") + num(args[k]);
                            w += ")";
                            p.waveform = w;
                            if (!have) {
                                p.value = u == "SIN" ? args[0] : u == "PWL" ? args[1] : args[0];
                                have = true;
                            }
                        } else {
                            waveform = true;
                            if (!have && !args.empty()) {
                                p.value = args[0];
                                have = true;
                            }
                        }
                        i = j - 1;
                        continue;
                    }
                    std::string k, v;
                    if (splitAssign(tok[i], k, v)) continue;
                    if (!have) {
                        if (!evalValue(c, tok[i], p.value, line, name)) return;
                        have = true;
                    }
                }
                if (waveform)
                    warn(line, name + ": its waveform (SFFM, AM or incomplete arguments) is not simulated; it is held at its DC value.");
                addPrim(std::move(p));
                return;
            }
            case 'E':
            case 'G':
            case 'F':
            case 'H': {
                if (!need(4)) return;
                p.nodes = {nd(1), nd(2)};
                p.voltageOutput = type == 'E' || type == 'H';
                const bool currentControlled = type == 'F' || type == 'H';
                const std::string kw = upper(tok[3]);
                // Keyword forms read the original text: VALUE = {expr}, TABLE {expr} = (x,y) …
                const std::string up = upper(card.text);
                if (!currentControlled && (startsWith(kw, "VALUE") || startsWith(kw, "TABLE"))) {
                    size_t at = up.find(startsWith(kw, "VALUE") ? "VALUE" : "TABLE");
                    std::string rest = trim(card.text.substr(at + 5));
                    if (startsWith(kw, "VALUE")) {
                        if (!rest.empty() && rest[0] == '=') rest = trim(rest.substr(1));
                        p.expr = runtimeExpr(c, stripBraces(rest), line, name);
                        if (!p.expr) return;
                        addPrim(std::move(p));
                        return;
                    }
                    // TABLE {expr} = (x1, y1) (x2, y2) …
                    if (rest.empty() || rest[0] != '{') {
                        error(line, name + ": TABLE needs {expression} = (x, y) pairs.");
                        return;
                    }
                    int depth = 0;
                    size_t close = std::string::npos;
                    for (size_t i = 0; i < rest.size(); ++i) {
                        if (rest[i] == '{') ++depth;
                        if (rest[i] == '}' && --depth == 0) {
                            close = i;
                            break;
                        }
                    }
                    if (close == std::string::npos) {
                        error(line, name + ": unbalanced braces in TABLE.");
                        return;
                    }
                    p.expr = runtimeExpr(c, rest.substr(1, close - 1), line, name);
                    if (!p.expr) return;
                    std::vector<std::string> pts = splitCard(rest.substr(close + 1), true);
                    std::vector<double> nums;
                    for (const auto& t : pts) {
                        if (t == "=") continue;
                        std::string tt = t[0] == '=' ? t.substr(1) : t;
                        if (tt.empty()) continue;
                        double v = 0;
                        if (!evalValue(c, tt, v, line, name + " TABLE")) return;
                        nums.push_back(v);
                    }
                    if (nums.size() < 2 || nums.size() % 2) {
                        error(line, name + ": TABLE needs (x, y) pairs.");
                        return;
                    }
                    for (size_t i = 0; i < nums.size(); i += 2) p.table.push_back({nums[i], nums[i + 1]});
                    for (size_t i = 1; i < p.table.size(); ++i)
                        if (p.table[i].first < p.table[i - 1].first) {
                            error(line, name + ": TABLE inputs must ascend.");
                            return;
                        }
                    addPrim(std::move(p));
                    return;
                }
                if (startsWith(kw, "LAPLACE") || startsWith(kw, "FREQ") || startsWith(kw, "CHEBYSHEV")) {
                    error(line, name + ": " + kw.substr(0, kw.find('=')) +
                                    " sources (frequency-domain transfer functions) are not supported.");
                    return;
                }
                if (kw == "POLY") {
                    if (!need(5)) return;
                    double dimsV = 0;
                    if (!parseSpiceNumber(tok[4], dimsV) || dimsV < 1 || dimsV > 20 || std::floor(dimsV) != dimsV) {
                        error(line, name + ": POLY(n) needs n from 1 to 20.");
                        return;
                    }
                    const size_t dims = static_cast<size_t>(dimsV);
                    size_t i = 5;
                    const size_t perControl = currentControlled ? 1 : 2;
                    if (tok.size() < i + dims * perControl) {
                        error(line, name + ": POLY(" + std::to_string(dims) + ") needs its controls.");
                        return;
                    }
                    for (size_t k = 0; k < dims; ++k) {
                        if (currentControlled) {
                            p.controlSources.push_back(c.prefix + upper(tok[i++]));
                        } else {
                            p.controlNodes.push_back({nd(i), nd(i + 1)});
                            i += 2;
                        }
                    }
                    for (; i < tok.size(); ++i) {
                        double v = 0;
                        if (!evalValue(c, tok[i], v, line, name + " coefficient")) return;
                        p.coeffs.push_back(v);
                    }
                    if (p.coeffs.empty()) {
                        error(line, name + ": POLY needs coefficients.");
                        return;
                    }
                    if (dims == 1 && p.coeffs.size() == 1) p.coeffs = {0.0, p.coeffs[0]};  // SPICE: one coefficient is the gain
                    if (p.coeffs.size() > 4096) {
                        error(line, name + ": too many POLY coefficients.");
                        return;
                    }
                    p.poly = true;
                    addPrim(std::move(p));
                    return;
                }
                if (currentControlled) {
                    if (!need(5)) return;
                    p.controlSources.push_back(c.prefix + upper(tok[3]));
                    if (!evalValue(c, tok[4], p.value, line, name)) return;
                } else {
                    if (!need(6)) return;
                    p.controlNodes.push_back({nd(3), nd(4)});
                    if (!evalValue(c, tok[5], p.value, line, name)) return;
                }
                p.coeffs = {0.0, p.value};
                addPrim(std::move(p));
                return;
            }
            case 'B': {
                std::vector<std::string> t = splitCard(card.text, false);
                if (t.size() < 4) {
                    error(line, name + ": B source needs nodes and V= or I=.");
                    return;
                }
                p.nodes = {nodeName(c, t[1]), nodeName(c, t[2])};
                std::string kv = t[3];
                std::string k, v;
                if (!splitAssign(kv, k, v) || (k != "V" && k != "I")) {
                    error(line, name + ": B source needs V= or I= (" + (k == "R" ? "behavioural resistors" : kv) +
                                    " not supported).");
                    return;
                }
                // The expression is the rest of the original text after the first '='.
                size_t eq = card.text.find('=');
                std::string body = card.text.substr(eq + 1);
                p.voltageOutput = k == "V";
                p.expr = runtimeExpr(c, stripBraces(body), line, name);
                if (!p.expr) return;
                addPrim(std::move(p));
                return;
            }
            case 'D': {
                if (!need(4)) return;
                p.nodes = {nd(1), nd(2)};
                const SpiceModelCard* m = model(c, tok[3]);
                if (!m || m->type != "D") {
                    error(line, name + ": diode model " + tok[3] + (m ? " is not a D model." : " is not defined."));
                    return;
                }
                if (!loadModel(p, *m, c, line) || !instanceParams(p, tok, 4, c, line, name)) return;
                addPrim(std::move(p));
                return;
            }
            case 'Q': {
                if (!need(5)) return;
                size_t mi = 4;
                const SpiceModelCard* m = model(c, tok[4]);
                if (!m && tok.size() > 5) {
                    m = model(c, tok[5]);
                    mi = 5;
                    if (m) warn(line, name + ": the substrate node is not modelled.");
                }
                if (!m || (m->type != "NPN" && m->type != "PNP")) {
                    error(line, name + ": BJT model " + tok[mi] + (m ? " is not an NPN or PNP model." : " is not defined."));
                    return;
                }
                p.nodes = {nd(1), nd(2), nd(3)};
                if (!loadModel(p, *m, c, line) || !instanceParams(p, tok, mi + 1, c, line, name)) return;
                addPrim(std::move(p));
                return;
            }
            case 'M': {
                if (!need(5)) return;
                size_t mi = 5;
                const SpiceModelCard* m = tok.size() > 5 ? model(c, tok[5]) : nullptr;
                if (!m) {
                    m = model(c, tok[4]);
                    mi = 4;
                }
                if (!m || (m->type != "NMOS" && m->type != "PMOS")) {
                    error(line, name + ": MOSFET model " + tok[mi] + (m ? " is not an NMOS or PMOS model." : " is not defined."));
                    return;
                }
                p.nodes = {nd(1), nd(2), nd(3), mi == 5 ? nd(4) : nd(3)};
                p.w = p.l = 100e-6;
                if (!loadModel(p, *m, c, line)) return;
                if (auto it = p.model.find("L"); it != p.model.end() && it->second > 0) p.l = it->second;
                if (auto it = p.model.find("W"); it != p.model.end() && it->second > 0) p.w = it->second;
                if (!instanceParams(p, tok, mi + 1, c, line, name)) return;
                addPrim(std::move(p));
                return;
            }
            case 'J': {
                if (!need(5)) return;
                p.nodes = {nd(1), nd(2), nd(3)};
                const SpiceModelCard* m = model(c, tok[4]);
                if (!m || (m->type != "NJF" && m->type != "PJF")) {
                    error(line, name + ": JFET model " + tok[4] + (m ? " is not an NJF or PJF model." : " is not defined."));
                    return;
                }
                if (!loadModel(p, *m, c, line) || !instanceParams(p, tok, 5, c, line, name)) return;
                addPrim(std::move(p));
                return;
            }
            case 'S':
            case 'W': {
                // Switches become a behavioural current: I = V(n+, n−)·g(control), g log-interpolated between
                // 1/ROFF and 1/RON across the threshold (no hysteresis).
                const bool vc = type == 'S';
                if (!need(vc ? 6 : 5)) return;
                const std::string mname = vc ? tok[5] : tok[4];
                const SpiceModelCard* m = model(c, mname);
                if (!m || (vc ? (m->type != "SW" && m->type != "VSWITCH") : (m->type != "CSW" && m->type != "ISWITCH"))) {
                    error(line, name + ": switch model " + mname + (m ? " has the wrong type." : " is not defined."));
                    return;
                }
                SpicePrimitive tmp;
                if (!loadModel(tmp, *m, c, line)) return;
                auto get = [&](const char* k, double d) {
                    auto it = tmp.model.find(k);
                    return it == tmp.model.end() ? d : it->second;
                };
                double ron = std::max(get("RON", 1.0), 1e-9), roff = std::max(get("ROFF", 1e12), 1e-9);
                double v0, v1;
                if (m->type == "VSWITCH") {
                    v0 = get("VOFF", 0.0);
                    v1 = get("VON", 1.0);
                } else if (m->type == "ISWITCH") {
                    v0 = get("IOFF", 0.0);
                    v1 = get("ION", 1e-3);
                } else {
                    double vt = get(vc ? "VT" : "IT", 0.0), vh = std::fabs(get(vc ? "VH" : "IH", 0.0));
                    double width = std::max(vh, vc ? 0.01 : 1e-5);
                    v0 = vt - width;
                    v1 = vt + width;
                    if (vh > 0) warn(line, name + ": switch hysteresis is not modelled; it switches across VT ± VH.");
                }
                auto ctl = vc ? std::make_shared<SpiceExpr>() : std::make_shared<SpiceExpr>();
                if (vc) {
                    ctl->op = SpiceExpr::Op::Voltage;
                    ctl->name = nd(3);
                    ctl->name2 = nd(4);
                } else {
                    ctl->op = SpiceExpr::Op::Current;
                    ctl->name = c.prefix + upper(tok[3]);
                }
                auto across = std::make_shared<SpiceExpr>();
                across->op = SpiceExpr::Op::Voltage;
                across->name = nd(1);
                across->name2 = nd(2);
                auto g = node(SpiceExpr::Op::Call, {ctl, number(v0), number(v1), number(1.0 / ron), number(1.0 / roff)});
                g->name = "$SWG";
                p.type = 'B';
                p.nodes = {nd(1), nd(2)};
                p.voltageOutput = false;
                p.expr = node(SpiceExpr::Op::Mul, {across, g});
                addPrim(std::move(p));
                return;
            }
            case 'X': {
                // X n1 n2 … subckt [PARAMS:] k=v …
                size_t nameIdx = 0;
                std::vector<std::pair<std::string, std::string>> params;
                for (size_t i = 1; i < tok.size(); ++i) {
                    std::string k, v, u = upper(tok[i]);
                    if (startsWith(u, "PARAMS:")) {
                        std::string rest = tok[i].substr(7);
                        if (splitAssign(rest, k, v)) params.push_back({k, v});
                        continue;
                    }
                    if (splitAssign(tok[i], k, v)) {
                        params.push_back({k, v});
                        continue;
                    }
                    if (params.empty()) nameIdx = i;
                }
                if (nameIdx < 1) {
                    error(line, name + ": no subcircuit name.");
                    return;
                }
                int si = lib_.findSubckt(tok[nameIdx], c.scope);
                if (si < 0) {
                    error(line, name + ": subcircuit " + tok[nameIdx] + " is not defined.");
                    return;
                }
                std::vector<std::string> nodes;
                for (size_t i = 1; i < nameIdx; ++i) nodes.push_back(nd(i));
                instantiate(si, c, name, nodes, params, line);
                return;
            }
            default: {
                static const std::map<char, const char*> kinds = {
                    {'T', "transmission line"}, {'O', "lossy transmission line"}, {'U', "digital primitive"},
                    {'A', "XSPICE code model"}, {'N', "digital / XSPICE element"}, {'Z', "MESFET / HFET"},
                    {'Y', "transmission line"}, {'P', "coupled line"}};
                auto it = kinds.find(type);
                error(line, name + ": " + (it != kinds.end() ? std::string(it->second) : std::string("unknown element")) +
                                " is not supported.");
                return;
            }
        }
    }

    bool instanceParams(SpicePrimitive& p, const std::vector<std::string>& tok, size_t from, const Ctx& c, int line,
                        const std::string& name) {
        bool areaSet = false;
        for (size_t i = from; i < tok.size(); ++i) {
            std::string k, v, u = upper(tok[i]);
            if (u == "OFF" || u == "ON") continue;
            if (splitAssign(tok[i], k, v)) {
                double val = 0;
                if (k == "IC" || k == "TEMP" || k == "DTEMP" || k == "NRD" || k == "NRS" || k == "NRG" || k == "NRB" ||
                    k == "SCALE" || k == "OFF")
                    continue;
                if (!evalValue(c, v, val, line, name + " " + k)) return false;
                if (k == "AREA") p.area = val;
                else if (k == "M") p.multiplier = val;
                else if (k == "W") p.w = val;
                else if (k == "L") p.l = val;
                else if (k == "AD") p.ad = val;
                else if (k == "AS") p.as = val;
                else if (k == "PD") p.pd = val;
                else if (k == "PS") p.ps = val;
                else warn(line, name + ": instance parameter " + k + " ignored.");
                continue;
            }
            if (!areaSet) {
                double val = 0;
                if (!evalValue(c, tok[i], val, line, name + " area")) return false;
                p.area = val;
                areaSet = true;
            }
        }
        if (!(p.area > 0) || !(p.multiplier > 0) || (p.type == 'M' && (!(p.w > 0) || !(p.l > 0)))) {
            error(line, name + ": area, M, W and L must be positive.");
            return false;
        }
        return true;
    }

    SpiceExprPtr runtimeExpr(const Ctx& c, const std::string& text, int line, const std::string& name) {
        std::string err;
        SpiceExprPtr e = parseSpiceExpression(text, err);
        if (!e) {
            error(line, name + ": " + err + " in '" + text.substr(0, 80) + "'.");
            return nullptr;
        }
        SpiceExprPtr s = substitute(*e, c, true, nullptr, 0, err);
        if (!s) {
            error(line, name + ": " + err + ".");
            return nullptr;
        }
        return s;
    }
};
}  // namespace

SpiceFlatCircuit flattenSpiceModel(const SpiceLibrary& library, const std::string& name) {
    SpiceFlatCircuit out;
    Flattener f(library, out);
    f.run(name);
    return out;
}

std::vector<SpiceLibraryEntry> spiceLibraryEntries(const SpiceLibrary& library) {
    std::vector<SpiceLibraryEntry> out;
    for (const auto& s : library.subckts)
        if (s.scope < 0) out.push_back({s.displayName, "subckt", "SUBCKT", s.ports, s.line});
    static const std::map<std::string, std::vector<std::string>> roles = {
        {"D", {"A", "K"}},           {"NPN", {"C", "B", "E"}}, {"PNP", {"C", "B", "E"}}, {"NMOS", {"D", "G", "S", "B"}},
        {"PMOS", {"D", "G", "S", "B"}}, {"NJF", {"D", "G", "S"}}, {"PJF", {"D", "G", "S"}}};
    for (const auto& m : library.models) {
        if (m.scope >= 0) continue;
        auto it = roles.find(m.type);
        if (it == roles.end()) continue;
        out.push_back({m.displayName, "model", m.type, it->second, m.line});
    }
    std::stable_sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return a.line < b.line; });
    return out;
}

std::string extractSpiceModel(const SpiceLibrary& library, const std::string& name) {
    SpiceFlatCircuit flat;
    Flattener f(library, flat);
    f.run(name);
    const std::string up = upper(trim(name));
    const SpiceModelCard* top = library.findModel(up, -1);
    const int topSub = top ? -1 : library.findSubckt(up, -1);
    if (!top && topSub < 0) return std::string();
    std::string out;
    for (const auto& t : library.paramText) out += t + "\n";
    for (const auto& fn : library.funcs) out += fn.text + "\n";
    for (const auto& m : library.models)
        if (m.scope < 0 && f.usedModels.count(&m) && &m != top) out += m.text + "\n";
    for (size_t i = 0; i < library.subckts.size(); ++i) {
        if (!f.usedSubckts.count(static_cast<int>(i)) || static_cast<int>(i) == topSub) continue;
        for (const auto& t : library.subckts[i].blockText) out += t + "\n";
    }
    if (top) {
        out += top->text + "\n";
    } else {
        for (const auto& t : library.subckts[static_cast<size_t>(topSub)].blockText) out += t + "\n";
    }
    return out;
}

namespace {
std::string normPin(const std::string& s) {
    std::string u = upper(trim(s));
    std::string out;
    for (char c : u)
        if (c != '_' && c != ' ') out += c;
    return out;
}
// Op-amp port classes: 0 IN+, 1 IN−, 2 OUT, 3 positive rail, 4 negative rail, −1 unknown.
int opAmpClass(const std::string& port) {
    const std::string p = normPin(port);
    static const std::set<std::string> inp = {"IN+", "+IN", "INP", "NONINV", "NINV", "INPLUS", "VIN+", "+", "IP", "PLUS", "NI"};
    static const std::set<std::string> inn = {"IN-", "-IN", "INN", "INV", "INM", "INMINUS", "VIN-", "-", "IM", "MINUS"};
    static const std::set<std::string> out = {"OUT", "VOUT", "OUTPUT", "VO", "O"};
    static const std::set<std::string> pos = {"V+", "VCC", "VDD", "VS+", "+V", "VPOS", "VP", "+VS", "VSP", "POS", "V+S", "VCC+"};
    static const std::set<std::string> neg = {"V-", "VEE", "VSS", "VS-", "-V", "VNEG", "VN", "-VS", "VSN", "NEG", "GND", "V-S"};
    if (inp.count(p)) return 0;
    if (inn.count(p)) return 1;
    if (out.count(p)) return 2;
    if (pos.count(p)) return 3;
    if (neg.count(p)) return 4;
    return -1;
}
}  // namespace

std::string defaultSpicePinMap(const SpiceFlatCircuit& flat, const std::vector<std::pair<std::string, std::string>>& pins,
                               bool isOpAmpSymbol) {
    auto token = [&](size_t i) { return pins[i].first.empty() ? pins[i].second : pins[i].first; };
    auto join = [](const std::vector<std::string>& v) {
        std::string s;
        for (const auto& x : v) s += (s.empty() ? "" : " ") + x;
        return s;
    };
    // Aliases of device terminal roles.
    static const std::map<std::string, std::set<std::string>> alias = {
        {"A", {"A", "ANODE", "AN", "+"}},     {"K", {"K", "CATHODE", "KA", "CA", "-"}},
        {"C", {"C", "COLLECTOR", "COL"}},     {"B", {"B", "BASE"}},
        {"E", {"E", "EMITTER", "EM"}},        {"D", {"D", "DRAIN"}},
        {"G", {"G", "GATE"}},                 {"S", {"S", "SOURCE"}},
    };
    auto findPin = [&](const std::string& port, bool useAlias) -> int {
        const std::string p = normPin(port);
        for (size_t i = 0; i < pins.size(); ++i)
            if (normPin(pins[i].second) == p) return static_cast<int>(i);
        if (!useAlias) return -1;
        auto it = alias.find(p);
        if (it == alias.end()) return -1;
        for (size_t i = 0; i < pins.size(); ++i)
            if (it->second.count(normPin(pins[i].second))) return static_cast<int>(i);
        return -1;
    };
    const bool device = flat.kind == "model";
    std::vector<std::string> map;
    bool all = true;
    std::set<int> used;
    for (size_t k = 0; k < flat.ports.size(); ++k) {
        const std::string& port = flat.ports[k];
        int i = findPin(port, device);
        if (i < 0 && device && port == "B" && (flat.type == "NMOS" || flat.type == "PMOS")) {
            // Bulk: a pin named B / BULK / SUB, else the source.
            for (size_t j = 0; j < pins.size() && i < 0; ++j) {
                std::string n = normPin(pins[j].second);
                if (n == "BULK" || n == "SUB" || n == "B") i = static_cast<int>(j);
            }
            if (i < 0) i = findPin("S", true);
        }
        if (i < 0 || (used.count(i) && !(device && port == "B"))) {
            all = false;
            break;
        }
        used.insert(i);
        map.push_back(token(static_cast<size_t>(i)));
    }
    if (all && !map.empty()) return join(map);
    if (!device && isOpAmpSymbol && pins.size() >= 3) {
        // IN+, IN−, OUT pins of the symbol; rails ±15 V.
        std::vector<int> cls;
        std::set<int> seen;
        bool ok = true;
        for (const auto& p : flat.ports) {
            int c = opAmpClass(p);
            cls.push_back(c);
            ok = ok && c >= 0 && seen.insert(c).second;
        }
        if (!ok && flat.ports.size() == 5) cls = {0, 1, 3, 4, 2};  // the common order: IN+ IN− V+ V− OUT
        if (ok || flat.ports.size() == 5) {
            std::vector<std::string> m;
            for (int c : cls) {
                switch (c) {
                    case 0: m.push_back(token(0)); break;
                    case 1: m.push_back(token(1)); break;
                    case 2: m.push_back(token(2)); break;
                    case 3: m.push_back("dc:15"); break;
                    case 4: m.push_back("dc:-15"); break;
                    default: m.push_back("nc"); break;
                }
            }
            return join(m);
        }
    }
    if (flat.ports.size() == pins.size() || (device && flat.ports.size() == 4 && pins.size() == 3 && flat.type.find("MOS") != std::string::npos)) {
        if (device) return std::string();  // devices need named pins: a positional guess would swap terminals
        std::vector<std::string> m;
        for (size_t i = 0; i < flat.ports.size(); ++i) m.push_back(token(i));
        return join(m);
    }
    return std::string();
}

std::shared_ptr<const SpiceFlatCircuit> cachedSpiceFlatten(const std::string& text, const std::string& name) {
    static std::mutex mutex;
    static std::map<std::string, std::shared_ptr<const SpiceFlatCircuit>> cache;
    std::string key = upper(name) + '\x1f' + text;
    {
        std::lock_guard<std::mutex> lock(mutex);
        auto it = cache.find(key);
        if (it != cache.end()) return it->second;
    }
    auto flat = std::make_shared<SpiceFlatCircuit>(flattenSpiceModel(parseSpiceLibrary(text), name));
    std::lock_guard<std::mutex> lock(mutex);
    if (cache.size() >= 32) cache.clear();
    cache[key] = flat;
    return flat;
}

const std::vector<SpiceBuiltinModel>& builtinSpiceModels() {
    static const std::vector<SpiceBuiltinModel> models = {
        {"1N4148", "Small-signal switching diode: 4 pF, 20 ns recovery, 100 V",
         ".model 1N4148 D(IS=2.52n RS=0.568 N=1.752 CJO=4p M=0.4 VJ=0.7 TT=20n BV=100 IBV=100u)\n"},
        {"1N4007", "1 A 1000 V rectifier: 10 pF, slow recovery",
         ".model 1N4007 D(IS=7n RS=0.034 N=1.9 CJO=10p M=0.5 VJ=0.7 TT=4u BV=1000 IBV=5u)\n"},
        {"1N5819", "1 A 40 V Schottky: 110 pF, no stored charge",
         ".model 1N5819 D(IS=3u RS=0.05 N=1.1 CJO=110p M=0.35 VJ=0.35 TT=0 BV=40 IBV=1m)\n"},
        {"BZX84C5V1", "5.1 V Zener (breakdown at 5 mA)",
         ".model BZX84C5V1 D(IS=1f N=1 RS=1 CJO=100p M=0.33 VJ=0.75 BV=5.1 IBV=5m NBV=1.5)\n"},
        {"2N3904", "40 V 200 mA NPN (Gummel–Poon)",
         ".model 2N3904 NPN(IS=6.734f BF=416.4 NF=1 VAF=74.03 IKF=66.78m ISE=6.734f NE=1.259 BR=0.7371 NR=1 "
         "RB=10 RC=1 CJE=4.493p VJE=0.75 MJE=0.2593 CJC=3.638p VJC=0.75 MJC=0.3085 FC=0.5 TF=301.2p TR=239.5n)\n"},
        {"2N3906", "40 V 200 mA PNP (Gummel–Poon)",
         ".model 2N3906 PNP(IS=1.41f BF=180.7 NF=1 VAF=18.7 IKF=80m ISE=0 NE=1.5 BR=4.977 NR=1 RB=10 RC=2.5 "
         "CJE=8.063p VJE=0.75 MJE=0.3677 CJC=9.728p VJC=0.75 MJC=0.5776 FC=0.5 TF=179.3p TR=33.42n)\n"},
        {"BC847B", "45 V 100 mA NPN (Gummel–Poon)",
         ".model BC847B NPN(IS=7.049f BF=375.5 NF=0.9885 VAF=111.3 IKF=0.1 ISE=7.6f NE=1.581 BR=4 NR=0.9925 VAR=24 "
         "IKR=0.12 RB=1 RE=0.4 RC=0.9 CJE=11.5p VJE=0.6 MJE=0.33 CJC=5.25p VJC=0.48 MJC=0.31 TF=410p TR=95n)\n"},
        {"2N7002", "60 V N-MOSFET with body diode (level 1 + capacitances)",
         ".subckt 2N7002 D G S\n"
         "M1 D G S S NMOD W=1 L=1\n"
         "DB S D DBODY\n"
         ".model NMOD NMOS(LEVEL=1 VTO=1.8 KP=0.32 LAMBDA=0.01 RD=0.6 RS=0.3 CGSO=22p CGDO=3p CBD=8p PB=0.8 MJ=0.5)\n"
         ".model DBODY D(IS=1p N=1.1 RS=0.2 CJO=5p TT=20n BV=60 IBV=10u)\n"
         ".ends 2N7002\n"},
        {"BSS84", "50 V P-MOSFET with body diode (level 1 + capacitances)",
         ".subckt BSS84 D G S\n"
         "M1 D G S S PMOD W=1 L=1\n"
         "DB D S DBODY\n"
         ".model PMOD PMOS(LEVEL=1 VTO=-1.7 KP=0.12 LAMBDA=0.01 RD=2 RS=1 CGSO=20p CGDO=3p CBD=6p PB=0.8 MJ=0.5)\n"
         ".model DBODY D(IS=1p N=1.1 RS=0.5 CJO=4p TT=20n BV=50 IBV=10u)\n"
         ".ends BSS84\n"},
        {"UA741", "µA741 op-amp, Boyle macromodel (IN+ IN- V+ V- OUT)",
         "* Boyle macromodel of the uA741 (public domain).\n"
         ".subckt UA741 1 2 3 4 5\n"
         "c1 11 12 8.661E-12\n"
         "c2 6 7 30.00E-12\n"
         "dc 5 53 dx\n"
         "de 54 5 dx\n"
         "dlp 90 91 dx\n"
         "dln 92 90 dx\n"
         "dp 4 3 dx\n"
         "egnd 99 0 poly(2) (3,0) (4,0) 0 .5 .5\n"
         "fb 7 99 poly(5) vb vc ve vlp vln 0 10.61E6 -10E6 10E6 10E6 -10E6\n"
         "ga 6 0 11 12 188.5E-6\n"
         "gcm 0 6 10 99 5.961E-9\n"
         "iee 10 4 dc 15.16E-6\n"
         "hlim 90 0 vlim 1K\n"
         "q1 11 2 13 qx\n"
         "q2 12 1 14 qx\n"
         "r2 6 9 100.0E3\n"
         "rc1 3 11 5.305E3\n"
         "rc2 3 12 5.305E3\n"
         "re1 13 10 1.836E3\n"
         "re2 14 10 1.836E3\n"
         "ree 10 99 13.19E6\n"
         "ro1 8 5 50\n"
         "ro2 7 99 100\n"
         "rp 3 4 18.16E3\n"
         "vb 9 0 dc 0\n"
         "vc 3 53 dc 1\n"
         "ve 54 4 dc 1\n"
         "vlim 7 8 dc 0\n"
         "vlp 91 0 dc 40\n"
         "vln 0 92 dc 40\n"
         ".model dx D(Is=800.0E-18 Rs=1)\n"
         ".model qx NPN(Is=800.0E-18 Bf=93.75)\n"
         ".ends\n"},
    };
    return models;
}

}  // namespace sieda
