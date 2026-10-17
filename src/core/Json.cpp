#include "Json.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace lumen::json {

namespace {
const Value kNull;
const std::string kEmpty;

struct Parser {
    const char* p;
    const char* e;
    std::string err;

    bool fail(const char* msg) {
        if (err.empty()) err = msg;
        return false;
    }
    void ws() {
        while (p < e && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) ++p;
    }
    static void appendUtf8(std::string& s, uint32_t cp) {
        if (cp < 0x80) {
            s += static_cast<char>(cp);
        } else if (cp < 0x800) {
            s += static_cast<char>(0xC0 | (cp >> 6));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            s += static_cast<char>(0xE0 | (cp >> 12));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            s += static_cast<char>(0xF0 | (cp >> 18));
            s += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            s += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            s += static_cast<char>(0x80 | (cp & 0x3F));
        }
    }
    bool hex4(uint32_t& out) {
        if (e - p < 4) return fail("bad \\u escape");
        out = 0;
        for (int i = 0; i < 4; ++i) {
            char c = *p++;
            out <<= 4;
            if (c >= '0' && c <= '9') out |= c - '0';
            else if (c >= 'a' && c <= 'f') out |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') out |= c - 'A' + 10;
            else return fail("bad hex digit");
        }
        return true;
    }
    bool parseString(std::string& s) {
        ++p;  // opening quote
        while (p < e) {
            char c = *p++;
            if (c == '"') return true;
            if (c != '\\') {
                s += c;
                continue;
            }
            if (p >= e) break;
            char esc = *p++;
            switch (esc) {
                case '"': s += '"'; break;
                case '\\': s += '\\'; break;
                case '/': s += '/'; break;
                case 'b': s += '\b'; break;
                case 'f': s += '\f'; break;
                case 'n': s += '\n'; break;
                case 'r': s += '\r'; break;
                case 't': s += '\t'; break;
                case 'u': {
                    uint32_t cp;
                    if (!hex4(cp)) return false;
                    if (cp >= 0xD800 && cp <= 0xDBFF && e - p >= 6 && p[0] == '\\' && p[1] == 'u') {
                        p += 2;
                        uint32_t lo;
                        if (!hex4(lo)) return false;
                        if (lo >= 0xDC00 && lo <= 0xDFFF) cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                    }
                    appendUtf8(s, cp);
                    break;
                }
                default: return fail("bad escape");
            }
        }
        return fail("unterminated string");
    }
    bool parseValue(Value& v, int depth) {
        if (depth > 256) return fail("nesting too deep");
        ws();
        if (p >= e) return fail("unexpected end");
        char c = *p;
        if (c == '{') {
            ++p;
            v = Value::Object();
            ws();
            if (p < e && *p == '}') { ++p; return true; }
            for (;;) {
                ws();
                if (p >= e || *p != '"') return fail("expected key");
                std::string key;
                if (!parseString(key)) return false;
                ws();
                if (p >= e || *p != ':') return fail("expected ':'");
                ++p;
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.set(key, std::move(child));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == '}') { ++p; return true; }
                return fail("expected ',' or '}'");
            }
        }
        if (c == '[') {
            ++p;
            v = Value::Array();
            ws();
            if (p < e && *p == ']') { ++p; return true; }
            for (;;) {
                Value child;
                if (!parseValue(child, depth + 1)) return false;
                v.push(std::move(child));
                ws();
                if (p < e && *p == ',') { ++p; continue; }
                if (p < e && *p == ']') { ++p; return true; }
                return fail("expected ',' or ']'");
            }
        }
        if (c == '"') {
            std::string s;
            if (!parseString(s)) return false;
            v = Value(std::move(s));
            return true;
        }
        if (e - p >= 4 && std::strncmp(p, "true", 4) == 0) { p += 4; v = Value(true); return true; }
        if (e - p >= 5 && std::strncmp(p, "false", 5) == 0) { p += 5; v = Value(false); return true; }
        if (e - p >= 4 && std::strncmp(p, "null", 4) == 0) { p += 4; v = Value(); return true; }
        if (c == '-' || (c >= '0' && c <= '9')) {
            char* end = nullptr;
            double d = std::strtod(p, &end);
            if (end == p) return fail("bad number");
            p = end;
            v = Value(d);
            return true;
        }
        return fail("unexpected character");
    }
};

