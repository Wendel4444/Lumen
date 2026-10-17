#include "Vdf.h"

#include <cstdlib>
#include <cstring>

namespace lumen::vdf {

namespace {
bool iequals(const std::string& a, const char* b) {
    size_t n = std::strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; ++i) {
        char x = a[i], y = b[i];
        if (x >= 'A' && x <= 'Z') x = static_cast<char>(x + 32);
        if (y >= 'A' && y <= 'Z') y = static_cast<char>(y + 32);
        if (x != y) return false;
    }
    return true;
}

enum class Tok { End, Open, Close, Str, Cond };

struct Lexer {
    const char* p;
    const char* e;

    Tok next(std::string& out) {
        for (;;) {
            while (p < e && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
            if (p + 1 < e && p[0] == '/' && p[1] == '/') {
                while (p < e && *p != '\n') ++p;
                continue;
            }
            break;
        }
        if (p >= e) return Tok::End;
        char c = *p;
        if (c == '{') { ++p; return Tok::Open; }
        if (c == '}') { ++p; return Tok::Close; }
        out.clear();
        if (c == '"') {
            ++p;
            while (p < e && *p != '"') {
                if (*p == '\\' && p + 1 < e) {
                    char n = p[1];
                    if (n == 'n') out += '\n';
                    else if (n == 't') out += '\t';
                    else out += n;  // \\ and \"
                    p += 2;
                } else {
                    out += *p++;
                }
            }
            if (p < e) ++p;
            return Tok::Str;
        }
        bool cond = c == '[';
        while (p < e && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n' && *p != '{' && *p != '}' && *p != '"')
            out += *p++;
        return cond ? Tok::Cond : Tok::Str;
    }
};

bool parseBody(Lexer& lx, Node& obj, bool top, int depth) {
    if (depth > 64) return false;
    std::string key, val;
    for (;;) {
        Tok t = lx.next(key);
        if (t == Tok::Cond) continue;
        if (t == Tok::End) return top;
        if (t == Tok::Close) return !top;
        if (t != Tok::Str) return false;
        Tok v = lx.next(val);
        while (v == Tok::Cond) v = lx.next(val);
        Node child;
        if (v == Tok::Open) {
            child.isObject = true;
            if (!parseBody(lx, child, false, depth + 1)) return false;
        } else if (v == Tok::Str) {
            child.value = val;
        } else {
            return false;
        }
        obj.keys.push_back(key);
        obj.children.push_back(std::move(child));
    }
}
}  // namespace

const Node* Node::get(const char* key) const {
    for (size_t i = 0; i < keys.size(); ++i)
        if (iequals(keys[i], key)) return &children[i];
    return nullptr;
}

const Node* Node::path(std::initializer_list<const char*> ks) const {
    const Node* n = this;
    for (const char* k : ks) {
        n = n->get(k);
        if (!n) return nullptr;
    }
    return n;
}

std::string Node::str(const char* key, const std::string& def) const {
    const Node* n = get(key);
    return (n && !n->isObject) ? n->value : def;
}

int64_t Node::num(const char* key, int64_t def) const {
    const Node* n = get(key);
    if (!n || n->isObject || n->value.empty()) return def;
    return std::strtoll(n->value.c_str(), nullptr, 10);
}

bool Parse(const std::string& text, Node& root) {
    root = Node{};
    root.isObject = true;
    Lexer lx{text.data(), text.data() + text.size()};
    return parseBody(lx, root, true, 0);
}

}  // namespace lumen::vdf
