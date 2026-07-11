#version 450 core
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;
layout(binding = 0) uniform sampler2D uTex;
void main() {
    // Font atlas is white with coverage in alpha; solid quads sample the
    // atlas's white block, so color * texel handles text, fills and images.
    fragColor = vColor * texture(uTex, vUV);
}
