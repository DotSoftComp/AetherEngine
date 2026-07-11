// Aether Engine — packages a project into a standalone distributable game
// folder:
//
//   <outputDir>/<GameName>/
//     <GameName>.exe        (AetherRuntime, renamed)
//     AetherCore.dll
//     game.json             (name, startup scene, module, development flag)
//     shaders/              (engine install's shaders)
//     assets/               (project content, copy-all)
//     Binaries/GameModule.dll   (when the project has native scripts)
//
// The runtime finds game.json next to its exe and needs nothing else — no
// engine install, no --project, no source tree.
#pragma once
#include <functional>
#include <string>

namespace ae {

struct Project;

struct PackageOptions {
    std::string outputDir;    // parent folder; the game folder is created inside
    std::string gameName;     // folder + exe name (defaults to project name)
    std::string startupScene; // project-relative map (defaults to the project's)
    bool development = false; // dev builds keep console/log output attached
};

// Blocking (module compile + file copies) — run on a worker thread from UI.
// Streams progress lines to onLine; returns true on a complete package.
bool packageGame(const Project& project, const PackageOptions& opts,
                 const std::function<void(const std::string&)>& onLine);

} // namespace ae
