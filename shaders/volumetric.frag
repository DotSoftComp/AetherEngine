#version 450 core
// Volumetric lighting — the fog you can SEE light travelling through.
//
// The composite pass already applies analytic height fog, but that only tints
// the scene by distance: it cannot show a shaft of light through a doorway, or
// the cone under a lamp, because it never asks whether a point in mid-air is
// lit. This does: it marches the view ray and, at each step, tests that point
// against the sun's cascade shadow map and every local light, accumulating
// in-scattered radiance with a Henyey–Greenstein phase function.
//
// Cost control:
//   - runs at half resolution (the result is low-frequency; it is upsampled
//     with a depth-aware filter at composite time),
//   - a fixed low step count with a per-pixel dithered start offset, which
//     trades banding for noise that TAA and the upsample then eat,
//   - transmittance is tracked so the march can stop early once the ray is
//     fully extinguished.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor; // rgb = in-scattered light, a = transmittance to the scene

layout(binding = 0) uniform sampler2D texDepth;
layout(binding = 6) uniform sampler2DArrayShadow texShadow;

const int MAX_LIGHTS = 8;
const int NUM_CASCADES = 4;

layout(std140, binding = 15) uniform U {
    mat4 uInvViewProj;
    mat4 uLightMat[NUM_CASCADES];
    vec4 uCascadeSplits;
    mat4 uView;
    vec3 uCamPos;
    vec3 uSunDir;
    vec3 uSunRadiance;
    vec3 uFogColor;
    float uFogDensity;
    float uFogHeightFalloff;
    float uAnisotropy;    // Henyey-Greenstein g: 0 isotropic, ->1 forward
    float uIntensity;     // artistic scale on the whole effect
    float uMaxDistance;   // stop marching past this (metres)
    int   uSteps;
    int   uNumLights;
    float uTime;
    vec3 uLightPos[MAX_LIGHTS];
    vec3 uLightColor[MAX_LIGHTS];
    vec3 uLightDir[MAX_LIGHTS];
    vec4 uLightParams[MAX_LIGHTS]; // (type, range, cosInner, cosOuter)
};

// Henyey-Greenstein: forward-scattering haze. g>0 makes the fog glow strongly
// when you look towards a light, which is what sells a shaft.
float phaseHG(float cosTheta, float g) {
    float gg = g * g;
    float d = 1.0 + gg - 2.0 * g * cosTheta;
    return (1.0 - gg) / (4.0 * PI * max(d * sqrt(max(d, 1e-4)), 1e-4));
}

// Fog density at a world point: the same exponential height falloff the
// composite pass integrates analytically, sampled pointwise here.
float densityAt(vec3 p) {
    return uFogDensity * exp(-max(p.y, 0.0) * uFogHeightFalloff);
}

float sunShadow(vec3 worldPos, float viewDepth) {
    int cascade = 3;
    if      (viewDepth < uCascadeSplits.x) cascade = 0;
    else if (viewDepth < uCascadeSplits.y) cascade = 1;
    else if (viewDepth < uCascadeSplits.z) cascade = 2;
    if (viewDepth > uCascadeSplits.w) return 1.0;

    vec4 lp = uLightMat[cascade] * vec4(worldPos, 1.0);
    vec3 coord = lp.xyz / lp.w * 0.5 + 0.5;
    if (coord.z >= 1.0) return 1.0;
    // A participating-medium sample has no surface normal, so there is no
    // normal-offset bias to apply — just a small constant depth bias.
    return texture(texShadow, vec4(coord.xy, float(cascade), coord.z - 0.0015));
}

// Interleaved gradient noise: cheap, well-distributed per-pixel dither. The
// time term rotates the pattern every frame so TAA averages the noise away.
float ditherIGN(vec2 pixel, float t) {
    pixel += 5.588238 * fract(t * 0.6180339887);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    float depth = texture(texDepth, vUV).r;

    // World-space ray for this pixel, and how far along it the scene sits.
    vec4 farH = uInvViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 farPos = farH.xyz / farH.w;
    vec3 rayDir = normalize(farPos - uCamPos);

    float sceneDist = uMaxDistance;
    if (depth < 1.0) {
        vec4 wh = uInvViewProj * vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        sceneDist = min(length(wh.xyz / wh.w - uCamPos), uMaxDistance);
    }
    if (sceneDist <= 0.01 || uSteps <= 0) {
        fragColor = vec4(0.0, 0.0, 0.0, 1.0);
        return;
    }

    float stepLen = sceneDist / float(uSteps);
    float jitter = ditherIGN(gl_FragCoord.xy, uTime);
    float t = stepLen * jitter;

    // The medium's SCATTERING ALBEDO — how much of the light it intercepts is
    // re-emitted, and in what hue. uFogColor is authored as the analytic fog's
    // radiance (often nearly black for a dim interior); using it directly here
    // would extinguish the view ray while scattering almost nothing back, so
    // the scene would just go dark as density rose. Renormalising keeps the
    // author's hue at an energy-conserving brightness, which is what real fog
    // does: light removed from the ray reappears as glow.
    float peak = max(uFogColor.r, max(uFogColor.g, uFogColor.b));
    vec3 mediumAlbedo = peak > 1e-4 ? uFogColor / peak : vec3(1.0);

    vec3 scattered = vec3(0.0);
    float transmittance = 1.0;

    for (int i = 0; i < uSteps; ++i) {
        vec3 p = uCamPos + rayDir * t;
        float density = densityAt(p);
        if (density > 1e-6) {
            float sigma = density * stepLen;

            // Sun: one cascade lookup per step is the whole point of the pass.
            float viewDepth = -(uView * vec4(p, 1.0)).z;
            vec3 lit = uSunRadiance * sunShadow(p, viewDepth) *
                       phaseHG(dot(rayDir, uSunDir), uAnisotropy);

            // Local lights: unshadowed (a shadow lookup per light per step is
            // far too costly here), but distance- and cone-attenuated, which is
            // what makes a lamp read as a glowing cone rather than a flat wash.
            for (int l = 0; l < uNumLights; ++l) {
                int type = int(uLightParams[l].x);
                if (type == 2) continue; // directional handled by the sun term
                vec3 toL = uLightPos[l] - p;
                float dist2 = dot(toL, toL);
                float range = uLightParams[l].y;
                if (dist2 > range * range) continue;
                float dist = sqrt(max(dist2, 1e-6));
                vec3 L = toL / dist;

                // Inverse-square with a smooth range cutoff, matching pbr.frag.
                float atten = 1.0 / max(dist2, 0.01);
                float edge = clamp(1.0 - (dist / range) * (dist / range), 0.0, 1.0);
                atten *= edge * edge;

                if (type == 1) { // spot cone
                    float cd = dot(-L, normalize(uLightDir[l]));
                    atten *= clamp((cd - uLightParams[l].w) /
                                   max(uLightParams[l].z - uLightParams[l].w, 1e-4), 0.0, 1.0);
                }
                if (atten <= 0.0) continue;
                lit += uLightColor[l] * atten * phaseHG(dot(rayDir, -L), uAnisotropy);
            }

            // Energy-conserving accumulation: integrate the scattered light
            // over the segment weighted by how much of it survives to the eye.
            float segT = exp(-sigma);
            scattered += transmittance * lit * mediumAlbedo * (1.0 - segT);
            transmittance *= segT;
            if (transmittance < 0.01) break; // ray is opaque; nothing beyond matters
        }
        t += stepLen;
    }

    fragColor = vec4(scattered * uIntensity, transmittance);
}
