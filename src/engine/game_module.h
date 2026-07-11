// Aether Engine — loads a project's compiled GameModule.dll (native C++
// scripts) into the running engine.
//
// The DLL is hot-copied to Intermediate/Hot/ before LoadLibrary so the linker
// can overwrite Binaries/GameModule.dll while a copy is loaded (rebuild-on-
// demand). Its exported GameModule_Register() adds the script component types
// to the shared ComponentRegistry; unload() removes them again and frees the
// DLL. Every live component whose vtable is inside the DLL must be destroyed
// (world cleared) BEFORE unload().
#pragma once
#include <string>

namespace ae {

struct Project;

class GameModule {
public:
    ~GameModule(); // does NOT unload — see main()s: at exit the OS reclaims it

    // Loads <project>/Binaries/<module>.dll. False (with a log line) when the
    // project has no module, the binary doesn't exist yet, or ABI mismatches.
    // hotCopy=true (editor) loads a copy so the original stays rebuildable;
    // packaged/runtime loads the DLL in place (game folders may be read-only).
    bool load(const Project& project, bool hotCopy = true);
    // Unregisters the module's component types and frees the DLL.
    void unload();
    bool loaded() const { return handle_ != nullptr; }

private:
    void* handle_ = nullptr; // HMODULE
};

} // namespace ae
