#version 450 core
// Draws the captured sky cubemap behind the scene (far-plane fullscreen pass),
// writing into the same MRT layout as the opaque pass.
in vec2 vUV;
layout(location = 0) out vec4 outDirect;
layout(location = 1) out vec4 outAmbient;
layout(location = 2) out vec4 outNormal;

layout(binding = 0) uniform samplerCube texEnv;
uniform mat4 uInvViewProj;
uniform vec3 uCamPos;

void main() {
    vec4 world = uInvViewProj * vec4(vUV * 2.0 - 1.0, 1.0, 1.0);
    vec3 dir = normalize(world.xyz / world.w - uCamPos);
    outDirect = vec4(textureLod(texEnv, dir, 0.0).rgb, 1.0);
    outAmbient = vec4(0.0);
    outNormal = vec4(0.5, 0.5, 1.0, 1.0);
}
