#include "paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

namespace ae {

bool pathExists(const std::string& path) {
    return GetFileAttributesA(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool isDirectory(const std::string& path) {
    DWORD attr = GetFileAttributesA(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY);
}

std::string parentPath(const std::string& path) {
    size_t s = path.find_last_of("\\/");
    return s == std::string::npos ? std::string() : path.substr(0, s);
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    char last = a.back();
    return (last == '\\' || last == '/') ? a + b : a + "\\" + b;
}

std::string engineBinDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    HMODULE mod = nullptr;
    // Resolve the module that contains this function — AetherCore.dll — not
    // the exe, so every host (editor, runtime, hub, game module) agrees.
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)&engineBinDir, &mod);
    char path[MAX_PATH];
    GetModuleFileNameA(mod, path, MAX_PATH);
    std::string dir = parentPath(path);
    cached = dir.empty() ? "." : dir;
    return cached;
}

std::string engineRoot() {
    static std::string cached;
    if (!cached.empty()) return cached;
    std::string probe = engineBinDir();
    for (int i = 0; i < 4 && !probe.empty(); ++i) {
        if (pathExists(joinPath(probe, "engine.json")) || isDirectory(joinPath(probe, "shaders"))) {
            cached = probe;
            return cached;
        }
        probe = parentPath(probe);
    }
    cached = engineBinDir();
    return cached;
}

std::string engineShaderDir() {
    return joinPath(engineRoot(), "shaders") + "\\";
}

std::string engineTemplatesDir() {
    static std::string cached;
    static bool resolved = false;
    if (resolved) return cached;
    resolved = true;
    std::string probe = engineBinDir();
    for (int i = 0; i < 5 && !probe.empty(); ++i) {
        std::string candidate = joinPath(probe, "Templates");
        if (isDirectory(candidate)) {
            cached = candidate;
            return cached;
        }
        probe = parentPath(probe);
    }
    return cached;
}

} // namespace ae
