// Aether Engine — render packet: the flat draw list the renderer consumes.
//
// This is deliberately NOT the engine's scene graph. The engine layer
// (src/engine) owns a World of Entity actors with components, and rebuilds
// this RenderScene each frame. Keeping the render packet dumb and flat lets
// the renderer stay decoupled from gameplay/simulation.
#pragma once
#include "../core/math3d.h"
#include "../render/mesh.h"
#include "../render/texture.h"
#include <string>
#include <vector>

namespace ae {

struct MaterialGraphAsset; // render/material_graph.h — node-authored shading

struct Material {
    Vec4 baseColor{1, 1, 1, 1};
    float metallic = 0.0f;
    float roughness = 0.5f;
    Vec3 emissive{0, 0, 0};        // factor; multiplies emissiveTex when present
    float uvScale = 1.0f;
    // Derive UVs from world position instead of the mesh's own UVs (uvScale
    // then reads as tiles-per-metre). For level geometry made of stretched
    // primitives, where per-face 0..1 UVs would smear the texture.
    bool worldUV = false;

    // Optional textures as opaque rhi ids (0 = unused; bind via
    // rhi::bindTexture). Factors multiply texture values (glTF).
    unsigned albedoTex = 0;        // sRGB
    unsigned normalTex = 0;        // tangent-space, linear
    unsigned mrTex = 0;            // G = roughness, B = metallic (glTF)
    unsigned occlusionTex = 0;     // R = ambient occlusion (may alias mrTex)
    unsigned emissiveTex = 0;      // sRGB

    float normalScale = 1.0f;
    float occlusionStrength = 1.0f;
    float alphaCutoff = -1.0f;     // >= 0 enables alpha-mask discard
    bool doubleSided = false;
    // Alpha-blended (glTF alphaMode BLEND): drawn in the sorted forward pass
    // after opaques, using baseColor.a * albedo alpha as opacity. Blended
    // surfaces don't cast shadows or receive SSAO.
    bool blend = false;

    // Node-authored shading: when set, the compiled graph's shader variant
    // replaces the standard uniform+texture inputs (graphPath is the serialized
    // asset reference; graph is resolved by MeshRenderer::onDeserialized).
    std::string graphPath;
    const MaterialGraphAsset* graph = nullptr;

    // Character-grade skin shading (pre-integrated subsurface scattering).
    float subsurface = 0.0f;       // 0 = standard lambert, 1 = full SSS diffusion
    float sssCurvature = 0.0f;     // 0 = derive from geometry, else fixed LUT row
    float translucency = 0.0f;     // light transmission through thin features
    Vec3 sssTint{1.0f, 0.30f, 0.20f}; // transmission color (blood-red for skin)

    void setTextures(const MaterialTextures& t) {
        albedoTex = t.albedo.id();
        normalTex = t.normal.id();
        mrTex = t.orm.id();
        occlusionTex = t.orm.id(); // procedural ORM packs AO in R
    }
};

// One draw submission. Produced by the engine's MeshRenderer/Model components.
struct Renderable {
    const Mesh* mesh = nullptr;
    Material material;
    Mat4 transform = Mat4::identity();
    bool castShadow = true;
    // Skinning palette (model-space joint matrices); null = rigid.
    const std::vector<Mat4>* jointMatrices = nullptr;
    // Local-space AABB (mesh space, pre-transform) for frustum culling and
    // editor picking. min == max means "no bounds known" — never culled.
    Vec3 boundsMin{0, 0, 0};
    Vec3 boundsMax{0, 0, 0};
    // Distance culling: skipped beyond this camera distance (0 = never).
    float maxDistance = 0.0f;
};

// A local light. type: 0 = point, 1 = spot, 2 = directional. Point/spot use
// position + range; spot/directional use direction (the beam axis); spot uses
// the cone cosines (cosInner..cosOuter for the soft edge).
struct Light {
    Vec3 position{0, 0, 0};
    Vec3 color{1, 1, 1};              // radiance at 1m
    Vec3 direction{0, -1, 0};         // beam axis (spot/directional)
    int type = 0;                     // LightType
    float range = 12.0f;
    float cosInner = 0.9f;
    float cosOuter = 0.8f;
};

// One live particle, ready to draw (world position, current size/color).
struct ParticlePoint {
    Vec3 pos;
    float size = 0.2f;
    Vec4 color{1, 1, 1, 1}; // HDR RGB + alpha (bloom picks up >1 values)
    float rot = 0.0f;       // billboard roll (radians, in the camera plane)
    float frame = 0.0f;     // flipbook cell (used when the batch has a grid)
};

// A particle emitter's output for this frame. Additive batches glow
// order-independently; alpha batches are depth-sorted by the renderer.
struct ParticleBatch {
    std::vector<ParticlePoint> points;
    bool additive = true;
    unsigned texture = 0;        // sprite texture (0 = procedural soft disc)
    int flipCols = 1, flipRows = 1; // flipbook grid inside `texture`
    float softFade = 0.0f;       // depth-fade distance in meters (0 = off)
};

struct RenderScene {
    std::vector<Renderable> entities;
    std::vector<Light> lights;
    std::vector<ParticleBatch> particles;
    Vec3 sunDir = normalize(Vec3(0.35f, 0.55f, 0.25f)); // towards the sun
    float skyIntensity = 22.0f;

    // Atmosphere, per scene. A sealed tech base and the inside of a volcano
    // want completely different air, and that is a property of the level, not
    // of the renderer. Negative = keep the renderer's default.
    float fogDensity = -1.0f;
    float fogHeightFalloff = -1.0f;
    Vec3 fogColor{-1, -1, -1};   // negative = derive from the sky, as before
    // Strength of the ray-marched volumetric medium (0 = off for this scene,
    // negative = use the renderer's default). Shares the fog's density, colour
    // and height falloff — this only scales how much light it scatters.
    float volumetricIntensity = -1.0f;

    void clear() {
        entities.clear();
        lights.clear();
        particles.clear();
    }
};

// Basis-form camera: position + forward/up directions. The engine's
// CameraComponent fills this from an entity's world transform each frame.
struct Camera {
    Vec3 position{0, 2, 8};
    Vec3 forwardDir{0, 0, -1};
    Vec3 upDir{0, 1, 0};
    float fovY = 55.0f;
    float zNear = 0.1f;
    float zFar = 300.0f;

    Vec3 forward() const { return normalize(forwardDir); }
    Vec3 right() const { return normalize(cross(forward(), upDir)); }
    Mat4 view() const { return lookAt(position, position + forward(), upDir); }
    Mat4 proj(float aspect) const { return perspective(radians(fovY), aspect, zNear, zFar); }
};

} // namespace ae
