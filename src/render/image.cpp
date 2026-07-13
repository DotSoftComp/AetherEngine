// PNG/JPEG/BMP/TGA/... decoding via stb_image — portable (Windows/Linux/macOS/
// Android), no OS imaging deps. The single-header implementation is compiled
// here once.
#include "image.h"
#define STB_IMAGE_IMPLEMENTATION
#define STBI_NO_STDIO // decode from memory only; the asset layer owns file I/O
#include "../../third_party/stb/stb_image.h"

namespace ae {

bool decodeImage(const uint8_t* bytes, size_t size, ImageData& out) {
    int w = 0, h = 0, channels = 0;
    // Force RGBA8, top-down (stb's default origin), matching the old WIC output.
    stbi_uc* pixels = stbi_load_from_memory(bytes, (int)size, &w, &h, &channels, 4);
    if (!pixels || w <= 0 || h <= 0) {
        if (pixels) stbi_image_free(pixels);
        return false;
    }
    out.width = w;
    out.height = h;
    out.rgba.assign(pixels, pixels + (size_t)w * h * 4);
    stbi_image_free(pixels);
    return true;
}

} // namespace ae
