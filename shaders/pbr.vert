#version 450 core
layout(location = 0) in vec3 aPosition;
layout(location = 1) in vec3 aNormal;
layout(location = 2) in vec4 aTangent;
layout(location = 3) in vec2 aUV;
layout(location = 4) in vec4 aJoints; // palette indices as float (cast below)
layout(location = 5) in vec4 aWeights;

uniform mat4 uModel;
uniform mat4 uView;
uniform mat4 uViewProj;
uniform int uSkinned;
uniform int uInstanced; // 1: model matrix comes from the instance buffer

layout(std140, binding = 0) uniform JointPalette {
    mat4 uJoints[128];
};
layout(std430, binding = 1) readonly buffer InstanceMatrices {
    mat4 uInstanceModel[];
};

out VS_OUT {
    vec3 worldPos;
    vec3 normal;
    vec3 tangent;
    vec3 bitangent;
    vec2 uv;
    float viewDepth;
} vs;

void main() {
    vec4 localPos = vec4(aPosition, 1.0);
    vec3 localNrm = aNormal;
    vec3 localTan = aTangent.xyz;

    if (uSkinned == 1) {
        mat4 skin = aWeights.x * uJoints[uint(aJoints.x + 0.5)] +
                    aWeights.y * uJoints[uint(aJoints.y + 0.5)] +
                    aWeights.z * uJoints[uint(aJoints.z + 0.5)] +
                    aWeights.w * uJoints[uint(aJoints.w + 0.5)];
        localPos = skin * localPos;
        localNrm = mat3(skin) * localNrm;
        localTan = mat3(skin) * localTan;
    }

    mat4 model = uInstanced != 0 ? uInstanceModel[gl_InstanceID] : uModel;
    vec4 world = model * localPos;
    mat3 normalMat = transpose(inverse(mat3(model)));

    vs.worldPos = world.xyz;
    vs.normal = normalize(normalMat * localNrm);
    vs.tangent = normalize(mat3(model) * localTan);
    vs.bitangent = cross(vs.normal, vs.tangent) * aTangent.w;
    vs.uv = aUV;
    vs.viewDepth = -(uView * world).z;

    gl_Position = uViewProj * world;
}
