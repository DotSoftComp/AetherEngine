#include "shader.h"
#include "../core/paths.h"
#include <cstdio>
#include <fstream>
#include <sstream>
#include <vector>

namespace ae {

// Shaders ship with the engine install, next to AetherCore.dll.
std::string shaderRoot() {
    return engineShaderDir();
}

static std::string readFileWithIncludes(const std::string& path, int depth = 0) {
    if (depth > 8) return "";
    std::ifstream f(path);
    if (!f) {
        std::fprintf(stderr, "[Shader] cannot open %s\n", path.c_str());
        return "";
    }
    std::string dir;
    size_t slash = path.find_last_of("\\/");
    if (slash != std::string::npos) dir = path.substr(0, slash + 1);

    std::ostringstream out;
    std::string line;
    while (std::getline(f, line)) {
        size_t p = line.find("#include");
        if (p != std::string::npos && line.find_first_not_of(" \t") == p) {
            size_t q1 = line.find('"', p);
            size_t q2 = (q1 == std::string::npos) ? std::string::npos : line.find('"', q1 + 1);
            if (q2 != std::string::npos) {
                std::string inc = line.substr(q1 + 1, q2 - q1 - 1);
                out << readFileWithIncludes(dir + inc, depth + 1) << "\n";
                continue;
            }
        }
        out << line << "\n";
    }
    return out.str();
}

std::string loadShaderSource(const char* relPath) {
    return readFileWithIncludes(shaderRoot() + relPath);
}

bool Shader::load(const char* vsPath, const char* fsPath) {
    std::string root = shaderRoot();
    std::string vsSrc = readFileWithIncludes(root + vsPath);
    std::string fsSrc = readFileWithIncludes(root + fsPath);
    if (vsSrc.empty() || fsSrc.empty()) return false;
    std::string label = std::string(vsPath) + " + " + fsPath;
    return loadFromSource(vsSrc, fsSrc, label.c_str());
}

bool Shader::loadFromSource(const std::string& vsSrc, const std::string& fsSrc,
                            const char* label) {
    rhi::ShaderHandle fresh = rhi::createShader(vsSrc, fsSrc, label);
    if (!fresh.valid()) return false;
    destroy(); // re-link support (material-graph hot reload)
    program_ = fresh;
    return true;
}

void Shader::destroy() {
    rhi::destroyShader(program_);
    uniforms_.clear();
}

void Shader::use() const { rhi::useShader(program_); }

int Shader::loc(const char* name) {
    auto it = uniforms_.find(name);
    if (it != uniforms_.end()) return it->second;
    int l = rhi::uniformLocation(program_, name);
    uniforms_.emplace(name, l);
    return l;
}

void Shader::setInt(const char* n, int v)             { rhi::setUniform1i(loc(n), v); }
void Shader::setFloat(const char* n, float v)         { rhi::setUniform1f(loc(n), v); }
void Shader::setVec2(const char* n, float x, float y) { rhi::setUniform2f(loc(n), x, y); }
void Shader::setVec3(const char* n, const Vec3& v)    { rhi::setUniform3f(loc(n), v.x, v.y, v.z); }
void Shader::setVec4(const char* n, const Vec4& v) {
    rhi::setUniform4f(loc(n), v.x, v.y, v.z, v.w);
}
void Shader::setMat4(const char* n, const Mat4& m) { rhi::setUniformMat4(loc(n), m.data()); }
void Shader::setMat4Array(const char* n, const Mat4* m, int count) {
    rhi::setUniformMat4(loc(n), m->data(), count);
}
void Shader::setFloatArray(const char* n, const float* v, int count) {
    rhi::setUniform1fv(loc(n), v, count);
}
void Shader::setVec3Array(const char* n, const Vec3* v, int count) {
    rhi::setUniform3fv(loc(n), &v->x, count);
}

} // namespace ae
