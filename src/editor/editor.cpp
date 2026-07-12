#include "editor.h"
#include "imgui_layer.h"
#include "inspector_visitor.h"
#include "../engine/components.h"
#include "../engine/behaviors.h"
#include "../engine/camera_rigs.h"
#include "../engine/scene_io.h"
#include "../engine/component_registry.h"
#include "../engine/module_build.h"
#include "../engine/engine_modules.h"
#include "../engine/doc_gen.h"
#include "../rhi/rhi.h"
#include "../engine/packager.h"
#include "../core/log.h"
#include "../audio/audio.h"
#include "../narrative/dialogue_trigger.h"
#include "../narrative/mission_hud.h"
#include "../ui/ui_document_component.h"
#include "../render/debug_draw.h"
#include "../engine/save_game.h"
#include "../physics/physics_components.h"
#include "imgui_internal.h" // DockBuilder
#include "ImGuizmo.h"
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <commdlg.h>
#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>

namespace ae {

// ---------------------------------------------------------------------------
// helpers
// ---------------------------------------------------------------------------
namespace {

struct DirEntry {
    std::string name;
    bool isDir;
};

std::vector<DirEntry> listDir(const std::string& dir) {
    std::vector<DirEntry> out;
    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        std::string n = fd.cFileName;
        if (n == "." || n == "..") continue;
        out.push_back({n, (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0});
    } while (FindNextFileA(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const DirEntry& a, const DirEntry& b) {
        if (a.isDir != b.isDir) return a.isDir > b.isDir;
        return _stricmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::string parentDir(const std::string& d) {
    size_t s = d.find_last_of("\\/");
    if (s == std::string::npos || s < 2) return d;
    return d.substr(0, s);
}

std::string toLower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

bool endsWith(const std::string& s, const char* suffix) {
    std::string l = toLower(s), suf = suffix;
    return l.size() >= suf.size() && l.compare(l.size() - suf.size(), suf.size(), suf) == 0;
}

enum class AssetKind { Folder, Model, Map, Dialogue, Missions, Prefab, Image, Shader, Audio, Script, MaterialG, UIDoc, Anim, Data, Other };

AssetKind classify(const std::string& dir, const DirEntry& e) {
    if (e.isDir) return AssetKind::Folder;
    std::string d = toLower(dir);
    if (endsWith(e.name, ".glb") || endsWith(e.name, ".gltf") || endsWith(e.name, ".obj") ||
        endsWith(e.name, ".fbx")) return AssetKind::Model;
    if (endsWith(e.name, ".json")) {
        if (d.find("maps") != std::string::npos) return AssetKind::Map;
        if (d.find("dialogue") != std::string::npos) return AssetKind::Dialogue;
        if (d.find("missions") != std::string::npos) return AssetKind::Missions;
        if (d.find("entities") != std::string::npos) return AssetKind::Prefab;
        if (d.find("scripts") != std::string::npos) return AssetKind::Script;
        if (d.find("materials") != std::string::npos) return AssetKind::MaterialG;
        if (d.find("anim") != std::string::npos) return AssetKind::Anim;
        if (d.find("data") != std::string::npos) return AssetKind::Data;
        if (d.find("ui") != std::string::npos && d.find("audio") == std::string::npos) return AssetKind::UIDoc;
        return AssetKind::Other;
    }
    if (endsWith(e.name, ".png") || endsWith(e.name, ".jpg") || endsWith(e.name, ".jpeg") ||
        endsWith(e.name, ".bmp"))
        return AssetKind::Image;
    if (endsWith(e.name, ".frag") || endsWith(e.name, ".vert") || endsWith(e.name, ".glsl"))
        return AssetKind::Shader;
    if (endsWith(e.name, ".wav")) return AssetKind::Audio;
    return AssetKind::Other;
}

// Decompose a world matrix into an entity's local TRS (relative to its parent).
void setLocalFromWorld(Entity* e, const Mat4& world) {
    Mat4 local = e->parent() ? inverse(e->parent()->worldMatrix()) * world : world;
    Vec3 cx(local.m[0][0], local.m[0][1], local.m[0][2]);
    Vec3 cy(local.m[1][0], local.m[1][1], local.m[1][2]);
    Vec3 cz(local.m[2][0], local.m[2][1], local.m[2][2]);
    Vec3 s(length(cx), length(cy), length(cz));
    e->transform.position = Vec3(local.m[3][0], local.m[3][1], local.m[3][2]);
    e->transform.scaling = s;
    e->transform.rotation = quatFromBasis(s.x > 1e-8f ? cx / s.x : Vec3(1, 0, 0),
                                          s.y > 1e-8f ? cy / s.y : Vec3(0, 1, 0),
                                          s.z > 1e-8f ? cz / s.z : Vec3(0, 0, 1));
}

// Ray/AABB slab test in the box's local space. Returns tNear >= 0 or -1.
float rayAabb(const Vec3& o, const Vec3& d, const Vec3& bmin, const Vec3& bmax) {
    float tmin = -1e30f, tmax = 1e30f;
    const float* op = &o.x;
    const float* dp = &d.x;
    const float* mn = &bmin.x;
    const float* mx = &bmax.x;
    for (int i = 0; i < 3; ++i) {
        if (std::fabs(dp[i]) < 1e-9f) {
            if (op[i] < mn[i] || op[i] > mx[i]) return -1.0f;
            continue;
        }
        float t0 = (mn[i] - op[i]) / dp[i];
        float t1 = (mx[i] - op[i]) / dp[i];
        if (t0 > t1) std::swap(t0, t1);
        tmin = std::max(tmin, t0);
        tmax = std::min(tmax, t1);
        if (tmin > tmax) return -1.0f;
    }
    if (tmax < 0.0f) return -1.0f;
    return tmin >= 0.0f ? tmin : tmax;
}

const char* componentDisplayName(Component* c) {
    const char* type = c->typeName();
    if (type && *type)
        if (const ComponentDesc* d = componentRegistry().find(type))
            return d->displayName.c_str();
    return "Component";
}

} // namespace

// ---------------------------------------------------------------------------
// lifecycle
// ---------------------------------------------------------------------------
bool Editor::init(Window* window, Renderer* renderer, World* world, AssetLibrary* assets,
                  Font* font, const Project& project, GameModule* gameModule,
                  PluginManager* plugins) {
    window_ = window;
    renderer_ = renderer;
    world_ = world;
    assets_ = assets;
    font_ = font;
    gameModule_ = gameModule;
    plugins_ = plugins;
    project_ = project;
    projectRoot_ = project.root;
    contentDir_ = projectRoot_ + "\\assets";

    char tmp[MAX_PATH];
    GetTempPathA(MAX_PATH, tmp);
    pieSnapshotPath_ = std::string(tmp) + "aether_pie_snapshot.json";

    // Dock layout persists with the project, not the working directory.
    if (!imguiInit(*window, projectRoot_ + "\\editor_layout.ini")) return false;
    if (!gameUI_.init(font)) return false;
    audioEngine().init(); // AudioSource components only sound during Play

    aiPanel_.init(projectRoot_);
    aiPanel_.onCompileScripts = [this]() { startCompileScripts(); };

    // Feed the borderless window our custom-title-bar drag regions.
    window->setCaptionHitTest(&Editor::captionHitThunk, this);

    // Weak-GPU downtier (fewer cascades + reduced render scale). Combined with
    // viewport-render caching (idle frames reuse the last texture), this keeps
    // the editor usable on an iGPU. All tunable in World Settings > Performance.
    applyGpuAutoTier(renderer_->settings);

    // Open the default dialogue file in the graph editor (without stealing
    // the Viewport's tab focus) so the flowchart is one click away.
    std::string defaultDialogue = projectRoot_ + "\\assets\\dialogue\\interrogation.json";
    if (GetFileAttributesA(defaultDialogue.c_str()) != INVALID_FILE_ATTRIBUTES)
        dialogueGraph_.open(defaultDialogue, false);
    // Same courtesy for visual scripting: preload the project's first script
    // graph into its (unfocused) tab.
    for (const DirEntry& de : listDir(projectRoot_ + "\\assets\\scripts")) {
        if (!de.isDir && endsWith(de.name, ".json")) {
            scriptGraph_.open(projectRoot_ + "\\assets\\scripts\\" + de.name, false);
            break;
        }
    }

    // Agent bridge: PulseLABS-gated localhost control server. The token is
    // the shared secret; generate it once so PulseLABS tooling can read it
    // from pulse.json.
    bridgeConfig_ = PulseConfig::load();
    if (bridgeConfig_.bridgeToken.empty()) {
        bridgeConfig_.bridgeToken = Guid::generate().toString();
        bridgeConfig_.save();
    }
    if (bridgeConfig_.bridgeEnabled) startAgentBridge();

    AE_LOG("[Editor] ready (project: %s)", projectRoot_.c_str());
    return true;
}

void Editor::shutdown() {
    stopAgentBridge();
    if (compileThread_.joinable()) compileThread_.join();
    if (packageThread_.joinable()) packageThread_.join();
    audioEngine().shutdown();
    gameUI_.destroy();
    rhi::destroyTexture(vpTex_);
    rhi::destroyFramebuffer(vpFBO_);
    imguiShutdown();
}

// ---------------------------------------------------------------------------
// simulation / play-in-editor
// ---------------------------------------------------------------------------
Camera Editor::editorCamera() const {
    float cy = std::cos(radians(camYaw_)), sy = std::sin(radians(camYaw_));
    float cp = std::cos(radians(camPitch_)), sp = std::sin(radians(camPitch_));
    Camera cam;
    cam.position = camPos_;
    cam.forwardDir = normalize(Vec3(cy * cp, sp, sy * cp));
    cam.upDir = Vec3(0, 1, 0);
    cam.fovY = 55.0f;
    return cam;
}

void Editor::updateFreecam(float dt) {
    const Input& in = window_->input();
    if (viewportHovered_ && in.wheelDelta != 0.0f && !rmbLook_)
        camSpeed_ = clampf(camSpeed_ * (1.0f + in.wheelDelta * 0.12f), 0.5f, 80.0f);
    if (!rmbLook_) return;

    camYaw_ += in.mouseDX * 0.15f;
    camPitch_ = clampf(camPitch_ - in.mouseDY * 0.15f, -89.0f, 89.0f);
    if (in.wheelDelta != 0.0f)
        camSpeed_ = clampf(camSpeed_ * (1.0f + in.wheelDelta * 0.12f), 0.5f, 80.0f);

    float sp = camSpeed_ * dt * (in.keys[VK_SHIFT] ? 3.0f : 1.0f);
    Camera c = editorCamera();
    Vec3 fwd = c.forward(), right = c.right();
    if (in.keys['W']) camPos_ += fwd * sp;
    if (in.keys['S']) camPos_ += fwd * -sp;
    if (in.keys['D']) camPos_ += right * sp;
    if (in.keys['A']) camPos_ += right * -sp;
    if (in.keys['E']) camPos_ += Vec3(0, 1, 0) * sp;
    if (in.keys['Q']) camPos_ += Vec3(0, -1, 0) * sp;
}

void Editor::startPlay() {
    if (playing_) return;
    if (!saveWorld(*world_, *assets_, pieSnapshotPath_))
        AE_WARN("[Editor] PIE snapshot failed — Stop won't restore edits exactly");
    world_->missions.resetRuntime();
    world_->requestCamera("");
    for (const auto& e : world_->entities())
        if (auto* trig = e->getComponent<DialogueTriggerComponent>()) trig->reset();
    playing_ = true;
    paused_ = false;
    playClock_ = 0.0f;
    AE_LOG("[Editor] ► Play");
}

void Editor::stopPlay() {
    if (!playing_) return;
    playing_ = false;
    paused_ = false;
    world_->setActiveDialogue(nullptr);
    world_->requestCamera("");
    world_->missions.resetRuntime();

    // Real PIE stop: restore the exact pre-play world from the snapshot.
    std::string selName = selected() ? selected()->name() : "";
    if (loadWorld(*world_, *assets_, pieSnapshotPath_)) {
        primed_ = false;
        select(selName.empty() ? nullptr : world_->find(selName));
    } else {
        AE_WARN("[Editor] PIE restore failed — world keeps its played state");
    }
    AE_LOG("[Editor] ■ Stop");
}

// ---------------------------------------------------------------------------
// undo / redo — whole-world snapshots through the scene serializer
// ---------------------------------------------------------------------------
void Editor::recordUndo() {
    if (playing_) return;
    UndoState s;
    s.json = serializeWorld(*world_, *assets_);
    s.sel = selected() ? selected()->guid() : Guid{};
    undoStack_.push_back(std::move(s));
    if (undoStack_.size() > 128) undoStack_.erase(undoStack_.begin());
    redoStack_.clear();
}

void Editor::applySnapshot(const std::string& json, const Guid& sel) {
    deserializeWorld(*world_, *assets_, json);
    Entity* e = sel.valid() ? world_->findByGuid(sel) : nullptr;
    selectedId_ = e ? e->id() : 0;
    primed_ = false; // world replaced; re-prime the edit-mode tick
    markViewportDirty();
}

void Editor::undo() {
    if (playing_ || undoStack_.empty()) return;
    UndoState cur;
    cur.json = serializeWorld(*world_, *assets_);
    cur.sel = selected() ? selected()->guid() : Guid{};
    redoStack_.push_back(std::move(cur));
    UndoState s = std::move(undoStack_.back());
    undoStack_.pop_back();
    applySnapshot(s.json, s.sel);
    AE_LOG("[Editor] undo (%d left)", (int)undoStack_.size());
}

void Editor::redo() {
    if (playing_ || redoStack_.empty()) return;
    UndoState cur;
    cur.json = serializeWorld(*world_, *assets_);
    cur.sel = selected() ? selected()->guid() : Guid{};
    undoStack_.push_back(std::move(cur));
    UndoState s = std::move(redoStack_.back());
    redoStack_.pop_back();
    applySnapshot(s.json, s.sel);
    AE_LOG("[Editor] redo (%d left)", (int)redoStack_.size());
}

// Captures continuous edits (gizmo drag, inspector slider) as a single undo
// step: snapshot the world when a gesture begins, commit it on release only if
// something actually changed. Called once per frame in edit mode.
void Editor::updateInteractionUndo() {
    if (playing_) { interacting_ = false; return; }
    bool now = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();
    if (now && !interacting_) {
        pendingSnapshot_ = serializeWorld(*world_, *assets_);
        pendingSel_ = selected() ? selected()->guid() : Guid{};
    } else if (!now && interacting_) {
        if (!pendingSnapshot_.empty() && serializeWorld(*world_, *assets_) != pendingSnapshot_) {
            undoStack_.push_back({pendingSnapshot_, pendingSel_});
            if (undoStack_.size() > 128) undoStack_.erase(undoStack_.begin());
            redoStack_.clear();
        }
        pendingSnapshot_.clear();
    }
    interacting_ = now;
}

// ---------------------------------------------------------------------------
// build & package
// ---------------------------------------------------------------------------
void Editor::drawBuildDialog() {
    if (buildDialogRequested_) {
        buildDialogRequested_ = false;
        // Prefill: game name, output folder, and the map list.
        if (!buildGameName_[0])
            std::snprintf(buildGameName_, sizeof(buildGameName_), "%s", project_.name.c_str());
        if (!buildOutputDir_[0])
            std::snprintf(buildOutputDir_, sizeof(buildOutputDir_), "%s\\Build",
                          projectRoot_.c_str());
        buildMaps_.clear();
        buildMapIndex_ = 0;
        for (const DirEntry& f : listDir(projectRoot_ + "\\assets\\maps")) {
            if (f.isDir || !endsWith(f.name, ".json")) continue;
            std::string rel = "assets/maps/" + f.name;
            if (rel == project_.startupScene) buildMapIndex_ = (int)buildMaps_.size();
            buildMaps_.push_back(rel);
        }
        ImGui::OpenPopup("Build Game");
    }

    ImVec2 center = ImGui::GetMainViewport()->GetCenter();
    ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (!ImGui::BeginPopupModal("Build Game", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) return;

    ImGui::InputText("Game name", buildGameName_, sizeof(buildGameName_));
    ImGui::InputText("Output folder", buildOutputDir_, sizeof(buildOutputDir_));
    if (buildMaps_.empty()) {
        ImGui::TextColored(ImVec4(1, 0.5f, 0.4f, 1), "No maps in assets/maps — save one first.");
    } else if (ImGui::BeginCombo("Startup map", buildMaps_[buildMapIndex_].c_str())) {
        for (int i = 0; i < (int)buildMaps_.size(); ++i)
            if (ImGui::Selectable(buildMaps_[i].c_str(), i == buildMapIndex_)) buildMapIndex_ = i;
        ImGui::EndCombo();
    }
    ImGui::Checkbox("Development build", &buildDevelopment_);
    if (project_.hasModule()) ImGui::TextDisabled("Scripts are recompiled in Release.");
    ImGui::TextDisabled("Players need the Microsoft VC++ x64 redistributable.");
    ImGui::Separator();

    if (packaging_) {
        ImGui::Text("Packaging... progress in the Output Log.");
    } else {
        ImGui::BeginDisabled(buildMaps_.empty() || !buildGameName_[0] || !buildOutputDir_[0]);
        if (ImGui::Button("Build", ImVec2(120, 0))) startPackage();
        ImGui::EndDisabled();
        ImGui::SameLine();
        if (ImGui::Button("Close", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Editor::startPackage() {
    if (packaging_) return;
    if (packageThread_.joinable()) packageThread_.join();
    packaging_ = true;
    packageDone_ = false;
    packageOk_ = false;

    PackageOptions opts;
    opts.outputDir = buildOutputDir_;
    opts.gameName = buildGameName_;
    opts.startupScene = buildMaps_.empty() ? project_.startupScene : buildMaps_[buildMapIndex_];
    opts.development = buildDevelopment_;
    Project project = project_; // worker owns copies

    packageThread_ = std::thread([this, project, opts]() {
        bool ok = packageGame(project, opts, [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(compileLogMutex_);
            compileLogPending_.push_back(line);
        });
        packageOk_ = ok;
        packageDone_ = true;
    });
}

// ---------------------------------------------------------------------------
// game module: compile + hot reload
// ---------------------------------------------------------------------------
void Editor::startCompileScripts() {
    if (compiling_ || !project_.hasModule()) return;
    if (compileThread_.joinable()) compileThread_.join();
    compiling_ = true;
    compileDone_ = false;
    compileOk_ = false;
    AE_LOG("[Module] compiling scripts (%s)...", project_.sourceDir.c_str());

    Project project = project_; // worker owns a copy
    compileThread_ = std::thread([this, project]() {
        bool ok = buildGameModule(project, "Release", [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(compileLogMutex_);
            compileLogPending_.push_back(line);
        });
        compileOk_ = ok;
        compileDone_ = true; // main thread finishes up in pollCompile()
    });
}

void Editor::pollCompile() {
    // Drain worker output into the (main-thread-only) engine log.
    {
        std::lock_guard<std::mutex> lock(compileLogMutex_);
        for (const std::string& line : compileLogPending_) {
            bool err = line.find("error") != std::string::npos ||
                       line.find("FAILED") != std::string::npos;
            if (err) AE_ERROR("%s", line.c_str());
            else AE_LOG("%s", line.c_str());
        }
        compileLogPending_.clear();
    }

    // Packaging completion (worker shares the pending-line queue above).
    if (packageDone_) {
        packageDone_ = false;
        packaging_ = false;
        if (packageThread_.joinable()) packageThread_.join();
        if (packageOk_) AE_LOG("[Package] build finished");
        else AE_ERROR("[Package] build FAILED — see output above");
    }

    if (pluginCompileDone_) {
        pluginCompileDone_ = false;
        pluginCompiling_ = false;
        if (pluginCompileThread_.joinable()) pluginCompileThread_.join();
        if (!pluginCompileOk_) {
            AE_ERROR("[Plugin] compile failed for %s (see output above)",
                     pluginCompileName_.c_str());
        } else if (plugins_) {
            for (PluginInfo& p : plugins_->plugins())
                if (p.name == pluginCompileName_ && p.enabled) reloadPlugin(p);
        }
    }

    if (!compileDone_) return;
    compileDone_ = false;
    compiling_ = false;
    if (compileThread_.joinable()) compileThread_.join();

    if (!compileOk_) {
        AE_ERROR("[Module] script compile failed — see output above");
        return;
    }
    reloadGameModule();
}

void Editor::reloadGameModule() {
    if (!gameModule_) return;

    // The world may hold components whose vtables live in the old DLL, so the
    // swap round-trips the scene through JSON: snapshot → clear → swap DLL →
    // restore. This is the same machinery PIE Stop uses.
    if (playing_) stopPlay();
    std::string selName = selected() ? selected()->name() : "";
    if (!saveWorld(*world_, *assets_, pieSnapshotPath_)) {
        AE_ERROR("[Module] snapshot failed — module reload aborted");
        return;
    }
    world_->clear(); // every DLL-owned component must die before FreeLibrary

    gameModule_->unload();
    bool loaded = gameModule_->load(project_);

    if (loadWorld(*world_, *assets_, pieSnapshotPath_)) {
        select(selName.empty() ? nullptr : world_->find(selName));
    } else {
        AE_ERROR("[Module] scene restore failed after reload");
    }
    markViewportDirty();
    if (loaded) AE_LOG("[Module] scripts reloaded");
}


// ---------------------------------------------------------------------------
// modules & plugins
// ---------------------------------------------------------------------------
// A plugin swap uses the exact same snapshot round-trip as a script reload:
// components whose vtables live in the plugin DLL must be destroyed before
// FreeLibrary, and the scene restore re-creates them from the fresh DLL.
void Editor::reloadPlugin(PluginInfo& p) {
    if (!plugins_) return;
    if (playing_) stopPlay();
    std::string selName = selected() ? selected()->name() : "";
    if (!saveWorld(*world_, *assets_, pieSnapshotPath_)) {
        AE_ERROR("[Plugin] snapshot failed - reload aborted");
        return;
    }
    world_->clear();
    plugins_->unload(p);
    bool loaded = plugins_->load(p, /*hotCopy=*/true);
    if (loadWorld(*world_, *assets_, pieSnapshotPath_))
        select(selName.empty() ? nullptr : world_->find(selName));
    else
        AE_ERROR("[Plugin] scene restore failed after reload");
    markViewportDirty();
    if (loaded) AE_LOG("[Plugin] %s reloaded", p.name.c_str());
}

void Editor::unloadPluginSafe(PluginInfo& p) {
    if (!plugins_ || !p.loaded) return;
    if (playing_) stopPlay();
    std::string selName = selected() ? selected()->name() : "";
    if (!saveWorld(*world_, *assets_, pieSnapshotPath_)) return;
    world_->clear();
    plugins_->unload(p);
    if (loadWorld(*world_, *assets_, pieSnapshotPath_)) // its components skip
        select(selName.empty() ? nullptr : world_->find(selName));
    markViewportDirty();
}

void Editor::startCompilePlugin(const PluginInfo& p) {
    if (pluginCompiling_ || compiling_) return;
    if (pluginCompileThread_.joinable()) pluginCompileThread_.join();
    pluginCompiling_ = true;
    pluginCompileDone_ = false;
    pluginCompileOk_ = false;
    pluginCompileName_ = p.name;
    AE_LOG("[Plugin] compiling %s...", p.name.c_str());

    std::string dir = p.dir, name = p.name, src = p.sourceDir;
    pluginCompileThread_ = std::thread([this, dir, name, src]() {
        bool ok = buildModuleAt(dir + "\\" + src, name, dir + "\\Binaries",
                                dir + "\\Intermediate\\PluginBuild", "Release",
                                [this](const std::string& line) {
                                    std::lock_guard<std::mutex> lock(compileLogMutex_);
                                    compileLogPending_.push_back(line);
                                });
        pluginCompileOk_ = ok;
        pluginCompileDone_ = true; // pollCompile finishes up + reloads
    });
}

void Editor::drawModulesPanel() {
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Modules & Plugins", &showModulesPanel_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Engine modules");
    ImGui::TextWrapped("Per-project feature set: a disabled module registers no components "
                       "and its systems stay off. Saved to project.aeproj; scene components "
                       "of disabled types are skipped on the next load.");
    ImGui::Spacing();
    for (const EngineModuleDesc& m : engineModules().all()) {
        bool on = m.enabled;
        ImGui::PushID(m.id.c_str());
        if (ImGui::Checkbox(m.name.c_str(), &on)) {
            engineModules().setEnabled(m.id, on, componentRegistry());
            bool found = false;
            for (auto& kv : project_.moduleFlags)
                if (kv.first == m.id) { kv.second = on; found = true; }
            if (!found) project_.moduleFlags.emplace_back(m.id, on);
            if (!project_.save(project_.file))
                AE_ERROR("[Modules] cannot write %s", project_.file.c_str());
        }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", m.description.c_str());
        ImGui::PopID();
    }

    ImGui::Spacing();
    ImGui::SeparatorText("Project plugins");
    if (!plugins_) {
        ImGui::TextDisabled("Plugin manager unavailable in this host.");
        ImGui::End();
        return;
    }
    ImGui::TextWrapped("Drop-in native modules: Plugins/<Name>/plugin.json + Source/*.cpp "
                       "(same SDK as game scripts). Compile builds the DLL; Reload swaps it "
                       "live (the scene round-trips through a snapshot).");
    if (ImGui::Button("Rescan")) plugins_->scan(project_);
    ImGui::Spacing();

    if (plugins_->plugins().empty())
        ImGui::TextDisabled("No plugins found. Create Plugins/MyPlugin/plugin.json to start.");
    for (PluginInfo& p : plugins_->plugins()) {
        ImGui::PushID(p.name.c_str());
        bool en = p.enabled;
        if (ImGui::Checkbox(p.name.c_str(), &en)) {
            p.enabled = en;
            PluginManager::saveManifest(p);
            if (en && !p.loaded) reloadPlugin(p);
            if (!en && p.loaded) unloadPluginSafe(p);
        }
        ImGui::SameLine(220);
        if (p.loaded) ImGui::TextColored(ImVec4(0.5f, 0.9f, 0.5f, 1), "loaded");
        else if (p.hasBinary()) ImGui::TextDisabled("compiled");
        else if (p.hasSource()) ImGui::TextDisabled("source only");
        else ImGui::TextDisabled("empty");
        ImGui::SameLine(320);
        if (ImGui::SmallButton("Reload") && p.enabled) reloadPlugin(p);
        ImGui::SameLine();
        ImGui::BeginDisabled(pluginCompiling_ || compiling_ || !p.hasSource());
        if (ImGui::SmallButton(pluginCompiling_ && pluginCompileName_ == p.name
                                   ? "Compiling..."
                                   : "Compile"))
            startCompilePlugin(p);
        ImGui::EndDisabled();
        if (!p.description.empty()) {
            ImGui::SameLine();
            ImGui::TextDisabled("- %s", p.description.c_str());
        }
        ImGui::PopID();
    }
    ImGui::End();
}

void Editor::simulate(float dt) {
    Input in = window_->input(); // copy: gate what the game sees
    ImGuiIO& io = ImGui::GetIO();
    if (io.WantTextInput) std::memset(in.keys, 0, sizeof(in.keys));
    if (!viewportHovered_ && !rmbLook_) {
        in.mouseButtons[0] = in.mouseButtons[1] = in.mouseButtons[2] = false;
        in.mouseDX = in.mouseDY = 0.0f;
        in.wheelDelta = 0.0f;
    }

    if (!primed_) {
        world_->update(1.0f / 60.0f, 0.0f, in, true);
        world_->missions.resetRuntime(); // the priming tick must not start missions in edit mode
        primed_ = true;
    }

    if (playing_ && !paused_) {
        playClock_ += dt;
        world_->update(dt, playClock_, in, true);
        processSaveRequests(*world_, *assets_);
    } else if (playing_) {
        world_->update(0.0f, playClock_, in, false); // paused: freeze, keep pose
    } else {
        updateFreecam(dt);
        world_->update(dt, 0.0f, in, false);
    }

    // Listener follows the resolved gameplay camera in Play; in edit mode no
    // AudioSource has started, so update() just keeps the mixer ticking.
    if (playing_) {
        const Camera& cam = world_->camera();
        audioEngine().setListener(cam.position, cam.forwardDir, cam.upDir);
    }
    audioEngine().update(paused_ ? 0.0f : dt);
}

// ---------------------------------------------------------------------------
// viewport rendering
// ---------------------------------------------------------------------------
void Editor::ensureViewportTarget(int w, int h) {
    if (w == vpW_ && h == vpH_ && vpFBO_.valid()) return;
    rhi::destroyTexture(vpTex_);
    rhi::destroyFramebuffer(vpFBO_);
    vpW_ = w;
    vpH_ = h;
    vpTex_ = rhi::createTexture2D(w, h, 1, rhi::TexFormat::RGBA8);
    rhi::SamplerDesc smp;
    smp.mipmaps = false;
    smp.repeat = false;
    rhi::setSampler(vpTex_, smp);
    vpFBO_ = rhi::createFramebuffer();
    rhi::attachColor(vpFBO_, 0, vpTex_);
    renderer_->resize(w, h);
}

void Editor::renderSceneToViewport(float dt) {
    // Render at renderScale of the panel resolution; the panel Image upscales.
    float s = clampf(renderer_->settings.renderScale, 0.25f, 1.0f);
    ensureViewportTarget(std::max(16, (int)(desiredVpW_ * s)),
                         std::max(16, (int)(desiredVpH_ * s)));

    world_->buildRenderScene(renderScene_);
    frameCamera_ = playing_ ? world_->camera() : editorCamera();

    renderer_->setOutputFramebuffer(vpFBO_);
    submitDebugShapes();
    renderer_->render(renderScene_, frameCamera_, playing_ ? playClock_ : 0.0f);
    renderer_->setOutputFramebuffer({});

    if (playing_) {
        // Game HUD composites into the viewport texture, exactly like the
        // standalone game draws over the backbuffer. Mouse is remapped into
        // viewport-local coordinates so hit-testing lines up 1:1.
        Input hudIn = window_->input();
        hudIn.mouseX -= vpImagePos_.x;
        hudIn.mouseY -= vpImagePos_.y;
        gameUI_.begin(vpW_, vpH_, hudIn);
        Rect area{0, 0, (float)vpW_, (float)vpH_};
        if (DialoguePlayer* active = world_->activeDialogue()) {
            active->update(gameUI_, paused_ ? 0.0f : dt, area);
            if (active->finished()) world_->setActiveDialogue(nullptr);
        }
        drawMissionHUD(gameUI_, world_->missions, area);
        for (const auto& e : world_->entities()) {
            if (!e->active()) continue;
            for (const auto& c : e->components())
                if (auto* doc = dynamic_cast<UIDocumentComponent*>(c.get()))
                    doc->draw(gameUI_, area);
        }
        gameUI_.end(vpFBO_);
    }
}

// ---------------------------------------------------------------------------
// per-frame
// ---------------------------------------------------------------------------
void Editor::frame(float dt, float) {
    frameMsHistory_[frameMsCursor_] = dt * 1000.0f;
    frameMsCursor_ = (frameMsCursor_ + 1) % 120;
    fps_ = fps_ * 0.95f + (dt > 1e-6f ? 1.0f / dt : 0.0f) * 0.05f;
    ++frameCount_;
    if (autoPlayRequested_ && !playing_ && frameCount_ > 5) {
        autoPlayRequested_ = false;
        startPlay();
    }

    pollCompile(); // stream script-build output; hot-reload the DLL when done
    pumpAgentBridge(); // PulseLABS requests, before simulate so edits tick this frame

    simulate(dt);
    if (viewportNeedsRender()) renderSceneToViewport(dt); // else reuse cached vpTex_

    rhi::bindFramebuffer({});
    rhi::setViewport(0, 0, window_->width(), window_->height());
    rhi::setScissor(false);
    rhi::clear(true, 0.045f, 0.045f, 0.055f, 1.0f, false);

    imguiBeginFrame();

    drawTitleBar();
    drawDockspace();
    drawViewport(dt);
    if (showOutliner_) drawOutliner();
    if (showPrefabs_) drawPrefabLibrary();
    if (showDetails_) drawDetails();
    if (showContent_) drawContentBrowser();
    if (showLog_) drawOutputLog();
    if (showWorldSettings_) drawWorldSettings();
    if (missionPanel_.visible)
        missionPanel_.draw(*world_, projectRoot_ + "\\assets\\missions\\missions.json", playing_);
    if (dialogueGraph_.visible) dialogueGraph_.draw(world_);
    if (scriptGraph_.visible) scriptGraph_.draw(world_, assets_);
    if (materialGraph_.visible) materialGraph_.draw(world_, assets_);
    if (uiDesigner_.visible) uiDesigner_.draw(world_, assets_);
    if (inputMap_.visible) inputMap_.draw(world_, assets_, projectRoot_);
    if (animGraph_.visible) animGraph_.draw(world_, assets_);
    if (dataTable_.visible) dataTable_.draw(assets_);
    if (showModulesPanel_) drawModulesPanel();
    if (aiPanel_.visible) aiPanel_.draw();
    if (showProfiler_) drawProfiler();
    drawBuildDialog();
    if (showImGuiDemo_) ImGui::ShowDemoWindow(&showImGuiDemo_);

    // ---- global shortcuts ----
    ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_S)) saveScene(false);
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_O)) openSceneDialog();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_D)) duplicateSelected();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Z)) undo();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiKey_Y) ||
            ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_Z))
            redo();
        if (ImGui::IsKeyChordPressed(ImGuiMod_Ctrl | ImGuiMod_Shift | ImGuiKey_B))
            startCompileScripts();
        if (ImGui::IsKeyPressed(ImGuiKey_Escape) && playing_) stopPlay();
        if (ImGui::IsKeyPressed(ImGuiKey_F5)) playing_ ? stopPlay() : startPlay();
        if (!rmbLook_) {
            if (ImGui::IsKeyPressed(ImGuiKey_Q)) gizmoOp_ = 0;
            if (ImGui::IsKeyPressed(ImGuiKey_W)) gizmoOp_ = ImGuizmo::TRANSLATE;
            if (ImGui::IsKeyPressed(ImGuiKey_E)) gizmoOp_ = ImGuizmo::ROTATE;
            if (ImGui::IsKeyPressed(ImGuiKey_R)) gizmoOp_ = ImGuizmo::SCALE;
            if (ImGui::IsKeyPressed(ImGuiKey_F)) focusSelected();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) deleteSelected();
    }

    // Commit an undo step for any gizmo/inspector gesture that just ended.
    updateInteractionUndo();

    // Remember, for next frame's cache test, whether the user was actively
    // manipulating anything (a drag/edit that likely changed the scene). Read
    // now, after all UI is submitted, so it reflects this frame's interactions.
    wasInteracting_ = ImGui::IsAnyItemActive() || ImGuizmo::IsUsing();

    imguiEndFrame();
}

