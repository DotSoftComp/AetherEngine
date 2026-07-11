// Aether Engine — RHI backend: OpenGL 4.5 (DSA).
//
// Handles map to GL object names through small tables so the ids the engine
// holds stay backend-agnostic. State is applied unconditionally for now —
// redundant-state caching is phase 2 (it needs an invalidate() hook around
// foreign GL code like the ImGui backend).
#include "rhi.h"
#include "../gl/gl_api.h"
#include "../core/log.h"
#include <vector>

namespace ae {
namespace rhi {

namespace {

DeviceInfo g_info;

struct GLGeometry {
    GLuint vao = 0, vbo = 0, ibo = 0;
    bool alive = false;
};
struct GLBuffer {
    GLuint buf = 0;
    bool alive = false;
};

// Slot 0 is reserved as the null handle; freed slots are reused.
std::vector<GLGeometry>& geometries() {
    static std::vector<GLGeometry> t(1);
    return t;
}
std::vector<GLBuffer>& buffers() {
    static std::vector<GLBuffer> t(1);
    return t;
}

template <typename T>
unsigned allocSlot(std::vector<T>& table) {
    for (size_t i = 1; i < table.size(); ++i)
        if (!table[i].alive) return (unsigned)i;
    table.push_back({});
    return (unsigned)(table.size() - 1);
}

} // namespace

// ---- device -----------------------------------------------------------------

bool init() {
    const char* device = (const char*)glGetString(GL_RENDERER);
    const char* version = (const char*)glGetString(GL_VERSION);
    if (!device || !version) {
        AE_ERROR("[RHI] no live GL context at rhi::init");
        return false;
    }
    g_info.backend = Backend::OpenGL;
    g_info.backendName = "OpenGL";
    g_info.device = device;
    g_info.version = version;
    // Backend-wide defaults the engine assumes everywhere.
    glDepthFunc(GL_LEQUAL);
    glEnable(GL_TEXTURE_CUBE_MAP_SEAMLESS);
#ifndef NDEBUG
    if (glDebugMessageCallback) {
        glEnable(GL_DEBUG_OUTPUT);
        glEnable(GL_DEBUG_OUTPUT_SYNCHRONOUS);
        glDebugMessageCallback(
            [](GLenum, GLenum, GLuint, GLenum severity, GLsizei, const GLchar* message,
               const void*) {
                if (severity == GL_DEBUG_SEVERITY_HIGH || severity == GL_DEBUG_SEVERITY_MEDIUM)
                    AE_ERROR("[GL] %s", message);
            },
            nullptr);
    }
#endif

    AE_LOG("[RHI] backend: %s (%s, %s)", g_info.backendName, g_info.device.c_str(),
           g_info.version.c_str());
    return true;
}

const DeviceInfo& info() { return g_info; }

// ---- pipeline state -----------------------------------------------------------

void setState(const RenderState& s) {
    if (s.depthTest) glEnable(GL_DEPTH_TEST);
    else glDisable(GL_DEPTH_TEST);
    glDepthMask(s.depthWrite ? GL_TRUE : GL_FALSE);

    if (s.blend == Blend::Off) {
        glDisable(GL_BLEND);
    } else {
        glEnable(GL_BLEND);
        switch (s.blend) {
        case Blend::Alpha: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
        case Blend::Additive: glBlendFunc(GL_ONE, GL_ONE); break;
        case Blend::Premultiplied: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
        default: break;
        }
    }

    if (s.cullBack) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
}

void setCull(bool cullBack) {
    if (cullBack) glEnable(GL_CULL_FACE);
    else glDisable(GL_CULL_FACE);
}

void setBlend(Blend blend) {
    if (blend == Blend::Off) {
        glDisable(GL_BLEND);
        return;
    }
    glEnable(GL_BLEND);
    switch (blend) {
    case Blend::Alpha: glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); break;
    case Blend::Additive: glBlendFunc(GL_ONE, GL_ONE); break;
    case Blend::Premultiplied: glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); break;
    default: break;
    }
}

void setViewport(int x, int y, int width, int height) { glViewport(x, y, width, height); }

