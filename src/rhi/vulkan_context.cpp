#include "vulkan_context.h"
#include "../core/capture.h" // writeBMP
#include "spirv_compile.h"    // GLSL -> SPIR-V (glslang)
#include "volk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <cstring>

namespace ae {

namespace {
constexpr int kFramesInFlight = 2;

uint32_t findMemoryType(VkPhysicalDevice phys, uint32_t typeBits, VkMemoryPropertyFlags want) {
    VkPhysicalDeviceMemoryProperties mp{};
    vkGetPhysicalDeviceMemoryProperties(phys, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        if ((typeBits & (1u << i)) && (mp.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}
} // namespace

struct VulkanContext::Impl {
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    uint32_t gfxFamily = 0;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkSurfaceKHR surface = VK_NULL_HANDLE;

    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_SRGB;
    VkExtent2D extent{};
    std::vector<VkImage> images;
    std::vector<VkImageView> views;
    std::vector<VkFramebuffer> framebuffers;

    VkRenderPass renderPass = VK_NULL_HANDLE;
    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmd[kFramesInFlight]{};
    VkSemaphore acquireSem[kFramesInFlight]{};
    VkFence inFlight[kFramesInFlight]{};
    std::vector<VkSemaphore> renderDone; // per swapchain image
    uint32_t frame = 0;
    uint32_t lastImage = 0;

    // Optional test triangle (proves pipeline/draw + descriptor-set/UBO path).
    bool drawTriangle = false;
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet dset = VK_NULL_HANDLE;
    VkBuffer ubo = VK_NULL_HANDLE;
    VkDeviceMemory uboMem = VK_NULL_HANDLE;
    VkImage tex = VK_NULL_HANDLE;
    VkDeviceMemory texMem = VK_NULL_HANDLE;
    VkImageView texView = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;
};

static VkShaderModule makeModule(VkDevice dev, const std::vector<uint32_t>& spv) {
    VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    ci.codeSize = spv.size() * sizeof(uint32_t);
    ci.pCode = spv.data();
    VkShaderModule m = VK_NULL_HANDLE;
    vkCreateShaderModule(dev, &ci, nullptr, &m);
    return m;
}

#define VKCHECK(expr, msg)                                                     \
    do {                                                                        \
        if ((expr) != VK_SUCCESS) { if (err) *err = msg; return false; }        \
    } while (0)

bool VulkanContext::create(SDL_Window* window, std::string* err) {
    d_ = new Impl();
    Impl& v = *d_;

    if (!SDL_Vulkan_LoadLibrary(nullptr)) { if (err) *err = SDL_GetError(); return false; }
    auto gipa = (PFN_vkGetInstanceProcAddr)SDL_Vulkan_GetVkGetInstanceProcAddr();
    if (!gipa) { if (err) *err = "no vkGetInstanceProcAddr"; return false; }
    volkInitializeCustom(gipa);

    // ---- instance ----
    uint32_t extCount = 0;
    const char* const* sdlExts = SDL_Vulkan_GetInstanceExtensions(&extCount);
    if (!sdlExts) { if (err) *err = "SDL_Vulkan_GetInstanceExtensions failed"; return false; }
    std::vector<const char*> exts(sdlExts, sdlExts + extCount);
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    ici.enabledExtensionCount = (uint32_t)exts.size();
    ici.ppEnabledExtensionNames = exts.data();
    VKCHECK(vkCreateInstance(&ici, nullptr, &v.instance), "vkCreateInstance failed");
    volkLoadInstanceOnly(v.instance);

    if (!SDL_Vulkan_CreateSurface(window, v.instance, nullptr, &v.surface)) {
        if (err) *err = SDL_GetError();
        return false;
    }

    // ---- physical device + graphics/present queue ----
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(v.instance, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(v.instance, &n, pds.data());
    for (int pass = 0; pass < 2 && v.phys == VK_NULL_HANDLE; ++pass) {
        for (VkPhysicalDevice pd : pds) {
            VkPhysicalDeviceProperties props{};
            vkGetPhysicalDeviceProperties(pd, &props);
            if (pass == 0 && props.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            uint32_t qn = 0;
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, nullptr);
            std::vector<VkQueueFamilyProperties> qs(qn);
            vkGetPhysicalDeviceQueueFamilyProperties(pd, &qn, qs.data());
            for (uint32_t i = 0; i < qn; ++i) {
                VkBool32 present = VK_FALSE;
                vkGetPhysicalDeviceSurfaceSupportKHR(pd, i, v.surface, &present);
                if ((qs[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present) {
                    v.phys = pd; v.gfxFamily = i; deviceName_ = props.deviceName; break;
                }
            }
            if (v.phys) break;
        }
    }
    if (!v.phys) { if (err) *err = "no graphics+present device"; return false; }

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = v.gfxFamily;
    qci.queueCount = 1;
    qci.pQueuePriorities = &prio;
    const char* devExts[] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos = &qci;
    dci.enabledExtensionCount = 1;
    dci.ppEnabledExtensionNames = devExts;
    VKCHECK(vkCreateDevice(v.phys, &dci, nullptr, &v.device), "vkCreateDevice failed");
    volkLoadDevice(v.device);
    vkGetDeviceQueue(v.device, v.gfxFamily, 0, &v.queue);

    // ---- swapchain ----
    VkSurfaceCapabilitiesKHR caps{};
    vkGetPhysicalDeviceSurfaceCapabilitiesKHR(v.phys, v.surface, &caps);
    uint32_t fn = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &fn, nullptr);
    std::vector<VkSurfaceFormatKHR> fmts(fn);
    vkGetPhysicalDeviceSurfaceFormatsKHR(v.phys, v.surface, &fn, fmts.data());
    VkSurfaceFormatKHR fmt = fmts[0];
    for (auto& f : fmts)
        if (f.format == VK_FORMAT_B8G8R8A8_SRGB) { fmt = f; break; }
    v.format = fmt.format;
    v.extent = caps.currentExtent;
    if (v.extent.width == UINT32_MAX) { v.extent = {1280, 720}; }
    extent_[0] = v.extent.width;
    extent_[1] = v.extent.height;
    uint32_t imgCount = caps.minImageCount + 1;
    if (caps.maxImageCount && imgCount > caps.maxImageCount) imgCount = caps.maxImageCount;

    VkSwapchainCreateInfoKHR sci{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    sci.surface = v.surface;
    sci.minImageCount = imgCount;
    sci.imageFormat = v.format;
    sci.imageColorSpace = fmt.colorSpace;
    sci.imageExtent = v.extent;
    sci.imageArrayLayers = 1;
    // TRANSFER_SRC so headless verify can read the presented image back.
    sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    sci.preTransform = caps.currentTransform;
    sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    sci.clipped = VK_TRUE;
    VKCHECK(vkCreateSwapchainKHR(v.device, &sci, nullptr, &v.swapchain), "vkCreateSwapchainKHR");

    uint32_t sc = 0;
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &sc, nullptr);
    v.images.resize(sc);
    vkGetSwapchainImagesKHR(v.device, v.swapchain, &sc, v.images.data());

    // ---- render pass (clear -> present) ----
    VkAttachmentDescription color{};
    color.format = v.format;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkSubpassDependency dep{};
    dep.srcSubpass = VK_SUBPASS_EXTERNAL;
    dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    rpci.dependencyCount = 1;
    rpci.pDependencies = &dep;
    VKCHECK(vkCreateRenderPass(v.device, &rpci, nullptr, &v.renderPass), "vkCreateRenderPass");

    // ---- image views + framebuffers ----
    v.views.resize(sc);
    v.framebuffers.resize(sc);
    v.renderDone.resize(sc);
    for (uint32_t i = 0; i < sc; ++i) {
        VkImageViewCreateInfo ivci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        ivci.image = v.images[i];
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = v.format;
        ivci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VKCHECK(vkCreateImageView(v.device, &ivci, nullptr, &v.views[i]), "vkCreateImageView");
        VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fci.renderPass = v.renderPass;
        fci.attachmentCount = 1;
        fci.pAttachments = &v.views[i];
        fci.width = v.extent.width;
        fci.height = v.extent.height;
        fci.layers = 1;
        VKCHECK(vkCreateFramebuffer(v.device, &fci, nullptr, &v.framebuffers[i]),
                "vkCreateFramebuffer");
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(v.device, &semci, nullptr, &v.renderDone[i]),
                "vkCreateSemaphore");
    }

    // ---- command pool + per-frame command buffers + sync ----
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    pci.queueFamilyIndex = v.gfxFamily;
    VKCHECK(vkCreateCommandPool(v.device, &pci, nullptr, &v.cmdPool), "vkCreateCommandPool");
    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = v.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = kFramesInFlight;
    VKCHECK(vkAllocateCommandBuffers(v.device, &cbai, v.cmd), "vkAllocateCommandBuffers");
    for (int i = 0; i < kFramesInFlight; ++i) {
        VkSemaphoreCreateInfo semci{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        VKCHECK(vkCreateSemaphore(v.device, &semci, nullptr, &v.acquireSem[i]), "acquire sem");
        VkFenceCreateInfo fci{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
        fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;
        VKCHECK(vkCreateFence(v.device, &fci, nullptr, &v.inFlight[i]), "fence");
    }
    return true;
}

bool VulkanContext::enableTestTriangle(std::string* err) {
    Impl& v = *d_;
    const char* vs = R"(#version 450
layout(location=0) out vec3 vColor;
layout(location=1) out vec2 vUV;
vec2 P[3] = vec2[](vec2(0.0,-0.6), vec2(0.6,0.6), vec2(-0.6,0.6));
vec3 C[3] = vec3[](vec3(1.0,0.3,0.2), vec3(0.3,1.0,0.3), vec3(0.3,0.4,1.0));
vec2 U[3] = vec2[](vec2(0.5,0.0), vec2(1.0,1.0), vec2(0.0,1.0));
void main(){ gl_Position=vec4(P[gl_VertexIndex],0.0,1.0); vColor=C[gl_VertexIndex]; vUV=U[gl_VertexIndex]*4.0; })";
    // UBO tint (binding 0) + sampled checkerboard (binding 1) through one
    // descriptor set — proves the full descriptor path (uniform block + sampler)
    // and the texture create/upload/sample the real backend uses.
    const char* fs = R"(#version 450
layout(location=0) in vec3 vColor;
layout(location=1) in vec2 vUV;
layout(location=0) out vec4 o;
layout(std140, set=0, binding=0) uniform Tint { vec4 uTint; };
layout(set=0, binding=1) uniform sampler2D uTex;
void main(){ o=vec4(vColor*uTint.rgb*texture(uTex,vUV).rgb, 1.0); })";

    std::vector<uint32_t> vspv, fspv;
    std::string e;
    if (!compileGlslToSpirv(vs, ShaderStage::Vertex, true, vspv, e)) { if (err) *err = "vs: " + e; return false; }
    if (!compileGlslToSpirv(fs, ShaderStage::Fragment, true, fspv, e)) { if (err) *err = "fs: " + e; return false; }
    VkShaderModule vm = makeModule(v.device, vspv), fm = makeModule(v.device, fspv);

    // UBO (host-visible) holding the tint.
    float tint[4] = {1.0f, 0.5f, 0.5f, 1.0f}; // halve G/B → reddish triangle
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = sizeof(tint);
    bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT;
    vkCreateBuffer(v.device, &bci, nullptr, &v.ubo);
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(v.device, v.ubo, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemoryType(v.phys, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(v.device, &mai, nullptr, &v.uboMem);
    vkBindBufferMemory(v.device, v.ubo, v.uboMem, 0);
    void* mapped = nullptr;
    vkMapMemory(v.device, v.uboMem, 0, sizeof(tint), 0, &mapped);
    std::memcpy(mapped, tint, sizeof(tint));
    vkUnmapMemory(v.device, v.uboMem);

    // ---- 4x4 checkerboard texture (device-local, uploaded via staging) ----
    const int TW = 4, TH = 4;
    unsigned char px[TW * TH * 4];
    for (int y = 0; y < TH; ++y)
        for (int x = 0; x < TW; ++x) {
            unsigned char c = ((x ^ y) & 1) ? 255 : 60;
            unsigned char* p = &px[(y * TW + x) * 4];
            p[0] = p[1] = p[2] = c;
            p[3] = 255;
        }
    // staging buffer
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    VkBufferCreateInfo sbci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    sbci.size = sizeof(px);
    sbci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    vkCreateBuffer(v.device, &sbci, nullptr, &stage);
    VkMemoryRequirements smr{};
    vkGetBufferMemoryRequirements(v.device, stage, &smr);
    VkMemoryAllocateInfo smai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    smai.allocationSize = smr.size;
    smai.memoryTypeIndex = findMemoryType(v.phys, smr.memoryTypeBits,
                                          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    vkAllocateMemory(v.device, &smai, nullptr, &stageMem);
    vkBindBufferMemory(v.device, stage, stageMem, 0);
    void* sp = nullptr;
    vkMapMemory(v.device, stageMem, 0, sizeof(px), 0, &sp);
    std::memcpy(sp, px, sizeof(px));
    vkUnmapMemory(v.device, stageMem);
    // image
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = VK_FORMAT_R8G8B8A8_UNORM;
    ici.extent = {TW, TH, 1};
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    vkCreateImage(v.device, &ici, nullptr, &v.tex);
    VkMemoryRequirements imr{};
    vkGetImageMemoryRequirements(v.device, v.tex, &imr);
    VkMemoryAllocateInfo imai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    imai.allocationSize = imr.size;
    imai.memoryTypeIndex =
        findMemoryType(v.phys, imr.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    vkAllocateMemory(v.device, &imai, nullptr, &v.texMem);
    vkBindImageMemory(v.device, v.tex, v.texMem, 0);
    // upload: UNDEFINED -> TRANSFER_DST, copy, -> SHADER_READ_ONLY
    VkCommandBufferAllocateInfo ucbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ucbai.commandPool = v.cmdPool;
    ucbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    ucbai.commandBufferCount = 1;
    VkCommandBuffer ucb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(v.device, &ucbai, &ucb);
    VkCommandBufferBeginInfo ubi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    ubi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(ucb, &ubi);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = v.tex;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(ucb, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &b);
    VkBufferImageCopy bic{};
    bic.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    bic.imageExtent = {TW, TH, 1};
    vkCmdCopyBufferToImage(ucb, stage, v.tex, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bic);
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(ucb, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1, &b);
    vkEndCommandBuffer(ucb);
    VkSubmitInfo usi{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    usi.commandBufferCount = 1;
    usi.pCommandBuffers = &ucb;
    vkQueueSubmit(v.queue, 1, &usi, VK_NULL_HANDLE);
    vkQueueWaitIdle(v.queue);
    vkFreeCommandBuffers(v.device, v.cmdPool, 1, &ucb);
    vkDestroyBuffer(v.device, stage, nullptr);
    vkFreeMemory(v.device, stageMem, nullptr);
    // view + sampler (nearest so the 4x4 checker stays crisp)
    VkImageViewCreateInfo tvci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    tvci.image = v.tex;
    tvci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    tvci.format = VK_FORMAT_R8G8B8A8_UNORM;
    tvci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(v.device, &tvci, nullptr, &v.texView);
    VkSamplerCreateInfo saci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    saci.magFilter = saci.minFilter = VK_FILTER_NEAREST;
    saci.addressModeU = saci.addressModeV = saci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    vkCreateSampler(v.device, &saci, nullptr, &v.sampler);

    // ---- descriptor set: binding 0 = UBO, binding 1 = combined image sampler ----
    VkDescriptorSetLayoutBinding lb[2]{};
    lb[0] = {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    lb[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo dslci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    dslci.bindingCount = 2;
    dslci.pBindings = lb;
    vkCreateDescriptorSetLayout(v.device, &dslci, nullptr, &v.dsl);
    VkDescriptorPoolSize ps[2] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1;
    dpci.poolSizeCount = 2;
    dpci.pPoolSizes = ps;
    vkCreateDescriptorPool(v.device, &dpci, nullptr, &v.dpool);
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = v.dpool;
    dsai.descriptorSetCount = 1;
    dsai.pSetLayouts = &v.dsl;
    vkAllocateDescriptorSets(v.device, &dsai, &v.dset);
    VkDescriptorBufferInfo dbi{v.ubo, 0, sizeof(tint)};
    VkDescriptorImageInfo dii{v.sampler, v.texView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet wr[2]{};
    wr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[0].dstSet = v.dset;
    wr[0].dstBinding = 0;
    wr[0].descriptorCount = 1;
    wr[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    wr[0].pBufferInfo = &dbi;
    wr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr[1].dstSet = v.dset;
    wr[1].dstBinding = 1;
    wr[1].descriptorCount = 1;
    wr[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    wr[1].pImageInfo = &dii;
    vkUpdateDescriptorSets(v.device, 2, wr, 0, nullptr);

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vm;
    stages[0].pName = "main";
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fm;
    stages[1].pName = "main";

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineColorBlendAttachmentState cba{};
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    ds.dynamicStateCount = 2;
    ds.pDynamicStates = dyn;

    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &v.dsl;
    if (vkCreatePipelineLayout(v.device, &plci, nullptr, &v.pipeLayout) != VK_SUCCESS) {
        if (err) *err = "vkCreatePipelineLayout";
        return false;
    }
    VkGraphicsPipelineCreateInfo gp{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    gp.stageCount = 2;
    gp.pStages = stages;
    gp.pVertexInputState = &vi;
    gp.pInputAssemblyState = &ia;
    gp.pViewportState = &vp;
    gp.pRasterizationState = &rs;
    gp.pMultisampleState = &ms;
    gp.pColorBlendState = &cb;
    gp.pDynamicState = &ds;
    gp.layout = v.pipeLayout;
    gp.renderPass = v.renderPass;
    VkResult pr = vkCreateGraphicsPipelines(v.device, VK_NULL_HANDLE, 1, &gp, nullptr, &v.pipeline);
    vkDestroyShaderModule(v.device, vm, nullptr);
    vkDestroyShaderModule(v.device, fm, nullptr);
    if (pr != VK_SUCCESS) { if (err) *err = "vkCreateGraphicsPipelines"; return false; }
    v.drawTriangle = true;
    return true;
}

bool VulkanContext::clearFrame(float r, float g, float b) {
    Impl& v = *d_;
    vkWaitForFences(v.device, 1, &v.inFlight[v.frame], VK_TRUE, UINT64_MAX);

    uint32_t img = 0;
    VkResult acq = vkAcquireNextImageKHR(v.device, v.swapchain, UINT64_MAX,
                                         v.acquireSem[v.frame], VK_NULL_HANDLE, &img);
    if (acq == VK_ERROR_OUT_OF_DATE_KHR) return false;
    v.lastImage = img;
    vkResetFences(v.device, 1, &v.inFlight[v.frame]);

    VkCommandBuffer cb = v.cmd[v.frame];
    vkResetCommandBuffer(cb, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    VkClearValue clear{};
    clear.color = {{r, g, b, 1.0f}};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = v.renderPass;
    rp.framebuffer = v.framebuffers[img];
    rp.renderArea.extent = v.extent;
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cb, &rp, VK_SUBPASS_CONTENTS_INLINE);
    if (v.drawTriangle && v.pipeline) {
        VkViewport vpr{0, 0, (float)v.extent.width, (float)v.extent.height, 0, 1};
        VkRect2D sc{{0, 0}, v.extent};
        vkCmdSetViewport(cb, 0, 1, &vpr);
        vkCmdSetScissor(cb, 0, 1, &sc);
        vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipeline);
        vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, v.pipeLayout, 0, 1,
                                &v.dset, 0, nullptr);
        vkCmdDraw(cb, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cb);
    vkEndCommandBuffer(cb);

    VkPipelineStageFlags wait = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &v.acquireSem[v.frame];
    si.pWaitDstStageMask = &wait;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &v.renderDone[img];
    vkQueueSubmit(v.queue, 1, &si, v.inFlight[v.frame]);

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &v.renderDone[img];
    pi.swapchainCount = 1;
    pi.pSwapchains = &v.swapchain;
    pi.pImageIndices = &img;
    VkResult pr = vkQueuePresentKHR(v.queue, &pi);
    v.frame = (v.frame + 1) % kFramesInFlight;
    return pr == VK_SUCCESS || pr == VK_SUBOPTIMAL_KHR;
}

bool VulkanContext::readback(std::vector<uint8_t>& rgba, int& w, int& h) {
    Impl& v = *d_;
    vkDeviceWaitIdle(v.device);
    w = (int)v.extent.width;
    h = (int)v.extent.height;
    VkDeviceSize bytes = (VkDeviceSize)w * h * 4;

    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkBuffer buf = VK_NULL_HANDLE;
    if (vkCreateBuffer(v.device, &bci, nullptr, &buf) != VK_SUCCESS) return false;
    VkMemoryRequirements mr{};
    vkGetBufferMemoryRequirements(v.device, buf, &mr);
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size;
    mai.memoryTypeIndex = findMemoryType(v.phys, mr.memoryTypeBits,
                                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                             VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    VkDeviceMemory mem = VK_NULL_HANDLE;
    vkAllocateMemory(v.device, &mai, nullptr, &mem);
    vkBindBufferMemory(v.device, buf, mem, 0);

    VkCommandBufferAllocateInfo cbai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cbai.commandPool = v.cmdPool;
    cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cbai.commandBufferCount = 1;
    VkCommandBuffer cb = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(v.device, &cbai, &cb);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    // PRESENT_SRC -> TRANSFER_SRC on the last presented image, copy, back.
    VkImage image = v.images[v.lastImage];
    VkImageMemoryBarrier toSrc{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    toSrc.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toSrc.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toSrc.image = image;
    toSrc.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    toSrc.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    toSrc.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toSrc);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {v.extent.width, v.extent.height, 1};
    vkCmdCopyImageToBuffer(cb, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);
    VkImageMemoryBarrier toPresent = toSrc;
    toPresent.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    toPresent.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    toPresent.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    toPresent.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0,
                         0, nullptr, 0, nullptr, 1, &toPresent);
    vkEndCommandBuffer(cb);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cb;
    vkQueueSubmit(v.queue, 1, &si, VK_NULL_HANDLE);
    vkQueueWaitIdle(v.queue);

    void* mapped = nullptr;
    vkMapMemory(v.device, mem, 0, bytes, 0, &mapped);
    rgba.assign((size_t)bytes, 0);
    std::memcpy(rgba.data(), mapped, (size_t)bytes);
    vkUnmapMemory(v.device, mem);
    // Swapchain is BGRA; present it as RGBA.
    bool bgr = (v.format == VK_FORMAT_B8G8R8A8_SRGB || v.format == VK_FORMAT_B8G8R8A8_UNORM);
    if (bgr)
        for (size_t p = 0; p < rgba.size(); p += 4) std::swap(rgba[p], rgba[p + 2]);

    vkFreeCommandBuffers(v.device, v.cmdPool, 1, &cb);
    vkDestroyBuffer(v.device, buf, nullptr);
    vkFreeMemory(v.device, mem, nullptr);
    return true;
}

void VulkanContext::destroy() {
    if (!d_) return;
    Impl& v = *d_;
    if (v.device) vkDeviceWaitIdle(v.device);
    for (int i = 0; i < kFramesInFlight; ++i) {
        if (v.acquireSem[i]) vkDestroySemaphore(v.device, v.acquireSem[i], nullptr);
        if (v.inFlight[i]) vkDestroyFence(v.device, v.inFlight[i], nullptr);
    }
    for (auto s : v.renderDone) if (s) vkDestroySemaphore(v.device, s, nullptr);
    if (v.pipeline) vkDestroyPipeline(v.device, v.pipeline, nullptr);
    if (v.pipeLayout) vkDestroyPipelineLayout(v.device, v.pipeLayout, nullptr);
    if (v.dpool) vkDestroyDescriptorPool(v.device, v.dpool, nullptr);
    if (v.dsl) vkDestroyDescriptorSetLayout(v.device, v.dsl, nullptr);
    if (v.ubo) vkDestroyBuffer(v.device, v.ubo, nullptr);
    if (v.uboMem) vkFreeMemory(v.device, v.uboMem, nullptr);
    if (v.sampler) vkDestroySampler(v.device, v.sampler, nullptr);
    if (v.texView) vkDestroyImageView(v.device, v.texView, nullptr);
    if (v.tex) vkDestroyImage(v.device, v.tex, nullptr);
    if (v.texMem) vkFreeMemory(v.device, v.texMem, nullptr);
    if (v.cmdPool) vkDestroyCommandPool(v.device, v.cmdPool, nullptr);
    for (auto fb : v.framebuffers) if (fb) vkDestroyFramebuffer(v.device, fb, nullptr);
    for (auto iv : v.views) if (iv) vkDestroyImageView(v.device, iv, nullptr);
    if (v.renderPass) vkDestroyRenderPass(v.device, v.renderPass, nullptr);
    if (v.swapchain) vkDestroySwapchainKHR(v.device, v.swapchain, nullptr);
    if (v.device) vkDestroyDevice(v.device, nullptr);
    if (v.surface) vkDestroySurfaceKHR(v.instance, v.surface, nullptr);
    if (v.instance) vkDestroyInstance(v.instance, nullptr);
    delete d_;
    d_ = nullptr;
}

bool runVulkanClear(int frames, const char* bmpPath, bool triangle, std::string* deviceName,
                    std::string* err) {
    if (!SDL_Init(SDL_INIT_VIDEO)) { if (err) *err = SDL_GetError(); return false; }
    SDL_Window* w = SDL_CreateWindow("Aether Vulkan", 1280, 720,
                                     SDL_WINDOW_VULKAN | (bmpPath ? SDL_WINDOW_HIDDEN : 0));
    if (!w) { if (err) *err = SDL_GetError(); SDL_Quit(); return false; }

    VulkanContext ctx;
    if (!ctx.create(w, err)) { ctx.destroy(); SDL_DestroyWindow(w); SDL_Quit(); return false; }
    if (deviceName) *deviceName = ctx.deviceName();
    if (triangle && !ctx.enableTestTriangle(err)) {
        ctx.destroy(); SDL_DestroyWindow(w); SDL_Quit(); return false;
    }

    bool ok = true;
    for (int i = 0; i < frames; ++i)
        if (!ctx.clearFrame(0.10f, 0.55f, 0.55f)) { ok = false; break; } // teal

    if (ok && bmpPath) {
        std::vector<uint8_t> rgba;
        int rw = 0, rh = 0;
        if (ctx.readback(rgba, rw, rh)) writeBMP(bmpPath, rw, rh, rgba);
    }
    ctx.destroy();
    SDL_DestroyWindow(w);
    SDL_Vulkan_UnloadLibrary();
    SDL_Quit();
    return ok;
}

} // namespace ae
