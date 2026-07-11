// Aether Engine game module entry point — do not remove.
//
// The engine loads this project's compiled GameModule.dll and calls
// GameModule_Register, which registers every script marked with
// AE_REGISTER_COMPONENT into the engine's component registry.
#include <ae.h>

extern "C" __declspec(dllexport) int GameModule_AbiVersion() {
    return AE_ABI_VERSION;
}

extern "C" __declspec(dllexport) void GameModule_Register(ae::ComponentRegistry& registry) {
    ae::detail::runPendingRegistrations(registry, ae::kGameModuleId);
}
