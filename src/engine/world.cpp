#include "world.h"
#include "engine_modules.h"
#include "../core/gamepad.h"
#include <algorithm>

namespace ae {

Entity* World::spawn(const std::string& name, Entity* parent) {
    return spawnWithGuid(name, Guid::generate(), parent);
}

Entity* World::spawnWithGuid(const std::string& name, const Guid& guid, Entity* parent) {
    auto e = std::make_unique<Entity>();
    e->name_ = name;
    e->id_ = nextId_++;
    e->guid_ = guid.valid() ? guid : Guid::generate();
    e->world_ = this;
    e->parent_ = parent;
    Entity* raw = e.get();
    if (parent) parent->children_.push_back(raw);
    entities_.push_back(std::move(e));
    return raw;
}

void World::destroy(Entity* e) {
    if (e) e->pendingDestroy_ = true;
}

Entity* World::find(const std::string& name) const {
    for (const auto& e : entities_)
        if (e->name_ == name) return e.get();
    return nullptr;
}

Entity* World::findById(uint32_t id) const {
    for (const auto& e : entities_)
        if (e->id_ == id) return e.get();
    return nullptr;
}

Entity* World::findByGuid(const Guid& guid) const {
    if (!guid.valid()) return nullptr;
    for (const auto& e : entities_)
        if (e->guid_ == guid) return e.get();
    return nullptr;
}

Entity* EntityRef::get(const World& world) const {
    if (!guid.valid()) return nullptr;
    if (cached && cached->guid() == guid) return cached;
    cached = world.findByGuid(guid);
    return cached;
}

void World::clear() {
    physics.clear();
    nav.clear();
    entities_.clear();
    uiEvents_.clear();
    cameraShots_.clear();
    requestedCameraName_.clear();
    activeDialogue_ = nullptr;
    blendT_ = blendDur_ = 0.0f;
    nextId_ = 1;
}

bool World::reparent(Entity* e, Entity* newParent) {
    if (!e || e == newParent) return false;
    for (Entity* p = newParent; p; p = p->parent_)
        if (p == e) return false; // would create a cycle

    Mat4 world = e->world_matrix_;
    if (e->parent_) {
        auto& sib = e->parent_->children_;
        sib.erase(std::remove(sib.begin(), sib.end(), e), sib.end());
    }
    e->parent_ = newParent;
    if (newParent) newParent->children_.push_back(e);

    // Preserve the world pose: local = parentWorld^-1 * world, decomposed back
    // into TRS (columns carry rotation*scale; lossy only for shear).
    Mat4 local = newParent ? inverse(newParent->world_matrix_) * world : world;
    Vec3 cx(local.m[0][0], local.m[0][1], local.m[0][2]);
    Vec3 cy(local.m[1][0], local.m[1][1], local.m[1][2]);
    Vec3 cz(local.m[2][0], local.m[2][1], local.m[2][2]);
    Vec3 s(length(cx), length(cy), length(cz));
    e->transform.position = Vec3(local.m[3][0], local.m[3][1], local.m[3][2]);
    e->transform.scaling = s;
    e->transform.rotation = quatFromBasis(
        s.x > 1e-8f ? cx / s.x : Vec3(1, 0, 0),
        s.y > 1e-8f ? cy / s.y : Vec3(0, 1, 0),
        s.z > 1e-8f ? cz / s.z : Vec3(0, 0, 1));
    e->world_matrix_ = world;
    return true;
}

void World::recomputeTransforms() {
    // Depth-first from roots so a parent's world matrix is ready before its
    // children. Entities carry raw parent/child pointers (stable: the entity
    // objects are heap-allocated behind unique_ptr).
    std::vector<Entity*> stack;
    for (const auto& e : entities_)
        if (!e->parent_) stack.push_back(e.get());

    while (!stack.empty()) {
        Entity* e = stack.back();
        stack.pop_back();
        Mat4 local = e->transform.matrix();
        e->world_matrix_ = e->parent_ ? e->parent_->world_matrix_ * local : local;
        for (Entity* c : e->children_) stack.push_back(c);
    }
}

void World::update(float dt, float time, const Input& input, bool tickBehaviors) {
    input_ = &input;
    dt_ = dt;
    time_ = time;
    actions.update(input, pollGamepad(0));

    // Resolve transforms *before* ticking components, so onStart/onUpdate
    // (which may read worldPosition()/worldMatrix() — proximity triggers,
    // look-at rigs, etc.) always see a valid pose, even for an entity spawned
    // this very frame. Without this, every entity's world matrix is still the
    // default identity the first time onUpdate ever runs, so e.g. a
    // proximity check reads a distance of zero regardless of actual
    // positions or configured radius.
    recomputeTransforms();

    // Start freshly-attached components, then tick everything active. Skipped
    // entirely in the editor's edit mode (transforms still recompute below).
    if (tickBehaviors) {
        for (const auto& e : entities_) {
            if (!e->active_ || e->pendingDestroy_) continue;
            for (const auto& c : e->components_) {
                if (!c->started_) { c->onStart(); c->started_ = true; }
                c->onUpdate(dt);
            }
        }
        missions.update(*this, dt);

        // Step physics after behaviors (which may set velocities / teleport
        // bodies) but before the render resolve. Refresh world matrices first
        // so the solver reads this frame's behavior-driven poses in world space.
        recomputeTransforms();
        if (engineModules().enabled("physics")) physics.step(*this, dt);

        // Late tick: pose post-processing (IK) and anything that must see the
        // physics-resolved transforms, whatever the component order.
        for (const auto& e : entities_) {
            if (!e->active_ || e->pendingDestroy_) continue;
            for (const auto& c : e->components_)
                if (c->started_) c->onLateUpdate(dt);
        }

        // UI button events posted during last frame's HUD draw were visible to
        // this update's scripts (OnUIButton); consume them now.
        uiEvents_.clear();
    }

    // Re-resolve so this frame's movement (position/rotation changes made
    // above) is reflected before rendering.
    recomputeTransforms();

    // Sweep destroyed entities. Detach from parents first, then drop; children
    // of a destroyed entity are orphaned to the root (kept simple on purpose).
    bool anyDead = false;
    for (const auto& e : entities_)
        if (e->pendingDestroy_) { anyDead = true; break; }
    if (anyDead) {
        for (const auto& e : entities_) {
            if (!e->pendingDestroy_) continue;
            if (e->parent_) {
                auto& sib = e->parent_->children_;
                sib.erase(std::remove(sib.begin(), sib.end(), e.get()), sib.end());
            }
            for (Entity* c : e->children_) c->parent_ = nullptr;
        }
        entities_.erase(
            std::remove_if(entities_.begin(), entities_.end(),
                           [](const std::unique_ptr<Entity>& e) { return e->pendingDestroy_; }),
            entities_.end());
    }
}

void World::buildRenderScene(RenderScene& out) {
    out.clear();
    out.sunDir = env.sunDir;
    out.skyIntensity = env.skyIntensity;
    cameraShots_.clear();
    for (const auto& e : entities_) {
        if (!e->active_) continue;
        for (const auto& c : e->components_) c->contribute(out);
    }
    resolveCamera();
}

void World::submitCameraShot(const std::string& name, const Camera& pose, bool isDefault) {
    cameraShots_.push_back({name, pose, isDefault});
}

void World::requestCamera(const std::string& name, float blendSeconds) {
    if (name == requestedCameraName_) return; // already the active request
    requestedCameraName_ = name;
    blendFrom_ = camera_; // snapshot of the currently-resolved pose to blend from
    blendT_ = 0.0f;
    blendDur_ = std::max(0.0f, blendSeconds);
}

static Camera lerpCameraPose(const Camera& a, const Camera& b, float t) {
    Camera c;
    c.position = a.position + (b.position - a.position) * t;
    c.forwardDir = normalize(a.forwardDir + (b.forwardDir - a.forwardDir) * t);
    c.upDir = normalize(a.upDir + (b.upDir - a.upDir) * t);
    c.fovY = lerpf(a.fovY, b.fovY, t);
    c.zNear = lerpf(a.zNear, b.zNear, t);
    c.zFar = lerpf(a.zFar, b.zFar, t);
    return c;
}

void World::resolveCamera() {
    // Prefer the explicitly requested shot; fall back to whichever shot is
    // flagged as the default gameplay camera; fall back to the first shot
    // submitted so something is always on screen if a scene has only one
    // camera. If a requested name isn't present this frame (e.g. a cinematic
    // rig entity doesn't exist yet), keep showing the last resolved pose.
    const CameraShot* target = nullptr;
    if (!requestedCameraName_.empty()) {
        for (const auto& s : cameraShots_)
            if (s.name == requestedCameraName_) { target = &s; break; }
    }
    if (!target) {
        for (const auto& s : cameraShots_)
            if (s.isDefault) { target = &s; break; }
    }
    if (!target && !cameraShots_.empty()) target = &cameraShots_[0];
    if (!target) return;

    if (blendDur_ > 0.0f && blendT_ < blendDur_) {
        blendT_ = std::min(blendDur_, blendT_ + dt_);
        camera_ = lerpCameraPose(blendFrom_, target->pose, blendT_ / blendDur_);
    } else {
        camera_ = target->pose;
    }
}

} // namespace ae
