// Aether Engine — GPU textures + CPU-side procedural material synthesis.
// ORM convention follows glTF: R = ambient occlusion, G = roughness, B = metallic.
#pragma once
#include "../rhi/rhi.h"
#include <string>
#include <vector>
#include <cstdint>

namespace ae {

class Texture2D {
public:
    // RGBA8 upload with full mip chain and anisotropic filtering.
    void create(int width, int height, const uint8_t* rgba, bool srgb);
    // BC-compressed upload (BC1 opaque / BC3 with alpha): CPU mip chain +
    // stb_dxt encode, 4-8x less VRAM than RGBA8. `contentHash` (FNV-1a of the
    // source file bytes; 0 = skip caching) keys a disk cache in the project's
    // Intermediate/TextureCache so the encode cost is paid once per asset.
    void createCompressed(int width, int height, const uint8_t* rgba, bool srgb,
                          uint64_t contentHash = 0);
    void destroy();
    // Opaque rhi texture id (bind with rhi::bindTexture). Materials store these
    // ids raw for cheap comparison/batching.
    unsigned id() const { return tex_.id; }

private:
    rhi::TextureHandle tex_;
};

// Where createCompressed caches encoded textures ("" disables the cache).
// AssetLibrary::init points this at <project>/Intermediate/TextureCache.
void setTextureCacheDir(const std::string& dir);

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
