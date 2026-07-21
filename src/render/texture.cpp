#include "texture.h"
#include "../core/math3d.h"
#include "../core/json.h"
#include "../core/log.h"
#define STB_DXT_IMPLEMENTATION
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../../third_party/stb/stb_dxt.h"
#include <windows.h> // CreateDirectoryA (texture cache), QueryPerformanceCounter
#include <cmath>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>

namespace ae {

// ---- BC compression + disk cache -------------------------------------------
namespace {
std::string g_texCacheDir;
int g_mipBias = 0;

struct MipLevel {
    int w, h;
    std::vector<uint8_t> rgba;
};

// Box-filtered mip chain (clamped for odd dimensions), down to 1x1.
std::vector<MipLevel> buildMips(int w, int h, const uint8_t* rgba) {
    std::vector<MipLevel> mips;
    mips.push_back({w, h, std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4)});
    while (mips.back().w > 1 || mips.back().h > 1) {
        const MipLevel& src = mips.back();
        int nw = src.w > 1 ? src.w / 2 : 1;
        int nh = src.h > 1 ? src.h / 2 : 1;
        MipLevel dst{nw, nh, std::vector<uint8_t>((size_t)nw * nh * 4)};
        for (int y = 0; y < nh; ++y) {
            int sy0 = y * 2, sy1 = std::min(sy0 + 1, src.h - 1);
            for (int x = 0; x < nw; ++x) {
                int sx0 = x * 2, sx1 = std::min(sx0 + 1, src.w - 1);
                for (int c = 0; c < 4; ++c) {
                    int sum = src.rgba[((size_t)sy0 * src.w + sx0) * 4 + c] +
                              src.rgba[((size_t)sy0 * src.w + sx1) * 4 + c] +
                              src.rgba[((size_t)sy1 * src.w + sx0) * 4 + c] +
                              src.rgba[((size_t)sy1 * src.w + sx1) * 4 + c];
                    dst.rgba[((size_t)y * nw + x) * 4 + c] = (uint8_t)(sum / 4);
                }
            }
        }
        mips.push_back(std::move(dst));
    }
    return mips;
}

// One level, 4x4 blocks (edges clamped), BC1 (8 B/block) or BC3 (16 B/block).
std::vector<uint8_t> compressLevel(const MipLevel& m, bool alpha) {
    int bw = (m.w + 3) / 4, bh = (m.h + 3) / 4;
    size_t blockSize = alpha ? 16 : 8;
    std::vector<uint8_t> out((size_t)bw * bh * blockSize);
    uint8_t block[16 * 4];
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int py = 0; py < 4; ++py) {
                int sy = std::min(by * 4 + py, m.h - 1);
                for (int px = 0; px < 4; ++px) {
                    int sx = std::min(bx * 4 + px, m.w - 1);
                    std::memcpy(&block[(py * 4 + px) * 4],
                                &m.rgba[((size_t)sy * m.w + sx) * 4], 4);
                }
            }
            stb_compress_dxt_block(&out[((size_t)by * bw + bx) * blockSize], block,
                                   alpha ? 1 : 0, STB_DXT_NORMAL);
        }
    }
    return out;
}

// BC5: two independent BC4 channels (R = normal.x, G = normal.y). 16 B/block,
// same size as BC3 but far cleaner for normals — BC1/BC3 quantize RGB jointly
// through a 565 line fit, which mangles tangent-space directions.
std::vector<uint8_t> compressLevelBC5(const MipLevel& m) {
    int bw = (m.w + 3) / 4, bh = (m.h + 3) / 4;
    std::vector<uint8_t> out((size_t)bw * bh * 16);
    uint8_t block[16 * 2]; // RG pairs
    for (int by = 0; by < bh; ++by) {
        for (int bx = 0; bx < bw; ++bx) {
            for (int py = 0; py < 4; ++py) {
                int sy = std::min(by * 4 + py, m.h - 1);
                for (int px = 0; px < 4; ++px) {
                    int sx = std::min(bx * 4 + px, m.w - 1);
                    const uint8_t* s = &m.rgba[((size_t)sy * m.w + sx) * 4];
                    block[(py * 4 + px) * 2 + 0] = s[0];
                    block[(py * 4 + px) * 2 + 1] = s[1];
                }
            }
            stb_compress_bc5_block(&out[((size_t)by * bw + bx) * 16], block);
        }
    }
    return out;
}

