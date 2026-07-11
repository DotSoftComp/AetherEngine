#version 450 core
// Debug lines render into the HDR buffer pre-tonemap; boost so they stay
// readable after ACES.
in vec3 vColor;
out vec4 fragColor;
void main() { fragColor = vec4(vColor * 2.5, 0.65); }
