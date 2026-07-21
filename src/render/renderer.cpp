#include "renderer.h"
#include "material_graph.h"
#include "debug_draw.h"
#include "../core/log.h"
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

namespace ae {
// Blended either by the material flag or by its material graph's Output node.
static bool isBlended(const Material& m) {
    return m.blend || (m.graph && m.graph->valid && m.graph->blend);
}

// Two renderables may share one instanced draw when every GPU-visible material
// input matches (uniforms, textures, graph variant, raster state).
static bool sameBatchMaterial(const Material& a, const Material& b) {
    auto v3 = [](const Vec3& x, const Vec3& y) { return x.x == y.x && x.y == y.y && x.z == y.z; };
    return a.baseColor.x == b.baseColor.x && a.baseColor.y == b.baseColor.y &&
           a.baseColor.z == b.baseColor.z && a.baseColor.w == b.baseColor.w &&
           a.metallic == b.metallic && a.roughness == b.roughness && v3(a.emissive, b.emissive) &&
           a.uvScale == b.uvScale && a.albedoTex == b.albedoTex && a.normalTex == b.normalTex &&
           a.mrTex == b.mrTex && a.occlusionTex == b.occlusionTex &&
           a.emissiveTex == b.emissiveTex && a.normalScale == b.normalScale &&
           a.occlusionStrength == b.occlusionStrength && a.alphaCutoff == b.alphaCutoff &&
           a.doubleSided == b.doubleSided && a.subsurface == b.subsurface &&
           a.sssCurvature == b.sssCurvature && a.translucency == b.translucency &&
           v3(a.sssTint, b.sssTint) && a.graph == b.graph;
}
} // namespace ae

namespace ae {

// ---------------------------------------------------------------------------

void applyGpuAutoTier(RenderSettings& s) {
    const char* renderer = rhi::info().device.c_str();
    if (!renderer[0]) return;
    if (std::strstr(renderer, "Intel") || std::strstr(renderer, "Microsoft") ||
        std::strstr(renderer, "llvmpipe")) {
        s.shadowCascades = 2;
        s.spotShadows = 1;  // one nearest spot caster on weak GPUs (0 to disable)
        s.pointShadows = 0; // omni cubes are 6 re-rasters each — off on weak GPUs
        s.renderScale = 0.75f;
        AE_LOG("[Renderer] integrated/software GPU (%s) - shadow cascades 2, spot shadows 1, "
               "point shadows 0, render scale 0.75", renderer);
    }
}

bool Renderer::init(int width, int height) {
    width_ = width;
    height_ = height;

    rhi::setState({}); // depth test+write on, cull on, blend off

    struct { Shader* s; const char* vs; const char* fs; } shaders[] = {
        {&shPBR_, "pbr.vert", "pbr.frag"},
        {&shShadow_, "shadow.vert", "shadow.frag"},
        {&shPointDepth_, "pointdepth.vert", "pointdepth.frag"},
        {&shSkyCapture_, "fullscreen.vert", "sky_capture.frag"},
        {&shBackground_, "background.vert", "background.frag"},
        {&shIrradiance_, "fullscreen.vert", "ibl_irradiance.frag"},
        {&shPrefilter_, "fullscreen.vert", "ibl_prefilter.frag"},
        {&shBrdf_, "fullscreen.vert", "ibl_brdf.frag"},
        {&shSkinLUT_, "fullscreen.vert", "skin_lut.frag"},
        {&shSSAO_, "fullscreen.vert", "ssao.frag"},
        {&shSSAOBlur_, "fullscreen.vert", "ssao_blur.frag"},
        {&shVolumetric_, "fullscreen.vert", "volumetric.frag"},
        {&shSSR_, "fullscreen.vert", "ssr.frag"},
        {&shSSRResolve_, "fullscreen.vert", "ssr_resolve.frag"},
        {&shComposite_, "fullscreen.vert", "composite.frag"},
        {&shBloomDown_, "fullscreen.vert", "bloom_down.frag"},
        {&shBloomUp_, "fullscreen.vert", "bloom_up.frag"},
        {&shTonemap_, "fullscreen.vert", "tonemap.frag"},
        {&shFXAA_, "fullscreen.vert", "fxaa.frag"},
        {&shParticle_, "particle.vert", "particle.frag"},
        {&shDebug_, "debug.vert", "debug.frag"},
        {&shLumDown_, "fullscreen.vert", "lum_down.frag"},
        {&shLumAdapt_, "fullscreen.vert", "lum_adapt.frag"},
        {&shTAA_, "fullscreen.vert", "taa.frag"},
    };
    for (auto& e : shaders)
        if (!e.s->load(e.vs, e.fs)) return false;

    // Particle billboard stream: pos3 + uv2 + color4, rebuilt each frame.
    {
        const rhi::StreamAttr attrs[] = {
            {0, 3, 0}, {1, 2, 3 * sizeof(float)}, {2, 4, 5 * sizeof(float)}};
        particleStream_ = rhi::createStream(9 * sizeof(float), attrs, 3);
    }

    // Debug line stream: pos3 + color3.
    {
        const rhi::StreamAttr attrs[] = {{0, 3, 0}, {1, 3, 3 * sizeof(float)}};
        debugStream_ = rhi::createStream(6 * sizeof(float), attrs, 2);
    }

    // Per-pass GPU timers.
    for (int q = 0; q < 2; ++q)
        for (int p = 0; p < FrameStats::PassCount; ++p) passQ_[q][p] = rhi::createTimer();

    // Instance matrices SSBO (batched draws; grown on demand).
    instanceBuffer_ = rhi::createStorageBuffer();

    // ---- shadow map array ----
    shadowTex_ = rhi::createTexture2DArray(kShadowRes, kShadowRes, kNumCascades,
                                           rhi::TexFormat::Depth32F);
    {
        rhi::SamplerDesc smp;
        smp.mipmaps = false;
        smp.clampToBorder = true; // border = 1.0 -> fully lit outside the map
        smp.shadowCompare = true;
        rhi::setSampler(shadowTex_, smp);
    }
    shadowFBO_ = rhi::createFramebuffer();
    rhi::setDrawBufferCount(shadowFBO_, 0); // depth-only

    // ---- spot-light shadow maps (perspective depth, one layer per caster) ----
    spotShadowTex_ = rhi::createTexture2DArray(kSpotShadowRes, kSpotShadowRes, kMaxSpotShadows,
                                               rhi::TexFormat::Depth32F);
    {
        rhi::SamplerDesc smp;
        smp.mipmaps = false;
        smp.clampToBorder = true; // border = 1.0 -> lit outside the map
        smp.shadowCompare = true;
        rhi::setSampler(spotShadowTex_, smp);
    }
    spotShadowFBO_ = rhi::createFramebuffer();
    rhi::setDrawBufferCount(spotShadowFBO_, 0); // depth-only
    for (int i = 0; i < kMaxLights; ++i) lightSpotLayer_[i] = -1;

    // ---- point-light shadow cubes (linear distance in R, one per caster) ----
    for (int i = 0; i < kMaxPointShadows; ++i) {
        pointCube_[i] = rhi::createTextureCube(kPointShadowRes, 1, rhi::TexFormat::RG16F);
        rhi::SamplerDesc smp;
        smp.mipmaps = false;
        smp.linear = true;
        rhi::setSampler(pointCube_[i], smp);
    }
    pointDepthTex_ = rhi::createTexture2D(kPointShadowRes, kPointShadowRes, 1,
                                          rhi::TexFormat::Depth32F);
    pointShadowFBO_ = rhi::createFramebuffer();
    for (int i = 0; i < kMaxLights; ++i) lightPointCube_[i] = -1;

    // ---- SSAO kernel + noise ----
    std::mt19937 rng(1337);
    std::uniform_real_distribution<float> dist(0.0f, 1.0f);
    for (int i = 0; i < 16; ++i) {
        Vec3 s(dist(rng) * 2 - 1, dist(rng) * 2 - 1, dist(rng));
        s = normalize(s) * dist(rng);
        float scale = (float)i / 16.0f;              // bias samples towards the center
        s = s * lerpf(0.1f, 1.0f, scale * scale);
        ssaoKernel_[i] = s;
    }
    uint8_t noise[16 * 4];
    for (int i = 0; i < 16; ++i) {
        Vec3 v = normalize(Vec3(dist(rng) * 2 - 1, dist(rng) * 2 - 1, 0.0f));
        noise[i * 4 + 0] = (uint8_t)((v.x * 0.5f + 0.5f) * 255);
        noise[i * 4 + 1] = (uint8_t)((v.y * 0.5f + 0.5f) * 255);
        noise[i * 4 + 2] = 128;
        noise[i * 4 + 3] = 255;
    }
    ssaoNoiseTex_ = rhi::createTexture2D(4, 4, 1, rhi::TexFormat::RGBA8);
    rhi::uploadTexture2D(ssaoNoiseTex_, 0, 4, 4, noise);
    {
        rhi::SamplerDesc smp;
        smp.linear = false;
        smp.mipmaps = false;
        rhi::setSampler(ssaoNoiseTex_, smp);
    }

    createEnvironmentResources();
    createWindowTargets();
    return true;
}

static rhi::TextureHandle makeColorTex(rhi::TexFormat format, int w, int h,
                                       bool linear = true) {
    rhi::TextureHandle tex = rhi::createTexture2D(w, h, 1, format);
    rhi::SamplerDesc smp;
    smp.linear = linear;
    smp.mipmaps = false;
    smp.repeat = false;
    rhi::setSampler(tex, smp);
    return tex;
}

// Same as makeColorTex but with a mip chain, so a pass can sample a blurred
// version of the frame (SSR uses it to widen a reflection with roughness).
static rhi::TextureHandle makeColorTexMips(rhi::TexFormat format, int w, int h) {
    int levels = 1;
    for (int d = (w > h ? w : h); d > 1; d >>= 1) ++levels;
    rhi::TextureHandle tex = rhi::createTexture2D(w, h, levels, format);
    rhi::SamplerDesc smp;
    smp.linear = true;
    smp.mipmaps = true;
    smp.repeat = false;
    rhi::setSampler(tex, smp);
    return tex;
}

void Renderer::createWindowTargets() {
    // Geometry MRT
    gDirect_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    gAmbient_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    gNormal_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_, /*linear=*/false);
    // Specular weight + roughness, for SSR. RGBA8 is plenty: both are 0..1.
    gSpecular_ = makeColorTex(rhi::TexFormat::RGBA8, width_, height_, /*linear=*/false);
    gDepth_ = makeColorTex(rhi::TexFormat::Depth32F, width_, height_, /*linear=*/false);

