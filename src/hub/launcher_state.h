// Aether Hub — persistent launcher state: every known engine install and
// project on this machine, stored at %APPDATA%/AetherEngine/launcher.json.
// The hub (and nothing else) reads and writes this file.
#pragma once
#include <string>
#include <vector>

namespace ae {

struct EngineInstall {
    std::string path;    // folder containing engine.json + the exes
    std::string version; // from engine.json
};

// Dotted-numeric ("semver-ish") version order: -1 / 0 / +1 when a < / == / > b.
// "0.10.0" beats "0.9.0"; missing segments count as 0; non-numeric segments
// fall back to string comparison.
int compareEngineVersions(const std::string& a, const std::string& b);

struct KnownProject {
    std::string name;
    std::string path;          // the .aeproj file
    std::string engineVersion; // engine it was created/last opened with
    long long lastOpened = 0;  // unix seconds; 0 = never
};

struct LauncherState {
    std::vector<EngineInstall> engines;
    std::vector<KnownProject> projects;

    bool load();       // missing file = empty state (first run)
    bool save() const;
    void pruneDeadPaths();

    // Both are upserts keyed on path.
    void registerEngine(const std::string& path, const std::string& version);
    void registerProject(const std::string& projFile, const std::string& name,
                         const std::string& engineVersion);
    void touchProject(const std::string& projFile); // updates lastOpened = now

    // Best engine for a project: exact version match, else the newest install.
    // Returns nullptr when no engine is registered at all.
    const EngineInstall* engineFor(const std::string& version, bool* exactMatch) const;

    static std::string stateFilePath(); // %APPDATA%/AetherEngine/launcher.json
};

} // namespace ae
