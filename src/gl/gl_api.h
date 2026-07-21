// Aether Engine — minimal OpenGL 4.5 core loader (Direct State Access subset).
// Zero dependencies: base GL 1.1 comes from opengl32.lib, everything else is
// resolved at runtime through wglGetProcAddress.
#pragma once
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "../core/ae_api.h"
#include <windows.h>
#include <GL/gl.h>
#include <cstddef>
#include <cstdint>

typedef char GLchar;
typedef ptrdiff_t GLsizeiptr;
typedef ptrdiff_t GLintptr;
typedef uint64_t GLuint64;

// ---- enums beyond GL 1.1 -------------------------------------------------
#define GL_ARRAY_BUFFER                   0x8892
#define GL_ELEMENT_ARRAY_BUFFER           0x8893
#define GL_STATIC_DRAW                    0x88E4
#define GL_DYNAMIC_DRAW                   0x88E8
#define GL_TIME_ELAPSED                   0x88BF
#define GL_SHADER_STORAGE_BUFFER          0x90D2
#define GL_R11F_G11F_B10F                 0x8C3A
#define GL_COMPRESSED_RGBA_S3TC_DXT1_EXT  0x83F1
#define GL_COMPRESSED_RGBA_S3TC_DXT5_EXT  0x83F3
#define GL_COMPRESSED_SRGB_S3TC_DXT1_EXT  0x8C4C
#define GL_COMPRESSED_SRGB_ALPHA_S3TC_DXT5_EXT 0x8C4F
#define GL_COMPRESSED_RG_RGTC2            0x8DBD // BC5: two-channel normals
#define GL_QUERY_RESULT                   0x8866
#define GL_QUERY_RESULT_AVAILABLE         0x8867
#define GL_LINES                          0x0001

#define GL_FRAGMENT_SHADER                0x8B30
#define GL_VERTEX_SHADER                  0x8B31
#define GL_COMPILE_STATUS                 0x8B81
#define GL_LINK_STATUS                    0x8B82
#define GL_INFO_LOG_LENGTH                0x8B84
#define GL_TEXTURE0                       0x84C0
#define GL_TEXTURE_CUBE_MAP               0x8513
#define GL_TEXTURE_2D_ARRAY               0x8C1A
#define GL_TEXTURE_WRAP_R                 0x8072
#define GL_CLAMP_TO_EDGE                  0x812F
#define GL_CLAMP_TO_BORDER                0x812D
#define GL_TEXTURE_BORDER_COLOR           0x1004
#define GL_TEXTURE_BASE_LEVEL             0x813C
#define GL_TEXTURE_MAX_LEVEL              0x813D
#define GL_TEXTURE_COMPARE_MODE           0x884C
#define GL_TEXTURE_COMPARE_FUNC           0x884D
#define GL_COMPARE_REF_TO_TEXTURE         0x884E
#define GL_TEXTURE_MAX_ANISOTROPY         0x84FE
#define GL_MAX_TEXTURE_MAX_ANISOTROPY     0x84FF
#define GL_TEXTURE_CUBE_MAP_SEAMLESS      0x884F
#define GL_R8                             0x8229
#define GL_RG8                            0x822B
#define GL_RG                             0x8227
#define GL_R16F                           0x822D
#define GL_RG16F                          0x822F
#define GL_RGB16F                         0x881B
#define GL_RGBA16F                        0x881A
#define GL_SRGB8_ALPHA8                   0x8C43
#define GL_DEPTH_COMPONENT32F             0x8CAC
#define GL_HALF_FLOAT                     0x140B
#define GL_FRAMEBUFFER                    0x8D40
#define GL_READ_FRAMEBUFFER               0x8CA8
#define GL_DRAW_FRAMEBUFFER               0x8CA9
#define GL_COLOR_ATTACHMENT0              0x8CE0
#define GL_COLOR_ATTACHMENT1              0x8CE1
#define GL_COLOR_ATTACHMENT2              0x8CE2
#define GL_DEPTH_ATTACHMENT               0x8D00
#define GL_FRAMEBUFFER_COMPLETE           0x8CD5
#define GL_FRAMEBUFFER_SRGB               0x8DB9
#define GL_UNIFORM_BUFFER                 0x8A11
#define GL_UNIFORM_TYPE                   0x8A37
#define GL_UNIFORM_OFFSET                 0x8A3B
#define GL_UNIFORM_ARRAY_STRIDE           0x8A3C
#define GL_UNIFORM_MATRIX_STRIDE          0x8A3D
#define GL_UNIFORM_BLOCK_DATA_SIZE        0x8A40
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORMS  0x8A42
#define GL_UNIFORM_BLOCK_ACTIVE_UNIFORM_INDICES 0x8A43
#define GL_INVALID_INDEX                  0xFFFFFFFFu
#define GL_DEBUG_OUTPUT                   0x92E0
#define GL_DEBUG_OUTPUT_SYNCHRONOUS       0x8242
#define GL_DEBUG_SEVERITY_HIGH            0x9146
#define GL_DEBUG_SEVERITY_MEDIUM          0x9147
#define GL_DEBUG_SEVERITY_NOTIFICATION    0x826B
#define GL_MULTISAMPLE                    0x809D

