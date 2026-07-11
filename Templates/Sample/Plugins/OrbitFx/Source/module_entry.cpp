// Aether plugin entry point — do not remove.
//
// Same SDK as a game module, but RegisterAs receives the module id the engine
// allotted this plugin, so its component types can be unregistered wholesale
// when the plugin is unloaded or reloaded.
#include <ae.h>

extern "C" __declspec(dllexport) int GameModule_AbiVersion() {
    return AE_ABI_VERSION;
}

extern "C" __declspec(dllexport) void GameModule_RegisterAs(ae::ComponentRegistry& registry,
                                                            int moduleId) {
    ae::detail::runPendingRegistrations(registry, moduleId);
}
