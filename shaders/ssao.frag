#version 450 core
// Hemisphere-kernel screen-space ambient occlusion.
in vec2 vUV;
out float fragColor;

layout(binding = 0) uniform sampler2D texDepth;
layout(binding = 1) uniform sampler2D texNormal;   // view-space, packed 0..1
layout(binding = 2) uniform sampler2D texNoise;    // 4x4 rotation vectors

const int KERNEL_SIZE = 16;
layout(std140, binding = 15) uniform U {
    mat4 uProj;
    mat4 uInvProj;
    float uProjA;
    float uProjB;
    vec2 uNoiseScale;
    vec3 uKernel[KERNEL_SIZE];
};

const float RADIUS = 0.6;
const float BIAS = 0.02;

vec3 viewPosAt(vec2 uv) {
    float depth = texture(texDepth, uv).r;
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 view = uInvProj * clip;
    return view.xyz / view.w;
}

// Analytic view-space Z from hardware depth — the sample loop only compares
// depths, so skip the full matrix reconstruction there.
float viewZAt(vec2 uv) {
    float ndc = texture(texDepth, uv).r * 2.0 - 1.0;
    return uProjB / (-ndc - uProjA);
}

void main() {
    float depth = texture(texDepth, vUV).r;
    if (depth >= 1.0) { fragColor = 1.0; return; }

    vec3 fragPos = viewPosAt(vUV);
    vec3 normal = normalize(texture(texNormal, vUV).xyz * 2.0 - 1.0);
    vec3 randomVec = normalize(texture(texNoise, vUV * uNoiseScale).xyz * 2.0 - 1.0);

    vec3 tangent = normalize(randomVec - normal * dot(randomVec, normal));
    vec3 bitangent = cross(normal, tangent);
    mat3 TBN = mat3(tangent, bitangent, normal);

    float occlusion = 0.0;
    for (int i = 0; i < KERNEL_SIZE; ++i) {
        vec3 samplePos = fragPos + TBN * uKernel[i] * RADIUS;

        vec4 offset = uProj * vec4(samplePos, 1.0);
        offset.xyz /= offset.w;
        vec2 sampleUV = offset.xy * 0.5 + 0.5;
        if (any(lessThan(sampleUV, vec2(0.0))) || any(greaterThan(sampleUV, vec2(1.0))))
            continue;

        float sceneDepth = viewZAt(sampleUV);
        float rangeCheck = smoothstep(0.0, 1.0, RADIUS / abs(fragPos.z - sceneDepth));
        occlusion += (sceneDepth >= samplePos.z + BIAS ? 1.0 : 0.0) * rangeCheck;
    }
    occlusion = 1.0 - occlusion / float(KERNEL_SIZE);
    fragColor = pow(occlusion, 1.6);
}