    gFBO_ = rhi::createFramebuffer();
    rhi::attachColor(gFBO_, 0, gDirect_);
    rhi::attachColor(gFBO_, 1, gAmbient_);
    rhi::attachColor(gFBO_, 2, gNormal_);
    rhi::attachColor(gFBO_, 3, gSpecular_);
    rhi::attachDepth(gFBO_, gDepth_);
    rhi::setDrawBufferCount(gFBO_, 4);
    if (!rhi::framebufferComplete(gFBO_))
        std::fprintf(stderr, "[Renderer] geometry FBO incomplete\n");

    // SSAO at half resolution (the blur + bilinear upsample hides it, and it
    // costs a quarter of the fill rate).
    int aw = width_ / 2 < 1 ? 1 : width_ / 2;
    int ah = height_ / 2 < 1 ? 1 : height_ / 2;
    ssaoTex_ = makeColorTex(rhi::TexFormat::R8, aw, ah);
    ssaoFBO_ = rhi::createFramebuffer();
    rhi::attachColor(ssaoFBO_, 0, ssaoTex_);
    ssaoBlurTex_ = makeColorTex(rhi::TexFormat::R8, aw, ah);
    ssaoBlurFBO_ = rhi::createFramebuffer();
    rhi::attachColor(ssaoBlurFBO_, 0, ssaoBlurTex_);

    // Volumetric in-scattering, also half res: the result is low-frequency, so
    // a depth-aware upsample at composite time is indistinguishable from full
    // rate at a quarter of the marching cost. RGB = scattered light,
    // A = transmittance through the medium to the scene.
    volumetricTex_ = makeColorTex(rhi::TexFormat::RGBA16F, aw, ah);
    volumetricFBO_ = rhi::createFramebuffer();
    rhi::attachColor(volumetricFBO_, 0, volumetricTex_);

    // SSR marches at half res too; the resolve upsamples it bilinearly, which
    // is fine because a reflection is already a blurred, low-contrast signal.
    ssrTex_ = makeColorTex(rhi::TexFormat::RGBA16F, aw, ah);
    ssrFBO_ = rhi::createFramebuffer();
    rhi::attachColor(ssrFBO_, 0, ssrTex_);

    // HDR composite target
    // Mipped: SSR samples a coarser level for rougher surfaces, which stands in
    // for a proper cone trace at a fraction of the cost.
    hdrTex_ = makeColorTexMips(rhi::TexFormat::RGBA16F, width_, height_);
    hdrFBO_ = rhi::createFramebuffer();
    rhi::attachColor(hdrFBO_, 0, hdrTex_);
    // The opaque depth buffer rides along so the forward (transparent) pass can
    // depth-test against it. Composite/bloom run with depth test disabled.
    rhi::attachDepth(hdrFBO_, gDepth_);

    // Bloom pyramid (starts at half resolution)
    int w = width_ / 2, h = height_ / 2;
    for (int i = 0; i < kBloomMips; ++i) {
        w = w < 1 ? 1 : w;
        h = h < 1 ? 1 : h;
        bloomW_[i] = w; bloomH_[i] = h;
        bloomTex_[i] = makeColorTex(rhi::TexFormat::RGBA16F, w, h);
        bloomFBO_[i] = rhi::createFramebuffer();
        rhi::attachColor(bloomFBO_[i], 0, bloomTex_[i]);
        w /= 2; h /= 2;
    }

    // LDR target for FXAA input
    ldrTex_ = makeColorTex(rhi::TexFormat::RGBA8, width_, height_);
    ldrFBO_ = rhi::createFramebuffer();
    rhi::attachColor(ldrFBO_, 0, ldrTex_);

    // TAA resolve + history (full res, HDR).
    taaTex_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    taaFBO_ = rhi::createFramebuffer();
    rhi::attachColor(taaFBO_, 0, taaTex_);
    historyTex_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    historyFBO_ = rhi::createFramebuffer();
    rhi::attachColor(historyFBO_, 0, historyTex_);
    taaReset_ = true; // no usable history for the new size

    // Auto-exposure reduction pyramid: start at 1/4 res and halve to 1x1.
    int lw = width_ / 4 < 1 ? 1 : width_ / 4;
    int lh = height_ / 4 < 1 ? 1 : height_ / 4;
    lumMipCount_ = 0;
    for (int i = 0; i < kLumMips; ++i) {
        lumW_[i] = lw;
        lumH_[i] = lh;
        lumTex_[i] = makeColorTex(rhi::TexFormat::R16F, lw, lh);
        lumFBO_[i] = rhi::createFramebuffer();
        rhi::attachColor(lumFBO_[i], 0, lumTex_[i]);
        ++lumMipCount_;
        if (lw == 1 && lh == 1) break;
        lw = lw > 1 ? lw / 2 : 1;
        lh = lh > 1 ? lh / 2 : 1;
    }
    for (int i = 0; i < 2; ++i) {
        adaptTex_[i] = makeColorTex(rhi::TexFormat::R16F, 1, 1);
        adaptFBO_[i] = rhi::createFramebuffer();
        rhi::attachColor(adaptFBO_[i], 0, adaptTex_[i]);
    }
    exposureReset_ = true;
}

void Renderer::destroyWindowTargets() {
    for (rhi::TextureHandle* t : {&gDirect_, &gAmbient_, &gNormal_, &gDepth_, &ssaoTex_,
                                  &ssaoBlurTex_, &volumetricTex_, &gSpecular_, &ssrTex_,
                                  &hdrTex_, &ldrTex_, &taaTex_, &historyTex_})
        rhi::destroyTexture(*t);
    for (rhi::FramebufferHandle* f : {&gFBO_, &ssaoFBO_, &ssaoBlurFBO_, &volumetricFBO_,
                                      &ssrFBO_, &hdrFBO_, &ldrFBO_, &taaFBO_, &historyFBO_})
        rhi::destroyFramebuffer(*f);
    for (int i = 0; i < kBloomMips; ++i) {
        rhi::destroyTexture(bloomTex_[i]);
        rhi::destroyFramebuffer(bloomFBO_[i]);
    }
    for (int i = 0; i < lumMipCount_; ++i) {
        rhi::destroyTexture(lumTex_[i]);
        rhi::destroyFramebuffer(lumFBO_[i]);
    }
    lumMipCount_ = 0;
    for (int i = 0; i < 2; ++i) {
        rhi::destroyTexture(adaptTex_[i]);
        rhi::destroyFramebuffer(adaptFBO_[i]);
    }
}

void Renderer::resize(int width, int height) {
    if (width == width_ && height == height_) return;
    width_ = width;
    height_ = height;
    destroyWindowTargets();
    createWindowTargets();
}

void Renderer::createEnvironmentResources() {
    auto makeCube = [](int res, int levels) {
        rhi::TextureHandle tex = rhi::createTextureCube(res, levels, rhi::TexFormat::RGBA16F);
        rhi::SamplerDesc smp;
        smp.mipmaps = levels > 1;
        smp.repeat = false;
        rhi::setSampler(tex, smp);
        return tex;
    };
    int envLevels = 1 + (int)std::floor(std::log2((double)kEnvRes));
    envCube_ = makeCube(kEnvRes, envLevels);
    irradianceCube_ = makeCube(kIrradianceRes, 1);
    prefilterCube_ = makeCube(kPrefilterRes, kPrefilterMips);

    captureFBO_ = rhi::createFramebuffer();

    // BRDF LUT — computed once, sun-independent.
    brdfLUT_ = makeColorTex(rhi::TexFormat::RG16F, 512, 512);
    rhi::attachColor(captureFBO_, 0, brdfLUT_);
    rhi::bindFramebuffer(captureFBO_);
    rhi::setViewport(0, 0, 512, 512);
    rhi::setState({false, true, rhi::Blend::Off, true});
    shBrdf_.use();
    drawFullscreen();

    // Pre-integrated skin diffusion LUT — also static.
    skinLUT_ = makeColorTex(rhi::TexFormat::RGBA16F, 128, 128);
    rhi::attachColor(captureFBO_, 0, skinLUT_);
    rhi::setViewport(0, 0, 128, 128);
    shSkinLUT_.use();
    drawFullscreen();

    rhi::setState({});
    rhi::bindFramebuffer({});

    // Joint palette UBO for GPU skinning (binding point 0, shared by shaders).
    jointUBO_ = rhi::createUniformBuffer(128 * sizeof(Mat4));
    rhi::bindUniformBuffer(0, jointUBO_);
    tonemapUBO_ = rhi::createUniformBuffer(16); // std140: 3 floats padded to 16
}

