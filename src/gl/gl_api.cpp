#include "gl_api.h"
#include <cstdio>

#define AE_DEFINE_GL(ret, name, args) PFN_##name name = nullptr;
AE_GL_FUNCS(AE_DEFINE_GL)
#undef AE_DEFINE_GL

PFNWGLSWAPINTERVALEXT wglSwapIntervalEXT = nullptr;
PFNWGLCREATECONTEXTATTRIBSARB wglCreateContextAttribsARB = nullptr;

namespace ae {

static void* getProc(const char* name) {
    void* p = (void*)wglGetProcAddress(name);
    // wglGetProcAddress returns sentinel values for failure on some drivers.
    if (p == nullptr || p == (void*)1 || p == (void*)2 || p == (void*)3 || p == (void*)-1) {
        static HMODULE gl32 = LoadLibraryA("opengl32.dll");
        p = gl32 ? (void*)GetProcAddress(gl32, name) : nullptr;
    }
    return p;
}

bool loadGLFunctions() {
    bool ok = true;
#define AE_LOAD_GL(ret, name, args) \
    name = (PFN_##name)getProc(#name); \
    if (!name) { std::fprintf(stderr, "[GL] missing function: %s\n", #name); ok = false; }
    AE_GL_FUNCS(AE_LOAD_GL)
#undef AE_LOAD_GL
    wglSwapIntervalEXT = (PFNWGLSWAPINTERVALEXT)getProc("wglSwapIntervalEXT");
    return ok;
}

} // namespace ae
