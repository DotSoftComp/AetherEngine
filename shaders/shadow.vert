#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 4) in vec4 aJoints; // palette indices as float (cast below)
layout(location = 5) in vec4 aWeights;

layout(std140, binding = 15) uniform U {
    mat4 uModel;
    mat4 uLightMat;
    int uSkinned;
    int uInstanced;
};

layout(std430, binding = 1) readonly buffer InstanceMatrices {
    mat4 uInstanceModel[];
};

layout(std140, binding = 0) uniform JointPalette {
    mat4 uJoints[128];
};

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
    gl_Position = uLightMat * model * localPos;
}
