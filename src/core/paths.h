// Aether Engine — canonical filesystem roots.
//
// Two distinct roots, never conflated:
//   engine install root  — where AetherCore.dll lives: shaders/, engine.json,
//                          Templates/, the exes. Found from the DLL itself.
//   project root         — where a project.aeproj lives: assets/, Source/.
//                          Always given explicitly (--project / game.json),
//                          never discovered by walking the filesystem.
//
// All new path handling should funnel through here so a future UTF-16
// migration stays localized.
#pragma once
#include <string>

namespace ae {

// Directory containing AetherCore.dll (no trailing slash).
std::string engineBinDir();

// Engine install root: engineBinDir(), or a bounded walk-up to the directory
// holding engine.json / shaders/ for layouts where binaries sit deeper.
std::string engineRoot();

// <engineRoot>/shaders/ (with trailing backslash, ready for concatenation).
std::string engineShaderDir();

// Templates/ directory of this engine install. In a packaged install it sits
// next to the binaries; in a dev checkout it lives at the repo root, so this
// walks up from the bin dir until it finds one. Empty string if none exists.
std::string engineTemplatesDir();

// Small helpers shared by project/packaging code.
bool pathExists(const std::string& path);
bool isDirectory(const std::string& path);
std::string parentPath(const std::string& path);   // "" when no parent remains
std::string joinPath(const std::string& a, const std::string& b);

} // namespace ae
