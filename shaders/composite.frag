#version 450 core
// Combines direct + occluded ambient lighting and applies aerial-perspective fog.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor;

layout(binding = 0) uniform sampler2D texDirect;
layout(binding = 1) uniform sampler2D texAmbient;
layout(binding = 2) uniform sampler2D texAO;
layout(binding = 3) uniform sampler2D texDepth;

layout(std140, binding = 15) uniform U {
    mat4 uInvViewProj;
    vec3 uCamPos;
    vec3 uSunDir;
    vec3 uFogColor;
    float uFogDensity;
    float uFogHeightFalloff;
};

void main() {
    vec3 direct = texture(texDirect, vUV).rgb;
    vec3 ambient = texture(texAmbient, vUV).rgb;
    float ao = texture(texAO, vUV).r;

    vec3 color = direct + ambient * ao;

    float depth = texture(texDepth, vUV).r;
    if (depth < 1.0) {
        vec4 world = uInvViewProj * vec4(vUV * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
        vec3 worldPos = world.xyz / world.w;
        vec3 toFrag = worldPos - uCamPos;
        float dist = length(toFrag);
        vec3 rayDir = toFrag / max(dist, 1e-4);

        // Exponential height fog, integrated analytically along the view ray.
        float h0 = max(uCamPos.y, 0.0);
        float dh = worldPos.y - uCamPos.y;
        float densityIntegral;
        if (abs(dh) > 0.01) {
            densityIntegral = uFogDensity * dist *
                (exp(-h0 * uFogHeightFalloff) - exp(-max(worldPos.y, 0.0) * uFogHeightFalloff)) /
                (uFogHeightFalloff * dh);
        } else {
            densityIntegral = uFogDensity * dist * exp(-h0 * uFogHeightFalloff);
        }
        float fog = 1.0 - exp(-max(densityIntegral, 0.0));

        // In-scattered sunlight: fog glows when looking towards the sun.
        float sunAmount = pow(max(dot(rayDir, uSunDir), 0.0), 8.0);
        vec3 fogColor = uFogColor + uFogColor * sunAmount * 2.0;

        color = mix(color, fogColor, clamp(fog, 0.0, 1.0));
    }

    fragColor = vec4(color, 1.0);
}
