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
        s.renderScale = 0.75f;
        AE_LOG("[Renderer] integrated/software GPU (%s) - shadow cascades 2, render scale 0.75",
               renderer);
    }
}

bool Renderer::init(int width, int height) {
    width_ = width;
    height_ = height;

    rhi::setState({}); // depth test+write on, cull on, blend off

    struct { Shader* s; const char* vs; const char* fs; } shaders[] = {
        {&shPBR_, "pbr.vert", "pbr.frag"},
        {&shShadow_, "shadow.vert", "shadow.frag"},
        {&shSkyCapture_, "fullscreen.vert", "sky_capture.frag"},
        {&shBackground_, "background.vert", "background.frag"},
        {&shIrradiance_, "fullscreen.vert", "ibl_irradiance.frag"},
        {&shPrefilter_, "fullscreen.vert", "ibl_prefilter.frag"},
        {&shBrdf_, "fullscreen.vert", "ibl_brdf.frag"},
        {&shSkinLUT_, "fullscreen.vert", "skin_lut.frag"},
        {&shSSAO_, "fullscreen.vert", "ssao.frag"},
        {&shSSAOBlur_, "fullscreen.vert", "ssao_blur.frag"},
        {&shComposite_, "fullscreen.vert", "composite.frag"},
        {&shBloomDown_, "fullscreen.vert", "bloom_down.frag"},
        {&shBloomUp_, "fullscreen.vert", "bloom_up.frag"},
        {&shTonemap_, "fullscreen.vert", "tonemap.frag"},
        {&shFXAA_, "fullscreen.vert", "fxaa.frag"},
        {&shParticle_, "particle.vert", "particle.frag"},
        {&shDebug_, "debug.vert", "debug.frag"},
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

void Renderer::createWindowTargets() {
    // Geometry MRT
    gDirect_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    gAmbient_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
    gNormal_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_, /*linear=*/false);
    gDepth_ = makeColorTex(rhi::TexFormat::Depth32F, width_, height_, /*linear=*/false);

    gFBO_ = rhi::createFramebuffer();
    rhi::attachColor(gFBO_, 0, gDirect_);
    rhi::attachColor(gFBO_, 1, gAmbient_);
    rhi::attachColor(gFBO_, 2, gNormal_);
    rhi::attachDepth(gFBO_, gDepth_);
    rhi::setDrawBufferCount(gFBO_, 3);
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

    // HDR composite target
    hdrTex_ = makeColorTex(rhi::TexFormat::RGBA16F, width_, height_);
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
}

void Renderer::destroyWindowTargets() {
    for (rhi::TextureHandle* t : {&gDirect_, &gAmbient_, &gNormal_, &gDepth_, &ssaoTex_,
                                  &ssaoBlurTex_, &hdrTex_, &ldrTex_})
        rhi::destroyTexture(*t);
    for (rhi::FramebufferHandle* f : {&gFBO_, &ssaoFBO_, &ssaoBlurFBO_, &hdrFBO_, &ldrFBO_})
        rhi::destroyFramebuffer(*f);
    for (int i = 0; i < kBloomMips; ++i) {
        rhi::destroyTexture(bloomTex_[i]);
        rhi::destroyFramebuffer(bloomFBO_[i]);
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
            shShadow_.setInt("uSkinned", 0);
            for (const auto& b : sbatches) {
                if (b.items.size() == 1) {
                    shShadow_.setInt("uInstanced", 0);
                    shShadow_.setMat4("uModel", b.items[0]->transform);
                    b.items[0]->mesh->draw();
                } else {
                    shShadow_.setInt("uInstanced", 1);
                    uploadInstanceMatrices(b.items);
                    b.items[0]->mesh->drawInstanced((int)b.items.size());
                }
                ++stats.shadowDraws;
            }
            for (const auto& e : scene.entities) { // skinned casters
                if (!e.castShadow || !e.mesh || !e.jointMatrices) continue;
                if (isBlended(e.material)) continue;
                ++stats.shadowDraws;
                shShadow_.setInt("uInstanced", 0);
                shShadow_.setMat4("uModel", e.transform);
                size_t n = e.jointMatrices->size() > 128 ? 128 : e.jointMatrices->size();
                rhi::updateUniformBuffer(jointUBO_, e.jointMatrices->data(), n * sizeof(Mat4));
                shShadow_.setInt("uSkinned", 1);
                e.mesh->draw();
            }
        } else {
            for (const auto& e : scene.entities) {
                if (!e.castShadow || !e.mesh) continue;
                if (isBlended(e.material)) continue;
                ++stats.shadowDraws;
                shShadow_.setInt("uInstanced", 0);
                shShadow_.setMat4("uModel", e.transform);
                if (e.jointMatrices && !e.jointMatrices->empty()) {
                    size_t n = e.jointMatrices->size() > 128 ? 128 : e.jointMatrices->size();
                    rhi::updateUniformBuffer(jointUBO_, e.jointMatrices->data(),
                                             n * sizeof(Mat4));
                    shShadow_.setInt("uSkinned", 1);
                } else {
                    shShadow_.setInt("uSkinned", 0);
                }
                e.mesh->draw();
            }
        }
    }
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
    Mat4 proj = camera.proj((float)width_ / (float)height_);
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
            rhi::bindTexture(10 + (int)t, m.graph->textures[t].id());

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
    Mat4 viewProj = camera.proj((float)width_ / (float)height_) * view;
    sh.setMat4("uView", view);
    sh.setMat4("uViewProj", viewProj);
    sh.setMat4("uViewForNormal", view);
    sh.setVec3("uCamPos", camera.position);
    sh.setVec3("uSunDir", scene.sunDir);
    sh.setVec3("uSunRadiance", sunRadiance_);
    sh.setFloat("uPrefilterMips", (float)(kPrefilterMips - 1));
    sh.setFloat("uTime", frameTime_);

    int numLights = (int)scene.lights.size();
    if (numLights > 8) numLights = 8;
    sh.setInt("uNumLights", numLights);
    for (int i = 0; i < numLights; ++i) {
        const Light& l = scene.lights[i];
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
    }

    sh.setMat4Array("uLightMat", cascadeMats_, kNumCascades);
    sh.setVec4("uCascadeSplits", Vec4(cascadeSplits_[0], cascadeSplits_[1], cascadeSplits_[2],
                                      cascadeSplits_[3]));
    sh.setVec4("uCascadeTexelWorld", Vec4(cascadeTexelWorld_[0], cascadeTexelWorld_[1],
                                          cascadeTexelWorld_[2], cascadeTexelWorld_[3]));

    // Forward-pass extras (harmless defaults in the geometry pass).
    sh.setInt("uForwardPass", frameForward_ ? 1 : 0);
    sh.setVec3("uFogColor", fogColor_);
    sh.setFloat("uFogDensity", settings.fogDensity);
    sh.setFloat("uFogHeightFalloff", settings.fogHeightFalloff);
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
            rhi::bindTexture(10 + (int)t, m.graph->textures[t].id());

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
    sh.setInt("uTexFlags", flags);

    if (m.doubleSided == culling) {
        culling = !m.doubleSided;
        rhi::setCull(culling);
    }
    e.mesh->draw();
}