// Re-render the 3D viewport only when something visible could have changed:
// during Play (things move), while the camera flies, on a viewport resize,
// while any widget/gizmo is being manipulated, or for a few frames after a
// discrete action (selection, spawn, delete, load...). Idle editing reuses the
// cached texture, so the editor costs ~one ImGui pass when nothing is happening.
bool Editor::viewportNeedsRender() {
    bool camMoved = length(camPos_ - lastRenderCamPos_) > 1e-4f ||
                    camYaw_ != lastRenderYaw_ || camPitch_ != lastRenderPitch_;
    float sc = clampf(renderer_->settings.renderScale, 0.25f, 1.0f);
    bool resized = (int)(desiredVpW_ * sc) != vpW_ || (int)(desiredVpH_ * sc) != vpH_;

    bool dirty = playing_ || camMoved || resized || rmbLook_ || wasInteracting_ ||
                 forceRenderFrames_ > 0;
    if (forceRenderFrames_ > 0) --forceRenderFrames_;
    if (dirty) {
        lastRenderCamPos_ = camPos_;
        lastRenderYaw_ = camYaw_;
        lastRenderPitch_ = camPitch_;
    }
    return dirty;
}

// ---------------------------------------------------------------------------
// custom title bar (borderless chrome) + menus
// ---------------------------------------------------------------------------
bool Editor::captionHitThunk(void* user, int x, int y) {
    return static_cast<Editor*>(user)->captionAt(x, y);
}
bool Editor::captionAt(int x, int y) const {
    return y >= 0 && y < (int)titleBarH_ && x >= (int)dragMinX_ && x <= (int)dragMaxX_;
}

