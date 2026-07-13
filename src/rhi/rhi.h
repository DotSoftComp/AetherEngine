// Aether Engine — RHI (render hardware interface).
//
// The API-neutral seam between the engine and the graphics backend. Engine
// code that goes through ae::rhi never names an OpenGL/Vulkan/D3D type, so a
// second backend is an rhi_<api>.cpp away instead of a rewrite. OpenGL 4.5 is
// the first (and currently only) backend (rhi_gl.cpp).
//
// Migration status (strangler pattern — new code MUST use rhi):
//   phase 1 (done)  pipeline state, mesh geometry + draws, storage buffers
//   phase 2 (done)  textures/samplers (2D/cube/array/compressed), framebuffers
//                   (MRT/layers/depth-only), vertex streams, fullscreen draws,
//                   GPU timers, uniform buffers, shaders (GLSL source), clear/
//                   blit/readback. The engine now makes ZERO direct GL calls
//                   outside this backend (editor-only exceptions: the vendored
//                   ImGui GL backend in editor/imgui_layer.cpp, and WGL context
//                   creation in core/window.cpp).
//   phase 3 (todo)  shader cross-compilation (GLSL -> SPIR-V -> HLSL/MSL),
//                   context/swapchain creation behind rhi, redundant-state
//                   caching (needs invalidate() around ImGui), then the second
//                   backend itself (D3D12 or Vulkan) — implement rhi_<api>.cpp
//                   against this exact header.
//
// Handles are opaque ids (0 = null); what they map to is the backend's
// business. All calls must come from the render thread with a live context.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace ae {
namespace rhi {

// ---- device -----------------------------------------------------------------
enum class Backend { OpenGL };

struct DeviceInfo {
    Backend backend = Backend::OpenGL;
    const char* backendName = "OpenGL";
    std::string device;  // GPU name as reported by the driver
    std::string version; // API/driver version string
};

// Call once after the context exists (hosts do this right after Window::create).
// Logs one "[RHI] ..." line identifying the backend + device.
bool init();
const DeviceInfo& info();

// ---- pipeline state ----------------------------------------------------------
// The complete fixed-function state the engine uses, applied as one value —
// no more scattered enable/disable pairs leaking state between passes.
enum class Blend {
    Off,
    Alpha,         // src_alpha, 1 - src_alpha
    Additive,      // one, one
    Premultiplied, // one, 1 - src_alpha
};

struct RenderState {
    bool depthTest = true;
    bool depthWrite = true;
    Blend blend = Blend::Off;
    bool cullBack = true; // back-face culling (the engine never front-culls)
};

void setState(const RenderState& s);
// Dynamic overrides for the two states that legitimately vary per draw
// (double-sided materials, per-batch particle blending).
void setCull(bool cullBack);
void setBlend(Blend blend);
void setViewport(int x, int y, int width, int height);
void setScissor(bool enabled, int x = 0, int y = 0, int width = 0, int height = 0);

// ---- geometry (vertex/index data + layout, one draw call) --------------------
struct VertexAttr {
    int location;      // shader attribute location
    int components;    // 1..4 floats (the engine's formats are all-float)
    unsigned offset;   // byte offset inside the vertex
};

struct GeometryDesc {
    const void* vertexData = nullptr;
    size_t vertexBytes = 0;
    unsigned vertexStride = 0;
    const uint32_t* indexData = nullptr; // may be null (non-indexed)
    size_t indexCount = 0;
    const VertexAttr* attrs = nullptr;
    int attrCount = 0;
    bool dynamic = false; // updated per frame (streaming)
};

struct GeometryHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

GeometryHandle createGeometry(const GeometryDesc& desc);
void destroyGeometry(GeometryHandle& g); // nulls the handle
// Re-uploads the vertex buffer in place (byte count must not exceed the
// original allocation). For geometry created with dynamic = true.
void updateGeometryVertices(GeometryHandle g, const void* data, size_t bytes);
// Indexed triangle draw; instances > 1 issues an instanced draw.
void draw(GeometryHandle g, unsigned indexCount, int instances = 1);

// ---- textures -----------------------------------------------------------------
// Sampled textures (assets, procedural) and render-target textures share one
// handle type. Formats cover exactly what the engine uses.
enum class TexFormat {
    RGBA8,       // linear 8-bit
    SRGBA8,      // sRGB 8-bit
    BC1,         // block-compressed opaque (DXT1)
    BC1_SRGB,
    BC3,         // block-compressed with alpha (DXT5)
    BC3_SRGB,
    R8,          // render targets
    RG16F,
    RGBA16F,
    R11G11B10F,
    Depth32F,
};

struct SamplerDesc {
    bool linear = true;         // false = nearest
    bool mipmaps = true;        // trilinear min filter when true
    bool repeat = true;         // false = clamp-to-edge
    float anisotropy = 0.0f;    // 0 = off
    bool clampToBorder = false; // overrides repeat (shadow maps)
    float borderColor[4] = {1, 1, 1, 1};
    bool shadowCompare = false; // depth-compare sampling (<=)
};

struct TextureHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

TextureHandle createTexture2D(int width, int height, int mipLevels, TexFormat format);
TextureHandle createTextureCube(int size, int mipLevels, TexFormat format);
TextureHandle createTexture2DArray(int width, int height, int layers, TexFormat format);
// Uncompressed RGBA8 upload into one mip level.
void uploadTexture2D(TextureHandle t, int level, int width, int height, const void* rgba);
// Pre-encoded BC blocks into one mip level (format fixed at create).
void uploadTexture2DCompressed(TextureHandle t, int level, int width, int height,
                               size_t byteCount, const void* blocks);
void generateMips(TextureHandle t);
void setSampler(TextureHandle t, const SamplerDesc& s);
void bindTexture(int slot, TextureHandle t); // null handle unbinds the slot
// Bind by raw handle id (Material stores plain ids for cheap comparison).
inline void bindTexture(int slot, unsigned id) { bindTexture(slot, TextureHandle{id}); }
void destroyTexture(TextureHandle& t);
// Backend-native handle for UI interop (the editor hands render-target
// textures to Dear ImGui as ImTextureID). Never persist the value.
uintptr_t nativeTextureHandle(TextureHandle t);

// ---- framebuffers (render targets; the null handle {0} = the backbuffer) -----
struct FramebufferHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

FramebufferHandle createFramebuffer();
void attachColor(FramebufferHandle f, int index, TextureHandle t, int mip = 0);
// One face/layer of a cube map (layer = face 0..5) or 2D array.
void attachColorLayer(FramebufferHandle f, int index, TextureHandle t, int mip, int layer);
void attachDepth(FramebufferHandle f, TextureHandle t);
void attachDepthLayer(FramebufferHandle f, TextureHandle t, int mip, int layer);
// How many color attachments the fragment shader writes (0 = depth-only).
void setDrawBufferCount(FramebufferHandle f, int count);
bool framebufferComplete(FramebufferHandle f);
void bindFramebuffer(FramebufferHandle f); // {0} = backbuffer
void destroyFramebuffer(FramebufferHandle& f);

void clear(bool color, float r, float g, float b, float a, bool depth);
// Stretch-copy a framebuffer's color onto the backbuffer (resolution scaling).
void blitToBackbuffer(FramebufferHandle src, int srcW, int srcH, int dstW, int dstH);
// Read the backbuffer as tightly-packed RGBA8 (screenshots).
void readBackbuffer(int width, int height, void* rgbaOut);
// Read a framebuffer's color attachment 0 (agent-bridge viewport screenshots).
void readFramebuffer(FramebufferHandle f, int width, int height, void* rgbaOut);

// ---- vertex streams (per-frame data: particles, debug lines, UI quads) --------
enum class Topology { Triangles, Lines };

struct StreamAttr {
    int location;
    int components;
    unsigned offset;
    bool unorm8 = false; // normalized u8 (vertex colors); default float
};

struct StreamHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

StreamHandle createStream(unsigned vertexStride, const StreamAttr* attrs, int attrCount);
void setStreamData(StreamHandle s, const void* data, size_t bytes); // grows as needed
void drawStream(StreamHandle s, Topology topology, int firstVertex, int vertexCount);
void destroyStream(StreamHandle& s);

// Fullscreen triangle with no vertex inputs (the shader synthesizes positions).
void drawFullscreen();

// ---- shaders --------------------------------------------------------------------
// Programs are created from GLSL 450 source today; offline cross-compilation
// (GLSL -> SPIR-V -> HLSL/MSL) is the remaining phase-3 gap for a non-GL
// backend. Uniforms are set on the currently-used program via cached
// locations (the Shader wrapper in render/shader.h owns the name cache).
struct ShaderHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

ShaderHandle createShader(const std::string& vsSource, const std::string& fsSource,
                          const char* debugLabel);
void useShader(ShaderHandle s);
void destroyShader(ShaderHandle& s);
int uniformLocation(ShaderHandle s, const char* name); // -1 = not found
void setUniform1i(int loc, int v);
void setUniform1f(int loc, float v);
void setUniform2f(int loc, float x, float y);
void setUniform3f(int loc, float x, float y, float z);
void setUniform4f(int loc, float x, float y, float z, float w);
void setUniformMat4(int loc, const float* m16, int count = 1);
void setUniform1fv(int loc, const float* v, int count);
void setUniform3fv(int loc, const float* v, int count);

// ---- GPU timers ----------------------------------------------------------------
struct TimerHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};
TimerHandle createTimer();
void destroyTimer(TimerHandle& t);
void beginTimer(TimerHandle t);
void endTimer();
// Nanoseconds of the last begin/end pair. Callers double-buffer their timers
// and read a frame late, so this may block only in degenerate cases.
unsigned long long timerNs(TimerHandle t);

// ---- storage buffers (shader-visible arrays, e.g. instance matrices) ---------
struct BufferHandle {
    unsigned id = 0;
    bool valid() const { return id != 0; }
};

BufferHandle createStorageBuffer();
// (Re)allocates and fills — growth is the backend's problem, call freely.
void setBufferData(BufferHandle b, const void* data, size_t bytes);
void bindStorageBuffer(int slot, BufferHandle b);
void destroyBuffer(BufferHandle& b); // nulls the handle

// Uniform buffers share BufferHandle: fixed-size, bound to a slot (e.g. the
// GPU-skinning joint palette at slot 0).
BufferHandle createUniformBuffer(size_t bytes);
void updateUniformBuffer(BufferHandle b, const void* data, size_t bytes);
void bindUniformBuffer(int slot, BufferHandle b);

} // namespace rhi
} // namespace ae
