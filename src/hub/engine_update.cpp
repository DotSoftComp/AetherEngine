#include "engine_update.h"
#include "launcher_state.h"
#include "../core/http.h"
#include "../core/json.h"
#include "../core/log.h"
#include "../core/paths.h"
#include "../core/process.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shlobj.h>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace fs = std::filesystem;

namespace ae {

namespace {

// Update channel: the engine's public GitHub repo. Releases are published by
// .github/workflows/release.yml on every v* tag.
constexpr const char* kRepo = "DotSoftComp/AetherEngine";
constexpr const char* kAssetSuffix = "-win64.zip";

// %LOCALAPPDATA%/AetherEngine — machine-local, user-writable (never Program
// Files), distinct from %APPDATA% where launcher.json lives.
std::string updateRoot() {
    char localAppData[MAX_PATH] = {};
    if (FAILED(SHGetFolderPathA(nullptr, CSIDL_LOCAL_APPDATA, nullptr, 0, localAppData)))
        return {};
    std::string dir = joinPath(localAppData, "AetherEngine");
    CreateDirectoryA(dir.c_str(), nullptr);
    return dir;
}

std::string manifestVersion(const std::string& installDir) {
    std::ifstream f(joinPath(installDir, "engine.json"), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc)) return {};
    return doc.string("version") ? *doc.string("version") : std::string();
}

// Windows 10 1803+ ships bsdtar, which extracts zip archives.
std::string tarExe() {
    char sysRoot[MAX_PATH] = {};
    GetEnvironmentVariableA("SystemRoot", sysRoot, MAX_PATH);
    std::string tar = joinPath(joinPath(sysRoot, "System32"), "tar.exe");
    return pathExists(tar) ? tar : "tar";
}

} // namespace

EngineUpdater::~EngineUpdater() {
    cancelled_ = true;
    join();
}

void EngineUpdater::join() {
    if (worker_.joinable()) worker_.join();
}

bool EngineUpdater::takeInstalledEvent() {
    return installedEvent_.exchange(false);
}

void EngineUpdater::checkForUpdate(const std::string& installedVersion) {
    Phase p = phase_;
    if (p == Phase::Checking || p == Phase::Downloading || p == Phase::Extracting) return;
    join();
    phase_ = Phase::Checking;
    worker_ = std::thread(&EngineUpdater::runCheck, this, installedVersion);
}

void EngineUpdater::install() {
    Phase p = phase_;
    if (p != Phase::UpdateAvailable && p != Phase::Failed) return;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (assetUrl_.empty()) return;
    }
    join();
    cancelled_ = false;
    downloaded_ = 0;
    phase_ = Phase::Downloading;
    worker_ = std::thread(&EngineUpdater::runInstall, this);
}

void EngineUpdater::runCheck(std::string installedVersion) {
    std::string url = std::string("https://api.github.com/repos/") + kRepo + "/releases/latest";
    HttpResponse res = httpGet(url, {{"Accept", "application/vnd.github+json"}}, 20000);
    if (res.status == 404) { // repo has no releases yet
        phase_ = Phase::UpToDate;
        return;
    }
    if (!res.ok()) {
        AE_WARN("[Hub] update check failed (%d): %s", res.status,
                res.error.empty() ? url.c_str() : res.error.c_str());
        phase_ = Phase::CheckFailed;
        return;
    }
    JsonValue doc;
    if (!jsonParse(res.body.c_str(), res.body.size(), doc) || doc.type != JsonValue::Object) {
        phase_ = Phase::CheckFailed;
        return;
    }

    std::string tag = doc.string("tag_name") ? *doc.string("tag_name") : std::string();
    std::string version = (!tag.empty() && (tag[0] == 'v' || tag[0] == 'V')) ? tag.substr(1) : tag;
    std::string assetUrl, assetName;
    long long assetSize = -1;
    if (const JsonValue* assets = doc.find("assets")) {
        for (size_t i = 0; i < assets->size(); ++i) {
            const std::string* name = (*assets)[i].string("name");
            const std::string* dl = (*assets)[i].string("browser_download_url");
            if (!name || !dl) continue;
            if (name->size() >= strlen(kAssetSuffix) &&
                _stricmp(name->c_str() + name->size() - strlen(kAssetSuffix), kAssetSuffix) == 0) {
                assetName = *name;
                assetUrl = *dl;
                assetSize = (long long)(*assets)[i].num("size", -1);
                break;
            }
        }
    }
    if (version.empty() || assetUrl.empty()) {
        AE_WARN("[Hub] latest release has no %s asset — skipping update", kAssetSuffix);
        phase_ = Phase::UpToDate;
        return;
    }
    if (!installedVersion.empty() && compareEngineVersions(version, installedVersion) <= 0) {
        phase_ = Phase::UpToDate;
        return;
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latestVersion_ = version;
        assetUrl_ = assetUrl;
        assetName_ = assetName;
        notesUrl_ = doc.string("html_url") ? *doc.string("html_url") : std::string();
    }
    total_ = assetSize;
    phase_ = Phase::UpdateAvailable;
}

