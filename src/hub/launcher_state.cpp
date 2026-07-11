#include "launcher_state.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace ae {

namespace {
std::string jsonEsc(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += c; }
        else if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}
} // namespace

std::string LauncherState::stateFilePath() {
    char appData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_APPDATA, nullptr, 0, appData))) return {};
    std::string dir = joinPath(appData, "AetherEngine");
    CreateDirectoryA(dir.c_str(), nullptr);
    return joinPath(dir, "launcher.json");
}

bool LauncherState::load() {
    engines.clear();
    projects.clear();
    std::ifstream f(stateFilePath(), std::ios::binary);
    if (!f) return true; // first run
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc) || doc.type != JsonValue::Object) {
        AE_WARN("[Hub] malformed launcher.json — starting fresh");
        return false;
    }
    if (const JsonValue* arr = doc.find("engines")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            EngineInstall e;
            if (const std::string* s = (*arr)[i].string("path")) e.path = *s;
            if (const std::string* s = (*arr)[i].string("version")) e.version = *s;
            if (!e.path.empty()) engines.push_back(std::move(e));
        }
    }
    if (const JsonValue* arr = doc.find("projects")) {
        for (size_t i = 0; i < arr->size(); ++i) {
            KnownProject p;
            if (const std::string* s = (*arr)[i].string("name")) p.name = *s;
            if (const std::string* s = (*arr)[i].string("path")) p.path = *s;
            if (const std::string* s = (*arr)[i].string("engineVersion")) p.engineVersion = *s;
            p.lastOpened = (long long)(*arr)[i].num("lastOpened", 0);
            if (!p.path.empty()) projects.push_back(std::move(p));
        }
    }
    return true;
}

bool LauncherState::save() const {
    std::ostringstream o;
    o << "{\n  \"engines\": [";
    for (size_t i = 0; i < engines.size(); ++i)
        o << (i ? "," : "") << "\n    { \"path\": \"" << jsonEsc(engines[i].path)
          << "\", \"version\": \"" << jsonEsc(engines[i].version) << "\" }";
    o << (engines.empty() ? "" : "\n  ") << "],\n  \"projects\": [";
    for (size_t i = 0; i < projects.size(); ++i)
        o << (i ? "," : "") << "\n    { \"name\": \"" << jsonEsc(projects[i].name)
          << "\", \"path\": \"" << jsonEsc(projects[i].path) << "\", \"engineVersion\": \""
          << jsonEsc(projects[i].engineVersion) << "\", \"lastOpened\": "
          << projects[i].lastOpened << " }";
    o << (projects.empty() ? "" : "\n  ") << "]\n}\n";
    std::ofstream f(stateFilePath(), std::ios::binary);
    if (!f) return false;
    f << o.str();
    return true;
}

void LauncherState::pruneDeadPaths() {
    engines.erase(std::remove_if(engines.begin(), engines.end(),
                                 [](const EngineInstall& e) {
                                     return !pathExists(joinPath(e.path, "engine.json"));
                                 }),
                  engines.end());
    projects.erase(std::remove_if(projects.begin(), projects.end(),
                                  [](const KnownProject& p) { return !pathExists(p.path); }),
                   projects.end());
}

void LauncherState::registerEngine(const std::string& path, const std::string& version) {
    for (EngineInstall& e : engines) {
        if (_stricmp(e.path.c_str(), path.c_str()) == 0) {
            e.version = version;
            return;
        }
    }
    engines.push_back({path, version});
}

void LauncherState::registerProject(const std::string& projFile, const std::string& name,
                                    const std::string& engineVersion) {
    for (KnownProject& p : projects) {
        if (_stricmp(p.path.c_str(), projFile.c_str()) == 0) {
            p.name = name;
            p.engineVersion = engineVersion;
            return;
        }
    }
    KnownProject p;
    p.name = name;
    p.path = projFile;
    p.engineVersion = engineVersion;
    projects.push_back(std::move(p));
}

void LauncherState::touchProject(const std::string& projFile) {
    for (KnownProject& p : projects)
        if (_stricmp(p.path.c_str(), projFile.c_str()) == 0) p.lastOpened = (long long)time(nullptr);
}

const EngineInstall* LauncherState::engineFor(const std::string& version, bool* exactMatch) const {
    if (exactMatch) *exactMatch = false;
    const EngineInstall* best = nullptr;
    for (const EngineInstall& e : engines) {
        if (e.version == version) {
            if (exactMatch) *exactMatch = true;
            return &e;
        }
        if (!best || e.version > best->version) best = &e; // lexicographic ~ semver-ish
    }
    return best;
}

} // namespace ae
