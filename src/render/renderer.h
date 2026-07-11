// Aether Engine — physically-based HDR renderer.
//
// Frame graph:
//   1. Cascaded shadow maps    (4 stabilized cascades, depth-only)
//   2. Opaque geometry (MRT)   direct light / IBL ambient / view normals + depth
//   3. Sky background          far-plane pass from the captured environment cubemap
//   4. SSAO + blur
//   5. Composite               direct + ambient*AO + analytic height fog
//   6. Bloom                   Jimenez 13-tap down / tent up, soft threshold
//   7. Tonemap                 exposure, ACES, vignette, sRGB, dither
//   8. FXAA                    to the backbuffer
//
// The environment (Nishita atmosphere) is captured to a cubemap and convolved
// into diffuse irradiance + GGX-prefiltered specular whenever the sun moves.
#pragma once
#include "shader.h"
#include "mesh.h"
#include "../scene/scene.h"
#include <vector>

namespace ae {

struct RenderSettings {
    float exposure = 0.75f;
    float bloomStrength = 0.06f;
    float fogDensity = 0.008f;
    float fogHeightFalloff = 0.10f;
    bool vsync = true;
    bool frustumCulling = true; // camera-frustum AABB culling in the geometry pass
    // Batches identical (mesh, material) draws into one instanced call (also
    // in the shadow pass). Off = one draw per renderable (A/B comparisons).
    bool instancing = true;
    // Coarse per-pass GPU timing (glFinish around every pass — slows the frame
    // itself, so only for diagnostics). Results land in FrameStats::msPass.
    bool profileGpu = false;
    // Internal render resolution multiplier for the editor viewport (panel
    // upscales). 1.0 = native.
    float renderScale = 1.0f;
    // Number of shadow cascades actually rendered (1..kNumCascades). The
    // shadow pass re-rasterizes the whole scene once per cascade, so this is
    // the dominant cost on GPUs with high per-draw overhead (iGPUs); dropping
    // to 2 nearly halves it. Fewer cascades = shorter shadow-cast distance.
    int shadowCascades = 4;
};

// Per-frame counters, reset at the top of render().
struct FrameStats {
    int drawCalls = 0;   // geometry-pass draws submitted
    int culled = 0;      // geometry-pass draws skipped by frustum culling
    int shadowDraws = 0; // shadow-pass draws across all cascades
    int lights = 0;
    int particles = 0;   // particle billboards drawn this frame
    int instancedDraws = 0;   // instanced draw calls issued
    int instancedObjects = 0; // renderables covered by those calls

    // Per-pass GPU milliseconds, filled when RenderSettings::profileGpu.
    enum Pass { PassEnv, PassShadow, PassGeom, PassSsao, PassComposite, PassForward,
                PassBloom, PassTonemap, PassFxaa, PassCount };
    float msPass[PassCount] = {};
    static const char* passName(int i) {
        static const char* names[PassCount] = {"env",   "shadow", "geometry", "ssao",
                                               "composite", "forward", "bloom", "tonemap",
                                               "fxaa"};
        return names[i];
    }
};

// Downtiers settings for weak GPUs after the GL context exists: integrated /
// software renderers get fewer shadow cascades (the shadow pass re-rasterizes
// the scene per cascade and dominates there) and a reduced 3D render scale.
// Explicit user/game settings should be applied after this.
void applyGpuAutoTier(RenderSettings& s);

class Renderer {
public:
    static constexpr int kNumCascades = 4;
    static constexpr int kShadowRes = 2048;
    static constexpr int kEnvRes = 256;
    static constexpr int kIrradianceRes = 32;
    static constexpr int kPrefilterRes = 128;
    static constexpr int kPrefilterMips = 5;
    static constexpr int kBloomMips = 6;

    bool init(int width, int height);
    void shutdown();
    void resize(int width, int height);
    void render(const RenderScene& scene, const Camera& camera, float time);

    // Redirect the final image to a framebuffer (e.g. an editor viewport
    // texture). The null handle = the window backbuffer (default).
    void setOutputFramebuffer(rhi::FramebufferHandle fbo) { outputFBO_ = fbo; }

    RenderSettings settings;
    FrameStats stats;

private:
    void createWindowTargets();
    void destroyWindowTargets();
    void createEnvironmentResources();
    void updateEnvironment(const Vec3& sunDir, float intensity);

