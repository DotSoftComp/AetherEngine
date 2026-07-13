#version 450 core
// Renders one cubemap face of the physically-simulated sky.
#include "atmosphere.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(std140, binding = 15) uniform U {
    int uFace;
    vec3 uSunDir;
    float uSunIntensity;
};

void main() {
    vec3 dir = cubeFaceDir(uFace, vUV * 2.0 - 1.0);
    fragColor = vec4(skyRadiance(dir, uSunDir, uSunIntensity), 1.0);
}