// Box-filtered half-step downscale, used to honour maxSize before encoding.
MipLevel downscaleTo(int w, int h, const uint8_t* rgba, int maxSize) {
    MipLevel cur{w, h, std::vector<uint8_t>(rgba, rgba + (size_t)w * h * 4)};
    while (maxSize > 0 && (cur.w > maxSize || cur.h > maxSize) && (cur.w > 1 || cur.h > 1)) {
        int nw = cur.w > 1 ? cur.w / 2 : 1, nh = cur.h > 1 ? cur.h / 2 : 1;
        MipLevel dst{nw, nh, std::vector<uint8_t>((size_t)nw * nh * 4)};
        for (int y = 0; y < nh; ++y) {
            int sy0 = std::min(y * 2, cur.h - 1), sy1 = std::min(y * 2 + 1, cur.h - 1);
            for (int x = 0; x < nw; ++x) {
                int sx0 = std::min(x * 2, cur.w - 1), sx1 = std::min(x * 2 + 1, cur.w - 1);
                for (int c = 0; c < 4; ++c) {
                    int sum = cur.rgba[((size_t)sy0 * cur.w + sx0) * 4 + c] +
                              cur.rgba[((size_t)sy0 * cur.w + sx1) * 4 + c] +
                              cur.rgba[((size_t)sy1 * cur.w + sx0) * 4 + c] +
                              cur.rgba[((size_t)sy1 * cur.w + sx1) * 4 + c];
                    dst.rgba[((size_t)y * nw + x) * 4 + c] = (uint8_t)(sum / 4);
                }
            }
        }
        cur = std::move(dst);
    }
    return cur;
}

struct AetexHeader {
    char magic[4];      // "AETX"
    uint32_t version;   // 1
    int32_t w, h, mips;
    uint32_t glFormat;  // format tag (historically the GL enum value - stable)
};

// The .aetex format tag values (== the GL compressed-format enums they were
// born as; kept verbatim so existing caches stay valid across backends).
constexpr uint32_t kTagBC1 = 0x83F1, kTagBC1s = 0x8C4C, kTagBC3 = 0x83F3, kTagBC3s = 0x8C4F,
                   kTagBC5 = 0x8DBD;

rhi::TexFormat tagToFormat(uint32_t tag) {
    switch (tag) {
    case kTagBC1: return rhi::TexFormat::BC1;
    case kTagBC1s: return rhi::TexFormat::BC1_SRGB;
    case kTagBC3: return rhi::TexFormat::BC3;
    case kTagBC3s: return rhi::TexFormat::BC3_SRGB;
    case kTagBC5: return rhi::TexFormat::BC5;
    }
    return rhi::TexFormat::BC1;
}

// Creates the GPU texture, reusing the handle's slot when it already has one
// so a hot re-import keeps the same id (materials cache ids raw).
void makeTexture(rhi::TextureHandle& h, int w, int hgt, int mips, rhi::TexFormat f) {
    if (h.valid()) rhi::recreateTexture2D(h, w, hgt, mips, f);
    else h = rhi::createTexture2D(w, hgt, mips, f);
}

std::string cacheFileFor(uint64_t hash) {
    if (g_texCacheDir.empty() || hash == 0) return "";
    char name[40];
    std::snprintf(name, sizeof(name), "%016llx.aetex", (unsigned long long)hash);
    return g_texCacheDir + "/" + name;
}

} // namespace

