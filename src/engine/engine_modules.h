// Aether Engine — optional engine modules (the modularity pillar).
//
// Engine features are grouped into named modules (physics, audio, narrative,
// scripting, ui, particles, animation, ai). A project that doesn't want one
// disables it in project.aeproj:
//
//   "modules": { "narrative": false, "physics": false }
//
// A disabled module's component types are never registered — scenes skip them
// with a warning, the Add Component menu doesn't offer them, and its systems
// don't run (physics doesn't step, audio doesn't initialize, navmesh won't
// bake). Everything defaults to enabled, so existing projects are unaffected.
// The editor's Modules & Plugins panel toggles them live.
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace ae {

class ComponentRegistry;

struct EngineModuleDesc {
    std::string id;          // manifest key ("physics")
    std::string name;        // display name ("Physics (Jolt)")
    std::string description; // one-liner for the editor panel
    int moduleId = 0;        // ComponentRegistry group (kEngineModuleBase + n)
    bool enabled = true;
    // Registers this module's component types (called when enabled).
    std::function<void(ComponentRegistry&, int moduleId)> registerComponents;
};

// moduleIds: 0 = always-on core, 1 = project GameModule, 100+ = engine
// modules, 1000+ = project plugins.
constexpr int kEngineModuleBase = 100;
constexpr int kPluginModuleBase = 1000;

class EngineModules {
public:
    // Called once by registerBuiltinComponents to declare the built-in set.
    EngineModuleDesc& declare(EngineModuleDesc d);

    // Applies a project's "modules" flags. Call any time before
    // registerBuiltinComponents; flags for modules declared later are held
    // and applied at declare() time, so host startup order doesn't matter.
    void configure(const std::vector<std::pair<std::string, bool>>& flags);

    bool enabled(const std::string& id) const;
    // Live toggle: registers / unregisters the module's components in place.
    // (Components already in the scene keep running until the scene reloads —
    // serialization simply skips their types once unregistered.)
    void setEnabled(const std::string& id, bool on, ComponentRegistry& registry);

    const std::vector<EngineModuleDesc>& all() const { return mods_; }
    EngineModuleDesc* find(const std::string& id);

private:
    std::vector<EngineModuleDesc> mods_;
    std::vector<std::pair<std::string, bool>> pending_; // flags seen pre-declare
};

// The single engine-wide instance (lives in AetherCore.dll).
EngineModules& engineModules();

} // namespace ae