void Editor::drawTitleBar() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const bool borderless = window_->borderless();

    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, titleBarH_));
    ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                             ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                             ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                             ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_MenuBar;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##TitleBar", nullptr, flags);
    ImGui::PopStyleVar(3);

    // Default the caption span to empty; widened to the gap between the menus
    // and the window buttons below.
    dragMinX_ = dragMaxX_ = 0.0f;

    if (ImGui::BeginMenuBar()) {
        ImGui::PushFont(imguiBoldFont());
        char brand[160];
        std::snprintf(brand, sizeof(brand), "  \xE2\x97\x86 Aether \xE2\x80\x94 %s  ",
                      project_.name.c_str());
        ImGui::TextColored(ImVec4(0.62f, 0.52f, 1.0f, 1.0f), "%s", brand);
        ImGui::PopFont();

        drawMenus();

        float menuEndX = ImGui::GetCursorPosX(); // window-local x where menus end

        // Center: current map + play state (non-interactive, so also draggable).
        char label[300];
        std::snprintf(label, sizeof(label), "%s%s",
                      scenePath_.empty()
                          ? "untitled map"
                          : scenePath_.substr(scenePath_.find_last_of("\\/") + 1).c_str(),
                      playing_ ? (paused_ ? "   [PAUSED]" : "   [PLAYING]") : "");

        const float btnW = 46.0f;
        const int nButtons = borderless ? 3 : 0;
        float buttonsStartX = vp->WorkSize.x - btnW * nButtons;

        // Map label, centered in the drag gap.
        float labelW = ImGui::CalcTextSize(label).x;
        float labelX = (menuEndX + buttonsStartX) * 0.5f - labelW * 0.5f;
        if (labelX > menuEndX + 8.0f) {
            ImGui::SameLine(labelX);
            ImGui::TextColored(playing_ ? ImVec4(0.35f, 0.9f, 0.5f, 1.0f)
                                        : ImVec4(0.5f, 0.53f, 0.6f, 1.0f),
                               "%s", label);
        }

        dragMinX_ = menuEndX;
        dragMaxX_ = buttonsStartX;

        // Window buttons (borderless only).
        if (borderless) {
            ImGui::SameLine(buttonsStartX);
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
            float bh = titleBarH_;
            if (ImGui::Button("\xE2\x80\x93##min", ImVec2(btnW, bh))) window_->minimize();
            ImGui::SameLine(0, 0);
            if (ImGui::Button(window_->isMaximized() ? "\xE2\x9D\x90##max" : "\xE2\x96\xA1##max",
                              ImVec2(btnW, bh)))
                window_->toggleMaximize();
            ImGui::SameLine(0, 0);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.79f, 0.24f, 0.26f, 1.0f));
            if (ImGui::Button("\xC3\x97##close", ImVec2(btnW, bh))) window_->close();
            ImGui::PopStyleColor(2);
            ImGui::PopStyleVar();
        }

        ImGui::EndMenuBar();
    }
    ImGui::End();
}