// Folds the import choices into the content hash so re-importing the same
// bytes with different settings can't collide in the cache.
uint64_t TextureImportSettings::hashInto(uint64_t contentHash) const {
    uint64_t h = contentHash ? contentHash : 0;
    if (!h) return 0; // caching disabled
    auto mix = [&h](uint64_t v) { h ^= v + 0x9e3779b97f4a7c15ull + (h << 6) + (h >> 2); };
    mix(srgb ? 1 : 2);
    mix(normalMap ? 3 : 4);
    mix(compress ? 5 : 6);
    mix((uint64_t)maxSize * 131 + 7);
    mix((uint64_t)(mipBias + g_mipBias) * 17 + 9);
    return h ? h : 1;
}

TextureImportSettings loadImportSettings(const std::string& imagePath,
                                         TextureImportSettings fallback) {
    std::string side = imagePath + ".import.json";
    std::ifstream f(side, std::ios::binary | std::ios::ate);
    if (!f) return fallback;
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::string text(size, '\0');
    f.read(&text[0], (std::streamsize)size);
    JsonValue root;
    if (!jsonParse(text.c_str(), text.size(), root)) {
        AE_WARN("[Tex] malformed import sidecar: %s", side.c_str());
        return fallback;
    }
    TextureImportSettings s = fallback;
    s.srgb = root.flag("srgb", s.srgb);
    s.normalMap = root.flag("normalMap", s.normalMap);
    s.compress = root.flag("compress", s.compress);
    s.maxSize = root.integer("maxSize", s.maxSize);
    s.mipBias = root.integer("mipBias", s.mipBias);
    if (s.normalMap) s.srgb = false; // normals are data, never sRGB
    return s;
}

void setTextureMipBias(int bias) { g_mipBias = bias < 0 ? 0 : bias; }
int textureMipBias() { return g_mipBias; }

void setTextureCacheDir(const std::string& dir) {
    g_texCacheDir = dir;
    if (!dir.empty()) {
        // Intermediate/ may not exist yet; create both levels.
        size_t slash = dir.find_last_of("\\/");
        if (slash != std::string::npos) CreateDirectoryA(dir.substr(0, slash).c_str(), nullptr);
        CreateDirectoryA(dir.c_str(), nullptr);
    }
}

uint64_t contentHash64(const uint8_t* data, size_t size) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= 1099511628211ull;
    }
    return h ? h : 1; // 0 is the "no cache" sentinel
}

void Texture2D::createCompressed(int w, int h, const uint8_t* rgba, bool srgb,
                                 uint64_t contentHash) {
    TextureImportSettings s;
    s.srgb = srgb;
    createImported(w, h, rgba, s, contentHash);
}

