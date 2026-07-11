// Aether Hub (AetherHub.exe) — the launcher: lists engine installs and every
// known project, creates projects from the engine's Templates/, and opens a
// project in the matching engine version's editor. Epic-launcher-style, but
// small: one ImGui window over launcher.json (%APPDATA%/AetherEngine).
#include "launcher_state.h"
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
#include <algorithm>
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
    // New Project form
    char newName[128] = "MyGame";
    char newLocation[260] = {};
    std::vector<std::string> templates; // template dir names
    int templateIndex = 0;
    std::string lastError;

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
            ImGui::TableSetupColumn("Engine", ImGuiTableColumnFlags_WidthStretch, 0.10f);
            ImGui::TableSetupColumn("Last opened", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableSetupColumn("Path", ImGuiTableColumnFlags_WidthStretch, 0.40f);
            ImGui::TableSetupColumn("##actions", ImGuiTableColumnFlags_WidthStretch, 0.16f);
            ImGui::TableHeadersRow();
            for (KnownProject* p : sorted) {
                ImGui::PushID(p->path.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(p->name.c_str());
                ImGui::TableNextColumn();
                bool exact = false;
                state.engineFor(p->engineVersion, &exact);
                ImGui::TextUnformatted(p->engineVersion.c_str());
                if (!exact && ImGui::IsItemHovered())
                    ImGui::SetTooltip("This engine version is not installed;\n"
                                      "the newest install will be used.");
                if (!exact) {
                    ImGui::SameLine();
                    ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "(!)");
                }
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(whenText(p->lastOpened).c_str());
                ImGui::TableNextColumn();
                ImGui::TextDisabled("%s", p->path.c_str());
                ImGui::TableNextColumn();
                if (ImGui::SmallButton("Open")) openProject(*p);
                ImGui::SameLine();
                if (ImGui::SmallButton("Remove")) toRemove = p->path;
                ImGui::PopID();
            }
            ImGui::EndTable();
        }
        if (!toRemove.empty()) {
            state.projects.erase(std::remove_if(state.projects.begin(), state.projects.end(),
                                                [&](const KnownProject& p) {
                                                    return p.path == toRemove;
                                                }),
                                 state.projects.end());
            state.save();
        }
    }

    void drawEnginesTab(Window& window) {
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

    while (window.poll()) {
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
