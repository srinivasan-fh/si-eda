// SiEDA Core — minimal, dependency-free JSON value, parser and writer.
// Used for project persistence and for the snapshot bridge to the Swift UI.
#pragma once

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace sieda {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    /// Keys in std::map order (so the text is unchanged), stored flat: one allocation per object instead of one
    /// per key, which is what makes building the 4 MB snapshot of a large board fast.
    class Object {
    public:
        using value_type = std::pair<std::string, Json>;
        using iterator = std::vector<value_type>::iterator;
        using const_iterator = std::vector<value_type>::const_iterator;
        iterator begin();
        iterator end();
        const_iterator begin() const;
        const_iterator end() const;
        size_t size() const;
        bool empty() const;
        const_iterator find(std::string_view key) const;
        iterator find(std::string_view key);
        size_t count(std::string_view key) const;
        Json& operator[](std::string_view key);  // inserts null when missing

    private:
        std::vector<value_type> items_;  // sorted by key
    };

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : v_(std::in_place_type<bool>, b) {}
    Json(int v) : v_(std::in_place_type<double>, v) {}
    Json(int64_t v) : v_(std::in_place_type<double>, static_cast<double>(v)) {}
    Json(size_t v) : v_(std::in_place_type<double>, static_cast<double>(v)) {}
    Json(double v) : v_(std::in_place_type<double>, v) {}
    Json(const char* s) : v_(std::in_place_type<std::string>, s) {}
    Json(std::string s) : v_(std::in_place_type<std::string>, std::move(s)) {}
    Json(Array a) : v_(std::in_place_type<std::shared_ptr<Array>>, std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : v_(std::in_place_type<std::shared_ptr<Object>>, std::make_shared<Object>(std::move(o))) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return static_cast<Type>(v_.index()); }
    bool isNull() const { return v_.index() == 0; }
    bool isNumber() const { return v_.index() == 2; }
    bool isString() const { return v_.index() == 3; }
    bool isArray() const { return v_.index() == 4; }
    bool isObject() const { return v_.index() == 5; }

    bool asBool(bool def = false) const { const bool* b = std::get_if<bool>(&v_); return b ? *b : def; }
    double asNumber(double def = 0.0) const { const double* n = std::get_if<double>(&v_); return n ? *n : def; }
    /// Numbers outside the int range clamp to it (hostile files: a cast of 1e300 would be undefined); NaN → `def`.
    int asInt(int def = 0) const {
        const double* n = std::get_if<double>(&v_);
        if (!n || *n != *n) return def;
        return *n >= 2147483647.0 ? 2147483647 : *n <= -2147483648.0 ? -2147483647 - 1 : static_cast<int>(*n);
    }
    const std::string& asString() const;
    std::string asString(const std::string& def) const {
        const std::string* p = std::get_if<std::string>(&v_);
        return p ? *p : def;
    }

    const Array& items() const;
    const Object& fields() const;

    // Array helpers
    void push(Json v);
    void reserve(size_t n);
    size_t size() const;
    const Json& operator[](size_t i) const;

    // Object helpers
    Json& operator[](std::string_view key);
    const Json& get(std::string_view key) const;  // returns null Json if missing
    bool has(std::string_view key) const;

    std::string dump(bool pretty = false) const;
    static Json parse(const std::string& text);  // throws JsonError

private:
    void dumpTo(std::string& out, bool pretty, int indent) const;
    void detach();

    // The alternatives are in Type order; 40 bytes per value (a string in place, arrays and objects shared, copied
    // on write), which keeps the snapshot of a large board — hundreds of thousands of values — cheap to build.
    std::variant<std::monostate, bool, double, std::string, std::shared_ptr<Array>, std::shared_ptr<Object>> v_;
};

inline Json::Object::iterator Json::Object::begin() { return items_.begin(); }
inline Json::Object::iterator Json::Object::end() { return items_.end(); }
inline Json::Object::const_iterator Json::Object::begin() const { return items_.begin(); }
inline Json::Object::const_iterator Json::Object::end() const { return items_.end(); }
inline size_t Json::Object::size() const { return items_.size(); }
inline bool Json::Object::empty() const { return items_.empty(); }
inline size_t Json::Object::count(std::string_view key) const { return find(key) == end() ? 0 : 1; }

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace sieda