void Editor::drawMenus() {
    if (ImGui::BeginMenu("File")) {
        ImGui::BeginDisabled(playing_);
        if (ImGui::MenuItem("New Map")) newScene();
        if (ImGui::MenuItem("Open Map...", "Ctrl+O")) openSceneDialog();
        ImGui::Separator();
        if (ImGui::MenuItem("Save Map", "Ctrl+S")) saveScene(false);
        if (ImGui::MenuItem("Save Map As...")) saveScene(true);
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::BeginDisabled(playing_ || packaging_);
        if (ImGui::MenuItem("Build Game...")) buildDialogRequested_ = true;
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Exit", "Alt+F4")) window_->close();
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Edit")) {
        if (ImGui::MenuItem("Undo", "Ctrl+Z", false, canUndo())) undo();
        if (ImGui::MenuItem("Redo", "Ctrl+Y", false, canRedo())) redo();
        ImGui::Separator();
        ImGui::BeginDisabled(!selected());
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
        if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
        if (ImGui::MenuItem("Focus", "F")) focusSelected();
        ImGui::EndDisabled();
        ImGui::Separator();
        ImGui::MenuItem("Snap to grid", nullptr, &snapEnabled_);
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Move snap", &snapMove_, 0.05f, 0.01f, 10.0f, "%.2f");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Rotate snap", &snapRotate_, 0.5f, 1.0f, 90.0f, "%.0f deg");
        ImGui::SetNextItemWidth(120);
        ImGui::DragFloat("Scale snap", &snapScale_, 0.01f, 0.01f, 2.0f, "%.2f");
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("View")) {
        ImGui::MenuItem("Outliner", nullptr, &showOutliner_);
        ImGui::MenuItem("Details", nullptr, &showDetails_);
        ImGui::MenuItem("Content Browser", nullptr, &showContent_);
        ImGui::MenuItem("Output Log", nullptr, &showLog_);
        ImGui::MenuItem("World Settings", nullptr, &showWorldSettings_);
        ImGui::MenuItem("Prefabs", nullptr, &showPrefabs_);
        ImGui::MenuItem("Missions", nullptr, &missionPanel_.visible);
        ImGui::MenuItem("Dialogue Graph", nullptr, &dialogueGraph_.visible);
        ImGui::MenuItem("Script Graph", nullptr, &scriptGraph_.visible);
        ImGui::MenuItem("Material Graph", nullptr, &materialGraph_.visible);
        ImGui::MenuItem("UI Designer", nullptr, &uiDesigner_.visible);
        ImGui::MenuItem("Input Map", nullptr, &inputMap_.visible);
        ImGui::MenuItem("Anim Graph", nullptr, &animGraph_.visible);
        ImGui::MenuItem("Data Table", nullptr, &dataTable_.visible);
        ImGui::MenuItem("Modules & Plugins", nullptr, &showModulesPanel_);
        ImGui::MenuItem("AI Assistant", nullptr, &aiPanel_.visible);
        ImGui::MenuItem("Viewport stats", nullptr, &showStats_);
        ImGui::MenuItem("Profiler", nullptr, &showProfiler_);
        ImGui::Separator();
        ImGui::MenuItem("Colliders (selected)", nullptr, &showColliders_);
        ImGui::MenuItem("Colliders (all)", nullptr, &showAllColliders_);
        ImGui::MenuItem("Navmesh", nullptr, &showNavmesh_);
        ImGui::Separator();
        if (ImGui::MenuItem("Reset Layout")) resetLayoutRequested_ = true;
        ImGui::MenuItem("ImGui Demo", nullptr, &showImGuiDemo_);
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Tools")) {
        ImGui::BeginDisabled(!project_.hasModule() || compiling_);
        if (ImGui::MenuItem(compiling_ ? "Compiling Scripts..." : "Compile Scripts",
                            "Ctrl+Shift+B"))
            startCompileScripts();
        ImGui::EndDisabled();
        ImGui::Separator();
        if (ImGui::MenuItem("Dialogue Graph Editor")) {
            dialogueGraph_.visible = true;
            if (!dialogueGraph_.loaded())
                dialogueGraph_.open(projectRoot_ + "\\assets\\dialogue\\interrogation.json");
        }
        if (ImGui::MenuItem("Script Graph Editor")) {
            scriptGraph_.visible = true;
            if (!scriptGraph_.loaded()) {
                // Open the first existing script, or create a starter one.
                std::string dir = projectRoot_ + "\\assets\\scripts";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string first;
                for (const DirEntry& de : listDir(dir))
                    if (!de.isDir && endsWith(de.name, ".json")) { first = dir + "\\" + de.name; break; }
                if (first.empty()) {
                    first = dir + "\\new_script.json";
                    ScriptGraphPanel::createStarterGraph(first);
                }
                scriptGraph_.open(first);
            }
        }
        if (ImGui::MenuItem("Material Graph Editor")) {
            materialGraph_.visible = true;
            if (!materialGraph_.loaded()) {
                std::string dir = projectRoot_ + "/assets/materials";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string first;
                for (const DirEntry& de : listDir(dir))
                    if (!de.isDir && endsWith(de.name, ".json")) { first = dir + "\\" + de.name; break; }
                if (first.empty()) {
                    first = dir + "/new_material.json";
                    MaterialGraphPanel::createStarterGraph(first);
                }
                materialGraph_.open(first);
            }
        }
        if (ImGui::MenuItem("UI Designer")) {
            uiDesigner_.visible = true;
            if (!uiDesigner_.loaded()) {
                std::string dir = projectRoot_ + "/assets/ui";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string first;
                for (const DirEntry& de : listDir(dir))
                    if (!de.isDir && endsWith(de.name, ".json")) { first = dir + "/" + de.name; break; }
                if (first.empty()) {
                    first = dir + "/new_hud.json";
                    UIDesignerPanel::createStarterDoc(first);
                }
                uiDesigner_.open(first);
            }
        }
        if (ImGui::MenuItem("Anim Graph Editor")) {
            animGraph_.visible = true;
            if (!animGraph_.loaded()) {
                std::string dir = projectRoot_ + "/assets/anim";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string first;
                for (const DirEntry& de : listDir(dir))
                    if (!de.isDir && endsWith(de.name, ".json")) { first = dir + "/" + de.name; break; }
                if (first.empty()) {
                    first = dir + "/new_anim.json";
                    AnimGraphPanel::createStarterGraph(first);
                }
                animGraph_.open(first);
            }
        }
        if (ImGui::MenuItem("Bake Navmesh")) {
            if (world_->nav.bake(*world_)) showNavmesh_ = true;
        }
        if (ImGui::MenuItem("Modules & Plugins")) showModulesPanel_ = true;
        if (ImGui::MenuItem("AI Assistant")) aiPanel_.visible = true;
        {
            char bridgeLabel[64];
            std::snprintf(bridgeLabel, sizeof(bridgeLabel), "Agent Bridge (port %d)",
                          bridgeConfig_.bridgePort);
            if (ImGui::MenuItem(bridgeLabel, nullptr, bridge_.running())) {
                bridgeConfig_.bridgeEnabled = !bridge_.running();
                bridgeConfig_.save();
                bridgeConfig_.bridgeEnabled ? startAgentBridge() : stopAgentBridge();
            }
            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort))
                ImGui::SetTooltip("Lets PulseLABS drive this editor over localhost.\n"
                                  "Token-gated: %%APPDATA%%/AetherEngine/pulse.json");
        }
        if (ImGui::MenuItem("Regenerate Doc Reference"))
            generateReferenceDocs(projectRoot_ + "\\Docs");
        if (ImGui::MenuItem("Data Table Editor")) {
            dataTable_.visible = true;
            if (!dataTable_.loaded()) {
                std::string dir = projectRoot_ + "/assets/data";
                CreateDirectoryA(dir.c_str(), nullptr);
                std::string first;
                for (const DirEntry& de : listDir(dir))
                    if (!de.isDir && endsWith(de.name, ".json")) { first = dir + "/" + de.name; break; }
                if (first.empty()) {
                    first = dir + "/new_table.json";
                    DataTablePanel::createStarterTable(first);
                }
                dataTable_.open(first);
            }
        }
        if (ImGui::MenuItem("Input Map Editor")) inputMap_.visible = true;
        if (ImGui::MenuItem("Mission Editor")) missionPanel_.visible = true;
        ImGui::EndMenu();
    }
    if (ImGui::BeginMenu("Help")) {
        if (ImGui::BeginMenu("About")) {
            ImGui::Text("Aether Engine - narrative 'choice' game engine");
            ImGui::Text("OpenGL 4.5 DSA, from-scratch PBR renderer");
            ImGui::Text("Editor: Dear ImGui %s (docking) + ImGuizmo", IMGUI_VERSION);
            ImGui::EndMenu();
        }
        ImGui::EndMenu();
    }
}

// ---------------------------------------------------------------------------
// toolbar + dockspace + status bar
// ---------------------------------------------------------------------------
void Editor::drawToolbar() {
    auto toggle = [&](const char* label, bool active, const char* tip) {
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.36f, 0.30f, 0.62f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.44f, 0.37f, 0.74f, 1.0f));
        }
        bool clicked = ImGui::Button(label);
        if (active) ImGui::PopStyleColor(2);
        if (tip && ImGui::IsItemHovered(ImGuiHoveredFlags_DelayShort)) ImGui::SetTooltip("%s", tip);
        ImGui::SameLine();
        return clicked;
    };

    ImGui::BeginDisabled(playing_);
    if (toggle("Save", false, "Save map (Ctrl+S)")) saveScene(false);
    if (toggle("Open", false, "Open map (Ctrl+O)")) openSceneDialog();
    ImGui::EndDisabled();

    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    if (toggle("Select", gizmoOp_ == 0, "No gizmo (Q)")) gizmoOp_ = 0;
    if (toggle("Move", gizmoOp_ == ImGuizmo::TRANSLATE, "Translate (W)"))
        gizmoOp_ = ImGuizmo::TRANSLATE;
    if (toggle("Rotate", gizmoOp_ == ImGuizmo::ROTATE, "Rotate (E)")) gizmoOp_ = ImGuizmo::ROTATE;
    if (toggle("Scale", gizmoOp_ == ImGuizmo::SCALE, "Scale (R)")) gizmoOp_ = ImGuizmo::SCALE;
    if (toggle(gizmoWorld_ ? "World" : "Local", false, "Gizmo space")) gizmoWorld_ = !gizmoWorld_;
    if (toggle(snapEnabled_ ? "Snap: on" : "Snap: off", snapEnabled_, "Grid snapping"))
        snapEnabled_ = !snapEnabled_;

    // Play controls, centered.
    float center = ImGui::GetWindowWidth() * 0.5f - 70.0f;
    ImGui::SameLine(std::max(center, ImGui::GetCursorPosX() + 12.0f));
    ImGui::PushFont(imguiBoldFont());
    if (!playing_) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.13f, 0.42f, 0.22f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.17f, 0.55f, 0.29f, 1.0f));
        if (ImGui::Button("  > Play  ")) startPlay();
        ImGui::PopStyleColor(2);
    } else {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.42f, 0.35f, 0.13f, 1.0f));
        if (ImGui::Button(paused_ ? " Resume " : " Pause ")) paused_ = !paused_;
        ImGui::PopStyleColor();
        ImGui::SameLine();
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.52f, 0.16f, 0.18f, 1.0f));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.66f, 0.21f, 0.23f, 1.0f));
        if (ImGui::Button("  [] Stop  ")) stopPlay();
        ImGui::PopStyleColor(2);
    }
    ImGui::PopFont();

    // Right: freecam speed.
    ImGui::SameLine(std::max(ImGui::GetWindowWidth() - 240.0f, ImGui::GetCursorPosX() + 12.0f));
    ImGui::SetNextItemWidth(180);
    ImGui::SliderFloat("##camspeed", &camSpeed_, 0.5f, 80.0f, "cam speed %.1f",
                       ImGuiSliderFlags_Logarithmic);
}

void Editor::buildDefaultLayout(unsigned int dockspaceId) {
    ImGuiID id = dockspaceId;
    ImGui::DockBuilderRemoveNode(id);
    ImGui::DockBuilderAddNode(id, ImGuiDockNodeFlags_DockSpace);
    ImGui::DockBuilderSetNodeSize(id, ImGui::GetMainViewport()->WorkSize);

    ImGuiID center = id;
    ImGuiID left = ImGui::DockBuilderSplitNode(center, ImGuiDir_Left, 0.17f, nullptr, &center);
    ImGuiID right = ImGui::DockBuilderSplitNode(center, ImGuiDir_Right, 0.27f, nullptr, &center);
    ImGuiID bottom = ImGui::DockBuilderSplitNode(center, ImGuiDir_Down, 0.30f, nullptr, &center);
    ImGuiID rightBottom = ImGui::DockBuilderSplitNode(right, ImGuiDir_Down, 0.42f, nullptr, &right);

    // Prefabs first, Outliner last so the Outliner is the selected tab.
    ImGui::DockBuilderDockWindow("Prefabs", left);
    ImGui::DockBuilderDockWindow("Outliner", left);
    ImGui::DockBuilderDockWindow("Details", right);
    ImGui::DockBuilderDockWindow("World Settings", rightBottom);
    ImGui::DockBuilderDockWindow("Missions", rightBottom);
    ImGui::DockBuilderDockWindow("Content Browser", bottom);
    ImGui::DockBuilderDockWindow("Output Log", bottom);
    // Graph editors first, Viewport last: the last window docked into a node
    // becomes its selected tab.
    ImGui::DockBuilderDockWindow("Anim Graph", center);
    ImGui::DockBuilderDockWindow("UI Designer", center);
    ImGui::DockBuilderDockWindow("Material Graph", center);
    ImGui::DockBuilderDockWindow("Script Graph", center);
    ImGui::DockBuilderDockWindow("###DialogueGraph", center); // its ### ID, not the visible title
    ImGui::DockBuilderDockWindow("Viewport", center);
    ImGui::DockBuilderFinish(id);
}

