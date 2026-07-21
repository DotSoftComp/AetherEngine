#version 450 core
// HDR resolve: bloom blend, exposure, ACES filmic curve, vignette, sRGB
// encode + triangular dither. Luma is stored in alpha for FXAA.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texHDR;
layout(binding = 1) uniform sampler2D texBloom;
layout(binding = 2) uniform sampler2D texAvgLum; // 1x1 adapted luminance

// UBO binding 2 (samplers occupy 0,1,2). Vulkan-ready: no default-block uniforms.
layout(std140, binding = 2) uniform Tonemap {
    float uExposure;
    float uBloomStrength;
    float uTime;
    // 0 = uExposure is the final multiplier. 1 = auto: the scene's adapted
    // luminance drives it and uExposure becomes a relative bias.
    float uAutoExposure;
};

// Narkowicz ACES filmic approximation.
vec3 acesFilm(vec3 x) {
    const float a = 2.51, b = 0.03, c = 2.43, d = 0.59, e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

vec3 linearToSRGB(vec3 c) {
    return mix(c * 12.92, 1.055 * pow(c, vec3(1.0 / 2.4)) - 0.055,
               step(0.0031308, c));
}

float interleavedGradientNoise(vec2 p) {
    return fract(52.9829189 * fract(dot(p, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec3 hdr = texture(texHDR, vUV).rgb;
    vec3 bloom = texture(texBloom, vUV).rgb;
    hdr = mix(hdr, bloom, uBloomStrength);

    // Auto-exposure: scale so the adapted average luminance lands on a fixed
    // middle-grey key, then apply uExposure as an artistic bias on top.
    float exposure = uExposure;
    if (uAutoExposure > 0.5) {
        const float keyValue = 0.18; // middle grey
        float avgLum = max(texture(texAvgLum, vec2(0.5)).r, 1e-4);
        exposure = uExposure * (keyValue / avgLum);
    }
    vec3 color = acesFilm(hdr * exposure);

    // Gentle vignette.
    vec2 d = vUV - 0.5;
    color *= 1.0 - dot(d, d) * 0.55;

    color = linearToSRGB(color);

    // Triangular-ish dither breaks up gradient banding in the sky.
    float n = interleavedGradientNoise(gl_FragCoord.xy + fract(uTime) * 17.0);
    color += (n - 0.5) / 255.0;

    fragColor = vec4(color, luminance(color));
}
