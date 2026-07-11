#include "json.h"
#include <cstdlib>
#include <cstring>

namespace ae {

namespace {

struct Parser {
    const char* p;
    const char* end;
    int depth = 0;

    void skipWs() {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    }

    bool parseString(std::string& out) {
        if (p >= end || *p != '"') return false;
        ++p;
        out.clear();
        while (p < end && *p != '"') {
            if (*p == '\\' && p + 1 < end) {
                ++p;
                switch (*p) {
                case '"': out += '"'; break;
                case '\\': out += '\\'; break;
                case '/': out += '/'; break;
                case 'b': out += '\b'; break;
                case 'f': out += '\f'; break;
                case 'n': out += '\n'; break;
                case 'r': out += '\r'; break;
                case 't': out += '\t'; break;
                case 'u': {
                    if (end - p < 5) return false;
                    unsigned code = (unsigned)std::strtoul(std::string(p + 1, p + 5).c_str(), nullptr, 16);
                    // UTF-8 encode (surrogate pairs not needed for glTF metadata).
                    if (code < 0x80) out += (char)code;
                    else if (code < 0x800) {
                        out += (char)(0xC0 | (code >> 6));
                        out += (char)(0x80 | (code & 0x3F));
                    } else {
                        out += (char)(0xE0 | (code >> 12));
                        out += (char)(0x80 | ((code >> 6) & 0x3F));
                        out += (char)(0x80 | (code & 0x3F));
                    }
                    p += 4;
                    break;
                }
                default: return false;
                }
                ++p;
            } else {
                out += *p++;
            }
        }
        if (p >= end) return false;
        ++p; // closing quote
        return true;
    }

    bool parseValue(JsonValue& v) {
        if (++depth > 64) return false;
        skipWs();
        if (p >= end) return false;
        bool ok = false;
        switch (*p) {
        case '{': {
            v.type = JsonValue::Object;
            ++p;
            skipWs();
            if (p < end && *p == '}') { ++p; ok = true; break; }
            while (p < end) {
                std::string key;
                skipWs();
                if (!parseString(key)) return false;
                skipWs();
                if (p >= end || *p != ':') return false;
                ++p;
                v.obj.emplace_back(std::move(key), JsonValue{});
                if (!parseValue(v.obj.back().second)) return false;
                skipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == '}') { ++p; ok = true; break; }
                return false;
            }
            break;
        }
        case '[': {
            v.type = JsonValue::Array;
            ++p;
            skipWs();
            if (p < end && *p == ']') { ++p; ok = true; break; }
            while (p < end) {
                v.arr.emplace_back();
                if (!parseValue(v.arr.back())) return false;
                skipWs();
                if (p < end && *p == ',') { ++p; continue; }
                if (p < end && *p == ']') { ++p; ok = true; break; }
                return false;
            }
            break;
        }
        case '"':
            v.type = JsonValue::String;
            ok = parseString(v.str);
            break;
        case 't':
            if (end - p >= 4 && !std::strncmp(p, "true", 4)) {
                v.type = JsonValue::Bool; v.boolean = true; p += 4; ok = true;
            }
            break;
        case 'f':
            if (end - p >= 5 && !std::strncmp(p, "false", 5)) {
                v.type = JsonValue::Bool; v.boolean = false; p += 5; ok = true;
            }
            break;
        case 'n':
            if (end - p >= 4 && !std::strncmp(p, "null", 4)) {
                v.type = JsonValue::Null; p += 4; ok = true;
            }
            break;
        default: {
            char* numEnd = nullptr;
            v.number = std::strtod(p, &numEnd);
            if (numEnd && numEnd > p && numEnd <= end) {
                v.type = JsonValue::Number;
                p = numEnd;
                ok = true;
            }
            break;
        }
        }
        --depth;
        return ok;
    }
};

} // namespace

bool jsonParse(const char* text, size_t len, JsonValue& out) {
    Parser parser{text, text + len};
    if (!parser.parseValue(out)) return false;
    parser.skipWs();
    return true;
}

} // namespace ae
