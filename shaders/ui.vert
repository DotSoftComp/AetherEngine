#version 450 core
layout(location = 0) in vec2 aPos;   // pixels, origin top-left
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;  // normalized RGBA
layout(std140, binding = 15) uniform U {
    vec2 uScreen;
};
out vec2 vUV;
out vec4 vColor;
void main() {
    vUV = aUV;
    vColor = aColor;
    vec2 p = vec2(aPos.x / uScreen.x * 2.0 - 1.0, 1.0 - aPos.y / uScreen.y * 2.0);
    gl_Position = vec4(p, 0.0, 1.0);
}
