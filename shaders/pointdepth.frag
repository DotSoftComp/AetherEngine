#version 450 core
// Writes normalized distance from the point light (0 at the light, 1 at range)
// into a cube face. Sampled by direction in pbr.frag for omnidirectional shadows.
layout(std140, binding = 15) uniform U {
    mat4 uModel;
    mat4 uLightMat;
    int  uSkinned;
    int  uInstanced;
    vec3 uLightPos;
    float uFar;
};

in vec3 vWorld;
layout(location = 0) out vec2 outDist; // RG16F target; .r carries the distance

void main() {
    float d = length(vWorld - uLightPos) / max(uFar, 1e-4);
    outDist = vec2(d, d);
}
