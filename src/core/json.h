// Aether Engine — minimal recursive-descent JSON parser (enough for glTF).
#pragma once
#include <string>
#include <vector>
#include <utility>

namespace ae {

struct JsonValue {
    enum Type { Null, Bool, Number, String, Array, Object } type = Null;
    bool boolean = false;
    double number = 0;
    std::string str;
    std::vector<JsonValue> arr;
    std::vector<std::pair<std::string, JsonValue>> obj;

    // Object member lookup; returns nullptr when absent or not an object.
    const JsonValue* find(const char* key) const {
        if (type != Object) return nullptr;
        for (auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    double num(const char* key, double def) const {
        const JsonValue* v = find(key);
        return v && v->type == Number ? v->number : def;
    }
    int integer(const char* key, int def) const { return (int)num(key, def); }
    bool flag(const char* key, bool def) const {
        const JsonValue* v = find(key);
        return v && v->type == Bool ? v->boolean : def;
    }
    const std::string* string(const char* key) const {
        const JsonValue* v = find(key);
        return v && v->type == String ? &v->str : nullptr;
    }
    size_t size() const { return type == Array ? arr.size() : 0; }
    const JsonValue& operator[](size_t i) const { return arr[i]; }
};

// Returns false on malformed input (out left partially filled).
bool jsonParse(const char* text, size_t len, JsonValue& out);

} // namespace ae
