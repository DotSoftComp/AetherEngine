// Aether Engine — builds a project's GameModule.dll from <project>/Source/
// via CMake + the same MSVC toolchain the engine was built with. The SDK
// (headers + AetherCore import lib) locations come from the engine install's
// engine.json manifest.
#pragma once
#include <functional>
#include <string>

namespace ae {

struct Project;

// Regenerates <Source>/CMakeLists.txt, then runs cmake configure (first time)
// + build into <project>/Intermediate, emitting compiler output line-by-line
// to onLine. Blocking — call from a worker thread in interactive contexts.
// Returns true when Binaries/<module>.dll built successfully.
bool buildGameModule(const Project& project, const std::string& config,
                     const std::function<void(const std::string&)>& onLine);

// Same scaffold for any module folder (plugins): compiles <sourceDir>/*.cpp
// into <binariesDir>/<moduleName>.dll via <intermediateDir>.
bool buildModuleAt(const std::string& sourceDir, const std::string& moduleName,
                   const std::string& binariesDir, const std::string& intermediateDir,
                   const std::string& config,
                   const std::function<void(const std::string&)>& onLine);

} // namespace ae
