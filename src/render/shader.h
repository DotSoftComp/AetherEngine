// Aether Engine — GLSL program wrapper with file loading, #include resolution,
// and name-cached uniform setters.
#pragma once
#include "../rhi/rhi.h"
#include "../core/math3d.h"
#include <string>
#include <unordered_map>

namespace ae {

// Resolves the directory that holds the runtime "shaders/" folder
// (next to the executable, or up to three parent directories above it).
std::string shaderRoot();

// Reads a shader file (relative to shaderRoot()) with #include resolution —
// the exact source Shader::load would compile. Used by the material-graph
// codegen to derive pbr.frag variants.
std::string loadShaderSource(const char* relPath);

class Shader {
public:
    // Paths are relative to shaderRoot(), e.g. "pbr.vert".
    bool load(const char* vsPath, const char* fsPath);
    // Compiles from in-memory source (includes already resolved).
    bool loadFromSource(const std::string& vsSrc, const std::string& fsSrc, const char* label);
    void destroy();

    void use() const;
    unsigned id() const { return program_.id; } // opaque rhi shader id

    void setInt(const char* name, int v);
    void setFloat(const char* name, float v);
    void setVec2(const char* name, float x, float y);
    void setVec3(const char* name, const Vec3& v);
    void setVec4(const char* name, const Vec4& v);
    void setMat4(const char* name, const Mat4& m);
    void setMat4Array(const char* name, const Mat4* m, int count);
    void setFloatArray(const char* name, const float* v, int count);
    void setVec3Array(const char* name, const Vec3* v, int count);

private:
    int loc(const char* name);
    rhi::ShaderHandle program_;
    std::unordered_map<std::string, int> uniforms_;
};

} // namespace ae