void setScissor(bool enabled, int x, int y, int width, int height) {
    if (!enabled) {
        glDisable(GL_SCISSOR_TEST);
        return;
    }
    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, width, height);
}

// ---- geometry -----------------------------------------------------------------

GeometryHandle createGeometry(const GeometryDesc& d) {
    unsigned slot = allocSlot(geometries());
    GLGeometry& g = geometries()[slot];
    g.alive = true;

    GLenum usage = d.dynamic ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW;
    glCreateBuffers(1, &g.vbo);
    glNamedBufferData(g.vbo, d.vertexBytes, d.vertexData, usage);
    if (d.indexData) {
        glCreateBuffers(1, &g.ibo);
        glNamedBufferData(g.ibo, d.indexCount * sizeof(uint32_t), d.indexData, usage);
    }

    glCreateVertexArrays(1, &g.vao);
    glVertexArrayVertexBuffer(g.vao, 0, g.vbo, 0, d.vertexStride);
    if (g.ibo) glVertexArrayElementBuffer(g.vao, g.ibo);
    for (int i = 0; i < d.attrCount; ++i) {
        const VertexAttr& a = d.attrs[i];
        glEnableVertexArrayAttrib(g.vao, a.location);
        glVertexArrayAttribFormat(g.vao, a.location, a.components, GL_FLOAT, GL_FALSE,
                                  a.offset);
        glVertexArrayAttribBinding(g.vao, a.location, 0);
    }
    return {slot};
}

void destroyGeometry(GeometryHandle& h) {
    if (!h.valid() || h.id >= geometries().size()) return;
    GLGeometry& g = geometries()[h.id];
    if (g.alive) {
        if (g.vao) glDeleteVertexArrays(1, &g.vao);
        if (g.vbo) glDeleteBuffers(1, &g.vbo);
        if (g.ibo) glDeleteBuffers(1, &g.ibo);
        g = {};
    }
    h.id = 0;
}

void draw(GeometryHandle h, unsigned indexCount, int instances) {
    if (!h.valid() || h.id >= geometries().size()) return;
    const GLGeometry& g = geometries()[h.id];
    glBindVertexArray(g.vao);
    if (instances > 1)
        glDrawElementsInstanced(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr, instances);
    else
        glDrawElements(GL_TRIANGLES, indexCount, GL_UNSIGNED_INT, nullptr);
}

// ---- textures -------------------------------------------------------------------

namespace {
struct GLTexture {
    GLuint tex = 0;
    GLenum internalFormat = 0;
    bool alive = false;
};
std::vector<GLTexture>& textures() {
    static std::vector<GLTexture> t(1);
    return t;
}

GLenum glFormat(TexFormat f) {
    switch (f) {
    case TexFormat::RGBA8: return GL_RGBA8;
    case TexFormat::SRGBA8: return GL_SRGB8_ALPHA8;
    case TexFormat::BC1: return GL_COMPRESSED_RGBA_S3TC_DXT1_EXT;
    case TexFormat::BC1_SRGB: return GL_COMPRESSED_SRGB_S3TC_DXT1_EXT;
    case TexFormat::BC3: return GL_COMPRESSED_RGBA_S3TC_DXT5_EXT;
    case TexFormat::BC3_SRGB: return GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT;
    case TexFormat::R8: return GL_R8;
    case TexFormat::RG16F: return GL_RG16F;
    case TexFormat::RGBA16F: return GL_RGBA16F;
    case TexFormat::R11G11B10F: return GL_R11F_G11F_B10F;
    case TexFormat::Depth32F: return GL_DEPTH_COMPONENT32F;
    }
    return GL_RGBA8;
}
} // namespace

TextureHandle createTexture2D(int width, int height, int mipLevels, TexFormat format) {
    unsigned slot = allocSlot(textures());
    GLTexture& t = textures()[slot];
    t.alive = true;
    glCreateTextures(GL_TEXTURE_2D, 1, &t.tex);
    t.internalFormat = glFormat(format);
    glTextureStorage2D(t.tex, mipLevels, t.internalFormat, width, height);
    return {slot};
}

