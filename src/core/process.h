// Aether Engine — child-process execution with captured output (the script
// compiler and the game packager shell out to cmake through this).
#pragma once
#include <functional>
#include <string>

namespace ae {

// Runs `cmdline` in `cwd` (empty = inherit), streaming each output line
// (stdout+stderr merged) to onLine. Blocks until exit; returns the process
// exit code, or -1 if it could not be started.
int runProcessCaptured(const std::string& cmdline, const std::string& cwd,
                       const std::function<void(const std::string&)>& onLine);

// Fire-and-forget launch (the hub opening an editor). Returns false when the
// process could not be started.
bool launchDetached(const std::string& cmdline, const std::string& cwd);

} // namespace ae
