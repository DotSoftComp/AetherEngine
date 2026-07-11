// Aether Engine — 128-bit globally-unique id for entities (and, later, assets).
//
// Unlike the per-session monotonic Entity::id(), a Guid is stable across
// save/load and across machines, so it's the durable way to *reference* an
// entity: serialized fields (camera targets, triggers) and editor dropdowns
// all store a Guid, then resolve it to a live Entity* through the World. A
// default-constructed Guid is the null/invalid reference (valid() == false).
#pragma once
#include <cstdint>
#include <functional>
#include <string>

namespace ae {

struct Guid {
    uint64_t hi = 0;
    uint64_t lo = 0;

    bool valid() const { return hi != 0 || lo != 0; }
    bool operator==(const Guid& o) const { return hi == o.hi && lo == o.lo; }
    bool operator!=(const Guid& o) const { return !(*this == o); }
    bool operator<(const Guid& o) const { return hi != o.hi ? hi < o.hi : lo < o.lo; }

    // A fresh random id (Win32 CoCreateGuid, with a deterministic fallback).
    static Guid generate();

    // 32 lowercase hex chars, no separators (compact + JSON-friendly).
    std::string toString() const;
    static Guid fromString(const std::string& s);
};

} // namespace ae

namespace std {
template <>
struct hash<ae::Guid> {
    size_t operator()(const ae::Guid& g) const noexcept {
        return std::hash<uint64_t>()(g.hi) ^ (std::hash<uint64_t>()(g.lo) << 1);
    }
};
} // namespace std
