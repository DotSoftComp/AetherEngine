// Nishita single-scattering atmosphere (Rayleigh + Mie), ray-marched.
// Adapted from the public-domain glsl-atmosphere formulation.

// Ray-sphere intersection; returns near/far t, or (1e5, -1e5) on miss.
vec2 raySphere(vec3 r0, vec3 rd, float sr) {
    float a = dot(rd, rd);
    float b = 2.0 * dot(rd, r0);
    float c = dot(r0, r0) - sr * sr;
    float d = b * b - 4.0 * a * c;
    if (d < 0.0) return vec2(1e5, -1e5);
    float sq = sqrt(d);
    return vec2((-b - sq) / (2.0 * a), (-b + sq) / (2.0 * a));
}

vec3 atmosphere(vec3 rayDir, vec3 rayOrigin, vec3 sunDir, float sunIntensity) {
    const float rPlanet = 6371e3;
    const float rAtmos  = 6471e3;
    const vec3  kRlh    = vec3(5.5e-6, 13.0e-6, 22.4e-6);
    const float kMie    = 21e-6;
    const float shRlh   = 8e3;   // Rayleigh scale height
    const float shMie   = 1.2e3; // Mie scale height
    const float g       = 0.758; // Mie phase anisotropy
    const int   iSteps  = 16;
    const int   jSteps  = 8;

    vec2 p = raySphere(rayOrigin, rayDir, rAtmos);
    if (p.x > p.y) return vec3(0.0);
    p.y = min(p.y, raySphere(rayOrigin, rayDir, rPlanet).x);
    float iStepSize = (p.y - p.x) / float(iSteps);

    float iTime = p.x;
    vec3 totalRlh = vec3(0.0);
    vec3 totalMie = vec3(0.0);
    float iOdRlh = 0.0;
    float iOdMie = 0.0;

    float mu = dot(rayDir, sunDir);
    float mumu = mu * mu;
    float gg = g * g;
    float pRlh = 3.0 / (16.0 * 3.14159265) * (1.0 + mumu);
    float pMie = 3.0 / (8.0 * 3.14159265) * ((1.0 - gg) * (mumu + 1.0)) /
                 (pow(1.0 + gg - 2.0 * mu * g, 1.5) * (2.0 + gg));

    for (int i = 0; i < iSteps; i++) {
        vec3 iPos = rayOrigin + rayDir * (iTime + iStepSize * 0.5);
        float iHeight = length(iPos) - rPlanet;

        float odStepRlh = exp(-iHeight / shRlh) * iStepSize;
        float odStepMie = exp(-iHeight / shMie) * iStepSize;
        iOdRlh += odStepRlh;
        iOdMie += odStepMie;

        // Secondary (sun) ray optical depth.
        float jStepSize = raySphere(iPos, sunDir, rAtmos).y / float(jSteps);
        float jTime = 0.0;
        float jOdRlh = 0.0;
        float jOdMie = 0.0;
        for (int j = 0; j < jSteps; j++) {
            vec3 jPos = iPos + sunDir * (jTime + jStepSize * 0.5);
            float jHeight = length(jPos) - rPlanet;
            jOdRlh += exp(-jHeight / shRlh) * jStepSize;
            jOdMie += exp(-jHeight / shMie) * jStepSize;
            jTime += jStepSize;
        }

        vec3 attn = exp(-(kMie * (iOdMie + jOdMie) + kRlh * (iOdRlh + jOdRlh)));
        totalRlh += odStepRlh * attn;
        totalMie += odStepMie * attn;
        iTime += iStepSize;
    }

    return sunIntensity * (pRlh * kRlh * totalRlh + pMie * kMie * totalMie);
}

// Full sky radiance: scattering plus a physically-bright sun disc with limb darkening.
vec3 skyRadiance(vec3 rayDir, vec3 sunDir, float sunIntensity) {
    vec3 origin = vec3(0.0, 6371e3 + 500.0, 0.0);
    vec3 sky = atmosphere(rayDir, origin, sunDir, sunIntensity);

    // Sun disc (~0.53 deg angular diameter), attenuated by atmospheric transmittance
    // approximated from the scattering falloff near the horizon.
    float cosSun = dot(rayDir, sunDir);
    float sunCos = cos(0.00465);
    if (cosSun > sunCos - 0.0003 && rayDir.y > -0.05) {
        float t = clamp((cosSun - (sunCos - 0.0003)) / 0.0006, 0.0, 1.0);
        float limb = 0.6 + 0.4 * sqrt(max(t, 0.0));
        float horizonAtten = clamp((sunDir.y + 0.05) * 8.0, 0.0, 1.0);
        vec3 sunColor = mix(vec3(1.0, 0.45, 0.15), vec3(1.0, 0.96, 0.92), horizonAtten);
        sky += t * limb * sunColor * sunIntensity * 4.0 * horizonAtten;
    }

    // Below the horizon: a sun-lit lambertian ground plane. Metals and glossy
    // materials reflect this hemisphere, so it must carry plausible bounce
    // energy (matching the bright floor of the scene) instead of fading black.
    float ground = smoothstep(-0.02, -0.12, rayDir.y);
    vec3 groundAlbedo = vec3(0.32, 0.30, 0.28);
    vec3 groundLit = groundAlbedo * (max(sunDir.y, 0.0) * 0.9 + 0.1) * sunIntensity * 0.26;
    // Keep the accumulated in-scattering so the ground aerial-fades near the horizon.
    sky = mix(sky, groundLit + sky * 0.4, ground);
    return sky;
}

// Standard OpenGL cubemap face -> world direction (uv in [-1,1]).
vec3 cubeFaceDir(int face, vec2 uv) {
    if (face == 0) return normalize(vec3( 1.0, -uv.y, -uv.x));
    if (face == 1) return normalize(vec3(-1.0, -uv.y,  uv.x));
    if (face == 2) return normalize(vec3( uv.x,  1.0,  uv.y));
    if (face == 3) return normalize(vec3( uv.x, -1.0, -uv.y));
    if (face == 4) return normalize(vec3( uv.x, -uv.y,  1.0));
    return normalize(vec3(-uv.x, -uv.y, -1.0));
}
