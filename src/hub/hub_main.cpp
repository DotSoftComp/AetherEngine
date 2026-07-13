// Aether Hub (AetherHub.exe) — the launcher: lists engine installs and every
// known project, creates projects from the engine's Templates/, and opens a
// project in the matching engine version's editor. Epic-launcher-style, but
// small: one ImGui window over launcher.json (%APPDATA%/AetherEngine).
#include "launcher_state.h"
#include "engine_update.h"
#include "../core/log.h"
#include "../core/json.h"
#include "../core/paths.h"
#include "../core/process.h"
#include "../core/window.h"
#include "../rhi/rhi.h"
#include "../engine/project.h"
#include "../editor/imgui_layer.h"
#include "imgui.h"
#include <commdlg.h>
#include <shellapi.h>
#include <algorithm>
#include <cfloat>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>

using namespace ae;

namespace {

std::string readEngineVersion(const std::string& engineDir) {
    std::ifstream f(joinPath(engineDir, "engine.json"), std::ios::binary);
    if (!f) return {};
    std::ostringstream ss;
    ss << f.rdbuf();
    std::string text = ss.str();
    JsonValue doc;
    if (!jsonParse(text.c_str(), text.size(), doc)) return {};
    return doc.string("version") ? *doc.string("version") : std::string();
}

std::string pickFileDialog(Window& window, const char* filter, const char* title) {
    char file[MAX_PATH] = {};
    OPENFILENAMEA ofn{};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window.hwnd();
    ofn.lpstrFilter = filter;
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrTitle = title;
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST;
    if (!GetOpenFileNameA(&ofn)) return {};
    return file;
}

std::string whenText(long long unixSecs) {
    if (unixSecs <= 0) return "never";
    std::time_t t = (std::time_t)unixSecs;
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M", &tm);
    return buf;
}

struct HubUI {
    LauncherState state;
    std::string selfVersion;
    EngineUpdater updater;
    bool quit = false; // set when restarting into a freshly installed hub
    // New Project form
    char newName[128] = "MyGame";
    char newLocation[260] = {};
    std::vector<std::string> templates; // template dir names
    int templateIndex = 0;
    std::string lastError;
    // Deferred open + "switch base engine on open?" modal state.
    std::string pendingOpen;     // project path clicked this frame (resolved after the table)
    std::string switchProjPath;  // project the modal is about
    std::string switchToVersion; // the compatible newer engine the modal offers

    KnownProject* findProject(const std::string& path) {
        for (KnownProject& p : state.projects)
            if (p.path == path) return &p;
        return nullptr;
    }

    // Re-pins a project to a different installed engine version. Edits only the
    // manifest's engineVersion (all other fields preserved) + updates
    // launcher.json. The editor refreshes generated docs on next open.
    void repinProject(KnownProject& p, const std::string& version) {
        std::string err;
        if (!setProjectEngineVersion(p.path, version, &err)) { lastError = err; return; }
        p.engineVersion = version;
        state.registerProject(p.path, p.name, version);
        state.save();
        lastError.clear();
    }

    // Newest engine version on this machine — what a release must beat.
    std::string newestInstalledVersion() const {
        std::string best = selfVersion;
        for (const EngineInstall& e : state.engines)
            if (compareEngineVersions(e.version, best) > 0) best = e.version;
        return best;
    }

    void openProject(KnownProject& p) {
        bool exact = false;
        const EngineInstall* engine = state.engineFor(p.engineVersion, &exact);
        if (!engine) {
            lastError = "No engine installs registered.";
            return;
        }
        std::string editor = joinPath(engine->path, "AetherEditor.exe");
        if (!pathExists(editor)) {
            lastError = "AetherEditor.exe missing in " + engine->path;
            return;
        }
        if (!launchDetached("\"" + editor + "\" --project \"" + p.path + "\"", engine->path)) {
            lastError = "Failed to launch " + editor;
            return;
        }
        if (!exact)
            AE_WARN("[Hub] no engine %s installed — opened with %s", p.engineVersion.c_str(),
                    engine->version.c_str());
        state.touchProject(p.path);
        state.save();
        lastError.clear();
    }

