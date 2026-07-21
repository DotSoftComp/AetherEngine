#version 450 core
// Blends the screen-space reflection over the environment probe already in the
// frame.
//
// The geometry pass added `specWeight * prefiltered(R, roughness)` as its IBL
// specular. Where SSR found a real hit we want `specWeight * ssrColor` instead,
// so this adds the DIFFERENCE, faded by confidence:
//
//     delta = specWeight * (ssrColor - prefiltered) * confidence
//
// Adding the reflection outright would double-count the probe and make every
// glossy surface too bright; replacing it wholesale would pop hard at the
// screen edge. The difference form does neither — at zero confidence it
// contributes exactly nothing and the frame is untouched.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texSSR;      // rgb hit colour, a confidence
layout(binding = 1) uniform sampler2D texSpecular; // rgb weight, a roughness
layout(binding = 2) uniform sampler2D texNormal;   // view-space, encoded
layout(binding = 3) uniform sampler2D texDepth;
layout(binding = 4) uniform samplerCube texPrefilter;

layout(std140, binding = 15) uniform U {
    mat4 uInvProj;
    mat4 uInvView;      // view space -> world, for the probe lookup
    float uPrefilterMips;
    float uStrength;
};

// The march is half-res, dithered and one sample per pixel, so its output is
// speckly on anything but a mirror. A reflection is a low-frequency signal, so
// a small cross blur removes the noise without removing the reflection —
// weighted by confidence so misses never bleed into hits.
vec4 blurSSR(vec2 uv) {
    vec2 texel = 1.0 / vec2(textureSize(texSSR, 0));
    vec4 sum = vec4(0.0);
    float wsum = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec4 s = texture(texSSR, uv + vec2(x, y) * texel);
            float w = s.a;
            sum += vec4(s.rgb * w, s.a);
            wsum += w;
        }
    }
    if (wsum <= 1e-4) return vec4(0.0);
    return vec4(sum.rgb / wsum, sum.a / 9.0);
}

void main() {
    vec4 ssr = blurSSR(vUV);
    if (ssr.a <= 0.001) {
        fragColor = vec4(0.0);
        return;
    }
    vec4 spec = texture(texSpecular, vUV);
    float depth = texture(texDepth, vUV).r;
    if (depth >= 1.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Rebuild the same mirror direction the march used, in world space, so the
    // probe sample being replaced is exactly the one the geometry pass added.
    vec4 clip = vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 vp = uInvProj * clip;
    vec3 P = vp.xyz / vp.w;
    vec3 N = normalize(texture(texNormal, vUV).xyz * 2.0 - 1.0);
    vec3 R = reflect(normalize(P), N);
    vec3 Rworld = normalize(mat3(uInvView) * R);

    vec3 probe = textureLod(texPrefilter, Rworld, spec.a * uPrefilterMips).rgb;
    vec3 delta = spec.rgb * (ssr.rgb - probe) * (ssr.a * uStrength);

    fragColor = vec4(delta, 1.0);
}