TextureHandle createTextureCube(int size, int mipLevels, TexFormat format) {
    unsigned slot = allocSlot(textures());
    GLTexture& t = textures()[slot];
    t.alive = true;
    glCreateTextures(GL_TEXTURE_CUBE_MAP, 1, &t.tex);
    t.internalFormat = glFormat(format);
    glTextureStorage2D(t.tex, mipLevels, t.internalFormat, size, size);
    return {slot};
}

TextureHandle createTexture2DArray(int width, int height, int layers, TexFormat format) {
    unsigned slot = allocSlot(textures());
    GLTexture& t = textures()[slot];
    t.alive = true;
    glCreateTextures(GL_TEXTURE_2D_ARRAY, 1, &t.tex);
    t.internalFormat = glFormat(format);
    glTextureStorage3D(t.tex, 1, t.internalFormat, width, height, layers);
    return {slot};
}

void uploadTexture2D(TextureHandle h, int level, int width, int height, const void* rgba) {
    if (!h.valid() || h.id >= textures().size()) return;
    glTextureSubImage2D(textures()[h.id].tex, level, 0, 0, width, height, GL_RGBA,
                        GL_UNSIGNED_BYTE, rgba);
}

void uploadTexture2DCompressed(TextureHandle h, int level, int width, int height,
                               size_t byteCount, const void* blocks) {
    if (!h.valid() || h.id >= textures().size()) return;
    const GLTexture& t = textures()[h.id];
    glCompressedTextureSubImage2D(t.tex, level, 0, 0, width, height, t.internalFormat,
                                  (GLsizei)byteCount, blocks);
}

void generateMips(TextureHandle h) {
    if (!h.valid() || h.id >= textures().size()) return;
    glGenerateTextureMipmap(textures()[h.id].tex);
}

void setSampler(TextureHandle h, const SamplerDesc& s) {
    if (!h.valid() || h.id >= textures().size()) return;
    GLuint tex = textures()[h.id].tex;
    glTextureParameteri(tex, GL_TEXTURE_MIN_FILTER,
                        s.mipmaps ? (s.linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST)
                                  : (s.linear ? GL_LINEAR : GL_NEAREST));
    glTextureParameteri(tex, GL_TEXTURE_MAG_FILTER, s.linear ? GL_LINEAR : GL_NEAREST);
    GLenum wrap = s.clampToBorder ? GL_CLAMP_TO_BORDER
                                  : (s.repeat ? GL_REPEAT : GL_CLAMP_TO_EDGE);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_S, wrap);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_T, wrap);
    glTextureParameteri(tex, GL_TEXTURE_WRAP_R, wrap); // cubes; harmless on 2D
    if (s.clampToBorder)
        glTextureParameterfv(tex, GL_TEXTURE_BORDER_COLOR, s.borderColor);
    if (s.shadowCompare) {
        glTextureParameteri(tex, GL_TEXTURE_COMPARE_MODE, GL_COMPARE_REF_TO_TEXTURE);
        glTextureParameteri(tex, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    }
    if (s.anisotropy > 0.0f)
        glTextureParameterf(tex, GL_TEXTURE_MAX_ANISOTROPY, s.anisotropy);
}

void bindTexture(int slot, TextureHandle h) {
    GLuint name = (h.valid() && h.id < textures().size()) ? textures()[h.id].tex : 0;
    glBindTextureUnit(slot, name); // null handle unbinds
}

uintptr_t nativeTextureHandle(TextureHandle h) {
    if (!h.valid() || h.id >= textures().size()) return 0;
    return (uintptr_t)textures()[h.id].tex;
}

void destroyTexture(TextureHandle& h) {
    if (!h.valid() || h.id >= textures().size()) return;
    GLTexture& t = textures()[h.id];
    if (t.alive) {
        if (t.tex) glDeleteTextures(1, &t.tex);
        t = {};
    }
    h.id = 0;
}

// ---- framebuffers ----------------------------------------------------------------

