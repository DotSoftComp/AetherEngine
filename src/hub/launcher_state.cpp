#include "launcher_state.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <algorithm>
#include <cstdlib>
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

int compareEngineVersions(const std::string& a, const std::string& b) {
    size_t ia = 0, ib = 0;
    while (ia < a.size() || ib < b.size()) {
        size_t ea = a.find('.', ia), eb = b.find('.', ib);
        std::string sa = a.substr(ia, (ea == std::string::npos ? a.size() : ea) - ia);
        std::string sb = b.substr(ib, (eb == std::string::npos ? b.size() : eb) - ib);
        char* endA = nullptr;
        char* endB = nullptr;
        long na = strtol(sa.c_str(), &endA, 10), nb = strtol(sb.c_str(), &endB, 10);
        bool numA = endA && *endA == 0, numB = endB && *endB == 0; // "" parses as 0
        if (numA && numB) {
            if (na != nb) return na < nb ? -1 : 1;
        } else if (sa != sb) {
            return sa < sb ? -1 : 1;
        }
        ia = ea == std::string::npos ? a.size() : ea + 1;
        ib = eb == std::string::npos ? b.size() : eb + 1;
    }
    return 0;
}

bool isCompatibleUpgrade(const std::string& pinned, const std::string& candidate) {
    if (pinned.empty() || candidate.empty()) return false;
    int majPinned = (int)strtol(pinned.c_str(), nullptr, 10);
    int majCand = (int)strtol(candidate.c_str(), nullptr, 10);
    if (majPinned != majCand) return false; // major bump = breaking, never auto-offered
    return compareEngineVersions(candidate, pinned) > 0;
}

const EngineInstall* LauncherState::newestCompatibleEngine(const std::string& pinned) const {
    const EngineInstall* best = nullptr;
    for (const EngineInstall& e : engines) {
        if (!isCompatibleUpgrade(pinned, e.version)) continue;
        if (!best || compareEngineVersions(e.version, best->version) > 0) best = &e;
    }
    return best;
}

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
        if (!best || compareEngineVersions(e.version, best->version) > 0) best = &e;
    }
    return best;
}

} // namespace ae
