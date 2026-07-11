#version 450 core
// Pre-integrated subsurface scattering LUT (Penner, SIGGRAPH 2011).
// x axis: wrapped N.L in [-1, 1]   y axis: skin curvature (1 = ~2mm features)
// Integrates the d'Eon/Luebke 6-Gaussian skin diffusion profile around a ring
// of the given curvature radius, producing the characteristic red terminator.
in vec2 vUV;
out vec4 fragColor;

const float PI = 3.14159265359;

// Sum-of-Gaussians fit of human skin diffusion (variances in mm^2).
vec3 diffusionProfile(float d) {
    float dd = d * d;
    return exp(-dd / (2.0 * 0.0064)) * vec3(0.233, 0.455, 0.649) +
           exp(-dd / (2.0 * 0.0484)) * vec3(0.100, 0.336, 0.344) +
           exp(-dd / (2.0 * 0.1870)) * vec3(0.118, 0.198, 0.000) +
           exp(-dd / (2.0 * 0.5670)) * vec3(0.113, 0.007, 0.007) +
           exp(-dd / (2.0 * 1.9900)) * vec3(0.358, 0.004, 0.000) +
           exp(-dd / (2.0 * 7.4100)) * vec3(0.078, 0.000, 0.000);
}

void main() {
    float NdotL = vUV.x * 2.0 - 1.0;
    float curvature = max(vUV.y, 0.004);
    float radiusMM = 2.0 / curvature; // y = 1 -> 2mm radius, y -> 0 -> flat

    float theta = acos(clamp(NdotL, -1.0, 1.0));

    vec3 total = vec3(0.0);
    vec3 weight = vec3(0.0);
    const int STEPS = 256;
    for (int i = 0; i < STEPS; ++i) {
        float x = (float(i) / float(STEPS) - 0.5) * 2.0 * PI; // ring angle offset
        float dist = abs(2.0 * radiusMM * sin(x * 0.5));      // chord length in mm
        vec3 r = diffusionProfile(dist);
        total += clamp(cos(theta + x), 0.0, 1.0) * r;
        weight += r;
    }
    fragColor = vec4(total / max(weight, vec3(1e-5)), 1.0);
}