namespace {
struct GLFramebuffer {
    GLuint fbo = 0;
    bool alive = false;
};
std::vector<GLFramebuffer>& framebuffers() {
    static std::vector<GLFramebuffer> t(1);
    return t;
}
GLuint fboName(FramebufferHandle h) {
    return h.valid() && h.id < framebuffers().size() ? framebuffers()[h.id].fbo : 0;
}
GLuint texName(TextureHandle h) {
    return h.valid() && h.id < textures().size() ? textures()[h.id].tex : 0;
}
} // namespace

FramebufferHandle createFramebuffer() {
    unsigned slot = allocSlot(framebuffers());
    GLFramebuffer& f = framebuffers()[slot];
    f.alive = true;
    glCreateFramebuffers(1, &f.fbo);
    return {slot};
}

void attachColor(FramebufferHandle f, int index, TextureHandle t, int mip) {
    glNamedFramebufferTexture(fboName(f), GL_COLOR_ATTACHMENT0 + index, texName(t), mip);
}

void attachColorLayer(FramebufferHandle f, int index, TextureHandle t, int mip, int layer) {
    glNamedFramebufferTextureLayer(fboName(f), GL_COLOR_ATTACHMENT0 + index, texName(t), mip,
                                   layer);
}

void attachDepth(FramebufferHandle f, TextureHandle t) {
    glNamedFramebufferTexture(fboName(f), GL_DEPTH_ATTACHMENT, texName(t), 0);
}

void attachDepthLayer(FramebufferHandle f, TextureHandle t, int mip, int layer) {
    glNamedFramebufferTextureLayer(fboName(f), GL_DEPTH_ATTACHMENT, texName(t), mip, layer);
}

void setDrawBufferCount(FramebufferHandle f, int count) {
    GLuint fbo = fboName(f);
    if (count <= 0) {
        glNamedFramebufferDrawBuffer(fbo, GL_NONE);
        glNamedFramebufferReadBuffer(fbo, GL_NONE);
        return;
    }
    GLenum bufs[8];
    for (int i = 0; i < count && i < 8; ++i) bufs[i] = GL_COLOR_ATTACHMENT0 + i;
    glNamedFramebufferDrawBuffers(fbo, count, bufs);
}

