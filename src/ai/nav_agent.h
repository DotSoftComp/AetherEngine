// Aether Engine — NavAgent: point-to-point navmesh locomotion for NPCs.
//
// moveTo() plans a path on world.nav (auto-baking it on first use if nobody
// baked yet) and the agent walks the string-pulled waypoints each update:
// through its RigidBody's velocity when it has a dynamic body (so gravity,
// collision and wall sliding still apply), else by moving the transform
// directly (pure-kinematic NPCs). With `avoidance` on, a kinematic agent is
// instead driven by the navmesh's DetourCrowd — path following + RVO local
// avoidance of other crowd agents and dynamic obstacles, so a group of NPCs
// flows around each other instead of stacking. Scripts drive it with the
// AIMoveTo / AIStop / AIArrived nodes.
#pragma once
#include "../engine/component.h"
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/reflect.h"
#include "../physics/physics_components.h"
#include <vector>

namespace ae {

class NavAgentComponent : public Component {
public:
    float speed = 3.0f;
    float radius = 0.4f;           // crowd footprint (avoidance spacing)
    float acceptance = 0.35f;      // "close enough" to a waypoint / the goal
    float repathInterval = 0.5f;   // s between re-plans while moving (0 = never)
    bool faceMoveDir = true;
    bool avoidance = false;        // steer via DetourCrowd (kinematic agents)

    const char* typeName() const override { return "NavAgent"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("speed", speed, {PropKind::Default, "Speed", 0.05f, 0.0f, 50.0f});
        v.visit("radius", radius, {PropKind::Default, "Radius", 0.02f, 0.05f, 5.0f});
        v.visit("acceptance", acceptance, {PropKind::Default, "Acceptance radius", 0.02f, 0.05f, 5.0f});
        v.visit("repathInterval", repathInterval, {PropKind::Default, "Repath interval (s)", 0.05f, 0.0f, 10.0f});
        v.visit("faceMoveDir", faceMoveDir, {PropKind::Default, "Face move direction"});
        v.visit("avoidance", avoidance, {PropKind::Default, "Local avoidance (crowd)"});
    }

    ~NavAgentComponent() override {
        // World destroys entities_ before its `nav` member, so this is safe.
        if (crowdIdx_ >= 0 && w_ && w_->nav.valid()) w_->nav.removeAgent(crowdIdx_);
    }

    void moveTo(const Vec3& target) {
        target_ = target;
        hasTarget_ = true;
        arrived_ = false;
        repathT_ = 0.0f; // plan immediately on the next update
        path_.clear();
        targetPosted_ = false;
    }
    void stop() {
        hasTarget_ = false;
        path_.clear();
        if (crowdIdx_ >= 0 && world().nav.valid()) world().nav.resetAgentTarget(crowdIdx_);
        haltBody();
    }
    bool arrived() const { return arrived_; }
    bool isMoving() const { return hasTarget_; }
    const std::vector<Vec3>& path() const { return path_; } // debug draw

    void onStart() override { w_ = &world(); }

    void onUpdate(float dt) override {
        if (!hasTarget_) return;
        World& w = world();
        if (!w.nav.valid() && !navBakeTried_) {
            navBakeTried_ = true; // one attempt per component; a bake failure
            w.nav.bake(w);        // (no static geometry) shouldn't spam per frame
        }
        if (!w.nav.valid()) return;

        if (useCrowd()) { updateCrowd(w); return; }
        updateDirect(w, dt);
    }

