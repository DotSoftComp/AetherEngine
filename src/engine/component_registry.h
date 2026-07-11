// Aether Engine — the component type registry.
//
// One instance lives inside AetherCore.dll (componentRegistry()); the editor,
// the game runtime, and per-project GameModule DLLs all register into and read
// from the same table. A registered type gets, with zero further code:
// serialization, duplication, prefab support, an Add Component entry, and a
// reflected inspector (see reflect.h).
//
// moduleId groups registrations so a game module's types can be unregistered
// wholesale before its DLL is unloaded (script rebuild/reload).
#pragma once
#include "entity.h"
#include "reflect.h"
#include <functional>
#include <string>
#include <vector>

namespace ae {

class AssetLibrary;

struct ComponentDesc {
    std::string type;         // serialized "type" string — the file format, stable
    std::string displayName;  // editor-facing name
    std::string category;     // Add Component grouping ("Rendering", "Scripts", ...)
    int moduleId = 0;         // 0 = engine built-ins; >0 = game module
    bool userCreatable = true; // shown in the Add Component menu
    std::function<Component*(Entity&)> create;
    // Editor-only: sensible defaults when created from the Add Component menu
    // (a fresh MeshRenderer gets a sphere, a DialogueTrigger loads its scene).
    std::function<void(Component*, AssetLibrary&)> initDefaults;
};

class ComponentRegistry {
public:
    template <typename T>
    ComponentDesc& add(const char* type, const char* displayName, const char* category,
                       int moduleId = 0) {
        ComponentDesc d;
        d.type = type;
        d.displayName = displayName;
        d.category = category;
        d.moduleId = moduleId;
        d.create = [](Entity& e) -> Component* { return e.addComponent<T>(); };
        return add(std::move(d));
    }

    ComponentDesc& add(ComponentDesc d);
    const ComponentDesc* find(const std::string& type) const;
    const std::vector<ComponentDesc>& all() const { return descs_; }
    void unregisterModule(int moduleId);

private:
    std::vector<ComponentDesc> descs_;
};

// The single engine-wide registry (lives in AetherCore.dll).
ComponentRegistry& componentRegistry();

// Registers every built-in component type. Idempotent; called at startup by
// each host executable before any scene is loaded.
void registerBuiltinComponents();

// ---- game-module scripting -------------------------------------------------

// Bumped whenever Component / PropertyVisitor / Entity layout changes in a way
// that breaks compiled game modules; the loader refuses mismatched DLLs.
#define AE_ABI_VERSION 1

// moduleId used for a project's GameModule.dll registrations.
constexpr int kGameModuleId = 1;

namespace detail {
// AE_REGISTER_COMPONENT appends to this list at the module DLL's static-init;
// GameModule_Register drains it into the registry with the module's id.
struct PendingRegistration {
    using Fn = void (*)(ComponentRegistry&, int moduleId);
    explicit PendingRegistration(Fn fn);
};
void runPendingRegistrations(ComponentRegistry& r, int moduleId);
} // namespace detail

} // namespace ae

// Inside a script class: stable type name + editor display name + category.
#define AE_COMPONENT(ClassName, DisplayName, Category)                          \
    const char* typeName() const override { return #ClassName; }                \
    static const char* aeDisplayName() { return DisplayName; }                  \
    static const char* aeCategory() { return Category; }

// Once in the script's .cpp: queues the class for GameModule_Register.
#define AE_REGISTER_COMPONENT(ClassName)                                        \
    static ::ae::detail::PendingRegistration s_aeReg_##ClassName(               \
        [](::ae::ComponentRegistry& r, int moduleId) {                          \
            r.add<ClassName>(#ClassName, ClassName::aeDisplayName(),            \
                             ClassName::aeCategory(), moduleId);                \
        });
