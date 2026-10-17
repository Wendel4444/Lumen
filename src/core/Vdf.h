#pragma once
// Valve KeyValues (text VDF/ACF) parser. Key lookups are case-insensitive,
// because Steam is inconsistent ("apps" vs "Apps").
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace lumen::vdf {

struct Node {
    std::string value;
    bool isObject = false;
    std::vector<std::string> keys;
    std::vector<Node> children;

    const Node* get(const char* key) const;
    const Node* path(std::initializer_list<const char*> keys) const;
    std::string str(const char* key, const std::string& def = {}) const;
    int64_t num(const char* key, int64_t def = 0) const;
};

bool Parse(const std::string& text, Node& root);

}  // namespace lumen::vdf
