// Shared PBR uniform block. Declared identically in pbr.vert and pbr.frag so
// the named block "U" links across stages (each stage uses only the members it
// needs; unused members are legal). Auto-UBO: bound at binding 15, reflected by
// the RHI so the renderer's setUniform(name) calls still work under Vulkan.
const int MAX_LIGHTS = 8;
const int NUM_CASCADES = 4;
const int MAX_SPOT_SHADOWS = 4;
layout(std140, binding = 15) uniform U {
    // vertex stage
    mat4 uModel;
    mat4 uView;
    mat4 uViewProj;
    int  uSkinned;
    int  uInstanced;
    // fragment stage — material
    vec4  uBaseColor;
    float uMetallic;
    float uRoughness;
    vec3  uEmissive;
    int   uTexFlags;
    float uUVScale;
    float uNormalScale;
    float uOcclusionStrength;
    float uAlphaCutoff;
    float uSubsurface;
    float uSSSCurvature;
    float uTranslucency;
    vec3  uSSSTint;
    float uPrefilterMips;
    // fragment stage — camera / sun / fog / time
    vec3  uCamPos;
    mat4  uViewForNormal;
    vec3  uSunDir;
    vec3  uSunRadiance;
    float uTime;
    int   uForwardPass;
    vec3  uFogColor;
    float uFogDensity;
    float uFogHeightFalloff;
    // fragment stage — lights + shadow cascades
    int  uNumLights;
    vec3 uLightPos[MAX_LIGHTS];
    vec3 uLightColor[MAX_LIGHTS];
    vec3 uLightDir[MAX_LIGHTS];
    vec4 uLightParams[MAX_LIGHTS];
    // Per-light extras. .x = spot-shadow layer in texSpotShadow (-1 = none);
    //                   .y = point-shadow cube index 0/1 (-1 = none).
    vec4 uLightExtra[MAX_LIGHTS];
    mat4 uLightMat[NUM_CASCADES];
    vec4 uCascadeSplits;
    vec4 uCascadeTexelWorld;
    // World -> light clip for each shadow-casting spot (perspective from the cone).
    mat4 uSpotShadowMat[MAX_SPOT_SHADOWS];
};
