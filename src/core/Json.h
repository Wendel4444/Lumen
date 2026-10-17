#pragma once
// Minimal JSON DOM: parse, query and serialize. Objects keep insertion order.
#include <cstdint>
#include <string>
#include <vector>

namespace lumen::json {

class Value {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    Value() = default;
    Value(std::nullptr_t) {}
    Value(bool v) : type_(Type::Bool), b_(v) {}
    Value(int v) : type_(Type::Number), n_(v) {}
    Value(unsigned v) : type_(Type::Number), n_(v) {}
    Value(int64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Value(uint64_t v) : type_(Type::Number), n_(static_cast<double>(v)) {}
    Value(double v) : type_(Type::Number), n_(v) {}
    Value(const char* v) : type_(Type::String), s_(v) {}
    Value(std::string v) : type_(Type::String), s_(std::move(v)) {}

    static Value Array() { Value v; v.type_ = Type::Array; return v; }
    static Value Object() { Value v; v.type_ = Type::Object; return v; }

    Type type() const { return type_; }
    bool isNull() const { return type_ == Type::Null; }
    bool isBool() const { return type_ == Type::Bool; }
    bool isNumber() const { return type_ == Type::Number; }
    bool isString() const { return type_ == Type::String; }
    bool isArray() const { return type_ == Type::Array; }
    bool isObject() const { return type_ == Type::Object; }

    bool asBool(bool def = false) const { return type_ == Type::Bool ? b_ : def; }
    double asNumber(double def = 0) const { return type_ == Type::Number ? n_ : def; }
    int64_t asInt(int64_t def = 0) const { return type_ == Type::Number ? static_cast<int64_t>(n_) : def; }
    const std::string& asString() const;
    std::string str(const std::string& def = {}) const { return type_ == Type::String ? s_ : def; }

    size_t size() const { return items_.size(); }
    const Value& operator[](size_t i) const;
    const Value& operator[](int i) const { return (*this)[static_cast<size_t>(i < 0 ? SIZE_MAX : i)]; }
    const Value& operator[](const std::string& key) const;
    const Value& operator[](const char* key) const { return (*this)[std::string(key)]; }
    const Value* find(const std::string& key) const;
    Value* find(const std::string& key);

    Value& set(const std::string& key, Value v);
    Value& push(Value v);
    void erase(const std::string& key);

    const std::vector<Value>& items() const { return items_; }
    std::vector<Value>& items() { return items_; }
    const std::vector<std::string>& keys() const { return keys_; }

private:
    Type type_ = Type::Null;
    bool b_ = false;
    double n_ = 0;
    std::string s_;
    std::vector<std::string> keys_;  // object keys (parallel to items_)
    std::vector<Value> items_;       // array items or object values
};

bool Parse(const std::string& text, Value& out, std::string* error = nullptr);
std::string Serialize(const Value& v, bool pretty = false);

}  // namespace lumen::json
