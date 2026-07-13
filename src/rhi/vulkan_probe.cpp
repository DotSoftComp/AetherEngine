#include "vulkan_probe.h"
#include "volk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <cstdio>
#include <vector>

namespace ae {

namespace {
std::string fail(std::string* err, const char* msg, VkResult r = VK_SUCCESS) {
    std::string s = msg;
    if (r != VK_SUCCESS) s += " (VkResult " + std::to_string((int)r) + ")";
    if (err) *err = s;
    return s;
}
} // namespace

bool vulkanProbe(SDL_Window* window, std::string* deviceName, std::string* err) {
    // 1. Loader: resolve vkGetInstanceProcAddr through SDL (works the same on
    //    Windows/Linux/Android — no platform loader lib).
    if (!SDL_Vulkan_LoadLibrary(nullptr)) return fail(err, SDL_GetError()), false;
    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) return fail(err, "SDL_Vulkan_GetVkGetInstanceProcAddr returned null"), false;
    volkInitializeCustom(gipa);

    // 2. Instance with the surface extensions SDL requires for this platform.
    Uint32 extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    if (!sdlExts) return fail(err, "SDL_Vulkan_GetInstanceExtensions failed"), false;
    std::vector<const char*> exts(sdlExts, sdlExts + extCount);

    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "Aether";
    app.apiVersion = VK_API_VERSION_1_1; // broad: desktop + Android + Switch
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();

    VkInstance instance = VK_NULL_HANDLE;
    VkResult r = vkCreateInstance(&ici, nullptr, &instance);
    if (r != VK_SUCCESS) return fail(err, "vkCreateInstance", r), false;
    volkLoadInstanceOnly(instance);

    auto cleanupInstance = [&]() { vkDestroyInstance(instance, nullptr); };

    // 3. Surface from the SDL window (SDL owns the platform-specific call).
    VkSurfaceKHR surface = VK_NULL_HANDLE;
    if (!SDL_Vulkan_CreateSurface(window, instance, nullptr, &surface)) {
        cleanupInstance();
        return fail(err, SDL_GetError()), false;
    }
    auto cleanupSurface = [&]() { vkDestroySurfaceKHR(instance, surface, nullptr); cleanupInstance(); };

    // 4. Physical device: prefer a discrete GPU that can present on this surface.
    uint32_t pdCount = 0;
    vkEnumeratePhysicalDevices(instance, &pdCount, nullptr);
    if (pdCount == 0) { cleanupSurface(); return fail(err, "no Vulkan physical devices"), false; }
    std::vector<VkPhysicalDevice> pds(pdCount);
    vkEnumeratePhysicalDevices(instance, &pdCount, pds.data());

    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t graphicsQueue = UINT32_MAX;
    VkPhysicalDeviceProperties chosenProps{};
    for (int pass = 0; pass < 2 && phys == VK_NULL_HANDLE; ++pass) {
        for (VkPhysicalDevice pd : pds) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            bool discrete = props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU;
            if (pass == 0 && !discrete) continue; // first pass: discrete only

            uint32_t qfCount = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, nullptr);
            std::vector<VkQueueFamilyProperties> qfs(qfCount);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qfCount, qfs.data());
            for (uint32_t i = 0; i < qfCount; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, surface, &present);
                if ((qfs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    phys = pd; graphicsQueue = i; chosenProps = props; break;
                }
            }
            if (phys != VK_NULL_HANDLE) break;
        }
    }
    if (phys == VK_NULL_HANDLE) { cleanupSurface(); return fail(err, "no graphics+present queue"), false; }

    // 5. Logical device with the swapchain extension.
    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = graphicsQueue;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;

    VkDevice device = VK_NULL_HANDLE;
    r = vkCreateDevice(phys, &dci, nullptr, &device);
    if (r != VK_SUCCESS) { cleanupSurface(); return fail(err, "vkCreateDevice", r), false; }
    volkLoadDevice(device);
    auto cleanupDevice = [&]() { vkDestroyDevice(device, nullptr); cleanupSurface(); };

    // 6. Swapchain — the part most likely to expose a platform mismatch.
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(phys, surface, &caps);
    uint32_t fmtCount = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, nullptr);
    if (fmtCount == 0) { cleanupDevice(); return fail(err, "no surface formats"), false; }
    std::vector<VkSurfaceFormatKHR> fmts(fmtCount);
    vkGetPhysicalDeviceSurfaceFormatsKHR(phys, surface, &fmtCount, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (const auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) { fmt = f; break; }

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) { extent.width = 1280; extent.height = 720; }
    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = surface;
    sci.minImageCount = imgCount;
    sci.imageFormat = fmt.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = extent;
    sci.imageArrayLayers = 1;
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR; // guaranteed present mode
    sci.clipped = VK_TRUE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    r = vkCreateSwapchainKHR(device, &sci, nullptr, &swapchain);
    if (r != VK_SUCCESS) { cleanupDevice(); return fail(err, "vkCreateSwapchainKHR", r), false; }

    uint32_t scImgs = 0;
    vkGetSwapchainImagesKHR(device, swapchain, &scImgs, nullptr);

    if (deviceName) {
        char buf[512];
        std::snprintf(buf, sizeof(buf),
                      "%s (Vulkan %u.%u.%u, %ux%u swapchain, %u images, format %d)",
                      chosenProps.deviceName, VK_API_VERSION_MAJOR(chosenProps.apiVersion),
                      VK_API_VERSION_MINOR(chosenProps.apiVersion),
                      VK_API_VERSION_PATCH(chosenProps.apiVersion), extent.width, extent.height,
                      scImgs, (int)fmt.format);
        *deviceName = buf;
    }

    vkDestroySwapchainKHR(device, swapchain, nullptr);
    cleanupDevice();
    return true;
}

bool runVulkanProbe(std::string* deviceName, std::string* err) {
    if (!SDL_Init(SDL_INIT_VIDEO)) return fail(err, SDL_GetError()), false;
    SDL_Window* w = SDL_CreateWindow("Aether Vulkan Probe", 640, 360,
                                     SDL_WINDOW_VULKAN | SDL_WINDOW_HIDDEN);
    if (!w) { std::string e = SDL_GetError(); SDL_Quit(); return fail(err, e.c_str()), false; }
    bool ok = vulkanProbe(w, deviceName, err);
    SDL_DestroyWindow(w);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return ok;
}

} // namespace ae