void Editor::drawDockspace() {
    ImGuiViewport* vp = ImGui::GetMainViewport();
    const float toolbarH = 40.0f, statusH = 26.0f;
    const float top = titleBarH_; // the custom title bar occupies the top strip

    // Toolbar strip (just below the title bar).
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, toolbarH));
    ImGuiWindowFlags stripFlags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                  ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar |
                                  ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking |
                                  ImGuiWindowFlags_NoBringToFrontOnFocus;
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    if (ImGui::Begin("##Toolbar", nullptr, stripFlags)) drawToolbar();
    ImGui::End();

    // Dock host.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + top + toolbarH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, vp->WorkSize.y - top - toolbarH - statusH));
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::Begin("##DockHost", nullptr,
                 stripFlags | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground);
    ImGui::PopStyleVar(2);

    ImGuiID dockId = ImGui::GetID("CEDockSpace");
    if (ImGui::DockBuilderGetNode(dockId) == nullptr || resetLayoutRequested_) {
        resetLayoutRequested_ = false;
        buildDefaultLayout(dockId);
        focusViewportFrames_ = 2;
    }
    ImGui::DockSpace(dockId, ImVec2(0, 0));
    ImGui::End();

    // Status bar.
    ImGui::SetNextWindowPos(ImVec2(vp->WorkPos.x, vp->WorkPos.y + vp->WorkSize.y - statusH));
    ImGui::SetNextWindowSize(ImVec2(vp->WorkSize.x, statusH));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(10, 4));
    if (ImGui::Begin("##StatusBar", nullptr, stripFlags)) drawStatusBar(statusH);
    ImGui::End();
    ImGui::PopStyleVar(2); // WindowPadding + WindowRounding
}

void Editor::drawStatusBar(float) {
    const FrameStats& st = renderer_->stats;
    ImGui::Text("%s", scenePath_.empty() ? "untitled map" : scenePath_.c_str());
    ImGui::SameLine(0, 24);
    ImGui::TextDisabled("%d entities", (int)world_->entities().size());
    ImGui::SameLine(0, 24);
    ImGui::TextDisabled("draws %d  culled %d  shadow %d  lights %d", st.drawCalls, st.culled,
                        st.shadowDraws, st.lights);
    char fpsTxt[64];
    std::snprintf(fpsTxt, sizeof(fpsTxt), "%.0f fps  (%.2f ms)", fps_,
                  fps_ > 1.0f ? 1000.0f / fps_ : 0.0f);
    ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::CalcTextSize(fpsTxt).x - 16.0f);
    ImGui::TextDisabled("%s", fpsTxt);
}


// ---------------------------------------------------------------------------
// profiler + debug shapes
// ---------------------------------------------------------------------------
void Editor::drawProfiler() {
    ImGui::SetNextWindowSize(ImVec2(430, 430), ImGuiCond_FirstUseEver);
    if (!ImGui::Begin("Profiler", &showProfiler_)) {
        ImGui::End();
        return;
    }
    float worst = 0.0f;
    for (float v : frameMsHistory_) worst = std::max(worst, v);
    char overlay[48];
    std::snprintf(overlay, sizeof(overlay), "%.1f fps - worst %.2f ms", fps_, worst);
    ImGui::PlotLines("##frame", frameMsHistory_, 120, frameMsCursor_, overlay, 0.0f,
                     std::max(worst * 1.2f, 20.0f), ImVec2(-1, 70));

    ImGui::SeparatorText("GPU passes (ms)");
    const FrameStats& st = renderer_->stats;
    float total = 0.0f;
    for (int p = 0; p < FrameStats::PassCount; ++p) total += st.msPass[p];
    for (int p = 0; p < FrameStats::PassCount; ++p) {
        float ms = st.msPass[p];
        ImGui::Text("%-10s %6.3f", FrameStats::passName(p), ms);
        ImGui::SameLine(150);
        ImGui::ProgressBar(total > 0.0001f ? ms / total : 0.0f, ImVec2(-1, 12), "");
    }
    ImGui::Text("%-10s %6.3f", "total", total);

    ImGui::SeparatorText("Counters");
    ImGui::Text("entities   %d", (int)world_->entities().size());
    ImGui::Text("draws      %d   culled %d", st.drawCalls, st.culled);
    ImGui::Text("instanced  %d draws covering %d objects", st.instancedDraws,
                st.instancedObjects);
    ImGui::Text("shadows    %d draws", st.shadowDraws);
    ImGui::Text("lights     %d   particles %d", st.lights, st.particles);
    ImGui::Text("physics    %d bodies", world_->physics.bodyCount());
    ImGui::End();
}

void Editor::submitDebugShapes() {
    if (showNavmesh_ && world_->nav.valid()) {
        static std::vector<Vec3> navLines;
        navLines.clear();
        world_->nav.debugLines(navLines);
        const Vec3 navColor(0.25f, 0.6f, 1.0f);
        for (size_t i = 0; i + 1 < navLines.size(); i += 2)
            debugDraw().line(navLines[i], navLines[i + 1], navColor);
    }
    if (!showColliders_ && !showAllColliders_) return;
    Entity* sel = selected();
    const Vec3 colColor(0.3f, 1.0f, 0.4f);
    const Vec3 trgColor(1.0f, 0.8f, 0.2f);
    const Vec3 lightColor(1.0f, 0.9f, 0.3f);

    auto colScale = [](const Mat4& m) {
        Vec3 cx(m.m[0][0], m.m[0][1], m.m[0][2]);
        Vec3 cy(m.m[1][0], m.m[1][1], m.m[1][2]);
        Vec3 cz(m.m[2][0], m.m[2][1], m.m[2][2]);
        return Vec3(length(cx), length(cy), length(cz));
    };

    for (const auto& e : world_->entities()) {
        if (!e->active()) continue;
        bool wantThis = showAllColliders_ || (showColliders_ && e.get() == sel);
        if (!wantThis) continue;
        for (const auto& c : e->components()) {
            if (auto* col = dynamic_cast<ColliderComponent*>(c.get())) {
                Mat4 wm = e->worldMatrix();
                Vec3 scale = colScale(wm);
                Vec4 cw = wm * Vec4(col->center, 1.0f);
                Vec3 center(cw.x, cw.y, cw.z);
                Vec3 color = col->isTrigger ? trgColor : colColor;
                if (col->kind() == ColliderShape::Box) {
                    // Same sizing rules the physics gather uses.
                    Vec3 half = col->halfExtents * scale;
                    Mat4 rot = Mat4::identity();
                    for (int i = 0; i < 3; ++i) {
                        float s = i == 0 ? scale.x : (i == 1 ? scale.y : scale.z);
                        if (s < 1e-6f) s = 1.0f;
                        rot.m[i][0] = wm.m[i][0] / s;
                        rot.m[i][1] = wm.m[i][1] / s;
                        rot.m[i][2] = wm.m[i][2] / s;
                    }
                    rot.m[3][0] = center.x;
                    rot.m[3][1] = center.y;
                    rot.m[3][2] = center.z;
                    debugDraw().wireBox(rot, half, color);
                } else if (col->kind() == ColliderShape::Sphere) {
                    debugDraw().wireSphere(center, col->radius * std::max(scale.x, scale.z),
                                           color);
                } else {
                    float r = col->radius * std::max(scale.x, scale.z);
                    float hs = std::max(0.0f, (col->height * 0.5f - col->radius)) * scale.y;
                    debugDraw().wireCapsule(center, r, hs, color);
                }
            }
            if (e.get() == sel) {
                if (auto* l = dynamic_cast<LightComponent*>(c.get())) {
                    Vec3 pos = e->worldPosition();
                    if (l->type == 1) { // spot cone along local -Z
                        Mat4 wm = e->worldMatrix();
                        Vec3 dir = normalize(Vec3(-wm.m[2][0], -wm.m[2][1], -wm.m[2][2]));
                        Vec3 u = normalize(cross(dir, std::fabs(dir.y) < 0.95f
                                                          ? Vec3(0, 1, 0)
                                                          : Vec3(1, 0, 0)));
                        Vec3 v = cross(dir, u);
                        float r = std::tan(radians(l->outerAngle)) * l->range;
                        Vec3 end = pos + dir * l->range;
                        debugDraw().circle(end, u, v, r, lightColor);
                        for (int i = 0; i < 4; ++i) {
                            float a = i * 0.5f * 3.14159265f;
                            debugDraw().line(pos,
                                             end + u * (std::cos(a) * r) + v * (std::sin(a) * r),
                                             lightColor);
                        }
                    } else if (l->type == 0) {
                        debugDraw().wireSphere(pos, l->range, lightColor);
                    }
                }
            }
        }
    }
}

// ---------------------------------------------------------------------------
// viewport
// ---------------------------------------------------------------------------
void Editor::drawViewport(float) {
    if (focusViewportFrames_ > 0) {
        --focusViewportFrames_;
        ImGui::SetNextWindowFocus();
    }
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    bool open = ImGui::Begin("Viewport", nullptr,
                             ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse |
                                 ImGuiWindowFlags_NoCollapse);
    ImGui::PopStyleVar();
    if (!open) {
        viewportHovered_ = false;
        ImGui::End();
        return;
    }

    ImVec2 avail = ImGui::GetContentRegionAvail();
    avail.x = std::max(avail.x, 16.0f);
    avail.y = std::max(avail.y, 16.0f);
    desiredVpW_ = (int)avail.x;
    desiredVpH_ = (int)avail.y;

    ImVec2 pos = ImGui::GetCursorScreenPos();
    vpImagePos_ = pos;
    vpImageSize_ = avail;
    ImGui::Image((ImTextureID)rhi::nativeTextureHandle(vpTex_), avail, ImVec2(0, 1),
                 ImVec2(1, 0));
    viewportHovered_ = ImGui::IsItemHovered();

    // RMB fly-look latch: engages over the viewport, persists while held.
    if (viewportHovered_ && ImGui::IsMouseDown(ImGuiMouseButton_Right)) rmbLook_ = true;
    if (!ImGui::IsMouseDown(ImGuiMouseButton_Right)) rmbLook_ = false;

    // Drop assets from the Content Browser / Prefab Library into the world.
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ASSET_PATH")) {
            std::string rel((const char*)p->Data, p->DataSize);
            if (endsWith(rel, ".glb") || endsWith(rel, ".gltf") || endsWith(rel, ".obj") ||
                endsWith(rel, ".fbx")) {
                if (Model* m = assets_->model(rel)) {
                    recordUndo();
                    Entity* e = world_->spawn(rel.substr(rel.find_last_of("\\/") + 1));
                    Camera c = editorCamera();
                    e->transform.position = c.position + c.forward() * 5.0f;
                    auto* mc = e->addComponent<ModelComponent>(m);
                    mc->modelPath = rel;
                    select(e);
                    AE_LOG("[Editor] spawned model %s", rel.c_str());
                }
            } else if (endsWith(rel, ".json") && !playing_) {
                loadScene(assets_->resolvePath(rel));
            }
        }
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_PREFAB_PATH")) {
            if (!playing_) placePrefab(assets_->resolvePath(std::string((const char*)p->Data, p->DataSize)));
        }
        ImGui::EndDragDropTarget();
    }

    bool clickedViewport = ImGui::IsItemClicked(ImGuiMouseButton_Left);

    if (!playing_) {
        drawGizmo(pos, avail, frameCamera_);
        if (clickedViewport && !ImGuizmo::IsOver() && !ImGuizmo::IsUsing() && !rmbLook_)
            pickAtMouse(pos, avail, frameCamera_);
    }

    drawViewportOverlay(pos, avail);
    ImGui::End();
}

void Editor::drawGizmo(const ImVec2& imgPos, const ImVec2& imgSize, const Camera& cam) {
    Entity* e = selected();
    if (!e || gizmoOp_ == 0) return;

    ImGuizmo::SetOrthographic(false);
    ImGuizmo::SetDrawlist();
    ImGuizmo::SetRect(imgPos.x, imgPos.y, imgSize.x, imgSize.y);

    Mat4 view = cam.view();
    Mat4 proj = cam.proj(imgSize.x / imgSize.y);
    float model[16];
    std::memcpy(model, e->worldMatrix().data(), sizeof(model));

    float snapVals[3] = {snapMove_, snapMove_, snapMove_};
    if (gizmoOp_ == ImGuizmo::ROTATE) snapVals[0] = snapVals[1] = snapVals[2] = snapRotate_;
    if (gizmoOp_ == ImGuizmo::SCALE) snapVals[0] = snapVals[1] = snapVals[2] = snapScale_;
    bool snap = snapEnabled_ || ImGui::IsKeyDown(ImGuiKey_LeftCtrl);

    ImGuizmo::MODE mode =
        (gizmoOp_ == ImGuizmo::SCALE || !gizmoWorld_) ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
    if (ImGuizmo::Manipulate(view.data(), proj.data(), (ImGuizmo::OPERATION)gizmoOp_, mode, model,
                             nullptr, snap ? snapVals : nullptr)) {
        Mat4 newWorld;
        std::memcpy(&newWorld, model, sizeof(model));
        setLocalFromWorld(e, newWorld);
    }
}

