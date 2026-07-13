// Anti-aliased UI font baked with stb_truetype — portable (Windows/Linux/
// macOS/Android), no GDI/CoreText. Only the font-FILE lookup is OS-specific
// (a tiny helper below); rasterization is stb's. The single-header impl is
// compiled here once.
#include "font.h"
#define STB_TRUETYPE_IMPLEMENTATION
#include "../../third_party/stb/stb_truetype.h"
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace ae {

namespace {

std::vector<uint8_t> readFile(const std::string& path) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return {};
    std::streamsize n = f.tellg();
    if (n <= 0) return {};
    f.seekg(0);
    std::vector<uint8_t> bytes((size_t)n);
    f.read((char*)bytes.data(), n);
    return bytes;
}

// Resolve a face name to a TrueType file across platforms. Windows uses the
// %WINDIR%\Fonts dir; Linux/Android fall back to their usual default families.
std::vector<uint8_t> loadFontBytes(const char* face) {
    std::string f = face ? face : "";
    std::transform(f.begin(), f.end(), f.begin(), [](unsigned char c) { return (char)std::tolower(c); });
    bool mono = f.find("consol") != std::string::npos || f.find("mono") != std::string::npos ||
                f.find("courier") != std::string::npos;

    std::vector<std::string> candidates;
    if (const char* windir = std::getenv("WINDIR")) {
        std::string fonts = std::string(windir) + "\\Fonts\\";
        candidates.push_back(fonts + (mono ? "consola.ttf" : "segoeui.ttf"));
        candidates.push_back(fonts + "arial.ttf");
        candidates.push_back(fonts + "tahoma.ttf");
    }
    // Cross-platform fallbacks (Linux distros / Android).
    candidates.push_back(mono ? "/usr/share/fonts/truetype/dejavu/DejaVuSansMono.ttf"
                              : "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf");
    candidates.push_back(mono ? "/system/fonts/RobotoMono-Regular.ttf"
                              : "/system/fonts/Roboto-Regular.ttf");
    candidates.push_back("/system/fonts/DroidSans.ttf");

    for (const std::string& c : candidates) {
        std::vector<uint8_t> bytes = readFile(c);
        if (!bytes.empty()) return bytes;
    }
    return {};
}

} // namespace

bool Font::bake(const char* face, int pixelHeight) {
    std::vector<uint8_t> ttf = loadFontBytes(face);
    if (ttf.empty()) return false;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, ttf.data(), stbtt_GetFontOffsetForIndex(ttf.data(), 0)))
        return false;

    const int atlasW = 1024, atlasH = 256;
    std::vector<uint8_t> atlas((size_t)atlasW * atlasH * 4, 0);

    // Reserve a 4x4 opaque-white block at the top-left for solid fills.
    for (int y = 0; y < 4; ++y)
        for (int x = 0; x < 4; ++x) {
            uint8_t* px = &atlas[((size_t)y * atlasW + x) * 4];
            px[0] = px[1] = px[2] = px[3] = 255;
        }
    whiteU_ = 2.0f / atlasW;
    whiteV_ = 2.0f / atlasH;

    float scale = stbtt_ScaleForPixelHeight(&info, (float)pixelHeight);
    int asc = 0, desc = 0, gap = 0;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    ascent_ = asc * scale;
    lineHeight_ = (asc - desc + gap) * scale;

    // Every glyph is baked into a fixed-height cell with its coverage placed at
    // the shared baseline, so the UI's top-aligned, equal-cell text layout
    // keeps all glyphs on one baseline (matches the old GDI baker).
    const int baseline = (int)(ascent_ + 0.5f);
    const int cellH = (int)((asc - desc) * scale + 0.5f) + 2;

    int penX = 8, penY = 2, rowH = 0;
    for (int ci = 32; ci <= 126; ++ci) {
        int adv = 0, lsb = 0;
        stbtt_GetCodepointHMetrics(&info, ci, &adv, &lsb);

        int gx0 = 0, gy0 = 0, gx1 = 0, gy1 = 0;
        stbtt_GetCodepointBitmapBox(&info, ci, scale, scale, &gx0, &gy0, &gx1, &gy1);
        int gw = gx1 - gx0, gh = gy1 - gy0;

        Glyph& g = glyphs_[ci - 32];
        g.advance = adv * scale;
        int cellW = gw > 0 ? gw : 1;

        if (penX + cellW + 2 > atlasW) { penX = 8; penY += rowH + 2; rowH = 0; }
        if (penY + cellH + 2 > atlasH) break; // atlas full (shouldn't happen for ASCII)
        if (cellH > rowH) rowH = cellH;

        if (gw > 0 && gh > 0) {
            std::vector<uint8_t> cov((size_t)gw * gh);
            stbtt_MakeCodepointBitmap(&info, cov.data(), gw, gh, gw, scale, scale, ci);
            // baseline + gy0 is the glyph top within the cell (gy0 < 0 above it).
            int top = baseline + gy0;
            for (int y = 0; y < gh; ++y) {
                int cy = top + y;
                if (cy < 0 || cy >= cellH) continue;
                for (int x = 0; x < gw; ++x) {
                    uint8_t a = cov[(size_t)y * gw + x];
                    uint8_t* px = &atlas[((size_t)(penY + cy) * atlasW + (penX + x)) * 4];
                    px[0] = px[1] = px[2] = 255;
                    px[3] = a;
                }
            }
        }

        g.u0 = (float)penX / atlasW;
        g.v0 = (float)penY / atlasH;
        g.u1 = (float)(penX + cellW) / atlasW;
        g.v1 = (float)(penY + cellH) / atlasH;
        g.w = (float)cellW;
        g.h = (float)cellH;

        penX += cellW + 2;
    }

    atlas_ = rhi::createTexture2D(atlasW, atlasH, 1, rhi::TexFormat::RGBA8);
    rhi::uploadTexture2D(atlas_, 0, atlasW, atlasH, atlas.data());
    rhi::SamplerDesc smp;
    smp.mipmaps = false;
    smp.repeat = false;
    rhi::setSampler(atlas_, smp);
    return true;
}

float Font::textWidth(const char* s) const {
    float w = 0;
    for (; *s; ++s) w += glyph(*s).advance;
    return w;
}

void Font::destroy() { rhi::destroyTexture(atlas_); }

} // namespace ae
