#version 450 core
// Temporal anti-aliasing.
//
// The camera projection is jittered by a sub-pixel offset each frame (Halton),
// so successive frames sample different points inside every pixel. This pass
// reprojects last frame's resolved image into the current view (reconstructing
// each pixel's world position from depth, then projecting it with the previous
// frame's view-projection) and blends it with the current frame. Averaging
// those jittered samples over time converges to a properly filtered pixel.
//
// The danger is stale history: anything that moved, was hidden, or is newly
// on-screen would smear. We bound it by clamping the history to the colour
// range of the current pixel's 3x3 neighbourhood (Karis' AABB clamp) — history
// too far outside that range is wrong, so it gets pulled back in.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texCurrent; // this frame's HDR
layout(binding = 1) uniform sampler2D texHistory; // last frame's resolved HDR
layout(binding = 2) uniform sampler2D texDepth;   // this frame's depth

layout(std140, binding = 15) uniform U {
    // Both UNJITTERED (see Renderer::taaPass): static geometry must reproject
    // exactly onto itself, or resampling the history every frame blurs it.
    mat4 uInvViewProj;     // this frame, depth -> world
    mat4 uPrevViewProj;    // last frame, world -> prev clip
    vec2 uTexel;
    float uBlend;          // history weight when fully converged
    float uReset;          // 1 = ignore history (first frame / resize)
};

vec3 rgbToYCoCg(vec3 c) {
    return vec3(0.25 * c.r + 0.5 * c.g + 0.25 * c.b,
                0.5 * c.r - 0.5 * c.b,
                -0.25 * c.r + 0.5 * c.g - 0.25 * c.b);
}
vec3 yCoCgToRGB(vec3 c) {
    float t = c.x - c.z;
    return vec3(t + c.y, c.x + c.z, t - c.y);
}

void main() {
    vec3 current = texture(texCurrent, vUV).rgb;
    if (uReset > 0.5) { fragColor = vec4(current, 1.0); return; }

    float depth = texture(texDepth, vUV).r;

    // Reconstruct this pixel's world position, then find where it sat last
    // frame. Background (depth == 1) reprojects by the camera rotation alone,
    // which is exactly right for a sky at infinity.
    vec4 clip = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 world = uInvViewProj * clip;
    world /= world.w;
    vec4 prevClip = uPrevViewProj * vec4(world.xyz, 1.0);
    vec2 prevUV = (prevClip.xy / prevClip.w) * 0.5 + 0.5;

    // Off-screen history is unusable (disocclusion at the frame edge).
    if (any(lessThan(prevUV, vec2(0.0))) || any(greaterThan(prevUV, vec2(1.0)))) {
        fragColor = vec4(current, 1.0);
        return;
    }

    // 3x3 neighbourhood bounds in YCoCg (chroma-aware, so colour edges clamp
    // tighter than a plain RGB box would).
    vec3 m1 = vec3(0.0), m2 = vec3(0.0);
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 c = rgbToYCoCg(texture(texCurrent, vUV + vec2(x, y) * uTexel).rgb);
            m1 += c;
            m2 += c * c;
        }
    }
    const float N = 9.0;
    vec3 mu = m1 / N;
    vec3 sigma = sqrt(max(m2 / N - mu * mu, 0.0));
    // 1.25 sigma keeps edges sharp while still admitting real history.
    vec3 minC = mu - 1.25 * sigma;
    vec3 maxC = mu + 1.25 * sigma;

    vec3 history = rgbToYCoCg(texture(texHistory, prevUV).rgb);
    history = clamp(history, minC, maxC);

    // Less history where it had to be clamped hard (i.e. it disagreed) — this
    // is what stops moving objects trailing ghosts.
    vec3 curYCoCg = rgbToYCoCg(current);
    float clipDist = length(history - curYCoCg) / max(length(sigma) + 1e-4, 1e-4);
    float blend = mix(uBlend, 0.0, clamp(clipDist * 0.25 - 0.25, 0.0, 1.0));

    fragColor = vec4(yCoCgToRGB(mix(curYCoCg, history, blend)), 1.0);
}