void Editor::pickAtMouse(const ImVec2& imgPos, const ImVec2& imgSize, const Camera& cam) {
    ImVec2 mouse = ImGui::GetMousePos();
    float nx = (mouse.x - imgPos.x) / imgSize.x * 2.0f - 1.0f;
    float ny = 1.0f - (mouse.y - imgPos.y) / imgSize.y * 2.0f;

    Mat4 invVP = inverse(cam.proj(imgSize.x / imgSize.y) * cam.view());
    Vec4 p0 = invVP * Vec4(nx, ny, -1.0f, 1.0f);
    Vec4 p1 = invVP * Vec4(nx, ny, 1.0f, 1.0f);
    Vec3 o(p0.x / p0.w, p0.y / p0.w, p0.z / p0.w);
    Vec3 far3(p1.x / p1.w, p1.y / p1.w, p1.z / p1.w);
    Vec3 d = normalize(far3 - o);

    Entity* best = nullptr;
    float bestT = 1e30f;
    for (const auto& eptr : world_->entities()) {
        Entity* e = eptr.get();
        if (!e->active()) continue;

        Vec3 bmin, bmax;
        bool has = false;
        if (auto* mr = e->getComponent<MeshRenderer>()) {
            if (mr->mesh) {
                bmin = mr->mesh->boundsMin();
                bmax = mr->mesh->boundsMax();
                has = true;
            }
        }
        if (!has) {
            if (auto* mc = e->getComponent<ModelComponent>()) {
                if (mc->model) {
                    bmin = mc->model->boundsMin();
                    bmax = mc->model->boundsMax();
                    has = true;
                }
            }
        }
        if (!has && (e->getComponent<LightComponent>() || e->getComponent<CameraComponent>() ||
                     e->getComponent<DialogueTriggerComponent>())) {
            bmin = Vec3(-0.35f, -0.35f, -0.35f);
            bmax = Vec3(0.35f, 0.35f, 0.35f);
            has = true;
        }
        if (!has) continue;

        Mat4 inv = inverse(e->worldMatrix());
        Vec4 lo4 = inv * Vec4(o, 1.0f);
        Vec4 ld4 = inv * Vec4(d, 0.0f);
        float t = rayAabb(Vec3(lo4.x, lo4.y, lo4.z), Vec3(ld4.x, ld4.y, ld4.z), bmin, bmax);
        if (t >= 0.0f && t < bestT) {
            bestT = t;
            best = e;
        }
    }
    select(best);
}

void Editor::drawViewportOverlay(const ImVec2& imgPos, const ImVec2& imgSize) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 cText = IM_COL32(235, 238, 245, 255);
    const ImU32 cDim = IM_COL32(150, 155, 165, 220);

    // Mode pill.
    const char* mode = playing_ ? (paused_ ? "PAUSED" : "PLAYING") : "EDIT";
    ImU32 pill = playing_ ? (paused_ ? IM_COL32(160, 120, 30, 220) : IM_COL32(180, 55, 60, 220))
                          : IM_COL32(20, 20, 26, 180);
    ImVec2 p(imgPos.x + 12, imgPos.y + 10);
    ImVec2 sz = ImGui::CalcTextSize(mode);
    dl->AddRectFilled(p, ImVec2(p.x + sz.x + 18, p.y + sz.y + 8), pill, 4.0f);
    dl->AddText(ImVec2(p.x + 9, p.y + 4), cText, mode);

    if (showStats_) {
        const FrameStats& st = renderer_->stats;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%.0f fps   draws %d / culled %d   lights %d", fps_,
                      st.drawCalls, st.culled, st.lights);
        dl->AddText(ImVec2(imgPos.x + 12, p.y + sz.y + 16), cDim, buf);
    }
    if (!playing_) {
        dl->AddText(ImVec2(imgPos.x + 12, imgPos.y + imgSize.y - 24), cDim,
                    "RMB + WASD fly · scroll speed · LMB pick · W/E/R gizmo · F focus · F5 play");
    }
    if (playing_ && world_->activeDialogue() == nullptr) {
        dl->AddText(ImVec2(imgPos.x + 12, imgPos.y + imgSize.y - 24), cDim,
                    "Esc stop · approach the cyan marker to start the dialogue scene");
    }
}

// ---------------------------------------------------------------------------
// outliner
// ---------------------------------------------------------------------------
void Editor::drawOutlinerNode(Entity* e) {
    ImGui::PushID((int)e->id());

    if (renamingId_ == e->id()) {
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", renameBuf_, sizeof(renameBuf_),
                             ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll)) {
            if (renameBuf_[0]) e->setName(renameBuf_);
            renamingId_ = 0;
        }
        if (ImGui::IsItemDeactivated()) renamingId_ = 0;
        ImGui::PopID();
        return;
    }

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth |
                               ImGuiTreeNodeFlags_OpenOnDoubleClick |
                               ImGuiTreeNodeFlags_DefaultOpen;
    if (e->children().empty()) flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (selected() == e) flags |= ImGuiTreeNodeFlags_Selected;

    if (!e->active()) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.45f, 0.47f, 0.52f, 1.0f));
    bool opened = ImGui::TreeNodeEx("##node", flags, "%s", e->name().c_str());
    if (!e->active()) ImGui::PopStyleColor();

    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !ImGui::IsItemToggledOpen()) select(e);

    // Drag to reparent.
    if (ImGui::BeginDragDropSource()) {
        uint32_t id = e->id();
        ImGui::SetDragDropPayload("AE_ENTITY", &id, sizeof(id));
        ImGui::Text("%s", e->name().c_str());
        ImGui::EndDragDropSource();
    }
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ENTITY")) {
            Entity* dragged = world_->findById(*(const uint32_t*)p->Data);
            if (dragged) {
                recordUndo();
                if (world_->reparent(dragged, e)) {
                    markViewportDirty();
                    AE_LOG("[Editor] parented '%s' under '%s'", dragged->name().c_str(),
                           e->name().c_str());
                } else {
                    undoStack_.pop_back(); // reparent refused (cycle) — no change
                }
            }
        }
        ImGui::EndDragDropTarget();
    }

    if (ImGui::BeginPopupContextItem("entity_ctx")) {
        select(e);
        if (ImGui::MenuItem("Rename")) {
            renamingId_ = e->id();
            std::snprintf(renameBuf_, sizeof(renameBuf_), "%s", e->name().c_str());
        }
        if (ImGui::MenuItem("Duplicate", "Ctrl+D")) duplicateSelected();
        if (ImGui::MenuItem("Focus", "F")) focusSelected();
        if (ImGui::MenuItem("Unparent", nullptr, false, e->parent() != nullptr)) {
            recordUndo();
            world_->reparent(e, nullptr);
            markViewportDirty();
        }
        if (ImGui::MenuItem("Save as Prefab...", nullptr, false, !playing_)) savePrefabDialog(e);
        ImGui::Separator();
        if (ImGui::MenuItem("Delete", "Del")) deleteSelected();
        ImGui::EndPopup();
    }

    if (opened && !e->children().empty()) {
        // Iterate a copy: reparenting/deleting mutates the children vector.
        std::vector<Entity*> kids = e->children();
        for (Entity* c : kids) drawOutlinerNode(c);
        ImGui::TreePop();
    }
    ImGui::PopID();
}

