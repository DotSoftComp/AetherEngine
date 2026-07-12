#include "capture.h"
#include "../rhi/rhi.h"
#include "window.h"
#include "../gl/gl_api.h"
#include <cmath>
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

bool readBMP(const char* path, int& w, int& h, std::vector<uint8_t>& rgba) {
    FILE* f = nullptr;
    fopen_s(&f, path, "rb");
    if (!f) return false;
    uint8_t hdr[54];
    if (fread(hdr, 1, 54, f) != 54 || hdr[0] != 'B' || hdr[1] != 'M') { fclose(f); return false; }
    uint32_t dataOffset = *(uint32_t*)(hdr + 10);
    w = *(int32_t*)(hdr + 18);
    int32_t rawH = *(int32_t*)(hdr + 22);
    uint16_t bpp = *(uint16_t*)(hdr + 28);
    uint32_t compression = *(uint32_t*)(hdr + 30);
    bool topDown = rawH < 0;
    h = topDown ? -rawH : rawH;
    if (bpp != 24 || compression != 0 || w <= 0 || h <= 0 || w > 16384 || h > 16384) {
        fclose(f);
        return false;
    }
    int rowSize = (w * 3 + 3) & ~3;
    std::vector<uint8_t> row(rowSize);
    rgba.assign((size_t)w * h * 4, 255);
    fseek(f, (long)dataOffset, SEEK_SET);
    for (int y = 0; y < h; ++y) {
        if (fread(row.data(), 1, rowSize, f) != (size_t)rowSize) { fclose(f); return false; }
        // Stored bottom-up (matching writeBMP) unless the height was negative.
        uint8_t* dst = rgba.data() + (size_t)(topDown ? h - 1 - y : y) * w * 4;
        for (int x = 0; x < w; ++x) {
            dst[x * 4 + 0] = row[x * 3 + 2];
            dst[x * 4 + 1] = row[x * 3 + 1];
            dst[x * 4 + 2] = row[x * 3 + 0];
        }
    }
    fclose(f);
    return true;
}

ImageDiff compareImages(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b,
                        int w, int h) {
    ImageDiff d;
    double sumSq = 0.0, sumAbs = 0.0;
    size_t diffPixels = 0, pixels = (size_t)w * h;
    for (size_t p = 0; p < pixels; ++p) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            int e = (int)a[p * 4 + c] - (int)b[p * 4 + c];
            if (e < 0) e = -e;
            sumSq += (double)e * e;
            sumAbs += e;
            if (e > worst) worst = e;
        }
        if (worst > d.maxAbs) d.maxAbs = worst;
        if (worst > 2) ++diffPixels;
    }
    double samples = (double)pixels * 3.0;
    double mse = samples > 0 ? sumSq / samples : 0.0;
    d.meanAbs = samples > 0 ? sumAbs / samples : 0.0;
    d.diffPct = pixels > 0 ? 100.0 * (double)diffPixels / (double)pixels : 0.0;
    d.psnr = mse > 0.0 ? 10.0 * std::log10(255.0 * 255.0 / mse) : 99.0;
    if (d.psnr > 99.0) d.psnr = 99.0;
    return d;
}

bool writeDiffBMP(const char* path, const std::vector<uint8_t>& a,
                  const std::vector<uint8_t>& b, int w, int h) {
    std::vector<uint8_t> out((size_t)w * h * 4, 255);
    for (size_t p = 0, n = (size_t)w * h; p < n; ++p) {
        int worst = 0;
        for (int c = 0; c < 3; ++c) {
            int e = (int)a[p * 4 + c] - (int)b[p * 4 + c];
            if (e < 0) e = -e;
            if (e > worst) worst = e;
        }
        int amp = worst * 8;
        if (amp > 255) amp = 255;
        out[p * 4 + 0] = (uint8_t)amp;          // differences glow red
        out[p * 4 + 1] = (uint8_t)(amp / 4);
        out[p * 4 + 2] = (uint8_t)(amp / 4);
    }
    return writeBMP(path, w, h, out);
}

} // namespace ae
