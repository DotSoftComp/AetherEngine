// Aether Engine — DLL linkage annotations for the engine core (AetherCore.dll).
//
// Functions and classes are exported wholesale via CMake's
// WINDOWS_EXPORT_ALL_SYMBOLS, which covers code but not the consumer side of
// global *data* symbols. AE_API therefore expands to dllimport for consumers
// (editor/runtime/hub exes, game modules) and to nothing while building the
// core itself — apply it to every `extern` global the core exposes (the GL
// function pointers, notably).
#pragma once

#if defined(AE_BUILD_CORE)
#define AE_API
#else
#define AE_API __declspec(dllimport)
#endif