void Texture2D::createImported(int w, int h, const uint8_t* rgba,
                               const TextureImportSettings& s, uint64_t contentHash) {
    bc5_ = false;

    // maxSize: downscale the source before anything else.
    MipLevel src = downscaleTo(w, h, rgba, s.maxSize);
    w = src.w;
    h = src.h;
    rgba = src.rgba.data();

    // Tiny textures (and opt-outs) gain nothing from block compression.
    if (!s.compress || w < 8 || h < 8) {
        create(w, h, rgba, s.srgb);
        return;
    }

    const int dropMips = s.mipBias + g_mipBias;

    // ---- cache hit: upload the pre-encoded blob directly ----
    std::string cacheFile = cacheFileFor(s.hashInto(contentHash));
    if (!cacheFile.empty()) {
        std::ifstream f(cacheFile, std::ios::binary);
        AetexHeader hd{};
        if (f && f.read((char*)&hd, sizeof(hd)) && !std::memcmp(hd.magic, "AETX", 4) &&
            hd.version == 1) {
            // Streaming budget: skip the `dropMips` biggest levels and upload
            // the rest as a smaller texture (the cache blob keeps every level,
            // so raising the budget later costs no re-encode).
            int skip = std::min(dropMips, hd.mips - 1);
            std::vector<uint8_t> blob;
            bool ok = true, created = false;
            for (int l = 0; l < hd.mips && ok; ++l) {
                int32_t dims[2];
                uint32_t bytes = 0;
                f.read((char*)dims, sizeof(dims));
                f.read((char*)&bytes, sizeof(bytes));
                blob.resize(bytes);
                f.read((char*)blob.data(), bytes);
                ok = f.good();
                if (!ok || l < skip) continue;
                if (!created) {
                    makeTexture(tex_, dims[0], dims[1], hd.mips - skip,
                                tagToFormat(hd.glFormat));
                    created = true;
                }
                rhi::uploadTexture2DCompressed(tex_, l - skip, dims[0], dims[1], bytes,
                                               blob.data());
            }
            if (ok && created) {
                bc5_ = hd.glFormat == kTagBC5;
                rhi::SamplerDesc smp;
                smp.anisotropy = 8.0f;
                rhi::setSampler(tex_, smp);
                return;
            }
            destroy(); // truncated cache file: fall through and re-encode
        }
    }

    // ---- encode: normal maps use BC5; else an alpha scan picks BC1 vs BC3 ----
    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    bool alpha = false;
    if (!s.normalMap)
        for (size_t i = 3; i < (size_t)w * h * 4; i += 4)
            if (rgba[i] < 250) { alpha = true; break; }
    uint32_t fmt = s.normalMap ? kTagBC5
                              : alpha ? (s.srgb ? kTagBC3s : kTagBC3)
                                      : (s.srgb ? kTagBC1s : kTagBC1);

    std::vector<MipLevel> mips = buildMips(w, h, rgba);
    std::vector<std::vector<uint8_t>> levels;
    size_t totalBytes = 0;
    for (const auto& m : mips) {
        levels.push_back(s.normalMap ? compressLevelBC5(m) : compressLevel(m, alpha));
        totalBytes += levels.back().size();
    }

    int skip = std::min(dropMips, (int)mips.size() - 1);
    makeTexture(tex_, mips[skip].w, mips[skip].h, (int)mips.size() - skip, tagToFormat(fmt));
    for (size_t l = skip; l < mips.size(); ++l)
        rhi::uploadTexture2DCompressed(tex_, (int)l - skip, mips[l].w, mips[l].h,
                                       levels[l].size(), levels[l].data());
    bc5_ = s.normalMap;
    rhi::SamplerDesc smp;
    smp.anisotropy = 8.0f;
    rhi::setSampler(tex_, smp);

    QueryPerformanceCounter(&t1);
    double ms = (double)(t1.QuadPart - t0.QuadPart) * 1000.0 / freq.QuadPart;
    AE_LOG("[Tex] %dx%d -> %s %.0f KB (rgba+mips %.0f KB, encode %.0f ms)%s", w, h,
           s.normalMap ? "BC5" : alpha ? "BC3" : "BC1", totalBytes / 1024.0,
           (double)w * h * 4 * 1.33 / 1024.0, ms, skip ? " [mip-biased]" : "");

    // ---- write the cache blob ----
    if (!cacheFile.empty()) {
        std::ofstream f(cacheFile, std::ios::binary);
        if (f) {
            AetexHeader hd{{'A', 'E', 'T', 'X'}, 1, w, h, (int32_t)mips.size(), fmt};
            f.write((char*)&hd, sizeof(hd));
            for (size_t l = 0; l < mips.size(); ++l) {
                int32_t dims[2] = {mips[l].w, mips[l].h};
                uint32_t bytes = (uint32_t)levels[l].size();
                f.write((char*)dims, sizeof(dims));
                f.write((char*)&bytes, sizeof(bytes));
                f.write((char*)levels[l].data(), bytes);
            }
        }
    }
}

void Texture2D::create(int w, int h, const uint8_t* rgba, bool srgb) {
    int levels = 1 + (int)std::floor(std::log2((double)(w > h ? w : h)));
    makeTexture(tex_, w, h, levels,
                srgb ? rhi::TexFormat::SRGBA8 : rhi::TexFormat::RGBA8);
    rhi::uploadTexture2D(tex_, 0, w, h, rgba);
    rhi::generateMips(tex_);
    rhi::SamplerDesc smp;
    smp.anisotropy = 8.0f;
    rhi::setSampler(tex_, smp);
}