    // "A newer compatible engine is installed — switch this project to it?"
    // Shown on Open (opened by drawProjectsTab). Project files are untouched;
    // only the base engine pin and the editor-regenerated reference docs change.
    void drawSwitchModal() {
        ImVec2 center = ImGui::GetMainViewport()->GetCenter();
        ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        if (!ImGui::BeginPopupModal("Switch base engine?", nullptr,
                                    ImGuiWindowFlags_AlwaysAutoResize))
            return;
        KnownProject* p = findProject(switchProjPath);
        if (!p) {
            ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return;
        }
        ImGui::Text("A newer compatible engine is installed.");
        ImGui::Spacing();
        ImGui::Text("\"%s\" is pinned to Aether %s.", p->name.c_str(), p->engineVersion.c_str());
        ImGui::Text("Switch it to %s and open?", switchToVersion.c_str());
        ImGui::TextDisabled("Same major version — a compatible update. Your project files are\n"
                            "untouched; only the base engine changes and the editor refreshes\n"
                            "the generated reference docs to match.");
        ImGui::Spacing();
        char openPinned[80];
        std::snprintf(openPinned, sizeof(openPinned), "Open on %s", p->engineVersion.c_str());
        if (ImGui::Button("Switch & open")) {
            repinProject(*p, switchToVersion);
            openProject(*p);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button(openPinned)) {
            openProject(*p);
            ImGui::CloseCurrentPopup();
        }
        ImGui::SameLine();
        if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    void drawProjectsTab(Window& window) {
        // ---- New Project ----
        ImGui::SeparatorText("New Project");
        ImGui::SetNextItemWidth(220);
        ImGui::InputText("Name", newName, sizeof(newName));
        ImGui::SetNextItemWidth(420);
        ImGui::InputText("Location", newLocation, sizeof(newLocation));
        if (!templates.empty()) {
            ImGui::SetNextItemWidth(220);
            if (ImGui::BeginCombo("Template", templates[templateIndex].c_str())) {
                for (int i = 0; i < (int)templates.size(); ++i)
                    if (ImGui::Selectable(templates[i].c_str(), i == templateIndex))
                        templateIndex = i;
                ImGui::EndCombo();
            }
        } else {
            ImGui::TextDisabled("No Templates/ found in this engine install.");
        }
        ImGui::BeginDisabled(templates.empty() || !newName[0] || !newLocation[0]);
        if (ImGui::Button("Create Project")) {
            std::string tmplDir = joinPath(engineTemplatesDir(), templates[templateIndex]);
            std::string dest = joinPath(newLocation, newName);
            std::string projFile, err;
            if (createProjectFromTemplate(tmplDir, dest, newName, selfVersion, &projFile, &err)) {
                state.registerProject(projFile, newName, selfVersion);
                state.save();
                lastError.clear();
                for (KnownProject& p : state.projects)
                    if (p.path == projFile) openProject(p);
            } else {
                lastError = err;
            }
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Import .aeproj...")) {
            std::string file =
                pickFileDialog(window, "Aether project (*.aeproj)\0*.aeproj\0\0",
                               "Import Aether project");
            if (!file.empty()) {
                Project proj;
                if (proj.load(file)) {
                    state.registerProject(proj.file, proj.name, proj.engineVersion);
                    state.save();
                    lastError.clear();
                } else {
                    lastError = "Not a valid .aeproj: " + file;
                }
            }
        }

        // ---- Project list ----
        ImGui::SeparatorText("Projects");
        std::vector<KnownProject*> sorted;
        for (KnownProject& p : state.projects) sorted.push_back(&p);
        std::sort(sorted.begin(), sorted.end(),
                  [](const KnownProject* a, const KnownProject* b) {
                      return a->lastOpened > b->lastOpened;
                  });
        if (sorted.empty()) ImGui::TextDisabled("No projects yet — create or import one above.");
        std::string toRemove;
        if (!sorted.empty() &&
            ImGui::BeginTable("projects", 5,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch, 0.18f);
            ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableSetupColumn("Last opened", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.34f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableHeadersRow();
            for (KnownProject* p : sorted) {
                ImGui::PushID(p->path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p->name.c_str());
                ImGui::TableNextColumn();
                // Base-engine picker: change which installed engine version this
                // project is pinned to (writes only the manifest's engineVersion).
                bool exact = false;
                state.engineFor(p->engineVersion, &exact);
                ImGui::SetNextItemWidth(-FLT_MIN);
                if (ImGui::BeginCombo("##engine", p->engineVersion.c_str())) {
                    for (const EngineInstall& e : state.engines) {
                        bool sel = e.version == p->engineVersion;
                        if (ImGui::Selectable(e.version.c_str(), sel) && !sel)
                            repinProject(*p, e.version);
                        if (sel) ImGui::SetItemDefaultFocus();
                    }
                    ImGui::EndCombo();
                }
                if (!exact && ImGui::IsItemHovered())
                    ImGui::SetTooltip("Pinned engine %s is not installed;\n"
                                      "the newest install is used until you re-pin.",
                                      p->engineVersion.c_str());
                if (const EngineInstall* up = state.newestCompatibleEngine(p->engineVersion)) {
                    ImGui::SameLine(0, 4);
                    ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "\xE2\x86\x91");
                    if (ImGui::IsItemHovered())
                        ImGui::SetTooltip("A newer compatible engine (%s) is installed.\n"
                                          "Open to switch, or pick it above.",
                                          up->version.c_str());
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(whenText(p->lastOpened).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", p->path.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Open")) pendingOpen = p->path;
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) toRemove = p->path;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        // Resolve a clicked Open: if a newer COMPATIBLE engine is installed, ask
        // whether to switch the project to it; otherwise open straight away.
        if (!pendingOpen.empty()) {
            KnownProject* p = findProject(pendingOpen);
            pendingOpen.clear();
            if (p) {
                const EngineInstall* up = state.newestCompatibleEngine(p->engineVersion);
                if (up) {
                    switchProjPath = p->path;
                    switchToVersion = up->version;
                    ImGui::OpenPopup("Switch base engine?");
                } else {
                    openProject(*p);
                }
            }
        }
        drawSwitchModal();
        if (!toRemove.empty()) {
            state.projects.erase(std::remove_if(state.projects.begin(), state.projects.end(),
                                                [&](const KnownProject& p) {
                                                    return p.path == toRemove;
                                                }),
                                 state.projects.end());
            state.save();
        }
    }

    void drawUpdatesSection() {
        ImGui::SeparatorText("Updates");
        switch (updater.phase()) {
        case EngineUpdater::Phase::Idle:
        case EngineUpdater::Phase::CheckFailed:
            if (ImGui::Button("Check for updates"))
                updater.checkForUpdate(newestInstalledVersion());
            if (updater.phase() == EngineUpdater::Phase::CheckFailed) {
                ImGui::SameLine();
                ImGui::TextDisabled("couldn't reach GitHub — offline or rate-limited");
            }
            break;
        case EngineUpdater::Phase::Checking:
            ImGui::TextDisabled("Checking for updates...");
            break;
        case EngineUpdater::Phase::UpToDate:
            ImGui::Text("Engine is up to date (%s).", newestInstalledVersion().c_str());
            ImGui::SameLine();
            if (ImGui::SmallButton("Check again"))
                updater.checkForUpdate(newestInstalledVersion());
            break;
        case EngineUpdater::Phase::UpdateAvailable: {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Aether %s is available.",
                               updater.latestVersion().c_str());
            if (ImGui::Button("Download and install")) updater.install();
            std::string notes = updater.releaseNotesUrl();
            if (!notes.empty()) {
                ImGui::SameLine();
                if (ImGui::Button("Release notes"))
                    ShellExecuteA(nullptr, "open", notes.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            }
            ImGui::TextDisabled("Installs side by side — existing installs and projects are"
                                " untouched.");
            break;
        }
        case EngineUpdater::Phase::Downloading: {
            double doneMb = (double)updater.downloadedBytes() / (1024.0 * 1024.0);
            long long total = updater.totalBytes();
            float frac = total > 0 ? (float)((double)updater.downloadedBytes() / (double)total)
                                   : -1.0f * (float)ImGui::GetTime(); // indeterminate
            char overlay[96];
            if (total > 0)
                std::snprintf(overlay, sizeof(overlay), "%.1f / %.1f MB", doneMb,
                              (double)total / (1024.0 * 1024.0));
            else
                std::snprintf(overlay, sizeof(overlay), "%.1f MB", doneMb);
            ImGui::Text("Downloading Aether %s", updater.latestVersion().c_str());
            ImGui::ProgressBar(frac, ImVec2(360, 0), overlay);
            ImGui::SameLine();
            if (ImGui::SmallButton("Cancel")) updater.cancel();
            break;
        }
        case EngineUpdater::Phase::Extracting:
            ImGui::Text("Installing Aether %s", updater.latestVersion().c_str());
            ImGui::ProgressBar(-1.0f * (float)ImGui::GetTime(), ImVec2(360, 0), "extracting");
            break;
        case EngineUpdater::Phase::Installed: {
            ImGui::TextColored(ImVec4(0.55f, 0.85f, 0.55f, 1.0f), "Aether %s installed.",
                               updater.latestVersion().c_str());
            std::string newHub = joinPath(updater.installedPath(), "AetherHub.exe");
            if (pathExists(newHub)) {
                ImGui::SameLine();
                if (ImGui::Button("Restart Hub in new version")) {
                    if (launchDetached("\"" + newHub + "\"", updater.installedPath()))
                        quit = true;
                    else
                        lastError = "Failed to launch " + newHub;
                }
            }
            break;
        }
        case EngineUpdater::Phase::Failed:
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "Update failed: %s",
                               updater.error().c_str());
            if (ImGui::Button("Retry")) updater.install();
            ImGui::SameLine();
            if (ImGui::SmallButton("Check again"))
                updater.checkForUpdate(newestInstalledVersion());
            break;
        }
    }

    void drawEnginesTab(Window& window) {
        drawUpdatesSection();
        ImGui::SeparatorText("Engine installs");
        if (ImGui::BeginTable("engines", 3,
                              ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerH |
                                  ImGuiTableFlags_SizingStretchProp)) {
            ImGui::TableSetupColumn("Version", ImGuiTableColumnFlags_WidthStretch, 0.12f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.72f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableHeadersRow();
            std::string toRemove;
            for (const EngineInstall& e : state.engines) {
                ImGui::PushID(e.path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(e.version.c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", e.path.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Remove")) toRemove = e.path;
                ImGui::PopID();
            }
            ImGui::EndTable();
            if (!toRemove.empty()) {
                state.engines.erase(std::remove_if(state.engines.begin(), state.engines.end(),
                                                   [&](const EngineInstall& e) {
                                                       return e.path == toRemove;
                                                   }),
                                    state.engines.end());
                state.save();
            }
        }
        if (ImGui::Button("Add engine install...")) {
            std::string file = pickFileDialog(
                window, "Engine manifest (engine.json)\0engine.json\0\0", "Locate engine.json");
            if (!file.empty()) {
                std::string dir = parentPath(file);
                std::string version = readEngineVersion(dir);
                if (version.empty()) {
                    lastError = "Not a valid engine install: " + dir;
                } else {
                    state.registerEngine(dir, version);
                    state.save();
                    lastError.clear();
                }
            }
        }
    }

    void draw(Window& window) {
        // A finished install registers exactly once, even if the user never
        // opens the Engines tab.
        if (updater.takeInstalledEvent()) {
            state.registerEngine(updater.installedPath(), updater.latestVersion());
            state.save();
        }
        const ImGuiViewport* vp = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(vp->WorkPos);
        ImGui::SetNextWindowSize(vp->WorkSize);
        ImGui::Begin("##hub", nullptr,
                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                         ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoSavedSettings);
        ImGui::PushFont(imguiBoldFont());
        ImGui::TextColored(ImVec4(0.62f, 0.52f, 1.0f, 1.0f), "\xE2\x97\x86 Aether Hub");
        ImGui::PopFont();
        ImGui::SameLine();
        ImGui::TextDisabled("engine %s", selfVersion.c_str());
        if (updater.phase() == EngineUpdater::Phase::UpdateAvailable) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f),
                               "— update %s available (Engines tab)",
                               updater.latestVersion().c_str());
        }
        if (ImGui::BeginTabBar("tabs")) {
            if (ImGui::BeginTabItem("Projects")) {
                drawProjectsTab(window);
                ImGui::EndTabItem();
            }
            if (ImGui::BeginTabItem("Engines")) {
                drawEnginesTab(window);
                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
        if (!lastError.empty())
            ImGui::TextColored(ImVec4(1.0f, 0.45f, 0.4f, 1.0f), "%s", lastError.c_str());
        ImGui::End();
    }
};

} // namespace

int main() {
    Window window;
    if (!window.create("Aether Hub", 980, 640, WindowChrome::System)) {
        MessageBoxA(nullptr, "Failed to create window / GL context", "Aether Hub",
                    MB_OK | MB_ICONERROR);
        return 1;
    }
    window.setVSync(true);
    std::string iniPath =
        joinPath(parentPath(LauncherState::stateFilePath()), "hub_layout.ini");
    if (!imguiInit(window, iniPath)) return 1;

    HubUI hub;
    hub.state.load();
    hub.state.pruneDeadPaths();

    // Self-register the engine install that owns this hub exe.
    hub.selfVersion = readEngineVersion(engineBinDir());
    if (!hub.selfVersion.empty()) hub.state.registerEngine(engineBinDir(), hub.selfVersion);
    hub.state.save();

    // Fire the update check once per launch — async, the UI never blocks on it.
    hub.updater.checkForUpdate(hub.newestInstalledVersion());

    // Default project location + available templates.
    {
        char docs[MAX_PATH] = {};
        GetEnvironmentVariableA("USERPROFILE", docs, MAX_PATH);
        std::snprintf(hub.newLocation, sizeof(hub.newLocation), "%s\\Documents\\AetherProjects",
                      docs);
        std::string tdir = engineTemplatesDir();
        if (!tdir.empty()) {
            WIN32_FIND_DATAA fd;
            HANDLE h = FindFirstFileA((tdir + "\\*").c_str(), &fd);
            if (h != INVALID_HANDLE_VALUE) {
                do {
                    std::string n = fd.cFileName;
                    if (n == "." || n == "..") continue;
                    if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                        pathExists(tdir + "\\" + n + "\\project.aeproj"))
                        hub.templates.push_back(n);
                } while (FindNextFileA(h, &fd));
                FindClose(h);
            }
        }
    }

    while (!hub.quit && window.poll()) {
        rhi::setViewport(0, 0, window.width(), window.height());
        rhi::clear(true, 0.045f, 0.045f, 0.055f, 1.0f, false);
        imguiBeginFrame();
        hub.draw(window);
        imguiEndFrame();
        window.swapBuffers();
    }
    imguiShutdown();
    window.destroy();
    return 0;
}
