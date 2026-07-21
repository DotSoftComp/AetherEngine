#version 450 core
// Screen-space reflections.
//
// Marches the mirror direction through the depth buffer and, where it finds a
// surface, returns what the screen already shows there. That is a real
// reflection of the actual scene — the monster standing on the deck plate, the
// lamp overhead — which no environment probe can give, because the probe was
// captured without any of it.
//
// What it cannot do is reflect anything off-screen or hidden behind something
// else. Every failure mode is therefore reported as a confidence in .a, and the
// resolve pass fades back to the prefiltered environment probe wherever
// confidence is low. That fade is the whole trick: it is why the effect adds
// detail without ever looking obviously broken at the screen edge.
#include "common.glsl"

in vec2 vUV;
out vec4 fragColor; // rgb = reflected colour, a = confidence 0..1

layout(binding = 0) uniform sampler2D texDepth;
layout(binding = 1) uniform sampler2D texNormal;   // view-space, encoded
layout(binding = 2) uniform sampler2D texColor;    // the composited HDR frame
layout(binding = 3) uniform sampler2D texSpecular; // rgb = weight, a = roughness

layout(std140, binding = 15) uniform U {
    mat4 uProj;
    mat4 uInvProj;
    float uMaxRoughness;  // skip pixels blurrier than this: the probe is enough
    float uThickness;     // how deep behind a surface still counts as a hit (m)
    float uMaxDistance;   // view-space march length (m)
    int   uSteps;         // linear steps before refinement
    int   uRefineSteps;   // binary-search steps that sharpen the hit
    float uTime;
};

vec3 viewFromDepth(vec2 uv, float depth) {
    vec4 clip = vec4(uv * 2.0 - 1.0, depth * 2.0 - 1.0, 1.0);
    vec4 v = uInvProj * clip;
    return v.xyz / v.w;
}

float ditherIGN(vec2 pixel, float t) {
    pixel += 5.588238 * fract(t * 0.6180339887);
    return fract(52.9829189 * fract(dot(pixel, vec2(0.06711056, 0.00583715))));
}

void main() {
    vec4 spec = texture(texSpecular, vUV);
    float roughness = spec.a;
    float depth = texture(texDepth, vUV).r;

    // Nothing to reflect off: the sky, or a surface too rough for a coherent
    // mirror ray (a screen-space march cannot produce a blurry lobe honestly).
    if (depth >= 1.0 || roughness > uMaxRoughness || dot(spec.rgb, spec.rgb) < 1e-6) {
        fragColor = vec4(0.0);
        return;
    }

    vec3 P = viewFromDepth(vUV, depth);
    vec3 N = normalize(texture(texNormal, vUV).xyz * 2.0 - 1.0);
    vec3 V = normalize(P);           // view space: the eye is at the origin
    vec3 R = normalize(reflect(V, N));

    // A ray heading back towards the eye can only leave the screen.
    if (R.z > 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    float stepLen = uMaxDistance / float(uSteps);
    float jitter = ditherIGN(gl_FragCoord.xy, uTime);

    // Push the ray off the surface it starts on before testing anything. The
    // first sample otherwise lands within a texel of the origin, "hits" the
    // very pixel being shaded, and reflects it back — which reads as a bright
    // smear spreading across the whole surface. The offset scales with depth
    // so it stays a constant few pixels at any distance.
    float startBias = max(0.08, abs(P.z) * 0.012);
    vec3 origin = P + N * startBias + R * startBias;

    vec3 prev = origin;
    float hitT = -1.0;
    float hitFacing = 0.0;
    vec2 hitUV = vec2(0.0);

    for (int i = 1; i <= uSteps; ++i) {
        vec3 cur = origin + R * (stepLen * (float(i - 1) + jitter));
        vec4 clip = uProj * vec4(cur, 1.0);
        if (clip.w <= 0.0) break;
        vec2 uv = (clip.xy / clip.w) * 0.5 + 0.5;
        if (any(lessThan(uv, vec2(0.0))) || any(greaterThan(uv, vec2(1.0)))) break;

        float sceneDepth = texture(texDepth, uv).r;
        if (sceneDepth >= 1.0) { prev = cur; continue; }
        vec3 sceneP = viewFromDepth(uv, sceneDepth);

        // The ray is behind a surface. `uThickness` stops it from matching a
        // wall that is metres in front of where the ray actually is — without
        // it, every ray eventually "hits" whatever is nearest the camera.
        float behind = sceneP.z - cur.z;
        if (behind > 0.0 && behind < uThickness) {
            // Binary-search between the last miss and this hit so the contact
            // point is sharp instead of quantised to the step length.
            // A hit on a surface facing away from the ray is geometry the
            // reflection could not actually see; treat it as a miss.
            // Reject back-faces AND grazing contacts. A ray running nearly
            // parallel to the surface it lands on has crossed many pixels of
            // depth for a tiny change in position, so the thickness test is
            // effectively guessing — those are the hits that show up as bright
            // speckle on walls.
            vec3 hitN = normalize(texture(texNormal, uv).xyz * 2.0 - 1.0);
            float facing = -dot(hitN, R);
            if (facing < 0.15) { prev = cur; continue; }

            vec3 lo = prev, hi = cur;
            for (int r = 0; r < uRefineSteps; ++r) {
                vec3 mid = (lo + hi) * 0.5;
                vec4 mc = uProj * vec4(mid, 1.0);
                vec2 muv = (mc.xy / mc.w) * 0.5 + 0.5;
                vec3 msp = viewFromDepth(muv, texture(texDepth, muv).r);
                if (msp.z - mid.z > 0.0) hi = mid; else lo = mid;
            }
            vec4 fc = uProj * vec4(hi, 1.0);
            hitUV = (fc.xy / fc.w) * 0.5 + 0.5;
            hitT = length(hi - origin);
            hitFacing = facing;
            break;
        }
        prev = cur;
    }

    if (hitT < 0.0) {
        fragColor = vec4(0.0);
        return;
    }

    // Confidence: fade at the screen border (the reflection runs out of data),
    // when the ray points back at the viewer (grazing rays reflect what the
    // camera cannot see), and as roughness approaches the cutoff.
    vec2 edge = smoothstep(vec2(0.0), vec2(0.12), hitUV) *
                smoothstep(vec2(0.0), vec2(0.12), 1.0 - hitUV);
    float conf = edge.x * edge.y;
    conf *= clamp(-R.z * 3.0, 0.0, 1.0);
    conf *= 1.0 - smoothstep(uMaxRoughness * 0.6, uMaxRoughness, roughness);
    conf *= 1.0 - smoothstep(uMaxDistance * 0.7, uMaxDistance, hitT);
    conf *= smoothstep(0.15, 0.45, hitFacing); // fade the near-grazing hits out

    // Rougher surfaces sample a blurrier mip of the frame, which is a cheap
    // stand-in for a proper cone trace and reads correctly at a glance.
    float lod = roughness * 6.0;
    // Clamp fireflies: one blown-out emissive texel reflected at full strength
    // flickers violently once TAA and bloom get hold of it.
    vec3 hitColor = min(textureLod(texColor, hitUV, lod).rgb, vec3(12.0));
    fragColor = vec4(hitColor, conf);
}
