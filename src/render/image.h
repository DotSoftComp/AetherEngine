// Aether Engine — PNG/JPEG decoding via Windows Imaging Component (no deps).
#pragma once
#include <vector>
#include <cstdint>

namespace ae {

struct ImageData {
    int width = 0, height = 0;
    std::vector<uint8_t> rgba; // 8-bit RGBA, row-major, top-down
};

// Decodes PNG/JPEG/BMP/... from a memory blob. Returns false on failure.
bool decodeImage(const uint8_t* bytes, size_t size, ImageData& out);

} // namespace ae
