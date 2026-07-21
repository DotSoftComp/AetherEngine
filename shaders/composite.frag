#version 450 core
// Combines direct + occluded ambient lighting and applies aerial-perspective fog.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texDirect;
layout(binding = 1) uniform sampler2D texAmbient;
layout(binding = 2) uniform sampler2D texAO;
layout(binding = 3) uniform sampler2D texDepth;
layout(binding = 4) uniform sampler2D texVolumetric; // half res: rgb scatter, a transmittance

layout(std140, binding = 15) uniform U {
    mat4 uInvViewProj;
    vec3 uCamPos;
    vec3 uSunDir;
    vec3 uFogColor;
    float uFogDensity;
    float uFogHeightFalloff;
    int  uVolumetric; // 1 = use the marched medium instead of analytic fog
};

// Depth-aware upsample of the half-res volumetric buffer. Plain bilinear bleeds
// fog from a distant surface across a near silhouette, which shows up as a halo
// around every object; weighting the four taps by how well their depth matches
// this pixel keeps the edges honest.
vec4 upsampleVolumetric(vec2 uv, float depth) {
    vec2 texel = 1.0 / vec2(textureSize(texVolumetric, 0));
    vec2 base = (floor(uv / texel - 0.5) + 0.5) * texel;
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = 0; y < 2; ++y) {
        for (int x = 0; x < 2; ++x) {
            vec2 t = base + vec2(x, y) * texel;
            float d = texture(texDepth, t).r;
            // Compare in a roughly linear space; raw depth is far too non-linear
            // near the far plane for a useful similarity test.
            float w = 1.0 / (1e-4 + abs(d - depth) * 200.0);
            sum += texture(texVolumetric, t) * w;
            wsum += w;
        }
    }
    return wsum > 0.0 ? sum / wsum : texture(texVolumetric, uv);
}

void main() {
    vec3 direct = texture(texDirect, vUV).rgb;
    vec3 ambient = texture(texAmbient, vUV).rgb;
    float ao = texture(texAO, vUV).r;

    vec3 color = direct + ambient * ao;

    float depth = texture(texDepth, vUV).r;

    // Volumetric medium: it already integrated both the extinction along the
    // view ray and the light scattered into it, so it replaces the analytic
    // height fog rather than stacking with it. Applied to the sky too — a
    // shaft has to be visible against the background, not just against walls.
    if (uVolumetric != 0) {
        vec4 vol = upsampleVolumetric(vUV, depth);
        fragColor = vec4(color * clamp(vol.a, 0.0, 1.0) + max(vol.rgb, vec3(0.0)), 1.0);
        return;
    }

    if (depth < 1.0) {
        vec4 world = uInvViewProj * vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 worldPos = world.xyz / world.w;
        vec3 toFrag = worldPos - uCamPos;
        float dist = length(toFrag);
        vec3 rayDir = toFrag / max(dist, 1e-4);

        // Exponential height fog, integrated analytically along the view ray.
        float h0 = max(uCamPos.y, 0.0);
        float dh = worldPos.y - uCamPos.y;
        float densityIntegral;
        if (abs(dh) > 0.01) {
            densityIntegral = uFogDensity * dist *
                (exp(-h0 * uFogHeightFalloff) - exp(-max(worldPos.y, 0.0) * uFogHeightFalloff)) /
                (uFogHeightFalloff * dh);
        } else {
            densityIntegral = uFogDensity * dist * exp(-h0 * uFogHeightFalloff);
        }
        float fog = 1.0 - exp(-max(densityIntegral, 0.0));

        // In-scattered sunlight: fog glows when looking towards the sun.
        float sunAmount = pow(max(dot(rayDir, uSunDir), 0.0), 8.0);
        vec3 fogColor = uFogColor + uFogColor * sunAmount * 2.0;

        color = mix(color, fogColor, clamp(fog, 0.0, 1.0));
    }

    fragColor = vec4(color, 1.0);
}
