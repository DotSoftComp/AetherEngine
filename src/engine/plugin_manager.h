// Aether Engine — project plugins: drop-in native modules, N per project.
//
// A plugin is a folder under <project>/Plugins/:
//
//   Plugins/OrbitFx/plugin.json           { "description": "...", "enabled": true }
//   Plugins/OrbitFx/Source/*.cpp          (optional — compiled like GameModule)
//   Plugins/OrbitFx/Binaries/OrbitFx.dll  (what actually loads)
//
// Each DLL uses the same SDK as GameModule (AE_COMPONENT/AE_REGISTER_COMPONENT
// + a module_entry.cpp) and exports GameModule_AbiVersion plus either
// GameModule_RegisterAs(registry, moduleId) — preferred, lets every plugin get
// its own registry group — or the legacy GameModule_Register. Plugins load
// after the game module at startup, can be unloaded/reloaded live from the
// editor's Modules & Plugins panel, and unregister their component types
// wholesale on unload (clear the world of their components first — the editor
// reloads the scene around a reload for exactly that reason).
#pragma once
#include <string>
#include <vector>

namespace ae {

struct Project;

struct PluginInfo {
    std::string name;        // folder name == DLL base name
    std::string dir;         // absolute Plugins/<Name>
    std::string description; // from plugin.json
    std::string sourceDir;   // plugin-relative sources (default "Source")
    bool enabled = true;     // from plugin.json; disabled plugins never load
    bool loaded = false;
    int moduleId = 0;        // kPluginModuleBase + slot
    void* handle = nullptr;  // HMODULE
    bool hasBinary() const;
    bool hasSource() const;
};

class PluginManager {
public:
    // Re-reads <project>/Plugins/*/plugin.json. Keeps loaded state for plugins
    // that still exist; newly appeared folders join the list unloaded.
    void scan(const Project& project);

    bool load(PluginInfo& p, bool hotCopy);
    void unload(PluginInfo& p);
    // scan() + load every enabled plugin with a binary (host startup).
    void loadEnabled(const Project& project, bool hotCopy);
    void unloadAll();

    // Persists a plugin's enabled flag + description back to its plugin.json.
    static bool saveManifest(const PluginInfo& p);

    std::vector<PluginInfo>& plugins() { return plugins_; }

private:
    std::vector<PluginInfo> plugins_;
    int nextSlot_ = 0;
};

} // namespace ae
