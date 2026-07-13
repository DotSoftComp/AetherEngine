// Aether Engine — Vulkan device + swapchain + frame loop (the reusable core the
// rhi_vulkan backend is built on). Owns instance/device/queue/swapchain/render-
// pass/framebuffers/command-buffers/sync and drives acquire → record → submit →
// present. Kept separate from rhi.h so the backend can grow around it.
//
// glslang (spirv_compile.h) turns the engine's GLSL into SPIR-V at load, and
// SDL3 provides the surface — so no Vulkan SDK is required. Verified today by
// AetherRuntime --vulkan-clear (clears to a color, presents, reads back).
#pragma once
#include <cstdint>
#include <string>
#include <vector>

struct SDL_Window;

namespace ae {

class VulkanContext {
public:
    bool create(SDL_Window* window, std::string* err);
    void destroy();

    // Builds a graphics pipeline from inline GLSL (compiled to SPIR-V via
    // glslang) and draws a vertex-less RGB triangle in clearFrame — proving the
    // pipeline/shader-module/draw path the rhi_vulkan backend reuses.
    bool enableTestTriangle(std::string* err);

    // One presented frame cleared to (r,g,b) (+ the test triangle if enabled).
    // False if the swapchain needs a rebuild (resize) — caller recreates.
    bool clearFrame(float r, float g, float b);

    // Reads the most-recently-rendered swapchain image back as tightly-packed
    // RGBA8 (top-down) for headless verification/screenshots.
    bool readback(std::vector<uint8_t>& rgba, int& w, int& h);

    const std::string& deviceName() const { return deviceName_; }
    int width() const { return (int)extent_[0]; }
    int height() const { return (int)extent_[1]; }

private:
    struct Impl;
    Impl* d_ = nullptr; // pimpl keeps <volk.h>/<vulkan.h> out of the header
    std::string deviceName_;
    uint32_t extent_[2] = {0, 0};
};

// Self-contained: SDL init + Vulkan window + context, clear `frames` frames to a
// recognizable teal, optionally screenshot the readback to `bmpPath`, tear down.
// `triangle` also builds+draws the test pipeline. Drives --vulkan-clear /
// --vulkan-triangle.
bool runVulkanClear(int frames, const char* bmpPath, bool triangle, std::string* deviceName,
                    std::string* err);

} // namespace ae