void Renderer::updateEnvironment(const Vec3& sunDir, float intensity) {
    rhi::setState({false, true, rhi::Blend::Off, true});

    // 1. Capture the atmosphere into the environment cubemap.
    shSkyCapture_.use();
    shSkyCapture_.setVec3("uSunDir", sunDir);
    shSkyCapture_.setFloat("uSunIntensity", intensity);
    rhi::bindFramebuffer(captureFBO_);
    rhi::setViewport(0, 0, kEnvRes, kEnvRes);
    for (int face = 0; face < 6; ++face) {
        rhi::attachColorLayer(captureFBO_, 0, envCube_, 0, face);
        shSkyCapture_.setInt("uFace", face);
        drawFullscreen();
    }
    rhi::generateMips(envCube_);

    // 2. Diffuse irradiance convolution.
    shIrradiance_.use();
    rhi::bindTexture(0, envCube_);
    rhi::setViewport(0, 0, kIrradianceRes, kIrradianceRes);
    for (int face = 0; face < 6; ++face) {
        rhi::attachColorLayer(captureFBO_, 0, irradianceCube_, 0, face);
        shIrradiance_.setInt("uFace", face);
        drawFullscreen();
    }

    // 3. GGX-prefiltered specular chain.
    shPrefilter_.use();
    shPrefilter_.setFloat("uEnvResolution", (float)kEnvRes);
    rhi::bindTexture(0, envCube_);
    for (int mip = 0; mip < kPrefilterMips; ++mip) {
        int res = kPrefilterRes >> mip;
        rhi::setViewport(0, 0, res, res);
        shPrefilter_.setFloat("uRoughness", (float)mip / (kPrefilterMips - 1));
        for (int face = 0; face < 6; ++face) {
            rhi::attachColorLayer(captureFBO_, 0, prefilterCube_, mip, face);
            shPrefilter_.setInt("uFace", face);
            drawFullscreen();
        }
    }

    rhi::bindFramebuffer({});
    rhi::setState({});

    // CPU-side lighting terms derived from sun elevation.
    float elev = clampf(sunDir.y, 0.0f, 1.0f);
    float warm = clampf(elev / 0.35f, 0.0f, 1.0f);
    Vec3 sunTint(1.0f, lerpf(0.42f, 0.96f, warm), lerpf(0.15f, 0.90f, warm));
    float horizonFade = clampf((sunDir.y + 0.03f) / 0.15f, 0.0f, 1.0f);
    sunRadiance_ = sunTint * (intensity * 0.62f) * horizonFade;

    Vec3 horizonBlue(0.55f, 0.68f, 0.85f);
    Vec3 horizonWarm(1.0f, 0.55f, 0.28f);
    Vec3 horizon = horizonWarm * (1.0f - warm) + horizonBlue * warm;
    fogColor_ = horizon * (intensity * 0.055f) * clampf((sunDir.y + 0.05f) / 0.25f, 0.05f, 1.0f);

    cachedSunDir_ = sunDir;
    cachedIntensity_ = intensity;
}

// ---------------------------------------------------------------------------

void Renderer::shadowPass(const RenderScene& scene, const Camera& camera) {
    const float shadowNear = camera.zNear;
    const float shadowFar = 65.0f;
    const float lambda = 0.82f;
    const int nc = settings.shadowCascades < 1 ? 1
                   : settings.shadowCascades > kNumCascades ? kNumCascades
                                                            : settings.shadowCascades;

    // Practical split scheme (log/linear blend), distributed over the active
    // cascade count so the last active cascade always reaches shadowFar.
    float splits[kNumCascades + 1];
    splits[0] = shadowNear;
    for (int i = 1; i <= nc; ++i) {
        float p = (float)i / nc;
        float linear = shadowNear + (shadowFar - shadowNear) * p;
        float logd = shadowNear * std::pow(shadowFar / shadowNear, p);
        splits[i] = lerpf(linear, logd, lambda);
    }

    Vec3 fwd = camera.forward();
    Vec3 right = camera.right();
    Vec3 up = cross(right, fwd);
    float tanHalfFov = std::tan(radians(camera.fovY) * 0.5f);
    float aspect = (float)width_ / (float)height_;

    for (int c = 0; c < nc; ++c) {
        float n = splits[c], f = splits[c + 1];
        cascadeSplits_[c] = f;

        // Bounding sphere of the frustum slice (stable under rotation).
        Vec3 corners[8];
        int idx = 0;
        for (float d : {n, f}) {
            float hh = tanHalfFov * d;
            float hw = hh * aspect;
            Vec3 center = camera.position + fwd * d;
            corners[idx++] = center + up * hh + right * hw;
            corners[idx++] = center + up * hh - right * hw;
            corners[idx++] = center - up * hh + right * hw;
            corners[idx++] = center - up * hh - right * hw;
        }
        Vec3 center(0, 0, 0);
        for (auto& p : corners) center += p * 0.125f;
        float radius = 0.0f;
        for (auto& p : corners) radius = std::fmax(radius, length(p - center));
        radius = std::ceil(radius * 16.0f) / 16.0f;

        const float casterBackup = 40.0f; // room for tall casters behind the slice
        Vec3 lightUp = std::fabs(scene.sunDir.y) > 0.98f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Mat4 lightView = lookAt(center + scene.sunDir * (radius + casterBackup), center, lightUp);
        Mat4 lightProj = ortho(-radius, radius, -radius, radius, 0.1f, 2.0f * radius + casterBackup);
        Mat4 mat = lightProj * lightView;

        // Texel snapping keeps shadow edges from shimmering as the camera moves.
        Vec4 origin = mat * Vec4(0, 0, 0, 1);
        float half = kShadowRes * 0.5f;
        float sx = origin.x * half, sy = origin.y * half;
        float ox = (std::round(sx) - sx) / half;
        float oy = (std::round(sy) - sy) / half;
        Mat4 snap = translate(Vec3(ox, oy, 0));
        cascadeMats_[c] = snap * mat;
        cascadeTexelWorld_[c] = 2.0f * radius / kShadowRes;
    }
    // Pad unused cascade slots with the last active one. The PBR shader selects
    // a cascade from uCascadeSplits and returns "lit" past the final split, so
    // duplicating the last far here means fragments never sample an unrendered
    // layer — no shader change needed to vary the cascade count at runtime.
    for (int c = nc; c < kNumCascades; ++c) {
        cascadeSplits_[c] = cascadeSplits_[nc - 1];
        cascadeMats_[c] = cascadeMats_[nc - 1];
        cascadeTexelWorld_[c] = cascadeTexelWorld_[nc - 1];
    }

    // The geometry pass leaves the shadow array bound on unit 6; rendering
    // into a texture that is still bound for sampling is a feedback hazard
    // that makes some drivers insert per-draw flushes. Unbind first.
    rhi::bindTexture(6, rhi::TextureHandle{}); // unbind: feedback hazard

    rhi::bindFramebuffer(shadowFBO_);
    rhi::setViewport(0, 0, kShadowRes, kShadowRes);
    shShadow_.use();
    for (int c = 0; c < nc; ++c) {
        rhi::attachDepthLayer(shadowFBO_, shadowTex_, 0, c);
        rhi::clear(false, 0, 0, 0, 0, /*depth=*/true);
        shShadow_.setMat4("uLightMat", cascadeMats_[c]);
        drawDepthCasters(scene, camera, shShadow_);
    }
}

// Depth/distance render of the scene's shadow casters using `prog` + its
// uLightMat. Shared by the cascade, spot, and point-shadow passes.
void Renderer::drawDepthCasters(const RenderScene& scene, const Camera& camera, Shader& prog) {
    // Instanced: rigid casters grouped by mesh (material is irrelevant to
    // depth). Skinned casters keep the single-draw path (unique palettes).
    if (settings.instancing) {
        struct SBatch { std::vector<const Renderable*> items; };
        std::vector<SBatch> sbatches;
        for (const auto& e : scene.entities) {
            if (!e.castShadow || !e.mesh || e.jointMatrices) continue;
            if (isBlended(e.material)) continue;
            if (e.maxDistance > 0.0f) {
                Vec3 d = Vec3(e.transform.m[3][0], e.transform.m[3][1],
                              e.transform.m[3][2]) - camera.position;
                if (dot(d, d) > e.maxDistance * e.maxDistance) continue;
            }
            bool merged = false;
            for (auto& b : sbatches)
                if (b.items[0]->mesh == e.mesh) { b.items.push_back(&e); merged = true; break; }
            if (!merged) sbatches.push_back({{&e}});
        }
        prog.setInt("uSkinned", 0);
        for (const auto& b : sbatches) {
            if (b.items.size() == 1) {
                prog.setInt("uInstanced", 0);
                prog.setMat4("uModel", b.items[0]->transform);
                b.items[0]->mesh->draw();
            } else {
                prog.setInt("uInstanced", 1);
                uploadInstanceMatrices(b.items);
                b.items[0]->mesh->drawInstanced((int)b.items.size());
            }
            ++stats.shadowDraws;
        }
        for (const auto& e : scene.entities) { // skinned casters
            if (!e.castShadow || !e.mesh || !e.jointMatrices) continue;
            if (isBlended(e.material)) continue;
            ++stats.shadowDraws;
            prog.setInt("uInstanced", 0);
            prog.setMat4("uModel", e.transform);
            size_t n = e.jointMatrices->size() > 128 ? 128 : e.jointMatrices->size();
            rhi::updateUniformBuffer(jointUBO_, e.jointMatrices->data(), n * sizeof(Mat4));
            prog.setInt("uSkinned", 1);
            e.mesh->draw();
        }
    } else {
        for (const auto& e : scene.entities) {
            if (!e.castShadow || !e.mesh) continue;
            if (isBlended(e.material)) continue;
            ++stats.shadowDraws;
            prog.setInt("uInstanced", 0);
            prog.setMat4("uModel", e.transform);
            if (e.jointMatrices && !e.jointMatrices->empty()) {
                size_t n = e.jointMatrices->size() > 128 ? 128 : e.jointMatrices->size();
                rhi::updateUniformBuffer(jointUBO_, e.jointMatrices->data(),
                                         n * sizeof(Mat4));
                prog.setInt("uSkinned", 1);
            } else {
                prog.setInt("uSkinned", 0);
            }
            e.mesh->draw();
        }
    }
}

