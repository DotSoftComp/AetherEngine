#include "spirv_compile.h"
#include "../render/shader.h" // loadShaderSource (flattens #include)
#include "../core/paths.h"
#include <glslang/Public/ShaderLang.h>
#include <glslang/Public/ResourceLimits.h>
#include <SPIRV/GlslangToSpv.h>
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h> // shader-dir enumeration (desktop audit tool only)
#include <cstdio>

namespace ae {

namespace {
struct GlslangGuard {
    GlslangGuard() { glslang::InitializeProcess(); }
    ~GlslangGuard() { glslang::FinalizeProcess(); }
};
EShLanguage toEsh(ShaderStage s) {
    return s == ShaderStage::Vertex ? EShLangVertex : EShLangFragment;
}
// Inject the AE_VID/AE_IID builtin shim after the #version line — the Vulkan
// variant uses gl_VertexIndex/gl_InstanceIndex (mirrors rhi_gl's GL variant).
std::string withCompatDefines(const std::string& src, bool vulkanTarget) {
    size_t nl = src.find('\n');
    if (nl == std::string::npos) return src;
    const char* defs = vulkanTarget
        ? "#define AE_VID gl_VertexIndex\n#define AE_IID gl_InstanceIndex\n"
        : "#define AE_VID gl_VertexID\n#define AE_IID gl_InstanceID\n";
    return src.substr(0, nl + 1) + defs + src.substr(nl + 1);
}
} // namespace

bool compileGlslToSpirv(const std::string& sourceIn, ShaderStage stage, bool vulkanTarget,
                        std::vector<uint32_t>& spirvOut, std::string& errOut) {
    static GlslangGuard guard; // one process-wide init

    EShLanguage lang = toEsh(stage);
    std::string source = withCompatDefines(sourceIn, vulkanTarget);
    glslang::TShader shader(lang);
    const char* src = source.c_str();
    shader.setStrings(&src, 1);

    if (vulkanTarget) {
        shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_1);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_3);
    } else {
        shader.setEnvInput(glslang::EShSourceGlsl, lang, glslang::EShClientOpenGL, 100);
        shader.setEnvClient(glslang::EShClientOpenGL, glslang::EShTargetOpenGL_450);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);
    }
    shader.setAutoMapLocations(true);
    shader.setAutoMapBindings(true);

    const int defaultVersion = 450;
    EShMessages msgs = (EShMessages)(EShMsgSpvRules | (vulkanTarget ? EShMsgVulkanRules : 0));
    if (!shader.parse(GetDefaultResources(), defaultVersion, false, msgs)) {
        errOut = shader.getInfoLog();
        return false;
    }
    glslang::TProgram program;
    program.addShader(&shader);
    if (!program.link(msgs)) {
        errOut = program.getInfoLog();
        return false;
    }
    std::vector<unsigned int> spv;
    glslang::GlslangToSpv(*program.getIntermediate(lang), spv);
    spirvOut.assign(spv.begin(), spv.end());
    return true;
}

bool runSpirvAudit() {
    std::string dir = engineShaderDir(); // trailing backslash
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) {
        std::printf("SPIRV-AUDIT: no shader dir at %s\n", dir.c_str());
        return false;
    }
    int total = 0, glOk = 0, vkOk = 0;
    std::printf("SPIRV-AUDIT (glslang) — shader dir: %s\n", dir.c_str());
    do {
        std::string name = fd.cFileName;
        ShaderStage stage;
        if (name.size() > 5 && name.substr(name.size() - 5) == ".vert") stage = ShaderStage::Vertex;
        else if (name.size() > 5 && name.substr(name.size() - 5) == ".frag") stage = ShaderStage::Fragment;
        else continue; // skip .glsl includes (compiled transitively)

        ++total;
        std::string source = loadShaderSource(name.c_str());
        std::vector<uint32_t> spv;
        std::string glErr, vkErr;
        bool gl = compileGlslToSpirv(source, stage, /*vulkan=*/false, spv, glErr);
        size_t glWords = spv.size();
        spv.clear();
        bool vk = compileGlslToSpirv(source, stage, /*vulkan=*/true, spv, vkErr);
        glOk += gl; vkOk += vk;

        std::printf("  %-22s  GL:%s  VK:%s%s\n", name.c_str(), gl ? "ok " : "FAIL",
                    vk ? "ok" : "needs-UBO",
                    gl ? (" (" + std::to_string(glWords) + " words)").c_str() : "");
        if (!gl) {
            std::string first = glErr.substr(0, glErr.find('\n'));
            std::printf("      GL error: %s\n", first.c_str());
        }
    } while (FindNextFileA(h, &fd));
    FindClose(h);

    std::printf("SPIRV-AUDIT: %d shaders — GL-SPIRV ok %d/%d, Vulkan-ready %d/%d\n", total, glOk,
                total, vkOk, total);
    return glOk == total;
}

} // namespace ae
