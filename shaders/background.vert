#version 450 core
// Fullscreen triangle pinned to the far plane (NDC z = 1) so the sky only
// covers pixels the opaque pass left empty (depth test LEQUAL).
out vec2 vUV;
void main() {
    vec2 p = vec2((AE_VID << 1) & 2, AE_VID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