// ---- WGL context-creation constants --------------------------------------
#define WGL_CONTEXT_MAJOR_VERSION_ARB     0x2091
#define WGL_CONTEXT_MINOR_VERSION_ARB     0x2092
#define WGL_CONTEXT_FLAGS_ARB             0x2094
#define WGL_CONTEXT_PROFILE_MASK_ARB      0x9126
#define WGL_CONTEXT_CORE_PROFILE_BIT_ARB  0x00000001
#define WGL_CONTEXT_DEBUG_BIT_ARB         0x00000001

typedef void (APIENTRY* GLDEBUGPROC)(GLenum source, GLenum type, GLuint id, GLenum severity,
                                     GLsizei length, const GLchar* message, const void* userParam);

// ---- function pointers ----------------------------------------------------
#define AE_GL_FUNCS(X) \
    X(GLuint, glCreateShader, (GLenum type)) \
    X(void, glShaderSource, (GLuint shader, GLsizei count, const GLchar* const* string, const GLint* length)) \
    X(void, glCompileShader, (GLuint shader)) \
    X(void, glGetShaderiv, (GLuint shader, GLenum pname, GLint* params)) \
    X(void, glGetShaderInfoLog, (GLuint shader, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteShader, (GLuint shader)) \
    X(GLuint, glCreateProgram, (void)) \
    X(void, glAttachShader, (GLuint program, GLuint shader)) \
    X(void, glLinkProgram, (GLuint program)) \
    X(void, glGetProgramiv, (GLuint program, GLenum pname, GLint* params)) \
    X(void, glGetProgramInfoLog, (GLuint program, GLsizei bufSize, GLsizei* length, GLchar* infoLog)) \
    X(void, glDeleteProgram, (GLuint program)) \
    X(void, glUseProgram, (GLuint program)) \
    X(GLint, glGetUniformLocation, (GLuint program, const GLchar* name)) \
    X(void, glUniform1i, (GLint location, GLint v0)) \
    X(void, glUniform1f, (GLint location, GLfloat v0)) \
    X(void, glUniform2f, (GLint location, GLfloat v0, GLfloat v1)) \
    X(void, glUniform3f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2)) \
    X(void, glUniform4f, (GLint location, GLfloat v0, GLfloat v1, GLfloat v2, GLfloat v3)) \
    X(void, glUniform1fv, (GLint location, GLsizei count, const GLfloat* value)) \
    X(void, glUniform3fv, (GLint location, GLsizei count, const GLfloat* value)) \
    X(void, glUniform4fv, (GLint location, GLsizei count, const GLfloat* value)) \
    X(void, glUniformMatrix4fv, (GLint location, GLsizei count, GLboolean transpose, const GLfloat* value)) \
    X(void, glCreateBuffers, (GLsizei n, GLuint* buffers)) \
    X(void, glDeleteBuffers, (GLsizei n, const GLuint* buffers)) \
    X(void, glNamedBufferData, (GLuint buffer, GLsizeiptr size, const void* data, GLenum usage)) \
    X(void, glCreateVertexArrays, (GLsizei n, GLuint* arrays)) \
    X(void, glDeleteVertexArrays, (GLsizei n, const GLuint* arrays)) \
    X(void, glBindVertexArray, (GLuint array)) \
    X(void, glVertexArrayVertexBuffer, (GLuint vaobj, GLuint bindingindex, GLuint buffer, GLintptr offset, GLsizei stride)) \
    X(void, glVertexArrayElementBuffer, (GLuint vaobj, GLuint buffer)) \
    X(void, glEnableVertexArrayAttrib, (GLuint vaobj, GLuint index)) \
    X(void, glVertexArrayAttribFormat, (GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLboolean normalized, GLuint relativeoffset)) \
    X(void, glVertexArrayAttribIFormat, (GLuint vaobj, GLuint attribindex, GLint size, GLenum type, GLuint relativeoffset)) \
    X(void, glNamedBufferSubData, (GLuint buffer, GLintptr offset, GLsizeiptr size, const void* data)) \
    X(void, glBindBufferBase, (GLenum target, GLuint index, GLuint buffer)) \
    X(GLuint, glGetUniformBlockIndex, (GLuint program, const GLchar* uniformBlockName)) \
    X(void, glGetActiveUniformBlockiv, (GLuint program, GLuint uniformBlockIndex, GLenum pname, GLint* params)) \
    X(void, glGetActiveUniformsiv, (GLuint program, GLsizei uniformCount, const GLuint* uniformIndices, GLenum pname, GLint* params)) \
    X(void, glGetActiveUniformName, (GLuint program, GLuint uniformIndex, GLsizei bufSize, GLsizei* length, GLchar* uniformName)) \
    X(void, glUniformBlockBinding, (GLuint program, GLuint uniformBlockIndex, GLuint uniformBlockBinding)) \
    X(void, glVertexArrayAttribBinding, (GLuint vaobj, GLuint attribindex, GLuint bindingindex)) \
    X(void, glCreateTextures, (GLenum target, GLsizei n, GLuint* textures)) \
    X(void, glTextureStorage2D, (GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height)) \
    X(void, glTextureStorage3D, (GLuint texture, GLsizei levels, GLenum internalformat, GLsizei width, GLsizei height, GLsizei depth)) \
    X(void, glTextureSubImage2D, (GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLenum type, const void* pixels)) \
    X(void, glTextureParameteri, (GLuint texture, GLenum pname, GLint param)) \
    X(void, glTextureParameterf, (GLuint texture, GLenum pname, GLfloat param)) \
    X(void, glTextureParameterfv, (GLuint texture, GLenum pname, const GLfloat* param)) \
    X(void, glGenerateTextureMipmap, (GLuint texture)) \
    X(void, glBindTextureUnit, (GLuint unit, GLuint texture)) \
    X(void, glCreateFramebuffers, (GLsizei n, GLuint* framebuffers)) \
    X(void, glDeleteFramebuffers, (GLsizei n, const GLuint* framebuffers)) \
    X(void, glBindFramebuffer, (GLenum target, GLuint framebuffer)) \
    X(void, glNamedFramebufferTexture, (GLuint framebuffer, GLenum attachment, GLuint texture, GLint level)) \
    X(void, glNamedFramebufferTextureLayer, (GLuint framebuffer, GLenum attachment, GLuint texture, GLint level, GLint layer)) \
    X(void, glNamedFramebufferDrawBuffers, (GLuint framebuffer, GLsizei n, const GLenum* bufs)) \
    X(void, glNamedFramebufferDrawBuffer, (GLuint framebuffer, GLenum buf)) \
    X(void, glNamedFramebufferReadBuffer, (GLuint framebuffer, GLenum src)) \
    X(GLenum, glCheckNamedFramebufferStatus, (GLuint framebuffer, GLenum target)) \
    X(void, glBlitNamedFramebuffer, (GLuint readFramebuffer, GLuint drawFramebuffer, GLint srcX0, GLint srcY0, GLint srcX1, GLint srcY1, GLint dstX0, GLint dstY0, GLint dstX1, GLint dstY1, GLbitfield mask, GLenum filter)) \
    X(void, glDebugMessageCallback, (GLDEBUGPROC callback, const void* userParam))     X(void, glCreateQueries, (GLenum target, GLsizei n, GLuint* ids))     X(void, glDeleteQueries, (GLsizei n, const GLuint* ids))     X(void, glBeginQuery, (GLenum target, GLuint id))     X(void, glEndQuery, (GLenum target))     X(void, glGetQueryObjectiv, (GLuint id, GLenum pname, GLint* params))     X(void, glGetQueryObjectui64v, (GLuint id, GLenum pname, GLuint64* params))     X(void, glCompressedTextureSubImage2D, (GLuint texture, GLint level, GLint xoffset, GLint yoffset, GLsizei width, GLsizei height, GLenum format, GLsizei imageSize, const void* data))     X(void, glDrawElementsInstanced, (GLenum mode, GLsizei count, GLenum type, const void* indices, GLsizei instancecount))

#define AE_DECLARE_GL(ret, name, args) typedef ret (APIENTRY* PFN_##name) args; extern AE_API PFN_##name name;
AE_GL_FUNCS(AE_DECLARE_GL)
#undef AE_DECLARE_GL

typedef BOOL (APIENTRY* PFNWGLSWAPINTERVALEXT)(int interval);
typedef HGLRC (APIENTRY* PFNWGLCREATECONTEXTATTRIBSARB)(HDC hDC, HGLRC hShareContext, const int* attribList);
extern AE_API PFNWGLSWAPINTERVALEXT wglSwapIntervalEXT;
extern AE_API PFNWGLCREATECONTEXTATTRIBSARB wglCreateContextAttribsARB;

namespace ae {
// Must be called with a current GL context. Returns false if any core function is missing.
bool loadGLFunctions(); // WGL loader (Win32 editor window)

// Backend-neutral loader: resolves every GL entry point through the supplied
// getProcAddress (e.g. SDL_GL_GetProcAddress). The portable path.
typedef void* (*GLLoaderProc)(const char*);
bool loadGLFunctionsWith(GLLoaderProc proc);
}
