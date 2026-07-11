#include "engine_modules.h"
#include "component_registry.h"
#include "../core/log.h"

namespace ae {

EngineModules& engineModules() {
    static EngineModules s;
    return s;
}

EngineModuleDesc& EngineModules::declare(EngineModuleDesc d) {
    if (EngineModuleDesc* existing = find(d.id)) return *existing; // idempotent
    d.moduleId = kEngineModuleBase + (int)mods_.size();
    for (const auto& kv : pending_)
        if (kv.first == d.id) {
            d.enabled = kv.second;
            if (!d.enabled)
                AE_LOG("[Modules] engine module disabled by project: %s", d.id.c_str());
        }
    mods_.push_back(std::move(d));
    return mods_.back();
}

void EngineModules::configure(const std::vector<std::pair<std::string, bool>>& flags) {
    for (const auto& kv : flags) {
        if (EngineModuleDesc* m = find(kv.first)) {
            m->enabled = kv.second;
            if (!kv.second)
                AE_LOG("[Modules] engine module disabled by project: %s", m->id.c_str());
        } else {
            pending_.push_back(kv); // applied when the module is declared
        }
    }
}

bool EngineModules::enabled(const std::string& id) const {
    for (const auto& m : mods_)
        if (m.id == id) return m.enabled;
    return true; // unknown ids never gate anything
}

EngineModuleDesc* EngineModules::find(const std::string& id) {
    for (auto& m : mods_)
        if (m.id == id) return &m;
    return nullptr;
}

void EngineModules::setEnabled(const std::string& id, bool on, ComponentRegistry& registry) {
    EngineModuleDesc* m = find(id);
    if (!m || m->enabled == on) return;
    m->enabled = on;
    if (on) {
        if (m->registerComponents) m->registerComponents(registry, m->moduleId);
        AE_LOG("[Modules] enabled %s", m->id.c_str());
    } else {
        registry.unregisterModule(m->moduleId);
        AE_LOG("[Modules] disabled %s (scene components of its types stop loading)",
               m->id.c_str());
    }
}

} // namespace ae