void Editor::drawOutliner() {
    if (!ImGui::Begin("Outliner", &showOutliner_)) {
        ImGui::End();
        return;
    }

    ImGui::SetNextItemWidth(-1);
    ImGui::InputTextWithHint("##filter", "Search...", outlinerFilter_, sizeof(outlinerFilter_));
    ImGui::Separator();

    ImGui::BeginChild("##tree", ImVec2(0, 0), ImGuiChildFlags_None);

    if (outlinerFilter_[0]) {
        std::string needle = toLower(outlinerFilter_);
        for (const auto& e : world_->entities()) {
            if (toLower(e->name()).find(needle) == std::string::npos) continue;
            ImGui::PushID((int)e->id());
            if (ImGui::Selectable(e->name().c_str(), selected() == e.get())) select(e.get());
            ImGui::PopID();
        }
    } else {
        std::vector<Entity*> roots;
        for (const auto& e : world_->entities())
            if (!e->parent()) roots.push_back(e.get());
        for (Entity* r : roots) drawOutlinerNode(r);
    }

    // Empty-space context menu + drop-to-root zone.
    if (ImGui::BeginPopupContextWindow("outliner_bg", ImGuiPopupFlags_MouseButtonRight |
                                                          ImGuiPopupFlags_NoOpenOverItems)) {
        if (ImGui::BeginMenu("Spawn")) {
            if (ImGui::MenuItem("Empty")) spawnPrimitive("empty");
            if (ImGui::MenuItem("Cube")) spawnPrimitive("cube");
            if (ImGui::MenuItem("Sphere")) spawnPrimitive("sphere");
            if (ImGui::MenuItem("Plane")) spawnPrimitive("plane");
            if (ImGui::MenuItem("Torus")) spawnPrimitive("torus");
            ImGui::Separator();
            if (ImGui::MenuItem("Point Light")) spawnPrimitive("light");
            if (ImGui::MenuItem("Camera")) spawnPrimitive("camera");
            if (ImGui::MenuItem("Dialogue Trigger")) spawnPrimitive("trigger");
            ImGui::EndMenu();
        }
        ImGui::EndPopup();
    }
    ImGui::Dummy(ImVec2(-1.0f, 40.0f));
    if (ImGui::BeginDragDropTarget()) {
        if (const ImGuiPayload* p = ImGui::AcceptDragDropPayload("AE_ENTITY")) {
            Entity* dragged = world_->findById(*(const uint32_t*)p->Data);
            if (dragged && dragged->parent()) { recordUndo(); world_->reparent(dragged, nullptr); markViewportDirty(); }
        }
        ImGui::EndDragDropTarget();
    }

    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// details
// ---------------------------------------------------------------------------
void Editor::drawDetails() {
    if (!ImGui::Begin("Details", &showDetails_)) {
        ImGui::End();
        return;
    }
    Entity* e = selected();
    if (!e) {
        ImGui::TextDisabled("Select an entity in the Outliner or the Viewport.");
        ImGui::End();
        return;
    }

    // Name + active.
    char nameBuf[128];
    std::snprintf(nameBuf, sizeof(nameBuf), "%s", e->name().c_str());
    ImGui::SetNextItemWidth(-64);
    if (ImGui::InputText("##name", nameBuf, sizeof(nameBuf))) e->setName(nameBuf);
    ImGui::SameLine();
    bool active = e->active();
    if (ImGui::Checkbox("On", &active)) e->setActive(active);

    // Durable id + prefab authoring.
    std::string gid = e->guid().toString();
    ImGui::TextDisabled("GUID %.8s...%s", gid.c_str(), gid.c_str() + 28);
    if (ImGui::IsItemClicked()) ImGui::SetClipboardText(gid.c_str());
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  (click to copy)", gid.c_str());
    ImGui::SameLine();
    ImGui::BeginDisabled(playing_);
    if (ImGui::SmallButton("Save as Prefab")) savePrefabDialog(e);
    ImGui::EndDisabled();

    // Transform.
    ImGui::SeparatorText("Transform");
    Transform& t = e->transform;
    ImGui::DragFloat3("Position", &t.position.x, 0.05f);

    // Euler UI via ImGuizmo's decompose/recompose (consistent pair).
    float mat[16];
    std::memcpy(mat, t.matrix().data(), sizeof(mat));
    float tr[3], rot[3], sc[3];
    ImGuizmo::DecomposeMatrixToComponents(mat, tr, rot, sc);
    if (ImGui::DragFloat3("Rotation", rot, 0.5f, 0, 0, "%.1f°")) {
        ImGuizmo::RecomposeMatrixFromComponents(tr, rot, sc, mat);
        Mat4 m;
        std::memcpy(&m, mat, sizeof(mat));
        Vec3 cx(m.m[0][0], m.m[0][1], m.m[0][2]);
        Vec3 cy(m.m[1][0], m.m[1][1], m.m[1][2]);
        Vec3 cz(m.m[2][0], m.m[2][1], m.m[2][2]);
        Vec3 s(length(cx), length(cy), length(cz));
        t.rotation = quatFromBasis(s.x > 1e-8f ? cx / s.x : Vec3(1, 0, 0),
                                   s.y > 1e-8f ? cy / s.y : Vec3(0, 1, 0),
                                   s.z > 1e-8f ? cz / s.z : Vec3(0, 0, 1));
    }
    ImGui::DragFloat3("Scale", &t.scaling.x, 0.02f);

    // Components.
    Component* toRemove = nullptr;
    for (const auto& compPtr : e->components()) {
        Component* c = compPtr.get();
        ImGui::PushID(c);
        bool keepOpen = ImGui::CollapsingHeader(componentDisplayName(c),
                                                ImGuiTreeNodeFlags_DefaultOpen);
        if (ImGui::BeginPopupContextItem("comp_ctx")) {
            if (ImGui::MenuItem("Remove Component")) toRemove = c;
            ImGui::EndPopup();
        }
        if (keepOpen) {
            // Generic reflected UI: every registered component (built-ins and
            // game-module scripts) renders through its reflect() — no per-type
            // editor code.
            ImGuiInspectorVisitor iv(assets_, world_);
            iv.openAnimGraph = [this](const std::string& path) {
                animGraph_.open(assets_->resolvePath(path));
                animGraph_.visible = true;
            };
            iv.openUIDesigner = [this](const std::string& path) {
                uiDesigner_.open(assets_->resolvePath(path));
                uiDesigner_.visible = true;
            };
            iv.openMaterialGraph = [this](const std::string& path) {
                materialGraph_.open(assets_->resolvePath(path));
                materialGraph_.visible = true;
            };
            iv.openScriptGraph = [this](const std::string& path) {
                scriptGraph_.open(assets_->resolvePath(path));
                scriptGraph_.visible = true;
            };
            iv.openDialogueScene = [this](const std::string& path) {
                dialogueGraph_.open(assets_->resolvePath(path));
                dialogueGraph_.visible = true;
            };
            iv.setCurrent(c);
            c->reflect(iv);
            if (iv.assetsDirty()) c->onDeserialized(*assets_);

            // Editor-only extra with no reflected field: animation clip picker
            // (clip selection lives on the shared Model, not the component).
            if (auto* mc = dynamic_cast<ModelComponent*>(c)) {
                if (mc->model && mc->model->clipCount() > 0) {
                    if (ImGui::BeginCombo("Clip", "animation clips")) {
                        for (int i = 0; i < mc->model->clipCount(); ++i)
                            if (ImGui::Selectable(mc->model->clipName(i)))
                                mc->model->setClip(i);
                        ImGui::EndCombo();
                    }
                }
            }
        }
        ImGui::PopID();
    }
    if (toRemove) { recordUndo(); e->removeComponent(toRemove); }

    // Add component: everything user-creatable in the registry, grouped by
    // category in registration order.
    ImGui::Spacing();
    if (ImGui::Button("+ Add Component", ImVec2(-1, 0))) ImGui::OpenPopup("add_comp");
    if (ImGui::BeginPopup("add_comp")) {
        std::string lastCategory;
        bool anyShown = false;
        for (const ComponentDesc& d : componentRegistry().all()) {
            if (!d.userCreatable) continue;
            if (anyShown && d.category != lastCategory) ImGui::Separator();
            lastCategory = d.category;
            anyShown = true;
            if (ImGui::MenuItem(d.displayName.c_str())) {
                recordUndo();
                Component* n = d.create(*e);
                if (d.initDefaults) d.initDefaults(n, *assets_);
            }
        }
        ImGui::EndPopup();
    }

    ImGui::End();
}

// ---------------------------------------------------------------------------
// content browser
// ---------------------------------------------------------------------------
void Editor::drawContentBrowser() {
    if (!ImGui::Begin("Content Browser", &showContent_)) {
        ImGui::End();
        return;
    }

    // Toolbar row.
    bool atRoot = _stricmp(contentDir_.c_str(), projectRoot_.c_str()) == 0;
    ImGui::BeginDisabled(atRoot);
    if (ImGui::Button("<- Up")) contentDir_ = parentDir(contentDir_);
    ImGui::EndDisabled();
    ImGui::SameLine();
    std::string crumb = contentDir_.size() > projectRoot_.size()
                            ? contentDir_.substr(projectRoot_.size() + 1)
                            : "(project root)";
    ImGui::TextDisabled("%s", crumb.c_str());
    ImGui::Separator();

    // Folder tree on the left.
    ImGui::BeginChild("##folders", ImVec2(200, 0), ImGuiChildFlags_ResizeX);
    std::function<void(const std::string&, const std::string&)> drawDir =
        [&](const std::string& path, const std::string& label) {
            std::string lower = toLower(label);
            if (lower == "build" || lower == "third_party" || lower == ".git") return;
            bool inPath = _strnicmp(contentDir_.c_str(), path.c_str(), path.size()) == 0;
            ImGuiTreeNodeFlags fl = ImGuiTreeNodeFlags_OpenOnArrow |
                                    ImGuiTreeNodeFlags_SpanAvailWidth;
            if (_stricmp(contentDir_.c_str(), path.c_str()) == 0)
                fl |= ImGuiTreeNodeFlags_Selected;
            if (inPath) ImGui::SetNextItemOpen(true, ImGuiCond_Once);
            bool opened = ImGui::TreeNodeEx(label.c_str(), fl);
            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) contentDir_ = path;
            if (opened) {
                for (const DirEntry& d : listDir(path))
                    if (d.isDir) drawDir(path + "\\" + d.name, d.name);
                ImGui::TreePop();
            }
        };
    drawDir(projectRoot_, "project");
    ImGui::EndChild();
    ImGui::SameLine();

    // Asset grid on the right.
    ImGui::BeginChild("##grid");
    const float tileW = 92.0f, tileH = 92.0f, pad = 10.0f;
    int cols = std::max(1, (int)((ImGui::GetContentRegionAvail().x) / (tileW + pad)));
    int i = 0;
    for (const DirEntry& de : listDir(contentDir_)) {
        AssetKind kind = classify(contentDir_, de);
        if (i % cols != 0) ImGui::SameLine();
        ++i;

        ImGui::PushID(de.name.c_str());
        ImGui::BeginGroup();
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImGui::InvisibleButton("##tile", ImVec2(tileW, tileH));
        bool hovered = ImGui::IsItemHovered();
        bool doubled = hovered && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left);

        ImU32 chip;
        const char* tag;
        switch (kind) {
        case AssetKind::Folder:   chip = IM_COL32(140, 115, 245, 255); tag = "DIR"; break;
        case AssetKind::Model:    chip = IM_COL32(80, 170, 245, 255);  tag = "GLTF"; break;
        case AssetKind::Map:      chip = IM_COL32(90, 200, 120, 255);  tag = "MAP"; break;
        case AssetKind::Dialogue: chip = IM_COL32(235, 160, 70, 255);  tag = "DLG"; break;
        case AssetKind::Missions: chip = IM_COL32(235, 110, 110, 255); tag = "MSN"; break;
        case AssetKind::Prefab:   chip = IM_COL32(120, 235, 170, 255); tag = "PREF"; break;
        case AssetKind::Image:    chip = IM_COL32(200, 120, 210, 255); tag = "IMG"; break;
        case AssetKind::Shader:   chip = IM_COL32(120, 210, 200, 255); tag = "GLSL"; break;
        case AssetKind::Audio:    chip = IM_COL32(240, 200, 90, 255);  tag = "WAV"; break;
        case AssetKind::Script:   chip = IM_COL32(110, 200, 235, 255); tag = "SCPT"; break;
        case AssetKind::MaterialG: chip = IM_COL32(220, 120, 170, 255); tag = "MAT"; break;
        case AssetKind::UIDoc:    chip = IM_COL32(150, 230, 150, 255); tag = "UI"; break;
        case AssetKind::Anim:     chip = IM_COL32(245, 170, 120, 255); tag = "ANIM"; break;
        case AssetKind::Data:     chip = IM_COL32(120, 230, 220, 255); tag = "DATA"; break;
        default:                  chip = IM_COL32(110, 115, 125, 255); tag = "FILE"; break;
        }

        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddRectFilled(pos, ImVec2(pos.x + tileW, pos.y + tileH),
                          hovered ? IM_COL32(44, 44, 54, 255) : IM_COL32(30, 30, 38, 255), 6.0f);
        dl->AddRectFilled(ImVec2(pos.x + 10, pos.y + 10), ImVec2(pos.x + tileW - 10, pos.y + 46),
                          (chip & 0x00FFFFFF) | 0x38000000, 5.0f);
        ImVec2 tagSz = ImGui::CalcTextSize(tag);
        dl->AddText(ImVec2(pos.x + (tileW - tagSz.x) * 0.5f, pos.y + 20), chip, tag);
        std::string label = de.name.size() > 13 ? de.name.substr(0, 12) + "…" : de.name;
        ImVec2 lsz = ImGui::CalcTextSize(label.c_str());
        dl->AddText(ImVec2(pos.x + (tileW - lsz.x) * 0.5f, pos.y + tileH - 34),
                    IM_COL32(225, 228, 235, 255), label.c_str());
        if (hovered && ImGui::CalcTextSize(de.name.c_str()).x > tileW)
            ImGui::SetTooltip("%s", de.name.c_str());

        std::string full = contentDir_ + "\\" + de.name;
        std::string rel = assets_->toProjectRelative(full);

        // Draggable assets (models spawn on viewport drop, maps load, prefabs
        // instantiate). Prefabs use their own payload so the viewport can tell
        // them apart from a map file.
        if (!de.isDir && ImGui::BeginDragDropSource()) {
            if (kind == AssetKind::Prefab)
                ImGui::SetDragDropPayload("AE_PREFAB_PATH", rel.c_str(), rel.size());
            else if (kind == AssetKind::Model || kind == AssetKind::Map ||
                     kind == AssetKind::Audio || kind == AssetKind::Script ||
                     kind == AssetKind::MaterialG || kind == AssetKind::Image ||
                     kind == AssetKind::UIDoc || kind == AssetKind::Anim)
                ImGui::SetDragDropPayload("AE_ASSET_PATH", rel.c_str(), rel.size());
            ImGui::Text("%s", de.name.c_str());
            ImGui::EndDragDropSource();
        }

        if (doubled) {
            switch (kind) {
            case AssetKind::Folder: contentDir_ = full; break;
            case AssetKind::Map: if (!playing_) loadScene(full); break;
            case AssetKind::Dialogue:
                dialogueGraph_.open(full);
                dialogueGraph_.visible = true;
                break;
            case AssetKind::Missions:
                missionPanel_.visible = true;
                world_->missions.load(full);
                break;
            case AssetKind::Prefab: if (!playing_) placePrefab(full); break;
            case AssetKind::Audio: { // preview the clip (2D one-shot)
                SoundId s = audioEngine().loadSound(full);
                if (s >= 0) audioEngine().playOneShot(s, 1.0f);
                break;
            }
            case AssetKind::Script:
                scriptGraph_.open(full);
                scriptGraph_.visible = true;
                break;
            case AssetKind::MaterialG:
                materialGraph_.open(full);
                materialGraph_.visible = true;
                break;
            case AssetKind::UIDoc:
                uiDesigner_.open(full);
                uiDesigner_.visible = true;
                break;
            case AssetKind::Anim:
                animGraph_.open(full);
                animGraph_.visible = true;
                break;
            case AssetKind::Data:
                dataTable_.open(full);
                dataTable_.visible = true;
                break;
            default: break;
            }
        }
        if (ImGui::BeginPopupContextItem("tile_ctx")) {
            if (kind == AssetKind::Model && ImGui::MenuItem("Spawn in scene")) {
                if (Model* m = assets_->model(rel)) {
                    recordUndo();
                    Entity* ne = world_->spawn(de.name);
                    Camera cam = editorCamera();
                    ne->transform.position = cam.position + cam.forward() * 5.0f;
                    auto* mc = ne->addComponent<ModelComponent>(m);
                    mc->modelPath = rel;
                    select(ne);
                }
            }
            if (kind == AssetKind::Map && ImGui::MenuItem("Load map", nullptr, false, !playing_))
                loadScene(full);
            if (kind == AssetKind::Dialogue && ImGui::MenuItem("Open in Dialogue Graph")) {
                dialogueGraph_.open(full);
                dialogueGraph_.visible = true;
            }
            if (kind == AssetKind::Prefab &&
                ImGui::MenuItem("Place in scene", nullptr, false, !playing_))
                placePrefab(full);
            ImGui::EndPopup();
        }

        ImGui::EndGroup();
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// output log
// ---------------------------------------------------------------------------
void Editor::drawOutputLog() {
    if (!ImGui::Begin("Output Log", &showLog_)) {
        ImGui::End();
        return;
    }
    if (ImGui::Button("Clear")) clearLog();
    ImGui::SameLine();
    ImGui::SetNextItemWidth(220);
    ImGui::InputTextWithHint("##logfilter", "Filter...", logFilter_, sizeof(logFilter_));
    ImGui::Separator();

    static uint64_t lastVersion = 0;
    ImGui::BeginChild("##logscroll", ImVec2(0, 0), ImGuiChildFlags_None,
                      ImGuiWindowFlags_HorizontalScrollbar);
    std::string needle = toLower(logFilter_);
    for (const LogEntry& le : logEntries()) {
        if (!needle.empty() && toLower(le.text).find(needle) == std::string::npos) continue;
        ImVec4 col = le.level == LogLevel::Error  ? ImVec4(0.95f, 0.45f, 0.45f, 1.0f)
                     : le.level == LogLevel::Warn ? ImVec4(0.95f, 0.78f, 0.40f, 1.0f)
                                                  : ImVec4(0.75f, 0.78f, 0.84f, 1.0f);
        ImGui::TextDisabled("[%7.2f]", le.time);
        ImGui::SameLine();
        ImGui::TextColored(col, "%s", le.text.c_str());
    }
    if (logVersion() != lastVersion && ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 40.0f)
        ImGui::SetScrollHereY(1.0f);
    lastVersion = logVersion();
    ImGui::EndChild();
    ImGui::End();
}

// ---------------------------------------------------------------------------
// world settings
// ---------------------------------------------------------------------------
void Editor::drawWorldSettings() {
    if (!ImGui::Begin("World Settings", &showWorldSettings_)) {
        ImGui::End();
        return;
    }

    ImGui::SeparatorText("Sun & Sky");
    Vec3 sd = world_->env.sunDir;
    float elevation = std::asin(clampf(sd.y, -1.0f, 1.0f)) * 180.0f / PI;
    float azimuth = std::atan2(sd.z, sd.x) * 180.0f / PI;
    bool sunChanged = false;
    sunChanged |= ImGui::SliderFloat("Elevation", &elevation, -5.0f, 89.0f, "%.1f°");
    sunChanged |= ImGui::SliderFloat("Azimuth", &azimuth, -180.0f, 180.0f, "%.1f°");
    if (sunChanged) {
        float er = radians(elevation), ar = radians(azimuth);
        world_->env.sunDir =
            normalize(Vec3(std::cos(er) * std::cos(ar), std::sin(er), std::cos(er) * std::sin(ar)));
    }
    ImGui::SliderFloat("Sky intensity", &world_->env.skyIntensity, 1.0f, 60.0f);

    ImGui::SeparatorText("Post-processing");
    RenderSettings& rs = renderer_->settings;
    ImGui::SliderFloat("Exposure", &rs.exposure, 0.05f, 4.0f);
    ImGui::SliderFloat("Bloom", &rs.bloomStrength, 0.0f, 0.3f);
    ImGui::SliderFloat("Fog density", &rs.fogDensity, 0.0f, 0.05f, "%.4f");
    ImGui::SliderFloat("Fog falloff", &rs.fogHeightFalloff, 0.01f, 0.5f);

    ImGui::SeparatorText("Performance");
    ImGui::SliderInt("Shadow cascades", &rs.shadowCascades, 1, Renderer::kNumCascades);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The whole scene is re-rendered once per cascade — the biggest\n"
                          "cost on integrated GPUs. Fewer = faster, shorter shadow range.");
    ImGui::SliderFloat("Render scale", &rs.renderScale, 0.25f, 1.0f, "%.2fx");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Renders the 3D viewport at this fraction of its pixel size,\n"
                          "then upscales (helps only when fragment-bound). %dx%d -> %dx%d",
                          desiredVpW_, desiredVpH_, (int)(desiredVpW_ * rs.renderScale),
                          (int)(desiredVpH_ * rs.renderScale));
    ImGui::Checkbox("Frustum culling", &rs.frustumCulling);
    ImGui::TextDisabled("Viewport re-renders only when the scene changes.");
    bool vs = rs.vsync;
    if (ImGui::Checkbox("VSync", &vs)) {
        rs.vsync = vs;
        window_->setVSync(vs);
    }
    ImGui::PlotLines("##ms", frameMsHistory_, 120, frameMsCursor_, "frame ms", 0.0f, 33.3f,
                     ImVec2(-1, 44));

    ImGui::SeparatorText("Gameplay");
    char pbuf[128];
    std::snprintf(pbuf, sizeof(pbuf), "%s", world_->missions.playerEntityName.c_str());
    if (ImGui::InputText("Player entity", pbuf, sizeof(pbuf)))
        world_->missions.playerEntityName = pbuf;

    ImGui::End();
}

