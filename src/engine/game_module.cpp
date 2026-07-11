#include "game_module.h"
#include "component_registry.h"
#include "project.h"
#include "../core/log.h"
#include "../core/paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>

namespace ae {

typedef int (*AbiVersionFn)();
typedef void (*RegisterFn)(ComponentRegistry&);

GameModule::~GameModule() = default;

bool GameModule::load(const Project& project, bool hotCopy) {
    if (loaded()) unload();
    if (!project.hasModule()) return false;

    std::string dllPath = joinPath(project.root, "Binaries\\" + project.moduleName + ".dll");
    if (!pathExists(dllPath)) {
        AE_LOG("[Module] %s not compiled yet (Tools > Compile Scripts)", dllPath.c_str());
        return false;
    }

    std::string loadPath = dllPath;
    if (hotCopy) {
        // Copy-before-load: the original stays writable for the next rebuild.
        std::string hotDir = joinPath(project.root, "Intermediate\\Hot");
        CreateDirectoryA(joinPath(project.root, "Intermediate").c_str(), nullptr);
        CreateDirectoryA(hotDir.c_str(), nullptr);
        char hotPath[MAX_PATH];
        std::snprintf(hotPath, sizeof(hotPath), "%s\\%s_%08lx.dll", hotDir.c_str(),
                      project.moduleName.c_str(), (unsigned long)GetTickCount());
        if (!CopyFileA(dllPath.c_str(), hotPath, FALSE)) {
            AE_ERROR("[Module] cannot hot-copy %s", dllPath.c_str());
            return false;
        }
        loadPath = hotPath;
    }

    HMODULE mod = LoadLibraryA(loadPath.c_str());
    if (!mod) {
        AE_ERROR("[Module] LoadLibrary failed for %s (error %lu)", loadPath.c_str(),
                 GetLastError());
        return false;
    }

    auto abi = (AbiVersionFn)GetProcAddress(mod, "GameModule_AbiVersion");
    auto reg = (RegisterFn)GetProcAddress(mod, "GameModule_Register");
    if (!abi || !reg) {
        AE_ERROR("[Module] %s does not export GameModule_AbiVersion/GameModule_Register",
                 dllPath.c_str());
        FreeLibrary(mod);
        return false;
    }
    if (abi() != AE_ABI_VERSION) {
        AE_ERROR("[Module] ABI mismatch: module built against v%d, engine is v%d — recompile "
                 "scripts",
                 abi(), AE_ABI_VERSION);
        FreeLibrary(mod);
        return false;
    }

    reg(componentRegistry());
    handle_ = mod;
    AE_LOG("[Module] loaded %s", dllPath.c_str());
    return true;
}

void GameModule::unload() {
    if (!handle_) return;
    componentRegistry().unregisterModule(kGameModuleId);
    FreeLibrary((HMODULE)handle_);
    handle_ = nullptr;
    AE_LOG("[Module] unloaded game module");
}

} // namespace ae
