#version 450 core
// Soft radial disc, premultiplied output. Additive batches blend ONE/ONE;
// alpha batches ONE/ONE_MINUS_SRC_ALPHA. HDR colors feed the bloom chain.
in vec2 vUV;
in vec4 vColor;
out vec4 fragColor;

void main() {
    float d = length(vUV * 2.0 - 1.0);
    float mask = smoothstep(1.0, 0.25, d);
    mask *= mask; // softer core falloff
    fragColor = vec4(vColor.rgb * vColor.a * mask, vColor.a * mask);
}
