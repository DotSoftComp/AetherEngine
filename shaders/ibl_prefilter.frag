#version 450 core
// GGX-prefilters the environment map for the split-sum specular approximation.
#include "common.glsl"
#include "atmosphere.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(std140, binding = 15) uniform U {
    int uFace;
    float uRoughness;
    float uEnvResolution;
};
layout(binding = 0) uniform samplerCube texEnv;

void main() {
    vec3 N = cubeFaceDir(uFace, vUV * 2.0 - 1.0);
    vec3 R = N;
    vec3 V = R;

    const uint SAMPLE_COUNT = 512u;
    vec3 prefiltered = vec3(0.0);
    float totalWeight = 0.0;

    for (uint i = 0u; i < SAMPLE_COUNT; ++i) {
        vec2 Xi = hammersley(i, SAMPLE_COUNT);
        vec3 H = importanceSampleGGX(Xi, N, uRoughness);
        vec3 L = normalize(2.0 * dot(V, H) * H - V);
        float NdotL = max(dot(N, L), 0.0);
        if (NdotL > 0.0) {
            // Sample-density-based mip selection (Karis) tames fireflies from
            // the very bright sun disc.
            float D = distributionGGX(N, H, uRoughness);
            float NdotH = max(dot(N, H), 0.0);
            float HdotV = max(dot(H, V), 0.0);
            float pdf = D * NdotH / (4.0 * HdotV) + 1e-4;
            float saTexel = 4.0 * PI / (6.0 * uEnvResolution * uEnvResolution);
            float saSample = 1.0 / (float(SAMPLE_COUNT) * pdf + 1e-4);
            float mip = uRoughness == 0.0 ? 0.0 : 0.5 * log2(saSample / saTexel);
            prefiltered += textureLod(texEnv, L, mip).rgb * NdotL;
            totalWeight += NdotL;
        }
    }
    fragColor = vec4(prefiltered / max(totalWeight, 1e-4), 1.0);
}