    // Crowd agents read their steered position back after nav.update() ran.
    void onLateUpdate(float) override {
        if (!useCrowd() || crowdIdx_ < 0 || !world().nav.agentValid(crowdIdx_)) return;
        Vec3 pos = world().nav.agentPosition(crowdIdx_);
        entity().transform.position = pos;
        if (!hasTarget_) return;
        Vec3 vel = world().nav.agentVelocity(crowdIdx_);
        Vec3 flat(vel.x, 0.0f, vel.z);
        if (faceMoveDir && length(flat) > 0.05f) entity().transform.rotation = quatLookAt(flat);
        Vec3 d = target_ - pos;
        d.y = 0.0f;
        if (length(d) <= acceptance) {
            hasTarget_ = false;
            arrived_ = true;
            world().nav.resetAgentTarget(crowdIdx_);
        }
    }

private:
    bool useCrowd() const {
        auto* rb = entity().getComponent<RigidBodyComponent>();
        bool dynamicBody = rb && rb->type() == BodyType::Dynamic;
        return avoidance && !dynamicBody;
    }

    void updateCrowd(World& w) {
        // (Re)register with the crowd after a (re)bake reset it.
        if (crowdIdx_ < 0 || crowdGen_ != w.nav.generation() || !w.nav.agentValid(crowdIdx_)) {
            Vec3 p = w.nav.nearestPoint(entity().worldPosition());
            crowdIdx_ = w.nav.addAgent(p, radius, 1.8f, speed);
            crowdGen_ = w.nav.generation();
            targetPosted_ = false;
            if (crowdIdx_ < 0) return;
        }
        if (!targetPosted_) {
            w.nav.setAgentTarget(crowdIdx_, target_);
            targetPosted_ = true;
        }
        // Position/facing are applied in onLateUpdate (after crowd steps).
    }

    void updateDirect(World& w, float dt) {
        repathT_ -= dt;
        if (repathT_ <= 0.0f) {
            repathT_ = repathInterval > 0.0f ? repathInterval : 1e9f;
            if (!w.nav.findPath(entity().worldPosition(), target_, path_)) {
                path_.clear();
                hasTarget_ = false; // unreachable: give up rather than run at walls
                haltBody();
                return;
            }
            wp_ = 0;
        }
        if (path_.empty()) return;

        Vec3 pos = entity().worldPosition();
        // Advance past reached waypoints (XZ distance; Y follows the mesh).
        while (wp_ < (int)path_.size()) {
            Vec3 d = path_[wp_] - pos;
            d.y = 0.0f;
            if (length(d) > acceptance) break;
            ++wp_;
        }
        if (wp_ >= (int)path_.size()) {
            hasTarget_ = false;
            arrived_ = true;
            haltBody();
            return;
        }

        Vec3 dir = path_[wp_] - pos;
        float vertical = dir.y;
        dir.y = 0.0f;
        float dist = length(dir);
        if (dist > 1e-4f) dir = dir * (1.0f / dist);

        auto* rb = entity().getComponent<RigidBodyComponent>();
        if (rb && rb->type() == BodyType::Dynamic) {
            rb->velocity.x = dir.x * speed;
            rb->velocity.z = dir.z * speed;
        } else {
            float step = speed * dt < dist ? speed * dt : dist;
            entity().transform.position = entity().transform.position + dir * step +
                                          Vec3(0, vertical * (dist > 1e-4f ? step / dist : 1.0f), 0);
        }
        if (faceMoveDir && dist > 1e-3f)
            entity().transform.rotation = quatLookAt(dir);
    }

    void haltBody() {
        auto* rb = entity().getComponent<RigidBodyComponent>();
        if (rb && rb->type() == BodyType::Dynamic) {
            rb->velocity.x = 0.0f;
            rb->velocity.z = 0.0f;
        }
    }

    World* w_ = nullptr; // cached for the destructor (see ~NavAgentComponent)
    Vec3 target_{0, 0, 0};
    std::vector<Vec3> path_;
    int wp_ = 0;
    float repathT_ = 0.0f;
    bool hasTarget_ = false;
    bool arrived_ = false;
    bool navBakeTried_ = false;
    int crowdIdx_ = -1;
    unsigned crowdGen_ = 0;
    bool targetPosted_ = false;
};

} // namespace ae
