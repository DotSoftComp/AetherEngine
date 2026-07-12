// Aether Hub — engine auto-update. Checks the newest GitHub release against
// the newest installed engine, downloads the win64 zip and installs it
// SIDE-BY-SIDE under %LOCALAPPDATA%/AetherEngine/Versions/<version>/ — an
// update never touches an existing install, because projects pin an engine
// version and the running hub/editor may live in one.
//
// All network and disk work happens on a private worker thread; the hub polls
// phase() every frame and reads the result fields only in the phases where
// they are documented valid.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>

namespace ae {

class EngineUpdater {
public:
    enum class Phase {
        Idle,            // nothing happened yet
        Checking,        // GET releases/latest in flight
        UpToDate,        // no release newer than installedVersion
        CheckFailed,     // offline / rate-limited — quiet, not an error banner
        UpdateAvailable, // latestVersion()/assetName() valid
        Downloading,     // downloadedBytes()/totalBytes() advancing
        Extracting,
        Installed,       // installedPath()/latestVersion() ready to register
        Failed,          // error() valid
    };

    ~EngineUpdater();

    // Kick an async check. `installedVersion` is the newest version already on
    // this machine (empty = none). No-op while a check/install is running.
    void checkForUpdate(const std::string& installedVersion);

    // Download + extract the release found by checkForUpdate. Only valid in
    // UpdateAvailable (or Failed, to retry).
    void install();

    // Abort an in-flight download (partial file is deleted).
    void cancel() { cancelled_ = true; }

    Phase phase() const { return phase_; }
    long long downloadedBytes() const { return downloaded_; }
    long long totalBytes() const { return total_; }

    std::string latestVersion() const { return locked(latestVersion_); }
    std::string releaseNotesUrl() const { return locked(notesUrl_); }
    std::string installedPath() const { return locked(installedPath_); }
    std::string error() const { return locked(error_); }

    // One-shot: true the first time it is called after an install finishes, so
    // the hub registers the new engine exactly once.
    bool takeInstalledEvent();

private:
    void runCheck(std::string installedVersion);
    void runInstall();
    void join();
    std::string locked(const std::string& s) const {
        std::lock_guard<std::mutex> lock(mutex_);
        return s;
    }

    std::thread worker_;
    mutable std::mutex mutex_;
    std::atomic<Phase> phase_{Phase::Idle};
    std::atomic<bool> cancelled_{false};
    std::atomic<bool> installedEvent_{false};
    std::atomic<long long> downloaded_{0}, total_{-1};

    // Written by the worker under mutex_, read by the UI thread via locked().
    std::string latestVersion_, notesUrl_, assetUrl_, assetName_;
    std::string installedPath_, error_;
};

} // namespace ae
