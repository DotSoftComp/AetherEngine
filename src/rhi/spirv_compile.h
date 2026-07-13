// Aether Engine — GLSL → SPIR-V via glslang. SPIR-V is what Vulkan consumes
// (and what GL 4.6 can consume too), so this is the shader half of the Vulkan
// port. No system Vulkan SDK needed — glslang is vendored/built.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace ae {

enum class ShaderStage { Vertex, Fragment };

// Compiles flattened GLSL (includes already resolved) to SPIR-V.
//   vulkanTarget = true  → Vulkan rules (uniforms MUST be in UBOs/push consts)
//   vulkanTarget = false → OpenGL target (default-block uniforms allowed)
// Returns false with a glslang log in `errOut` on failure.
bool compileGlslToSpirv(const std::string& source, ShaderStage stage, bool vulkanTarget,
                        std::vector<uint32_t>& spirvOut, std::string& errOut);

// Compiles every shader in the engine shader dir against both targets and
// prints a readiness report (which shaders already produce valid Vulkan
// SPIR-V, which still use default-block uniforms and need the UBO rework).
// Returns true when every shader compiles for the OpenGL target.
bool runSpirvAudit();

} // namespace ae
