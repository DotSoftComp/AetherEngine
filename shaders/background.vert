#version 450 core
// Fullscreen triangle pinned to the far plane (NDC z = 1) so the sky only
// covers pixels the opaque pass left empty (depth test LEQUAL).
out vec2 vUV;
void main() {
    vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 1.0, 1.0);
}
