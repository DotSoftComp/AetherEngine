#version 450 core
// Auto-exposure step 1: log-luminance reduction.
// First pass reads the HDR scene and writes log(luminance); later passes are
// plain 4-tap box reductions, so the final 1x1 texel holds the LOG-AVERAGE
// luminance of the frame (geometric mean — far more stable against a few
// bright pixels than a linear average).
#include "common.glsl"

in vec2 vUV;
out float fragColor;

layout(binding = 0) uniform sampler2D texSrc;
layout(std140, binding = 15) uniform U {
    int uFirstPass;
};

void main() {
    vec2 t = 1.0 / vec2(textureSize(texSrc, 0));
    if (uFirstPass != 0) {
        // 4 taps of the HDR frame -> average of log luminance.
        float sum = 0.0;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x) {
                vec3 c = texture(texSrc, vUV + (vec2(x, y) - 0.5) * t).rgb;
                // Clamp keeps pure-black pixels from dragging the mean to -inf.
                sum += log(max(luminance(c), 1e-4));
            }
        fragColor = sum * 0.25;
    } else {
        float sum = 0.0;
        for (int y = 0; y < 2; ++y)
            for (int x = 0; x < 2; ++x)
                sum += texture(texSrc, vUV + (vec2(x, y) - 0.5) * t).r;
        fragColor = sum * 0.25;
    }
}