// Picks which local lights get shaded this frame.
//
// The PBR shader takes a fixed 8 lights, but a real level has dozens — a lamp
// in every room, a torch in every alcove. Taking the first 8 in scene order
// would light whichever entities happen to be authored first and leave the
// room you are standing in dark. So: drop lights whose sphere of influence
// can't reach the camera at all, then keep the 8 whose influence reaches
// nearest. Walking down a corridor hands the budget from lamp to lamp.
void Renderer::selectLights(const RenderScene& scene, const Camera& camera) {
    activeLights_.clear();
    struct Cand { int index; float score; };
    std::vector<Cand> cands;
    cands.reserve(scene.lights.size());
    for (size_t i = 0; i < scene.lights.size(); ++i) {
        const Light& l = scene.lights[i];
        // Distance from the camera to the light's influence sphere: <= 0 means
        // the camera is inside it (always relevant), large means far outside.
        float d = length(l.position - camera.position) - std::fmax(l.range, 0.0f);
        if (d > 0.0f && l.type != 2) {
            // A light whose reach ends well before the near geometry cannot
            // affect anything on screen; skip it rather than spend a slot.
            if (d > l.range * 2.0f + 8.0f) continue;
        }
        cands.push_back({(int)i, d});
    }
    std::stable_sort(cands.begin(), cands.end(),
                     [](const Cand& a, const Cand& b) { return a.score < b.score; });
    if ((int)cands.size() > kMaxLights) cands.resize(kMaxLights);
    for (const Cand& c : cands) activeLights_.push_back(scene.lights[c.index]);
}

// Renders a perspective depth map for each of the nearest shadow-casting spot
// lights (up to kMaxSpotShadows, gated by settings.spotShadows). lightSpotLayer_
// maps each scene light index to its layer (-1 = none) for the PBR shader.
void Renderer::spotShadowPass(const RenderScene& scene, const Camera& camera) {
    spotShadowCount_ = 0;
    for (int i = 0; i < kMaxLights; ++i) lightSpotLayer_[i] = -1;

    int budget = settings.spotShadows < 0 ? 0
                 : settings.spotShadows > kMaxSpotShadows ? kMaxSpotShadows
                                                          : settings.spotShadows;
    int nLights = (int)activeLights_.size();
    if (budget == 0 || nLights == 0) return;

    // Pick the spot lights nearest the camera (most visually relevant).
    struct Cand { int light; float dist2; };
    std::vector<Cand> cands;
    for (int i = 0; i < nLights; ++i) {
        const Light& l = activeLights_[i];
        if (l.type != 1) continue; // spot only (point shadows = cube map, later)
        Vec3 d = l.position - camera.position;
        cands.push_back({i, dot(d, d)});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
    if ((int)cands.size() > budget) cands.resize(budget);
    if (cands.empty()) return;

    // Unbind the spot array from its sampling unit (feedback hazard, as with 6).
    rhi::bindTexture(10, rhi::TextureHandle{});
    rhi::bindFramebuffer(spotShadowFBO_);
    rhi::setViewport(0, 0, kSpotShadowRes, kSpotShadowRes);
    shShadow_.use();

    for (size_t k = 0; k < cands.size(); ++k) {
        const Light& l = activeLights_[cands[k].light];
        Vec3 dir = normalize(l.direction);
        // FOV from the outer cone half-angle, with margin, capped shy of 180.
        float cosOuter = clampf(l.cosOuter, 0.10f, 0.999f);
        float fov = std::acos(cosOuter) * 2.0f * 1.10f;
        fov = std::fmin(fov, radians(160.0f));
        float far = std::fmax(l.range, 1.0f);
        Vec3 up = std::fabs(dir.y) > 0.98f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
        Mat4 view = lookAt(l.position, l.position + dir, up);
        Mat4 proj = perspective(fov, 1.0f, 0.05f, far);
        spotShadowMat_[k] = proj * view;
        lightSpotLayer_[cands[k].light] = (int)k;

        rhi::attachDepthLayer(spotShadowFBO_, spotShadowTex_, 0, (int)k);
        rhi::clear(false, 0, 0, 0, 0, /*depth=*/true);
        shShadow_.setMat4("uLightMat", spotShadowMat_[k]);
        drawDepthCasters(scene, camera, shShadow_);
    }
    spotShadowCount_ = (int)cands.size();
}

// Renders a linear-distance cube for each of the nearest shadow-casting point
// lights (up to kMaxPointShadows). lightPointCube_ maps each scene light index
// to its cube (-1 = none) for the PBR shader.
void Renderer::pointShadowPass(const RenderScene& scene, const Camera& camera) {
    pointShadowCount_ = 0;
    for (int i = 0; i < kMaxLights; ++i) lightPointCube_[i] = -1;

    int budget = settings.pointShadows < 0 ? 0
                 : settings.pointShadows > kMaxPointShadows ? kMaxPointShadows
                                                            : settings.pointShadows;
    int nLights = (int)activeLights_.size();
    if (budget == 0 || nLights == 0) return;

    struct Cand { int light; float dist2; };
    std::vector<Cand> cands;
    for (int i = 0; i < nLights; ++i) {
        if (activeLights_[i].type != 0) continue; // point only
        Vec3 d = activeLights_[i].position - camera.position;
        cands.push_back({i, dot(d, d)});
    }
    std::sort(cands.begin(), cands.end(),
              [](const Cand& a, const Cand& b) { return a.dist2 < b.dist2; });
    if ((int)cands.size() > budget) cands.resize(budget);
    if (cands.empty()) return;

    // The six cube-face view directions + up vectors (standard GL cube layout).
    static const Vec3 faceDir[6] = {{1, 0, 0}, {-1, 0, 0}, {0, 1, 0},
                                    {0, -1, 0}, {0, 0, 1}, {0, 0, -1}};
    static const Vec3 faceUp[6] = {{0, -1, 0}, {0, -1, 0}, {0, 0, 1},
                                   {0, 0, -1}, {0, -1, 0}, {0, -1, 0}};

    rhi::bindTexture(11, rhi::TextureHandle{}); // unbind: feedback hazard
    rhi::bindTexture(12, rhi::TextureHandle{});
    rhi::bindFramebuffer(pointShadowFBO_);
    rhi::setViewport(0, 0, kPointShadowRes, kPointShadowRes);
    rhi::attachDepth(pointShadowFBO_, pointDepthTex_);
    rhi::setDrawBufferCount(pointShadowFBO_, 1);
    shPointDepth_.use();

    for (size_t k = 0; k < cands.size(); ++k) {
        const Light& l = activeLights_[cands[k].light];
        float far = std::fmax(l.range, 1.0f);
        Mat4 proj = perspective(radians(90.0f), 1.0f, 0.05f, far);
        shPointDepth_.setVec3("uLightPos", l.position);
        shPointDepth_.setFloat("uFar", far);
        for (int f = 0; f < 6; ++f) {
            Mat4 view = lookAt(l.position, l.position + faceDir[f], faceUp[f]);
            rhi::attachColorLayer(pointShadowFBO_, 0, pointCube_[k], 0, f);
            rhi::clear(true, 1, 1, 1, 1, /*depth=*/true); // 1.0 = beyond range -> lit
            shPointDepth_.setMat4("uLightMat", proj * view);
            drawDepthCasters(scene, camera, shPointDepth_);
        }
        lightPointCube_[cands[k].light] = (int)k;
    }
    pointShadowCount_ = (int)cands.size();
}

// Gribb–Hartmann frustum plane extraction from a view-projection matrix
// (plane normals point inward; xyz = normal, w = distance).
static void extractFrustumPlanes(const Mat4& vp, Vec4 planes[6]) {
    // Row i of the column-major matrix is (m[0][i], m[1][i], m[2][i], m[3][i]).
    auto row = [&](int i) { return Vec4(vp.m[0][i], vp.m[1][i], vp.m[2][i], vp.m[3][i]); };
    Vec4 r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);
    auto add = [](const Vec4& a, const Vec4& b) {
        return Vec4(a.x + b.x, a.y + b.y, a.z + b.z, a.w + b.w);
    };
    auto sub = [](const Vec4& a, const Vec4& b) {
        return Vec4(a.x - b.x, a.y - b.y, a.z - b.z, a.w - b.w);
    };
    planes[0] = add(r3, r0); // left
    planes[1] = sub(r3, r0); // right
    planes[2] = add(r3, r1); // bottom
    planes[3] = sub(r3, r1); // top
    planes[4] = add(r3, r2); // near
    planes[5] = sub(r3, r2); // far
}

// Conservative AABB-vs-frustum: reject only when all 8 transformed corners are
// outside one plane. Skinned draws (bounds move with animation) and draws with
// no bounds (min == max) are never culled.
static bool aabbOutsideFrustum(const Vec4 planes[6], const Mat4& transform,
                               const Vec3& bmin, const Vec3& bmax) {
    Vec4 corners[8];
    for (int i = 0; i < 8; ++i) {
        Vec3 c(i & 1 ? bmax.x : bmin.x, i & 2 ? bmax.y : bmin.y, i & 4 ? bmax.z : bmin.z);
        corners[i] = transform * Vec4(c, 1.0f);
    }
    for (int p = 0; p < 6; ++p) {
        const Vec4& pl = planes[p];
        bool allOut = true;
        for (int i = 0; i < 8; ++i) {
            if (pl.x * corners[i].x + pl.y * corners[i].y + pl.z * corners[i].z + pl.w >= 0.0f) {
                allOut = false;
                break;
            }
        }
        if (allOut) return true;
    }
    return false;
}

