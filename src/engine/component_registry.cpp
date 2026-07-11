#include "component_registry.h"
#include "engine_modules.h"
#include "components.h"
#include "behaviors.h"
#include "camera_rigs.h"
#include "assets.h"
#include "../physics/physics_components.h"
#include "../audio/audio_components.h"
// Deliberate one-way include from this leaf .cpp (mirrors scene_io.cpp): the
// narrative bridge component registers here, but engine headers stay free of
// narrative/UI dependencies.
#include "../narrative/dialogue_trigger.h"
#include "../script/script_graph.h"
#include "../ui/ui_document_component.h"
#include "particles.h"
#include "anim_graph.h"
#include "../ai/nav_agent.h"
#include "../core/log.h"
#include <algorithm>

namespace ae {

ComponentRegistry& componentRegistry() {
    static ComponentRegistry g_registry;
    return g_registry;
}

namespace detail {
namespace {
// Pending script registrations. The list lives in AetherCore.dll and is only
// ever appended by a game module's static initializers (during its
// LoadLibrary) and drained immediately after by GameModule_Register — one
// module at a time, so a plain vector is fine.
std::vector<PendingRegistration::Fn>& pendingList() {
    static std::vector<PendingRegistration::Fn> g_pending;
    return g_pending;
}
} // namespace

PendingRegistration::PendingRegistration(Fn fn) { pendingList().push_back(fn); }

void runPendingRegistrations(ComponentRegistry& r, int moduleId) {
    for (PendingRegistration::Fn fn : pendingList()) fn(r, moduleId);
    AE_LOG("[Registry] module registered %d component type(s)", (int)pendingList().size());
    pendingList().clear();
}
} // namespace detail

ComponentDesc& ComponentRegistry::add(ComponentDesc d) {
    for (auto& existing : descs_) {
        if (existing.type == d.type) {
            AE_WARN("[Registry] component type '%s' re-registered (module %d replaces %d)",
                    d.type.c_str(), d.moduleId, existing.moduleId);
            existing = std::move(d);
            return existing;
        }
    }
    descs_.push_back(std::move(d));
    return descs_.back();
}

const ComponentDesc* ComponentRegistry::find(const std::string& type) const {
    for (const auto& d : descs_)
        if (d.type == type) return &d;
    return nullptr;
}

void ComponentRegistry::unregisterModule(int moduleId) {
    descs_.erase(std::remove_if(descs_.begin(), descs_.end(),
                                [&](const ComponentDesc& d) { return d.moduleId == moduleId; }),
                 descs_.end());
}

// Always-on core: rendering, cameras, and generic behaviors — the minimum a
// scene needs to exist at all. Everything else is an optional engine module.
static void registerCoreComponents(ComponentRegistry& r) {
    r.add<MeshRenderer>("MeshRenderer", "Mesh Renderer", "Rendering").initDefaults =
        [](Component* c, AssetLibrary& assets) {
            auto* mr = static_cast<MeshRenderer*>(c);
            mr->meshName = "sphere";
            mr->mesh = assets.mesh("sphere");
        };
    r.add<LightComponent>("Light", "Light", "Rendering").initDefaults =
        [](Component* c, AssetLibrary&) {
            auto* lc = static_cast<LightComponent*>(c);
            lc->color = Vec3(1, 1, 1);
            lc->intensity = 10.0f;
        };
    r.add<CameraComponent>("Camera", "Camera", "Rendering");
    // Models are placed from the Content Browser (they need an asset), so the
    // component itself is not offered in the Add Component menu.
    r.add<ModelComponent>("Model", "Model", "Rendering").userCreatable = false;

    r.add<SpinBehavior>("Spin", "Spin", "Behaviors");
    r.add<OrbitBehavior>("Orbit", "Orbit", "Behaviors");
    r.add<CameraController>("CameraController", "Camera Controller", "Behaviors");
    r.add<ThirdPersonCameraBehavior>("ThirdPersonCamera", "Third-Person Camera", "Behaviors");
    r.add<FollowCameraBehavior>("FollowCamera", "Follow Camera", "Behaviors");
}

// Declares every optional engine module and its component registrations.
// registerBuiltinComponents() then registers whichever ones the project left
// enabled (engineModules().configure ran first, from the .aeproj manifest).
static void declareEngineModules() {
    EngineModules& em = engineModules();

    em.declare({"physics", "Physics (Jolt)",
                "Rigid bodies, colliders, character controller, raycasts", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<RigidBodyComponent>("RigidBody", "Rigid Body", "Physics", id);
                    r.add<ColliderComponent>("Collider", "Collider", "Physics", id);
                    r.add<CharacterControllerBehavior>("CharacterController",
                                                       "Character Controller", "Physics", id);
                }});
    em.declare({"audio", "Audio (XAudio2)", "3D audio sources + one-shot playback", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<AudioSourceComponent>("AudioSource", "Audio Source", "Audio", id);
                }});
    em.declare({"scripting", "Visual Scripting", "Script Graph gameplay logic", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<ScriptGraphComponent>("ScriptGraph", "Script Graph", "Scripting", id);
                }});
    em.declare({"ui", "Game UI", "Retained UIDocument widget trees (HUD, menus)", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<UIDocumentComponent>("UIDocument", "UI Document", "UI", id);
                }});
    em.declare({"particles", "Particles", "CPU particle emitters (HDR billboards)", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<ParticlesComponent>("Particles", "Particles", "Effects", id);
                }});
    em.declare({"animation", "Animation", "Animator state machines (AnimGraph)", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<AnimatorComponent>("Animator", "Animator", "Animation", id);
                }});
    em.declare({"ai", "AI Navigation", "Recast navmesh + NavAgent pathfinding", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<NavAgentComponent>("NavAgent", "Nav Agent", "AI", id);
                }});
    em.declare({"narrative", "Narrative", "Dialogue scenes, QTEs, choice UI", 0, true,
                [](ComponentRegistry& r, int id) {
                    r.add<DialogueTriggerComponent>("DialogueTrigger", "Dialogue Trigger",
                                                    "Narrative", id)
                        .initDefaults = [](Component* c, AssetLibrary& assets) {
                        auto* dt = static_cast<DialogueTriggerComponent*>(c);
                        dt->scenePath = "assets/dialogue/interrogation.json";
                        dt->onDeserialized(assets); // load scene, keep relative path
                    };
                }});
}

void registerBuiltinComponents() {
    static bool done = false;
    if (done) return;
    done = true;
    ComponentRegistry& r = componentRegistry();

    registerCoreComponents(r);
    declareEngineModules();
    for (const EngineModuleDesc& m : engineModules().all())
        if (m.enabled && m.registerComponents) m.registerComponents(r, m.moduleId);
}

} // namespace ae