// ---------------------------------------------------------------------------
// actions
// ---------------------------------------------------------------------------
void Editor::focusSelected() {
    Entity* e = selected();
    if (!e) return;
    Vec3 center = e->worldPosition();
    float dist = 5.0f;
    if (auto* mr = e->getComponent<MeshRenderer>()) {
        if (mr->mesh) {
            Vec3 ext = (mr->mesh->boundsMax() - mr->mesh->boundsMin()) * 0.5f;
            float r = length(ext * e->transform.scaling);
            dist = std::max(2.0f, r * 3.0f);
        }
    } else if (auto* mc = e->getComponent<ModelComponent>()) {
        if (mc->model) {
            Vec3 ext = (mc->model->boundsMax() - mc->model->boundsMin()) * 0.5f;
            float r = length(ext * e->transform.scaling);
            dist = std::max(2.0f, r * 3.0f);
        }
    }
    camPos_ = center - editorCamera().forward() * dist;
}

void Editor::deleteSelected() {
    Entity* e = selected();
    if (!e || playing_) return;
    recordUndo();
    AE_LOG("[Editor] deleted '%s'", e->name().c_str());
    world_->destroy(e);
    select(nullptr);
}

void Editor::duplicateSelected() {
    Entity* e = selected();
    if (!e || playing_) return;
    recordUndo();
    Entity* copy = duplicateEntity(*world_, *assets_, e);
    if (copy) {
        copy->transform.position += Vec3(0.5f, 0.0f, 0.5f);
        select(copy);
        AE_LOG("[Editor] duplicated '%s'", e->name().c_str());
    }
}

Entity* Editor::spawnPrimitive(const char* kind) {
    recordUndo();
    Camera cam = editorCamera();
    Vec3 at = cam.position + cam.forward() * 5.0f;
    Entity* e = nullptr;
    std::string k = kind;

    auto solid = [](const Vec4& color, float metallic, float roughness) {
        Material m;
        m.baseColor = color;
        m.metallic = metallic;
        m.roughness = roughness;
        return m;
    };

    if (k == "empty") {
        e = world_->spawn("Empty");
    } else if (k == "cube" || k == "sphere" || k == "plane" || k == "torus") {
        e = world_->spawn(k == "cube" ? "Cube" : k == "sphere" ? "Sphere"
                                             : k == "plane"    ? "Plane"
                                                               : "Torus");
        auto* mr = e->addComponent<MeshRenderer>(
            assets_->mesh(k), solid(Vec4(0.75f, 0.76f, 0.80f, 1.0f), 0.0f, 0.5f));
        mr->meshName = k;
        if (k == "plane") at.y = 0.0f;
    } else if (k == "light") {
        e = world_->spawn("PointLight");
        e->addComponent<LightComponent>(Vec3(1.0f, 0.9f, 0.7f), 14.0f);
    } else if (k == "camera") {
        e = world_->spawn("Camera");
        e->addComponent<CameraComponent>();
    } else if (k == "trigger") {
        e = world_->spawn("DialogueTrigger");
        e->transform.scaling = Vec3(0.3f);
        Material m = solid(Vec4(0.2f, 0.9f, 0.95f, 1.0f), 0.0f, 0.3f);
        m.emissive = Vec3(0.2f, 0.9f, 0.95f) * 0.6f;
        auto* mr = e->addComponent<MeshRenderer>(assets_->mesh("sphere"), m, false);
        mr->meshName = "sphere";
        e->addComponent<SpinBehavior>()->degreesPerSec = 90.0f;
        auto* dt = e->addComponent<DialogueTriggerComponent>();
        dt->scenePath = "assets/dialogue/interrogation.json";
        dt->loadFromFile(assets_->resolvePath(dt->scenePath));
        dt->radius = 4.0f;
    }
    if (e) {
        e->transform.position = at;
        select(e);
        AE_LOG("[Editor] spawned %s", e->name().c_str());
    }
    return e;
}

void Editor::newScene() {
    world_->clear();
    scenePath_.clear();
    select(nullptr);
    world_->env.sunDir = normalize(Vec3(0.42f, 0.55f, 0.30f));
    world_->env.skyIntensity = 22.0f;

    Material tileMat;
    if (const MaterialTextures* t = assets_->textureSet("tile")) tileMat.setTextures(*t);
    tileMat.metallic = 1.0f;
    tileMat.roughness = 1.0f;
    Entity* floor = world_->spawn("Floor");
    auto* mr = floor->addComponent<MeshRenderer>(assets_->mesh("plane"), tileMat, false);
    mr->meshName = "plane";
    mr->texSetName = "tile";

    Entity* cam = world_->spawn("MainCamera");
    cam->transform.position = Vec3(0, 2.5f, 8.0f);
    cam->addComponent<CameraComponent>()->isDefault = true;
    cam->addComponent<CameraController>();
    primed_ = false;
    clearUndo();
    AE_LOG("[Editor] new map");
}

void Editor::loadScene(const std::string& path) {
    if (loadWorld(*world_, *assets_, path)) {
        scenePath_ = path;
        select(nullptr);
        primed_ = false;
        clearUndo();
    }
}

void Editor::openSceneDialog() {
    char file[MAX_PATH] = {};
    std::string initial = projectRoot_ + "\\assets\\maps";
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_->hwnd();
    ofn.lpstrFilter = "Map JSON (*.json)\0*.json\0All files (*.*)\0*.*\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initial.c_str();
    ofn.Flags = OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
    if (GetOpenFileNameA(&ofn)) loadScene(file);
}

bool Editor::saveScene(bool saveAs) {
    if (playing_) return false;
    std::string path = scenePath_;
    if (saveAs || path.empty()) {
        CreateDirectoryA((projectRoot_ + "\\assets\\maps").c_str(), nullptr);
        char file[MAX_PATH] = "map.json";
        std::string initial = projectRoot_ + "\\assets\\maps";
        OPENFILENAMEA ofn = {};
        ofn.lStructSize = sizeof(ofn);
        ofn.hwndOwner = window_->hwnd();
        ofn.lpstrFilter = "Map JSON (*.json)\0*.json\0\0";
        ofn.lpstrFile = file;
        ofn.nMaxFile = MAX_PATH;
        ofn.lpstrInitialDir = initial.c_str();
        ofn.lpstrDefExt = "json";
        ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
        if (!GetSaveFileNameA(&ofn)) return false;
        path = file;
    }
    if (!saveWorld(*world_, *assets_, path)) return false;
    scenePath_ = path;
    return true;
}

// ---------------------------------------------------------------------------
// entity references + prefabs
// ---------------------------------------------------------------------------
bool Editor::entityPicker(const char* label, EntityRef& ref) {
    Entity* cur = ref.get(*world_);
    bool changed = false;
    if (ImGui::BeginCombo(label, cur ? cur->name().c_str() : "(none)")) {
        if (ImGui::Selectable("(none)", !ref.valid())) {
            ref.clear();
            changed = true;
        }
        for (const auto& e : world_->entities()) {
            ImGui::PushID((int)e->id());
            char item[160];
            std::snprintf(item, sizeof(item), "%s", e->name().c_str());
            if (ImGui::Selectable(item, cur == e.get())) {
                ref.set(e.get());
                changed = true;
            }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    return changed;
}

void Editor::savePrefabDialog(Entity* e) {
    if (!e || playing_) return;
    CreateDirectoryA((projectRoot_ + "\\assets\\entities").c_str(), nullptr);
    std::string initial = projectRoot_ + "\\assets\\entities";
    char file[MAX_PATH];
    std::snprintf(file, sizeof(file), "%s.json", e->name().c_str());
    OPENFILENAMEA ofn = {};
    ofn.lStructSize = sizeof(ofn);
    ofn.hwndOwner = window_->hwnd();
    ofn.lpstrFilter = "Prefab JSON (*.json)\0*.json\0\0";
    ofn.lpstrFile = file;
    ofn.nMaxFile = MAX_PATH;
    ofn.lpstrInitialDir = initial.c_str();
    ofn.lpstrDefExt = "json";
    ofn.Flags = OFN_OVERWRITEPROMPT | OFN_NOCHANGEDIR;
    if (GetSaveFileNameA(&ofn)) saveEntityPrefab(*e, *assets_, file);
}

Entity* Editor::placePrefab(const std::string& path) {
    recordUndo();
    Entity* root = instantiatePrefab(*world_, *assets_, path, nullptr);
    if (root) {
        Camera cam = editorCamera();
        root->transform.position = cam.position + cam.forward() * 5.0f;
        select(root);
    }
    return root;
}

void Editor::drawPrefabLibrary() {
    if (!ImGui::Begin("Prefabs", &showPrefabs_)) {
        ImGui::End();
        return;
    }
    std::string dir = projectRoot_ + "\\assets\\entities";

    ImGui::TextDisabled("assets/entities");
    ImGui::SameLine();
    if (ImGui::SmallButton("New from selection") && selected() && !playing_)
        savePrefabDialog(selected());
    ImGui::Separator();

    WIN32_FIND_DATAA fd;
    HANDLE h = FindFirstFileA((dir + "\\*.json").c_str(), &fd);
    bool any = false;
    if (h != INVALID_HANDLE_VALUE) {
        do {
            any = true;
            std::string name = fd.cFileName;
            std::string full = dir + "\\" + name;
            std::string display = name.substr(0, name.find_last_of('.'));
            ImGui::PushID(name.c_str());
            ImGui::Bullet();
            ImGui::SameLine();
            ImGui::Selectable(display.c_str());
            // Drag into the viewport to place, or double-click.
            if (ImGui::BeginDragDropSource()) {
                std::string rel = assets_->toProjectRelative(full);
                ImGui::SetDragDropPayload("AE_PREFAB_PATH", rel.c_str(), rel.size());
                ImGui::Text("Place %s", display.c_str());
                ImGui::EndDragDropSource();
            }
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                !playing_)
                placePrefab(full);
            if (ImGui::BeginPopupContextItem("prefab_ctx")) {
                if (ImGui::MenuItem("Place in scene", nullptr, false, !playing_)) placePrefab(full);
                ImGui::EndPopup();
            }
            ImGui::PopID();
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    if (!any)
        ImGui::TextWrapped(
            "No prefabs yet. Select an entity and click 'Save as Prefab' in the "
            "Details panel (or the Outliner right-click menu) to create one.");

    ImGui::End();
}

} // namespace ae
