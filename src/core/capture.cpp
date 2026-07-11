#include "capture.h"
#include "../rhi/rhi.h"
#include "window.h"
#include "../gl/gl_api.h"
#include <cstdio>

namespace ae {

bool writeBMP(const char* path, int w, int h, const std::vector<uint8_t>& rgba) {
    int rowSize = (w * 3 + 3) & ~3, dataSize = rowSize * h, fileSize = 54 + dataSize;
    std::vector<uint8_t> file(fileSize, 0);
    uint8_t* p = file.data();
    p[0] = 'B'; p[1] = 'M';
    *(uint32_t*)(p + 2) = fileSize; *(uint32_t*)(p + 10) = 54; *(uint32_t*)(p + 14) = 40;
    *(int32_t*)(p + 18) = w; *(int32_t*)(p + 22) = h;
    *(uint16_t*)(p + 26) = 1; *(uint16_t*)(p + 28) = 24; *(uint32_t*)(p + 34) = dataSize;
    for (int y = 0; y < h; ++y) {
        uint8_t* row = p + 54 + y * rowSize;
        const uint8_t* src = rgba.data() + (size_t)y * w * 4;
        for (int x = 0; x < w; ++x) {
            row[x * 3 + 0] = src[x * 4 + 2];
            row[x * 3 + 1] = src[x * 4 + 1];
            row[x * 3 + 2] = src[x * 4 + 0];
        }
    }
    FILE* f = nullptr;
    fopen_s(&f, path, "wb");
    if (!f) return false;
    fwrite(file.data(), 1, file.size(), f);
    fclose(f);
    return true;
}

void captureScreenshot(Window& window, const char* path) {
    int w = window.width(), h = window.height();
    std::vector<uint8_t> pixels((size_t)w * h * 4);
    rhi::readBackbuffer(w, h, pixels.data());
    if (writeBMP(path, w, h, pixels)) std::printf("Screenshot written: %s\n", path);
    else std::fprintf(stderr, "Failed to write %s\n", path);
}

} // namespace ae
