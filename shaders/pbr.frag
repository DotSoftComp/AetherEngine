#version 450 core
#include "common.glsl"

in VS_OUT {
    vec3 worldPos;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec2 uv;
    float viewDepth;
} fs;

// MRT: direct lighting (+emissive), image-based ambient (SSAO is applied to
// this buffer at composite time), and view-space normals for SSAO.
layout(location = 0) out vec4 outDirect;
layout(location = 1) out vec4 outAmbient;
layout(location = 2) out vec4 outNormal;

// ---- material ----
#include "pbr_uniforms.glsl"
layout(binding = 0) uniform sampler2D texAlbedo;
layout(binding = 1) uniform sampler2D texNormal;
layout(binding = 2) uniform sampler2D texMR;        // glTF: G = roughness, B = metallic
layout(binding = 7) uniform sampler2D texOcclusion; // R = AO (may alias texMR)
layout(binding = 8) uniform sampler2D texEmissive;

// ---- character skin (pre-integrated subsurface scattering) ----
layout(binding = 9) uniform sampler2D texSkinLUT;

// ---- environment ----
layout(binding = 3) uniform samplerCube texIrradiance;
layout(binding = 4) uniform samplerCube texPrefilter;
layout(binding = 5) uniform sampler2D  texBrdfLUT;

// ---- lights ----

// Material-graph variants: generated sampler declarations land here.
//__MG_DECLS__

// ---- forward (transparent) pass ----
// 0 = deferred MRT geometry pass. 1 = forward: combine direct+ambient here,
// apply the same analytic height fog as composite.frag (which already ran),
// and output premixed color with alpha for SRC_ALPHA blending.


// ---- cascaded shadow maps ----
layout(binding = 6) uniform sampler2DArrayShadow texShadow;

// ---- local (spot) shadow maps: one perspective depth layer per caster ----
layout(binding = 10) uniform sampler2DArrayShadow texSpotShadow;

// ---- point (omni) shadow maps: linear-distance cubes, one per caster ----
layout(binding = 11) uniform samplerCube texPointShadow0;
layout(binding = 12) uniform samplerCube texPointShadow1;

float pointShadowFactor(int cube, vec3 lightToFrag, float dist01) {
    // Small PCF: center + a few offsets around the sample direction so cube
    // shadows aren't a hard aliased edge. dist01 is our distance normalized by
    // range; the cube stores the nearest caster's normalized distance.
    const vec3 offs[5] = vec3[5](vec3(0, 0, 0), vec3(0.03, 0.03, 0.0),
                                 vec3(-0.03, 0.03, 0.0), vec3(0.03, -0.03, 0.0),
                                 vec3(-0.03, -0.03, 0.03));
    float bias = 0.015 + 0.02 * dist01;
    float lit = 0.0;
    for (int i = 0; i < 5; ++i) {
        vec3 dir = lightToFrag + offs[i];
        float stored = (cube == 0) ? texture(texPointShadow0, dir).r
                                   : texture(texPointShadow1, dir).r;
        lit += (dist01 - bias <= stored) ? 1.0 : 0.0;
    }
    return lit / 5.0;
}

