#version 450 core
out vec2 vUV;
void main() {
    vec2 p = vec2((AE_VID << 1) & 2, AE_VID & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