void Texture2D::destroy() { rhi::destroyTexture(tex_); }

// ---- procedural noise ------------------------------------------------------

static float hash2(int x, int y, int seed) {
    uint32_t h = (uint32_t)(x * 374761393 + y * 668265263 + seed * 2246822519u);
    h = (h ^ (h >> 13)) * 1274126177u;
    return (float)((h ^ (h >> 16)) & 0xFFFFFF) / (float)0xFFFFFF;
}

// Tileable value noise over a periodic lattice.
static float valueNoise(float x, float y, int period, int seed) {
    int x0 = (int)std::floor(x), y0 = (int)std::floor(y);
    float fx = x - x0, fy = y - y0;
    fx = fx * fx * (3 - 2 * fx);
    fy = fy * fy * (3 - 2 * fy);
    auto wrap = [period](int v) { return ((v % period) + period) % period; };
    float v00 = hash2(wrap(x0), wrap(y0), seed);
    float v10 = hash2(wrap(x0 + 1), wrap(y0), seed);
    float v01 = hash2(wrap(x0), wrap(y0 + 1), seed);
    float v11 = hash2(wrap(x0 + 1), wrap(y0 + 1), seed);
    return lerpf(lerpf(v00, v10, fx), lerpf(v01, v11, fx), fy);
}

static float fbm(float x, float y, int octaves, int period, int seed) {
    float sum = 0, amp = 0.5f, freq = 1.0f;
    for (int i = 0; i < octaves; ++i) {
        sum += amp * valueNoise(x * freq, y * freq, (int)(period * freq), seed + i * 131);
        amp *= 0.5f;
        freq *= 2.0f;
    }
    return sum;
}

// Sobel-filtered height field -> tangent-space normal map (Y up in texture space).
static void heightToNormal(const std::vector<float>& height, int size, float strength,
                           std::vector<uint8_t>& outRGBA) {
    auto H = [&](int x, int y) {
        x = ((x % size) + size) % size;
        y = ((y % size) + size) % size;
        return height[y * size + x];
    };
    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float dx = (H(x + 1, y) - H(x - 1, y)) * strength;
            float dy = (H(x, y + 1) - H(x, y - 1)) * strength;
            Vec3 n = normalize(Vec3(-dx, -dy, 1.0f));
            size_t i = (size_t)(y * size + x) * 4;
            outRGBA[i + 0] = (uint8_t)((n.x * 0.5f + 0.5f) * 255.0f);
            outRGBA[i + 1] = (uint8_t)((n.y * 0.5f + 0.5f) * 255.0f);
            outRGBA[i + 2] = (uint8_t)((n.z * 0.5f + 0.5f) * 255.0f);
            outRGBA[i + 3] = 255;
        }
    }
}

static uint8_t toByte(float v) { return (uint8_t)(clampf(v, 0.0f, 1.0f) * 255.0f); }

