// Aether Engine — GPU textures + CPU-side procedural material synthesis.
// ORM convention follows glTF: R = ambient occlusion, G = roughness, B = metallic.
#pragma once
#include "../rhi/rhi.h"
#include <string>
#include <vector>
#include <cstdint>

namespace ae {

// How one source image should be imported. Defaults match the historical
// behaviour; a `<image>.import.json` sidecar next to the file overrides them
// (see loadImportSettings), so import choices live in the project, in git,
// next to the asset — no editor database.
struct TextureImportSettings {
    bool srgb = true;         // color textures; false for data (normal/ORM)
    bool normalMap = false;   // encode BC5 (RG) + reconstruct Z in the shader
    bool compress = true;     // false = plain RGBA8
    int maxSize = 0;          // downscale so the longest edge fits (0 = source)
    // Drops the `mipBias` largest mip levels at upload (the streaming budget
    // knob: 1 = half res in VRAM, 2 = quarter). Source stays untouched.
    int mipBias = 0;

    // Distinguishes cache entries built with different settings.
    uint64_t hashInto(uint64_t contentHash) const;
};

// Reads `<imagePath>.import.json` (e.g. assets/rock_n.png.import.json).
// Missing/malformed file = `fallback` unchanged. Keys are the field names:
//   { "srgb": false, "normalMap": true, "maxSize": 1024, "mipBias": 0 }
TextureImportSettings loadImportSettings(const std::string& imagePath,
                                         TextureImportSettings fallback = {});

class Texture2D {
public:
    // RGBA8 upload with full mip chain and anisotropic filtering.
    void create(int width, int height, const uint8_t* rgba, bool srgb);
    // BC-compressed upload (BC1 opaque / BC3 alpha / BC5 normal maps): CPU mip
    // chain + stb_dxt encode, 4-8x less VRAM than RGBA8. `contentHash` (FNV-1a
    // of the source file bytes; 0 = skip caching) keys a disk cache in the
    // project's Intermediate/TextureCache so the encode is paid once per asset.
    void createCompressed(int width, int height, const uint8_t* rgba, bool srgb,
                          uint64_t contentHash = 0);
    // Full-control import (sidecar settings): BC5 for normal maps, max-size
    // downscale, mip-bias streaming budget.
    void createImported(int width, int height, const uint8_t* rgba,
                        const TextureImportSettings& s, uint64_t contentHash = 0);
    void destroy();
    // Opaque rhi texture id (bind with rhi::bindTexture). Materials store these
    // ids raw for cheap comparison/batching.
    unsigned id() const { return tex_.id; }
    // True when uploaded as BC5 (the shader must rebuild Z from RG).
    bool isBC5() const { return bc5_; }

private:
    rhi::TextureHandle tex_;
    bool bc5_ = false;
};

// Where createCompressed caches encoded textures ("" disables the cache).
// AssetLibrary::init points this at <project>/Intermediate/TextureCache.
void setTextureCacheDir(const std::string& dir);

// Global streaming budget: added to every import's mipBias, so a weak-GPU tier
// (or a game setting) can halve texture VRAM without touching any asset.
void setTextureMipBias(int bias);
int textureMipBias();

// FNV-1a 64 content hash (cache keys for imported images).
uint64_t contentHash64(const uint8_t* data, size_t size);

struct MaterialTextures {
    Texture2D albedo;   // sRGB
    Texture2D normal;   // linear, tangent-space
    Texture2D orm;      // linear
};

// Marble-veined polished tiles for the floor.
void makeTileTextures(MaterialTextures& out, int size);
// Weathered painted metal with rust breakthrough.
void makeRustedMetalTextures(MaterialTextures& out, int size);

} // namespace ae