void escape(std::string& out, const std::string& s) {
    out += '"';
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    out += '"';
}

void write(std::string& out, const Value& v, bool pretty, int indent) {
    auto nl = [&](int lvl) {
        if (!pretty) return;
        out += '\n';
        out.append(static_cast<size_t>(lvl) * 2, ' ');
    };
    switch (v.type()) {
        case Value::Type::Null: out += "null"; break;
        case Value::Type::Bool: out += v.asBool() ? "true" : "false"; break;
        case Value::Type::Number: {
            double d = v.asNumber();
            char buf[40];
            if (std::isfinite(d) && std::floor(d) == d && std::fabs(d) < 1e15)
                std::snprintf(buf, sizeof(buf), "%lld", static_cast<long long>(d));
            else if (std::isfinite(d))
                std::snprintf(buf, sizeof(buf), "%.10g", d);
            else
                std::snprintf(buf, sizeof(buf), "0");
            out += buf;
            break;
        }
        case Value::Type::String: escape(out, v.asString()); break;
        case Value::Type::Array: {
            out += '[';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                write(out, v[i], pretty, indent + 1);
            }
            if (v.size()) nl(indent);
            out += ']';
            break;
        }
        case Value::Type::Object: {
            out += '{';
            for (size_t i = 0; i < v.size(); ++i) {
                if (i) out += ',';
                nl(indent + 1);
                escape(out, v.keys()[i]);
                out += pretty ? ": " : ":";
                write(out, v.items()[i], pretty, indent + 1);
            }
            if (v.size()) nl(indent);
            out += '}';
            break;
        }
    }
}
}  // namespace

const std::string& Value::asString() const { return type_ == Type::String ? s_ : kEmpty; }

const Value& Value::operator[](size_t i) const {
    return (type_ == Type::Array && i < items_.size()) ? items_[i] : kNull;
}

const Value* Value::find(const std::string& key) const {
    if (type_ != Type::Object) return nullptr;
    for (size_t i = 0; i < keys_.size(); ++i)
        if (keys_[i] == key) return &items_[i];
    return nullptr;
}

Value* Value::find(const std::string& key) {
    return const_cast<Value*>(static_cast<const Value*>(this)->find(key));
}

const Value& Value::operator[](const std::string& key) const {
    const Value* v = find(key);
    return v ? *v : kNull;
}

Value& Value::set(const std::string& key, Value v) {
    if (type_ != Type::Object) *this = Object();
    if (Value* existing = find(key)) {
        *existing = std::move(v);
        return *existing;
    }
    keys_.push_back(key);
    items_.push_back(std::move(v));
    return items_.back();
}

Value& Value::push(Value v) {
    if (type_ != Type::Array) *this = Array();
    items_.push_back(std::move(v));
    return items_.back();
}

void Value::erase(const std::string& key) {
    for (size_t i = 0; i < keys_.size(); ++i) {
        if (keys_[i] == key) {
            keys_.erase(keys_.begin() + static_cast<ptrdiff_t>(i));
            items_.erase(items_.begin() + static_cast<ptrdiff_t>(i));
            return;
        }
    }
}

bool Parse(const std::string& text, Value& out, std::string* error) {
    Parser ps{text.data(), text.data() + text.size(), {}};
    // Skip UTF-8 BOM.
    if (text.size() >= 3 && static_cast<unsigned char>(text[0]) == 0xEF &&
        static_cast<unsigned char>(text[1]) == 0xBB && static_cast<unsigned char>(text[2]) == 0xBF)
        ps.p += 3;
    Value v;
    if (!ps.parseValue(v, 0)) {
        if (error) *error = ps.err;
        return false;
    }
    out = std::move(v);
    return true;
}

std::string Serialize(const Value& v, bool pretty) {
    std::string out;
    write(out, v, pretty, 0);
    return out;
}

}  // namespace lumen::json
