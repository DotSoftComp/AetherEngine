#version 450 core
// Integrates the split-sum BRDF LUT: scale & bias for F0 over (NdotV, roughness).
#include "common.glsl"

in vec2 vUV;
out vec2 fragColor;

void main() {
    float NdotV = max(vUV.x, 1e-3);
    float roughness = vUV.y;

    vec3 V = vec3(sqrt(1.0 - NdotV * NdotV), 0.0, NdotV);
    vec3 N = vec3(0.0, 0.0, 1.0);

    float A = 0.0;
    float B = 0.0;
    const uint SAMPLE_COUNT = 1024u;
    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec2 Xi = hammersley(i, SAMPLE_COUNT);
        vec3 H = importanceSampleGGX(Xi, N, roughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(L.z, 0.0);
        if (NdotL > 0.0) {
            float NdotH = max(H.z, 0.0);
            float VdotH = max(dot(V, H), 0.0);
            float G = geometrySmith(N, V, L, roughness);
            float GVis = (G * VdotH) / (NdotH * NdotV);
            float Fc = pow(1.0 - VdotH, 5.0);
            A += (1.0 - Fc) * GVis;
            B += Fc * GVis;
        }
    }
    fragColor = vec2(A, B) / float(SAMPLE_COUNT);
}
