#include "guid.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <objbase.h> // CoCreateGuid
#include <cstdio>

namespace ae {

Guid Guid::generate() {
    GUID g;
    if (CoCreateGuid(&g) == S_OK) {
        Guid out;
        out.hi = ((uint64_t)g.Data1 << 32) | ((uint64_t)g.Data2 << 16) | (uint64_t)g.Data3;
        uint64_t lo = 0;
        for (int i = 0; i < 8; ++i) lo = (lo << 8) | g.Data4[i];
        out.lo = lo;
        return out;
    }
    // Fallback: mix the high-res counter with a per-call sequence so ids stay
    // unique even if COM is somehow unavailable.
    static uint64_t seq = 0x9E3779B97F4A7C15ull;
    LARGE_INTEGER c;
    QueryPerformanceCounter(&c);
    seq = seq * 6364136223846793005ull + 1442695040888963407ull;
    Guid out;
    out.hi = (uint64_t)c.QuadPart ^ seq;
    out.lo = seq * 0xD1B54A32D192ED03ull;
    if (!out.valid()) out.lo = 1;
    return out;
}

std::string Guid::toString() const {
    char buf[33];
    std::snprintf(buf, sizeof(buf), "%016llx%016llx", (unsigned long long)hi,
                  (unsigned long long)lo);
    return buf;
}

Guid Guid::fromString(const std::string& s) {
    Guid g;
    if (s.size() != 32) return g;
    auto hexToU64 = [](const char* p) -> uint64_t {
        uint64_t v = 0;
        for (int i = 0; i < 16; ++i) {
            char c = p[i];
            v <<= 4;
            if (c >= '0' && c <= '9') v |= (uint64_t)(c - '0');
            else if (c >= 'a' && c <= 'f') v |= (uint64_t)(c - 'a' + 10);
            else if (c >= 'A' && c <= 'F') v |= (uint64_t)(c - 'A' + 10);
        }
        return v;
    };
    g.hi = hexToU64(s.c_str());
    g.lo = hexToU64(s.c_str() + 16);
    return g;
}

} // namespace ae