void Renderer::geometryPass(const RenderScene& scene, const Camera& camera) {
    rhi::bindFramebuffer(gFBO_);
    rhi::setViewport(0, 0, width_, height_);
    rhi::clear(true, 0, 0, 0, 0, true);

    Mat4 view = camera.view();
    Mat4 proj = projFor(camera);
    Mat4 viewProj = proj * view;

    Vec4 frustum[6];
    extractFrustumPlanes(viewProj, frustum);

    frameForward_ = false;
    preppedPrograms_.clear();

    rhi::bindTexture(3, irradianceCube_);
    rhi::bindTexture(4, prefilterCube_);
    rhi::bindTexture(5, brdfLUT_);
    rhi::bindTexture(6, shadowTex_);
    rhi::bindTexture(9, skinLUT_);
    rhi::bindTexture(10, spotShadowTex_);  // local spot-light shadow maps
    rhi::bindTexture(11, pointCube_[0]);   // local point-light shadow cubes
    rhi::bindTexture(12, pointCube_[1]);

    // Visible set (frustum + draw-distance), then batch identical draws.
    std::vector<const Renderable*> visible;
    visible.reserve(scene.entities.size());
    for (const auto& e : scene.entities) {
        if (!e.mesh) continue;
        if (isBlended(e.material)) continue; // drawn by the forward pass after composite
        if (e.maxDistance > 0.0f) {
            Vec3 d = Vec3(e.transform.m[3][0], e.transform.m[3][1], e.transform.m[3][2]) -
                     camera.position;
            if (dot(d, d) > e.maxDistance * e.maxDistance) {
                ++stats.culled;
                continue;
            }
        }
        bool hasBounds = e.boundsMin.x != e.boundsMax.x || e.boundsMin.y != e.boundsMax.y ||
                         e.boundsMin.z != e.boundsMax.z;
        if (settings.frustumCulling && hasBounds && !e.jointMatrices &&
            aabbOutsideFrustum(frustum, e.transform, e.boundsMin, e.boundsMax)) {
            ++stats.culled;
            continue;
        }
        visible.push_back(&e);
    }

    bool culling = true;
    if (!settings.instancing) {
        for (const Renderable* e : visible) submitPBRDraw(*e, culling, scene, camera);
    } else {
        // Group by (mesh, batch-equal material); skinned draws stay singles.
        struct Batch { std::vector<const Renderable*> items; };
        std::vector<Batch> batches;
        for (const Renderable* e : visible) {
            if (e->jointMatrices) { // skinned: unique palette per draw
                batches.push_back({{e}});
                continue;
            }
            bool merged = false;
            for (auto& b : batches) {
                const Renderable* r0 = b.items[0];
                if (r0->jointMatrices || r0->mesh != e->mesh) continue;
                if (!sameBatchMaterial(r0->material, e->material)) continue;
                b.items.push_back(e);
                merged = true;
                break;
            }
            if (!merged) batches.push_back({{e}});
        }
        for (const auto& b : batches) {
            if (b.items.size() == 1) {
                submitPBRDraw(*b.items[0], culling, scene, camera);
            } else {
                submitInstanced(b.items, culling, scene, camera);
            }
        }
    }
    if (!culling) rhi::setCull(true);

    // Sky background: far-plane fullscreen triangle into the same MRT.
    Mat4 invViewProj = inverse(viewProj);
    rhi::setState({true, false, rhi::Blend::Off, true});
    shBackground_.use();
    shBackground_.setMat4("uInvViewProj", invViewProj);
    shBackground_.setVec3("uCamPos", camera.position);
    rhi::bindTexture(0, envCube_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::submitInstanced(const std::vector<const Renderable*>& items, bool& culling,
                               const RenderScene& scene, const Camera& camera) {
    const Renderable& e0 = *items[0];
    const Material& m = e0.material;

    Shader& sh = (m.graph && m.graph->valid) ? const_cast<Shader&>(m.graph->shader) : shPBR_;
    sh.use();
    if (std::find(preppedPrograms_.begin(), preppedPrograms_.end(), sh.id()) ==
        preppedPrograms_.end()) {
        applyFrameUniforms(sh, scene, camera);
        preppedPrograms_.push_back(sh.id());
    }
    if (m.graph && m.graph->valid)
        for (size_t t = 0; t < m.graph->textures.size(); ++t)
            rhi::bindTexture(13 + (int)t, m.graph->textures[t].id());

    sh.setVec4("uBaseColor", m.baseColor);
    sh.setFloat("uMetallic", m.metallic);
    sh.setFloat("uRoughness", m.roughness);
    sh.setVec3("uEmissive", m.emissive);
    sh.setFloat("uUVScale", m.uvScale);
    sh.setFloat("uNormalScale", m.normalScale);
    sh.setFloat("uOcclusionStrength", m.occlusionStrength);
    sh.setFloat("uAlphaCutoff", m.alphaCutoff);
    sh.setFloat("uSubsurface", m.subsurface);
    sh.setFloat("uSSSCurvature", m.sssCurvature);
    sh.setFloat("uTranslucency", m.translucency);
    sh.setVec3("uSSSTint", m.sssTint);
    sh.setInt("uSkinned", 0);
    sh.setInt("uInstanced", 1);

    int flags = 0;
    if (m.albedoTex)    { flags |= 1;  rhi::bindTexture(0, m.albedoTex); }
    if (m.normalTex)    { flags |= 2;  rhi::bindTexture(1, m.normalTex); }
    if (m.mrTex)        { flags |= 4;  rhi::bindTexture(2, m.mrTex); }
    if (m.emissiveTex)  { flags |= 8;  rhi::bindTexture(8, m.emissiveTex); }
    if (m.occlusionTex) { flags |= 16; rhi::bindTexture(7, m.occlusionTex); }
    if (m.worldUV)      { flags |= 32; } // UVs from world position (see pbr.frag)
    sh.setInt("uTexFlags", flags);

    if (m.doubleSided == culling) {
        culling = !m.doubleSided;
        rhi::setCull(culling);
    }

    uploadInstanceMatrices(items);
    e0.mesh->drawInstanced((int)items.size());
    ++stats.drawCalls;
    ++stats.instancedDraws;
    stats.instancedObjects += (int)items.size();
}

void Renderer::uploadInstanceMatrices(const std::vector<const Renderable*>& items) {
    static std::vector<Mat4> mats;
    mats.clear();
    mats.reserve(items.size());
    for (const Renderable* r : items) mats.push_back(r->transform);
    size_t bytes = mats.size() * sizeof(Mat4);
    rhi::setBufferData(instanceBuffer_, mats.data(), bytes);
    rhi::bindStorageBuffer(1, instanceBuffer_);
}

void Renderer::applyFrameUniforms(Shader& sh, const RenderScene& scene, const Camera& camera) {
    Mat4 view = camera.view();
    Mat4 viewProj = projFor(camera) * view;
    sh.setMat4("uView", view);
    sh.setMat4("uViewProj", viewProj);
    sh.setMat4("uViewForNormal", view);
    sh.setVec3("uCamPos", camera.position);
    sh.setVec3("uSunDir", scene.sunDir);
    sh.setVec3("uSunRadiance", sunRadiance_);
    sh.setFloat("uPrefilterMips", (float)(kPrefilterMips - 1));
    sh.setFloat("uTime", frameTime_);

    int numLights = (int)activeLights_.size();
    sh.setInt("uNumLights", numLights);
    for (int i = 0; i < numLights; ++i) {
        const Light& l = activeLights_[i];
        char name[32];
        std::snprintf(name, sizeof(name), "uLightPos[%d]", i);
        sh.setVec3(name, l.position);
        std::snprintf(name, sizeof(name), "uLightColor[%d]", i);
        sh.setVec3(name, l.color);
        std::snprintf(name, sizeof(name), "uLightDir[%d]", i);
        sh.setVec3(name, l.direction);
        // (type, range, cosInner, cosOuter) packed into one vec4.
        std::snprintf(name, sizeof(name), "uLightParams[%d]", i);
        sh.setVec4(name, Vec4((float)l.type, l.range, l.cosInner, l.cosOuter));
        // .x = spot-shadow layer, .y = point-shadow cube (both -1 = none).
        std::snprintf(name, sizeof(name), "uLightExtra[%d]", i);
        sh.setVec4(name, Vec4((float)lightSpotLayer_[i], (float)lightPointCube_[i], 0, 0));
    }
    if (spotShadowCount_ > 0)
        sh.setMat4Array("uSpotShadowMat", spotShadowMat_, spotShadowCount_);

    sh.setMat4Array("uLightMat", cascadeMats_, kNumCascades);
    sh.setVec4("uCascadeSplits", Vec4(cascadeSplits_[0], cascadeSplits_[1], cascadeSplits_[2],
                                      cascadeSplits_[3]));
    sh.setVec4("uCascadeTexelWorld", Vec4(cascadeTexelWorld_[0], cascadeTexelWorld_[1],
                                          cascadeTexelWorld_[2], cascadeTexelWorld_[3]));

    // Forward-pass extras (harmless defaults in the geometry pass).
    sh.setInt("uForwardPass", frameForward_ ? 1 : 0);
    sh.setVec3("uFogColor", sceneFogColor_);
    sh.setFloat("uFogDensity", sceneFogDensity_);
    sh.setFloat("uFogHeightFalloff", sceneFogFalloff_);
}

void Renderer::submitPBRDraw(const Renderable& e, bool& culling, const RenderScene& scene,
                             const Camera& camera) {
    ++stats.drawCalls;
    const Material& m = e.material;

    // Material-graph variant or the standard uber-shader. Frame uniforms are
    // applied the first time a pass touches each distinct program.
    Shader& sh = (m.graph && m.graph->valid) ? const_cast<Shader&>(m.graph->shader) : shPBR_;
    sh.use();
    if (std::find(preppedPrograms_.begin(), preppedPrograms_.end(), sh.id()) ==
        preppedPrograms_.end()) {
        applyFrameUniforms(sh, scene, camera);
        preppedPrograms_.push_back(sh.id());
    }
    if (m.graph && m.graph->valid)
        for (size_t t = 0; t < m.graph->textures.size(); ++t)
            rhi::bindTexture(13 + (int)t, m.graph->textures[t].id());

    sh.setMat4("uModel", e.transform);
    sh.setVec4("uBaseColor", m.baseColor);
    sh.setFloat("uMetallic", m.metallic);
    sh.setFloat("uRoughness", m.roughness);
    sh.setVec3("uEmissive", m.emissive);
    sh.setFloat("uUVScale", m.uvScale);
    sh.setFloat("uNormalScale", m.normalScale);
    sh.setFloat("uOcclusionStrength", m.occlusionStrength);
    sh.setFloat("uAlphaCutoff", m.alphaCutoff);
    sh.setFloat("uSubsurface", m.subsurface);
    sh.setFloat("uSSSCurvature", m.sssCurvature);
    sh.setFloat("uTranslucency", m.translucency);
    sh.setVec3("uSSSTint", m.sssTint);

    if (e.jointMatrices && !e.jointMatrices->empty()) {
        size_t n = e.jointMatrices->size() > 128 ? 128 : e.jointMatrices->size();
        rhi::updateUniformBuffer(jointUBO_, e.jointMatrices->data(), n * sizeof(Mat4));
        sh.setInt("uSkinned", 1);
    } else {
        sh.setInt("uSkinned", 0);
    }
    sh.setInt("uInstanced", 0);

    int flags = 0;
    if (m.albedoTex)    { flags |= 1;  rhi::bindTexture(0, m.albedoTex); }
    if (m.normalTex)    { flags |= 2;  rhi::bindTexture(1, m.normalTex); }
    if (m.mrTex)        { flags |= 4;  rhi::bindTexture(2, m.mrTex); }
    if (m.emissiveTex)  { flags |= 8;  rhi::bindTexture(8, m.emissiveTex); }
    if (m.occlusionTex) { flags |= 16; rhi::bindTexture(7, m.occlusionTex); }
    if (m.worldUV)      { flags |= 32; } // UVs from world position (see pbr.frag)
    sh.setInt("uTexFlags", flags);

    if (m.doubleSided == culling) {
        culling = !m.doubleSided;
        rhi::setCull(culling);
    }
    e.mesh->draw();
}

void Renderer::ssaoPass(const Camera& camera) {
    Mat4 proj = projFor(camera);
    int aw = width_ / 2 < 1 ? 1 : width_ / 2;
    int ah = height_ / 2 < 1 ? 1 : height_ / 2;

    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(ssaoFBO_);
    rhi::setViewport(0, 0, aw, ah);
    shSSAO_.use();
    shSSAO_.setMat4("uProj", proj);
    shSSAO_.setMat4("uInvProj", inverse(proj));
    shSSAO_.setFloat("uProjA", proj.m[2][2]);
    shSSAO_.setFloat("uProjB", proj.m[3][2]);
    shSSAO_.setVec2("uNoiseScale", aw / 4.0f, ah / 4.0f);
    shSSAO_.setVec3Array("uKernel", ssaoKernel_, 16);
    rhi::bindTexture(0, gDepth_);
    rhi::bindTexture(1, gNormal_);
    rhi::bindTexture(2, ssaoNoiseTex_);
    drawFullscreen();

    rhi::bindFramebuffer(ssaoBlurFBO_);
    shSSAOBlur_.use();
    rhi::bindTexture(0, ssaoTex_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::volumetricPass(const RenderScene& scene, const Camera& camera) {
    int aw = width_ / 2 < 1 ? 1 : width_ / 2;
    int ah = height_ / 2 < 1 ? 1 : height_ / 2;
    Mat4 viewProj = projFor(camera) * camera.view();

    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(volumetricFBO_);
    rhi::setViewport(0, 0, aw, ah);
    shVolumetric_.use();
    shVolumetric_.setMat4("uInvViewProj", inverse(viewProj));
    shVolumetric_.setMat4("uView", camera.view());
    shVolumetric_.setMat4Array("uLightMat", cascadeMats_, kNumCascades);
    shVolumetric_.setVec4("uCascadeSplits",
                          Vec4(cascadeSplits_[0], cascadeSplits_[1], cascadeSplits_[2],
                               cascadeSplits_[3]));
    shVolumetric_.setVec3("uCamPos", camera.position);
    shVolumetric_.setVec3("uSunDir", scene.sunDir);
    shVolumetric_.setVec3("uSunRadiance", sunRadiance_);
    shVolumetric_.setVec3("uFogColor", sceneFogColor_);
    shVolumetric_.setFloat("uFogDensity", sceneFogDensity_);
    shVolumetric_.setFloat("uFogHeightFalloff", sceneFogFalloff_);
    shVolumetric_.setFloat("uAnisotropy", settings.volumetricAnisotropy);
    shVolumetric_.setFloat("uIntensity", sceneVolumetric_);
    shVolumetric_.setFloat("uMaxDistance", settings.volumetricMaxDistance);
    shVolumetric_.setInt("uSteps", settings.volumetricSteps);
    shVolumetric_.setFloat("uTime", frameTime_);

    int numLights = (int)activeLights_.size();
    shVolumetric_.setInt("uNumLights", numLights);
    for (int i = 0; i < numLights; ++i) {
        const Light& l = activeLights_[i];
        char n[32];
        std::snprintf(n, sizeof(n), "uLightPos[%d]", i);
        shVolumetric_.setVec3(n, l.position);
        std::snprintf(n, sizeof(n), "uLightColor[%d]", i);
        shVolumetric_.setVec3(n, l.color);
        std::snprintf(n, sizeof(n), "uLightDir[%d]", i);
        shVolumetric_.setVec3(n, l.direction);
        std::snprintf(n, sizeof(n), "uLightParams[%d]", i);
        shVolumetric_.setVec4(n, Vec4((float)l.type, l.range, l.cosInner, l.cosOuter));
    }

    rhi::bindTexture(0, gDepth_);
    rhi::bindTexture(6, shadowTex_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::ssrPass(const Camera& camera) {
    int aw = width_ / 2 < 1 ? 1 : width_ / 2;
    int ah = height_ / 2 < 1 ? 1 : height_ / 2;
    Mat4 proj = projFor(camera);
    Mat4 view = camera.view();

    // The march samples the composited frame, so its mip chain has to be built
    // first. Level 0 is untouched by this — only the coarser levels are filled.
    rhi::generateMips(hdrTex_);

    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(ssrFBO_);
    rhi::setViewport(0, 0, aw, ah);
    shSSR_.use();
    shSSR_.setMat4("uProj", proj);
    shSSR_.setMat4("uInvProj", inverse(proj));
    shSSR_.setFloat("uMaxRoughness", settings.ssrMaxRoughness);
    shSSR_.setFloat("uThickness", settings.ssrThickness);
    shSSR_.setFloat("uMaxDistance", settings.ssrMaxDistance);
    shSSR_.setInt("uSteps", settings.ssrSteps);
    shSSR_.setInt("uRefineSteps", settings.ssrRefineSteps);
    shSSR_.setFloat("uTime", frameTime_);
    rhi::bindTexture(0, gDepth_);
    rhi::bindTexture(1, gNormal_);
    rhi::bindTexture(2, hdrTex_);
    rhi::bindTexture(3, gSpecular_);
    drawFullscreen();

    // Resolve additively into the frame: the shader emits only the DIFFERENCE
    // between the traced reflection and the probe already in there.
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    rhi::setState({false, false, rhi::Blend::Additive, true});
    shSSRResolve_.use();
    shSSRResolve_.setMat4("uInvProj", inverse(proj));
    shSSRResolve_.setMat4("uInvView", inverse(view));
    shSSRResolve_.setFloat("uPrefilterMips", (float)(kPrefilterMips - 1));
    shSSRResolve_.setFloat("uStrength", settings.ssrStrength);
    rhi::bindTexture(0, ssrTex_);
    rhi::bindTexture(1, gSpecular_);
    rhi::bindTexture(2, gNormal_);
    rhi::bindTexture(3, gDepth_);
    rhi::bindTexture(4, prefilterCube_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::compositePass(const RenderScene& scene, const Camera& camera) {
    Mat4 viewProj = projFor(camera) * camera.view();

    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    shComposite_.use();
    shComposite_.setMat4("uInvViewProj", inverse(viewProj));
    shComposite_.setVec3("uCamPos", camera.position);
    shComposite_.setVec3("uSunDir", scene.sunDir);
    shComposite_.setVec3("uFogColor", sceneFogColor_);
    shComposite_.setFloat("uFogDensity", sceneFogDensity_);
    shComposite_.setFloat("uFogHeightFalloff", sceneFogFalloff_);
    bool useVolumetric = settings.volumetric && sceneVolumetric_ > 0.0f;
    shComposite_.setInt("uVolumetric", useVolumetric ? 1 : 0);
    rhi::bindTexture(0, gDirect_);
    rhi::bindTexture(1, gAmbient_);
    rhi::bindTexture(2, ssaoBlurTex_);
    rhi::bindTexture(3, gDepth_);
    rhi::bindTexture(4, volumetricTex_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::forwardPass(const RenderScene& scene, const Camera& camera) {
    // Gather blended surfaces; nothing to do most frames.
    std::vector<const Renderable*> blended;
    for (const auto& e : scene.entities)
        if (e.mesh && isBlended(e.material)) blended.push_back(&e);
    if (blended.empty()) return;

    // Painter's sort: farthest first, keyed on view-space depth of the origin.
    Mat4 view = camera.view();
    auto depthOf = [&](const Renderable* r) {
        const Mat4& t = r->transform;
        Vec4 v = view * Vec4(t.m[3][0], t.m[3][1], t.m[3][2], 1.0f);
        return v.z; // more negative = farther along -Z
    };
    std::sort(blended.begin(), blended.end(),
              [&](const Renderable* a, const Renderable* b) { return depthOf(a) < depthOf(b); });

    // The composite output (hdrFBO_) carries the opaque depth buffer: test
    // against it so transparents hide behind opaques, but never write depth.
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    rhi::setState({true, false, rhi::Blend::Alpha, true});

    // Re-prep every shader this pass touches with uForwardPass=1 + fog.
    frameForward_ = true;
    preppedPrograms_.clear();

    bool culling = true;
    for (const Renderable* e : blended) submitPBRDraw(*e, culling, scene, camera);
    if (!culling) rhi::setCull(true);

    frameForward_ = false;
    rhi::setState({});
}

void Renderer::particlePass(const RenderScene& scene, const Camera& camera) {
    size_t total = 0;
    for (const auto& b : scene.particles) total += b.points.size();
    stats.particles = (int)total;
    if (total == 0) return;

    Mat4 view = camera.view();
    Mat4 viewProj = projFor(camera) * view;
    // Camera basis in world space (rows of the view rotation).
    Vec3 right(view.m[0][0], view.m[1][0], view.m[2][0]);
    Vec3 up(view.m[0][1], view.m[1][1], view.m[2][1]);

    // Expand every particle into two triangles (pos3 uv2 color4). Rotation
    // rolls the billboard basis in the camera plane; flipbook batches remap
    // the quad UVs to the particle's cell.
    struct V { float x, y, z, u, v, r, g, b, a; };
    static std::vector<V> verts; // scratch, reused across frames
    verts.clear();
    verts.reserve(total * 6);
    int flipCols = 1, flipRows = 1;
    auto emit = [&](const ParticlePoint& p) {
        Vec3 rx = right, uy = up;
        if (p.rot != 0.0f) {
            float cs = std::cos(p.rot), sn = std::sin(p.rot);
            rx = right * cs + up * sn;
            uy = up * cs - right * sn;
        }
        rx = rx * (p.size * 0.5f);
        uy = uy * (p.size * 0.5f);
        Vec3 c[4] = {p.pos - rx - uy, p.pos + rx - uy, p.pos + rx + uy, p.pos - rx + uy};
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        if (flipCols > 1 || flipRows > 1) {
            int fi = (int)p.frame;
            int frames = flipCols * flipRows;
            fi = fi < 0 ? 0 : fi >= frames ? frames - 1 : fi;
            float cw = 1.0f / flipCols, ch = 1.0f / flipRows;
            float u0 = (fi % flipCols) * cw;
            // Row 0 is the top of the image (decode order), which is v = 0.
            float v0 = (fi / flipCols) * ch;
            for (auto& t : uv) {
                t[0] = u0 + t[0] * cw;
                t[1] = v0 + (1.0f - t[1]) * ch;
            }
        }
        int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            const Vec3& pos = c[idx[i]];
            verts.push_back({pos.x, pos.y, pos.z, uv[idx[i]][0], uv[idx[i]][1], p.color.x,
                             p.color.y, p.color.z, p.color.w});
        }
    };

    // Alpha batches draw farthest-first for correct compositing.
    Vec3 fwd = camera.forward();
    struct Range {
        size_t first, count;
        bool additive;
        unsigned texture;
        float softFade;
    };
    std::vector<Range> ranges;
    for (const auto& b : scene.particles) {
        size_t first = verts.size();
        flipCols = b.texture ? b.flipCols : 1;
        flipRows = b.texture ? b.flipRows : 1;
        if (b.additive) {
            for (const auto& p : b.points) emit(p);
        } else {
            std::vector<const ParticlePoint*> sorted;
            sorted.reserve(b.points.size());
            for (const auto& p : b.points) sorted.push_back(&p);
            std::sort(sorted.begin(), sorted.end(),
                      [&](const ParticlePoint* a, const ParticlePoint* c2) {
                          return dot(a->pos - camera.position, fwd) >
                                 dot(c2->pos - camera.position, fwd);
                      });
            for (const auto* p : sorted) emit(*p);
        }
        ranges.push_back({first, verts.size() - first, b.additive, b.texture, b.softFade});
    }

    size_t bytes = verts.size() * sizeof(V);
    rhi::setStreamData(particleStream_, verts.data(), bytes);

    // Into the HDR buffer, tested against opaque depth, never writing it. The
    // depth texture stays bound for soft fade — legal here because depth
    // writes are off (same pattern as the composite pass).
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    rhi::setState({true, false, rhi::Blend::Premultiplied, false});

    Mat4 proj = projFor(camera);
    shParticle_.use();
    shParticle_.setMat4("uViewProj", viewProj);
    rhi::bindTexture(1, gDepth_);
    for (const Range& r : ranges) {
        if (r.texture) rhi::bindTexture(0, r.texture);
        // x = textured, y = soft-fade distance, z/w = depth linearization.
        shParticle_.setVec4("uParams", Vec4(r.texture ? 1.0f : 0.0f, r.softFade,
                                            proj.m[2][2], proj.m[3][2]));
        // Premultiplied output: additive ONE/ONE, alpha ONE/ONE_MINUS_SRC_ALPHA.
        rhi::setBlend(r.additive ? rhi::Blend::Additive : rhi::Blend::Premultiplied);
        rhi::drawStream(particleStream_, rhi::Topology::Triangles, (int)r.first, (int)r.count);
        ++stats.drawCalls;
    }

    rhi::setState({});
}

void Renderer::debugPass(const Camera& camera) {
    DebugDraw& dd = debugDraw();
    if (dd.lines().empty()) return;

    struct V { float x, y, z, r, g, b; };
    static std::vector<V> verts;
    verts.clear();
    verts.reserve(dd.lines().size() * 2);
    for (const auto& l : dd.lines()) {
        verts.push_back({l.a.x, l.a.y, l.a.z, l.color.x, l.color.y, l.color.z});
        verts.push_back({l.b.x, l.b.y, l.b.z, l.color.x, l.color.y, l.color.z});
    }
    rhi::setStreamData(debugStream_, verts.data(), verts.size() * sizeof(V));

    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    // X-ray overlay: debug shapes must stay readable through the geometry they
    // describe (a collider hugging its mesh would z-fight or hide otherwise).
    rhi::setState({false, false, rhi::Blend::Alpha, true});
    shDebug_.use();
    shDebug_.setMat4("uViewProj",
                     projFor(camera) * camera.view());
    rhi::drawStream(debugStream_, rhi::Topology::Lines, 0, (int)verts.size());
    rhi::setState({});
    dd.clear();
}

// Halton(2,3) low-discrepancy sequence — spreads the jitter evenly inside the
// pixel instead of clustering like random offsets would.
static float halton(int index, int base) {
    float f = 1.0f, r = 0.0f;
    for (int i = index; i > 0; i /= base) {
        f /= base;
        r += f * (i % base);
    }
    return r;
}

Vec2 Renderer::jitterNDC() const {
    if (!settings.taa || width_ <= 0 || height_ <= 0) return Vec2(0.0f, 0.0f);
    int i = (int)(frameIndex_ % 8) + 1; // 8-frame cycle
    // [0,1) -> [-0.5,0.5) pixel, then to NDC (which spans 2 over the viewport).
    float jx = (halton(i, 2) - 0.5f) * 2.0f / (float)width_;
    float jy = (halton(i, 3) - 0.5f) * 2.0f / (float)height_;
    return Vec2(jx, jy);
}

Mat4 Renderer::projFor(const Camera& camera) const {
    Mat4 p = camera.proj((float)width_ / (float)height_);
    Vec2 j = jitterNDC();
    // Post-multiplying the clip-space x/y translation == shifting the whole
    // frustum sub-pixel, which is what makes each frame sample a new point.
    p.m[2][0] += j.x;
    p.m[2][1] += j.y;
    return p;
}

void Renderer::autoExposurePass(float dt) {
    if (!settings.autoExposure || lumMipCount_ == 0) return;
    rhi::setState({false, true, rhi::Blend::Off, true});

    // Reduce the HDR frame to 1x1 log-average luminance.
    shLumDown_.use();
    rhi::TextureHandle src = hdrSource_;
    for (int i = 0; i < lumMipCount_; ++i) {
        rhi::bindFramebuffer(lumFBO_[i]);
        rhi::setViewport(0, 0, lumW_[i], lumH_[i]);
        shLumDown_.setInt("uFirstPass", i == 0 ? 1 : 0);
        rhi::bindTexture(0, src);
        drawFullscreen();
        src = lumTex_[i];
    }

    // Adapt toward it, ping-ponging so this frame reads last frame's value.
    int prev = adaptCur_;
    adaptCur_ ^= 1;
    rhi::bindFramebuffer(adaptFBO_[adaptCur_]);
    rhi::setViewport(0, 0, 1, 1);
    shLumAdapt_.use();
    shLumAdapt_.setFloat("uDeltaTime", dt);
    shLumAdapt_.setFloat("uSpeedUp", settings.autoExposureSpeedUp);
    shLumAdapt_.setFloat("uSpeedDown", settings.autoExposureSpeedDown);
    shLumAdapt_.setFloat("uReset", exposureReset_ ? 1.0f : 0.0f);
    rhi::bindTexture(0, lumTex_[lumMipCount_ - 1]);
    rhi::bindTexture(1, adaptTex_[prev]);
    drawFullscreen();
    exposureReset_ = false;
    rhi::setState({});
}

void Renderer::taaPass(const Camera& camera) {
    // Reprojection deliberately uses the UNJITTERED projection. With the
    // jittered one, a static pixel maps a sub-pixel away from itself every
    // frame, so the history gets bilinearly resampled over and over and the
    // image melts into blur. Unjittered, static content maps exactly 1:1 —
    // history is a clean texel fetch, and the jitter enters only through the
    // current frame, which is what makes the accumulation converge to a
    // supersampled result instead of a smeared one.
    Mat4 viewProj = camera.proj((float)width_ / (float)height_) * camera.view();
    if (!settings.taa) {
        prevViewProj_ = viewProj;
        taaReset_ = true; // any history is stale if TAA is toggled back on
        return;
    }
    rhi::setState({false, true, rhi::Blend::Off, true});

    // Resolve current + history -> taaTex_.
    rhi::bindFramebuffer(taaFBO_);
    rhi::setViewport(0, 0, width_, height_);
    shTAA_.use();
    shTAA_.setMat4("uInvViewProj", inverse(viewProj));
    shTAA_.setMat4("uPrevViewProj", prevViewProj_);
    shTAA_.setVec2("uTexel", 1.0f / (float)width_, 1.0f / (float)height_);
    shTAA_.setFloat("uBlend", 0.9f);
    shTAA_.setFloat("uReset", taaReset_ ? 1.0f : 0.0f);
    rhi::bindTexture(0, hdrTex_);
    rhi::bindTexture(1, historyTex_);
    rhi::bindTexture(2, gDepth_);
    drawFullscreen();

    // The rest of the chain reads the resolved image; it also becomes next
    // frame's history. Swapping the two targets does both with no copy: the
    // texture just written becomes `historyTex_`, and the stale one is recycled
    // as the next resolve target.
    hdrSource_ = taaTex_;
    std::swap(taaTex_, historyTex_);
    std::swap(taaFBO_, historyFBO_);

    prevViewProj_ = viewProj;
    taaReset_ = false;
    rhi::setState({});
}

void Renderer::bloomPass() {
    rhi::setState({false, true, rhi::Blend::Off, true});

    // Downsample chain.
    shBloomDown_.use();
    shBloomDown_.setFloat("uThreshold", 1.0f);
    shBloomDown_.setFloat("uKnee", 0.5f);
    rhi::TextureHandle src = hdrSource_;
    for (int i = 0; i < kBloomMips; ++i) {
        rhi::bindFramebuffer(bloomFBO_[i]);
        rhi::setViewport(0, 0, bloomW_[i], bloomH_[i]);
        shBloomDown_.setInt("uFirstPass", i == 0 ? 1 : 0);
        rhi::bindTexture(0, src);
        drawFullscreen();
        src = bloomTex_[i];
    }

    // Upsample + accumulate.
    rhi::setBlend(rhi::Blend::Additive);
    shBloomUp_.use();
    shBloomUp_.setFloat("uRadius", 1.0f);
    for (int i = kBloomMips - 1; i > 0; --i) {
        rhi::bindFramebuffer(bloomFBO_[i - 1]);
        rhi::setViewport(0, 0, bloomW_[i - 1], bloomH_[i - 1]);
        rhi::bindTexture(0, bloomTex_[i]);
        drawFullscreen();
    }
    rhi::setState({});
}

void Renderer::tonemapPass(float time) {
    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(ldrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    shTonemap_.use();
    bool autoExp = settings.autoExposure && lumMipCount_ > 0;
    struct { float exposure, bloomStrength, time, autoExposure; } tm{
        settings.exposure, settings.bloomStrength, time, autoExp ? 1.0f : 0.0f};
    rhi::updateUniformBuffer(tonemapUBO_, &tm, sizeof(tm));
    rhi::bindUniformBuffer(2, tonemapUBO_);
    rhi::bindTexture(0, hdrSource_);
    rhi::bindTexture(1, bloomTex_[0]);
    rhi::bindTexture(2, adaptTex_[adaptCur_]);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::fxaaPass() {
    rhi::setState({false, true, rhi::Blend::Off, true});
    // Final image goes to the backbuffer by default, or to an editor-supplied
    // texture FBO so it can be shown inside a viewport panel.
    rhi::bindFramebuffer(outputFBO_);
    rhi::setViewport(0, 0, width_, height_);
    shFXAA_.use();
    rhi::bindTexture(0, ldrTex_);
    drawFullscreen();
    rhi::setState({});
}

void Renderer::render(const RenderScene& scene, const Camera& camera, float time) {
    stats = FrameStats{};
    stats.lights = (int)scene.lights.size();
    frameTime_ = time;
    // Exposure adaptation needs a delta; render() only gets absolute time, and
    // deriving it here keeps the signature every host already calls.
    float dt = prevTime_ > 0.0f ? time - prevTime_ : 0.0f;
    if (dt < 0.0f || dt > 0.5f) dt = 0.0f; // first frame / scrub / pause
    prevTime_ = time;
    ++frameIndex_;
    hdrSource_ = hdrTex_; // TAA overrides this with its resolve

    // Per-pass GPU times: read last frame's timer queries (they are complete
    // by now in steady state), then record this frame with the other set.
    passQFrame_ ^= 1;
    int prevQ = passQFrame_ ^ 1;
    if (passQReady_[prevQ]) {
        for (int p = 0; p < FrameStats::PassCount; ++p)
            stats.msPass[p] = (float)(rhi::timerNs(passQ_[prevQ][p]) * 1e-6);
    }
    auto beginPass = [&](int p) { rhi::beginTimer(passQ_[passQFrame_][p]); };
    auto endPass = [&]() { rhi::endTimer(); };

    beginPass(FrameStats::PassEnv);
    Vec3 dSun = scene.sunDir - cachedSunDir_;
    if (dot(dSun, dSun) > 1e-10f || scene.skyIntensity != cachedIntensity_)
        updateEnvironment(scene.sunDir, scene.skyIntensity);
    endPass();

    // Resolve this frame's atmosphere: the scene wins where it says anything,
    // otherwise the renderer's own defaults (and its sky-derived fog color).
    sceneFogDensity_ = scene.fogDensity >= 0.0f ? scene.fogDensity : settings.fogDensity;
    sceneFogFalloff_ =
        scene.fogHeightFalloff >= 0.0f ? scene.fogHeightFalloff : settings.fogHeightFalloff;
    sceneFogColor_ = scene.fogColor.x >= 0.0f ? scene.fogColor : fogColor_;
    sceneVolumetric_ = scene.volumetricIntensity >= 0.0f ? scene.volumetricIntensity
                                                         : settings.volumetricIntensity;

    // Which local lights this frame shades — everything downstream (both
    // shadow passes and the material upload) indexes the list this produces.
    selectLights(scene, camera);

    beginPass(FrameStats::PassShadow);
    shadowPass(scene, camera);
    spotShadowPass(scene, camera);
    pointShadowPass(scene, camera);
    endPass();
    beginPass(FrameStats::PassGeom);
    geometryPass(scene, camera);
    endPass();
    beginPass(FrameStats::PassSsao);
    ssaoPass(camera);
    endPass();
    beginPass(FrameStats::PassComposite);
    if (settings.volumetric && sceneVolumetric_ > 0.0f) volumetricPass(scene, camera);
    compositePass(scene, camera);
    if (settings.ssr) ssrPass(camera);
    endPass();
    beginPass(FrameStats::PassForward);
    forwardPass(scene, camera);
    particlePass(scene, camera);
    debugPass(camera);
    endPass();
    // TAA resolves the finished HDR frame; auto-exposure then measures the
    // resolved (stable) image, and bloom/tonemap read it too.
    beginPass(FrameStats::PassTaa);
    taaPass(camera);
    endPass();
    autoExposurePass(dt);
    beginPass(FrameStats::PassBloom);
    bloomPass();
    endPass();
    beginPass(FrameStats::PassTonemap);
    tonemapPass(time);
    endPass();
    beginPass(FrameStats::PassFxaa);
    fxaaPass();
    endPass();
    passQReady_[passQFrame_] = true;
}

void Renderer::drawFullscreen() { rhi::drawFullscreen(); }

void Renderer::shutdown() {
    destroyWindowTargets();
    for (rhi::TextureHandle* t : {&shadowTex_, &spotShadowTex_, &pointCube_[0], &pointCube_[1],
                                  &pointDepthTex_, &ssaoNoiseTex_, &envCube_, &irradianceCube_,
                                  &prefilterCube_, &brdfLUT_, &skinLUT_})
        rhi::destroyTexture(*t);
    rhi::destroyBuffer(jointUBO_);
    rhi::destroyFramebuffer(shadowFBO_);
    rhi::destroyFramebuffer(spotShadowFBO_);
    rhi::destroyFramebuffer(pointShadowFBO_);
    rhi::destroyFramebuffer(captureFBO_);
    rhi::destroyStream(particleStream_);
    rhi::destroyStream(debugStream_);
    rhi::destroyBuffer(instanceBuffer_);
    for (int q = 0; q < 2; ++q)
        for (int p = 0; p < FrameStats::PassCount; ++p) rhi::destroyTimer(passQ_[q][p]);
}

} // namespace ae
