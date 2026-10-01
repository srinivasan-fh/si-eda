// SiEDA Core — minimal, dependency-free JSON value, parser and writer.
// Used for project persistence and for the snapshot bridge to the Swift UI.
#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace sieda {

class Json {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };
    using Array = std::vector<Json>;
    using Object = std::map<std::string, Json>;

    Json() = default;
    Json(std::nullptr_t) {}
    Json(bool b) : type_(Type::Bool), bool_(b) {}
    Json(int v) : type_(Type::Number), num_(v) {}
    Json(int64_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(size_t v) : type_(Type::Number), num_(static_cast<double>(v)) {}
    Json(double v) : type_(Type::Number), num_(v) {}
    Json(const char* s) : type_(Type::String), str_(s) {}
    Json(std::string s) : type_(Type::String), str_(std::move(s)) {}
    Json(Array a) : type_(Type::Array), arr_(std::make_shared<Array>(std::move(a))) {}
    Json(Object o) : type_(Type::Object), obj_(std::make_shared<Object>(std::move(o))) {}

    static Json array() { return Json(Array{}); }
    static Json object() { return Json(Object{}); }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? bool_ : def; }
    double asNumber(double def = 0.0) const { return type_ == Type::Number ? num_ : def; }
    int asInt(int def = 0) const { return type_ == Type::Number ? static_cast<int>(num_) : def; }
    const std::string& asString() const;
    std::string asString(const std::string& def) const { return type_ == Type::String ? str_ : def; }

    const Array& items() const;
    const Object& fields() const;

    // Array helpers
    void push(Json v);
    size_t size() const;
    const Json& operator[](size_t i) const;

    // Object helpers
    Json& operator[](const std::string& key);
    const Json& get(const std::string& key) const;  // returns null Json if missing
    bool has(const std::string& key) const;

    std::string dump(bool pretty = false) const;
    static Json parse(const std::string& text);  // throws JsonError

private:
    void dumpTo(std::string& out, bool pretty, int indent) const;
    void detach();

    Type type_ = Type::Null;
    bool bool_ = false;
    double num_ = 0.0;
    std::string str_;
    std::shared_ptr<Array> arr_;
    std::shared_ptr<Object> obj_;
};

struct JsonError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

}  // namespace sieda