void Renderer::ssaoPass(const Camera& camera) {
    Mat4 proj = camera.proj((float)width_ / (float)height_);
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

void Renderer::compositePass(const RenderScene& scene, const Camera& camera) {
    Mat4 viewProj = camera.proj((float)width_ / (float)height_) * camera.view();

    rhi::setState({false, true, rhi::Blend::Off, true});
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    shComposite_.use();
    shComposite_.setMat4("uInvViewProj", inverse(viewProj));
    shComposite_.setVec3("uCamPos", camera.position);
    shComposite_.setVec3("uSunDir", scene.sunDir);
    shComposite_.setVec3("uFogColor", fogColor_);
    shComposite_.setFloat("uFogDensity", settings.fogDensity);
    shComposite_.setFloat("uFogHeightFalloff", settings.fogHeightFalloff);
    rhi::bindTexture(0, gDirect_);
    rhi::bindTexture(1, gAmbient_);
    rhi::bindTexture(2, ssaoBlurTex_);
    rhi::bindTexture(3, gDepth_);
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
    Mat4 viewProj = camera.proj((float)width_ / (float)height_) * view;
    // Camera basis in world space (rows of the view rotation).
    Vec3 right(view.m[0][0], view.m[1][0], view.m[2][0]);
    Vec3 up(view.m[0][1], view.m[1][1], view.m[2][1]);

    // Expand every particle into two triangles (pos3 uv2 color4).
    struct V { float x, y, z, u, v, r, g, b, a; };
    static std::vector<V> verts; // scratch, reused across frames
    verts.clear();
    verts.reserve(total * 6);
    auto emit = [&](const ParticlePoint& p) {
        Vec3 rx = right * (p.size * 0.5f);
        Vec3 uy = up * (p.size * 0.5f);
        Vec3 c[4] = {p.pos - rx - uy, p.pos + rx - uy, p.pos + rx + uy, p.pos - rx + uy};
        float uv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
        int idx[6] = {0, 1, 2, 0, 2, 3};
        for (int i = 0; i < 6; ++i) {
            const Vec3& pos = c[idx[i]];
            verts.push_back({pos.x, pos.y, pos.z, uv[idx[i]][0], uv[idx[i]][1], p.color.x,
                             p.color.y, p.color.z, p.color.w});
        }
    };

    // Alpha batches draw farthest-first for correct compositing.
    Vec3 fwd = camera.forward();
    struct Range { size_t first, count; bool additive; };
    std::vector<Range> ranges;
    for (const auto& b : scene.particles) {
        size_t first = verts.size();
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
        ranges.push_back({first, verts.size() - first, b.additive});
    }

    size_t bytes = verts.size() * sizeof(V);
    rhi::setStreamData(particleStream_, verts.data(), bytes);

    // Into the HDR buffer, tested against opaque depth, never writing it.
    rhi::bindFramebuffer(hdrFBO_);
    rhi::setViewport(0, 0, width_, height_);
    rhi::setState({true, false, rhi::Blend::Premultiplied, false});

    shParticle_.use();
    shParticle_.setMat4("uViewProj", viewProj);
    for (const Range& r : ranges) {
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
                     camera.proj((float)width_ / (float)height_) * camera.view());
    rhi::drawStream(debugStream_, rhi::Topology::Lines, 0, (int)verts.size());
    rhi::setState({});
    dd.clear();
}

void Renderer::bloomPass() {
    rhi::setState({false, true, rhi::Blend::Off, true});

    // Downsample chain.
    shBloomDown_.use();
    shBloomDown_.setFloat("uThreshold", 1.0f);
    shBloomDown_.setFloat("uKnee", 0.5f);
    rhi::TextureHandle src = hdrTex_;
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
    shTonemap_.setFloat("uExposure", settings.exposure);
    shTonemap_.setFloat("uBloomStrength", settings.bloomStrength);
    shTonemap_.setFloat("uTime", time);
    rhi::bindTexture(0, hdrTex_);
    rhi::bindTexture(1, bloomTex_[0]);
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

    beginPass(FrameStats::PassShadow);
    shadowPass(scene, camera);
    endPass();
    beginPass(FrameStats::PassGeom);
    geometryPass(scene, camera);
    endPass();
    beginPass(FrameStats::PassSsao);
    ssaoPass(camera);
    endPass();
    beginPass(FrameStats::PassComposite);
    compositePass(scene, camera);
    endPass();
    beginPass(FrameStats::PassForward);
    forwardPass(scene, camera);
    particlePass(scene, camera);
    debugPass(camera);
    endPass();
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
    for (rhi::TextureHandle* t : {&shadowTex_, &ssaoNoiseTex_, &envCube_, &irradianceCube_,
                                  &prefilterCube_, &brdfLUT_, &skinLUT_})
        rhi::destroyTexture(*t);
    rhi::destroyBuffer(jointUBO_);
    rhi::destroyFramebuffer(shadowFBO_);
    rhi::destroyFramebuffer(captureFBO_);
    rhi::destroyStream(particleStream_);
    rhi::destroyStream(debugStream_);
    rhi::destroyBuffer(instanceBuffer_);
    for (int q = 0; q < 2; ++q)
        for (int p = 0; p < FrameStats::PassCount; ++p) rhi::destroyTimer(passQ_[q][p]);
}

} // namespace ae
