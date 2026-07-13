#version 450 core
// Jimenez 13-tap downsample (SIGGRAPH 2014, "Next Generation Post Processing").
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texSrc;
layout(std140, binding = 15) uniform U {
    int uFirstPass;
    float uThreshold;
    float uKnee;
};

vec3 sampleSrc(vec2 uv) { return texture(texSrc, uv).rgb; }

vec3 karis(vec3 c) { return c / (1.0 + luminance(c)); }

void main() {
    vec2 t = 1.0 / vec2(textureSize(texSrc, 0));
    vec3 a = sampleSrc(vUV + vec2(-2, -2) * t);
    vec3 b = sampleSrc(vUV + vec2( 0, -2) * t);
    vec3 c = sampleSrc(vUV + vec2( 2, -2) * t);
    vec3 d = sampleSrc(vUV + vec2(-2,  0) * t);
    vec3 e = sampleSrc(vUV);
    vec3 f = sampleSrc(vUV + vec2( 2,  0) * t);
    vec3 g = sampleSrc(vUV + vec2(-2,  2) * t);
    vec3 h = sampleSrc(vUV + vec2( 0,  2) * t);
    vec3 i = sampleSrc(vUV + vec2( 2,  2) * t);
    vec3 j = sampleSrc(vUV + vec2(-1, -1) * t);
    vec3 k = sampleSrc(vUV + vec2( 1, -1) * t);
    vec3 l = sampleSrc(vUV + vec2(-1,  1) * t);
    vec3 m = sampleSrc(vUV + vec2( 1,  1) * t);

    vec3 col;
    if (uFirstPass == 1) {
        // Karis average per quad kills single-pixel fireflies.
        vec3 g0 = karis((a + b + d + e) * 0.25);
        vec3 g1 = karis((b + c + e + f) * 0.25);
        vec3 g2 = karis((d + e + g + h) * 0.25);
        vec3 g3 = karis((e + f + h + i) * 0.25);
        vec3 g4 = karis((j + k + l + m) * 0.25);
        col = g4 * 0.5 + (g0 + g1 + g2 + g3) * 0.125;
        col = col / max(1.0 - luminance(col), 1e-4); // undo karis compression

        // Soft-knee threshold keeps bloom physically anchored to bright pixels.
        float lum = luminance(col);
        float soft = clamp(lum - uThreshold + uKnee, 0.0, 2.0 * uKnee);
        soft = soft * soft / (4.0 * uKnee + 1e-4);
        float contribution = max(soft, lum - uThreshold) / max(lum, 1e-4);
        col *= contribution;
    } else {
        col = e * 0.125;
        col += (a + c + g + i) * 0.03125;
        col += (b + d + f + h) * 0.0625;
        col += (j + k + l + m) * 0.125;
    }
    fragColor = vec4(col, 1.0);
}