void EngineUpdater::runInstall() {
    std::string version, assetUrl, assetName;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        version = latestVersion_;
        assetUrl = assetUrl_;
        assetName = assetName_;
    }
    auto fail = [&](const std::string& why) {
        AE_WARN("[Hub] update failed: %s", why.c_str());
        std::lock_guard<std::mutex> lock(mutex_);
        error_ = why;
        phase_ = Phase::Failed;
    };

    std::string root = updateRoot();
    if (root.empty()) return fail("cannot resolve %LOCALAPPDATA%");
    std::string versionsDir = joinPath(root, "Versions");
    std::string installDir = joinPath(versionsDir, version);

    // Already installed (e.g. a previous run finished but the hub closed
    // before registering it) — just hand it back.
    if (pathExists(joinPath(installDir, "engine.json"))) {
        std::lock_guard<std::mutex> lock(mutex_);
        installedPath_ = installDir;
        installedEvent_ = true;
        phase_ = Phase::Installed;
        return;
    }

    // ---- download --------------------------------------------------------
    std::string dlDir = joinPath(root, "Downloads");
    CreateDirectoryA(dlDir.c_str(), nullptr);
    std::string zipPath = joinPath(dlDir, assetName);
    std::string err;
    if (!httpDownloadToFile(assetUrl, zipPath, {},
                            [&](long long done, long long total) {
                                downloaded_ = done;
                                if (total > 0) total_ = total;
                                return !cancelled_.load();
                            },
                            &err))
        return fail(err);

    // ---- extract ----------------------------------------------------------
    phase_ = Phase::Extracting;
    std::error_code ec;
    fs::remove_all(installDir, ec); // clear any half-extracted attempt
    fs::create_directories(installDir, ec);
    if (ec) return fail("cannot create " + installDir);

    std::string tarOut;
    int code = runProcessCaptured(
        "\"" + tarExe() + "\" -xf \"" + zipPath + "\" -C \"" + installDir + "\"", std::string(),
        [&](const std::string& line) { tarOut += line + "\n"; });
    DeleteFileA(zipPath.c_str());
    if (code != 0) {
        fs::remove_all(installDir, ec);
        return fail("zip extraction failed (tar exit " + std::to_string(code) + "): " + tarOut);
    }

    // Accept both zip layouts: files at the archive root, or everything under
    // one wrapper folder.
    std::string finalDir = installDir;
    if (!pathExists(joinPath(finalDir, "engine.json"))) {
        std::string onlySubdir;
        int entries = 0;
        for (const fs::directory_entry& e : fs::directory_iterator(installDir, ec)) {
            ++entries;
            if (e.is_directory()) onlySubdir = e.path().string();
        }
        if (entries == 1 && !onlySubdir.empty() &&
            pathExists(joinPath(onlySubdir, "engine.json")))
            finalDir = onlySubdir;
    }
    std::string actual = manifestVersion(finalDir);
    if (actual.empty()) {
        fs::remove_all(installDir, ec);
        return fail("downloaded archive is not an engine install (no engine.json)");
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        latestVersion_ = actual; // manifest is the source of truth
        installedPath_ = finalDir;
    }
    installedEvent_ = true;
    phase_ = Phase::Installed;
    AE_LOG("[Hub] engine %s installed at %s", actual.c_str(), finalDir.c_str());
}

} // namespace ae