void makeTileTextures(MaterialTextures& out, int size) {
    std::vector<uint8_t> albedo((size_t)size * size * 4);
    std::vector<uint8_t> orm((size_t)size * size * 4);
    std::vector<uint8_t> normalMap((size_t)size * size * 4);
    std::vector<float> height((size_t)size * size);

    const int tiles = 4;             // tiles per texture repeat
    const float groutWidth = 0.025f; // in tile UV units

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float u = (float)x / size, v = (float)y / size;
            float tu = u * tiles, tv = v * tiles;
            float fu = tu - std::floor(tu), fv = tv - std::floor(tv);
            int tileX = (int)std::floor(tu), tileY = (int)std::floor(tv);

            // Distance to grout line.
            float edge = std::min(std::min(fu, 1.0f - fu), std::min(fv, 1.0f - fv));
            float grout = clampf(edge / groutWidth, 0.0f, 1.0f);

            // Marble veining: domain-warped FBM.
            float warp = fbm(u * 24, v * 24, 4, 24, 7);
            float vein = fbm(u * 6 + warp * 1.6f, v * 6 + warp * 1.6f, 5, 6, 3);
            float marble = std::pow(std::fabs(std::sin((u * 4 + vein * 3.0f) * PI)), 0.35f);

            // Per-tile tonal variation.
            float tileTone = 0.88f + 0.12f * hash2(tileX, tileY, 42);

            Vec3 stoneA(0.82f, 0.80f, 0.76f);
            Vec3 stoneB(0.45f, 0.44f, 0.46f);
            Vec3 col = (stoneA * marble + stoneB * (1.0f - marble)) * tileTone;
            Vec3 groutCol(0.30f, 0.29f, 0.28f);
            col = col * grout + groutCol * (1.0f - grout);

            float rough = lerpf(0.85f, lerpf(0.35f, 0.15f, marble), grout);
            rough += (fbm(u * 48, v * 48, 3, 48, 91) - 0.5f) * 0.08f;
            float h = grout * 0.5f + marble * 0.03f * grout;

            size_t i = (size_t)(y * size + x) * 4;
            albedo[i + 0] = toByte(col.x);
            albedo[i + 1] = toByte(col.y);
            albedo[i + 2] = toByte(col.z);
            albedo[i + 3] = 255;
            orm[i + 0] = toByte(lerpf(0.6f, 1.0f, grout)); // grout lines slightly occluded
            orm[i + 1] = toByte(rough);
            orm[i + 2] = 0;                                 // dielectric
            orm[i + 3] = 255;
            height[(size_t)y * size + x] = h;
        }
    }
    heightToNormal(height, size, 2.5f, normalMap);
    out.albedo.create(size, size, albedo.data(), true);
    out.orm.create(size, size, orm.data(), false);
    out.normal.create(size, size, normalMap.data(), false);
}

void makeRustedMetalTextures(MaterialTextures& out, int size) {
    std::vector<uint8_t> albedo((size_t)size * size * 4);
    std::vector<uint8_t> orm((size_t)size * size * 4);
    std::vector<uint8_t> normalMap((size_t)size * size * 4);
    std::vector<float> height((size_t)size * size);

    for (int y = 0; y < size; ++y) {
        for (int x = 0; x < size; ++x) {
            float u = (float)x / size, v = (float)y / size;

            // Rust mask: thresholded FBM with sharpened transition.
            float mask = fbm(u * 8, v * 8, 6, 8, 17);
            float rust = clampf((mask - 0.48f) * 6.0f, 0.0f, 1.0f);

            float rustDetail = fbm(u * 32, v * 32, 4, 32, 55);
            Vec3 paint(0.28f, 0.35f, 0.42f); // weathered blue-grey paint
            paint = paint * (0.85f + 0.3f * fbm(u * 16, v * 16, 3, 16, 23));
            Vec3 rustA(0.42f, 0.18f, 0.08f);
            Vec3 rustB(0.60f, 0.32f, 0.12f);
            Vec3 rustCol = rustA * (1.0f - rustDetail) + rustB * rustDetail;

            Vec3 col = paint * (1.0f - rust) + rustCol * rust;

            float rough = lerpf(0.35f, 0.92f, rust) + (rustDetail - 0.5f) * 0.1f;
            float metal = 1.0f - rust; // painted metal reads metallic where paint is intact
            float h = -rust * 0.6f + rustDetail * rust * 0.3f;

            size_t i = (size_t)(y * size + x) * 4;
            albedo[i + 0] = toByte(col.x);
            albedo[i + 1] = toByte(col.y);
            albedo[i + 2] = toByte(col.z);
            albedo[i + 3] = 255;
            orm[i + 0] = toByte(1.0f - rust * 0.4f);
            orm[i + 1] = toByte(rough);
            orm[i + 2] = toByte(metal);
            orm[i + 3] = 255;
            height[(size_t)y * size + x] = h;
        }
    }
    heightToNormal(height, size, 3.0f, normalMap);
    out.albedo.create(size, size, albedo.data(), true);
    out.orm.create(size, size, orm.data(), false);
    out.normal.create(size, size, normalMap.data(), false);
}

} // namespace ae
