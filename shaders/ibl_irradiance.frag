#version 450 core
// Cosine-convolves the environment map into a diffuse irradiance cubemap.
#include "common.glsl"
#include "atmosphere.glsl"

in vec2 vUV;
out vec4 fragColor;

uniform int uFace;
layout(binding = 0) uniform samplerCube texEnv;

void main() {
    vec3 N = cubeFaceDir(uFace, vUV * 2.0 - 1.0);
    vec3 up = abs(N.y) < 0.999 ? vec3(0.0, 1.0, 0.0) : vec3(1.0, 0.0, 0.0);
    vec3 right = normalize(cross(up, N));
    up = cross(N, right);

    vec3 irradiance = vec3(0.0);
    const float dPhi = 0.049;   // ~128 azimuth steps
    const float dTheta = 0.049; // ~32 elevation steps
    float samples = 0.0;
    for (float phi = 0.0; phi < 2.0 * PI; phi += dPhi) {
        for (float theta = 0.0; theta < 0.5 * PI; theta += dTheta) {
            vec3 tangentDir = vec3(sin(theta) * cos(phi), sin(theta) * sin(phi), cos(theta));
            vec3 dir = tangentDir.x * right + tangentDir.y * up + tangentDir.z * N;
            // Sample a mid mip to average out the sun disc (prevents fireflies).
            irradiance += textureLod(texEnv, dir, 3.0).rgb * cos(theta) * sin(theta);
            samples += 1.0;
        }
    }
    fragColor = vec4(PI * irradiance / samples, 1.0);
}