    void shadowPass(const RenderScene& scene, const Camera& camera);
    void geometryPass(const RenderScene& scene, const Camera& camera);
    void ssaoPass(const Camera& camera);
    void compositePass(const RenderScene& scene, const Camera& camera);
    // Alpha-blended surfaces, drawn forward into the HDR buffer after composite:
    // depth-tested against the opaque depth (no write), sorted back-to-front.
    void forwardPass(const RenderScene& scene, const Camera& camera);
    // Camera-facing particle billboards into the HDR buffer (after the forward
    // pass, before bloom — HDR colors glow). Additive or premultiplied alpha.
    void particlePass(const RenderScene& scene, const Camera& camera);
    // Draws + clears the global debug-draw line collector (colliders, gizmos,
    // gameplay debug) as a translucent X-ray overlay into the HDR buffer.
    void debugPass(const Camera& camera);
    void bloomPass();
    void tonemapPass(float time);
    void fxaaPass();

    // Sets the per-draw PBR uniforms/textures and issues the draw. Shared by
    // the geometry (deferred MRT) and forward (blended) passes; `culling`
    // tracks the current GL_CULL_FACE state across consecutive draws. Picks
    // the material-graph shader variant when the material carries one.
    void submitPBRDraw(const Renderable& e, bool& culling, const RenderScene& scene,
                       const Camera& camera);
    // One instanced draw for a batch of identical (mesh, material) renderables.
    void submitInstanced(const std::vector<const Renderable*>& items, bool& culling,
                         const RenderScene& scene, const Camera& camera);
    void uploadInstanceMatrices(const std::vector<const Renderable*>& items);

    // Applies the per-frame uniforms (camera, sun, lights, cascades, fog, time)
    // to one PBR-family program. Called lazily for every distinct shader a pass
    // touches (the base uber-shader + any material-graph variants).
    void applyFrameUniforms(Shader& sh, const RenderScene& scene, const Camera& camera);

    void drawFullscreen();

    int width_ = 0, height_ = 0;

    // Shaders
    Shader shPBR_, shShadow_, shSkyCapture_, shBackground_;
    Shader shIrradiance_, shPrefilter_, shBrdf_, shSkinLUT_;
    Shader shSSAO_, shSSAOBlur_, shComposite_;
    Shader shBloomDown_, shBloomUp_, shTonemap_, shFXAA_;
    Shader shParticle_, shDebug_;

    // Particle billboards (dynamic vertex stream rebuilt per frame).
    rhi::StreamHandle particleStream_;

    // Per-instance model matrices for batched draws (std430, binding 1).
    rhi::BufferHandle instanceBuffer_;

    // Debug lines (dynamic, drawn depth-tested over the HDR frame).
    rhi::StreamHandle debugStream_;

    // Per-pass GPU timers (double-buffered; read a frame late).
    rhi::TimerHandle passQ_[2][FrameStats::PassCount];
    int passQFrame_ = 0;
    bool passQReady_[2] = {false, false};

    // Shadow
    rhi::TextureHandle shadowTex_;
    rhi::FramebufferHandle shadowFBO_;
    Mat4 cascadeMats_[kNumCascades];
    float cascadeSplits_[kNumCascades] = {};
    float cascadeTexelWorld_[kNumCascades] = {};

    // Geometry MRT
    rhi::FramebufferHandle gFBO_;
    rhi::TextureHandle gDirect_, gAmbient_, gNormal_, gDepth_;

    // SSAO
    rhi::FramebufferHandle ssaoFBO_, ssaoBlurFBO_;
    rhi::TextureHandle ssaoTex_, ssaoBlurTex_, ssaoNoiseTex_;
    Vec3 ssaoKernel_[16];

    // HDR + post
    rhi::FramebufferHandle hdrFBO_;
    rhi::TextureHandle hdrTex_;
    rhi::FramebufferHandle bloomFBO_[kBloomMips];
    rhi::TextureHandle bloomTex_[kBloomMips];
    int bloomW_[kBloomMips] = {}, bloomH_[kBloomMips] = {};
    rhi::FramebufferHandle ldrFBO_;
    rhi::TextureHandle ldrTex_;

    // Environment / IBL
    rhi::TextureHandle envCube_, irradianceCube_, prefilterCube_, brdfLUT_;
    rhi::FramebufferHandle captureFBO_;
    Vec3 cachedSunDir_{0, 0, 0};
    float cachedIntensity_ = -1.0f;
    Vec3 fogColor_{0.5f, 0.6f, 0.7f};
    Vec3 sunRadiance_{10, 10, 10};

    // Character shading + skinning
    rhi::TextureHandle skinLUT_;  // pre-integrated SSS (NdotL x curvature)
    rhi::BufferHandle jointUBO_;  // 128 mat4 palette, binding point 0

    rhi::FramebufferHandle outputFBO_; // final image (null = backbuffer)

    // Frame-uniform bookkeeping across shader variants (see applyFrameUniforms).
    float frameTime_ = 0.0f;
    bool frameForward_ = false;              // forward pass: fog + uForwardPass=1
    std::vector<unsigned> preppedPrograms_;  // programs already prepped this pass
};

} // namespace ae
