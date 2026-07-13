#version 450 core
// Point-light shadow depth: renders each cube face, passing world position so
// the fragment stage can write linear light-space distance (see pointdepth.frag).
layout(location = 0) in vec3 aPosition;
layout(location = 4) in vec4 aJoints;
layout(location = 5) in vec4 aWeights;

layout(std140, binding = 15) uniform U {
    mat4 uModel;
    mat4 uLightMat;   // this face's viewProj
    int  uSkinned;
    int  uInstanced;
    vec3 uLightPos;   // world-space light position
    float uFar;       // light range (normalizer for stored distance)
};

layout(std430, binding = 1) readonly buffer InstanceMatrices {
    mat4 uInstanceModel[];
};
layout(std140, binding = 0) uniform JointPalette {
    mat4 uJoints[128];
};

out vec3 vWorld;

void main() {
    vec4 localPos = vec4(aPosition, 1.0);
    if (uSkinned == 1) {
        mat4 skin = aWeights.x * uJoints[uint(aJoints.x + 0.5)] +
                    aWeights.y * uJoints[uint(aJoints.y + 0.5)] +
                    aWeights.z * uJoints[uint(aJoints.z + 0.5)] +
                    aWeights.w * uJoints[uint(aJoints.w + 0.5)];
        localPos = skin * localPos;
    }
    mat4 model = uInstanced != 0 ? uInstanceModel[AE_IID] : uModel;
    vec4 world = model * localPos;
    vWorld = world.xyz;
    gl_Position = uLightMat * world;
}
