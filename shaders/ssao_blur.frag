#version 450 core
// 4x4 box blur to dissolve the SSAO rotation-noise pattern.
in vec2 vUV;
out float fragColor;
layout(binding = 0) uniform sampler2D texAO;

void main() {
    vec2 texel = 1.0 / vec2(textureSize(texAO, 0));
    float sum = 0.0;
    for (int y = -2; y < 2; ++y)
        for (int x = -2; x < 2; ++x)
            sum += texture(texAO, vUV + vec2(x, y) * texel).r;
    fragColor = sum / 16.0;
}
