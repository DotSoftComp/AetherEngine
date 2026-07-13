// Aether Engine — anti-aliased UI font baked from a system TrueType face via
// Windows GDI (no external font libraries; uses the OS the way WIC decodes PNGs).
#pragma once
#include "../rhi/rhi.h"

namespace ae {

struct Glyph {
    float u0 = 0, v0 = 0, u1 = 0, v1 = 0; // atlas UVs
    float w = 0, h = 0;                    // pixel size
    float advance = 0;                     // pen advance
};

class Font {
public:
    // Bakes ASCII 32..126 of `face` at `pixelHeight` into an RGBA atlas.
    bool bake(const char* face, int pixelHeight);
    void destroy();

    unsigned atlas() const { return atlas_.id; } // rhi texture id
    float lineHeight() const { return lineHeight_; }
    float ascent() const { return ascent_; }
    const Glyph& glyph(char c) const {
        unsigned i = (unsigned char)c;
        if (i < 32 || i > 126) i = '?';
        return glyphs_[i - 32];
    }
    float textWidth(const char* s) const;

    // UVs of a fully-opaque white texel (for solid fills).
    float whiteU() const { return whiteU_; }
    float whiteV() const { return whiteV_; }

private:
    rhi::TextureHandle atlas_;
    Glyph glyphs_[95];
    float lineHeight_ = 0, ascent_ = 0;
    float whiteU_ = 0, whiteV_ = 0;
};

} // namespace ae