bool framebufferComplete(FramebufferHandle f) {
    return glCheckNamedFramebufferStatus(fboName(f), GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

void bindFramebuffer(FramebufferHandle f) { glBindFramebuffer(GL_FRAMEBUFFER, fboName(f)); }

void destroyFramebuffer(FramebufferHandle& h) {
    if (!h.valid() || h.id >= framebuffers().size()) return;
    GLFramebuffer& f = framebuffers()[h.id];
    if (f.alive) {
        if (f.fbo) glDeleteFramebuffers(1, &f.fbo);
        f = {};
    }
    h.id = 0;
}

void clear(bool color, float r, float g, float b, float a, bool depth) {
    GLbitfield mask = 0;
    if (color) {
        glClearColor(r, g, b, a);
        mask |= GL_COLOR_BUFFER_BIT;
    }
    if (depth) mask |= GL_DEPTH_BUFFER_BIT;
    if (mask) glClear(mask);
}

void blitToBackbuffer(FramebufferHandle src, int srcW, int srcH, int dstW, int dstH) {
    glBlitNamedFramebuffer(fboName(src), 0, 0, 0, srcW, srcH, 0, 0, dstW, dstH,
                           GL_COLOR_BUFFER_BIT, GL_LINEAR);
}

void readBackbuffer(int width, int height, void* rgbaOut) {
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, rgbaOut);
}

// ---- vertex streams ---------------------------------------------------------------

namespace {
struct GLStream {
    GLuint vao = 0, vbo = 0;
    size_t capacity = 0;
    bool alive = false;
};
std::vector<GLStream>& streams() {
    static std::vector<GLStream> t(1);
    return t;
}
} // namespace

StreamHandle createStream(unsigned vertexStride, const StreamAttr* attrs, int attrCount) {
    unsigned slot = allocSlot(streams());
    GLStream& st = streams()[slot];
    st.alive = true;
    glCreateBuffers(1, &st.vbo);
    glCreateVertexArrays(1, &st.vao);
    glVertexArrayVertexBuffer(st.vao, 0, st.vbo, 0, vertexStride);
    for (int i = 0; i < attrCount; ++i) {
        const StreamAttr& a = attrs[i];
        glEnableVertexArrayAttrib(st.vao, a.location);
        glVertexArrayAttribFormat(st.vao, a.location, a.components,
                                  a.unorm8 ? GL_UNSIGNED_BYTE : GL_FLOAT,
                                  a.unorm8 ? GL_TRUE : GL_FALSE, a.offset);
        glVertexArrayAttribBinding(st.vao, a.location, 0);
    }
    return {slot};
}

void setStreamData(StreamHandle h, const void* data, size_t bytes) {
    if (!h.valid() || h.id >= streams().size()) return;
    GLStream& st = streams()[h.id];
    if (bytes > st.capacity) {
        st.capacity = bytes * 2;
        glNamedBufferData(st.vbo, (GLsizeiptr)st.capacity, nullptr, GL_DYNAMIC_DRAW);
    }
    glNamedBufferSubData(st.vbo, 0, (GLsizeiptr)bytes, data);
}

void drawStream(StreamHandle h, Topology topology, int firstVertex, int vertexCount) {
    if (!h.valid() || h.id >= streams().size()) return;
    glBindVertexArray(streams()[h.id].vao);
    glDrawArrays(topology == Topology::Lines ? GL_LINES : GL_TRIANGLES, firstVertex,
                 vertexCount);
}

void destroyStream(StreamHandle& h) {
    if (!h.valid() || h.id >= streams().size()) return;
    GLStream& st = streams()[h.id];
    if (st.alive) {
        if (st.vao) glDeleteVertexArrays(1, &st.vao);
        if (st.vbo) glDeleteBuffers(1, &st.vbo);
        st = {};
    }
    h.id = 0;
}

void drawFullscreen() {
    static GLuint s_vao = 0;
    if (!s_vao) glCreateVertexArrays(1, &s_vao);
    glBindVertexArray(s_vao);
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

// ---- shaders --------------------------------------------------------------------

namespace {
struct GLShader {
    GLuint program = 0;
    bool alive = false;
};
std::vector<GLShader>& shaders() {
    static std::vector<GLShader> t(1);
    return t;
}

GLuint compileStage(GLenum type, const std::string& src, const char* label) {
    GLuint sh = glCreateShader(type);
    const char* csrc = src.c_str();
    glShaderSource(sh, 1, &csrc, nullptr);
    glCompileShader(sh);
    GLint ok = 0;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetShaderInfoLog(sh, sizeof(log), nullptr, log);
        AE_ERROR("[RHI] shader compile error in %s:\n%s", label, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}
} // namespace

ShaderHandle createShader(const std::string& vsSource, const std::string& fsSource,
                          const char* debugLabel) {
    GLuint vs = compileStage(GL_VERTEX_SHADER, vsSource, debugLabel);
    GLuint fs = compileStage(GL_FRAGMENT_SHADER, fsSource, debugLabel);
    if (!vs || !fs) {
        if (vs) glDeleteShader(vs);
        if (fs) glDeleteShader(fs);
        return {};
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[4096];
        glGetProgramInfoLog(program, sizeof(log), nullptr, log);
        AE_ERROR("[RHI] shader link error (%s):\n%s", debugLabel, log);
        glDeleteProgram(program);
        return {};
    }
    unsigned slot = allocSlot(shaders());
    GLShader& sh = shaders()[slot];
    sh.alive = true;
    sh.program = program;
    return {slot};
}

void useShader(ShaderHandle h) {
    glUseProgram(h.valid() && h.id < shaders().size() ? shaders()[h.id].program : 0);
}

void destroyShader(ShaderHandle& h) {
    if (!h.valid() || h.id >= shaders().size()) return;
    GLShader& sh = shaders()[h.id];
    if (sh.alive) {
        if (sh.program) glDeleteProgram(sh.program);
        sh = {};
    }
    h.id = 0;
}

int uniformLocation(ShaderHandle h, const char* name) {
    if (!h.valid() || h.id >= shaders().size()) return -1;
    return glGetUniformLocation(shaders()[h.id].program, name);
}

void setUniform1i(int loc, int v) { glUniform1i(loc, v); }
void setUniform1f(int loc, float v) { glUniform1f(loc, v); }
void setUniform2f(int loc, float x, float y) { glUniform2f(loc, x, y); }
void setUniform3f(int loc, float x, float y, float z) { glUniform3f(loc, x, y, z); }
void setUniform4f(int loc, float x, float y, float z, float w) {
    glUniform4f(loc, x, y, z, w);
}
void setUniformMat4(int loc, const float* m16, int count) {
    glUniformMatrix4fv(loc, count, GL_FALSE, m16);
}
void setUniform1fv(int loc, const float* v, int count) { glUniform1fv(loc, count, v); }
void setUniform3fv(int loc, const float* v, int count) { glUniform3fv(loc, count, v); }

// ---- GPU timers ----------------------------------------------------------------

namespace {
struct GLTimer {
    GLuint query = 0;
    bool alive = false;
};
std::vector<GLTimer>& timers() {
    static std::vector<GLTimer> t(1);
    return t;
}
} // namespace

TimerHandle createTimer() {
    unsigned slot = allocSlot(timers());
    GLTimer& t = timers()[slot];
    t.alive = true;
    glCreateQueries(GL_TIME_ELAPSED, 1, &t.query);
    return {slot};
}

void destroyTimer(TimerHandle& h) {
    if (!h.valid() || h.id >= timers().size()) return;
    GLTimer& t = timers()[h.id];
    if (t.alive) {
        if (t.query) glDeleteQueries(1, &t.query);
        t = {};
    }
    h.id = 0;
}

void beginTimer(TimerHandle h) {
    if (!h.valid() || h.id >= timers().size()) return;
    glBeginQuery(GL_TIME_ELAPSED, timers()[h.id].query);
}

void endTimer() { glEndQuery(GL_TIME_ELAPSED); }

unsigned long long timerNs(TimerHandle h) {
    if (!h.valid() || h.id >= timers().size()) return 0;
    GLuint64 ns = 0;
    glGetQueryObjectui64v(timers()[h.id].query, GL_QUERY_RESULT, &ns);
    return ns;
}

// ---- storage buffers ------------------------------------------------------------

BufferHandle createStorageBuffer() {
    unsigned slot = allocSlot(buffers());
    GLBuffer& b = buffers()[slot];
    b.alive = true;
    glCreateBuffers(1, &b.buf);
    return {slot};
}

void setBufferData(BufferHandle h, const void* data, size_t bytes) {
    if (!h.valid() || h.id >= buffers().size()) return;
    glNamedBufferData(buffers()[h.id].buf, bytes, data, GL_DYNAMIC_DRAW);
}

void bindStorageBuffer(int slot, BufferHandle h) {
    if (!h.valid() || h.id >= buffers().size()) return;
    glBindBufferBase(GL_SHADER_STORAGE_BUFFER, slot, buffers()[h.id].buf);
}

void destroyBuffer(BufferHandle& h) {
    if (!h.valid() || h.id >= buffers().size()) return;
    GLBuffer& b = buffers()[h.id];
    if (b.alive) {
        if (b.buf) glDeleteBuffers(1, &b.buf);
        b = {};
    }
    h.id = 0;
}

BufferHandle createUniformBuffer(size_t bytes) {
    unsigned slot = allocSlot(buffers());
    GLBuffer& b = buffers()[slot];
    b.alive = true;
    glCreateBuffers(1, &b.buf);
    glNamedBufferData(b.buf, (GLsizeiptr)bytes, nullptr, GL_DYNAMIC_DRAW);
    return {slot};
}

void updateUniformBuffer(BufferHandle h, const void* data, size_t bytes) {
    if (!h.valid() || h.id >= buffers().size()) return;
    glNamedBufferSubData(buffers()[h.id].buf, 0, (GLsizeiptr)bytes, data);
}

void bindUniformBuffer(int slot, BufferHandle h) {
    if (!h.valid() || h.id >= buffers().size()) return;
    glBindBufferBase(GL_UNIFORM_BUFFER, slot, buffers()[h.id].buf);
}

} // namespace rhi
} // namespace ae
