#include "plugin_manager.h"
#include "component_registry.h"
#include "engine_modules.h" // kPluginModuleBase
#include "project.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

typedef int (*AbiVersionFn)();
typedef void (*RegisterFn)(ComponentRegistry&);
typedef void (*RegisterAsFn)(ComponentRegistry&, int);

bool PluginInfo::hasBinary() const {
    return pathExists(joinPath(dir, "Binaries\\" + name + ".dll"));
}
bool PluginInfo::hasSource() const {
    return isDirectory(joinPath(dir, sourceDir));
}

void PluginManager::scan(const Project& project) {
    std::string pluginsRoot = joinPath(project.root, "Plugins");
    std::vector<PluginInfo> found;

    std::error_code ec;
    if (isDirectory(pluginsRoot)) {
        for (fs::directory_iterator it(pluginsRoot, ec), end; it != end && !ec;
             it.increment(ec)) {
            if (!it->is_directory()) continue;
            std::string dir = it->path().string();
            std::string manifest = joinPath(dir, "plugin.json");
            if (!pathExists(manifest)) continue;

            PluginInfo p;
            p.name = it->path().filename().string();
            p.dir = dir;
            p.sourceDir = "Source";

            std::ifstream f(manifest, std::ios::binary);
            std::ostringstream ss;
            ss << f.rdbuf();
            std::string text = ss.str();
            JsonValue doc;
            if (jsonParse(text.c_str(), text.size(), doc)) {
                if (const std::string* d = doc.string("description")) p.description = *d;
                if (const std::string* s = doc.string("sourceDir")) p.sourceDir = *s;
                p.enabled = doc.flag("enabled", true);
            }
            found.push_back(std::move(p));
        }
    }

    // Preserve live state (handle/moduleId/loaded) across rescans.
    for (PluginInfo& p : found) {
        for (PluginInfo& old : plugins_) {
            if (old.name != p.name) continue;
            p.loaded = old.loaded;
            p.handle = old.handle;
            p.moduleId = old.moduleId;
        }
    }
    // A vanished-but-loaded plugin keeps its slot until unloadAll (its code is
    // still mapped; dropping the entry would orphan the DLL).
    for (PluginInfo& old : plugins_)
        if (old.loaded) {
            bool still = false;
            for (const PluginInfo& p : found) still |= p.name == old.name;
            if (!still) found.push_back(old);
        }
    plugins_ = std::move(found);
}

bool PluginManager::load(PluginInfo& p, bool hotCopy) {
    if (p.loaded) return true;
    std::string dllPath = joinPath(p.dir, "Binaries\\" + p.name + ".dll");
    if (!pathExists(dllPath)) {
        AE_LOG("[Plugin] %s has no compiled binary yet (%s)", p.name.c_str(), dllPath.c_str());
        return false;
    }

    std::string loadPath = dllPath;
    if (hotCopy) {
        std::string hotDir = joinPath(p.dir, "Intermediate\\Hot");
        CreateDirectoryA(joinPath(p.dir, "Intermediate").c_str(), nullptr);
        CreateDirectoryA(hotDir.c_str(), nullptr);
        char hotPath[MAX_PATH];
        std::snprintf(hotPath, sizeof(hotPath), "%s\\%s_%08lx.dll", hotDir.c_str(),
                      p.name.c_str(), (unsigned long)GetTickCount());
        if (!CopyFileA(dllPath.c_str(), hotPath, FALSE)) {
            AE_ERROR("[Plugin] cannot hot-copy %s", dllPath.c_str());
            return false;
        }
        loadPath = hotPath;
    }

    HMODULE mod = LoadLibraryA(loadPath.c_str());
    if (!mod) {
        AE_ERROR("[Plugin] LoadLibrary failed for %s (error %lu)", loadPath.c_str(),
                 GetLastError());
        return false;
    }
    auto abi = (AbiVersionFn)GetProcAddress(mod, "GameModule_AbiVersion");
    auto regAs = (RegisterAsFn)GetProcAddress(mod, "GameModule_RegisterAs");
    auto reg = (RegisterFn)GetProcAddress(mod, "GameModule_Register");
    if (!abi || (!regAs && !reg)) {
        AE_ERROR("[Plugin] %s lacks the module exports (AbiVersion + RegisterAs/Register)",
                 dllPath.c_str());
        FreeLibrary(mod);
        return false;
    }
    if (abi() != AE_ABI_VERSION) {
        AE_ERROR("[Plugin] %s ABI v%d != engine v%d — recompile the plugin", p.name.c_str(),
                 abi(), AE_ABI_VERSION);
        FreeLibrary(mod);
        return false;
    }

    if (!p.moduleId) p.moduleId = kPluginModuleBase + nextSlot_++;
    if (regAs) regAs(componentRegistry(), p.moduleId);
    else reg(componentRegistry()); // legacy export: lands in the game-module group

    p.handle = mod;
    p.loaded = true;
    AE_LOG("[Plugin] loaded %s (module id %d)", p.name.c_str(), p.moduleId);
    return true;
}

void PluginManager::unload(PluginInfo& p) {
    if (!p.loaded) return;
    componentRegistry().unregisterModule(p.moduleId);
    FreeLibrary((HMODULE)p.handle);
    p.handle = nullptr;
    p.loaded = false;
    AE_LOG("[Plugin] unloaded %s", p.name.c_str());
}

void PluginManager::loadEnabled(const Project& project, bool hotCopy) {
    scan(project);
    for (PluginInfo& p : plugins_)
        if (p.enabled && !p.loaded) load(p, hotCopy);
}

void PluginManager::unloadAll() {
    for (PluginInfo& p : plugins_) unload(p);
}

bool PluginManager::saveManifest(const PluginInfo& p) {
    std::ostringstream o;
    std::string desc = p.description;
    for (size_t i = 0; (i = desc.find('"', i)) != std::string::npos; i += 2)
        desc.insert(i, "\\");
    o << "{\n  \"description\": \"" << desc << "\",\n  \"enabled\": "
      << (p.enabled ? "true" : "false") << ",\n  \"sourceDir\": \"" << p.sourceDir
      << "\"\n}\n";
    std::ofstream f(joinPath(p.dir, "plugin.json"), std::ios::binary);
    if (!f) return false;
    f << o.str();
    return true;
}

} // namespace ae
