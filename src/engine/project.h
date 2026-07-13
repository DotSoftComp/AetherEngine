// Aether Engine — project descriptor (project.aeproj).
//
// A project is a folder with a project.aeproj manifest at its root:
//
//   {
//     "name": "MyGame",
//     "engineVersion": "0.1.0",
//     "startupScene": "assets/maps/main.json",
//     "module": { "name": "GameModule", "sourceDir": "Source" },  // optional
//     "modules": { "narrative": false }                           // optional
//   }
//
// assets/ paths inside scenes stay project-relative (AssetLibrary resolves
// them against `root`), so projects are fully relocatable. Shaders belong to
// the engine install, not the project (core/paths.h).
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace ae {

struct Project {
    std::string root;          // absolute project directory (no trailing slash)
    std::string file;          // absolute path of the .aeproj manifest
    std::string name;
    std::string engineVersion;
    std::string startupScene;  // project-relative; may be empty
    std::string moduleName;    // "" = no native game module
    std::string sourceDir;     // project-relative scripts dir (default "Source")
    // Optional engine-module toggles ("modules": {"physics": false, ...});
    // ids missing here stay at their default (enabled). Order preserved.
    std::vector<std::pair<std::string, bool>> moduleFlags;

    bool hasModule() const { return !moduleName.empty(); }

    // Accepts a .aeproj path or a directory containing project.aeproj.
    bool load(const std::string& pathOrDir);
    bool save(const std::string& path) const;
};

// New Project: recursively copies a template directory (skipping Intermediate/,
// Binaries/, .git/) and rewrites the manifest's name + engineVersion.
// On success fills outProjectFile with the new .aeproj path.
bool createProjectFromTemplate(const std::string& templateDir, const std::string& destDir,
                               const std::string& name, const std::string& engineVersion,
                               std::string* outProjectFile, std::string* outError);

// Re-pins a project to a different engine version by editing ONLY the
// `engineVersion` value in the manifest text — every other field, unknown key,
// and the file's formatting is preserved (a full load/save would rewrite the
// whole manifest). Used by the Hub's "change base engine" action.
bool setProjectEngineVersion(const std::string& projFile, const std::string& version,
                             std::string* outError = nullptr);

} // namespace ae
