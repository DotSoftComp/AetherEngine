// Aether Engine — physics components.
//
// A body is a RigidBody + a Collider on the same entity. The solver
// (physics.cpp) reads these directly each step, so runtime state (velocity,
// grounded, overlap) lives right here on the components.
#pragma once
#include "../engine/component.h"
#include "../engine/entity.h"
#include "../engine/world.h"
#include "../engine/reflect.h"
#include "../core/window.h" // Input (CharacterController)

namespace ae {

enum class BodyType { Static = 0, Dynamic = 1, Kinematic = 2 };
enum class ColliderShape { Box = 0, Sphere = 1, Capsule = 2 };

// Mass + motion. Without a RigidBody a Collider is treated as immovable Static
// world geometry (floors, walls).
class RigidBodyComponent : public Component {
public:
    int bodyType = (int)BodyType::Dynamic; // BodyType
    float mass = 1.0f;
    bool useGravity = true;
    float linearDamping = 0.05f;
    float angularDamping = 0.05f;
    float restitution = 0.0f;   // bounciness 0..1
    float friction = 0.5f;      // tangential damping on contact 0..1
    bool lockRotation = false;  // keep upright (characters, coins-on-edge, etc.)

    // ---- runtime (not serialized) ----
    Vec3 velocity{0, 0, 0};
    Vec3 angularVelocity{0, 0, 0}; // world-space, rad/s
    bool grounded = false;      // set by the solver when resting on a surface

    BodyType type() const { return (BodyType)bodyType; }
    float invMass() const {
        return (type() == BodyType::Dynamic && mass > 1e-4f) ? 1.0f / mass : 0.0f;
    }

    const char* typeName() const override { return "RigidBody"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("bodyType", bodyType, {PropKind::Default, "Type (0=Static 1=Dyn 2=Kin)", 1, 0, 2});
        v.visit("mass", mass, {PropKind::Default, "Mass", 0.05f, 0.0f, 1000.0f});
        v.visit("useGravity", useGravity, {PropKind::Default, "Use gravity"});
        v.visit("linearDamping", linearDamping, {PropKind::SliderNorm, "Linear damping", 0.005f, 0.0f, 1.0f});
        v.visit("angularDamping", angularDamping, {PropKind::SliderNorm, "Angular damping", 0.005f, 0.0f, 1.0f});
        v.visit("restitution", restitution, {PropKind::SliderNorm, "Restitution"});
        v.visit("friction", friction, {PropKind::SliderNorm, "Friction"});
        v.visit("lockRotation", lockRotation, {PropKind::Default, "Lock rotation"});
    }
};

// Collision shape, sized in the entity's local space and scaled by its world
// scale each step. `isTrigger` colliders detect overlap (see `overlapping`) but
// never push.
class ColliderComponent : public Component {
public:
    int shape = (int)ColliderShape::Box; // ColliderShape
    Vec3 halfExtents{0.5f, 0.5f, 0.5f};  // Box
    float radius = 0.5f;                 // Sphere / Capsule
    float height = 2.0f;                 // Capsule total height (incl. caps)
    Vec3 center{0, 0, 0};                // local offset from the entity origin
    bool isTrigger = false;

    // ---- runtime (not serialized) ----
    bool overlapping = false;            // any contact this step (triggers + solids)

    ColliderShape kind() const { return (ColliderShape)shape; }

    const char* typeName() const override { return "Collider"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("shape", shape, {PropKind::Default, "Shape (0=Box 1=Sphere 2=Capsule)", 1, 0, 2});
        v.visit("halfExtents", halfExtents, {PropKind::Default, "Half extents", 0.02f, 0.0f, 100.0f});
        v.visit("radius", radius, {PropKind::Default, "Radius", 0.02f, 0.0f, 100.0f});
        v.visit("height", height, {PropKind::Default, "Height", 0.02f, 0.0f, 100.0f});
        v.visit("center", center, {PropKind::Default, "Center offset", 0.02f});
        v.visit("isTrigger", isTrigger, {PropKind::Default, "Is trigger"});
    }
};

// Kinematic-feeling character built on the solver: steers a Dynamic capsule's
// horizontal velocity from input (relative to the active camera) and jumps when
// grounded. Gravity + wall sliding come for free from the solver.
class CharacterControllerBehavior : public Behavior {
public:
    float moveSpeed = 4.5f;
    float sprintSpeed = 8.0f;
    float jumpSpeed = 5.0f;
    bool faceMoveDir = true;

    const char* typeName() const override { return "CharacterController"; }
    void reflect(PropertyVisitor& v) override {
        v.visit("moveSpeed", moveSpeed, {PropKind::Default, "Move speed", 0.05f, 0.0f, 50.0f});
        v.visit("sprintSpeed", sprintSpeed, {PropKind::Default, "Sprint speed", 0.05f, 0.0f, 50.0f});
        v.visit("jumpSpeed", jumpSpeed, {PropKind::Default, "Jump speed", 0.05f, 0.0f, 50.0f});
        v.visit("faceMoveDir", faceMoveDir, {PropKind::Default, "Face move direction"});
    }

    void onUpdate(float) override {
        auto* rb = entity().getComponent<RigidBodyComponent>();
        if (!rb) return;
        rb->lockRotation = true; // a character stays upright; we drive its facing
        rb->angularVelocity = Vec3(0, 0, 0);

        // Camera-relative ground basis, driven by the input map (MoveX/MoveY
        // axes + Jump action) so keyboard AND gamepad both steer the character.
        Vec3 fwd = world().camera().forwardDir;
        fwd.y = 0.0f;
        fwd = normalize(fwd);
        Vec3 right = normalize(cross(fwd, Vec3(0, 1, 0)));

        float mx = world().actions.axis("MoveX");
        float my = world().actions.axis("MoveY");
        Vec3 wish = fwd * my + right * mx;

        float mag = clampf(length(wish), 0.0f, 1.0f);
        bool sprint = world().input().keys[0x10]; // Shift (stick magnitude also scales)
        float speed = (sprint ? sprintSpeed : moveSpeed) * mag;
        if (mag > 0.05f) {
            wish = normalize(wish);
            rb->velocity.x = wish.x * speed;
            rb->velocity.z = wish.z * speed;
            if (faceMoveDir)
                entity().transform.rotation = quatLookAt(wish);
        } else {
            rb->velocity.x = 0.0f;
            rb->velocity.z = 0.0f;
        }
        if (rb->grounded && world().actions.pressed("Jump")) rb->velocity.y = jumpSpeed;
    }
};

} // namespace ae