float spotShadowFactor(int layer, vec3 worldPos, vec3 N, float NdotL) {
    // Normal-offset bias (spots are close-range, so a small world offset works).
    vec4 lp = uSpotShadowMat[layer] * vec4(worldPos + N * 0.03, 1.0);
    if (lp.w <= 0.0) return 1.0;               // behind the light
    vec3 coord = lp.xyz / lp.w * 0.5 + 0.5;
    if (coord.z >= 1.0) return 1.0;            // past the far plane -> lit
    // Outside the light's frustum: lit (the cone attenuation already handles it).
    if (any(lessThan(coord.xy, vec2(0.0))) || any(greaterThan(coord.xy, vec2(1.0))))
        return 1.0;

    float bias = 0.0009 + 0.0025 * (1.0 - NdotL);
    float ref = coord.z - bias;
    // 3x3 PCF (manual offsets: no textureOffset for sampler2DArrayShadow on some
    // drivers — same reason as the cascade path).
    vec2 texelUV = 1.0 / vec2(textureSize(texSpotShadow, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(texSpotShadow, vec4(coord.xy + vec2(x, y) * texelUV,
                                               float(layer), ref));
    return sum / 9.0;
}

float sampleCascade(int cascade, vec3 worldPos, vec3 N, float NdotL) {
    // Normal-offset bias: push the receiver towards the light along the
    // surface normal by ~1.5 texels to kill acne without peter-panning.
    float texel = uCascadeTexelWorld[cascade];
    vec3 offsetPos = worldPos + N * texel * 1.5;

    vec4 lp = uLightMat[cascade] * vec4(offsetPos, 1.0);
    vec3 coord = lp.xyz / lp.w * 0.5 + 0.5;
    if (coord.z >= 1.0) return 1.0;

    float bias = 0.0012 + 0.0025 * (1.0 - NdotL);
    float ref = coord.z - bias;

    // 3x3 PCF over hardware 2x2 comparisons -> effectively 4x4 soft filter.
    // (Manual UV offsets: textureOffset lacks a sampler2DArrayShadow overload
    // on several drivers.)
    vec2 texelUV = 1.0 / vec2(textureSize(texShadow, 0).xy);
    float sum = 0.0;
    for (int y = -1; y <= 1; ++y)
        for (int x = -1; x <= 1; ++x)
            sum += texture(texShadow, vec4(coord.xy + vec2(x, y) * texelUV, float(cascade), ref));
    return sum / 9.0;
}

float shadowFactor(vec3 worldPos, vec3 N, float NdotL) {
    int cascade = 3;
    if      (fs.viewDepth < uCascadeSplits.x) cascade = 0;
    else if (fs.viewDepth < uCascadeSplits.y) cascade = 1;
    else if (fs.viewDepth < uCascadeSplits.z) cascade = 2;
    if (fs.viewDepth > uCascadeSplits.w) return 1.0;
    return sampleCascade(cascade, worldPos, N, NdotL);
}

vec3 specGGX(vec3 N, vec3 V, vec3 L, float roughness, vec3 F0, float NdotL) {
    vec3 H = normalize(V + L);
    float D = distributionGGX(N, H, roughness);
    float G = geometrySmith(N, V, L, roughness);
    vec3  F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    return (D * G * F) / max(4.0 * max(dot(N, V), 0.0) * NdotL, 1e-5);
}

// Direct lighting. For skin (uSubsurface > 0) the diffuse term comes from the
// pre-integrated diffusion LUT (soft, reddened terminator) and the specular
// uses two GGX lobes, matching measured skin response.
vec3 evalBRDF(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo, float metallic,
              float roughness, vec3 F0, float curvature) {
    float NdotLu = dot(N, L);
    float NdotL = max(NdotLu, 0.0);

    vec3 diffuseW = vec3(NdotL);
    vec3 spec = vec3(0.0);
    if (uSubsurface > 0.0) {
        vec3 lut = texture(texSkinLUT, vec2(NdotLu * 0.5 + 0.5, curvature)).rgb;
        diffuseW = mix(vec3(NdotL), lut, uSubsurface);
        if (NdotL > 0.0)
            spec = (0.65 * specGGX(N, V, L, roughness * 0.8, F0, NdotL) +
                    0.35 * specGGX(N, V, L, min(roughness * 1.75, 1.0), F0, NdotL)) * NdotL;
    } else if (NdotL > 0.0) {
        spec = specGGX(N, V, L, roughness, F0, NdotL) * NdotL;
    }
    if (diffuseW == vec3(0.0) && spec == vec3(0.0)) return vec3(0.0);

    vec3 H = normalize(V + L);
    vec3 F = fresnelSchlick(max(dot(H, V), 0.0), F0);
    vec3 kD = (vec3(1.0) - F) * (1.0 - metallic);
    return (kD * albedo / PI * diffuseW + spec) * radiance;
}

// Light bleeding through thin features (Barre-Brisebois & Bouchard).
vec3 transmission(vec3 N, vec3 V, vec3 L, vec3 radiance, vec3 albedo) {
    vec3 Lt = normalize(-(L + N * 0.35));
    float td = pow(clamp(dot(V, Lt), 0.0, 1.0), 5.0);
    return (td * 1.5 + 0.08) * uTranslucency * uSSSTint * albedo * radiance * (0.25 / PI);
}

void main() {
    vec2 uv = fs.uv * uUVScale;

    // ---- material inputs: either the standard uniform+texture path, or code
    // generated from a material graph (MATERIAL_GRAPH variants). Both fill the
    // same six values; everything below is shared.
    vec3 albedo;
    float alpha;
    float metallic;
    float roughness;
    float ao;
    vec3 emissiveV;
#ifdef MATERIAL_GRAPH
//__MG_CODE__
    roughness = clamp(roughness, 0.04, 1.0);
#else
    albedo = uBaseColor.rgb;
    alpha = uBaseColor.a;
    if ((uTexFlags & 1) != 0) {
        vec4 base = texture(texAlbedo, uv);
        albedo *= base.rgb;
        alpha *= base.a;
    }
    metallic = uMetallic;
    roughness = uRoughness;
    if ((uTexFlags & 4) != 0) {
        vec3 mr = texture(texMR, uv).rgb;
        roughness *= mr.g;
        metallic *= mr.b;
    }
    roughness = clamp(roughness, 0.04, 1.0);

    ao = 1.0;
    if ((uTexFlags & 16) != 0)
        ao = mix(1.0, texture(texOcclusion, uv).r, uOcclusionStrength);

    emissiveV = uEmissive;
    if ((uTexFlags & 8) != 0) emissiveV *= texture(texEmissive, uv).rgb;
#endif
    if (uAlphaCutoff >= 0.0 && alpha < uAlphaCutoff) discard;

    vec3 geoN = normalize(fs.normal);
    if (!gl_FrontFacing) geoN = -geoN; // double-sided materials
    vec3 N = geoN;
#ifdef MATERIAL_GRAPH
    // Generated TBN application when the graph's Output.Normal is connected.
//__MG_NORMAL__
#else
    if ((uTexFlags & 2) != 0) {
        vec3 tn = texture(texNormal, uv).xyz * 2.0 - 1.0;
        tn.xy *= uNormalScale;
        mat3 TBN = mat3(normalize(fs.tangent), normalize(fs.bitangent), geoN);
        N = normalize(TBN * tn);
    }
#endif

    vec3 V = normalize(uCamPos - fs.worldPos);
    vec3 F0 = mix(vec3(0.04), albedo, metallic);

    // Skin curvature drives the diffusion LUT row (1/r scaled to a 2mm ref).
    float curvature = 0.0;
    if (uSubsurface > 0.0) {
        curvature = uSSSCurvature > 0.0
            ? uSSSCurvature
            : clamp(length(fwidth(geoN)) / max(length(fwidth(fs.worldPos)), 1e-6) * 0.002,
                    0.0, 1.0);
    }

    // ---- direct: sun ----
    float NdotL = max(dot(N, uSunDir), 0.0);
    float sssWrap = uSubsurface > 0.0 ? 1.0 : 0.0; // LUT needs the terminator region
    float shadow = (NdotL > 0.0 || sssWrap > 0.0)
        ? shadowFactor(fs.worldPos, geoN, max(NdotL, 0.2)) : 0.0;
    vec3 direct = evalBRDF(N, V, uSunDir, uSunRadiance, albedo, metallic, roughness, F0,
                           curvature) * shadow;
    if (uTranslucency > 0.0)
        direct += transmission(N, V, uSunDir, uSunRadiance, albedo) * (shadow * 0.6 + 0.4);

    // ---- direct: local lights (point / spot / directional) ----
    for (int i = 0; i < uNumLights; ++i) {
        int type = int(uLightParams[i].x + 0.5);
        vec3 L;
        vec3 radiance = uLightColor[i];
        if (type == 2) {
            // Directional: parallel rays; L points towards the light.
            L = -normalize(uLightDir[i]);
        } else {
            vec3 toLight = uLightPos[i] - fs.worldPos;
            float dist2 = dot(toLight, toLight);
            L = toLight * inversesqrt(max(dist2, 1e-6));
            float range = uLightParams[i].y;
            float atten = 1.0 / max(dist2, 0.01);
            atten *= pow(clamp(1.0 - pow(dist2 / (range * range), 2.0), 0.0, 1.0), 2.0);
            if (type == 1) {
                // Spot: soft cone from the beam axis (inner..outer cosines).
                float spotCos = dot(-L, normalize(uLightDir[i]));
                atten *= smoothstep(uLightParams[i].w, uLightParams[i].z, spotCos);
                // Cast shadow if this spot has an allocated shadow layer.
                int layer = int(uLightExtra[i].x);
                if (layer >= 0 && atten > 0.0)
                    atten *= spotShadowFactor(layer, fs.worldPos, geoN, max(dot(N, L), 0.0));
            } else if (type == 0) {
                // Point: omnidirectional cube shadow if one is allocated.
                int cube = int(uLightExtra[i].y);
                if (cube >= 0 && atten > 0.0) {
                    vec3 lightToFrag = fs.worldPos - uLightPos[i];
                    float dist01 = sqrt(dist2) / max(range, 1e-4);
                    atten *= pointShadowFactor(cube, lightToFrag, dist01);
                }
            }
            radiance *= atten;
        }
        direct += evalBRDF(N, V, L, radiance, albedo, metallic, roughness, F0, curvature);
        if (uTranslucency > 0.0)
            direct += transmission(N, V, L, radiance, albedo);
    }

    direct += emissiveV;

    // ---- image-based ambient ----
    float NdotV = max(dot(N, V), 0.0);
    vec3 F = fresnelSchlickRoughness(NdotV, F0, roughness);
    vec3 kD = (1.0 - F) * (1.0 - metallic);

    vec3 irradiance = texture(texIrradiance, N).rgb;
    vec3 diffuseIBL = irradiance * albedo * kD;

    vec3 R = reflect(-V, N);
    vec3 prefiltered = textureLod(texPrefilter, R, roughness * uPrefilterMips).rgb;
    vec2 brdf = texture(texBrdfLUT, vec2(NdotV, roughness)).rg;
    vec3 specularIBL = prefiltered * (F0 * brdf.x + brdf.y);

    vec3 ambient = (diffuseIBL + specularIBL) * ao;

    if (uForwardPass == 1) {
        // Transparent surfaces: no SSAO (only material AO, already in `ambient`),
        // fog applied here since the composite fog pass has already run.
        vec3 color = direct + ambient;

        vec3 toFrag = fs.worldPos - uCamPos;
        float dist = length(toFrag);
        vec3 rayDir = toFrag / max(dist, 1e-4);
        float h0 = max(uCamPos.y, 0.0);
        float dh = fs.worldPos.y - uCamPos.y;
        float densityIntegral;
        if (abs(dh) > 0.01) {
            densityIntegral = uFogDensity * dist *
                (exp(-h0 * uFogHeightFalloff) - exp(-max(fs.worldPos.y, 0.0) * uFogHeightFalloff)) /
                (uFogHeightFalloff * dh);
        } else {
            densityIntegral = uFogDensity * dist * exp(-h0 * uFogHeightFalloff);
        }
        float fog = 1.0 - exp(-max(densityIntegral, 0.0));
        float sunAmount = pow(max(dot(rayDir, uSunDir), 0.0), 8.0);
        vec3 fogCol = uFogColor + uFogColor * sunAmount * 2.0;
        color = mix(color, fogCol, clamp(fog, 0.0, 1.0));

        outDirect = vec4(color, alpha);
        outAmbient = vec4(0.0);
        outNormal = vec4(0.0);
        return;
    }

    vec3 viewN = normalize(mat3(uViewForNormal) * N);

    outDirect = vec4(direct, 1.0);
    outAmbient = vec4(ambient, 1.0);
    outNormal = vec4(viewN * 0.5 + 0.5, 1.0);
}
