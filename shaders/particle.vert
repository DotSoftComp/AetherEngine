#version 450 core
// Camera-facing particle billboards: the CPU expands each particle into a quad
// in world space (using the camera basis), so this is a plain pass-through.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

uniform mat4 uViewProj;

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
