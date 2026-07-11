// Aether Engine — physics service (Jolt Physics backend).
//
// The World talks to physics only through this class; the RigidBody/Collider
// components remain the authoring surface. Internally each collider is mirrored
// as a Jolt body — full rigid-body dynamics (stable stacking, tumbling, edge
// contacts, restitution/friction, CCD-ready) come from Jolt, not hand-written
// code. Jolt types are kept out of this header (pimpl) so world.h and the rest
// of the engine never see them.
//
// Bodies are reconciled from the live components each step (created on first
// sight, removed when their entity disappears), so there is no separate registry
// to keep in sync and PIE Stop / entity destroy can't leak a body.
#pragma once
#include "../core/math3d.h"

namespace ae {

class World;
class Entity;

struct RayHit {
    bool hit = false;
    Entity* entity = nullptr;
    Vec3 point{0, 0, 0};
    Vec3 normal{0, 1, 0};
    float distance = 0.0f;
    explicit operator bool() const { return hit; }
};

class PhysicsWorld {
public:
    Vec3 gravity{0.0f, -9.81f, 0.0f};

    PhysicsWorld() = default;
    ~PhysicsWorld();
    PhysicsWorld(const PhysicsWorld&) = delete;
    PhysicsWorld& operator=(const PhysicsWorld&) = delete;

    // Advances the simulation by dt and syncs results back into entity
    // transforms. Called from World::update while behaviors tick.
    void step(World& world, float dt);

    // Closest collider hit by the ray, ignoring triggers/sensors.
    RayHit raycast(World& world, const Vec3& origin, const Vec3& dir, float maxDist) const;

    // Drops every body (scene reload / File > New). Safe to call repeatedly.
    void clear();

    // Live Jolt body count (profiler).
    int bodyCount() const;

private:
    struct Impl;
    Impl* impl_ = nullptr;
    Impl* ensure();
};

} // namespace ae
