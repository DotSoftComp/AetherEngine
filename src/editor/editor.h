// Aether Engine editor — a UE-style docked workspace built on Dear ImGui
// (docking branch) + ImGuizmo:
//
//   [ menu bar: File / Edit / View / Tools / Help                          ]
//   [ toolbar: save/load · select/move/rotate/scale · snap · Play/Pause/Stop ]
//   [ Outliner | Viewport (3D, gizmos, picking) / Dialogue Graph | Details ]
//   [          |                                          | World Settings ]
//   [ Content Browser / Output Log / Missions                              ]
//   [ status bar: map · entity/draw stats · GPU                            ]
//
// Edit mode flies a freecam and never ticks gameplay. Play mode snapshots the
// World to a temp scene file (real PIE), runs behaviors/animation/missions/
// dialogue with the game HUD composited into the viewport texture, and Stop
// restores the snapshot exactly — no play-mode drift leaks into the map.
#pragma once
#include "../core/window.h"
#include "../render/renderer.h"
#include "../engine/world.h"
#include "../engine/assets.h"
#include "../engine/project.h"
#include "../engine/game_module.h"
#include "../engine/plugin_manager.h"
#include "../ui/ui.h"
#include "../ui/font.h"
#include "dialogue_graph.h"
#include "script_graph_panel.h"
#include "material_graph_panel.h"
#include "ui_designer_panel.h"
#include "input_map_panel.h"
#include "anim_graph_panel.h"
#include "data_table_panel.h"
#include "ai_panel.h"
#include "mission_panel.h"
#include "agent_bridge.h"
#include "../ai_assist/pulse_client.h"
#include "imgui.h"
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace ae {

class Editor {
public:
    bool init(Window* window, Renderer* renderer, World* world, AssetLibrary* assets,
              Font* font, const Project& project, GameModule* gameModule = nullptr,
              PluginManager* plugins = nullptr);

    // Tells the editor which map file the already-loaded world came from, so
    // the title bar and Ctrl+S target it (main loads the startup scene).
    void setScenePath(const std::string& path) { scenePath_ = path; }
    void shutdown();

    // One editor frame: simulate (edit or play), render the scene + HUD into
    // the viewport texture, then build the whole ImGui workspace.
    void frame(float dt, float realTime);

    bool playing() const { return playing_; }

    // Auto-enter Play mode a few frames after startup (--play; also handy for
    // headless verification screenshots).
    void requestAutoPlay() { autoPlayRequested_ = true; }

private:
    // ---- simulation / play-in-editor ----
    void simulate(float dt);
    void startPlay();
    void stopPlay();
    void updateFreecam(float dt);
    Camera editorCamera() const;

    // ---- viewport rendering ----
    void ensureViewportTarget(int w, int h);
    void renderSceneToViewport(float dt);
    // The 3D viewport is expensive (shadows + PBR); in Edit mode it only needs
    // re-rendering when something visible changed. Returns true then, and
    // reuses the cached viewport texture otherwise so an idle editor is cheap.
    bool viewportNeedsRender();
    void markViewportDirty() { forceRenderFrames_ = 3; }

    // ---- ImGui workspace ----
    void drawTitleBar();
    void drawMenus();  // the File/Edit/View/... menus, shared by the title bar
    void drawToolbar();
    void drawDockspace();

    // Borderless custom-frame caption hit-testing (see Window::CaptionHit).
    static bool captionHitThunk(void* user, int x, int y);
    bool captionAt(int x, int y) const;
    void buildDefaultLayout(unsigned int dockspaceId);
    void drawViewport(float dt);
    void drawViewportOverlay(const ImVec2& imgPos, const ImVec2& imgSize);
    void drawGizmo(const ImVec2& imgPos, const ImVec2& imgSize, const Camera& cam);
    void drawOutliner();
    void drawOutlinerNode(Entity* e);
    void drawDetails();
    void drawContentBrowser();
    void drawOutputLog();
    void drawWorldSettings();
    void drawStatusBar(float height);
    void drawProfiler();
    // Pushes collider / light-range wireframes into the debug-draw collector
    // (selected entity, or everything when showAllColliders_).
    void submitDebugShapes();

    // ---- actions ----
    void pickAtMouse(const ImVec2& imgPos, const ImVec2& imgSize, const Camera& cam);
    void focusSelected();
    void deleteSelected();
    void duplicateSelected();
    Entity* spawnPrimitive(const char* kind); // "empty","cube","sphere","plane","torus","light","camera","trigger"
    void newScene();
    void openSceneDialog();
    bool saveScene(bool saveAs);
    void loadScene(const std::string& path);

    // ---- undo / redo (whole-world snapshots via the scene serializer) ----
    // Discrete mutations (delete, duplicate, spawn, add/remove component,
    // reparent, prefab place) call recordUndo() just before changing the world.
    // Continuous gestures (gizmo drag, inspector slider) are captured by
    // updateInteractionUndo(), which snapshots on gesture start and commits on
    // release only if something actually changed. Disabled during Play.
    void recordUndo();
    void undo();
    void redo();
    bool canUndo() const { return !undoStack_.empty(); }
    bool canRedo() const { return !redoStack_.empty(); }
    void clearUndo() { undoStack_.clear(); redoStack_.clear(); }
    void updateInteractionUndo();
    void applySnapshot(const std::string& json, const Guid& sel);

    // Reusable entity-reference dropdown: lists every entity, writes the pick
    // into `ref` by Guid. Returns true when the selection changed.
    bool entityPicker(const char* label, EntityRef& ref);

    // ---- build & package ----
    void drawBuildDialog(); // modal: output dir, startup map, dev/shipping
    void startPackage();

    // ---- game module (native C++ scripts) ----
    // Kicks off a background cmake build of <project>/Source; output streams
    // into the Output Log. On success the DLL is hot-reloaded (see
    // pollCompile) with the scene round-tripped through the PIE snapshot.
    void startCompileScripts();
    void pollCompile();        // drains worker log lines; finishes the build
    void reloadGameModule();
    void drawModulesPanel();       // engine-module toggles + plugin lifecycle
    void reloadPlugin(PluginInfo& p);   // same snapshot round-trip as above
    void unloadPluginSafe(PluginInfo& p);
    void startCompilePlugin(const PluginInfo& p);   // snapshot → clear world → swap DLL → restore

    // ---- agent bridge (PulseLABS drives the live editor) ----
    // Transport lives in agent_bridge.h; the command dispatcher (main thread,
    // full editor access) is in bridge_commands.cpp.
    void startAgentBridge();
    void stopAgentBridge();
    void pumpAgentBridge(); // once per frame, before simulate()
    std::string handleBridgeCommand(const std::string& method, const JsonValue& params);

    // ---- prefabs (entity assets) ----
    void savePrefabDialog(Entity* e);
    void drawPrefabLibrary();
    Entity* placePrefab(const std::string& path);

    Entity* selected() const { return selectedId_ ? world_->findById(selectedId_) : nullptr; }
    void select(Entity* e) {
        selectedId_ = e ? e->id() : 0;
        markViewportDirty(); // selection outline / new entity needs a redraw
    }

    // ---- wiring ----
    Window* window_ = nullptr;
    Renderer* renderer_ = nullptr;
    World* world_ = nullptr;
    AssetLibrary* assets_ = nullptr;
    Font* font_ = nullptr;
    UI gameUI_; // renders the in-game HUD into the viewport FBO during Play
    RenderScene renderScene_;
    Project project_;
    std::string projectRoot_; // == project_.root (kept for the many path joins)

    // ---- viewport target ----
    rhi::FramebufferHandle vpFBO_;
    rhi::TextureHandle vpTex_;
    int vpW_ = 0, vpH_ = 0;
    int desiredVpW_ = 1280, desiredVpH_ = 720;
    ImVec2 vpImagePos_{0, 0};
    ImVec2 vpImageSize_{0, 0};
    bool viewportHovered_ = false;
    bool rmbLook_ = false;

    // Viewport-render caching (see viewportNeedsRender).
    Vec3 lastRenderCamPos_{1e9f, 1e9f, 1e9f};
    float lastRenderYaw_ = 1e9f, lastRenderPitch_ = 1e9f;
    int forceRenderFrames_ = 4;   // render the first frames unconditionally
    bool wasInteracting_ = false; // any ImGui item / gizmo active last frame

    // ---- freecam ----
    Vec3 camPos_{-2.0f, 3.2f, 9.5f};
    float camYaw_ = -75.0f, camPitch_ = -14.0f, camSpeed_ = 6.0f;
    Camera frameCamera_; // camera used for this frame's render (for gizmos/picking)

    // ---- play-in-editor ----
    bool playing_ = false;
    bool paused_ = false;
    bool primed_ = false;
    bool autoPlayRequested_ = false;
    int frameCount_ = 0;
    float playClock_ = 0.0f;
    std::string pieSnapshotPath_;

    // ---- undo / redo ----
    struct UndoState { std::string json; Guid sel; };
    std::vector<UndoState> undoStack_, redoStack_;
    std::string pendingSnapshot_; // pre-edit state captured at a gesture's start
    Guid pendingSel_;
    bool interacting_ = false;     // a gizmo/inspector gesture is in progress

    // ---- selection / gizmo ----
    uint32_t selectedId_ = 0;
    int gizmoOp_ = 7; // ImGuizmo::TRANSLATE (value inlined to keep the header ImGuizmo-free)
    bool gizmoWorld_ = true;
    bool snapEnabled_ = false;
    float snapMove_ = 0.5f, snapRotate_ = 15.0f, snapScale_ = 0.25f;

    // ---- panels ----
    bool showOutliner_ = true, showDetails_ = true, showContent_ = true, showLog_ = true;
    bool showWorldSettings_ = true, showStats_ = true, showImGuiDemo_ = false;
    bool showPrefabs_ = true;
    bool showProfiler_ = false;
    bool showModulesPanel_ = false;
    bool showColliders_ = true;      // wireframes for the selected entity
    bool showAllColliders_ = false;  // wireframes for every entity
    bool showNavmesh_ = false;       // walkable-poly overlay (world.nav)
    bool resetLayoutRequested_ = false;

    // ---- custom title bar (borderless chrome) ----
    float titleBarH_ = 34.0f;
    float dragMinX_ = 0.0f, dragMaxX_ = 0.0f; // draggable caption span, updated per frame
    int focusViewportFrames_ = 2; // select the Viewport tab after (re)building the layout
    std::string contentDir_;
    std::string scenePath_; // current map ("" = unsaved)
    char outlinerFilter_[64] = {};
    char logFilter_[64] = {};
    char renameBuf_[128] = {};
    uint32_t renamingId_ = 0;

    // ---- build & package state ----
    bool buildDialogRequested_ = false;
    char buildOutputDir_[260] = {};
    char buildGameName_[128] = {};
    int buildMapIndex_ = 0;
    std::vector<std::string> buildMaps_; // project-relative candidates
    bool buildDevelopment_ = false;
    std::thread packageThread_;
    std::atomic<bool> packaging_{false};
    std::atomic<bool> packageDone_{false};
    bool packageOk_ = false;

    // ---- game module compile state ----
    GameModule* gameModule_ = nullptr;
    PluginManager* plugins_ = nullptr;
    std::thread compileThread_;
    std::atomic<bool> compiling_{false};
    std::atomic<bool> compileDone_{false};
    std::thread pluginCompileThread_;
    std::atomic<bool> pluginCompiling_{false};
    std::atomic<bool> pluginCompileDone_{false};
    bool pluginCompileOk_ = false;
    std::string pluginCompileName_; // reload target when the worker finishes
    bool compileOk_ = false; // written by the worker before compileDone_
    std::mutex compileLogMutex_;
    std::vector<std::string> compileLogPending_; // worker → main-thread log

    // ---- agent bridge ----
    AgentBridge bridge_;
    PulseConfig bridgeConfig_; // pulse.json: enabled/port/token shared with PulseLABS

    // ---- sub-editors ----
    DialogueGraphEditor dialogueGraph_;
    ScriptGraphPanel scriptGraph_;
    MaterialGraphPanel materialGraph_;
    UIDesignerPanel uiDesigner_;
    InputMapPanel inputMap_;
    AnimGraphPanel animGraph_;
    DataTablePanel dataTable_;
    AiPanel aiPanel_;
    MissionPanel missionPanel_;

    // ---- stats ----
    float fps_ = 0.0f;
    float frameMsHistory_[120] = {};
    int frameMsCursor_ = 0;
};

} // namespace ae
