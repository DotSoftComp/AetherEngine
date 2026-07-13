// Aether Engine — Vulkan bring-up probe. Creates a full instance → physical
// device → surface → logical device → swapchain against an SDL Vulkan window,
// reports the GPU, and tears down. This is the reusable device/swapchain path
// that the real rhi_vulkan backend (and every console backend) builds on;
// proving it here de-risks Vulkan on the target hardware without touching the
// working GL renderer.
#pragma once
#include <string>

struct SDL_Window;

namespace ae {

// Returns true on a clean full bring-up. On success fills `deviceName`
// (+ apiVersion/present-mode detail); on failure fills `err`.
bool vulkanProbe(SDL_Window* window, std::string* deviceName, std::string* err);

// Self-contained: inits SDL, creates a hidden Vulkan window, probes, tears
// everything down. Used by `AetherRuntime --vulkan-probe`.
bool runVulkanProbe(std::string* deviceName, std::string* err);

} // namespace ae
