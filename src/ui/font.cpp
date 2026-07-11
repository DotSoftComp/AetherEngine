#include "font.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <vector>
#include <cstring>

namespace ae {

bool Font::bake(const char* face, int pixelHeight) {
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

    HDC dc = CreateCompatibleDC(nullptr);
    if (!dc) return false;

    LOGFONTA lf = {};
    lf.lfHeight = -pixelHeight;
    lf.lfWeight = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    lf.lfQuality = ANTIALIASED_QUALITY;
    std::strncpy(lf.lfFaceName, face, LF_FACESIZE - 1);
    HFONT font = CreateFontIndirectA(&lf);
    HFONT oldFont = (HFONT)SelectObject(dc, font);

    TEXTMETRICA tm;
    GetTextMetricsA(dc, &tm);
    lineHeight_ = (float)tm.tmHeight;
    ascent_ = (float)tm.tmAscent;

    const int cellW = pixelHeight * 2 + 4;
    const int cellH = tm.tmHeight + 2;

    // A scratch DIB we render each glyph into, then copy its coverage out.
    BITMAPINFO bmi = {};
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = cellW;
    bmi.bmiHeader.biHeight = -cellH; // top-down
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    void* dibBits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bmi, DIB_RGB_COLORS, &dibBits, nullptr, 0);
    HBITMAP oldBmp = (HBITMAP)SelectObject(dc, dib);
    SetTextColor(dc, RGB(255, 255, 255));

    int penX = 8, penY = 2, rowH = 0;
    for (int ci = 32; ci <= 126; ++ci) {
        char c = (char)ci;
        SIZE sz;
        GetTextExtentPoint32A(dc, &c, 1, &sz);
        int gw = sz.cx < 1 ? 1 : (sz.cx > cellW ? cellW : sz.cx);
        int gh = cellH;

        // Clear the cell to black, then draw the glyph in white (transparent bg
        // so anti-aliased edges blend against black -> coverage in the channel).
        PatBlt(dc, 0, 0, cellW, cellH, BLACKNESS);
        SetBkMode(dc, TRANSPARENT);
        TextOutA(dc, 0, 0, &c, 1);
        GdiFlush();

        if (penX + gw + 2 > atlasW) { penX = 8; penY += rowH + 2; rowH = 0; }
        if (penY + gh + 2 > atlasH) break; // atlas full (shouldn't happen)
        if (gh > rowH) rowH = gh;

        const uint8_t* src = (const uint8_t*)dibBits;
        for (int y = 0; y < gh; ++y)
            for (int x = 0; x < gw; ++x) {
                uint8_t cov = src[((size_t)y * cellW + x) * 4]; // B channel = coverage
                uint8_t* px = &atlas[((size_t)(penY + y) * atlasW + (penX + x)) * 4];
                px[0] = px[1] = px[2] = 255;
                px[3] = cov;
            }

        Glyph& g = glyphs_[ci - 32];
        g.u0 = (float)penX / atlasW;
        g.v0 = (float)penY / atlasH;
        g.u1 = (float)(penX + gw) / atlasW;
        g.v1 = (float)(penY + gh) / atlasH;
        g.w = (float)gw;
        g.h = (float)gh;
        g.advance = (float)sz.cx;

        penX += gw + 2;
    }

    SelectObject(dc, oldBmp);
    DeleteObject(dib);
    SelectObject(dc, oldFont);
    DeleteObject(font);
    DeleteDC(dc);

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
