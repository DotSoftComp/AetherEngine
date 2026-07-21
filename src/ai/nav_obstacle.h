// Aether Engine — NavObstacle: a dynamic, rebake-free navmesh carve-out.
//
// Attach to an entity (a door, a crate, a spawned barricade). While active it
// registers a vertical cylinder obstacle with world.nav; agents route and
// steer around it immediately, and only the touched navmesh tiles rebuild —
// no full rebake. Moving the entity past `moveThreshold` re-carves at the new
// spot; deactivating or destroying the entity removes the carve. Scripts can
// toggle the whole thing by toggling the entity active.
#pragma once
#include "../engine/component.h"
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/reflect.h"
#include "nav_mesh.h"

namespace ae {

class NavObstacleComponent : public Component {
public:
    float radius = 0.6f;
    float height = 2.0f;
    float moveThreshold = 0.25f; // re-carve when the entity moves this far

    const char* typeName() const override { return "NavObstacle"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("radius", radius, {PropKind::Default, "Radius", 0.02f, 0.05f, 20.0f});
        v.visit("height", height, {PropKind::Default, "Height", 0.05f, 0.1f, 40.0f});
        v.visit("moveThreshold", moveThreshold,
                {PropKind::Default, "Move threshold (m)", 0.02f, 0.0f, 10.0f});
    }

    ~NavObstacleComponent() override {
        if (obstacle_ && w_ && w_->nav.valid()) w_->nav.removeObstacle(obstacle_);
    }

    void onStart() override { w_ = &world(); }

    void onUpdate(float) override {
        World& w = world();
        if (!w.nav.valid()) return; // nothing to carve yet (bakes elsewhere)
        Vec3 pos = entity().worldPosition();
        if (obstacle_ && length(pos - placedAt_) <= moveThreshold) return; // unchanged
        if (obstacle_) w.nav.removeObstacle(obstacle_);
        obstacle_ = w.nav.addObstacle(pos, radius, height);
        placedAt_ = pos;
    }

private:
    World* w_ = nullptr;
    NavObstacle obstacle_ = 0;
    Vec3 placedAt_{0, 0, 0};
};

} // namespace ae
