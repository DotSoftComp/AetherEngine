// Aether Engine — the World owns all entities and drives the sim/render bridge.
//
//   world.update(dt, time, input);         // start + tick components, transforms
//   world.buildRenderScene(renderScene);   // gather draws/lights, resolve camera
//   renderer.render(renderScene, world.camera(), time);
#pragma once
#include "entity.h"
#include "mission.h"
#include "input_actions.h"
#include "../scene/scene.h"
#include "../physics/physics.h"
#include "../ai/nav_mesh.h"
#include "../core/window.h" // Input
#include <memory>
#include <string>
#include <vector>

namespace ae {

class DialoguePlayer; // narrative/dialogue_player.h — forward-declared so the
                       // engine layer stays free of UI/narrative includes.

class World {
public:
    // Environment drives the sky / sun / IBL (see RenderScene).
    struct Environment {
        Vec3 sunDir = normalize(Vec3(0.42f, 0.55f, 0.30f));
        float skyIntensity = 22.0f;
    } env;

    // Missions + story flags (Detroit-style objective tracking), ticked with
    // behaviors each update. Data lives in assets/missions/.
    MissionSystem missions;

    // Rigid-body physics, stepped each update while behaviors tick (Play/runtime,
    // not edit mode). Bodies live on entity RigidBody/Collider components.
    PhysicsWorld physics;

    // Named action/axis input (keyboard + mouse + gamepad), resolved from the
    // raw Input at the top of every update. Load bindings with
    // actions.loadOrDefaults(<project>/assets/input.json).
    InputActions actions;

    // Navigation mesh (Recast/Detour) baked from static colliders. The editor
    // bakes explicitly (Tools > Bake Navmesh); NavAgents auto-bake on first
    // use. Cleared with the scene (see clear()).
    NavMesh nav;

    Entity* spawn(const std::string& name, Entity* parent = nullptr);
    // Loader path: recreate an entity with a pre-existing persistent Guid so
    // serialized references still resolve (an invalid guid gets a fresh one).
    Entity* spawnWithGuid(const std::string& name, const Guid& guid, Entity* parent = nullptr);
    void destroy(Entity* e);              // deferred until end of frame
    Entity* find(const std::string& name) const;
    Entity* findById(uint32_t id) const;
    Entity* findByGuid(const Guid& guid) const;

    // Destroys every entity immediately and resets the camera director /
    // active dialogue (used by the scene loader and File > New).
    void clear();

    // Moves `e` under `newParent` (nullptr = root), preserving its world
    // transform. Refuses cycles (parenting to itself or a descendant).
    bool reparent(Entity* e, Entity* newParent);

    // tickBehaviors=false (editor edit mode) recomputes transforms but does not
    // run component onStart/onUpdate — the game is paused, not playing.
    void update(float dt, float time, const Input& input, bool tickBehaviors = true);
    void buildRenderScene(RenderScene& out);

    // For editor tooling (hierarchy / inspector).
    const std::vector<std::unique_ptr<Entity>>& entities() const { return entities_; }

    // The resolved, possibly-blending camera for this frame (see below).
    const Camera& camera() const { return camera_; }

    const Input& input() const { return *input_; }
    float time() const { return time_; }
    float dt() const { return dt_; }

    // A single "currently playing" dialogue/QTE sequence (Detroit-style: only
    // one plays at a time). A DialogueTriggerComponent registers itself here
    // when it fires; whoever owns the UI (the game, or the editor's Play
    // mode) drains it by calling update() on the returned pointer each frame.
    void setActiveDialogue(DialoguePlayer* p) { activeDialogue_ = p; }
    DialoguePlayer* activeDialogue() const { return activeDialogue_; }

    // ---- save-game requests ---------------------------------------------
    // Scripts/menus queue a save or load here (project-relative path); the app
    // layer executes it between frames via processSaveRequests (a script must
    // never destroy the world mid-execution).
    void requestSaveGame(const std::string& path) { saveReq_ = path; }
    void requestLoadGame(const std::string& path) { loadReq_ = path; }
    std::string takeSaveRequest() { std::string s; s.swap(saveReq_); return s; }
    std::string takeLoadRequest() { std::string s; s.swap(loadReq_); return s; }

    // ---- game-UI events -----------------------------------------------
    // Retained-UI Buttons post their id here when clicked (during the HUD draw,
    // after update). The next update's script tick reads them (OnUIButton),
    // then the queue is cleared at the end of that update.
    void postUIEvent(const std::string& buttonId) { uiEvents_.push_back(buttonId); }
    const std::vector<std::string>& uiEvents() const { return uiEvents_; }

    // ---- camera director ---------------------------------------------
    // Every CameraComponent submits its pose here each frame (buildRenderScene
    // clears the list first), keyed by its entity's name. requestCamera picks
    // which one becomes the resolved camera(): cinematics (dialogue/QTE cuts,
    // scripted shots) call it with a name and a blend duration; an empty name
    // releases back to whichever shot has isDefault set (the ordinary
    // gameplay camera — third-person, follow-cam, or free-fly). A request for
    // a camera that isn't present this frame is remembered and picked up the
    // moment it appears (e.g. a rig that only exists while a cutscene runs).
    void submitCameraShot(const std::string& name, const Camera& pose, bool isDefault);
    void requestCamera(const std::string& name, float blendSeconds = 0.0f);

private:
    void recomputeTransforms();
    void resolveCamera();

    std::vector<std::unique_ptr<Entity>> entities_;
    uint32_t nextId_ = 1;
    const Input* input_ = nullptr;
    float time_ = 0, dt_ = 0;
    DialoguePlayer* activeDialogue_ = nullptr;

    struct CameraShot {
        std::string name;
        Camera pose;
        bool isDefault = false;
    };
    std::string saveReq_, loadReq_;
    std::vector<std::string> uiEvents_;
    std::vector<CameraShot> cameraShots_;
    std::string requestedCameraName_;
    Camera camera_;
    Camera blendFrom_;
    float blendT_ = 0, blendDur_ = 0;
};

} // namespace ae
