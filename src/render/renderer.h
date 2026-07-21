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
    // Volumetric lighting: ray-marched in-scattering that is actually shadowed,
    // so a lamp casts a visible cone and a doorway throws a shaft. Replaces the
    // analytic height fog when on (they model the same medium; running both
    // double-counts it). Steps trade cost against banding — the march is
    // dithered per pixel and half-res, so TAA cleans up what is left.
    bool volumetric = true;
    int volumetricSteps = 28;
    float volumetricIntensity = 1.0f;
    float volumetricAnisotropy = 0.62f; // Henyey-Greenstein g (forward scatter)
    float volumetricMaxDistance = 140.0f;

    // Screen-space reflections: marches the mirror ray through the depth buffer
    // and blends what the screen shows over the environment probe. Only worth
    // running below maxRoughness — a blurry lobe cannot be traced honestly in
    // screen space, and the probe already looks right there.
    bool ssr = true;
    int ssrSteps = 32;
    int ssrRefineSteps = 5;
    float ssrMaxRoughness = 0.48f;
    float ssrMaxDistance = 34.0f;  // view-space metres
    float ssrThickness = 1.1f;     // depth tolerance for "the ray is behind this"
    float ssrStrength = 1.0f;
    // Auto-exposure: measure the frame's log-average luminance and adapt the
    // exposure toward a middle-grey key, so a scene stays readable walking from
    // sunlight into a cave with no per-level tuning. `exposure` then acts as a
    // relative bias instead of an absolute multiplier.
    bool autoExposure = false;
    float autoExposureSpeedUp = 3.0f;   // adaptation rate when brightening
    float autoExposureSpeedDown = 1.0f; // slower when darkening (like an eye)
    // Temporal anti-aliasing: sub-pixel jitter + reprojected history. Cleans up
    // geometry edges and specular shimmer far better than FXAA alone (which
    // still runs after it).
    bool taa = true;
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
    // Number of nearest spot lights that cast a real shadow map (0 disables).
    // Each adds one depth re-raster of the scene, so it's capped and tier-gated.
    int spotShadows = 4;
    // Number of nearest point lights that cast an omnidirectional cube shadow
    // (0 disables). Each is 6 face re-rasters, so it's small and tier-gated.
    int pointShadows = 2;
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
                PassTaa, PassBloom, PassTonemap, PassFxaa, PassCount };
    float msPass[PassCount] = {};
    static const char* passName(int i) {
        static const char* names[PassCount] = {"env",   "shadow", "geometry", "ssao",
                                               "composite", "forward", "taa", "bloom",
                                               "tonemap", "fxaa"};
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
    static constexpr int kMaxSpotShadows = 4;
    static constexpr int kSpotShadowRes = 1024;
    static constexpr int kMaxPointShadows = 2;
    static constexpr int kPointShadowRes = 512;
    static constexpr int kMaxLights = 8;
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
    // Renders the scene's shadow casters with the given depth/distance program
    // + its uLightMat (shared by the cascade, spot, and point passes).
    void drawDepthCasters(const RenderScene& scene, const Camera& camera, Shader& prog);
    // Per-frame depth maps for the nearest shadow-casting spot lights.
    void spotShadowPass(const RenderScene& scene, const Camera& camera);
    // Per-frame linear-distance cubes for the nearest shadow-casting point lights.
    void pointShadowPass(const RenderScene& scene, const Camera& camera);
    void geometryPass(const RenderScene& scene, const Camera& camera);
    void ssaoPass(const Camera& camera);
    // Half-res ray-marched in-scattering (sun + local lights), consumed by the
    // composite pass through a depth-aware upsample.
    void volumetricPass(const RenderScene& scene, const Camera& camera);
    // Half-res mirror-ray march + a full-res resolve that blends it over the
    // environment probe already present in the frame.
    void ssrPass(const Camera& camera);
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
    // Reduces the HDR frame to a 1x1 log-average luminance and adapts it
    // toward that target over time (ping-ponged, so it survives frames).
    void autoExposurePass(float dt);
    // Resolves hdrTex_ + the history buffer into taaTex_, then swaps it in as
    // the HDR image the rest of the chain reads.
    void taaPass(const Camera& camera);
    void bloomPass();
    void tonemapPass(float time);
    void fxaaPass();

    // Sub-pixel projection jitter for TAA (Halton 2,3), in NDC. Zero when TAA
    // is off. Every pass must use the SAME projection, so they all go through
    // projFor() rather than calling camera.proj() directly.
    Vec2 jitterNDC() const;
    Mat4 projFor(const Camera& camera) const;

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
    Shader shPBR_, shShadow_, shPointDepth_, shSkyCapture_, shBackground_;
    Shader shIrradiance_, shPrefilter_, shBrdf_, shSkinLUT_;
    Shader shSSAO_, shSSAOBlur_, shVolumetric_, shSSR_, shSSRResolve_, shComposite_;
    Shader shBloomDown_, shBloomUp_, shTonemap_, shFXAA_;
    Shader shParticle_, shDebug_;
    Shader shLumDown_, shLumAdapt_, shTAA_;

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

    // Local spot-light shadows (perspective depth, one array layer per caster).
    rhi::TextureHandle spotShadowTex_;
    rhi::FramebufferHandle spotShadowFBO_;
    Mat4 spotShadowMat_[kMaxSpotShadows];
    int spotShadowCount_ = 0;
    // The <= kMaxLights local lights actually shaded this frame, chosen by
    // selectLights() from the whole scene. Every per-light array below is
    // indexed against THIS list, not scene.lights.
    std::vector<Light> activeLights_;
    // This frame's resolved fog (scene override, else settings / sky-derived).
    float sceneFogDensity_ = 0.008f;
    float sceneFogFalloff_ = 0.10f;
    Vec3 sceneFogColor_{0, 0, 0};
    float sceneVolumetric_ = 1.0f; // resolved intensity (0 = off this frame)
    rhi::TextureHandle volumetricTex_{};
    rhi::FramebufferHandle volumetricFBO_{};
    rhi::TextureHandle gSpecular_{};   // rgb = specular weight, a = roughness
    rhi::TextureHandle ssrTex_{};      // half-res hit colour + confidence
    rhi::FramebufferHandle ssrFBO_{};
    void selectLights(const RenderScene& scene, const Camera& camera);

    int lightSpotLayer_[kMaxLights] = {}; // per-light spot layer, -1 = none

    // Local point-light shadows (linear-distance cubes, one per caster).
    rhi::TextureHandle pointCube_[kMaxPointShadows];
    rhi::TextureHandle pointDepthTex_; // shared scratch depth for the face renders
    rhi::FramebufferHandle pointShadowFBO_;
    int pointShadowCount_ = 0;
    int lightPointCube_[kMaxLights] = {}; // per-light cube index, -1 = none

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

    // Auto-exposure: a log-luminance reduction pyramid down to 1x1, plus a
    // ping-ponged pair holding the adapted value across frames.
    static constexpr int kLumMips = 8;
    rhi::FramebufferHandle lumFBO_[kLumMips];
    rhi::TextureHandle lumTex_[kLumMips];
    int lumW_[kLumMips] = {}, lumH_[kLumMips] = {};
    int lumMipCount_ = 0;
    rhi::FramebufferHandle adaptFBO_[2];
    rhi::TextureHandle adaptTex_[2];
    int adaptCur_ = 0;
    bool exposureReset_ = true;

    // TAA: resolve target + last frame's resolved image, and the previous
    // frame's (jittered) view-projection for reprojection.
    rhi::FramebufferHandle taaFBO_, historyFBO_;
    rhi::TextureHandle taaTex_, historyTex_;
    Mat4 prevViewProj_ = Mat4::identity();
    bool taaReset_ = true;
    uint32_t frameIndex_ = 0;
    // The HDR image downstream passes (auto-exposure, bloom, tonemap) read:
    // hdrTex_ normally, or the TAA resolve when TAA ran.
    rhi::TextureHandle hdrSource_;

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
    rhi::BufferHandle tonemapUBO_; // exposure/bloom/time, binding point 2 (UBO-uniform migration)

    rhi::FramebufferHandle outputFBO_; // final image (null = backbuffer)

    // Frame-uniform bookkeeping across shader variants (see applyFrameUniforms).
    float frameTime_ = 0.0f;
    float prevTime_ = 0.0f; // for the exposure-adaptation delta
    bool frameForward_ = false;              // forward pass: fog + uForwardPass=1
    std::vector<unsigned> preppedPrograms_;  // programs already prepped this pass
};

} // namespace ae
