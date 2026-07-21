#version 450 core
// Auto-exposure step 2: temporal adaptation (eye adjustment).
// Reads the 1x1 log-average luminance and the previous frame's adapted value,
// and moves toward the target exponentially — fast when it brightens, slower
// when it darkens, like an eye. Output is the adapted LINEAR luminance.
in vec2 vUV;
out float fragColor;

layout(binding = 0) uniform sampler2D texLogLum; // 1x1, log-average
layout(binding = 1) uniform sampler2D texPrev;   // 1x1, previous adapted

layout(std140, binding = 15) uniform U {
    float uDeltaTime;
    float uSpeedUp;    // adaptation rate when getting brighter
    float uSpeedDown;  // adaptation rate when getting darker
    float uReset;      // 1 = snap (first frame / camera cut)
};

void main() {
    float target = exp(texture(texLogLum, vec2(0.5)).r); // back to linear
    float prev = texture(texPrev, vec2(0.5)).r;
    if (uReset > 0.5 || prev <= 0.0) { fragColor = target; return; }
    float speed = target > prev ? uSpeedUp : uSpeedDown;
    // Frame-rate independent exponential approach.
    fragColor = mix(target, prev, exp(-uDeltaTime * speed));
}
