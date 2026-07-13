#version 450 core
// Camera-facing particle billboards: the CPU expands each particle into a quad
// in world space (using the camera basis, rolled per particle), so this is a
// plain pass-through. Flipbook batches arrive with cell-mapped UVs.
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec2 aUV;
layout(location = 2) in vec4 aColor;

layout(std140, binding = 15) uniform U {
    mat4 uViewProj;
    vec4 uParams; // x = textured, y = soft-fade meters, z/w = proj A/B
};

out vec2 vUV;
out vec4 vColor;

void main() {
    vUV = aUV;
    vColor = aColor;
    gl_Position = uViewProj * vec4(aPosition, 1.0);
}
